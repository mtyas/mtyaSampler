#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParameterSpec.h"
#include "UiResources.h"

MtyaSamplerProcessor::MtyaSamplerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Record In", juce::AudioChannelSet::stereo(), true)   // used by the Create page
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      paramSpec (ParameterSpec::load()),
      apvts (*this, nullptr, "PARAMS", ParameterSpec::createLayout (paramSpec)),
      engine (SamplerEngine::Params::fromApvts (apvts)),
      loader (engine)
{
    masterVol = apvts.getRawParameterValue ("master_vol");
    setInstrumentJson (UiResources::getText ("demo-layers.json"), false);
}

bool MtyaSamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Stereo out. The input is optional: stereo, mono or switched off by the host.
    const auto in = layouts.getMainInputChannelSet();
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && (in.isDisabled() || in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void MtyaSamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    uiMidi.reset (sampleRate);
    loadMeasurer.reset (sampleRate, samplesPerBlock);
    inputCopy.setSize (2, juce::jmax (1, samplesPerBlock) * 2);
    recorder.prepare (sampleRate, samplesPerBlock);
}

void MtyaSamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    juce::AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasurer, buffer.getNumSamples());

    // Keep the audio input for the recorder, then clear: an instrument starts from silence.
    const int numSamples = buffer.getNumSamples();
    const int inChannels = juce::jmin (getTotalNumInputChannels(), 2);
    const bool inputFits = numSamples <= inputCopy.getNumSamples();   // hosts may exceed the announced block size
    juce::AudioBuffer<float> inputView (inputCopy.getArrayOfWritePointers(), inputFits ? inChannels : 0, inputFits ? numSamples : 0);
    for (int ch = 0; ch < inputView.getNumChannels(); ++ch)
        inputView.copyFrom (ch, 0, buffer, ch, 0, numSamples);

    buffer.clear();
    uiMidi.removeNextBlockOfMessages (midi, buffer.getNumSamples());

    bool playing = false;
    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
            playing = position->getIsPlaying();
    engine.process (buffer, midi, playing);

    buffer.applyGain (masterVol->load() / 100.0f);

    // Guide track (prompts, tone, count-in) and optional input monitoring; not affected by Master.
    recorder.process (inputView, buffer, numSamples);

    activeVoices.store (engine.getActiveVoices());

    // Simple peak meters with fall-off, read by the editor
    const auto n = buffer.getNumSamples();
    meterL.store (juce::jmax (buffer.getMagnitude (0, 0, n), meterL.load() * 0.9f));
    meterR.store (juce::jmax (buffer.getMagnitude (buffer.getNumChannels() > 1 ? 1 : 0, 0, n), meterR.load() * 0.9f));
    cpuLoad.store ((float) loadMeasurer.getLoadAsProportion());
}

juce::AudioProcessorEditor* MtyaSamplerProcessor::createEditor()
{
    return new MtyaSamplerEditor (*this);
}

// ---------------- instrument JSON ----------------
juce::String MtyaSamplerProcessor::getInstrumentJson() const
{
    const juce::ScopedLock sl (instrumentLock);
    return instrumentJson;
}

void MtyaSamplerProcessor::setInstrumentJson (const juce::String& json, bool notifyUi)
{
    {
        const juce::ScopedLock sl (instrumentLock);
        instrumentJson = json;
    }
    loader.load (json);   // builds the zone map and loads samples off the audio thread
    if (notifyUi)
        sendChangeMessage();
}

// ---------------- state ----------------
void MtyaSamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("instrument", getInstrumentJson(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MtyaSamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto state = juce::ValueTree::fromXml (*xml);
        if (state.hasType (apvts.state.getType()))
        {
            const auto inst = state.getProperty ("instrument").toString();
            state.removeProperty ("instrument", nullptr);
            apvts.replaceState (state);
            if (inst.isNotEmpty())
                setInstrumentJson (inst, true);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MtyaSamplerProcessor();
}
