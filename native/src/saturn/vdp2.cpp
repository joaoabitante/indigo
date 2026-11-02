// VDP2 do Saturn: planos NBG0-NBG3 (células de 8x8/16x16 ou bitmap) com rolagem por linha, RBG0 (rotação com a
// tabela de parâmetros A/B e a tabela de coeficientes), camada de sprites (o quadro do VDP1), tela de fundo,
// janelas retangulares W0/W1, prioridades, cálculo de cor (proporção ou soma com a camada de baixo) e
// deslocamento de cor.
//
// Fora daqui (ainda): RBG1, janela de linha e de sprite, rolagem por célula vertical, mosaico, tela de cor de
// linha, cálculo de cor especial e o entrelaçado do VDP1 (no modo de densidade dupla o quadro de sprites vale
// para as duas linhas de cada par).
// A imagem é montada de uma vez no começo do V-blank, com os registradores desse momento.
#include <algorithm>
#include <cstring>

#include "saturn.h"

namespace indigo::saturn {

namespace {
// camadas na ordem de desempate (prioridade igual: sprite ganha de RBG0, que ganha de NBG0...)
enum Layer { L_SPRITE = 0, L_RBG0, L_NBG0, L_NBG1, L_NBG2, L_NBG3, L_BACK };
inline uint32_t rgb555(uint16_t c) {
  const uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
  return (r << 3 | r >> 2) | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2) << 16;
}
inline int32_t sx(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }
}  // namespace

struct Vdp2::Tile {
  int bpp = 4, colors = 0;
  bool charSize = false, bitmap = false, pnd1 = false, aux = false, tpOff = false, rot = false;
  uint32_t bmW = 512, bmH = 256, supp = 0, craOffs = 0, mapOffs = 0, bmPal = 0;
  int plsz = 0, over = 0;
  uint32_t mapAddr[16] = {};
};

void Vdp2::reset() {
  std::fill(vram.begin(), vram.end(), 0);
  std::memset(cram, 0, sizeof cram);
  std::memset(regs, 0, sizeof regs);
  std::memset(out, 0, sizeof out);
  outW = 320;
  outH = 224;
  vb_ = hb_ = odd_ = false;
  line_ = 0;
}

int Vdp2::visibleLines() const {
  const int v = (regs[0] >> 4) & 3;
  return v == 0 ? 224 : v == 1 ? 240 : 256;
}

int Vdp2::hres() const {
  static const int W[8] = {320, 352, 640, 704, 320, 352, 640, 704};
  return W[regs[0] & 7];
}

uint16_t Vdp2::readReg(uint32_t off) {
  off &= 0x1FE;
  switch (off) {
    case 0x04: return uint16_t((vb_ ? 8 : 0) | (hb_ ? 4 : 0) | (odd_ ? 2 : 0) | (pal ? 1 : 0));
    case 0x08: return uint16_t(hb_ ? 0x1A0 : 0x40);
    case 0x0A: return uint16_t(line_ & 0x3FF);
    default: return reg(off);
  }
}

void Vdp2::writeReg(uint32_t off, uint16_t v) {
  off &= 0x1FE;
  if ((off >> 1) < 0x90 && off != 0x04 && off != 0x08 && off != 0x0A) regs[off >> 1] = v;
}

uint32_t Vdp2::color(uint32_t idx) const {
  const int mode = (regs[7] >> 12) & 3;  // RAMCTL.CRMD
  if (mode == 2 || mode == 3) {         // RGB888, 1024 cores de 32 bits
    const uint32_t a = (idx & 0x3FF) * 4;
    return uint32_t(cram[a + 3]) | uint32_t(cram[a + 2]) << 8 | uint32_t(cram[a + 1]) << 16;
  }
  const uint32_t a = (idx & (mode == 0 ? 0x3FF : 0x7FF)) * 2;
  return rgb555(uint16_t(cram[a] << 8 | cram[a + 1]));
}

uint32_t Vdp2::backColor(int y) const {
  const uint16_t u = reg(0xAC), l = reg(0xAE);
  uint32_t a = ((uint32_t(u & 7) << 16) | l) * 2;
  if (u & 0x8000) a += uint32_t(y) * 2;  // uma cor por linha
  return rgb555(vr16(a));
}

bool Vdp2::windowed(uint8_t ctrl, int x, int y) const {
  if (!(ctrl & 0x2A)) return false;  // nenhuma janela ligada para esta camada
  const bool andLogic = ctrl & 0x80;
  const bool hires = outW >= 640;
  bool v[2];
  for (int w = 0; w < 2; w++) {
    const bool en = ctrl & (w ? 0x08 : 0x02), area = ctrl & (w ? 0x04 : 0x01);
    if (!en) { v[w] = andLogic; continue; }
    const uint32_t b = w ? 0xC8 : 0xC0;
    int xs = reg(b) & 0x3FF, xe = reg(b + 4) & 0x3FF;
    if (!hires) { xs >>= 1; xe >>= 1; }
    const int ys = reg(b + 2) & 0x1FF, ye = reg(b + 6) & 0x1FF;
    const bool in = x >= xs && x <= xe && y >= ys && y <= ye;
    v[w] = in != area;
  }
  const bool sw = (ctrl & 0x20) ? (bool(swin_[x]) != bool(ctrl & 0x10)) : andLogic;
  return andLogic ? (v[0] && v[1] && sw) : (v[0] || v[1] || sw);
}

Vdp2::Tile Vdp2::tileNbg(int n) const {
  Tile t;
  const uint16_t chctla = reg(0x28), chctlb = reg(0x2A);
  int bmSize = 0;
  switch (n) {
    case 0: t.colors = (chctla >> 4) & 7; t.charSize = chctla & 1; t.bitmap = chctla & 2; bmSize = (chctla >> 2) & 3; break;
    case 1: t.colors = (chctla >> 12) & 3; t.charSize = chctla & 0x100; t.bitmap = chctla & 0x200; bmSize = (chctla >> 10) & 3; break;
    case 2: t.colors = (chctlb >> 1) & 1; t.charSize = chctlb & 1; break;
    default: t.colors = (chctlb >> 5) & 1; t.charSize = chctlb & 0x10; break;
  }
  if (t.colors > 4) t.colors = 4;
  static const int BPP[5] = {4, 8, 16, 16, 32};
  t.bpp = BPP[t.colors];
  t.bmW = (bmSize & 2) ? 1024 : 512;
  t.bmH = (bmSize & 1) ? 512 : 256;
  t.tpOff = reg(0x20) & (0x100 << n);
  t.craOffs = uint32_t((reg(0xE4) >> (n * 4)) & 7) << 8;
  t.mapOffs = (reg(0x3C) >> (n * 4)) & 7;
  t.bmPal = n < 2 ? uint32_t((reg(0x2C) >> (n * 8)) & 7) << 4 : 0;
  const uint16_t pncn = reg(0x30 + uint32_t(n) * 2);
  t.pnd1 = pncn & 0x8000;
  t.aux = pncn & 0x4000;
  t.supp = pncn & 0x3FF;
  t.plsz = (reg(0x3A) >> (n * 2)) & 3;
  const unsigned psshift = 13 - (t.pnd1 ? 1 : 0) - (t.charSize ? 2 : 0);
  for (int i = 0; i < 4; i++) {
    const uint32_t m = (reg(0x40 + uint32_t(n) * 4 + uint32_t(i / 2) * 2) >> ((i & 1) * 8)) & 0x3F;
    t.mapAddr[i] = ((t.mapOffs << 6) + (m & ~uint32_t(t.plsz))) << psshift;
  }
  return t;
}

Vdp2::Tile Vdp2::tileRbg(int which) const {
  Tile t;
  t.rot = true;
  const uint16_t chctlb = reg(0x2A);
  t.colors = std::min((chctlb >> 12) & 7, 4);
  static const int BPP[5] = {4, 8, 16, 16, 32};
  t.bpp = BPP[t.colors];
  t.charSize = chctlb & 0x100;
  t.bitmap = chctlb & 0x200;
  t.bmW = 512;
  t.bmH = (chctlb & 0x400) ? 512 : 256;
  t.tpOff = reg(0x20) & 0x1000;
  t.craOffs = uint32_t(reg(0xE6) & 7) << 8;
  t.mapOffs = (reg(0x3E) >> (which * 4)) & 7;
  t.bmPal = uint32_t(reg(0x2E) & 7) << 4;
  const uint16_t pncr = reg(0x38);
  t.pnd1 = pncr & 0x8000;
  t.aux = pncr & 0x4000;
  t.supp = pncr & 0x3FF;
  t.plsz = (reg(0x3A) >> (8 + which * 4)) & 3;
  t.over = (reg(0x3A) >> (10 + which * 4)) & 3;
  const unsigned psshift = 13 - (t.pnd1 ? 1 : 0) - (t.charSize ? 2 : 0);
  for (int i = 0; i < 16; i++) {
    const uint32_t m = (reg(0x50 + uint32_t(which) * 0x10 + uint32_t(i / 2) * 2) >> ((i & 1) * 8)) & 0x3F;
    t.mapAddr[i] = ((t.mapOffs << 6) + (m & ~uint32_t(t.plsz))) << psshift;
  }
  return t;
}

bool Vdp2::dot(const Tile& t, uint32_t ix, uint32_t iy, uint32_t& rgb) const {
  uint32_t cgByte, palno;
  int cx;
  if (t.rot && t.over >= 2) {  // fora do plano (ou de 512x512): transparente
    uint32_t wx, wy;
    if (t.over == 3) wx = wy = 512;
    else if (t.bitmap) { wx = t.bmW; wy = t.bmH; }
    else { wx = (512u << (t.plsz & 1)) * 4; wy = (512u << ((t.plsz >> 1) & 1)) * 4; }
    if (ix >= wx || iy >= wy) return false;
  }
  if (t.bitmap) {
    const uint32_t px = ix & (t.bmW - 1), py = iy & (t.bmH - 1);
    cgByte = (t.mapOffs << 17) + (py * t.bmW + (px & ~7u)) * uint32_t(t.bpp) / 8;
    cx = int(px & 7);
    palno = t.bmPal;
  } else {
    const uint32_t mapidx = t.rot ? (((ix >> (9 + (t.plsz & 1))) & 3) | ((iy >> (9 + ((t.plsz & 2) ? 1 : 0) - 2)) & 0xC))
                                  : (((ix >> (9 + (t.plsz & 1))) & 1) | ((iy >> (9 + ((t.plsz & 2) ? 1 : 0) - 1)) & 2));
    const uint32_t planeidx = ((ix >> 9) & uint32_t(t.plsz) & 1) | ((iy >> 8) & uint32_t(t.plsz) & 2);
    const unsigned psshift = 13 - (t.pnd1 ? 1 : 0) - (t.charSize ? 2 : 0);
    const uint32_t cs = t.charSize ? 1 : 0;
    const uint32_t pageoffs = ((((ix >> 3) & 0x3F) >> cs) + ((((iy >> 3) & 0x3F) >> cs) << (6 - cs))) << (t.pnd1 ? 0 : 1);
    const uint32_t nt = ((t.mapAddr[mapidx] + (planeidx << psshift) + pageoffs) & 0x3FFFF) * 2;
    const uint16_t w0 = vr16(nt);
    bool vflip, hflip;
    uint32_t charno;
    if (!t.pnd1) {
      const uint16_t w1 = vr16(nt + 2);
      palno = w0 & 0x7F;
      vflip = w0 & 0x8000;
      hflip = w0 & 0x4000;
      charno = w1 & 0x7FFF;
    } else {
      palno = t.bpp >= 8 ? ((w0 >> 12) & 7u) << 4 : ((w0 >> 12) & 0xFu) | ((t.supp >> 5) & 7) << 4;
      if (!t.aux) {
        vflip = w0 & 0x800;
        hflip = w0 & 0x400;
        charno = t.charSize ? ((w0 & 0x3FFu) << 2) + ((t.supp & 0x1C) << 10) + (t.supp & 3) : (w0 & 0x3FFu) + ((t.supp & 0x1F) << 10);
      } else {
        vflip = hflip = false;
        charno = t.charSize ? ((w0 & 0xFFFu) << 2) + ((t.supp & 0x10) << 10) + (t.supp & 3) : (w0 & 0xFFFu) + ((t.supp & 0x1C) << 10);
      }
    }
    if (t.charSize) {
      const uint32_t cidx = (((ix >> 3) ^ (hflip ? 1 : 0)) & 1) + (((iy >> 2) ^ (vflip ? 2 : 0)) & 2);
      charno = (charno + cidx * uint32_t(t.bpp / 4)) & 0x7FFF;
    }
    cx = int(ix & 7) ^ (hflip ? 7 : 0);
    const uint32_t celly = (iy & 7) ^ (vflip ? 7 : 0);
    cgByte = (charno * 32 + celly * uint32_t(t.bpp)) & 0x7FFFF;
  }
  switch (t.bpp) {
    case 4: {
      const uint8_t b = vram[(cgByte + uint32_t(cx) / 2) & 0x7FFFF];
      const uint32_t d = (cx & 1) ? (b & 0xF) : (b >> 4);
      rgb = color(((palno << 4) & ~0xFu) + t.craOffs + d);
      return d || t.tpOff;
    }
    case 8: {
      const uint32_t d = vram[(cgByte + uint32_t(cx)) & 0x7FFFF];
      rgb = color(((palno << 4) & ~0xFFu) + t.craOffs + d);
      return d || t.tpOff;
    }
    case 16: {
      const uint16_t d = vr16(cgByte + uint32_t(cx) * 2);
      if (t.colors == 2) { rgb = color(t.craOffs + (d & 0x7FF)); return (d & 0x7FF) || t.tpOff; }
      rgb = rgb555(d);
      return (d & 0x8000) || t.tpOff;
    }
    default: {
      const uint32_t a = (cgByte + uint32_t(cx) * 4) & 0x7FFFC;
      rgb = uint32_t(vram[a + 3]) | uint32_t(vram[a + 2]) << 8 | uint32_t(vram[a + 1]) << 16;
      return (vram[a] & 0x80) || t.tpOff;
    }
  }
}

void Vdp2::renderNbg(int n, int y, Pix* line, LineScroll* ls) {
  const uint16_t bgon = reg(0x20);
  if (!(bgon & (1 << n))) return;
  const int pri = n < 2 ? (reg(0xF8) >> (n * 8)) & 7 : (reg(0xFA) >> ((n - 2) * 8)) & 7;
  if (!pri) return;
  const Tile t = tileNbg(n);
  const bool ccOn = reg(0xEC) & (1 << n);
  const int ratio = (n < 2 ? reg(0x108) >> (n * 8) : reg(0x10A) >> ((n - 2) * 8)) & 31;
  const uint8_t win = uint8_t(reg(n < 2 ? 0xD0 : 0xD2) >> ((n & 1) * 8));
  uint32_t sxv, iy, zx = 0x100;
  if (n < 2) {
    sxv = ls->x;
    iy = (ls->yBase + ls->yAcc) >> 8;
    zx = ls->zx;
  } else {
    sxv = uint32_t(reg(n == 2 ? 0x90 : 0x94) & 0x7FF) << 8;
    iy = uint32_t((reg(n == 2 ? 0x92 : 0x96) & 0x7FF) + y);
  }
  const int W = outW;
  for (int x = 0; x < W; x++) {
    const uint32_t ix = (sxv + uint32_t(x) * zx) >> 8;
    uint32_t rgb;
    if (!dot(t, ix, iy, rgb)) continue;
    if (windowed(win, x, y)) continue;
    line[W * (L_NBG0 + n) + x] = Pix{rgb, uint8_t(pri), uint8_t(ratio), uint8_t(L_NBG0 + n), ccOn};
  }
}

void Vdp2::renderRbg(int y, Pix* line) {
  const uint16_t bgon = reg(0x20);
  if (!(bgon & 0x10)) return;
  const int pri = reg(0xFC) & 7;
  if (!pri) return;
  const int ab = (reg(0xB0) & 3) == 1 ? 1 : 0;  // RPMD: A ou B (os modos de troca usam A)
  const Tile t = tileRbg(ab);
  // tabela de parâmetros (endereços em palavras de 16 bits)
  const uint32_t a = ((uint32_t(reg(0xBC) & 7) << 16 | (reg(0xBE) & 0xFFFE)) & 0x7FFBE) + uint32_t(ab) * 0x40;
  auto w32 = [&](uint32_t wi) { return uint32_t(vr16((a + wi) * 2)) << 16 | vr16((a + wi + 1) * 2); };
  auto w16 = [&](uint32_t wi) { return uint32_t(vr16((a + wi) * 2)); };
  const int32_t Xst = sx(w32(0x00) >> 6, 23), Yst = sx(w32(0x02) >> 6, 23), Zst = sx(w32(0x04) >> 6, 23);
  const int32_t DXst = sx(w32(0x06) >> 6, 13), DYst = sx(w32(0x08) >> 6, 13);
  const int32_t DX = sx(w32(0x0A) >> 6, 13), DY = sx(w32(0x0C) >> 6, 13);
  int32_t M[6];
  for (int m = 0; m < 6; m++) M[m] = sx(w32(0x0E + uint32_t(m) * 2) >> 6, 14);
  const int32_t Px = sx(w16(0x1A), 14), Py = sx(w16(0x1B), 14), Pz = sx(w16(0x1C), 14);
  const int32_t Cx = sx(w16(0x1E), 14), Cy = sx(w16(0x1F), 14), Cz = sx(w16(0x20), 14);
  const int32_t Mx = sx(w32(0x22) >> 6, 24), My = sx(w32(0x24) >> 6, 24);
  const int32_t kx0 = sx(w32(0x26), 24), ky0 = sx(w32(0x28), 24);
  const uint32_t KAst = w32(0x2A) >> 6;
  const int32_t DKAst = sx(w32(0x2C) >> 6, 20), DKAx = sx(w32(0x2E) >> 6, 20);
  const int32_t XstA = Xst + DXst * y, YstA = Yst + DYst * y;
  const int32_t Xsp = int32_t((int64_t(M[0]) * (XstA - Px * 1024) + int64_t(M[1]) * (YstA - Py * 1024) + int64_t(M[2]) * (Zst - Pz * 1024)) >> 10);
  const int32_t Ysp = int32_t((int64_t(M[3]) * (XstA - Px * 1024) + int64_t(M[4]) * (YstA - Py * 1024) + int64_t(M[5]) * (Zst - Pz * 1024)) >> 10);
  const int32_t Xp0 = M[0] * (Px - Cx) + M[1] * (Py - Cy) + M[2] * (Pz - Cz) + Cx * 1024 + Mx;
  const int32_t Yp = M[3] * (Px - Cx) + M[4] * (Py - Cy) + M[5] * (Pz - Cz) + Cy * 1024 + My;
  const int32_t dX = (M[0] * DX + M[1] * DY) >> 10, dY = (M[3] * DX + M[4] * DY) >> 10;
  // coeficientes (KTCTL: bit 0 liga, bit 1 = 1 palavra, bits 3-2 = o que o coeficiente troca)
  const uint8_t ktctl = uint8_t(reg(0xB4) >> (ab * 8));
  const bool useCoeff = ktctl & 1, coeff1 = ktctl & 2;
  const uint32_t KA = (uint32_t((reg(0xB6) >> (ab * 8)) & 7) << 26) + KAst + uint32_t(DKAst * y);
  const bool ccOn = reg(0xEC) & 0x10;
  const int ratio = reg(0x10C) & 31;
  const uint8_t win = uint8_t(reg(0xD4));
  const int W = outW;
  for (int x = 0; x < W; x++) {
    int32_t kx = kx0, ky = ky0, Xp = Xp0;
    if (useCoeff) {
      uint32_t off = (KA + uint32_t(DKAx * x)) >> 10;
      if (!coeff1) off <<= 1;
      off &= 0x3FFFF;
      uint32_t c;
      if (coeff1) {
        const uint16_t tmp = vr16(off * 2);
        c = (uint32_t(sx(uint32_t(tmp) << 6, 21)) & 0x00FFFFFF) | uint32_t(tmp & 0x8000) << 16;
      } else {
        c = uint32_t(vr16(off * 2)) << 16 | vr16(off * 2 + 2);
      }
      if (c & 0x80000000u) continue;  // coeficiente transparente
      const int32_t e = sx(c, 24);
      switch ((ktctl >> 2) & 3) {
        case 0: kx = ky = e; break;
        case 1: kx = e; break;
        case 2: ky = e; break;
        default: Xp = e << 2; break;
      }
    }
    const uint32_t ix = (uint32_t(Xp) + uint32_t((int64_t(kx) * int32_t(Xsp + dX * x)) >> 16)) >> 10;
    const uint32_t iy = (uint32_t(Yp) + uint32_t((int64_t(ky) * int32_t(Ysp + dY * x)) >> 16)) >> 10;
    uint32_t rgb;
    if (!dot(t, ix, iy, rgb)) continue;
    if (windowed(win, x, y)) continue;
    line[W * L_RBG0 + x] = Pix{rgb, uint8_t(pri), uint8_t(ratio), uint8_t(L_RBG0), ccOn};
  }
}

void Vdp2::renderSprite(int y, Pix* line) {
  const uint16_t spctl = reg(0xE0);
  const int type = spctl & 0xF;
  const bool mixed = spctl & 0x20;
  const uint32_t cao = uint32_t((reg(0xE6) >> 4) & 7) << 8;
  const bool ccEnable = reg(0xEC) & 0x40;
  const int ccCond = (spctl >> 12) & 3, ccNum = (spctl >> 8) & 7;
  const uint8_t win = uint8_t(reg(0xD4) >> 8);
  int prio[8], ratio[8];
  for (int i = 0; i < 8; i++) {
    prio[i] = (reg(0xF0 + uint32_t(i / 2) * 2) >> ((i & 1) * 8)) & 7;
    ratio[i] = (reg(0x100 + uint32_t(i / 2) * 2) >> ((i & 1) * 8)) & 31;
  }
  const Vdp1& v1 = sys_.vdp1;
  const uint16_t* fb = v1.display();
  const bool hires = outW >= 640;
  const bool winEn = spctl & 0x10;
  const int W = outW;
  std::memset(swin_, 0, sizeof swin_);
  std::memset(shadow_, 0, sizeof shadow_);
  if (y >= 256) return;
  for (int x = 0; x < W; x++) {
    uint16_t src;
    if (v1.eightBit()) {
      const int fx = hires ? x : x * 2;
      if (fx >= 1024) continue;
      const uint16_t w = fb[(y * 1024 + fx) >> 1];
      src = uint16_t(0xFF00 | ((fx & 1) ? (w & 0xFF) : (w >> 8)));
    } else {
      const int fx = hires ? x / 2 : x;
      if (fx >= 512) continue;
      src = fb[y * 512 + fx];
    }
    uint32_t rgb;
    int pr = 0, cc = 0;
    if (mixed && (src & 0x8000)) {
      if (type & 8) { if (!(src & 0xFF)) continue; }
      rgb = rgb555(src);
    } else {
      if (type & 8) src &= 0xFF;
      if (!src) continue;
      // tipos 2-7: bit 15 = janela de sprite (com SPWINEN) — o ponto vira janela e não aparece
      if (winEn && type >= 2 && type <= 7 && (src & 0x8000)) { swin_[x] = 1; continue; }
      uint32_t dc;
      switch (type) {
        case 0x0: pr = (src >> 14) & 3; cc = (src >> 11) & 7; dc = src & 0x7FF; break;
        case 0x1: pr = (src >> 13) & 7; cc = (src >> 11) & 3; dc = src & 0x7FF; break;
        case 0x2: pr = (src >> 14) & 1; cc = (src >> 11) & 7; dc = src & 0x7FF; break;
        case 0x3: pr = (src >> 13) & 3; cc = (src >> 11) & 3; dc = src & 0x7FF; break;
        case 0x4: pr = (src >> 13) & 3; cc = (src >> 10) & 7; dc = src & 0x3FF; break;
        case 0x5: pr = (src >> 12) & 7; cc = (src >> 11) & 1; dc = src & 0x7FF; break;
        case 0x6: pr = (src >> 12) & 7; cc = (src >> 10) & 3; dc = src & 0x3FF; break;
        case 0x7: pr = (src >> 12) & 7; cc = (src >> 9) & 7; dc = src & 0x1FF; break;
        case 0x8: pr = (src >> 7) & 1; dc = src & 0x7F; break;
        case 0x9: pr = (src >> 7) & 1; cc = (src >> 6) & 1; dc = src & 0x3F; break;
        case 0xA: pr = (src >> 6) & 3; dc = src & 0x3F; break;
        case 0xB: cc = (src >> 6) & 3; dc = src & 0x3F; break;
        case 0xC: pr = (src >> 7) & 1; dc = src & 0xFF; break;
        case 0xD: pr = (src >> 7) & 1; cc = (src >> 6) & 1; dc = src & 0xFF; break;
        case 0xE: pr = (src >> 6) & 3; dc = src & 0xFF; break;
        default: cc = (src >> 6) & 3; dc = src & 0xFF; break;
      }
      // sombra normal: o código mais alto menos 1 (0x7FE, 0x3FE...) escurece a camada de baixo
      static const uint16_t SHADOW[16] = {0x7FE, 0x7FE, 0x7FE, 0x7FE, 0x3FE, 0x7FE, 0x3FE, 0x1FE,
                                          0x7E, 0x3E, 0x3E, 0x3E, 0xFE, 0xFE, 0xFE, 0xFE};
      if (dc == SHADOW[type]) { if (prio[pr]) shadow_[x] = 1; continue; }
      rgb = color(cao + dc);
    }
    const int p = prio[pr];
    if (!p) continue;
    if (windowed(win, x, y)) continue;
    bool ccOn = ccEnable;
    if (ccOn) {
      switch (ccCond) {
        case 0: ccOn = p <= ccNum; break;
        case 1: ccOn = p == ccNum; break;
        case 2: ccOn = p >= ccNum; break;
        default: ccOn = src & 0x8000; break;
      }
    }
    line[W * L_SPRITE + x] = Pix{rgb, uint8_t(p), uint8_t(ratio[cc]), uint8_t(L_SPRITE), ccOn};
  }
}

void Vdp2::render() {
  outW = hres();
  // entrelaçado de densidade dupla (LSMD = 3): os dois campos juntos, com o dobro de linhas
  const bool dd = ((regs[0] >> 6) & 3) == 3;
  outH = visibleLines() * (dd ? 2 : 1);
  const int W = outW;
  if (!(regs[0] & 0x8000)) {  // DISP desligado: tela preta
    std::fill(out, out + W * outH, 0xFF000000u);
    return;
  }
  std::vector<Pix> layers(size_t(W) * 6);
  const uint16_t ccctl = reg(0xEC), clofen = reg(0x110), clofsl = reg(0x112), scrctl = reg(0x9A), sdctl = reg(0xE2);
  const bool ccAdd = ccctl & 0x100;
  int offs[2][3];
  for (int s = 0; s < 2; s++)
    for (int c = 0; c < 3; c++) {
      const int v = reg(0x114 + uint32_t(s) * 6 + uint32_t(c) * 2) & 0x1FF;
      offs[s][c] = (v & 0x100) ? v - 0x200 : v;
    }
  // rolagem por linha do NBG0/NBG1 (tabela em LSTA: X, Y e ampliação X, conforme SCRCTL)
  LineScroll ls[2];
  uint32_t zy[2], baseX[2];
  for (int n = 0; n < 2; n++) {
    const uint32_t b = n ? 0x80 : 0x70;
    baseX[n] = uint32_t(reg(b) & 0x7FF) << 8 | (reg(b + 2) >> 8);
    ls[n].x = baseX[n];
    ls[n].yBase = uint32_t(reg(b + 4) & 0x7FF) << 8 | (reg(b + 6) >> 8);
    ls[n].yAcc = 0;
    ls[n].zx = uint32_t(reg(b + 8) & 7) << 8 | (reg(b + 10) >> 8);
    zy[n] = uint32_t(reg(b + 12) & 7) << 8 | (reg(b + 14) >> 8);
    if (!ls[n].zx) ls[n].zx = 0x100;
    if (!zy[n]) zy[n] = 0x100;
    ls[n].lsa = (uint32_t(reg(0xA0 + uint32_t(n) * 4) & 7) << 16 | (reg(0xA2 + uint32_t(n) * 4) & 0xFFFE));
  }
  const uint32_t zx0[2] = {ls[0].zx, ls[1].zx};
  for (int y = 0; y < outH; y++) {
    for (int n = 0; n < 2; n++) {
      const uint8_t sc = uint8_t(scrctl >> (n * 8));
      const int lss = (sc >> 4) & 3;
      if ((sc & 0x0E) && (y & ((1 << lss) - 1)) == 0) {
        auto rd = [&]() { const uint32_t v = uint32_t(vr16(ls[n].lsa * 2)) << 16 | vr16(ls[n].lsa * 2 + 2); ls[n].lsa += 2; return v; };
        if (sc & 2) { const uint32_t v = rd(); ls[n].x = (((v >> 16) & 0x7FF) << 8 | ((v >> 8) & 0xFF)) + baseX[n]; }
        if (sc & 4) {
          const uint32_t v = rd();
          const uint32_t b = n ? 0x80 : 0x70;
          ls[n].yBase = (((v >> 16) & 0x7FF) << 8 | ((v >> 8) & 0xFF)) + (uint32_t(reg(b + 4) & 0x7FF) << 8 | (reg(b + 6) >> 8));
          ls[n].yAcc = 0;
        }
        if (sc & 8) { const uint32_t v = rd(); ls[n].zx = ((v >> 16) & 7) << 8 | ((v >> 8) & 0xFF); }
      }
      if (!(sc & 8)) ls[n].zx = zx0[n];
    }
    for (auto& p : layers) p = Pix{0, 0, 0, 0xFF, false};
    renderSprite(dd ? y >> 1 : y, layers.data());
    renderRbg(y, layers.data());
    for (int n = 0; n < 4; n++) renderNbg(n, y, layers.data(), n < 2 ? &ls[n] : nullptr);
    for (int n = 0; n < 2; n++) ls[n].yAcc += zy[n];
    const uint32_t back = backColor(y);
    uint32_t* o = out + y * W;
    for (int x = 0; x < W; x++) {
      // as duas camadas de cima (prioridade maior; empate pela ordem fixa)
      const Pix* top = nullptr;
      const Pix* sec = nullptr;
      for (int l = 0; l < 6; l++) {
        const Pix& p = layers[size_t(W * l + x)];
        if (!p.pri) continue;
        if (!top || p.pri > top->pri) { sec = top; top = &p; }
        else if (!sec || p.pri > sec->pri) sec = &p;
      }
      uint32_t rgb;
      int layer;
      if (!top) { rgb = back; layer = L_BACK; }
      else {
        rgb = top->rgb;
        layer = top->layer;
        if (top->ccOn) {
          const uint32_t below = sec ? sec->rgb : back;
          uint32_t r = 0;
          for (int sh = 0; sh < 24; sh += 8) {
            const int a = int((rgb >> sh) & 0xFF), b = int((below >> sh) & 0xFF);
            const int c = ccAdd ? std::min(255, a + b) : (a * (32 - top->cc) + b * top->cc) / 32;
            r |= uint32_t(c) << sh;
          }
          rgb = r;
        }
      }
      // deslocamento de cor por camada (CLOFEN: NBG0-3 bits 0-3, RBG0 4, fundo 5, sprite 6)
      static const int bitOf[7] = {6, 4, 0, 1, 2, 3, 5};
      const int bit = bitOf[layer];
      if (shadow_[x] && layer != L_SPRITE && (sdctl & (1 << bit))) rgb = (rgb >> 1) & 0x7F7F7F;
      if (clofen & (1 << bit)) {
        const int s = (clofsl >> bit) & 1;
        uint32_t r = 0;
        for (int c = 0; c < 3; c++) r |= uint32_t(std::clamp(int((rgb >> (8 * c)) & 0xFF) + offs[s][c], 0, 255)) << (8 * c);
        rgb = r;
      }
      o[x] = 0xFF000000u | rgb;
    }
  }
}

void Vdp2::sync(StateIO& io) {
  io.bytes(vram);
  io.raw(cram, sizeof cram);
  io.raw(regs, sizeof regs);
  io.sync(outW);
  io.sync(outH);
  io.sync(vb_); io.sync(hb_); io.sync(odd_); io.sync(line_);
  if (io.reading()) {
    if (outW != 320 && outW != 352 && outW != 640 && outW != 704) outW = 320;
    if (outH != 224 && outH != 240 && outH != 256 && outH != 448 && outH != 480 && outH != 512) outH = 224;
  }
}

}  // namespace indigo::saturn
