// OKI MSM6295: 4 vozes de ADPCM de 4 bits (12 bits de saída), tabela de 128 frases no começo da ROM de
// amostras (3 bytes de início, 3 de fim), volume por voz. Taxa = relógio / 132 (pino 7 alto) ou / 165.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::cps {

class Oki6295 {
public:
  void reset();
  void write(uint8_t v);
  uint8_t read() const;
  int sample(const std::vector<uint8_t>& rom);  // próxima amostra (escala de 16 bits, já com o volume)
  void sync(StateIO& io);
  bool pin7 = true;  // alto = divisor 132

private:
  struct Voice {
    bool playing = false;
    uint32_t addr = 0, end = 0;  // em nibbles
    int sig = 0, step = 0, vol = 0;
  };
  Voice v_[4];
  int pendingPhrase_ = -1;
};

}  // namespace indigo::cps
