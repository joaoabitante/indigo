// MADAM do 3DO: motor de cels, motor de matrizes e VDL (ver madam.h).
#include "madam.h"

#include <algorithm>
#include <cstring>

namespace indigo::tdo {

namespace ccb {
constexpr uint32_t SKIP = 0x80000000, LAST = 0x40000000, NPABS = 0x20000000, SPABS = 0x10000000,
                   PPABS = 0x08000000, LDSIZE = 0x04000000, LDPRS = 0x02000000, LDPPMP = 0x01000000,
                   LDPLUT = 0x00800000, CCBPRE = 0x00400000, YOXY = 0x00200000, ACW = 0x00040000,
                   ACCW = 0x00020000, USEAV = 0x00000400, PACKED = 0x00000200, POVER = 0x00000180,
                   BGND = 0x00000020, PLUTA = 0x0000000F;
}

namespace vdl {
constexpr uint32_t ENVIDDMA = 0x00200000, RELSEL = 0x00040000, LDCUR = 0x00010000, LDPREV = 0x00008000;
}

void Madam::reset() {
  std::memset(regs, 0, sizeof regs);
  regs[0] = 0x01020000;  // revisão
  xpos = ypos = hdy = vdx = hddx = hddy = 0;
  hdx = 1 << 20;
  vdy = 1 << 16;
  pixc = 0x1F001F00;
  plutPtr = 0;
  for (int i = 0; i < 32; i++) plut[i] = uint16_t((i << 10) | (i << 5) | i);
  celPixels = 0;
  celsDrawn = 0;
}

uint32_t Madam::read(uint32_t off) {
  off &= 0x7FC;
  if (off == 0x0000) return 0x01020000;
  if (off == 0x07F8) return 0;  // motor de matrizes nunca ocupado (a conta é feita na hora)
  return regs[off / 4];
}

int Madam::write(uint32_t off, uint32_t v) {
  off &= 0x7FC;
  if (off == 0x0000) return 0;
  regs[off / 4] = v;
  if (off == 0x0100) return drawCels(regs[0x5A0 / 4]);
  if (off == 0x07FC) matrixOp(v);
  return 0;
}

void Madam::matrixOp(uint32_t cmd) {
  auto M = [&](int i, int j) { return int64_t(int32_t(regs[(0x600 >> 2) + i * 4 + j])); };
  auto V = [&](int i) { return int64_t(int32_t(regs[(0x640 >> 2) + i])); };
  int64_t out[4] = {0, 0, 0, 0};
  const int n = cmd == 2 ? 4 : 3;
  for (int i = 0; i < n; i++) {
    int64_t s = 0;
    for (int j = 0; j < n; j++) s += M(i, j) * V(j);
    out[i] = s >> 16;
  }
  if (cmd == 3) {
    const int64_t nf = int32_t(regs[0x680 >> 2]);
    const int64_t z = out[2];
    if (z) { out[0] = out[0] * nf / z; out[1] = out[1] * nf / z; }
    else { out[0] = out[1] = 0; }
  }
  for (int i = 0; i < 4; i++) regs[(0x660 >> 2) + i] = uint32_t(int32_t(out[i]));
}

// ------------------------------------------------------------------------------------------------ cels

struct Madam::CelCtx {
  uint32_t flags = 0;
  int width = 320;  // largura do destino
  int clipX = 319, clipY = 239;
  uint32_t readBase = 0, writeBase = 0;
};

uint16_t Madam::blend(uint16_t half, uint16_t cel, uint16_t frame, int amv, bool useAv) const {
  if (half == 0x1F00) return cel;  // opaco: copia a cel
  const bool s1Frame = half & 0x8000;
  const int ms = (half >> 13) & 3;
  int mf = ((half >> 10) & 7) + 1;
  if (ms == 1) mf = amv + 1;
  const int sfCode = (half >> 8) & 3;
  const int shift = sfCode ? sfCode : 4;
  const int s2 = (half >> 6) & 3;
  const int av = (half >> 1) & 0x1F;
  const bool dv2 = half & 1;
  const uint16_t p1 = s1Frame ? frame : cel;
  uint16_t out = cel & 0x8000;
  for (int ch = 0; ch < 3; ch++) {
    const int sh = ch * 5;
    const int a = (p1 >> sh) & 31;
    int t = (a * mf) >> shift;
    int b;
    switch (s2) {
      case 0: b = 0; break;
      case 1: b = av; break;
      case 2: b = (frame >> sh) & 31; break;
      default: b = (cel >> sh) & 31; break;
    }
    if (dv2) b >>= 1;
    // USEAV com o bit 0 do AV: o segundo operando é subtraído
    int r = (useAv && (av & 1)) ? t - b : t + b;
    if (r < 0) r = 0;
    if (r > 31) r = 31;
    out |= uint16_t(r << sh);
  }
  return out;
}

void Madam::putPixel(CelCtx& c, int x, int y, uint16_t color, bool pbit, int amv) {
  if (x < 0 || y < 0 || x > c.clipX || y > c.clipY) return;
  const uint32_t off = uint32_t(y >> 1) * uint32_t(c.width) * 4 + uint32_t(x) * 4 + uint32_t(y & 1) * 2;
  // POVER: 00 = P do pixel, 10 = P zero, 11 = P um
  const uint32_t pov = (c.flags & ccb::POVER) >> 7;
  if (pov == 2) pbit = false;
  else if (pov == 3) pbit = true;
  const uint16_t half = pbit ? uint16_t(pixc >> 16) : uint16_t(pixc);
  uint16_t outc = color;
  if (half != 0x1F00) {
    const uint16_t frame = m16(c.readBase + off);
    outc = blend(half, color, frame, amv, c.flags & ccb::USEAV);
  }
  w16(c.writeBase + off, outc);
  celPixels++;
}

void Madam::plotQuad(CelCtx& c, const int64_t (&px)[4], const int64_t (&py)[4], uint16_t color, bool pbit, int amv) {
  // área com sinal: > 0 no sentido horário da tela (y para baixo)
  int64_t area = 0;
  for (int i = 0; i < 4; i++) {
    const int j = (i + 1) & 3;
    area += px[i] * py[j] - px[j] * py[i];
  }
  if (area == 0) return;
  if (area > 0 && !(c.flags & ccb::ACW)) return;
  if (area < 0 && !(c.flags & ccb::ACCW)) return;
  int64_t qx[4], qy[4];
  for (int i = 0; i < 4; i++) {
    const int k = area > 0 ? i : 3 - i;
    qx[i] = px[k];
    qy[i] = py[k];
  }
  int64_t minx = qx[0], maxx = qx[0], miny = qy[0], maxy = qy[0];
  for (int i = 1; i < 4; i++) {
    minx = std::min(minx, qx[i]); maxx = std::max(maxx, qx[i]);
    miny = std::min(miny, qy[i]); maxy = std::max(maxy, qy[i]);
  }
  // centros de pixel em (n*256 + 128)
  int x0 = int(std::max<int64_t>((minx - 128 + 255) >> 8, 0));
  int x1 = int(std::min<int64_t>((maxx - 128) >> 8, c.clipX));
  int y0 = int(std::max<int64_t>((miny - 128 + 255) >> 8, 0));
  int y1 = int(std::min<int64_t>((maxy - 128) >> 8, c.clipY));
  if (x0 > x1 || y0 > y1) return;
  // eixo alinhado (caso comum): o quadrilátero é um retângulo
  const bool rect = (qy[0] == qy[1] && qx[1] == qx[2] && qy[2] == qy[3] && qx[3] == qx[0]) ||
                    (qx[0] == qx[1] && qy[1] == qy[2] && qx[2] == qx[3] && qy[3] == qy[0]);
  for (int y = y0; y <= y1; y++) {
    const int64_t cy = int64_t(y) * 256 + 128;
    for (int x = x0; x <= x1; x++) {
      const int64_t cx = int64_t(x) * 256 + 128;
      bool in = true;
      for (int i = 0; i < 4 && in; i++) {
        const int j = (i + 1) & 3;
        const int64_t dx = qx[j] - qx[i], dy = qy[j] - qy[i];
        const int64_t e = dx * (cy - qy[i]) - dy * (cx - qx[i]);
        if (e < 0) in = false;
        else if (e == 0 && !(dy < 0 || (dy == 0 && dx > 0))) in = false;
      }
      if (in) putPixel(c, x, y, color, pbit, amv);
    }
  }
  (void)rect;
}

namespace {
class BitReader {
public:
  BitReader(const Madam& m, uint32_t addr) : m_(m), addr_(addr) {}
  uint32_t get(int n) {
    uint32_t v = 0;
    while (n > 0) {
      if (!left_) { word_ = m_.m32(addr_); addr_ += 4; left_ = 32; }
      const int take = std::min(n, left_);
      const uint32_t bits = (word_ >> (left_ - take)) & ((take == 32) ? 0xFFFFFFFFu : ((1u << take) - 1));
      v = (take == 32) ? bits : (v << take) | bits;
      left_ -= take;
      n -= take;
    }
    return v;
  }
private:
  const Madam& m_;
  uint32_t addr_, word_ = 0;
  int left_ = 0;
};

int bppOf(uint32_t pre0) {
  static const int T[8] = {0, 1, 2, 4, 6, 8, 16, 0};
  return T[pre0 & 7];
}
}  // namespace

void Madam::drawOne(uint32_t flags, uint32_t src, uint32_t pre0, uint32_t pre1) {
  const int bpp = bppOf(pre0);
  if (!bpp) return;
  const bool linear = pre0 & 0x10;  // sem PLUT (cor direta)
  const bool packed = flags & ccb::PACKED;
  const int height = int((pre0 >> 6) & 0x3FF) + 1;
  const int width = packed ? 2048 : int(pre1 & 0x7FF) + 1;
  const int skipX = packed ? 0 : int((pre0 >> 24) & 0xF);
  const uint32_t woff = bpp >= 8 ? (pre1 >> 16) & 0xFF : (pre1 >> 16) & 0x3FF;
  const uint32_t pluta = flags & ccb::PLUTA;
  const bool bgnd = flags & ccb::BGND;

  CelCtx c;
  c.flags = flags;
  c.width = int(regs[0x130 / 4] & 0x7FF);
  if (!c.width) c.width = 320;
  const uint32_t clip = regs[0x134 / 4];
  if (clip) { c.clipX = int(clip & 0x7FF); c.clipY = int((clip >> 16) & 0x7FF); }
  c.readBase = regs[0x138 / 4];
  c.writeBase = regs[0x13C / 4];

  auto decode = [&](uint32_t p, uint16_t& color, bool& pbit, int& amv) -> bool {
    amv = 7;
    if (linear) {
      if (bpp == 16) color = uint16_t(p);
      else if (bpp == 8) {
        const uint32_t r = (p >> 5) & 7, g = (p >> 2) & 7, b = p & 3;
        color = uint16_t(((r << 2 | r >> 1) << 10) | ((g << 2 | g >> 1) << 5) | (b << 3 | b << 1 | b >> 1));
      } else {
        // poucos bits sem PLUT: a intensidade vira cinza
        const uint32_t m = (1u << bpp) - 1;
        const uint32_t v = m ? p * 31 / m : 0;
        color = uint16_t(v << 10 | v << 5 | v);
      }
      pbit = color & 0x8000;
    } else {
      uint32_t idx;
      switch (bpp) {
        case 1: idx = (pluta << 1) | p; break;
        case 2: idx = ((pluta & 7) << 2) | p; break;
        case 4: idx = ((pluta & 1) << 4) | p; break;
        default: idx = p & 31; break;
      }
      color = plut[idx & 31];
      pbit = color & 0x8000;
      if (bpp == 6) { pbit = (p >> 5) & 1; color = uint16_t((color & 0x7FFF) | (pbit ? 0x8000 : 0)); }
      if (bpp == 8) amv = int((p >> 5) & 7);
    }
    return bgnd || (color & 0x7FFF) != 0;
  };

  // pontos dos cantos em 1/256 de pixel: P(col, linha)
  const int64_t X20 = int64_t(xpos) * 16, Y20 = int64_t(ypos) * 16;
  const int64_t VDX20 = int64_t(vdx) * 16, VDY20 = int64_t(vdy) * 16;
  auto point = [&](int col, int row, int64_t& ox, int64_t& oy) {
    const int64_t hx = int64_t(hdx) + int64_t(row) * hddx, hy = int64_t(hdy) + int64_t(row) * hddy;
    ox = (X20 + int64_t(row) * VDX20 + int64_t(col) * hx) >> 12;
    oy = (Y20 + int64_t(row) * VDY20 + int64_t(col) * hy) >> 12;
  };
  auto drawPixel = [&](int col, int row, uint16_t color, bool pbit, int amv) {
    int64_t px[4], py[4];
    point(col, row, px[0], py[0]);
    point(col + 1, row, px[1], py[1]);
    point(col + 1, row + 1, px[2], py[2]);
    point(col, row + 1, px[3], py[3]);
    plotQuad(c, px, py, color, pbit, amv);
  };

  uint32_t row = src;
  for (int r = 0; r < height; r++) {
    if (packed) {
      BitReader br(*this, row);
      const uint32_t off = br.get(bpp >= 8 ? 10 : 8);
      int col = 0;
      for (int guard = 0; guard < 2048 && col < 2048; guard++) {
        const uint32_t type = br.get(2);
        const int count = int(br.get(6)) + 1;
        if (type == 0) break;
        if (type == 1) {
          for (int k = 0; k < count; k++, col++) {
            uint16_t color; bool pb; int amv;
            if (decode(br.get(bpp), color, pb, amv)) drawPixel(col, r, color, pb, amv);
          }
        } else if (type == 2) {
          col += count;
        } else {
          uint16_t color; bool pb; int amv;
          const bool vis = decode(br.get(bpp), color, pb, amv);
          for (int k = 0; k < count; k++, col++) if (vis) drawPixel(col, r, color, pb, amv);
        }
      }
      row += (off + 2) * 4;
    } else {
      BitReader br(*this, row);
      for (int k = 0; k < skipX; k++) br.get(bpp);
      for (int col = 0; col + skipX < width; col++) {
        uint16_t color; bool pb; int amv;
        if (decode(br.get(bpp), color, pb, amv)) drawPixel(col, r, color, pb, amv);
      }
      row += (woff + 2) * 4;
    }
  }
}

int Madam::drawCels(uint32_t addr) {
  const uint64_t before = celPixels;
  int cels = 0;
  for (; cels < 4096; cels++) {
    addr = (addr % MEM_SIZE) & ~3u;
    const uint32_t flags = m32(addr);
    const uint32_t nextRaw = m32(addr + 4), srcRaw = m32(addr + 8), plutRaw = m32(addr + 12);
    const uint32_t next = (flags & ccb::NPABS) ? nextRaw : addr + 4 + 4 + nextRaw;
    if (!(flags & ccb::SKIP)) {
      uint32_t src = (flags & ccb::SPABS) ? srcRaw : addr + 8 + 4 + srcRaw;
      const uint32_t pl = (flags & ccb::PPABS) ? plutRaw : addr + 12 + 4 + plutRaw;
      if (flags & ccb::YOXY) { xpos = int32_t(m32(addr + 16)); ypos = int32_t(m32(addr + 20)); }
      if (flags & ccb::LDSIZE) {
        hdx = int32_t(m32(addr + 24)); hdy = int32_t(m32(addr + 28));
        vdx = int32_t(m32(addr + 32)); vdy = int32_t(m32(addr + 36));
      }
      if (flags & ccb::LDPRS) { hddx = int32_t(m32(addr + 40)); hddy = int32_t(m32(addr + 44)); }
      if (flags & ccb::LDPPMP) pixc = m32(addr + 48);
      uint32_t pre0, pre1 = 0;
      if (flags & ccb::CCBPRE) { pre0 = m32(addr + 52); pre1 = m32(addr + 56); }
      else {
        pre0 = m32(src); src += 4;
        if (!(flags & ccb::PACKED)) { pre1 = m32(src); src += 4; }
      }
      if (flags & ccb::LDPLUT) {
        plutPtr = pl;
        const int bpp = bppOf(pre0);
        const int n = bpp == 1 ? 2 : bpp == 2 ? 4 : bpp == 4 ? 16 : 32;
        for (int i = 0; i < n; i++) plut[i] = m16(pl + uint32_t(i) * 2);
      }
      drawOne(flags, src, pre0, pre1);
      celsDrawn++;
    }
    if (flags & ccb::LAST) { cels++; break; }
    addr = next;
  }
  regs[0x5A0 / 4] = addr;
  // tempo: ~2 pixels por ciclo da CPU e o custo de ler cada CCB
  return int((celPixels - before) / 2) + cels * 20;
}

// ------------------------------------------------------------------------------------------------ VDL

void Madam::renderFrame(uint32_t* out) {
  uint8_t clut[3][32];
  for (int i = 0; i < 32; i++) clut[0][i] = clut[1][i] = clut[2][i] = uint8_t(i << 3 | i >> 2);
  uint32_t bg = 0xFF000000;
  uint32_t vdlAddr = regs[0x580 / 4];
  uint32_t rowAddr = 0;
  bool video = false;
  int y = 0;
  for (int entries = 0; y < 240 && entries < 512; entries++) {
    vdlAddr = (vdlAddr % MEM_SIZE) & ~3u;
    const uint32_t ctrl = m32(vdlAddr), cur = m32(vdlAddr + 4), nxt = m32(vdlAddr + 12);
    const int nwords = int((ctrl >> 9) & 0x3F);
    int nlines = int(ctrl & 0x1FF);
    if (!nlines) nlines = 240 - y;
    if (ctrl & vdl::LDCUR) rowAddr = cur;
    video = ctrl & vdl::ENVIDDMA;
    for (int k = 0; k < nwords; k++) {
      const uint32_t w = m32(vdlAddr + 16 + uint32_t(k) * 4);
      if (!(w & 0x80000000u)) {
        const int idx = (w >> 24) & 31, sel = (w >> 29) & 3;
        const uint8_t rr = uint8_t(w >> 16), gg = uint8_t(w >> 8), bb = uint8_t(w);
        if (sel == 0 || sel == 3) clut[0][idx] = rr;
        if (sel == 0 || sel == 2) clut[1][idx] = gg;
        if (sel == 0 || sel == 1) clut[2][idx] = bb;
      } else if ((w >> 29) == 7) {
        bg = 0xFF000000u | (w & 0xFF) << 16 | (w & 0xFF00) | (w >> 16 & 0xFF);
      }
    }
    for (int l = 0; l < nlines && y < 240; l++, y++) {
      uint32_t* o = out + y * 320;
      if (!video) { std::fill(o, o + 320, bg); }
      else {
        for (int x = 0; x < 320; x++) {
          const uint16_t p = m16(rowAddr + uint32_t(x) * 4 + uint32_t(y & 1) * 2);
          if (!p) { o[x] = bg; continue; }
          o[x] = 0xFF000000u | uint32_t(clut[2][p & 31]) << 16 | uint32_t(clut[1][(p >> 5) & 31]) << 8 | clut[0][(p >> 10) & 31];
        }
      }
      if (y & 1) rowAddr += 320 * 4;
    }
    vdlAddr = (ctrl & vdl::RELSEL) ? vdlAddr + nxt : nxt;
  }
  for (; y < 240; y++) std::fill(out + y * 320, out + y * 320 + 320, bg);
}

void Madam::sync(StateIO& io) {
  io.raw(regs, sizeof regs);
  io.sync(xpos); io.sync(ypos); io.sync(hdx); io.sync(hdy); io.sync(vdx); io.sync(vdy); io.sync(hddx); io.sync(hddy);
  io.sync(pixc); io.sync(plutPtr);
  io.raw(plut, sizeof plut);
  io.sync(celPixels); io.sync(celsDrawn);
}

}  // namespace indigo::tdo
