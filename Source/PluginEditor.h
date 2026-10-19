#pragma once
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"

// The editor is a single WebBrowserComponent (WebView2 on Windows, WKWebView on macOS)
// showing the HTML/CSS/JS UI from ui/. Every parameter in params.json gets a JUCE "relay"
// so the UI controls stay in sync with host automation, undo and preset recall.
class MtyaSamplerEditor final : public juce::AudioProcessorEditor,
                                private juce::Timer,
                                private juce::ChangeListener
{
public:
    explicit MtyaSamplerEditor (MtyaSamplerProcessor&);
    ~MtyaSamplerEditor() override;

    void resized() override;
    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xff0e0f11)); }

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    juce::WebBrowserComponent::Options buildOptions();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::var handleCommand (const juce::String& name, const juce::Array<juce::var>& args);

    MtyaSamplerProcessor& proc;
    std::unique_ptr<juce::FileChooser> folderChooser;

    // Declaration order matters: relays → browser → attachments.
    std::vector<std::unique_ptr<juce::WebSliderRelay>> sliderRelays;
    std::vector<std::unique_ptr<juce::WebToggleButtonRelay>> toggleRelays;
    std::vector<std::unique_ptr<juce::WebComboBoxRelay>> comboRelays;
    juce::StringArray sliderIds, toggleIds, comboIds;   // parameter id for each relay, same order

    juce::WebBrowserComponent browser;

    std::vector<std::unique_ptr<juce::WebSliderParameterAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<juce::WebToggleButtonParameterAttachment>> toggleAttachments;
    std::vector<std::unique_ptr<juce::WebComboBoxParameterAttachment>> comboAttachments;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MtyaSamplerEditor)
};
