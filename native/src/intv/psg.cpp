#include "psg.h"

#include <cmath>

namespace indigo::intv {

namespace {
// amplitude de cada nível (escala logarítmica de 3 dB por passo, como o AY-3-891x)
struct Amp {
  float v[16];
  Amp() {
    v[0] = 0;
    for (int i = 1; i < 16; i++) v[i] = static_cast<float>(std::pow(2.0, (i - 15) / 2.0));
  }
};
const Amp AMP;
const int ESHIFT[4] = {31, 2, 1, 0};  // bits 4-5 do volume: envelope /4, /2 ou inteiro
}  // namespace

void PSG::reset() {
  for (auto& x : r_) x = 0;
  for (auto& c : cnt_) c = 0;
  for (auto& c : chan_) c = 0;
  rng_ = 1;
  envCnt_ = 0;
  envVol_ = 0;
  envHold_ = envAltr_ = envAtak_ = envCont_ = false;
  last_ = 0;
  frac_ = acc_ = 0;
  accN_ = 0;
}

void PSG::setSampleRate(int rate, double cpuHz) {
  if (rate < 8000) rate = 8000;
  ticksPerSample_ = (cpuHz / 4.0) / rate;
}

void PSG::write(int reg, uint8_t v, int64_t now) {
  reg &= 15;
  run(now);
  if (reg >= 4 && reg <= 6) v &= 0x0F;
  else if (reg == 9) v &= 0x1F;
  else if (reg == 10) v &= 0x0F;
  else if (reg >= 11 && reg <= 13) v &= 0x3F;
  r_[reg] = v;
  if (reg == 10) {  // novo envelope: recomeça a rampa
    envCnt_ = 0;
    const int ep = (r_[3] | (r_[7] << 8)) * 2;
    cnt_[3] = ep ? ep : 0x20000;
    envHold_ = v & 1;
    envAltr_ = v & 2;
    envAtak_ = v & 4;
    envCont_ = v & 8;
    envVol_ = envAtak_ ? 0 : 15;
  }
}

void PSG::tick() {
  bool hit[5];
  for (int i = 0; i < 5; i++) {
    hit[i] = --cnt_[i] <= 0;
    if (hit[i]) {
      int per;
      if (i < 3) { per = r_[i] | ((r_[i + 4] & 0xF) << 8); if (!per) per = 0x1000; }
      else if (i == 3) { per = (r_[3] | (r_[7] << 8)) * 2; if (!per) per = 0x20000; }
      else { per = (r_[9] & 31) * 2; if (!per) per = 64; }
      cnt_[i] += per;
      if (cnt_[i] <= 0) cnt_[i] = per;
    }
  }
  if (hit[4]) rng_ = (rng_ >> 1) ^ ((rng_ & 1) ? 0x10004u : 0u);
  if (hit[3] && envCnt_ >= 0) {
    envCnt_ = (envCnt_ + 1) & 31;
    int idx = envVol_;
    if (envCnt_ < 16) {
      idx = envAtak_ ? envCnt_ : 15 - envCnt_;
    } else if (envCnt_ == 16 && (!envCont_ || envHold_)) {
      idx = (envCont_ && (envAtak_ != envAltr_)) ? 15 : 0;
      envCnt_ = -1;
    } else if (envCnt_ == 16 && !envAltr_) {
      envCnt_ = 0;
      idx = envAtak_ ? 0 : 15;
    } else {
      idx = envAtak_ ? 15 - (envCnt_ & 15) : (envCnt_ & 15);
    }
    envVol_ = static_cast<uint8_t>(idx & 15);
  }
  const int noise = rng_ & 1;
  float s = 0;
  for (int c = 0; c < 3; c++) {
    const int toneOff = (r_[8] >> c) & 1, noiseOff = (r_[8] >> (c + 3)) & 1;
    const bool on = (toneOff | chan_[c]) & (noiseOff | noise);
    if (on) {
      const uint8_t v = r_[11 + c];
      if (v & 0x30) s += AMP.v[envVol_] / static_cast<float>(1 << ESHIFT[(v >> 4) & 3]);
      else s += AMP.v[v & 15];
    }
    if (hit[c]) chan_[c] ^= 1;
  }
  acc_ += s;
  accN_++;
  frac_ += 1.0;
  if (frac_ >= ticksPerSample_) {
    frac_ -= ticksPerSample_;
    const double x = acc_ / accN_ / 3.0;
    acc_ = 0;
    accN_ = 0;
    dc_ += (x - dc_) * 0.0005;  // tira o nível contínuo (a saída do chip é só positiva)
    push(static_cast<float>((x - dc_) * 1.8));
  }
}

void PSG::push(float v) {
  lastOut_ = v;
  buf_[wpos_] = v;
  wpos_ = (wpos_ + 1) % BUF;
  if (wpos_ == rpos_) rpos_ = (rpos_ + 1) % BUF;
}

void PSG::run(int64_t now) {
  while (last_ + 4 <= now) {
    tick();
    last_ += 4;
  }
}

void PSG::pull(float* l, float* rr, size_t n) {
  for (size_t k = 0; k < n; k++) {
    float v = 0;
    if (rpos_ != wpos_) { v = buf_[rpos_]; rpos_ = (rpos_ + 1) % BUF; }
    l[k] = rr[k] = v;
  }
}

void PSG::sync(StateIO& io) {
  io.raw(r_, sizeof r_);
  io.sync(last_);
  for (auto& c : cnt_) io.sync(c);
  io.raw(chan_, sizeof chan_);
  io.sync(rng_);
  io.sync(envCnt_); io.sync(envVol_);
  io.sync(envHold_); io.sync(envAltr_); io.sync(envAtak_); io.sync(envCont_);
  if (io.reading()) {
    for (auto& c : cnt_) clampTo<int32_t>(c, -1, 0x20000);
    for (auto& c : chan_) c &= 1;
    rng_ &= 0x1FFFF;
    if (!rng_) rng_ = 1;
    clampTo<int32_t>(envCnt_, -1, 31);
    envVol_ &= 15;
    // o som que já estava no buffer não faz parte do estado
    acc_ = 0; accN_ = 0; frac_ = 0;
  }
}

}  // namespace indigo::intv
