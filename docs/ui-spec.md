# mtyaSampler UI spec

Three pages in one fixed 1280×920 window, which scales to the plugin window. Screenshots are in `docs/screenshots/`.

## Main page

| Area | Contents | Bound to |
|---|---|---|
| Header | Logo, page switch (Main / Layers), preset name with previous/next, Browse, Save, voice count, CPU, master slider, output meter | `master_vol`; commands `presetPrev/Next/Browse/Save`; `meters` event |
| Sample editor | Selected layer's sample name, playback-mode switch, waveform with start/end markers (S/E), loop region (L), slice lines or grain window depending on mode; 7 knobs; Root, Slices, Snap 0×, Normalize | `play_mode`, `smp_start`, `smp_end`, `loop_start`, `loop_end`, `loop_xfade`, `tune`, `fine`, `grn_pos` |
| Layers (compact) | Scrolling list of every layer (number, name, key/velocity range, RR count, mute, solo), Mapping button → Layers page, Add layer | instrument JSON |
| Granular | Freeze, grain-cloud display, Size, Density, Spray, Pitch Rnd | `grn_*` |
| Filter | Type switch (LP24, LP12, BP, HP, Comb), live response curve, Cutoff, Reso, Drive, Env Amt | `flt_*` |
| Env | Tabs Amp / Filter / Mod, live ADSR shape, A D S R knobs per tab | `env1_*`, `env2_*`, `env3_*` |
| LFO | Tabs 1–4, shape buttons in the display, live waveform, destination tag, Rate, Depth, Phase, Fade In | `lfo1_*` … `lfo4_*` |
| FX chain | Saturate, Bitcrush, Chorus, Delay, Reverb: on/off button plus a draggable amount bar | `fx_*` |
| Macros | Texture, Motion, Space, Grit | `macro1..4` |
| Keyboard | Pitch and mod wheels, last note, octave −/+, 4-octave keyboard (also the computer keys A–K), zone bar showing the selected layer's key range and root | native `noteOn/noteOff/wheel` |

## Layers page

| Area | Contents |
|---|---|
| Key / velocity map | One box per layer on a grid of keys C0–B7 against velocity 0–127. Group filter. Click a box to select it, drag it to move, drag its edges to resize. Arrow keys nudge it. A keyboard strip shows the range (amber) and root (cyan). |
| All layers table | #, name, group, keys, root, velocity, RR count, volume, pan, output, mute/solo. Add, Duplicate, Delete, Import folder. |
| Round robin | Variations of the selected layer as mini waveforms with file names. "Audition next" follows the layer's RR mode and plays the root note. |
| Velocity curve | Lin / Soft / Hard / Fixed, with an In → Out readout. Applies to the whole instrument. |
| Inspector | Name (editable), previous/next. **MIDI mapping:** low/high key, root, key track, low/high velocity, key and velocity crossfade, plus *Learn key range* and *Detect root pitch*. **Round robin:** mode, group, position, reset. **Trigger:** note on / release / legato / first, release volume, release decay, velocity → volume. **Voice:** polyphony, choke group, steal mode, voice mode, glide, group. **Articulation & output:** keyswitch, articulation, output bus, volume, pan. |

Note fields accept names like `C3`, `F#2` or `D♯3`, or MIDI numbers. C3 = MIDI 60.

## Create page (sample-recording assistant)

| Area | Contents |
|---|---|
| Setup | **Instrument presets:** Voice, Guitar, Keys/Piano, Bass, Strings & Winds, Hardware Synth, Drums & One-shots, Custom. **Range:** lowest/highest note, plus "Sample every…" (every note, 2, 3, 4 or 6 semitones, or an octave), with a keyboard preview of the recorded notes. **Takes:** session name, round robins (1–8), velocity layers (1–4), max length per take, order (by note or one pass per round robin), direction. **Assistant:** count-in beats and tempo, tone length, spoken prompts, reference tone, go on automatically. **Recording:** input (stereo, left, right), start level, stop after silence, keep before start, fade-out, normalize (off, whole session, each take), hear the input, replace the instrument, save folder. The footer shows the take count, the estimated time and the Start button. Settings are remembered in the browser storage. |
| Session | Step strip (Announce, Tone, Count-in, Record, Save), large note name with velocity / RR / take counter, count-in circle (3, 2, 1, GO), live waveform of the take with a REC timer, input meter with the start-level marker and a clip light, and Pause/Resume, Redo last take, Skip, Keep & next (when "go on automatically" is off), Stop. A progress bar runs underneath. |
| Take map | Rows are velocity layers and columns are recorded notes; each cell has one small box per round robin, coloured to do / now / saved / check / skipped. Click a box to record or redo that take. |
| Recent takes | The last 12 takes, each with a waveform, file, length, peak and any warnings (clipped, nothing heard), plus a Redo button. |

**Auto-sample over MIDI** (Setup, on by default for the Hardware Synth preset): the plugin plays each take's note from its MIDI output instead of prompting you. Settings: MIDI channel, Hold note (s), Gap between (s). Each take waits the gap, sends note-on (velocity = the top of its velocity layer, or 100 with one layer) as recording starts, sends note-off after the hold time, and keeps recording until the release falls silent (or Max length, which is raised to at least hold + 0.5 s). Prompts, tone and count-in are skipped. A held note is always released when a take stops, is skipped or paused, or the session ends. Route the plugin's MIDI out to the synth and the synth's audio into the plugin's input ("Record In").

When a session starts, the planned layers are added to the instrument with empty sample slots, as a group named after the session (or replacing the instrument). Each saved take fills its slot. Skipped slots are removed when the session ends.

File names follow `<Session>_<Note>_<vel>_rr<n>.wav`, for example `My_Guitar_Ds2_f_rr2.wav`, which matches the planned auto-map-by-name feature.

## Controls (ui/js/controls.js)

- **Knob:** drag up/down; Shift for fine; mouse wheel; double-click resets to the default; arrow keys, Home and End when focused. It has `role="slider"` with value text.
- **Horizontal slider:** click or drag; the same keys, wheel and reset as the knob.
- **Segmented switch:** one button per choice, with `aria-pressed`.
- **Toggle:** a button with `aria-pressed`.

## Bridge API (ui/js/bridge.js ↔ Source/PluginEditor.cpp)

Native functions (JS calls C++):

| Name | Arguments | Returns |
|---|---|---|
| `getInstrument` | — | instrument JSON string |
| `setInstrument` | JSON string | `true` |
| `noteOn` | note, velocity (0–127) | — |
| `noteOff` | note | — |
| `wheel` | `"pitch"` or `"mod"`, value 0–1 | — |
| `command` | name, …args | `{ ok, reason?, message? }` or data (see below) |

| `recorder` | command, JSON argument | result `{ ok, message? }` |

`recorder` commands: `start` (plan from `plan.js` `buildPlan`), `pause`, `resume`, `retake` (redo the last saved take), `skip`, `next`, `goto` (take index), `stop`, `monitor` (bool).

`command` names (`Source/PluginEditor.cpp`). A `message`, when there is one, is shown as a toast:

| Name | Arguments | Does |
|---|---|---|
| `presetList` | — | `[{ name, category, path }]` |
| `presetLoad` / `presetSave` | path / name, category | loads or saves `Documents/mtyaSampler/Presets/<Category>/<Name>.mtyapreset` |
| `presetPrev` / `presetNext` / `presetInit` / `presetCurrent` / `presetReveal` | — | step through the list, reset to defaults, report the current one, show the folder |
| `importSamples` | — | folder chooser; every audio file becomes layers, mapped by name (async) |
| `importDropped` | JSON `[{ name, data (base64) }]` | files dropped on the window, saved to `Samples/Dropped`, then imported the same way |
| `autoMap` / `fillGaps` | — | re-map layers from their file names / close key gaps between zones |
| `learnRange` | layer id | waits up to 15 s for two notes, sets low/high key (async) |
| `detectRoot` | layer id | YIN pitch detection on the layer's first sample → `root` |
| `snapZero` | layer id | moves Start, End and the loop points to zero crossings of that sample |
| `normalize` | layer id | toggles the layer's non-destructive `normalize` flag |
| `peaks` | layer id | `{ ok, file, meta, peaks: [[min, max] × 1000] }` for the waveform |
| `previewFile` / `stopPreview` | path / — | plays a WAV through the output once (Create page "Play") |
| `chooseFolder` | start folder | `{ ok, path }` |
| `about` | — | `{ version, format }` for the About box |
| `openUrl` | url | opens one of the About links (GitHub, source, mtyas.com, Ko-fi) in the system browser; any other URL is refused |

In the browser preview, `bridge.js` keeps presets in localStorage; the other commands answer `{ reason: 'preview' }`.

Events (C++ → JS):
- `instrumentChanged`: the instrument JSON.
- `meters`: `{ l, r, voices, cpu, samples, missing }`, 30 per second. `samples` and `missing` count the loaded and not-found sample files in the current instrument.
- `recorderState`: `{ status, phase, take, total, saved, lastSaved, count, phaseTime, phaseLen, recSec, maxSec, inDb, clip, voiceInUi, voicePack, peaks[] }`, 30 per second.
- `recorderTake`: `{ index, file, path, lengthSec, peakDb, clipped, empty, skipped, peaks[], error? }`, once per take.
- `preset`: `{ name, category, path }` when a preset is loaded, saved or reset.
- `meters` also carries `rr` (`{ layerId: next round-robin position }`) and `previewing`.

Parameters use JUCE's relays. In JS, `getSliderState(id)`, `getToggleState(id)` and `getComboBoxState(id)` are wrapped by `bridge.param(id)`, which offers `getValue`, `setValue`, `getNorm`, `setNorm`, `begin`, `end` and `onChange`.

## Instrument JSON

```json
{
  "version": 1,
  "velocityCurve": "linear | soft | hard | fixed",
  "layers": [
    {
      "id": 1, "name": "Felt A1 pp", "group": "Felt Piano",
      "lo": 36, "hi": 53, "root": 45, "vlo": 0, "vhi": 52,
      "keyFade": 2, "velFade": 8, "keyTrack": 100,
      "rrMode": "off | cycle | random | norepeat", "rrGroup": "RR-Felt", "rrReset": "never | idle | transport",
      "samples": ["Felt_A1_pp_rr1.wav", "Felt_A1_pp_rr2.wav"],
      "volume": 0.0, "pan": 0, "output": "Main | Bus 2 | Bus 3 | Bus 4",
      "muted": false, "solo": false,
      "trigger": "attack | release | legato | first", "releaseVolume": -6, "releaseDecay": 600, "velToVol": 80,
      "polyphony": 16, "choke": 0, "steal": "oldest | quietest | lowest | highest",
      "voiceMode": "poly | mono | legato", "glide": 0,
      "keyswitch": -1, "articulation": "Sustain"
    }
  ]
}
```

Units: keys and keyswitch are MIDI notes (−1 = none), volume is in dB, pan runs −50…50, times are in ms, `keyTrack` and `velToVol` are percentages. `samples` holds one file per round-robin variation: a full path, or a bare file name looked up in the optional top-level `"sampleFolder"` (a full path), then under `Documents/mtyaSampler/Recordings` and `Documents/mtyaSampler/Samples` (recursively). Placeholders such as `(drop a sample)` and files that can't be found play a quiet sine at the layer's root.

How the engine reads the layer fields:

- **Fades** are centred on the zone edge: a layer with `velFade: 8` and `vlo: 64` fades in over velocities 60–68, so a neighbour ending at 63 with the same fade crosses over at equal power. There's no fade at 0 or 127.
- **Round robin:** layers sharing an `rrGroup` share one counter and pick the same variation for a note, so velocity layers stay in step. An empty group name means the layer has its own counter.
- **Trigger:** `release` layers play on key-up (always through to the end), at `releaseVolume`, fading by e^(−held ms ÷ `releaseDecay`) the longer the key was held (0 = no fade). `legato` plays only while another key is held, `first` only when none is.
- **Voice:** `polyphony` and `steal` apply per layer. `mono` retriggers, `legato` keeps the voice and changes pitch; both return to the last key still held. `glide` slides from the previous note (in poly mode too, when above 0). `choke` > 0 cuts every other voice in the same choke group.
- **Keyswitch:** a layer with a keyswitch only plays while that keyswitch (or another with the same articulation) was the last one pressed; the lowest one is active at first. Keyswitch notes make no sound. Layers with −1 always play.
- `output`: Bus 2–4 play on those plugin outputs when the host has switched them on (they skip the FX chain and Master); otherwise on Main.
- `normalize: true` applies a gain so the layer's loudest sample peaks at 0 dBFS (the files aren't changed).

Playback modes (`play_mode`, global): **One-shot** plays Start→End once, stopping early on key-up via the amp envelope. **Loop** plays into the loop and repeats Loop Start→Loop End with an equal-power crossfade (`loop_xfade`, limited by the audio before the loop start). **Ping-pong** bounces between the loop points. **Slice** splits Start→End into 16; the key's distance from the root picks the slice (root = slice 1), played at its own pitch. **Granular** sprays grains (`grn_size`, `grn_density`, `grn_spray`, `grn_pitchrnd`) around a position that starts at `grn_pos` and moves at normal speed, or stays at `grn_pos` while Freeze is on. Release samples always play as one-shots. Pitch bend is ±2 st; the sustain pedal (CC 64) holds notes.

Sound design (per voice: sample → filter with drive → modulated pan → Env 1 amp; then the FX chain and Master on the whole mix):

- **Filter** (`flt_type`), all running at 2× the sample rate with half-band FIR up/down-sampling, the knobs smoothed over ~10 ms:
  - **LP24**: a zero-delay-feedback transistor ladder with tanh stages. Resonance past ~95 % self-oscillates; the passband is compensated so turning it up doesn't thin the bass much.
  - **LP12 / BP / HP**: a state-variable filter with Q = 0.707 + Reso × 9 up to 90 % (like the UI curve); above that the damping goes negative and it self-oscillates. Its band state saturates, so loud resonant peaks compress like an analogue filter. BP is unity at the centre.
  - **Comb**: feedback comb with peaks at the cutoff and its multiples, a damped, DC-blocked, saturating loop; past 90 % the feedback exceeds 1 and it rings on its own.
  - **Drive** (`flt_drive`) is the level going into the filter (0–24 dB, ±24 dB more from modulation), with the output compensated.
- **Cutoff**: the knob, plus Env 2 × Env Amt (±6 octaves), plus any `cutoff` routes.
- **Envelopes**: Env 1 = amp (fixed). Env 2 and Env 3 are mod sources (Env 2 also drives Env Amt).
- **LFOs** run per voice from the note start (Phase sets where they begin; Fade In ramps the depth from zero). Shapes match the display: sine, triangle, saw (falling), square, S&H (new random value each cycle). Depth scales the LFO before its routes.
- Modulation and the filter coefficients update every 32 samples and glide in between.

### Modulation (`"modRoutes"`)

```json
"modRoutes": [
  { "source": "lfo1", "target": "grain_pos", "amount": 50 },
  { "source": "macro3", "target": "param:fx_reverb_mix", "amount": 30 }
]
```

Sources: `env2`, `env3` (0…1), `lfo1`…`lfo4` (−1…1 × depth), `velocity` (0…1), `key` ((note − 60) / 60), and the global ones `macro1`…`macro4`, `modwheel`, `aftertouch` (0…1). Amount is −100…100 %.

| Target | 100 % means |
|---|---|
| `pitch` | ±24 semitones |
| `volume` | ×(1 ± 1), clamped at 0 |
| `pan` | hard left/right (balance) |
| `cutoff` | ±6 octaves |
| `reso` | ±100 % resonance |
| `drive` | ±24 dB |
| `grain_pos` | ± the whole start–end region |
| `grain_size`, `grain_density` | ×¼ … ×4 |
| `grain_spray` | ±100 % |
| `grain_pitchrnd` | ±12 st |
| `start` | sample start moves up to the end of the region (read at note-on) |
| `param:<id>` | any float parameter, ± its whole range (global sources only) |

Global sources are applied once per block; per-voice ones per voice. An instrument without `"modRoutes"` gets the defaults (`defaultModRoutes()` in `Source/InstrumentMap.cpp`, `DEFAULT_ROUTES` in `ui/js/modmatrix.js`): Env 3 → grain size, LFO 1 → grain pos, LFO 2 → pan, LFO 3 → cutoff, LFO 4 → spray, and two targets for each macro. An empty list means nothing is modulated.

### FX chain

`"fxOrder": ["sat", "crush", "chorus", "delay", "reverb"]` sets the order (dragged in the UI). Each effect fades in and out over 20 ms when switched and is skipped while off.

| Effect | Parameters |
|---|---|
| Saturate | Drive, Type (soft tanh, hard clip, tube = asymmetric, fold), Tone (low-pass 800 Hz…open), Mix; 2× oversampled |
| Bitcrush | Bits, Rate (sample-and-hold 500 Hz…48 kHz), Mix |
| Chorus | Mix, Rate, Depth, Feedback (`juce::dsp::Chorus`) |
| Delay | Time or Sync + Division (host tempo), Feedback, Mix, Ping-pong, Tone (low-pass in the loop); time changes glide |
| Reverb | Mix, Size, Damping, Width, Pre-delay (`juce::dsp::Reverb`) |

## Parameters (from ui/params.json)

| id | Name | Type | Range | Default | Group |
|---|---|---|---|---|---|
| `master_vol` | Master Volume | float | 0 – 100 % | 72 | Master |
| `play_mode` | Playback Mode | choice | One-shot / Loop / Ping-pong / Slice / Granular | Granular | Sample |
| `smp_start` | Sample Start | float | 0 – 100 % | 8 | Sample |
| `smp_end` | Sample End | float | 0 – 100 % | 86 | Sample |
| `loop_start` | Loop Start | float | 0 – 100 % | 34 | Sample |
| `loop_end` | Loop End | float | 0 – 100 % | 72 | Sample |
| `loop_xfade` | Loop Crossfade | float | 0 – 500 ms (skew 0.4) | 22 | Sample |
| `tune` | Tune | float | -24 – 24 st (step 1, bipolar) | 0 | Sample |
| `fine` | Fine Tune | float | -100 – 100 ct (step 1, bipolar) | 12 | Sample |
| `grn_pos` | Grain Position | float | 0 – 100 % | 52.5 | Granular |
| `grn_size` | Grain Size | float | 5 – 500 ms (skew 0.5) | 84 | Granular |
| `grn_density` | Grain Density | float | 1 – 100 /s (skew 0.5) | 32 | Granular |
| `grn_spray` | Grain Spray | float | 0 – 100 % | 18 | Granular |
| `grn_pitchrnd` | Grain Pitch Random | float | 0 – 12 st | 2 | Granular |
| `grn_freeze` | Grain Freeze | bool | on / off | off | Granular |
| `flt_type` | Filter Type | choice | LP24 / LP12 / BP / HP / Comb | LP24 | Filter |
| `flt_cutoff` | Filter Cutoff | float | 20 – 20000 Hz (skew 0.25) | 2400 | Filter |
| `flt_reso` | Filter Resonance | float | 0 – 100 % | 38 | Filter |
| `flt_drive` | Filter Drive | float | 0 – 24 dB | 4 | Filter |
| `flt_envamt` | Filter Env Amount | float | -100 – 100 % (bipolar) | 36 | Filter |
| `env1_attack` | Env 1 (Amp) Attack | float | 0.001 – 10 s (skew 0.3) | 0.012 | Env 1 |
| `env1_decay` | Env 1 (Amp) Decay | float | 0.001 – 10 s (skew 0.3) | 0.42 | Env 1 |
| `env1_sustain` | Env 1 (Amp) Sustain | float | 0 – 100 % | 70 | Env 1 |
| `env1_release` | Env 1 (Amp) Release | float | 0.001 – 20 s (skew 0.3) | 1.2 | Env 1 |
| `env2_attack` | Env 2 (Filter) Attack | float | 0.001 – 10 s (skew 0.3) | 0.002 | Env 2 |
| `env2_decay` | Env 2 (Filter) Decay | float | 0.001 – 10 s (skew 0.3) | 0.68 | Env 2 |
| `env2_sustain` | Env 2 (Filter) Sustain | float | 0 – 100 % | 30 | Env 2 |
| `env2_release` | Env 2 (Filter) Release | float | 0.001 – 20 s (skew 0.3) | 0.6 | Env 2 |
| `env3_attack` | Env 3 (Mod) Attack | float | 0.001 – 10 s (skew 0.3) | 1.4 | Env 3 |
| `env3_decay` | Env 3 (Mod) Decay | float | 0.001 – 10 s (skew 0.3) | 0.9 | Env 3 |
| `env3_sustain` | Env 3 (Mod) Sustain | float | 0 – 100 % | 50 | Env 3 |
| `env3_release` | Env 3 (Mod) Release | float | 0.001 – 20 s (skew 0.3) | 2.4 | Env 3 |
| `lfo1_shape` | LFO 1 Shape | choice | Sine / Triangle / Saw / Square / S&H | Sine | LFO 1 |
| `lfo1_rate` | LFO 1 Rate | float | 0.01 – 30 Hz (skew 0.3) | 2 | LFO 1 |
| `lfo1_depth` | LFO 1 Depth | float | 0 – 100 % | 55 | LFO 1 |
| `lfo1_phase` | LFO 1 Phase | float | 0 – 360 ° | 90 | LFO 1 |
| `lfo1_fade` | LFO 1 Fade In | float | 0 – 5000 ms (skew 0.4) | 180 | LFO 1 |
| `lfo2_shape` | LFO 2 Shape | choice | Sine / Triangle / Saw / Square / S&H | Triangle | LFO 2 |
| `lfo2_rate` | LFO 2 Rate | float | 0.01 – 30 Hz (skew 0.3) | 0.3 | LFO 2 |
| `lfo2_depth` | LFO 2 Depth | float | 0 – 100 % | 20 | LFO 2 |
| `lfo2_phase` | LFO 2 Phase | float | 0 – 360 ° | 180 | LFO 2 |
| `lfo2_fade` | LFO 2 Fade In | float | 0 – 5000 ms (skew 0.4) | 0 | LFO 2 |
| `lfo3_shape` | LFO 3 Shape | choice | Sine / Triangle / Saw / Square / S&H | S&H | LFO 3 |
| `lfo3_rate` | LFO 3 Rate | float | 0.01 – 30 Hz (skew 0.3) | 4 | LFO 3 |
| `lfo3_depth` | LFO 3 Depth | float | 0 – 100 % | 35 | LFO 3 |
| `lfo3_phase` | LFO 3 Phase | float | 0 – 360 ° | 0 | LFO 3 |
| `lfo3_fade` | LFO 3 Fade In | float | 0 – 5000 ms (skew 0.4) | 600 | LFO 3 |
| `lfo4_shape` | LFO 4 Shape | choice | Sine / Triangle / Saw / Square / S&H | Saw | LFO 4 |
| `lfo4_rate` | LFO 4 Rate | float | 0.01 – 30 Hz (skew 0.3) | 0.06 | LFO 4 |
| `lfo4_depth` | LFO 4 Depth | float | 0 – 100 % | 70 | LFO 4 |
| `lfo4_phase` | LFO 4 Phase | float | 0 – 360 ° | 270 | LFO 4 |
| `lfo4_fade` | LFO 4 Fade In | float | 0 – 5000 ms (skew 0.4) | 50 | LFO 4 |
| `fx_sat_on` | Saturate On | bool | on / off | on | FX |
| `fx_sat_drive` | Saturate Drive | float | 0 – 24 dB | 4.2 | FX |
| `fx_crush_on` | Bitcrush On | bool | on / off | off | FX |
| `fx_crush_bits` | Bitcrush Bits | float | 1 – 16 bit (step 1) | 12 | FX |
| `fx_chorus_on` | Chorus On | bool | on / off | on | FX |
| `fx_chorus_mix` | Chorus Mix | float | 0 – 100 % | 28 | FX |
| `fx_delay_on` | Delay On | bool | on / off | on | FX |
| `fx_delay_fb` | Delay Feedback | float | 0 – 95 % | 42 | FX |
| `fx_reverb_on` | Reverb On | bool | on / off | on | FX |
| `fx_reverb_mix` | Reverb Mix | float | 0 – 100 % | 30 | FX |
| `macro1` | Macro 1 (Texture) | float | 0 – 100 % | 48 | Macros |
| `macro2` | Macro 2 (Motion) | float | 0 – 100 % | 62 | Macros |
| `macro3` | Macro 3 (Space) | float | 0 – 100 % | 70 | Macros |
| `macro4` | Macro 4 (Grit) | float | 0 – 100 % | 22 | Macros |
