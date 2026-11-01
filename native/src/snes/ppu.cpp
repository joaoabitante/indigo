// PPU do SNES, desenhada linha a linha (com os registradores como estão no fim da linha, antes do HDMA da
// próxima): modos 0–7, fundos de 2/4/8 bits por pixel com telhas de 8 ou 16, deslocamento por coluna (modos 2,
// 4 e 6), Mode 7 com EXTBG, 128 sprites (32 por linha, 34 telhas), duas janelas por camada, tela principal e
// secundária, soma/subtração de cores com metade, cor direta, mosaico, alta resolução (modos 5/6 e pseudo) e
// brilho.
#include <algorithm>
#include <cstring>

#include "snes.h"

namespace indigo::snes {

namespace {
constexpr int OBJ_W[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {16, 32}, {16, 32}};
constexpr int OBJ_H[8][2] = {{8, 16}, {8, 32}, {8, 64}, {16, 32}, {16, 64}, {32, 64}, {32, 64}, {32, 32}};
inline uint32_t toRgba(uint16_t c) {
  const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return 0xFF000000u | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
}
inline uint16_t directColor(int idx, int pal) {
  return static_cast<uint16_t>(((idx << 2) & 0x001C) + ((pal << 1) & 0x0002) + ((idx << 4) & 0x0380) + ((pal << 5) & 0x0040) +
                               ((idx << 7) & 0x6000) + ((pal << 10) & 0x1000));
}
}  // namespace

void PPU::reset() {
  inidisp = 0x80;
  obsel = bgmode = mosaic = 0;
  std::memset(bgsc, 0, sizeof bgsc);
  std::memset(bgnba, 0, sizeof bgnba);
  std::memset(hofs, 0, sizeof hofs);
  std::memset(vofs, 0, sizeof vofs);
  ofsPrev = ofsPrev2 = 0;
  vmain = 0; vaddr = 0; vlatch = 0;
  m7sel = m7prev = 0;
  m7a = m7b = m7c = m7d = m7x = m7y = m7hofs = m7vofs = 0;
  oamAddr = oamAddrReload = 0; oamLatch = 0; oamPriority = false;
  cgAddr = 0; cgLatch = 0;
  std::memset(wsel, 0, sizeof wsel);
  std::memset(wpos, 0, sizeof wpos);
  wbglog = wobjlog = tm = ts = tmw = tsw = cgwsel = cgadsub = setini = 0;
  fixedColor = 0;
  hLatch = vLatch = 0; counterLatched = hFlip = vFlip = false;
  ppu1Mdr = ppu2Mdr = 0;
  timeOver = rangeOver = false;
  rendering = false;
  mosaicStartY = 1;
  field = false;
  std::fill(std::begin(fb), std::end(fb), 0xFF000000u);
}

void PPU::vblankStart() {
  if (!forceBlank()) oamAddr = oamAddrReload;
}

// ---------------------------------------------------------------- VRAM
uint16_t PPU::vramAddrRemap() const {
  const uint16_t a = vaddr;
  switch ((vmain >> 2) & 3) {
    case 1: return static_cast<uint16_t>((a & 0xFF00) | ((a & 0x001F) << 3) | ((a >> 5) & 7));
    case 2: return static_cast<uint16_t>((a & 0xFE00) | ((a & 0x003F) << 3) | ((a >> 6) & 7));
    case 3: return static_cast<uint16_t>((a & 0xFC00) | ((a & 0x007F) << 3) | ((a >> 7) & 7));
    default: return a;
  }
}

void PPU::vramPrefetch() { vlatch = vramWord(vramAddrRemap()); }

// ---------------------------------------------------------------- registradores
void PPU::write(uint8_t reg, uint8_t v) {
  static const int STEP[4] = {1, 32, 128, 128};
  switch (reg) {
    case 0x00: inidisp = v; break;
    case 0x01: obsel = v; break;
    case 0x02: oamAddrReload = static_cast<uint16_t>((oamAddrReload & 0x200) | (v << 1)); oamAddr = oamAddrReload; break;
    case 0x03:
      oamAddrReload = static_cast<uint16_t>(((v & 1) << 9) | (oamAddrReload & 0x1FE));
      oamPriority = v & 0x80;
      oamAddr = oamAddrReload;
      break;
    case 0x04:
      if (oamAddr & 0x200) oam[512 + (oamAddr & 0x1F)] = v;
      else if (!(oamAddr & 1)) oamLatch = v;
      else { oam[oamAddr - 1] = oamLatch; oam[oamAddr] = v; }
      oamAddr = (oamAddr + 1) & 0x3FF;
      break;
    case 0x05: bgmode = v; break;
    case 0x06: mosaic = v; break;
    case 0x07: case 0x08: case 0x09: case 0x0A: bgsc[reg - 7] = v; break;
    case 0x0B: case 0x0C: bgnba[reg - 0x0B] = v; break;
    case 0x0D: case 0x0F: case 0x11: case 0x13: {
      const int bg = (reg - 0x0D) >> 1;
      if (reg == 0x0D) { m7hofs = static_cast<int16_t>(((v << 8) | m7prev) << 3); m7hofs >>= 3; m7prev = v; }
      hofs[bg] = static_cast<uint16_t>(((v << 8) | (ofsPrev & ~7) | (ofsPrev2 & 7)) & 0x3FF);
      ofsPrev = v;
      ofsPrev2 = v;
      break;
    }
    case 0x0E: case 0x10: case 0x12: case 0x14: {
      const int bg = (reg - 0x0E) >> 1;
      if (reg == 0x0E) { m7vofs = static_cast<int16_t>(((v << 8) | m7prev) << 3); m7vofs >>= 3; m7prev = v; }
      vofs[bg] = static_cast<uint16_t>(((v << 8) | ofsPrev) & 0x3FF);
      ofsPrev = v;
      break;
    }
    case 0x15: vmain = v; break;
    case 0x16: vaddr = static_cast<uint16_t>((vaddr & 0xFF00) | v); vramPrefetch(); break;
    case 0x17: vaddr = static_cast<uint16_t>((vaddr & 0x00FF) | (v << 8)); vramPrefetch(); break;
    case 0x18: case 0x19: {
      const uint16_t a = vramAddrRemap() & 0x7FFF;
      if (!rendering || forceBlank()) vram[a * 2 + (reg - 0x18)] = v;
      if (((vmain & 0x80) != 0) == (reg == 0x19)) vaddr = static_cast<uint16_t>(vaddr + STEP[vmain & 3]);
      break;
    }
    case 0x1A: m7sel = v; break;
    case 0x1B: m7a = static_cast<int16_t>((v << 8) | m7prev); m7prev = v; break;
    case 0x1C: m7b = static_cast<int16_t>((v << 8) | m7prev); m7prev = v; break;
    case 0x1D: m7c = static_cast<int16_t>((v << 8) | m7prev); m7prev = v; break;
    case 0x1E: m7d = static_cast<int16_t>((v << 8) | m7prev); m7prev = v; break;
    case 0x1F: m7x = static_cast<int16_t>(((v << 8) | m7prev) << 3); m7x >>= 3; m7prev = v; break;
    case 0x20: m7y = static_cast<int16_t>(((v << 8) | m7prev) << 3); m7y >>= 3; m7prev = v; break;
    case 0x21: cgAddr = static_cast<uint16_t>(v << 1); break;
    case 0x22:
      if (!(cgAddr & 1)) cgLatch = v;
      else cgram[cgAddr >> 1] = static_cast<uint16_t>(((v & 0x7F) << 8) | cgLatch);
      cgAddr = (cgAddr + 1) & 0x1FF;
      break;
    case 0x23: wsel[0] = v; break;
    case 0x24: wsel[1] = v; break;
    case 0x25: wsel[2] = v; break;
    case 0x26: case 0x27: case 0x28: case 0x29: wpos[reg - 0x26] = v; break;
    case 0x2A: wbglog = v; break;
    case 0x2B: wobjlog = v; break;
    case 0x2C: tm = v; break;
    case 0x2D: ts = v; break;
    case 0x2E: tmw = v; break;
    case 0x2F: tsw = v; break;
    case 0x30: cgwsel = v; break;
    case 0x31: cgadsub = v; break;
    case 0x32:
      if (v & 0x20) fixedColor = static_cast<uint16_t>((fixedColor & ~0x001F) | (v & 31));
      if (v & 0x40) fixedColor = static_cast<uint16_t>((fixedColor & ~0x03E0) | ((v & 31) << 5));
      if (v & 0x80) fixedColor = static_cast<uint16_t>((fixedColor & ~0x7C00) | ((v & 31) << 10));
      break;
    case 0x33: setini = v; break;
    default: break;
  }
}

uint8_t PPU::read(uint8_t reg, uint8_t mdr) {
  static const int STEP[4] = {1, 32, 128, 128};
  switch (reg) {
    case 0x34: case 0x35: case 0x36: {
      const int32_t r = m7a * static_cast<int8_t>(m7b >> 8);
      ppu1Mdr = static_cast<uint8_t>(r >> ((reg - 0x34) * 8));
      return ppu1Mdr;
    }
    case 0x37: return mdr;  // o barramento trava os contadores (SNES::read)
    case 0x38: {
      uint8_t v;
      if (oamAddr & 0x200) v = oam[512 + (oamAddr & 0x1F)];
      else v = oam[oamAddr];
      oamAddr = (oamAddr + 1) & 0x3FF;
      ppu1Mdr = v;
      return v;
    }
    case 0x39: {
      const uint8_t v = static_cast<uint8_t>(vlatch);
      if (!(vmain & 0x80)) { vramPrefetch(); vaddr = static_cast<uint16_t>(vaddr + STEP[vmain & 3]); }
      ppu1Mdr = v;
      return v;
    }
    case 0x3A: {
      const uint8_t v = static_cast<uint8_t>(vlatch >> 8);
      if (vmain & 0x80) { vramPrefetch(); vaddr = static_cast<uint16_t>(vaddr + STEP[vmain & 3]); }
      ppu1Mdr = v;
      return v;
    }
    case 0x3B: {
      uint8_t v;
      if (cgAddr & 1) v = static_cast<uint8_t>(((cgram[cgAddr >> 1] >> 8) & 0x7F) | (ppu2Mdr & 0x80));
      else v = static_cast<uint8_t>(cgram[cgAddr >> 1]);
      cgAddr = (cgAddr + 1) & 0x1FF;
      ppu2Mdr = v;
      return v;
    }
    case 0x3C: {
      const uint8_t v = hFlip ? static_cast<uint8_t>(((hLatch >> 8) & 1) | (ppu2Mdr & 0xFE)) : static_cast<uint8_t>(hLatch);
      hFlip = !hFlip;
      ppu2Mdr = v;
      return v;
    }
    case 0x3D: {
      const uint8_t v = vFlip ? static_cast<uint8_t>(((vLatch >> 8) & 1) | (ppu2Mdr & 0xFE)) : static_cast<uint8_t>(vLatch);
      vFlip = !vFlip;
      ppu2Mdr = v;
      return v;
    }
    case 0x3E: ppu1Mdr = static_cast<uint8_t>((timeOver ? 0x80 : 0) | (rangeOver ? 0x40 : 0) | (ppu1Mdr & 0x10) | 1); return ppu1Mdr;
    case 0x3F: {
      const uint8_t v = static_cast<uint8_t>((field ? 0x80 : 0) | (counterLatched ? 0x40 : 0) | (ppu2Mdr & 0x20) | 3);
      counterLatched = false;
      hFlip = vFlip = false;
      ppu2Mdr = v;
      return v;
    }
    case 0x04: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0A: case 0x14: case 0x15: case 0x16:
    case 0x18: case 0x19: case 0x1A: case 0x24: case 0x25: case 0x26: case 0x28: case 0x29: case 0x2A:
      return ppu1Mdr;  // registradores só de escrita: valor antigo do barramento da PPU1
    default: return mdr;
  }
}

// ---------------------------------------------------------------- janelas
void PPU::windowMask(int layer, bool mask[256]) const {
  const uint8_t sel = (wsel[layer >> 1] >> ((layer & 1) * 4)) & 0xF;
  const bool e1 = sel & 2, e2 = sel & 8, i1 = sel & 1, i2 = sel & 4;
  if (!e1 && !e2) { std::memset(mask, 0, 256); return; }
  int logic;
  if (layer < 4) logic = (wbglog >> (layer * 2)) & 3;
  else if (layer == 4) logic = wobjlog & 3;
  else logic = (wobjlog >> 2) & 3;
  for (int x = 0; x < 256; x++) {
    const bool w1 = ((x >= wpos[0] && x <= wpos[1]) != i1);
    const bool w2 = ((x >= wpos[2] && x <= wpos[3]) != i2);
    bool r;
    if (e1 && !e2) r = w1;
    else if (e2 && !e1) r = w2;
    else {
      switch (logic) {
        case 0: r = w1 || w2; break;
        case 1: r = w1 && w2; break;
        case 2: r = w1 != w2; break;
        default: r = w1 == w2; break;
      }
    }
    mask[x] = r;
  }
}

// ---------------------------------------------------------------- fundos
void PPU::renderBg(int bg, int y, int bpp, Layer& out) {
  const int mode = bgmode & 7;
  const bool hires = mode == 5 || mode == 6;
  const bool big = bgmode & (0x10 << bg);
  const int tileW = (big || hires) ? 16 : 8, tileH = big ? 16 : 8;
  const int sc = bgsc[bg];
  const int sizeX = (sc & 1) ? 64 : 32, sizeY = (sc & 2) ? 64 : 32;
  const uint16_t charBase = static_cast<uint16_t>(((bgnba[bg >> 1] >> ((bg & 1) * 4)) & 0xF) << 12);
  const int tileWords = bpp * 4;
  const int palOffset = mode == 0 ? bg * 32 : 0;
  const bool direct = bpp == 8 && (cgwsel & 1);
  const int mos = (mosaic & (1 << bg)) ? (mosaic >> 4) + 1 : 1;
  int line = y;
  if (mos > 1) line = y - ((y - mosaicStartY + 256 * mos) % mos);
  // entrelaçado nos modos 5 e 6: 448 linhas, cada quadro mostra as pares ou as ímpares
  if (hires && (setini & 1)) line = line * 2 + (field ? 1 : 0);
  const bool opt = (mode == 2 || mode == 4 || mode == 6) && bg < 2;
  auto entryAt = [&](int b, int tx, int ty, int sx, int sy) -> uint16_t {
    const int bsc = bgsc[b];
    const uint16_t base = static_cast<uint16_t>((bsc & 0xFC) << 8);
    tx &= sx - 1;
    ty &= sy - 1;
    uint16_t a = static_cast<uint16_t>(base + ((ty & 31) << 5) + (tx & 31));
    if (tx & 32) a = static_cast<uint16_t>(a + 0x400);
    if (ty & 32) a = static_cast<uint16_t>(a + (sx == 64 ? 0x800 : 0x400));
    return vramWord(a);
  };
  // cache da linha de 8 pixels da telha atual
  int lastKey = -1;
  uint8_t pix[8] = {};
  int lastPal = 0, lastPrio = 0;
  const int samples = hires ? 512 : 256;
  uint16_t col[512];
  uint8_t pri[512];
  bool opq[512];
  for (int sx = 0; sx < samples; sx++) {
    int hs = hofs[bg], vs = vofs[bg];
    if (opt) {
      const int screenX = hires ? sx >> 1 : sx;
      const int column = (screenX + (hofs[bg] & 7)) >> 3;
      if (column > 0) {
        const int sc3 = bgsc[2];
        const int s3x = (sc3 & 1) ? 64 : 32, s3y = (sc3 & 2) ? 64 : 32;
        const int ox = ((column - 1) * 8 + (hofs[2] & ~7)) >> 3;
        const int oy = vofs[2] >> 3;
        const uint16_t hv = entryAt(2, ox, oy, s3x, s3y);
        if (mode == 4) {
          if (hv & (0x2000 << bg)) {
            if (hv & 0x8000) vs = hv & 0x3FF;
            else hs = (hv & 0x3F8) | (hofs[bg] & 7);
          }
        } else {
          const uint16_t vv = entryAt(2, ox, oy + 1, s3x, s3y);
          if (hv & (0x2000 << bg)) hs = (hv & 0x3F8) | (hofs[bg] & 7);
          if (vv & (0x2000 << bg)) vs = vv & 0x3FF;
        }
      }
    }
    int xx = hires ? sx + (hs << 1) : sx + hs;
    if (mos > 1) {
      const int base = hires ? sx - (sx % (mos * 2)) : sx - (sx % mos);
      xx = (hires ? base + (hs << 1) : base + hs);
    }
    const int yy = line + vs;
    const int tx = xx / tileW, ty = yy / tileH;
    const uint16_t e = entryAt(bg, tx, ty, sizeX, sizeY);
    int fx = xx & (tileW - 1), fy = yy & (tileH - 1);
    if (e & 0x4000) fx = tileW - 1 - fx;
    if (e & 0x8000) fy = tileH - 1 - fy;
    const int chr = ((e & 0x3FF) + (fx >> 3) + ((fy >> 3) << 4)) & 0x3FF;
    const int key = (chr << 3) | (fy & 7) | (bpp << 16);
    if (key != lastKey) {
      lastKey = key;
      const uint16_t addr = static_cast<uint16_t>(charBase + chr * tileWords + (fy & 7));
      std::memset(pix, 0, sizeof pix);
      for (int pl = 0; pl < bpp / 2; pl++) {
        const uint16_t w = vramWord(static_cast<uint16_t>(addr + pl * 8));
        for (int c = 0; c < 8; c++) {
          pix[c] |= static_cast<uint8_t>((((w >> (7 - c)) & 1) << (pl * 2)) | (((w >> (15 - c)) & 1) << (pl * 2 + 1)));
        }
      }
    }
    lastPal = (e >> 10) & 7;
    lastPrio = (e >> 13) & 1;
    const int idx = pix[fx & 7];
    opq[sx] = idx != 0;
    pri[sx] = static_cast<uint8_t>(lastPrio);
    if (!idx) { col[sx] = 0; continue; }
    if (bpp == 2) col[sx] = cgram[(palOffset + lastPal * 4 + idx) & 0xFF];
    else if (bpp == 4) col[sx] = cgram[(lastPal * 16 + idx) & 0xFF];
    else col[sx] = direct ? directColor(idx, lastPal) : cgram[idx];
  }
  if (!hires) {
    for (int x = 0; x < 256; x++) { out.color[x] = col[x]; out.prio[x] = pri[x]; out.opaque[x] = opq[x]; out.mathOk[x] = true; }
  } else {
    // alta resolução: a tela principal mostra as colunas ímpares e a secundária as pares; guarda as duas
    for (int x = 0; x < 256; x++) {
      out.color[x] = col[2 * x + 1]; out.prio[x] = pri[2 * x + 1]; out.opaque[x] = opq[2 * x + 1];
      hiresColor[bg][x] = col[2 * x]; hiresPrio[bg][x] = pri[2 * x]; hiresOpaque[bg][x] = opq[2 * x];
      out.mathOk[x] = true;
    }
  }
}

void PPU::renderMode7(int y, Layer& bg1, Layer& bg2) {
  const bool ext = setini & 0x40;
  const int mos1 = (mosaic & 1) ? (mosaic >> 4) + 1 : 1;
  int line = y;
  if (mos1 > 1) line = y - ((y - mosaicStartY + 256 * mos1) % mos1);
  const int sy = (m7sel & 2) ? 255 - line : line;
  auto clip = [](int v) { return (v & 0x2000) ? (v | ~0x3FF) : (v & 0x3FF); };
  const int hs = clip(m7hofs - m7x), vs = clip(m7vofs - m7y);
  const int a = m7a, b = m7b, c = m7c, d = m7d, cx = m7x, cy = m7y;
  const int ox = ((a * hs) & ~63) + ((b * sy) & ~63) + ((b * vs) & ~63) + (cx << 8);
  const int oy = ((c * hs) & ~63) + ((d * sy) & ~63) + ((d * vs) & ~63) + (cy << 8);
  const int repeat = m7sel >> 6;
  const bool direct = cgwsel & 1;
  for (int x = 0; x < 256; x++) {
    const int sx = (m7sel & 1) ? 255 - x : x;
    const int px = ox + a * sx, py = oy + c * sx;
    int tx = px >> 8, ty = py >> 8;
    const bool outside = (tx | ty) & ~0x3FF;
    uint8_t pixel;
    if (outside && repeat == 2) {
      pixel = 0;
    } else {
      int tile;
      if (outside && repeat == 3) tile = 0;
      else tile = vram[(((ty >> 3) & 127) * 128 + ((tx >> 3) & 127)) * 2];
      pixel = vram[(tile * 64 + (ty & 7) * 8 + (tx & 7)) * 2 + 1];
    }
    bg1.opaque[x] = pixel != 0;
    bg1.prio[x] = 0;
    bg1.mathOk[x] = true;
    bg1.color[x] = pixel ? (direct ? directColor(pixel, 0) : cgram[pixel]) : 0;
    if (ext) {
      const int p2 = pixel & 0x7F;
      bg2.opaque[x] = p2 != 0;
      bg2.prio[x] = pixel >> 7;
      bg2.mathOk[x] = true;
      bg2.color[x] = p2 ? cgram[p2] : 0;
    }
  }
  if (mos1 > 1) {
    for (int x = 0; x < 256; x++) {
      const int src = x - (x % mos1);
      bg1.color[x] = bg1.color[src]; bg1.opaque[x] = bg1.opaque[src];
      if (ext) { bg2.color[x] = bg2.color[src]; bg2.opaque[x] = bg2.opaque[src]; bg2.prio[x] = bg2.prio[src]; }
    }
  }
}

// ---------------------------------------------------------------- sprites
void PPU::renderSprites(int y, Layer& out) {
  std::memset(out.opaque, 0, sizeof out.opaque);
  const int line = y - 1;
  const int sizeSel = obsel >> 5;
  const uint16_t nameBase = static_cast<uint16_t>((obsel & 7) << 13);
  const uint16_t gap = static_cast<uint16_t>((((obsel >> 3) & 3) + 1) << 12);
  const int first = oamPriority ? (oamAddrReload >> 2) & 0x7F : 0;
  int found[32], count = 0;
  for (int k = 0; k < 128; k++) {
    const int i = (first + k) & 0x7F;
    const uint8_t hi = static_cast<uint8_t>(oam[512 + (i >> 2)] >> ((i & 3) * 2));
    const int large = (hi >> 1) & 1;
    const int w = OBJ_W[sizeSel][large], h = OBJ_H[sizeSel][large];
    int sx = oam[i * 4] | ((hi & 1) << 8);
    if (sx >= 256) sx -= 512;
    if (sx <= -w) continue;
    const int row = (line - oam[i * 4 + 1]) & 0xFF;
    if (row >= h) continue;
    if (count == 32) { rangeOver = true; break; }
    found[count++] = i;
  }
  int tiles = 0;
  for (int n = 0; n < count && tiles < 34; n++) {
    const int i = found[n];
    const uint8_t hi = static_cast<uint8_t>(oam[512 + (i >> 2)] >> ((i & 3) * 2));
    const int large = (hi >> 1) & 1;
    const int w = OBJ_W[sizeSel][large], h = OBJ_H[sizeSel][large];
    int sx = oam[i * 4] | ((hi & 1) << 8);
    if (sx >= 256) sx -= 512;
    const uint8_t tile = oam[i * 4 + 2], attr = oam[i * 4 + 3];
    int row = (line - oam[i * 4 + 1]) & 0xFF;
    if (attr & 0x80) row = h - 1 - row;
    const int pal = (attr >> 1) & 7, prio = (attr >> 4) & 3;
    const int ty = row >> 3;
    for (int t = 0; t < w / 8; t++) {
      const int px0 = sx + t * 8;
      if (px0 <= -8 || px0 >= 256) continue;
      if (++tiles > 34) { timeOver = true; break; }
      const int tt = (attr & 0x40) ? (w / 8 - 1 - t) : t;
      const int chr = (((tile & 0x0F) + tt) & 0x0F) | (((tile & 0xF0) + (ty << 4)) & 0xF0);
      const uint16_t addr = static_cast<uint16_t>(nameBase + ((attr & 1) ? gap : 0) + chr * 16 + (row & 7));
      const uint16_t w0 = vramWord(addr), w1 = vramWord(static_cast<uint16_t>(addr + 8));
      for (int c = 0; c < 8; c++) {
        const int x = px0 + c;
        if (x < 0 || x >= 256 || out.opaque[x]) continue;
        const int bit = (attr & 0x40) ? c : 7 - c;
        const int idx = ((w0 >> bit) & 1) | (((w0 >> (bit + 8)) & 1) << 1) | (((w1 >> bit) & 1) << 2) | (((w1 >> (bit + 8)) & 1) << 3);
        if (!idx) continue;
        out.opaque[x] = true;
        out.color[x] = cgram[128 + pal * 16 + idx];
        out.prio[x] = static_cast<uint8_t>(prio);
        out.mathOk[x] = pal >= 4;
      }
    }
  }
}

// ---------------------------------------------------------------- linha
void PPU::renderLine(int y) {
  uint32_t* dst = fb + (y - 1) * 256;
  if (forceBlank()) { std::fill(dst, dst + 256, 0xFF000000u); return; }
  const int mode = bgmode & 7;
  static const int BPP[8][4] = {{2, 2, 2, 2}, {4, 4, 2, 0}, {4, 4, 0, 0}, {8, 4, 0, 0}, {8, 2, 0, 0}, {4, 2, 0, 0}, {4, 0, 0, 0}, {8, 0, 0, 0}};
  static Layer layer[5];
  bool has[5] = {};
  const uint8_t used = tm | ts;
  if (mode == 7) {
    if (used & 3) { renderMode7(y, layer[0], layer[1]); has[0] = true; has[1] = (setini & 0x40) != 0; }
  } else {
    for (int bg = 0; bg < 4; bg++) {
      if (!BPP[mode][bg] || !(used & (1 << bg))) continue;
      renderBg(bg, y, BPP[mode][bg], layer[bg]);
      has[bg] = true;
    }
  }
  if (used & 0x10) { renderSprites(y, layer[4]); has[4] = true; }

  // ordem de prioridade (da frente para trás) de cada modo: camada * 2 + bit de prioridade; sprites = 8 + prioridade
  int z[10];
  for (int& v : z) v = -1;
  auto order = [&](std::initializer_list<int> list) { int r = 20; for (int e : list) z[e] = r--; };
  const int S0 = 8, S1 = 9, S2 = 10, S3 = 11;
  int zObj[4] = {0, 0, 0, 0};
  {
    int zz[12];
    for (int& v : zz) v = -1;
    auto ord = [&](std::initializer_list<int> list) { int r = 20; for (int e : list) zz[e] = r--; };
    switch (mode) {
      case 0: ord({S3, 1, 3, S2, 0, 2, S1, 5, 7, S0, 4, 6}); break;
      case 1:
        if (bgmode & 8) ord({5, S3, 1, 3, S2, 0, 2, S1, S0, 4});
        else ord({S3, 1, 3, S2, 0, 2, S1, 5, S0, 4});
        break;
      case 6: ord({S3, 1, S2, S1, 0, S0}); break;
      case 7: ord({S3, S2, 3, S1, 0, S0, 2}); break;
      default: ord({S3, 1, S2, 3, S1, 0, S0, 2}); break;
    }
    for (int k = 0; k < 8; k++) z[k] = zz[k];
    for (int k = 0; k < 4; k++) zObj[k] = zz[8 + k];
  }
  (void)order;

  bool winMain[6][256], winSub[6][256], colorWin[256];
  for (int l = 0; l < 5; l++) {
    if (!has[l]) continue;
    bool m[256];
    windowMask(l, m);
    for (int x = 0; x < 256; x++) {
      winMain[l][x] = ((tmw >> l) & 1) && m[x];
      winSub[l][x] = ((tsw >> l) & 1) && m[x];
    }
  }
  windowMask(5, colorWin);

  const bool hires = mode == 5 || mode == 6 || (setini & 8);
  const int bright = inidisp & 15;
  auto pick = [&](uint8_t enable, bool win[6][256], int x, bool useHires, uint16_t& color, int& src, bool& mathOk) {
    int best = -1;
    src = 5;
    for (int l = 0; l < 4; l++) {
      if (!has[l] || !((enable >> l) & 1) || win[l][x]) continue;
      bool op; int pr; uint16_t c;
      if (useHires && (mode == 5 || mode == 6)) { op = hiresOpaque[l][x]; pr = hiresPrio[l][x]; c = hiresColor[l][x]; }
      else { op = layer[l].opaque[x]; pr = layer[l].prio[x]; c = layer[l].color[x]; }
      if (!op) continue;
      const int zz = z[l * 2 + pr];
      if (zz > best) { best = zz; color = c; src = l; mathOk = true; }
    }
    if (has[4] && (enable & 0x10) && !win[4][x] && layer[4].opaque[x]) {
      const int zz = zObj[layer[4].prio[x]];
      if (zz > best) { best = zz; color = layer[4].color[x]; src = 4; mathOk = layer[4].mathOk[x]; }
    }
    return best >= 0;
  };

  for (int x = 0; x < 256; x++) {
    uint16_t mainC = cgram[0], subC = fixedColor;
    int mainSrc = 5, subSrc = 5;
    bool mainMath = true, subMath = true;
    if (!pick(tm, winMain, x, false, mainC, mainSrc, mainMath)) { mainC = cgram[0]; mainSrc = 5; mainMath = true; }
    bool subIsBackdrop = !pick(ts, winSub, x, true, subC, subSrc, subMath);
    if (subIsBackdrop) subC = fixedColor;
    if (hires && subIsBackdrop) subC = cgram[0];
    // soma/subtração de cores
    const bool cw = colorWin[x];
    auto region = [&](int sel) { switch (sel & 3) { case 0: return false; case 1: return !cw; case 2: return cw; default: return true; } };
    if (region(cgwsel >> 6)) mainC = 0;
    bool math = !region(cgwsel >> 4);
    if (math) {
      if (mainSrc == 5) math = cgadsub & 0x20;
      else if (mainSrc == 4) math = (cgadsub & 0x10) && mainMath;
      else math = (cgadsub >> mainSrc) & 1;
    }
    uint16_t outC = mainC;
    if (math) {
      const bool useSub = cgwsel & 2;
      const uint16_t opnd = useSub ? (subIsBackdrop ? fixedColor : subC) : fixedColor;
      const bool half = (cgadsub & 0x40) && !(useSub && subIsBackdrop) && !region(cgwsel >> 6);
      int r = mainC & 31, g = (mainC >> 5) & 31, b = (mainC >> 10) & 31;
      const int r2 = opnd & 31, g2 = (opnd >> 5) & 31, b2 = (opnd >> 10) & 31;
      if (cgadsub & 0x80) {
        r -= r2; g -= g2; b -= b2;
        if (half) { r >>= 1; g >>= 1; b >>= 1; }
        r = std::max(r, 0); g = std::max(g, 0); b = std::max(b, 0);
      } else {
        r += r2; g += g2; b += b2;
        if (half) { r >>= 1; g >>= 1; b >>= 1; }
        r = std::min(r, 31); g = std::min(g, 31); b = std::min(b, 31);
      }
      outC = static_cast<uint16_t>(r | (g << 5) | (b << 10));
    }
    if (hires) {
      // média entre o pixel da tela secundária (coluna par) e o da principal (ímpar)
      const int r = ((outC & 31) + (subC & 31)) >> 1, g = (((outC >> 5) & 31) + ((subC >> 5) & 31)) >> 1,
                b = (((outC >> 10) & 31) + ((subC >> 10) & 31)) >> 1;
      outC = static_cast<uint16_t>(r | (g << 5) | (b << 10));
    }
    if (bright != 15) {
      int r = outC & 31, g = (outC >> 5) & 31, b = (outC >> 10) & 31;
      if (bright == 0) { r = g = b = 0; }
      else { r = r * (bright + 1) / 16; g = g * (bright + 1) / 16; b = b * (bright + 1) / 16; }
      outC = static_cast<uint16_t>(r | (g << 5) | (b << 10));
    }
    dst[x] = toRgba(outC);
  }
}

void PPU::sync(StateIO& io) {
  io.raw(vram, sizeof vram); io.raw(cgram, sizeof cgram); io.raw(oam, sizeof oam);
  io.sync(inidisp); io.sync(obsel); io.sync(bgmode); io.sync(mosaic); io.raw(bgsc, sizeof bgsc); io.raw(bgnba, sizeof bgnba);
  io.raw(hofs, sizeof hofs); io.raw(vofs, sizeof vofs); io.sync(ofsPrev); io.sync(ofsPrev2);
  io.sync(vmain); io.sync(vaddr); io.sync(vlatch);
  io.sync(m7sel); io.sync(m7prev); io.sync(m7a); io.sync(m7b); io.sync(m7c); io.sync(m7d); io.sync(m7x); io.sync(m7y); io.sync(m7hofs); io.sync(m7vofs);
  io.sync(oamAddr); io.sync(oamAddrReload); io.sync(oamLatch); io.sync(oamPriority);
  io.sync(cgAddr); io.sync(cgLatch);
  io.raw(wsel, sizeof wsel); io.raw(wpos, sizeof wpos); io.sync(wbglog); io.sync(wobjlog);
  io.sync(tm); io.sync(ts); io.sync(tmw); io.sync(tsw); io.sync(cgwsel); io.sync(cgadsub); io.sync(setini); io.sync(fixedColor);
  io.sync(hLatch); io.sync(vLatch); io.sync(counterLatched); io.sync(hFlip); io.sync(vFlip);
  io.sync(ppu1Mdr); io.sync(ppu2Mdr); io.sync(timeOver); io.sync(rangeOver); io.sync(rendering); io.sync(mosaicStartY); io.sync(field);
  if (io.reading()) {
    for (auto& h : hofs) h &= 0x3FF;
    for (auto& v : vofs) v &= 0x3FF;
    oamAddr &= 0x3FF; oamAddrReload &= 0x3FF; cgAddr &= 0x1FF;
    clampTo(mosaicStartY, 0, 261);
  }
}

}  // namespace indigo::snes
