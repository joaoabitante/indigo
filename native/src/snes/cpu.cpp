// WDC 65816: as 256 instruções, nos dois modos, com os ciclos na ordem do chip.
#include "cpu.h"

namespace indigo::snes {

// ---------------------------------------------------------------- estado
void CPU::fixRegisters() {
  if (e) {
    p |= M | X;
    s = static_cast<uint16_t>(0x100 | (s & 0xFF));
  }
  if (p & X) { x &= 0xFF; y &= 0xFF; }
}

void CPU::reset() {
  e = true;
  p = static_cast<uint8_t>((p | I | M | X) & ~D);
  d = 0;
  dbr = pbr = 0;
  s = static_cast<uint16_t>(0x100 | (s & 0xFF));
  fixRegisters();
  waiting = stopped = false;
  nmiPending = false;
  // 7 ciclos: 2 internos, 3 "escritas" que viram leituras na pilha e o vetor
  idle(); idle();
  for (int k = 0; k < 3; k++) { bus->read(0x100 | (s & 0xFF)); s = static_cast<uint16_t>(0x100 | ((s - 1) & 0xFF)); }
  const uint8_t lo = bus->read(0xFFFC);
  pc = static_cast<uint16_t>(lo | (bus->read(0xFFFD) << 8));
}

void CPU::sync(StateIO& io) {
  io.sync(a); io.sync(x); io.sync(y); io.sync(s); io.sync(d); io.sync(pc);
  io.sync(dbr); io.sync(pbr); io.sync(p); io.sync(e); io.sync(waiting); io.sync(stopped);
  io.sync(nmiPending); io.sync(irqLine);
  if (io.reading()) fixRegisters();
}

// ---------------------------------------------------------------- memória e pilha
uint32_t CPU::at(const Op& o, int i) const {
  switch (o.kind) {
    case 0: return (o.addr + i) & 0xFFFFFF;
    case 1: return (o.addr + i) & 0xFFFF;
    default: return (o.addr & 0xFF00) | ((o.addr + i) & 0xFF);
  }
}

void CPU::push(uint8_t v) {
  bus->write(s, v);
  s = e ? static_cast<uint16_t>(0x100 | ((s - 1) & 0xFF)) : static_cast<uint16_t>(s - 1);
}

uint8_t CPU::pull() {
  s = e ? static_cast<uint16_t>(0x100 | ((s + 1) & 0xFF)) : static_cast<uint16_t>(s + 1);
  return bus->read(s);
}

// ---------------------------------------------------------------- endereçamento
CPU::Op CPU::direct(uint16_t off) const {
  // modo emulação com a página direta alinhada (DL = 0): o acesso fica preso na página, como no 6502
  if (e && !(d & 0xFF)) return {static_cast<uint32_t>((d & 0xFF00) | (off & 0xFF)), 2};
  return {static_cast<uint16_t>(d + off), 1};
}

CPU::Op CPU::aDp() { const uint8_t o = fetch(); dpPenalty(); return direct(o); }
CPU::Op CPU::aDpX() { const uint8_t o = fetch(); dpPenalty(); idle(); return direct(static_cast<uint16_t>(o + x)); }
CPU::Op CPU::aDpY() { const uint8_t o = fetch(); dpPenalty(); idle(); return direct(static_cast<uint16_t>(o + y)); }

CPU::Op CPU::aDpInd() {
  const uint8_t o = fetch();
  dpPenalty();
  const Op ptr = direct(o);
  const uint8_t lo = rd(ptr, 0), hi = rd(ptr, 1);
  return {(uint32_t(dbr) << 16) | (hi << 8) | lo, 0};
}

CPU::Op CPU::aDpIndX() {
  const uint8_t o = fetch();
  dpPenalty();
  idle();
  const Op ptr = direct(static_cast<uint16_t>(o + x));
  const uint8_t lo = rd(ptr, 0), hi = rd(ptr, 1);
  return {(uint32_t(dbr) << 16) | (hi << 8) | lo, 0};
}

CPU::Op CPU::aDpIndY(bool write) {
  const uint8_t o = fetch();
  dpPenalty();
  const Op ptr = direct(o);
  const uint8_t lo = rd(ptr, 0), hi = rd(ptr, 1);
  const uint32_t base = (uint32_t(dbr) << 16) | (hi << 8) | lo;
  const uint32_t ea = (base + y) & 0xFFFFFF;
  if (write || !x8() || ((base ^ ea) & 0xFFFF00)) idleAt((base & 0xFFFF00) | ((base + y) & 0xFF));
  return {ea, 0};
}

CPU::Op CPU::aDpIndLong() {
  const uint8_t o = fetch();
  dpPenalty();
  const Op ptr = directN(o);
  const uint8_t lo = rd(ptr, 0), hi = rd(ptr, 1), bank = rd(ptr, 2);
  return {(uint32_t(bank) << 16) | (hi << 8) | lo, 0};
}

CPU::Op CPU::aDpIndLongY() {
  Op o = aDpIndLong();
  o.addr = (o.addr + y) & 0xFFFFFF;
  return o;
}

CPU::Op CPU::aAbs() { const uint16_t w = fetch16(); return {(uint32_t(dbr) << 16) | w, 0}; }

CPU::Op CPU::aAbsX(bool write) {
  const uint16_t w = fetch16();
  const uint32_t base = (uint32_t(dbr) << 16) | w;
  const uint32_t ea = (base + x) & 0xFFFFFF;
  if (write || !x8() || ((base ^ ea) & 0xFFFF00)) idleAt((base & 0xFFFF00) | ((base + x) & 0xFF));
  return {ea, 0};
}

CPU::Op CPU::aAbsY(bool write) {
  const uint16_t w = fetch16();
  const uint32_t base = (uint32_t(dbr) << 16) | w;
  const uint32_t ea = (base + y) & 0xFFFFFF;
  if (write || !x8() || ((base ^ ea) & 0xFFFF00)) idleAt((base & 0xFFFF00) | ((base + y) & 0xFF));
  return {ea, 0};
}

CPU::Op CPU::aLong() {
  const uint8_t lo = fetch(), hi = fetch(), bank = fetch();
  return {(uint32_t(bank) << 16) | (hi << 8) | lo, 0};
}

CPU::Op CPU::aLongX() { Op o = aLong(); o.addr = (o.addr + x) & 0xFFFFFF; return o; }

CPU::Op CPU::aSr() { const uint8_t o = fetch(); idle(); return {static_cast<uint16_t>(s + o), 1}; }

CPU::Op CPU::aSrIndY() {
  const uint8_t o = fetch();
  idle();
  const Op ptr{static_cast<uint16_t>(s + o), 1};
  const uint8_t lo = rd(ptr, 0), hi = rd(ptr, 1);
  idleAt(at(ptr, 1));
  const uint32_t base = (uint32_t(dbr) << 16) | (hi << 8) | lo;
  return {(base + y) & 0xFFFFFF, 0};
}

// ---------------------------------------------------------------- aritmética
void CPU::adc(uint16_t v) {
  const int c = p & C;
  int r;
  if (m8()) {
    const int al = a & 0xFF;
    v &= 0xFF;
    if (!(p & D)) {
      r = al + v + c;
    } else {
      r = (al & 0x0F) + (v & 0x0F) + c;
      if (r > 0x09) r += 0x06;
      const int c1 = r > 0x0F;
      r = (al & 0xF0) + (v & 0xF0) + (c1 << 4) + (r & 0x0F);
    }
    const bool ov = ~(al ^ v) & (al ^ r) & 0x80;
    if ((p & D) && r > 0x9F) r += 0x60;
    p = static_cast<uint8_t>((p & ~(C | V)) | (r > 0xFF ? C : 0) | (ov ? V : 0));
    a = static_cast<uint16_t>((a & 0xFF00) | (r & 0xFF));
    setZN8(static_cast<uint8_t>(r));
  } else {
    const int aw = a;
    if (!(p & D)) {
      r = aw + v + c;
    } else {
      r = (aw & 0x000F) + (v & 0x000F) + c;
      if (r > 0x0009) r += 0x0006;
      int cc = r > 0x000F;
      r = (aw & 0x00F0) + (v & 0x00F0) + (cc << 4) + (r & 0x000F);
      if (r > 0x009F) r += 0x0060;
      cc = r > 0x00FF;
      r = (aw & 0x0F00) + (v & 0x0F00) + (cc << 8) + (r & 0x00FF);
      if (r > 0x09FF) r += 0x0600;
      cc = r > 0x0FFF;
      r = (aw & 0xF000) + (v & 0xF000) + (cc << 12) + (r & 0x0FFF);
    }
    const bool ov = ~(aw ^ v) & (aw ^ r) & 0x8000;
    if ((p & D) && r > 0x9FFF) r += 0x6000;
    p = static_cast<uint8_t>((p & ~(C | V)) | (r > 0xFFFF ? C : 0) | (ov ? V : 0));
    a = static_cast<uint16_t>(r);
    setZN16(a);
  }
}

void CPU::sbc(uint16_t v) {
  const int c = p & C;
  int r;
  if (m8()) {
    const int al = a & 0xFF;
    const int iv = ~v & 0xFF;
    if (!(p & D)) {
      r = al + iv + c;
    } else {
      r = (al & 0x0F) + (iv & 0x0F) + c;
      if (r <= 0x0F) r -= 0x06;
      const int c1 = r > 0x0F;
      r = (al & 0xF0) + (iv & 0xF0) + (c1 << 4) + (r & 0x0F);
    }
    const bool ov = ~(al ^ iv) & (al ^ r) & 0x80;
    if ((p & D) && r <= 0xFF) r -= 0x60;
    p = static_cast<uint8_t>((p & ~(C | V)) | (r > 0xFF ? C : 0) | (ov ? V : 0));
    a = static_cast<uint16_t>((a & 0xFF00) | (r & 0xFF));
    setZN8(static_cast<uint8_t>(r));
  } else {
    const int aw = a;
    const int iv = ~v & 0xFFFF;
    if (!(p & D)) {
      r = aw + iv + c;
    } else {
      r = (aw & 0x000F) + (iv & 0x000F) + c;
      if (r <= 0x000F) r -= 0x0006;
      int cc = r > 0x000F;
      r = (aw & 0x00F0) + (iv & 0x00F0) + (cc << 4) + (r & 0x000F);
      if (r <= 0x00FF) r -= 0x0060;
      cc = r > 0x00FF;
      r = (aw & 0x0F00) + (iv & 0x0F00) + (cc << 8) + (r & 0x00FF);
      if (r <= 0x0FFF) r -= 0x0600;
      cc = r > 0x0FFF;
      r = (aw & 0xF000) + (iv & 0xF000) + (cc << 12) + (r & 0x0FFF);
    }
    const bool ov = ~(aw ^ iv) & (aw ^ r) & 0x8000;
    if ((p & D) && r <= 0xFFFF) r -= 0x6000;
    p = static_cast<uint8_t>((p & ~(C | V)) | (r > 0xFFFF ? C : 0) | (ov ? V : 0));
    a = static_cast<uint16_t>(r);
    setZN16(a);
  }
}

void CPU::compare(uint16_t r, uint16_t v, bool w8) {
  if (w8) {
    const int t = (r & 0xFF) - (v & 0xFF);
    p = static_cast<uint8_t>((p & ~C) | (t >= 0 ? C : 0));
    setZN8(static_cast<uint8_t>(t));
  } else {
    const int t = r - v;
    p = static_cast<uint8_t>((p & ~C) | (t >= 0 ? C : 0));
    setZN16(static_cast<uint16_t>(t));
  }
}

void CPU::bit(uint16_t v, bool immediate) {
  if (m8()) {
    p = static_cast<uint8_t>((p & ~Z) | ((a & v & 0xFF) ? 0 : Z));
    if (!immediate) p = static_cast<uint8_t>((p & ~(N | V)) | (v & (N | V)));
  } else {
    p = static_cast<uint8_t>((p & ~Z) | ((a & v) ? 0 : Z));
    if (!immediate) p = static_cast<uint8_t>((p & ~(N | V)) | ((v >> 8) & (N | V)));
  }
}

void CPU::alu(int kind, uint16_t v) {
  const bool w8 = m8();
  switch (kind) {
    case 0: loadReg(a, static_cast<uint16_t>(a | v), w8); break;
    case 1: loadReg(a, static_cast<uint16_t>(a & v), w8); break;
    case 2: loadReg(a, static_cast<uint16_t>(a ^ v), w8); break;
    case 3: adc(v); break;
    case 5: loadReg(a, v, w8); break;
    case 6: compare(a, v, w8); break;
    case 7: sbc(v); break;
    default: break;
  }
}

uint16_t CPU::modify(int kind, uint16_t v) {
  const bool w8 = m8();
  const uint16_t msb = w8 ? 0x80 : 0x8000, mask = w8 ? 0xFF : 0xFFFF;
  uint16_t r = v;
  switch (kind) {
    case 0: p = static_cast<uint8_t>((p & ~C) | ((v & msb) ? C : 0)); r = static_cast<uint16_t>(v << 1); break;
    case 1: { const int c = p & C; p = static_cast<uint8_t>((p & ~C) | ((v & msb) ? C : 0)); r = static_cast<uint16_t>((v << 1) | c); break; }
    case 2: p = static_cast<uint8_t>((p & ~C) | (v & 1)); r = static_cast<uint16_t>(v >> 1); break;
    case 3: { const int c = p & C; p = static_cast<uint8_t>((p & ~C) | (v & 1)); r = static_cast<uint16_t>((v >> 1) | (c ? msb : 0)); break; }
    case 4: r = static_cast<uint16_t>(v + 1); break;
    case 5: r = static_cast<uint16_t>(v - 1); break;
  }
  r &= mask;
  setZN(r, w8);
  return r;
}

void CPU::rmw(const Op& o, int kind) {
  const bool w8 = m8();
  const uint16_t v = readOp(o, w8);
  if (e) bus->write(at(o, 0), static_cast<uint8_t>(v));  // modo emulação: escrita falsa do valor antigo (6502)
  else idleAt(at(o, w8 ? 0 : 1));
  const uint16_t r = modify(kind, v);
  if (!w8) wr(o, 1, static_cast<uint8_t>(r >> 8));
  wr(o, 0, static_cast<uint8_t>(r));
}

void CPU::tsbTrb(const Op& o, bool set) {
  const bool w8 = m8();
  const uint16_t v = readOp(o, w8);
  if (e) bus->write(at(o, 0), static_cast<uint8_t>(v));
  else idleAt(at(o, w8 ? 0 : 1));
  const uint16_t am = w8 ? (a & 0xFF) : a;
  p = static_cast<uint8_t>((p & ~Z) | ((am & v) ? 0 : Z));
  const uint16_t r = set ? static_cast<uint16_t>(v | am) : static_cast<uint16_t>(v & ~am);
  if (!w8) wr(o, 1, static_cast<uint8_t>(r >> 8));
  wr(o, 0, static_cast<uint8_t>(r));
}

void CPU::branch(bool cond) {
  const int8_t off = static_cast<int8_t>(fetch());
  if (!cond) return;
  const uint16_t target = static_cast<uint16_t>(pc + off);
  if (e && ((target ^ pc) & 0xFF00)) idle();
  idle();
  pc = target;
}

void CPU::interrupt(uint16_t nativeVector, uint16_t emuVector, bool software, bool brk) {
  if (software) fetch();  // byte de assinatura do BRK/COP
  else { bus->read(pcAddr()); idle(); }
  if (!e) push(pbr);
  push(static_cast<uint8_t>(pc >> 8));
  push(static_cast<uint8_t>(pc));
  push(e && !software ? static_cast<uint8_t>(p & ~0x10) : p);
  p = static_cast<uint8_t>((p | I) & ~D);
  pbr = 0;
  const uint16_t vec = e ? emuVector : nativeVector;
  const uint8_t lo = bus->read(vec);
  pc = static_cast<uint16_t>(lo | (bus->read(vec + 1) << 8));
}

// ---------------------------------------------------------------- execução
void CPU::step() {
  if (stopped) { idle(); return; }
  if (waiting) {
    if (!nmiPending && !irqLine) { idle(); return; }
    waiting = false;
    idle();
    idle();
  }
  if (nmiPending) {
    nmiPending = false;
    interrupt(0xFFEA, 0xFFFA, false, false);
    return;
  }
  if (irqLine && !(p & I)) {
    interrupt(0xFFEE, 0xFFFE, false, false);
    return;
  }
  opPc = pc;
  execute(fetch());
  if (e) s = static_cast<uint16_t>(0x100 | (s & 0xFF));
}

void CPU::execute(uint8_t op) {
  // ---------------- grupo das operações de acumulador (ORA, AND, EOR, ADC, STA, LDA, CMP, SBC)
  const int low = op & 0x1F;
  if (((low & 1) && low != 0x0B && low != 0x1B) || low == 0x12) {
    if (op == 0x89) { bit(imm(m8()), true); return; }  // BIT #
    const int kind = op >> 5;
    const bool store = kind == 4;
    Op o{};
    switch (low) {
      case 0x01: o = aDpIndX(); break;
      case 0x03: o = aSr(); break;
      case 0x05: o = aDp(); break;
      case 0x07: o = aDpIndLong(); break;
      case 0x09: alu(kind, imm(m8())); return;
      case 0x0D: o = aAbs(); break;
      case 0x0F: o = aLong(); break;
      case 0x11: o = aDpIndY(store); break;
      case 0x12: o = aDpInd(); break;
      case 0x13: o = aSrIndY(); break;
      case 0x15: o = aDpX(); break;
      case 0x17: o = aDpIndLongY(); break;
      case 0x19: o = aAbsY(store); break;
      case 0x1D: o = aAbsX(store); break;
      case 0x1F: o = aLongX(); break;
      default: return;
    }
    if (store) writeOp(o, m8(), a);
    else alu(kind, readOp(o, m8()));
    return;
  }

  switch (op) {
    // ---------------- deslocamentos, incrementos e TSB/TRB na memória
    case 0x06: rmw(aDp(), 0); break;
    case 0x0E: rmw(aAbs(), 0); break;
    case 0x16: rmw(aDpX(), 0); break;
    case 0x1E: rmw(aAbsX(true), 0); break;
    case 0x26: rmw(aDp(), 1); break;
    case 0x2E: rmw(aAbs(), 1); break;
    case 0x36: rmw(aDpX(), 1); break;
    case 0x3E: rmw(aAbsX(true), 1); break;
    case 0x46: rmw(aDp(), 2); break;
    case 0x4E: rmw(aAbs(), 2); break;
    case 0x56: rmw(aDpX(), 2); break;
    case 0x5E: rmw(aAbsX(true), 2); break;
    case 0x66: rmw(aDp(), 3); break;
    case 0x6E: rmw(aAbs(), 3); break;
    case 0x76: rmw(aDpX(), 3); break;
    case 0x7E: rmw(aAbsX(true), 3); break;
    case 0xE6: rmw(aDp(), 4); break;
    case 0xEE: rmw(aAbs(), 4); break;
    case 0xF6: rmw(aDpX(), 4); break;
    case 0xFE: rmw(aAbsX(true), 4); break;
    case 0xC6: rmw(aDp(), 5); break;
    case 0xCE: rmw(aAbs(), 5); break;
    case 0xD6: rmw(aDpX(), 5); break;
    case 0xDE: rmw(aAbsX(true), 5); break;
    case 0x04: tsbTrb(aDp(), true); break;
    case 0x0C: tsbTrb(aAbs(), true); break;
    case 0x14: tsbTrb(aDp(), false); break;
    case 0x1C: tsbTrb(aAbs(), false); break;
    case 0x0A: idle(); a = m8() ? static_cast<uint16_t>((a & 0xFF00) | modify(0, a & 0xFF)) : modify(0, a); break;
    case 0x2A: idle(); a = m8() ? static_cast<uint16_t>((a & 0xFF00) | modify(1, a & 0xFF)) : modify(1, a); break;
    case 0x4A: idle(); a = m8() ? static_cast<uint16_t>((a & 0xFF00) | modify(2, a & 0xFF)) : modify(2, a); break;
    case 0x6A: idle(); a = m8() ? static_cast<uint16_t>((a & 0xFF00) | modify(3, a & 0xFF)) : modify(3, a); break;
    case 0x1A: idle(); a = m8() ? static_cast<uint16_t>((a & 0xFF00) | modify(4, a & 0xFF)) : modify(4, a); break;
    case 0x3A: idle(); a = m8() ? static_cast<uint16_t>((a & 0xFF00) | modify(5, a & 0xFF)) : modify(5, a); break;

    // ---------------- X e Y
    case 0xA2: loadReg(x, imm(x8()), x8()); break;
    case 0xA6: loadReg(x, readOp(aDp(), x8()), x8()); break;
    case 0xAE: loadReg(x, readOp(aAbs(), x8()), x8()); break;
    case 0xB6: loadReg(x, readOp(aDpY(), x8()), x8()); break;
    case 0xBE: loadReg(x, readOp(aAbsY(false), x8()), x8()); break;
    case 0xA0: loadReg(y, imm(x8()), x8()); break;
    case 0xA4: loadReg(y, readOp(aDp(), x8()), x8()); break;
    case 0xAC: loadReg(y, readOp(aAbs(), x8()), x8()); break;
    case 0xB4: loadReg(y, readOp(aDpX(), x8()), x8()); break;
    case 0xBC: loadReg(y, readOp(aAbsX(false), x8()), x8()); break;
    case 0x86: writeOp(aDp(), x8(), x); break;
    case 0x8E: writeOp(aAbs(), x8(), x); break;
    case 0x96: writeOp(aDpY(), x8(), x); break;
    case 0x84: writeOp(aDp(), x8(), y); break;
    case 0x8C: writeOp(aAbs(), x8(), y); break;
    case 0x94: writeOp(aDpX(), x8(), y); break;
    case 0x64: writeOp(aDp(), m8(), 0); break;
    case 0x9C: writeOp(aAbs(), m8(), 0); break;
    case 0x74: writeOp(aDpX(), m8(), 0); break;
    case 0x9E: writeOp(aAbsX(true), m8(), 0); break;
    case 0xE0: compare(x, imm(x8()), x8()); break;
    case 0xE4: compare(x, readOp(aDp(), x8()), x8()); break;
    case 0xEC: compare(x, readOp(aAbs(), x8()), x8()); break;
    case 0xC0: compare(y, imm(x8()), x8()); break;
    case 0xC4: compare(y, readOp(aDp(), x8()), x8()); break;
    case 0xCC: compare(y, readOp(aAbs(), x8()), x8()); break;
    case 0x24: bit(readOp(aDp(), m8()), false); break;
    case 0x2C: bit(readOp(aAbs(), m8()), false); break;
    case 0x34: bit(readOp(aDpX(), m8()), false); break;
    case 0x3C: bit(readOp(aAbsX(false), m8()), false); break;
    case 0xE8: idle(); loadReg(x, static_cast<uint16_t>(x + 1), x8()); if (x8()) x &= 0xFF; break;
    case 0xC8: idle(); loadReg(y, static_cast<uint16_t>(y + 1), x8()); if (x8()) y &= 0xFF; break;
    case 0xCA: idle(); loadReg(x, static_cast<uint16_t>(x - 1), x8()); if (x8()) x &= 0xFF; break;
    case 0x88: idle(); loadReg(y, static_cast<uint16_t>(y - 1), x8()); if (x8()) y &= 0xFF; break;

    // ---------------- transferências
    case 0xAA: idle(); loadReg(x, a, x8()); if (x8()) x &= 0xFF; break;
    case 0xA8: idle(); loadReg(y, a, x8()); if (x8()) y &= 0xFF; break;
    case 0x8A: idle(); loadReg(a, x, m8()); break;
    case 0x98: idle(); loadReg(a, y, m8()); break;
    case 0x9B: idle(); loadReg(y, x, x8()); break;
    case 0xBB: idle(); loadReg(x, y, x8()); break;
    case 0x9A: idle(); s = e ? static_cast<uint16_t>(0x100 | (x & 0xFF)) : x; break;
    case 0xBA: idle(); loadReg(x, s, x8()); if (x8()) x &= 0xFF; break;
    case 0x5B: idle(); d = a; setZN16(d); break;
    case 0x7B: idle(); a = d; setZN16(a); break;
    case 0x1B: idle(); s = e ? static_cast<uint16_t>(0x100 | (a & 0xFF)) : a; break;
    case 0x3B: idle(); a = s; setZN16(a); break;
    case 0xEB: idle(); idle(); a = static_cast<uint16_t>((a >> 8) | (a << 8)); setZN8(static_cast<uint8_t>(a)); break;
    case 0xFB: {
      idle();
      const bool c = p & C;
      p = static_cast<uint8_t>((p & ~C) | (e ? C : 0));
      e = c;
      fixRegisters();
      break;
    }

    // ---------------- flags
    case 0x18: idle(); p &= ~C; break;
    case 0x38: idle(); p |= C; break;
    case 0x58: idle(); p &= ~I; break;
    case 0x78: idle(); p |= I; break;
    case 0xB8: idle(); p &= ~V; break;
    case 0xD8: idle(); p &= ~D; break;
    case 0xF8: idle(); p |= D; break;
    case 0xC2: { const uint8_t v = fetch(); idle(); p &= ~v; fixRegisters(); break; }
    case 0xE2: { const uint8_t v = fetch(); idle(); p |= v; fixRegisters(); break; }

    // ---------------- desvios
    case 0x10: branch(!(p & N)); break;
    case 0x30: branch(p & N); break;
    case 0x50: branch(!(p & V)); break;
    case 0x70: branch(p & V); break;
    case 0x90: branch(!(p & C)); break;
    case 0xB0: branch(p & C); break;
    case 0xD0: branch(!(p & Z)); break;
    case 0xF0: branch(p & Z); break;
    case 0x80: branch(true); break;
    case 0x82: { const uint16_t off = fetch16(); idleAt(pcAddr() - 1); pc = static_cast<uint16_t>(pc + off); break; }

    // ---------------- saltos e sub-rotinas
    case 0x4C: pc = fetch16(); break;
    case 0x5C: { const uint8_t lo = fetch(), hi = fetch(), bank = fetch(); pc = static_cast<uint16_t>(lo | (hi << 8)); pbr = bank; break; }
    case 0x6C: {
      const uint16_t w = fetch16();
      const uint8_t lo = bus->read(w);
      pc = static_cast<uint16_t>(lo | (bus->read(static_cast<uint16_t>(w + 1)) << 8));
      break;
    }
    case 0x7C: {
      const uint16_t w = fetch16();
      idleAt(pcAddr() - 1);
      const uint16_t ptr = static_cast<uint16_t>(w + x);
      const uint8_t lo = bus->read((uint32_t(pbr) << 16) | ptr);
      pc = static_cast<uint16_t>(lo | (bus->read((uint32_t(pbr) << 16) | static_cast<uint16_t>(ptr + 1)) << 8));
      break;
    }
    case 0xDC: {
      const uint16_t w = fetch16();
      const uint8_t lo = bus->read(w), hi = bus->read(static_cast<uint16_t>(w + 1)), bank = bus->read(static_cast<uint16_t>(w + 2));
      pc = static_cast<uint16_t>(lo | (hi << 8));
      pbr = bank;
      break;
    }
    case 0x20: {
      const uint16_t w = fetch16();
      idleAt(pcAddr() - 1);
      const uint16_t ret = static_cast<uint16_t>(pc - 1);
      push(static_cast<uint8_t>(ret >> 8));
      push(static_cast<uint8_t>(ret));
      pc = w;
      break;
    }
    case 0x22: {
      const uint8_t lo = fetch(), hi = fetch();
      pushN(pbr);
      idleAt(static_cast<uint16_t>(s + 1));
      const uint8_t bank = fetch();
      const uint16_t ret = static_cast<uint16_t>(pc - 1);
      pushN(static_cast<uint8_t>(ret >> 8));
      pushN(static_cast<uint8_t>(ret));
      pc = static_cast<uint16_t>(lo | (hi << 8));
      pbr = bank;
      break;
    }
    case 0xFC: {
      const uint8_t lo = fetch();
      pushN(static_cast<uint8_t>(pc >> 8));
      pushN(static_cast<uint8_t>(pc));
      const uint8_t hi = fetch();
      idleAt(pcAddr() - 1);
      const uint16_t ptr = static_cast<uint16_t>((lo | (hi << 8)) + x);
      const uint8_t tl = bus->read((uint32_t(pbr) << 16) | ptr);
      pc = static_cast<uint16_t>(tl | (bus->read((uint32_t(pbr) << 16) | static_cast<uint16_t>(ptr + 1)) << 8));
      break;
    }
    case 0x60: {
      idle(); idle();
      const uint8_t lo = pull(), hi = pull();
      idleAt(s);
      pc = static_cast<uint16_t>((lo | (hi << 8)) + 1);
      break;
    }
    case 0x6B: {
      idle(); idle();
      const uint8_t lo = pullN(), hi = pullN(), bank = pullN();
      pc = static_cast<uint16_t>((lo | (hi << 8)) + 1);
      pbr = bank;
      break;
    }
    case 0x40: {
      idle(); idle();
      p = pull();
      fixRegisters();
      const uint8_t lo = pull(), hi = pull();
      pc = static_cast<uint16_t>(lo | (hi << 8));
      if (!e) pbr = pull();
      break;
    }

    // ---------------- pilha
    case 0x48: idle(); if (!m8()) push(static_cast<uint8_t>(a >> 8)); push(static_cast<uint8_t>(a)); break;
    case 0xDA: idle(); if (!x8()) push(static_cast<uint8_t>(x >> 8)); push(static_cast<uint8_t>(x)); break;
    case 0x5A: idle(); if (!x8()) push(static_cast<uint8_t>(y >> 8)); push(static_cast<uint8_t>(y)); break;
    case 0x68: { idle(); idle(); const uint8_t lo = pull(); loadReg(a, m8() ? lo : static_cast<uint16_t>(lo | (pull() << 8)), m8()); break; }
    case 0xFA: { idle(); idle(); const uint8_t lo = pull(); loadReg(x, x8() ? lo : static_cast<uint16_t>(lo | (pull() << 8)), x8()); break; }
    case 0x7A: { idle(); idle(); const uint8_t lo = pull(); loadReg(y, x8() ? lo : static_cast<uint16_t>(lo | (pull() << 8)), x8()); break; }
    case 0x08: idle(); push(p); break;
    case 0x28: idle(); idle(); p = pull(); fixRegisters(); break;
    case 0x8B: idle(); push(dbr); break;
    case 0x4B: idle(); push(pbr); break;
    case 0xAB: idle(); idle(); dbr = pullN(); setZN8(dbr); break;
    case 0x0B: idle(); pushN(static_cast<uint8_t>(d >> 8)); pushN(static_cast<uint8_t>(d)); break;
    case 0x2B: { idle(); idle(); const uint8_t lo = pullN(); d = static_cast<uint16_t>(lo | (pullN() << 8)); setZN16(d); break; }
    case 0xF4: { const uint16_t w = fetch16(); pushN(static_cast<uint8_t>(w >> 8)); pushN(static_cast<uint8_t>(w)); break; }
    case 0xD4: {
      const uint8_t o = fetch();
      dpPenalty();
      const Op ptr = directN(o);
      const uint8_t lo = rd(ptr, 0), hi = rd(ptr, 1);
      pushN(hi);
      pushN(lo);
      break;
    }
    case 0x62: {
      const uint16_t w = fetch16();
      idleAt(pcAddr() - 1);
      const uint16_t v = static_cast<uint16_t>(pc + w);
      pushN(static_cast<uint8_t>(v >> 8));
      pushN(static_cast<uint8_t>(v));
      break;
    }

    // ---------------- cópia de blocos
    case 0x54: case 0x44: {
      const uint8_t dst = fetch(), src = fetch();
      dbr = dst;
      const uint8_t v = bus->read((uint32_t(src) << 16) | x);
      const uint32_t to = (uint32_t(dst) << 16) | y;
      bus->write(to, v);
      idleAt(to);
      const int adj = op == 0x54 ? 1 : -1;
      if (x8()) { x = static_cast<uint8_t>(x + adj); y = static_cast<uint8_t>(y + adj); }
      else { x = static_cast<uint16_t>(x + adj); y = static_cast<uint16_t>(y + adj); }
      idleAt(to);
      if (a--) pc = static_cast<uint16_t>(pc - 3);
      break;
    }

    // ---------------- interrupções e diversos
    case 0x00: interrupt(0xFFE6, 0xFFFE, true, true); break;
    case 0x02: interrupt(0xFFE4, 0xFFF4, true, false); break;
    case 0x42: idle(); pc++; break;  // WDM: reservado, 2 bytes (o segundo não é lido)
    case 0xEA: idle(); break;
    case 0xCB: idle(); idle(); waiting = true; break;
    case 0xDB: idle(); idle(); stopped = true; break;
    default: idle(); break;
  }
}

}  // namespace indigo::snes
