// APU do GBA (porte de src/apu.js): 4 canais PSG (estilo Game Boy) + 2 canais DirectSound (FIFO A/B).
// Os temporizadores dos canais são números reais (double), como na versão web: cada amostra avança
// 16777216 / taxa ciclos, que não é inteiro.
#include <cstddef>
#include <cstring>

#include "gba.h"

namespace indigo::gba {

namespace {
const int DUTY[4][8] = {
  {0, 0, 0, 0, 0, 0, 0, 1},
  {1, 0, 0, 0, 0, 0, 0, 1},
  {1, 0, 0, 0, 0, 1, 1, 1},
  {0, 1, 1, 1, 1, 1, 1, 0},
};
}  // namespace

APU::APU(GBA& g) : gba(g), bufL(bufSize), bufR(bufSize) {}

void APU::reset() {
  for (auto& c : ch) c = PsgChannel{};
  fifoA.reset(); fifoB.reset();
  fifoA.out = 0; fifoB.out = 0;
  sampleCycle = 0; seqCycle = 0; seqStep = 0;
  wpos = rpos = 0;
}

int APU::statusBits() const {
  int s = 0;
  for (int k = 0; k < 4; k++) if (ch[k].on) s |= 1 << k;
  return s;
}

int APU::readReg(int a, int raw) const {
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
        const int bank = (gba.io.regs[0x70 >> 1] & 0x40) ? 0 : 16;
        const int o = bank + (a - 0x90);
        return wave[o] | (wave[o + 1] << 8);
      }
      return 0;
  }
}

void APU::writeReg(int a, int v) {
  catchUp();
  uint16_t* regs = gba.io.regs;
  const bool master = (regs[0x84 >> 1] & 0x80) != 0;
  switch (a) {
    case 0x60:
      ch[0].sweepShift = v & 7; ch[0].sweepDir = (v >> 3) & 1; ch[0].sweepTime = (v >> 4) & 7;
      break;
    case 0x62: case 0x68: {
      PsgChannel& c = a == 0x62 ? ch[0] : ch[1];
      c.length = 64 - (v & 0x3F); c.duty = (v >> 6) & 3;
      c.envStep = (v >> 8) & 7; c.envDir = (v >> 11) & 1;
      if (((v >> 12) & 0xF) == 0 && !c.envDir) c.on = false;
      break;
    }
    case 0x64: case 0x6C: {
      PsgChannel& c = a == 0x64 ? ch[0] : ch[1];
      c.freq = v & 0x7FF; c.lenEnable = (v & 0x4000) != 0;
      if ((v & 0x8000) && master) trigger(a == 0x64 ? 0 : 1);
      regs[a >> 1] = static_cast<uint16_t>(v & 0x7FFF);
      break;
    }
    case 0x70:
      if (!(v & 0x80)) ch[2].on = false;
      break;
    case 0x72: ch[2].length = 256 - (v & 0xFF); break;
    case 0x74:
      ch[2].freq = v & 0x7FF; ch[2].lenEnable = (v & 0x4000) != 0;
      if ((v & 0x8000) && master) trigger(2);
      regs[a >> 1] = static_cast<uint16_t>(v & 0x7FFF);
      break;
    case 0x78:
      ch[3].length = 64 - (v & 0x3F);
      ch[3].envStep = (v >> 8) & 7; ch[3].envDir = (v >> 11) & 1;
      if (((v >> 12) & 0xF) == 0 && !ch[3].envDir) ch[3].on = false;
      break;
    case 0x7C:
      ch[3].lenEnable = (v & 0x4000) != 0; ch[3].width7 = (v & 8) != 0;
      if ((v & 0x8000) && master) trigger(3);
      regs[a >> 1] = static_cast<uint16_t>(v & 0x7FFF);
      break;
    case 0x82:
      if (v & 0x0800) fifoA.reset();
      if (v & 0x8000) fifoB.reset();
      regs[a >> 1] = static_cast<uint16_t>(v & 0x770F);
      break;
    case 0x84:
      if (!(v & 0x80)) for (auto& c : ch) c.on = false;
      break;
    default:
      if (a >= 0x90 && a < 0xA0) {
        const int bank = (regs[0x70 >> 1] & 0x40) ? 0 : 16;
        const int o = bank + (a - 0x90);
        wave[o] = static_cast<uint8_t>(v & 0xFF); wave[o + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
      }
  }
}

void APU::trigger(int nch) {
  const uint16_t* regs = gba.io.regs;
  PsgChannel& c = ch[nch];
  c.on = true;
  if (nch < 2) {
    const int env = regs[(nch == 0 ? 0x62 : 0x68) >> 1];
    c.vol = (env >> 12) & 0xF;
    c.envTimer = c.envStep;
    if (c.length == 0) c.length = 64;
    c.timer = (2048 - c.freq) * 16;
    if (nch == 0) {
      c.shadow = c.freq; c.sweepTimer = c.sweepTime;
      if (c.sweepShift) sweepCalc(true);
    }
    if (c.vol == 0 && !c.envDir) c.on = false;
  } else if (nch == 2) {
    if (!(regs[0x70 >> 1] & 0x80)) c.on = false;
    if (c.length == 0) c.length = 256;
    c.timer = (2048 - c.freq) * 8;
    c.pos = 0;
  } else {
    const int env = regs[0x78 >> 1];
    c.vol = (env >> 12) & 0xF;
    c.envTimer = c.envStep;
    if (c.length == 0) c.length = 64;
    c.lfsr = c.width7 ? 0x7F : 0x7FFF;
    c.timer = noisePeriod();
    if (c.vol == 0 && !c.envDir) c.on = false;
  }
}

int APU::noisePeriod() const {
  const int v = gba.io.regs[0x7C >> 1];
  const int r = v & 7, s = (v >> 4) & 0xF;
  return (r == 0 ? 16 : 32 * r) << (s + 1);
}

void APU::sweepCalc(bool write) {
  PsgChannel& c = ch[0];
  const int delta = c.shadow >> c.sweepShift;
  const int nf = c.sweepDir ? c.shadow - delta : c.shadow + delta;
  if (nf > 2047) { c.on = false; return; }
  if (write && c.sweepShift) { c.shadow = nf; c.freq = nf; }
}

// sequenciador de quadros: 512 Hz
void APU::sequencer() {
  const int step = seqStep;
  seqStep = (step + 1) & 7;
  if ((step & 1) == 0) {
    for (auto& c : ch) {
      if (c.lenEnable && c.length > 0) { c.length--; if (c.length == 0) c.on = false; }
    }
  }
  if (step == 2 || step == 6) {
    PsgChannel& c = ch[0];
    if (c.sweepTime) {
      if (--c.sweepTimer <= 0) {
        c.sweepTimer = c.sweepTime;
        if (c.on) sweepCalc(true);
      }
    }
  }
  if (step == 7) {
    for (int k : {0, 1, 3}) {
      PsgChannel& c = ch[k];
      if (!c.envStep) continue;
      if (--c.envTimer <= 0) {
        c.envTimer = c.envStep;
        if (c.envDir && c.vol < 15) c.vol++;
        else if (!c.envDir && c.vol > 0) c.vol--;
      }
    }
  }
}

// ---------------- DirectSound ----------------
void APU::fifoWrite32(int a, int32_t v) {
  Fifo& f = a == 0xA0 ? fifoA : fifoB;
  const uint32_t u = U32(v);
  f.push(u & 0xFF); f.push((u >> 8) & 0xFF); f.push((u >> 16) & 0xFF); f.push((u >> 24) & 0xFF);
}
void APU::fifoWrite16(int a, int v) {
  Fifo& f = a < 0xA4 ? fifoA : fifoB;
  f.push(v & 0xFF); f.push((v >> 8) & 0xFF);
}
void APU::fifoWrite8(int a, int v) {
  Fifo& f = a < 0xA4 ? fifoA : fifoB;
  f.push(v & 0xFF);
}

void APU::timerOverflow(int t) {
  const uint16_t* regs = gba.io.regs;
  const int cnt = regs[0x82 >> 1];
  if (!(regs[0x84 >> 1] & 0x80)) return;
  const int aT = (cnt >> 10) & 1, bT = (cnt >> 14) & 1;
  if (aT == t || bT == t) catchUp();
  if (aT == t) {
    fifoA.pop();
    if (fifoA.count <= 16) gba.io.soundDma(0x040000A0);
  }
  if (bT == t) {
    fifoB.pop();
    if (fifoB.count <= 16) gba.io.soundDma(0x040000A4);
  }
}

// ---------------- geração de amostras ----------------
void APU::catchUp() {
  const double now = static_cast<double>(gba.now());
  const double cps = cyclesPerSample;
  while (sampleCycle + cps <= now) {
    sampleCycle += cps;
    genSample(cps);
  }
}

void APU::advanceChannels(double cyc) {
  // canais de onda quadrada
  for (int k = 0; k < 2; k++) {
    PsgChannel& c = ch[k];
    if (!c.on) continue;
    c.timer -= cyc;
    const int period = (2048 - c.freq) * 16;
    while (c.timer <= 0) { c.timer += period; c.pos = (c.pos + 1) & 7; }
  }
  // onda programável
  PsgChannel& w = ch[2];
  if (w.on) {
    w.timer -= cyc;
    const int period = (2048 - w.freq) * 8;
    const bool dim = (gba.io.regs[0x70 >> 1] & 0x20) != 0;
    while (w.timer <= 0) { w.timer += period; w.pos = (w.pos + 1) & (dim ? 63 : 31); }
  }
  // ruído
  PsgChannel& nz = ch[3];
  if (nz.on) {
    nz.timer -= cyc;
    const int period = noisePeriod();
    while (nz.timer <= 0) {
      nz.timer += period;
      const int bit = (nz.lfsr ^ (nz.lfsr >> 1)) & 1;
      if (nz.width7) nz.lfsr = ((nz.lfsr >> 1) & 0x3F) | (bit << 6);
      else nz.lfsr = ((nz.lfsr >> 1) & 0x3FFF) | (bit << 14);
    }
  }
}

void APU::genSample(double cyc) {
  seqCycle += cyc;
  while (seqCycle >= 32768) { seqCycle -= 32768; sequencer(); }
  advanceChannels(cyc);
  const uint16_t* r = gba.io.regs;
  int L = 0, R = 0;
  if (r[0x84 >> 1] & 0x80) {
    const int cntL = r[0x80 >> 1], cntH = r[0x82 >> 1];
    int outs[4] = {0, 0, 0, 0};
    for (int k = 0; k < 2; k++) {
      const PsgChannel& c = ch[k];
      if (c.on) outs[k] = DUTY[c.duty][c.pos] ? c.vol : -c.vol;
    }
    const PsgChannel& w = ch[2];
    if (w.on) {
      const int w72 = r[0x72 >> 1];
      const bool dim = (r[0x70 >> 1] & 0x20) != 0;
      const int bankStart = dim ? 0 : ((r[0x70 >> 1] & 0x40) ? 16 : 0);
      const int idx = (bankStart * 2 + w.pos) & 63;
      const int byte = wave[idx >> 1];
      const int s = (idx & 1) ? (byte & 0xF) : (byte >> 4);
      int vv = s * 2 - 15;
      if (w72 & 0x8000) vv = (vv * 3) >> 2;
      else switch ((w72 >> 13) & 3) {
        case 0: vv = 0; break;
        case 1: break;
        case 2: vv >>= 1; break;
        case 3: vv >>= 2; break;
      }
      outs[2] = vv;
    }
    const PsgChannel& nz = ch[3];
    if (nz.on) outs[3] = (nz.lfsr & 1) ? -nz.vol : nz.vol;
    int pl = 0, pr = 0;
    for (int k = 0; k < 4; k++) {
      if (cntL & (0x1000 << k)) pl += outs[k];
      if (cntL & (0x100 << k)) pr += outs[k];
    }
    pl *= ((cntL >> 4) & 7) + 1;
    pr *= (cntL & 7) + 1;
    static const int PSG_SHIFT[4] = {2, 1, 0, 2};
    const int psgShift = PSG_SHIFT[cntH & 3];
    pl >>= psgShift; pr >>= psgShift;
    const int a = fifoA.out * ((cntH & 4) ? 4 : 2);
    const int b = fifoB.out * ((cntH & 8) ? 4 : 2);
    L = pl + ((cntH & 0x200) ? a : 0) + ((cntH & 0x2000) ? b : 0);
    R = pr + ((cntH & 0x100) ? a : 0) + ((cntH & 0x1000) ? b : 0);
  }
  const size_t wp = wpos;
  bufL[wp] = static_cast<float>(L / 1024.0);
  bufR[wp] = static_cast<float>(R / 1024.0);
  wpos = (wp + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void APU::pull(float* outL, float* outR, size_t cnt) {
  for (size_t k = 0; k < cnt; k++) {
    if (rpos == wpos) {
      const size_t last = (rpos + bufSize - 1) % bufSize;
      outL[k] = static_cast<float>(bufL[last] * 0.98); outR[k] = static_cast<float>(bufR[last] * 0.98);
      continue;
    }
    outL[k] = bufL[rpos]; outR[k] = bufR[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void APU::sync(StateIO& io) {
  for (int k = 0; k < 4; k++) {
    PsgChannel& c = ch[k];
    io.sync(c.on); io.sync(c.lenEnable); io.sync(c.width7);
    for (int* p : {&c.vol, &c.envDir, &c.envStep, &c.envTimer, &c.length, &c.freq, &c.pos, &c.duty, &c.sweepTime,
                   &c.sweepDir, &c.sweepShift, &c.sweepTimer, &c.shadow, &c.lfsr})
      io.sync(*p);
    io.sync(c.timer);
    if (io.reading()) {
      c.vol &= 15; c.envDir &= 1; c.envStep &= 7; clampTo(c.envTimer, -1, 7); clampTo(c.length, 0, 256);
      c.freq &= 0x7FF; c.pos &= 63; c.duty &= 3; c.sweepTime &= 7; c.sweepDir &= 1; c.sweepShift &= 7;
      clampTo(c.sweepTimer, -1, 7); c.shadow &= 0x7FF; c.lfsr &= 0x7FFF;
      finiteOr(c.timer, 1); clampTo(c.timer, -4.0e6, 4.0e6);
    }
  }
  io.raw(wave, sizeof wave);
  for (Fifo* f : {&fifoA, &fifoB}) {
    io.raw(f->buf, sizeof f->buf);
    io.sync(f->r); io.sync(f->w); io.sync(f->count); io.sync(f->out);
    if (io.reading()) { f->r &= 31; f->w &= 31; clampTo(f->count, 0, 32); clampTo(f->out, -128, 127); }
  }
  io.sync(sampleCycle); io.sync(seqCycle); io.sync(seqStep);
  if (io.reading()) {
    finiteOr(sampleCycle, 0); finiteOr(seqCycle, 0); clampTo(seqCycle, 0.0, 32768.0); seqStep &= 7;
    // o relógio do som não pode ficar muito atrás do console (senão catchUp geraria horas de amostras)
    const double now = static_cast<double>(gba.now());
    if (sampleCycle < now - 4.0e6 || sampleCycle > now) sampleCycle = now;
  }
}

}  // namespace indigo::gba
