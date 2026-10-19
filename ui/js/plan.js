// Recording-session planning: instrument presets, the ordered list of takes, and how the recorded
// samples map onto keys and velocities. Pure functions, shared by the page and the preview engine.
// The C++ engine receives the finished plan (takes + timings) and does not re-derive it.

import { noteName } from './layers.js';

export const DEFAULTS = {
  name: 'My Instrument', inst: 'keys', pitched: true,
  low: 33, high: 96, step: 3, rr: 2, vel: 3, maxSec: 8,
  order: 'note', direction: 'up',
  countBeats: 3, bpm: 90, toneSec: 1.5, tone: true, voice: true, autoAdvance: true,
  input: 'stereo', thresholdDb: -42, silenceMs: 900, prerollMs: 20, fadeMs: 60, normalize: 'session',
  monitor: false, replace: false, folder: ''
};

// Note numbers use C3 = 60 (the convention used everywhere in this plugin).
export const PRESETS = {
  voice:   { label: 'Voice',            low: 48, high: 72, step: 3, rr: 2, vel: 1, maxSec: 4, pitched: true,  tone: true },
  guitar:  { label: 'Guitar',           low: 40, high: 84, step: 3, rr: 3, vel: 2, maxSec: 6, pitched: true,  tone: true },
  keys:    { label: 'Keys / Piano',     low: 33, high: 96, step: 3, rr: 2, vel: 3, maxSec: 8, pitched: true,  tone: true },
  bass:    { label: 'Bass',             low: 28, high: 67, step: 3, rr: 2, vel: 2, maxSec: 5, pitched: true,  tone: true },
  strings: { label: 'Strings & Winds',  low: 55, high: 93, step: 2, rr: 2, vel: 2, maxSec: 6, pitched: true,  tone: true },
  synth:   { label: 'Hardware Synth',   low: 36, high: 96, step: 3, rr: 1, vel: 1, maxSec: 4, pitched: true,  tone: true },
  drums:   { label: 'Drums & One-shots', low: 36, high: 47, step: 1, rr: 4, vel: 3, maxSec: 2, pitched: false, tone: false },
  custom:  { label: 'Custom' }
};

export const STEPS = [[1, 'Note'], [2, '2 st'], [3, '3 st'], [4, '4 st'], [6, '6 st'], [12, 'Octave']];

const VEL_LABELS = { 1: ['Normal'], 2: ['Soft', 'Loud'], 3: ['Soft', 'Medium', 'Loud'], 4: ['Very soft', 'Soft', 'Loud', 'Very loud'] };
const VEL_TAGS = { 1: ['mf'], 2: ['p', 'f'], 3: ['p', 'mf', 'f'], 4: ['pp', 'mp', 'mf', 'ff'] };

const safe = (s) => (s || 'Session').replace(/[^A-Za-z0-9_-]+/g, '_').replace(/^_+|_+$/g, '') || 'Session';
const noteTag = (n) => noteName(n).replace('♯', 's').replace('-', 'm');

export function roots(s) {
  const out = [];
  const lo = Math.min(s.low, s.high), hi = Math.max(s.low, s.high);
  for (let n = lo; n <= hi; n += s.step) out.push(n);
  return out;
}

// Key range covered by each recorded root: split halfway between neighbouring roots,
// and stretch the outer ones to the lowest / highest note.
export function zonesFor(s) {
  const r = roots(s);
  const lo = Math.min(s.low, s.high), hi = Math.max(s.low, s.high);
  const zones = [];
  r.forEach((root, i) => {
    const zlo = i === 0 ? lo : Math.floor((r[i - 1] + root) / 2) + 1;
    const zhi = i === r.length - 1 ? hi : Math.floor((root + r[i + 1]) / 2);
    for (let v = 0; v < s.vel; v++) {
      zones.push({
        root, lo: zlo, hi: zhi, v,
        vlo: Math.round(v * 128 / s.vel), vhi: Math.round((v + 1) * 128 / s.vel) - 1,
        velLabel: VEL_LABELS[s.vel][v], velTag: VEL_TAGS[s.vel][v]
      });
    }
  });
  return zones;
}

export function announceText(s, t) {
  const parts = [];
  if (s.pitched) parts.push(spokenNote(t.note)); else parts.push('Sound ' + (t.rootIndex + 1));
  if (s.vel > 1) parts.push(t.velLabel);
  if (s.rr > 1) parts.push('take ' + (t.rr + 1));
  return parts.join('. ') + '.';
}

export function spokenNote(n) {
  const name = noteName(n);                    // e.g. D♯3, C-1
  const m = /^([A-G])(♯?)(-?\d)$/.exec(name);
  if (!m) return name;
  const oct = m[3].startsWith('-') ? 'minus ' + m[3].slice(1) : m[3];
  return `${m[1]}${m[2] ? ' sharp' : ''} ${oct}`;
}

export function buildPlan(s) {
  const r = roots(s);
  const ordered = s.direction === 'down' ? [...r].reverse() : r;
  const zones = zonesFor(s);
  const zoneIndex = (root, v) => zones.findIndex((z) => z.root === root && z.v === v);
  const takes = [];
  const add = (root, v, rr) => {
    const z = zones[zoneIndex(root, v)];
    takes.push({
      i: takes.length, note: root, rootIndex: r.indexOf(root), v, rr, zone: zoneIndex(root, v),
      velLabel: z.velLabel,
      file: `${safe(s.name)}_${noteTag(root)}_${z.velTag}_rr${rr + 1}.wav`
    });
  };
  if (s.order === 'passes') {
    for (let rr = 0; rr < s.rr; rr++) for (const root of ordered) for (let v = 0; v < s.vel; v++) add(root, v, rr);
  } else {
    for (const root of ordered) for (let v = 0; v < s.vel; v++) for (let rr = 0; rr < s.rr; rr++) add(root, v, rr);
  }
  takes.forEach((t) => { t.say = announceText(s, t); });

  const beatSec = 60 / s.bpm;
  const timing = {
    announceSec: s.voice ? 2.2 : 0.8,            // the C++ engine uses the real prompt length when a voice pack is installed
    toneSec: s.tone && s.pitched ? s.toneSec : 0,
    beatSec, countBeats: s.countBeats, maxSec: s.maxSec
  };
  const perTake = timing.announceSec + timing.toneSec + s.countBeats * beatSec + s.maxSec * 0.75 + 0.6;
  return { settings: { ...s }, roots: r, zones, takes, timing, estimateSec: Math.round(perTake * takes.length) };
}

// Layers to add to the instrument for a plan (samples are filled in as takes are saved).
export function layersForPlan(plan) {
  const s = plan.settings;
  return plan.zones.map((z, i) => ({
    id: Date.now() + i, name: `${s.name} ${s.pitched ? noteName(z.root) : 'Sound ' + (plan.roots.indexOf(z.root) + 1)}${s.vel > 1 ? ' ' + z.velTag : ''}`,
    group: s.name, lo: z.lo, hi: z.hi, root: z.root, vlo: z.vlo, vhi: z.vhi,
    keyFade: 0, velFade: s.vel > 1 ? 6 : 0, keyTrack: s.pitched ? 100 : 0,
    rrMode: s.rr > 1 ? 'norepeat' : 'off', rrGroup: s.rr > 1 ? 'RR-' + s.name : '', rrReset: 'idle',
    samples: Array(s.rr).fill(''), folder: s.folder,
    volume: 0, pan: 0, output: 'Main', muted: false, solo: false,
    trigger: 'attack', releaseVolume: -6, releaseDecay: 600, velToVol: s.vel > 1 ? 30 : 80,
    polyphony: 16, choke: 0, steal: 'oldest', voiceMode: 'poly', glide: 0, keyswitch: -1, articulation: 'All'
  }));
}

export function fmtDuration(sec) {
  if (sec < 60) return Math.round(sec) + ' s';
  const m = Math.round(sec / 60);
  return m < 60 ? m + ' min' : Math.floor(m / 60) + ' h ' + (m % 60) + ' min';
}
