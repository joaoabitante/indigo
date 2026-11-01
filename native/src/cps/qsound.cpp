#include "qsound.h"

#include <cstdlib>
#include <cstring>

namespace indigo::cps {

namespace {
template <typename T>
inline T clampv(T x, T lo, T hi) { return x > hi ? hi : x < lo ? lo : x; }

// tabelas da ROM do DSP (DL-1425), como publicadas por superctr/Valley Bell
const int16_t DRY_MIX[33] = {-16384, -16384, -16384, -16384, -16384, -16384, -16384, -16384, -16384, -16384, -16384,
                             -16384, -16384, -16384, -16384, -16384, -16384, -14746, -13107, -11633, -10486, -9175,
                             -8520,  -7209,  -6226,  -5226,  -4588,  -3768,  -3277,  -2703,  -2130,  -1802,  0};
const int16_t WET_MIX[33] = {0,      -1638,  -1966,  -2458,  -2949,  -3441,  -4096,  -4669,  -4915,  -5120,  -5489,
                             -6144,  -7537,  -8831,  -9339,  -9830,  -10240, -10322, -10486, -10568, -10650, -11796,
                             -12288, -12288, -12534, -12648, -12780, -12829, -12943, -13107, -13418, -14090, -16384};
const int16_t LINEAR_MIX[33] = {-16379, -16338, -16257, -16135, -15973, -15772, -15531, -15251, -14934, -14580, -14189,
                                -13763, -13303, -12810, -12284, -11729, -11729, -11144, -10531, -9893,  -9229,  -8543,
                                -7836,  -7109,  -6364,  -5604,  -4829,  -4043,  -3246,  -2442,  -1631,  -817,   0};
const int16_t FILTER[5][95] = {
    {0,    0,    0,    6,    44,   -24,  -53,  -10,  59,   -40,  -27,  1,    39,   -27,  56,   127,  174,  36,   -13,
     49,   212,  142,  143,  -73,  -20,  66,   -108, -117, -399, -265, -392, -569, -473, -71,  95,   -319, -218, -230,
     331,  638,  449,  477,  -180, 532,  1107, 750,  9899, 3828, -2418, 1071, -176, 191,  -431, 64,   117,  -150, -274,
     -97,  -238, 165,  166,  250,  -19,  4,    37,   204,  186,  -6,   140,  -77,  -1,   1,    18,   -10,  -151, -149,
     -103, -9,   55,   23,   -102, -97,  -11,  13,   -48,  -27,  5,    18,   -61,  -30,  64,   72,   0,    0,    0},
    {0,    0,    0,    85,   24,   -76,  -123, -86,  -29,  -14,  -20,  -7,   6,    -28,  -87,  -89,  -5,   100,  154,
     160,  150,  118,  41,   -48,  -78,  -23,  59,   83,   -2,   -176, -333, -344, -203, -66,  -39,  2,    224,  495,
     495,  280,  432,  1340, 2483, 5377, 1905, 658,  0,    97,   347,  285,  35,   -95,  -78,  -82,  -151, -192, -171,
     -149, -147, -113, -22,  71,   118,  129,  127,  110,  71,   31,   20,   36,   46,   23,   -27,  -63,  -53,  -21,
     -19,  -60,  -92,  -69,  -12,  25,   29,   30,   40,   41,   29,   30,   46,   39,   -15,  -74,  0,    0,    0},
    {0,    0,    0,    23,   42,   47,   29,   10,   2,    -14,  -54,  -92,  -93,  -70,  -64,  -77,  -57,  18,   94,
     113,  87,   69,   67,   50,   25,   29,   58,   62,   24,   -39,  -131, -256, -325, -234, -45,  58,   78,   223,
     485,  496,  127,  6,    857,  2283, 2683, 4928, 1328, 132,  79,   314,  189,  -80,  -90,  35,   -21,  -186, -195,
     -99,  -136, -258, -189, 82,   257,  185,  53,   41,   84,   68,   38,   63,   77,   14,   -60,  -71,  -71,  -120,
     -151, -84,  14,   29,   -8,   7,    66,   69,   12,   -3,   54,   92,   52,   -6,   -15,  -2,   0,    0,    0},
    {0,    0,    0,    2,    -28,  -37,  -17,  0,    -9,   -22,  -3,   35,   52,   39,   20,   7,    -6,   2,    55,
     121,  129,  67,   8,    1,    9,    -6,   -16,  16,   66,   96,   118,  130,  75,   -47,  -92,  43,   223,  239,
     151,  219,  440,  475,  226,  206,  940,  2100, 2663, 4980, 865,  49,   -33,  186,  231,  103,  42,   114,  191,
     184,  116,  29,   -47,  -72,  -21,  60,   96,   68,   31,   32,   63,   87,   76,   39,   7,    14,   55,   85,
     67,   18,   -12,  -3,   21,   34,   29,   6,    -27,  -49,  -37,  -2,   16,   0,    -21,  -16,  0,    0,    0},
    {0,    0,    0,    48,   7,    -22,  -29,  -10,  24,   54,   59,   29,   -36,  -117, -185, -213, -185, -99,  13,
     90,   83,   24,   -5,   23,   53,   47,   38,   56,   67,   57,   75,   107,  16,   -242, -440, -355, -120, -33,
     -47,  152,  501,  472,  -57,  -292, 544,  1937, 2277, 6145, 1240, 153,  47,   200,  152,  36,   64,   134,  74,
     -82,  -208, -266, -268, -188, -42,  65,   74,   56,   89,   133,  114,  44,   -3,   -1,   17,   29,   29,   -2,
     -76,  -156, -187, -151, -85,  -31,  -5,   7,    20,   32,   24,   -5,   -20,  6,    48,   62,   0,    0,    0}};
// 0xF2E: 95 zeros (saída desligada); 0xF73: filtro do modo 2 (sobreposto); 0xFA0: filtro "desligado"
int16_t FILTER2[209];
struct Filter2Init {
  Filter2Init() {
    static const int16_t mode2[19] = {-371, -196, -268, -512, -303, -315, -184, -76, 276, -256,
                                      298,  196,  990,  236,  1114, -126, 4377, 6549, 791};
    std::memset(FILTER2, 0, sizeof FILTER2);
    // 69 zeros + 26 zeros, depois os 19 valores do modo 2 (posições 95..113), depois o filtro de passagem
    for (int i = 0; i < 19; i++) FILTER2[95 + i] = mode2[i];
    FILTER2[114 + 47] = -16384;
  }
};
const Filter2Init kFilter2Init;
const int16_t ADPCM_STEP[16] = {154, 154, 128, 102, 77, 58, 58, 58, 58, 58, 58, 58, 77, 102, 128, 154};

enum : uint16_t {
  ST_INIT1 = 0x288, ST_INIT2 = 0x61A, ST_REFRESH1 = 0x039, ST_REFRESH2 = 0x04F, ST_NORMAL1 = 0x314, ST_NORMAL2 = 0x6B2
};

const int16_t* filterTable(uint16_t off, int n) {
  if (off >= 0xF2E && off < 0xFFF) {
    const int i = off - 0xF2E;
    return i + n <= 209 ? &FILTER2[i] : nullptr;
  }
  const int idx = (static_cast<int>(off) - 0xD53) / 95;
  if (off >= 0xD53 && idx >= 0 && idx < 5) return FILTER[idx];
  return nullptr;
}
}  // namespace

QSound::QSound() {
  for (int i = 0; i < 33; i++) {
    panTab_[0][0][i] = DRY_MIX[i];
    panTab_[1][0][i] = DRY_MIX[32 - i];
    panTab_[0][1][i] = WET_MIX[i];
    panTab_[1][1][i] = WET_MIX[32 - i];
    panTab_[0][0][i + 0x30] = LINEAR_MIX[i];
    panTab_[1][0][i + 0x30] = LINEAR_MIX[32 - i];
  }
  reset();
}

void QSound::reset() {
  ready_ = 0;
  out_[0] = out_[1] = 0;
  state_ = 0;
  counter_ = 0;
  latch_ = 0;
  next_ = 0;
  delayUpd_ = 0;
  for (auto& v : voice_) v = Voice();
  for (auto& a : adpcm_) a = Adpcm();
  for (int i = 0; i < 19; i++) { pan_[i] = 0; vout_[i] = 0; }
  echo_ = Echo();
  for (int c = 0; c < 2; c++) { filter_[c] = Fir(); alt_[c] = Fir(); wet_[c] = Delay(); dry_[c] = Delay(); }
}

uint16_t* QSound::regPtr(uint8_t r) {
  if (r < 0x80) {
    const int v = r >> 3;
    switch (r & 7) {
      case 0: return &voice_[(v + 1) % 16].bank;  // o banco vale para a voz seguinte
      case 1: return reinterpret_cast<uint16_t*>(&voice_[v].addr);
      case 2: return &voice_[v].rate;
      case 3: return &voice_[v].phase;
      case 4: return reinterpret_cast<uint16_t*>(&voice_[v].loopLen);
      case 5: return reinterpret_cast<uint16_t*>(&voice_[v].endAddr);
      case 6: return reinterpret_cast<uint16_t*>(&voice_[v].volume);
      default: return nullptr;
    }
  }
  if (r < 0x90) return &pan_[r - 0x80];
  if (r < 0x93) return &pan_[16 + r - 0x90];
  if (r == 0x93) return reinterpret_cast<uint16_t*>(&echo_.feedback);
  if (r >= 0xBA && r < 0xCA) return reinterpret_cast<uint16_t*>(&voice_[r - 0xBA].echo);
  if (r >= 0xCA && r < 0xD6) {
    Adpcm& a = adpcm_[(r - 0xCA) >> 2];
    switch ((r - 0xCA) & 3) {
      case 0: return &a.start;
      case 1: return &a.end;
      case 2: return &a.bank;
      default: return reinterpret_cast<uint16_t*>(&a.volume);
    }
  }
  if (r >= 0xD6 && r < 0xD9) return &adpcm_[r - 0xD6].flag;
  if (r == 0xD9) return &echo_.endPos;
  if (r >= 0xDA && r < 0xE2) {
    const int k = r - 0xDA, ch = (k >> 1) & 1;
    // DA/DC: filtro molhado E/D; DB/DD: filtro seco; DE/E0: atraso molhado; DF/E1: atraso seco
    if (k < 4) return (k & 1) ? &alt_[ch].tablePos : &filter_[ch].tablePos;
    return (k & 1) ? reinterpret_cast<uint16_t*>(&dry_[ch].delay) : reinterpret_cast<uint16_t*>(&wet_[ch].delay);
  }
  if (r == 0xE2) return &delayUpd_;
  if (r == 0xE3) return &next_;
  if (r >= 0xE4 && r < 0xE8) {
    const int k = r - 0xE4, ch = (k >> 1) & 1;
    return (k & 1) ? reinterpret_cast<uint16_t*>(&dry_[ch].volume) : reinterpret_cast<uint16_t*>(&wet_[ch].volume);
  }
  return nullptr;
}

void QSound::writeData(uint8_t reg, uint16_t v) {
  if (uint16_t* p = regPtr(reg)) *p = v;
  ready_ = 0;
}

void QSound::write(int port, uint8_t v) {
  switch (port) {
    case 0: latch_ = static_cast<uint16_t>((latch_ & 0x00FF) | (v << 8)); break;
    case 1: latch_ = static_cast<uint16_t>((latch_ & 0xFF00) | v); break;
    case 2: writeData(v, latch_); break;
    default: break;
  }
}

int16_t QSound::romSample(uint16_t bank, uint16_t addr) const {
  if (!rom_ || rom_->empty() || !(bank & 0x8000)) return 0;
  const size_t a = (static_cast<size_t>(bank & 0x7FFF) << 16) | addr;
  if (a >= rom_->size()) return 0;
  return static_cast<int16_t>((*rom_)[a] << 8);
}

void QSound::stateInit() {
  const int mode = state_ == ST_INIT2 ? 1 : 0;
  // ocupado por 4 amostras, contando a atualização dos filtros
  if (counter_ >= 2) { counter_ = 0; state_ = next_; return; }
  if (counter_ == 1) { counter_++; return; }
  for (auto& v : voice_) v = Voice();
  for (auto& a : adpcm_) a = Adpcm();
  for (int c = 0; c < 2; c++) { filter_[c] = Fir(); alt_[c] = Fir(); wet_[c] = Delay(); dry_[c] = Delay(); }
  echo_ = Echo();
  for (int i = 0; i < 19; i++) { pan_[i] = 0x120; vout_[i] = 0; }
  for (auto& v : voice_) v.bank = 0x8000;
  for (auto& a : adpcm_) a.bank = 0x8000;
  if (mode == 0) {
    wet_[0].delay = 0; dry_[0].delay = 46; wet_[1].delay = 0; dry_[1].delay = 48;
    filter_[0].tablePos = 0xDB2; filter_[1].tablePos = 0xE11;
    echo_.endPos = 0x554 + 6;
    next_ = ST_REFRESH1;
  } else {
    wet_[0].delay = 1; dry_[0].delay = 0; wet_[1].delay = 0; dry_[1].delay = 0;
    filter_[0].tablePos = 0xF73; filter_[1].tablePos = 0xFA4;
    alt_[0].tablePos = 0xF73; alt_[1].tablePos = 0xFA4;
    echo_.endPos = 0x53C + 6;
    next_ = ST_REFRESH2;
  }
  wet_[0].volume = dry_[0].volume = wet_[1].volume = dry_[1].volume = 0x3FFF;
  delayUpd_ = 1;
  ready_ = 0;
  counter_ = 1;
}

void QSound::refresh(int mode) {
  for (int c = 0; c < 2; c++) {
    Fir& f = filter_[c];
    f.pos = 0;
    f.taps = mode == 1 ? 95 : 45;
    if (const int16_t* t = filterTable(f.tablePos, f.taps)) std::memcpy(f.coef, t, sizeof(int16_t) * static_cast<size_t>(f.taps));
    if (mode == 2) {
      Fir& a = alt_[c];
      a.pos = 0;
      a.taps = 44;
      if (const int16_t* t = filterTable(a.tablePos, 44)) std::memcpy(a.coef, t, sizeof(int16_t) * 44);
    }
  }
  state_ = next_ = mode == 1 ? ST_NORMAL1 : ST_NORMAL2;
}

int16_t QSound::pcmUpdate(int vi, int32_t& echoIn) {
  Voice& v = voice_[vi];
  const int16_t out = static_cast<int16_t>((v.volume * romSample(v.bank, static_cast<uint16_t>(v.addr))) >> 14);
  echoIn += (out * v.echo) * 4;
  int32_t ph = v.rate + ((v.addr * 4096) | (v.phase >> 4));
  if ((ph >> 12) >= v.endAddr) ph -= v.loopLen * 4096;
  ph = clampv<int32_t>(ph, -0x8000000, 0x7FFFFFF);
  v.addr = static_cast<int16_t>(ph >> 12);
  v.phase = static_cast<uint16_t>((ph << 4) & 0xFFFF);
  return out;
}

void QSound::adpcmUpdate(int vi, int nibble) {
  Adpcm& v = adpcm_[vi];
  int step;
  if (!nibble) {
    if (v.cur == v.end) v.curVol = 0;
    if (v.flag) {
      vout_[16 + vi] = 0;
      v.flag = 0;
      v.stepSize = 10;
      v.curVol = v.volume;
      v.cur = v.start;
    }
    step = static_cast<int8_t>(romSample(v.bank, v.cur) >> 8);
  } else {
    step = static_cast<int8_t>(romSample(v.bank, v.cur++) >> 4);
  }
  step >>= 4;  // com sinal
  int32_t delta = ((1 + std::abs(step * 2)) * v.stepSize) >> 1;
  if (step <= 0) delta = -delta;
  delta += vout_[16 + vi];
  delta = clampv<int32_t>(delta, -32768, 32767);
  vout_[16 + vi] = static_cast<int16_t>((delta * v.curVol) >> 16);
  v.stepSize = static_cast<int16_t>(clampv<int32_t>((ADPCM_STEP[8 + step] * v.stepSize) >> 6, 1, 2000));
}

int16_t QSound::echoApply(int32_t in) {
  Echo& r = echo_;
  int32_t old = r.line[r.pos & 1023];
  const int32_t last = r.last;
  r.last = static_cast<int16_t>(old);
  old = (old + last) >> 1;
  const int32_t ns = in + ((old * r.feedback) * 4);
  r.line[r.pos & 1023] = static_cast<int16_t>(ns >> 16);
  r.pos = static_cast<int16_t>(r.pos + 1);
  if (r.pos >= r.length || r.pos < 0 || r.pos >= 1024) r.pos = 0;
  return static_cast<int16_t>(old);
}

int32_t QSound::fir(Fir& f, int16_t in) {
  int32_t out = 0;
  int t = 0;
  const int n = f.taps - 1;
  if (n <= 0) return -(f.coef[0] * in) * 4;
  if (f.pos < 0 || f.pos >= n) f.pos = 0;
  for (; t < n; t++) {
    out -= (f.coef[t] * f.line[f.pos++]) * 4;
    if (f.pos >= n) f.pos = 0;
  }
  out -= (f.coef[t] * in) * 4;
  f.line[f.pos++] = in;
  if (f.pos >= n) f.pos = 0;
  return out;
}

int32_t QSound::delay(Delay& d, int32_t in) {
  if (d.wpos < 0 || d.wpos >= 51) d.wpos = 0;
  if (d.rpos < 0 || d.rpos >= 51) d.rpos = 0;
  d.line[d.wpos++] = static_cast<int16_t>(in >> 16);
  if (d.wpos >= 51) d.wpos = 0;
  const int32_t out = d.line[d.rpos++] * d.volume;
  if (d.rpos >= 51) d.rpos = 0;
  return out;
}

void QSound::delayUpdate(Delay& d) {
  int r = (d.wpos - d.delay) % 51;
  if (r < 0) r += 51;
  d.rpos = static_cast<int16_t>(r);
}

void QSound::normalUpdate() {
  ready_ = 0x80;
  echo_.length = static_cast<int16_t>(clampv<int>(state_ == ST_NORMAL2 ? echo_.endPos - 0x53C : echo_.endPos - 0x554, 0, 1024));
  int32_t echoIn = 0;
  for (int v = 0; v < 16; v++) vout_[v] = pcmUpdate(v, echoIn);
  adpcmUpdate(counter_ % 3, counter_ / 3);
  const int16_t eo = echoApply(echoIn);
  for (int ch = 0; ch < 2; ch++) {
    // o eco sai na componente sem filtro da esquerda e na filtrada da direita
    int64_t wet = ch == 1 ? static_cast<int64_t>(eo) << 14 : 0;
    int64_t dry = ch == 0 ? static_cast<int64_t>(eo) << 14 : 0;
    for (int v = 0; v < 19; v++) {
      int pi = pan_[v] - 0x110;
      if (pi < 0 || pi > 97) pi = 97;
      dry -= static_cast<int64_t>(vout_[v]) * panTab_[ch][0][pi];
      wet -= static_cast<int64_t>(vout_[v]) * panTab_[ch][1][pi];
    }
    int32_t d32 = static_cast<int32_t>(clampv<int64_t>(dry, -0x1FFFFFFF, 0x1FFFFFFF)) * 4;
    int32_t w32 = static_cast<int32_t>(clampv<int64_t>(wet, -0x1FFFFFFF, 0x1FFFFFFF)) * 4;
    w32 = fir(filter_[ch], static_cast<int16_t>(w32 >> 16));
    if (state_ == ST_NORMAL2) d32 = fir(alt_[ch], static_cast<int16_t>(d32 >> 16));
    int64_t out = static_cast<int64_t>(delay(wet_[ch], w32)) + delay(dry_[ch], d32);
    out = (out + 0x2000) >> 14;
    out_[ch] = static_cast<int16_t>(clampv<int64_t>(out, -0x7FFF, 0x7FFF));
    if (delayUpd_) { delayUpdate(wet_[ch]); delayUpdate(dry_[ch]); }
  }
  delayUpd_ = 0;
  if (++counter_ > 5) { counter_ = 0; state_ = next_; }
}

void QSound::sample(int& L, int& R) {
  switch (state_) {
    case ST_REFRESH1: refresh(1); break;
    case ST_REFRESH2: refresh(2); break;
    case ST_NORMAL1:
    case ST_NORMAL2: normalUpdate(); break;
    default: stateInit(); break;
  }
  L = out_[0];
  R = out_[1];
}

void QSound::sync(StateIO& io) {
  io.sync(latch_); io.sync(ready_);
  for (auto& v : voice_) {
    io.sync(v.bank); io.sync(v.addr); io.sync(v.phase); io.sync(v.rate); io.sync(v.loopLen); io.sync(v.endAddr);
    io.sync(v.volume); io.sync(v.echo);
  }
  for (auto& a : adpcm_) {
    io.sync(a.start); io.sync(a.end); io.sync(a.bank); io.sync(a.volume); io.sync(a.flag); io.sync(a.curVol);
    io.sync(a.stepSize); io.sync(a.cur);
    if (io.reading()) clampTo<int16_t>(a.stepSize, 0, 2000);
  }
  io.raw(pan_, sizeof pan_); io.raw(vout_, sizeof vout_);
  io.sync(echo_.endPos); io.sync(echo_.feedback); io.sync(echo_.length); io.sync(echo_.last);
  io.raw(echo_.line, sizeof echo_.line); io.sync(echo_.pos);
  if (io.reading()) { clampTo<int16_t>(echo_.length, 0, 1024); clampTo<int16_t>(echo_.pos, 0, 1023); }
  for (Fir* f : {&filter_[0], &filter_[1], &alt_[0], &alt_[1]}) {
    io.sync(f->taps); io.sync(f->pos); io.sync(f->tablePos); io.raw(f->coef, sizeof f->coef); io.raw(f->line, sizeof f->line);
    if (io.reading()) { clampTo(f->taps, 0, 95); clampTo(f->pos, 0, 94); }
  }
  for (Delay* d : {&wet_[0], &wet_[1], &dry_[0], &dry_[1]}) {
    io.sync(d->delay); io.sync(d->volume); io.sync(d->wpos); io.sync(d->rpos); io.raw(d->line, sizeof d->line);
    if (io.reading()) { clampTo<int16_t>(d->wpos, 0, 50); clampTo<int16_t>(d->rpos, 0, 50); }
  }
  io.sync(state_); io.sync(next_); io.sync(delayUpd_); io.sync(counter_); io.raw(out_, sizeof out_);
  if (io.reading()) clampTo(counter_, 0, 5);
}

}  // namespace indigo::cps
