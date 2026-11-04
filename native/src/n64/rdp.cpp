// RDP em software. Formato dos comandos conforme a documentação pública do RDP (manual de comandos do SDK do
// N64 e n64brew.dev). Uma amostra por pixel: a borda dos triângulos é avaliada no centro de cada linha e cada
// pixel entra se o centro dele estiver dentro do vão.
#include "rdp.h"

#include <algorithm>
#include <cstring>

#include "n64.h"

namespace indigo::n64 {

namespace {
inline int32_t sx(uint64_t v, int bits) { return int32_t(int64_t(v << (64 - bits)) >> (64 - bits)); }
inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// z de 18 bits <-> 14 bits gravados (expoente de 3 bits + mantissa de 11)
constexpr uint32_t Z_BASE[8] = {0, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800};
constexpr int Z_SHIFT[8] = {6, 5, 4, 3, 2, 1, 0, 0};
inline uint16_t zComprime(uint32_t z) {
  // expoente = quantos bits 1 seguidos no alto do z (até 7)
  const uint32_t t = (~z >> 11) & 0x7F;
  const int e = t ? std::min(7, __builtin_clz(t) - 25) : 7;
  const uint32_t m = ((z - Z_BASE[e]) >> Z_SHIFT[e]) & 0x7FF;
  return uint16_t((e << 13) | (m << 2));
}
inline uint32_t zExpande(uint16_t v) {
  const int e = v >> 13;
  return Z_BASE[e] + (uint32_t((v >> 2) & 0x7FF) << Z_SHIFT[e]);
}
}  // namespace

void Rdp::reset() {
  other_ = combine_ = 0;
  fillColor_ = fogColor_ = blendColor_ = primColor_ = envColor_ = 0;
  primLodFrac_ = 0; primZ_ = primDz_ = 0;
  texAddr_ = colorAddr_ = zAddr_ = 0;
  texFmt_ = texSize_ = colorFmt_ = colorSize_ = 0;
  texWidth_ = colorWidth_ = 0;
  sxh_ = syh_ = 0; sxl_ = 320 << 2; syl_ = 240 << 2;
  for (auto& t : tiles_) t = Tile{};
  std::memset(tmem_, 0, sizeof tmem_);
  cmdLen_ = 0;
}

// --------------------------------------------------------------------------------------- RDRAM
uint8_t Rdp::rd8(uint32_t a) const { return sys.rdram[(a & 0x7FFFFF) ^ 3]; }
uint16_t Rdp::rd16(uint32_t a) const { uint16_t v; std::memcpy(&v, &sys.rdram[(a & 0x7FFFFE) ^ 2], 2); return v; }
uint32_t Rdp::rd32(uint32_t a) const { uint32_t v; std::memcpy(&v, &sys.rdram[a & 0x7FFFFC], 4); return v; }
void Rdp::wr8(uint32_t a, uint8_t v) { sys.rdram[(a & 0x7FFFFF) ^ 3] = v; }
void Rdp::wr16(uint32_t a, uint16_t v) { std::memcpy(&sys.rdram[(a & 0x7FFFFE) ^ 2], &v, 2); }
void Rdp::wr32(uint32_t a, uint32_t v) { std::memcpy(&sys.rdram[a & 0x7FFFFC], &v, 4); }

// --------------------------------------------------------------------------------------- lista de comandos
static int tamanho(uint64_t w0) {
  const int id = int(w0 >> 56) & 0x3F;
  if (id >= 0x08 && id <= 0x0F) return 4 + ((id & 4) ? 8 : 0) + ((id & 2) ? 8 : 0) + ((id & 1) ? 2 : 0);
  if (id == 0x24 || id == 0x25) return 2;
  return 1;
}

void Rdp::run() {
  uint32_t cur = sys.dpc[2], fim = sys.dpc[1];
  const bool xbus = sys.dpc[3] & 1;
  int limite = 1 << 20;
  const uint64_t inicio = pixels;
  while (cur < fim && limite-- > 0) {
    if (pixels - inicio > (32u << 20)) { cur = fim; cmdLen_ = 0; break; }  // lista absurda: o resto é ignorado
    uint64_t w;
    if (xbus) {
      uint32_t a, b;
      std::memcpy(&a, &sys.spmem[cur & 0xFF8], 4);
      std::memcpy(&b, &sys.spmem[(cur & 0xFF8) + 4], 4);
      w = (uint64_t(a) << 32) | b;
    } else {
      w = (uint64_t(rd32(cur & ~7u)) << 32) | rd32((cur & ~7u) + 4);
    }
    cur += 8;
    cmd_[cmdLen_++] = w;
    if (cmdLen_ >= tamanho(cmd_[0])) {
      command(cmd_);
      cmdLen_ = 0;
    }
  }
  sys.dpc[2] = cur;
}

void Rdp::command(const uint64_t* w) {
  const uint64_t w0 = w[0];
  const int id = int(w0 >> 56) & 0x3F;
  switch (id) {
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
      triangle(w, id & 4, id & 2, id & 1);
      break;
    case 0x24: texRect(w, false); break;
    case 0x25: texRect(w, true); break;
    case 0x26: case 0x27: case 0x28: break;  // syncs
    case 0x29: sys.raise(5); break;           // full sync: interrupção do DP
    case 0x2A: case 0x2B: case 0x2C: break;   // key GB/R, convert
    case 0x2D:
      sxh_ = int(w0 >> 44) & 0xFFF; syh_ = int(w0 >> 32) & 0xFFF;
      sxl_ = int(w0 >> 12) & 0xFFF; syl_ = int(w0) & 0xFFF;
      break;
    case 0x2E: primZ_ = uint16_t(w0 >> 16); primDz_ = uint16_t(w0); break;
    case 0x2F: other_ = w0; break;
    case 0x30: loadTlut(w); break;
    case 0x32: {
      Tile& t = tiles_[(w0 >> 24) & 7];
      t.sl = uint16_t((w0 >> 44) & 0xFFF); t.tl = uint16_t((w0 >> 32) & 0xFFF);
      t.sh = uint16_t((w0 >> 12) & 0xFFF); t.th = uint16_t(w0 & 0xFFF);
      break;
    }
    case 0x33: loadBlock(w); break;
    case 0x34: loadTile(w); break;
    case 0x35: {
      Tile& t = tiles_[(w0 >> 24) & 7];
      t.fmt = uint8_t((w0 >> 53) & 7); t.size = uint8_t((w0 >> 51) & 3);
      t.line = uint16_t((w0 >> 41) & 0x1FF); t.tmem = uint16_t((w0 >> 32) & 0x1FF);
      t.pal = uint8_t((w0 >> 20) & 15);
      t.ct = uint8_t((w0 >> 19) & 1); t.mt = uint8_t((w0 >> 18) & 1);
      t.maskT = uint8_t((w0 >> 14) & 15); t.shiftT = uint8_t((w0 >> 10) & 15);
      t.cs = uint8_t((w0 >> 9) & 1); t.ms = uint8_t((w0 >> 8) & 1);
      t.maskS = uint8_t((w0 >> 4) & 15); t.shiftS = uint8_t(w0 & 15);
      break;
    }
    case 0x36: fillRect(w); break;
    case 0x37: fillColor_ = uint32_t(w0); break;
    case 0x38: fogColor_ = uint32_t(w0); break;
    case 0x39: blendColor_ = uint32_t(w0); break;
    case 0x3A: primColor_ = uint32_t(w0); primLodFrac_ = uint8_t(w0 >> 32); break;
    case 0x3B: envColor_ = uint32_t(w0); break;
    case 0x3C: combine_ = w0; break;
    case 0x3D:
      texFmt_ = uint8_t((w0 >> 53) & 7); texSize_ = uint8_t((w0 >> 51) & 3);
      texWidth_ = uint16_t(((w0 >> 32) & 0x3FF) + 1); texAddr_ = uint32_t(w0) & 0x3FFFFFF;
      break;
    case 0x3E: zAddr_ = uint32_t(w0) & 0x3FFFFFF; break;
    case 0x3F:
      colorFmt_ = uint8_t((w0 >> 53) & 7); colorSize_ = uint8_t((w0 >> 51) & 3);
      colorWidth_ = uint16_t(((w0 >> 32) & 0x3FF) + 1); colorAddr_ = uint32_t(w0) & 0x3FFFFFF;
      break;
    default: break;
  }
}

// --------------------------------------------------------------------------------------- TMEM
void Rdp::loadTile(const uint64_t* w) {
  const uint64_t w0 = w[0];
  Tile& t = tiles_[(w0 >> 24) & 7];
  t.sl = uint16_t((w0 >> 44) & 0xFFF); t.tl = uint16_t((w0 >> 32) & 0xFFF);
  t.sh = uint16_t((w0 >> 12) & 0xFFF); t.th = uint16_t(w0 & 0xFFF);
  const int s0 = t.sl >> 2, t0 = t.tl >> 2, s1 = t.sh >> 2, t1 = t.th >> 2;
  const uint32_t base = uint32_t(t.tmem) * 8, line = uint32_t(t.line) * 8;
  for (int y = t0, r = 0; y <= t1 && r < 4096; y++, r++) {
    const uint32_t xo = (r & 1) ? 4 : 0;
    for (int x = s0, c = 0; x <= s1 && c < 4096; x++, c++) {
      switch (texSize_) {
        case 0: {  // 4 bits: dois texels por byte
          if (c & 1) break;
          const uint8_t v = rd8(texAddr_ + uint32_t((y * texWidth_ + x) >> 1));
          tmem_[((base + r * line + (c >> 1)) ^ xo) & 0xFFF] = v;
          break;
        }
        case 1: tmem_[((base + r * line + c) ^ xo) & 0xFFF] = rd8(texAddr_ + uint32_t(y * texWidth_ + x)); break;
        case 2: {
          const uint16_t v = rd16(texAddr_ + uint32_t(y * texWidth_ + x) * 2);
          const uint32_t d = ((base + r * line + c * 2) ^ xo) & 0xFFE;
          tmem_[d] = uint8_t(v >> 8); tmem_[d + 1] = uint8_t(v);
          break;
        }
        case 3: {
          const uint32_t v = rd32(texAddr_ + uint32_t(y * texWidth_ + x) * 4);
          const uint32_t d = ((base + r * line + c * 2) ^ xo) & 0x7FE;
          tmem_[d] = uint8_t(v >> 24); tmem_[d + 1] = uint8_t(v >> 16);
          tmem_[d + 0x800] = uint8_t(v >> 8); tmem_[d + 0x801] = uint8_t(v);
          break;
        }
      }
    }
  }
}

void Rdp::loadBlock(const uint64_t* w) {
  const uint64_t w0 = w[0];
  Tile& t = tiles_[(w0 >> 24) & 7];
  const int sl = int(w0 >> 44) & 0xFFF, tl = int(w0 >> 32) & 0xFFF, sh = int(w0 >> 12) & 0xFFF, dxt = int(w0) & 0xFFF;
  t.sl = uint16_t(sl); t.tl = uint16_t(tl); t.sh = uint16_t(sh); t.th = uint16_t(dxt);
  const int n = sh - sl + 1;  // texels
  if (n <= 0) return;
  const uint32_t base = uint32_t(t.tmem) * 8;
  if (texSize_ == 3) {
    const uint32_t src = texAddr_ + uint32_t(tl * texWidth_ + sl) * 4;
    for (int i = 0; i < n && i < 2048; i++) {
      const uint32_t v = rd32(src + uint32_t(i) * 4);
      const uint32_t palavra = uint32_t(i) / 2;
      const bool impar = ((palavra * uint32_t(dxt)) >> 11) & 1;
      const uint32_t d = ((base + uint32_t(i) * 2) ^ (impar ? 4 : 0)) & 0x7FE;
      tmem_[d] = uint8_t(v >> 24); tmem_[d + 1] = uint8_t(v >> 16);
      tmem_[d + 0x800] = uint8_t(v >> 8); tmem_[d + 0x801] = uint8_t(v);
    }
    return;
  }
  const int bits = 4 << texSize_;
  const uint32_t src = texAddr_ + uint32_t((tl * texWidth_ + sl) * bits / 8);
  const uint32_t bytes = uint32_t(n * bits + 7) / 8;
  const uint32_t palavras = std::min<uint32_t>((bytes + 7) / 8, 512);
  for (uint32_t p = 0; p < palavras; p++) {
    const bool impar = ((p * uint32_t(dxt)) >> 11) & 1;
    for (int b = 0; b < 8; b++) {
      const uint32_t d = (base + p * 8 + (uint32_t(b) ^ (impar ? 4 : 0))) & 0xFFF;
      tmem_[d] = rd8(src + p * 8 + uint32_t(b));
    }
  }
}

void Rdp::loadTlut(const uint64_t* w) {
  const uint64_t w0 = w[0];
  Tile& t = tiles_[(w0 >> 24) & 7];
  t.sl = uint16_t((w0 >> 44) & 0xFFF); t.tl = uint16_t((w0 >> 32) & 0xFFF);
  t.sh = uint16_t((w0 >> 12) & 0xFFF); t.th = uint16_t(w0 & 0xFFF);
  const int s0 = t.sl >> 2, s1 = t.sh >> 2, t0 = t.tl >> 2;
  const uint32_t base = uint32_t(t.tmem) * 8;
  for (int i = s0, k = 0; i <= s1 && k < 256; i++, k++) {
    const uint16_t v = rd16(texAddr_ + uint32_t(t0 * texWidth_ + i) * 2);
    for (int c = 0; c < 4; c++) {
      const uint32_t d = (base + uint32_t(k) * 8 + uint32_t(c) * 2) & 0xFFE;
      tmem_[d] = uint8_t(v >> 8); tmem_[d + 1] = uint8_t(v);
    }
  }
}

// texel (s, t inteiros já dentro do tile) em RGBA de 8 bits
void Rdp::fetch(const Tile& tl, int s, int t, uint8_t* c) {
  const uint32_t base = uint32_t(tl.tmem) * 8, line = uint32_t(tl.line) * 8;
  const uint32_t xo = (t & 1) ? 4 : 0;
  const bool tlut = (other_ >> 47) & 1;
  auto pal = [&](uint32_t idx) {
    const uint32_t d = (0x800 + idx * 8) & 0xFFE;
    const uint16_t v = uint16_t((tmem_[d] << 8) | tmem_[d + 1]);
    if ((other_ >> 46) & 1) { c[0] = c[1] = c[2] = uint8_t(v >> 8); c[3] = uint8_t(v); }
    else {
      const int r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
      c[0] = uint8_t((r << 3) | (r >> 2)); c[1] = uint8_t((g << 3) | (g >> 2)); c[2] = uint8_t((b << 3) | (b >> 2));
      c[3] = (v & 1) ? 255 : 0;
    }
  };
  switch (tl.size) {
    case 0: {
      const uint32_t a = ((base + uint32_t(t) * line + uint32_t(s >> 1)) ^ xo) & (tlut ? 0x7FF : 0xFFF);
      const uint8_t n = (s & 1) ? (tmem_[a] & 15) : (tmem_[a] >> 4);
      if (tlut) { pal(uint32_t(tl.pal << 4) | n); return; }
      if (tl.fmt == 3) {  // IA4
        const int i = n >> 1;
        c[0] = c[1] = c[2] = uint8_t((i << 5) | (i << 2) | (i >> 1));
        c[3] = (n & 1) ? 255 : 0;
      } else {
        c[0] = c[1] = c[2] = c[3] = uint8_t(n * 17);
      }
      return;
    }
    case 1: {
      const uint32_t a = ((base + uint32_t(t) * line + uint32_t(s)) ^ xo) & (tlut ? 0x7FF : 0xFFF);
      const uint8_t v = tmem_[a];
      if (tlut) { pal(v); return; }
      if (tl.fmt == 3) { c[0] = c[1] = c[2] = uint8_t((v >> 4) * 17); c[3] = uint8_t((v & 15) * 17); }
      else { c[0] = c[1] = c[2] = c[3] = v; }
      return;
    }
    case 2: {
      const uint32_t a = ((base + uint32_t(t) * line + uint32_t(s) * 2) ^ xo) & 0xFFE;
      const uint16_t v = uint16_t((tmem_[a] << 8) | tmem_[a + 1]);
      if (tl.fmt == 3 || tl.fmt == 4) { c[0] = c[1] = c[2] = uint8_t(v >> 8); c[3] = uint8_t(v); return; }
      if (tl.fmt == 1) { c[0] = c[1] = c[2] = uint8_t(v >> 8); c[3] = 255; return; }
      const int r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
      c[0] = uint8_t((r << 3) | (r >> 2)); c[1] = uint8_t((g << 3) | (g >> 2)); c[2] = uint8_t((b << 3) | (b >> 2));
      c[3] = (v & 1) ? 255 : 0;
      return;
    }
    default: {
      const uint32_t a = ((base + uint32_t(t) * line + uint32_t(s) * 2) ^ xo) & 0x7FE;
      c[0] = tmem_[a]; c[1] = tmem_[a + 1]; c[2] = tmem_[a + 0x800]; c[3] = tmem_[a + 0x801];
      return;
    }
  }
}

namespace {
// coordenada inteira de texel depois do espelho e da máscara
inline int envolve(int i, int mask, int mirror) {
  if (!mask) return i;
  if (mirror && ((i >> mask) & 1)) i = ~i;
  return i & ((1 << mask) - 1);
}
}  // namespace

// amostra (s, t em s10.5) do tile, com filtro de 3 pontos se pedido; out = 0xAABBGGRR
void Rdp::texel(int tile, int32_t s, int32_t t, uint32_t& out) {
  const Tile& tl = tiles_[tile & 7];
  if (tl.shiftS) s = tl.shiftS < 11 ? (s >> tl.shiftS) : (s << (16 - tl.shiftS));
  if (tl.shiftT) t = tl.shiftT < 11 ? (t >> tl.shiftT) : (t << (16 - tl.shiftT));
  s -= int32_t(tl.sl) << 3;
  t -= int32_t(tl.tl) << 3;
  const bool bilinear = ((other_ >> 45) & 1) && cycleType() < 2;
  if (tl.cs || !tl.maskS) { const int32_t mx = (int32_t(tl.sh) - int32_t(tl.sl)) << 3; s = std::clamp(s, 0, std::max(mx, 0)); }
  if (tl.ct || !tl.maskT) { const int32_t mx = (int32_t(tl.th) - int32_t(tl.tl)) << 3; t = std::clamp(t, 0, std::max(mx, 0)); }
  const int si = s >> 5, ti = t >> 5;
  uint8_t c0[4];
  if (!bilinear) {
    fetch(tl, envolve(si, tl.maskS, tl.ms), envolve(ti, tl.maskT, tl.mt), c0);
  } else {
    const int fs = s & 31, ft = t & 31;
    const int s0 = envolve(si, tl.maskS, tl.ms), s1 = envolve(si + 1, tl.maskS, tl.ms);
    const int t0 = envolve(ti, tl.maskT, tl.mt), t1 = envolve(ti + 1, tl.maskT, tl.mt);
    uint8_t a[4], b[4], d[4];
    if (fs + ft < 32) {
      fetch(tl, s0, t0, a); fetch(tl, s1, t0, b); fetch(tl, s0, t1, d);
      for (int k = 0; k < 4; k++) c0[k] = uint8_t(a[k] + (((b[k] - a[k]) * fs + (d[k] - a[k]) * ft + 16) >> 5));
    } else {
      fetch(tl, s1, t1, a); fetch(tl, s0, t1, b); fetch(tl, s1, t0, d);
      for (int k = 0; k < 4; k++)
        c0[k] = uint8_t(a[k] + (((b[k] - a[k]) * (32 - fs) + (d[k] - a[k]) * (32 - ft) + 16) >> 5));
    }
  }
  out = uint32_t(c0[0]) | (uint32_t(c0[1]) << 8) | (uint32_t(c0[2]) << 16) | (uint32_t(c0[3]) << 24);
}

// --------------------------------------------------------------------------------------- framebuffer
uint32_t Rdp::readColor(int x, int y, uint8_t& cvg) {
  const uint32_t i = uint32_t(y) * colorWidth_ + uint32_t(x);
  if (colorSize_ == 3) {
    const uint32_t v = rd32(colorAddr_ + i * 4);
    cvg = uint8_t(v);
    return (v >> 24) | (((v >> 16) & 0xFF) << 8) | (((v >> 8) & 0xFF) << 16) | 0xFF000000u;
  }
  if (colorSize_ == 2) {
    const uint16_t v = rd16(colorAddr_ + i * 2);
    const int r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
    cvg = (v & 1) ? 255 : 0;
    return uint32_t((r << 3) | (r >> 2)) | (uint32_t((g << 3) | (g >> 2)) << 8) | (uint32_t((b << 3) | (b >> 2)) << 16) | 0xFF000000u;
  }
  const uint8_t v = rd8(colorAddr_ + i);
  cvg = 255;
  return v | (uint32_t(v) << 8) | (uint32_t(v) << 16) | 0xFF000000u;
}

void Rdp::writeColor(int x, int y, uint32_t c) {
  const uint32_t i = uint32_t(y) * colorWidth_ + uint32_t(x);
  const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
  if (colorSize_ == 3) wr32(colorAddr_ + i * 4, (r << 24) | (g << 16) | (b << 8) | a);
  else if (colorSize_ == 2) wr16(colorAddr_ + i * 2, uint16_t(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a ? 1 : 0)));
  else wr8(colorAddr_ + i, uint8_t(r));
}

uint32_t Rdp::readZ(int x, int y) { return zExpande(rd16(zAddr_ + (uint32_t(y) * colorWidth_ + uint32_t(x)) * 2)); }
void Rdp::writeZ(int x, int y, uint32_t z, uint16_t) { wr16(zAddr_ + (uint32_t(y) * colorWidth_ + uint32_t(x)) * 2, zComprime(z)); }

// --------------------------------------------------------------------------------------- pixel (ciclos 1 e 2)
namespace {
struct Comb { int a, b, c, d; };
inline int canal(uint32_t c, int k) { return int((c >> (8 * k)) & 0xFF); }
inline uint32_t rgbaDe(uint32_t corRdp) {  // cor do RDP (RRGGBBAA) -> 0xAABBGGRR
  return (corRdp >> 24) | (((corRdp >> 16) & 0xFF) << 8) | (((corRdp >> 8) & 0xFF) << 16) | ((corRdp & 0xFF) << 24);
}
}  // namespace

// seletores do combinador e do blender, refeitos quando mudam os modos (fontes: enum F* em rdp.h)
void Rdp::preparaPipe() {
  const uint64_t cb = combine_;
  const int rgbA[2] = {int(cb >> 52) & 15, int(cb >> 37) & 15}, rgbB[2] = {int(cb >> 28) & 15, int(cb >> 24) & 15};
  const int rgbC[2] = {int(cb >> 47) & 31, int(cb >> 32) & 31}, rgbD[2] = {int(cb >> 15) & 7, int(cb >> 6) & 7};
  const int alA[2] = {int(cb >> 44) & 7, int(cb >> 21) & 7}, alB[2] = {int(cb >> 12) & 7, int(cb >> 3) & 7};
  const int alC[2] = {int(cb >> 41) & 7, int(cb >> 18) & 7}, alD[2] = {int(cb >> 9) & 7, int(cb >> 0) & 7};
  static const uint8_t BASE[6] = {F_COMB, F_T0, F_T1, F_PRIM, F_SHADE, F_ENV};
  auto mA = [](int v) -> uint8_t { return v < 6 ? BASE[v] : v == 6 ? F_ONE : v == 7 ? F_NOISE : F_ZERO; };
  auto mB = [](int v) -> uint8_t { return v < 6 ? BASE[v] : v == 6 ? F_KEYC : v == 7 ? F_K4 : F_ZERO; };
  auto mC = [](int v) -> uint8_t {
    static const uint8_t T[16] = {F_COMB, F_T0, F_T1, F_PRIM, F_SHADE, F_ENV, F_KEYS, F_COMB_A,
                                  F_T0_A, F_T1_A, F_PRIM_A, F_SHADE_A, F_ENV_A, F_LOD, F_PRIMLOD, F_K5};
    return v < 16 ? T[v] : F_ZERO;
  };
  auto mD = [](int v) -> uint8_t { return v < 6 ? BASE[v] : v == 6 ? F_ONE : F_ZERO; };
  auto aABD = [](int v) -> uint8_t { return v < 6 ? BASE[v] : v == 6 ? F_ONE : F_ZERO; };
  auto aC = [](int v) -> uint8_t {
    static const uint8_t T[8] = {F_LOD, F_T0, F_T1, F_PRIM, F_SHADE, F_ENV, F_PRIMLOD, F_ZERO};
    return T[v & 7];
  };
  pipe_.usaT0 = pipe_.usaT1 = pipe_.usaShade = false;
  pipe_.usaRuido = (other_ & 3) == 3;  // teste de alfa com dither
  const bool dois = cycleType() == 1;
  for (int c = 0; c < 2; c++) {
    for (int k = 0; k < 3; k++) {
      pipe_.sel[c][k][0] = mA(rgbA[c]); pipe_.sel[c][k][1] = mB(rgbB[c]);
      pipe_.sel[c][k][2] = mC(rgbC[c]); pipe_.sel[c][k][3] = mD(rgbD[c]);
    }
    pipe_.sel[c][3][0] = aABD(alA[c]); pipe_.sel[c][3][1] = aABD(alB[c]);
    pipe_.sel[c][3][2] = aC(alC[c]); pipe_.sel[c][3][3] = aABD(alD[c]);
    if (!dois)  // 1 ciclo: texel 1 é o mesmo texel 0
      for (int k = 0; k < 4; k++)
        for (int q = 0; q < 4; q++) {
          uint8_t& f = pipe_.sel[c][k][q];
          if (f == F_T1) f = F_T0;
          if (f == F_T1_A) f = F_T0_A;
        }
    if (c == 0 && !dois) continue;  // 1 ciclo: só o segundo conjunto vale
    for (int k = 0; k < 4; k++)
      for (int q = 0; q < 4; q++) {
        const uint8_t f = pipe_.sel[c][k][q];
        if (f == F_T0 || f == F_T0_A) pipe_.usaT0 = true;
        if (f == F_T1 || f == F_T1_A) pipe_.usaT1 = true;
        if (f == F_SHADE || f == F_SHADE_A) pipe_.usaShade = true;
        if (f == F_NOISE) pipe_.usaRuido = true;
      }
  }
  for (int c = 0; c < 2; c++) {
    pipe_.m1a[c] = uint8_t(other_ >> (30 - 2 * c)) & 3; pipe_.m1b[c] = uint8_t(other_ >> (26 - 2 * c)) & 3;
    pipe_.m2a[c] = uint8_t(other_ >> (22 - 2 * c)) & 3; pipe_.m2b[c] = uint8_t(other_ >> (18 - 2 * c)) & 3;
    if (pipe_.m1b[c] == 2 && (c == 1 || dois)) pipe_.usaShade = true;
  }
  pipe_.prontoOther = other_;
  pipe_.prontoComb = combine_;
  pipe_.valido = true;
}

// constantes do combinador (cores do estado), preenchidas no começo de cada desenho
void Rdp::preparaFontes() {
  if (!pipe_.valido || pipe_.prontoOther != other_ || pipe_.prontoComb != combine_) preparaPipe();
  auto cor = [&](int f, uint32_t c) {  // c = RRGGBBAA
    src_[f][0] = int(c >> 24); src_[f][1] = int((c >> 16) & 0xFF); src_[f][2] = int((c >> 8) & 0xFF); src_[f][3] = int(c & 0xFF);
  };
  auto rep = [&](int f, int v) { for (int k = 0; k < 4; k++) src_[f][k] = v; };
  cor(F_PRIM, primColor_);
  cor(F_ENV, envColor_);
  rep(F_PRIM_A, int(primColor_ & 0xFF));
  rep(F_ENV_A, int(envColor_ & 0xFF));
  rep(F_ONE, 256);
  rep(F_ZERO, 0);
  rep(F_LOD, lodFrac_);
  rep(F_PRIMLOD, primLodFrac_);
  rep(F_K4, 0); rep(F_K5, 0); rep(F_KEYC, 0); rep(F_KEYS, 0);
  rep(F_COMB, 0); rep(F_COMB_A, 0);
  rep(F_T0, 0); rep(F_T1, 0); rep(F_T0_A, 0); rep(F_T1_A, 0); rep(F_SHADE, 0); rep(F_SHADE_A, 0);
}

void Rdp::pixel(int x, int y, const Px& p, int tile, uint16_t dz) {
  const uint64_t om = other_;
  const bool dois = cycleType() == 1;
  // z
  const uint32_t z = (om & 4) ? (uint32_t(primZ_ & 0x7FFF) << 3) : p.z;
  if (om & 0x10) {
    const uint32_t old = readZ(x, y);
    if (((om >> 10) & 3) == 3) {
      const int32_t lim = std::max<int32_t>(dz, 0x40);
      if (std::abs(int32_t(z) - int32_t(old)) > lim) return;
    } else if (z > old) {
      return;
    }
  }
  int32_t (*src)[4] = src_;
  if (pipe_.usaT0) {
    uint32_t t0;
    texel(tile, p.s, p.t, t0);
    for (int k = 0; k < 4; k++) src[F_T0][k] = int((t0 >> (8 * k)) & 0xFF);
    const int a = src[F_T0][3];
    for (int k = 0; k < 4; k++) src[F_T0_A][k] = a;
  }
  if (pipe_.usaT1) {
    uint32_t t1;
    texel((tile + 1) & 7, p.s, p.t, t1);
    for (int k = 0; k < 4; k++) src[F_T1][k] = int((t1 >> (8 * k)) & 0xFF);
    const int a = src[F_T1][3];
    for (int k = 0; k < 4; k++) src[F_T1_A][k] = a;
  }
  if (pipe_.usaShade) {
    src[F_SHADE][0] = clamp8(p.r); src[F_SHADE][1] = clamp8(p.g); src[F_SHADE][2] = clamp8(p.b);
    const int a = clamp8(p.a);
    src[F_SHADE][3] = a;
    for (int k = 0; k < 4; k++) src[F_SHADE_A][k] = a;
  }
  int ruido = 0;
  if (pipe_.usaRuido) {
    noise_ = noise_ * 1103515245u + 12345u;
    ruido = int(noise_ >> 16) & 0xFF;
    for (int k = 0; k < 4; k++) src[F_NOISE][k] = ruido;
  }
  for (int c = dois ? 0 : 1; c < 2; c++) {
    int out[4];
    for (int k = 0; k < 4; k++) {
      const uint8_t* q = pipe_.sel[c][k];
      out[k] = clamp8(((((src[q[0]][k] - src[q[1]][k]) * src[q[2]][k]) + 0x80) >> 8) + src[q[3]][k]);
    }
    for (int k = 0; k < 4; k++) { src[F_COMB][k] = out[k]; src[F_COMB_A][k] = out[3]; }
  }
  const int alpha = src[F_COMB][3];
  // teste de alfa
  if (om & 1) {
    const int lim = (om & 2) ? ruido : int(blendColor_ & 0xFF);
    if (alpha < lim) return;
  }
  // blender
  uint8_t mcvg = 255;
  uint32_t mem = 0;
  bool temMem = false;
  if ((om >> 6) & 1) { mem = readColor(x, y, mcvg); temMem = true; }
  int pix[4] = {src[F_COMB][0], src[F_COMB][1], src[F_COMB][2], alpha};
  const int sa = clamp8(p.a);
  auto blender = [&](int c, bool forca) {
    const int m1a = pipe_.m1a[c], m1b = pipe_.m1b[c], m2a = pipe_.m2a[c], m2b = pipe_.m2b[c];
    auto cor = [&](int sel, int k) -> int {
      switch (sel) {
        case 0: return pix[k];
        case 1: if (!temMem) { mem = readColor(x, y, mcvg); temMem = true; } return int((mem >> (8 * k)) & 0xFF);
        case 2: return int((blendColor_ >> (24 - 8 * k)) & 0xFF);
        default: return int((fogColor_ >> (24 - 8 * k)) & 0xFF);
      }
    };
    int A;
    switch (m1b) {
      case 0: A = pix[3]; break;
      case 1: A = int(fogColor_ & 0xFF); break;
      case 2: A = sa; break;
      default: A = 0; break;
    }
    int out[3];
    if (!forca) {
      for (int k = 0; k < 3; k++) out[k] = cor(m1a, k);
    } else {
      int B;
      switch (m2b) {
        case 0: B = 255 - A; break;
        case 1: if (!temMem) { mem = readColor(x, y, mcvg); temMem = true; } B = mcvg; break;
        case 2: B = 255; break;
        default: B = 0; break;
      }
      const int soma = A + B;
      for (int k = 0; k < 3; k++) {
        const int v = cor(m1a, k) * A + cor(m2a, k) * B;
        out[k] = soma > 255 ? v / soma : (v + 127) / 255;
      }
    }
    for (int k = 0; k < 3; k++) pix[k] = clamp8(out[k]);
  };
  const bool forca = (om >> 14) & 1;
  if (dois) { blender(0, true); blender(1, forca); }
  else if (forca || pipe_.m1a[0] != 0) blender(0, forca);
  writeColor(x, y, uint32_t(pix[0]) | (uint32_t(pix[1]) << 8) | (uint32_t(pix[2]) << 16) | 0xFF000000u);
  if (om & 0x20) writeZ(x, y, z, dz);
  pixels++;
}

// --------------------------------------------------------------------------------------- retângulos
void Rdp::fillRect(const uint64_t* w) {
  const uint64_t w0 = w[0];
  const int xl = int(w0 >> 44) & 0xFFF, yl = int(w0 >> 32) & 0xFFF, xh = int(w0 >> 12) & 0xFFF, yh = int(w0) & 0xFFF;
  const int ct = cycleType();
  int x0 = xh >> 2, y0 = yh >> 2, x1 = xl >> 2, y1 = yl >> 2;
  if (ct >= 2) { x1++; y1++; }
  x0 = std::max(x0, sxh_ >> 2); y0 = std::max(y0, syh_ >> 2);
  x1 = std::min(x1, (sxl_ + 3) >> 2); y1 = std::min(y1, (syl_ + 3) >> 2);
  x1 = std::min(x1, int(colorWidth_));
  if (ct == 3) {
    for (int y = y0; y < y1; y++) {
      for (int x = x0; x < x1; x++) {
        const uint32_t i = uint32_t(y) * colorWidth_ + uint32_t(x);
        if (colorSize_ == 3) wr32(colorAddr_ + i * 4, fillColor_);
        else if (colorSize_ == 2) wr16(colorAddr_ + i * 2, uint16_t((x & 1) ? fillColor_ : fillColor_ >> 16));
        else wr8(colorAddr_ + i, uint8_t(fillColor_ >> (8 * (3 - (x & 3)))));
      }
    }
    pixels += uint64_t(std::max(0, x1 - x0)) * uint64_t(std::max(0, y1 - y0));
    return;
  }
  preparaFontes();
  Px p{};
  p.z = uint32_t(primZ_ & 0x7FFF) << 3;
  for (int y = y0; y < y1; y++)
    for (int x = x0; x < x1; x++) pixel(x, y, p, 0, primDz_);
}

void Rdp::texRect(const uint64_t* w, bool flip) {
  const uint64_t w0 = w[0], w1 = w[1];
  const int xl = int(w0 >> 44) & 0xFFF, yl = int(w0 >> 32) & 0xFFF, tile = int(w0 >> 24) & 7;
  const int xh = int(w0 >> 12) & 0xFFF, yh = int(w0) & 0xFFF;
  const int32_t S = int16_t(w1 >> 48), T = int16_t(w1 >> 32), dsdx = int16_t(w1 >> 16), dtdy = int16_t(w1);
  const int ct = cycleType();
  int x0 = xh >> 2, y0 = yh >> 2, x1 = xl >> 2, y1 = yl >> 2;
  if (ct >= 2) { x1++; y1++; }
  const int cx0 = std::max(x0, sxh_ >> 2), cy0 = std::max(y0, syh_ >> 2);
  x1 = std::min(x1, (sxl_ + 3) >> 2); y1 = std::min(y1, (syl_ + 3) >> 2);
  x1 = std::min(x1, int(colorWidth_));
  const int32_t dx = ct == 2 ? dsdx >> 2 : dsdx;
  if (ct < 2) preparaFontes();
  // s, t em s10.15 (5 bits a mais para os passos s5.10)
  for (int y = cy0; y < y1; y++) {
    for (int x = cx0; x < x1; x++) {
      int32_t s, t;
      if (!flip) {
        s = (S << 5) + (x - x0) * dx;
        t = (T << 5) + (y - y0) * dtdy;
      } else {
        s = (S << 5) + (y - y0) * dsdx;
        t = (T << 5) + (x - x0) * (ct == 2 ? dtdy >> 2 : dtdy);
      }
      s >>= 5; t >>= 5;
      if (ct == 2) {
        // cópia: o texel vai direto para a imagem
        const Tile& tl = tiles_[tile];
        int32_t ss = s - (int32_t(tl.sl) << 3), tt = t - (int32_t(tl.tl) << 3);
        int si = envolve(ss >> 5, tl.maskS, tl.ms), ti = envolve(tt >> 5, tl.maskT, tl.mt);
        const uint32_t i = uint32_t(y) * colorWidth_ + uint32_t(x);
        const uint32_t base = uint32_t(tl.tmem) * 8, line = uint32_t(tl.line) * 8;
        const uint32_t xo = (ti & 1) ? 4 : 0;
        if (colorSize_ == 2) {
          uint16_t v;
          if (tl.size == 2) {
            const uint32_t a = ((base + uint32_t(ti) * line + uint32_t(si) * 2) ^ xo) & 0xFFE;
            v = uint16_t((tmem_[a] << 8) | tmem_[a + 1]);
          } else {
            uint8_t c[4];
            fetch(tl, si, ti, c);
            v = uint16_t(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (c[3] ? 1 : 0));
          }
          if ((other_ & 1) && !(v & 1)) continue;
          wr16(colorAddr_ + i * 2, v);
        } else if (colorSize_ == 1) {
          const uint32_t a = ((base + uint32_t(ti) * line + uint32_t(si)) ^ xo) & 0xFFF;
          wr8(colorAddr_ + i, tmem_[a]);
        } else {
          uint8_t c[4];
          fetch(tl, si, ti, c);
          if ((other_ & 1) && !c[3]) continue;
          wr32(colorAddr_ + i * 4, (uint32_t(c[0]) << 24) | (uint32_t(c[1]) << 16) | (uint32_t(c[2]) << 8) | c[3]);
        }
        pixels++;
        continue;
      }
      Px p{};
      p.s = s; p.t = t;
      p.z = uint32_t(primZ_ & 0x7FFF) << 3;
      pixel(x, y, p, tile, primDz_);
    }
  }
}

// --------------------------------------------------------------------------------------- triângulos
void Rdp::triangle(const uint64_t* w, bool shade, bool tex, bool zb) {
  const uint64_t w0 = w[0];
  const int tile = int(w0 >> 48) & 7;
  const int32_t yl = sx(w0 >> 32, 14), ym = sx(w0 >> 16, 14), yh = sx(w0, 14);
  const int64_t XL = int32_t(w[1] >> 32), DXL = int32_t(w[1]);
  const int64_t XH = int32_t(w[2] >> 32), DXH = int32_t(w[2]);
  const int64_t XM = int32_t(w[3] >> 32), DXM = int32_t(w[3]);
  int k = 4;
  // atributos: 0-3 r,g,b,a; 4-6 s,t,w; 7 z
  int64_t A[8] = {}, DX[8] = {}, DE[8] = {}, DY[8] = {};
  auto le4 = [&](int base) {
    const uint64_t* q = &w[k];
    for (int i = 0; i < 4 && base + i < 7; i++) {
      const int sh = 48 - 16 * i;
      A[base + i] = int32_t((uint32_t(uint16_t(q[0] >> sh)) << 16) | uint16_t(q[2] >> sh));
      DX[base + i] = int32_t((uint32_t(uint16_t(q[1] >> sh)) << 16) | uint16_t(q[3] >> sh));
      DE[base + i] = int32_t((uint32_t(uint16_t(q[4] >> sh)) << 16) | uint16_t(q[6] >> sh));
      DY[base + i] = int32_t((uint32_t(uint16_t(q[5] >> sh)) << 16) | uint16_t(q[7] >> sh));
    }
    k += 8;
  };
  if (shade) le4(0);
  if (tex) le4(4);
  if (zb) {
    A[7] = int32_t(w[k] >> 32); DX[7] = int32_t(w[k]);
    DE[7] = int32_t(w[k + 1] >> 32); DY[7] = int32_t(w[k + 1]);
  }
  if (!shade) {
    // sem shade: a cor do shade fica 0 (o combinador usa prim/env)
  }
  const bool persp = (other_ >> 51) & 1;
  if (cycleType() < 2) preparaFontes();
  const int ytop = yh & ~3;
  const uint16_t dz = uint16_t(std::min<int64_t>(0xFFFF, ((DX[7] < 0 ? -DX[7] : DX[7]) + (DY[7] < 0 ? -DY[7] : DY[7])) >> 13));
  const int ct = cycleType();
  int yIni = std::max(ytop >> 2, syh_ >> 2), yFim = std::min((yl + 3) >> 2, (syl_ + 3) >> 2);
  const int clipX0 = sxh_ >> 2, clipX1 = std::min((sxl_ + 3) >> 2, int(colorWidth_));
  if (yFim - yIni > 1024) yFim = yIni + 1024;
  for (int y = yIni; y < yFim; y++) {
    const int yq = y * 4 + 2;
    if (yq < yh || yq >= yl) continue;
    const int64_t xh = XH + ((DXH * (yq - ytop)) >> 2);
    const int64_t xo = yq < ym ? XM + ((DXM * (yq - ytop)) >> 2) : XL + ((DXL * (yq - ym)) >> 2);
    int64_t esq = std::min(xh, xo), dir = std::max(xh, xo);
    int xs = int((esq - 0x8000 + 0xFFFF) >> 16), xe = int((dir - 0x8000 + 0xFFFF) >> 16);
    xs = std::max(xs, clipX0); xe = std::min(xe, clipX1);
    if (xs >= xe) continue;
    // atributos no começo do vão
    const int64_t dyq = yq - ytop;
    const int64_t xc = (int64_t(xs) << 16) + 0x8000;
    int64_t v[8];
    for (int i = 0; i < 8; i++) v[i] = A[i] + ((DE[i] * dyq) >> 2) + ((DX[i] * (xc - xh)) >> 16);
    for (int x = xs; x < xe; x++) {
      if (ct == 3) {
        const uint32_t i = uint32_t(y) * colorWidth_ + uint32_t(x);
        if (colorSize_ == 3) wr32(colorAddr_ + i * 4, fillColor_);
        else if (colorSize_ == 2) wr16(colorAddr_ + i * 2, uint16_t((x & 1) ? fillColor_ : fillColor_ >> 16));
        pixels++;
      } else {
        Px p;
        p.r = int32_t(v[0] >> 16); p.g = int32_t(v[1] >> 16); p.b = int32_t(v[2] >> 16); p.a = int32_t(v[3] >> 16);
        if (tex) {
          if (persp) {
            const double inv = 32768.0 / double(v[6] > 0 ? v[6] : 1);
            p.s = int32_t(std::clamp(double(v[4]) * inv, -1048576.0, 1048576.0));
            p.t = int32_t(std::clamp(double(v[5]) * inv, -1048576.0, 1048576.0));
          } else {
            p.s = int32_t(v[4] >> 16);
            p.t = int32_t(v[5] >> 16);
          }
        } else {
          p.s = p.t = 0;
        }
        p.z = zb ? uint32_t(std::clamp<int64_t>(v[7] >> 13, 0, 0x3FFFF)) : 0;
        pixel(x, y, p, tile, dz);
      }
      for (int i = 0; i < 8; i++) v[i] += DX[i];
    }
  }
}

// --------------------------------------------------------------------------------------- save state
void Rdp::sync(StateIO& io) {
  io.sync(other_); io.sync(combine_);
  io.sync(fillColor_); io.sync(fogColor_); io.sync(blendColor_); io.sync(primColor_); io.sync(envColor_);
  io.sync(primLodFrac_); io.sync(primZ_); io.sync(primDz_);
  io.sync(texAddr_); io.sync(colorAddr_); io.sync(zAddr_);
  io.sync(texFmt_); io.sync(texSize_); io.sync(colorFmt_); io.sync(colorSize_);
  io.sync(texWidth_); io.sync(colorWidth_);
  io.sync(sxh_); io.sync(syh_); io.sync(sxl_); io.sync(syl_);
  for (auto& t : tiles_) io.raw(&t, sizeof t);
  io.raw(tmem_, sizeof tmem_);
  io.raw(cmd_, sizeof cmd_);
  io.sync(cmdLen_);
  if (io.reading()) {
    clampTo(cmdLen_, 0, 21);
    texSize_ &= 3; colorSize_ &= 3; texFmt_ &= 7; colorFmt_ &= 7;
    texAddr_ &= 0x3FFFFFF; colorAddr_ &= 0x3FFFFFF; zAddr_ &= 0x3FFFFFF;
    for (auto& v : {&sxh_, &syh_, &sxl_, &syl_}) clampTo(*v, 0, 0xFFF);
    for (auto& t : tiles_) {
      t.fmt &= 7; t.size &= 3; t.pal &= 15; t.maskT &= 15; t.maskS &= 15; t.shiftT &= 15; t.shiftS &= 15;
      t.line &= 0x1FF; t.tmem &= 0x1FF; t.ct &= 1; t.mt &= 1; t.cs &= 1; t.ms &= 1;
      t.sl &= 0xFFF; t.tl &= 0xFFF; t.sh &= 0xFFF; t.th &= 0xFFF;
    }
  }
}

}  // namespace indigo::n64
