# mtyaSampler — brief for Claude Code

**Name and licence:** always written as one word, **mtyaSampler** (the header shows it as `mtya` + amber `Sampler`). Free software under **GPL-3.0-or-later** (`LICENSE`); JUCE is used under AGPLv3. The About box (click the logo, `ui/js/about.js`) links to github.com/mtyas, the repo, mtyas.com and ko-fi.com/mtyas. User docs: `docs/MANUAL.md` (screenshots in `docs/screenshots/`).

A VST3 + CLAP **sampler instrument** with sound-design features and a serious multi-sample layer editor. The owner is Matt, a musician/sound designer on Windows. Keep explanations practical and in plain language.

## Current state (start here)

- **UI: done.** `ui/` has three pages (Main, Layers, Create), and every control is bound to a parameter. It runs in a browser in preview mode (`cd ui && py -m http.server 8765`).
  - `js/modmatrix.js`: target popovers on the Env/LFO tags and under each macro, plus the full matrix dialog.
  - `js/fxchain.js`: FX slots (drag to reorder, pencil = settings popover).
  - `js/presets.js`: preset header, browser and Save dialog. `js/overlay.js`: shared popover/modal helpers.
- **C++: builds on macOS** (VST3, CLAP, Standalone; `cmake --preset mac`). **Not yet built on Windows.** The first Windows job is to build it, load it in a DAW, and fix whatever the Windows build turns up.
- **Sampler engine (roadmap 2–3): done, not yet tried in a DAW.**
  - `Source/InstrumentMap.*`: instrument JSON → immutable zone map; `SampleCache` loads files into RAM and finds bare file names.
  - `Source/SampleLoader.*`: background thread that builds maps and deletes old ones.
  - `Source/SamplerEngine.*`: voices, round robin, trigger modes, voice modes, keyswitches, the five playback modes, Env 1–3, the 4 LFOs and the modulation matrix; Bus 2–4 outputs.
  - `Source/VoiceFilter.h`: the analogue-style filters (nonlinear ladder, saturating SVF and comb, 2× oversampled).
  - `Source/FxChain.*`: the five effects in the instrument's order.
  - `Source/PresetManager.*`, `Source/SampleTools.*`: presets, and the sample commands (import, auto-map, fill gaps, detect root, peaks…).
  - `tests/EngineTests.cpp` (`mtyaEngineTests`, 81 checks) and `tests/PluginTests.cpp` (`mtyaPluginTests`: the real processor with buses and saved state). Run both after engine changes.
  - How each layer field and playback mode behaves is written down in `docs/ui-spec.md` (Instrument JSON).
- **Create page (sample-recording assistant): done.**
  - UI: `ui/js/create.js` and `ui/js/plan.js`, with a browser preview engine in `ui/js/mock-recorder.js`.
  - Engine: `Source/SampleRecorder.*`. It was compiled and run on Linux in a test program that simulated a full session; the takes saved, trimmed and normalized correctly, and an empty take was retried.
  - Not yet tried with a real audio input in a DAW.

## Architecture decisions (don't change without asking)

1. **JUCE 8 + CMake.** JUCE comes in through FetchContent. The tag is `MTYA_JUCE_TAG` in CMakeLists.txt.
2. **CLAP comes from clap-juce-extensions** (`clap_juce_extensions_plugin`). If a later JUCE release adds native CLAP support, it's fine to switch to it and drop the extension.
3. **The UI is a web UI inside `juce::WebBrowserComponent`.**
   - It uses WebView2 on Windows, linked statically (`NEEDS_WEBVIEW2`, `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`).
   - The UI files are embedded as BinaryData (`MtyaUI` namespace) and served by a resource provider. Files are found by **file name**, so names under `ui/` must be unique. CMake checks this.
4. **`ui/params.json` is the only list of automatable parameters.**
   - `ParameterSpec::createLayout` builds the APVTS from it.
   - The editor creates one JUCE relay per entry: `WebSliderRelay` for float, `WebToggleButtonRelay` for bool, `WebComboBoxRelay` for choice. Each relay is named by the parameter id.
   - The UI reads the same file for labels, ranges and formatting.
5. **The instrument (layers, mapping, round robin…) is JSON, not parameters.**
   - The processor stores it as a string. It's saved in the plugin state under the `instrument` property.
   - The UI fetches it with the native function `getInstrument` and sends back the whole edited copy with `setInstrument`.
   - C++ tells the UI about changes with the `instrumentChanged` event.
   - The schema is in `docs/ui-spec.md`.
6. **The Create page records from the plugin's audio input bus ("Record In", stereo, optional).**
   - The JS side plans the session (`plan.js` builds the take list, file names and key/velocity zones) and sends the finished plan with `recorder('start', plan)`.
   - `SampleRecorder` runs the steps for each take sample-accurately on the audio thread: announce → tone → count-in → record → save.
   - Saving (trim, fades, normalize, 24-bit WAV) happens on the message thread while the audio thread waits in the save step.
   - The JS side writes each saved file into its layer's round-robin slot. That's why a session pauses when the editor closes.
   - Spoken prompts come from a voice pack (`Documents/mtyaSampler/VoicePack/*.wav`, one file per word, made by `tools/make-voice-pack.ps1`) and play through the plugin output. If any word is missing, the UI speaks the prompts with the browser's speech synthesis instead.
7. **The UI is a fixed 1280×920 design scaled to fit the window** (`fitToWindow` in main.js). The editor keeps that aspect ratio.

8. **Engine parameters come from `params.json`** through `SamplerEngine::makeParamList` (the APVTS's values in the plugin, the tests' own in tests). The engine reads them by index; `engine.param(i)` includes macro/mod-wheel modulation, and the FX chain reads through it too.
9. **Sample points are per sample** (every round-robin variation too): `{ file, start, end, loopStart, loopEnd }` entries in `samples`, fractions of the file, read into `Layer::regions`. The UI edits them in `ui/js/regions.js` (knobs + draggable markers) and writes them back in the instrument; Auto S/E is `SampleTools::findRegion`.
   **The clean Init** (`ui/clean-init.json`) is shared by Init and new Create-page instruments (`presetNew`).
   **Modulation routes and the FX order live in the instrument JSON** (`modRoutes`, `fxOrder`), not in parameters, so presets carry them and hosts don't see hundreds of extra parameters.
10. **Engine threading.** The loader thread parses the JSON and loads samples, then `SamplerEngine::publish` swaps the new map in through an atomic pointer. The audio thread keeps the old map in a `draining` slot until no voice points into it, then hands it back through a FIFO (`popRetired`) and the loader deletes it. Voices hold raw pointers into their map, so never free a map anywhere else.
11. **One-shot means "play once, no loop"**: it still stops on key-up through the amp envelope (a piano needs that). Only release-trigger samples ignore note-off.

## Rules

- **Never rename or remove a parameter id once it has shipped.** (Before the first release, `smp_start`, `smp_end`, `loop_start` and `loop_end` were removed when the points became per sample.) Hosts store automation and presets by id. Add new ids instead, and bump the `ParameterID` version if the meaning changes.
- **Adding a parameter takes three steps:**
  1. Add it to `params.json`.
  2. Use it in the DSP (`apvts.getRawParameterValue`).
  3. Put a control in `index.html`, either `data-knobs="…"`, `data-choice`, `data-toggle` or `data-hslider`.

  Nothing else needs registering.
- **Audio thread rules:**
  - No allocation, locks, JSON parsing or file I/O in `processBlock`.
  - Parse the instrument JSON into an immutable zone map on a background or message thread, then swap it in atomically.
- **The UI must keep working in preview mode.** Every new native call needs a mock path in `ui/js/bridge.js`.
- **No frameworks or build step for the UI** (plain ES modules), unless Matt asks. That keeps the embed and the dev loop simple.
- **Check UI changes in a real browser.** Serve `ui/`, then use Playwright (or open it) to take screenshots and look at the console. Compare with `docs/screenshots/`.
- Keep the visual language:
  - **Colours:** graphite panels, amber `--acc` for sample/filter/amp, cyan `--mod` for modulation.
  - **Fonts:** Barlow for text, JetBrains Mono for values, both bundled in `ui/fonts/` (no internet needed).
  - **Sizing:** knobs are 44 px.

## Roadmap (suggested order)

1. **Build and load.** Configure, build Release, load the VST3 and CLAP in a DAW (Reaper or Bitwig test CLAP well).
   - Check that the knobs move with host automation and that undo works.
   - Check that the state is saved and restored.
   - Check that the on-screen keyboard plays the placeholder voice.
2. **Sample engine (core): done** (see Current state). Left over: disk streaming for very large libraries.

   Original brief:
   - Load the files named in each layer's `samples` with `AudioFormatManager`. Keep them in RAM first; disk streaming can come later.
   - Build a `ZoneMap` from the instrument JSON, then for each note decide which zones play: key and velocity ranges plus key/velocity crossfades.
   - Round robin modes: off, cycle, random, no-repeat. Each RR group keeps its own counter. `rrReset` decides when the counter goes back to the start.
   - Trigger modes: attack, release (uses `releaseVolume` and `releaseDecay`), legato, first.
   - Voices: polyphony per layer, choke groups, steal modes, poly/mono/legato voice modes, glide, keyswitch/articulation filtering.
   - Pitch from `root` and `keyTrack`, plus the `tune` and `fine` parameters. Volume, pan and output bus per layer (multi-out comes later).
3. **Playback modes: done.** (`play_mode`): one-shot, loop (with `loop_xfade`), ping-pong, slice (16 slices between start and end), and granular (the `grn_*` parameters; Freeze holds the position).
4. **Sound design: done**, with a modulation matrix; ranges in `docs/ui-spec.md`. Left over: better interpolation than Hermite (it leaves images around −60 dB when transposing bright samples).

   Original brief:
   - Filter types (`flt_type`): LP24, LP12, BP, HP, Comb. `juce::dsp::StateVariableTPTFilter` or `LadderFilter` cover LP/BP/HP; the comb is custom.
   - 3 ADSRs: Env 1 = amp, Env 2 = filter (`flt_envamt`), Env 3 = mod.
   - 4 LFOs: shape, rate, depth, phase, fade.
   - Each envelope and LFO currently has one fixed destination, shown in the UI. Later, add a mod-matrix page.
5. **FX chain: done** (drag to reorder, settings per effect).
6. **Macros: done** (routes in the mod matrix).
7. **Native commands: done**: presets and browser, import (folder and drag-and-drop), auto-map, fill gaps, learn range, detect root, snap to zero, normalize, waveform peaks.
8. **Create page next steps:**
   - Test it in Reaper, Bitwig and the Standalone app with a real mic or instrument; check input routing and latency.
   - Done: MIDI auto-sampling ("Auto-sample over MIDI"): the plugin has a MIDI output and plays each take's note itself. Try it with a real hardware synth in Reaper/Bitwig and the Standalone app (its settings have a MIDI output menu).
   - Done: session normalize runs on a background thread (and the sampler reloads the rewritten files); recent takes have a Play button.
9. **Polish:**
   - Done: the fonts are bundled in `ui/fonts/` (Latin + Latin Extended woff2, OFL licences alongside).
   - Done: drag-and-drop import, window size remembered.

## Gotchas

- **WebView2 data folder:** it needs a writable folder. Ours goes in `%TEMP%\mtyaSampler_WebView2`, otherwise hosts installed in Program Files can fail.
- **Missing WebView2 SDK:** if `find_package(WebView2)` fails, the NuGet package isn't installed (see README).
- **Computer-keyboard notes:** the UI maps the A–K row to notes. Some DAWs take keystrokes first; that's expected.
- **Debug assert on startup:** probably `params.json` isn't embedded or isn't valid JSON (jassert in `ParameterSpec::load`).
- **Keep `ui/js/juce/` out of git.** CMake copies it from JUCE on every configure.
- **The plugin's MIDI output carries only the Create page's auto-sampling notes.** `processBlock` clears the MIDI buffer before the recorder writes to it, so incoming MIDI is never echoed back to the host.
- **Dropped files arrive as base64** over the bridge (a WebView can't give the page real file paths), capped at 200 MB per file; Import folder… is better for big libraries.
- **A popover or modal must be inside `#app`** (overlay.js does this) so it scales with the window.
