// Chip SPC7110 e relógio RTC-4513 (ver spc7110.h).
#include "spc7110.h"

#include <algorithm>

#include "snes.h"

namespace indigo::snes {

std::time_t (*SPC7110::deviceClock)() = [] { return std::time(nullptr); };

namespace {
constexpr int64_t MASTER_HZ = 21477272, RTC_HZ = 32768 * 64;

// estados do modelo de probabilidade: probabilidade do símbolo mais provável (de 256) e o próximo estado depois de
// renormalizar com o símbolo mais provável (MPS) ou com o menos provável (LPS)
struct State { uint8_t p, nextMps, nextLps; };
constexpr State STATES[53] = {
    {0x5a, 1, 1},   {0x25, 2, 6},   {0x11, 3, 8},   {0x08, 4, 10},  {0x03, 5, 12},  {0x01, 5, 15},
    {0x5a, 7, 7},   {0x3f, 8, 19},  {0x2c, 9, 21},  {0x20, 10, 22}, {0x17, 11, 23}, {0x11, 12, 25},
    {0x0c, 13, 26}, {0x09, 14, 28}, {0x07, 15, 29}, {0x05, 16, 31}, {0x04, 17, 32}, {0x03, 18, 34},
    {0x02, 5, 35},
    {0x5a, 20, 20}, {0x48, 21, 39}, {0x3a, 22, 40}, {0x2e, 23, 42}, {0x26, 24, 44}, {0x1f, 25, 45},
    {0x19, 26, 46}, {0x15, 27, 25}, {0x11, 28, 26}, {0x0e, 29, 26}, {0x0b, 30, 27}, {0x09, 31, 28},
    {0x08, 32, 29}, {0x07, 33, 30}, {0x05, 34, 31}, {0x04, 35, 33}, {0x04, 36, 33}, {0x03, 37, 34},
    {0x02, 38, 35}, {0x02, 5, 36},
    {0x58, 40, 39}, {0x4d, 41, 47}, {0x43, 42, 48}, {0x3b, 43, 49}, {0x34, 44, 50}, {0x2e, 45, 51},
    {0x29, 46, 44}, {0x25, 24, 45},
    {0x56, 48, 47}, {0x4f, 49, 47}, {0x47, 50, 48}, {0x41, 51, 49}, {0x3c, 52, 50}, {0x37, 43, 51},
};

// separa os bits pares e ímpares dos `bits` de baixo (pixels empacotados → planos de bits): ímpares na metade de
// baixo do resultado, pares na de cima
uint32_t unzip(uint64_t v, int bits) {
  v &= (bits == 64 ? ~0ull : (1ull << bits) - 1);
  v = 0x5555555555555555ull & ((v << bits) | (v >> 1));
  v = 0x3333333333333333ull & (v | (v >> 1));
  v = 0x0f0f0f0f0f0f0f0full & (v | (v >> 2));
  v = 0x00ff00ff00ff00ffull & (v | (v >> 4));
  v = 0x0000ffff0000ffffull & (v | (v >> 8));
  return static_cast<uint32_t>(v | (v >> 16));
}

// lista de 16 cores (um nibble cada, a mais recente embaixo): traz a cor c para a frente
uint64_t toFront(uint64_t list, unsigned c) {
  for (int n = 0; n < 64; n += 4) {
    if (((list >> n) & 15) != c) continue;
    const uint64_t above = n + 4 < 64 ? ~0ull << (n + 4) : 0;  // nibbles acima de c ficam onde estão
    return (list & above) | ((list << 4) & ~above & ~0xFull) | c;
  }
  return list;
}
}  // namespace

// ---------------------------------------------------------------- montagem
SPC7110::SPC7110(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram, bool rtc) : rom_(rom), ram_(ram), rtc_(rtc) {
  if (rom.size() > 0x100000) {
    dromBase_ = 0x100000;
    dromSize_ = rom.size() - 0x100000;
    if (rom.size() == 0x700000) { dromSize_ = 0x500000; expBase_ = 0x600000; expSize_ = 0x100000; }
  }
  reset(true);
}

void SPC7110::reset(bool power) {
  r4801 = r4802 = r4803 = r4804 = r4805 = r4806 = r4807 = 0;
  r4809 = r480a = r480b = r480c = 0;
  dcuMode = 0; dcuAddress = 0; dcuOffset = 0; dcuWait = 0;
  std::fill(std::begin(dcuTile), std::end(dcuTile), 0);
  dec_ = Decoder{};
  r4810 = r4811 = r4812 = r4813 = r4814 = r4815 = r4816 = r4817 = r4818 = 0;
  std::fill(std::begin(alu), std::end(alu), 0);
  mulWait = divWait = 0;
  r4830 = 0; r4831 = 0; r4832 = 1; r4833 = 2; r4834 = 0;
  // o relógio tem bateria: o reset não mexe na hora; ao ligar, o chip começa como novo (bateria acabada)
  clock_.chipSelect = 0; clock_.state = 0; clock_.offset = 0; clock_.wait = 0; clock_.ready = 0; clock_.holdTick = 0;
  if (power) {
    clock_.powerOn();
    rtcDebt_ = 0;
    if (rtc_) { clock_.followDevice = true; clock_.setFrom(deviceClock()); }
  }
}

void SPC7110::run(int n) {
  if (mulWait > 0 && (mulWait -= n) <= 0) { mulWait = 0; multiply(); }
  if (divWait > 0 && (divWait -= n) <= 0) { divWait = 0; divide(); }
  if (dcuWait > 0 && (dcuWait -= n) <= 0) { dcuWait = 0; dcuBegin(); }
  if (rtc_) {
    rtcDebt_ += int64_t(n) * RTC_HZ;
    while (rtcDebt_ >= MASTER_HZ) { rtcDebt_ -= MASTER_HZ; clock_.step(); }
  }
}

// ---------------------------------------------------------------- memória
uint8_t SPC7110::dataRom(uint32_t addr) const {
  const int sz = r4834 & 3;  // 1, 2, 4 ou 8 MB
  if (sz != 3 && (addr & 0x400000)) return 0x00;
  if (!dromSize_) return 0x00;
  const size_t off = addr & ((size_t(0x100000) << sz) - 1);
  return rom_[dromBase_ + romMirror(off, dromSize_)];
}

bool SPC7110::romRead(uint32_t addr, uint8_t& v) const {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (expSize_ && bank >= 0x40 && bank < 0x50) { v = rom_[expBase_ + ((size_t(bank & 0x0F) << 16) | a)]; return true; }
  if (!(bank >= 0xC0 || (!(bank & 0x40) && a >= 0x8000))) return false;
  const uint32_t r = (uint32_t(bank & 0x3F) << 16) | a;  // 4 MB vistos pela CPU: 4 janelas de 1 MB
  const uint32_t in = r & 0xFFFFF;
  const size_t promSize = dromSize_ ? 0x100000 : rom_.size();
  switch (r >> 20) {
    case 0: v = rom_[romMirror(in, promSize)]; return true;  // ROM de programa (o 1º MB)
    case 1:
      if (r4834 & 4) { v = rom_[romMirror(0x100000 + in, promSize)]; return true; }
      v = dataRom(in | (0x100000u * (r4831 & 7)));
      return true;
    case 2: v = dataRom(in | (0x100000u * (r4832 & 7))); return true;
    default: v = dataRom(in | (0x100000u * (r4833 & 7))); return true;
  }
}

bool SPC7110::ramRead(uint32_t addr, uint8_t& v) const {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if ((bank & 0x40) || a < 0x6000 || a >= 0x8000) return false;
  v = 0x00;
  if ((r4830 & 0x80) && !ram_.empty()) v = ram_[romMirror((uint32_t(bank) << 13) | (a & 0x1FFF), ram_.size())];
  return true;
}

bool SPC7110::ramWrite(uint32_t addr, uint8_t v, bool& changed) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if ((bank & 0x40) || a < 0x6000 || a >= 0x8000) return false;
  if ((r4830 & 0x80) && !ram_.empty()) {
    uint8_t& m = ram_[romMirror((uint32_t(bank) << 13) | (a & 0x1FFF), ram_.size())];
    if (m != v) { m = v; changed = true; }
  }
  return true;
}

// ---------------------------------------------------------------- registradores
uint8_t SPC7110::ioRead(uint32_t addr, uint8_t openBus) {
  const int bank = (addr >> 16) & 0xFF;
  uint16_t a = static_cast<uint16_t>(addr);
  if (bank == 0x50) a = 0x4800;
  else if (bank == 0x58) a = 0x4808;
  if (a >= 0x4840) {
    if (!rtc_) return openBus;
    return a < 0x4843 ? clock_.read(a & 3) : openBus;
  }
  a = 0x4800 | (a & 0x3F);
  switch (a) {
    case 0x4800: {
      const uint16_t n = static_cast<uint16_t>((r4809 | (r480a << 8)) - 1);
      r4809 = uint8_t(n); r480a = uint8_t(n >> 8);
      return dcuRead();
    }
    case 0x4801: return r4801;
    case 0x4802: return r4802;
    case 0x4803: return r4803;
    case 0x4804: return r4804;
    case 0x4805: return r4805;
    case 0x4806: return r4806;
    case 0x4807: return r4807;
    case 0x4808: return 0x00;
    case 0x4809: return r4809;
    case 0x480A: return r480a;
    case 0x480B: return r480b;
    case 0x480C: return r480c;
    case 0x4810: { const uint8_t v = r4810; dataPortStep(); return v; }
    case 0x4811: return r4811;
    case 0x4812: return r4812;
    case 0x4813: return r4813;
    case 0x4814: return r4814;
    case 0x4815: return r4815;
    case 0x4816: return r4816;
    case 0x4817: return r4817;
    case 0x4818: return r4818;
    case 0x481A: dataPortAdjust(3); return 0x00;
    case 0x4830: return r4830;
    case 0x4831: return r4831;
    case 0x4832: return r4832;
    case 0x4833: return r4833;
    case 0x4834: return r4834;
    default:
      if (a >= 0x4820 && a < 0x4830) return alu[a - 0x4820];
      return openBus;
  }
}

void SPC7110::ioWrite(uint32_t addr, uint8_t v) {
  const int bank = (addr >> 16) & 0xFF;
  uint16_t a = static_cast<uint16_t>(addr);
  if (bank == 0x50) a = 0x4800;
  else if (bank == 0x58) a = 0x4808;
  if (a >= 0x4840) {
    if (rtc_ && a < 0x4843) clock_.write(a & 3, v);
    return;
  }
  a = 0x4800 | (a & 0x3F);
  switch (a) {
    case 0x4801: r4801 = v; break;
    case 0x4802: r4802 = v; break;
    case 0x4803: r4803 = v & 0x7F; break;
    case 0x4804: r4804 = v; dcuLoadAddress(); break;
    case 0x4805: r4805 = v; break;
    case 0x4806: r4806 = v; r480c &= 0x7F; dcuWait = 21; break;  // começa um ciclo depois e gasta 20
    case 0x4807: r4807 = v; break;
    case 0x4809: r4809 = v; break;
    case 0x480A: r480a = v; break;
    case 0x480B: r480b = v & 0x03; break;
    case 0x4811: r4811 = v; break;
    case 0x4812: r4812 = v; break;
    case 0x4813: r4813 = v & 0x7F; dataPortRead(); break;
    case 0x4814: r4814 = v; dataPortAdjust(1); break;
    case 0x4815: r4815 = v; if (r4818 & 2) dataPortRead(); dataPortAdjust(2); break;
    case 0x4816: r4816 = v; break;
    case 0x4817: r4817 = v; break;
    case 0x4818: r4818 = v & 0x7F; dataPortRead(); break;
    case 0x4820: case 0x4821: case 0x4822: case 0x4823: case 0x4824: case 0x4826: alu[a - 0x4820] = v; break;
    case 0x4825: alu[5] = v; alu[15] |= 0x81; mulWait = 31; break;
    case 0x4827: alu[7] = v; alu[15] |= 0x80; divWait = 41; break;
    case 0x482E: alu[14] = v & 0x01; break;
    case 0x4830: r4830 = v & 0x87; break;
    case 0x4831: r4831 = v & 0x07; break;
    case 0x4832: r4832 = v & 0x07; break;
    case 0x4833: r4833 = v & 0x07; break;
    case 0x4834: r4834 = v & 0x07; break;
    default: break;
  }
}

// ---------------------------------------------------------------- porta de dados
void SPC7110::dataPortRead() {
  uint32_t adjust = (r4818 & 2) ? dataAdjust() : 0;
  if (r4818 & 8) adjust = static_cast<uint32_t>(int32_t(int16_t(adjust)));
  r4810 = dataRom(dataOffset() + adjust);
}

void SPC7110::dataPortStep() {
  uint32_t stride = (r4818 & 1) ? dataStride() : 1;
  uint32_t adjust = dataAdjust();
  if (r4818 & 4) stride = static_cast<uint32_t>(int32_t(int16_t(stride)));
  if (r4818 & 8) adjust = static_cast<uint32_t>(int32_t(int16_t(adjust)));
  if (r4818 & 16) setDataAdjust(adjust + stride);
  else setDataOffset(dataOffset() + stride);
  dataPortRead();
}

void SPC7110::dataPortAdjust(int trigger) {
  if ((r4818 >> 5) != trigger) return;
  uint32_t adjust = dataAdjust();
  if (r4818 & 8) adjust = static_cast<uint32_t>(int32_t(int16_t(adjust)));
  setDataOffset(dataOffset() + adjust);
  dataPortRead();
}

// ---------------------------------------------------------------- contas
void SPC7110::multiply() {
  const uint16_t a = static_cast<uint16_t>(alu[4] | (alu[5] << 8)), b = static_cast<uint16_t>(alu[0] | (alu[1] << 8));
  uint32_t r;
  if (alu[14] & 1) r = static_cast<uint32_t>(int32_t(int16_t(a)) * int32_t(int16_t(b)));
  else r = uint32_t(a) * uint32_t(b);
  for (int i = 0; i < 4; i++) alu[8 + i] = uint8_t(r >> (8 * i));
  alu[15] &= 0x7F;
}

void SPC7110::divide() {
  const uint32_t dividend = alu[0] | (alu[1] << 8) | (alu[2] << 16) | (uint32_t(alu[3]) << 24);
  const uint16_t divisor = static_cast<uint16_t>(alu[6] | (alu[7] << 8));
  uint32_t q;
  uint16_t rem;
  if (alu[14] & 1) {
    const int32_t n = static_cast<int32_t>(dividend);
    const int32_t d = int16_t(divisor);
    if (d) {
      // -2^31 ÷ -1 não cabe: o resultado é o próprio dividendo (como a conta em 64 bits truncada)
      const int64_t qq = int64_t(n) / d;
      q = static_cast<uint32_t>(qq);
      rem = static_cast<uint16_t>(int64_t(n) % d);
    } else { q = 0; rem = static_cast<uint16_t>(dividend); }
  } else {
    if (divisor) { q = dividend / divisor; rem = static_cast<uint16_t>(dividend % divisor); }
    else { q = 0; rem = static_cast<uint16_t>(dividend); }
  }
  for (int i = 0; i < 4; i++) alu[8 + i] = uint8_t(q >> (8 * i));
  alu[12] = uint8_t(rem); alu[13] = uint8_t(rem >> 8);
  alu[15] &= 0x7F;
}

// ---------------------------------------------------------------- descompressão
void SPC7110::dcuLoadAddress() {
  const uint32_t at = (r4801 | (r4802 << 8) | (uint32_t(r4803) << 16)) + (uint32_t(r4804) << 2);
  dcuMode = dataRom(at) & 3;
  dcuAddress = ((uint32_t(dataRom(at + 1)) << 16) | (dataRom(at + 2) << 8) | dataRom(at + 3)) & 0x7FFFFF;
}

void SPC7110::dcuBegin() {
  if (dcuMode == 3) return;  // modo inválido: nada acontece
  dec_.start(*this, dcuMode, dcuAddress);
  dec_.decode(*this);
  uint32_t skip = (r480b & 2) ? (r4805 | (r4806 << 8)) : 0;
  while (skip--) dec_.decode(*this);
  r480c |= 0x80;
  dcuOffset = 0;
}

uint8_t SPC7110::dcuRead() {
  if (!(r480c & 0x80)) return 0x00;
  if (dcuOffset == 0) {
    // um tile inteiro (8 linhas) de cada vez; entre as linhas, pula r4807 palavras se o bit 0 de $480B estiver ligado
    for (int row = 0; row < 8; row++) {
      const uint32_t w = dec_.word;
      if (dec_.bpp == 1) dcuTile[row] = uint8_t(w);
      else if (dec_.bpp == 2) { dcuTile[row * 2] = uint8_t(w); dcuTile[row * 2 + 1] = uint8_t(w >> 8); }
      else {
        dcuTile[row * 2] = uint8_t(w); dcuTile[row * 2 + 1] = uint8_t(w >> 8);
        dcuTile[row * 2 + 16] = uint8_t(w >> 16); dcuTile[row * 2 + 17] = uint8_t(w >> 24);
      }
      uint32_t skip = (r480b & 1) ? r4807 : 1;
      while (skip--) dec_.decode(*this);
    }
  }
  const uint8_t v = dcuTile[dcuOffset++];
  dcuOffset &= static_cast<uint8_t>(8 * dec_.bpp - 1);
  return v;
}

void SPC7110::Decoder::start(const SPC7110& c, int mode, uint32_t origin) {
  for (auto& set : ctx) for (auto& x : set) x = Ctx{};
  bpp = static_cast<uint8_t>(1 << mode);
  offset = origin;
  bitsLeft = 8;
  range = 0x100;
  code = static_cast<uint16_t>(c.dataRom(offset++) << 8);
  code = static_cast<uint16_t>(code | c.dataRom(offset++));
  history = 0;
  pixels = 0;
  recent = 0xfedcba9876543210ull;
  word = 0;
}

void SPC7110::Decoder::decode(const SPC7110& c) {
  for (int px = 0; px < 8; px++) {
    uint64_t order = recent;
    int similar = 0;  // como os três vizinhos (esquerda, cima, cima-esquerda) se parecem
    if (bpp > 1) {
      const unsigned left = bpp == 2 ? (pixels >> 2) & 3 : pixels & 15;
      const unsigned up = bpp == 2 ? (pixels >> 14) & 3 : (pixels >> 28) & 15;
      const unsigned upLeft = bpp == 2 ? (pixels >> 16) & 3 : (pixels >> 32) & 15;
      if (left != up || up != upLeft) {
        if (up == upLeft) similar = 1;          // só o da esquerda é diferente
        else if (upLeft == left) similar = 2;   // só o de cima é diferente
        else if (left == up) similar = 3;       // só o de cima-esquerda é diferente
        else similar = 4;                       // os três diferentes
      }
      recent = toFront(recent, left);
      order = toFront(toFront(toFront(order, upLeft), up), left);
    }
    for (int plane = 0; plane < bpp; plane++) {
      const unsigned bit = bpp > 1 ? 1u << plane : 1u << (px & 3);
      const unsigned prev = (bit - 1) & history;
      int set = 0;
      if (bpp == 1) set = px >= 4;
      if (bpp == 2) set = similar;
      if (plane >= 2 && prev <= 1) set = similar;
      Ctx& cx = ctx[set][bit + prev - 1];
      const State& st = STATES[cx.state];
      const uint8_t split = static_cast<uint8_t>(range - st.p);  // abaixo: símbolo mais provável
      const unsigned lps = code >= (split << 8);
      history = static_cast<uint8_t>((history << 1) | (lps ^ cx.invert));
      if (!lps) range = split;
      else { range = static_cast<uint16_t>(range - split); code = static_cast<uint16_t>(code - (split << 8)); }
      while (range <= 0x7F) {
        cx.state = lps ? st.nextLps : st.nextMps;
        range = static_cast<uint16_t>(range << 1);
        code = static_cast<uint16_t>(code << 1);
        if (--bitsLeft == 0) { bitsLeft = 8; code = static_cast<uint16_t>(code + c.dataRom(offset++)); }
      }
      if (lps && st.p > 0x55) cx.invert ^= 1;
    }
    unsigned idx = history & ((1u << bpp) - 1);
    if (bpp == 1) idx ^= (pixels >> 15) & 1;
    pixels = (pixels << bpp) | ((order >> (4 * idx)) & 15);
  }
  if (bpp == 1) word = static_cast<uint32_t>(pixels);
  else if (bpp == 2) word = unzip(pixels, 16);
  else word = unzip(unzip(pixels, 32), 32);
}

void SPC7110::Decoder::sync(StateIO& io) {
  for (auto& set : ctx) for (auto& x : set) { io.sync(x.state); io.sync(x.invert); }
  io.sync(bpp); io.sync(offset); io.sync(bitsLeft); io.sync(range); io.sync(code); io.sync(history);
  io.sync(pixels); io.sync(recent); io.sync(word);
  if (io.reading()) {
    for (auto& set : ctx) for (auto& x : set) { if (x.state >= 53) x.state = 0; x.invert &= 1; }
    if (bpp != 1 && bpp != 2 && bpp != 4) bpp = 1;
    offset &= 0xFFFFFF;
    if (bitsLeft < 1 || bitsLeft > 8) bitsLeft = 8;
    clampTo<uint16_t>(range, 0x80, 0x100);
  }
}

// ---------------------------------------------------------------- relógio RTC-4513
// Três portas: $4840 seleção do chip, $4841 dados (um nibble), $4842 pronto (bit 7). Protocolo: com o chip
// selecionado, o modo (3 = gravar, $C = ler), o endereço do registrador e depois os nibbles, com o endereço
// avançando sozinho. Entre um nibble e outro o chip fica ocupado por 8 ciclos do relógio dele.
void SPC7110::Rtc::powerOn() {
  ticks = 0; seconds = 0;
  secondLo = secondHi = 0; batteryFail = 1; minuteLo = minuteHi = 0; resync = 0;
  hourLo = hourHi = 0; meridian = 0; dayLo = dayHi = 0; dayRam = 0;
  monthLo = monthHi = 0; monthRam = 0; yearLo = yearHi = 0; weekday = 0;
  hold = calendar = irqFlag = roundSec = 0; irqMask = irqDuty = irqPeriod = 0;
  pause = stop = atime = test = 0;
}

void SPC7110::Rtc::setFrom(std::time_t t) {
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  const int sec = std::min(59, tm.tm_sec);
  secondLo = uint8_t(sec % 10); secondHi = uint8_t(sec / 10);
  minuteLo = uint8_t(tm.tm_min % 10); minuteHi = uint8_t(tm.tm_min / 10);
  int hr = tm.tm_hour;
  if (atime) meridian = 0;
  else { meridian = hr >= 12; hr %= 12; if (hr == 0) hr = 12; }
  hourLo = uint8_t(hr % 10); hourHi = uint8_t(hr / 10);
  dayLo = uint8_t(tm.tm_mday % 10); dayHi = uint8_t(tm.tm_mday / 10);
  const int mon = tm.tm_mon + 1;
  monthLo = uint8_t(mon % 10); monthHi = uint8_t(mon / 10);
  const int yr = tm.tm_year % 100;
  yearLo = uint8_t(yr % 10); yearHi = uint8_t(yr / 10);
  weekday = uint8_t(tm.tm_wday);
  batteryFail = 0;  // hora válida: o jogo não vê "bateria acabada"
  calendar = 1;     // e a data anda à meia-noite
  resync = 1;       // avisa o jogo de que a hora mudou
  mask();
}

void SPC7110::Rtc::mask() {
  secondLo &= 15; secondHi &= 7; batteryFail &= 1; minuteLo &= 15; minuteHi &= 7; resync &= 1;
  hourLo &= 15; hourHi &= 3; meridian &= 1; dayLo &= 15; dayHi &= 3; dayRam &= 1;
  monthLo &= 15; monthHi &= 1; monthRam &= 3; yearLo &= 15; yearHi &= 15; weekday &= 7;
  hold &= 1; calendar &= 1; irqFlag &= 1; roundSec &= 1; irqMask &= 1; irqDuty &= 1; irqPeriod &= 3;
  pause &= 1; stop &= 1; atime &= 1; test &= 1;
  chipSelect &= 3; offset &= 15; mdr &= 15; ready &= 1; holdTick &= 1; ticks &= 0x1FFFFF;
}

void SPC7110::Rtc::step() {
  if (wait && --wait == 0) ready = 1;
  ticks = (ticks + 1) & 0x1FFFFF;
  if ((ticks & 0xFF) == 0 && roundSec) {  // 125 µs
    roundSec = 0;
    if (secondHi >= 3) tickMinute();
    secondLo = secondHi = 0;
  }
  if ((ticks & 0x3FFF) == 0 && irqDuty) irqFlag = 0;  // 1/128 s
  if ((ticks & 0x7FFF) == 0) irq(0);                  // 1/64 s
  if (ticks == 0) {                                   // 1 s
    seconds++;
    irq(1);
    if (seconds % 60 == 0) irq(2);
    if (seconds % 3600 == 0) { irq(3); seconds = 0; }
    tick();
  }
}

uint8_t SPC7110::Rtc::read(int a) {
  if (a == 0) return chipSelect;
  if (a == 2) return static_cast<uint8_t>(ready << 7);
  if (chipSelect != 1 || !ready) return 0;
  if (state == 3) return mdr;  // gravando
  if (state != 2) return 0;
  ready = 0;
  wait = 8;
  const uint8_t v = regRead(offset);
  offset = (offset + 1) & 15;
  return v;
}

void SPC7110::Rtc::write(int a, uint8_t v) {
  v &= 15;
  if (a == 0) {
    chipSelect = v & 3;
    if (chipSelect != 1) { state = 0; offset = 0; resync = 0; pause = 0; test = 0; }
    ready = 1;
    return;
  }
  if (a != 1 || chipSelect != 1 || !ready) return;
  if (state == 0) {  // modo
    if (v != 0x03 && v != 0x0C) return;
    state = 1;
  } else if (state == 1) {  // endereço
    state = mdr == 0x03 ? 3 : 2;
    offset = v;
  } else if (state == 3) {
    regWrite(offset, v);
    offset = (offset + 1) & 15;
  } else {
    return;  // lendo: gravação ignorada
  }
  ready = 0;
  wait = 8;
  mdr = v;
}

uint8_t SPC7110::Rtc::regRead(int a) {
  switch (a & 15) {
    case 0: return secondLo;
    case 1: return uint8_t(secondHi | (batteryFail << 3));
    case 2: return minuteLo;
    case 3: return uint8_t(minuteHi | (resync << 3));
    case 4: return hourLo;
    case 5: return uint8_t(hourHi | (meridian << 2) | (resync << 3));
    case 6: return dayLo;
    case 7: return uint8_t(dayHi | (dayRam << 2) | (resync << 3));
    case 8: return monthLo;
    case 9: return uint8_t(monthHi | (monthRam << 1) | (resync << 3));
    case 10: return yearLo;
    case 11: return yearHi;
    case 12: return uint8_t(weekday | (resync << 3));
    case 13: {
      const uint8_t f = irqFlag & !irqMask;
      irqFlag = 0;
      return uint8_t(hold | (calendar << 1) | (f << 2) | (roundSec << 3));
    }
    case 14: return uint8_t(irqMask | (irqDuty << 1) | (irqPeriod << 2));
    default: return uint8_t(pause | (stop << 1) | (atime << 2) | (test << 3));
  }
}

void SPC7110::Rtc::regWrite(int a, uint8_t v) {
  switch (a & 15) {
    case 0: secondLo = v; break;
    case 1: secondHi = v; batteryFail = v >> 3; break;
    case 2: minuteLo = v; break;
    case 3: minuteHi = v; break;
    case 4: hourLo = v; break;
    case 5:
      hourHi = v; meridian = (v >> 2) & 1;
      if (atime) meridian = 0; else hourHi &= 1;
      break;
    case 6: dayLo = v; break;
    case 7: dayHi = v; dayRam = (v >> 2) & 1; break;
    case 8: monthLo = v; break;
    case 9: monthHi = v; monthRam = (v >> 1) & 3; break;
    case 10: yearLo = v; break;
    case 11: yearHi = v; break;
    case 12: weekday = v; break;
    case 13: {
      const bool held = hold;
      hold = v & 1; calendar = (v >> 1) & 1; roundSec = (v >> 3) & 1;
      if (held && !hold && holdTick) { holdTick = 0; mask(); tickSecond(); }  // um segundo passou parado
      break;
    }
    case 14: irqMask = v & 1; irqDuty = (v >> 1) & 1; irqPeriod = (v >> 2) & 3; break;
    default: {
      const uint8_t oldMode = atime;
      pause = v & 1; stop = (v >> 1) & 1; atime = (v >> 2) & 1; test = (v >> 3) & 1;
      if (atime) meridian = 0; else hourHi &= 1;
      if (pause) secondLo = secondHi = 0;
      // o jogo trocou entre 12 e 24 horas: a hora do aparelho de novo, já no formato novo
      if (followDevice && atime != oldMode) { mask(); setFrom(SPC7110::deviceClock()); }
      break;
    }
  }
  mask();
}

void SPC7110::Rtc::tick() {
  if (stop || pause) return;
  if (hold) { holdTick = 1; return; }
  resync = 1;
  tickSecond();
}

// as contas abaixo seguem o chip também com valores de BCD inválidos (por isso as comparações com 12 e os
// "!(x & 1)": é o que o contador de 4 bits do RTC-4513 faz)
void SPC7110::Rtc::tickSecond() {
  if (secondLo <= 8 || secondLo == 12) secondLo++;
  else {
    secondLo = 0;
    if (secondHi < 5) secondHi++;
    else { secondHi = 0; tickMinute(); }
  }
  mask();
}

void SPC7110::Rtc::tickMinute() {
  if (minuteLo <= 8 || minuteLo == 12) minuteLo++;
  else {
    minuteLo = 0;
    if (minuteHi < 5) minuteHi++;
    else { minuteHi = 0; tickHour(); }
  }
  mask();
}

void SPC7110::Rtc::tickHour() {
  if (atime) {  // 24 horas
    if (hourHi < 2) {
      if (hourLo <= 8 || hourLo == 12) hourLo++;
      else { hourLo = !(hourLo & 1); hourHi++; }
    } else if (hourLo != 3 && !(hourLo & 4)) {
      if (hourLo <= 8 || hourLo >= 12) hourLo++;
      else { hourLo = !(hourLo & 1); hourHi++; }
    } else {
      hourLo = !(hourLo & 1); hourHi = 0;
      mask();
      tickDay();
    }
  } else {  // 12 horas com AM/PM
    if (hourHi == 0) {
      if (hourLo <= 8 || hourLo == 12) hourLo++;
      else { hourLo = !(hourLo & 1); hourHi ^= 1; }
    } else {
      if (hourLo & 1) meridian ^= 1;
      if (hourLo < 2 || hourLo == 4 || hourLo == 5 || hourLo == 8 || hourLo == 12) hourLo++;
      else { hourLo = !(hourLo & 1); hourHi ^= 1; }
      mask();
      if (meridian == 0 && !(hourLo & 1)) tickDay();
    }
  }
  mask();
}

void SPC7110::Rtc::tickDay() {
  if (!calendar) return;
  weekday = uint8_t(((weekday + 1) + (weekday == 6)) & 7);
  // dias do mês pelo valor BCD do mês ($01-$09, $10-$12; os inválidos seguem o chip)
  static const uint8_t DAYS[32] = {30, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 30, 31, 30,
                                   31, 30, 31, 30, 31, 30, 31, 30, 31, 30, 31, 30, 31, 30, 31, 30};
  int days = DAYS[((monthHi & 1) << 4) | monthLo];
  if (days == 28) {  // ano bissexto (anos com dezena par terminam em 0, 4, 8; ímpar em 2, 6)
    if (!(yearHi & 1) && ((yearLo - 0) & 3) == 0) days++;
    if ((yearHi & 1) && ((yearLo - 2) & 3) == 0) days++;
  }
  const bool next = (days == 28 && (dayHi == 3 || (dayHi == 2 && dayLo >= 8))) ||
                    (days == 29 && (dayHi == 3 || (dayHi == 2 && dayLo > 8 && dayLo != 12))) ||
                    (days == 30 && (dayHi == 3 || (dayHi == 2 && (dayLo == 10 || dayLo == 14)))) ||
                    (days == 31 && dayHi == 3 && (dayLo & 3));
  if (next) { dayLo = 1; dayHi = 0; tickMonth(); return; }
  if (dayLo <= 8 || dayLo == 12) dayLo++;
  else { dayLo = !(dayLo & 1); dayHi++; }
  mask();
}

void SPC7110::Rtc::tickMonth() {
  if (monthHi == 0 || !(monthLo & 2)) {
    if (monthLo <= 8 || monthLo == 12) monthLo++;
    else { monthLo = !(monthLo & 1); monthHi ^= 1; }
  } else {
    monthLo = !(monthLo & 1); monthHi = 0;
    mask();
    tickYear();
  }
  mask();
}

void SPC7110::Rtc::tickYear() {
  if (yearLo <= 8 || yearLo == 12) yearLo++;
  else {
    yearLo = !(yearLo & 1);
    if (yearHi <= 8 || yearHi == 12) yearHi++;
    else yearHi = !(yearHi & 1);
  }
  mask();
}

void SPC7110::Rtc::sync(StateIO& io) {
  io.sync(ticks); io.sync(seconds); io.sync(chipSelect); io.sync(state); io.sync(mdr); io.sync(offset);
  io.sync(ready); io.sync(holdTick); io.sync(wait);
  uint8_t* f[] = {&secondLo, &secondHi, &batteryFail, &minuteLo, &minuteHi, &resync, &hourLo, &hourHi, &meridian,
                  &dayLo, &dayHi, &dayRam, &monthLo, &monthHi, &monthRam, &yearLo, &yearHi, &weekday,
                  &hold, &calendar, &irqFlag, &roundSec, &irqMask, &irqDuty, &irqPeriod, &pause, &stop, &atime, &test};
  for (uint8_t* p : f) io.sync(*p);
  if (io.reading()) {
    mask();
    if (state > 3) state = 0;
    if (wait > 8) wait = 8;
    seconds %= 3600;
  }
}

// ---------------------------------------------------------------- estado
void SPC7110::sync(StateIO& io) {
  uint8_t* regs[] = {&r4801, &r4802, &r4803, &r4804, &r4805, &r4806, &r4807, &r4809, &r480a, &r480b, &r480c,
                     &dcuMode, &dcuOffset, &r4810, &r4811, &r4812, &r4813, &r4814, &r4815, &r4816, &r4817, &r4818,
                     &r4830, &r4831, &r4832, &r4833, &r4834};
  for (uint8_t* p : regs) io.sync(*p);
  io.sync(dcuAddress); io.raw(dcuTile, sizeof dcuTile); io.sync(dcuWait);
  dec_.sync(io);
  io.raw(alu, sizeof alu); io.sync(mulWait); io.sync(divWait);
  if (rtc_) { clock_.sync(io); io.sync(rtcDebt_); }
  if (io.reading()) {
    r4803 &= 0x7F; r4813 &= 0x7F; r480b &= 3; r4818 &= 0x7F; alu[14] &= 1;
    r4830 &= 0x87; r4831 &= 7; r4832 &= 7; r4833 &= 7; r4834 &= 7;
    dcuMode &= 3; dcuAddress &= 0x7FFFFF; dcuOffset &= 31;
    clampTo<int32_t>(dcuWait, 0, 21); clampTo<int32_t>(mulWait, 0, 31); clampTo<int32_t>(divWait, 0, 41);
    clampTo<int64_t>(rtcDebt_, 0, MASTER_HZ);
  }
}

}  // namespace indigo::snes
