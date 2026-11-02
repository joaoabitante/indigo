// POKEY (Atari C012294), comum ao Atari 7800 (no cartucho) e ao Atari 5200 (no console): 4 canais de som com
// os polinômios de 4, 5, 9 e 17 bits, temporizadores com IRQ, RANDOM e, no 5200, teclado (KBCODE), botão de
// baixo (como o Shift/Break dos computadores) e os potenciômetros (joystick analógico).
// O som é tratado por eventos (cada canal sabe o ciclo do próximo pulso), sem passo por ciclo.
// Tempo em ciclos da CPU (1,79 MHz).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::atari {

// sequências pseudoaleatórias (registrador de deslocamento de n bits), geradas uma vez
struct Polys {
  std::vector<uint8_t> p4, p5, p9, p17;
  int d31a = 0, d31b = 13;  // os dois pontos do divisor por 31 da TIA (13 e 18 passos)
  Polys();
};
const Polys& polys();

class Pokey {
public:
  void reset();
  void write(int r, uint8_t v, uint64_t now);
  uint8_t read(int r, uint64_t now);
  uint64_t nextEvent() const;  // ciclo do próximo pulso de canal
  void fire(uint64_t now);     // processa os pulsos que vencem em "now"
  int level() const;           // 0..60
  void sanitize(uint64_t now); // depois de ler um estado
  bool irqEnabled() const { return irqen & irqMask(); }
  bool irq() const { return (~irqst & irqen & irqMask()) != 0; }
  void sync(StateIO& io);

  // ---- entradas do 5200 (ext = true liga teclado, potenciômetros e as IRQ de tecla e de Break)
  bool ext = false;
  uint8_t pot[8] = {228, 228, 228, 228, 228, 228, 228, 228};  // posição de cada potenciômetro (0..228)
  // tecla (código KBCODE de 6 bits, -1 = nenhuma) e o segundo botão (Shift); chamado uma vez por quadro
  void keyboard(int code, bool shift);
  void syncExt(StateIO& io);  // estado das entradas (só o 5200 grava)

private:
  uint8_t irqMask() const { return ext ? 0xC7 : 0x07; }
  void recalc(uint64_t now);
  uint8_t audf[4] = {}, audc[4] = {}, audctl = 0, skctl = 0, irqen = 0, irqst = 0xFF;
  uint32_t period[4] = {1, 1, 1, 1};
  uint64_t next[4] = {};
  uint8_t out[4] = {}, hp[2] = {};
  bool active[4] = {};
  // 5200
  uint8_t kbcode = 0xFF, skstat = 0xFF, potLatch[8] = {}, lastKey = 0xFF;
  uint64_t potStart = 0;
  bool potRun = false, lastShift = false, keyBit5 = false, keyDown = false;
};

}  // namespace indigo::atari
