// Create page: the "create your own samples" assistant.
// 1. The user picks an instrument preset and adjusts range, resolution, round robins, velocity layers, timing.
// 2. Start session → layers are added to the instrument (empty), and the plan is sent to the recorder engine.
// 3. For every take the engine announces the note, plays a reference tone, counts in, records the input,
//    trims it and saves a WAV. Each saved take is written into its layer's round-robin slot right away.

import { DEFAULTS, PRESETS, STEPS, buildPlan, layersForPlan, roots, fmtDuration, spokenNote } from './plan.js';
import { noteName, parseNote } from './layers.js';
import { downsample } from './mock-recorder.js';

const $ = (id) => document.getElementById(id);
const STORE_KEY = 'mtyaSampler.create.settings';
const clone = (o) => JSON.parse(JSON.stringify(o));

const ICONS = {
  voice: 'M10 3 a3 3 0 0 1 3 3 v4 a3 3 0 0 1 -6 0 v-4 a3 3 0 0 1 3 -3 Z M5 10 a5 5 0 0 0 10 0 M10 15 v3',
  guitar: 'M14 3 l3 3 -5 5 M12 8 a4 4 0 1 0 -1 6 a3 3 0 0 0 1 -6 Z',
  keys: 'M3 5 h14 v10 h-14 Z M7 5 v6 M10 5 v10 M13 5 v6',
  bass: 'M15 2 l3 3 -6 6 M11 9 a4 5 0 1 0 0 8 a3 4 0 0 0 0 -8 Z',
  strings: 'M6 17 L14 3 M9 3 c4 2 4 12 0 14 c-4 -2 -4 -12 0 -14 Z',
  synth: 'M3 6 h14 v8 h-14 Z M6 9 v2 M10 9 v2 M14 9 v2',
  drums: 'M3 8 a7 3 0 0 0 14 0 a7 3 0 0 0 -14 0 v5 a7 3 0 0 0 14 0 v-5 M5 2 l3 5 M15 2 l-3 5',
  custom: 'M4 6 h12 M4 10 h12 M4 14 h12 M8 4 v4 M13 8 v4 M7 12 v4'
};

export function setupCreate(bridge, ui) {
  let s = { ...DEFAULTS };
  try { Object.assign(s, JSON.parse(localStorage.getItem(STORE_KEY) || '{}')); } catch { /* storage unavailable */ }
  let plan = null;          // active plan
  let mapKey = '';          // which settings the take-map preview shows
  let status = [];          // per take: '', 'done', 'bad', 'skip'
  let recent = [];
  let st = { status: 'idle', phase: 'idle', take: 0, total: 0 };
  let spokenFor = '';       // avoid repeating speech

  const save = () => { try { localStorage.setItem(STORE_KEY, JSON.stringify(s)); } catch { /* ignore */ } };
  const running = () => ['running', 'paused', 'review'].includes(st.status);

  // ---------- setup form ----------
  const grid = $('inst-grid');
  Object.entries(PRESETS).forEach(([id, p]) => {
    const b = document.createElement('button');
    b.type = 'button'; b.className = 'inst'; b.dataset.inst = id;
    b.innerHTML = `<svg viewBox="0 0 20 20" aria-hidden="true"><path d="${ICONS[id]}" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg><span></span>`;
    b.querySelector('span').textContent = p.label;
    b.onclick = () => { const { label, ...vals } = p; s = { ...s, ...vals, inst: id }; if (id !== 'custom' && (!s.name || Object.values(PRESETS).some((q) => s.name === 'My ' + q.label))) s.name = 'My ' + label; render(); };
    grid.appendChild(b);
  });

  const segs = {
    step: STEPS,
    order: [['note', 'By note'], ['passes', 'RR passes']],
    direction: [['up', 'Low → high'], ['down', 'High → low']]
  };
  document.querySelectorAll('#page-create [data-seg]').forEach((el) => {
    const key = el.dataset.seg;
    segs[key].forEach(([v, label]) => {
      const b = document.createElement('button');
      b.type = 'button'; b.textContent = label; b.dataset.v = v;
      b.onclick = () => { s[key] = typeof s[key] === 'number' ? Number(v) : v; markCustom(key); render(); };
      el.appendChild(b);
    });
  });

  const PRESET_KEYS = ['low', 'high', 'step', 'rr', 'vel', 'maxSec', 'tone', 'midi'];
  const markCustom = (key) => { if (PRESET_KEYS.includes(key) && s.inst !== 'custom') { const p = PRESETS[s.inst]; if (p[key] !== undefined && p[key] !== s[key]) s.inst = 'custom'; } };

  document.querySelectorAll('#setup-form [data-key]').forEach((el) => {
    const key = el.dataset.key, kind = el.dataset.kind;
    el.addEventListener('change', () => {
      if (el.type === 'checkbox') s[key] = el.checked;
      else if (el.tagName === 'SELECT') s[key] = el.value;
      else if (kind === 'note') { const n = parseNote(el.value); if (n === null || n < 0) { el.classList.add('bad'); return; } s[key] = n; }
      else if (kind === 'int' || kind === 'num') {
        let v = kind === 'int' ? parseInt(el.value, 10) : parseFloat(el.value);
        if (Number.isNaN(v)) { el.classList.add('bad'); return; }
        s[key] = Math.max(+el.dataset.min, Math.min(+el.dataset.max, v));
      } else s[key] = el.value;
      el.classList.remove('bad');
      if (key === 'monitor') bridge.recorder('monitor', s.monitor);
      markCustom(key); render();
    });
  });
  document.querySelectorAll('#setup-form [data-step]').forEach((b) => {
    const [key, d] = b.dataset.step.split(':');
    b.onclick = () => {
      const input = document.querySelector(`#setup-form [data-key="${key}"]`);
      s[key] = Math.max(+input.dataset.min, Math.min(+input.dataset.max, s[key] + Number(d)));
      markCustom(key); render();
    };
  });

  function renderRangeKbd() {
    const el = $('range-kbd');
    const r = new Set(roots(s));
    const lo = Math.min(s.low, s.high), hi = Math.max(s.low, s.high);
    const WN = [0, 2, 4, 5, 7, 9, 11], first = 24, oct = 8, bw = 100 / (oct * 7);
    let html = '';
    for (let o = 0; o < oct; o++) for (let w = 0; w < 7; w++) {
      const n = first + o * 12 + WN[w];
      html += `<div class="w${n >= lo && n <= hi ? ' in' : ''}${r.has(n) ? ' root' : ''}"></div>`;
    }
    for (let o = 0; o < oct; o++) for (let w = 0; w < 7; w++) if ([0, 1, 3, 4, 5].includes(w)) {
      const i = o * 7 + w, n = first + o * 12 + WN[w] + 1;
      html += `<div class="b${n >= lo && n <= hi ? ' in' : ''}${r.has(n) ? ' root' : ''}" style="left:${((i + 1) * bw - bw * 0.31).toFixed(3)}%;width:${(bw * 0.62).toFixed(3)}%"></div>`;
    }
    el.innerHTML = html;
  }

  function render() {
    save();
    document.querySelectorAll('#inst-grid .inst').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.inst === s.inst)));
    document.querySelectorAll('#page-create [data-seg]').forEach((el) => {
      el.querySelectorAll('button').forEach((b) => b.setAttribute('aria-pressed', String(String(s[el.dataset.seg]) === b.dataset.v)));
    });
    document.querySelectorAll('#setup-form [data-key]').forEach((el) => {
      if (document.activeElement === el) return;
      const v = s[el.dataset.key];
      if (el.type === 'checkbox') el.checked = !!v;
      else if (el.dataset.kind === 'note') el.value = noteName(v);
      else el.value = v ?? '';
    });
    const toneBox = document.querySelector('#setup-form [data-key="tone"]');
    toneBox.disabled = !s.pitched || s.midi;
    // MIDI mode plays the notes itself: the prompt, tone and count-in settings don't apply
    ['voice', 'countBeats', 'bpm', 'toneSec'].forEach((k) => { document.querySelector(`#setup-form [data-key="${k}"]`).disabled = s.midi; });
    ['midiChannel', 'holdSec', 'gapSec'].forEach((k) => { document.querySelector(`#setup-form [data-key="${k}"]`).disabled = !s.midi; });
    $('midi-hint').hidden = !s.midi;
    renderRangeKbd();
    const p = buildPlan(s);
    $('range-info').textContent = `${noteName(Math.min(s.low, s.high))} to ${noteName(Math.max(s.low, s.high))} · ${p.roots.length} ${s.pitched ? 'notes recorded' : 'sounds'}, each covering ${s.step === 1 ? '1 key' : 'up to ' + s.step + ' keys'}`;
    $('plan-takes').textContent = `${p.takes.length} takes`;
    $('plan-detail').textContent = `${p.roots.length} × ${s.vel} velocity × ${s.rr} RR · up to ~${fmtDuration(p.estimateSec)}`;
    $('c-folder').placeholder = 'Documents/mtyaSampler/Recordings/' + (s.name || 'Session');
    // Outside a session the take map previews the current settings
    const key = JSON.stringify(p.takes.map((t) => [t.note, t.v, t.rr]));
    if (!running() && key !== mapKey) { mapKey = key; plan = null; status = []; renderMap(); }
  }

  // ---------- session ----------
  async function start() {
    if (running()) return;
    plan = buildPlan(s);
    if (!plan.takes.length) { ui.toast('Nothing to record: check the note range.'); return; }
    if (plan.takes.length > 2000) { ui.toast('That is over 2000 takes. Reduce the range, round robins or velocity layers.'); return; }
    status = Array(plan.takes.length).fill('');
    recent = [];
    renderRecent();
    // add (empty) layers for this session to the instrument
    const inst = clone(bridge.getInstrument());
    if (s.replace) inst.layers = [];
    else inst.layers = inst.layers.filter((l) => l.group !== s.name);
    inst.layers.push(...layersForPlan(plan));
    await bridge.setInstrument(inst);
    plan.layerIds = inst.layers.filter((l) => l.group === s.name).map((l) => l.id);
    if (ui.showGroup) ui.showGroup(s.name);
    spokenFor = '';
    await bridge.recorder('start', plan);
  }

  function onTake(t) {
    if (!plan) return;
    const tk = plan.takes[t.index];
    if (t.skipped) { status[t.index] = 'skip'; renderMap(); return; }
    status[t.index] = t.empty || t.clipped || t.error ? 'bad' : 'done';
    if (t.error) ui.toast(t.error);
    if (t.empty) ui.toast('Nothing heard. Check the input and the start level.');
    // put the file into its layer's round-robin slot
    if (t.file && !t.empty && !t.error) {
      const inst = clone(bridge.getInstrument());
      const layer = inst.layers.find((l) => l.id === plan.layerIds[tk.zone]);
      if (layer) { layer.samples[tk.rr] = t.file; bridge.setInstrument(inst); }
    }
    recent.unshift({ ...t, take: tk });
    recent = recent.slice(0, 12);
    renderRecent(); renderMap();
  }

  function finish() {
    // drop empty RR slots / layers left by skipped takes
    if (!plan) return;
    const inst = clone(bridge.getInstrument());
    inst.layers = inst.layers
      .map((l) => (plan.layerIds.includes(l.id) ? { ...l, samples: l.samples.filter(Boolean) } : l))
      .filter((l) => !plan.layerIds.includes(l.id) || l.samples.length);
    bridge.setInstrument(inst);
    const n = status.filter((x) => x === 'done' || x === 'bad').length;
    ui.toast(`Session finished: ${n} sample${n === 1 ? '' : 's'} saved and mapped.`);
  }

  // ---------- speech (used when the plugin has no voice pack, and in the browser preview) ----------
  function speak(text) {
    if (!s.voice || !('speechSynthesis' in window)) return;
    try {
      speechSynthesis.cancel();
      const u = new SpeechSynthesisUtterance(text);
      u.rate = 1.05;
      speechSynthesis.speak(u);
    } catch { /* speech not available */ }
  }

  // ---------- engine state → view ----------
  const PH_NAMES = { announce: 'Listen', tone: 'Reference tone', countin: 'Get ready', record: 'Play now', save: 'Saving' };
  function onState(next) {
    const prevStatus = st.status;
    st = next;
    const tk = plan?.takes[st.take];
    const active = running();
    $('setup-form').disabled = active;
    $('setup-lock').hidden = !active;
    $('rec-start').disabled = active;
    $('rec-pause').disabled = !(st.status === 'running' || st.status === 'paused');
    $('rec-pause').textContent = st.status === 'paused' ? 'Resume' : 'Pause';
    $('rec-retake').disabled = !active || st.lastSaved < 0;
    $('rec-skip').disabled = !(st.status === 'running' || st.status === 'paused');
    $('rec-stop').disabled = !active;
    $('rec-next').hidden = st.status !== 'review';
    $('rec-next').disabled = st.status !== 'review';

    // phases strip
    const order = ['announce', 'tone', 'countin', 'record', 'save'];
    const pi = order.indexOf(st.phase);
    document.querySelectorAll('#phases li').forEach((li) => {
      const i = order.indexOf(li.dataset.phase);
      li.classList.toggle('now', i === pi);
      li.classList.toggle('past', pi >= 0 && i < pi);
      if (li.dataset.phase === 'tone') li.hidden = !!plan && !plan.timing.toneSec;
    });

    // prompt
    if (tk && active) {
      $('p-label').textContent = st.status === 'paused' ? 'Paused' : st.status === 'review' ? 'Take saved. Listen back, then keep or redo' : (PH_NAMES[st.phase] || '');
      $('p-note').textContent = plan.settings.pitched ? noteName(tk.note) : 'Sound ' + (tk.rootIndex + 1);
      const bits = [];
      if (plan.settings.vel > 1) bits.push(tk.velLabel);
      if (plan.settings.rr > 1) bits.push(`Round robin ${tk.rr + 1} of ${plan.settings.rr}`);
      bits.push(`Take ${st.take + 1} of ${st.total}`);
      $('p-sub').textContent = bits.join(' · ');
    } else if (st.status === 'done') {
      $('p-label').textContent = 'All done'; $('p-note').textContent = '✓';
      $('p-sub').textContent = 'Every take is in the layer map. Open the Layers page to check the mapping.';
    } else {
      $('p-label').textContent = 'Ready when you are'; $('p-note').textContent = '—';
      $('p-sub').textContent = 'Choose your settings, then press Start session.';
    }

    // countdown
    const c = $('p-count');
    const label = st.phase === 'countin' ? String(st.count) : st.phase === 'record' ? 'GO' : '';
    if (c.textContent !== label) { c.textContent = label; c.classList.remove('beat'); void c.offsetWidth; if (label) c.classList.add('beat'); }
    c.classList.toggle('go', st.phase === 'record');

    // speech prompts
    if (st.voiceInUi && active && st.status === 'running' && tk) {
      const key = st.take + ':' + st.phase + ':' + (st.phase === 'countin' ? st.count : '');
      if (key !== spokenFor) {
        spokenFor = key;
        if (st.phase === 'announce') speak(tk.say);
        else if (st.phase === 'countin' && st.count > 0 && st.count <= 3) speak(String(st.count));
      }
    }

    // waveform + meter
    const pk = st.peaks || [];
    const maxSec = st.maxSec || plan?.settings.maxSec || 1;
    const filled = st.phase === 'record' ? Math.min(1, st.recSec / maxSec) : (pk.length ? 1 : 0);
    $('t-wave').setAttribute('d', peaksToPath(pk, 400 * filled));
    $('t-max').setAttribute('x1', 400); $('t-max').setAttribute('x2', 400);
    $('rec-badge').hidden = st.phase !== 'record';
    $('rec-time').textContent = (st.recSec || 0).toFixed(1) + ' s';
    const db = st.inDb ?? -90;
    $('in-fill').style.width = Math.max(0, Math.min(100, (db + 60) / 60 * 100)) + '%';
    $('in-db').textContent = db <= -89 ? '−∞ dB' : db.toFixed(1) + ' dB';
    $('in-thr').style.left = Math.max(0, Math.min(100, (s.thresholdDb + 60) / 60 * 100)) + '%';
    $('in-clip').classList.toggle('on', !!st.clip);

    const done = status.filter((x) => x).length;
    $('rec-progress-txt').textContent = `${st.saved ?? 0} saved · ${done} / ${st.total || 0} takes`;
    $('rec-progress').style.width = (st.total ? done / st.total * 100 : 0) + '%';

    if (prevStatus !== st.status || st.phase === 'announce') renderMap();
    if (st.status === 'done' && prevStatus !== 'done') { renderMap(); finish(); }
  }

  function peaksToPath(pk, width) {
    if (!pk.length || width <= 0) return '';
    let top = '', bot = [];
    pk.forEach((p, i) => {
      const x = (i / Math.max(1, pk.length - 1) * width).toFixed(1);
      const h = Math.min(1, p) * 46;
      top += (i ? ' L' : 'M') + x + ' ' + (50 - h).toFixed(1);
      bot.push(x + ' ' + (50 + h).toFixed(1));
    });
    return top + ' L' + bot.reverse().join(' L') + ' Z';
  }

  // ---------- take map: rows = velocity layers, columns = notes, small boxes = round robins ----------
  function renderMap() {
    const g = $('tm-grid');
    const p = plan || buildPlan(s);
    const cols = p.roots.length;
    g.style.gridTemplateColumns = `auto repeat(${cols}, minmax(${Math.max(20, 9 * p.settings.rr)}px, 1fr))`;
    let html = '<div></div>' + p.roots.map((r) => `<div class="tm-head">${p.settings.pitched ? noteName(r) : p.roots.indexOf(r) + 1}</div>`).join('');
    for (let v = p.settings.vel - 1; v >= 0; v--) {
      html += `<div class="tm-row">${p.zones.find((z) => z.v === v).velLabel}</div>`;
      for (const r of p.roots) {
        html += '<div class="tm-cell">';
        for (let rr = 0; rr < p.settings.rr; rr++) {
          const t = p.takes.find((x) => x.note === r && x.v === v && x.rr === rr);
          const cls = plan ? (running() && t.i === st.take ? 'cur' : status[t.i]) : '';
          html += `<button class="${cls}" data-take="${t.i}" aria-label="${noteName(r)} ${t.velLabel} round robin ${rr + 1}${cls ? ', ' + ({ done: 'saved', bad: 'needs checking', skip: 'skipped', cur: 'recording now' })[cls] : ''}"></button>`;
        }
        html += '</div>';
      }
    }
    g.innerHTML = html;
    g.querySelectorAll('[data-take]').forEach((b) => {
      b.onclick = () => { if (plan && running()) bridge.recorder('goto', +b.dataset.take); };
    });
  }

  function renderRecent() {
    const el = $('recent-list');
    if (!recent.length) { el.innerHTML = '<p class="meta empty">Saved takes show up here and go straight into the layer map.</p>'; return; }
    el.innerHTML = '';
    recent.forEach((r) => {
      const d = document.createElement('div');
      const bad = r.empty || r.clipped;
      d.className = 'rt' + (bad ? ' bad' : '');
      const what = plan?.settings.pitched ? noteName(r.take.note) : 'Sound ' + (r.take.rootIndex + 1);
      d.innerHTML = `<b></b><span class="rt-btns"><button class="btn play" style="height:22px" ${r.path && !r.empty ? '' : 'disabled'}>Play</button><button class="btn redo" style="height:22px">Redo</button></span><span class="meta"></span><span class="meta${bad ? ' warn' : ''}"></span><svg viewBox="0 0 120 26" preserveAspectRatio="none" aria-hidden="true"><path d="${miniPath(r.peaks || [])}"/></svg>`;
      d.querySelector('b').textContent = `${what}${plan?.settings.vel > 1 ? ' · ' + r.take.velLabel : ''}${plan?.settings.rr > 1 ? ' · RR ' + (r.take.rr + 1) : ''}`;
      const metas = d.querySelectorAll('.meta');
      metas[0].textContent = r.file || r.take.file;
      metas[1].textContent = r.empty ? 'Nothing heard: redo' : r.clipped ? `Clipped (${r.peakDb} dB): redo` : `${r.lengthSec} s · peak ${r.peakDb} dB`;
      const redo = d.querySelector('.redo'), play = d.querySelector('.play');
      redo.onclick = () => bridge.recorder('goto', r.index);
      redo.setAttribute('aria-label', 'Redo ' + what);
      // Listen back: plays the saved WAV through the plugin's output
      play.setAttribute('aria-label', 'Play ' + what);
      play.onclick = () => ui.command('previewFile', r.path);
      el.appendChild(d);
    });
  }
  function miniPath(pk) {
    pk = downsample(pk, 120);
    if (!pk.length) return '';
    let top = '', bot = [];
    pk.forEach((p, i) => { const x = (i / Math.max(1, pk.length - 1) * 120).toFixed(1); const h = Math.min(1, p) * 12; top += (i ? ' L' : 'M') + x + ' ' + (13 - h).toFixed(1); bot.push(x + ' ' + (13 + h).toFixed(1)); });
    return top + ' L' + bot.reverse().join(' L') + ' Z';
  }

  // ---------- buttons ----------
  $('rec-start').onclick = start;
  $('choose-folder').onclick = async () => {
    const r = await ui.command('chooseFolder', s.folder);
    if (r && r.ok && r.path) { s.folder = r.path; render(); }
  };
  $('rec-pause').onclick = () => bridge.recorder(st.status === 'paused' ? 'resume' : 'pause');
  $('rec-retake').onclick = () => bridge.recorder('retake');
  $('rec-skip').onclick = () => bridge.recorder('skip');
  $('rec-next').onclick = () => bridge.recorder('next');
  $('rec-stop').onclick = async () => { await bridge.recorder('stop'); st.status = 'idle'; finish(); onState({ ...st, status: 'idle', phase: 'idle' }); };

  bridge.onRecorderState(onState);
  bridge.onRecorderTake(onTake);
  render();
  renderMap();

  return {
    // Space = start / pause, used when the Create page is showing
    key(e) {
      if (e.key === ' ' && !e.target.closest('input,select,button')) {
        e.preventDefault();
        if (!running()) start(); else bridge.recorder(st.status === 'paused' ? 'resume' : 'pause');
        return true;
      }
      return false;
    },
    spokenNote
  };
}
