# mtyaSampler UI spec

Two pages in one fixed 1280×920 window, which scales to the plugin window. Screenshots are in `docs/screenshots/`.

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
| `command` | name, …args | `{ ok, reason?, message? }` |

Events (C++ → JS): `instrumentChanged` (instrument JSON), `meters` (`{ l, r, voices, cpu }`, 30 per second). Planned: `peaks` (waveform `[min, max]` pairs).

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

Units: keys and keyswitch are MIDI notes (−1 = none), volume is in dB, pan runs −50…50, times are in ms, `keyTrack` and `velToVol` are percentages. `samples` holds one file per round-robin variation. Paths are relative to the preset/sample folder; that still needs deciding when import is built.

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
