// CPU GI CP1610 do Intellivision: 8 registros de 16 bits (R6 = pilha, R7 = PC), instruções de 10 bits
// ("decles"), flags S Z O C, I (interrupção) e D (SDBD: próximo acesso lê dois bytes).
// Semântica e ciclos conferidos com o manual da GI e com o jzIntv (GPL-2.0-or-later; ver TERCEIROS.md).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::intv {

struct Cp1610Bus {
  virtual uint16_t read(uint16_t a) = 0;
  virtual void write(uint16_t a, uint16_t v) = 0;
  virtual ~Cp1610Bus() = default;
};

class CP1610 {
public:
  explicit CP1610(Cp1610Bus& bus) : bus_(bus) {}

  void reset();
  // executa uma instrução; devolve os ciclos (de 894,886 kHz no NTSC)
  int step();
  // atende a interrupção: empilha o PC e pula para $1004 (12 ciclos)
  int interrupt();
  void sync(StateIO& io);

  uint16_t r[8] = {};
  bool S = false, Z = false, O = false, C = false, I = false;
  uint8_t D = 0;        // SDBD: 2 logo depois do prefixo, 1 durante a instrução seguinte
  uint8_t intr = 0;     // bit 0: pode atender interrupção agora; bit 1: pode ceder o barramento (BUSRQ)
  bool halted = false;  // HLT
  uint16_t lastPc = 0;  // endereço da última instrução (para depuração)

private:
  uint16_t rd(uint16_t a) { return bus_.read(a); }
  void wr(uint16_t a, uint16_t v) { bus_.write(a, v); }
  void setSZ(uint16_t v) { S = v & 0x8000; Z = v == 0; }
  uint16_t add(uint16_t a, uint16_t b, int c = 0);
  uint16_t sub(uint16_t d, uint16_t s);  // d - s
  int shift(uint16_t op);
  int memOp(uint16_t op);
  int branch(uint16_t op);

  Cp1610Bus& bus_;
};

}  // namespace indigo::intv
