// Os 3 timers (1F801100..1F80112F): contador de 16 bits, alvo, modos de sincronia com hblank/vblank e as fontes
// de relógio (sistema, pontos da GPU, hblank, sistema/8). Contagem preguiçosa: avançam até sys.now quando lidos,
// escritos ou quando o escalonador chega ao próximo alvo.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::ps1 {

class System;

class Timers {
public:
  explicit Timers(System& s) : sys_(s) {}
  void reset();
  uint32_t read(uint32_t off);
  void write(uint32_t off, uint32_t v);
  void sync();                   // avança até sys.now
  uint64_t nextEvent() const;    // ciclo da próxima interrupção possível
  void hblank();                 // início de hblank (uma vez por linha)
  void vblank(bool on);          // entrada e saída do vblank
  void sync(StateIO& io);

private:
  struct T {
    uint32_t counter, mode, target;
    bool paused, irqDone;
  };
  System& sys_;
  T t_[3] = {};
  uint64_t last_ = 0;
  uint64_t dotFrac_ = 0;         // relógio de pontos: fração em 1/2^32 de ponto
  bool inVblank_ = false;

  void add(int i, uint32_t n);
  void irq(int i);
  bool countsSys(int i) const;   // conta ciclos do sistema (ou do sistema/8)
  bool sysClock(int i) const;    // conta cada ciclo do sistema
  static constexpr uint32_t HOLD = 0x8000;  // no modo (fora do registrador): 0 recém-voltado do alvo
};

}  // namespace indigo::ps1
