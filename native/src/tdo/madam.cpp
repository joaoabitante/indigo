// MADAM do 3DO: motor de cels, motor de matrizes e VDL (ver madam.h).
#include "madam.h"

#include <algorithm>
#include <cstdint>
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
  regs[0] = 0x01020200;  // revisão (MADAM "Green", fabricante MEC)
  regs[1] = 0x51;        // MSYSBITS da partida: a BIOS procura o tamanho da memória a partir daqui
  xpos = ypos = hdy = vdx = hddx = hddy = 0;
  hdx = 1 << 20;
  vdy = 1 << 16;
  pixc = 0x1F001F00;
  plutPtr = 0;
  statbits = 0;
  for (int i = 0; i < 32; i++) plut[i] = uint16_t((i << 10) | (i << 5) | i);
  celPixels = 0;
  celsDrawn = 0;
}

uint32_t Madam::read(uint32_t off) {
  off &= 0x7FC;
  if (off == 0x0000) return 0x01020200;
  if (off == 0x0028) return statbits;
  if (off == 0x07F8) return 0;  // motor de matrizes nunca ocupado (a conta é feita na hora)
  return regs[off / 4];
}

int Madam::write(uint32_t off, uint32_t v) {
  off &= 0x7FC;
  switch (off) {
    case 0x0000: return 0;  // diagnóstico (porta do analisador lógico)
    case 0x0028: statbits = v; return 0;
    case 0x0100:            // SPRSTRT
      regs[off / 4] = v;
      return drawCels(regs[0x5A4 / 4]);
    case 0x0104: case 0x0108: case 0x010C: return 0;  // parar/continuar/pausar: o desenho já terminou
    case 0x07F0: regs[off / 4] |= v; return 0;
    case 0x07F4: regs[0x7F0 / 4] &= ~v; return 0;
    case 0x07FC: matrixOp(v); return 0;
    default: break;
  }
  regs[off / 4] = v;
  return 0;
}

// motor de matrizes: M em 0x600 (linha i, coluna j = 0x600 + 16i + 4j), B em 0x640, resultado em 0x670 e o
// anterior em 0x660; N (64 bits) em 0x680/0x684. Tudo 16.16.
void Madam::matrixOp(uint32_t cmd) {
  int32_t* r = reinterpret_cast<int32_t*>(&regs[0x600 / 4]);
  if (cmd > 3) return;  // 4x1, 1x1, conversão de CCB e divisões não existem neste núcleo
  const int n = cmd == 1 ? 4 : 3;
  for (int i = 0; i < 4; i++) r[24 + i] = r[28 + i];  // resultado anterior
  if (cmd == 0) return;
  // contas em 64 bits com volta (como o somador do hardware), sem estouro indefinido
  auto mul = [](int64_t a, int64_t b) { return int64_t(uint64_t(a) * uint64_t(b)); };
  int64_t out[4] = {0, 0, 0, 0};
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++) out[i] = int64_t(uint64_t(out[i]) + uint64_t(mul(r[i * 4 + j], r[16 + j])));
  if (cmd == 3) {
    const int64_t nn = int64_t(uint64_t(uint32_t(r[32])) << 32 | uint32_t(r[33]));
    const int64_t z = out[2];
    const int64_t nz = (z == 0 || (z == -1 && nn == INT64_MIN)) ? 0 : nn / z;
    out[0] = mul(out[0], nz) >> 16;
    out[1] = mul(out[1], nz) >> 16;
  }
  for (int i = 0; i < n; i++) r[28 + i] = int32_t(out[i] >> 16);
}

// ------------------------------------------------------------------------------------------------ cels

struct Madam::CelCtx {
  uint32_t flags = 0;
  int readWidth = 320, writeWidth = 320;  // largura dos quadros lido e escrito (REGCTL0)
  int clipX = 319, clipY = 239;
  uint32_t readBase = 0, writeBase = 0;
};

namespace {
// REGCTL0: largura = soma dos dois grupos de bits (cada um com um só bit ligado)
int widthOf(uint32_t b) {
  int g1 = 0, g2 = 0;
  switch (b & 0xF) { case 1: g1 = 32; break; case 4: g1 = 256; break; case 8: g1 = 1024; break; default: break; }
  switch (b & 0xF0) { case 0x10: g2 = 64; break; case 0x20: g2 = 128; break; case 0x40: g2 = 256; break; default: break; }
  return g1 + g2 ? g1 + g2 : 320;
}
int sext5(int v) { return (v & 0x10) ? v - 32 : v; }
}  // namespace

// PIXC (uma metade de 16 bits, escolhida pelo bit P do pixel):
//   15 1S: fonte primária (0 = cel, 1 = quadro)   14-13 MS: multiplicador (0 = MF, 1 = AMV da cel por cor,
//   2 = PMV e PDV da cel, 3 = PMV da cel por cor)   12-10 MF (+1)   9-8 DF: divisor (0 = 16, 1 = 2, 2 = 4, 3 = 8)
//   7-6 2S: fonte secundária (0 = zero, 1 = AV, 2 = quadro, 3 = cel)   5-1 AV   0 2D: secundária dividida por 2
// Com USEAV no CCB o AV escolhe a conta: bit 0 subtrai a secundária, bit 1 estende o sinal dela (5 bits),
// bit 2 deixa dar a volta em vez de saturar, bits 3-4 dividem o resultado por 2, 4 ou 8.
// Com PXOR no CCB o resultado é primária XOR secundária.
uint16_t Madam::blend(uint16_t half, uint16_t cel, uint16_t frame, uint16_t amv, uint32_t flags) const {
  const int ms = (half >> 13) & 3;
  const int mf = ((half >> 10) & 7) + 1;
  static const int DF[4] = {4, 1, 2, 3};
  const int df = DF[(half >> 8) & 3];
  const int s2 = (half >> 6) & 3;
  const int av = (half >> 1) & 0x1F;
  const int sdv = half & 1;
  const uint16_t p1 = (half & 0x8000) ? frame : cel;
  const bool useAv = flags & ccb::USEAV, pxor = flags & 0x800;
  uint16_t out = 0;
  for (int ch = 0; ch < 3; ch++) {
    const int sh = ch * 5;  // 0 azul, 5 verde, 10 vermelho
    int pmv = mf, pdv = df;
    if (ms == 1) pmv = ((amv >> (ch * 3)) & 7) + 1;
    else if (ms == 2) { pmv = (cel & 7) + 1; pdv = DF[(cel >> 10) & 3]; }
    else if (ms == 3) { static const int S[3] = {2, 7, 12}; pmv = ((cel >> S[ch]) & 7) + 1; pdv = 2; }
    const int t = (((p1 >> sh) & 31) * pmv) >> pdv;
    int b;
    switch (s2) {
      case 0: b = 0; break;
      case 1: b = av; break;
      case 2: b = (frame >> sh) & 31; break;
      default: b = (cel >> sh) & 31; break;
    }
    b >>= sdv;
    int r;
    if (pxor) {
      r = (t ^ b) & 31;
    } else if (useAv) {
      int b2 = (av & 2) ? sext5(b) : b;
      r = (av & 1) ? t - b2 : t + b2;
      r >>= (av >> 3) & 3;
      r = (av & 4) ? (r & 31) : std::clamp(r, 0, 31);
    } else {
      r = std::min(t + b, 31);
    }
    out |= uint16_t(r << sh);
  }
  return out;
}

void Madam::putPixel(CelCtx& c, int x, int y, uint16_t color, bool pbit, uint16_t amv) {
  if (x < 0 || y < 0 || x > c.clipX || y > c.clipY) return;
  const uint32_t rel = uint32_t(x) * 4 + uint32_t(y & 1) * 2;
  // POVER: 00 = P do pixel, 10 = P zero, 11 = P um
  const uint32_t pov = (c.flags & ccb::POVER) >> 7;
  if (pov == 2) pbit = false;
  else if (pov == 3) pbit = true;
  const uint16_t half = pbit ? uint16_t(pixc >> 16) : uint16_t(pixc);
  uint16_t outc = uint16_t(color & 0x7FFF);
  if (half != 0x1F00 || (c.flags & (ccb::USEAV | 0x800))) {
    const uint16_t frame = m16(c.readBase + uint32_t(y >> 1) * uint32_t(c.readWidth) * 4 + rel);
    outc = blend(half, color, frame, amv, c.flags);
  }
  // bit 15 do quadro: o P da cel só com PLUTPOS (CCB bit 6); senão zero
  const bool p15 = (c.flags & 0x40) && pbit;
  w16(c.writeBase + uint32_t(y >> 1) * uint32_t(c.writeWidth) * 4 + rel, uint16_t(outc | (p15 ? 0x8000 : 0)));
  celPixels++;
}

void Madam::plotQuad(CelCtx& c, const int64_t (&px)[4], const int64_t (&py)[4], uint16_t color, bool pbit, uint16_t amv) {
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
  const int x0 = int(std::max<int64_t>((minx - 128 + 255) >> 8, 0));
  const int x1 = int(std::min<int64_t>((maxx - 128) >> 8, c.clipX));
  const int y0 = int(std::max<int64_t>((miny - 128 + 255) >> 8, 0));
  const int y1 = int(std::min<int64_t>((maxy - 128) >> 8, c.clipY));
  if (x0 > x1 || y0 > y1) return;
  quadBudget -= int64_t(x1 - x0 + 1) * (y1 - y0 + 1);
  if (quadBudget < 0) { budgetHit = true; return; }
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
  const bool lrform = !packed && bpp == 16 && (pre1 & 0x800);  // 16 bits no formato do quadro (duas linhas)
  const int height = (int((pre0 >> 6) & 0x3FF) + 1) << (lrform ? 1 : 0);
  const int width = packed ? 2048 : int(pre1 & 0x7FF) + 1;
  const int skipX = int((pre0 >> 24) & 0xF);
  const uint32_t woff = bpp >= 8 ? (pre1 >> 16) & 0x3FF : (pre1 >> 24) & 0xFF;
  const uint32_t pluta = flags & ccb::PLUTA;
  const bool bgnd = flags & ccb::BGND;

  CelCtx c;
  c.flags = flags;
  c.readWidth = widthOf(regs[0x130 / 4]);
  c.writeWidth = widthOf(regs[0x130 / 4] >> 8);
  const uint32_t clip = regs[0x134 / 4];
  if (clip) { c.clipX = int(clip & 0x7FF); c.clipY = int((clip >> 16) & 0x7FF); }
  c.readBase = regs[0x138 / 4];
  c.writeBase = regs[0x13C / 4];

  // decodifica um valor da cel: cor 1-5-5-5, bit P e AMV (3 bits por cor) para o PIXC
  auto decode = [&](uint32_t p, uint16_t& color, bool& pbit, uint16_t& amv) -> bool {
    amv = 0x1FF;
    if (linear) {
      if (bpp == 16) color = uint16_t(p);
      else if (bpp == 8) {  // 3-3-2: bits de baixo em zero, ou repetindo os de cima com REP8 (PRE0 bit 3)
        const uint32_t r = (p >> 5) & 7, g = (p >> 2) & 7, b = p & 3;
        if (pre0 & 8) color = uint16_t((r << 2 | r >> 1) << 10 | (g << 2 | g >> 1) << 5 | (b << 3 | b << 1 | b >> 1));
        else color = uint16_t(r << 12 | g << 7 | b << 3);
      } else {
        // poucos bits sem PLUT (não existe no hardware): a intensidade vira cinza
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
      if (bpp == 6) pbit = (p >> 5) & 1;
      if (bpp == 8) { const uint16_t a = uint16_t((p >> 5) & 7); amv = uint16_t(a << 6 | a << 3 | a); }
    }
    color &= 0x7FFF;
    return bgnd || color != 0;
  };

  // pontos dos cantos em 1/256 de pixel: P(col, linha)
  const int64_t X20 = int64_t(xpos) * 16, Y20 = int64_t(ypos) * 16;
  const int64_t VDX20 = int64_t(vdx) * 16, VDY20 = int64_t(vdy) * 16;
  auto point = [&](int col, int row, int64_t& ox, int64_t& oy) {
    const int64_t hx = int64_t(hdx) + int64_t(row) * hddx, hy = int64_t(hdy) + int64_t(row) * hddy;
    ox = (X20 + int64_t(row) * VDX20 + int64_t(col) * hx) >> 12;
    oy = (Y20 + int64_t(row) * VDY20 + int64_t(col) * hy) >> 12;
  };
  auto drawPixel = [&](int col, int row, uint16_t color, bool pbit, uint16_t amv) {
    int64_t px[4], py[4];
    point(col, row, px[0], py[0]);
    point(col + 1, row, px[1], py[1]);
    point(col + 1, row + 1, px[2], py[2]);
    point(col, row + 1, px[3], py[3]);
    plotQuad(c, px, py, color, pbit, amv);
  };

  uint32_t row = src;
  for (int r = 0; r < height && !budgetHit; r++) {
    if (packed) {
      // início da linha: deslocamento até a próxima (8 bits; nas cels de 8 e 16 bits, 10 bits num campo de 16)
      BitReader br(*this, row);
      const uint32_t off = bpp >= 8 ? br.get(16) & 0x3FF : br.get(8);
      int col = 0;
      for (int guard = 0; guard < 2048 && col < 2048; guard++) {
        const uint32_t type = br.get(2);
        const int count = int(br.get(6)) + 1;
        if (type == 0) break;
        if (type == 1) {
          for (int k = 0; k < count; k++, col++) {
            uint16_t color; bool pb; uint16_t amv;
            const bool vis = decode(br.get(bpp), color, pb, amv);
            if (vis && col >= skipX) drawPixel(col - skipX, r, color, pb, amv);
          }
        } else if (type == 2) {
          col += count;
        } else {
          uint16_t color; bool pb; uint16_t amv;
          const bool vis = decode(br.get(bpp), color, pb, amv);
          for (int k = 0; k < count; k++, col++) if (vis && col >= skipX) drawPixel(col - skipX, r, color, pb, amv);
        }
      }
      row += (off + 2) * 4;
    } else if (lrform) {
      // linhas aos pares na mesma palavra (como o quadro): par na metade de cima, ímpar na de baixo
      const uint32_t base = src + uint32_t(r >> 1) * (woff + 2) * 4 + uint32_t(r & 1) * 2;
      for (int col = 0; col + skipX < width; col++) {
        uint16_t color; bool pb; uint16_t amv;
        if (decode(m16(base + uint32_t(col + skipX) * 4), color, pb, amv)) drawPixel(col, r, color, pb, amv);
      }
    } else {
      BitReader br(*this, row);
      for (int k = 0; k < skipX; k++) br.get(bpp);
      for (int col = 0; col + skipX < width; col++) {
        uint16_t color; bool pb; uint16_t amv;
        if (decode(br.get(bpp), color, pb, amv)) drawPixel(col, r, color, pb, amv);
      }
      row += (woff + 2) * 4;
    }
  }
}

int Madam::drawCels(uint32_t addr) {
  const uint64_t before = celPixels;
  statbits = (statbits | 0x10) & ~0x40u;
  quadBudget = QUAD_BUDGET;
  budgetHit = false;
  int cels = 0;
  for (; cels < 4096; cels++) {
    addr = (addr % MEM_SIZE) & ~3u;
    const uint32_t flags = m32(addr);
    const uint32_t nextRaw = m32(addr + 4);
    const uint32_t next = (flags & ccb::NPABS) ? nextRaw : addr + 8 + nextRaw;
    // ponteiro seguinte impossível: a lista acaba aqui
    const bool last = (flags & ccb::LAST) || next == 0 || (next & ~0x3FFFFFu);
    if (!(flags & ccb::SKIP)) {
      const uint32_t srcRaw = m32(addr + 8);
      uint32_t src = (flags & ccb::SPABS) ? srcRaw : addr + 12 + srcRaw;
      if (flags & ccb::LDPLUT) {
        const uint32_t plutRaw = m32(addr + 12);
        plutPtr = (flags & ccb::PPABS) ? plutRaw : addr + 16 + plutRaw;
      }
      if (flags & ccb::YOXY) { xpos = int32_t(m32(addr + 16)); ypos = int32_t(m32(addr + 20)); }
      // campos opcionais: só os pedidos, um atrás do outro
      uint32_t o = addr + 24;
      if (flags & ccb::LDSIZE) {
        hdx = int32_t(m32(o)); hdy = int32_t(m32(o + 4)); vdx = int32_t(m32(o + 8)); vdy = int32_t(m32(o + 12));
        o += 16;
      }
      if (flags & ccb::LDPRS) { hddx = int32_t(m32(o)); hddy = int32_t(m32(o + 4)); o += 8; }
      if (flags & ccb::LDPPMP) { pixc = m32(o); o += 4; }
      uint32_t pre0, pre1 = 0;
      if (flags & ccb::CCBPRE) {
        pre0 = m32(o); o += 4;
        if (!(flags & ccb::PACKED)) pre1 = m32(o);
      } else {
        pre0 = m32(src); src += 4;
        if (!(flags & ccb::PACKED)) { pre1 = m32(src); src += 4; }
      }
      if (flags & ccb::LDPLUT) {
        const int bpp = bppOf(pre0);
        const int n = bpp == 1 ? 2 : bpp == 2 ? 4 : bpp == 4 ? 16 : 32;
        for (int i = 0; i < n; i++) plut[i] = m16(plutPtr + uint32_t(i) * 2);
      }
      drawOne(flags, src, pre0, pre1);
      celsDrawn++;
    }
    if (last || budgetHit) { cels++; break; }
    addr = next;
  }
  statbits = (statbits & ~0x10u) | 0x40;
  // tempo: ~2 pixels por ciclo da CPU e o custo de ler cada CCB
  return int((celPixels - before) / 2) + cels * 20;
}

// ------------------------------------------------------------------------------------------------ VDL

// Cada entrada da VDL: controle, endereço do quadro atual, do anterior, próxima entrada e as palavras de CLUT.
// Controle: linhas (bits 0-8; 0 = só aplica e segue), palavras de CLUT (bits 9-14), bit 15 carrega o anterior,
// bit 16 o atual, bit 18 próxima relativa, bit 21 liga o vídeo, bits 23-25 largura do quadro (320, 384, 512,
// 640, 1024). CLUT: bit 31 = 0 entrada de cor (bits 24-28 índice; bits 29-30: 0 todas, 1 azul, 2 verde, 3
// vermelho); 0xE0 cor de fundo, 0xE1 nada, 0xC0/0xC2 controle do vídeo (bit 25: pixels com o bit 15 usam a
// CLUT fixa, uma rampa linear).
void Madam::renderFrame(uint32_t* out) {
  if (!(regs[0x008 / 4] & 0x4000)) {  // VDL desligada no MCTL: tela preta
    std::fill(out, out + 320 * 240, 0xFF000000u);
    return;
  }
  uint8_t clut[3][32];
  for (int i = 0; i < 32; i++) clut[0][i] = clut[1][i] = clut[2][i] = uint8_t(i << 3 | i >> 2);
  uint32_t bg = 0xFF000000;
  bool fixedClut = false;
  uint32_t vdlAddr = regs[0x580 / 4];
  uint32_t fbBase = 0, srcY = 0, modulo = 320;
  bool video = false;
  int y = 0, hidden = HIDDEN_LINES;
  for (int entries = 0; y < 240 && entries < 512; entries++) {
    vdlAddr = (vdlAddr % MEM_SIZE) & ~3u;
    const uint32_t ctrl = m32(vdlAddr), cur = m32(vdlAddr + 4), nxt = m32(vdlAddr + 12);
    const int nwords = std::min(int((ctrl >> 9) & 0x3F), 34);
    const int nlines = int(ctrl & 0x1FF);
    static const uint32_t MOD[8] = {320, 384, 512, 640, 1024, 320, 320, 320};
    modulo = MOD[(ctrl >> 23) & 7];
    if (ctrl & vdl::LDCUR) { fbBase = cur & ~3u; srcY = (cur >> 1) & 1; }
    video = ctrl & vdl::ENVIDDMA;
    for (int k = 0; k < nwords; k++) {
      const uint32_t w = m32(vdlAddr + 16 + uint32_t(k) * 4);
      if (!(w & 0x80000000u)) {
        const int idx = (w >> 24) & 31, sel = (w >> 29) & 3;
        const uint8_t rr = uint8_t(w >> 16), gg = uint8_t(w >> 8), bb = uint8_t(w);
        if (sel == 0 || sel == 3) clut[0][idx] = rr;
        if (sel == 0 || sel == 2) clut[1][idx] = gg;
        if (sel == 0 || sel == 1) clut[2][idx] = bb;
      } else if ((w >> 24) == 0xE0) {
        bg = 0xFF000000u | (w & 0xFF) << 16 | (w & 0xFF00) | (w >> 16 & 0xFF);
      } else if ((w >> 24) == 0xC0 || (w >> 24) == 0xC2) {
        fixedClut = w & (1u << 25);  // controle do vídeo: o bit 15 do pixel escolhe a CLUT fixa (rampa)
      }
    }
    for (int l = 0; l < nlines && y < 240; l++, srcY++) {
      if (hidden > 0) { hidden--; continue; }  // linhas da VDL antes da área visível
      uint32_t* o = out + y++ * 320;
      if (!video) { std::fill(o, o + 320, bg); continue; }
      const uint32_t rowAddr = fbBase + (srcY >> 1) * modulo * 4 + (srcY & 1) * 2;
      for (int x = 0; x < 320; x++) {
        const uint16_t p = m16(rowAddr + uint32_t(x) * 4);
        if (!(p & 0x7FFF)) { o[x] = bg; continue; }
        if (fixedClut && (p & 0x8000)) {
          auto ramp = [](uint32_t i) { return (i << 3) | (i >> 2); };
          o[x] = 0xFF000000u | ramp(p & 31) << 16 | ramp((p >> 5) & 31) << 8 | ramp((p >> 10) & 31);
          continue;
        }
        o[x] = 0xFF000000u | uint32_t(clut[2][p & 31]) << 16 | uint32_t(clut[1][(p >> 5) & 31]) << 8 | clut[0][(p >> 10) & 31];
      }
    }
    vdlAddr = (ctrl & vdl::RELSEL) ? vdlAddr + nxt : nxt;
  }
  for (; y < 240; y++) std::fill(out + y * 320, out + y * 320 + 320, bg);
}

void Madam::sync(StateIO& io) {
  io.raw(regs, sizeof regs);
  io.sync(xpos); io.sync(ypos); io.sync(hdx); io.sync(hdy); io.sync(vdx); io.sync(vdy); io.sync(hddx); io.sync(hddy);
  io.sync(pixc); io.sync(plutPtr); io.sync(statbits);
  io.raw(plut, sizeof plut);
  io.sync(celPixels); io.sync(celsDrawn);
}

}  // namespace indigo::tdo
