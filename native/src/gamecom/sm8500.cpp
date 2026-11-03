// CPU Sharp SM8500 (ver sm8500.h). Decodificação e ciclos adaptados do núcleo sm8500 do MAME
// (copyright Wilbert Pol, licença BSD-3-Clause; texto da licença em TERCEIROS.md).
#include "sm8500.h"

namespace indigo::gamecom {

namespace {
// número do registrador de 8 bits do par RRn (n = 0..7) -> endereço do byte alto
constexpr uint8_t B2W[8] = {0, 8, 2, 10, 4, 12, 6, 14};
}  // namespace

void Sm8500::reset() {
  for (auto& r : regs) r = 0;
  pc = 0x1020;
  halted = false;
  pending = 0;
  ie0 = ie1 = ir0 = ir1 = 0;
  sys = 0;
  ckc = ckcApplied = 0;
  ckcPending = false;
  stops = 0;
  ps0 = 0;
  ps1 = 0;
  sph = spl = 0;
}

// ------------------------------------------------------------------ memória como a CPU vê
uint8_t Sm8500::rd(uint16_t a) {
  if (a < 0x10) return regs[a + (ps0 & 0xF8)];
  if (a < 0x20) {
    switch (a) {
      case 0x10: return ie0;
      case 0x11: return ie1;
      case 0x12: return ir0;
      case 0x13: return ir1;
      case 0x19: return sys;
      case 0x1A: return ckc;
      case 0x1C: return sph;
      case 0x1D: return spl;
      case 0x1E: return ps0;
      case 0x1F: return ps1;
      default: break;
    }
  }
  return bus_->read(a);
}

void Sm8500::wr(uint16_t a, uint8_t v) {
  if (a < 0x10) { regs[a + (ps0 & 0xF8)] = v; return; }
  if (a < 0x20) {
    switch (a) {
      case 0x10: ie0 = v; return;
      case 0x11: ie1 = v; return;
      case 0x12: ir0 = v; dropCleared(); return;
      case 0x13: ir1 = v; dropCleared(); return;
      case 0x19: sys = v; return;
      case 0x1A: if (v != ckc) ckcPending = true; ckc = v; return;
      case 0x1C: sph = v; return;
      case 0x1D: spl = v; return;
      case 0x1E: ps0 = v; return;
      case 0x1F: ps1 = v; return;
      default: break;
    }
  }
  bus_->write(a, v);
}

void Sm8500::push(uint8_t v) {
  const uint16_t s = uint16_t(sp() - 1);
  setSp(s);
  wr(sp(), v);
}

uint8_t Sm8500::pop() {
  const uint8_t v = rd(sp());
  setSp(uint16_t(sp() + 1));
  return v;
}

// ------------------------------------------------------------------ interrupções
void Sm8500::irq(int line) {
  pending = uint16_t(pending | (1u << line));
  switch (line) {
    case IRQ_DMA: ir0 |= 0x80; break;
    case IRQ_TIM0: ir0 |= 0x40; break;
    case IRQ_EXT: ir0 |= 0x10; break;
    case IRQ_UART: ir0 |= 0x08; break;
    case IRQ_LCDC: ir0 |= 0x01; break;
    case IRQ_TIM1: ir1 |= 0x40; break;
    case IRQ_CK: ir1 |= 0x10; break;
    case IRQ_PIO: ir1 |= 0x04; break;
    default: break;
  }
}

void Sm8500::clearIrq(int line) { pending = uint16_t(pending & ~(1u << line)); }

// o programa zerou o bit em IR0/IR1: o pedido some (é o mesmo flag no hardware)
void Sm8500::dropCleared() {
  static constexpr struct { int line; bool hi; uint8_t bit; } MAP[] = {
    {IRQ_DMA, false, 0x80}, {IRQ_TIM0, false, 0x40}, {IRQ_EXT, false, 0x10}, {IRQ_UART, false, 0x08},
    {IRQ_LCDC, false, 0x01}, {IRQ_TIM1, true, 0x40}, {IRQ_CK, true, 0x10}, {IRQ_PIO, true, 0x04}};
  for (const auto& m : MAP)
    if (!((m.hi ? ir1 : ir0) & m.bit)) clearIrq(m.line);
}

void Sm8500::takeIrq(uint16_t vector) {
  push(uint8_t(pc));
  push(uint8_t(pc >> 8));
  push(ps1);
  ps1 &= uint8_t(~FI);
  pc = rdw(vector);
}

// atende a primeira linha pendente que puder; devolve true se desviou
bool Sm8500::checkIrqs() {
  if (!pending) return false;
  halted = false;  // qualquer pedido acorda do HALT
  const int pri = ps0 & 7;
  const bool on = ps1 & FI;
  for (int line = 0; line < 11; line++) {
    if (!(pending & (1u << line))) continue;
    uint16_t vec = 0;
    bool enabled = false, allowed = on;
    switch (line) {
      case IRQ_WDT: vec = 0x101C; enabled = true; allowed = true; break;
      case IRQ_ILL:
      case IRQ_NMI: vec = 0x101E; enabled = true; allowed = true; break;
      case IRQ_DMA: vec = 0x1000; enabled = ie0 & 0x80; break;
      case IRQ_TIM0: vec = 0x1002; enabled = ie0 & 0x40; break;
      case IRQ_EXT: vec = 0x1006; enabled = ie0 & 0x10; allowed = on && pri < 7; break;
      case IRQ_UART: vec = 0x1008; enabled = ie0 & 0x08; allowed = on && pri < 6; break;
      case IRQ_LCDC: vec = 0x100E; enabled = ie0 & 0x01; allowed = on && pri < 5; break;
      case IRQ_TIM1: vec = 0x1012; enabled = ie1 & 0x40; allowed = on && pri < 4; break;
      case IRQ_CK: vec = 0x1016; enabled = ie1 & 0x10; allowed = on && pri < 3; break;
      case IRQ_PIO: vec = 0x101A; enabled = ie1 & 0x04; allowed = on && pri < 2; break;
      default: break;
    }
    if (!enabled) { clearIrq(line); continue; }  // fonte desligada em IE: o pedido se perde (IR fica marcado)
    if (!allowed) continue;                       // espera: I = 0 ou prioridade mais alta em curso
    clearIrq(line);
    takeIrq(vec);
    return true;
  }
  return false;
}

bool Sm8500::cond(uint8_t op) const {
  const bool c = ps1 & FC, z = ps1 & FZ, s = ps1 & FS, v = ps1 & FV;
  switch (op & 0x0F) {
    case 0x0: return false;          // F
    case 0x1: return s != v;         // LT
    case 0x2: return z || (s != v);  // LE
    case 0x3: return z || c;         // ULE
    case 0x4: return v;              // OV
    case 0x5: return s;              // MI
    case 0x6: return z;              // Z (EQ)
    case 0x7: return c;              // C (ULT)
    case 0x8: return true;           // T
    case 0x9: return s == v;         // GE
    case 0xA: return !z && (s == v); // GT
    case 0xB: return !(z || c);      // UGT
    case 0xC: return !v;             // NOV
    case 0xD: return !s;             // PL
    case 0xE: return !z;             // NZ (NE)
    default: return !c;              // NC (UGE)
  }
}

// ------------------------------------------------------------------ ULA
uint8_t Sm8500::sub8(uint8_t a, uint8_t b, int carry, bool cmp) {
  const uint32_t res = uint32_t(a) - b - uint32_t(carry);
  ps1 &= cmp ? uint8_t(FB | FI | FH | FD) : uint8_t(FB | FI);
  if (res > 0xFF) ps1 |= FC;
  if (!(res & 0xFF)) ps1 |= FZ;
  if (res & 0x80) ps1 |= FS;
  if ((a ^ b) & (a ^ res) & 0x80) ps1 |= FV;
  if (!cmp) {
    ps1 |= FD;
    if ((a ^ b ^ res) & 0x10) ps1 |= FH;
  }
  return uint8_t(res);
}

uint16_t Sm8500::sub16(uint16_t a, uint16_t b, int carry, bool cmp) {
  const uint32_t res = uint32_t(a) - b - uint32_t(carry);
  ps1 &= cmp ? uint8_t(FB | FI | FH | FD) : uint8_t(FB | FI);
  if (res > 0xFFFF) ps1 |= FC;
  if (!(res & 0xFFFF)) ps1 |= FZ;
  if (res & 0x8000) ps1 |= FS;
  if ((a ^ b) & (a ^ res) & 0x8000) ps1 |= FV;
  if (!cmp) {
    ps1 |= FD;
    if ((a ^ b ^ res) & 0x10) ps1 |= FH;  // o H dos 16 bits vem do bit 4, como no MAME
  }
  return uint16_t(res);
}

uint8_t Sm8500::add8(uint8_t a, uint8_t b, int carry) {
  const uint32_t res = uint32_t(a) + b + uint32_t(carry);
  ps1 &= uint8_t(FB | FI);
  if (res > 0xFF) ps1 |= FC;
  if (!(res & 0xFF)) ps1 |= FZ;
  if (res & 0x80) ps1 |= FS;
  if (~(a ^ b) & (a ^ res) & 0x80) ps1 |= FV;  // estouro da soma (o MAME usa a fórmula da subtração no ADC)
  if ((a ^ b ^ res) & 0x10) ps1 |= FH;
  return uint8_t(res);
}

uint16_t Sm8500::add16(uint16_t a, uint16_t b, int carry) {
  const uint32_t res = uint32_t(a) + b + uint32_t(carry);
  ps1 &= uint8_t(FB | FI);
  if (res > 0xFFFF) ps1 |= FC;
  if (!(res & 0xFFFF)) ps1 |= FZ;
  if (res & 0x8000) ps1 |= FS;
  if (~(a ^ b) & (a ^ res) & 0x8000) ps1 |= FV;
  if ((a ^ b ^ res) & 0x10) ps1 |= FH;
  return uint16_t(res);
}

uint8_t Sm8500::logic8(uint8_t r) {
  ps1 &= uint8_t(FB | FC | FI | FH | FD);
  if (!r) ps1 |= FZ;
  if (r & 0x80) ps1 |= FS;
  return r;
}

uint16_t Sm8500::logic16(uint16_t r) {
  ps1 &= uint8_t(FC | FS | FB | FI | FH | FD);
  if (!r) ps1 |= FZ;
  return r;
}

uint8_t Sm8500::unary(int which, uint8_t d) {
  uint32_t res = 0;
  const uint8_t keepRot = FD | FH | FB | FI;
  auto zs = [&](uint32_t r) {
    if (!(r & 0xFF)) ps1 |= FZ;
    if (r & 0x80) ps1 |= FS;
  };
  switch (which) {
    case 0: res = 0; break;  // CLR: sem flags
    case 1:                  // NEG
      res = uint32_t(-int(d));
      ps1 &= keepRot;
      if (!(res & 0xFF)) ps1 |= FC | FZ;
      if (res & 0x80) ps1 |= FS;
      if ((res & 0xFF) == 0x80) ps1 |= FV;
      break;
    case 2:  // COM
      res = uint8_t(~d);
      ps1 &= uint8_t(FC | keepRot);
      zs(res);
      break;
    case 3:  // RR
    case 5:  // RRC
      res = d >> 1;
      if (which == 3 ? (d & 1) : (ps1 & FC)) res |= 0x80;
      ps1 &= keepRot;
      if (d & 1) ps1 |= FC;
      zs(res);
      if (((d ^ res) & 0x80) && !(res & 0x80)) ps1 |= FV;
      break;
    case 4:  // RL
    case 6:  // RLC
      res = uint32_t(d) << 1;
      if (which == 4 ? (d & 0x80) : (ps1 & FC)) res |= 1;
      ps1 &= keepRot;
      if (d & 0x80) ps1 |= FC;
      zs(res);
      if ((d ^ res) & 0x80) ps1 |= FV;
      break;
    case 7:  // SRL
      res = d >> 1;
      ps1 &= keepRot;
      if (d & 1) ps1 |= FC;
      if (!(res & 0xFF)) ps1 |= FZ;
      break;
    case 8:  // INC (V quando passa de $7F para $80)
      res = uint32_t(d) + 1;
      ps1 &= uint8_t(FC | keepRot);
      zs(res);
      if ((res & 0xFF) == 0x80) ps1 |= FV;
      break;
    case 9:  // DEC (V quando passa de $80 para $7F)
      res = uint32_t(d) - 1;
      ps1 &= uint8_t(FC | keepRot);
      zs(res);
      if ((res & 0xFF) == 0x7F) ps1 |= FV;
      break;
    case 10:  // SRA
      res = (d >> 1) | (d & 0x80);
      ps1 &= keepRot;
      if (d & 1) ps1 |= FC;
      zs(res);
      break;
    case 11:  // SLL
      res = uint32_t(d) << 1;
      ps1 &= keepRot;
      if (d & 0x80) ps1 |= FC;
      zs(res);
      break;
    case 12: {  // DA (ajuste decimal; D indica que a última operação foi subtração)
      res = d;
      const bool c = ps1 & FC, h = ps1 & FH;
      if (ps1 & FD) {
        if (c) res += h ? 0x9A : 0xA0;
        else if (h) res += 0xFA;
      } else if (c) {
        res += (h || (res & 0x0F) >= 10) ? 0x66 : 0x60;
      } else if (h) {
        if ((res & 0xF0) < 0xA0) res += 0x06;
        else { res += 0x66; ps1 |= FC; }
      } else if ((res & 0x0F) < 10) {
        if ((res & 0xF0) >= 0xA0) { res += 0x60; ps1 |= FC; }
      } else if ((res & 0xF0) < 0x90) {
        res += 0x06;
      } else {
        res += 0x66;
        ps1 |= FC;
      }
      ps1 &= uint8_t(~(FZ | FS));
      zs(res);
      break;
    }
    case 13: res = uint32_t(d << 4) | (d >> 4); break;  // SWAP: sem flags
    default: break;
  }
  return uint8_t(res);
}

// op: 0 CMP 1 ADD 2 SUB 3 ADC 4 SBC 5 AND 6 OR 7 XOR 8 MOV
uint8_t Sm8500::alu8(int op, uint8_t a, uint8_t b, bool& store) {
  store = op != 0;
  const int c = (ps1 & FC) ? 1 : 0;
  switch (op) {
    case 0: sub8(a, b, 0, true); return a;
    case 1: return add8(a, b, 0);
    case 2: return sub8(a, b, 0, false);
    case 3: return add8(a, b, c);
    case 4: return sub8(a, b, c, false);
    case 5: return logic8(a & b);
    case 6: return logic8(a | b);
    case 7: return logic8(a ^ b);
    default: return b;
  }
}

// ------------------------------------------------------------------ execução
int Sm8500::step() {
  checkIrqs();  // o desvio para a interrupção não tem custo conhecido (o MAME também não conta)
  if (halted) {
    bus_->tick(4, true);
    return 4;
  }
  int cyc = 0;
  const bool sp16 = sys & 0x40;
  const uint8_t op = fetch();
  uint8_t r1 = 0, r2 = 0;
  uint16_t s1 = 0, s2 = 0;
  bool st = false;

  // modos "r,@r / (r)+ / @w / w(r) / -(r)" com ponteiro de 8 bits
  auto modeB = [&]() {
    r1 = fetch();
    switch (r1 & 0xC0) {
      case 0x00: s2 = rd(r1 & 7); break;
      case 0x40: s2 = rd(r1 & 7); wr(r1 & 7, uint8_t(s2 + 1)); break;
      case 0x80:
        s2 = fetch();
        if (r1 & 7) s2 = uint16_t(s2 + rd(r1 & 7));
        break;
      default:  // pré-decremento (o MAME usa o valor antigo no modo de 8 bits; aqui igual ao de 16 bits)
        s2 = uint8_t(rd(r1 & 7) - 1);
        wr(r1 & 7, uint8_t(s2));
        break;
    }
    r2 = r1;
    r1 = (r1 >> 3) & 7;
  };
  // os mesmos modos com ponteiro de 16 bits (RRn); inc = passo do pós/pré
  auto modeW = [&](int inc) {
    r1 = fetch();
    const uint8_t rr = B2W[r1 & 7];
    switch (r1 & 0xC0) {
      case 0x00: s2 = rdw(rr); break;
      case 0x40: s2 = rdw(rr); wrw(rr, uint16_t(s2 + inc)); break;
      case 0x80:
        s2 = fetchw();
        if (r1 & 7) s2 = uint16_t(s2 + rdw(rr));
        break;
      default:
        s2 = uint16_t(rdw(rr) - inc);
        wrw(rr, s2);
        break;
    }
    r2 = r1;
    r1 = (r1 >> 3) & 7;
  };
  auto cycB = [&](int a, int b, int cAbs, int cIdx, int d) {
    switch (r2 & 0xC0) {
      case 0x00: cyc += a; break;
      case 0x40: cyc += b; break;
      case 0x80: cyc += (r2 & 7) ? cIdx : cAbs; break;
      default: cyc += d; break;
    }
  };
  // "Rr,Rs" curto (registradores 0..7 num byte); fora do formato, r1 = o byte e r2 = 0 (como no MAME)
  auto shortRR = [&](bool word) {
    r1 = fetch();
    r2 = 0;
    if ((r1 & 0xC0) == 0) {
      r2 = word ? B2W[r1 & 7] : uint8_t(r1 & 7);
      r1 = word ? B2W[(r1 >> 3) & 7] : uint8_t((r1 >> 3) & 7);
    }
  };
  auto rel = [&](uint8_t d) { return uint16_t(pc + int8_t(d)); };
  auto bitAddr = [&]() {  // "$FFdd" ou "dd(Rr)" das instruções de bit 1C/1D/2A/2B
    r1 = fetch();
    s2 = uint16_t(1u << (r1 & 7));
    const uint8_t d = fetch();
    s1 = (r1 & 0x38) ? uint16_t(d + rd((r1 >> 3) & 7)) : uint16_t(0xFF00 + d);
  };

  switch (op) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: {
      // CLR NEG COM RR RL RRC RLC SRL INC DEC SRA SLL DA SWAP  R
      r1 = fetch();
      wr(r1, unary(op, rd(r1)));
      cyc += op == 0x01 ? 5 : op == 0x0D ? 7 : 4;
      break;
    }
    case 0x0E:  // PUSH R
      r1 = fetch();
      push(rd(r1));
      cyc += sp16 ? 12 : 10;
      break;
    case 0x0F:  // POP R
      r1 = fetch();
      r2 = pop();
      wr(r1, r2);
      cyc += sp16 ? 9 : 8;
      break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: {
      shortRR(false);
      const uint8_t v = alu8(op & 7, rd(r1), rd(r2), st);
      if (st) wr(r1, v);
      cyc += 5;
      break;
    }
    case 0x18: {  // INCW S
      r1 = fetch();
      const uint16_t d = rdw(r1), res = uint16_t(d + 1);
      ps1 &= uint8_t(FC | FD | FH | FB | FI);
      if (!res) ps1 |= FZ;
      if (res & 0x8000) ps1 |= FS;
      if (res == 0x8000) ps1 |= FV;
      wrw(r1, res);
      cyc += 8;
      break;
    }
    case 0x19: {  // DECW S
      r1 = fetch();
      const uint16_t d = rdw(r1), res = uint16_t(d - 1);
      ps1 &= uint8_t(FC | FD | FH | FB | FI);
      if (!res) ps1 |= FZ;
      if (res & 0x8000) ps1 |= FS;
      if (res == 0x7FFF) ps1 |= FV;
      wrw(r1, res);
      cyc += 8;
      break;
    }
    case 0x1A: {  // CLR/NEG/COM/RR/RL/RRC/RLC/SRL @Rr
      shortRR(false);
      s1 = rd(r1);
      static constexpr int C1A[8] = {7, 8, 7, 7, 7, 7, 7, 6};
      wr(s1, unary(r2, rd(s1)));
      cyc += C1A[r2 & 7];
      break;
    }
    case 0x1B: {  // INC/DEC/SRA/SLL/DA/SWAP/PUSH/POP @Rr
      shortRR(false);
      s1 = rd(r1);
      switch (r2 & 7) {
        case 0: wr(s1, unary(8, rd(s1))); cyc += 7; break;
        case 1: wr(s1, unary(9, rd(s1))); cyc += 7; break;
        case 2: wr(s1, unary(10, rd(s1))); cyc += 6; break;
        case 3: wr(s1, unary(11, rd(s1))); cyc += 6; break;
        case 4: wr(s1, unary(12, rd(s1))); cyc += 7; break;
        case 5: wr(s1, unary(13, rd(s1))); cyc += 9; break;
        case 6: push(rd(s1)); cyc += sp16 ? 13 : 8; break;
        default: { const uint8_t v = pop(); wr(s1, v); cyc += sp16 ? 12 : 11; break; }
      }
      break;
    }
    case 0x1C:  // BCLR $FFdd/dd(Rr),#b
      bitAddr();
      wr(s1, uint8_t(rd(s1) & ~s2));
      cyc += (r1 & 0x38) ? 8 : 12;
      break;
    case 0x1D:  // BSET
      bitAddr();
      wr(s1, uint8_t(rd(s1) | s2));
      cyc += (r1 & 0x38) ? 8 : 12;
      break;
    case 0x1E:  // PUSHW S
      r1 = fetch();
      push(rd(uint8_t(r1 + 1)));
      push(rd(r1));
      cyc += sp16 ? 12 : 9;
      break;
    case 0x1F:  // POPW S
      r1 = fetch();
      r2 = pop();
      wr(r1, r2);
      r2 = pop();
      wr(uint8_t(r1 + 1), r2);
      cyc += sp16 ? 12 : 13;
      break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: {
      modeB();
      const uint8_t v = alu8(op & 7, rd(r1), rd(s2), st);
      if (st) wr(r1, v);
      cycB(7, 8, 10, 8, 9);
      break;
    }
    case 0x28:  // MOV r,@r ...
      modeB();
      wr(r1, rd(s2));
      cycB(6, 7, 10, 7, 8);
      break;
    case 0x29:  // MOV @r,r ...
      modeB();
      wr(s2, rd(r1));
      cycB(8, 8, 10, 9, 9);
      break;
    case 0x2A:  // BBC $FFdd/dd(Rr),#b,rel
    case 0x2B: {  // BBS
      bitAddr();
      const uint8_t d = fetch();
      const bool set = rd(s1) & s2;
      if (set == (op == 0x2B)) { pc = rel(d); cyc += 14; } else { cyc += 10; }
      if (!(r1 & 0x38)) cyc += 2;
      break;
    }
    case 0x2C: {  // EXTS RR
      r1 = fetch();
      uint16_t v = rdw(r1);
      v = (v & 0x80) ? uint16_t(v | 0xFF00) : uint16_t(v & 0xFF);
      wrw(r1, v);
      cyc += 6;
      break;
    }
    case 0x2D:  // desconhecida (2 bytes no desmontador do MAME, que não a executa)
      cyc += 4;
      break;
    case 0x2E:  // MOV PS0,#i
      ps0 = fetch();
      cyc += 4;
      break;
    case 0x2F:  // BTST R,#i
      r2 = fetch();
      r1 = fetch();
      ps1 &= uint8_t(~FV);
      if (!(rd(r2) & r1)) ps1 |= FZ; else ps1 &= uint8_t(~FZ);
      cyc += 6;
      break;
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
      modeW(1);
      const uint8_t v = alu8(op & 7, rd(r1), rd(s2), st);
      if (st) wr(r1, v);
      cycB(8, 13, 15, 11, 13);
      break;
    }
    case 0x38:
      modeW(1);
      wr(r1, rd(s2));
      cycB(8, 13, 15, 11, 13);
      break;
    case 0x39:
      modeW(1);
      wr(s2, rd(r1));
      cycB(8, 13, 15, 11, 13);
      break;
    case 0x3A:  // MOVW rr,@rr ...
      modeW(2);
      r1 = B2W[r1];
      wrw(r1, rdw(s2));
      cycB(11, 16, 14, 18, 16);
      break;
    case 0x3B:  // MOVW @rr,rr ...
      modeW(2);
      r1 = B2W[r1];
      wrw(s2, rdw(r1));
      cycB(11, 16, 14, 18, 16);
      break;
    case 0x3C:  // MOVW RRr,RRs (curto)
      shortRR(true);
      wrw(r1, rdw(r2));
      cyc += 7;
      break;
    case 0x3D:  // desconhecida
      cyc += 4;
      break;
    case 0x3E:    // JMP RRr / @ww / ww(Rr)
    case 0x3F: {  // CALL
      r1 = fetch();
      s2 = 0;
      if ((r1 & 0xC0) == 0x00) {
        s2 = rdw(B2W[r1 & 7]);
      } else if ((r1 & 0xC0) == 0x40) {
        s2 = fetchw();
        if (r1 & 0x38) s2 = uint16_t(s2 + rd((r1 >> 3) & 7));
        s2 = rdw(s2);
      }
      if (op == 0x3F) {
        push(uint8_t(pc));
        push(uint8_t(pc >> 8));
      }
      pc = s2;
      if (op == 0x3E) {
        cyc += (r1 & 0xC0) == 0 ? 7 : (r1 & 0xC0) == 0x40 ? ((r1 & 0x38) ? 19 : 15) : 4;
      } else {
        cyc += (r1 & 0xC0) == 0 ? (sp16 ? 14 : 11)
               : (r1 & 0xC0) == 0x40 ? ((r1 & 0x38) ? (sp16 ? 26 : 23) : (sp16 ? 22 : 19)) : 4;
      }
      break;
    }
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: {
      r2 = fetch();  // origem
      r1 = fetch();  // destino
      const uint8_t v = alu8(op == 0x48 ? 8 : (op & 7), rd(r1), rd(r2), st);
      if (st) wr(r1, v);
      cyc += 6;
      break;
    }
    case 0x49:  // CALL ad16
      s2 = fetchw();
      push(uint8_t(pc));
      push(uint8_t(pc >> 8));
      pc = s2;
      cyc += sp16 ? 12 : 10;
      break;
    case 0x4A:  // MOVW RRr,RRs
      r2 = fetch();
      r1 = fetch();
      wrw(r1, rdw(r2));
      cyc += 8;
      break;
    case 0x4B:  // MOVW RRr,#w
      r1 = fetch();
      s2 = fetchw();
      wrw(r1, s2);
      cyc += 9;
      break;
    case 0x4C:    // MULT RRr,Rs
    case 0x4D: {  // MULT RRr,#i
      r2 = fetch();
      r1 = fetch();
      const uint8_t m = op == 0x4C ? rd(r2) : r2;
      const uint16_t res = uint16_t(rd(r1 | 1) * m);  // o byte baixo de RRr vezes o operando
      wrw(r1, res);
      ps1 &= uint8_t(~(FZ | FV));
      if (!res) ps1 |= FZ;
      cyc += 24;
      break;
    }
    case 0x4E: {  // BMOV Rr,#b,BF / BF,Rr,#b
      r2 = fetch();
      r1 = fetch();
      const uint8_t bit = uint8_t(1u << (r2 & 7));
      if ((r2 & 0xC0) == 0x40) {
        uint8_t v = rd(r1);
        v = (ps1 & FB) ? uint8_t(v | bit) : uint8_t(v & ~bit);
        wr(r1, v);
      } else if ((r2 & 0xC0) == 0x00) {
        ps1 &= uint8_t(FC | FS | FD | FH | FI);
        ps1 |= (rd(r1) & bit) ? FB : FZ;
      }
      cyc += 6;
      break;
    }
    case 0x4F: {  // BCMP/BAND/BOR/BXOR BF,Rr,#b
      r2 = fetch();
      r1 = fetch();
      const uint8_t a = rd(r1) & uint8_t(1u << (r2 & 7));
      const uint8_t b = uint8_t(((ps1 & FB) >> 1) << (r2 & 7));
      switch (r2 & 0xC0) {
        case 0x00:
          ps1 &= uint8_t(~(FZ | FV));
          if (a == b) ps1 |= FZ;
          break;
        case 0x40: ps1 &= uint8_t(~(FZ | FV | FB)); ps1 |= (a & b) ? FB : FZ; break;
        case 0x80: ps1 &= uint8_t(~(FZ | FV | FB)); ps1 |= (a | b) ? FB : FZ; break;
        default: ps1 &= uint8_t(~(FZ | FV | FB)); ps1 |= (a ^ b) ? FB : FZ; break;
      }
      cyc += 6;
      break;
    }
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: {
      r2 = fetch();  // imediato
      r1 = fetch();  // registrador
      const uint8_t v = alu8(op == 0x58 ? 8 : (op & 7), rd(r1), r2, st);
      if (st) wr(r1, v);
      cyc += 6;
      break;
    }
    case 0x59:  // inválida
      cyc += 2;
      break;
    case 0x5A:    // CMP @r,#i / (r)+,#i / w(r),#i / -(r),#i
    case 0x5B: {  // MOV com os mesmos modos
      // endereço = conteúdo do registrador (byte), como nos comentários do MAME sobre os usos conhecidos
      // (5A 02 09 na calculadora, 5A 86 C6 00 em Henry, 5B 42 00). O MAME lê mais um nível de indireção em
      // três modos do 5B, sem caso conhecido; aqui todos os modos usam um nível só, como no 5A.
      r2 = fetch();
      r1 = fetch();
      uint16_t addr = 0;
      switch (r2 & 0xC0) {
        case 0x00: addr = rd(r2 & 7); break;
        case 0x40: {
          const uint8_t p = rd(r2 & 7);
          addr = p;
          wr(r2 & 7, uint8_t(p + 1));
          break;
        }
        case 0x80: {
          const uint8_t base = r1;
          r1 = fetch();
          addr = uint16_t(base + rd(r2 & 7));
          cyc += 4;
          break;
        }
        default: {
          const uint8_t p = uint8_t(rd(r2 & 7) - 1);
          wr(r2 & 7, p);
          addr = p;
          break;
        }
      }
      if (op == 0x5A) { sub8(rd(addr), r1, 0, true); cyc += 7; }
      else { wr(addr, r1); cyc += 6; }
      break;
    }
    case 0x5C: {  // DIV RRr,RRs (divisor = byte baixo de RRs; resto no byte alto de RRs)
      r2 = fetch();
      r1 = fetch();
      ps1 &= uint8_t(~(FZ | FV));
      const uint8_t dv = rd(uint8_t(r2 + 1));
      if (dv) {
        const uint16_t n = rdw(r1);
        const uint16_t q = uint16_t(n / dv);
        wr(r2, uint8_t(n % dv));
        wrw(r1, q);
        if (!q) ps1 |= FZ;
      } else {
        ps1 |= FV;
      }
      cyc += 47;
      break;
    }
    case 0x5D: {  // DIV RRr,#i
      r2 = fetch();
      r1 = fetch();
      ps1 &= uint8_t(~(FZ | FV));
      if (r2) {
        const uint16_t q = uint16_t(rdw(r1) / r2);
        wrw(r1, q);
        if (!q) ps1 |= FZ;
      } else {
        ps1 |= FV;
      }
      cyc += 44;
      break;
    }
    case 0x5E: {  // MOVM Rr,#mask,Rs
      r1 = fetch();
      const uint8_t m = fetch();
      r2 = fetch();
      wr(r1, uint8_t((rd(r1) & m) | rd(r2)));
      cyc += 9;
      break;
    }
    case 0x5F: {  // MOVM Rr,#mask,#i
      r1 = fetch();
      const uint8_t m = fetch();
      r2 = fetch();
      wr(r1, uint8_t((rd(r1) & m) | r2));
      cyc += 8;
      break;
    }
    case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
      uint16_t a, b;
      if (op < 0x68) {
        r2 = fetch();
        r1 = fetch();
        a = rdw(r1);
        b = rdw(r2);
      } else {
        r1 = fetch();
        b = fetchw();
        a = rdw(r1);
      }
      const int c = (ps1 & FC) ? 1 : 0;
      uint16_t res = 0;
      bool store = true;
      switch (op & 7) {
        case 0: sub16(a, b, 0, true); store = false; break;
        case 1: res = add16(a, b, 0); break;
        case 2: res = sub16(a, b, 0, false); break;
        case 3: res = add16(a, b, c); break;
        case 4: res = sub16(a, b, c, false); break;
        case 5: res = logic16(a & b); break;
        case 6: res = logic16(a | b); break;
        default: res = logic16(a ^ b); break;
      }
      if (store) wrw(r1, res);
      static constexpr int CR[8] = {9, 10, 10, 10, 10, 14, 14, 14}, CI[8] = {9, 10, 10, 10, 10, 13, 13, 13};
      cyc += op < 0x68 ? CR[op & 7] : CI[op & 7];
      break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77: {  // DBNZ r,rel
      const uint8_t d = fetch();
      const uint8_t v = uint8_t(rd(op & 7) - 1);
      wr(op & 7, v);
      if (v) { pc = rel(d); cyc += 10; } else { cyc += 6; }
      break;
    }
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:  // MOVW rr,#w
      s2 = fetchw();
      wrw(B2W[op & 7], s2);
      cyc += 6;
      break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {  // BBC/BBS R,#b,rel
      r1 = fetch();
      const uint8_t d = fetch();
      const bool set = rd(r1) & (1u << (op & 7));
      if (set == bool(op & 8)) { pc = rel(d); cyc += 10; } else { cyc += 6; }
      break;
    }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:  // JMP cc,ad16
      s2 = fetchw();
      if (cond(op)) pc = s2;
      cyc += 6;
      break;
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:  // BCLR R,#b
      r1 = fetch();
      wr(r1, uint8_t(rd(r1) & ~(1u << (op & 7))));
      cyc += 4;
      break;
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:  // BSET R,#b
      r1 = fetch();
      wr(r1, uint8_t(rd(r1) | (1u << (op & 7))));
      cyc += 4;
      break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:  // MOV r,R
      r2 = fetch();
      wr(op & 7, rd(r2));
      cyc += 4;
      break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:  // MOV R,r
      r2 = fetch();
      wr(r2, rd(op & 7));
      cyc += 4;
      break;
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7:  // MOV r,#i
      wr(op & 7, fetch());
      cyc += 4;
      break;
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:  // MOV IE0..P3,#i
      wr(uint16_t(0x10 + (op & 7)), fetch());
      cyc += 4;
      break;
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF: {  // BR cc,rel
      const uint8_t d = fetch();
      if (cond(op)) { pc = rel(d); cyc += 8; } else { cyc += 4; }
      break;
    }
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:  // CALS $1xxx
      r1 = fetch();
      s2 = uint16_t(0x1000 + ((op & 0x0F) << 8) + r1);
      push(uint8_t(pc));
      push(uint8_t(pc >> 8));
      pc = s2;
      cyc += sp16 ? 12 : 9;
      break;
    case 0xF0:  // STOP: aplica a troca de relógio pedida em CKC (a divisão do relógio em si não é emulada)
      stops++;
      if (ckcPending) { ckcApplied = ckc; ckcPending = false; }
      cyc += 2;
      break;
    case 0xF1: halted = true; cyc += 2; break;    // HALT
    case 0xF8: {                                  // RET
      const uint8_t hi = pop(), lo = pop();
      pc = uint16_t(hi << 8 | lo);
      cyc += sp16 ? 10 : 8;
      break;
    }
    case 0xF9: {  // IRET
      ps1 = pop();
      const uint8_t hi = pop(), lo = pop();
      pc = uint16_t(hi << 8 | lo);
      cyc += sp16 ? 12 : 10;
      break;
    }
    case 0xFA: ps1 &= uint8_t(~FC); cyc += 2; break;  // CLRC
    case 0xFB: ps1 ^= FC; cyc += 2; break;            // COMC
    case 0xFC: ps1 |= FC; cyc += 2; break;            // SETC
    case 0xFD: ps1 |= FI; cyc += 2; break;            // EI
    case 0xFE: ps1 &= uint8_t(~FI); cyc += 2; break;  // DI
    default: cyc += 2; break;                         // NOP ($FF) e inválidas ($F2-$F7)
  }
  bus_->tick(cyc, false);
  return cyc;
}

void Sm8500::sync(StateIO& s, int version) {
  s.sync(pc); s.sync(ps0); s.sync(ps1); s.sync(sys); s.sync(sph); s.sync(spl);
  if (version >= 2) { s.sync(ckc); s.sync(ckcApplied); s.sync(ckcPending); s.sync(stops); }
  else if (s.reading()) { ckc = ckcApplied = 0; ckcPending = false; stops = 0; }  // estado antigo: CKC como no reset
  s.sync(ie0); s.sync(ie1); s.sync(ir0); s.sync(ir1);
  s.sync(halted); s.sync(pending);
  s.raw(regs, sizeof regs);
}

}  // namespace indigo::gamecom
