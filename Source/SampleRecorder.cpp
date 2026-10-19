#include "SampleRecorder.h"

namespace
{
    const char* numberWords[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                  "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
                                  "nineteen", "twenty" };

    juce::String safeName (juce::String s)
    {
        s = s.retainCharacters ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_- ").trim().replaceCharacter (' ', '_');
        return s.isEmpty() ? juce::String ("Session") : s;
    }
}

SampleRecorder::SampleRecorder()
{
    formats.registerBasicFormats();
    loadVoicePack();
    startTimerHz (30);
}

SampleRecorder::~SampleRecorder()
{
    stopTimer();
}

juce::File SampleRecorder::defaultRecordingFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("mtyaSampler").getChildFile ("Recordings");
}

juce::File SampleRecorder::voicePackFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("mtyaSampler").getChildFile ("VoicePack");
}

// ============================================================================ voice pack
// One WAV per word (c.wav, sharp.wav, three.wav, medium.wav, take.wav ...), made by tools/make-voice-pack.ps1.
void SampleRecorder::loadVoicePack()
{
    for (const auto& f : voicePackFolder().findChildFiles (juce::File::findFiles, false, "*.wav"))
    {
        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (f) })
        {
            if (reader->lengthInSamples <= 0 || reader->lengthInSamples > (juce::int64) (reader->sampleRate * 5))
                continue;
            VoiceClip clip;
            clip.sampleRate = reader->sampleRate;
            clip.audio.setSize (1, (int) reader->lengthInSamples);
            reader->read (&clip.audio, 0, (int) reader->lengthInSamples, 0, true, false);

            // Text-to-speech files carry silence at both ends; trim it so prompts flow quickly.
            const int n = clip.audio.getNumSamples();
            const float floor = juce::Decibels::decibelsToGain (-45.0f) * juce::jmax (0.001f, clip.audio.getMagnitude (0, n));
            int a = 0, b = n - 1;
            while (a < n && std::abs (clip.audio.getSample (0, a)) < floor) ++a;
            while (b > a && std::abs (clip.audio.getSample (0, b)) < floor) --b;
            if (b <= a) continue;
            a = juce::jmax (0, a - (int) (0.01 * clip.sampleRate));
            b = juce::jmin (n - 1, b + (int) (0.03 * clip.sampleRate));
            juce::AudioBuffer<float> trimmed (1, b - a + 1);
            trimmed.copyFrom (0, 0, clip.audio, 0, a, b - a + 1);
            clip.audio = std::move (trimmed);
            voiceIndex[f.getFileNameWithoutExtension().toLowerCase()] = (int) voiceClips.size();
            voiceClips.push_back (std::move (clip));
        }
    }
}

std::vector<int> SampleRecorder::promptFor (const juce::String& text) const
{
    std::vector<int> out;
    if (voiceClips.empty()) return out;
    auto words = juce::StringArray::fromTokens (text.toLowerCase().replaceCharacters (".,", "  "), " ", "");
    words.removeEmptyStrings();
    for (auto w : words)
    {
        if (w.containsOnly ("0123456789") && w.getIntValue() <= 20) w = numberWords[w.getIntValue()];
        auto it = voiceIndex.find (w);
        if (it == voiceIndex.end()) return {};    // a word is missing → let the UI speak instead
        out.push_back (it->second);
    }
    return out;
}

// ============================================================================ setup
void SampleRecorder::prepare (double sr, int)
{
    const juce::SpinLock::ScopedLockType lock (planLock);
    sampleRate = sr;
    preroll.setSize (2, juce::jmax (1, (int) (sr * 0.5)));
    preroll.clear();
    prerollWrite = 0;
    envDecay = (float) std::exp (-1.0 / (0.05 * sr));    // ~50 ms release for the level follower
}

juce::var SampleRecorder::startSession (const juce::var& j)
{
    auto p = std::make_unique<Plan>();
    const auto s = j["settings"];
    const auto t = j["timing"];

    p->announceSec = (double) t["announceSec"];
    p->toneSec     = (double) t["toneSec"];
    p->beatSec     = juce::jmax (0.1, (double) t["beatSec"]);
    p->countBeats  = juce::jlimit (0, 8, (int) t["countBeats"]);
    p->maxSec      = juce::jlimit (0.5, 30.0, (double) t["maxSec"]);
    p->threshold   = juce::Decibels::decibelsToGain ((float) (double) s["thresholdDb"]);
    p->silenceMs   = (int) s["silenceMs"];
    p->prerollMs   = juce::jlimit (0, 500, (int) s["prerollMs"]);
    p->fadeMs      = (int) s["fadeMs"];
    p->normalize   = s["normalize"].toString();
    p->autoAdvance = (bool) s["autoAdvance"];
    p->midi        = (bool) s["midi"];
    p->midiChannel = juce::jlimit (1, 16, s.hasProperty ("midiChannel") ? (int) s["midiChannel"] : 1);
    p->holdSec     = juce::jlimit (0.05, 60.0, s.hasProperty ("holdSec") ? (double) s["holdSec"] : 2.0);
    p->pitched     = (bool) s["pitched"];
    const auto in  = s["input"].toString();
    p->inputMode   = in == "left" ? 1 : in == "right" ? 2 : 0;

    const auto folderText = s["folder"].toString().trim();
    p->folder = folderText.isNotEmpty() && juce::File::isAbsolutePath (folderText)
                    ? juce::File (folderText)
                    : defaultRecordingFolder().getChildFile (safeName (s["name"].toString()));
    if (! p->folder.createDirectory())
        return juce::JSON::parse (R"({"ok":false,"message":"Could not create the recording folder"})");

    const bool voice = (bool) s["voice"];
    bool allPrompts = voice && hasVoicePack();
    if (auto* takes = j["takes"].getArray())
    {
        for (const auto& tk : *takes)
        {
            Take take;
            take.note = (int) tk["note"];
            take.rr   = (int) tk["rr"];
            take.v    = (int) tk["v"];
            take.file = tk["file"].toString();
            take.velocity = juce::jlimit (1, 127, tk.hasProperty ("velocity") ? (int) tk["velocity"] : 100);
            if (voice) take.prompt = promptFor (tk["say"].toString());
            allPrompts = allPrompts && ! take.prompt.empty();
            p->takes.push_back (std::move (take));
        }
    }
    if (p->takes.empty())
        return juce::JSON::parse (R"({"ok":false,"message":"The plan has no takes"})");

    p->voiceInUi = voice && ! allPrompts;
    if (p->voiceInUi) for (auto& tk : p->takes) tk.prompt.clear();
    for (int k = 0; k < 9; ++k)
    {
        auto it = voiceIndex.find (numberWords[k]);
        p->countClips[(size_t) k] = (voice && ! p->voiceInUi && it != voiceIndex.end()) ? it->second : -1;
    }

    // allocate before taking the lock (no allocation while the audio thread waits)
    juce::AudioBuffer<float> buffer (2, (int) ((p->maxSec + 0.6) * sampleRate) + 16);

    savedTakes.clear();
    sessionNormalized = p->normalize != "session";
    emptyRetries = 0; emptyRetryTake = -1;
    savedCount = 0; lastSaved = -1; pendingSkip = -1; takeReady = false;

    {
        const juce::SpinLock::ScopedLockType lock (planLock);
        plan = std::move (p);
        std::swap (recBuffer, buffer);
        status = Status::running;
        take = 0;
        enterPhase (Phase::announce);
        publish();
    }
    return juce::JSON::parse (R"({"ok":true})");
}

// ============================================================================ commands (message thread)
void SampleRecorder::push (Cmd c, int arg)
{
    const auto scope = cmdFifo.write (1);
    if (scope.blockSize1 > 0)      cmdBuffer[(size_t) scope.startIndex1] = { c, arg };
    else if (scope.blockSize2 > 0) cmdBuffer[(size_t) scope.startIndex2] = { c, arg };
}

juce::var SampleRecorder::command (const juce::String& name, const juce::var& arg)
{
    if (name == "start")   return startSession (arg);
    if (name == "monitor") { monitor = (bool) arg; return true; }
    if (name == "pause")   push (Cmd::pause);
    else if (name == "resume") push (Cmd::resume);
    else if (name == "retake") push (Cmd::retake);
    else if (name == "skip")   push (Cmd::skip);
    else if (name == "next")   push (Cmd::next);
    else if (name == "goto")   push (Cmd::gotoTake, (int) arg);
    else if (name == "stop")   { push (Cmd::stop); normalizeSession(); }
    else return juce::JSON::parse (R"({"ok":false,"message":"Unknown recorder command"})");
    return juce::JSON::parse (R"({"ok":true})");
}

// ============================================================================ audio thread
void SampleRecorder::applyCommands()
{
    const auto scope = cmdFifo.read (cmdFifo.getNumReady());
    auto run = [this] (int start, int size)
    {
        for (int i = start; i < start + size; ++i)
        {
            const auto [c, arg] = cmdBuffer[(size_t) i];
            const bool saving = phase == Phase::save;
            switch (c)
            {
                case Cmd::pause:
                    if (saving) pauseAfterSave = true;
                    else if (status == Status::running) { status = Status::paused; phase = Phase::idle; }
                    break;
                case Cmd::resume:   if (status == Status::paused) { status = Status::running; enterPhase (Phase::announce); } break;
                case Cmd::retake:   if (! saving) goTo (lastSaved.load() >= 0 ? lastSaved.load() : take); break;
                case Cmd::skip:     if (! saving && (status == Status::running || status == Status::paused)) { pendingSkip = take; status = Status::running; finishTake (true); } break;
                case Cmd::next:     if (status == Status::review) { status = Status::running; finishTake (true); } break;
                case Cmd::gotoTake: if (! saving) goTo (arg); break;
                case Cmd::stop:     status = Status::idle; phase = Phase::idle; break;
                case Cmd::afterSave:
                    if (phase != Phase::save) break;
                    if (pauseAfterSave) { pauseAfterSave = false; finishTake (true); if (status == Status::running) { status = Status::paused; phase = Phase::idle; } }
                    else if (arg == 1) enterPhase (Phase::announce);                  // try the same take again
                    else if (plan->autoAdvance) finishTake (true);
                    else { status = Status::review; phase = Phase::idle; }
                    break;
            }
        }
    };
    run (scope.startIndex1, scope.blockSize1);
    run (scope.startIndex2, scope.blockSize2);
}

void SampleRecorder::goTo (int index)
{
    if (plan == nullptr) return;
    take = juce::jlimit (0, (int) plan->takes.size() - 1, index);
    status = Status::running;
    enterPhase (Phase::announce);
}

void SampleRecorder::finishTake (bool advance)
{
    if (advance && take + 1 < (int) plan->takes.size()) { ++take; enterPhase (Phase::announce); }
    else { status = Status::done; phase = Phase::idle; }
}

juce::int64 SampleRecorder::lengthOf (Phase p) const
{
    const auto& pl = *plan;
    switch (p)
    {
        case Phase::announce:
        {
            const auto& prompt = pl.takes[(size_t) take].prompt;
            if (prompt.empty()) return (juce::int64) (pl.announceSec * sampleRate);
            double secs = 0.3;
            for (auto c : prompt) secs += voiceClips[(size_t) c].audio.getNumSamples() / voiceClips[(size_t) c].sampleRate + 0.08;
            return (juce::int64) (secs * sampleRate);
        }
        case Phase::tone:    return pl.pitched && ! pl.midi ? (juce::int64) (pl.toneSec * sampleRate) : 0;
        case Phase::countin: return pl.midi ? 0 : (juce::int64) (pl.countBeats * pl.beatSec * sampleRate);
        case Phase::record:  return maxRecSamples;
        case Phase::save:    return std::numeric_limits<juce::int64>::max();
        case Phase::idle:    break;
    }
    return 0;
}

void SampleRecorder::enterPhase (Phase p)
{
    phase = p;
    pos = 0;
    len = lengthOf (p);
    if (p == Phase::tone && len == 0)    return enterPhase (Phase::countin);
    if (p == Phase::countin && len == 0) return enterPhase (Phase::record);

    const auto& tk = plan->takes[(size_t) take];
    if (p == Phase::announce) { promptIndex = 0; promptPos = 0.0; promptGap = 0; }
    if (p == Phase::tone)
    {
        tonePhase = 0.0;
        toneDelta = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (tk.note) / sampleRate;
    }
    if (p == Phase::countin) countClip = -1;
    if (p == Phase::record)
    {
        // start with the last few ms before GO, so early attacks are kept
        const int pre = juce::jmin ((int) (plan->prerollMs * sampleRate / 1000.0), preroll.getNumSamples());
        for (int i = 0; i < pre; ++i)
        {
            const int r = (prerollWrite - pre + i + preroll.getNumSamples()) % preroll.getNumSamples();
            recBuffer.setSample (0, i, preroll.getSample (0, r));
            recBuffer.setSample (1, i, preroll.getSample (1, r));
        }
        recPos = pre;
        maxRecSamples = juce::jmin ((juce::int64) recBuffer.getNumSamples(), (juce::int64) (plan->maxSec * sampleRate) + pre);
        len = maxRecSamples;
        silentSamples = 0;
        envelope = 0.0f;
        heard = false;
        clipFlag = false;
        sendNoteOn = plan->midi;
        for (auto& lp : livePeaks) lp.store (0.0f);
    }
}

float SampleRecorder::guideSample()
{
    const auto& pl = *plan;
    switch (phase)
    {
        case Phase::announce:
        {
            const auto& prompt = pl.takes[(size_t) take].prompt;
            if (promptIndex >= (int) prompt.size()) return 0.0f;
            if (promptGap > 0) { --promptGap; return 0.0f; }
            const auto& clip = voiceClips[(size_t) prompt[(size_t) promptIndex]];
            const int i = (int) promptPos;
            if (i + 1 >= clip.audio.getNumSamples())
            {
                ++promptIndex; promptPos = 0.0; promptGap = (juce::int64) (0.08 * sampleRate);
                return 0.0f;
            }
            const float frac = (float) (promptPos - i);
            const float v = clip.audio.getSample (0, i) * (1.0f - frac) + clip.audio.getSample (0, i + 1) * frac;
            promptPos += clip.sampleRate / sampleRate;
            return v * 0.8f;
        }
        case Phase::tone:
        {
            const double edge = 0.015 * sampleRate;
            const auto fade = (float) juce::jmin (1.0, pos / edge, (len - pos) / edge);
            const auto v = (float) std::sin (tonePhase) * 0.12f * fade;
            tonePhase += toneDelta;
            return v;
        }
        case Phase::countin:
        {
            const auto beatLen = (juce::int64) (pl.beatSec * sampleRate);
            const auto beat = (int) (pos / beatLen);
            const auto inBeat = pos - (juce::int64) beat * beatLen;
            const bool last = beat == pl.countBeats - 1;
            float v = (float) (std::sin (juce::MathConstants<double>::twoPi * (last ? 1760.0 : 1320.0) * inBeat / sampleRate)
                               * std::exp (-inBeat / (0.012 * sampleRate)) * 0.3);
            if (inBeat == 0) { const int n = pl.countBeats - beat; countClip = n <= 8 ? pl.countClips[(size_t) n] : -1; countPos = 0.0; }
            if (countClip >= 0)
            {
                const auto& clip = voiceClips[(size_t) countClip];
                const int i = (int) countPos;
                if (i + 1 < clip.audio.getNumSamples()) { v += clip.audio.getSample (0, i) * 0.7f; countPos += clip.sampleRate / sampleRate; }
                else countClip = -1;
            }
            return v;
        }
        case Phase::record: case Phase::save: case Phase::idle: break;
    }
    return 0.0f;
}

void SampleRecorder::process (const juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output, juce::MidiBuffer& midiOut, int numSamples)
{
    const juce::SpinLock::ScopedTryLockType lock (planLock);
    if (! lock.isLocked() || preroll.getNumSamples() == 0) return;

    // MIDI mode: a held note is always released once its take stops recording (save, skip, stop, pause…)
    auto releaseNote = [&] (int at)
    {
        if (heldNote < 0) return;
        midiOut.addEvent (juce::MidiMessage::noteOff (heldChannel, heldNote), at);
        heldNote = -1;
    };

    if (plan != nullptr) applyCommands();

    const int inCh = input.getNumChannels();
    const int outCh = output.getNumChannels();
    const int mode = plan != nullptr ? plan->inputMode : 0;
    const bool mon = monitor.load();
    float blockPeak = 0.0f, recPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float l = inCh > 0 ? input.getSample (0, i) : 0.0f;
        float r = inCh > 1 ? input.getSample (1, i) : l;
        if (mode == 1) r = l; else if (mode == 2) l = r;
        const float a = juce::jmax (std::abs (l), std::abs (r));
        blockPeak = juce::jmax (blockPeak, a);

        preroll.setSample (0, prerollWrite, l);
        preroll.setSample (1, prerollWrite, r);
        prerollWrite = (prerollWrite + 1) % preroll.getNumSamples();

        if (mon) { output.addSample (0, i, l); if (outCh > 1) output.addSample (1, i, r); }

        if (heldNote >= 0 && (plan == nullptr || status != Status::running || phase != Phase::record))
            releaseNote (i);

        if (plan == nullptr || status != Status::running || phase == Phase::idle) continue;

        if (phase == Phase::record)
        {
            if (sendNoteOn)
            {
                // The note starts at the first recorded sample; the pre-roll before it stays silent
                const auto& tk = plan->takes[(size_t) take];
                releaseNote (i);
                heldNote = juce::jlimit (0, 127, tk.note);
                heldChannel = plan->midiChannel;
                heldSamples = 0;
                midiOut.addEvent (juce::MidiMessage::noteOn (heldChannel, heldNote, (juce::uint8) tk.velocity), i);
                sendNoteOn = false;
            }
            if (heldNote >= 0 && ++heldSamples >= (juce::int64) (plan->holdSec * sampleRate))
                releaseNote (i);

            if (recPos < maxRecSamples)
            {
                recBuffer.setSample (0, (int) recPos, l);
                recBuffer.setSample (1, (int) recPos, r);
                ++recPos;
            }
            recPeak = juce::jmax (recPeak, a);
            if (a >= 0.999f) clipFlag = true;
            envelope = juce::jmax (a, envelope * envDecay);
            if (envelope > plan->threshold) heard = true;
            silentSamples = (heard && envelope < plan->threshold * 0.5f) ? silentSamples + 1 : 0;
            // In MIDI mode the take can only end on silence after the note was released (its tail is kept)
            const bool silenceStop = plan->silenceMs > 0 && heard && heldNote < 0 && ! sendNoteOn
                                     && silentSamples > (juce::int64) (plan->silenceMs * sampleRate / 1000.0);
            if (recPos >= maxRecSamples || silenceStop)
            {
                pubRecLen = recPos;
                enterPhase (Phase::save);
                takeReady = true;       // the message thread saves it and replies with afterSave
            }
            continue;
        }

        if (phase == Phase::save) continue;

        const float g = guideSample();
        for (int ch = 0; ch < outCh; ++ch) output.addSample (ch, i, g);

        if (++pos >= len)
        {
            if (phase == Phase::announce)     enterPhase (Phase::tone);
            else if (phase == Phase::tone)    enterPhase (Phase::countin);
            else if (phase == Phase::countin) enterPhase (Phase::record);
        }
    }

    if (heldNote >= 0 && (plan == nullptr || status != Status::running || phase != Phase::record))
        releaseNote (juce::jmax (0, numSamples - 1));

    if (phase == Phase::record || (phase == Phase::save && recPeak > 0.0f))
    {
        const auto bucket = (size_t) juce::jlimit<juce::int64> (0, numLivePeaks - 1, (recPos - 1) * numLivePeaks / juce::jmax<juce::int64> (1, maxRecSamples));
        livePeaks[bucket].store (juce::jmax (livePeaks[bucket].load(), recPeak));
    }
    inLevel.store (juce::jmax (blockPeak, inLevel.load() * 0.85f));
    publish();
}

void SampleRecorder::publish()
{
    pubStatus = (int) status;
    pubPhase = (int) phase;
    pubTake = take;
    const bool rec = phase == Phase::record || phase == Phase::save;
    pubPos = rec ? recPos : pos;
    pubLen = rec ? maxRecSamples : len;
    if (phase == Phase::countin && plan != nullptr)
        pubCount = plan->countBeats - (int) (pos / juce::jmax<juce::int64> (1, (juce::int64) (plan->beatSec * sampleRate)));
    else
        pubCount = 0;
}

// ============================================================================ message thread
void SampleRecorder::timerCallback()
{
    if (filesRewritten.exchange (false) && onFilesRewritten)
        onFilesRewritten();

    const int skipped = pendingSkip.exchange (-1);
    if (skipped >= 0 && onTake)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("index", skipped);
        o->setProperty ("skipped", true);
        onTake (juce::var (o));
    }

    if (takeReady.load())
    {
        takeReady = false;
        handleFinishedTake();
    }

    if ((Status) pubStatus.load() == Status::done)
        normalizeSession();
}

void SampleRecorder::handleFinishedTake()
{
    // The audio thread is parked in Phase::save, so recBuffer is not being written.
    const auto& pl = *plan;
    const int index = pubTake.load();
    const auto& tk = pl.takes[(size_t) index];
    const auto total = (int) pubRecLen.load();
    const int numCh = pl.inputMode == 0 ? 2 : 1;

    auto* result = new juce::DynamicObject();
    result->setProperty ("index", index);
    if (! heard.load())
    {
        // Nothing crossed the start level: try the same take once more, then move on.
        result->setProperty ("empty", true);
        const bool retry = emptyRetryTake != index;
        emptyRetryTake = retry ? index : -1;
        if (onTake) onTake (juce::var (result));
        push (Cmd::afterSave, retry ? 1 : 0);
        return;
    }
    emptyRetryTake = -1;

    // ---- trim: from just before the first sound to just after the last ----
    auto peakAt = [&] (int i) { return juce::jmax (std::abs (recBuffer.getSample (0, i)), std::abs (recBuffer.getSample (1, i))); };
    const int pre = (int) (pl.prerollMs * sampleRate / 1000.0);
    int onset = 0;
    while (onset < total && peakAt (onset) < pl.threshold) ++onset;
    int start = juce::jmax (0, onset - juce::jmax (pre, (int) (0.003 * sampleRate)));
    int end = total - 1;
    while (end > onset && peakAt (end) < pl.threshold * 0.25f) --end;
    end = juce::jmin (total, end + (int) (0.05 * sampleRate));
    const int length = juce::jmax (1, end - start);

    juce::AudioBuffer<float> out (numCh, length);
    for (int ch = 0; ch < numCh; ++ch)
        out.copyFrom (ch, 0, recBuffer, numCh == 1 ? 0 : ch, start, length);

    out.applyGainRamp (0, juce::jmin (length, (int) (0.002 * sampleRate)), 0.0f, 1.0f);
    const int fade = juce::jmin (length, (int) (pl.fadeMs * sampleRate / 1000.0));
    if (fade > 0) out.applyGainRamp (length - fade, fade, 1.0f, 0.0f);

    float peak = out.getMagnitude (0, length);
    const bool clipped = clipFlag.load();
    if (pl.normalize == "take" && peak > 0.0f && ! clipped) { out.applyGain (0.891f / peak); peak = 0.891f; }

    // ---- write a 24-bit WAV ----
    const auto file = pl.folder.getChildFile (tk.file);
    file.deleteFile();
    bool ok = false;
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        juce::WavAudioFormat wav;
        if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                           .withSampleRate (sampleRate)
                                                           .withNumChannels (numCh)
                                                           .withBitsPerSample (24)))
            ok = writer->writeFromAudioSampleBuffer (out, 0, length);
    }

    // ---- report ----
    juce::Array<juce::var> peaks;
    const int buckets = 120;
    for (int b = 0; b < buckets; ++b)
    {
        const int s0 = b * length / buckets, s1 = juce::jmax (s0 + 1, (b + 1) * length / buckets);
        peaks.add (out.getMagnitude (s0, juce::jmin (s1, length) - s0));
    }
    result->setProperty ("file", tk.file);
    result->setProperty ("path", file.getFullPathName());
    result->setProperty ("lengthSec", length / sampleRate);
    result->setProperty ("peakDb", std::round (juce::Decibels::gainToDecibels (peak) * 10.0f) / 10.0f);
    result->setProperty ("clipped", clipped);
    result->setProperty ("empty", false);
    result->setProperty ("peaks", peaks);
    if (! ok) { result->setProperty ("error", "Could not write " + file.getFullPathName()); }

    if (ok)
    {
        savedTakes.erase (std::remove_if (savedTakes.begin(), savedTakes.end(), [&] (const SavedTake& s) { return s.file == file; }), savedTakes.end());
        savedTakes.push_back ({ file, peak });
        ++savedCount;
        lastSaved = index;
    }
    if (onTake) onTake (juce::var (result));
    push (Cmd::afterSave, 0);
}

// Bring the loudest take of the session to -1 dBFS and every other take by the same amount,
// so velocity layers keep their natural balance.
void SampleRecorder::normalizeSession()
{
    if (sessionNormalized || savedTakes.empty()) return;
    sessionNormalized = true;

    float loudest = 0.0f;
    for (const auto& s : savedTakes) loudest = juce::jmax (loudest, s.peak);
    if (loudest <= 0.0f || loudest >= 0.89f) return;
    const float gain = 0.891f / loudest;

    std::vector<juce::File> files;
    for (const auto& s : savedTakes)
        files.push_back (s.file);

    // Rewriting every take can take a while on a big session, so it runs on its own thread.
    normalizePool.addJob ([this, files, gain]
    {
        juce::AudioFormatManager readers;
        readers.registerBasicFormats();
        for (const auto& file : files)
        {
            std::unique_ptr<juce::AudioFormatReader> reader { readers.createReaderFor (file) };
            if (reader == nullptr) continue;
            juce::AudioBuffer<float> buf ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&buf, 0, buf.getNumSamples(), 0, true, true);
            const auto sr = reader->sampleRate;
            reader.reset();
            buf.applyGain (gain);

            const auto tmp = file.getSiblingFile (file.getFileNameWithoutExtension() + ".tmp.wav");
            bool ok = false;
            {
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (tmp);
                juce::WavAudioFormat wav;
                if (auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                                   .withSampleRate (sr)
                                                                   .withNumChannels (buf.getNumChannels())
                                                                   .withBitsPerSample (24)))
                    ok = writer->writeFromAudioSampleBuffer (buf, 0, buf.getNumSamples());
            }
            if (ok) tmp.moveFileTo (file);
            else tmp.deleteFile();
        }
        filesRewritten = true;   // the timer tells the processor, which reloads the samples
    });
}

juce::var SampleRecorder::getState() const
{
    static const char* statusNames[] = { "idle", "running", "paused", "review", "done" };
    static const char* phaseNames[]  = { "idle", "announce", "tone", "countin", "record", "save" };

    auto* o = new juce::DynamicObject();
    const auto ph = (Phase) pubPhase.load();
    const double sr = sampleRate > 0 ? sampleRate : 44100.0;
    o->setProperty ("status", statusNames[pubStatus.load()]);
    o->setProperty ("phase", phaseNames[(int) ph]);
    o->setProperty ("take", pubTake.load());
    o->setProperty ("total", plan != nullptr ? (int) plan->takes.size() : 0);
    o->setProperty ("saved", savedCount.load());
    o->setProperty ("lastSaved", lastSaved.load());
    o->setProperty ("count", pubCount.load());
    o->setProperty ("phaseTime", pubPos.load() / sr);
    o->setProperty ("phaseLen", ph == Phase::save ? 0.0 : pubLen.load() / sr);
    o->setProperty ("recSec", ph == Phase::record ? pubPos.load() / sr : 0.0);
    o->setProperty ("maxSec", plan != nullptr ? plan->maxSec : 0.0);
    o->setProperty ("inDb", juce::Decibels::gainToDecibels (inLevel.load(), -90.0f));
    o->setProperty ("clip", clipFlag.load());
    o->setProperty ("voiceInUi", plan == nullptr || plan->voiceInUi);
    o->setProperty ("voicePack", hasVoicePack());

    juce::Array<juce::var> peaks;
    if (ph == Phase::record || ph == Phase::save)
    {
        const auto filled = juce::jlimit<juce::int64> (0, numLivePeaks, pubPos.load() * numLivePeaks / juce::jmax<juce::int64> (1, pubLen.load()) + 1);
        for (int i = 0; i < (int) filled; ++i) peaks.add (livePeaks[(size_t) i].load());
    }
    o->setProperty ("peaks", peaks);
    return juce::var (o);
}
