#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParameterSpec.h"
#include "UiResources.h"

namespace
{
    // ---- PLACEHOLDER engine: a sine voice with Env 1 (Amp). Replace with the sampler. ----
    struct PlaceholderSound final : juce::SynthesiserSound
    {
        bool appliesToNote (int) override { return true; }
        bool appliesToChannel (int) override { return true; }
    };

    struct PlaceholderVoice final : juce::SynthesiserVoice
    {
        juce::ADSR adsr;
        juce::ADSR::Parameters adsrParams;
        double phase = 0.0, delta = 0.0;
        float level = 0.0f;

        bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<PlaceholderSound*> (s) != nullptr; }

        void startNote (int note, float velocity, juce::SynthesiserSound*, int) override
        {
            phase = 0.0;
            delta = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz (note) / getSampleRate();
            level = 0.15f * velocity;
            adsr.setSampleRate (getSampleRate());
            adsr.setParameters (adsrParams);
            adsr.noteOn();
        }

        void stopNote (float, bool allowTailOff) override
        {
            if (allowTailOff) adsr.noteOff();
            else { adsr.reset(); clearCurrentNote(); }
        }

        void pitchWheelMoved (int) override {}
        void controllerMoved (int, int) override {}

        void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
        {
            if (! isVoiceActive()) return;
            adsr.setParameters (adsrParams);
            for (int i = 0; i < num; ++i)
            {
                const auto s = (float) std::sin (phase) * level * adsr.getNextSample();
                phase += delta;
                for (int ch = 0; ch < out.getNumChannels(); ++ch)
                    out.addSample (ch, start + i, s);
                if (! adsr.isActive()) { clearCurrentNote(); break; }
            }
        }
    };
}

MtyaSamplerProcessor::MtyaSamplerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Record In", juce::AudioChannelSet::stereo(), true)   // used by the Create page
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      paramSpec (ParameterSpec::load()),
      apvts (*this, nullptr, "PARAMS", ParameterSpec::createLayout (paramSpec))
{
    masterVol = apvts.getRawParameterValue ("master_vol");
    instrumentJson = UiResources::getText ("demo-layers.json");

    for (int i = 0; i < 32; ++i)
        synth.addVoice (new PlaceholderVoice());
    synth.addSound (new PlaceholderSound());
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
    synth.setCurrentPlaybackSampleRate (sampleRate);
    uiMidi.reset (sampleRate);
    loadMeasurer.reset (sampleRate, samplesPerBlock);
    inputCopy.setSize (2, juce::jmax (1, samplesPerBlock) * 2);
    recorder.prepare (sampleRate, samplesPerBlock);
}

void MtyaSamplerProcessor::updateVoiceParameters()
{
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id)->load(); };
    juce::ADSR::Parameters p { raw ("env1_attack"), raw ("env1_decay"), raw ("env1_sustain") / 100.0f, raw ("env1_release") };
    for (int i = 0; i < synth.getNumVoices(); ++i)
        if (auto* v = dynamic_cast<PlaceholderVoice*> (synth.getVoice (i)))
            v->adsrParams = p;
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

    updateVoiceParameters();
    synth.renderNextBlock (buffer, midi, 0, buffer.getNumSamples());

    buffer.applyGain (masterVol->load() / 100.0f);

    // Guide track (prompts, tone, count-in) and optional input monitoring; not affected by Master.
    recorder.process (inputView, buffer, numSamples);

    int voices = 0;
    for (int i = 0; i < synth.getNumVoices(); ++i)
        voices += synth.getVoice (i)->isVoiceActive() ? 1 : 0;
    activeVoices.store (voices);

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
    // TODO: parse into the engine's zone map (off the audio thread) and swap it in atomically.
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
