// SCSP do Saturn (Yamaha YMF292): 32 canais de PCM (8 ou 16 bits, com laço), envelope ADSR com o relógio e as
// curvas do chip (ataque exponencial, escala de tecla, EGHOLD, LPSLNK), LFO de altura e de amplitude (4 formas de
// onda), FM pela pilha de som, ruído, volume e pan diretos, a entrada externa (CD) pelos canais 16/17,
// temporizadores A/B/C, interrupções do 68000 e do SH-2 mestre, DMA interno e a RAM de som de 512 KB do 68000
// (src/md/m68k.h). Envelope, LFO, atenuação e volumes seguem o comportamento medido no aparelho e descrito no
// Mednafen (ver TERCEIROS.md).
//
// O DSP de efeitos roda o programa de 128 passos por amostra (eco/reverberação no anel da RAM de som); a saída
// de efeito de cada canal é o EFREG do DSP. Fora daqui (ainda): MIDI.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "saturn.h"

namespace indigo::saturn {

namespace {
// volume de um envio (nível 0-7 e pan de 5 bits) em 1.14: cada passo de nível dobra; o pan atenua um lado
inline void panVolume(int level, int pan, int32_t& l, int32_t& r) {
  const int32_t base = level ? 0x80 << level : 0;
  int32_t p = base >> ((pan & 0xF) >> 1);
  if (pan & 1) p -= p >> 2;
  if ((pan & 0xF) == 0xF) p = 0;
  if (pan & 0x10) { l = base; r = p; } else { l = p; r = base; }
}
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
  dsp_ = Dsp{};
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
      s.inLoop = false;
      s.rev = false;
      s.phase = 0;
      // ataque instantâneo (taxa + escala >= 32) começa no máximo; os outros, em 0x280
      s.env = (s.r[4] & 31) + keyScale(s) >= 0x20 ? 0 : 0x280;
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
        return uint16_t((((s.pos >> 14) >> 12) & 0xF) << 7 | (s.phase & 3) << 5 | ((s.env >> 5) & 31));
      }
      case 0x41E: return scieb_;
      case 0x420: return scipd_;
      case 0x42A: return mcieb_;
      case 0x42C: return mcipd_;
      default: return common_[(off - 0x400) >> 1];
    }
  }
  if (off >= 0x600 && off < 0x680) return stack_[(off - 0x600) >> 1];
  if (off >= 0x700 && off < 0xEE4) return dspRead(off);
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
  if (off >= 0x700 && off < 0xEE4) dspWrite(off, v, mask);
}

// ------------------------------------------------------------------------------------------------ DSP
namespace {
inline int32_t sext(uint32_t v, int bits) { return int32_t(v << (32 - bits)) >> (32 - bits); }
// formato de ponto flutuante do anel (16 bits): sinal, expoente de 4 bits e 11 de mantissa
inline uint32_t dspFloatToInt(uint16_t in) {
  const uint32_t sign = uint32_t(int32_t((in & 0x8000) << 16) >> 1);
  const uint32_t exp = (in >> 11) & 0xF;
  uint32_t r = in & 0x7FF;
  if (exp < 12) r |= 0x800;
  r <<= 11 + 8;
  r ^= sign;
  return uint32_t(int32_t(r) >> (8 + std::min<uint32_t>(11, exp)));
}
inline uint16_t intToDspFloat(uint32_t in) {
  const uint32_t s8 = in << 8;
  const uint32_t sign = uint32_t(int32_t(s8) >> 31);
  const uint32_t x = ((s8 ^ sign) << 1) | (1u << 19);
  uint32_t exp = 0;
  while (exp < 32 && !(x & (0x80000000u >> exp))) exp++;
  const uint32_t sh = exp - (exp == 12 ? 1 : 0);
  uint32_t r = uint32_t(int32_t(s8) >> (19 - sh));
  r &= 0x87FF;
  r |= exp << 11;
  return uint16_t(r);
}
}  // namespace

uint16_t Scsp::dspRead(uint32_t off) const {
  if (off < 0x780) return uint16_t(dsp_.coef[(off - 0x700) >> 1] << 3);
  if (off < 0x7C0) return dsp_.madrs[(off - 0x780) >> 1];
  if (off >= 0x800 && off < 0xC00) return dsp_.mprog[(off - 0x800) >> 1];
  if (off >= 0xC00 && off < 0xE00) { const uint32_t t = uint32_t(dsp_.temp[(off - 0xC00) >> 2]); return uint16_t((off & 2) ? (t >> 8) & 0xFFFF : t & 0xFF); }
  if (off >= 0xE00 && off < 0xE80) { const uint32_t t = uint32_t(dsp_.mems[(off - 0xE00) >> 2]); return uint16_t((off & 2) ? (t >> 8) & 0xFFFF : t & 0xFF); }
  if (off >= 0xE80 && off < 0xEC0) { const uint32_t t = dsp_.mixs[(off - 0xE80) >> 2]; return uint16_t((off & 2) ? (t >> 4) & 0xFFFF : t & 0xF); }
  if (off >= 0xEC0 && off < 0xEE0) return dsp_.efreg[(off - 0xEC0) >> 1];
  if (off >= 0xEE0) return uint16_t(off & 2 ? exr_ : exl_);
  return 0;
}

void Scsp::dspWrite(uint32_t off, uint16_t v, uint16_t mask) {
  auto mix = [&](uint16_t old) { return uint16_t((old & ~mask) | (v & mask)); };
  if (off < 0x780) { uint16_t& c = dsp_.coef[(off - 0x700) >> 1]; c = uint16_t(mix(uint16_t(c << 3)) >> 3); return; }
  if (off < 0x7C0) { uint16_t& m = dsp_.madrs[(off - 0x780) >> 1]; m = mix(m); return; }
  if (off >= 0x800 && off < 0xC00) {
    uint16_t& w = dsp_.mprog[(off - 0x800) >> 1];
    w = mix(w);
    dsp_.active = false;
    for (uint16_t x : dsp_.mprog) if (x) { dsp_.active = true; break; }
    return;
  }
  auto part = [&](uint32_t cur, int lowBits) -> uint32_t {   // palavra baixa: lowBits bits; alta: os 16 de cima
    if (off & 2) return (cur & ((1u << lowBits) - 1)) | uint32_t(mix(uint16_t(cur >> lowBits))) << lowBits;
    return (cur & ~((1u << lowBits) - 1)) | (mix(uint16_t(cur)) & ((1u << lowBits) - 1));
  };
  if (off >= 0xC00 && off < 0xE00) { int32_t& t = dsp_.temp[(off - 0xC00) >> 2]; t = sext(part(uint32_t(t), 8) & 0xFFFFFF, 24); return; }
  if (off >= 0xE00 && off < 0xE80) { int32_t& t = dsp_.mems[(off - 0xE00) >> 2]; t = sext(part(uint32_t(t), 8) & 0xFFFFFF, 24); return; }
  if (off >= 0xE80 && off < 0xEC0) { uint32_t& t = dsp_.mixs[(off - 0xE80) >> 2]; t = part(t, 4) & 0xFFFFF; return; }
  if (off >= 0xEC0 && off < 0xEE0) { uint16_t& e = dsp_.efreg[(off - 0xEC0) >> 1]; e = mix(e); return; }
}

// um quadro de som do DSP: os 128 passos (multiplicação, soma, deslocamento, TEMP, MEMS, EFREG e o anel)
void Scsp::dspRun() {
  Dsp& d = dsp_;
  const uint32_t rbp = common_[1] & 0x7F, rbl = (common_[1] >> 7) & 3;
  for (int step = 0; step < 128; step++) {
    const uint64_t in = uint64_t(d.mprog[step * 4]) << 48 | uint64_t(d.mprog[step * 4 + 1]) << 32 |
                        uint64_t(d.mprog[step * 4 + 2]) << 16 | d.mprog[step * 4 + 3];
    auto bit = [&](int b) { return bool((in >> b) & 1); };
    const unsigned masa = (in >> 2) & 0x1F, cra = (in >> 9) & 0x3F, ewa = (in >> 24) & 0xF, iwa = (in >> 32) & 0x1F;
    const unsigned ira = (in >> 38) & 0x3F, ysel = (in >> 45) & 3, twa = (in >> 48) & 0x7F, tra = (in >> 56) & 0x7F;
    const bool shft0 = bit(20), shft1 = bit(21);
    // entrada: MEMS, MIXS ou a entrada externa (CD)
    if (ira & 0x20) {
      if (ira & 0x10) { if (!(ira & 0xE)) d.inputs = uint32_t(int32_t(ira & 1 ? exr_ : exl_) << 8); }
      else d.inputs = uint32_t(sext(d.mixs[ira & 0xF], 20) << 4);
    } else {
      d.inputs = uint32_t(d.mems[ira & 0x1F]);
    }
    const int32_t inputs = int32_t(d.inputs);
    const uint16_t yin = (ysel & 2) ? ((ysel & 1) ? uint16_t((d.y >> 4) & 0xFFF) : uint16_t((d.y >> 11) & 0x1FFF))
                                    : ((ysel & 1) ? d.coef[cra] : d.frc);
    if (bit(19)) d.y = uint32_t(inputs) & 0xFFFFFF;
    int32_t so = int32_t(uint32_t(sext(d.sft, 26)) << (shft0 ^ shft1));
    if (!shft1) {
      if (so > 0x7FFFFF) so = 0x7FFFFF;
      else if (so < -0x800000) so = 0x800000;
    }
    so = sext(uint32_t(so), 24);
    if (bit(22)) d.frc = (shft0 && shft1) ? uint16_t(so & 0xFFF) : uint16_t((so >> 11) & 0x1FFF);
    {
      const int32_t temp = d.temp[(tra + d.mdecCt) & 0x7F];
      const int32_t x = bit(47) ? inputs : temp;
      const uint32_t prod = uint32_t((int64_t(sext(yin, 13)) * x) >> 12);
      uint32_t sga = bit(16) ? d.sft : uint32_t(temp);
      if (bit(18)) sga = uint32_t(-int32_t(sga));
      if (bit(17)) sga = 0;
      d.sft = (prod + sga) & 0x3FFFFFF;
    }
    if (bit(28)) d.efreg[ewa] = uint16_t(so >> 8);
    if (bit(55)) d.temp[(twa + d.mdecCt) & 0x7F] = so;
    if (bit(37)) d.mems[iwa] = int32_t(d.readValue);
    // acesso à RAM de som pedido no passo anterior
    if (d.readPending) {
      const uint32_t a = (d.rwAddr & 0x3FFFF) * 2;
      const uint16_t w = (d.rwAddr & 0x40000) ? 0 : uint16_t(ram[a] << 8 | ram[a + 1]);
      d.readValue = d.readPending == 2 ? uint32_t(int32_t(int16_t(w)) << 8) : dspFloatToInt(w);
      d.readPending = 0;
    } else if (d.writePending) {
      if (!(d.rwAddr & 0x40000)) { const uint32_t a = (d.rwAddr & 0x3FFFF) * 2; ram[a] = uint8_t(d.writeValue >> 8); ram[a + 1] = uint8_t(d.writeValue); }
      d.writePending = false;
    }
    {
      uint16_t addr = uint16_t(d.madrs[masa] + (bit(0) ? 1 : 0));
      if (bit(1)) addr = uint16_t(addr + sext(d.adrs, 12));
      if (!bit(31)) { addr = uint16_t(addr + d.mdecCt); addr &= uint16_t((0x2000u << rbl) - 1); }
      d.rwAddr = (addr + (rbp << 12)) & 0x7FFFF;
      if (bit(29)) d.readPending = bit(8) ? 2 : 1;
      if (bit(30)) { d.writePending = true; d.writeValue = bit(8) ? uint16_t(so >> 8) : intToDspFloat(uint32_t(so)); }
    }
    if (bit(23)) d.adrs = (shft0 && shft1) ? uint16_t((so >> 12) & 0xFFF) : uint16_t((inputs >> 16) & 0xFFF);
  }
  if (!d.mdecCt) d.mdecCt = uint16_t(0x2000u << rbl);
  d.mdecCt--;
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

// escala de tecla (KRS): a taxa do envelope sobe com a oitava; 0xF desliga
int Scsp::keyScale(const Slot& s) const {
  const int krs = (s.r[5] >> 10) & 0xF;
  if (krs == 0xF) return 0;
  const int oct = int((s.r[8] >> 11) & 0xF) - (((s.r[8] >> 11) & 0x8) ? 16 : 0);
  return std::clamp(krs + oct, 0, 0xF);
}

// envelope: o relógio do chip conta amostras; taxas altas andam todo passo, as baixas a cada 2^n amostras.
// Ataque soma ~nível >> n (curva exponencial), os decaimentos somam 16 >> n.
void Scsp::envStep(Slot& s) {
  const int ks = keyScale(s);
  const int rates[4] = {s.r[4] & 31, (s.r[4] >> 6) & 31, (s.r[4] >> 11) & 31, s.r[5] & 31};
  const int raw = rates[s.phase & 3];
  const int rate = std::min(0x1F, raw + ks);
  const uint32_t sc = sampleCount_, scxc = (sc ^ (sc - 1)) & (sc ^ 1);
  const int wbt = (0x18 - std::min(0x18, rate)) >> 1;
  const int adj = (rate < 0x18) ? (rate & 1) : 0;
  bool clock = raw && (((scxc | (scxc >> adj)) >> (wbt + adj)) & 1);
  if (s.phase == 0 && raw + ks >= 0x20) clock = false;
  const int cache = s.env;
  if (clock) {
    const int32_t inc = s.phase == 0 ? ~cache : 16;
    const int emax = std::max(0x18, std::min(0x1E, rate));
    const int sh = ((0x20 - emax) >> 1) + (emax & 1 & int(sc >> (wbt + 1)));
    s.env = std::clamp(cache + (inc >> sh), 0, 0x3FF);
  }
  if (s.phase == 1) {
    if ((cache >> 5) == ((s.r[5] >> 5) & 31)) s.phase = 2;
  } else if (s.phase == 0) {
    const bool lpslnk = s.r[5] & 0x4000;
    if ((lpslnk && s.inLoop) || (!lpslnk && cache == 0)) s.phase = 1;
  }
}

int16_t Scsp::slotStep(int n, int32_t& outL, int32_t& outR) {
  Slot& s = slot_[n];
  const uint16_t* r = s.r;
  // LFO (r9): LFORE, LFOF, PLFOWS/PLFOS (altura), ALFOWS/ALFOS (amplitude)
  const uint16_t lr = r[9];
  int plfo = 0;
  {
    const int ps = (lr >> 5) & 7;
    if (ps) {
      switch ((lr >> 8) & 3) {
        case 0: plfo = int8_t(s.lfoCount & ~1); break;
        case 1: plfo = int8_t((s.lfoCount & 0x80) ? 0x80 : 0x7E); break;
        case 2: plfo = int8_t(((s.lfoCount & 0x3F) ^ ((s.lfoCount & 0x40) ? 0x3F : 0) ^ ((s.lfoCount & 0x80) ? 0x7F : 0)) << 1); break;
        default: plfo = int8_t(lfsr_ & 0xFE); break;
      }
      plfo >>= (7 - ps);
      plfo = ((0x40 ^ ((r[8] & 0x3FF) >> 4)) * plfo) >> 6;
    }
  }
  envStep(s);
  int32_t smp = 0;
  const bool silent = s.phase == 3 && s.env >= 0x3FF;
  if (!silent) {
    const bool pcm8 = r[0] & 0x10;
    const uint32_t sa = (uint32_t(r[0] & 0xF) << 16 | r[1]) & 0xFFFFF;
    const uint32_t lsa = r[2], lea = r[3];
    const int loop = (r[0] >> 5) & 3;
    if (!s.inLoop && (s.pos >> 14) >= lsa) s.inLoop = true;
    // modulação (FM): a soma de duas saídas da pilha de som desloca a leitura da onda
    int32_t mod = 0;
    const int mdl = (r[7] >> 12) & 0xF;
    if (mdl > 4) {
      const uint32_t g = sampleCount_ * 32 + uint32_t(n);
      const int32_t m = int16_t(stack_[(g + ((r[7] >> 6) & 0x3F)) & 63]) + int16_t(stack_[(g + (r[7] & 0x3F)) & 63]);
      mod = (m * 64) >> (16 - mdl);  // em 1/64 de amostra
    }
    const int32_t pos6 = int32_t((s.pos >> 8) & 0x3F) + (mod & 0x3F);  // fração para a interpolação
    const uint32_t idx = (s.pos >> 14) + uint32_t(mod >> 6) + uint32_t(pos6 >> 6);
    const int32_t frac = pos6 & 0x3F;
    auto pcmAt = [&](uint32_t i) -> int32_t {
      if (pcm8) return int32_t(int8_t(ram[(sa + i) & 0x7FFFF])) << 8;
      const uint32_t a = (sa + i * 2) & 0x7FFFE;
      return int16_t(ram[a] << 8 | ram[a + 1]);
    };
    switch ((r[0] >> 7) & 3) {
      case 0: {
        // interpolação linear entre a amostra atual e a seguinte (dentro do laço)
        const uint32_t nxt = (idx + 1 > lea && loop) ? (loop == 1 ? lsa : lea) : idx + 1;
        smp = (pcmAt(idx) * (64 - frac) + pcmAt(nxt) * frac) >> 6;
        break;
      }
      case 1: smp = int16_t(uint16_t(lfsr_ << 8)); break;   // ruído
      default: smp = 0; break;
    }
    static const uint16_t SBX[4] = {0, 0x7FFF, 0x8000, 0xFFFF};
    smp = int16_t(uint16_t(smp) ^ SBX[(r[0] >> 9) & 3]);
    // passo: ((1024 + FNS) + PLFO) * 2^OCT / 1024 amostras por amostra de saída (fração de 14 bits)
    const int oct = int((r[8] >> 11) & 0xF) - (((r[8] >> 11) & 0x8) ? 16 : 0);
    const int32_t f = int32_t(0x400 | (r[8] & 0x3FF)) + plfo;
    const uint32_t step = uint32_t((int64_t(f) << (oct + 8)) >> 4);
    // LPCTL: 0 sem laço, 1 para frente, 2 para trás (LEA -> LSA, de novo), 3 alternado (vai e volta)
    const int64_t lo = int64_t(lsa) << 14, hi = int64_t(lea + 1) << 14;
    if (!s.rev) {
      s.pos += step;
      if ((s.pos >> 14) > lea) {
        const int64_t over = int64_t(s.pos) - hi;
        if (loop == 0) { s.pos = lea << 14; s.phase = 3; s.env = 0x3FF; s.on = false; }
        else if (loop == 1) s.pos = uint32_t(lo + over);
        else { s.rev = true; s.pos = uint32_t(std::max(lo, hi - 1 - over)); }
        if ((s.pos >> 14) > lea) s.pos = lsa << 14;
      }
    } else {
      const int64_t p = int64_t(s.pos) - int64_t(step);
      if (p < lo) {
        const int64_t under = lo - p;
        if (loop == 3) { s.rev = false; s.pos = uint32_t(std::min(hi - 1, lo + under)); }
        else s.pos = uint32_t(std::max(lo, hi - 1 - under));
        if (loop < 2) s.rev = false;   // o programa trocou o LPCTL no meio
      } else {
        s.pos = uint32_t(p);
      }
    }
  }
  // LFO anda entre a leitura do PLFO e a do ALFO; o ruído anda uma vez por canal
  if (--s.lfoTime <= 0) {
    s.lfoCount++;
    const int lf = (lr >> 10) & 31;
    s.lfoTime = (((8 - (lf & 3)) << 7) >> (lf >> 2)) - 4;
  }
  if (lr & 0x8000) s.lfoCount = 0;
  lfsr_ = (lfsr_ >> 1) | (((lfsr_ >> 5) ^ lfsr_) & 1) << 16;
  if (silent) { s.out = 0; return 0; }
  // atenuação: envelope + TL + ALFO (SDIR = sem nada); 0x40 passos = 6 dB, com 64 degraus dentro de cada
  if (!(r[6] & 0x100)) {
    int lvl = (s.phase == 0 && (r[4] & 0x20)) ? 0 : s.env;   // EGHOLD: ataque no máximo
    lvl += (r[6] & 0xFF) << 2;
    const int as = lr & 7;
    if (as) {
      uint8_t a;
      switch ((lr >> 3) & 3) {
        case 0: a = uint8_t(s.lfoCount & ~1); break;
        case 1: a = uint8_t((int8_t(s.lfoCount) >> 7) & ~1); break;
        case 2: a = uint8_t((s.lfoCount ^ uint8_t(int8_t(s.lfoCount) >> 7)) << 1); break;
        default: a = uint8_t(lfsr_ & 0xFE); break;
      }
      lvl += a >> (7 - as);
    }
    lvl = std::min(lvl, 0x3FF);
    smp = (int16_t(smp) * ((lvl & 0x3F) ^ 0x7F)) >> ((lvl >> 6) + 7);
  }
  s.out = int16_t(smp);
  // envio para o DSP (IMXL/ISEL) e saída direta (DISDL/DIPAN)
  const int imxl = r[10] & 7;
  if (imxl) {
    uint32_t& m = dsp_.mixs[(r[10] >> 3) & 0xF];
    m = (m + uint32_t((int32_t(s.out) << 4) >> (7 - imxl))) & 0xFFFFF;
  }
  const int sdl = (r[0xB] >> 13) & 7;
  if (sdl) {
    int32_t vl, vr;
    panVolume(sdl, (r[0xB] >> 8) & 31, vl, vr);
    outL += (s.out * vl) >> 14;
    outR += (s.out * vr) >> 14;
  }
  return s.out;
}

void Scsp::sample(int16_t& l, int16_t& r) {
  int32_t L = 0, R = 0;
  if (dsp_.active) dspRun();
  std::memset(dsp_.mixs, 0, sizeof dsp_.mixs);
  for (int n = 0; n < 32; n++) {
    const int16_t o = slotStep(n, L, R);
    // pilha de som: a saída de cada canal entra 4 canais depois (para a modulação dos outros)
    const uint32_t g = sampleCount_ * 32 + uint32_t(n);
    if (!(slot_[(n + 28) & 31].r[6] & 0x200)) stack_[(g - 4) & 63] = uint16_t(delay_[0]);
    delay_[0] = delay_[1]; delay_[1] = delay_[2]; delay_[2] = delay_[3]; delay_[3] = o;
  }
  // saída de efeito: EFREG do DSP pelos canais 0-15 (EFSDL/EFPAN de cada um)
  for (int n = 0; n < 16; n++) {
    const uint16_t e = slot_[n].r[0xB];
    if (!(e & 0xE0) || !dsp_.efreg[n]) continue;
    int32_t vl, vr;
    panVolume((e >> 5) & 7, e & 31, vl, vr);
    L += (int16_t(dsp_.efreg[n]) * vl) >> 14;
    R += (int16_t(dsp_.efreg[n]) * vr) >> 14;
  }
  // entrada externa (CD): esquerda pelo envio de efeito do canal 16, direita pelo do 17
  {
    int32_t vl, vr;
    panVolume((slot_[16].r[0xB] >> 5) & 7, slot_[16].r[0xB] & 31, vl, vr);
    L += (exl_ * vl) >> 14; R += (exl_ * vr) >> 14;
    panVolume((slot_[17].r[0xB] >> 5) & 7, slot_[17].r[0xB] & 31, vl, vr);
    L += (exr_ * vl) >> 14; R += (exr_ * vr) >> 14;
  }
  // volume geral (MVOL): cada 2 passos dobram; os pares são 3/4 do ímpar seguinte
  const int mvol = common_[0] & 0xF;
  int32_t mv = 2 << (mvol >> 1);
  if (!(mvol & 1)) mv -= mv >> 2;
  if (!mvol) mv = 0;
  L = (L * mv) >> 8;
  R = (R * mv) >> 8;
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
    io.sync(s.pos); io.sync(s.env); io.sync(s.phase); io.sync(s.on); io.sync(s.inLoop); io.sync(s.rev); io.sync(s.lfoTime); io.sync(s.lfoCount); io.sync(s.out);
    if (io.reading()) { s.env = std::clamp(s.env, 0, 0x3FF); s.phase &= 3; s.lfoTime = std::clamp(s.lfoTime, 1, 1024); }
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
  {
    Dsp& d = dsp_;
    io.raw(d.mprog, sizeof d.mprog); io.raw(d.coef, sizeof d.coef); io.raw(d.madrs, sizeof d.madrs); io.raw(d.efreg, sizeof d.efreg);
    io.raw(d.temp, sizeof d.temp); io.raw(d.mems, sizeof d.mems); io.raw(d.mixs, sizeof d.mixs);
    io.sync(d.sft); io.sync(d.y); io.sync(d.inputs); io.sync(d.readValue); io.sync(d.rwAddr);
    io.sync(d.frc); io.sync(d.adrs); io.sync(d.writeValue); io.sync(d.mdecCt); io.sync(d.readPending); io.sync(d.writePending);
    if (io.reading()) {
      d.active = false;
      for (uint16_t x : d.mprog) if (x) { d.active = true; break; }
      for (auto& c : d.coef) c &= 0x1FFF;
      for (auto& t : d.temp) t = sext(uint32_t(t) & 0xFFFFFF, 24);
      for (auto& t : d.mems) t = sext(uint32_t(t) & 0xFFFFFF, 24);
      for (auto& m : d.mixs) m &= 0xFFFFF;
      d.sft &= 0x3FFFFFF; d.y &= 0xFFFFFF; d.rwAddr &= 0x7FFFF; d.adrs &= 0xFFF; d.frc &= 0x1FFF;
      if (d.readPending > 2) d.readPending = 0;
    }
  }
  if (io.reading()) {
    for (auto& t : tctl_) t &= 7;
    lfsr_ &= 0x1FFFF;
    if (cycleDebt_ > 100000 || cycleDebt_ < -100000) cycleDebt_ = 0;
  }
}

}  // namespace indigo::saturn
