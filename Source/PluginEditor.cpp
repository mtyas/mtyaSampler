#include "PluginEditor.h"
#include "UiResources.h"

static constexpr int designWidth = 1280, designHeight = 920;

MtyaSamplerEditor::MtyaSamplerEditor (MtyaSamplerProcessor& p)
    : AudioProcessorEditor (p), proc (p), browser (buildOptions())
{
    // Bind every relay to its parameter (relay names == parameter ids from params.json)
    for (size_t i = 0; i < sliderRelays.size(); ++i)
        sliderAttachments.push_back (std::make_unique<juce::WebSliderParameterAttachment> (
            *proc.apvts.getParameter (sliderIds[(int) i]), *sliderRelays[i], nullptr));
    for (size_t i = 0; i < toggleRelays.size(); ++i)
        toggleAttachments.push_back (std::make_unique<juce::WebToggleButtonParameterAttachment> (
            *proc.apvts.getParameter (toggleIds[(int) i]), *toggleRelays[i], nullptr));
    for (size_t i = 0; i < comboRelays.size(); ++i)
        comboAttachments.push_back (std::make_unique<juce::WebComboBoxParameterAttachment> (
            *proc.apvts.getParameter (comboIds[(int) i]), *comboRelays[i], nullptr));

    addAndMakeVisible (browser);

   #if MTYA_UI_DEV_SERVER
    browser.goToURL ("http://localhost:8765/");
   #else
    browser.goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
   #endif

    // The UI is drawn at 1280×920 and scales itself to the window; keep the aspect ratio.
    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) designWidth / designHeight);
    setResizeLimits (designWidth / 2, designHeight / 2, designWidth * 3 / 2, designHeight * 3 / 2);
    setSize (designWidth * 7 / 8, designHeight * 7 / 8);

    proc.addChangeListener (this);
    proc.recorder.onTake = [this] (const juce::var& take) { browser.emitEventIfBrowserIsVisible ("recorderTake", take); };
    startTimerHz (30);
}

MtyaSamplerEditor::~MtyaSamplerEditor()
{
    proc.removeChangeListener (this);
    // The UI fills the layer map as takes arrive, so a session cannot run without it.
    proc.recorder.onTake = nullptr;
    proc.recorder.command ("pause", {});
}

void MtyaSamplerEditor::resized()
{
    browser.setBounds (getLocalBounds());
}

juce::WebBrowserComponent::Options MtyaSamplerEditor::buildOptions()
{
    using Options = juce::WebBrowserComponent::Options;
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

    auto options = Options{};
   #if JUCE_WINDOWS
    // WebView2 needs a writable data folder; the default (next to the host .exe) often is not.
    options = options.withBackend (Options::Backend::webview2)
                     .withWinWebView2Options (Options::WinWebView2{}
                         .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("mtyaSampler_WebView2")));
   #endif
    options = options
        .withNativeIntegrationEnabled()
        .withResourceProvider ([this] (const auto& url) { return getResource (url); },
                               juce::URL ("http://localhost:8765/").getOrigin())

        // ---- instrument (layers) ----
        .withNativeFunction ("getInstrument", [this] (const juce::Array<juce::var>&, Completion done)
        {
            done (proc.getInstrumentJson());
        })
        .withNativeFunction ("setInstrument", [this] (const juce::Array<juce::var>& args, Completion done)
        {
            if (args.size() > 0)
                proc.setInstrumentJson (args[0].toString(), false);
            done (true);
        })

        // ---- on-screen keyboard and wheels ----
        .withNativeFunction ("noteOn", [this] (const juce::Array<juce::var>& args, Completion done)
        {
            if (args.size() >= 2)
                proc.uiMidi.addMessageToQueue (juce::MidiMessage::noteOn (1, (int) args[0], (juce::uint8) (int) args[1])
                                                   .withTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001));
            done ({});
        })
        .withNativeFunction ("noteOff", [this] (const juce::Array<juce::var>& args, Completion done)
        {
            if (args.size() >= 1)
                proc.uiMidi.addMessageToQueue (juce::MidiMessage::noteOff (1, (int) args[0])
                                                   .withTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001));
            done ({});
        })
        .withNativeFunction ("wheel", [this] (const juce::Array<juce::var>& args, Completion done)
        {
            if (args.size() >= 2)
            {
                const auto v = juce::jlimit (0.0, 1.0, (double) args[1]);
                auto msg = args[0].toString() == "pitch"
                    ? juce::MidiMessage::pitchWheel (1, juce::roundToInt (v * 16383.0))
                    : juce::MidiMessage::controllerEvent (1, 1, juce::roundToInt (v * 127.0));
                proc.uiMidi.addMessageToQueue (msg.withTimeStamp (juce::Time::getMillisecondCounterHiRes() * 0.001));
            }
            done ({});
        })

        // ---- Create page: sample-recording assistant ----
        .withNativeFunction ("recorder", [this] (const juce::Array<juce::var>& args, Completion done)
        {
            const auto name = args.size() > 0 ? args[0].toString() : juce::String();
            const auto arg = args.size() > 1 && args[1].toString().isNotEmpty() ? juce::JSON::parse (args[1].toString()) : juce::var();
            done (proc.recorder.command (name, arg));
        })

        // ---- everything that needs native dialogs or file access ----
        .withNativeFunction ("command", [this] (const juce::Array<juce::var>& args, Completion done)
        {
            const auto name = args.size() > 0 ? args[0].toString() : juce::String();
            if (name == "chooseFolder")
            {
                // Async: the completion is called when the user closes the dialog
                auto start = args.size() > 1 && juce::File::isAbsolutePath (args[1].toString()) ? juce::File (args[1].toString())
                                                                                                  : SampleRecorder::defaultRecordingFolder();
                folderChooser = std::make_unique<juce::FileChooser> ("Where should the recorded samples go?", start);
                folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                            [done] (const juce::FileChooser& fc)
                                            {
                                                auto* r = new juce::DynamicObject();
                                                const auto dir = fc.getResult();
                                                r->setProperty ("ok", dir != juce::File());
                                                r->setProperty ("path", dir.getFullPathName());
                                                done (juce::var (r));
                                            });
                return;
            }
            juce::Array<juce::var> rest;
            for (int i = 1; i < args.size(); ++i) rest.add (args[i]);
            done (handleCommand (name, rest));
        });

    // One relay per parameter, named by its id
    if (auto* params = proc.paramSpec["params"].getArray())
    {
        for (const auto& p : *params)
        {
            const auto id = p["id"].toString();
            const auto type = p["type"].toString();

            if (type == "bool")
            {
                toggleRelays.push_back (std::make_unique<juce::WebToggleButtonRelay> (id));
                toggleIds.add (id);
                options = options.withOptionsFrom (*toggleRelays.back());
            }
            else if (type == "choice")
            {
                comboRelays.push_back (std::make_unique<juce::WebComboBoxRelay> (id));
                comboIds.add (id);
                options = options.withOptionsFrom (*comboRelays.back());
            }
            else
            {
                sliderRelays.push_back (std::make_unique<juce::WebSliderRelay> (id));
                sliderIds.add (id);
                options = options.withOptionsFrom (*sliderRelays.back());
            }
        }
    }

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> MtyaSamplerEditor::getResource (const juce::String& url)
{
    // url looks like "/" or "/js/main.js"; files are embedded by file name.
    auto path = url.upToFirstOccurrenceOf ("?", false, false);
    auto fileName = path == "/" ? juce::String ("index.html") : path.fromLastOccurrenceOf ("/", false, false);

    auto block = UiResources::get (fileName);
    if (block.isEmpty())
        return std::nullopt;

    const auto* bytes = static_cast<const std::byte*> (block.getData());
    return juce::WebBrowserComponent::Resource { std::vector<std::byte> (bytes, bytes + block.getSize()),
                                                 UiResources::mimeTypeFor (fileName) };
}

juce::var MtyaSamplerEditor::handleCommand (const juce::String& name, const juce::Array<juce::var>& args)
{
    juce::ignoreUnused (args);
    // TODO (see CLAUDE.md roadmap): importSamples, autoMap, fillGaps, learnRange, detectRoot,
    // presetPrev/Next/Browse/Save, snapZero, normalize.
    auto* result = new juce::DynamicObject();
    result->setProperty ("ok", false);
    result->setProperty ("reason", "todo");
    result->setProperty ("message", "“" + name + "” is not implemented yet");
    return juce::var (result);
}

void MtyaSamplerEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // Instrument changed from C++ (state recall, sample import...) → tell the UI
    browser.emitEventIfBrowserIsVisible ("instrumentChanged", proc.getInstrumentJson());
}

void MtyaSamplerEditor::timerCallback()
{
    auto* m = new juce::DynamicObject();
    m->setProperty ("l", proc.meterL.load());
    m->setProperty ("r", proc.meterR.load());
    m->setProperty ("voices", proc.activeVoices.load());
    m->setProperty ("cpu", proc.cpuLoad.load() * 100.0f);
    browser.emitEventIfBrowserIsVisible ("meters", juce::var (m));
    browser.emitEventIfBrowserIsVisible ("recorderState", proc.recorder.getState());
}
