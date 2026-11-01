// CPU Zilog Z80 (porte de src/sms/z80.js): conjunto completo, incluindo instruções e flags não
// documentadas (bits 3 e 5, IXH/IXL, SLL, DDCB com cópia no registrador) e MEMPTR para BIT n,(HL).
// Cada step() executa uma instrução e devolve os T-states gastos.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo {

class Z80Bus {
public:
  virtual ~Z80Bus() = default;
  virtual int read(int a) = 0;
  virtual void write(int a, int v) = 0;
  virtual int in(int port) = 0;
  virtual void out(int port, int v) = 0;
};

class Z80 {
public:
  explicit Z80(Z80Bus& busRef) : bus(busRef) { reset(); }

  int a, f, b, c, d, e, h, l;
  int a2, f2, b2, c2, d2, e2, h2, l2;
  int ix, iy, sp, pc;
  int i, r, wz;
  int iff1, iff2, im;
  bool halted, eiDelay, irqLine, nmiPending;

  void reset();
  int step();
  int bc() const { return (b << 8) | c; }
  int de() const { return (d << 8) | e; }
  int hl() const { return (h << 8) | l; }
  void sync(StateIO& io);

private:
  int rd(int addr) { return bus.read(addr & 0xFFFF); }
  void wr(int addr, int v) { bus.write(addr & 0xFFFF, v & 0xFF); }
  int rd16(int addr) { const int lo = rd(addr); const int hi = rd(addr + 1); return lo | (hi << 8); }
  void wr16(int addr, int v) { wr(addr, v); wr(addr + 1, v >> 8); }
  int imm() { const int v = rd(pc); pc = (pc + 1) & 0xFFFF; return v; }
  int imm16() { const int v = rd16(pc); pc = (pc + 2) & 0xFFFF; return v; }
  int disp() { const int x = imm(); return x < 128 ? x : x - 256; }
  void push(int v) { sp = (sp - 2) & 0xFFFF; wr16(sp, v); }
  int pop() { const int v = rd16(sp); sp = (sp + 2) & 0xFFFF; return v; }
  void incR() { r = (r & 0x80) | ((r + 1) & 0x7F); }

  void setBC(int v) { b = (v >> 8) & 0xFF; c = v & 0xFF; }
  void setDE(int v) { d = (v >> 8) & 0xFF; e = v & 0xFF; }
  void setHLr(int v) { h = (v >> 8) & 0xFF; l = v & 0xFF; }
  int af() const { return (a << 8) | f; }
  void setAF(int v) { a = (v >> 8) & 0xFF; f = v & 0xFF; }

  int interrupt();
  int getR(int i, int idx) const;
  void setR(int i, int idx, int v);
  int getHL(int idx) const { return idx == 0 ? hl() : idx == 1 ? ix : iy; }
  void setHL(int idx, int v);
  int addrHL(int idx);
  int getRR(int p, int idx) const { return p == 0 ? bc() : p == 1 ? de() : p == 2 ? getHL(idx) : sp; }
  void setRR(int p, int idx, int v);
  bool cond(int cc) const;

  void alu(int op, int v);
  int inc8(int v);
  int dec8(int v);
  int add16(int x, int v);
  void adc16(int v);
  void sbc16(int v);
  int rot(int op, int v);
  void bit(int n, int v, int xy);
  void daa();

  int exec(int op, int idx);
  int execCB();
  int execIndexCB(int idx);
  int execED();
  int block(int y, int z);

  Z80Bus& bus;
};

}  // namespace indigo
