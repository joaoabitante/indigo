// Som do Atari 7800: os 2 canais da TIA (31,4 kHz). O POKEY do cartucho e as sequências pseudoaleatórias
// (polinômios de 4, 5, 9 e 17 bits) ficam em src/atari/pokey.cpp, comuns com o Atari 5200.
#include <vector>

#include "a7800.h"

namespace indigo::a7800 {

using atari::Polys;
namespace {
const Polys& P() { return atari::polys(); }
}  // namespace

// ------------------------------------------------------------------ TIA
void TiaSound::reset() {
  for (int c = 0; c < 2; c++) { audc[c] = audf[c] = audv[c] = 0; div[c] = 0; p4[c] = p5[c] = p9[c] = 0; bit[c] = 0; }
}

void TiaSound::write(int r, uint8_t v) {
  switch (r) {
    case 0x15: audc[0] = v & 15; break;
    case 0x16: audc[1] = v & 15; break;
    case 0x17: audf[0] = v & 31; break;
    case 0x18: audf[1] = v & 31; break;
    case 0x19: audv[0] = v & 15; break;
    case 0x1A: audv[1] = v & 15; break;
    default: break;
  }
}

void TiaSound::channel(int c) {
  const Polys& T = P();
  const uint8_t m = audc[c];
  if (m == 0 || m == 0x0B) { bit[c] = 1; return; }  // saída fixa: o volume vira nível
  p5[c] = static_cast<uint16_t>((p5[c] + 1) % 31);
  const bool clk = !(m & 2) || (!(m & 1) && (p5[c] == T.d31a || p5[c] == T.d31b)) || ((m & 1) && T.p5[p5[c]]);
  if (!clk) return;
  if (m & 4) bit[c] ^= 1;  // tom puro
  else if (m & 8) {
    if (m == 8) { p9[c] = static_cast<uint16_t>((p9[c] + 1) % 511); bit[c] = T.p9[p9[c]]; }
    else bit[c] = T.p5[p5[c]];
  } else {
    p4[c] = static_cast<uint16_t>((p4[c] + 1) % 15);
    bit[c] = T.p4[p4[c]];
  }
}

void TiaSound::tick() {
  for (int c = 0; c < 2; c++) {
    if (div[c]) { div[c]--; continue; }
    div[c] = static_cast<uint16_t>((audf[c] + 1) * ((audc[c] & 0x0C) == 0x0C ? 3 : 1) - 1);
    channel(c);
  }
}

void TiaSound::sync(StateIO& io) {
  io.raw(audc, sizeof audc); io.raw(audf, sizeof audf); io.raw(audv, sizeof audv);
  io.raw(div, sizeof div); io.raw(p4, sizeof p4); io.raw(p5, sizeof p5); io.raw(p9, sizeof p9); io.raw(bit, sizeof bit);
  if (io.reading()) {
    for (int c = 0; c < 2; c++) {
      audc[c] &= 15; audf[c] &= 31; audv[c] &= 15; bit[c] &= 1;
      div[c] = static_cast<uint16_t>(div[c] % 96);
      p4[c] = static_cast<uint16_t>(p4[c] % 15); p5[c] = static_cast<uint16_t>(p5[c] % 31);
      p9[c] = static_cast<uint16_t>(p9[c] % 511);
    }
  }
}

}  // namespace indigo::a7800
