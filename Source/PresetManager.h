#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

class MtyaSamplerProcessor;

// Presets are JSON files: Documents/mtyaSampler/Presets/<Category>/<Name>.mtyapreset, holding
// every parameter value plus the instrument (layers, modulation routes, FX order).
// Message thread only.
class PresetManager
{
public:
    explicit PresetManager (MtyaSamplerProcessor&);

    static juce::File folder();
    static constexpr const char* extension = ".mtyapreset";

    // [{ name, category, path }], sorted by category then name
    juce::var list() const;

    juce::var save (const juce::String& name, const juce::String& category);   // { ok, message }
    juce::var load (const juce::File&);                                       // { ok, message }
    juce::var step (int delta);                                               // previous / next in the list
    juce::var init();                                                          // default parameters + the demo instrument

    // What the header shows. Saved with the plugin state.
    juce::String name { "Init" }, category;
    juce::String path;
    std::function<void()> onChange;   // name/category changed (the editor tells the UI)

private:
    static juce::var result (bool ok, const juce::String& message);
    void setCurrent (const juce::String& newName, const juce::String& newCategory, const juce::String& newPath);

    MtyaSamplerProcessor& proc;
};
