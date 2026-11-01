#include "psg.h"

#include <cmath>

namespace indigo::sms {

namespace {
constexpr double CLOCK = 3579545;
// volumes como o Float32Array da versão web (o arredondamento para float faz parte do resultado)
struct VolTable {
  float v[16];
  VolTable() { for (int i = 0; i < 15; i++) v[i] = static_cast<float>(std::pow(10.0, -0.1 * i)); v[15] = 0; }
};
const VolTable VOL;
}  // namespace

PSG::PSG() : bufL(bufSize), bufR(bufSize) {
  setSampleRate(48000);
  reset();
}

void PSG::reset() {
  for (int c = 0; c < 3; c++) tone[c] = 0;
  for (int c = 0; c < 4; c++) { counter[c] = 0; out[c] = 1; vol[c] = 15; }
  noise = 0; lfsr = 0x8000; latch = 0;
  stereo = 0xFF;
  cycles = 0; sub = 0;
  accL = 0; accR = 0; accN = 0; frac = 0;
  capL = 0; capR = 0;
  wpos = rpos = 0;
}

void PSG::setSampleRate(int rate) { ticksPerSample = CLOCK / 16 / rate; }

void PSG::write(int v) {
  if (v & 0x80) {
    latch = (v >> 4) & 7;
    const int ch = latch >> 1;
    if (latch & 1) vol[ch] = v & 0xF;
    else if (ch < 3) tone[ch] = (tone[ch] & 0x3F0) | (v & 0xF);
    else { noise = v & 7; lfsr = 0x8000; }
  } else {
    const int ch = latch >> 1;
    if (latch & 1) vol[ch] = v & 0xF;
    else if (ch < 3) tone[ch] = (tone[ch] & 0xF) | ((v & 0x3F) << 4);
    else { noise = v & 7; lfsr = 0x8000; }
  }
}

// avança o chip até o ciclo de CPU indicado
void PSG::run(int64_t toCycle) {
  int64_t n = toCycle - cycles;
  if (n <= 0) return;
  cycles = toCycle;
  n += sub;
  int64_t ticks = n >> 4;
  sub = static_cast<int>(n & 15);
  while (ticks-- > 0) {
    for (int c = 0; c < 3; c++) {
      if (--counter[c] <= 0) {
        counter[c] = tone[c] ? tone[c] : 1;
        out[c] = tone[c] > 1 ? out[c] ^ 1 : 1;
      }
    }
    if (--counter[3] <= 0) {
      const int r = noise & 3;
      counter[3] = r == 3 ? (tone[2] ? tone[2] : 1) : (0x10 << r);
      out[3] ^= 1;
      if (out[3]) {
        const int fb = (noise & 4) ? ((lfsr & 1) ^ ((lfsr >> 3) & 1)) : (lfsr & 1);
        lfsr = (lfsr >> 1) | (fb << 15);
      }
    }
    double L = 0, R = 0;
    for (int c = 0; c < 4; c++) {
      const float s = (c < 3 ? out[c] : (lfsr & 1)) ? VOL.v[vol[c]] : -VOL.v[vol[c]];
      if (stereo & (0x10 << c)) L += s;
      if (stereo & (1 << c)) R += s;
    }
    accL += L; accR += R; accN++;
    frac += 1;
    if (frac >= ticksPerSample) { frac -= ticksPerSample; emit(); }
  }
}

void PSG::emit() {
  const double inL = accL / (accN ? accN : 1) * 0.3, inR = accR / (accN ? accN : 1) * 0.3;
  accL = accR = 0; accN = 0;
  // passa-alta: tira o nível contínuo
  const double outL = inL - capL, outR = inR - capR;
  capL += outL * 0.0015; capR += outR * 0.0015;
  const size_t wp = wpos;
  bufL[wp] = static_cast<float>(outL); bufR[wp] = static_cast<float>(outR);
  wpos = (wp + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void PSG::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { L[k] = 0; R[k] = 0; continue; }
    L[k] = bufL[rpos]; R[k] = bufR[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void PSG::sync(StateIO& io) {
  io.raw(tone, sizeof tone); io.raw(counter, sizeof counter); io.raw(out, sizeof out); io.raw(vol, sizeof vol);
  io.sync(noise); io.sync(lfsr); io.sync(latch); io.sync(stereo);
  io.sync(cycles); io.sync(sub); io.sync(frac); io.sync(accL); io.sync(accR); io.sync(accN); io.sync(capL); io.sync(capR);
  if (io.reading()) {
    for (int k = 0; k < 3; k++) tone[k] &= 0x3FF;
    for (int k = 0; k < 4; k++) { clampTo(counter[k], -1, 0x10000); out[k] &= 1; vol[k] &= 15; }
    noise &= 7; lfsr &= 0xFFFF; latch &= 7; stereo &= 0xFF; sub &= 15;
    clampTo<int64_t>(cycles, 0, int64_t(1) << 60); clampTo(accN, 0, 1 << 20);
    finiteOr(frac, 0); clampTo(frac, 0.0, ticksPerSample);
    finiteOr(accL, 0); finiteOr(accR, 0); finiteOr(capL, 0); finiteOr(capR, 0);
  }
}

}  // namespace indigo::sms
