// FX chain panel: one slot per effect, in the instrument's order ("fxOrder"). Drag a slot by its
// grip (or anywhere outside its controls) to move it; the pencil opens that effect's settings.
// The order is saved in the instrument JSON, so presets keep it.

import { knob, hslider, toggle, segmented, fmt } from './controls.js';
import { popover, h } from './overlay.js';

const FX = {
  sat:    { name: 'Saturate', on: 'fx_sat_on', amount: 'fx_sat_drive', knobs: ['fx_sat_drive', 'fx_sat_tone', 'fx_sat_mix'], choice: 'fx_sat_type' },
  crush:  { name: 'Bitcrush', on: 'fx_crush_on', amount: 'fx_crush_bits', knobs: ['fx_crush_bits', 'fx_crush_rate', 'fx_crush_mix'] },
  chorus: { name: 'Chorus', on: 'fx_chorus_on', amount: 'fx_chorus_mix', knobs: ['fx_chorus_mix', 'fx_chorus_rate', 'fx_chorus_depth', 'fx_chorus_fb'] },
  delay:  { name: 'Delay', on: 'fx_delay_on', amount: 'fx_delay_fb', knobs: ['fx_delay_time', 'fx_delay_fb', 'fx_delay_mix', 'fx_delay_tone'], toggles: ['fx_delay_sync', 'fx_delay_pingpong'], select: 'fx_delay_div' },
  reverb: { name: 'Reverb', on: 'fx_reverb_on', amount: 'fx_reverb_mix', knobs: ['fx_reverb_mix', 'fx_reverb_size', 'fx_reverb_damp', 'fx_reverb_width', 'fx_reverb_predelay'] },
  tape:   { name: 'Tape', on: 'fx_tape_on', amount: 'fx_tape_drive', knobs: ['fx_tape_wow', 'fx_tape_flutter', 'fx_tape_drive', 'fx_tape_tone', 'fx_tape_mix'] }
};
export const DEFAULT_ORDER = ['sat', 'crush', 'chorus', 'delay', 'reverb', 'tape'];

const GRIP = '<svg width="10" height="14" viewBox="0 0 10 14" aria-hidden="true"><g fill="currentColor"><circle cx="3" cy="3" r="1.2"/><circle cx="7" cy="3" r="1.2"/><circle cx="3" cy="7" r="1.2"/><circle cx="7" cy="7" r="1.2"/><circle cx="3" cy="11" r="1.2"/><circle cx="7" cy="11" r="1.2"/></g></svg>';
const PENCIL = '<svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true"><path d="M2 10 L2.6 7.6 L8 2.2 L9.8 4 L4.4 9.4 Z M7 3.2 L8.8 5" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/></svg>';
const POWER = '<svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true"><path d="M6 1.5 V6 M3.2 3.3 A3.8 3.8 0 1 0 8.8 3.3" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>';

export function setupFxChain(bridge) {
  const grid = document.getElementById('fx-grid');
  const order = () => {
    const o = bridge.getInstrument().fxOrder;
    const valid = Array.isArray(o) ? o.filter((k, i) => FX[k] && o.indexOf(k) === i) : [];
    return [...valid, ...DEFAULT_ORDER.filter((k) => !valid.includes(k))];
  };
  async function saveOrder(next) {
    const inst = JSON.parse(JSON.stringify(bridge.getInstrument()));
    inst.fxOrder = next;
    await bridge.setInstrument(inst);
  }

  // Each effect's settings panel is built once and reused (its controls stay bound to the parameters)
  const editors = {};
  function editorFor(key) {
    if (editors[key]) return editors[key];
    const fx = FX[key];
    const box = h('div', { class: 'fx-edit' });
    if (fx.choice) {
      const seg = h('div', { class: 'seg fill', role: 'group', 'aria-label': bridge.param(fx.choice).spec.name });
      segmented(bridge, seg, fx.choice);
      box.append(seg);
    }
    const knobs = h('div', { class: 'knobs' });
    fx.knobs.forEach((id) => knobs.append(knob(bridge, id)));
    box.append(knobs);
    if (fx.toggles || fx.select) {
      const row = h('div', { class: 'fx-edit-row' });
      (fx.toggles || []).forEach((id) => {
        const b = h('button', { class: 'toggle' }, bridge.param(id).spec.label);
        toggle(bridge, b, id);
        row.append(b);
      });
      if (fx.select) {
        const p = bridge.param(fx.select);
        const sel = h('select', { 'aria-label': p.spec.name });
        p.spec.choices.forEach((c, i) => sel.append(h('option', { value: i }, c)));
        const upd = () => { sel.value = p.getValue(); sel.disabled = fx.toggles && !bridge.param('fx_delay_sync').getValue(); };
        sel.onchange = () => p.setValue(+sel.value);
        p.onChange(upd);
        bridge.param('fx_delay_sync').onChange(upd);
        upd();
        row.append(h('label', { class: 'meta fx-div' }, 'Sync to', sel));
      }
      box.append(row);
    }
    editors[key] = box;
    return box;
  }

  function slotFor(key) {
    const fx = FX[key];
    const slot = h('div', { class: 'fxslot', 'data-fx': key });
    slot.innerHTML =
      `<div class="top-line"><span class="grip" title="Drag to reorder" aria-hidden="true">${GRIP}</span><b>${fx.name}</b>` +
      `<span class="fx-btns"><button class="icon-btn edit" aria-label="${fx.name} settings">${PENCIL}</button>` +
      `<button class="power" aria-label="${fx.name} on/off">${POWER}</button></span></div>` +
      `<span class="meta"></span><div class="hslider"></div>`;
    toggle(bridge, slot.querySelector('.power'), fx.on);
    hslider(bridge, slot.querySelector('.hslider'), fx.amount);
    const onP = bridge.param(fx.on), amtP = bridge.param(fx.amount);
    const meta = slot.querySelector('.meta');
    const upd = () => {
      slot.classList.toggle('off', !onP.getValue());
      const s = amtP.spec;
      let text = `${s.label} ${fmt(s, amtP.getValue())}`;
      if (key === 'delay') {
        const sync = bridge.param('fx_delay_sync').getValue();
        text = (sync ? bridge.param('fx_delay_div').spec.choices[bridge.param('fx_delay_div').getValue()] : fmt(bridge.param('fx_delay_time').spec, bridge.param('fx_delay_time').getValue())) + ' · FB ' + fmt(s, amtP.getValue());
      }
      if (key === 'tape') text = `Wow ${Math.round(bridge.param('fx_tape_wow').getValue())} · Flut ${Math.round(bridge.param('fx_tape_flutter').getValue())} · ${fmt(s, amtP.getValue())}`;
      meta.textContent = text;
    };
    [onP, amtP].forEach((p) => p.onChange(upd));
    if (key === 'delay') ['fx_delay_sync', 'fx_delay_div', 'fx_delay_time'].forEach((id) => bridge.param(id).onChange(upd));
    if (key === 'tape') ['fx_tape_wow', 'fx_tape_flutter'].forEach((id) => bridge.param(id).onChange(upd));
    upd();

    const edit = slot.querySelector('.edit');
    edit.onclick = () => popover(edit, fx.name, (body) => body.append(editorFor(key)), { width: key === 'reverb' || key === 'tape' ? 420 : 360 });
    makeDraggable(slot);
    return slot;
  }

  // ---------- drag to reorder ----------
  function makeDraggable(slot) {
    slot.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || e.target.closest('button, .hslider, select')) return;
      e.preventDefault();
      const startX = e.clientX;
      let moved = false;
      let shift = 0;   // how far the slot's place in the row has moved (unscaled px)
      const onMove = (ev) => {
        const dx = ev.clientX - startX;
        if (!moved) {
          if (Math.abs(dx) < 4) return;
          moved = true;
          slot.classList.add('dragging');
        }
        const scale = grid.getBoundingClientRect().width / grid.offsetWidth || 1;
        const follow = () => { slot.style.transform = `translateX(${dx / scale - shift}px)`; };
        follow();
        // Swap with neighbours while the dragged slot's middle is past theirs (a fast drag can pass several).
        // Only the neighbours move in the page; moving the dragged slot itself would cancel the drag.
        const centre = (el) => { const b = el.getBoundingClientRect(); return (b.left + b.right) / 2; };
        const gap = (el) => el.getBoundingClientRect().width / scale + 8;
        for (let guard = 0; guard < 5; ++guard) {
          const r = slot.getBoundingClientRect(), mid = (r.left + r.right) / 2;
          const next = slot.nextElementSibling, prev = slot.previousElementSibling;
          if (next && mid > centre(next)) { shift += gap(next); grid.insertBefore(next, slot); follow(); }
          else if (prev && mid < centre(prev)) { shift -= gap(prev); grid.insertBefore(prev, slot.nextSibling); follow(); }
          else break;
        }
      };
      const up = () => {
        window.removeEventListener('pointermove', onMove);
        window.removeEventListener('pointerup', up);
        window.removeEventListener('pointercancel', up);
        slot.classList.remove('dragging');
        slot.style.transform = '';
        if (moved) {
          const next = [...grid.children].map((x) => x.dataset.fx);
          if (next.join() !== order().join()) saveOrder(next);
        }
      };
      window.addEventListener('pointermove', onMove);
      window.addEventListener('pointerup', up);
      window.addEventListener('pointercancel', up);
    });
    // Keyboard: Alt + arrow keys move the focused slot
    slot.addEventListener('keydown', (e) => {
      if (!e.altKey || (e.key !== 'ArrowLeft' && e.key !== 'ArrowRight')) return;
      e.preventDefault();
      const o = order(), i = o.indexOf(slot.dataset.fx), j = i + (e.key === 'ArrowLeft' ? -1 : 1);
      if (j < 0 || j >= o.length) return;
      [o[i], o[j]] = [o[j], o[i]];
      saveOrder(o).then(() => grid.querySelector(`[data-fx="${o[j]}"] .power`)?.focus());
    });
  }

  // Slots are created once; re-rendering only reorders them
  const slots = {};
  DEFAULT_ORDER.forEach((k) => { slots[k] = slotFor(k); });
  const render = () => order().forEach((k) => grid.appendChild(slots[k]));
  bridge.onInstrument(render);
  render();
}
