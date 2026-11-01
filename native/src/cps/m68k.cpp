#include "m68k.h"

#include <utility>

namespace indigo::cps {

namespace {
inline uint32_t MASK(int sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
inline uint32_t MSB(int sz) { return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u; }
inline int SZ(int bits) { return bits == 0 ? 1 : bits == 1 ? 2 : 4; }  // bits 6-7 do opcode
inline uint32_t sext(uint32_t v, int sz) {
  return sz == 1 ? static_cast<uint32_t>(static_cast<int8_t>(v)) : sz == 2 ? static_cast<uint32_t>(static_cast<int16_t>(v)) : v;
}
inline int popcount16(uint32_t v) { int n = 0; v &= 0xFFFF; while (v) { n += v & 1; v >>= 1; } return n; }
}  // namespace

M68k::M68k(M68kBus& b) : bus(b) {}

uint16_t M68k::getSR() const {
  return static_cast<uint16_t>((tMode << 15) | (sMode << 13) | (mask << 8) | (fx << 4) | (fn << 3) | (fz << 2) | (fv << 1) | fc);
}

void M68k::setSupervisor(bool s) {
  if (static_cast<int>(s) == sMode) return;
  const uint32_t t = a[7]; a[7] = otherSp; otherSp = t;
  sMode = s ? 1 : 0;
}

void M68k::setSR(uint16_t v) {
  tMode = (v >> 15) & 1;
  mask = (v >> 8) & 7;
  fx = (v >> 4) & 1; fn = (v >> 3) & 1; fz = (v >> 2) & 1; fv = (v >> 1) & 1; fc = v & 1;
  setSupervisor((v >> 13) & 1);
}

void M68k::reset() {
  stopped = false; halted = false; nmiPrev_ = false;
  tMode = 0; mask = 7;
  setSupervisor(true);
  cycles = 0;
  try {
    progData_ = true;  // os vetores de reset são lidos no espaço de programa (FC = programa do supervisor)
    a[7] = rd(0, 4);
    const uint32_t start = rd(4, 4);
    progData_ = false;
    jump(start);
  } catch (const AddressError&) {
    halted = true;
  }
}

// ------------------------------------------------------------ barramento
uint32_t M68k::rd(uint32_t ad, int sz) {
  if (progData_) {
    if (sz == 1) { cycles += 4; const uint16_t w = bus.fetch16(ad & 0xFFFFFE); return (ad & 1) ? (w & 0xFF) : (w >> 8); }
    if (ad & 1) throw AddressError{ad, true, false};
    cycles += 4;
    const uint32_t h = bus.fetch16(ad & 0xFFFFFF);
    if (sz == 2) return h;
    cycles += 4;
    return (h << 16) | bus.fetch16((ad + 2) & 0xFFFFFF);
  }
  if (sz == 1) { cycles += 4; return bus.read8(ad & 0xFFFFFF); }
  if (ad & 1) throw AddressError{ad, true, false};
  cycles += 4;
  const uint32_t h = bus.read16(ad & 0xFFFFFF);
  if (sz == 2) return h;
  cycles += 4;
  return (h << 16) | bus.read16((ad + 2) & 0xFFFFFF);
}

void M68k::wr(uint32_t ad, int sz, uint32_t v) {
  if (sz == 1) { cycles += 4; bus.write8(ad & 0xFFFFFF, static_cast<uint8_t>(v)); return; }
  if (ad & 1) throw AddressError{ad, false, false};
  if (sz == 2) { cycles += 4; bus.write16(ad & 0xFFFFFF, static_cast<uint16_t>(v)); return; }
  cycles += 4; bus.write16(ad & 0xFFFFFF, static_cast<uint16_t>(v >> 16));
  cycles += 4; bus.write16((ad + 2) & 0xFFFFFF, static_cast<uint16_t>(v));
}

uint16_t M68k::rdProg(uint32_t ad) {
  if (ad & 1) throw AddressError{ad, true, true};
  cycles += 4;
  return bus.fetch16(ad & 0xFFFFFF);
}

uint16_t M68k::ext() {
  const uint16_t v = irc;
  pc += 2;
  irc = rdProg(pc + 2);
  return v;
}

void M68k::prefetch() {
  ir = irc;
  pc += 2;
  irc = rdProg(pc + 2);
}

void M68k::jump(uint32_t to) {
  if (to & 1) { faultPc_ = to; throw AddressError{to, true, true}; }
  pc = to;
  ir = rdProg(to);
  irc = rdProg(to + 2);
}

// ------------------------------------------------------------ modos de endereçamento
uint32_t M68k::indexed(uint32_t base, uint16_t e) const {
  const int r = (e >> 12) & 7;
  uint32_t x = (e & 0x8000) ? a[r] : d[r];
  if (!(e & 0x800)) x = static_cast<uint32_t>(static_cast<int16_t>(x));
  return base + x + static_cast<uint32_t>(static_cast<int8_t>(e & 0xFF));
}

uint32_t M68k::eaAddr(int mode, int reg, int sz, bool moveDst) {
  progData_ = mode == 7 && (reg == 2 || reg == 3);
  switch (mode) {
    case 2: return a[reg];
    case 3: {
      const uint32_t ad = a[reg];
      // destino do MOVE com erro de endereço: o registrador não chega a ser incrementado
      if (!moveDst || sz == 1 || !(ad & 1)) a[reg] += (reg == 7 && sz == 1) ? 2 : sz;
      return ad;
    }
    case 4: {
      if (!moveDst) idle(2);
      // destino longo do MOVE em endereço ímpar: o chip decrementa 2 e falha no acesso à palavra de baixo
      a[reg] -= (reg == 7 && sz == 1) ? 2 : (moveDst && sz == 4 && (a[reg] & 1)) ? 2 : sz;
      return a[reg];
    }
    case 5: { const uint32_t base = a[reg]; return base + static_cast<uint32_t>(static_cast<int16_t>(ext())); }
    case 6: { idle(2); const uint32_t base = a[reg]; return indexed(base, ext()); }
    default:
      switch (reg) {
        case 0: return static_cast<uint32_t>(static_cast<int16_t>(ext()));
        case 1: return ext32();
        case 2: { const uint32_t base = pc + 2; return base + static_cast<uint32_t>(static_cast<int16_t>(ext())); }
        case 3: { idle(2); const uint32_t base = pc + 2; return indexed(base, ext()); }
        default: return 0;
      }
  }
}

// endereço de JMP/JSR: a última palavra de extensão vem de IRC sem nova busca (o destino é buscado logo depois)
uint32_t M68k::ctrlAddr(int mode, int reg, int& n) {
  switch (mode) {
    case 2: n = 0; return a[reg];
    case 5: n = 1; idle(2); return a[reg] + static_cast<uint32_t>(static_cast<int16_t>(irc));
    case 6: n = 1; idle(6); return indexed(a[reg], irc);
    default:
      switch (reg) {
        case 0: n = 1; idle(2); return static_cast<uint32_t>(static_cast<int16_t>(irc));
        case 1: { n = 2; const uint32_t h = ext(); return (h << 16) | irc; }
        case 2: n = 1; idle(2); return pc + 2 + static_cast<uint32_t>(static_cast<int16_t>(irc));
        case 3: n = 1; idle(6); return indexed(pc + 2, irc);
        default: n = 0; return 0;
      }
  }
}

uint32_t M68k::readEA(int mode, int reg, int sz) {
  switch (mode) {
    case 0: return d[reg] & MASK(sz);
    case 1: return a[reg] & MASK(sz);
    case 7:
      if (reg == 4) {
        if (sz == 4) return ext32();
        return ext() & MASK(sz);
      }
      [[fallthrough]];
    default: return rd(eaAddr(mode, reg, sz), sz);
  }
}

void M68k::writeReg(int reg, int sz, uint32_t v) {
  const uint32_t m = MASK(sz);
  d[reg] = (d[reg] & ~m) | (v & m);
}

// ------------------------------------------------------------ exceções
void M68k::exception(int vec, uint32_t pushPc, int idleFirst) {
  progData_ = false;
  const uint16_t sr = getSR();
  setSupervisor(true);
  tMode = 0;
  traceNow_ = 0;
  idle(idleFirst);
  push32(pushPc);
  push16(sr);
  const uint32_t to = rd(static_cast<uint32_t>(vec) * 4, 4);
  if (to & 1) { faultPc_ = to; throw AddressError{to, true, true}; }
  pc = to;
  ir = rdProg(to);
  idle(2);
  irc = rdProg(to + 2);
}

void M68k::addressError(const AddressError& e) {
  progData_ = false;
  const int fcode = (sMode ? 4 : 0) | (e.program ? 2 : 1);
  const uint16_t sr = getSR();
  uint32_t pushPc = pc;
  if (e.program && e.addr == faultPc_) pushPc = faultPc_ - 4;
  const uint16_t status = static_cast<uint16_t>((opcode_ & 0xFFE0) | (e.read ? 0x10 : 0) | (e.program ? 0x08 : 0) | fcode);
  setSupervisor(true);
  tMode = 0;
  traceNow_ = 0;
  idle(4);
  a[7] -= 14;
  const uint32_t sp = a[7];
  if (sp & 1) { halted = true; return; }
  wr(sp + 12, 2, pushPc & 0xFFFF);
  wr(sp + 8, 2, sr);
  wr(sp + 10, 2, pushPc >> 16);
  wr(sp + 6, 2, opcode_);
  wr(sp + 4, 2, e.addr & 0xFFFF);
  wr(sp + 0, 2, status);
  wr(sp + 2, 2, e.addr >> 16);
  const uint32_t to = rd(12, 4);
  if (to & 1) { halted = true; return; }
  pc = to;
  ir = rdProg(to);
  idle(2);
  irc = rdProg(to + 2);
}

bool M68k::privileged() {
  if (sMode) return true;
  exception(8, pc, 4);
  return false;
}

void M68k::interrupt(int level) {
  const uint16_t sr = getSR();
  setSupervisor(true);
  tMode = 0;
  mask = level;
  idle(6);
  push32(pc);
  push16(sr);
  idle(8);  // ciclo de reconhecimento (autovetor, sincronizado com o relógio E)
  const int vec = bus.intAck(level);
  const uint32_t to = rd(static_cast<uint32_t>(vec & 0xFF) * 4, 4);
  if (to & 1) { faultPc_ = to; throw AddressError{to, true, true}; }
  pc = to;
  ir = rdProg(to);
  idle(2);
  irc = rdProg(to + 2);
}

int M68k::step() {
  cycles = 0;
  progData_ = false;
  if (halted) return 4;
  const bool nmi = ipl == 7 && !nmiPrev_;
  nmiPrev_ = ipl == 7;
  try {
    if (nmi || (ipl > mask && ipl < 7)) {
      stopped = false;
      interrupt(ipl);
      return cycles;
    }
    if (stopped) return 4;
    traceNow_ = tMode;
    opcode_ = ir;
    execute();
    if (traceNow_) exception(9, pc, 4);
  } catch (const AddressError& e) {
    try {
      addressError(e);
    } catch (const AddressError&) {
      halted = true;
    }
  }
  return cycles;
}

bool M68k::cond(int cc) const {
  switch (cc) {
    case 0: return true;
    case 1: return false;
    case 2: return !fc && !fz;
    case 3: return fc || fz;
    case 4: return !fc;
    case 5: return fc;
    case 6: return !fz;
    case 7: return fz;
    case 8: return !fv;
    case 9: return fv;
    case 10: return !fn;
    case 11: return fn;
    case 12: return fn == fv;
    case 13: return fn != fv;
    case 14: return !fz && fn == fv;
    default: return fz || fn != fv;
  }
}

void M68k::setNZ(uint32_t r, int sz) {
  r &= MASK(sz);
  fn = (r & MSB(sz)) ? 1 : 0;
  fz = r == 0;
}

uint32_t M68k::add(uint32_t s, uint32_t dd, int sz, int x, bool extMode) {
  const uint32_t m = MASK(sz);
  const uint64_t r = static_cast<uint64_t>(s & m) + (dd & m) + static_cast<uint64_t>(x);
  const uint32_t res = static_cast<uint32_t>(r) & m;
  fc = fx = r > m ? 1 : 0;
  fv = ((~(s ^ dd) & (res ^ dd)) & MSB(sz)) ? 1 : 0;
  fn = (res & MSB(sz)) ? 1 : 0;
  if (extMode) { if (res) fz = 0; } else fz = res == 0;
  return res;
}

uint32_t M68k::sub(uint32_t s, uint32_t dd, int sz, int x, bool extMode, bool setX) {
  const uint32_t m = MASK(sz);
  const uint32_t res = (dd - s - static_cast<uint32_t>(x)) & m;
  fc = static_cast<uint64_t>(s & m) + static_cast<uint64_t>(x) > (dd & m) ? 1 : 0;
  if (setX) fx = fc;
  fv = (((s ^ dd) & (res ^ dd)) & MSB(sz)) ? 1 : 0;
  fn = (res & MSB(sz)) ? 1 : 0;
  if (extMode) { if (res) fz = 0; } else fz = res == 0;
  return res;
}

// BCD com as bandeiras N e V que o chip de verdade produz (pesquisa de flamewing)
uint8_t M68k::abcd(uint8_t s, uint8_t dd) {
  const unsigned ss = s + dd + static_cast<unsigned>(fx);
  const unsigned bc = ((s & dd) | (~ss & s) | (~ss & dd)) & 0x88;
  const unsigned dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
  const unsigned corf = (bc | dc) - ((bc | dc) >> 2);
  const unsigned rr = (ss + corf) & 0xFF;
  fx = fc = ((bc | (ss & ~rr)) >> 7) & 1;
  fv = ((~ss & rr) >> 7) & 1;
  fn = (rr >> 7) & 1;
  if (rr) fz = 0;
  return static_cast<uint8_t>(rr);
}

uint8_t M68k::sbcd(uint8_t s, uint8_t dd) {
  const unsigned xx = dd, yy = s;
  const unsigned di = (xx - yy - static_cast<unsigned>(fx)) & 0x3FF;
  const unsigned bc = ((~xx & yy) | (di & ~xx) | (di & yy)) & 0x88;
  const unsigned corf = bc - (bc >> 2);
  const unsigned rr = (di - corf) & 0x3FF;
  fx = fc = ((bc | (~di & rr)) >> 7) & 1;
  fv = ((di & ~rr) >> 7) & 1;
  fn = (rr >> 7) & 1;
  if (rr & 0xFF) fz = 0;
  return static_cast<uint8_t>(rr);
}

// ------------------------------------------------------------ decodificação
void M68k::execute() {
  switch (ir >> 12) {
    case 0: line0(); break;
    case 1: move(1); break;
    case 2: move(4); break;
    case 3: move(2); break;
    case 4: line4(); break;
    case 5: line5(); break;
    case 6: {  // Bcc / BRA / BSR
      const int cc = (ir >> 8) & 15;
      const int8_t d8 = static_cast<int8_t>(ir & 0xFF);
      const int32_t disp = d8 ? d8 : static_cast<int16_t>(irc);
      if (cc == 1) {  // BSR
        idle(2);
        const uint32_t ret = pc + (d8 ? 2 : 4);
        const uint32_t to = pc + 2 + static_cast<uint32_t>(disp);
        push32(ret);
        jump(to);
      } else if (cc == 0 || cond(cc)) {
        idle(2);
        jump(pc + 2 + static_cast<uint32_t>(disp));
      } else {
        idle(4);
        if (!d8) ext();
        prefetch();
      }
      break;
    }
    case 7:  // MOVEQ
      if (ir & 0x100) { illegal(); break; }
      d[(ir >> 9) & 7] = static_cast<uint32_t>(static_cast<int8_t>(ir & 0xFF));
      setNZ(d[(ir >> 9) & 7], 4); fv = fc = 0;
      prefetch();
      break;
    case 8: line8(); break;
    case 9: line9D(false); break;
    case 10: illegal(10); break;
    case 11: lineB(); break;
    case 12: lineC(); break;
    case 13: line9D(true); break;
    case 14: lineE(); break;
    default: illegal(11); break;
  }
}

void M68k::move(int sz) {
  const int smode = (ir >> 3) & 7, sreg = ir & 7, dmode = (ir >> 6) & 7, dreg = (ir >> 9) & 7;
  if ((smode == 7 && sreg > 4) || (dmode == 7 && dreg > 1) || (sz == 1 && (smode == 1 || dmode == 1))) { illegal(); return; }
  const uint32_t v = readEA(smode, sreg, sz);
  if (dmode == 1) {  // MOVEA
    a[dreg] = sext(v, sz);
    prefetch();
    return;
  }
  if (dmode == 0) {
    setNZ(v, sz); fv = fc = 0;
    writeReg(dreg, sz, v);
    prefetch();
    return;
  }
  if (dmode == 7 && dreg == 1 && smode >= 2 && !(smode == 7 && sreg == 4)) {
    // (xxx).l com origem na memória: a palavra de baixo do endereço ainda está em IRC quando a escrita acontece
    const uint32_t hi = ext();
    const uint32_t ad = (hi << 16) | irc;
    setNZ(v, sz); fv = fc = 0;
    wr(ad, sz, v);
    pc += 2;
    irc = rdProg(pc + 2);
    prefetch();
    return;
  }
  const uint32_t ad = eaAddr(dmode, dreg, sz, true);
  setNZ(v, sz); fv = fc = 0;
  if (dmode == 4) {  // -(An): a pré-busca vem antes da escrita
    prefetch();
    wr(ad, sz, v);
    return;
  }
  wr(ad, sz, v);
  prefetch();
}

void M68k::line0() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7;
  int kind = -1;   // operação de bit (0 BTST, 1 BCHG, 2 BCLR, 3 BSET)
  uint32_t bitn = 0;
  if (op & 0x100) {
    if (mode == 1) {  // MOVEP
      const int dn = (op >> 9) & 7, opm = (op >> 6) & 7;
      const uint32_t ad = a[reg] + static_cast<uint32_t>(static_cast<int16_t>(ext()));
      switch (opm) {
        case 4: { const uint32_t h = rd(ad, 1); const uint32_t l = rd(ad + 2, 1); writeReg(dn, 2, (h << 8) | l); break; }
        case 5: {
          uint32_t v = 0;
          for (int k = 0; k < 4; k++) v = (v << 8) | rd(ad + 2 * k, 1);
          d[dn] = v;
          break;
        }
        case 6: wr(ad, 1, d[dn] >> 8); wr(ad + 2, 1, d[dn]); break;
        default: for (int k = 0; k < 4; k++) wr(ad + 2 * k, 1, d[dn] >> (24 - 8 * k)); break;
      }
      prefetch();
      return;
    }
    kind = (op >> 6) & 3;
    bitn = d[(op >> 9) & 7];
  } else if (((op >> 9) & 7) == 4) {
    kind = (op >> 6) & 3;
    bitn = ext() & 0xFF;
  }
  if (kind >= 0) {
    const bool stat = !(op & 0x100);
    if (mode == 0) {
      const uint32_t b = bitn & 31;
      fz = !((d[reg] >> b) & 1);
      switch (kind) {
        case 0: prefetch(); idle(2); break;
        case 1: d[reg] ^= 1u << b; prefetch(); idle(b < 16 ? 2 : 4); break;
        case 2: d[reg] &= ~(1u << b); prefetch(); idle(b < 16 ? 4 : 6); break;
        default: d[reg] |= 1u << b; prefetch(); idle(b < 16 ? 2 : 4); break;
      }
      (void)stat;
      return;
    }
    const uint32_t b = bitn & 7;
    if (kind == 0) {
      const uint32_t v = readEA(mode, reg, 1);
      fz = !((v >> b) & 1);
      prefetch();
      if (mode == 7 && reg == 4) idle(2);
      return;
    }
    const uint32_t ad = eaAddr(mode, reg, 1);
    uint32_t v = rd(ad, 1);
    fz = !((v >> b) & 1);
    if (kind == 1) v ^= 1u << b; else if (kind == 2) v &= ~(1u << b); else v |= 1u << b;
    prefetch();
    wr(ad, 1, v);
    return;
  }
  // imediatos
  const int which = (op >> 9) & 7;
  const int szb = (op >> 6) & 3;
  if (szb == 3 || which == 7) { illegal(); return; }
  const int sz = SZ(szb);
  if (mode == 7 && reg == 4) {  // para CCR / SR
    if (which != 0 && which != 1 && which != 5) { illegal(); return; }
    if (sz == 4) { illegal(); return; }
    if (sz == 2 && !privileged()) return;
    const uint16_t imm = ext();
    uint16_t sr = getSR();
    const uint16_t m = sz == 1 ? 0x001F : 0xA71F;
    if (which == 0) sr |= imm & m; else if (which == 1) sr &= imm | ~m; else sr ^= imm & m;
    if (sz == 1) sr = (getSR() & 0xFF00) | (sr & 0xFF);
    idle(8);
    setSR(sr);
    irc = rdProg(pc + 2);
    prefetch();
    return;
  }
  const uint32_t imm = sz == 4 ? ext32() : (ext() & MASK(sz));
  auto alu = [&](uint32_t v) -> uint32_t {
    switch (which) {
      case 0: v |= imm; setNZ(v, sz); fv = fc = 0; return v;
      case 1: v &= imm; setNZ(v, sz); fv = fc = 0; return v;
      case 2: return sub(imm, v, sz, 0, false);
      case 3: return add(imm, v, sz, 0, false);
      case 5: v ^= imm; setNZ(v, sz); fv = fc = 0; return v;
      default: sub(imm, v, sz, 0, false, false); return v;  // CMPI
    }
  };
  if (mode == 0) {
    const uint32_t r = alu(d[reg]);
    if (which != 6) writeReg(reg, sz, r);
    prefetch();
    if (sz == 4) idle(which == 6 ? 2 : 4);
    return;
  }
  if (mode == 1) { illegal(); return; }
  const uint32_t ad = eaAddr(mode, reg, sz);
  const uint32_t v = rd(ad, sz);
  const uint32_t r = alu(v);
  prefetch();
  if (which != 6) wr(ad, sz, r);
}

// ------------------------------------------------------------ linha 4: diversas
void M68k::line4() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7;
  if ((op & 0x1C0) == 0x1C0) {  // LEA
    if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) { illegal(); return; }
    const uint32_t ad = eaAddr(mode, reg, 4);
    if (mode == 6 || (mode == 7 && reg == 3)) idle(2);
    a[(op >> 9) & 7] = ad;
    prefetch();
    return;
  }
  if ((op & 0x1C0) == 0x180) {  // CHK.w
    const int32_t src = static_cast<int16_t>(readEA(mode, reg, 2));
    const int32_t dn = static_cast<int16_t>(d[(op >> 9) & 7]);
    prefetch();
    fz = dn == 0; fv = 0; fc = 0;
    // N sai como o chip deixa (fica igual se não houver exceção); Z = Dn zero; V e C zerados
    if (dn > src) { fn = dn < 0; idle(4); exception(6, pc, 0); return; }
    if (dn < 0) { fn = 1; idle(6); exception(6, pc, 0); return; }
    idle(6);
    return;
  }
  const int szb = (op >> 6) & 3;
  switch ((op >> 8) & 0xF) {
    case 0x0: case 0x2: case 0x4: case 0x6: {
      const int k = (op >> 9) & 3;  // 0 NEGX, 1 CLR, 2 NEG, 3 NOT
      if (szb == 3) {
        if (k == 0) {  // MOVE from SR
          if (mode == 0) { writeReg(reg, 2, getSR()); prefetch(); idle(2); return; }
          const uint32_t ad = eaAddr(mode, reg, 2);
          rd(ad, 2);
          prefetch();
          wr(ad, 2, getSR());
          return;
        }
        if (k == 1) { illegal(); return; }  // MOVE from CCR não existe no 68000
        const uint32_t v = readEA(mode, reg, 2);
        if (k == 3 && !sMode) { exception(8, pc - 2 * 0, 4); return; }
        idle(4);
        if (k == 2) setSR(static_cast<uint16_t>((getSR() & 0xFF00) | (v & 0x1F)));
        else setSR(static_cast<uint16_t>(v));
        irc = rdProg(pc + 2);
        prefetch();
        return;
      }
      const int sz = SZ(szb);
      auto f = [&](uint32_t v) -> uint32_t {
        switch (k) {
          case 0: return sub(v, 0, sz, fx, true);
          case 1: fn = 0; fz = 1; fv = 0; fc = 0; return 0;
          case 2: return sub(v, 0, sz, 0, false);
          default: v = ~v; setNZ(v, sz); fv = fc = 0; return v;
        }
      };
      if (mode == 0) {
        writeReg(reg, sz, f(d[reg]));
        prefetch();
        if (sz == 4) idle(2);
        return;
      }
      const uint32_t ad = eaAddr(mode, reg, sz);
      const uint32_t v = rd(ad, sz);
      const uint32_t r = f(v);
      prefetch();
      wr(ad, sz, r);
      return;
    }
    case 0x8:
      if (szb == 0) {  // NBCD
        if (mode == 0) {
          writeReg(reg, 1, sbcd(static_cast<uint8_t>(d[reg]), 0));
          prefetch(); idle(2);
          return;
        }
        const uint32_t ad = eaAddr(mode, reg, 1);
        const uint8_t r = sbcd(static_cast<uint8_t>(rd(ad, 1)), 0);
        prefetch();
        wr(ad, 1, r);
        return;
      }
      if (szb == 1) {
        if (mode == 0) {  // SWAP
          d[reg] = (d[reg] >> 16) | (d[reg] << 16);
          setNZ(d[reg], 4); fv = fc = 0;
          prefetch();
          return;
        }
        // PEA
        const uint32_t ad = eaAddr(mode, reg, 4);
        if (mode == 6 || (mode == 7 && reg == 3)) idle(2);
        prefetch();
        push32(ad);
        return;
      }
      if (mode == 0) {  // EXT
        if (szb == 2) { writeReg(reg, 2, sext(d[reg], 1)); setNZ(d[reg], 2); }
        else { d[reg] = sext(d[reg], 2); setNZ(d[reg], 4); }
        fv = fc = 0;
        prefetch();
        return;
      }
      movem();
      return;
    case 0xA:
      if (op == 0x4AFC) { illegal(); return; }
      if (szb == 3) {  // TAS
        if (mode == 0) {
          setNZ(d[reg], 1); fv = fc = 0;
          d[reg] |= 0x80;
          prefetch();
          return;
        }
        const uint32_t ad = eaAddr(mode, reg, 1);
        const uint32_t v = rd(ad, 1);
        setNZ(v, 1); fv = fc = 0;
        idle(2);
        if (bus.tasWrites()) wr(ad, 1, v | 0x80); else idle(4);
        prefetch();
        return;
      }
      {  // TST
        const int sz = SZ(szb);
        const uint32_t v = readEA(mode, reg, sz);
        setNZ(v, sz); fv = fc = 0;
        prefetch();
        return;
      }
    case 0xC:
      if (szb >= 2) { movem(); return; }
      illegal();
      return;
    case 0xE:
      if (szb == 1) {
        if (op < 0x4E50) { exception(32 + (op & 15), pc + 2, 4); return; }  // TRAP
        if (op < 0x4E58) {  // LINK
          const uint32_t disp = static_cast<uint32_t>(static_cast<int16_t>(ext()));
          a[7] -= 4;
          const uint32_t v = a[reg];
          wr(a[7], 4, v);
          a[reg] = a[7];
          a[7] += disp;
          prefetch();
          return;
        }
        if (op < 0x4E60) {  // UNLK
          a[7] = a[reg];
          const uint32_t v = rd(a[7], 4);
          a[7] += 4;
          a[reg] = v;
          prefetch();
          return;
        }
        if (op < 0x4E68) { if (!privileged()) return; otherSp = a[reg]; prefetch(); return; }  // MOVE An,USP
        if (op < 0x4E70) { if (!privileged()) return; a[reg] = otherSp; prefetch(); return; }  // MOVE USP,An
        switch (op) {
          case 0x4E70:  // RESET
            if (!privileged()) return;
            bus.resetDevices();
            idle(128);
            prefetch();
            return;
          case 0x4E71: prefetch(); return;  // NOP
          case 0x4E72: {  // STOP
            if (!privileged()) return;
            const uint16_t v = irc;
            setSR(v);
            pc += 2;
            ir = irc;
            pc += 2;
            irc = rdProg(pc + 2);
            stopped = true;
            return;
          }
          case 0x4E73: {  // RTE
            if (!privileged()) return;
            const uint32_t sr = rd(a[7], 2);
            const uint32_t to = rd(a[7] + 2, 4);
            a[7] += 6;
            setSR(static_cast<uint16_t>(sr));
            jump(to);
            return;
          }
          case 0x4E75: { const uint32_t to = rd(a[7], 4); a[7] += 4; jump(to); return; }  // RTS
          case 0x4E76:  // TRAPV
            prefetch();
            if (fv) exception(7, pc, 0);
            return;
          case 0x4E77: {  // RTR
            const uint32_t ccr = rd(a[7], 2);
            const uint32_t to = rd(a[7] + 2, 4);
            a[7] += 6;
            setSR(static_cast<uint16_t>((getSR() & 0xFF00) | (ccr & 0x1F)));
            jump(to);
            return;
          }
          default: illegal(); return;
        }
      }
      if (szb >= 2) {  // JSR / JMP
        if (mode < 2 || mode == 3 || mode == 4 || (mode == 7 && reg > 3)) { illegal(); return; }
        const uint32_t start = pc;
        int n = 0;
        const uint32_t to = ctrlAddr(mode, reg, n);
        if (szb == 2) {
          const uint32_t ret = start + 2 + 2 * static_cast<uint32_t>(n);
          // JSR: busca o destino antes de empilhar o retorno
          if (to & 1) { pc = start + 2 * static_cast<uint32_t>(n); faultPc_ = to; throw AddressError{to, true, true}; }
          pc = to;
          ir = rdProg(to);
          push32(ret);
          irc = rdProg(to + 2);
          return;
        }
        jump(to);
        return;
      }
      illegal();
      return;
    default:
      illegal();
      return;
  }
}

void M68k::movem() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7;
  const int sz = (op & 0x40) ? 4 : 2;
  const bool toReg = (op & 0x400) != 0;
  const uint16_t list = ext();
  if (toReg) {
    if (mode < 2 || mode == 4 || (mode == 7 && reg > 3)) { illegal(); return; }
    uint32_t ad = (mode == 3) ? a[reg] : eaAddr(mode, reg, sz);
    if (mode == 3 && (ad & 1) && list) a[reg] = ad + 2;  // erro de endereço: o chip já somou 2
    for (int i = 0; i < 16; i++) {
      if (!(list & (1 << i))) continue;
      uint32_t v = rd(ad, sz);
      if (sz == 2) v = sext(v, 2);
      if (i < 8) d[i] = v; else a[i - 8] = v;
      ad += static_cast<uint32_t>(sz);
    }
    rd(ad, 2);
    if (mode == 3) a[reg] = ad;
    prefetch();
    return;
  }
  if (mode < 2 || mode == 3 || (mode == 7 && reg > 1)) { illegal(); return; }
  if (mode == 4) {
    uint32_t ad = a[reg];
    for (int i = 0; i < 16; i++) {
      if (!(list & (1 << i))) continue;
      const int r = 15 - i;
      ad -= static_cast<uint32_t>(sz);
      if (sz == 4 && (ad & 1)) throw AddressError{ad + 2, false, false};  // a palavra de baixo vai primeiro
      wr(ad, sz, r < 8 ? d[r] : a[r - 8]);
    }
    a[reg] = ad;
    prefetch();
    return;
  }
  uint32_t ad = eaAddr(mode, reg, sz);
  for (int i = 0; i < 16; i++) {
    if (!(list & (1 << i))) continue;
    wr(ad, sz, i < 8 ? d[i] : a[i - 8]);
    ad += static_cast<uint32_t>(sz);
  }
  prefetch();
}

// ------------------------------------------------------------ linha 5: ADDQ/SUBQ/Scc/DBcc
void M68k::line5() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7;
  const int szb = (op >> 6) & 3;
  if (szb == 3) {
    const int cc = (op >> 8) & 15;
    if (mode == 1) {  // DBcc
      if (cond(cc)) { idle(4); ext(); prefetch(); return; }
      const uint32_t cnt = (d[reg] - 1) & 0xFFFF;
      writeReg(reg, 2, cnt);
      if (cnt != 0xFFFF) {
        idle(2);
        jump(pc + 2 + static_cast<uint32_t>(static_cast<int16_t>(irc)));
        return;
      }
      idle(6);
      ext();
      prefetch();
      return;
    }
    const bool t = cond(cc);
    if (mode == 0) {
      writeReg(reg, 1, t ? 0xFF : 0);
      prefetch();
      if (t) idle(2);
      return;
    }
    const uint32_t ad = eaAddr(mode, reg, 1);
    rd(ad, 1);
    prefetch();
    wr(ad, 1, t ? 0xFF : 0);
    return;
  }
  const int sz = SZ(szb);
  uint32_t q = (op >> 9) & 7;
  if (!q) q = 8;
  const bool isSub = (op & 0x100) != 0;
  if (mode == 1) {
    if (sz == 1) { illegal(); return; }
    a[reg] = isSub ? a[reg] - q : a[reg] + q;
    prefetch();
    idle(sz == 4 ? 2 : 4);
    return;
  }
  if (mode == 0) {
    writeReg(reg, sz, isSub ? sub(q, d[reg], sz, 0, false) : add(q, d[reg], sz, 0, false));
    prefetch();
    if (sz == 4) idle(4);
    return;
  }
  const uint32_t ad = eaAddr(mode, reg, sz);
  const uint32_t v = rd(ad, sz);
  const uint32_t r = isSub ? sub(q, v, sz, 0, false) : add(q, v, sz, 0, false);
  prefetch();
  wr(ad, sz, r);
}

// ------------------------------------------------------------ multiplicação e divisão
void M68k::mulu(int mode, int reg) {
  const uint32_t s = readEA(mode, reg, 2);
  const int dn = (ir >> 9) & 7;
  const uint32_t r = (d[dn] & 0xFFFF) * s;
  d[dn] = r;
  setNZ(r, 4); fv = fc = 0;
  prefetch();
  idle(34 + 2 * popcount16(s));
}

void M68k::muls(int mode, int reg) {
  const uint32_t s = readEA(mode, reg, 2);
  const int dn = (ir >> 9) & 7;
  const int32_t r = static_cast<int32_t>(static_cast<int16_t>(d[dn])) * static_cast<int16_t>(s);
  d[dn] = static_cast<uint32_t>(r);
  setNZ(d[dn], 4); fv = fc = 0;
  prefetch();
  idle(34 + 2 * popcount16((s ^ (s << 1)) & 0xFFFF));
}

void M68k::divu(int mode, int reg) {
  const uint32_t s = readEA(mode, reg, 2);
  const int dn = (ir >> 9) & 7;
  uint32_t dividend = d[dn];
  if (s == 0) {
    fn = 0; fz = 0; fv = 0; fc = 0;
    exception(5, pc + 2, 8);
    return;
  }
  if ((dividend >> 16) >= s) {  // estouro
    fv = 1; fc = 0;  // estouro: N e Z ficam como estavam
    idle(6);
    prefetch();
    return;
  }
  const uint32_t q = dividend / s, r = dividend % s;
  int mc = 38;
  const uint32_t hd = s << 16;
  for (int i = 0; i < 15; i++) {
    const uint32_t t = dividend;
    dividend <<= 1;
    if (static_cast<int32_t>(t) < 0) dividend -= hd;
    else { mc += 2; if (dividend >= hd) { dividend -= hd; mc--; } }
  }
  d[dn] = (r << 16) | q;
  fn = (q >> 15) & 1; fz = q == 0; fv = 0; fc = 0;
  idle(mc * 2 - 4);
  prefetch();
}

void M68k::divs(int mode, int reg) {
  const uint32_t s = readEA(mode, reg, 2);
  const int dn = (ir >> 9) & 7;
  const int32_t dividend = static_cast<int32_t>(d[dn]);
  const int32_t divisor = static_cast<int16_t>(s);
  if (divisor == 0) {
    fn = 0; fz = 0; fv = 0; fc = 0;
    exception(5, pc + 2, 8);
    return;
  }
  int mc = 6;
  if (dividend < 0) mc++;
  const uint32_t adiv = dividend < 0 ? 0u - static_cast<uint32_t>(dividend) : static_cast<uint32_t>(dividend);
  const uint32_t ador = static_cast<uint32_t>(divisor < 0 ? -divisor : divisor);
  if ((adiv >> 15) >= ador) {
    fv = 1; fc = 0;  // estouro: N e Z ficam como estavam
    idle((mc + 2) * 2 - 4);
    prefetch();
    return;
  }
  uint32_t aq = adiv / ador;
  mc += 55;
  if (divisor >= 0) { if (dividend >= 0) mc--; else mc++; }
  for (int i = 0; i < 15; i++) { if (static_cast<int16_t>(aq) >= 0) mc++; aq <<= 1; }
  const int64_t q = static_cast<int64_t>(dividend) / divisor;
  const int64_t r = static_cast<int64_t>(dividend) % divisor;
  idle(mc * 2 - 4);
  if (q < -32768 || q > 32767) {
    fv = 1; fc = 0;  // estouro: N e Z ficam como estavam
    prefetch();
    return;
  }
  d[dn] = (static_cast<uint32_t>(r & 0xFFFF) << 16) | static_cast<uint32_t>(q & 0xFFFF);
  fn = (q & 0x8000) ? 1 : 0; fz = (q & 0xFFFF) == 0; fv = 0; fc = 0;
  prefetch();
}

// ------------------------------------------------------------ linha 8: OR/DIV/SBCD
void M68k::line8() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7, opm = (op >> 6) & 7, dn = (op >> 9) & 7;
  if (opm == 3) { divu(mode, reg); return; }
  if (opm == 7) { divs(mode, reg); return; }
  if ((op & 0x1F0) == 0x100) {  // SBCD
    if (op & 8) {
      idle(2);
      a[reg] -= reg == 7 ? 2 : 1;
      const uint8_t s = static_cast<uint8_t>(rd(a[reg], 1));
      a[dn] -= dn == 7 ? 2 : 1;
      const uint8_t dd = static_cast<uint8_t>(rd(a[dn], 1));
      const uint8_t r = sbcd(s, dd);
      prefetch();
      wr(a[dn], 1, r);
      return;
    }
    writeReg(dn, 1, sbcd(static_cast<uint8_t>(d[reg]), static_cast<uint8_t>(d[dn])));
    prefetch(); idle(2);
    return;
  }
  const int sz = SZ(opm & 3);
  if (opm < 3) {
    const uint32_t v = readEA(mode, reg, sz) | d[dn];
    setNZ(v, sz); fv = fc = 0;
    writeReg(dn, sz, v);
    prefetch();
    if (sz == 4) idle(mode == 0 || (mode == 7 && reg == 4) ? 4 : 2);
    return;
  }
  const uint32_t ad = eaAddr(mode, reg, sz);
  const uint32_t v = rd(ad, sz) | d[dn];
  setNZ(v, sz); fv = fc = 0;
  prefetch();
  wr(ad, sz, v);
}

// ------------------------------------------------------------ linhas 9 e D: SUB e ADD
void M68k::line9D(bool isAdd) {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7, opm = (op >> 6) & 7, dn = (op >> 9) & 7;
  if (opm == 3 || opm == 7) {  // ADDA / SUBA
    const int sz = opm == 3 ? 2 : 4;
    const uint32_t s = sext(readEA(mode, reg, sz), sz);
    a[dn] = isAdd ? a[dn] + s : a[dn] - s;
    prefetch();
    idle(sz == 2 || mode <= 1 || (mode == 7 && reg == 4) ? 4 : 2);
    return;
  }
  const int sz = SZ(opm & 3);
  if ((op & 0x130) == 0x100) {  // ADDX / SUBX
    if (op & 8) {
      idle(2);
      const uint32_t step = (sz == 1 && reg == 7) ? 2 : static_cast<uint32_t>(sz);
      a[reg] -= (sz == 4 && (a[reg] & 1)) ? 2 : step;
      const uint32_t s = rd(a[reg], sz);
      const uint32_t dstep = (sz == 1 && dn == 7) ? 2 : static_cast<uint32_t>(sz);
      a[dn] -= (sz == 4 && (a[dn] & 1)) ? 2 : dstep;
      const uint32_t dd = rd(a[dn], sz);
      const uint32_t r = isAdd ? add(s, dd, sz, fx, true) : sub(s, dd, sz, fx, true);
      prefetch();
      wr(a[dn], sz, r);
      return;
    }
    const uint32_t r = isAdd ? add(d[reg], d[dn], sz, fx, true) : sub(d[reg], d[dn], sz, fx, true);
    writeReg(dn, sz, r);
    prefetch();
    if (sz == 4) idle(4);
    return;
  }
  if (opm < 3) {
    const uint32_t s = readEA(mode, reg, sz);
    const uint32_t r = isAdd ? add(s, d[dn], sz, 0, false) : sub(s, d[dn], sz, 0, false);
    writeReg(dn, sz, r);
    prefetch();
    if (sz == 4) idle(mode <= 1 || (mode == 7 && reg == 4) ? 4 : 2);
    return;
  }
  const uint32_t ad = eaAddr(mode, reg, sz);
  const uint32_t v = rd(ad, sz);
  const uint32_t r = isAdd ? add(d[dn], v, sz, 0, false) : sub(d[dn], v, sz, 0, false);
  prefetch();
  wr(ad, sz, r);
}

// ------------------------------------------------------------ linha B: CMP/CMPA/CMPM/EOR
void M68k::lineB() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7, opm = (op >> 6) & 7, dn = (op >> 9) & 7;
  if (opm == 3 || opm == 7) {
    const int sz = opm == 3 ? 2 : 4;
    const uint32_t s = sext(readEA(mode, reg, sz), sz);
    sub(s, a[dn], 4, 0, false, false);
    prefetch();
    idle(2);
    return;
  }
  const int sz = SZ(opm & 3);
  if (opm < 3) {
    const uint32_t s = readEA(mode, reg, sz);
    sub(s, d[dn], sz, 0, false, false);
    prefetch();
    if (sz == 4) idle(2);
    return;
  }
  if (mode == 1) {  // CMPM
    const uint32_t s = rd(eaAddr(3, reg, sz), sz);
    const uint32_t dd = rd(eaAddr(3, dn, sz), sz);
    sub(s, dd, sz, 0, false, false);
    prefetch();
    return;
  }
  if (mode == 0) {
    const uint32_t v = d[reg] ^ d[dn];
    setNZ(v, sz); fv = fc = 0;
    writeReg(reg, sz, v);
    prefetch();
    if (sz == 4) idle(4);
    return;
  }
  const uint32_t ad = eaAddr(mode, reg, sz);
  const uint32_t v = rd(ad, sz) ^ d[dn];
  setNZ(v, sz); fv = fc = 0;
  prefetch();
  wr(ad, sz, v);
}

// ------------------------------------------------------------ linha C: AND/MUL/ABCD/EXG
void M68k::lineC() {
  const int op = ir, mode = (op >> 3) & 7, reg = op & 7, opm = (op >> 6) & 7, dn = (op >> 9) & 7;
  if (opm == 3) { mulu(mode, reg); return; }
  if (opm == 7) { muls(mode, reg); return; }
  if ((op & 0x1F0) == 0x100) {  // ABCD
    if (op & 8) {
      idle(2);
      a[reg] -= reg == 7 ? 2 : 1;
      const uint8_t s = static_cast<uint8_t>(rd(a[reg], 1));
      a[dn] -= dn == 7 ? 2 : 1;
      const uint8_t dd = static_cast<uint8_t>(rd(a[dn], 1));
      const uint8_t r = abcd(s, dd);
      prefetch();
      wr(a[dn], 1, r);
      return;
    }
    writeReg(dn, 1, abcd(static_cast<uint8_t>(d[reg]), static_cast<uint8_t>(d[dn])));
    prefetch(); idle(2);
    return;
  }
  if ((op & 0x1F8) == 0x140) { std::swap(d[dn], d[reg]); prefetch(); idle(2); return; }
  if ((op & 0x1F8) == 0x148) { std::swap(a[dn], a[reg]); prefetch(); idle(2); return; }
  if ((op & 0x1F8) == 0x188) { std::swap(d[dn], a[reg]); prefetch(); idle(2); return; }
  const int sz = SZ(opm & 3);
  if (opm < 3) {
    const uint32_t v = readEA(mode, reg, sz) & d[dn];
    setNZ(v, sz); fv = fc = 0;
    writeReg(dn, sz, v);
    prefetch();
    if (sz == 4) idle(mode == 0 || (mode == 7 && reg == 4) ? 4 : 2);
    return;
  }
  const uint32_t ad = eaAddr(mode, reg, sz);
  const uint32_t v = rd(ad, sz) & d[dn];
  setNZ(v, sz); fv = fc = 0;
  prefetch();
  wr(ad, sz, v);
}

// ------------------------------------------------------------ linha E: deslocamentos e rotações
uint32_t M68k::shift(int type, bool left, uint32_t v, int sz, int cnt) {
  const uint32_t m = MASK(sz), msb = MSB(sz);
  v &= m;
  int c = 0, vf = 0;
  switch (type) {
    case 0:  // AS
      for (int i = 0; i < cnt; i++) {
        if (left) {
          c = (v & msb) ? 1 : 0;
          const uint32_t nv = (v << 1) & m;
          if ((nv ^ v) & msb) vf = 1;
          v = nv;
        } else {
          c = v & 1;
          v = (v >> 1) | (v & msb);
        }
      }
      // contagem maior que o tamanho: o chip de verdade deixa C e X zerados também no ASR de número negativo
      if (!left && cnt > (sz * 8)) c = 0;
      if (cnt) fx = c;
      fc = cnt ? c : 0;
      fv = vf;
      break;
    case 1:  // LS
      for (int i = 0; i < cnt; i++) {
        if (left) { c = (v & msb) ? 1 : 0; v = (v << 1) & m; }
        else { c = v & 1; v >>= 1; }
      }
      if (cnt) fx = c;
      fc = cnt ? c : 0;
      fv = 0;
      break;
    case 2:  // ROX
      for (int i = 0; i < cnt; i++) {
        if (left) { c = (v & msb) ? 1 : 0; v = ((v << 1) | static_cast<uint32_t>(fx)) & m; }
        else { c = v & 1; v = (v >> 1) | (fx ? msb : 0); }
        fx = c;
      }
      fc = fx;
      fv = 0;
      break;
    default:  // RO
      for (int i = 0; i < cnt; i++) {
        if (left) { c = (v & msb) ? 1 : 0; v = ((v << 1) | static_cast<uint32_t>(c)) & m; }
        else { c = v & 1; v = (v >> 1) | (c ? msb : 0); }
      }
      fc = cnt ? c : 0;
      fv = 0;
      break;
  }
  setNZ(v, sz);
  return v;
}

void M68k::lineE() {
  const int op = ir, szb = (op >> 6) & 3;
  if (szb == 3) {  // memória (palavra, 1 posição)
    const int mode = (op >> 3) & 7, reg = op & 7;
    if ((op & 0x800) || mode < 2 || (mode == 7 && reg > 1)) { illegal(); return; }
    const uint32_t ad = eaAddr(mode, reg, 2);
    const uint32_t v = rd(ad, 2);
    const uint32_t r = shift((op >> 9) & 3, (op & 0x100) != 0, v, 2, 1);
    prefetch();
    wr(ad, 2, r);
    return;
  }
  const int sz = SZ(szb), reg = op & 7;
  const int cnt = (op & 0x20) ? static_cast<int>(d[(op >> 9) & 7] & 63) : (((op >> 9) & 7) ? ((op >> 9) & 7) : 8);
  writeReg(reg, sz, shift((op >> 3) & 3, (op & 0x100) != 0, d[reg], sz, cnt));
  prefetch();
  idle(2 * cnt + (sz == 4 ? 4 : 2));
}

// ------------------------------------------------------------ estado
void M68k::sync(StateIO& io) {
  io.raw(d, sizeof d); io.raw(a, sizeof a);
  io.sync(otherSp); io.sync(pc); io.sync(ir); io.sync(irc);
  io.sync(fx); io.sync(fn); io.sync(fz); io.sync(fv); io.sync(fc);
  io.sync(sMode); io.sync(tMode); io.sync(mask);
  io.sync(stopped); io.sync(halted); io.sync(ipl); io.sync(nmiPrev_); io.sync(faultPc_);
  if (io.reading()) {
    fx &= 1; fn &= 1; fz &= 1; fv &= 1; fc &= 1; sMode &= 1; tMode &= 1; mask &= 7; ipl &= 7;
  }
}

}  // namespace indigo::cps
