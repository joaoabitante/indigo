#include "psg.h"

#include <algorithm>

namespace indigo::pce {

namespace {
constexpr double PSG_HZ = 21477272.727272 / 6;
// redução de volume em passos de 1,5 dB (255 * 10^(-1,5k/20)); 30 passos ou mais = mudo
constexpr uint8_t VOL[30] = {255, 214, 180, 151, 127, 107, 90, 76, 64, 53, 45, 38, 32, 27, 22,
                             19,  16,  13,  11,  9,   8,   6,  5,  4,  4,  3,  2,  2,  2,  1};
}  // namespace

void Psg::reset() {
  for (auto& c : ch_) c = Ch{};
  sel_ = mainL_ = mainR_ = lfoFreq_ = lfoCtrl_ = 0;
  outL_ = outR_ = 0;
  accL_ = accR_ = accN_ = hpL_ = hpR_ = 0;
  aw_ = ar_ = 0;
}

void Psg::setSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 192000) rate = 192000;
  hostRate_ = rate;
}

void Psg::pull(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (ar_ == aw_) { l[k] = 0; r[k] = 0; continue; }
    l[k] = abL_[ar_]; r[k] = abR_[ar_];
    ar_ = (ar_ + 1) % ABUF;
  }
}

int32_t Psg::period(int i) const {
  int32_t p = ch_[i].freq;
  if (i == 0 && lfoOn()) {
    // o LFO soma ao período do canal 1 a saída atual do canal 2 (x1, x4 ou x16)
    const int shift = ((lfoCtrl_ & 3) - 1) * 2;
    p = (p + (static_cast<int32_t>(ch_[1].out) << shift)) & 0xFFF;
  }
  if (!p) p = 0x1000;
  if (i == 1 && lfoOn()) p *= lfoFreq_ ? lfoFreq_ : 0x100;  // o canal 2 anda dividido pela frequência do LFO
  return p;
}

void Psg::write(int reg, uint8_t v) {
  switch (reg & 0x0F) {
    case 0: sel_ = v & 7; break;
    case 1: mainR_ = v & 0x0F; mainL_ = v >> 4; break;
    case 8: lfoFreq_ = v; break;
    case 9: lfoCtrl_ = v; break;
    default: {
      if (sel_ >= 6) break;
      Ch& c = ch_[sel_];
      switch (reg & 0x0F) {
        case 2: c.freq = static_cast<uint16_t>((c.freq & 0xF00) | v); break;
        case 3: c.freq = static_cast<uint16_t>((c.freq & 0xFF) | ((v & 0x0F) << 8)); break;
        case 4:
          if (c.on != ((v & 0x80) != 0)) { c.timer = period(sel_); c.on = (v & 0x80) != 0; }
          c.dda = (v & 0x40) != 0;
          c.amp = v & 0x1F;
          if (c.dda) {
            if (c.on) c.out = static_cast<int8_t>(c.ddaVal);
            else c.waddr = 0;  // DDA ligado com o canal desligado zera o índice da onda
          }
          break;
        case 5: c.rvol = v & 0x0F; c.lvol = v >> 4; break;
        case 6:
          if (c.dda) {
            c.ddaVal = v & 0x1F;
            if (c.on) c.out = static_cast<int8_t>(c.ddaVal);
          } else {
            c.wave[c.waddr] = v & 0x1F;
            if (!c.on) c.waddr = (c.waddr + 1) & 0x1F;
            if (!c.noise) c.out = static_cast<int8_t>(c.wave[c.waddr]);
          }
          break;
        case 7:
          if (sel_ >= 4) { c.noise = (v & 0x80) != 0; c.noiseFreq = v & 0x1F; }
          break;
        default: break;
      }
    }
  }
  mix();
}

void Psg::advance(int i, int32_t n) {
  Ch& c = ch_[i];
  if (i >= 4) {
    // o LFSR anda sempre na frequência do ruído, mesmo com o canal desligado
    if (c.noiseTimer <= n) {
      c.noiseTimer = noisePeriod(c);
      const uint32_t v = c.lfsr;
      const uint32_t bit = (v ^ (v >> 1) ^ (v >> 11) ^ (v >> 12) ^ (v >> 17)) & 1;
      c.noiseOut = static_cast<int8_t>((v & 1) ? 0x1F : 0);
      c.lfsr = (v >> 1) | (bit << 17);
    } else {
      c.noiseTimer -= n;
    }
  }
  if (!c.on) { c.out = 0; return; }
  if (c.dda) { c.out = static_cast<int8_t>(c.ddaVal); return; }
  if (c.noise) { c.out = c.noiseOut; return; }
  c.timer -= n;
  while (c.timer <= 0) {
    c.timer += period(i);
    c.waddr = (c.waddr + 1) & 0x1F;
  }
  c.out = static_cast<int8_t>(c.wave[c.waddr]);
}

void Psg::mix() {
  int32_t l = 0, r = 0;
  for (const Ch& c : ch_) {
    const int rl = (15 - mainL_) * 2 + (31 - c.amp) + (15 - c.lvol) * 2;
    const int rr = (15 - mainR_) * 2 + (31 - c.amp) + (15 - c.rvol) * 2;
    if (rl < 30) l += c.out * VOL[rl];
    if (rr < 30) r += c.out * VOL[rr];
  }
  outL_ = l; outR_ = r;
}

void Psg::emit(double l, double r) {
  // tira o nível contínuo (as ondas vão de 0 a 31) e põe na escala de -1 a 1
  const double ol = l - hpL_, orr = r - hpR_;
  hpL_ += ol * 0.0015; hpR_ += orr * 0.0015;
  // um canal no volume máximo dá cerca de ±0,25; seis juntos podem passar de 1 e são limitados
  abL_[aw_] = static_cast<float>(std::clamp(ol / 16000.0, -1.0, 1.0));
  abR_[aw_] = static_cast<float>(std::clamp(orr / 16000.0, -1.0, 1.0));
  aw_ = (aw_ + 1) % ABUF;
  if (aw_ == ar_) ar_ = (ar_ + 1) % ABUF;
}

void Psg::run(int64_t clocks) {
  const double spc = PSG_HZ / hostRate_;  // relógios do PSG por amostra de saída
  while (clocks > 0) {
    int64_t step = clocks;
    for (int i = 0; i < 6; i++) {
      const Ch& c = ch_[i];
      if (i >= 4 && c.noiseTimer > 0 && c.noiseTimer < step) step = c.noiseTimer;
      if (c.on && !c.dda && !c.noise && c.timer > 0 && c.timer < step) step = c.timer;
    }
    if (step < 4) step = clocks < 4 ? clocks : 4;  // ondas muito agudas: passos mínimos de 4 relógios
    double rem = static_cast<double>(step);
    while (rem > 0) {
      const double need = spc - accN_;
      if (rem < need) { accL_ += outL_ * rem; accR_ += outR_ * rem; accN_ += rem; break; }
      accL_ += outL_ * need; accR_ += outR_ * need;
      emit(accL_ / spc, accR_ / spc);
      accL_ = accR_ = accN_ = 0;
      rem -= need;
    }
    for (int i = 0; i < 6; i++) advance(i, static_cast<int32_t>(step));
    clocks -= step;
    mix();
  }
}

void Psg::sync(StateIO& io) {
  for (Ch& c : ch_) {
    io.sync(c.freq); io.sync(c.on); io.sync(c.dda); io.sync(c.noise); io.sync(c.amp); io.sync(c.lvol); io.sync(c.rvol);
    io.sync(c.waddr); io.sync(c.ddaVal); io.sync(c.noiseFreq); io.raw(c.wave, sizeof c.wave); io.sync(c.out);
    io.sync(c.noiseOut); io.sync(c.lfsr); io.sync(c.timer); io.sync(c.noiseTimer);
    if (io.reading()) {
      c.freq &= 0xFFF; c.amp &= 0x1F; c.lvol &= 15; c.rvol &= 15; c.waddr &= 31; c.ddaVal &= 31; c.noiseFreq &= 31;
      for (auto& w : c.wave) w &= 31;
      c.lfsr &= 0x3FFFF;
      clampTo<int32_t>(c.timer, -64, 0x1000 * 256);
      clampTo<int32_t>(c.noiseTimer, -64, 32 * 64);
    }
  }
  io.sync(sel_); io.sync(mainL_); io.sync(mainR_); io.sync(lfoFreq_); io.sync(lfoCtrl_);
  io.sync(accL_); io.sync(accR_); io.sync(accN_); io.sync(hpL_); io.sync(hpR_);
  if (io.reading()) {
    sel_ &= 7; mainL_ &= 15; mainR_ &= 15;
    finiteOr(accL_, 0); finiteOr(accR_, 0); finiteOr(hpL_, 0); finiteOr(hpR_, 0); finiteOr(accN_, 0);
    if (accN_ < 0 || accN_ > 1e6) accN_ = 0;
    mix();
  }
}

}  // namespace indigo::pce
