#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Builds the plugin's parameters from ui/params.json, the single list shared with the web UI.
// To add a parameter: add it to params.json, rebuild, then use it in the DSP and the UI.
namespace ParameterSpec
{
    // Parsed params.json ({ "params": [ ... ] }).
    juce::var load();

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const juce::var& spec);

    // The range of one float entry, exactly as the APVTS builds it.
    inline juce::NormalisableRange<float> rangeFor (const juce::var& p)
    {
        const auto min  = (float) (double) p["min"];
        const auto max  = (float) (double) p["max"];
        const auto step = p.hasProperty ("step") ? (float) (double) p["step"] : 0.0f;
        const auto skew = p.hasProperty ("skew") ? (float) (double) p["skew"] : 1.0f;
        return { min, max, step, skew };
    }
}
