// Offline checks of the whole processor (engine + FX + buses + state), using the real plugin code.
// Build and run: cmake --build <build dir> --target mtyaPluginTests, then run the executable.

#include "../Source/PluginProcessor.h"
#include <iostream>

namespace
{
    int failures = 0;
    void check (bool ok, const juce::String& what)
    {
        std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
        if (! ok) ++failures;
    }

    // Runs `seconds` of audio with a note held from the start; returns the main output's peak
    float play (MtyaSamplerProcessor& p, juce::AudioBuffer<float>& buffer, double seconds, bool withNote = true)
    {
        juce::MidiBuffer midi;
        if (withNote)
            midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
        float peak = 0;
        for (int done = 0; done < (int) (seconds * 48000); done += 256)
        {
            buffer.clear();
            p.processBlock (buffer, midi);
            midi.clear();
            peak = juce::jmax (peak, buffer.getMagnitude (0, 0, 256));
        }
        return peak;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;
    std::unique_ptr<juce::AudioProcessor> plugin (createPluginFilter());
    auto& p = dynamic_cast<MtyaSamplerProcessor&> (*plugin);

    std::cout << "Processor" << std::endl;
    check (p.getBusCount (false) == 4 && ! p.getBus (false, 1)->isEnabled(), "main output plus Bus 2-4, which start switched off");

    // Switch on Bus 2 as a host would
    auto layout = p.getBusesLayout();
    layout.getChannelSet (false, 1) = juce::AudioChannelSet::stereo();
    check (p.setBusesLayout (layout), "a host can switch Bus 2 on");

    p.prepareToPlay (48000, 256);
    juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), 256);
    juce::Thread::sleep (300);   // the loader thread builds the demo instrument

    const auto peak = play (p, buffer, 0.5);
    check (peak > 0.01f && std::isfinite (peak), "the demo instrument plays through engine, FX and master (peak " + juce::String (peak, 3) + ")");

    // Route every layer to Bus 2: the main output goes quiet apart from effect tails
    auto inst = juce::JSON::parse (p.getInstrumentJson());
    for (auto& l : *inst["layers"].getArray())
        l.getDynamicObject()->setProperty ("output", "Bus 2");
    for (auto id : { "fx_reverb_on", "fx_delay_on", "fx_chorus_on" })
        p.apvts.getParameter (id)->setValueNotifyingHost (0.0f);
    p.setInstrumentJson (juce::JSON::toString (inst), false);
    juce::Thread::sleep (300);
    {
        juce::MidiBuffer off;
        off.addEvent (juce::MidiMessage::allNotesOff (1), 0);
        buffer.clear();
        p.processBlock (buffer, off);
    }
    play (p, buffer, 2.0, false);   // let the released notes and tails die away
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    float mainPeak = 0, busPeak = 0;
    for (int i = 0; i < 40; ++i)
    {
        buffer.clear();
        p.processBlock (buffer, midi);
        midi.clear();
        auto main = p.getBusBuffer (buffer, false, 0), bus2 = p.getBusBuffer (buffer, false, 1);
        mainPeak = juce::jmax (mainPeak, main.getMagnitude (0, 256));
        busPeak = juce::jmax (busPeak, bus2.getMagnitude (0, 256));
    }
    check (busPeak > 0.01f && mainPeak < busPeak * 0.05f, "layers set to Bus 2 come out of Bus 2 (" + juce::String (busPeak, 3) + " vs main " + juce::String (mainPeak, 4) + ")");

    // State round trip: parameters, instrument (with routes and FX order) and the window size
    p.apvts.getParameter ("flt_cutoff")->setValueNotifyingHost (0.25f);
    inst.getDynamicObject()->setProperty ("fxOrder", juce::Array<juce::var> { "reverb", "sat", "crush", "chorus", "delay" });
    p.setInstrumentJson (juce::JSON::toString (inst), false);
    p.editorWidth = 1000;
    p.editorHeight = 719;
    juce::MemoryBlock state;
    p.getStateInformation (state);

    std::unique_ptr<juce::AudioProcessor> other (createPluginFilter());
    auto& q = dynamic_cast<MtyaSamplerProcessor&> (*other);
    q.setStateInformation (state.getData(), (int) state.getSize());
    const auto restored = juce::JSON::parse (q.getInstrumentJson());
    check (std::abs (q.apvts.getParameter ("flt_cutoff")->getValue() - 0.25f) < 1e-4f, "saved state restores parameters");
    check (restored["fxOrder"][0].toString() == "reverb" && restored["layers"][0]["output"].toString() == "Bus 2", "and the instrument with its FX order");
    check (q.editorWidth == 1000 && q.editorHeight == 719, "and the window size");

    std::cout << "Create page: MIDI auto-sampling" << std::endl;
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mtyaMidiTest");
        auto planFor = [&] (double holdSec)
        {
            return juce::JSON::parse (R"({ "settings": { "midi": true, "midiChannel": 2, "holdSec": )" + juce::String (holdSec) + R"(,
                "name": "MidiTest", "folder": )" + juce::JSON::toString (dir.getFullPathName()) + R"(, "thresholdDb": -42, "silenceMs": 300,
                "prerollMs": 20, "fadeMs": 20, "normalize": "off", "autoAdvance": true, "pitched": true, "input": "stereo", "voice": false },
              "timing": { "announceSec": 0.2, "toneSec": 1.5, "beatSec": 0.5, "countBeats": 3, "maxSec": 2 },
              "takes": [ { "note": 62, "rr": 0, "v": 0, "velocity": 90, "file": "t1.wav", "say": "" } ] })");
        };
        // Runs `seconds`, returning every MIDI event the plugin sent with its time in seconds
        auto runCollect = [&] (double seconds, std::vector<std::pair<double, juce::MidiMessage>>& events, double& clock)
        {
            for (int done = 0; done < (int) (seconds * 48000); done += 256)
            {
                juce::MidiBuffer m;
                buffer.clear();
                p.processBlock (buffer, m);
                for (const auto meta : m)
                    events.push_back ({ clock + meta.samplePosition / 48000.0, meta.getMessage() });
                clock += 256 / 48000.0;
            }
        };

        std::vector<std::pair<double, juce::MidiMessage>> events;
        double clock = 0;
        check ((bool) p.recorder.command ("start", planFor (0.5))["ok"], "a MIDI session starts");
        runCollect (1.2, events, clock);
        const auto on = std::find_if (events.begin(), events.end(), [] (auto& e) { return e.second.isNoteOn(); });
        const auto off = std::find_if (events.begin(), events.end(), [] (auto& e) { return e.second.isNoteOff(); });
        check (on != events.end() && on->second.getNoteNumber() == 62 && on->second.getChannel() == 2 && on->second.getVelocity() == 90,
               "it sends the take's note on channel 2 at velocity 90");
        check (on != events.end() && on->first > 0.15 && on->first < 0.3,
               "after the gap, with no tone or count-in (" + juce::String (on != events.end() ? on->first : -1.0, 2) + " s)");
        check (off != events.end() && on != events.end() && std::abs ((off->first - on->first) - 0.5) < 0.01,
               "and releases it after the hold time (" + juce::String (off != events.end() && on != events.end() ? off->first - on->first : -1.0, 3) + " s)");
        p.recorder.command ("stop", {});
        runCollect (0.1, events, clock);

        // Stopping while the note is held must release it
        events.clear();
        clock = 0;
        p.recorder.command ("start", planFor (5.0));
        runCollect (0.6, events, clock);
        p.recorder.command ("stop", {});
        runCollect (0.1, events, clock);
        const int ons = (int) std::count_if (events.begin(), events.end(), [] (auto& e) { return e.second.isNoteOn(); });
        const int offs = (int) std::count_if (events.begin(), events.end(), [] (auto& e) { return e.second.isNoteOff(); });
        check (ons == 1 && offs == 1, "stopping mid-note sends the note-off (no hanging notes)");

        // Normal sessions send nothing
        events.clear();
        auto quiet = planFor (0.5);
        quiet["settings"].getDynamicObject()->setProperty ("midi", false);
        p.recorder.command ("start", quiet);
        runCollect (1.5, events, clock);
        p.recorder.command ("stop", {});
        check (events.empty(), "a session without MIDI mode sends no MIDI");
        check (p.producesMidi(), "the plugin declares a MIDI output");
        dir.deleteRecursively();
    }

    std::cout << "Clean Init / new instrument" << std::endl;
    {
        const auto r = p.presets.init ("My Synth");
        const auto fresh = juce::JSON::parse (p.getInstrumentJson());
        auto isOn = [&p] (const char* id) { return p.apvts.getRawParameterValue (id)->load() >= 0.5f; };
        check ((bool) r["ok"] && p.presets.name == "My Synth" && p.presets.path.isEmpty(), "a new instrument is named after the session and isn't saved yet");
        check (! isOn ("fx_sat_on") && ! isOn ("fx_crush_on") && ! isOn ("fx_chorus_on") && ! isOn ("fx_delay_on") && ! isOn ("fx_reverb_on"), "it has no effects on");
        check (fresh["layers"].size() == 0 && fresh["modRoutes"].isArray() && fresh["modRoutes"].size() == 0, "no layers and no modulation routes");
        check (p.apvts.getRawParameterValue ("flt_cutoff")->load() > 19000.0f && p.apvts.getRawParameterValue ("lfo1_depth")->load() == 0.0f
                 && p.apvts.getRawParameterValue ("macro1")->load() == 0.0f, "filter open, LFOs and macros at zero");

        // The Create page passes its planned layers so the new instrument is set in one step
        p.presets.init ("Session", R"({"version":1,"modRoutes":[],"layers":[{"id":7,"name":"Session C3","lo":60,"hi":62,"root":60,"samples":[""]}]})");
        const auto planned = juce::JSON::parse (p.getInstrumentJson());
        check (planned["layers"].size() == 1 && (int) planned["layers"][0]["id"] == 7 && p.presets.name == "Session",
               "a new instrument can start with the session's planned layers already in it");
    }

    p.releaseResources();
    std::cout << (failures == 0 ? "All checks passed" : juce::String (failures) + " check(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
