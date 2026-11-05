#include "sg1000.h"

#include <algorithm>
#include <stdexcept>

namespace indigo::tms {

namespace {
const char STATE_MAGIC[4] = {'I', 'S', 'G', '1'};
constexpr size_t MAX_ROM = 4 << 20;

bool endsWith(const std::string& s, const char* suf) {
  const std::string t(suf);
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}
}  // namespace

bool isSg1000Rom(const std::vector<uint8_t>& rom, const std::string& n) {
  (void)rom;
  return endsWith(n, ".sg") || endsWith(n, ".sc");
}

SG1000::SG1000(const std::vector<uint8_t>& data) : ram(0x400), cartRam(0x2000) {
  if (data.empty() || data.size() > MAX_ROM) throw std::runtime_error("ROM de SG-1000 inválida.");
  mapper = data.size() > 0xC000;
  if (mapper) {
    size_t size = 0x4000;
    while (size < data.size()) size <<= 1;
    rom.assign(size, 0xFF);
    banks = static_cast<int>(size >> 14);
  } else {
    rom.assign(0xC000, 0xFF);
    banks = 3;
  }
  std::copy(data.begin(), data.end(), rom.begin());
  extraRam = data.size() <= 0x8000;
  reset();
}

void SG1000::reset() {
  resetBase();
  std::fill(ram.begin(), ram.end(), 0);
  std::fill(cartRam.begin(), cartRam.end(), 0);
  slot[0] = 0; slot[1] = 1; slot[2] = 2;
  pausePrev = false;
}

void SG1000::setKeys(uint32_t mask) {
  // START é o botão de pausa do console: NMI na borda
  const bool pause = (mask & KEY_START) != 0;
  if (pause && !pausePrev) cpu.nmiPending = true;
  pausePrev = pause;
  keys = mask;
}

// ------------------------------------------------------------ memória
int SG1000::read(int a) {
  if (a < 0xC000) {
    if (extraRam && a >= 0x8000) return cartRam[a & 0x1FFF];
    if (!mapper) return rom[a];
    if (a < 0x400) return rom[a];
    return rom[(static_cast<size_t>(slot[a >> 14] % banks) << 14) | (a & 0x3FFF)];
  }
  return ram[a & 0x3FF];
}

void SG1000::write(int a, int v) {
  if (a >= 0xC000) {
    if (mapper && a >= 0xFFFD) { slot[a - 0xFFFD] = v & 0xFF; return; }
    ram[a & 0x3FF] = static_cast<uint8_t>(v);
    return;
  }
  if (extraRam && a >= 0x8000) cartRam[a & 0x1FFF] = static_cast<uint8_t>(v);
}

// ------------------------------------------------------------ portas
int SG1000::in(int port) {
  const int p = port & 0xFF;
  switch (p & 0xC0) {
    case 0x80:
      if (p & 1) { const int v = vdp.readStatus(); updateIrq(); return v; }
      return vdp.readData(now());
    case 0xC0: {
      const uint32_t k = keys, k2 = keys2;
      if (!(p & 1)) {  // DCh: controle 1 + cima/baixo do controle 2
        int v = 0xFF;
        if (k2 & KEY_UP) v &= ~0x40;
        if (k2 & KEY_DOWN) v &= ~0x80;
        if (k & KEY_UP) v &= ~0x01;
        if (k & KEY_DOWN) v &= ~0x02;
        if (k & KEY_LEFT) v &= ~0x04;
        if (k & KEY_RIGHT) v &= ~0x08;
        if (k & KEY_B) v &= ~0x10;
        if (k & KEY_A) v &= ~0x20;
        return v;
      }
      int v = 0xFF;  // DDh: resto do controle 2
      if (k2 & KEY_LEFT) v &= ~0x01;
      if (k2 & KEY_RIGHT) v &= ~0x02;
      if (k2 & KEY_B) v &= ~0x04;
      if (k2 & KEY_A) v &= ~0x08;
      return v;
    }
    default: return 0xFF;
  }
}

void SG1000::out(int port, int v) {
  const int p = port & 0xFF;
  switch (p & 0xC0) {
    case 0x40: snd.run(now()); snd.snWrite(v); return;
    case 0x80:
      if (p & 1) { vdp.writeControl(v, now()); updateIrq(); }
      else vdp.writeData(v, now());
      return;
    default: return;  // C0h–FFh: PPI do teclado do SC-3000 (não emulado)
  }
}

// ------------------------------------------------------------ save state
void SG1000::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  int nb = banks;
  io.sync(nb);
  if (io.reading() && nb != banks) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  syncBase(io);
  io.bytes(ram); io.bytes(cartRam);
  for (int& s : slot) io.sync(s);
  io.sync(pausePrev);
  if (io.reading()) for (int& s : slot) s &= 0xFF;
}

std::vector<uint8_t> SG1000::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void SG1000::loadState(const std::vector<uint8_t>& s) {
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

// ------------------------------------------------------------ adaptador
SG1000Core::SG1000Core(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : m_(std::make_unique<SG1000>(rom)) {
  (void)save;
}

}  // namespace indigo::tms
