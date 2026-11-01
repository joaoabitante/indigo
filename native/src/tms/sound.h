// Som do ColecoVision e do SG-1000: SN76489AN da Texas (o chip avulso, diferente do embutido no vídeo do
// Master System: ruído de 15 bits com realimentação nos bits 0 e 1, período 0 = 1024) e, no Super Game
// Module do ColecoVision, um AY-3-8910 (3 quadradas, ruído de 17 bits, envelope de 16 passos) a 1,79 MHz.
// Os dois andam no mesmo passo (CPU/16 = 223,7 kHz); cada amostra é a média dos passos, em mono.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::tms {

class Sound {
public:
  Sound();
  void reset();
  void setSampleRate(int rate);
  void run(int64_t toCycle);  // avança até o ciclo de CPU indicado
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);
  void clampClock(int64_t cpuCycles) {
    if (cycles > cpuCycles + 4096 || cycles < cpuCycles - (int64_t(1) << 20)) cycles = cpuCycles;
  }

  // SN76489
  void snWrite(int v);
  // AY-3-8910 (Super Game Module)
  void ayAddress(int v) { ayLatch = v & 0x0F; }
  void ayWrite(int v);
  int ayRead() const { return ayReg[ayLatch]; }

private:
  void tick();
  void emit();

  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL;
  size_t wpos = 0, rpos = 0;
  double ticksPerSample = 0;
  int64_t cycles = 0;
  int sub = 0;
  double acc = 0, frac = 0, cap = 0;
  int accN = 0;

  // SN76489
  int tone[3] = {}, counter[4] = {}, out[4] = {}, vol[4] = {};
  int noise = 0, lfsr = 0x4000, latch = 0;

  // AY-3-8910
  uint8_t ayReg[16] = {};
  int ayLatch = 0;
  bool ayOn = false;  // algum registrador já foi escrito (senão nem calcula)
  int ayCnt[3] = {}, ayOut[3] = {}, ayNoiseCnt = 0, ayNoisePre = 0, ayRng = 1;
  int envCnt = 0, envPos = 0, envAttack = 0, envHoldVol = 0;
  bool envHold = false;
};

}  // namespace indigo::tms
