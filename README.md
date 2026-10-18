# mtyaSampler

A sampler instrument with sound-design tools (granular engine, filter, 3 envelopes, 4 LFOs, FX chain, macros) and a full multi-sample layer editor (key/velocity mapping, round robin, trigger modes, choke groups, keyswitches).

**Formats:** VST3 and CLAP instrument (plus a Standalone app for quick testing). Windows first; the code is cross-platform.

**Status:** the UI is complete and wired to every parameter. The audio engine is a placeholder sine voice so the whole chain (host → MIDI → voices → UI) can be tested. The sampler engine is the next job; see [CLAUDE.md](CLAUDE.md), *Roadmap*.

![Main page](docs/screenshots/main.png)
![Layers page](docs/screenshots/layers.png)

## How it is built

| Part | Choice |
|---|---|
| Plugin framework | [JUCE 8](https://juce.com) with CMake, fetched automatically |
| CLAP | [clap-juce-extensions](https://github.com/free-audio/clap-juce-extensions), fetched automatically |
| UI | HTML/CSS/JS in `ui/`, shown with JUCE's `WebBrowserComponent` (WebView2 on Windows) |
| Parameters | One list, `ui/params.json`, read by both the C++ and the UI |

Why a web UI: the design was made as HTML, and it uses things that are slow to rebuild in JUCE's own components (live curves, a draggable zone map, tables and forms). The UI can also be developed in a normal browser without rebuilding the plugin.

## Build on Windows

You need:

- **Visual Studio 2022** with the *Desktop development with C++* workload
- **CMake 3.22+** and **Git** (both come with Visual Studio, or install separately)
- **The WebView2 SDK**, installed once from PowerShell:

  ```powershell
  Register-PackageSource -provider NuGet -name nugetRepository -location https://www.nuget.org/api/v2
  Install-Package Microsoft.Web.WebView2 -Scope CurrentUser -Source nugetRepository
  ```

Then, from this folder:

```powershell
cmake --preset vs2022
cmake --build --preset release
```

The first configure downloads JUCE and the CLAP extensions, which takes a few minutes. The results are in `build/mtyaSampler_artefacts/Release/`:

- `VST3/mtyaSampler.vst3` → copy to `C:\Program Files\Common Files\VST3\`
- `CLAP/mtyaSampler.clap` → copy to `C:\Program Files\Common Files\CLAP\`
- `Standalone/mtyaSampler.exe`, which runs without a DAW

## Work on the UI without rebuilding

```powershell
cd ui
py -m http.server 8765
```

Open <http://localhost:8765>. Everything works in "preview mode": the parameters are kept in the page, and the layers come from `ui/demo-layers.json`.

To see UI changes inside the real plugin without rebuilding, build the `uidev` preset (`cmake --preset vs2022-uidev`, then `cmake --build --preset uidev`). It loads the UI from that local server instead of the copy built into the plugin.

## Folder map

```
CMakeLists.txt         build (JUCE + CLAP + embedded UI)
Source/                C++: processor, editor (WebView host), parameter loader
ui/                    the interface: index.html, styles.css, js/, params.json, demo-layers.json
docs/ui-spec.md        pages, controls, bridge API, layer schema, parameter table
docs/screenshots/      what the UI should look like
design/                the original design canvases (reference only)
CLAUDE.md              brief for Claude Code: decisions, rules, roadmap
```

## Licences

JUCE 8 is dual-licensed (AGPLv3, or a commercial licence that has a free tier for small developers). Check the JUCE licence before you distribute builds. clap-juce-extensions and the CLAP SDK are MIT.
