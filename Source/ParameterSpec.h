#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

// Builds the plugin's parameters from ui/params.json, the single list shared with the web UI.
// To add a parameter: add it to params.json, rebuild, then use it in the DSP and the UI.
namespace ParameterSpec
{
    // Parsed params.json ({ "params": [ ... ] }).
    juce::var load();

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const juce::var& spec);
}
