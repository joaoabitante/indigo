// ARM7TDMI — interpretador ARM (32 bits) + Thumb (16 bits) (porte de src/cpu.js)
// Convenção do pipeline: durante a execução de uma instrução no endereço A, r[15] = A + 8 (ARM) ou A + 4 (Thumb).
#include <array>

#include "gba.h"

namespace indigo::gba {

namespace {
int bankOf(int mode) {
  switch (mode) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0;
  }
}
inline int32_t shl(int32_t v, int n) { return static_cast<int32_t>(U32(v) << n); }
inline int32_t lsr(int32_t v, int n) { return static_cast<int32_t>(U32(v) >> n); }
inline int32_t ror(int32_t v, int n) { return static_cast<int32_t>((U32(v) >> n) | (U32(v) << (32 - n))); }
inline int bit31(int32_t v) { return static_cast<int>(U32(v) >> 31); }
}  // namespace

CPU::CPU(GBA& g) : gba(g) {}

// ---------------- estado / modos ----------------
int32_t CPU::cpsr() const {
  return static_cast<int32_t>((U32(n) << 31) | (U32(z) << 30) | (U32(c) << 29) | (U32(v) << 28) |
                              (U32(i) << 7) | (U32(f) << 6) | (U32(t) << 5) | U32(mode));
}
void CPU::setCpsr(int32_t val) {
  const uint32_t u = U32(val);
  n = (u >> 31) & 1; z = (u >> 30) & 1; c = (u >> 29) & 1; v = (u >> 28) & 1;
  i = (u >> 7) & 1; f = (u >> 6) & 1; t = (u >> 5) & 1;
  switchMode(static_cast<int>(u & 0x1F));
}
int32_t CPU::spsr() const {
  const int b = bankOf(mode);
  return b == 0 ? cpsr() : spsrBank[b];
}
void CPU::setSpsr(int32_t val) {
  const int b = bankOf(mode);
  if (b != 0) spsrBank[b] = val;
}

void CPU::switchMode(int newMode) {
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

void CPU::reset(bool skipBios) {
  for (auto& x : r) x = 0;
  for (int k = 0; k < 6; k++) { bankR13[k] = bankR14[k] = spsrBank[k] = 0; }
  for (int k = 0; k < 5; k++) { usrHi[k] = fiqHi[k] = 0; }
  mode = MODE_SVC;
  n = z = c = v = 0;
  i = 1; f = 1; t = 0;
  halted = false;
  if (skipBios) {
    // estado deixado pela BIOS oficial ao saltar para o cartucho
    switchMode(MODE_IRQ); r[13] = 0x03007FA0;
    switchMode(MODE_SVC); r[13] = 0x03007FE0;
    switchMode(MODE_SYS); r[13] = 0x03007F00;
    i = 0; f = 0;
    nextPC = 0x08000000;
  } else {
    nextPC = 0;
  }
}

bool CPU::checkCond(int cond) const {
  switch (cond) {
    case 0x0: return z == 1;
    case 0x1: return z == 0;
    case 0x2: return c == 1;
    case 0x3: return c == 0;
    case 0x4: return n == 1;
    case 0x5: return n == 0;
    case 0x6: return v == 1;
    case 0x7: return v == 0;
    case 0x8: return c == 1 && z == 0;
    case 0x9: return c == 0 || z == 1;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return z == 0 && n == v;
    case 0xD: return z == 1 || n != v;
    case 0xE: return true;
    default: return false;
  }
}

// executa uma instrução; devolve ciclos de "trabalho interno" (acessos de memória são contados pelo barramento)
int CPU::step() {
  const int64_t pc = nextPC;
  Bus& bus = gba.bus;
  int cyc;
  branched = false;
  if (t) {
    const uint32_t ins = U32(bus.fetch16(pc)) & 0xFFFF;
    bus.codeCost(pc + 4, false);  // o pipeline busca a instrução 2 à frente no 1º ciclo
    r[15] = I32(pc + 4);
    cyc = (this->*thumbLut()[ins >> 6])(ins);
    if (branched) refill();
    else nextPC = pc + 2;
  } else {
    const uint32_t ins = U32(bus.fetch32(pc));
    bus.codeCost(pc + 8, true);
    r[15] = I32(pc + 8);
    const int cond = static_cast<int>(ins >> 28);
    if (cond == 0xE || checkCond(cond)) cyc = (this->*armLut()[((ins >> 16) & 0xFF0) | ((ins >> 4) & 0xF)])(ins);
    else cyc = 0;
    if (branched) refill();
    else nextPC = pc + 4;
  }
  return cyc;
}

// desvio: o pipeline busca o destino (N) e a instrução seguinte (S)
void CPU::refill() {
  const int32_t target = t ? (r[15] & ~1) : (r[15] & ~3);
  nextPC = target;
  gba.bus.codeCost(target, !t);
  gba.bus.codeCost(int64_t(target) + (t ? 2 : 4), !t);
}

void CPU::irq() {
  const int64_t ret = nextPC + 4;
  const int32_t old = cpsr();
  switchMode(MODE_IRQ);
  spsrBank[2] = old;
  r[14] = I32(ret);
  t = 0; i = 1;
  nextPC = 0x18;
  halted = false;
  gba.bus.codeCost(0x18, true); gba.bus.codeCost(0x1C, true);
}

void CPU::swiException(int32_t retAddr) {
  const int32_t old = cpsr();
  switchMode(MODE_SVC);
  spsrBank[3] = old;
  r[14] = retAddr;
  t = 0; i = 1;
  setPC(0x08);
}

// ---------------- barrel shifter ----------------
int32_t CPU::shiftImm(int type, int32_t val, int amt) {
  switch (type) {
    case 0:  // LSL
      if (amt == 0) { shC = c; return val; }
      shC = static_cast<int>((U32(val) >> (32 - amt)) & 1); return shl(val, amt);
    case 1:  // LSR
      if (amt == 0) { shC = bit31(val); return 0; }
      shC = static_cast<int>((U32(val) >> (amt - 1)) & 1); return lsr(val, amt);
    case 2:  // ASR
      if (amt == 0) { shC = bit31(val); return val >> 31; }
      shC = (val >> (amt - 1)) & 1; return val >> amt;
    default:  // ROR / RRX
      if (amt == 0) { shC = val & 1; return static_cast<int32_t>((U32(c) << 31) | (U32(val) >> 1)); }
      shC = static_cast<int>((U32(val) >> (amt - 1)) & 1); return ror(val, amt);
  }
}

int32_t CPU::shiftReg(int type, int32_t val, int amt) {
  if (amt == 0) { shC = c; return val; }
  switch (type) {
    case 0:
      if (amt < 32) { shC = static_cast<int>((U32(val) >> (32 - amt)) & 1); return shl(val, amt); }
      if (amt == 32) { shC = val & 1; return 0; }
      shC = 0; return 0;
    case 1:
      if (amt < 32) { shC = static_cast<int>((U32(val) >> (amt - 1)) & 1); return lsr(val, amt); }
      if (amt == 32) { shC = bit31(val); return 0; }
      shC = 0; return 0;
    case 2:
      if (amt < 32) { shC = (val >> (amt - 1)) & 1; return val >> amt; }
      shC = bit31(val); return val >> 31;
    default: {
      const int a = amt & 31;
      if (a == 0) { shC = bit31(val); return val; }
      shC = static_cast<int>((U32(val) >> (a - 1)) & 1); return ror(val, a);
    }
  }
}

// ---------------- aritmética com flags ----------------
int32_t CPU::add(int32_t a, int32_t b, bool s) {
  const int32_t res = I32(int64_t(a) + b);
  if (s) {
    n = bit31(res); z = res == 0 ? 1 : 0;
    c = (uint64_t(U32(a)) + U32(b)) > 0xFFFFFFFFull ? 1 : 0;
    v = bit31(~(a ^ b) & (a ^ res));
  }
  return res;
}
int32_t CPU::adc(int32_t a, int32_t b, bool s) {
  const int cin = c;
  const int32_t res = I32(int64_t(a) + b + cin);
  if (s) {
    n = bit31(res); z = res == 0 ? 1 : 0;
    c = (uint64_t(U32(a)) + U32(b) + cin) > 0xFFFFFFFFull ? 1 : 0;
    v = bit31(~(a ^ b) & (a ^ res));
  }
  return res;
}
int32_t CPU::sub(int32_t a, int32_t b, bool s) {
  const int32_t res = I32(int64_t(a) - b);
  if (s) {
    n = bit31(res); z = res == 0 ? 1 : 0;
    c = U32(a) >= U32(b) ? 1 : 0;
    v = bit31((a ^ b) & (a ^ res));
  }
  return res;
}
int32_t CPU::sbc(int32_t a, int32_t b, bool s) {
  const int borrow = 1 - c;
  const int32_t res = I32(int64_t(a) - b - borrow);
  if (s) {
    n = bit31(res); z = res == 0 ? 1 : 0;
    c = uint64_t(U32(a)) >= uint64_t(U32(b)) + borrow ? 1 : 0;
    v = bit31((a ^ b) & (a ^ res));
  }
  return res;
}

int CPU::mulCycles(int32_t rs) {
  // Igual ao JS: lá "(v & 0xFFFFFF00) === 0xFFFFFF00" compara um int32 negativo com um número positivo e nunca
  // é verdade, então operandos negativos sempre custam 4 ciclos (no hardware custariam 1 a 3). Mantido para o
  // porte continuar idêntico à versão web; quando o JS for corrigido, aqui volta a testar os prefixos de uns.
  const uint32_t x = U32(rs);
  if ((x & 0xFFFFFF00u) == 0) return 1;
  if ((x & 0xFFFF0000u) == 0) return 2;
  if ((x & 0xFF000000u) == 0) return 3;
  return 4;
}

// leitura com rotação (LDR desalinhado)
int32_t CPU::ldrWord(int64_t addr) {
  const int32_t val = gba.bus.read32(I32(addr) & ~3);
  const int rot = static_cast<int>((addr & 3) << 3);
  return rot ? ror(val, rot) : val;
}
int32_t CPU::ldrHalf(int64_t addr) {
  const int32_t val = gba.bus.read16(I32(addr) & ~1);
  return (addr & 1) ? static_cast<int32_t>((U32(val) >> 8) | (U32(val) << 24)) : val;
}
int32_t CPU::ldrSHalf(int64_t addr) {
  if (addr & 1) return static_cast<int8_t>(gba.bus.read8(addr));
  return static_cast<int16_t>(gba.bus.read16(addr));
}

// =====================================================================
//                              ARM
// =====================================================================
const CPU::Op* CPU::armLut() {
  static const std::array<Op, 4096> lut = [] {
    std::array<Op, 4096> l{};
    for (int key = 0; key < 4096; key++) l[key] = decodeArm(key);
    return l;
  }();
  return lut.data();
}

const CPU::Op* CPU::thumbLut() {
  static const std::array<Op, 1024> lut = [] {
    std::array<Op, 1024> l{};
    for (uint32_t key = 0; key < 1024; key++) l[key] = decodeThumb(key << 6);
    return l;
  }();
  return lut.data();
}

CPU::Op CPU::decodeArm(int key) {
  const int hi = key >> 4, lo = key & 0xF;
  switch (hi >> 5) {
    case 0:
      if (lo == 9) {
        if ((hi & 0xFC) == 0x00) return &CPU::armMul;
        if ((hi & 0xF8) == 0x08) return &CPU::armMulLong;
        if ((hi & 0xFB) == 0x10) return &CPU::armSwp;
        return &CPU::undefinedInstr;
      }
      if ((lo & 9) == 9) return &CPU::armHalf;
      if (hi == 0x12 && lo == 1) return &CPU::armBx;
      if ((hi & 0xF9) == 0x10) {
        if (lo != 0) return &CPU::undefinedInstr;
        return (hi & 2) ? &CPU::armMsr : &CPU::armMrs;
      }
      return &CPU::armDP;
    case 1:
      if ((hi & 0xF9) == 0x30) return (hi & 2) ? &CPU::armMsr : &CPU::undefinedInstr;
      return &CPU::armDP;
    case 2: return &CPU::armSdt;
    case 3: return (lo & 1) ? &CPU::undefinedInstr : &CPU::armSdt;
    case 4: return &CPU::armBlock;
    case 5: return &CPU::armBranch;
    case 6: return &CPU::undefinedInstr;
    default: return (hi & 0x10) ? &CPU::armSwi : &CPU::undefinedInstr;
  }
}

int CPU::armDP(uint32_t ins) {
  const int op = (ins >> 21) & 15;
  const bool s = (ins >> 20) & 1;
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
  int32_t op2, rnv;
  int cyc = 0;
  if (ins & 0x02000000) {
    const int32_t imm = ins & 0xFF;
    const int rot = ((ins >> 8) & 15) << 1;
    if (rot) { op2 = ror(imm, rot); shC = bit31(op2); }
    else { op2 = imm; shC = c; }
    rnv = r[rn];
  } else if (ins & 0x10) {
    const int rm = ins & 15;
    int32_t rmv = r[rm]; if (rm == 15) rmv = I32(int64_t(rmv) + 4);
    rnv = r[rn]; if (rn == 15) rnv = I32(int64_t(rnv) + 4);
    op2 = shiftReg((ins >> 5) & 3, rmv, r[(ins >> 8) & 15] & 0xFF);
    cyc = 1;
  } else {
    op2 = shiftImm((ins >> 5) & 3, r[ins & 15], (ins >> 7) & 31);
    rnv = r[rn];
  }
  int32_t res;
  const bool sf = s && rd != 15;  // com Rd=PC e S=1, as flags vêm do SPSR
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

int CPU::armMrs(uint32_t ins) {
  r[(ins >> 12) & 15] = (ins & 0x00400000) ? spsr() : cpsr();
  return 0;
}

int CPU::armMsr(uint32_t ins) {
  int32_t val;
  if (ins & 0x02000000) {
    const int32_t imm = ins & 0xFF;
    const int rot = ((ins >> 8) & 15) << 1;
    val = rot ? ror(imm, rot) : imm;
  } else val = r[ins & 15];
  uint32_t mask = 0;
  if (ins & 0x00080000) mask |= 0xFF000000u;
  if (ins & 0x00010000) mask |= 0x000000FFu;
  if (ins & 0x00400000) {
    if (bankOf(mode) != 0) setSpsr(static_cast<int32_t>((U32(spsr()) & ~mask) | (U32(val) & mask)));
  } else {
    if (mode == MODE_USR) mask &= 0xFF000000u;
    const uint32_t nv = (U32(cpsr()) & ~mask) | (U32(val) & mask);
    setCpsr(static_cast<int32_t>((nv & ~0x20u) | (U32(t) << 5)));  // T não muda por MSR
  }
  return 0;
}

int CPU::armBx(uint32_t ins) {
  const int32_t val = r[ins & 15];
  t = val & 1;
  setPC(val & ~1);
  return 2;
}

int CPU::armMul(uint32_t ins) {
  const int rd = (ins >> 16) & 15, rn = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
  const int32_t rsv = r[rs];
  int32_t res = static_cast<int32_t>(U32(r[rm]) * U32(rsv));
  int cyc = mulCycles(rsv);
  if (ins & 0x00200000) { res = I32(int64_t(res) + r[rn]); cyc++; }
  if (ins & 0x00100000) setNZ(res);
  r[rd] = res;
  return cyc;
}

int CPU::armMulLong(uint32_t ins) {
  const int rdHi = (ins >> 16) & 15, rdLo = (ins >> 12) & 15, rs = (ins >> 8) & 15, rm = ins & 15;
  const bool isSigned = (ins & 0x00400000) != 0;
  const int32_t a = r[rm], b = r[rs];
  uint64_t prod = isSigned ? static_cast<uint64_t>(int64_t(a) * int64_t(b)) : uint64_t(U32(a)) * U32(b);
  int cyc = mulCycles(b) + 1;
  if (ins & 0x00200000) {
    prod += (uint64_t(U32(r[rdHi])) << 32) | U32(r[rdLo]);
    cyc++;
  }
  const uint32_t lo = static_cast<uint32_t>(prod), hi = static_cast<uint32_t>(prod >> 32);
  if (ins & 0x00100000) {
    n = static_cast<int>(hi >> 31);
    z = (lo == 0 && hi == 0) ? 1 : 0;
  }
  r[rdLo] = static_cast<int32_t>(lo);
  r[rdHi] = static_cast<int32_t>(hi);
  return cyc;
}

int CPU::armSwp(uint32_t ins) {
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15, rm = ins & 15;
  const int32_t addr = r[rn];
  const int32_t src = r[rm];
  int32_t tmp;
  if (ins & 0x00400000) {
    tmp = gba.bus.read8(addr);
    gba.bus.write8(addr, src);
  } else {
    tmp = ldrWord(addr);
    gba.bus.write32(addr, src);
  }
  r[rd] = tmp;
  return 1;
}

int CPU::armHalf(uint32_t ins) {
  const bool p = ins & 0x01000000, u = ins & 0x00800000, w = ins & 0x00200000, l = ins & 0x00100000;
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
  const int sh = (ins >> 5) & 3;
  const int32_t off = (ins & 0x00400000) ? static_cast<int32_t>(((ins >> 4) & 0xF0) | (ins & 0xF)) : r[ins & 15];
  const int32_t base = r[rn];
  const int32_t offAddr = u ? I32(int64_t(base) + off) : I32(int64_t(base) - off);
  const int32_t addr = p ? offAddr : base;
  if (l) {
    int32_t val;
    if (sh == 1) val = ldrHalf(addr);
    else if (sh == 2) val = static_cast<int8_t>(gba.bus.read8(addr));
    else val = ldrSHalf(addr);
    if (!p || w) r[rn] = offAddr;
    if (rd == 15) { setPC(val); return 3; }
    r[rd] = val;
    return 1;
  }
  if (sh == 1) {
    int64_t val = r[rd]; if (rd == 15) val += 4;
    gba.bus.write16(addr, val);
  }
  if (!p || w) r[rn] = offAddr;
  return 0;
}

int CPU::armSdt(uint32_t ins) {
  const bool p = ins & 0x01000000, u = ins & 0x00800000, b = ins & 0x00400000, w = ins & 0x00200000, l = ins & 0x00100000;
  const int rn = (ins >> 16) & 15, rd = (ins >> 12) & 15;
  int32_t off;
  if (ins & 0x02000000) {
    const int saveC = shC;
    off = shiftImm((ins >> 5) & 3, r[ins & 15], (ins >> 7) & 31);
    shC = saveC;
  } else off = ins & 0xFFF;
  const int32_t base = r[rn];
  const int32_t offAddr = u ? I32(int64_t(base) + off) : I32(int64_t(base) - off);
  const int32_t addr = p ? offAddr : base;
  if (l) {
    const int32_t val = b ? gba.bus.read8(addr) : ldrWord(addr);
    if (!p || w) r[rn] = offAddr;
    if (rd == 15) { setPC(val); return 3; }
    r[rd] = val;
    return 1;
  }
  int64_t val = r[rd]; if (rd == 15) val += 4;
  if (b) gba.bus.write8(addr, val); else gba.bus.write32(addr, val);
  if (!p || w) r[rn] = offAddr;
  return 0;
}

int CPU::armBlock(uint32_t ins) {
  const bool p = ins & 0x01000000, u = ins & 0x00800000, s = ins & 0x00400000, w = ins & 0x00200000, l = ins & 0x00100000;
  const int rn = (ins >> 16) & 15;
  int rlist = ins & 0xFFFF;
  const int32_t base = r[rn];
  int count = 0;
  for (int k = 0; k < 16; k++) if (rlist & (1 << k)) count++;
  if (rlist == 0) { rlist = 0x8000; count = 16; }
  int64_t addr;
  int32_t finalBase;
  if (u) { addr = p ? int64_t(base) + 4 : base; finalBase = I32(int64_t(base) + count * 4); }
  else { addr = p ? int64_t(base) - count * 4 : int64_t(base) - count * 4 + 4; finalBase = I32(int64_t(base) - count * 4); }
  addr = I32(addr);
  const bool userBank = s && !(l && (rlist & 0x8000));
  const int oldMode = mode;
  if (userBank) switchMode(MODE_USR);
  Bus& bus = gba.bus;
  if (l) {
    if (w) r[rn] = finalBase;
    for (int k = 0; k < 16; k++) {
      if (!(rlist & (1 << k))) continue;
      const int32_t val = bus.read32(I32(addr) & ~3);
      addr = I32(addr + 4);
      if (k == 15) {
        if (s) setCpsr(spsr());
        setPC(val);
      } else r[k] = val;
    }
    if (userBank) switchMode(oldMode);
    return 1;
  }
  bool first = true;
  for (int k = 0; k < 16; k++) {
    if (!(rlist & (1 << k))) continue;
    int64_t val;
    if (k == 15) val = int64_t(r[15]) + 4;
    else if (k == rn) val = first ? base : finalBase;
    else val = r[k];
    bus.write32(addr, val);
    addr = I32(addr + 4);
    first = false;
  }
  if (userBank) switchMode(oldMode);
  if (w) r[rn] = finalBase;
  return 0;
}

int CPU::armBranch(uint32_t ins) {
  const int32_t off = static_cast<int32_t>(ins << 8) >> 6;
  if (ins & 0x01000000) r[14] = I32(int64_t(r[15]) - 4);
  setPC(I32(int64_t(r[15]) + off));
  return 2;
}

int CPU::armSwi(uint32_t ins) {
  const int64_t ret = int64_t(r[15]) - 4;
  if (hle) { gba.hle.handle(static_cast<int>((ins >> 16) & 0xFF), ret - 4); return 2; }
  swiException(I32(ret));
  return 2;
}

// =====================================================================
//                              THUMB
// =====================================================================
CPU::Op CPU::decodeThumb(uint32_t ins) {
  const uint32_t top = ins >> 8;
  if ((ins & 0xF800) == 0x1800) return &CPU::thAddSub;
  if ((ins & 0xE000) == 0x0000) return &CPU::thShift;
  if ((ins & 0xE000) == 0x2000) return &CPU::thImm;
  if ((ins & 0xFC00) == 0x4000) return &CPU::thAlu;
  if ((ins & 0xFC00) == 0x4400) return &CPU::thHi;
  if ((ins & 0xF800) == 0x4800) return &CPU::thLdrPc;
  if ((ins & 0xF200) == 0x5000) return &CPU::thLsReg;
  if ((ins & 0xF200) == 0x5200) return &CPU::thLsSign;
  if ((ins & 0xE000) == 0x6000) return &CPU::thLsImm;
  if ((ins & 0xF000) == 0x8000) return &CPU::thLsHalf;
  if ((ins & 0xF000) == 0x9000) return &CPU::thLsSp;
  if ((ins & 0xF000) == 0xA000) return &CPU::thAddr;
  if (top == 0xB0) return &CPU::thAddSp;
  if ((ins & 0xF600) == 0xB400) return &CPU::thPushPop;
  if ((ins & 0xF000) == 0xC000) return &CPU::thLdmStm;
  if (top == 0xDF) return &CPU::thSwi;
  if ((ins & 0xF000) == 0xD000) return &CPU::thBcond;
  if ((ins & 0xF800) == 0xE000) return &CPU::thB;
  if ((ins & 0xF800) == 0xF000) return &CPU::thBlHi;
  if ((ins & 0xF800) == 0xF800) return &CPU::thBlLo;
  return &CPU::undefinedInstr;
}

int CPU::thShift(uint32_t ins) {
  const int rd = ins & 7, rs = (ins >> 3) & 7, amt = (ins >> 6) & 31;
  const int32_t res = shiftImm((ins >> 11) & 3, r[rs], amt);
  r[rd] = res;
  setNZ(res); c = shC;
  return 0;
}

int CPU::thAddSub(uint32_t ins) {
  const int rd = ins & 7, rs = (ins >> 3) & 7, x = (ins >> 6) & 7;
  const int32_t op2 = (ins & 0x400) ? x : r[x];
  r[rd] = (ins & 0x200) ? sub(r[rs], op2, true) : add(r[rs], op2, true);
  return 0;
}

int CPU::thImm(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  const int32_t imm = ins & 0xFF;
  switch ((ins >> 11) & 3) {
    case 0: r[rd] = imm; setNZ(imm); break;
    case 1: sub(r[rd], imm, true); break;
    case 2: r[rd] = add(r[rd], imm, true); break;
    default: r[rd] = sub(r[rd], imm, true); break;
  }
  return 0;
}

int CPU::thAlu(uint32_t ins) {
  const int rd = ins & 7, rs = (ins >> 3) & 7;
  const int32_t a = r[rd], b = r[rs];
  int32_t res;
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
    case 0xD: res = static_cast<int32_t>(U32(a) * U32(b)); setNZ(res); r[rd] = res; return mulCycles(a);
    case 0xE: res = a & ~b; setNZ(res); r[rd] = res; return 0;
    default: res = ~b; setNZ(res); r[rd] = res; return 0;
  }
}

int CPU::thHi(uint32_t ins) {
  const int rd = (ins & 7) | ((ins >> 4) & 8), rs = (ins >> 3) & 15;
  const int32_t sv = r[rs];
  switch ((ins >> 8) & 3) {
    case 0: {
      const int32_t res = I32(int64_t(r[rd]) + sv);
      if (rd == 15) { setPC(res & ~1); return 2; }
      r[rd] = res; return 0;
    }
    case 1: sub(r[rd], sv, true); return 0;
    case 2:
      if (rd == 15) { setPC(sv & ~1); return 2; }
      r[rd] = sv; return 0;
    default:
      t = sv & 1;
      setPC(sv & ~1);
      return 2;
  }
}

int CPU::thLdrPc(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  r[rd] = gba.bus.read32(I32(int64_t(r[15] & ~2) + ((ins & 0xFF) << 2)) & ~3);
  return 1;
}

int CPU::thLsReg(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7, ro = (ins >> 6) & 7;
  const int32_t addr = I32(int64_t(r[rb]) + r[ro]);
  switch ((ins >> 10) & 3) {
    case 0: gba.bus.write32(addr, r[rd]); return 0;
    case 1: gba.bus.write8(addr, r[rd]); return 0;
    case 2: r[rd] = ldrWord(addr); return 1;
    default: r[rd] = gba.bus.read8(addr); return 1;
  }
}

int CPU::thLsSign(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7, ro = (ins >> 6) & 7;
  const int32_t addr = I32(int64_t(r[rb]) + r[ro]);
  switch ((ins >> 10) & 3) {
    case 0: gba.bus.write16(addr, r[rd]); return 0;
    case 1: r[rd] = static_cast<int8_t>(gba.bus.read8(addr)); return 1;
    case 2: r[rd] = ldrHalf(addr); return 1;
    default: r[rd] = ldrSHalf(addr); return 1;
  }
}

int CPU::thLsImm(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7, off = (ins >> 6) & 31;
  if (ins & 0x1000) {  // byte
    const int32_t addr = I32(int64_t(r[rb]) + off);
    if (ins & 0x800) { r[rd] = gba.bus.read8(addr); return 1; }
    gba.bus.write8(addr, r[rd]); return 0;
  }
  const int32_t addr = I32(int64_t(r[rb]) + (off << 2));
  if (ins & 0x800) { r[rd] = ldrWord(addr); return 1; }
  gba.bus.write32(addr, r[rd]); return 0;
}

int CPU::thLsHalf(uint32_t ins) {
  const int rd = ins & 7, rb = (ins >> 3) & 7;
  const int32_t addr = I32(int64_t(r[rb]) + (((ins >> 6) & 31) << 1));
  if (ins & 0x800) { r[rd] = ldrHalf(addr); return 1; }
  gba.bus.write16(addr, r[rd]); return 0;
}

int CPU::thLsSp(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  const int32_t addr = I32(int64_t(r[13]) + ((ins & 0xFF) << 2));
  if (ins & 0x800) { r[rd] = ldrWord(addr); return 1; }
  gba.bus.write32(addr, r[rd]); return 0;
}

int CPU::thAddr(uint32_t ins) {
  const int rd = (ins >> 8) & 7;
  const int32_t off = (ins & 0xFF) << 2;
  r[rd] = (ins & 0x800) ? I32(int64_t(r[13]) + off) : I32(int64_t(r[15] & ~2) + off);
  return 0;
}

int CPU::thAddSp(uint32_t ins) {
  const int32_t off = (ins & 0x7F) << 2;
  r[13] = (ins & 0x80) ? I32(int64_t(r[13]) - off) : I32(int64_t(r[13]) + off);
  return 0;
}

int CPU::thPushPop(uint32_t ins) {
  Bus& bus = gba.bus;
  const int rlist = ins & 0xFF;
  const bool extra = (ins & 0x100) != 0;
  if (ins & 0x800) {  // POP
    int64_t addr = r[13];
    if (rlist == 0 && !extra) {  // lista vazia: carrega PC, SP += 0x40
      const int32_t val = bus.read32(I32(addr) & ~3);
      r[13] = I32(addr + 0x40);
      setPC(val & ~1);
      return 2;
    }
    for (int k = 0; k < 8; k++) {
      if (rlist & (1 << k)) { r[k] = bus.read32(I32(addr) & ~3); addr += 4; }
    }
    if (extra) {
      const int32_t val = bus.read32(I32(addr) & ~3); addr += 4;
      setPC(val & ~1);
    }
    r[13] = I32(addr);
    return extra ? 3 : 1;
  }
  // PUSH
  if (rlist == 0 && !extra) {
    r[13] = I32(int64_t(r[13]) - 0x40);
    bus.write32(r[13] & ~3, int64_t(r[15]) + 2);
    return 0;
  }
  int count = extra ? 1 : 0;
  for (int k = 0; k < 8; k++) if (rlist & (1 << k)) count++;
  int64_t addr = I32(int64_t(r[13]) - count * 4);
  r[13] = I32(addr);
  for (int k = 0; k < 8; k++) {
    if (rlist & (1 << k)) { bus.write32(addr, r[k]); addr += 4; }
  }
  if (extra) bus.write32(addr, r[14]);
  return 0;
}

int CPU::thLdmStm(uint32_t ins) {
  Bus& bus = gba.bus;
  const int rb = (ins >> 8) & 7;
  const int rlist = ins & 0xFF;
  int64_t addr = r[rb];
  if (rlist == 0) {
    if (ins & 0x800) { const int32_t val = bus.read32(I32(addr) & ~3); setPC(val & ~1); }
    else bus.write32(addr, int64_t(r[15]) + 2);
    r[rb] = I32(addr + 0x40);
    return 1;
  }
  int count = 0;
  for (int k = 0; k < 8; k++) if (rlist & (1 << k)) count++;
  const int32_t finalAddr = I32(addr + count * 4);
  if (ins & 0x800) {
    for (int k = 0; k < 8; k++) {
      if (rlist & (1 << k)) { r[k] = bus.read32(I32(addr) & ~3); addr += 4; }
    }
    if (!(rlist & (1 << rb))) r[rb] = finalAddr;
    return 1;
  }
  bool first = true;
  for (int k = 0; k < 8; k++) {
    if (rlist & (1 << k)) {
      const int32_t val = (k == rb) ? (first ? r[rb] : finalAddr) : r[k];
      bus.write32(addr, val); addr += 4; first = false;
    }
  }
  r[rb] = finalAddr;
  return 0;
}

int CPU::thBcond(uint32_t ins) {
  if (!checkCond((ins >> 8) & 15)) return 0;
  const int32_t off = static_cast<int32_t>((ins & 0xFF) << 24) >> 23;
  setPC(I32(int64_t(r[15]) + off));
  return 2;
}

int CPU::thSwi(uint32_t ins) {
  const int64_t ret = int64_t(r[15]) - 2;
  if (hle) { gba.hle.handle(static_cast<int>(ins & 0xFF), ret - 2); return 2; }
  swiException(I32(ret));
  return 2;
}

int CPU::thB(uint32_t ins) {
  const int32_t off = static_cast<int32_t>((ins & 0x7FF) << 21) >> 20;
  setPC(I32(int64_t(r[15]) + off));
  return 2;
}

int CPU::thBlHi(uint32_t ins) {
  const int32_t off = static_cast<int32_t>((ins & 0x7FF) << 21) >> 9;
  r[14] = I32(int64_t(r[15]) + off);
  return 0;
}

int CPU::thBlLo(uint32_t ins) {
  const int32_t next = I32(int64_t(r[15]) - 2) | 1;
  setPC(I32(int64_t(r[14]) + ((ins & 0x7FF) << 1)));
  r[14] = next;
  return 2;
}

void CPU::sync(StateIO& io) {
  io.raw(r, sizeof r); io.raw(bankR13, sizeof bankR13); io.raw(bankR14, sizeof bankR14); io.raw(spsrBank, sizeof spsrBank);
  io.raw(usrHi, sizeof usrHi); io.raw(fiqHi, sizeof fiqHi);
  for (int* p : {&n, &z, &c, &v, &i, &f, &t, &mode, &shC}) io.sync(*p);
  io.sync(nextPC); io.sync(halted); io.sync(branched);
  if (io.reading()) {
    for (int* p : {&n, &z, &c, &v, &i, &f, &t}) *p &= 1;
    mode = (mode & 0x1F) | 0x10;
    shC &= 1;
    nextPC = I32(nextPC);
  }
}

}  // namespace indigo::gba
