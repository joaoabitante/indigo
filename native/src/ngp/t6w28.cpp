#include "t6w28.h"

#include <cmath>

namespace indigo::ngp {

namespace {
constexpr double CPU_CLOCK = 6144000;
// volumes como o Float32Array da versão web (o arredondamento para float faz parte do resultado)
struct VolTable {
  float v[16];
  VolTable() { for (int i = 0; i < 15; i++) v[i] = static_cast<float>(std::pow(10.0, -0.1 * i)); v[15] = 0; }
};
const VolTable VOL;
}  // namespace

T6W28::T6W28() : bufL(bufSize), bufR(bufSize) {
  setSampleRate(48000);
  reset();
  dacL = 0x80; dacR = 0x80;
}

void T6W28::reset() {
  for (int c = 0; c < 3; c++) tone[c] = 0;
  noiseTone = 0;
  for (int c = 0; c < 4; c++) { counter[c] = 0; out[c] = 1; volL[c] = 15; volR[c] = 15; }
  noiseSel = 0; white = true; lfsr = 0x4000;
  latchL = 0; latchR = 0;
  enabled = false;
  cycles = 0; sub = 0;
  accL = 0; accR = 0; accN = 0; frac = 0;
  capL = 0; capR = 0;
  wpos = rpos = 0;
}

void T6W28::setSampleRate(int rate) { ticksPerSample = CPU_CLOCK / 32 / rate; }

void T6W28::writeLeft(int v) {
  if (!enabled) return;
  if (v & 0x80) latchL = v;
  const int ch = (latchL >> 5) & 3;
  if (latchL & 0x10) volL[ch] = v & 15;
  else if (ch < 3) {
    if (v & 0x80) tone[ch] = (tone[ch] & 0x3F0) | (v & 0xF);
    else tone[ch] = (tone[ch] & 0xF) | ((v & 0x3F) << 4);
  }
}

void T6W28::writeRight(int v) {
  if (!enabled) return;
  if (v & 0x80) latchR = v;
  const int ch = (latchR >> 5) & 3;
  if (latchR & 0x10) volR[ch] = v & 15;
  else if (ch == 2) {
    if (v & 0x80) noiseTone = (noiseTone & 0x3F0) | (v & 0xF);
    else noiseTone = (noiseTone & 0xF) | ((v & 0x3F) << 4);
  } else if (ch == 3) {
    noiseSel = v & 3; white = (v & 4) != 0; lfsr = 0x4000;
  }
}

void T6W28::setEnabled(bool on) {
  if (!on) {
    const size_t w = wpos, r = rpos;
    const int64_t c = cycles;
    reset();
    wpos = w; rpos = r; cycles = c;
  }
  enabled = on;
}

// avança até o ciclo de CPU indicado
void T6W28::run(int64_t toCycle) {
  int64_t n = toCycle - cycles;
  if (n <= 0) return;
  cycles = toCycle;
  n += sub;
  int64_t ticks = n >> 5;
  sub = static_cast<int>(n & 31);
  const bool dac = ((dacL - 0x80) + (dacR - 0x80)) != 0;
  const double dl = (dacL - 0x80) / 128.0 * 0.5, dr = (dacR - 0x80) / 128.0 * 0.5;
  while (ticks-- > 0) {
    for (int c = 0; c < 3; c++) {
      if (--counter[c] <= 0) { counter[c] = tone[c] ? tone[c] : 1; out[c] = tone[c] > 1 ? out[c] ^ 1 : 1; }
    }
    if (--counter[3] <= 0) {
      counter[3] = noiseSel == 3 ? (noiseTone ? noiseTone : 1) : (16 << noiseSel);
      out[3] ^= 1;
      if (out[3]) {
        const int s = lfsr;
        const int fb = white ? (s ^ (s >> 1)) & 1 : s & 1;
        lfsr = (s >> 1) | (fb << 14);
      }
    }
    double L = 0, R = 0;
    for (int c = 0; c < 4; c++) {
      const int hi = c < 3 ? out[c] : (lfsr & 1);
      L += hi ? VOL.v[volL[c]] : -VOL.v[volL[c]];
      R += hi ? VOL.v[volR[c]] : -VOL.v[volR[c]];
    }
    accL += L * 0.25 + (dac ? dl : 0); accR += R * 0.25 + (dac ? dr : 0); accN++;
    frac += 1;
    if (frac >= ticksPerSample) { frac -= ticksPerSample; emit(); }
  }
}

void T6W28::emit() {
  const int n = accN ? accN : 1;
  const double inL = accL / n * 0.3, inR = accR / n * 0.3;
  accL = accR = 0; accN = 0;
  const double outL = inL - capL, outR = inR - capR;
  capL += outL * 0.0015; capR += outR * 0.0015;
  const size_t wp = wpos;
  bufL[wp] = static_cast<float>(outL); bufR[wp] = static_cast<float>(outR);
  wpos = (wp + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void T6W28::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { L[k] = 0; R[k] = 0; continue; }
    L[k] = bufL[rpos]; R[k] = bufR[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void T6W28::sync(StateIO& io) {
  io.raw(tone, sizeof tone); io.raw(counter, sizeof counter); io.raw(out, sizeof out);
  io.raw(volL, sizeof volL); io.raw(volR, sizeof volR);
  io.sync(noiseTone); io.sync(noiseSel); io.sync(white); io.sync(lfsr); io.sync(latchL); io.sync(latchR);
  io.sync(enabled); io.sync(dacL); io.sync(dacR);
  io.sync(cycles); io.sync(sub); io.sync(frac); io.sync(accL); io.sync(accR); io.sync(accN); io.sync(capL); io.sync(capR);
  if (io.reading()) {
    for (int k = 0; k < 3; k++) tone[k] &= 0x3FF;
    noiseTone &= 0x3FF;
    for (int k = 0; k < 4; k++) { clampTo(counter[k], -1, 0x10000); out[k] &= 1; volL[k] &= 15; volR[k] &= 15; }
    noiseSel &= 3; lfsr &= 0x7FFF; latchL &= 0xFF; latchR &= 0xFF; dacL &= 0xFF; dacR &= 0xFF; sub &= 31;
    clampTo<int64_t>(cycles, 0, int64_t(1) << 60); clampTo(accN, 0, 1 << 20);
    finiteOr(frac, 0); clampTo(frac, 0.0, ticksPerSample);
    finiteOr(accL, 0); finiteOr(accR, 0); finiteOr(capL, 0); finiteOr(capR, 0);
  }
}

}  // namespace indigo::ngp
