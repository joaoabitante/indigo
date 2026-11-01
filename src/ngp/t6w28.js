// Som do Neo Geo Pocket: T6W28 (parente do SN76489 com duas portas, uma por lado do estéreo) + dois DACs de 8 bits.
// Porta esquerda: frequência dos 3 tons e volumes da esquerda. Porta direita: volumes da direita, controle do
// ruído e uma frequência própria (canal 2 da direita) que o ruído pode usar. O chip anda a 3,072 MHz / 16.

const CPU_CLOCK = 6144000;
const VOL = new Float32Array(16);
for (let i = 0; i < 15; i++) VOL[i] = Math.pow(10, -0.1 * i);
VOL[15] = 0;

export class T6W28 {
  constructor() {
    this.bufSize = 16384;
    this.bufL = new Float32Array(this.bufSize); this.bufR = new Float32Array(this.bufSize);
    this.setSampleRate(48000);
    this.reset();
    this.dacL = 0x80; this.dacR = 0x80;
  }

  reset() {
    this.tone = new Uint16Array(3);   // períodos dos tons (porta esquerda)
    this.noiseTone = 0;               // período "extra" do ruído (canal 2 da porta direita)
    this.counter = new Int32Array(4); this.out = new Uint8Array(4).fill(1);
    this.volL = new Uint8Array(4).fill(15); this.volR = new Uint8Array(4).fill(15);
    this.noiseSel = 0; this.white = true; this.lfsr = 0x4000;
    this.latchL = 0; this.latchR = 0;
    this.enabled = false;
    this.cycles = 0; this.sub = 0;
    this.accL = 0; this.accR = 0; this.accN = 0; this.frac = 0;
    this.capL = 0; this.capR = 0;
    this.wpos = this.rpos = 0;
  }

  setSampleRate(rate) { this.rate = rate; this.ticksPerSample = CPU_CLOCK / 32 / rate; }

  writeLeft(v) {
    if (!this.enabled) return;
    if (v & 0x80) this.latchL = v;
    const ch = (this.latchL >> 5) & 3;
    if (this.latchL & 0x10) this.volL[ch] = v & 15;
    else if (ch < 3) {
      if (v & 0x80) this.tone[ch] = (this.tone[ch] & 0x3F0) | (v & 0xF);
      else this.tone[ch] = (this.tone[ch] & 0xF) | ((v & 0x3F) << 4);
    }
  }

  writeRight(v) {
    if (!this.enabled) return;
    if (v & 0x80) this.latchR = v;
    const ch = (this.latchR >> 5) & 3;
    if (this.latchR & 0x10) this.volR[ch] = v & 15;
    else if (ch === 2) {
      if (v & 0x80) this.noiseTone = (this.noiseTone & 0x3F0) | (v & 0xF);
      else this.noiseTone = (this.noiseTone & 0xF) | ((v & 0x3F) << 4);
    } else if (ch === 3) {
      this.noiseSel = v & 3; this.white = (v & 4) !== 0; this.lfsr = 0x4000;
    }
  }

  setEnabled(on) { if (!on) { const keep = [this.wpos, this.rpos, this.cycles]; this.reset(); [this.wpos, this.rpos, this.cycles] = keep; } this.enabled = on; }

  // avança até o ciclo de CPU indicado
  run(toCycle) {
    let n = toCycle - this.cycles;
    if (n <= 0) return;
    this.cycles = toCycle;
    n += this.sub;
    let ticks = n >> 5;
    this.sub = n & 31;
    const tone = this.tone, counter = this.counter, out = this.out, vl = this.volL, vr = this.volR;
    const dac = ((this.dacL - 0x80) + (this.dacR - 0x80)) !== 0;
    const dl = (this.dacL - 0x80) / 128 * 0.5, dr = (this.dacR - 0x80) / 128 * 0.5;
    while (ticks-- > 0) {
      for (let c = 0; c < 3; c++) {
        if (--counter[c] <= 0) { counter[c] = tone[c] || 1; out[c] = tone[c] > 1 ? out[c] ^ 1 : 1; }
      }
      if (--counter[3] <= 0) {
        counter[3] = this.noiseSel === 3 ? (this.noiseTone || 1) : (16 << this.noiseSel);
        out[3] ^= 1;
        if (out[3]) {
          const s = this.lfsr;
          const fb = this.white ? (s ^ (s >> 1)) & 1 : s & 1;
          this.lfsr = (s >> 1) | (fb << 14);
        }
      }
      let L = 0, R = 0;
      for (let c = 0; c < 4; c++) {
        const hi = c < 3 ? out[c] : (this.lfsr & 1);
        L += hi ? VOL[vl[c]] : -VOL[vl[c]];
        R += hi ? VOL[vr[c]] : -VOL[vr[c]];
      }
      this.accL += L * 0.25 + (dac ? dl : 0); this.accR += R * 0.25 + (dac ? dr : 0); this.accN++;
      this.frac += 1;
      if (this.frac >= this.ticksPerSample) { this.frac -= this.ticksPerSample; this.emit(); }
    }
  }

  emit() {
    const n = this.accN || 1;
    const inL = this.accL / n * 0.3, inR = this.accR / n * 0.3;
    this.accL = this.accR = 0; this.accN = 0;
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
    return { tone: Array.from(this.tone), counter: Array.from(this.counter), out: Array.from(this.out), volL: Array.from(this.volL), volR: Array.from(this.volR),
      s: [this.noiseTone, this.noiseSel, +this.white, this.lfsr, this.latchL, this.latchR, +this.enabled, this.dacL, this.dacR] };
  }
  restore(st) {
    this.tone.set(st.tone); this.counter.set(st.counter); this.out.set(st.out); this.volL.set(st.volL); this.volR.set(st.volR);
    let w, en;
    [this.noiseTone, this.noiseSel, w, this.lfsr, this.latchL, this.latchR, en, this.dacL, this.dacR] = st.s;
    this.white = !!w; this.enabled = !!en;
  }
}
