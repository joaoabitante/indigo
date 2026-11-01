#include "cpu.h"

namespace indigo::nes {

namespace {

enum Op : uint8_t {
  ADC, AND, ASL, BCC, BCS, BEQ, BIT, BMI, BNE, BPL, BRK, BVC, BVS, CLC, CLD, CLI, CLV, CMP, CPX, CPY, DEC, DEX, DEY,
  EOR, INC, INX, INY, JMP, JSR, LDA, LDX, LDY, LSR, NOP, ORA, PHA, PHP, PLA, PLP, ROL, ROR, RTI, RTS, SBC, SEC, SED,
  SEI, STA, STX, STY, TAX, TAY, TSX, TXA, TXS, TYA,
  // ilegais
  SLO, RLA, SRE, RRA, SAX, LAX, DCP, ISC, ANC, ALR, ARR, AXS, LAS, XAA, LXA, AHX, TAS, SHX, SHY, JAM,
};
enum Mode : uint8_t { IMP, ACC, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };

struct Entry { Op op; Mode mode; };

// tabela padrão do 6502 NMOS (linhas = nibble alto do opcode)
constexpr Entry T[256] = {
    {BRK, IMP}, {ORA, IZX}, {JAM, IMP}, {SLO, IZX}, {NOP, ZP},  {ORA, ZP},  {ASL, ZP},  {SLO, ZP},
    {PHP, IMP}, {ORA, IMM}, {ASL, ACC}, {ANC, IMM}, {NOP, ABS}, {ORA, ABS}, {ASL, ABS}, {SLO, ABS},
    {BPL, REL}, {ORA, IZY}, {JAM, IMP}, {SLO, IZY}, {NOP, ZPX}, {ORA, ZPX}, {ASL, ZPX}, {SLO, ZPX},
    {CLC, IMP}, {ORA, ABY}, {NOP, IMP}, {SLO, ABY}, {NOP, ABX}, {ORA, ABX}, {ASL, ABX}, {SLO, ABX},
    {JSR, ABS}, {AND, IZX}, {JAM, IMP}, {RLA, IZX}, {BIT, ZP},  {AND, ZP},  {ROL, ZP},  {RLA, ZP},
    {PLP, IMP}, {AND, IMM}, {ROL, ACC}, {ANC, IMM}, {BIT, ABS}, {AND, ABS}, {ROL, ABS}, {RLA, ABS},
    {BMI, REL}, {AND, IZY}, {JAM, IMP}, {RLA, IZY}, {NOP, ZPX}, {AND, ZPX}, {ROL, ZPX}, {RLA, ZPX},
    {SEC, IMP}, {AND, ABY}, {NOP, IMP}, {RLA, ABY}, {NOP, ABX}, {AND, ABX}, {ROL, ABX}, {RLA, ABX},
    {RTI, IMP}, {EOR, IZX}, {JAM, IMP}, {SRE, IZX}, {NOP, ZP},  {EOR, ZP},  {LSR, ZP},  {SRE, ZP},
    {PHA, IMP}, {EOR, IMM}, {LSR, ACC}, {ALR, IMM}, {JMP, ABS}, {EOR, ABS}, {LSR, ABS}, {SRE, ABS},
    {BVC, REL}, {EOR, IZY}, {JAM, IMP}, {SRE, IZY}, {NOP, ZPX}, {EOR, ZPX}, {LSR, ZPX}, {SRE, ZPX},
    {CLI, IMP}, {EOR, ABY}, {NOP, IMP}, {SRE, ABY}, {NOP, ABX}, {EOR, ABX}, {LSR, ABX}, {SRE, ABX},
    {RTS, IMP}, {ADC, IZX}, {JAM, IMP}, {RRA, IZX}, {NOP, ZP},  {ADC, ZP},  {ROR, ZP},  {RRA, ZP},
    {PLA, IMP}, {ADC, IMM}, {ROR, ACC}, {ARR, IMM}, {JMP, IND}, {ADC, ABS}, {ROR, ABS}, {RRA, ABS},
    {BVS, REL}, {ADC, IZY}, {JAM, IMP}, {RRA, IZY}, {NOP, ZPX}, {ADC, ZPX}, {ROR, ZPX}, {RRA, ZPX},
    {SEI, IMP}, {ADC, ABY}, {NOP, IMP}, {RRA, ABY}, {NOP, ABX}, {ADC, ABX}, {ROR, ABX}, {RRA, ABX},
    {NOP, IMM}, {STA, IZX}, {NOP, IMM}, {SAX, IZX}, {STY, ZP},  {STA, ZP},  {STX, ZP},  {SAX, ZP},
    {DEY, IMP}, {NOP, IMM}, {TXA, IMP}, {XAA, IMM}, {STY, ABS}, {STA, ABS}, {STX, ABS}, {SAX, ABS},
    {BCC, REL}, {STA, IZY}, {JAM, IMP}, {AHX, IZY}, {STY, ZPX}, {STA, ZPX}, {STX, ZPY}, {SAX, ZPY},
    {TYA, IMP}, {STA, ABY}, {TXS, IMP}, {TAS, ABY}, {SHY, ABX}, {STA, ABX}, {SHX, ABY}, {AHX, ABY},
    {LDY, IMM}, {LDA, IZX}, {LDX, IMM}, {LAX, IZX}, {LDY, ZP},  {LDA, ZP},  {LDX, ZP},  {LAX, ZP},
    {TAY, IMP}, {LDA, IMM}, {TAX, IMP}, {LXA, IMM}, {LDY, ABS}, {LDA, ABS}, {LDX, ABS}, {LAX, ABS},
    {BCS, REL}, {LDA, IZY}, {JAM, IMP}, {LAX, IZY}, {LDY, ZPX}, {LDA, ZPX}, {LDX, ZPY}, {LAX, ZPY},
    {CLV, IMP}, {LDA, ABY}, {TSX, IMP}, {LAS, ABY}, {LDY, ABX}, {LDA, ABX}, {LDX, ABY}, {LAX, ABY},
    {CPY, IMM}, {CMP, IZX}, {NOP, IMM}, {DCP, IZX}, {CPY, ZP},  {CMP, ZP},  {DEC, ZP},  {DCP, ZP},
    {INY, IMP}, {CMP, IMM}, {DEX, IMP}, {AXS, IMM}, {CPY, ABS}, {CMP, ABS}, {DEC, ABS}, {DCP, ABS},
    {BNE, REL}, {CMP, IZY}, {JAM, IMP}, {DCP, IZY}, {NOP, ZPX}, {CMP, ZPX}, {DEC, ZPX}, {DCP, ZPX},
    {CLD, IMP}, {CMP, ABY}, {NOP, IMP}, {DCP, ABY}, {NOP, ABX}, {CMP, ABX}, {DEC, ABX}, {DCP, ABX},
    {CPX, IMM}, {SBC, IZX}, {NOP, IMM}, {ISC, IZX}, {CPX, ZP},  {SBC, ZP},  {INC, ZP},  {ISC, ZP},
    {INX, IMP}, {SBC, IMM}, {NOP, IMP}, {SBC, IMM}, {CPX, ABS}, {SBC, ABS}, {INC, ABS}, {ISC, ABS},
    {BEQ, REL}, {SBC, IZY}, {JAM, IMP}, {ISC, IZY}, {NOP, ZPX}, {SBC, ZPX}, {INC, ZPX}, {ISC, ZPX},
    {SED, IMP}, {SBC, ABY}, {NOP, IMP}, {ISC, ABY}, {NOP, ABX}, {SBC, ABX}, {INC, ABX}, {ISC, ABX},
};

enum Kind { K_READ, K_WRITE, K_RMW, K_OTHER };
constexpr Kind kindOf(Op op) {
  switch (op) {
    case STA: case STX: case STY: case SAX: case AHX: case TAS: case SHX: case SHY: return K_WRITE;
    case ASL: case LSR: case ROL: case ROR: case INC: case DEC: case SLO: case RLA: case SRE: case RRA: case DCP: case ISC:
      return K_RMW;
    default: return K_READ;
  }
}

}  // namespace

void CPU::powerOn() {
  a = x = y = 0;
  s = 0;  // o reset em seguida desce 3: fica $FD
  p = 0x24;
  jammed = false;
  needNmi = prevNeedNmi = prevNmiLine = runIrq = prevRunIrq = false;
}

void CPU::reset() {
  // o reset é um BRK com as escritas na pilha viradas em leituras
  rd(pc); rd(pc);
  rd(0x100 | s); s--;
  rd(0x100 | s); s--;
  rd(0x100 | s); s--;
  p |= I;
  const uint8_t lo = rd(0xFFFC);
  pc = lo | (rd(0xFFFD) << 8);
  jammed = false;
  needNmi = prevNeedNmi = false;
}

void CPU::interrupt() {
  rd(pc); rd(pc);
  push(pc >> 8);
  push(pc & 0xFF);
  // o NMI ganha se chegar até aqui (inclusive "sequestrando" um IRQ já começado)
  const bool nmi = needNmi;
  if (nmi) needNmi = false;
  push((p | U) & ~B);
  p |= I;
  const uint16_t vec = nmi ? 0xFFFA : 0xFFFE;
  const uint8_t lo = rd(vec);
  pc = lo | (rd(vec + 1) << 8);
}

void CPU::branch(bool cond) {
  const int8_t off = static_cast<int8_t>(fetch());
  if (!cond) return;
  // desvio sem cruzar página: a interrupção que chegou no último ciclo fica para depois da próxima instrução
  if (runIrq && !prevRunIrq) runIrq = false;
  rd(pc);
  const uint16_t to = static_cast<uint16_t>(pc + off);
  if ((to & 0xFF00) != (pc & 0xFF00)) rd((pc & 0xFF00) | (to & 0xFF));
  pc = to;
}

void CPU::adc(uint8_t v) {
  const int c = p & C;
  if ((p & D) && decimalMode) {
    int t = (a & 0xF) + (v & 0xF) + c;
    if (t > 9) t += 6;
    t = t <= 0xF ? (t & 0xF) + (a & 0xF0) + (v & 0xF0) : (t & 0xF) + (a & 0xF0) + (v & 0xF0) + 0x10;
    p &= ~(C | Z | V | N);
    if (!((a + v + c) & 0xFF)) p |= Z;
    if (t & 0x80) p |= N;
    if (((a ^ t) & 0x80) && !((a ^ v) & 0x80)) p |= V;
    if ((t & 0x1F0) > 0x90) t += 0x60;
    if ((t & 0xFF0) > 0xF0) p |= C;
    a = static_cast<uint8_t>(t);
    return;
  }
  const int t = a + v + c;
  p &= ~(C | V);
  if (t > 0xFF) p |= C;
  if (~(a ^ v) & (a ^ t) & 0x80) p |= V;
  a = static_cast<uint8_t>(t);
  setZN(a);
}

void CPU::sbc(uint8_t v) {
  const int borrow = (p & C) ? 0 : 1;
  const int t = a - v - borrow;
  if ((p & D) && decimalMode) {
    int r = (a & 0xF) - (v & 0xF) - borrow;
    if (r & 0x10) r = ((r - 6) & 0xF) | ((a & 0xF0) - (v & 0xF0) - 0x10);
    else r = (r & 0xF) | ((a & 0xF0) - (v & 0xF0));
    if (r & 0x100) r -= 0x60;
    p &= ~(C | V);
    if (t >= 0) p |= C;
    if ((a ^ t) & (a ^ v) & 0x80) p |= V;
    setZN(static_cast<uint8_t>(t));
    a = static_cast<uint8_t>(r);
    return;
  }
  p &= ~(C | V);
  if (t >= 0) p |= C;
  if ((a ^ t) & (a ^ v) & 0x80) p |= V;
  a = static_cast<uint8_t>(t);
  setZN(a);
}

void CPU::cmp(uint8_t r, uint8_t v) {
  const int t = r - v;
  p = (p & ~C) | (t >= 0 ? C : 0);
  setZN(static_cast<uint8_t>(t));
}

// SHA/SHX/SHY/TAS: grava reg & (byte alto + 1); se o índice cruzou a página, o byte alto do endereço vira o valor
uint16_t CPU::storeUnstable(uint16_t base, uint8_t idx, uint8_t value, bool) {
  const uint16_t addr = static_cast<uint16_t>(base + idx);
  const uint8_t v = value & static_cast<uint8_t>((base >> 8) + 1);
  rd((base & 0xFF00) | (addr & 0xFF));
  const uint16_t to = ((base ^ addr) & 0xFF00) ? static_cast<uint16_t>((v << 8) | (addr & 0xFF)) : addr;
  wr(to, v);
  return to;
}

void CPU::step() {
  if (jammed) { rd(0xFFFF); return; }
  const uint8_t opcode = fetch();
  const Entry e = T[opcode];
  const Kind kind = kindOf(e.op);

  // ---------------- instruções com sequência própria
  switch (e.op) {
    case BRK:
      fetch();
      push(pc >> 8);
      push(pc & 0xFF);
      {
        const bool nmi = needNmi;
        if (nmi) needNmi = false;
        push(p | B | U);
        p |= I;
        const uint16_t vec = nmi ? 0xFFFA : 0xFFFE;
        const uint8_t lo = rd(vec);
        pc = lo | (rd(vec + 1) << 8);
      }
      // o BRK não confere interrupção no fim (a próxima instrução roda primeiro)
      prevRunIrq = false;
      prevNeedNmi = false;
      return;
    case JSR: {
      const uint8_t lo = fetch();
      rd(0x100 | s);
      push(pc >> 8);
      push(pc & 0xFF);
      pc = lo | (rd(pc) << 8);
      break;
    }
    case RTS: {
      rd(pc);
      rd(0x100 | s);
      const uint8_t lo = pull();
      pc = lo | (pull() << 8);
      rd(pc);
      pc++;
      break;
    }
    case RTI: {
      rd(pc);
      rd(0x100 | s);
      p = (pull() & ~B) | U;
      const uint8_t lo = pull();
      pc = lo | (pull() << 8);
      break;
    }
    case PHA: rd(pc); push(a); break;
    case PHP: rd(pc); push(p | B | U); break;
    case PLA: rd(pc); rd(0x100 | s); a = pull(); setZN(a); break;
    case PLP: rd(pc); rd(0x100 | s); p = (pull() & ~B) | U; break;
    case JMP:
      if (e.mode == ABS) {
        const uint8_t lo = fetch();
        pc = lo | (rd(pc) << 8);
      } else {
        const uint8_t lo = fetch();
        const uint16_t ptr = lo | (fetch() << 8);
        const uint8_t tlo = rd(ptr);
        pc = tlo | (rd((ptr & 0xFF00) | ((ptr + 1) & 0xFF)) << 8);
      }
      break;
    case BPL: branch(!(p & N)); break;
    case BMI: branch(p & N); break;
    case BVC: branch(!(p & V)); break;
    case BVS: branch(p & V); break;
    case BCC: branch(!(p & C)); break;
    case BCS: branch(p & C); break;
    case BNE: branch(!(p & Z)); break;
    case BEQ: branch(p & Z); break;
    case JAM:
      // trava até o reset (o barramento fica lendo $FFFF)
      rd(pc);
      rd(0xFFFF);
      rd(0xFFFE);
      rd(0xFFFE);
      jammed = true;
      pc--;
      return;
    default: break;
  }

  if (e.mode == IMP || e.mode == ACC) {
    switch (e.op) {
      case PHA: case PHP: case PLA: case PLP: case RTS: case RTI: break;
      default:
        rd(pc);
        switch (e.op) {
          case CLC: p &= ~C; break;
          case SEC: p |= C; break;
          case CLI: p &= ~I; break;
          case SEI: p |= I; break;
          case CLD: p &= ~D; break;
          case SED: p |= D; break;
          case CLV: p &= ~V; break;
          case TAX: x = a; setZN(x); break;
          case TAY: y = a; setZN(y); break;
          case TXA: a = x; setZN(a); break;
          case TYA: a = y; setZN(a); break;
          case TSX: x = s; setZN(x); break;
          case TXS: s = x; break;
          case INX: x++; setZN(x); break;
          case INY: y++; setZN(y); break;
          case DEX: x--; setZN(x); break;
          case DEY: y--; setZN(y); break;
          case ASL: p = (p & ~C) | (a >> 7); a <<= 1; setZN(a); break;
          case LSR: p = (p & ~C) | (a & 1); a >>= 1; setZN(a); break;
          case ROL: { const uint8_t c = p & C; p = (p & ~C) | (a >> 7); a = (a << 1) | c; setZN(a); break; }
          case ROR: { const uint8_t c = p & C; p = (p & ~C) | (a & 1); a = (a >> 1) | (c << 7); setZN(a); break; }
          default: break;  // NOP
        }
    }
  } else if (e.mode != REL && e.op != JMP && e.op != JSR) {
    // ---------------- endereço efetivo
    uint16_t addr = 0;
    uint8_t value = 0;
    bool haveValue = false;
    const bool alwaysDummy = kind != K_READ;
    switch (e.mode) {
      case IMM: value = fetch(); haveValue = true; break;
      case ZP: addr = fetch(); break;
      case ZPX: { const uint8_t b = fetch(); rd(b); addr = static_cast<uint8_t>(b + x); break; }
      case ZPY: { const uint8_t b = fetch(); rd(b); addr = static_cast<uint8_t>(b + y); break; }
      case ABS: { const uint8_t lo = fetch(); addr = lo | (fetch() << 8); break; }
      case ABX: case ABY: {
        const uint8_t lo = fetch();
        const uint16_t base = lo | (fetch() << 8);
        const uint8_t idx = e.mode == ABX ? x : y;
        if (e.op == SHY) { storeUnstable(base, x, y, true); return endStep(); }
        if (e.op == SHX) { storeUnstable(base, y, x, true); return endStep(); }
        if (e.op == AHX) { storeUnstable(base, y, a & x, true); return endStep(); }
        if (e.op == TAS) { s = a & x; storeUnstable(base, y, s, true); return endStep(); }
        addr = static_cast<uint16_t>(base + idx);
        if (alwaysDummy || ((base ^ addr) & 0xFF00)) rd((base & 0xFF00) | (addr & 0xFF));
        break;
      }
      case IZX: {
        uint8_t ptr = fetch();
        rd(ptr);
        ptr = static_cast<uint8_t>(ptr + x);
        const uint8_t lo = rd(ptr);
        addr = lo | (rd(static_cast<uint8_t>(ptr + 1)) << 8);
        break;
      }
      case IZY: {
        const uint8_t ptr = fetch();
        const uint8_t lo = rd(ptr);
        const uint16_t base = lo | (rd(static_cast<uint8_t>(ptr + 1)) << 8);
        if (e.op == AHX) { storeUnstable(base, y, a & x, true); return endStep(); }
        addr = static_cast<uint16_t>(base + y);
        if (alwaysDummy || ((base ^ addr) & 0xFF00)) rd((base & 0xFF00) | (addr & 0xFF));
        break;
      }
      default: break;
    }

    if (kind == K_WRITE) {
      switch (e.op) {
        case STA: wr(addr, a); break;
        case STX: wr(addr, x); break;
        case STY: wr(addr, y); break;
        case SAX: wr(addr, a & x); break;
        default: break;
      }
    } else if (kind == K_RMW) {
      uint8_t v = rd(addr);
      wr(addr, v);
      switch (e.op) {
        case ASL: p = (p & ~C) | (v >> 7); v <<= 1; setZN(v); break;
        case LSR: p = (p & ~C) | (v & 1); v >>= 1; setZN(v); break;
        case ROL: { const uint8_t c = p & C; p = (p & ~C) | (v >> 7); v = (v << 1) | c; setZN(v); break; }
        case ROR: { const uint8_t c = p & C; p = (p & ~C) | (v & 1); v = (v >> 1) | (c << 7); setZN(v); break; }
        case INC: v++; setZN(v); break;
        case DEC: v--; setZN(v); break;
        case SLO: p = (p & ~C) | (v >> 7); v <<= 1; a |= v; setZN(a); break;
        case RLA: { const uint8_t c = p & C; p = (p & ~C) | (v >> 7); v = (v << 1) | c; a &= v; setZN(a); break; }
        case SRE: p = (p & ~C) | (v & 1); v >>= 1; a ^= v; setZN(a); break;
        case RRA: { const uint8_t c = p & C; p = (p & ~C) | (v & 1); v = (v >> 1) | (c << 7); adc(v); break; }
        case DCP: v--; cmp(a, v); break;
        case ISC: v++; sbc(v); break;
        default: break;
      }
      wr(addr, v);
    } else {
      if (!haveValue) value = rd(addr);
      const uint8_t v = value;
      switch (e.op) {
        case LDA: a = v; setZN(a); break;
        case LDX: x = v; setZN(x); break;
        case LDY: y = v; setZN(y); break;
        case LAX: a = x = v; setZN(a); break;
        case ORA: a |= v; setZN(a); break;
        case AND: a &= v; setZN(a); break;
        case EOR: a ^= v; setZN(a); break;
        case ADC: adc(v); break;
        case SBC: sbc(v); break;
        case CMP: cmp(a, v); break;
        case CPX: cmp(x, v); break;
        case CPY: cmp(y, v); break;
        case BIT: p = (p & ~(Z | V | N)) | ((a & v) ? 0 : Z) | (v & (V | N)); break;
        case ANC: a &= v; setZN(a); p = (p & ~C) | (a >> 7); break;
        case ALR: a &= v; p = (p & ~C) | (a & 1); a >>= 1; setZN(a); break;
        case ARR: {
          const uint8_t t = a & v;
          const uint8_t c = p & C;
          if ((p & D) && decimalMode) {
            int r = (t >> 1) | (c << 7);
            p &= ~(N | Z | V | C);
            if (c) p |= N;
            if (!r) p |= Z;
            if ((r ^ t) & 0x40) p |= V;
            if (((t & 0xF) + (t & 1)) > 5) r = (r & 0xF0) | ((r + 6) & 0xF);
            if (((t & 0xF0) + (t & 0x10)) > 0x50) { r = (r & 0x0F) | ((r + 0x60) & 0xF0); p |= C; }
            a = static_cast<uint8_t>(r);
          } else {
            a = static_cast<uint8_t>((t >> 1) | (c << 7));
            setZN(a);
            p &= ~(C | V);
            if (a & 0x40) p |= C;
            if (((a >> 6) ^ (a >> 5)) & 1) p |= V;
          }
          break;
        }
        case AXS: { const int t = (a & x) - v; x = static_cast<uint8_t>(t); p = (p & ~C) | (t >= 0 ? C : 0); setZN(x); break; }
        case LAS: a = x = s = v & s; setZN(a); break;
        case XAA: a = (a | magic) & x & v; setZN(a); break;
        case LXA: a = x = (a | magic) & v; setZN(a); break;
        default: break;  // NOP com operando
      }
    }
  }
  endStep();
}

void CPU::endStep() {
  if (prevRunIrq || prevNeedNmi) interrupt();
}

void CPU::sync(StateIO& io) {
  io.sync(pc); io.sync(a); io.sync(x); io.sync(y); io.sync(s); io.sync(p); io.sync(jammed);
  io.sync(needNmi); io.sync(prevNeedNmi); io.sync(prevNmiLine); io.sync(runIrq); io.sync(prevRunIrq);
  if (io.reading()) p |= U;
}

}  // namespace indigo::nes
