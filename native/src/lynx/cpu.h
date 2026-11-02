// CPU do Atari Lynx: 65SC02 embutido no Mikey (VTI VL65NC02). Cada acesso ao barramento é um ciclo, na
// mesma ordem do chip (inclusive as leituras "falsas" dos modos indexados e do ciclo extra do modo decimal),
// conferido contra os SingleStepTests do 65x02 (variante synertek65c02 = 65SC02).
//
// Instruções de bit da Rockwell (RMB/SMB/BBR/BBS, opcodes $x7 e $xF): o Lynx I não tem (são NOPs de 1 byte),
// mas o Lynx II tem (manual do BLL, doc/65sc02.txt) e há demos/homebrew que usam. Com bitOps = true (padrão
// do núcleo, já que emulamos o Lynx II) elas funcionam como no 65C02 da WDC (conferidas contra wdc65c02).
// WAI/STP não existem no 65SC02: $CB e $DB fazem o mesmo que no Synertek (NOPs).
//
// O barramento é um parâmetro de modelo (sem chamadas virtuais): precisa ter
//   uint8_t read(uint16_t a);  void write(uint16_t a, uint8_t v);
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::lynx {

template <class Bus>
class Cpu65SC02 {
public:
  enum : uint8_t { C = 1, Z = 2, I = 4, D = 8, B = 0x10, U = 0x20, V = 0x40, N = 0x80 };

  explicit Cpu65SC02(Bus& b) : bus(b) {}

  Bus& bus;
  bool bitOps = true;
  uint16_t pc = 0;
  uint8_t a = 0, x = 0, y = 0, s = 0xFF, p = 0x34;

  void reset() {
    rd(pc); rd(pc);
    rd(0x100 | s); s--;
    rd(0x100 | s); s--;
    rd(0x100 | s); s--;
    p = (p | I | U) & ~D;
    const uint8_t lo = rd(0xFFFC);
    pc = lo | (rd(0xFFFD) << 8);
  }

  // atende a interrupção (o chamador já conferiu a linha e a flag I)
  void irq() {
    rd(pc); rd(pc);
    push(pc >> 8);
    push(pc & 0xFF);
    push((p | U) & ~B);
    p = (p | I) & ~D;
    const uint8_t lo = rd(0xFFFE);
    pc = lo | (rd(0xFFFF) << 8);
  }

  // NMI (não existe no Lynx; usado pelo Watara Supervision): mesma sequência da IRQ, vetor $FFFA
  void nmi() {
    rd(pc); rd(pc);
    push(pc >> 8);
    push(pc & 0xFF);
    push((p | U) & ~B);
    p = (p | I) & ~D;
    const uint8_t lo = rd(0xFFFA);
    pc = lo | (rd(0xFFFB) << 8);
  }

  void step();

  void sync(StateIO& io) {
    io.sync(pc); io.sync(a); io.sync(x); io.sync(y); io.sync(s); io.sync(p);
  }

private:
  uint8_t rd(uint16_t ad) { return bus.read(ad); }
  void wr(uint16_t ad, uint8_t v) { bus.write(ad, v); }
  uint8_t fetch() { return rd(pc++); }
  uint16_t fetch16() { const uint8_t lo = fetch(); return lo | (fetch() << 8); }
  void push(uint8_t v) { wr(0x100 | s, v); s--; }
  uint8_t pull() { s++; return rd(0x100 | s); }
  void setZN(uint8_t v) { p = (p & ~(Z | N)) | (v ? 0 : Z) | (v & N); }

  // ---------------------------------------------------------------- endereçamento
  uint16_t zp() { return fetch(); }
  uint16_t zpi(uint8_t r) { const uint8_t b = fetch(); rd(b); return static_cast<uint8_t>(b + r); }
  uint16_t ab() { return fetch16(); }
  // absoluto indexado: o ciclo extra (ao cruzar página, ou sempre nas escritas/RMW) relê o último byte da
  // instrução
  uint16_t abi(uint8_t r, bool always) {
    const uint16_t base = fetch16();
    const uint16_t ea = static_cast<uint16_t>(base + r);
    if (always || ((base ^ ea) & 0xFF00)) rd(static_cast<uint16_t>(pc - 1));
    return ea;
  }
  uint16_t izx() {
    const uint8_t b = fetch();
    rd(b);
    const uint8_t z = static_cast<uint8_t>(b + x);
    const uint8_t lo = rd(z);
    return lo | (rd(static_cast<uint8_t>(z + 1)) << 8);
  }
  uint16_t izy(bool always) {
    const uint8_t z = fetch();
    const uint8_t lo = rd(z);
    const uint16_t base = lo | (rd(static_cast<uint8_t>(z + 1)) << 8);
    const uint16_t ea = static_cast<uint16_t>(base + y);
    if (always || ((base ^ ea) & 0xFF00)) rd(static_cast<uint16_t>(pc - 1));
    return ea;
  }
  uint16_t izp() {
    const uint8_t z = fetch();
    const uint8_t lo = rd(z);
    return lo | (rd(static_cast<uint8_t>(z + 1)) << 8);
  }

  // ---------------------------------------------------------------- ALU
  void adc(uint8_t v, uint16_t dummy) {
    const int c = p & C;
    if (p & D) {
      int al = (a & 0x0F) + (v & 0x0F) + c;
      if (al >= 0x0A) al = ((al + 0x06) & 0x0F) + 0x10;
      int r = (a & 0xF0) + (v & 0xF0) + al;
      const int sr = static_cast<int8_t>(a & 0xF0) + static_cast<int8_t>(v & 0xF0) + al;
      p &= ~(C | V);
      if (sr < -128 || sr > 127) p |= V;
      if (r >= 0xA0) r += 0x60;
      if (r >= 0x100) p |= C;
      a = static_cast<uint8_t>(r);
      rd(dummy);  // ciclo extra do modo decimal
      setZN(a);
      return;
    }
    const int t = a + v + c;
    p &= ~(C | V);
    if (t > 0xFF) p |= C;
    if (~(a ^ v) & (a ^ t) & 0x80) p |= V;
    a = static_cast<uint8_t>(t);
    setZN(a);
  }
  void sbc(uint8_t v, uint16_t dummy) {
    const int c = p & C;
    const int bin = a - v + c - 1;
    p &= ~(C | V);
    if (bin >= 0) p |= C;
    if ((a ^ v) & (a ^ bin) & 0x80) p |= V;
    if (p & D) {
      const int al = (a & 0x0F) - (v & 0x0F) + c - 1;
      int r = bin;
      if (r < 0) r -= 0x60;
      if (al < 0) r -= 0x06;
      a = static_cast<uint8_t>(r);
      rd(dummy);
      setZN(a);
      return;
    }
    a = static_cast<uint8_t>(bin);
    setZN(a);
  }
  void cmp(uint8_t r, uint8_t v) {
    const int t = r - v;
    p = (p & ~C) | (t >= 0 ? C : 0);
    setZN(static_cast<uint8_t>(t));
  }
  void bit(uint8_t v) { p = (p & ~(Z | V | N)) | ((a & v) ? 0 : Z) | (v & (V | N)); }
  uint8_t asl(uint8_t v) { p = (p & ~C) | (v >> 7); v <<= 1; setZN(v); return v; }
  uint8_t lsr(uint8_t v) { p = (p & ~C) | (v & 1); v >>= 1; setZN(v); return v; }
  uint8_t rol(uint8_t v) { const int c = p & C; p = (p & ~C) | (v >> 7); v = static_cast<uint8_t>((v << 1) | c); setZN(v); return v; }
  uint8_t ror(uint8_t v) { const int c = p & C; p = (p & ~C) | (v & 1); v = static_cast<uint8_t>((v >> 1) | (c << 7)); setZN(v); return v; }

  // leitura-modificação-escrita: lê, relê (65C02 não escreve o valor velho) e grava
  template <class F>
  void rmw(uint16_t ea, F f) {
    uint8_t v = rd(ea);
    rd(ea);
    v = f(v);
    wr(ea, v);
  }

  void branch(bool cond) {
    const int8_t off = static_cast<int8_t>(fetch());
    if (!cond) return;
    rd(pc);
    const uint16_t to = static_cast<uint16_t>(pc + off);
    if ((to ^ pc) & 0xFF00) rd((pc & 0xFF00) | (to & 0xFF));
    pc = to;
  }
};

template <class Bus>
void Cpu65SC02<Bus>::step() {
  const uint8_t op = fetch();
  uint16_t ea;
  // opcodes $x7/$xF: bits da Rockwell (Lynx II) ou NOP de 1 byte (Lynx I)
  if ((op & 0x07) == 0x07) {
    if (!bitOps) return;
    const uint8_t mask = static_cast<uint8_t>(1u << ((op >> 4) & 7));
    const uint8_t z = fetch();
    uint8_t v = rd(z);
    rd(z);
    if ((op & 0x0F) == 0x07) {  // RMB/SMB
      v = (op & 0x80) ? (v | mask) : (v & ~mask);
      wr(z, v);
      return;
    }
    // BBR/BBS: os ciclos extras do desvio releem o endereço seguinte à instrução
    const int8_t off = static_cast<int8_t>(fetch());
    const bool set = (v & mask) != 0;
    if (set == ((op & 0x80) != 0)) {
      rd(pc);
      const uint16_t to = static_cast<uint16_t>(pc + off);
      if ((to ^ pc) & 0xFF00) rd(pc);
      pc = to;
    }
    return;
  }
  switch (op) {
    // ------------------------------------------------ ORA
    case 0x09: a |= fetch(); setZN(a); break;
    case 0x05: a |= rd(zp()); setZN(a); break;
    case 0x15: a |= rd(zpi(x)); setZN(a); break;
    case 0x0D: a |= rd(ab()); setZN(a); break;
    case 0x1D: a |= rd(abi(x, false)); setZN(a); break;
    case 0x19: a |= rd(abi(y, false)); setZN(a); break;
    case 0x01: a |= rd(izx()); setZN(a); break;
    case 0x11: a |= rd(izy(false)); setZN(a); break;
    case 0x12: a |= rd(izp()); setZN(a); break;
    // ------------------------------------------------ AND
    case 0x29: a &= fetch(); setZN(a); break;
    case 0x25: a &= rd(zp()); setZN(a); break;
    case 0x35: a &= rd(zpi(x)); setZN(a); break;
    case 0x2D: a &= rd(ab()); setZN(a); break;
    case 0x3D: a &= rd(abi(x, false)); setZN(a); break;
    case 0x39: a &= rd(abi(y, false)); setZN(a); break;
    case 0x21: a &= rd(izx()); setZN(a); break;
    case 0x31: a &= rd(izy(false)); setZN(a); break;
    case 0x32: a &= rd(izp()); setZN(a); break;
    // ------------------------------------------------ EOR
    case 0x49: a ^= fetch(); setZN(a); break;
    case 0x45: a ^= rd(zp()); setZN(a); break;
    case 0x55: a ^= rd(zpi(x)); setZN(a); break;
    case 0x4D: a ^= rd(ab()); setZN(a); break;
    case 0x5D: a ^= rd(abi(x, false)); setZN(a); break;
    case 0x59: a ^= rd(abi(y, false)); setZN(a); break;
    case 0x41: a ^= rd(izx()); setZN(a); break;
    case 0x51: a ^= rd(izy(false)); setZN(a); break;
    case 0x52: a ^= rd(izp()); setZN(a); break;
    // ------------------------------------------------ ADC / SBC (o ciclo extra do decimal relê o endereço;
    // no imediato o chip lê $0056 (ADC) ou $0000 (SBC), como registram os testes)
    case 0x69: adc(fetch(), 0x0056); break;
    case 0x65: ea = zp(); adc(rd(ea), ea); break;
    case 0x75: ea = zpi(x); adc(rd(ea), ea); break;
    case 0x6D: ea = ab(); adc(rd(ea), ea); break;
    case 0x7D: ea = abi(x, false); adc(rd(ea), ea); break;
    case 0x79: ea = abi(y, false); adc(rd(ea), ea); break;
    case 0x61: ea = izx(); adc(rd(ea), ea); break;
    case 0x71: ea = izy(false); adc(rd(ea), ea); break;
    case 0x72: ea = izp(); adc(rd(ea), ea); break;
    case 0xE9: sbc(fetch(), 0x0000); break;
    case 0xE5: ea = zp(); sbc(rd(ea), ea); break;
    case 0xF5: ea = zpi(x); sbc(rd(ea), ea); break;
    case 0xED: ea = ab(); sbc(rd(ea), ea); break;
    case 0xFD: ea = abi(x, false); sbc(rd(ea), ea); break;
    case 0xF9: ea = abi(y, false); sbc(rd(ea), ea); break;
    case 0xE1: ea = izx(); sbc(rd(ea), ea); break;
    case 0xF1: ea = izy(false); sbc(rd(ea), ea); break;
    case 0xF2: ea = izp(); sbc(rd(ea), ea); break;
    // ------------------------------------------------ CMP / CPX / CPY
    case 0xC9: cmp(a, fetch()); break;
    case 0xC5: cmp(a, rd(zp())); break;
    case 0xD5: cmp(a, rd(zpi(x))); break;
    case 0xCD: cmp(a, rd(ab())); break;
    case 0xDD: cmp(a, rd(abi(x, false))); break;
    case 0xD9: cmp(a, rd(abi(y, false))); break;
    case 0xC1: cmp(a, rd(izx())); break;
    case 0xD1: cmp(a, rd(izy(false))); break;
    case 0xD2: cmp(a, rd(izp())); break;
    case 0xE0: cmp(x, fetch()); break;
    case 0xE4: cmp(x, rd(zp())); break;
    case 0xEC: cmp(x, rd(ab())); break;
    case 0xC0: cmp(y, fetch()); break;
    case 0xC4: cmp(y, rd(zp())); break;
    case 0xCC: cmp(y, rd(ab())); break;
    // ------------------------------------------------ BIT
    case 0x89: p = (p & ~Z) | ((a & fetch()) ? 0 : Z); break;
    case 0x24: bit(rd(zp())); break;
    case 0x34: bit(rd(zpi(x))); break;
    case 0x2C: bit(rd(ab())); break;
    case 0x3C: bit(rd(abi(x, false))); break;
    // ------------------------------------------------ LDA / LDX / LDY
    case 0xA9: a = fetch(); setZN(a); break;
    case 0xA5: a = rd(zp()); setZN(a); break;
    case 0xB5: a = rd(zpi(x)); setZN(a); break;
    case 0xAD: a = rd(ab()); setZN(a); break;
    case 0xBD: a = rd(abi(x, false)); setZN(a); break;
    case 0xB9: a = rd(abi(y, false)); setZN(a); break;
    case 0xA1: a = rd(izx()); setZN(a); break;
    case 0xB1: a = rd(izy(false)); setZN(a); break;
    case 0xB2: a = rd(izp()); setZN(a); break;
    case 0xA2: x = fetch(); setZN(x); break;
    case 0xA6: x = rd(zp()); setZN(x); break;
    case 0xB6: x = rd(zpi(y)); setZN(x); break;
    case 0xAE: x = rd(ab()); setZN(x); break;
    case 0xBE: x = rd(abi(y, false)); setZN(x); break;
    case 0xA0: y = fetch(); setZN(y); break;
    case 0xA4: y = rd(zp()); setZN(y); break;
    case 0xB4: y = rd(zpi(x)); setZN(y); break;
    case 0xAC: y = rd(ab()); setZN(y); break;
    case 0xBC: y = rd(abi(x, false)); setZN(y); break;
    // ------------------------------------------------ STA / STX / STY / STZ
    case 0x85: wr(zp(), a); break;
    case 0x95: wr(zpi(x), a); break;
    case 0x8D: wr(ab(), a); break;
    case 0x9D: wr(abi(x, true), a); break;
    case 0x99: wr(abi(y, true), a); break;
    case 0x81: wr(izx(), a); break;
    case 0x91: wr(izy(true), a); break;
    case 0x92: wr(izp(), a); break;
    case 0x86: wr(zp(), x); break;
    case 0x96: wr(zpi(y), x); break;
    case 0x8E: wr(ab(), x); break;
    case 0x84: wr(zp(), y); break;
    case 0x94: wr(zpi(x), y); break;
    case 0x8C: wr(ab(), y); break;
    case 0x64: wr(zp(), 0); break;
    case 0x74: wr(zpi(x), 0); break;
    case 0x9C: wr(ab(), 0); break;
    case 0x9E: wr(abi(x, true), 0); break;
    // ------------------------------------------------ deslocamentos, INC/DEC (RMW)
    case 0x0A: rd(pc); a = asl(a); break;
    case 0x06: rmw(zp(), [this](uint8_t v) { return asl(v); }); break;
    case 0x16: rmw(zpi(x), [this](uint8_t v) { return asl(v); }); break;
    case 0x0E: rmw(ab(), [this](uint8_t v) { return asl(v); }); break;
    case 0x1E: rmw(abi(x, false), [this](uint8_t v) { return asl(v); }); break;
    case 0x4A: rd(pc); a = lsr(a); break;
    case 0x46: rmw(zp(), [this](uint8_t v) { return lsr(v); }); break;
    case 0x56: rmw(zpi(x), [this](uint8_t v) { return lsr(v); }); break;
    case 0x4E: rmw(ab(), [this](uint8_t v) { return lsr(v); }); break;
    case 0x5E: rmw(abi(x, false), [this](uint8_t v) { return lsr(v); }); break;
    case 0x2A: rd(pc); a = rol(a); break;
    case 0x26: rmw(zp(), [this](uint8_t v) { return rol(v); }); break;
    case 0x36: rmw(zpi(x), [this](uint8_t v) { return rol(v); }); break;
    case 0x2E: rmw(ab(), [this](uint8_t v) { return rol(v); }); break;
    case 0x3E: rmw(abi(x, false), [this](uint8_t v) { return rol(v); }); break;
    case 0x6A: rd(pc); a = ror(a); break;
    case 0x66: rmw(zp(), [this](uint8_t v) { return ror(v); }); break;
    case 0x76: rmw(zpi(x), [this](uint8_t v) { return ror(v); }); break;
    case 0x6E: rmw(ab(), [this](uint8_t v) { return ror(v); }); break;
    case 0x7E: rmw(abi(x, false), [this](uint8_t v) { return ror(v); }); break;
    case 0x1A: rd(pc); a++; setZN(a); break;
    case 0xE6: rmw(zp(), [this](uint8_t v) { v++; setZN(v); return v; }); break;
    case 0xF6: rmw(zpi(x), [this](uint8_t v) { v++; setZN(v); return v; }); break;
    case 0xEE: rmw(ab(), [this](uint8_t v) { v++; setZN(v); return v; }); break;
    case 0xFE: rmw(abi(x, true), [this](uint8_t v) { v++; setZN(v); return v; }); break;
    case 0x3A: rd(pc); a--; setZN(a); break;
    case 0xC6: rmw(zp(), [this](uint8_t v) { v--; setZN(v); return v; }); break;
    case 0xD6: rmw(zpi(x), [this](uint8_t v) { v--; setZN(v); return v; }); break;
    case 0xCE: rmw(ab(), [this](uint8_t v) { v--; setZN(v); return v; }); break;
    case 0xDE: rmw(abi(x, true), [this](uint8_t v) { v--; setZN(v); return v; }); break;
    // TSB / TRB
    case 0x04: rmw(zp(), [this](uint8_t v) { p = (p & ~Z) | ((a & v) ? 0 : Z); return static_cast<uint8_t>(v | a); }); break;
    case 0x0C: rmw(ab(), [this](uint8_t v) { p = (p & ~Z) | ((a & v) ? 0 : Z); return static_cast<uint8_t>(v | a); }); break;
    case 0x14: rmw(zp(), [this](uint8_t v) { p = (p & ~Z) | ((a & v) ? 0 : Z); return static_cast<uint8_t>(v & ~a); }); break;
    case 0x1C: rmw(ab(), [this](uint8_t v) { p = (p & ~Z) | ((a & v) ? 0 : Z); return static_cast<uint8_t>(v & ~a); }); break;
    // ------------------------------------------------ registradores
    case 0xE8: rd(pc); x++; setZN(x); break;
    case 0xC8: rd(pc); y++; setZN(y); break;
    case 0xCA: rd(pc); x--; setZN(x); break;
    case 0x88: rd(pc); y--; setZN(y); break;
    case 0xAA: rd(pc); x = a; setZN(x); break;
    case 0xA8: rd(pc); y = a; setZN(y); break;
    case 0x8A: rd(pc); a = x; setZN(a); break;
    case 0x98: rd(pc); a = y; setZN(a); break;
    case 0xBA: rd(pc); x = s; setZN(x); break;
    case 0x9A: rd(pc); s = x; break;
    case 0x18: rd(pc); p &= ~C; break;
    case 0x38: rd(pc); p |= C; break;
    case 0x58: rd(pc); p &= ~I; break;
    case 0x78: rd(pc); p |= I; break;
    case 0xB8: rd(pc); p &= ~V; break;
    case 0xD8: rd(pc); p &= ~D; break;
    case 0xF8: rd(pc); p |= D; break;
    case 0xEA: rd(pc); break;
    // ------------------------------------------------ pilha
    case 0x48: rd(pc); push(a); break;
    case 0xDA: rd(pc); push(x); break;
    case 0x5A: rd(pc); push(y); break;
    case 0x08: rd(pc); push(p | B | U); break;
    case 0x68: rd(pc); rd(0x100 | s); a = pull(); setZN(a); break;
    case 0xFA: rd(pc); rd(0x100 | s); x = pull(); setZN(x); break;
    case 0x7A: rd(pc); rd(0x100 | s); y = pull(); setZN(y); break;
    case 0x28: rd(pc); rd(0x100 | s); p = (pull() & ~B) | U; break;
    // ------------------------------------------------ desvios e saltos
    case 0x10: branch(!(p & N)); break;
    case 0x30: branch(p & N); break;
    case 0x50: branch(!(p & V)); break;
    case 0x70: branch(p & V); break;
    case 0x90: branch(!(p & C)); break;
    case 0xB0: branch(p & C); break;
    case 0xD0: branch(!(p & Z)); break;
    case 0xF0: branch(p & Z); break;
    case 0x80: branch(true); break;
    case 0x4C: pc = fetch16(); break;
    case 0x6C: {  // JMP (abs): sem o defeito do 6502 NMOS, com um ciclo a mais
      const uint16_t ptr = fetch16();
      const uint8_t lo = rd(ptr);
      rd((ptr & 0xFF00) | ((ptr + 1) & 0xFF));  // leitura dentro da página, descartada
      pc = lo | (rd(static_cast<uint16_t>(ptr + 1)) << 8);
      break;
    }
    case 0x7C: {  // JMP (abs,X)
      const uint16_t base = fetch16();
      rd(static_cast<uint16_t>(pc - 2));
      const uint16_t ptr = static_cast<uint16_t>(base + x);
      const uint8_t lo = rd(ptr);
      pc = lo | (rd(static_cast<uint16_t>(ptr + 1)) << 8);
      break;
    }
    case 0x20: {  // JSR
      const uint8_t lo = fetch();
      rd(0x100 | s);
      push(pc >> 8);
      push(pc & 0xFF);
      pc = lo | (fetch() << 8);
      break;
    }
    case 0x60: {  // RTS
      rd(pc);
      rd(0x100 | s);
      const uint8_t lo = pull();
      pc = lo | (pull() << 8);
      rd(pc);
      pc++;
      break;
    }
    case 0x40: {  // RTI
      rd(pc);
      rd(0x100 | s);
      p = (pull() & ~B) | U;
      const uint8_t lo = pull();
      pc = lo | (pull() << 8);
      break;
    }
    case 0x00: {  // BRK
      fetch();
      push(pc >> 8);
      push(pc & 0xFF);
      push(p | B | U);
      p = (p | I) & ~D;
      const uint8_t lo = rd(0xFFFE);
      pc = lo | (rd(0xFFFF) << 8);
      break;
    }
    // ------------------------------------------------ opcodes sem uso (NOPs com o tamanho e os ciclos do chip)
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xC2: case 0xE2: fetch(); break;
    case 0x44: rd(zp()); break;
    case 0x54: case 0xD4: case 0xF4: rd(zpi(x)); break;
    case 0xDB: rd(zpi(x)); break;
    case 0xCB: rd(pc); break;
    case 0x5C: case 0xDC: case 0xFC: fetch16(); rd(static_cast<uint16_t>(pc - 1)); break;
    default: break;  // $x3 e $xB: 1 byte, 1 ciclo
  }
}

}  // namespace indigo::lynx
