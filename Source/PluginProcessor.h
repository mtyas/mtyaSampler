#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "SampleRecorder.h"
#include "SamplerEngine.h"
#include "SampleLoader.h"

// mtyaSampler audio processor.
//
// Two kinds of state:
//  * Automatable parameters: built from ui/params.json into `apvts`. Hosts see and automate these.
//  * The instrument: layers, mapping, round robin etc. Not automatable. Kept as JSON (schema in
//    docs/ui-spec.md), edited by the Layers page, saved with the plugin state.
//
// The audio engine is SamplerEngine (voices, round robin, trigger and playback modes); its
// instrument maps are built from the JSON by SampleLoader on a background thread.
// Filter, LFOs, FX and macros are still to come. See CLAUDE.md, Roadmap.
class MtyaSamplerProcessor final : public juce::AudioProcessor,
                                   public juce::ChangeBroadcaster
{
public:
    MtyaSamplerProcessor();
    ~MtyaSamplerProcessor() override = default;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Parameters (order matters: the spec must exist before apvts is built)
    const juce::var paramSpec;
    juce::AudioProcessorValueTreeState apvts;

    // Instrument JSON (layers + global settings). Thread-safe.
    juce::String getInstrumentJson() const;
    // notifyUi = false when the change came from the UI itself.
    void setInstrumentJson (const juce::String& json, bool notifyUi);

    // Notes and wheels played on the on-screen keyboard (message thread → audio thread).
    juce::MidiMessageCollector uiMidi;

    // Create page: the sample-recording assistant. It records from the plugin's audio input.
    SampleRecorder recorder;

    // The sampler and the thread that loads its samples (declared after apvts: they read its parameters)
    SamplerEngine engine;
    SampleLoader loader;

    // Read by the editor's timer for meters/stats.
    std::atomic<float> meterL { 0.0f }, meterR { 0.0f };
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> cpuLoad { 0.0f };

private:
    mutable juce::CriticalSection instrumentLock;
    juce::String instrumentJson;

    juce::AudioBuffer<float> inputCopy;   // input saved before the buffer is cleared
    juce::AudioProcessLoadMeasurer loadMeasurer;
    std::atomic<float>* masterVol = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MtyaSamplerProcessor)
};
