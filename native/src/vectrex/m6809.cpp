#include "m6809.h"

namespace indigo::vectrex {

namespace {
// Ciclos base da página 0 (manual de dados da Motorola, MC6809E). O modo indexado soma o custo do pós-byte;
// PSHS/PULS somam 1 por byte; desvios longos condicionais somam 1 quando tomados. -1 = prefixo (0x10/0x11),
// 0 = código que para a CPU (14, 15, CD).
const int8_t CYC0[256] = {
//  0  1  2  3  4  5  6  7  8  9  A  B  C  D  E  F
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6,   // 0x  direto (leitura-modificação-gravação)
   -1,-1, 2, 4, 0, 0, 5, 9, 3, 2, 3, 2, 3, 2, 8, 6,   // 1x
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,   // 2x  desvios curtos
    4, 4, 4, 4, 5, 5, 5, 5, 4, 5, 3, 6,20,11,19,19,   // 3x
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,   // 4x  A
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,   // 5x  B
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 6,   // 6x  indexado
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 4, 7,   // 7x  estendido
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 4, 7, 3, 3,   // 8x  A imediato
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,   // 9x  A direto
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 6, 7, 5, 5,   // Ax  A indexado
    5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 7, 8, 6, 6,   // Bx  A estendido
    2, 2, 2, 4, 2, 2, 2, 2, 2, 2, 2, 2, 3, 0, 3, 3,   // Cx  B imediato
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,   // Dx  B direto
    4, 4, 4, 6, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,   // Ex  B indexado
    5, 5, 5, 7, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,   // Fx  B estendido
};

// páginas 2 e 3: ciclos já contando o prefixo; 0 = código inválido (roda o da página 0 com 1 ciclo a mais)
int cycPage2(int op) {
  if (op == 0x20) return 6;                       // LBRA não documentado
  if (op > 0x20 && op < 0x30) return 5;           // LBcc (6 se tomado)
  if (op == 0x3E || op == 0x3F) return 20;        // SWI2
  const int col = op & 0x0F, mode = (op >> 4) & 3;
  if (op < 0x80) return 0;
  static const int8_t cmp16[4] = {5, 7, 7, 8}, ld16[4] = {4, 6, 6, 7}, st16[4] = {4, 6, 6, 7};
  if (op < 0xC0) {
    if (col == 0x3 || col == 0xC) return cmp16[mode];  // CMPD, CMPY
    if (col == 0xE) return ld16[mode];                 // LDY
    if (col == 0xF) return st16[mode];                 // STY (imediato: XSTY não documentado)
    return 0;
  }
  if (col == 0x3) return cmp16[mode];  // XADDD não documentado
  if (col == 0xE) return ld16[mode];   // LDS
  if (col == 0xF) return st16[mode];   // STS (imediato: XSTS não documentado)
  return 0;
}

int cycPage3(int op) {
  if (op == 0x3E || op == 0x3F) return 20;  // 113E = FIRQ por software (não documentado), 113F = SWI3
  if (op < 0x80) return 0;
  const int col = op & 0x0F, mode = (op >> 4) & 3;
  static const int8_t cmp16[4] = {5, 7, 7, 8};
  if (op < 0xC0) return (col == 0x3 || col == 0xC) ? cmp16[mode] : 0;  // CMPU, CMPS
  return col == 0x3 ? cmp16[mode] : 0;                                 // XADDU não documentado
}
}  // namespace

int m6809BaseCycles(int page, int op) {
  op &= 0xFF;
  if (page == 0) return CYC0[op] < 0 ? 0 : CYC0[op];
  const int c = page == 2 ? cycPage2(op) : cycPage3(op);
  if (c) return c;
  return CYC0[op] > 0 ? CYC0[op] + 1 : 0;
}

void M6809::reset() {
  dp = 0;
  cc = static_cast<uint8_t>(cc | CC_I | CC_F);
  nmiArmed = false;
  nmiPending = false;
  state = RUN;
  cyc = 0;
  lag = 0;
  pc = rd16(0xFFFE);
}

// ---------------------------------------------------------------- endereçamento

uint16_t M6809::eaIndexed() {
  const uint8_t pb = fetch();
  uint16_t* r;
  switch ((pb >> 5) & 3) {
    case 0: r = &x; break;
    case 1: r = &y; break;
    case 2: r = &u; break;
    default: r = &s; break;
  }
  uint16_t ea;
  if (!(pb & 0x80)) {  // deslocamento de 5 bits com sinal
    int off = pb & 0x1F;
    if (off & 0x10) off -= 0x20;
    cyc += 1;
    return static_cast<uint16_t>(*r + off);
  }
  switch (pb & 0x0F) {
    case 0x0: ea = *r; *r = static_cast<uint16_t>(*r + 1); cyc += 2; break;                          // ,R+
    case 0x1: ea = *r; *r = static_cast<uint16_t>(*r + 2); cyc += 3; break;                          // ,R++
    case 0x2: *r = static_cast<uint16_t>(*r - 1); ea = *r; cyc += 2; break;                          // ,-R
    case 0x3: *r = static_cast<uint16_t>(*r - 2); ea = *r; cyc += 3; break;                          // ,--R
    case 0x4: ea = *r; break;                                                                         // ,R
    case 0x5: ea = static_cast<uint16_t>(*r + static_cast<int8_t>(b)); cyc += 1; break;               // B,R
    case 0x6: case 0x7: ea = static_cast<uint16_t>(*r + static_cast<int8_t>(a)); cyc += 1; break;    // A,R (7: não doc.)
    case 0x8: ea = static_cast<uint16_t>(*r + static_cast<int8_t>(fetch())); cyc += 1; break;         // n8,R
    case 0x9: { const uint16_t o = fetch16(); ea = static_cast<uint16_t>(*r + o); cyc += 4; break; }  // n16,R
    case 0xA: ea = static_cast<uint16_t>((pc + 1) | 0xFF); cyc += 4; break;                           // não documentado
    case 0xB: ea = static_cast<uint16_t>(*r + D()); cyc += 4; break;                                  // D,R
    case 0xC: { const int8_t o = static_cast<int8_t>(fetch()); ea = static_cast<uint16_t>(pc + o); cyc += 1; break; }  // n8,PCR
    case 0xD: { const uint16_t o = fetch16(); ea = static_cast<uint16_t>(pc + o); cyc += 5; break; }  // n16,PCR
    case 0xE: ea = 0xFFFF; cyc += 5; break;                                                           // não documentado
    default: ea = fetch16(); cyc += 2; break;                                                         // [n16] (sem o bit 4: não doc.)
  }
  if (pb & 0x10) {  // indireto
    cyc += 3;
    ea = rd16(ea);
  }
  return ea;
}

uint16_t M6809::ea(int mode) {
  if (mode == 1) return eaDirect();
  if (mode == 2) return eaIndexed();
  return eaExtended();
}

// ---------------------------------------------------------------- ULA

uint8_t M6809::add8(uint8_t r, uint8_t m, int carry) {
  const unsigned t = r + m + carry;
  const uint8_t res = static_cast<uint8_t>(t);
  cc = static_cast<uint8_t>(cc & ~(CC_H | CC_N | CC_Z | CC_V | CC_C));
  if ((r ^ m ^ res) & 0x10) cc |= CC_H;
  if (t & 0x100) cc |= CC_C;
  if ((~(r ^ m) & (r ^ res)) & 0x80) cc |= CC_V;
  nz8(res);
  return res;
}

uint8_t M6809::sub8(uint8_t r, uint8_t m, int borrow) {
  const unsigned t = static_cast<unsigned>(r) - m - borrow;
  const uint8_t res = static_cast<uint8_t>(t);
  cc = static_cast<uint8_t>(cc & ~(CC_N | CC_Z | CC_V | CC_C));
  if (t & 0x100) cc |= CC_C;
  if (((r ^ m) & (r ^ res)) & 0x80) cc |= CC_V;
  nz8(res);
  return res;
}

uint16_t M6809::add16(uint16_t r, uint16_t m) {
  const uint32_t t = static_cast<uint32_t>(r) + m;
  const uint16_t res = static_cast<uint16_t>(t);
  cc = static_cast<uint8_t>(cc & ~(CC_N | CC_Z | CC_V | CC_C));
  if (t & 0x10000) cc |= CC_C;
  if ((~(r ^ m) & (r ^ res)) & 0x8000) cc |= CC_V;
  nz16(res);
  return res;
}

uint16_t M6809::sub16(uint16_t r, uint16_t m) {
  const uint32_t t = static_cast<uint32_t>(r) - m;
  const uint16_t res = static_cast<uint16_t>(t);
  cc = static_cast<uint8_t>(cc & ~(CC_N | CC_Z | CC_V | CC_C));
  if (t & 0x10000) cc |= CC_C;
  if (((r ^ m) & (r ^ res)) & 0x8000) cc |= CC_V;
  nz16(res);
  return res;
}

// coluna das instruções de um operando (0x00-0x0F, 0x40-0x7F): NEG COM LSR ROR ASR ASL ROL DEC INC TST CLR
uint8_t M6809::rmw(int op, uint8_t v) {
  uint8_t r;
  switch (op & 0x0F) {
    case 0x2:  // XNC (não documentado): COM se C=1, NEG se C=0
      if (cc & CC_C) goto com;
      [[fallthrough]];
    case 0x0: case 0x1:  // NEG
      r = static_cast<uint8_t>(-v);
      cc = static_cast<uint8_t>(cc & ~(CC_V | CC_C));
      if (v == 0x80) cc |= CC_V;
      if (v) cc |= CC_C;
      nz8(r);
      return r;
    case 0x3:
    com:
      r = static_cast<uint8_t>(~v);
      cc = static_cast<uint8_t>((cc & ~CC_V) | CC_C);
      nz8(r);
      return r;
    case 0x4: case 0x5:  // LSR
      r = static_cast<uint8_t>(v >> 1);
      setF(CC_C, v & 1);
      nz8(r);
      return r;
    case 0x6:  // ROR
      r = static_cast<uint8_t>((v >> 1) | ((cc & CC_C) ? 0x80 : 0));
      setF(CC_C, v & 1);
      nz8(r);
      return r;
    case 0x7:  // ASR
      r = static_cast<uint8_t>((v >> 1) | (v & 0x80));
      setF(CC_C, v & 1);
      nz8(r);
      return r;
    case 0x8:  // ASL/LSL
      r = static_cast<uint8_t>(v << 1);
      setF(CC_C, v & 0x80);
      setF(CC_V, ((v ^ (v << 1)) & 0x80) != 0);
      nz8(r);
      return r;
    case 0x9:  // ROL
      r = static_cast<uint8_t>((v << 1) | (cc & CC_C));
      setF(CC_C, v & 0x80);
      setF(CC_V, ((v ^ (v << 1)) & 0x80) != 0);
      nz8(r);
      return r;
    case 0xB:  // XDEC (não documentado): DEC que também põe C = (valor != 0)
      setF(CC_C, v != 0);
      [[fallthrough]];
    case 0xA:  // DEC
      r = static_cast<uint8_t>(v - 1);
      setF(CC_V, v == 0x80);
      nz8(r);
      return r;
    case 0xC:  // INC
      r = static_cast<uint8_t>(v + 1);
      setF(CC_V, v == 0x7F);
      nz8(r);
      return r;
    case 0xD:  // TST
      setF(CC_V, false);
      nz8(v);
      return v;
    case 0xE:  // XCLR (não documentado, só em A/B): como CLR, mas C não muda
      cc = static_cast<uint8_t>((cc & ~(CC_N | CC_V)) | CC_Z);
      return 0;
    default:  // CLR
      cc = static_cast<uint8_t>((cc & ~(CC_N | CC_V | CC_C)) | CC_Z);
      return 0;
  }
}

// ---------------------------------------------------------------- registradores (TFR/EXG)

// regras de David Flamand: de 8 para 16 bits, A/B ganham $FF no byte alto e CC/DP se repetem; registrador
// inexistente vale $FFFF e gravar nele não faz nada
uint16_t M6809::regGet(int code) {
  switch (code & 15) {
    case 0: return D();
    case 1: return x;
    case 2: return y;
    case 3: return u;
    case 4: return s;
    case 5: return pc;
    case 8: return static_cast<uint16_t>(0xFF00 | a);
    case 9: return static_cast<uint16_t>(0xFF00 | b);
    case 10: return static_cast<uint16_t>(cc << 8 | cc);
    case 11: return static_cast<uint16_t>(dp << 8 | dp);
    default: return 0xFFFF;
  }
}

void M6809::regSet(int code, uint16_t v) {
  switch (code & 15) {
    case 0: setD(v); break;
    case 1: x = v; break;
    case 2: y = v; break;
    case 3: u = v; break;
    case 4: s = v; nmiArmed = true; break;
    case 5: pc = v; break;
    case 8: a = static_cast<uint8_t>(v); break;
    case 9: b = static_cast<uint8_t>(v); break;
    case 10: cc = static_cast<uint8_t>(v); break;
    case 11: dp = static_cast<uint8_t>(v); break;
    default: break;
  }
}

// EXG é uma transferência tripla (origem baixa -> temp, alta -> baixa, temp -> alta)
void M6809::exg(uint8_t pb) {
  const int r1 = pb >> 4, r2 = pb & 15;
  uint16_t tmp = regGet(r2);
  regSet(r2, regGet(r1));
  if (r2 >= 8 && r1 < 8) tmp = static_cast<uint16_t>(0xFF00 | (tmp & 0xFF));
  regSet(r1, tmp);
}

void M6809::pushRegs(uint16_t& sp, uint16_t& other, uint8_t m) {
  if (m & 0x80) { push16(sp, pc); cyc += 2; }
  if (m & 0x40) { push16(sp, other); cyc += 2; }
  if (m & 0x20) { push16(sp, y); cyc += 2; }
  if (m & 0x10) { push16(sp, x); cyc += 2; }
  if (m & 0x08) { push8(sp, dp); cyc += 1; }
  if (m & 0x04) { push8(sp, b); cyc += 1; }
  if (m & 0x02) { push8(sp, a); cyc += 1; }
  if (m & 0x01) { push8(sp, cc); cyc += 1; }
}

void M6809::pullRegs(uint16_t& sp, uint16_t& other, uint8_t m) {
  if (m & 0x01) { cc = pull8(sp); cyc += 1; }
  if (m & 0x02) { a = pull8(sp); cyc += 1; }
  if (m & 0x04) { b = pull8(sp); cyc += 1; }
  if (m & 0x08) { dp = pull8(sp); cyc += 1; }
  if (m & 0x10) { x = pull16(sp); cyc += 2; }
  if (m & 0x20) { y = pull16(sp); cyc += 2; }
  if (m & 0x40) { other = pull16(sp); cyc += 2; }
  if (m & 0x80) { pc = pull16(sp); cyc += 2; }
}

bool M6809::cond(int c) const {
  const bool C = cc & CC_C, Z = cc & CC_Z, N = cc & CC_N, V = cc & CC_V;
  switch (c & 15) {
    case 0x0: return true;          // BRA
    case 0x1: return false;         // BRN
    case 0x2: return !(C || Z);     // BHI
    case 0x3: return C || Z;        // BLS
    case 0x4: return !C;            // BCC/BHS
    case 0x5: return C;             // BCS/BLO
    case 0x6: return !Z;            // BNE
    case 0x7: return Z;             // BEQ
    case 0x8: return !V;            // BVC
    case 0x9: return V;             // BVS
    case 0xA: return !N;            // BPL
    case 0xB: return N;             // BMI
    case 0xC: return N == V;        // BGE
    case 0xD: return N != V;        // BLT
    case 0xE: return !Z && N == V;  // BGT
    default: return Z || N != V;    // BLE
  }
}

// empilha o estado (inteiro: E=1, todos os registradores; senão só PC e CC) e salta pelo vetor
void M6809::interrupt(bool entire, uint16_t vector, uint8_t mask) {
  if (entire) {
    cc |= CC_E;
    push16(s, pc); push16(s, u); push16(s, y); push16(s, x);
    push8(s, dp); push8(s, b); push8(s, a); push8(s, cc);
  } else {
    cc = static_cast<uint8_t>(cc & ~CC_E);
    push16(s, pc); push8(s, cc);
  }
  cc |= mask;
  pc = rd16(vector);
}

// ---------------------------------------------------------------- execução

int M6809::step() {
  cyc = 0;
  lag = 0;
  if (state == STOPPED) return cyc = 1;  // 14/15/CD: a CPU fica presa até o reset

  const bool nmiReq = nmiPending;
  const bool firqReq = firqLine && !(cc & CC_F);
  const bool irqReq = irqLine && !(cc & CC_I);

  if (state == SYNCING) {
    // qualquer linha de interrupção (mesmo mascarada) solta o SYNC; mascarada, segue para a próxima instrução
    if (!nmiReq && !firqLine && !irqLine) return cyc = 1;
    state = RUN;
    if (!nmiReq && !firqReq && !irqReq) { cyc = 2; return cyc; }
  }
  if (state == CWAITING) {
    // o estado já foi empilhado pelo CWAI: só busca o vetor
    if (!nmiReq && !firqReq && !irqReq) return cyc = 1;
    state = RUN;
    cyc = 2;
    if (nmiReq) { nmiPending = false; cc |= CC_I | CC_F; pc = rd16(0xFFFC); }
    else if (firqReq) { cc |= CC_I | CC_F; pc = rd16(0xFFF6); }
    else { cc |= CC_I; pc = rd16(0xFFF8); }
    return cyc;
  }
  if (nmiReq) { nmiPending = false; cyc = 19; interrupt(true, 0xFFFC, CC_I | CC_F); return cyc; }
  if (firqReq) { cyc = 10; interrupt(false, 0xFFF6, CC_I | CC_F); return cyc; }
  if (irqReq) { cyc = 19; interrupt(true, 0xFFF8, CC_I); return cyc; }

  const uint8_t op = fetch();
  if (op == 0x10 || op == 0x11) {
    uint8_t op2 = fetch();
    while (op2 == 0x10 || op2 == 0x11) { cyc += 1; op2 = fetch(); }  // prefixos a mais: 1 ciclo cada
    const int page = op == 0x10 ? 2 : 3;
    const int c = page == 2 ? cycPage2(op2) : cycPage3(op2);
    if (c) {
      cyc += c;
      execPage23(page, op2);
    } else {
      cyc += 1 + (CYC0[op2] > 0 ? CYC0[op2] : 0);
      execPage0(op2);
    }
    return cyc;
  }
  cyc += CYC0[op];
  execPage0(op);
  return cyc;
}

void M6809::execPage0(uint8_t op) {
  const int hi = op >> 4;
  switch (hi) {
    case 0x0: case 0x6: case 0x7: {  // memória: leitura-modificação-gravação
      const uint16_t addr = ea(hi == 0 ? 1 : hi == 6 ? 2 : 3);
      const int f = op & 0x0F;
      if (f == 0xE) { pc = addr; return; }  // JMP
      const uint8_t v = rd(addr);           // CLR também lê antes de gravar (como o 6809 de verdade)
      const uint8_t r = rmw(f == 0xE ? 0xF : f, v);
      if (f != 0xD) wr(addr, r);            // TST não grava
      return;
    }
    case 0x4: a = rmw(op, a); return;
    case 0x5: b = rmw(op, b); return;
    case 0x2: {  // desvios curtos
      const int8_t o = static_cast<int8_t>(fetch());
      if (cond(op)) pc = static_cast<uint16_t>(pc + o);
      return;
    }
    default: break;
  }

  switch (op) {
    case 0x12: case 0x1B: return;  // NOP (1B não documentado)
    case 0x13: state = SYNCING; return;
    case 0x14: case 0x15: case 0xCD: state = STOPPED; return;  // "halt and catch fire"
    case 0x16: { const uint16_t o = fetch16(); pc = static_cast<uint16_t>(pc + o); return; }  // LBRA
    case 0x17: { const uint16_t o = fetch16(); push16(s, pc); pc = static_cast<uint16_t>(pc + o); return; }  // LBSR
    case 0x18: {  // não documentado: desloca CC para a esquerda filtrado pelo próximo byte (medido no hardware)
      const uint8_t nb = rd(pc);
      const uint8_t o = cc;
      uint8_t n = 0;
      if ((nb & 0x40) && (o & CC_F)) n |= CC_E;
      if ((nb & 0x20) && (o & CC_H)) n |= CC_F;
      if ((nb & 0x10) && (o & CC_I)) n |= CC_H;
      if ((nb & 0x08) && (o & CC_N)) n |= CC_I;
      if ((nb & 0x04) && (o & CC_Z)) n |= CC_N;
      if ((nb & 0x02) && (o & CC_V)) n |= CC_Z;
      if (((nb & 0x01) && (o & CC_C)) || ((nb & 0x04) && (o & CC_Z))) n |= CC_V;
      cc = n;
      return;
    }
    case 0x19: {  // DAA (V como medido no hardware: bit 7 do resultado xor C)
      int corr = 0;
      const int lo = a & 0x0F, hi4 = a & 0xF0;
      if ((cc & CC_H) || lo > 9) corr |= 0x06;
      if ((cc & CC_C) || hi4 > 0x90 || (hi4 > 0x80 && lo > 9)) corr |= 0x60;
      const int t = a + corr;
      if (t & 0x100) cc |= CC_C;
      a = static_cast<uint8_t>(t);
      setF(CC_V, (((t >> 7) & 1) ^ (cc & CC_C)) != 0);
      nz8(a);
      return;
    }
    case 0x1A: cc |= fetch(); return;  // ORCC
    case 0x1C: case 0x38: cc &= fetch(); return;  // ANDCC (38 não documentado)
    case 0x1D: a = (b & 0x80) ? 0xFF : 0x00; nz8(b); return;  // SEX (V não muda, como no hardware)
    case 0x1E: exg(fetch()); return;
    case 0x1F: { const uint8_t pb = fetch(); regSet(pb & 15, regGet(pb >> 4)); return; }  // TFR
    case 0x30: x = eaIndexed(); setF(CC_Z, x == 0); return;  // LEAX
    case 0x31: y = eaIndexed(); setF(CC_Z, y == 0); return;  // LEAY
    case 0x32: s = eaIndexed(); nmiArmed = true; return;     // LEAS
    case 0x33: u = eaIndexed(); return;                      // LEAU
    case 0x34: pushRegs(s, u, fetch()); return;              // PSHS
    case 0x35: pullRegs(s, u, fetch()); return;              // PULS
    case 0x36: pushRegs(u, s, fetch()); return;              // PSHU
    case 0x37: { const uint8_t m = fetch(); pullRegs(u, s, m); if (m & 0x40) nmiArmed = true; return; }  // PULU
    case 0x39: pc = pull16(s); return;                       // RTS
    case 0x3A: x = static_cast<uint16_t>(x + b); return;     // ABX
    case 0x3B:                                               // RTI
      cc = pull8(s);
      if (cc & CC_E) {
        cyc += 9;
        a = pull8(s); b = pull8(s); dp = pull8(s);
        x = pull16(s); y = pull16(s); u = pull16(s);
      }
      pc = pull16(s);
      return;
    case 0x3C:  // CWAI: empilha tudo agora e espera a interrupção
      cc &= fetch();
      cc |= CC_E;
      push16(s, pc); push16(s, u); push16(s, y); push16(s, x);
      push8(s, dp); push8(s, b); push8(s, a); push8(s, cc);
      state = CWAITING;
      return;
    case 0x3D: {  // MUL
      const uint16_t r = static_cast<uint16_t>(a * b);
      setD(r);
      setF(CC_Z, r == 0);
      setF(CC_C, r & 0x80);
      return;
    }
    case 0x3E: interrupt(true, 0xFFFE, CC_I | CC_F); return;  // não documentado: "reset" por software
    case 0x3F: interrupt(true, 0xFFFA, CC_I | CC_F); return;  // SWI
    case 0x8D: { const int8_t o = static_cast<int8_t>(fetch()); push16(s, pc); pc = static_cast<uint16_t>(pc + o); return; }  // BSR
    case 0x87: case 0xC7:  // não documentados: "gravação imediata" de 8 bits (só mexe nas flags)
      fetch();
      cc = static_cast<uint8_t>((cc & ~(CC_Z | CC_V)) | CC_N);
      return;
    case 0x8F: case 0xCF: {  // não documentados: grava o byte baixo de X/U no lugar do operando imediato
      const uint16_t addr = pc++;
      wr(addr, static_cast<uint8_t>(op == 0x8F ? x : u));
      cc = static_cast<uint8_t>((cc & ~(CC_Z | CC_V)) | CC_N);
      return;
    }
    default: break;
  }

  // 0x80-0xFF: operações com A (0x80-0xBF) ou B (0xC0-0xFF)
  const int mode = (op >> 4) & 3;  // 0 imediato, 1 direto, 2 indexado, 3 estendido
  const int col = op & 0x0F;
  const bool isB = op >= 0xC0;
  uint8_t& r = isB ? b : a;
  switch (col) {
    case 0x3: case 0xC: case 0xE: {  // 16 bits: SUBD/ADDD, CMPX/LDD, LDX/LDU
      uint16_t m;
      if (mode == 0) m = fetch16();
      else m = rd16(ea(mode));
      if (col == 0x3) setD(isB ? add16(D(), m) : sub16(D(), m));
      else if (col == 0xC) { if (isB) { setD(m); nz16(m); setF(CC_V, false); } else sub16(x, m); }
      else { if (isB) u = m; else x = m; nz16(m); setF(CC_V, false); }
      return;
    }
    case 0xD:
      if (isB) {  // STD
        const uint16_t addr = ea(mode);
        const uint16_t v = D();
        wr16(addr, v); nz16(v); setF(CC_V, false);
      } else {    // JSR
        const uint16_t addr = ea(mode);
        push16(s, pc);
        pc = addr;
      }
      return;
    case 0xF: {  // STX/STU
      const uint16_t addr = ea(mode);
      const uint16_t v = isB ? u : x;
      wr16(addr, v); nz16(v); setF(CC_V, false);
      return;
    }
    case 0x7: {  // STA/STB
      const uint16_t addr = ea(mode);
      wr(addr, r); nz8(r); setF(CC_V, false);
      return;
    }
    default: break;
  }
  const uint8_t m = mode == 0 ? fetch() : rd(ea(mode));
  switch (col) {
    case 0x0: r = sub8(r, m, 0); break;                     // SUB
    case 0x1: sub8(r, m, 0); break;                         // CMP
    case 0x2: r = sub8(r, m, (cc & CC_C) ? 1 : 0); break;   // SBC
    case 0x4: r = logic8(r & m); break;                     // AND
    case 0x5: logic8(r & m); break;                         // BIT
    case 0x6: r = logic8(m); break;                         // LD
    case 0x8: r = logic8(r ^ m); break;                     // EOR
    case 0x9: r = add8(r, m, (cc & CC_C) ? 1 : 0); break;   // ADC
    case 0xA: r = logic8(r | m); break;                     // OR
    default: r = add8(r, m, 0); break;                      // ADD (0xB)
  }
}

void M6809::execPage23(int page, uint8_t op) {
  if (page == 2) {
    if (op >= 0x20 && op < 0x30) {  // desvios longos (1020 = LBRA não documentado, sempre 6 ciclos)
      const uint16_t o = fetch16();
      if (cond(op)) {
        pc = static_cast<uint16_t>(pc + o);
        if (op != 0x20) cyc += 1;
      }
      return;
    }
    if (op == 0x3E || op == 0x3F) { interrupt(true, 0xFFF4, 0); return; }  // SWI2
  } else if (op == 0x3F) {
    interrupt(true, 0xFFF2, 0);  // SWI3
    return;
  } else if (op == 0x3E) {
    interrupt(true, 0xFFF6, CC_I | CC_F);  // não documentado: FIRQ com estado inteiro
    return;
  }
  const int mode = (op >> 4) & 3, col = op & 0x0F;
  const bool low = op < 0xC0;
  if (col == 0xF) {  // STY/STS (imediato: gravação não documentada do byte baixo)
    const uint16_t v = low ? y : s;
    if (mode == 0) {
      const uint16_t addr = pc++;
      wr(addr, static_cast<uint8_t>(v));
      cc = static_cast<uint8_t>((cc & ~(CC_Z | CC_V)) | CC_N);
      return;
    }
    const uint16_t addr = ea(mode);
    wr16(addr, v); nz16(v); setF(CC_V, false);
    return;
  }
  const uint16_t m = mode == 0 ? fetch16() : rd16(ea(mode));
  if (col == 0xE) {  // LDY/LDS
    if (low) y = m; else { s = m; nmiArmed = true; }
    nz16(m); setF(CC_V, false);
    return;
  }
  if (page == 2) {
    if (low) sub16(col == 0x3 ? D() : y, m);  // CMPD/CMPY
    else add16(D(), m);                       // XADDD não documentado: só as flags
  } else {
    if (low) sub16(col == 0x3 ? u : s, m);                  // CMPU/CMPS
    else add16(static_cast<uint16_t>(u | 0xFF00), m);       // XADDU não documentado: só as flags
  }
}

void M6809::sync(StateIO& io) {
  io.sync(a); io.sync(b); io.sync(dp); io.sync(cc);
  io.sync(x); io.sync(y); io.sync(u); io.sync(s); io.sync(pc);
  io.sync(irqLine); io.sync(firqLine); io.sync(nmiArmed); io.sync(nmiPending);
  io.sync(state);
  if (io.reading() && state > STOPPED) state = RUN;
}

}  // namespace indigo::vectrex
