// Base comum do ColecoVision e do SG-1000: Z80 a 3,58 MHz, vídeo TMS9918A e o SN76489, com o quadro NTSC
// de 262 linhas × 228 ciclos. Cada console só diz como o sinal de interrupção do vídeo chega à CPU
// (NMI no ColecoVision, INT no SG-1000) e como são a memória e as portas.
#pragma once
#include <cstdint>

#include "../core/state.h"
#include "../sms/z80.h"
#include "sound.h"
#include "tms9918.h"

namespace indigo::tms {

class Machine : public Z80Bus {
public:
  static constexpr int LINES = 262, CYCLES_PER_LINE = 228;
  static constexpr double FPS = 3579545.0 / (LINES * CYCLES_PER_LINE);

  Machine() : cpu(*this) {}
  ~Machine() override = default;

  Z80 cpu;
  TMS9918 vdp;
  Sound snd;
  int frame = 0;

  void runFrame() {
    for (int ln = 0; ln < LINES; ln++) {
      vdp.startLine(ln, cycles);
      updateIrq();
      int c = over;
      while (c < CYCLES_PER_LINE) {
        instrStart = cycles + c;
        c += cpu.step();
      }
      over = c - CYCLES_PER_LINE;
      cycles += CYCLES_PER_LINE;
    }
    instrStart = cycles + over;
    snd.run(cycles);
    frame++;
  }

protected:
  // relógio no momento de um acesso de E/S: o Z80 executa a instrução inteira de uma vez, e o acesso às
  // portas fica perto do fim dela (OUT (n),A: ciclo 8 de 11; OUTI: 13 de 16)
  int64_t now() const { return instrStart + 8; }
  virtual void updateIrq() = 0;

  void resetBase() {
    cpu.reset();
    vdp.reset();
    snd.reset();
    cycles = 0; instrStart = 0; over = 0; frame = 0;
  }
  void syncBase(StateIO& io) {
    cpu.sync(io);
    vdp.sync(io);
    snd.sync(io);
    io.sync(cycles); io.sync(over); io.sync(instrStart);
    if (io.reading()) {
      clampTo<int64_t>(cycles, 0, int64_t(1) << 50);
      clampTo(over, 0, 32);
      clampTo<int64_t>(instrStart, cycles - 64, cycles + 64);
      snd.clampClock(cycles);
      vdp.clampClock(cycles);
    }
  }

  int64_t cycles = 0;      // ciclo do começo da linha atual
  int64_t instrStart = 0;  // ciclo do começo da instrução atual
  int over = 0;
};

}  // namespace indigo::tms
