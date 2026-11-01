// Som do Game Boy / Game Boy Color: 2 canais de onda quadrada (o 1º com sweep), canal de onda programável
// e ruído. O sinal de cada canal é integrado entre eventos (filtro de média por amostra, sem serrilhado
// de agudos) e passa por um filtro passa-alta como o capacitor de saída do aparelho.

const CLOCK = 4194304;
const DUTY = [
  [0, 0, 0, 0, 0, 0, 0, 1],
  [1, 0, 0, 0, 0, 0, 0, 1],
  [1, 0, 0, 0, 0, 1, 1, 1],
  [0, 1, 1, 1, 1, 1, 1, 0],
];
// bits que sempre leem 1 em cada registrador FF10–FF2F
const READ_OR = [
  0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
  0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
];

function channel() {
  return {
    on: false, dac: false, len: 0, lenEn: false, maxLen: 64,
    vol: 0, envInit: 0, envUp: false, envPer: 0, envTimer: 0,
    freq: 0, timer: 1, pos: 0, duty: 0,
    sweepPer: 0, sweepNeg: false, sweepShift: 0, sweepTimer: 0, sweepEn: false, shadow: 0, negUsed: false,
    volCode: 0, sample: 0,
    shift: 0, width7: false, div: 0, lfsr: 0x7FFF,
  };
}

export class APU {
  constructor(gb) {
    this.gb = gb;
    this.regs = new Uint8Array(0x20);
    this.wave = new Uint8Array(16);
    this.bufSize = 16384;
    this.bufL = new Float32Array(this.bufSize);
    this.bufR = new Float32Array(this.bufSize);
    this.wpos = 0; this.rpos = 0;
    this.setSampleRate(48000);
    this.reset();
  }

  setSampleRate(rate) {
    this.sampleRate = rate;
    this.dotsPerSample = CLOCK / rate;
    this.charge = Math.pow(0.999958, this.dotsPerSample);
    if (this.time !== undefined) {
      this.sampleBase = this.time; this.sampleIdx = 0; this.lastSampleAt = 0;
      this.nextSampleAt = this.time + Math.round(this.dotsPerSample);
      this.accL = this.accR = 0;
    }
  }

  reset() {
    this.c = [channel(), channel(), channel(), channel()];
    this.c[2].maxLen = 256;
    this.power = true;
    this.regs.fill(0);
    this.seqStep = 0;
    this.time = this.gb.dotClock || 0;
    this.sampleIdx = 0; this.sampleBase = this.time;
    this.nextSampleAt = this.time + Math.round(this.dotsPerSample);
    this.accL = 0; this.accR = 0; this.capL = 0; this.capR = 0;
    this.curL = 0; this.curR = 0;
    this.wpos = this.rpos = 0;
    // valores deixados pelo boot ROM
    const init = [0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
      0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1];
    for (let k = 0; k < init.length; k++) this.regs[k] = init[k];
    this.c[0].on = true; this.c[0].dac = true; this.c[0].duty = 2; this.c[0].envInit = 0xF; this.c[0].envPer = 3;
    this.c[0].vol = 0; this.c[0].freq = 0x7FF;
    this.mix();
  }

  // ------------------------------------------------------------ registradores
  read(a) {
    const i = a - 0xFF10;
    if (a >= 0xFF30) {
      this.catchUp();
      const w = this.c[2];
      if (!w.on) return this.wave[a - 0xFF30];
      // no DMG só dá para ler a onda no exato ciclo em que o canal busca um byte; no GBC, sempre o byte atual
      if (!this.gb.cgb && !this.waveJustRead()) return 0xFF;
      return this.wave[w.pos >> 1];
    }
    if (i === 0x16) {
      this.catchUp();
      let s = (this.power ? 0x80 : 0) | 0x70;
      for (let k = 0; k < 4; k++) if (this.c[k].on) s |= 1 << k;
      return s;
    }
    return this.regs[i] | READ_OR[i];
  }

  write(a, v) {
    this.catchUp();
    if (a >= 0xFF30) {
      const w = this.c[2];
      if (!w.on) this.wave[a - 0xFF30] = v;
      else if (this.gb.cgb || this.waveJustRead()) this.wave[w.pos >> 1] = v;
      return;
    }
    const i = a - 0xFF10;
    if (i === 0x16) {
      const on = (v & 0x80) !== 0;
      if (this.power && !on) {
        for (let k = 0; k < 0x16; k++) this.regs[k] = 0;
        const lens = this.c.map(ch => ch.len);
        this.c = [channel(), channel(), channel(), channel()];
        this.c[2].maxLen = 256;
        // no DMG os contadores de duração não são zerados ao desligar o som
        if (!this.gb.cgb) for (let k = 0; k < 4; k++) this.c[k].len = lens[k];
      } else if (!this.power && on) {
        this.seqStep = 0;
      }
      this.power = on;
      this.mix();
      return;
    }
    if (!this.power) {
      // DMG: com o som desligado, ainda aceita escrever a duração (NRx1)
      if (!this.gb.cgb) {
        if (i === 0x01 || i === 0x06 || i === 0x10) this.c[i === 1 ? 0 : i === 6 ? 1 : 3].len = 64 - (v & 63);
        else if (i === 0x0B) this.c[2].len = 256 - v;
      }
      return;
    }
    if (i >= 0x17) { this.regs[i] = v; return; }
    this.regs[i] = v;
    const c = this.c;
    switch (i) {
      case 0x00: {
        const ch = c[0];
        ch.sweepPer = (v >> 4) & 7; ch.sweepShift = v & 7;
        const neg = (v & 8) !== 0;
        if (ch.sweepNeg && !neg && ch.negUsed) ch.on = false;
        ch.sweepNeg = neg;
        break;
      }
      case 0x01: case 0x06: { const ch = c[i === 1 ? 0 : 1]; ch.duty = v >> 6; ch.len = 64 - (v & 63); break; }
      case 0x02: case 0x07: case 0x11: this.writeEnv(c[i === 2 ? 0 : i === 7 ? 1 : 3], v); break;
      case 0x03: case 0x08: case 0x0D: { const ch = c[i === 3 ? 0 : i === 8 ? 1 : 2]; ch.freq = (ch.freq & 0x700) | v; break; }
      case 0x04: case 0x09: case 0x0E: case 0x13: {
        const k = i === 4 ? 0 : i === 9 ? 1 : i === 0x0E ? 2 : 3;
        const ch = c[k];
        if (k < 3) ch.freq = (ch.freq & 0xFF) | ((v & 7) << 8);
        this.writeControl(k, ch, v);
        break;
      }
      case 0x0A: { const ch = c[2]; ch.dac = (v & 0x80) !== 0; if (!ch.dac) ch.on = false; break; }
      case 0x0B: c[2].len = 256 - v; break;
      case 0x0C: c[2].volCode = (v >> 5) & 3; break;
      case 0x10: c[3].len = 64 - (v & 63); break;
      case 0x12: { const ch = c[3]; ch.shift = v >> 4; ch.width7 = (v & 8) !== 0; ch.div = v & 7; break; }
      default: break;
    }
    this.mix();
  }

  writeEnv(ch, v) {
    ch.envInit = v >> 4; ch.envUp = (v & 8) !== 0; ch.envPer = v & 7;
    ch.dac = (v & 0xF8) !== 0;
    if (!ch.dac) ch.on = false;
  }

  writeControl(k, ch, v) {
    // o contador de duração é decrementado a mais quando ligado na metade do período que não o conta
    const oddNext = (this.seqStep & 1) === 1;
    const wasEn = ch.lenEn;
    ch.lenEn = (v & 0x40) !== 0;
    if (!wasEn && ch.lenEn && oddNext && ch.len > 0) {
      ch.len--;
      if (ch.len === 0 && !(v & 0x80)) ch.on = false;
    }
    if (!(v & 0x80)) return;
    // disparo
    ch.on = ch.dac;
    if (ch.len === 0) { ch.len = ch.maxLen; if (ch.lenEn && oddNext) ch.len--; }
    if (k === 2) {
      // DMG: redisparar a onda bem quando ela busca um byte corrompe o começo da memória de onda
      if (!this.gb.cgb && ch.on && ch.timer <= 2) {
        const b = ((ch.pos + 1) & 31) >> 1;
        if (b < 4) this.wave[0] = this.wave[b];
        else { const base = b & ~3; for (let j = 0; j < 4; j++) this.wave[j] = this.wave[base + j]; }
      }
      ch.timer = this.period(2) + 6; ch.pos = 0;
    }
    else {
      ch.timer = this.period(k);
      ch.vol = ch.envInit; ch.envTimer = ch.envPer || 8;
      if (k === 3) ch.lfsr = 0x7FFF;
    }
    if (k === 0) {
      ch.shadow = ch.freq; ch.negUsed = false;
      ch.sweepTimer = ch.sweepPer || 8;
      ch.sweepEn = ch.sweepPer > 0 || ch.sweepShift > 0;
      if (ch.sweepShift) this.sweepCalc(ch);
    }
  }

  waveJustRead() { const w = this.c[2]; return this.period(2) - w.timer < 2; }

  period(k) {
    const ch = this.c[k];
    if (k === 2) return (2048 - ch.freq) * 2;
    if (k === 3) return (ch.div ? ch.div * 16 : 8) << ch.shift;
    return (2048 - ch.freq) * 4;
  }

  sweepCalc(ch) {
    let f = ch.shadow >> ch.sweepShift;
    if (ch.sweepNeg) { f = ch.shadow - f; ch.negUsed = true; } else f = ch.shadow + f;
    if (f > 2047) ch.on = false;
    return f;
  }

  // ------------------------------------------------------------ sequenciador de quadros (512 Hz, via DIV)
  frameStep() {
    this.catchUp();
    if (!this.power) return;
    const s = this.seqStep;
    this.seqStep = (s + 1) & 7;
    const c = this.c;
    if (!(s & 1)) {
      for (let k = 0; k < 4; k++) {
        const ch = c[k];
        if (ch.lenEn && ch.len > 0 && --ch.len === 0) ch.on = false;
      }
    }
    if (s === 2 || s === 6) {
      const ch = c[0];
      if (--ch.sweepTimer <= 0) {
        ch.sweepTimer = ch.sweepPer || 8;
        if (ch.sweepEn && ch.sweepPer) {
          const f = this.sweepCalc(ch);
          if (f <= 2047 && ch.sweepShift) { ch.freq = ch.shadow = f; this.sweepCalc(ch); }
        }
      }
    }
    if (s === 7) {
      for (const k of [0, 1, 3]) {
        const ch = c[k];
        if (!ch.envPer) continue;
        if (--ch.envTimer <= 0) {
          ch.envTimer = ch.envPer;
          if (ch.envUp && ch.vol < 15) ch.vol++;
          else if (!ch.envUp && ch.vol > 0) ch.vol--;
        }
      }
    }
    this.mix();
  }

  // ------------------------------------------------------------ geração
  digital(k) {
    const ch = this.c[k];
    if (!ch.on) return 0;
    if (k < 2) return DUTY[ch.duty][ch.pos] ? ch.vol : 0;
    if (k === 2) return ch.volCode ? ch.sample >> (ch.volCode - 1) : 0;
    return (ch.lfsr & 1) ? 0 : ch.vol;
  }

  mix() {
    if (!this.power) { this.curL = this.curR = 0; return; }
    const nr50 = this.regs[0x14], nr51 = this.regs[0x15];
    let L = 0, R = 0;
    for (let k = 0; k < 4; k++) {
      const ch = this.c[k];
      if (!ch.dac) continue;
      const a = this.digital(k) / 7.5 - 1;
      if (nr51 & (0x10 << k)) L += a;
      if (nr51 & (1 << k)) R += a;
    }
    this.curL = L * (((nr50 >> 4) & 7) + 1) / 32;
    this.curR = R * ((nr50 & 7) + 1) / 32;
  }

  catchUp() {
    const end = this.gb.dotClock;
    const c = this.c;
    while (this.time < end) {
      let dt = Math.min(end, this.nextSampleAt) - this.time;
      for (let k = 0; k < 4; k++) if (c[k].on && c[k].timer < dt) dt = c[k].timer;
      if (dt < 1) dt = 1;
      this.accL += this.curL * dt; this.accR += this.curR * dt;
      this.time += dt;
      let changed = false;
      for (let k = 0; k < 4; k++) {
        const ch = c[k];
        ch.timer -= dt;
        if (ch.timer > 0) continue;
        const p = this.period(k);
        if (!ch.on) { ch.timer = p; continue; }
        while (ch.timer <= 0) {
          ch.timer += p;
          if (k < 2) ch.pos = (ch.pos + 1) & 7;
          else if (k === 2) {
            ch.pos = (ch.pos + 1) & 31;
            const b = this.wave[ch.pos >> 1];
            ch.sample = (ch.pos & 1) ? b & 0xF : b >> 4;
          } else {
            const x = (ch.lfsr ^ (ch.lfsr >> 1)) & 1;
            ch.lfsr = (ch.lfsr >> 1) | (x << 14);
            if (ch.width7) ch.lfsr = (ch.lfsr & ~0x40) | (x << 6);
          }
        }
        changed = true;
      }
      if (changed) this.mix();
      if (this.time >= this.nextSampleAt) this.emit();
    }
  }

  emit() {
    const span = this.nextSampleAt - (this.lastSampleAt || (this.nextSampleAt - this.dotsPerSample));
    this.lastSampleAt = this.nextSampleAt;
    this.sampleIdx++;
    this.nextSampleAt = this.sampleBase + Math.round((this.sampleIdx + 1) * this.dotsPerSample);
    const inL = this.accL / span, inR = this.accR / span;
    this.accL = 0; this.accR = 0;
    // passa-alta (capacitor): tira o nível contínuo dos DACs
    const outL = inL - this.capL, outR = inR - this.capR;
    this.capL = inL - outL * this.charge; this.capR = inR - outR * this.charge;
    const wp = this.wpos;
    this.bufL[wp] = outL; this.bufR[wp] = outR;
    this.wpos = (wp + 1) % this.bufSize;
    if (this.wpos === this.rpos) this.rpos = (this.rpos + 1) % this.bufSize;
  }

  available() { return (this.wpos - this.rpos + this.bufSize) % this.bufSize; }

  pull(outL, outR) {
    const n = outL.length;
    for (let k = 0; k < n; k++) {
      if (this.rpos === this.wpos) {
        const last = (this.rpos - 1 + this.bufSize) % this.bufSize;
        outL[k] = this.bufL[last] * 0.98; outR[k] = this.bufR[last] * 0.98;
        this.bufL[last] *= 0.98; this.bufR[last] *= 0.98;
        continue;
      }
      outL[k] = this.bufL[this.rpos]; outR[k] = this.bufR[this.rpos];
      this.rpos = (this.rpos + 1) % this.bufSize;
    }
  }

  // ------------------------------------------------------------ save state
  state() {
    this.catchUp();
    return { regs: this.regs.slice(), wave: this.wave.slice(), c: this.c.map(ch => ({ ...ch })), power: this.power, seq: this.seqStep };
  }
  restore(s) {
    this.regs.set(s.regs); this.wave.set(s.wave);
    this.c = s.c.map(ch => ({ ...ch }));
    this.power = s.power; this.seqStep = s.seq;
    this.time = this.gb.dotClock; this.sampleBase = this.time; this.sampleIdx = 0;
    this.lastSampleAt = 0; this.nextSampleAt = this.time + Math.round(this.dotsPerSample);
    this.accL = this.accR = 0;
    this.mix();
  }
}
