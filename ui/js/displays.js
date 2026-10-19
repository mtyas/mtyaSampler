// Live displays: waveform + markers, grain cloud, filter response, envelope and LFO shapes.
// Each one redraws when the parameters it depends on change.

const $ = (id) => document.getElementById(id);
const rnd = (i, k) => Math.abs(Math.sin(i * k) * 43758.5453) % 1;

// Placeholder waveform until the C++ side sends real peak data (see CLAUDE.md, "Waveform peaks").
export function wavePath(points = 360, w = 1000, mid = 100, up = 92, down = 88, seed = 12.9898) {
  let top = ''; const bot = [];
  for (let i = 0; i <= points; i++) {
    const x = i / points;
    const env = (1 - Math.exp(-x * 60)) * Math.exp(-x * 1.6) * 0.88 + 0.05;
    const amp = 0.55 + 0.45 * Math.abs(Math.sin(i * 0.37) + 0.5 * Math.sin(i * 0.11));
    const a = Math.min(0.95, env * (0.35 + 0.65 * rnd(i, seed)) * amp);
    const X = (x * w).toFixed(1);
    top += (i ? ' L' : 'M') + X + ' ' + (mid - a * up).toFixed(1);
    bot.push(X + ' ' + (mid + a * down).toFixed(1));
  }
  return top + ' L' + bot.reverse().join(' L') + ' Z';
}

export function peaksPath(peaks, w = 1000, mid = 100, h = 92) {
  // peaks: array of [min, max] in -1..1
  const n = peaks.length; if (!n) return '';
  let top = ''; const bot = [];
  peaks.forEach(([mn, mx], i) => {
    const X = (i / (n - 1) * w).toFixed(1);
    top += (i ? ' L' : 'M') + X + ' ' + (mid - mx * h).toFixed(1);
    bot.push(X + ' ' + (mid - mn * h).toFixed(1));
  });
  return top + ' L' + bot.reverse().join(' L') + ' Z';
}

export function setupDisplays(bridge) {
  const P = (id) => bridge.param(id);
  const val = (id) => P(id).getValue();
  const on = (ids, fn) => { ids.forEach((id) => P(id).onChange(fn)); fn(); };

  // ---------- waveform ----------
  $('w-path').setAttribute('d', wavePath());
  $('ruler').innerHTML = ['0.0s', '0.8s', '1.6s', '2.4s', '3.2s', '4.0s', '4.8s'].map((t) => `<span>${t}</span>`).join('');
  let slices = '';
  const drawWave = () => {
    const s = val('smp_start') * 10, e = Math.max(s, val('smp_end') * 10);
    const ls = val('loop_start') * 10, le = Math.max(ls, val('loop_end') * 10);
    const mode = val('play_mode');          // 0 one-shot 1 loop 2 ping-pong 3 slice 4 granular
    const loop = mode === 1 || mode === 2;
    $('w-pre').setAttribute('width', s);
    $('w-post').setAttribute('x', e); $('w-post').setAttribute('width', 1000 - e);
    $('w-se').setAttribute('d', `M${s} 0 V200 M${e} 0 V200`);
    $('w-loop').setAttribute('x', ls); $('w-loop').setAttribute('width', le - ls);
    $('w-loop').style.display = loop ? '' : 'none';
    $('w-looplines').setAttribute('d', loop ? `M${ls} 0 V200 M${le} 0 V200` : '');
    slices = '';
    if (mode === 3) for (let i = 1; i < 16; i++) { const x = (s + i * (e - s) / 16).toFixed(1); slices += `M${x} 0 V200 `; }
    $('w-slices').setAttribute('d', slices);
    const gp = val('grn_pos') * 10, gw = 20 + val('grn_spray') * 2.4;
    $('w-grain').style.display = mode === 4 ? '' : 'none';
    $('w-grain-win').setAttribute('x', gp - gw / 2); $('w-grain-win').setAttribute('width', gw);
    let g = '';
    const n = Math.round(4 + val('grn_density') / 4);
    for (let i = 0; i < n; i++) {
      const x = (gp - gw / 2 + rnd(i + 3, 78.233) * gw).toFixed(1);
      const y = 30 + rnd(i + 7, 39.425) * 120, len = 6 + val('grn_size') / 6;
      g += `M${x} ${y.toFixed(1)} L${x} ${(y + len).toFixed(1)} `;
    }
    $('w-grains').setAttribute('d', g);
    const place = (id, x, show = true) => { const m = $(id); m.style.left = (x / 10) + '%'; m.hidden = !show; };
    place('m-s', s); place('m-e', e); place('m-l1', ls, loop); place('m-l2', le, loop);
    const mg = $('m-g'); mg.hidden = mode !== 4; mg.style.left = Math.min(80, (gp + gw / 2) / 10 + 0.5) + '%';
    mg.textContent = 'Grain · pos ' + val('grn_pos').toFixed(1) + '%';
  };
  on(['smp_start', 'smp_end', 'loop_start', 'loop_end', 'play_mode', 'grn_pos', 'grn_spray', 'grn_density', 'grn_size'], drawWave);

  // ---------- grain cloud ----------
  $('g-wave').setAttribute('d', wavePath(130, 260, 32, 30, 30, 7.13));
  on(['grn_pos', 'grn_spray', 'grn_density', 'grn_size', 'grn_freeze'], () => {
    const cx = val('grn_pos') / 100 * 260, w = 12 + val('grn_spray') * 1.4;
    const n = Math.round(6 + val('grn_density') * 0.8);
    let d = '';
    for (let i = 0; i < n; i++) {
      const x = (cx - w / 2 + rnd(i + 1, 11.7) * w).toFixed(1);
      const y = (8 + rnd(i + 5, 3.91) * 48).toFixed(1);
      d += `M${x} ${y} l0.01 0 `;
    }
    $('g-cloud').setAttribute('d', d);
    $('g-cloud').setAttribute('stroke-width', (2 + val('grn_size') / 60).toFixed(1));
    $('g-cloud').setAttribute('opacity', val('grn_freeze') ? '1' : '.9');
    $('g-pos').setAttribute('x1', cx); $('g-pos').setAttribute('x2', cx);
  });

  // ---------- filter response (approximate magnitude, for display only) ----------
  on(['flt_type', 'flt_cutoff', 'flt_reso', 'flt_drive'], () => {
    const type = val('flt_type'), fc = val('flt_cutoff'), res = val('flt_reso') / 100;
    // Same as the engine up to 90 %; above that the filter self-oscillates (drawn as a very sharp peak)
    const Q = res <= 0.9 ? 0.707 + res * 9 : Math.min(40, 8.807 + (res - 0.9) * 300);
    let d = '';
    for (let i = 0; i <= 130; i++) {
      const x = i * 2, f = 20 * Math.pow(1000, x / 260), w = f / fc;
      const den = (q) => Math.sqrt((1 - w * w) ** 2 + (w / q) ** 2);
      let h;
      if (type === 0) h = (1 / den(Q)) * (1 / den(0.707));           // LP24
      else if (type === 1) h = 1 / den(Q);                           // LP12
      else if (type === 2) h = (w / Q) / den(Q) * Math.max(1, Q / 2); // BP
      else if (type === 3) h = (w * w) / den(Q);                     // HP
      else { const g = 0.2 + res * 0.75; h = Math.sqrt(1 + g * g + 2 * g * Math.cos(2 * Math.PI * f / fc)) / (1 + g); } // comb
      const db = 20 * Math.log10(Math.max(h, 1e-4));
      const y = Math.max(2, Math.min(63, 24 - db * 0.9));
      d += (i ? ' L' : 'M') + x + ' ' + y.toFixed(1);
    }
    $('f-curve').setAttribute('d', d);
    $('f-fill').setAttribute('d', d + ' L260 64 L0 64 Z');
  });

  // ---------- envelope (current tab) ----------
  let envN = 1;
  const drawEnv = () => {
    const n = (id) => P(`env${envN}_${id}`).getNorm();
    const a = 6 + 60 * n('attack'), dcy = 6 + 60 * n('decay'), r = 6 + 70 * n('release');
    const hold = 256 - 2 - a - dcy - r;
    const sy = 62 - 56 * (val(`env${envN}_sustain`) / 100);
    const x1 = 2 + a, x2 = x1 + dcy, x3 = x2 + Math.max(20, hold), x4 = Math.min(258, x3 + r);
    const d = `M2 62 L${x1.toFixed(1)} 6 Q${(x1 + dcy * 0.15).toFixed(1)} ${sy.toFixed(1)} ${x2.toFixed(1)} ${sy.toFixed(1)} L${x3.toFixed(1)} ${sy.toFixed(1)} Q${(x3 + r * 0.15).toFixed(1)} 62 ${x4.toFixed(1)} 62`;
    const color = envN === 3 ? 'var(--mod)' : 'var(--acc)';
    $('e-curve').setAttribute('d', d); $('e-curve').setAttribute('stroke', color);
    $('e-fill').setAttribute('d', d + ' Z'); $('e-fill').setAttribute('fill', color);
  };
  for (let i = 1; i <= 3; i++) ['attack', 'decay', 'sustain', 'release'].forEach((k) => P(`env${i}_${k}`).onChange(() => i === envN && drawEnv()));

  // ---------- LFO (current tab) ----------
  const SH = [0.6, -0.3, 0.9, -0.8, 0.2, -0.5, 0.75, -0.1];
  const shapes = [
    (t) => Math.sin(t * 2 * Math.PI),
    (t) => { const u = t % 1; return u < 0.25 ? u * 4 : u < 0.75 ? 2 - u * 4 : u * 4 - 4; },
    (t) => 1 - 2 * (t % 1),
    (t) => (t % 1) < 0.5 ? 1 : -1,
    (t) => SH[Math.floor(t * 4) % 8]
  ];
  let lfoN = 1;
  const drawLfo = () => {
    const f = shapes[val(`lfo${lfoN}_shape`)];
    const depth = val(`lfo${lfoN}_depth`) / 100, ph = val(`lfo${lfoN}_phase`) / 360;
    const fade = P(`lfo${lfoN}_fade`).getNorm();
    const cycles = 1 + Math.round(P(`lfo${lfoN}_rate`).getNorm() * 5);
    let d = '';
    for (let i = 0; i <= 260; i++) {
      const t = Math.min(i / 260 * cycles + ph, 999.9999);
      const env = fade > 0 ? Math.min(1, i / (260 * fade + 1)) : 1;
      d += (i ? ' L' : 'M') + i + ' ' + (32 - f(t) * 24 * (0.15 + 0.85 * depth) * env).toFixed(1);
    }
    $('l-curve').setAttribute('d', d);
  };
  for (let i = 1; i <= 4; i++) ['shape', 'rate', 'depth', 'phase', 'fade'].forEach((k) => P(`lfo${i}_${k}`).onChange(() => i === lfoN && drawLfo()));

  return {
    setEnv(n) { envN = n; drawEnv(); },
    setLfo(n) { lfoN = n; drawLfo(); },
    setPeaks(peaks) { $('w-path').setAttribute('d', peaksPath(peaks)); }
  };
}
