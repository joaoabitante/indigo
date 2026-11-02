#include "sm510.h"

#include <algorithm>
#include <cstring>

namespace indigo::gnw {

void Sm5xx::setRom(const std::vector<uint8_t>& prog, const std::vector<uint8_t>& melody) {
  rom_.assign(0x1000, 0);
  std::copy_n(prog.begin(), std::min<size_t>(prog.size(), 0x1000), rom_.begin());
  melody_ = melody;
  if (!melody_.empty()) melody_.resize(0x100, 0);
  // SM510: ROM de 2,7 KB nas faixas 0x000-0x2BF, 0x400-0x6BF, 0x800-0xABF e 0xC00-0xEBF (o resto lê 0)
  if (tipo_ == Cpu::SM510)
    for (int a = 0; a < 0x1000; a++)
      if ((a & 0x3FF) >= 0x2C0) rom_[a] = 0;
  prgMask_ = sm5a() ? 0x7FF : 0xFFF;
  stackLevels_ = sm5a() ? 1 : 2;
}

uint8_t Sm5xx::fetch(uint16_t a) const {
  a &= prgMask_;
  if (sm5a() && a >= 0x700) a = 0x700 | (a & 0x3F);  // 0x700-0x73F se repete até 0x7FF
  return rom_[a];
}

int Sm5xx::ramAddr() const {
  if (sm5a()) {
    // 5 bancos de 13 nibbles: banco 4 se repete nos bancos 5-7; posições D-F leem a C
    int b = bm_ & 7, l = bl_ & 0xF;
    if (b > 4) b = 4;
    if (l > 0xC) l = 0xC;
    return b << 4 | l;
  }
  return (bmask_ | bm_ << 4 | bl_) & 0x7F;
}

// PL é um LFSR de 6 bits: bit novo = (bit0 == bit1)
void Sm5xx::incPc() {
  const int feed = ((pc_ >> 1 ^ pc_) & 1) ? 0 : 0x20;
  pc_ = static_cast<uint16_t>(feed | (pc_ >> 1 & 0x1F) | (pc_ & ~0x3F));
}

void Sm5xx::push() {
  for (int i = stackLevels_ - 1; i >= 1; i--) stack_[i] = stack_[i - 1];
  stack_[0] = pc_;
}

void Sm5xx::pop() {
  pc_ = stack_[0] & prgMask_;
  for (int i = 0; i < stackLevels_ - 1; i++) stack_[i] = stack_[i + 1];
}

void Sm5xx::branch(uint8_t pu, uint8_t pm, uint8_t pl) {
  pc_ = static_cast<uint16_t>(((pu << 10) | (pm << 6 & 0x3C0) | (pl & 0x3F)) & prgMask_);
}

void Sm5xx::powerOn() {
  std::memset(ram_, 0, sizeof ram_);
  pc_ = prevPc_ = op_ = prevOp_ = 0;
  param_ = 0;
  stack_[0] = stack_[1] = 0;
  acc_ = bl_ = bm_ = bmask_ = c_ = 0;
  skip_ = halt_ = false;
  w_ = r_ = rOut_ = 0;
  div_ = 0;
  gamma_ = 0;
  l_ = x_ = y_ = bp_ = 0;
  bc_ = false;
  clkDiv_ = 2;
  icount_ = 0;
  melRd_ = melStep_ = melDutyCount_ = melDutyIndex_ = melAddr_ = 0;
  std::memset(ox_, 0, sizeof ox_);
  std::memset(o_, 0, sizeof o_);
  mx_ = cb_ = 0;
  rsub_ = false;
  reset();
}

void Sm5xx::reset() {
  skip_ = halt_ = false;
  bmask_ = 0;
  op_ = prevOp_ = 0;
  if (sm5a()) branch(0, 0xF, 0);
  else branch(3, 7, 0);
  prevPc_ = pc_;
  bp_ = 1;  // LCD ligado
  bc_ = false;
  y_ = 0;
  r_ = rOut_ = 0;
  io_->writeR(0);
  if (sm5a()) {
    push();
    div_ &= 0x3F;
    gamma_ = 1;
    cb_ = 0;
    rsub_ = false;
    r_ = 0xF;
  } else if (tipo_ == Cpu::SM511 || tipo_ == Cpu::SM512) {
    melRd_ &= ~1;
    clkDiv_ = 4;  // 8 kHz
  }
}

void Sm5xx::tickDivider() {
  div_ = (div_ + 1) & 0x7FFF;
  if (div_ == 0) gamma_ |= 1;  // sinal de 1 s
  clockMelody();
}

void Sm5xx::clockMelody() {
  uint8_t out = 0;
  if (sm5a()) {
    // R1 do divisor ou direto; R2-R4 saídas comuns (invertidas)
    const uint8_t mask = rMask_ < 0 ? 1 : (div_ >> rMask_ & 1);
    out = static_cast<uint8_t>(((mask & ~r_) | (~r_ & 0xE)) & 0xF);
  } else if (tipo_ == Cpu::SM510) {
    if (rMask_ < 0) out = r_ & 3;
    else {
      out = div_ >> rMask_ & 1;  // R2 em fase oposta
      out |= (out << 1 ^ 2);
      out &= r_;
    }
  } else {
    if (melody_.empty()) return;
    // ciclos de cada tom (tabela da folha de dados do SM511/SM512); 0 = pausa, 1 = fim
    static const uint8_t tons[4 * 16] = {
        0, 0, 7, 8, 8, 9, 9, 10, 11, 11, 12, 13, 14, 14, 0, 0,
        0, 0, 8, 8, 9, 9, 10, 11, 11, 12, 13, 13, 14, 15, 0, 0,
        0, 0, 8, 8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 0, 0,
        0, 0, 8, 9, 9, 10, 10, 11, 11, 12, 13, 14, 14, 15, 0, 0,
    };
    const uint8_t cmd = melody_[melAddr_] & 0x3F;
    if ((cmd & 0xF) >= 2 && (cmd & 0xF) <= 13) {
      out = melDutyIndex_ & melRd_ & 1;
      melDutyCount_++;
      const int idx = melDutyIndex_ << 4 | (cmd & 0xF);
      const int shift = ~cmd >> 4 & 1;  // oitava
      if (melDutyCount_ >= (tons[idx] << shift)) {
        melDutyCount_ = 0;
        melDutyIndex_ = (melDutyIndex_ + 1) & 3;
      }
    } else if ((cmd & 0xF) == 1) {
      melRd_ |= 2;  // fim da melodia
    }
    if ((div_ & 0x7F) == 0) {  // base de tempo nos bits F7/F8 do divisor
      const uint8_t m = (cmd & 0x20) ? 0x1F : 0x0F;
      melStep_ = (melStep_ + 1) & m;
      if (melStep_ == 0) melAddr_++;
    }
  }
  if (out != rOut_) {
    rOut_ = out;
    io_->writeR(out);
  }
}

void Sm5xx::lcdOutputs(uint16_t out[32]) const {
  std::memset(out, 0, 32 * sizeof(uint16_t));
  if (sm5a()) {
    for (int h = 0; h < 2; h++)
      for (int o = 0; o < 9; o++) out[o << 1 | h] = (bp_ & 1) ? (h ? ox_[o] : o_[o]) : 0;
    return;
  }
  const bool off = bc_ || !(bp_ & 1);
  for (int h = 0; h < 4; h++) {
    if (!off) {
      uint16_t a = 0, b = 0, c = 0;
      for (int i = 0; i < 16; i++) {
        a |= (ram_[0x60 + i] >> h & 1) << i;
        b |= (ram_[0x70 + i] >> h & 1) << i;
        if (tipo_ == Cpu::SM512) c |= (ram_[0x50 + i] >> h & 1) << i;
      }
      out[0 + h] = a;
      out[4 + h] = b;
      out[12 + h] = c;
      const uint8_t blink = (div_ & 0x4000) ? y_ : 0;
      out[8 + h] = static_cast<uint16_t>(((l_ & ~blink) >> h & 1) | ((x_ * 2) >> h & 2));
    }
  }
}

bool Sm5xx::twoBytes() const {
  const uint8_t o = static_cast<uint8_t>(op_);
  switch (tipo_) {
    case Cpu::SM5A: return o == 0x5E || o == 0x5F;
    case Cpu::SM510: return o == 0x5F || (o & 0xF0) == 0x70;
    default: return (o >= 0x5F && o <= 0x61) || (o & 0xF0) == 0x70 || (o & 0xFC) == 0x68;
  }
}

void Sm5xx::cpuStep() {
  int ciclos = 0;
  if (halt_) {
    if (io_->readK() != 0 || gamma_) {
      // acorda (pilha e B ficam como estavam)
      ciclos++;
      halt_ = false;
      if (sm5a()) { cb_ = 0; branch(0, 0, 0); }
      else branch(1, 0, 0);
    } else {
      icount_ = 1;  // parada: confere de novo no próximo ciclo
      return;
    }
  }
  ciclos++;
  prevOp_ = op_;
  prevPc_ = pc_;
  op_ = fetch(pc_);
  incPc();
  if (twoBytes()) {
    ciclos++;
    param_ = fetch(pc_);
    incPc();
  }
  if (skip_) {
    skip_ = false;
    op_ = 0;  // vira NOP
  } else {
    if (sm5a()) exec5a();
    else if (tipo_ == Cpu::SM510) exec510();
    else exec511();
    if ((op_ & 0xC0) == 0xC0 && op_ < 0x100 && !sm5a()) ciclos++;  // TM lê o ponteiro
  }
  icount_ += ciclos * clkDiv_;
}

void Sm5xx::opIncb() {
  bl_ = (bl_ + 1) & 0xF;
  skip_ = sm5a() ? (bl_ == 8) : (bl_ == 0);
}

void Sm5xx::opRtn0() {
  pop();
  if (sm5a()) rsub_ = false;
}

// ------------------------------------------------------------------ SM510
void Sm5xx::exec510() {
  const uint8_t o = static_cast<uint8_t>(op_);
  switch (o & 0xF0) {
    case 0x20: opLax(); break;
    case 0x30:  // ADX: soma imediato, pula no vai-um (exceto x = 10)
      acc_ += o & 0xF;
      skip_ = ((o & 0xF) != 10 && (acc_ & 0x10));
      acc_ &= 0xF;
      break;
    case 0x40: bm_ = (bm_ & 4) | (o & 3); bl_ = (o >> 2 & 3) | ((o & 0xC) ? 0xC : 0); break;  // LB
    case 0x80: case 0x90: case 0xA0: case 0xB0: pc_ = (pc_ & ~0x3F) | (o & 0x3F); break;  // T
    case 0xC0: case 0xD0: case 0xE0: case 0xF0: {  // TM: chamada indireta pela tabela da página 0
      push();
      const uint8_t idx = fetch(o & 0x3F);
      branch(idx >> 6 & 3, 4, idx & 0x3F);
      break;
    }
    default:
      switch (o & 0xFC) {
        case 0x04: ramW(ramR() & ~(1 << (o & 3))); goto fim;  // RM
        case 0x0C: ramW(ramR() | (1 << (o & 3))); goto fim;   // SM
        case 0x10: opExc(); goto fim;
        case 0x14: opExc(); opIncb(); goto fim;  // EXCI
        case 0x18: acc_ = ramR(); bm_ ^= (o & 3); goto fim;  // LDA
        case 0x1C: opExc(); opDecb(); goto fim;  // EXCD
        case 0x54: skip_ = (ramR() & (1 << (o & 3))) != 0; goto fim;  // TMI
        case 0x70: case 0x74: case 0x78: branch(param_ >> 6 & 3, o & 0xF, param_ & 0x3F); goto fim;  // TL
        case 0x7C: push(); branch(param_ >> 6 & 3, o & 3, param_ & 0x3F); goto fim;  // TML
        default: break;
      }
      switch (o) {
        case 0x00: break;  // SKIP
        case 0x01: bp_ = acc_; break;  // ATBP
        case 0x02: break;  // SBM (bmask abaixo)
        case 0x03: pc_ = (prevPc_ & ~0xF) | acc_; break;  // ATPL
        case 0x08: acc_ = (acc_ + ramR()) & 0xF; break;  // ADD
        case 0x09: acc_ += ramR() + c_; c_ = acc_ >> 4 & 1; skip_ = c_; acc_ &= 0xF; break;  // ADD11
        case 0x0A: acc_ ^= 0xF; break;  // COMA
        case 0x0B: std::swap(acc_, bl_); break;  // EXBLA
        case 0x51: skip_ = io_->readB() != 0; break;  // TB
        case 0x52: skip_ = !c_; break;  // TC
        case 0x53: skip_ = acc_ == ramR(); break;  // TAM
        case 0x58: skip_ = !gamma_; gamma_ = 0; break;  // TIS
        case 0x59: l_ = acc_; break;  // ATL
        case 0x5A: skip_ = !acc_; break;  // TA0
        case 0x5B: skip_ = acc_ == bl_; break;  // TABL
        case 0x5D: halt_ = true; break;  // CEND
        case 0x5E: skip_ = io_->readBA() != 0; break;  // TAL
        case 0x5F: bl_ = param_ & 0xF; bm_ = (param_ & 0x7F) >> 4; break;  // LBL
        case 0x60: y_ = acc_; break;  // ATFC
        case 0x61: opAtr(); break;
        case 0x62: w_ = static_cast<uint8_t>(w_ << 1); io_->writeS(w_); break;  // WR
        case 0x63: w_ = static_cast<uint8_t>(w_ << 1 | 1); io_->writeS(w_); break;  // WS
        case 0x64: opIncb(); break;
        case 0x65: div_ = 0; break;  // IDIV
        case 0x66: c_ = 0; break;  // RC
        case 0x67: c_ = 1; break;  // SC
        case 0x68: skip_ = (div_ & 0x4000) != 0; break;  // TF1
        case 0x69: skip_ = (div_ & 0x0800) != 0; break;  // TF4
        case 0x6A: acc_ = io_->readK() & 0xF; break;  // KTA
        case 0x6B: { const uint8_t c = acc_ & 1; acc_ = acc_ >> 1 | c_ << 3; c_ = c; break; }  // ROT
        case 0x6C: opDecb(); break;
        case 0x6D: bc_ = c_ != 0; break;  // BDC
        case 0x6E: opRtn0(); break;
        case 0x6F: opRtn0(); skip_ = true; break;  // RTN1
        default: break;  // instrução desconhecida: nada
      }
  }
fim:
  bmask_ = (op_ == 0x02) ? 0x40 : 0;
}

// ------------------------------------------------------------------ SM511 / SM512
void Sm5xx::exec511() {
  const uint8_t o = static_cast<uint8_t>(op_);
  switch (o & 0xF0) {
    case 0x20: opLax(); break;
    case 0x30:
      acc_ += o & 0xF;
      skip_ = ((o & 0xF) != 10 && (acc_ & 0x10));
      acc_ &= 0xF;
      break;
    case 0x40: bm_ = (bm_ & 4) | (o & 3); bl_ = (o >> 2 & 3) | ((o & 0xC) ? 0xC : 0); break;
    case 0x70: branch(param_ >> 6 & 3, o & 0xF, param_ & 0x3F); break;  // TL
    case 0x80: case 0x90: case 0xA0: case 0xB0: pc_ = (pc_ & ~0x3F) | (o & 0x3F); break;
    case 0xC0: case 0xD0: case 0xE0: case 0xF0: {
      push();
      const uint8_t idx = fetch(o & 0x3F);
      branch(idx >> 6 & 3, 4, idx & 0x3F);
      break;
    }
    default:
      switch (o & 0xFC) {
        case 0x04: ramW(ramR() & ~(1 << (o & 3))); goto fim;
        case 0x0C: ramW(ramR() | (1 << (o & 3))); goto fim;
        case 0x10: opExc(); goto fim;
        case 0x14: opExc(); opIncb(); goto fim;
        case 0x18: acc_ = ramR(); bm_ ^= (o & 3); goto fim;
        case 0x1C: opExc(); opDecb(); goto fim;
        case 0x54: skip_ = (ramR() & (1 << (o & 3))) != 0; goto fim;
        case 0x68: push(); branch(param_ >> 6 & 3, o & 3, param_ & 0x3F); goto fim;  // TML
        default: break;
      }
      switch (o) {
        case 0x00: { const uint8_t c = acc_ & 1; acc_ = acc_ >> 1 | c_ << 3; c_ = c; break; }  // ROT
        case 0x01: acc_ = div_ >> 11 & 0xF; break;  // DTA
        case 0x02: break;  // SBM
        case 0x03: pc_ = (prevPc_ & ~0xF) | acc_; break;
        case 0x08: acc_ = (acc_ + ramR()) & 0xF; break;
        case 0x09: acc_ += ramR() + c_; c_ = acc_ >> 4 & 1; skip_ = c_; acc_ &= 0xF; break;
        case 0x0A: acc_ ^= 0xF; break;
        case 0x0B: std::swap(acc_, bl_); break;
        case 0x50: acc_ = io_->readK() & 0xF; break;  // KTA
        case 0x51: skip_ = io_->readB() != 0; break;
        case 0x52: skip_ = !c_; break;
        case 0x53: skip_ = acc_ == ramR(); break;
        case 0x58: skip_ = !gamma_; gamma_ = 0; break;
        case 0x59: l_ = acc_; break;
        case 0x5A: skip_ = !acc_; break;
        case 0x5B: skip_ = acc_ == bl_; break;
        case 0x5C: x_ = acc_; break;  // ATX
        case 0x5D: halt_ = true; break;
        case 0x5E: skip_ = io_->readBA() != 0; break;
        case 0x5F: bl_ = param_ & 0xF; bm_ = (param_ & 0x7F) >> 4; break;
        case 0x61: melAddr_ = param_; melStep_ = 0; break;  // PRE
        case 0x62: w_ = static_cast<uint8_t>(w_ << 1); break;  // WR (a saída S muda só no PTW)
        case 0x63: w_ = static_cast<uint8_t>(w_ << 1 | 1); break;  // WS
        case 0x64: opIncb(); break;
        case 0x65: div_ = 0; break;
        case 0x66: c_ = 0; break;
        case 0x67: c_ = 1; break;
        case 0x6C: opDecb(); break;
        case 0x6D: io_->writeS(w_); break;  // PTW
        case 0x6E: opRtn0(); break;
        case 0x6F: opRtn0(); skip_ = true; break;
        case 0x60:  // instruções de 2 bytes com prefixo 0x60
          op_ = static_cast<uint16_t>(op_ << 8 | param_);
          switch (param_) {
            case 0x30: melRd_ &= ~1; break;  // RME
            case 0x31: melRd_ |= 1; break;   // SME
            case 0x32: skip_ = (melRd_ & 2) != 0; melRd_ &= ~2; break;  // TMEL
            case 0x33: y_ = acc_; break;  // ATFC
            case 0x34: bc_ = c_ != 0; break;  // BDC
            case 0x35: bp_ = acc_; break;  // ATBP
            case 0x36: clkDiv_ = 2; break;  // 16 kHz
            case 0x37: clkDiv_ = 4; break;  // 8 kHz
            default: break;
          }
          break;
        default: break;
      }
  }
fim:
  bmask_ = (op_ == 0x02) ? 0x40 : 0;
}

// ------------------------------------------------------------------ SM5A
uint8_t Sm5xx::digit() const {
  // PLA de dígitos (linha escolhida pelo bit 3 de BP, o flag CN)
  static const uint8_t lut[0x20] = {0xe, 0x0, 0xc, 0x8, 0x2, 0xa, 0xe, 0x2, 0xe, 0xa, 0x0, 0x0, 0x2, 0xa, 0x2, 0x2,
                                    0xb, 0x9, 0x7, 0xf, 0xd, 0xe, 0xe, 0xb, 0xf, 0xf, 0x4, 0x0, 0xd, 0xe, 0x4, 0x0};
  const uint8_t sel = bp_ >> 3 & 1;
  return static_cast<uint8_t>(lut[sel << 4 | acc_] | (~sel & mx_ & 1));
}

void Sm5xx::exec5a() {
  const uint8_t o = static_cast<uint8_t>(op_);
  auto su = [&]() { return static_cast<uint8_t>(stack_[0] >> 6 & 0xF); };
  auto shiftW = [&]() { for (int i = 0; i < 8; i++) ox_[i] = ox_[i + 1]; };
  switch (o & 0xF0) {
    case 0x20: opLax(); return;
    case 0x30:
      acc_ += o & 0xF;
      skip_ = ((o & 0xF) != 10 && (acc_ & 0x10));
      acc_ &= 0xF;
      return;
    case 0x40: bm_ = o & 3; bl_ = (o >> 2 & 3) | ((o & 0xC) ? 8 : 0); return;  // LB
    case 0x70: stack_[0] = static_cast<uint16_t>((stack_[0] & ~0x3C0) | (o << 6 & 0x3C0)); return;  // SSR
    case 0x80: case 0x90: case 0xA0: case 0xB0:  // TR: salto curto ou longo
      pc_ = (pc_ & ~0x3F) | (o & 0x3F);
      if (!rsub_) branch(cb_, su(), pc_ & 0x3F);
      return;
    case 0xC0: case 0xD0: case 0xE0: case 0xF0:  // TRS: chamada
      if (!rsub_) {
        rsub_ = true;
        const uint8_t s = su();
        push();
        branch(1, 0, o & 0x3F);
        if ((prevOp_ & 0xF0) == 0x70) branch(cb_, s, pc_ & 0x3F);  // depois de SSR (flag E)
      } else {
        pc_ = (pc_ & ~0xFF) | (o << 2 & 0xC0) | (o & 0xF);
      }
      return;
    default: break;
  }
  switch (o & 0xFC) {
    case 0x04: ramW(ramR() & ~(1 << (o & 3))); return;
    case 0x0C: ramW(ramR() | (1 << (o & 3))); return;
    case 0x10: opExc(); return;
    case 0x14: opExc(); opIncb(); return;
    case 0x18: acc_ = ramR(); bm_ ^= (o & 3); return;
    case 0x1C: opExc(); opDecb(); return;
    case 0x54: skip_ = (ramR() & (1 << (o & 3))) != 0; return;
    default: break;
  }
  switch (o) {
    case 0x00: break;  // SKIP
    case 0x01: opAtr(); break;
    case 0x02: bm_ |= 4; break;  // SBM
    case 0x03: bp_ = acc_; break;  // ATBP
    case 0x08: acc_ = (acc_ + ramR()) & 0xF; break;
    case 0x09: acc_ += ramR() + c_; c_ = acc_ >> 4 & 1; skip_ = c_; acc_ &= 0xF; break;
    case 0x0A: acc_ ^= 0xF; break;
    case 0x0B: std::swap(acc_, bl_); break;
    case 0x50: skip_ = io_->readBA() != 0; break;  // TA
    case 0x51: skip_ = io_->readB() != 0; break;
    case 0x52: skip_ = !c_; break;
    case 0x53: skip_ = acc_ == ramR(); break;
    case 0x58: skip_ = !gamma_; gamma_ = 0; break;  // TG
    case 0x59: o_[8] = ox_[8]; o_[7] = ox_[7]; break;  // PTW
    case 0x5A: skip_ = !acc_; break;
    case 0x5B: skip_ = acc_ == bl_; break;
    case 0x5C: std::memcpy(o_, ox_, sizeof o_); break;  // TW
    case 0x5D: shiftW(); ox_[8] = digit(); break;  // DTW
    case 0x5F: bl_ = param_ & 0xF; bm_ = (param_ & 0x7F) >> 4; break;
    case 0x60: bp_ ^= 8; break;  // COMCN
    case 0x61: ox_[7] = ox_[8]; ox_[8] = digit(); break;  // PDTW
    case 0x62: shiftW(); ox_[8] = acc_ & 7; break;  // WR
    case 0x63: shiftW(); ox_[8] = acc_ | 8; break;  // WS
    case 0x64: opIncb(); break;
    case 0x65: div_ &= 0x3F; break;  // IDIV (só os 9 bits de cima)
    case 0x66: c_ = 0; break;
    case 0x67: c_ = 1; break;
    case 0x68: mx_ = 0; acc_ = 0; break;  // RMF
    case 0x69: mx_ = 1; break;  // SMF
    case 0x6A: acc_ = io_->readK() & 0xF; break;
    case 0x6B: bm_ &= ~4; break;  // RBM
    case 0x6C: opDecb(); break;
    case 0x6D: cb_ ^= 1; break;  // COMCB
    case 0x6E: opRtn0(); break;
    case 0x6F: opRtn0(); skip_ = true; break;
    case 0x5E:
      op_ = static_cast<uint16_t>(op_ << 8 | param_);
      if (param_ == 0x00) halt_ = true;  // CEND
      else if (param_ == 0x04) acc_ = div_ >> 11 & 0xF;  // DTA
      break;
    default: break;
  }
}

std::vector<int> Sm5xx::registros() const {
  std::vector<int> v = {pc_, prevPc_, op_, prevOp_, param_, stack_[0], stack_[1], acc_, bl_, bm_, bmask_, c_, skip_, halt_,
                        w_, r_, rOut_, div_, gamma_, l_, x_, y_, bp_, bc_, clkDiv_, melRd_, melStep_, melDutyCount_,
                        melDutyIndex_, melAddr_};
  for (int i = 0; i < 9; i++) v.push_back(ox_[i]);
  for (int i = 0; i < 9; i++) v.push_back(o_[i]);
  v.push_back(mx_);
  v.push_back(cb_);
  v.push_back(rsub_);
  return v;
}

void Sm5xx::sync(StateIO& s) {
  s.raw(ram_, sizeof ram_);
  s.sync(pc_); s.sync(prevPc_); s.sync(op_); s.sync(prevOp_); s.sync(param_);
  s.sync(stack_[0]); s.sync(stack_[1]);
  s.sync(acc_); s.sync(bl_); s.sync(bm_); s.sync(bmask_); s.sync(c_);
  s.sync(skip_); s.sync(halt_);
  s.sync(w_); s.sync(r_); s.sync(rOut_);
  s.sync(div_); s.sync(gamma_);
  s.sync(l_); s.sync(x_); s.sync(y_); s.sync(bp_); s.sync(bc_);
  s.sync(clkDiv_); s.sync(icount_);
  s.sync(melRd_); s.sync(melStep_); s.sync(melDutyCount_); s.sync(melDutyIndex_); s.sync(melAddr_);
  s.raw(ox_, sizeof ox_); s.raw(o_, sizeof o_);
  s.sync(mx_); s.sync(cb_); s.sync(rsub_);
  if (s.reading()) {
    // estado danificado não pode sair da memória nem travar
    pc_ &= prgMask_; prevPc_ &= prgMask_;
    stack_[0] &= prgMask_; stack_[1] &= prgMask_;
    for (auto& v : ram_) v &= 0xF;
    acc_ &= 0xF; bl_ &= 0xF; bm_ &= 7; bmask_ &= 0x40; c_ &= 1;
    div_ &= 0x7FFF;
    if (clkDiv_ != 2 && clkDiv_ != 4) clkDiv_ = 2;
    clampTo<int32_t>(icount_, -64, 64);
    for (auto& v : ox_) v &= 0xF;
    for (auto& v : o_) v &= 0xF;
    cb_ &= 1;
    melDutyIndex_ &= 3;
  }
}

}  // namespace indigo::gnw
