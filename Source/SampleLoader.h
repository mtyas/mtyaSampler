#pragma once
#include "SamplerEngine.h"
#include <juce_events/juce_events.h>

// Background thread that turns instrument JSON into an InstrumentMap (loading any sample files
// it needs) and hands it to the engine. It also deletes the maps the engine has finished with,
// so no file I/O or freeing happens on the audio thread.
//
// Edits arrive often (the Layers page sends the whole instrument on every drag); only the newest
// JSON is built, and files already in RAM are reused.
class SampleLoader final : private juce::Thread
{
public:
    explicit SampleLoader (SamplerEngine&);
    ~SampleLoader() override;

    // Any thread.
    void load (const juce::String& instrumentJson);

    // Totals for the newest map, for the UI.
    std::atomic<int> loadedSamples { 0 }, missingSamples { 0 };
    std::atomic<bool> busy { false };

    // { layerId: next round-robin position } for the inspector (message thread)
    juce::var rrPositions() const;

private:
    void run() override;

    SamplerEngine& engine;
    SampleCache cache;

    mutable juce::CriticalSection lock;
    juce::String pendingJson;
    bool hasPending = false;
    std::vector<std::pair<juce::int64, int>> layerGroups;   // (layer id, RR group) of the newest map, under `lock`
};
