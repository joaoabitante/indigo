#include "vdp.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::msx {

namespace {
enum { M_G1 = 0x00, M_T1 = 0x01, M_MC = 0x02, M_G2 = 0x04, M_G3 = 0x08, M_T2 = 0x09,
       M_G4 = 0x0C, M_G5 = 0x10, M_G6 = 0x14, M_G7 = 0x1C };

constexpr uint32_t rgb(int r, int g, int b) {
  return 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}
// cores fixas do TMS9918A (as mesmas do openMSX)
const uint32_t TMS_PAL[16] = {
  rgb(0, 0, 0),       rgb(0, 0, 0),       rgb(33, 200, 66),   rgb(94, 220, 120),
  rgb(84, 85, 237),   rgb(125, 118, 252), rgb(212, 82, 77),   rgb(66, 235, 245),
  rgb(252, 85, 84),   rgb(255, 121, 120), rgb(212, 193, 84),  rgb(230, 206, 128),
  rgb(33, 176, 59),   rgb(201, 91, 186),  rgb(204, 204, 204), rgb(255, 255, 255)};
// paleta inicial do V9938 (a mesma que a BIOS do MSX2 grava), em (r<<6)|(g<<3)|b
const uint16_t V9938_PAL[16] = {
  0000, 0000, 0161, 0373, 0117, 0237, 0511, 0267, 0711, 0733, 0661, 0664, 0141, 0625, 0555, 0777};
// cores dos sprites no SCREEN 8 (fixas), em (g<<6)|(r<<3)|b
const uint16_t G7_SPRITE[16] = {
  0000, 0002, 0030, 0032, 0300, 0302, 0330, 0332, 0472, 0007, 0070, 0077, 0700, 0707, 0770, 0777};
const uint8_t C3[8] = {0, 36, 73, 109, 146, 182, 219, 255};

struct G7Table {
  uint32_t c[256 + 16];
  G7Table() {
    for (int v = 0; v < 256; v++) {
      const int g = (v >> 5) & 7, r = (v >> 2) & 7, b2 = v & 3;
      c[v] = rgb(C3[r], C3[g], C3[(b2 << 1) | (b2 >> 1)]);
    }
    for (int i = 0; i < 16; i++) {
      const int g = (G7_SPRITE[i] >> 6) & 7, r = (G7_SPRITE[i] >> 3) & 7, b = G7_SPRITE[i] & 7;
      c[256 + i] = rgb(C3[r], C3[g], C3[b]);
    }
  }
};
const G7Table G7;

// máscaras dos registradores
const uint8_t V9938_MASK[64] = {
  0x7E, 0x7F, 0x7F, 0xFF, 0x3F, 0xFF, 0x3F, 0xFF, 0xFB, 0xBF, 0x07, 0x03, 0xFF, 0xFF, 0x07, 0x0F,
  0x0F, 0xBF, 0xFF, 0xFF, 0xFF, 0x3F, 0x3F, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0,
  0xFF, 0x01, 0xFF, 0x03, 0xFF, 0x01, 0xFF, 0x03, 0xFF, 0x01, 0xFF, 0x03, 0xFF, 0x7F, 0xFF, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const uint8_t TMS_MASK[8] = {0x03, 0xFB, 0x0F, 0xFF, 0x07, 0x7F, 0x07, 0xFF};
}  // namespace

VDP::VDP(bool isV9938)
    : v9938(isV9938), vram(isV9938 ? 0x20000 : 0x4000, 0), vramMask(isV9938 ? 0x1FFFF : 0x3FFF), lines(212) {
  reset();
}

void VDP::reset() {
  std::memset(reg, 0, sizeof reg);
  std::memset(stat, 0, sizeof stat);
  if (v9938) stat[9] = 0xFE;
  for (int i = 0; i < 16; i++) { pal[i] = V9938_PAL[i]; updatePalette(i); }
  ptr = 0; latch = 0; latchFull = false; readAhead = 0;
  palLatch = 0; palLatchFull = false;
  hrIrq = false;
  blinkCount = 0; blinkOn = false;
  frameLines = 192; scanLine = 0;
  cmd = 0; cmdLop = 0; ce = false; tr = false; bd = false;
  cx = cy = cnx = cny = cxStart = cleft = 0; cdix = cdiy = 1;
  for (auto& l : lines) { std::fill(l.px, l.px + 512, 0xFF000000u); l.hi = false; }
  outW = 256; outH = 192;
  std::fill(fb, fb + 512 * 424, 0xFF000000u);
}

void VDP::updatePalette(int i) {
  if (!v9938) { palRGB[i] = TMS_PAL[i]; return; }
  const int r = (pal[i] >> 6) & 7, g = (pal[i] >> 3) & 7, b = pal[i] & 7;
  palRGB[i] = rgb(C3[r], C3[g], C3[b]);
}

int VDP::mode() const {
  int m = ((reg[1] >> 4) & 1) | ((reg[1] >> 2) & 2) | ((reg[0] << 1) & 4);
  if (v9938) m |= (reg[0] << 1) & 0x18;
  return m;
}

bool VDP::bitmap() const {
  const int m = mode();
  return m == M_G4 || m == M_G5 || m == M_G6 || m == M_G7;
}

int VDP::cpuAddr() const {
  if (!v9938) return ptr & 0x3FFF;
  const int a = ((reg[14] & 7) << 14) | ptr;
  const int m = mode();
  return (m == M_G6 || m == M_G7) ? planarAddr(a) : a;
}

void VDP::incAddr() {
  ptr = (ptr + 1) & 0x3FFF;
  if (!ptr && v9938 && (mode() & 0x18)) reg[14] = (reg[14] + 1) & 7;
}

// ------------------------------------------------------------ portas
int VDP::readData() {
  latchFull = false;
  const int v = readAhead;
  readAhead = vram[cpuAddr()];
  incAddr();
  return v;
}

void VDP::writeData(int v) {
  latchFull = false;
  vram[cpuAddr()] = static_cast<uint8_t>(v);
  readAhead = v & 0xFF;
  incAddr();
}

int VDP::readStatus(int cycle) {
  latchFull = false;
  const int s = v9938 ? (reg[15] & 15) : 0;
  switch (s) {
    case 0: {
      const int v = stat[0];
      stat[0] &= 0x1F;
      return v;
    }
    case 1: {
      const int v = hrIrq ? 1 : 0;  // ID do V9938 = 0
      hrIrq = false;
      return v;
    }
    case 2: {
      const bool vr = scanLine >= frameLines;
      const bool hr = cycle >= HBLANK_CYCLE || cycle < 4;
      return (tr ? 0x80 : 0) | (vr ? 0x40 : 0) | (hr ? 0x20 : 0) | (bd ? 0x10 : 0) | 0x0C | (ce ? 1 : 0);
    }
    case 3: case 4: case 6: case 8: return stat[s];
    case 5: {  // ler S5 zera as coordenadas de colisão
      const int v = stat[5];
      stat[3] = stat[4] = stat[5] = stat[6] = 0;
      return v;
    }
    case 7: return cmdReadS7();
    case 9: return stat[9] | 0xFE;
    default: return 0xFF;
  }
}

void VDP::writeControl(int v) {
  v &= 0xFF;
  if (!latchFull) {
    latch = v;
    latchFull = true;
    return;
  }
  latchFull = false;
  if (v & 0x80) {
    if (v9938) { const int r = v & 0x3F; if (r < 47) setReg(r, latch); }
    else setReg(v & 7, latch);
  } else {
    ptr = ((v & 0x3F) << 8) | latch;
    if (!(v & 0x40)) {  // leitura: já busca o primeiro byte
      readAhead = vram[cpuAddr()];
      incAddr();
    }
  }
}

void VDP::setReg(int r, int v) {
  v &= v9938 ? V9938_MASK[r & 63] : TMS_MASK[r & 7];
  reg[r] = static_cast<uint8_t>(v);
  if (!v9938) return;
  switch (r) {
    case 0: if (!(v & 0x10)) hrIrq = false; break;
    case 16: palLatchFull = false; break;
    case 44: if (ce && (cmd == 0xF || cmd == 0xB)) cmdTransfer(v); break;
    case 46: startCommand(); break;
    default: break;
  }
}

void VDP::writePalette(int v) {
  if (!v9938) return;
  if (!palLatchFull) { palLatch = v & 0xFF; palLatchFull = true; return; }
  palLatchFull = false;
  const int i = reg[16] & 15;
  pal[i] = static_cast<uint16_t>((((palLatch >> 4) & 7) << 6) | ((v & 7) << 3) | (palLatch & 7));
  updatePalette(i);
  reg[16] = (i + 1) & 15;
}

void VDP::writeIndirect(int v) {
  if (!v9938) return;
  const int r = reg[17] & 0x3F;
  if (r != 17 && r < 47) setReg(r, v);
  if (!(reg[17] & 0x80)) reg[17] = static_cast<uint8_t>((r + 1) & 0x3F);
}

bool VDP::irq() const {
  if ((stat[0] & 0x80) && (reg[1] & 0x20)) return true;
  return v9938 && hrIrq && (reg[0] & 0x10);
}

// ------------------------------------------------------------ tempo
void VDP::startLine(int line) {
  scanLine = line;
  if (line == 0) {
    frameLines = activeLines();
    // piscar do texto de 80 colunas: R13 = tempo aceso/apagado em unidades de 10 quadros
    const int on = reg[13] >> 4, off = reg[13] & 15;
    if (!on) blinkOn = false;
    else if (!off) blinkOn = true;
    else if (--blinkCount <= 0) {
      blinkOn = !blinkOn;
      blinkCount = (blinkOn ? on : off) * 10;
    }
  }
  if (line < frameLines) renderLine(line);
  if (line == frameLines) stat[0] |= 0x80;
}

void VDP::hblank(int line) {
  if (!v9938) return;
  // como no openMSX: FH só sobe com IE1 ligado (e desligar o IE1 apaga o pedido)
  if ((reg[0] & 0x10) && line < 256 && line == ((reg[19] - reg[23]) & 0xFF)) hrIrq = true;
}

void VDP::endFrame() {
  bool hi = false;
  for (int l = 0; l < frameLines; l++) hi |= lines[l].hi;
  outW = hi ? 512 : 256;
  outH = hi ? frameLines * 2 : frameLines;
  for (int l = 0; l < frameLines; l++) {
    const Line& L = lines[l];
    if (!hi) { std::memcpy(fb + l * 256, L.px, 256 * sizeof(uint32_t)); continue; }
    uint32_t* d = fb + (l * 2) * 512;
    if (L.hi) std::memcpy(d, L.px, 512 * sizeof(uint32_t));
    else for (int x = 0; x < 256; x++) d[x * 2] = d[x * 2 + 1] = L.px[x];
    std::memcpy(d + 512, d, 512 * sizeof(uint32_t));
  }
}

// ------------------------------------------------------------ desenho
void VDP::renderLine(int line) {
  Line& L = lines[line];
  L.hi = false;
  const int m = mode();
  const int y = v9938 ? (line + reg[23]) & 0xFF : line;
  const bool transparent = !(v9938 && (reg[8] & 0x20));
  const int back = reg[7] & 15;
  const bool spritesOn = !(v9938 && (reg[8] & 0x02));

  if (!(reg[1] & 0x40)) {  // tela desligada: só a cor da borda
    uint32_t c = palRGB[back];
    if (m == M_G7) c = G7.c[reg[7]];
    else if (m == M_G5) c = palRGB[(reg[7] >> 2) & 3];
    std::fill(L.px, L.px + 256, c);
    return;
  }

  int width = 256;
  switch (m) {
    case M_T1: {
      uint8_t tmp[256];
      renderText(y, tmp, false);
      for (int x = 0; x < 256; x++) idx[x] = tmp[x];
      break;
    }
    case M_T2: {
      uint8_t tmp[512];
      renderText(y, tmp, true);
      for (int x = 0; x < 512; x++) idx[x] = tmp[x];
      width = 512;
      break;
    }
    case M_G1: case M_G2: case M_G3: case M_MC: {
      uint8_t tmp[256];
      if (m == M_G1) renderG1(y, tmp);
      else if (m == M_MC) renderMC(y, tmp);
      else renderG2(y, tmp);
      if (spritesOn) {
        if (m == M_G3) {
          for (int x = 0; x < 256; x++) idx[x] = tmp[x];
          spritesMode2(y, line);
          for (int x = 0; x < 256; x++)
            if (sprGroup[x] && (sprCol[x] || !transparent)) idx[x] = sprCol[x];
          break;
        }
        spritesMode1(y, tmp, line);
      }
      for (int x = 0; x < 256; x++) idx[x] = tmp[x];
      break;
    }
    case M_G4: case M_G5: {
      const int msk = ((reg[2] & 0x7F) << 10) | 0x3FF;
      const int la = (0x18000 | (y << 7)) & msk;
      if (m == M_G4) {
        for (int x = 0; x < 128; x++) {
          const int b = vram[(la | x) & vramMask];
          idx[x * 2] = static_cast<uint16_t>(b >> 4);
          idx[x * 2 + 1] = static_cast<uint16_t>(b & 15);
        }
      } else {
        width = 512;
        for (int x = 0; x < 128; x++) {
          const int b = vram[(la | x) & vramMask];
          idx[x * 4] = static_cast<uint16_t>(b >> 6);
          idx[x * 4 + 1] = static_cast<uint16_t>((b >> 4) & 3);
          idx[x * 4 + 2] = static_cast<uint16_t>((b >> 2) & 3);
          idx[x * 4 + 3] = static_cast<uint16_t>(b & 3);
        }
      }
      break;
    }
    case M_G6: case M_G7: {
      const int msk = ((reg[2] & 0x3F) << 11) | 0x7FF;
      const int la = (0x10000 | (y << 8)) & msk;
      if (m == M_G6) {
        width = 512;
        for (int x = 0; x < 256; x++) {
          const int b = vram[planarAddr(la | x)];
          idx[x * 2] = static_cast<uint16_t>(b >> 4);
          idx[x * 2 + 1] = static_cast<uint16_t>(b & 15);
        }
      } else {
        for (int x = 0; x < 256; x++) idx[x] = vram[planarAddr(la | x)];
      }
      break;
    }
    default:  // combinação de modo sem uso: só a borda
      for (int x = 0; x < 256; x++) idx[x] = 0;
      break;
  }

  // sprites modo 2 nos bitmaps
  if (spritesOn && (m == M_G4 || m == M_G5 || m == M_G6 || m == M_G7)) {
    spritesMode2(y, line);
    for (int x = 0; x < 256; x++) {
      if (!sprGroup[x]) continue;
      const int c = sprCol[x];
      if (!c && transparent) continue;
      if (m == M_G4) idx[x] = static_cast<uint16_t>(c);
      else if (m == M_G7) idx[x] = static_cast<uint16_t>(256 + c);
      else if (m == M_G6) idx[x * 2] = idx[x * 2 + 1] = static_cast<uint16_t>(c);
      else { idx[x * 2] = static_cast<uint16_t>((c >> 2) & 3); idx[x * 2 + 1] = static_cast<uint16_t>(c & 3); }
    }
  }

  // cores
  L.hi = width == 512;
  if (m == M_G7) {
    for (int x = 0; x < 256; x++) L.px[x] = G7.c[idx[x]];
  } else if (m == M_G5) {
    const int b0 = (reg[7] >> 2) & 3, b1 = reg[7] & 3;
    for (int x = 0; x < 512; x++) {
      int c = idx[x];
      if (!c && transparent) c = (x & 1) ? b1 : b0;
      L.px[x] = palRGB[c];
    }
  } else {
    for (int x = 0; x < width; x++) {
      int c = idx[x] & 15;
      if (!c && transparent) c = back;
      L.px[x] = palRGB[c];
    }
  }
}

void VDP::renderText(int y, uint8_t* pix, bool text2) {
  const int fg = reg[7] >> 4, bg = reg[7] & 15;
  const int pg = (reg[4] & (v9938 ? 0x3F : 0x07)) << 11;
  const int row = y >> 3, sub = y & 7;
  if (!text2) {
    const int nb = v9938 ? (reg[2] & 0x7F) << 10 : (reg[2] & 0x0F) << 10;
    std::fill(pix, pix + 8, 0);
    std::fill(pix + 248, pix + 256, 0);
    for (int col = 0; col < 40; col++) {
      const int name = vram[(nb + row * 40 + col) & vramMask];
      const int p = vram[(pg + name * 8 + sub) & vramMask];
      for (int k = 0; k < 6; k++) pix[8 + col * 6 + k] = static_cast<uint8_t>((p & (0x80 >> k)) ? fg : bg);
    }
    return;
  }
  const int nb = (reg[2] & 0x7C) << 10;
  const int bt = ((reg[10] & 7) << 14) | ((reg[3] & 0xF8) << 6);
  const int fg2 = reg[12] >> 4, bg2 = reg[12] & 15;
  std::fill(pix, pix + 16, 0);
  std::fill(pix + 496, pix + 512, 0);
  for (int col = 0; col < 80; col++) {
    const int n = row * 80 + col;
    const int name = vram[(nb + n) & vramMask];
    const int p = vram[(pg + name * 8 + sub) & vramMask];
    int f = fg, b = bg;
    if (blinkOn && (vram[(bt + (n >> 3)) & vramMask] & (0x80 >> (col & 7)))) { f = fg2; b = bg2; }
    for (int k = 0; k < 6; k++) pix[16 + col * 6 + k] = static_cast<uint8_t>((p & (0x80 >> k)) ? f : b);
  }
}

void VDP::renderG1(int y, uint8_t* pix) {
  const int nb = v9938 ? (reg[2] & 0x7F) << 10 : (reg[2] & 0x0F) << 10;
  const int ct = v9938 ? ((reg[10] & 7) << 14) | (reg[3] << 6) : (reg[3] << 6) & 0x3FFF;
  const int pg = (reg[4] & (v9938 ? 0x3F : 0x07)) << 11;
  const int rowBase = nb + (y >> 3) * 32;
  for (int col = 0; col < 32; col++) {
    const int name = vram[(rowBase + col) & vramMask];
    const int p = vram[(pg + name * 8 + (y & 7)) & vramMask];
    const int c = vram[(ct + (name >> 3)) & vramMask];
    const uint8_t f = static_cast<uint8_t>(c >> 4), b = static_cast<uint8_t>(c & 15);
    for (int k = 0; k < 8; k++) pix[col * 8 + k] = (p & (0x80 >> k)) ? f : b;
  }
}

void VDP::renderG2(int y, uint8_t* pix) {
  const int nb = v9938 ? (reg[2] & 0x7F) << 10 : (reg[2] & 0x0F) << 10;
  const int pBase = v9938 ? (reg[4] & 0x3C) << 11 : (reg[4] & 0x04) << 11;
  const int pMask = ((reg[4] & 3) << 11) | 0x7FF;
  const int cBase = (v9938 ? (reg[10] & 7) << 14 : 0) | ((reg[3] & 0x80) << 6);
  const int cMask = ((reg[3] & 0x7F) << 6) | 0x3F;
  const int rowBase = nb + (y >> 3) * 32;
  const int third = (y >> 6) << 8;
  for (int col = 0; col < 32; col++) {
    const int n = vram[(rowBase + col) & vramMask] | third;
    const int i = (n << 3) | (y & 7);
    const int p = vram[(pBase | (i & pMask)) & vramMask];
    const int c = vram[(cBase | (i & cMask)) & vramMask];
    const uint8_t f = static_cast<uint8_t>(c >> 4), b = static_cast<uint8_t>(c & 15);
    for (int k = 0; k < 8; k++) pix[col * 8 + k] = (p & (0x80 >> k)) ? f : b;
  }
}

void VDP::renderMC(int y, uint8_t* pix) {
  const int nb = v9938 ? (reg[2] & 0x7F) << 10 : (reg[2] & 0x0F) << 10;
  const int pg = (reg[4] & (v9938 ? 0x3F : 0x07)) << 11;
  const int row = y >> 3;
  for (int col = 0; col < 32; col++) {
    const int name = vram[(nb + row * 32 + col) & vramMask];
    const int c = vram[(pg + name * 8 + ((row & 3) << 1) + ((y >> 2) & 1)) & vramMask];
    std::fill(pix + col * 8, pix + col * 8 + 4, static_cast<uint8_t>(c >> 4));
    std::fill(pix + col * 8 + 4, pix + col * 8 + 8, static_cast<uint8_t>(c & 15));
  }
}

// sprites modo 1 (TMS9918A): 4 por linha; o de número menor fica na frente; colisão = dois pontos de
// padrão no mesmo lugar (mesmo com a cor 0)
void VDP::spritesMode1(int y, uint8_t* pix, int line) {
  (void)line;
  const int big = (reg[1] & 2) ? 16 : 8, mag = reg[1] & 1, h = big << mag;
  const int sat = v9938 ? (((reg[11] & 3) << 15) | (reg[5] << 7)) : ((reg[5] & 0x7F) << 7);
  const int spg = (reg[6] & (v9938 ? 0x3F : 0x07)) << 11;
  uint8_t cover[256 + 64];
  uint8_t drawn[256];
  std::memset(cover, 0, sizeof cover);
  std::memset(drawn, 0, sizeof drawn);
  int count = 0, last = 31;
  for (int i = 0; i < 32; i++) {
    const int base = (sat + i * 4) & vramMask;
    const int sy = vram[base];
    if (sy == 208) { last = i; break; }
    const int diff = (y - sy - 1) & 0xFF;
    if (diff >= h) continue;
    if (++count > 4) {
      if (!(stat[0] & 0x40)) stat[0] = static_cast<uint8_t>((stat[0] & 0xA0) | 0x40 | i);
      last = -1;
      break;
    }
    int sx = vram[(base + 1) & vramMask];
    int pat = vram[(base + 2) & vramMask];
    const int attr = vram[(base + 3) & vramMask];
    if (attr & 0x80) sx -= 32;
    if (big == 16) pat &= 0xFC;
    const int r = diff >> mag;
    const int pa = spg + pat * 8 + r;
    int bits = vram[pa & vramMask] << 8;
    if (big == 16) bits |= vram[(pa + 16) & vramMask];
    const int c = attr & 15;
    for (int px = 0; px < h; px++) {
      if (!(bits & (0x8000 >> (px >> mag)))) continue;
      const int x = sx + px;
      if (x < 0 || x > 255) continue;
      if (cover[x]) stat[0] |= 0x20;
      cover[x] = 1;
      if (!drawn[x] && c) { drawn[x] = 1; pix[x] = static_cast<uint8_t>(c); }
    }
  }
  if (last >= 0 && !(stat[0] & 0x40)) stat[0] = static_cast<uint8_t>((stat[0] & 0xE0) | last);
}

// sprites modo 2 (V9938): 8 por linha, cor por linha na tabela 512 bytes antes dos atributos.
// CC=1: o sprite junta (OU) as cores com o grupo do sprite anterior de CC=0; IC=1: sem colisão.
void VDP::spritesMode2(int y, int line) {
  std::memset(sprGroup, 0, sizeof sprGroup);
  const int big = (reg[1] & 2) ? 16 : 8, mag = reg[1] & 1, h = big << mag;
  const int ct = ((reg[11] & 3) << 15) | ((reg[5] & 0xF8) << 7);
  const int sat = ct + 0x200;
  const int spg = (reg[6] & 0x3F) << 11;
  uint8_t cover[256];
  std::memset(cover, 0, sizeof cover);
  int count = 0, group = 0, last = 31;
  bool primary = false;
  for (int i = 0; i < 32; i++) {
    const int base = (sat + i * 4) & vramMask;
    const int sy = vram[base];
    if (sy == 216) { last = i; break; }
    const int diff = (y - sy - 1) & 0xFF;
    if (diff >= h) continue;
    if (++count > 8) {
      if (!(stat[0] & 0x40)) stat[0] = static_cast<uint8_t>((stat[0] & 0xA0) | 0x40 | i);
      last = -1;
      break;
    }
    const int r = diff >> mag;
    const int attr = vram[(ct + i * 16 + r) & vramMask];
    const bool cc = attr & 0x40, ic = attr & 0x20;
    if (!cc) { group++; primary = true; }
    else if (!primary) continue;
    int sx = vram[(base + 1) & vramMask];
    int pat = vram[(base + 2) & vramMask];
    if (attr & 0x80) sx -= 32;
    if (big == 16) pat &= 0xFC;
    const int pa = spg + pat * 8 + r;
    int bits = vram[pa & vramMask] << 8;
    if (big == 16) bits |= vram[(pa + 16) & vramMask];
    const int c = attr & 15;
    for (int px = 0; px < h; px++) {
      if (!(bits & (0x8000 >> (px >> mag)))) continue;
      const int x = sx + px;
      if (x < 0 || x > 255) continue;
      if (!sprGroup[x]) { sprGroup[x] = static_cast<uint8_t>(group); sprCol[x] = static_cast<uint8_t>(c); }
      else if (cc && sprGroup[x] == group) sprCol[x] |= static_cast<uint8_t>(c);
      if (!cc && !ic) {
        if (cover[x] && !(stat[0] & 0x20)) {
          stat[0] |= 0x20;
          const int cx12 = x + 12, cy8 = line + 8;
          stat[3] = static_cast<uint8_t>(cx12); stat[4] = static_cast<uint8_t>((cx12 >> 8) | 0xFE);
          stat[5] = static_cast<uint8_t>(cy8); stat[6] = static_cast<uint8_t>((cy8 >> 8) | 0xFC);
        }
        cover[x] = 1;
      }
    }
  }
  if (last >= 0 && !(stat[0] & 0x40)) stat[0] = static_cast<uint8_t>((stat[0] & 0xE0) | last);
}

// ------------------------------------------------------------ save state
void VDP::sync(StateIO& io) {
  bool kind = v9938;
  io.sync(kind);
  if (io.reading() && kind != v9938) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  io.bytes(vram);
  io.raw(reg, sizeof reg); io.raw(stat, sizeof stat); io.raw(pal, sizeof pal);
  io.sync(ptr); io.sync(latch); io.sync(latchFull); io.sync(readAhead); io.sync(palLatch); io.sync(palLatchFull);
  io.sync(hrIrq); io.sync(blinkCount); io.sync(blinkOn); io.sync(frameLines); io.sync(scanLine);
  io.sync(cmd); io.sync(cmdLop); io.sync(ce); io.sync(tr); io.sync(bd);
  io.sync(cx); io.sync(cy); io.sync(cnx); io.sync(cny); io.sync(cxStart); io.sync(cdix); io.sync(cdiy); io.sync(cleft);
  if (io.reading()) {
    for (int r = 0; r < 64; r++) reg[r] &= v9938 ? V9938_MASK[r] : (r < 8 ? TMS_MASK[r] : 0);
    for (int i = 0; i < 16; i++) { pal[i] &= 0x1FF; updatePalette(i); }
    ptr &= 0x3FFF; latch &= 0xFF; readAhead &= 0xFF; palLatch &= 0xFF;
    frameLines = (frameLines == 212 && v9938) ? 212 : 192;
    clampTo(scanLine, 0, 261); clampTo(blinkCount, 0, 150);
    cmd &= 15; cmdLop &= 15;
    clampTo(cx, -1024, 1024); clampTo(cy, -2048, 2048); clampTo(cnx, 0, 1024); clampTo(cny, 0, 1024);
    clampTo(cxStart, -1024, 1024); clampTo(cleft, 0, 1024);
    cdix = cdix < 0 ? -1 : 1; cdiy = cdiy < 0 ? -1 : 1;
    if (!v9938) { ce = false; }
  }
}

}  // namespace indigo::msx
