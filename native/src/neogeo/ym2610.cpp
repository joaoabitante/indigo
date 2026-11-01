#include "ym2610.h"

#include <algorithm>
#include <cmath>

namespace indigo::neogeo {

namespace {
struct Tables {
  uint16_t logsin[256], exp[256];
  int ssgVol[32];
  Tables() {
    for (int i = 0; i < 256; i++) {
      const double s = std::sin((i + 0.5) * 3.14159265358979323846 / 512.0);
      logsin[i] = static_cast<uint16_t>(std::lround(-std::log2(s) * 256.0));
      exp[i] = static_cast<uint16_t>(std::lround((std::pow(2.0, i / 256.0) - 1.0) * 1024.0));
    }
    // SSG: 32 níveis de 1,5 dB (YM2149); o máximo de um canal fica em 1/3 da escala (volume medido no Geolith)
    ssgVol[0] = 0;
    for (int i = 1; i < 32; i++) ssgVol[i] = static_cast<int>(std::lround(10922.0 * std::pow(10.0, -(31 - i) * 1.5 / 20.0)));
  }
};
const Tables T;

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
const int LFO_PERIOD[8] = {108, 77, 71, 67, 62, 44, 8, 5};
const int AM_SHIFT[4] = {8, 3, 1, 0};

// ADPCM-A (tabela de passos da OKI) e ADPCM-B (multiplicadores do passo, em 1/64)
const int16_t A_STEPS[49] = {16,  17,  19,  21,  23,  25,  28,  31,  34,  37,  41,  45,  50,  55,  60,  66,  73,
                             80,  88,  97,  107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
                             408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552};
const int8_t A_INC[8] = {-1, -1, -1, -1, 2, 5, 7, 9};
const int16_t B_STEPS[8] = {57, 57, 57, 57, 77, 102, 128, 153};

int keyCode(int fnum, int block) {
  const int b10 = (fnum >> 10) & 1, b9 = (fnum >> 9) & 1, b8 = (fnum >> 8) & 1, b7 = (fnum >> 7) & 1;
  const int n3 = b10 ? (b9 | b8 | b7) : (b9 & b8 & b7);
  return (block << 2) | (b10 << 1) | n3;
}
}  // namespace

YM2610::YM2610() : bufL_(BUF), bufR_(BUF) { reset(); }

void YM2610::reset() {
  for (auto& c : ch_) c = Ch();
  addr_[0] = addr_[1] = 0; latchHi_[0] = latchHi_[1] = 0; latch3Hi_ = 0;
  for (int i = 0; i < 3; i++) { ch3fnum_[i] = 0; ch3block_[i] = 0; ch3kc_[i] = 0; }
  mode27_ = 0; ta_ = tb_ = taCnt_ = tbCnt_ = tbSub_ = 0; status_ = 0;
  lfoOn_ = lfoFreq_ = lfoCnt_ = lfoSub_ = 0;
  egCnt_ = 0; egSub_ = 0; csmKey_ = false;
  std::fill(std::begin(ssgReg_), std::end(ssgReg_), 0);
  for (int i = 0; i < 3; i++) { toneCnt_[i] = 0; toneOut_[i] = 0; }
  noiseCnt_ = noiseSub_ = 0; noiseLfsr_ = 1;
  envCnt_ = envStep_ = envSub_ = 0; envHold_ = envAlt_ = envAtt_ = envDone_ = false;
  ssgAcc_ = 0;
  for (auto& a : a_) a = AdpcmA();
  aTotal_ = aSub_ = aOutL_ = aOutR_ = 0;
  std::fill(std::begin(aReg_), std::end(aReg_), 0);
  std::fill(std::begin(bReg_), std::end(bReg_), 0);
  bOn_ = false; bAddr_ = bPos_ = 0; bAcc_ = bPrev_ = 0; bStep_ = 127;
  flagMask_ = 0; eos_ = 0;
  clkSub_ = 0;
  curL_ = curR_ = 0; frac_ = 0;
  wpos_ = rpos_ = 0;
}

void YM2610::setRom(const uint8_t* a, size_t na, const uint8_t* b, size_t nb) {
  romA_ = a; sizeA_ = a ? na : 0;
  romB_ = b; sizeB_ = b ? nb : 0;
}

void YM2610::setRates(double chipRate, int outRate) {
  chipRate_ = chipRate;
  outRate_ = outRate > 0 ? outRate : 48000;
}

uint8_t YM2610::read(int port) {
  switch (port & 3) {
    case 0: return static_cast<uint8_t>(status_ & 3);
    case 1: return addr_[0] < 0x10 ? ssgReg_[addr_[0]] : 0;
    case 2: return eos_;
    default: return 0;
  }
}

void YM2610::write(int port, uint8_t v) {
  const int part = (port >> 1) & 1;
  if (!(port & 1)) { addr_[part] = v; return; }
  const int r = addr_[part];
  if (part == 0) {
    if (r < 0x10) writeSsg(r, v);
    else if (r < 0x20) writeAdpcmB(r, v);
    else writeFm(0, r, v);
  } else {
    if (r < 0x30) writeAdpcmA(r, v);
    else writeFm(1, r, v);
  }
}

// ------------------------------------------------------------------ SSG
void YM2610::writeSsg(int r, uint8_t v) {
  static const uint8_t MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                   0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
  ssgReg_[r] = v & MASK[r];
  if (r == 13) {
    envStep_ = 0; envSub_ = 0; envCnt_ = 0;
    envAtt_ = (v & 4) != 0;
    envDone_ = false;
  }
}

int YM2610::ssgSample() {
  // 4,5 passos de 250 kHz (8 MHz / 32) por amostra; a saída é a média dos passos
  ssgAcc_ += 9;
  int sum = 0, n = 0;
  while (ssgAcc_ >= 2) {
    ssgAcc_ -= 2;
    for (int c = 0; c < 3; c++) {
      int per = ssgReg_[c * 2] | (ssgReg_[c * 2 + 1] << 8);
      if (per == 0) per = 1;
      if (++toneCnt_[c] >= per) { toneCnt_[c] = 0; toneOut_[c] ^= 1; }
    }
    if (++noiseSub_ >= 2) {  // ruído a 125 kHz
      noiseSub_ = 0;
      int per = ssgReg_[6];
      if (per == 0) per = 1;
      if (++noiseCnt_ >= per) {
        noiseCnt_ = 0;
        const uint32_t bit = (noiseLfsr_ ^ (noiseLfsr_ >> 3)) & 1;
        noiseLfsr_ = (noiseLfsr_ >> 1) | (bit << 16);
      }
    }
    // envelope: 32 passos a 250 kHz / período
    int eper = ssgReg_[11] | (ssgReg_[12] << 8);
    if (eper == 0) eper = 1;
    if (!envDone_ && ++envCnt_ >= eper) {
      envCnt_ = 0;
      if (++envStep_ > 31) {
        const uint8_t shape = ssgReg_[13];
        if (!(shape & 8)) { envDone_ = true; envAtt_ = false; envStep_ = 31; }       // vai a zero e fica
        else if (shape & 1) { envDone_ = true; if (shape & 2) envAtt_ = !envAtt_; envStep_ = 31; }  // segura
        else { if (shape & 2) envAtt_ = !envAtt_; envStep_ = 0; }                       // repete
      }
    }
    const int envLevel = envAtt_ ? envStep_ : 31 - envStep_;
    const int noise = noiseLfsr_ & 1;
    const uint8_t mix = ssgReg_[7];
    for (int c = 0; c < 3; c++) {
      const bool t = (mix >> c) & 1 ? true : toneOut_[c] != 0;
      const bool nz = (mix >> (c + 3)) & 1 ? true : noise != 0;
      if (!(t && nz)) continue;
      const uint8_t vr = ssgReg_[8 + c];
      const int lvl = (vr & 0x10) ? envLevel : ((vr & 15) ? (vr & 15) * 2 + 1 : 0);
      sum += T.ssgVol[lvl];
    }
    n++;
  }
  return n ? sum / n : 0;
}

// ------------------------------------------------------------------ ADPCM-A
void YM2610::writeAdpcmA(int r, uint8_t v) {
  aReg_[r] = v;
  if (r == 0x00) {
    if (v & 0x80) {  // "dump": desliga os canais marcados
      for (int c = 0; c < 6; c++) if (v & (1 << c)) a_[c].on = false;
    } else {
      for (int c = 0; c < 6; c++) {
        if (!(v & (1 << c))) continue;
        AdpcmA& a = a_[c];
        a.on = true;
        a.start = static_cast<uint32_t>(aReg_[0x10 + c] | (aReg_[0x18 + c] << 8)) << 9;
        a.end = ((static_cast<uint32_t>(aReg_[0x20 + c] | (aReg_[0x28 + c] << 8)) << 9)) | 0x1FF;
        a.addr = a.start;
        a.acc = 0;
        a.step = 0;
      }
    }
  } else if (r == 0x01) {
    aTotal_ = v & 0x3F;
  } else if (r >= 0x08 && r <= 0x0D) {
    AdpcmA& a = a_[r - 8];
    a.left = (v & 0x80) != 0;
    a.right = (v & 0x40) != 0;
    a.level = v & 0x1F;
  }
}

void YM2610::adpcmASample() {
  int L = 0, R = 0;
  for (int c = 0; c < 6; c++) {
    AdpcmA& a = a_[c];
    if (!a.on) continue;
    if (a.addr > a.end) {
      a.on = false;
      if (!(flagMask_ & (1 << c))) eos_ |= static_cast<uint8_t>(1 << c);
      continue;
    }
    const uint8_t b = romA(a.addr >> 1);
    const int nib = (a.addr & 1) ? (b & 15) : (b >> 4);
    a.addr++;
    int delta = (2 * (nib & 7) + 1) * A_STEPS[a.step] / 8;
    if (nib & 8) delta = -delta;
    a.acc = (a.acc + delta) & 0xFFF;  // acumulador de 12 bits com volta (como no chip)
    a.step = std::clamp(a.step + A_INC[nib & 7], 0, 48);
    const int vol = (a.level ^ 0x1F) + (aTotal_ ^ 0x3F);
    if (vol >= 63) continue;
    const int mul = 15 - (vol & 7), shift = 4 + 1 + (vol >> 3);
    const int value = ((static_cast<int16_t>(a.acc << 4) * mul) >> shift) & ~3;
    if (a.left) L += value;
    if (a.right) R += value;
  }
  aOutL_ = L;
  aOutR_ = R;
}

// ------------------------------------------------------------------ ADPCM-B
void YM2610::writeAdpcmB(int r, uint8_t v) {
  if (r == 0x1C) {  // controle das bandeiras de fim: 1 apaga e mascara
    flagMask_ = v;
    eos_ &= static_cast<uint8_t>(~v);
    return;
  }
  bReg_[r & 15] = v;
  if (r == 0x10) {
    if (v & 1) { bOn_ = false; return; }
    if (v & 0x80) {
      bOn_ = true;
      bAddr_ = static_cast<uint32_t>(bReg_[2] | (bReg_[3] << 8)) << 9;
      bPos_ = 0;
      bAcc_ = bPrev_ = 0;
      bStep_ = 127;
    } else {
      bOn_ = false;
    }
  }
}

void YM2610::adpcmBStep() {
  const uint32_t end = (static_cast<uint32_t>(bReg_[4] | (bReg_[5] << 8)) << 9) | 0x1FF;
  if (bAddr_ > end) {
    if (bReg_[0] & 0x10) {
      bAddr_ = static_cast<uint32_t>(bReg_[2] | (bReg_[3] << 8)) << 9;
      bAcc_ = bPrev_ = 0;
      bStep_ = 127;
    } else {
      bOn_ = false;
      bAcc_ = bPrev_ = 0;
      if (!(flagMask_ & 0x80)) eos_ |= 0x80;
      return;
    }
  }
  const uint8_t b = romB(bAddr_ >> 1);
  const int nib = (bAddr_ & 1) ? (b & 15) : (b >> 4);
  bAddr_++;
  bPrev_ = bAcc_;
  int delta = (2 * (nib & 7) + 1) * bStep_ / 8;
  if (nib & 8) delta = -delta;
  bAcc_ = std::clamp(bAcc_ + delta, -32768, 32767);
  bStep_ = std::clamp((bStep_ * B_STEPS[nib & 7]) / 64, 127, 24576);
}

// ------------------------------------------------------------------ FM (derivado do YM2612 do Mega Drive)
void YM2610::keyOn(Ch& c, int slot, bool on) {
  Op& o = c.op[slot];
  if (on && !o.key) {
    o.key = true;
    o.phase = 0;
    o.ssgInv = (o.ssg & 8) && (o.ssg & 4);
    o.state = 0;
    if (rateOf(o, o.ar, c.kc) >= 62) { o.env = 0; o.state = 1; }
  } else if (!on && o.key) {
    o.key = false;
    if ((o.ssg & 8) && o.ssgInv) o.env = (0x200 - o.env) & 0x3FF;
    o.ssgInv = false;
    o.state = 3;
  }
}

void YM2610::writeFm(int part, int r, uint8_t v) {
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
        if (c == 0 || c == 3) break;  // o YM2610 só tem os canais 2, 3, 5 e 6
        if (v & 4) c += 3;
        Ch& ch = ch_[c];
        keyOn(ch, 0, (v & 0x10) != 0);
        keyOn(ch, 2, (v & 0x20) != 0);
        keyOn(ch, 1, (v & 0x40) != 0);
        keyOn(ch, 3, (v & 0x80) != 0);
        break;
      }
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

int YM2610::rateOf(const Op& o, int r, int kc) const {
  if (!r) return 0;
  const int rate = 2 * r + (kc >> (3 - o.ks));
  return rate > 63 ? 63 : rate;
}

void YM2610::updatePhaseInc(Ch& c, int chIdx) {
  const int pm = lfoOn_ ? (lfoCnt_ >> 2) : 0;
  for (int s = 0; s < 4; s++) {
    Op& o = c.op[s];
    int fnum = c.fnum, block = c.block, kc = c.kc;
    if (chIdx == 2 && (mode27_ & 0xC0) && s != 3) {
      const int k = s == 0 ? 1 : s == 1 ? 0 : 2;
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
      const int k = kc > 0x1C ? 0x1C : kc;
      const int sum = (k >> 2) + 9 + ((dtl == 3) | (dtl & 2));
      const int det = DETUNE[((sum & 1) << 2) | (k & 3)] >> (9 - (sum >> 1));
      if (o.dt & 4) base -= static_cast<uint32_t>(det); else base += static_cast<uint32_t>(det);
    }
    base &= 0x1FFFF;
    const uint32_t mul = o.mul ? static_cast<uint32_t>(o.mul) * 2 : 1;
    o.inc = ((base * mul) >> 1) & 0xFFFFF;
  }
}

void YM2610::egStep(Ch& c) {
  for (int s = 0; s < 4; s++) {
    Op& o = c.op[s];
    const int kc = c.kc;
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
      if (rate >= 62) o.env = 0;
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

int YM2610::opCalc(Op& o, int mod, int am) {
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

int YM2610::fmSample(int& L, int& R) {
  int am = 0;
  if (lfoOn_) {
    if (++lfoSub_ >= LFO_PERIOD[lfoFreq_]) { lfoSub_ = 0; lfoCnt_ = (lfoCnt_ + 1) & 127; }
    am = (lfoCnt_ < 64 ? lfoCnt_ : 127 - lfoCnt_) * 2;
  }
  if (++egSub_ >= 3) {
    egSub_ = 0;
    egCnt_ = (egCnt_ + 1) & 0xFFF;
    if (!egCnt_) egCnt_ = 1;
    for (int ci : {1, 2, 4, 5}) egStep(ch_[ci]);
  }
  if (mode27_ & 1) {
    if (++taCnt_ >= 1024 - ta_) {
      taCnt_ = 0;
      if (mode27_ & 4) status_ |= 1;
      if ((mode27_ & 0xC0) == 0x80) {
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
  for (int ci : {1, 2, 4, 5}) {
    Ch& c = ch_[ci];
    if (c.dirty || (lfoOn_ && c.pms)) { updatePhaseInc(c, ci); c.dirty = lfoOn_ && c.pms; }
    const int amv = am >> AM_SHIFT[c.ams];
    Op* o = c.op;
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
    out = std::clamp(out, -8192, 8191) >> 1;
    if (c.left) L += out;
    if (c.right) R += out;
  }
  if (csmKey_) {
    csmKey_ = false;
    for (int s = 0; s < 4; s++) keyOn(ch_[2], s, false);
  }
  return 0;
}

void YM2610::sample() {
  int L = 0, R = 0;
  fmSample(L, R);
  if (++aSub_ >= 3) { aSub_ = 0; adpcmASample(); }  // ADPCM-A a 1/3 da taxa do FM
  L += aOutL_;
  R += aOutR_;
  if (bOn_) {
    const uint32_t dn = bReg_[9] | (bReg_[10] << 8);
    bPos_ += dn;
    while (bPos_ >= 65536 && bOn_) { bPos_ -= 65536; adpcmBStep(); }
    if (bOn_) {
      const int v = bPrev_ + static_cast<int>((static_cast<int64_t>(bAcc_ - bPrev_) * bPos_) >> 16);
      const int out = (v * bReg_[11]) >> 9;
      if (bReg_[1] & 0x80) L += out;
      if (bReg_[1] & 0x40) R += out;
    }
  }
  const int ssg = ssgSample();
  L += ssg;
  R += ssg;
  // reamostragem por média na janela de cada amostra de saída
  const float xl = std::clamp(static_cast<float>(L) * (1.0f / 32768.0f), -1.0f, 1.0f);
  const float xr = std::clamp(static_cast<float>(R) * (1.0f / 32768.0f), -1.0f, 1.0f);
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

void YM2610::run(int64_t toClock) {
  int64_t n = toClock - clock_;
  if (n <= 0) return;
  clock_ = toClock;
  n += clkSub_;
  int64_t samples = n / CYC_PER_SAMPLE;
  clkSub_ = static_cast<int>(n % CYC_PER_SAMPLE);
  while (samples-- > 0) sample();
}

void YM2610::pull(float* L, float* R, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos_ == wpos_) { L[k] = R[k] = 0; continue; }
    L[k] = bufL_[rpos_]; R[k] = bufR_[rpos_];
    rpos_ = (rpos_ + 1) % BUF;
  }
}

void YM2610::sync(StateIO& io) {
  for (auto& c : ch_) {
    for (auto& o : c.op) {
      io.sync(o.phase); io.sync(o.inc); io.sync(o.dt); io.sync(o.mul); io.sync(o.tl); io.sync(o.ks); io.sync(o.ar);
      io.sync(o.d1r); io.sync(o.d2r); io.sync(o.d1l); io.sync(o.rr); io.sync(o.ssg); io.sync(o.am); io.sync(o.env);
      io.sync(o.state); io.sync(o.key); io.sync(o.ssgInv);
      if (io.reading()) {
        o.phase &= 0xFFFFF; o.inc &= 0xFFFFF; o.dt &= 7; o.mul &= 15; o.tl &= 0x7F; o.ks &= 3; o.ar &= 31; o.d1r &= 31;
        o.d2r &= 31; o.d1l &= 15; o.rr &= 15; o.ssg &= 15; o.am &= 1; clampTo(o.env, 0, 0x3FF); o.state &= 3;
      }
    }
    io.sync(c.fnum); io.sync(c.block); io.sync(c.kc); io.sync(c.alg); io.sync(c.fb); io.sync(c.ams); io.sync(c.pms);
    io.sync(c.dirty); io.sync(c.left); io.sync(c.right); io.sync(c.fb1); io.sync(c.fb2);
    if (io.reading()) {
      c.fnum &= 0x7FF; c.block &= 7; c.kc &= 31; c.alg &= 7; c.fb &= 7; c.ams &= 3; c.pms &= 7;
      clampTo(c.fb1, -8192, 8191); clampTo(c.fb2, -8192, 8191);
    }
  }
  io.raw(addr_, sizeof addr_); io.raw(latchHi_, sizeof latchHi_); io.sync(latch3Hi_);
  io.raw(ch3fnum_, sizeof ch3fnum_); io.raw(ch3block_, sizeof ch3block_); io.raw(ch3kc_, sizeof ch3kc_);
  io.sync(mode27_); io.sync(ta_); io.sync(tb_); io.sync(taCnt_); io.sync(tbCnt_); io.sync(tbSub_); io.sync(status_);
  io.sync(lfoOn_); io.sync(lfoFreq_); io.sync(lfoCnt_); io.sync(lfoSub_);
  io.sync(egCnt_); io.sync(egSub_); io.sync(csmKey_);
  // SSG
  io.raw(ssgReg_, sizeof ssgReg_); io.raw(toneCnt_, sizeof toneCnt_); io.raw(toneOut_, sizeof toneOut_);
  io.sync(noiseCnt_); io.sync(noiseSub_); io.sync(noiseLfsr_); io.sync(envCnt_); io.sync(envStep_); io.sync(envSub_);
  io.sync(envHold_); io.sync(envAlt_); io.sync(envAtt_); io.sync(envDone_); io.sync(ssgAcc_);
  // ADPCM
  for (auto& a : a_) {
    io.sync(a.on); io.sync(a.left); io.sync(a.right); io.sync(a.level); io.sync(a.start); io.sync(a.end); io.sync(a.addr);
    io.sync(a.acc); io.sync(a.step);
    if (io.reading()) { a.level &= 31; a.acc &= 0xFFF; clampTo(a.step, 0, 48); }
  }
  io.sync(aTotal_); io.sync(aSub_); io.sync(aOutL_); io.sync(aOutR_); io.raw(aReg_, sizeof aReg_);
  io.raw(bReg_, sizeof bReg_); io.sync(bOn_); io.sync(bAddr_); io.sync(bPos_); io.sync(bAcc_); io.sync(bPrev_); io.sync(bStep_);
  io.sync(flagMask_); io.sync(eos_);
  io.sync(clock_); io.sync(clkSub_);
  io.sync(frac_); io.sync(curL_); io.sync(curR_);
  if (io.reading()) {
    for (int k = 0; k < 2; k++) { addr_[k] &= 0xFF; latchHi_[k] &= 0x3F; }
    latch3Hi_ &= 0x3F;
    for (int k = 0; k < 3; k++) {
      ch3fnum_[k] &= 0x7FF; ch3block_[k] &= 7; ch3kc_[k] &= 31;
      clampTo(toneCnt_[k], 0, 4095); toneOut_[k] &= 1;
    }
    mode27_ &= 0xFF; ta_ &= 0x3FF; tb_ &= 0xFF; clampTo(taCnt_, 0, 1023); clampTo(tbCnt_, 0, 255); clampTo(tbSub_, 0, 15);
    status_ &= 3; lfoOn_ &= 1; lfoFreq_ &= 7; lfoCnt_ &= 127; clampTo(lfoSub_, 0, 108);
    egCnt_ &= 0xFFF; clampTo(egSub_, 0, 2); clampTo(clkSub_, 0, CYC_PER_SAMPLE - 1);
    clampTo(noiseCnt_, 0, 31); noiseSub_ &= 1; noiseLfsr_ &= 0x1FFFF; clampTo(envCnt_, 0, 65535); clampTo(envStep_, 0, 31);
    clampTo(ssgAcc_, 0, 10);
    aTotal_ &= 0x3F; clampTo(aSub_, 0, 2); clampTo(aOutL_, -200000, 200000); clampTo(aOutR_, -200000, 200000);
    clampTo(bAcc_, -32768, 32767); clampTo(bPrev_, -32768, 32767); clampTo(bStep_, 127, 24576); bPos_ &= 0xFFFFF;
    finiteOr(frac_, 0); clampTo(frac_, 0.0, chipRate_ / outRate_ + 1.0);
    for (float* f : {&curL_, &curR_}) if (!(*f == *f) || *f > 1000 || *f < -1000) *f = 0;
  }
}

}  // namespace indigo::neogeo
