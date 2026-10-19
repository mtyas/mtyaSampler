// Popovers (anchored to a control) and modal dialogs, drawn inside the scaled #app canvas.
// Both close on Escape or a click outside; only one popover is open at a time.

let openPop = null;

function appGeometry() {
  const app = document.getElementById('app');
  const r = app.getBoundingClientRect();
  return { app, r, scale: r.width / app.offsetWidth || 1 };
}

function onOutside(el, close) {
  const down = (e) => { if (!el.contains(e.target) && !e.target.closest('[data-keep-pop]')) close(); };
  const key = (e) => { if (e.key === 'Escape') { e.stopPropagation(); close(); } };
  setTimeout(() => document.addEventListener('pointerdown', down, true), 0);
  document.addEventListener('keydown', key, true);
  return () => { document.removeEventListener('pointerdown', down, true); document.removeEventListener('keydown', key, true); };
}

// Opens a popover under (or above, if there's no room) `anchor`. `build(body, api)` fills it.
export function popover(anchor, title, build, { width = 340 } = {}) {
  if (openPop) openPop.close();
  const { app, r, scale } = appGeometry();
  const a = anchor.getBoundingClientRect();
  const el = document.createElement('div');
  el.className = 'pop';
  el.setAttribute('role', 'dialog');
  el.setAttribute('aria-label', title);
  el.style.width = width + 'px';
  el.innerHTML = '<div class="pop-head"><b></b><button class="icon-btn pop-x" aria-label="Close">×</button></div><div class="pop-body"></div>';
  el.querySelector('b').textContent = title;
  app.appendChild(el);

  let detach = () => {};
  const api = {
    el,
    body: el.querySelector('.pop-body'),
    close() { detach(); el.remove(); if (openPop === api) openPop = null; },
    place() {
      const x = (a.left - r.left) / scale, yBelow = (a.bottom - r.top) / scale + 6, yAbove = (a.top - r.top) / scale - 6;
      const h = el.offsetHeight;
      el.style.left = Math.max(8, Math.min(app.offsetWidth - width - 8, x)) + 'px';
      // Below the control when it fits comfortably, otherwise above it
      el.style.top = (yBelow + h < app.offsetHeight - 24 ? yBelow : Math.max(8, yAbove - h)) + 'px';
    }
  };
  el.querySelector('.pop-x').onclick = () => api.close();
  build(api.body, api);
  api.place();
  detach = onOutside(el, () => api.close());
  openPop = api;
  return api;
}

// A centred dialog over the whole canvas.
export function modal(title, build, { width = 720 } = {}) {
  if (openPop) openPop.close();
  const { app } = appGeometry();
  const back = document.createElement('div');
  back.className = 'modal-back';
  back.innerHTML = '<div class="modal" role="dialog" aria-modal="true"><div class="pop-head"><b></b><button class="icon-btn pop-x" aria-label="Close">×</button></div><div class="pop-body"></div></div>';
  const box = back.querySelector('.modal');
  box.style.width = width + 'px';
  box.setAttribute('aria-label', title);
  back.querySelector('b').textContent = title;
  app.appendChild(back);
  const api = {
    el: box,
    body: back.querySelector('.pop-body'),
    close() { detach(); back.remove(); }
  };
  back.querySelector('.pop-x').onclick = () => api.close();
  back.addEventListener('pointerdown', (e) => { if (e.target === back) api.close(); });
  const detach = onOutside(box, () => {});   // Escape only; clicks on the backdrop are handled above
  const key = (e) => { if (e.key === 'Escape') api.close(); };
  document.addEventListener('keydown', key);
  const close = api.close;
  api.close = () => { document.removeEventListener('keydown', key); close(); };
  build(api.body, api);
  const first = box.querySelector('input,button:not(.pop-x),select');
  if (first) first.focus();
  return api;
}

// Small helpers for building rows
export function h(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'class') el.className = v;
    else if (k.startsWith('on')) el[k] = v;
    else if (v !== false && v !== undefined) el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c != null) el.append(c);
  return el;
}
