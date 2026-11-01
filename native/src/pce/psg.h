// PSG do HuC6280: 6 canais de tabela de onda (32 amostras de 5 bits), modo DDA (amostra direta), ruído nos
// canais 5 e 6 (LFSR de 18 bits), LFO (o canal 2 modula a frequência do canal 1), volume geral e de cada canal
// em passos de 1,5 dB. Relógio de 3,58 MHz (mestre/6). A saída é integrada (média em caixa) na taxa pedida.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::pce {

class Psg {
public:
  void reset();
  void write(int reg, uint8_t v);
  void run(int64_t psgClocks);  // avança o PSG

  void setSampleRate(int rate);
  size_t available() const { return (aw_ + ABUF - ar_) % ABUF; }
  void pull(float* l, float* r, size_t n);
  void sync(StateIO& io);

private:
  struct Ch {
    uint16_t freq = 0;
    bool on = false, dda = false, noise = false;
    uint8_t amp = 0, lvol = 0, rvol = 0, waddr = 0, ddaVal = 0, noiseFreq = 0;
    uint8_t wave[32] = {};
    int8_t out = 0, noiseOut = 0;
    uint32_t lfsr = 1;
    int32_t timer = 0, noiseTimer = 0;
  };
  Ch ch_[6];
  uint8_t sel_ = 0, mainL_ = 0, mainR_ = 0, lfoFreq_ = 0, lfoCtrl_ = 0;
  int32_t outL_ = 0, outR_ = 0;
  double accL_ = 0, accR_ = 0, accN_ = 0, hpL_ = 0, hpR_ = 0;
  int hostRate_ = 48000;

  static constexpr size_t ABUF = 16384;
  std::vector<float> abL_ = std::vector<float>(ABUF), abR_ = std::vector<float>(ABUF);
  size_t aw_ = 0, ar_ = 0;

  bool lfoOn() const { return !(lfoCtrl_ & 0x80) && (lfoCtrl_ & 3); }
  int32_t period(int i) const;
  int32_t noisePeriod(const Ch& c) const { return c.noiseFreq == 0x1F ? 32 : ((~c.noiseFreq) & 0x1F) * 64; }
  void advance(int i, int32_t n);
  void mix();
  void emit(double l, double r);
};

}  // namespace indigo::pce
