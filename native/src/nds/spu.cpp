// Som do DS (ARM7): 16 canais PCM8/PCM16/IMA-ADPCM, onda quadrada (canais 8-13) e ruído (14-15), volume com
// divisor, pan e volume mestre. Gera a 33513982/1024 Hz (~32,7 kHz) e reamostra para a taxa do aparelho.
// A captura de som (SNDCAPxCNT) só guarda os registradores.
#include <algorithm>
#include <cstring>

#include "nds.h"

namespace indigo::nds {

namespace {
const int16_t ADPCM_STEP[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
  107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
  876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
  5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
  27086, 29794, 32767};
const int8_t ADPCM_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
constexpr int MIX_CYCLES = 1024;
constexpr double OUT_RATE = ARM7_HZ / MIX_CYCLES;
}  // namespace

void Spu::reset() {
  for (auto& c : ch) c = SoundChannel{};
  soundcnt = 0; bias = 0x200;
  capCnt[0] = capCnt[1] = 0;
  capDad[0] = capDad[1] = capLen[0] = capLen[1] = 0;
  cyc_ = 0; resamp_ = 0; lastL_ = lastR_ = 0;
  wpos_ = rpos_ = 0;
}

uint32_t Spu::read(uint32_t a, int size) const {
  const uint32_t off = a & 0xFFF;
  uint32_t v = 0;
  if (off < 0x500) {
    const SoundChannel& c = ch[(off - 0x400) >> 4];
    switch ((off & 0xF) & ~3u) {
      case 0x0: v = (c.cnt & 0x7FFFFFFFu) | (c.on ? 0x80000000u : 0); break;
      case 0x4: v = c.sad; break;
      case 0x8: v = c.tmr | (uint32_t(c.pnt) << 16); break;
      default: v = c.len; break;
    }
  } else {
    switch (off & ~3u) {
      case 0x500: v = soundcnt; break;
      case 0x504: v = bias; break;
      case 0x508: v = capCnt[0] | (capCnt[1] << 8); break;
      case 0x510: v = capDad[0]; break;
      case 0x514: v = capLen[0]; break;
      case 0x518: v = capDad[1]; break;
      case 0x51C: v = capLen[1]; break;
      default: break;
    }
  }
  v >>= (off & 3) * 8;
  return size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v;
}

void Spu::write(uint32_t a, uint32_t v, int size) {
  const uint32_t off = a & 0xFFF;
  // monta o valor de 32 bits do registrador com os bytes novos
  const uint32_t base = off & ~3u;
  uint32_t cur = read(0x04000000 | base, 4);
  const uint32_t mask = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
  const int sh = (off & 3) * 8;
  cur = (cur & ~(mask << sh)) | ((v & mask) << sh);
  if (base < 0x500) {
    const int i = static_cast<int>((base - 0x400) >> 4);
    SoundChannel& c = ch[i];
    switch (base & 0xF) {
      case 0x0: {
        const bool wasOn = c.on;
        c.cnt = cur & 0xFF7F837Fu;
        c.cnt |= cur & 0x80000000u;
        if (!(cur & 0x80000000u)) c.on = false;
        else if (!wasOn) start(i);
        break;
      }
      case 0x4: c.sad = cur & 0x07FFFFFC; break;
      case 0x8: c.tmr = uint16_t(cur); c.pnt = uint16_t(cur >> 16); break;
      default: c.len = cur & 0x3FFFFF; break;
    }
    return;
  }
  switch (base) {
    case 0x500: soundcnt = uint16_t(cur & 0xBF7F); break;
    case 0x504: bias = uint16_t(cur & 0x3FF); break;
    case 0x508: capCnt[0] = uint8_t(cur & 0x8F); capCnt[1] = uint8_t((cur >> 8) & 0x8F); break;
    case 0x510: capDad[0] = cur & 0x07FFFFFC; break;
    case 0x514: capLen[0] = cur & 0xFFFF; break;
    case 0x518: capDad[1] = cur & 0x07FFFFFC; break;
    case 0x51C: capLen[1] = cur & 0xFFFF; break;
    default: break;
  }
}

void Spu::start(int i) {
  SoundChannel& c = ch[i];
  c.on = true;
  c.pos = 0;
  c.timer = c.tmr;
  c.sample = 0;
  c.lfsr = 0x7FFF;
  const int fmt = (c.cnt >> 29) & 3;
  if (fmt == 2) {
    const uint32_t h = s.bus7.read32(c.sad);
    c.adpcmVal = static_cast<int16_t>(h & 0xFFFF);
    c.adpcmIdx = std::min<int>((h >> 16) & 0x7F, 88);
    c.loopVal = c.adpcmVal; c.loopIdx = c.adpcmIdx;
    c.pos = 8;  // nibbles: o cabeçalho ocupa 4 bytes
  }
}

namespace {
// próxima amostra de um canal; devolve false se o canal parou
bool nextSample(NDS& s, SoundChannel& c, int idx) {
  const int fmt = (c.cnt >> 29) & 3;
  const int rep = (c.cnt >> 27) & 3;
  const uint32_t loopStart = uint32_t(c.pnt) * 4, total = loopStart + c.len * 4;  // em bytes
  switch (fmt) {
    case 0: {
      if (c.pos >= total) {
        if (rep == 1 && c.len) c.pos = loopStart;
        else { c.on = false; if (!(c.cnt & 0x8000)) c.sample = 0; return false; }
      }
      c.sample = static_cast<int8_t>(s.bus7.read8(c.sad + c.pos)) << 8;
      c.pos++;
      return true;
    }
    case 1: {
      if (c.pos >= total) {
        if (rep == 1 && c.len) c.pos = loopStart;
        else { c.on = false; if (!(c.cnt & 0x8000)) c.sample = 0; return false; }
      }
      c.sample = static_cast<int16_t>(s.bus7.read16(c.sad + c.pos));
      c.pos += 2;
      return true;
    }
    case 2: {
      if (c.pos >= total * 2) {
        if (rep == 1 && c.len) { c.pos = loopStart * 2; c.adpcmVal = c.loopVal; c.adpcmIdx = c.loopIdx; }
        else { c.on = false; if (!(c.cnt & 0x8000)) c.sample = 0; return false; }
      }
      if (c.pos == loopStart * 2) { c.loopVal = c.adpcmVal; c.loopIdx = c.adpcmIdx; }
      const uint8_t b = static_cast<uint8_t>(s.bus7.read8(c.sad + (c.pos >> 1)));
      const int nib = (c.pos & 1) ? b >> 4 : b & 15;
      const int st = ADPCM_STEP[c.adpcmIdx];
      int diff = st >> 3;
      if (nib & 1) diff += st >> 2;
      if (nib & 2) diff += st >> 1;
      if (nib & 4) diff += st;
      c.adpcmVal = (nib & 8) ? std::max(c.adpcmVal - diff, -0x7FFF) : std::min(c.adpcmVal + diff, 0x7FFF);
      c.adpcmIdx = std::clamp(c.adpcmIdx + ADPCM_IDX[nib & 7], 0, 88);
      c.sample = c.adpcmVal;
      c.pos++;
      return true;
    }
    default:
      if (idx >= 8 && idx <= 13) {
        const int duty = (c.cnt >> 24) & 7;
        c.sample = (int(c.pos & 7) <= duty) ? 0x7FFF : -0x7FFF;  // duty: (n+1)/8 em alto
        c.pos++;
      } else if (idx >= 14) {
        const bool out = c.lfsr & 1;
        c.lfsr >>= 1;
        if (out) c.lfsr ^= 0x6000;
        c.sample = out ? -0x7FFF : 0x7FFF;
      } else c.sample = 0;
      return true;
  }
}
}  // namespace

void Spu::mix() {
  int64_t L = 0, R = 0;
  static const int SHIFT[4] = {0, 1, 2, 4};
  for (int i = 0; i < 16; i++) {
    SoundChannel& c = ch[i];
    if (c.on) {
      // o timer conta a 16,76 MHz (metade do ARM7): 512 passos por amostra de saída
      uint32_t t = c.timer + MIX_CYCLES / 2;
      const uint32_t reload = c.tmr;
      int guard = 0;
      while (t >= 0x10000 && guard++ < 4096) {
        t = t - 0x10000 + reload;
        if (!nextSample(s, c, i)) break;
      }
      if (guard >= 4096) t &= 0xFFFF;
      c.timer = t;
    }
    if (!c.on && !(c.cnt & 0x8000)) continue;
    const int vol = c.cnt & 0x7F;
    const int pan = (c.cnt >> 16) & 0x7F;
    const int64_t v = (int64_t(c.sample) * vol) >> SHIFT[(c.cnt >> 8) & 3];
    L += (v * (127 - pan)) >> 7;
    R += (v * pan) >> 7;
  }
  if (!(soundcnt & 0x8000)) { L = R = 0; }
  const int mvol = soundcnt & 0x7F;
  L = (L * mvol) >> 7;
  R = (R * mvol) >> 7;
  // valores em escala de 16 bits * 128 (volume do canal); 16 canais somados cabem com folga
  const int32_t l = static_cast<int32_t>(std::clamp<int64_t>(L >> 7, -32768 * 4, 32767 * 4));
  const int32_t r = static_cast<int32_t>(std::clamp<int64_t>(R >> 7, -32768 * 4, 32767 * 4));
  // reamostragem linear para a taxa do aparelho
  const double step = OUT_RATE / hostRate_;
  while (resamp_ < 1.0) {
    const float fl = static_cast<float>(lastL_ + (l - lastL_) * resamp_) / 65536.0f;
    const float fr = static_cast<float>(lastR_ + (r - lastR_) * resamp_) / 65536.0f;
    if (available() < BUF - 1) {
      bufL_[wpos_] = fl;
      bufR_[wpos_] = fr;
      wpos_ = (wpos_ + 1) % BUF;
    }
    resamp_ += step;
  }
  resamp_ -= 1.0;
  lastL_ = l;
  lastR_ = r;
}

void Spu::run(int cycles) {
  cyc_ += cycles;
  while (cyc_ >= MIX_CYCLES) {
    cyc_ -= MIX_CYCLES;
    mix();
  }
}

void Spu::pull(float* l, float* r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (rpos_ != wpos_) { l[i] = bufL_[rpos_]; r[i] = bufR_[rpos_]; rpos_ = (rpos_ + 1) % BUF; }
    else { l[i] = 0; r[i] = 0; }
  }
}

void Spu::sync(StateIO& io) {
  for (auto& c : ch) {
    io.sync(c.cnt); io.sync(c.sad); io.sync(c.len); io.sync(c.tmr); io.sync(c.pnt); io.sync(c.on);
    io.sync(c.pos); io.sync(c.timer); io.sync(c.sample); io.sync(c.adpcmVal); io.sync(c.adpcmIdx);
    io.sync(c.loopVal); io.sync(c.loopIdx); io.sync(c.lfsr);
    if (io.reading()) {
      c.adpcmIdx = std::clamp(c.adpcmIdx, 0, 88); c.loopIdx = std::clamp(c.loopIdx, 0, 88);
      c.adpcmVal = std::clamp(c.adpcmVal, -0x7FFF, 0x7FFF); c.loopVal = std::clamp(c.loopVal, -0x7FFF, 0x7FFF);
      c.timer &= 0x1FFFF;
    }
  }
  io.sync(soundcnt); io.sync(bias); io.raw(capCnt, sizeof capCnt); io.raw(capDad, sizeof capDad); io.raw(capLen, sizeof capLen);
  io.sync(cyc_); io.sync(resamp_); io.sync(lastL_); io.sync(lastR_);
  if (io.reading()) {
    if (cyc_ < 0 || cyc_ > MIX_CYCLES * 4) cyc_ = 0;
    finiteOr(resamp_, 0);
    if (resamp_ < 0 || resamp_ > 4) resamp_ = 0;
  }
}

}  // namespace indigo::nds
