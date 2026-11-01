// Yamaha YM2151 (OPM), o FM do CPS1: 8 canais de 4 operadores, 8 algoritmos, realimentação, desafinação DT1 e
// DT2, LFO com 4 formas de onda (serra, quadrada, triângulo, ruído) para vibrato e tremolo, ruído no último
// operador do canal 8, temporizadores A e B com interrupção para o Z80. Seno e exponencial em tabelas de
// logaritmo como no chip (o mesmo esquema do YM2612 do Mega Drive); uma amostra a cada 64 ciclos do relógio
// (3,579545 MHz / 64 = 55,93 kHz).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::cps {

class YM2151 {
public:
  YM2151();
  void reset();
  void write(int port, uint8_t v);  // port 0 = endereço, 1 = dado
  uint8_t status() const { return static_cast<uint8_t>(status_); }
  bool irq() const { return (status_ & 3) != 0; }
  // gera uma amostra (saída esquerda/direita em 16 bits com sinal)
  void sample(int& L, int& R);
  void sync(StateIO& io);

private:
  struct Op {
    uint32_t phase = 0, inc = 0;
    int dt1 = 0, dt2 = 0, mul = 1, tl = 0, ks = 0, ar = 0, d1r = 0, d2r = 0, d1l = 0, rr = 0, amEn = 0;
    int env = 0x3FF, state = 3;  // 0 ataque, 1 decaimento, 2 sustentação, 3 liberação
    bool key = false;
  };
  struct Ch {
    Op op[4];  // ordem dos registradores: M1, M2, C1, C2
    int kc = 0, kf = 0, con = 0, fb = 0, pms = 0, ams = 0;
    bool left = false, right = false;
    int fb1 = 0, fb2 = 0;
    bool dirty = true;  // incrementos de fase a recalcular (não vai no estado: sai igual dos registradores)
    int lastPm = 0;
  };

  void writeReg(int r, uint8_t v);
  void keyOn(Ch& c, int slot, bool on);
  void updatePhase(Ch& c, int pmDelta);
  int opCalc(Op& o, int mod, int am);
  int envLevel(const Op& o, int am) const;
  void egStep(Ch& c);
  int rateOf(const Op& o, int r, int kc) const;
  int lfoTick();

  Ch ch_[8];
  int addr_ = 0;
  int ta_ = 0, tb_ = 0, taCnt_ = 0, tbCnt_ = 0, tbSub_ = 0, ctl14_ = 0, status_ = 0;
  int lfrq_ = 0, pmd_ = 0, amd_ = 0, wave_ = 0, test_ = 0;
  uint32_t lfoCnt_ = 0;
  int lfoAm_ = 0;
  int noiseEn_ = 0, noiseFreq_ = 0, noiseCnt_ = 0, noiseState_ = 0;
  uint32_t noiseLfsr_ = 1;
  int egCnt_ = 0, egSub_ = 0;
};

}  // namespace indigo::cps
