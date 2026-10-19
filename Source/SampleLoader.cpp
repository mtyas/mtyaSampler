#include "SampleLoader.h"

SampleLoader::SampleLoader (SamplerEngine& e) : juce::Thread ("mtyaSampler loader"), engine (e)
{
    startThread (juce::Thread::Priority::low);
}

SampleLoader::~SampleLoader()
{
    stopThread (4000);
}

void SampleLoader::load (const juce::String& instrumentJson)
{
    {
        const juce::ScopedLock sl (lock);
        pendingJson = instrumentJson;
        hasPending = true;
        busy = true;
    }
    notify();
}

juce::var SampleLoader::rrPositions() const
{
    auto* o = new juce::DynamicObject();
    const juce::ScopedLock sl (lock);
    for (const auto& [id, group] : layerGroups)
        o->setProperty (juce::String (id), engine.rrPosition (group));
    return juce::var (o);
}

void SampleLoader::run()
{
    while (! threadShouldExit())
    {
        wait (50);

        // Maps the audio thread has finished with
        bool freed = false;
        while (auto old = engine.popRetired())
            freed = true;

        juce::String json;
        bool build = false;
        {
            const juce::ScopedLock sl (lock);
            std::swap (build, hasPending);
            json = std::move (pendingJson);
            pendingJson = {};
        }

        if (build)
        {
            auto parsed = juce::JSON::parse (json);
            auto map = buildInstrumentMap (parsed.isObject() ? parsed : juce::var(), cache, engine.paramIds());
            loadedSamples = map->loadedSamples;
            missingSamples = map->missingSamples;
            std::vector<std::pair<juce::int64, int>> groups;
            for (const auto& l : map->layers)
                groups.emplace_back (l.id, l.rrGroup);
            engine.publish (std::move (map));

            const juce::ScopedLock sl (lock);
            layerGroups = std::move (groups);
            busy = hasPending;
        }

        if (freed || build)
            cache.prune();
    }
}
