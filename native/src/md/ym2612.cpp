#include "ym2612.h"

#include <cmath>

namespace indigo::md {

namespace {
struct Tables {
  uint16_t logsin[256], exp[256];
  Tables() {
    for (int i = 0; i < 256; i++) {
      const double s = std::sin((i + 0.5) * 3.14159265358979323846 / 512.0);
      logsin[i] = static_cast<uint16_t>(std::lround(-std::log2(s) * 256.0));
      exp[i] = static_cast<uint16_t>(std::lround((std::pow(2.0, i / 256.0) - 1.0) * 1024.0));
    }
  }
};
const Tables T;

// incremento do envelope por ciclo (8 passos) para cada linha de taxa
const uint8_t EG_INC[19][8] = {
    {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1},
    {1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2},
    {2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 4, 2, 2, 2, 4}, {2, 4, 2, 4, 2, 4, 2, 4}, {2, 4, 4, 4, 2, 4, 4, 4},
    {4, 4, 4, 4, 4, 4, 4, 4}, {4, 4, 4, 8, 4, 4, 4, 8}, {4, 8, 4, 8, 4, 8, 4, 8}, {4, 8, 8, 8, 4, 8, 8, 8},
    {8, 8, 8, 8, 8, 8, 8, 8}, {16, 16, 16, 16, 16, 16, 16, 16}, {0, 0, 0, 0, 0, 0, 0, 0}};

const uint8_t DETUNE[8] = {16, 17, 19, 20, 22, 24, 27, 29};
const uint8_t LFO_SH1[8][8] = {{7, 7, 7, 7, 7, 7, 7, 7}, {7, 7, 7, 7, 7, 7, 7, 7}, {7, 7, 7, 7, 7, 7, 1, 1},
                               {7, 7, 7, 7, 1, 1, 1, 1}, {7, 7, 7, 1, 1, 1, 1, 0}, {7, 7, 1, 1, 0, 0, 0, 0},
                               {7, 7, 1, 1, 0, 0, 0, 0}, {7, 7, 1, 1, 0, 0, 0, 0}};
const uint8_t LFO_SH2[8][8] = {{7, 7, 7, 7, 7, 7, 7, 7}, {7, 7, 7, 7, 2, 2, 2, 2}, {7, 7, 7, 2, 2, 2, 7, 7},
                               {7, 7, 2, 2, 7, 7, 2, 2}, {7, 7, 2, 7, 7, 7, 2, 7}, {7, 7, 7, 2, 7, 7, 2, 1},
                               {7, 7, 7, 2, 7, 7, 2, 1}, {7, 7, 7, 2, 7, 7, 2, 1}};
const int LFO_PERIOD[8] = {108, 77, 71, 67, 62, 44, 8, 5};  // amostras por passo (128 passos)
const int AM_SHIFT[4] = {8, 3, 1, 0};

int keyCode(int fnum, int block) {
  const int b10 = (fnum >> 10) & 1, b9 = (fnum >> 9) & 1, b8 = (fnum >> 8) & 1, b7 = (fnum >> 7) & 1;
  const int n3 = b10 ? (b9 | b8 | b7) : (b9 & b8 & b7);
  return (block << 2) | (b10 << 1) | n3;
}
}  // namespace

YM2612::YM2612() : bufL_(BUF), bufR_(BUF) { reset(); }

void YM2612::reset() {
  for (auto& c : ch_) { c = Ch(); }
  addr_[0] = addr_[1] = 0; latchHi_[0] = latchHi_[1] = 0; latch3Hi_ = 0;
  for (int i = 0; i < 3; i++) { ch3fnum_[i] = 0; ch3block_[i] = 0; ch3kc_[i] = 0; }
  mode27_ = 0; ta_ = tb_ = taCnt_ = tbCnt_ = tbSub_ = 0; status_ = 0;
  lfoOn_ = lfoFreq_ = lfoCnt_ = lfoSub_ = 0;
  dac_ = 0x80; dacOn_ = false; egCnt_ = 0; egSub_ = 0; csmKey_ = false;
  clkSub_ = 0;
  prevL_ = prevR_ = curL_ = curR_ = 0; frac_ = 0;
  wpos_ = rpos_ = 0;
}

void YM2612::setRates(double chipRate, int outRate) {
  chipRate_ = chipRate;
  outRate_ = outRate > 0 ? outRate : 48000;
}

void YM2612::write(int port, uint8_t v) {
  const int part = (port >> 1) & 1;
  if (!(port & 1)) { addr_[part] = v; return; }
  writeReg(part, addr_[part], v);
}

void YM2612::keyOn(Ch& c, int slot, bool on) {
  Op& o = c.op[slot];
  if (on && !o.key) {
    o.key = true;
    o.phase = 0;
    o.ssgInv = (o.ssg & 8) && (o.ssg & 4);
    o.state = 0;
    if (rateOf(o, o.ar ? o.ar : 0, c.kc) >= 62) { o.env = 0; o.state = 1; }
  } else if (!on && o.key) {
    o.key = false;
    if ((o.ssg & 8) && o.ssgInv) o.env = (0x200 - o.env) & 0x3FF;
    o.ssgInv = false;
    o.state = 3;
  }
}

void YM2612::writeReg(int part, int r, uint8_t v) {
  if (part == 0 && r < 0x30) {
    switch (r) {
      case 0x22: for (auto& c2 : ch_) c2.dirty = true; lfoOn_ = (v >> 3) & 1; lfoFreq_ = v & 7; if (!lfoOn_) { lfoCnt_ = 0; lfoSub_ = 0; } break;
      case 0x24: ta_ = (ta_ & 3) | (v << 2); break;
      case 0x25: ta_ = (ta_ & 0x3FC) | (v & 3); break;
      case 0x26: tb_ = v; break;
      case 0x27:
        if ((v & 1) && !(mode27_ & 1)) taCnt_ = 0;
        if ((v & 2) && !(mode27_ & 2)) { tbCnt_ = 0; tbSub_ = 0; }
        if (v & 0x10) status_ &= ~1;
        if (v & 0x20) status_ &= ~2;
        mode27_ = v;
        ch_[2].dirty = true;
        break;
      case 0x28: {
        int c = v & 3;
        if (c == 3) break;
        if (v & 4) c += 3;
        Ch& ch = ch_[c];
        keyOn(ch, 0, (v & 0x10) != 0);  // S1
        keyOn(ch, 2, (v & 0x20) != 0);  // S2
        keyOn(ch, 1, (v & 0x40) != 0);  // S3
        keyOn(ch, 3, (v & 0x80) != 0);  // S4
        break;
      }
      case 0x2A: dac_ = v; break;
      case 0x2B: dacOn_ = (v & 0x80) != 0; break;
      default: break;
    }
    return;
  }
  if (r < 0x30) return;
  const int c = r & 3;
  if (c == 3) return;
  Ch& ch = ch_[c + part * 3];
  ch.dirty = true;
  ch_[2].dirty = true;
  if (r < 0xA0) {
    Op& o = ch.op[(r >> 2) & 3];
    switch (r & 0xF0) {
      case 0x30: o.dt = (v >> 4) & 7; o.mul = v & 15; break;
      case 0x40: o.tl = v & 0x7F; break;
      case 0x50: o.ks = v >> 6; o.ar = v & 31; break;
      case 0x60: o.am = v >> 7; o.d1r = v & 31; break;
      case 0x70: o.d2r = v & 31; break;
      case 0x80: o.d1l = v >> 4; o.rr = v & 15; break;
      case 0x90: o.ssg = v & 15; break;
      default: break;
    }
    return;
  }
  switch (r & 0xFC) {
    case 0xA0: ch.fnum = ((latchHi_[part] & 7) << 8) | v; ch.block = (latchHi_[part] >> 3) & 7; ch.kc = keyCode(ch.fnum, ch.block); break;
    case 0xA4: latchHi_[part] = v & 0x3F; break;
    case 0xA8:
      if (part == 0) {
        ch3fnum_[c] = ((latch3Hi_ & 7) << 8) | v; ch3block_[c] = (latch3Hi_ >> 3) & 7;
        ch3kc_[c] = keyCode(ch3fnum_[c], ch3block_[c]);
      }
      break;
    case 0xAC: if (part == 0) latch3Hi_ = v & 0x3F; break;
    case 0xB0: ch.fb = (v >> 3) & 7; ch.alg = v & 7; break;
    case 0xB4: ch.left = (v & 0x80) != 0; ch.right = (v & 0x40) != 0; ch.ams = (v >> 4) & 3; ch.pms = v & 7; break;
    default: break;
  }
}

int YM2612::rateOf(const Op& o, int r, int kc) const {
  if (!r) return 0;
  const int rate = 2 * r + (kc >> (3 - o.ks));
  return rate > 63 ? 63 : rate;
}

void YM2612::updatePhaseInc(Ch& c, int chIdx) {
  const int pm = lfoOn_ ? (lfoCnt_ >> 2) : 0;  // 5 bits
  for (int s = 0; s < 4; s++) {
    Op& o = c.op[s];
    int fnum = c.fnum, block = c.block, kc = c.kc;
    if (chIdx == 2 && (mode27_ & 0xC0) && s != 3) {
      const int k = s == 0 ? 1 : s == 1 ? 0 : 2;  // S1 <- A9, S3 <- A8, S2 <- AA
      fnum = ch3fnum_[k]; block = ch3block_[k]; kc = ch3kc_[k];
    }
    uint32_t f = static_cast<uint32_t>(fnum) << 1;
    if (c.pms && lfoOn_) {
      int l = pm & 15;
      if (l & 8) l ^= 15;
      const uint32_t fh = static_cast<uint32_t>(fnum) >> 4;
      uint32_t fm = (fh >> LFO_SH1[c.pms][l & 7]) + (fh >> LFO_SH2[c.pms][l & 7]);
      if (c.pms > 5) fm <<= c.pms - 5;
      fm >>= 2;
      if (pm & 16) f -= fm; else f += fm;
      f &= 0xFFF;
    }
    uint32_t base = (f << block) >> 2;
    const int dtl = o.dt & 3;
    if (dtl) {
      int k = kc > 0x1C ? 0x1C : kc;
      const int sum = (k >> 2) + 9 + ((dtl == 3) | (dtl & 2));
      const int det = DETUNE[((sum & 1) << 2) | (k & 3)] >> (9 - (sum >> 1));
      if (o.dt & 4) base -= static_cast<uint32_t>(det); else base += static_cast<uint32_t>(det);
    }
    base &= 0x1FFFF;
    const uint32_t mul = o.mul ? static_cast<uint32_t>(o.mul) * 2 : 1;
    o.inc = ((base * mul) >> 1) & 0xFFFFF;
    o.ks = o.ks;  // escala de tecla usa o kc do operador abaixo
    (void)kc;
  }
}

void YM2612::egStep(Ch& c) {
  for (int s = 0; s < 4; s++) {
    Op& o = c.op[s];
    int kc = c.kc;
    // SSG-EG: chegou ao meio da escala
    if ((o.ssg & 8) && o.state != 3 && o.env >= 0x200) {
      if (o.ssg & 1) {
        if (o.ssg & 2) o.ssgInv = !((o.ssg >> 2) & 1);
        else o.ssgInv = (o.ssg >> 2) & 1;
        o.env = 0x3FF;
        o.state = 2;
      } else {
        if (o.ssg & 2) o.ssgInv = !o.ssgInv;
        else o.phase = 0;
        o.state = 0;
        o.env = 0x3FF;
        if (rateOf(o, o.ar, kc) >= 62) { o.env = 0; o.state = 1; }
        continue;
      }
    }
    int r;
    switch (o.state) {
      case 0: r = o.ar; break;
      case 1: r = o.d1r; break;
      case 2: r = o.d2r; break;
      default: r = o.rr * 2 + 1; break;
    }
    const int rate = rateOf(o, r, kc);
    if (!rate) continue;
    int row, shift;
    if (rate < 48) { shift = 11 - (rate >> 2); row = rate & 3; }
    else if (rate < 60) { shift = 0; row = 4 + (rate - 48); }
    else { shift = 0; row = 16; }
    if (o.state == 0 && rate >= 62) row = 17;
    if (egCnt_ & ((1 << shift) - 1)) continue;
    int inc = EG_INC[row][(egCnt_ >> shift) & 7];
    if (o.state == 0) {
      if (rate >= 62) { o.env = 0; }
      else o.env += (~o.env * inc) >> 4;
      if (o.env <= 0) { o.env = 0; o.state = 1; }
      continue;
    }
    if ((o.ssg & 8) && o.env < 0x200) inc *= 4;
    else if (o.ssg & 8) inc = 0;
    o.env += inc;
    if (o.env > 0x3FF) o.env = 0x3FF;
    if (o.state == 1) {
      const int sl = o.d1l == 15 ? 0x3E0 : o.d1l << 5;
      if (o.env >= sl) o.state = 2;
    }
  }
}

int YM2612::opCalc(Op& o, int mod, int am) {
  const int ph = static_cast<int>((o.phase >> 10) + static_cast<uint32_t>(mod)) & 0x3FF;
  o.phase = (o.phase + o.inc) & 0xFFFFF;
  int e = o.env;
  if ((o.ssg & 8) && o.ssgInv && o.state != 3) e = (0x200 - e) & 0x3FF;
  e += o.tl << 3;
  if (o.am) e += am;
  if (e > 0x3FF) e = 0x3FF;
  const int q = (ph & 0x100) ? (~ph & 0xFF) : (ph & 0xFF);
  const int level = T.logsin[q] + (e << 2);
  int out = 0;
  if (level < 13 * 256) out = ((T.exp[(level & 0xFF) ^ 0xFF] | 0x400) << 2) >> (level >> 8);
  return (ph & 0x200) ? -out : out;
}

void YM2612::sample() {
  // LFO
  int am = 0;
  if (lfoOn_) {
    if (++lfoSub_ >= LFO_PERIOD[lfoFreq_]) { lfoSub_ = 0; lfoCnt_ = (lfoCnt_ + 1) & 127; }
    am = (lfoCnt_ < 64 ? lfoCnt_ : 127 - lfoCnt_) * 2;
  }
  // envelope a cada 3 amostras
  if (++egSub_ >= 3) {
    egSub_ = 0;
    egCnt_ = (egCnt_ + 1) & 0xFFF;
    if (!egCnt_) egCnt_ = 1;
    for (auto& c : ch_) egStep(c);
  }
  // temporizadores
  if (mode27_ & 1) {
    if (++taCnt_ >= 1024 - ta_) {
      taCnt_ = 0;
      if (mode27_ & 4) status_ |= 1;
      if ((mode27_ & 0xC0) == 0x80) {  // CSM: liga todos os operadores do canal 3
        for (int s = 0; s < 4; s++) keyOn(ch_[2], s, true);
        csmKey_ = true;
      }
    }
  }
  if (mode27_ & 2) {
    if (++tbSub_ >= 16) {
      tbSub_ = 0;
      if (++tbCnt_ >= 256 - tb_) { tbCnt_ = 0; if (mode27_ & 8) status_ |= 2; }
    }
  }
  int L = 0, R = 0;
  for (int ci = 0; ci < 6; ci++) {
    Ch& c = ch_[ci];
    if (c.dirty || (lfoOn_ && c.pms)) { updatePhaseInc(c, ci); c.dirty = lfoOn_ && c.pms; }
    const int amv = am >> AM_SHIFT[c.ams];
    Op* o = c.op;
    // S1 = op[0], S2 = op[2], S3 = op[1], S4 = op[3]
    const int fbIn = c.fb ? (c.fb1 + c.fb2) >> (10 - c.fb) : 0;
    const int s1 = opCalc(o[0], fbIn, amv);
    c.fb2 = c.fb1; c.fb1 = s1;
    int s2, s3, s4, out;
    switch (c.alg) {
      case 0: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], s2 >> 1, amv); out = opCalc(o[3], s3 >> 1, amv); break;
      case 1: s2 = opCalc(o[2], 0, amv); s3 = opCalc(o[1], (s1 + s2) >> 1, amv); out = opCalc(o[3], s3 >> 1, amv); break;
      case 2: s2 = opCalc(o[2], 0, amv); s3 = opCalc(o[1], s2 >> 1, amv); out = opCalc(o[3], (s1 + s3) >> 1, amv); break;
      case 3: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], 0, amv); out = opCalc(o[3], (s2 + s3) >> 1, amv); break;
      case 4: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], 0, amv); s4 = opCalc(o[3], s3 >> 1, amv); out = s2 + s4; break;
      case 5: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], s1 >> 1, amv); s4 = opCalc(o[3], s1 >> 1, amv); out = s2 + s3 + s4; break;
      case 6: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], 0, amv); s4 = opCalc(o[3], 0, amv); out = s2 + s3 + s4; break;
      default: s2 = opCalc(o[2], 0, amv); s3 = opCalc(o[1], 0, amv); s4 = opCalc(o[3], 0, amv); out = s1 + s2 + s3 + s4; break;
    }
    if (out > 8191) out = 8191;
    if (out < -8192) out = -8192;
    if (ci == 5 && dacOn_) out = (dac_ - 0x80) << 6;
    // out = (out >> 5) << 5;  // DAC de 9 bits
    c.out = out;
    if (c.left) L += out;
    if (c.right) R += out;
  }
  if (csmKey_) {
    csmKey_ = false;
    for (int s = 0; s < 4; s++) keyOn(ch_[2], s, false);
  }
  // reamostragem por média na janela de cada amostra de saída (filtro em caixa: corta o que dobraria acima
  // da metade da taxa de saída)
  const float xl = static_cast<float>(L) * (1.0f / 32768.0f), xr = static_cast<float>(R) * (1.0f / 32768.0f);
  const double step = chipRate_ / outRate_;
  double d = 1.0;
  while (d > 0) {
    if (frac_ <= 0) frac_ = step;
    const double t = d < frac_ ? d : frac_;
    curL_ += static_cast<float>(xl * t); curR_ += static_cast<float>(xr * t);
    frac_ -= t; d -= t;
    if (frac_ <= 1e-9) {
      bufL_[wpos_] = static_cast<float>(curL_ / step);
      bufR_[wpos_] = static_cast<float>(curR_ / step);
      wpos_ = (wpos_ + 1) % BUF;
      if (wpos_ == rpos_) rpos_ = (rpos_ + 1) % BUF;
      curL_ = curR_ = 0;
      frac_ = step;
    }
  }
}

void YM2612::run(int64_t toClock) {
  int64_t n = toClock - clock_;
  if (n <= 0) return;
  clock_ = toClock;
  n += clkSub_;
  int64_t samples = n / 144;
  clkSub_ = static_cast<int>(n % 144);
  while (samples-- > 0) sample();
}

void YM2612::pull(float* L, float* R, size_t n, float*) {
  for (size_t k = 0; k < n; k++) {
    if (rpos_ == wpos_) { L[k] = R[k] = 0; continue; }
    L[k] = bufL_[rpos_]; R[k] = bufR_[rpos_];
    rpos_ = (rpos_ + 1) % BUF;
  }
}

void YM2612::sync(StateIO& io) {
  for (auto& c : ch_) {
    for (auto& o : c.op) {
      io.sync(o.phase); io.sync(o.inc); io.sync(o.dt); io.sync(o.mul); io.sync(o.tl); io.sync(o.ks); io.sync(o.ar);
      io.sync(o.d1r); io.sync(o.d2r); io.sync(o.d1l); io.sync(o.rr); io.sync(o.ssg); io.sync(o.am); io.sync(o.env);
      io.sync(o.state); io.sync(o.key); io.sync(o.ssgInv); io.sync(o.out);
      if (io.reading()) {
        o.phase &= 0xFFFFF; o.inc &= 0xFFFFF; o.dt &= 7; o.mul &= 15; o.tl &= 0x7F; o.ks &= 3; o.ar &= 31; o.d1r &= 31;
        o.d2r &= 31; o.d1l &= 15; o.rr &= 15; o.ssg &= 15; o.am &= 1; clampTo(o.env, 0, 0x3FF); o.state &= 3;
      }
    }
    io.sync(c.fnum); io.sync(c.block); io.sync(c.kc); io.sync(c.alg); io.sync(c.fb); io.sync(c.ams); io.sync(c.pms);
    io.sync(c.dirty); io.sync(c.left); io.sync(c.right); io.sync(c.fb1); io.sync(c.fb2); io.sync(c.out);
    if (io.reading()) {
      c.fnum &= 0x7FF; c.block &= 7; c.kc &= 31; c.alg &= 7; c.fb &= 7; c.ams &= 3; c.pms &= 7;
      clampTo(c.fb1, -8192, 8191); clampTo(c.fb2, -8192, 8191);
    }
  }
  io.raw(addr_, sizeof addr_); io.raw(latchHi_, sizeof latchHi_); io.sync(latch3Hi_);
  io.raw(ch3fnum_, sizeof ch3fnum_); io.raw(ch3block_, sizeof ch3block_); io.raw(ch3kc_, sizeof ch3kc_);
  io.sync(mode27_); io.sync(ta_); io.sync(tb_); io.sync(taCnt_); io.sync(tbCnt_); io.sync(tbSub_); io.sync(status_);
  io.sync(lfoOn_); io.sync(lfoFreq_); io.sync(lfoCnt_); io.sync(lfoSub_); io.sync(dac_); io.sync(dacOn_);
  io.sync(egCnt_); io.sync(egSub_); io.sync(csmKey_); io.sync(clock_); io.sync(clkSub_);
  io.sync(frac_); io.sync(prevL_); io.sync(prevR_); io.sync(curL_); io.sync(curR_);
  if (io.reading()) {
    for (int k = 0; k < 2; k++) { addr_[k] &= 0xFF; latchHi_[k] &= 0x3F; }
    latch3Hi_ &= 0x3F;
    for (int k = 0; k < 3; k++) { ch3fnum_[k] &= 0x7FF; ch3block_[k] &= 7; ch3kc_[k] &= 31; }
    mode27_ &= 0xFF; ta_ &= 0x3FF; tb_ &= 0xFF; clampTo(taCnt_, 0, 1023); clampTo(tbCnt_, 0, 255); clampTo(tbSub_, 0, 15);
    status_ &= 3; lfoOn_ &= 1; lfoFreq_ &= 7; lfoCnt_ &= 127; clampTo(lfoSub_, 0, 108); dac_ &= 0xFF;
    egCnt_ &= 0xFFF; clampTo(egSub_, 0, 2); clampTo(clkSub_, 0, 143);
    finiteOr(frac_, 0); clampTo(frac_, 0.0, chipRate_ / outRate_ + 1.0);
    for (float* f : {&prevL_, &prevR_, &curL_, &curR_}) if (!(*f == *f) || *f > 1000 || *f < -1000) *f = 0;
  }
}

}  // namespace indigo::md
