// Offline tests for the sampler engine: builds instruments from JSON with generated WAV files,
// plays MIDI into SamplerEngine and checks what comes out (pitch, which sample played, level).
//
// Build and run:  cmake --build <build dir> --target mtyaEngineTests, then run the executable.
// It prints one line per check and exits with 1 if any failed.

#include "../Source/InstrumentMap.h"
#include "../Source/SamplerEngine.h"
#include "../Source/FxChain.h"
#include "../Source/SampleTools.h"
#include <juce_dsp/juce_dsp.h>
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

    juce::File sine (const juce::File& dir, const juce::String& name, double hz, double seconds = 1.0, double rate = 44100.0, float amplitude = 0.5f)
    {
        return writeWav (dir, name, rate, (int) (seconds * rate),
                         [=] (int i) { return amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / rate); });
    }

    // The real parameter list from ui/params.json, with our own storage instead of an APVTS
    juce::var loadSpec()
    {
        return juce::JSON::parse (juce::File (__FILE__).getParentDirectory().getSiblingFile ("ui").getChildFile ("params.json"));
    }

    struct FakeParams
    {
        std::map<juce::String, std::unique_ptr<std::atomic<float>>> values;
        juce::var spec = loadSpec();

        void set (const char* id, float v) { jassert (values.count (id) > 0); values[id]->store (v); }

        SamplerEngine::ParamList make()
        {
            auto list = SamplerEngine::makeParamList (spec, [this] (const juce::String& id)
            {
                auto& slot = values[id];
                slot = std::make_unique<std::atomic<float>> (0.0f);
                return slot.get();
            });
            for (const auto& p : *spec["params"].getArray())
                values[p["id"].toString()]->store ((float) (double) p["default"]);
            neutral();
            return list;
        }

        // Filter wide open, no modulation, fast envelopes, no effects: the sampler checks hear the raw samples
        void neutral()
        {
            const std::pair<const char*, float> v[] = {
                { "play_mode", 0 }, { "loop_xfade", 10 }, { "stretch_mode", 0 }, { "time_speed", 100 }, { "formant", 0 },
                { "tune", 0 }, { "fine", 0 }, { "grn_pos", 50 }, { "grn_size", 80 }, { "grn_density", 30 }, { "grn_spray", 10 },
                { "grn_pitchrnd", 0 }, { "grn_freeze", 0 },
                { "env1_attack", 0.001f }, { "env1_decay", 0.1f }, { "env1_sustain", 100 }, { "env1_release", 0.01f },
                { "env2_attack", 0.001f }, { "env2_decay", 0.1f }, { "env2_sustain", 100 }, { "env2_release", 0.01f },
                { "env3_attack", 0.001f }, { "env3_decay", 0.1f }, { "env3_sustain", 50 }, { "env3_release", 0.01f },
                { "flt_type", 1 }, { "flt_cutoff", 20000 }, { "flt_reso", 0 }, { "flt_drive", 0 }, { "flt_envamt", 0 },
                { "macro1", 0 }, { "macro2", 0 }, { "macro3", 0 }, { "macro4", 0 },
                { "fx_sat_on", 0 }, { "fx_crush_on", 0 }, { "fx_chorus_on", 0 }, { "fx_delay_on", 0 }, { "fx_reverb_on", 0 }, { "fx_tape_on", 0 } };
            for (const auto& [id, value] : v)
                set (id, value);
            for (int i = 1; i <= 4; ++i)
            {
                set (("lfo" + juce::String (i) + "_depth").toRawUTF8(), 0);
                set (("lfo" + juce::String (i) + "_rate").toRawUTF8(), 1);
                set (("lfo" + juce::String (i) + "_phase").toRawUTF8(), 0);
                set (("lfo" + juce::String (i) + "_fade").toRawUTF8(), 0);
                set (("lfo" + juce::String (i) + "_shape").toRawUTF8(), 0);
            }
        }
    };

    // One engine with an instrument loaded, plus helpers to play it
    struct Rig
    {
        FakeParams fp;
        SampleCache cache;
        std::unique_ptr<SamplerEngine> engine;
        std::unique_ptr<FxChain> fx;
        double bpm = 120;
        juce::AudioBuffer<float> out { 2, 0 };
        juce::MidiBuffer pendingMidi;
        int missing = 0;

        explicit Rig (const juce::String& json)
        {
            engine = std::make_unique<SamplerEngine> (fp.make());
            engine->prepare (hostRate);
            fx = std::make_unique<FxChain> (*engine);
            fx->prepare (hostRate, blockSize);
            load (json);
        }

        void load (const juce::String& json)
        {
            auto map = buildInstrumentMap (juce::JSON::parse (json), cache, engine->paramIds());
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
                fx->process (block, bpm);
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

        // Quiet tones for the clean response (loud ones make the resonance saturate, by design)
        const auto q2000 = sine (dir, "q2000.wav", 2000.0, 1.0, 44100.0, 0.1f), q200 = sine (dir, "q200.wav", 200.0, 1.0, 44100.0, 0.1f);
        const auto q500 = sine (dir, "q500.wav", 500.0, 1.0, 44100.0, 0.1f), q750 = sine (dir, "q750.wav", 750.0, 1.0, 44100.0, 0.1f);
        const auto openQ2k = level (q2000, [] (FakeParams&) {}), openQ200 = level (q200, [] (FakeParams&) {});
        const auto bpPeak = level (q2000, [] (FakeParams& f) { f.set ("flt_type", 2); f.set ("flt_cutoff", 2000); f.set ("flt_reso", 50); }) - openQ2k;
        const auto bpOff = level (q200, [] (FakeParams& f) { f.set ("flt_type", 2); f.set ("flt_cutoff", 2000); f.set ("flt_reso", 50); }) - openQ200;
        check (std::abs (bpPeak) < 1.0f && bpOff < -30, "BP passes its centre at unity and cuts far below it (" + juce::String (bpPeak, 1) + " / " + juce::String (bpOff, 1) + " dB)");

        const auto loudBp = level (s2000, [] (FakeParams& f) { f.set ("flt_type", 2); f.set ("flt_cutoff", 2000); f.set ("flt_reso", 50); }) - level (s2000, [] (FakeParams&) {});
        check (loudBp < bpPeak - 1.0f && loudBp > -12.0f, "a loud tone on a resonant peak compresses gently (" + juce::String (loudBp, 1) + " dB)");

        const auto open2k = level (s2000, [] (FakeParams&) {});

        const auto shut = level (s2000, [] (FakeParams& f) { f.set ("flt_type", 0); f.set ("flt_cutoff", 200); }) - open2k;
        const auto envOpen = level (s2000, [] (FakeParams& f) { f.set ("flt_type", 0); f.set ("flt_cutoff", 200); f.set ("flt_envamt", 100); f.set ("env2_sustain", 100); }) - open2k;
        check (shut < -30 && envOpen > -1, "Env 2 at full Env Amt opens a 200 Hz LP24 (2 kHz: " + juce::String (shut, 1) + " dB → " + juce::String (envOpen, 1) + " dB)");

        const auto open500 = level (q500, [] (FakeParams&) {});
        const auto open750 = level (q750, [] (FakeParams&) {});
        const auto combPeak = level (q500, [] (FakeParams& f) { f.set ("flt_type", 4); f.set ("flt_cutoff", 500); f.set ("flt_reso", 80); }) - open500;
        const auto combNotch = level (q750, [] (FakeParams& f) { f.set ("flt_type", 4); f.set ("flt_cutoff", 500); f.set ("flt_reso", 80); }) - open750;
        // Notch depth in theory: (1 - fb) / (1 + fb) = -19.1 dB at 80 %
        check (std::abs (combPeak) < 1.5f && combNotch < -17, "comb at 500 Hz: peak at 500, notch at 750 (" + juce::String (combPeak, 1) + " / " + juce::String (combNotch, 1) + " dB)");

        // Self-oscillation: a short click, then silence; at full resonance the filter keeps singing at its cutoff
        const auto click = writeWav (dir, "click.wav", hostRate, (int) hostRate * 2, [] (int i) { return i < 48 ? 0.5f : 0.0f; });
        for (auto [type, name] : { std::pair { 0, "LP24" }, { 1, "LP12" }, { 2, "BP" }, { 4, "Comb" } })
        {
            auto rig = rigFor (click);
            rig->fp.set ("flt_type", (float) type);
            rig->fp.set ("flt_cutoff", 440);
            rig->fp.set ("flt_reso", 100);
            rig->on (60); rig->run (1.5);
            const auto hz = rig->hz (1.0, 1.5);
            const auto amp = rig->out.getMagnitude (0, (int) (1.0 * hostRate), (int) (0.5 * hostRate));
            check (amp > 0.1f && amp < 1.6f && near (hz, 440, 440 * 0.06),
                   juce::String (name) + " self-oscillates at full resonance (" + juce::String (hz, 0) + " Hz, peak " + juce::String (amp, 2) + ")");
        }
        {
            auto rig = rigFor (click);
            rig->fp.set ("flt_type", 0);
            rig->fp.set ("flt_cutoff", 440);
            rig->fp.set ("flt_reso", 60);
            rig->on (60); rig->run (1.0);
            check (rig->rms (0.6, 1.0) < 1e-4f, "below about 90 % it rings out instead (LP24, 60 %)");
        }

        // Smooth sweeps: jumping the cutoff between blocks glides instead of stepping
        {
            auto rig = rigFor (s200);
            rig->fp.set ("flt_type", 0);
            rig->fp.set ("flt_reso", 50);
            rig->on (60);
            juce::AudioBuffer<float> all (2, (int) hostRate);
            for (int b = 0; b < (int) hostRate / blockSize; ++b)
            {
                rig->fp.set ("flt_cutoff", (b / 8) % 2 ? 300.0f : 3000.0f);   // a square wave on the cutoff
                rig->run ((double) blockSize / hostRate);
                for (int ch = 0; ch < 2; ++ch)
                    all.copyFrom (ch, b * blockSize, rig->out, ch, 0, blockSize);
            }
            float maxStep = 0;
            for (int i = (int) (0.05 * hostRate); i < all.getNumSamples() - blockSize; ++i)
                maxStep = juce::jmax (maxStep, std::abs (all.getSample (0, i) - all.getSample (0, i - 1)));
            check (maxStep < 0.08f, "cutoff jumps of 3 octaves don't click (largest step " + juce::String (maxStep, 3) + ")");
        }

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

    std::cout << "Modulation matrix" << std::endl;
    {
        const auto plain = juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]";
        auto withRoutes = [&] (const juce::String& routes) { return instrument ({ layer (plain) }, ",\"modRoutes\":" + routes); };

        Rig vib (withRoutes (R"([{ "source": "lfo1", "target": "pitch", "amount": 50 }])"));
        vib.fp.set ("lfo1_shape", 3);   // square
        vib.fp.set ("lfo1_depth", 100);
        vib.fp.set ("lfo1_rate", 2);
        vib.on (60); vib.run (0.5);
        check (near (vib.hz (0.03, 0.24), 400, 6) && near (vib.hz (0.28, 0.49), 100, 3),
               "LFO 1 -> Pitch 50 %: a square LFO jumps an octave up, then down (" + juce::String (vib.hz (0.03, 0.24), 0) + " / " + juce::String (vib.hz (0.28, 0.49), 0) + " Hz)");

        Rig none (withRoutes ("[]"));
        none.fp.set ("lfo2_shape", 3);
        none.fp.set ("lfo2_depth", 100);
        none.on (60); none.run (0.4);
        check (std::abs (none.rms (0.05, 0.4, 0) - none.rms (0.05, 0.4, 1)) < 1e-3f, "with no routes, LFO 2 no longer pans (the old fixed routing is gone)");

        Rig dflt (instrument ({ layer (plain) }));
        dflt.fp.set ("lfo2_shape", 3);
        dflt.fp.set ("lfo2_depth", 100);
        dflt.on (60); dflt.run (0.4);
        check (dflt.rms (0.05, 0.4, 0) < 1e-3f, "an instrument without \"modRoutes\" gets the default routing (LFO 2 -> Pan)");

        const auto s2000m = sine (dir, "m2000.wav", 2000.0);
        Rig macro (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s2000m) + "]") },
                              R"(,"modRoutes":[{ "source": "macro1", "target": "param:flt_cutoff", "amount": -100 }])"));
        macro.fp.set ("flt_type", 0);
        macro.on (60); macro.run (0.3);
        const auto openLevel = macro.rms (0.1, 0.3);
        macro.fp.set ("macro1", 100);
        macro.run (0.3);
        const auto closedLevel = macro.rms (0.1, 0.3);
        check (juce::Decibels::gainToDecibels (closedLevel / openLevel) < -40, "Macro 1 -> param:flt_cutoff -100 %: turning the macro up closes the filter ("
               + juce::String (juce::Decibels::gainToDecibels (closedLevel / openLevel), 0) + " dB)");
        check (macro.engine->param (macro.engine->indexOf ("flt_cutoff")) < 25.0f, "and param() reports the modulated cutoff");

        Rig vel (withRoutes (R"([{ "source": "velocity", "target": "volume", "amount": -50 }])"));
        vel.on (60, 1); vel.run (0.2);
        const auto soft = vel.rms (0.05, 0.2);
        vel.off (60); vel.run (0.1);
        vel.on (60, 127); vel.run (0.2);
        const auto hard = vel.rms (0.05, 0.2);
        check (near (hard / soft, 0.5, 0.03), "Velocity -> Volume -50 %: hard notes play at half level (" + juce::String (hard / soft, 2) + "×)");

        // Env 1 (amp) as a source: its attack sweeps the pitch up
        Rig e1 (withRoutes (R"([{ "source": "env1", "target": "pitch", "amount": 50 }])"));
        e1.fp.set ("env1_attack", 0.5f);
        e1.fp.set ("play_mode", 1);   // loop, so the sample (sped up by the sweep) keeps sounding
        e1.on (60); e1.run (1.0);
        check (e1.hz (0.02, 0.1) < 230 && near (e1.hz (0.7, 0.99), 400, 6), "Env 1 -> Pitch: the amp envelope's attack sweeps the pitch up (" + juce::String (e1.hz (0.02, 0.1), 0) + " -> " + juce::String (e1.hz (0.7, 0.99), 0) + " Hz)");

        // An LFO can move any knob: LFO 1 (square) -> the Tune knob, following the newest note
        Rig knobLfo (withRoutes (R"([{ "source": "lfo1", "target": "param:tune", "amount": 25 }])"));
        knobLfo.fp.set ("lfo1_shape", 3);
        knobLfo.fp.set ("lfo1_depth", 100);
        knobLfo.fp.set ("lfo1_rate", 2);
        knobLfo.on (60); knobLfo.run (0.5);
        const auto up = knobLfo.hz (0.05, 0.24), down = knobLfo.hz (0.3, 0.49);
        check (up > 260 && down < 160 && knobLfo.engine->param (knobLfo.engine->indexOf ("tune")) < -5.0f,
               "LFO 1 -> param:tune moves the Tune knob itself (" + juce::String (up, 0) + " / " + juce::String (down, 0) + " Hz)");

        Rig wheel (withRoutes (R"([{ "source": "modwheel", "target": "pitch", "amount": 50 }])"));
        wheel.on (60);
        wheel.pendingMidi.addEvent (juce::MidiMessage::controllerEvent (1, 1, 127), 0);
        wheel.run (0.3);
        check (near (wheel.hz (0.1, 0.29), 400, 6), "Mod wheel -> Pitch 50 %: full wheel = +12 st (" + juce::String (wheel.hz (0.1, 0.29), 0) + " Hz)");
    }

    std::cout << "FX chain" << std::endl;
    {
        const auto click = writeWav (dir, "fxclick.wav", hostRate, (int) hostRate, [] (int i) { return i < 96 ? 0.5f : 0.0f; });
        const auto inst = instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (click) + "]") });
        auto energy = [] (const Rig& r, double a, double b) { return r.rms (a, b); };

        Rig delay (inst);
        delay.fp.set ("fx_delay_on", 1);
        delay.fp.set ("fx_delay_time", 100);
        delay.fp.set ("fx_delay_fb", 0);
        delay.fp.set ("fx_delay_mix", 50);
        delay.fp.set ("fx_delay_pingpong", 0);
        delay.fp.set ("fx_delay_tone", 20000);
        delay.fx->reset();
        delay.on (60); delay.run (0.3);
        check (energy (delay, 0.05, 0.09) < 1e-4f && energy (delay, 0.098, 0.108) > 0.01f, "delay 100 ms: one echo at 100 ms and silence before it");

        Rig synced (inst);
        synced.fp.set ("fx_delay_on", 1);
        synced.fp.set ("fx_delay_sync", 1);
        synced.fp.set ("fx_delay_div", 8);   // 1/4
        synced.fp.set ("fx_delay_fb", 0);
        synced.fp.set ("fx_delay_mix", 50);
        synced.fp.set ("fx_delay_pingpong", 0);
        synced.fx->reset();
        synced.on (60); synced.run (0.7);
        check (energy (synced, 0.3, 0.49) < 1e-4f && energy (synced, 0.498, 0.51) > 0.01f, "synced 1/4 at 120 BPM echoes at 500 ms");

        Rig reverb (inst);
        reverb.fp.set ("fx_reverb_on", 1);
        reverb.fp.set ("fx_reverb_mix", 50);
        reverb.fx->reset();
        reverb.on (60); reverb.run (1.0);
        check (energy (reverb, 0.3, 0.6) > 1e-4f, "reverb leaves a tail after a click");

        Rig order (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (click) + "]") },
                              R"(,"fxOrder":["reverb","delay","chorus","crush","sat"])"));
        order.run (0.01);
        check ((order.engine->fxOrder() == std::array<int, 6> { 4, 3, 2, 1, 0, 5 }), "the FX order comes from the instrument (\"fxOrder\")");

        Rig partial (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (click) + "]") },
                                R"(,"fxOrder":["delay","bogus","delay"])"));
        partial.run (0.01);
        check ((partial.engine->fxOrder() == std::array<int, 6> { 3, 0, 1, 2, 4, 5 }), "a partial or broken order keeps every effect once");

        // Tape: wow makes the pitch wander, flutter adds a fast wobble, drive squares off the wave
        auto pitchSpread = [&] (float wow, float flutter)
        {
            Rig tape (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s1000) + "]") }));
            tape.fp.set ("fx_tape_on", 1);
            tape.fp.set ("fx_tape_wow", wow);
            tape.fp.set ("fx_tape_flutter", flutter);
            tape.fp.set ("fx_tape_drive", 0);
            tape.fp.set ("fx_tape_tone", 20000);
            tape.fx->reset();
            tape.on (60); tape.run (1.0);
            double lo = 1e9, hi = 0;
            for (double t = 0.1; t < 0.95; t += 0.05)
            {
                const auto f = tape.hz (t, t + 0.05);
                lo = juce::jmin (lo, f); hi = juce::jmax (hi, f);
            }
            return hi - lo;
        };
        const auto steady = pitchSpread (0, 0), wobbly = pitchSpread (100, 0), fluttery = pitchSpread (0, 100);
        check (steady < 3 && wobbly > 6 && fluttery > 4, "tape wow and flutter make the pitch wander (spread " + juce::String (steady, 1) + " / "
                   + juce::String (wobbly, 1) + " / " + juce::String (fluttery, 1) + " Hz)");

        Rig driven (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]") }));
        driven.fp.set ("fx_tape_on", 1);
        driven.fp.set ("fx_tape_wow", 0);
        driven.fp.set ("fx_tape_flutter", 0);
        driven.fp.set ("fx_tape_drive", 24);
        driven.fx->reset();
        driven.on (60); driven.run (0.4);
        const auto crestDriven = driven.out.getMagnitude (0, (int) (0.1 * hostRate), (int) (0.3 * hostRate)) / driven.rms (0.1, 0.4);
        // A clean sine is 1.41; tape's saturation is asymmetric (even harmonics), so it doesn't go fully square
        check (crestDriven < 1.32f, "tape drive saturates (crest factor 1.41 -> " + juce::String (crestDriven, 2) + ")");

        Rig crush (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]") }));
        crush.fp.set ("fx_crush_on", 1);
        crush.fp.set ("fx_crush_bits", 2);
        crush.fx->reset();
        crush.on (60); crush.run (0.3);
        juce::SortedSet<int> levels;
        for (int i = (int) (0.1 * hostRate); i < (int) (0.3 * hostRate); ++i)
            levels.add (juce::roundToInt (crush.out.getSample (0, i) * 1000));
        check (levels.size() <= 4, "a 2-bit crush leaves only a few output levels (" + juce::String (levels.size()) + ")");
    }

    std::cout << "Pitch / time / formant engines" << std::endl;
    {
        const auto inst = instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]") });
        // How long a note keeps sounding (one-shot), and its pitch early on
        auto measure = [&] (int mode, int note, float speed, double& seconds, double& hz)
        {
            Rig rig (inst);
            rig.fp.set ("stretch_mode", (float) mode);
            rig.fp.set ("time_speed", speed);
            rig.on (note); rig.run (2.6);
            hz = rig.hz (0.1, 0.3);
            seconds = 0;
            for (double t = 0; t < 2.6; t += 0.02)
                if (rig.rms (t, t + 0.02) > 0.02f) seconds = t + 0.02;
        };
        double secs, hz;
        measure (0, 72, 100, secs, hz);
        check (near (hz, 400, 6) && near (secs, 0.5, 0.06), "Resample: an octave up plays at 400 Hz and lasts half as long (" + juce::String (secs, 2) + " s)");
        measure (1, 72, 100, secs, hz);
        check (near (hz, 400, 6) && near (secs, 1.0, 0.08), "Stretch: an octave up plays at 400 Hz but keeps its length (" + juce::String (hz, 0) + " Hz, " + juce::String (secs, 2) + " s)");
        measure (1, 60, 50, secs, hz);
        check (near (hz, 200, 4) && near (secs, 2.0, 0.12), "Stretch: Speed 50 % doubles the length at the same pitch (" + juce::String (hz, 0) + " Hz, " + juce::String (secs, 2) + " s)");
        measure (2, 67, 100, secs, hz);
        check (near (hz, 200 * std::exp2 (7 / 12.0), 6) && near (secs, 1.0, 0.08), "Formant: a fifth up plays at the new pitch and keeps its length (" + juce::String (hz, 0) + " Hz, " + juce::String (secs, 2) + " s)");

        // Grains mustn't make the level flutter: 20 ms windows stay within a few percent of each other
        for (int mode : { 1, 2 })
        {
            Rig rig (inst);
            rig.fp.set ("stretch_mode", (float) mode);
            rig.on (65); rig.run (0.8);
            float lo = 10, hi = 0;
            for (double t = 0.1; t < 0.7; t += 0.02)
            {
                const auto level = rig.rms (t, t + 0.02);
                lo = juce::jmin (lo, level); hi = juce::jmax (hi, level);
            }
            check (hi / lo < 1.12f, juce::String (mode == 1 ? "Stretch" : "Formant") + ": the level stays steady (" + juce::String ((hi / lo - 1) * 100, 1) + " % spread)");
        }

        // A "vowel": a 150 Hz buzz through a resonance at 1 kHz. Shifting it up a fifth should move the
        // resonance with Resample, but keep it near 1 kHz with Formant.
        const auto vowel = writeWav (dir, "vowel.wav", hostRate, (int) hostRate, [] (int i)
        {
            static double y1 = 0, y2 = 0;
            if (i == 0) y1 = y2 = 0;
            const double rr = 0.995, theta = juce::MathConstants<double>::twoPi * 1000.0 / hostRate;
            const double x = (i % 320 == 0) ? 1.0 : 0.0;   // 150 Hz pulses
            const double y = x + 2 * rr * std::cos (theta) * y1 - rr * rr * y2;
            y2 = y1; y1 = y;
            return (float) (y * 0.02);
        });
        auto strongestHz = [&] (int mode)
        {
            Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (vowel) + "]") }));
            rig.fp.set ("stretch_mode", (float) mode);
            rig.on (67); rig.run (0.6);
            juce::dsp::FFT fft (14);
            std::vector<float> data (2 << 14, 0.0f);
            const int startAt = (int) (0.15 * hostRate);
            for (int i = 0; i < (1 << 14) && startAt + i < rig.out.getNumSamples(); ++i)
                data[(size_t) i] = rig.out.getSample (0, startAt + i) * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (1 << 14)));
            fft.performFrequencyOnlyForwardTransform (data.data());
            int best = 1;
            for (int b = 1; b < (1 << 13); ++b)
                if (data[(size_t) b] > data[(size_t) best]) best = b;
            return best * hostRate / (1 << 14);
        };
        const auto resampled = strongestHz (0), formant = strongestHz (2);
        check (resampled > 1300 && formant > 800 && formant < 1250,
               "Formant keeps the vowel's resonance near 1 kHz when the pitch goes up a fifth (Resample: " + juce::String (resampled, 0) + " Hz, Formant: " + juce::String (formant, 0) + " Hz)");
    }

    std::cout << "Playheads" << std::endl;
    {
        // Layers made in the UI get ids from Date.now(), far beyond 32 bits: they must come back unchanged
        Rig rig (instrument ({ layer (juce::String ("\"id\":1792419905776,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]") }));
        rig.on (60); rig.run (0.25);
        SamplerEngine::Playhead heads[SamplerEngine::maxPlayheads];
        const int n = rig.engine->getPlayheads (heads, SamplerEngine::maxPlayheads);
        // 0.25 s into a 1 s file at the same pitch
        check (n == 1 && heads[0].layerId == 1792419905776LL && heads[0].variation == 0 && near (heads[0].position, 0.25, 0.02),
               "the playing note's position and its layer's full id are reported (" + juce::String (n > 0 ? heads[0].position : -1.0f, 3) + ")");
        rig.off (60); rig.run (0.1);
        check (rig.engine->getPlayheads (heads, SamplerEngine::maxPlayheads) == 0, "and disappears when the note ends");
    }

    std::cout << "Outputs and normalize" << std::endl;
    {
        Rig bus (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"output\":\"Bus 2\",") + base + ",\"samples\":[" + path (s200) + "]") }));
        juce::AudioBuffer<float> main (2, blockSize), b2 (2, blockSize);
        juce::AudioBuffer<float>* aux[SamplerEngine::numAuxOutputs] { &b2, nullptr, nullptr };
        bus.on (60);
        float mainLevel = 0, auxLevel = 0;
        for (int i = 0; i < 20; ++i)
        {
            main.clear(); b2.clear();
            bus.engine->process (main, aux, bus.pendingMidi, false);
            bus.pendingMidi.clear();
            mainLevel = juce::jmax (mainLevel, main.getMagnitude (0, blockSize));
            auxLevel = juce::jmax (auxLevel, b2.getMagnitude (0, blockSize));
        }
        check (mainLevel < 1e-5f && auxLevel > 0.1f, "a layer routed to Bus 2 plays there when the host has Bus 2 on");
        bus.run (0.1);
        check (bus.rms (0.0, 0.1) > 0.1f, "and falls back to the main output when it's off");

        Rig norm (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"normalize\":true,") + base + ",\"samples\":[" + path (s200) + "]") }));
        Rig plainRig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base + ",\"samples\":[" + path (s200) + "]") }));
        norm.on (60); norm.run (0.2);
        plainRig.on (60); plainRig.run (0.2);
        check (near (norm.rms (0.05, 0.2) / plainRig.rms (0.05, 0.2), 2.0, 0.1), "normalize lifts a sample peaking at 0.5 by 6 dB (non-destructive)");
    }

    std::cout << "Sample tools" << std::endl;
    {
        const auto a = SampleTools::parseName ("Piano_C#3_mf_rr2.wav");
        check (a.note == 61 && a.dynamic == 4 && a.rr == 2, "parses Piano_C#3_mf_rr2.wav as C#3 (61), mf, rr2");
        const auto b = SampleTools::parseName ("My_Guitar_Ds2_f_rr1.wav");
        check (b.note == 51 && b.dynamic == 5 && b.rr == 1, "parses the Create page's names (Ds2 = D#2 = 51, f, rr1)");
        const auto c = SampleTools::parseName ("Bb1-v100.aif");
        check (c.note == 46 && c.dynamic == 200, "parses Bb1 and an explicit v100");
        check (SampleTools::parseName ("Kick_01.wav").note == -1 && SampleTools::parseName ("Bass_Long.wav").note == -1, "finds no note in Kick_01 or Bass_Long");

        const auto audio = SampleTools::read (s200);
        const auto hz = SampleTools::detectPitchHz (audio.buffer, audio.sampleRate);
        check (near (hz, 200, 1), "detects the pitch of a 200 Hz sine (" + juce::String (hz, 2) + " Hz)");
        const auto aSharp = SampleTools::read (sine (dir, "yin.wav", 466.16));
        check (near (69 + 12 * std::log2 (SampleTools::detectPitchHz (aSharp.buffer, aSharp.sampleRate) / 440.0), 70, 0.05), "and of A#3 (466 Hz)");

        const int z = SampleTools::nearestZeroCrossing (audio.buffer, 1000);
        const auto* wave = audio.buffer.getReadPointer (0);
        check (std::abs (wave[z]) < 0.01f || (wave[z - 1] < 0) != (wave[z] < 0), "finds a zero crossing near a position");

        auto inst = juce::JSON::parse (R"({ "layers": [
            { "id": 1, "group": "P", "samples": ["Piano_C2_mf.wav"], "root": 0, "lo": 0, "hi": 0, "vlo": 0, "vhi": 127 },
            { "id": 2, "group": "P", "samples": ["Piano_C4_mf.wav"], "root": 0, "lo": 0, "hi": 0, "vlo": 0, "vhi": 127 },
            { "id": 3, "group": "P", "samples": ["Piano_C3_mf.wav"], "root": 0, "lo": 0, "hi": 0, "vlo": 0, "vhi": 127 } ] })");
        SampleTools::autoMap (inst);
        auto L = [&inst] (int i, const char* k) { return (int) inst["layers"][i][k]; };
        check (L (0, "root") == 48 && L (2, "root") == 60 && L (1, "root") == 72
                 && L (0, "hi") + 1 == L (2, "lo") && L (2, "hi") + 1 == L (1, "lo"),
               "auto-map sets roots from names and joins the key ranges (" + juce::String (L (0, "lo")) + "–" + juce::String (L (0, "hi")) + ", "
                   + juce::String (L (2, "lo")) + "–" + juce::String (L (2, "hi")) + ", " + juce::String (L (1, "lo")) + "–" + juce::String (L (1, "hi")) + ")");

        auto gaps = juce::JSON::parse (R"({ "layers": [
            { "id": 1, "group": "G", "root": 38, "lo": 36, "hi": 40, "vlo": 0, "vhi": 127 },
            { "id": 2, "group": "G", "root": 52, "lo": 50, "hi": 55, "vlo": 0, "vhi": 127 } ] })");
        SampleTools::fillGaps (gaps);
        check ((int) gaps["layers"][0]["hi"] + 1 == (int) gaps["layers"][1]["lo"], "fill gaps closes the space between two zones");

        juce::Array<juce::File> files;
        for (auto n : { "Str_C3_pp.wav", "Str_C3_ff.wav", "Str_C3_ff_rr2.wav", "Str_E3_pp.wav", "Str_E3_ff.wav" })
            files.add (sine (dir, n, 200, 0.05));
        auto imported = juce::JSON::parse (R"({ "layers": [] })");
        const auto message = SampleTools::importFiles (imported, files, "Strings");
        const auto* ls = imported["layers"].getArray();
        int rrLayer = -1;
        for (int i = 0; i < ls->size(); ++i)
            if ((*ls)[i]["samples"].size() == 2) rrLayer = i;
        check (ls->size() == 4 && rrLayer >= 0 && (int) (*ls)[rrLayer]["vlo"] == 64 && (*ls)[rrLayer]["rrMode"].toString() == "cycle",
               "import makes one layer per note and dynamic, with round robins and velocity tiers (" + message + ")");
    }

    std::cout << "Per-sample start, end and loop points" << std::endl;
    {
        // Half a second of silence, then a 400 Hz tone: where each variation starts decides what you hear first
        const auto gapTone = writeWav (dir, "gaptone.wav", hostRate, (int) hostRate,
                                       [] (int i) { return i < 24000 ? 0.0f : 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 400.0 * i / hostRate); });
        const auto entry = [&] (double start) { return "{\"file\":" + path (gapTone) + ",\"start\":" + juce::String (start) + ",\"end\":1}"; };
        Rig rig (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,\"rrMode\":\"cycle\",") + base
                                      + ",\"samples\":[" + entry (0.5) + "," + entry (0.0) + "]") }));
        rig.on (60); rig.run (0.2);
        const auto first = rig.rms (0.01, 0.2);
        rig.off (60); rig.run (0.05);
        rig.on (60); rig.run (0.2);
        const auto second = rig.rms (0.01, 0.2);
        check (first > 0.2f && second < 1e-4f, "each round-robin variation has its own start (RR1 starts on the tone, RR2 in the silence)");

        // Loop points per sample: RR1 loops early in the file, plain-name entries get the defaults
        Rig loop (instrument ({ layer (juce::String ("\"id\":1,\"lo\":0,\"hi\":127,\"root\":60,") + base
                                       + ",\"samples\":[{\"file\":" + path (gapTone) + ",\"start\":0.5,\"end\":1,\"loopStart\":0.6,\"loopEnd\":0.7}]") }));
        loop.fp.set ("play_mode", 1);
        loop.on (60); loop.run (2.0);
        check (loop.rms (1.5, 2.0) > 0.2f && near (loop.hz (1.0, 2.0), 400, 4), "a sample's own loop points keep it sounding (loop inside the tone)");

        const auto level = SampleTools::read (gapTone);
        const auto r = SampleTools::findRegion (level.buffer, level.sampleRate);
        check (near (r.start, 0.5, 0.005) && r.end > 0.99f && r.loopStart > r.start && r.loopEnd > r.loopStart && r.loopEnd <= r.end,
               "auto-find puts the start where the sound begins (" + juce::String (r.start, 3) + ") and a loop inside it");
        const auto tail = SampleTools::read (writeWav (dir, "tail.wav", hostRate, (int) hostRate,
                                                  [] (int i) { return i > 4800 && i < 24000 ? 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 300.0 * i / hostRate) : 0.0f; }));
        const auto rt = SampleTools::findRegion (tail.buffer, tail.sampleRate);
        check (near (rt.start, 0.1, 0.005) && near (rt.end, 0.51, 0.02), "and the end where it stops (" + juce::String (rt.start, 3) + " → " + juce::String (rt.end, 3) + ")");
        const auto* data = tail.buffer.getReadPointer (0);
        const auto frames = (float) tail.buffer.getNumSamples();
        const int ls = (int) (rt.loopStart * frames), le = (int) (rt.loopEnd * frames);
        check (data[ls - 1] < 0 && data[ls] >= 0 && data[le - 1] < 0 && data[le] >= 0, "loop points land on rising zero crossings");
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
