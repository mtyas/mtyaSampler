#include "PluginEditor.h"
#include "UiResources.h"
#include "SampleTools.h"

namespace
{
    juce::var result (bool ok, const juce::String& message = {})
    {
        auto* r = new juce::DynamicObject();
        r->setProperty ("ok", ok);
        if (message.isNotEmpty())
            r->setProperty ("message", message);
        return juce::var (r);
    }

    // The layer a command is about: by id, or (older UI) by index
    int findLayer (const juce::var& instrument, const juce::var& key)
    {
        const auto* layers = instrument["layers"].getArray();
        if (layers == nullptr)
            return -1;
        for (int i = 0; i < layers->size(); ++i)
            if ((*layers)[i]["id"].toString() == key.toString())
                return i;
        const int index = (int) key;
        return key.isInt() || key.isDouble() ? (index >= 0 && index < layers->size() ? index : -1) : -1;
    }
}

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
    if (proc.editorWidth > 0 && proc.editorHeight > 0)
        setSize (proc.editorWidth, proc.editorHeight);   // the size it had last time
    else
        setSize (designWidth * 7 / 8, designHeight * 7 / 8);

    proc.addChangeListener (this);
    proc.presets.onChange = [this] { emitPreset(); };
    proc.recorder.onTake = [this] (const juce::var& take) { browser.emitEventIfBrowserIsVisible ("recorderTake", take); };
    startTimerHz (30);
}

MtyaSamplerEditor::~MtyaSamplerEditor()
{
    proc.removeChangeListener (this);
    proc.presets.onChange = nullptr;
    if (learn.active && learn.done)
        learn.done (result (false, "Closed"));
    // The UI fills the layer map as takes arrive, so a session cannot run without it.
    proc.recorder.onTake = nullptr;
    proc.recorder.command ("pause", {});
}

void MtyaSamplerEditor::resized()
{
    browser.setBounds (getLocalBounds());
    proc.editorWidth = getWidth();
    proc.editorHeight = getHeight();
}

void MtyaSamplerEditor::emitPreset()
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("name", proc.presets.name);
    o->setProperty ("category", proc.presets.category);
    o->setProperty ("path", proc.presets.path);
    browser.emitEventIfBrowserIsVisible ("preset", juce::var (o));
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
            if (! handleAsyncCommand (name, rest, done))
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

bool MtyaSamplerEditor::handleAsyncCommand (const juce::String& name, const juce::Array<juce::var>& args,
                                            juce::WebBrowserComponent::NativeFunctionCompletion& done)
{
    if (name == "importSamples")
    {
        // Pick a folder; every audio file in it (and below) becomes layers, mapped by name
        importChooser = std::make_unique<juce::FileChooser> ("Import a folder of samples", SampleCache::defaultSearchFolders()[1]);
        importChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                    [this, done] (const juce::FileChooser& fc)
                                    {
                                        const auto dir = fc.getResult();
                                        if (dir == juce::File())
                                            return done (result (false, "Import cancelled"));
                                        auto inst = juce::JSON::parse (proc.getInstrumentJson());
                                        const auto message = SampleTools::importFiles (inst, SampleTools::audioFilesIn (dir), dir.getFileName());
                                        proc.setInstrumentJson (juce::JSON::toString (inst), true);
                                        done (result (true, message));
                                    });
        return true;
    }

    if (name == "learnRange")
    {
        if (learn.active && learn.done)
            learn.done (result (false, "Cancelled"));
        int dummy;
        while (proc.popLearnedNote (dummy)) {}
        learn = { true, args[0], {}, juce::Time::getMillisecondCounter() + 15000, done };
        return true;
    }

    return false;
}

void MtyaSamplerEditor::pollLearn()
{
    if (! learn.active)
        return;
    int note;
    while (learn.notes.size() < 2 && proc.popLearnedNote (note))
        learn.notes.add (note);

    if (learn.notes.size() >= 2)
    {
        auto inst = juce::JSON::parse (proc.getInstrumentJson());
        const auto i = findLayer (inst, learn.layerId);
        if (i >= 0)
        {
            auto& l = inst["layers"].getArray()->getReference (i);
            const int lo = juce::jmin (learn.notes[0], learn.notes[1]), hi = juce::jmax (learn.notes[0], learn.notes[1]);
            l.getDynamicObject()->setProperty ("lo", lo);
            l.getDynamicObject()->setProperty ("hi", hi);
            proc.setInstrumentJson (juce::JSON::toString (inst), true);
            learn.done (result (true, "Key range " + SampleTools::noteName (lo) + "–" + SampleTools::noteName (hi)));
        }
        else
            learn.done (result (false, "That layer no longer exists"));
        learn.active = false;
    }
    else if (juce::Time::getMillisecondCounter() > learn.deadline)
    {
        learn.done (result (false, "No keys played: try again and play the lowest, then the highest key"));
        learn.active = false;
    }
}

juce::var MtyaSamplerEditor::handleCommand (const juce::String& name, const juce::Array<juce::var>& args)
{
    auto& presets = proc.presets;

    // ---- presets ----
    if (name == "presetList")   return presets.list();
    if (name == "presetLoad")   return presets.load (juce::File (args[0].toString()));
    if (name == "presetSave")   return presets.save (args[0].toString(), args[1].toString());
    if (name == "presetPrev")   return presets.step (-1);
    if (name == "presetNext")   return presets.step (1);
    if (name == "presetInit")   return presets.init();
    if (name == "presetCurrent")
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", presets.name);
        o->setProperty ("category", presets.category);
        o->setProperty ("path", presets.path);
        return juce::var (o);
    }
    if (name == "presetReveal")
    {
        PresetManager::folder().createDirectory();
        PresetManager::folder().revealToUser();
        return result (true);
    }

    // ---- listen back (Create page) ----
    if (name == "previewFile")
    {
        auto audio = SampleTools::read (juce::File (args[0].toString()));
        if (! audio.ok())
            return result (false, "Couldn't read that file");
        proc.clipPlayer.play (std::move (audio.buffer), audio.sampleRate);
        return result (true);
    }
    if (name == "stopPreview")
    {
        proc.clipPlayer.stop();
        return result (true);
    }

    auto inst = juce::JSON::parse (proc.getInstrumentJson());

    // ---- whole-instrument edits ----
    if (name == "autoMap" || name == "fillGaps")
    {
        const auto message = name == "autoMap" ? SampleTools::autoMap (inst) : SampleTools::fillGaps (inst);
        proc.setInstrumentJson (juce::JSON::toString (inst), true);
        return result (true, message);
    }
    if (name == "importDropped")
    {
        // Files dropped on the UI arrive as base64; they're saved to Documents/mtyaSampler/Samples/Dropped
        const auto dir = SampleCache::defaultSearchFolders()[1].getChildFile ("Dropped");
        dir.createDirectory();
        juce::Array<juce::File> files;
        if (const auto* list = juce::JSON::parse (args[0].toString()).getArray())
            for (const auto& item : *list)
            {
                juce::MemoryOutputStream data;
                if (! juce::Base64::convertFromBase64 (data, item["data"].toString()))
                    continue;
                const auto file = dir.getNonexistentChildFile (juce::File::createLegalFileName (item["name"].toString()).upToLastOccurrenceOf (".", false, false),
                                                               "." + item["name"].toString().fromLastOccurrenceOf (".", false, false), false);
                if (file.replaceWithData (data.getData(), data.getDataSize()))
                    files.add (file);
            }
        if (files.isEmpty())
            return result (false, "No audio files in the drop");
        const auto message = SampleTools::importFiles (inst, files, "Dropped");
        proc.setInstrumentJson (juce::JSON::toString (inst), true);
        return result (true, message);
    }

    // ---- commands on one layer (args[0] = layer id) ----
    const int index = findLayer (inst, args[0]);
    if (index < 0)
        return result (false, "Select a layer first");
    auto& layer = inst["layers"].getArray()->getReference (index);
    const auto* samples = layer["samples"].getArray();
    const auto sampleName = samples != nullptr && ! samples->isEmpty() ? (*samples)[0].toString() : juce::String();
    const auto file = SampleTools::locate (sampleName, inst["sampleFolder"].toString());

    if (name == "peaks")
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("ok", file.existsAsFile());
        o->setProperty ("file", sampleName.isNotEmpty() ? juce::File::createFileWithoutCheckingPath (sampleName).getFileName() : layer["name"].toString());
        if (file.existsAsFile())
        {
            const auto audio = SampleTools::read (file);
            if (audio.ok())
            {
                o->setProperty ("peaks", SampleTools::peaks (audio.buffer));
                o->setProperty ("meta", juce::String (audio.sampleRate / 1000.0, 1) + " kHz · " + juce::String (audio.bitsPerSample) + "-bit · "
                                          + juce::String (audio.buffer.getNumSamples() / audio.sampleRate, 2) + " s"
                                          + (audio.buffer.getNumChannels() > 1 ? " · stereo" : " · mono"));
            }
        }
        else
            o->setProperty ("meta", sampleName.isEmpty() || sampleName.startsWith ("(") ? "No sample yet" : "File not found: plays a stand-in tone");
        return juce::var (o);
    }

    if (! file.existsAsFile())
        return result (false, "The layer's sample file wasn't found");
    const auto audio = SampleTools::read (file);
    if (! audio.ok())
        return result (false, "Couldn't read " + file.getFileName());

    if (name == "detectRoot")
    {
        const auto hz = SampleTools::detectPitchHz (audio.buffer, audio.sampleRate);
        if (hz <= 0)
            return result (false, "No clear pitch found in " + file.getFileName());
        const auto midi = 69.0 + 12.0 * std::log2 (hz / 440.0);
        const int root = juce::jlimit (0, 127, juce::roundToInt (midi));
        const int cents = juce::roundToInt ((midi - root) * 100.0);
        layer.getDynamicObject()->setProperty ("root", root);
        proc.setInstrumentJson (juce::JSON::toString (inst), true);
        return result (true, "Root " + SampleTools::noteName (root) + (cents != 0 ? " (" + juce::String (cents > 0 ? "+" : "") + juce::String (cents) + " ct)" : juce::String())
                                 + " · " + juce::String (hz, 1) + " Hz");
    }

    if (name == "normalize")
    {
        // Non-destructive: the engine applies a gain so the loudest sample of the layer reaches 0 dBFS
        const bool on = ! (bool) layer["normalize"];
        layer.getDynamicObject()->setProperty ("normalize", on);
        proc.setInstrumentJson (juce::JSON::toString (inst), true);
        const auto peak = audio.buffer.getMagnitude (0, audio.buffer.getNumSamples());
        return result (true, on ? "Normalize on (" + juce::String (juce::Decibels::gainToDecibels (1.0f / juce::jmax (1.0e-6f, peak)), 1) + " dB)"
                                : juce::String ("Normalize off"));
    }

    if (name == "snapZero")
    {
        // Move start, end and loop points to the nearest zero crossings of this layer's sample
        const int len = audio.buffer.getNumSamples();
        for (auto id : { "smp_start", "smp_end", "loop_start", "loop_end" })
            if (auto* p = proc.apvts.getParameter (id))
            {
                auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p);
                const auto percent = ranged->convertFrom0to1 (ranged->getValue());
                const int at = SampleTools::nearestZeroCrossing (audio.buffer, (int) (percent * 0.01f * (float) len));
                ranged->beginChangeGesture();
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (100.0f * (float) at / (float) len));
                ranged->endChangeGesture();
            }
        return result (true, "Snapped to zero crossings");
    }

    return result (false, "“" + name + "” isn't a command");
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
    m->setProperty ("samples", proc.loader.loadedSamples.load());
    m->setProperty ("missing", proc.loader.missingSamples.load());
    m->setProperty ("rr", proc.loader.rrPositions());
    m->setProperty ("previewing", proc.clipPlayer.isPlaying());
    browser.emitEventIfBrowserIsVisible ("meters", juce::var (m));
    browser.emitEventIfBrowserIsVisible ("recorderState", proc.recorder.getState());
    proc.clipPlayer.collectGarbage();
    pollLearn();
}
