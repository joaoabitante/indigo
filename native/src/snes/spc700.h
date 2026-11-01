// Processador de som do SNES: Sony SPC700 (dentro do S-SMP). 8 bits, 64 KB de memória própria, página direta em
// $00xx ou $01xx (flag P), pilha em $01xx. Cada acesso é um ciclo, na ordem do chip, inclusive as leituras
// "falsas" (que zeram os contadores dos timers quando caem em $FD–$FF) e os ciclos internos (idle).
// A régua são os SingleStepTests do SPC700.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::snes {

class SpcBus {
public:
  virtual ~SpcBus() = default;
  virtual uint8_t read(uint16_t addr) = 0;
  virtual void write(uint16_t addr, uint8_t v) = 0;
  virtual void idle() = 0;
};

class SPC700 {
public:
  enum : uint8_t { C = 1, Z = 2, I = 4, H = 8, B = 0x10, P = 0x20, V = 0x40, N = 0x80 };
  SpcBus* bus = nullptr;
  uint16_t pc = 0xFFC0;
  uint8_t a = 0, x = 0, y = 0, sp = 0xEF, psw = 0x02;
  bool halted = false;  // SLEEP e STOP

  void step();  // uma instrução
  void sync(StateIO& io);

private:
  uint8_t rd(uint16_t a) { return bus->read(a); }
  void wr(uint16_t a, uint8_t v) { bus->write(a, v); }
  void idle() { bus->idle(); }
  uint8_t fetch() { return rd(pc++); }
  uint16_t fetch16() { const uint8_t lo = fetch(); return static_cast<uint16_t>(lo | (fetch() << 8)); }
  void dummy() { rd(pc); }  // leitura falsa do byte seguinte
  uint16_t dp(uint8_t off) const { return static_cast<uint16_t>(((psw & P) ? 0x100 : 0) | off); }
  uint8_t ld(uint8_t off) { return rd(dp(off)); }
  void st(uint8_t off, uint8_t v) { wr(dp(off), v); }
  void push(uint8_t v) { wr(static_cast<uint16_t>(0x100 | sp), v); sp--; }
  uint8_t pull() { sp++; return rd(static_cast<uint16_t>(0x100 | sp)); }
  void setZN(uint8_t v) { psw = static_cast<uint8_t>((psw & ~(Z | N)) | (v ? 0 : Z) | (v & N)); }
  void setZN16(uint16_t v) { psw = static_cast<uint8_t>((psw & ~(Z | N)) | (v ? 0 : Z) | ((v >> 8) & N)); }

  uint8_t alu(int kind, uint8_t a, uint8_t b);  // 0 OR 1 AND 2 EOR 3 CMP 4 ADC 5 SBC (resultado; CMP devolve a)
  uint8_t adc(uint8_t a, uint8_t b);
  uint8_t shift(int kind, uint8_t v);           // 0 ASL 1 ROL 2 LSR 3 ROR 4 DEC 5 INC
  void branch(bool cond);
  void execute(uint8_t op);
};

}  // namespace indigo::snes
