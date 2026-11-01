#include "ym2151.h"

#include <cmath>

namespace indigo::cps {

namespace {
struct Tables {
  uint16_t logsin[256], exp[256];
  uint32_t phaseStep[768];  // passo de fase no bloco 7 para cada 1/64 de semitom (índice 512 = Lá)
  Tables() {
    for (int i = 0; i < 256; i++) {
      const double s = std::sin((i + 0.5) * 3.14159265358979323846 / 512.0);
      logsin[i] = static_cast<uint16_t>(std::lround(-std::log2(s) * 256.0));
      exp[i] = static_cast<uint16_t>(std::lround((std::pow(2.0, i / 256.0) - 1.0) * 1024.0));
    }
    // o código de nota 10 da oitava 4 é o Lá de 440 Hz com o relógio de 3,579545 MHz (55930 Hz de amostragem);
    // a fase tem 20 bits e o passo do bloco 4 é este valor >> 3
    const double fs = 3579545.0 / 64.0;
    for (int i = 0; i < 768; i++)
      phaseStep[i] = static_cast<uint32_t>(std::lround(8.0 * 1048576.0 / fs * 440.0 * std::pow(2.0, (i - 512) / 768.0)));
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

// desafinação DT1 pelo código de tecla de 5 bits (a mesma tabela do OPN, do manual da Yamaha)
const uint8_t DT1[32][4] = {
    {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 0, 1, 2},  {0, 1, 2, 2},   {0, 1, 2, 3},   {0, 1, 2, 3},   {0, 1, 2, 3},
    {0, 1, 2, 4},  {0, 1, 3, 4},  {0, 1, 3, 4},  {0, 1, 3, 5},  {0, 2, 4, 5},   {0, 2, 4, 6},   {0, 2, 4, 6},   {0, 2, 5, 7},
    {0, 2, 5, 8},  {0, 3, 6, 8},  {0, 3, 6, 9},  {0, 3, 7, 10}, {0, 4, 8, 11},  {0, 4, 8, 12},  {0, 4, 9, 13},  {0, 5, 10, 14},
    {0, 5, 11, 16}, {0, 6, 12, 17}, {0, 6, 13, 19}, {0, 7, 14, 20}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}, {0, 8, 16, 22}};

// DT2: soma em 1/64 de semitom (0, +600, +781, +950 centésimos)
const int DT2[4] = {0, 384, 500, 608};

// código de nota "com buracos" (0-2, 4-6, 8-10, 12-14) e fração -> passo de fase
uint32_t phaseOf(int kc, int kf, int delta) {
  int block = (kc >> 4) & 7;
  const int code = kc & 15;
  int eff = ((code - (code >> 2)) << 6) + kf + delta;
  if (eff < 0) {
    eff += 768;
    if (block-- == 0) return T.phaseStep[0] >> 7;
  } else if (eff >= 768) {
    eff -= 768;
    if (eff >= 768) { block++; eff -= 768; }
    if (block++ >= 7) return T.phaseStep[767];
  }
  if (block > 7) return T.phaseStep[767];
  return T.phaseStep[eff] >> (block ^ 7);
}
}  // namespace

YM2151::YM2151() { reset(); }

void YM2151::reset() {
  for (auto& c : ch_) c = Ch();
  addr_ = 0;
  ta_ = tb_ = taCnt_ = tbCnt_ = tbSub_ = ctl14_ = status_ = 0;
  lfrq_ = pmd_ = amd_ = wave_ = test_ = 0;
  lfoCnt_ = 0; lfoAm_ = 0;
  noiseEn_ = noiseFreq_ = noiseCnt_ = noiseState_ = 0;
  noiseLfsr_ = 1;
  egCnt_ = 0; egSub_ = 0;
}

void YM2151::write(int port, uint8_t v) {
  if (!(port & 1)) { addr_ = v; return; }
  writeReg(addr_, v);
}

void YM2151::keyOn(Ch& c, int slot, bool on) {
  Op& o = c.op[slot];
  const int kc5 = (c.kc >> 2) & 31;
  if (on && !o.key) {
    o.key = true;
    o.phase = 0;
    o.state = 0;
    if (rateOf(o, o.ar, kc5) >= 62) { o.env = 0; o.state = 1; }
  } else if (!on && o.key) {
    o.key = false;
    o.state = 3;
  }
}

void YM2151::writeReg(int r, uint8_t v) {
  if (r < 0x20) {
    switch (r) {
      case 0x01: test_ = v; if (v & 2) lfoCnt_ = 0; break;
      case 0x08: {
        Ch& c = ch_[v & 7];
        keyOn(c, 0, (v & 0x08) != 0);  // M1
        keyOn(c, 2, (v & 0x10) != 0);  // C1
        keyOn(c, 1, (v & 0x20) != 0);  // M2
        keyOn(c, 3, (v & 0x40) != 0);  // C2
        break;
      }
      case 0x0F: noiseEn_ = v >> 7; noiseFreq_ = v & 31; break;
      case 0x10: ta_ = (ta_ & 3) | (v << 2); break;
      case 0x11: ta_ = (ta_ & 0x3FC) | (v & 3); break;
      case 0x12: tb_ = v; break;
      case 0x14:
        if ((v & 1) && !(ctl14_ & 1)) taCnt_ = 0;
        if ((v & 2) && !(ctl14_ & 2)) { tbCnt_ = 0; tbSub_ = 0; }
        if (v & 0x10) status_ &= ~1;
        if (v & 0x20) status_ &= ~2;
        ctl14_ = v & 0x8F;
        break;
      case 0x18: lfrq_ = v; break;
      case 0x19: if (v & 0x80) pmd_ = v & 0x7F; else amd_ = v & 0x7F; break;
      case 0x1B: wave_ = v & 3; break;
      default: break;
    }
    return;
  }
  ch_[r & 7].dirty = true;
  if (r < 0x40) {
    Ch& c = ch_[r & 7];
    switch (r & 0x38) {
      case 0x20: c.left = (v & 0x40) != 0; c.right = (v & 0x80) != 0; c.fb = (v >> 3) & 7; c.con = v & 7; break;
      case 0x28: c.kc = v & 0x7F; break;
      case 0x30: c.kf = v >> 2; break;
      default: c.pms = (v >> 4) & 7; c.ams = v & 3; break;
    }
    return;
  }
  Op& o = ch_[r & 7].op[(r >> 3) & 3];
  switch (r & 0xE0) {
    case 0x40: o.dt1 = (v >> 4) & 7; o.mul = v & 15; break;
    case 0x60: o.tl = v & 0x7F; break;
    case 0x80: o.ks = v >> 6; o.ar = v & 31; break;
    case 0xA0: o.amEn = v >> 7; o.d1r = v & 31; break;
    case 0xC0: o.dt2 = v >> 6; o.d2r = v & 31; break;
    default: o.d1l = v >> 4; o.rr = v & 15; break;
  }
}

int YM2151::rateOf(const Op& o, int r, int kc5) const {
  if (!r) return 0;
  const int rate = 2 * r + (kc5 >> (3 - o.ks));
  return rate > 63 ? 63 : rate;
}

void YM2151::updatePhase(Ch& c, int pmDelta) {
  const int kc5 = (c.kc >> 2) & 31;
  int pm = 0;
  if (c.pms) pm = c.pms < 6 ? (pmDelta >> (6 - c.pms)) : (pmDelta * (1 << (c.pms - 5)));
  if (!c.dirty && pm == c.lastPm) return;
  c.dirty = false;
  c.lastPm = pm;
  for (Op& o : c.op) {
    uint32_t step = phaseOf(c.kc, c.kf, DT2[o.dt2] + pm);
    const int det = DT1[kc5][o.dt1 & 3];
    if (o.dt1 & 4) step -= static_cast<uint32_t>(det); else step += static_cast<uint32_t>(det);
    step &= 0x1FFFF;
    const uint32_t mul = o.mul ? static_cast<uint32_t>(o.mul) * 2 : 1;
    o.inc = (step * mul) >> 1;
  }
}

void YM2151::egStep(Ch& c) {
  const int kc5 = (c.kc >> 2) & 31;
  for (Op& o : c.op) {
    int r;
    switch (o.state) {
      case 0: r = o.ar; break;
      case 1: r = o.d1r; break;
      case 2: r = o.d2r; break;
      default: r = o.rr * 2 + 1; break;
    }
    const int rate = rateOf(o, r, kc5);
    if (!rate) continue;
    int row, shift;
    if (rate < 48) { shift = 11 - (rate >> 2); row = rate & 3; }
    else if (rate < 60) { shift = 0; row = 4 + (rate - 48); }
    else { shift = 0; row = 16; }
    if (o.state == 0 && rate >= 62) row = 17;
    if (egCnt_ & ((1 << shift) - 1)) continue;
    const int inc = EG_INC[row][(egCnt_ >> shift) & 7];
    if (o.state == 0) {
      if (rate >= 62) o.env = 0;
      else o.env += (~o.env * inc) >> 4;
      if (o.env <= 0) { o.env = 0; o.state = 1; }
      continue;
    }
    o.env += inc;
    if (o.env > 0x3FF) o.env = 0x3FF;
    if (o.state == 1) {
      const int sl = o.d1l == 15 ? 0x3E0 : o.d1l << 5;
      if (o.env >= sl) o.state = 2;
    }
  }
}

int YM2151::envLevel(const Op& o, int am) const {
  int e = o.env + (o.tl << 3);
  if (o.amEn) e += am;
  return e > 0x3FF ? 0x3FF : e;
}

int YM2151::opCalc(Op& o, int mod, int am) {
  const int ph = static_cast<int>((o.phase >> 10) + static_cast<uint32_t>(mod)) & 0x3FF;
  o.phase = (o.phase + o.inc) & 0xFFFFF;
  const int e = envLevel(o, am);
  const int q = (ph & 0x100) ? (~ph & 0xFF) : (ph & 0xFF);
  const int level = T.logsin[q] + (e << 2);
  int out = 0;
  if (level < 13 * 256) out = ((T.exp[(level & 0xFF) ^ 0xFF] | 0x400) << 2) >> (level >> 8);
  return (ph & 0x200) ? -out : out;
}

// LFO e ruído: devolve o desvio de fase (vibrato) com sinal; guarda o tremolo em lfoAm_
int YM2151::lfoTick() {
  const int nf = noiseFreq_ ^ 31;
  for (int rep = 0; rep < 2; rep++) {
    noiseLfsr_ = ((noiseLfsr_ << 1) | (((noiseLfsr_ >> 17) ^ (noiseLfsr_ >> 14) ^ 1) & 1)) & 0x1FFFFFF;
    if (noiseCnt_++ >= nf) { noiseCnt_ = 0; noiseState_ = (noiseLfsr_ >> 17) & 1; }
  }
  lfoCnt_ += static_cast<uint32_t>(0x10 | (lfrq_ & 15)) << (lfrq_ >> 4);
  if (test_ & 2) lfoCnt_ = 0;
  const int q = static_cast<int>((lfoCnt_ >> 22) & 0xFF);
  int am, pm;
  switch (wave_) {
    case 0: am = 255 - q; pm = q < 128 ? q : q - 256; break;                 // serra
    case 1: am = q < 128 ? 255 : 0; pm = q < 128 ? 127 : -128; break;         // quadrada
    case 2:                                                                   // triângulo
      am = q < 128 ? 255 - 2 * q : 2 * q - 256;
      pm = q < 64 ? 2 * q : q < 192 ? 255 - 2 * q : 2 * q - 512;
      break;
    default: am = static_cast<int>(noiseLfsr_ & 0xFF); pm = am < 128 ? am : am - 256; break;  // ruído
  }
  lfoAm_ = (am * amd_) >> 7;
  return (pm * pmd_) >> 7;
}

void YM2151::sample(int& outL, int& outR) {
  const int pm = lfoTick();
  if (++egSub_ >= 3) {
    egSub_ = 0;
    egCnt_ = (egCnt_ + 1) & 0xFFF;
    if (!egCnt_) egCnt_ = 1;
    for (auto& c : ch_) egStep(c);
  }
  // temporizadores: A conta (1024 - TA) amostras, B conta 16 x (256 - TB)
  if (ctl14_ & 1) {
    if (++taCnt_ >= 1024 - ta_) {
      taCnt_ = 0;
      if (ctl14_ & 4) status_ |= 1;
    }
  }
  if (ctl14_ & 2) {
    if (++tbSub_ >= 16) {
      tbSub_ = 0;
      if (++tbCnt_ >= 256 - tb_) { tbCnt_ = 0; if (ctl14_ & 8) status_ |= 2; }
    }
  }
  int L = 0, R = 0;
  for (int ci = 0; ci < 8; ci++) {
    Ch& c = ch_[ci];
    updatePhase(c, pm);
    const int amv = c.ams ? lfoAm_ << (c.ams - 1) : 0;
    Op* o = c.op;
    // S1 = M1 = op[0], S2 = C1 = op[2], S3 = M2 = op[1], S4 = C2 = op[3]
    const int fbIn = c.fb ? (c.fb1 + c.fb2) >> (10 - c.fb) : 0;
    const int s1 = opCalc(o[0], fbIn, amv);
    c.fb2 = c.fb1; c.fb1 = s1;
    int s2, s3, s4 = 0, out;
    const bool noise = ci == 7 && noiseEn_;
    auto last = [&](int mod) -> int {
      if (!noise) return opCalc(o[3], mod, amv);
      o[3].phase = (o[3].phase + o[3].inc) & 0xFFFFF;
      const int lvl = envLevel(o[3], amv) << 2;
      const int v = lvl < 13 * 256 ? ((T.exp[(lvl & 0xFF) ^ 0xFF] | 0x400) << 2) >> (lvl >> 8) : 0;
      return noiseState_ ? -v : v;
    };
    switch (c.con) {
      case 0: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], s2 >> 1, amv); out = last(s3 >> 1); break;
      case 1: s2 = opCalc(o[2], 0, amv); s3 = opCalc(o[1], (s1 + s2) >> 1, amv); out = last(s3 >> 1); break;
      case 2: s2 = opCalc(o[2], 0, amv); s3 = opCalc(o[1], s2 >> 1, amv); out = last((s1 + s3) >> 1); break;
      case 3: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], 0, amv); out = last((s2 + s3) >> 1); break;
      case 4: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], 0, amv); s4 = last(s3 >> 1); out = s2 + s4; break;
      case 5: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], s1 >> 1, amv); s4 = last(s1 >> 1); out = s2 + s3 + s4; break;
      case 6: s2 = opCalc(o[2], s1 >> 1, amv); s3 = opCalc(o[1], 0, amv); s4 = last(0); out = s2 + s3 + s4; break;
      default: s2 = opCalc(o[2], 0, amv); s3 = opCalc(o[1], 0, amv); s4 = last(0); out = s1 + s2 + s3 + s4; break;
    }
    if (out > 8191) out = 8191;
    if (out < -8192) out = -8192;
    if (c.left) L += out;
    if (c.right) R += out;
  }
  outL = L > 32767 ? 32767 : L < -32768 ? -32768 : L;
  outR = R > 32767 ? 32767 : R < -32768 ? -32768 : R;
}

void YM2151::sync(StateIO& io) {
  for (auto& c : ch_) {
    for (auto& o : c.op) {
      io.sync(o.phase); io.sync(o.inc); io.sync(o.dt1); io.sync(o.dt2); io.sync(o.mul); io.sync(o.tl); io.sync(o.ks);
      io.sync(o.ar); io.sync(o.d1r); io.sync(o.d2r); io.sync(o.d1l); io.sync(o.rr); io.sync(o.amEn); io.sync(o.env);
      io.sync(o.state); io.sync(o.key);
      if (io.reading()) {
        o.phase &= 0xFFFFF; o.inc &= 0xFFFFF; o.dt1 &= 7; o.dt2 &= 3; o.mul &= 15; o.tl &= 0x7F; o.ks &= 3;
        o.ar &= 31; o.d1r &= 31; o.d2r &= 31; o.d1l &= 15; o.rr &= 15; o.amEn &= 1; clampTo(o.env, 0, 0x3FF); o.state &= 3;
      }
    }
    io.sync(c.kc); io.sync(c.kf); io.sync(c.con); io.sync(c.fb); io.sync(c.pms); io.sync(c.ams);
    io.sync(c.left); io.sync(c.right); io.sync(c.fb1); io.sync(c.fb2);
    if (io.reading()) {
      c.dirty = true;
      c.kc &= 0x7F; c.kf &= 63; c.con &= 7; c.fb &= 7; c.pms &= 7; c.ams &= 3;
      clampTo(c.fb1, -8192, 8191); clampTo(c.fb2, -8192, 8191);
    }
  }
  io.sync(addr_); io.sync(ta_); io.sync(tb_); io.sync(taCnt_); io.sync(tbCnt_); io.sync(tbSub_); io.sync(ctl14_);
  io.sync(status_); io.sync(lfrq_); io.sync(pmd_); io.sync(amd_); io.sync(wave_); io.sync(test_); io.sync(lfoCnt_);
  io.sync(lfoAm_); io.sync(noiseEn_); io.sync(noiseFreq_); io.sync(noiseCnt_); io.sync(noiseState_); io.sync(noiseLfsr_);
  io.sync(egCnt_); io.sync(egSub_);
  if (io.reading()) {
    addr_ &= 0xFF; ta_ &= 0x3FF; tb_ &= 0xFF; clampTo(taCnt_, 0, 1023); clampTo(tbCnt_, 0, 255); clampTo(tbSub_, 0, 15);
    ctl14_ &= 0x8F; status_ &= 3; lfrq_ &= 0xFF; pmd_ &= 0x7F; amd_ &= 0x7F; wave_ &= 3; test_ &= 0xFF;
    clampTo(lfoAm_, 0, 255); noiseEn_ &= 1; noiseFreq_ &= 31; clampTo(noiseCnt_, 0, 32); noiseState_ &= 1;
    noiseLfsr_ &= 0x1FFFFFF; egCnt_ &= 0xFFF; clampTo(egSub_, 0, 2);
  }
}

}  // namespace indigo::cps
