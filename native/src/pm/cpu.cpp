#include "cpu.h"

namespace indigo::pm {

namespace {
inline uint16_t sx8(uint8_t v) { return uint16_t(int16_t(int8_t(v))); }
}  // namespace

void Minx::reset(bool hard) {
  if (hard) {
    A = B = L = H = I = N = XI = YI = 0;
    X = Y = SP = 0;
    U1 = U2 = 0;
    shiftU = 0;
  }
  status = RUN;
  irqVector = 0;
  V = 0;
  PC = rd16(hard ? 0 : 2);
  E = 0x1F;
  F = 0xC0;
  setU(0);
}

void Minx::setU(uint8_t v) {
  if (v != U2) shiftU = 2;
  U1 = v;
  U2 = v;
  flagsChanged();
}

// ------------------------------------------------------------------ aritmética
uint8_t Minx::add8(uint8_t a, uint8_t b, int c) {
  F &= 0xF0;
  switch (F & (FBCD | FNIB)) {
    case 0: {
      const int t = a + b + c;
      const uint8_t r = uint8_t(t);
      if (!r) F |= FZ;
      if (t > 0xFF) F |= FC;
      if (~(a ^ b) & (a ^ r) & 0x80) F |= FO;
      if (r & 0x80) F |= FS;
      return r;
    }
    case FBCD: {
      int t = a + b + c;
      if ((a & 15) + (b & 15) + c >= 10) t += 6;
      if (t >= 0xA0) t += 0x60;
      const uint8_t r = uint8_t(t);
      if (!r) F |= FZ;
      if (t > 0xFF) F |= FC;
      return r;
    }
    case FNIB: {
      const int t = (a & 15) + (b & 15) + c;
      if (!(t & 15)) F |= FZ;
      if (t >= 16) F |= FC;
      if (~(a ^ b) & (a ^ t) & 8) F |= FO;
      if (t & 8) F |= FS;
      return uint8_t(t & 15);
    }
    default: {
      int t = (a & 15) + (b & 15) + c;
      if (t >= 10) t += 6;
      if (!(t & 15)) F |= FZ;
      if (t >= 16) F |= FC;
      return uint8_t(t & 15);
    }
  }
}

uint8_t Minx::sub8(uint8_t a, uint8_t b, int c) {
  F &= 0xF0;
  switch (F & (FBCD | FNIB)) {
    case 0: {
      const int t = a - b - c;
      const uint8_t r = uint8_t(t);
      if (!r) F |= FZ;
      if (t < 0) F |= FC;
      if ((a ^ b) & (a ^ r) & 0x80) F |= FO;
      if (r & 0x80) F |= FS;
      return r;
    }
    case FBCD: {
      const int t = a - b - c;
      int r = t;
      if (unsigned((a & 15) - (b & 15) - c) >= 10u) r -= 6;
      r &= 0xFF;
      if (r >= 0xA0) r -= 0x60;
      if (!r) F |= FZ;
      if (t < 0) F |= FC;
      return uint8_t(r);
    }
    case FNIB: {
      const int t = (a & 15) - (b & 15) - c;
      if (!(t & 15)) F |= FZ;
      if (t < 0) F |= FC;
      if ((a ^ b) & (a ^ t) & 8) F |= FO;
      if (t & 8) F |= FS;
      return uint8_t(t & 15);
    }
    default: {
      int t = (a & 15) - (b & 15) - c;
      const bool borrow = t < 0;
      if (unsigned(t) >= 10u) t -= 6;
      if (!(t & 15)) F |= FZ;
      if (borrow) F |= FC;
      return uint8_t(t & 15);
    }
  }
}

uint16_t Minx::add16(uint16_t a, uint16_t b, int c) {
  const int t = a + b + c;
  const uint16_t r = uint16_t(t);
  F &= 0xF0;
  if (!r) F |= FZ;
  if (t > 0xFFFF) F |= FC;
  if (~(a ^ b) & (a ^ r) & 0x8000) F |= FO;
  if (r & 0x8000) F |= FS;
  return r;
}

uint16_t Minx::sub16(uint16_t a, uint16_t b, int c) {
  const int t = a - b - c;
  const uint16_t r = uint16_t(t);
  F &= 0xF0;
  if (!r) F |= FZ;
  if (t < 0) F |= FC;
  if ((a ^ b) & (a ^ r) & 0x8000) F |= FO;
  if (r & 0x8000) F |= FS;
  return r;
}

uint8_t Minx::logic(uint8_t r) {
  F &= 0xF6;
  if (!r) F |= FZ;
  if (r & 0x80) F |= FS;
  return r;
}

uint8_t Minx::inc8(uint8_t v) { v++; F &= 0xFE; if (!v) F |= FZ; return v; }
uint8_t Minx::dec8(uint8_t v) { v--; F &= 0xFE; if (!v) F |= FZ; return v; }
uint16_t Minx::inc16(uint16_t v) { v++; F &= 0xFE; if (!v) F |= FZ; return v; }
uint16_t Minx::dec16(uint16_t v) { v--; F &= 0xFE; if (!v) F |= FZ; return v; }

uint8_t Minx::shiftRot(int op, uint8_t v) {
  uint8_t r;
  const int c = carry();
  if (op == 0 || op == 2) F &= 0xF0;  // SAL e SAR mexem também no O
  else F &= 0xF4;
  switch (op) {
    case 0:  // SAL
      if (v & 0x80) F |= FC;
      if (((v >> 1) ^ v) & 0x40) F |= FO;
      r = uint8_t(v << 1);
      break;
    case 1:  // SHL
      if (v & 0x80) F |= FC;
      r = uint8_t(v << 1);
      break;
    case 2:  // SAR
      if (v & 1) F |= FC;
      r = (v & 0x80) | (v >> 1);
      break;
    case 3:  // SHR
      if (v & 1) F |= FC;
      r = v >> 1;
      break;
    case 4:  // ROLC
      if (v & 0x80) F |= FC;
      r = uint8_t((v << 1) | c);
      break;
    case 5:  // ROL
      if (v & 0x80) F |= FC;
      r = uint8_t((v << 1) | (v >> 7));
      break;
    case 6:  // RORC
      if (v & 1) F |= FC;
      r = uint8_t((v >> 1) | (c << 7));
      break;
    default:  // ROR
      if (v & 1) F |= FC;
      r = uint8_t((v >> 1) | (v << 7));
      break;
  }
  if (!r) F |= FZ;
  if (r & 0x80) F |= FS;
  return r;
}

uint8_t Minx::notOp(uint8_t v) {
  v = ~v;
  F &= 0xF6;
  if (!v) F |= FZ;
  if (v & 0x80) F |= FS;
  return v;
}

uint8_t Minx::neg(uint8_t v) {
  v = uint8_t(-v);
  F &= 0xF0;
  if (!v) F |= FZ;
  else F |= FC;
  if (v == 0x80) F |= FO;
  if (v & 0x80) F |= FS;
  return v;
}

void Minx::ret() {
  const uint8_t lo = pop(), hi = pop();
  PC = uint16_t(lo | (hi << 8));
  V = pop();
  setU(V);
}

// CALLI (interrupções e CINT) e JINT: empilha (banco, PC e) F, desliga as interrupções e lê o vetor no banco 0
void Minx::enterIrq(uint8_t vector, bool pushPc) {
  if (pushPc) {
    push(V);
    push(PC >> 8);
    push(PC & 0xFF);
  }
  push(F);
  F |= 0xC0;
  V = U1;
  U2 = U1;
  PC = rd16(vector);
  flagsChanged();
}

bool Minx::cond(int c) const {
  const bool z = F & FZ, o = F & FO, s = F & FS;
  switch (c & 15) {
    case 0: return o != s;           // L  (menor, com sinal)
    case 1: return (o != s) || z;    // LE
    case 2: return (o == s) && !z;   // G
    case 3: return o == s;           // GE
    case 4: return o;                // O
    case 5: return !o;               // NO
    case 6: return !s;               // P (positivo)
    case 7: return s;                // NP (negativo)
    default: {
      const bool bit = (E >> (c & 3)) & 1;
      return (c & 4) ? bit : !bit;   // 8..B: NX0..NX3; C..F: X0..X3
    }
  }
}

// ------------------------------------------------------------------ instruções de 1 byte
int Minx::step() {
  if (shiftU) {
    U1 = U2;
    U2 = V;
    shiftU--;
    flagsChanged();
  }
  if (status != RUN) {
    if (status == IRQ) {
      status = RUN;
      enterIrq(irqVector, true);
      return 20;
    }
    return 8;
  }

  const uint8_t op = fetch();

  // operando de 8 bits das operações da ALU 00..3F (A, B, #nn, [HL], [N+nn], [nnnn], [X], [Y])
  auto aluSrc = [&](int sel) -> uint8_t {
    switch (sel) {
      case 0: return A;
      case 1: return B;
      case 2: return fetch();
      case 3: return rd(aHL());
      case 4: return rd(aN(fetch()));
      case 5: return rd(aAbs(fetch16()));
      case 6: return rd(aX());
      default: return rd(aY());
    }
  };
  // fonte dos MOV 40..7F (A, B, L, H, [N+nn], [HL], [X], [Y])
  auto movSrc = [&](int sel) -> uint8_t {
    switch (sel) {
      case 0: return A;
      case 1: return B;
      case 2: return L;
      case 3: return H;
      case 4: return rd(aN(fetch()));
      case 5: return rd(aHL());
      case 6: return rd(aX());
      default: return rd(aY());
    }
  };
  auto get16 = [&](int r) -> uint16_t {
    switch (r & 3) { case 0: return BA(); case 1: return HL(); case 2: return X; default: return Y; }
  };
  auto set16 = [&](int r, uint16_t v) {
    switch (r & 3) { case 0: setBA(v); break; case 1: setHL(v); break; case 2: X = v; break; default: Y = v; break; }
  };

  if (op < 0x40) {
    const int sel = op & 7;
    const uint8_t v = aluSrc(sel);
    switch (op >> 3) {
      case 0: A = add8(A, v, 0); break;
      case 1: A = add8(A, v, carry()); break;
      case 2: A = sub8(A, v, 0); break;
      case 3: A = sub8(A, v, carry()); break;
      case 4: A = logic(A & v); break;
      case 5: A = logic(A | v); break;
      case 6: sub8(A, v, 0); break;
      default: A = logic(A ^ v); break;
    }
    return sel == 4 ? 12 : sel == 5 ? 16 : 8;
  }
  if (op < 0x60) {
    const int sel = op & 7;
    const uint8_t v = movSrc(sel);
    switch ((op >> 3) & 3) { case 0: A = v; break; case 1: B = v; break; case 2: L = v; break; default: H = v; break; }
    return sel < 4 ? 4 : sel == 4 ? 12 : 8;
  }
  if (op < 0x78) {
    const int sel = op & 7;
    const uint8_t v = movSrc(sel);
    const int d = (op >> 3) & 3;
    wr(d == 0 ? aX() : d == 1 ? aHL() : aY(), v);
    return sel < 4 ? 8 : sel == 4 ? 16 : 12;
  }
  if (op < 0x80) {
    const uint8_t o = fetch();
    switch (op) {
      case 0x78: wr(aN(o), A); return 8;
      case 0x79: wr(aN(o), B); return 8;
      case 0x7A: wr(aN(o), L); return 8;
      case 0x7B: wr(aN(o), H); return 8;
      case 0x7C: return 64;  // não documentada: não faz nada
      case 0x7D: wr(aN(o), rd(aHL())); return 16;
      case 0x7E: wr(aN(o), rd(aX())); return 16;
      default: wr(aN(o), rd(aY())); return 16;
    }
  }

  switch (op) {
    case 0x80: A = inc8(A); return 8;
    case 0x81: B = inc8(B); return 8;
    case 0x82: L = inc8(L); return 8;
    case 0x83: H = inc8(H); return 8;
    case 0x84: N = inc8(N); return 8;
    case 0x85: { const uint32_t a = aN(fetch()); wr(a, inc8(rd(a))); return 16; }
    case 0x86: { const uint32_t a = aHL(); wr(a, inc8(rd(a))); return 12; }
    case 0x87: SP = inc16(SP); return 8;
    case 0x88: A = dec8(A); return 8;
    case 0x89: B = dec8(B); return 8;
    case 0x8A: L = dec8(L); return 8;
    case 0x8B: H = dec8(H); return 8;
    case 0x8C: N = dec8(N); return 8;
    case 0x8D: { const uint32_t a = aN(fetch()); wr(a, dec8(rd(a))); return 16; }
    case 0x8E: { const uint32_t a = aHL(); wr(a, dec8(rd(a))); return 12; }
    case 0x8F: SP = dec16(SP); return 8;
    case 0x90: case 0x91: case 0x92: case 0x93: set16(op, inc16(get16(op))); return 8;
    case 0x94: logic(A & B); return 8;
    case 0x95: { const uint8_t v = fetch(); logic(rd(aHL()) & v); return 12; }
    case 0x96: logic(A & fetch()); return 8;
    case 0x97: logic(B & fetch()); return 8;
    case 0x98: case 0x99: case 0x9A: case 0x9B: set16(op, dec16(get16(op))); return 8;
    case 0x9C: F &= fetch(); flagsChanged(); return 12;
    case 0x9D: F |= fetch(); flagsChanged(); return 12;
    case 0x9E: F ^= fetch(); flagsChanged(); return 12;
    case 0x9F: F = fetch(); flagsChanged(); return 12;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
      const uint16_t v = get16(op);
      push(v >> 8);
      push(v & 0xFF);
      return 16;
    }
    case 0xA4: push(N); return 12;
    case 0xA5: push(I); return 12;
    case 0xA6: push(XI); push(YI); return 16;
    case 0xA7: push(F); return 12;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: {
      const uint8_t lo = pop();
      set16(op, uint16_t(lo | (pop() << 8)));
      return 12;
    }
    case 0xAC: N = pop(); return 8;
    case 0xAD: I = pop(); return 8;
    case 0xAE: YI = pop(); XI = pop(); return 12;
    case 0xAF: F = pop(); flagsChanged(); return 8;
    case 0xB0: A = fetch(); return 8;
    case 0xB1: B = fetch(); return 8;
    case 0xB2: L = fetch(); return 8;
    case 0xB3: H = fetch(); return 8;
    case 0xB4: N = fetch(); return 8;
    case 0xB5: { const uint8_t v = fetch(); wr(aHL(), v); return 12; }
    case 0xB6: { const uint8_t v = fetch(); wr(aX(), v); return 12; }
    case 0xB7: { const uint8_t v = fetch(); wr(aY(), v); return 12; }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: {
      const uint16_t a = fetch16();
      const uint8_t lo = rd(aAbs(a));
      set16(op, uint16_t(lo | (rd(aAbs(uint16_t(a + 1))) << 8)));
      return 20;
    }
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
      const uint16_t a = fetch16();
      const uint16_t v = get16(op);
      wr(aAbs(a), v & 0xFF);
      wr(aAbs(uint16_t(a + 1)), v >> 8);
      return 20;
    }
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: set16(op, add16(get16(op), fetch16(), 0)); return 12;
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: set16(op, fetch16()); return 12;
    case 0xC8: case 0xC9: case 0xCA: {
      const int r = (op & 3) + 1;
      const uint16_t t = get16(r);
      set16(r, BA());
      setBA(t);
      return 12;
    }
    case 0xCB: { const uint16_t t = SP; SP = BA(); setBA(t); return 12; }
    case 0xCC: { const uint8_t t = A; A = B; B = t; return 8; }
    case 0xCD: { const uint8_t t = rd(aHL()); wr(aHL(), A); A = t; return 12; }
    case 0xCE: return execCE();
    case 0xCF: return execCF();
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: set16(op, sub16(get16(op), fetch16(), 0)); return 12;
    case 0xD4: case 0xD5: case 0xD6: case 0xD7: sub16(get16(op), fetch16(), 0); return 12;
    case 0xD8: case 0xD9: case 0xDA: {
      const uint32_t a = aN(fetch());
      const uint8_t v = fetch();
      const uint8_t m = rd(a);
      wr(a, logic(op == 0xD8 ? (m & v) : op == 0xD9 ? (m | v) : (m ^ v)));
      return 20;
    }
    case 0xDB: { const uint32_t a = aN(fetch()); const uint8_t v = fetch(); sub8(rd(a), v, 0); return 16; }
    case 0xDC: { const uint32_t a = aN(fetch()); const uint8_t v = fetch(); logic(rd(a) & v); return 16; }
    case 0xDD: { const uint32_t a = aN(fetch()); const uint8_t v = fetch(); wr(a, v); return 16; }
    case 0xDE: A = uint8_t((A & 0x0F) | (B << 4)); return 8;
    case 0xDF: B = A >> 4; A &= 0x0F; return 8;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7: {
      const uint8_t o = fetch();
      const bool c = (op & 2) ? bool(F & FZ) : bool(F & FC);
      if (c != bool(op & 1)) {
        if (op & 4) { jumpRel(sx8(o)); return 8; }
        callRel(sx8(o));
        return 20;
      }
      return 8;
    }
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: case 0xEF: {
      const uint16_t o = fetch16();
      const bool c = (op & 2) ? bool(F & FZ) : bool(F & FC);
      if (c != bool(op & 1)) {
        if (op & 4) { jumpRel(o); return 12; }
        callRel(o);
        return 24;
      }
      return 12;
    }
    case 0xF0: callRel(sx8(fetch())); return 20;
    case 0xF1: jumpRel(sx8(fetch())); return 8;
    case 0xF2: callRel(fetch16()); return 24;
    case 0xF3: jumpRel(fetch16()); return 12;
    case 0xF4: V = U1; U2 = U1; PC = HL(); return 8;
    case 0xF5: {
      const uint8_t o = fetch();
      B = dec8(B);
      if (B) jumpRel(sx8(o));
      return 16;
    }
    case 0xF6: A = uint8_t((A << 4) | (A >> 4)); return 8;
    case 0xF7: { const uint32_t a = aHL(); const uint8_t v = rd(a); wr(a, uint8_t((v << 4) | (v >> 4))); return 12; }
    case 0xF8: ret(); return 16;
    case 0xF9: F = pop(); ret(); flagsChanged(); return 16;
    case 0xFA: ret(); PC = uint16_t(PC + 2); return 16;
    case 0xFB: {
      const uint16_t a = fetch16();
      push(V); push(PC >> 8); push(PC & 0xFF);
      V = U1; U2 = U1;
      PC = rd16(aAbs(a));
      return 20;
    }
    case 0xFC: enterIrq(fetch(), true); return 20;
    case 0xFD: enterIrq(fetch(), false); return 8;
    case 0xFF: return 8;
    default: return 4;  // FE: trava o aparelho de verdade; aqui não faz nada
  }
}

// ------------------------------------------------------------------ prefixo CE
int Minx::execCE() {
  const uint8_t op = fetch();
  // [X+ss], [Y+ss], [X+L], [Y+L]
  auto idx = [&](int sel) -> uint32_t {
    switch (sel & 3) {
      case 0: return aXo(int8_t(fetch()));
      case 1: return aYo(int8_t(fetch()));
      case 2: return aXo(int8_t(L));
      default: return aYo(int8_t(L));
    }
  };
  auto alu = [&](int o, uint8_t a, uint8_t b) -> uint8_t {
    switch (o) {
      case 0: return add8(a, b, 0);
      case 1: return add8(a, b, carry());
      case 2: case 6: return sub8(a, b, 0);
      case 3: return sub8(a, b, carry());
      case 4: return logic(a & b);
      case 5: return logic(a | b);
      default: return logic(a ^ b);
    }
  };

  if (op < 0x40) {
    const int o = op >> 3, sel = op & 7;
    if (sel < 4) {
      const uint8_t r = alu(o, A, rd(idx(sel)));
      if (o != 6) A = r;
      return 16;
    }
    uint8_t v;
    int cyc = 20;
    if (sel == 4) { v = A; cyc = 16; }
    else if (sel == 5) v = fetch();
    else v = rd(sel == 6 ? aX() : aY());
    const uint32_t a = aHL();
    const uint8_t r = alu(o, rd(a), v);
    if (o != 6) wr(a, r);
    return cyc;
  }
  if (op < 0x60) {
    uint8_t* reg[4] = {&A, &B, &L, &H};
    uint8_t& r = *reg[(op >> 3) & 3];
    const uint32_t a = idx(op);
    if (op & 4) wr(a, r);
    else r = rd(a);
    return 16;
  }
  if ((op >= 0x60 && op <= 0x63) || (op >= 0x68 && op <= 0x6B) || (op >= 0x78 && op <= 0x7B)) {
    const uint8_t v = rd(idx(op));
    wr(op < 0x64 ? aHL() : op < 0x6C ? aX() : aY(), v);
    return 20;
  }
  if (op >= 0x80 && op < 0xA0) {
    const int k = op & 3;
    if (k == 0) { A = shiftRot((op >> 2) & 7, A); return 12; }
    if (k == 1) { B = shiftRot((op >> 2) & 7, B); return 12; }
    const uint32_t a = k == 2 ? aN(fetch()) : aHL();
    wr(a, shiftRot((op >> 2) & 7, rd(a)));
    return k == 2 ? 20 : 16;
  }
  if (op >= 0xA0 && op < 0xA8) {
    const int k = op & 3;
    const bool isNeg = op & 4;
    if (k == 0) { A = isNeg ? neg(A) : notOp(A); return 12; }
    if (k == 1) { B = isNeg ? neg(B) : notOp(B); return 12; }
    const uint32_t a = k == 2 ? aN(fetch()) : aHL();
    const uint8_t v = rd(a);
    wr(a, isNeg ? neg(v) : notOp(v));
    return k == 2 ? 20 : 16;
  }
  if (op >= 0xE0) {
    const uint8_t o = fetch();
    if (cond(op)) {
      if (op < 0xF0) jumpRel(sx8(o));
      else callRel(sx8(o));
    }
    return 12;
  }

  switch (op) {
    // não documentadas (comportamento medido pelo PokeMini)
    case 0x64: setBA(add16(BA(), fetch16(), carry())); return 16;
    case 0x65: setHL(add16(HL(), fetch16(), carry())); return 16;
    case 0x66: { const uint8_t n = fetch(); setBA(add16(BA(), uint16_t((n << 8) | L), carry())); return 24; }
    case 0x67: { const uint8_t n = fetch(); setHL(add16(HL(), uint16_t((n << 8) | L), carry())); return 24; }
    case 0x6C:  // emenda na 6D, como no PokeMini
      SP = add16(SP, fetch(), 0);
      [[fallthrough]];
    case 0x6D: {
      const uint8_t n = fetch();
      setHL(add16(X, uint16_t(n * 48 + ((n & 8) >> 3)), 0));
      F &= ~FC;
      return 40;
    }
    case 0x6E: { const uint8_t n = fetch(); SP = add16(SP, uint16_t((n << 8) | L), 0); return 16; }
    case 0x6F: setHL(add16(X, uint16_t(L * 48 + ((L & 8) >> 3)), 0)); F &= ~FC; return 40;
    case 0x70: case 0x71: PC++; return 64;
    case 0x72: case 0x73: return 64;
    case 0x74: A = rd(idx(0)); return 64;
    case 0x75: L = rd(idx(1)); return 64;
    case 0x76: A = rd(idx(2)); return 64;
    case 0x77: L = rd(idx(3)); return 64;
    case 0x7C: PC++; return 20;
    case 0x7D: PC++; return 16;
    case 0x7E: return 20;
    case 0x7F: return 16;
    case 0xA8: setBA(sx8(A)); return 12;
    case 0xA9: return 8;
    case 0xAA: return 12;
    case 0xAB: case 0xAC: case 0xAD: return 64;
    case 0xAE: status = HALT; return 8;
    case 0xAF: status = STOP; return 8;
    case 0xB0: B = logic(B & fetch()); return 12;
    case 0xB1: L = logic(L & fetch()); return 12;
    case 0xB2: H = logic(H & fetch()); return 12;
    case 0xB3: H = V; return 12;
    case 0xB4: B = logic(B | fetch()); return 12;
    case 0xB5: L = logic(L | fetch()); return 12;
    case 0xB6: H = logic(H | fetch()); return 12;
    case 0xB7: X = uint16_t((X & 0xFF) | (V << 8)); return 12;
    case 0xB8: B = logic(B ^ fetch()); return 12;
    case 0xB9: L = logic(L ^ fetch()); return 12;
    case 0xBA: H = logic(H ^ fetch()); return 12;
    case 0xBB: Y = uint16_t((Y & 0xFF) | (V << 8)); return 12;
    case 0xBC: sub8(B, fetch(), 0); return 12;
    case 0xBD: sub8(L, fetch(), 0); return 12;
    case 0xBE: sub8(H, fetch(), 0); return 12;
    case 0xBF: sub8(N, fetch(), 0); return 12;
    case 0xC0: A = N; return 8;
    case 0xC1: A = F; return 8;
    case 0xC2: N = A; return 8;
    case 0xC3: F = A; flagsChanged(); return 8;
    case 0xC4: setU(fetch()); return 16;
    case 0xC5: I = fetch(); return 12;
    case 0xC6: XI = fetch(); return 12;
    case 0xC7: YI = fetch(); return 12;
    case 0xC8: A = V; return 8;
    case 0xC9: A = I; return 8;
    case 0xCA: A = XI; return 8;
    case 0xCB: A = YI; return 8;
    case 0xCC: setU(A); return 8;
    case 0xCD: I = A; return 8;
    case 0xCE: XI = A; return 8;
    case 0xCF: YI = A; return 8;
    // MOV r,[nnnn] / MOV [nnnn],r: a documentação dá o endereço sem o banco I
    case 0xD0: A = rd(fetch16()); return 20;
    case 0xD1: B = rd(fetch16()); return 20;
    case 0xD2: L = rd(fetch16()); return 20;
    case 0xD3: H = rd(fetch16()); return 20;
    case 0xD4: wr(fetch16(), A); return 20;
    case 0xD5: wr(fetch16(), B); return 20;
    case 0xD6: wr(fetch16(), L); return 20;
    case 0xD7: wr(fetch16(), H); return 20;
    case 0xD8: {  // MUL L, A
      const uint16_t r = uint16_t(L * A);
      setHL(r);
      F &= 0xF0;
      if (!r) F |= FZ;
      if (r & 0x8000) F |= FS;
      return 48;
    }
    case 0xD9: {  // DIV HL, A
      F &= 0xF0;
      if (A == 0) return 52;  // divisão por zero: o aparelho trava; aqui só limpa as flags
      const uint16_t hl = HL();
      const unsigned q = hl / A;
      if (q < 256) {
        H = uint8_t(hl % A);
        L = uint8_t(q);
        if (!L) F |= FZ;
        if (L & 0x80) F |= FS;
      } else {
        F |= FO | FS;
      }
      return 52;
    }
    case 0xDA: case 0xDB: case 0xDE: case 0xDF: fetch(); return 64;
    case 0xDC: return 64;
    case 0xDD: return 16;
    default: return 4;
  }
}

// ------------------------------------------------------------------ prefixo CF
int Minx::execCF() {
  const uint8_t op = fetch();
  auto get16 = [&](int r) -> uint16_t {
    switch (r & 3) { case 0: return BA(); case 1: return HL(); case 2: return X; default: return Y; }
  };
  auto set16 = [&](int r, uint16_t v) {
    switch (r & 3) { case 0: setBA(v); break; case 1: setHL(v); break; case 2: X = v; break; default: Y = v; break; }
  };

  if (op < 0x40) {
    // BA (00..1F) ou HL (20..3F) com BA/HL/X/Y; 10..17 e 30..37 repetem ADD/ADC; 1C..1F e 3C..3F são SBC BA
    const int src = op & 3;
    const bool hl = op & 0x20;
    int kind = (op >> 2) & 7;  // 0 ADD, 1 ADC, 2 SUB, 3 SBC, 4 ADD, 5 ADC, 6 CMP, 7 SBC
    const uint16_t s = get16(src);
    if (kind == 7) { setBA(sub16(BA(), s, carry())); return 16; }
    if (kind >= 4 && kind < 6) kind -= 4;
    const uint16_t d = hl ? HL() : BA();
    uint16_t r = 0;
    switch (kind) {
      case 0: r = add16(d, s, 0); break;
      case 1: r = add16(d, s, carry()); break;
      case 2: r = sub16(d, s, 0); break;
      case 3: r = sub16(d, s, carry()); break;
      default: sub16(d, s, 0); return 16;  // CMP
    }
    if (hl) setHL(r);
    else setBA(r);
    return 16;
  }
  if (op < 0x60) {
    const int k = op & 7;
    if (k >= 6) return 16;
    const bool sub = op & 8;
    const uint16_t s = (k & 1) ? HL() : BA();
    if (k >= 4) {
      if ((op & 0x18) == 0x18) sub16(SP, s, 0);  // 5C/5D: CMP SP
      else SP = sub ? sub16(SP, s, 0) : add16(SP, s, 0);
      return 16;
    }
    uint16_t& d = (k & 2) ? Y : X;
    d = sub ? sub16(d, s, 0) : add16(d, s, 0);
    return 16;
  }
  switch (op) {
    case 0x60: setBA(add16(BA(), fetch16(), carry())); return 16;
    case 0x61: setHL(add16(HL(), fetch16(), carry())); return 16;
    case 0x62: setBA(sub16(BA(), fetch16(), carry())); return 16;
    case 0x63: setHL(sub16(HL(), fetch16(), carry())); return 16;
    case 0x68: SP = add16(SP, fetch16(), 0); return 16;
    case 0x6A: SP = sub16(SP, fetch16(), 0); return 16;
    case 0x6C: sub16(SP, fetch16(), 0); return 16;
    case 0x6E: SP = fetch16(); return 16;
    case 0x70: case 0x71: case 0x72: case 0x73: {
      const uint16_t a = uint16_t(SP + sx8(fetch()));
      const uint8_t lo = rd(a);
      set16(op, uint16_t(lo | (rd(uint16_t(a + 1)) << 8)));
      return 24;
    }
    case 0x74: case 0x75: case 0x76: case 0x77: {
      const uint16_t a = uint16_t(SP + sx8(fetch()));
      const uint16_t v = get16(op);
      wr(a, v & 0xFF);
      wr(uint16_t(a + 1), v >> 8);
      return 24;
    }
    case 0x78: {
      const uint16_t a = fetch16();
      const uint8_t lo = rd(aAbs(a));
      SP = uint16_t(lo | (rd(aAbs(uint16_t(a + 1))) << 8));
      return 24;
    }
    case 0x7C: {
      const uint16_t a = fetch16();
      wr(aAbs(a), SP & 0xFF);
      wr(aAbs(uint16_t(a + 1)), SP >> 8);
      return 24;
    }
    case 0xB0: push(A); return 12;
    case 0xB1: push(B); return 12;
    case 0xB2: push(L); return 12;
    case 0xB3: push(H); return 12;
    case 0xB4: A = pop(); return 12;
    case 0xB5: B = pop(); return 12;
    case 0xB6: L = pop(); return 12;
    case 0xB7: H = pop(); return 12;
    case 0xB8: case 0xB9:  // PUSHA / PUSHAX
      push(B); push(A); push(H); push(L);
      push(X >> 8); push(X & 0xFF); push(Y >> 8); push(Y & 0xFF);
      push(N);
      if (op == 0xB8) return 48;
      push(I); push(XI); push(YI);
      return 60;
    case 0xBC: case 0xBD: {  // POPA / POPAX
      if (op == 0xBD) { YI = pop(); XI = pop(); I = pop(); }
      N = pop();
      Y = pop(); Y |= pop() << 8;
      X = pop(); X |= pop() << 8;
      L = pop(); H = pop();
      A = pop(); B = pop();
      return op == 0xBC ? 44 : 56;
    }
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: {
      const uint16_t a = HL();
      const uint8_t lo = rd((uint32_t(I) << 16) | a);
      set16(op, uint16_t(lo | (rd((uint32_t(I) << 16) | uint16_t(a + 1)) << 8)));
      return 20;
    }
    case 0xC4: case 0xC5: case 0xC6: case 0xC7: {
      const uint16_t a = HL();
      const uint16_t v = get16(op);
      wr((uint32_t(I) << 16) | a, v & 0xFF);
      wr((uint32_t(I) << 16) | uint16_t(a + 1), v >> 8);
      return 20;
    }
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: {
      const uint8_t bank = (op & 8) ? YI : XI;
      const uint16_t a = (op & 8) ? Y : X;
      const uint8_t lo = rd((uint32_t(bank) << 16) | a);
      set16(op, uint16_t(lo | (rd((uint32_t(bank) << 16) | uint16_t(a + 1)) << 8)));
      return 20;
    }
    case 0xD4: case 0xD5: case 0xD6: case 0xD7:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF: {
      const uint8_t bank = (op & 8) ? YI : XI;
      const uint16_t a = (op & 8) ? Y : X;
      const uint16_t v = get16(op);
      wr((uint32_t(bank) << 16) | a, v & 0xFF);
      wr((uint32_t(bank) << 16) | uint16_t(a + 1), v >> 8);
      return 20;
    }
    case 0xF0: case 0xF1: case 0xF2: case 0xF3: SP = get16(op); return 8;
    case 0xF4: setHL(SP); return 8;
    case 0xF5: setHL(PC); return 8;
    case 0xF6: X = uint16_t((X & 0xFF) | (V << 8)); return 12;
    case 0xF7: Y = uint16_t((Y & 0xFF) | (V << 8)); return 12;
    case 0xF8: setBA(SP); return 8;
    case 0xF9: setBA(PC); return 8;
    case 0xFA: X = SP; return 8;
    case 0xFB: case 0xFC: return 12;
    case 0xFD: A = E; return 12;
    case 0xFE: Y = SP; return 8;
    case 0xFF: return 64;
    default: break;
  }
  if (op >= 0xC8 && op <= 0xCF) { B = V; return 12; }
  if (op >= 0xE0 && op <= 0xEF) { set16(op >> 2, get16(op)); return 8; }
  if ((op >= 0x79 && op <= 0x7B) || (op >= 0x7D && op <= 0xAF) || op == 0xBA || op == 0xBB || op == 0xBE ||
      op == 0xBF)
    return execCFSpecial();
  return 16;  // 64..67, 69, 6B, 6D, 6F: instáveis, não fazem nada
}

// sub-tabela não documentada CF xx yy (comportamento medido pelo PokeMini, só para não divergir)
int Minx::execCFSpecial() {
  const uint8_t op = fetch();
  if (op < 0x40) {
    const bool hl = op & 0x20;
    const int k = (op >> 2) & 7;  // 0 DEC, 1 DEC se !C, 2 INC, 3 INC se !C, 4 DEC, 5 DEC se !C, 6 INC (só flags), 7
    const uint16_t v = hl ? HL() : BA();
    uint16_t r = v;
    bool store = true;
    switch (k) {
      case 0: case 4: r = dec16(v); break;
      case 1: case 5: if (F & FC) store = false; else r = dec16(v); break;
      case 2: r = inc16(v); break;
      case 6: inc16(v); store = false; break;
      default: if (F & FC) store = false; else r = inc16(v); break;
    }
    if (store) { if (hl) setHL(r); else setBA(r); }
    return 16;
  }
  if (op < 0x60) {
    const int k = op & 7;
    if (k >= 6) return 16;
    const bool inc = op & 8;
    if (k >= 4) SP = inc ? inc16(SP) : dec16(SP);
    else X = inc ? inc16(X) : dec16(X);  // o "Y" também mexe no X (como medido)
    return 16;
  }
  if (op < 0x70) return 16;
  if (op < 0x74) { setR16(op, uint16_t(0x4D00 | V)); return 24; }
  if (op < 0xB0) return 24;
  if (op < 0xC0) return 12;
  if (op < 0xE0) {
    const int k = op & 7;
    if (k >= 4) return 20;
    if ((op & 0xF8) == 0xC8) return 20;
    if (op == 0xD3) Y = V;
    else setR16(op, uint16_t(0x2000 | V));
    return 20;
  }
  return 24;
}

void Minx::sync(StateIO& io) {
  for (uint8_t* p : {&A, &B, &L, &H, &I, &N, &XI, &YI, &F, &E, &V, &U1, &U2, &shiftU, &IR, &status, &irqVector})
    io.sync(*p);
  io.sync(X); io.sync(Y); io.sync(SP); io.sync(PC);
  if (io.reading()) {
    if (shiftU > 2) shiftU = 2;
    if (status > IRQ) status = RUN;
  }
}

}  // namespace indigo::pm
