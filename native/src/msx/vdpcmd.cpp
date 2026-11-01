// Processador de comandos do V9938 (R32–R46): cópias e preenchimentos de retângulos por byte (HMMx/YMMM)
// e por ponto com operação lógica (LMMx), linha, busca de cor (SRCH), PSET e POINT. Os comandos
// terminam na hora; as transferências pela CPU (HMMC, LMMC, LMCM) ficam com CE=1 até o último dado.
#include "vdp.h"

#include <algorithm>

namespace indigo::msx {

namespace {
enum { C_STOP = 0, C_POINT = 4, C_PSET = 5, C_SRCH = 6, C_LINE = 7, C_LMMV = 8, C_LMMM = 9,
       C_LMCM = 0xA, C_LMMC = 0xB, C_HMMV = 0xC, C_HMMM = 0xD, C_YMMM = 0xE, C_HMMC = 0xF };
}

VDP::CmdGeo VDP::geo() const {
  switch (mode()) {
    case 0x0C: return {256, 2, 4, 1024, 0};  // G4 (SCREEN 5)
    case 0x10: return {512, 4, 2, 1024, 1};  // G5 (SCREEN 6)
    case 0x14: return {512, 2, 4, 512, 2};   // G6 (SCREEN 7)
    case 0x1C: return {256, 1, 8, 512, 3};   // G7 (SCREEN 8)
    default: return {256, 1, 8, 512, 4};     // modos de caracteres: como o G7, sem entrelaçar
  }
}

int VDP::pixAddr(const CmdGeo& g, int x, int y) const {
  switch (g.kind) {
    case 0: return (((y & 1023) << 7) | ((x & 255) >> 1)) & vramMask;
    case 1: return (((y & 1023) << 7) | ((x & 511) >> 2)) & vramMask;
    case 2: return planarAddr(((y & 511) << 8) | ((x & 511) >> 1));
    case 3: return planarAddr(((y & 511) << 8) | (x & 255));
    default: return (((y & 511) << 8) | (x & 255)) & vramMask;
  }
}

int VDP::point(const CmdGeo& g, int x, int y) const {
  const int b = vram[pixAddr(g, x, y)];
  switch (g.kind) {
    case 0: case 2: return (x & 1) ? (b & 15) : (b >> 4);
    case 1: return (b >> ((3 - (x & 3)) * 2)) & 3;
    default: return b;
  }
}

void VDP::pset(const CmdGeo& g, int x, int y, int color, int lop) {
  const int pm = (1 << g.bpp) - 1;
  const int src = color & pm;
  if ((lop & 8) && !src) return;  // operações "T": a cor 0 não é gravada
  const int a = pixAddr(g, x, y);
  const int old = vram[a];
  int sh = 0;
  if (g.kind == 0 || g.kind == 2) sh = (x & 1) ? 0 : 4;
  else if (g.kind == 1) sh = (3 - (x & 3)) * 2;
  const int dst = (old >> sh) & pm;
  int r;
  switch (lop & 7) {
    case 0: r = src; break;
    case 1: r = src & dst; break;
    case 2: r = src | dst; break;
    case 3: r = src ^ dst; break;
    case 4: r = ~src; break;
    default: return;
  }
  vram[a] = static_cast<uint8_t>((old & ~(pm << sh)) | ((r & pm) << sh));
}

void VDP::startCommand() {
  const CmdGeo g = geo();
  cmd = reg[46] >> 4;
  cmdLop = reg[46] & 15;
  const int sx = getReg16(32, 0x1FF), sy = getReg16(34, 0x3FF);
  const int dx = getReg16(36, 0x1FF), dy = getReg16(38, 0x3FF);
  const int rawNx = getReg16(40, 0x1FF), rawNy = getReg16(42, 0x3FF);
  const int arg = reg[45], clr = reg[44];
  const int dix = (arg & 4) ? -1 : 1, diy = (arg & 8) ? -1 : 1;
  const int ylim = g.lines - 1;
  ce = false; tr = false;
  int nx = rawNx ? rawNx : 512, ny = rawNy ? rawNy : 1024;

  // largura útil de uma linha a partir de x (o comando para na borda da tela)
  auto clipNx = [&](int x, int n, int step) {
    const int room = dix > 0 ? g.width - x : x + 1;
    n = std::min(n, std::max(room, 0));
    return step > 1 ? n / step : n;
  };

  switch (cmd) {
    case C_POINT:
      stat[7] = static_cast<uint8_t>(point(g, sx, sy));
      return;
    case C_PSET:
      pset(g, dx, dy, clr, cmdLop);
      return;
    case C_SRCH: {
      const int target = clr & ((1 << g.bpp) - 1);
      const bool eq = arg & 2;
      bd = false;
      int x = sx;
      while (x >= 0 && x < g.width) {
        const int c = point(g, x, sy);
        if ((c == target) != eq) { bd = true; break; }
        x += dix;
      }
      if (bd) { stat[8] = static_cast<uint8_t>(x); stat[9] = static_cast<uint8_t>((x >> 8) | 0xFE); }
      return;
    }
    case C_LINE: {
      const int nxl = rawNx, nyl = rawNy;
      int asx = (nxl - 1) >> 1;
      int x = dx, y = dy;
      for (int n = 0;; n++) {
        pset(g, x, y, clr, cmdLop);
        if (!(arg & 1)) {
          x += dix;
          if (asx < nyl) { asx += nxl; y += diy; }
        } else {
          y += diy;
          if (asx < nyl) { asx += nxl; x += dix; }
        }
        asx = (asx - nyl) & 1023;
        y &= ylim;
        if (n >= nxl || x < 0 || x >= g.width) break;
      }
      setReg16(38, y & 1023);
      return;
    }
    case C_LMMV: case C_LMMM: case C_HMMV: case C_HMMM: {
      const bool bytes = cmd == C_HMMV || cmd == C_HMMM;
      const int step = bytes ? g.ppb : 1;
      const int x0 = bytes ? dx & ~(g.ppb - 1) : dx, s0 = bytes ? sx & ~(g.ppb - 1) : sx;
      int n = clipNx(x0, nx, step);
      if (cmd == C_LMMM || cmd == C_HMMM) n = std::min(n, clipNx(s0, nx, step));
      int ys = sy, yd = dy;
      for (int row = 0; row < ny; row++) {
        int xs = s0, xd = x0;
        for (int i = 0; i < n; i++) {
          switch (cmd) {
            case C_LMMV: pset(g, xd, yd, clr, cmdLop); break;
            case C_LMMM: pset(g, xd, yd, point(g, xs, ys), cmdLop); break;
            case C_HMMV: vram[pixAddr(g, xd, yd)] = static_cast<uint8_t>(clr); break;
            default: vram[pixAddr(g, xd, yd)] = vram[pixAddr(g, xs, ys)]; break;
          }
          xs += dix * step; xd += dix * step;
        }
        ys = (ys + diy) & 1023; yd = (yd + diy) & 1023;
      }
      if (cmd == C_LMMM || cmd == C_HMMM) setReg16(34, ys);
      setReg16(38, yd);
      setReg16(42, 0);
      return;
    }
    case C_YMMM: {
      const int x0 = dx & ~(g.ppb - 1);
      const int n = clipNx(x0, g.width, g.ppb);
      int ys = sy, yd = dy;
      for (int row = 0; row < ny; row++) {
        int x = x0;
        for (int i = 0; i < n; i++) { vram[pixAddr(g, x, yd)] = vram[pixAddr(g, x, ys)]; x += dix * g.ppb; }
        ys = (ys + diy) & 1023; yd = (yd + diy) & 1023;
      }
      setReg16(34, ys);
      setReg16(38, yd);
      setReg16(42, 0);
      return;
    }
    case C_LMCM: case C_LMMC: case C_HMMC: {
      const bool bytes = cmd == C_HMMC;
      const int step = bytes ? g.ppb : 1;
      const int bx = cmd == C_LMCM ? sx : dx;
      cxStart = bytes ? bx & ~(g.ppb - 1) : bx;
      cnx = clipNx(cxStart, nx, step);
      if (!cnx) { cmdFinish(); return; }
      cx = cxStart; cleft = cnx;
      cy = cmd == C_LMCM ? sy : dy;
      cny = ny;
      cdix = dix; cdiy = diy;
      ce = true; tr = true;
      if (cmd == C_LMCM) stat[7] = static_cast<uint8_t>(point(g, cx, cy));
      else cmdTransfer(clr);  // o primeiro dado já está em R44
      return;
    }
    default:  // STOP e códigos sem uso
      return;
  }
}

void VDP::cmdAdvance() {
  const CmdGeo g = geo();
  const int step = cmd == C_HMMC ? g.ppb : 1;
  cx += cdix * step;
  if (--cleft > 0) return;
  cy = (cy + cdiy) & 1023;
  if (--cny <= 0) { cmdFinish(); return; }
  cx = cxStart; cleft = cnx;
}

void VDP::cmdTransfer(int v) {
  if (!ce) return;
  const CmdGeo g = geo();
  if (cmd == C_HMMC) vram[pixAddr(g, cx, cy)] = static_cast<uint8_t>(v);
  else if (cmd == C_LMMC) pset(g, cx, cy, v, cmdLop);
  else return;
  cmdAdvance();
}

int VDP::cmdReadS7() {
  if (!(ce && cmd == C_LMCM)) return stat[7];
  const int v = stat[7];
  cmdAdvance();
  if (ce) stat[7] = static_cast<uint8_t>(point(geo(), cx, cy));
  return v;
}

void VDP::cmdFinish() {
  ce = false; tr = false;
  if (cmd == C_LMCM) setReg16(34, cy & 1023);
  else setReg16(38, cy & 1023);
  setReg16(42, 0);
}

}  // namespace indigo::msx
