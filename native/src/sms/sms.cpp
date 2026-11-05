#include "sms.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <stdexcept>

namespace indigo::sms {

namespace {
// mesmos bits de Key em core.h (KEY de src/sms/sms.js)
enum { K_A = 0, K_B = 1, K_SELECT = 2, K_START = 3, K_RIGHT = 4, K_LEFT = 5, K_UP = 6, K_DOWN = 7 };
constexpr int LINES = 262, CYCLES_PER_LINE = 228;
const char STATE_MAGIC[4] = {'I', 'S', 'M', '1'};

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
}  // namespace

// ------------------------------------------------------------ cabeçalho (src/sms/header.js)
std::string smsKind(const std::vector<uint8_t>& rom, const std::string& name) {
  std::string n = name;
  std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
  if (endsWith(n, ".gg")) return "gg";
  if (endsWith(n, ".sms") || endsWith(n, ".sg")) return "sms";
  static const char TAG[] = "TMR SEGA";
  for (size_t base : {0x7FF0u, 0x3FF0u, 0x1FF0u}) {
    if (base + 16 > rom.size()) continue;
    bool ok = true;
    for (int k = 0; k < 8; k++) if (rom[base + k] != static_cast<uint8_t>(TAG[k])) { ok = false; break; }
    if (ok) return (rom[base + 15] >> 4) >= 5 ? "gg" : "sms";
  }
  return "";
}

// ROM sem cabeçalho de copiadora e com tamanho potência de 2 (como o núcleo enxerga)
std::vector<uint8_t> smsNormalize(const std::vector<uint8_t>& data) {
  size_t start = (data.size() & 0x3FFF) == 512 ? 512 : 0;
  const size_t len = data.size() - start;
  size_t size = 0x4000;
  while (size < len) size <<= 1;
  std::vector<uint8_t> rom(size, 0xFF);
  std::copy(data.begin() + start, data.end(), rom.begin());
  return rom;
}

// código estável da cópia (o Master System não tem código de jogo no cabeçalho)
std::string smsCode(const std::vector<uint8_t>& rom) {
  uint32_t h = 0x811C9DC5u;
  const size_t step = std::max<size_t>(1, rom.size() / 4096);
  for (size_t i = 0; i < rom.size(); i += step) { h ^= rom[i]; h *= 0x01000193u; }
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08X", h);
  return buf;
}

// ------------------------------------------------------------ console
SMS::SMS(bool isGG) : gg(isGG), cpu(*this), vdp(isGG), ram(0x2000), cartRam(0x8000) {}

void SMS::loadRom(const std::vector<uint8_t>& data) {
  rom = smsNormalize(data);
  banks = static_cast<int>(rom.size() >> 14);
  gameCode = smsCode(rom);
  const int cs = rom[0x7FE6 % rom.size()] | (rom[0x7FE7 % rom.size()] << 8);
  const int inv = rom[0x7FE8 % rom.size()] | (rom[0x7FE9 % rom.size()] << 8);
  codemasters = rom.size() >= 0x8000 && cs && ((cs + inv) & 0xFFFF) == 0;
  reset();
}

void SMS::reset() {
  cpu.reset();
  vdp.reset();
  apu.reset();
  std::fill(ram.begin(), ram.end(), 0);
  slot[0] = 0; slot[1] = 1; slot[2] = 2;
  ramCtl = 0;  // FFFC
  cartRamUsed = false; dirty = false;
  ioCtl = 0xFF; memCtl = 0;
  cycles = 0;  // ciclos totais (para o som)
  over = 0; cpuCyclesInLine = 0;
  pausePrev = false;
  frame = 0;
  if (codemasters) { slot[0] = 0; slot[1] = 1; slot[2] = 0; }
}

int SMS::read(int a) {
  if (a < 0x400) return rom[a];
  if (a < 0x4000) return rom[((slot[0] % banks) << 14) | a];
  if (a < 0x8000) return rom[((slot[1] % banks) << 14) | (a & 0x3FFF)];
  if (a < 0xC000) {
    if (ramCtl & 0x08) return cartRam[((ramCtl & 4) << 12) | (a & 0x3FFF)];
    return rom[((slot[2] % banks) << 14) | (a & 0x3FFF)];
  }
  return ram[a & 0x1FFF];
}

void SMS::write(int a, int v) {
  if (a >= 0xC000) {
    ram[a & 0x1FFF] = static_cast<uint8_t>(v);
    if (a >= 0xFFFC && !codemasters) {
      switch (a) {
        case 0xFFFC: ramCtl = v; if (v & 0x08) cartRamUsed = true; break;
        case 0xFFFD: slot[0] = v; break;
        case 0xFFFE: slot[1] = v; break;
        case 0xFFFF: slot[2] = v; break;
      }
    }
    return;
  }
  if (a >= 0x8000 && (ramCtl & 0x08)) {
    cartRam[((ramCtl & 4) << 12) | (a & 0x3FFF)] = static_cast<uint8_t>(v);
    dirty = true;
    return;
  }
  if (codemasters) {
    if (a == 0x0000) slot[0] = v;
    else if (a == 0x4000) slot[1] = v;
    else if (a == 0x8000) slot[2] = v;
  }
}

int SMS::in(int port) {
  const int p = port & 0xFF;
  if (gg && p <= 0x06) {
    if (p == 0x00) return ((keys & (1 << K_START)) ? 0 : 0x80) | 0x40;  // Start + região exportação
    if (p == 0x01 || p == 0x05) return 0x00;
    if (p == 0x02) return 0x7F;
    if (p == 0x03) return 0x00;
    if (p == 0x04) return 0xFF;
    return 0x00;
  }
  switch (p & 0xC1) {
    case 0x00: case 0x01: return 0xFF;
    case 0x40: return vdp.readV();
    case 0x41: return vdp.hcount;
    case 0x80: return vdp.readData();
    case 0x81: { const int v = vdp.readStatus(); cpu.irqLine = vdp.irq(); return v; }
    case 0xC0: {  // controle 1 + cima/baixo do controle 2
      int v = 0xFF;
      if (keys2 & (1 << K_UP)) v &= ~0x40;
      if (keys2 & (1 << K_DOWN)) v &= ~0x80;
      if (keys & (1 << K_UP)) v &= ~0x01;
      if (keys & (1 << K_DOWN)) v &= ~0x02;
      if (keys & (1 << K_LEFT)) v &= ~0x04;
      if (keys & (1 << K_RIGHT)) v &= ~0x08;
      if (keys & (1 << K_B)) v &= ~0x10;  // botão 1
      if (keys & (1 << K_A)) v &= ~0x20;  // botão 2
      return v;
    }
    default: {  // controle 2 + reset + linhas TH (console de exportação: TH lê de volta o que foi escrito)
      if (gg) return 0xFF;
      const int io = ioCtl;
      const int thA = (io & 0x02) ? 1 : (io >> 5) & 1;
      const int thB = (io & 0x08) ? 1 : (io >> 7) & 1;
      int v = 0x3F | (thA << 6) | (thB << 7);
      if (keys2 & (1 << K_LEFT)) v &= ~0x01;
      if (keys2 & (1 << K_RIGHT)) v &= ~0x02;
      if (keys2 & (1 << K_B)) v &= ~0x04;  // botão 1
      if (keys2 & (1 << K_A)) v &= ~0x08;  // botão 2
      return v;
    }
  }
}

void SMS::out(int port, int v) {
  const int p = port & 0xFF;
  if (gg && p <= 0x06) {
    if (p == 0x06) { apu.run(cycles + cpuCyclesInLine); apu.stereo = v; }
    return;
  }
  switch (p & 0xC1) {
    case 0x00: memCtl = v; return;
    case 0x01: ioCtl = v; vdp.hcount = 0x80; return;
    case 0x40: case 0x41: apu.run(cycles + cpuCyclesInLine); apu.write(v); return;
    case 0x80: vdp.writeData(v); return;
    case 0x81: vdp.writeControl(v); cpu.irqLine = vdp.irq(); return;
    default: return;  // som FM (Japão) não emulado
  }
}

void SMS::setKeys(int mask) {
  // Master System: Start é o botão de pausa do console (NMI na borda)
  const bool pause = !gg && (mask & (1 << K_START)) != 0;
  if (pause && !pausePrev) cpu.nmiPending = true;
  pausePrev = pause;
  keys = mask;
}

void SMS::runFrame() {
  for (int ln = 0; ln < LINES; ln++) {
    vdp.lineStart(ln);
    if (ln < 192) vdp.renderLine(ln);
    cpu.irqLine = vdp.irq();
    int c = over;
    cpuCyclesInLine = c;
    while (c < CYCLES_PER_LINE) {
      c += cpu.step();
      cpuCyclesInLine = c;
    }
    over = c - CYCLES_PER_LINE;
    cycles += CYCLES_PER_LINE;
    cpuCyclesInLine = 0;
  }
  apu.run(cycles);
  frame++;
}

void SMS::importSave(const std::vector<uint8_t>& b) {
  if (b.empty()) return;
  std::copy(b.begin(), b.begin() + std::min(b.size(), cartRam.size()), cartRam.begin());
  cartRamUsed = true;
}

// ------------------------------------------------------------ save state
void SMS::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  bool isGG = gg;
  io.sync(isGG);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || isGG != gg))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  vdp.sync(io);
  apu.sync(io);
  io.bytes(ram); io.bytes(cartRam);
  for (int* p : {&slot[0], &slot[1], &slot[2], &ramCtl, &ioCtl, &memCtl, &over}) io.sync(*p);
  io.sync(cycles); io.sync(cartRamUsed); io.sync(pausePrev);
  if (io.reading()) {
    for (int* p : {&slot[0], &slot[1], &slot[2], &ramCtl, &ioCtl, &memCtl}) *p &= 0xFF;
    clampTo(over, 0, 32); clampTo<int64_t>(cycles, 0, int64_t(1) << 60);
    // o relógio do som anda junto com o da CPU (no máximo uma linha à frente): um estado danificado com os dois
    // muito afastados faria o som "alcançar" bilhões de ciclos de uma vez
    apu.clampClock(cycles);
  }
}

std::vector<uint8_t> SMS::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void SMS::loadState(const std::vector<uint8_t>& s) {
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
SMSCore::SMSCore(const std::vector<uint8_t>& rom, bool gg, const std::vector<uint8_t>* save)
    : m_(std::make_unique<SMS>(gg)) {
  m_->loadRom(rom);
  if (save) m_->importSave(*save);
}

}  // namespace indigo::sms
