// RDP (Reality Display Processor) do Nintendo 64 em software: lê a lista de comandos (da RDRAM ou da DMEM) e
// desenha na RDRAM. Triângulos com borda, shade, textura (com perspectiva) e z; retângulos (preenchimento e
// textura, também invertidos); modos de ciclo 1, 2, cópia e preenchimento; combinador de cor e blender de dois
// ciclos; teste e escrita de z (z comprimido de 14 bits); TMEM de 4 KB com LOAD_TILE/LOAD_BLOCK/LOAD_TLUT;
// formatos RGBA16/32, IA4/8/16, I4/8, CI4/8 (paleta RGBA16 ou IA16); filtro de 3 pontos; corte (scissor);
// teste de alfa. Uma amostra por pixel (sem cobertura de antisserrilhado), sem dithering e sem LOD automático.
// Escrito a partir da documentação pública dos comandos do RDP (manuais do SDK e n64brew), sem código de terceiros.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::n64 {

class N64;

class Rdp {
public:
  explicit Rdp(N64& s) : sys(s) {}
  void reset();
  // executa os comandos de DPC_CURRENT até DPC_END
  void run();
  void sync(StateIO& io);
  uint64_t pixels = 0;  // diagnóstico

  struct Tile {
    uint8_t fmt = 0, size = 0, pal = 0, ct = 0, mt = 0, maskT = 0, shiftT = 0, cs = 0, ms = 0, maskS = 0, shiftS = 0;
    uint16_t line = 0, tmem = 0;
    uint16_t sl = 0, tl = 0, sh = 0, th = 0;  // u10.2
  };

private:
  N64& sys;
  // estado
  uint64_t other_ = 0, combine_ = 0;
  uint32_t fillColor_ = 0, fogColor_ = 0, blendColor_ = 0, primColor_ = 0, envColor_ = 0;
  uint8_t primLodFrac_ = 0;
  uint16_t primZ_ = 0, primDz_ = 0;
  uint32_t texAddr_ = 0, colorAddr_ = 0, zAddr_ = 0;
  uint8_t texFmt_ = 0, texSize_ = 0, colorFmt_ = 0, colorSize_ = 0;
  uint16_t texWidth_ = 0, colorWidth_ = 0;
  int sxh_ = 0, syh_ = 0, sxl_ = 0, syl_ = 0;  // scissor u10.2
  Tile tiles_[8];
  uint8_t tmem_[4096] = {};
  uint64_t cmd_[44] = {};  // comando em montagem (o maior triângulo tem 22 palavras)
  int cmdLen_ = 0;

  void command(const uint64_t* w);
  void triangle(const uint64_t* w, bool shade, bool tex, bool z);
  void texRect(const uint64_t* w, bool flip);
  void fillRect(const uint64_t* w);
  void loadTile(const uint64_t* w);
  void loadBlock(const uint64_t* w);
  void loadTlut(const uint64_t* w);

  // pipeline de pixel
  struct Px {
    int32_t r, g, b, a;  // shade (0-255)
    int32_t s, t;        // coordenadas s10.5 já divididas
    uint32_t z;          // 18 bits
  };
  int cycleType() const { return int(other_ >> 52) & 3; }
  void pixel(int x, int y, const Px& p, int tile, uint16_t dz);
  void texel(int tile, int32_t s, int32_t t, uint32_t& out);
  void fetch(const Tile& tl, int s, int t, uint8_t* rgba);
  uint32_t readColor(int x, int y, uint8_t& cvg);
  void writeColor(int x, int y, uint32_t rgba);
  uint32_t readZ(int x, int y);
  void writeZ(int x, int y, uint32_t z, uint16_t dz);
  uint8_t rd8(uint32_t a) const;
  uint16_t rd16(uint32_t a) const;
  uint32_t rd32(uint32_t a) const;
  void wr8(uint32_t a, uint8_t v);
  void wr16(uint32_t a, uint16_t v);
  void wr32(uint32_t a, uint32_t v);
  // estado por desenho (pré-calculado)
  uint32_t noise_ = 1;
  int lodFrac_ = 0;
  // fontes do combinador: cada uma com 4 canais (r, g, b, a)
  enum { F_COMB, F_T0, F_T1, F_PRIM, F_SHADE, F_ENV, F_ONE, F_ZERO, F_NOISE, F_COMB_A, F_T0_A, F_T1_A, F_PRIM_A,
         F_SHADE_A, F_ENV_A, F_LOD, F_PRIMLOD, F_K4, F_K5, F_KEYC, F_KEYS, F_N };
  int32_t src_[F_N][4] = {};
  struct Pipe {
    uint8_t sel[2][4][4] = {};  // [ciclo][canal][A, B, C, D]
    uint8_t m1a[2] = {}, m1b[2] = {}, m2a[2] = {}, m2b[2] = {};
    bool usaT0 = false, usaT1 = false, usaShade = false, usaRuido = false, valido = false;
    uint64_t prontoOther = 0, prontoComb = 0;
  } pipe_;
  void preparaPipe();
  void preparaFontes();
};

}  // namespace indigo::n64
