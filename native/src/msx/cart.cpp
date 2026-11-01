#include "cart.h"

#include <algorithm>
#include <stdexcept>

namespace indigo::msx {

namespace {
// janela vazia: lê FFh
struct Empty {
  uint8_t b[0x2000];
  Empty() { std::fill(b, b + sizeof b, 0xFF); }
};
const Empty EMPTY;

bool hasAB(const std::vector<uint8_t>& r, size_t off) {
  return off + 16 <= r.size() && r[off] == 'A' && r[off + 1] == 'B';
}
int word(const std::vector<uint8_t>& r, size_t off) { return r[off] | (r[off + 1] << 8); }
}  // namespace

const char* mapperName(Mapper m) {
  switch (m) {
    case Mapper::PLAIN: return "ROM simples";
    case Mapper::KONAMI: return "Konami";
    case Mapper::KONAMI_SCC: return "Konami SCC";
    case Mapper::ASCII8: return "ASCII 8 KB";
    case Mapper::ASCII16: return "ASCII 16 KB";
    case Mapper::GENERIC8K: return "genérico 8 KB";
  }
  return "?";
}

// posição de uma ROM simples: página do cabeçalho "AB" e para onde apontam INIT/STATEMENT/DEVICE/TEXT
static int guessPlainStart(const std::vector<uint8_t>& r) {
  const size_t size = r.size();
  if (size > 0xC000) return 0x0000;  // 64 KB: 0000h–FFFFh
  int hdr = -1;
  if (hasAB(r, 0)) hdr = 0;
  else if (size > 0x4000 && hasAB(r, 0x4000)) hdr = 1;
  if (hdr < 0) return size > 0x8000 ? 0x0000 : 0x4000;
  const size_t h = static_cast<size_t>(hdr) * 0x4000;
  const int init = word(r, h + 2), text = word(r, h + 8);
  if (!init && text) {  // programa em BASIC: fica em 8000h
    const int p = text >> 14;
    return std::max(0, p - hdr) * 0x4000;
  }
  int votes[4] = {};
  for (int k = 2; k < 10; k += 2) {
    const int a = word(r, h + k);
    if (a) votes[a >> 14] += (k == 2) ? 2 : 1;  // o INIT pesa mais
  }
  int best = -1;
  for (int p = 0; p < 4; p++) if (votes[p] && (best < 0 || votes[p] > votes[best])) best = p;
  if (best < 0) best = 1;
  int start = (best - hdr) * 0x4000;
  if (start < 0) start = 0;
  if (start + static_cast<int>(size) > 0x10000) start = std::max(0, 0x10000 - static_cast<int>((size + 0x3FFF) & ~size_t(0x3FFF)));
  return start;
}

Mapper guessMapper(const std::vector<uint8_t>& r, int& plainStart) {
  plainStart = 0x4000;
  const size_t size = r.size();
  if (size < 0x10000 || (size == 0x10000 && !hasAB(r, 0))) {
    plainStart = guessPlainStart(r);
    return Mapper::PLAIN;
  }
  // megaROM: conta as gravações LD (nnnn),A nos registradores de cada mapeador (heurística do openMSX)
  enum { G8 = 0, KON, SCC_, A8, A16, N };
  unsigned guess[N] = {};
  for (size_t i = 0; i + 3 < size; i++) {
    if (r[i] != 0x32) continue;
    switch (r[i + 1] | (r[i + 2] << 8)) {
      case 0x5000: case 0x9000: case 0xB000: guess[SCC_]++; break;
      case 0x4000: case 0x8000: case 0xA000: guess[KON]++; guess[G8]++; break;
      case 0x6800: case 0x7800: guess[A8]++; break;
      case 0x6000: guess[KON]++; guess[A8]++; guess[A16]++; guess[G8]++; break;
      case 0x7000: guess[SCC_]++; guess[A8]++; guess[A16]++; break;
      case 0x77FF: guess[A16]++; break;
      default: break;
    }
  }
  if (guess[A8]) guess[A8]--;  // empate entre ASCII 8 e 16: fica o 16
  int type = G8;
  for (int t = 0; t < N; t++) if (guess[t] && guess[t] >= guess[type]) type = t;
  if (type == A16 && guess[G8] == guess[A16]) type = G8;
  static const Mapper M[N] = {Mapper::GENERIC8K, Mapper::KONAMI, Mapper::KONAMI_SCC, Mapper::ASCII8, Mapper::ASCII16};
  return M[type];
}

Cart::Cart(const std::vector<uint8_t>& data) : sram(0x2000, 0xFF) {
  if (data.empty()) throw std::runtime_error("ROM de MSX vazia.");
  if (data.size() > 8u * 1024 * 1024) throw std::runtime_error("ROM de MSX grande demais (mais de 8 MB).");
  mapper = guessMapper(data, plainStart);
  size_t sz = 0x2000;
  while (sz < data.size()) sz <<= 1;
  rom.assign(sz, 0xFF);
  std::copy(data.begin(), data.end(), rom.begin());
  banks8 = static_cast<int>(sz >> 13);
  if (mapper == Mapper::ASCII8 && banks8 <= 0x80) sramBit = banks8;
  if (mapper == Mapper::ASCII16 && banks8 / 2 <= 0x80) sramBit = std::max(1, banks8 / 2);
  reset();
}

void Cart::reset() {
  sccOn = false;
  scc.reset();
  for (int& b : bank) b = -1;
  for (bool& b : sramReg) b = false;
  switch (mapper) {
    case Mapper::PLAIN: {
      const int s = static_cast<int>(rom.size());
      const bool basic = plainStart == 0x8000 && s <= 0x4000 && !(rom[2] | rom[3]);
      for (int reg = 0; reg < 8; reg++) {
        const int a = reg * 0x2000;
        if (basic && (a < plainStart || a >= plainStart + s)) continue;  // BASIC: sem espelhos
        if (s > 0x10000) continue;
        bank[reg] = ((a - plainStart) & (s - 1)) >> 13;
      }
      break;
    }
    case Mapper::KONAMI: case Mapper::KONAMI_SCC: case Mapper::GENERIC8K:
      for (int k = 0; k < 4; k++) bank[2 + k] = k;
      break;
    case Mapper::ASCII8:
      for (int k = 0; k < 4; k++) bank[2 + k] = 0;
      break;
    case Mapper::ASCII16:
      bank[2] = 0; bank[3] = 1; bank[4] = 0; bank[5] = 1;
      break;
  }
  remap();
}

void Cart::remap() {
  for (int reg = 0; reg < 8; reg++) {
    const int b = bank[reg];
    if (b == -2) rd[reg] = sram.data();
    else if (b < 0) rd[reg] = EMPTY.b;
    else rd[reg] = rom.data() + (static_cast<size_t>(b & (banks8 - 1)) << 13);
  }
  if (sccOn) rd[4] = nullptr;
}

int Cart::read(int addr) const {
  const int reg = (addr >> 13) & 7;
  if (reg == 4 && sccOn && addr >= 0x9800) return scc.read(addr);
  if (bank[reg] == -2) return sram[addr & 0x1FFF];
  if (bank[reg] < 0) return 0xFF;
  return rom[(static_cast<size_t>(bank[reg] & (banks8 - 1)) << 13) | (addr & 0x1FFF)];
}

bool Cart::write(int addr, int v) {
  const int reg = (addr >> 13) & 7;
  switch (mapper) {
    case Mapper::PLAIN: return false;
    case Mapper::KONAMI:
      if (addr < 0x6000 || addr >= 0xC000) return false;
      bank[reg] = v;
      remap();
      return true;
    case Mapper::KONAMI_SCC:
      if (addr >= 0x9800 && addr < 0xA000 && sccOn) { scc.write(addr, v); return false; }
      if (addr < 0x4000 || addr >= 0xC000 || (addr & 0x1800) != 0x1000) return false;
      bank[reg] = v;
      if (reg == 4) sccOn = (v & 0x3F) == 0x3F;
      remap();
      return true;
    case Mapper::GENERIC8K:
      if (addr < 0x4000 || addr >= 0xC000) return false;
      bank[reg] = v;
      remap();
      return true;
    case Mapper::ASCII8: case Mapper::ASCII16: {
      if (addr >= 0x8000 && addr < 0xC000) {
        if (sramReg[reg] && !sramUsed) {  // gravou na janela com o bit da SRAM: o cartucho tem SRAM
          sramUsed = true;
          for (int k = 4; k < 6; k++) if (sramReg[k]) bank[k] = -2;
          remap();
        }
        if (bank[reg] == -2) { sram[addr & 0x1FFF] = static_cast<uint8_t>(v); sramDirty = true; }
        return sramUsed;
      }
      if (addr < 0x6000 || addr >= 0x8000) return false;
      if (mapper == Mapper::ASCII8) {
        const int r = 2 + ((addr >> 11) & 3);
        sramReg[r] = sramBit && (v & sramBit) && r >= 4;
        bank[r] = (sramReg[r] && sramUsed) ? -2 : v;
      } else {
        if (addr >= 0x7800 || (addr & 0x0800)) return false;
        const int r = addr < 0x7000 ? 2 : 4;
        sramReg[r] = sramReg[r + 1] = sramBit && (v & sramBit) && r == 4;
        if (sramReg[r] && sramUsed) bank[r] = bank[r + 1] = -2;
        else { bank[r] = v * 2; bank[r + 1] = v * 2 + 1; }
      }
      remap();
      return true;
    }
  }
  return false;
}

void Cart::sync(StateIO& io) {
  int m = static_cast<int>(mapper);
  io.sync(m);
  if (io.reading() && m != static_cast<int>(mapper)) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  io.raw(bank, sizeof bank);
  for (bool& b : sramReg) io.sync(b);
  io.sync(sccOn); io.sync(sramUsed);
  io.bytes(sram);
  scc.sync(io);
  if (io.reading()) {
    for (int& b : bank) {
      if (b < -2) b = -1;
      // só valores que o próprio registrador pode ter (o banco é sempre reduzido pela máscara da ROM)
      if (b > 0x1FF) b &= 0x1FF;
    }
    if (mapper != Mapper::KONAMI_SCC) sccOn = false;
    remap();
  }
}

}  // namespace indigo::msx
