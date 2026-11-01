#include "oki6295.h"

namespace indigo::cps {

namespace {
const int STEPS[49] = {16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,  45,  50,  55,  60,  66,  73,
                       80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
                       408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552};
const int INDEX_ADJ[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
// atenuação de 3 dB por passo (multiplicador / 32)
const int VOLUME[16] = {0x20, 0x16, 0x10, 0x0b, 0x08, 0x06, 0x04, 0x03, 0x02, 0, 0, 0, 0, 0, 0, 0};
}  // namespace

void Oki6295::reset() {
  for (auto& v : v_) v = Voice();
  pendingPhrase_ = -1;
}

void Oki6295::write(uint8_t data) {
  if (pendingPhrase_ >= 0) {
    // segundo byte: vozes (bits 4-7) e volume (bits 0-3) da frase pedida
    const int phrase = pendingPhrase_;
    pendingPhrase_ = -1;
    for (int i = 0; i < 4; i++) {
      if (!(data & (0x10 << i))) continue;
      Voice& v = v_[i];
      if (v.playing) continue;  // voz ocupada: o pedido é ignorado
      // a tabela de frases é lida na hora de tocar (veja sample); aqui só guarda o pedido
      v.addr = static_cast<uint32_t>(phrase) << 24;  // marca: frase pendente no byte alto
      v.end = 0xFFFFFFFF;
      v.vol = VOLUME[data & 15];
      v.sig = -2;
      v.step = 0;
      v.playing = true;
    }
    return;
  }
  if (data & 0x80) {
    pendingPhrase_ = data & 0x7F;
    return;
  }
  for (int i = 0; i < 4; i++)
    if (data & (0x08 << i)) v_[i].playing = false;
}

uint8_t Oki6295::read() const {
  uint8_t r = 0xF0;
  for (int i = 0; i < 4; i++)
    if (v_[i].playing) r |= static_cast<uint8_t>(1 << i);
  return r;
}

int Oki6295::sample(const std::vector<uint8_t>& rom) {
  if (rom.empty()) return 0;
  const size_t mask = 0x3FFFF;  // 18 bits de endereço
  auto byteAt = [&](size_t a) -> uint8_t { a &= mask; return a < rom.size() ? rom[a] : 0; };
  int out = 0;
  for (auto& v : v_) {
    if (!v.playing) continue;
    if (v.end == 0xFFFFFFFF) {
      // começo da frase: endereços de 18 bits (em bytes) na tabela; convertidos para nibbles
      const size_t t = static_cast<size_t>(v.addr >> 24) * 8;
      const uint32_t start = ((byteAt(t) << 16) | (byteAt(t + 1) << 8) | byteAt(t + 2)) & 0x3FFFF;
      const uint32_t end = ((byteAt(t + 3) << 16) | (byteAt(t + 4) << 8) | byteAt(t + 5)) & 0x3FFFF;
      if (start >= end) { v.playing = false; continue; }
      v.addr = start * 2;
      v.end = end * 2 + 2;  // o byte final também toca
    }
    if (v.addr >= v.end) { v.playing = false; continue; }
    const uint8_t b = byteAt(v.addr >> 1);
    const int nib = (v.addr & 1) ? (b & 15) : (b >> 4);
    v.addr++;
    const int ss = STEPS[v.step];
    int diff = ss / 8 + ((nib & 4) ? ss : 0) + ((nib & 2) ? ss / 2 : 0) + ((nib & 1) ? ss / 4 : 0);
    if (nib & 8) diff = -diff;
    v.sig += diff;
    if (v.sig > 2047) v.sig = 2047;
    if (v.sig < -2048) v.sig = -2048;
    v.step += INDEX_ADJ[nib & 7];
    if (v.step < 0) v.step = 0;
    if (v.step > 48) v.step = 48;
    out += v.sig * v.vol / 2;
  }
  return out;
}

void Oki6295::sync(StateIO& io) {
  for (auto& v : v_) {
    io.sync(v.playing); io.sync(v.addr); io.sync(v.end); io.sync(v.sig); io.sync(v.step); io.sync(v.vol);
    if (io.reading()) { clampTo(v.sig, -2048, 2047); clampTo(v.step, 0, 48); clampTo(v.vol, 0, 32); }
  }
  io.sync(pendingPhrase_); io.sync(pin7);
  if (io.reading()) clampTo(pendingPhrase_, -1, 127);
}

}  // namespace indigo::cps
