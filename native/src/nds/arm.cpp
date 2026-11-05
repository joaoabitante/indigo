// CPU ARM do DS (ARMv4T no ARM7, ARMv5TE no ARM9). Veja arm.h.
#include "arm.h"

#include <array>

namespace indigo::nds {

namespace {
int bankOf(int mode) {
  switch (mode) {
    case M_FIQ: return 1;
    case M_IRQ: return 2;
    case M_SVC: return 3;
    case M_ABT: return 4;
    case M_UND: return 5;
    default: return 0;
  }
}
inline uint32_t ror(uint32_t v, int n) { n &= 31; return n ? (v >> n) | (v << (32 - n)) : v; }
// soma com o Q ligado se estourar (sem saturar), como SMLAxy/SMLAWy
inline int32_t addQ(int64_t a, int32_t b, int& q) {
  const int64_t x = static_cast<int32_t>(a) + int64_t(b);
  if (x > INT32_MAX || x < INT32_MIN) q = 1;
  return static_cast<int32_t>(static_cast<uint32_t>(x));
}
inline int32_t sat32(int64_t x, int& q) {
  if (x > INT32_MAX) { q = 1; return INT32_MAX; }
  if (x < INT32_MIN) { q = 1; return INT32_MIN; }
  return static_cast<int32_t>(x);
}
}  // namespace

// ---------------- estado / modos ----------------
uint32_t Arm::cpsr() const {
  return (uint32_t(n) << 31) | (uint32_t(z) << 30) | (uint32_t(c) << 29) | (uint32_t(v) << 28) | (uint32_t(q) << 27) |
         (uint32_t(i) << 7) | (uint32_t(f) << 6) | (uint32_t(t) << 5) | uint32_t(mode);
}
void Arm::setCpsr(uint32_t u) {
  n = (u >> 31) & 1; z = (u >> 30) & 1; c = (u >> 29) & 1; v = (u >> 28) & 1;
  q = v5 ? (u >> 27) & 1 : 0;
  i = (u >> 7) & 1; f = (u >> 6) & 1; t = (u >> 5) & 1;
  switchMode(static_cast<int>(u & 0x1F));
}
uint32_t Arm::spsr() const {
  const int b = bankOf(mode);
  return b == 0 ? cpsr() : spsrBank[b];
}
void Arm::setSpsr(uint32_t val) {
  const int b = bankOf(mode);
  if (b != 0) spsrBank[b] = val;
}

void Arm::switchMode(int newMode) {
  newMode |= 0x10;
  const int oldMode = mode;
  if (oldMode == newMode) return;
  const int ob = bankOf(oldMode), nb = bankOf(newMode);
  if (ob != nb) {
    bankR13[ob] = r[13]; bankR14[ob] = r[14];
    if (ob == 1) for (int k = 0; k < 5; k++) { fiqHi[k] = r[8 + k]; r[8 + k] = usrHi[k]; }
    if (nb == 1) for (int k = 0; k < 5; k++) { usrHi[k] = r[8 + k]; r[8 + k] = fiqHi[k]; }
    r[13] = bankR13[nb]; r[14] = bankR14[nb];
  }
  mode = newMode;
}

void Arm::reset() {
  for (auto& x : r) x = 0;
  for (int k = 0; k < 6; k++) bankR13[k] = bankR14[k] = spsrBank[k] = 0;
  for (int k = 0; k < 5; k++) usrHi[k] = fiqHi[k] = 0;
  mode = M_SVC;
  n = z = c = v = q = 0;
  i = 1; f = 1; t = 0;
  halted = false;
  nextPC = excBase;
  cp_ = nullptr; cpLo_ = 0; cpLen_ = 0;
  armLut_ = armLut(v5);
  thumbLut_ = thumbLut(v5);
}

namespace {
// tabela [condição][NZCV] -> executa?
struct CondTable {
  bool t[16][16];
  CondTable() {
    for (int c = 0; c < 16; c++)
      for (int f = 0; f < 16; f++) {
        const bool n = f & 8, z = f & 4, cc = f & 2, v = f & 1;
        bool r;
        switch (c) {
          case 0x0: r = z; break;
          case 0x1: r = !z; break;
          case 0x2: r = cc; break;
          case 0x3: r = !cc; break;
          case 0x4: r = n; break;
          case 0x5: r = !n; break;
          case 0x6: r = v; break;
          case 0x7: r = !v; break;
          case 0x8: r = cc && !z; break;
          case 0x9: r = !cc || z; break;
          case 0xA: r = n == v; break;
          case 0xB: r = n != v; break;
          case 0xC: r = !z && n == v; break;
          case 0xD: r = z || n != v; break;
          case 0xE: r = true; break;
          default: r = false; break;
        }
        t[c][f] = r;
      }
  }
};
const CondTable COND;
}  // namespace

bool Arm::checkCond(int cond) const {
  return COND.t[cond & 15][(n << 3) | (z << 2) | (c << 1) | v];
}

uint32_t Arm::fetch32(uint32_t a) {
  if (a - cpLo_ < cpLen_) {
    const uint8_t* p = cp_ + ((a - cpLo_) & cpMask_);
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
  }
  uint32_t lo, hi, mask;
  const uint8_t* page = bus.codePage(a, lo, hi, mask);
  if (page) {
    cp_ = page; cpLo_ = lo; cpLen_ = hi - lo - 3; cpMask_ = mask;
    const uint8_t* p = cp_ + ((a - cpLo_) & cpMask_);
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
  }
  return bus.read32(a);
}
uint32_t Arm::fetch16(uint32_t a) {
  if (a - cpLo_ < cpLen_) {
    const uint8_t* p = cp_ + ((a - cpLo_) & cpMask_);
    return p[0] | (p[1] << 8);
  }
  uint32_t lo, hi, mask;
  const uint8_t* page = bus.codePage(a, lo, hi, mask);
  if (page) {
    cp_ = page; cpLo_ = lo; cpLen_ = hi - lo - 3; cpMask_ = mask;
    const uint8_t* p = cp_ + ((a - cpLo_) & cpMask_);
    return p[0] | (p[1] << 8);
  }
  // só 16 bits: o índice da tabela Thumb (ins >> 6) não pode passar de 1023
  return bus.read16(a) & 0xFFFF;
}

int Arm::run(int budget, uint32_t gen) {
  if (gen != mapGen) { mapGen = gen; cp_ = nullptr; cpLo_ = 0; cpLen_ = 0; }
  int used = 0;
  while (used < budget) {
    if (irqLine && (!i || forceIrq)) irq();
    if (halted) return used > budget ? used : budget;
    used += step() << costShift;
    // a espera conta dentro desta fatia: sem isso a CPU seguia até o fim da fatia antes de "esperar"
    if (stall) { used += stall; stall = 0; }
  }
  return used;
}

// executa uma instrução; devolve ciclos aproximados
int Arm::step() {
  const uint32_t pc = nextPC;
  branched = false;
  int cyc;
  if (t) {
    uint32_t ins;
    if (pc - cpLo_ < cpLen_) { const uint8_t* p = cp_ + ((pc - cpLo_) & cpMask_); ins = p[0] | (p[1] << 8); }
    else ins = fetch16(pc);
    r[15] = pc + 4;
    cyc = 1 + (this->*thumbLut_[ins >> 6])(ins);
    if (branched) refill();
    else nextPC = pc + 2;
  } else {
    uint32_t ins;
    if (pc - cpLo_ < cpLen_) {
      const uint8_t* p = cp_ + ((pc - cpLo_) & cpMask_);
      ins = p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
    } else ins = fetch32(pc);
    r[15] = pc + 8;
    const int cond = static_cast<int>(ins >> 28);
    if (cond == 0xE || COND.t[cond][(n << 3) | (z << 2) | (c << 1) | v]) cyc = 1 + (this->*armLut_[((ins >> 16) & 0xFF0) | ((ins >> 4) & 0xF)])(ins);
    else if (cond == 0xF) {
      cyc = 1;
      if (v5 && (ins & 0x0E000000) == 0x0A000000) {  // BLX endereço (troca para Thumb)
        const int32_t off = static_cast<int32_t>(ins << 8) >> 6;
        r[14] = pc + 4;
        t = 1;
        setPC(r[15] + off + ((ins >> 23) & 2));
        cyc += 2;
      } else if (v5 && (ins & 0x0D70F000) == 0x0550F000) {
        // PLD: nada a fazer
      } else {
        undefCount++;
      }
    } else cyc = 1;
    if (branched) refill();
    else nextPC = pc + 4;
  }
  return cyc;
}

void Arm::refill() { nextPC = t ? (r[15] & ~1u) : (r[15] & ~3u); }

void Arm::exception(int newMode, uint32_t vec, uint32_t ret) {
  const uint32_t old = cpsr();
  switchMode(newMode);
  spsrBank[bankOf(newMode)] = old;
  r[14] = ret;
  t = 0; i = 1;
  if (newMode == M_FIQ) f = 1;
  nextPC = excBase + vec;
  r[15] = nextPC;
  branched = true;  // dentro de step(): o refill mantém o vetor
}

void Arm::irq() {
  halted = false;
  forceIrq = false;
  exception(M_IRQ, 0x18, nextPC + 4);
  branched = false;
}

int Arm::undefinedInstr(uint32_t) {
  undefCount++;
  return 0;
}

// ---------------- barrel shifter ----------------
uint32_t Arm::shiftImm(int type, uint32_t val, int amt) {
  switch (type) {
    case 0:
      if (amt == 0) { shC = c; return val; }
      shC = (val >> (32 - amt)) & 1; return val << amt;
    case 1:
      if (amt == 0) { shC = val >> 31; return 0; }
      shC = (val >> (amt - 1)) & 1; return val >> amt;
    case 2:
      if (amt == 0) { shC = val >> 31; return static_cast<uint32_t>(static_cast<int32_t>(val) >> 31); }
      shC = (static_cast<int32_t>(val) >> (amt - 1)) & 1; return static_cast<uint32_t>(static_cast<int32_t>(val) >> amt);
    default:
      if (amt == 0) { shC = val & 1; return (uint32_t(c) << 31) | (val >> 1); }
      shC = (val >> (amt - 1)) & 1; return ror(val, amt);
  }
}

uint32_t Arm::shiftReg(int type, uint32_t val, int amt) {
  if (amt == 0) { shC = c; return val; }
  switch (type) {
    case 0:
      if (amt < 32) { shC = (val >> (32 - amt)) & 1; return val << amt; }
      if (amt == 32) { shC = val & 1; return 0; }
      shC = 0; return 0;
    case 1:
      if (amt < 32) { shC = (val >> (amt - 1)) & 1; return val >> amt; }
      if (amt == 32) { shC = val >> 31; return 0; }
      shC = 0; return 0;
    case 2:
      if (amt < 32) { shC = (static_cast<int32_t>(val) >> (amt - 1)) & 1; return static_cast<uint32_t>(static_cast<int32_t>(val) >> amt); }
      shC = val >> 31; return static_cast<uint32_t>(static_cast<int32_t>(val) >> 31);
    default: {
      const int a = amt & 31;
      if (a == 0) { shC = val >> 31; return val; }
      shC = (val >> (a - 1)) & 1; return ror(val, a);
    }
  }
}

// ---------------- aritmética com flags ----------------
uint32_t Arm::add(uint32_t a, uint32_t b, bool s) {
  const uint32_t res = a + b;
  if (s) { setNZ(res); c = res < a; v = ((~(a ^ b) & (a ^ res)) >> 31) & 1; }
  return res;
}
uint32_t Arm::adc(uint32_t a, uint32_t b, bool s) {
  const uint64_t w = uint64_t(a) + b + c;
  const uint32_t res = static_cast<uint32_t>(w);
  if (s) { setNZ(res); c = (w >> 32) & 1; v = ((~(a ^ b) & (a ^ res)) >> 31) & 1; }
  return res;
}
uint32_t Arm::sub(uint32_t a, uint32_t b, bool s) {
  const uint32_t res = a - b;
  if (s) { setNZ(res); c = a >= b; v = (((a ^ b) & (a ^ res)) >> 31) & 1; }
  return res;
}
uint32_t Arm::sbc(uint32_t a, uint32_t b, bool s) {
  const uint32_t borrow = 1 - c;
  const uint32_t res = a - b - borrow;
  if (s) { setNZ(res); c = uint64_t(a) >= uint64_t(b) + borrow; v = (((a ^ b) & (a ^ res)) >> 31) & 1; }
  return res;
}

uint32_t Arm::ldrWord(uint32_t addr) {
  const uint32_t val = bus.read32(addr & ~3u);
  return ror(val, (addr & 3) << 3);
}
uint32_t Arm::ldrHalf(uint32_t addr) {
  const uint32_t val = bus.read16(addr & ~1u);
  if (v5) return val;  // o ARM9 ignora o bit 0
  return (addr & 1) ? ror(val, 8) : val;
}
uint32_t Arm::ldrSHalf(uint32_t addr) {
  if ((addr & 1) && !v5) return static_cast<uint32_t>(static_cast<int8_t>(bus.read8(addr)));
  return static_cast<uint32_t>(static_cast<int16_t>(bus.read16(addr & ~1u)));
}
void Arm::loadPC(uint32_t val) {
  if (v5) {
    t = val & 1;
    setPC(val & ~1u);
  } else setPC(val);
}

// =====================================================================
//                              ARM
// =====================================================================
const Arm::Op* Arm::armLut(bool v5) {
  static const std::array<Op, 4096> l4 = [] { std::array<Op, 4096> l{}; for (int k = 0; k < 4096; k++) l[k] = decodeArm(k, false); return l; }();
  static const std::array<Op, 4096> l5 = [] { std::array<Op, 4096> l{}; for (int k = 0; k < 4096; k++) l[k] = decodeArm(k, true); return l; }();
  return v5 ? l5.data() : l4.data();
}
const Arm::Op* Arm::thumbLut(bool v5) {
  static const std::array<Op, 1024> l4 = [] { std::array<Op, 1024> l{}; for (uint32_t k = 0; k < 1024; k++) l[k] = decodeThumb(k << 6, false); return l; }();
  static const std::array<Op, 1024> l5 = [] { std::array<Op, 1024> l{}; for (uint32_t k = 0; k < 1024; k++) l[k] = decodeThumb(k << 6, true); return l; }();
  return v5 ? l5.data() : l4.data();
}

Arm::Op Arm::decodeArm(int key, bool v5) {
  const int hi = key >> 4, lo = key & 0xF;
  switch (hi >> 5) {
    case 0:
      if (lo == 9) {
        if ((hi & 0xFC) == 0x00) return &Arm::armMul;
        if ((hi & 0xF8) == 0x08) return &Arm::armMulLong;
        if ((hi & 0xFB) == 0x10) return &Arm::armSwp;
        return &Arm::undefinedInstr;
      }
      if ((lo & 9) == 9) return &Arm::armHalf;
      if (hi == 0x12 && lo == 1) return &Arm::armBx;
      if (v5) {
        if (hi == 0x12 && lo == 3) return &Arm::armBlxReg;
        if (hi == 0x12 && lo == 7) return &Arm::armBkpt;
        if (hi == 0x16 && lo == 1) return &Arm::armClz;
        if ((hi & 0xF9) == 0x10 && lo == 5) return &Arm::armQ;
        if ((hi & 0xF9) == 0x10 && (lo & 9) == 8) return &Arm::armDspMul;
      }
      if ((hi & 0xF9) == 0x10) {
        if (lo != 0) return &Arm::undefinedInstr;
        return (hi & 2) ? &Arm::armMsr : &Arm::armMrs;
      }
      return &Arm::armDP;
    case 1:
      if ((hi & 0xF9) == 0x30) return (hi & 2) ? &Arm::armMsr : &Arm::undefinedInstr;
      return &Arm::armDP;
    case 2: return &Arm::armSdt;
    case 3: return (lo & 1) ? &Arm::undefinedInstr : &Arm::armSdt;
    case 4: return &Arm::armBlock;
    case 5: return &Arm::armBranch;
    case 6: return &Arm::undefinedInstr;  // LDC/STC: sem coprocessador
    default:
      if (hi & 0x10) return &Arm::armSwi;
      return (lo & 1) ? &Arm::armCop : &Arm::undefinedInstr;
  }
}

int Arm::armDP(uint32_t ins) {
  const int op = (ins >> 21) & 15;
  const bool s = (ins >> 20) & 1;
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
  uint32_t op2, rnv;
  int cyc = 0;
  if (ins & 0x02000000) {
    const uint32_t imm = ins & 0xFF;
    const int rot = ((ins >> 8) & 15) << 1;
    if (rot) { op2 = ror(imm, rot); shC = op2 >> 31; }
    else { op2 = imm; shC = c; }
    rnv = r[rn];
  } else if (ins & 0x10) {
    const int rm = ins & 15;
    uint32_t rmv = r[rm]; if (rm == 15) rmv += 4;
    rnv = r[rn]; if (rn == 15) rnv += 4;
    op2 = shiftReg((ins >> 5) & 3, rmv, r[(ins >> 8) & 15] & 0xFF);
    cyc = 1;
  } else {
    op2 = shiftImm((ins >> 5) & 3, r[ins & 15], (ins >> 7) & 31);
    rnv = r[rn];
  }
  uint32_t res;
  const bool sf = s && rd != 15;
  switch (op) {
    case 0x0: res = rnv & op2; if (sf) { setNZ(res); c = shC; } break;
    case 0x1: res = rnv ^ op2; if (sf) { setNZ(res); c = shC; } break;
    case 0x2: res = sub(rnv, op2, sf); break;
    case 0x3: res = sub(op2, rnv, sf); break;
    case 0x4: res = add(rnv, op2, sf); break;
    case 0x5: res = adc(rnv, op2, sf); break;
    case 0x6: res = sbc(rnv, op2, sf); break;
    case 0x7: res = sbc(op2, rnv, sf); break;
    case 0x8: res = rnv & op2; setNZ(res); c = shC; if (rd == 15) setCpsr(spsr()); return cyc;
    case 0x9: res = rnv ^ op2; setNZ(res); c = shC; if (rd == 15) setCpsr(spsr()); return cyc;
    case 0xA: sub(rnv, op2, true); if (rd == 15) setCpsr(spsr()); return cyc;
    case 0xB: add(rnv, op2, true); if (rd == 15) setCpsr(spsr()); return cyc;
    case 0xC: res = rnv | op2; if (sf) { setNZ(res); c = shC; } break;
    case 0xD: res = op2; if (sf) { setNZ(res); c = shC; } break;
    case 0xE: res = rnv & ~op2; if (sf) { setNZ(res); c = shC; } break;
    default: res = ~op2; if (sf) { setNZ(res); c = shC; } break;
  }
  if (rd == 15) {
    if (s) setCpsr(spsr());
    setPC(res);
    return cyc + 2;
  }
  r[rd] = res;
  return cyc;
}

int Arm::armMrs(uint32_t ins) {
  r[(ins >> 12) & 15] = (ins & 0x00400000) ? spsr() : cpsr();
  return 0;
}

int Arm::armMsr(uint32_t ins) {
  uint32_t val;
  if (ins & 0x02000000) val = ror(ins & 0xFF, ((ins >> 8) & 15) << 1);
  else val = r[ins & 15];
  uint32_t mask = 0;
  if (ins & 0x00080000) mask |= 0xFF000000u;
  if (ins & 0x00040000) mask |= 0x00FF0000u;
  if (ins & 0x00020000) mask |= 0x0000FF00u;
  if (ins & 0x00010000) mask |= 0x000000FFu;
  if (ins & 0x00400000) {
    if (bankOf(mode) != 0) setSpsr((spsr() & ~mask) | (val & mask));
  } else {
    if (mode == M_USR) mask &= 0xFF000000u;
    const uint32_t nv = (cpsr() & ~mask) | (val & mask);
    setCpsr((nv & ~0x20u) | (uint32_t(t) << 5));  // T não muda por MSR
  }
  return 0;
}

int Arm::armBx(uint32_t ins) {
  const uint32_t val = r[ins & 15];
  t = val & 1;
  setPC(val & ~1u);
  return 2;
}

int Arm::armBlxReg(uint32_t ins) {
  const uint32_t val = r[ins & 15];
  r[14] = r[15] - 4;
  t = val & 1;
  setPC(val & ~1u);
  return 2;
}

int Arm::armBkpt(uint32_t) {
  exception(M_ABT, 0x0C, r[15] - 4);
  return 2;
}

int Arm::armClz(uint32_t ins) {
  const uint32_t x = r[ins & 15];
  r[(ins >> 12) & 15] = x ? static_cast<uint32_t>(__builtin_clz(x)) : 32;
  return 0;
}

int Arm::armQ(uint32_t ins) {
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15, rm = ins & 15;
  const int op = (ins >> 21) & 3;
  int64_t a = static_cast<int32_t>(r[rm]);
  int64_t b = static_cast<int32_t>(r[rn]);
  if (op & 2) b = sat32(b * 2, q);  // QDADD/QDSUB: dobra Rn com saturação
  r[rd] = static_cast<uint32_t>(sat32((op & 1) ? a - b : a + b, q));
  return 0;
}

int Arm::armDspMul(uint32_t ins) {
  const int rd = (ins >> 16) & 15, rn = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
  const bool x = ins & 0x20, y = ins & 0x40;
  const int op = (ins >> 21) & 3;
  const int32_t vs = y ? static_cast<int16_t>(r[rs] >> 16) : static_cast<int16_t>(r[rs]);
  if (op == 1) {  // SMLAWy (x=0) / SMULWy (x=1)
    const int64_t prod = (int64_t(static_cast<int32_t>(r[rm])) * vs) >> 16;
    if (!x) r[rd] = static_cast<uint32_t>(addQ(prod, static_cast<int32_t>(r[rn]), q));
    else r[rd] = static_cast<uint32_t>(static_cast<int32_t>(prod));
    return 1;
  }
  const int32_t vm = x ? static_cast<int16_t>(r[rm] >> 16) : static_cast<int16_t>(r[rm]);
  const int32_t prod = vm * vs;
  switch (op) {
    case 0: r[rd] = static_cast<uint32_t>(addQ(prod, static_cast<int32_t>(r[rn]), q)); return 1;  // SMLAxy
    case 2: {  // SMLALxy
      int64_t acc = static_cast<int64_t>((uint64_t(r[rd]) << 32) | r[rn]);
      acc += prod;
      r[rn] = static_cast<uint32_t>(acc);
      r[rd] = static_cast<uint32_t>(static_cast<uint64_t>(acc) >> 32);
      return 2;
    }
    default: r[rd] = static_cast<uint32_t>(prod); return 1;  // SMULxy
  }
}

int Arm::armMul(uint32_t ins) {
  const int rd = (ins >> 16) & 15, rn = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
  uint32_t res = r[rm] * r[rs];
  int cyc = 1;
  if (ins & 0x00200000) { res += r[rn]; cyc++; }
  if (ins & 0x00100000) setNZ(res);
  r[rd] = res;
  return cyc;
}

int Arm::armMulLong(uint32_t ins) {
  const int rdHi = (ins >> 16) & 15, rdLo = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
  const bool sgn = (ins & 0x00400000) != 0;
  uint64_t prod = sgn ? static_cast<uint64_t>(int64_t(static_cast<int32_t>(r[rm])) * int64_t(static_cast<int32_t>(r[rs])))
                      : uint64_t(r[rm]) * r[rs];
  if (ins & 0x00200000) prod += (uint64_t(r[rdHi]) << 32) | r[rdLo];
  const uint32_t lo = static_cast<uint32_t>(prod), hi = static_cast<uint32_t>(prod >> 32);
  if (ins & 0x00100000) { n = hi >> 31; z = (lo == 0 && hi == 0); }
  r[rdLo] = lo;
  r[rdHi] = hi;
  return 2;
}

int Arm::armSwp(uint32_t ins) {
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15, rm = ins & 15;
  const uint32_t addr = r[rn], src = r[rm];
  uint32_t tmp;
  if (ins & 0x00400000) { tmp = bus.read8(addr); bus.write8(addr, src & 0xFF); }
  else { tmp = ldrWord(addr); bus.write32(addr & ~3u, src); }
  r[rd] = tmp;
  return 2;
}

int Arm::armHalf(uint32_t ins) {
  const bool p = ins & 0x01000000, u = ins & 0x00800000, w = ins & 0x00200000, l = ins & 0x00100000;
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
  const int sh = (ins >> 5) & 3;
  const uint32_t off = (ins & 0x00400000) ? (((ins >> 4) & 0xF0) | (ins & 0xF)) : r[ins & 15];
  const uint32_t base = r[rn];
  const uint32_t offAddr = u ? base + off : base - off;
  const uint32_t addr = p ? offAddr : base;
  if (l) {
    uint32_t val;
    if (sh == 1) val = ldrHalf(addr);
    else if (sh == 2) val = static_cast<uint32_t>(static_cast<int8_t>(bus.read8(addr)));
    else val = ldrSHalf(addr);
    if (!p || w) r[rn] = offAddr;
    if (rd == 15) { loadPC(val); return 3; }
    r[rd] = val;
    return 1;
  }
  if (sh == 1) {
    uint32_t val = r[rd]; if (rd == 15) val += 4;
    bus.write16(addr & ~1u, val & 0xFFFF);
  } else if (v5 && sh == 2) {  // LDRD
    const int rd2 = rd | 1;
    const uint32_t a = bus.read32(addr & ~3u), b = bus.read32((addr + 4) & ~3u);
    if (!p || w) r[rn] = offAddr;
    r[rd & ~1] = a;
    if (rd2 == 15) loadPC(b); else r[rd2] = b;
    return 2;
  } else if (v5 && sh == 3) {  // STRD
    bus.write32(addr & ~3u, r[rd & ~1]);
    bus.write32((addr + 4) & ~3u, (rd | 1) == 15 ? r[15] + 4 : r[rd | 1]);
  }
  if (!p || w) r[rn] = offAddr;
  return 0;
}

int Arm::armSdt(uint32_t ins) {
  const bool p = ins & 0x01000000, u = ins & 0x00800000, b = ins & 0x00400000, w = ins & 0x00200000, l = ins & 0x00100000;
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
  uint32_t off;
  if (ins & 0x02000000) {
    const int saveC = shC;
    off = shiftImm((ins >> 5) & 3, r[ins & 15], (ins >> 7) & 31);
    shC = saveC;
  } else off = ins & 0xFFF;
  const uint32_t base = r[rn];
  const uint32_t offAddr = u ? base + off : base - off;
  const uint32_t addr = p ? offAddr : base;
  if (l) {
    const uint32_t val = b ? bus.read8(addr) : ldrWord(addr);
    if (!p || w) r[rn] = offAddr;
    if (rd == 15) { loadPC(val); return 3; }
    r[rd] = val;
    return 1;
  }
  uint32_t val = r[rd]; if (rd == 15) val += 4;
  if (b) bus.write8(addr, val & 0xFF); else bus.write32(addr & ~3u, val);
  if (!p || w) r[rn] = offAddr;
  return 0;
}

int Arm::armBlock(uint32_t ins) {
  const bool p = ins & 0x01000000, u = ins & 0x00800000, s = ins & 0x00400000, w = ins & 0x00200000, l = ins & 0x00100000;
  const int rn = (ins >> 16) & 15;
  int rlist = ins & 0xFFFF;
  const uint32_t base = r[rn];
  int count = __builtin_popcount(rlist);
  bool empty = false;
  if (rlist == 0) {
    // lista vazia: o ARMv4 carrega/grava o PC e anda 0x40; o ARMv5 não transfere nada mas anda 0x40
    empty = true;
    count = 16;
    rlist = v5 ? 0 : 0x8000;
  }
  uint32_t addr, finalBase;
  if (u) { addr = p ? base + 4 : base; finalBase = base + count * 4; }
  else { addr = p ? base - count * 4 : base - count * 4 + 4; finalBase = base - count * 4; }
  const bool userBank = s && !(l && (rlist & 0x8000));
  const int oldMode = mode;
  if (userBank) switchMode(M_USR);
  if (l) {
    if (w) r[rn] = finalBase;
    for (int k = 0; k < 16; k++) {
      if (!(rlist & (1 << k))) continue;
      const uint32_t val = bus.read32(addr & ~3u);
      addr += 4;
      if (k == 15) {
        if (s) { setCpsr(spsr()); setPC(val & (t ? ~1u : ~3u)); }
        else loadPC(val);
      } else r[k] = val;
    }
    // ARMv5: com a base na lista, ela fica com o valor final se for a única ou não for a última da lista
    if (w && v5 && (rlist & (1 << rn)) && !empty && (rlist == (1 << rn) || (rlist >> (rn + 1)) != 0)) r[rn] = finalBase;
    if (userBank) switchMode(oldMode);
    return count + 1;
  }
  bool first = true;
  for (int k = 0; k < 16; k++) {
    if (!(rlist & (1 << k))) continue;
    uint32_t val;
    if (k == 15) val = r[15] + 4;
    else if (k == rn && !userBank) val = (first || v5 || !w) ? base : finalBase;
    else val = r[k];
    bus.write32(addr & ~3u, val);
    addr += 4;
    first = false;
  }
  if (userBank) switchMode(oldMode);
  if (w) r[rn] = finalBase;
  return count;
}

int Arm::armBranch(uint32_t ins) {
  const int32_t off = static_cast<int32_t>(ins << 8) >> 6;
  if (ins & 0x01000000) r[14] = r[15] - 4;
  setPC(r[15] + off);
  return 2;
}

int Arm::armCop(uint32_t ins) {
  const int cp = (ins >> 8) & 15;
  if (!v5 || cp != 15) { undefCount++; return 0; }
  const uint32_t crn = (ins >> 16) & 15, crm = ins & 15, op2 = (ins >> 5) & 7;
  const int rd = (ins >> 12) & 15;
  if (ins & 0x00100000) {  // MRC
    const uint32_t val = bus.cp15Read(crn, crm, op2);
    if (rd == 15) { n = val >> 31; z = (val >> 30) & 1; c = (val >> 29) & 1; v = (val >> 28) & 1; }
    else r[rd] = val;
  } else {
    bus.cp15Write(crn, crm, op2, rd == 15 ? r[15] + 4 : r[rd]);
  }
  return 1;
}

int Arm::armSwi(uint32_t ins) {
  const uint32_t ret = r[15] - 4;
  if (bus.swi(static_cast<int>((ins >> 16) & 0xFF), ret - 4)) return 3;
  exception(M_SVC, 0x08, ret);
  return 3;
}

// =====================================================================
//                              THUMB
// =====================================================================
Arm::Op Arm::decodeThumb(uint32_t ins, bool v5) {
  const uint32_t top = ins >> 8;
  if ((ins & 0xF800) == 0x1800) return &Arm::thAddSub;
  if ((ins & 0xE000) == 0x0000) return &Arm::thShift;
  if ((ins & 0xE000) == 0x2000) return &Arm::thImm;
  if ((ins & 0xFC00) == 0x4000) return &Arm::thAlu;
  if ((ins & 0xFC00) == 0x4400) return &Arm::thHi;
  if ((ins & 0xF800) == 0x4800) return &Arm::thLdrPc;
  if ((ins & 0xF200) == 0x5000) return &Arm::thLsReg;
  if ((ins & 0xF200) == 0x5200) return &Arm::thLsSign;
  if ((ins & 0xE000) == 0x6000) return &Arm::thLsImm;
  if ((ins & 0xF000) == 0x8000) return &Arm::thLsHalf;
  if ((ins & 0xF000) == 0x9000) return &Arm::thLsSp;
  if ((ins & 0xF000) == 0xA000) return &Arm::thAddr;
  if (top == 0xB0) return &Arm::thAddSp;
  if ((ins & 0xF600) == 0xB400) return &Arm::thPushPop;
  if (v5 && top == 0xBE) return &Arm::thBkpt;
  if ((ins & 0xF000) == 0xC000) return &Arm::thLdmStm;
  if (top == 0xDF) return &Arm::thSwi;
  if ((ins & 0xF000) == 0xD000) return &Arm::thBcond;
  if ((ins & 0xF800) == 0xE000) return &Arm::thB;
  if ((ins & 0xF800) == 0xF000) return &Arm::thBlHi;
  if ((ins & 0xF800) == 0xF800) return &Arm::thBlLo;
  if (v5 && (ins & 0xF800) == 0xE800) return &Arm::thBlxLo;
  return &Arm::undefinedInstr;
}

int Arm::thShift(uint32_t ins) {
  const int rd = ins & 7, rs = (ins >> 3) & 7, amt = (ins >> 6) & 31;
  const uint32_t res = shiftImm((ins >> 11) & 3, r[rs], amt);
  r[rd] = res;
  setNZ(res); c = shC;
  return 0;
}

int Arm::thAddSub(uint32_t ins) {
  const int rd = ins & 7, rs = (ins >> 3) & 7, x = (ins >> 6) & 7;
  const uint32_t op2 = (ins & 0x400) ? x : r[x];
  r[rd] = (ins & 0x200) ? sub(r[rs], op2, true) : add(r[rs], op2, true);
  return 0;
}

int Arm::thImm(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  const uint32_t imm = ins & 0xFF;
  switch ((ins >> 11) & 3) {
    case 0: r[rd] = imm; setNZ(imm); break;
    case 1: sub(r[rd], imm, true); break;
    case 2: r[rd] = add(r[rd], imm, true); break;
    default: r[rd] = sub(r[rd], imm, true); break;
  }
  return 0;
}

int Arm::thAlu(uint32_t ins) {
  const int rd = ins & 7, rs = (ins >> 3) & 7;
  const uint32_t a = r[rd], b = r[rs];
  uint32_t res;
  switch ((ins >> 6) & 15) {
    case 0x0: res = a & b; setNZ(res); r[rd] = res; return 0;
    case 0x1: res = a ^ b; setNZ(res); r[rd] = res; return 0;
    case 0x2: res = shiftReg(0, a, b & 0xFF); setNZ(res); c = shC; r[rd] = res; return 1;
    case 0x3: res = shiftReg(1, a, b & 0xFF); setNZ(res); c = shC; r[rd] = res; return 1;
    case 0x4: res = shiftReg(2, a, b & 0xFF); setNZ(res); c = shC; r[rd] = res; return 1;
    case 0x5: r[rd] = adc(a, b, true); return 0;
    case 0x6: r[rd] = sbc(a, b, true); return 0;
    case 0x7: res = shiftReg(3, a, b & 0xFF); setNZ(res); c = shC; r[rd] = res; return 1;
    case 0x8: setNZ(a & b); return 0;
    case 0x9: r[rd] = sub(0, b, true); return 0;
    case 0xA: sub(a, b, true); return 0;
    case 0xB: add(a, b, true); return 0;
    case 0xC: res = a | b; setNZ(res); r[rd] = res; return 0;
    case 0xD: res = a * b; setNZ(res); r[rd] = res; return 1;
    case 0xE: res = a & ~b; setNZ(res); r[rd] = res; return 0;
    default: res = ~b; setNZ(res); r[rd] = res; return 0;
  }
}

int Arm::thHi(uint32_t ins) {
  const int rd = (ins & 7) | ((ins >> 4) & 8), rs = (ins >> 3) & 15;
  const uint32_t sv = r[rs];
  switch ((ins >> 8) & 3) {
    case 0: {
      const uint32_t res = r[rd] + sv;
      if (rd == 15) { setPC(res & ~1u); return 2; }
      r[rd] = res; return 0;
    }
    case 1: sub(r[rd], sv, true); return 0;
    case 2:
      if (rd == 15) { setPC(sv & ~1u); return 2; }
      r[rd] = sv; return 0;
    default:
      if (v5 && (ins & 0x80)) r[14] = (r[15] - 2) | 1;  // BLX registrador
      t = sv & 1;
      setPC(sv & ~1u);
      return 2;
  }
}

int Arm::thLdrPc(uint32_t ins) {
  r[(ins >> 8) & 7] = bus.read32(((r[15] & ~2u) + ((ins & 0xFF) << 2)) & ~3u);
  return 1;
}

int Arm::thLsReg(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7, ro = (ins >> 6) & 7;
  const uint32_t addr = r[rb] + r[ro];
  switch ((ins >> 10) & 3) {
    case 0: bus.write32(addr & ~3u, r[rd]); return 0;
    case 1: bus.write8(addr, r[rd] & 0xFF); return 0;
    case 2: r[rd] = ldrWord(addr); return 1;
    default: r[rd] = bus.read8(addr); return 1;
  }
}

int Arm::thLsSign(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7, ro = (ins >> 6) & 7;
  const uint32_t addr = r[rb] + r[ro];
  switch ((ins >> 10) & 3) {
    case 0: bus.write16(addr & ~1u, r[rd] & 0xFFFF); return 0;
    case 1: r[rd] = static_cast<uint32_t>(static_cast<int8_t>(bus.read8(addr))); return 1;
    case 2: r[rd] = ldrHalf(addr); return 1;
    default: r[rd] = ldrSHalf(addr); return 1;
  }
}

int Arm::thLsImm(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7, off = (ins >> 6) & 31;
  if (ins & 0x1000) {
    const uint32_t addr = r[rb] + off;
    if (ins & 0x800) { r[rd] = bus.read8(addr); return 1; }
    bus.write8(addr, r[rd] & 0xFF); return 0;
  }
  const uint32_t addr = r[rb] + (off << 2);
  if (ins & 0x800) { r[rd] = ldrWord(addr); return 1; }
  bus.write32(addr & ~3u, r[rd]); return 0;
}

int Arm::thLsHalf(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7;
  const uint32_t addr = r[rb] + (((ins >> 6) & 31) << 1);
  if (ins & 0x800) { r[rd] = ldrHalf(addr); return 1; }
  bus.write16(addr & ~1u, r[rd] & 0xFFFF); return 0;
}

int Arm::thLsSp(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  const uint32_t addr = r[13] + ((ins & 0xFF) << 2);
  if (ins & 0x800) { r[rd] = ldrWord(addr); return 1; }
  bus.write32(addr & ~3u, r[rd]); return 0;
}

int Arm::thAddr(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  const uint32_t off = (ins & 0xFF) << 2;
  r[rd] = (ins & 0x800) ? r[13] + off : (r[15] & ~2u) + off;
  return 0;
}

int Arm::thAddSp(uint32_t ins) {
  const uint32_t off = (ins & 0x7F) << 2;
  r[13] = (ins & 0x80) ? r[13] - off : r[13] + off;
  return 0;
}

int Arm::thPushPop(uint32_t ins) {
  const int rlist = ins & 0xFF;
  const bool extra = (ins & 0x100) != 0;
  if (ins & 0x800) {  // POP
    uint32_t addr = r[13];
    if (rlist == 0 && !extra) {
      if (v5) { r[13] = addr + 0x40; return 1; }
      const uint32_t val = bus.read32(addr & ~3u);
      r[13] = addr + 0x40;
      setPC(val & ~1u);
      return 2;
    }
    for (int k = 0; k < 8; k++) if (rlist & (1 << k)) { r[k] = bus.read32(addr & ~3u); addr += 4; }
    if (extra) {
      const uint32_t val = bus.read32(addr & ~3u); addr += 4;
      if (v5) { t = val & 1; setPC(val & ~1u); }
      else setPC(val & ~1u);
    }
    r[13] = addr;
    return __builtin_popcount(rlist) + (extra ? 3 : 1);
  }
  if (rlist == 0 && !extra) {
    r[13] -= 0x40;
    if (!v5) bus.write32(r[13] & ~3u, r[15] + 2);
    return 0;
  }
  const int count = __builtin_popcount(rlist) + (extra ? 1 : 0);
  uint32_t addr = r[13] - count * 4;
  r[13] = addr;
  for (int k = 0; k < 8; k++) if (rlist & (1 << k)) { bus.write32(addr & ~3u, r[k]); addr += 4; }
  if (extra) bus.write32(addr & ~3u, r[14]);
  return count;
}

int Arm::thLdmStm(uint32_t ins) {
  const int rb = (ins >> 8) & 7;
  const int rlist = ins & 0xFF;
  uint32_t addr = r[rb];
  if (rlist == 0) {
    if (!v5) {
      if (ins & 0x800) setPC(bus.read32(addr & ~3u) & ~1u);
      else bus.write32(addr & ~3u, r[15] + 2);
    }
    r[rb] = addr + 0x40;
    return 1;
  }
  const int count = __builtin_popcount(rlist);
  const uint32_t finalAddr = addr + count * 4;
  if (ins & 0x800) {
    for (int k = 0; k < 8; k++) if (rlist & (1 << k)) { r[k] = bus.read32(addr & ~3u); addr += 4; }
    // ARMv5: escreve a base de volta se ela não está na lista ou não é a última da lista
    if (!(rlist & (1 << rb))) r[rb] = finalAddr;
    else if (v5 && (rlist == (1 << rb) || (rlist >> (rb + 1)) != 0)) r[rb] = finalAddr;
    return count + 1;
  }
  bool first = true;
  for (int k = 0; k < 8; k++) {
    if (rlist & (1 << k)) {
      const uint32_t val = (k == rb) ? ((first || v5) ? r[rb] : finalAddr) : r[k];
      bus.write32(addr & ~3u, val); addr += 4; first = false;
    }
  }
  r[rb] = finalAddr;
  return count;
}

int Arm::thBcond(uint32_t ins) {
  if (!checkCond((ins >> 8) & 15)) return 0;
  const int32_t off = static_cast<int32_t>((ins & 0xFF) << 24) >> 23;
  setPC(r[15] + off);
  return 2;
}

int Arm::thSwi(uint32_t ins) {
  const uint32_t ret = r[15] - 2;
  if (bus.swi(static_cast<int>(ins & 0xFF), ret - 2)) return 3;
  exception(M_SVC, 0x08, ret);
  return 3;
}

int Arm::thBkpt(uint32_t) {
  exception(M_ABT, 0x0C, r[15] - 2);
  return 2;
}

int Arm::thB(uint32_t ins) {
  const int32_t off = static_cast<int32_t>((ins & 0x7FF) << 21) >> 20;
  setPC(r[15] + off);
  return 2;
}

int Arm::thBlHi(uint32_t ins) {
  const int32_t off = static_cast<int32_t>((ins & 0x7FF) << 21) >> 9;
  r[14] = r[15] + off;
  return 0;
}

int Arm::thBlLo(uint32_t ins) {
  const uint32_t next = (r[15] - 2) | 1;
  setPC(r[14] + ((ins & 0x7FF) << 1));
  r[14] = next;
  return 2;
}

int Arm::thBlxLo(uint32_t ins) {
  const uint32_t next = (r[15] - 2) | 1;
  const uint32_t target = (r[14] + ((ins & 0x7FF) << 1)) & ~3u;
  r[14] = next;
  t = 0;
  setPC(target);
  return 2;
}

void Arm::sync(StateIO& io) {
  io.raw(r, sizeof r); io.raw(bankR13, sizeof bankR13); io.raw(bankR14, sizeof bankR14); io.raw(spsrBank, sizeof spsrBank);
  io.raw(usrHi, sizeof usrHi); io.raw(fiqHi, sizeof fiqHi);
  for (int* p : {&n, &z, &c, &v, &q, &i, &f, &t, &mode, &shC}) io.sync(*p);
  io.sync(nextPC); io.sync(halted); io.sync(excBase);
  if (io.reading()) {
    for (int* p : {&n, &z, &c, &v, &q, &i, &f, &t, &shC}) *p &= 1;
    if (!v5) q = 0;
    mode = (mode & 0x1F) | 0x10;
    if (excBase != 0 && excBase != 0xFFFF0000u) excBase = 0;
    cp_ = nullptr; cpLo_ = 0; cpLen_ = 0;
    mapGen = ~0u;
    branched = false;
  }
}

}  // namespace indigo::nds
