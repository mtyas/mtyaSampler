// Browser-preview version of the recording assistant. It follows the same steps and sends the same
// state as the C++ SampleRecorder, but plays the guide (tone + count-in clicks) with WebAudio and makes
// up the recorded audio, so the whole flow can be tried in a browser. Inside the plugin this file is unused.

const PHASES = ['announce', 'tone', 'countin', 'record', 'save'];

export class MockRecorder {
  constructor() {
    this.listeners = { state: new Set(), take: new Set() };
    this.plan = null;
    this.status = 'idle';
    this.phase = 'idle';
    this.take = 0;
    this.t = 0;              // seconds into the current phase
    this.peaks = [];
    this.lastSaved = -1;
    this.saved = 0;
    this.monitor = false;
    this.inDb = -90;
    this.ctx = null;
    this.last = performance.now();
    setInterval(() => this.tick(), 33);
  }

  on(kind, fn) { this.listeners[kind].add(fn); return () => this.listeners[kind].delete(fn); }
  emit(kind, data) { this.listeners[kind].forEach((f) => f(data)); }

  send(cmd, arg) {
    switch (cmd) {
      case 'start':
        this.plan = arg; this.take = 0; this.saved = 0; this.lastSaved = -1;
        this.status = 'running'; this.enter('announce');
        this.audio();
        break;
      case 'pause': if (this.status === 'running') this.status = 'paused'; break;
      case 'resume': if (this.status === 'paused') { this.status = 'running'; this.enter('announce'); } break;
      case 'retake': {
        const i = this.lastSaved >= 0 ? this.lastSaved : this.take;
        this.goto(i); break;
      }
      case 'skip': this.emit('take', { index: this.take, skipped: true }); this.next(); break;
      case 'next': if (this.status === 'review') { this.status = 'running'; this.next(); } break;
      case 'goto': this.goto(arg); break;
      case 'stop': this.status = 'idle'; this.phase = 'idle'; break;
      case 'monitor': this.monitor = !!arg; break;
    }
    this.publish();
    return { ok: true };
  }

  goto(i) {
    if (!this.plan) return;
    this.take = Math.max(0, Math.min(this.plan.takes.length - 1, i));
    this.status = 'running';
    this.enter('announce');
  }

  next() {
    if (this.take + 1 >= this.plan.takes.length) { this.status = 'done'; this.phase = 'idle'; this.publish(); return; }
    this.take++;
    this.enter('announce');
  }

  phaseLen(p) {
    const tm = this.plan.timing;
    if (p === 'announce') return tm.announceSec;
    if (p === 'tone') return tm.toneSec;
    if (p === 'countin') return tm.countBeats * tm.beatSec;
    if (p === 'record') return this.recLen;
    if (p === 'save') return 0.45;
    return 0;
  }

  enter(p) {
    this.phase = p; this.t = 0;
    if (p === 'tone' && this.phaseLen('tone') === 0) return this.enter('countin');
    if (p === 'tone') this.playTone(this.plan.takes[this.take].note, this.phaseLen('tone'));
    if (p === 'countin') this.scheduleClicks();
    if (p === 'record') {
      this.peaks = [];
      const s = this.plan.settings;
      // pretend the player stops after a while; auto-stop then trims the silence
      const played = 0.8 + Math.random() * s.maxSec * 0.6;
      this.recLen = s.silenceMs > 0 ? Math.min(s.maxSec, played + s.silenceMs / 1000) : s.maxSec;
      this.played = played;
    }
  }

  tick() {
    const now = performance.now();
    const dt = Math.min(0.1, (now - this.last) / 1000);
    this.last = now;
    if (this.status === 'running' && this.phase !== 'idle') {
      this.t += dt;
      if (this.phase === 'record') {
        const x = this.t;
        const env = x < 0.12 ? 0 : Math.exp(-(x - 0.12) * (3.2 / this.played)) * (x < this.played ? 1 : Math.max(0, 1 - (x - this.played) * 4));
        const lvl = Math.max(0.002, env * (0.55 + 0.35 * Math.random()));
        this.peaks.push(lvl);
        this.inDb = 20 * Math.log10(lvl);
      } else this.inDb = -62 + Math.random() * 4;
      if (this.t >= this.phaseLen(this.phase)) this.advance();
    } else this.inDb = -90;
    this.publish();
  }

  advance() {
    const i = PHASES.indexOf(this.phase);
    if (this.phase === 'save') {
      const tk = this.plan.takes[this.take];
      const peak = Math.max(...this.peaks, 0.001);
      this.lastSaved = this.take; this.saved++;
      this.emit('take', {
        index: this.take, file: tk.file, lengthSec: +(this.played + 0.1).toFixed(2),
        peakDb: +(20 * Math.log10(peak)).toFixed(1), clipped: false, empty: false,
        peaks: downsample(this.peaks, 120)
      });
      if (this.plan.settings.autoAdvance) this.next();
      else { this.status = 'review'; this.phase = 'idle'; }
      return;
    }
    this.enter(PHASES[i + 1]);
  }

  publish() {
    const tm = this.plan?.timing;
    const count = this.phase === 'countin' && tm ? Math.max(0, tm.countBeats - Math.floor(this.t / tm.beatSec)) : 0;
    this.emit('state', {
      status: this.status, phase: this.phase, take: this.take, total: this.plan?.takes.length ?? 0,
      saved: this.saved, phaseTime: this.t, phaseLen: this.plan && this.phase !== 'idle' ? this.phaseLen(this.phase) : 0,
      count, recSec: this.phase === 'record' ? this.t : 0, inDb: this.inDb, clip: false,
      peaks: this.phase === 'record' || this.phase === 'save' ? downsample(this.peaks, 200) : [],
      maxSec: this.plan?.settings.maxSec ?? 0, voiceInUi: true, lastSaved: this.lastSaved
    });
  }

  // ---- guide audio in the browser ----
  audio() {
    try { this.ctx = this.ctx || new (window.AudioContext || window.webkitAudioContext)(); this.ctx.resume(); } catch { this.ctx = null; }
  }
  playTone(note, sec) {
    if (!this.ctx) return;
    const f = 440 * Math.pow(2, (note - 69) / 12);
    const o = this.ctx.createOscillator(), g = this.ctx.createGain(), t0 = this.ctx.currentTime;
    o.frequency.value = f; o.type = 'sine';
    g.gain.setValueAtTime(0, t0); g.gain.linearRampToValueAtTime(0.18, t0 + 0.03);
    g.gain.setValueAtTime(0.18, t0 + Math.max(0.05, sec - 0.12)); g.gain.linearRampToValueAtTime(0, t0 + sec);
    o.connect(g).connect(this.ctx.destination); o.start(t0); o.stop(t0 + sec + 0.05);
  }
  scheduleClicks() {
    if (!this.ctx) return;
    const tm = this.plan.timing, t0 = this.ctx.currentTime;
    for (let b = 0; b < tm.countBeats; b++) {
      const o = this.ctx.createOscillator(), g = this.ctx.createGain(), t = t0 + b * tm.beatSec;
      o.frequency.value = b === tm.countBeats - 1 ? 1760 : 1320;
      g.gain.setValueAtTime(0.25, t); g.gain.exponentialRampToValueAtTime(0.001, t + 0.06);
      o.connect(g).connect(this.ctx.destination); o.start(t); o.stop(t + 0.08);
    }
  }
}

export function downsample(arr, n) {
  if (arr.length <= n) return arr.slice();
  const out = [], step = arr.length / n;
  for (let i = 0; i < n; i++) {
    let m = 0;
    for (let j = Math.floor(i * step); j < Math.floor((i + 1) * step); j++) m = Math.max(m, arr[j]);
    out.push(m);
  }
  return out;
}
