import { createBridge } from './bridge.js';
import { knob, hslider, segmented, toggle, tabs, setAppScale } from './controls.js';
import { setupDisplays } from './displays.js';
import { setupLayers, noteName } from './layers.js';

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
  const FX = [
    ['sat', 'Saturate', 'fx_sat_on', 'fx_sat_drive'],
    ['crush', 'Bitcrush', 'fx_crush_on', 'fx_crush_bits'],
    ['chorus', 'Chorus', 'fx_chorus_on', 'fx_chorus_mix'],
    ['delay', 'Delay', 'fx_delay_on', 'fx_delay_fb'],
    ['reverb', 'Reverb', 'fx_reverb_on', 'fx_reverb_mix']
  ];
  const fxGrid = $('fx-grid');
  FX.forEach(([key, name, onId, amtId]) => {
    const slot = document.createElement('div');
    slot.className = 'fxslot';
    slot.innerHTML =
      `<div class="top-line"><b>${name}</b><button class="power" aria-label="${name} on/off"><svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true"><path d="M6 1.5 V6 M3.2 3.3 A3.8 3.8 0 1 0 8.8 3.3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg></button></div>` +
      `<span class="meta"></span><div class="hslider"></div>`;
    fxGrid.appendChild(slot);
    toggle(bridge, slot.querySelector('.power'), onId);
    hslider(bridge, slot.querySelector('.hslider'), amtId);
    const onP = bridge.param(onId), amtP = bridge.param(amtId);
    const meta = slot.querySelector('.meta');
    const upd = () => {
      slot.classList.toggle('off', !onP.getValue());
      const v = amtP.getValue(), s = amtP.spec;
      meta.textContent = `${s.label} ${s.unit === 'bit' ? Math.round(v) + '-bit' : v.toFixed(s.decimals ?? 0) + (s.unit === '%' ? '%' : ' ' + s.unit)}`;
    };
    onP.onChange(upd); amtP.onChange(upd); upd();
  });

  // ---------- displays + tabs ----------
  const displays = setupDisplays(bridge);
  tabs($('env-tabs'), 'env', (n) => displays.setEnv(n));
  tabs($('lfo-tabs'), 'lfo', (n) => displays.setLfo(n));

  // ---------- pages ----------
  const showPage = (name) => {
    $('page-main').hidden = name !== 'main';
    $('page-layers').hidden = name !== 'layers';
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
    if (e.repeat || e.target.closest('input,select')) return;
    const i = QWERTY.indexOf(e.key.toLowerCase());
    if (i < 0) return;
    const n = kbdRange()[0] + 12 + i;
    const el = [...keys.children].find((k) => k.getAttribute('aria-label') === noteName(n));
    if (el) press(n, el);
  });
  window.addEventListener('keyup', (e) => {
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
    if (m.voices !== undefined) $('stat-voices').textContent = String(m.voices).padStart(2, '0') + ' / 32';
    if (m.cpu !== undefined) $('stat-cpu').textContent = m.cpu.toFixed(1) + '%';
  });

  // ---------- commands that need C++ (file dialogs, presets, sample analysis) ----------
  const ui = {
    kbdRange,
    async command(name, ...args) {
      const r = await bridge.command(name, ...args);
      if (r && r.reason === 'preview') toast(`“${name}” works inside the plugin`);
      else if (r && r.ok === false) toast(r.message || `“${name}” failed`);
      return r;
    }
  };
  const layers = setupLayers(bridge, ui);
  const octave = (d) => { baseOct = Math.max(-1, Math.min(6, baseOct + d)); buildKeys(); layers.renderMini(); };
  $('oct-down').onclick = () => octave(-1);
  $('oct-up').onclick = () => octave(1);

  document.querySelectorAll('[data-action]').forEach((b) => b.addEventListener('click', () => {
    const a = b.dataset.action;
    if (layers.actions[a]) layers.actions[a]();
    else ui.command(a);
  }));

  if (!bridge.inPlugin) console.info('mtyaSampler UI: browser preview mode (no plugin connected).');
}

boot().catch((e) => {
  console.error(e);
  document.body.insertAdjacentHTML('beforeend', `<pre style="position:fixed;left:8px;bottom:8px;color:#E8664A;font:12px monospace;white-space:pre-wrap">${String(e.stack || e)}</pre>`);
});
