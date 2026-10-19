// Per-sample start/end/loop points (Main page sample editor).
// Every sample of every layer, round-robin variations included, has its own points, stored in the
// layer's "samples" entry as { file, start, end, loopStart, loopEnd } (fractions of the file).
// They're edited with the Start / End / Loop In / Loop Out knobs, by dragging the S, E and L markers
// on the waveform, with Snap 0× and with Auto (found from the sound level by the plugin).

import { knob } from './controls.js';
import { popover, h } from './overlay.js';
import { fileOf, baseName } from './layers.js';

const DEFAULT_REGION = { start: 0, end: 1, loopStart: 0.3, loopEnd: 0.8 };   // same as Layer::Region in C++
const MIN_GAP = 0.001;
const KEYS = [
  ['start', 'Sample Start', 'Start'], ['end', 'Sample End', 'End'],
  ['loopStart', 'Loop Start', 'Loop In'], ['loopEnd', 'Loop End', 'Loop Out']
];

export function regionOf(entry) {
  const r = { ...DEFAULT_REGION };
  if (entry && typeof entry === 'object') for (const k of Object.keys(DEFAULT_REGION)) if (typeof entry[k] === 'number') r[k] = entry[k];
  return r;
}

export function setupRegions(bridge, ui, displays) {
  const $ = (id) => document.getElementById(id);
  let layerId = null, variation = 0, region = { ...DEFAULT_REGION };
  let dragging = 0;            // > 0 while a knob or marker is being dragged
  let commitTimer = 0;
  const listeners = new Set();

  const layer = () => (bridge.getInstrument().layers || []).find((l) => l.id === layerId);
  const notify = () => { listeners.forEach((f) => f()); displays.setRegion(region); };

  // Keep the points in order: start < end, and the loop inside them
  function setPoint(key, value) {
    const r = { ...region, [key]: Math.max(0, Math.min(1, value)) };
    if (key === 'start') r.start = Math.min(r.start, r.end - MIN_GAP);
    if (key === 'end') r.end = Math.max(r.end, r.start + MIN_GAP);
    if (key === 'loopStart') r.loopStart = Math.min(r.loopStart, r.loopEnd - MIN_GAP);
    if (key === 'loopEnd') r.loopEnd = Math.max(r.loopEnd, r.loopStart + MIN_GAP);
    r.loopStart = Math.max(r.start, Math.min(r.end, r.loopStart));
    r.loopEnd = Math.max(r.loopStart, Math.min(r.end, r.loopEnd));
    region = r;
    notify();
    scheduleCommit();
  }

  function scheduleCommit(now = false) {
    clearTimeout(commitTimer);
    commitTimer = setTimeout(commit, now ? 0 : 120);
  }
  async function commit() {
    const inst = JSON.parse(JSON.stringify(bridge.getInstrument()));
    const l = (inst.layers || []).find((x) => x.id === layerId);
    if (!l || !l.samples || variation >= l.samples.length) return;
    const round = (x) => Math.round(x * 1e6) / 1e6;
    l.samples[variation] = { file: fileOf(l.samples[variation]), start: round(region.start), end: round(region.end), loopStart: round(region.loopStart), loopEnd: round(region.loopEnd) };
    await bridge.setInstrument(inst);
  }

  // ---------- the four knobs ----------
  // Same interface as a parameter wrapper (bridge.js), so the normal knob control works.
  function pointParam([key, name, label]) {
    const spec = { id: 'region_' + key, name, label, type: 'float', min: 0, max: 100, default: key === 'end' ? 100 : 0, unit: '%', decimals: 1 };
    return {
      spec,
      getValue: () => region[key] * 100,
      getNorm: () => region[key],
      setNorm: (n) => setPoint(key, n),
      setValue: (v) => setPoint(key, v / 100),
      begin: () => { dragging++; },
      end: () => { dragging = Math.max(0, dragging - 1); scheduleCommit(true); },
      onChange: (fn) => { listeners.add(fn); return () => listeners.delete(fn); }
    };
  }
  const box = $('editor-knobs');
  [...KEYS].reverse().forEach((k) => box.prepend(knob(bridge, pointParam(k))));

  // ---------- round-robin picker ----------
  const picker = $('rr-pick');
  function renderPicker(l) {
    const n = l ? l.samples.length : 0;
    picker.hidden = n < 2;
    picker.innerHTML = '';
    for (let i = 0; i < Math.min(n, 8); i++) {
      const b = h('button', { type: 'button', 'aria-pressed': String(i === variation), title: baseName(l.samples[i]) }, 'RR' + (i + 1));
      b.onclick = () => select(i);
      picker.append(b);
    }
  }

  // ---------- show a layer / variation ----------
  let peaksKey = '';
  async function showPeaks(l) {
    const key = l ? `${l.id}|${variation}|${fileOf(l.samples[variation])}` : '';
    if (key === peaksKey) return;
    peaksKey = key;
    if (!l) return;
    if (!bridge.inPlugin) { $('sel-file-meta').textContent = 'Preview: no audio files'; return; }   // keeps the drawn waveform
    const r = await bridge.command('peaks', l.id, variation);
    if (key !== peaksKey || !r) return;
    displays.setPeaks(r.peaks || []);
    if (r.seconds) displays.setDuration(r.seconds);
    if (r.meta) $('sel-file-meta').textContent = r.meta;
  }

  function load(l) {
    if (!l) { layerId = null; $('sel-file').textContent = '—'; renderPicker(null); return; }
    if (l.id !== layerId) { layerId = l.id; variation = 0; }
    variation = Math.min(variation, Math.max(0, l.samples.length - 1));
    // While dragging, our copy is newer than what comes back from the instrument
    if (!dragging) region = regionOf(l.samples[variation]);
    $('sel-file').textContent = baseName(l.samples[variation]) || l.name;
    renderPicker(l);
    notify();
    showPeaks(l);
  }
  function select(i) { variation = i; region = regionOf(layer()?.samples[i]); load(layer()); }

  // ---------- drag the markers on the waveform ----------
  const wave = $('wave');
  const loopVisible = () => [1, 2].includes(bridge.param('play_mode').getValue());
  const nearest = (clientX) => {
    const r = wave.getBoundingClientRect();
    const keys = loopVisible() ? ['start', 'end', 'loopStart', 'loopEnd'] : ['start', 'end'];
    let best = null, bestDist = 12 * (r.width / wave.offsetWidth || 1);   // 12 px, scaled like the UI
    for (const k of keys) {
      const d = Math.abs(r.left + region[k] * r.width - clientX);
      if (d < bestDist) { best = k; bestDist = d; }
    }
    return best;
  };
  // In Granular mode, dragging anywhere else inside start–end moves the grain position
  const granular = () => bridge.param('play_mode').getValue() === 4;
  const insideRegion = (clientX) => { const r = wave.getBoundingClientRect(), f = (clientX - r.left) / r.width; return f >= region.start && f <= region.end; };
  wave.addEventListener('pointermove', (e) => {
    if (wave.hasPointerCapture(e.pointerId)) return;
    wave.style.cursor = nearest(e.clientX) || (granular() && insideRegion(e.clientX)) ? 'ew-resize' : '';
  });
  wave.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 || layerId === null) return;
    const key = nearest(e.clientX);
    if (!key && granular() && insideRegion(e.clientX)) {
      e.preventDefault();
      const p = bridge.param('grn_pos');
      const set = (ev) => {
        const r = wave.getBoundingClientRect(), f = (ev.clientX - r.left) / r.width;
        p.setValue(Math.max(0, Math.min(100, (f - region.start) / Math.max(1e-6, region.end - region.start) * 100)));
      };
      try { wave.setPointerCapture(e.pointerId); } catch { /* pointer already gone */ }
      p.begin(); set(e);
      const up = () => { p.end(); wave.removeEventListener('pointermove', set); wave.removeEventListener('pointerup', up); wave.removeEventListener('pointercancel', up); };
      wave.addEventListener('pointermove', set);
      wave.addEventListener('pointerup', up);
      wave.addEventListener('pointercancel', up);
      return;
    }
    if (!key) return;
    e.preventDefault();
    try { wave.setPointerCapture(e.pointerId); } catch { /* pointer already gone */ }
    dragging++;
    const move = (ev) => {
      const r = wave.getBoundingClientRect();
      setPoint(key, (ev.clientX - r.left) / r.width);
    };
    const up = () => {
      wave.removeEventListener('pointermove', move);
      wave.removeEventListener('pointerup', up);
      wave.removeEventListener('pointercancel', up);
      dragging = Math.max(0, dragging - 1);
      scheduleCommit(true);
    };
    wave.addEventListener('pointermove', move);
    wave.addEventListener('pointerup', up);
    wave.addEventListener('pointercancel', up);
  });
  bridge.param('play_mode').onChange(() => displays.setRegion(region));

  // ---------- Auto: start and end from the sound level ----------
  const autoBtn = $('auto-points');
  let threshold = -40;
  try { threshold = Number(localStorage.getItem('mtya.autoThreshold')) || -40; } catch { /* storage blocked */ }
  autoBtn.onclick = () => popover(autoBtn, 'Find start and end', (body, api) => {
    const value = h('span', { class: 'mono' });
    const range = h('input', { type: 'range', min: -80, max: -10, step: 1, 'aria-label': 'Threshold below the peak (dB)' });
    const show = () => { value.textContent = threshold + ' dB'; range.style.setProperty('--a', ((threshold + 80) / 70 * 100) + '%'); };
    range.value = threshold;
    range.oninput = () => { threshold = +range.value; show(); try { localStorage.setItem('mtya.autoThreshold', threshold); } catch { /* ignore */ } };
    show();
    const run = async (scope) => {
      api.close();
      await ui.command('autoRegion', layerId, variation, scope, threshold);
    };
    body.append(
      h('p', { class: 'meta pop-empty' }, 'Start where the sound rises above the threshold (below its peak), end where it falls 20 dB lower, a loop in the steady part. All on zero crossings.'),
      h('div', { class: 'auto-thr' }, h('span', { class: 'meta' }, 'Threshold'), h('div', { class: 'amt' }, range), value),
      h('div', { class: 'pop-foot' },
        h('button', { class: 'btn', onclick: () => run('sample') }, 'This sample'),
        h('button', { class: 'btn', onclick: () => run('layer') }, 'Whole layer'),
        h('button', { class: 'btn', onclick: () => run('all') }, 'Every sample')));
  }, { width: 380 });

  return {
    load,
    select,
    variation: () => variation,
    current: () => ({ layerId, variation, region: { ...region } })
  };
}
