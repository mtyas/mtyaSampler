// Preset header (name, previous/next), the preset browser and the Save dialog.
// Presets live in Documents/mtyaSampler/Presets/<Category>/<Name>.mtyapreset (Source/PresetManager.*).
// In the browser preview they're kept in localStorage by bridge.js.

import { modal, h } from './overlay.js';

export function setupPresets(bridge, ui) {
  const nameEl = document.getElementById('preset-name');
  const subEl = document.getElementById('preset-sub');
  let current = { name: 'Init', category: '', path: '' };

  const show = (p) => {
    current = { ...current, ...p };
    nameEl.textContent = current.name || 'Untitled';
    subEl.textContent = current.category || (current.path ? 'User' : 'Default sound');
  };
  bridge.onPreset(show);
  bridge.command('presetCurrent').then((p) => { if (p && p.name !== undefined) show(p); });

  const run = async (name, ...args) => {
    const r = await bridge.command(name, ...args);
    if (r && r.message) ui.toast(r.message);
    return r;
  };

  // ---------- browser ----------
  async function browse() {
    const list = (await bridge.command('presetList')) || [];
    let category = current.category || 'All', query = '';
    modal('Presets', (body, api) => {
      const cats = ['All', ...new Set(list.map((p) => p.category || 'User'))];
      const side = h('div', { class: 'pb-cats', role: 'listbox', 'aria-label': 'Categories' });
      const search = h('input', { type: 'search', placeholder: 'Search presets', 'aria-label': 'Search presets', spellcheck: 'false' });
      const items = h('div', { class: 'pb-list', role: 'listbox', 'aria-label': 'Presets' });
      if (!cats.includes(category)) category = 'All';

      const renderCats = () => {
        side.innerHTML = '';
        cats.forEach((c) => {
          const n = c === 'All' ? list.length : list.filter((p) => (p.category || 'User') === c).length;
          const b = h('button', { 'aria-selected': String(c === category), role: 'option' }, h('span', {}, c), h('span', { class: 'meta' }, String(n)));
          b.onclick = () => { category = c; renderCats(); renderItems(); };
          side.append(b);
        });
      };
      const renderItems = () => {
        items.innerHTML = '';
        const q = query.toLowerCase();
        const shown = list.filter((p) => (category === 'All' || (p.category || 'User') === category) && (!q || p.name.toLowerCase().includes(q) || (p.category || '').toLowerCase().includes(q)));
        if (!list.length) items.append(h('p', { class: 'meta pop-empty' }, 'No presets yet. Shape a sound, then press Save.'));
        else if (!shown.length) items.append(h('p', { class: 'meta pop-empty' }, 'Nothing matches.'));
        shown.forEach((p) => {
          const b = h('button', { class: 'pb-item', role: 'option', 'aria-selected': String(p.path === current.path) },
            h('b', {}, p.name), h('span', { class: 'meta' }, p.category || 'User'));
          b.onclick = async () => {
            const r = await run('presetLoad', p.path);
            if (r && r.ok) { items.querySelectorAll('.pb-item').forEach((x) => x.setAttribute('aria-selected', 'false')); b.setAttribute('aria-selected', 'true'); }
          };
          b.ondblclick = () => api.close();
          items.append(b);
        });
      };
      search.oninput = () => { query = search.value; renderItems(); };
      renderCats();
      renderItems();
      body.append(
        h('div', { class: 'pb' }, side, h('div', { class: 'pb-main' }, search, items)),
        h('div', { class: 'pop-foot' },
          h('button', { class: 'btn', onclick: async () => { await run('presetInit'); api.close(); } }, 'Init sound'),
          bridge.inPlugin ? h('button', { class: 'btn', onclick: () => bridge.command('presetReveal') }, 'Show folder') : null,
          h('span', { class: 'meta' }, 'Click to load · double-click to load and close'),
          h('button', { class: 'btn primary', onclick: () => api.close() }, 'Done')));
    }, { width: 760 });
  }

  // ---------- save ----------
  async function save() {
    const list = (await bridge.command('presetList')) || [];
    modal('Save preset', (body, api) => {
      const name = h('input', { type: 'text', spellcheck: 'false', 'aria-label': 'Preset name' });
      const cat = h('input', { type: 'text', spellcheck: 'false', 'aria-label': 'Category', list: 'preset-cats' });
      const cats = h('datalist', { id: 'preset-cats' }, [...new Set(list.map((p) => p.category).filter(Boolean))].map((c) => h('option', { value: c })));
      name.value = current.name && current.name !== 'Init' ? current.name : '';
      cat.value = current.category || '';
      const warn = h('span', { class: 'meta warn' });
      const check = () => {
        const exists = list.some((p) => p.name.toLowerCase() === name.value.trim().toLowerCase() && (p.category || '') === cat.value.trim());
        warn.textContent = exists ? 'Replaces the existing preset' : '';
      };
      name.oninput = check; cat.oninput = check; check();
      const doSave = async () => {
        if (!name.value.trim()) { name.focus(); return; }
        const r = await run('presetSave', name.value.trim(), cat.value.trim());
        if (r && r.ok) api.close();
      };
      name.onkeydown = (e) => { if (e.key === 'Enter') doSave(); };
      cat.onkeydown = (e) => { if (e.key === 'Enter') doSave(); };
      body.append(
        h('div', { class: 'fields c2' },
          h('div', { class: 'field' }, h('label', {}, 'Name'), name),
          h('div', { class: 'field' }, h('label', {}, 'Category'), cat, cats)),
        h('div', { class: 'pop-foot' }, warn, h('span', { style: 'flex:1' }),
          h('button', { class: 'btn', onclick: () => api.close() }, 'Cancel'),
          h('button', { class: 'btn primary', onclick: doSave }, 'Save')));
    }, { width: 480 });
  }

  return {
    actions: {
      presetBrowse: browse,
      presetSave: save,
      presetPrev: () => run('presetPrev'),
      presetNext: () => run('presetNext')
    }
  };
}
