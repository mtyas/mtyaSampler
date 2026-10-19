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
  } else {
    const { MockRecorder } = await import('./mock-recorder.js');
    mockRec = new MockRecorder();
    mockRec.on('state', (st) => recStateEv.emit(st));
    mockRec.on('take', (t) => recTakeEv.emit(t));
    // Fake meters + stats so the preview feels alive
    let t = 0;
    setInterval(() => { t += 0.1; meterEv.emit({ l: 0.55 + 0.1 * Math.sin(t), r: 0.5 + 0.1 * Math.cos(t * 1.3), voices: 6, cpu: 4.1 }); }, 80);
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
    // Things the C++ side does with native dialogs (file choosers, preset browser).
    async command(name, ...args) {
      if (!inPlugin) return { ok: false, reason: 'preview' };
      return native('command')(name, ...args);
    }
  };
}
