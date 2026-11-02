// Som do Intellivision: GI AY-3-8914 (mesmo núcleo do AY-3-8910 com outra ordem de registros), em $01F0-$01FF.
// Três tons, ruído e envelope; os registros 14 e 15 são as portas dos controles (lidas pelo Intv).
// O chip anda a cada 4 ciclos da CPU (223,7 kHz no NTSC): tom = 111860,8 / período Hz.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::intv {

class PSG {
public:
  void reset();
  void write(int reg, uint8_t v, int64_t now);
  uint8_t read(int reg) const { return reg < 14 ? r_[reg] : 0xFF; }
  void run(int64_t now);  // avança o som até o ciclo da CPU "now"

  void setSampleRate(int rate, double cpuHz);
  size_t available() const { return (wpos_ + BUF - rpos_) % BUF; }
  void pull(float* l, float* rr, size_t n);
  void sync(StateIO& io);
  void clampClock(int64_t now) { if (last_ > now || last_ < now - 4) last_ = now; }
  float level() const { return lastOut_; }  // para os testes
  int envLevel() const { return envVol_; }

  uint8_t r_[16] = {};

private:
  void tick();
  void push(float v);

  int64_t last_ = 0;      // ciclo da CPU até onde o som foi calculado (múltiplo de 4 desde o reset)
  int32_t cnt_[5] = {};   // contadores de tom A/B/C, envelope e ruído
  uint8_t chan_[3] = {};  // saída quadrada de cada tom
  uint32_t rng_ = 1;
  int32_t envCnt_ = 0;
  uint8_t envVol_ = 0;    // índice 0..15
  bool envHold_ = false, envAltr_ = false, envAtak_ = false, envCont_ = false;

  // saída: média dos ticks em cada amostra
  static constexpr size_t BUF = 16384;
  std::vector<float> buf_ = std::vector<float>(BUF, 0.0f);
  size_t wpos_ = 0, rpos_ = 0;
  double ticksPerSample_ = 223721.6 / 48000.0, frac_ = 0, acc_ = 0, dc_ = 0;
  int accN_ = 0;
  float lastOut_ = 0;
};

}  // namespace indigo::intv
