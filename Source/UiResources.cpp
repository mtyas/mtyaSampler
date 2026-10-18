#include "UiResources.h"
#include "MtyaUI.h"

namespace UiResources
{
    juce::MemoryBlock get (const juce::String& fileName)
    {
        for (int i = 0; i < MtyaUI::namedResourceListSize; ++i)
        {
            if (fileName == MtyaUI::originalFilenames[i])
            {
                int size = 0;
                if (auto* data = MtyaUI::getNamedResource (MtyaUI::namedResourceList[i], size))
                    return { data, (size_t) size };
            }
        }
        return {};
    }

    juce::String getText (const juce::String& fileName)
    {
        auto block = get (fileName);
        return juce::String::fromUTF8 (static_cast<const char*> (block.getData()), (int) block.getSize());
    }

    juce::String mimeTypeFor (const juce::String& fileName)
    {
        const auto ext = fileName.fromLastOccurrenceOf (".", false, false).toLowerCase();
        if (ext == "html")  return "text/html";
        if (ext == "css")   return "text/css";
        if (ext == "js")    return "text/javascript";
        if (ext == "json")  return "application/json";
        if (ext == "svg")   return "image/svg+xml";
        if (ext == "png")   return "image/png";
        if (ext == "woff2") return "font/woff2";
        return "application/octet-stream";
    }
}
