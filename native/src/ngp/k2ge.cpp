#include "k2ge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "ngp.h"

namespace indigo::ngp {

namespace {
struct Tables {
  uint32_t COLOR[4096];
  uint16_t MIRROR16[65536];
  Tables() {
    for (uint32_t i = 0; i < 4096; i++) {
      const uint32_t r = (i & 15) * 17, g = ((i >> 4) & 15) * 17, b = ((i >> 8) & 15) * 17;
      COLOR[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    // espelhamento horizontal de uma linha de bloco (8 pixels de 2 bits)
    for (int i = 0; i < 65536; i++) {
      int m = 0;
      for (int p = 0; p < 8; p++) m |= ((i >> (p * 2)) & 3) << ((7 - p) * 2);
      MIRROR16[i] = static_cast<uint16_t>(m);
    }
  }
};
const Tables T;
// tom monocromático (0 = branco … 7 = quase preto) em cor de 12 bits
inline int shade(int v) { return (~(((v & 7) << 1) | ((v & 7) << 5) | ((v & 7) << 9))) & 0xFFF; }
}  // namespace

K2GE::K2GE(NGP& s) : sys(s) {
  std::memset(line, 0, sizeof line);
  std::memset(z, 0, sizeof z);
  power();
}

void K2GE::power() {
  std::memset(scroll, 0, sizeof scroll); std::memset(chr, 0, sizeof chr); std::memset(spr, 0, sizeof spr);
  std::memset(sprCol, 0, sizeof sprCol); std::memset(pal, 0, sizeof pal);
  std::fill(fb, fb + W * H, 0xFF000000u);
  reset();
}

void K2GE::reset() {
  std::memset(monoPal, 7, sizeof monoPal);
  raster = 0;
  s1x = 0; s1y = 0; s2x = 0; s2y = 0;
  wbaH = 0; wbaV = 0; wsiH = 0xFF; wsiV = 0xFF;
  cOvr = 0; blnk = 0;
  poH = 0; poV = 0; pf = 0; bgCol = 7;
  ctl2d = 0; ctlInt = 0; period = 0xC6; mode = 0;
  latch();
}

void K2GE::latch() {
  winx = wbaH; winy = wbaV; winw = wsiH; winh = wsiV;
  sc1x = s1x; sc1y = s1y; sc2x = s2x; sc2y = s2y;
  spx = poH; spy = poV;
  swap = pf & 0x80;
  bgc = bgCol;
  oowc = ctl2d & 7; neg = ctl2d & 0x80;
}

int K2GE::read8(uint32_t a) const {
  if (a >= 0x9000 && a < 0xA000) return scroll[a - 0x9000];
  if (a >= 0xA000 && a < 0xC000) return chr[a - 0xA000];
  if (a >= 0x8800 && a < 0x8900) return spr[a - 0x8800];
  if (a >= 0x8C00 && a < 0x8C40) return sprCol[a - 0x8C00];
  if (a >= 0x8200 && a < 0x8400) return pal[a - 0x8200];
  if (a >= 0x8100 && a < 0x8118) return monoPal[a - 0x8100];
  switch (a) {
    case 0x8000: return ctlInt;
    case 0x8002: return wbaH;
    case 0x8003: return wbaV;
    case 0x8004: return wsiH;
    case 0x8005: return wsiV;
    case 0x8006: return period;
    case 0x8008: return (std::abs(515 - sys.lineCycles) >> 2) & 0xFF;  // posição horizontal aproximada
    case 0x8009: return raster;
    case 0x8010: return (cOvr ? 0x80 : 0) | (blnk ? 0x40 : 0);
    case 0x8012: return ctl2d;
    case 0x8020: return poH;
    case 0x8021: return poV;
    case 0x8030: return pf;
    case 0x8032: return s1x;
    case 0x8033: return s1y;
    case 0x8034: return s2x;
    case 0x8035: return s2y;
    case 0x8118: return bgCol;
    case 0x87E2: return mode;
    case 0x87FE: return 0x3F;  // entrada do controle de expansão (nada ligado)
    default: return 0;
  }
}

void K2GE::write8(uint32_t a, int v) {
  if (a >= 0x9000 && a < 0xA000) { scroll[a - 0x9000] = static_cast<uint8_t>(v); return; }
  if (a >= 0xA000 && a < 0xC000) { chr[a - 0xA000] = static_cast<uint8_t>(v); return; }
  if (a >= 0x8800 && a < 0x8900) { spr[a - 0x8800] = static_cast<uint8_t>(v); return; }
  if (a >= 0x8C00 && a < 0x8C40) { sprCol[a - 0x8C00] = static_cast<uint8_t>(v & 0x0F); return; }
  if (a >= 0x8200 && a < 0x8400) { pal[a - 0x8200] = static_cast<uint8_t>((a & 1) ? v & 0x0F : v); return; }
  if (a >= 0x8100 && a < 0x8118) { if (a & 3) monoPal[a - 0x8100] = static_cast<uint8_t>(v & 7); return; }
  switch (a) {
    case 0x8000: ctlInt = v & 0xC0; break;
    case 0x8002: wbaH = v; break;
    case 0x8003: wbaV = v; break;
    case 0x8004: wsiH = v; break;
    case 0x8005: wsiV = v; break;
    case 0x8006: period = v; break;
    case 0x8012: ctl2d = v & 0x87; break;
    case 0x8020: poH = v; break;
    case 0x8021: poV = v; break;
    case 0x8030: pf = v & 0x80; break;
    case 0x8032: s1x = v; break;
    case 0x8033: s1y = v; break;
    case 0x8034: s2x = v; break;
    case 0x8035: s2y = v; break;
    case 0x8118: bgCol = v & 0xC7; break;
    case 0x87E0: if (v == 0x52) sys.requestReset = true; break;  // reinício pelo vídeo
    case 0x87E2: mode = v & 0x80; break;
    default: break;
  }
}

// chamada a cada 515 ciclos; devolve true se deve haver interrupção de linha (usada pelo timer 0)
bool K2GE::hblank() {
  if (raster < H - 1 || raster == period) {
    latch();
    if (ctlInt & 0x40) return true;
  }
  return false;
}

// desenha a linha atual e avança; devolve true no início do retraço vertical
bool K2GE::endLine() {
  if (raster < H) drawLine(raster);
  raster++;
  bool vbl = false;
  if (raster == H) { blnk = 1; vbl = true; }
  // o contador do chip tem 8 bits: com o período escrito abaixo da linha atual ele dá a volta em 256 (antes
  // seguia contando sem fim e o estado salvo não voltava igual)
  if (raster == period + 1 || raster > 0xFF) { raster = 0; cOvr = 0; blnk = 0; }
  return vbl;
}

void K2GE::drawLine(int y) {
  const bool mono = mode != 0;
  std::memset(z, 0, sizeof z);
  const int ng = neg ? 0xFFF : 0;
  const bool inWin = y >= winy && y < winy + winh;
  const int wx0 = winx, wx1 = std::min(winx + winw, W);
  // cor de fora da janela
  int out;
  if (mono) out = shade(oowc) ^ ng;
  else out = ((pal[0x1F0 + oowc * 2] | (pal[0x1F1 + oowc * 2] << 8)) & 0xFFF) ^ ng;
  std::fill(line, line + W, static_cast<uint16_t>(out));
  if (inWin) {
    int bg;
    if (mono) bg = ((bgc & 0xC0) == 0x80 ? shade(bgc) : 0xFFF) ^ ng;
    else { const int k = 0x1E0 + (bgc & 7) * 2; bg = ((pal[k] | (pal[k + 1] << 8)) & 0xFFF) ^ ng; }
    if (wx1 > wx0) std::fill(line + wx0, line + wx1, static_cast<uint16_t>(bg));
    if (swap) { plane(y, 0, 3, mono); plane(y, 1, 5, mono); }
    else { plane(y, 1, 3, mono); plane(y, 0, 5, mono); }
    sprites(y, mono);
  }
  const int o = y * W;
  for (int x = 0; x < W; x++) fb[o + x] = T.COLOR[line[x] & 0xFFF];
}

// plano 0 = rolagem 1 (9000), plano 1 = rolagem 2 (9800)
void K2GE::plane(int y, int p, int depth, bool mono) {
  const int sx = p ? sc2x : sc1x, sy = p ? sc2y : sc1y;
  const int ln = (y + sy) & 0xFF, row = ln & 7, base = (p ? 0x800 : 0) + ((ln >> 3) << 6);
  for (int i = 0; i < 32; i++) {
    const int d = scroll[base + i * 2] | (scroll[base + i * 2 + 1] << 8);
    pattern(((i << 3) - sx) & 0xFF, d & 0x1FF, (d & 0x4000) ? 7 - row : row, d & 0x8000,
            mono ? (p ? 0x10 : 0x08) + ((d & 0x2000) ? 4 : 0) : (p ? 0x100 : 0x80) + ((d >> 9) & 15) * 8, depth, mono);
  }
}

void K2GE::sprites(int y, bool mono) {
  int lx = 0, ly = 0;
  for (int n = 0; n < 64; n++) {
    const int d = spr[n * 4] | (spr[n * 4 + 1] << 8);
    int x = spr[n * 4 + 2], yy = spr[n * 4 + 3];
    if (d & 0x0400) x = lx + x;    // encadeado na horizontal
    if (d & 0x0200) yy = ly + yy;  // encadeado na vertical
    lx = x; ly = yy;
    const int prio = (d >> 11) & 3;
    if (!prio) continue;
    x += spx; yy += spy;
    x = (x > 248 && x < 256) ? x - 256 : x & 0xFF;
    yy = (yy > 248 && yy < 256) ? yy - 256 : yy & 0xFF;
    if (y < yy || y > yy + 7) continue;
    const int row = (y - yy) & 7;
    pattern(x & 0xFF, d & 0x1FF, (d & 0x4000) ? 7 - row : row, d & 0x8000,
            mono ? ((d & 0x2000) ? 4 : 0) : (sprCol[n] & 15) * 8, prio << 1, mono);
  }
}

// uma linha de um bloco na posição x (0–255; acima de 248 volta pela esquerda)
void K2GE::pattern(int sx, int tile, int row, int flip, int palBase, int depth, bool mono) {
  const int x = sx > 0xF8 ? sx - 256 : sx;
  if (x >= W) return;
  const int k = tile * 16 + row * 2;
  int d = chr[k] | (chr[k + 1] << 8);
  if (!d) return;
  if (flip) d = T.MIRROR16[d];
  const int left = std::max({x, winx, 0}), right = std::min(x + 7, std::min(winx + winw, W) - 1);
  const int ng = neg ? 0xFFF : 0;
  for (int xx = left; xx <= right; xx++) {
    const int c = (d >> ((7 - (xx - x)) * 2)) & 3;
    if (!c || depth <= z[xx]) continue;
    z[xx] = static_cast<uint8_t>(depth);
    int col;
    if (mono) col = shade(monoPal[palBase + c]);
    else col = (pal[palBase + c * 2] | (pal[palBase + c * 2 + 1] << 8)) & 0xFFF;
    line[xx] = static_cast<uint16_t>(col ^ ng);
  }
}

void K2GE::sync(StateIO& io) {
  io.raw(scroll, sizeof scroll); io.raw(chr, sizeof chr); io.raw(spr, sizeof spr); io.raw(sprCol, sizeof sprCol);
  io.raw(pal, sizeof pal); io.raw(monoPal, sizeof monoPal);
  for (int* p : {&raster, &s1x, &s1y, &s2x, &s2y, &wbaH, &wbaV, &wsiH, &wsiV, &cOvr, &blnk, &poH, &poV, &pf, &bgCol,
                 &ctl2d, &ctlInt, &period, &mode, &winx, &winy, &winw, &winh, &sc1x, &sc1y, &sc2x, &sc2y, &spx, &spy,
                 &swap, &bgc, &oowc, &neg})
    io.sync(*p);
  if (io.reading()) {
    for (int* p : {&raster, &s1x, &s1y, &s2x, &s2y, &wbaH, &wbaV, &wsiH, &wsiV, &poH, &poV, &period,
                   &winx, &winy, &winw, &winh, &sc1x, &sc1y, &sc2x, &sc2y, &spx, &spy})
      *p &= 0xFF;
    cOvr &= 1; blnk &= 1; pf &= 0x80; bgCol &= 0xC7; ctl2d &= 0x87; ctlInt &= 0xC0; mode &= 0x80;
    swap &= 0x80; bgc &= 0xC7; oowc &= 7; neg &= 0x80;
  }
}

}  // namespace indigo::ngp
