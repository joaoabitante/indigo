#include "v30mz.h"

namespace indigo::ws {

// ------------------------------------------------------------ estado
void V30MZ::power() {
  for (auto& x : r) x = 0;
  sreg[ES] = 0; sreg[CS] = 0xFFFF; sreg[SS] = 0; sreg[DS] = 0;
  ip = 0;
  flags = MD;
  halted = false; prefixActive = false; pollOk = true;
  ieSnap_ = false; brkSnap_ = false; mulCV_ = false;
  opcode_ = 0;
  prefixFlush();
  m_ = ModRM{};
  flush();
}

void V30MZ::flush() {
  pfCount = 0;
  pfHead_ = 0;
  pfp = ip;
  pfWait_ = 0;
}

// um ciclo: a fila puxa o próximo byte (ou palavra, num endereço par de região de 16 bits) quando a espera da
// região já passou
void V30MZ::prefetch() {
  cycles++;
  const uint32_t a = lin(sreg[CS], pfp);
  const int t = bus_.memTiming(a);
  const int speed = t & 15;
  if (++pfWait_ < speed) return;
  pfWait_ -= speed;
  if ((t & 16) && !(pfp & 1) && pfCount < 16) {
    pf_[(pfHead_ + pfCount++) & 15] = bus_.memRead(a);
    pfp++;
  }
  if (pfCount < 16) {
    pf_[(pfHead_ + pfCount++) & 15] = bus_.memRead(lin(sreg[CS], pfp));
    pfp++;
  }
}

uint8_t V30MZ::fetch8() {
  ip++;
  while (pfCount < 2) prefetch();
  const uint8_t v = pf_[pfHead_];
  pfHead_ = (pfHead_ + 1) & 15;
  pfCount--;
  return v;
}

uint16_t V30MZ::fetch16() {
  ip = static_cast<uint16_t>(ip + 2);
  while (pfCount < 2) prefetch();
  const uint8_t lo = pf_[pfHead_];
  const uint8_t hi = pf_[(pfHead_ + 1) & 15];
  pfHead_ = (pfHead_ + 2) & 15;
  pfCount -= 2;
  return static_cast<uint16_t>(lo | (hi << 8));
}

// REP: devolve o opcode da instrução de texto para a frente da fila, para ela rodar de novo
void V30MZ::loopQueue() {
  if (pfCount == 16) { pfCount = 15; pfp--; }
  pfHead_ = (pfHead_ - 1) & 15;
  pf_[pfHead_] = opcode_;
  pfCount++;
}

// ------------------------------------------------------------ memória e portas
uint16_t V30MZ::rd8(uint16_t s, uint16_t off) {
  const uint32_t a = lin(s, off);
  cycles += bus_.memTiming(a) & 15;
  return bus_.memRead(a);
}

uint16_t V30MZ::rd16(uint16_t s, uint16_t off) {
  uint32_t a = lin(s, off);
  cycles += bus_.memTiming(a) & 15;
  const uint16_t lo = bus_.memRead(a);
  off++;
  a = lin(s, off);
  const int t = bus_.memTiming(a);
  if (!(t & 16) || !(off & 1)) cycles += t & 15;
  return static_cast<uint16_t>(lo | (bus_.memRead(a) << 8));
}

void V30MZ::wr8(uint16_t s, uint16_t off, uint16_t v) {
  const uint32_t a = lin(s, off);
  cycles += bus_.memTiming(a) & 15;
  bus_.memWrite(a, static_cast<uint8_t>(v));
}

void V30MZ::wr16(uint16_t s, uint16_t off, uint16_t v) {
  uint32_t a = lin(s, off);
  cycles += bus_.memTiming(a) & 15;
  bus_.memWrite(a, static_cast<uint8_t>(v));
  off++;
  a = lin(s, off);
  const int t = bus_.memTiming(a);
  if (!(t & 16) || !(off & 1)) cycles += t & 15;
  bus_.memWrite(a, static_cast<uint8_t>(v >> 8));
}

uint16_t V30MZ::in8(uint16_t p) {
  cycles += bus_.ioTiming(p) & 15;
  return bus_.ioRead(p);
}

uint16_t V30MZ::in16(uint16_t p) {
  cycles += bus_.ioTiming(p) & 15;
  const uint16_t lo = bus_.ioRead(p);
  p++;
  const int t = bus_.ioTiming(p);
  if (!(t & 16) || !(p & 1)) cycles += t & 15;
  return static_cast<uint16_t>(lo | (bus_.ioRead(p) << 8));
}

void V30MZ::out8(uint16_t p, uint16_t v) {
  cycles += bus_.ioTiming(p) & 15;
  bus_.ioWrite(p, static_cast<uint8_t>(v));
}

void V30MZ::out16(uint16_t p, uint16_t v) {
  cycles += bus_.ioTiming(p) & 15;
  bus_.ioWrite(p, static_cast<uint8_t>(v));
  p++;
  const int t = bus_.ioTiming(p);
  if (!(t & 16) || !(p & 1)) cycles += t & 15;
  bus_.ioWrite(p, static_cast<uint8_t>(v >> 8));
}

void V30MZ::push(uint16_t v) {
  r[SP] = static_cast<uint16_t>(r[SP] - 2);
  wr16(sreg[SS], r[SP], v);
}

uint16_t V30MZ::pop() {
  const uint16_t v = rd16(sreg[SS], r[SP]);
  r[SP] = static_cast<uint16_t>(r[SP] + 2);
  return v;
}

// segmento padrão ou o do prefixo
uint16_t V30MZ::seg(int def) const {
  switch (prefixSeg_) {
    case 0x26: return sreg[ES];
    case 0x2E: return sreg[CS];
    case 0x36: return sreg[SS];
    case 0x3E: return sreg[DS];
    default: return sreg[def];
  }
}

// ------------------------------------------------------------ ModRM
// forceAddress: LEA/LES/LDS (e CALL/JMP far) com mod=3 não usam registrador, e sim estes modos extras
void V30MZ::modrm(bool forceAddress) {
  const uint8_t b = fetch8();
  m_.mem = b & 7;
  m_.reg = (b >> 3) & 7;
  m_.mod = b >> 6;
  m_.useAddr = m_.mod != 3;
  if (m_.mod == 0 && m_.mem == 6) {
    m_.seg = seg(DS);
    m_.addr = fetch16();
  } else if (m_.mod == 3) {
    if (!forceAddress) return;
    m_.useAddr = true;
    wait(1);
    switch (m_.mem) {
      case 0: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[AX]); break;
      case 1: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[CX]); break;
      case 2: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[DX]); break;
      case 3: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[BX]); break;
      case 4: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[SI] + r[SP]); break;
      case 5: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[DI] + r[BP]); break;
      case 6: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[SI]); break;
      default: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[DI]); break;
    }
  } else {
    switch (m_.mem) {
      case 0: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[SI]); wait(1); break;
      case 1: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[DI]); wait(1); break;
      case 2: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[SI]); wait(1); break;
      case 3: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[DI]); wait(1); break;
      case 4: m_.seg = seg(DS); m_.addr = r[SI]; break;
      case 5: m_.seg = seg(DS); m_.addr = r[DI]; break;
      case 6: m_.seg = seg(SS); m_.addr = r[BP]; break;
      default: m_.seg = seg(DS); m_.addr = r[BX]; break;
    }
    if (m_.mod == 1) m_.addr = static_cast<uint16_t>(m_.addr + static_cast<int8_t>(fetch8()));
    else if (m_.mod == 2) m_.addr = static_cast<uint16_t>(m_.addr + fetch16());
  }
}

template <bool W>
uint16_t V30MZ::getMem(uint16_t offset) {
  if (!m_.useAddr) return W ? r[m_.mem] : reg8(m_.mem);
  return rd<W>(m_.seg, static_cast<uint16_t>(m_.addr + offset));
}

template <bool W>
void V30MZ::setMem(uint16_t v) {
  if (!m_.useAddr) {
    if (W) r[m_.mem] = v; else setReg8(m_.mem, v);
    return;
  }
  wr<W>(m_.seg, m_.addr, v);
}

// ------------------------------------------------------------ ALU
bool V30MZ::parity(uint8_t v) {
  v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
  return !(v & 1);
}

template <bool W>
void V30MZ::setSZP(uint16_t res) {
  constexpr uint16_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  res &= mask;
  setFlag(PF, parity(static_cast<uint8_t>(res)));
  setFlag(ZF, res == 0);
  setFlag(SF, (res & sign) != 0);
}

template <bool W>
uint16_t V30MZ::ADD(uint16_t x, uint16_t y, int c) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  const uint32_t res = static_cast<uint32_t>(x) + y + c;
  setFlag(CF, res > mask);
  setFlag(AF, ((x & 15) + (y & 15) + c) >= 16);
  setFlag(OF, ((res ^ x) & (res ^ y) & sign) != 0);
  setSZP<W>(static_cast<uint16_t>(res));
  return static_cast<uint16_t>(res & mask);
}

template <bool W>
uint16_t V30MZ::SUB(uint16_t x, uint16_t y, int c) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  const int32_t res = static_cast<int32_t>(x) - y - c;
  setFlag(CF, res < 0);
  setFlag(AF, (static_cast<int>(x & 15) - static_cast<int>(y & 15) - c) < 0);
  setFlag(OF, ((x ^ y) & (x ^ static_cast<uint32_t>(res)) & sign) != 0);
  setSZP<W>(static_cast<uint16_t>(res));
  return static_cast<uint16_t>(static_cast<uint32_t>(res) & mask);
}

// AND/OR/XOR/TEST: CF, OF e AF sempre zerados
template <bool W>
uint16_t V30MZ::LOGIC(uint16_t res) {
  flags &= static_cast<uint16_t>(~(CF | OF | AF));
  setSZP<W>(res);
  return static_cast<uint16_t>(res & (W ? 0xFFFF : 0xFF));
}

template <bool W>
uint16_t V30MZ::INC(uint16_t x) {
  constexpr uint16_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  const uint16_t res = static_cast<uint16_t>((x + 1) & mask);
  setFlag(AF, (x & 15) == 15);
  setFlag(OF, res == sign);
  setSZP<W>(res);
  return res;
}

template <bool W>
uint16_t V30MZ::DEC(uint16_t x) {
  constexpr uint16_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  const uint16_t res = static_cast<uint16_t>((x - 1) & mask);
  setFlag(AF, (x & 15) == 0);
  setFlag(OF, res == sign - 1);
  setSZP<W>(res);
  return res;
}

template <bool W>
uint16_t V30MZ::NEG(uint16_t x) {
  constexpr uint16_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  x &= mask;
  const uint16_t res = static_cast<uint16_t>((0 - x) & mask);
  setFlag(CF, x != 0);
  setFlag(AF, (x & 15) != 0);
  setFlag(OF, x == sign);
  setSZP<W>(res);
  return res;
}

// rotações: só CF e OF mudam; com contagem 0 o CF fica como estava (a contagem já vem com & 0x1F)
template <bool W>
uint16_t V30MZ::ROL(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  uint32_t res = x & mask;
  for (int i = 0; i < n; i++) {
    const bool c = (res & sign) != 0;
    res = ((res << 1) | c) & mask;
    setFlag(CF, c);
  }
  setFlag(OF, ((res ^ (flag(CF) ? sign : 0)) & sign) != 0);
  return static_cast<uint16_t>(res);
}

template <bool W>
uint16_t V30MZ::ROR(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  uint32_t res = x & mask;
  for (int i = 0; i < n; i++) {
    const bool c = res & 1;
    res = (c ? sign : 0) | (res >> 1);
    setFlag(CF, c);
  }
  setFlag(OF, ((res ^ (res << 1)) & sign) != 0);
  return static_cast<uint16_t>(res);
}

template <bool W>
uint16_t V30MZ::RCL(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  uint32_t res = x & mask;
  for (int i = 0; i < n; i++) {
    const bool c = (res & sign) != 0;
    res = ((res << 1) | (flag(CF) ? 1 : 0)) & mask;
    setFlag(CF, c);
  }
  setFlag(OF, ((res ^ (flag(CF) ? sign : 0)) & sign) != 0);
  return static_cast<uint16_t>(res);
}

template <bool W>
uint16_t V30MZ::RCR(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  uint32_t res = x & mask;
  for (int i = 0; i < n; i++) {
    const bool c = res & 1;
    res = (flag(CF) ? sign : 0) | (res >> 1);
    setFlag(CF, c);
  }
  setFlag(OF, ((res ^ (res << 1)) & sign) != 0);
  return static_cast<uint16_t>(res);
}

// deslocamentos: AF sempre zerado; S, Z e P pelo resultado (mesmo com contagem 0)
template <bool W>
uint16_t V30MZ::SHL(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80, bits = W ? 16 : 8;
  const uint32_t wide = static_cast<uint32_t>(x & mask) << n;
  if (n) setFlag(CF, (wide & (1u << bits)) != 0);
  const uint16_t res = static_cast<uint16_t>(wide & mask);
  flags &= static_cast<uint16_t>(~AF);
  setSZP<W>(res);
  setFlag(OF, ((res ^ (flag(CF) ? sign : 0)) & sign) != 0);
  return res;
}

template <bool W>
uint16_t V30MZ::SHR(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  const uint32_t v = x & mask;
  if (n) setFlag(CF, ((v >> (n - 1)) & 1) != 0);
  const uint16_t res = static_cast<uint16_t>((v >> n) & mask);
  flags &= static_cast<uint16_t>(~AF);
  setSZP<W>(res);
  setFlag(OF, ((res ^ (res << 1)) & sign) != 0);
  return res;
}

template <bool W>
uint16_t V30MZ::SAR(uint16_t x, int n) {
  constexpr uint32_t mask = W ? 0xFFFF : 0xFF, sign = W ? 0x8000 : 0x80;
  uint32_t res = x & mask;
  for (int i = 0; i < n; i++) {
    setFlag(CF, res & 1);
    res = (res & sign) | (res >> 1);
  }
  flags &= static_cast<uint16_t>(~AF);
  setSZP<W>(static_cast<uint16_t>(res));
  setFlag(OF, ((res ^ (res << 1)) & sign) != 0);
  return static_cast<uint16_t>(res);
}

// multiplicações: AF, PF e SF zerados; ZF fica ligado (no monocromático, o MUL sem sinal o zera)
template <bool W>
uint32_t V30MZ::MULU(uint16_t x, uint16_t y) {
  const uint32_t res = W ? static_cast<uint32_t>(x) * y : static_cast<uint32_t>(x & 0xFF) * (y & 0xFF);
  const bool cv = (res >> (W ? 16 : 8)) != 0;
  setFlag(CF, cv); setFlag(OF, cv);
  flags &= static_cast<uint16_t>(~(AF | PF | SF));
  setFlag(ZF, !aswan);
  mulCV_ = cv;
  return res;
}

template <bool W>
uint32_t V30MZ::MULI(uint16_t x, uint16_t y) {
  const int32_t a = W ? static_cast<int16_t>(x) : static_cast<int8_t>(x);
  const int32_t b = W ? static_cast<int16_t>(y) : static_cast<int8_t>(y);
  const int32_t res = a * b;
  const int32_t trunc = W ? static_cast<int16_t>(res) : static_cast<int8_t>(res);
  const bool cv = res != trunc;
  setFlag(CF, cv); setFlag(OF, cv);
  flags &= static_cast<uint16_t>(~(AF | PF | SF));
  flags |= ZF;
  mulCV_ = cv;
  return static_cast<uint32_t>(res);
}

// ------------------------------------------------------------ interrupções
void V30MZ::interrupt(uint8_t vector, int source) {
  if (source == 0 && swiHook) swiHook(swiCtx, vector, reg8(0));
  wait(source == 2 ? 32 : source == 1 ? 26 : 25);
  halted = false;
  pollOk = true;
  // interrupção no meio de um REP: volta ao primeiro prefixo para a cópia continuar depois
  if (prefixActive) {
    prefixActive = false;
    ip = static_cast<uint16_t>(ip - prefixCount_);
  }
  prefixFlush();
  const uint16_t newIp = rd16(0, static_cast<uint16_t>(vector * 4));
  const uint16_t newCs = rd16(0, static_cast<uint16_t>(vector * 4 + 2));
  push(psw());
  flags |= MD;
  flags &= static_cast<uint16_t>(~(IF | TF));
  push(sreg[CS]);
  sreg[CS] = newCs;
  push(ip);
  ip = newIp;
  flush();
}

bool V30MZ::irq(uint8_t vector) {
  halted = false;
  if (!ieSnap_ || !flag(IF)) return false;
  interrupt(vector, 2);
  return true;
}

// ------------------------------------------------------------ laço principal
void V30MZ::step() {
  brkSnap_ = flag(TF);
  ieSnap_ = flag(IF);
  pollOk = true;
  prefixActive = false;
  if (halted) { wait(1); return; }
  opcode_ = fetch8();
  execute(opcode_);
  if (!prefixActive) prefixFlush();
  if (pollOk && brkSnap_) interrupt(1, 0);
}

// ------------------------------------------------------------ grupos
template <bool W>
uint16_t V30MZ::alu(int kind, uint16_t x, uint16_t y) {
  switch (kind) {
    case 0: return ADD<W>(x, y);
    case 1: return LOGIC<W>(x | y);
    case 2: return ADD<W>(x, y, flag(CF) ? 1 : 0);
    case 3: return SUB<W>(x, y, flag(CF) ? 1 : 0);
    case 4: return LOGIC<W>(x & y);
    case 5: return SUB<W>(x, y);
    case 6: return LOGIC<W>(x ^ y);
    default: SUB<W>(x, y); return x;  // CMP
  }
}

template <bool W>
void V30MZ::aluMemReg(int kind) {
  wait(1);
  modrm();
  const uint16_t res = alu<W>(kind, getMem<W>(), getReg<W>());
  if (kind != 7) setMem<W>(res);
}

template <bool W>
void V30MZ::aluRegMem(int kind) {
  wait(1);
  modrm();
  const uint16_t res = alu<W>(kind, getReg<W>(), getMem<W>());
  if (kind != 7) setReg<W>(res);
}

template <bool W>
void V30MZ::aluAccImm(int kind) {
  wait(1);
  const uint16_t imm = W ? fetch16() : fetch8();
  const uint16_t res = alu<W>(kind, acc<W>(), imm);
  if (kind != 7) setAcc<W>(res);
}

template <bool W>
void V30MZ::group1(bool signExtend) {
  wait(1);
  modrm();
  const uint16_t mem = getMem<W>();
  uint16_t imm;
  if (signExtend) imm = static_cast<uint16_t>(static_cast<int8_t>(fetch8()));
  else imm = W ? fetch16() : fetch8();
  if (!W) imm &= 0xFF;
  const uint16_t res = alu<W>(m_.reg, mem, imm);
  if (m_.reg != 7) setMem<W>(res);
}

template <bool W>
void V30MZ::group2(int clocks, int count) {
  wait(clocks);
  modrm();
  const uint16_t mem = getMem<W>();
  const int n = (count < 0 ? fetch8() : count) & 31;
  switch (m_.reg) {
    case 0: setMem<W>(ROL<W>(mem, n)); break;
    case 1: setMem<W>(ROR<W>(mem, n)); break;
    case 2: setMem<W>(RCL<W>(mem, n)); break;
    case 3: setMem<W>(RCR<W>(mem, n)); break;
    case 4: setMem<W>(SHL<W>(mem, n)); break;
    case 5: setMem<W>(SHR<W>(mem, n)); break;
    case 6: setMem<W>(0); break;  // não documentado: zera o operando
    default: setMem<W>(SAR<W>(mem, n)); break;
  }
}

template <bool W>
void V30MZ::group3() {
  modrm();
  const uint16_t mem = getMem<W>();
  switch (m_.reg) {
    case 0: wait(1); LOGIC<W>(mem & (W ? fetch16() : fetch8())); break;  // TEST
    case 1: wait(1); break;  // não documentado: não faz nada (nem lê imediato)
    case 2: wait(1); setMem<W>(static_cast<uint16_t>(~mem)); break;
    case 3: wait(1); setMem<W>(NEG<W>(mem)); break;
    case 4: {
      wait(3);
      const uint32_t res = MULU<W>(acc<W>(), mem);
      if (W) { r[AX] = static_cast<uint16_t>(res); r[DX] = static_cast<uint16_t>(res >> 16); }
      else r[AX] = static_cast<uint16_t>(res);
      break;
    }
    case 5: {
      wait(3);
      const uint32_t res = MULI<W>(acc<W>(), mem);
      if (W) { r[AX] = static_cast<uint16_t>(res); r[DX] = static_cast<uint16_t>(res >> 16); }
      else r[AX] = static_cast<uint16_t>(res);
      break;
    }
    case 6: wait(W ? 23 : 15); divu<W>(); break;
    default: wait(W ? 24 : 17); divs<W>(); break;
  }
}

// DIV sem sinal. As bandeiras seguem o hardware (WSCPUTest): no 16/8, CF e OF repetem a última multiplicação;
// ZF sai de um detalhe do divisor interno, inclusive quando a divisão estoura.
template <bool W>
void V30MZ::divu() {
  const uint16_t d = getMem<W>();
  flags &= static_cast<uint16_t>(~(AF | PF | SF | CF | OF | ZF));
  if (!W) {
    const uint16_t x = r[AX];
    const uint16_t dv = d & 0xFF;
    if (mulCV_) flags |= CF | OF;
    if ((x >> 8) >= dv) {
      bool z;
      if (dv == 0) z = (x & 0x3FC0) == 0;
      else {
        const uint32_t D = x & 0xFFC0, X = ((0x100u - dv) & 0xFF) << 6;
        z = ((D + X) & 0xFFFF) == 0 || ((D + 3 * X) & 0xFFFF) == 0;
      }
      setFlag(ZF, z);
      interrupt(0, 0);
      return;
    }
    const uint16_t q = x / dv, rem = x % dv;
    r[AX] = static_cast<uint16_t>((rem << 8) | q);
    setFlag(ZF, rem == 0 && (q & 1));
  } else {
    const uint32_t x = r[AX] | (static_cast<uint32_t>(r[DX]) << 16);
    if ((x >> 16) >= d) {
      bool z;
      if (d == 0) z = (x & 0x3FFFC000u) == 0;
      else {
        const uint32_t D = x & 0xFFFFC000u, X = ((0x10000u - d) & 0xFFFF) << 14;
        z = ((D + X) & 0xFFFFC000u) == 0 || ((D + 3 * X) & 0xFFFFC000u) == 0;
      }
      setFlag(ZF, z);
      interrupt(0, 0);
      return;
    }
    const uint32_t q = x / d, rem = x % d;
    r[AX] = static_cast<uint16_t>(q);
    r[DX] = static_cast<uint16_t>(rem);
    setFlag(ZF, rem == 0 && (q & 1));
  }
}

// DIV com sinal. Curiosidades do V30MZ: 8000h/0 dá 0081h e 80000000h/0 dá 00008001h sem exceção; um
// quociente de -128 (ou -32768) também estoura.
template <bool W>
void V30MZ::divs() {
  const uint16_t dRaw = getMem<W>();
  flags &= static_cast<uint16_t>(~(AF | PF | SF | CF | OF | ZF));
  if (!W) {
    const int32_t x = static_cast<int16_t>(r[AX]);
    const int32_t d = static_cast<int8_t>(dRaw);
    auto fail = [&] {
      if (mulCV_) flags |= CF | OF;
      interrupt(0, 0);
    };
    if (d == 0) {
      if (r[AX] == 0x8000) { r[AX] = 0x0081; setSZP<false>(0x81); return; }
      fail();
      return;
    }
    const uint32_t n = static_cast<uint32_t>(x < 0 ? -x : x), dd = static_cast<uint32_t>(d < 0 ? -d : d);
    if (n >= (dd << 7)) { fail(); return; }
    uint32_t q = n / dd, rem = n % dd;
    if ((x < 0) != (d < 0)) q = 0 - q;
    if (x < 0) rem = 0 - rem;
    r[AX] = static_cast<uint16_t>(((rem & 0xFF) << 8) | (q & 0xFF));
    setSZP<false>(static_cast<uint16_t>(q & 0xFF));
  } else {
    const uint32_t raw = r[AX] | (static_cast<uint32_t>(r[DX]) << 16);
    const int64_t x = static_cast<int32_t>(raw);
    const int64_t d = static_cast<int16_t>(dRaw);
    if (d == 0) {
      if (raw == 0x80000000u) { r[AX] = 0x8001; r[DX] = 0; flags |= ZF; return; }
      interrupt(0, 0);
      return;
    }
    const uint64_t n = static_cast<uint64_t>(x < 0 ? -x : x), dd = static_cast<uint64_t>(d < 0 ? -d : d);
    if (n >= (dd << 15)) { interrupt(0, 0); return; }
    uint64_t q = n / dd, rem = n % dd;
    if ((x < 0) != (d < 0)) q = 0 - q;
    if (x < 0) rem = 0 - rem;
    r[AX] = static_cast<uint16_t>(q);
    r[DX] = static_cast<uint16_t>(rem);
    setFlag(ZF, (rem & 0xFFFF) == 0 && (q & 1));
  }
}

template <bool W>
void V30MZ::group4() {
  modrm();
  // CALL/JMP far com mod=3 usam os modos de endereço extras (como LEA com registrador)
  if (m_.mod == 3 && (m_.reg == 3 || m_.reg == 5)) {
    m_.useAddr = true;
    wait(1);
    switch (m_.mem) {
      case 0: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[AX]); break;
      case 1: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[CX]); break;
      case 2: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[DX]); break;
      case 3: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[BX]); break;
      case 4: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[SI] + r[SP]); break;
      case 5: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[DI] + r[BP]); break;
      case 6: m_.seg = seg(SS); m_.addr = static_cast<uint16_t>(r[BP] + r[SI]); break;
      default: m_.seg = seg(DS); m_.addr = static_cast<uint16_t>(r[BX] + r[DI]); break;
    }
  }
  switch (m_.reg) {
    case 0: wait(1); setMem<W>(INC<W>(getMem<W>())); break;
    case 1: wait(1); setMem<W>(DEC<W>(getMem<W>())); break;
    case 2: {  // CALL near
      wait(3);
      const uint16_t target = getMem<true>();
      push(ip);
      ip = target;
      flush();
      break;
    }
    case 3: {  // CALL far: empilha o retorno antes de ler o destino
      wait(7);
      push(sreg[CS]);
      push(ip);
      const uint16_t newIp = getMem<true>(0);
      const uint16_t newCs = getMem<true>(2);
      ip = newIp;
      sreg[CS] = newCs;
      flush();
      break;
    }
    case 4: wait(4); ip = getMem<true>(); flush(); break;
    case 5: {
      wait(9);
      const uint16_t newIp = getMem<true>(0);
      const uint16_t newCs = getMem<true>(2);
      ip = newIp;
      sreg[CS] = newCs;
      flush();
      break;
    }
    case 6:  // PUSH (com SP como registrador, empilha o valor já decrementado, como o 54h)
      if (!m_.useAddr && m_.mem == SP) push(static_cast<uint16_t>(r[SP] - 2));
      else push(getMem<true>());
      break;
    default: wait(1); break;
  }
}

// ------------------------------------------------------------ BCD
void V30MZ::daa(bool sub) {
  wait(sub ? 11 : 10);
  const uint8_t orig = static_cast<uint8_t>(r[AX]);
  const bool cy = flag(CF) || orig >= 0x9A;
  const bool ac = flag(AF) || (orig & 15) >= 10;
  uint8_t al = orig;
  if (cy) al = static_cast<uint8_t>(sub ? al - 0x60 : al + 0x60);
  if (ac) al = static_cast<uint8_t>(sub ? al - 0x06 : al + 0x06);
  setReg8(0, al);
  setFlag(CF, cy);
  setFlag(AF, ac);
  // OF: estouro com sinal da diferença entre o resultado e o valor original
  setFlag(OF, ((al ^ orig) & (al ^ static_cast<uint8_t>(al - orig)) & 0x80) != 0);
  setSZP<false>(al);
}

void V30MZ::aaa(bool sub) {
  wait(9);
  if (flag(AF) || (r[AX] & 15) > 9) {
    const uint8_t al = static_cast<uint8_t>(((r[AX] & 0xFF) + (sub ? -6 : 6)) & 15);
    const uint8_t ah = static_cast<uint8_t>((r[AX] >> 8) + (sub ? -1 : 1));
    r[AX] = static_cast<uint16_t>(al | (ah << 8));
    flags |= AF | CF | ZF;
    flags &= static_cast<uint16_t>(~SF);
  } else {
    r[AX] &= 0xFF0F;
    flags &= static_cast<uint16_t>(~(AF | CF | ZF));
    flags |= SF;
  }
  flags &= static_cast<uint16_t>(~OF);
  flags |= PF;
}

void V30MZ::aam() {
  wait(16);
  const uint8_t imm = fetch8();
  const uint8_t al = static_cast<uint8_t>(r[AX]);
  flags &= static_cast<uint16_t>(~(AF | PF | SF | CF | OF | ZF));
  if (imm == 0) {
    if (mulCV_) flags |= CF | OF;
    setFlag(ZF, (al & 0xC0) == 0);
    interrupt(0, 0);
    return;
  }
  r[AX] = static_cast<uint16_t>(((al / imm) << 8) | (al % imm));
  setSZP<false>(static_cast<uint16_t>(al % imm));
}

void V30MZ::aad() {
  wait(6);
  const uint8_t imm = fetch8();
  const uint8_t prod = static_cast<uint8_t>((r[AX] >> 8) * imm);
  r[AX] = ADD<false>(prod, r[AX] & 0xFF);
}

// ------------------------------------------------------------ desvios e texto
void V30MZ::jumpIf(bool cond) {
  wait(1);
  const int8_t off = static_cast<int8_t>(fetch8());
  if (cond) {
    wait(2);
    ip = static_cast<uint16_t>(ip + off);
    flush();
  }
}

// depois de uma volta de REP: decide se repete (true = a instrução volta para a fila)
bool V30MZ::repeatContinue(bool compare) {
  if (!prefixRepeat_) return false;
  if (--r[CX] == 0) return false;
  if (compare) {
    if (prefixRepeat_ == 0xF2 && flag(ZF)) return false;
    if (prefixRepeat_ == 0xF3 && !flag(ZF)) return false;
  }
  prefixActive = true;
  ip--;
  loopQueue();
  return true;
}

template <bool W>
void V30MZ::movs() {
  wait(3);
  if (prefixRepeat_ && !r[CX]) return;
  const uint16_t v = rd<W>(seg(DS), r[SI]);
  wr<W>(sreg[ES], r[DI], v);
  const int d = flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1);
  r[SI] = static_cast<uint16_t>(r[SI] + d);
  r[DI] = static_cast<uint16_t>(r[DI] + d);
  repeatContinue(false);
}

template <bool W>
void V30MZ::cmps() {
  wait(4);
  if (prefixRepeat_ && !r[CX]) return;
  const uint16_t x = rd<W>(seg(DS), r[SI]);
  const uint16_t y = rd<W>(sreg[ES], r[DI]);
  const int d = flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1);
  r[SI] = static_cast<uint16_t>(r[SI] + d);
  r[DI] = static_cast<uint16_t>(r[DI] + d);
  SUB<W>(x, y);
  repeatContinue(true);
}

template <bool W>
void V30MZ::stos() {
  wait(2);
  if (prefixRepeat_ && !r[CX]) return;
  wr<W>(sreg[ES], r[DI], acc<W>());
  r[DI] = static_cast<uint16_t>(r[DI] + (flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1)));
  repeatContinue(false);
}

template <bool W>
void V30MZ::lods() {
  wait(2);
  if (prefixRepeat_ && !r[CX]) return;
  setAcc<W>(rd<W>(seg(DS), r[SI]));
  r[SI] = static_cast<uint16_t>(r[SI] + (flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1)));
  repeatContinue(false);
}

template <bool W>
void V30MZ::scas() {
  wait(3);
  if (prefixRepeat_ && !r[CX]) return;
  const uint16_t y = rd<W>(sreg[ES], r[DI]);
  r[DI] = static_cast<uint16_t>(r[DI] + (flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1)));
  SUB<W>(acc<W>(), y);
  repeatContinue(true);
}

template <bool W>
void V30MZ::ins() {
  wait(3);
  if (prefixRepeat_ && !r[CX]) return;
  const uint16_t v = in<W>(r[DX]);
  wr<W>(sreg[ES], r[DI], v);
  r[DI] = static_cast<uint16_t>(r[DI] + (flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1)));
  repeatContinue(false);
}

template <bool W>
void V30MZ::outs() {
  wait(3);
  if (prefixRepeat_ && !r[CX]) return;
  const uint16_t v = rd<W>(seg(DS), r[SI]);
  out<W>(r[DX], v);
  r[SI] = static_cast<uint16_t>(r[SI] + (flag(DF) ? -(W ? 2 : 1) : (W ? 2 : 1)));
  repeatContinue(false);
}

void V30MZ::enter() {
  wait(7);
  const uint16_t size = fetch16();
  const int level = fetch8() & 0x1F;
  push(r[BP]);
  const uint16_t frame = r[SP];
  if (level) {
    wait(level > 1 ? 7 : 6);
    for (int i = 1; i < level; i++) {
      wait(2);
      push(rd16(seg(SS), static_cast<uint16_t>(r[BP] - i * 2)));
    }
    push(frame);
  }
  r[BP] = frame;
  r[SP] = static_cast<uint16_t>(r[SP] - size);
}

// ------------------------------------------------------------ decodificação
void V30MZ::execute(uint8_t op) {
  // as 6 operações de ALU de cada bloco 00-3F
  if (op < 0x40 && (op & 7) < 6) {
    const int kind = op >> 3;
    switch (op & 7) {
      case 0: aluMemReg<false>(kind); return;
      case 1: aluMemReg<true>(kind); return;
      case 2: aluRegMem<false>(kind); return;
      case 3: aluRegMem<true>(kind); return;
      case 4: aluAccImm<false>(kind); return;
      default: aluAccImm<true>(kind); return;
    }
  }
  if (op >= 0x40 && op < 0x60) {
    const int i = op & 7;
    switch (op >> 3) {
      case 8: wait(1); r[i] = INC<true>(r[i]); return;
      case 9: wait(1); r[i] = DEC<true>(r[i]); return;
      case 10: push(i == SP ? static_cast<uint16_t>(r[SP] - 2) : r[i]); return;
      default: r[i] = pop(); return;
    }
  }
  if (op >= 0x70 && op < 0x80) {
    bool c;
    switch (op & 15) {
      case 0: c = flag(OF); break;
      case 1: c = !flag(OF); break;
      case 2: c = flag(CF); break;
      case 3: c = !flag(CF); break;
      case 4: c = flag(ZF); break;
      case 5: c = !flag(ZF); break;
      case 6: c = flag(ZF) || flag(CF); break;
      case 7: c = !flag(ZF) && !flag(CF); break;
      case 8: c = flag(SF); break;
      case 9: c = !flag(SF); break;
      case 10: c = flag(PF); break;
      case 11: c = !flag(PF); break;
      case 12: c = flag(SF) != flag(OF); break;
      case 13: c = flag(SF) == flag(OF); break;
      case 14: c = flag(SF) != flag(OF) || flag(ZF); break;
      default: c = flag(SF) == flag(OF) && !flag(ZF); break;
    }
    jumpIf(c);
    return;
  }
  if (op >= 0x91 && op <= 0x97) {
    wait(3);
    const uint16_t t = r[AX]; r[AX] = r[op & 7]; r[op & 7] = t;
    return;
  }
  if (op >= 0xB0 && op <= 0xB7) { wait(1); setReg8(op & 7, fetch8()); return; }
  if (op >= 0xB8 && op <= 0xBF) { wait(1); r[op & 7] = fetch16(); return; }

  switch (op) {
    case 0x06: wait(1); push(sreg[ES]); return;
    case 0x07: wait(2); sreg[ES] = pop(); return;
    case 0x0E: wait(1); push(sreg[CS]); return;
    case 0x16: wait(1); push(sreg[SS]); return;
    case 0x17: wait(2); sreg[SS] = pop(); pollOk = false; return;
    case 0x1E: wait(1); push(sreg[DS]); return;
    case 0x1F: wait(2); sreg[DS] = pop(); return;
    case 0x26: case 0x2E: case 0x36: case 0x3E:  // prefixos de segmento
      prefixSeg_ = op; prefixCount_++; prefixActive = true; pollOk = false; return;
    case 0x27: daa(false); return;
    case 0x2F: daa(true); return;
    case 0x37: aaa(false); return;
    case 0x3F: aaa(true); return;
    case 0x60: {  // PUSHA
      wait(1);
      const uint16_t sp = r[SP];
      push(r[AX]); push(r[CX]); push(r[DX]); push(r[BX]);
      push(sp); push(r[BP]); push(r[SI]); push(r[DI]);
      return;
    }
    case 0x61:  // POPA (SP não é restaurado)
      wait(1);
      r[DI] = pop(); r[SI] = pop(); r[BP] = pop(); pop();
      r[BX] = pop(); r[DX] = pop(); r[CX] = pop(); r[AX] = pop();
      return;
    case 0x62: {  // BOUND
      wait(12);
      modrm();
      const int16_t lo = static_cast<int16_t>(getMem<true>(0));
      const int16_t hi = static_cast<int16_t>(getMem<true>(2));
      const int16_t v = static_cast<int16_t>(getReg<true>());
      if (v < lo || v > hi) interrupt(5, 0);
      return;
    }
    case 0x68: push(fetch16()); return;
    case 0x69: case 0x6B: {
      wait(3);
      modrm();
      const uint16_t mem = getMem<true>();
      const uint16_t imm = op == 0x69 ? fetch16() : static_cast<uint16_t>(static_cast<int8_t>(fetch8()));
      setReg<true>(static_cast<uint16_t>(MULI<true>(mem, imm)));
      return;
    }
    case 0x6A: push(static_cast<uint16_t>(static_cast<int8_t>(fetch8()))); return;
    case 0x6C: ins<false>(); return;
    case 0x6D: ins<true>(); return;
    case 0x6E: outs<false>(); return;
    case 0x6F: outs<true>(); return;
    case 0x80: group1<false>(false); return;
    case 0x81: group1<true>(false); return;
    case 0x82: group1<false>(true); return;
    case 0x83: group1<true>(true); return;
    case 0x84: wait(1); modrm(); LOGIC<false>(getMem<false>() & getReg<false>()); return;
    case 0x85: wait(1); modrm(); LOGIC<true>(getMem<true>() & getReg<true>()); return;
    case 0x86: case 0x87: {
      wait(3);
      modrm();
      if (op == 0x86) { const uint16_t a = getMem<false>(), b = getReg<false>(); setMem<false>(b); setReg<false>(a); }
      else { const uint16_t a = getMem<true>(), b = getReg<true>(); setMem<true>(b); setReg<true>(a); }
      return;
    }
    case 0x88: modrm(); if (m_.mod == 3) wait(1); setMem<false>(getReg<false>()); return;
    case 0x89: modrm(); if (m_.mod == 3) wait(1); setMem<true>(getReg<true>()); return;
    case 0x8A: modrm(); if (m_.mod == 3) wait(1); setReg<false>(getMem<false>()); return;
    case 0x8B: modrm(); if (m_.mod == 3) wait(1); setReg<true>(getMem<true>()); return;
    case 0x8C: wait(1); modrm(); setMem<true>(sreg[m_.reg & 3]); return;
    case 0x8D: wait(1); modrm(true); setReg<true>(m_.addr); return;
    case 0x8E:
      wait(2);
      modrm();
      sreg[m_.reg & 3] = getMem<true>();
      if ((m_.reg & 3) == SS) pollOk = false;
      return;
    case 0x8F: { wait(1); modrm(); const uint16_t v = pop(); setMem<true>(v); return; }
    case 0x90: wait(1); return;
    case 0x98: wait(1); r[AX] = static_cast<uint16_t>(static_cast<int8_t>(r[AX] & 0xFF)); return;
    case 0x99: wait(1); r[DX] = (r[AX] & 0x8000) ? 0xFFFF : 0; return;
    case 0x9A: {
      wait(6);
      const uint16_t newIp = fetch16(), newCs = fetch16();
      push(sreg[CS]); push(ip);
      sreg[CS] = newCs; ip = newIp;
      flush();
      return;
    }
    case 0x9B: wait(10); return;  // POLL: no WonderSwan o pino fica sempre pronto
    case 0x9C: wait(1); push(psw()); return;
    case 0x9D: wait(2); flags = pop() & 0x8FD5; return;
    case 0x9E: wait(4); flags = static_cast<uint16_t>(((flags & 0xFF00) | (r[AX] >> 8)) & 0x8FD5); return;
    case 0x9F: wait(2); setReg8(4, psw() & 0xFF); return;
    case 0xA0: setReg8(0, rd8(seg(DS), fetch16())); return;
    case 0xA1: r[AX] = rd16(seg(DS), fetch16()); return;
    case 0xA2: wr8(seg(DS), fetch16(), r[AX]); return;
    case 0xA3: wr16(seg(DS), fetch16(), r[AX]); return;
    case 0xA4: movs<false>(); return;
    case 0xA5: movs<true>(); return;
    case 0xA6: cmps<false>(); return;
    case 0xA7: cmps<true>(); return;
    case 0xA8: wait(1); LOGIC<false>(r[AX] & fetch8()); return;
    case 0xA9: wait(1); LOGIC<true>(r[AX] & fetch16()); return;
    case 0xAA: stos<false>(); return;
    case 0xAB: stos<true>(); return;
    case 0xAC: lods<false>(); return;
    case 0xAD: lods<true>(); return;
    case 0xAE: scas<false>(); return;
    case 0xAF: scas<true>(); return;
    case 0xC0: group2<false>(3, -1); return;
    case 0xC1: group2<true>(3, -1); return;
    case 0xC2: { wait(4); const uint16_t n = fetch16(); ip = pop(); r[SP] = static_cast<uint16_t>(r[SP] + n); flush(); return; }
    case 0xC3: wait(4); ip = pop(); flush(); return;
    case 0xC4: case 0xC5:
      wait(4);
      modrm(true);
      setReg<true>(getMem<true>(0));
      sreg[op == 0xC4 ? ES : DS] = getMem<true>(2);
      return;
    case 0xC6: modrm(); setMem<false>(fetch8()); return;
    case 0xC7: modrm(); setMem<true>(fetch16()); return;
    case 0xC8: enter(); return;
    case 0xC9: wait(2); r[SP] = r[BP]; r[BP] = pop(); return;
    case 0xCA: {
      wait(6);
      const uint16_t n = fetch16();
      ip = pop(); sreg[CS] = pop();
      r[SP] = static_cast<uint16_t>(r[SP] + n);
      flush();
      return;
    }
    case 0xCB: wait(5); ip = pop(); sreg[CS] = pop(); flush(); return;
    case 0xCC: wait(9); interrupt(3, 0); return;
    case 0xCD: { wait(10); const uint8_t v = fetch8(); interrupt(v, 0); return; }
    case 0xCE: wait(6); if (flag(OF)) { wait(7); interrupt(4, 0); } return;
    case 0xCF: wait(6); ip = pop(); sreg[CS] = pop(); flags = pop() & 0x8FD5; flush(); return;
    case 0xD0: group2<false>(1, 1); return;
    case 0xD1: group2<true>(1, 1); return;
    case 0xD2: group2<false>(3, r[CX] & 0xFF); return;
    case 0xD3: group2<true>(3, r[CX] & 0xFF); return;
    case 0xD4: aam(); return;
    case 0xD5: aad(); return;
    case 0xD6: wait(8); setReg8(0, flag(CF) ? 0xFF : 0x00); return;  // SALC
    case 0xD7: wait(4); setReg8(0, rd8(seg(DS), static_cast<uint16_t>(r[BX] + (r[AX] & 0xFF)))); return;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
      wait(1); fetch8(); return;  // FPO1: NOP de 2 bytes
    case 0xE0: case 0xE1: {  // LOOPNZ / LOOPZ
      wait(2);
      const int8_t off = static_cast<int8_t>(fetch8());
      if (--r[CX] && flag(ZF) == (op == 0xE1)) { wait(3); ip = static_cast<uint16_t>(ip + off); flush(); }
      return;
    }
    case 0xE2: {
      wait(1);
      const int8_t off = static_cast<int8_t>(fetch8());
      if (--r[CX]) { wait(3); ip = static_cast<uint16_t>(ip + off); flush(); }
      return;
    }
    case 0xE3: jumpIf(r[CX] == 0); return;
    case 0xE4: { wait(5); const uint8_t p = fetch8(); setReg8(0, in8(p)); wait(1); return; }
    case 0xE5: { wait(5); const uint8_t p = fetch8(); r[AX] = in16(p); wait(1); return; }
    case 0xE6: { wait(6); const uint8_t p = fetch8(); out8(p, r[AX]); return; }
    case 0xE7: { wait(6); const uint8_t p = fetch8(); out16(p, r[AX]); return; }
    case 0xE8: {
      wait(2);
      const uint16_t off = fetch16();
      push(ip);
      ip = static_cast<uint16_t>(ip + off);
      flush();
      return;
    }
    case 0xE9: { wait(3); const uint16_t off = fetch16(); ip = static_cast<uint16_t>(ip + off); flush(); return; }
    case 0xEA: {
      wait(6);
      const uint16_t newIp = fetch16(), newCs = fetch16();
      sreg[CS] = newCs; ip = newIp;
      flush();
      return;
    }
    case 0xEB: { wait(3); const int8_t off = static_cast<int8_t>(fetch8()); ip = static_cast<uint16_t>(ip + off); flush(); return; }
    case 0xEC: wait(4); setReg8(0, in8(r[DX])); wait(1); return;
    case 0xED: wait(4); r[AX] = in16(r[DX]); wait(1); return;
    case 0xEE: wait(5); out8(r[DX], r[AX]); return;
    case 0xEF: wait(5); out16(r[DX], r[AX]); return;
    case 0xF0: prefixCount_++; prefixActive = true; pollOk = false; return;  // LOCK
    case 0xF2: case 0xF3:
      prefixRepeat_ = op; prefixCount_++; wait(4); prefixActive = true; pollOk = false; return;
    case 0xF4: wait(9); halted = true; return;
    case 0xF5: wait(4); flags ^= CF; return;
    case 0xF6: group3<false>(); return;
    case 0xF7: group3<true>(); return;
    case 0xF8: wait(4); flags &= static_cast<uint16_t>(~CF); return;
    case 0xF9: wait(4); flags |= CF; return;
    case 0xFA: wait(4); flags &= static_cast<uint16_t>(~IF); return;
    case 0xFB: wait(4); flags |= IF; return;
    case 0xFC: wait(4); flags &= static_cast<uint16_t>(~DF); return;
    case 0xFD: wait(4); flags |= DF; return;
    case 0xFE: group4<false>(); return;
    case 0xFF: group4<true>(); return;
    default: wait(1); return;  // 0F, 63-67, F1 e outros sem função: NOP de 1 byte
  }
}

// ------------------------------------------------------------ save state
void V30MZ::sync(StateIO& io) {
  io.raw(r, sizeof r); io.raw(sreg, sizeof sreg);
  io.sync(ip); io.sync(flags); io.sync(cycles);
  io.sync(halted); io.sync(prefixActive); io.sync(pollOk);
  io.sync(pfp); io.sync(pfCount); io.raw(pf_, sizeof pf_); io.sync(pfHead_); io.sync(pfWait_);
  io.sync(opcode_); io.sync(prefixCount_); io.sync(prefixRepeat_); io.sync(prefixSeg_);
  io.sync(ieSnap_); io.sync(brkSnap_); io.sync(mulCV_);
  if (io.reading()) {
    flags &= 0x8FD5;
    clampTo(pfCount, 0, 16);
    pfHead_ &= 15;
    clampTo(pfWait_, 0, 2);
    clampTo(prefixCount_, 0, 1 << 20);
    if (prefixRepeat_ != 0xF2 && prefixRepeat_ != 0xF3) prefixRepeat_ = 0;
    if (prefixSeg_ != 0x26 && prefixSeg_ != 0x2E && prefixSeg_ != 0x36 && prefixSeg_ != 0x3E) prefixSeg_ = 0;
    clampTo<int64_t>(cycles, 0, int64_t(1) << 60);
  }
}

}  // namespace indigo::ws
