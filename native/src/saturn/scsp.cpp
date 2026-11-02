// SCSP do Saturn (Yamaha YMF292): 32 canais de PCM (8 ou 16 bits, com laço), envelope ADSR, volume e pan
// diretos, temporizadores A/B/C, interrupções do 68000 e do SH-2 mestre, DMA interno e a RAM de som de 512 KB
// do 68000 (src/md/m68k.h).
//
// Fora daqui (ainda): modulação FM entre canais, LFO, o DSP de efeitos (a saída de efeito entra direto, sem eco)
// e MIDI. O envelope usa uma curva aproximada (tempo dobra a cada 4 passos de taxa), não a tabela do chip.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "saturn.h"

namespace indigo::saturn {

namespace {
// ganho por atenuação (1/32 de 3 dB por passo de 0,09375 dB) e passo do envelope por taxa efetiva
struct Tables {
  int32_t gain[4096];   // Q15
  int32_t step[64];     // passo do envelope por amostra, Q16 em unidades de atenuação
  Tables() {
    for (int i = 0; i < 4096; i++) gain[i] = int32_t(std::lround(32767.0 * std::pow(10.0, -i * 0.09375 / 20.0)));
    for (int r = 0; r < 64; r++) {
      if (r < 2) { step[r] = 0; continue; }
      const double samples = std::pow(2.0, (62 - r) / 4.0) * 4.0;  // tempo para o envelope todo
      step[r] = int32_t(std::min(1024.0 * 65536.0, 1024.0 * 65536.0 / samples));
    }
  }
};
const Tables& tables() { static const Tables t; return t; }
}  // namespace

Scsp::Scsp(System& s) : sys_(s) {}

void Scsp::reset() {
  std::fill(ram.begin(), ram.end(), 0);
  for (auto& s : slot_) s = Slot{};
  std::memset(common_, 0, sizeof common_);
  scieb_ = scipd_ = mcieb_ = mcipd_ = 0;
  std::memset(scilv_, 0, sizeof scilv_);
  std::memset(timer_, 0, sizeof timer_);
  std::memset(tctl_, 0, sizeof tctl_);
  std::memset(timerSub_, 0, sizeof timerSub_);
  std::memset(stack_, 0, sizeof stack_);
  exl_ = exr_ = 0;
  sampleCount_ = 0;
  cycleDebt_ = 0;
  lfsr_ = 1;
  std::memset(delay_, 0, sizeof delay_);
  cpuOn = false;
  cpu.reset();
  cpu.ipl = 0;
}

void Scsp::updateIrq() {
  // 68000: o nível vem de SCILV0-2 (um bit de cada registrador por fonte; as fontes 8-10 usam o nível da 7)
  const uint16_t p = scipd_ & scieb_;
  int lvl = 0;
  for (int b = 0; b < 11; b++) {
    if (!(p & (1 << b))) continue;
    const int k = std::min(b, 7);
    const int l = ((scilv_[0] >> k) & 1) | ((scilv_[1] >> k) & 1) << 1 | ((scilv_[2] >> k) & 1) << 2;
    lvl = std::max(lvl, l);
  }
  cpu.ipl = lvl;
}

void Scsp::keyOnOff() {
  for (auto& s : slot_) {
    const bool kb = s.r[0] & 0x800;
    if (kb && !s.on) {
      s.on = true;
      s.pos = 0;
      s.back = false;
      s.phase = 0;
      s.env = 0x3FF;
      s.envAcc = 0;
      if ((s.r[4] & 31) >= 31) s.env = 0;
    } else if (!kb && s.on) {
      s.on = false;
      s.phase = 3;
    }
  }
}

uint16_t Scsp::readReg(uint32_t off) {
  off &= 0xFFE;
  if (off < 0x400) {
    const Slot& s = slot_[off >> 5];
    const int r = (off & 0x1F) >> 1;
    return r == 0 ? uint16_t(s.r[0] & 0x0FFF) : s.r[r];
  }
  if (off < 0x430) {
    switch (off) {
      case 0x404: return 0x0900;  // MIDI: entrada vazia, saída vazia
      case 0x408: {               // monitor de canal: CA (endereço), fase e nível do envelope
        const int n = (common_[4] >> 11) & 31;
        const Slot& s = slot_[n];
        return uint16_t((((s.pos >> 12) >> 12) & 0xF) << 7 | (s.phase & 3) << 5 | ((s.env >> 5) & 31));
      }
      case 0x41E: return scieb_;
      case 0x420: return scipd_;
      case 0x42A: return mcieb_;
      case 0x42C: return mcipd_;
      default: return common_[(off - 0x400) >> 1];
    }
  }
  if (off >= 0x600 && off < 0x680) return stack_[(off - 0x600) >> 1];
  return 0;
}

void Scsp::writeReg(uint32_t off, uint16_t v, uint16_t mask) {
  off &= 0xFFE;
  if (off < 0x400) {
    Slot& s = slot_[off >> 5];
    const int r = (off & 0x1F) >> 1;
    s.r[r] = uint16_t((s.r[r] & ~mask) | (v & mask));
    if (r == 0 && (v & mask & 0x1000)) { s.r[0] &= 0x0FFF; keyOnOff(); }
    return;
  }
  if (off < 0x430) {
    const int i = (off - 0x400) >> 1;
    const uint16_t nv = uint16_t((common_[i] & ~mask) | (v & mask));
    switch (off) {
      case 0x416:
        common_[i] = nv;
        if ((nv & 0x1000) && dmaBusy_) common_[i] &= ~0x1000;  // gravado pela própria DMA: ignora a partida
        else if (nv & 0x1000) {  // DMA: RAM de som <-> registradores
          dmaBusy_ = true;
          const uint32_t mem = (uint32_t(common_[0xA] & 0xF000) << 4 | (common_[9] & 0xFFFE)) & 0x7FFFE;
          const uint32_t reg = common_[0xA] & 0x0FFE, len = nv & 0x0FFE;
          for (uint32_t k = 0; k < len; k += 2) {
            if (nv & 0x2000) {
              const uint16_t d = (nv & 0x4000) ? 0 : readReg(reg + k);
              ram[(mem + k) & 0x7FFFF] = uint8_t(d >> 8);
              ram[(mem + k + 1) & 0x7FFFF] = uint8_t(d);
            } else {
              const uint16_t d = (nv & 0x4000) ? 0 : uint16_t(ram[(mem + k) & 0x7FFFF] << 8 | ram[(mem + k + 1) & 0x7FFFF]);
              writeReg(reg + k, d, 0xFFFF);
            }
          }
          dmaBusy_ = false;
          common_[i] &= ~0x1000;
          scipd_ |= 0x10;
          mcipd_ |= 0x10;
          updateIrq();
          if (mcipd_ & mcieb_ & 0x10) sys_.scu.raise(INT_SCSP);
        }
        return;
      case 0x418: case 0x41A: case 0x41C: {
        const int t = (off - 0x418) >> 1;
        common_[i] = nv;
        tctl_[t] = uint8_t((nv >> 8) & 7);
        if (mask & 0x00FF) timer_[t] = uint8_t(nv);
        return;
      }
      case 0x41E: scieb_ = nv & 0x7FF; updateIrq(); return;
      case 0x420: if (v & mask & 0x20) { scipd_ |= 0x20; updateIrq(); } return;
      case 0x422: scipd_ &= ~(v & mask); updateIrq(); return;
      case 0x424: case 0x426: case 0x428: scilv_[(off - 0x424) >> 1] = nv & 0xFF; updateIrq(); return;
      case 0x42A: {
        mcieb_ = nv & 0x7FF;
        if (mcipd_ & mcieb_) sys_.scu.raise(INT_SCSP);
        return;
      }
      case 0x42C:
        if (v & mask & 0x20) { mcipd_ |= 0x20; if (mcieb_ & 0x20) sys_.scu.raise(INT_SCSP); }
        return;
      case 0x42E: mcipd_ &= ~(v & mask); return;
      default: common_[i] = nv; return;
    }
  }
  if (off >= 0x600 && off < 0x680) stack_[(off - 0x600) >> 1] = uint16_t((stack_[(off - 0x600) >> 1] & ~mask) | (v & mask));
}

void Scsp::timersStep() {
  // temporizadores a 44,1 kHz / 2^TxCTL; ao passar de 0xFF, liga o bit 6/7/8
  for (int t = 0; t < 3; t++) {
    if (++timerSub_[t] < (1u << tctl_[t])) continue;
    timerSub_[t] = 0;
    if (++timer_[t] == 0) {
      const uint16_t bit = uint16_t(0x40 << t);
      scipd_ |= bit;
      if (!(mcipd_ & bit) && (mcieb_ & bit)) { mcipd_ |= bit; sys_.scu.raise(INT_SCSP); }
      else mcipd_ |= bit;
    }
  }
  scipd_ |= 0x400;  // uma interrupção por amostra
  if (!(mcipd_ & 0x400) && (mcieb_ & 0x400)) sys_.scu.raise(INT_SCSP);
  mcipd_ |= 0x400;
  updateIrq();
}

int16_t Scsp::slotStep(int n, int32_t& outL, int32_t& outR) {
  Slot& s = slot_[n];
  if (s.phase == 3 && s.env >= 0x3FF) return 0;
  const auto& T = tables();
  const uint16_t* r = s.r;
  // envelope
  const int ar = r[4] & 31, d1r = (r[4] >> 6) & 31, d2r = (r[4] >> 11) & 31, rr = r[5] & 31, dl = (r[5] >> 5) & 31;
  int rate = 0;
  switch (s.phase) {
    case 0: rate = ar; break;
    case 1: rate = d1r; break;
    case 2: rate = d2r; break;
    default: rate = rr; break;
  }
  s.envAcc += T.step[std::min(63, rate * 2)];
  const int d = s.envAcc >> 16;
  s.envAcc &= 0xFFFF;
  if (s.phase == 0) {
    s.env -= d * 4;
    if (s.env <= 0) { s.env = 0; s.phase = 1; }
  } else {
    s.env = std::min(0x3FF, s.env + d);
    if (s.phase == 1 && s.env >= dl << 5) s.phase = 2;
  }
  // amostra
  const bool pcm8 = r[0] & 0x10;
  const uint32_t sa = (uint32_t(r[0] & 0xF) << 16 | r[1]) & 0xFFFFF;
  const uint32_t lsa = r[2], lea = r[3];
  const int loop = (r[0] >> 5) & 3;
  // modulação (FM): a soma de duas saídas da pilha de som desloca a leitura da onda
  int32_t mod = 0;
  const int mdl = (r[7] >> 12) & 0xF;
  if (mdl > 4) {
    const uint32_t g = sampleCount_ * 32 + uint32_t(n);
    const int32_t m = int16_t(stack_[(g + ((r[7] >> 6) & 0x3F)) & 63]) + int16_t(stack_[(g + (r[7] & 0x3F)) & 63]);
    mod = (m * 64) >> (16 - mdl);  // em 1/64 de amostra
  }
  const int32_t pos6 = int32_t((s.pos >> 6) & 0x3F) + (mod & 0x3F);  // fração para a interpolação
  const uint32_t idx = (s.pos >> 12) + uint32_t(mod >> 6) + uint32_t(pos6 >> 6);
  const int32_t frac = pos6 & 0x3F;
  int32_t smp;
  auto pcmAt = [&](uint32_t i) -> int32_t {
    if (pcm8) return int32_t(int8_t(ram[(sa + i) & 0x7FFFF])) << 8;
    const uint32_t a = (sa + i * 2) & 0x7FFFE;
    return int16_t(ram[a] << 8 | ram[a + 1]);
  };
  switch ((r[0] >> 7) & 3) {
    case 0: {
      // interpolação linear entre a amostra atual e a seguinte (dentro do laço)
      const uint32_t nxt = (idx + 1 > lea && ((r[0] >> 5) & 3)) ? lsa : idx + 1;
      smp = (pcmAt(idx) * (64 - frac) + pcmAt(nxt) * frac) >> 6;
      break;
    }
    case 1:  // ruído
      lfsr_ = (lfsr_ >> 1) ^ (uint32_t(-(int32_t(lfsr_ & 1))) & 0xB400u);
      smp = int16_t(lfsr_);
      break;
    default: smp = 0; break;
  }
  static const uint16_t SBX[4] = {0, 0x7FFF, 0x8000, 0xFFFF};
  smp = int16_t(uint16_t(smp) ^ SBX[(r[0] >> 9) & 3]);
  // passo: (1024 + FNS) * 2^OCT / 1024 amostras por amostra de saída
  const int oct = int((r[8] >> 11) & 0xF) - (((r[8] >> 11) & 0x8) ? 16 : 0);
  uint32_t inc = (0x400u | (r[8] & 0x3FF)) << 2;
  inc = oct >= 0 ? inc << oct : inc >> -oct;
  s.pos += inc;
  if ((s.pos >> 12) > lea) {
    if (loop == 0) { s.pos = lea << 12; s.phase = 3; s.env = 0x3FF; s.on = false; }
    else s.pos = (lsa << 12) + (s.pos - ((lea + 1) << 12));
    if ((s.pos >> 12) > lea) s.pos = lsa << 12;
  }
  // volume: envelope + TL (SDIR = sem envelope nem TL); saída direta (ou a de efeito, sem o DSP) com pan
  const int tl = r[6] & 0xFF;
  const int att = (r[6] & 0x100) ? 0 : s.env + tl * 4;
  if (att > 0x3FF) return 0;
  int32_t v = (smp * T.gain[att]) >> 15;
  s.out = int16_t(v);
  int sdl = (r[0xB] >> 13) & 7, pan = (r[0xB] >> 8) & 31;
  if (!sdl) { sdl = (r[0xB] >> 5) & 7; pan = r[0xB] & 31; }
  if (!sdl) return s.out;
  v >>= (7 - sdl);
  const int pa = (pan & 0xF) == 0xF ? 4095 : (pan & 0xF) * 32;  // 3 dB por passo
  const int32_t att2 = T.gain[std::min(4095, pa)];
  int32_t l = v, rr2 = v;
  if (pan & 0x10) l = (l * att2) >> 15; else rr2 = (rr2 * att2) >> 15;
  outL += l;
  outR += rr2;
  return s.out;
}

void Scsp::sample(int16_t& l, int16_t& r) {
  int32_t L = 0, R = 0;
  for (int n = 0; n < 32; n++) {
    const int16_t o = slotStep(n, L, R);
    // pilha de som: a saída de cada canal entra 4 canais depois (para a modulação dos outros)
    const uint32_t g = sampleCount_ * 32 + uint32_t(n);
    if (!(slot_[(n + 28) & 31].r[6] & 0x200)) stack_[(g - 4) & 63] = uint16_t(delay_[0]);
    delay_[0] = delay_[1]; delay_[1] = delay_[2]; delay_[2] = delay_[3]; delay_[3] = o;
  }
  L += exl_;
  R += exr_;
  const int mvol = common_[0] & 0xF;
  L = L * mvol / 15;
  R = R * mvol / 15;
  l = int16_t(std::clamp(L, -32768, 32767));
  r = int16_t(std::clamp(R, -32768, 32767));
  timersStep();
  sampleCount_++;
}

void Scsp::run68k(int cycles) {
  cycleDebt_ += cycles;
  while (cycleDebt_ > 0) {
    if (cpu.halted) { cycleDebt_ = 0; break; }
    cycleDebt_ -= cpu.step();
  }
}

uint8_t Scsp::read8(uint32_t a) {
  a &= 0xFFFFFF;
  if (a < 0x100000) return ram[a & 0x7FFFF];
  if (a < 0x101000) { const uint16_t w = readReg(a & 0xFFE); return uint8_t((a & 1) ? w : w >> 8); }
  return 0;
}
uint16_t Scsp::read16(uint32_t a) {
  a &= 0xFFFFFF;
  if (a < 0x100000) { a &= 0x7FFFE; return uint16_t(ram[a] << 8 | ram[a + 1]); }
  if (a < 0x101000) return readReg(a & 0xFFF);
  return 0;
}
void Scsp::write8(uint32_t a, uint8_t v) {
  a &= 0xFFFFFF;
  if (a < 0x100000) { ram[a & 0x7FFFF] = v; return; }
  if (a < 0x101000) writeReg(a & 0xFFE, uint16_t(v * 0x101), (a & 1) ? 0x00FF : 0xFF00);
}
void Scsp::write16(uint32_t a, uint16_t v) {
  a &= 0xFFFFFF;
  if (a < 0x100000) { a &= 0x7FFFE; ram[a] = uint8_t(v >> 8); ram[a + 1] = uint8_t(v); return; }
  if (a < 0x101000) writeReg(a & 0xFFE, v, 0xFFFF);
}

void Scsp::sync(StateIO& io) {
  io.bytes(ram);
  cpu.sync(io);
  io.sync(cpuOn);
  for (auto& s : slot_) {
    io.raw(s.r, sizeof s.r);
    io.sync(s.pos); io.sync(s.env); io.sync(s.phase); io.sync(s.on); io.sync(s.back); io.sync(s.envAcc); io.sync(s.out);
    if (io.reading()) { s.env = std::clamp(s.env, 0, 0x3FF); s.phase &= 3; }
  }
  io.raw(common_, sizeof common_);
  io.sync(scieb_); io.sync(scipd_); io.sync(mcieb_); io.sync(mcipd_);
  io.raw(scilv_, sizeof scilv_);
  io.raw(timer_, sizeof timer_);
  io.raw(tctl_, sizeof tctl_);
  io.raw(timerSub_, sizeof timerSub_);
  io.raw(stack_, sizeof stack_);
  io.sync(sampleCount_);
  io.sync(cycleDebt_);
  io.sync(lfsr_);
  io.raw(delay_, sizeof delay_);
  if (io.reading()) {
    for (auto& t : tctl_) t &= 7;
    if (cycleDebt_ > 100000 || cycleDebt_ < -100000) cycleDebt_ = 0;
  }
}

}  // namespace indigo::saturn
