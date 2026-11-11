// Panasonic 3DO: barramento, CLIO e o núcleo (ver tdo.h).
#include "tdo.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::tdo {

namespace {
constexpr uint32_t ROM_SIZE = 0x100000;
constexpr uint32_t NVRAM_SIZE = 0x8000;
constexpr uint32_t DMA_XBUS = 1u << 20;
constexpr uint32_t IRQ_VINT0 = 1, IRQ_VINT1 = 2, IRQ_XBUS = 4, IRQ_DSP = 1u << 11, IRQ_XBUS_DMA = 1u << 29;

bool contains(const std::vector<uint8_t>& b, const char* s) {
  const size_t n = std::strlen(s);
  return std::search(b.begin(), b.end(), s, s + n) != b.end();
}
}  // namespace

bool isTdoBios(const std::vector<uint8_t>& b) {
  if (b.size() != ROM_SIZE) return false;  // as BIOS conhecidas (FZ-1, FZ-10, GoldStar, Sanyo) têm 1 MB
  if (b[4] == 0x24 && b[5] == 0xFF && b[6] == 0xAE && b[7] == 0x51) return false;  // logotipo de ROM de GBA
  if ((b[0] >> 4) != 0xE) return false;  // primeira instrução do ARM com condição "sempre" (big-endian)
  if (b.size() > 6 && b[0] == 1 && b[1] == 0x5A) return false;
  return contains(b, "3DO") || contains(b, "3do") || contains(b, "Opera") || contains(b, "OPERA");
}

bool isOperaDisc(cdrom::Disc& d) {
  int first = -1;
  for (size_t i = 0; i < d.tracks().size(); i++)
    if (d.tracks()[i].type != cdrom::TrackType::Audio) { first = int(i); break; }
  if (first < 0) return false;
  uint8_t buf[cdrom::DATA_SECTOR];
  if (!d.readData(d.tracks()[size_t(first)].start, buf)) return false;
  return buf[0] == 1 && buf[1] == 0x5A && buf[2] == 0x5A && buf[3] == 0x5A && buf[4] == 0x5A && buf[5] == 0x5A &&
         buf[6] == 1 && std::memcmp(buf + 40, "CD-ROM", 6) == 0;
}

// ------------------------------------------------------------------------------------------------ sistema

System::System(const std::vector<uint8_t>& b, cdrom::Disc* disc) : mem(MEM_SIZE), bios(ROM_SIZE), nvram(NVRAM_SIZE) {
  std::copy(b.begin(), b.begin() + std::min<size_t>(b.size(), ROM_SIZE), bios.begin());
  cd.setDisc(disc);
  dsp.readMem = [this](uint32_t a) { return a < MEM_SIZE ? mem[a] : uint8_t(0); };
  dsp.writeMem = [this](uint32_t a, uint8_t v) { if (a < MEM_SIZE) mem[a] = v; };
  dsp.irq = [this]() { raise0(IRQ_DSP); };
  // troca de bloco do DMA: canais de entrada só interrompem com o bit do MODE; os de saída sempre
  dsp.rollover = [this](int ch) {
    if (ch < 13) { if (mode_ & (1u << (16 + ch))) raise0(1u << (16 + ch)); }
    else if (ch >= 16 && ch < 20) raise0(1u << (12 + ch - 16));
  };
  reset();
}

void System::reset() {
  std::fill(mem.begin(), mem.end(), 0);
  madam.reset();
  dsp.clearAll();
  std::memset(dsp.code, 0, sizeof dsp.code);
  cd.reset();
  cpu.reset(0);
  overlay_ = true;
  memoryConfig(madam.regs[1]);
  pend0_ = pend1_ = mask0_ = mask1_ = 0;
  vint0_ = vint1_ = 0xFFFFFFFFu;
  for (int i = 0; i < 16; i++) timers_[i] = reload_[i] = 0;
  timerCtl_[0] = timerCtl_[1] = 0;
  slack_ = 336;
  timerFrac_ = 0;
  dmaEnable_ = 0;
  xbusSel_ = xbusPollEn_ = xbusMedia_ = 0;
  expctl_ = 0x80;  // a CPU tem o barramento de expansão
  seed_ = 0x13579BDF;
  csysbits_ = audin_ = audout_ = wdog_ = mode_ = badbits_ = 0;
  cstat_ = 0x01;  // reinício por ligar o aparelho
  hdelay_ = adbio_ = adbctl_ = type04_ = xfrcnt_ = dipir1_ = dipir2_ = 0;
  std::memset(clioOther_, 0, sizeof clioOther_);
  std::memset(sport_, 0, sizeof sport_);
  svfColor_ = uncleAddr_ = slowIn_ = 0;
  slowOut_ = 4;
  line_ = field_ = 0;
  budget_ = stall_ = 0;
  audioAcc_ = 0;
  audio.clear();
  std::memset(fb, 0, sizeof fb);
  if (cd.hasDisc()) mediaChange();  // disco já na gaveta ao ligar: avisado como troca de disco
}

// MSYSBITS: bits 5-6 tamanho do conjunto 0 da DRAM, bits 3-4 do conjunto 1 (0 nada, 1 = 1 MB, 2 = 4 MB,
// 3 = 16 MB), bits 0-2 MB de VRAM. Ver o mapa em tdo.h.
void System::memoryConfig(uint32_t msys) {
  constexpr int MB = 1, SPACE = 16;
  const int set0 = ((1 << (((msys >> 5) & 3) * 2)) >> 2) * MB;
  const int set1 = ((1 << (((msys >> 3) & 3) * 2)) >> 2) * MB;
  const int vram = std::min<int>(int(msys & 7), SPACE);
  const int dram = std::min(set0 + set1, SPACE - vram);
  for (int i = 0; i < 16; i++) mbMap_[i] = -1;
  for (int m = 0; m < dram; m++) mbMap_[m] = int8_t(m < set0 ? 0 : 1);
  if (vram && dram < SPACE) mbMap_[dram] = 2;
}

void System::mediaChange() {
  xbusMedia_ = 1;
  if (!(dipir1_ & 0x8000)) dipir1_ = 0x8000;  // pedido ativo, dispositivo 0
  else dipir2_ = 0x8000;
  pend1_ |= 2;
  updateFiq();
}

void System::updateFiq() {
  // bit 31 da palavra 0: há interrupção pendente e ligada na palavra 1 (a máscara dele está sempre ligada)
  if (pend1_ & mask1_) pend0_ |= 0x80000000u;
  else pend0_ &= 0x7FFFFFFFu;
  cpu.setFiq((pend0_ & (mask0_ | 0x80000000u)) != 0);
}

uint32_t System::read32(uint32_t a) {
  if (a < 0x01000000) {
    if (overlay_ && a < 0x200000) {
      if (a >= ROM_SIZE) return 0xFFFFFFFFu;  // banco de kanji (ausente)
      const uint32_t o = a & (ROM_SIZE - 4);
      return uint32_t(bios[o]) << 24 | uint32_t(bios[o + 1]) << 16 | uint32_t(bios[o + 2]) << 8 | bios[o + 3];
    }
    const int32_t p = phys(a);
    return p < 0 ? 0 : madam.m32(uint32_t(p));
  }
  if (a >= 0x03000000 && a < 0x03100000) {
    if ((adbio_ & 0x44) == 0x44) return 0xFFFFFFFFu;  // banco de kanji escolhido pelo ADBIO
    const uint32_t o = a & (ROM_SIZE - 4);
    return uint32_t(bios[o]) << 24 | uint32_t(bios[o + 1]) << 16 | uint32_t(bios[o + 2]) << 8 | bios[o + 3];
  }
  if (a >= 0x03140000 && a < 0x03180000) return nvram[(a >> 2) & (NVRAM_SIZE - 1)];
  if (a >= 0x03180000 && a < 0x031C0000) {  // porta serial de diagnóstico: devolve 0, 0, 1, 0, ... (sem modo diag)
    if (a & 0x3FFFF) return 0;
    const uint32_t b = slowOut_ & 1;
    slowOut_ >>= 1;
    return b;
  }
  if (a >= 0x03200000 && a < 0x03210000) return svfRead(a & 0xFFFF);
  if (a >= 0x03300000 && a < 0x03400000) {
    const uint32_t off = a & 0x7FC;
    if (const int ch = dspDmaChannel(off); ch >= 0) return dsp.hostDmaRead(ch, int(off >> 2) & 3) & 0x3FFFFC;
    return madam.read(a & 0xFFFFF);
  }
  if (a >= 0x03400000 && a < 0x03500000) return clioRead(a & 0xFFFF);
  return 0;
}

uint8_t System::read8(uint32_t a) {
  if (a < 0x01000000 && !(overlay_ && a < 0x200000)) {
    const int32_t p = phys(a);
    return p < 0 ? 0 : mem[size_t(p)];
  }
  if (a >= 0x03000000 && a < 0x03100000 && (adbio_ & 0x44) != 0x44) return bios[a & (ROM_SIZE - 1)];
  const uint32_t w = read32(a & ~3u);
  return uint8_t(w >> (24 - (a & 3) * 8));
}

void System::write32(uint32_t a, uint32_t v) {
  if (a < 0x01000000) {
    if (overlay_ && a < 0x200000) { overlay_ = false; return; }  // a escrita só tira a BIOS de cima da DRAM
    const int32_t p = phys(a);
    if (p < 0) return;
    const size_t o = size_t(p) & ~size_t(3);
    mem[o] = uint8_t(v >> 24); mem[o + 1] = uint8_t(v >> 16); mem[o + 2] = uint8_t(v >> 8); mem[o + 3] = uint8_t(v);
    return;
  }
  if (a >= 0x03140000 && a < 0x03180000) {
    uint8_t& n = nvram[(a >> 2) & (NVRAM_SIZE - 1)];
    if (n != uint8_t(v)) { n = uint8_t(v); nvramDirty = true; }
    return;
  }
  if (a >= 0x03200000 && a < 0x03210000) { svfWrite(a & 0xFFFF, v); return; }
  if (a >= 0x03180000 && a < 0x031C0000) { if (!(a & 0x3FFFF)) slowIn_ = slowIn_ << 1 | (v & 1); return; }
  if (a >= 0x03300000 && a < 0x03400000) {
    const uint32_t off = a & 0x7FC;
    if (const int ch = dspDmaChannel(off); ch >= 0) { dsp.hostDmaWrite(ch, int(off >> 2) & 3, v & 0x3FFFFC); return; }
    stall_ += madam.write(a & 0xFFFFF, v);
    if (off == 0x0004) memoryConfig(madam.regs[1]);
    return;
  }
  if (a >= 0x03400000 && a < 0x03500000) { clioWrite(a & 0xFFFF, v); return; }
}

void System::write8(uint32_t a, uint8_t v) {
  if (a < 0x01000000) {
    if (overlay_ && a < 0x200000) { overlay_ = false; return; }
    const int32_t p = phys(a);
    if (p >= 0) mem[size_t(p)] = v;
    return;
  }
  if (a >= 0x03140000 && a < 0x03180000) { write32(a & ~3u, v); return; }
  if (a >= 0x03400000 && a < 0x03500000) { clioWrite(a & 0xFFFC, v); return; }
}

// dispositivo do XBUS que não existe: o acesso não termina e a CPU leva um "data abort" (ABORTBITS do MADAM,
// bit 2 = tempo do CLIO); é assim que a BIOS descobre o que está ligado
void System::xbusTimeout() {
  madam.regs[0x20 / 4] |= 4;
  cpu.dataAbort();
}

// SPORT (VRAM TC528267): página de 2 KB = bits 2-10 do endereço; bits 13-15 escolhem a operação
//   0x0000 leitura: copia a página para o registrador de transferência; escrita: a página recebe o registrador
//     de transferência nos bits ligados do valor escrito (máscara)
//   0x2000 registrador de cor  0x4000 escrita: preenche a página com a cor nos bits da máscara  0x6000 refresh
uint32_t System::svfRead(uint32_t off) {
  const uint32_t page = 0x200000 + (((off >> 2) & 0x1FF) << 11);
  switch (off & 0xE000) {
    case 0x0000:
      for (int i = 0; i < 512; i++) sport_[i] = madam.m32(page + uint32_t(i) * 4);
      stall_ += 32;
      return 0;
    case 0x2000: return svfColor_;
    default: return 0;
  }
}

void System::svfWrite(uint32_t off, uint32_t v) {
  const uint32_t page = 0x200000 + (((off >> 2) & 0x1FF) << 11);
  const uint32_t mode = off & 0xE000;
  if (mode == 0x2000) { svfColor_ = v; return; }
  if (mode != 0x0000 && mode != 0x4000) return;
  for (int i = 0; i < 512; i++) {
    const uint32_t a = page + uint32_t(i) * 4;
    const uint32_t src = mode == 0x0000 ? sport_[i] : svfColor_;
    const uint32_t w = (madam.m32(a) & ~v) | (src & v);
    mem[a] = uint8_t(w >> 24); mem[a + 1] = uint8_t(w >> 16); mem[a + 2] = uint8_t(w >> 8); mem[a + 3] = uint8_t(w);
  }
  stall_ += 32;
}

uint32_t System::clioRead(uint32_t off) {
  off &= 0xFFFC;
  if (off >= 0x0100 && off < 0x0180) {
    const int n = int((off - 0x100) >> 3);
    return (off & 4) ? reload_[n] : timers_[n];
  }
  // EO do DSP: duas palavras por leitura em 0x3800, uma em 0x3C00 (endereços 0x300-0x3FF do DSP)
  if (off >= 0x3800 && off < 0x3A00) {
    const uint16_t i = uint16_t(0x300 + ((off - 0x3800) >> 1));
    return uint32_t(dsp.read(i)) << 16 | dsp.read(uint16_t(i + 1));
  }
  if (off >= 0x3C00 && off < 0x4000) return dsp.read(uint16_t(0x300 + ((off - 0x3C00) >> 2)));
  if (off >= 0xC000 && off < 0xC010) {  // UNCLE (barramento da expansão de vídeo)
    if (off == 0xC000) return 0x03800000;
    return off == 0xC008 ? uncleAddr_ : 0;
  }
  if (off >= 0x0380 && off < 0x0400) return dsp.fifoDepth(int((off - 0x0380) >> 2));
  const bool unit = xbusSel_ == 0 || xbusSel_ == 0x8F;  // só o CD e o detector de "aparelho novo" respondem
  if (off >= 0x0540 && off < 0x0600 && !unit) { xbusTimeout(); return 0xFF; }
  if (off >= 0x0540 && off < 0x0580) {
    if (xbusSel_ != 0) return xbusPollEn_ & 0xF;
    uint32_t s = xbusPollEn_ & 0xF;
    if (cd.statusReady()) s |= 0x10;
    if (cd.dataReady()) s |= 0x20;
    if (cd.canCommand()) s |= 0x40;
    if (xbusMedia_) s |= 0x80;
    return s;
  }
  if (off >= 0x0580 && off < 0x05C0) return xbusSel_ == 0 ? cd.readStatus() : 0xFF;
  if (off >= 0x05C0 && off < 0x0600) return xbusSel_ == 0 ? cd.readData() : 0xFF;
  if (off >= 0x0500 && off < 0x0540) return xbusSel_;
  switch (off) {
    case 0x0000: return 0x02020000;
    case 0x0004: return csysbits_;
    case 0x0008: return vint0_;
    case 0x000C: return vint1_;
    case 0x0020: return audin_;
    case 0x0024: return audout_;
    case 0x0028: return cstat_;
    case 0x002C: return wdog_;
    case 0x0030: return uint32_t(std::clamp(CYCLES_PER_LINE - budget_, 0, CYCLES_PER_LINE - 1));
    case 0x0034: return uint32_t(line_) | uint32_t(field_) << 11;
    case 0x0038: return seed_ & 0x0FFF0FFF;
    case 0x003C:
      seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
      return seed_;
    case 0x0040: case 0x0044: return pend0_;
    case 0x0048: case 0x004C: return mask0_ | 0x80000000u;
    case 0x0050: case 0x0054: return mode_;
    case 0x0058: return badbits_;
    case 0x0060: case 0x0064: return pend1_;
    case 0x0068: case 0x006C: return mask1_;
    case 0x0080: return hdelay_;
    case 0x0084: return adbio_;
    case 0x0088: return adbctl_;
    case 0x0200: case 0x0204: return timerCtl_[0];
    case 0x0208: case 0x020C: return timerCtl_[1];
    case 0x0220: return slack_;
    case 0x0304: case 0x0308: return dmaEnable_;
    case 0x0400: case 0x0404: return expctl_;
    case 0x040C: return xfrcnt_;
    case 0x0410: return dipir1_;
    case 0x0414: return dipir2_;
    case 0x17D0: return uint32_t(dsp.semStatus) << 16 | dsp.semData;
    case 0x17F0: return dsp.read(0x0EA);  // ruído
    case 0x17F4: return dsp.pc;
    default: break;
  }
  if (off < 0x0100) return clioOther_[off >> 2];
  return 0;
}

void System::clioWrite(uint32_t off, uint32_t v) {
  off &= 0xFFFC;
  if (off >= 0x0100 && off < 0x0180) {
    const int n = int((off - 0x100) >> 3);
    if (off & 4) reload_[n] = uint16_t(v);
    else timers_[n] = uint16_t(v);
    return;
  }
  // programa do DSP: duas instruções por escrita em 0x1800, uma em 0x2000
  if (off >= 0x1800 && off < 0x2000) {
    const uint32_t i = (off - 0x1800) >> 1;
    dsp.code[i] = uint16_t(v >> 16);
    dsp.code[i + 1] = uint16_t(v);
    return;
  }
  if (off >= 0x2000 && off < 0x3000) { dsp.code[(off - 0x2000) >> 2] = uint16_t(v); return; }
  // EI do DSP: duas palavras por escrita em 0x3000, uma em 0x3400
  if (off >= 0x3000 && off < 0x3200) {
    const uint16_t i = uint16_t((off - 0x3000) >> 1);
    dsp.write(i, uint16_t(v >> 16));
    dsp.write(uint16_t(i + 1), uint16_t(v));
    return;
  }
  if (off >= 0x3400 && off < 0x3800) { dsp.write(uint16_t((off - 0x3400) >> 2), uint16_t(v)); return; }
  const bool unit = xbusSel_ == 0 || xbusSel_ == 0x8F;
  if (off >= 0x0500 && off < 0x0540) { xbusSel_ = v & 0xFF; return; }
  if (off >= 0x0540 && off < 0x0600 && !unit) { xbusTimeout(); return; }
  if (off == 0xC008) { uncleAddr_ = v; return; }
  if (off >= 0x0540 && off < 0x0580) {
    xbusPollEn_ = v & 0xF;
    if (xbusSel_ == 0 && (v & 0x80)) xbusMedia_ = 0;  // a troca de disco é apagada ao escrever o bit 7
    return;
  }
  if (off >= 0x0580 && off < 0x05C0) { if (xbusSel_ == 0) cd.writeCommand(uint8_t(v)); return; }
  if (off >= 0x05C0 && off < 0x0600) return;
  switch (off) {
    case 0x0004: csysbits_ = v; return;
    case 0x0008: vint0_ = v; return;
    case 0x000C: vint1_ = v; return;
    case 0x0020: audin_ = v; return;
    case 0x0024: audout_ = v; return;
    case 0x0028:
      if (v & 0x10) {  // reinício "macio" da CPU (a BIOS volta pelo vetor de reset, com a ROM por cima)
        cstat_ = 0x40;
        dipir1_ = (dipir1_ & 0x8000) ? ((dipir1_ & 0xFF) | 0x4000) : 0;
        dipir2_ = (dipir2_ & 0x8000) ? ((dipir2_ & 0xFF) | 0x4000) : 0;
        pend1_ &= ~2u;
        updateFiq();
        cpu.requestReset();
        overlay_ = true;
      } else if (v & 0x20) {  // apaga o pedido do DIPIR
        dipir1_ &= ~0x8000u;
        dipir2_ &= ~0x8000u;
        pend1_ &= ~2u;
        updateFiq();
      } else {
        cstat_ = v;
      }
      return;
    case 0x002C: wdog_ = v; return;
    case 0x0038: seed_ = (v & 0x0FFF0FFF) ? (v & 0x0FFF0FFF) : 1; return;
    case 0x0040: pend0_ |= v & 0x7FFFFFFFu; updateFiq(); return;
    case 0x0044: pend0_ &= ~v; updateFiq(); return;
    case 0x0048: mask0_ |= v & 0x7FFFFFFFu; updateFiq(); return;
    case 0x004C: mask0_ &= ~v; updateFiq(); return;
    case 0x0050: mode_ |= v; return;
    case 0x0054: mode_ &= ~v; return;
    case 0x0058: badbits_ = v; return;
    case 0x0060: pend1_ |= v; updateFiq(); return;
    case 0x0064: pend1_ &= ~v; updateFiq(); return;
    case 0x0068: mask1_ |= v; updateFiq(); return;
    case 0x006C: mask1_ &= ~v; updateFiq(); return;
    case 0x0080: hdelay_ = v; return;
    case 0x0084: adbio_ = v & 0xFF; return;
    case 0x0088: adbctl_ = v; return;
    case 0x0200: timerCtl_[0] |= v; return;
    case 0x0204: timerCtl_[0] &= ~v; return;
    case 0x0208: timerCtl_[1] |= v; return;
    case 0x020C: timerCtl_[1] &= ~v; return;
    case 0x0220: slack_ = v & 0x7FF; return;
    case 0x0300: dsp.hostFifoInit(v); return;
    case 0x0304: dmaEnable_ |= v; dsp.hostEnable(v, 0); expBus(); return;
    case 0x0308: dmaEnable_ &= ~v; dsp.hostEnable(0, v); expBus(); return;
    case 0x0400: expctl_ |= v & 0xCA80; expBus(); return;
    case 0x0404: expctl_ &= ~(v & 0xCA80); expBus(); return;
    case 0x0408: type04_ = v; return;
    case 0x040C: xfrcnt_ = v; return;
    case 0x17D0: dsp.hostSemaphore(uint16_t(v)); return;
    case 0x17D4: dsp.hostSemaphoreAck(); return;
    case 0x17E4: case 0x17E8: dsp.pc = 0; dsp.sleeping = false; return;  // recomeça a amostra
    case 0x17FC:  // bit 0 = GW (roda), bit 2 = reinicia o DSP
      if (v & 4) dsp.reset();
      dsp.running = v & 1;
      return;
    default: break;
  }
  if (off < 0x0100) clioOther_[off >> 2] = v;
}

// um passo a cada 64 + SLACK ciclos de 25 MHz; o timer encadeado só anda quando o anterior (ligado) estoura
void System::timersTick(int cycles) {
  const uint32_t period = 64 + slack_;
  timerFrac_ += uint32_t(cycles) * 2;
  while (timerFrac_ >= period) {
    timerFrac_ -= period;
    bool carry = true;
    for (int t = 0; t < 16; t++) {
      uint32_t& ctlWord = timerCtl_[t >> 3];
      const int sh = (t & 7) * 4;
      const uint32_t ctl = (ctlWord >> sh) & 0xF;
      if (!(ctl & 1)) continue;
      const bool step = (ctl & 4) ? carry : true;
      if (step && timers_[t] == 0) {
        if (t & 1) pend0_ |= 1u << (10 - (t >> 1));
        carry = true;
        if (ctl & 2) timers_[t] = reload_[t];
        else { timers_[t] = 0xFFFF; ctlWord &= ~(1u << sh); }  // sem recarga: para
      } else {
        if (step) timers_[t]--;
        carry = false;
      }
    }
  }
  updateFiq();
}

// sem DMA do XBUS pedido (bit 20 do DMA e bit 11 do EXPCTL) o barramento volta para a CPU: o bit 7 do EXPCTL
// é o estado "a CPU tem o barramento" e a BIOS real o apaga e espera ele voltar
void System::expBus() {
  if ((dmaEnable_ & DMA_XBUS) && (expctl_ & 0x800)) return;
  expctl_ = (expctl_ | 0x80) & ~0x400u;
}

// DMA do XBUS (canal 20 do MADAM, 0x540): contagem em bytes menos 4, termina quando fica negativa
void System::xbusDma() {
  if (!(dmaEnable_ & DMA_XBUS) || !(expctl_ & 0x800)) return;
  uint32_t& addr = madam.regs[0x540 >> 2];
  uint32_t& count = madam.regs[0x544 >> 2];
  expctl_ = (expctl_ & ~0x80u) | 0x400;  // o DMA tem o barramento
  int moved = 0;
  while (!(count & 0x80000000u) && cd.dataSize() >= 4 && moved < 4096) {
    for (int i = 0; i < 4; i++) {
      const uint8_t b = cd.readData();
      if (addr + uint32_t(i) < MEM_SIZE) mem[addr + uint32_t(i)] = b;
    }
    addr += 4;
    count -= 4;
    moved += 4;
  }
  if (count & 0x80000000u) {
    dmaEnable_ &= ~DMA_XBUS;
    expctl_ = (expctl_ | 0x80) & ~0x400u;
    raise0(IRQ_XBUS_DMA);
  }
}

// DMA do controle (canal 23 do MADAM, 0x570): começa com o bit 15 do MCTL. O que os controles devolvem entra a
// partir da 2ª palavra da origem (0x578) e a origem inteira vai para o destino (0x570); contagem em bytes menos 4.
// Palavra dos controles: controle 1 nos bytes 0-1 e o 2 (se ligado) nos bytes 2-3; depois o barramento fica em 1
// (fim da corrente).
void System::pbusDma() {
  if (!(madam.regs[0x008 >> 2] & 0x8000)) return;
  uint32_t* r = &madam.regs[0x570 >> 2];
  const uint32_t words[3] = {uint32_t(pad_) << 16 | (pad2On_ ? pad2_ : 0xFFFFu), 0xFFFFFFFFu, 0xFFFFFFFFu};
  for (int i = 0; i < 3; i++) {
    const uint32_t a = r[2] + uint32_t(i + 1) * 4;
    // (a + 4 em 32 bits daria a volta com a perto de 4 GB)
    if (a <= MEM_SIZE - 4) for (int k = 0; k < 4; k++) mem[a + uint32_t(k)] = uint8_t(words[i] >> (24 - 8 * k));
  }
  for (int n = 0; n < 64 && !(r[1] & 0x80000000u); n++) {
    if (r[0] <= MEM_SIZE - 4 && r[2] <= MEM_SIZE - 4) std::memmove(&mem[r[0]], &mem[r[2]], 4);
    r[0] += 4; r[2] += 4; r[1] -= 4;
  }
  r[1] |= 0x80000000u;
  madam.regs[0x008 >> 2] &= ~0x8000u;
  raise1(1);
}

void System::tickHardware(int cycles) {
  timersTick(cycles);
  cd.tick(cycles);
  xbusDma();
  pbusDma();
  // XBUS: interrupção enquanto a unidade tiver resposta ou dado e a interrupção estiver ligada
  if (xbusSel_ == 0 && (((xbusPollEn_ & 1) && cd.statusReady()) || ((xbusPollEn_ & 2) && cd.dataReady()))) pend0_ |= IRQ_XBUS;
  audioAcc_ += uint64_t(cycles) * AUDIO_HZ;
  while (audioAcc_ >= uint64_t(CPU_HZ)) {
    audioAcc_ -= uint64_t(CPU_HZ);
    dsp.runFrame();
    int16_t cl, cr;
    cd.audioSample(cl, cr);  // áudio de CD somado ao do DSP, com saturação
    const int16_t l = int16_t(std::clamp(dsp.outL + cl, -32768, 32767)), r = int16_t(std::clamp(dsp.outR + cr, -32768, 32767));
    if (audio.size() < 44100 * 4) { audio.push_back(l); audio.push_back(r); }
  }
  updateFiq();
}

void System::runLine() {
  if (uint32_t(line_) == vint0_) raise0(IRQ_VINT0);
  if (uint32_t(line_) == vint1_) raise0(IRQ_VINT1);
  budget_ += CYCLES_PER_LINE;
  while (budget_ > 0) {
    if (stall_ > 0) {
      const int s = std::min<int32_t>(stall_, budget_);
      stall_ -= s;
      budget_ -= s;
      continue;
    }
    budget_ -= cpu.step();
  }
  tickHardware(CYCLES_PER_LINE);
  if (line_ == VISIBLE) madam.renderFrame(fb);
  if (++line_ >= LINES) { line_ = 0; field_ ^= 1; }
}

void System::runFrame() {
  for (int i = 0; i < LINES; i++) runLine();
  frames++;
}

void System::sync(StateIO& io) {
  cpu.sync(io);
  io.raw(mem.data(), mem.size());
  io.raw(nvram.data(), nvram.size());
  madam.sync(io);
  dsp.sync(io);
  cd.sync(io);
  io.sync(pend0_); io.sync(pend1_); io.sync(mask0_); io.sync(mask1_);
  io.sync(vint0_); io.sync(vint1_);
  io.raw(timers_, sizeof timers_);
  io.raw(reload_, sizeof reload_);
  io.raw(timerCtl_, sizeof timerCtl_);
  io.sync(slack_); io.sync(timerFrac_);
  io.sync(dmaEnable_);
  io.sync(xbusSel_); io.sync(xbusPollEn_); io.sync(expctl_); io.sync(xbusMedia_);
  io.sync(seed_);
  io.sync(csysbits_); io.sync(audin_); io.sync(audout_); io.sync(cstat_); io.sync(wdog_); io.sync(mode_);
  io.sync(badbits_); io.sync(hdelay_); io.sync(adbio_); io.sync(adbctl_); io.sync(type04_); io.sync(xfrcnt_);
  io.sync(dipir1_); io.sync(dipir2_);
  io.raw(clioOther_, sizeof clioOther_);
  io.sync(overlay_);
  io.raw(sport_, sizeof sport_);
  io.sync(svfColor_); io.sync(uncleAddr_); io.sync(slowIn_); io.sync(slowOut_);
  io.sync(line_); io.sync(field_); io.sync(budget_); io.sync(stall_);
  io.sync(audioAcc_);
  io.sync(pad_);
  io.sync(frames);
  if (io.reading()) {
    slack_ &= 0x7FF;
    if (timerFrac_ >= 64 + slack_) timerFrac_ = 0;
    xbusPollEn_ &= 0xF;
    xbusMedia_ &= 1;
    adbio_ &= 0xFF;
    memoryConfig(madam.regs[1]);
    if (!seed_) seed_ = 1;
    if (line_ < 0 || line_ >= LINES) line_ = 0;
    field_ &= 1;
    clampTo(budget_, int32_t(-1000), int32_t(CYCLES_PER_LINE));
    clampTo(stall_, int32_t(0), int32_t(1) << 26);
    if (audioAcc_ >= uint64_t(CPU_HZ)) audioAcc_ = 0;
    audio.clear();
    updateFiq();
  }
}

// ------------------------------------------------------------------------------------------------ núcleo

namespace {
std::string volumeName(cdrom::Disc& d) {
  uint8_t buf[cdrom::DATA_SECTOR];
  int first = -1;
  for (size_t i = 0; i < d.tracks().size(); i++)
    if (d.tracks()[i].type != cdrom::TrackType::Audio) { first = int(i); break; }
  if (first >= 0 && d.readData(d.tracks()[size_t(first)].start, buf)) {
    std::string s(reinterpret_cast<const char*>(buf + 8), 32);
    s = s.substr(0, s.find('\0'));
    bool ok = !s.empty();
    for (char c : s) if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) ok = false;
    if (ok) return s;
  }
  return d.name();
}
}  // namespace

TdoCore::TdoCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios)
    : disc_(std::move(disc)) {
  const std::vector<uint8_t>& b = bios ? *bios : firmware("3do");
  if (b.empty()) throw std::runtime_error("o 3DO precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo panafz10.bin) uma vez no Índigo.");
  sys_ = std::make_unique<System>(b, disc_.get());
  if (save) importSave(*save);
  title_ = disc_ ? volumeName(*disc_) : "3DO";
  note_ = contains(b, "Indigo 3DO") ? " · BIOS de teste do Índigo" : " · 3DO: ainda não testado com jogos; se algo der errado, avise";
}

namespace {
uint16_t padBits(uint32_t k) {
  uint16_t p = 0x8000;  // identificação do controle
  if (k & KEY_DOWN) p |= 0x1000;
  if (k & KEY_UP) p |= 0x0800;
  if (k & KEY_RIGHT) p |= 0x0400;
  if (k & KEY_LEFT) p |= 0x0200;
  // A, B e C seguem a fileira do controle do portátil: botão da esquerda (Y), de baixo (A) e da direita (B)
  if (k & KEY_Y) p |= 0x0100;      // A
  if (k & KEY_A) p |= 0x0080;      // B
  if (k & KEY_B) p |= 0x0040;      // C
  if (k & KEY_START) p |= 0x0020;  // P (play)
  if (k & KEY_SELECT) p |= 0x0010; // X (stop)
  if (k & KEY_R) p |= 0x0008;
  if (k & KEY_L) p |= 0x0004;
  return p;
}
}  // namespace

void TdoCore::setKeys(uint32_t k) { sys_->setPad(padBits(k)); }
void TdoCore::setKeys2(uint32_t k) { sys_->setPad2(padBits(k)); }

size_t TdoCore::audioAvailable() const {
  const double frames = double(sys_->audio.size() / 2);
  const double n = (frames - 1 - audioPos_) * rate_ / System::AUDIO_HZ;
  return n > 0 ? size_t(n) : 0;
}

void TdoCore::pullAudio(float* l, float* r, size_t n) {
  auto& a = sys_->audio;
  const size_t frames = a.size() / 2;
  const double step = double(System::AUDIO_HZ) / rate_;
  for (size_t i = 0; i < n; i++) {
    const size_t k = size_t(audioPos_);
    if (k + 1 >= frames) { l[i] = r[i] = 0; continue; }
    const double f = audioPos_ - double(k);
    l[i] = float((a[2 * k] * (1 - f) + a[2 * k + 2] * f) / 32768.0);
    r[i] = float((a[2 * k + 1] * (1 - f) + a[2 * k + 3] * f) / 32768.0);
    audioPos_ += step;
  }
  const size_t used = std::min(size_t(audioPos_), frames);
  a.erase(a.begin(), a.begin() + long(used * 2));
  audioPos_ -= double(used);
}

void TdoCore::importSave(const std::vector<uint8_t>& b) {
  std::fill(sys_->nvram.begin(), sys_->nvram.end(), 0);
  std::copy(b.begin(), b.begin() + std::min(b.size(), sys_->nvram.size()), sys_->nvram.begin());
  sys_->nvramDirty = false;
}

// "3DO2": formato da 2.5 (DSPP, SPORT e campos novos de CLIO/MADAM/CD); o "3DO1" da 2.4 é recusado
static constexpr uint32_t STATE_MAGIC = 0x33444F32;

std::vector<uint8_t> TdoCore::saveState() {
  StateIO io;
  uint32_t magic = STATE_MAGIC;
  io.sync(magic);
  sys_->sync(io);
  return io.data();
}

void TdoCore::loadState(const std::vector<uint8_t>& s) {
  StateIO probe(s);
  uint32_t magic = 0;
  probe.sync(magic);
  if (magic == 0x33444F31) throw std::runtime_error("Estado salvo de versão antiga do 3DO (2.4): não serve mais.");
  if (magic != STATE_MAGIC) throw std::runtime_error("Estado salvo não é deste console.");
  // primeiro numa cópia: um estado truncado não pode deixar o jogo pela metade
  const std::vector<uint8_t> before = saveState();
  try {
    sys_->sync(probe);
    if (!probe.atEnd()) throw std::runtime_error("Estado salvo com bytes sobrando.");
  } catch (...) {
    StateIO back(before);
    uint32_t m = 0;
    back.sync(m);
    sys_->sync(back);
    throw;
  }
  audioPos_ -= double(size_t(audioPos_));  // a fila de som recomeça vazia (só a fração do reamostrador fica)
}

std::unique_ptr<TdoCore> createTdoCore(const std::string& path, const std::vector<uint8_t>* save, std::string* err) {
  std::string e;
  auto disc = cdrom::open(path, &e);
  if (!disc) { if (err) *err = e; return nullptr; }
  try {
    return std::make_unique<TdoCore>(std::move(disc), save);
  } catch (const std::exception& ex) {
    if (err) *err = ex.what();
    return nullptr;
  }
}

}  // namespace indigo::tdo
