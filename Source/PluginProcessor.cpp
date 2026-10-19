#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ParameterSpec.h"
#include "UiResources.h"

MtyaSamplerProcessor::MtyaSamplerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Record In", juce::AudioChannelSet::stereo(), true)   // used by the Create page
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Bus 2", juce::AudioChannelSet::stereo(), false)   // layers can be routed here (multi-out)
                          .withOutput ("Bus 3", juce::AudioChannelSet::stereo(), false)
                          .withOutput ("Bus 4", juce::AudioChannelSet::stereo(), false)),
      paramSpec (ParameterSpec::load()),
      apvts (*this, nullptr, "PARAMS", ParameterSpec::createLayout (paramSpec)),
      engine (SamplerEngine::makeParamList (paramSpec, [this] (const juce::String& id) { return apvts.getRawParameterValue (id); })),
      fx (engine),
      loader (engine)
{
    masterVol = apvts.getRawParameterValue ("master_vol");
    // After session normalize rewrites the takes, rebuild the instrument so the new audio is used
    recorder.onFilesRewritten = [this] { setInstrumentJson (getInstrumentJson(), false); };
    setInstrumentJson (UiResources::getText ("demo-layers.json"), false);
}

bool MtyaSamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Stereo out, plus optional stereo Bus 2–4. The input is optional: stereo, mono or switched off by the host.
    const auto in = layouts.getMainInputChannelSet();
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()
        || ! (in.isDisabled() || in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo()))
        return false;
    for (int i = 1; i < layouts.outputBuses.size(); ++i)
    {
        const auto& bus = layouts.outputBuses.getReference (i);
        if (! bus.isDisabled() && bus != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

void MtyaSamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    fx.prepare (sampleRate, samplesPerBlock);
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

    // Notes for "Learn key range"
    for (const auto meta : midi)
        if (meta.getMessage().isNoteOn())
            learnFifo.write (1).forEach ([&] (int i) { learnNotes[(size_t) i] = meta.getMessage().getNoteNumber(); });

    bool playing = false;
    double bpm = 0;
    if (auto* head = getPlayHead())
        if (auto position = head->getPosition())
        {
            playing = position->getIsPlaying();
            bpm = position->getBpm().orFallback (0.0);
        }

    // Main output and the optional Bus 2–4 (only when the host has them switched on)
    auto main = getBusBuffer (buffer, false, 0);
    juce::AudioBuffer<float> auxBuffers[SamplerEngine::numAuxOutputs];
    juce::AudioBuffer<float>* aux[SamplerEngine::numAuxOutputs] {};
    for (int i = 0; i < SamplerEngine::numAuxOutputs; ++i)
        if (i + 1 < getBusCount (false) && getBus (false, i + 1)->isEnabled() && getChannelCountOfBus (false, i + 1) > 0)
        {
            auxBuffers[i] = getBusBuffer (buffer, false, i + 1);
            aux[i] = &auxBuffers[i];
        }

    engine.process (main, aux, midi, playing);
    fx.process (main, bpm);
    clipPlayer.process (main, getSampleRate());

    main.applyGain (masterVol->load() / 100.0f);

    // The plugin's MIDI output carries only the Create page's auto-sampling notes (not the input)
    midi.clear();

    // Guide track (prompts, tone, count-in) and optional input monitoring; not affected by Master.
    recorder.process (inputView, main, midi, numSamples);

    activeVoices.store (engine.getActiveVoices());

    // Simple peak meters with fall-off, read by the editor
    const auto n = main.getNumSamples();
    meterL.store (juce::jmax (main.getMagnitude (0, 0, n), meterL.load() * 0.9f));
    meterR.store (juce::jmax (main.getMagnitude (main.getNumChannels() > 1 ? 1 : 0, 0, n), meterR.load() * 0.9f));
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
    state.setProperty ("presetName", presets.name, nullptr);
    state.setProperty ("presetCategory", presets.category, nullptr);
    state.setProperty ("presetPath", presets.path, nullptr);
    state.setProperty ("editorWidth", editorWidth.load(), nullptr);
    state.setProperty ("editorHeight", editorHeight.load(), nullptr);
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
            presets.name = state.getProperty ("presetName", "Init").toString();
            presets.category = state.getProperty ("presetCategory").toString();
            presets.path = state.getProperty ("presetPath").toString();
            editorWidth = (int) state.getProperty ("editorWidth", 0);
            editorHeight = (int) state.getProperty ("editorHeight", 0);
            for (auto id : { "instrument", "presetName", "presetCategory", "presetPath", "editorWidth", "editorHeight" })
                state.removeProperty (id, nullptr);
            apvts.replaceState (state);
            if (presets.onChange)
                presets.onChange();
            if (inst.isNotEmpty())
                setInstrumentJson (inst, true);
        }
    }
}

bool MtyaSamplerProcessor::popLearnedNote (int& note)
{
    bool got = false;
    learnFifo.read (1).forEach ([&] (int i) { note = learnNotes[(size_t) i]; got = true; });
    return got;
}

// ---------------- clip player ----------------
MtyaSamplerProcessor::ClipPlayer::~ClipPlayer()
{
    delete pending.exchange (nullptr);
    delete retired.exchange (nullptr);
    delete current;
}

void MtyaSamplerProcessor::ClipPlayer::play (juce::AudioBuffer<float>&& audio, double sampleRate)
{
    auto* clip = new Clip { std::move (audio), sampleRate };
    stopRequest = false;
    delete pending.exchange (clip);   // a clip still pending was never played
}

void MtyaSamplerProcessor::ClipPlayer::collectGarbage()
{
    delete retired.exchange (nullptr);
}

void MtyaSamplerProcessor::ClipPlayer::process (juce::AudioBuffer<float>& out, double hostRate) noexcept
{
    auto retire = [this]
    {
        // Hand the finished clip back for deleting; if the slot is busy, keep it one more block.
        Clip* expected = nullptr;
        if (current != nullptr && retired.compare_exchange_strong (expected, current))
            current = nullptr;
    };

    if (stopRequest.exchange (false))
        retire();
    if (pending.load() != nullptr && current == nullptr)
    {
        current = pending.exchange (nullptr);
        pos = 0;
    }
    else if (pending.load() != nullptr)
        retire();

    playing = current != nullptr;
    if (current == nullptr)
        return;

    const auto& a = current->audio;
    const int len = a.getNumSamples();
    const auto inc = current->rate / juce::jmax (1.0, hostRate);
    for (int i = 0; i < out.getNumSamples(); ++i)
    {
        const int i0 = (int) pos;
        if (i0 + 1 >= len)
        {
            retire();
            return;
        }
        const auto f = (float) (pos - i0);
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            const auto* d = a.getReadPointer (juce::jmin (ch, a.getNumChannels() - 1));
            out.addSample (ch, i, d[i0] + f * (d[i0 + 1] - d[i0]));
        }
        pos += inc;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MtyaSamplerProcessor();
}
