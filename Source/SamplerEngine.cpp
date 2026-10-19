#include "SamplerEngine.h"
#include "ParameterSpec.h"

namespace
{
    constexpr int pitchChunk = 32;           // samples between pitch/modulation updates
    constexpr double killSeconds = 0.005;    // fade used when a voice is stolen or choked
    constexpr double endFadeSeconds = 0.003; // fade at the end of a one-shot or slice
    constexpr double idleResetSeconds = 2.0; // "2 s idle" round-robin reset
    constexpr float bendRangeSemis = 2.0f;

    // What 100 % of each voice target means
    constexpr float pitchRangeSemis = 24.0f, cutoffRangeOctaves = 6.0f, driveRangeDb = 24.0f;
    constexpr float grainScaleOctaves = 2.0f, grainPitchRndRange = 12.0f;
    constexpr float envAmtOctaves = 6.0f;    // Env 2 at full Env Amt moves the cutoff ±6 octaves

    float hann (float x) { return 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * x); }

    // LFO shapes, the same as the UI's display (ui/js/displays.js). u is the phase in 0..1.
    float lfoShape (int shape, float u, float held)
    {
        switch (shape)
        {
            case 0:  return std::sin (u * juce::MathConstants<float>::twoPi);
            case 1:  return u < 0.25f ? u * 4.0f : (u < 0.75f ? 2.0f - u * 4.0f : u * 4.0f - 4.0f);
            case 2:  return 1.0f - 2.0f * u;
            case 3:  return u < 0.5f ? 1.0f : -1.0f;
            default: return held;   // sample & hold: a new random value every cycle
        }
    }
}

SamplerEngine::ParamList SamplerEngine::makeParamList (const juce::var& spec, const std::function<std::atomic<float>* (const juce::String&)>& valueFor)
{
    ParamList list;
    if (const auto* all = spec["params"].getArray())
        for (const auto& p : *all)
        {
            ParamRef r;
            r.id = p["id"].toString();
            r.value = valueFor (r.id);
            const auto type = p["type"].toString();
            if (type == "float")
            {
                r.range = ParameterSpec::rangeFor (p);
                r.modulatable = true;
            }
            else if (type == "choice")
                r.range = { 0.0f, (float) juce::jmax (1, p["choices"].size() - 1), 1.0f };
            else
                r.range = { 0.0f, 1.0f, 1.0f };
            list.push_back (r);
        }
    return list;
}

SamplerEngine::SamplerEngine (ParamList p) : params (std::move (p))
{
    rrLast.fill (-1);
    for (const auto& r : params)
        ids.add (r.id);
    paramOffset.assign (params.size(), 0.0f);

    auto i = [this] (const juce::String& id) { return ids.indexOf (id); };
    ix.playMode = i ("play_mode");     ix.loopXfade = i ("loop_xfade");
    ix.stretchMode = i ("stretch_mode"); ix.timeSpeed = i ("time_speed"); ix.formant = i ("formant");
    ix.tune = i ("tune");              ix.fine = i ("fine");
    ix.grnPos = i ("grn_pos");         ix.grnSize = i ("grn_size");      ix.grnDensity = i ("grn_density");
    ix.grnSpray = i ("grn_spray");     ix.grnPitchRnd = i ("grn_pitchrnd"); ix.grnFreeze = i ("grn_freeze");
    const char* adsr[] = { "attack", "decay", "sustain", "release" };
    for (int k = 0; k < 4; ++k)
    {
        ix.env1[k] = i (juce::String ("env1_") + adsr[k]);
        ix.env2[k] = i (juce::String ("env2_") + adsr[k]);
        ix.env3[k] = i (juce::String ("env3_") + adsr[k]);
    }
    ix.fltType = i ("flt_type");       ix.fltCutoff = i ("flt_cutoff");  ix.fltReso = i ("flt_reso");
    ix.fltDrive = i ("flt_drive");     ix.fltEnvAmt = i ("flt_envamt");
    const char* lfo[] = { "shape", "rate", "depth", "phase", "fade" };
    for (int n = 0; n < 4; ++n)
    {
        for (int k = 0; k < 5; ++k)
            ix.lfo[n][k] = i ("lfo" + juce::String (n + 1) + "_" + lfo[k]);
        ix.macro[n] = i ("macro" + juce::String (n + 1));
    }
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
    {
        if (v.active)
            freeVoice (v);
        v.filter.prepare (sampleRate);
    }
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
// parameters and global modulation
// =====================================================================================
float SamplerEngine::param (int index) const noexcept
{
    if (index < 0 || index >= (int) params.size())
        return 0.0f;
    const auto& p = params[(size_t) index];
    const auto value = p.value != nullptr ? p.value->load (std::memory_order_relaxed) : p.range.start;
    const auto offset = paramOffset[(size_t) index];
    if (offset == 0.0f || ! p.modulatable)
        return value;
    return p.range.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, p.range.convertTo0to1 (value) + offset));
}

int SamplerEngine::getPlayheads (Playhead* out, int max) const noexcept
{
    const int n = juce::jmin (max, phCount.load (std::memory_order_acquire));
    for (int k = 0; k < n; ++k)
        out[k] = { phLayer[(size_t) k].load (std::memory_order_relaxed), phVariation[(size_t) k].load (std::memory_order_relaxed),
                   phPosition[(size_t) k].load (std::memory_order_relaxed) };
    return n;
}

void SamplerEngine::readBlockParams()
{
    // Global sources: macros, mod wheel, aftertouch
    std::array<float, Mod::numSources> source {};
    for (int n = 0; n < 4; ++n)
    {
        const auto i = ix.macro[n];
        source[(size_t) (Mod::macro1 + n)] = i >= 0 && params[(size_t) i].value != nullptr ? params[(size_t) i].value->load() * 0.01f : 0.0f;
    }
    source[Mod::modWheel] = modWheel;
    source[Mod::aftertouch] = aftertouch;

    // Per-voice sources moving a whole-instrument knob follow the most recently started note
    const Voice* newest = nullptr;
    for (const auto& v : voices)
        if (v.active && ! v.killing && ! v.isRelease && (newest == nullptr || v.order > newest->order))
            newest = &v;
    if (newest != nullptr)
    {
        source[Mod::env1] = newest->env1Value;
        source[Mod::env2] = newest->env2Value;
        source[Mod::env3] = newest->env3Value;
        for (int i = 0; i < 4; ++i)
            source[(size_t) (Mod::lfo1 + i)] = newest->lfos[i].value;
        source[Mod::velocity] = (float) newest->velocity / 127.0f;
        source[Mod::key] = (float) (newest->note - 60) / 60.0f;
    }

    std::fill (paramOffset.begin(), paramOffset.end(), 0.0f);
    globalMod.fill (0.0f);
    if (map != nullptr)
        for (const auto& r : map->routes)
        {
            const auto v = source[(size_t) r.source] * r.amount;
            if (r.param >= 0 && r.param < (int) paramOffset.size())
                paramOffset[(size_t) r.param] += v;          // any source → a knob
            else if (r.target >= 0 && Mod::isGlobal (r.source))
                globalMod[(size_t) r.target] += v;           // global source → every voice's target
        }

    auto get = [this] (int i, float fallback) { return param (i, fallback); };
    bp.playMode    = (PlayMode) juce::jlimit (0, 4, juce::roundToInt (get (ix.playMode, 0)));
    bp.loopXfadeMs = get (ix.loopXfade, 0);
    bp.stretchMode = juce::jlimit (0, 2, juce::roundToInt (get (ix.stretchMode, 0)));
    bp.speed       = juce::jlimit (0.05f, 8.0f, get (ix.timeSpeed, 100) * 0.01f);
    bp.formantRatio = std::exp2 (get (ix.formant, 0) / 12.0f);
    bp.tuneSemis   = get (ix.tune, 0) + get (ix.fine, 0) * 0.01f + pitchBendSemis;
    bp.grnPos      = get (ix.grnPos, 50) * 0.01f;
    bp.grnSizeMs   = juce::jmax (1.0f, get (ix.grnSize, 80));
    bp.grnDensity  = juce::jmax (0.1f, get (ix.grnDensity, 20));
    bp.grnSpray    = get (ix.grnSpray, 0) * 0.01f;
    bp.grnPitchRnd = get (ix.grnPitchRnd, 0);
    bp.grnFreeze   = get (ix.grnFreeze, 0) >= 0.5f;
    auto adsr = [&get] (const int (&e)[4]) -> juce::ADSR::Parameters
    {
        return { get (e[0], 0.005f), get (e[1], 0.1f), get (e[2], 100) * 0.01f, get (e[3], 0.1f) };
    };
    bp.env  = adsr (ix.env1);
    bp.env2 = adsr (ix.env2);
    bp.env3 = adsr (ix.env3);

    // With no filter parameters (tests), the defaults leave the sound untouched.
    bp.fltType = juce::jlimit (0, 4, juce::roundToInt (get (ix.fltType, 1)));
    bp.cutoff  = get (ix.fltCutoff, 20000);
    bp.reso    = juce::jlimit (0.0f, 1.0f, get (ix.fltReso, 0) * 0.01f);
    bp.driveDb = juce::jmax (0.0f, get (ix.fltDrive, 0));
    bp.envAmt  = get (ix.fltEnvAmt, 0) * 0.01f;
    for (size_t n = 0; n < 4; ++n)
    {
        const auto& l = ix.lfo[n];
        bp.lfo[n] = { juce::jlimit (0, 4, juce::roundToInt (get (l[0], 0))), get (l[1], 1), get (l[2], 0) * 0.01f,
                      get (l[3], 0) / 360.0f, get (l[4], 0) };
    }
}

// =====================================================================================
// processing
// =====================================================================================
void SamplerEngine::process (juce::AudioBuffer<float>& main, juce::AudioBuffer<float>* const* aux, const juce::MidiBuffer& midi, bool transportPlaying)
{
    pickUpNewMap();
    readBlockParams();

    const int numSamples = main.getNumSamples();
    outputs[0] = &main;
    for (int i = 0; i < numAuxOutputs; ++i)
        outputs[i + 1] = aux != nullptr && aux[i] != nullptr && aux[i]->getNumSamples() >= numSamples ? aux[i] : nullptr;

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
            v.env2.setParameters (bp.env2);
            v.env3.setParameters (bp.env3);
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
                renderVoice (v, pos, eventPos - pos);
            now += eventPos - pos;
            pos = eventPos;
        }
        handleMidi (meta.getMessage());
    }
    if (pos < numSamples)
    {
        for (auto& v : voices)
            renderVoice (v, pos, numSamples - pos);
        now += numSamples - pos;
    }

    samplesSinceNoteOn += numSamples;

    int count = 0;
    for (const auto& v : voices)
        count += v.active ? 1 : 0;
    activeVoiceCount.store (count);

    // Playheads for the waveform, newest notes first
    const Voice* list[maxPlayheads] {};
    int n = 0;
    for (const auto& v : voices)
    {
        if (! v.active || v.killing)
            continue;
        if (n < maxPlayheads)
            list[n++] = &v;
        else
        {
            int oldest = 0;
            for (int k = 1; k < n; ++k)
                if (list[k]->order < list[oldest]->order) oldest = k;
            if (v.order > list[oldest]->order) list[oldest] = &v;
        }
    }
    std::sort (list, list + n, [] (const Voice* a, const Voice* b) { return a->order > b->order; });
    for (int k = 0; k < n; ++k)
    {
        const auto& v = *list[k];
        const auto len = (double) juce::jmax (1, v.sample->audio.getNumSamples());
        const auto at = v.mode == PlayMode::granular ? v.grainHead : v.pos;
        phLayer[(size_t) k].store (v.layer->id, std::memory_order_relaxed);
        phVariation[(size_t) k].store (v.variation, std::memory_order_relaxed);
        phPosition[(size_t) k].store ((float) juce::jlimit (0.0, 1.0, at / len), std::memory_order_relaxed);
    }
    phCount.store (n, std::memory_order_release);
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
        bp.tuneSemis = param (ix.tune, 0) + param (ix.fine, 0) * 0.01f + pitchBendSemis;
    }
    else if (m.isController() && m.getControllerNumber() == 1)
        modWheel = (float) m.getControllerValue() / 127.0f;   // takes effect from the next block
    else if (m.isChannelPressure())
        aftertouch = (float) m.getChannelPressureValue() / 127.0f;
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
        v.env2.noteOff();
        v.env3.noteOff();
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
        rrShown[g].store (layer.rrMode == Layer::RRMode::cycle ? rrCounter[g] : pick, std::memory_order_relaxed);
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
    const auto which = (size_t) juce::jlimit (0, (int) layer.samples.size() - 1, variation);
    v.sample = layer.samples[which].get();
    v.region = &layer.regions[which];
    v.variation = (int) which;
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
    const auto g = layer.gain * layer.normalizeGain * velGain * layer.zoneGain (note, velocity) * extraGain;
    v.gainL = g * layer.panL;
    v.gainR = g * layer.panR;

    for (auto [env, p] : { std::pair { &v.env, &bp.env }, { &v.env2, &bp.env2 }, { &v.env3, &bp.env3 } })
    {
        env->setSampleRate (sampleRate);
        env->setParameters (*p);
        env->reset();
        env->noteOn();
    }

    updateRegion (v);
    startModulation (v);   // also gives the sample-start modulation for this note
    v.pos = v.regionStart + juce::jlimit (0.0, 0.999, (double) v.startMod) * (v.regionEnd - v.regionStart);
    v.grainHead = v.regionStart + bp.grnPos * (v.regionEnd - v.regionStart);
    v.untilNextGrain = 0;
    v.stretch = v.mode == PlayMode::granular ? 0 : bp.stretchMode;
    v.timeInc = v.rateScale * bp.speed;
    v.hasPrevGrain = false;
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
    // Each sample (and round-robin variation) has its own points
    const auto& r = *v.region;
    auto s = juce::jlimit (0.0, len, (double) r.start * len);
    auto e = juce::jlimit (0.0, len, (double) r.end * len);
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
    auto ls = juce::jlimit (s, e, (double) r.loopStart * len);
    auto le = juce::jlimit (s, e, (double) r.loopEnd * len);
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
        const auto spray = (double) v.grainSpray * region * (random.nextDouble() - 0.5);
        const auto pitch = v.grainPitchRnd > 0 ? std::exp2 ((random.nextFloat() * 2.0f - 1.0f) * v.grainPitchRnd / 12.0f) : 1.0f;
        g.inc = v.inc * pitch;
        g.length = juce::jmax (8, (int) (bp.grnSizeMs * v.grainSizeMul * 0.001 * sampleRate));
        // Centre the grain on the chosen position (modulation can move it)
        const auto centre = v.grainHead + (double) v.grainPosOffset * region + spray;
        g.pos = juce::jlimit (v.regionStart, v.regionEnd, centre) - g.inc * g.length * 0.5;
        g.pos = juce::jmax (0.0, g.pos);
        g.age = 0;
        g.active = true;
        return;
    }
}

// ---------- Stretch (WSOLA) and Formant (PSOLA) ----------
// Both play the sound as overlapping Hann-windowed grains while the play head (v.pos) moves at the
// speed. Stretch: 60 ms grains at 50 % overlap, each read at the pitch, placed where the waveform best
// continues the previous grain (so pitch changes without changing length). Formant: grains two pitch
// periods long, read at the original rate (times the Formant shift) and started once per *output*
// period, so the pitch changes while the spectral envelope (formants) stays put.
double SamplerEngine::bestMatch (const SampleData& s, double target, double reference, int window, int length) const noexcept
{
    const auto* d = s.audio.getReadPointer (0);
    const int n = s.audio.getNumSamples();
    const int ref = (int) reference, base = (int) target;
    if (ref < 0 || ref + length >= n)
        return target;
    double best = target, bestScore = -1.0e30;
    for (int off = -window; off <= window; off += 2)
    {
        const int c = base + off;
        if (c < 0 || c + length >= n)
            continue;
        double dot = 0, energy = 1.0e-9;
        for (int k = 0; k < length; k += 2)
        {
            dot += (double) d[c + k] * d[ref + k];
            energy += (double) d[c + k] * d[c + k];
        }
        const auto score = dot / std::sqrt (energy);
        if (score > bestScore) { bestScore = score; best = c; }
    }
    return best;
}

void SamplerEngine::startStretchGrain (Voice& v, const SampleData& s)
{
    Grain* g = nullptr;
    for (auto& candidate : v.grains)
        if (! candidate.active) { g = &candidate; break; }
    if (g == nullptr)
        return;

    const auto fileRate = s.sampleRate;
    if (v.stretch == 1)
    {
        const int length = juce::jmax (64, (int) (0.06 * sampleRate));
        const int hop = length / 2;
        auto start = v.pos;
        if (v.hasPrevGrain)
        {
            // Where the previous grain's content naturally continues, searched for near the play head
            const auto natural = v.prevGrainStart + hop * v.inc;
            start = bestMatch (s, v.pos, natural, (int) (0.008 * fileRate), (int) (0.006 * fileRate));
        }
        *g = { true, juce::jmax (0.0, start), v.inc, 0, length, 1.0f };
        v.prevGrainStart = g->pos;
        v.hasPrevGrain = true;
        v.untilNextGrain = hop;
        return;
    }

    // Formant (PSOLA)
    auto period = s.periodSamples > 0 ? (double) s.periodSamples
                                       : fileRate / juce::MidiMessage::getMidiNoteInHertz (v.layer->root);
    period = juce::jlimit (fileRate / 2000.0, fileRate / 30.0, period);
    const auto readInc = v.rateScale * bp.formantRatio;              // formants move with this, not with pitch
    const int length = juce::jmax (16, (int) (2.0 * period / readInc));
    const auto spacing = juce::jmax (1.0, period / v.inc);            // one output period at the played pitch

    auto start = v.pos - period;
    if (v.hasPrevGrain)
    {
        // Stay a whole number of periods from the previous grain (keeps the waveform in phase), then refine
        const auto k = std::round ((start - v.prevGrainStart) / period);
        start = v.prevGrainStart + k * period;
        if (std::abs (k) > 0.5)
            start = bestMatch (s, start, v.prevGrainStart, juce::jmin (64, (int) (period / 4)), juce::jmin (128, (int) period));
    }
    const auto gain = juce::jlimit (0.15f, 2.0f, (float) (2.0 * spacing / length));
    *g = { true, juce::jmax (0.0, start), readInc, 0, length, gain };
    v.prevGrainStart = g->pos;
    v.hasPrevGrain = true;
    v.untilNextGrain = juce::jmax (1, (int) std::round (spacing));
}

void SamplerEngine::stretchFrame (Voice& v, const SampleData& s, bool stereo, float& l, float& r)
{
    if (--v.untilNextGrain <= 0)
        startStretchGrain (v, s);
    l = r = 0;
    const auto last = (double) s.audio.getNumSamples() - 1;
    for (auto& g : v.grains)
    {
        if (! g.active)
            continue;
        const auto w = hann ((float) g.age / (float) g.length) * g.gain;
        l += read (s, 0, g.pos) * w;
        if (stereo)
            r += read (s, 1, g.pos) * w;
        g.pos += g.inc;
        if (++g.age >= g.length || g.pos >= last)
            g.active = false;
    }
    if (! stereo)
        r = l;
}

void SamplerEngine::startModulation (Voice& v)
{
    v.env1Value = v.env2Value = v.env3Value = 0;
    v.age = 0;
    v.samplesSinceMod = 0;
    for (size_t i = 0; i < 4; ++i)
        v.lfos[i] = { (double) bp.lfo[i].phase, 0.0f, random.nextFloat() * 2.0f - 1.0f };
    v.filter.reset();
    v.filterFresh = true;
    updateModulation (v, 0);
}

void SamplerEngine::updateModulation (Voice& v, int rampSamples)
{
    // LFOs: free-running per voice from the note start, with a fade-in
    const auto elapsed = (double) v.samplesSinceMod;
    v.samplesSinceMod = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        const auto& p = bp.lfo[i];
        auto& st = v.lfos[i];
        const auto before = st.phase;
        st.phase += p.rate * elapsed / sampleRate;
        if (std::floor (st.phase) != std::floor (before))
            st.held = random.nextFloat() * 2.0f - 1.0f;
        const auto u = (float) (st.phase - std::floor (st.phase));
        const auto fade = p.fadeMs > 0 ? (float) juce::jmin (1.0, v.age / (p.fadeMs * 0.001 * sampleRate)) : 1.0f;
        st.value = lfoShape (p.shape, u, st.held) * p.depth * fade;
    }

    // Sum the routes into each target: global part (this block) + this voice's sources
    std::array<float, Mod::numTargets> mod = globalMod;
    std::array<float, Mod::numSources> source {};
    source[Mod::env1] = v.env1Value;
    source[Mod::env2] = v.env2Value;
    source[Mod::env3] = v.env3Value;
    for (int i = 0; i < 4; ++i)
        source[(size_t) (Mod::lfo1 + i)] = v.lfos[i].value;
    source[Mod::velocity] = (float) v.velocity / 127.0f;
    source[Mod::key] = (float) (v.note - 60) / 60.0f;
    for (const auto& r : v.map->routes)
        if (! Mod::isGlobal (r.source) && r.target >= 0)
            mod[(size_t) r.target] += source[(size_t) r.source] * r.amount;

    v.pitchMod = mod[Mod::pitch] * pitchRangeSemis;
    v.volumeMod = juce::jlimit (0.0f, 4.0f, 1.0f + mod[Mod::volume]);
    const auto pan = juce::jlimit (-1.0f, 1.0f, mod[Mod::pan]);
    v.panModL = juce::jmin (1.0f, 1.0f - pan);
    v.panModR = juce::jmin (1.0f, 1.0f + pan);
    v.startMod = mod[Mod::start];
    v.grainPosOffset = mod[Mod::grainPos];
    v.grainSizeMul = std::exp2 (mod[Mod::grainSize] * grainScaleOctaves);
    v.grainDensityMul = std::exp2 (mod[Mod::grainDensity] * grainScaleOctaves);
    v.grainSpray = juce::jlimit (0.0f, 1.0f, bp.grnSpray + mod[Mod::grainSpray]);
    v.grainPitchRnd = juce::jmax (0.0f, bp.grnPitchRnd + mod[Mod::grainPitchRnd] * grainPitchRndRange);

    // Filter: the knobs (smoothed, in octaves), Env 2 through Env Amt, plus routed modulation
    const auto knobOct = std::log2 (juce::jmax (20.0f, bp.cutoff));
    if (v.filterFresh)
    {
        v.cutoffOct = knobOct;
        v.resoSmoothed = bp.reso;
        v.filterFresh = false;
    }
    else
    {
        const auto a = 1.0f - std::exp (-(float) juce::jmax (1, rampSamples) / (0.01f * (float) sampleRate));
        v.cutoffOct += a * (knobOct - v.cutoffOct);
        v.resoSmoothed += a * (bp.reso - v.resoSmoothed);
    }
    const auto octaves = v.cutoffOct + v.env2Value * bp.envAmt * envAmtOctaves + mod[Mod::cutoff] * cutoffRangeOctaves;
    v.filter.setTargets (bp.fltType, std::exp2 (octaves), v.resoSmoothed + mod[Mod::reso],
                         bp.driveDb + mod[Mod::drive] * driveRangeDb, rampSamples);
}

void SamplerEngine::renderVoice (Voice& v, int start, int num)
{
    if (! v.active)
        return;

    auto* out = outputs[(size_t) juce::jlimit (0, numAuxOutputs, v.layer->output)];
    if (out == nullptr)
        out = outputs[0];
    auto* outL = out->getWritePointer (0, start);
    auto* outR = out->getWritePointer (out->getNumChannels() > 1 ? 1 : 0, start);
    const bool stereoOut = out->getNumChannels() > 1;
    const auto& smp = *v.sample;
    const bool stereoSample = smp.audio.getNumChannels() > 1;
    const auto killStep = (float) (1.0 / (killSeconds * sampleRate));
    const auto endFade = endFadeSeconds * smp.sampleRate;

    // Granular: the cloud is normalised by how many grains overlap
    const auto overlap = bp.grnDensity * bp.grnSizeMs * 0.001f;
    const auto grainNorm = 1.0f / std::sqrt (juce::jmax (1.0f, overlap));
    const auto freezePos = v.regionStart + bp.grnPos * (v.regionEnd - v.regionStart);

    for (int i = 0; i < num; ++i)
    {
        // Pitch, glide and modulation, every few samples
        if (v.samplesSinceMod >= pitchChunk || i == 0)
        {
            if (v.glideStep != 0.0f)
            {
                v.semis += v.glideStep * (float) juce::jmax (1, v.samplesSinceMod);
                if ((v.glideStep > 0 && v.semis >= v.targetSemis) || (v.glideStep < 0 && v.semis <= v.targetSemis))
                {
                    v.semis = v.targetSemis;
                    v.glideStep = 0;
                }
            }
            updateModulation (v, pitchChunk);
            v.inc = v.rateScale * std::exp2 ((v.semis + bp.tuneSemis + v.pitchMod) / 12.0f);
            v.timeInc = v.rateScale * bp.speed;
        }

        float l = 0, r = 0;
        bool finished = false;
        // Resample: the read position moves at the pitch increment. Stretch / Formant: it moves at
        // the speed (time), and grains supply the pitch.
        const bool stretched = v.stretch != 0 && v.mode != PlayMode::granular;
        const auto step = stretched ? v.timeInc : v.inc;
        auto frame = [&] (double at)
        {
            if (stretched) { stretchFrame (v, smp, stereoSample, l, r); return; }
            l = read (smp, 0, at);
            r = stereoSample ? read (smp, 1, at) : l;
        };

        switch (v.mode)
        {
            case PlayMode::oneShot:
            case PlayMode::slice:
            {
                if (v.pos >= v.regionEnd) { finished = true; break; }
                const auto fade = (float) juce::jmin (1.0, (v.regionEnd - v.pos) / endFade);
                frame (v.pos);
                l *= fade;
                r *= fade;
                v.pos += step;
                break;
            }

            case PlayMode::loop:
            {
                if (! v.loopValid)
                {
                    // No usable loop: behave like a one-shot that stops at the end
                    if (v.pos >= v.regionEnd) { finished = true; break; }
                    frame (v.pos);
                    v.pos += step;
                    break;
                }
                const auto loopLen = v.loopEnd - v.loopStart;
                if (v.pos >= v.loopEnd)
                    v.pos = v.loopStart + std::fmod (v.pos - v.loopStart, loopLen);

                frame (v.pos);

                // Equal-power crossfade into the audio just before the loop start
                // (stretched playback doesn't need it: overlapping grains already blend across the seam)
                if (! stretched && v.xfadeLen > 1.0 && v.pos > v.loopEnd - v.xfadeLen)
                {
                    const auto t = (float) ((v.pos - (v.loopEnd - v.xfadeLen)) / v.xfadeLen);
                    const auto a = std::cos (t * juce::MathConstants<float>::halfPi);
                    const auto b = std::sin (t * juce::MathConstants<float>::halfPi);
                    const auto other = v.pos - loopLen;
                    l = l * a + read (smp, 0, other) * b;
                    r = stereoSample ? r * a + read (smp, 1, other) * b : l;
                }
                v.pos += step;
                break;
            }

            case PlayMode::pingPong:
            {
                if (! v.loopValid && v.pos >= v.regionEnd) { finished = true; break; }
                frame (v.pos);
                v.pos += step * v.direction;
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
                    const auto interval = sampleRate / (bp.grnDensity * v.grainDensityMul);
                    v.untilNextGrain = juce::jmax (1, (int) (interval * (0.75 + 0.5 * random.nextDouble())));
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

        // ---- filter (with drive) and modulated pan ----
        v.filter.process (l, r, stereoSample);
        if (! stereoSample)
            r = l;
        l *= v.panModL;
        r *= v.panModR;

        v.env2Value = v.env2.getNextSample();
        v.env3Value = v.env3.getNextSample();
        ++v.samplesSinceMod;
        v.age += 1.0;

        v.env1Value = v.env.getNextSample();
        auto gain = v.env1Value * v.volumeMod;
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
