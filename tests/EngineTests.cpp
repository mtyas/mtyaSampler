// Offline tests for the sampler engine: builds instruments from JSON with generated WAV files,
// plays MIDI into SamplerEngine and checks what comes out (pitch, which sample played, level).
//
// Build and run:  cmake --build <build dir> --target mtyaEngineTests, then run the executable.
// It prints one line per check and exits with 1 if any failed.

#include "../Source/InstrumentMap.h"
#include "../Source/SamplerEngine.h"
#include <iostream>

namespace
{
    constexpr double hostRate = 48000.0;
    constexpr int blockSize = 256;
    int failures = 0;

    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! ok)
            ++failures;
    }

    // Writes a sine (or any generator) to a 24-bit WAV and returns its path
    juce::File writeWav (const juce::File& dir, const juce::String& name, double rate, int length,
                         std::function<float (int)> gen, int channels = 1)
    {
        auto file = dir.getChildFile (name);
        file.deleteFile();
        juce::AudioBuffer<float> buf (channels, length);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < length; ++i)
                buf.setSample (ch, i, gen (i));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream().release());
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (rate)
                                                                                 .withNumChannels (channels)
                                                                                 .withBitsPerSample (24));
        writer->writeFromAudioSampleBuffer (buf, 0, length);
        return file;
    }

    juce::File sine (const juce::File& dir, const juce::String& name, double hz, double seconds = 1.0, double rate = 44100.0)
    {
        return writeWav (dir, name, rate, (int) (seconds * rate),
                         [=] (int i) { return 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate); });
    }

    // Parameter values the engine reads (normally owned by the APVTS)
    struct FakeParams
    {
        std::map<juce::String, std::unique_ptr<std::atomic<float>>> values;

        std::atomic<float>* operator() (const char* id, float v)
        {
            auto& slot = values[id];
            slot = std::make_unique<std::atomic<float>> (v);
            return slot.get();
        }
        void set (const char* id, float v) { jassert (values.count (id) > 0); values[id]->store (v); }

        SamplerEngine::Params make()
        {
            SamplerEngine::Params p;
            auto& self = *this;
            p.playMode = self ("play_mode", 0);
            p.start = self ("smp_start", 0);        p.end = self ("smp_end", 100);
            p.loopStart = self ("loop_start", 20);  p.loopEnd = self ("loop_end", 80);
            p.loopXfade = self ("loop_xfade", 10);
            p.tune = self ("tune", 0);              p.fine = self ("fine", 0);
            p.grnPos = self ("grn_pos", 50);        p.grnSize = self ("grn_size", 80);
            p.grnDensity = self ("grn_density", 30); p.grnSpray = self ("grn_spray", 10);
            p.grnPitchRnd = self ("grn_pitchrnd", 0); p.grnFreeze = self ("grn_freeze", 0);
            p.attack = self ("env1_attack", 0.001f); p.decay = self ("env1_decay", 0.1f);
            p.sustain = self ("env1_sustain", 100); p.release = self ("env1_release", 0.01f);
            // Filter wide open and no modulation, so the sampler checks hear the raw samples
            p.fltType = self ("flt_type", 1);       p.fltCutoff = self ("flt_cutoff", 20000);
            p.fltReso = self ("flt_reso", 0);       p.fltDrive = self ("flt_drive", 0);
            p.fltEnvAmt = self ("flt_envamt", 0);
            p.env2 = { self ("env2_attack", 0.001f), self ("env2_decay", 0.1f), self ("env2_sustain", 100), self ("env2_release", 0.01f) };
            p.env3 = { self ("env3_attack", 0.001f), self ("env3_decay", 0.1f), self ("env3_sustain", 50), self ("env3_release", 0.01f) };
            for (int i = 0; i < 4; ++i)
            {
                const auto pre = "lfo" + juce::String (i + 1) + "_";
                auto q = [&] (const char* name, float v) { return self ((pre + name).toRawUTF8(), v); };
                p.lfo[(size_t) i] = { q ("shape", 0), q ("rate", 1), q ("depth", 0), q ("phase", 0), q ("fade", 0) };
            }
            return p;
        }
    };

    // One engine with an instrument loaded, plus helpers to play it
    struct Rig
    {
        FakeParams fp;
        SampleCache cache;
        std::unique_ptr<SamplerEngine> engine;
        juce::AudioBuffer<float> out { 2, 0 };
        juce::MidiBuffer pendingMidi;
        int missing = 0;

        explicit Rig (const juce::String& json)
        {
            engine = std::make_unique<SamplerEngine> (fp.make());
            engine->prepare (hostRate);
            load (json);
        }

        void load (const juce::String& json)
        {
            auto map = buildInstrumentMap (juce::JSON::parse (json), cache);
            missing = map->missingSamples;
            engine->publish (std::move (map));
        }

        void on (int note, int vel = 100)  { pendingMidi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) vel), 0); }
        void off (int note)                { pendingMidi.addEvent (juce::MidiMessage::noteOff (1, note), 0); }

        // Renders `seconds` of audio into `out` (replacing it); MIDI queued with on()/off() goes at the start
        void run (double seconds, bool playing = false)
        {
            const int total = (int) (seconds * hostRate);
            out.setSize (2, total);
            out.clear();
            for (int pos = 0; pos < total; pos += blockSize)
            {
                const int n = juce::jmin (blockSize, total - pos);
                juce::AudioBuffer<float> block (out.getArrayOfWritePointers(), 2, pos, n);
                engine->process (block, pendingMidi, playing);
                pendingMidi.clear();
            }
        }

        float rms (double from, double to, int ch = 0) const
        {
            const int a = (int) (from * hostRate), b = juce::jmin (out.getNumSamples(), (int) (to * hostRate));
            return b > a ? out.getRMSLevel (ch, a, b - a) : 0.0f;
        }

        // Frequency from rising zero crossings
        double hz (double from, double to) const
        {
            const int a = (int) (from * hostRate), b = juce::jmin (out.getNumSamples(), (int) (to * hostRate));
            const auto* d = out.getReadPointer (0);
            int first = -1, last = -1, count = 0;
            for (int i = a + 1; i < b; ++i)
                if (d[i - 1] < 0.0f && d[i] >= 0.0f)
                {
                    if (first < 0) first = i;
                    last = i;
                    ++count;
                }
            return count > 1 ? (count - 1) * hostRate / (last - first) : 0.0;
        }
    };

    bool near (double a, double b, double tolerance) { return std::abs (a - b) <= tolerance; }

    juce::String layer (const juce::String& fields) { return "{" + fields + "}"; }
    juce::String instrument (const juce::StringArray& layers, const juce::String& extra = {})
    {
        return "{\"version\":1" + extra + ",\"layers\":[" + layers.joinIntoString (",") + "]}";
    }
    juce::String path (const juce::File& f) { return juce::JSON::toString (f.getFullPathName()); }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;   // message manager for the format readers
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mtyaEngineTests");
    dir.deleteRecursively();
    dir.createDirectory();

    const auto a440 = sine (dir, "a440.wav", 440.0);
    const auto s200 = sine (dir, "s200.wav", 200.0), s300 = sine (dir, "s300.wav", 300.0), s400 = sine (dir, "s400.wav", 400.0);
    const auto s500 = sine (dir, "s500.wav", 500.0), s600 = sine (dir, "s600.wav", 600.0), s1000 = sine (dir, "s1000.wav", 1000.0);
    const auto shortSine = sine (dir, "short.wav", 300.0, 0.2);
    const auto base = "\"vlo\":0,\"vhi\":127,\"velToVol\":0,\"volume\":0,\"pan\":0";

    std::cout << "Pitch" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":69,") + base + ",\"samples\":[" + path (a440) + "]") }));
        rig.on (69); rig.run (0.3);
        check (near (rig.hz (0.05, 0.25), 440.0, 2.0), "root note plays at the sample's pitch (44.1k file, 48k host): " + juce::String (rig.hz (0.05, 0.25), 1) + " Hz");
        rig.off (69); rig.run (0.1);
        rig.on (81); rig.run (0.3);
        check (near (rig.hz (0.05, 0.25), 880.0, 4.0), "an octave up plays at 880 Hz: " + juce::String (rig.hz (0.05, 0.25), 1));
        rig.off (81); rig.run (0.1);
        rig.fp.set ("tune", -12); rig.on (69); rig.run (0.3);
        check (near (rig.hz (0.05, 0.25), 220.0, 2.0), "Tune -12 st halves the pitch: " + juce::String (rig.hz (0.05, 0.25), 1));
    }

    std::cout << "Key track" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":69,\"keyTrack\":0,") + base + ",\"samples\":[" + path (a440) + "]") }));
        rig.on (81); rig.run (0.3);
        check (near (rig.hz (0.05, 0.25), 440.0, 2.0), "key track 0 % keeps the pitch fixed");
    }

    std::cout << "Round robin" << std::endl;
    {
        const auto rr = [&] (const juce::String& mode)
        {
            return instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"rrMode\":\"") + mode + "\",\"rrReset\":\"idle\","
                                        + base + ",\"samples\":[" + path (s200) + "," + path (s300) + "," + path (s400) + "]") });
        };
        Rig rig (rr ("cycle"));
        juce::Array<int> heard;
        for (int i = 0; i < 4; ++i)
        {
            rig.on (60); rig.run (0.15); heard.add (juce::roundToInt (rig.hz (0.03, 0.14) / 100.0) * 100);
            rig.off (60); rig.run (0.05);
        }
        check (heard == juce::Array<int> { 200, 300, 400, 200 }, "cycle plays 1, 2, 3, 1: " + juce::String (heard[0]) + " " + juce::String (heard[1]) + " " + juce::String (heard[2]) + " " + juce::String (heard[3]));

        rig.run (2.5);   // idle reset after 2 s
        rig.on (60); rig.run (0.15);
        check (near (rig.hz (0.03, 0.14), 200, 10), "the counter resets after 2 s idle");
        rig.off (60); rig.run (0.05);

        Rig nr (rr ("norepeat"));
        int last = 0, repeats = 0;
        for (int i = 0; i < 30; ++i)
        {
            nr.on (60); nr.run (0.12);
            const int f = juce::roundToInt (nr.hz (0.03, 0.11) / 100.0) * 100;
            repeats += f == last ? 1 : 0;
            last = f;
            nr.off (60); nr.run (0.03);
        }
        check (repeats == 0, "no-repeat never plays the same variation twice in a row (30 notes)");
    }

    std::cout << "Velocity layers and crossfades" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"vlo\":0,\"vhi\":63,\"velToVol\":0,\"samples\":[") + path (s200) + "]"),
                               layer (juce::String ("\"id\":2,\"lo\":0,\"hi\":127,\"root\":60,\"vlo\":64,\"vhi\":127,\"velToVol\":0,\"samples\":[") + path (s500) + "]") }));
        rig.on (60, 30); rig.run (0.2);
        check (near (rig.hz (0.05, 0.19), 200, 5), "velocity 30 plays the soft layer");
        rig.off (60); rig.run (0.05);
        rig.on (60, 100); rig.run (0.2);
        check (near (rig.hz (0.05, 0.19), 500, 5), "velocity 100 plays the loud layer");
        rig.off (60); rig.run (0.05);

        Rig xf (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"vlo\":0,\"vhi\":63,\"velFade\":8,\"velToVol\":0,\"samples\":[") + path (s200) + "]"),
                              layer (juce::String ("\"id\":2,\"lo\":0,\"hi\":127,\"root\":60,\"vlo\":64,\"vhi\":127,\"velFade\":8,\"velToVol\":0,\"samples\":[") + path (s500) + "]") }));
        juce::Array<float> levels;
        for (int v : { 50, 60, 63, 64, 67, 80 })
        {
            xf.on (60, v); xf.run (0.2); levels.add (xf.rms (0.05, 0.19)); xf.off (60); xf.run (0.05);
        }
        float lo = 10, hi = 0;
        for (auto l : levels) { lo = juce::jmin (lo, l); hi = juce::jmax (hi, l); }
        check (hi / lo < 1.1f, "an equal-power velocity crossfade keeps the level steady (spread " + juce::String ((hi / lo - 1) * 100, 1) + " %)");
    }

    std::cout << "Key ranges, mute and solo" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":48,\"hi\":59,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]"),
                               layer (juce::String ("\"id\":2,\"lo\":60,\"hi\":71,\"root\":60,\"muted\":true,") + base + ",\"samples\":[" + path (s500) + "]") }));
        rig.on (40); rig.run (0.1);
        check (rig.rms (0, 0.1) < 1e-4f, "a note outside every range is silent");
        rig.on (65); rig.run (0.1);
        check (rig.rms (0, 0.1) < 1e-4f, "a muted layer is silent");
    }

    std::cout << "Trigger modes" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"trigger\":\"attack\",") + base + ",\"samples\":[" + path (s200) + "]"),
                               layer (juce::String ("\"id\":2,\"lo\":0,\"hi\":127,\"root\":60,\"trigger\":\"release\",\"releaseVolume\":0,\"releaseDecay\":0,") + base + ",\"samples\":[" + path (s1000) + "]") }));
        rig.on (60); rig.run (0.2);
        check (near (rig.hz (0.05, 0.19), 200, 5), "note on plays only the attack layer");
        rig.off (60); rig.run (0.2);
        check (near (rig.hz (0.05, 0.19), 1000, 10), "note off plays the release layer");

        Rig decay (instrument ({ layer (juce::String ("\"id\":2,\"lo\":0,\"hi\":127,\"root\":60,\"trigger\":\"release\",\"releaseVolume\":0,\"releaseDecay\":200,") + base + ",\"samples\":[" + path (s1000) + "]") }));
        decay.on (60); decay.run (0.05); decay.off (60); decay.run (0.1);
        const auto shortHold = decay.rms (0.01, 0.09);
        decay.run (1.0);   // let the first release sample (1 s) finish
        decay.on (60); decay.run (0.6); decay.off (60); decay.run (0.1);
        const auto longHold = decay.rms (0.01, 0.09);
        check (longHold < shortHold * 0.2f, "release samples get quieter the longer the key was held");

        Rig leg (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"trigger\":\"first\",") + base + ",\"samples\":[" + path (s200) + "]"),
                               layer (juce::String ("\"id\":2,\"lo\":0,\"hi\":127,\"root\":60,\"trigger\":\"legato\",") + base + ",\"samples\":[" + path (s500) + "]") }));
        leg.on (60); leg.run (0.2);
        check (near (leg.hz (0.05, 0.19), 200, 5) && leg.engine->getActiveVoices() == 1, "first-note layer plays when no other key is held");
        leg.on (60 + 12); leg.run (0.2);
        check (leg.engine->getActiveVoices() == 2, "legato layer joins when a key is already held");
    }

    std::cout << "Voices: polyphony, choke, mono, legato, glide" << std::endl;
    {
        Rig poly (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"polyphony\":2,\"steal\":\"oldest\",") + base + ",\"samples\":[" + path (s200) + "]") }));
        poly.on (60); poly.on (62); poly.on (64); poly.run (0.1);
        check (poly.engine->getActiveVoices() == 2, "polyphony 2 keeps two voices (stolen voice fades out)");

        Rig choke (instrument ({ layer (juce::String ("\"id\":1,\"lo\":42,\"hi\":42,\"root\":42,\"choke\":1,") + base + ",\"samples\":[" + path (s200) + "]"),
                                 layer (juce::String ("\"id\":2,\"lo\":46,\"hi\":46,\"root\":46,\"choke\":1,") + base + ",\"samples\":[" + path (s500) + "]") }));
        choke.on (46); choke.run (0.1);
        choke.on (42); choke.run (0.2);
        check (choke.engine->getActiveVoices() == 1 && near (choke.hz (0.05, 0.19), 200 * std::exp2 ((42 - 42) / 12.0), 5),
               "a closed hat chokes the open hat (same choke group)");

        Rig mono (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"voiceMode\":\"mono\",") + base + ",\"samples\":[" + path (s200) + "]") }));
        mono.on (60); mono.run (0.05); mono.on (67); mono.run (0.2);
        check (mono.engine->getActiveVoices() == 1 && near (mono.hz (0.05, 0.19), 200 * std::exp2 (7 / 12.0), 5), "mono: the new note replaces the old one");
        mono.off (67); mono.run (0.2);
        check (near (mono.hz (0.05, 0.19), 200, 5), "mono: releasing it goes back to the key still held");

        Rig legato (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"voiceMode\":\"legato\",\"glide\":100,") + base + ",\"samples\":[" + path (s200) + "]") }));
        legato.on (60); legato.run (0.1); legato.on (72); legato.run (0.4);
        const auto mid = legato.hz (0.01, 0.06), end = legato.hz (0.25, 0.39);
        check (legato.engine->getActiveVoices() == 1 && mid > 210 && mid < 390 && near (end, 400, 6),
               "legato + glide: one voice slides up an octave (" + juce::String (mid, 0) + " Hz mid-glide, " + juce::String (end, 0) + " Hz after)");
    }

    std::cout << "Keyswitches" << std::endl;
    {
        Rig ks (instrument ({ layer (juce::String ("\"id\":1,\"lo\":48,\"hi\":72,\"root\":60,\"keyswitch\":24,\"articulation\":\"Sustain\",") + base + ",\"samples\":[" + path (s200) + "]"),
                              layer (juce::String ("\"id\":2,\"lo\":48,\"hi\":72,\"root\":60,\"keyswitch\":25,\"articulation\":\"Staccato\",") + base + ",\"samples\":[" + path (s600) + "]") }));
        ks.on (60); ks.run (0.2);
        check (near (ks.hz (0.05, 0.19), 200, 5), "the lowest keyswitch is active at first");
        ks.off (60); ks.run (0.05);
        ks.on (25); ks.run (0.01);
        check (ks.engine->getActiveVoices() == 0, "a keyswitch note makes no sound");
        ks.off (25); ks.on (60); ks.run (0.2);
        check (near (ks.hz (0.05, 0.19), 600, 5), "after pressing the second keyswitch, its articulation plays");
    }

    std::cout << "Playback modes" << std::endl;
    {
        const auto inst = instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (shortSine) + "]") });

        Rig oneShot (inst);
        oneShot.on (60); oneShot.run (0.5);
        check (oneShot.rms (0.05, 0.15) > 0.1f && oneShot.rms (0.25, 0.5) < 1e-4f && oneShot.engine->getActiveVoices() == 0,
               "one-shot plays the sample once and stops at its end (0.2 s) while the key is held");

        Rig loop (inst);
        loop.fp.set ("play_mode", 1);
        loop.on (60); loop.run (1.0);
        check (loop.rms (0.8, 1.0) > 0.1f, "loop keeps playing past the end of a 0.2 s sample");
        check (near (loop.hz (0.3, 1.0), 300, 3), "and the loop holds the pitch (" + juce::String (loop.hz (0.3, 1.0), 1) + " Hz)");
        float maxStep = 0;
        for (int i = (int) (0.3 * hostRate); i < loop.out.getNumSamples(); ++i)
            maxStep = juce::jmax (maxStep, std::abs (loop.out.getSample (0, i) - loop.out.getSample (0, i - 1)));
        // A 300 Hz sine at 0.5 moves at most ~0.02 per sample at 48 kHz; a hard loop seam would jump much more
        check (maxStep < 0.03f, "the loop crossfade leaves no click at the seam (largest step " + juce::String (maxStep, 4) + ")");
        loop.off (60); loop.run (0.2);
        check (loop.rms (0.1, 0.2) < 1e-4f, "note off releases a looping voice");

        Rig pp (inst);
        pp.fp.set ("play_mode", 2);
        pp.on (60); pp.run (1.0);
        check (pp.rms (0.8, 1.0) > 0.1f, "ping-pong keeps playing past the end");

        // Slice: each sixteenth of the sample holds a different constant level
        const auto steps = writeWav (dir, "steps.wav", hostRate, 16 * 4800, [] (int i) { return (float) (i / 4800 + 1) / 20.0f; });
        Rig slice (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (steps) + "]") }));
        slice.fp.set ("play_mode", 3);
        slice.on (63); slice.run (0.12);
        const auto level = slice.out.getSample (0, (int) (0.05 * hostRate));
        check (near (level, 4.0 / 20.0, 0.01) && slice.rms (0.11, 0.12) < 1e-4f,
               "slice: root + 3 plays slice 4 at its own pitch, then stops (" + juce::String (level, 3) + ")");

        Rig grain (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s300) + "]") }));
        grain.fp.set ("play_mode", 4);
        grain.fp.set ("grn_freeze", 1);
        grain.on (60); grain.run (2.0);
        check (grain.rms (1.5, 2.0) > 0.05f && grain.engine->getActiveVoices() == 1, "granular + freeze keeps a cloud going on one voice");
        check (near (grain.hz (0.5, 2.0), 300, 15), "grains keep the note's pitch with no pitch randomness (" + juce::String (grain.hz (0.5, 2.0), 1) + " Hz)");
    }

    std::cout << "Filter" << std::endl;
    {
        const auto s4000 = sine (dir, "s4000.wav", 4000.0);
        const auto s2000 = sine (dir, "s2000.wav", 2000.0);
        const auto s750 = sine (dir, "s750.wav", 750.0);
        auto rigFor = [&] (const juce::File& f) { return std::make_unique<Rig> (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (f) + "]") })); };
        auto level = [&] (const juce::File& f, std::function<void (FakeParams&)> setup)
        {
            auto rig = rigFor (f);
            setup (rig->fp);
            rig->on (60); rig->run (0.3);
            return juce::Decibels::gainToDecibels (rig->rms (0.1, 0.3));
        };
        const auto open4k = level (s4000, [] (FakeParams&) {});
        const auto lp24 = level (s4000, [] (FakeParams& f) { f.set ("flt_type", 0); f.set ("flt_cutoff", 500); }) - open4k;
        const auto lp12 = level (s4000, [] (FakeParams& f) { f.set ("flt_type", 1); f.set ("flt_cutoff", 500); }) - open4k;
        // Theory says 72 / 36 dB; LP24 bottoms out near -60 dB because the Hermite resampling
        // (44.1 kHz file at 48 kHz) leaves faint images around that level.
        check (lp24 < -55 && lp12 < -30 && lp12 > -45 && lp24 < lp12 - 15,
               "LP24 / LP12 at 500 Hz cut 4 kHz hard / by about 36 dB (" + juce::String (lp24, 1) + " / " + juce::String (lp12, 1) + " dB)");

        const auto open200 = level (s200, [] (FakeParams&) {});
        const auto hp = level (s200, [] (FakeParams& f) { f.set ("flt_type", 3); f.set ("flt_cutoff", 4000); }) - open200;
        check (hp < -40, "HP at 4 kHz cuts 200 Hz (" + juce::String (hp, 1) + " dB)");

        const auto open2k = level (s2000, [] (FakeParams&) {});
        const auto bpPeak = level (s2000, [] (FakeParams& f) { f.set ("flt_type", 2); f.set ("flt_cutoff", 2000); f.set ("flt_reso", 50); }) - open2k;
        const auto bpOff = level (s200, [] (FakeParams& f) { f.set ("flt_type", 2); f.set ("flt_cutoff", 2000); f.set ("flt_reso", 50); }) - open200;
        check (std::abs (bpPeak) < 1.0f && bpOff < -30, "BP passes its centre at unity and cuts far below it (" + juce::String (bpPeak, 1) + " / " + juce::String (bpOff, 1) + " dB)");

        const auto shut = level (s2000, [] (FakeParams& f) { f.set ("flt_type", 0); f.set ("flt_cutoff", 200); }) - open2k;
        const auto envOpen = level (s2000, [] (FakeParams& f) { f.set ("flt_type", 0); f.set ("flt_cutoff", 200); f.set ("flt_envamt", 100); f.set ("env2_sustain", 100); }) - open2k;
        check (shut < -30 && envOpen > -1, "Env 2 at full Env Amt opens a 200 Hz LP24 (2 kHz: " + juce::String (shut, 1) + " dB → " + juce::String (envOpen, 1) + " dB)");

        const auto open500 = level (s500, [] (FakeParams&) {});
        const auto open750 = level (s750, [] (FakeParams&) {});
        const auto combPeak = level (s500, [] (FakeParams& f) { f.set ("flt_type", 4); f.set ("flt_cutoff", 500); f.set ("flt_reso", 100); }) - open500;
        const auto combNotch = level (s750, [] (FakeParams& f) { f.set ("flt_type", 4); f.set ("flt_cutoff", 500); f.set ("flt_reso", 100); }) - open750;
        check (std::abs (combPeak) < 1.5f && combNotch < -20, "comb at 500 Hz: peak at 500, notch at 750 (" + juce::String (combPeak, 1) + " / " + juce::String (combNotch, 1) + " dB)");

        auto crest = [&] (float driveDb)
        {
            auto rig = rigFor (s200);
            rig->fp.set ("flt_drive", driveDb);
            rig->on (60); rig->run (0.3);
            const int a = (int) (0.1 * hostRate);
            return rig->out.getMagnitude (0, a, (int) (0.2 * hostRate)) / rig->rms (0.1, 0.3);
        };
        check (crest (24) < crest (0) * 0.85f, "drive squares off the wave (crest factor " + juce::String (crest (0), 2) + " → " + juce::String (crest (24), 2) + ")");
    }

    std::cout << "LFOs and envelopes" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]") }));
        rig.fp.set ("lfo2_shape", 3);   // square: +1 for the first half cycle
        rig.fp.set ("lfo2_depth", 100);
        rig.fp.set ("lfo2_rate", 1);
        rig.on (60); rig.run (1.0);
        check (rig.rms (0.05, 0.45, 0) < 1e-3f && rig.rms (0.05, 0.45, 1) > 0.2f
                 && rig.rms (0.55, 0.95, 1) < 1e-3f && rig.rms (0.55, 0.95, 0) > 0.2f,
               "LFO 2 (square, 1 Hz) pans hard right, then hard left");
        rig.off (60); rig.run (0.1);

        rig.fp.set ("lfo2_fade", 2000);
        rig.on (60); rig.run (0.2);
        const auto l = rig.rms (0.02, 0.06, 0), r = rig.rms (0.02, 0.06, 1);
        check (l > 0.9f * r, "LFO fade-in: the pan barely moves in the first 60 ms of a 2 s fade");
    }

    std::cout << "Missing files and instrument edits" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":69,") + base + ",\"samples\":[\"no_such_file.wav\"]") }));
        check (rig.missing == 1, "a missing file is counted");
        rig.on (69); rig.run (0.3);
        check (near (rig.hz (0.05, 0.25), 440, 3), "and plays a stand-in tone at the root pitch");

        // Swap the instrument while a note plays: the old map is released only after its voice ends
        rig.load (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s500) + "]") }));
        rig.run (0.1);
        check (rig.engine->popRetired() == nullptr && rig.rms (0, 0.1) > 0.05f, "the old instrument stays alive while its note still sounds");
        rig.off (69); rig.run (0.2);
        check (rig.engine->popRetired() != nullptr, "and is handed back for deleting once the note ends");
        rig.on (60); rig.run (0.2);
        check (near (rig.hz (0.05, 0.19), 500, 5), "new notes use the new instrument");
    }

    std::cout << "Bare file names are found in a sampleFolder" << std::endl;
    {
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[\"s400.wav\"]") },
                             ",\"sampleFolder\":" + path (dir)));
        check (rig.missing == 0, "s400.wav is found next to the other test files");
    }

    dir.deleteRecursively();
    std::cout << (failures == 0 ? "All checks passed" : juce::String (failures) + " check(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
