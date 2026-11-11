#include "sa1.h"

#include <algorithm>

namespace indigo::snes {

namespace {
// espelhamento de ROM com tamanho que não é potência de 2 (como no barramento do SNES)
size_t mirrorRom(size_t addr, size_t size) {
  if (!size) return 0;
  size_t base = 0, mask = size_t(1) << 23;
  while (addr >= size) {
    while (!(addr & mask)) mask >>= 1;
    addr -= mask;
    if (size > mask) { size -= mask; base += mask; }
    mask >>= 1;
  }
  return base + addr;
}

enum : uint8_t {
  SA1_IRQ_SCPU = 0x80, SA1_IRQ_TIMER = 0x40, SA1_IRQ_DMA = 0x20, SA1_NMI_SCPU = 0x10,
  SCPU_IRQ_SA1 = 0x80, SCPU_IVSW = 0x40, SCPU_IRQ_CC = 0x20, SCPU_NVSW = 0x10,
};
}  // namespace

SA1::SA1(const std::vector<uint8_t>& rom, std::vector<uint8_t>& bwram) : rom_(rom), bw_(bwram) {
  cpu.bus = this;
  reset();
}

void SA1::reset() {
  std::fill(std::begin(iram_), std::end(iram_), 0);
  ctrl_ = 0x20;  // o SA-1 começa parado, em reset
  resetFlag_ = true;
  scpuSie_ = sa1Sie_ = scpuCtrl_ = timerCtrl_ = 0;
  scpuFlags_ = sa1Flags_ = 0;
  nmiPrev_ = false;
  crv_ = cnv_ = civ_ = snv_ = siv_ = 0;
  hpos_ = vpos_ = hcount_ = vcount_ = hcr_ = vcr_ = 0;
  for (int k = 0; k < 4; k++) { bankHi_[k] = false; bankRom_[k] = static_cast<uint8_t>(k); }
  bwSnes_ = bwSa1_ = iramWriteSnes_ = iramWriteSa1_ = 0;
  bwBitmap_ = bw2bpp_ = false;
  dmaCtrl_ = ccParam_ = 0;
  src_ = dst_ = 0;
  dmaCount_ = 0;
  cc1Active_ = false;
  cc2Line_ = 0;
  std::fill(std::begin(brf_), std::end(brf_), 0);
  mathCtrl_ = mathOverflow_ = 0;
  mathA_ = mathB_ = 0;
  mathRes_ = 0;
  vda_ = 0; vbit_ = 0; vlen_ = 16; vauto_ = false;
  debt_ = 0;
  timerFrac_ = 0;
  cpu.irqLine = false;
  cpu.nmiPending = false;
}

void SA1::run(int clocks) {
  // timer H/V: um passo a cada 2 ciclos do SA-1 (4 do relógio mestre)
  timerFrac_ += clocks;
  if (timerFrac_ >= 4) { timerTick(timerFrac_ >> 2); timerFrac_ &= 3; }
  if (halted()) { debt_ = 0; return; }
  debt_ += clocks;
  while (debt_ > 0 && !halted()) cpu.step();
}

void SA1::timerTick(int ticks) {
  for (int t = 0; t < ticks; t++) {
    if (timerCtrl_ & 0x80) {  // linear de 18 bits
      hpos_++;
      vpos_ = static_cast<uint16_t>((vpos_ + (hpos_ >> 9)) & 0x1FF);
      hpos_ &= 0x1FF;
    } else if (++hpos_ >= 341) {
      hpos_ = 0;
      if (++vpos_ >= 262) vpos_ = 0;
    }
    const bool hen = timerCtrl_ & 1, ven = timerCtrl_ & 2;
    bool hit = false;
    if (hen && ven) hit = hpos_ == hcount_ && vpos_ == vcount_;
    else if (hen) hit = hpos_ == hcount_;
    else if (ven) hit = hpos_ == 0 && vpos_ == vcount_;
    if (hit) { sa1Flags_ |= SA1_IRQ_TIMER; updateIrqs(); }
  }
}

void SA1::updateIrqs() {
  cpu.irqLine = !halted() && (sa1Flags_ & sa1Sie_ & (SA1_IRQ_SCPU | SA1_IRQ_TIMER | SA1_IRQ_DMA));
  const bool nmi = !halted() && (sa1Flags_ & sa1Sie_ & SA1_NMI_SCPU);
  if (nmi && !nmiPrev_) cpu.nmiPending = true;
  nmiPrev_ = nmi;
}

// ---------------------------------------------------------------- memórias
uint8_t SA1::romRead(uint32_t addr) const {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  size_t off;
  if (bank >= 0xC0) {
    off = size_t(bankRom_[(bank >> 4) & 3]) * 0x100000 + ((size_t(bank & 0x0F) << 16) | a);
  } else if (a >= 0x8000) {
    // Super MMC: $00-1F, $20-3F, $80-9F e $A0-BF são os blocos C, D, E e F (fixos no 1º, 2º, 3º e 4º MB, ou o
    // bloco escolhido quando o bit 7 do registrador está ligado)
    const int slot = ((bank >> 7) << 1) | ((bank >> 5) & 1);
    const int block = bankHi_[slot] ? bankRom_[slot] : slot;
    off = size_t(block) * 0x100000 + size_t(bank & 0x1F) * 0x8000 + (a & 0x7FFF);
  } else {
    return 0;
  }
  return rom_.empty() ? 0 : rom_[mirrorRom(off, rom_.size())];
}

uint8_t SA1::bwramRead(uint32_t off, bool bitmap) const {
  if (bw_.empty()) return 0xFF;
  const size_t mask = bw_.size() - 1;
  if (!bitmap) return bw_[off & mask];
  // bitmap: cada byte do lado do SA-1 é um ponto de 2 ou 4 bits da BW-RAM
  if (bw2bpp_) return (bw_[(off >> 2) & mask] >> ((off & 3) << 1)) & 3;
  return (bw_[(off >> 1) & mask] >> ((off & 1) << 2)) & 15;
}

void SA1::bwramWrite(uint32_t off, uint8_t v, bool bitmap) {
  if (bw_.empty()) return;
  const size_t mask = bw_.size() - 1;
  dirty = true;
  if (!bitmap) { bw_[off & mask] = v; return; }
  if (bw2bpp_) {
    const int sh = (off & 3) << 1;
    uint8_t& b = bw_[(off >> 2) & mask];
    b = static_cast<uint8_t>((b & ~(3 << sh)) | ((v & 3) << sh));
  } else {
    const int sh = (off & 1) << 2;
    uint8_t& b = bw_[(off >> 1) & mask];
    b = static_cast<uint8_t>((b & ~(15 << sh)) | ((v & 15) << sh));
  }
}

// ---------------------------------------------------------------- barramento do SA-1
uint8_t SA1::read(uint32_t addr) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  debt_ -= 2;
  if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
    if (a < 0x0800 || (a >= 0x3000 && a < 0x3800)) return iramRead(a);
    if (a >= 0x2200 && a < 0x2400) return sa1RegRead(a);
    if (a >= 0x6000 && a < 0x8000) { debt_ -= 2; return bwramRead(uint32_t(bwSa1_) * 0x2000 + (a & 0x1FFF), bwBitmap_); }
    if (a >= 0x8000) {
      if (bank == 0x00) {  // vetores do SA-1 vêm dos registradores
        switch (a) {
          case 0xFFEA: case 0xFFFA: return static_cast<uint8_t>(cnv_);
          case 0xFFEB: case 0xFFFB: return static_cast<uint8_t>(cnv_ >> 8);
          case 0xFFEE: case 0xFFFE: return static_cast<uint8_t>(civ_);
          case 0xFFEF: case 0xFFFF: return static_cast<uint8_t>(civ_ >> 8);
          case 0xFFFC: return static_cast<uint8_t>(crv_);
          case 0xFFFD: return static_cast<uint8_t>(crv_ >> 8);
          default: break;
        }
      }
      return romRead(addr);
    }
    return 0xFF;
  }
  if (bank >= 0xC0) return romRead(addr);
  if (bank >= 0x40 && bank < 0x50) { debt_ -= 2; return bwramRead(addr & 0xFFFFF, false); }
  if (bank >= 0x60 && bank < 0x70) { debt_ -= 2; return bwramRead(addr & 0xFFFFF, true); }
  return 0xFF;
}

void SA1::write(uint32_t addr, uint8_t v) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  debt_ -= 2;
  if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
    if (a < 0x0800 || (a >= 0x3000 && a < 0x3800)) {
      if (iramWriteSa1_ & (1 << ((a >> 8) & 7))) iram_[a & 0x7FF] = v;
      return;
    }
    if (a >= 0x2200 && a < 0x2400) { sa1RegWrite(a, v); return; }
    if (a >= 0x6000 && a < 0x8000) { debt_ -= 2; bwramWrite(uint32_t(bwSa1_) * 0x2000 + (a & 0x1FFF), v, bwBitmap_); }
    return;
  }
  if (bank >= 0x40 && bank < 0x50) { debt_ -= 2; bwramWrite(addr & 0xFFFFF, v, false); return; }
  if (bank >= 0x60 && bank < 0x70) { debt_ -= 2; bwramWrite(addr & 0xFFFFF, v, true); }
}

// ---------------------------------------------------------------- lado do SNES
bool SA1::snesRead(uint32_t addr, uint8_t& v) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
    if (a >= 0x6000 && a < 0x8000) {
      const uint32_t off = uint32_t(bwSnes_) * 0x2000 + (a & 0x1FFF);
      v = cc1Active_ ? charConv1(off) : bwramRead(off, false);
      return true;
    }
    if (a >= 0x8000) {
      if (bank == 0x00) {  // vetores de NMI e IRQ do SNES trocados pelo SA-1
        if ((scpuCtrl_ & SCPU_NVSW) && (a == 0xFFEA || a == 0xFFEB)) { v = static_cast<uint8_t>(snv_ >> ((a & 1) * 8)); return true; }
        if ((scpuCtrl_ & SCPU_IVSW) && (a == 0xFFEE || a == 0xFFEF)) { v = static_cast<uint8_t>(siv_ >> ((a & 1) * 8)); return true; }
      }
      v = romRead(addr);
      return true;
    }
    return false;
  }
  if (bank >= 0xC0) { v = romRead(addr); return true; }
  if (bank >= 0x40 && bank < 0x50) {
    const uint32_t off = addr & 0xFFFFF;
    v = cc1Active_ ? charConv1(off) : bwramRead(off, false);
    return true;
  }
  return false;
}

bool SA1::snesWrite(uint32_t addr, uint8_t v, bool& changed) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  changed = false;
  auto put = [&](uint32_t off) {
    if (bw_.empty()) return;
    uint8_t& b = bw_[off & (bw_.size() - 1)];
    if (b != v) { b = v; changed = true; }
  };
  if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
    if (a >= 0x6000 && a < 0x8000) { put(uint32_t(bwSnes_) * 0x2000 + (a & 0x1FFF)); return true; }
    return a >= 0x8000;
  }
  if (bank >= 0x40 && bank < 0x50) { put(addr & 0xFFFFF); return true; }
  return bank >= 0xC0;
}

uint8_t SA1::ioRead(uint16_t a, uint8_t openBus) {
  if (a >= 0x3000 && a < 0x3800) return iram_[a & 0x7FF];
  if (a == 0x2300) return static_cast<uint8_t>((scpuCtrl_ & 0x0F) | scpuFlags_);
  return openBus;
}

void SA1::ioWrite(uint16_t a, uint8_t v) {
  if (a >= 0x3000 && a < 0x3800) {
    if (iramWriteSnes_ & (1 << ((a >> 8) & 7))) iram_[a & 0x7FF] = v;
    return;
  }
  switch (a) {
    case 0x2200: {  // controle do SA-1: mensagem, NMI, reset, espera, IRQ
      const bool wasStopped = ctrl_ & 0x60, stop = v & 0x60;
      if ((ctrl_ & 0x40) && (v & 0x20)) resetFlag_ = true;
      if (stop && !wasStopped) {
        if (v & 0x20) resetFlag_ = true;
      } else if (!stop && wasStopped) {
        ctrl_ = v;
        if (resetFlag_ && !(v & 0x20)) {
          resetFlag_ = false;
          cpu.reset();  // lê o vetor de reset dos registradores
          debt_ = 0;
        }
      }
      ctrl_ = v;
      if (v & 0x80) sa1Flags_ |= SA1_IRQ_SCPU;
      if (v & 0x10) sa1Flags_ |= SA1_NMI_SCPU;
      updateIrqs();
      break;
    }
    case 0x2201: scpuSie_ = v; break;
    case 0x2202:
      if (v & 0x80) scpuFlags_ &= ~SCPU_IRQ_SA1;
      if (v & 0x20) scpuFlags_ &= ~SCPU_IRQ_CC;
      break;
    case 0x2203: crv_ = (crv_ & 0xFF00) | v; break;
    case 0x2204: crv_ = static_cast<uint16_t>((crv_ & 0x00FF) | (v << 8)); break;
    case 0x2205: cnv_ = (cnv_ & 0xFF00) | v; break;
    case 0x2206: cnv_ = static_cast<uint16_t>((cnv_ & 0x00FF) | (v << 8)); break;
    case 0x2207: civ_ = (civ_ & 0xFF00) | v; break;
    case 0x2208: civ_ = static_cast<uint16_t>((civ_ & 0x00FF) | (v << 8)); break;
    case 0x2220: case 0x2221: case 0x2222: case 0x2223:
      bankHi_[a & 3] = v & 0x80;
      bankRom_[a & 3] = v & 7;
      break;
    case 0x2224: bwSnes_ = v & 0x1F; break;
    case 0x2226: case 0x2228: break;  // proteção de escrita da BW-RAM (não usada pelos jogos conhecidos)
    case 0x2229: iramWriteSnes_ = v; break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237: sharedWrite(a, v); break;
    default: break;
  }
}

// ---------------------------------------------------------------- registradores do lado do SA-1
uint8_t SA1::sa1RegRead(uint16_t a) {
  switch (a) {
    case 0x2301: return static_cast<uint8_t>((ctrl_ & 0x0F) | sa1Flags_);
    case 0x2302: hcr_ = hpos_; vcr_ = vpos_; return static_cast<uint8_t>(hcr_);
    case 0x2303: return static_cast<uint8_t>(hcr_ >> 8);
    case 0x2304: return static_cast<uint8_t>(vcr_);
    case 0x2305: return static_cast<uint8_t>(vcr_ >> 8);
    case 0x2306: case 0x2307: case 0x2308: case 0x2309: case 0x230A: return static_cast<uint8_t>(mathRes_ >> ((a - 0x2306) * 8));
    case 0x230B: return mathOverflow_;
    case 0x230C: case 0x230D: {
      uint32_t d = varRead(vda_) | (varRead(vda_ + 1) << 8) | (uint32_t(varRead(vda_ + 2)) << 16);
      d >>= vbit_;
      if (a == 0x230D && vauto_) {
        vbit_ = static_cast<uint8_t>(vbit_ + vlen_);
        vda_ = (vda_ + (vbit_ >> 3)) & 0xFFFFFF;
        vbit_ &= 7;
      }
      return static_cast<uint8_t>(a == 0x230C ? d : d >> 8);
    }
    default: return 0xFF;
  }
}

uint8_t SA1::varRead(uint32_t addr) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
    if (a >= 0x8000) return romRead(addr);
    if (a >= 0x6000) return bwramRead(uint32_t(bwSnes_) * 0x2000 + (a & 0x1FFF), false);
    if (a < 0x0800 || (a >= 0x3000 && a < 0x3800)) return iramRead(a);
    return 0;
  }
  if (bank >= 0xC0) return romRead(addr);
  if (bank >= 0x40 && bank < 0x50) return bwramRead(addr & 0xFFFFF, false);
  return 0;
}

void SA1::sa1RegWrite(uint16_t a, uint8_t v) {
  switch (a) {
    case 0x2209:  // controle do lado do SNES: mensagem, troca dos vetores, IRQ para o SNES
      scpuCtrl_ = v;
      if (v & 0x80) scpuFlags_ |= SCPU_IRQ_SA1;
      scpuFlags_ = static_cast<uint8_t>((scpuFlags_ & ~(SCPU_IVSW | SCPU_NVSW)) | (v & (SCPU_IVSW | SCPU_NVSW)));
      break;
    case 0x220A: sa1Sie_ = v; updateIrqs(); break;
    case 0x220B:
      if (v & 0x80) sa1Flags_ &= ~SA1_IRQ_SCPU;
      if (v & 0x40) sa1Flags_ &= ~SA1_IRQ_TIMER;
      if (v & 0x20) sa1Flags_ &= ~SA1_IRQ_DMA;
      if (v & 0x10) sa1Flags_ &= ~SA1_NMI_SCPU;
      updateIrqs();
      break;
    case 0x220C: snv_ = (snv_ & 0xFF00) | v; break;
    case 0x220D: snv_ = static_cast<uint16_t>((snv_ & 0x00FF) | (v << 8)); break;
    case 0x220E: siv_ = (siv_ & 0xFF00) | v; break;
    case 0x220F: siv_ = static_cast<uint16_t>((siv_ & 0x00FF) | (v << 8)); break;
    case 0x2210: timerCtrl_ = v; break;
    case 0x2211: hpos_ = vpos_ = 0; break;
    case 0x2212: hcount_ = (hcount_ & 0xFF00) | v; break;
    case 0x2213: hcount_ = static_cast<uint16_t>((hcount_ & 0x00FF) | ((v & 1) << 8)); break;
    case 0x2214: vcount_ = (vcount_ & 0xFF00) | v; break;
    case 0x2215: vcount_ = static_cast<uint16_t>((vcount_ & 0x00FF) | ((v & 1) << 8)); break;
    case 0x2225: bwBitmap_ = v & 0x80; bwSa1_ = v & 0x7F; break;
    case 0x2227: break;
    case 0x222A: iramWriteSa1_ = v; break;
    case 0x2230: dmaCtrl_ = v; break;
    case 0x2231: case 0x2232: case 0x2233: case 0x2234: case 0x2235: case 0x2236: case 0x2237: sharedWrite(a, v); break;
    case 0x2238: dmaCount_ = (dmaCount_ & 0xFF00) | v; break;
    case 0x2239: dmaCount_ = static_cast<uint16_t>((dmaCount_ & 0x00FF) | (v << 8)); break;
    case 0x223F: bw2bpp_ = v & 0x80; break;
    case 0x2250:
      mathCtrl_ = v & 3;
      if (v & 2) mathRes_ = 0;
      break;
    case 0x2251: mathA_ = (mathA_ & 0xFF00) | v; break;
    case 0x2252: mathA_ = static_cast<uint16_t>((mathA_ & 0x00FF) | (v << 8)); break;
    case 0x2253: mathB_ = (mathB_ & 0xFF00) | v; break;
    case 0x2254:
      mathB_ = static_cast<uint16_t>((mathB_ & 0x00FF) | (v << 8));
      switch (mathCtrl_) {
        case 0:  // multiplicação com sinal
          mathRes_ = static_cast<uint64_t>(int64_t(int16_t(mathA_)) * int16_t(mathB_));
          mathB_ = 0;
          break;
        case 1:  // divisão: dividendo com sinal, divisor sem sinal
          if (mathB_ == 0) mathRes_ = 0;
          else {
            // resto sempre positivo (o quociente arredonda para baixo), como no chip
            const int32_t n = int16_t(mathA_), d = mathB_;
            const int32_t r = ((n % d) + d) % d;
            const int32_t q = (n - r) / d;
            mathRes_ = (uint64_t(uint16_t(r)) << 16) | uint16_t(q);
          }
          break;
        default: {  // soma de produtos (40 bits)
          mathRes_ += static_cast<uint64_t>(int64_t(int16_t(mathA_)) * int16_t(mathB_));
          mathOverflow_ = mathRes_ > 0xFFFFFFFFFFull ? 0x80 : 0;
          mathRes_ &= 0xFFFFFFFFFFull;
          mathB_ = 0;
          break;
        }
      }
      break;
    case 0x2258:
      vauto_ = v & 0x80;
      vlen_ = v & 15;
      if (vlen_ == 0) vlen_ = 16;
      if (!vauto_) {
        vbit_ = static_cast<uint8_t>(vbit_ + vlen_);
        vda_ = (vda_ + (vbit_ >> 3)) & 0xFFFFFF;
        vbit_ &= 7;
      }
      break;
    case 0x2259: vda_ = (vda_ & 0xFFFF00) | v; break;
    case 0x225A: vda_ = (vda_ & 0xFF00FF) | (uint32_t(v) << 8); break;
    case 0x225B: vda_ = (vda_ & 0x00FFFF) | (uint32_t(v) << 16); vbit_ = 0; break;
    default:
      if (a >= 0x2240 && a < 0x2250) {  // registradores de bitmap (conversão tipo 2)
        brf_[a & 15] = v;
        if ((a & 7) == 7 && (dmaCtrl_ & 0x80) && (dmaCtrl_ & 0x20) && !(dmaCtrl_ & 0x10)) charConv2();
      }
      break;
  }
}

void SA1::sharedWrite(uint16_t a, uint8_t v) {
  switch (a) {
    case 0x2231:
      ccParam_ = v;
      if (v & 0x80) cc1Active_ = false;
      break;
    case 0x2232: src_ = (src_ & 0xFFFF00) | v; break;
    case 0x2233: src_ = (src_ & 0xFF00FF) | (uint32_t(v) << 8); break;
    case 0x2234: src_ = (src_ & 0x00FFFF) | (uint32_t(v) << 16); break;
    case 0x2235: dst_ = (dst_ & 0xFFFF00) | v; break;
    case 0x2236:
      dst_ = (dst_ & 0xFF00FF) | (uint32_t(v) << 8);
      if (dmaCtrl_ & 0x80) {
        if (!(dmaCtrl_ & 0x20) && !(dmaCtrl_ & 0x04)) dma();  // DMA comum para a I-RAM
        if ((dmaCtrl_ & 0x20) && (dmaCtrl_ & 0x10)) {          // conversão tipo 1: o SNES lê a BW-RAM convertida
          cc1Active_ = true;
          scpuFlags_ |= SCPU_IRQ_CC;
        }
      }
      break;
    case 0x2237:
      dst_ = (dst_ & 0x00FFFF) | (uint32_t(v) << 16);
      if ((dmaCtrl_ & 0x80) && !(dmaCtrl_ & 0x20) && (dmaCtrl_ & 0x04)) dma();  // DMA comum para a BW-RAM
      break;
  }
}

void SA1::dma() {
  const int source = dmaCtrl_ & 3;
  const bool toBw = dmaCtrl_ & 4;
  uint16_t n = dmaCount_;
  while (n--) {
    const uint32_t s = src_++, d = dst_++;
    if ((source == 1 && toBw) || (source == 2 && !toBw)) continue;  // mesma memória dos dois lados: não copia
    uint8_t v = 0;
    switch (source) {
      case 0: v = romRead(s & 0xFFFFFF); break;
      case 1: v = bwramRead(s & 0xFFFFF, false); break;
      case 2: v = iramRead(s); break;
      default: break;
    }
    if (toBw) bwramWrite(d & 0xFFFFF, v, false);
    else iram_[d & 0x7FF] = v;
    debt_ -= toBw || source == 1 ? 4 : 2;
  }
  dmaCount_ = 0;
  sa1Flags_ |= SA1_IRQ_DMA;
  updateIrqs();
}

// conversão tipo 1: o SNES lê (por DMA) a BW-RAM e recebe os blocos já no formato da VRAM; o SA-1 monta cada
// bloco de 8×8 na I-RAM a partir do bitmap linear quando o SNES chega no primeiro byte dele
uint8_t SA1::charConv1(uint32_t off) {
  const int cb = std::min(ccParam_ & 3, 2), size = (ccParam_ >> 2) & 7;
  const uint32_t storeMask = (1u << (6 - cb)) - 1;
  if ((off & storeMask) == 0 && !bw_.empty()) {
    const uint32_t bpp = 2u << (2 - cb);
    const uint32_t stride = (8u << size) >> cb;
    const uint32_t mask = static_cast<uint32_t>(bw_.size() - 1);
    const uint32_t tile = ((off - src_) & mask) >> (6 - cb);
    const uint32_t ty = tile >> size, tx = tile & ((1u << size) - 1);
    uint32_t from = src_ + ty * 8 * stride + tx * bpp;
    for (uint32_t y = 0; y < 8; y++) {
      uint64_t raw = 0;
      for (uint32_t b = 0; b < bpp; b++) raw |= uint64_t(bw_[(from + b) & mask]) << (b * 8);
      from += stride;
      uint8_t planes[8] = {};
      for (int x = 0; x < 8; x++) {
        for (uint32_t b = 0; b < bpp; b++) planes[b] |= ((raw >> b) & 1) << (7 - x);
        raw >>= bpp;
      }
      for (uint32_t b = 0; b < bpp; b++) {
        const uint32_t plane = (b & 1) | ((b >> 1) << 4);
        iram_[(dst_ + y * 2 + plane) & 0x7FF] = planes[b];
      }
    }
  }
  return iram_[(dst_ + (off & storeMask)) & 0x7FF];
}

// conversão tipo 2: o SA-1 escreve 8 pontos por vez nos registradores de bitmap e o bloco sai na I-RAM
void SA1::charConv2() {
  const int cb = std::min(ccParam_ & 3, 2);
  const int half = (cc2Line_ & 1) << 3;
  const uint32_t bpp = 2u << (2 - cb);
  const uint32_t tx = uint32_t((cc2Line_ >> 3) & 1) << (6 - cb);
  const uint32_t ty = uint32_t(cc2Line_ & 7) << 1;
  const uint32_t base = (dst_ & ~((1u << (7 - cb)) - 1)) + tx + ty;
  for (uint32_t b = 0; b < bpp; b++) {
    uint8_t byte = 0;
    for (int x = 0; x < 8; x++) byte |= ((brf_[half | x] >> b) & 1) << (7 - x);
    const uint32_t plane = (b & 1) | ((b >> 1) << 4);
    iram_[(base + plane) & 0x7FF] = byte;
  }
  cc2Line_ = (cc2Line_ + 1) & 15;
}

void SA1::sync(StateIO& io) {
  cpu.sync(io);
  io.raw(iram_, sizeof iram_);
  io.sync(debt_); io.sync(timerFrac_);
  io.sync(ctrl_); io.sync(scpuSie_); io.sync(sa1Sie_); io.sync(scpuCtrl_); io.sync(timerCtrl_);
  io.sync(scpuFlags_); io.sync(sa1Flags_); io.sync(resetFlag_); io.sync(nmiPrev_);
  io.sync(crv_); io.sync(cnv_); io.sync(civ_); io.sync(snv_); io.sync(siv_);
  io.sync(hpos_); io.sync(vpos_); io.sync(hcount_); io.sync(vcount_); io.sync(hcr_); io.sync(vcr_);
  for (bool& b : bankHi_) io.sync(b);
  io.raw(bankRom_, sizeof bankRom_);
  io.sync(bwSnes_); io.sync(bwSa1_); io.sync(iramWriteSnes_); io.sync(iramWriteSa1_);
  io.sync(bwBitmap_); io.sync(bw2bpp_);
  io.sync(dmaCtrl_); io.sync(ccParam_); io.sync(src_); io.sync(dst_); io.sync(dmaCount_);
  io.sync(cc1Active_); io.sync(cc2Line_); io.raw(brf_, sizeof brf_);
  io.sync(mathCtrl_); io.sync(mathOverflow_); io.sync(mathA_); io.sync(mathB_); io.sync(mathRes_);
  io.sync(vda_); io.sync(vbit_); io.sync(vlen_); io.sync(vauto_);
  if (io.reading()) {
    clampTo<int64_t>(debt_, -(4 * 65536 + 64), 64);  // um DMA do SA-1 deixa até 4 relógios por byte de dívida (65535 bytes)
    timerFrac_ &= 3;
    for (auto& b : bankRom_) b &= 7;
    bwSnes_ &= 0x1F; bwSa1_ &= 0x7F;
    hpos_ &= 0x1FF; vpos_ &= 0x1FF;
    if (hpos_ > 340 && !(timerCtrl_ & 0x80)) hpos_ = 0;
    if (vpos_ > 261 && !(timerCtrl_ & 0x80)) vpos_ = 0;
    cc2Line_ &= 15; mathCtrl_ &= 3;
    vda_ &= 0xFFFFFF; vbit_ &= 7;
    clampTo<uint8_t>(vlen_, 1, 16);
    mathRes_ &= 0xFFFFFFFFFFFFFFFFull;
  }
}

}  // namespace indigo::snes
