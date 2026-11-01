#include "md.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::md {

namespace {
const char STATE_MAGIC[4] = {'I', 'M', 'D', '1'};
constexpr int64_t PAD_TIMEOUT = 80000;  // ~1,5 ms em ciclos mestres: o controle de 6 botões volta ao início

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
bool hasSega(const std::vector<uint8_t>& r) {
  if (r.size() < 0x200) return false;
  return (std::memcmp(&r[0x100], "SEGA", 4) == 0) || (std::memcmp(&r[0x101], "SEGA", 4) == 0);
}
uint32_t be32(const std::vector<uint8_t>& r, size_t a) {
  return a + 4 <= r.size() ? (uint32_t(r[a]) << 24) | (r[a + 1] << 16) | (r[a + 2] << 8) | r[a + 3] : 0;
}
std::vector<uint8_t> deinterleave(const std::vector<uint8_t>& d) {
  std::vector<uint8_t> out(d.size() - 512);
  for (size_t b = 0; b + 16384 <= out.size(); b += 16384) {
    const uint8_t* src = &d[512 + b];
    for (int i = 0; i < 8192; i++) {
      out[b + i * 2 + 1] = src[i];
      out[b + i * 2] = src[8192 + i];
    }
  }
  return out;
}
}  // namespace

std::vector<uint8_t> mdNormalize(const std::vector<uint8_t>& data) {
  if (data.size() > 512 && (data.size() & 0x3FFF) == 512 && !hasSega(data)) {
    std::vector<uint8_t> d = deinterleave(data);
    if (hasSega(d) || (data[8] == 0xAA && data[9] == 0xBB)) return d;
  }
  return data;
}

bool isMdRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.size() < 0x200 || rom.size() > (10u << 20) + 512) return false;
  const bool ext = endsWith(n, ".md") || endsWith(n, ".gen") || endsWith(n, ".smd");
  if (ext) return true;
  if (endsWith(n, ".32x")) return false;
  return hasSega(mdNormalize(rom));
}

// ------------------------------------------------------------ console
MD::~MD() = default;

MD::MD(const std::vector<uint8_t>& romIn, const std::vector<uint8_t>* save)
    : cpu(*this), z80(*this), vdp(*this), ram_(0x10000), zram_(0x2000) {
  rom_ = mdNormalize(romIn);
  if (rom_.size() < 0x200 || rom_.size() > (10u << 20)) throw std::runtime_error("ROM do Mega Drive inválida.");
  if (rom_.size() & 1) rom_.push_back(0xFF);
  uint32_t p = 1;
  while (p < rom_.size()) p <<= 1;
  romMask_ = p - 1;
  ssf2_ = rom_.size() > (4u << 20) || std::memcmp(&rom_[0x100], "SEGA SSF", 8) == 0;
  // título (nome internacional, 48 bytes em 0x150)
  for (int i = 0; i < 48; i++) {
    const char c = static_cast<char>(rom_[0x150 + i]);
    title += (c >= 32 && c < 127) ? c : ' ';
  }
  while (!title.empty() && title.back() == ' ') title.pop_back();
  size_t f = title.find_first_not_of(' ');
  title = f == std::string::npos ? "" : title.substr(f);
  {  // espaços repetidos
    std::string t;
    for (char c : title) if (!(c == ' ' && !t.empty() && t.back() == ' ')) t += c;
    title = t;
  }
  // região
  bool jp = false, us = false, eu = false;
  const char r0 = static_cast<char>(rom_[0x1F0]);
  const bool newStyle = ((r0 >= '0' && r0 <= '9') || (r0 >= 'A' && r0 <= 'F' && r0 != 'E')) &&
                        (rom_[0x1F1] == ' ' || rom_[0x1F1] == 0);
  if (newStyle) {
    const int v = r0 <= '9' ? r0 - '0' : r0 - 'A' + 10;
    jp = v & 1; us = v & 4; eu = v & 8;
  } else {
    for (int i = 0; i < 3; i++) {
      const char c = static_cast<char>(rom_[0x1F0 + i]);
      if (c == 'J') jp = true;
      if (c == 'U') us = true;
      if (c == 'E') eu = true;
    }
  }
  if (us || (!jp && !eu)) { overseas = true; pal = false; }
  else if (jp) { overseas = false; pal = false; }
  else { overseas = true; pal = true; }
  // SRAM pelo cabeçalho
  if (rom_[0x1B0] == 'R' && rom_[0x1B1] == 'A') {
    uint32_t s = be32(rom_, 0x1B4), e = be32(rom_, 0x1B8);
    if (s < 0x200000 || s > 0x3FFFFF || e < s || e - s > 0xFFFF) { s = 0x200001; e = 0x20FFFF; }
    sramStart_ = s & ~1u;
    sramEnd_ = e | 1u;
    sram_.assign(sramEnd_ - sramStart_ + 1, 0xFF);
  }
  if (save) importSave(*save);
  reset();
}

void MD::reset() {
  std::fill(ram_.begin(), ram_.end(), 0);
  std::fill(zram_.begin(), zram_.end(), 0);
  vdp.reset(pal);
  ym.reset();
  psg.reset();
  for (int i = 0; i < 8; i++) bank_[i] = static_cast<uint8_t>(i);
  sramCtl_ = 0;
  mclk_ = lineStart_ = refreshAt_ = ymBusyUntil_ = m68kStall_ = 0;
  // ao ligar, o 68000 começa num ponto fixo do quadro (o quadro emulado começa na linha 192 do modo 4):
  // medido no aparelho, o primeiro HV lido é 0x9F21 (NTSC) / 0x8421 (PAL); sem interrupção de quadro no 1º
  m68Mclk_ = ((static_cast<int64_t>(vdp.linesPerFrame()) - 192 + 159 - (pal ? 27 : 0)) * VDP::MCLK_PER_LINE + 1004) / 7 * 7;
  z80Mclk_ = m68Mclk_ / 15 * 15;
  firstFrame_ = true;
  z80Reset_ = true; z80BusReq_ = false; z80Bank_ = 0;
  z80.reset();
  for (int i = 0; i < 3; i++) { ioData_[i] = 0x7F; ioCtrl_[i] = 0; }
  padPhase_[0] = padPhase_[1] = 0; padLastTh_[0] = padLastTh_[1] = 0;
  ymL_.clear(); ymR_.clear(); psgL_.clear(); psgR_.clear(); outW_ = outR0_ = 0; psgLpL_ = psgLpR_ = 0;
  frame = 0;
  cpu.reset();
}

void MD::importSave(const std::vector<uint8_t>& b) {
  if (sram_.empty() || b.empty()) return;
  std::copy(b.begin(), b.begin() + std::min(b.size(), sram_.size()), sram_.begin());
}

// ------------------------------------------------------------ memória do 68000
uint8_t MD::romByte(uint32_t a) const {
  if (ssf2_) {
    const uint32_t off = (static_cast<uint32_t>(bank_[(a >> 19) & 7]) << 19) | (a & 0x7FFFF);
    return off < rom_.size() ? rom_[off] : 0xFF;
  }
  const uint32_t off = a & romMask_;
  return off < rom_.size() ? rom_[off] : 0xFF;
}

bool MD::sramMapped(uint32_t a) const {
  if (sram_.empty() || a < sramStart_ || a > sramEnd_) return false;
  if (rom_.size() > 0x200000) return (sramCtl_ & 1) != 0;
  return true;
}

uint8_t MD::read8(uint32_t a) {
  if (ext_ && extArea(a)) return ext_->read8(a);
  if (ext) { uint16_t x; if (ext->busRead(a, 1, x)) return static_cast<uint8_t>(x); }
  if (a < 0x400000) {
    if (sramMapped(a)) return sram_[a - sramStart_];
    return romByte(a);
  }
  if (a >= 0xE00000) return ram_[a & 0xFFFF];
  if (a >= 0xC00000) {
    if (a > 0xDFFFFF || (a & 0xE700E0) != 0xC00000) return 0xFF;
    const uint16_t v = vdpRead(a & ~1u);
    return static_cast<uint8_t>((a & 1) ? v : v >> 8);
  }
  if (a >= 0xA00000 && a < 0xA10000) return z80AreaRead(a);
  if (a >= 0xA10000 && a < 0xA10020) return ioRead((a >> 1) & 0xF);
  if ((a & 0xFFFF00) == 0xA11100 && !(a & 1)) {
    syncZ80(now());
    return static_cast<uint8_t>(0xFE | (z80BusReq_ ? 0 : 1));
  }
  if ((a & 0xFFFF00) == 0xA11100) return 0xFF;
  return 0xFF;
}

uint16_t MD::read16(uint32_t a) {
  if (ext_ && extArea(a)) return ext_->read16(a);
  if (ext) { uint16_t x; if (ext->busRead(a, 2, x)) return x; }
  if (a < 0x400000) {
    if (sramMapped(a)) return static_cast<uint16_t>((sram_[a - sramStart_] << 8) | sram_[a + 1 - sramStart_]);
    return static_cast<uint16_t>((romByte(a) << 8) | romByte(a + 1));
  }
  if (a >= 0xE00000) { const uint32_t o = a & 0xFFFF; return static_cast<uint16_t>((ram_[o] << 8) | ram_[o + 1]); }
  if (a >= 0xC00000) {
    if (a > 0xDFFFFF || (a & 0xE700E0) != 0xC00000) return 0xFFFF;
    return vdpRead(a);
  }
  if (a >= 0xA00000 && a < 0xA10000) { const uint8_t v = z80AreaRead(a); return static_cast<uint16_t>((v << 8) | v); }
  if (a >= 0xA10000 && a < 0xA10020) { const uint8_t v = ioRead((a >> 1) & 0xF); return static_cast<uint16_t>((v << 8) | v); }
  if ((a & 0xFFFF00) == 0xA11100) {
    syncZ80(now());
    return static_cast<uint16_t>(0xFEFF | (z80BusReq_ ? 0 : 0x100));
  }
  return 0xFFFF;
}

void MD::write8(uint32_t a, uint8_t v) {
  if (ext_ && extArea(a)) { ext_->write8(a, v); return; }
  if (ext && ext->busWrite(a, 1, v)) return;
  if (a >= 0xE00000) { ram_[a & 0xFFFF] = v; return; }
  if (a < 0x400000) {
    if (sramMapped(a) && !(sramCtl_ & 2)) {
      uint8_t& s = sram_[a - sramStart_];
      if (s != v) { s = v; dirty_ = true; }
    }
    return;
  }
  if (a >= 0xC00000) {
    if (a > 0xDFFFFF || (a & 0xE700E0) != 0xC00000) return;
    vdpWrite(a, static_cast<uint16_t>((v << 8) | v), true);
    return;
  }
  if (a >= 0xA00000 && a < 0xA10000) { z80AreaWrite(a, v); return; }
  if (a >= 0xA10000 && a < 0xA10020) { ioWrite((a >> 1) & 0xF, v); return; }
  if ((a & 0xFFFF00) == 0xA11100) {
    if (a & 1) return;
    syncZ80(now());
    z80BusReq_ = (v & 1) != 0;
    return;
  }
  if ((a & 0xFFFF00) == 0xA11200) {
    if (a & 1) return;
    syncZ80(now());
    const bool rst = !(v & 1);
    if (rst && !z80Reset_) { z80.reset(); ym.run(now() / 7); ym.reset(); }
    z80Reset_ = rst;
    return;
  }
  if (a >= 0xA130F0 && a <= 0xA130FF && (a & 1)) {
    const int r = (a >> 1) & 7;
    if (r == 0) sramCtl_ = v & 3;
    else if (ssf2_) bank_[r] = v & 0x3F;
    return;
  }
}

void MD::write16(uint32_t a, uint16_t v) {
  if (ext_ && extArea(a)) { ext_->write16(a, v); return; }
  if (ext && ext->busWrite(a, 2, v)) return;
  if (a >= 0xE00000) { const uint32_t o = a & 0xFFFF; ram_[o] = static_cast<uint8_t>(v >> 8); ram_[o + 1] = static_cast<uint8_t>(v); return; }
  if (a >= 0xC00000) {
    if (a > 0xDFFFFF || (a & 0xE700E0) != 0xC00000) return;
    vdpWrite(a, v, false);
    return;
  }
  if (a < 0x400000) {
    write8(a, static_cast<uint8_t>(v >> 8));
    write8(a + 1, static_cast<uint8_t>(v));
    return;
  }
  if (a >= 0xA00000 && a < 0xA10000) { z80AreaWrite(a, static_cast<uint8_t>(v >> 8)); return; }
  if ((a & 0xFFFF00) == 0xA11100 || (a & 0xFFFF00) == 0xA11200) { write8(a, static_cast<uint8_t>(v >> 8)); return; }
  write8(a + 1, static_cast<uint8_t>(v));
}

uint16_t MD::dmaRead(uint32_t a) {
  a &= 0xFFFFFE;
  if (ext_ && extArea(a)) return ext_->dmaRead(a);
  if (ext) { uint16_t x; if (ext->busRead(a, 2, x)) return x; }
  if (a < 0x400000) return static_cast<uint16_t>((romByte(a) << 8) | romByte(a + 1));
  if (a >= 0xE00000) { const uint32_t o = a & 0xFFFF; return static_cast<uint16_t>((ram_[o] << 8) | ram_[o + 1]); }
  return 0xFFFF;
}

// ------------------------------------------------------------ VDP e PSG vistos pelo 68000
uint16_t MD::vdpRead(uint32_t a) {
  switch (a & 0x1C) {
    case 0x00: return vdp.readData();
    case 0x04: {
      const int64_t t = now();
      const uint16_t s = vdp.readStatus(t, static_cast<int>(t - lineStart_));
      return static_cast<uint16_t>((s & 0x3FF) | (cpu.irc & 0xFC00));
    }
    case 0x08: case 0x0C: {
      int m = static_cast<int>(now() - lineStart_);
      if (m < 0) m = 0;
      if (m >= VDP::MCLK_PER_LINE) m = VDP::MCLK_PER_LINE - 1;
      return vdp.hvCounter(m);
    }
    default: return 0xFFFF;
  }
}

void MD::vdpWrite(uint32_t a, uint16_t v, bool byte) {
  switch (a & 0x1C) {
    case 0x00: vdp.writeData(v, now()); break;
    case 0x04: vdp.writeCtrl(v, now()); break;
    case 0x10: case 0x14:
      if (byte ? (a & 1) : true) { psg.run(now() / 15); psg.write(v & 0xFF); }
      break;
    default: break;
  }
}

// ------------------------------------------------------------ área do Z80 vista pelo 68000
uint8_t MD::ymStatus(uint32_t a, int64_t t) {
  // YM2612 separado (modelo 1): o estado só sai no endereço 0; bit 7 = ocupado por 32 ciclos do chip após cada dado
  if (a & 3) return 0;
  ym.run(t / 7);
  return static_cast<uint8_t>(ym.status() | (t < ymBusyUntil_ ? 0x80 : 0));
}

void MD::ymWrite(uint32_t a, uint8_t v, int64_t t) {
  ym.run(t / 7);
  if (a & 1) ymBusyUntil_ = ((t + 41) / 42 + 32) * 42;
  ym.write(a & 3, v);
}

uint8_t MD::z80AreaRead(uint32_t a) {
  cpu.cycles += 1;  // acesso ao barramento do Z80 custa 1 ciclo a mais
  syncZ80(now());
  a &= 0x7FFF;
  if (a < 0x4000) return zram_[a & 0x1FFF];
  if (a < 0x6000) return ymStatus(a, now());
  return 0xFF;
}

void MD::z80AreaWrite(uint32_t a, uint8_t v) {
  cpu.cycles += 1;
  syncZ80(now());
  a &= 0x7FFF;
  if (a < 0x4000) { zram_[a & 0x1FFF] = v; return; }
  if (a < 0x6000) { ymWrite(a, v, now()); return; }
  if (a < 0x6100) { z80Bank_ = ((z80Bank_ >> 1) | ((v & 1) << 8)) & 0x1FF; return; }
  if (a >= 0x7F00 && a < 0x7F20) {
    if ((a & 0x1C) == 0x10 || (a & 0x1C) == 0x14) { psg.run(now() / 15); psg.write(v); }
  }
}

// ------------------------------------------------------------ memória do Z80
// o Z80 usando o barramento do 68000 (janela de 32 KB): o 68000 espera ~10 ciclos e o Z80 ~3
void MD::z80BusAccess() {
  m68kStall_ += ((z80Mclk_ % 7) + 72) / 7 * 7;
  z80Mclk_ += 3 * 15;
}

int MD::read(int a) {
  if (a < 0x4000) return zram_[a & 0x1FFF];
  if (a < 0x6000) return ymStatus(static_cast<uint32_t>(a), z80Mclk_);
  if (a < 0x8000) {
    if (a >= 0x7F00 && a < 0x7F20) {
      if ((a & 0x1C) == 0x08 || (a & 0x1C) == 0x0C) {
        int m = static_cast<int>(z80Mclk_ - lineStart_);
        m = std::clamp(m, 0, VDP::MCLK_PER_LINE - 1);
        const uint16_t hv = vdp.hvCounter(m);
        return (a & 1) ? (hv & 0xFF) : (hv >> 8);
      }
    }
    return 0xFF;
  }
  const uint32_t ad = (static_cast<uint32_t>(z80Bank_) << 15) | static_cast<uint32_t>(a & 0x7FFF);
  z80BusAccess();
  if (ext_ && extArea(ad)) return ext_->read8(ad);
  if (ext) { uint16_t x; if (ext->busRead(ad, 1, x)) return x & 0xFF; }
  if (ad < 0x400000) return sramMapped(ad) ? sram_[ad - sramStart_] : romByte(ad);
  if (ad >= 0xE00000) return ram_[ad & 0xFFFF];
  return 0xFF;
}

void MD::write(int a, int v) {
  if (a < 0x4000) { zram_[a & 0x1FFF] = static_cast<uint8_t>(v); return; }
  if (a < 0x6000) { ymWrite(static_cast<uint32_t>(a), static_cast<uint8_t>(v), z80Mclk_); return; }
  if (a < 0x6100) { z80Bank_ = ((z80Bank_ >> 1) | ((v & 1) << 8)) & 0x1FF; return; }
  if (a < 0x8000) {
    if (a >= 0x7F00 && a < 0x7F20 && ((a & 0x1C) == 0x10 || (a & 0x1C) == 0x14)) {
      psg.run(z80Mclk_ / 15);
      psg.write(v & 0xFF);
    }
    return;
  }
  const uint32_t ad = (static_cast<uint32_t>(z80Bank_) << 15) | static_cast<uint32_t>(a & 0x7FFF);
  z80BusAccess();
  if (ext_ && extArea(ad)) { ext_->write8(ad, static_cast<uint8_t>(v)); return; }
  if (ext && ext->busWrite(ad, 1, static_cast<uint8_t>(v))) return;
  if ((ad & 0xFFFFF9) == 0xC00011) { psg.run(z80Mclk_ / 15); psg.write(v & 0xFF); return; }  // PSG pela janela
  if (ad >= 0xE00000) ram_[ad & 0xFFFF] = static_cast<uint8_t>(v);
  else if (ad < 0x400000 && sramMapped(ad) && !(sramCtl_ & 2)) { sram_[ad - sramStart_] = static_cast<uint8_t>(v); dirty_ = true; }
}

// ------------------------------------------------------------ E/S e controle
uint8_t MD::padRead(int port) {
  port &= 1;
  const bool th = (ioCtrl_[port] & 0x40) ? (ioData_[port] & 0x40) != 0 : true;
  if (now() - padLastTh_[port] > PAD_TIMEOUT) padPhase_[port] = 0;
  if (port == 1) padPhase_[1] = 0;  // controle 2: 3 botões, nada apertado
  const uint32_t k = port == 0 ? keys_ : 0;  // controle 2 ligado, sem nada apertado
  auto bit = [&](uint32_t key, int b) { return (k & key) ? 0 : (1 << b); };
  uint8_t v;
  if (th) {
    if (padPhase_[port] == 6) {  // ?1CBMXYZ
      v = static_cast<uint8_t>(0x40 | bit(KEY_B, 5) | bit(KEY_A, 4) | bit(KEY_SELECT, 3) | bit(KEY_L, 2) |
                               bit(KEY_X, 1) | bit(KEY_R, 0));
    } else {  // ?1CBRLDU
      v = static_cast<uint8_t>(0x40 | bit(KEY_B, 5) | bit(KEY_A, 4) | bit(KEY_RIGHT, 3) | bit(KEY_LEFT, 2) |
                               bit(KEY_DOWN, 1) | bit(KEY_UP, 0));
    }
  } else {
    if (padPhase_[port] == 5) v = static_cast<uint8_t>(bit(KEY_START, 5) | bit(KEY_Y, 4));  // ?0SA0000
    else if (padPhase_[port] == 7) v = static_cast<uint8_t>(bit(KEY_START, 5) | bit(KEY_Y, 4) | 0x0F);
    else v = static_cast<uint8_t>(bit(KEY_START, 5) | bit(KEY_Y, 4) | bit(KEY_DOWN, 1) | bit(KEY_UP, 0));
  }
  return v;
}

uint8_t MD::ioRead(int r) {
  switch (r) {
    case 0: return static_cast<uint8_t>((overseas ? 0x80 : 0) | (pal ? 0x40 : 0) | (ext_ && ext_->diskUnit() ? 0 : 0x20));
    case 1: case 2: {
      const int p = r - 1;
      const uint8_t in = padRead(p);
      return static_cast<uint8_t>((ioData_[p] & (ioCtrl_[p] | 0x80)) | (in & ~ioCtrl_[p] & 0x7F));
    }
    case 3: return static_cast<uint8_t>((ioData_[2] & (ioCtrl_[2] | 0x80)) | (0x7F & ~ioCtrl_[2]));
    case 4: case 5: case 6: return ioCtrl_[r - 4];
    case 7: case 10: case 13: return 0xFF;
    default: return 0;
  }
}

void MD::ioWrite(int r, uint8_t v) {
  if (r >= 1 && r <= 3) {
    if (r <= 2) {
      const int p = r - 1;
      const bool oldTh = (ioCtrl_[p] & 0x40) ? (ioData_[p] & 0x40) != 0 : true;
      const bool newTh = (ioCtrl_[p] & 0x40) ? (v & 0x40) != 0 : true;
      if (oldTh != newTh) {
        if (now() - padLastTh_[p] > PAD_TIMEOUT) padPhase_[p] = 0;
        padPhase_[p] = (padPhase_[p] + 1) & 7;
        padLastTh_[p] = now();
      }
    }
    ioData_[r - 1] = v;
  } else if (r >= 4 && r <= 6) {
    ioCtrl_[r - 4] = v;
  }
}

// ------------------------------------------------------------ quadro
void MD::syncZ80(int64_t target) {
  while (z80Mclk_ < target) {
    if (z80Reset_ || z80BusReq_) { z80Mclk_ = target; break; }
    const int c = z80.step();
    z80Mclk_ += static_cast<int64_t>(c > 0 ? c : 4) * 15;
  }
}

void MD::runUntil(int64_t target) {
  while (m68Mclk_ < target) {
    const int64_t st = vdp.takeStall() + m68kStall_;
    m68kStall_ = 0;
    if (st) { m68Mclk_ += st; continue; }
    cpu.ipl = vdp.irqLevel();
    // ciclo de atualização da DRAM: a cada 128 ciclos do 68000 o barramento fica 2 ciclos ocupado
    if (m68Mclk_ >= refreshAt_) { refreshAt_ = m68Mclk_ + 128 * 7; m68Mclk_ += 2 * 7; }
    const int c = cpu.step();
    m68Mclk_ += static_cast<int64_t>(c > 0 ? c : 4) * 7;
    cpu.cycles = 0;
  }
  syncZ80(target);
}

void MD::runFrame() {
  const int total = vdp.linesPerFrame();
  const int act = vdp.activeLines();
  for (int i = 0; i < total; i++) {
    const int line = (act + i) % total;
    lineStart_ = mclk_;
    if (ext) ext->busLine(line, mclk_);
    vdp.startLine(line, mclk_);
    if (line == act && !firstFrame_) {
      runUntil(lineStart_ + (vdp.h40() ? 788 : 770));
      vdp.vint();
      z80.irqLine = true;
      runUntil(lineStart_ + VDP::MCLK_PER_LINE);
      z80.irqLine = false;
    } else {
      runUntil(lineStart_ + VDP::MCLK_PER_LINE);
    }
    mclk_ = lineStart_ + VDP::MCLK_PER_LINE;
    if (ext_) ext_->runTo(mclk_);
  }
  ym.run(mclk_ / 7);
  psg.run(mclk_ / 15);
  mixAudio();
  firstFrame_ = false;
  frame++;
}

// ------------------------------------------------------------ som
void MD::setSampleRate(int rate) {
  if (rate <= 0) rate = 48000;
  ym.setRates(masterClock() / 7.0 / 144.0, rate);
  const double psgClock = masterClock() / 15.0;
  psg.setSampleRate(static_cast<int>(rate * 3579545.0 / psgClock + 0.5));
  psgLpK_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979 * 6000.0 / rate));
}

// fim de quadro: tudo o que o YM2612 e o PSG produziram vai para filas próprias (que entram no save state) e o
// que os dois têm em comum é misturado na saída
void MD::mixAudio() {
  auto take = [](auto& chip, std::vector<float>& L, std::vector<float>& R) {
    const size_t n = chip.available();
    const size_t o = L.size();
    L.resize(o + n); R.resize(o + n);
    chip.pull(L.data() + o, R.data() + o, n);
  };
  take(ym, ymL_, ymR_);
  take(psg, psgL_, psgR_);
  const size_t n = std::min(ymL_.size(), psgL_.size());
  for (size_t i = 0; i < n; i++) {
    // PSG: volume relativo ao FM como no Genesis Plus GX e um filtro passa-baixa suave (~6 kHz)
    psgLpL_ += (psgL_[i] - psgLpL_) * psgLpK_;
    psgLpR_ += (psgR_[i] - psgLpR_) * psgLpK_;
    outL_[outW_] = std::clamp(ymL_[i] + psgLpL_ * 0.31f, -1.0f, 1.0f);
    outR_[outW_] = std::clamp(ymR_[i] + psgLpR_ * 0.31f, -1.0f, 1.0f);
    outW_ = (outW_ + 1) % OUTBUF;
    if (outW_ == outR0_) outR0_ = (outR0_ + 1) % OUTBUF;
  }
  ymL_.erase(ymL_.begin(), ymL_.begin() + static_cast<long>(n)); ymR_.erase(ymR_.begin(), ymR_.begin() + static_cast<long>(n));
  psgL_.erase(psgL_.begin(), psgL_.begin() + static_cast<long>(n)); psgR_.erase(psgR_.begin(), psgR_.begin() + static_cast<long>(n));
  // nunca deixa as filas crescerem (uma das fontes parada, taxa estranha)
  if (ymL_.size() > 4096) { ymL_.clear(); ymR_.clear(); }
  if (psgL_.size() > 4096) { psgL_.clear(); psgR_.clear(); }
}

size_t MD::audioAvailable() const { return (outW_ + OUTBUF - outR0_) % OUTBUF; }

void MD::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (outR0_ == outW_) { l[k] = r[k] = 0; continue; }
    l[k] = outL_[outR0_]; r[k] = outR_[outR0_];
    outR0_ = (outR0_ + 1) % OUTBUF;
  }
}

// ------------------------------------------------------------ save state
void MD::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  uint32_t romSize = static_cast<uint32_t>(rom_.size());
  io.sync(romSize);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || romSize != rom_.size()))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  z80.sync(io);
  vdp.sync(io);
  ym.sync(io);
  psg.sync(io);
  io.bytes(ram_); io.bytes(zram_); io.bytes(sram_);
  io.raw(bank_, sizeof bank_); io.sync(sramCtl_);
  io.sync(mclk_); io.sync(lineStart_); io.sync(m68Mclk_); io.sync(z80Mclk_); io.sync(refreshAt_); io.sync(ymBusyUntil_); io.sync(m68kStall_);
  io.sync(z80Reset_); io.sync(z80BusReq_); io.sync(z80Bank_);
  io.raw(ioData_, sizeof ioData_); io.raw(ioCtrl_, sizeof ioCtrl_);
  io.sync(psgLpL_); io.sync(psgLpR_);
  for (auto* v : {&ymL_, &ymR_, &psgL_, &psgR_}) {
    uint32_t n = static_cast<uint32_t>(v->size());
    io.sync(n);
    if (io.reading()) { if (n > 4096) throw std::runtime_error("Estado salvo danificado."); v->resize(n); }
    if (n) io.raw(v->data(), n * sizeof(float));
  }
  io.raw(padPhase_, sizeof padPhase_); io.raw(padLastTh_, sizeof padLastTh_); io.sync(frame); io.sync(firstFrame_);
  if (io.reading()) {
    for (auto& b : bank_) b &= 0x3F;
    for (float* f : {&psgLpL_, &psgLpR_}) if (!(*f == *f) || *f > 100 || *f < -100) *f = 0;
    for (auto* v : {&ymL_, &ymR_, &psgL_, &psgR_}) for (float& f : *v) if (!(f == f) || f > 100 || f < -100) f = 0;
    if (ymL_.size() != ymR_.size()) ymR_.resize(ymL_.size());
    if (psgL_.size() != psgR_.size()) psgR_.resize(psgL_.size());
    sramCtl_ &= 3; z80Bank_ &= 0x1FF; padPhase_[0] &= 7; padPhase_[1] &= 7;
    // relógios: todos andam juntos (no máximo alguns quadros de diferença)
    clampTo<int64_t>(mclk_, 0, int64_t(1) << 50);
    clampTo<int64_t>(lineStart_, mclk_ - 3420 * 2, mclk_);
    clampTo<int64_t>(m68Mclk_, mclk_ - 3420 * 4, mclk_ + int64_t(3420) * 8000);  // DMA longo na tela segura a CPU por vários quadros
    clampTo<int64_t>(z80Mclk_, mclk_ - 3420 * 4, mclk_ + 3420 * 4);
    clampTo<int64_t>(refreshAt_, 0, m68Mclk_ + 128 * 7);
    clampTo<int64_t>(ymBusyUntil_, 0, mclk_ + 3420 * 400);
    clampTo<int64_t>(m68kStall_, 0, 3420 * 400);
    for (auto& t : padLastTh_) clampTo<int64_t>(t, -(int64_t(1) << 50), mclk_ + 3420 * 400);
    ym.clampClock(mclk_ / 7);
    psg.clampClock(mclk_ / 15);
  }
}

std::vector<uint8_t> MD::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void MD::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

}  // namespace indigo::md
