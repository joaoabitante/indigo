// Codificador do formato do RAINBOW (HuC6271) para o teste do PC-FX, escrito do zero a partir da descrição em
// src/pcfx/rainbow.h: monta faixas de 256x16 (YUV parecido com JPEG ou paleta com repetição) e calcula, com uma
// IDCT em ponto flutuante, a imagem que o chip deve devolver. Usa só as tabelas de Huffman do núcleo.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "../src/pcfx/rainbow.h"

namespace rbt {
using indigo::pcfx::RbHuff;
using indigo::pcfx::RB_ZIGZAG;

struct Code { uint16_t bits = 0; uint8_t len = 0; };
struct Enc { Code c[256]; };

// valor -> código mais curto da tabela canônica
inline Enc makeEnc(const RbHuff& t) {
  Enc e;
  for (int n = 2; n <= 12; n++) {
    if (t.max[n] == 0xFFFF) continue;
    for (uint32_t c = t.min[n]; c <= t.max[n]; c++) {
      const uint8_t v = t.codes[t.base[n] + (c - t.min[n])];
      if (!e.c[v].len) e.c[v] = {uint16_t(c), uint8_t(n)};
    }
  }
  return e;
}

// nenhum código da tabela é começo de outro
inline bool prefixFree(const RbHuff& t) {
  std::vector<std::pair<uint32_t, int>> all;
  for (int n = 2; n <= 12; n++)
    if (t.max[n] != 0xFFFF)
      for (uint32_t c = t.min[n]; c <= t.max[n]; c++) all.push_back({c, n});
  for (const auto& a : all)
    for (const auto& b : all)
      if (&a != &b && a.second <= b.second && (b.first >> (b.second - a.second)) == a.first) return false;
  return !all.empty();
}

struct Bits {
  std::vector<uint8_t> out;
  uint32_t acc = 0;
  int n = 0;
  size_t logical = 0;
  int stuffed = 0;
  void byte(uint8_t b) {
    out.push_back(b);
    logical++;
    if (b == 0xFF) { out.push_back(0x00); stuffed++; }  // enchimento depois de 0xFF
  }
  void put(uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
      acc = (acc << 1) | ((v >> i) & 1);
      if (++n == 8) { byte(uint8_t(acc)); acc = 0; n = 0; }
    }
  }
  void code(const Code& c) { put(c.bits, c.len); }
  void flush() { while (n) put(0, 1); }
};

inline int category(int v) { int a = std::abs(v), s = 0; while (a) { s++; a >>= 1; } return s; }
inline uint32_t valBits(int v, int s) { return v >= 0 ? uint32_t(v) : uint32_t(v + (1 << s) - 1); }

// faixa de entrada (0..255): Y 256x16, U e V 128x8
struct Strip { int y[16][256]; int u[8][128]; int v[8][128]; };
// imagem que o RAINBOW deve devolver (Y<<16 | U<<8 | V)
struct Out { uint32_t px[16][256]; };

struct Opt {
  bool tables = false;   // bloco 0xFF (com as tabelas de quantização) em vez de 0xF8
  int scale = -1;        // código de escala (0..15) antes da primeira coluna
  int nullFrom = -1;     // colunas vazias
  int nullCount = 0;
};

class Encoder {
public:
  uint8_t qbase[2][64], q[2][64];
  Enc dcy = makeEnc(indigo::pcfx::RB_DC_Y), dcuv = makeEnc(indigo::pcfx::RB_DC_UV);
  Enc acy = makeEnc(indigo::pcfx::RB_AC_Y), acuv = makeEnc(indigo::pcfx::RB_AC_UV);
  uint32_t nullColor = 0;
  int stuffed = 0;

  Encoder() {
    for (int i = 0; i < 64; i++) {
      const int r = i / 8, c = i % 8;
      qbase[0][i] = q[0][i] = uint8_t(4 + (r + c) * 2);   // Y: 4 no DC, cresce com a frequência
      qbase[1][i] = q[1][i] = uint8_t(6 + (r + c) * 3);   // UV
    }
  }

  // uma faixa YUV; devolve os bytes do bloco e preenche ref com a imagem esperada
  std::vector<uint8_t> yuv(const Strip& s, const Opt& o, Out& ref) {
    Bits b;
    int pdy = 0, pdu = 0, pdv = 0;
    for (int col = 0; col < 16; col++) {
      if (col == 0 && o.scale >= 0) {  // nova escala: o chip refaz as tabelas a partir das de base
        b.code(dcy.c[0x10 + o.scale]);
        for (int i = 0; i < 64; i++) {
          int v = (qbase[0][i] * o.scale) >> 2;
          q[0][i] = uint8_t(v < 1 ? 1 : v > 0xFE ? 0xFE : v);
          v = i ? (qbase[1][i] * o.scale) >> 2 : qbase[1][i] >> 2;
          q[1][i] = uint8_t(v < 1 ? 1 : v > 0xFE ? 0xFE : v);
        }
      }
      if (col == o.nullFrom) {  // colunas vazias: código 0x0F do DC + contagem num código AC
        b.code(dcy.c[0x0F]);
        if (o.nullCount == 1) b.put(0x1F, 5);
        else { b.code(acy.c[((o.nullCount - 1) << 4) | 1]); b.put(0, 1); }
        for (int c = col; c < col + o.nullCount && c < 16; c++)
          for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) ref.px[y][c * 16 + x] = nullColor;
        col += o.nullCount - 1;
        pdy = pdu = pdv = 0;
        continue;
      }
      double blk[6][64];
      for (int k = 0; k < 4; k++) {  // A (em cima, esquerda), B (embaixo), C (em cima, direita), D
        const int oy = (k & 1) * 8, ox = col * 16 + (k >> 1) * 8;
        for (int y = 0; y < 8; y++)
          for (int x = 0; x < 8; x++) blk[k][y * 8 + x] = s.y[oy + y][ox + x] - 128;
      }
      for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
          blk[4][y * 8 + x] = s.u[y][col * 8 + x] - 128;
          blk[5][y * 8 + x] = s.v[y][col * 8 + x] - 128;
        }
      double rec[6][64];
      for (int k = 0; k < 6; k++) {
        const int t = k < 4 ? 0 : 1;
        int coef[64];
        fdct(blk[k], coef, q[t]);
        int& pred = k < 4 ? pdy : k == 4 ? pdu : pdv;
        int diff = coef[0] - pred;
        if (diff > 511) diff = 511;
        if (diff < -511) diff = -511;
        pred += diff;
        const int cat = category(diff);
        b.code((k < 4 ? dcy : dcuv).c[cat]);
        b.put(valBits(diff, cat), cat);
        // AC em zigue-zague: (corrida, tamanho), 0x10 = 16 zeros, 11111 = fim do bloco
        const Enc& ac = t ? acuv : acy;
        int run = 0;
        for (int z = 0; z < 63; z++) {
          int v = coef[RB_ZIGZAG[z]];
          if (v > 511) v = 511;
          if (v < -511) v = -511;
          coef[RB_ZIGZAG[z]] = v;
          if (!v) { run++; continue; }
          while (run > 15) { b.code(ac.c[0x10]); run -= 16; }
          const int sz = category(v);
          b.code(ac.c[(run << 4) | sz]);
          b.put(valBits(v, sz), sz);
          run = 0;
        }
        if (run) b.put(0x1F, 5);
        // o que o chip reconstrói: coeficiente * quantização (16 bits) e IDCT
        double deq[64];
        deq[0] = int16_t(uint16_t(uint32_t(q[t][0]) * uint32_t(pred)));
        for (int i = 1; i < 64; i++) deq[i] = int16_t(uint16_t(uint32_t(q[t][i]) * uint32_t(coef[i])));
        idct(deq, rec[k]);
      }
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
          const int k = (x >> 3) * 2 + (y >> 3);
          const int c = (y >> 1) * 8 + (x >> 1);
          ref.px[y][col * 16 + x] = uint32_t(cl(rec[k][(y & 7) * 8 + (x & 7)])) << 16 | uint32_t(cl(rec[4][c])) << 8 | cl(rec[5][c]);
        }
    }
    b.flush();
    stuffed += b.stuffed;
    std::vector<uint8_t> blk;
    const size_t size = 2 + (o.tables ? 128 : 0) + b.logical;
    blk.push_back(0xFF);
    blk.push_back(o.tables ? 0xFF : 0xF8);
    blk.push_back(uint8_t(size >> 8));
    blk.push_back(uint8_t(size));
    if (o.tables)
      for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) blk.push_back(qbase[t][i]);
    blk.insert(blk.end(), b.out.begin(), b.out.end());
    return blk;
  }

  // faixa de paleta com repetição (tipo 0xF0..0xF3); idx = 4096 índices
  static std::vector<uint8_t> rle(int type, const uint8_t* idx, int& stuffedOut) {
    const unsigned shift = 4 - (type & 3), mask = (1u << shift) - 1;
    std::vector<uint8_t> d;
    auto put = [&](uint8_t v) { d.push_back(v); if (v == 0xFF) { d.push_back(0); stuffedOut++; } };
    int x = 0;
    while (x < 4096) {
      int n = 1;
      while (x + n < 4096 && idx[x + n] == idx[x] && n < 256) n++;
      if (unsigned(n) <= mask) put(uint8_t(idx[x] << shift | n));
      else { put(uint8_t(idx[x] << shift)); put(uint8_t(n - 1)); }
      x += n;
    }
    std::vector<uint8_t> blk = {0xFF, uint8_t(type), uint8_t((d.size() + 2) >> 8), uint8_t(d.size() + 2)};
    blk.insert(blk.end(), d.begin(), d.end());
    return blk;
  }

private:
  static uint8_t cl(double v) { const long r = std::lround(v) + 128; return uint8_t(r < 0 ? 0 : r > 255 ? 255 : r); }
  static double cu(int u) { return u ? 1.0 : 1.0 / std::sqrt(2.0); }
  static double cs(int x, int u) { return std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0); }
  static void fdct(const double* in, int* out, const uint8_t* qt) {
    for (int v = 0; v < 8; v++)
      for (int u = 0; u < 8; u++) {
        double s = 0;
        for (int y = 0; y < 8; y++)
          for (int x = 0; x < 8; x++) s += in[y * 8 + x] * cs(x, u) * cs(y, v);
        const double f = 0.25 * cu(u) * cu(v) * s;
        out[v * 8 + u] = int(std::lround(f / qt[v * 8 + u]));
      }
  }
  static void idct(const double* in, double* out) {
    for (int y = 0; y < 8; y++)
      for (int x = 0; x < 8; x++) {
        double s = 0;
        for (int v = 0; v < 8; v++)
          for (int u = 0; u < 8; u++) s += cu(u) * cu(v) * in[v * 8 + u] * cs(x, u) * cs(y, v);
        out[y * 8 + x] = 0.25 * s;
      }
  }
};

}  // namespace rbt
