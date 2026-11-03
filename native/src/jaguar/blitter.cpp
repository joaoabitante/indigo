// Blitter do Tom: copia e preenche retângulos de pixels entre duas janelas (A1 e A2), ponto a ponto. O modo phrase
// (64 bits por vez) é feito como uma sequência de pixels, com o mesmo resultado na memória e o mesmo alinhamento
// do ponteiro no fim de cada linha. A memória muda na hora, mas o Blitter fica "ocupado" (B_CMD) pelo tempo que
// o chip levaria, segura o barramento nesse tempo e só no fim interrompe o GPU (interrupção 4).
// Custo (aproximado): cada ciclo de memória da DRAM ~2 tiques; por pixel (ou por phrase no modo phrase) uma
// escrita, mais uma leitura para cada SRCEN/DSTEN/SRCENZ/DSTENZ e uma escrita a mais para DSTWRZ; 3 tiques por
// linha (passo dos ponteiros) e 4 para começar.
#include <algorithm>
#include <cstring>

#include "jaguar.h"

namespace indigo::jaguar {

namespace {
uint32_t rd32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t rd64(const uint8_t* p) { return (uint64_t(rd32(p)) << 32) | rd32(p + 4); }

// registros (offsets a partir de 0xF02200)
enum : uint32_t {
  A1_BASE = 0x00, A1_FLAGS = 0x04, A1_CLIP = 0x08, A1_PIXEL = 0x0C, A1_STEP = 0x10, A1_FSTEP = 0x14, A1_FPIXEL = 0x18,
  A1_INC = 0x1C, A1_FINC = 0x20, A2_BASE = 0x24, A2_FLAGS = 0x28, A2_MASK = 0x2C, A2_PIXEL = 0x30, A2_STEP = 0x34,
  B_COUNT = 0x3C, B_SRCD = 0x40, B_DSTD = 0x48, B_DSTZ = 0x50, B_SRCZ1 = 0x58, B_SRCZ2 = 0x60, B_PATD = 0x68,
  B_IINC = 0x70, B_ZINC = 0x74,
};

struct Ptr {
  uint32_t base = 0;
  int32_t x = 0, y = 0;  // 16.16
  uint32_t width = 0, bits = 16, ppp = 4, pitch = 0, zoff = 0;
  int32_t xadd = 0, yadd = 0;
  bool phrase = false;
  uint32_t maskx = 0xFFFFFFFFu, masky = 0xFFFFFFFFu;
  uint32_t xi() const { return uint32_t(x) >> 16; }
  uint32_t yi() const { return uint32_t(y) >> 16; }
  // posição do pixel em bits a partir da base (as phrases de uma linha podem ter espaço entre si: pitch)
  uint64_t bitPos(uint32_t extraPhrases = 0) const {
    const uint64_t xx = xi(), yy = yi();
    const uint64_t off = (yy * width + (xx & ~uint64_t(ppp - 1))) * (1 + pitch) + (xx & (ppp - 1)) + uint64_t(extraPhrases) * ppp;
    return off * bits;
  }
};

void setup(Ptr& p, uint32_t base, uint32_t flags) {
  static const uint32_t PITCH[4] = {0, 1, 3, 2};
  p.base = base & ~7u;
  p.bits = 1u << ((flags >> 3) & 7);
  if (p.bits > 32) p.bits = 32;
  p.ppp = 64 / p.bits;
  p.pitch = PITCH[flags & 3];
  p.zoff = (flags >> 6) & 7;
  const uint32_t m = (flags >> 9) & 3, e = (flags >> 11) & 15;
  p.width = ((4 | m) << e) >> 2;
}
}  // namespace

// os registros de intensidade (B_I3..B_I0) e de Z (B_Z3..B_Z0) são outra vista do padrão e dos dados de origem
void Jaguar::blitAlias(uint32_t o) {
  const uint32_t v = rd32(&blitRegs[o]);
  if (o >= 0x7C && o <= 0x88) {
    const int b = 6 - 2 * (3 - int(o - 0x7C) / 4);
    blitRegs[B_PATD + b + 1] = uint8_t(v >> 16);
    blitRegs[B_SRCD + b] = uint8_t(v >> 8);
    blitRegs[B_SRCD + b + 1] = uint8_t(v);
  } else if (o >= 0x8C && o <= 0x98) {
    const int b = 6 - 2 * (3 - int(o - 0x8C) / 4);
    blitRegs[B_SRCZ1 + b] = uint8_t(v >> 24); blitRegs[B_SRCZ1 + b + 1] = uint8_t(v >> 16);
    blitRegs[B_SRCZ2 + b] = uint8_t(v >> 8); blitRegs[B_SRCZ2 + b + 1] = uint8_t(v);
  }
}

void Jaguar::blit(uint32_t cmd) {
  // o destino pode cair nos próprios registros (B_CMD): não começa outra cópia no meio desta
  if (blitting_) return;
  struct Guarda { bool& b; ~Guarda() { b = false; } } guarda{blitting_};
  blitting_ = true;
  blits++;
  const uint8_t* R = blitRegs;
  auto reg = [&](uint32_t o) { return rd32(&R[o]); };
  const bool srcen = cmd & 1, srcenz = cmd & 2, srcenx = cmd & 4, dsten = cmd & 8, dstenz = cmd & 0x10,
             dstwrz = cmd & 0x20, clip = cmd & 0x40, upda1f = cmd & 0x100, upda1 = cmd & 0x200, upda2 = cmd & 0x400,
             dsta2 = cmd & 0x800, gourd = cmd & 0x1000, gourz = cmd & 0x2000, topben = cmd & 0x4000,
             patdsel = cmd & 0x10000, adddsel = cmd & 0x20000, cmpdst = cmd & (1u << 25),
             bcompen = cmd & (1u << 26), dcompen = cmd & (1u << 27), bkgwren = cmd & (1u << 28),
             srcshade = cmd & (1u << 30);
  const uint32_t zmode = (cmd >> 18) & 7, lfu = (cmd >> 21) & 15;
  const uint32_t f1 = reg(A1_FLAGS), f2 = reg(A2_FLAGS);
  Ptr a1, a2;
  setup(a1, reg(A1_BASE), f1);
  setup(a2, reg(A2_BASE), f2);
  a1.x = int32_t((reg(A1_PIXEL) << 16) | (reg(A1_FPIXEL) & 0xFFFF));
  a1.y = int32_t((reg(A1_PIXEL) & 0xFFFF0000u) | (reg(A1_FPIXEL) >> 16));
  a2.x = int32_t(reg(A2_PIXEL) << 16);
  a2.y = int32_t(reg(A2_PIXEL) & 0xFFFF0000u);
  if (f2 & 0x8000) {  // máscara da janela A2
    a2.maskx = ((reg(A2_MASK) & 0xFFFF) << 16) | 0xFFFF;
    a2.masky = (reg(A2_MASK) & 0xFFFF0000u) | 0xFFFF;
  }
  // o bit de Y da A1 vale para as duas janelas (defeito documentado do chip)
  a1.yadd = a2.yadd = (f1 & 0x40000) ? 0x10000 : 0;
  if (f1 & 0x100000) a1.yadd = -a1.yadd;
  switch ((f1 >> 16) & 3) {
    case 0: a1.xadd = 0x10000; a1.phrase = true; break;
    case 1: a1.xadd = 0x10000; break;
    case 2: a1.xadd = 0; break;
    default:
      a1.xadd = int32_t((reg(A1_INC) << 16) | (reg(A1_FINC) & 0xFFFF));
      a1.yadd = int32_t((reg(A1_INC) & 0xFFFF0000u) | (reg(A1_FINC) >> 16));
  }
  if (f1 & 0x80000) a1.xadd = -a1.xadd;
  if (f2 & 0x100000) a2.yadd = -a2.yadd;
  switch ((f2 >> 16) & 3) {
    case 0: a2.xadd = 0x10000; a2.phrase = true; break;
    case 1: a2.xadd = 0x10000; break;
    default: a2.xadd = 0; break;
  }
  if (f2 & 0x80000) a2.xadd = -a2.xadd;
  int32_t s1x = 0, s1y = 0, s2x = 0, s2y = 0;
  if (upda1f) { s1x = int32_t(reg(A1_FSTEP) & 0xFFFF); s1y = int32_t(reg(A1_FSTEP) >> 16); }
  if (upda1) { s1x |= int32_t(reg(A1_STEP) << 16); s1y |= int32_t(reg(A1_STEP) & 0xFFFF0000u); }
  if (upda2) { s2x = int32_t(reg(A2_STEP) << 16); s2y = int32_t(reg(A2_STEP) & 0xFFFF0000u); }
  const uint32_t count = reg(B_COUNT);
  uint32_t inner = count & 0xFFFF, outer = count >> 16;
  if (uint64_t(inner) * outer > (1u << 21)) outer = (1u << 21) / (inner ? inner : 1);  // proteção contra contas absurdas (2 M pontos)
  const int32_t clipW = int32_t(reg(A1_CLIP) & 0x7FFF), clipH = int32_t((reg(A1_CLIP) >> 16) & 0x7FFF);

  // Gouraud e Z: quatro faixas (uma por pixel da phrase); no modo pixel só a faixa 0
  uint32_t gi[4], gc[4], zi[4];
  for (int v = 0; v < 4; v++) {
    const int b = 6 - v * 2;
    gc[v] = R[B_PATD + b];
    gi[v] = (uint32_t(R[B_PATD + b + 1]) << 16) | (uint32_t(R[B_SRCD + b]) << 8) | R[B_SRCD + b + 1];
    zi[v] = (uint32_t(R[B_SRCZ1 + b]) << 24) | (uint32_t(R[B_SRCZ1 + b + 1]) << 16) | (uint32_t(R[B_SRCZ2 + b]) << 8) |
            R[B_SRCZ2 + b + 1];
  }
  int32_t ia = int32_t(reg(B_IINC) & 0xFFFFFF);
  if (ia & 0x800000) ia |= int32_t(0xFF000000u);
  const int32_t zadd = int32_t(reg(B_ZINC));

  Ptr& D = dsta2 ? a2 : a1;
  Ptr& S = dsta2 ? a1 : a2;
  auto readPix = [&](const Ptr& p, uint32_t extra = 0) -> uint32_t {
    const uint64_t bp = p.bitPos(extra);
    const uint32_t a = (p.base + uint32_t(bp >> 3)) & 0xFFFFFF;
    switch (p.bits) {
      case 32: return read32(a);
      case 16: return read16(a);
      case 8: return read8(a);
      default: return (read8(a) >> (8 - p.bits - (bp & 7))) & ((1u << p.bits) - 1);
    }
  };
  auto writePix = [&](const Ptr& p, uint32_t v, uint32_t extra = 0) {
    const uint64_t bp = p.bitPos(extra);
    const uint32_t a = (p.base + uint32_t(bp >> 3)) & 0xFFFFFF;
    switch (p.bits) {
      case 32: write32(a, v); break;
      case 16: write16(a, uint16_t(v)); break;
      case 8: write8(a, uint8_t(v)); break;
      default: {
        const int sh = int(8 - p.bits - (bp & 7));
        const uint8_t m = uint8_t(((1u << p.bits) - 1) << sh);
        write8(a, uint8_t((read8(a) & ~m) | ((v << sh) & m)));
      }
    }
  };
  // dado de um registro de 64 bits: no modo phrase, o campo do pixel; no modo pixel, o campo de baixo
  auto field = [&](uint32_t off, const Ptr& p) -> uint32_t {
    const uint64_t v = rd64(&R[off]);
    const uint32_t lane = p.phrase ? (p.ppp - 1) - (p.xi() & (p.ppp - 1)) : 0;
    const uint64_t m = p.bits == 32 ? 0xFFFFFFFFull : ((1ull << p.bits) - 1);
    return uint32_t((v >> (lane * p.bits)) & m);
  };
  // Z: sempre 16 bits por ponto (4 por phrase)
  auto zfield = [&](uint32_t off, const Ptr& p) -> uint32_t {
    const uint64_t v = rd64(&R[off]);
    const uint32_t lane = p.phrase ? 3 - (p.xi() & 3) : 0;
    return uint32_t((v >> (lane * 16)) & 0xFFFF);
  };
  const uint32_t mask = D.bits == 32 ? 0xFFFFFFFFu : ((1u << D.bits) - 1);
  const uint32_t srcBits = S.bits;

  // ciclos de memória por unidade (pixel, ou phrase do destino no modo phrase)
  const int64_t perUnit = 2 * (1 + (srcen || srcenx ? 1 : 0) + (dsten ? 1 : 0) + (srcenz ? 1 : 0) + (dstenz ? 1 : 0) +
                               (dstwrz ? 1 : 0));
  int64_t cost = 4;
  for (uint32_t ol = 0; ol < outer; ol++) {
    const int32_t a1start = a1.x, a2start = a2.x;
    int64_t lastUnit = -1;
    cost += 3;
    uint32_t bitPos = 0;
    if (bcompen && srcenx && inner < srcBits) bitPos = srcBits - inner;
    for (uint32_t il = 0; il < inner; il++) {
      int lane = 0;
      const uint32_t dPhr = D.xi() / 4;
      {  // tempo: um pixel, ou uma phrase nova do destino
        const int64_t u = D.phrase ? int64_t(D.xi() / D.ppp) : int64_t(il);
        if (!D.phrase || u != lastUnit) { cost += perUnit; lastUnit = u; }
      }
      if (D.phrase && (gourd || srcshade || gourz)) lane = 3 - int(D.xi() & 3);
      uint32_t src = (srcen || srcenx) ? readPix(S) : field(B_SRCD, S);
      uint32_t srcz = srcenz ? readPix(S, S.zoff) : zfield(B_SRCZ1, D);
      const uint32_t dst = dsten ? readPix(D) : field(B_DSTD, D);
      const uint32_t dstz = dstenz ? readPix(D, D.zoff) : zfield(B_DSTZ, D);
      if (gourz) srcz = zi[lane] >> 16;
      bool inhibit = false;
      if ((zmode & 1) && srcz < dstz) inhibit = true;
      if ((zmode & 2) && srcz == dstz) inhibit = true;
      if ((zmode & 4) && srcz > dstz) inhibit = true;
      if (bcompen) {  // um bit da origem decide se o pixel é escrito
        src = (src >> ((~bitPos) & (srcBits - 1))) & 1;
        bitPos++;
        if (!cmpdst && src == 0) inhibit = true;
      } else if (dcompen) {  // comparação com o padrão: transparência
        if (!cmpdst && src == field(B_PATD, S)) inhibit = true;
      }
      if ((bcompen || dcompen) && cmpdst && dst == field(B_PATD, D)) inhibit = true;
      if (clip) {
        const int32_t x = a1.x >> 16, y = a1.y >> 16;
        if (x < 0 || x >= clipW || y < 0 || y >= clipH) inhibit = true;
      }
      uint32_t out = dst, outz = dstz;
      if (!inhibit) {
        outz = srcz;
        if (patdsel) out = field(B_PATD, D);
        else if (adddsel) {
          uint32_t lo = (src & 0xFF) + (dst & 0xFF);
          if (!topben) {
            const int sum = int(int8_t(src & 0xFF)) + int(dst & 0xFF);
            lo = uint32_t(sum < 0 ? 0 : (sum > 255 ? 255 : sum));
          }
          out = lo | ((src & 0xF00) + (dst & 0xF00)) | ((src & 0xF000) + (dst & 0xF000));
        } else {
          out = 0;
          if (lfu & 1) out |= ~src & ~dst;
          if (lfu & 2) out |= ~src & dst;
          if (lfu & 4) out |= src & ~dst;
          if (lfu & 8) out |= src & dst;
        }
        if (gourd) out = ((gc[lane] & 0xFF) << 8) | ((gi[lane] >> 16) & 0xFF);
        if (srcshade) {
          int i = int(src & 0xFF) + int(int8_t((ia >> 16) & 0xFF));
          i = i < 0 ? 0 : (i > 255 ? 255 : i);
          out = (src & 0xFF00) | uint32_t(i);
        }
        out &= mask;
      }
      if (bkgwren || !inhibit) {
        writePix(D, out);
        if (dstwrz) writePix(D, outz, D.zoff);
      }
      // avanço dos ponteiros
      if (!bcompen) {
        a1.x += a1.xadd; a1.y += a1.yadd;
        a2.x = int32_t(uint32_t(a2.x + a2.xadd) & a2.maskx); a2.y = int32_t(uint32_t(a2.y + a2.yadd) & a2.masky);
      } else {
        a1.y += a1.yadd; a2.y = int32_t(uint32_t(a2.y + a2.yadd) & a2.masky);
        if (!dsta2) { a1.x += a1.xadd; if (bitPos % srcBits == 0) a2.x = int32_t(uint32_t(a2.x + a2.xadd) & a2.maskx); }
        else { a2.x = int32_t(uint32_t(a2.x + a2.xadd) & a2.maskx); if (bitPos % srcBits == 0) a1.x += a1.xadd; }
      }
      if (gourd || srcshade || gourz) {
        auto adv = [&](int v) { gi[v] = uint32_t(int32_t(gi[v]) + ia) & 0xFFFFFF; zi[v] += uint32_t(zadd); };
        if (D.phrase) {
          if (il + 1 == inner || D.xi() / 4 != dPhr)
            for (int v = 0; v < 4; v++) adv(v);
        } else adv(lane);
      }
    }
    // modo phrase: no fim da linha o ponteiro vai para o começo da phrase seguinte
    if (a1.phrase) {
      const uint32_t size = a1.ppp;
      if (a2.phrase && dsta2) a1.x += int32_t(((uint32_t(a2start) >> 16) % size) << 16);
      const uint32_t ps = (size - 1) << 16;
      a1.x = int32_t((uint32_t(a1.x) + ps) & ~ps);
    }
    if (a2.phrase) {
      const uint32_t size = a2.ppp;
      if (a1.phrase && !dsta2) a2.x += int32_t(((uint32_t(a1start) >> 16) % size) << 16);
      const uint32_t ps = (size - 1) << 16;
      a2.x = int32_t((uint32_t(a2.x) + ps) & ~ps);
    }
    a1.x += s1x; a1.y += s1y;
    a2.x += s2x; a2.y += s2y;
  }
  auto put = [&](uint32_t o, uint32_t v) { blitRegs[o] = uint8_t(v >> 24); blitRegs[o + 1] = uint8_t(v >> 16); blitRegs[o + 2] = uint8_t(v >> 8); blitRegs[o + 3] = uint8_t(v); };
  put(A1_PIXEL, (uint32_t(a1.y) & 0xFFFF0000u) | ((uint32_t(a1.x) >> 16) & 0xFFFF));
  put(A1_FPIXEL, (uint32_t(a1.y) << 16) | (uint32_t(a1.x) & 0xFFFF));
  put(A2_PIXEL, (uint32_t(a2.y) & 0xFFFF0000u) | ((uint32_t(a2.x) >> 16) & 0xFFFF));
  // tempo: começa quando quem escreveu B_CMD escreveu (ou quando o blit anterior acabar)
  const int64_t start = std::max(accessTime, blitEnd_);
  lastBlitCost = cost;
  blitEnd_ = start + cost;
  busFree_ = std::max(busFree_, blitEnd_);
  blitIrq_ = true;  // a interrupção 4 do GPU vem no fim (timers)
}

}  // namespace indigo::jaguar
