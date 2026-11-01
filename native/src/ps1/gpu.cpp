#include "gpu.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>

#include "system.h"

namespace indigo::ps1 {

namespace {
constexpr int VRAM_W = 1024, VRAM_H = 512;

const int8_t DITHER[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

inline int sx11(uint32_t v) { return int32_t(v << 21) >> 21; }
inline uint32_t c5to8(uint32_t c) { return (c << 3) | (c >> 2); }

// valor de 8 bits (ou mais, na modulação) -> 5 bits, com ou sem pontilhado
inline uint32_t to5(int v, int d) {
  v += d;
  v = v < 0 ? 0 : v > 255 ? 255 : v;
  return uint32_t(v) >> 3;
}

// posição x em 32.32 para as bordas do triângulo (centro de ponto com a regra de preenchimento do PS1)
// mistura semitransparente (fundo d, frente c) do modo da página
inline uint16_t blend(uint16_t d, uint16_t c, uint32_t page) {
  const int br = d & 31, bg = (d >> 5) & 31, bb = (d >> 10) & 31;
  const int fr = c & 31, fg = (c >> 5) & 31, fb = (c >> 10) & 31;
  int r, g, b;
  switch ((page >> 5) & 3) {
    case 0: r = (br + fr) >> 1; g = (bg + fg) >> 1; b = (bb + fb) >> 1; break;
    case 1: r = std::min(br + fr, 31); g = std::min(bg + fg, 31); b = std::min(bb + fb, 31); break;
    case 2: r = std::max(br - fr, 0); g = std::max(bg - fg, 0); b = std::max(bb - fb, 0); break;
    default: r = std::min(br + (fr >> 2), 31); g = std::min(bg + (fg >> 2), 31); b = std::min(bb + (fb >> 2), 31); break;
  }
  return uint16_t((c & 0x8000) | r | (g << 5) | (b << 10));
}

inline int64_t xfp(int x) { return (int64_t(x) << 32) + ((int64_t(1) << 32) - (int64_t(1) << 11)); }
inline int64_t xstep(int dx, int dy) {
  int64_t d = int64_t(dx) * (int64_t(1) << 32);
  if (d < 0) d -= dy - 1;
  if (d > 0) d += dy - 1;
  return d / dy;
}
}  // namespace

Gpu::Gpu(System& s) : sys_(s), vram_(VRAM_W * VRAM_H, 0), out_(640 * 576, 0xFF000000u) {}

void Gpu::reset() {
  std::fill(vram_.begin(), vram_.end(), 0);
  std::fill(hi_.begin(), hi_.end(), 0);
  fifoLen_ = need_ = 0;
  mode_ = CMD;
  tx_ = ty_ = tw_ = th_ = tcx_ = tcy_ = 0;
  readLatch_ = 0;
  texPage_ = 0; rectFlipX_ = rectFlipY_ = false; texWindow_ = 0;
  drawX1_ = drawY1_ = drawX2_ = drawY2_ = 0;
  offX_ = offY_ = 0;
  setMask_ = checkMask_ = false;
  areaTL_ = areaBR_ = offsetReg_ = 0;
  displayOff_ = true; irq_ = false; dmaDir_ = 0;
  dispX_ = dispY_ = 0;
  hStart_ = 0x200; hEnd_ = 0xC00; vStart_ = 0x10; vEnd_ = 0x100;
  dispMode_ = pal_ ? 8 : 0;
  lastInfo_ = 0;
  texDisableAllowed_ = false;
  lineStart_ = sys_.now << 16;
  busyUntil_ = 0; work_ = 0; backlog_ = false;
  std::memset(clutCache_, 0, sizeof clutCache_); clutKey_ = 0xFFFFFFFF; clutCount_ = 0;
  line_ = 0;
  inVblank_ = false; field_ = false; frameVblankSeen_ = false;
  plColor_ = plPrev_ = plPrevC_ = 0; plShaded_ = plSemi_ = plWantColor_ = false;
  outW_ = 320; outH_ = 240;
  std::fill(out_.begin(), out_.end(), 0xFF000000u);
}

int Gpu::dotDivider() const {
  if (dispMode_ & 0x40) return 7;
  static const int d[4] = {10, 8, 5, 4};
  return d[dispMode_ & 3];
}

uint64_t Gpu::lineLen16() const {
  // 3413 (NTSC) / 3406 (PAL) ciclos de vídeo por linha, convertidos para ciclos da CPU * 65536
  const bool pal = (dispMode_ & 8) != 0;
  return pal ? uint64_t(3406.0 * 65536.0 * 33868800.0 / 53203425.0) : uint64_t(3413.0 * 65536.0 * 33868800.0 / 53693175.0);
}

uint64_t Gpu::nextEvent() const { return ((lineStart_ + lineLen16()) >> 16) + 1; }

void Gpu::sync() {
  const uint64_t now16 = sys_.now << 16;
  const uint64_t len = lineLen16();
  while (lineStart_ + len <= now16) {
    lineStart_ += len;
    endLine();
  }
}

void Gpu::endLine() {
  line_++;
  sys_.timers.hblank();
  const int total = (dispMode_ & 8) ? 314 : 263;
  if (line_ >= total) {
    line_ = 0;
    if (!frameVblankSeen_) {  // faixa vertical sem vblank: o quadro termina na virada mesmo assim
      renderFrame();
      sys_.frameDone = true;
    }
    frameVblankSeen_ = false;
  }
  const bool vb = !(line_ >= vStart_ && line_ < vEnd_);
  if (vb != inVblank_) {
    inVblank_ = vb;
    sys_.timers.vblank(vb);
    if (vb) {
      sys_.raiseIrq(IRQ_VBLANK);
      if (interlaced480()) field_ = !field_; else field_ = false;
      renderFrame();
      sys_.frameDone = true;
      frameVblankSeen_ = true;
    }
  }
}

uint32_t Gpu::status() {
  uint32_t s = texPage_ & 0x7FF;
  s |= uint32_t(setMask_) << 11 | uint32_t(checkMask_) << 12;
  s |= uint32_t(!interlaced480() || field_) << 13;
  s |= ((dispMode_ >> 7) & 1) << 14;
  s |= ((texPage_ >> 11) & 1) << 15;
  s |= ((dispMode_ >> 6) & 1) << 16;
  s |= (dispMode_ & 3) << 17;
  s |= ((dispMode_ >> 2) & 1) << 19;
  s |= ((dispMode_ >> 3) & 1) << 20;
  s |= ((dispMode_ >> 4) & 1) << 21;
  s |= ((dispMode_ >> 5) & 1) << 22;
  s |= uint32_t(displayOff_) << 23;
  s |= uint32_t(irq_) << 24;
  const bool busy = sys_.now < busyUntil_;
  const bool readyCmd = !busy && (mode_ == CMD || mode_ == POLYLINE);
  const bool readyVram = mode_ == FROM_VRAM;
  if (!busy) backlog_ = false;
  const bool readyDma = !(busy && backlog_);  // fila cheia só quando chegou mais coisa durante o desenho
  s |= uint32_t(readyCmd) << 26 | uint32_t(readyVram) << 27 | uint32_t(readyDma) << 28;
  s |= dmaDir_ << 29;
  uint32_t req = 0;
  switch (dmaDir_) {
    case 1: req = 1; break;
    case 2: req = readyDma; break;
    case 3: req = readyVram; break;
    default: break;
  }
  s |= req << 25;
  bool odd;
  if (interlaced480()) odd = field_;
  else odd = (line_ & 1) != 0;
  if (inVblank_) odd = false;
  s |= uint32_t(odd) << 31;
  return s;
}

uint32_t Gpu::readData() {
  if (mode_ != FROM_VRAM) return readLatch_;
  uint32_t v = 0;
  for (int k = 0; k < 2; k++) {
    const uint16_t px = vram_[size_t(((ty_ + tcy_) & 511) * VRAM_W + ((tx_ + tcx_) & 1023))];
    v |= uint32_t(px) << (16 * k);
    if (++tcx_ >= tw_) {
      tcx_ = 0;
      if (++tcy_ >= th_) { mode_ = CMD; break; }
    }
  }
  readLatch_ = v;
  return v;
}

int Gpu::commandLength(uint32_t w) const {
  const uint32_t op = w >> 24;
  switch (op >> 5) {
    case 1: {  // polígono
      const int n = (op & 8) ? 4 : 3;
      return 1 + n * (1 + ((op & 4) ? 1 : 0)) + ((op & 0x10) ? n - 1 : 0);
    }
    case 2: return (op & 0x10) ? 4 : 3;  // linha (polilinha: os primeiros dois pontos)
    case 3: return 2 + ((op & 4) ? 1 : 0) + (((op >> 3) & 3) == 0 ? 1 : 0);
    case 4: return 4;
    case 5: case 6: return 3;
    default: return op == 0x02 ? 3 : 1;
  }
}

void Gpu::gp0(uint32_t v) {
  if (mode_ == TO_VRAM) {
    writeVramWord(uint16_t(v));
    if (mode_ == TO_VRAM) writeVramWord(uint16_t(v >> 16));
    return;
  }
  if (mode_ == POLYLINE) {
    if ((v & 0xF000F000u) == 0x50005000u) { mode_ = CMD; return; }
    if (plShaded_ && plWantColor_) { plColor_ = v & 0xFFFFFF; plWantColor_ = false; return; }
    const uint32_t c = plShaded_ ? plColor_ : plPrevC_;
    drawLine(sx11(plPrev_) + offX_, sx11(plPrev_ >> 16) + offY_, plPrevC_, sx11(v) + offX_, sx11(v >> 16) + offY_, c,
             plShaded_, plSemi_);
    plPrev_ = v; plPrevC_ = c;
    plWantColor_ = plShaded_;
    return;
  }
  if (sys_.now < busyUntil_) backlog_ = true;
  if (fifoLen_ == 0) need_ = commandLength(v);
  fifo_[fifoLen_++] = v;
  if (fifoLen_ < need_) return;
  fifoLen_ = 0;
  work_ = 0;
  command(fifo_);
  if (work_) {
    // tempo de desenho estimado (pontos x custo); enquanto isso o GPUSTAT diz "ocupada"
    busyUntil_ = std::max(busyUntil_, sys_.now) + work_ / 16;
    // uma fila de desenho maior que meio segundo só vem de lixo (ou de uma lista sem fim lida de uma vez): o
    // limite é o mesmo que a leitura do estado salvo aceita
    busyUntil_ = std::min(busyUntil_, sys_.now + 0xFFFFFF);
    work_ = 0;
  }
}

void Gpu::writeVramWord(uint16_t v) {
  const int x = (tx_ + tcx_) & 1023, y = (ty_ + tcy_) & 511;
  uint16_t& d = vram_[size_t(y * VRAM_W + x)];
  if (!(checkMask_ && (d & 0x8000))) {
    d = v | (setMask_ ? 0x8000 : 0);
    if (sh_) setBlock(x, y, d);
  }
  if (++tcx_ >= tw_) {
    tcx_ = 0;
    if (++tcy_ >= th_) mode_ = CMD;
  }
}

void Gpu::command(uint32_t* p) {
  const uint32_t op = p[0] >> 24;
  switch (op >> 5) {
    case 0:
      if (op == 0x01) clutCount_ = 0;  // limpa a cache (também a da paleta, confere no console)
      else if (op == 0x02) fill(p);
      else if (op == 0x1F) { if (!irq_) { irq_ = true; sys_.raiseIrq(IRQ_GPU); } }
      return;
    case 1: {  // polígonos
      const bool shaded = op & 0x10, quad = op & 8, textured = op & 4, semi = op & 2, raw = op & 1;
      const int n = quad ? 4 : 3;
      Vtx v[4];
      int idx = 0;
      uint16_t clut = 0;
      uint32_t page = texPage_;
      for (int i = 0; i < n; i++) {
        v[i].c = (i == 0 || !shaded) ? (p[0] & 0xFFFFFF) : (p[idx] & 0xFFFFFF);
        if (i == 0 || shaded) idx++;
        const uint32_t xy = p[idx++];
        v[i].x = sx11(xy) + offX_;
        v[i].y = sx11(xy >> 16) + offY_;
        v[i].u = v[i].v = 0;
        if (textured) {
          const uint32_t uv = p[idx++];
          v[i].u = int(uv & 0xFF);
          v[i].v = int((uv >> 8) & 0xFF);
          if (i == 0) clut = uint16_t(uv >> 16);
          if (i == 1) {
            const uint32_t pg = uv >> 16;
            texPage_ = (texPage_ & ~0x9FFu) | (pg & 0x1FF) | (texDisableAllowed_ ? (pg & 0x800) : 0);
            page = texPage_;
          }
        }
      }
      const bool tex = textured && !(texDisableAllowed_ && (page & 0x800));
      drawTriangle(v, shaded, tex, raw, semi, clut, page);
      if (quad) drawTriangle(v + 1, shaded, tex, raw, semi, clut, page);
      return;
    }
    case 2: {  // linhas
      const bool shaded = op & 0x10, poly = op & 8, semi = op & 2;
      const uint32_t c0 = p[0] & 0xFFFFFF;
      const uint32_t v0 = p[1];
      const uint32_t c1 = shaded ? p[2] & 0xFFFFFF : c0;
      const uint32_t v1 = shaded ? p[3] : p[2];
      drawLine(sx11(v0) + offX_, sx11(v0 >> 16) + offY_, c0, sx11(v1) + offX_, sx11(v1 >> 16) + offY_, c1, shaded, semi);
      if (poly) {
        mode_ = POLYLINE;
        plShaded_ = shaded; plSemi_ = semi;
        plPrev_ = v1; plPrevC_ = c1; plColor_ = c1;
        plWantColor_ = shaded;
      }
      return;
    }
    case 3: {  // retângulos
      const bool textured = op & 4, semi = op & 2, raw = op & 1;
      const int size = (op >> 3) & 3;
      const uint32_t c = p[0] & 0xFFFFFF;
      const int x = sx11(p[1]) + offX_, y = sx11(p[1] >> 16) + offY_;
      int idx = 2, u = 0, vv = 0;
      uint16_t clut = 0;
      if (textured) { u = int(p[2] & 0xFF); vv = int((p[2] >> 8) & 0xFF); clut = uint16_t(p[2] >> 16); idx = 3; }
      int w, h;
      if (size == 0) { w = int(p[idx] & 0x3FF); h = int((p[idx] >> 16) & 0x1FF); }
      else { w = h = size == 1 ? 1 : size == 2 ? 8 : 16; }
      const bool tex = textured && !(texDisableAllowed_ && (texPage_ & 0x800));
      drawRect(x, y, w, h, c, tex, raw, semi, u, vv, clut);
      return;
    }
    case 4: copyVram(p); return;
    case 5: {  // CPU -> VRAM
      tx_ = int(p[1] & 0x3FF); ty_ = int((p[1] >> 16) & 0x1FF);
      tw_ = int(((p[2] & 0xFFFF) - 1) & 0x3FF) + 1; th_ = int((((p[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
      tcx_ = tcy_ = 0;
      mode_ = TO_VRAM;
      return;
    }
    case 6: {  // VRAM -> CPU
      tx_ = int(p[1] & 0x3FF); ty_ = int((p[1] >> 16) & 0x1FF);
      tw_ = int(((p[2] & 0xFFFF) - 1) & 0x3FF) + 1; th_ = int((((p[2] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
      tcx_ = tcy_ = 0;
      mode_ = FROM_VRAM;
      return;
    }
    default: break;
  }
  const uint32_t v = p[0];
  switch (op) {
    case 0xE1:
      texPage_ = (v & 0x7FF) | (texDisableAllowed_ ? (v & 0x800) : 0);
      rectFlipX_ = (v >> 12) & 1; rectFlipY_ = (v >> 13) & 1;
      break;
    case 0xE2: texWindow_ = v & 0xFFFFF; break;
    case 0xE3: areaTL_ = v & 0xFFFFF; drawX1_ = int(v & 0x3FF); drawY1_ = int((v >> 10) & 0x1FF); break;
    case 0xE4: areaBR_ = v & 0xFFFFF; drawX2_ = int(v & 0x3FF); drawY2_ = int((v >> 10) & 0x1FF); break;
    case 0xE5: offsetReg_ = v & 0x3FFFFF; offX_ = sx11(v); offY_ = sx11(v >> 11); break;
    case 0xE6: setMask_ = v & 1; checkMask_ = (v >> 1) & 1; break;
    default: break;
  }
}

void Gpu::gp1(uint32_t v) {
  const uint32_t op = (v >> 24) & 0x3F;
  switch (op) {
    case 0x00: {
      fifoLen_ = 0; mode_ = CMD; irq_ = false; displayOff_ = true; dmaDir_ = 0;
      dispX_ = dispY_ = 0; hStart_ = 0x200; hEnd_ = 0xC00; vStart_ = 0x10; vEnd_ = 0x100;
      dispMode_ = 0;
      texPage_ = 0; rectFlipX_ = rectFlipY_ = false; texWindow_ = 0;
      drawX1_ = drawY1_ = drawX2_ = drawY2_ = 0; offX_ = offY_ = 0; areaTL_ = areaBR_ = offsetReg_ = 0;
      setMask_ = checkMask_ = false;
      break;
    }
    case 0x01: fifoLen_ = 0; mode_ = CMD; break;
    case 0x02: irq_ = false; break;
    case 0x03: displayOff_ = v & 1; break;
    case 0x04: dmaDir_ = v & 3; break;
    case 0x05: dispX_ = int(v & 0x3FE); dispY_ = int((v >> 10) & 0x1FF); break;
    case 0x06: hStart_ = int(v & 0xFFF); hEnd_ = int((v >> 12) & 0xFFF); break;
    case 0x07: vStart_ = int(v & 0x3FF); vEnd_ = int((v >> 10) & 0x3FF); break;
    case 0x08: dispMode_ = v & 0xFF; break;
    case 0x09: texDisableAllowed_ = v & 1; break;
    default:
      if (op >= 0x10 && op <= 0x1F) {
        switch (v & 7) {
          case 2: readLatch_ = texWindow_; break;
          case 3: readLatch_ = areaTL_; break;
          case 4: readLatch_ = areaBR_; break;
          case 5: readLatch_ = offsetReg_; break;
          case 7: readLatch_ = 2; break;  // versão da GPU
          default: break;
        }
      }
      break;
  }
}

// ---------------------------------------------------------------- desenho

bool Gpu::skipLine(int y) const {
  if (!interlaced480() || (texPage_ & 0x400)) return false;
  return ((y & 1) == ((dispY_ + (field_ ? 1 : 0)) & 1));
}

// cache da paleta (CLUT): a GPU copia a paleta antes de desenhar com textura de 4/8 bits e só a recarrega quando
// muda a posição da paleta ou quando precisa de mais cores (4 -> 8 bits); escrever na VRAM por cima não muda o que
// já está na cache (teste clut-cache do ps1-tests, conferido com a VRAM do console)
void Gpu::loadClut(uint16_t clut, uint32_t page) {
  const uint32_t depth = (page >> 7) & 3;
  if (depth >= 2) return;
  const int need = depth == 0 ? 16 : 256;
  if (clutKey_ == clut && clutCount_ >= need) return;
  const size_t row = size_t((clut >> 6) & 511) * VRAM_W;
  const int cx = (clut & 0x3F) * 16;
  for (int i = 0; i < need; i++) clutCache_[i] = vram_[row + size_t((cx + i) & 1023)];
  clutKey_ = clut;
  clutCount_ = need;
}

uint16_t Gpu::texel(int u, int v, uint16_t clut, uint32_t page) const {
  (void)clut;
  const int mx = int(texWindow_ & 0x1F), my = int((texWindow_ >> 5) & 0x1F);
  const int ox = int((texWindow_ >> 10) & 0x1F), oy = int((texWindow_ >> 15) & 0x1F);
  u = (u & ~(mx * 8)) | ((ox & mx) * 8);
  v = (v & ~(my * 8)) | ((oy & my) * 8);
  const int tx = int(page & 15) * 64, ty = int((page >> 4) & 1) * 256;
  const size_t row = size_t((ty + v) & 511) * VRAM_W;
  switch ((page >> 7) & 3) {
    case 0: {
      const uint16_t w = vram_[row + size_t((tx + (u >> 2)) & 1023)];
      return clutCache_[(w >> ((u & 3) * 4)) & 15];
    }
    case 1: {
      const uint16_t w = vram_[row + size_t((tx + (u >> 1)) & 1023)];
      return clutCache_[(w >> ((u & 1) * 8)) & 0xFF];
    }
    default: return vram_[row + size_t((tx + u) & 1023)];
  }
}

void Gpu::plot(int x, int y, uint16_t c, bool semi, uint32_t page) {
  uint16_t& d = vram_[size_t(y * VRAM_W + x)];
  if (checkMask_ && (d & 0x8000)) return;
  if (semi) c = blend(d, c, page);
  d = c | (setMask_ ? 0x8000 : 0);
}

// ---------------------------------------------------------------- VRAM ampliada (resolução interna 2x / 4x)

void Gpu::plotHi(int x, int y, uint16_t c, bool semi, uint32_t page) {
  uint16_t& d = hi_[size_t(y) * size_t(VRAM_W << sh_) + size_t(x)];
  if (checkMask_ && (d & 0x8000)) return;
  if (semi) c = blend(d, c, page);
  d = c | (setMask_ ? 0x8000 : 0);
}

void Gpu::plotBlock(int x, int y, uint16_t c, bool semi, uint32_t page) {
  const int n = 1 << sh_;
  for (int j = 0; j < n; j++)
    for (int i = 0; i < n; i++) plotHi((x << sh_) + i, (y << sh_) + j, c, semi, page);
}

void Gpu::setBlock(int x, int y, uint16_t v) {
  const int n = 1 << sh_;
  const size_t w = size_t(VRAM_W << sh_);
  for (int j = 0; j < n; j++) {
    uint16_t* d = hi_.data() + size_t((y << sh_) + j) * w + size_t(x << sh_);
    for (int i = 0; i < n; i++) d[i] = v;
  }
}

void Gpu::rebuildHi() {
  if (!sh_) return;
  for (int y = 0; y < VRAM_H; y++)
    for (int x = 0; x < VRAM_W; x++) setBlock(x, y, vram_[size_t(y * VRAM_W + x)]);
}

void Gpu::setScale(int n) {
  const int sh = n >= 4 ? 2 : n >= 2 ? 1 : 0;
  if (sh == sh_ && (sh == 0 || !hi_.empty())) return;
  sh_ = sh;
  if (sh_) hi_.assign(size_t(VRAM_W << sh_) * size_t(VRAM_H << sh_), 0);
  else std::vector<uint16_t>().swap(hi_);
  rebuildHi();
  renderFrame();
}

// o mesmo triângulo (vértices já em ordem de y) desenhado na VRAM ampliada: vértices multiplicados pela escala,
// mesma regra de preenchimento, cores e coordenadas de textura interpoladas em cada ponto ampliado (com 32 bits
// de fração, para não acumular erro nas linhas de 4096 pontos); as texturas vêm da VRAM de verdade
template <int SH>
void Gpu::rasterTriangle(const Vtx* in, bool shaded, bool textured, bool raw, bool semi, uint16_t clut, uint32_t page) {
  constexpr size_t W = size_t(VRAM_W) << SH;
  Vtx v[3] = {in[0], in[1], in[2]};
  for (Vtx& p : v) { p.x *= 1 << SH; p.y *= 1 << SH; }
  const int64_t denom = int64_t(v[1].x - v[0].x) * (v[2].y - v[1].y) - int64_t(v[2].x - v[1].x) * (v[1].y - v[0].y);
  if (denom == 0) return;
  auto gx = [&](int a0, int a1, int a2) {
    const int64_t n = int64_t(a1 - a0) * (v[2].y - v[1].y) - int64_t(a2 - a1) * (v[1].y - v[0].y);
    return n * (int64_t(1) << 32) / denom;
  };
  auto gy = [&](int a0, int a1, int a2) {
    const int64_t n = int64_t(v[1].x - v[0].x) * (a2 - a1) - int64_t(v[2].x - v[1].x) * (a1 - a0);
    return n * (int64_t(1) << 32) / denom;
  };
  int cv = 0;
  if (v[1].x <= v[0].x) cv = v[2].x <= v[1].x ? 2 : 1;
  else if (v[2].x < v[0].x) cv = 2;
  enum { R, G, B, U, V, N };
  int att[3][N];
  for (int i = 0; i < 3; i++) {
    att[i][R] = int(v[i].c & 0xFF); att[i][G] = int((v[i].c >> 8) & 0xFF); att[i][B] = int((v[i].c >> 16) & 0xFF);
    att[i][U] = v[i].u; att[i][V] = v[i].v;
  }
  int64_t dx[N], dy[N], base[N];
  for (int k = 0; k < N; k++) {
    dx[k] = gx(att[0][k], att[1][k], att[2][k]);
    dy[k] = gy(att[0][k], att[1][k], att[2][k]);
    base[k] = (int64_t(att[cv][k]) << 32) + (int64_t(1) << 31) - dx[k] * v[cv].x - dy[k] * v[cv].y;
  }
  if (!shaded) { for (int k = R; k <= B; k++) { dx[k] = dy[k] = 0; base[k] = int64_t(att[0][k]) << 32; } }
  const bool dither = (texPage_ & 0x200) && (shaded || (textured && !raw));
  const bool modulate = textured && !raw;

  const int64_t baseStep = xstep(v[2].x - v[0].x, v[2].y - v[0].y);
  int64_t upperStep;
  bool rightFacing;
  if (v[1].y == v[0].y) { upperStep = 0; rightFacing = v[1].x > v[0].x; }
  else { upperStep = xstep(v[1].x - v[0].x, v[1].y - v[0].y); rightFacing = upperStep > baseStep; }
  const int64_t lowerStep = v[2].y == v[1].y ? 0 : xstep(v[2].x - v[1].x, v[2].y - v[1].y);
  struct Part { int y0, y1; int64_t xc[2], xs[2]; } parts[2];
  parts[0].y0 = v[0].y; parts[0].y1 = v[1].y;
  parts[0].xc[rightFacing] = xfp(v[0].x); parts[0].xs[rightFacing] = upperStep;
  parts[0].xc[!rightFacing] = xfp(v[0].x); parts[0].xs[!rightFacing] = baseStep;
  parts[1].y0 = v[1].y; parts[1].y1 = v[2].y;
  parts[1].xc[rightFacing] = xfp(v[1].x); parts[1].xs[rightFacing] = lowerStep;
  parts[1].xc[!rightFacing] = xfp(v[0].x) + baseStep * (v[1].y - v[0].y);
  parts[1].xs[!rightFacing] = baseStep;

  const int cx1 = drawX1_ << SH, cx2 = (drawX2_ + 1) << SH, cy1 = drawY1_ << SH, cy2 = ((drawY2_ + 1) << SH) - 1;
  for (const Part& pt : parts) {
    int64_t lc = pt.xc[0], rc = pt.xc[1];
    for (int y = pt.y0; y < pt.y1; y++, lc += pt.xs[0], rc += pt.xs[1]) {
      if (y < cy1) continue;
      if (y > cy2) break;
      if (skipLine(y >> SH)) continue;
      int x0 = int(lc >> 32), x1 = int(rc >> 32);
      if (x0 < cx1) x0 = cx1;
      if (x1 > cx2) x1 = cx2;
      if (x0 >= x1) continue;
      int64_t a[N];
      for (int k = 0; k < N; k++) a[k] = base[k] + dx[k] * x0 + dy[k] * y;
      uint16_t* row = hi_.data() + size_t(y) * W;
      const int8_t* drow = DITHER[(y >> SH) & 3];
      for (int x = x0; x < x1; x++) {
        const int d = dither ? drow[(x >> SH) & 3] : 0;
        uint16_t c;
        bool sm = semi;
        if (textured) {
          const uint16_t t = texel(int((a[U] >> 32) & 0xFF), int((a[V] >> 32) & 0xFF), clut, page);
          if (t == 0) goto next;
          if (modulate) {
            const int r8 = int((a[R] >> 32) & 0xFF), g8 = int((a[G] >> 32) & 0xFF), b8 = int((a[B] >> 32) & 0xFF);
            c = uint16_t(to5(((t & 31) * r8) >> 4, d) | to5((((t >> 5) & 31) * g8) >> 4, d) << 5 |
                         to5((((t >> 10) & 31) * b8) >> 4, d) << 10 | (t & 0x8000));
          } else {
            c = t;
          }
          sm = semi && (t & 0x8000);
        } else {
          c = uint16_t(to5(int((a[R] >> 32) & 0xFF), d) | to5(int((a[G] >> 32) & 0xFF), d) << 5 | to5(int((a[B] >> 32) & 0xFF), d) << 10);
        }
        {
          uint16_t& dst = row[x];
          if (!(checkMask_ && (dst & 0x8000))) dst = (sm ? blend(dst, c, page) : c) | (setMask_ ? 0x8000 : 0);
        }
      next:
        for (int k = 0; k < N; k++) a[k] += dx[k];
      }
    }
  }
}

void Gpu::drawTriangle(const Vtx* in, bool shaded, bool textured, bool raw, bool semi, uint16_t clut, uint32_t page) {
  Vtx v[3] = {in[0], in[1], in[2]};
  if (v[1].y < v[0].y) std::swap(v[0], v[1]);
  if (v[2].y < v[1].y) std::swap(v[1], v[2]);
  if (v[1].y < v[0].y) std::swap(v[0], v[1]);
  if (v[0].y == v[2].y) return;
  for (int i = 0; i < 3; i++) {
    const Vtx& a = v[i];
    const Vtx& b = v[(i + 1) % 3];
    if (std::abs(a.x - b.x) >= 1024 || std::abs(a.y - b.y) >= 512) return;
  }
  const int64_t denom = int64_t(v[1].x - v[0].x) * (v[2].y - v[1].y) - int64_t(v[2].x - v[1].x) * (v[1].y - v[0].y);
  if (denom == 0) return;

  // gradientes (12 bits de fração, deslocados para os 8 bits inteiros ficarem no topo de 32 bits)
  auto gx = [&](int a0, int a1, int a2) {
    const int64_t n = int64_t(a1 - a0) * (v[2].y - v[1].y) - int64_t(a2 - a1) * (v[1].y - v[0].y);
    return uint32_t(int32_t(n * 4096 / denom)) << 12;
  };
  auto gy = [&](int a0, int a1, int a2) {
    const int64_t n = int64_t(v[1].x - v[0].x) * (a2 - a1) - int64_t(v[2].x - v[1].x) * (a1 - a0);
    return uint32_t(int32_t(n * 4096 / denom)) << 12;
  };
  int cv = 0;
  if (v[1].x <= v[0].x) cv = v[2].x <= v[1].x ? 2 : 1;
  else if (v[2].x < v[0].x) cv = 2;

  enum { R, G, B, U, V, N };
  int att[3][N];
  for (int i = 0; i < 3; i++) {
    att[i][R] = int(v[i].c & 0xFF); att[i][G] = int((v[i].c >> 8) & 0xFF); att[i][B] = int((v[i].c >> 16) & 0xFF);
    att[i][U] = v[i].u; att[i][V] = v[i].v;
  }
  uint32_t dx[N], dy[N], base[N];
  for (int k = 0; k < N; k++) {
    dx[k] = gx(att[0][k], att[1][k], att[2][k]);
    dy[k] = gy(att[0][k], att[1][k], att[2][k]);
    base[k] = ((uint32_t(att[cv][k]) << 12) + 2048) << 12;
    base[k] -= dx[k] * uint32_t(v[cv].x) + dy[k] * uint32_t(v[cv].y);
  }
  if (!shaded) { for (int k = R; k <= B; k++) { dx[k] = dy[k] = 0; base[k] = uint32_t(att[0][k]) << 24; } }

  if (textured) loadClut(clut, page);
  // na VRAM ampliada primeiro: as texturas lidas são as de antes deste triângulo, como no desenho de verdade
  if (sh_ == 1) rasterTriangle<1>(v, shaded, textured, raw, semi, clut, page);
  else if (sh_ == 2) rasterTriangle<2>(v, shaded, textured, raw, semi, clut, page);
  const bool dither = (texPage_ & 0x200) && (shaded || (textured && !raw));
  const bool modulate = textured && !raw;
  const uint32_t flatColor = v[0].c;
  (void)flatColor;

  // bordas
  const int64_t baseStep = xstep(v[2].x - v[0].x, v[2].y - v[0].y);
  int64_t upperStep;
  bool rightFacing;
  if (v[1].y == v[0].y) { upperStep = 0; rightFacing = v[1].x > v[0].x; }
  else { upperStep = xstep(v[1].x - v[0].x, v[1].y - v[0].y); rightFacing = upperStep > baseStep; }
  const int64_t lowerStep = v[2].y == v[1].y ? 0 : xstep(v[2].x - v[1].x, v[2].y - v[1].y);

  struct Part { int y0, y1; int64_t xc[2], xs[2]; } parts[2];
  parts[0].y0 = v[0].y; parts[0].y1 = v[1].y;
  parts[0].xc[rightFacing] = xfp(v[0].x); parts[0].xs[rightFacing] = upperStep;
  parts[0].xc[!rightFacing] = xfp(v[0].x); parts[0].xs[!rightFacing] = baseStep;
  parts[1].y0 = v[1].y; parts[1].y1 = v[2].y;
  parts[1].xc[rightFacing] = xfp(v[1].x); parts[1].xs[rightFacing] = lowerStep;
  parts[1].xc[!rightFacing] = xfp(v[0].x) + baseStep * (v[1].y - v[0].y);
  parts[1].xs[!rightFacing] = baseStep;

  for (const Part& pt : parts) {
    int64_t lc = pt.xc[0], rc = pt.xc[1];
    for (int y = pt.y0; y < pt.y1; y++, lc += pt.xs[0], rc += pt.xs[1]) {
      if (y < drawY1_) continue;
      if (y > drawY2_) break;
      if (skipLine(y)) continue;
      int x0 = int(lc >> 32), x1 = int(rc >> 32);  // [x0, x1)
      if (x0 < drawX1_) x0 = drawX1_;
      if (x1 > drawX2_ + 1) x1 = drawX2_ + 1;
      if (x0 >= x1) continue;
      work_ += uint32_t(x1 - x0) * (textured ? 46u : semi ? 13u : 9u);
      uint32_t a[N];
      for (int k = 0; k < N; k++) a[k] = base[k] + dx[k] * uint32_t(x0) + dy[k] * uint32_t(y);
      for (int x = x0; x < x1; x++) {
        const int d = dither ? DITHER[y & 3][x & 3] : 0;
        uint16_t c;
        if (textured) {
          const uint16_t t = texel(int(a[U] >> 24), int(a[V] >> 24), clut, page);
          if (t == 0) goto next;
          if (modulate) {
            const int r8 = int(a[R] >> 24), g8 = int(a[G] >> 24), b8 = int(a[B] >> 24);
            c = uint16_t(to5(((t & 31) * r8) >> 4, d) | to5((((t >> 5) & 31) * g8) >> 4, d) << 5 |
                         to5((((t >> 10) & 31) * b8) >> 4, d) << 10 | (t & 0x8000));
          } else {
            c = t;
          }
          plot(x, y, c, semi && (t & 0x8000), page);
        } else {
          c = uint16_t(to5(int(a[R] >> 24), d) | to5(int(a[G] >> 24), d) << 5 | to5(int(a[B] >> 24), d) << 10);
          plot(x, y, c, semi, page);
        }
      next:
        for (int k = 0; k < N; k++) a[k] += dx[k];
      }
    }
  }
}

void Gpu::drawRect(int x, int y, int w, int h, uint32_t c, bool textured, bool raw, bool semi, int u0, int v0, uint16_t clut) {
  if (w <= 0 || h <= 0) return;
  const int xs = std::max(x, drawX1_), xe = std::min(x + w - 1, drawX2_);
  const int ys = std::max(y, drawY1_), ye = std::min(y + h - 1, drawY2_);
  const int r8 = int(c & 0xFF), g8 = int((c >> 8) & 0xFF), b8 = int((c >> 16) & 0xFF);
  const uint16_t flat = uint16_t((r8 >> 3) | ((g8 >> 3) << 5) | ((b8 >> 3) << 10));
  if (textured) loadClut(clut, texPage_);
  if (xe >= xs && ye >= ys) work_ += uint32_t((xe - xs + 1) * (ye - ys + 1)) * (textured ? 18u : semi ? 13u : 9u);
  for (int yy = ys; yy <= ye; yy++) {
    if (skipLine(yy)) continue;
    const int dv = yy - y;
    const int tv = (rectFlipY_ ? v0 - dv : v0 + dv) & 0xFF;
    for (int xx = xs; xx <= xe; xx++) {
      if (!textured) { if (sh_) plotBlock(xx, yy, flat, semi, texPage_); plot(xx, yy, flat, semi, texPage_); continue; }
      const int du = xx - x;
      const int tu = (rectFlipX_ ? u0 + 1 - du : u0 + du) & 0xFF;  // espelhado em X: começa em u0+1 (confere no console)
      const uint16_t t = texel(tu, tv, clut, texPage_);
      if (t == 0) continue;
      uint16_t o = t;
      if (!raw) {
        o = uint16_t(std::min<uint32_t>(((t & 31) * uint32_t(r8)) >> 7, 31) |
                     std::min<uint32_t>((((t >> 5) & 31) * uint32_t(g8)) >> 7, 31) << 5 |
                     std::min<uint32_t>((((t >> 10) & 31) * uint32_t(b8)) >> 7, 31) << 10 | (t & 0x8000));
      }
      if (sh_) plotBlock(xx, yy, o, semi && (t & 0x8000), texPage_);
      plot(xx, yy, o, semi && (t & 0x8000), texPage_);
    }
  }
}

void Gpu::drawLine(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool shaded, bool semi) {
  const int adx = std::abs(x1 - x0), ady = std::abs(y1 - y0);
  if (adx >= 1024 || ady >= 512) return;
  if (x0 >= x1 && std::max(adx, ady) > 0) { std::swap(x0, x1); std::swap(y0, y1); std::swap(c0, c1); }
  const int k = std::max(adx, ady);
  auto div = [&](int64_t delta) -> int64_t {
    int64_t d = delta * (int64_t(1) << 32);
    if (d < 0) d -= k - 1;
    if (d > 0) d += k - 1;
    return d / k;
  };
  int64_t dxk = 0, dyk = 0;
  int32_t drk = 0, dgk = 0, dbk = 0;
  const int r0 = int(c0 & 0xFF), g0 = int((c0 >> 8) & 0xFF), b0 = int((c0 >> 16) & 0xFF);
  if (k) {
    dxk = div(x1 - x0); dyk = div(y1 - y0);
    if (shaded) {
      drk = int32_t(uint32_t(int(c1 & 0xFF) - r0) << 12) / k;
      dgk = int32_t(uint32_t(int((c1 >> 8) & 0xFF) - g0) << 12) / k;
      dbk = int32_t(uint32_t(int((c1 >> 16) & 0xFF) - b0) << 12) / k;
    }
  }
  int64_t cx = (int64_t(x0) << 32) | (int64_t(1) << 31);
  int64_t cy = (int64_t(y0) << 32) | (int64_t(1) << 31);
  cx -= 1024;
  if (dyk < 0) cy -= 1024;
  int32_t cr = (r0 << 12) | (1 << 11), cg = (g0 << 12) | (1 << 11), cb = (b0 << 12) | (1 << 11);
  const bool dither = (texPage_ & 0x200) != 0;  // linhas planas também são pontilhadas (confere no console)
  work_ += uint32_t(k + 1) * 16;
  for (int i = 0; i <= k; i++) {
    const int x = int(cx >> 32) & 2047, y = int(cy >> 32) & 2047;
    const int xs = sx11(uint32_t(x)), ys = sx11(uint32_t(y));
    if (xs >= drawX1_ && xs <= drawX2_ && ys >= drawY1_ && ys <= drawY2_ && !skipLine(ys)) {
      const int d = dither ? DITHER[ys & 3][xs & 3] : 0;
      const uint16_t c = uint16_t(to5(cr >> 12, d) | to5(cg >> 12, d) << 5 | to5(cb >> 12, d) << 10);
      if (sh_) plotBlock(xs, ys, c, semi, texPage_);
      plot(xs, ys, c, semi, texPage_);
    }
    cx += dxk; cy += dyk;
    cr += drk; cg += dgk; cb += dbk;
  }
}

void Gpu::fill(uint32_t* p) {
  const uint32_t c = p[0];
  const uint16_t v = uint16_t(((c & 0xFF) >> 3) | (((c >> 8) & 0xFF) >> 3) << 5 | (((c >> 16) & 0xFF) >> 3) << 10);
  const int x = int(p[1] & 0x3F0), y = int((p[1] >> 16) & 0x1FF);
  const int w = int(((p[2] & 0x3FF) + 15) & ~15u), h = int((p[2] >> 16) & 0x1FF);
  work_ += uint32_t(w * h) * 3 / 2;
  for (int j = 0; j < h; j++) {
    const int yy = (y + j) & 511;
    if (skipLine(yy)) continue;
    for (int i = 0; i < w; i++) vram_[size_t(yy * VRAM_W + ((x + i) & 1023))] = v;
    if (sh_) for (int i = 0; i < w; i++) setBlock((x + i) & 1023, yy, v);
  }
}

void Gpu::copyVram(uint32_t* p) {
  const int sx = int(p[1] & 0x3FF), sy = int((p[1] >> 16) & 0x1FF);
  const int dx = int(p[2] & 0x3FF), dy = int((p[2] >> 16) & 0x1FF);
  const int w = int(((p[3] & 0xFFFF) - 1) & 0x3FF) + 1, h = int((((p[3] >> 16) & 0xFFFF) - 1) & 0x1FF) + 1;
  uint16_t row[1024];
  work_ += uint32_t(w * h) * 22;
  for (int j = 0; j < h; j++) {
    const size_t sr = size_t((sy + j) & 511) * VRAM_W, dr = size_t((dy + j) & 511) * VRAM_W;
    for (int i = 0; i < w; i++) row[i] = vram_[sr + size_t((sx + i) & 1023)];
    for (int i = 0; i < w; i++) {
      uint16_t& d = vram_[dr + size_t((dx + i) & 1023)];
      if (checkMask_ && (d & 0x8000)) continue;
      d = row[i] | (setMask_ ? 0x8000 : 0);
    }
  }
  if (!sh_) return;
  // a mesma cópia na VRAM ampliada (mantém o que foi desenhado em alta resolução, como a troca de quadros)
  const int n = 1 << sh_, hw = VRAM_W << sh_, hh = VRAM_H << sh_;
  std::vector<uint16_t> hrow(size_t(w * n));
  for (int j = 0; j < h * n; j++) {
    const size_t sr = size_t(((sy << sh_) + j) & (hh - 1)) * size_t(hw), dr = size_t(((dy << sh_) + j) & (hh - 1)) * size_t(hw);
    for (int i = 0; i < w * n; i++) hrow[size_t(i)] = hi_[sr + size_t(((sx << sh_) + i) & (hw - 1))];
    for (int i = 0; i < w * n; i++) {
      uint16_t& d = hi_[dr + size_t(((dx << sh_) + i) & (hw - 1))];
      if (checkMask_ && (d & 0x8000)) continue;
      d = hrow[size_t(i)] | (setMask_ ? 0x8000 : 0);
    }
  }
}

// ---------------------------------------------------------------- saída de vídeo

void Gpu::renderFrame() {
  static const int HRES[4] = {256, 320, 512, 640};
  const int w = (dispMode_ & 0x40) ? 368 : HRES[dispMode_ & 3];
  const bool pal = (dispMode_ & 8) != 0;
  int lines = vEnd_ - vStart_;
  if (lines <= 0 || lines > (pal ? 288 : 240)) lines = pal ? 288 : 240;
  const int h = interlaced480() ? lines * 2 : lines;
  const bool rgb24 = (dispMode_ & 0x10) != 0;
  if (sh_) { renderFrameHi(w, h, rgb24); return; }
  outW_ = w; outH_ = h;
  if (out_.size() < size_t(w * h)) out_.resize(size_t(w * h));
  uint32_t* o = out_.data();
  if (displayOff_) { std::fill(o, o + w * h, 0xFF000000u); return; }
  for (int y = 0; y < h; y++) {
    const size_t row = size_t((dispY_ + y) & 511) * VRAM_W;
    uint32_t* d = o + size_t(y) * size_t(w);
    if (!rgb24) {
      for (int x = 0; x < w; x++) {
        const uint16_t p = vram_[row + size_t((dispX_ + x) & 1023)];
        d[x] = 0xFF000000u | c5to8(p & 31) | c5to8((p >> 5) & 31) << 8 | c5to8((p >> 10) & 31) << 16;
      }
    } else {
      const uint8_t* b = reinterpret_cast<const uint8_t*>(vram_.data() + row);
      for (int x = 0; x < w; x++) {
        const int a = dispX_ * 2 + x * 3;
        const uint32_t r = b[a & 2047], g = b[(a + 1) & 2047], bl = b[(a + 2) & 2047];
        d[x] = 0xFF000000u | r | g << 8 | bl << 16;
      }
    }
  }
}

// imagem ampliada: a área da tela tirada da VRAM ampliada; em 24 bits (vídeos do MDEC) a da VRAM de verdade,
// ampliada por repetição
void Gpu::renderFrameHi(int w, int h, bool rgb24) {
  const int n = 1 << sh_, W = w * n, H = h * n, hw = VRAM_W << sh_, hh = VRAM_H << sh_;
  outW_ = W; outH_ = H;
  if (out_.size() < size_t(W) * size_t(H)) out_.resize(size_t(W) * size_t(H));
  uint32_t* o = out_.data();
  if (displayOff_) { std::fill(o, o + size_t(W) * size_t(H), 0xFF000000u); return; }
  if (!rgb24) {
    for (int y = 0; y < H; y++) {
      const uint16_t* row = hi_.data() + size_t(((dispY_ << sh_) + y) & (hh - 1)) * size_t(hw);
      uint32_t* d = o + size_t(y) * size_t(W);
      const int x0 = dispX_ << sh_;
      for (int x = 0; x < W; x++) {
        const uint16_t p = row[(x0 + x) & (hw - 1)];
        d[x] = 0xFF000000u | c5to8(p & 31) | c5to8((p >> 5) & 31) << 8 | c5to8((p >> 10) & 31) << 16;
      }
    }
    return;
  }
  for (int y = 0; y < h; y++) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(vram_.data() + size_t((dispY_ + y) & 511) * VRAM_W);
    uint32_t* d = o + size_t(y * n) * size_t(W);
    for (int x = 0; x < w; x++) {
      const int a = dispX_ * 2 + x * 3;
      const uint32_t px = 0xFF000000u | b[a & 2047] | uint32_t(b[(a + 1) & 2047]) << 8 | uint32_t(b[(a + 2) & 2047]) << 16;
      for (int i = 0; i < n; i++) d[x * n + i] = px;
    }
    for (int j = 1; j < n; j++) std::memcpy(d + size_t(j) * size_t(W), d, size_t(W) * 4);
  }
}

void Gpu::sync(StateIO& io) {
  io.raw(vram_.data(), vram_.size() * 2);
  io.raw(fifo_, sizeof fifo_);
  io.sync(fifoLen_); io.sync(need_); io.sync(mode_);
  io.sync(tx_); io.sync(ty_); io.sync(tw_); io.sync(th_); io.sync(tcx_); io.sync(tcy_);
  io.sync(readLatch_);
  io.sync(texPage_); io.sync(rectFlipX_); io.sync(rectFlipY_); io.sync(texWindow_);
  io.sync(drawX1_); io.sync(drawY1_); io.sync(drawX2_); io.sync(drawY2_);
  io.sync(offX_); io.sync(offY_); io.sync(setMask_); io.sync(checkMask_);
  io.sync(areaTL_); io.sync(areaBR_); io.sync(offsetReg_);
  io.sync(displayOff_); io.sync(irq_); io.sync(dmaDir_);
  io.sync(dispX_); io.sync(dispY_); io.sync(hStart_); io.sync(hEnd_); io.sync(vStart_); io.sync(vEnd_);
  io.sync(dispMode_); io.sync(lastInfo_); io.sync(texDisableAllowed_);
  io.sync(lineStart_); io.sync(line_);
  io.sync(inVblank_); io.sync(field_); io.sync(frameVblankSeen_);
  io.sync(busyUntil_); io.sync(backlog_);
  io.raw(clutCache_, sizeof clutCache_); io.sync(clutKey_); io.sync(clutCount_);
  io.sync(plColor_); io.sync(plPrev_); io.sync(plPrevC_); io.sync(plShaded_); io.sync(plSemi_); io.sync(plWantColor_);
  if (io.reading()) {
    fifoLen_ = std::clamp(fifoLen_, 0, 15);
    need_ = std::clamp(need_, 0, 16);
    if (mode_ > FROM_VRAM) mode_ = CMD;
    tx_ &= 1023; ty_ &= 511;
    tw_ = std::clamp(tw_, 0, 1024); th_ = std::clamp(th_, 0, 512);
    tcx_ = std::clamp(tcx_, 0, tw_); tcy_ = std::clamp(tcy_, 0, th_);
    if ((mode_ == TO_VRAM || mode_ == FROM_VRAM) && (!tw_ || !th_)) mode_ = CMD;
    drawX1_ &= 1023; drawX2_ &= 1023; drawY1_ &= 511; drawY2_ &= 511;
    offX_ = sx11(uint32_t(offX_)); offY_ = sx11(uint32_t(offY_));
    dmaDir_ &= 3;
    dispX_ &= 0x3FE; dispY_ &= 0x1FF;
    hStart_ &= 0xFFF; hEnd_ &= 0xFFF; vStart_ &= 0x3FF; vEnd_ &= 0x3FF;
    dispMode_ &= 0xFF;
    texPage_ &= 0xFFF;
    texWindow_ &= 0xFFFFF;
    line_ = std::clamp(line_, 0, 313);
    const uint64_t n16 = sys_.now << 16;
    if (lineStart_ > n16) lineStart_ = n16;
    if (busyUntil_ > sys_.now + 0x1000000) busyUntil_ = sys_.now;
    clutCount_ = std::clamp(clutCount_, 0, 256);
    if (n16 - lineStart_ > lineLen16() * 2) lineStart_ = n16;
    rebuildHi();  // a VRAM ampliada não vai no estado: volta da VRAM de verdade
    renderFrame();
  }
}

}  // namespace indigo::ps1
