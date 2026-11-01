// Decifração do programa do CPS2. Algoritmo descoberto por Andreas Naive (redes de Feistel de 4 rodadas, com
// caixas-S de 6 entradas e 2 saídas), documentado publicamente e implementado no MAME por Nicola Salmoria
// (capcom/cps2crypt.cpp, BSD-3-Clause; dados extraídos do hardware por Charles MacDonald e Razoola).
// As tabelas abaixo (caixas-S e permutações de bits) são os dados desse trabalho; o código é próprio.
//
//   D = FN2(E, K xor EX(FN1(A, K)))
// E = palavra cifrada, A = 16 bits de baixo do endereço da palavra, K = chave de 64 bits, D = palavra decifrada.
// Só as buscas de instrução (espaço de programa) passam pela decifração; os dados são lidos em claro.
#include "cps2crypt.h"

namespace indigo::cps {

namespace {

struct SBox {
  uint8_t table[64];
  int8_t inputs[6];   // bits de entrada (-1 = só a chave entra)
  int8_t outputs[2];  // bits de saída
};

const SBox kFn1R1[4] = {
    {{0,2,2,0,1,0,1,1,3,2,0,3,0,3,1,2,1,1,1,2,1,3,2,2,2,3,3,2,1,1,1,2,2,2,0,0,3,1,3,1,1,1,3,0,0,1,0,0,1,2,2,1,2,3,2,2,2,3,1,3,2,0,1,3},
     {3, 4, 5, 6, -1, -1}, {3, 6}},
    {{3,0,2,2,2,1,1,1,1,2,1,0,0,0,2,3,2,3,1,3,0,0,0,2,1,2,2,3,0,3,3,3,0,1,3,2,3,3,3,1,1,1,1,2,0,1,2,1,3,2,3,1,1,3,2,2,2,3,1,3,2,3,0,0},
     {0, 1, 2, 4, 7, -1}, {2, 7}},
    {{3,0,3,1,1,0,2,2,3,1,2,0,3,3,2,3,0,1,0,1,2,3,0,2,0,2,0,1,0,0,1,0,2,3,1,2,1,0,2,0,2,1,0,1,0,2,1,0,3,1,2,3,1,3,1,1,1,2,0,2,2,0,0,0},
     {0, 1, 2, 3, 6, 7}, {0, 1}},
    {{3,2,0,3,0,2,2,1,1,2,3,2,1,3,2,1,2,2,1,3,3,2,1,0,1,0,1,3,0,0,0,2,2,1,0,1,0,1,0,1,3,1,1,2,2,3,2,0,3,3,2,0,2,1,3,3,0,0,3,0,1,1,3,3},
     {0, 1, 3, 5, 6, 7}, {4, 5}},
};
const SBox kFn1R2[4] = {
    {{3,3,2,0,3,0,3,1,0,3,0,1,0,2,1,3,1,3,0,3,3,1,3,3,3,2,3,2,2,3,1,2,0,2,2,1,0,1,2,0,3,3,0,1,3,2,1,2,3,0,1,3,0,1,2,2,1,2,1,2,0,1,3,0},
     {0, 1, 2, 3, 6, -1}, {1, 6}},
    {{1,2,3,2,1,3,0,1,1,0,2,0,0,2,3,2,3,3,0,1,2,2,1,0,1,0,1,2,3,2,1,3,2,2,2,0,1,0,2,3,2,1,2,1,2,1,0,3,0,1,2,3,1,2,1,3,2,0,3,2,3,0,2,0},
     {2, 4, 5, 6, 7, -1}, {5, 7}},
    {{0,1,0,2,1,1,0,1,0,2,2,2,1,3,0,0,1,1,3,1,2,2,2,3,1,0,3,3,3,2,2,2,1,1,3,0,3,1,3,0,1,3,3,2,1,1,0,0,1,2,2,2,1,1,1,2,2,0,0,3,2,3,1,3},
     {1, 2, 3, 4, 5, 7}, {0, 3}},
    {{2,1,0,3,3,3,2,0,1,2,1,1,1,0,3,1,1,3,3,0,1,2,1,0,0,0,3,0,3,0,3,0,1,3,3,3,0,3,2,0,2,1,2,2,2,1,1,3,0,1,0,1,0,1,1,1,1,3,1,0,1,2,3,3},
     {0, 1, 3, 4, 6, 7}, {2, 4}},
};
const SBox kFn1R3[4] = {
    {{0,0,0,3,3,1,1,0,2,0,2,0,0,0,3,2,0,1,2,3,2,2,1,0,3,0,0,0,0,0,2,3,3,0,0,1,1,2,3,3,0,1,3,2,0,1,3,3,2,0,0,1,0,2,0,0,0,3,1,3,3,3,3,3},
     {0, 1, 5, 6, 7, -1}, {0, 5}},
    {{2,3,2,3,0,2,3,0,2,2,3,0,3,2,0,2,1,0,2,3,1,1,1,0,0,1,0,2,1,2,2,1,3,0,2,1,2,3,3,0,3,2,3,1,0,2,1,0,1,2,2,3,0,2,1,3,1,3,0,2,1,1,1,3},
     {2, 3, 4, 6, 7, -1}, {6, 7}},
    {{3,0,2,1,1,3,1,2,2,1,2,2,2,0,0,1,2,3,1,0,2,0,0,2,3,1,2,0,0,0,3,0,2,1,1,2,0,0,1,2,3,1,1,2,0,1,3,0,3,1,1,0,0,2,3,0,0,0,0,3,2,0,0,0},
     {0, 2, 3, 4, 5, 6}, {1, 4}},
    {{0,1,0,0,2,1,3,2,3,3,2,1,0,1,1,1,1,1,0,3,3,1,1,0,0,2,2,1,0,3,3,2,1,3,3,0,3,0,2,1,1,2,3,2,2,2,1,0,0,3,3,3,2,2,3,1,0,2,3,0,3,1,1,0},
     {0, 1, 2, 3, 5, 7}, {2, 3}},
};
const SBox kFn1R4[4] = {
    {{1,1,1,1,1,0,1,3,3,2,3,0,1,2,0,2,3,3,0,1,2,1,2,3,0,3,2,3,2,0,1,2,0,1,0,3,2,1,3,2,3,1,2,3,2,0,1,2,2,0,0,0,2,1,3,0,3,1,3,0,1,3,3,0},
     {1, 2, 3, 4, 5, 7}, {0, 4}},
    {{3,0,0,0,0,1,0,2,3,3,1,3,0,3,1,2,2,2,3,1,0,0,2,0,1,0,2,2,3,3,0,0,1,1,3,0,2,3,0,3,0,3,0,2,0,2,0,1,0,3,0,1,3,1,1,0,0,1,3,3,2,2,1,0},
     {0, 1, 2, 3, 5, 6}, {1, 3}},
    {{0,1,1,2,0,1,3,1,2,0,3,2,0,0,3,0,3,0,1,2,2,3,3,2,3,2,0,1,0,0,1,0,3,0,2,3,0,2,2,2,1,1,0,2,2,0,0,1,2,1,1,1,2,3,0,3,1,2,3,3,1,1,3,0},
     {0, 2, 4, 5, 6, 7}, {2, 6}},
    {{0,1,2,2,0,1,0,3,2,2,1,1,3,2,0,2,0,1,3,3,0,2,2,3,3,2,0,0,2,1,3,3,1,1,1,3,1,2,1,1,0,3,3,2,3,2,3,0,3,1,0,0,3,0,0,0,2,2,2,1,2,3,0,0},
     {0, 1, 3, 4, 6, 7}, {5, 7}},
};
const SBox kFn2R1[4] = {
    {{2,0,2,0,3,0,0,3,1,1,0,1,3,2,0,1,2,0,1,2,0,2,0,2,2,2,3,0,2,1,3,0,0,1,0,1,2,2,3,3,0,3,0,2,3,0,1,2,1,1,0,2,0,3,1,1,2,2,1,3,1,1,3,1},
     {0, 3, 4, 5, 7, -1}, {6, 7}},
    {{1,1,0,3,0,2,0,1,3,0,2,0,1,1,0,0,1,3,2,2,0,2,2,2,2,0,1,3,3,3,1,1,1,3,1,3,2,2,2,2,2,2,0,1,0,1,1,2,3,1,1,2,0,3,3,3,2,2,3,1,1,1,3,0},
     {1, 2, 3, 4, 6, -1}, {3, 5}},
    {{1,0,2,2,3,3,3,3,1,2,2,1,0,1,2,1,1,2,3,1,2,0,0,1,2,3,1,2,0,0,0,2,2,0,1,1,0,0,2,0,0,0,2,3,2,3,0,1,3,0,0,0,2,3,2,0,1,3,2,1,3,1,1,3},
     {1, 2, 4, 5, 6, 7}, {1, 4}},
    {{1,3,3,0,3,2,3,1,3,2,1,1,3,3,2,1,2,3,0,3,1,0,0,2,3,0,0,0,3,3,0,1,2,3,0,0,0,1,2,1,3,0,0,1,0,2,2,2,3,3,1,2,1,3,0,0,0,3,0,1,3,2,2,0},
     {0, 2, 3, 5, 6, 7}, {0, 2}},
};
const SBox kFn2R2[4] = {
    {{3,1,3,0,3,0,3,1,3,0,0,1,1,3,0,3,1,1,0,1,2,3,2,3,3,1,2,2,2,0,2,3,2,2,2,1,1,3,3,0,3,1,2,1,1,1,0,2,0,3,3,0,0,2,0,0,1,1,2,1,2,1,1,0},
     {0, 2, 4, 6, -1, -1}, {4, 6}},
    {{0,3,0,3,3,2,1,2,3,1,1,1,2,0,2,3,0,3,1,2,2,1,3,3,3,2,1,2,2,0,1,0,2,3,0,1,2,0,1,1,2,0,2,1,2,0,2,3,3,1,0,2,3,3,0,3,1,1,3,0,0,1,2,0},
     {1, 3, 4, 5, 6, 7}, {0, 3}},
    {{0,0,2,1,3,2,1,0,1,2,2,2,1,1,0,3,1,2,2,3,2,1,1,0,3,0,0,1,1,2,3,1,3,3,2,2,1,0,1,1,1,2,0,1,2,3,0,3,3,0,3,2,2,0,2,2,1,2,3,2,1,0,2,1},
     {0, 1, 3, 4, 5, 7}, {1, 7}},
    {{0,2,1,2,0,2,2,0,1,3,2,0,3,2,3,0,3,3,2,3,1,2,3,1,2,2,0,0,2,2,1,2,2,3,3,3,1,1,0,0,0,3,2,0,3,2,3,1,1,1,1,0,1,0,1,3,0,0,1,2,2,3,2,0},
     {1, 2, 3, 5, 6, 7}, {2, 5}},
};
const SBox kFn2R3[4] = {
    {{2,1,2,1,2,3,1,3,2,2,1,3,3,0,0,1,0,2,0,3,3,1,0,0,1,1,0,2,3,2,1,2,1,1,2,1,1,3,2,2,0,2,2,3,3,3,2,0,0,0,0,0,3,3,3,0,1,2,1,0,2,3,3,1},
     {2, 3, 4, 6, -1, -1}, {3, 5}},
    {{3,2,3,3,1,0,3,0,2,0,1,1,1,0,3,0,3,1,3,1,0,1,2,3,2,2,3,2,0,1,1,2,3,0,0,2,1,0,0,2,2,0,1,0,0,2,0,0,1,3,1,3,2,0,3,3,1,0,2,2,2,3,0,0},
     {0, 1, 3, 5, 7, -1}, {0, 2}},
    {{2,2,1,0,2,3,3,0,0,0,1,3,1,2,3,2,2,3,1,3,0,3,0,3,3,2,2,1,0,0,0,2,1,2,2,2,0,0,1,2,0,1,3,0,2,3,2,1,3,2,2,2,3,1,3,0,2,0,2,1,0,3,3,1},
     {0, 1, 2, 3, 5, 7}, {1, 6}},
    {{1,2,3,2,0,2,1,3,3,1,0,1,1,2,2,0,0,1,1,1,2,1,1,2,0,1,3,3,1,1,1,2,3,3,1,0,2,1,1,1,2,1,0,0,2,2,3,2,3,2,2,0,2,2,3,3,0,2,3,0,2,2,1,1},
     {0, 2, 4, 5, 6, 7}, {4, 7}},
};
const SBox kFn2R4[4] = {
    {{2,0,1,1,2,1,3,3,1,1,1,2,0,1,0,2,0,1,2,0,2,3,0,2,3,3,2,2,3,2,0,1,3,0,2,0,2,3,1,3,2,0,0,1,1,2,3,1,1,1,0,1,2,0,3,3,1,1,1,3,3,1,1,0},
     {0, 1, 3, 6, 7, -1}, {0, 3}},
    {{1,2,2,1,0,3,3,1,0,2,2,2,1,0,1,0,1,1,0,1,0,2,1,0,2,1,0,2,3,2,3,3,2,2,1,2,2,3,1,3,3,3,0,1,0,1,3,0,0,0,1,2,0,3,3,2,3,2,1,3,2,1,0,2},
     {0, 1, 2, 4, 5, 6}, {4, 7}},
    {{2,3,2,1,3,2,3,0,0,2,1,1,0,0,3,2,3,1,0,1,2,2,2,1,3,2,2,1,0,2,1,2,0,3,1,0,0,3,1,1,3,3,2,0,1,0,1,3,0,0,1,2,1,2,3,2,1,0,0,3,2,1,1,3},
     {0, 2, 3, 4, 5, 7}, {1, 2}},
    {{2,0,0,3,2,2,2,1,3,3,1,1,2,0,0,3,1,0,3,2,1,0,2,0,3,2,2,3,2,0,3,0,1,3,0,2,2,1,3,3,0,1,0,3,1,1,3,2,0,3,0,2,3,2,1,3,2,3,0,0,1,3,2,1},
     {2, 3, 4, 5, 6, 7}, {5, 6}},
};
const uint8_t kKey1Bits[96] = {33, 58, 49, 36, 0, 31, 22, 30, 3, 16, 5, 53, 10, 41, 23, 19, 27, 39, 43, 6, 34, 12, 61, 21, 48, 13, 32, 35, 6, 42, 43, 14, 21, 41, 52, 25, 18, 47, 46, 37, 57, 53, 20, 8, 55, 54, 59, 60, 27, 33, 35, 18, 8, 15, 63, 1, 50, 44, 16, 46, 5, 4, 45, 51, 38, 25, 13, 11, 62, 29, 48, 2, 59, 61, 62, 56, 51, 57, 54, 9, 24, 63, 22, 7, 26, 42, 45, 40, 23, 14, 2, 31, 52, 28, 44, 17};
const uint8_t kKey2Bits[96] = {34, 9, 32, 24, 44, 54, 38, 61, 47, 13, 28, 7, 29, 58, 18, 1, 20, 60, 15, 6, 11, 43, 39, 19, 63, 23, 16, 62, 54, 40, 31, 3, 56, 61, 17, 25, 47, 38, 55, 57, 5, 4, 15, 42, 22, 7, 2, 19, 46, 37, 29, 39, 12, 30, 49, 57, 31, 41, 26, 27, 24, 36, 11, 63, 33, 16, 56, 62, 48, 60, 59, 32, 12, 30, 53, 48, 10, 0, 50, 35, 3, 59, 14, 49, 51, 45, 44, 2, 21, 33, 55, 52, 23, 28, 8, 26};
const uint8_t kSubkeyBits[64] = {5, 10, 14, 9, 4, 0, 15, 6, 1, 8, 3, 2, 12, 7, 13, 11, 5, 12, 7, 2, 13, 11, 9, 14, 4, 1, 6, 10, 8, 0, 15, 3, 4, 10, 2, 0, 6, 9, 12, 1, 11, 7, 15, 8, 13, 5, 14, 3, 14, 11, 12, 7, 4, 5, 2, 10, 1, 15, 0, 9, 8, 6, 13, 3};

const int8_t kFn1A[8] = {10, 4, 6, 7, 2, 13, 15, 14};
const int8_t kFn1B[8] = {0, 1, 3, 5, 8, 9, 11, 12};
const int8_t kFn2A[8] = {6, 0, 2, 13, 1, 4, 14, 7};
const int8_t kFn2B[8] = {3, 5, 9, 10, 8, 15, 12, 11};

// caixa-S pré-calculada: entrada de 8 bits -> índice de 6 bits; índice -> bits de saída já posicionados
struct FastBox {
  uint8_t in[256];
  uint8_t out[64];
  void build(const SBox& b) {
    for (int v = 0; v < 256; v++) {
      int r = 0;
      for (int i = 0; i < 6; i++)
        if (b.inputs[i] >= 0) r |= ((v >> b.inputs[i]) & 1) << i;
      in[v] = static_cast<uint8_t>(r);
    }
    for (int i = 0; i < 64; i++) {
      const int o = b.table[i];
      out[i] = static_cast<uint8_t>(((o & 1) ? 1 << b.outputs[0] : 0) | ((o & 2) ? 1 << b.outputs[1] : 0));
    }
  }
};

struct Boxes {
  FastBox f1[4][4], f2[4][4];
  Boxes() {
    const SBox* a[4] = {kFn1R1, kFn1R2, kFn1R3, kFn1R4};
    const SBox* b[4] = {kFn2R1, kFn2R2, kFn2R3, kFn2R4};
    for (int r = 0; r < 4; r++)
      for (int i = 0; i < 4; i++) { f1[r][i].build(a[r][i]); f2[r][i].build(b[r][i]); }
  }
};
const Boxes& boxes() {
  static const Boxes b;
  return b;
}

inline uint8_t roundFn(uint8_t in, const FastBox* bx, uint32_t key) {
  return static_cast<uint8_t>(bx[0].out[bx[0].in[in] ^ (key & 0x3F)] | bx[1].out[bx[1].in[in] ^ ((key >> 6) & 0x3F)] |
                              bx[2].out[bx[2].in[in] ^ ((key >> 12) & 0x3F)] | bx[3].out[bx[3].in[in] ^ ((key >> 18) & 0x3F)]);
}

inline uint8_t gather(uint16_t v, const int8_t* bits) {
  uint8_t r = 0;
  for (int i = 0; i < 8; i++) r = static_cast<uint8_t>(r | (((v >> bits[i]) & 1) << i));
  return r;
}
inline uint16_t scatter(uint8_t v, const int8_t* bits) {
  uint16_t r = 0;
  for (int i = 0; i < 8; i++) r = static_cast<uint16_t>(r | (((v >> i) & 1) << bits[i]));
  return r;
}

uint16_t feistel(uint16_t val, const int8_t* A, const int8_t* B, const FastBox (*bx)[4], const uint32_t* k) {
  uint8_t l = gather(val, B), r = gather(val, A);
  l ^= roundFn(r, bx[0], k[0]);
  r ^= roundFn(l, bx[1], k[1]);
  l ^= roundFn(r, bx[2], k[2]);
  r ^= roundFn(l, bx[3], k[3]);
  return static_cast<uint16_t>(scatter(l, A) | scatter(r, B));
}

// inverso da rede (cifra): as rodadas em ordem contrária
uint16_t feistelInv(uint16_t val, const int8_t* A, const int8_t* B, const FastBox (*bx)[4], const uint32_t* k) {
  uint8_t l = gather(val, A), r = gather(val, B);
  r ^= roundFn(l, bx[3], k[3]);
  l ^= roundFn(r, bx[2], k[2]);
  r ^= roundFn(l, bx[1], k[1]);
  l ^= roundFn(r, bx[0], k[0]);
  return static_cast<uint16_t>(scatter(l, B) | scatter(r, A));
}

inline uint32_t bitOf(const uint32_t* k, int b) { return (k[b / 32] >> (b % 32)) & 1; }
inline uint32_t bit(uint32_t v, int n) { return (v >> n) & 1; }

void expandKey(uint32_t* dst, const uint32_t* src, const uint8_t* bits) {
  dst[0] = dst[1] = dst[2] = dst[3] = 0;
  for (int i = 0; i < 96; i++) dst[i / 24] |= bitOf(src, bits[i]) << (i % 24);
}

void key1Of(const uint32_t* master, uint32_t* k1) {
  expandKey(k1, master, kKey1Bits);
  // bits extras para as caixas-S com menos de 6 entradas
  k1[0] ^= bit(k1[0], 1) << 4;
  k1[0] ^= bit(k1[0], 2) << 5;
  k1[0] ^= bit(k1[0], 8) << 11;
  k1[1] ^= bit(k1[1], 0) << 5;
  k1[1] ^= bit(k1[1], 8) << 11;
  k1[2] ^= bit(k1[2], 1) << 5;
  k1[2] ^= bit(k1[2], 8) << 11;
}

void key2Of(const uint32_t* master, const uint32_t* k1, uint16_t addr, uint32_t* k2) {
  const Boxes& B = boxes();
  const uint16_t seed = feistel(addr, kFn1A, kFn1B, B.f1, k1);
  uint32_t sub[2] = {0, 0};
  for (int i = 0; i < 64; i++) sub[i / 32] |= static_cast<uint32_t>((seed >> kSubkeyBits[i]) & 1) << (i % 32);
  sub[0] ^= master[0];
  sub[1] ^= master[1];
  expandKey(k2, sub, kKey2Bits);
  k2[0] ^= bit(k2[0], 0) << 5;
  k2[0] ^= bit(k2[0], 6) << 11;
  k2[1] ^= bit(k2[1], 0) << 5;
  k2[1] ^= bit(k2[1], 1) << 4;
  k2[2] ^= bit(k2[2], 2) << 5;
  k2[2] ^= bit(k2[2], 3) << 4;
  k2[2] ^= bit(k2[2], 7) << 11;
  k2[3] ^= bit(k2[3], 1) << 5;
}

}  // namespace

Cps2Key cps2ParseKey(const std::vector<uint8_t>& keyFile) {
  Cps2Key k;
  if (keyFile.size() < 20) return k;
  uint16_t dec[10] = {};
  // os 160 bits do arquivo .key estão numa ordem embaralhada (a mesma leitura do MAME)
  for (int b = 0; b < 160; b++) {
    const int bitPos = (317 - b) % 160;
    if ((keyFile[static_cast<size_t>(bitPos / 8)] >> ((bitPos ^ 7) % 8)) & 1) dec[b / 16] = static_cast<uint16_t>(dec[b / 16] | (0x8000 >> (b % 16)));
  }
  k.key[0] = (static_cast<uint32_t>(dec[0]) << 16) | dec[1];
  k.key[1] = (static_cast<uint32_t>(dec[2]) << 16) | dec[3];
  // dec[4..6] = instrução do "cão de guarda"; dec[7], dec[8] = constantes; dec[9] = fim da faixa cifrada
  if (dec[9] == 0xFFFF) {
    // placa "morta" (chave vazia): só FF0000-FFFFFF ficaria cifrado, ou seja, nada da ROM
    k.lower = 0xFF0000;
    k.upper = 0xFFFFFF;
  } else {
    k.lower = 0;
    k.upper = (((~static_cast<uint32_t>(dec[9]) & 0x3FF) << 14) | 0x3FFF) + 1;
  }
  k.valid = true;
  return k;
}

void cps2Decrypt(const std::vector<uint8_t>& rom, std::vector<uint8_t>& dec, const Cps2Key& key) {
  dec = rom;
  if (!key.valid) return;
  const Boxes& B = boxes();
  uint32_t k1[4];
  key1Of(key.key, k1);
  const size_t words = rom.size() / 2;
  const size_t lo = key.lower / 2, hi = key.upper / 2;
  for (uint32_t i = 0; i < 0x10000 && i < words; i++) {
    bool any = false;
    for (size_t a = i; a < words; a += 0x10000)
      if (a >= lo && a <= hi) { any = true; break; }
    if (!any) continue;
    uint32_t k2[4];
    key2Of(key.key, k1, static_cast<uint16_t>(i), k2);
    for (size_t a = i; a < words; a += 0x10000) {
      if (a < lo || a > hi) continue;
      const uint16_t e = static_cast<uint16_t>((rom[2 * a] << 8) | rom[2 * a + 1]);
      const uint16_t d = feistel(e, kFn2A, kFn2B, B.f2, k2);
      dec[2 * a] = static_cast<uint8_t>(d >> 8);
      dec[2 * a + 1] = static_cast<uint8_t>(d);
    }
  }
}

uint16_t cps2DecryptWord(uint16_t enc, uint32_t wordAddr, const Cps2Key& key) {
  uint32_t k1[4], k2[4];
  key1Of(key.key, k1);
  key2Of(key.key, k1, static_cast<uint16_t>(wordAddr), k2);
  return feistel(enc, kFn2A, kFn2B, boxes().f2, k2);
}

uint16_t cps2EncryptWord(uint16_t plain, uint32_t wordAddr, const Cps2Key& key) {
  uint32_t k1[4], k2[4];
  key1Of(key.key, k1);
  key2Of(key.key, k1, static_cast<uint16_t>(wordAddr), k2);
  return feistelInv(plain, kFn2A, kFn2B, boxes().f2, k2);
}

}  // namespace indigo::cps
