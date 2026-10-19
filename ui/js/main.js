import { createBridge } from './bridge.js';
import { knob, hslider, segmented, toggle, tabs, setAppScale } from './controls.js';
import { setupDisplays } from './displays.js';
import { setupLayers, noteName } from './layers.js';
import { setupCreate } from './create.js';
import { setupFxChain } from './fxchain.js';
import { setupModMatrix } from './modmatrix.js';
import { setupPresets } from './presets.js';
import { setupAbout } from './about.js';

const $ = (id) => document.getElementById(id);
const W = 1280, H = 920;

function fitToWindow() {
  // Keep the 1280×920 design and scale it to whatever size the plugin window is.
  const s = Math.min(window.innerWidth / W, window.innerHeight / H);
  $('app').style.transform = `scale(${s})`;
  setAppScale(s);
}

let toastTimer = 0;
function toast(msg) {
  const t = $('toast');
  t.textContent = msg; t.classList.add('show');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => t.classList.remove('show'), 2200);
}

async function boot() {
  fitToWindow();
  window.addEventListener('resize', fitToWindow);
  document.addEventListener('contextmenu', (e) => e.preventDefault()); // no browser menu inside the plugin

  const bridge = await createBridge();
  await bridge.loadInstrument();

  // ---------- parameter-bound controls ----------
  document.querySelectorAll('[data-knobs]').forEach((box) => {
    box.dataset.knobs.split(',').forEach((id) => box.appendChild(knob(bridge, id.trim(), box.dataset.color)));
  });
  document.querySelectorAll('[data-choice]').forEach((el) => segmented(bridge, el, el.dataset.choice));
  document.querySelectorAll('[data-toggle]').forEach((el) => toggle(bridge, el, el.dataset.toggle));
  document.querySelectorAll('[data-hslider]').forEach((el) => hslider(bridge, el, el.dataset.hslider));

  // ---------- FX chain ----------
  setupFxChain(bridge);

  // ---------- displays + tabs ----------
  const displays = setupDisplays(bridge);
  const mod = setupModMatrix(bridge);
  tabs($('env-tabs'), 'env', (n) => { displays.setEnv(n); mod.setEnv(n); });
  tabs($('lfo-tabs'), 'lfo', (n) => { displays.setLfo(n); mod.setLfo(n); });

  // ---------- pages ----------
  let currentPage = 'main';
  let create = null;
  const showPage = (name) => {
    $('page-main').hidden = name !== 'main';
    $('page-layers').hidden = name !== 'layers';
    $('page-create').hidden = name !== 'create';
    currentPage = name;
    document.querySelectorAll('#nav [data-page]').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.page === name)));
  };
  document.querySelectorAll('[data-page]').forEach((b) => b.addEventListener('click', () => showPage(b.dataset.page)));
  document.querySelectorAll('[data-page-link]').forEach((b) => b.addEventListener('click', () => showPage(b.dataset.pageLink)));

  // ---------- keyboard (C2..C6 by default, octave buttons shift it) ----------
  let baseOct = 2; // C2 = MIDI 48 with C3 = 60
  const keys = $('keys');
  const kbdRange = () => [(baseOct + 2) * 12, (baseOct + 2) * 12 + 48];
  const held = new Set();
  const press = (n, el) => {
    held.add(n); el.classList.add('down');
    $('last-note').textContent = noteName(n) + ' · v100';
    bridge.noteOn(n, 100);
  };
  const release = (n, el) => { if (!held.has(n)) return; held.delete(n); el.classList.remove('down'); bridge.noteOff(n); };
  function buildKeys() {
    keys.innerHTML = '';
    const [k0] = kbdRange();
    const WN = [0, 2, 4, 5, 7, 9, 11], whites = 29, bw = 100 / whites;
    const make = (n, cls, style, label) => {
      const b = document.createElement('button');
      b.className = cls; b.setAttribute('aria-label', noteName(n));
      if (style) b.setAttribute('style', style);
      if (label) b.textContent = label;
      b.addEventListener('pointerdown', (e) => { b.setPointerCapture(e.pointerId); press(n, b); });
      b.addEventListener('pointerup', () => release(n, b));
      b.addEventListener('pointercancel', () => release(n, b));
      b.addEventListener('keydown', (e) => { if ((e.key === ' ' || e.key === 'Enter') && !held.has(n)) { e.preventDefault(); press(n, b); } });
      b.addEventListener('keyup', (e) => { if (e.key === ' ' || e.key === 'Enter') release(n, b); });
      keys.appendChild(b);
    };
    for (let i = 0; i < whites; i++) {
      const n = k0 + Math.floor(i / 7) * 12 + WN[i % 7];
      make(n, 'w', '', i % 7 === 0 ? noteName(n) : '');
    }
    for (let i = 0; i < whites - 1; i++) {
      if (![0, 1, 3, 4, 5].includes(i % 7)) continue;
      const n = k0 + Math.floor(i / 7) * 12 + WN[i % 7] + 1;
      make(n, 'b', `left:${((i + 1) * bw - bw * 0.31).toFixed(3)}%;width:${(bw * 0.62).toFixed(3)}%`, '');
    }
  }
  buildKeys();

  // Computer keyboard → notes (A W S E D F T G Y H U J K), like most plugins
  const QWERTY = 'awsedftgyhujk';
  window.addEventListener('keydown', (e) => {
    if (currentPage === 'create') { if (create) create.key(e); return; }
    if (e.repeat || e.target.closest('input,select')) return;
    const i = QWERTY.indexOf(e.key.toLowerCase());
    if (i < 0) return;
    const n = kbdRange()[0] + 12 + i;
    const el = [...keys.children].find((k) => k.getAttribute('aria-label') === noteName(n));
    if (el) press(n, el);
  });
  window.addEventListener('keyup', (e) => {
    if (currentPage === 'create') return;
    const i = QWERTY.indexOf(e.key.toLowerCase());
    if (i < 0) return;
    const n = kbdRange()[0] + 12 + i;
    const el = [...keys.children].find((k) => k.getAttribute('aria-label') === noteName(n));
    if (el) release(n, el);
  });

  // Pitch / mod wheels
  function wheel(el, kind, spring) {
    const thumb = el.querySelector('i'), bar = el.querySelector('b');
    let v = spring ? 0.5 : 0;
    const draw = () => {
      thumb.style.top = ''; thumb.style.bottom = `calc(${(v * 100).toFixed(1)}% - ${v * 10}px)`;
      if (bar) bar.style.height = (v * 100) + '%';
      el.setAttribute('aria-valuenow', spring ? (v * 2 - 1).toFixed(2) : String(Math.round(v * 127)));
    };
    const set = (e) => { const r = el.getBoundingClientRect(); v = Math.max(0, Math.min(1, 1 - (e.clientY - r.top) / r.height)); draw(); bridge.wheel(kind, v); };
    el.addEventListener('pointerdown', (e) => { el.setPointerCapture(e.pointerId); set(e); });
    el.addEventListener('pointermove', (e) => { if (el.hasPointerCapture(e.pointerId)) set(e); });
    el.addEventListener('pointerup', () => { if (spring) { v = 0.5; draw(); bridge.wheel(kind, v); } });
    el.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowUp') v = Math.min(1, v + 0.05); else if (e.key === 'ArrowDown') v = Math.max(0, v - 0.05); else return;
      e.preventDefault(); draw(); bridge.wheel(kind, v);
    });
    draw();
  }
  wheel($('pitch-wheel'), 'pitch', true);
  wheel($('mod-wheel'), 'mod', false);

  // ---------- meters + stats ----------
  bridge.onMeters((m) => {
    $('meter-l').style.height = Math.round((m.l ?? 0) * 100) + '%';
    $('meter-r').style.height = Math.round((m.r ?? 0) * 100) + '%';
    if (m.voices !== undefined) $('stat-voices').textContent = String(m.voices).padStart(2, '0') + ' / 96';
    if (m.cpu !== undefined) $('stat-cpu').textContent = m.cpu.toFixed(1) + '%';
  });

  // ---------- commands that need C++ (file dialogs, presets, sample analysis) ----------
  const ui = {
    kbdRange,
    toast,
    async command(name, ...args) {
      if (name === 'learnRange') toast('Play the lowest key, then the highest key');
      const r = await bridge.command(name, ...args);
      if (r && r.reason === 'preview') toast(`“${name}” works inside the plugin`);
      else if (r && r.message) toast(r.message);
      else if (r && r.ok === false) toast(`“${name}” failed`);
      return r;
    },
    // The selected layer changed (or its samples did): show its real waveform
    layerChanged: (() => {
      let lastKey = '';
      return async (l) => {
        const key = l ? l.id + '|' + (l.samples[0] || '') : '';
        if (key === lastKey) return;
        lastKey = key;
        if (!l) return;
        if (!bridge.inPlugin) return;   // the preview keeps its drawn waveform
        const r = await bridge.command('peaks', l.id);
        if (key !== lastKey || !r) return;
        displays.setPeaks(r.peaks && r.peaks.length ? r.peaks : []);
        if (r.meta) $('sel-file-meta').textContent = r.meta;
      };
    })()
  };
  const layers = setupLayers(bridge, ui);
  const presets = setupPresets(bridge, ui);
  setupAbout(bridge);
  ui.showGroup = layers.showGroup;
  create = setupCreate(bridge, ui);
  const octave = (d) => { baseOct = Math.max(-1, Math.min(6, baseOct + d)); buildKeys(); layers.renderMini(); };
  $('oct-down').onclick = () => octave(-1);
  $('oct-up').onclick = () => octave(1);

  // Commands that act on the selected layer get its id
  const LAYER_COMMANDS = new Set(['snapZero', 'normalize', 'detectRoot', 'learnRange']);
  document.querySelectorAll('[data-action]').forEach((b) => b.addEventListener('click', () => {
    const a = b.dataset.action;
    if (layers.actions[a]) layers.actions[a]();
    else if (presets.actions[a]) presets.actions[a]();
    else if (LAYER_COMMANDS.has(a)) ui.command(a, layers.current()?.id);
    else ui.command(a);
  }));

  // ---------- drag audio files onto the window to import them ----------
  const AUDIO = /\.(wav|aiff?|flac|ogg|mp3)$/i;
  const zone = $('drop-zone');
  let dragDepth = 0;
  const hasFiles = (e) => [...(e.dataTransfer?.types || [])].includes('Files');
  window.addEventListener('dragenter', (e) => { if (!hasFiles(e)) return; e.preventDefault(); dragDepth++; zone.hidden = false; });
  window.addEventListener('dragover', (e) => { if (hasFiles(e)) e.preventDefault(); });
  window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; zone.hidden = true; } });
  window.addEventListener('drop', async (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    dragDepth = 0; zone.hidden = true;
    const files = [...e.dataTransfer.files].filter((f) => AUDIO.test(f.name));
    if (!files.length) { toast('Drop WAV, AIFF, FLAC, OGG or MP3 files'); return; }
    if (!bridge.inPlugin) { toast('Dropping samples works inside the plugin'); return; }
    const tooBig = files.find((f) => f.size > 200 * 1024 * 1024);
    if (tooBig) { toast(`“${tooBig.name}” is over 200 MB: use Import folder instead`); return; }
    toast(`Importing ${files.length} file${files.length === 1 ? '' : 's'}…`);
    const read = (f) => new Promise((ok, fail) => {
      const r = new FileReader();
      r.onload = () => ok({ name: f.name, data: String(r.result).split(',')[1] || '' });
      r.onerror = () => fail(r.error);
      r.readAsDataURL(f);
    });
    try {
      ui.command('importDropped', JSON.stringify(await Promise.all(files.map(read))));
    } catch (err) {
      toast('Couldn’t read the dropped files');
      console.error(err);
    }
  });

  if (!bridge.inPlugin) console.info('mtyaSampler UI: browser preview mode (no plugin connected).');
}

boot().catch((e) => {
  console.error(e);
  document.body.insertAdjacentHTML('beforeend', `<pre style="position:fixed;left:8px;bottom:8px;color:#E8664A;font:12px monospace;white-space:pre-wrap">${String(e.stack || e)}</pre>`);
});
