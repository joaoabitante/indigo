#include "tlcs900h.h"

#include <algorithm>
#include <cstring>

namespace indigo::ngp {

namespace {
constexpr int FS = 0x80, FZ = 0x40, FH = 0x10, FV = 0x04, FN = 0x02, FC = 0x01;
const uint32_t MASK[5] = {0, 0xFF, 0xFFFF, 0, 0xFFFFFFFFu};
const uint32_t SIGN[5] = {0, 0x80, 0x8000, 0, 0x80000000u};

// modos de endereçamento e estados extras de cada um (folha de dados)
enum Mode { M_NONE, M_R, M_RD8, M_N, M_NN, M_NNN, M_R32, M_R32D16, M_R32R8, M_R32R16, M_PRE, M_POST, M_LDAR };
const int MODE_X[] = {0, 0, 2, 2, 2, 3, 3, 5, 8, 8, 3, 3, 5};

struct Tables {
  uint8_t PAR[256];
  uint16_t REG_CYC[256] = {};
  Tables() {
    for (int v = 0; v < 256; v++) { int p = v; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1; PAR[v] = (p & 1) ? 0 : FV; }
    // ciclos das operações sobre registrador (byte/palavra no byte baixo, 32 bits no alto); 0 = calculado
    auto set1 = [&](int o, int bw, int l) { REG_CYC[o] = static_cast<uint16_t>(bw | (l << 8)); };
    auto setr = [&](int a, int n, int bw, int l) { for (int i = 0; i < n; i++) set1(a + i, bw, l); };
    set1(0x03, 4, 6); set1(0x04, 5, 7); set1(0x05, 6, 8); set1(0x06, 4, 4); set1(0x07, 5, 5);
    set1(0x0C, 10, 10); set1(0x0D, 8, 8); set1(0x0E, 4, 4); set1(0x0F, 4, 4); set1(0x10, 6, 6); set1(0x12, 4, 4);
    set1(0x13, 5, 5); set1(0x14, 4, 4); set1(0x16, 4, 4); set1(0x19, 31, 31);
    setr(0x20, 5, 4, 4); setr(0x28, 5, 4, 4); set1(0x2E, 8, 8); set1(0x2F, 8, 8); setr(0x30, 4, 4, 4); set1(0x34, 6, 6);
    setr(0x38, 3, 8, 8); setr(0x3C, 3, 7, 7); setr(0x60, 8, 4, 4); setr(0x68, 8, 4, 5); setr(0x70, 16, 6, 6);
    for (int b : {0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0}) setr(b, 8, 4, 7);
    setr(0x88, 8, 4, 4); setr(0x98, 8, 4, 4); setr(0xA8, 8, 4, 4); setr(0xD8, 8, 4, 4);
    setr(0xB8, 8, 5, 5); setr(0xC8, 8, 4, 7);
  }
};
const Tables T;
inline int parity16(uint32_t v) { return T.PAR[v & 0xFF] == T.PAR[(v >> 8) & 0xFF] ? FV : 0; }

const int MD_CYC[4] = {18, 18, 22, 24};  // MUL MULS DIV DIVS (8 bits; +8 em 16 bits)
const int BREG[8] = {0xE1, 0xE0, 0xE5, 0xE4, 0xE9, 0xE8, 0xED, 0xEC};  // W A B C D E H L
const int WREG[8] = {0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC};  // WA BC DE HL IX IY IZ SP

inline int32_t s8(uint32_t v) { return static_cast<int8_t>(v); }
inline int32_t s16(uint32_t v) { return static_cast<int16_t>(v); }
}  // namespace

void TLCS900H::reset() {
  std::memset(r, 0, sizeof r);
  pc = 0; sr = 0xF800; fd = 0;
  intNest = 0; halted = false;
  std::memset(dmaS, 0, sizeof dmaS); std::memset(dmaD, 0, sizeof dmaD);
  std::memset(dmaC, 0, sizeof dmaC); std::memset(dmaM, 0, sizeof dmaM);
}

// ------------------------------------------------------------ registradores por código completo
int TLCS900H::idx(int c) const {
  if (c < 0x40) return c >> 2;                                   // bancos 0–3 explícitos
  if (c >= 0xF0) return 16 + ((c >> 2) & 3);                     // XIX XIY XIZ XSP
  if (c >= 0xE0) return rfp() * 4 + ((c >> 2) & 3);              // banco atual
  if (c >= 0xD0) return rfp() ? (rfp() - 1) * 4 + ((c >> 2) & 3) : 20;  // banco anterior
  return 20;
}

uint32_t TLCS900H::getB(int c) const {
  const int i = idx(c);
  return (r[i] >> (i == 20 ? 0 : (c & 3) << 3)) & 0xFF;
}
void TLCS900H::setB(int c, int64_t v) {
  const int i = idx(c);
  const int s = i == 20 ? 0 : (c & 3) << 3;
  r[i] = (r[i] & ~(0xFFu << s)) | (static_cast<uint32_t>(v & 0xFF) << s);
}
uint32_t TLCS900H::getW(int c) const {
  const int i = idx(c);
  return ((c & 2) && i != 20) ? r[i] >> 16 : r[i] & 0xFFFF;
}
void TLCS900H::setW(int c, int64_t v) {
  const int i = idx(c);
  const uint32_t w = static_cast<uint32_t>(v & 0xFFFF);
  if ((c & 2) && i != 20) r[i] = (r[i] & 0xFFFF) | (w << 16);
  else r[i] = (r[i] & 0xFFFF0000u) | w;
}

// ------------------------------------------------------------ memória
uint32_t TLCS900H::rd(int64_t a, int sz) {
  const uint32_t ad = static_cast<uint32_t>(a) & 0xFFFFFF;
  return sz == 1 ? bus.read8(ad) : sz == 2 ? bus.read16(ad) : bus.read32(ad);
}
void TLCS900H::wr(int64_t a, int sz, int64_t v) {
  const uint32_t ad = static_cast<uint32_t>(a) & 0xFFFFFF;
  const uint32_t x = static_cast<uint32_t>(v);
  if (sz == 1) bus.write8(ad, x & 0xFF);
  else if (sz == 2) bus.write16(ad, x & 0xFFFF);
  else bus.write32(ad, x);
}
uint32_t TLCS900H::f8() { const uint32_t v = bus.read8(pc & 0xFFFFFF); pc = (pc + 1) & 0xFFFFFF; return v; }
uint32_t TLCS900H::f16() { const uint32_t v = bus.read16(pc & 0xFFFFFF); pc = (pc + 2) & 0xFFFFFF; return v; }
uint32_t TLCS900H::f32() { const uint32_t v = bus.read32(pc & 0xFFFFFF); pc = (pc + 4) & 0xFFFFFF; return v; }

bool TLCS900H::cond(int cc) const {
  const int f = sr;
  const int s = (f >> 7) & 1, z = (f >> 6) & 1, v = (f >> 2) & 1, c = f & 1;
  switch (cc & 15) {
    case 0: return false; case 1: return (s ^ v) == 1; case 2: return ((s ^ v) | z) == 1; case 3: return (c | z) == 1;
    case 4: return v == 1; case 5: return s == 1; case 6: return z == 1; case 7: return c == 1;
    case 8: return true; case 9: return (s ^ v) == 0; case 10: return ((s ^ v) | z) == 0; case 11: return (c | z) == 0;
    case 12: return v == 0; case 13: return s == 0; case 14: return z == 0; default: return c == 0;
  }
}

// ------------------------------------------------------------ ULA
int TLCS900H::szFlags(uint32_t v, int sz) const {
  return ((v & SIGN[sz]) ? FS : 0) | ((v & MASK[sz]) == 0 ? FZ : 0);
}

uint32_t TLCS900H::add(uint32_t a, uint32_t b, int sz, bool carry) {
  const uint32_t m = MASK[sz];
  const int c = carry ? (sr & FC) : 0;
  const int64_t rr = int64_t(a) + b + c;
  const uint32_t res = static_cast<uint32_t>(rr) & m;
  int f = szFlags(res, sz);
  if (sz < 4 ? rr > m : rr > 0xFFFFFFFFLL) f |= FC;
  if (((a ^ ~b) & (a ^ res) & SIGN[sz]) != 0) f |= FV;
  if (sz != 4 ? ((a ^ b ^ res) & 0x10) : (sr & FH)) f |= FH;
  setFlags(f);
  return res;
}

uint32_t TLCS900H::sub(uint32_t a, uint32_t b, int sz, bool carry) {
  const uint32_t m = MASK[sz];
  const int c = carry ? (sr & FC) : 0;
  const int64_t rr = int64_t(a) - b - c;
  const uint32_t res = static_cast<uint32_t>(rr) & m;
  int f = szFlags(res, sz) | FN;
  if (rr < 0) f |= FC;
  if (((a ^ b) & (a ^ res) & SIGN[sz]) != 0) f |= FV;
  if (sz != 4 ? ((a ^ b ^ res) & 0x10) : (sr & FH)) f |= FH;
  setFlags(f);
  return res;
}

uint32_t TLCS900H::logic(int op, uint32_t a, uint32_t b, int sz) {
  uint32_t res = op == 0 ? a & b : op == 1 ? a ^ b : a | b;
  res &= MASK[sz];
  int f = szFlags(res, sz) | (op == 0 ? FH : 0);
  if (sz == 1) f |= T.PAR[res]; else if (sz == 2) f |= parity16(res); else f |= sr & FV;
  setFlags(f);
  return res;
}

// operação aritmética/lógica por número (0 ADD,1 ADC,2 SUB,3 SBC,4 AND,5 XOR,6 OR,7 CP)
uint32_t TLCS900H::alu(int op, uint32_t a, uint32_t b, int sz) {
  switch (op) {
    case 0: return add(a, b, sz, false);
    case 1: return add(a, b, sz, true);
    case 2: return sub(a, b, sz, false);
    case 3: return sub(a, b, sz, true);
    case 4: return logic(0, a, b, sz);
    case 5: return logic(1, a, b, sz);
    case 6: return logic(2, a, b, sz);
    default: sub(a, b, sz, false); return a;
  }
}

uint32_t TLCS900H::inc(uint32_t v, uint32_t n, int sz, bool flags) {
  const uint32_t res = static_cast<uint32_t>(int64_t(v) + n) & MASK[sz];
  if (flags) {
    int f = (sr & FC) | szFlags(res, sz);
    if (((v ^ res) & ~(v ^ n) & SIGN[sz]) != 0 && ((res & SIGN[sz]) != (v & SIGN[sz]))) f |= FV;
    if (sz == 1 && ((v ^ n ^ res) & 0x10)) f |= FH;
    if (sz == 2 && ((v ^ n ^ res) & 0x10)) f |= FH;
    setFlags(f);
  }
  return res;
}

uint32_t TLCS900H::dec(uint32_t v, uint32_t n, int sz, bool flags) {
  const uint32_t res = static_cast<uint32_t>(int64_t(v) - n) & MASK[sz];
  if (flags) {
    int f = (sr & FC) | szFlags(res, sz) | FN;
    if (((v ^ n) & (v ^ res) & SIGN[sz]) != 0) f |= FV;
    if (sz != 4 && ((v ^ n ^ res) & 0x10)) f |= FH;
    setFlags(f);
  }
  return res;
}

// rotações/deslocamentos: op 0 RLC,1 RRC,2 RL,3 RR,4 SLA,5 SRA,6 SLL,7 SRL
uint32_t TLCS900H::shift(int op, uint32_t v, int n, int sz) {
  const uint32_t m = MASK[sz], sign = SIGN[sz];
  uint32_t c = sr & FC;
  v &= m;
  for (int k = 0; k < n; k++) {
    switch (op) {
      case 0: c = (v & sign) ? 1 : 0; v = ((v << 1) | c) & m; break;
      case 1: c = v & 1; v = (v >> 1) | (c ? sign : 0); break;
      case 2: { const uint32_t nc = (v & sign) ? 1 : 0; v = ((v << 1) | c) & m; c = nc; break; }
      case 3: { const uint32_t nc = v & 1; v = (v >> 1) | (c ? sign : 0); c = nc; break; }
      case 4: case 6: c = (v & sign) ? 1 : 0; v = (v << 1) & m; break;
      case 5: c = v & 1; v = (v >> 1) | (v & sign); break;
      default: c = v & 1; v = v >> 1; break;
    }
  }
  int f = szFlags(v, sz) | static_cast<int>(c);
  if (sz == 1) f |= T.PAR[v]; else if (sz == 2) f |= parity16(v);
  else f |= sr & FV;  // 32 bits: a paridade não é calculada (V fica como estava)
  setFlags(f);
  return v;
}

// ------------------------------------------------------------ execução
int TLCS900H::step() {
  if (halted) return 8;
  cycles = 0;
  op2 = -1; mode = M_NONE;
  const int op = static_cast<int>(f8());
  exec(op);
  return cycles;
}

void TLCS900H::exec(int op) {
  if (op >= 0x80) { prefixed(op); return; }
  const int hi = op >> 3, n = op & 7;
  switch (hi) {
    case 0x00: case 0x01: case 0x02: case 0x03: single(op); return;
    case 0x04: { const uint32_t v = f8(); setB(BREG[n], v); cycles = 2; return; }          // LD R,n
    case 0x05: push(2, getW(WREG[n])); cycles = 3; return;                                  // PUSH RR
    case 0x06: { const uint32_t v = f16(); setW(WREG[n], v); cycles = 3; return; }         // LD RR,nn
    case 0x07: push(4, getL(WREG[n])); cycles = 5; return;                                  // PUSH XRR
    case 0x08: { const uint32_t v = f32(); setL(WREG[n], v); cycles = 5; return; }         // LD XRR,nnnn
    case 0x09: { const uint32_t v = pop(2); setW(WREG[n], v); cycles = 4; return; }        // POP RR
    case 0x0A: cycles = 2; return;                                                          // (indefinido)
    case 0x0B: { const uint32_t v = pop(4); setL(WREG[n], v); cycles = 6; return; }        // POP XRR
    case 0x0C: case 0x0D: {                                                                 // JR cc,d
      const int32_t d = s8(f8());
      if (cond(op & 15)) { pc = static_cast<uint32_t>(int64_t(pc) + d) & 0xFFFFFF; cycles = 8; } else cycles = 4;
      return;
    }
    default: {                                                                              // JRL cc,dd
      const int32_t d = s16(f16());
      if (cond(op & 15)) { pc = static_cast<uint32_t>(int64_t(pc) + d) & 0xFFFFFF; cycles = 8; } else cycles = 4;
    }
  }
}

void TLCS900H::single(int op) {
  switch (op) {
    case 0x00: cycles = 2; return;                                          // NOP
    case 0x01: cycles = 4; return;                                          // NORMAL (900H: nada)
    case 0x02: push(2, sr); cycles = 4; return;                             // PUSH SR
    case 0x03: sr = static_cast<int>(pop(2)); cycles = 6; return;           // POP SR
    case 0x04: cycles = 4; return;                                          // MAX (nada)
    case 0x05: halted = true; cycles = 8; return;                           // HALT
    case 0x06: { const int n = f8() & 7; sr = (sr & 0x8FFF) | (n << 12); cycles = 5; return; }  // EI n
    case 0x07: {                                                            // RETI
      sr = static_cast<int>(pop(2));
      pc = pop(4) & 0xFFFFFF;
      intNest = (intNest - 1) & 0xFFFF;
      cycles = 12; return;
    }
    case 0x08: { const uint32_t a = f8(); const uint32_t v = f8(); wr(a, 1, v); cycles = 5; return; }   // LD (n),n
    case 0x09: push(1, f8()); cycles = 4; return;                                                       // PUSH n
    case 0x0A: { const uint32_t a = f8(); const uint32_t v = f16(); wr(a, 2, v); cycles = 6; return; }  // LDW (n),nn
    case 0x0B: push(2, f16()); cycles = 5; return;                                                      // PUSHW nn
    case 0x0C: sr = (sr & 0xF8FF) | (((rfp() + 1) & 3) << 8); cycles = 2; return;                       // INCF
    case 0x0D: sr = (sr & 0xF8FF) | (((rfp() - 1) & 3) << 8); cycles = 2; return;                       // DECF
    case 0x0E: pc = pop(4) & 0xFFFFFF; cycles = 9; return;                                              // RET
    case 0x0F: { const int32_t d = s16(f16()); pc = pop(4) & 0xFFFFFF; setXsp(int64_t(xsp()) + d); cycles = 9; return; }  // RETD
    case 0x10: setFlags(f() & ~(FN | FV | FC)); cycles = 2; return;                                     // RCF
    case 0x11: setFlags((f() & ~(FH | FN)) | FC); cycles = 2; return;                                   // SCF
    case 0x12: setFlags((f() & ~FN) ^ FC); cycles = 2; return;                                          // CCF
    case 0x13: { const int z = (f() & FZ) ? 1 : 0; setFlags((f() & ~(FN | FC)) | (z ^ 1)); cycles = 2; return; }  // ZCF
    case 0x14: push(1, getB(0xE0)); cycles = 3; return;                                                 // PUSH A
    case 0x15: { const uint32_t v = pop(1); setB(0xE0, v); cycles = 4; return; }                        // POP A
    case 0x16: { const int t = f(); setF(fd); fd = t; cycles = 2; return; }                             // EX F,F'
    case 0x17: { const int n = f8() & 3; sr = (sr & 0xF8FF) | (n << 8); cycles = 2; return; }           // LDF n
    case 0x18: push(1, f()); cycles = 3; return;                                                        // PUSH F
    case 0x19: setF(static_cast<int>(pop(1))); cycles = 4; return;                                      // POP F
    case 0x1A: pc = f16(); cycles = 7; return;                                                          // JP nn
    case 0x1B: pc = f24(); cycles = 7; return;                                                          // JP nnn
    case 0x1C: { const uint32_t a = f16(); push(4, pc); pc = a; cycles = 12; return; }                  // CALL nn
    case 0x1D: { const uint32_t a = f24(); push(4, pc); pc = a; cycles = 12; return; }                  // CALL nnn
    case 0x1E: { const int32_t d = s16(f16()); push(4, pc); pc = static_cast<uint32_t>(int64_t(pc) + d) & 0xFFFFFF; cycles = 12; return; }  // CALR
    default:  // 0x1F: chamada da BIOS própria (tratada pelo sistema)
      cycles = 2;
      if (onBios) onBios();
  }
}

// ------------------------------------------------------------ instruções com prefixo
void TLCS900H::prefixed(int op) {
  if (op >= 0xF8) {  // SWI n
    const int n = op & 7;
    push(4, pc); push(2, sr);
    pc = rd(0xFFFF00 + n * 4, 4) & 0xFFFFFF;
    cycles = 16; return;
  }
  const int grp = op >> 4;
  // prefixos de registrador
  if ((op & 0xF8) == 0xC8) { regOp(BREG[op & 7], 1, op); return; }
  if ((op & 0xF8) == 0xD8) { regOp(WREG[op & 7], 2, op); return; }
  if ((op & 0xF8) == 0xE8) { regOp(WREG[op & 7], 4, op); return; }
  if (op == 0xC7) { const int c = static_cast<int>(f8()); regOp(c, 1, op); cycles++; return; }
  if (op == 0xD7) { const int c = static_cast<int>(f8()); regOp(c, 2, op); cycles++; return; }
  if (op == 0xE7) { const int c = static_cast<int>(f8()); regOp(c, 4, op); cycles++; return; }
  if (op == 0xF7) {  // LDX (n),#
    f8(); const uint32_t a = f8(); f8(); const uint32_t v = f8(); f8();
    wr(a, 1, v); cycles = 8; return;
  }
  // memória: fonte (80–AF, C0–C5, D0–D5, E0–E5) ou destino (B0–BF, F0–F5)
  int sz;
  bool dst = false;
  int64_t addr;
  mode = M_NONE;
  if (op < 0xC0) {
    const int xr = WREG[op & 7];
    if (grp >= 0xB) dst = true;
    sz = grp == 8 ? 1 : grp == 9 ? 2 : 4;
    addr = getL(xr);
    mode = M_R;
    if (op & 8) { addr = addr + s8(f8()); mode = M_RD8; }
  } else {
    const int m = op & 7;
    sz = grp == 0xC ? 1 : grp == 0xD ? 2 : 4;
    if (grp == 0xF) dst = true;
    addr = memMode(m, sz);
  }
  const uint32_t a = static_cast<uint32_t>(addr) & 0xFFFFFF;
  cycles = 0;
  if (dst) dstOp(a); else srcOp(a, sz, op);
  cycles += MODE_X[mode];
}

// endereço dos modos C0–C5 (lê os bytes de endereço; aplica pré-decremento/pós-incremento)
int64_t TLCS900H::memMode(int m, int sz) {
  switch (m) {
    case 0: mode = M_N; return f8();
    case 1: mode = M_NN; return f16();
    case 2: mode = M_NNN; return f24();
    case 3: {
      const int b = static_cast<int>(f8());
      switch (b & 3) {
        case 0: mode = M_R32; return getL(b);
        case 1: { mode = M_R32D16; const int64_t base = getL(b & 0xFC); return base + s16(f16()); }
        default: {
          if (b == 0x03) { mode = M_R32R8; const int rl = static_cast<int>(f8()); const int rb = static_cast<int>(f8()); return int64_t(getL(rl)) + s8(getB(rb)); }
          if (b == 0x07) { mode = M_R32R16; const int rl = static_cast<int>(f8()); const int rw = static_cast<int>(f8()); return int64_t(getL(rl)) + s16(getW(rw)); }
          if (b == 0x13) { mode = M_LDAR; const int32_t d = s16(f16()); return int64_t(pc) + d; }  // LDAR
          return 0;
        }
      }
    }
    case 4: {  // (-r32)
      mode = M_PRE;
      const int b = static_cast<int>(f8());
      static const int STEP[4] = {1, 2, 4, 0};
      const uint32_t v = getL(b & 0xFC) - STEP[b & 3];
      setL(b & 0xFC, v);
      return v;
    }
    case 5: {  // (r32+)
      mode = M_POST;
      const int b = static_cast<int>(f8());
      static const int STEP[4] = {1, 2, 4, 0};
      const uint32_t v = getL(b & 0xFC);
      setL(b & 0xFC, int64_t(v) + STEP[b & 3]);
      return v;
    }
    default: return 0;
  }
}

// ------------------------------------------------------------ operações com fonte em memória
void TLCS900H::srcOp(uint32_t a, int sz, int prefix) {
  const int op = static_cast<int>(f8());
  op2 = op;
  const int R = op & 7;
  const int rcode = sz == 1 ? BREG[R] : WREG[R];
  if (op >= 0x80) {  // ALU R,(mem) / ALU (mem),R
    const int al = (op >> 4) & 7;
    const bool toMem = (op & 8) != 0;
    const uint32_t m = rd(a, sz);
    if (!toMem) { const uint32_t res = alu(al, getR(rcode, sz), m, sz); if (al != 7) setR(rcode, sz, res); cycles += sz == 4 ? 6 : 4; }
    else { const uint32_t res = alu(al, m, getR(rcode, sz), sz); if (al != 7) wr(a, sz, res); cycles += sz == 4 && al != 7 ? 10 : 6; }
    return;
  }
  if (op >= 0x78) {  // RLC…SRL (mem), 1 bit
    if (sz == 4) { cycles += 6; return; }  // não existe em 32 bits
    const int hn = sr & (FH | FN), k = op & 7;
    const uint32_t m = rd(a, sz);
    const uint32_t v = shift(k, m, 1, sz);
    if (k == 2 || k == 3) setFlags((f() & ~(FH | FN)) | hn);  // RL/RR (mem) não mexem em H e N
    wr(a, sz, v); cycles += 8; return;
  }
  if (op >= 0x60) {  // INC/DEC #3,(mem)
    if (sz == 4) { cycles += 2; return; }
    const uint32_t n = R ? R : 8;
    const uint32_t v = rd(a, sz);
    const uint32_t res = op < 0x68 ? inc(v, n, sz, true) : dec(v, n, sz, true);
    wr(a, sz, res);
    cycles += 6; return;
  }
  if (op >= 0x40) {  // MUL/MULS/DIV/DIVS RR,(mem)
    const int kind = (op >> 3) & 3;
    if (sz == 4 || (sz == 1 && !(R & 1))) { cycles += 2; return; }
    const uint32_t m = rd(a, sz);
    muldiv(kind, sz == 1 ? WREG[R >> 1] : WREG[R], m, sz);
    cycles += MD_CYC[kind] + (sz == 2 ? 8 : 0); return;
  }
  if (op >= 0x30 && sz == 4) { cycles += 2; return; }  // ALU (mem),# e EX não existem em 32 bits
  if (op >= 0x38) {  // ALU (mem),#
    const uint32_t imm = fimm(sz);
    const int al = op & 7;
    const uint32_t m = rd(a, sz);
    const uint32_t res = alu(al, m, imm, sz);
    if (al != 7) wr(a, sz, res);
    cycles += al == 7 ? 6 : sz == 1 ? 7 : 8; return;
  }
  if (op >= 0x30) {  // EX (mem),R
    const uint32_t m = rd(a, sz), rv = getR(rcode, sz);
    wr(a, sz, rv); setR(rcode, sz, m); cycles += 6; return;
  }
  if (op >= 0x20) {  // LD R,(mem)
    const uint32_t m = rd(a, sz);
    setR(rcode, sz, m); cycles += sz == 4 ? 6 : 4; return;
  }
  switch (op) {
    case 0x04: if (sz != 4) { const uint32_t m = rd(a, sz); push(sz, m); } cycles += 7; return;  // PUSH (mem)
    case 0x06: case 0x07: {                                                                       // RLD/RRD A,(mem)
      const uint32_t m = rd(a, 1), A = getB(0xE0);
      uint32_t nm, na;
      if (op == 0x06) { nm = ((m << 4) | (A & 0xF)) & 0xFF; na = (A & 0xF0) | (m >> 4); }
      else { nm = ((A << 4) | (m >> 4)) & 0xFF; na = (A & 0xF0) | (m & 0xF); }
      wr(a, 1, nm); setB(0xE0, na);
      setFlags((f() & FC) | szFlags(na, 1) | T.PAR[na]);
      cycles += 12; return;
    }
    case 0x10: case 0x11: case 0x12: case 0x13: if (sz == 4) { cycles += 2; return; } blockLd(prefix, sz, op); return;
    case 0x14: case 0x15: case 0x16: case 0x17: if (sz == 4) { cycles += 2; return; } blockCp(prefix, sz, op); return;
    case 0x19: {  // LD (nn),(mem)
      if (sz == 4) { cycles += 2; return; }
      const uint32_t d = f16();
      const uint32_t m = rd(a, sz);
      wr(d, sz, m); cycles += 8; return;
    }
    default: cycles += 2;
  }
}

void TLCS900H::blockLd(int prefix, int sz, int op) {
  // LDI/LDIR/LDD/LDDR: (XDE),(XHL) ou (XIX),(XIY) conforme o registrador do prefixo
  const bool ix = (prefix & 7) == 5;
  const int dc = ix ? 0xF0 : 0xE8, sc = ix ? 0xF4 : 0xEC;
  const int dir = (op & 2) ? -sz : sz;
  int n = 0;
  do {
    const uint32_t d = getL(dc), s = getL(sc);
    const uint32_t v = rd(s, sz);
    wr(d, sz, v);
    setL(dc, int64_t(d) + dir); setL(sc, int64_t(s) + dir);
    const uint32_t bc = (getW(0xE4) - 1) & 0xFFFF;
    setW(0xE4, bc);
    n++;
    if (bc == 0) break;
  } while (op & 1);
  setFlags((f() & ~(FH | FV | FN)) | (getW(0xE4) ? FV : 0));
  cycles += 10 + (n - 1) * 14;
}

void TLCS900H::blockCp(int prefix, int sz, int op) {
  // CPI/CPIR/CPD/CPDR: compara A (ou WA) com (XHL) — registrador do prefixo
  const int rc = WREG[prefix & 7];
  const int dir = (op & 2) ? -sz : sz;
  const uint32_t acc = sz == 1 ? getB(0xE0) : getW(0xE0);
  int n = 0;
  bool eq = false;
  do {
    const uint32_t ad = getL(rc);
    const uint32_t m = rd(ad, sz);
    setL(rc, int64_t(ad) + dir);
    const uint32_t bc = (getW(0xE4) - 1) & 0xFFFF;
    setW(0xE4, bc);
    const int c = f() & FC;
    sub(acc, m, sz, false);
    setFlags((f() & ~(FV | FC)) | c | (bc ? FV : 0));
    eq = (f() & FZ) != 0;
    n++;
    if (bc == 0 || eq) break;
    // única diferença deliberada em relação ao JS: com o próprio XBC como ponteiro (forma inválida da
    // instrução) o contador nunca chega a zero e o JS fica preso para sempre; aqui para em 65536 voltas
    if (n >= 0x10000) break;
  } while (op & 1);
  cycles += 10 + (n - 1) * 14;
}

// ------------------------------------------------------------ operações com destino em memória
void TLCS900H::dstOp(uint32_t a) {
  const int op = static_cast<int>(f8());
  op2 = op;
  const int R = op & 7;
  if (op >= 0xF0) { if (cond(op & 15)) { pc = pop(4) & 0xFFFFFF; cycles += 12; } else cycles += 6; return; }       // RET cc
  if (op >= 0xE0) { if (cond(op & 15)) { push(4, pc); pc = a; cycles += 12; } else cycles += 6; return; }          // CALL cc,mem
  if (op >= 0xD0) { if (cond(op & 15)) { pc = a; cycles += 8; } else cycles += 6; return; }                        // JP cc,mem
  if (op >= 0x80) {  // bits em memória (byte)
    const int b = R, kind = (op >> 3) & 15;
    const uint32_t m = rd(a, 1);
    const int bit = (m >> b) & 1;
    switch (kind) {
      case 0: setFlags((f() & ~FC) | ((f() & FC) & bit)); break;                                     // ANDCF
      case 1: setFlags(f() | bit); break;                                                            // ORCF
      case 2: setFlags(f() ^ bit); break;                                                            // XORCF
      case 3: setFlags((f() & ~FC) | bit); break;                                                    // LDCF
      case 4: wr(a, 1, (f() & FC) ? (m | (1u << b)) : (m & ~(1u << b))); break;                      // STCF
      case 5: setFlags((f() & ~(FZ | FN)) | FH | (bit ? 0 : FZ)); wr(a, 1, m | (1u << b)); break;    // TSET
      case 6: wr(a, 1, m & ~(1u << b)); break;                                                       // RES
      case 7: wr(a, 1, m | (1u << b)); break;                                                        // SET
      case 8: wr(a, 1, m ^ (1u << b)); break;                                                        // CHG
      case 9: setFlags((f() & ~(FZ | FN)) | FH | (bit ? 0 : FZ)); break;                             // BIT
    }
    cycles += kind == 5 ? 10 : 8; return;
  }
  if (op >= 0x60 && op < 0x68) { wr(a, 4, getL(WREG[R])); cycles += 6; return; }  // LD (mem),XRR
  if (op >= 0x50 && op < 0x58) { wr(a, 2, getW(WREG[R])); cycles += 4; return; }  // LD (mem),RR
  if (op >= 0x40 && op < 0x48) { wr(a, 1, getB(BREG[R])); cycles += 4; return; }  // LD (mem),R
  if (op >= 0x30 && op < 0x38) { setL(WREG[R], a); cycles += 4; return; }         // LDA XRR,mem
  if (op >= 0x20 && op < 0x28) { setW(WREG[R], a); cycles += 4; return; }         // LDA RR,mem
  if (op >= 0x28 && op < 0x2D) {  // ANDCF…STCF A,(mem)
    const int b = getB(0xE0) & 15;
    cycles += 8;
    if (b >= 8) { if (op < 0x2B) rd(a, 1); return; }  // bit fora do byte: nada muda
    const uint32_t m = rd(a, 1);
    const int bit = (m >> b) & 1;
    switch (op) {
      case 0x28: setFlags((f() & ~FC) | ((f() & FC) & bit)); break;
      case 0x29: setFlags(f() | bit); break;
      case 0x2A: setFlags(f() ^ bit); break;
      case 0x2B: setFlags((f() & ~FC) | bit); break;
      case 0x2C: wr(a, 1, (f() & FC) ? (m | (1u << b)) : (m & ~(1u << b))); break;
    }
    return;
  }
  switch (op) {
    case 0x00: { const uint32_t v = f8(); wr(a, 1, v); cycles += 5; return; }                       // LD (mem),#8
    case 0x02: { const uint32_t v = f16(); wr(a, 2, v); cycles += 6; return; }                      // LDW (mem),#16
    case 0x04: { const uint32_t v = pop(1); wr(a, 1, v); cycles += 6; return; }                     // POP (mem)
    case 0x06: { const uint32_t v = pop(2); wr(a, 2, v); cycles += 6; return; }                     // POPW (mem)
    case 0x14: { const uint32_t s = f16(); const uint32_t v = rd(s, 1); wr(a, 1, v); cycles += 8; return; }  // LD (mem),(nn)
    case 0x16: { const uint32_t s = f16(); const uint32_t v = rd(s, 2); wr(a, 2, v); cycles += 8; return; }  // LDW (mem),(nn)
    default: cycles += 2;
  }
}

// ------------------------------------------------------------ multiplicação e divisão
// kind: 0 MUL, 1 MULS, 2 DIV, 3 DIVS; dc = registrador de destino (código de palavra); sz = tamanho do operando
void TLCS900H::muldiv(int kind, int dc, uint32_t v, int sz) {
  if (kind < 2) {
    if (sz == 1) {
      int32_t a = getW(dc) & 0xFF, b = v & 0xFF;
      if (kind == 1) { a = s8(a); b = s8(b); }
      setW(dc, (a * b) & 0xFFFF);
    } else {
      int32_t a = getL(dc) & 0xFFFF, b = v & 0xFFFF;
      if (kind == 1) { a = s16(a); b = s16(b); }
      setL(dc, static_cast<uint32_t>(int64_t(a) * b));
    }
    return;
  }
  // divisão: quociente na parte baixa, resto na alta; por zero, V=1 e resultado (dividendo << meio) | ~parte alta
  if (sz == 1) {
    const int32_t a = static_cast<int32_t>(getW(dc)), b = v & 0xFF;
    if (b == 0) {
      const int32_t h = kind == 3 ? s16(a) >> 8 : a >> 8;
      setW(dc, ((a << 8) | (h ^ 0xFF)) & 0xFFFF);
      setFlags(f() | FV); return;
    }
    int32_t q, rm;
    if (kind == 2) { q = a / b; rm = a % b; }
    else { const int32_t sa = s16(a), sb = s8(b); q = sa / sb; rm = sa % sb; }
    setFlags((f() & ~FV) | (q > 0xFF ? FV : 0));
    setW(dc, ((rm & 0xFF) << 8) | (q & 0xFF));
  } else {
    const uint32_t a = getL(dc), b = v & 0xFFFF;
    if (b == 0) {
      const uint32_t h = kind == 3 ? static_cast<uint32_t>(static_cast<int32_t>(a) >> 16) : a >> 16;
      setL(dc, (a << 16) | (h ^ 0xFFFF));
      setFlags(f() | FV); return;
    }
    int64_t q, rm;
    if (kind == 2) { q = a / b; rm = a % b; }
    else { const int64_t sa = static_cast<int32_t>(a), sb = s16(b); q = sa / sb; rm = sa % sb; }
    setFlags((f() & ~FV) | (q > 0xFFFF ? FV : 0));
    setL(dc, ((static_cast<uint32_t>(rm) & 0xFFFF) << 16) | (static_cast<uint32_t>(q) & 0xFFFF));
  }
}

// ------------------------------------------------------------ operações sobre registrador
void TLCS900H::regOp(int c, int sz, int prefix) {
  regOpX(c, sz, prefix);
  const int t = T.REG_CYC[op2 & 0xFF];
  if (t) cycles = sz == 4 ? t >> 8 : t & 0xFF;
}

void TLCS900H::regOpX(int c, int sz, int prefix) {
  const int op = static_cast<int>(f8());
  op2 = op;
  const int R = op & 7;
  const int Rc = sz == 1 ? BREG[R] : WREG[R];
  const uint32_t v = getR(c, sz);
  cycles = 4;
  // operações aritméticas/lógicas de 32 bits entre registradores custam 7
  const int L7 = sz == 4 ? 7 : 4;
  if (op >= 0xF8) {  // RLC…SRL A,r (contagem em A)
    int n = getB(0xE0) & 15; if (!n) n = 16;
    setR(c, sz, shift(op & 7, v, n, sz)); cycles = (sz == 4 ? 8 : 6) + 2 * n; return;
  }
  if (op >= 0xF0) { alu(7, getR(Rc, sz), v, sz); cycles = L7; return; }  // CP R,r
  if (op >= 0xE8) { int n = f8() & 15; if (!n) n = 16; setR(c, sz, shift(op & 7, v, n, sz)); cycles = (sz == 4 ? 8 : 6) + 2 * n; return; }  // RLC… #,r
  if (op >= 0xE0) { setR(Rc, sz, alu(6, getR(Rc, sz), v, sz)); cycles = L7; return; }  // OR R,r
  if (op >= 0xD8) { if (sz != 4) alu(7, v, R, sz); return; }                           // CP r,#3
  if (op >= 0xD0) { setR(Rc, sz, alu(5, getR(Rc, sz), v, sz)); cycles = L7; return; }  // XOR R,r
  if (op >= 0xC8) { const uint32_t imm = fimm(sz); const uint32_t res = alu(op & 7, v, imm, sz); if ((op & 7) != 7) setR(c, sz, res); cycles = L7; return; }  // ALU r,#
  if (op >= 0xC0) { setR(Rc, sz, alu(4, getR(Rc, sz), v, sz)); cycles = L7; return; }  // AND R,r
  if (op >= 0xB8) { const uint32_t t = getR(Rc, sz); setR(Rc, sz, v); setR(c, sz, t); return; }  // EX R,r
  if (op >= 0xB0) { setR(Rc, sz, alu(3, getR(Rc, sz), v, sz)); cycles = L7; return; }  // SBC R,r
  if (op >= 0xA8) { setR(c, sz, R); return; }                                          // LD r,#3
  if (op >= 0xA0) { setR(Rc, sz, alu(2, getR(Rc, sz), v, sz)); cycles = L7; return; }  // SUB R,r
  if (op >= 0x98) { setR(c, sz, getR(Rc, sz)); return; }                               // LD r,R
  if (op >= 0x90) { setR(Rc, sz, alu(1, getR(Rc, sz), v, sz)); cycles = L7; return; }  // ADC R,r
  if (op >= 0x88) { setR(Rc, sz, v); return; }                                         // LD R,r
  if (op >= 0x80) { setR(Rc, sz, alu(0, getR(Rc, sz), v, sz)); cycles = L7; return; }  // ADD R,r
  if (op >= 0x70) { if (sz != 4) setR(c, sz, cond(op & 15) ? 1 : 0); return; }        // SCC cc,r
  if (op >= 0x68) { setR(c, sz, dec(v, R ? R : 8, sz, sz == 1)); cycles = sz == 4 ? 5 : 4; return; }  // DEC #3,r
  if (op >= 0x60) { setR(c, sz, inc(v, R ? R : 8, sz, sz == 1)); return; }                           // INC #3,r
  if (op >= 0x40) {  // MUL…DIVS RR,r (alvo inválido: registrador de descarte)
    const int k = (op >> 3) & 3;
    if (sz == 4) { cycles = 2; return; }
    muldiv(k, sz == 1 ? ((R & 1) ? WREG[R >> 1] : 0x80) : WREG[R], v, sz);
    cycles = MD_CYC[k] + (sz == 2 ? 8 : 0); return;
  }
  switch (op) {
    case 0x03: { const uint32_t imm = fimm(sz); setR(c, sz, imm); if (sz == 4) cycles = 6; return; }  // LD r,#
    case 0x04: push(sz, v); cycles = sz == 4 ? 7 : 5; return;                                          // PUSH r
    case 0x05: { const uint32_t p = pop(sz); setR(c, sz, p); cycles = sz == 4 ? 8 : 6; return; }      // POP r
    case 0x06: if (sz != 4) setR(c, sz, ~int64_t(v)); setFlags(f() | FH | FN); return;                // CPL
    case 0x07: if (sz != 4) setR(c, sz, sub(0, v, sz, false)); return;                                // NEG
    case 0x08: case 0x09: case 0x0A: case 0x0B: {  // MUL… rr,#: o alvo vem do prefixo
      if (sz == 4) { cycles = 2; return; }
      const int k = op - 8, n = prefix & 7;
      const int t = sz == 1 ? (prefix == 0xC7 ? (c & ~1) : (n & 1) ? 0xE0 + (n >> 1) * 4 : 0x80) : 0xE0 + n * 4;
      const uint32_t imm = fimm(sz);
      muldiv(k, t, imm, sz); cycles = MD_CYC[k] + (sz == 2 ? 8 : 0); return;
    }
    case 0x0C: { const int32_t d = s16(f16()); push(4, v); setL(c, xsp()); setXsp(int64_t(xsp()) + d); return; }  // LINK
    case 0x0D: { setXsp(v); const uint32_t p = pop(4); setL(c, p); return; }                                       // UNLK
    case 0x0E: case 0x0F: {  // BS1F/BS1B A,r
      if (v == 0) { setFlags(f() | FV); return; }
      setFlags(f() & ~FV);
      int i;
      // como no JS: o deslocamento usa só os 5 bits baixos (num registrador de 32 bits o BS1B pode passar de 0)
      if (op == 0x0E) { i = 0; while (!((v >> (i & 31)) & 1)) i++; }
      else { i = 15; while (!((v >> (i & 31)) & 1)) i--; }
      setB(0xE0, i); return;
    }
    case 0x10: {  // DAA
      const int a0 = v & 0xFF, fl = f();
      int a = a0, carry = fl & FC, fix = 0;
      if ((fl & FH) || (a & 0xF) > 9) fix |= 0x06;
      if (carry || a > 0x99) fix |= 0x60;
      a = (fl & FN) ? (a - fix) & 0xFF : (a + fix) & 0xFF;
      if ((fl & FN) ? a > a0 : a < a0) carry = 1;
      setFlags(szFlags(a, 1) | T.PAR[a] | (fl & FN) | (((a0 ^ a) & 0x10) ? FH : 0) | carry);
      setB(c, a);
      return;
    }
    case 0x12: setR(c, sz, sz == 2 ? (v & 0xFF) : (v & 0xFFFF)); return;                              // EXTZ
    case 0x13: setR(c, sz, sz == 2 ? (s8(v) & 0xFFFF) : int64_t(s16(v))); cycles = 5; return;          // EXTS
    case 0x14: if (sz != 1) setR(c, sz, (v & 1) ? int64_t(v) + 1 : int64_t(v)); return;               // PAA
    case 0x16: { uint32_t m = 0; for (int k = 0; k < 16; k++) if (v & (1u << k)) m |= 1u << (15 - k); setR(c, 2, m); return; }  // MIRR
    case 0x19: {  // MULA rr
      const uint32_t xhl = getL(0xEC);
      const int32_t m1 = s16(rd(getL(0xE8), 2));
      const int32_t m2 = s16(rd(xhl, 2));
      const int32_t p = static_cast<int32_t>(static_cast<uint32_t>(int64_t(m1) * m2));
      const uint32_t old = getL(c);
      const int32_t res = static_cast<int32_t>(old + static_cast<uint32_t>(p));
      setL(c, static_cast<uint32_t>(res));
      setL(0xEC, int64_t(xhl) - 2);
      const uint32_t vbit = ((old ^ ~static_cast<uint32_t>(p)) & (old ^ static_cast<uint32_t>(res))) >> 31;
      setFlags((f() & ~(FS | FZ | FV)) | (res < 0 ? FS : 0) | (res == 0 ? FZ : 0) | (vbit ? FV : 0));
      cycles = 31; return;
    }
    case 0x1C: {  // DJNZ
      const int32_t d = s8(f8());
      if (sz == 4) { cycles = 7; return; }
      const uint32_t nv = (v - 1) & MASK[sz];
      setR(c, sz, nv);
      if (nv) { pc = static_cast<uint32_t>(int64_t(pc) + d) & 0xFFFFFF; cycles = 11; } else cycles = 7;
      return;
    }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: { const int b = f8() & 15; if (sz != 4) bitCF(op & 7, c, sz, v, b); return; }
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: { const int b = getB(0xE0) & 15; if (sz != 4) bitCF(op & 7, c, sz, v, b); return; }
    case 0x2E: { const int cr = static_cast<int>(f8()); setCR(cr, v); return; }                 // LDC cr,r
    case 0x2F: { const int cr = static_cast<int>(f8()); setR(c, sz, getCR(cr)); return; }       // LDC r,cr
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: {  // RES/SET/CHG/BIT/TSET #,r (só 8/16 bits)
      const int b = f8() & 15;
      if (sz == 4) { if (op >= 0x33) setFlags((f() & ~FN) | FH); return; }
      if (op == 0x30) setR(c, sz, v & ~(1u << b));
      else if (op == 0x31) setR(c, sz, v | (1u << b));
      else if (op == 0x32) setR(c, sz, v ^ (1u << b));
      else { setFlags((f() & ~(FZ | FN)) | FH | (((v >> b) & 1) ? 0 : FZ)); if (op == 0x34) setR(c, sz, v | (1u << b)); }
      return;
    }
    case 0x38: case 0x39: case 0x3A: {  // MINC1/2/4 (só 16 bits)
      const uint32_t stp = 1u << (op - 0x38), num = (f16() + stp) & 0xFFFF;
      cycles = 8;
      if (sz != 2 || !num) return;
      const int64_t res = (v % num) == num - stp ? int64_t(v) - (num - stp) : int64_t(v) + stp;
      setR(c, sz, res); return;
    }
    case 0x3C: case 0x3D: case 0x3E: {  // MDEC1/2/4 (só 16 bits)
      const uint32_t stp = 1u << (op - 0x3C), num = (f16() + stp) & 0xFFFF;
      cycles = 7;
      if (sz != 2 || !num) return;
      const int64_t res = (v % num) == 0 ? int64_t(v) + (num - stp) : int64_t(v) - stp;
      setR(c, sz, res); return;
    }
    default: cycles = 2;
  }
}

void TLCS900H::bitCF(int kind, int c, int sz, uint32_t v, int b) {
  const int bit = (v >> b) & 1;
  switch (kind) {
    case 0: if (b < sz * 8) setFlags((f() & ~FC) | ((f() & FC) & bit)); break;  // ANDCF
    case 1: if (b < sz * 8) setFlags(f() | bit); break;                          // ORCF
    case 2: if (b < sz * 8) setFlags(f() ^ bit); break;                          // XORCF
    case 3: if (b < sz * 8) setFlags((f() & ~FC) | bit); break;                  // LDCF
    case 4: if (b < sz * 8) setR(c, sz, (f() & FC) ? (v | (1u << b)) : (v & ~(1u << b))); break;  // STCF
  }
}

// registradores de controle (micro-DMA e aninhamento de interrupções)
uint32_t TLCS900H::getCR(int cr) const {
  if (cr < 0x10) return dmaS[cr >> 2];
  if (cr < 0x20) return dmaD[(cr - 0x10) >> 2];
  if (cr < 0x30) return (cr & 2) ? dmaM[(cr - 0x20) >> 2] : dmaC[(cr - 0x20) >> 2];
  if (cr == 0x3C) return static_cast<uint32_t>(intNest);
  return 0;
}
void TLCS900H::setCR(int cr, uint32_t v) {
  if (cr < 0x10) dmaS[cr >> 2] = v;
  else if (cr < 0x20) dmaD[(cr - 0x10) >> 2] = v;
  else if (cr < 0x30) { if (cr & 2) dmaM[(cr - 0x20) >> 2] = static_cast<uint8_t>(v); else dmaC[(cr - 0x20) >> 2] = static_cast<uint16_t>(v); }
  else if (cr == 0x3C) intNest = v & 0xFFFF;
}

// ------------------------------------------------------------ interrupção (nível > IFF)
int TLCS900H::interrupt(int vector, int level) {
  halted = false;
  push(4, pc); push(2, sr);
  sr = (sr & 0x8FFF) | (std::min(level + 1, 7) << 12);
  intNest = (intNest + 1) & 0xFFFF;
  pc = rd(0xFFFF00 + vector, 4) & 0xFFFFFF;
  return 18;
}

void TLCS900H::sync(StateIO& io) {
  io.raw(r, sizeof r);
  io.sync(pc); io.sync(sr); io.sync(fd); io.sync(intNest); io.sync(halted);
  io.raw(dmaS, sizeof dmaS); io.raw(dmaD, sizeof dmaD); io.raw(dmaC, sizeof dmaC); io.raw(dmaM, sizeof dmaM);
  if (io.reading()) { pc &= 0xFFFFFF; sr &= 0xFFFF; fd &= 0xFF; intNest &= 0xFFFF; }
}

}  // namespace indigo::ngp
