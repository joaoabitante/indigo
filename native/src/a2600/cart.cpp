// Cartuchos do Atari 2600: detecção do esquema de bancos (pelo tamanho e por assinaturas no código, na mesma
// ordem do detector do Stella) e os esquemas F8/F6/F4 (com e sem Superchip), FE, E0, E7, 3F, FA, F0, UA e DPC.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "a2600.h"

namespace indigo::a2600 {

namespace {

bool findSig(const std::vector<uint8_t>& img, std::initializer_list<uint8_t> sig, size_t minHits = 1,
          size_t from = 0, size_t to = SIZE_MAX) {
  const size_t n = sig.size();
  to = std::min(to, img.size());
  if (to < from + n) return false;
  size_t hits = 0;
  for (size_t i = from; i + n <= to; i++) {
    if (std::equal(sig.begin(), sig.end(), img.begin() + i)) {
      if (++hits == minHits) return true;
      i += n - 1;
    }
  }
  return false;
}
bool anyOf(const std::vector<uint8_t>& img, std::initializer_list<std::initializer_list<uint8_t>> sigs,
           size_t minHits = 1) {
  for (auto& s : sigs) if (findSig(img, s, minHits)) return true;
  return false;
}
bool tail(const std::vector<uint8_t>& img, std::initializer_list<uint8_t> sig) {
  return img.size() >= 8 && findSig(img, sig, 1, img.size() - 8);
}

// Superchip: os primeiros 256 bytes de cada banco de 4 KB têm as 2 metades de 128 iguais (área da RAM)
bool isSC(const std::vector<uint8_t>& img) {
  for (size_t p = 0; p + 4096 <= img.size(); p += 4096)
    if (std::memcmp(&img[p], &img[p + 128], 128) != 0) return false;
  return true;
}
bool is4KSC(const std::vector<uint8_t>& img) {
  for (int i = 1; i < 256; i++) if (img[i] != img[0]) return false;
  return img[img.size() - 6] == 'S' && img[img.size() - 5] == 'C';
}
bool isE0(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0x8D, 0xE0, 0x1F}, {0x8D, 0xE0, 0x5F}, {0x8D, 0xE9, 0xFF}, {0x0C, 0xE0, 0x1F},
                   {0xAD, 0xE0, 0x1F}, {0xAD, 0xE9, 0xFF}, {0xAD, 0xED, 0xFF}, {0xAD, 0xF3, 0xBF}});
}
bool is3E(const std::vector<uint8_t>& i) { return findSig(i, {0x85, 0x3E}) && findSig(i, {0x85, 0x3F}, 2); }
bool is3EX(const std::vector<uint8_t>& i) { return findSig(i, {'3', 'E', 'X'}, 2); }
bool is3F(const std::vector<uint8_t>& i) { return findSig(i, {0x85, 0x3F}, 2); }
bool isUA(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0x8D, 0x40, 0x02}, {0xAD, 0x40, 0x02}, {0xBD, 0x1F, 0x02}, {0x2C, 0xC0, 0x02},
                   {0x8D, 0xC0, 0x02}, {0xAD, 0xC0, 0x02}, {0x2C, 0xB0, 0x0F}});
}
bool is0FA0(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0x2C, 0xC0, 0x0F}, {0x8D, 0xC0, 0x0F}, {0xAD, 0xC0, 0x0F}, {0x2C, 0xC0, 0xEF}});
}
bool isFE(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0x20, 0x00, 0xD0, 0xC6, 0xC5}, {0x20, 0xC3, 0xF8, 0xA5, 0x82}, {0xD0, 0xFB, 0x20, 0x73, 0xFE},
                   {0xD0, 0xFB, 0x20, 0x68, 0xFE}, {0x20, 0x00, 0xF0, 0x84, 0xD6}});
}
bool is0840(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0xAD, 0x00, 0x08}, {0xAD, 0x40, 0x08}, {0x2C, 0x00, 0x08}}, 2) ||
         anyOf(i, {{0x0C, 0x00, 0x08, 0x4C}, {0x0C, 0xFF, 0x0F, 0x4C}}, 2);
}
bool isE78K(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0xAD, 0xE4, 0xFF}, {0xAD, 0xE5, 0xFF}, {0xAD, 0xE6, 0xFF}});
}
bool isE7(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0xAD, 0xE2, 0xFF}, {0xAD, 0xE5, 0xFF}, {0xAD, 0xE5, 0x1F}, {0xAD, 0xE7, 0x1F},
                   {0x0C, 0xE7, 0x1F}, {0x8D, 0xE7, 0xFF}, {0x8D, 0xE7, 0x1F}});
}
bool isWD(const std::vector<uint8_t>& i) { return findSig(i, {0xA5, 0x39, 0x4C}); }
bool isFC(const std::vector<uint8_t>& i) {
  return anyOf(i, {{0x8d, 0xf8, 0x1f, 0x4a, 0x4a, 0x8d}, {0x8d, 0xf8, 0xff, 0x8d, 0xfc, 0xff},
                   {0x8c, 0xf9, 0xff, 0xad, 0xfc, 0xff}});
}
bool is03E0(const std::vector<uint8_t>& i) { return anyOf(i, {{0x0D, 0xE0, 0x03, 0x0D}, {0xAD, 0xE0, 0x03, 0xAD}}); }
bool isCV(const std::vector<uint8_t>& i) { return anyOf(i, {{0x9D, 0xFF, 0xF3}, {0x99, 0x00, 0xF4}}); }

[[noreturn]] void unsupported(const char* name) {
  throw std::runtime_error(std::string("Este jogo de Atari 2600 usa o esquema de bancos ") + name +
                           ", que o Índigo ainda não tem.");
}

}  // namespace

const char* bankName(Bank b) {
  switch (b) {
    case Bank::K2: return "2K";
    case Bank::K4: return "4K";
    case Bank::K4SC: return "4KSC";
    case Bank::F8: return "F8";
    case Bank::F8SC: return "F8SC";
    case Bank::F6: return "F6";
    case Bank::F6SC: return "F6SC";
    case Bank::F4: return "F4";
    case Bank::F4SC: return "F4SC";
    case Bank::FE: return "FE";
    case Bank::E0: return "E0";
    case Bank::E7: return "E7";
    case Bank::F3F: return "3F";
    case Bank::FA: return "FA";
    case Bank::F0: return "F0";
    case Bank::UA: return "UA";
    case Bank::DPC: return "DPC";
  }
  return "?";
}

Bank detectBank(const std::vector<uint8_t>& img) {
  const size_t s = img.size();
  if (s == 0) throw std::runtime_error("Arquivo vazio.");
  if (s >= 4 && img[0] == 0x7F && img[1] == 'E' && img[2] == 'L' && img[3] == 'F') unsupported("ELF");
  if (s % 8448 == 0 || s == 6144) unsupported("Supercharger");
  if (findSig(img, {'T', 'J', '3', 'E'})) unsupported("3E+");
  if (s <= 2048 || (s == 4096 && std::memcmp(img.data(), img.data() + 2048, 2048) == 0)) {
    if (isCV(img)) unsupported("CV");
    return Bank::K2;
  }
  if (s == 4096) {
    if (isCV(img)) unsupported("CV");
    if (is4KSC(img)) return Bank::K4SC;
    return Bank::K4;
  }
  if (s == 8192) {
    const bool f8 = anyOf(img, {{0x8D, 0xF9, 0x1F}, {0x8D, 0xF9, 0xFF}}, 2);
    if (isSC(img)) return Bank::F8SC;
    if (std::memcmp(img.data(), img.data() + 4096, 4096) == 0) return Bank::K4;
    if (isE0(img)) return Bank::E0;
    if (is3EX(img) || is3E(img)) unsupported("3E");
    if (is3F(img)) return Bank::F3F;
    if (isUA(img)) return Bank::UA;
    if (is0FA0(img)) unsupported("0FA0");
    if (isFE(img) && !f8) return Bank::FE;
    if (is0840(img)) unsupported("0840");
    if (isE78K(img)) return Bank::E7;
    if (isWD(img)) unsupported("WD");
    if (isFC(img)) unsupported("FC");
    if (is03E0(img)) unsupported("03E0");
    return Bank::F8;
  }
  if (s >= 10240 && s <= 10240 + 256) return Bank::DPC;
  if (s == 12288) return isE7(img) ? Bank::E7 : Bank::FA;
  if (s == 16384) {
    if (isSC(img)) return Bank::F6SC;
    if (isE7(img)) return Bank::E7;
    if (isFC(img)) unsupported("FC");
    if (is3EX(img) || is3E(img)) unsupported("3E");
    if (findSig(img, {0xad, 0xf1, 0xff, 0x60})) unsupported("JANE");
    return Bank::F6;
  }
  if (s == 24576 || s == 28672 || s == 29696) unsupported("FA2/DPC+");
  if (s == 32768) {
    if (findSig(img, {'L', 'E', 'N', 'I', 'N'})) unsupported("CTY");
    if (findSig(img, {'C', 'D', 'F'}, 3) || findSig(img, {'D', 'P', 'C', '+'}, 2) || findSig(img, {'B', 'U', 'S'}, 2))
      unsupported("ARM (DPC+/CDF/BUS)");
    if (isSC(img)) return Bank::F4SC;
    if (is3EX(img) || is3E(img)) unsupported("3E");
    if (is3F(img)) return Bank::F3F;
    if (isFC(img)) unsupported("FC");
    return Bank::F4;
  }
  if (s == 65536) {
    if (tail(img, {'E', 'F', 'F', 'B'}) || findSig(img, {'C', 'D', 'F'}, 3)) unsupported("EFF/CDF");
    if (is3EX(img) || is3E(img)) unsupported("3E");
    if (is3F(img)) return Bank::F3F;
    if (tail(img, {'E', 'F', 'E', 'F'}) || tail(img, {'E', 'F', 'S', 'C'}) ||
        anyOf(img, {{0x0C, 0xE0, 0xFF}, {0xAD, 0xE0, 0xFF}, {0x0C, 0xE0, 0x1F}, {0xAD, 0xE0, 0x1F}}))
      unsupported("EF");
    return Bank::F0;
  }
  if (s <= 512 * 1024 && s % 2048 == 0 && is3F(img)) return Bank::F3F;
  throw std::runtime_error("Tamanho de cartucho de Atari 2600 desconhecido (" + std::to_string(s) + " bytes).");
}

// ---------------------------------------------------------------- montagem
Cart::Cart(const std::vector<uint8_t>& img) : type(detectBank(img)) {
  switch (type) {
    case Bank::K2: {
      // cartuchos de até 2 KB: espelhados em toda a janela de 4 KB
      size_t p = 1;
      while (p < img.size()) p <<= 1;
      std::vector<uint8_t> pow(p, 0);
      std::copy(img.begin(), img.end(), pow.begin());
      rom_.resize(4096);
      for (size_t i = 0; i < 4096; i++) rom_[i] = pow[i % p];
      break;
    }
    case Bank::K4:
      rom_.assign(img.begin(), img.begin() + 4096);
      break;
    case Bank::DPC:
      rom_.assign(img.begin(), img.begin() + 8192);
      display_.assign(img.begin() + 8192, img.begin() + 10240);
      break;
    case Bank::F3F: {
      size_t p = 2048;
      while (p < img.size()) p <<= 1;
      rom_.assign(p, 0);
      std::copy(img.begin(), img.end(), rom_.begin());
      break;
    }
    default:
      rom_ = img;
      break;
  }
  switch (type) {
    case Bank::K4SC: case Bank::F8SC: case Bank::F6SC: case Bank::F4SC: ramSize_ = 128; break;
    case Bank::FA: ramSize_ = 256; break;
    case Bank::E7: ramSize_ = 2048; break;
    default: ramSize_ = 0; break;
  }
  ram_.assign(ramSize_, 0);
  if (type == Bank::E7 || type == Bank::F3F) banks4k_ = static_cast<int>(rom_.size() / 2048);
  else if (type == Bank::E0) banks4k_ = 8;
  else banks4k_ = static_cast<int>(rom_.size() / 4096);
  if (banks4k_ < 1) banks4k_ = 1;
  e7ramBank_ = banks4k_ - 1;
  reset();
}

void Cart::reset() {
  std::fill(ram_.begin(), ram_.end(), 0);
  lastFE_ = false;
  switch (type) {
    case Bank::F8: case Bank::F8SC: case Bank::DPC: cur_ = 1; break;
    case Bank::FA: cur_ = 2; break;
    case Bank::F0: cur_ = 15; break;
    default: cur_ = 0; break;
  }
  e0_[0] = 4; e0_[1] = 5; e0_[2] = 6;
  e7rom_ = 0; e7ram_ = 0;
  f3f_ = 0;
  std::memset(tops_, 0, sizeof tops_);
  std::memset(bottoms_, 0, sizeof bottoms_);
  std::memset(flags_, 0, sizeof flags_);
  std::memset(counters_, 0, sizeof counters_);
  music_[0] = music_[1] = music_[2] = false;
  random_ = 1;
  audioCycles_ = cycleCounter ? *cycleCounter : 0;
  fracClocks_ = 0;
  remap();
}

void Cart::remap() {
  switch (type) {
    case Bank::E0:
      for (int s = 0; s < 3; s++) off_[s] = static_cast<uint32_t>((e0_[s] & 7) * 1024);
      off_[3] = 7 * 1024;
      break;
    case Bank::E7: case Bank::F3F: {
      const int lo = type == Bank::E7 ? e7rom_ : f3f_;
      off_[0] = static_cast<uint32_t>(lo * 2048);
      off_[1] = off_[0] + 1024;
      off_[2] = static_cast<uint32_t>((banks4k_ - 1) * 2048);
      off_[3] = off_[2] + 1024;
      break;
    }
    default:
      for (int s = 0; s < 4; s++) off_[s] = static_cast<uint32_t>(cur_ * 4096 + s * 1024);
      break;
  }
}

// ---------------------------------------------------------------- bancos
void Cart::hotspot(uint16_t a) {
  if (a < 0xFE0) return;
  switch (type) {
    case Bank::F8: case Bank::F8SC: case Bank::DPC:
      if (a == 0xFF8 || a == 0xFF9) { cur_ = a - 0xFF8; remap(); }
      break;
    case Bank::F6: case Bank::F6SC:
      if (a >= 0xFF6 && a <= 0xFF9) { cur_ = a - 0xFF6; remap(); }
      break;
    case Bank::F4: case Bank::F4SC:
      if (a >= 0xFF4 && a <= 0xFFB) { cur_ = a - 0xFF4; remap(); }
      break;
    case Bank::FA:
      if (a >= 0xFF8 && a <= 0xFFA) { cur_ = a - 0xFF8; remap(); }
      break;
    case Bank::F0:
      if (a == 0xFF0) { cur_ = (cur_ + 1) & 15; remap(); }
      break;
    case Bank::E0:
      if (a <= 0xFF7) { e0_[(a - 0xFE0) >> 3] = a & 7; remap(); }
      break;
    case Bank::E7:
      if (banks4k_ == 4 && a >= 0xFE4 && a <= 0xFE7) { e7rom_ = a & 3; remap(); }
      else if (banks4k_ == 6 && a <= 0xFE7) {
        static constexpr int t[8] = {0, 1, 0, 1, 2, 3, 4, 5};
        e7rom_ = t[a & 7];
        remap();
      } else if (banks4k_ == 8 && a <= 0xFE7) { e7rom_ = a & 7; remap(); }
      else if (a >= 0xFE8 && a <= 0xFEB) e7ram_ = a & 3;
      break;
    default:
      break;
  }
}

void Cart::feWatch(uint16_t a, uint8_t v) {
  // FE: o JSR/RTS acessa a pilha em $01FE; o byte seguinte (endereço alto) diz o banco ($Fxxx = 0, $Dxxx = 1)
  if (lastFE_) {
    cur_ = ((v >> 5) & 1) ^ 1;
    remap();
    lastFE_ = false;
  } else {
    lastFE_ = a == 0x01FE;
  }
}

void Cart::lowAccess(uint16_t a, uint8_t v, bool isWrite) {
  switch (type) {
    case Bank::F3F:
      if (isWrite && a <= 0x3F) { f3f_ = v % banks4k_; remap(); }
      break;
    case Bank::UA:
      if ((a & 0x1260) == 0x0220) { cur_ = 0; remap(); }
      else if ((a & 0x1260) == 0x0240) { cur_ = 1; remap(); }
      break;
    case Bank::FE:
      if (a >= 0x1C0 && a < 0x200) feWatch(a, v);
      break;
    default:
      break;
  }
}

uint8_t Cart::read(uint16_t addr, uint8_t bus) {
  const uint16_t a = addr & 0xFFF;
  if (type == Bank::DPC) {
    if (a < 0x80 || a >= 0xFC0) dpcRandom();
    if (a < 0x40) return dpcRead(a);
  }
  hotspot(a);
  if (ramSize_ && type != Bank::E7) {
    if (a < ramSize_) {  // ler a porta de escrita grava o que estiver no barramento
      ram_[a] = bus;
      return bus;
    }
    if (a < 2 * ramSize_) return ram_[a - ramSize_];
  } else if (type == Bank::E7) {
    if (e7rom_ == e7ramBank_ && a < 0x800) {
      if (a < 0x400) { ram_[a] = bus; return bus; }
      return ram_[a & 0x3FF];
    }
    if (a >= 0x800 && a < 0x900) { ram_[0x400 + e7ram_ * 256 + (a & 0xFF)] = bus; return bus; }
    if (a >= 0x900 && a < 0xA00) return ram_[0x400 + e7ram_ * 256 + (a & 0xFF)];
  }
  const uint8_t v = romAt(a);
  if (type == Bank::FE) feWatch(addr, v);
  return v;
}

void Cart::write(uint16_t addr, uint8_t v) {
  const uint16_t a = addr & 0xFFF;
  if (type == Bank::DPC) {
    dpcRandom();
    if (a >= 0x40 && a < 0x80) { dpcWrite(a, v); return; }
  }
  hotspot(a);
  if (ramSize_ && type != Bank::E7) {
    if (a < ramSize_) ram_[a] = v;
  } else if (type == Bank::E7) {
    if (e7rom_ == e7ramBank_ && a < 0x400) ram_[a] = v;
    else if (a >= 0x800 && a < 0x900) ram_[0x400 + e7ram_ * 256 + (a & 0xFF)] = v;
  }
  if (type == Bank::FE) feWatch(addr, v);
}

uint8_t Cart::peek(uint16_t addr) const {
  const uint16_t a = addr & 0xFFF;
  return rom_[off_[(a >> 10) & 3] + (a & 0x3FF)];
}

// ---------------------------------------------------------------- DPC (Pitfall II)
void Cart::dpcRandom() {
  static constexpr uint8_t f[16] = {1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1};
  const uint8_t bit = f[((random_ >> 3) & 7) | ((random_ & 0x80) ? 8 : 0)];
  random_ = static_cast<uint8_t>((random_ << 1) | bit);
}

void Cart::dpcMusic() {
  const uint64_t now = cycleCounter ? *cycleCounter : 0;
  const uint64_t cycles = now - audioCycles_;
  audioCycles_ = now;
  const double clocks = 20000.0 * static_cast<double>(cycles) / clockRate + fracClocks_;
  const uint64_t whole = static_cast<uint64_t>(clocks);
  fracClocks_ = clocks - static_cast<double>(whole);
  if (!whole) return;
  for (int x = 5; x < 8; x++) {
    if (!music_[x - 5]) continue;
    const int top = tops_[x] + 1;
    int low = counters_[x] & 0xFF;
    if (tops_[x] != 0) {
      low -= static_cast<int>(whole % static_cast<uint64_t>(top));
      if (low < 0) low += top;
    } else {
      low = 0;
    }
    if (low <= bottoms_[x]) flags_[x] = 0x00;
    else if (low <= tops_[x]) flags_[x] = 0xFF;
    counters_[x] = static_cast<uint16_t>((counters_[x] & 0x700) | low);
  }
}

uint8_t Cart::dpcRead(uint16_t a) {
  const int i = a & 7, fn = (a >> 3) & 7;
  if ((counters_[i] & 0xFF) == tops_[i]) flags_[i] = 0xFF;
  else if ((counters_[i] & 0xFF) == bottoms_[i]) flags_[i] = 0x00;
  uint8_t r = 0;
  switch (fn) {
    case 0:
      if (i < 4) {
        r = random_;
      } else {
        static constexpr uint8_t amp[8] = {0x00, 0x04, 0x05, 0x09, 0x06, 0x0a, 0x0b, 0x0f};
        dpcMusic();
        int k = 0;
        if (music_[0] && flags_[5]) k |= 1;
        if (music_[1] && flags_[6]) k |= 2;
        if (music_[2] && flags_[7]) k |= 4;
        r = amp[k];
      }
      break;
    case 1: r = display_[2047 - (counters_[i] & 0x7FF)]; break;
    case 2: r = display_[2047 - (counters_[i] & 0x7FF)] & flags_[i]; break;
    case 7: r = flags_[i]; break;
    default: r = 0; break;
  }
  if (i < 5 || !music_[i - 5]) counters_[i] = (counters_[i] - 1) & 0x7FF;
  return r;
}

void Cart::dpcWrite(uint16_t a, uint8_t v) {
  const int i = a & 7, fn = (a >> 3) & 7;
  switch (fn) {
    case 0: tops_[i] = v; flags_[i] = 0; break;
    case 1: bottoms_[i] = v; break;
    case 2:
      if (i >= 5 && music_[i - 5]) counters_[i] = static_cast<uint16_t>((counters_[i] & 0x700) | tops_[i]);
      else counters_[i] = static_cast<uint16_t>((counters_[i] & 0x700) | v);
      break;
    case 3:
      counters_[i] = static_cast<uint16_t>(((v & 7) << 8) | (counters_[i] & 0xFF));
      if (i >= 5) music_[i - 5] = v & 0x10;
      break;
    case 6: random_ = 1; break;
    default: break;
  }
}

// ---------------------------------------------------------------- estado
void Cart::sync(StateIO& io) {
  io.bytes(ram_);
  io.sync(cur_); io.raw(e0_, sizeof e0_); io.sync(e7rom_); io.sync(e7ram_); io.sync(f3f_); io.sync(lastFE_);
  io.raw(tops_, sizeof tops_); io.raw(bottoms_, sizeof bottoms_); io.raw(flags_, sizeof flags_);
  io.raw(counters_, sizeof counters_);
  for (bool& m : music_) io.sync(m);
  io.sync(random_); io.sync(audioCycles_); io.sync(fracClocks_);
  if (io.reading()) {
    const int nb = (type == Bank::E0) ? 8 : banks4k_;
    clampTo(cur_, 0, std::max(0, static_cast<int>(rom_.size() / 4096) - 1));
    for (int& b : e0_) clampTo(b, 0, 7);
    clampTo(e7rom_, 0, nb - 1);
    clampTo(e7ram_, 0, 3);
    clampTo(f3f_, 0, nb - 1);
    for (auto& c : counters_) c &= 0x7FF;
    finiteOr(fracClocks_, 0);
    if (fracClocks_ < 0 || fracClocks_ >= 1) fracClocks_ = 0;
    remap();
  }
}

}  // namespace indigo::a2600
