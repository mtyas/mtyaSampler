// Bridge between the UI and the plugin.
//
// Inside the plugin, JUCE's WebBrowserComponent injects window.__JUCE__ and we use JUCE's own
// frontend library (copied to js/juce/ by CMake from JUCE/modules/juce_gui_extra/native/javascript).
// Parameters are bound through WebSliderRelay / WebToggleButtonRelay / WebComboBoxRelay on the C++ side,
// one relay per parameter id in params.json.
//
// In a normal browser (python -m http.server, see README) there is no __JUCE__, so a mock keeps
// all values locally. That lets the UI be developed and tested without building the plugin.

const clamp01 = (x) => Math.max(0, Math.min(1, x));

// Same maths as juce::NormalisableRange (non-symmetric skew).
export function toNorm(spec, v) {
  if (spec.type === 'bool') return v ? 1 : 0;
  if (spec.type === 'choice') return spec.choices.length > 1 ? v / (spec.choices.length - 1) : 0;
  const p = clamp01((v - spec.min) / (spec.max - spec.min));
  return spec.skew && spec.skew !== 1 ? Math.pow(p, spec.skew) : p;
}
export function fromNorm(spec, n) {
  n = clamp01(n);
  if (spec.type === 'bool') return n >= 0.5;
  if (spec.type === 'choice') return Math.round(n * (spec.choices.length - 1));
  const p = spec.skew && spec.skew !== 1 ? Math.pow(n, 1 / spec.skew) : n;
  let v = spec.min + (spec.max - spec.min) * p;
  if (spec.step) v = Math.round(v / spec.step) * spec.step;
  return Math.max(spec.min, Math.min(spec.max, v));
}

class Emitter {
  constructor() { this.fns = new Set(); }
  on(fn) { this.fns.add(fn); return () => this.fns.delete(fn); }
  emit(...a) { this.fns.forEach((f) => f(...a)); }
}

// ---------- parameter wrappers ----------
class MockParam {
  constructor(spec) { this.spec = spec; this.value = spec.default; this.ev = new Emitter(); }
  getValue() { return this.value; }
  getNorm() { return toNorm(this.spec, this.value); }
  setNorm(n) { this.setValue(fromNorm(this.spec, n)); }
  setValue(v) { if (v === this.value) return; this.value = v; this.ev.emit(v); }
  begin() {} end() {}
  onChange(fn) { return this.ev.on(fn); }
}

class JuceParam {
  constructor(spec, Juce) {
    this.spec = spec;
    if (spec.type === 'bool') this.state = Juce.getToggleState(spec.id);
    else if (spec.type === 'choice') this.state = Juce.getComboBoxState(spec.id);
    else this.state = Juce.getSliderState(spec.id);
  }
  getValue() {
    const s = this.state;
    if (this.spec.type === 'bool') return s.getValue();
    if (this.spec.type === 'choice') return s.getChoiceIndex();
    return s.getScaledValue();
  }
  getNorm() {
    if (this.spec.type === 'float') return this.state.getNormalisedValue();
    return toNorm(this.spec, this.getValue());
  }
  setNorm(n) {
    if (this.spec.type === 'float') this.state.setNormalisedValue(clamp01(n));
    else this.setValue(fromNorm(this.spec, n));
  }
  setValue(v) {
    const s = this.state;
    if (this.spec.type === 'bool') s.setValue(!!v);
    else if (this.spec.type === 'choice') s.setChoiceIndex(v);
    else s.setNormalisedValue(toNorm(this.spec, v));
  }
  begin() { if (this.spec.type === 'float') this.state.sliderDragStarted(); }
  end() { if (this.spec.type === 'float') this.state.sliderDragEnded(); }
  onChange(fn) {
    const id = this.state.valueChangedEvent.addListener(() => fn(this.getValue()));
    return () => this.state.valueChangedEvent.removeListener(id);
  }
}

// ---------- bridge ----------
export async function createBridge() {
  const spec = await (await fetch('params.json')).json();
  const inPlugin = typeof window.__JUCE__ !== 'undefined';
  let Juce = null;
  if (inPlugin) Juce = await import('./juce/index.js');

  const params = new Map();
  for (const s of spec.params) params.set(s.id, inPlugin ? new JuceParam(s, Juce) : new MockParam(s));

  const layersEv = new Emitter();
  const recStateEv = new Emitter();
  const recTakeEv = new Emitter();
  let mockRec = null;
  const meterEv = new Emitter();
  const presetEv = new Emitter();
  let instrument = null;   // { version, velocityCurve, layers: [...] }

  const native = (name) => (Juce ? Juce.getNativeFunction(name) : null);

  async function loadInstrument() {
    if (inPlugin) instrument = JSON.parse(await native('getInstrument')());
    else instrument = await (await fetch('demo-layers.json')).json();
    return instrument;
  }

  if (inPlugin) {
    // C++ emits these with WebBrowserComponent::emitEventIfBrowserIsVisible
    window.__JUCE__.backend.addEventListener('instrumentChanged', (json) => {
      instrument = typeof json === 'string' ? JSON.parse(json) : json;
      layersEv.emit(instrument);
    });
    window.__JUCE__.backend.addEventListener('meters', (m) => meterEv.emit(m));
    window.__JUCE__.backend.addEventListener('recorderState', (st) => recStateEv.emit(st));
    window.__JUCE__.backend.addEventListener('recorderTake', (t) => recTakeEv.emit(t));
    window.__JUCE__.backend.addEventListener('preset', (p) => presetEv.emit(p));
  } else {
    const { MockRecorder } = await import('./mock-recorder.js');
    mockRec = new MockRecorder();
    mockRec.on('state', (st) => recStateEv.emit(st));
    mockRec.on('take', (t) => recTakeEv.emit(t));
    // Fake meters + stats so the preview feels alive
    let t = 0;
    setInterval(() => { t += 0.1; meterEv.emit({ l: 0.55 + 0.1 * Math.sin(t), r: 0.5 + 0.1 * Math.cos(t * 1.3), voices: 6, cpu: 4.1 }); }, 80);
  }

  // ---------- preview-mode presets (localStorage), same results as Source/PresetManager ----------
  const mockPresets = {
    current: { name: 'Init', category: '', path: '' },
    all() { try { return JSON.parse(localStorage.getItem('mtya.presets') || '[]'); } catch { return []; } },
    store(list) { try { localStorage.setItem('mtya.presets', JSON.stringify(list)); } catch { /* storage blocked */ } },
    setCurrent(p) { this.current = { name: p.name, category: p.category, path: p.path }; presetEv.emit(this.current); },
    list() {
      return this.all().map(({ name, category, path }) => ({ name, category, path }))
        .sort((a, b) => (a.category || '').localeCompare(b.category || '') || a.name.localeCompare(b.name));
    },
    async load(path) {
      const p = this.all().find((x) => x.path === path);
      if (!p) return { ok: false, message: 'Preset not found' };
      for (const [id, v] of params) v.setValue(id in p.params ? p.params[id] : v.spec.default);
      instrument = JSON.parse(JSON.stringify(p.instrument));
      layersEv.emit(instrument);
      this.setCurrent(p);
      return { ok: true, message: `Loaded “${p.name}”` };
    },
    save(name, category) {
      if (!name) return { ok: false, message: 'Give the preset a name' };
      const values = {};
      for (const [id, v] of params) values[id] = v.getValue();
      const path = (category ? category + '/' : '') + name;
      const list = this.all().filter((x) => x.path !== path);
      list.push({ name, category, path, params: values, instrument: JSON.parse(JSON.stringify(instrument)) });
      this.store(list);
      this.setCurrent({ name, category, path });
      return { ok: true, message: `Saved “${name}”` };
    },
    async step(d) {
      const list = this.list();
      if (!list.length) return { ok: false, message: 'No presets yet: press Save to make one' };
      const i = list.findIndex((x) => x.path === this.current.path);
      const next = i < 0 ? (d > 0 ? 0 : list.length - 1) : (i + d + list.length) % list.length;
      return this.load(list[next].path);
    },
    async init() {
      for (const [, v] of params) v.setValue(v.spec.default);
      instrument = await (await fetch('demo-layers.json')).json();
      layersEv.emit(instrument);
      this.setCurrent({ name: 'Init', category: '', path: '' });
      return { ok: true, message: 'Back to the Init sound' };
    }
  };
  async function mockCommand(name, ...args) {
    switch (name) {
      case 'presetList': return mockPresets.list();
      case 'presetCurrent': return mockPresets.current;
      case 'presetLoad': return mockPresets.load(args[0]);
      case 'presetSave': return mockPresets.save(String(args[0] || '').trim(), String(args[1] || '').trim());
      case 'presetPrev': return mockPresets.step(-1);
      case 'presetNext': return mockPresets.step(1);
      case 'presetInit': return mockPresets.init();
      case 'peaks': return { ok: false, meta: 'Preview: no audio files' };
      default: return { ok: false, reason: 'preview' };
    }
  }

  return {
    inPlugin,
    spec,
    param(id) {
      const p = params.get(id);
      if (!p) throw new Error('Unknown parameter id: ' + id + ' (add it to params.json)');
      return p;
    },
    loadInstrument,
    getInstrument: () => instrument,
    // Replace the whole instrument (layers + global settings). Cheap enough for a few hundred layers.
    async setInstrument(next) {
      instrument = next;
      if (inPlugin) await native('setInstrument')(JSON.stringify(next));
      layersEv.emit(instrument);
    },
    onInstrument: (fn) => layersEv.on(fn),
    onMeters: (fn) => meterEv.on(fn),
    onPreset: (fn) => presetEv.on(fn),
    async noteOn(note, vel) { if (inPlugin) await native('noteOn')(note, vel); },
    async noteOff(note) { if (inPlugin) await native('noteOff')(note); },
    async wheel(kind, value) { if (inPlugin) await native('wheel')(kind, value); },
    // Sample-recording assistant (Create page). Commands: start(plan) pause resume retake skip next goto(i) stop monitor(bool)
    async recorder(cmd, arg) {
      if (inPlugin) return native('recorder')(cmd, arg === undefined ? '' : JSON.stringify(arg));
      return mockRec.send(cmd, arg);
    },
    onRecorderState: (fn) => recStateEv.on(fn),
    onRecorderTake: (fn) => recTakeEv.on(fn),
    // Things the C++ side does: presets, file dialogs, sample analysis, instrument edits.
    // Returns { ok, message? } (or data). In the preview, presets are mocked; the rest says 'preview'.
    async command(name, ...args) {
      if (!inPlugin) return mockCommand(name, ...args);
      return native('command')(name, ...args);
    }
  };
}
