// PPU do GBA (porte de src/ppu.js): renderizador por scanline (modos 0-5, sprites, janelas, efeitos de cor, mosaico)
#include <algorithm>

#include "gba.h"

namespace indigo::gba {

namespace {
const int OBJ_W[12] = {8, 16, 32, 64, 16, 32, 32, 64, 8, 8, 16, 32};
const int OBJ_H[12] = {8, 16, 32, 64, 8, 8, 16, 32, 16, 32, 32, 64};

struct ColorLut {
  uint32_t lut[32768];
  ColorLut() {
    for (uint32_t c = 0; c < 32768; c++) {
      const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
      const uint32_t R = (r << 3) | (r >> 2), G = (g << 3) | (g >> 2), B = (b << 3) | (b >> 2);
      lut[c] = 0xFF000000u | (B << 16) | (G << 8) | R;
    }
  }
};
const ColorLut LUT;

inline int s16(int v) { return static_cast<int16_t>(v); }

int blend(int a, int b, int eva, int evb) {
  int r = ((a & 31) * eva + (b & 31) * evb) >> 4;
  int g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4;
  int bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
  if (r > 31) r = 31;
  if (g > 31) g = 31;
  if (bl > 31) bl = 31;
  return r | (g << 5) | (bl << 10);
}
int brighten(int a, int evy) {
  int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
  r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
  return r | (g << 5) | (b << 10);
}
int darken(int a, int evy) {
  int r = a & 31, g = (a >> 5) & 31, b = (a >> 10) & 31;
  r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
  return r | (g << 5) | (b << 10);
}
}  // namespace

PPU::PPU(GBA& g) : gba(g) { std::fill(fb, fb + 240 * 160, 0u); }

void PPU::latchAffine(int a) {
  const uint16_t* r = gba.io.regs;
  const int bg = a >= 0x38 ? 1 : 0;
  const int base = bg ? 0x38 : 0x28;
  refX[bg] = static_cast<int32_t>((U32(r[base >> 1]) | (U32(r[(base + 2) >> 1]) << 16)) << 4) >> 4;
  refY[bg] = static_cast<int32_t>((U32(r[(base + 4) >> 1]) | (U32(r[(base + 6) >> 1]) << 16)) << 4) >> 4;
}

void PPU::renderLine(int y) {
  const uint16_t* r = gba.io.regs;
  const int dispcnt = r[0];
  const int off = y * 240;
  if (dispcnt & 0x80) {
    for (int x = 0; x < 240; x++) fb[off + x] = 0xFFFFFFFFu;
    advanceAffine();
    return;
  }
  const int mode = dispcnt & 7;
  int bgMask;
  switch (mode) {
    case 0: bgMask = 0xF; break;
    case 1: bgMask = 0x7; break;
    case 2: bgMask = 0xC; break;
    case 3: case 4: case 5: bgMask = 0x4; break;
    default: bgMask = 0; break;
  }
  bgMask &= (dispcnt >> 8) & 0xF;

  const int mosaic = r[0x4C >> 1];
  for (int bg = 0; bg < 4; bg++) {
    if (!(bgMask & (1 << bg))) continue;
    int32_t* out = bgLine[bg];
    if (mode == 0 || (mode == 1 && bg < 2)) renderText(bg, y, out);
    else if (mode <= 2) renderAffine(bg, out);
    else renderBitmap(mode, dispcnt, out);
    const int cnt = r[(8 + bg * 2) >> 1];
    if ((cnt & 0x40) && (mosaic & 0xF)) {
      const int mw = (mosaic & 0xF) + 1;
      for (int x = 0; x < 240; x++) out[x] = out[x - (x % mw)];
    }
  }

  const bool objOn = (dispcnt & 0x1000) != 0;
  if (objOn || (dispcnt & 0x8000)) renderObjs(y, dispcnt, mode);

  compose(y, dispcnt, bgMask, objOn);
  advanceAffine();
}

void PPU::advanceAffine() {
  const uint16_t* r = gba.io.regs;
  refX[0] = I32(int64_t(refX[0]) + s16(r[0x22 >> 1]));
  refY[0] = I32(int64_t(refY[0]) + s16(r[0x26 >> 1]));
  refX[1] = I32(int64_t(refX[1]) + s16(r[0x32 >> 1]));
  refY[1] = I32(int64_t(refY[1]) + s16(r[0x36 >> 1]));
}

void PPU::renderText(int bg, int y, int32_t* out) {
  const uint16_t* r = gba.io.regs;
  const std::vector<uint8_t>& vram = gba.bus.vram;
  const std::vector<uint8_t>& pal = gba.bus.pal;
  const int cnt = r[(8 + bg * 2) >> 1];
  const int hofs = r[(0x10 + bg * 4) >> 1] & 0x1FF;
  const int vofs = r[(0x12 + bg * 4) >> 1] & 0x1FF;
  const int charBase = ((cnt >> 2) & 3) * 0x4000;
  const int screenBase = ((cnt >> 8) & 31) * 0x800;
  const bool c256 = (cnt & 0x80) != 0;
  const int size = cnt >> 14;
  const int w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
  int yy = y;
  const int mosaic = r[0x4C >> 1];
  if ((cnt & 0x40) && (mosaic & 0xF0)) { const int mh = ((mosaic >> 4) & 0xF) + 1; yy = y - (y % mh); }
  const int py = (yy + vofs) & (h - 1);
  const int sby = py >> 8;
  const int ty = py & 7;
  const int rowOff = ((py & 255) >> 3) * 64;
  const int blocksPerRow = w >> 8;
  int x = 0;
  while (x < 240) {
    const int px = (x + hofs) & (w - 1);
    const int block = (px >> 8) + sby * blocksPerRow;
    const int entry = Bus::ld16(vram, static_cast<size_t>(screenBase + block * 0x800 + rowOff + ((px & 255) >> 3) * 2) & ~size_t(1));
    const int tile = entry & 0x3FF;
    const int tyy = (entry & 0x800) ? 7 - ty : ty;
    const bool hflip = (entry & 0x400) != 0;
    // percorre os pixels restantes deste tile
    for (int tx = px & 7; tx < 8 && x < 240; tx++, x++) {
      const int sx = hflip ? 7 - tx : tx;
      int32_t color = -1;
      if (c256) {
        const int addr = charBase + tile * 64 + tyy * 8 + sx;
        if (addr < 0x10000) { const int idx = vram[addr]; if (idx) color = Bus::ld16(pal, idx * 2); }
      } else {
        const int addr = charBase + tile * 32 + tyy * 4 + (sx >> 1);
        if (addr < 0x10000) {
          const int b = vram[addr];
          const int idx = (sx & 1) ? (b >> 4) : (b & 15);
          if (idx) color = Bus::ld16(pal, (((entry >> 12) << 4) + idx) * 2);
        }
      }
      out[x] = color;
    }
  }
}

void PPU::renderAffine(int bg, int32_t* out) {
  const uint16_t* r = gba.io.regs;
  const std::vector<uint8_t>& vram = gba.bus.vram;
  const std::vector<uint8_t>& pal = gba.bus.pal;
  const int cnt = r[(8 + bg * 2) >> 1];
  const int charBase = ((cnt >> 2) & 3) * 0x4000;
  const int screenBase = ((cnt >> 8) & 31) * 0x800;
  const int size = 128 << (cnt >> 14);
  const bool wrap = (cnt & 0x2000) != 0;
  const int k = bg - 2;
  const int base = k ? 0x30 : 0x20;
  const int pa = s16(r[base >> 1]), pc = s16(r[(base + 4) >> 1]);
  int64_t fx = refX[k], fy = refY[k];
  const int tilesPerRow = size >> 3;
  for (int x = 0; x < 240; x++, fx += pa, fy += pc) {
    int tx = I32(fx) >> 8, ty = I32(fy) >> 8;
    if (wrap) { tx &= size - 1; ty &= size - 1; }
    else if (tx < 0 || ty < 0 || tx >= size || ty >= size) { out[x] = -1; continue; }
    const int tile = vram[screenBase + (ty >> 3) * tilesPerRow + (tx >> 3)];
    const int idx = vram[charBase + tile * 64 + (ty & 7) * 8 + (tx & 7)];
    out[x] = idx ? Bus::ld16(pal, idx * 2) : -1;
  }
}

void PPU::renderBitmap(int mode, int dispcnt, int32_t* out) {
  const uint16_t* r = gba.io.regs;
  const std::vector<uint8_t>& vram = gba.bus.vram;
  const int pa = s16(r[0x20 >> 1]), pc = s16(r[0x24 >> 1]);
  int64_t fx = refX[0], fy = refY[0];
  const int frame = (dispcnt & 0x10) ? 0xA000 : 0;
  if (mode == 3) {
    for (int x = 0; x < 240; x++, fx += pa, fy += pc) {
      const int tx = I32(fx) >> 8, ty = I32(fy) >> 8;
      out[x] = (tx < 0 || ty < 0 || tx >= 240 || ty >= 160) ? -1 : (Bus::ld16(vram, (ty * 240 + tx) * 2) & 0x7FFF);
    }
  } else if (mode == 4) {
    const std::vector<uint8_t>& pal = gba.bus.pal;
    for (int x = 0; x < 240; x++, fx += pa, fy += pc) {
      const int tx = I32(fx) >> 8, ty = I32(fy) >> 8;
      if (tx < 0 || ty < 0 || tx >= 240 || ty >= 160) { out[x] = -1; continue; }
      const int idx = vram[frame + ty * 240 + tx];
      out[x] = idx ? Bus::ld16(pal, idx * 2) : -1;
    }
  } else {
    const int b = frame >> 1;
    for (int x = 0; x < 240; x++, fx += pa, fy += pc) {
      const int tx = I32(fx) >> 8, ty = I32(fy) >> 8;
      out[x] = (tx < 0 || ty < 0 || tx >= 160 || ty >= 128) ? -1 : (Bus::ld16(vram, (b + ty * 160 + tx) * 2) & 0x7FFF);
    }
  }
}

void PPU::renderObjs(int y, int dispcnt, int mode) {
  const std::vector<uint8_t>& oam = gba.bus.oam;
  const std::vector<uint8_t>& vram = gba.bus.vram;
  const std::vector<uint8_t>& pal = gba.bus.pal;
  auto oam16 = [&oam](int i) { return static_cast<int>(Bus::ld16(oam, i * 2)); };
  std::fill(objColor, objColor + 240, -1);
  std::fill(objPrio, objPrio + 240, 4);
  std::fill(objAlpha, objAlpha + 240, 0);
  std::fill(objWin, objWin + 240, 0);
  const bool oneD = (dispcnt & 0x40) != 0;
  const bool bitmap = mode >= 3;
  const int mosaic = gba.io.regs[0x4C >> 1];
  const int mosH = ((mosaic >> 12) & 0xF) + 1;
  for (int i = 0; i < 128; i++) {
    const int a0 = oam16(i * 4), a1 = oam16(i * 4 + 1), a2 = oam16(i * 4 + 2);
    const bool affine = (a0 & 0x100) != 0;
    if (!affine && (a0 & 0x200)) continue;
    const int objMode = (a0 >> 10) & 3;
    if (objMode == 3) continue;
    const int shape = a0 >> 14;
    if (shape == 3) continue;
    const int sz = a1 >> 14;
    const int w = OBJ_W[shape * 4 + sz], h = OBJ_H[shape * 4 + sz];
    int bw = w, bh = h;
    if (affine && (a0 & 0x200)) { bw <<= 1; bh <<= 1; }
    const int sy = a0 & 0xFF;
    int dy = (y - sy) & 0xFF;
    if (dy >= bh) continue;
    if (a0 & 0x1000) dy = dy - (dy % mosH);
    int sx = a1 & 0x1FF;
    if (sx >= 240) sx -= 512;
    if (sx + bw <= 0) continue;
    const int tileBase = a2 & 0x3FF;
    if (bitmap && tileBase < 512) continue;
    const int prio = (a2 >> 10) & 3;
    const bool c256 = (a0 & 0x2000) != 0;
    const int palBank = 256 + ((a2 >> 12) << 4);
    const int rowStride = oneD ? (c256 ? (w >> 3) * 2 : (w >> 3)) : 32;
    const bool isWin = objMode == 2;
    const int semi = objMode == 1 ? 1 : 0;
    int pa = 256, pb = 0, pc = 0, pd = 256;
    if (affine) {
      const int p = ((a1 >> 9) & 31) * 16;
      pa = s16(oam16(p + 3)); pb = s16(oam16(p + 7));
      pc = s16(oam16(p + 11)); pd = s16(oam16(p + 15));
    }
    const int hw = bw >> 1, hh = bh >> 1;
    const int iy = dy - hh;
    const bool hflip = (a1 & 0x1000) != 0, vflip = (a1 & 0x2000) != 0;
    for (int bx = 0; bx < bw; bx++) {
      const int x = sx + bx;
      if (x < 0) continue;
      if (x >= 240) break;
      int tx, ty;
      if (affine) {
        const int ix = bx - hw;
        tx = ((pa * ix + pb * iy) >> 8) + (w >> 1);
        ty = ((pc * ix + pd * iy) >> 8) + (h >> 1);
        if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
      } else {
        tx = hflip ? w - 1 - bx : bx;
        ty = vflip ? h - 1 - dy : dy;
      }
      int32_t color = -1;
      if (c256) {
        const int tn = (tileBase & ~1) + (ty >> 3) * rowStride + (tx >> 3) * 2;
        const int idx = vram[0x10000 + ((tn & 0x3FF) << 5) + (ty & 7) * 8 + (tx & 7)];
        if (idx) color = Bus::ld16(pal, (256 + idx) * 2);
      } else {
        const int tn = tileBase + (ty >> 3) * rowStride + (tx >> 3);
        const int b = vram[0x10000 + ((tn & 0x3FF) << 5) + (ty & 7) * 4 + ((tx & 7) >> 1)];
        const int idx = (tx & 1) ? (b >> 4) : (b & 15);
        if (idx) color = Bus::ld16(pal, (palBank + idx) * 2);
      }
      if (color < 0) continue;
      if (isWin) { objWin[x] = 1; continue; }
      if (objColor[x] < 0 || prio < objPrio[x]) {
        objColor[x] = color; objPrio[x] = static_cast<uint8_t>(prio); objAlpha[x] = static_cast<uint8_t>(semi);
      }
    }
  }
}

void PPU::compose(int y, int dispcnt, int bgMask, bool objOn) {
  const uint16_t* r = gba.io.regs;
  const int off = y * 240;
  const int backdrop = Bus::ld16(gba.bus.pal, 0) & 0x7FFF;
  // ordem de prioridade dos BGs
  int prio[4] = {0, 0, 0, 0};
  int nbg = 0;
  for (int p = 0; p < 4; p++) {
    for (int bg = 0; bg < 4; bg++) {
      if (!(bgMask & (1 << bg))) continue;
      const int bp = r[(8 + bg * 2) >> 1] & 3;
      prio[bg] = bp;
      if (bp == p) order[nbg++] = bg;
    }
  }
  const int bldcnt = r[0x50 >> 1];
  const int effect = (bldcnt >> 6) & 3;
  const int t1 = bldcnt & 0x3F, t2 = (bldcnt >> 8) & 0x3F;
  const int bldalpha = r[0x52 >> 1];
  const int eva = std::min(16, bldalpha & 0x1F), evb = std::min(16, (bldalpha >> 8) & 0x1F);
  const int evy = std::min(16, r[0x54 >> 1] & 0x1F);

  const bool winAny = (dispcnt & 0xE000) != 0;
  const int winin = r[0x48 >> 1], winout = r[0x4A >> 1];
  bool w0y = false, w1y = false;
  int w0x1 = 0, w0x2 = 0, w1x1 = 0, w1x2 = 0;
  if (dispcnt & 0x2000) {
    const int hh = r[0x40 >> 1], vv = r[0x44 >> 1];
    w0x1 = hh >> 8; w0x2 = hh & 0xFF; if (w0x2 > 240 || (w0x1 > w0x2 && w0x2 == 0)) w0x2 = 240;
    const int y1 = vv >> 8;
    int y2 = vv & 0xFF; if (y2 > 160) y2 = 160;
    w0y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
  }
  if (dispcnt & 0x4000) {
    const int hh = r[0x42 >> 1], vv = r[0x46 >> 1];
    w1x1 = hh >> 8; w1x2 = hh & 0xFF; if (w1x2 > 240 || (w1x1 > w1x2 && w1x2 == 0)) w1x2 = 240;
    const int y1 = vv >> 8;
    int y2 = vv & 0xFF; if (y2 > 160) y2 = 160;
    w1y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
  }
  const bool objWinOn = (dispcnt & 0x8000) != 0 && objOn;

  for (int x = 0; x < 240; x++) {
    int wm = 0x3F;
    if (winAny) {
      if (w0y && (w0x1 <= w0x2 ? (x >= w0x1 && x < w0x2) : (x >= w0x1 || x < w0x2))) wm = winin & 0x3F;
      else if (w1y && (w1x1 <= w1x2 ? (x >= w1x1 && x < w1x2) : (x >= w1x1 || x < w1x2))) wm = (winin >> 8) & 0x3F;
      else if (objWinOn && objWin[x]) wm = (winout >> 8) & 0x3F;
      else wm = winout & 0x3F;
    }
    int c1 = backdrop, l1 = 5, c2 = backdrop, l2 = 5, found = 0;
    const int oc = (objOn && (wm & 0x10)) ? objColor[x] : -1;
    bool objDone = oc < 0;
    const int op = objPrio[x];
    for (int k = 0; k < nbg && found < 2; k++) {
      const int bg = order[k];
      if (!objDone && op <= prio[bg]) {
        objDone = true;
        if (found == 0) { c1 = oc; l1 = 4; } else { c2 = oc; l2 = 4; }
        found++;
        if (found == 2) break;
      }
      if (!(wm & (1 << bg))) continue;
      const int cc = bgLine[bg][x];
      if (cc < 0) continue;
      if (found == 0) { c1 = cc; l1 = bg; } else { c2 = cc; l2 = bg; }
      found++;
    }
    if (!objDone && found < 2) {
      if (found == 0) { c1 = oc; l1 = 4; } else { c2 = oc; l2 = 4; }
    }
    int out = c1;
    if (l1 == 4 && objAlpha[x] && (t2 & (1 << l2))) {
      out = blend(c1, c2, eva, evb);
    } else if ((wm & 0x20) && effect && (t1 & (1 << l1))) {
      if (effect == 1) { if (t2 & (1 << l2)) out = blend(c1, c2, eva, evb); }
      else if (effect == 2) out = brighten(c1, evy);
      else out = darken(c1, evy);
    }
    fb[off + x] = LUT.lut[out & 0x7FFF];
  }
}

void PPU::sync(StateIO& io) {
  io.raw(refX, sizeof refX); io.raw(refY, sizeof refY);
  io.raw(order, sizeof order);
  if (io.reading()) for (auto& o : order) o &= 3;
}

}  // namespace indigo::gba
