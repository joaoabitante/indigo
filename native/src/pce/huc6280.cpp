#include "huc6280.h"

#include <utility>

namespace indigo::pce {

namespace {
// modos de endereçamento (uma letra por opcode):
// N nenhum (JSR), A acumulador, I implícito, M imediato, R relativo, Z página zero, B absoluto, X zp,X, Y zp,Y,
// D (abs), P (zp,X), Q (zp),Y, C abs,X, E abs,Y, W (zp), L zp+relativo (BBR/BBS), K bloco,
// 1 #imm,zp  2 #imm,zp,X  3 #imm,abs  4 #imm,abs,X (TST), J (abs,X)
const char MODES[] =
    "MPIMZZZZIMAIBBBL"
    "RQWMZXXZIEIIBCCL"
    "NPIMZZZZIMAIBBBL"
    "RQWIXXXZIEIICCCL"
    "IPIMRZZZIMAIBBBL"
    "RQWMIXXZIEIIICCL"
    "IPIIZZZZIMAIDBBL"
    "RQWKXXXZIEIIJCCL"
    "RPI1ZZZZIMIIBBBL"
    "RQW3XXYZIEIIBCCL"
    "MPM2ZZZZIMIIBBBL"
    "RQW4XXYZIEIICCEL"
    "MPIKZZZZIMIIBBBL"
    "RQWKIXXZIEIIICCL"
    "MPIKZZZZIMIIBBBL"
    "RQWKIXXZIEIIICCL";

bool memMode(char m) { return m != 'N' && m != 'A' && m != 'I' && m != 'M' && m != 'R'; }
}  // namespace

void HuC6280::reset() {
  for (int i = 0; i < 8; i++) mpr[i] = 0;
  mprBuf = 0;
  fast = false;
  p = static_cast<uint8_t>((p | FI) & ~(FD | FT));
  pc = static_cast<uint16_t>(peek(0xFFFE) | (peek(0xFFFF) << 8));
  irqDisabled = 0;
  tflag_ = false;
  pend_ = 0;
}

uint8_t HuC6280::peek(uint16_t addr) {
  const uint8_t* r = rd[mpr[addr >> 13]];
  return r ? r[addr & 0x1FFF] : 0xFF;
}

uint8_t HuC6280::operand(int m) {
  switch (m) {
    case 'A': case 'I': dummy(); return 0;
    case 'M': case 'R': return fetch();
    case 'Z': op_ = static_cast<uint16_t>(0x2000 | fetch()); idle(); break;
    case 'X': op_ = static_cast<uint16_t>(0x2000 | static_cast<uint8_t>(fetch() + x)); idle(); break;
    case 'Y': op_ = static_cast<uint16_t>(0x2000 | static_cast<uint8_t>(fetch() + y)); idle(); break;
    case 'D': op_ = fetch16(); break;
    case 'P': { uint8_t z = fetch(); idle(); op_ = zpWord(static_cast<uint8_t>(z + x)); idle(); break; }
    case 'Q': { uint8_t z = fetch(); idle(); op_ = zpWord(z); idle(); op_ = static_cast<uint16_t>(op_ + y); break; }
    case 'W': { uint8_t z = fetch(); idle(); op_ = zpWord(z); idle(); break; }
    case 'B': op_ = fetch16(); idle(); break;
    case 'C': case 'J': op_ = static_cast<uint16_t>(fetch16() + x); idle(); break;
    case 'E': op_ = static_cast<uint16_t>(fetch16() + y); idle(); break;
    case '1': op_ = fetch(); op2_ = static_cast<uint16_t>(0x2000 | fetch()); idle(); break;
    case '2': op_ = fetch(); op2_ = static_cast<uint16_t>(0x2000 | static_cast<uint8_t>(fetch() + x)); idle(); break;
    case '3': op_ = fetch(); op2_ = fetch16(); idle(); break;
    case '4': op_ = fetch(); op2_ = static_cast<uint16_t>(fetch16() + x); idle(); break;
    default: break;
  }
  return 0;
}

// AND/EOR/ORA; com a bandeira T a operação é feita na memória em (X) da página zero
void HuC6280::logic(int kind, uint8_t v) {
  auto f = [kind](uint8_t l, uint8_t r) -> uint8_t { return kind == 0 ? (l & r) : kind == 1 ? (l ^ r) : (l | r); };
  if (tflag_) {
    uint8_t m = read(0x2000 | x);
    idle();
    uint8_t r = f(v, m);
    nz(r);
    write(0x2000 | x, r);
  } else {
    a = f(a, v);
    nz(a);
  }
}

void HuC6280::adc(uint8_t v) {
  uint8_t src;
  if (tflag_) { src = read(0x2000 | x); idle(); } else src = a;
  unsigned r;
  if (p & FD) {
    if (tflag_) idle(); else dummy();
    r = (src & 0x0F) + (v & 0x0F) + (p & FC);
    if (r > 0x09) r += 0x06;
    r = (src & 0xF0) + (v & 0xF0) + (r > 0x0F ? 0x10 : 0) + (r & 0x0F);
    if (r > 0x9F) r += 0x60;
  } else {
    r = src + v + (p & FC);
    if (~(src ^ v) & (src ^ r) & 0x80) p |= FV; else p &= static_cast<uint8_t>(~FV);
  }
  p = static_cast<uint8_t>((p & ~FC) | (r > 0xFF ? FC : 0));
  nz(static_cast<uint8_t>(r));
  if (tflag_) write(0x2000 | x, static_cast<uint8_t>(r));
  else a = static_cast<uint8_t>(r);
}

void HuC6280::sbc(uint8_t v) {
  v = static_cast<uint8_t>(~v);
  int r;
  if (p & FD) {
    if (tflag_) idle(); else dummy();
    r = (a & 0x0F) + (v & 0x0F) + (p & FC);
    if (r <= 0x0F) r -= 0x06;
    r = (a & 0xF0) + (v & 0xF0) + (r > 0x0F ? 0x10 : 0) + (r & 0x0F);
    if (r <= 0xFF) r -= 0x60;
  } else {
    r = a + v + (p & FC);
    if (~(a ^ v) & (a ^ r) & 0x80) p |= FV; else p &= static_cast<uint8_t>(~FV);
  }
  p = static_cast<uint8_t>((p & ~FC) | (r > 0xFF ? FC : 0));
  a = static_cast<uint8_t>(r);
  nz(a);
}

void HuC6280::cmp(uint8_t r, uint8_t v) {
  p = static_cast<uint8_t>((p & ~(FC | FZ | FN)) | (r >= v ? FC : 0) | (r == v ? FZ : 0) | ((r - v) & 0x80));
}

// ASL/ROL/LSR/ROR/INC/DEC na memória: lê, ciclo interno, escreve
void HuC6280::rmw(int kind) {
  uint8_t v = read(op_);
  idle();
  switch (kind) {
    case 0: p = static_cast<uint8_t>((p & ~FC) | (v >> 7)); v = static_cast<uint8_t>(v << 1); break;
    case 1: { uint8_t c = p & FC; p = static_cast<uint8_t>((p & ~FC) | (v >> 7)); v = static_cast<uint8_t>((v << 1) | c); break; }
    case 2: p = static_cast<uint8_t>((p & ~FC) | (v & 1)); v >>= 1; break;
    case 3: { uint8_t c = p & FC; p = static_cast<uint8_t>((p & ~FC) | (v & 1)); v = static_cast<uint8_t>((v >> 1) | (c << 7)); break; }
    case 4: v++; break;
    default: v--; break;
  }
  nz(v);
  write(op_, v);
}

void HuC6280::branch(bool c) {
  const int8_t off = static_cast<int8_t>(op_);
  if (c) { dummy(); idle(); pc = static_cast<uint16_t>(pc + off); }
}

void HuC6280::interrupt(bool brk) {
  uint16_t vec;
  if (brk) vec = 0xFFF6;
  else if ((pend_ & TIQ) && (irqActive & ~irqDisabled & TIQ)) vec = 0xFFFA;
  else if (pend_ & IRQ1) vec = 0xFFF8;
  else if (pend_ & IRQ2) vec = 0xFFF6;
  else return;
  if (!brk) { dummy(); dummy(); }
  push(static_cast<uint8_t>(pc >> 8));
  push(static_cast<uint8_t>(pc));
  push(brk ? static_cast<uint8_t>(p | FB) : static_cast<uint8_t>(p & ~FB));
  p = static_cast<uint8_t>((p & ~(FD | FT)) | FI);
  uint8_t lo = read(vec);
  pc = static_cast<uint16_t>(lo | (read(static_cast<uint16_t>(vec + 1)) << 8));
  idle();
}

void HuC6280::blockStart() {
  dummy();
  idle();
  push(y); push(a); push(x);
  op_ = fetch16(); op2_ = fetch16(); op3_ = fetch16();
  idle();
}

void HuC6280::blockEnd() {
  idle();
  x = pop(); a = pop(); y = pop();
}

void HuC6280::step() {
  tflag_ = (p & FT) != 0;
  p &= static_cast<uint8_t>(~FT);
  const uint8_t opc = fetch();
  const char m = MODES[opc];
  const uint8_t imm = operand(m);
  auto val = [&]() -> uint8_t { return memMode(m) ? read(op_) : imm; };
  if (m == 'R') op_ = imm;
  if (m == 'M') op_ = imm;

  switch (opc) {
    // ---- lógica e aritmética
    case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x12: case 0x15: case 0x19: case 0x1D:
      logic(2, val()); break;
    case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x32: case 0x35: case 0x39: case 0x3D:
      logic(0, val()); break;
    case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x52: case 0x55: case 0x59: case 0x5D:
      logic(1, val()); break;
    case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x72: case 0x75: case 0x79: case 0x7D:
      adc(val()); break;
    case 0xE1: case 0xE5: case 0xE9: case 0xED: case 0xF1: case 0xF2: case 0xF5: case 0xF9: case 0xFD:
      sbc(val()); break;
    case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD2: case 0xD5: case 0xD9: case 0xDD:
      cmp(a, val()); break;
    case 0xE0: case 0xE4: case 0xEC: cmp(x, val()); break;
    case 0xC0: case 0xC4: case 0xCC: cmp(y, val()); break;
    case 0x24: case 0x2C: case 0x34: case 0x3C: case 0x89: {
      uint8_t v = val();
      p = static_cast<uint8_t>((p & ~(FZ | FV | FN)) | ((a & v) ? 0 : FZ) | (v & (FV | FN)));
      break;
    }
    // ---- leitura e escrita
    case 0xA1: case 0xA5: case 0xA9: case 0xAD: case 0xB1: case 0xB2: case 0xB5: case 0xB9: case 0xBD:
      a = val(); nz(a); break;
    case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE: x = val(); nz(x); break;
    case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC: y = val(); nz(y); break;
    case 0x81: case 0x85: case 0x8D: case 0x91: case 0x92: case 0x95: case 0x99: case 0x9D: write(op_, a); break;
    case 0x86: case 0x8E: case 0x96: write(op_, x); break;
    case 0x84: case 0x8C: case 0x94: write(op_, y); break;
    case 0x64: case 0x74: case 0x9C: case 0x9E: write(op_, 0); break;
    // ---- leitura-modificação-escrita
    case 0x06: case 0x0E: case 0x16: case 0x1E: rmw(0); break;
    case 0x26: case 0x2E: case 0x36: case 0x3E: rmw(1); break;
    case 0x46: case 0x4E: case 0x56: case 0x5E: rmw(2); break;
    case 0x66: case 0x6E: case 0x76: case 0x7E: rmw(3); break;
    case 0xE6: case 0xEE: case 0xF6: case 0xFE: rmw(4); break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: rmw(5); break;
    case 0x0A: p = static_cast<uint8_t>((p & ~FC) | (a >> 7)); a = static_cast<uint8_t>(a << 1); nz(a); break;
    case 0x2A: { uint8_t c = p & FC; p = static_cast<uint8_t>((p & ~FC) | (a >> 7)); a = static_cast<uint8_t>((a << 1) | c); nz(a); break; }
    case 0x4A: p = static_cast<uint8_t>((p & ~FC) | (a & 1)); a >>= 1; nz(a); break;
    case 0x6A: { uint8_t c = p & FC; p = static_cast<uint8_t>((p & ~FC) | (a & 1)); a = static_cast<uint8_t>((a >> 1) | (c << 7)); nz(a); break; }
    case 0x1A: a++; nz(a); break;
    case 0x3A: a--; nz(a); break;
    case 0xE8: x++; nz(x); break;
    case 0xC8: y++; nz(y); break;
    case 0xCA: x--; nz(x); break;
    case 0x88: y--; nz(y); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: {  // TSB / TRB
      uint8_t v = read(op_);
      p = static_cast<uint8_t>((p & ~(FZ | FV | FN)) | (v & (FV | FN)));
      if (!(v & a)) p |= FZ;
      v = (opc & 0x10) ? static_cast<uint8_t>(v & ~a) : static_cast<uint8_t>(v | a);
      idle();
      write(op_, v);
      break;
    }
    case 0x83: case 0x93: case 0xA3: case 0xB3: {  // TST
      idle();
      uint8_t v = read(op2_);
      idle();
      p = static_cast<uint8_t>((p & ~(FZ | FV | FN)) | (v & (FV | FN)) | ((v & op_) ? 0 : FZ));
      break;
    }
    case 0x07: case 0x17: case 0x27: case 0x37: case 0x47: case 0x57: case 0x67: case 0x77:  // RMB
    case 0x87: case 0x97: case 0xA7: case 0xB7: case 0xC7: case 0xD7: case 0xE7: case 0xF7: {  // SMB
      uint8_t v = read(op_);
      const uint8_t bit = static_cast<uint8_t>(1 << ((opc >> 4) & 7));
      v = (opc & 0x80) ? static_cast<uint8_t>(v | bit) : static_cast<uint8_t>(v & ~bit);
      idle(); idle();
      write(op_, v);
      break;
    }
    case 0x0F: case 0x1F: case 0x2F: case 0x3F: case 0x4F: case 0x5F: case 0x6F: case 0x7F:  // BBR
    case 0x8F: case 0x9F: case 0xAF: case 0xBF: case 0xCF: case 0xDF: case 0xEF: case 0xFF: {  // BBS
      uint8_t zp = fetch(); idle();
      int8_t off = static_cast<int8_t>(fetch()); idle();
      uint8_t v = read(0x2000 | zp);
      bool set = (v >> ((opc >> 4) & 7)) & 1;
      if (set == ((opc & 0x80) != 0)) { idle(); idle(); pc = static_cast<uint16_t>(pc + off); }
      break;
    }
    // ---- transferências e pilha
    case 0xAA: x = a; nz(x); break;
    case 0xA8: y = a; nz(y); break;
    case 0xBA: x = s; nz(x); break;
    case 0x8A: a = x; nz(a); break;
    case 0x9A: s = x; break;
    case 0x98: a = y; nz(a); break;
    case 0x48: push(a); break;
    case 0xDA: push(x); break;
    case 0x5A: push(y); break;
    case 0x08: push(static_cast<uint8_t>(p | FB)); break;
    case 0x68: idle(); a = pop(); nz(a); break;
    case 0xFA: idle(); x = pop(); nz(x); break;
    case 0x7A: idle(); y = pop(); nz(y); break;
    case 0x28: idle(); p = static_cast<uint8_t>(pop() & ~FB); break;
    case 0x02: idle(); std::swap(x, y); break;
    case 0x22: idle(); std::swap(a, x); break;
    case 0x42: idle(); std::swap(a, y); break;
    case 0x62: a = 0; break;
    case 0x82: x = 0; break;
    case 0xC2: y = 0; break;
    // ---- bandeiras
    case 0x18: p &= static_cast<uint8_t>(~FC); break;
    case 0xD8: p &= static_cast<uint8_t>(~FD); break;
    case 0x58: p &= static_cast<uint8_t>(~FI); break;
    case 0xB8: p &= static_cast<uint8_t>(~FV); break;
    case 0x38: p |= FC; break;
    case 0xF8: p |= FD; break;
    case 0x78: p |= FI; break;
    case 0xF4: p |= FT; break;
    // ---- desvios
    case 0x10: branch(!(p & FN)); break;
    case 0x30: branch(p & FN); break;
    case 0x50: branch(!(p & FV)); break;
    case 0x70: branch(p & FV); break;
    case 0x90: branch(!(p & FC)); break;
    case 0xB0: branch(p & FC); break;
    case 0xD0: branch(!(p & FZ)); break;
    case 0xF0: branch(p & FZ); break;
    case 0x80: idle(); idle(); pc = static_cast<uint16_t>(pc + static_cast<int8_t>(op_)); break;
    case 0x44: {  // BSR
      idle();
      const uint16_t ret = static_cast<uint16_t>(pc - 1);
      push(static_cast<uint8_t>(ret >> 8)); push(static_cast<uint8_t>(ret));
      idle(); idle(); idle();
      pc = static_cast<uint16_t>(pc + static_cast<int8_t>(op_));
      break;
    }
    case 0x4C: pc = op_; break;
    case 0x6C: { idle(); uint8_t lo = read(op_); pc = static_cast<uint16_t>(lo | (read(static_cast<uint16_t>(op_ + 1)) << 8)); idle(); break; }
    case 0x7C: { uint8_t lo = read(op_); pc = static_cast<uint16_t>(lo | (read(static_cast<uint16_t>(op_ + 1)) << 8)); idle(); break; }
    case 0x20: {
      uint8_t lo = fetch();
      idle();
      push(static_cast<uint8_t>(pc >> 8)); push(static_cast<uint8_t>(pc));
      uint8_t hi = fetch();
      idle();
      pc = static_cast<uint16_t>(lo | (hi << 8));
      break;
    }
    case 0x60: { idle(); uint8_t lo = pop(); uint8_t hi = pop(); idle(); idle(); pc = static_cast<uint16_t>((lo | (hi << 8)) + 1); break; }
    case 0x40: { idle(); p = static_cast<uint8_t>(pop() & ~FB); uint8_t lo = pop(); uint8_t hi = pop(); pc = static_cast<uint16_t>(lo | (hi << 8)); idle(); break; }
    case 0x00: interrupt(true); break;
    // ---- exclusivas do HuC6280
    case 0x54: fast = false; idle(); break;  // CSL
    case 0xD4: fast = true; idle(); break;   // CSH
    case 0x03: case 0x13: case 0x23:         // ST0/ST1/ST2
      idle();
      tick();
      bus->stWrite(opc == 0x03 ? 0 : opc == 0x13 ? 2 : 3, static_cast<uint8_t>(op_));
      break;
    case 0x43: {  // TMA
      idle(); idle();
      if (op_ & 0xFF) {
        uint8_t v = 0;
        for (int i = 0; i < 8; i++) if (op_ & (1 << i)) v |= mpr[i];
        mprBuf = v;
      }
      a = mprBuf;
      break;
    }
    case 0x53:  // TAM
      idle(); idle(); idle();
      if (op_ & 0xFF) {
        mprBuf = a;
        for (int i = 0; i < 8; i++) if (op_ & (1 << i)) mpr[i] = a;
      }
      break;
    case 0x73: case 0xC3: case 0xD3: case 0xE3: case 0xF3: {  // TII TDD TIN TIA TAI
      blockStart();
      uint16_t src = op_, dst = op2_, len = op3_;
      uint32_t n = 0;
      do {
        idle();
        uint8_t v = read(src);
        idle();
        write(dst, v);
        idle(); idle();
        switch (opc) {
          case 0x73: src++; dst++; break;
          case 0xC3: src--; dst--; break;
          case 0xD3: src++; break;
          case 0xE3: src++; dst = static_cast<uint16_t>(dst + ((n & 1) ? -1 : 1)); break;
          default: src = static_cast<uint16_t>(src + ((n & 1) ? -1 : 1)); dst++; break;
        }
        n++;
        len--;
      } while (len);
      blockEnd();
      break;
    }
    default: break;  // NOPs (o ciclo de leitura falsa já aconteceu)
  }

  if (pend_ || (irqActive & ~irqDisabled & TIQ)) interrupt(false);
}

void HuC6280::sync(StateIO& io) {
  io.sync(a); io.sync(x); io.sync(y); io.sync(s); io.sync(p); io.sync(pc);
  io.raw(mpr, sizeof mpr); io.sync(mprBuf); io.sync(fast); io.sync(clk);
  io.sync(irqActive); io.sync(irqDisabled); io.sync(pend_);
  if (io.reading()) { irqActive &= 7; irqDisabled &= 7; pend_ &= 7; }
}

}  // namespace indigo::pce
