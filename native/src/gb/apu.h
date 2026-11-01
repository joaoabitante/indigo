// Som do Game Boy / Game Boy Color (porte de src/gb/apu.js): 2 quadradas (a 1ª com sweep), onda
// programável e ruído; sinal integrado entre eventos e passa-alta como o capacitor de saída.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::gb {

class GB;

struct Channel {
  bool on = false, dac = false;
  int len = 0;
  bool lenEn = false;
  int maxLen = 64;
  int vol = 0, envInit = 0;
  bool envUp = false;
  int envPer = 0, envTimer = 0;
  int freq = 0, timer = 1, pos = 0, duty = 0;
  int sweepPer = 0;
  bool sweepNeg = false;
  int sweepShift = 0, sweepTimer = 0;
  bool sweepEn = false;
  int shadow = 0;
  bool negUsed = false;
  int volCode = 0, sample = 0;
  int shift = 0;
  bool width7 = false;
  int div = 0, lfsr = 0x7FFF;
};

class APU {
public:
  explicit APU(GB& gb);

  void setSampleRate(int rate);
  void reset();
  int read(int a);
  void write(int a, int v);
  void frameStep();
  void catchUp();
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* outL, float* outR, size_t n);

  void sync(StateIO& io);

private:
  void writeEnv(Channel& ch, int v);
  void writeControl(int k, Channel& ch, int v);
  bool waveJustRead() const;
  int period(int k) const;
  int sweepCalc(Channel& ch);
  int digital(int k) const;
  void mix();
  void emit();
  void resetChannels();

  GB& gb;
  uint8_t regs[0x20] = {};
  uint8_t wave[16] = {};
  Channel c[4];
  bool power = true;
  int seqStep = 0;

  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL, bufR;
  size_t wpos = 0, rpos = 0;

  int sampleRate = 48000;
  double dotsPerSample = 0, charge = 0;
  bool timeInit = false;
  int64_t time = 0, sampleBase = 0, sampleIdx = 0, nextSampleAt = 0, lastSampleAt = 0;
  double accL = 0, accR = 0, capL = 0, capR = 0, curL = 0, curR = 0;
};

}  // namespace indigo::gb
