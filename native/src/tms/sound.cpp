#include "sound.h"

#include <cmath>

namespace indigo::tms {

namespace {
constexpr double CLOCK = 3579545;  // CPU; o SN76489 conta a CPU/16 e o AY (CPU/2) a cada 8 do seu relógio
// SN76489: passos de 2 dB, 15 = mudo
struct VolTable {
  float v[16];
  VolTable() { for (int i = 0; i < 15; i++) v[i] = static_cast<float>(std::pow(10.0, -0.1 * i)); v[15] = 0; }
};
const VolTable SN_VOL;
// AY-3-8910: níveis medidos do conversor do chip (normalizados; tabela pública do ayumi)
const float AY_VOL[16] = {0.0f,          0.00999466f, 0.01445029f, 0.02105745f, 0.03070115f, 0.04554818f,
                          0.06449989f,   0.10736248f, 0.12658885f, 0.20498970f, 0.29221027f, 0.37283894f,
                          0.49253071f,   0.63532464f, 0.80558480f, 1.0f};
}  // namespace

Sound::Sound() : bufL(bufSize) {
  setSampleRate(48000);
  reset();
}

void Sound::reset() {
  for (int c = 0; c < 3; c++) tone[c] = 0;
  for (int c = 0; c < 4; c++) { counter[c] = 0; out[c] = 1; vol[c] = 15; }
  noise = 0; lfsr = 0x4000; latch = 0;
  for (auto& r : ayReg) r = 0;
  ayReg[7] = 0xFF;
  ayLatch = 0; ayOn = false;
  for (int c = 0; c < 3; c++) { ayCnt[c] = 0; ayOut[c] = 0; }
  ayNoiseCnt = 0; ayNoisePre = 0; ayRng = 1;
  envCnt = 0; envPos = 0; envAttack = 0; envHoldVol = 0; envHold = true;
  cycles = 0; sub = 0;
  acc = 0; accN = 0; frac = 0; cap = 0;
  wpos = rpos = 0;
}

void Sound::setSampleRate(int rate) { ticksPerSample = CLOCK / 16 / (rate > 0 ? rate : 48000); }

void Sound::snWrite(int v) {
  if (v & 0x80) latch = (v >> 4) & 7;
  const int ch = latch >> 1;
  if (latch & 1) vol[ch] = v & 0xF;
  else if (ch < 3) {
    if (v & 0x80) tone[ch] = (tone[ch] & 0x3F0) | (v & 0xF);
    else tone[ch] = (tone[ch] & 0xF) | ((v & 0x3F) << 4);
  } else { noise = v & 7; lfsr = 0x4000; }
}

void Sound::ayWrite(int v) {
  static const uint8_t MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                   0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
  ayReg[ayLatch] = static_cast<uint8_t>(v & MASK[ayLatch]);
  ayOn = true;
  if (ayLatch == 13) {  // nova forma de envelope: recomeça
    envCnt = 0; envPos = 0; envHold = false;
    envAttack = (ayReg[13] & 4) ? 1 : 0;
  }
}

void Sound::run(int64_t toCycle) {
  int64_t n = toCycle - cycles;
  if (n <= 0) return;
  cycles = toCycle;
  n += sub;
  int64_t ticks = n >> 4;
  sub = static_cast<int>(n & 15);
  while (ticks-- > 0) tick();
}

void Sound::tick() {
  // ---- SN76489
  for (int c = 0; c < 3; c++) {
    if (--counter[c] <= 0) {
      counter[c] = tone[c] ? tone[c] : 0x400;
      out[c] ^= 1;
    }
  }
  if (--counter[3] <= 0) {
    const int r = noise & 3;
    counter[3] = r == 3 ? (tone[2] ? tone[2] : 0x400) : (0x10 << r);
    out[3] ^= 1;
    if (out[3]) {
      const int fb = (noise & 4) ? ((lfsr ^ (lfsr >> 1)) & 1) : (lfsr & 1);
      lfsr = (lfsr >> 1) | (fb << 14);
    }
  }
  double s = 0;
  for (int c = 0; c < 4; c++) {
    const float v = SN_VOL.v[vol[c]];
    s += (c < 3 ? out[c] : (lfsr & 1)) ? v : -v;
  }
  // ---- AY-3-8910
  if (ayOn) {
    for (int c = 0; c < 3; c++) {
      int per = ayReg[c * 2] | (ayReg[c * 2 + 1] << 8);
      if (!per) per = 1;
      if (++ayCnt[c] >= per) { ayCnt[c] = 0; ayOut[c] ^= 1; }
    }
    int np = ayReg[6] ? ayReg[6] : 1;
    if (++ayNoiseCnt >= np) {
      ayNoiseCnt = 0;
      ayNoisePre ^= 1;
      if (!ayNoisePre) {
        const int bit = (ayRng ^ (ayRng >> 3)) & 1;
        ayRng = (ayRng >> 1) | (bit << 16);
      }
    }
    const int ep = ayReg[11] | (ayReg[12] << 8);
    if (!envHold && ++envCnt >= 2 * (ep ? ep : 1)) {
      envCnt = 0;
      if (++envPos > 15) {
        const int shape = ayReg[13];
        if (!(shape & 8)) { envHold = true; envHoldVol = 0; }
        else if (shape & 1) { envHold = true; envHoldVol = (envAttack ^ ((shape >> 1) & 1)) ? 15 : 0; }
        else { envPos = 0; if (shape & 2) envAttack ^= 1; }
      }
    }
    const int env = envHold ? envHoldVol : (envAttack ? envPos : 15 - envPos);
    const int mix = ayReg[7];
    for (int c = 0; c < 3; c++) {
      const bool on = (ayOut[c] || (mix & (1 << c))) && ((ayRng & 1) || (mix & (8 << c)));
      if (!on) continue;
      const int lv = (ayReg[8 + c] & 0x10) ? env : (ayReg[8 + c] & 15);
      s += 2 * AY_VOL[lv];
    }
  }
  acc += s; accN++;
  frac += 1;
  if (frac >= ticksPerSample) { frac -= ticksPerSample; emit(); }
}

void Sound::emit() {
  const double in = acc / (accN ? accN : 1) * 0.3;
  acc = 0; accN = 0;
  const double o = in - cap;  // passa-alta: tira o nível contínuo
  cap += o * 0.0015;
  bufL[wpos] = static_cast<float>(o);
  wpos = (wpos + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void Sound::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { L[k] = 0; R[k] = 0; continue; }
    L[k] = R[k] = bufL[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void Sound::sync(StateIO& io) {
  io.raw(tone, sizeof tone); io.raw(counter, sizeof counter); io.raw(out, sizeof out); io.raw(vol, sizeof vol);
  io.sync(noise); io.sync(lfsr); io.sync(latch);
  io.raw(ayReg, sizeof ayReg); io.sync(ayLatch); io.sync(ayOn);
  io.raw(ayCnt, sizeof ayCnt); io.raw(ayOut, sizeof ayOut);
  io.sync(ayNoiseCnt); io.sync(ayNoisePre); io.sync(ayRng);
  io.sync(envCnt); io.sync(envPos); io.sync(envAttack); io.sync(envHoldVol); io.sync(envHold);
  io.sync(cycles); io.sync(sub); io.sync(frac); io.sync(acc); io.sync(accN); io.sync(cap);
  if (io.reading()) {
    for (int k = 0; k < 3; k++) tone[k] &= 0x3FF;
    for (int k = 0; k < 4; k++) { clampTo(counter[k], -1, 0x10000); out[k] &= 1; vol[k] &= 15; }
    noise &= 7; lfsr &= 0x7FFF; latch &= 7;
    static const uint8_t MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                     0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
    for (int k = 0; k < 16; k++) ayReg[k] &= MASK[k];
    ayLatch &= 15;
    for (int k = 0; k < 3; k++) { clampTo(ayCnt[k], 0, 0x10000); ayOut[k] &= 1; }
    clampTo(ayNoiseCnt, 0, 64); ayNoisePre &= 1; ayRng &= 0x1FFFF;
    clampTo(envCnt, 0, 0x20000); clampTo(envPos, 0, 15); envAttack &= 1; envHoldVol = envHoldVol ? 15 : 0;
    sub &= 15;
    clampTo<int64_t>(cycles, 0, int64_t(1) << 60); clampTo(accN, 0, 1 << 20);
    finiteOr(frac, 0); clampTo(frac, 0.0, ticksPerSample);
    finiteOr(acc, 0); finiteOr(cap, 0);
    clampTo(acc, -1e9, 1e9); clampTo(cap, -100.0, 100.0);
  }
}

}  // namespace indigo::tms
