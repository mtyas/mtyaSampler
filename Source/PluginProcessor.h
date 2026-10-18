#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_basics/juce_audio_basics.h>

// mtyaSampler audio processor.
//
// Two kinds of state:
//  * Automatable parameters: built from ui/params.json into `apvts`. Hosts see and automate these.
//  * The instrument: layers, mapping, round robin etc. Not automatable. Kept as JSON (schema in
//    docs/ui-spec.md), edited by the Layers page, saved with the plugin state.
//
// The audio engine is a PLACEHOLDER (a sine voice shaped by Env 1) so the whole pipeline
// (host → MIDI → voices → UI) can be tested before the sampler engine exists. See CLAUDE.md, Roadmap.
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

    // Read by the editor's timer for meters/stats.
    std::atomic<float> meterL { 0.0f }, meterR { 0.0f };
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> cpuLoad { 0.0f };

private:
    void updateVoiceParameters();

    mutable juce::CriticalSection instrumentLock;
    juce::String instrumentJson;

    juce::Synthesiser synth;   // TODO: replace placeholder voices with the sampler engine
    juce::AudioProcessLoadMeasurer loadMeasurer;
    std::atomic<float>* masterVol = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MtyaSamplerProcessor)
};
