#pragma once
#include "SamplerEngine.h"
#include <juce_dsp/juce_dsp.h>

// The master effects: saturate, bitcrush, chorus, delay and reverb, in the order the instrument
// sets ("fxOrder", dragged in the UI). Each effect fades in and out when switched (no clicks) and
// is skipped entirely while off. Parameter values come from the engine, so macros can move them.
// Audio thread only after prepare().
class FxChain
{
public:
    explicit FxChain (const SamplerEngine&);

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    // `bpm` is the host tempo (for the synced delay); 0 when unknown.
    void process (juce::AudioBuffer<float>&, double bpm) noexcept;

    static constexpr double tailSeconds = 10.0;

private:
    enum { sat, crush, chorusFx, delayFx, reverbFx, numFx };

    float p (int index, float fallback) const noexcept { return engine.param (index, fallback); }

    void processSaturate (juce::AudioBuffer<float>&, int n) noexcept;
    void processCrush (juce::AudioBuffer<float>&, int n) noexcept;
    void processChorus (juce::AudioBuffer<float>&, int n) noexcept;
    void processDelay (juce::AudioBuffer<float>&, int n, double bpm) noexcept;
    void processReverb (juce::AudioBuffer<float>&, int n) noexcept;

    const SamplerEngine& engine;
    double sr = 44100.0;

    struct Ix
    {
        int on[numFx];
        int satDrive, satType, satTone, satMix;
        int crushBits, crushRate, crushMix;
        int chorusMix, chorusRate, chorusDepth, chorusFb;
        int delayFb, delayTime, delaySync, delayDiv, delayMix, delayPingPong, delayTone;
        int reverbMix, reverbSize, reverbDamp, reverbWidth, reverbPredelay;
    } ix {};

    juce::SmoothedValue<float> onGain[numFx];
    juce::AudioBuffer<float> dry;   // copy of the input of the effect being blended in or out

    // saturate (2× oversampled)
    mtya::Oversampler2x satUp[2], satDown[2];
    float satTone[2] {};

    // bitcrush
    float crushHold[2] {};
    double crushPhase = 1.0;

    // chorus
    juce::dsp::Chorus<float> chorus;

    // delay
    juce::AudioBuffer<float> delayLine;
    int delayWrite = 0;
    juce::SmoothedValue<float> delaySamples;
    bool delayFresh = true;   // just switched on: start at the set time instead of gliding to it
    float delayLp[2] {};

    // reverb
    juce::dsp::Reverb reverb;
    juce::AudioBuffer<float> wet, predelayLine;
    int predelayWrite = 0;
};
