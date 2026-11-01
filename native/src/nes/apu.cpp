// APU do 2A03 (NTSC): 2 pulsos com envelope e sweep, triângulo, ruído, DMC (amostras de 1 bit lidas por DMA) e
// o contador de quadros de 4/5 passos com IRQ. Mistura não linear (tabelas do hardware), média por amostra de
// saída e filtros passa-alta (90 Hz) e passa-baixa (14 kHz) como os do console.
#include <cmath>

#include "nes.h"

namespace indigo::nes {

namespace {
constexpr uint8_t LENGTH[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr uint8_t DUTY[4][8] = {{0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
constexpr uint8_t TRI[32] = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr uint16_t NOISE[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
constexpr uint16_t DMC_RATE[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
// passos do contador de quadros (ciclos de CPU depois do reinício)
constexpr int STEP[2][6] = {{7457, 14913, 22371, 29828, 29829, 29830}, {7457, 14913, 22371, 29829, 37281, 37282}};
enum { NONE, QUARTER, HALF };
constexpr int STEP_TYPE[2][6] = {{QUARTER, HALF, QUARTER, NONE, HALF, NONE}, {QUARTER, HALF, QUARTER, NONE, HALF, NONE}};

struct Mix {
  float pulse[31], tnd[203];
  Mix() {
    pulse[0] = 0;
    for (int i = 1; i < 31; i++) pulse[i] = static_cast<float>(95.52 / (8128.0 / i + 100));
    tnd[0] = 0;
    for (int i = 1; i < 203; i++) tnd[i] = static_cast<float>(163.67 / (24329.0 / i + 100));
  }
};
const Mix MIX;
constexpr double CPU_HZ = 236250000.0 / 11.0 / 12.0;
}  // namespace

void APU::setSampleRate(int rate) { cyclesPerSample = CPU_HZ / rate; }

void APU::reset(bool power) {
  for (int i = 0; i < 2; i++) {
    Pulse& p = pulse[i];
    p = Pulse{};
  }
  // no reset o triângulo fica como estava (só desligado); no ligar, tudo zera
  triEnabled = false;
  if (power) {
    triControl = triReloadFlag = triNewHalt = false;
    triLength = triLinear = triLinearLoad = triSeq = triReloadLen = triPrevLen = 0;
    triPeriod = 0; triTimer = 0;
  }
  noiseEnv = Envelope{};
  noiseEnabled = noiseHalt = noiseMode = noiseNewHalt = false;
  noiseLength = noiseReloadLen = noisePrevLen = 0;
  noisePeriod = 4; noiseShift = 1; noiseTimer = 0;
  dmcIrqEnable = dmcLoop = dmcIrq = false;
  dmcSilence = true; dmcBufferEmpty = true;
  dmcRate = 0; dmcBuffer = 0; dmcShift = 0; dmcBits = 8;
  if (power) dmcLevel = 0;
  dmcStart = dmcAddr = 0xC000; dmcLen = 1; dmcRemaining = 0;
  dmcTimer = DMC_RATE[0];
  // o reset age como uma escrita em $4017 com o valor anterior (no ligar, 0)
  if (power) fcNewValue = 0;
  fcInhibit = (fcNewValue & 0x40) != 0;
  fcMode = 0; fcCycle = 0; fcStep = 0; fcBlock = 0;
  fcWriteDelay = 3;
  frameIrq = false;
  odd = false;
  frac = acc = 0; accN = 0;
  wpos = rpos = 0;
}

// ---------------------------------------------------------------- unidades comuns
void APU::envClock(Envelope& e) {
  if (e.start) { e.start = false; e.decay = 15; e.divider = e.vol; return; }
  if (e.divider == 0) {
    e.divider = e.vol;
    if (e.decay) e.decay--;
    else if (e.loop) e.decay = 15;
  } else {
    e.divider--;
  }
}

int APU::sweepTarget(const Pulse& p, bool second) const {
  const int change = p.period >> p.sweepShift;
  if (p.sweepNeg) return p.period - change - (second ? 0 : 1);
  return p.period + change;
}

void APU::sweepClock(Pulse& p, bool second) {
  const int target = sweepTarget(p, second);
  if (p.sweepDiv == 0 && p.sweepOn && p.sweepShift && p.period >= 8 && target <= 0x7FF) p.period = static_cast<uint16_t>(target);
  if (p.sweepDiv == 0 || p.sweepReload) { p.sweepDiv = p.sweepPeriod; p.sweepReload = false; }
  else p.sweepDiv--;
}

int APU::pulseOut(const Pulse& p, bool second) const {
  if (!p.length || p.period < 8 || sweepTarget(p, second) > 0x7FF || !DUTY[p.duty][p.seq]) return 0;
  return p.env.constant ? p.env.vol : p.env.decay;
}

void APU::quarter() {
  envClock(pulse[0].env);
  envClock(pulse[1].env);
  envClock(noiseEnv);
  if (triReloadFlag) triLinear = triLinearLoad;
  else if (triLinear) triLinear--;
  if (!triControl) triReloadFlag = false;
}

void APU::half() {
  for (int i = 0; i < 2; i++) {
    if (pulse[i].length && !pulse[i].halt) pulse[i].length--;
    sweepClock(pulse[i], i == 1);
  }
  if (triLength && !triControl) triLength--;
  if (noiseLength && !noiseHalt) noiseLength--;
}

// recarga do contador de tamanho e mudança do "halt" valem no fim do ciclo; a recarga é ignorada se o contador
// de quadros acabou de mexer no contador neste mesmo ciclo
void APU::lengthEnd() {
  for (auto& p : pulse) {
    if (p.reloadLen) { if (p.length == p.prevLen) p.length = p.reloadLen; p.reloadLen = 0; }
    p.halt = p.newHalt;
  }
  if (triReloadLen) { if (triLength == triPrevLen) triLength = triReloadLen; triReloadLen = 0; }
  triControl = triNewHalt;
  if (noiseReloadLen) { if (noiseLength == noisePrevLen) noiseLength = noiseReloadLen; noiseReloadLen = 0; }
  noiseHalt = noiseNewHalt;
}

// ---------------------------------------------------------------- registradores
void APU::write(uint16_t a, uint8_t v) {
  switch (a) {
    case 0x4000: case 0x4004: {
      Pulse& p = pulse[(a >> 2) & 1];
      p.duty = v >> 6;
      p.newHalt = v & 0x20;
      p.env.loop = v & 0x20;
      p.env.constant = v & 0x10;
      p.env.vol = v & 0xF;
      break;
    }
    case 0x4001: case 0x4005: {
      Pulse& p = pulse[(a >> 2) & 1];
      p.sweepOn = v & 0x80;
      p.sweepPeriod = (v >> 4) & 7;
      p.sweepNeg = v & 8;
      p.sweepShift = v & 7;
      p.sweepReload = true;
      break;
    }
    case 0x4002: case 0x4006: { Pulse& p = pulse[(a >> 2) & 1]; p.period = (p.period & 0x700) | v; break; }
    case 0x4003: case 0x4007: {
      Pulse& p = pulse[(a >> 2) & 1];
      p.period = static_cast<uint16_t>((p.period & 0xFF) | ((v & 7) << 8));
      if (p.enabled) { p.reloadLen = LENGTH[v >> 3]; p.prevLen = p.length; }
      p.seq = 0;
      p.env.start = true;
      break;
    }
    case 0x4008:
      triNewHalt = v & 0x80;
      triLinearLoad = v & 0x7F;
      break;
    case 0x400A: triPeriod = (triPeriod & 0x700) | v; break;
    case 0x400B:
      triPeriod = static_cast<uint16_t>((triPeriod & 0xFF) | ((v & 7) << 8));
      if (triEnabled) { triReloadLen = LENGTH[v >> 3]; triPrevLen = triLength; }
      triReloadFlag = true;
      break;
    case 0x400C:
      noiseNewHalt = v & 0x20;
      noiseEnv.loop = v & 0x20;
      noiseEnv.constant = v & 0x10;
      noiseEnv.vol = v & 0xF;
      break;
    case 0x400E:
      noiseMode = v & 0x80;
      noisePeriod = NOISE[v & 0xF];
      break;
    case 0x400F:
      if (noiseEnabled) { noiseReloadLen = LENGTH[v >> 3]; noisePrevLen = noiseLength; }
      noiseEnv.start = true;
      break;
    case 0x4010:
      dmcIrqEnable = v & 0x80;
      dmcLoop = v & 0x40;
      dmcRate = v & 0xF;
      if (!dmcIrqEnable) dmcIrq = false;
      break;
    case 0x4011: dmcLevel = v & 0x7F; break;
    case 0x4012: dmcStart = static_cast<uint16_t>(0xC000 | (v << 6)); break;
    case 0x4013: dmcLen = static_cast<uint16_t>((v << 4) | 1); break;
    case 0x4015:
      pulse[0].enabled = v & 1;
      pulse[1].enabled = v & 2;
      triEnabled = v & 4;
      noiseEnabled = v & 8;
      if (!pulse[0].enabled) pulse[0].length = 0;
      if (!pulse[1].enabled) pulse[1].length = 0;
      if (!triEnabled) triLength = 0;
      if (!noiseEnabled) noiseLength = 0;
      dmcIrq = false;
      if (!(v & 0x10)) dmcRemaining = 0;
      else if (!dmcRemaining) { dmcAddr = dmcStart; dmcRemaining = dmcLen; }
      break;
    case 0x4017:
      fcNewValue = v;
      fcWriteDelay = (nes.cycles & 1) ? 4 : 3;
      fcInhibit = v & 0x40;
      if (fcInhibit) frameIrq = false;
      break;
    default: break;
  }
}

uint8_t APU::readStatus() {
  uint8_t r = 0;
  if (pulse[0].length) r |= 1;
  if (pulse[1].length) r |= 2;
  if (triLength) r |= 4;
  if (noiseLength) r |= 8;
  if (dmcRemaining) r |= 0x10;
  if (frameIrq) r |= 0x40;
  if (dmcIrq) r |= 0x80;
  frameIrq = false;
  return r;
}

void APU::dmcFetched(uint8_t v) {
  dmcBuffer = v;
  dmcBufferEmpty = false;
  dmcAddr = dmcAddr == 0xFFFF ? 0x8000 : dmcAddr + 1;
  if (--dmcRemaining == 0) {
    if (dmcLoop) { dmcAddr = dmcStart; dmcRemaining = dmcLen; }
    else if (dmcIrqEnable) dmcIrq = true;
  }
}

// ---------------------------------------------------------------- um ciclo de CPU
void APU::clock() {
  // contador de quadros (a escrita em $4017 só vale 3 ou 4 ciclos depois)
  if (fcCycle + 1 >= STEP[fcMode][fcStep]) {
    if (fcMode == 0 && fcStep >= 3 && !fcInhibit) frameIrq = true;
    const int type = STEP_TYPE[fcMode][fcStep];
    if (type != NONE && !fcBlock) {
      quarter();
      if (type == HALF) half();
      fcBlock = 2;
    }
    if (++fcStep == 6) { fcStep = 0; fcCycle = 0; }
    else fcCycle++;
  } else {
    fcCycle++;
  }
  if (fcWriteDelay > 0 && --fcWriteDelay == 0) {
    fcMode = (fcNewValue & 0x80) ? 1 : 0;
    fcCycle = 0;
    fcStep = 0;
    if (fcMode == 1 && !fcBlock) { quarter(); half(); fcBlock = 2; }
  }
  if (fcBlock > 0) fcBlock--;

  // geradores
  if (odd) {
    for (auto& p : pulse) {
      if (p.timer == 0) { p.timer = p.period; p.seq = (p.seq - 1) & 7; }
      else p.timer--;
    }
  }
  if (triTimer == 0) {
    triTimer = triPeriod;
    if (triLength && triLinear && triPeriod >= 2) triSeq = (triSeq + 1) & 31;
  } else {
    triTimer--;
  }
  if (noiseTimer == 0) {
    noiseTimer = noisePeriod - 1;
    const int fb = (noiseShift & 1) ^ ((noiseShift >> (noiseMode ? 6 : 1)) & 1);
    noiseShift = static_cast<uint16_t>((noiseShift >> 1) | (fb << 14));
  } else {
    noiseTimer--;
  }
  if (--dmcTimer <= 0) {
    dmcTimer = DMC_RATE[dmcRate];
    if (!dmcSilence) {
      if (dmcShift & 1) { if (dmcLevel <= 125) dmcLevel += 2; }
      else if (dmcLevel >= 2) dmcLevel -= 2;
    }
    dmcShift >>= 1;
    if (--dmcBits == 0) {
      dmcBits = 8;
      if (dmcBufferEmpty) dmcSilence = true;
      else { dmcSilence = false; dmcShift = dmcBuffer; dmcBufferEmpty = true; }
    }
  }
  odd = !odd;
  lengthEnd();

  // mistura
  const int p = pulseOut(pulse[0], false) + pulseOut(pulse[1], true);
  const int noise = (noiseLength && !(noiseShift & 1)) ? (noiseEnv.constant ? noiseEnv.vol : noiseEnv.decay) : 0;
  acc += MIX.pulse[p] + MIX.tnd[3 * TRI[triSeq] + 2 * noise + dmcLevel] + nes.mapper->audio();
  accN++;
  frac += 1;
  if (frac >= cyclesPerSample) { frac -= cyclesPerSample; emit(); }
}

void APU::emit() {
  const double in = acc / (accN ? accN : 1);
  acc = 0;
  accN = 0;
  // passa-baixa ~14 kHz e passa-alta ~90 Hz (tira o nível contínuo)
  const double sr = CPU_HZ / cyclesPerSample;
  const double aLo = 1.0 - std::exp(-2 * 3.14159265358979 * 14000 / sr);
  lowState += (in - lowState) * aLo;
  const double aHi = 2 * 3.14159265358979 * 90 / sr;
  capHi += (lowState - capHi) * aHi;
  const float out = static_cast<float>((lowState - capHi) * 1.4);
  buf[wpos] = out;
  wpos = (wpos + 1) % bufSize;
  if (wpos == rpos) rpos = (rpos + 1) % bufSize;
}

void APU::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { L[k] = R[k] = 0; continue; }
    L[k] = R[k] = buf[rpos];
    rpos = (rpos + 1) % bufSize;
  }
}

void APU::sync(StateIO& io) {
  for (auto& p : pulse) {
    io.sync(p.env.start); io.sync(p.env.loop); io.sync(p.env.constant); io.sync(p.env.vol); io.sync(p.env.divider); io.sync(p.env.decay);
    io.sync(p.duty); io.sync(p.seq); io.sync(p.length); io.sync(p.period); io.sync(p.timer); io.sync(p.enabled); io.sync(p.halt);
    io.sync(p.sweepOn); io.sync(p.sweepNeg); io.sync(p.sweepReload); io.sync(p.sweepPeriod); io.sync(p.sweepShift); io.sync(p.sweepDiv);
    io.sync(p.reloadLen); io.sync(p.prevLen); io.sync(p.newHalt);
  }
  io.sync(triEnabled); io.sync(triControl); io.sync(triReloadFlag); io.sync(triNewHalt);
  io.sync(triLength); io.sync(triLinear); io.sync(triLinearLoad); io.sync(triSeq); io.sync(triReloadLen); io.sync(triPrevLen);
  io.sync(triPeriod); io.sync(triTimer);
  io.sync(noiseEnv.start); io.sync(noiseEnv.loop); io.sync(noiseEnv.constant); io.sync(noiseEnv.vol); io.sync(noiseEnv.divider); io.sync(noiseEnv.decay);
  io.sync(noiseEnabled); io.sync(noiseHalt); io.sync(noiseMode); io.sync(noiseNewHalt);
  io.sync(noiseLength); io.sync(noiseReloadLen); io.sync(noisePrevLen); io.sync(noisePeriod); io.sync(noiseShift); io.sync(noiseTimer);
  io.sync(dmcIrqEnable); io.sync(dmcLoop); io.sync(dmcIrq); io.sync(dmcSilence); io.sync(dmcBufferEmpty);
  io.sync(dmcRate); io.sync(dmcLevel); io.sync(dmcBuffer); io.sync(dmcShift); io.sync(dmcBits);
  io.sync(dmcStart); io.sync(dmcAddr); io.sync(dmcLen); io.sync(dmcRemaining); io.sync(dmcTimer);
  io.sync(fcMode); io.sync(fcCycle); io.sync(fcStep); io.sync(fcWriteDelay); io.sync(fcBlock); io.sync(fcNewValue);
  io.sync(fcInhibit); io.sync(frameIrq); io.sync(odd);
  io.sync(lowState); io.sync(capHi);
  if (io.reading()) {
    for (auto& p : pulse) { p.duty &= 3; p.seq &= 7; p.period &= 0x7FF; clampTo(p.timer, 0, 0x7FF); p.sweepPeriod &= 7; p.sweepShift &= 7; p.sweepDiv &= 7; p.env.vol &= 15; p.env.divider &= 15; p.env.decay &= 15; }
    triSeq &= 31; triPeriod &= 0x7FF; clampTo(triTimer, 0, 0x7FF); triLinear &= 0x7F; triLinearLoad &= 0x7F;
    noiseEnv.vol &= 15; noiseEnv.divider &= 15; noiseEnv.decay &= 15;
    clampTo<uint16_t>(noisePeriod, 4, 4068); noiseShift &= 0x7FFF; if (!noiseShift) noiseShift = 1; clampTo(noiseTimer, 0, 4068);
    dmcRate &= 15; dmcLevel &= 0x7F; clampTo<uint8_t>(dmcBits, 1, 8); clampTo(dmcTimer, 0, 428);
    fcMode &= 1; clampTo(fcStep, 0, 5); clampTo(fcCycle, 0, 37282); clampTo(fcWriteDelay, 0, 4); clampTo(fcBlock, 0, 2);
    finiteOr(lowState, 0); finiteOr(capHi, 0);
  }
}

}  // namespace indigo::nes
