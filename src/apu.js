// APU — 4 canais PSG (estilo Game Boy) + 2 canais DirectSound (FIFO A/B)

const CLOCK = 16777216;
const DUTY = [
  [0, 0, 0, 0, 0, 0, 0, 1],
  [1, 0, 0, 0, 0, 0, 0, 1],
  [1, 0, 0, 0, 0, 1, 1, 1],
  [0, 1, 1, 1, 1, 1, 1, 0],
];

class Fifo {
  constructor() { this.buf = new Int8Array(32); this.r = 0; this.w = 0; this.count = 0; this.out = 0; }
  push(b) { if (this.count >= 32) return; this.buf[this.w] = b; this.w = (this.w + 1) & 31; this.count++; }
  pop() {
    if (this.count === 0) return this.out;
    this.out = this.buf[this.r]; this.r = (this.r + 1) & 31; this.count--; return this.out;
  }
  reset() { this.r = this.w = this.count = 0; }
}

function makeChannel() {
  return {
    on: false, vol: 0, envDir: 0, envStep: 0, envTimer: 0,
    length: 0, lenEnable: false, freq: 0, timer: 0, pos: 0, duty: 0,
    sweepTime: 0, sweepDir: 0, sweepShift: 0, sweepTimer: 0, shadow: 0,
    lfsr: 0x7FFF, width7: false, out: 0,
  };
}

export class APU {
  constructor(gba) {
    this.gba = gba;
    this.sampleRate = 48000;
    this.cyclesPerSample = CLOCK / this.sampleRate;
    this.ch = [makeChannel(), makeChannel(), makeChannel(), makeChannel()];
    this.wave = new Uint8Array(32); // 2 bancos x 16 bytes
    this.fifoA = new Fifo(); this.fifoB = new Fifo();
    this.sampleCycle = 0;
    this.seqCycle = 0;
    this.seqStep = 0;
    // buffer circular estéreo
    this.bufSize = 16384;
    this.bufL = new Float32Array(this.bufSize);
    this.bufR = new Float32Array(this.bufSize);
    this.wpos = 0; this.rpos = 0;
  }

  setSampleRate(rate) {
    this.sampleRate = rate;
    this.cyclesPerSample = CLOCK / rate;
  }

  reset() {
    this.ch = [makeChannel(), makeChannel(), makeChannel(), makeChannel()];
    this.fifoA.reset(); this.fifoB.reset();
    this.fifoA.out = 0; this.fifoB.out = 0;
    this.sampleCycle = 0; this.seqCycle = 0; this.seqStep = 0;
    this.wpos = this.rpos = 0;
  }

  get regs() { return this.gba.io.regs; }

  statusBits() {
    let s = 0;
    for (let k = 0; k < 4; k++) if (this.ch[k].on) s |= 1 << k;
    return s;
  }

  readReg(a, raw) {
    switch (a) {
      case 0x60: return raw & 0x7F;
      case 0x62: case 0x68: return raw & 0xFFC0;
      case 0x64: case 0x6C: case 0x74: case 0x7C: return raw & 0x4000;
      case 0x70: return raw & 0xE0;
      case 0x72: return raw & 0xE000;
      case 0x78: return raw & 0xFF00;
      case 0x80: return raw & 0xFF77;
      case 0x82: return raw & 0x770F;
      case 0x88: return raw;
      default:
        if (a >= 0x90 && a < 0xA0) {
          const bank = (this.regs[0x70 >> 1] & 0x40) ? 0 : 16;
          const o = bank + (a - 0x90);
          return this.wave[o] | (this.wave[o + 1] << 8);
        }
        return 0;
    }
  }

  writeReg(a, v) {
    this.catchUp();
    const master = (this.regs[0x84 >> 1] & 0x80) !== 0;
    const c = this.ch;
    switch (a) {
      case 0x60:
        c[0].sweepShift = v & 7; c[0].sweepDir = (v >>> 3) & 1; c[0].sweepTime = (v >>> 4) & 7;
        break;
      case 0x62: case 0x68: {
        const ch = a === 0x62 ? c[0] : c[1];
        ch.length = 64 - (v & 0x3F); ch.duty = (v >>> 6) & 3;
        ch.envStep = (v >>> 8) & 7; ch.envDir = (v >>> 11) & 1;
        if (((v >>> 12) & 0xF) === 0 && !ch.envDir) ch.on = false;
        break;
      }
      case 0x64: case 0x6C: {
        const ch = a === 0x64 ? c[0] : c[1];
        ch.freq = v & 0x7FF; ch.lenEnable = (v & 0x4000) !== 0;
        if ((v & 0x8000) && master) this.trigger(a === 0x64 ? 0 : 1);
        this.regs[a >> 1] = v & 0x7FFF;
        break;
      }
      case 0x70:
        if (!(v & 0x80)) c[2].on = false;
        break;
      case 0x72: c[2].length = 256 - (v & 0xFF); break;
      case 0x74:
        c[2].freq = v & 0x7FF; c[2].lenEnable = (v & 0x4000) !== 0;
        if ((v & 0x8000) && master) this.trigger(2);
        this.regs[a >> 1] = v & 0x7FFF;
        break;
      case 0x78:
        c[3].length = 64 - (v & 0x3F);
        c[3].envStep = (v >>> 8) & 7; c[3].envDir = (v >>> 11) & 1;
        if (((v >>> 12) & 0xF) === 0 && !c[3].envDir) c[3].on = false;
        break;
      case 0x7C:
        c[3].lenEnable = (v & 0x4000) !== 0; c[3].width7 = (v & 8) !== 0;
        if ((v & 0x8000) && master) this.trigger(3);
        this.regs[a >> 1] = v & 0x7FFF;
        break;
      case 0x82:
        if (v & 0x0800) { this.fifoA.reset(); }
        if (v & 0x8000) { this.fifoB.reset(); }
        this.regs[a >> 1] = v & 0x770F;
        break;
      case 0x84:
        if (!(v & 0x80)) { for (const ch of c) ch.on = false; }
        break;
      default:
        if (a >= 0x90 && a < 0xA0) {
          const bank = (this.regs[0x70 >> 1] & 0x40) ? 0 : 16;
          const o = bank + (a - 0x90);
          this.wave[o] = v & 0xFF; this.wave[o + 1] = v >>> 8;
        }
    }
  }

  trigger(n) {
    const r = this.regs, ch = this.ch[n];
    ch.on = true;
    if (n < 2) {
      const env = r[(n === 0 ? 0x62 : 0x68) >> 1];
      ch.vol = (env >>> 12) & 0xF;
      ch.envTimer = ch.envStep;
      if (ch.length === 0) ch.length = 64;
      ch.timer = (2048 - ch.freq) * 16;
      if (n === 0) {
        ch.shadow = ch.freq; ch.sweepTimer = ch.sweepTime;
        if (ch.sweepShift) this.sweepCalc(true);
      }
      if (ch.vol === 0 && !ch.envDir) ch.on = false;
    } else if (n === 2) {
      if (!(r[0x70 >> 1] & 0x80)) ch.on = false;
      if (ch.length === 0) ch.length = 256;
      ch.timer = (2048 - ch.freq) * 8;
      ch.pos = 0;
    } else {
      const env = r[0x78 >> 1];
      ch.vol = (env >>> 12) & 0xF;
      ch.envTimer = ch.envStep;
      if (ch.length === 0) ch.length = 64;
      ch.lfsr = ch.width7 ? 0x7F : 0x7FFF;
      ch.timer = this.noisePeriod();
      if (ch.vol === 0 && !ch.envDir) ch.on = false;
    }
  }

  noisePeriod() {
    const v = this.regs[0x7C >> 1];
    const r = v & 7, s = (v >>> 4) & 0xF;
    return (r === 0 ? 16 : 32 * r) << (s + 1);
  }

  sweepCalc(write) {
    const ch = this.ch[0];
    const delta = ch.shadow >>> ch.sweepShift;
    const nf = ch.sweepDir ? ch.shadow - delta : ch.shadow + delta;
    if (nf > 2047) { ch.on = false; return; }
    if (write && ch.sweepShift) { ch.shadow = nf; ch.freq = nf; }
  }

  // sequenciador de quadros: 512 Hz
  sequencer() {
    const step = this.seqStep;
    this.seqStep = (step + 1) & 7;
    const c = this.ch;
    if ((step & 1) === 0) {
      for (let k = 0; k < 4; k++) {
        const ch = c[k];
        if (ch.lenEnable && ch.length > 0) { ch.length--; if (ch.length === 0) ch.on = false; }
      }
    }
    if (step === 2 || step === 6) {
      const ch = c[0];
      if (ch.sweepTime) {
        if (--ch.sweepTimer <= 0) {
          ch.sweepTimer = ch.sweepTime;
          if (ch.on) this.sweepCalc(true);
        }
      }
    }
    if (step === 7) {
      for (const k of [0, 1, 3]) {
        const ch = c[k];
        if (!ch.envStep) continue;
        if (--ch.envTimer <= 0) {
          ch.envTimer = ch.envStep;
          if (ch.envDir && ch.vol < 15) ch.vol++;
          else if (!ch.envDir && ch.vol > 0) ch.vol--;
        }
      }
    }
  }

  // ---------------- DirectSound ----------------
  fifoWrite32(a, v) {
    const f = a === 0xA0 ? this.fifoA : this.fifoB;
    f.push(v & 0xFF); f.push((v >>> 8) & 0xFF); f.push((v >>> 16) & 0xFF); f.push((v >>> 24) & 0xFF);
  }
  fifoWrite16(a, v) {
    const f = a < 0xA4 ? this.fifoA : this.fifoB;
    f.push(v & 0xFF); f.push((v >>> 8) & 0xFF);
  }
  fifoWrite8(a, v) {
    const f = a < 0xA4 ? this.fifoA : this.fifoB;
    f.push(v & 0xFF);
  }

  timerOverflow(t) {
    const cnt = this.regs[0x82 >> 1];
    if (!(this.regs[0x84 >> 1] & 0x80)) return;
    const aT = (cnt >>> 10) & 1, bT = (cnt >>> 14) & 1;
    if (aT === t || bT === t) this.catchUp();
    if (aT === t) {
      this.fifoA.pop();
      if (this.fifoA.count <= 16) this.gba.io.soundDma(0x040000A0);
    }
    if (bT === t) {
      this.fifoB.pop();
      if (this.fifoB.count <= 16) this.gba.io.soundDma(0x040000A4);
    }
  }

  // ---------------- geração de amostras ----------------
  catchUp() {
    const now = this.gba.now();
    const cps = this.cyclesPerSample;
    while (this.sampleCycle + cps <= now) {
      this.sampleCycle += cps;
      this.genSample(cps);
    }
  }

  advanceChannels(cyc) {
    const c = this.ch;
    // canais de onda quadrada
    for (let k = 0; k < 2; k++) {
      const ch = c[k];
      if (!ch.on) continue;
      ch.timer -= cyc;
      const period = (2048 - ch.freq) * 16;
      while (ch.timer <= 0) { ch.timer += period; ch.pos = (ch.pos + 1) & 7; }
    }
    // onda programável
    const w = c[2];
    if (w.on) {
      w.timer -= cyc;
      const period = (2048 - w.freq) * 8;
      const dim = (this.regs[0x70 >> 1] & 0x20) !== 0;
      while (w.timer <= 0) { w.timer += period; w.pos = (w.pos + 1) & (dim ? 63 : 31); }
    }
    // ruído
    const nz = c[3];
    if (nz.on) {
      nz.timer -= cyc;
      const period = this.noisePeriod();
      while (nz.timer <= 0) {
        nz.timer += period;
        const bit = (nz.lfsr ^ (nz.lfsr >>> 1)) & 1;
        if (nz.width7) nz.lfsr = ((nz.lfsr >>> 1) & 0x3F) | (bit << 6);
        else nz.lfsr = ((nz.lfsr >>> 1) & 0x3FFF) | (bit << 14);
      }
    }
  }

  genSample(cyc) {
    this.seqCycle += cyc;
    while (this.seqCycle >= 32768) { this.seqCycle -= 32768; this.sequencer(); }
    this.advanceChannels(cyc);
    const r = this.regs;
    let L = 0, R = 0;
    if (r[0x84 >> 1] & 0x80) {
      const cntL = r[0x80 >> 1], cntH = r[0x82 >> 1];
      const c = this.ch;
      const outs = [0, 0, 0, 0];
      for (let k = 0; k < 2; k++) {
        const ch = c[k];
        if (ch.on) outs[k] = DUTY[ch.duty][ch.pos] ? ch.vol : -ch.vol;
      }
      const w = c[2];
      if (w.on) {
        const w72 = r[0x72 >> 1];
        const dim = (r[0x70 >> 1] & 0x20) !== 0;
        const bankStart = dim ? 0 : ((r[0x70 >> 1] & 0x40) ? 16 : 0);
        const idx = (bankStart * 2 + w.pos) & 63;
        const byte = this.wave[idx >>> 1];
        const s = (idx & 1) ? (byte & 0xF) : (byte >>> 4);
        let v = s * 2 - 15;
        if (w72 & 0x8000) v = (v * 3) >> 2;
        else switch ((w72 >>> 13) & 3) {
          case 0: v = 0; break;
          case 1: break;
          case 2: v >>= 1; break;
          case 3: v >>= 2; break;
        }
        outs[2] = v;
      }
      const nz = c[3];
      if (nz.on) outs[3] = (nz.lfsr & 1) ? -nz.vol : nz.vol;
      let pl = 0, pr = 0;
      for (let k = 0; k < 4; k++) {
        if (cntL & (0x1000 << k)) pl += outs[k];
        if (cntL & (0x100 << k)) pr += outs[k];
      }
      pl *= ((cntL >>> 4) & 7) + 1;
      pr *= (cntL & 7) + 1;
      const psgShift = [2, 1, 0, 2][cntH & 3];
      pl >>= psgShift; pr >>= psgShift;
      const a = this.fifoA.out * ((cntH & 4) ? 4 : 2);
      const b = this.fifoB.out * ((cntH & 8) ? 4 : 2);
      L = pl + ((cntH & 0x200) ? a : 0) + ((cntH & 0x2000) ? b : 0);
      R = pr + ((cntH & 0x100) ? a : 0) + ((cntH & 0x1000) ? b : 0);
    }
    const wp = this.wpos;
    this.bufL[wp] = L / 1024;
    this.bufR[wp] = R / 1024;
    this.wpos = (wp + 1) % this.bufSize;
    if (this.wpos === this.rpos) this.rpos = (this.rpos + 1) % this.bufSize;
  }

  available() { return (this.wpos - this.rpos + this.bufSize) % this.bufSize; }

  // entrega amostras ao front-end
  pull(outL, outR) {
    const n = outL.length;
    for (let k = 0; k < n; k++) {
      if (this.rpos === this.wpos) {
        const last = (this.rpos - 1 + this.bufSize) % this.bufSize;
        outL[k] = this.bufL[last] * 0.98; outR[k] = this.bufR[last] * 0.98;
        continue;
      }
      outL[k] = this.bufL[this.rpos]; outR[k] = this.bufR[this.rpos];
      this.rpos = (this.rpos + 1) % this.bufSize;
    }
  }
}
