// Engines 2D do DS (A e B): BGs de texto, afins, estendidas (afim de 16 bits, bitmap de 256 cores e cor direta)
// e o bitmap grande do modo 6, sprites (comuns, afins, de 256 cores com paletas estendidas, bitmap), janelas,
// efeitos de cor, a camada 3D na BG0 da engine A e a captura de tela. Escrito a partir do GBATEK.
#include <algorithm>
#include <cstring>

#include "nds.h"

namespace indigo::nds {

namespace {
inline uint16_t blendAlpha(uint16_t a, uint16_t b, int eva, int evb) {
  int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
  int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
  int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
  if (r > 31) r = 31;
  if (g > 31) g = 31;
  if (bl > 31) bl = 31;
  return static_cast<uint16_t>(r | (g << 5) | (bl << 10));
}
inline uint16_t brighten(uint16_t a, int evy) {
  int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
  r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
  return static_cast<uint16_t>(r | (g << 5) | (b << 10));
}
inline uint16_t darken(uint16_t a, int evy) {
  int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
  r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
  return static_cast<uint16_t>(r | (g << 5) | (b << 10));
}
inline uint16_t blend3d(uint16_t a, int alpha, uint16_t b) {
  const int ea = alpha + 1, eb = 31 - alpha;
  const int r = ((a & 31) * ea + (b & 31) * eb) >> 5;
  const int g = (((a >> 5) & 31) * ea + ((b >> 5) & 31) * eb) >> 5;
  const int bl = (((a >> 10) & 31) * ea + ((b >> 10) & 31) * eb) >> 5;
  return static_cast<uint16_t>(r | (g << 5) | (bl << 10));
}
const int OBJ_W[4][4] = {{8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}, {8, 8, 8, 8}};
const int OBJ_H[4][4] = {{8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}, {8, 8, 8, 8}};
}  // namespace

void Gpu2D::reset() {
  std::memset(reg, 0, sizeof reg);
  refX[0] = refX[1] = refY[0] = refY[1] = 0;
  std::memset(line, 0, sizeof line);
  capturing = false;
}

// ------------------------------------------------------------------ registradores
void Gpu2D::writeReg(uint32_t off, uint32_t v, int size) {
  for (int k = 0; k < size; k++) {
    const uint32_t o = off + k;
    if (o >= 0x70) break;
    uint16_t& r = reg[o >> 1];
    const uint8_t b = uint8_t(v >> (8 * k));
    r = (o & 1) ? uint16_t((r & 0x00FF) | (b << 8)) : uint16_t((r & 0xFF00) | b);
  }
  const uint32_t end = off + size;
  if (off < 0x30 && end > 0x28) latchAffine(2);
  if (off < 0x40 && end > 0x38) latchAffine(3);
  if (isA && off < 0x62 && end > 0x60) s.gpu3d.disp3dcnt = reg[0x30];
}

uint32_t Gpu2D::readReg(uint32_t off, int size) const {
  auto r16 = [&](uint32_t o) -> uint32_t {
    switch (o) {
      case 0x00: case 0x02: case 0x08: case 0x0A: case 0x0C: case 0x0E:
      case 0x48: case 0x4A: case 0x50: case 0x52: case 0x6C:
        return reg[o >> 1];
      case 0x60: case 0x64: case 0x66: return isA ? reg[o >> 1] : 0;
      default: return 0;
    }
  };
  const uint32_t v = r16(off & ~1u);
  if (size == 1) return (off & 1) ? v >> 8 : v & 0xFF;
  if (size == 4) return v | (r16((off & ~1u) + 2) << 16);
  return v;
}

void Gpu2D::latchAffine(int bg) {
  const int i = bg - 2;
  const int base = 0x28 / 2 + i * 8;
  int32_t x = reg[base] | (int32_t(reg[base + 1]) << 16);
  int32_t y = reg[base + 2] | (int32_t(reg[base + 3]) << 16);
  refX[i] = (x << 4) >> 4;  // 28 bits com sinal
  refY[i] = (y << 4) >> 4;
}

// ------------------------------------------------------------------ memória
uint8_t Gpu2D::bgVram8(uint32_t a) const {
  const int32_t p = isA ? s.pgBgA[(a >> 14) & 31] : s.pgBgB[(a >> 14) & 7];
  return p < 0 ? 0 : s.vram[p + (a & 0x3FFF)];
}
uint16_t Gpu2D::bgVram16(uint32_t a) const {
  const int32_t p = isA ? s.pgBgA[(a >> 14) & 31] : s.pgBgB[(a >> 14) & 7];
  if (p < 0) return 0;
  const uint8_t* q = &s.vram[p + (a & 0x3FFE)];
  return uint16_t(q[0] | (q[1] << 8));
}
uint8_t Gpu2D::objVram8(uint32_t a) const {
  const int32_t p = isA ? s.pgObjA[(a >> 14) & 15] : s.pgObjB[(a >> 14) & 7];
  return p < 0 ? 0 : s.vram[p + (a & 0x3FFF)];
}
uint16_t Gpu2D::objVram16(uint32_t a) const {
  const int32_t p = isA ? s.pgObjA[(a >> 14) & 15] : s.pgObjB[(a >> 14) & 7];
  if (p < 0) return 0;
  const uint8_t* q = &s.vram[p + (a & 0x3FFE)];
  return uint16_t(q[0] | (q[1] << 8));
}
uint16_t Gpu2D::pal(int idx) const {
  const uint8_t* q = &s.pal[(isA ? 0 : 0x400) + ((idx & 0xFF) << 1)];
  return uint16_t((q[0] | (q[1] << 8)) & 0x7FFF);
}
uint16_t Gpu2D::objPal(int idx) const {
  const uint8_t* q = &s.pal[(isA ? 0x200 : 0x600) + ((idx & 0xFF) << 1)];
  return uint16_t((q[0] | (q[1] << 8)) & 0x7FFF);
}
uint16_t Gpu2D::bgExtPal(int slot, int idx) const {
  const int32_t p = isA ? s.pgBgExtA[slot & 3] : s.pgBgExtB[slot & 3];
  if (p < 0) return 0;
  const uint8_t* q = &s.vram[p + ((idx & 0xFFF) << 1)];
  return uint16_t((q[0] | (q[1] << 8)) & 0x7FFF);
}
uint16_t Gpu2D::objExtPal(int idx) const {
  const int32_t p = isA ? s.pgObjExtA : s.pgObjExtB;
  if (p < 0) return 0;
  const uint8_t* q = &s.vram[p + ((idx & 0xFFF) << 1)];
  return uint16_t((q[0] | (q[1] << 8)) & 0x7FFF);
}

// ------------------------------------------------------------------ BGs
void Gpu2D::renderText(int bg, int y) {
  uint16_t* out = bgLine[bg];
  const uint32_t dc = dispcnt();
  const uint16_t cnt = reg[4 + bg];
  uint32_t charBase = ((cnt >> 2) & 15) * 0x4000u, screenBase = ((cnt >> 8) & 31) * 0x800u;
  if (isA) { charBase += ((dc >> 24) & 7) * 0x10000u; screenBase += ((dc >> 27) & 7) * 0x10000u; }
  const bool c256 = cnt & 0x80;
  const int size = cnt >> 14;
  const int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
  const int hofs = reg[8 + bg * 2] & 0x1FF, vofs = reg[9 + bg * 2] & 0x1FF;
  const int yy = (y + vofs) & (h - 1);
  const bool ext = c256 && (dc & (1u << 30));
  const int slot = (bg < 2 && (cnt & 0x2000)) ? bg + 2 : bg;
  uint32_t rowBase = screenBase + ((yy >> 3) & 31) * 64;
  if (yy >= 256) rowBase += (w == 512) ? 0x1000 : 0x800;
  int x = 0;
  while (x < SCREEN_W) {
    const int xx = (x + hofs) & (w - 1);
    uint32_t sb = rowBase + ((xx >> 3) & 31) * 2;
    if (xx >= 256) sb += 0x800;
    const uint16_t e = bgVram16(sb);
    const int tile = e & 0x3FF;
    const int py = (e & 0x800) ? 7 - (yy & 7) : (yy & 7);
    const bool hf = e & 0x400;
    const int palNo = e >> 12;
    for (int px = xx & 7; px < 8 && x < SCREEN_W; px++, x++) {
      const int tx = hf ? 7 - px : px;
      uint16_t col = 0;
      if (c256) {
        const int idx = bgVram8(charBase + tile * 64 + py * 8 + tx);
        if (idx) col = 0x8000 | (ext ? bgExtPal(slot, palNo * 256 + idx) : pal(idx));
      } else {
        const int b = bgVram8(charBase + tile * 32 + py * 4 + (tx >> 1));
        const int idx = (tx & 1) ? b >> 4 : b & 15;
        if (idx) col = 0x8000 | pal(palNo * 16 + idx);
      }
      out[x] = col;
    }
  }
}

void Gpu2D::renderAffine(int bg, int) {
  uint16_t* out = bgLine[bg];
  const uint32_t dc = dispcnt();
  const uint16_t cnt = reg[4 + bg];
  uint32_t charBase = ((cnt >> 2) & 15) * 0x4000u, screenBase = ((cnt >> 8) & 31) * 0x800u;
  if (isA) { charBase += ((dc >> 24) & 7) * 0x10000u; screenBase += ((dc >> 27) & 7) * 0x10000u; }
  const int size = 128 << (cnt >> 14);
  const bool wrap = cnt & 0x2000;
  const int i = bg - 2;
  const int32_t pa = int16_t(reg[0x10 + i * 8]), pc = int16_t(reg[0x12 + i * 8]);
  int32_t fx = refX[i], fy = refY[i];
  for (int x = 0; x < SCREEN_W; x++, fx += pa, fy += pc) {
    int tx = fx >> 8, ty = fy >> 8;
    if (wrap) { tx &= size - 1; ty &= size - 1; }
    else if (tx < 0 || ty < 0 || tx >= size || ty >= size) { out[x] = 0; continue; }
    const int tile = bgVram8(screenBase + (ty >> 3) * (size >> 3) + (tx >> 3));
    const int idx = bgVram8(charBase + tile * 64 + (ty & 7) * 8 + (tx & 7));
    out[x] = idx ? (0x8000 | pal(idx)) : 0;
  }
}

void Gpu2D::renderExtended(int bg, int) {
  uint16_t* out = bgLine[bg];
  const uint32_t dc = dispcnt();
  const uint16_t cnt = reg[4 + bg];
  const int i = bg - 2;
  const int32_t pa = int16_t(reg[0x10 + i * 8]), pc = int16_t(reg[0x12 + i * 8]);
  int32_t fx = refX[i], fy = refY[i];
  const bool wrap = cnt & 0x2000;
  if (!(cnt & 0x80)) {
    // afim com entradas de 16 bits (256 cores, paletas estendidas)
    uint32_t charBase = ((cnt >> 2) & 15) * 0x4000u, screenBase = ((cnt >> 8) & 31) * 0x800u;
    if (isA) { charBase += ((dc >> 24) & 7) * 0x10000u; screenBase += ((dc >> 27) & 7) * 0x10000u; }
    const int size = 128 << (cnt >> 14);
    const bool ext = dc & (1u << 30);
    for (int x = 0; x < SCREEN_W; x++, fx += pa, fy += pc) {
      int tx = fx >> 8, ty = fy >> 8;
      if (wrap) { tx &= size - 1; ty &= size - 1; }
      else if (tx < 0 || ty < 0 || tx >= size || ty >= size) { out[x] = 0; continue; }
      const uint16_t e = bgVram16(screenBase + ((ty >> 3) * (size >> 3) + (tx >> 3)) * 2);
      int px = tx & 7, py = ty & 7;
      if (e & 0x400) px = 7 - px;
      if (e & 0x800) py = 7 - py;
      const int idx = bgVram8(charBase + (e & 0x3FF) * 64 + py * 8 + px);
      out[x] = idx ? (0x8000 | (ext ? bgExtPal(bg, (e >> 12) * 256 + idx) : pal(idx))) : 0;
    }
    return;
  }
  static const int BW[4] = {128, 256, 512, 512}, BH[4] = {128, 256, 256, 512};
  const int w = BW[cnt >> 14], h = BH[cnt >> 14];
  const uint32_t base = ((cnt >> 8) & 31) * 0x4000u;
  const bool direct = cnt & 4;
  for (int x = 0; x < SCREEN_W; x++, fx += pa, fy += pc) {
    int tx = fx >> 8, ty = fy >> 8;
    if (wrap) { tx &= w - 1; ty &= h - 1; }
    else if (tx < 0 || ty < 0 || tx >= w || ty >= h) { out[x] = 0; continue; }
    if (direct) {
      const uint16_t c = bgVram16(base + (ty * w + tx) * 2);
      out[x] = (c & 0x8000) ? c : 0;
    } else {
      const int idx = bgVram8(base + ty * w + tx);
      out[x] = idx ? (0x8000 | pal(idx)) : 0;
    }
  }
}

void Gpu2D::renderLarge(int bg, int) {
  uint16_t* out = bgLine[bg];
  const uint16_t cnt = reg[4 + bg];
  const int w = (cnt & 0x4000) ? 1024 : 512, h = (cnt & 0x4000) ? 512 : 1024;
  const int32_t pa = int16_t(reg[0x10]), pc = int16_t(reg[0x12]);
  int32_t fx = refX[0], fy = refY[0];
  const bool wrap = cnt & 0x2000;
  for (int x = 0; x < SCREEN_W; x++, fx += pa, fy += pc) {
    int tx = fx >> 8, ty = fy >> 8;
    if (wrap) { tx &= w - 1; ty &= h - 1; }
    else if (tx < 0 || ty < 0 || tx >= w || ty >= h) { out[x] = 0; continue; }
    const int idx = bgVram8(uint32_t(ty * w + tx));
    out[x] = idx ? (0x8000 | pal(idx)) : 0;
  }
}

void Gpu2D::render3d(int y) {
  uint16_t* out = bgLine[0];
  const int hofs = reg[8] & 0x1FF;
  const uint16_t* src = &s.gpu3d.color[y * SCREEN_W];
  const uint8_t* al = &s.gpu3d.alpha[y * SCREEN_W];
  for (int x = 0; x < SCREEN_W; x++) {
    const int sx = (x + hofs) & 0x1FF;
    if (sx >= SCREEN_W || !(src[sx] & 0x8000) || al[sx] == 0) { out[x] = 0; alpha3d[x] = 0; continue; }
    out[x] = src[sx];
    alpha3d[x] = al[sx];
  }
}

// ------------------------------------------------------------------ sprites
void Gpu2D::renderObjs(int y) {
  const uint32_t dc = dispcnt();
  std::memset(objOpaque, 0, sizeof objOpaque);
  std::memset(objWin, 0, sizeof objWin);
  for (int x = 0; x < SCREEN_W; x++) objPrio[x] = 4;
  const uint8_t* oamBase = &s.oam[isA ? 0 : 0x400];
  const bool map1d = dc & 0x10;
  const uint32_t bound = 32u << ((dc >> 20) & 3);
  const bool extPal = dc & (1u << 31);
  for (int i = 127; i >= 0; i--) {
    const uint8_t* o = oamBase + i * 8;
    const uint16_t a0 = uint16_t(o[0] | (o[1] << 8)), a1 = uint16_t(o[2] | (o[3] << 8)), a2 = uint16_t(o[4] | (o[5] << 8));
    const bool affine = a0 & 0x100;
    if (!affine && (a0 & 0x200)) continue;
    const int mode = (a0 >> 10) & 3;
    const int shape = (a0 >> 14) & 3, sz = (a1 >> 14) & 3;
    if (shape == 3) continue;
    const int w = OBJ_W[shape][sz], h = OBJ_H[shape][sz];
    const bool dbl = affine && (a0 & 0x200);
    const int w2 = dbl ? w * 2 : w, h2 = dbl ? h * 2 : h;
    const int dy = (y - (a0 & 0xFF)) & 0xFF;
    if (dy >= h2) continue;
    int x0 = a1 & 0x1FF;
    if (x0 >= 256) x0 -= 512;
    const int tile = a2 & 0x3FF, prio = (a2 >> 10) & 3, palNo = a2 >> 12;
    const bool c256 = a0 & 0x2000;
    int32_t pa = 256, pb = 0, pc = 0, pd = 256;
    if (affine) {
      const int g = (a1 >> 9) & 31;
      const uint8_t* p = oamBase + g * 32 + 6;
      pa = int16_t(p[0] | (p[1] << 8)); pb = int16_t(p[8] | (p[9] << 8));
      pc = int16_t(p[16] | (p[17] << 8)); pd = int16_t(p[24] | (p[25] << 8));
    }
    const bool hf = !affine && (a1 & 0x1000), vf = !affine && (a1 & 0x2000);
    for (int sx = 0; sx < w2; sx++) {
      const int px = x0 + sx;
      if (px < 0 || px >= SCREEN_W) continue;
      int tx, ty;
      if (affine) {
        const int ix = sx - w2 / 2, iy = dy - h2 / 2;
        tx = ((pa * ix + pb * iy) >> 8) + w / 2;
        ty = ((pc * ix + pd * iy) >> 8) + h / 2;
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
      } else {
        tx = hf ? w - 1 - sx : sx;
        ty = vf ? h - 1 - dy : dy;
      }
      uint16_t col;
      int alpha = 16;
      if (mode == 3) {  // sprite bitmap
        uint32_t addr;
        if (dc & 0x40) {
          addr = tile * ((dc & (1u << 22)) ? 256u : 128u) + (ty * w + tx) * 2;
        } else if (dc & 0x20) {
          addr = (tile & 0x1F) * 0x10 + (tile & 0x3E0) * 0x80 + (ty * 256 + tx) * 2;
        } else {
          addr = (tile & 0x0F) * 0x10 + (tile & 0x3F0) * 0x80 + (ty * 128 + tx) * 2;
        }
        const uint16_t c = objVram16(addr);
        if (!(c & 0x8000) || palNo == 0) continue;
        col = c & 0x7FFF;
        alpha = palNo + 1;
      } else {
        uint32_t addr;
        if (map1d) {
          const uint32_t base = tile * bound;
          const int t = (ty >> 3) * (w >> 3) + (tx >> 3);
          addr = c256 ? base + t * 64 + (ty & 7) * 8 + (tx & 7) : base + t * 32 + (ty & 7) * 4 + ((tx & 7) >> 1);
        } else {
          addr = tile * 32 + (ty >> 3) * 1024 + (c256 ? (tx >> 3) * 64 + (ty & 7) * 8 + (tx & 7)
                                                     : (tx >> 3) * 32 + (ty & 7) * 4 + ((tx & 7) >> 1));
        }
        int idx;
        if (c256) {
          idx = objVram8(addr);
          if (!idx) continue;
          col = extPal ? objExtPal(palNo * 256 + idx) : objPal(idx);
        } else {
          const int b = objVram8(addr);
          idx = (tx & 1) ? b >> 4 : b & 15;
          if (!idx) continue;
          col = objPal(palNo * 16 + idx);
        }
      }
      if (mode == 2) { objWin[px] = 1; continue; }
      if (prio <= objPrio[px]) {
        objColor[px] = col;
        objPrio[px] = static_cast<uint8_t>(prio);
        objMode[px] = static_cast<uint8_t>(mode);
        objAlpha[px] = static_cast<uint8_t>(alpha);
        objOpaque[px] = true;
      }
    }
  }
}

// ------------------------------------------------------------------ composição
void Gpu2D::compose(int) {
  const uint32_t dc = dispcnt();
  const bool winOn = dc & 0xE000;
  const uint16_t winin = reg[0x48 / 2], winout = reg[0x4A / 2];
  const uint16_t bldcnt = reg[0x50 / 2], bldalpha = reg[0x52 / 2];
  const int bmode = (bldcnt >> 6) & 3;
  int eva = bldalpha & 31, evb = (bldalpha >> 8) & 31, evy = reg[0x54 / 2] & 31;
  if (eva > 16) eva = 16;
  if (evb > 16) evb = 16;
  if (evy > 16) evy = 16;
  const uint16_t backdrop = pal(0);
  const bool is3d = isA && (dc & 8);
  // BGs ligadas em ordem de prioridade (a de número menor ganha no empate)
  int order[4], prio[4], nbg = 0;
  for (int p = 0; p < 4; p++)
    for (int bg = 0; bg < 4; bg++)
      if ((dc & (0x100 << bg)) && (reg[4 + bg] & 3) == p) { order[nbg] = bg; prio[nbg] = p; nbg++; }
  // máscara das janelas por pixel
  uint8_t winMask[SCREEN_W];
  if (winOn) {
    const int y = s.vcount;
    std::memset(winMask, winout & 0x3F, sizeof winMask);
    if (dc & 0x8000) {
      const uint8_t m = (winout >> 8) & 0x3F;
      for (int x = 0; x < SCREEN_W; x++) if (objWin[x]) winMask[x] = m;
    }
    for (int k = 1; k >= 0; k--) {
      if (!(dc & (0x2000 << k))) continue;
      const uint16_t wv = reg[(0x44 + k * 2) / 2];
      const int y1 = wv >> 8, y2 = wv & 0xFF;
      const bool inY = (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
      if (!inY) continue;
      const uint16_t wh = reg[(0x40 + k * 2) / 2];
      const int x1 = wh >> 8, x2 = wh & 0xFF;
      const uint8_t m = (winin >> (8 * k)) & 0x3F;
      for (int x = 0; x < SCREEN_W; x++) {
        const bool in = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
        if (in) winMask[x] = m;
      }
    }
  }
  const bool anyEffect = bmode != 0 || (dc & 0x1000) || is3d;
  for (int x = 0; x < SCREEN_W; x++) {
    const int mask = winOn ? winMask[x] : 0x3F;
    // as duas camadas de cima
    int id1 = 5, id2 = 5;
    uint16_t c1 = backdrop, c2 = backdrop;
    int found = 0;
    bool objPending = objOpaque[x] && (mask & 0x10);
    const int op = objPrio[x];
    for (int k = 0; k < nbg && found < 2; k++) {
      if (objPending && op <= prio[k]) {
        if (found == 0) { id1 = 4; c1 = objColor[x]; } else { id2 = 4; c2 = objColor[x]; }
        found++;
        objPending = false;
        if (found == 2) break;
      }
      const int bg = order[k];
      if (!(mask & (1 << bg))) continue;
      const uint16_t c = bgLine[bg][x];
      if (!(c & 0x8000)) continue;
      if (found == 0) { id1 = bg; c1 = c & 0x7FFF; } else { id2 = bg; c2 = c & 0x7FFF; }
      found++;
    }
    if (found < 2 && objPending) {
      if (found == 0) { id1 = 4; c1 = objColor[x]; } else { id2 = 4; c2 = objColor[x]; }
    }
    uint16_t out = c1;
    if (anyEffect) {
      const bool second = (bldcnt >> (8 + id2)) & 1;
      if (id1 == 4 && (objMode[x] == 1 || objMode[x] == 3) && second) {
        const int a = objMode[x] == 3 ? objAlpha[x] : eva;
        const int b = objMode[x] == 3 ? 16 - objAlpha[x] : evb;
        out = blendAlpha(c1, c2, a, b);
      } else if (id1 == 0 && is3d && alpha3d[x] < 31 && second) {
        out = blend3d(c1, alpha3d[x], c2);
      } else if ((mask & 0x20) && ((bldcnt >> id1) & 1)) {
        if (bmode == 1 && second) out = blendAlpha(c1, c2, eva, evb);
        else if (bmode == 2) out = brighten(c1, evy);
        else if (bmode == 3) out = darken(c1, evy);
      }
    }
    line[x] = out;
  }
}

// ------------------------------------------------------------------ linha
void Gpu2D::renderLine(int y) {
  const uint32_t dc = dispcnt();
  int dispMode = (dc >> 16) & 3;
  if (!isA) dispMode &= 1;
  // captura de tela começa no quadro em que o bit 31 está ligado na linha 0
  const uint32_t cap = isA ? (reg[0x64 / 2] | (uint32_t(reg[0x66 / 2]) << 16)) : 0;
  if (isA && y == 0) capturing = cap & 0x80000000u;
  const bool needCompose = dispMode == 1 || (capturing && !(cap & 0x01000000u));
  if (needCompose) {
    const int mode = dc & 7;
    for (int bg = 0; bg < 4; bg++) std::memset(bgLine[bg], 0, sizeof bgLine[bg]);
    if (dc & 0x100) { if (isA && (dc & 8)) render3d(y); else renderText(0, y); }
    if (dc & 0x200) renderText(1, y);
    if (dc & 0x400) {
      switch (mode) {
        case 0: case 1: case 3: renderText(2, y); break;
        case 2: case 4: renderAffine(2, y); break;
        case 5: renderExtended(2, y); break;
        case 6: if (isA) renderLarge(2, y); break;
        default: break;
      }
    }
    if (dc & 0x800) {
      switch (mode) {
        case 0: renderText(3, y); break;
        case 1: case 2: renderAffine(3, y); break;
        case 3: case 4: case 5: renderExtended(3, y); break;
        default: break;
      }
    }
    if (dc & 0x1000) renderObjs(y);
    else { std::memset(objOpaque, 0, sizeof objOpaque); std::memset(objWin, 0, sizeof objWin); }
    compose(y);
  }
  uint16_t composed[SCREEN_W];
  if (capturing) std::memcpy(composed, line, sizeof composed);
  switch (dispMode) {
    case 0: for (auto& p : line) p = 0x7FFF; break;
    case 2: {
      const uint32_t base = ((dc >> 18) & 3) * 0x20000u + y * SCREEN_W * 2;
      for (int x = 0; x < SCREEN_W; x++) line[x] = uint16_t((s.vram[base + x * 2] | (s.vram[base + x * 2 + 1] << 8)) & 0x7FFF);
      break;
    }
    case 3: for (auto& p : line) p = 0; break;
    default: break;
  }
  if (capturing) capture(y, composed);
  // referências afins andam a cada linha
  for (int i = 0; i < 2; i++) {
    refX[i] += int16_t(reg[0x11 + i * 8]);
    refY[i] += int16_t(reg[0x13 + i * 8]);
  }
}

void Gpu2D::capture(int y, const uint16_t* srcA2d) {
  const uint32_t cap = reg[0x64 / 2] | (uint32_t(reg[0x66 / 2]) << 16);
  static const int CW[4] = {128, 256, 256, 256}, CH[4] = {128, 64, 128, 192};
  const int sz = (cap >> 20) & 3;
  const int w = CW[sz], h = CH[sz];
  if (y >= h) { capturing = false; reg[0x66 / 2] &= 0x7FFF; return; }
  const int wbank = (cap >> 16) & 3;
  if ((s.vramcnt[wbank] & 0x87) != 0x80) return;  // o banco precisa estar no modo LCDC
  const uint32_t woff = ((cap >> 18) & 3) * 0x8000u;
  const uint32_t dc = dispcnt();
  const uint32_t rbank = (dc >> 18) & 3;
  const uint32_t roff = ((cap >> 26) & 3) * 0x8000u;
  const int src = (cap >> 29) & 3;
  const int eva = std::min<int>(cap & 31, 16), evb = std::min<int>((cap >> 8) & 31, 16);
  for (int x = 0; x < w; x++) {
    uint16_t a;
    if (cap & 0x01000000u) {
      const int i = y * SCREEN_W + x;
      a = (s.gpu3d.color[i] & 0x8000) && s.gpu3d.alpha[i] ? s.gpu3d.color[i] : 0;
    } else a = srcA2d[x] | 0x8000;
    const uint32_t ra = rbank * 0x20000u + ((roff + (y * SCREEN_W + x) * 2) & 0x1FFFF);
    const uint16_t b = (cap & 0x02000000u) ? 0 : uint16_t(s.vram[ra] | (s.vram[ra + 1] << 8));
    uint16_t o;
    if (src == 0) o = a;
    else if (src == 1) o = b;
    else {
      const int fa = (a & 0x8000) ? eva : 0, fb = (b & 0x8000) ? evb : 0;
      o = blendAlpha(a, b, fa, fb) | ((fa || fb) ? 0x8000 : 0);
    }
    const uint32_t wa = wbank * 0x20000u + ((woff + (y * w + x) * 2) & 0x1FFFF);
    s.vram[wa] = uint8_t(o);
    s.vram[wa + 1] = uint8_t(o >> 8);
  }
  if (y == h - 1) { capturing = false; reg[0x66 / 2] &= 0x7FFF; }  // fim da captura (192 linhas não têm linha seguinte)
}

void Gpu2D::sync(StateIO& io) {
  io.raw(reg, sizeof reg);
  io.raw(refX, sizeof refX);
  io.raw(refY, sizeof refY);
  io.sync(capturing);
}

}  // namespace indigo::nds
