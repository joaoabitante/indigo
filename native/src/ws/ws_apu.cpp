// Som do WonderSwan: 4 canais de tabela de onda (32 amostras de 4 bits na RAM, frequência 3,072 MHz/(2048-p)),
// canal 2 como voz PCM de 8 bits, canal 3 com sweep (a cada 8192 ciclos), canal 4 com ruído (LFSR de 15 bits).
// Uma amostra a cada 128 ciclos (24 kHz). No Color: DMA de som (4/6/12/24 kHz) e Hyper Voice (PCM de 8 bits
// estéreo). A saída usada é a do fone (estéreo); o alto-falante mono só quando o jogo desliga o fone.
#include "ws.h"

namespace indigo::ws {

void WS::setSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 192000) rate = 192000;
  hostRate_ = rate;
}

void WS::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (ar_ == aw_) { l[k] = 0; r[k] = 0; continue; }
    l[k] = abL_[ar_]; r[k] = abR_[ar_];
    ar_ = (ar_ + 1) % ABUF;
  }
}

void WS::apuRun() {
  while (apuClock_ + 128 <= cpu.cycles) {
    apuClock_ += 128;
    apuSample();
  }
}

void WS::hyperWrite(uint8_t v) {
  if (hvChannel_) { hvRight_ = v; hvRightChanged_ = true; }
  else { hvLeft_ = v; hvLeftChanged_ = true; }
}

void WS::soundDma() {
  if (!colorHw || !(sdmaCtrl_ & 0x80)) return;
  static const int div[4] = {6, 4, 2, 1};
  if (++sdmaClock_ < div[sdmaCtrl_ & 3]) return;
  sdmaClock_ = 0;
  uint8_t d = 0;
  const bool hold = (sdmaCtrl_ & 4) != 0;
  if (!hold) {
    d = memRead(sdmaSrc_);
    sdmaSrc_ = (sdmaSrc_ + ((sdmaCtrl_ & 0x40) ? 0xFFFFF : 1)) & 0xFFFFF;
  }
  if (!(sdmaCtrl_ & 0x10)) {
    ch_[1].volR = d & 15;
    ch_[1].volL = d >> 4;
  } else {
    const int m = (hyperChan_ >> 5) & 3;
    if (m == 0) { hyperWrite(d); hvChannel_ = !hvChannel_; }
    if (m == 1 || m == 3) { hvChannel_ = false; hyperWrite(d); }
    if (m == 2 || m == 3) { hvChannel_ = true; hyperWrite(d); }
  }
  if (hold) return;
  sdmaLen_ = (sdmaLen_ - 1) & 0xFFFFF;
  if (sdmaLen_) return;
  if (sdmaCtrl_ & 8) { sdmaSrc_ = sdmaSrcReload_; sdmaLen_ = sdmaLenReload_; }
  else sdmaCtrl_ &= 0x7F;
}

void WS::apuSample() {
  soundDma();
  // avança cada canal 128 ciclos: o contador de 11 bits desce até igualar o "pitch", então vai para a
  // próxima amostra da onda
  for (int c = 0; c < 4; c++) {
    if (!(sndCtrl_ & (1 << c))) continue;
    Channel& ch = ch_[c];
    int n = 128;
    int k = (ch.period - ch.pitch) & 2047;
    if (!k) k = 2048;
    while (n >= k) {
      n -= k;
      ch.period = 0;
      ch.offset = (ch.offset + 1) & 31;
      if (c == 3 && (noiseCtrl_ & 0x10)) {
        static const int taps[8] = {14, 10, 13, 4, 8, 6, 9, 11};
        const int tap = taps[noiseCtrl_ & 7];
        noiseOut_ = ((1 ^ (noiseLfsr_ >> 7) ^ (noiseLfsr_ >> tap)) & 1) != 0;
        noiseLfsr_ = static_cast<uint16_t>(((noiseLfsr_ << 1) | (noiseOut_ ? 1 : 0)) & 0x7FFF);
      }
      k = (0 - ch.pitch) & 2047;
      if (!k) k = 2048;
    }
    ch.period = static_cast<uint16_t>((ch.period - n) & 2047);
  }
  sweepClock_ += 128;
  while (sweepClock_ >= 8192) {
    sweepClock_ -= 8192;
    if ((sndCtrl_ & 0x40) && --sweepCounter_ < 0) {
      sweepCounter_ = sweepTime_;
      ch_[2].pitch = static_cast<uint16_t>((ch_[2].pitch + sweepValue_) & 0x7FF);
    }
  }
  auto wave = [&](int c) -> int {
    const uint8_t d = iram_[((waveBase_ << 6) + (c << 4) + (ch_[c].offset >> 1)) & 0xFFFF];
    return (ch_[c].offset & 1) ? d >> 4 : d & 15;
  };
  int L = 0, R = 0;
  if (sndCtrl_ & 1) { const int s = wave(0); L += s * ch_[0].volL; R += s * ch_[0].volR; }
  if (sndCtrl_ & 0x20) {
    const int v = (ch_[1].volL << 4) | ch_[1].volR;
    L += (voiceCtrl_ & 8) ? v : (voiceCtrl_ & 4) ? v >> 1 : 0;
    R += (voiceCtrl_ & 2) ? v : (voiceCtrl_ & 1) ? v >> 1 : 0;
  } else if (sndCtrl_ & 2) { const int s = wave(1); L += s * ch_[1].volL; R += s * ch_[1].volR; }
  if (sndCtrl_ & 4) { const int s = wave(2); L += s * ch_[2].volL; R += s * ch_[2].volR; }
  if (sndCtrl_ & 8) {
    const int s = (sndCtrl_ & 0x80) ? (noiseOut_ ? 15 : 0) : wave(3);
    L += s * ch_[3].volL; R += s * ch_[3].volR;
  }
  outL_ = static_cast<uint16_t>(L & 0x3FF);
  outR_ = static_cast<uint16_t>(R & 0x3FF);

  // Hyper Voice
  if (colorHw && (hyperCtrl_ & 0x80)) {
    static const int div[8] = {1, 2, 3, 4, 5, 6, 8, 12};
    if (++hvClock_ >= div[(hyperCtrl_ >> 4) & 7]) {
      hvClock_ = 0;
      auto scale = [&](uint8_t s) -> int16_t {
        const int vol = hyperCtrl_ & 3;
        if (!vol) return static_cast<int16_t>(static_cast<int8_t>(s) * 256);
        switch ((hyperCtrl_ >> 2) & 3) {
          case 0: return static_cast<int16_t>(s << (8 - vol));
          case 1: return static_cast<int16_t>((s << (8 - vol)) - (0x10000 >> vol));
          case 2: return static_cast<int16_t>(static_cast<int8_t>(s) * (1 << (8 - vol)));
          default: return static_cast<int16_t>(static_cast<int8_t>(s) * 256);
        }
      };
      if (hvLeftChanged_) { hvOutL_ = scale(hvLeft_); hvLeftChanged_ = false; }
      if (hvRightChanged_) { hvOutR_ = scale(hvRight_); hvRightChanged_ = false; }
    }
  }

  double l = 0, r = 0;
  if (sndOut_ & 8) {
    l = L << 5; r = R << 5;
    if (colorHw && (hyperCtrl_ & 0x80)) { l += hvOutL_; r += hvOutR_; }
  } else if (sndOut_ & 1) {
    l = r = (((L + R) >> ((sndOut_ >> 1) & 3)) & 0xFF) << 7;
  }
  const double amp = static_cast<double>(masterVol_) / (colorHw ? 3.0 : 2.0) / 32768.0 * 0.8;
  l *= amp; r *= amp;
  // passa-alta: tira o nível contínuo (as ondas são só positivas)
  const double ol = l - hpL_, orr = r - hpR_;
  hpL_ += ol * 0.002; hpR_ += orr * 0.002;
  outPhase_ += hostRate_;
  while (outPhase_ >= 24000) {
    outPhase_ -= 24000;
    abL_[aw_] = static_cast<float>(ol);
    abR_[aw_] = static_cast<float>(orr);
    aw_ = (aw_ + 1) % ABUF;
    if (aw_ == ar_) ar_ = (ar_ + 1) % ABUF;
  }
}

}  // namespace indigo::ws
