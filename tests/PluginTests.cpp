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

    p.releaseResources();
    std::cout << (failures == 0 ? "All checks passed" : juce::String (failures) + " check(s) failed") << std::endl;
    return failures == 0 ? 0 : 1;
}
