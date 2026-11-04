// AICA (Yamaha) do Dreamcast: 2 MB de RAM de som, CPU ARM7 (aqui a ARM60 do 3DO, que é ARMv3 como o ARM7DI do
// AICA, ligada a um barramento little-endian) e 64 canais. Registradores de 16 bits, um a cada 4 bytes:
//   canal n (0x80 * n): 0x00 KYONEX(15) KYONB(14) SSCTL(10) LPCTL(9) PCMS(8-7) SA(22-16); 0x04 SA(15-0);
//   0x08 LSA; 0x0C LEA; 0x10 D2R(15-11) D1R(10-6) AR(4-0); 0x14 LPSLNK(14) KRS(13-10) DL(9-5) RR(4-0);
//   0x18 OCT(14-11) FNS(10-0); 0x24 DISDL(11-8) DIPAN(4-0); 0x28 TL(15-8)
//   0x2800 MVOL(3-0); 0x280C MSLC(13-8); 0x2810 LP/SGC/EG do canal monitorado; 0x2814 CA (posição);
//   0x2890/94/98 temporizadores A/B/C (prescala 10-8, contador 7-0); 0x289C SCIEB, 0x28A0 SCIPD, 0x28A4 SCIRE e
//   0x28A8-0x28B0 SCILV0-2 (interrupções do ARM, pela FIQ); 0x28B4 MCIEB, 0x28B8 MCIPD, 0x28BC MCIRE (do SH-4,
//   pelo Holly); 0x2C00 bit 0 = ARM em reset; 0x2D00 nível da interrupção do ARM; 0x2D04 confirma.
//   Relógio (só do SH-4): 0x10000 segundos desde 1950 (16 bits altos), 0x10004 (baixos), 0x10008 bit 0 libera.
// Formatos: PCM de 16 e 8 bits e ADPCM Yamaha de 4 bits (com laço). Envelope ADSR com velocidades aproximadas
// (não as tabelas exatas do chip); o DSP de efeitos, os LFOs e o filtro não são emulados.
#include <algorithm>
#include <cmath>

#include "dc.h"

namespace indigo::dc {

namespace {
float gainTab[2048];
float lvTab[16];
struct GainInit {
  GainInit() {
    for (int i = 0; i < 2048; i++) gainTab[i] = float(std::pow(10.0, -(i * 0.09375) / 20.0));
    gainTab[2047] = 0;
    for (int i = 0; i < 16; i++) lvTab[i] = i == 0 ? 0.0f : float(std::pow(10.0, -((15 - i) * 3.0) / 20.0));
  }
} gainInit;
// passo do envelope por amostra (em 1/65536 de um passo de 0x3FF) para a velocidade 0..63
uint32_t envTab[64];
struct EnvInit {
  EnvInit() { for (int v = 0; v < 64; v++) envTab[v] = v == 0 ? 0 : uint32_t(65536.0 * std::exp2(double(v) / 4.0 - 10.0)); }
} envInit;
const int adpcmMul[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
const int adpcmStep[8] = {230, 230, 230, 230, 307, 409, 512, 614};
}  // namespace

Aica::Aica(System& s) : sys(s) {}

void Aica::reset() {
  std::memset(chan, 0, sizeof chan);
  std::memset(common, 0, sizeof common);
  std::memset(dspRegs_, 0, sizeof dspRegs_);
  for (auto& v : voice) v = Voice{};
  armReset = true;
  com(0x2C00) = 1;
  arm.reset(0);
  out.clear();
  for (int& t : timerCnt_) t = 0;
}

void Aica::raiseSci(uint32_t bits) { com(0x28A0) |= uint16_t(bits); updateArmIrq(); }
void Aica::raiseMci(uint32_t bits) {
  com(0x28B8) |= uint16_t(bits);
  sys.setExt(1, (com(0x28B8) & com(0x28B4)) != 0);
}

void Aica::updateArmIrq() {
  const uint32_t p = com(0x28A0) & com(0x289C) & 0x7FF;
  arm.setFiq(p != 0 && !armReset);
}

uint32_t Aica::regRead(uint32_t off, int size) {
  if (off >= 0x10000) {
    switch (off & 0xC) {
      case 0: return rtc >> 16;
      case 4: return rtc & 0xFFFF;
      default: return 0;
    }
  }
  const uint32_t sh = (off & 1) ? 8 : 0;
  uint32_t v = 0;
  if (off < 0x2000) {
    v = (off & 2) ? 0 : chan[off >> 7][(off >> 2) & 31];
  } else if (off < 0x3000) {
    const uint32_t r = off & ~3u;
    if (off & 2) v = 0;
    else if (r == 0x2810) {
      const int ch = (com(0x280C) >> 8) & 63;
      Voice& vc = voice[ch];
      v = (vc.looped ? 0x8000u : 0) | uint32_t(vc.stage & 3) << 13 | uint32_t(vc.env & 0x3FF) << 3;
      vc.looped = false;
    } else if (r == 0x2814) {
      v = voice[(com(0x280C) >> 8) & 63].pos & 0xFFFF;
    } else if (r == 0x2890 || r == 0x2894 || r == 0x2898) {
      v = com(r);
    } else if (r == 0x2D00) {
      const uint32_t p = com(0x28A0) & com(0x289C) & 0x7FF;
      v = 0;
      if (p) {
        int b = 31 - __builtin_clz(p);
        if (b > 7) b = 7;
        v = ((com(0x28A8) >> b) & 1) | ((com(0x28AC) >> b) & 1) << 1 | ((com(0x28B0) >> b) & 1) << 2;
      }
    } else {
      v = com(r);
    }
  } else {
    v = (off & 2) ? 0 : dspRegs_[((off - 0x3000) >> 2) & 0x7FF];
  }
  if (size == 1) return (v >> sh) & 0xFF;
  return v;
}

void Aica::keyOnEx() {
  for (int ch = 0; ch < 64; ch++) {
    const uint16_t r0 = chan[ch][0];
    Voice& v = voice[ch];
    if (r0 & 0x4000) {
      if (!v.on || v.stage == 3) {
        v.on = true; v.pos = 0; v.frac = 0; v.stage = 0; v.env = 0x3FF;
        v.adpcmS = 0; v.adpcmStep = 127; v.loopSaved = false; v.looped = false;
        if ((chan[ch][4] & 31) == 31) { v.env = 0; v.stage = 1; }
      }
    } else if (v.on) {
      v.stage = 3;
    }
  }
}

void Aica::regWrite(uint32_t off, uint32_t val, int size) {
  if (off >= 0x10000) {
    switch (off & 0xC) {
      case 0: if (rtcWrite_) rtc = (rtc & 0xFFFF) | (val & 0xFFFF) << 16; break;
      case 4: if (rtcWrite_) rtc = (rtc & 0xFFFF0000u) | (val & 0xFFFF); break;
      case 8: rtcWrite_ = val & 1; break;
      default: break;
    }
    return;
  }
  if (off & 2) return;  // metade alta da palavra: não existe
  auto merge = [&](uint16_t old) -> uint16_t {
    if (size == 1) return (off & 1) ? uint16_t((old & 0x00FF) | (val & 0xFF) << 8) : uint16_t((old & 0xFF00) | (val & 0xFF));
    return uint16_t(val);
  };
  if (off < 0x2000) {
    const int ch = int(off >> 7), r = int((off >> 2) & 31);
    const uint16_t nv = merge(chan[ch][r]);
    chan[ch][r] = r == 0 ? uint16_t(nv & 0x7FFF) : nv;
    if (r == 0 && (nv & 0x8000)) keyOnEx();
    return;
  }
  if (off >= 0x3000) { dspRegs_[((off - 0x3000) >> 2) & 0x7FF] = merge(dspRegs_[((off - 0x3000) >> 2) & 0x7FF]); return; }
  const uint32_t r = off & ~3u;
  const uint16_t nv = merge(com(r));
  switch (r) {
    case 0x28A0: if (nv & 0x20) raiseSci(0x20); return;  // o SH-4 chama o ARM
    case 0x28A4: com(0x28A0) &= uint16_t(~nv); updateArmIrq(); return;
    case 0x289C: com(r) = nv; updateArmIrq(); return;
    case 0x28B4: com(r) = nv; sys.setExt(1, (com(0x28B8) & com(0x28B4)) != 0); return;
    case 0x28B8: if (nv & 0x20) raiseMci(0x20); return;  // o ARM chama o SH-4
    case 0x28BC: com(0x28B8) &= uint16_t(~nv); sys.setExt(1, (com(0x28B8) & com(0x28B4)) != 0); return;
    case 0x2C00: {
      const bool rs = nv & 1;
      com(r) = nv;
      if (armReset && !rs) arm.reset(0);
      armReset = rs;
      updateArmIrq();
      return;
    }
    case 0x2D04: return;
    case 0x2890: case 0x2894: case 0x2898:
      com(r) = uint16_t(nv & 0x7FF);
      timerCnt_[(r - 0x2890) >> 2] = 0;
      return;
    default: com(r) = nv; return;
  }
}

// ---------------------------------------------------------------- ARM
uint32_t Aica::read32(uint32_t a) {
  a &= 0x00FFFFFC;
  if (a < 0x800000) { uint32_t v; std::memcpy(&v, &sys.aram[a & (ARAM_SIZE - 1)], 4); return v; }
  if (a < 0x810000) return regRead(a - 0x800000, 4);
  return 0;
}
uint8_t Aica::read8(uint32_t a) {
  a &= 0x00FFFFFF;
  if (a < 0x800000) return sys.aram[a & (ARAM_SIZE - 1)];
  if (a < 0x810000) return uint8_t(regRead(a - 0x800000, 1));
  return 0;
}
void Aica::write32(uint32_t a, uint32_t v) {
  a &= 0x00FFFFFC;
  if (a < 0x800000) { std::memcpy(&sys.aram[a & (ARAM_SIZE - 1)], &v, 4); return; }
  if (a < 0x810000) regWrite(a - 0x800000, v, 4);
}
void Aica::write8(uint32_t a, uint8_t v) {
  a &= 0x00FFFFFF;
  if (a < 0x800000) { sys.aram[a & (ARAM_SIZE - 1)] = v; return; }
  if (a < 0x810000) regWrite(a - 0x800000, v, 1);
}

void Aica::runArm(int cycles) {
  if (armReset) return;
  armDebt_ += cycles;
  if (armDebt_ > 0) armDebt_ -= arm.run(armDebt_);
}

// ---------------------------------------------------------------- som
int Aica::voiceSample(int ch) {
  Voice& v = voice[ch];
  const uint16_t* r = chan[ch];
  const int pcms = (r[0] >> 7) & 3;
  const uint32_t sa = (uint32_t(r[0] & 0x7F) << 16 | r[1]) & (ARAM_SIZE - 1);
  const uint8_t* m = sys.aram.data();
  int s;
  if (pcms == 0) {
    const uint32_t a = (sa + v.pos * 2) & (ARAM_SIZE - 2);
    s = int16_t(m[a] | m[a + 1] << 8);
  } else if (pcms == 1) {
    s = int(int8_t(m[(sa + v.pos) & (ARAM_SIZE - 1)])) << 8;
  } else {
    s = v.adpcmS;
  }
  // avança a posição
  int oct = (r[6] >> 11) & 15;
  if (oct & 8) oct -= 16;
  uint32_t step = (1024u + (r[6] & 0x3FF)) << 6;
  step = oct >= 0 ? step << oct : step >> -oct;
  v.frac += step;
  const uint32_t lsa = r[2], lea = r[3];
  while (v.frac >= 0x10000) {
    v.frac -= 0x10000;
    if (pcms >= 2 && v.pos == lsa && !v.loopSaved) { v.loopS = v.adpcmS; v.loopStep = v.adpcmStep; v.loopSaved = true; }
    v.pos++;
    if (v.pos >= lea && lea) {
      v.looped = true;
      if (r[0] & 0x200) {
        v.pos = lsa;
        if (pcms >= 2 && v.loopSaved) { v.adpcmS = v.loopS; v.adpcmStep = v.loopStep; }
      } else {
        v.on = false; v.env = 0x3FF; v.stage = 3;
        return 0;
      }
    }
    if (pcms >= 2) {
      const uint8_t b = m[(sa + v.pos / 2) & (ARAM_SIZE - 1)];
      const int nib = (v.pos & 1) ? b >> 4 : b & 15;
      int x = v.adpcmS + (v.adpcmStep * adpcmMul[nib]) / 8;
      v.adpcmS = std::clamp(x, -32768, 32767);
      v.adpcmStep = std::clamp((v.adpcmStep * adpcmStep[nib & 7]) >> 8, 127, 24576);
    }
  }
  // envelope
  const int ar = r[4] & 31, d1r = (r[4] >> 6) & 31, d2r = (r[4] >> 11) & 31, dl = (r[5] >> 5) & 31, rr = r[5] & 31;
  auto go = [&](int rate, int dir) {
    v.envFrac += envTab[rate * 2];
    const int d = int(v.envFrac >> 16);
    v.envFrac &= 0xFFFF;
    v.env = std::clamp(v.env + dir * d, 0, 0x3FF);
  };
  switch (v.stage) {
    case 0:
      if (ar == 31) v.env = 0; else go(ar, -1);
      if (v.env == 0) v.stage = 1;
      break;
    case 1:
      go(d1r, 1);
      if (v.env >= dl << 5) v.stage = 2;
      break;
    case 2: go(d2r, 1); break;
    default:
      go(rr, 1);
      if (v.env >= 0x3FF) { v.on = false; return 0; }
      break;
  }
  return s;
}

void Aica::sample() {
  float l = 0, rr = 0;
  for (int ch = 0; ch < 64; ch++) {
    if (!voice[ch].on) continue;
    const int s = voiceSample(ch);
    const uint16_t* r = chan[ch];
    const int disdl = (r[9] >> 8) & 15;
    if (!disdl) continue;
    const int att = std::min(voice[ch].env + ((r[10] >> 8) & 0xFF) * 4, 2047);
    const float g = gainTab[att] * lvTab[disdl] * float(s);
    const int pan = r[9] & 31, pa = pan & 15;
    const float pg = pa == 15 ? 0.0f : lvTab[15 - pa];
    if (pan & 0x10) { l += g; rr += g * pg; } else { l += g * pg; rr += g; }
  }
  // CD-DA do GD-ROM
  int16_t cd[2];
  sys.gdrom.cddaSamples(cd, 1);
  l += cd[0]; rr += cd[1];
  const float mv = lvTab[com(0x2800) & 15];
  l *= mv; rr *= mv;
  out.push_back(int16_t(std::clamp(l, -32768.0f, 32767.0f)));
  out.push_back(int16_t(std::clamp(rr, -32768.0f, 32767.0f)));
  // temporizadores A, B, C (passo a cada 2^prescala amostras)
  for (int t = 0; t < 3; t++) {
    uint16_t& tr = com(0x2890 + 4 * uint32_t(t));
    const int pre = (tr >> 8) & 7;
    if (++timerCnt_[t] >= (1 << pre)) {
      timerCnt_[t] = 0;
      const uint16_t c = uint16_t((tr & 0xFF) + 1);
      tr = uint16_t((tr & 0x700) | (c & 0xFF));
      if (c == 0x100) { raiseSci(0x40u << t); raiseMci(0x40u << t); }
    }
  }
  // interrupção de amostra (bit 10)
  if (com(0x289C) & 0x400) raiseSci(0x400);
  if (++rtcFrac_ >= 44100) { rtcFrac_ = 0; rtc++; }
}

void Aica::sync(StateIO& io) {
  arm.sync(io);
  io.raw(chan, sizeof chan);
  io.raw(common, sizeof common);
  io.raw(dspRegs_, sizeof dspRegs_);
  io.sync(rtc); io.sync(rtcWrite_); io.sync(armReset); io.sync(armDebt_); io.sync(rtcFrac_);
  for (auto& v : voice) {
    io.sync(v.on); io.sync(v.pos); io.sync(v.frac); io.sync(v.env); io.sync(v.stage); io.sync(v.envFrac);
    io.sync(v.adpcmS); io.sync(v.adpcmStep); io.sync(v.loopS); io.sync(v.loopStep); io.sync(v.loopSaved);
    io.sync(v.looped);
  }
  io.raw(timerCnt_, sizeof timerCnt_);
  if (io.reading()) {
    for (auto& v : voice) {
      clampTo(v.env, 0, 0x3FF); clampTo(v.stage, 0, 3);
      clampTo(v.adpcmS, -32768, 32767); clampTo(v.adpcmStep, 127, 24576);
      clampTo(v.loopS, -32768, 32767); clampTo(v.loopStep, 127, 24576);
      v.pos &= 0xFFFF; v.frac &= 0xFFFF;
    }
    for (int& t : timerCnt_) clampTo(t, 0, 128);
    clampTo(armDebt_, -100000, 100000);
    out.clear();
  }
}

}  // namespace indigo::dc
