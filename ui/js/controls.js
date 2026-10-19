// Reusable controls bound to plugin parameters: knob, horizontal slider, segmented choice, toggle.
// Every control is keyboard accessible and talks to the bridge only through param wrappers.

export function fmt(spec, v) {
  const d = spec.decimals ?? 0;
  const sign = (x) => (spec.bipolar && x > 0 ? '+' : '');
  switch (spec.unit) {
    case 'Hz':
      if (v >= 1000) return (v / 1000).toFixed(v >= 10000 ? 1 : 2) + ' kHz';
      return (v < 10 ? v.toFixed(2) : v.toFixed(0)) + ' Hz';
    case 's': return v < 1 ? Math.round(v * 1000) + ' ms' : v.toFixed(2) + ' s';
    case 'ms': return v >= 1000 ? (v / 1000).toFixed(2) + ' s' : Math.round(v) + ' ms';
    case '%': return sign(v) + v.toFixed(d) + '%';
    case 'dB': return sign(v) + v.toFixed(d) + ' dB';
    case 'st': return (v > 0 ? '+' : '') + v.toFixed(d) + ' st';
    case 'ct': return (v > 0 ? '+' : '') + v.toFixed(d) + ' ct';
    case '°': return Math.round(v) + '°';
    case '/s': return v.toFixed(d) + '/s';
    case 'bit': return Math.round(v) + '-bit';
    default: return v.toFixed(d) + (spec.unit ? ' ' + spec.unit : '');
  }
}

const clamp01 = (x) => Math.max(0, Math.min(1, x));
const pt = (r, deg) => { const a = deg * Math.PI / 180; return [22 + r * Math.sin(a), 22 - r * Math.cos(a)]; };
function arc(a0, a1) {
  if (a1 - a0 < 1) a1 = a0 + 1;
  const [x0, y0] = pt(17, a0), [x1, y1] = pt(17, a1);
  return `M${x0.toFixed(2)} ${y0.toFixed(2)} A17 17 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
const TRACK = arc(-135, 135);

// Shared drag behaviour: vertical (knob) or horizontal (slider). Shift = fine.
function attachDrag(el, p, axis, pixels) {
  let start = 0, startN = 0, active = false;
  el.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    active = true; el.setPointerCapture(e.pointerId);
    start = axis === 'y' ? e.clientY : e.clientX;
    startN = p.getNorm();
    if (axis === 'x') { // click-to-position on sliders
      const r = el.getBoundingClientRect();
      startN = clamp01((e.clientX - r.left) / r.width);
      p.begin(); p.setNorm(startN);
    } else p.begin();
    e.preventDefault();
  });
  el.addEventListener('pointermove', (e) => {
    if (!active) return;
    const pos = axis === 'y' ? e.clientY : e.clientX;
    const delta = axis === 'y' ? start - pos : pos - start;
    const scale = (e.shiftKey ? 5 : 1) * pixels / appScale();
    p.setNorm(clamp01(startN + delta / scale));
  });
  const stop = () => { if (active) { active = false; p.end(); } };
  el.addEventListener('pointerup', stop);
  el.addEventListener('pointercancel', stop);
  el.addEventListener('wheel', (e) => {
    e.preventDefault();
    p.begin(); p.setNorm(clamp01(p.getNorm() + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 0.002 : 0.01))); p.end();
  }, { passive: false });
  el.addEventListener('dblclick', () => { p.begin(); p.setValue(p.spec.default); p.end(); });
  el.addEventListener('keydown', (e) => {
    // Stepped knobs (Tune, Fine, Bits…) move one step per press (ten with Shift); 1 % could round to nothing
    if (p.spec.step && (e.key.startsWith('Arrow'))) {
      const dir = e.key === 'ArrowUp' || e.key === 'ArrowRight' ? 1 : -1;
      const v = Math.max(p.spec.min, Math.min(p.spec.max, p.getValue() + dir * p.spec.step * (e.shiftKey ? 10 : 1)));
      e.preventDefault(); p.begin(); p.setValue(v); p.end();
      return;
    }
    const big = e.shiftKey ? 0.1 : 0.01;
    let n = null;
    if (e.key === 'ArrowUp' || e.key === 'ArrowRight') n = p.getNorm() + big;
    if (e.key === 'ArrowDown' || e.key === 'ArrowLeft') n = p.getNorm() - big;
    if (e.key === 'Home') n = 0;
    if (e.key === 'End') n = 1;
    if (n !== null) { e.preventDefault(); p.begin(); p.setNorm(clamp01(n)); p.end(); }
  });
}

// The app is drawn at 1280×920 and CSS-scaled; drag distances must use unscaled pixels.
let _scale = 1;
export const setAppScale = (s) => { _scale = s; };
const appScale = () => _scale;

function aria(el, p) {
  const v = p.getValue();
  el.setAttribute('aria-valuemin', String(p.spec.min));
  el.setAttribute('aria-valuemax', String(p.spec.max));
  el.setAttribute('aria-valuenow', String(+v.toFixed(4)));
  el.setAttribute('aria-valuetext', fmt(p.spec, v));
}

// `id` is a parameter id, or an object with the same interface (getValue, setValue, getNorm, setNorm,
// begin, end, onChange, spec), e.g. a sample's own start point (js/regions.js).
export function knob(bridge, id, color) {
  const p = typeof id === 'object' ? id : bridge.param(id);
  const s = p.spec;
  const root = document.createElement('div');
  root.className = 'knob' + (color === 'mod' ? ' mod' : '');
  root.innerHTML =
    `<div class="knob-dial" role="slider" tabindex="0" aria-label="${s.name}" title="${s.name} — drag, scroll, shift for fine, double-click to reset">` +
    `<svg width="44" height="44" viewBox="0 0 44 44" aria-hidden="true"><path class="k-track" d="${TRACK}"/><path class="k-arc"/>` +
    `<circle class="k-cap" cx="22" cy="22" r="12.5"/><line class="k-ptr" x1="22" y1="22"/></svg></div>` +
    `<div class="k-label">${s.label ?? s.name}</div><output class="k-value"></output>`;
  const dial = root.querySelector('.knob-dial');
  const arcEl = root.querySelector('.k-arc');
  const ptr = root.querySelector('.k-ptr');
  const out = root.querySelector('.k-value');
  const centre = s.bipolar ? 0.5 : 0;  // bipolar knobs fill from 12 o'clock
  const update = () => {
    const n = p.getNorm();
    const ang = -135 + 270 * n;
    const c = -135 + 270 * centre;
    arcEl.setAttribute('d', arc(Math.min(c, ang), Math.max(c, ang)));
    const [x, y] = pt(10, ang);
    ptr.setAttribute('x2', x.toFixed(2)); ptr.setAttribute('y2', y.toFixed(2));
    out.textContent = fmt(s, p.getValue());
    aria(dial, p);
  };
  attachDrag(dial, p, 'y', 180);
  p.onChange(update);
  update();
  return root;
}

export function hslider(bridge, el, id) {
  const p = bridge.param(id);
  el.setAttribute('role', 'slider');
  el.setAttribute('tabindex', '0');
  el.setAttribute('aria-label', p.spec.name);
  el.innerHTML = '<div class="rail"><div class="fill"></div></div>';
  const fill = el.querySelector('.fill');
  const update = () => { fill.style.width = (p.getNorm() * 100).toFixed(1) + '%'; aria(el, p); el.title = p.spec.name + ': ' + fmt(p.spec, p.getValue()); };
  attachDrag(el, p, 'x', el.getBoundingClientRect().width || 120);
  p.onChange(update);
  update();
  return { update };
}

const LFO_ICONS = [
  'M2 8 C4 2 6 2 8 8 C10 14 12 14 14 8',
  'M2 8 L5 3 L11 13 L14 8',
  'M2 12 L8 4 L8 12 L14 4',
  'M2 12 V4 H8 V12 H14 V4',
  'M2 10 H5 V5 H8 V12 H11 V7 H14'
];

export function segmented(bridge, el, id) {
  const p = bridge.param(id);
  const icons = el.dataset.icons === 'lfo';
  el.innerHTML = '';
  const buttons = p.spec.choices.map((label, i) => {
    const b = document.createElement('button');
    b.type = 'button';
    if (icons) {
      b.setAttribute('aria-label', label);
      b.title = label;
      b.innerHTML = `<svg width="14" height="14" viewBox="0 0 16 16" aria-hidden="true"><path d="${LFO_ICONS[i]}" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round"/></svg>`;
    } else b.textContent = label;
    b.addEventListener('click', () => p.setValue(i));
    el.appendChild(b);
    return b;
  });
  const update = () => { const v = p.getValue(); buttons.forEach((b, i) => b.setAttribute('aria-pressed', String(i === v))); };
  p.onChange(update);
  update();
}

export function toggle(bridge, el, id) {
  const p = bridge.param(id);
  if (!el.getAttribute('aria-label') && !el.textContent.trim()) el.setAttribute('aria-label', p.spec.name);
  el.addEventListener('click', () => p.setValue(!p.getValue()));
  const update = () => el.setAttribute('aria-pressed', String(!!p.getValue()));
  p.onChange(update);
  update();
}

// Simple tab group: buttons with data-tab, panels found with data-group="<prefix><n>".
export function tabs(el, prefix, onChange) {
  const btns = [...el.querySelectorAll('[data-tab]')];
  const select = (n) => {
    btns.forEach((b) => b.setAttribute('aria-selected', String(b.dataset.tab === String(n))));
    document.querySelectorAll(`[data-group^="${prefix}"]`).forEach((g) => { g.hidden = g.dataset.group !== prefix + n; });
    onChange && onChange(Number(n));
  };
  btns.forEach((b) => b.addEventListener('click', () => select(b.dataset.tab)));
  el.addEventListener('keydown', (e) => {
    if (e.key !== 'ArrowRight' && e.key !== 'ArrowLeft') return;
    const i = btns.findIndex((b) => b.getAttribute('aria-selected') === 'true');
    const next = btns[(i + (e.key === 'ArrowRight' ? 1 : btns.length - 1)) % btns.length];
    next.focus(); select(next.dataset.tab);
  });
  select(1);
  return select;
}
