// Partes adaptadas do Mednafen (Beetle PC-FX, Mednafen Team, GPL-2.0 ou posterior); veja native/TERCEIROS.md.
#include "soundbox.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace indigo::pcfx {

namespace {
constexpr double MASTER_HZ = 236250000.0 / 11.0;
const int STEP[49] = {16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,  45,  50,  55,  60,  66,  73,
                      80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
                      408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552};
const int STEP_DELTA[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
// volume do ADPCM: passos de 1,5 dB; abaixo de 0x1C fica mudo
double adpcmVol(int v) { return v <= 0x1B ? 0.0 : std::pow(2.0, -(63 - v) / 4.0); }
}  // namespace

void SoundBox::reset(int64_t now) {
  psg.reset();
  last_ = psgClk_ = now;
  nextTick_ = now * 2 + 2;
  ctrl_ = 0;
  std::memset(vol_, 0, sizeof vol_);
  std::memset(cdVol_, 0, sizeof cdVol_);
  smallDiv_ = 0;
  for (int c = 0; c < 2; c++) {
    delta_[c] = haveDelta_[c] = pred_[c] = step_[c] = 0;
    nibble_[c] = 0; half_[c] = 0; haveHalf_[c] = false;
  }
  outL_ = outR_ = accL_ = accR_ = accT_ = 0;
  lastT_ = double(now);
  hostPeriod_ = MASTER_HZ / hostRate_;
  hostNext_ = double(now) + hostPeriod_;
  cdPos_ = 0; cdFrac_ = 0; cdL_ = cdR_ = 0;
}

void SoundBox::setSampleRate(int rate) {
  hostRate_ = rate > 0 ? rate : 48000;
  hostPeriod_ = MASTER_HZ / hostRate_;
  psg.setSampleRate(hostRate_);
}

void SoundBox::write(int64_t now, uint32_t a, uint16_t v) {
  run(now);
  a &= 0x3F;
  if (a < 0x20) { psg.write(int(a >> 1), uint8_t(v)); return; }
  switch (a) {
    case 0x20:
      for (int c = 0; c < 2; c++)
        if (!(ctrl_ & (0x10 << c)) && (v & (0x10 << c))) { pred_[c] = 0; step_[c] = 0; }  // reinício do canal
      ctrl_ = v;
      break;
    case 0x22: vol_[0][0] = v & 0x3F; break;
    case 0x24: vol_[0][1] = v & 0x3F; break;
    case 0x26: vol_[1][0] = v & 0x3F; break;
    case 0x28: vol_[1][1] = v & 0x3F; break;
    case 0x2A: cdVol_[0] = v & 0x3F; break;
    case 0x2C: cdVol_[1] = v & 0x3F; break;
    default: break;
  }
}

// um passo do ADPCM (a cada 682,5 ciclos mestres): decodifica um nibble quando o divisor manda e anda o preditor
void SoundBox::tick() {
  const uint16_t kc = king_.adpcmControl();
  const int rate = (kc >> 2) & 3;
  if (--smallDiv_ <= 0) {
    smallDiv_ += 1 << rate;
    for (int c = 0; c < 2; c++) {
      if (!(haveHalf_[c] || (king_.adpcmControl() & (1 << c)))) continue;
      if (!nibble_[c]) { half_[c] = king_.adpcmHalfWord(c); haveHalf_[c] = true; }
      if (ctrl_ & (0x10 << c)) {
        delta_[c] = 0;
      } else {
        const uint8_t nib = (half_[c] >> nibble_[c]) & 0xF;
        delta_[c] = STEP[step_[c]] * ((nib & 7) + 1);
        if (ctrl_ & (0x4 << c)) delta_[c] >>= rate;
        if (nib & 8) delta_[c] = -delta_[c];
        step_[c] = std::clamp(step_[c] + STEP_DELTA[nib], 0, 48);
        adpcmNibbles++;
      }
      haveDelta_[c] = (ctrl_ & (0x4 << c)) ? (1 << rate) : 1;
      nibble_[c] = (nibble_[c] + 4) & 0xF;
      if (!nibble_[c]) haveHalf_[c] = false;
    }
  }
  double l = 0, r = 0;
  for (int c = 0; c < 2; c++) {
    if (haveDelta_[c]) {
      pred_[c] = std::clamp(pred_[c] + delta_[c], -0x4000, 0x3FFF);
      haveDelta_[c]--;
    }
    l += pred_[c] * adpcmVol(vol_[c][0]);
    r += pred_[c] * adpcmVol(vol_[c][1]);
  }
  outL_ = l * (0.5 / 16384.0);
  outR_ = r * (0.5 / 16384.0);
}

void SoundBox::integrate(double upTo) {
  if (upTo > lastT_) {
    accL_ += outL_ * (upTo - lastT_);
    accR_ += outR_ * (upTo - lastT_);
    accT_ += upTo - lastT_;
    lastT_ = upTo;
  }
}

void SoundBox::emitHost() {
  double l = accT_ > 0 ? accL_ / accT_ : outL_, r = accT_ > 0 ? accR_ / accT_ : outR_;
  accL_ = accR_ = accT_ = 0;
  // CD-DA: as amostras de 44,1 kHz que cabem neste período
  cdFrac_ += 44100.0 / hostRate_;
  int take = int(cdFrac_);
  cdFrac_ -= take;
  double sl = 0, sr = 0;
  int got = 0;
  const auto& v = cd_.cdda;
  for (int i = 0; i < take && cdPos_ + 1 < v.size(); i++, cdPos_ += 2, got++) { sl += v[cdPos_]; sr += v[cdPos_ + 1]; }
  if (got) { cdL_ = int16_t(sl / got); cdR_ = int16_t(sr / got); }
  else if (cdPos_ + 1 >= v.size()) { cdL_ = cdR_ = 0; }
  l += cdL_ * (0.5 * cdVol_[0] / 63.0) / 32768.0;
  r += cdR_ * (0.5 * cdVol_[1] / 63.0) / 32768.0;
  if ((aw_ + 1) % ABUF == ar_) ar_ = (ar_ + 1) % ABUF;  // cheio: perde o mais antigo
  abL_[aw_] = float(l);
  abR_[aw_] = float(r);
  aw_ = (aw_ + 1) % ABUF;
}

void SoundBox::run(int64_t t) {
  if (t <= last_) return;
  king_.update(t);
  // PSG: relógio mestre / 6
  const int64_t n = (t - psgClk_) / 6;
  if (n > 0) { psg.run(n); psgClk_ += n * 6; }
  for (;;) {
    const double tk = nextTick_ / 2.0;
    const double nx = std::min(tk, hostNext_);
    if (nx > double(t)) break;
    if (tk <= hostNext_) {
      integrate(tk);
      tick();
      nextTick_ += 1365;
    } else {
      integrate(hostNext_);
      emitHost();
      hostNext_ += hostPeriod_;
    }
  }
  integrate(double(t));
  last_ = t;
  // CD-DA já consumido sai do vetor
  if (cdPos_ > 8192) {
    auto& v = cd_.cdda;
    v.erase(v.begin(), v.begin() + std::ptrdiff_t(std::min(cdPos_, v.size())));
    cdPos_ = 0;
  }
  if (cd_.cdda.size() > 44100 * 2) { cd_.cdda.clear(); cdPos_ = 0; }  // segurança: ninguém consumiu
}

size_t SoundBox::available() const {
  return std::min(psg.available(), (aw_ + ABUF - ar_) % ABUF);
}

void SoundBox::pull(float* l, float* r, size_t n) {
  psg.pull(l, r, n);
  for (size_t i = 0; i < n; i++) {
    if (ar_ == aw_) break;
    l[i] += abL_[ar_];
    r[i] += abR_[ar_];
    ar_ = (ar_ + 1) % ABUF;
  }
}

bool SoundBox::temposOk(int64_t agora) const {
  // NaN/infinito: as comparações dão sempre falso e o laço de run() não termina
  for (double d : {hostNext_, lastT_, outL_, outR_})
    if (!std::isfinite(d)) return false;
  const double a = double(agora), j = double(JANELA_ESTADO);
  return tempoOk(last_, agora) && tempoOk(psgClk_, agora) && tempoOk(nextTick_ / 2, agora) &&
         hostNext_ >= a - j && hostNext_ <= a + j && lastT_ >= a - j && lastT_ <= a + j &&
         std::fabs(outL_) < 1e6 && std::fabs(outR_) < 1e6;
}

void SoundBox::sync(StateIO& io) {
  psg.sync(io);
  io.sync(last_); io.sync(psgClk_); io.sync(nextTick_); io.sync(ctrl_);
  io.raw(vol_, sizeof vol_); io.raw(cdVol_, sizeof cdVol_);
  io.sync(smallDiv_);
  io.raw(delta_, sizeof delta_); io.raw(haveDelta_, sizeof haveDelta_); io.raw(pred_, sizeof pred_); io.raw(step_, sizeof step_);
  io.raw(nibble_, sizeof nibble_); io.raw(half_, sizeof half_);
  io.sync(haveHalf_[0]); io.sync(haveHalf_[1]);
  io.sync(outL_); io.sync(outR_); io.sync(hostNext_); io.sync(lastT_);
  if (io.reading()) {
    for (int c = 0; c < 2; c++) {
      step_[c] = std::clamp(step_[c], 0, 48);
      pred_[c] = std::clamp(pred_[c], -0x4000, 0x3FFF);
      nibble_[c] &= 0xC;
      haveDelta_[c] = std::clamp(haveDelta_[c], 0, 8);
      vol_[c][0] &= 0x3F; vol_[c][1] &= 0x3F; cdVol_[c] &= 0x3F;
    }
    if (smallDiv_ < -1 || smallDiv_ > 8) smallDiv_ = 0;
    accL_ = accR_ = accT_ = 0;
    cdPos_ = 0;
    aw_ = ar_ = 0;
  }
}

}  // namespace indigo::pcfx
