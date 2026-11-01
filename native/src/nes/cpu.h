// CPU do NES (Ricoh 2A03: um 6502 NMOS sem o modo decimal). Cada acesso ao barramento é um ciclo, na mesma
// ordem do chip de verdade (inclusive as leituras "falsas" dos modos indexados, que mexem com os registradores
// do PPU e do controle). Instruções ilegais estáveis e instáveis implementadas; JAM trava a CPU.
// Interrupções conferidas no penúltimo ciclo de cada instrução (o NMI pode "sequestrar" um BRK/IRQ).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::nes {

class CpuBus {
public:
  virtual ~CpuBus() = default;
  virtual uint8_t read(uint16_t addr) = 0;
  virtual void write(uint16_t addr, uint8_t v) = 0;
};

class CPU {
public:
  enum : uint8_t { C = 1, Z = 2, I = 4, D = 8, B = 0x10, U = 0x20, V = 0x40, N = 0x80 };

  CpuBus* bus = nullptr;
  bool decimalMode = false;  // false no NES; true só nos testes do 6502 genérico
  uint8_t magic = 0xFF;      // constante das instruções instáveis XAA/LXA (0xFF no 2A03; 0xEE nos testes do 6502)
  uint16_t pc = 0;
  uint8_t a = 0, x = 0, y = 0, s = 0xFD, p = 0x24;
  bool jammed = false;

  // linhas de interrupção (atualizadas no fim de cada ciclo pelo barramento)
  bool needNmi = false, prevNeedNmi = false, prevNmiLine = false;
  bool runIrq = false, prevRunIrq = false;
  void endCycle(bool nmiLine, bool irqLine) {
    prevNeedNmi = needNmi;
    if (!prevNmiLine && nmiLine) needNmi = true;
    prevNmiLine = nmiLine;
    prevRunIrq = runIrq;
    runIrq = irqLine && !(p & I);
  }

  void powerOn();
  void reset();    // sequência de reset do chip (7 ciclos, lê o vetor em $FFFC)
  void step();     // uma instrução (e a interrupção pendente, se houver)
  void sync(StateIO& io);

private:
  uint8_t rd(uint16_t a) { return bus->read(a); }
  void wr(uint16_t a, uint8_t v) { bus->write(a, v); }
  uint8_t fetch() { return rd(pc++); }
  void push(uint8_t v) { wr(0x100 | s, v); s--; }
  uint8_t pull() { s++; return rd(0x100 | s); }
  void setZN(uint8_t v) { p = (p & ~(Z | N)) | (v ? 0 : Z) | (v & N); }
  void interrupt();
  void endStep();
  void branch(bool cond);
  void adc(uint8_t v);
  void sbc(uint8_t v);
  void cmp(uint8_t r, uint8_t v);
  uint16_t storeUnstable(uint16_t base, uint8_t idx, uint8_t value, bool dummy);
};

}  // namespace indigo::nes
