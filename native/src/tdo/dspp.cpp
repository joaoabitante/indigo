// DSPP do 3DO, ver dspp.h.
#include "dspp.h"

#include <cstring>

namespace indigo::tdo {

namespace {
int32_t sext20(uint32_t v) { return int32_t(v << 12) >> 12; }
}  // namespace

void Dspp::reset() {
  pc = 0;
  sp = 0;
  semStatus = semData = 0;
  audlock = false;
  sleeping = false;
  writeback_ = -1;
  setRmap(0, 0);
}

void Dspp::clearAll() {
  std::memset(data, 0, sizeof data);
  running = false;
  acc = 0;
  fz = fn = fc = fv = fx = false;
  std::memset(stack, 0, sizeof stack);
  tclock = tick = 0;
  noise = 0x1234567;
  outputs[0] = outputs[1] = 0;
  outL = outR = 0;
  for (auto& f : fifo) f = Fifo{};
  chEnable = chComplete = 0;
  reset();
}

// RMAP 0 (o do sistema): R0-R3 na EI (0x000), R4-R7 na I (0x104), R8-R11 em 0x200 (espelho da I), R12-R15 na
// EO (0x304); o RBASE entra por XOR nos bits 2-7
void Dspp::setRmap(uint8_t map, uint16_t base) {
  rmap = map & 7;
  rbaseXor = base & 0xFC;
  for (int g = 0; g < 4; g++) {
    const bool x = g & 1, y = g & 2;
    bool sel;
    switch (rmap) {
      case 4: sel = y; break;
      case 5: sel = !y; break;
      case 6: sel = x && y; break;
      case 7: sel = x || y; break;
      default: sel = x; break;
    }
    rbase[g] = uint16_t((((y ? 1 : 0) << 9) | ((sel ? 1 : 0) << 8) | ((x ? 1 : 0) << 2)) ^ rbaseXor);
  }
}

void Dspp::setRbase(int base, uint16_t addr) {
  switch (base) {
    case 4: rbase[1] = addr; break;
    case 0: rbase[0] = addr; rbase[1] = uint16_t(addr + 4); [[fallthrough]];
    case 8: rbase[2] = uint16_t(addr + 8 - base); [[fallthrough]];
    case 12: rbase[3] = uint16_t(addr + 12 - base); break;
    default: break;
  }
}

// ------------------------------------------------------------------------------------------------ memória

uint16_t Dspp::popFifo(int ch) {
  Fifo& f = fifo[ch];
  if (!f.depth) return f.prev;  // vazio: repete a última
  const uint16_t v = f.buf[f.dspPtr];
  f.dspPtr = uint8_t((f.dspPtr + 1) & (FIFO_DEPTH - 1));
  f.depth--;
  f.prev = v;
  return v;
}

uint16_t Dspp::read(uint16_t a) {
  a &= DATA_WORDS - 1;
  if (a >= 0x070 && a <= 0x07E) { const Fifo& f = fifo[a - 0x070]; return f.depth ? f.buf[f.dspPtr] : f.prev; }
  if (a >= 0x0D0 && a <= 0x0DE) return fifo[a - 0x0D0].depth;
  if (a >= 0x0E0 && a <= 0x0E3) return fifo[16 + a - 0x0E0].depth;
  if (a >= 0x0F0 && a <= 0x0FE) return popFifo(a - 0x0F0);
  switch (a) {
    case 0x0EA:
      noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
      return uint16_t(noise);
    case 0x0EC: return semStatus;
    case 0x0ED: return semData;
    case 0x0EE: return pc;
    case 0x0EF: return tclock;
    case 0x3EE: return tick;
    default: break;
  }
  if (a >= 0x200 && a < 0x300) return data[a - 0x100];
  if (a >= 0x3EB) return 0;
  return data[a];
}

void Dspp::write(uint16_t a, uint16_t v) {
  a &= DATA_WORDS - 1;
  if (a >= 0x070 && a <= 0x07E) { fifo[a - 0x070].prev = v; return; }
  if (a >= 0x0D0 && a < 0x100) {
    if (a == 0x0EE) pc = v & (CODE_WORDS - 1);
    else if (a == 0x0EF) tclock = v;
    return;
  }
  if (a >= 0x200 && a < 0x300) { data[a - 0x100] = v; return; }
  if (a >= 0x3EB) {
    switch (a) {
      case 0x3EB: audlock = v & 0x8000; break;
      case 0x3EC: semStatus |= 1; break;
      case 0x3ED: semStatus = 4; semData = v; break;
      case 0x3EE: tick = v; if (irq) irq(); break;
      case 0x3EF: tclock = v; break;
      case 0x3F0: case 0x3F1: case 0x3F2: case 0x3F3: {
        Fifo& f = fifo[16 + a - 0x3F0];
        if (f.depth < FIFO_DEPTH) {
          f.buf[f.dspPtr] = v;
          f.dspPtr = uint8_t((f.dspPtr + 1) & (FIFO_DEPTH - 1));
          f.depth++;
        }
        break;
      }
      case 0x3FE: outputs[0] = v; break;
      case 0x3FF: outputs[1] = v; break;
      default: break;
    }
    return;
  }
  data[a] = v;
}

// ------------------------------------------------------------------------------------------------ DMA

void Dspp::nextBlock(int ch) {
  Fifo& f = fifo[ch];
  if (f.curCount > 0) return;
  const uint32_t bit = 1u << ch;
  if (f.nextValid) {
    f.curAddr = f.nextAddr;
    f.curCount = f.nextCount;
    chComplete &= ~bit;
    if (rollover) rollover(ch);
  } else {
    chEnable &= ~bit;  // acabou: o canal desliga
  }
}

// atende um canal (no máximo 4 amostras); devolve false quando não há nada a fazer
bool Dspp::serviceDma() {
  uint32_t mask = chEnable & ~chComplete;
  while (mask) {
    const int ch = 31 - __builtin_clz(mask);
    mask &= ~(1u << ch);
    if (ch >= CHANNELS) continue;
    Fifo& f = fifo[ch];
    if (f.curCount <= 0) continue;
    const int n = f.curCount > 4 ? 4 : f.curCount;
    if (ch >= 16) {  // DSP -> RAM
      if (f.depth < 4) continue;
      for (int i = 0; i < n; i++) {
        const uint16_t v = f.buf[f.dmaPtr];
        if (f.depth) { f.dmaPtr = uint8_t((f.dmaPtr + 1) & (FIFO_DEPTH - 1)); f.depth--; }
        if (writeMem) { writeMem(f.curAddr, uint8_t(v >> 8)); writeMem(f.curAddr + 1, uint8_t(v)); }
        f.curAddr += 2;
      }
    } else {  // RAM -> DSP
      if (f.depth > 4) continue;
      for (int i = 0; i < n; i++) {
        const uint16_t v = readMem ? uint16_t(readMem(f.curAddr) << 8 | readMem(f.curAddr + 1)) : 0;
        f.curAddr += 2;
        f.buf[f.dmaPtr] = v;
        f.dmaPtr = uint8_t((f.dmaPtr + 1) & (FIFO_DEPTH - 1));
        f.depth++;
      }
    }
    f.curCount -= n;
    nextBlock(ch);
    return true;
  }
  return false;
}

// contagem do DMA em amostras, com o mesmo teto do carregamento do estado (1 << 22 é maior que toda a memória):
// assim um estado gravado com contagem enorme continua igual depois de carregado
static int32_t dmaCount(uint32_t v) {
  const uint32_t n = (v + 4) >> 1;
  return int32_t(n > (1u << 22) ? (1u << 22) : n);
}

void Dspp::hostDmaWrite(int ch, int reg, uint32_t v) {
  if (ch < 0 || ch >= CHANNELS) return;
  Fifo& f = fifo[ch];
  switch (reg & 3) {
    case 0: f.curAddr = v; break;
    case 1: f.curCount = dmaCount(v); break;
    case 2: f.nextAddr = v; f.nextValid = true; break;
    default: f.nextCount = dmaCount(v); f.nextValid = true; break;
  }
}

uint32_t Dspp::hostDmaRead(int ch, int reg) const {
  if (ch < 0 || ch >= CHANNELS) return 0;
  const Fifo& f = fifo[ch];
  switch (reg & 3) {
    case 0: return f.curAddr;
    case 1: return uint32_t(f.curCount) * 2 - 4;
    case 2: return f.nextAddr;
    default: return uint32_t(f.nextCount) * 2 - 4;
  }
}

void Dspp::hostEnable(uint32_t set, uint32_t clr) {
  set &= 0x000F1FFF;
  clr &= 0x000F1FFF;
  chEnable = (chEnable | set) & ~clr;
  chComplete &= ~set;
}

void Dspp::hostFifoInit(uint32_t mask) {
  for (int ch = 0; ch < CHANNELS; ch++)
    if (mask & (1u << ch)) { Fifo& f = fifo[ch]; f.dmaPtr = f.dspPtr = f.depth = 0; f.prev = 0; }
}

// ------------------------------------------------------------------------------------------------ execução

void Dspp::runFrame() {
  ticks_ = 0;
  if (running) {
    pc = 0;
    sleeping = false;
    while (ticks_ < FRAME_TICKS && !sleeping) {
      serviceDma();
      step();
    }
  }
  // o resto da amostra: só o DMA anda (até não ter mais o que fazer)
  for (; ticks_ < FRAME_TICKS; ticks_++) {
    tclock--;
    if (!serviceDma()) { tclock = uint16_t(tclock - (FRAME_TICKS - ticks_ - 1)); break; }
  }
  outL = int16_t(outputs[0]);
  outR = int16_t(outputs[1]);
}

void Dspp::step() {
  const uint16_t op = fetch();
  if (op & 0x8000) control(op);
  else arith(op);
}

void Dspp::parseOperands(int n) {
  for (auto& o : ops_) o = {-1, -1};
  opIdx_ = 0;
  nOps_ = n;
  int idx = 0;
  while (idx < n && idx < 8) {
    const uint16_t w = fetch();
    int nregs = 0;
    if (w & 0x8000) {
      if ((w & 0xC000) == 0xC000) {  // imediato
        const uint16_t v = (w & 0x2000) ? uint16_t((w & 0x1FFF) << 3) : uint16_t(int16_t(w << 3) >> 3);
        ops_[idx++].value = v;
      } else if ((w & 0xE000) == 0x8000) {  // endereço
        uint16_t a = w & 0x3FF;
        if (w & 0x400) a = read(a) & 0x3FF;
        ops_[idx++].addr = a;
        if (w & 0x800) writeback_ = a;
      } else {  // um ou dois registradores
        nregs = (w & 0x400) ? 2 : 1;
      }
    } else {
      nregs = 3;
    }
    for (int i = 0; i < nregs && idx < 8; i++) {
      const int rd = (w >> ((nregs - i - 1) * 5)) & 0x1F;
      uint16_t a = reg(rd & 0xF) & 0x3FF;
      if (rd & 0x10) a = read(a) & 0x3FF;
      if (nregs == 2) {
        if ((i == 0 && (w & 0x1000)) || (i == 1 && (w & 0x800))) writeback_ = a;
      } else if (nregs == 1 && (w & 0x800)) {
        writeback_ = a;
      }
      ops_[idx++].addr = a;
    }
  }
}

uint16_t Dspp::nextOperand() {
  if (opIdx_ >= 8) return 0;
  const Operand& o = ops_[opIdx_++];
  if (o.value >= 0) return uint16_t(o.value);
  return o.addr >= 0 ? read(uint16_t(o.addr)) : 0;
}

void Dspp::writeNextOperand(uint16_t v) {
  if (opIdx_ >= 8) return;
  const Operand& o = ops_[opIdx_++];
  if (o.addr >= 0) write(uint16_t(o.addr), v);
}

void Dspp::arith(uint16_t op) {
  int nops = (op >> 13) & 3;
  const int muxa = (op >> 10) & 3, muxb = (op >> 8) & 3, alu = (op >> 4) & 0xF;
  int bs = op & 0xF;
  if (nops == 0 && (muxa == 1 || muxa == 2 || muxb == 1 || muxb == 2)) nops = 4;
  if (bs == 8) nops++;
  writeback_ = -1;
  parseOperands(nops);
  int32_t mul = 0;
  if (muxa == 3 || muxb == 3) {
    const int32_t a = int16_t(nextOperand());
    const int32_t b = int16_t((op & 0x1000) ? nextOperand() : uint16_t(acc >> 4));
    mul = (a * b) >> 11;
  }
  auto src = [&](int mux) -> int32_t {
    switch (mux) {
      case 0: return acc;
      case 3: return mul;
      default: return int32_t(uint32_t(nextOperand()) << 4);
    }
  };
  const uint32_t a = uint32_t(src(muxa)) & 0xFFFFF;
  const uint32_t b = uint32_t(src(muxb)) & 0xFFFFF;
  uint32_t r = 0;
  bool v = false, c = false;
  switch (alu) {
    case 0: r = a; break;
    case 1: r = 0u - b; break;
    case 2: r = a + b; v = ((a ^ b) & 0x80000) == 0 && ((a ^ r) & 0x80000); c = r & 0x100000; break;
    case 3: r = a + (fc ? 16u : 0u); c = r & 0x100000; break;
    case 4: r = a - b; v = ((a ^ b) & 0x80000) && ((a ^ r) & 0x80000); c = r & 0x100000; break;
    case 5: r = a - (fc ? 16u : 0u); c = r & 0x100000; break;
    case 6: r = a + 1; v = !(a & 0x80000) && (r & 0x80000); break;
    case 7: r = a - 1; v = (a & 0x80000) && !(r & 0x80000); break;
    case 8: r = a; break;
    case 9: r = ~a; break;
    case 10: r = a & b; break;
    case 11: r = ~(a & b); break;
    case 12: r = a | b; break;
    case 13: r = ~(a | b); break;
    case 14: r = a ^ b; break;
    default: r = ~(a ^ b); break;
  }
  fv = v;
  fc = c;
  fn = r & 0x80000;
  fz = (r & 0xFFFF0) == 0;
  fx = (r & 0xF) == 0;
  static const int SH[8] = {0, 1, 2, 3, 4, 5, 8, 16};
  if (bs == 8) bs = nextOperand() & 0xF;
  if (bs & 8) {
    const int s = SH[(8 - (bs & 7)) & 7];
    acc = alu < 8 ? (sext20(r) >> s) : int32_t((r & 0xFFFFF) >> s);
  } else if (SH[bs] == 16) {  // satura em 16 bits
    acc = fv ? (fn ? 0x7FFFF : -0x80000) : sext20(r);
  } else {
    acc = sext20(r << SH[bs]);
  }
  if (writeback_ >= 0) {
    write(uint16_t(writeback_), uint16_t(acc >> 4));
    writeback_ = -1;
  } else if (opIdx_ < nOps_) {
    writeNextOperand(uint16_t(acc >> 4));
  }
}

void Dspp::control(uint16_t op) {
  const int mode = (op >> 13) & 3;
  const uint16_t target = op & 0x3FF;
  if (mode == 0) {
    switch ((op >> 10) & 7) {
      case 0:
        switch ((op >> 7) & 7) {
          case 1: pc = uint16_t((acc >> 4) & (CODE_WORDS - 1)); break;  // BAC
          case 2: setRmap(rmap, uint16_t((op & 0x3F) << 2)); break;     // RBASE
          case 3: setRmap(uint8_t(op & 7), rbaseXor); break;            // RMAP
          case 4: if (sp) pc = stack[--sp]; break;                      // RTS
          case 7: pc = uint16_t((pc - 1) & (CODE_WORDS - 1)); sleeping = true; break;  // SLEEP
          default: break;                                               // NOP, OP_MASK
        }
        break;
      case 1: pc = target; break;                                       // JUMP
      case 2: if (sp < 4) stack[sp++] = pc; pc = target; break;         // JSR
      case 3: break;                                                    // BFM
      case 4: {                                                         // MOVEREG
        const int rd = op & 0x1F;
        uint16_t a = reg(rd & 0xF) & 0x3FF;
        if (rd & 0x10) a = read(a) & 0x3FF;
        parseOperands(1);
        write(a, nextOperand());
        break;
      }
      case 5: setRbase((op & 3) << 2, op & 0x3FC); break;              // RBASE
      case 6: parseOperands(1); write(target, nextOperand()); break;    // MOVED
      default: {                                                        // MOVEI
        parseOperands(1);
        const uint16_t a = read(target);
        write(a, nextOperand());
        break;
      }
    }
    return;
  }
  bool take;
  if (mode == 3) {
    const bool lt = fn != fv;
    switch ((op >> 10) & 7) {
      case 0: take = lt; break;          // BLT
      case 1: take = lt || fz; break;    // BLE
      case 2: take = !lt; break;         // BGE
      case 3: take = !lt && !fz; break;  // BGT
      case 4: take = fc && !fz; break;   // BHI
      case 5: take = !fc || fz; break;   // BLS
      case 6: take = fx; break;          // BXS
      default: take = !fx; break;        // BXC
    }
  } else {
    const bool sel = op & 0x1000;
    const bool f0 = sel ? fc : fn, f1 = sel ? fz : fv;
    const bool m0 = op & 0x800, m1 = op & 0x400;
    take = (f0 || !m0) && (f1 || !m1);
    if (mode == 2) take = !take;
  }
  if (take) pc = target;
}

void Dspp::sync(StateIO& io) {
  io.raw(code, sizeof code);
  io.raw(data, sizeof data);
  io.sync(running);
  io.sync(pc); io.sync(acc);
  io.sync(fz); io.sync(fn); io.sync(fc); io.sync(fv); io.sync(fx);
  io.sync(sleeping); io.sync(audlock);
  io.raw(stack, sizeof stack);
  io.sync(sp);
  io.raw(rbase, sizeof rbase);
  io.sync(rmap); io.sync(rbaseXor);
  io.sync(tclock); io.sync(tick); io.sync(semStatus); io.sync(semData);
  io.sync(noise);
  io.raw(outputs, sizeof outputs);
  io.sync(outL); io.sync(outR);
  for (auto& f : fifo) {
    io.sync(f.curAddr); io.sync(f.nextAddr); io.sync(f.curCount); io.sync(f.nextCount); io.sync(f.nextValid);
    io.raw(f.buf, sizeof f.buf);
    io.sync(f.dmaPtr); io.sync(f.dspPtr); io.sync(f.depth); io.sync(f.prev);
  }
  io.sync(chEnable); io.sync(chComplete);
  if (io.reading()) {
    pc &= CODE_WORDS - 1;
    acc = sext20(uint32_t(acc));
    if (sp > 4) sp = 0;
    for (auto& s : stack) s &= CODE_WORDS - 1;
    rmap &= 7;
    for (auto& r : rbase) r &= DATA_WORDS - 1;
    if (!noise) noise = 1;
    for (auto& f : fifo) {
      f.dmaPtr &= FIFO_DEPTH - 1;
      f.dspPtr &= FIFO_DEPTH - 1;
      if (f.depth > FIFO_DEPTH) f.depth = 0;
      if (f.curCount < 0) f.curCount = 0;
      if (f.nextCount < 0) f.nextCount = 0;
      if (f.curCount > (1 << 22)) f.curCount = 1 << 22;
      if (f.nextCount > (1 << 22)) f.nextCount = 1 << 22;
    }
    chEnable &= 0x000F1FFF;
    writeback_ = -1;
  }
}

}  // namespace indigo::tdo
