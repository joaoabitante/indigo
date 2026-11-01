#include "ws.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

namespace indigo::ws {

namespace {
const char STATE_MAGIC[4] = {'I', 'W', 'S', '1'};
constexpr int LINE_CYCLES = 256;

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
uint8_t bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
}  // namespace

// ------------------------------------------------------------ detecção
// o formato não tem assinatura: vale a extensão, conferindo que o rodapé começa com um JMP FAR (EAh)
bool isWsRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (!(endsWith(n, ".ws") || endsWith(n, ".wsc") || endsWith(n, ".pc2"))) return false;
  return rom.size() >= 0x200 && rom.size() <= (16u << 20);
}

// ------------------------------------------------------------ EEPROM
void Eeprom::init(size_t bytes, bool isInternal, bool color) {
  data.assign(bytes, isInternal ? 0x00 : 0xFF);
  internal = isInternal;
  internalColor = color;
  // endereço em palavras de 16 bits (93C56 e 93C76 têm um bit de endereço sobrando)
  addrBits = bytes <= 128 ? 6 : bytes <= 512 ? 8 : 10;
  rdData = wrData = command = 0;
  control = 0;
  readDone = false; writable = true; protect = false; dirty = false;
}

uint8_t Eeprom::read(int port, bool) const {
  if (data.empty()) return 0xFF;
  switch (port) {
    case 0: return static_cast<uint8_t>(rdData);
    case 1: return static_cast<uint8_t>(rdData >> 8);
    case 2: return static_cast<uint8_t>(command);
    case 3: return static_cast<uint8_t>(command >> 8);
    default: return static_cast<uint8_t>((readDone ? 1 : 0) | 2 | (internal && protect ? 0x80 : 0));
  }
}

void Eeprom::write(int port, uint8_t v, bool colorMode) {
  if (data.empty()) return;
  switch (port) {
    case 0: wrData = static_cast<uint16_t>((wrData & 0xFF00) | v); break;
    case 1: wrData = static_cast<uint16_t>((wrData & 0x00FF) | (v << 8)); break;
    case 2: command = static_cast<uint16_t>((command & 0xFF00) | v); break;
    case 3: command = static_cast<uint16_t>((command & 0x00FF) | (v << 8)); break;
    default:
      control = v >> 4;
      if (control == 8) protect = true;
      else if (control == 1 || control == 2 || control == 4) execute(colorMode);
      control = 0;
      break;
  }
}

bool Eeprom::canWrite(uint16_t cmd) const {
  // a EEPROM interna protegida só aceita escrita na área do jogo (abaixo de 30h)
  if (!internal || !protect) return true;
  if (!internalColor) return !(cmd & 0xC0) || (cmd & 0x3F) < 0x30;
  return !(cmd & 0xC00) || (cmd & 0x3FF) < 0x30;
}

void Eeprom::execute(bool colorMode) {
  uint16_t cmd = command;
  // Color em modo monocromático: a 93C86 interna responde a comandos no formato da 93C46
  if (internal && internalColor && !colorMode) cmd = (cmd & 0xC0) ? static_cast<uint16_t>(((cmd & 0xFFC0) << 4) | (cmd & 0x3F)) : static_cast<uint16_t>(cmd << 4);
  int top = 15;
  while (top >= 0 && !((cmd >> top) & 1)) top--;
  if (top < 2 + addrBits) return;
  const int op = (cmd >> (top - 2)) & 3;
  const uint32_t addr = (cmd >> (top - 2 - addrBits)) & ((1u << addrBits) - 1);
  const uint32_t byteAddr = (addr << 1) & (data.size() - 1);
  switch (op) {
    case 2:  // READ
      rdData = static_cast<uint16_t>(data[byteAddr] | (data[byteAddr + 1] << 8));
      readDone = true;
      break;
    case 1:  // WRITE
      if (!writable || !canWrite(cmd)) break;
      data[byteAddr] = static_cast<uint8_t>(wrData);
      data[byteAddr + 1] = static_cast<uint8_t>(wrData >> 8);
      dirty = true;
      break;
    case 3:  // ERASE
      if (!writable || !canWrite(cmd)) break;
      data[byteAddr] = data[byteAddr + 1] = 0xFF;
      dirty = true;
      break;
    default:
      switch (addr >> (addrBits - 2)) {
        case 0: writable = false; break;  // EWDS
        case 3: writable = true; break;   // EWEN
        case 1:                           // WRAL
          if (!writable || (internal && protect)) break;
          for (size_t i = 0; i + 1 < data.size(); i += 2) { data[i] = static_cast<uint8_t>(wrData); data[i + 1] = static_cast<uint8_t>(wrData >> 8); }
          dirty = true;
          break;
        default:                          // ERAL
          if (!writable || (internal && protect)) break;
          std::fill(data.begin(), data.end(), 0xFF);
          dirty = true;
          break;
      }
  }
}

void Eeprom::sync(StateIO& io) {
  io.bytes(data);
  io.sync(rdData); io.sync(wrData); io.sync(command); io.sync(control);
  io.sync(readDone); io.sync(writable); io.sync(protect);
}

// ------------------------------------------------------------ relógio (Seiko S-3511 do mapeador 2003)
void Rtc::power() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::memset(reg, 0, sizeof reg);
  reg[0] = bcd(tm.tm_year % 100); reg[1] = bcd(tm.tm_mon + 1); reg[2] = bcd(tm.tm_mday);
  reg[3] = static_cast<uint8_t>(tm.tm_wday); reg[4] = bcd(tm.tm_hour); reg[5] = bcd(tm.tm_min);
  reg[6] = bcd(std::min(tm.tm_sec, 59));
  reg[7] = 0x40;  // relógio de 24 horas
  command = 0; index = 0; fetched = 0xFF; active = false; ready = false;
  secondClock = 0;
}

void Rtc::tickSecond() {
  auto inc = [](uint8_t& v) -> uint8_t { v = (v & 0x0F) >= 9 ? static_cast<uint8_t>((v & 0xF0) + 0x10) : static_cast<uint8_t>(v + 1); return v; };
  if (inc(reg[6]) < 0x60) return;
  reg[6] = 0;
  if (inc(reg[5]) < 0x60) return;
  reg[5] = 0;
  if (reg[7] & 0x40) {
    if ((inc(reg[4]) & 0x7F) < 0x24) return;
    reg[4] &= 0xC0;
  } else {
    if ((inc(reg[4]) & 0x7F) < 0x12) return;
    reg[4] = static_cast<uint8_t>((reg[4] & 0x40) | ((reg[4] & 0x80) ^ 0x80));
  }
  reg[3] = static_cast<uint8_t>((reg[3] + 1) % 7);
  uint8_t days[12] = {0x31, 0x28, 0x31, 0x30, 0x31, 0x30, 0x31, 0x31, 0x30, 0x31, 0x30, 0x31};
  if (!(reg[0] & 3) && !((reg[0] >> 4) & 1)) days[1] = 0x29;  // ano BCD múltiplo de 4
  const int m = ((reg[1] >> 4) * 10 + (reg[1] & 15)) - 1;
  if (inc(reg[2]) <= days[m >= 0 && m < 12 ? m : 0]) return;
  reg[2] = 1;
  if (inc(reg[1]) <= 0x12) return;
  reg[1] = 1;
  inc(reg[0]);
  if (reg[0] >= 0xA0) reg[0] = 0;
}

void Rtc::controlWrite(uint8_t v) {
  command = v & 15;
  active = (v & 0x10) != 0;
  if (active) {
    if ((command & 0x0E) == 0) {  // RESET
      std::memset(reg, 0, sizeof reg);
      reg[1] = reg[2] = 1;
      active = false;
    }
    index = 0;
  }
  ready = true;
  if (active && (command & 1)) fetch();
  else if (active) dataWrite(fetched);
}

void Rtc::fetch() {
  uint8_t d = 0;
  switch (command & 0x0E) {
    case 0x02: d = reg[7]; reg[7] &= 0x7F; index = 1; active = false; break;
    case 0x04: d = index < 7 ? reg[index] : 0; if (++index >= 7) active = false; break;
    case 0x06: d = index < 3 ? reg[4 + index] : 0; if (++index >= 3) active = false; break;
    case 0x08: d = index < 2 ? reg[16 + index] : 0; if (++index >= 2) active = false; break;
    default: d = 0xFF; if (++index >= 2) active = false; break;
  }
  ready = true;
  fetched = d;
}

uint8_t Rtc::dataRead() {
  const uint8_t d = fetched;
  if (!active) ready = false;
  if (active && (command & 1)) fetch();
  return d;
}

void Rtc::dataWrite(uint8_t v) {
  fetched = v;
  if (!active) { ready = false; return; }
  if (command & 1) return;
  switch (command & 0x0E) {
    case 0x02: reg[7] = static_cast<uint8_t>((reg[7] & 0x95) | (v & 0x6A)); active = false; break;
    case 0x04:
      if (index < 7) reg[index] = index == 6 ? static_cast<uint8_t>(v & 0x7F) : v;
      if (++index >= 7) active = false;
      break;
    case 0x06:
      if (index < 3) reg[4 + index] = index == 2 ? static_cast<uint8_t>(v & 0x7F) : v;
      if (++index >= 3) active = false;
      break;
    case 0x08: if (index < 2) reg[16 + index] = v; if (++index >= 2) active = false; break;
    case 0x0A: if (++index >= 2) active = false; break;
    default: active = false; break;
  }
}

void Rtc::sync(StateIO& io) {
  io.raw(reg, sizeof reg);
  io.sync(command); io.sync(index); io.sync(fetched); io.sync(active); io.sync(ready); io.sync(secondClock);
  if (io.reading()) { command &= 15; clampTo<int64_t>(secondClock, 0, 3072000); }
}

// ------------------------------------------------------------ console
WS::WS(const std::vector<uint8_t>& data, const std::vector<uint8_t>* save) : cpu(*this), iram_(0x10000) {
  if (data.size() < 16) throw std::runtime_error("ROM de WonderSwan inválida.");
  // a ROM fica no fim do espaço (o rodapé é sempre o fim do último banco)
  size_t size = 0x10000;
  while (size < data.size()) size <<= 1;
  if (size > (16u << 20)) throw std::runtime_error("ROM de WonderSwan grande demais.");
  rom_.assign(size, 0xFF);
  std::copy(data.begin(), data.end(), rom_.begin() + static_cast<long>(size - data.size()));
  romMask_ = static_cast<uint32_t>(size - 1);
  const uint8_t* f = &rom_[size - 16];
  colorHw = (f[7] & 1) != 0;
  footerVertical_ = (f[12] & 1) != 0;
  romWord_ = !(f[12] & 2);
  hasRtc_ = f[13] == 1;
  switch (f[11]) {
    case 0x01: sram_.assign(0x2000, 0xFF); break;
    case 0x02: sram_.assign(0x8000, 0xFF); break;
    case 0x03: sram_.assign(0x20000, 0xFF); break;
    case 0x04: sram_.assign(0x40000, 0xFF); break;
    case 0x05: sram_.assign(0x80000, 0xFF); break;
    case 0x10: cartEeprom_.init(128, false, false); break;
    case 0x20: cartEeprom_.init(2048, false, false); break;
    case 0x50: cartEeprom_.init(1024, false, false); break;
    default: break;
  }
  if (hasRtc_ && sram_.empty()) sram_.assign(0x8000, 0xFF);
  cpu.aswan = !colorHw;
  intEeprom_.init(colorHw ? 2048 : 128, true, colorHw);
  // conteúdo de fábrica da EEPROM interna (nome do modelo e ajustes, como o ares grava)
  auto& e = intEeprom_.data;
  const char* name = colorHw ? "WONDERSWANCOLOR" : "WONDERSWAN";
  for (int i = 0; name[i]; i++) e[0x60 + i] = static_cast<uint8_t>(name[i] - 'A' + 0x0B);
  e[0x76] = 0x01; e[0x77] = colorHw ? 0x01 : 0x00; e[0x78] = colorHw ? 0x27 : 0x24; e[0x7C] = 0x01;
  if (colorHw) { e[0x80] = 0x01; e[0x81] = 0x01; e[0x82] = 0x27; e[0x83] = 0x03; }
  reset();
  if (save) importSave(*save);
}

void WS::reset() {
  cpu.power();
  // estado deixado pela ROM de boot
  cpu.sreg[V30MZ::SS] = 0;
  cpu.r[V30MZ::SP] = 0x2000;
  cpu.sreg[V30MZ::DS] = colorHw ? 0xFE00 : 0xFF00;
  cpu.flush();
  std::fill(iram_.begin(), iram_.end(), 0);
  hwFlags_ = static_cast<uint8_t>((romWord_ ? 4 : 0) | ((rom_[rom_.size() - 4] & 4) ? 0 : 8));
  dispMode_ = 0;
  intBase_ = intEnable_ = intStatus_ = keyMatrix_ = 0;
  serialCtrl_ = 0; nmiCtrl_ = 0; lastKeys_ = 0;
  gdmaSrc_ = 0; gdmaDst_ = gdmaLen_ = 0; gdmaCtrl_ = 0;
  gpoEn_ = gpoData_ = 0;
  bankRom0_ = bankRom1_ = bankRom2_ = bankSram_ = 0xFF;
  dispCtrl_ = backColor_ = line_ = lineCmp_ = sprBase_ = sprFirst_ = sprCount_ = 0;
  mapBase_ = scr1x_ = scr1y_ = scr2x_ = scr2y_ = 0;
  lcdCtrl_ = 1; lcdIcon_ = 0; vtotal_ = 158; vsync_ = 155;
  win2_ = Window{}; winSpr_ = Window{};
  for (int i = 0; i < 8; i++) pool_[i] = static_cast<uint8_t>(i * 2);
  std::memset(monoPal_, 0, sizeof monoPal_);
  tmrCtrl_ = 0; htFreq_ = htCnt_ = vtFreq_ = vtCnt_ = 0;
  std::memset(oam_, 0, sizeof oam_);
  oamCount_ = 0;
  for (auto& c : ch_) c = Channel{};
  sndCtrl_ = sndOut_ = waveBase_ = voiceCtrl_ = noiseCtrl_ = sweepTime_ = sndDbg_ = 0;
  sweepValue_ = 0; sweepCounter_ = 0; noiseLfsr_ = 0; noiseOut_ = false;
  masterVol_ = colorHw ? 3 : 2;
  apuClock_ = 0; sweepClock_ = 0; outL_ = outR_ = 0;
  sdmaSrc_ = sdmaLen_ = sdmaSrcReload_ = sdmaLenReload_ = 0; sdmaCtrl_ = 0; sdmaClock_ = 0;
  hyperCtrl_ = hyperChan_ = 0; hvLeft_ = hvRight_ = 0;
  hvLeftChanged_ = hvRightChanged_ = hvChannel_ = false; hvClock_ = 0; hvOutL_ = hvOutR_ = 0;
  aw_ = ar_ = 0; outPhase_ = 0; hpL_ = hpR_ = 0;
  frameStart_ = 0;
  frame = 0;
  if (hasRtc_) rtc_.power();
  vertical = footerVertical_;
}

std::string WS::note() const {
  std::string s;
  if (footerVertical_) s = "Jogo para o WonderSwan em pé: a imagem foi girada; o direcional vira o Y-pad e A/B/X/Y o X-pad.";
  if (hasRtc_) s += std::string(s.empty() ? "" : " ") + "O relógio do cartucho começa com a hora do aparelho.";
  return s;
}

// ------------------------------------------------------------ memória
uint8_t WS::memRead(uint32_t a) {
  a &= 0xFFFFF;
  switch (a >> 16) {
    case 0:
      if (a >= 0x4000 && !colorMode()) return 0x90;
      return iram_[a];
    case 1:
      if (sram_.empty()) return 0xFF;
      return sram_[((static_cast<uint32_t>(bankSram_) << 16) | (a & 0xFFFF)) % sram_.size()];
    case 2: return rom_[((static_cast<uint32_t>(bankRom0_) << 16) | (a & 0xFFFF)) & romMask_];
    case 3: return rom_[((static_cast<uint32_t>(bankRom1_) << 16) | (a & 0xFFFF)) & romMask_];
    default: return rom_[((static_cast<uint32_t>(bankRom2_) << 20) | a) & romMask_];
  }
}

void WS::memWrite(uint32_t a, uint8_t v) {
  a &= 0xFFFFF;
  if (a < 0x10000) {
    if (a >= 0x4000 && !colorMode()) return;
    iram_[a] = v;
    return;
  }
  if (a < 0x20000 && !sram_.empty()) {
    uint8_t& m = sram_[((static_cast<uint32_t>(bankSram_) << 16) | (a & 0xFFFF)) % sram_.size()];
    if (m != v) { m = v; dirty_ = true; }
  }
}

int WS::memTiming(uint32_t a) {
  switch ((a >> 16) & 15) {
    case 0: return 1 | 16;
    case 1: return ((dispMode_ & 2) || !colorHw) ? 2 : 1;
    default: return ((hwFlags_ & 8) ? 2 : 1) | ((hwFlags_ & 4) ? 16 : 0);
  }
}

int WS::ioTiming(uint16_t p) {
  if ((p & 0xFF) >= 0xC0) return ((dispMode_ & 8) || !colorHw) ? 2 : 1;
  return 1 | 16;
}

// ------------------------------------------------------------ teclado
// linhas da matriz: bit 4 = Y1-Y4, bit 5 = X1-X4, bit 6 = Start/A/B
uint8_t WS::keypadRead() const {
  const uint32_t k = keys_;
  auto b = [&](uint32_t key) { return (k & key) ? 1 : 0; };
  int y[4], x[4];
  if (!vertical) {
    y[0] = b(KEY_X); y[1] = b(KEY_R); y[2] = b(KEY_L); y[3] = b(KEY_Y);
    x[0] = b(KEY_UP); x[1] = b(KEY_RIGHT); x[2] = b(KEY_DOWN); x[3] = b(KEY_LEFT);
  } else {
    y[1] = b(KEY_UP); y[2] = b(KEY_RIGHT); y[3] = b(KEY_DOWN); y[0] = b(KEY_LEFT);
    x[1] = b(KEY_X); x[2] = b(KEY_A); x[3] = b(KEY_B); x[0] = b(KEY_Y);
  }
  const int btnA = vertical ? b(KEY_R) : b(KEY_A), btnB = vertical ? b(KEY_L) : b(KEY_B);
  uint8_t d = 0;
  if (keyMatrix_ & 0x10) d |= static_cast<uint8_t>(y[0] | (y[1] << 1) | (y[2] << 2) | (y[3] << 3));
  if (keyMatrix_ & 0x20) d |= static_cast<uint8_t>(x[0] | (x[1] << 1) | (x[2] << 2) | (x[3] << 3));
  if (keyMatrix_ & 0x40) d |= static_cast<uint8_t>((b(KEY_START) << 1) | (btnA << 2) | (btnB << 3));
  return d;
}

// ------------------------------------------------------------ portas
uint8_t WS::ioRead(uint16_t port) {
  if ((port & 0x100) || (port >= 0x100 && (port & 0xFF) >= 0xB8)) return colorHw ? 0x00 : 0x90;
  const int p = port & 0xFF;
  const bool cm = colorMode();
  if (p >= 0x80 && p <= 0x9E) apuRun();
  if (!colorHw && p >= 0x40 && p < 0x80) return 0x90;  // portas que só existem no Color
  switch (p) {
    case 0x00: return dispCtrl_ & 0x3F;
    case 0x1B: case 0xB6: return 0;
    case 0xB7: return nmiCtrl_;
    case 0x01: return cm ? backColor_ : backColor_ & 7;
    case 0x02: return line_;
    case 0x03: return lineCmp_;
    case 0x04: return sprBase_ & (cm ? 0x3F : 0x1F);
    case 0x05: return sprFirst_;
    case 0x06: return sprCount_;
    case 0x07: return mapBase_ & (cm ? 0xFF : 0x77);
    case 0x08: return win2_.x0;
    case 0x09: return win2_.y0;
    case 0x0A: return win2_.x1;
    case 0x0B: return win2_.y1;
    case 0x0C: return winSpr_.x0;
    case 0x0D: return winSpr_.y0;
    case 0x0E: return winSpr_.x1;
    case 0x0F: return winSpr_.y1;
    case 0x10: return scr1x_;
    case 0x11: return scr1y_;
    case 0x12: return scr2x_;
    case 0x13: return scr2y_;
    case 0x14: return lcdCtrl_;
    case 0x15: return lcdIcon_;
    case 0x16: return vtotal_;
    case 0x17: return vsync_;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
      return static_cast<uint8_t>(pool_[(p & 3) * 2] | (pool_[(p & 3) * 2 + 1] << 4));
    case 0x40: case 0x41: case 0x42: return cm ? static_cast<uint8_t>(gdmaSrc_ >> ((p - 0x40) * 8)) : 0;
    case 0x44: case 0x45: return cm ? static_cast<uint8_t>(gdmaDst_ >> ((p - 0x44) * 8)) : 0;
    case 0x46: case 0x47: return cm ? static_cast<uint8_t>(gdmaLen_ >> ((p - 0x46) * 8)) : 0;
    case 0x48: return cm ? gdmaCtrl_ : 0;
    case 0x4A: case 0x4B: case 0x4C: return cm ? static_cast<uint8_t>(sdmaSrc_ >> ((p - 0x4A) * 8)) : 0;
    case 0x4E: case 0x4F: case 0x50: return cm ? static_cast<uint8_t>(sdmaLen_ >> ((p - 0x4E) * 8)) : 0;
    case 0x52: return cm ? sdmaCtrl_ : 0;
    case 0x60: return dispMode_ & 0xEB;
    case 0x62: return 0;  // bit 7 = SwanCrystal
    case 0x6A: return colorHw ? hyperCtrl_ : 0;
    case 0x6B: return colorHw ? static_cast<uint8_t>(hyperChan_ & 0x6F) : 0;
    case 0x80: case 0x82: case 0x84: case 0x86: return static_cast<uint8_t>(ch_[(p - 0x80) >> 1].pitch);
    case 0x81: case 0x83: case 0x85: case 0x87: return static_cast<uint8_t>(ch_[(p - 0x80) >> 1].pitch >> 8);
    case 0x88: case 0x89: case 0x8A: case 0x8B: return static_cast<uint8_t>(ch_[p - 0x88].volR | (ch_[p - 0x88].volL << 4));
    case 0x8C: return static_cast<uint8_t>(sweepValue_);
    case 0x8D: return sweepTime_;
    case 0x8E: return noiseCtrl_ & 0x17;
    case 0x8F: return waveBase_;
    case 0x90: return sndCtrl_ & 0xEF;
    case 0x91: return static_cast<uint8_t>((sndOut_ & 0x0F) | 0x80);  // fone sempre ligado (saída estéreo)
    case 0x92: return static_cast<uint8_t>(noiseLfsr_);
    case 0x93: return static_cast<uint8_t>(noiseLfsr_ >> 8);
    case 0x94: return voiceCtrl_;
    case 0x95: return sndDbg_;
    case 0x96: return static_cast<uint8_t>(outR_);
    case 0x97: return static_cast<uint8_t>(outR_ >> 8);
    case 0x98: return static_cast<uint8_t>(outL_);
    case 0x99: return static_cast<uint8_t>(outL_ >> 8);
    case 0x9A: return static_cast<uint8_t>(outL_ + outR_);
    case 0x9B: return static_cast<uint8_t>((outL_ + outR_) >> 8);
    case 0x9E: if (colorHw) return masterVol_; break;
    case 0xA0: return static_cast<uint8_t>(0x81 | (colorHw ? 2 : 0) | (hwFlags_ & 0x0C));
    case 0xA2: case 0xA3: return tmrCtrl_;
    case 0xA4: return static_cast<uint8_t>(htFreq_);
    case 0xA5: return static_cast<uint8_t>(htFreq_ >> 8);
    case 0xA6: return static_cast<uint8_t>(vtFreq_);
    case 0xA7: return static_cast<uint8_t>(vtFreq_ >> 8);
    case 0xA8: return static_cast<uint8_t>(htCnt_);
    case 0xA9: return static_cast<uint8_t>(htCnt_ >> 8);
    case 0xAA: return static_cast<uint8_t>(vtCnt_);
    case 0xAB: return static_cast<uint8_t>(vtCnt_ >> 8);
    case 0xB0: return intBase_;
    case 0xB1: return 0;  // serial sem cabo: nada recebido
    case 0xB2: return intEnable_;
    case 0xB3: return static_cast<uint8_t>((serialCtrl_ & 0xC0) | ((serialCtrl_ & 0x80) ? 4 : 0));
    case 0xB4: return static_cast<uint8_t>(intStatus_ | ((serialCtrl_ & 0x80) ? (intEnable_ & 1) : 0));
    case 0xB5: return static_cast<uint8_t>(keypadRead() | keyMatrix_);
    case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: return intEeprom_.read(p - 0xBA, cm);
    case 0xC0: case 0xCF: return static_cast<uint8_t>(bankRom2_);
    case 0xC1: case 0xD0: return static_cast<uint8_t>(bankSram_);
    case 0xD1: return static_cast<uint8_t>(bankSram_ >> 8);
    case 0xC2: case 0xD2: return static_cast<uint8_t>(bankRom0_);
    case 0xD3: return static_cast<uint8_t>(bankRom0_ >> 8);
    case 0xC3: case 0xD4: return static_cast<uint8_t>(bankRom1_);
    case 0xD5: return static_cast<uint8_t>(bankRom1_ >> 8);
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xC8: return cartEeprom_.read(p - 0xC4, cm);
    case 0xCA: return hasRtc_ ? rtc_.controlRead() : 0;
    case 0xCB: return hasRtc_ ? rtc_.dataRead() : 0xFF;
    case 0xCC: return gpoEn_;
    case 0xCD: return gpoData_;
    default:
      if (p >= 0x20 && p < 0x40) return static_cast<uint8_t>(monoPal_[(p - 0x20) >> 1][(p & 1) * 2] | (monoPal_[(p - 0x20) >> 1][(p & 1) * 2 + 1] << 4));
      return colorHw ? 0x00 : 0x90;
  }
  return colorHw ? 0x00 : 0x90;
}

void WS::ioWrite(uint16_t port, uint8_t v) {
  if ((port & 0x100) || (port >= 0x100 && (port & 0xFF) >= 0xB8)) return;
  const int p = port & 0xFF;
  const bool cm = colorMode();
  if ((p >= 0x80 && p <= 0x9E) || (p >= 0x4A && p <= 0x6B)) apuRun();
  if (!colorHw && p >= 0x40 && p < 0x80) return;
  switch (p) {
    case 0x00: dispCtrl_ = v & 0x3F; return;
    case 0x01: backColor_ = v; return;
    case 0x03: lineCmp_ = v; return;
    case 0x04: sprBase_ = v & 0x3F; return;
    case 0x05: sprFirst_ = v & 0x7F; return;
    case 0x06: sprCount_ = v; return;
    case 0x07: mapBase_ = v; return;
    case 0x08: win2_.x0 = v; return;
    case 0x09: win2_.y0 = v; return;
    case 0x0A: win2_.x1 = v; return;
    case 0x0B: win2_.y1 = v; return;
    case 0x0C: winSpr_.x0 = v; return;
    case 0x0D: winSpr_.y0 = v; return;
    case 0x0E: winSpr_.x1 = v; return;
    case 0x0F: winSpr_.y1 = v; return;
    case 0x10: scr1x_ = v; return;
    case 0x11: scr1y_ = v; return;
    case 0x12: scr2x_ = v; return;
    case 0x13: scr2y_ = v; return;
    case 0x14: lcdCtrl_ = colorHw ? static_cast<uint8_t>(v & 0xF3) : static_cast<uint8_t>(v & 0xF1); return;
    case 0x15: lcdIcon_ = v & 0x3F; updateVertical(); return;
    case 0x16: vtotal_ = v; return;
    case 0x17: vsync_ = v; return;
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
      pool_[(p & 3) * 2] = v & 15;
      pool_[(p & 3) * 2 + 1] = v >> 4;
      return;
    case 0x40: case 0x41: case 0x42:
      if (!cm) return;
      gdmaSrc_ = (gdmaSrc_ & ~(0xFFu << ((p - 0x40) * 8))) | (static_cast<uint32_t>(v) << ((p - 0x40) * 8));
      gdmaSrc_ &= 0xFFFFE;
      return;
    case 0x44: case 0x45:
      if (!cm) return;
      gdmaDst_ = static_cast<uint16_t>(((gdmaDst_ & ~(0xFF << ((p - 0x44) * 8))) | (v << ((p - 0x44) * 8))) & 0xFFFE);
      return;
    case 0x46: case 0x47:
      if (!cm) return;
      gdmaLen_ = static_cast<uint16_t>(((gdmaLen_ & ~(0xFF << ((p - 0x46) * 8))) | (v << ((p - 0x46) * 8))) & 0xFFFE);
      return;
    case 0x48: if (!cm) return; gdmaCtrl_ = v & 0xC0; if (v & 0x80) gdma(); return;
    case 0x4A: case 0x4B: case 0x4C: {
      if (!cm) return;
      const int sh = (p - 0x4A) * 8;
      sdmaSrc_ = ((sdmaSrc_ & ~(0xFFu << sh)) | (static_cast<uint32_t>(v) << sh)) & 0xFFFFF;
      sdmaSrcReload_ = ((sdmaSrcReload_ & ~(0xFFu << sh)) | (static_cast<uint32_t>(v) << sh)) & 0xFFFFF;
      return;
    }
    case 0x4E: case 0x4F: case 0x50: {
      if (!cm) return;
      const int sh = (p - 0x4E) * 8;
      sdmaLen_ = ((sdmaLen_ & ~(0xFFu << sh)) | (static_cast<uint32_t>(v) << sh)) & 0xFFFFF;
      sdmaLenReload_ = ((sdmaLenReload_ & ~(0xFFu << sh)) | (static_cast<uint32_t>(v) << sh)) & 0xFFFFF;
      return;
    }
    case 0x52: if (!cm) return; sdmaCtrl_ = v & 0xDF; if (!sdmaLen_) sdmaCtrl_ &= 0x7F; return;
    case 0x60: dispMode_ = v & 0xEB; return;
    case 0x64: case 0x65: if (colorHw) hvOutL_ = static_cast<int16_t>(p == 0x64 ? (hvOutL_ & 0xFF00) | v : (hvOutL_ & 0xFF) | (v << 8)); return;
    case 0x66: case 0x67: if (colorHw) hvOutR_ = static_cast<int16_t>(p == 0x66 ? (hvOutR_ & 0xFF00) | v : (hvOutR_ & 0xFF) | (v << 8)); return;
    case 0x69: if (colorHw) { hyperWrite(v); hvChannel_ = !hvChannel_; } return;
    case 0x6A: if (colorHw) hyperCtrl_ = v; return;
    case 0x6B: if (colorHw) { hyperChan_ = v & 0x6F; if (v & 0x10) hvChannel_ = false; } return;
    case 0x80: case 0x82: case 0x84: case 0x86: { auto& c = ch_[(p - 0x80) >> 1]; c.pitch = static_cast<uint16_t>((c.pitch & 0x700) | v); return; }
    case 0x81: case 0x83: case 0x85: case 0x87: { auto& c = ch_[(p - 0x80) >> 1]; c.pitch = static_cast<uint16_t>((c.pitch & 0xFF) | ((v & 7) << 8)); return; }
    case 0x88: case 0x89: case 0x8A: case 0x8B: ch_[p - 0x88].volR = v & 15; ch_[p - 0x88].volL = v >> 4; return;
    case 0x8C: sweepValue_ = static_cast<int8_t>(v); return;
    case 0x8D: sweepTime_ = v & 31; return;
    case 0x8E: noiseCtrl_ = v & 0x17; if (v & 8) noiseLfsr_ = 0; return;
    case 0x8F: waveBase_ = v; return;
    case 0x90: sndCtrl_ = v; return;
    case 0x91: sndOut_ = v & 0x0F; return;
    case 0x94: voiceCtrl_ = v & 0x0F; return;
    case 0x95: sndDbg_ = v; return;
    case 0x9E: if (colorHw) masterVol_ = v & 3; return;
    case 0xA0: hwFlags_ = static_cast<uint8_t>(v & 0x0C); return;  // bit 0 (trava da ROM de boot) sempre ligado
    case 0xA2: case 0xA3: tmrCtrl_ = v & 0x0F; return;
    case 0xA4: htFreq_ = static_cast<uint16_t>((htFreq_ & 0xFF00) | v); htCnt_ = htFreq_; return;
    case 0xA5: htFreq_ = static_cast<uint16_t>((htFreq_ & 0xFF) | (v << 8)); htCnt_ = htFreq_; return;
    case 0xA6: vtFreq_ = static_cast<uint16_t>((vtFreq_ & 0xFF00) | v); vtCnt_ = vtFreq_; return;
    case 0xA7: vtFreq_ = static_cast<uint16_t>((vtFreq_ & 0xFF) | (v << 8)); vtCnt_ = vtFreq_; return;
    case 0xB0: intBase_ = v & 0xF8; return;
    case 0xB2: intEnable_ = v; return;
    case 0xB3: serialCtrl_ = v & 0xC0; return;
    case 0xB5: keyMatrix_ = v & 0x70; return;
    case 0xB7: nmiCtrl_ = v & 0x10; return;
    case 0xB6: intStatus_ &= static_cast<uint8_t>(~v); return;
    case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: intEeprom_.write(p - 0xBA, v, cm); return;
    case 0xC0: case 0xCF: bankRom2_ = v; return;
    case 0xC1: case 0xD0: bankSram_ = static_cast<uint16_t>((bankSram_ & 0xFF00) | v); return;
    case 0xD1: bankSram_ = static_cast<uint16_t>((bankSram_ & 0xFF) | (v << 8)); return;
    case 0xC2: case 0xD2: bankRom0_ = static_cast<uint16_t>((bankRom0_ & 0xFF00) | v); return;
    case 0xD3: bankRom0_ = static_cast<uint16_t>((bankRom0_ & 0xFF) | (v << 8)); return;
    case 0xC3: case 0xD4: bankRom1_ = static_cast<uint16_t>((bankRom1_ & 0xFF00) | v); return;
    case 0xD5: bankRom1_ = static_cast<uint16_t>((bankRom1_ & 0xFF) | (v << 8)); return;
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: case 0xC8: cartEeprom_.write(p - 0xC4, v, cm); return;
    case 0xCA: if (hasRtc_) rtc_.controlWrite(v); return;
    case 0xCB: if (hasRtc_) rtc_.dataWrite(v); return;
    case 0xCC: gpoEn_ = v; return;
    case 0xCD: gpoData_ = v; return;
    default:
      if (p >= 0x20 && p < 0x40) {
        // paletas 4-7 e 12-15: a cor 0 não pode ser alterada
        auto& pal = monoPal_[(p - 0x20) >> 1];
        if ((p & 9) != 8) pal[(p & 1) * 2] = v & 7;
        pal[(p & 1) * 2 + 1] = (v >> 4) & 7;
      }
      return;
  }
}

void WS::updateVertical() {
  if (lcdIcon_ & 2) vertical = true;
  if (lcdIcon_ & 4) vertical = false;
}

// DMA geral (Color): copia palavras para a RAM interna; origem na SRAM cancela
void WS::gdma() {
  auto valid = [&](uint32_t a) { const int t = memTiming(a); return (t & 15) == 1 && (t & 16); };
  if (gdmaLen_ == 0 || !valid(gdmaSrc_)) { gdmaCtrl_ &= 0x7F; return; }
  cpu.cycles += 5;
  while (gdmaLen_) {
    cpu.cycles += 2;
    if (valid(gdmaSrc_)) {
      memWrite(gdmaDst_, memRead(gdmaSrc_));
      memWrite(static_cast<uint16_t>(gdmaDst_ + 1), memRead(gdmaSrc_ + 1));
    }
    const int d = (gdmaCtrl_ & 0x40) ? -2 : 2;
    gdmaSrc_ = (gdmaSrc_ + d) & 0xFFFFF;
    gdmaDst_ = static_cast<uint16_t>(gdmaDst_ + d);
    gdmaLen_ = static_cast<uint16_t>(gdmaLen_ - 2);
  }
  gdmaCtrl_ &= 0x7F;
}

// ------------------------------------------------------------ quadro
void WS::pollInterrupts() {
  if (!cpu.pollOk) return;
  const uint8_t level = (serialCtrl_ & 0x80) ? 1 : 0;
  intStatus_ |= static_cast<uint8_t>(level & intEnable_);
  if (!intStatus_) return;
  int id = 7;
  while (!((intStatus_ >> id) & 1)) id--;
  cpu.irq(static_cast<uint8_t>(intBase_ | id));
}

void WS::runCpu(int64_t until) {
  while (cpu.cycles < until) {
    pollInterrupts();
    if (cpu.halted) { cpu.cycles = until; break; }  // HLT sem interrupção pendente: dorme até a próxima linha
    cpu.step();
  }
}

// temporizador (WSHWTest): a contagem sempre acontece; o bit "ligado" só decide se o valor volta ao contador
void WS::timerStep(uint16_t& counter, uint16_t freq, int ctrl, int irq) {
  if (!counter) return;
  uint16_t next = static_cast<uint16_t>(counter - 1);
  if (!next) {
    raise(irq);
    if (ctrl & 2) next = freq;
  }
  if (ctrl & 1) counter = next;
}

void WS::runFrame() {
  // tecla nova apertada: interrupção de teclado
  const uint16_t pressed = static_cast<uint16_t>(keys_ & 0xFFF);
  if (pressed & ~lastKeys_) raise(1);
  lastKeys_ = pressed;
  const int lines = std::max(144, std::min(256, vtotal_ + 1));
  int64_t t = cpu.cycles;
  for (int v = 0; v < lines; v++) {
    line_ = static_cast<uint8_t>(v);
    if (v == lineCmp_) raise(4);
    timerStep(htCnt_, htFreq_, tmrCtrl_ & 3, 7);
    if (v < 144) renderLine(v);
    if (v == 144) {
      raise(6);
      timerStep(vtCnt_, vtFreq_, (tmrCtrl_ >> 2) & 3, 5);
      // os sprites do próximo quadro são lidos agora
      const uint32_t base = static_cast<uint32_t>(sprBase_ & (colorMode() ? 0x3F : 0x1F)) << 9;
      oamCount_ = std::min<int>(128, sprCount_);
      for (int i = 0; i < oamCount_; i++) {
        const uint32_t a = (base + ((sprFirst_ + i) & 127) * 4) & 0xFFFF;
        oam_[i] = iram_[a] | (iram_[(a + 1) & 0xFFFF] << 8) | (iram_[(a + 2) & 0xFFFF] << 16) | (static_cast<uint32_t>(iram_[(a + 3) & 0xFFFF]) << 24);
      }
    }
    t += LINE_CYCLES;
    runCpu(t);
  }
  apuRun();
  if (hasRtc_) {
    rtc_.secondClock += static_cast<int64_t>(lines) * LINE_CYCLES;
    while (rtc_.secondClock >= 3072000) { rtc_.secondClock -= 3072000; rtc_.tickSecond(); }
  }
  if (vertical)
    for (int y = 0; y < 144; y++)
      for (int x = 0; x < 224; x++) fbRot[(223 - x) * 144 + y] = fb[y * 224 + x];
  frame++;
}

// ------------------------------------------------------------ save
std::vector<uint8_t> WS::exportSave() {
  dirty_ = false; cartEeprom_.dirty = false; intEeprom_.dirty = false;
  std::vector<uint8_t> out = !sram_.empty() ? sram_ : cartEeprom_.data;
  out.insert(out.end(), intEeprom_.data.begin(), intEeprom_.data.end());  // ajustes do console (nome, volume)
  return out;
}

void WS::importSave(const std::vector<uint8_t>& b) {
  std::vector<uint8_t>& cart = !sram_.empty() ? sram_ : cartEeprom_.data;
  const size_t n = std::min(b.size(), cart.size());
  std::copy(b.begin(), b.begin() + static_cast<long>(n), cart.begin());
  if (b.size() == cart.size() + intEeprom_.data.size())
    std::copy(b.begin() + static_cast<long>(cart.size()), b.end(), intEeprom_.data.begin());
}

// ------------------------------------------------------------ save state
void WS::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  bool c = colorHw;
  uint32_t romSize = static_cast<uint32_t>(rom_.size());
  io.sync(c); io.sync(romSize);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || c != colorHw || romSize != rom_.size()))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  io.bytes(iram_); io.bytes(sram_);
  cartEeprom_.sync(io); intEeprom_.sync(io);
  if (hasRtc_) rtc_.sync(io);
  io.sync(bankRom0_); io.sync(bankRom1_); io.sync(bankRom2_); io.sync(bankSram_);
  io.sync(hwFlags_); io.sync(dispMode_); io.sync(intBase_); io.sync(intEnable_); io.sync(intStatus_);
  io.sync(keyMatrix_); io.sync(serialCtrl_); io.sync(nmiCtrl_); io.sync(lastKeys_);
  io.sync(gdmaSrc_); io.sync(gdmaDst_); io.sync(gdmaLen_); io.sync(gdmaCtrl_); io.sync(gpoEn_); io.sync(gpoData_);
  io.sync(dispCtrl_); io.sync(backColor_); io.sync(line_); io.sync(lineCmp_); io.sync(sprBase_); io.sync(sprFirst_);
  io.sync(sprCount_); io.sync(mapBase_); io.sync(scr1x_); io.sync(scr1y_); io.sync(scr2x_); io.sync(scr2y_);
  io.sync(lcdCtrl_); io.sync(lcdIcon_); io.sync(vtotal_); io.sync(vsync_); io.sync(win2_); io.sync(winSpr_);
  io.raw(pool_, sizeof pool_); io.raw(monoPal_, sizeof monoPal_);
  io.sync(tmrCtrl_); io.sync(htFreq_); io.sync(htCnt_); io.sync(vtFreq_); io.sync(vtCnt_);
  io.raw(oam_, sizeof oam_); io.sync(oamCount_);
  io.raw(ch_, sizeof ch_);
  io.sync(sndCtrl_); io.sync(sndOut_); io.sync(waveBase_); io.sync(voiceCtrl_); io.sync(noiseCtrl_); io.sync(sweepTime_);
  io.sync(sndDbg_); io.sync(sweepValue_); io.sync(sweepCounter_); io.sync(noiseLfsr_); io.sync(noiseOut_); io.sync(masterVol_);
  io.sync(apuClock_); io.sync(sweepClock_); io.sync(outL_); io.sync(outR_);
  io.sync(sdmaSrc_); io.sync(sdmaLen_); io.sync(sdmaSrcReload_); io.sync(sdmaLenReload_); io.sync(sdmaCtrl_); io.sync(sdmaClock_);
  io.sync(hyperCtrl_); io.sync(hyperChan_); io.sync(hvLeft_); io.sync(hvRight_); io.sync(hvLeftChanged_);
  io.sync(hvRightChanged_); io.sync(hvChannel_); io.sync(hvClock_); io.sync(hvOutL_); io.sync(hvOutR_);
  io.sync(outPhase_); io.sync(hpL_); io.sync(hpR_);
  io.sync(vertical); io.sync(frame);
  io.raw(fb, sizeof fb);
  if (io.reading()) {
    // só limites que contêm todos os valores válidos (salvar → carregar → salvar dá os mesmos bytes)
    clampTo(oamCount_, 0, 128);
    for (auto& ch : ch_) { ch.pitch &= 0x7FF; ch.period &= 0x7FF; ch.volL &= 15; ch.volR &= 15; ch.offset &= 31; }
    sweepTime_ &= 31; noiseLfsr_ &= 0x7FFF; masterVol_ &= 3;
    clampTo(sweepCounter_, -1, 31);
    clampTo(sweepClock_, 0, 8191);
    clampTo(sdmaClock_, 0, 5);
    clampTo(hvClock_, 0, 11);
    sdmaSrc_ &= 0xFFFFF; sdmaLen_ &= 0xFFFFF; sdmaSrcReload_ &= 0xFFFFF; sdmaLenReload_ &= 0xFFFFF;
    gdmaSrc_ &= 0xFFFFF;
    clampTo<int64_t>(outPhase_, 0, 23999);
    finiteOr(hpL_, 0); finiteOr(hpR_, 0);
    // o relógio do som anda junto com o da CPU (no máximo um bloco de 128 ciclos atrás)
    if (apuClock_ > cpu.cycles || apuClock_ < cpu.cycles - 4096) apuClock_ = cpu.cycles;
    if (vertical)
      for (int y = 0; y < 144; y++)
        for (int x = 0; x < 224; x++) fbRot[(223 - x) * 144 + y] = fb[y * 224 + x];
  }
}

std::vector<uint8_t> WS::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void WS::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

}  // namespace indigo::ws
