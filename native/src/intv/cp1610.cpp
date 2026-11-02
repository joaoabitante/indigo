#include "cp1610.h"

namespace indigo::intv {

void CP1610::reset() {
  for (auto& x : r) x = 0;
  r[7] = 0x1000;  // a EXEC começa em $1000
  S = Z = O = C = I = false;
  D = 0;
  intr = 0;
  halted = false;
  lastPc = 0;
}

int CP1610::interrupt() {
  halted = false;
  wr(r[6], r[7]);
  r[6]++;
  r[7] = 0x1004;
  return 12;
}

uint16_t CP1610::add(uint16_t a, uint16_t b, int c) {
  const uint32_t s = uint32_t(a) + b + c;
  const uint16_t v = static_cast<uint16_t>(s);
  S = v & 0x8000;
  Z = v == 0;
  C = s & 0x10000;
  O = ((a ^ v) & ~(a ^ b)) & 0x8000;
  return v;
}

uint16_t CP1610::sub(uint16_t d, uint16_t s) {
  const uint32_t t = uint32_t(d) + (s ^ 0xFFFFu) + 1;
  const uint16_t v = static_cast<uint16_t>(t);
  S = v & 0x8000;
  Z = v == 0;
  C = t & 0x10000;
  O = ((d ^ v) & (d ^ s)) & 0x8000;
  return v;
}

// deslocamentos e rotações (só R0..R3; não aceitam interrupção). S vem do bit 15 do resultado nos
// deslocamentos à esquerda e do bit 7 nos à direita e no SWAP.
int CP1610::shift(uint16_t op) {
  const int reg = op & 3;
  const bool two = op & 4;
  const uint16_t v = r[reg];
  uint16_t x = 0;
  intr = 0;
  switch ((op >> 3) & 7) {
    case 0:  // SWAP
      x = two ? static_cast<uint16_t>((v & 0xFF) * 0x101) : static_cast<uint16_t>((v >> 8) | (v << 8));
      S = x & 0x80;
      break;
    case 1:  // SLL
      x = static_cast<uint16_t>(v << (two ? 2 : 1));
      S = x & 0x8000;
      break;
    case 2: {  // RLC (por 2: C e O entram embaixo)
      const bool c = C, o = O;
      if (two) { x = static_cast<uint16_t>((v << 2) | (c << 1) | o); C = v & 0x8000; O = v & 0x4000; }
      else { x = static_cast<uint16_t>((v << 1) | c); C = v & 0x8000; }
      S = x & 0x8000;
      break;
    }
    case 3:  // SLLC
      if (two) { x = static_cast<uint16_t>(v << 2); C = v & 0x8000; O = v & 0x4000; }
      else { x = static_cast<uint16_t>(v << 1); C = v & 0x8000; }
      S = x & 0x8000;
      break;
    case 4:  // SLR
      x = static_cast<uint16_t>(v >> (two ? 2 : 1));
      S = x & 0x80;
      break;
    case 5:  // SAR
      x = static_cast<uint16_t>(static_cast<int16_t>(v) >> (two ? 2 : 1));
      S = x & 0x80;
      break;
    case 6: {  // RRC
      const bool c = C, o = O;
      if (two) { x = static_cast<uint16_t>((v >> 2) | (c << 14) | (o << 15)); C = v & 1; O = v & 2; }
      else { x = static_cast<uint16_t>((v >> 1) | (c << 15)); C = v & 1; }
      S = x & 0x80;
      break;
    }
    default:  // SARC
      x = static_cast<uint16_t>(static_cast<int16_t>(v) >> (two ? 2 : 1));
      if (two) { C = v & 1; O = v & 2; }
      else C = v & 1;
      S = x & 0x80;
      break;
  }
  Z = x == 0;
  r[reg] = x;
  return two ? 8 : 6;
}

int CP1610::branch(uint16_t op) {
  const uint16_t disp = rd(r[7]);
  r[7]++;
  const uint16_t target = static_cast<uint16_t>((op & 0x20) ? r[7] + static_cast<uint16_t>(~disp) : r[7] + disp);
  const int cond = op & 0xF;
  bool t;
  if (op & 0x10) {
    t = cond == 0;  // BEXT: o Intellivision não liga nada às linhas externas (EF = 0)
  } else {
    switch (cond & 7) {
      case 0: t = true; break;
      case 1: t = C; break;
      case 2: t = O; break;
      case 3: t = !S; break;
      case 4: t = Z; break;
      case 5: t = S != O; break;
      case 6: t = Z || (S != O); break;
      default: t = S != C; break;
    }
    if (cond & 8) t = !t;
  }
  if (t) r[7] = target;
  return t ? 9 : 7;
}

// MVO, MVI, ADD, SUB, CMP, AND, XOR com endereço direto, indireto (R1..R6) ou imediato (@R7)
int CP1610::memOp(uint16_t op) {
  const int opc = (op >> 6) & 7, m = (op >> 3) & 7, reg = op & 7;
  const bool dbd = D != 0;
  if (opc == 1) {  // MVO: não aceita interrupção
    intr = 0;
    if (m == 0) {
      const uint16_t a = rd(r[7]);
      r[7]++;
      wr(a, r[reg]);
      return 11;
    }
    const uint16_t a = r[m];
    wr(a, r[reg]);
    if (m >= 4) r[m] = static_cast<uint16_t>(a + 1 + (m == 7 && dbd ? 1 : 0));
    return 9;
  }
  uint16_t v;
  int cyc;
  if (m == 0) {  // direto (SDBD não tem efeito)
    const uint16_t a = rd(r[7]);
    r[7]++;
    v = rd(a);
    cyc = 10;
  } else if (m == 6) {  // pilha: pré-decremento
    r[6]--;
    v = rd(r[6]);
    cyc = 11;
  } else {
    uint16_t a = r[m];
    v = rd(a);
    if (m >= 4) a++;
    if (dbd) {  // dois bytes: o menos significativo primeiro
      v = static_cast<uint16_t>((v & 0xFF) | ((rd(a) & 0xFF) << 8));
      if (m >= 4) a++;
    }
    if (m >= 4) r[m] = a;
    cyc = dbd ? 10 : 8;
  }
  switch (opc) {
    case 2: r[reg] = v; break;
    case 3: r[reg] = add(r[reg], v); break;
    case 4: r[reg] = sub(r[reg], v); break;
    case 5: sub(r[reg], v); break;
    case 6: r[reg] &= v; setSZ(r[reg]); break;
    default: r[reg] ^= v; setSZ(r[reg]); break;
  }
  return cyc + (reg >= 6 ? 1 : 0);
}

int CP1610::step() {
  if (halted) { intr = 2 | (I ? 1 : 0); return 4; }
  const uint16_t pc = r[7];
  lastPc = pc;
  const uint16_t op = rd(pc) & 0x3FF;
  r[7] = static_cast<uint16_t>(pc + 1);
  intr = 2 | (I ? 1 : 0);
  int cyc;
  if (op >= 0x240) {
    cyc = memOp(op);
  } else if (op >= 0x200) {
    cyc = branch(op);
  } else if (op >= 0x080) {  // registro, registro
    const int s = (op >> 3) & 7, d = op & 7;
    const uint16_t a = r[s], b = r[d];
    switch (op >> 6) {
      case 2: r[d] = a; setSZ(a); break;  // MOVR (TSTR quando s == d)
      case 3: r[d] = add(b, a); break;     // ADDR
      case 4: r[d] = sub(b, a); break;     // SUBR
      case 5: sub(b, a); break;            // CMPR
      case 6: r[d] = a & b; setSZ(r[d]); break;  // ANDR
      default: r[d] = a ^ b; setSZ(r[d]); break;  // XORR (CLRR quando s == d)
    }
    cyc = d >= 6 ? 7 : 6;
  } else if (op >= 0x040) {
    cyc = shift(op);
  } else if (op >= 0x008) {
    const int reg = op & 7;
    switch (op >> 3) {
      case 1: r[reg]++; setSZ(r[reg]); break;                          // INCR
      case 2: r[reg]--; setSZ(r[reg]); break;                          // DECR
      case 3: r[reg] = static_cast<uint16_t>(~r[reg]); setSZ(r[reg]); break;  // COMR
      case 4: r[reg] = sub(0, r[reg]); break;                          // NEGR
      case 5: r[reg] = add(r[reg], 0, C ? 1 : 0); break;               // ADCR
      case 6:
        if (reg < 4) {  // GSWD
          const uint16_t f = static_cast<uint16_t>((S << 7) | (Z << 6) | (O << 5) | (C << 4));
          r[reg] = static_cast<uint16_t>(f | (f << 8));
          intr = 0;
        }
        // NOP (4, 5) e SIN (6, 7): nada
        break;
      default: {  // RSWD
        const uint16_t f = r[reg];
        S = f & 0x80; Z = f & 0x40; O = f & 0x20; C = f & 0x10;
        intr = 0;
        break;
      }
    }
    cyc = (op >> 3) == 7 || ((op >> 3) == 6) ? 6 : (reg >= 6 ? 7 : 6);
  } else {
    switch (op) {
      case 0: halted = true; cyc = 4; break;  // HLT: para até uma interrupção (volta depois do HLT) ou o reset
      case 1: D = 2; intr = 0; cyc = 4; break;           // SDBD
      case 2: I = true; intr = 0; cyc = 4; break;        // EIS
      case 3: I = false; intr = 0; cyc = 4; break;       // DIS
      case 4: {  // J / JSR / JE / JD / JSRE / JSRD (três palavras)
        const uint16_t w2 = rd(r[7]), w3 = rd(static_cast<uint16_t>(r[7] + 1));
        r[7] = static_cast<uint16_t>(r[7] + 2);
        const int reg = (w2 >> 8) & 3, mode = w2 & 3;
        const uint16_t dst = static_cast<uint16_t>(((w2 & 0xFC) << 8) | (w3 & 0x3FF));
        if (reg != 3) r[4 + reg] = r[7];
        if (mode) { I = mode & 1; intr = static_cast<uint8_t>(mode & 1); }
        r[7] = dst;
        cyc = 12;
        break;
      }
      case 5: intr = 0; cyc = 4; break;   // TCI
      case 6: C = false; intr = 0; cyc = 4; break;  // CLRC
      default: C = true; intr = 0; cyc = 4; break;  // SETC
    }
  }
  D >>= 1;
  return cyc;
}

void CP1610::sync(StateIO& io) {
  for (auto& x : r) io.sync(x);
  io.sync(S); io.sync(Z); io.sync(O); io.sync(C); io.sync(I);
  io.sync(D); io.sync(intr); io.sync(halted);
  if (io.reading()) { D &= 3; intr &= 3; }
}

}  // namespace indigo::intv
