// CPU ARM60 (ARMv3), ver arm60.h.
#include "arm60.h"

namespace indigo::tdo {

int Arm60::bankOf(uint32_t mode) {
  switch (mode & 0x1F) {
    case FIQ: return 1;
    case IRQ: return 2;
    case SVC: return 3;
    case ABT: return 4;
    case UND: return 5;
    default: return 0;  // USR, SYS e modos inválidos usam o banco do usuário
  }
}

uint32_t& Arm60::spsrRef() {
  static uint32_t dummy;
  const int b = bankOf(cpsr);
  if (!b) { dummy = cpsr; return dummy; }
  return spsr_[b];
}

void Arm60::reset(uint32_t pc) {
  for (auto& x : r) x = 0;
  for (int i = 0; i < 6; i++) bankR13_[i] = bankR14_[i] = spsr_[i] = 0;
  for (int i = 0; i < 5; i++) fiqR8_[i] = usrR8_[i] = 0;
  cpsr = SVC | FI | FF;
  r[15] = pc & ~3u;
  cycles = 0;
  undefinedCount = 0;
  irqLine_ = fiqLine_ = false;
}

static bool validMode(uint32_t m) {
  return m == 0x10 || m == 0x11 || m == 0x12 || m == 0x13 || m == 0x17 || m == 0x1B || m == 0x1F;
}

void Arm60::setMode(uint32_t mode) {
  mode = (mode & 0x1F) | 0x10;
  if (!validMode(mode)) mode = SVC;  // modo inexistente: imprevisível no ARM6; aqui vira supervisor
  const uint32_t old = cpsr & 0x1F;
  if (old == mode) return;
  const int ob = bankOf(old), nb = bankOf(mode);
  // guarda os registradores do banco antigo
  bankR13_[ob] = r[13];
  bankR14_[ob] = r[14];
  if (ob == 1) { for (int i = 0; i < 5; i++) fiqR8_[i] = r[8 + i]; }
  else { for (int i = 0; i < 5; i++) usrR8_[i] = r[8 + i]; }
  // e traz os do novo
  r[13] = bankR13_[nb];
  r[14] = bankR14_[nb];
  if (nb == 1) { for (int i = 0; i < 5; i++) r[8 + i] = fiqR8_[i]; }
  else { for (int i = 0; i < 5; i++) r[8 + i] = usrR8_[i]; }
  cpsr = (cpsr & ~0x1Fu) | mode;
}

void Arm60::writeCpsr(uint32_t v) {
  setMode(v & 0x1F);
  cpsr = (v & ~0x1Fu) | (cpsr & 0x1F);
  cpsr = (cpsr & 0xF00000FFu);  // bits reservados ficam em zero
}

uint32_t& Arm60::userReg(int n) {
  const uint32_t m = cpsr & 0x1F;
  if (n >= 8 && n <= 12 && bankOf(m) == 1) return usrR8_[n - 8];
  if ((n == 13 || n == 14) && bankOf(m) != 0) return n == 13 ? bankR13_[0] : bankR14_[0];
  return r[n];
}

bool Arm60::cond(uint32_t c) const {
  const bool n = cpsr & FN, z = cpsr & FZ, cy = cpsr & FC, v = cpsr & FV;
  switch (c) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return cy;
    case 0x3: return !cy;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return cy && !z;
    case 0x9: return !cy || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return true;
    default: return false;  // NV: nunca executa no ARMv3
  }
}

void Arm60::exception(uint32_t vector, uint32_t mode, uint32_t lr, bool fiq) {
  const uint32_t old = cpsr;
  setMode(mode);
  spsr_[bankOf(mode)] = old;
  r[14] = lr;
  cpsr |= FI;
  if (fiq) cpsr |= FF;
  r[15] = vector;
  extra_ += 2;
}

void Arm60::undefined() {
  undefinedCount++;
  exception(0x04, UND, r[15], false);
}

uint32_t Arm60::load32(uint32_t a) {
  const uint32_t v = bus_.read32(a & ~3u);
  const uint32_t rot = (a & 3) * 8;
  return rot ? (v >> rot) | (v << (32 - rot)) : v;
}

int Arm60::run(int c) {
  int done = 0;
  while (done < c) done += step();
  return done;
}

int Arm60::step() {
  extra_ = 0;
  if (fiqLine_ && !(cpsr & FF)) {
    exception(0x1C, FIQ, r[15] + 4, true);
    cycles += 3;
    return 3;
  }
  if (irqLine_ && !(cpsr & FI)) {
    exception(0x18, IRQ, r[15] + 4, false);
    cycles += 3;
    return 3;
  }
  const uint32_t pc = r[15];
  const uint32_t op = bus_.read32(pc);
  r[15] = pc + 4;
  branched_ = false;
  inStep_ = true;
  abortPending_ = false;
  if (cond(op >> 28)) {
    switch ((op >> 25) & 7) {
      case 0:
        if ((op & 0x0FC000F0) == 0x00000090) multiply(op);
        else if ((op & 0x0FB00FF0) == 0x01000090) swap(op);
        else if ((op & 0x90) == 0x90) undefined();  // meia palavra / multiplicação longa: não existe no ARMv3
        else if ((op & 0x01900000) == 0x01000000) psrTransfer(op);
        else dataProc(op);
        break;
      case 1:
        if ((op & 0x01900000) == 0x01000000) psrTransfer(op);
        else dataProc(op);
        break;
      case 2: singleTransfer(op); break;
      case 3:
        if (op & 0x10) undefined();
        else singleTransfer(op);
        break;
      case 4: blockTransfer(op); break;
      case 5: {
        int32_t off = int32_t(op << 8) >> 6;
        if (op & (1u << 24)) r[14] = pc + 4;
        r[15] = pc + 8 + uint32_t(off);
        branched_ = true;
        break;
      }
      case 6: undefined(); break;  // LDC/STC: sem coprocessador
      case 7:
        if (op & (1u << 24)) exception(0x08, SVC, pc + 4, false);  // SWI
        else undefined();                                             // CDP/MRC/MCR
        break;
    }
  }
  inStep_ = false;
  if (resetPending_) {  // o reinício ganha de um "data abort" da mesma instrução
    resetPending_ = abortPending_ = false;
    reset(0);
    return 1;
  }
  if (abortPending_) {  // acesso recusado pelo barramento: "data abort"
    abortPending_ = false;
    exception(0x10, ABT, pc + 8, false);
    branched_ = true;
  }
  int c = 1 + extra_ + (branched_ ? 2 : 0);
  cycles += uint32_t(c);
  return c;
}

uint32_t Arm60::shiftOperand(uint32_t op, bool& carry) {
  carry = cpsr & FC;
  if (op & (1u << 25)) {
    const uint32_t imm = op & 0xFF, rot = ((op >> 8) & 0xF) * 2;
    if (!rot) return imm;
    const uint32_t v = (imm >> rot) | (imm << (32 - rot));
    carry = v >> 31;
    return v;
  }
  const int rm = op & 0xF;
  const uint32_t type = (op >> 5) & 3;
  uint32_t v;
  uint32_t amt;
  if (op & 0x10) {
    // deslocamento pelo registrador: um ciclo a mais, e o PC vale +12
    extra_ += 1;
    v = reg(rm, 12);
    amt = reg((op >> 8) & 0xF, 12) & 0xFF;
    if (amt == 0) return v;
    switch (type) {
      case 0:
        if (amt < 32) { carry = (v >> (32 - amt)) & 1; return v << amt; }
        carry = amt == 32 ? (v & 1) : false;
        return 0;
      case 1:
        if (amt < 32) { carry = (v >> (amt - 1)) & 1; return v >> amt; }
        carry = amt == 32 ? (v >> 31) : false;
        return 0;
      case 2:
        if (amt < 32) { carry = (v >> (amt - 1)) & 1; return uint32_t(int32_t(v) >> amt); }
        carry = v >> 31;
        return carry ? 0xFFFFFFFFu : 0;
      default: {
        const uint32_t a = amt & 31;
        if (a == 0) { carry = v >> 31; return v; }
        carry = (v >> (a - 1)) & 1;
        return (v >> a) | (v << (32 - a));
      }
    }
  }
  v = reg(rm);
  amt = (op >> 7) & 0x1F;
  switch (type) {
    case 0:
      if (!amt) return v;
      carry = (v >> (32 - amt)) & 1;
      return v << amt;
    case 1:
      if (!amt) { carry = v >> 31; return 0; }
      carry = (v >> (amt - 1)) & 1;
      return v >> amt;
    case 2:
      if (!amt) { carry = v >> 31; return carry ? 0xFFFFFFFFu : 0; }
      carry = (v >> (amt - 1)) & 1;
      return uint32_t(int32_t(v) >> amt);
    default:
      if (!amt) {  // RRX
        const bool c0 = cpsr & FC;
        carry = v & 1;
        return (v >> 1) | (c0 ? 0x80000000u : 0);
      }
      carry = (v >> (amt - 1)) & 1;
      return (v >> amt) | (v << (32 - amt));
  }
}

void Arm60::dataProc(uint32_t op) {
  const uint32_t opc = (op >> 21) & 0xF;
  const bool s = op & (1u << 20);
  const int rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
  bool sc;
  const uint32_t b = shiftOperand(op, sc);
  // com deslocamento por registrador o Rn=PC também vale +12
  const uint32_t a = reg(rn, (!(op & (1u << 25)) && (op & 0x10)) ? 12 : 8);
  uint32_t res = 0;
  bool c = cpsr & FC, v = cpsr & FV;
  bool logical = false, write = true;
  auto add = [&](uint32_t x, uint32_t y, uint32_t cin) {
    const uint64_t t = uint64_t(x) + y + cin;
    res = uint32_t(t);
    c = t >> 32;
    v = (~(x ^ y) & (x ^ res)) >> 31;
  };
  auto sub = [&](uint32_t x, uint32_t y, uint32_t cin) {  // x - y - !cin
    add(x, ~y, cin);
  };
  switch (opc) {
    case 0x0: res = a & b; logical = true; break;              // AND
    case 0x1: res = a ^ b; logical = true; break;              // EOR
    case 0x2: sub(a, b, 1); break;                             // SUB
    case 0x3: sub(b, a, 1); break;                             // RSB
    case 0x4: add(a, b, 0); break;                             // ADD
    case 0x5: add(a, b, (cpsr & FC) ? 1 : 0); break;           // ADC
    case 0x6: sub(a, b, (cpsr & FC) ? 1 : 0); break;           // SBC
    case 0x7: sub(b, a, (cpsr & FC) ? 1 : 0); break;           // RSC
    case 0x8: res = a & b; logical = true; write = false; break;  // TST
    case 0x9: res = a ^ b; logical = true; write = false; break;  // TEQ
    case 0xA: sub(a, b, 1); write = false; break;                 // CMP
    case 0xB: add(a, b, 0); write = false; break;                 // CMN
    case 0xC: res = a | b; logical = true; break;              // ORR
    case 0xD: res = b; logical = true; break;                  // MOV
    case 0xE: res = a & ~b; logical = true; break;             // BIC
    case 0xF: res = ~b; logical = true; break;                 // MVN
  }
  if (logical) c = sc;
  if (s && rd == 15 && bankOf(cpsr)) {
    // S com destino PC num modo com SPSR: volta o CPSR do SPSR (retorno de exceção); vale também para TST/CMP com
    // Rd=15 (a forma TSTP/CMPP do código de 26 bits). No modo de usuário as flags mudam normalmente.
    if (write) setReg(15, res);
    writeCpsr(spsr_[bankOf(cpsr)]);
    return;
  }
  if (write) setReg(rd, res);
  if (s) {
    cpsr &= ~(FN | FZ | FC | FV);
    if (res >> 31) cpsr |= FN;
    if (!res) cpsr |= FZ;
    if (c) cpsr |= FC;
    if (v) cpsr |= FV;
  }
}

void Arm60::multiply(uint32_t op) {
  const int rd = (op >> 16) & 0xF, rn = (op >> 12) & 0xF, rs = (op >> 8) & 0xF, rm = op & 0xF;
  const uint32_t m = r[rs];
  uint32_t res = r[rm] * m;
  if (op & (1u << 21)) { res += r[rn]; extra_ += 1; }
  // tempo do multiplicador de Booth: 2 bits por ciclo até o resto do multiplicador ser zero
  uint32_t t = m;
  int n = 0;
  do { t >>= 2; n++; } while (t && n < 16);
  extra_ += n;
  if (rd != 15) r[rd] = res;
  if (op & (1u << 20)) {
    cpsr &= ~(FN | FZ);
    if (res >> 31) cpsr |= FN;
    if (!res) cpsr |= FZ;
  }
}

void Arm60::swap(uint32_t op) {
  const int rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF, rm = op & 0xF;
  const uint32_t addr = r[rn];
  const uint32_t src = r[rm];
  if (op & (1u << 22)) {
    const uint8_t v = bus_.read8(addr);
    bus_.write8(addr, uint8_t(src));
    setReg(rd, v);
  } else {
    const uint32_t v = load32(addr);
    bus_.write32(addr & ~3u, src);
    setReg(rd, v);
  }
  extra_ += 3;
}

void Arm60::psrTransfer(uint32_t op) {
  const bool useSpsr = op & (1u << 22);
  if (!(op & (1u << 21))) {
    // MRS
    if ((op & 0x0FBF0FFF) != 0x010F0000) { undefined(); return; }
    const int rd = (op >> 12) & 0xF;
    setReg(rd, useSpsr ? spsrFor(cpsr) : cpsr);
    return;
  }
  // MSR: campos f (bits 24-31) e c (bits 0-7)
  uint32_t v;
  if (op & (1u << 25)) {
    const uint32_t imm = op & 0xFF, rot = ((op >> 8) & 0xF) * 2;
    v = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
  } else {
    v = r[op & 0xF];
  }
  uint32_t mask = 0;
  if (op & (1u << 19)) mask |= 0xF0000000u;
  if (op & (1u << 16)) mask |= 0x000000FFu;
  const bool user = (cpsr & 0x1F) == USR;
  if (useSpsr) {
    const int b = bankOf(cpsr);
    if (b) spsr_[b] = ((spsr_[b] & ~mask) | (v & mask)) & 0xF00000FFu;
    return;
  }
  if (user) mask &= 0xF0000000u;  // no modo de usuário só as flags mudam
  writeCpsr((cpsr & ~mask) | (v & mask));
}

void Arm60::singleTransfer(uint32_t op) {
  const bool pre = op & (1u << 24), up = op & (1u << 23), byte = op & (1u << 22), wb = op & (1u << 21),
             load = op & (1u << 20);
  const int rn = (op >> 16) & 0xF, rd = (op >> 12) & 0xF;
  uint32_t off;
  if (op & (1u << 25)) {
    bool dummy;
    off = shiftOperand(op & ~(0x10u | (1u << 25)), dummy);  // registrador deslocado por imediato
  } else {
    off = op & 0xFFF;
  }
  const uint32_t base = reg(rn);
  const uint32_t moved = up ? base + off : base - off;
  const uint32_t addr = pre ? moved : base;
  if (load) {
    const uint32_t v = byte ? bus_.read8(addr) : load32(addr);
    if (!pre || wb) setReg(rn, moved);
    // "data abort": a base já andou (modelo do ARM6), mas o dado não chega ao registrador
    if (!abortPending_) setReg(rd, v);
    extra_ += 2;
  } else {
    const uint32_t v = reg(rd, 12);
    if (byte) bus_.write8(addr, uint8_t(v));
    else bus_.write32(addr & ~3u, v);
    if (!pre || wb) setReg(rn, moved);
    extra_ += 1;
  }
}

void Arm60::blockTransfer(uint32_t op) {
  const bool pre = op & (1u << 24), up = op & (1u << 23), psr = op & (1u << 22), wb = op & (1u << 21),
             load = op & (1u << 20);
  const int rn = (op >> 16) & 0xF;
  uint32_t list = op & 0xFFFF;
  const uint32_t base = r[rn];
  int n = 0;
  for (int i = 0; i < 16; i++) n += (list >> i) & 1;
  const bool empty = n == 0;
  if (empty) { list = 1u << 15; n = 16; }  // lista vazia: transfere o PC e anda 64 bytes (como o ARM6/7)
  // endereço mais baixo e o novo valor da base
  uint32_t start;
  uint32_t newBase;
  if (up) { start = pre ? base + 4 : base; newBase = base + uint32_t(n) * 4; }
  else { start = pre ? base - uint32_t(n) * 4 : base - uint32_t(n) * 4 + 4; newBase = base - uint32_t(n) * 4; }
  const bool userBank = psr && (!load || !(list & (1u << 15)));
  uint32_t a = start & ~3u;
  bool first = true;
  if (load) {
    if (wb) r[rn] = newBase;
    for (int i = 0; i < 16; i++) {
      if (!(list & (1u << i))) continue;
      const uint32_t v = bus_.read32(a);
      a += 4;
      if (userBank) userReg(i) = v;
      else setReg(i, v);
    }
    if (psr && (list & (1u << 15)) && bankOf(cpsr)) writeCpsr(spsr_[bankOf(cpsr)]);
    extra_ += n + 1;
  } else {
    for (int i = 0; i < 16; i++) {
      if (!(list & (1u << i))) continue;
      uint32_t v;
      if (i == 15) v = r[15] + 8;  // PC + 12
      else if (i == rn && !first && wb) v = newBase;  // base já atualizada quando não é o primeiro da lista
      else if (userBank) v = userReg(i);
      else v = r[i];
      bus_.write32(a, v);
      a += 4;
      first = false;
    }
    if (wb) r[rn] = newBase;
    extra_ += n;
  }
  (void)empty;
}

void Arm60::sync(StateIO& io) {
  for (auto& x : r) io.sync(x);
  io.sync(cpsr);
  io.sync(cycles);
  io.sync(undefinedCount);
  io.sync(irqLine_);
  io.sync(fiqLine_);
  for (int i = 0; i < 6; i++) { io.sync(bankR13_[i]); io.sync(bankR14_[i]); io.sync(spsr_[i]); }
  for (int i = 0; i < 5; i++) { io.sync(fiqR8_[i]); io.sync(usrR8_[i]); }
  if (io.reading()) {
    cpsr = (cpsr & 0xF00000FFu) | 0x10;
    if (!validMode(cpsr & 0x1F)) cpsr = (cpsr & ~0x1Fu) | SVC;  // modo inválido vira supervisor
    r[15] &= ~3u;
    for (auto& s : spsr_) s &= 0xF00000FFu;
  }
}

}  // namespace indigo::tdo
