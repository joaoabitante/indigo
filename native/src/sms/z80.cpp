#include "z80.h"

namespace indigo {

namespace {
constexpr int FC = 0x01, FN = 0x02, FP = 0x04, F3 = 0x08, FH = 0x10, F5 = 0x20, FZ = 0x40, FS = 0x80;

struct Tables {
  uint8_t SZ53[256], SZ53P[256], PAR[256];
  Tables() {
    for (int v = 0; v < 256; v++) {
      int p = v; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
      PAR[v] = (p & 1) ? 0 : FP;
      SZ53[v] = (v & (FS | F3 | F5)) | (v == 0 ? FZ : 0);
      SZ53P[v] = SZ53[v] | PAR[v];
    }
  }
};
const Tables T;
const uint8_t* const SZ53 = T.SZ53;
const uint8_t* const SZ53P = T.SZ53P;
const uint8_t* const PAR = T.PAR;

// T-states das instruções sem prefixo (condicionais somam o extra no código)
const uint8_t CYC[256] = {
  4, 10, 7, 6, 4, 4, 7, 4, 4, 11, 7, 6, 4, 4, 7, 4,
  8, 10, 7, 6, 4, 4, 7, 4, 12, 11, 7, 6, 4, 4, 7, 4,
  7, 10, 16, 6, 4, 4, 7, 4, 7, 11, 16, 6, 4, 4, 7, 4,
  7, 10, 13, 6, 11, 11, 10, 4, 7, 11, 13, 6, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  5, 10, 10, 10, 10, 11, 7, 11, 5, 10, 10, 0, 10, 17, 7, 11,
  5, 10, 10, 11, 10, 11, 7, 11, 5, 4, 10, 11, 10, 0, 7, 11,
  5, 10, 10, 19, 10, 11, 7, 11, 5, 4, 10, 4, 10, 0, 7, 11,
  5, 10, 10, 4, 10, 11, 7, 11, 5, 6, 10, 4, 10, 0, 7, 11,
};
}  // namespace

void Z80::reset() {
  a = 0xFF; f = 0xFF; b = 0; c = 0; d = 0; e = 0; h = 0; l = 0;
  a2 = 0; f2 = 0; b2 = 0; c2 = 0; d2 = 0; e2 = 0; h2 = 0; l2 = 0;
  ix = 0xFFFF; iy = 0xFFFF; sp = 0xDFF0; pc = 0;
  i = 0; r = 0; wz = 0;
  iff1 = 0; iff2 = 0; im = 1; halted = false; eiDelay = false;
  irqLine = false; nmiPending = false;
}

// IRQ mascarável (nível): o VDP mantém a linha enquanto houver pedido pendente
int Z80::interrupt() {
  if (nmiPending) {
    nmiPending = false;
    halted = false;
    iff1 = 0;
    incR();
    push(pc); pc = 0x66;
    return 11;
  }
  if (!irqLine || !iff1 || eiDelay) return 0;
  halted = false;
  iff1 = iff2 = 0;
  incR();
  if (im == 2) {
    push(pc);
    pc = rd16((i << 8) | 0xFF);
    wz = pc;
    return 19;
  }
  push(pc); pc = 0x38; wz = 0x38;
  return 13;
}

int Z80::step() {
  const int t = interrupt();
  if (t) return t;
  eiDelay = false;
  if (halted) { incR(); return 4; }
  incR();
  const int op = imm();
  return exec(op, 0);
}

// registradores por índice (0 B,1 C,2 D,3 E,4 H,5 L,6 (HL),7 A); idx: 0 = HL, 1 = IX, 2 = IY
int Z80::getR(int n, int idx) const {
  switch (n) {
    case 0: return b; case 1: return c; case 2: return d; case 3: return e;
    case 4: return idx == 0 ? h : (idx == 1 ? ix : iy) >> 8;
    case 5: return idx == 0 ? l : (idx == 1 ? ix : iy) & 0xFF;
    case 7: return a;
  }
  return 0;
}

void Z80::setR(int n, int idx, int v) {
  v &= 0xFF;
  switch (n) {
    case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
    case 4: if (idx == 0) h = v; else if (idx == 1) ix = (ix & 0xFF) | (v << 8); else iy = (iy & 0xFF) | (v << 8); break;
    case 5: if (idx == 0) l = v; else if (idx == 1) ix = (ix & 0xFF00) | v; else iy = (iy & 0xFF00) | v; break;
    case 7: a = v; break;
  }
}

void Z80::setHL(int idx, int v) {
  v &= 0xFFFF;
  if (idx == 0) setHLr(v); else if (idx == 1) ix = v; else iy = v;
}

// endereço de (HL) ou (IX+d)/(IY+d); lê o deslocamento
int Z80::addrHL(int idx) {
  if (idx == 0) return hl();
  const int base = idx == 1 ? ix : iy;
  const int addr = (base + disp()) & 0xFFFF;
  wz = addr;
  return addr;
}

void Z80::setRR(int p, int idx, int v) {
  v &= 0xFFFF;
  if (p == 0) setBC(v); else if (p == 1) setDE(v); else if (p == 2) setHL(idx, v); else sp = v;
}

bool Z80::cond(int cc) const {
  switch (cc) {
    case 0: return !(f & FZ); case 1: return (f & FZ) != 0; case 2: return !(f & FC); case 3: return (f & FC) != 0;
    case 4: return !(f & FP); case 5: return (f & FP) != 0; case 6: return !(f & FS); default: return (f & FS) != 0;
  }
}

// ------------------------------------------------------------ ULA
void Z80::alu(int op, int v) {
  const int x = a;
  int res;
  switch (op) {
    case 0: case 1: {
      const int cy = op == 1 ? f & FC : 0;
      res = x + v + cy;
      f = SZ53[res & 0xFF] | ((x ^ v ^ res) & FH) | ((res >> 8) & 1) | (((x ^ ~v) & (x ^ res) & 0x80) >> 5);
      a = res & 0xFF; return;
    }
    case 2: case 3: case 7: {
      const int cy = op == 3 ? f & FC : 0;
      res = x - v - cy;
      const int fl = (SZ53[res & 0xFF] & ~(F3 | F5)) | FN | ((x ^ v ^ res) & FH) | ((res >> 8) & 1) | (((x ^ v) & (x ^ res) & 0x80) >> 5);
      if (op == 7) { f = fl | (v & (F3 | F5)); return; }
      f = fl | (res & (F3 | F5));
      a = res & 0xFF; return;
    }
    case 4: a = x & v; f = SZ53P[a] | FH; return;
    case 5: a = (x ^ v) & 0xFF; f = SZ53P[a]; return;
    case 6: a = (x | v) & 0xFF; f = SZ53P[a]; return;
  }
}

int Z80::inc8(int v) { const int res = (v + 1) & 0xFF; f = (f & FC) | SZ53[res] | ((res & 0xF) == 0 ? FH : 0) | (res == 0x80 ? FP : 0); return res; }
int Z80::dec8(int v) { const int res = (v - 1) & 0xFF; f = (f & FC) | FN | SZ53[res] | ((res & 0xF) == 0xF ? FH : 0) | (res == 0x7F ? FP : 0); return res; }

int Z80::add16(int x, int v) {
  const int res = x + v;
  wz = (x + 1) & 0xFFFF;
  f = (f & (FS | FZ | FP)) | ((res >> 8) & (F3 | F5)) | (((x ^ v ^ res) >> 8) & FH) | ((res >> 16) & 1);
  return res & 0xFFFF;
}

void Z80::adc16(int v) {
  const int x = hl(), res = x + v + (f & FC);
  wz = (x + 1) & 0xFFFF;
  f = ((res >> 8) & (FS | F3 | F5)) | ((res & 0xFFFF) ? 0 : FZ) | (((x ^ v ^ res) >> 8) & FH) | ((res >> 16) & 1) | ((((x ^ ~v) & (x ^ res)) >> 13) & FP);
  setHLr(res & 0xFFFF);
}

void Z80::sbc16(int v) {
  const int x = hl(), res = x - v - (f & FC);
  wz = (x + 1) & 0xFFFF;
  f = ((res >> 8) & (FS | F3 | F5)) | ((res & 0xFFFF) ? 0 : FZ) | FN | (((x ^ v ^ res) >> 8) & FH) | ((res >> 16) & 1) | ((((x ^ v) & (x ^ res)) >> 13) & FP);
  setHLr(res & 0xFFFF);
}

int Z80::rot(int op, int v) {
  int cy, res;
  switch (op) {
    case 0: cy = v >> 7; res = ((v << 1) | cy) & 0xFF; break;            // RLC
    case 1: cy = v & 1; res = (v >> 1) | (cy << 7); break;               // RRC
    case 2: cy = v >> 7; res = ((v << 1) | (f & FC)) & 0xFF; break;      // RL
    case 3: cy = v & 1; res = (v >> 1) | ((f & FC) << 7); break;         // RR
    case 4: cy = v >> 7; res = (v << 1) & 0xFF; break;                   // SLA
    case 5: cy = v & 1; res = (v >> 1) | (v & 0x80); break;              // SRA
    case 6: cy = v >> 7; res = ((v << 1) | 1) & 0xFF; break;             // SLL (não documentada)
    default: cy = v & 1; res = v >> 1; break;                            // SRL
  }
  f = SZ53P[res] | cy;
  return res;
}

void Z80::bit(int n, int v, int xy) {
  const int set = v & (1 << n);
  f = (f & FC) | FH | (xy & (F3 | F5)) | (set ? 0 : (FZ | FP)) | (n == 7 && set ? FS : 0);
}

void Z80::daa() {
  int x = a, t = 0, cy = f & FC;
  const int hf0 = f & FH, n = f & FN;
  if (hf0 || (x & 0xF) > 9) t++;
  if (cy || x > 0x99) { t += 2; cy = 1; }
  int hf;
  if (n && !hf0) hf = 0;
  else if (n && hf0) hf = (x & 0xF) < 6 ? FH : 0;
  else hf = (x & 0xF) >= 0xA ? FH : 0;
  switch (t) {
    case 1: x += n ? 0xFA : 0x06; break;
    case 2: x += n ? 0xA0 : 0x60; break;
    case 3: x += n ? 0x9A : 0x66; break;
  }
  x &= 0xFF;
  a = x;
  f = SZ53P[x] | hf | n | cy;
}

// ------------------------------------------------------------ execução (idx: 0 sem prefixo, 1 DD, 2 FD)
int Z80::exec(int op, int idx) {
  int t = CYC[op];
  if (idx) t += 4;
  const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

  if (x == 1) {
    if (op == 0x76) { halted = true; return t; }
    if (z == 6) { const int m = addrHL(idx); if (idx) t += 8; const int v = rd(m); setR(y, 0, v); return t; }
    if (y == 6) { const int m = addrHL(idx); if (idx) t += 8; wr(m, getR(z, 0)); return t; }
    setR(y, idx, getR(z, idx));
    return t;
  }
  if (x == 2) {
    if (z == 6) { const int m = addrHL(idx); if (idx) t += 8; const int v = rd(m); alu(y, v); return t; }
    alu(y, getR(z, idx));
    return t;
  }
  if (x == 0) {
    switch (z) {
      case 0:
        switch (y) {
          case 0: return t;
          case 1: {  // EX AF,AF'
            int v = a; a = a2; a2 = v; v = f; f = f2; f2 = v; return t;
          }
          case 2: { const int dd = disp(); b = (b - 1) & 0xFF; if (b) { pc = (pc + dd) & 0xFFFF; wz = pc; return t + 5; } return t; }
          case 3: { const int dd = disp(); pc = (pc + dd) & 0xFFFF; wz = pc; return t; }
          default: { const int dd = disp(); if (cond(y - 4)) { pc = (pc + dd) & 0xFFFF; wz = pc; return t + 5; } return t; }
        }
      case 1:
        if (!q) { const int v = imm16(); setRR(p, idx, v); return t; }
        setHL(idx, add16(getHL(idx), getRR(p, idx)));
        return t;
      case 2:
        switch (y) {
          case 0: wr(bc(), a); wz = ((bc() + 1) & 0xFF) | (a << 8); return t;
          case 1: a = rd(bc()); wz = (bc() + 1) & 0xFFFF; return t;
          case 2: wr(de(), a); wz = ((de() + 1) & 0xFF) | (a << 8); return t;
          case 3: a = rd(de()); wz = (de() + 1) & 0xFFFF; return t;
          case 4: { const int m = imm16(); wr16(m, getHL(idx)); wz = (m + 1) & 0xFFFF; return t; }
          case 5: { const int m = imm16(); const int v = rd16(m); setHL(idx, v); wz = (m + 1) & 0xFFFF; return t; }
          case 6: { const int m = imm16(); wr(m, a); wz = ((m + 1) & 0xFF) | (a << 8); return t; }
          default: { const int m = imm16(); a = rd(m); wz = (m + 1) & 0xFFFF; return t; }
        }
      case 3:
        setRR(p, idx, getRR(p, idx) + (q ? -1 : 1));
        return t;
      case 4: case 5: {
        if (y == 6) {
          const int m = addrHL(idx); if (idx) t += 8;
          const int v = rd(m);
          const int res = z == 4 ? inc8(v) : dec8(v);
          wr(m, res);
          return t;
        }
        const int v = getR(y, idx);
        setR(y, idx, z == 4 ? inc8(v) : dec8(v));
        return t;
      }
      case 6: {
        if (y == 6) { const int m = addrHL(idx); if (idx) t += 5; const int v = imm(); wr(m, v); return t; }
        const int v = imm();
        setR(y, idx, v);
        return t;
      }
      default:
        switch (y) {
          case 0: { const int cy = a >> 7; a = ((a << 1) | cy) & 0xFF; f = (f & (FS | FZ | FP)) | (a & (F3 | F5)) | cy; return t; }
          case 1: { const int cy = a & 1; a = (a >> 1) | (cy << 7); f = (f & (FS | FZ | FP)) | (a & (F3 | F5)) | cy; return t; }
          case 2: { const int cy = a >> 7; a = ((a << 1) | (f & FC)) & 0xFF; f = (f & (FS | FZ | FP)) | (a & (F3 | F5)) | cy; return t; }
          case 3: { const int cy = a & 1; a = (a >> 1) | ((f & FC) << 7); f = (f & (FS | FZ | FP)) | (a & (F3 | F5)) | cy; return t; }
          case 4: daa(); return t;
          case 5: a ^= 0xFF; f = (f & (FS | FZ | FP | FC)) | (a & (F3 | F5)) | FH | FN; return t;
          case 6: f = (f & (FS | FZ | FP)) | (a & (F3 | F5)) | FC; return t;
          default: f = ((f & (FS | FZ | FP)) | ((f & FC) ? FH : 0) | (a & (F3 | F5)) | ((f & FC) ^ FC)); return t;
        }
    }
  }
  // x == 3
  switch (z) {
    case 0: if (cond(y)) { pc = pop(); wz = pc; return t + 6; } return t;
    case 1:
      if (!q) {
        const int v = pop();
        if (p == 3) setAF(v); else setRR(p, idx, v);
        return t;
      }
      switch (p) {
        case 0: pc = pop(); wz = pc; return t;
        case 1: {  // EXX
          int v;
          v = b; b = b2; b2 = v; v = c; c = c2; c2 = v;
          v = d; d = d2; d2 = v; v = e; e = e2; e2 = v;
          v = h; h = h2; h2 = v; v = l; l = l2; l2 = v;
          return t;
        }
        case 2: pc = getHL(idx); return t;
        default: sp = getHL(idx); return t;
      }
    case 2: { const int m = imm16(); wz = m; if (cond(y)) pc = m; return t; }
    case 3:
      switch (y) {
        case 0: pc = imm16(); wz = pc; return t;
        case 1: return idx ? execIndexCB(idx) : execCB();
        case 2: { const int n = imm(); bus.out((a << 8) | n, a); wz = ((n + 1) & 0xFF) | (a << 8); return t; }
        case 3: { const int n = imm(); const int port = (a << 8) | n; a = bus.in(port); wz = (port + 1) & 0xFFFF; return t; }
        case 4: {  // EX (SP),HL
          const int v = rd16(sp); wr16(sp, getHL(idx)); setHL(idx, v); wz = v; return t;
        }
        case 5: { const int v = de(); setDE(hl()); setHLr(v); return t; }
        case 6: iff1 = iff2 = 0; return t;
        default: iff1 = iff2 = 1; eiDelay = true; return t;
      }
    case 4: { const int m = imm16(); wz = m; if (cond(y)) { push(pc); pc = m; return t + 7; } return t; }
    case 5:
      if (!q) { push(p == 3 ? af() : getRR(p, idx)); return t; }
      switch (p) {
        case 0: { const int m = imm16(); push(pc); pc = m; wz = m; return t; }
        case 1: { incR(); const int nop = imm(); return exec(nop, 1); }
        case 2: incR(); return execED();
        default: { incR(); const int nop = imm(); return exec(nop, 2); }
      }
    case 6: { const int v = imm(); alu(y, v); return t; }
    default: push(pc); pc = y * 8; wz = pc; return t;
  }
}

int Z80::execCB() {
  incR();
  const int op = imm();
  const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
  if (z == 6) {
    const int m = hl(), v = rd(m);
    if (x == 0) { const int res = rot(y, v); wr(m, res); return 15; }
    if (x == 1) { bit(y, v, wz >> 8); return 12; }
    wr(m, x == 2 ? v & ~(1 << y) : v | (1 << y));
    return 15;
  }
  const int v = getR(z, 0);
  if (x == 0) setR(z, 0, rot(y, v));
  else if (x == 1) bit(y, v, v);
  else setR(z, 0, x == 2 ? v & ~(1 << y) : v | (1 << y));
  return 8;
}

// DDCB/FDCB: deslocamento vem antes do opcode; o resultado também vai para o registrador (não documentado)
int Z80::execIndexCB(int idx) {
  const int base = idx == 1 ? ix : iy;
  const int m = (base + disp()) & 0xFFFF;
  wz = m;
  const int op = imm();
  const int x = op >> 6, y = (op >> 3) & 7, z = op & 7;
  const int v = rd(m);
  if (x == 1) { bit(y, v, m >> 8); return 20; }
  const int res = x == 0 ? rot(y, v) : x == 2 ? v & ~(1 << y) : v | (1 << y);
  wr(m, res);
  if (z != 6) setR(z, 0, res);
  return 23;
}

int Z80::execED() {
  const int op = imm();
  const int x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
  if (x == 1) {
    switch (z) {
      case 0: {  // IN r,(C)
        const int v = bus.in(bc()); wz = (bc() + 1) & 0xFFFF;
        if (y != 6) setR(y, 0, v);
        f = (f & FC) | SZ53P[v];
        return 12;
      }
      case 1: bus.out(bc(), y == 6 ? 0 : getR(y, 0)); wz = (bc() + 1) & 0xFFFF; return 12;
      case 2: if (q) adc16(getRR(p, 0)); else sbc16(getRR(p, 0)); return 15;
      case 3: {
        const int m = imm16(); wz = (m + 1) & 0xFFFF;
        if (q) { const int v = rd16(m); setRR(p, 0, v); } else wr16(m, getRR(p, 0));
        return 20;
      }
      case 4: { const int v = a; a = 0; alu(2, v); return 8; }  // NEG
      case 5: pc = pop(); wz = pc; iff1 = iff2; return 14;     // RETN/RETI
      case 6: { static const int IM[8] = {0, 0, 1, 2, 0, 0, 1, 2}; im = IM[y]; return 8; }
      default:
        switch (y) {
          case 0: i = a; return 9;
          case 1: r = a; return 9;
          case 2: a = i; f = (f & FC) | SZ53[a] | (iff2 ? FP : 0); return 9;
          case 3: a = r; f = (f & FC) | SZ53[a] | (iff2 ? FP : 0); return 9;
          case 4: {  // RRD
            const int m = hl(), v = rd(m);
            wr(m, ((a << 4) | (v >> 4)) & 0xFF);
            a = (a & 0xF0) | (v & 0x0F);
            f = (f & FC) | SZ53P[a]; wz = (m + 1) & 0xFFFF;
            return 18;
          }
          case 5: {  // RLD
            const int m = hl(), v = rd(m);
            wr(m, ((v << 4) | (a & 0x0F)) & 0xFF);
            a = (a & 0xF0) | (v >> 4);
            f = (f & FC) | SZ53P[a]; wz = (m + 1) & 0xFFFF;
            return 18;
          }
          default: return 8;
        }
    }
  }
  if (x == 2 && z <= 3 && y >= 4) return block(y, z);
  return 8;  // NOP estendido
}

// LDI/LDD/LDIR/LDDR, CPI…, INI…, OUTI…
int Z80::block(int y, int z) {
  const int dec = y & 1;
  const bool rep = y >= 6;
  const int dir = dec ? -1 : 1;
  if (z == 0) {  // LD
    const int v = rd(hl());
    wr(de(), v);
    setHLr(hl() + dir); setDE(de() + dir); setBC(bc() - 1);
    const int n = (v + a) & 0xFF;
    f = (f & (FS | FZ | FC)) | (bc() ? FP : 0) | (n & F3) | ((n & 0x02) << 4);
    if (rep && bc()) { pc = (pc - 2) & 0xFFFF; wz = (pc + 1) & 0xFFFF; return 21; }
    return 16;
  }
  if (z == 1) {  // CP
    const int v = rd(hl()), res = (a - v) & 0xFF;
    const int hf = (a ^ v ^ res) & FH;
    setHLr(hl() + dir); setBC(bc() - 1);
    const int n = (res - (hf ? 1 : 0)) & 0xFF;
    f = (f & FC) | FN | (SZ53[res] & (FS | FZ)) | hf | (bc() ? FP : 0) | (n & F3) | ((n & 0x02) << 4);
    wz = (wz + dir) & 0xFFFF;
    if (rep && bc() && res != 0) { pc = (pc - 2) & 0xFFFF; wz = (pc + 1) & 0xFFFF; return 21; }
    return 16;
  }
  if (z == 2) {  // IN
    const int v = bus.in(bc());
    wz = (bc() + dir) & 0xFFFF;
    wr(hl(), v);
    b = (b - 1) & 0xFF; setHLr(hl() + dir);
    const int k = v + ((c + dir) & 0xFF);
    f = SZ53[b] | ((v & 0x80) ? FN : 0) | (k > 255 ? (FH | FC) : 0) | PAR[(k & 7) ^ b];
    if (rep && b) { pc = (pc - 2) & 0xFFFF; return 21; }
    return 16;
  }
  // OUT
  const int v = rd(hl());
  b = (b - 1) & 0xFF;
  wz = (bc() + dir) & 0xFFFF;
  bus.out(bc(), v);
  setHLr(hl() + dir);
  const int k = v + l;
  f = SZ53[b] | ((v & 0x80) ? FN : 0) | (k > 255 ? (FH | FC) : 0) | PAR[(k & 7) ^ b];
  if (rep && b) { pc = (pc - 2) & 0xFFFF; return 21; }
  return 16;
}

void Z80::sync(StateIO& io) {
  for (int* p : {&a, &f, &b, &c, &d, &e, &h, &l, &a2, &f2, &b2, &c2, &d2, &e2, &h2, &l2, &ix, &iy, &sp, &pc,
                 &i, &r, &wz, &iff1, &iff2, &im})
    io.sync(*p);
  io.sync(halted); io.sync(eiDelay); io.sync(irqLine); io.sync(nmiPending);
  if (io.reading()) {
    for (int* p : {&a, &f, &b, &c, &d, &e, &h, &l, &a2, &f2, &b2, &c2, &d2, &e2, &h2, &l2, &i, &r}) *p &= 0xFF;
    for (int* p : {&ix, &iy, &sp, &pc, &wz}) *p &= 0xFFFF;
    iff1 &= 1; iff2 &= 1; clampTo(im, 0, 2);
  }
}

}  // namespace indigo
