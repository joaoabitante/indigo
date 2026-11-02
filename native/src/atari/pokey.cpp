// POKEY comum ao 7800 e ao 5200 (veja pokey.h). O som e os temporizadores vieram do núcleo do 7800 sem mudança
// (o estado do 7800 continua o mesmo, byte a byte); o teclado e os potenciômetros só valem com ext = true.
#include "pokey.h"

namespace indigo::atari {

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
}  // namespace

Polys::Polys() : p4(lfsr(4, 1)), p5(lfsr(5, 2)), p9(lfsr(9, 4)), p17(lfsr(17, 5)) {
  for (int k = 0; k < 31; k++)
    if (p5[static_cast<size_t>(k)] != p5[static_cast<size_t>((k + 13) % 31)]) { d31a = k; d31b = (k + 13) % 31; break; }
}

const Polys& polys() { static const Polys p; return p; }

void Pokey::reset() {
  for (int i = 0; i < 4; i++) { audf[i] = audc[i] = 0; out[i] = 0; next[i] = 1; active[i] = true; }
  hp[0] = hp[1] = 0;
  audctl = skctl = irqen = 0;
  irqst = 0xFF;
  kbcode = 0xFF; skstat = 0xFF; lastKey = 0xFF;
  for (uint8_t& p : potLatch) p = 0;
  potStart = 0; potRun = false; lastShift = false; keyBit5 = false; keyDown = false;
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
    case 0xA: if (ext) skstat |= 0xE0; break;                                // SKRES
    case 0xB:                                                               // POTGO
      if (ext) { for (int i = 0; i < 8; i++) potLatch[i] = pot[i]; potStart = now; potRun = true; }
      break;
    case 0xE: irqen = v; irqst |= static_cast<uint8_t>(~v); break;
    case 0xF: skctl = v; break;
    default: break;
  }
}

uint8_t Pokey::read(int r, uint64_t now) {
  r &= 15;
  if (ext && r < 9) {  // potenciômetros: o contador sobe uma vez por linha (ou por ciclo no modo rápido)
    uint64_t e = 228;
    if (potRun) {
      e = (now - potStart) / ((skctl & 4) ? 1 : 114);
      if (e >= 228) { e = 228; potRun = false; }
    }
    if (r == 8) {  // ALLPOT: 1 = ainda contando
      uint8_t v = 0;
      for (int i = 0; i < 8; i++) if (e < potLatch[i]) v |= static_cast<uint8_t>(1 << i);
      return v;
    }
    return e < potLatch[r] ? static_cast<uint8_t>(e) : potLatch[r];
  }
  switch (r) {
    case 0x9: return ext ? kbcode : 0;
    case 0xA: {  // RANDOM: 8 bits do polinômio que está correndo
      if ((skctl & 3) == 0) return 0xFF;
      const Polys& T = polys();
      uint8_t v = 0;
      for (int k = 0; k < 8; k++) {
        const uint64_t t = now + static_cast<uint64_t>(k);
        v = static_cast<uint8_t>((v << 1) | ((audctl & 0x80) ? T.p9[t % 511] : T.p17[t % 131071]));
      }
      return v;
    }
    case 0xE: return irqst;
    case 0xF: return ext ? skstat : 0xFF;  // SKSTAT
    default: return 0;  // 7800: sem potenciômetros
  }
}

void Pokey::keyboard(int code, bool shift) {
  if (shift && !lastShift && (irqen & 0x80)) irqst &= 0x7F;  // botão de baixo: IRQ do Break
  lastShift = shift;
  skstat |= 0x0C;
  if (shift) skstat &= ~0x08;
  if (code < 0) { keyDown = false; keyBit5 = false; return; }
  // o 5200 só liga 4 das 6 linhas de varredura: o bit 5 alterna e a tecla presa gera um evento por quadro
  if (keyBit5) code &= ~0x20;
  keyBit5 = !keyBit5;
  if (shift) code |= 0xC0;
  skstat &= ~0x04;
  if (!keyDown || ((code ^ lastKey) & 0x3F)) {
    keyDown = true;
    lastKey = static_cast<uint8_t>(code);
    kbcode = static_cast<uint8_t>(code);
    if (irqen & 0x40) {
      if (irqst & 0x40) irqst &= ~0x40;
      else skstat &= ~0x40;  // tecla perdida (a anterior não foi atendida)
    }
  }
}

uint64_t Pokey::nextEvent() const {
  uint64_t n = UINT64_MAX;
  for (int i = 0; i < 4; i++) if (active[i] && next[i] < n) n = next[i];
  return n;
}

void Pokey::fire(uint64_t now) {
  const Polys& T = polys();
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
  if (potRun && (potStart > now || now - potStart > (1ull << 24))) potRun = false;
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

void Pokey::syncExt(StateIO& io) {
  io.sync(kbcode); io.sync(skstat); io.raw(potLatch, sizeof potLatch); io.sync(lastKey);
  io.sync(potStart); io.sync(potRun); io.sync(lastShift); io.sync(keyBit5); io.sync(keyDown);
}

}  // namespace indigo::atari
