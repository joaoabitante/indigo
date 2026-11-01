// Capcom QSound (DL-1425, um DSP16 da AT&T com programa próprio) em HLE: imitação do programa do DSP feita
// por superctr e Valley Bell (MAME qsoundhle.cpp / libvgm qsound_ctr.c, BSD-3-Clause), reescrita aqui.
// 16 vozes de PCM de 8 bits com taxa, laço e volume, 3 vozes de ADPCM, eco, panorâmica com componentes "seca" e
// "molhada", filtros FIR de 95 coeficientes (o efeito Q1) e linhas de atraso. As tabelas (panorâmica, filtros,
// passo do ADPCM) são as que esses autores tiraram da ROM do DSP; a ROM do DSP não é necessária.
// Taxa: 60 MHz / 2 / 1248 = 24038 Hz.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::cps {

class QSound {
public:
  QSound();
  void reset();
  void write(int port, uint8_t v);  // portas 0-2 do Z80 (0xD000-0xD002)
  uint8_t read() const { return ready_; }
  void setRom(const std::vector<uint8_t>* rom) { rom_ = rom; }
  void sample(int& L, int& R);
  void sync(StateIO& io);

private:
  struct Voice { uint16_t bank = 0; int16_t addr = 0; uint16_t phase = 0, rate = 0; int16_t loopLen = 0, endAddr = 0, volume = 0, echo = 0; };
  struct Adpcm { uint16_t start = 0, end = 0, bank = 0; int16_t volume = 0; uint16_t flag = 0; int16_t curVol = 0, stepSize = 0; uint16_t cur = 0; };
  struct Fir { int taps = 0, pos = 0; uint16_t tablePos = 0; int16_t coef[95] = {}; int16_t line[95] = {}; };
  struct Delay { int16_t delay = 0, volume = 0, wpos = 0, rpos = 0; int16_t line[51] = {}; };
  struct Echo { uint16_t endPos = 0; int16_t feedback = 0, length = 0, last = 0; int16_t line[1024] = {}; int16_t pos = 0; };

  void writeData(uint8_t reg, uint16_t v);
  uint16_t* regPtr(uint8_t reg);
  int16_t romSample(uint16_t bank, uint16_t addr) const;
  int16_t pcmUpdate(int v, int32_t& echoIn);
  void adpcmUpdate(int v, int nibble);
  int16_t echoApply(int32_t in);
  static int32_t fir(Fir& f, int16_t in);
  static int32_t delay(Delay& d, int32_t in);
  static void delayUpdate(Delay& d);
  void stateInit();
  void refresh(int mode);
  void normalUpdate();

  const std::vector<uint8_t>* rom_ = nullptr;
  uint16_t latch_ = 0;
  uint8_t ready_ = 0;
  Voice voice_[16];
  Adpcm adpcm_[3];
  uint16_t pan_[19] = {};
  int16_t vout_[19] = {};
  Echo echo_;
  Fir filter_[2], alt_[2];
  Delay wet_[2], dry_[2];
  uint16_t state_ = 0, next_ = 0, delayUpd_ = 0;
  int counter_ = 0;
  int16_t out_[2] = {};
  int16_t panTab_[2][2][98] = {};
};

}  // namespace indigo::cps
