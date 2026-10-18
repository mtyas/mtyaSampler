#pragma once
#include <juce_core/juce_core.h>

// Access to the files under ui/ that CMake embeds as BinaryData (namespace MtyaUI).
// Files are found by their file name (e.g. "params.json"), which CMake checks is unique.
namespace UiResources
{
    // Returns the bytes of an embedded file, or an empty block if it does not exist.
    juce::MemoryBlock get (const juce::String& fileName);

    // Convenience for text files (JSON, HTML...).
    juce::String getText (const juce::String& fileName);

    // MIME type from the file extension, for the WebView resource provider.
    juce::String mimeTypeFor (const juce::String& fileName);
}
