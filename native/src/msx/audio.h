// Som do MSX: PSG AY-3-8910 (3 quadradas + ruído + envelope; as portas de E/S dele leem os joysticks),
// SCC da Konami (5 canais de forma de onda de 32 amostras, nos cartuchos Konami SCC) e o "clique" das
// teclas (bit 7 da porta C do PPI). Tudo anda a 1/16 do relógio da CPU (223,7 kHz) e é reduzido para a
// taxa pedida pela média das amostras (filtro de caixa) e um passa-alta que tira o nível contínuo.
// O MSX é mono: a mesma amostra sai nos dois lados.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::msx {

// AY-3-8910 (clock = CPU/2 = 1,79 MHz). Tom: f = clock/(16·TP); ruído: LFSR de 17 bits a clock/(16·NP);
// envelope de 16 passos com f = clock/(256·EP).
class AY8910 {
public:
  void reset();
  void writeAddr(int v) { addr = v & 0x0F; }
  void writeData(int v);
  int readData(int portA) const;  // portA = valor das entradas da porta A (registrador 14)
  int portB() const { return regs[15]; }
  float tick();  // um passo de clock/8 (CPU/16)
  void sync(StateIO& io);

private:
  uint8_t regs[16] = {};
  int addr = 0;
  int toneCnt[3] = {}, toneOut[3] = {};
  int noiseCnt = 0, noisePre = 0, rng = 1;
  int envCnt = 0, envStep = 0, envAttack = 0;
  bool envHold = false, envAlt = false, envHolding = false;
};

// SCC (Konami 051649): registradores em 9800h–98FFh (espelhados até 9FFFh) quando o banco 3 do mapeador
// recebe um valor com os 6 bits baixos em 1 (3Fh). f = CPU/(32·(P+1)).
class SCC {
public:
  void reset();
  int read(int addr) const;
  void write(int addr, int v);
  float tick();  // 16 ciclos de CPU
  void sync(StateIO& io);
  bool used = false;  // o jogo já mexeu no SCC (só então ele entra na mistura)

private:
  int8_t wave[5][32] = {};
  int period[5] = {}, volume[5] = {}, pos[5] = {}, count[5] = {};
  int enable = 0, deform = 0;
};

// mistura e reamostragem
class Mixer {
public:
  Mixer() : bufL(bufSize) { setSampleRate(48000); }
  void reset();
  void setSampleRate(int rate);
  void push(float v);  // uma amostra na taxa interna (CPU/16)
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);

private:
  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL;
  size_t wpos = 0, rpos = 0;
  double ticksPerSample = 0, frac = 0, acc = 0, cap = 0;
  int accN = 0;
};

}  // namespace indigo::msx
