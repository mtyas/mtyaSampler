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
            auto map = buildInstrumentMap (parsed.isObject() ? parsed : juce::var(), cache);
            loadedSamples = map->loadedSamples;
            missingSamples = map->missingSamples;
            engine.publish (std::move (map));

            const juce::ScopedLock sl (lock);
            busy = hasPending;
        }

        if (freed || build)
            cache.prune();
    }
}
