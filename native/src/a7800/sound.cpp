// Som do Atari 7800: os 2 canais da TIA (31,4 kHz) e o POKEY do cartucho (1,79 MHz). As sequências
// pseudoaleatórias (polinômios de 4, 5, 9 e 17 bits) são tabelas geradas uma vez; o POKEY é tratado por eventos
// (cada canal sabe o ciclo do próximo pulso), sem passo por ciclo.
#include <vector>

#include "a7800.h"

namespace indigo::a7800 {

namespace {
// registrador de deslocamento de n bits, realimentação pelos bits "tap" e 0
std::vector<uint8_t> lfsr(int bits, int tap) {
  const int len = (1 << bits) - 1;
  std::vector<uint8_t> out(static_cast<size_t>(len));
  uint32_t r = (1u << bits) - 1;
  for (int k = 0; k < len; k++) {
    out[static_cast<size_t>(k)] = r & 1;
    const uint32_t fb = (r ^ (r >> tap)) & 1;
    r = (r >> 1) | (fb << (bits - 1));
  }
  return out;
}
struct Polys {
  std::vector<uint8_t> p4 = lfsr(4, 1), p5 = lfsr(5, 2), p9 = lfsr(9, 4), p17 = lfsr(17, 5);
  int d31a = 0, d31b = 13;  // os dois pontos do divisor por 31 (13 e 18 passos)
  Polys() {
    for (int k = 0; k < 31; k++)
      if (p5[static_cast<size_t>(k)] != p5[static_cast<size_t>((k + 13) % 31)]) { d31a = k; d31b = (k + 13) % 31; break; }
  }
};
const Polys& P() { static const Polys p; return p; }
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

// ------------------------------------------------------------------ POKEY
void Pokey::reset() {
  for (int i = 0; i < 4; i++) { audf[i] = audc[i] = 0; out[i] = 0; next[i] = 1; active[i] = true; }
  hp[0] = hp[1] = 0;
  audctl = skctl = irqen = 0;
  irqst = 0xFF;
  recalc(0);
}

void Pokey::recalc(uint64_t now) {
  const uint32_t base = (audctl & 1) ? 114 : 28;
  bool act[4] = {true, true, true, true};
  for (int pair = 0; pair < 2; pair++) {
    const int lo = pair * 2, hi = lo + 1;
    const bool fast = audctl & (pair ? 0x20 : 0x40);
    if (audctl & (pair ? 0x08 : 0x10)) {  // dois canais juntos (16 bits)
      const uint32_t v = audf[hi] * 256u + audf[lo];
      period[hi] = fast ? v + 7 : (v + 1) * base;
      period[lo] = 1;
      act[lo] = false;
    } else {
      period[lo] = fast ? audf[lo] + 4u : (audf[lo] + 1u) * base;
      period[hi] = (audf[hi] + 1u) * base;
    }
  }
  for (int i = 0; i < 4; i++) {
    if (act[i] && !active[i]) next[i] = now + period[i];
    active[i] = act[i];
  }
}

void Pokey::write(int r, uint8_t v, uint64_t now) {
  r &= 15;
  switch (r) {
    case 0: case 2: case 4: case 6: audf[r >> 1] = v; recalc(now); break;
    case 1: case 3: case 5: case 7: audc[r >> 1] = v; break;
    case 8: audctl = v; recalc(now); break;
    case 9: for (int i = 0; i < 4; i++) next[i] = now + period[i]; break;  // STIMER
    case 0xE: irqen = v; irqst |= static_cast<uint8_t>(~v); break;
    case 0xF: skctl = v; break;
    default: break;
  }
}

uint8_t Pokey::read(int r, uint64_t now) {
  switch (r & 15) {
    case 0xA: {  // RANDOM: 8 bits do polinômio que está correndo
      if ((skctl & 3) == 0) return 0xFF;
      const Polys& T = P();
      uint8_t v = 0;
      for (int k = 0; k < 8; k++) {
        const uint64_t t = now + static_cast<uint64_t>(k);
        v = static_cast<uint8_t>((v << 1) | ((audctl & 0x80) ? T.p9[t % 511] : T.p17[t % 131071]));
      }
      return v;
    }
    case 0xE: return irqst;
    case 0xF: return 0xFF;  // SKSTAT
    case 0x8: return 0;     // ALLPOT
    default: return 0;      // potenciômetros: sem paddles
  }
}

uint64_t Pokey::nextEvent() const {
  uint64_t n = UINT64_MAX;
  for (int i = 0; i < 4; i++) if (active[i] && next[i] < n) n = next[i];
  return n;
}

void Pokey::fire(uint64_t now) {
  const Polys& T = P();
  for (int i = 0; i < 4; i++) {
    while (active[i] && next[i] <= now) {
      const uint64_t t = next[i];
      next[i] += period[i];
      static const uint8_t irqBit[4] = {1, 2, 0, 4};
      if (irqBit[i] & irqen) irqst &= static_cast<uint8_t>(~irqBit[i]);
      if ((skctl & 3) == 0) continue;  // POKEY parado (inicialização)
      const uint8_t c = audc[i];
      if ((c & 0x80) || T.p5[t % 31]) {
        if (c & 0x20) out[i] ^= 1;
        else if (c & 0x40) out[i] = T.p4[t % 15];
        else out[i] = (audctl & 0x80) ? T.p9[t % 511] : T.p17[t % 131071];
      }
      if (i == 2) hp[0] = out[0];  // canal 3 marca o filtro passa-alta do canal 1
      if (i == 3) hp[1] = out[1];
    }
  }
}

void Pokey::sanitize(uint64_t now) {
  for (int i = 0; i < 4; i++)
    if (active[i] && (next[i] <= now || next[i] > now + (1ull << 24))) next[i] = now + period[i];
}

int Pokey::level() const {
  int s = 0;
  for (int i = 0; i < 4; i++) {
    const int vol = audc[i] & 15;
    if (!vol) continue;
    if (audc[i] & 0x10) { s += vol; continue; }  // só volume
    uint8_t b = out[i];
    if (i == 0 && (audctl & 4)) b ^= hp[0];
    if (i == 1 && (audctl & 2)) b ^= hp[1];
    if (b) s += vol;
  }
  return s;
}

void Pokey::sync(StateIO& io) {
  io.raw(audf, sizeof audf); io.raw(audc, sizeof audc);
  io.sync(audctl); io.sync(skctl); io.sync(irqen); io.sync(irqst);
  io.raw(next, sizeof next); io.raw(out, sizeof out); io.raw(hp, sizeof hp);
  for (bool& a : active) io.sync(a);
  if (io.reading()) {
    for (int i = 0; i < 4; i++) out[i] &= 1;
    hp[0] &= 1; hp[1] &= 1;
    // períodos e canais ativos vêm dos registradores
    for (int i = 0; i < 4; i++) active[i] = true;
    recalc(0);
    active[0] = !(audctl & 0x10);
    active[2] = !(audctl & 0x08);
  }
}

}  // namespace indigo::a7800
