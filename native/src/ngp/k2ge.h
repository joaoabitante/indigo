// Vídeo do Neo Geo Pocket Color (porte de src/ngp/k2ge.js): 160×152, dois planos de 32×32 blocos, 64 sprites
// de 8×8, paletas de 12 bits (modo cor) ou 8 tons de cinza (modo K1GE), janela de recorte. Os registros de
// rolagem/janela/cor de fundo são copiados no fim de cada linha (efeito na linha seguinte).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::ngp {

constexpr int W = 160, H = 152;

class NGP;

class K2GE {
public:
  explicit K2GE(NGP& sys);

  uint32_t fb[W * H];
  uint8_t scroll[0x1000], chr[0x2000], spr[0x100], sprCol[0x40], pal[0x200], monoPal[0x20];
  int ctlInt = 0;

  void power();
  void reset();
  void latch();
  int read8(uint32_t a) const;
  void write8(uint32_t a, int v);
  bool hblank();
  bool endLine();
  void sync(StateIO& io);

private:
  void drawLine(int y);
  void plane(int y, int p, int depth, bool mono);
  void sprites(int y, bool mono);
  void pattern(int sx, int tile, int row, int flip, int palBase, int depth, bool mono);

  NGP& sys;
  uint16_t line[256];
  uint8_t z[256];
  int raster = 0;
  int s1x = 0, s1y = 0, s2x = 0, s2y = 0;
  int wbaH = 0, wbaV = 0, wsiH = 0xFF, wsiV = 0xFF;
  int cOvr = 0, blnk = 0;
  int poH = 0, poV = 0, pf = 0, bgCol = 7;
  int ctl2d = 0, period = 0xC6, mode = 0;
  int winx = 0, winy = 0, winw = 0, winh = 0;
  int sc1x = 0, sc1y = 0, sc2x = 0, sc2y = 0, spx = 0, spy = 0;
  int swap = 0, bgc = 0, oowc = 0, neg = 0;
};

}  // namespace indigo::ngp
