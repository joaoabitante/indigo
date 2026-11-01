// Som do Master System / Game Gear (porte de src/sms/psg.js, SN76489): 3 quadradas + ruído, volume em
// passos de 2 dB; no Game Gear a porta 0x06 liga cada canal à esquerda/direita. Cada amostra é a média
// dos passos do chip (clock/16).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::sms {

class PSG {
public:
  PSG();
  void reset();
  void setSampleRate(int rate);
  void write(int v);
  void run(int64_t toCycle);
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);
  // depois de carregar um estado: o relógio do som anda junto com o da CPU (no máximo uma linha à frente)
  void clampClock(int64_t cpuCycles) {
    if (cycles > cpuCycles + 4096 || cycles < cpuCycles - (int64_t(1) << 20)) cycles = cpuCycles;
  }

  int stereo = 0xFF;

private:
  void emit();

  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL, bufR;
  size_t wpos = 0, rpos = 0;
  double ticksPerSample = 0;
  int tone[3] = {}, counter[4] = {}, out[4] = {}, vol[4] = {};
  int noise = 0, lfsr = 0x8000, latch = 0;
  int64_t cycles = 0;
  int sub = 0;
  double accL = 0, accR = 0, frac = 0, capL = 0, capR = 0;
  int accN = 0;
};

}  // namespace indigo::sms
