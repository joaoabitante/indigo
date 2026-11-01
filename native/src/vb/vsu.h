// Som do Virtual Boy (VSU): 5 canais de onda (tabelas de 32 amostras de 6 bits) e 1 de ruído (LFSR com 8
// derivações), volume esquerdo/direito, envelope, duração e, no canal 5, varredura ou modulação de frequência.
// O chip roda a 5 MHz (CPU / 4) e gera uma amostra a cada 120 ciclos dele (41 667 Hz); aqui a mesma amostragem
// é feita e depois convertida para a taxa pedida por interpolação linear, com um filtro que tira o nível contínuo.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::vb {

class VSU {
public:
  VSU();
  void reset();
  void setSampleRate(int rate);
  void write(uint32_t a, uint8_t v, int64_t ts);  // ts em ciclos da CPU
  void run(int64_t ts);
  size_t available() const { return (wpos_ + BUF - rpos_) % BUF; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);
  void clampClock(int64_t ts) { if (lastTs_ > ts || lastTs_ < ts - (int64_t(1) << 22)) lastTs_ = ts; }

private:
  void tick();       // 120 ciclos do VSU = uma amostra
  void effects(int ch);
  void emit(double l, double r);

  static constexpr size_t BUF = 16384;
  std::vector<float> bufL_, bufR_;
  size_t wpos_ = 0, rpos_ = 0;

  uint8_t wave_[5][32] = {}, mod_[32] = {};
  uint8_t intl_[6] = {}, lvl_[6] = {}, ram_[6] = {}, sweep_ = 0;
  uint16_t freq_[6] = {}, env_[6] = {};
  int32_t effFreq_[6] = {}, envelope_[6] = {}, pos_[6] = {};
  int32_t freqCounter_[6] = {}, intervalCounter_[6] = {}, envCounter_[6] = {};
  int32_t effDiv_[6] = {}, intervalDiv_[6] = {}, envDiv_[6] = {};
  int32_t sweepCounter_ = 0, sweepDiv_ = 1, modPos_ = 0;
  uint32_t lfsr_ = 1;
  int64_t lastTs_ = 0;
  int32_t sub_ = 0;  // ciclos da CPU ainda não convertidos em amostra (0–479)
  double prevL_ = 0, prevR_ = 0, phase_ = 0, step_ = 1, capL_ = 0, capR_ = 0;
};

}  // namespace indigo::vb
