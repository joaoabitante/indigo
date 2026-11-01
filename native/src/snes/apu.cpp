// S-SMP: SPC700 + 64 KB de RAM + IPL de boot (64 bytes) + 3 timers + portas com a CPU + DSP.
// O DSP gera uma amostra estéreo a cada 32 ciclos do SPC700 (32 kHz): 8 vozes de BRR com interpolação
// gaussiana, envelopes ADSR/GAIN com a tabela de taxas do chip, ruído, modulação de pitch e eco com FIR de 8
// passos. Saída reamostrada para a taxa do aparelho.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "snes.h"

namespace indigo::snes {

namespace {

// programa de boot do SPC700 ($FFC0–$FFFF)
constexpr uint8_t IPL[64] = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0, 0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4, 0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB, 0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD, 0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF};

// taxas dos envelopes e do ruído: um evento a cada RATE amostras, com o deslocamento do chip
constexpr int COUNTER_RANGE = 2048 * 5 * 3;
constexpr int RATES[32] = {COUNTER_RANGE + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256, 192, 160, 128, 96, 80,
                           64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
constexpr int OFFSETS[32] = {1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
                             0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

// pesos da interpolação de 4 amostras (curva gaussiana; soma ~2048 em cada posição)
struct Gauss {
  int16_t t[512];
  Gauss() {
    const double sigma = 0.89;
    auto w = [&](double d) { return std::exp(-(d / sigma) * (d / sigma)); };
    const double k = 2047.0 / (w(0) + 2 * w(1) + w(2));
    for (int j = 0; j < 512; j++) t[j] = static_cast<int16_t>(std::lround(k * w(2.0 - j / 256.0)));
  }
};
const Gauss GAUSS;

inline int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

}  // namespace

// ================================================================ DSP
void DSP::reset() {
  std::memset(regs, 0, sizeof regs);
  regs[0x6C] = 0xE0;
  for (auto& v : voice) v = Voice{};
  counter = 0;
  noise = 0x4000;
  echoPos = 0;
  echoLength = 0;
  std::memset(echoHist, 0, sizeof echoHist);
  echoHistPos = 0;
  konLatch = koffLatch = 0;
  everyOther = false;
}

void DSP::write(uint8_t addr, uint8_t v) {
  addr &= 0x7F;
  regs[addr] = v;
  if (addr == 0x4C) konLatch = v;
  else if (addr == 0x7C) regs[0x7C] = 0;  // gravar em ENDX zera
}

bool DSP::rateTick(int rate) const { return ((counter + OFFSETS[rate]) % RATES[rate]) == 0; }

void DSP::decodeBrr(Voice& v, int n) {
  // 4 amostras do bloco atual (2 bytes) para o buffer circular
  const uint8_t header = ram[v.brrAddr];
  const int shift = header >> 4, filter = header & 0x0C;
  for (int b = 0; b < 2; b++) {
    const uint8_t byte = ram[static_cast<uint16_t>(v.brrAddr + v.brrOffset + b)];
    for (int h = 0; h < 2; h++) {
      int s = static_cast<int8_t>(h == 0 ? byte & 0xF0 : (byte << 4) & 0xF0) >> 4;
      s = (s << shift) >> 1;
      if (shift >= 0xD) s = (s >> 25) << 11;
      const int p1 = v.buf[(v.bufPos + 11) % 12];
      const int p2 = v.buf[(v.bufPos + 10) % 12] >> 1;
      if (filter >= 8) {
        s += p1;
        s -= p2;
        if (filter == 8) { s += p2 >> 4; s += (p1 * -3) >> 6; }
        else { s += (p1 * -13) >> 7; s += (p2 * 3) >> 4; }
      } else if (filter) {
        s += p1 >> 1;
        s += (-p1) >> 5;
      }
      s = clamp16(s);
      v.buf[v.bufPos] = static_cast<int16_t>(s * 2);
      v.bufPos = (v.bufPos + 1) % 12;
    }
  }
  v.brrOffset += 2;
  if (v.brrOffset >= 9) {
    v.brrOffset = 1;
    if (header & 1) {  // fim do sample
      regs[0x7C] |= static_cast<uint8_t>(1 << n);
      const uint16_t dir = static_cast<uint16_t>((regs[0x5D] << 8) + regs[(n << 4) | 4] * 4);
      v.brrAddr = static_cast<uint16_t>(ram[static_cast<uint16_t>(dir + 2)] | (ram[static_cast<uint16_t>(dir + 3)] << 8));
      if (!(header & 2)) { v.envMode = 3; v.env = 0; }  // sem repetição: silencia
    } else {
      v.brrAddr = static_cast<uint16_t>(v.brrAddr + 9);
    }
  }
}

void DSP::runEnvelope(Voice& v, int n) {
  const uint8_t* vr = regs + (n << 4);
  int env = v.env;
  if (v.envMode == 3) {
    env -= 8;
    if (env < 0) env = 0;
    v.env = env;
    return;
  }
  int rate;
  int envData = vr[6];
  const uint8_t adsr0 = vr[5];
  if (adsr0 & 0x80) {
    if (v.envMode >= 1) {
      env--;
      env -= env >> 8;
      rate = envData & 0x1F;
      if (v.envMode == 1) rate = ((adsr0 >> 3) & 0x0E) + 0x10;
    } else {
      rate = (adsr0 & 0x0F) * 2 + 1;
      env += rate < 31 ? 0x20 : 0x400;
    }
  } else {
    envData = vr[7];
    const int mode = envData >> 5;
    if (mode < 4) {
      env = envData * 0x10;
      rate = 31;
    } else {
      rate = envData & 0x1F;
      if (mode == 4) env -= 0x20;
      else if (mode < 6) { env--; env -= env >> 8; }
      else {
        env += 0x20;
        if (mode > 6 && static_cast<unsigned>(v.hiddenEnv) >= 0x600) env += 0x8 - 0x20;
      }
    }
  }
  if ((env >> 8) == (envData >> 5) && v.envMode == 1) v.envMode = 2;
  v.hiddenEnv = env;
  if (static_cast<unsigned>(env) > 0x7FF) {
    env = env < 0 ? 0 : 0x7FF;
    if (v.envMode == 0) v.envMode = 1;
  }
  if (rateTick(rate)) v.env = env;
}

int DSP::voiceOutput(Voice& v, int n, int pitchModIn) {
  uint8_t* vr = regs + (n << 4);
  if (v.konDelay > 0) {
    if (--v.konDelay == 0) {
      // começa: preenche o buffer com as 12 primeiras amostras
      for (int k = 0; k < 3; k++) decodeBrr(v, n);
      v.interp = 0;
    }
    vr[8] = 0;
    vr[9] = 0;
    v.out = 0;
    return 0;
  }
  int pitch = ((vr[3] & 0x3F) << 8) | vr[2];
  if (n > 0 && ((regs[0x2D] >> n) & 1)) pitch += ((pitchModIn >> 5) * pitch) >> 10;
  // interpolação
  int out;
  if ((regs[0x3D] >> n) & 1) {
    out = static_cast<int16_t>(noise * 2);
  } else {
    const int off = (v.interp >> 4) & 0xFF;
    const int base = v.bufPos + (v.interp >> 12);
    const int i0 = v.buf[base % 12], i1 = v.buf[(base + 1) % 12], i2 = v.buf[(base + 2) % 12], i3 = v.buf[(base + 3) % 12];
    out = (GAUSS.t[255 - off] * i0) >> 11;
    out += (GAUSS.t[511 - off] * i1) >> 11;
    out += (GAUSS.t[256 + off] * i2) >> 11;
    out = static_cast<int16_t>(out);
    out += (GAUSS.t[off] * i3) >> 11;
    out = clamp16(out) & ~1;
  }
  runEnvelope(v, n);
  out = ((out * v.env) >> 11) & ~1;
  // avança o pitch e decodifica mais 4 amostras quando passa de um grupo
  v.interp = (v.interp & 0x3FFF) + std::min(pitch, 0x3FFF);
  if (v.interp > 0x7FFF) v.interp = 0x7FFF;
  while (v.interp >= 0x4000) {
    v.interp -= 0x4000;
    decodeBrr(v, n);
  }
  vr[8] = static_cast<uint8_t>(v.env >> 4);
  vr[9] = static_cast<uint8_t>(out >> 8);
  v.out = out;
  return out;
}

void DSP::sample(int16_t& left, int16_t& right) {
  if (--counter < 0) counter = COUNTER_RANGE - 1;
  // KON/KOFF são lidos a cada duas amostras
  everyOther = !everyOther;
  if (everyOther) {
    const uint8_t kon = konLatch, koff = regs[0x5C];
    konLatch = 0;
    for (int n = 0; n < 8; n++) {
      Voice& v = voice[n];
      if ((koff >> n) & 1) v.envMode = 3;
      if ((kon >> n) & 1) {
        const uint16_t dir = static_cast<uint16_t>((regs[0x5D] << 8) + regs[(n << 4) | 4] * 4);
        v.brrAddr = static_cast<uint16_t>(ram[dir] | (ram[static_cast<uint16_t>(dir + 1)] << 8));
        v.brrOffset = 1;
        v.bufPos = 0;
        std::memset(v.buf, 0, sizeof v.buf);
        v.interp = 0;
        v.env = 0;
        v.hiddenEnv = 0;
        v.envMode = 0;
        v.konDelay = 5;
        regs[0x7C] &= static_cast<uint8_t>(~(1 << n));
      }
    }
  }
  const uint8_t flg = regs[0x6C];
  if (flg & 0x80) {  // reinício: todas as vozes param
    for (auto& v : voice) { v.envMode = 3; v.env = 0; }
  }
  // ruído
  if (rateTick(flg & 0x1F)) {
    const int fb = (noise << 13) ^ (noise << 14);
    noise = (fb & 0x4000) ^ (noise >> 1);
  }
  int mainL = 0, mainR = 0, echoL = 0, echoR = 0;
  int prev = 0;
  for (int n = 0; n < 8; n++) {
    const int out = voiceOutput(voice[n], n, prev);
    prev = out;
    const int l = (out * static_cast<int8_t>(regs[(n << 4) | 0])) >> 7;
    const int r = (out * static_cast<int8_t>(regs[(n << 4) | 1])) >> 7;
    mainL = clamp16(mainL + l);
    mainR = clamp16(mainR + r);
    if ((regs[0x4D] >> n) & 1) {
      echoL = clamp16(echoL + l);
      echoR = clamp16(echoR + r);
    }
  }
  // eco: lê a amostra antiga do buffer, passa pelo FIR e grava a nova (com realimentação)
  const uint16_t ea = static_cast<uint16_t>((regs[0x6D] << 8) + echoPos);
  const int16_t inL = static_cast<int16_t>(ram[ea] | (ram[static_cast<uint16_t>(ea + 1)] << 8));
  const int16_t inR = static_cast<int16_t>(ram[static_cast<uint16_t>(ea + 2)] | (ram[static_cast<uint16_t>(ea + 3)] << 8));
  echoHistPos = (echoHistPos + 1) & 7;
  echoHist[0][echoHistPos] = static_cast<int16_t>(inL >> 1);
  echoHist[1][echoHistPos] = static_cast<int16_t>(inR >> 1);
  int fir[2];
  for (int c = 0; c < 2; c++) {
    int s = 0;
    for (int i = 0; i < 7; i++) s += (echoHist[c][(echoHistPos + 1 + i) & 7] * static_cast<int8_t>(regs[(i << 4) | 0x0F])) >> 6;
    s = static_cast<int16_t>(s);
    s += (echoHist[c][echoHistPos] * static_cast<int8_t>(regs[0x7F])) >> 6;
    fir[c] = clamp16(s) & ~1;
  }
  const int outEchoL = (fir[0] * static_cast<int8_t>(regs[0x2C])) >> 7;
  const int outEchoR = (fir[1] * static_cast<int8_t>(regs[0x3C])) >> 7;
  if (!(flg & 0x20)) {
    const int wl = clamp16(echoL + ((fir[0] * static_cast<int8_t>(regs[0x0D])) >> 7)) & ~1;
    const int wr = clamp16(echoR + ((fir[1] * static_cast<int8_t>(regs[0x0D])) >> 7)) & ~1;
    ram[ea] = static_cast<uint8_t>(wl);
    ram[static_cast<uint16_t>(ea + 1)] = static_cast<uint8_t>(wl >> 8);
    ram[static_cast<uint16_t>(ea + 2)] = static_cast<uint8_t>(wr);
    ram[static_cast<uint16_t>(ea + 3)] = static_cast<uint8_t>(wr >> 8);
  }
  if (echoPos == 0) echoLength = (regs[0x7D] & 0x0F) << 11;
  echoPos += 4;
  if (echoPos >= echoLength) echoPos = 0;
  int l = clamp16(((mainL * static_cast<int8_t>(regs[0x0C])) >> 7) + outEchoL);
  int r = clamp16(((mainR * static_cast<int8_t>(regs[0x1C])) >> 7) + outEchoR);
  if (flg & 0x40) l = r = 0;
  left = static_cast<int16_t>(l);
  right = static_cast<int16_t>(r);
}

void DSP::sync(StateIO& io) {
  io.raw(regs, sizeof regs);
  for (auto& v : voice) {
    io.sync(v.brrAddr); io.sync(v.brrOffset); io.raw(v.buf, sizeof v.buf); io.sync(v.bufPos); io.sync(v.interp);
    io.sync(v.env); io.sync(v.envMode); io.sync(v.konDelay); io.sync(v.hiddenEnv); io.sync(v.out);
  }
  io.sync(counter); io.sync(noise); io.sync(echoPos); io.sync(echoLength); io.raw(echoHist, sizeof echoHist); io.sync(echoHistPos);
  io.sync(konLatch); io.sync(koffLatch); io.sync(everyOther);
  if (io.reading()) {
    for (auto& v : voice) {
      clampTo(v.brrOffset, 1, 8); clampTo(v.bufPos, 0, 11); clampTo(v.interp, 0, 0x7FFF); clampTo(v.env, 0, 0x7FF);
      clampTo(v.envMode, 0, 3); clampTo(v.konDelay, 0, 5);
    }
    clampTo(counter, 0, COUNTER_RANGE - 1); noise &= 0x7FFF; clampTo(echoPos, 0, 0x7FFC); echoPos &= ~3;
    clampTo(echoLength, 0, 0x7800); echoHistPos &= 7;
  }
}

// ================================================================ S-SMP
APU::APU() : dsp(ram) { smp.bus = this; }

void APU::reset() {
  std::memset(ram, 0, sizeof ram);
  std::memset(in, 0, sizeof in);
  std::memset(out, 0, sizeof out);
  control = 0xB0;
  dspAddr = 0;
  test = 0x0A;
  for (auto& t : timer) t = Timer{};
  div128 = div16 = dspDiv = 0;
  debt = 0;
  dsp.reset();
  smp = SPC700{};
  smp.bus = this;
  smp.pc = static_cast<uint16_t>(IPL[0x3E] | (IPL[0x3F] << 8));
  wpos = rpos = 0;
  phase = 0;
  lastL = lastR = 0;
}

void APU::runCycles(int64_t n) {
  debt += n;
  while (debt > 0) smp.step();
}

void APU::cycle() {
  debt--;
  if (++div16 >= 16) {
    div16 = 0;
    auto tickTimer = [](Timer& t) {
      if (!t.on) return;
      const int st = t.stage + 1;
      if (st >= (t.target ? t.target : 256)) { t.stage = 0; t.outv = (t.outv + 1) & 15; }
      else t.stage = static_cast<uint8_t>(st);
    };
    tickTimer(timer[2]);
    if (++div128 >= 8) {
      div128 = 0;
      tickTimer(timer[0]);
      tickTimer(timer[1]);
    }
  }
  if (++dspDiv >= 32) {
    dspDiv = 0;
    int16_t l, r;
    dsp.sample(l, r);
    emit(l, r);
  }
}

uint8_t APU::read(uint16_t a) {
  cycle();
  if (a >= 0xF0 && a <= 0xFF) {
    switch (a) {
      case 0xF2: return dspAddr;
      case 0xF3: return dsp.read(dspAddr & 0x7F);
      case 0xF4: case 0xF5: case 0xF6: case 0xF7: return in[a - 0xF4];
      case 0xF8: case 0xF9: return ram[a];
      case 0xFD: case 0xFE: case 0xFF: {
        Timer& t = timer[a - 0xFD];
        const uint8_t v = t.outv;
        t.outv = 0;
        return v;
      }
      default: return 0;
    }
  }
  if (a >= 0xFFC0 && (control & 0x80)) return IPL[a - 0xFFC0];
  return ram[a];
}

void APU::write(uint16_t a, uint8_t v) {
  cycle();
  if (a >= 0xF0 && a <= 0xFF) {
    switch (a) {
      case 0xF0: test = v; break;
      case 0xF1:
        for (int i = 0; i < 3; i++) {
          const bool on = (v >> i) & 1;
          if (on && !timer[i].on) { timer[i].stage = 0; timer[i].outv = 0; }
          timer[i].on = on;
        }
        if (v & 0x10) { in[0] = in[1] = 0; }
        if (v & 0x20) { in[2] = in[3] = 0; }
        control = v;
        break;
      case 0xF2: dspAddr = v; break;
      case 0xF3: if (!(dspAddr & 0x80)) dsp.write(dspAddr, v); break;
      case 0xF4: case 0xF5: case 0xF6: case 0xF7: out[a - 0xF4] = v; break;
      case 0xFA: case 0xFB: case 0xFC: timer[a - 0xFA].target = v; break;
      default: break;
    }
  }
  ram[a] = v;
}

void APU::setSampleRate(int rate) { step = 32000.0 / (rate > 0 ? rate : 48000); }

void APU::emit(int16_t l, int16_t r) {
  const float cl = l / 32768.0f, cr = r / 32768.0f;
  while (phase < 1.0) {
    const float f = static_cast<float>(phase);
    bufL[wpos] = lastL + (cl - lastL) * f;
    bufR[wpos] = lastR + (cr - lastR) * f;
    wpos = (wpos + 1) % bufSize;
    if (wpos == rpos) rpos = (rpos + 1) % bufSize;
    phase += step;
  }
  phase -= 1.0;
  lastL = cl;
  lastR = cr;
}

void APU::pull(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { l[k] = r[k] = 0; continue; }
    l[k] = bufL[rpos];
    r[k] = bufR[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void APU::sync(StateIO& io) {
  io.raw(ram, sizeof ram);
  smp.sync(io);
  dsp.sync(io);
  io.sync(debt);
  io.raw(in, sizeof in); io.raw(out, sizeof out);
  io.sync(control); io.sync(dspAddr); io.sync(test);
  for (auto& t : timer) { io.sync(t.target); io.sync(t.stage); io.sync(t.outv); io.sync(t.on); }
  io.sync(div128); io.sync(div16); io.sync(dspDiv);
  io.sync(phase); io.sync(lastL); io.sync(lastR);
  if (io.reading()) {
    clampTo<int64_t>(debt, -64, 64);
    for (auto& t : timer) t.outv &= 15;
    clampTo(div128, 0, 7); clampTo(div16, 0, 15); clampTo(dspDiv, 0, 31);
    finiteOr(phase, 0); clampTo(phase, 0.0, 1.0);
    if (!(lastL > -2 && lastL < 2)) lastL = 0;
    if (!(lastR > -2 && lastR < 2)) lastR = 0;
  }
}

}  // namespace indigo::snes
