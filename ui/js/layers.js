// Multi-sample layer model UI: the Layers page (map, table, round robin, velocity curve, inspector)
// plus the compact layer list, zone bar and sample name on the Main page.
//
// The instrument (all layers + global settings) lives in C++. The UI receives it as JSON, edits a copy
// and sends the whole thing back with bridge.setInstrument(). See docs/ui-spec.md for the layer schema.

const $ = (id) => document.getElementById(id);
const NOTE = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B'];
export const noteName = (n) => (n < 0 ? '—' : NOTE[n % 12] + (Math.floor(n / 12) - 2)); // C3 = 60
export function parseNote(str) {
  const s = String(str).trim().replace('#', '♯').toUpperCase().replace('♯', '♯');
  if (s === '' || s === '—' || s === '-') return -1;
  if (/^-?\d+$/.test(s)) return Math.max(0, Math.min(127, parseInt(s, 10)));
  const m = /^([A-G])(♯?)(-?\d)$/.exec(s);
  if (!m) return null;
  const i = NOTE.indexOf(m[1] + m[2]);
  if (i < 0) return null;
  const n = (parseInt(m[3], 10) + 2) * 12 + i;
  return n >= 0 && n <= 127 ? n : null;
}

const KEY_LO = 24, KEY_SPAN = 96;   // map shows C0..B7
const rnd = (i, k) => Math.abs(Math.sin(i * k) * 43758.5453) % 1;
const clone = (o) => JSON.parse(JSON.stringify(o));

// A "samples" entry is a file name or { file, start, end, loopStart, loopEnd }
export const fileOf = (entry) => (entry && typeof entry === 'object' ? entry.file : entry) || '';
export const baseName = (entry) => String(fileOf(entry)).split(/[\\/]/).pop();

export function setupLayers(bridge, ui) {
  let inst = bridge.getInstrument();
  let sel = Math.min(4, inst.layers.length - 1);
  let group = null;   // null = first group
  let rrPos = 0;
  let editing = false; // while a zone is dragged we redraw locally only

  const layers = () => inst.layers;
  const cur = () => layers()[sel];
  const colorOf = (l) => {
    const groups = [...new Set(layers().map((x) => x.group))];
    const gi = groups.indexOf(l.group);
    return ['var(--acc)', 'var(--mod)', '#9AA0A6', '#E08AC4', '#B9D35A'][gi % 5];
  };
  const commit = async () => { await bridge.setInstrument(clone(inst)); };
  // Two clicks on the same thing within 400 ms (the first click re-draws the map, so the browser's
  // own dblclick can't be relied on) open that sample in the editor on the Main page
  let lastTap = { key: '', t: 0 };
  const doubleTap = (key) => {
    const now = performance.now(), hit = lastTap.key === key && now - lastTap.t < 400;
    lastTap = { key: hit ? '' : key, t: now };
    return hit;
  };
  const openInEditor = () => { if (ui.openEditor) ui.openEditor(); };
  const select = (i) => { sel = Math.max(0, Math.min(layers().length - 1, i)); rrPos = 0; renderAll(); };

  // ---------- Main page bits ----------
  function renderMini() {
    $('layer-count').textContent = layers().length;
    const list = $('mini-layers');
    list.innerHTML = '';
    layers().forEach((l, i) => {
      const row = document.createElement('div');
      row.className = 'mrow' + (i === sel ? ' on' : '');
      row.innerHTML =
        `<button class="pick" aria-pressed="${i === sel}"><span class="badge">${String(i + 1).padStart(2, '0')}</span>` +
        `<span class="txt"><b></b><span class="meta" style="font-size:10px">${noteName(l.lo)}–${noteName(l.hi)} · v${l.vlo}–${l.vhi}</span></span></button>` +
        `${l.engine && l.engine.own ? '<span class="own-badge" title="Has its own sample settings">OWN</span>' : ''}` +
        `<span class="meta" style="font-size:10px">${l.samples.length > 1 ? 'RR×' + l.samples.length : ''}</span>` +
        `<button class="ms" aria-label="Mute ${l.name}" aria-pressed="${!!l.muted}">M</button>` +
        `<button class="ms solo" aria-label="Solo ${l.name}" aria-pressed="${!!l.solo}">S</button>`;
      row.querySelector('b').textContent = l.name;
      row.querySelector('.pick').onclick = () => select(i);
      const [m, s] = row.querySelectorAll('.ms');
      m.onclick = () => { l.muted = !l.muted; commit(); };
      s.onclick = () => { l.solo = !l.solo; commit(); };
      list.appendChild(row);
    });
    const l = cur();
    if (ui.layerChanged) ui.layerChanged(l);   // the sample editor shows this layer (file name, waveform, points)
    $('sel-root').textContent = l ? noteName(l.root) : '—';
    if (!l) { $('zone-z').style.width = '0'; $('zone-r').style.left = '-10%'; return; }   // an empty instrument (e.g. a fresh Init)
    // zone bar spans the on-screen keyboard (ui.kbdRange)
    const [k0, k1] = ui.kbdRange();
    const span = k1 - k0 + 1;
    const a = Math.max(l.lo, k0), b = Math.min(l.hi, k1);
    $('zone-z').style.left = Math.max(0, (a - k0) / span * 100) + '%';
    $('zone-z').style.width = Math.max(0, (b - a + 1) / span * 100) + '%';
    $('zone-r').style.left = Math.max(0, Math.min(100, (l.root - k0 + 0.5) / span * 100)) + '%';
  }

  // ---------- group filter ----------
  function renderGroups() {
    const groups = [...new Set(layers().map((l) => l.group))];
    if (group !== 'All' && !groups.includes(group)) group = groups[0] ?? 'All';
    const el = $('group-filter');
    el.innerHTML = '';
    [['All', layers().length], ...groups.map((g) => [g, layers().filter((l) => l.group === g).length])].forEach(([g, n]) => {
      const b = document.createElement('button');
      b.setAttribute('aria-pressed', String(g === group));
      b.innerHTML = `<span></span><span class="mono" style="font-size:10px;opacity:.8">${n}</span>`;
      b.firstChild.textContent = g;
      b.onclick = () => { group = g; renderGrid(); renderGroups(); };
      el.appendChild(b);
    });
  }

  // ---------- key / velocity grid ----------
  function zoneBox(l) {
    return {
      left: (l.lo - KEY_LO) / KEY_SPAN * 100, width: (l.hi - l.lo + 1) / KEY_SPAN * 100,
      top: (127 - l.vhi) / 128 * 100, height: (l.vhi - l.vlo + 1) / 128 * 100
    };
  }
  function placeZone(el, l) {
    const b = zoneBox(l);
    Object.assign(el.style, { left: b.left + '%', width: b.width + '%', top: b.top + '%', height: b.height + '%' });
    el.querySelector('span').textContent = `${noteName(l.lo)}–${noteName(l.hi)} · v${l.vlo}–${l.vhi}` + (l.samples.length > 1 ? ' · RR×' + l.samples.length : '');
  }
  function renderGrid() {
    const grid = $('grid');
    grid.innerHTML = '';
    for (let o = 1; o < 8; o++) grid.insertAdjacentHTML('beforeend', `<div class="gl" style="top:0;bottom:0;width:1px;left:${o * 12.5}%"></div>`);
    [25, 50, 75].forEach((t) => grid.insertAdjacentHTML('beforeend', `<div class="gl" style="left:0;right:0;height:1px;top:${t}%"></div>`));
    // draw big zones first so small ones stay clickable on top
    const order = layers().map((l, i) => i).sort((a, b) => {
      const A = layers()[a], B = layers()[b];
      return (B.hi - B.lo) * (B.vhi - B.vlo) - (A.hi - A.lo) * (A.vhi - A.vlo);
    });
    order.forEach((i) => {
      const l = layers()[i];
      if (group !== 'All' && l.group !== group) return;
      const on = i === sel;
      const c = colorOf(l);
      const z = document.createElement('button');
      z.className = 'zone' + (on ? ' on' : '');
      z.setAttribute('aria-label', 'Select ' + l.name + '. Drag to move, drag edges to resize.');
      z.setAttribute('aria-pressed', String(on));
      const velShade = l.vhi > 100 ? 45 : l.vhi > 52 ? 30 : 18;
      z.style.background = on ? `color-mix(in srgb, ${c} 80%, transparent)` : `color-mix(in srgb, ${c} ${velShade}%, transparent)`;
      z.style.borderColor = on ? 'var(--text)' : c;
      z.innerHTML = '<b></b><span></span>';
      z.querySelector('b').textContent = l.name;
      placeZone(z, l);
      attachZoneDrag(z, i);
      grid.appendChild(z);
    });
  }
  function attachZoneDrag(z, i) {
    let start = null;
    z.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      if (doubleTap('zone' + i)) { select(i); openInEditor(); return; }
      if (i !== sel) { select(i); return; } // first click selects; drag the selected zone
      const r = z.getBoundingClientRect(), g = $('grid').getBoundingClientRect();
      const edge = 6;
      const mode = {
        l: e.clientX - r.left < edge, r: r.right - e.clientX < edge,
        t: e.clientY - r.top < edge, b: r.bottom - e.clientY < edge
      };
      if (!mode.l && !mode.r && !mode.t && !mode.b) mode.move = true;
      start = { x: e.clientX, y: e.clientY, mode, semi: g.width / KEY_SPAN, vel: g.height / 128, orig: clone(layers()[i]) };
      z.setPointerCapture(e.pointerId);
      editing = true;
    });
    z.addEventListener('pointermove', (e) => {
      const r = z.getBoundingClientRect();
      if (!start) { // cursor hint
        const edge = 6, h = e.clientX - r.left < edge || r.right - e.clientX < edge, v = e.clientY - r.top < edge || r.bottom - e.clientY < edge;
        z.style.cursor = i !== sel ? 'pointer' : h ? 'ew-resize' : v ? 'ns-resize' : 'move';
        return;
      }
      const dk = Math.round((e.clientX - start.x) / start.semi), dv = Math.round((start.y - e.clientY) / start.vel);
      const o = start.orig, l = layers()[i], m = start.mode;
      const ck = (n) => Math.max(0, Math.min(127, n));
      if (m.move) {
        const w = o.hi - o.lo, h = o.vhi - o.vlo;
        l.lo = ck(Math.min(o.lo + dk, 127 - w)); l.hi = l.lo + w;
        l.vlo = ck(Math.min(Math.max(o.vlo + dv, 0), 127 - h)); l.vhi = l.vlo + h;
        l.root = ck(o.root + (l.lo - o.lo));
      } else {
        if (m.l) l.lo = Math.min(ck(o.lo + dk), l.hi);
        if (m.r) l.hi = Math.max(ck(o.hi + dk), l.lo);
        if (m.t) l.vhi = Math.max(ck(o.vhi + dv), l.vlo);
        if (m.b) l.vlo = Math.min(ck(o.vlo + dv), l.vhi);
      }
      placeZone(z, l);
      renderStrip();
    });
    const end = () => { if (!start) return; start = null; editing = false; commit(); };
    z.addEventListener('pointerup', end);
    z.addEventListener('pointercancel', end);
    z.addEventListener('keydown', (e) => { // keyboard nudging of the selected zone
      if (i !== sel) return;
      const l = layers()[i]; let changed = true;
      if (e.key === 'ArrowLeft' && l.lo > 0) { l.lo--; l.hi--; l.root--; }
      else if (e.key === 'ArrowRight' && l.hi < 127) { l.lo++; l.hi++; l.root++; }
      else if (e.key === 'ArrowUp' && l.vhi < 127) { l.vlo++; l.vhi++; }
      else if (e.key === 'ArrowDown' && l.vlo > 0) { l.vlo--; l.vhi--; }
      else changed = false;
      if (changed) { e.preventDefault(); commit(); }
    });
  }

  // ---------- keyboard strip under the grid ----------
  function renderStrip() {
    const l = cur();
    const strip = $('strip');
    if (!strip.childElementCount) {
      const WN = [0, 2, 4, 5, 7, 9, 11], bw = 100 / 56;
      let html = '';
      for (let o = 0; o < 8; o++) for (let w = 0; w < 7; w++) html += `<div class="w" data-n="${KEY_LO + o * 12 + WN[w]}"></div>`;
      for (let o = 0; o < 8; o++) for (let w = 0; w < 7; w++) if ([0, 1, 3, 4, 5].includes(w)) {
        const i = o * 7 + w;
        html += `<div class="b" data-n="${KEY_LO + o * 12 + WN[w] + 1}" style="left:${((i + 1) * bw - bw * 0.31).toFixed(3)}%;width:${(bw * 0.62).toFixed(3)}%"></div>`;
      }
      strip.innerHTML = html;
      $('octs').innerHTML = [...Array(8)].map((_, o) => `<span style="left:${o * 12.5}%">C${o}</span>`).join('');
    }
    strip.querySelectorAll('[data-n]').forEach((k) => {
      const n = +k.dataset.n;
      k.classList.toggle('in', !!l && n >= l.lo && n <= l.hi);
      k.classList.toggle('root', !!l && n === l.root);
    });
  }

  // ---------- table ----------
  function renderTable() {
    const tb = $('layer-rows');
    tb.innerHTML = '';
    layers().forEach((l, i) => {
      const tr = document.createElement('tr');
      if (i === sel) tr.className = 'on';
      const pan = l.pan === 0 ? 'C' : (l.pan < 0 ? 'L' : 'R') + Math.abs(l.pan);
      tr.innerHTML =
        `<td style="color:var(--dim)">${String(i + 1).padStart(2, '0')}</td><td><button class="name" aria-pressed="${i === sel}"></button></td>` +
        `<td class="grp"></td><td>${noteName(l.lo)} – ${noteName(l.hi)}</td><td>${noteName(l.root)}</td><td>${l.vlo} – ${l.vhi}</td>` +
        `<td>${l.samples.length > 1 ? '×' + l.samples.length : '—'}</td><td>${l.volume.toFixed(1)} dB</td><td>${pan}</td><td>${l.output}</td>` +
        `<td><div style="display:flex;gap:4px"><button class="ms" aria-label="Mute ${l.name}" aria-pressed="${!!l.muted}">M</button><button class="ms solo" aria-label="Solo ${l.name}" aria-pressed="${!!l.solo}">S</button></div></td>`;
      tr.querySelector('.name').textContent = l.name;
      tr.querySelector('.grp').textContent = l.group;
      tr.querySelector('.name').onclick = () => { if (doubleTap('row' + i)) openInEditor(); select(i); };
      tr.title = 'Double-click to edit this layer\'s sample on the Main page';
      tr.ondblclick = (e) => { if (!e.target.closest('.ms')) { select(i); openInEditor(); } };
      const [m, s] = tr.querySelectorAll('.ms');
      m.onclick = () => { l.muted = !l.muted; commit(); };
      s.onclick = () => { l.solo = !l.solo; commit(); };
      tb.appendChild(tr);
    });
    const row = tb.children[sel];
    if (row) row.scrollIntoView({ block: 'nearest' });
  }

  // ---------- round robin ----------
  function miniWave(seed) {
    let t = ''; const b = [];
    for (let i = 0; i <= 80; i++) {
      const x = i / 80;
      const env = (1 - Math.exp(-x * 40)) * Math.exp(-x * (1.8 + (seed % 3) * 0.6)) * 0.9 + 0.04;
      const a = Math.min(0.95, env * (0.4 + 0.6 * rnd(i + seed * 13, 12.9898)));
      const X = (x * 200).toFixed(1);
      t += (i ? ' L' : 'M') + X + ' ' + (22 - a * 20).toFixed(1);
      b.push(X + ' ' + (22 + a * 20).toFixed(1));
    }
    return t + ' L' + b.reverse().join(' L') + ' Z';
  }
  const RR_NAMES = { off: 'Off', cycle: 'Cycle', random: 'Random', norepeat: 'No Repeat' };
  function renderRR() {
    const l = cur(); if (!l) return;
    const n = l.samples.length;
    $('rr-summary').textContent = `${l.name} · ${n} ${n > 1 ? 'variations · ' + RR_NAMES[l.rrMode] : 'sample'}`;
    const g = $('rr-grid');
    g.innerHTML = '';
    l.samples.slice(0, 8).forEach((file, r) => {
      const d = document.createElement('div');
      d.className = 'rrslot' + (r === rrPos % n ? ' on' : '');
      d.innerHTML = `<div class="top-line"><b>RR ${r + 1}</b>${r === rrPos % n ? '<span class="last">last played</span>' : ''}</div>` +
        `<svg viewBox="0 0 200 44" preserveAspectRatio="none" aria-hidden="true"><path d="${miniWave(sel * 7 + r)}"/></svg><span class="meta"></span>`;
      d.querySelector('.meta').textContent = baseName(file);
      // Click a variation to edit its start, end and loop points on the Main page
      d.title = 'Click to pick RR ' + (r + 1) + ' for the sample editor · double-click to open it';
      d.style.cursor = 'pointer';
      d.onclick = () => { if (ui.selectVariation) ui.selectVariation(r); if (doubleTap('rr' + r)) openInEditor(); };
      g.appendChild(d);
    });
  }
  $('rr-audition').onclick = () => {
    const l = cur(); const n = l.samples.length;
    if (l.rrMode === 'off' || n < 2) rrPos = 0;
    else if (l.rrMode === 'cycle') rrPos = (rrPos + 1) % n;
    else { let k = Math.floor(Math.random() * n); if (l.rrMode === 'norepeat' && k === rrPos) k = (k + 1) % n; rrPos = k; }
    bridge.noteOn(l.root, Math.round((l.vlo + l.vhi) / 2));
    setTimeout(() => bridge.noteOff(l.root), 500);
    renderRR(); renderInspector();
  };

  // ---------- velocity curve ----------
  const CURVES = {
    linear: ['Lin', 'M0 100 L100 0', (x) => x],
    soft: ['Soft', 'M0 100 C20 40 50 10 100 0', (x) => Math.pow(x, 0.6)],
    hard: ['Hard', 'M0 100 C50 92 82 60 100 0', (x) => Math.pow(x, 1.8)],
    fixed: ['Fixed', 'M0 30 L100 30', () => 0.7]
  };
  function renderCurve() {
    const c = inst.velocityCurve || 'linear';
    const el = $('curve-seg');
    el.innerHTML = '';
    Object.entries(CURVES).forEach(([id, [label]]) => {
      const b = document.createElement('button');
      b.textContent = label;
      b.setAttribute('aria-pressed', String(id === c));
      b.onclick = () => { inst.velocityCurve = id; commit(); };
      el.appendChild(b);
    });
    $('vc-path').setAttribute('d', CURVES[c][1]);
    $('vc-out').textContent = Math.round(CURVES[c][2](96 / 127) * 127);
  }

  // ---------- inspector ----------
  const F = {
    note: (key, label) => ({ key, label, kind: 'note' }),
    int: (key, label, min, max, suffix = '') => ({ key, label, kind: 'int', min, max, suffix }),
    num: (key, label, min, max, suffix = '') => ({ key, label, kind: 'num', min, max, suffix }),
    sel: (key, label, options) => ({ key, label, kind: 'select', options }),
    text: (key, label) => ({ key, label, kind: 'text' })
  };
  const SECTIONS = [
    { title: 'MIDI Mapping', cols: 4, fields: [F.note('lo', 'Low Key'), F.note('hi', 'High Key'), F.note('root', 'Root'), F.int('keyTrack', 'Key Track %', 0, 200), F.int('vlo', 'Low Vel', 0, 127), F.int('vhi', 'High Vel', 0, 127), F.int('keyFade', 'Key Fade st', 0, 24), F.int('velFade', 'Vel Fade', 0, 64)], buttons: [['learnRange', 'Learn key range'], ['detectRoot', 'Detect root pitch']] },
    { title: 'Round Robin', seg: { key: 'rrMode', options: [['off', 'Off'], ['cycle', 'Cycle'], ['random', 'Random'], ['norepeat', 'No Rep.']] }, cols: 3, fields: [F.text('rrGroup', 'RR Group'), { key: '_rrpos', label: 'Position', kind: 'readonly' }, F.sel('rrReset', 'Reset', [['never', 'Never'], ['idle', '2 s idle'], ['transport', 'On play']])] },
    { title: 'Trigger', seg: { key: 'trigger', options: [['attack', 'Note On'], ['release', 'Release'], ['legato', 'Legato'], ['first', 'First']] }, cols: 3, fields: [F.num('releaseVolume', 'Release dB', -60, 12), F.int('releaseDecay', 'Rel. Decay ms', 0, 10000), F.int('velToVol', 'Vel → Vol %', 0, 100)] },
    { title: 'Voice', cols: 3, fields: [F.int('polyphony', 'Polyphony', 1, 128), F.int('choke', 'Choke Grp', 0, 16), F.sel('steal', 'Steal', [['oldest', 'Oldest'], ['quietest', 'Quietest'], ['lowest', 'Lowest'], ['highest', 'Highest']]), F.sel('voiceMode', 'Voice Mode', [['poly', 'Poly'], ['mono', 'Mono'], ['legato', 'Legato']]), F.int('glide', 'Glide ms', 0, 5000), F.text('group', 'Group')] },
    { title: 'Articulation & Output', cols: 3, fields: [F.note('keyswitch', 'Keyswitch'), F.text('articulation', 'Articulation'), F.sel('output', 'Output', [['Main', 'Main 1/2'], ['Bus 2', 'Bus 2'], ['Bus 3', 'Bus 3'], ['Bus 4', 'Bus 4']]), F.num('volume', 'Volume dB', -60, 12), F.int('pan', 'Pan', -50, 50)] }
  ];
  function renderInspector() {
    const l = cur();
    const box = $('inspector');
    if (!l) { box.innerHTML = '<p class="meta">No layers yet. Use Add or Import folder.</p>'; return; }
    box.innerHTML = '';
    const head = document.createElement('div');
    head.className = 'insp-head';
    head.innerHTML =
      `<span class="badge" style="background:${colorOf(l)}">${String(sel + 1).padStart(2, '0')}</span>` +
      `<div class="t"><input aria-label="Layer name"><span class="meta"></span></div>` +
      `<button class="icon-btn" aria-label="Previous layer"><svg width="12" height="12" viewBox="0 0 14 14" aria-hidden="true"><path d="M9 2 L4 7 L9 12" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></button>` +
      `<button class="icon-btn" aria-label="Next layer"><svg width="12" height="12" viewBox="0 0 14 14" aria-hidden="true"><path d="M5 2 L10 7 L5 12" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg></button>`;
    const nameIn = head.querySelector('input');
    nameIn.value = l.name;
    nameIn.onchange = () => { l.name = nameIn.value.trim() || l.name; commit(); };
    head.querySelector('.meta').textContent = `${l.group} · ${l.samples.length} file${l.samples.length === 1 ? '' : 's'}`;
    const [prev, next] = head.querySelectorAll('.icon-btn');
    prev.onclick = () => select((sel + layers().length - 1) % layers().length);
    next.onclick = () => select((sel + 1) % layers().length);
    box.appendChild(head);

    SECTIONS.forEach((sec, si) => {
      box.insertAdjacentHTML('beforeend', '<div class="sep"></div>');
      const s = document.createElement('div');
      s.className = 'sec';
      s.innerHTML = `<h3 class="h"></h3>`;
      s.firstChild.textContent = sec.title;
      if (sec.seg) {
        const seg = document.createElement('div');
        seg.className = 'seg fill';
        seg.setAttribute('role', 'group');
        seg.setAttribute('aria-label', sec.title + ' mode');
        sec.seg.options.forEach(([v, label]) => {
          const b = document.createElement('button');
          b.textContent = label;
          b.setAttribute('aria-pressed', String(l[sec.seg.key] === v));
          b.onclick = () => { l[sec.seg.key] = v; commit(); };
          seg.appendChild(b);
        });
        s.appendChild(seg);
      }
      const grid = document.createElement('div');
      grid.className = 'fields c' + sec.cols;
      sec.fields.forEach((f, fi) => {
        const id = `f-${si}-${fi}`;
        const wrap = document.createElement('div');
        wrap.className = 'field';
        wrap.innerHTML = `<label for="${id}"></label>`;
        wrap.firstChild.textContent = f.label;
        let input;
        if (f.kind === 'select') {
          input = document.createElement('select');
          f.options.forEach(([v, t]) => { const o = document.createElement('option'); o.value = v; o.textContent = t; input.appendChild(o); });
          input.value = l[f.key] ?? f.options[0][0];
        } else {
          input = document.createElement('input');
          input.type = 'text';
          input.spellcheck = false;
          if (f.kind === 'note') input.value = noteName(l[f.key]);
          else if (f.kind === 'readonly') { input.value = `${(rrPos % l.samples.length) + 1} / ${l.samples.length}`; input.readOnly = true; input.dataset.rrpos = '1'; }
          else input.value = l[f.key] ?? '';
          if (f.kind === 'int' || f.kind === 'num') input.inputMode = 'decimal';
        }
        input.id = id;
        input.onchange = () => {
          let v = input.value;
          if (f.kind === 'note') { v = parseNote(v); if (v === null) { input.classList.add('bad'); return; } }
          else if (f.kind === 'int' || f.kind === 'num') {
            v = f.kind === 'int' ? parseInt(v, 10) : parseFloat(v);
            if (Number.isNaN(v)) { input.classList.add('bad'); return; }
            v = Math.max(f.min, Math.min(f.max, v));
          } else if (f.kind === 'readonly') return;
          input.classList.remove('bad');
          l[f.key] = v;
          if (l.lo > l.hi) [l.lo, l.hi] = [l.hi, l.lo];
          if (l.vlo > l.vhi) [l.vlo, l.vhi] = [l.vhi, l.vlo];
          commit();
        };
        wrap.appendChild(input);
        grid.appendChild(wrap);
      });
      s.appendChild(grid);
      if (sec.buttons) {
        const row = document.createElement('div');
        row.className = 'btnrow';
        sec.buttons.forEach(([cmd, label]) => {
          const b = document.createElement('button');
          b.className = 'btn'; b.textContent = label;
          b.onclick = () => ui.command(cmd, cur()?.id);
          row.appendChild(b);
        });
        s.appendChild(row);
      }
      box.appendChild(s);
    });
  }

  // ---------- structural actions ----------
  function newLayer(i) {
    return {
      id: Date.now(), name: 'New Layer ' + (i + 1), group: cur()?.group ?? 'Group 1', lo: 24, hi: 119, root: 60, vlo: 0, vhi: 127,
      keyFade: 0, velFade: 0, keyTrack: 100, rrMode: 'off', rrGroup: '', rrReset: 'idle', samples: ['(drop a sample)'],
      volume: 0, pan: 0, output: 'Main', muted: false, solo: false, trigger: 'attack', releaseVolume: -6, releaseDecay: 600,
      velToVol: 80, polyphony: 16, choke: 0, steal: 'oldest', voiceMode: 'poly', glide: 0, keyswitch: -1, articulation: 'All'
    };
  }
  const actions = {
    addLayer: () => { inst.layers.push(newLayer(inst.layers.length)); sel = inst.layers.length - 1; commit(); },
    duplicateLayer: () => { if (!cur()) return; const c = clone(cur()); c.id = Date.now(); c.name += ' copy'; inst.layers.splice(sel + 1, 0, c); sel++; commit(); },
    deleteLayer: () => { if (!cur()) return; inst.layers.splice(sel, 1); sel = Math.max(0, sel - 1); commit(); }
  };

  function renderAll() {
    renderMini();
    if (editing) return;
    renderGroups(); renderGrid(); renderStrip(); renderTable(); renderRR(); renderCurve(); renderInspector();
  }
  // The engine reports where each layer's round robin is (plugin only)
  bridge.onMeters((m) => {
    const l = cur(), el = document.querySelector('[data-rrpos]');
    if (!m.rr || !l || !el || m.rr[l.id] === undefined) return;
    const n = l.samples.length;
    el.value = `${(m.rr[l.id] % n) + 1} / ${n}`;
  });
  bridge.onInstrument((next) => { inst = next; if (sel >= inst.layers.length) sel = Math.max(0, inst.layers.length - 1); renderAll(); });
  renderAll();

  // Show the layer a key plays (keyboard clicks): keep the current one if it covers the key, otherwise
  // prefer a layer in the same group, then any unmuted one.
  function selectForNote(note, vel = 100) {
    const plays = (l) => note >= l.lo && note <= l.hi && vel >= l.vlo && vel <= l.vhi && !l.muted;
    const c = cur();
    if (c && plays(c)) return;
    const all = layers();
    let i = all.findIndex((l) => plays(l) && c && l.group === c.group);
    if (i < 0) i = all.findIndex(plays);
    if (i >= 0) select(i);
  }

  return { actions, current: cur, renderMini, selectForNote, showGroup(g) { group = g; const i = layers().findIndex((l) => l.group === g); if (i >= 0) sel = i; renderAll(); } };
}
