// CPU Toshiba TLCS-900/H do Neo Geo Pocket (porte de src/ngp/tlcs900h.js): 4 bancos de XWA/XBC/XDE/XHL +
// XIX/XIY/XIZ/XSP de 32 bits, operações de 8/16/32 bits, modos de endereçamento com prefixo e registradores
// de controle do micro-DMA. step() executa uma instrução e devolve os estados gastos.
#pragma once
#include <cstdint>
#include <functional>

#include "../core/state.h"

namespace indigo::ngp {

class TlcsBus {
public:
  virtual ~TlcsBus() = default;
  virtual uint32_t read8(uint32_t a) = 0;
  virtual uint32_t read16(uint32_t a) = 0;
  virtual uint32_t read32(uint32_t a) = 0;
  virtual void write8(uint32_t a, uint32_t v) = 0;
  virtual void write16(uint32_t a, uint32_t v) = 0;
  virtual void write32(uint32_t a, uint32_t v) = 0;
};

class TLCS900H {
public:
  explicit TLCS900H(TlcsBus& busRef) : bus(busRef) { reset(); }

  uint32_t r[21];  // [banco*4 + n], 16..19 = XIX XIY XIZ XSP, 20 = código inválido (descarte)
  uint32_t pc = 0;
  int sr = 0xF800, fd = 0;
  int intNest = 0;
  bool halted = false;
  uint32_t dmaS[4], dmaD[4];
  uint16_t dmaC[4];
  uint8_t dmaM[4];
  int cycles = 0;
  int op2 = -1;  // segundo byte da instrução (diagnóstico)
  std::function<void()> onBios;

  void reset();
  int step();
  int interrupt(int vector, int level);

  int rfp() const { return (sr >> 8) & 3; }
  int f() const { return sr & 0xFF; }
  void setFlags(int v) { sr = (sr & 0xFF28) | (v & 0xD7); }  // bits 5 e 3 não são afetados pelas operações
  void setF(int v) { sr = (sr & 0xFF00) | (v & 0xFF); }

  int idx(int c) const;
  uint32_t getB(int c) const;
  void setB(int c, int64_t v);
  uint32_t getW(int c) const;
  void setW(int c, int64_t v);
  uint32_t getL(int c) const { return r[idx(c)]; }
  void setL(int c, int64_t v) { r[idx(c)] = static_cast<uint32_t>(v); }
  uint32_t getR(int c, int sz) const { return sz == 1 ? getB(c) : sz == 2 ? getW(c) : getL(c); }
  void setR(int c, int sz, int64_t v) { if (sz == 1) setB(c, v); else if (sz == 2) setW(c, v); else setL(c, v); }
  uint32_t xsp() const { return r[19]; }
  void setXsp(int64_t v) { r[19] = static_cast<uint32_t>(v); }

  uint32_t rd(int64_t a, int sz);
  void wr(int64_t a, int sz, int64_t v);
  void push(int sz, int64_t v) { setXsp(int64_t(xsp()) - sz); wr(xsp(), sz, v); }
  uint32_t pop(int sz) { const uint32_t v = rd(xsp(), sz); setXsp(int64_t(xsp()) + sz); return v; }

  void sync(StateIO& io);

private:
  uint32_t f8();
  uint32_t f16();
  uint32_t f24() { const uint32_t lo = f16(); const uint32_t hi = f8(); return lo | (hi << 16); }
  uint32_t f32();
  uint32_t fimm(int sz) { return sz == 1 ? f8() : sz == 2 ? f16() : f32(); }
  bool cond(int cc) const;

  int szFlags(uint32_t v, int sz) const;
  uint32_t add(uint32_t a, uint32_t b, int sz, bool carry);
  uint32_t sub(uint32_t a, uint32_t b, int sz, bool carry);
  uint32_t logic(int op, uint32_t a, uint32_t b, int sz);
  uint32_t alu(int op, uint32_t a, uint32_t b, int sz);
  uint32_t inc(uint32_t v, uint32_t n, int sz, bool flags);
  uint32_t dec(uint32_t v, uint32_t n, int sz, bool flags);
  uint32_t shift(int op, uint32_t v, int n, int sz);

  void exec(int op);
  void single(int op);
  void prefixed(int op);
  int64_t memMode(int mode, int sz);
  void srcOp(uint32_t a, int sz, int prefix);
  void blockLd(int prefix, int sz, int op);
  void blockCp(int prefix, int sz, int op);
  void dstOp(uint32_t a);
  void muldiv(int kind, int dc, uint32_t v, int sz);
  void regOp(int c, int sz, int prefix);
  void regOpX(int c, int sz, int prefix);
  void bitCF(int kind, int c, int sz, uint32_t v, int b);
  uint32_t getCR(int cr) const;
  void setCR(int cr, uint32_t v);

  TlcsBus& bus;
  int mode = 0;  // modo de endereçamento da instrução atual (estados extras)
};

}  // namespace indigo::ngp
