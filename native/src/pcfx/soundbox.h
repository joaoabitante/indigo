// Som do PC-FX (HuC6230 "SoundBox"): o PSG do PC Engine (6 canais, src/pce/psg), dois canais de ADPCM que o
// KING lê da KRAM (passo de 31,47 kHz dividido por 1, 2, 4 ou 8, interpolação linear opcional, volume por
// lado em passos de 1,5 dB) e o volume do CD-DA. O resultado vai para a taxa pedida pela média de cada período.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"
#include "../pce/psg.h"
#include "king.h"
#include "scsicd.h"

namespace indigo::pcfx {

class SoundBox {
public:
  SoundBox(King& king, ScsiCd& cd) : king_(king), cd_(cd) {}
  void reset(int64_t now);
  void write(int64_t now, uint32_t a, uint16_t v);
  void run(int64_t t);
  int64_t nextTick() const { return nextTick_ / 2 + 1; }  // próximo passo do ADPCM

  void setSampleRate(int rate);
  size_t available() const;
  void pull(float* l, float* r, size_t n);
  void sync(StateIO& io);
  bool temposOk(int64_t agora) const;

  pce::Psg psg;
  // para os testes
  int32_t predictor(int ch) const { return pred_[ch]; }
  uint64_t adpcmNibbles = 0;

private:
  King& king_;
  ScsiCd& cd_;
  int64_t last_ = 0, psgClk_ = 0;
  int64_t nextTick_ = 0;   // em meios ciclos mestres
  uint16_t ctrl_ = 0;
  uint8_t vol_[2][2] = {}, cdVol_[2] = {};
  int32_t smallDiv_ = 0;
  int32_t delta_[2] = {}, haveDelta_[2] = {}, pred_[2] = {}, step_[2] = {};
  uint32_t nibble_[2] = {};
  uint16_t half_[2] = {};
  bool haveHalf_[2] = {};
  // saída na taxa do aparelho
  static constexpr size_t ABUF = 16384;
  std::vector<float> abL_ = std::vector<float>(ABUF), abR_ = std::vector<float>(ABUF);
  size_t aw_ = 0, ar_ = 0;
  int hostRate_ = 48000;
  double hostPeriod_ = 0, hostNext_ = 0;
  double outL_ = 0, outR_ = 0, accL_ = 0, accR_ = 0, accT_ = 0, lastT_ = 0;
  // CD-DA consumido em 44,1 kHz
  size_t cdPos_ = 0;
  double cdFrac_ = 0;
  int16_t cdL_ = 0, cdR_ = 0;

  void tick();
  void integrate(double upTo);
  void emitHost();
};

}  // namespace indigo::pcfx
