// About dialog: opened from the logo. Links open in the system browser (native "openUrl",
// which only accepts the addresses listed here and in PluginEditor.cpp).

import { modal, h } from './overlay.js';

export const LINKS = [
  ['GitHub', 'github.com/mtyas', 'https://github.com/mtyas'],
  ['Source code', 'github.com/mtyas/mtyaSampler', 'https://github.com/mtyas/mtyaSampler'],
  ['Website', 'mtyas.com', 'https://mtyas.com'],
  ['Support me', 'ko-fi.com/mtyas', 'https://ko-fi.com/mtyas']
];

const LOGO = '<svg width="56" height="56" viewBox="0 0 30 30" aria-hidden="true"><rect x="1" y="1" width="28" height="28" rx="7" fill="#1C1F22" stroke="#33373C"/><path d="M7 11 C11 9 15 13 23 10" fill="none" stroke="var(--acc)" stroke-width="2" stroke-linecap="round"/><path d="M7 16 C12 14 17 18 23 15" fill="none" stroke="var(--acc)" stroke-opacity=".7" stroke-width="2" stroke-linecap="round"/><path d="M7 21 C11 19 16 23 23 20" fill="none" stroke="var(--mod)" stroke-width="2" stroke-linecap="round"/></svg>';

export function setupAbout(bridge) {
  async function open() {
    const info = (await bridge.command('about')) || {};
    modal('About', (body) => {
      const head = h('div', { class: 'about-head' });
      head.innerHTML = LOGO;
      head.append(h('div', {},
        h('div', { class: 'about-name' }, 'mtya', h('b', {}, 'Sampler')),
        h('div', { class: 'meta' }, `Version ${info.version || '0.1.0'}${info.format ? ' · ' + info.format : ''}`)));
      const links = h('div', { class: 'about-links' }, LINKS.map(([label, text, url]) =>
        h('button', { class: 'about-link', onclick: () => bridge.openUrl(url), title: url },
          h('span', { class: 'meta' }, label), h('b', {}, text))));
      body.append(head,
        h('p', { class: 'about-text' }, 'A free sampler with a granular engine, analogue-style filters, a modulation matrix, a full multi-sample layer editor and a recording assistant, made by mtyas.'),
        links,
        h('p', { class: 'about-small' },
          'Free software under the GNU General Public License v3 (or later): use it, study it, change it and share it. ',
          'Built with JUCE (AGPLv3) and clap-juce-extensions (MIT). Fonts: Barlow and JetBrains Mono (SIL Open Font License).'));
    }, { width: 520 });
  }
  document.getElementById('about-btn').onclick = open;
  return { open };
}
