#include "apu.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "gb.h"

namespace indigo::gb {

namespace {
constexpr double CLOCK = 4194304;
const int DUTY[4][8] = {
  {0, 0, 0, 0, 0, 0, 0, 1},
  {1, 0, 0, 0, 0, 0, 0, 1},
  {1, 0, 0, 0, 0, 1, 1, 1},
  {0, 1, 1, 1, 1, 1, 1, 0},
};
// bits que sempre leem 1 em cada registrador FF10–FF2F
const uint8_t READ_OR[32] = {
  0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
  0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};
// Math.round do JavaScript (só usado com valores positivos)
int64_t jsRound(double x) { return static_cast<int64_t>(std::floor(x + 0.5)); }
}  // namespace

APU::APU(GB& g) : gb(g), bufL(bufSize), bufR(bufSize) {
  setSampleRate(48000);
  reset();
}

void APU::setSampleRate(int rate) {
  sampleRate = rate;
  dotsPerSample = CLOCK / rate;
  charge = std::pow(0.999958, dotsPerSample);
  if (timeInit) {
    sampleBase = time; sampleIdx = 0; lastSampleAt = 0;
    nextSampleAt = time + jsRound(dotsPerSample);
    accL = accR = 0;
  }
}

void APU::resetChannels() {
  for (auto& ch : c) ch = Channel{};
  c[2].maxLen = 256;
}

void APU::reset() {
  resetChannels();
  power = true;
  std::fill(regs, regs + 0x20, 0);
  seqStep = 0;
  time = gb.dotClock;
  timeInit = true;
  sampleIdx = 0; sampleBase = time;
  nextSampleAt = time + jsRound(dotsPerSample);
  lastSampleAt = 0;
  accL = 0; accR = 0; capL = 0; capR = 0;
  curL = 0; curR = 0;
  wpos = rpos = 0;
  // valores deixados pelo boot ROM
  const uint8_t init[] = {0x80, 0xBF, 0xF3, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF, 0xBF, 0xFF,
                          0xFF, 0x00, 0x00, 0xBF, 0x77, 0xF3, 0xF1};
  std::copy(std::begin(init), std::end(init), regs);
  c[0].on = true; c[0].dac = true; c[0].duty = 2; c[0].envInit = 0xF; c[0].envPer = 3;
  c[0].vol = 0; c[0].freq = 0x7FF;
  mix();
}

int APU::read(int a) {
  const int i = a - 0xFF10;
  if (a >= 0xFF30) {
    catchUp();
    const Channel& w = c[2];
    if (!w.on) return wave[a - 0xFF30];
    // no DMG só dá para ler a onda no exato ciclo em que o canal busca um byte; no GBC, sempre o byte atual
    if (!gb.cgb && !waveJustRead()) return 0xFF;
    return wave[w.pos >> 1];
  }
  if (i == 0x16) {
    catchUp();
    int s = (power ? 0x80 : 0) | 0x70;
    for (int k = 0; k < 4; k++) if (c[k].on) s |= 1 << k;
    return s;
  }
  return regs[i] | READ_OR[i];
}

void APU::write(int a, int v) {
  catchUp();
  if (a >= 0xFF30) {
    Channel& w = c[2];
    if (!w.on) wave[a - 0xFF30] = static_cast<uint8_t>(v);
    else if (gb.cgb || waveJustRead()) wave[w.pos >> 1] = static_cast<uint8_t>(v);
    return;
  }
  const int i = a - 0xFF10;
  if (i == 0x16) {
    const bool on = (v & 0x80) != 0;
    if (power && !on) {
      for (int k = 0; k < 0x16; k++) regs[k] = 0;
      int lens[4];
      for (int k = 0; k < 4; k++) lens[k] = c[k].len;
      resetChannels();
      // no DMG os contadores de duração não são zerados ao desligar o som
      if (!gb.cgb) for (int k = 0; k < 4; k++) c[k].len = lens[k];
    } else if (!power && on) {
      seqStep = 0;
    }
    power = on;
    mix();
    return;
  }
  if (!power) {
    // DMG: com o som desligado, ainda aceita escrever a duração (NRx1)
    if (!gb.cgb) {
      if (i == 0x01 || i == 0x06 || i == 0x10) c[i == 1 ? 0 : i == 6 ? 1 : 3].len = 64 - (v & 63);
      else if (i == 0x0B) c[2].len = 256 - v;
    }
    return;
  }
  if (i >= 0x17) { regs[i] = static_cast<uint8_t>(v); return; }
  regs[i] = static_cast<uint8_t>(v);
  switch (i) {
    case 0x00: {
      Channel& ch = c[0];
      ch.sweepPer = (v >> 4) & 7; ch.sweepShift = v & 7;
      const bool neg = (v & 8) != 0;
      if (ch.sweepNeg && !neg && ch.negUsed) ch.on = false;
      ch.sweepNeg = neg;
      break;
    }
    case 0x01: case 0x06: { Channel& ch = c[i == 1 ? 0 : 1]; ch.duty = v >> 6; ch.len = 64 - (v & 63); break; }
    case 0x02: case 0x07: case 0x11: writeEnv(c[i == 2 ? 0 : i == 7 ? 1 : 3], v); break;
    case 0x03: case 0x08: case 0x0D: { Channel& ch = c[i == 3 ? 0 : i == 8 ? 1 : 2]; ch.freq = (ch.freq & 0x700) | v; break; }
    case 0x04: case 0x09: case 0x0E: case 0x13: {
      const int k = i == 4 ? 0 : i == 9 ? 1 : i == 0x0E ? 2 : 3;
      Channel& ch = c[k];
      if (k < 3) ch.freq = (ch.freq & 0xFF) | ((v & 7) << 8);
      writeControl(k, ch, v);
      break;
    }
    case 0x0A: { Channel& ch = c[2]; ch.dac = (v & 0x80) != 0; if (!ch.dac) ch.on = false; break; }
    case 0x0B: c[2].len = 256 - v; break;
    case 0x0C: c[2].volCode = (v >> 5) & 3; break;
    case 0x10: c[3].len = 64 - (v & 63); break;
    case 0x12: { Channel& ch = c[3]; ch.shift = v >> 4; ch.width7 = (v & 8) != 0; ch.div = v & 7; break; }
    default: break;
  }
  mix();
}

void APU::writeEnv(Channel& ch, int v) {
  ch.envInit = v >> 4; ch.envUp = (v & 8) != 0; ch.envPer = v & 7;
  ch.dac = (v & 0xF8) != 0;
  if (!ch.dac) ch.on = false;
}

void APU::writeControl(int k, Channel& ch, int v) {
  // o contador de duração é decrementado a mais quando ligado na metade do período que não o conta
  const bool oddNext = (seqStep & 1) == 1;
  const bool wasEn = ch.lenEn;
  ch.lenEn = (v & 0x40) != 0;
  if (!wasEn && ch.lenEn && oddNext && ch.len > 0) {
    ch.len--;
    if (ch.len == 0 && !(v & 0x80)) ch.on = false;
  }
  if (!(v & 0x80)) return;
  // disparo
  ch.on = ch.dac;
  if (ch.len == 0) { ch.len = ch.maxLen; if (ch.lenEn && oddNext) ch.len--; }
  if (k == 2) {
    // DMG: redisparar a onda bem quando ela busca um byte corrompe o começo da memória de onda
    if (!gb.cgb && ch.on && ch.timer <= 2) {
      const int b = ((ch.pos + 1) & 31) >> 1;
      if (b < 4) wave[0] = wave[b];
      else { const int base = b & ~3; for (int j = 0; j < 4; j++) wave[j] = wave[base + j]; }
    }
    ch.timer = period(2) + 6; ch.pos = 0;
  } else {
    ch.timer = period(k);
    ch.vol = ch.envInit; ch.envTimer = ch.envPer ? ch.envPer : 8;
    if (k == 3) ch.lfsr = 0x7FFF;
  }
  if (k == 0) {
    ch.shadow = ch.freq; ch.negUsed = false;
    ch.sweepTimer = ch.sweepPer ? ch.sweepPer : 8;
    ch.sweepEn = ch.sweepPer > 0 || ch.sweepShift > 0;
    if (ch.sweepShift) sweepCalc(ch);
  }
}

bool APU::waveJustRead() const { return period(2) - c[2].timer < 2; }

int APU::period(int k) const {
  const Channel& ch = c[k];
  if (k == 2) return (2048 - ch.freq) * 2;
  if (k == 3) return (ch.div ? ch.div * 16 : 8) << ch.shift;
  return (2048 - ch.freq) * 4;
}

int APU::sweepCalc(Channel& ch) {
  int f = ch.shadow >> ch.sweepShift;
  if (ch.sweepNeg) { f = ch.shadow - f; ch.negUsed = true; } else f = ch.shadow + f;
  if (f > 2047) ch.on = false;
  return f;
}

// sequenciador de quadros (512 Hz, via DIV)
void APU::frameStep() {
  catchUp();
  if (!power) return;
  const int s = seqStep;
  seqStep = (s + 1) & 7;
  if (!(s & 1)) {
    for (int k = 0; k < 4; k++) {
      Channel& ch = c[k];
      if (ch.lenEn && ch.len > 0 && --ch.len == 0) ch.on = false;
    }
  }
  if (s == 2 || s == 6) {
    Channel& ch = c[0];
    if (--ch.sweepTimer <= 0) {
      ch.sweepTimer = ch.sweepPer ? ch.sweepPer : 8;
      if (ch.sweepEn && ch.sweepPer) {
        const int f = sweepCalc(ch);
        if (f <= 2047 && ch.sweepShift) { ch.freq = ch.shadow = f; sweepCalc(ch); }
      }
    }
  }
  if (s == 7) {
    for (int k : {0, 1, 3}) {
      Channel& ch = c[k];
      if (!ch.envPer) continue;
      if (--ch.envTimer <= 0) {
        ch.envTimer = ch.envPer;
        if (ch.envUp && ch.vol < 15) ch.vol++;
        else if (!ch.envUp && ch.vol > 0) ch.vol--;
      }
    }
  }
  mix();
}

int APU::digital(int k) const {
  const Channel& ch = c[k];
  if (!ch.on) return 0;
  if (k < 2) return DUTY[ch.duty][ch.pos] ? ch.vol : 0;
  if (k == 2) return ch.volCode ? ch.sample >> (ch.volCode - 1) : 0;
  return (ch.lfsr & 1) ? 0 : ch.vol;
}

void APU::mix() {
  if (!power) { curL = curR = 0; return; }
  const int nr50 = regs[0x14], nr51 = regs[0x15];
  double L = 0, R = 0;
  for (int k = 0; k < 4; k++) {
    if (!c[k].dac) continue;
    const double a = digital(k) / 7.5 - 1;
    if (nr51 & (0x10 << k)) L += a;
    if (nr51 & (1 << k)) R += a;
  }
  curL = L * (((nr50 >> 4) & 7) + 1) / 32;
  curR = R * ((nr50 & 7) + 1) / 32;
}

void APU::catchUp() {
  const int64_t end = gb.dotClock;
  while (time < end) {
    int64_t dt = std::min(end, nextSampleAt) - time;
    for (int k = 0; k < 4; k++) if (c[k].on && c[k].timer < dt) dt = c[k].timer;
    if (dt < 1) dt = 1;
    accL += curL * dt; accR += curR * dt;
    time += dt;
    bool changed = false;
    for (int k = 0; k < 4; k++) {
      Channel& ch = c[k];
      ch.timer -= static_cast<int>(dt);
      if (ch.timer > 0) continue;
      const int p = period(k);
      if (!ch.on) { ch.timer = p; continue; }
      while (ch.timer <= 0) {
        ch.timer += p;
        if (k < 2) ch.pos = (ch.pos + 1) & 7;
        else if (k == 2) {
          ch.pos = (ch.pos + 1) & 31;
          const int b = wave[ch.pos >> 1];
          ch.sample = (ch.pos & 1) ? b & 0xF : b >> 4;
        } else {
          const int x = (ch.lfsr ^ (ch.lfsr >> 1)) & 1;
          ch.lfsr = (ch.lfsr >> 1) | (x << 14);
          if (ch.width7) ch.lfsr = (ch.lfsr & ~0x40) | (x << 6);
        }
      }
      changed = true;
    }
    if (changed) mix();
    if (time >= nextSampleAt) emit();
  }
}

void APU::emit() {
  const double span = nextSampleAt - (lastSampleAt ? static_cast<double>(lastSampleAt) : (nextSampleAt - dotsPerSample));
  lastSampleAt = nextSampleAt;
  sampleIdx++;
  nextSampleAt = sampleBase + jsRound((sampleIdx + 1) * dotsPerSample);
  const double inL = accL / span, inR = accR / span;
  accL = 0; accR = 0;
  // passa-alta (capacitor): tira o nível contínuo dos DACs
  const double outL = inL - capL, outR = inR - capR;
  capL = inL - outL * charge; capR = inR - outR * charge;
  const size_t wp = wpos;
  bufL[wp] = static_cast<float>(outL); bufR[wp] = static_cast<float>(outR);
  wpos = (wp + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void APU::pull(float* outL, float* outR, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) {
      const size_t last = (rpos + bufSize - 1) % bufSize;
      outL[k] = bufL[last] * 0.98f; outR[k] = bufR[last] * 0.98f;
      bufL[last] *= 0.98f; bufR[last] *= 0.98f;
      continue;
    }
    outL[k] = bufL[rpos]; outR[k] = bufR[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void APU::sync(StateIO& io) {
  if (!io.reading()) catchUp();
  io.raw(regs, sizeof regs);
  io.raw(wave, sizeof wave);
  for (int k = 0; k < 4; k++) {
    // a estrutura vai byte a byte (mesmo formato de antes), mas os bools são normalizados antes de virar bool
    Channel& ch = c[k];
    uint8_t buf[sizeof(Channel)];
    std::memcpy(buf, &ch, sizeof buf);
    io.raw(buf, sizeof buf);
    if (io.reading()) {
      for (size_t off : {offsetof(Channel, on), offsetof(Channel, dac), offsetof(Channel, lenEn), offsetof(Channel, envUp),
                         offsetof(Channel, sweepNeg), offsetof(Channel, sweepEn), offsetof(Channel, negUsed), offsetof(Channel, width7)})
        buf[off] = buf[off] ? 1 : 0;
      std::memcpy(&ch, buf, sizeof buf);
      ch.maxLen = k == 2 ? 256 : 64;
      ch.pos &= k == 2 ? 31 : 7;
      ch.duty &= 3; ch.vol &= 15; ch.envInit &= 15; ch.envPer &= 7; clampTo(ch.envTimer, 0, 8);
      ch.freq &= 0x7FF; clampTo(ch.timer, 1, 1 << 23); clampTo(ch.len, 0, 256);  // timer do ruído: até 112 << 15 ciclos
      ch.sweepPer &= 7; ch.sweepShift &= 7; clampTo(ch.sweepTimer, 0, 8); ch.shadow &= 0x7FF;
      ch.volCode &= 3; ch.sample &= 15; ch.shift &= 15; ch.div &= 7; ch.lfsr &= 0x7FFF;
    }
  }
  io.sync(power); io.sync(seqStep);
  if (io.reading()) {
    seqStep &= 7;
    time = gb.dotClock; sampleBase = time; sampleIdx = 0;
    lastSampleAt = 0; nextSampleAt = time + jsRound(dotsPerSample);
    accL = accR = 0;
    mix();
  }
}

}  // namespace indigo::gb
