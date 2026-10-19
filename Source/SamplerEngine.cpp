#include "SamplerEngine.h"

namespace
{
    constexpr int pitchChunk = 32;           // samples between pitch/glide updates
    constexpr double killSeconds = 0.005;    // fade used when a voice is stolen or choked
    constexpr double endFadeSeconds = 0.003; // fade at the end of a one-shot or slice
    constexpr double idleResetSeconds = 2.0; // "2 s idle" round-robin reset
    constexpr float bendRangeSemis = 2.0f;

    float hann (float x) { return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * x); }
}

SamplerEngine::Params SamplerEngine::Params::fromApvts (juce::AudioProcessorValueTreeState& apvts)
{
    auto p = [&apvts] (const char* id)
    {
        auto* v = apvts.getRawParameterValue (id);
        jassert (v != nullptr);   // id missing from ui/params.json
        return v;
    };
    Params r;
    r.playMode = p ("play_mode");
    r.start = p ("smp_start");          r.end = p ("smp_end");
    r.loopStart = p ("loop_start");     r.loopEnd = p ("loop_end");     r.loopXfade = p ("loop_xfade");
    r.tune = p ("tune");                r.fine = p ("fine");
    r.grnPos = p ("grn_pos");           r.grnSize = p ("grn_size");     r.grnDensity = p ("grn_density");
    r.grnSpray = p ("grn_spray");       r.grnPitchRnd = p ("grn_pitchrnd"); r.grnFreeze = p ("grn_freeze");
    r.attack = p ("env1_attack");       r.decay = p ("env1_decay");
    r.sustain = p ("env1_sustain");     r.release = p ("env1_release");
    return r;
}

SamplerEngine::SamplerEngine (Params p) : params (p)
{
    rrLast.fill (-1);
}

SamplerEngine::~SamplerEngine()
{
    delete map;
    delete pending.exchange (nullptr);
    for (auto* m : draining) delete m;
    while (auto m = popRetired()) {}
}

void SamplerEngine::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    for (auto& v : voices)
        if (v.active)
            freeVoice (v);
    keyOrder.fill (0);
    sustainedKey.fill (false);
    sustainPedal = false;
}

// =====================================================================================
// map handover
// =====================================================================================
void SamplerEngine::publish (std::unique_ptr<InstrumentMap> m)
{
    // The audio thread only ever takes the pending map (exchange with nullptr), so a map that is
    // still pending was never used and can be deleted here.
    delete pending.exchange (m.release());
}

std::unique_ptr<InstrumentMap> SamplerEngine::popRetired()
{
    InstrumentMap* m = nullptr;
    retiredFifo.read (1).forEach ([&] (int i) { m = retired[(size_t) i]; retired[(size_t) i] = nullptr; });
    return std::unique_ptr<InstrumentMap> (m);
}

bool SamplerEngine::pushRetired (InstrumentMap* m)
{
    bool done = false;
    retiredFifo.write (1).forEach ([&] (int i) { retired[(size_t) i] = m; done = true; });
    return done;
}

void SamplerEngine::collectDrainedMaps()
{
    for (auto& m : draining)
        if (m != nullptr && m->voiceRefs == 0 && pushRetired (m))
            m = nullptr;
}

void SamplerEngine::pickUpNewMap()
{
    collectDrainedMaps();

    // The old map needs somewhere to wait; if every slot is busy, try again next block.
    auto freeSlot = std::find (draining.begin(), draining.end(), nullptr);
    if (freeSlot == draining.end() || pending.load() == nullptr)
        return;

    auto* m = pending.exchange (nullptr);
    if (map != nullptr)
        *freeSlot = map;   // collected (and handed to the loader) once its voices have finished
    map = m;
    collectDrainedMaps();
}

// =====================================================================================
// processing
// =====================================================================================
void SamplerEngine::readBlockParams()
{
    auto get = [] (std::atomic<float>* p, float fallback) { return p != nullptr ? p->load() : fallback; };
    bp.playMode    = (PlayMode) juce::jlimit (0, 4, juce::roundToInt (get (params.playMode, 0)));
    bp.start       = get (params.start, 0) * 0.01f;
    bp.end         = get (params.end, 100) * 0.01f;
    bp.loopStart   = get (params.loopStart, 0) * 0.01f;
    bp.loopEnd     = get (params.loopEnd, 100) * 0.01f;
    bp.loopXfadeMs = get (params.loopXfade, 0);
    bp.tuneSemis   = get (params.tune, 0) + get (params.fine, 0) * 0.01f + pitchBendSemis;
    bp.grnPos      = get (params.grnPos, 50) * 0.01f;
    bp.grnSizeMs   = juce::jmax (1.0f, get (params.grnSize, 80));
    bp.grnDensity  = juce::jmax (0.1f, get (params.grnDensity, 20));
    bp.grnSpray    = get (params.grnSpray, 0) * 0.01f;
    bp.grnPitchRnd = get (params.grnPitchRnd, 0);
    bp.grnFreeze   = get (params.grnFreeze, 0) >= 0.5f;
    bp.env = { get (params.attack, 0.005f), get (params.decay, 0.1f), get (params.sustain, 100) * 0.01f, get (params.release, 0.1f) };
}

void SamplerEngine::process (juce::AudioBuffer<float>& out, const juce::MidiBuffer& midi, bool transportPlaying)
{
    pickUpNewMap();
    readBlockParams();

    const int numSamples = out.getNumSamples();

    // Round-robin resets
    if (transportPlaying && ! wasPlaying)
        resetRoundRobin (Layer::RRReset::transport);
    wasPlaying = transportPlaying;
    if (! idleResetDone && samplesSinceNoteOn > idleResetSeconds * sampleRate)
    {
        resetRoundRobin (Layer::RRReset::idle);
        idleResetDone = true;
    }

    for (auto& v : voices)
        if (v.active)
        {
            v.env.setParameters (bp.env);
            updateRegion (v);
        }

    // Render up to each MIDI event, then handle it (sample-accurate)
    int pos = 0;
    for (const auto meta : midi)
    {
        const int eventPos = juce::jlimit (0, numSamples, meta.samplePosition);
        if (eventPos > pos)
        {
            for (auto& v : voices)
                renderVoice (v, out, pos, eventPos - pos);
            now += eventPos - pos;
            pos = eventPos;
        }
        handleMidi (meta.getMessage());
    }
    if (pos < numSamples)
    {
        for (auto& v : voices)
            renderVoice (v, out, pos, numSamples - pos);
        now += numSamples - pos;
    }

    samplesSinceNoteOn += numSamples;

    int count = 0;
    for (const auto& v : voices)
        count += v.active ? 1 : 0;
    activeVoiceCount.store (count);
}

void SamplerEngine::resetRoundRobin (Layer::RRReset which)
{
    if (map == nullptr)
        return;
    for (const auto& l : map->layers)
        if (l.rrReset == which && l.rrGroup < maxRRGroups)
        {
            rrCounter[(size_t) l.rrGroup] = 0;
            rrLast[(size_t) l.rrGroup] = -1;
        }
}

// =====================================================================================
// MIDI
// =====================================================================================
void SamplerEngine::handleMidi (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
        noteOn (m.getNoteNumber(), m.getVelocity());
    else if (m.isNoteOff())
        noteOff (m.getNoteNumber());
    else if (m.isSustainPedalOn())
        sustainPedal = true;
    else if (m.isSustainPedalOff())
    {
        sustainPedal = false;
        for (int k = 0; k < 128; ++k)
            if (sustainedKey[(size_t) k] && keyOrder[(size_t) k] == 0)
            {
                sustainedKey[(size_t) k] = false;
                releaseKey (k);
            }
    }
    else if (m.isPitchWheel())
    {
        pitchBendSemis = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * bendRangeSemis;
        readBlockParams();   // tune includes the bend
    }
    else if (m.isAllSoundOff())
    {
        for (auto& v : voices)
            if (v.active)
                kill (v);
    }
    else if (m.isAllNotesOff())
    {
        sustainPedal = false;
        for (int k = 0; k < 128; ++k)
            if (keyOrder[(size_t) k] != 0 || sustainedKey[(size_t) k])
            {
                keyOrder[(size_t) k] = 0;
                sustainedKey[(size_t) k] = false;
                releaseKey (k);
            }
    }
}

bool SamplerEngine::layerIsPlayable (const InstrumentMap& m, const Layer& layer, int note, int velocity) const noexcept
{
    if (! layer.covers (note, velocity))
        return false;
    if (layer.keyswitch < 0)
        return true;

    // Articulation layers only play while their keyswitch (or one with the same articulation) is active.
    const int ks = m.isKeyswitch (currentKeyswitch) ? currentKeyswitch : m.defaultKeyswitch;
    return ks >= 0 && m.keyswitchArticulation[(size_t) ks] == layer.articulation;
}

void SamplerEngine::noteOn (int note, int rawVelocity)
{
    if (map == nullptr || note < 0 || note > 127)
        return;

    if (map->isKeyswitch (note))
    {
        currentKeyswitch = note;
        return;
    }

    const int velocity = map->applyVelocityCurve (rawVelocity);
    bool othersHeld = false;
    for (int k = 0; k < 128; ++k)
        othersHeld = othersHeld || (k != note && keyOrder[(size_t) k] != 0);

    keyOrder[(size_t) note] = ++keyCounter;
    keyVelocity[(size_t) note] = velocity;
    keyDownTime[(size_t) note] = now;
    sustainedKey[(size_t) note] = false;
    samplesSinceNoteOn = 0;
    idleResetDone = false;
    const auto eventId = ++eventCounter;

    std::array<int, maxRRGroups> picks;
    picks.fill (-1);

    for (const auto& layer : map->layers)
    {
        using T = Layer::Trigger;
        if (layer.trigger == T::release
            || (layer.trigger == T::legato && ! othersHeld)
            || (layer.trigger == T::first && othersHeld)
            || ! layerIsPlayable (*map, layer, note, velocity))
            continue;

        float glideFrom = 0;
        bool hasGlide = false;

        if (layer.voiceMode != Layer::VoiceMode::poly)
        {
            Voice* held = nullptr;
            for (auto& v : voices)
                if (v.active && ! v.killing && ! v.isRelease && v.layer->id == layer.id && v.keyDown)
                    held = &v;

            if (held != nullptr && layer.voiceMode == Layer::VoiceMode::legato && held->map == map)
            {
                retarget (*held, note, velocity);   // keep playing, just change pitch
                continue;
            }
            if (held != nullptr)
            {
                glideFrom = held->semis;
                hasGlide = layer.glideMs > 0;
            }
            for (auto& v : voices)   // mono: one voice per layer
                if (v.active && ! v.isRelease && v.layer->id == layer.id)
                    kill (v);
        }
        else
        {
            int playing = 0;
            for (const auto& v : voices)
                playing += (v.active && ! v.killing && ! v.isRelease && v.layer->id == layer.id) ? 1 : 0;
            if (playing >= layer.polyphony)
                stealWithinLayer (layer);

            if (layer.glideMs > 0 && hasLastNote)
            {
                glideFrom = semitonesFor (layer, lastNote);
                hasGlide = true;
            }
        }

        if (layer.choke > 0)
            for (auto& v : voices)
                if (v.active && v.eventId != eventId && v.layer->choke == layer.choke)
                    kill (v);

        startVoice (*map, layer, note, velocity, 1.0f, false, pickVariation (layer, picks.data()), eventId, glideFrom, hasGlide);
    }

    lastNote = note;
    hasLastNote = true;
}

void SamplerEngine::noteOff (int note)
{
    if (note < 0 || note > 127 || keyOrder[(size_t) note] == 0)
        return;
    keyOrder[(size_t) note] = 0;

    if (sustainPedal)
        sustainedKey[(size_t) note] = true;   // released when the pedal comes up
    else
        releaseKey (note);
}

void SamplerEngine::releaseKey (int note)
{
    for (auto& v : voices)
    {
        if (! v.active || v.killing || v.isRelease || ! v.keyDown || v.note != note)
            continue;

        // Mono / legato: fall back to the most recent key that is still held.
        if (v.layer->voiceMode != Layer::VoiceMode::poly)
        {
            int best = -1;
            for (int k = 0; k < 128; ++k)
                if (keyOrder[(size_t) k] != 0 && v.layer->covers (k, keyVelocity[(size_t) k])
                    && (best < 0 || keyOrder[(size_t) k] > keyOrder[(size_t) best]))
                    best = k;

            if (best >= 0)
            {
                if (v.layer->voiceMode == Layer::VoiceMode::legato)
                {
                    retarget (v, best, keyVelocity[(size_t) best]);
                }
                else
                {
                    const auto& layer = *v.layer;
                    const auto& layerMap = *v.map;
                    const auto from = v.semis;
                    kill (v);
                    std::array<int, maxRRGroups> picks;
                    picks.fill (-1);
                    startVoice (layerMap, layer, best, keyVelocity[(size_t) best], 1.0f, false,
                                pickVariation (layer, picks.data()), ++eventCounter, from, layer.glideMs > 0);
                }
                continue;
            }
        }

        v.keyDown = false;
        v.env.noteOff();
    }

    // Release triggers: quieter the longer the key was held.
    if (map == nullptr)
        return;
    const int velocity = keyVelocity[(size_t) note];
    const auto heldMs = (now - keyDownTime[(size_t) note]) * 1000.0 / sampleRate;
    std::array<int, maxRRGroups> picks;
    picks.fill (-1);
    for (const auto& layer : map->layers)
    {
        if (layer.trigger != Layer::Trigger::release || ! layerIsPlayable (*map, layer, note, velocity))
            continue;
        const auto decay = layer.releaseDecayMs > 0 ? (float) std::exp (-heldMs / layer.releaseDecayMs) : 1.0f;
        startVoice (*map, layer, note, velocity, layer.releaseGain * decay, true, pickVariation (layer, picks.data()), ++eventCounter, 0, false);
    }
}

// =====================================================================================
// voices
// =====================================================================================
int SamplerEngine::pickVariation (const Layer& layer, int* picks)
{
    const auto numSamples = (int) layer.samples.size();
    if (layer.rrMode == Layer::RRMode::off || numSamples <= 1 || layer.rrGroup >= maxRRGroups)
        return 0;

    // Every layer in an RR group uses the same pick for one note, so velocity layers stay in step.
    const auto g = (size_t) layer.rrGroup;
    if (picks[g] < 0)
    {
        const int n = juce::jmax (1, map != nullptr && g < map->rrGroupSizes.size() ? map->rrGroupSizes[g] : numSamples);
        int pick = 0;
        switch (layer.rrMode)
        {
            case Layer::RRMode::cycle:    pick = rrCounter[g] % n; rrCounter[g] = (pick + 1) % n; break;
            case Layer::RRMode::random:   pick = random.nextInt (n); break;
            case Layer::RRMode::noRepeat:
                pick = random.nextInt (juce::jmax (1, n - 1));
                if (n > 1 && rrLast[g] >= 0 && pick >= rrLast[g])
                    ++pick;
                break;
            case Layer::RRMode::off: break;
        }
        rrLast[g] = pick;
        picks[g] = pick;
    }
    return picks[g] % numSamples;
}

float SamplerEngine::semitonesFor (const Layer& layer, int note) const noexcept
{
    return (float) (note - layer.root) * layer.keyTrack;
}

SamplerEngine::Voice* SamplerEngine::allocateVoice()
{
    for (auto& v : voices)
        if (! v.active)
            return &v;

    // Pool full: take the voice that will be missed least, and cut it.
    Voice* best = nullptr;
    auto score = [] (const Voice& v) { return v.killing ? 0 : (! v.keyDown ? 1 : 2); };
    for (auto& v : voices)
        if (best == nullptr || score (v) < score (*best) || (score (v) == score (*best) && v.order < best->order))
            best = &v;
    freeVoice (*best);
    return best;
}

void SamplerEngine::stealWithinLayer (const Layer& layer)
{
    Voice* victim = nullptr;
    for (auto& v : voices)
    {
        if (! v.active || v.killing || v.isRelease || v.layer->id != layer.id)
            continue;
        if (victim == nullptr)
        {
            victim = &v;
            continue;
        }
        switch (layer.steal)
        {
            case Layer::Steal::oldest:   if (v.order < victim->order) victim = &v; break;
            case Layer::Steal::quietest: if (v.level < victim->level) victim = &v; break;
            case Layer::Steal::lowest:   if (v.note < victim->note)   victim = &v; break;
            case Layer::Steal::highest:  if (v.note > victim->note)   victim = &v; break;
        }
    }
    if (victim != nullptr)
        kill (*victim);
}

SamplerEngine::Voice* SamplerEngine::startVoice (const InstrumentMap& srcMap, const Layer& layer, int note, int velocity,
                                                 float extraGain, bool isRelease, int variation, juce::uint32 eventId,
                                                 float glideFrom, bool hasGlide)
{
    auto* vp = allocateVoice();
    auto& v = *vp;

    v.map = &srcMap;
    ++srcMap.voiceRefs;
    v.layer = &layer;
    v.sample = layer.samples[(size_t) juce::jlimit (0, (int) layer.samples.size() - 1, variation)].get();
    v.note = note;
    v.velocity = velocity;
    v.order = ++voiceOrder;
    v.eventId = eventId;
    v.isRelease = isRelease;
    v.keyDown = ! isRelease;
    v.killing = false;
    v.killGain = 1.0f;
    v.direction = 1;
    v.level = 0;

    // Release samples always play through once; otherwise the Playback Mode applies.
    v.mode = isRelease ? PlayMode::oneShot : bp.playMode;
    v.sliceIndex = ((note - layer.root) % 16 + 16) % 16;
    v.rateScale = v.sample->sampleRate / sampleRate;

    // Slices play at the sample's own pitch: the key picks the slice, not the pitch.
    v.targetSemis = v.mode == PlayMode::slice ? 0.0f : semitonesFor (layer, note);
    v.semis = hasGlide ? glideFrom : v.targetSemis;
    v.glideStep = hasGlide ? (v.targetSemis - v.semis) / (float) juce::jmax (1.0, layer.glideMs * 0.001 * sampleRate) : 0.0f;

    const auto vn = (float) velocity / 127.0f;
    const auto velGain = (1.0f - layer.velToVol) + layer.velToVol * vn * vn;
    const auto g = layer.gain * velGain * layer.zoneGain (note, velocity) * extraGain;
    v.gainL = g * layer.panL;
    v.gainR = g * layer.panR;

    v.env.setSampleRate (sampleRate);
    v.env.setParameters (bp.env);
    v.env.reset();
    v.env.noteOn();

    updateRegion (v);
    v.pos = v.regionStart;
    v.grainHead = v.regionStart + bp.grnPos * (v.regionEnd - v.regionStart);
    v.untilNextGrain = 0;
    for (auto& gr : v.grains)
        gr.active = false;

    v.active = true;
    return vp;
}

void SamplerEngine::retarget (Voice& v, int note, int velocity)
{
    v.note = note;
    v.velocity = velocity;
    v.keyDown = true;
    v.targetSemis = v.mode == PlayMode::slice ? 0.0f : semitonesFor (*v.layer, note);
    if (v.layer->glideMs > 0)
        v.glideStep = (v.targetSemis - v.semis) / (float) juce::jmax (1.0, v.layer->glideMs * 0.001 * sampleRate);
    else
    {
        v.semis = v.targetSemis;
        v.glideStep = 0;
    }
}

void SamplerEngine::kill (Voice& v)
{
    v.killing = true;
}

void SamplerEngine::freeVoice (Voice& v)
{
    if (v.map != nullptr)
        --v.map->voiceRefs;
    v.map = nullptr;
    v.active = false;
    v.killing = false;
}

// =====================================================================================
// rendering
// =====================================================================================
void SamplerEngine::updateRegion (Voice& v) const
{
    const auto len = (double) v.sample->audio.getNumSamples();
    auto s = juce::jlimit (0.0, len, (double) bp.start * len);
    auto e = juce::jlimit (0.0, len, (double) bp.end * len);
    if (e < s)
        std::swap (s, e);
    if (e - s < 16.0)
        e = juce::jmin (len, s + 16.0);

    if (v.mode == PlayMode::slice)
    {
        const auto sliceLen = (e - s) / 16.0;
        v.regionStart = s + v.sliceIndex * sliceLen;
        v.regionEnd = v.regionStart + sliceLen;
    }
    else
    {
        v.regionStart = s;
        v.regionEnd = e;
    }

    // Loop points sit inside the start/end region.
    auto ls = juce::jlimit (s, e, (double) bp.loopStart * len);
    auto le = juce::jlimit (s, e, (double) bp.loopEnd * len);
    if (le < ls)
        std::swap (ls, le);
    v.loopStart = ls;
    v.loopEnd = le;
    v.loopValid = le - ls >= 32.0;
    // The crossfade reads audio from before the loop start, so it can't be longer than that.
    v.xfadeLen = juce::jmin ((double) bp.loopXfadeMs * 0.001 * v.sample->sampleRate, (le - ls) * 0.5, ls);
}

float SamplerEngine::read (const SampleData& s, int channel, double pos) const noexcept
{
    // 4-point Hermite interpolation
    const int len = s.audio.getNumSamples();
    const auto* d = s.audio.getReadPointer (juce::jmin (channel, s.audio.getNumChannels() - 1));
    const int i = (int) std::floor (pos);
    const auto f = (float) (pos - i);
    auto at = [d, len] (int k) { return d[juce::jlimit (0, len - 1, k)]; };
    const auto xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
    const auto c1 = 0.5f * (x1 - xm1);
    const auto c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const auto c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

void SamplerEngine::triggerGrain (Voice& v)
{
    for (auto& g : v.grains)
    {
        if (g.active)
            continue;
        const auto region = v.regionEnd - v.regionStart;
        const auto spray = (double) bp.grnSpray * region * (random.nextDouble() - 0.5);
        const auto pitch = bp.grnPitchRnd > 0 ? std::exp2 ((random.nextFloat() * 2.0f - 1.0f) * bp.grnPitchRnd / 12.0f) : 1.0f;
        g.inc = v.inc * pitch;
        g.length = juce::jmax (8, (int) (bp.grnSizeMs * 0.001 * sampleRate));
        // Centre the grain on the chosen position
        g.pos = juce::jlimit (v.regionStart, v.regionEnd, v.grainHead + spray) - g.inc * g.length * 0.5;
        g.pos = juce::jmax (0.0, g.pos);
        g.age = 0;
        g.active = true;
        return;
    }
}

void SamplerEngine::renderVoice (Voice& v, juce::AudioBuffer<float>& out, int start, int num)
{
    if (! v.active)
        return;

    auto* outL = out.getWritePointer (0, start);
    auto* outR = out.getWritePointer (out.getNumChannels() > 1 ? 1 : 0, start);
    const bool stereoOut = out.getNumChannels() > 1;
    const auto& smp = *v.sample;
    const bool stereoSample = smp.audio.getNumChannels() > 1;
    const auto killStep = (float) (1.0 / (killSeconds * sampleRate));
    const auto endFade = endFadeSeconds * smp.sampleRate;

    // Granular: the cloud is normalised by how many grains overlap
    const auto overlap = bp.grnDensity * bp.grnSizeMs * 0.001f;
    const auto grainNorm = 1.0f / std::sqrt (juce::jmax (1.0f, overlap));
    const auto grainInterval = sampleRate / bp.grnDensity;
    const auto freezePos = v.regionStart + bp.grnPos * (v.regionEnd - v.regionStart);

    for (int i = 0; i < num; ++i)
    {
        // Pitch and glide, every few samples
        if ((i % pitchChunk) == 0)
        {
            if (v.glideStep != 0.0f)
            {
                v.semis += v.glideStep * (float) pitchChunk;
                if ((v.glideStep > 0 && v.semis >= v.targetSemis) || (v.glideStep < 0 && v.semis <= v.targetSemis))
                {
                    v.semis = v.targetSemis;
                    v.glideStep = 0;
                }
            }
            v.inc = v.rateScale * std::exp2 ((v.semis + bp.tuneSemis) / 12.0f);
        }

        float l = 0, r = 0;
        bool finished = false;

        switch (v.mode)
        {
            case PlayMode::oneShot:
            case PlayMode::slice:
            {
                if (v.pos >= v.regionEnd) { finished = true; break; }
                const auto fade = (float) juce::jmin (1.0, (v.regionEnd - v.pos) / endFade);
                l = read (smp, 0, v.pos) * fade;
                r = stereoSample ? read (smp, 1, v.pos) * fade : l;
                v.pos += v.inc;
                break;
            }

            case PlayMode::loop:
            {
                if (! v.loopValid)
                {
                    // No usable loop: behave like a one-shot that stops at the end
                    if (v.pos >= v.regionEnd) { finished = true; break; }
                    l = read (smp, 0, v.pos);
                    r = stereoSample ? read (smp, 1, v.pos) : l;
                    v.pos += v.inc;
                    break;
                }
                const auto loopLen = v.loopEnd - v.loopStart;
                if (v.pos >= v.loopEnd)
                    v.pos = v.loopStart + std::fmod (v.pos - v.loopStart, loopLen);

                l = read (smp, 0, v.pos);
                r = stereoSample ? read (smp, 1, v.pos) : l;

                // Equal-power crossfade into the audio just before the loop start
                if (v.xfadeLen > 1.0 && v.pos > v.loopEnd - v.xfadeLen)
                {
                    const auto t = (float) ((v.pos - (v.loopEnd - v.xfadeLen)) / v.xfadeLen);
                    const auto a = std::cos (t * juce::MathConstants<float>::halfPi);
                    const auto b = std::sin (t * juce::MathConstants<float>::halfPi);
                    const auto other = v.pos - loopLen;
                    l = l * a + read (smp, 0, other) * b;
                    r = stereoSample ? r * a + read (smp, 1, other) * b : l;
                }
                v.pos += v.inc;
                break;
            }

            case PlayMode::pingPong:
            {
                if (! v.loopValid && v.pos >= v.regionEnd) { finished = true; break; }
                l = read (smp, 0, v.pos);
                r = stereoSample ? read (smp, 1, v.pos) : l;
                v.pos += v.inc * v.direction;
                if (v.loopValid)
                {
                    if (v.direction > 0 && v.pos >= v.loopEnd)
                    {
                        v.pos = juce::jmax (v.loopStart, v.loopEnd - (v.pos - v.loopEnd));
                        v.direction = -1;
                    }
                    else if (v.direction < 0 && v.pos <= v.loopStart)
                    {
                        v.pos = juce::jmin (v.loopEnd, v.loopStart + (v.loopStart - v.pos));
                        v.direction = 1;
                    }
                }
                break;
            }

            case PlayMode::granular:
            {
                if (--v.untilNextGrain <= 0)
                {
                    triggerGrain (v);
                    v.untilNextGrain = juce::jmax (1, (int) (grainInterval * (0.75 + 0.5 * random.nextDouble())));
                }
                for (auto& g : v.grains)
                {
                    if (! g.active)
                        continue;
                    const auto w = hann ((float) g.age / (float) g.length);
                    l += read (smp, 0, g.pos) * w;
                    if (stereoSample)
                        r += read (smp, 1, g.pos) * w;
                    g.pos += g.inc;
                    if (++g.age >= g.length || g.pos >= smp.audio.getNumSamples())
                        g.active = false;
                }
                if (! stereoSample)
                    r = l;
                l *= grainNorm;
                r *= grainNorm;

                // Freeze holds the cloud at Grain Position; otherwise it drifts through the region in real time.
                if (bp.grnFreeze)
                    v.grainHead = freezePos;
                else
                {
                    v.grainHead += v.rateScale;
                    if (v.grainHead >= v.regionEnd)
                        v.grainHead = v.regionStart + std::fmod (v.grainHead - v.regionStart, juce::jmax (1.0, v.regionEnd - v.regionStart));
                }
                break;
            }
        }

        if (finished)
        {
            freeVoice (v);
            return;
        }

        auto gain = v.env.getNextSample();
        if (v.killing)
        {
            v.killGain -= killStep;
            if (v.killGain <= 0.0f)
            {
                freeVoice (v);
                return;
            }
            gain *= v.killGain;
        }

        if (stereoOut)
        {
            outL[i] += l * gain * v.gainL;
            outR[i] += r * gain * v.gainR;
        }
        else
        {
            outL[i] += 0.5f * (l * v.gainL + r * v.gainR) * gain;
        }
        v.level = gain * juce::jmax (v.gainL, v.gainR);

        if (! v.env.isActive())
        {
            freeVoice (v);
            return;
        }
    }
}
