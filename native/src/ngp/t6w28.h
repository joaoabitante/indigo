// Som do Neo Geo Pocket (porte de src/ngp/t6w28.js): T6W28 (parente do SN76489 com uma porta por lado do
// estéreo) + dois DACs de 8 bits. O chip anda a 3,072 MHz / 16.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::ngp {

class T6W28 {
public:
  T6W28();
  void reset();
  void setSampleRate(int rate);
  void writeLeft(int v);
  void writeRight(int v);
  void setEnabled(bool on);
  void run(int64_t toCycle);
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);
  // depois de carregar um estado: o relógio do som não pode ficar muito longe do da CPU (um estado danificado
  // com os dois afastados faria o som "alcançar" bilhões de ciclos de uma vez)
  void clampClock(int64_t cpuCycles) {
    if (cycles > cpuCycles || cycles < cpuCycles - (int64_t(1) << 22)) cycles = cpuCycles;
  }

  int dacL = 0x80, dacR = 0x80;

private:
  void emit();

  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL, bufR;
  size_t wpos = 0, rpos = 0;
  double ticksPerSample = 0;
  int tone[3] = {}, noiseTone = 0;
  int counter[4] = {}, out[4] = {}, volL[4] = {}, volR[4] = {};
  int noiseSel = 0;
  bool white = true;
  int lfsr = 0x4000, latchL = 0, latchR = 0;
  bool enabled = false;
  int64_t cycles = 0;
  int sub = 0;
  double accL = 0, accR = 0, frac = 0, capL = 0, capR = 0;
  int accN = 0;
};

}  // namespace indigo::ngp
