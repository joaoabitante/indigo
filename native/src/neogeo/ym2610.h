// Som do Neo Geo: Yamaha YM2610 (OPNB) a 8 MHz.
//  - FM: 4 canais de 4 operadores (os canais 2, 3, 5 e 6 do OPN; os números 1 e 4 não existem), derivado do
//    YM2612 do Mega Drive (md/ym2612.cpp): mesmos 8 algoritmos, realimentação, LFO, envelope com SSG-EG,
//    canal 3 especial/CSM, temporizadores A e B (que geram a interrupção do Z80). Uma amostra a cada 144 ciclos
//    do chip (55 555 Hz).
//  - SSG: gerador de 3 ondas quadradas, ruído e envelope (compatível com o AY-3-8910/YM2149, envelope de 32
//    passos), mono nos dois lados.
//  - ADPCM-A: 6 canais de 4 bits a 18 518 Hz (8 MHz / 432) lidos da V ROM (endereços em blocos de 256 bytes),
//    volume total + volume por canal, esquerda/direita, bandeira de fim por canal.
//  - ADPCM-B: 1 canal de ADPCM Yamaha com taxa variável (delta-N), repetição, volume e interpolação linear.
// O relógio usado por run() é o do 68000 (12 MHz): 216 ciclos por amostra.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::neogeo {

class YM2610 {
public:
  YM2610();
  void reset();
  void setRom(const uint8_t* a, size_t na, const uint8_t* b, size_t nb);
  void setRates(double chipRate, int outRate);  // chipRate = relógio / 144
  void write(int port, uint8_t v);              // port 0..3 (endereço/dado, parte A/B)
  uint8_t read(int port);
  bool irq() const { return (status_ & irqMask()) != 0; }
  void run(int64_t toClock);                    // avança até o ciclo (relógio do 68000) indicado
  void clampClock(int64_t c) { if (clock_ > c + 100000 || clock_ < c - 300000) clock_ = c; }
  int64_t clock() const { return clock_; }
  size_t available() const { return (wpos_ + BUF - rpos_) % BUF; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);

  static constexpr size_t BUF = 16384;
  static constexpr int CYC_PER_SAMPLE = 216;  // ciclos do 68000 por amostra (144 ciclos do YM2610 a 8 MHz)

private:
  struct Op {
    uint32_t phase = 0, inc = 0;
    int dt = 0, mul = 1, tl = 0, ks = 0, ar = 0, d1r = 0, d2r = 0, d1l = 0, rr = 0, ssg = 0, am = 0;
    int env = 0x3FF, state = 3;  // 0 ataque, 1 decaimento, 2 sustentação, 3 liberação
    bool key = false, ssgInv = false;
  };
  struct Ch {
    Op op[4];  // ordem dos registradores: S1, S3, S2, S4
    int fnum = 0, block = 0, kc = 0;
    int alg = 0, fb = 0, ams = 0, pms = 0;
    bool left = true, right = true;
    bool dirty = true;
    int fb1 = 0, fb2 = 0;
  };
  struct AdpcmA {
    bool on = false, left = true, right = true;
    int level = 0;  // IL (0..31)
    uint32_t start = 0, end = 0, addr = 0;  // endereço em nibbles
    int acc = 0, step = 0;
  };

  int irqMask() const { return ((mode27_ >> 2) & 3); }
  void writeFm(int part, int r, uint8_t v);
  void writeSsg(int r, uint8_t v);
  void writeAdpcmA(int r, uint8_t v);
  void writeAdpcmB(int r, uint8_t v);
  void keyOn(Ch& c, int slot, bool on);
  void sample();
  int fmSample(int& L, int& R);
  int ssgSample();
  void adpcmASample();
  void adpcmBStep();
  void updatePhaseInc(Ch& c, int ch);
  int opCalc(Op& o, int mod, int am);
  void egStep(Ch& c);
  int rateOf(const Op& o, int r, int kc) const;
  uint8_t romA(uint32_t a) const { return a < sizeA_ ? romA_[a] : 0; }
  uint8_t romB(uint32_t a) const { return a < sizeB_ ? romB_[a] : 0; }

  // FM
  Ch ch_[6];
  int addr_[2] = {}, latchHi_[2] = {}, latch3Hi_ = 0;
  int ch3fnum_[3] = {}, ch3block_[3] = {}, ch3kc_[3] = {};
  int mode27_ = 0;
  int ta_ = 0, tb_ = 0, taCnt_ = 0, tbCnt_ = 0, tbSub_ = 0;
  int status_ = 0;
  int lfoOn_ = 0, lfoFreq_ = 0, lfoCnt_ = 0, lfoSub_ = 0;
  int egCnt_ = 0, egSub_ = 0;
  bool csmKey_ = false;

  // SSG
  uint8_t ssgReg_[16] = {};
  int toneCnt_[3] = {}, toneOut_[3] = {};
  int noiseCnt_ = 0, noiseSub_ = 0;
  uint32_t noiseLfsr_ = 1;
  int envCnt_ = 0, envStep_ = 0, envSub_ = 0;
  bool envHold_ = false, envAlt_ = false, envAtt_ = false, envDone_ = false;
  int ssgAcc_ = 0;  // fração de passos de 250 kHz por amostra

  // ADPCM-A
  AdpcmA a_[6];
  int aTotal_ = 0, aSub_ = 0, aOutL_ = 0, aOutR_ = 0;
  uint8_t aReg_[0x30] = {};
  // ADPCM-B
  uint8_t bReg_[0x10] = {};
  bool bOn_ = false;
  uint32_t bAddr_ = 0, bPos_ = 0;
  int bAcc_ = 0, bPrev_ = 0, bStep_ = 127;
  uint8_t flagMask_ = 0, eos_ = 0;

  int64_t clock_ = 0;
  int clkSub_ = 0;

  const uint8_t* romA_ = nullptr;
  size_t sizeA_ = 0;
  const uint8_t* romB_ = nullptr;
  size_t sizeB_ = 0;

  // reamostragem
  double chipRate_ = 55555.5, outRate_ = 48000, frac_ = 0;
  float curL_ = 0, curR_ = 0;
  std::vector<float> bufL_, bufR_;
  size_t wpos_ = 0, rpos_ = 0;
};

}  // namespace indigo::neogeo
