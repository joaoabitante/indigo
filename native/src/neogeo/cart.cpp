#include "cart.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <stdexcept>

#include "../core/zip.h"

namespace indigo::neogeo {

namespace {

// ------------------------------------------------------------------ tabela de pacotes (gerada)
struct SetRom {
  uint8_t area, flag;
  uint32_t crc, offset, size;
  int value;
  const char* name;
};
struct SetInfo {
  const char* name;
  const char* parent;
  const char* slot;
  const char* title;
  uint32_t size[7];
  int first, count;
};
enum { A_P = 0, A_S, A_M, A_MCRYPT, A_V1, A_V2, A_C };
enum { F_LOAD = 0, F_WSWAP, F_BYTE, F_CONT, F_FILL, F_IGNORE, F_LOAD32, F_LOAD32WS, F_WORD };
#include "sets.inc"
#include "cmc_tables.inc"

// bitswap como no MAME: o primeiro bit indicado vai para o bit mais alto do resultado
template <typename... B>
uint32_t bitswap(uint32_t v, B... bits) {
  const int b[] = {static_cast<int>(bits)...};
  const int n = static_cast<int>(sizeof...(bits));
  uint32_t r = 0;
  for (int i = 0; i < n; i++) r |= ((v >> b[i]) & 1u) << (n - 1 - i);
  return r;
}

// P ROM durante a decifração: bytes na ordem do arquivo .p1 (palavras em little-endian, como a memória do MAME
// num computador little-endian). Palavra i = endereço 2*i do 68000.
uint16_t wget(const std::vector<uint8_t>& p, size_t i) { return static_cast<uint16_t>(p[2 * i] | (p[2 * i + 1] << 8)); }
void wset(std::vector<uint8_t>& p, size_t i, uint16_t v) { p[2 * i] = static_cast<uint8_t>(v); p[2 * i + 1] = static_cast<uint8_t>(v >> 8); }

void need(const std::vector<uint8_t>& v, size_t n) {
  if (v.size() < n) throw std::runtime_error("Pacote do Neo Geo com tamanho inesperado.");
}

// ------------------------------------------------------------------ NEO-SMA (P ROM)
using SwapFn = uint32_t (*)(uint32_t);

void smaData(std::vector<uint8_t>& p, uint32_t (*f)(uint32_t)) {
  for (size_t i = 0; i < 0x800000 / 2; i++) wset(p, 0x100000 / 2 + i, static_cast<uint16_t>(f(wget(p, 0x100000 / 2 + i))));
}
void smaBanked(std::vector<uint8_t>& p, size_t len, size_t block, uint32_t (*f)(uint32_t)) {
  std::vector<uint16_t> buf(block / 2);
  for (size_t i = 0; i < len / 2; i += block / 2) {
    for (size_t j = 0; j < block / 2; j++) buf[j] = wget(p, 0x100000 / 2 + i + j);
    for (size_t j = 0; j < block / 2; j++) wset(p, 0x100000 / 2 + i + j, buf[f(static_cast<uint32_t>(j))]);
  }
}
void smaFixed(std::vector<uint8_t>& p, uint32_t base, uint32_t (*f)(uint32_t)) {
  for (uint32_t i = 0; i < 0x0C0000 / 2; i++) wset(p, i, wget(p, base / 2 + f(i)));
}

void smaDecrypt(int type, std::vector<uint8_t>& p) {
  need(p, 0x900000);
  switch (type) {
    case SMA_KOF99:
      smaData(p, [](uint32_t v) { return bitswap(v, 13, 7, 3, 0, 9, 4, 5, 6, 1, 12, 8, 14, 10, 11, 2, 15); });
      smaBanked(p, 0x600000, 0x800, [](uint32_t j) { return bitswap(j, 6, 2, 4, 9, 8, 3, 1, 7, 0, 5); });
      smaFixed(p, 0x700000, [](uint32_t i) { return bitswap(i, 18, 11, 6, 14, 17, 16, 5, 8, 10, 12, 0, 4, 3, 2, 7, 9, 15, 13, 1); });
      break;
    case SMA_GAROU:
      smaData(p, [](uint32_t v) { return bitswap(v, 13, 12, 14, 10, 8, 2, 3, 1, 5, 9, 11, 4, 15, 0, 6, 7); });
      smaFixed(p, 0x710000, [](uint32_t i) { return bitswap(i, 18, 4, 5, 16, 14, 7, 9, 6, 13, 17, 15, 3, 1, 2, 12, 11, 8, 10, 0); });
      smaBanked(p, 0x800000, 0x8000, [](uint32_t j) { return bitswap(j, 9, 4, 8, 3, 13, 6, 2, 7, 0, 12, 1, 11, 10, 5); });
      break;
    case SMA_GAROUH:
      smaData(p, [](uint32_t v) { return bitswap(v, 14, 5, 1, 11, 7, 4, 10, 15, 3, 12, 8, 13, 0, 2, 9, 6); });
      smaFixed(p, 0x7F8000, [](uint32_t i) { return bitswap(i, 18, 5, 16, 11, 2, 6, 7, 17, 3, 12, 8, 14, 4, 0, 9, 1, 10, 15, 13); });
      smaBanked(p, 0x800000, 0x8000, [](uint32_t j) { return bitswap(j, 12, 8, 1, 7, 11, 3, 13, 10, 6, 9, 5, 4, 0, 2); });
      break;
    case SMA_MSLUG3:
      smaData(p, [](uint32_t v) { return bitswap(v, 4, 11, 14, 3, 1, 13, 0, 7, 2, 8, 12, 15, 10, 9, 5, 6); });
      smaFixed(p, 0x5D0000, [](uint32_t i) { return bitswap(i, 18, 15, 2, 1, 13, 3, 0, 9, 6, 16, 4, 11, 5, 7, 12, 17, 14, 10, 8); });
      smaBanked(p, 0x800000, 0x10000, [](uint32_t j) { return bitswap(j, 2, 11, 0, 14, 6, 4, 13, 8, 9, 3, 10, 7, 5, 12, 1); });
      break;
    case SMA_MSLUG3A:
      smaData(p, [](uint32_t v) { return bitswap(v, 2, 11, 12, 14, 9, 3, 1, 4, 13, 7, 6, 8, 10, 15, 0, 5); });
      smaFixed(p, 0x5D0000, [](uint32_t i) { return bitswap(i, 18, 1, 16, 14, 7, 17, 5, 8, 4, 15, 6, 3, 2, 0, 13, 10, 12, 9, 11); });
      smaBanked(p, 0x800000, 0x10000, [](uint32_t j) { return bitswap(j, 12, 0, 11, 3, 4, 13, 6, 8, 14, 7, 5, 2, 10, 9, 1); });
      break;
    case SMA_KOF2000:
      smaData(p, [](uint32_t v) { return bitswap(v, 12, 8, 11, 3, 15, 14, 7, 0, 10, 13, 6, 5, 9, 2, 1, 4); });
      smaBanked(p, 0x63A000, 0x800, [](uint32_t j) { return bitswap(j, 4, 1, 3, 8, 6, 2, 7, 0, 9, 5); });
      smaFixed(p, 0x73A000, [](uint32_t i) { return bitswap(i, 18, 8, 4, 15, 13, 3, 14, 16, 2, 6, 17, 7, 12, 10, 0, 5, 11, 1, 9); });
      break;
    default: break;
  }
}

// ------------------------------------------------------------------ KOF98 (P ROM)
void kof98Decrypt(std::vector<uint8_t>& src) {
  need(src, 0x600000);
  std::vector<uint8_t> dst(src.begin(), src.begin() + 0x200000);
  static const uint32_t sec[] = {0x000000, 0x100000, 0x000004, 0x100004, 0x10000a, 0x00000a, 0x10000e, 0x00000e};
  static const uint32_t pos[] = {0x000, 0x004, 0x00a, 0x00e};
  auto cp = [&](size_t d, size_t s) { src[d] = dst[s]; src[d + 1] = dst[s + 1]; };
  for (uint32_t i = 0x800; i < 0x100000; i += 0x200) {
    for (uint32_t j = 0; j < 0x100; j += 0x10) {
      for (uint32_t k = 0; k < 16; k += 2) {
        cp(i + j + k, i + j + sec[k / 2] + 0x100);
        cp(i + j + k + 0x100, i + j + sec[k / 2]);
      }
      if (i >= 0x080000 && i < 0x0c0000) {
        for (int k = 0; k < 4; k++) { cp(i + j + pos[k], i + j + pos[k]); cp(i + j + pos[k] + 0x100, i + j + pos[k] + 0x100); }
      } else if (i >= 0x0c0000) {
        for (int k = 0; k < 4; k++) { cp(i + j + pos[k], i + j + pos[k] + 0x100); cp(i + j + pos[k] + 0x100, i + j + pos[k]); }
      }
    }
    cp(i + 0x000000, i + 0x000000);
    cp(i + 0x000002, i + 0x100000);
    cp(i + 0x000100, i + 0x000100);
    cp(i + 0x000102, i + 0x100100);
  }
  std::memmove(&src[0x100000], &src[0x200000], 0x400000);
  src.resize(0x500000);
}

// ------------------------------------------------------------------ KOF2002 e parecidos (blocos de 512 KB)
void blockSwap(std::vector<uint8_t>& p, size_t base, const uint32_t* sec, int n) {
  need(p, base + static_cast<size_t>(n) * 0x80000);
  std::vector<uint8_t> d(p.begin() + static_cast<long>(base), p.begin() + static_cast<long>(base + n * 0x80000));
  for (int i = 0; i < n; i++) std::memcpy(&p[base + i * 0x80000], &d[sec[i]], 0x80000);
}

// ------------------------------------------------------------------ NEO-PVC (P ROM de Metal Slug 5, SVC, KOF2003)
struct PvcKey {
  uint8_t xor1[32], xor2[32];
  int wordSwap[16];    // bitswap das palavras do meio
  int fixedSwap[4];    // bitswap dos 4 bits de baixo do bloco de 64 KB da parte fixa
  uint32_t midXor;     // xor dos bits 8-11 dos blocos de 256 bytes
  int bankSwap[4];     // bitswap dos 4 bits de baixo dos bits 12-19
  int bankSwapHi[4];   // e dos 4 de cima
  bool p3;             // KOF2003: a P3 (0x800000) é misturada com a P2 e vira o banco 1
};

void pvcDecrypt(std::vector<uint8_t>& rom, const PvcKey& k) {
  const size_t size = k.p3 ? 0x900000 : 0x800000;
  need(rom, size);
  if (k.p3)
    for (size_t i = 0; i < 0x100000; i++) rom[0x800000 + i] ^= rom[0x100002 | i];
  for (size_t i = 0; i < 0x100000; i++) rom[i] ^= k.xor1[i % 0x20];
  for (size_t i = 0x100000; i < 0x800000; i++) rom[i] ^= k.xor2[i % 0x20];
  for (size_t i = 0x100000; i < 0x800000; i += 4) {
    uint32_t w = rom[i + 1] | (rom[i + 2] << 8);
    const int* s = k.wordSwap;
    w = bitswap(w, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15]);
    rom[i + 1] = static_cast<uint8_t>(w);
    rom[i + 2] = static_cast<uint8_t>(w >> 8);
  }
  std::vector<uint8_t> buf(rom.begin(), rom.begin() + static_cast<long>(size));
  std::vector<uint8_t>& out = k.p3 ? buf : rom;
  const std::vector<uint8_t>& in = k.p3 ? rom : buf;
  for (uint32_t i = 0; i < 0x10; i++) {
    const uint32_t ofst = (i & 0xF0) + bitswap(i & 0x0F, 7, 6, 5, 4, k.fixedSwap[0], k.fixedSwap[1], k.fixedSwap[2], k.fixedSwap[3]);
    std::memcpy(&out[i * 0x10000], &in[ofst * 0x10000], 0x10000);
  }
  for (uint32_t i = 0x100000; i < size; i += 0x100) {
    const uint32_t hi = (i & 0x0FF000) >> 12;
    const uint32_t ofst = (i & 0xF000FF) + ((i & 0x000F00) ^ k.midXor) +
                          (bitswap(hi, k.bankSwapHi[0], k.bankSwapHi[1], k.bankSwapHi[2], k.bankSwapHi[3], k.bankSwap[0],
                                   k.bankSwap[1], k.bankSwap[2], k.bankSwap[3]) << 12);
    std::memcpy(&out[i], &in[ofst], 0x100);
  }
  if (k.p3) {
    std::memcpy(&rom[0x000000], &buf[0x000000], 0x100000);
    std::memcpy(&rom[0x100000], &buf[0x800000], 0x100000);
    std::memcpy(&rom[0x200000], &buf[0x100000], 0x700000);
  } else {
    std::memcpy(&buf[0], rom.data(), size);
    std::memcpy(&rom[0x100000], &buf[0x700000], 0x100000);
    std::memcpy(&rom[0x200000], &buf[0x100000], 0x600000);
  }
}

const PvcKey PVC_MSLUG5 = {
    {0xc2, 0x4b, 0x74, 0xfd, 0x0b, 0x34, 0xeb, 0xd7, 0x10, 0x6d, 0xf9, 0xce, 0x5d, 0xd5, 0x61, 0x29,
     0xf5, 0xbe, 0x0d, 0x82, 0x72, 0x45, 0x0f, 0x24, 0xb3, 0x34, 0x1b, 0x99, 0xea, 0x09, 0xf3, 0x03},
    {0x36, 0x09, 0xb0, 0x64, 0x95, 0x0f, 0x90, 0x42, 0x6e, 0x0f, 0x30, 0xf6, 0xe5, 0x08, 0x30, 0x64,
     0x08, 0x04, 0x00, 0x2f, 0x72, 0x09, 0xa0, 0x13, 0xc9, 0x0b, 0xa0, 0x3e, 0xc2, 0x00, 0x40, 0x2b},
    {15, 14, 13, 12, 10, 11, 8, 9, 6, 7, 4, 5, 3, 2, 1, 0}, {1, 0, 3, 2}, 0x700, {1, 0, 3, 2}, {5, 4, 7, 6}, false};
const PvcKey PVC_SVC = {
    {0x3b, 0x6a, 0xf7, 0xb7, 0xe8, 0xa9, 0x20, 0x99, 0x9f, 0x39, 0x34, 0x0c, 0xc3, 0x9a, 0xa5, 0xc8,
     0xb8, 0x18, 0xce, 0x56, 0x94, 0x44, 0xe3, 0x7a, 0xf7, 0xdd, 0x42, 0xf0, 0x18, 0x60, 0x92, 0x9f},
    {0x69, 0x0b, 0x60, 0xd6, 0x4f, 0x01, 0x40, 0x1a, 0x9f, 0x0b, 0xf0, 0x75, 0x58, 0x0e, 0x60, 0xb4,
     0x14, 0x04, 0x20, 0xe4, 0xb9, 0x0d, 0x10, 0x89, 0xeb, 0x07, 0x30, 0x90, 0x50, 0x0e, 0x20, 0x26},
    {15, 14, 13, 12, 10, 11, 8, 9, 6, 7, 4, 5, 3, 2, 1, 0}, {2, 3, 0, 1}, 0xA00, {1, 0, 3, 2}, {4, 5, 6, 7}, false};
const PvcKey PVC_KOF2003 = {
    {0x3b, 0x6a, 0xf7, 0xb7, 0xe8, 0xa9, 0x20, 0x99, 0x9f, 0x39, 0x34, 0x0c, 0xc3, 0x9a, 0xa5, 0xc8,
     0xb8, 0x18, 0xce, 0x56, 0x94, 0x44, 0xe3, 0x7a, 0xf7, 0xdd, 0x42, 0xf0, 0x18, 0x60, 0x92, 0x9f},
    {0x2f, 0x02, 0x60, 0xbb, 0x77, 0x01, 0x30, 0x08, 0xd8, 0x01, 0xa0, 0xdf, 0x37, 0x0a, 0xf0, 0x65,
     0x28, 0x03, 0xd0, 0x23, 0xd3, 0x03, 0x70, 0x42, 0xbb, 0x06, 0xf0, 0x28, 0xba, 0x0f, 0xf0, 0x7a},
    {15, 14, 13, 12, 5, 4, 7, 6, 9, 8, 11, 10, 3, 2, 1, 0}, {0, 1, 2, 3}, 0x800, {1, 0, 3, 2}, {4, 5, 6, 7}, true};
const PvcKey PVC_KOF2003H = {
    {0xc2, 0x4b, 0x74, 0xfd, 0x0b, 0x34, 0xeb, 0xd7, 0x10, 0x6d, 0xf9, 0xce, 0x5d, 0xd5, 0x61, 0x29,
     0xf5, 0xbe, 0x0d, 0x82, 0x72, 0x45, 0x0f, 0x24, 0xb3, 0x34, 0x1b, 0x99, 0xea, 0x09, 0xf3, 0x03},
    {0x2b, 0x09, 0xd0, 0x7f, 0x51, 0x0b, 0x10, 0x4c, 0x5b, 0x07, 0x70, 0x9d, 0x3e, 0x0b, 0xb0, 0xb6,
     0x54, 0x09, 0xe0, 0xcc, 0x3d, 0x0d, 0x80, 0x99, 0x87, 0x03, 0x90, 0x82, 0xfe, 0x04, 0x20, 0x18},
    {15, 14, 13, 12, 10, 11, 8, 9, 6, 7, 4, 5, 3, 2, 1, 0}, {1, 0, 3, 2}, 0x400, {0, 1, 2, 3}, {6, 7, 4, 5}, true};

// ------------------------------------------------------------------ NEO-CMC (gráficos, camada fixa e M1)
struct CmcTables {
  const uint8_t *t0_03, *t0_12, *t1_03, *t1_12, *a8_15_1, *a8_15_2, *a16_23_1, *a16_23_2, *a0_7;
};
const CmcTables CMC42 = {KOF99_TYPE0_T03, KOF99_TYPE0_T12, KOF99_TYPE1_T03, KOF99_TYPE1_T12,
                         KOF99_ADDRESS_8_15_XOR1, KOF99_ADDRESS_8_15_XOR2, KOF99_ADDRESS_16_23_XOR1,
                         KOF99_ADDRESS_16_23_XOR2, KOF99_ADDRESS_0_7_XOR};
const CmcTables CMC50 = {KOF2000_TYPE0_T03, KOF2000_TYPE0_T12, KOF2000_TYPE1_T03, KOF2000_TYPE1_T12,
                         KOF2000_ADDRESS_8_15_XOR1, KOF2000_ADDRESS_8_15_XOR2, KOF2000_ADDRESS_16_23_XOR1,
                         KOF2000_ADDRESS_16_23_XOR2, KOF2000_ADDRESS_0_7_XOR};

void cmcPair(const CmcTables& t, uint8_t* r0, uint8_t* r1, uint8_t c0, uint8_t c1, const uint8_t* hi,
             const uint8_t* lo, const uint8_t* t1, uint32_t base, bool invert) {
  const uint8_t tmp = t1[(base & 0xFF) ^ t.a0_7[(base >> 8) & 0xFF]];
  const uint8_t x0 = static_cast<uint8_t>((hi[(base >> 8) & 0xFF] & 0xFE) | (tmp & 1));
  const uint8_t x1 = static_cast<uint8_t>((tmp & 0xFE) | (lo[(base >> 8) & 0xFF] & 1));
  if (invert) { *r0 = c1 ^ x0; *r1 = c0 ^ x1; }
  else { *r0 = c0 ^ x0; *r1 = c1 ^ x1; }
}

void cmcGfx(std::vector<uint8_t>& rom, const CmcTables& t, uint32_t extraXor) {
  const size_t size = rom.size() & ~size_t(3);
  if (size < 4) return;
  std::vector<uint8_t> buf(size);
  for (uint32_t r = 0; r < size / 4; r++) {
    cmcPair(t, &buf[4 * r + 0], &buf[4 * r + 3], rom[4 * r + 0], rom[4 * r + 3], t.t0_03, t.t0_12, t.t1_03, r, (r >> 8) & 1);
    cmcPair(t, &buf[4 * r + 1], &buf[4 * r + 2], rom[4 * r + 1], rom[4 * r + 2], t.t0_12, t.t0_03, t.t1_12, r,
            ((r >> 16) ^ t.a16_23_2[(r >> 8) & 0xFF]) & 1);
  }
  for (uint32_t r = 0; r < size / 4; r++) {
    uint32_t b = r ^ extraXor;
    b ^= t.a8_15_1[(b >> 16) & 0xFF] << 8;
    b ^= t.a8_15_2[b & 0xFF] << 8;
    b ^= t.a16_23_1[b & 0xFF] << 16;
    b ^= t.a16_23_2[(b >> 8) & 0xFF] << 16;
    b ^= t.a0_7[(b >> 8) & 0xFF];
    if (size == 0x3000000) {  // Prehistoric Isle 2
      b = r < 0x2000000 / 4 ? (b & (0x2000000 / 4 - 1)) : 0x2000000 / 4 + (b & (0x1000000 / 4 - 1));
    } else if (size == 0x6000000) {
      b = r < 0x4000000 / 4 ? (b & (0x4000000 / 4 - 1)) : 0x4000000 / 4 + (b & (0x1000000 / 4 - 1));
    } else {
      uint32_t m = 1;
      while (m < size / 4) m <<= 1;
      b &= m - 1;
      if (b >= size / 4) b %= static_cast<uint32_t>(size / 4);
    }
    std::memcpy(&rom[4 * r], &buf[4 * b], 4);
  }
}

// a camada fixa dos cartuchos com CMC fica no fim da C ROM
void sfixFromC(const std::vector<uint8_t>& c, std::vector<uint8_t>& s, size_t size) {
  if (c.size() < size) return;
  s.assign(size, 0);
  const uint8_t* src = c.data() + c.size() - size;
  for (size_t i = 0; i < size; i++) s[i] = src[(i & ~size_t(0x1F)) + ((i & 7) << 2) + ((~i & 8) >> 2) + ((i & 0x10) >> 4)];
}

int m1Scramble(int address, uint16_t key) {
  static const int p1[8][16] = {
      {15, 14, 10, 7, 1, 2, 3, 8, 0, 12, 11, 13, 6, 9, 5, 4}, {7, 1, 8, 11, 15, 9, 2, 3, 5, 13, 4, 14, 10, 0, 6, 12},
      {8, 6, 14, 3, 10, 7, 15, 1, 4, 0, 2, 5, 13, 11, 12, 9}, {2, 8, 15, 9, 3, 4, 11, 7, 13, 6, 0, 10, 1, 12, 14, 5},
      {1, 13, 6, 15, 14, 3, 8, 10, 9, 4, 7, 12, 5, 2, 0, 11}, {11, 15, 3, 4, 7, 0, 9, 2, 6, 14, 12, 1, 8, 5, 10, 13},
      {10, 5, 13, 8, 6, 15, 1, 14, 11, 9, 3, 0, 12, 7, 4, 2}, {9, 3, 7, 0, 2, 12, 4, 11, 14, 10, 5, 8, 15, 13, 1, 6}};
  const int block = (address >> 16) & 7;
  uint32_t aux = address & 0xFFFF;
  aux ^= bitswap(key, 12, 0, 2, 4, 8, 15, 7, 13, 10, 1, 3, 6, 11, 9, 14, 5);
  const int* b = p1[block];
  aux = bitswap(aux, b[15], b[14], b[13], b[12], b[11], b[10], b[9], b[8], b[7], b[6], b[5], b[4], b[3], b[2], b[1], b[0]);
  aux ^= M1_ADDRESS_0_7_XOR[(aux >> 8) & 0xFF];
  aux ^= M1_ADDRESS_8_15_XOR[aux & 0xFF] << 8;
  aux = bitswap(aux, 7, 15, 14, 6, 5, 13, 12, 4, 11, 3, 10, 2, 9, 1, 8, 0);
  return (block << 16) | static_cast<int>(aux);
}

std::vector<uint8_t> cmc50M1(std::vector<uint8_t> rom) {
  rom.resize(0x80000, 0);
  uint16_t key = 0;  // o CMC50 usa a soma dos primeiros 64 KB como chave
  for (size_t i = 0; i < 0x10000; i++) key = static_cast<uint16_t>(key + rom[i]);
  std::vector<uint8_t> out(0x80000);
  for (int i = 0; i < 0x80000; i++) out[i] = rom[m1Scramble(i, key) & 0x7FFFF];
  return out;
}

// ------------------------------------------------------------------ NEO-PCM2 (V ROM)
void pcm2Decrypt(std::vector<uint8_t>& v, int value) {
  const size_t words = v.size() / 2, block = static_cast<size_t>(value) / 2;
  std::vector<uint16_t> buf(block);
  for (size_t i = 0; i + block <= words; i += block) {
    for (size_t j = 0; j < block; j++) buf[j] = static_cast<uint16_t>(v[2 * (i + j)] | (v[2 * (i + j) + 1] << 8));
    for (size_t j = 0; j < block; j++) {
      const uint16_t w = buf[j ^ (static_cast<size_t>(value) / 4)];
      v[2 * (i + j)] = static_cast<uint8_t>(w);
      v[2 * (i + j) + 1] = static_cast<uint8_t>(w >> 8);
    }
  }
}

void pcm2Swap(std::vector<uint8_t>& v, int value) {
  static const uint32_t addrs[7][2] = {{0x000000, 0xa5000}, {0xffce20, 0x01000}, {0xfe2cf6, 0x4e001}, {0xffac28, 0xc2000},
                                       {0xfeb2c0, 0x0a000}, {0xff14ea, 0xa7001}, {0xffb440, 0x02000}};
  static const uint8_t xordata[7][8] = {{0xf9, 0xe0, 0x5d, 0xf3, 0xea, 0x92, 0xbe, 0xef}, {0xc4, 0x83, 0xa8, 0x5f, 0x21, 0x27, 0x64, 0xaf},
                                        {0xc3, 0xfd, 0x81, 0xac, 0x6d, 0xe7, 0xbf, 0x9e}, {0xc3, 0xfd, 0x81, 0xac, 0x6d, 0xe7, 0xbf, 0x9e},
                                        {0xcb, 0x29, 0x7d, 0x43, 0xd2, 0x3a, 0xc2, 0xb4}, {0x4b, 0xa4, 0x63, 0x46, 0xf0, 0x91, 0xea, 0x62},
                                        {0x4b, 0xa4, 0x63, 0x46, 0xf0, 0x91, 0xea, 0x62}};
  need(v, 0x1000000);
  std::vector<uint8_t> buf(v.begin(), v.begin() + 0x1000000);
  for (uint32_t i = 0; i < 0x1000000; i++) {
    uint32_t j = bitswap(i, 23, 22, 21, 20, 19, 18, 17, 0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 16);
    j ^= addrs[value][1];
    const uint32_t d = (i + addrs[value][0]) & 0xFFFFFF;
    v[j & 0xFFFFFF] = buf[d] ^ xordata[value][j & 7];
  }
}

// ------------------------------------------------------------------ o que cada tipo de cartucho faz
struct SlotOps {
  const char* slot;
  int board, sma, fixBank;
  int pDecrypt;      // 0 nada, 1 SMA, 2 KOF98, 3 KOF2002/Matrimelee, 4 Samurai Shodown V, 5 SS V Special,
                     // 6 PVC Metal Slug 5, 7 PVC SVC, 8 PVC KOF2003, 9 PVC KOF2003 (AES)
  int cmc;           // 0 nenhum, 42 ou 50
  int gfxKey;
  bool sfix;         // camada fixa no fim da C ROM
  int pcm2, pcm2Swap;  // PCM2: valor da troca de endereços (0 = nada); versão nova: tabela (-1 = nada)
};
const SlotOps SLOTS[] = {
    {"rom", BOARD_DEFAULT, 0, 0, 0, 0, 0, false, 0, -1},
    {"rom_fatfur2", BOARD_CT0, 0, 0, 0, 0, 0, false, 0, -1},
    {"rom_kof98", BOARD_KOF98, 0, 0, 2, 0, 0, false, 0, -1},
    {"rom_mslugx", BOARD_MSLUGX, 0, 0, 0, 0, 0, false, 0, -1},
    {"rom_vliner", BOARD_RAM8K, 0, 0, 0, 0, 0, false, 0, -1},
    {"cmc42_zupapa", BOARD_DEFAULT, 0, 1, 0, 42, 0xbd, true, 0, -1},
    {"cmc42_mslug3h", BOARD_DEFAULT, 0, 1, 0, 42, 0xad, true, 0, -1},
    {"cmc42_ganryu", BOARD_DEFAULT, 0, 1, 0, 42, 0x07, true, 0, -1},
    {"cmc42_s1945p", BOARD_DEFAULT, 0, 1, 0, 42, 0x05, true, 0, -1},
    {"cmc42_preisle2", BOARD_DEFAULT, 0, 1, 0, 42, 0x9f, true, 0, -1},
    {"cmc42_bangbead", BOARD_DEFAULT, 0, 1, 0, 42, 0xf8, true, 0, -1},
    {"cmc42_nitd", BOARD_DEFAULT, 0, 1, 0, 42, 0xff, true, 0, -1},
    {"cmc42_sengoku3", BOARD_DEFAULT, 0, 1, 0, 42, 0xfe, true, 0, -1},
    {"cmc42_kof99k", BOARD_DEFAULT, 0, 1, 0, 42, 0x00, true, 0, -1},
    {"cmc50_kof2001", BOARD_DEFAULT, 0, 1, 0, 50, 0x1e, true, 0, -1},
    {"cmc50_kof2000n", BOARD_DEFAULT, 0, 2, 0, 50, 0x00, true, 0, -1},
    {"cmc50_jockeygp", BOARD_RAM8K, 0, 1, 0, 50, 0xac, true, 0, -1},
    {"sma_kof99", BOARD_SMA, SMA_KOF99, 1, 1, 42, 0x00, true, 0, -1},
    {"sma_garou", BOARD_SMA, SMA_GAROU, 1, 1, 42, 0x06, true, 0, -1},
    {"sma_garouh", BOARD_SMA, SMA_GAROUH, 1, 1, 42, 0x06, true, 0, -1},
    {"sma_mslug3", BOARD_SMA, SMA_MSLUG3, 1, 1, 42, 0xad, true, 0, -1},
    {"sma_mslug3a", BOARD_SMA, SMA_MSLUG3A, 1, 1, 42, 0xad, true, 0, -1},
    {"sma_kof2k", BOARD_SMA, SMA_KOF2000, 2, 1, 50, 0x00, true, 0, -1},
    {"pcm2_mslug4", BOARD_DEFAULT, 0, 1, 0, 50, 0x31, true, 8, -1},
    {"pcm2_rotd", BOARD_DEFAULT, 0, 1, 0, 50, 0x3f, true, 16, -1},
    {"pcm2_pnyaa", BOARD_DEFAULT, 0, 1, 0, 50, 0x2e, true, 4, -1},
    {"pcm2_ms4p", BOARD_DEFAULT, 0, 0, 0, 50, 0x31, false, 8, -1},
    {"k2k2_kof2k2", BOARD_DEFAULT, 0, 0, 3, 50, 0xec, true, 0, 0},
    {"k2k2_kf2k2p", BOARD_DEFAULT, 0, 0, 3, 50, 0xec, false, 0, 0},
    {"k2k2_matrim", BOARD_DEFAULT, 0, 2, 3, 50, 0x6a, true, 0, 1},
    {"k2k2_samsh5", BOARD_DEFAULT, 0, 1, 4, 50, 0x0f, true, 0, 4},
    {"k2k2_sams5s", BOARD_DEFAULT, 0, 1, 5, 50, 0x0d, true, 0, 6},
    {"pvc_mslug5", BOARD_PVC, 0, 1, 6, 50, 0x19, true, 0, 2},
    {"pvc_svc", BOARD_PVC, 0, 2, 7, 50, 0x57, true, 0, 3},
    {"pvc_kf2k3", BOARD_PVC, 0, 2, 8, 50, 0x9d, true, 0, 5},
    {"pvc_kf2k3h", BOARD_PVC, 0, 2, 9, 50, 0x9d, true, 0, 5},
};

const SlotOps* findSlot(const std::string& s) {
  for (const auto& o : SLOTS)
    if (s == o.slot) return &o;
  return nullptr;
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
std::string baseName(const std::string& n) {
  std::string b = n;
  const size_t sl = b.find_last_of("/\\");
  if (sl != std::string::npos) b = b.substr(sl + 1);
  const size_t dot = b.rfind('.');
  if (dot != std::string::npos) b = b.substr(0, dot);
  return lower(b);
}

void padPow2(std::vector<uint8_t>& v, size_t minSize) {
  size_t n = minSize;
  while (n < v.size()) n <<= 1;
  v.resize(n, 0);
}

// P em little-endian (ordem dos arquivos) -> big-endian para o 68000
void toBigEndian(std::vector<uint8_t>& p) {
  if (p.size() & 1) p.push_back(0xFF);
  for (size_t i = 0; i + 1 < p.size(); i += 2) std::swap(p[i], p[i + 1]);
}

// ------------------------------------------------------------------ montagem pelos dados da tabela
struct ZipIndex {
  std::vector<zip::Entry> entries;
  std::map<uint32_t, size_t> byCrc;
  std::map<std::string, size_t> byName;
};

ZipIndex indexZip(const std::vector<uint8_t>& data) {
  ZipIndex z;
  if (!zip::listEntries(data, z.entries)) throw std::runtime_error("Arquivo .zip danificado.");
  for (size_t i = 0; i < z.entries.size(); i++) {
    z.byCrc.emplace(z.entries[i].crc, i);
    z.byName.emplace(lower(z.entries[i].name), i);
  }
  return z;
}

int findFile(const ZipIndex& z, const SetRom& r) {
  if (r.crc) {
    auto it = z.byCrc.find(r.crc);
    if (it != z.byCrc.end()) return static_cast<int>(it->second);
  }
  auto it = z.byName.find(lower(r.name));
  if (it != z.byName.end() && (!r.crc || z.entries[it->second].size >= r.size)) return static_cast<int>(it->second);
  return -1;
}

bool isFileRom(const SetRom& r) { return r.flag != F_CONT && r.flag != F_FILL && r.flag != F_IGNORE && r.name[0]; }

Cart buildFromSet(const std::vector<uint8_t>& data, const ZipIndex& z, const SetInfo& set) {
  const SlotOps* ops = findSlot(set.slot);
  if (!ops)
    throw std::runtime_error(std::string("O jogo \"") + set.title + "\" (" + set.name +
                             ") usa uma placa pirata/proteção que o Índigo ainda não emula.");
  std::vector<uint8_t> area[7];
  for (int a = 0; a < 7; a++) area[a].assign(set.size[a], a == A_P ? 0xFF : 0);
  std::vector<uint8_t> file;
  size_t cursor = 0;
  int lastFlag = F_LOAD, lastFile = -2;
  for (int k = 0; k < set.count; k++) {
    const SetRom& r = SET_ROMS[set.first + k];
    std::vector<uint8_t>& reg = area[r.area];
    int flag = r.flag;
    if (flag == F_FILL) {
      for (uint32_t i = 0; i < r.size && r.offset + i < reg.size(); i++) reg[r.offset + i] = static_cast<uint8_t>(r.value);
      continue;
    }
    if (flag == F_IGNORE) { cursor += r.size; continue; }
    if (flag == F_CONT) {
      flag = lastFlag;
    } else {
      const int f = findFile(z, r);
      if (f < 0) throw std::runtime_error(std::string("Falta o arquivo ") + r.name + " no pacote.");
      if (f != lastFile) {
        if (!zip::extractEntry(data, z.entries[f].name, file)) throw std::runtime_error("Arquivo .zip danificado.");
        lastFile = f;
      }
      cursor = 0;
      lastFlag = flag;
    }
    const bool p = r.area == A_P;
    for (uint32_t i = 0; i < r.size; i++) {
      const size_t src = cursor + i;
      if (src >= file.size()) break;
      size_t dst;
      switch (flag) {
        case F_BYTE: dst = p ? ((r.offset + 2 * i) ^ 1) : r.offset + 2 * i; break;
        case F_LOAD32: dst = r.offset + 4 * i; break;
        case F_LOAD32WS: dst = r.offset + 4 * (i / 2) + ((i & 1) ^ 1); break;
        case F_WORD: dst = (r.offset + i) ^ 1; break;
        default: dst = r.offset + i; break;  // LOAD e LOAD16_WORD_SWAP (o arquivo já está na ordem da memória)
      }
      if (dst < reg.size()) reg[dst] = file[src];
    }
    cursor += r.size;
  }

  Cart c;
  c.setName = set.name;
  c.title = set.title;
  c.board = ops->board;
  c.smaType = ops->sma;
  c.fixBank = ops->fixBank;
  std::vector<uint8_t>& P = area[A_P];
  switch (ops->pDecrypt) {
    case 1: smaDecrypt(ops->sma, P); break;
    case 2: kof98Decrypt(P); break;
    case 3: {
      static const uint32_t sec[] = {0x100000, 0x280000, 0x300000, 0x180000, 0x000000, 0x380000, 0x200000, 0x080000};
      blockSwap(P, 0x100000, sec, 8);
      break;
    }
    case 4: {
      static const uint32_t sec[] = {0x000000, 0x080000, 0x700000, 0x680000, 0x500000, 0x180000, 0x200000, 0x480000,
                                     0x300000, 0x780000, 0x600000, 0x280000, 0x100000, 0x580000, 0x400000, 0x380000};
      blockSwap(P, 0, sec, 16);
      break;
    }
    case 5: {
      static const uint32_t sec[] = {0x000000, 0x080000, 0x500000, 0x480000, 0x600000, 0x580000, 0x700000, 0x280000,
                                     0x100000, 0x680000, 0x400000, 0x780000, 0x200000, 0x380000, 0x300000, 0x180000};
      blockSwap(P, 0, sec, 16);
      break;
    }
    case 6: pvcDecrypt(P, PVC_MSLUG5); break;
    case 7: pvcDecrypt(P, PVC_SVC); break;
    case 8: pvcDecrypt(P, PVC_KOF2003); break;
    case 9: pvcDecrypt(P, PVC_KOF2003H); break;
    default: break;
  }
  if (ops->pcm2) pcm2Decrypt(area[A_V1], ops->pcm2);
  if (ops->pcm2Swap >= 0) pcm2Swap(area[A_V1], ops->pcm2Swap);
  if (ops->cmc) cmcGfx(area[A_C], ops->cmc == 50 ? CMC50 : CMC42, static_cast<uint32_t>(ops->gfxKey));
  if (ops->sfix) sfixFromC(area[A_C], area[A_S], set.size[A_S] ? set.size[A_S] : 0x20000);
  if (!area[A_MCRYPT].empty()) area[A_M] = cmc50M1(area[A_MCRYPT]);
  toBigEndian(P);
  c.p = std::move(P);
  c.s = std::move(area[A_S]);
  c.m = std::move(area[A_M]);
  c.v1 = std::move(area[A_V1]);
  c.v2 = std::move(area[A_V2]);
  c.c = std::move(area[A_C]);
  return c;
}

// ------------------------------------------------------------------ pelos sufixos (homebrew e jogos fora da tabela)
// ".p1", ".c12"...: letra de `kinds` depois do ponto e número no fim do nome (já em minúsculas). Feito à mão,
// sem std::regex (nome enorme estourava a pilha) e sem exceção (número grande demais = não serve).
bool suffixPart(const std::string& n, const char* kinds, char& kind, int& num) {
  if (n.size() > 255) return false;
  size_t i = n.size();
  while (i > 0 && n[i - 1] >= '0' && n[i - 1] <= '9') i--;
  const size_t digits = n.size() - i;
  if (digits == 0 || digits > 6 || i < 2 || n[i - 2] != '.' || !n[i - 1] || !std::strchr(kinds, n[i - 1])) return false;
  kind = n[i - 1];
  num = 0;
  for (size_t k = i; k < n.size(); k++) num = num * 10 + (n[k] - '0');
  return true;
}

Cart buildBySuffix(const std::vector<uint8_t>& data, const ZipIndex& z, const std::string& nome) {
  std::map<char, std::vector<std::pair<int, size_t>>> parts;
  for (size_t i = 0; i < z.entries.size(); i++) {
    char k = 0;
    int num = 0;
    if (suffixPart(lower(z.entries[i].name), "psmvc", k, num)) parts[k].push_back({num, i});
  }
  if (parts['p'].empty() || (parts['c'].empty() && parts['s'].empty()))
    throw std::runtime_error("Pacote de Neo Geo desconhecido (sem os arquivos .p1/.c1).");
  auto get = [&](size_t i) {
    std::vector<uint8_t> d;
    if (!zip::extractEntry(data, z.entries[i].name, d)) throw std::runtime_error("Arquivo .zip danificado.");
    return d;
  };
  auto cat = [&](char k, std::vector<uint8_t>& out, int lo, int hi) {
    auto v = parts[k];
    std::sort(v.begin(), v.end());
    for (auto& [num, idx] : v) {
      if (num < lo || num > hi) continue;
      auto d = get(idx);
      out.insert(out.end(), d.begin(), d.end());
    }
  };
  Cart c;
  c.setName = baseName(nome);
  c.title = c.setName;
  cat('p', c.p, 0, 99);
  cat('s', c.s, 0, 99);
  cat('m', c.m, 0, 99);
  bool twoDigit = false;
  for (auto& [num, idx] : parts['v']) if (num >= 10) twoDigit = true;
  if (twoDigit) { cat('v', c.v1, 10, 19); cat('v', c.v2, 20, 29); }
  else cat('v', c.v1, 0, 99);
  auto cs = parts['c'];
  std::sort(cs.begin(), cs.end());
  for (size_t i = 0; i + 1 < cs.size(); i += 2) {
    const auto a = get(cs[i].second), b = get(cs[i + 1].second);
    const size_t n = std::min(a.size(), b.size()), o = c.c.size();
    c.c.resize(o + 2 * n);
    for (size_t k = 0; k < n; k++) { c.c[o + 2 * k] = a[k]; c.c[o + 2 * k + 1] = b[k]; }
  }
  toBigEndian(c.p);
  return c;
}

void finish(Cart& c) {
  if (c.p.size() < 0x80) throw std::runtime_error("Jogo de Neo Geo sem programa (P ROM).");
  if (c.p.size() > 0x1000000) c.p.resize(0x1000000);
  padPow2(c.s, 0x20000);
  if (c.m.empty()) c.m.assign(0x20000, 0xFF);
  padPow2(c.m, 0x800);
  if (c.c.size() & 127) c.c.resize((c.c.size() + 127) & ~size_t(127), 0);
  // nome do jogo no cabeçalho do programa ("NEO-GEO" em 0x100, NGH em 0x108)
  if (c.p.size() >= 0x10A) c.ngh = (c.p[0x108] << 8) | c.p[0x109];
}

}  // namespace

// ------------------------------------------------------------------ NEO-SMA: endereços e bancos (MAME/Geolith)
const SmaInfo SMA_INFO[6] = {
    {{0x2FFFF8, 0x2FFFFA}, 0x2FFFF0, {14, 6, 8, 10, 12, 5},
     {0x000000, 0x100000, 0x200000, 0x300000, 0x3cc000, 0x4cc000, 0x3f2000, 0x4f2000, 0x407800, 0x507800, 0x40d000,
      0x50d000, 0x417800, 0x517800, 0x420800, 0x520800, 0x424800, 0x524800, 0x429000, 0x529000, 0x42e800, 0x52e800,
      0x431800, 0x531800, 0x54d000, 0x551000, 0x567000, 0x592800, 0x588800, 0x581800, 0x599800, 0x594800, 0x598000}},
    {{0x2FFFCC, 0x2FFFF0}, 0x2FFFC0, {5, 9, 7, 6, 14, 12},
     {0x000000, 0x100000, 0x200000, 0x300000, 0x280000, 0x380000, 0x2d0000, 0x3d0000, 0x2f0000, 0x3f0000, 0x400000,
      0x500000, 0x420000, 0x520000, 0x440000, 0x540000, 0x498000, 0x598000, 0x4a0000, 0x5a0000, 0x4a8000, 0x5a8000,
      0x4b0000, 0x5b0000, 0x4b8000, 0x5b8000, 0x4c0000, 0x5c0000, 0x4c8000, 0x5c8000, 0x4d0000, 0x5d0000, 0x458000,
      0x558000, 0x460000, 0x560000, 0x468000, 0x568000, 0x470000, 0x570000, 0x478000, 0x578000, 0x480000, 0x580000,
      0x488000, 0x588000, 0x490000, 0x590000, 0x5d0000, 0x5d8000, 0x5e0000, 0x5e8000, 0x5f0000, 0x5f8000, 0x600000}},
    {{0x2FFFCC, 0x2FFFF0}, 0x2FFFC0, {4, 8, 14, 2, 11, 13},
     {0x000000, 0x100000, 0x200000, 0x300000, 0x280000, 0x380000, 0x2d0000, 0x3d0000, 0x2c8000, 0x3c8000, 0x400000,
      0x500000, 0x420000, 0x520000, 0x440000, 0x540000, 0x598000, 0x698000, 0x5a0000, 0x6a0000, 0x5a8000, 0x6a8000,
      0x5b0000, 0x6b0000, 0x5b8000, 0x6b8000, 0x5c0000, 0x6c0000, 0x5c8000, 0x6c8000, 0x5d0000, 0x6d0000, 0x458000,
      0x558000, 0x460000, 0x560000, 0x468000, 0x568000, 0x470000, 0x570000, 0x478000, 0x578000, 0x480000, 0x580000,
      0x488000, 0x588000, 0x490000, 0x590000, 0x5d8000, 0x6d8000, 0x5e0000, 0x6e0000, 0x5e8000, 0x6e8000, 0x6e8000}},
    {{0x2FFFF8, 0x2FFFFA}, 0x2FFFE4, {14, 12, 15, 6, 3, 9},
     {0x000000, 0x020000, 0x040000, 0x060000, 0x070000, 0x090000, 0x0b0000, 0x0d0000, 0x0e0000, 0x0f0000, 0x120000,
      0x130000, 0x140000, 0x150000, 0x180000, 0x190000, 0x1a0000, 0x1b0000, 0x1e0000, 0x1f0000, 0x200000, 0x210000,
      0x240000, 0x250000, 0x260000, 0x270000, 0x2a0000, 0x2b0000, 0x2c0000, 0x2d0000, 0x300000, 0x310000, 0x320000,
      0x330000, 0x360000, 0x370000, 0x380000, 0x390000, 0x3c0000, 0x3d0000, 0x400000, 0x410000, 0x440000, 0x450000,
      0x460000, 0x470000, 0x4a0000, 0x4b0000, 0x4c0000}},
    {{0x2FFFF8, 0x2FFFFA}, 0x2FFFE4, {15, 3, 1, 6, 12, 11},
     {0x000000, 0x030000, 0x040000, 0x070000, 0x080000, 0x0a0000, 0x0c0000, 0x0e0000, 0x0f0000, 0x100000, 0x130000,
      0x140000, 0x150000, 0x160000, 0x190000, 0x1a0000, 0x1b0000, 0x1c0000, 0x1f0000, 0x200000, 0x210000, 0x220000,
      0x250000, 0x260000, 0x270000, 0x280000, 0x2b0000, 0x2c0000, 0x2d0000, 0x2e0000, 0x310000, 0x320000, 0x330000,
      0x340000, 0x370000, 0x380000, 0x390000, 0x3a0000, 0x3d0000, 0x3e0000, 0x400000, 0x410000, 0x440000, 0x450000,
      0x460000, 0x470000, 0x4a0000, 0x4b0000, 0x4c0000}},
    {{0x2FFFD8, 0x2FFFDA}, 0x2FFFEC, {15, 14, 7, 3, 10, 5},
     {0x000000, 0x100000, 0x200000, 0x300000, 0x3f7800, 0x4f7800, 0x3ff800, 0x4ff800, 0x407800, 0x507800, 0x40f800,
      0x50f800, 0x416800, 0x516800, 0x41d800, 0x51d800, 0x424000, 0x524000, 0x523800, 0x623800, 0x526000, 0x626000,
      0x528000, 0x628000, 0x52a000, 0x62a000, 0x52b800, 0x62b800, 0x52d000, 0x62d000, 0x52e800, 0x62e800, 0x618000,
      0x619000, 0x61a000, 0x61a800}},
};

bool isNeoFile(const std::vector<uint8_t>& d) {
  return d.size() >= 4096 && d[0] == 'N' && d[1] == 'E' && d[2] == 'O' && d[3] == 1;
}

Cart loadNeo(const std::vector<uint8_t>& d) {
  if (!isNeoFile(d)) throw std::runtime_error("Arquivo .neo inválido.");
  auto le32 = [&](size_t a) { return static_cast<uint32_t>(d[a] | (d[a + 1] << 8) | (d[a + 2] << 16) | (d[a + 3] << 24)); };
  const uint32_t ps = le32(4), ss = le32(8), ms = le32(12), v1s = le32(16), v2s = le32(20), cs = le32(24);
  uint64_t total = 4096;
  for (uint32_t s : {ps, ss, ms, v1s, v2s, cs}) total += s;
  if (total > d.size() || ps > 0x1000000 || ss > 0x100000 || ms > 0x100000 || cs > 0x6000000)
    throw std::runtime_error("Arquivo .neo truncado ou danificado.");
  Cart c;
  size_t o = 4096;
  auto take = [&](uint32_t n) {
    std::vector<uint8_t> v(d.begin() + static_cast<long>(o), d.begin() + static_cast<long>(o + n));
    o += n;
    return v;
  };
  c.p = take(ps);
  c.s = take(ss);
  c.m = take(ms);
  c.v1 = take(v1s);
  c.v2 = take(v2s);
  c.c = take(cs);
  toBigEndian(c.p);
  c.ngh = le32(40);
  std::string name(reinterpret_cast<const char*>(&d[44]), 33);
  name = name.substr(0, name.find('\0'));
  while (!name.empty() && name.back() == ' ') name.pop_back();
  c.title = name;
  c.setName = name;
  // placas especiais pelo NGH (como o Geolith: o .neo já vem decifrado, mas a proteção em tempo de execução não)
  const auto pbyte = [&](size_t a) -> int { return a < c.p.size() ? c.p[a ^ 1] : -1; };  // ordem do arquivo
  switch (c.ngh) {
    case 0x047: case 0x052: c.board = BOARD_CT0; break;
    case 0x242: c.board = BOARD_KOF98; break;
    case 0x250: c.board = BOARD_MSLUGX; break;
    case 0x151: case 0x251:
      if (ps > 0x500000) { c.board = BOARD_SMA; c.smaType = SMA_KOF99; }
      break;
    case 0x253:
      if (pbyte(0xC0000 + 0x3E481) == 0x9F) { c.board = BOARD_SMA; c.smaType = SMA_GAROUH; c.fixBank = 1; }
      else if (pbyte(0xC0000 + 0x3E481) == 0x41) { c.board = BOARD_SMA; c.smaType = SMA_GAROU; c.fixBank = 1; }
      break;
    case 0x256:
      c.fixBank = 1;
      if (ps > 0x500000) { c.board = BOARD_SMA; c.smaType = pbyte(0x141) == 0x33 ? SMA_MSLUG3A : SMA_MSLUG3; }
      break;
    case 0x257:
      c.fixBank = 2;
      if (ps > 0x500000) { c.board = BOARD_SMA; c.smaType = SMA_KOF2000; }
      break;
    case 0x263: if (pbyte(0x809) != 0x0C) c.fixBank = 1; break;
    case 0x266: c.fixBank = 2; break;
    case 0x268: if (pbyte(0x267) == 0x4F) c.board = BOARD_PVC; break;
    case 0x269:
      if (pbyte(0x3D25) == 0xC4) c.fixBank = 2;
      if (pbyte(0x2F8F) == 0xC0) c.board = BOARD_PVC;
      break;
    case 0x271:
      if (pbyte(0x689) != 0x10 && pbyte(0xC1) != 0x02) { c.fixBank = 2; c.board = BOARD_PVC; }
      break;
    case 0x008: case 0x3E7: case 0x999: c.board = BOARD_RAM8K; break;
    default: break;
  }
  if (c.fixBank == 0 && c.s.size() > 0x20000) c.fixBank = 1;
  // o NGH acima vem do cabeçalho do .neo; o do programa (0x108) fica em c.ngh depois de finish()
  const uint32_t neoNgh = c.ngh;
  finish(c);
  c.ngh = neoNgh;
  return c;
}

Cart loadZipSet(const std::vector<uint8_t>& data, const std::string& nome) {
  const ZipIndex z = indexZip(data);
  const std::string base = baseName(nome);
  // escolhe o pacote: o do nome do arquivo, se completo; senão o completo com mais arquivos achados
  const int nsets = static_cast<int>(sizeof SET_INFO / sizeof SET_INFO[0]);
  int named = -1, best = -1, bestFound = 0, partial = -1, partialFound = 0;
  for (int i = 0; i < nsets; i++) {
    const SetInfo& s = SET_INFO[i];
    int found = 0, total = 0;
    for (int k = 0; k < s.count; k++) {
      const SetRom& r = SET_ROMS[s.first + k];
      if (!isFileRom(r)) continue;
      total++;
      if (findFile(z, r) >= 0) found++;
    }
    const bool isNamed = base == s.name;
    if (isNamed) named = i;
    if (total && found == total) {
      const bool bestNamed = best >= 0 && base == SET_INFO[best].name;
      // prioridade: o pacote com o nome do arquivo; depois o que usa mais arquivos; no empate, o pai
      if (best < 0 || isNamed || (!bestNamed && (found > bestFound || (found == bestFound && !s.parent[0])))) {
        best = i;
        bestFound = found;
      }
    } else if (found * 2 > total && found > partialFound) {
      partial = i;
      partialFound = found;
    }
  }
  Cart c;
  if (best >= 0) {
    c = buildFromSet(data, z, SET_INFO[best]);
  } else if (named >= 0 || partial >= 0) {
    const SetInfo& s = SET_INFO[named >= 0 ? named : partial];
    std::string falta;
    int n = 0;
    for (int k = 0; k < s.count; k++) {
      const SetRom& r = SET_ROMS[s.first + k];
      if (isFileRom(r) && findFile(z, r) < 0) {
        if (n < 4) falta += (n ? ", " : " ") + std::string(r.name);
        n++;
      }
    }
    std::string msg = "O pacote " + std::string(s.name) + " está incompleto: faltam " + std::to_string(n) +
                      " arquivo(s) (" + falta.substr(1) + (n > 4 ? "..." : "") + ").";
    if (s.parent[0])
      msg += std::string(" É um clone de ") + s.parent + ": use o pacote completo (\"non-merged\" ou \"merged\").";
    throw std::runtime_error(msg);
  } else {
    c = buildBySuffix(data, z, nome);
  }
  finish(c);
  return c;
}

bool looksLikeNeoGeoZip(const std::vector<uint8_t>& data, const std::string& nome) {
  std::vector<zip::Entry> e;
  if (!zip::isZip(data) || !zip::listEntries(data, e)) return false;
  const std::string base = baseName(nome);
  if (base == "neogeo" || base == "aes") return false;  // BIOS, não jogo
  std::map<uint32_t, int> crcs;
  for (auto& x : e) crcs[x.crc]++;
  for (const auto& r : SET_ROMS)
    if (r.crc && r.area == A_P && crcs.count(r.crc)) return true;
  for (const auto& s : SET_INFO)
    if (base == s.name) return true;
  bool p = false, c = false;
  for (auto& x : e) {
    const std::string n = lower(x.name);
    char k = 0;
    int num = 0;
    if (suffixPart(n, "p", k, num)) p = true;
    if (suffixPart(n, "cs", k, num)) c = true;
  }
  return p && c;
}

}  // namespace indigo::neogeo
