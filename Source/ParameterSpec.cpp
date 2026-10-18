#include "ParameterSpec.h"
#include "UiResources.h"

namespace ParameterSpec
{
    juce::var load()
    {
        auto parsed = juce::JSON::parse (UiResources::getText ("params.json"));
        jassert (parsed.isObject()); // params.json missing from BinaryData or invalid JSON
        return parsed;
    }

    juce::AudioProcessorValueTreeState::ParameterLayout createLayout (const juce::var& spec)
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        if (auto* params = spec["params"].getArray())
        {
            for (const auto& p : *params)
            {
                const auto id   = p["id"].toString();
                const auto name = p["name"].toString();
                const auto type = p["type"].toString();
                const juce::ParameterID pid { id, 1 };

                if (type == "bool")
                {
                    layout.add (std::make_unique<juce::AudioParameterBool> (pid, name, (bool) p["default"]));
                }
                else if (type == "choice")
                {
                    juce::StringArray choices;
                    if (auto* arr = p["choices"].getArray())
                        for (const auto& c : *arr)
                            choices.add (c.toString());

                    layout.add (std::make_unique<juce::AudioParameterChoice> (pid, name, choices, (int) p["default"]));
                }
                else // float
                {
                    const auto min  = (float) (double) p["min"];
                    const auto max  = (float) (double) p["max"];
                    const auto step = p.hasProperty ("step") ? (float) (double) p["step"] : 0.0f;
                    const auto skew = p.hasProperty ("skew") ? (float) (double) p["skew"] : 1.0f;

                    layout.add (std::make_unique<juce::AudioParameterFloat> (
                        pid, name,
                        juce::NormalisableRange<float> (min, max, step, skew),
                        (float) (double) p["default"],
                        juce::AudioParameterFloatAttributes().withLabel (p["unit"].toString())));
                }
            }
        }

        return layout;
    }
}
