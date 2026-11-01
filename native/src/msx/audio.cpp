#include "audio.h"

#include <algorithm>

namespace indigo::msx {

namespace {
// níveis de volume do AY-3-8910 (medidos no chip, normalizados; 0 = mudo)
const float AY_VOL[16] = {0.0f,     0.00999f, 0.01445f, 0.02106f, 0.03070f, 0.04555f, 0.06450f, 0.10736f,
                          0.12658f, 0.20498f, 0.29221f, 0.37283f, 0.49253f, 0.63532f, 0.80558f, 1.0f};
constexpr double CPU_CLOCK = 3579545.0;
}  // namespace

// ------------------------------------------------------------ AY-3-8910
void AY8910::reset() {
  std::fill(regs, regs + 16, 0);
  regs[7] = 0xBF;  // como a BIOS deixa: porta A entrada, porta B saída, tudo mudo
  addr = 0;
  for (int c = 0; c < 3; c++) { toneCnt[c] = 0; toneOut[c] = 0; }
  noiseCnt = 0; noisePre = 0; rng = 1;
  envCnt = 0; envStep = 0; envAttack = 0; envHold = true; envAlt = false; envHolding = true;
}

void AY8910::writeData(int v) {
  static const uint8_t MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                   0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
  v &= MASK[addr];
  regs[addr] = static_cast<uint8_t>(v);
  if (addr == 13) {  // nova forma de envelope: recomeça
    envAttack = (v & 4) ? 15 : 0;
    if (!(v & 8)) { envHold = true; envAlt = envAttack != 0; }
    else { envHold = (v & 1) != 0; envAlt = (v & 2) != 0; }
    envStep = 15; envHolding = false; envCnt = 0;
  }
}

int AY8910::readData(int portA) const {
  if (addr == 14) return portA & 0xFF;
  return regs[addr];
}

float AY8910::tick() {
  for (int c = 0; c < 3; c++) {
    int p = regs[c * 2] | (regs[c * 2 + 1] << 8);
    if (!p) p = 1;
    if (++toneCnt[c] >= p) { toneCnt[c] = 0; toneOut[c] ^= 1; }
  }
  const int np = regs[6] ? regs[6] : 1;
  if (++noiseCnt >= np) {
    noiseCnt = 0;
    noisePre ^= 1;
    if (noisePre) rng = (rng >> 1) | (((rng & 1) ^ ((rng >> 3) & 1)) << 16);
  }
  int ep = regs[11] | (regs[12] << 8);
  if (!ep) ep = 1;
  if (++envCnt >= ep * 2) {
    envCnt = 0;
    if (!envHolding && --envStep < 0) {
      if (envAlt) envAttack ^= 15;
      if (envHold) { envHolding = true; envStep = 0; }
      else envStep = 15;
    }
  }
  const int env = envStep ^ envAttack;
  const int mix = regs[7];
  const int noiseBit = rng & 1;
  float out = 0;
  for (int c = 0; c < 3; c++) {
    const int t = toneOut[c] | ((mix >> c) & 1);
    const int n = noiseBit | ((mix >> (c + 3)) & 1);
    if (t & n) {
      const int v = regs[8 + c];
      out += (v & 0x10) ? AY_VOL[env] : AY_VOL[v & 15];
    }
  }
  return out;
}

void AY8910::sync(StateIO& io) {
  io.raw(regs, sizeof regs);
  io.sync(addr); io.raw(toneCnt, sizeof toneCnt); io.raw(toneOut, sizeof toneOut);
  io.sync(noiseCnt); io.sync(noisePre); io.sync(rng);
  io.sync(envCnt); io.sync(envStep); io.sync(envAttack); io.sync(envHold); io.sync(envAlt); io.sync(envHolding);
  if (io.reading()) {
    addr &= 15;
    for (int c = 0; c < 3; c++) { clampTo(toneCnt[c], 0, 4096); toneOut[c] &= 1; }
    clampTo(noiseCnt, 0, 32); noisePre &= 1; rng &= 0x1FFFF;
    clampTo(envCnt, 0, 1 << 17); clampTo(envStep, 0, 15); envAttack &= 15;
  }
}

// ------------------------------------------------------------ SCC
void SCC::reset() {
  for (auto& w : wave) std::fill(w, w + 32, 0);
  for (int c = 0; c < 5; c++) { period[c] = 0; volume[c] = 0; pos[c] = 0; count[c] = 0; }
  enable = 0; deform = 0;
  used = false;
}

int SCC::read(int addr) const {
  const int a = addr & 0xFF;
  if (a < 0x80) return static_cast<uint8_t>(wave[a >> 5][a & 31]);
  if (a >= 0xA0 && a < 0xC0) return static_cast<uint8_t>(wave[3][a & 31]);  // canal 5 = onda do canal 4
  return 0xFF;
}

void SCC::write(int addr, int v) {
  int a = addr & 0xFF;
  used = true;
  if (a < 0x80) {
    const int ch = a >> 5;
    wave[ch][a & 31] = static_cast<int8_t>(v);
    if (ch == 3) wave[4][a & 31] = static_cast<int8_t>(v);  // no SCC o canal 5 usa a mesma onda do 4
    return;
  }
  if (a < 0xA0) {
    a &= 0x8F;  // 9890h–989Fh espelham 9880h–988Fh
    if (a < 0x8A) {
      const int ch = (a - 0x80) >> 1;
      if (a & 1) period[ch] = (period[ch] & 0xFF) | ((v & 0x0F) << 8);
      else period[ch] = (period[ch] & 0xF00) | v;
      if (deform & 0x20) count[ch] = 0;
    } else if (a < 0x8F) volume[a - 0x8A] = v & 0x0F;
    else enable = v & 0x1F;
    return;
  }
  if (a >= 0xE0) deform = v;
}

float SCC::tick() {
  int sum = 0;
  for (int c = 0; c < 5; c++) {
    const int p = period[c] + 1;
    if (p > 9) {  // períodos muito curtos deixam o canal parado (como no chip)
      count[c] -= 16;
      if (count[c] < 0) {
        const int steps = (-count[c] + p - 1) / p;
        count[c] += steps * p;
        pos[c] = (pos[c] + steps) & 31;
      }
    }
    if (enable & (1 << c)) sum += wave[c][pos[c]] * volume[c];
  }
  return static_cast<float>(sum) * (1.0f / (128 * 15));
}

void SCC::sync(StateIO& io) {
  io.raw(wave, sizeof wave);
  io.raw(period, sizeof period); io.raw(volume, sizeof volume); io.raw(pos, sizeof pos); io.raw(count, sizeof count);
  io.sync(enable); io.sync(deform); io.sync(used);
  if (io.reading()) {
    for (int c = 0; c < 5; c++) {
      period[c] &= 0xFFF; volume[c] &= 15; pos[c] &= 31; clampTo(count[c], 0, 0x1000);
    }
    enable &= 0x1F; deform &= 0xFF;
  }
}

// ------------------------------------------------------------ mistura
void Mixer::reset() {
  wpos = rpos = 0;
  frac = 0; acc = 0; accN = 0; cap = 0;
}

void Mixer::setSampleRate(int rate) { ticksPerSample = CPU_CLOCK / 16 / (rate > 0 ? rate : 48000); }

void Mixer::push(float v) {
  acc += v; accN++;
  frac += 1;
  if (frac < ticksPerSample) return;
  frac -= ticksPerSample;
  const double in = acc / accN;
  acc = 0; accN = 0;
  const double out = in - cap;  // passa-alta: tira o nível contínuo
  cap += out * 0.0015;
  bufL[wpos] = static_cast<float>(out);
  wpos = (wpos + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void Mixer::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { L[k] = 0; R[k] = 0; continue; }
    L[k] = R[k] = bufL[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void Mixer::sync(StateIO& io) {
  io.sync(frac); io.sync(acc); io.sync(cap); io.sync(accN);
  if (io.reading()) {
    finiteOr(frac, 0); clampTo(frac, 0.0, ticksPerSample);
    finiteOr(acc, 0); finiteOr(cap, 0); clampTo(accN, 0, 1 << 20);
    if (!accN) acc = 0;
  }
}

}  // namespace indigo::msx
