// Modulation routing: which targets each envelope, LFO, macro and controller moves.
// Two lists of routes, both { source, target, amount } with amount −100…100:
//  * All layers: the instrument's "modRoutes". Targets are voice targets (below, moved per note) or
//    "param:<id>" for any float parameter. Envelopes, LFOs, velocity and key moving a parameter (a knob
//    shared by all notes) follow the newest note.
//  * This layer: a layer's own "modRoutes", voice targets only, on top of the instrument's.
// "LFO 1" / "Env 2" mean the playing layer's own modulators when it has them (Layer switch in the LFO /
// Env panels), otherwise the instrument's; "Common LFO 1" / "Common Env 2" are always the instrument's.
// The engine side is Source/InstrumentMap.* (parseRoutes) and Source/SamplerEngine.cpp.

import { popover, modal, h } from './overlay.js';

export const SOURCES = [
  ['env1', 'Env 1 (Amp)'], ['env2', 'Env 2 (Filter)'], ['env3', 'Env 3 (Mod)'],
  ['lfo1', 'LFO 1'], ['lfo2', 'LFO 2'], ['lfo3', 'LFO 3'], ['lfo4', 'LFO 4'],
  ['clfo1', 'Common LFO 1'], ['clfo2', 'Common LFO 2'], ['clfo3', 'Common LFO 3'], ['clfo4', 'Common LFO 4'],
  ['cenv2', 'Common Env 2'], ['cenv3', 'Common Env 3'],
  ['macro1', 'Macro 1'], ['macro2', 'Macro 2'], ['macro3', 'Macro 3'], ['macro4', 'Macro 4'],
  ['modwheel', 'Mod wheel'], ['aftertouch', 'Aftertouch'], ['velocity', 'Velocity'], ['key', 'Key track (pitch, from C3)']
];
const GLOBAL = new Set(['macro1', 'macro2', 'macro3', 'macro4', 'modwheel', 'aftertouch']);

// What 100 % means for each voice target (same as the engine)
export const VOICE_TARGETS = [
  ['pitch', 'Pitch', '±24 st'], ['volume', 'Volume', '±100 %'], ['pan', 'Pan', 'L…R'],
  ['cutoff', 'Cutoff', '±6 oct'], ['reso', 'Resonance', '±100 %'], ['drive', 'Drive', '±24 dB'],
  ['grain_pos', 'Grain Pos', '±region'], ['grain_size', 'Grain Size', '×¼…×4'], ['grain_density', 'Grain Density', '×¼…×4'],
  ['grain_spray', 'Grain Spray', '±100 %'], ['grain_pitchrnd', 'Grain Pitch Rnd', '±12 st'], ['start', 'Sample Start', '+region']
];

// Used when an instrument has no "modRoutes" (keep in step with defaultModRoutes() in InstrumentMap.cpp)
export const DEFAULT_ROUTES = [
  { source: 'env3', target: 'grain_size', amount: 50 },
  { source: 'lfo1', target: 'grain_pos', amount: 50 },
  { source: 'lfo2', target: 'pan', amount: 100 },
  { source: 'lfo3', target: 'cutoff', amount: 50 },
  { source: 'lfo4', target: 'grain_spray', amount: 100 },
  { source: 'macro1', target: 'grain_density', amount: 40 },
  { source: 'macro1', target: 'grain_spray', amount: 30 },
  { source: 'macro2', target: 'param:lfo1_rate', amount: 30 },
  { source: 'macro2', target: 'param:lfo1_depth', amount: 40 },
  { source: 'macro3', target: 'param:fx_reverb_mix', amount: 30 },
  { source: 'macro3', target: 'param:fx_delay_mix', amount: 20 },
  { source: 'macro4', target: 'drive', amount: 40 },
  { source: 'macro4', target: 'param:fx_sat_drive', amount: 40 }
];

const clone = (o) => JSON.parse(JSON.stringify(o));

// ctx.layer(): the selected layer (set by main.js once the Layers page exists)
export function setupModMatrix(bridge, ctx = { layer: () => null }) {
  const floatParams = bridge.spec.params.filter((p) => p.type === 'float' && !/^macro\d$/.test(p.id));
  const sourceLabel = (id) => {
    const m = /^macro(\d)$/.exec(id);
    if (m) return 'Macro ' + m[1] + ' (' + (bridge.param(id).spec.label ?? '') + ')';
    return (SOURCES.find(([s]) => s === id) || [id, id])[1];
  };
  const targetLabel = (t) => {
    if (t.startsWith('param:')) {
      const p = bridge.spec.params.find((x) => x.id === t.slice(6));
      return p ? p.name : t.slice(6);
    }
    return (VOICE_TARGETS.find(([id]) => id === t) || [t, t])[1];
  };

  // scope: 'all' (the instrument's routes) or 'layer' (the selected layer's own)
  const layer = () => ctx.layer();
  const routes = (scope) => {
    const inst = bridge.getInstrument();
    if (scope === 'layer') return clone((layer() && layer().modRoutes) || []);
    return clone(inst.modRoutes ?? DEFAULT_ROUTES);
  };
  async function save(scope, next) {
    const inst = clone(bridge.getInstrument());
    if (scope === 'layer') {
      const l = inst.layers.find((x) => layer() && x.id === layer().id);
      if (!l) return;
      l.modRoutes = next;
    } else inst.modRoutes = next;
    await bridge.setInstrument(inst);
  }
  const listeners = new Set();
  bridge.onInstrument(() => listeners.forEach((f) => f()));

  // ---------- one row: target + amount ----------
  function targetSelect(source, value, onChange, scope) {
    const sel = h('select', { 'aria-label': 'Target' });
    const voice = h('optgroup', { label: 'Voice (each note)' });
    VOICE_TARGETS.forEach(([id, label, range]) => voice.append(h('option', { value: id }, `${label}  (${range})`)));
    sel.append(voice);
    if (scope !== 'layer') {
      // Any knob (float parameter), grouped like the UI. Not for a layer's own routes: knobs are shared.
      const groups = new Map();
      floatParams.forEach((p) => { if (!groups.has(p.group)) groups.set(p.group, []); groups.get(p.group).push(p); });
      groups.forEach((list, g) => {
        const og = h('optgroup', { label: g + ' parameters' });
        list.forEach((p) => og.append(h('option', { value: 'param:' + p.id }, p.name)));
        sel.append(og);
      });
    }
    sel.value = value;
    sel.onchange = () => onChange(sel.value);
    return sel;
  }

  function amountControl(value, onChange) {
    const wrap = h('div', { class: 'amt' });
    const range = h('input', { type: 'range', min: -100, max: 100, step: 1, 'aria-label': 'Amount' });
    const num = h('input', { type: 'text', inputmode: 'decimal', class: 'amt-num', 'aria-label': 'Amount %' });
    const show = (v) => { range.value = v; num.value = (v > 0 ? '+' : '') + v; range.style.setProperty('--a', ((v + 100) / 2) + '%'); };
    show(value);
    range.oninput = () => show(+range.value);
    range.onchange = () => onChange(+range.value);
    range.ondblclick = () => { show(0); onChange(0); };
    num.onchange = () => {
      const v = Math.max(-100, Math.min(100, Math.round(parseFloat(num.value))));
      if (Number.isNaN(v)) { show(+range.value); return; }
      show(v); onChange(v);
    };
    wrap.append(range, num);
    return wrap;
  }

  // ---------- popover for one source: both lists ----------
  function openSource(anchor, source) {
    popover(anchor, sourceLabel(source) + ' targets', (body, api) => {
      const section = (scope, title, note) => {
        const box = h('div', { class: 'route-sec' }, h('div', { class: 'route-sec-head' }, h('b', {}, title), h('span', { class: 'meta' }, note)));
        const all = routes(scope);
        const mine = all.map((r, i) => [r, i]).filter(([r]) => r.source === source);
        if (!mine.length) box.append(h('p', { class: 'meta pop-empty' }, 'Nothing assigned.'));
        mine.forEach(([r, i]) => {
          box.append(h('div', { class: 'route' },
            targetSelect(source, r.target, (t) => { all[i].target = t; save(scope, all); }, scope),
            amountControl(r.amount, (v) => { all[i].amount = v; save(scope, all); }),
            h('button', { class: 'icon-btn', 'aria-label': 'Remove ' + targetLabel(r.target), onclick: () => { all.splice(i, 1); save(scope, all).then(render); } }, '×')));
        });
        const used = new Set(mine.map(([r]) => r.target));
        const next = VOICE_TARGETS.map(([id]) => id).find((id) => !used.has(id)) ?? 'pitch';
        box.append(h('button', { class: 'btn small add-route', onclick: () => { all.push({ source, target: next, amount: 50 }); save(scope, all).then(() => { render(); api.place(); }); } }, '+ Add target'));
        return box;
      };
      const render = () => {
        body.innerHTML = '';
        body.append(section('all', 'All layers', GLOBAL.has(source) ? 'Same for every note' : 'Voice targets move each note; knobs follow the newest note'));
        const l = layer();
        if (l) body.append(section('layer', 'This layer · ' + l.name, 'Only this layer, voice targets'));
      };
      render();
    }, { width: 420 });
  }

  // ---------- the whole matrix ----------
  function openMatrix() {
    let scope = 'all';
    modal('Modulation matrix', (body) => {
      const render = () => {
        body.innerHTML = '';
        const l = layer();
        const tabs = h('div', { class: 'seg', role: 'group', 'aria-label': 'Which routes' },
          h('button', { 'aria-pressed': String(scope === 'all'), onclick: () => { scope = 'all'; render(); } }, 'All layers'),
          h('button', { 'aria-pressed': String(scope === 'layer'), disabled: !l, onclick: () => { scope = 'layer'; render(); } }, l ? 'This layer · ' + l.name : 'This layer'));
        const all = routes(scope);
        const table = h('div', { class: 'matrix' });
        table.append(h('div', { class: 'matrix-row head' }, h('span', {}, 'Source'), h('span', {}, 'Target'), h('span', {}, 'Amount'), h('span')));
        all.forEach((r, i) => {
          const src = h('select', { 'aria-label': 'Source' });
          SOURCES.forEach(([id]) => src.append(h('option', { value: id }, sourceLabel(id))));
          src.value = r.source;
          src.onchange = () => { all[i].source = src.value; save(scope, all).then(render); };
          table.append(h('div', { class: 'matrix-row' }, src,
            targetSelect(r.source, r.target, (t) => { all[i].target = t; save(scope, all); }, scope),
            amountControl(r.amount, (v) => { all[i].amount = v; save(scope, all); }),
            h('button', { class: 'icon-btn', 'aria-label': 'Remove route', onclick: () => { all.splice(i, 1); save(scope, all).then(render); } }, '×')));
        });
        if (!all.length) table.append(h('p', { class: 'meta pop-empty' }, scope === 'layer' ? 'No routes for this layer: it uses the "All layers" routes only.' : 'No routes: nothing is modulated.'));
        body.append(h('div', { class: 'pop-foot' }, tabs,
          h('span', { class: 'meta' }, scope === 'layer' ? 'Added to the "All layers" routes, for this layer only' : '"LFO n" is a layer\'s own LFO when it has one; "Common LFO n" is always the instrument\'s')),
          table,
          h('div', { class: 'pop-foot' },
            h('button', { class: 'btn', onclick: () => { all.push({ source: scope === 'layer' ? 'lfo1' : 'modwheel', target: 'cutoff', amount: 50 }); save(scope, all).then(render); } }, '+ Add route'),
            scope === 'all' ? h('button', { class: 'btn', onclick: () => save('all', clone(DEFAULT_ROUTES)).then(render) }, 'Reset to defaults') : null,
            h('span', { class: 'meta' }, 'Double-click an amount to zero it')));
      };
      render();
    }, { width: 780 });
  }

  // Short text for a tag: "→ Grain Pos +50%", "→ 3 targets" or "+ Assign" (both lists)
  function summary(source) {
    const mine = [...routes('all'), ...routes('layer')].filter((r) => r.source === source);
    if (!mine.length) return '+ Assign';
    if (mine.length === 1) return '→ ' + targetLabel(mine[0].target) + ' ' + (mine[0].amount > 0 ? '+' : '') + mine[0].amount + '%';
    return '→ ' + mine.length + ' targets';
  }

  // ---------- wire the panels ----------
  let envN = 1, lfoN = 1;
  const envTag = document.getElementById('env-tag'), lfoTag = document.getElementById('lfo-dest');
  const macroBtns = [...document.querySelectorAll('#macro-routes [data-macro]')];
  function refresh() {
    envTag.textContent = summary('env' + envN);
    envTag.title = 'Choose what Env ' + envN + ' modulates' + (envN === 1 ? ' (it always shapes the level too)' : envN === 2 ? ' (it also moves the cutoff through Env Amt)' : '');
    lfoTag.textContent = summary('lfo' + lfoN);
    lfoTag.title = 'Choose what LFO ' + lfoN + ' modulates';
    macroBtns.forEach((b) => { b.textContent = summary('macro' + b.dataset.macro); b.title = 'Choose what ' + sourceLabel('macro' + b.dataset.macro) + ' controls'; });
  }
  envTag.onclick = () => openSource(envTag, 'env' + envN);
  lfoTag.onclick = () => openSource(lfoTag, 'lfo' + lfoN);
  macroBtns.forEach((b) => { b.onclick = () => openSource(b, 'macro' + b.dataset.macro); });
  document.getElementById('open-matrix').onclick = openMatrix;
  listeners.add(refresh);
  refresh();

  return {
    setEnv(n) { envN = n; refresh(); },
    setLfo(n) { lfoN = n; refresh(); },
    refresh,
    openMatrix
  };
}
