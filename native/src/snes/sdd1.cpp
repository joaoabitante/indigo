// Chip S-DD1: MMC dos bancos $C0-$FF e descompressão durante o DMA (ver sdd1.h).
#include "sdd1.h"

#include "snes.h"

namespace indigo::snes {

namespace {
// estados da estimativa de probabilidade: código de Golomb, próximo estado se o símbolo é o mais provável (MPS)
// e se é o menos provável (LPS)
struct Evolution { uint8_t code, nextMps, nextLps; };
constexpr Evolution EVOLUTION[33] = {
    {0, 25, 25}, {0, 2, 1},   {0, 3, 1},   {0, 4, 2},   {0, 5, 3},   {1, 6, 4},   {1, 7, 5},   {1, 8, 6},
    {1, 9, 7},   {2, 10, 8},  {2, 11, 9},  {2, 12, 10}, {2, 13, 11}, {3, 14, 12}, {3, 15, 13}, {3, 16, 14},
    {3, 17, 15}, {4, 18, 16}, {4, 19, 17}, {5, 20, 18}, {5, 21, 19}, {6, 22, 20}, {6, 23, 21}, {7, 24, 22},
    {7, 24, 23}, {0, 26, 1},  {1, 27, 2},  {2, 28, 4},  {3, 29, 8},  {4, 30, 12}, {5, 31, 16}, {6, 32, 18},
    {7, 24, 22}};

// tamanho da sequência de MPS antes de um LPS, pela palavra de código (1 + n bits): os n bits invertidos e
// lidos ao contrário. Índice = palavra com o 1 de marcação na frente.
struct RunTable {
  uint8_t v[256];
  constexpr RunTable() : v() {
    for (int i = 0; i < 256; i++) {
      int top = 7;
      while (top > 0 && !(i >> top)) top--;
      int r = 0;
      for (int b = 0; b < top; b++) r |= ((~i >> b) & 1) << (top - 1 - b);
      v[i] = static_cast<uint8_t>(r);
    }
  }
};
constexpr RunTable RUN;
}  // namespace

void SDD1::reset() {
  r4800_ = r4801_ = 0;
  for (int i = 0; i < 4; i++) mmc_[i] = static_cast<uint8_t>(i);
  for (int i = 0; i < 8; i++) { dmaAddr_[i] = 0; dmaSize_[i] = 0; }
  dmaReady_ = false;
  dec_ = Decompressor{};
}

uint8_t SDD1::ioRead(uint16_t a, uint8_t openBus) const {
  switch (a & 0xF) {
    case 0: return r4800_;
    case 1: return r4801_;
    case 4: case 5: case 6: case 7: return mmc_[(a & 0xF) - 4];
    default: return openBus;
  }
}

void SDD1::ioWrite(uint16_t a, uint8_t v) {
  switch (a & 0xF) {
    case 0: r4800_ = v; break;
    case 1: r4801_ = v; break;
    case 4: case 5: case 6: case 7: mmc_[(a & 0xF) - 4] = v & 0x8F; break;
    default: break;
  }
}

void SDD1::dmaRegWrite(uint16_t a, uint8_t v) {
  const int c = (a >> 4) & 7;
  switch (a & 0xF) {
    case 2: dmaAddr_[c] = (dmaAddr_[c] & 0xFFFF00) | v; break;
    case 3: dmaAddr_[c] = (dmaAddr_[c] & 0xFF00FF) | (uint32_t(v) << 8); break;
    case 4: dmaAddr_[c] = (dmaAddr_[c] & 0x00FFFF) | (uint32_t(v) << 16); break;
    case 5: dmaSize_[c] = static_cast<uint16_t>((dmaSize_[c] & 0xFF00) | v); break;
    case 6: dmaSize_[c] = static_cast<uint16_t>((dmaSize_[c] & 0x00FF) | (v << 8)); break;
    default: break;
  }
}

uint8_t SDD1::mmcRead(uint32_t addr) const {
  const size_t off = (size_t(mmc_[(addr >> 20) & 3] & 0x0F) << 20) | (addr & 0xFFFFF);
  return rom_[romMirror(off, rom_.size())];
}

uint8_t SDD1::romRead(uint32_t addr) {
  addr &= 0xFFFFFF;
  if (!(addr & 0x400000)) {
    // $00-$3F/$80-$BF:$8000-$FFFF, como LoROM; o bit 7 de $4805 ($20-$3F) e de $4807 ($A0-$BF) espelha o 1º MB
    if ((addr & 0x200000) && (mmc_[(addr & 0x800000) ? 3 : 1] & 0x80)) addr &= ~0x200000u;
    const size_t off = ((addr >> 1) & 0x1F8000) | (addr & 0x7FFF);
    return rom_[romMirror(off, rom_.size())];
  }
  if (r4800_ & r4801_) {
    for (int n = 0; n < 8; n++) {
      if (!((r4800_ & r4801_) >> n & 1) || addr != dmaAddr_[n]) continue;
      // o DMA do S-DD1 é sempre de endereço fixo: a mesma leitura repetida tira o próximo byte descomprimido
      if (!dmaReady_) { dec_.init(*this, addr); dmaReady_ = true; }
      const uint8_t v = dec_.read(*this);
      if (--dmaSize_[n] == 0) { dmaReady_ = false; r4801_ = static_cast<uint8_t>(r4801_ & ~(1 << n)); }
      return v;
    }
  }
  return mmcRead(addr);
}

// ---------------------------------------------------------------- descompressor
void SDD1::Decompressor::init(const SDD1& c, uint32_t addr) {
  *this = Decompressor{};
  offset = addr & 0xFFFFFF;
  bitCount = 4;  // os 4 primeiros bits são o cabeçalho
  const uint8_t head = c.mmcRead(offset);
  bitplanesInfo = head & 0xC0;
  contextBitsInfo = head & 0x30;
  currentBitplane = bitplanesInfo == 0x40 ? 7 : bitplanesInfo == 0x80 ? 3 : 1;
  r0 = 1;
}

uint8_t SDD1::Decompressor::codeWord(const SDD1& c, int length) {
  uint8_t w = static_cast<uint8_t>(c.mmcRead(offset) << bitCount);
  bitCount++;
  if (w & 0x80) {
    w = static_cast<uint8_t>(w | (c.mmcRead((offset + 1) & 0xFFFFFF) >> (9 - bitCount)));
    bitCount = static_cast<uint8_t>(bitCount + length);
  }
  if (bitCount & 8) { offset = (offset + 1) & 0xFFFFFF; bitCount &= 7; }
  return w;
}

uint8_t SDD1::Decompressor::bgBit(const SDD1& c, int code, bool& endOfRun) {
  if (!mpsCount[code] && !lpsIndex[code]) {
    const uint8_t w = codeWord(c, code);
    if (w & 0x80) { lpsIndex[code] = true; mpsCount[code] = RUN.v[w >> (code ^ 7)]; }
    else mpsCount[code] = static_cast<uint8_t>(1 << code);
  }
  uint8_t bit;
  if (mpsCount[code]) { bit = 0; mpsCount[code]--; }
  else { bit = 1; lpsIndex[code] = false; }
  endOfRun = !mpsCount[code] && !lpsIndex[code];
  return bit;
}

uint8_t SDD1::Decompressor::pemBit(const SDD1& c, int context) {
  const uint8_t st = status[context], m = mps[context];
  const Evolution& e = EVOLUTION[st];
  bool endOfRun;
  const uint8_t bit = bgBit(c, e.code, endOfRun);
  if (endOfRun) {
    if (bit) {
      if (!(st & 0xFE)) mps[context] ^= 1;
      status[context] = e.nextLps;
    } else {
      status[context] = e.nextMps;
    }
  }
  return bit ^ m;
}

uint8_t SDD1::Decompressor::cmBit(const SDD1& c) {
  switch (bitplanesInfo) {
    case 0x00: currentBitplane ^= 1; break;
    case 0x40:
      currentBitplane ^= 1;
      if (!(bitNumber & 0x7F)) currentBitplane = (currentBitplane + 2) & 7;
      break;
    case 0x80:
      currentBitplane ^= 1;
      if (!(bitNumber & 0x7F)) currentBitplane ^= 2;
      break;
    default: currentBitplane = bitNumber & 7; break;
  }
  uint16_t& bits = prevBits[currentBitplane & 7];
  int ctx = (currentBitplane & 1) << 4;
  switch (contextBitsInfo) {
    case 0x00: ctx |= ((bits & 0x01C0) >> 5) | (bits & 1); break;
    case 0x10: ctx |= ((bits & 0x0180) >> 5) | (bits & 1); break;
    case 0x20: ctx |= ((bits & 0x00C0) >> 5) | (bits & 1); break;
    default: ctx |= ((bits & 0x0180) >> 5) | (bits & 3); break;
  }
  const uint8_t bit = pemBit(c, ctx);
  bits = static_cast<uint16_t>((bits << 1) | bit);
  bitNumber++;
  return bit;
}

uint8_t SDD1::Decompressor::read(const SDD1& c) {
  if (bitplanesInfo == 0xC0) {
    // 8 planos de bits: um byte por vez, do bit 0 ao 7
    r1 = 0;
    for (int b = 0; b < 8; b++) if (cmBit(c)) r1 |= static_cast<uint8_t>(1 << b);
    return r1;
  }
  // 2, 4 ou 8 planos em pares: cada rodada dá dois bytes (planos par e ímpar), entregues um de cada vez
  if (r0 == 0) { r0 = 0xFF; return r2; }
  r1 = r2 = 0;
  for (int b = 7; b >= 0; b--) {
    if (cmBit(c)) r1 |= static_cast<uint8_t>(1 << b);
    if (cmBit(c)) r2 |= static_cast<uint8_t>(1 << b);
  }
  r0 = 0;
  return r1;
}

void SDD1::Decompressor::sync(StateIO& io) {
  io.sync(offset); io.sync(bitCount);
  io.raw(mpsCount, sizeof mpsCount);
  for (bool& b : lpsIndex) io.sync(b);
  io.raw(status, sizeof status); io.raw(mps, sizeof mps);
  io.sync(bitplanesInfo); io.sync(contextBitsInfo); io.sync(bitNumber); io.sync(currentBitplane);
  io.raw(prevBits, sizeof prevBits);
  io.sync(r0); io.sync(r1); io.sync(r2);
  if (io.reading()) {
    offset &= 0xFFFFFF;
    bitCount &= 7;
    for (auto& s : status) if (s > 32) s = 0;
    for (auto& m : mps) m &= 1;
    bitplanesInfo &= 0xC0;
    contextBitsInfo &= 0x30;
  }
}

void SDD1::sync(StateIO& io) {
  io.sync(r4800_); io.sync(r4801_); io.raw(mmc_, sizeof mmc_);
  io.raw(dmaAddr_, sizeof dmaAddr_); io.raw(dmaSize_, sizeof dmaSize_);
  io.sync(dmaReady_);
  dec_.sync(io);
  if (io.reading()) {
    for (auto& m : mmc_) m &= 0x8F;
    for (auto& a : dmaAddr_) a &= 0xFFFFFF;
  }
}

}  // namespace indigo::snes
