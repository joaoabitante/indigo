// VDP1 do Saturn: lista de comandos na VRAM (sprites normais, ampliados e distorcidos, polígonos, linhas,
// recortes e coordenadas locais) desenhada num dos dois quadros de 512x256 (16 bits por ponto).
//
// Simplificações conhecidas: o desenho é instantâneo (no começo do V-blank ou na escrita de PTMR = 1); o código
// de fim de linha vira ponto transparente; não há modos de quadro de alta resolução/rotação além do 8 bits.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "saturn.h"

namespace indigo::saturn {

struct Vdp1::Cmd {
  uint16_t ctrl, link, pmod, colr, srca, size, grda;
  int x[4], y[4];
  int w, h;
  bool hflip, vflip;
  uint16_t gour[4];
};

namespace {
inline int sx13(uint16_t v) { return int(int16_t(v << 3)) >> 3; }
inline uint16_t blend(uint16_t a, uint16_t b) {  // média de duas cores RGB555
  const int r = ((a & 31) + (b & 31)) >> 1, g = (((a >> 5) & 31) + ((b >> 5) & 31)) >> 1, bl = (((a >> 10) & 31) + ((b >> 10) & 31)) >> 1;
  return uint16_t(0x8000 | bl << 10 | g << 5 | r);
}
inline uint16_t half(uint16_t a) { return uint16_t(0x8000 | ((a >> 1) & 0x3DEF)); }
}  // namespace

void Vdp1::reset() {
  std::fill(vram.begin(), vram.end(), 0);
  std::memset(fb, 0, sizeof fb);
  drawBuf = 0;
  tvmr_ = fbcr_ = ptmr_ = ewdr_ = ewlr_ = 0;
  ewrr_ = 0x50DF;
  edsr_ = lopr_ = copr_ = modr_ = 0;
  manualSwap_ = manualErase_ = false;
  sysClipX_ = 319; sysClipY_ = 223;
  userX0_ = userY0_ = 0; userX1_ = 319; userY1_ = 223;
  localX_ = localY_ = 0;
  drawPending_ = false;
  commandsRun = 0;
}

uint16_t Vdp1::readReg(uint32_t off) {
  switch (off & 0x1E) {
    case 0x10: return edsr_;
    case 0x12: return lopr_;
    case 0x14: return copr_;
    case 0x16: return uint16_t(0x1000 | (ptmr_ & 3) << 8 | (fbcr_ & 0x1F) << 3 | (tvmr_ & 0xF));
    default: return 0;
  }
}

void Vdp1::writeReg(uint32_t off, uint16_t v) {
  switch (off & 0x1E) {
    case 0x00: tvmr_ = v & 0xF; break;
    case 0x02:
      fbcr_ = v & 0x1F;
      if ((v & 3) == 3) manualSwap_ = true;
      else if ((v & 3) == 2) manualErase_ = true;
      break;
    case 0x04:
      ptmr_ = v & 3;
      if (ptmr_ == 1) draw();
      break;
    case 0x06: ewdr_ = v; break;
    case 0x08: ewlr_ = v; break;
    case 0x0A: ewrr_ = v; break;
    case 0x0C: break;  // ENDR: o desenho já terminou
    default: break;
  }
}

uint16_t Vdp1::fbRead16(uint32_t off) const { return fb[drawBuf][(off >> 1) & 0x1FFFF]; }
void Vdp1::fbWrite16(uint32_t off, uint16_t v) { fb[drawBuf][(off >> 1) & 0x1FFFF] = v; }
void Vdp1::fbWrite8(uint32_t off, uint8_t v) {
  uint16_t& w = fb[drawBuf][(off >> 1) & 0x1FFFF];
  w = (off & 1) ? uint16_t((w & 0xFF00) | v) : uint16_t((w & 0x00FF) | v << 8);
}

void Vdp1::eraseFb(int buf) {
  const int x1 = ((ewlr_ >> 9) & 0x3F) * 8, y1 = ewlr_ & 0x1FF;
  const int x3 = ((ewrr_ >> 9) & 0x7F) * 8, y3 = ewrr_ & 0x1FF;
  const int stride = eightBit() ? 512 : 512;  // em palavras: 1024 pontos de 8 bits = 512 palavras
  const int xs = eightBit() ? x1 / 2 : x1, xe = eightBit() ? x3 / 2 : x3;
  for (int y = y1; y <= y3 && y < 256; y++)
    for (int x = xs; x < xe && x < 512; x++) fb[buf][y * stride + x] = ewdr_;
}

void Vdp1::vblankIn() {
  const bool autoMode = !(fbcr_ & 2);
  if (autoMode || manualSwap_) {
    drawBuf ^= 1;
    edsr_ = uint16_t((edsr_ & 2) >> 1);  // BEF <- CEF
    if (autoMode || manualErase_) eraseFb(drawBuf);
    manualSwap_ = manualErase_ = false;
    if (ptmr_ == 2) draw();
  } else if (manualErase_) {
    eraseFb(drawBuf ^ 1);
    manualErase_ = false;
  }
}

bool Vdp1::texel(const Cmd& c, int u, int v, uint16_t& out, bool& endCode) const {
  if (u < 0 || v < 0 || u >= c.w || v >= c.h) return false;
  const uint32_t base = uint32_t(c.srca) * 8;
  const int cm = (c.pmod >> 3) & 7;
  const bool spd = c.pmod & 0x40, ecd = c.pmod & 0x80;
  endCode = false;
  uint32_t dot;
  switch (cm) {
    case 0: case 1: {
      const uint8_t b = vram[(base + uint32_t(v * c.w + u) / 2) & 0x7FFFF];
      dot = (u & 1) ? (b & 0xF) : (b >> 4);
      if (dot == 0xF && !ecd) { endCode = true; return false; }
      if (dot == 0 && !spd) return false;
      out = cm == 0 ? uint16_t((c.colr & 0xFFF0) | dot) : vr16(uint32_t(c.colr) * 8 + dot * 2);
      return true;
    }
    case 2: case 3: case 4: {
      dot = vram[(base + uint32_t(v * c.w + u)) & 0x7FFFF];
      if (dot == 0xFF && !ecd) { endCode = true; return false; }
      if (dot == 0 && !spd) return false;
      const uint16_t m = cm == 2 ? 0x3F : cm == 3 ? 0x7F : 0xFF;
      out = uint16_t((c.colr & ~m) | (dot & m));
      return true;
    }
    default: {
      dot = vr16(base + uint32_t(v * c.w + u) * 2);
      if (dot == 0x7FFF && !ecd) { endCode = true; return false; }
      if (dot == 0 && !spd) return false;
      out = uint16_t(dot);
      return true;
    }
  }
}

void Vdp1::plot(int x, int y, uint16_t color, const Cmd& c) {
  if (x < 0 || y < 0 || x > sysClipX_ || y > sysClipY_) return;
  if (c.pmod & 0x400) {  // recorte do usuário: bit 9 = desenha fora
    const bool in = x >= userX0_ && x <= userX1_ && y >= userY0_ && y <= userY1_;
    if (in == bool(c.pmod & 0x200)) return;
  }
  if ((c.pmod & 0x100) && ((x ^ y) & 1)) return;  // malha
  if (eightBit()) {
    if (x >= 1024 || y >= 256) return;
    uint16_t& w = fb[drawBuf][(y * 1024 + x) >> 1];
    if (x & 1) w = uint16_t((w & 0xFF00) | (color & 0xFF)); else w = uint16_t((w & 0x00FF) | (color & 0xFF) << 8);
    return;
  }
  if (x >= 512 || y >= 256) return;
  uint16_t& d = fb[drawBuf][y * 512 + x];
  if (c.pmod & 0x8000) { d |= 0x8000; return; }  // MSB on
  switch (c.pmod & 3) {
    case 1:  // sombra: escurece o que já está desenhado (só cor RGB)
      if (d & 0x8000) d = half(d);
      return;
    case 2:  // meia luminância
      d = (color & 0x8000) ? half(color) : color;
      return;
    case 3:  // meia transparência
      d = ((color & 0x8000) && (d & 0x8000)) ? blend(color, d) : color;
      return;
    default: d = color; return;
  }
}

// linha do VDP1: passos pelo eixo maior; com dois eixos mudando, um ponto extra evita buracos
void Vdp1::line(const Cmd& c, int x0, int y0, int x1, int y1, int v, bool textured, int texW, const int* g0, const int* g1) {
  const int dx = x1 - x0, dy = y1 - y0;
  const int m = std::max(std::abs(dx), std::abs(dy));
  int px = x0, py = y0;
  bool ended = false;
  if (budget_ <= 0) return;
  budget_ -= m + 1;
  for (int j = 0; j <= m; j++) {
    const int x = m ? x0 + (dx * j + (dx >= 0 ? m / 2 : -m / 2)) / m : x0;
    const int y = m ? y0 + (dy * j + (dy >= 0 ? m / 2 : -m / 2)) / m : y0;
    uint16_t col = c.colr;
    bool draw = true;
    if (textured) {
      int u = m ? j * texW / (m + 1) : 0;
      if (c.hflip) u = texW - 1 - u;
      bool end = false;
      draw = !ended && texel(c, u, v, col, end);
      if (end) draw = false;
    }
    if (draw && g0 && (col & 0x8000)) {
      int ch[3];
      for (int k = 0; k < 3; k++) {
        const int g = m ? g0[k] + (g1[k] - g0[k]) * j / m : g0[k];
        ch[k] = std::clamp(int((col >> (5 * k)) & 31) + g - 16, 0, 31);
      }
      col = uint16_t(0x8000 | ch[2] << 10 | ch[1] << 5 | ch[0]);
    }
    if (draw) {
      if (j && x != px && y != py) plot(x, py, col, c);
      plot(x, y, col, c);
    }
    px = x;
    py = y;
  }
}

void Vdp1::quad(const Cmd& c, const int* xs, const int* ys, bool textured) {
  // lados A->D (esquerdo) e B->C (direito), andando pelo maior deles
  const int dl = std::max(std::abs(xs[3] - xs[0]), std::abs(ys[3] - ys[0]));
  const int dr = std::max(std::abs(xs[2] - xs[1]), std::abs(ys[2] - ys[1]));
  const int n = std::max(dl, dr);
  if (n > 2048) return;  // coordenadas absurdas (o aparelho também desiste)
  const bool gour = (c.pmod & 4) != 0;
  int gc[4][3];
  for (int k = 0; k < 4; k++)
    for (int ch = 0; ch < 3; ch++) gc[k][ch] = (c.gour[k] >> (5 * ch)) & 31;
  auto lerp = [n](int a, int b, int i) { return n ? a + ((b - a) * i + ((b - a) >= 0 ? n / 2 : -n / 2)) / n : a; };
  for (int i = 0; i <= n; i++) {
    const int lx = lerp(xs[0], xs[3], i), ly = lerp(ys[0], ys[3], i);
    const int rx = lerp(xs[1], xs[2], i), ry = lerp(ys[1], ys[2], i);
    int v = textured ? (n ? i * c.h / (n + 1) : 0) : 0;
    if (textured && c.vflip) v = c.h - 1 - v;
    int g0[3], g1[3];
    for (int ch = 0; ch < 3; ch++) {
      g0[ch] = n ? gc[0][ch] + (gc[3][ch] - gc[0][ch]) * i / n : gc[0][ch];
      g1[ch] = n ? gc[1][ch] + (gc[2][ch] - gc[1][ch]) * i / n : gc[1][ch];
    }
    line(c, lx, ly, rx, ry, v, textured, c.w, gour ? g0 : nullptr, gour ? g1 : nullptr);
  }
}

void Vdp1::sprite(const Cmd& c) {
  // sprite normal: cópia direta, com espelhamento
  bool gour = (c.pmod & 4) != 0;
  if (budget_ <= 0) return;
  budget_ -= c.w * c.h;
  for (int v = 0; v < c.h; v++) {
    for (int u = 0; u < c.w; u++) {
      uint16_t col;
      bool end = false;
      const int tu = c.hflip ? c.w - 1 - u : u, tv = c.vflip ? c.h - 1 - v : v;
      if (!texel(c, tu, tv, col, end)) continue;
      if (gour && (col & 0x8000)) {
        int ch[3];
        for (int k = 0; k < 3; k++) {
          const int a = (c.gour[0] >> (5 * k)) & 31, b = (c.gour[1] >> (5 * k)) & 31;
          const int cc = (c.gour[2] >> (5 * k)) & 31, d = (c.gour[3] >> (5 * k)) & 31;
          const int top = c.w > 1 ? a + (b - a) * u / (c.w - 1) : a, bot = c.w > 1 ? d + (cc - d) * u / (c.w - 1) : d;
          const int g = c.h > 1 ? top + (bot - top) * v / (c.h - 1) : top;
          ch[k] = std::clamp(int((col >> (5 * k)) & 31) + g - 16, 0, 31);
        }
        col = uint16_t(0x8000 | ch[2] << 10 | ch[1] << 5 | ch[0]);
      }
      plot(c.x[0] + u, c.y[0] + v, col, c);
    }
  }
}

void Vdp1::draw() {
  uint32_t addr = 0, ret = 0;
  commandsRun = 0;
  budget_ = 8 * 1024 * 1024;  // pontos por desenho: uma lista absurda não trava o app
  for (int guard = 0; guard < 16384; guard++) {
    const uint16_t ctrl = vr16(addr);
    copr_ = uint16_t(addr >> 3);
    if (ctrl & 0x8000) break;  // fim da lista
    Cmd c{};
    c.ctrl = ctrl;
    c.link = vr16(addr + 2);
    c.pmod = vr16(addr + 4);
    c.colr = vr16(addr + 6);
    c.srca = vr16(addr + 8);
    c.size = vr16(addr + 10);
    for (int k = 0; k < 4; k++) { c.x[k] = sx13(vr16(addr + 12 + k * 4)); c.y[k] = sx13(vr16(addr + 14 + k * 4)); }
    c.grda = vr16(addr + 28);
    for (int k = 0; k < 4; k++) c.gour[k] = vr16(uint32_t(c.grda) * 8 + k * 2);
    c.w = ((c.size >> 8) & 0x3F) * 8;
    c.h = c.size & 0xFF;
    c.hflip = ctrl & 0x10;
    c.vflip = ctrl & 0x20;
    const int jp = (ctrl >> 12) & 7;
    if (!(jp & 4)) {
      commandsRun++;
      const int cmd = ctrl & 0xF;
      switch (cmd) {
        case 0x0:
          c.x[0] += localX_; c.y[0] += localY_;
          sprite(c);
          break;
        case 0x1: {  // sprite ampliado
          int x0, y0, x1, y1;
          const int zp = (ctrl >> 8) & 0xF;
          if (!zp) { x0 = c.x[0]; y0 = c.y[0]; x1 = c.x[2]; y1 = c.y[2]; }
          else {
            const int w = c.x[1], h = c.y[1];
            x0 = c.x[0]; y0 = c.y[0];
            if ((zp & 3) == 2) x0 -= w / 2; else if ((zp & 3) == 3) x0 -= w;
            if (((zp >> 2) & 3) == 2) y0 -= h / 2; else if (((zp >> 2) & 3) == 3) y0 -= h;
            x1 = x0 + w; y1 = y0 + h;
          }
          const int xs[4] = {x0 + localX_, x1 + localX_, x1 + localX_, x0 + localX_};
          const int ys[4] = {y0 + localY_, y0 + localY_, y1 + localY_, y1 + localY_};
          quad(c, xs, ys, true);
          break;
        }
        case 0x2: case 0x3: case 0x4: {  // sprite distorcido e polígono
          int xs[4], ys[4];
          for (int k = 0; k < 4; k++) { xs[k] = c.x[k] + localX_; ys[k] = c.y[k] + localY_; }
          if (cmd == 4) c.pmod |= 0xC0;  // polígono: cor única, sem transparência
          quad(c, xs, ys, cmd != 4);
          break;
        }
        case 0x5: case 0x7: case 0x6: {  // polilinha e linha
          c.pmod |= 0xC0;
          const int npts = cmd == 6 ? 2 : 4;
          for (int k = 0; k < (cmd == 6 ? 1 : 4); k++) {
            const int a = k, b = (k + 1) % npts;
            int g0[3], g1[3];
            for (int ch = 0; ch < 3; ch++) { g0[ch] = (c.gour[a] >> (5 * ch)) & 31; g1[ch] = (c.gour[b] >> (5 * ch)) & 31; }
            line(c, c.x[a] + localX_, c.y[a] + localY_, c.x[b] + localX_, c.y[b] + localY_, 0, false, 0,
                 (c.pmod & 4) ? g0 : nullptr, (c.pmod & 4) ? g1 : nullptr);
          }
          break;
        }
        case 0x8: case 0xB:
          userX0_ = c.x[0] & 0x3FF; userY0_ = c.y[0] & 0x1FF; userX1_ = c.x[2] & 0x3FF; userY1_ = c.y[2] & 0x1FF;
          break;
        case 0x9: sysClipX_ = c.x[2] & 0x3FF; sysClipY_ = c.y[2] & 0x1FF; break;
        case 0xA: localX_ = c.x[0]; localY_ = c.y[0]; break;
        default: break;
      }
    }
    lopr_ = uint16_t(addr >> 3);
    switch (jp & 3) {
      case 0: addr += 32; break;
      case 1: addr = uint32_t(c.link) * 8; break;
      case 2: ret = addr + 32; addr = uint32_t(c.link) * 8; break;
      default: addr = ret; break;
    }
    addr &= 0x7FFFF;
  }
  edsr_ |= 2;  // CEF: a lista terminou
  sys_.scu.raise(INT_VDP1);
  sys_.scu.factor(DF_SPRITE);
}

void Vdp1::sync(StateIO& io) {
  io.bytes(vram);
  io.raw(fb, sizeof fb);
  io.sync(drawBuf);
  io.sync(tvmr_); io.sync(fbcr_); io.sync(ptmr_); io.sync(ewdr_); io.sync(ewlr_); io.sync(ewrr_);
  io.sync(edsr_); io.sync(lopr_); io.sync(copr_); io.sync(modr_);
  io.sync(manualSwap_); io.sync(manualErase_);
  io.sync(sysClipX_); io.sync(sysClipY_); io.sync(userX0_); io.sync(userY0_); io.sync(userX1_); io.sync(userY1_);
  io.sync(localX_); io.sync(localY_);
  if (io.reading()) drawBuf &= 1;
}

}  // namespace indigo::saturn
