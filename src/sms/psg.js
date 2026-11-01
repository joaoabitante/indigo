// Som do Master System / Game Gear (SN76489): 3 canais de onda quadrada + ruído (branco ou periódico),
// volume em passos de 2 dB. No Game Gear, a porta 0x06 liga cada canal no lado esquerdo/direito.
// O chip anda a clock/16; cada amostra de saída é a média dos passos do chip (sem serrilhado).

const CLOCK = 3579545;
const VOL = new Float32Array(16);
for (let i = 0; i < 15; i++) VOL[i] = Math.pow(10, -0.1 * i);
VOL[15] = 0;

export class PSG {
  constructor() {
    this.bufSize = 16384;
    this.bufL = new Float32Array(this.bufSize); this.bufR = new Float32Array(this.bufSize);
    this.setSampleRate(48000);
    this.reset();
  }

  reset() {
    this.tone = new Uint16Array(3); this.counter = new Int32Array(4); this.out = new Uint8Array(4).fill(1);
    this.vol = new Uint8Array(4).fill(15);
    this.noise = 0; this.lfsr = 0x8000; this.latch = 0;
    this.stereo = 0xFF;
    this.cycles = 0;      // ciclos de CPU já processados
    this.sub = 0;         // resto de ciclos (<16)
    this.accL = 0; this.accR = 0; this.accN = 0; this.frac = 0;
    this.capL = 0; this.capR = 0;
    this.wpos = this.rpos = 0;
  }

  setSampleRate(rate) {
    this.rate = rate;
    this.ticksPerSample = CLOCK / 16 / rate;
  }

  write(v) {
    if (v & 0x80) {
      this.latch = (v >> 4) & 7;
      const ch = this.latch >> 1;
      if (this.latch & 1) this.vol[ch] = v & 0xF;
      else if (ch < 3) this.tone[ch] = (this.tone[ch] & 0x3F0) | (v & 0xF);
      else { this.noise = v & 7; this.lfsr = 0x8000; }
    } else {
      const ch = this.latch >> 1;
      if (this.latch & 1) this.vol[ch] = v & 0xF;
      else if (ch < 3) this.tone[ch] = (this.tone[ch] & 0xF) | ((v & 0x3F) << 4);
      else { this.noise = v & 7; this.lfsr = 0x8000; }
    }
  }

  // avança o chip até o ciclo de CPU indicado
  run(toCycle) {
    let n = toCycle - this.cycles;
    if (n <= 0) return;
    this.cycles = toCycle;
    n += this.sub;
    let ticks = n >> 4;
    this.sub = n & 15;
    const tone = this.tone, counter = this.counter, out = this.out, vol = this.vol;
    while (ticks-- > 0) {
      for (let c = 0; c < 3; c++) {
        if (--counter[c] <= 0) {
          counter[c] = tone[c] || 1;
          out[c] = tone[c] > 1 ? out[c] ^ 1 : 1;
        }
      }
      if (--counter[3] <= 0) {
        const r = this.noise & 3;
        counter[3] = r === 3 ? (tone[2] || 1) : (0x10 << r);
        out[3] ^= 1;
        if (out[3]) {
          const fb = (this.noise & 4) ? ((this.lfsr & 1) ^ ((this.lfsr >> 3) & 1)) : (this.lfsr & 1);
          this.lfsr = (this.lfsr >> 1) | (fb << 15);
        }
      }
      let L = 0, R = 0;
      for (let c = 0; c < 4; c++) {
        const s = (c < 3 ? out[c] : (this.lfsr & 1)) ? VOL[vol[c]] : -VOL[vol[c]];
        if (this.stereo & (0x10 << c)) L += s;
        if (this.stereo & (1 << c)) R += s;
      }
      this.accL += L; this.accR += R; this.accN++;
      this.frac += 1;
      if (this.frac >= this.ticksPerSample) { this.frac -= this.ticksPerSample; this.emit(); }
    }
  }

  emit() {
    const inL = this.accL / (this.accN || 1) * 0.3, inR = this.accR / (this.accN || 1) * 0.3;
    this.accL = this.accR = 0; this.accN = 0;
    // passa-alta: tira o nível contínuo
    const outL = inL - this.capL, outR = inR - this.capR;
    this.capL += outL * 0.0015; this.capR += outR * 0.0015;
    const wp = this.wpos;
    this.bufL[wp] = outL; this.bufR[wp] = outR;
    this.wpos = (wp + 1) % this.bufSize;
    if (this.wpos === this.rpos) this.rpos = (this.rpos + 1) % this.bufSize;
  }

  available() { return (this.wpos - this.rpos + this.bufSize) % this.bufSize; }
  pull(L, R) {
    for (let k = 0; k < L.length; k++) {
      if (this.rpos === this.wpos) { L[k] = 0; R[k] = 0; continue; }
      L[k] = this.bufL[this.rpos]; R[k] = this.bufR[this.rpos];
      this.rpos = (this.rpos + 1) % this.bufSize;
    }
  }

  state() {
    return { tone: Array.from(this.tone), counter: Array.from(this.counter), out: Array.from(this.out), vol: Array.from(this.vol),
      s: [this.noise, this.lfsr, this.latch, this.stereo] };
  }
  restore(st) {
    this.tone.set(st.tone); this.counter.set(st.counter); this.out.set(st.out); this.vol.set(st.vol);
    [this.noise, this.lfsr, this.latch, this.stereo] = st.s;
  }
}
