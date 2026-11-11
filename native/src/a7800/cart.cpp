// Cartuchos do Atari 7800: leitura do cabeçalho .a78 e os esquemas de troca de banco. A memória do cartucho é
// vista por tabelas de páginas de 4 KB (uma para a CPU, uma para o MARIA), refeitas a cada troca de banco.
#include <algorithm>
#include <cstring>

#include "a7800.h"

namespace indigo::a7800 {

namespace {
constexpr size_t K16 = 0x4000;
bool hasHeader(const std::vector<uint8_t>& f) {
  return f.size() >= 128 && std::memcmp(f.data() + 1, "ATARI7800", 9) == 0;
}
}  // namespace

bool isA7800Rom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (hasHeader(rom)) return true;
  return n.size() > 4 && n.compare(n.size() - 4, 4, ".a78") == 0 && rom.size() >= 0x1000;
}

bool Cart::load(const std::vector<uint8_t>& f, const std::string&, std::string& erro) {
  std::vector<uint8_t> data;
  uint16_t t = 0;
  uint8_t h53 = 0;
  if (hasHeader(f)) {
    size_t size = (size_t(f[49]) << 24) | (size_t(f[50]) << 16) | (size_t(f[51]) << 8) | f[52];
    if (size == 0 || size > f.size() - 128) size = f.size() - 128;
    data.assign(f.begin() + 128, f.begin() + 128 + static_cast<std::ptrdiff_t>(size));
    h53 = f[53];
    t = static_cast<uint16_t>((f[53] << 8) | f[54]);
    // combinações impossíveis que aparecem em cabeçalhos errados (as mesmas correções do MAME)
    switch (t & 0x3D) {
      case 0x05: case 0x09: case 0x11: case 0x21: t &= ~0x01; break;
      case 0x0C: case 0x14: case 0x24: t &= ~0x04; break;
      case 0x18: case 0x28: t &= ~0x08; break;
      case 0x30: t &= ~0x10; break;
      default: break;
    }
    pal = f[57] & 1;
    saveKey = (f[58] & 2) != 0;
    char name[33] = {};
    std::memcpy(name, f.data() + 17, 32);
    title = name;
    while (!title.empty() && (title.back() == ' ' || title.back() == 0)) title.pop_back();
  } else {
    data = f;
    // sem cabeçalho: só dá para adivinhar pelo tamanho
    if (data.size() > 0xD000) t = data.size() == 0x24000 ? 0x0A : 0x02;
  }
  if (data.empty()) { erro = "ROM de Atari 7800 vazia."; return false; }
  if (data.size() > 0x400000) { erro = "ROM de Atari 7800 grande demais."; return false; }

  const bool banksets = h53 & 0x20;
  haltRam = banksets && (h53 & 0x40);
  pokey800 = h53 & 0x80;
  pokey4000 = t & 0x01;
  pokey450 = t & 0x40;
  exrom = t & 0x08;
  bank6 = t & 0x10;
  bankedRam = t & 0x20;
  mirrorRam = (t & 0x80) && !(t & 0x02);
  const bool ram4000 = t & 0x04;
  if (h53 & 0x01) kind = ACTIVISION;
  else if (h53 & 0x02) kind = ABSOLUTE;
  else if (h53 & 0x10) kind = SOUPER;
  else if (t & 0x02) kind = SUPERGAME;
  else kind = data.size() / (banksets ? 2 : 1) > 0xD000 ? SUPERGAME : FLAT;

  if (banksets) {
    const size_t half = data.size() / 2;
    mrom.assign(data.begin() + static_cast<std::ptrdiff_t>(half), data.end());
    data.resize(half);
    // um arquivo de 1 byte com "banksets" ficaria sem ROM (e os bancos dividem pelo tamanho)
    if (data.empty()) { erro = "ROM de Atari 7800 vazia."; return false; }
  }
  romSize = data.size();
  // completa até múltiplo de 16 KB: o plano completa no começo (a ROM termina em $FFFF), os outros no fim
  const size_t padded = (romSize + K16 - 1) / K16 * K16;
  auto pad = [&](std::vector<uint8_t>& v) {
    if (v.size() >= padded && v.size() % K16 == 0) return;
    if (kind == FLAT) v.insert(v.begin(), padded - v.size(), 0xFF);
    else v.resize(padded, 0xFF);
  };
  pad(data);
  if (!mrom.empty()) { mrom.resize(std::min(mrom.size(), padded)); pad(mrom); }
  rom = std::move(data);
  banks = static_cast<int>(rom.size() / K16);

  if (kind == SOUPER || haltRam || bankedRam) ram.assign(0x8000, 0);
  else if (ram4000 || mirrorRam) ram.assign(0x4000, 0);
  if (kind == FLAT && ram4000 && romSize > 0x8000) { ram.clear(); mirrorRam = false; }  // ROM já ocupa $4000 (sem RAM, não há o que espelhar)
  if (ram.empty()) mirrorRam = false;
  if (kind == FLAT && pokey4000 && romSize > 0x8000) pokey4000 = false;
  if (kind != SUPERGAME && kind != FLAT) pokey4000 = false;  // $4000 tem ROM ou RAM nesses esquemas
  reset();
  return true;
}

void Cart::reset() {
  bank = 0;
  ramBank = 0;
  souperChr[0] = souperChr[1] = 0;
  souperMode = 0;
  souperRam[0] = souperRam[1] = 0;
  rebuild();
}

const uint8_t* Cart::romAt(size_t off) const { return rom.data() + (off % rom.size()); }
const uint8_t* Cart::mromAt(size_t off) const {
  const std::vector<uint8_t>& m = mrom.empty() ? rom : mrom;
  return m.data() + (off % m.size());
}

void Cart::rebuild() {
  for (int p = 0; p < 16; p++) { rpage[p] = nullptr; wpage[p] = nullptr; mpage[p] = nullptr; }
  auto map = [&](int page, size_t off) {  // página de 4 KB da ROM
    rpage[page] = romAt(off);
    mpage[page] = mromAt(off);
  };
  auto map16 = [&](int first, size_t off) { for (int k = 0; k < 4; k++) map(first + k, off + size_t(k) * 0x1000); };
  const size_t last = rom.size() - K16;
  switch (kind) {
    case FLAT: {
      const size_t start = rom.size() >= 0x10000 ? 0 : 0x10000 - rom.size();
      for (int p = 3; p < 16; p++) {
        const size_t a = size_t(p) << 12;
        if (a >= start && a - start < rom.size()) map(p, a - start);
      }
      if (romSize <= 0xC000) rpage[3] = mpage[3] = nullptr;  // $3000 só nas ROMs de 52 KB
      break;
    }
    case SUPERGAME: {
      const int sw = exrom ? std::max(1, banks - 1) : banks;
      const int b = (exrom ? 1 : 0) + bank % sw;
      map16(8, size_t(b) * K16);
      map16(12, last);
      if (exrom) map16(4, 0);
      else map16(4, size_t(std::max(0, banks - 2)) * K16);  // banco 6 (penúltimo) em $4000
      break;
    }
    case ABSOLUTE:
      map16(4, size_t(bank & 1) * K16);
      map16(8, 0x8000);
      map16(12, 0xC000);
      break;
    case ACTIVISION: {
      auto map8 = [&](int page, size_t off) { map(page, off); map(page + 1, off + 0x1000); };
      map8(4, 0x1A000);
      map8(6, 0x18000);
      map8(8, 0x1E000);
      map8(10, size_t(bank & 7) * K16);
      map8(12, size_t(bank & 7) * K16 + 0x2000);
      map8(14, 0x1C000);
      break;
    }
    case SOUPER:
      map16(8, size_t(bank & 31) * K16);
      map16(12, size_t(31) * K16);
      for (int p = 0; p < 4; p++) {
        int rp = p;
        if (souperMode & 4) { if (p == 2) rp = souperRam[0] & 7; if (p == 3) rp = souperRam[1] & 7; }
        rpage[4 + p] = mpage[4 + p] = wpage[4 + p] = ram.data() + size_t(rp) * 0x1000;
      }
      if (souperMode & 1) {  // MARIA: $8000 = ROM fixa de $C000, $C000 = RAM de $4000
        mpage[8] = rpage[12]; mpage[9] = rpage[13];
        for (int p = 0; p < 4; p++) mpage[12 + p] = rpage[4 + p];
      }
      return;
  }
  // RAM do cartucho em $4000
  if (!ram.empty() && !mirrorRam) {
    const size_t cpuOff = bankedRam ? size_t(ramBank & 1) * K16 : 0;
    for (int p = 0; p < 4; p++) {
      rpage[4 + p] = wpage[4 + p] = ram.data() + cpuOff + size_t(p) * 0x1000;
      mpage[4 + p] = ram.data() + (haltRam ? K16 : cpuOff) + size_t(p) * 0x1000;
    }
  }
  if (mirrorRam || pokey4000) for (int p = 4; p < 8; p++) rpage[p] = mpage[p] = nullptr;
}

void Cart::write(uint16_t a, uint8_t v) {
  switch (kind) {
    case FLAT: break;
    case SUPERGAME:
      if (a >= 0x8000 && a < 0xC000) {
        bank = v;
        if (bankedRam) ramBank = (v >> 5) & 1;
        rebuild();
      }
      break;
    case ABSOLUTE:
      if (a == 0x8000) {
        if (v & 1) bank = 0; else if (v & 2) bank = 1;
        rebuild();
      }
      break;
    case ACTIVISION:
      if (a >= 0xFF80 && a <= 0xFF8F) { bank = a & 7; rebuild(); }
      break;
    case SOUPER:
      switch (a) {
        case 0x8000: bank = v & 31; break;
        case 0x8001: souperChr[0] = v; break;
        case 0x8002: souperChr[1] = v; break;
        case 0x8003: souperMode = v; break;
        case 0x8004: souperRam[0] = v & 7; break;
        case 0x8005: souperRam[1] = v & 7; break;
        default: return;
      }
      rebuild();
      break;
  }
  // banksets com RAM: a CPU grava a RAM do MARIA em $C000–$FFFF
  if (haltRam && a >= 0xC000 && ram.size() >= 0x8000) ram[K16 + (a & 0x3FFF)] = v;
}

// Souper no modo de caracteres: $A000–$BFFF vira uma janela de 2 bancos de gráficos
uint8_t Cart::mariaSpecial(uint16_t a) const {
  const uint8_t page = souperChr[(a & 0x80) ? 1 : 0];
  const size_t off = (size_t(((page & 0xFE) << 4) | (page & 1)) << 7) | (a & 0x0F7F);
  return off < rom.size() ? rom[off] : 0;
}

void Cart::sync(StateIO& io) {
  io.sync(bank); io.sync(ramBank);
  io.raw(souperChr, sizeof souperChr); io.sync(souperMode); io.raw(souperRam, sizeof souperRam);
  io.bytes(ram);
  if (io.reading()) rebuild();
}

}  // namespace indigo::a7800
