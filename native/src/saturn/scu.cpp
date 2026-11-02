// SCU do Saturn: interrupções do SH-2 mestre, DMA de 3 níveis (direto e indireto), temporizadores 0/1 e o DSP.
//
// Interrupções (como o aparelho, segundo os testes do Mednafen): cada fonte tem um nível fixo; a SCU apresenta
// ao mestre a de maior prioridade que não está mascarada em IMS e tira o bit de IST nesse momento. Quando o
// mestre aceita (lê o vetor), a SCU mascara tudo (IMS = 0xBFFF) até o programa escrever IMS de novo.
#include <algorithm>
#include <cstring>

#include "saturn.h"

namespace indigo::saturn {

namespace {
// nível de cada fonte interna (bit 0..15) e das externas do barramento A (bit 16..31)
constexpr uint8_t INT_LEVEL[17] = {0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x8, 0x6, 0x6, 0x5, 0x3, 0x2, 0x0, 0x0, 0x0};
constexpr uint8_t EXT_LEVEL[17] = {0x7, 0x7, 0x7, 0x7, 0x4, 0x4, 0x4, 0x4, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x1, 0x0};
inline int lowestBit(uint32_t v) { if (!v) return 16; int n = 0; while (!(v & 1)) { v >>= 1; n++; } return n; }
inline int32_t sx(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }
}  // namespace

void Scu::reset() {
  ist_ = 0;
  ims_ = 0xBFFF;
  level_ = vec_ = 0;
  for (auto& d : dma_) d = Level{};
  t0cmp_ = t0cnt_ = t1rel_ = t1md_ = 0;
  asr0_ = asr1_ = aref_ = rsel_ = 0;
  dsp = Dsp{};
}

void Scu::check() {
  if (level_ == 0) {
    const uint32_t pm = ist_ & ~uint32_t(int32_t(int16_t(ims_)));
    const int wi = lowestBit(pm & 0xFFFF), we = lowestBit(pm >> 16);
    int lv = INT_LEVEL[wi], vec = 0x40 + wi, bit = wi;
    if (EXT_LEVEL[we] > INT_LEVEL[wi]) { lv = EXT_LEVEL[we]; vec = 0x50 + we; bit = 16 + we; }
    if (lv) {
      level_ = lv;
      vec_ = vec;
      ist_ &= ~(1u << bit);
    }
  }
  sys_.master.setIrl(level_);
}

void Scu::raise(int src) {
  ist_ |= 1u << src;
  if (src == INT_DSP) dsp.end = true;
  check();
}

int Scu::masterVector() {
  const int v = vec_;
  ims_ = 0xBFFF;
  level_ = 0;
  sys_.master.setIrl(0);
  return v;
}

void Scu::lineStart(int line) {
  if (!(t1md_ & 1)) return;  // TENB
  t0cnt_ = (t0cnt_ + 1) & 0x3FF;
  bool t0 = false;
  if (t0cnt_ == t0cmp_) { raise(INT_TIMER0); factor(DF_TIMER0); t0 = true; }
  // temporizador 1: a cada linha (ou só na linha do temporizador 0, com o modo ligado)
  if (!(t1md_ & 0x100) || t0) { raise(INT_TIMER1); factor(DF_TIMER1); }
}

void Scu::vblankOut() { t0cnt_ = 0; }

uint32_t Scu::read(uint32_t off) {
  switch (off & 0xFC) {
    case 0x00: case 0x20: case 0x40: return dma_[off >> 5].r;
    case 0x04: case 0x24: case 0x44: return dma_[off >> 5].w;
    case 0x7C: return 0;  // DMA sempre já terminou (é feito na hora)
    case 0x80: {
      uint32_t v = dsp.pc;
      v |= uint32_t(dsp.t0) << 23 | uint32_t(dsp.s) << 22 | uint32_t(dsp.z) << 21 | uint32_t(dsp.c) << 20;
      v |= uint32_t(dsp.v) << 19 | uint32_t(dsp.end) << 18 | uint32_t(dsp.run) << 16;
      dsp.v = dsp.end = false;
      return v;
    }
    case 0x8C: {
      if (dsp.run) return 0xFFFFFFFF;
      const uint32_t v = dsp.data[(dsp.pda >> 6) & 3][dsp.pda & 63];
      dsp.pda++;
      return v;
    }
    case 0xA4: return ist_;
    case 0xA8: return 0;
    case 0xC4: return rsel_;
    case 0xC8: return 4;  // versão da SCU
    default: return 0;
  }
}

void Scu::write(uint32_t off, uint32_t v, uint32_t m) {
  auto upd = [&](uint32_t& r, uint32_t keep) { r = (r & ~(m & keep)) | (v & m & keep); };
  switch (off & 0xFC) {
    case 0x00: case 0x20: case 0x40: upd(dma_[off >> 5].r, 0x07FFFFFF); break;
    case 0x04: case 0x24: case 0x44: upd(dma_[off >> 5].w, 0x07FFFFFF); break;
    case 0x08: case 0x28: case 0x48: upd(dma_[off >> 5].c, (off >> 5) ? 0xFFF : 0xFFFFF); break;
    case 0x0C: case 0x2C: case 0x4C: upd(dma_[off >> 5].add, 0x107); break;
    case 0x10: case 0x30: case 0x50: {
      Level& d = dma_[off >> 5];
      upd(d.en, 0x100);
      if ((v & m & 1) && (d.en & 0x100) && (d.md & 7) == DF_NOW) startDma(int(off >> 5));
      break;
    }
    case 0x14: case 0x34: case 0x54: upd(dma_[off >> 5].md, 0x01010107); break;
    case 0x60: break;  // DMA forçado a parar: já terminou
    case 0x80: {
      const uint32_t x = v & m;
      if (x & (1u << 15)) { dsp.pc = uint8_t(x); dsp.jumpPending = false; }
      if (!(x & ((1u << 25) | (1u << 26)))) {
        dsp.run = (x >> 16) & 1;
        if ((x & (1u << 17)) && !dsp.run && !dspBusy_) {  // passo único
          dspBusy_ = true;
          dspExec(dsp.prog[dsp.pc++]);
          dspBusy_ = false;
        }
      }
      break;
    }
    case 0x84: if (!dsp.run) dsp.prog[dsp.pc++] = v; break;
    case 0x88: dsp.pda = uint8_t(v); break;   // endereço de acesso à RAM de dados
    case 0x8C:
      if (!dsp.run) { dsp.data[(dsp.pda >> 6) & 3][dsp.pda & 63] = v; dsp.pda++; }
      break;
    case 0x90: upd(t0cmp_, 0x3FF); break;
    case 0x94: upd(t1rel_, 0x1FF); break;
    case 0x98: upd(t1md_, 0x101); if (!(t1md_ & 1)) t0cnt_ = 0; break;
    case 0xA0: upd(ims_, 0xBFFF); check(); break;
    case 0xA4: ist_ &= (v | ~m); check(); break;
    case 0xA8: break;
    case 0xB0: upd(asr0_, 0xFFFFFFFF); break;
    case 0xB4: upd(asr1_, 0xFFFFFFFF); break;
    case 0xB8: upd(aref_, 0x1F); break;
    case 0xC4: upd(rsel_, 1); break;
    default: break;
  }
}

void Scu::factor(int f) {
  for (int l = 0; l < 3; l++) {
    const Level& d = dma_[l];
    if ((d.en & 0x100) && int(d.md & 7) == f && f != DF_NOW) startDma(l);
  }
}

// o DMA não alcança os registradores da própria SCU
static bool scuReg(uint32_t a) { return (a & 0x07FF0000) == 0x05FE0000; }

// transferência de um bloco: leitura de 32 bits, escrita com o passo de D?AD (0, 2, 4, 8... bytes);
// no barramento B (VDP, SCSP) a escrita é em palavras de 16 bits
void Scu::dmaTransfer(int level, uint32_t src, uint32_t dst, uint32_t count) {
  Level& d = dma_[level];
  const uint32_t radd = (d.add & 0x100) ? 4 : 0;
  const uint32_t wadd = (d.add & 7) ? (1u << (d.add & 7)) : 0;
  const bool bbusW = dst >= 0x05A00000 && dst < 0x05FF0000;
  uint32_t r = src, w = dst;
  for (uint32_t done = 0; done < count;) {
    if (bbusW) {
      const uint16_t v = uint16_t(sys_.busRead(r & ~1u, 2));
      if (!scuReg(w)) sys_.busWrite(w, 2, v);
      w += wadd;
      r += 2;
      if (!radd) r -= 2;
      done += 2;
    } else {
      const uint32_t v = sys_.busRead(r & ~3u, 4);
      const uint32_t n = std::min<uint32_t>(4, count - done);
      if (scuReg(w)) {}
      else if (n == 4) sys_.busWrite(w & ~3u, 4, v);
      else for (uint32_t k = 0; k < n; k++) sys_.busWrite(w + k, 1, (v >> (24 - 8 * k)) & 0xFF);
      w += wadd ? std::max<uint32_t>(wadd, 4) : 0;
      r += radd;
      done += 4;
    }
  }
  if (d.md & 0x10000) d.r = r & 0x07FFFFFF;   // atualiza o endereço de leitura
  if (d.md & 0x100) d.w = w & 0x07FFFFFF;     // e o de escrita
}

void Scu::startDma(int level) {
  if (dmaBusy_) return;  // um DMA não dispara outro (trava contra recursão)
  dmaBusy_ = true;
  struct Fim { bool& b; ~Fim() { b = false; } } fim{dmaBusy_};
  Level& d = dma_[level];
  if (d.md & 0x01000000) {
    // indireto: tabela em D?W com (quantidade, destino, origem); bit 31 da origem marca a última
    uint32_t t = d.w & 0x07FFFFFC;
    const uint32_t saveR = d.r, saveW = d.w;
    uint64_t total = 0;
    for (int guard = 0; guard < 4096 && total < 0x1000000; guard++) {
      uint32_t cnt = sys_.busRead(t, 4) & 0xFFFFF, dst = sys_.busRead(t + 4, 4), src = sys_.busRead(t + 8, 4);
      t += 12;
      if (!cnt) cnt = 0x100000;
      const uint32_t md = d.md;
      d.md &= ~0x10100u;  // na tabela os endereços não são escritos de volta
      dmaTransfer(level, src & 0x07FFFFFF, dst & 0x07FFFFFF, cnt);
      total += cnt;
      d.md = md;
      if (src & 0x80000000u) break;
    }
    d.r = saveR;
    d.w = (d.md & 0x100) ? t : saveW;
  } else {
    uint32_t cnt = d.c;
    if (!cnt) cnt = level ? 0x1000 : 0x100000;
    dmaTransfer(level, d.r, d.w, cnt);
  }
  raise(level == 0 ? INT_DMA0 : level == 1 ? INT_DMA1 : INT_DMA2);
}

// ------------------------------------------------------------------------------------------------ DSP

uint32_t Scu::dspSrc(int sel) {
  const int bank = sel & 3;
  return dsp.data[bank][dsp.ct[bank] & 63];
}

void Scu::dspRun(int cycles) {
  if (!dsp.run) return;
  // a instrução seguinte já foi buscada quando um desvio acontece: ela roda antes do destino
  for (int n = cycles / 2; n > 0 && dsp.run; n--) {
    const bool jp = dsp.jumpPending;
    dsp.jumpPending = false;
    const uint32_t op = dsp.prog[dsp.pc++];
    if (jp) dsp.pc = dsp.jumpTo;
    dspExec(op);
  }
}

void Scu::dspDma(uint32_t op) {
  const bool hold = (op >> 14) & 1, fmt = (op >> 13) & 1, dir = (op >> 12) & 1;
  const int drw = (op >> 8) & 7, addMode = (op >> 15) & 7;
  int count;
  if (fmt) {
    const int crw = op & 3;
    count = int(dsp.data[crw][dsp.ct[crw]] & 0xFF);
    if (op & 4) dsp.ct[crw] = (dsp.ct[crw] + 1) & 63;
  } else {
    count = int(op & 0xFF);
  }
  if (!count) count = 256;
  if (dir) {  // RAM de dados -> barramento externo
    const uint32_t add = (1u << addMode) & ~1u;
    uint32_t a = (dsp.wa0 << 2) & 0x07FFFFFF;
    const bool bbus = a >= 0x05A00000 && a < 0x05FF0000;
    for (int i = 0; i < count; i++) {
      uint32_t v = 0xFFFFFFFF;
      if (!(drw & 4)) { v = dsp.data[drw][dsp.ct[drw]]; dsp.ct[drw] = (dsp.ct[drw] + 1) & 63; }
      if (bbus) {
        if (!scuReg(a)) sys_.busWrite(a, 2, v >> 16);
        a += add;
        if (!scuReg(a)) sys_.busWrite(a, 2, v & 0xFFFF);
        a += add;
      } else { if (!scuReg(a)) sys_.busWrite(a & ~3u, 4, v); a += add; }
    }
    if (!hold) dsp.wa0 = (a + 2) >> 2;
  } else {  // barramento externo -> RAM de dados (ou de programa)
    const uint32_t add = (1u << (addMode & 2)) & ~1u;
    uint32_t a = (dsp.ra0 << 2) & 0x07FFFFFF;
    int pram = 0;
    for (int i = 0; i < count; i++) {
      const uint32_t v = sys_.busRead(a & ~3u, 4);
      a += (a >= 0x05A00000 && a < 0x05FF0000) ? 4 : add;
      if (drw & 4) { if (!(drw & 3)) dsp.prog[(pram++) & 0xFF] = v; }
      else { dsp.data[drw][dsp.ct[drw]] = v; dsp.ct[drw] = (dsp.ct[drw] + 1) & 63; }
    }
    if (!hold) dsp.ra0 = a >> 2;
  }
}

void Scu::dspExec(uint32_t op) {
  auto cond = [&](uint32_t c) {
    if (!(c & 0x40)) return true;
    bool r = false;
    if (c & 1) r |= dsp.z;
    if (c & 2) r |= dsp.s;
    if (c & 4) r |= dsp.c;
    if (c & 8) r |= dsp.t0;
    return r == bool(c & 0x20);
  };
  switch (op >> 28) {
    case 0x0: case 0x1: case 0x2: case 0x3: {
      // instrução de operação: ALU, barramento X, barramento Y e D1 em paralelo
      int64_t alu = dsp.acc;
      const uint32_t al = uint32_t(dsp.acc), pl = uint32_t(dsp.p);
      auto zs32 = [&](uint32_t v) { dsp.s = int32_t(v) < 0; dsp.z = v == 0; };
      auto setL = [&](uint32_t v) { alu = (alu & ~int64_t(0xFFFFFFFF)) | v; };
      switch ((op >> 26) & 0xF) {
        case 0x1: setL(al & pl); dsp.c = false; zs32(al & pl); break;
        case 0x2: setL(al | pl); dsp.c = false; zs32(al | pl); break;
        case 0x3: setL(al ^ pl); dsp.c = false; zs32(al ^ pl); break;
        case 0x4: { const uint64_t t = uint64_t(al) + pl; dsp.v |= ((~(al ^ pl) & (al ^ uint32_t(t))) >> 31) & 1; dsp.c = (t >> 32) & 1; zs32(uint32_t(t)); setL(uint32_t(t)); break; }
        case 0x5: { const uint64_t t = uint64_t(al) - pl; dsp.v |= (((al ^ pl) & (al ^ uint32_t(t))) >> 31) & 1; dsp.c = (t >> 32) & 1; zs32(uint32_t(t)); setL(uint32_t(t)); break; }
        case 0x6: {
          const uint64_t a48 = uint64_t(dsp.acc) & 0xFFFFFFFFFFFFull, p48 = uint64_t(dsp.p) & 0xFFFFFFFFFFFFull, t = a48 + p48;
          dsp.v |= ((~(a48 ^ p48) & (a48 ^ t)) >> 47) & 1;
          dsp.c = (t >> 48) & 1;
          const int64_t s48 = int64_t(t << 16) >> 16;
          dsp.s = s48 < 0; dsp.z = (t & 0xFFFFFFFFFFFFull) == 0;
          alu = s48;
          break;
        }
        case 0x8: { dsp.c = al & 1; const uint32_t t = uint32_t(int32_t(al) >> 1); zs32(t); setL(t); break; }
        case 0x9: { dsp.c = al & 1; const uint32_t t = (al >> 1) | (al << 31); zs32(t); setL(t); break; }
        case 0xA: { dsp.c = al >> 31; const uint32_t t = al << 1; zs32(t); setL(t); break; }
        case 0xB: { dsp.c = al >> 31; const uint32_t t = (al << 1) | (al >> 31); zs32(t); setL(t); break; }
        case 0xF: { dsp.c = (al >> 24) & 1; const uint32_t t = (al << 8) | (al >> 24); zs32(t); setL(t); break; }
        default: break;
      }
      uint32_t readMask = 0, ctInc = 0;
      const int xop = (op >> 23) & 7, yop = (op >> 17) & 7, d1 = (op >> 12) & 3;
      if ((xop & 3) == 2) dsp.p = int64_t(int32_t(dsp.rx)) * int32_t(dsp.ry);
      if (xop >= 3) {
        const int s = (op >> 20) & 7, b = s & 3;
        const uint32_t v = dsp.data[b][dsp.ct[b]];
        readMask |= 1u << b;
        if (s & 4) ctInc |= 1u << b;
        if ((xop & 3) == 3) dsp.p = int32_t(v);
        if (xop & 4) dsp.rx = v;
      }
      if ((yop & 3) == 1) dsp.acc = 0;
      else if ((yop & 3) == 2) dsp.acc = alu;
      if (yop >= 3) {
        const int s = (op >> 14) & 7, b = s & 3;
        const uint32_t v = dsp.data[b][dsp.ct[b]];
        readMask |= 1u << b;
        if (s & 4) ctInc |= 1u << b;
        if ((yop & 3) == 3) dsp.acc = int32_t(v);
        if (yop & 4) dsp.ry = v;
      }
      if (d1 & 1) {
        const int d = (op >> 8) & 0xF;
        uint32_t v = uint32_t(int32_t(int8_t(op & 0xFF)));
        if (d1 & 2) {
          const int s = op & 0xF;
          if (s < 4) { v = dsp.data[s][dsp.ct[s]]; readMask |= 1u << s; }
          else if (s < 8) { v = dsp.data[s & 3][dsp.ct[s & 3]]; readMask |= 1u << (s & 3); if (d != (s & 3)) ctInc |= 1u << (s & 3); }
          else if (s == 9) v = uint32_t(alu);
          else if (s == 0xA) v = uint32_t(alu >> 16);
          else v = 0xFFFFFFFF;
        }
        switch (d) {
          case 0: case 1: case 2: case 3:
            if (!(readMask & (1u << d))) { dsp.data[d][dsp.ct[d]] = v; ctInc |= 1u << d; }
            break;
          case 4: dsp.rx = v; break;
          case 5: dsp.p = int32_t(v); break;
          case 6: dsp.ra0 = v; break;
          case 7: dsp.wa0 = v; break;
          case 0xA: dsp.lop = uint16_t(v & 0xFFF); break;
          case 0xB: dsp.top = uint8_t(v); break;
          case 0xC: case 0xD: case 0xE: case 0xF: dsp.ct[d - 0xC] = uint8_t(v & 63); ctInc &= ~(1u << (d - 0xC)); break;
          default: break;
        }
      }
      for (int b = 0; b < 4; b++) if (ctInc & (1u << b)) dsp.ct[b] = (dsp.ct[b] + 1) & 63;
      break;
    }
    case 0x8: case 0x9: case 0xA: case 0xB: {  // MVI
      const uint32_t c = (op >> 19) & 0x7F;
      const uint32_t imm = (c & 0x40) ? uint32_t(sx(op, 19)) : uint32_t(sx(op, 25));
      if (!cond(c)) break;
      const int d = (op >> 26) & 0xF;
      switch (d) {
        case 0: case 1: case 2: case 3: dsp.data[d][dsp.ct[d]] = imm; dsp.ct[d] = (dsp.ct[d] + 1) & 63; break;
        case 4: dsp.rx = imm; break;
        case 5: dsp.p = int32_t(imm); break;
        case 6: dsp.ra0 = imm; break;
        case 7: dsp.wa0 = imm; break;
        case 0xA: dsp.lop = uint16_t(imm & 0xFFF); break;
        case 0xC: dsp.top = dsp.pc; dsp.jumpPending = true; dsp.jumpTo = uint8_t(imm); break;
        default: break;
      }
      break;
    }
    case 0xC: dspDma(op); break;
    case 0xD:  // JMP (com um atraso de uma instrução, como o aparelho)
      if (cond((op >> 19) & 0x7F)) { dsp.jumpPending = true; dsp.jumpTo = uint8_t(op); }
      break;
    case 0xE:
      if (op & (1u << 27)) {  // LPS: repete a próxima instrução LOP vezes
        // LOP é lido uma vez só (LOP+1 execuções); LPS dentro de LPS não repete nada
        const uint32_t next = dsp.prog[dsp.pc++];
        const bool lps = (next >> 28) == 0xE && (next & (1u << 27));
        const uint32_t n = lps ? 0 : uint32_t(dsp.lop & 0xFFF) + 1;
        for (uint32_t k = 0; k < n; k++) dspExec(next);
        dsp.lop = 0xFFF;
      } else {  // BTM
        if (dsp.lop) { dsp.jumpPending = true; dsp.jumpTo = dsp.top; }
        dsp.lop = (dsp.lop - 1) & 0xFFF;
      }
      break;
    case 0xF:  // END / ENDI
      dsp.run = false;
      if (op & (1u << 27)) raise(INT_DSP);
      break;
    default: break;
  }
}

void Scu::sync(StateIO& io) {
  io.sync(ist_); io.sync(ims_); io.sync(level_); io.sync(vec_);
  for (auto& d : dma_) { io.sync(d.r); io.sync(d.w); io.sync(d.c); io.sync(d.add); io.sync(d.en); io.sync(d.md); }
  io.sync(t0cmp_); io.sync(t0cnt_); io.sync(t1rel_); io.sync(t1md_);
  io.sync(asr0_); io.sync(asr1_); io.sync(aref_); io.sync(rsel_);
  io.raw(dsp.prog, sizeof dsp.prog);
  io.raw(dsp.data, sizeof dsp.data);
  io.sync(dsp.pc); io.raw(dsp.ct, 4); io.sync(dsp.top); io.sync(dsp.pda); io.sync(dsp.lop);
  io.sync(dsp.rx); io.sync(dsp.ry); io.sync(dsp.ra0); io.sync(dsp.wa0); io.sync(dsp.acc); io.sync(dsp.p);
  io.sync(dsp.run); io.sync(dsp.step); io.sync(dsp.end); io.sync(dsp.s); io.sync(dsp.z); io.sync(dsp.c); io.sync(dsp.v);
  io.sync(dsp.t0); io.sync(dsp.jumpPending); io.sync(dsp.jumpTo);
  if (io.reading()) {
    for (auto& c : dsp.ct) c &= 63;
    dsp.lop &= 0xFFF;
    if (level_ < 0 || level_ > 15) level_ = 0;
  }
}

}  // namespace indigo::saturn
