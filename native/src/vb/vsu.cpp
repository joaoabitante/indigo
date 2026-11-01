#include "vsu.h"

#include <cstring>

namespace indigo::vb {

namespace {
constexpr double VSU_RATE = 20000000.0 / 480.0;  // uma amostra a cada 120 ciclos de 5 MHz
const int TAP[8] = {14, 10, 13, 4, 8, 6, 9, 11};  // derivação do LFSR do ruído (EV1 bits 4–6)
}  // namespace

VSU::VSU() : bufL_(BUF), bufR_(BUF) {
  setSampleRate(48000);
  reset();
}

void VSU::reset() {
  std::memset(wave_, 0, sizeof wave_);
  std::memset(mod_, 0, sizeof mod_);
  for (int c = 0; c < 6; c++) {
    intl_[c] = lvl_[c] = ram_[c] = 0;
    freq_[c] = env_[c] = 0;
    effFreq_[c] = envelope_[c] = pos_[c] = 0;
    freqCounter_[c] = 1; intervalCounter_[c] = 0; envCounter_[c] = 1;
    effDiv_[c] = 40; intervalDiv_[c] = 4; envDiv_[c] = 4;
  }
  sweep_ = 0; sweepCounter_ = 0; sweepDiv_ = 1; modPos_ = 0; lfsr_ = 1;
  lastTs_ = 0; sub_ = 0;
  prevL_ = prevR_ = 0; phase_ = 0; capL_ = capR_ = 0;
  wpos_ = rpos_ = 0;
}

void VSU::setSampleRate(int rate) { step_ = VSU_RATE / (rate > 0 ? rate : 48000); }

void VSU::write(uint32_t a, uint8_t v, int64_t ts) {
  if (a & 3) return;
  run(ts);
  a &= 0x7FF;
  if (a < 0x280) { wave_[a >> 7][(a >> 2) & 31] = v & 0x3F; return; }
  if (a < 0x400) { mod_[(a >> 2) & 31] = v; return; }
  if (a >= 0x600) return;
  const int ch = (a >> 6) & 0xF;
  if (ch > 5) {
    if (a == 0x580 && (v & 1)) for (auto& i : intl_) i &= ~0x80;  // SSTOP
    return;
  }
  switch ((a >> 2) & 0xF) {
    case 0x0:  // SxINT: liga o canal e reinicia os contadores
      intl_[ch] = v & ~0x40;
      if (v & 0x80) {
        effFreq_[ch] = freq_[ch];
        freqCounter_[ch] = (ch == 5 ? 10 : 1) * (2048 - effFreq_[ch]);
        intervalCounter_[ch] = (v & 0x1F) + 1;
        envCounter_[ch] = (env_[ch] & 7) + 1;
        if (ch == 4) { sweepCounter_ = (sweep_ >> 4) & 7; sweepDiv_ = (sweep_ & 0x80) ? 8 : 1; modPos_ = 0; }
        pos_[ch] = 0;
        if (ch == 5) lfsr_ = 1;
        effDiv_[ch] = 40; intervalDiv_[ch] = 4; envDiv_[ch] = 4;
      }
      break;
    case 0x1: lvl_[ch] = v; break;  // SxLRV
    case 0x2:
      freq_[ch] = static_cast<uint16_t>((freq_[ch] & 0x700) | v);
      effFreq_[ch] = (effFreq_[ch] & 0x700) | v;
      break;
    case 0x3:
      freq_[ch] = static_cast<uint16_t>((freq_[ch] & 0xFF) | ((v & 7) << 8));
      effFreq_[ch] = (effFreq_[ch] & 0xFF) | ((v & 7) << 8);
      break;
    case 0x4: env_[ch] = static_cast<uint16_t>((env_[ch] & 0xFF00) | v); envelope_[ch] = v >> 4; break;
    case 0x5:
      env_[ch] = static_cast<uint16_t>((env_[ch] & 0xFF) | ((v & (ch >= 4 ? 0x73 : 0x03)) << 8));
      if (ch == 5) lfsr_ = 1;
      break;
    case 0x6: ram_[ch] = v & 0xF; break;
    case 0x7: if (ch == 4) sweep_ = v; break;
    default: break;
  }
}

// duração (intervalo), envelope e, no canal 5, varredura/modulação: relógio de 4800 ciclos (0,96 ms)
void VSU::effects(int ch) {
  if (--intervalDiv_[ch] <= 0) {
    intervalDiv_[ch] = 4;
    if ((intl_[ch] & 0x20) && --intervalCounter_[ch] <= 0) intl_[ch] &= ~0x80;
    if (--envDiv_[ch] <= 0) {
      envDiv_[ch] = 4;
      if ((env_[ch] & 0x100) && --envCounter_[ch] <= 0) {
        envCounter_[ch] = (env_[ch] & 7) + 1;
        if (env_[ch] & 8) { if (envelope_[ch] < 15 || (env_[ch] & 0x200)) envelope_[ch] = (envelope_[ch] + 1) & 15; }
        else if (envelope_[ch] > 0 || (env_[ch] & 0x200)) envelope_[ch] = (envelope_[ch] - 1) & 15;
      }
    }
  }
  if (ch != 4) return;
  if (--sweepDiv_ > 0) return;
  sweepDiv_ = (sweep_ & 0x80) ? 8 : 1;
  if (!((sweep_ >> 4) & 7) || !(env_[4] & 0x4000)) return;
  if (sweepCounter_) sweepCounter_--;
  if (sweepCounter_) return;
  sweepCounter_ = (sweep_ >> 4) & 7;
  if (env_[4] & 0x1000) {  // modulação: frequência = base + tabela (com repetição opcional)
    if (modPos_ < 32 || (env_[4] & 0x2000)) {
      modPos_ &= 31;
      effFreq_[4] = (freq_[4] + static_cast<int8_t>(mod_[modPos_])) & 0x7FF;
      modPos_++;
    }
  } else {  // varredura
    const int32_t delta = effFreq_[4] >> (sweep_ & 7);
    const int32_t nf = effFreq_[4] + ((sweep_ & 8) ? delta : -delta);
    if (nf < 0) effFreq_[4] = 0;
    else if (nf > 0x7FF) intl_[4] &= ~0x80;
    else effFreq_[4] = nf;
  }
}

void VSU::tick() {
  int32_t L = 0, R = 0;
  for (int ch = 0; ch < 6; ch++) {
    if (!(intl_[ch] & 0x80)) continue;
    const int32_t period = (ch == 5 ? 10 : 1) * (2048 - effFreq_[ch]);
    freqCounter_[ch] -= 120;
    if (freqCounter_[ch] <= 0) {
      const int32_t n = -freqCounter_[ch] / period + 1;
      freqCounter_[ch] += n * period;
      if (ch < 5) {
        pos_[ch] = (pos_[ch] + n) & 31;
      } else {
        const int tap = TAP[(env_[5] >> 12) & 7];
        for (int k = 0; k < n; k++) {
          const uint32_t fbk = ((lfsr_ >> 7) & 1) ^ ((lfsr_ >> tap) & 1) ^ 1;
          lfsr_ = ((lfsr_ << 1) & 0x7FFF) | fbk;
        }
      }
    }
    if (--effDiv_[ch] <= 0) { effDiv_[ch] = 40; effects(ch); }
    if (!(intl_[ch] & 0x80)) continue;
    const int wd = ch == 5 ? ((lfsr_ & 1) ? 63 : 0) : (ram_[ch] > 4 ? 0 : wave_[ram_[ch]][pos_[ch]]);
    int l = envelope_[ch] * (lvl_[ch] >> 4), r = envelope_[ch] * (lvl_[ch] & 15);
    if (l) l = (l >> 3) + 1;
    if (r) r = (r >> 3) + 1;
    L += wd * l;
    R += wd * r;
  }
  emit(L / 8192.0, R / 8192.0);
}

void VSU::emit(double l, double r) {
  // passa-alta (tira o nível contínuo das ondas, que são só positivas)
  const double ol = l - capL_, orr = r - capR_;
  capL_ += ol * 0.002;
  capR_ += orr * 0.002;
  while (phase_ < 1.0) {
    bufL_[wpos_] = static_cast<float>(prevL_ + (ol - prevL_) * phase_);
    bufR_[wpos_] = static_cast<float>(prevR_ + (orr - prevR_) * phase_);
    wpos_ = (wpos_ + 1) % BUF;
    if (wpos_ == rpos_) rpos_ = (rpos_ + 1) % BUF;
    phase_ += step_;
  }
  phase_ -= 1.0;
  prevL_ = ol;
  prevR_ = orr;
}

void VSU::run(int64_t ts) {
  int64_t n = ts - lastTs_;
  if (n <= 0) return;
  lastTs_ = ts;
  n += sub_;
  int64_t ticks = n / 480;
  sub_ = static_cast<int32_t>(n % 480);
  while (ticks-- > 0) tick();
}

void VSU::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos_ == wpos_) { L[k] = 0; R[k] = 0; continue; }
    L[k] = bufL_[rpos_];
    R[k] = bufR_[rpos_];
    rpos_ = (rpos_ + 1) % BUF;
  }
}

void VSU::sync(StateIO& io) {
  io.raw(wave_, sizeof wave_); io.raw(mod_, sizeof mod_);
  io.raw(intl_, sizeof intl_); io.raw(lvl_, sizeof lvl_); io.raw(ram_, sizeof ram_); io.sync(sweep_);
  io.raw(freq_, sizeof freq_); io.raw(env_, sizeof env_);
  io.raw(effFreq_, sizeof effFreq_); io.raw(envelope_, sizeof envelope_); io.raw(pos_, sizeof pos_);
  io.raw(freqCounter_, sizeof freqCounter_); io.raw(intervalCounter_, sizeof intervalCounter_);
  io.raw(envCounter_, sizeof envCounter_); io.raw(effDiv_, sizeof effDiv_);
  io.raw(intervalDiv_, sizeof intervalDiv_); io.raw(envDiv_, sizeof envDiv_);
  io.sync(sweepCounter_); io.sync(sweepDiv_); io.sync(modPos_); io.sync(lfsr_);
  io.sync(lastTs_); io.sync(sub_);
  io.sync(prevL_); io.sync(prevR_); io.sync(phase_); io.sync(capL_); io.sync(capR_);
  if (io.reading()) {
    for (auto& w : wave_) for (auto& s : w) s &= 0x3F;
    for (int c = 0; c < 6; c++) {
      ram_[c] &= 0xF; freq_[c] &= 0x7FF; effFreq_[c] &= 0x7FF; envelope_[c] &= 15; pos_[c] &= 31;
      env_[c] &= c >= 4 ? 0x73FF : 0x03FF;
      clampTo(freqCounter_[c], 1, 10 * 2048);
      clampTo(intervalCounter_[c], 0, 32);
      clampTo(envCounter_[c], 1, 8);
      clampTo(effDiv_[c], 1, 40); clampTo(intervalDiv_[c], 1, 4); clampTo(envDiv_[c], 1, 4);
    }
    sweepCounter_ &= 7; clampTo(sweepDiv_, 1, 8); clampTo(modPos_, 0, 32); lfsr_ &= 0x7FFF;
    clampTo(sub_, 0, 479);
    for (double* d : {&prevL_, &prevR_, &capL_, &capR_}) { finiteOr(*d, 0); clampTo(*d, -100.0, 100.0); }
    finiteOr(phase_, 0); clampTo(phase_, 0.0, 1.0);
  }
}

}  // namespace indigo::vb
