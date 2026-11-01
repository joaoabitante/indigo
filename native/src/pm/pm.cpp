#include "pm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace indigo::pm {

namespace {
const char STATE_MAGIC[4] = {'I', 'P', 'M', '1'};
constexpr uint32_t OSC1_DIV[8] = {2, 8, 32, 64, 128, 256, 1024, 4096};
constexpr uint64_t T256_CYCLES = 15625;    // 4 MHz / 256 Hz
constexpr uint64_t SECOND_CYCLES = 4000000;

// cores da tela (pixel apagado e aceso), no formato 0xAABBGGRR
constexpr int LIGHT_R = 200, LIGHT_G = 212, LIGHT_B = 176;
constexpr int DARK_R = 24, DARK_G = 32, DARK_B = 24;

// interrupções: número, registrador de ativo/habilita (0..3 = $27..$2A / $23..$26), bit, prioridade
struct IrqInfo { uint8_t num, reg, bit, priReg, priShift; };
constexpr IrqInfo IRQS[] = {
    {0x03, 0, 7, 0, 6}, {0x04, 0, 6, 0, 6}, {0x05, 0, 5, 0, 4}, {0x06, 0, 4, 0, 4},
    {0x07, 0, 3, 0, 2}, {0x08, 0, 2, 0, 2}, {0x09, 0, 1, 0, 0}, {0x0A, 0, 0, 0, 0},
    {0x0B, 1, 5, 1, 6}, {0x0C, 1, 4, 1, 6}, {0x0D, 1, 3, 1, 6}, {0x0E, 1, 2, 1, 6},
    {0x0F, 3, 7, 2, 0}, {0x10, 3, 6, 2, 0}, {0x13, 1, 1, 1, 4}, {0x14, 1, 0, 1, 4},
    {0x15, 2, 7, 1, 2}, {0x16, 2, 6, 1, 2}, {0x17, 2, 5, 1, 2}, {0x18, 2, 4, 1, 2},
    {0x19, 2, 3, 1, 2}, {0x1A, 2, 2, 1, 2}, {0x1B, 2, 1, 1, 2}, {0x1C, 2, 0, 1, 2},
    {0x1D, 3, 2, 1, 0}, {0x1E, 3, 1, 1, 0}, {0x1F, 3, 0, 1, 0},
};
constexpr uint8_t ACT_MASK[4] = {0xFF, 0x3F, 0xFF, 0xF7};

const IrqInfo* irqInfo(int n) {
  for (const IrqInfo& i : IRQS)
    if (i.num == n) return &i;
  return nullptr;
}

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

uint8_t bitRev(uint8_t v) {
  v = uint8_t(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
  v = uint8_t(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
  return uint8_t(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
}

// Avança um contador decrescente de n passos: ao passar de 0 recarrega com "preset" (período = preset + 1).
// pivot >= 0: avisa se o valor cruzou de > pivô para <= pivô e conta os passos que terminaram com valor <= pivô.
struct Advance { bool under = false, pivot = false; uint64_t high = 0; };
void advanceCounter(uint32_t& cnt, uint32_t preset, uint64_t n, int64_t pivot, Advance& r) {
  const uint64_t period = uint64_t(preset) + 1;
  while (n) {
    if (cnt == 0) {
      cnt = preset;
      r.under = true;
      n--;
      if (pivot >= 0 && cnt <= pivot) r.high++;
      if (n >= period) {  // períodos inteiros de uma vez
        const uint64_t k = n / period;
        n -= k * period;
        if (pivot >= 0) {
          r.high += k * (uint64_t(std::min<int64_t>(pivot, preset)) + 1);
          if (preset > pivot) r.pivot = true;
        }
      }
      continue;
    }
    const uint64_t step = std::min<uint64_t>(n, cnt);
    const uint32_t lo = uint32_t(cnt - step);
    if (pivot >= 0) {
      if (int64_t(cnt) > pivot && int64_t(lo) <= pivot) r.pivot = true;
      if (pivot >= int64_t(lo)) r.high += uint64_t(std::min<int64_t>(pivot, cnt - 1) - lo + 1);
    }
    cnt = lo;
    n -= step;
  }
}
}  // namespace

bool isPMRom(const std::vector<uint8_t>& rom, const std::string& nome) {
  if (endsWith(nome, ".min")) return true;
  if (rom.size() < 0x21D0) return false;
  return rom[0x2100] == 'M' && rom[0x2101] == 'N' && std::memcmp(&rom[0x21A4], "NINTENDO", 8) == 0;
}

// ------------------------------------------------------------------ construção e reset
PM::PM(const std::vector<uint8_t>& data, const std::vector<uint8_t>& biosFile)
    : eeprom(8192, 0xFF), bufL(AUDIO_BUF), bufR(AUDIO_BUF) {
  size_t size = 0x4000;
  while (size < data.size()) size <<= 1;
  rom.assign(size, 0xFF);
  std::copy(data.begin(), data.end(), rom.begin());
  romMask = uint32_t(size - 1);
  if (biosFile.size() == 4096) {
    std::copy(biosFile.begin(), biosFile.end(), bios);
    usingFreeBios = false;
  } else {
    std::copy(FREEBIOS, FREEBIOS + 4096, bios);
  }
  cpu.bus = this;
  reset(true);
}

void PM::writeRtcStamp() {
  // como o PokeMini: a hora do computador vai para o fim da EEPROM (onde a BIOS guarda a data acertada) e o
  // contador de segundos começa em 0
  const std::time_t now = std::time(nullptr);
  const std::tm* tmv = std::localtime(&now);
  if (!tmv) return;
  const uint8_t v[6] = {uint8_t(tmv->tm_year % 100), uint8_t(tmv->tm_mon + 1), uint8_t(tmv->tm_mday),
                        uint8_t(tmv->tm_hour), uint8_t(tmv->tm_min), uint8_t(tmv->tm_sec)};
  uint8_t sum = 0;
  eeprom[0x1FF6] = eeprom[0x1FF7] = eeprom[0x1FF8] = 0;
  for (int k = 0; k < 6; k++) { eeprom[0x1FF9 + k] = v[k]; sum = uint8_t(sum + v[k]); }
  eeprom[0x1FFF] = sum;
  io[0x02] |= 0x02;
  seconds = 0;
}

void PM::reset(bool hard) {
  (void)hard;  // o reset do aparelho é sempre "ligar de novo" (a EEPROM fica)
  std::memset(ram, 0xFF, sizeof ram);
  std::memset(io, 0, sizeof io);
  io[0x00] = 0x7F; io[0x01] = 0x20; io[0x02] = 0x5C; io[0x08] = 0x01; io[0x10] = 0x08;
  io[0x21] = 0x30; io[0x22] = 0x02; io[0x24] = 0x02; io[0x50] = 0xFF; io[0x52] = 0xFF;
  io[0x54] = 0x01; io[0x55] = 0x01; io[0x60] = 0x32; io[0x61] = 0x64; io[0x81] = 0x07; io[0xFE] = 0x40;
  for (Timer& t : tm) t = Timer();
  pivot3 = 0;
  t256 = 0;
  seconds = 0;
  shockPrev = false;
  cycles = frameStart = stallUntil = timerSynced = miscSynced = 0;
  frameDone = false;
  prcState = 0;
  prcRateMatch = 0x10;
  mapPX = mapPY = 0;
  std::memset(gdram, 0, sizeof gdram);
  lcdCol = lcdPage = lcdStart = 0;
  lcdContrast = 0x1F;
  lcdSetContrast = lcdSegRev = lcdMaxContrast = lcdAllOn = lcdInvert = lcdOn = lcdRowRev = lcdRmw = false;
  lcdDummy = true;
  lcdRmwCol = 0;
  lcdReadLatch = 0x40;
  lcdWritten = false;
  std::memset(lcdPrev, 0, sizeof lcdPrev);
  std::memset(lcdCur, 0, sizeof lcdCur);
  i2cLast = 0x0C;
  i2cState = i2cBit = i2cByte = i2cPhase = 0;
  i2cOut = 1;
  i2cAddr = 0;
  nextSample = 0;
  accHigh = accTotal = hpCap = 0;
  wpos = rpos = 0;
  writeRtcStamp();
  cpu.reset(true);
  lcdRefresh(false);
}

void PM::importEeprom(const std::vector<uint8_t>& b) {
  if (b.empty()) return;
  std::copy(b.begin(), b.begin() + std::min(b.size(), eeprom.size()), eeprom.begin());
}

std::string PM::title() const {
  if (std::memcmp(&rom[0x21A4], "NINTENDO", 8) != 0) return "";
  std::string t;
  for (int k = 0; k < 12; k++) {
    const uint8_t c = rom[0x21B0 + k];
    if (c < 0x20 || c >= 0x7F) break;
    t += char(c);
  }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  return t;
}

// ------------------------------------------------------------------ barramento
uint8_t PM::read(uint32_t a) {
  if (a < 0x1000) return bios[a];
  if (a < 0x2000) return ram[a - 0x1000];
  if (a < 0x2100) return ioRead(uint8_t(a));
  return rom[a & romMask];
}

void PM::write(uint32_t a, uint8_t v) {
  if (a < 0x1000) return;
  if (a < 0x2000) { ram[a - 0x1000] = v; return; }
  if (a < 0x2100) ioWrite(uint8_t(a), v);
  // ROM: gravações ignoradas (cartuchos com flash não são emulados)
}

// leitura sem efeitos colaterais para o PRC
static inline uint8_t prcPeek(const uint8_t* bios, const uint8_t* ram, const uint8_t* io,
                              const std::vector<uint8_t>& rom, uint32_t mask, uint32_t a) {
  a &= 0xFFFFFF;
  if (a < 0x1000) return bios[a];
  if (a < 0x2000) return ram[a - 0x1000];
  if (a < 0x2100) return io[a & 0xFF];
  return rom[a & mask];
}

// ------------------------------------------------------------------ interrupções
void PM::setIrq(int n) {
  const IrqInfo* i = irqInfo(n);
  if (!i) return;
  io[0x27 + i->reg] |= uint8_t(1 << i->bit);
  irqCheck();
}

// a de maior prioridade entre as ativas e habilitadas (empate: menor número); só entra se a CPU aceita
// (fora do atraso do U e sem I0+I1 ligados, como no PokeMini)
void PM::irqCheck() {
  if (!cpu.irqMaster()) return;
  if (!((io[0x27] & io[0x23]) | (io[0x28] & io[0x24] & 0x3F) | (io[0x29] & io[0x25]) |
        (io[0x2A] & io[0x26] & 0xF7)))
    return;
  int best = -1, bestPri = 0;
  for (const IrqInfo& i : IRQS) {
    const uint8_t m = uint8_t(1 << i.bit);
    if (!(io[0x27 + i.reg] & io[0x23 + i.reg] & ACT_MASK[i.reg] & m)) continue;
    const int pri = (io[0x20 + i.priReg] >> i.priShift) & 3;
    if (pri > bestPri) { bestPri = pri; best = i.num; }
  }
  if (best >= 0) cpu.requestIrq(uint8_t(best * 2));
}

// ------------------------------------------------------------------ timers
bool PM::halfRunning(int t, bool hi) const {
  const Timer& T = tm[t];
  if (hi && (T.ctrlL & 0x80)) return false;  // 16 bits: só a configuração de baixo vale
  if (!((hi ? T.ctrlH : T.ctrlL) & 0x04)) return false;
  if (!(T.scale & (hi ? 0x80 : 0x08))) return false;
  const bool osc2 = T.osc & (hi ? 2 : 1);
  return io[0x19] & (osc2 ? 0x10 : 0x20);
}

uint32_t PM::halfDivider(int t, bool hi, bool& osc2) const {
  const Timer& T = tm[t];
  osc2 = T.osc & (hi ? 2 : 1);
  const int idx = (hi ? T.scale >> 4 : T.scale) & 7;
  return osc2 ? (1u << idx) : OSC1_DIV[idx];
}

// passos do pré-divisor até o ciclo t (o oscilador 2 é o cristal de 32768 Hz)
uint64_t PM::tickCount(uint64_t t, uint32_t div, bool osc2) const {
  return osc2 ? (t * 256) / (31250ull * div) : t / div;
}

// primeiro ciclo em que tickCount chega a "tick"
uint64_t PM::tickTime(uint64_t tick, uint32_t div, bool osc2) const {
  if (!osc2) return tick * div;
  const uint64_t num = tick * 31250ull * div;
  return (num + 255) / 256;
}

void PM::syncTimers(uint64_t t) {
  if (t <= timerSynced) return;
  const uint64_t from = timerSynced;
  timerSynced = t;
  double level = 0, tickHigh = -1;  // para o som (timer 3)
  for (int i = 0; i < 3; i++) {
    Timer& T = tm[i];
    const bool wide = T.ctrlL & 0x80;
    for (int h = 0; h < 2; h++) {
      const bool hi = h == 1;
      if (i == 2 && !hi && !wide) {
        // som: nível antes dos passos
      }
      if (!halfRunning(i, hi)) continue;
      bool osc2;
      const uint32_t div = halfDivider(i, hi, osc2);
      const uint64_t n = tickCount(t, div, osc2) - tickCount(from, div, osc2);
      if (!n) continue;
      Advance r;
      if (wide) {
        uint32_t c = (T.cntH << 8) | T.cntL;
        const int64_t piv = i == 2 ? int64_t(pivot3) : -1;
        advanceCounter(c, (T.preH << 8) | T.preL, n, piv, r);
        T.cntL = uint8_t(c);
        T.cntH = uint8_t(c >> 8);
        if (r.under) setIrq(i == 0 ? 0x07 : i == 1 ? 0x05 : 0x09);
        if (i == 2) {
          if (r.pivot) setIrq(0x0A);
          tickHigh = double(r.high) / double(n);
        }
      } else {
        uint32_t c = hi ? T.cntH : T.cntL;
        const int64_t piv = (i == 2 && hi) ? int64_t(T.pvtH) : -1;
        advanceCounter(c, hi ? T.preH : T.preL, n, piv, r);
        (hi ? T.cntH : T.cntL) = uint8_t(c);
        if (r.under) {
          if (hi) setIrq(i == 0 ? 0x07 : i == 1 ? 0x05 : 0x09);
          else if (i < 2) setIrq(i == 0 ? 0x08 : 0x06);
        }
        if (i == 2 && hi) {
          if (r.pivot) setIrq(0x0A);
          tickHigh = double(r.high) / double(n);
        }
      }
    }
  }
  // som: fração do tempo com a saída alta (contador do timer 3 <= pivô)
  const Timer& T3 = tm[2];
  if (tickHigh >= 0) level = tickHigh;
  else if (T3.ctrlL & 0x80) level = ((T3.cntH << 8) | T3.cntL) <= pivot3 ? 1 : 0;
  else level = T3.cntH <= T3.pvtH ? 1 : 0;
  const double d = double(t - from);
  accHigh += level * d;
  accTotal += d;
}

void PM::timerCtrl(int t, bool hi, uint8_t v) {
  Timer& T = tm[t];
  if (hi) {
    T.ctrlH = v & 0x0D;
    if ((v & 0x02) && !(T.ctrlL & 0x80)) T.cntH = T.preH;
  } else {
    T.ctrlL = v & 0x8D;
    if (v & 0x02) {
      T.cntL = T.preL;
      if (v & 0x80) T.cntH = T.preH;
    }
  }
}

// passos até a próxima interrupção que ainda não está marcada como ativa (0 = nenhuma)
uint64_t PM::eventTicks(int t, bool hi) const {
  const Timer& T = tm[t];
  const bool wide = T.ctrlL & 0x80;
  uint64_t best = 0;
  auto consider = [&](uint64_t k) { if (k && (!best || k < best)) best = k; };
  const int underIrq = (hi || wide) ? (t == 0 ? 0x07 : t == 1 ? 0x05 : 0x09) : (t == 0 ? 0x08 : t == 1 ? 0x06 : 0);
  const uint32_t cnt = wide ? uint32_t((T.cntH << 8) | T.cntL) : (hi ? T.cntH : T.cntL);
  const uint32_t pre = wide ? uint32_t((T.preH << 8) | T.preL) : (hi ? T.preH : T.preL);
  if (underIrq) {
    const IrqInfo* i = irqInfo(underIrq);
    if (!(io[0x27 + i->reg] & (1 << i->bit))) consider(uint64_t(cnt) + 1);
  }
  if (t == 2 && (hi || wide) && !(io[0x27] & 0x01)) {
    const uint32_t piv = wide ? pivot3 : T.pvtH;
    if (cnt > piv) consider(cnt - piv);
    else if (pre > piv) consider(uint64_t(cnt) + 1 + (pre - piv));
  }
  return best;
}

// ------------------------------------------------------------------ som
void PM::setSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 192000) rate = 192000;
  cyclesPerSample = double(CPU_HZ) / rate;
}

void PM::emitSample() {
  double v = accTotal > 0 ? accHigh / accTotal : 0;
  accHigh = accTotal = 0;
  const int vol = io[0x71] & 3;
  double gain = vol == 0 ? 0 : vol == 3 ? 1.0 : 0.5;
  if (io[0x70] & 3) gain = 0;
  v *= gain * 0.5;
  // passa-alta (o alto-falante piezo não reproduz o nível contínuo)
  const double out = v - hpCap;
  hpCap += out * 0.002;
  bufL[wpos] = bufR[wpos] = float(out);
  wpos = (wpos + 1) % AUDIO_BUF;
  if (wpos == rpos) rpos = (rpos + 1) % AUDIO_BUF;
}

void PM::syncAudio(uint64_t t) {
  if (nextSample < double(timerSynced)) nextSample = double(timerSynced);
  while (nextSample <= double(t)) {
    syncTimers(uint64_t(std::ceil(nextSample)));
    emitSample();
    nextSample += cyclesPerSample;
  }
  syncTimers(t);
}

void PM::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { l[k] = 0; r[k] = 0; continue; }
    l[k] = bufL[rpos];
    r[k] = bufR[rpos];
    rpos = (rpos + 1) % AUDIO_BUF;
  }
}

// ------------------------------------------------------------------ sincronização e eventos
int PM::prcCount() const {
  const uint64_t d = cycles >= frameStart ? cycles - frameStart : 0;
  return int(std::min<uint64_t>(1 + d / PRC_STEP, 0x41));
}

void PM::sync() {
  const uint64_t t = cycles;
  syncAudio(t);
  // timer de 256 Hz e contador de segundos (contam o tempo absoluto: o divisor é livre)
  if (t > miscSynced) {
    const uint64_t k = t / T256_CYCLES - miscSynced / T256_CYCLES;
    if (k && (io[0x40] & 1)) {
      const uint64_t a = t256, b = a + k;
      if ((a >> 3) != (b >> 3)) setIrq(0x0B);
      if ((a >> 5) != (b >> 5)) setIrq(0x0C);
      if ((a >> 7) != (b >> 7)) setIrq(0x0D);
      if ((a >> 8) != (b >> 8)) setIrq(0x0E);
      t256 = uint8_t(b);
    }
    const uint64_t s = t / SECOND_CYCLES - miscSynced / SECOND_CYCLES;
    if (s && (io[0x08] & 1)) seconds = uint32_t((seconds + s) & 0xFFFFFF);
    miscSynced = t;
  }
  // PRC: desenho no passo $18, cópia no $39, fim do quadro depois do $41
  for (;;) {
    const bool active = (io[0x81] & 0xF0) >= prcRateMatch;
    const int mode = (io[0x80] & 0x08) ? ((io[0x80] & 0x06) ? 2 : 1) : 0;
    if (prcState == 0) {
      if (t < frameStart + 0x17 * PRC_STEP) break;
      if (active && mode == 2) {
        prcRender();
        stallUntil = frameStart + FRAME_CYCLES;
      }
      prcState = 1;
    } else if (prcState == 1) {
      if (t < frameStart + 0x38 * PRC_STEP) break;
      if (active && mode) {
        prcCopy();
        stallUntil = frameStart + FRAME_CYCLES;
        setIrq(0x03);
      }
      prcState = 2;
    } else {
      if (t < frameStart + FRAME_CYCLES) break;
      frameEnd();
      frameStart += FRAME_CYCLES;
      prcState = 0;
    }
  }
}

uint64_t PM::nextEvent() {
  uint64_t best = frameStart + (prcState == 0 ? 0x17 * PRC_STEP : prcState == 1 ? 0x38 * PRC_STEP : FRAME_CYCLES);
  for (int i = 0; i < 3; i++) {
    for (int h = 0; h < 2; h++) {
      if (!halfRunning(i, h)) continue;
      const uint64_t k = eventTicks(i, h);
      if (!k) continue;
      bool osc2;
      const uint32_t div = halfDivider(i, h, osc2);
      best = std::min(best, tickTime(tickCount(cycles, div, osc2) + k, div, osc2));
    }
  }
  if ((io[0x40] & 1) && (~io[0x28] & 0x3C)) best = std::min(best, (cycles / T256_CYCLES + 1) * T256_CYCLES);
  return std::max(best, cycles + 1);
}

void PM::runFrame() {
  frameDone = false;
  while (!frameDone) {
    sync();
    if (frameDone) break;
    const uint64_t dl = nextEvent();
    resched = false;
    if (cycles < stallUntil) {
      cycles = std::min(dl, stallUntil);
      continue;
    }
    while (cycles < dl && !resched) {
      if ((cpu.status == Minx::HALT || cpu.status == Minx::STOP) && cpu.shiftU == 0) {
        cycles = dl;
        break;
      }
      cycles += uint64_t(cpu.step());
    }
  }
}

// ------------------------------------------------------------------ teclas
void PM::setKeys(uint32_t mask) {
  // bits do registrador $52 (0 = apertado): A, B, C, cima, baixo, esquerda, direita, Power
  static const uint32_t MAP[8] = {KEY_A, KEY_B, KEY_R | KEY_X, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_SELECT};
  static const int IRQ[8] = {0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15};
  sync();
  uint8_t pad = 0xFF;
  for (int b = 0; b < 8; b++)
    if (mask & MAP[b]) pad &= uint8_t(~(1 << b));
  const uint8_t pressed = uint8_t(io[0x52] & ~pad);  // estava solto e agora está apertado
  io[0x52] = pad;
  for (int b = 0; b < 8; b++)
    if (pressed & (1 << b)) setIrq(IRQ[b]);
  const bool shock = mask & KEY_L;
  if (shock && !shockPrev) setIrq(0x10);
  shockPrev = shock;
  keys = mask;
}

// ------------------------------------------------------------------ registradores
uint8_t PM::ioRead(uint8_t r) {
  sync();
  switch (r) {
    case 0x00: case 0x01: case 0x02: return io[r];
    case 0x08: return io[0x08] & 1;
    case 0x09: return uint8_t(seconds);
    case 0x0A: return uint8_t(seconds >> 8);
    case 0x0B: return uint8_t(seconds >> 16);
    case 0x10: return io[0x10];
    case 0x18: return tm[0].scale;
    case 0x19: return io[0x19] & 0x33;
    case 0x1A: return tm[1].scale;
    case 0x1B: return tm[1].osc & 3;
    case 0x1C: return tm[2].scale;
    case 0x1D: return tm[2].osc & 3;
    case 0x20: case 0x21: case 0x23: case 0x25: case 0x27: case 0x29: return io[r];
    case 0x22: return io[r] & 0x03;
    case 0x24: case 0x28: return io[r] & 0x3F;
    case 0x26: case 0x2A: return io[r] & 0xF7;
    case 0x30: case 0x38: case 0x48: return tm[(r - 0x30) >> 3 == 0 ? 0 : r == 0x38 ? 1 : 2].ctrlL;
    case 0x31: case 0x39: case 0x49: return tm[r == 0x31 ? 0 : r == 0x39 ? 1 : 2].ctrlH;
    case 0x32: case 0x3A: case 0x4A: return tm[r == 0x32 ? 0 : r == 0x3A ? 1 : 2].preL;
    case 0x33: case 0x3B: case 0x4B: return tm[r == 0x33 ? 0 : r == 0x3B ? 1 : 2].preH;
    case 0x34: case 0x3C: return tm[r == 0x34 ? 0 : 1].pvtL;
    case 0x35: case 0x3D: return tm[r == 0x35 ? 0 : 1].pvtH;
    case 0x4C: return uint8_t(pivot3);
    case 0x4D: return uint8_t(pivot3 >> 8);
    case 0x36: case 0x3E: case 0x4E: return tm[r == 0x36 ? 0 : r == 0x3E ? 1 : 2].cntL;
    case 0x37: case 0x3F: case 0x4F: return tm[r == 0x37 ? 0 : r == 0x3F ? 1 : 2].cntH;
    case 0x40: return io[0x40] & 1;
    case 0x41: return t256;
    case 0x44: case 0x45: case 0x46: case 0x47: case 0x50: case 0x51: case 0x53: case 0x54: case 0x55:
    case 0x60: case 0x62:
      return io[r];
    case 0x52: return io[0x52];
    case 0x61: return ioPortRead();
    case 0x70: case 0x71: return io[r] & 0x07;
    case 0x80: return io[0x80] & 0x3F;
    case 0x81: return io[0x81];
    case 0x82: return io[0x82] & 0xF8;
    case 0x83: case 0x88: return io[r];
    case 0x84: case 0x89: return io[r] & 0x1F;
    case 0x85: case 0x86: return io[r] & 0x7F;
    case 0x87: return io[0x87] & 0xC0;
    case 0x8A: return uint8_t(prcCount());
    case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: return 0;
    case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF7: return 0;
    case 0xFE:
      if (lcdSetContrast) { lcdSetContrast = false; lcdContrast = 0x3F; return 0; }
      return uint8_t(0x40 | (lcdOn ? 0x20 : 0));
    case 0xFF: return lcdReadData();
    default: return cpu.IR;  // barramento aberto
  }
}

void PM::ioWrite(uint8_t r, uint8_t v) {
  sync();
  resched = true;
  switch (r) {
    case 0x00: case 0x01: case 0x02: io[r] = v; return;
    case 0x08:
      if (v & 0x02) seconds = 0;
      io[0x08] = v & 1;
      return;
    case 0x10: io[0x10] = uint8_t((io[0x10] & 0x20) | (v & 0x1F)); return;
    case 0x18: tm[0].scale = v; return;
    case 0x19: io[0x19] = v & 0x33; tm[0].osc = v & 3; return;
    case 0x1A: tm[1].scale = v; return;
    case 0x1B: tm[1].osc = v & 3; return;
    case 0x1C: tm[2].scale = v; return;
    case 0x1D: tm[2].osc = v & 3; return;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x25: io[r] = v; irqCheck(); return;
    case 0x24: io[r] = v & 0x3F; irqCheck(); return;
    case 0x26: io[r] = v & 0xF7; irqCheck(); return;
    case 0x27: case 0x28: case 0x29: case 0x2A: io[r] &= uint8_t(~v); return;
    case 0x30: timerCtrl(0, false, v); return;
    case 0x31: timerCtrl(0, true, v); return;
    case 0x38: timerCtrl(1, false, v); return;
    case 0x39: timerCtrl(1, true, v); return;
    case 0x48: timerCtrl(2, false, v); return;
    case 0x49: timerCtrl(2, true, v); return;
    case 0x32: tm[0].preL = v; return;
    case 0x33: tm[0].preH = v; return;
    case 0x3A: tm[1].preL = v; return;
    case 0x3B: tm[1].preH = v; return;
    case 0x4A: tm[2].preL = v; return;
    case 0x4B: tm[2].preH = v; return;
    case 0x34: tm[0].pvtL = v; return;
    case 0x35: tm[0].pvtH = v; return;
    case 0x3C: tm[1].pvtL = v; return;
    case 0x3D: tm[1].pvtH = v; return;
    case 0x4C: pivot3 = uint16_t((pivot3 & 0xFF00) | v); tm[2].pvtL = v; return;
    case 0x4D: pivot3 = uint16_t((pivot3 & 0x00FF) | (v << 8)); tm[2].pvtH = v; return;
    case 0x40:
      io[0x40] = v & 1;
      if (v & 0x02) t256 = 0;
      return;
    case 0x44: io[r] = v & 0xF7; return;
    case 0x45: case 0x47: io[r] = v & 0x0F; return;
    case 0x46: case 0x50: io[r] = v; return;
    case 0x51: io[r] = v & 0x03; return;
    case 0x53: io[r] = 0; return;
    case 0x54: io[r] = v & 0x77; return;
    case 0x55: io[r] = v & 0x07; return;
    case 0x60: case 0x61: io[r] = v; i2cUpdate(); return;  // motor de vibração e infravermelho: ignorados
    case 0x62: io[r] = v & 0xF0; return;
    case 0x70: case 0x71: io[r] = v & 0x07; return;
    case 0x80: io[0x80] = v & 0x3F; return;
    case 0x81: {
      if ((io[0x81] & 0x0E) != (v & 0x0E)) io[0x81] = v & 0x0F;
      else io[0x81] = uint8_t((io[0x81] & 0xF0) | (v & 0x0F));
      static const uint8_t MATCH[8] = {0x20, 0x50, 0x80, 0xB0, 0x10, 0x30, 0x50, 0x70};  // /3 /6 /9 /12 /2 /4 /6 /8
      prcRateMatch = MATCH[(v >> 1) & 7];
      return;
    }
    case 0x82: io[r] = v & 0xF8; return;
    case 0x83: case 0x88: io[r] = v; return;
    case 0x84: case 0x89: io[r] = v & 0x1F; return;
    case 0x85: {
      io[r] = v & 0x7F;
      static const int TH[4] = {16, 12, 8, 16};
      if (io[r] <= TH[(io[0x80] >> 4) & 3] * 8 - 64) mapPY = io[r];
      return;
    }
    case 0x86: {
      io[r] = v & 0x7F;
      static const int TW[4] = {12, 16, 24, 24};
      if (io[r] <= TW[(io[0x80] >> 4) & 3] * 8 - 96) mapPX = io[r];
      return;
    }
    case 0x87: io[r] = v & 0xC0; return;
    case 0xFE: lcdCtrl(v); return;
    case 0xFF: lcdData(v); return;
    default: return;
  }
}

// ------------------------------------------------------------------ porta de E/S e EEPROM (I2C)
uint8_t PM::ioPortRead() {
  const uint8_t dir = io[0x60], data = io[0x61];
  const uint8_t sda = (i2cOut && ((dir & 0x04) == 0 || (data & 0x04))) ? 0x04 : 0;
  return uint8_t((data & dir) | (sda & ~dir));
}

void PM::i2cUpdate() {
  const uint8_t dir = io[0x60], data = io[0x61];
  // linha solta = alta (resistor de pull-up)
  const uint8_t lines = uint8_t(((dir & 0x04) ? (data & 0x04) : 0x04) | ((dir & 0x08) ? (data & 0x08) : 0x08));
  const uint8_t prev = i2cLast;
  i2cLast = lines;
  const bool sclNow = lines & 0x08, sclPrev = prev & 0x08;
  const bool sdaNow = lines & 0x04, sdaPrev = prev & 0x04;
  if (sclNow && sclPrev && sdaPrev && !sdaNow) {  // START
    i2cState = 1; i2cBit = 0; i2cByte = 0; i2cPhase = 0; i2cOut = 1;
    return;
  }
  if (sclNow && sclPrev && !sdaPrev && sdaNow) {  // STOP
    i2cState = 0; i2cOut = 1;
    return;
  }
  if (sclNow && !sclPrev) {  // subida do relógio: lê a linha
    if (i2cState == 1) {
      if (i2cBit < 8) { i2cByte = uint8_t((i2cByte << 1) | (sdaNow ? 1 : 0)); i2cBit++; }
      else if (i2cBit == 8) i2cBit = 9;
    } else if (i2cState == 2) {
      if (i2cBit < 8) i2cBit++;
      else if (i2cBit == 8) {
        i2cBit = 9;
        if (sdaNow) i2cState = 3;  // a CPU não confirmou: acabou a leitura
      }
    }
    return;
  }
  if (!sclNow && sclPrev) {  // descida do relógio
    if (i2cState == 1) {
      if (i2cBit == 8) {  // byte completo: processa e confirma
        bool ack = true;
        switch (i2cPhase) {
          case 0:  // endereço do chip
            if ((i2cByte & 0xF0) != 0xA0) { ack = false; break; }
            i2cPhase = (i2cByte & 1) ? 4 : 1;
            break;
          case 1: i2cAddr = uint16_t(((i2cByte << 8) | (i2cAddr & 0xFF)) & 0x1FFF); i2cPhase = 2; break;
          case 2: i2cAddr = uint16_t((i2cAddr & 0x1F00) | i2cByte); i2cPhase = 3; break;
          case 3:
            eeprom[i2cAddr & 0x1FFF] = i2cByte;
            i2cAddr = (i2cAddr + 1) & 0x1FFF;
            eepromDirty = true;
            break;
          default: break;
        }
        i2cOut = ack ? 0 : 1;
        if (!ack) i2cState = 3;
      } else if (i2cBit == 9) {
        i2cBit = 0;
        i2cByte = 0;
        i2cOut = 1;
        if (i2cPhase == 4) {  // leitura: começa a mandar os bytes
          i2cState = 2;
          i2cByte = eeprom[i2cAddr & 0x1FFF];
          i2cOut = i2cByte >> 7;
        }
      }
    } else if (i2cState == 2) {
      if (i2cBit >= 1 && i2cBit < 8) i2cOut = (i2cByte >> (7 - i2cBit)) & 1;
      else if (i2cBit == 8) i2cOut = 1;  // solta a linha para a confirmação da CPU
      else if (i2cBit == 9) {
        i2cBit = 0;
        i2cAddr = (i2cAddr + 1) & 0x1FFF;
        i2cByte = eeprom[i2cAddr];
        i2cOut = i2cByte >> 7;
      }
    }
  }
}

// ------------------------------------------------------------------ PRC
void PM::prcRender() {
  auto peek = [&](uint32_t a) { return prcPeek(bios, ram, io, rom, romMask, a); };
  uint8_t* fbuf = ram;  // $1000-$12FF
  const uint8_t mode = io[0x80];
  if (mode & 0x02) {
    static const int TW[4] = {12, 16, 24, 24};
    const int tw = TW[(mode >> 4) & 3];
    const uint32_t base = (io[0x82] & 0xF8) | (io[0x83] << 8) | ((io[0x84] & 0x1F) << 16);
    for (int page = 0; page < 8; page++) {
      const int ty = page * 8 + mapPY;
      for (int x = 0; x < 96; x++) {
        const int tx = x + mapPX;
        const uint32_t idx = 0x1360 + (ty >> 3) * tw + (tx >> 3);
        const uint32_t top = base + peek(idx) * 8, bot = base + peek(idx + tw) * 8;
        uint8_t d = uint8_t(peek(top + (tx & 7)) >> (ty & 7));
        if (ty & 7) d |= uint8_t(peek(bot + (tx & 7)) << (8 - (ty & 7)));
        fbuf[page * 96 + x] = (mode & 0x01) ? uint8_t(~d) : d;
      }
    }
  }
  if (mode & 0x04) {
    const uint32_t base = (io[0x87] & 0xC0) | (io[0x88] << 8) | ((io[0x89] & 0x1F) << 16);
    for (int s = 23; s >= 0; s--) {
      const uint8_t* at = &ram[0x300 + s * 4];
      const uint8_t ctl = at[3];
      if (!(ctl & 0x08)) continue;
      const int sx = (at[0] & 0x7F) - 16, sy = (at[1] & 0x7F) - 16;
      const uint32_t tile = base + at[2] * 64;
      for (int c = 0; c < 16; c++) {
        const int x = sx + c;
        if (x < 0 || x >= 96) continue;
        const int scol = (ctl & 0x01) ? 15 - c : c;  // espelho horizontal
        const uint32_t sub = tile + ((scol >= 8) ? 32 : 0) + (scol & 7);
        // coluna de 16 pixels: máscara (blocos 0/1 ou 4/5) e desenho (2/3 ou 6/7), bit 0 = em cima
        uint16_t mask = uint16_t(peek(sub) | (peek(sub + 8) << 8));
        uint16_t draw = uint16_t(peek(sub + 16) | (peek(sub + 24) << 8));
        if (ctl & 0x02) {  // espelho vertical
          mask = uint16_t((bitRev(uint8_t(mask)) << 8) | bitRev(uint8_t(mask >> 8)));
          draw = uint16_t((bitRev(uint8_t(draw)) << 8) | bitRev(uint8_t(draw >> 8)));
        }
        if (ctl & 0x04) draw = uint16_t(~draw);
        for (int r = 0; r < 16; r++) {
          const int y = sy + r;
          if (y < 0 || y >= 64) continue;
          if ((mask >> r) & 1) continue;  // 1 = transparente
          uint8_t& b = fbuf[(y >> 3) * 96 + x];
          const uint8_t bit = uint8_t(1 << (y & 7));
          if ((draw >> r) & 1) b |= bit;
          else b &= uint8_t(~bit);
        }
      }
    }
  }
}

void PM::prcCopy() {
  for (int page = 0; page < 8; page++)
    for (int x = 0; x < 96; x++) gdram[page][lcdSegRev ? 131 - x : x] = ram[page * 96 + x];
  lcdPage = 7;
  lcdCol = 96;
  lcdDummy = true;
  lcdRmw = false;
  lcdWritten = true;
}

void PM::frameEnd() {
  const bool active = (io[0x81] & 0xF0) >= prcRateMatch;
  if (active) {
    io[0x81] &= 0x0F;
    setIrq(0x04);
  } else {
    io[0x81] = uint8_t(io[0x81] + 0x10);
  }
  lcdRefresh(active && (io[0x80] & 0x08));
  frameDone = true;
}

// ------------------------------------------------------------------ LCD
void PM::lcdCtrl(uint8_t v) {
  if (lcdSetContrast) {
    lcdSetContrast = false;
    lcdContrast = v & 0x3F;
    return;
  }
  if (v <= 0x0F) { if (!lcdRmw) { lcdCol = uint8_t((lcdCol & 0xF0) | v); lcdDummy = true; } return; }
  if (v <= 0x1F) { if (!lcdRmw) { lcdCol = uint8_t((lcdCol & 0x0F) | ((v & 0x0F) << 4)); lcdDummy = true; } return; }
  if (v >= 0x40 && v <= 0x7F) { lcdStart = v - 0x40; return; }
  if (v >= 0xB0 && v <= 0xB8) { lcdPage = v & 0x0F; return; }
  if (v >= 0xC0 && v <= 0xCF) { lcdRowRev = v >= 0xC8; return; }
  switch (v) {
    case 0x81: lcdSetContrast = true; return;
    case 0xA0: case 0xA1: lcdSegRev = v & 1; return;
    case 0xA2: case 0xA3: lcdMaxContrast = v & 1; return;
    case 0xA4: case 0xA5: lcdAllOn = v & 1; return;
    case 0xA6: case 0xA7: lcdInvert = v & 1; return;
    case 0xAE: case 0xAF: lcdOn = v & 1; return;
    case 0xE0: lcdRmw = true; lcdRmwCol = lcdCol; return;
    case 0xE2:  // reset do controlador
      lcdSetContrast = false; lcdContrast = 0x20; lcdCol = 0; lcdStart = 0; lcdSegRev = false;
      lcdMaxContrast = false; lcdAllOn = false; lcdInvert = false; lcdOn = false; lcdPage = 0;
      lcdRowRev = false; lcdRmw = false; lcdDummy = true; lcdRmwCol = 0;
      return;
    case 0xEE: lcdRmw = false; lcdCol = lcdRmwCol; return;
    default: return;  // tensões, oscilador etc.: sem efeito visível aqui
  }
}

void PM::lcdData(uint8_t v) {
  if (lcdSetContrast) {
    lcdSetContrast = false;
    lcdContrast = v & 0x3F;
    return;
  }
  // coluna além da 131 (o jogo pode mandar até 255): o controlador ignora a gravação
  if (lcdCol <= 131) gdram[lcdPage][lcdSegRev ? 131 - lcdCol : lcdCol] = v;
  if (lcdCol < 131) lcdCol++;
  lcdDummy = true;
  lcdWritten = true;
}

uint8_t PM::lcdReadData() {
  if (lcdSetContrast) {
    lcdSetContrast = false;
    lcdContrast = 0x3F;
    lcdReadLatch = 0;
    return 0;
  }
  // como no PokeMini: depois de mudar o endereço (ou de cada leitura fora do modo RMW) vem uma leitura "falsa"
  if (!lcdDummy) {
    lcdReadLatch = lcdCol <= 131 ? gdram[lcdPage][lcdSegRev ? 131 - lcdCol : lcdCol] : 0;
    if (lcdPage >= 8) lcdReadLatch &= 1;
    if (!lcdRmw) {
      if (lcdCol < 131) lcdCol++;
      lcdDummy = true;
    }
  } else {
    lcdDummy = false;
  }
  return lcdReadLatch;
}

// fim de um quadro (≈72 Hz): a imagem do LCD vira pixels; a tela mostra a média das duas últimas imagens
void PM::lcdRefresh(bool prcFrame) {
  if (prcFrame || lcdWritten) std::memcpy(lcdPrev, lcdCur, sizeof lcdCur);
  lcdWritten = false;
  for (int y = 0; y < 64; y++) {
    int yp = (y + lcdStart) & 63;
    if (lcdRowRev) yp = 63 - yp;
    const uint8_t* rowp = gdram[yp >> 3];
    const int sh = yp & 7;
    for (int x = 0; x < 96; x++) {
      uint8_t p = lcdOn ? uint8_t(((rowp[x] >> sh) & 1) ^ (lcdInvert ? 1 : 0)) : 0;
      if (lcdOn && lcdAllOn) p = 1;
      lcdCur[y * 96 + x] = p;
    }
  }
  // contraste: intensidade (0..255) do pixel apagado e do aceso
  int p0, p1;
  if (lcdMaxContrast) {
    p0 = 240; p1 = 255;
  } else {
    const int c = lcdContrast & 0x3F;
    p0 = c <= 27 ? 0 : std::min(240, (c - 27) * (c - 27) * 240 / 784);
    p1 = c >= 27 ? 255 : std::max(4, int(255.0 * std::pow(std::max(0, c - 3) / 24.0, 1.4)));
  }
  uint32_t pal[3];
  for (int l = 0; l < 3; l++) {
    const int s = (p0 * (2 - l) + p1 * l) / 2;
    const int r = LIGHT_R + (DARK_R - LIGHT_R) * s / 255;
    const int g = LIGHT_G + (DARK_G - LIGHT_G) * s / 255;
    const int b = LIGHT_B + (DARK_B - LIGHT_B) * s / 255;
    pal[l] = 0xFF000000u | uint32_t(b << 16) | uint32_t(g << 8) | uint32_t(r);
  }
  for (int k = 0; k < SCREEN_W * SCREEN_H; k++) fb[k] = pal[lcdPrev[k] + lcdCur[k]];
}

// ------------------------------------------------------------------ save state
void PM::syncState(StateIO& s) {
  char magic[4];
  std::memcpy(magic, STATE_MAGIC, 4);
  s.raw(magic, 4);
  uint32_t romSize = uint32_t(rom.size());
  s.sync(romSize);
  if (s.reading() && (std::memcmp(magic, STATE_MAGIC, 4) != 0 || romSize != rom.size()))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(s);
  s.raw(ram, sizeof ram);
  s.raw(io, sizeof io);
  s.bytes(eeprom);
  s.sync(cycles); s.sync(frameStart); s.sync(stallUntil); s.sync(timerSynced); s.sync(miscSynced);
  for (Timer& t : tm) {
    for (uint8_t* p : {&t.scale, &t.osc, &t.ctrlL, &t.ctrlH, &t.preL, &t.preH, &t.pvtL, &t.pvtH, &t.cntL, &t.cntH})
      s.sync(*p);
  }
  s.sync(pivot3); s.sync(t256); s.sync(seconds); s.sync(keys); s.sync(shockPrev);
  s.sync(prcState); s.sync(prcRateMatch); s.sync(mapPX); s.sync(mapPY);
  s.raw(gdram, sizeof gdram);
  for (uint8_t* p : {&lcdCol, &lcdPage, &lcdStart, &lcdContrast, &lcdRmwCol, &lcdReadLatch}) s.sync(*p);
  for (bool* p : {&lcdSetContrast, &lcdSegRev, &lcdMaxContrast, &lcdAllOn, &lcdInvert, &lcdOn, &lcdRowRev, &lcdRmw,
                  &lcdDummy, &lcdWritten})
    s.sync(*p);
  s.raw(lcdPrev, sizeof lcdPrev);
  s.raw(lcdCur, sizeof lcdCur);
  for (uint8_t* p : {&i2cLast, &i2cState, &i2cBit, &i2cByte, &i2cPhase, &i2cOut}) s.sync(*p);
  s.sync(i2cAddr);
  s.sync(nextSample); s.sync(accHigh); s.sync(accTotal); s.sync(hpCap);
  if (s.reading()) {
    // limites: um estado danificado não pode levar a acesso fora da memória nem a laços sem fim
    clampTo<uint64_t>(cycles, 0, uint64_t(1) << 50);
    if (frameStart > cycles || cycles - frameStart >= uint64_t(FRAME_CYCLES)) { frameStart = cycles; prcState = 0; }
    if (prcState > 2) prcState = 0;
    if (stallUntil > frameStart + FRAME_CYCLES) stallUntil = frameStart + FRAME_CYCLES;
    if (timerSynced > cycles || cycles - timerSynced > uint64_t(FRAME_CYCLES)) timerSynced = cycles;
    if (miscSynced > cycles || cycles - miscSynced > uint64_t(FRAME_CYCLES)) miscSynced = cycles;
    seconds &= 0xFFFFFF;
    if (pivot3 != ((tm[2].pvtH << 8) | tm[2].pvtL)) pivot3 = uint16_t((tm[2].pvtH << 8) | tm[2].pvtL);
    for (Timer& t : tm) { t.osc &= 3; t.ctrlL &= 0x8D; t.ctrlH &= 0x0D; }
    if (mapPX > 127) mapPX = 0;
    if (mapPY > 127) mapPY = 0;
    if (lcdPage > 8) lcdPage = 8;
    lcdStart &= 63; lcdContrast &= 63;
    for (int k = 0; k < SCREEN_W * SCREEN_H; k++) { lcdPrev[k] &= 1; lcdCur[k] &= 1; }
    if (i2cState > 3) i2cState = 0;
    if (i2cBit > 9) i2cBit = 0;
    if (i2cPhase > 4) i2cPhase = 0;
    i2cOut &= 1;
    i2cAddr &= 0x1FFF;
    finiteOr(nextSample, double(cycles));
    if (nextSample < double(cycles) - 4 * cyclesPerSample || nextSample > double(cycles) + 4 * cyclesPerSample)
      nextSample = double(cycles);
    finiteOr(accHigh, 0); finiteOr(accTotal, 0); finiteOr(hpCap, 0);
    if (accHigh < 0 || accTotal < 0 || accHigh > accTotal) accHigh = accTotal = 0;
    clampTo(hpCap, -4.0, 4.0);
    eepromDirty = true;
  }
}

std::vector<uint8_t> PM::saveState() {
  StateIO s;
  syncState(s);
  return s.data();
}

void PM::loadState(const std::vector<uint8_t>& data) {
  const std::vector<uint8_t> backup = saveState();
  const bool dirty = eepromDirty;
  try {
    StateIO s(data);
    syncState(s);
  } catch (...) {
    StateIO s(backup);
    syncState(s);
    eepromDirty = dirty;
    throw;
  }
  wpos = rpos = 0;
  lcdRefresh(false);
}

// ------------------------------------------------------------------ adaptador
PMCore::PMCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) {
  if (rom.size() < 0x2200) throw std::runtime_error("ROM de Pokémon Mini inválida (pequena demais).");
  if (rom.size() > 0x200000) throw std::runtime_error("ROM de Pokémon Mini grande demais (máximo 2 MB).");
  m_ = std::make_unique<PM>(rom, indigo::firmware("pm"));
  if (save) m_->importEeprom(*save);
}

std::string PMCore::note() const {
  if (m_->usingFreeBios) return "Usando a BIOS livre (FreeBIOS). Para a abertura original, forneça o bios.min.";
  return "";
}

}  // namespace indigo::pm
