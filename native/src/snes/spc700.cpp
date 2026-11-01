// SPC700: as 256 instruções com os ciclos na ordem do chip.
#include "spc700.h"

namespace indigo::snes {

void SPC700::sync(StateIO& io) {
  io.sync(pc); io.sync(a); io.sync(x); io.sync(y); io.sync(sp); io.sync(psw); io.sync(halted);
}

uint8_t SPC700::adc(uint8_t l, uint8_t r) {
  const int s = l + r + (psw & C);
  psw = static_cast<uint8_t>(psw & ~(C | V | H));
  if (s > 0xFF) psw |= C;
  if (~(l ^ r) & (l ^ s) & 0x80) psw |= V;
  if ((l ^ r ^ s) & 0x10) psw |= H;
  setZN(static_cast<uint8_t>(s));
  return static_cast<uint8_t>(s);
}

uint8_t SPC700::alu(int kind, uint8_t l, uint8_t r) {
  switch (kind) {
    case 0: l |= r; setZN(l); return l;
    case 1: l &= r; setZN(l); return l;
    case 2: l ^= r; setZN(l); return l;
    case 3: {
      const int t = l - r;
      psw = static_cast<uint8_t>((psw & ~C) | (t >= 0 ? C : 0));
      setZN(static_cast<uint8_t>(t));
      return l;
    }
    case 4: return adc(l, r);
    default: return adc(l, static_cast<uint8_t>(~r));
  }
}

uint8_t SPC700::shift(int kind, uint8_t v) {
  switch (kind) {
    case 0: psw = static_cast<uint8_t>((psw & ~C) | (v >> 7)); v = static_cast<uint8_t>(v << 1); break;
    case 1: { const int c = psw & C; psw = static_cast<uint8_t>((psw & ~C) | (v >> 7)); v = static_cast<uint8_t>((v << 1) | c); break; }
    case 2: psw = static_cast<uint8_t>((psw & ~C) | (v & 1)); v >>= 1; break;
    case 3: { const int c = psw & C; psw = static_cast<uint8_t>((psw & ~C) | (v & 1)); v = static_cast<uint8_t>((v >> 1) | (c << 7)); break; }
    case 4: v--; break;
    default: v++; break;
  }
  setZN(v);
  return v;
}

void SPC700::branch(bool cond) {
  const int8_t rel = static_cast<int8_t>(fetch());
  if (!cond) return;
  idle();
  idle();
  pc = static_cast<uint16_t>(pc + rel);
}

void SPC700::step() {
  if (halted) { dummy(); idle(); return; }
  execute(fetch());
}

void SPC700::execute(uint8_t op) {
  const int lo = op & 0x0F, row = op >> 5;
  const bool odd = op & 0x10;

  // ---------------- operações de A com memória (linhas 0–5: OR AND EOR CMP ADC SBC; 6: MOV mem,A; 7: MOV A,mem)
  if (lo >= 4 && lo <= 9 && !(row >= 6 && lo >= 8)) {
    const int kind = row;
    if (lo == 8 && !odd) { a = alu(kind, a, fetch()); return; }  // A op #imm
    if (lo == 8) {                                                // d op #imm
      const uint8_t imm = fetch(), off = fetch();
      const uint8_t v = ld(off);
      if (kind == 3) { alu(kind, v, imm); idle(); }
      else st(off, alu(kind, v, imm));
      return;
    }
    if (lo == 9 && !odd) {  // dd op ds
      const uint8_t so = fetch();
      const uint8_t sv = ld(so);
      const uint8_t dof = fetch();
      const uint8_t dv = ld(dof);
      if (kind == 3) { alu(kind, dv, sv); idle(); }
      else st(dof, alu(kind, dv, sv));
      return;
    }
    if (lo == 9) {  // (X) op (Y)
      dummy();
      const uint8_t sv = ld(y);
      const uint8_t dv = ld(x);
      if (kind == 3) { alu(kind, dv, sv); idle(); }
      else st(x, alu(kind, dv, sv));
      return;
    }
    uint16_t addr = 0;
    const bool store = row == 6;
    switch (lo | (odd ? 0x10 : 0)) {
      case 0x04: addr = dp(fetch()); break;
      case 0x14: { const uint8_t o = fetch(); idle(); addr = dp(static_cast<uint8_t>(o + x)); break; }
      case 0x05: addr = fetch16(); break;
      case 0x15: { const uint16_t w = fetch16(); idle(); addr = static_cast<uint16_t>(w + x); break; }
      case 0x06: dummy(); addr = dp(x); break;
      case 0x16: { const uint16_t w = fetch16(); idle(); addr = static_cast<uint16_t>(w + y); break; }
      case 0x07: {
        const uint8_t o = fetch();
        idle();
        const uint8_t pl = ld(static_cast<uint8_t>(o + x)), ph = ld(static_cast<uint8_t>(o + x + 1));
        addr = static_cast<uint16_t>(pl | (ph << 8));
        break;
      }
      default: {  // 0x17: [d]+Y
        const uint8_t o = fetch();
        if (!store) idle();
        const uint8_t pl = ld(o), ph = ld(static_cast<uint8_t>(o + 1));
        if (store) idle();
        addr = static_cast<uint16_t>((pl | (ph << 8)) + y);
        break;
      }
    }
    if (store) { rd(addr); wr(addr, a); return; }
    const uint8_t v = rd(addr);
    if (row == 7) { a = v; setZN(a); }
    else a = alu(kind, a, v);
    return;
  }

  switch (op) {
    // ---------------- desvios e flags (coluna 0)
    case 0x00: dummy(); break;
    case 0x10: branch(!(psw & N)); break;
    case 0x30: branch(psw & N); break;
    case 0x50: branch(!(psw & V)); break;
    case 0x70: branch(psw & V); break;
    case 0x90: branch(!(psw & C)); break;
    case 0xB0: branch(psw & C); break;
    case 0xD0: branch(!(psw & Z)); break;
    case 0xF0: branch(psw & Z); break;
    case 0x2F: branch(true); break;
    case 0x20: dummy(); psw &= ~P; break;
    case 0x40: dummy(); psw |= P; break;
    case 0x60: dummy(); psw &= ~C; break;
    case 0x80: dummy(); psw |= C; break;
    case 0xA0: dummy(); idle(); psw |= I; break;
    case 0xC0: dummy(); idle(); psw &= ~I; break;
    case 0xE0: dummy(); psw &= ~(V | H); break;
    case 0xED: dummy(); idle(); psw ^= C; break;

    // ---------------- TCALL (coluna 1)
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xA1: case 0xB1: case 0xC1: case 0xD1: case 0xE1: case 0xF1: {
      dummy();
      idle();
      push(static_cast<uint8_t>(pc >> 8));
      push(static_cast<uint8_t>(pc));
      idle();
      const uint16_t vec = static_cast<uint16_t>(0xFFDE - 2 * (op >> 4));
      const uint8_t l = rd(vec);
      pc = static_cast<uint16_t>(l | (rd(static_cast<uint16_t>(vec + 1)) << 8));
      break;
    }

    // ---------------- SET1 / CLR1 (coluna 2) e BBS / BBC (coluna 3)
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xA2: case 0xC2: case 0xE2:
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xB2: case 0xD2: case 0xF2: {
      const uint8_t o = fetch();
      const uint8_t v = ld(o);
      const uint8_t bit = static_cast<uint8_t>(1 << (op >> 5));
      st(o, odd ? static_cast<uint8_t>(v & ~bit) : static_cast<uint8_t>(v | bit));
      break;
    }
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xA3: case 0xC3: case 0xE3:
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xB3: case 0xD3: case 0xF3: {
      const uint8_t o = fetch();
      const uint8_t v = ld(o);
      idle();
      const bool set = v & (1 << (op >> 5));
      branch(odd ? !set : set);
      break;
    }

    // ---------------- linhas 6 e 7 das colunas 8 e 9
    case 0xC8: alu(3, x, fetch()); break;
    case 0xD8: { const uint8_t o = fetch(); ld(o); st(o, x); break; }
    case 0xC9: { const uint16_t w = fetch16(); rd(w); wr(w, x); break; }
    case 0xD9: { const uint8_t o = fetch(); idle(); const uint16_t ad = dp(static_cast<uint8_t>(o + y)); rd(ad); wr(ad, x); break; }
    case 0xE8: a = fetch(); setZN(a); break;
    case 0xF8: x = ld(fetch()); setZN(x); break;
    case 0xE9: x = rd(fetch16()); setZN(x); break;
    case 0xF9: { const uint8_t o = fetch(); idle(); x = ld(static_cast<uint8_t>(o + y)); setZN(x); break; }

    // ---------------- operações de bit com endereço de 13 bits (coluna A, linhas pares)
    case 0x0A: case 0x2A: case 0x4A: case 0x6A: case 0x8A: case 0xAA: case 0xCA: case 0xEA: {
      const uint16_t w = fetch16();
      const uint16_t addr = w & 0x1FFF;
      const int bit = w >> 13;
      const uint8_t v = rd(addr);
      const bool b = (v >> bit) & 1;
      const bool c = psw & C;
      switch (op) {
        case 0x0A: idle(); psw = static_cast<uint8_t>((psw & ~C) | ((c || b) ? C : 0)); break;
        case 0x2A: idle(); psw = static_cast<uint8_t>((psw & ~C) | ((c || !b) ? C : 0)); break;
        case 0x4A: psw = static_cast<uint8_t>((psw & ~C) | ((c && b) ? C : 0)); break;
        case 0x6A: psw = static_cast<uint8_t>((psw & ~C) | ((c && !b) ? C : 0)); break;
        case 0x8A: idle(); psw = static_cast<uint8_t>((psw & ~C) | ((c != b) ? C : 0)); break;
        case 0xAA: psw = static_cast<uint8_t>((psw & ~C) | (b ? C : 0)); break;
        case 0xCA: idle(); wr(addr, static_cast<uint8_t>(c ? (v | (1 << bit)) : (v & ~(1 << bit)))); break;
        default: wr(addr, static_cast<uint8_t>(v ^ (1 << bit))); break;
      }
      break;
    }

    // ---------------- palavras de 16 bits (coluna A, linhas ímpares)
    case 0x1A: case 0x3A: {  // DECW / INCW
      const uint8_t o = fetch();
      int v = ld(o) + (op == 0x3A ? 1 : -1);
      st(o, static_cast<uint8_t>(v));
      v += ld(static_cast<uint8_t>(o + 1)) << 8;
      st(static_cast<uint8_t>(o + 1), static_cast<uint8_t>(v >> 8));
      setZN16(static_cast<uint16_t>(v));
      break;
    }
    case 0x5A: {  // CMPW YA,d
      const uint8_t o = fetch();
      const uint8_t l = ld(o), h = ld(static_cast<uint8_t>(o + 1));
      const int ya = (y << 8) | a, w = l | (h << 8);
      const int t = ya - w;
      psw = static_cast<uint8_t>((psw & ~C) | (t >= 0 ? C : 0));
      setZN16(static_cast<uint16_t>(t));
      break;
    }
    case 0x7A: case 0x9A: {  // ADDW / SUBW
      const uint8_t o = fetch();
      const uint8_t l = ld(o);
      idle();
      const uint8_t h = ld(static_cast<uint8_t>(o + 1));
      if (op == 0x7A) {
        psw &= ~C;
        a = adc(a, l);
        y = adc(y, h);
      } else {
        psw |= C;
        a = adc(a, static_cast<uint8_t>(~l));
        y = adc(y, static_cast<uint8_t>(~h));
      }
      setZN16(static_cast<uint16_t>((y << 8) | a));
      break;
    }
    case 0xBA: {  // MOVW YA,d
      const uint8_t o = fetch();
      a = ld(o);
      idle();
      y = ld(static_cast<uint8_t>(o + 1));
      setZN16(static_cast<uint16_t>((y << 8) | a));
      break;
    }
    case 0xDA: {  // MOVW d,YA
      const uint8_t o = fetch();
      ld(o);
      st(o, a);
      st(static_cast<uint8_t>(o + 1), y);
      break;
    }
    case 0xFA: {  // MOV dd,ds
      const uint8_t so = fetch();
      const uint8_t v = ld(so);
      const uint8_t dof = fetch();
      st(dof, v);
      break;
    }

    // ---------------- deslocamentos e incrementos (coluna B e C)
    case 0x0B: case 0x2B: case 0x4B: case 0x6B: case 0x8B: case 0xAB: {
      static const int K[6] = {0, 1, 2, 3, 4, 5};
      const uint8_t o = fetch();
      st(o, shift(K[op >> 5], ld(o)));
      break;
    }
    case 0x1B: case 0x3B: case 0x5B: case 0x7B: case 0x9B: case 0xBB: {
      const uint8_t o = fetch();
      idle();
      const uint8_t o2 = static_cast<uint8_t>(o + x);
      st(o2, shift(op >> 5, ld(o2)));
      break;
    }
    case 0x0C: case 0x2C: case 0x4C: case 0x6C: case 0x8C: case 0xAC: {
      const uint16_t w = fetch16();
      wr(w, shift(op >> 5, rd(w)));
      break;
    }
    case 0x1C: case 0x3C: case 0x5C: case 0x7C: case 0x9C: case 0xBC:
      dummy();
      a = shift(op >> 5, a);
      break;
    case 0xDC: dummy(); y--; setZN(y); break;
    case 0xFC: dummy(); y++; setZN(y); break;
    case 0x1D: dummy(); x--; setZN(x); break;
    case 0x3D: dummy(); x++; setZN(x); break;
    case 0xCB: { const uint8_t o = fetch(); ld(o); st(o, y); break; }
    case 0xDB: { const uint8_t o = fetch(); idle(); const uint8_t o2 = static_cast<uint8_t>(o + x); ld(o2); st(o2, y); break; }
    case 0xEB: y = ld(fetch()); setZN(y); break;
    case 0xFB: { const uint8_t o = fetch(); idle(); y = ld(static_cast<uint8_t>(o + x)); setZN(y); break; }
    case 0xCC: { const uint16_t w = fetch16(); rd(w); wr(w, y); break; }
    case 0xEC: y = rd(fetch16()); setZN(y); break;

    // ---------------- pilha e transferências (coluna D)
    case 0x0D: dummy(); push(psw); idle(); break;
    case 0x2D: dummy(); push(a); idle(); break;
    case 0x4D: dummy(); push(x); idle(); break;
    case 0x6D: dummy(); push(y); idle(); break;
    case 0x8E: dummy(); idle(); psw = pull(); break;
    case 0xAE: dummy(); idle(); a = pull(); break;
    case 0xCE: dummy(); idle(); x = pull(); break;
    case 0xEE: dummy(); idle(); y = pull(); break;
    case 0x8D: y = fetch(); setZN(y); break;
    case 0xAD: alu(3, y, fetch()); break;
    case 0xCD: x = fetch(); setZN(x); break;
    case 0x5D: dummy(); x = a; setZN(x); break;
    case 0x7D: dummy(); a = x; setZN(a); break;
    case 0x9D: dummy(); x = sp; setZN(x); break;
    case 0xBD: dummy(); sp = x; break;
    case 0xDD: dummy(); a = y; setZN(a); break;
    case 0xFD: dummy(); y = a; setZN(y); break;

    // ---------------- coluna E
    case 0x0E: case 0x4E: {  // TSET1 / TCLR1
      const uint16_t w = fetch16();
      const uint8_t v = rd(w);
      setZN(static_cast<uint8_t>(a - v));
      rd(w);
      wr(w, op == 0x0E ? static_cast<uint8_t>(v | a) : static_cast<uint8_t>(v & ~a));
      break;
    }
    case 0x2E: { const uint8_t o = fetch(); const uint8_t v = ld(o); idle(); branch(a != v); break; }  // CBNE d
    case 0xDE: {  // CBNE d+X
      const uint8_t o = fetch();
      idle();
      const uint8_t v = ld(static_cast<uint8_t>(o + x));
      idle();
      branch(a != v);
      break;
    }
    case 0x6E: {  // DBNZ d
      const uint8_t o = fetch();
      const uint8_t v = static_cast<uint8_t>(ld(o) - 1);
      st(o, v);
      branch(v != 0);
      break;
    }
    case 0xFE: dummy(); idle(); y--; branch(y != 0); break;  // DBNZ Y
    case 0x1E: alu(3, x, rd(fetch16())); break;
    case 0x3E: alu(3, x, ld(fetch())); break;
    case 0x5E: alu(3, y, rd(fetch16())); break;
    case 0x7E: alu(3, y, ld(fetch())); break;
    case 0x9E: {  // DIV YA,X
      dummy();
      for (int k = 0; k < 10; k++) idle();
      const unsigned ya = (y << 8) | a;
      psw = static_cast<uint8_t>(psw & ~(H | V));
      if ((y & 15) >= (x & 15)) psw |= H;
      if (y >= x) psw |= V;
      if (y < (x << 1)) {
        a = static_cast<uint8_t>(ya / x);
        y = static_cast<uint8_t>(ya % x);
      } else {
        a = static_cast<uint8_t>(255 - (ya - (x << 9)) / (256 - x));
        y = static_cast<uint8_t>(x + (ya - (x << 9)) % (256 - x));
      }
      setZN(a);
      break;
    }
    case 0xBE: {  // DAS
      dummy();
      idle();
      if (!(psw & C) || a > 0x99) { a = static_cast<uint8_t>(a - 0x60); psw &= ~C; }
      if (!(psw & H) || (a & 15) > 9) a = static_cast<uint8_t>(a - 6);
      setZN(a);
      break;
    }
    case 0xDF: {  // DAA
      dummy();
      idle();
      if ((psw & C) || a > 0x99) { a = static_cast<uint8_t>(a + 0x60); psw |= C; }
      if ((psw & H) || (a & 15) > 9) a = static_cast<uint8_t>(a + 6);
      setZN(a);
      break;
    }

    // ---------------- coluna F
    case 0x0F: {  // BRK
      dummy();
      push(static_cast<uint8_t>(pc >> 8));
      push(static_cast<uint8_t>(pc));
      push(psw);
      idle();
      const uint8_t l = rd(0xFFDE);
      pc = static_cast<uint16_t>(l | (rd(0xFFDF) << 8));
      psw = static_cast<uint8_t>((psw | B) & ~I);
      break;
    }
    case 0x1F: {  // JMP [!a+X]
      const uint16_t w = fetch16();
      idle();
      const uint16_t ad = static_cast<uint16_t>(w + x);
      const uint8_t l = rd(ad);
      pc = static_cast<uint16_t>(l | (rd(static_cast<uint16_t>(ad + 1)) << 8));
      break;
    }
    case 0x3F: {  // CALL
      const uint16_t w = fetch16();
      idle();
      push(static_cast<uint8_t>(pc >> 8));
      push(static_cast<uint8_t>(pc));
      idle();
      idle();
      pc = w;
      break;
    }
    case 0x4F: {  // PCALL
      const uint8_t o = fetch();
      idle();
      push(static_cast<uint8_t>(pc >> 8));
      push(static_cast<uint8_t>(pc));
      idle();
      pc = static_cast<uint16_t>(0xFF00 | o);
      break;
    }
    case 0x5F: pc = fetch16(); break;
    case 0x6F: { dummy(); idle(); const uint8_t l = pull(); pc = static_cast<uint16_t>(l | (pull() << 8)); break; }
    case 0x7F: { dummy(); idle(); psw = pull(); const uint8_t l = pull(); pc = static_cast<uint16_t>(l | (pull() << 8)); break; }
    case 0x8F: { const uint8_t imm = fetch(), o = fetch(); ld(o); st(o, imm); break; }
    case 0x9F: dummy(); idle(); idle(); idle(); a = static_cast<uint8_t>((a >> 4) | (a << 4)); setZN(a); break;  // XCN
    case 0xAF: dummy(); idle(); st(x, a); x++; break;  // MOV (X)+,A
    case 0xBF: dummy(); a = ld(x); idle(); x++; setZN(a); break;  // MOV A,(X)+
    case 0xCF: {  // MUL YA
      dummy();
      for (int k = 0; k < 7; k++) idle();
      const uint16_t r = static_cast<uint16_t>(y * a);
      a = static_cast<uint8_t>(r);
      y = static_cast<uint8_t>(r >> 8);
      setZN(y);
      break;
    }
    case 0xEF: case 0xFF: dummy(); idle(); halted = true; break;  // SLEEP / STOP
    default: dummy(); break;
  }
}

}  // namespace indigo::snes
