// Som FM do Mega Drive: Yamaha YM2612 (OPN2). 6 canais de 4 operadores, 8 algoritmos, realimentação, LFO
// (vibrato e tremolo), envelope ADSR com escala de tecla, SSG-EG, canal 3 com frequência por operador (modo
// especial e CSM), temporizadores A e B e o DAC de 8 bits no canal 6. Seno e exponencial em tabelas de
// logaritmo como no chip; uma amostra a cada 144 ciclos do relógio do 68000 (~53 kHz), reamostrada para a
// taxa de saída.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::md {

class YM2612 {
public:
  YM2612();
  void reset();
  void setRates(double chipRate, int outRate);  // chipRate = relógio / 144
  void write(int port, uint8_t v);              // port 0..3 (endereço/dado, parte I/II)
  uint8_t status() const { return static_cast<uint8_t>(status_); }
  void run(int64_t toClock);                    // avança até o ciclo (relógio do 68000) indicado
  void clampClock(int64_t c) { if (clock_ > c + 100000 || clock_ < c - 300000) clock_ = c; }
  size_t available() const { return (wpos_ + BUF - rpos_) % BUF; }
  void pull(float* L, float* R, size_t n, float* addTo = nullptr);
  void sync(StateIO& io);

  static constexpr size_t BUF = 16384;

private:
  struct Op {
    uint32_t phase = 0, inc = 0;
    int dt = 0, mul = 1, tl = 0, ks = 0, ar = 0, d1r = 0, d2r = 0, d1l = 0, rr = 0, ssg = 0, am = 0;
    int env = 0x3FF, state = 3;  // 0 ataque, 1 decaimento, 2 sustentação, 3 liberação
    bool key = false, ssgInv = false;
    int out = 0;
  };
  struct Ch {
    Op op[4];  // ordem dos registradores: S1, S3, S2, S4
    int fnum = 0, block = 0, kc = 0;
    int alg = 0, fb = 0, ams = 0, pms = 0;
    bool left = true, right = true;
    bool dirty = true;  // incremento de fase a recalcular
    int fb1 = 0, fb2 = 0;
    int out = 0;
  };

  void writeReg(int part, int r, uint8_t v);
  void keyOn(Ch& c, int slot, bool on);
  void sample();
  void updatePhaseInc(Ch& c, int ch);
  int opCalc(Op& o, int mod, int am);
  void egStep(Ch& c);
  int rateOf(const Op& o, int r, int kc) const;

  Ch ch_[6];
  int addr_[2] = {}, latchHi_[2] = {}, latch3Hi_ = 0;
  int ch3fnum_[3] = {}, ch3block_[3] = {}, ch3kc_[3] = {};
  int mode27_ = 0;
  int ta_ = 0, tb_ = 0, taCnt_ = 0, tbCnt_ = 0, tbSub_ = 0;
  int status_ = 0;
  int lfoOn_ = 0, lfoFreq_ = 0, lfoCnt_ = 0, lfoSub_ = 0;
  int dac_ = 0x80;
  bool dacOn_ = false;
  int egCnt_ = 0, egSub_ = 0;
  bool csmKey_ = false;
  int64_t clock_ = 0;
  int clkSub_ = 0;

  // reamostragem
  double chipRate_ = 53267, outRate_ = 48000, frac_ = 0;
  float prevL_ = 0, prevR_ = 0, curL_ = 0, curR_ = 0;
  std::vector<float> bufL_, bufR_;
  size_t wpos_ = 0, rpos_ = 0;
};

}  // namespace indigo::md
