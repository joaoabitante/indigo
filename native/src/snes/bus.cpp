// Máquina SNES: cartucho, mapa de memória, registradores da CPU, DMA/HDMA, relógio e interrupções.
// Tempo: cada linha tem 1364 ciclos do relógio mestre e o quadro 262 linhas. Eventos de cada linha: renovação da
// DRAM (40 ciclos parados em ~536), desenho da linha (1096, início do hblank), HDMA (1104), IRQ de H/V, vblank e
// NMI na linha 225, leitura automática dos controles. O SPC700 anda junto, a 1,024 MHz, sincronizado quando a CPU
// mexe nas portas $2140–$217F e no fim de cada linha.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

#include "snes.h"

namespace indigo::snes {

namespace {
constexpr int64_t MASTER_HZ = 21477272, SMP_HZ = 1024000;
constexpr int DMA_OFF[8][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 2, 3}, {0, 1, 0, 1}, {0, 0, 0, 0}, {0, 0, 1, 1}};
constexpr int HDMA_LEN[8] = {1, 2, 2, 4, 4, 4, 2, 4};

size_t mirror(size_t addr, size_t size) { return romMirror(addr, size); }
}  // namespace

// espelhamento de ROMs com tamanho que não é potência de 2 (como no cartucho)
size_t romMirror(size_t addr, size_t size) {
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

namespace {
int headerScore(const std::vector<uint8_t>& d, size_t at, Cart::Map map) {
  if (at + 0x40 > d.size()) return -100;
  int score = 0;
  const uint8_t mapMode = d[at + 0x15];
  const uint16_t checksum = static_cast<uint16_t>(d[at + 0x1E] | (d[at + 0x1F] << 8));
  const uint16_t complement = static_cast<uint16_t>(d[at + 0x1C] | (d[at + 0x1D] << 8));
  const uint16_t reset = static_cast<uint16_t>(d[at + 0x3C] | (d[at + 0x3D] << 8));
  if (static_cast<uint16_t>(checksum + complement) == 0xFFFF) score += 4;
  const int low = mapMode & 0x0F;
  if (map == Cart::LOROM && (low == 0 || low == 2 || low == 3)) score += 2;  // 2: S-DD1; 3: SA-1
  if (map == Cart::HIROM && (low == 1 || low == 0xA)) score += 2;  // A: SPC7110
  if (map == Cart::EXHIROM && low == 5) score += 2;
  if ((mapMode & 0xE0) == 0x20) score += 1;
  if (reset >= 0x8000) score += 1; else score -= 4;
  int printable = 0;
  for (int i = 0; i < 21; i++) { const uint8_t c = d[at + i]; if (c >= 0x20 && c < 0x7F) printable++; }
  if (printable >= 18) score += 1;
  // primeira instrução do vetor de reset: SEI, CLC e outras são o começo mais comum
  size_t resetOff;
  if (map == Cart::LOROM) resetOff = reset & 0x7FFF;
  else if (map == Cart::HIROM) resetOff = reset;
  else resetOff = 0x400000 + size_t(reset);
  if (resetOff < d.size()) {
    const uint8_t op = d[resetOff];
    if (op == 0x78 || op == 0x18 || op == 0x38 || op == 0x9C || op == 0x4C || op == 0x5C) score += 2;
  }
  return score;
}
}  // namespace

// ---------------------------------------------------------------- cartucho
bool isSnesRom(const std::vector<uint8_t>& data, const std::string& fileName) {
  std::string ext;
  const auto dot = fileName.find_last_of('.');
  if (dot != std::string::npos) ext = fileName.substr(dot + 1);
  for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (ext == "sfc" || ext == "smc" || ext == "swc" || ext == "fig") return data.size() >= 0x8000;
  return false;
}

bool parseRom(std::vector<uint8_t> d, Cart& c, std::string& error) {
  if (d.size() % 0x400 == 0x200) d.erase(d.begin(), d.begin() + 0x200);  // cabeçalho de copiadora
  if (d.size() < 0x8000) { error = "ROM de SNES pequena demais."; return false; }
  struct Cand { Cart::Map map; size_t at; };
  const Cand cands[3] = {{Cart::LOROM, 0x7FC0}, {Cart::HIROM, 0xFFC0}, {Cart::EXHIROM, 0x40FFC0}};
  int best = -1000;
  Cand chosen = cands[0];
  for (const auto& cd : cands) {
    if (cd.map == Cart::EXHIROM && d.size() <= 0x400000) continue;
    const int s = headerScore(d, cd.at, cd.map);
    if (s > best) { best = s; chosen = cd; }
  }
  c.map = chosen.map;
  const size_t h = chosen.at;
  c.rom = std::move(d);
  c.title.clear();
  for (int i = 0; i < 21 && h + i < c.rom.size(); i++) {
    const char ch = static_cast<char>(c.rom[h + i]);
    c.title.push_back(ch >= 0x20 && ch < 0x7F ? ch : ' ');
  }
  while (!c.title.empty() && c.title.back() == ' ') c.title.pop_back();
  c.fastRom = c.rom[h + 0x15] & 0x10;
  c.chip = c.rom[h + 0x16];
  const int ramShift = c.rom[h + 0x18];
  const bool hasRam = (c.chip & 0x0F) == 1 || (c.chip & 0x0F) == 2 || (c.chip & 0x0F) == 4 || (c.chip & 0x0F) == 5;
  c.superfx = false;
  c.battery = true;
  const int kind = c.chip & 0x0F;
  if ((c.chip >> 4) == 1 && ((kind >= 3 && kind <= 6) || kind == 0xA) && c.map == Cart::LOROM) {
    // Super FX: RAM do cartucho pelo cabeçalho estendido ($FFBD, 1 KB << n); sem ele, 64 KB
    c.superfx = true;
    const int ext = h >= 3 ? c.rom[h - 3] : 0;
    const bool extOk = c.rom[h + 0x1A] == 0x33 || (ext >= 5 && ext <= 7);
    c.sram.assign(extOk && ext >= 1 && ext <= 7 ? size_t(0x400) << ext : 0x10000, 0);
    c.battery = kind == 5 || kind == 6 || kind == 0xA;
    return true;
  }
  c.dsp1 = Cart::DSP_NONE;
  c.sa1 = false;
  if ((c.chip >> 4) == 3 && kind >= 3 && kind <= 6 && c.map == Cart::LOROM) {
    // SA-1: a RAM do cartucho (BW-RAM) pelo cabeçalho; o mapa de memória é todo do chip
    c.sa1 = true;
    c.sram.assign(ramShift > 0 && ramShift <= 8 ? size_t(0x400) << ramShift : 0x800, 0xFF);
    c.battery = kind == 5 || kind == 6;
    return true;
  }
  c.sdd1 = false;
  if ((c.chip >> 4) == 4 && kind >= 3 && kind <= 5 && c.map == Cart::LOROM) {
    // S-DD1 (Star Ocean com 8 KB de RAM e bateria; Street Fighter Alpha 2 sem RAM)
    c.sdd1 = true;
    if (kind >= 4 && ramShift > 0 && ramShift <= 7) c.sram.assign(size_t(0x400) << ramShift, 0xFF);
    else c.sram.clear();
    c.battery = kind == 5;
    return true;
  }
  c.spc7110 = c.epsonRtc = false;
  if ((c.chip == 0xF5 || c.chip == 0xF9) && c.map == Cart::HIROM && (c.rom[h + 0x15] & 0x0F) == 0x0A) {
    // SPC7110 (os três jogos têm 8 KB de RAM com bateria); $F9 = com o relógio RTC-4513
    c.spc7110 = true;
    c.epsonRtc = c.chip == 0xF9;
    if (ramShift > 0 && ramShift <= 7) c.sram.assign(size_t(0x400) << ramShift, 0xFF);
    else c.sram.clear();
    c.battery = true;
    return true;
  }
  if ((c.chip >> 4) == 0 && kind >= 3 && kind <= 5) {
    // DSP-n: o cabeçalho é o mesmo para os quatro; DSP-2, DSP-3 e DSP-4 se reconhecem pelo jogo
    const std::string& t = c.title;
    const bool dsp2 = t.find("DUNGEON MASTER") != std::string::npos;
    const bool dsp3 = t.rfind("SD", 0) == 0 && t.size() >= 2 && t.compare(t.size() - 2, 2, "GX") == 0;
    const bool dsp4 = t.find("TOP GEAR 3000") != std::string::npos || t.find("PLANETS CHAMP") != std::string::npos;
    if (dsp2 || dsp3 || dsp4) {
      error = std::string("Este jogo de SNES usa o chip ") + (dsp2 ? "DSP-2" : dsp3 ? "DSP-3" : "DSP-4") +
              " no cartucho, que o Índigo ainda não tem.";
      return false;
    }
    if (c.map == Cart::LOROM) c.dsp1 = c.rom.size() > 0x100000 ? Cart::DSP_LOROM_LARGE : Cart::DSP_LOROM_SMALL;
    else c.dsp1 = Cart::DSP_HIROM;
    if (hasRam && ramShift > 0 && ramShift <= 7) c.sram.assign(size_t(0x400) << ramShift, 0xFF);
    else c.sram.clear();
    return true;
  }
  if ((c.chip & 0x0F) >= 3 && c.chip != 0xE3) {
    static const char* NAMES[16] = {"DSP", "Super FX", "OBC1", "SA-1", "S-DD1", "S-RTC", "", "", "", "", "", "", "", "", "Super Game Boy", "especial"};
    const char* name = NAMES[c.chip >> 4];
    error = std::string("Este jogo de SNES usa o chip ") + (name[0] ? name : "especial") + " no cartucho, que o Índigo ainda não tem.";
    return false;
  }
  if (hasRam && ramShift > 0 && ramShift <= 7) c.sram.assign(size_t(0x400) << ramShift, 0xFF);
  else c.sram.clear();
  return true;
}

// registradores do DSP-1 (como nos cartuchos): LoROM até 1 MB em $30-$3F:$8000-$FFFF; LoROM maior em
// $60-$6F:$0000-$7FFF; HiROM em $00-$1F:$6000-$7FFF (e os espelhos em $80+). A metade de cima é o SR.
int SNES::dspPort(uint32_t addr) const {
  const int bank = (addr >> 16) & 0x7F;
  const uint16_t a = static_cast<uint16_t>(addr);
  switch (cart.dsp1) {
    case Cart::DSP_LOROM_SMALL: if (bank >= 0x30 && bank < 0x40 && a >= 0x8000) return a >= 0xC000; break;
    case Cart::DSP_LOROM_LARGE: if (bank >= 0x60 && bank < 0x70 && a < 0x8000) return a >= 0x4000; break;
    case Cart::DSP_HIROM: if (bank < 0x20 && a >= 0x6000 && a < 0x8000) return a >= 0x7000; break;
    default: break;
  }
  return -1;
}

uint8_t SNES::cartRead(uint32_t addr, bool& hit) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  hit = true;
  if (sa1) { uint8_t v = 0; hit = sa1->snesRead(addr, v); return v; }
  if (dsp) { const int port = dspPort(addr); if (port >= 0) return dsp->read(port == 1); }
  if (spc7110) {
    if (bank == 0x50 || bank == 0x58) return spc7110->ioRead(addr, mdr);
    uint8_t v = 0;
    if (spc7110->ramRead(addr, v) || spc7110->romRead(addr, v)) return v;
    hit = false;
    return 0;
  }
  if (sdd1) {
    // ROM em $00-$3F/$80-$BF:$8000-$FFFF e $C0-$FF; RAM em $00-$3F/$80-$BF:$6000-$7FFF e $70-$73
    if (bank >= 0xC0 || (!(bank & 0x40) && a >= 0x8000)) return sdd1->romRead(addr);
    if (!cart.sram.empty()) {
      if (!(bank & 0x40) && a >= 0x6000) return cart.sram[(a & 0x1FFF) % cart.sram.size()];
      if (bank >= 0x70 && bank < 0x74) return cart.sram[((bank & 3) * 0x8000 + (a & 0x7FFF)) % cart.sram.size()];
    }
    hit = false;
    return 0;
  }
  if (gsu) {
    if (bank >= 0x40 && (bank & 0x7F) < 0x60) {
      if (gsu->romBusy()) { static const uint8_t VEC[16] = {0, 1, 0, 1, 4, 1, 0, 1, 0, 1, 8, 1, 0, 1, 0x0C, 1}; return VEC[a & 15]; }
      return gsu->romAt(0x400000 | (uint32_t(bank & 0x1F) << 16) | a);
    }
    if (!(bank & 0x40) && a >= 0x8000) {
      if (gsu->romBusy()) { static const uint8_t VEC[16] = {0, 1, 0, 1, 4, 1, 0, 1, 0, 1, 8, 1, 0, 1, 0x0C, 1}; return VEC[a & 15]; }
      return gsu->romAt((uint32_t(bank & 0x3F) << 16) | a);
    }
    if (gsu->ramBusy()) { hit = false; return 0; }
    if (!(bank & 0x40) && a >= 0x6000 && a < 0x8000) return cart.sram[(a & 0x1FFF) % cart.sram.size()];
    if ((bank & 0x7E) == 0x70) return cart.sram[(((bank & 1) << 16) | a) % cart.sram.size()];
    hit = false;
    return 0;
  }
  size_t off;
  switch (cart.map) {
    case Cart::LOROM:
      if (!cart.sram.empty() && (bank & 0x7F) >= 0x70 && (bank & 0x7F) < 0x7E && a < 0x8000)
        return cart.sram[((bank & 0x0F) * 0x8000 + a) % cart.sram.size()];
      if (a < 0x8000 && !(bank & 0x40)) { hit = false; return 0; }
      off = (bank & 0x7F) * size_t(0x8000) + (a & 0x7FFF);
      break;
    default:
      if (!cart.sram.empty() && (bank & 0x7F) >= 0x20 && (bank & 0x7F) < 0x40 && a >= 0x6000 && a < 0x8000)
        return cart.sram[((bank & 0x1F) * 0x2000 + (a - 0x6000)) % cart.sram.size()];
      if (a < 0x8000 && !(bank & 0x40)) { hit = false; return 0; }
      off = (size_t(bank & 0x3F) << 16) | a;
      if (cart.map == Cart::EXHIROM && !(bank & 0x80)) off += 0x400000;
      break;
  }
  return cart.rom[mirror(off, cart.rom.size())];
}

bool SNES::cartWrite(uint32_t addr, uint8_t v) {
  if (sa1) {
    bool changed = false;
    const bool hit = sa1->snesWrite(addr, v, changed);
    if (changed) dirty = true;
    return hit;
  }
  if (dsp) { const int port = dspPort(addr); if (port >= 0) { dsp->write(port == 1, v); return true; } }
  if (spc7110) {
    const int b = (addr >> 16) & 0xFF;
    if (b == 0x50 || b == 0x58) { spc7110->ioWrite(addr, v); return true; }
    bool changed = false;
    const bool hit = spc7110->ramWrite(addr, v, changed);
    if (changed) dirty = true;
    return hit;
  }
  if (cart.sram.empty()) return false;
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  size_t off;
  if (sdd1) {
    if (!(bank & 0x40) && a >= 0x6000 && a < 0x8000) off = (a & 0x1FFF) % cart.sram.size();
    else if (bank >= 0x70 && bank < 0x74) off = ((bank & 3) * 0x8000 + (a & 0x7FFF)) % cart.sram.size();
    else return false;
    if (cart.sram[off] != v) { cart.sram[off] = v; dirty = true; }
    return true;
  }
  if (gsu) {
    if (gsu->ramBusy()) return false;
    if (!(bank & 0x40) && a >= 0x6000 && a < 0x8000) off = (a & 0x1FFF) % cart.sram.size();
    else if ((bank & 0x7E) == 0x70) off = (((bank & 1) << 16) | a) % cart.sram.size();
    else return false;
    if (cart.sram[off] != v) { cart.sram[off] = v; dirty = true; }
    return true;
  }
  if (cart.map == Cart::LOROM) {
    if ((bank & 0x7F) < 0x70 || (bank & 0x7F) >= 0x7E || a >= 0x8000) return false;
    off = ((bank & 0x0F) * 0x8000 + a) % cart.sram.size();
  } else {
    if ((bank & 0x7F) < 0x20 || (bank & 0x7F) >= 0x40 || a < 0x6000 || a >= 0x8000) return false;
    off = ((bank & 0x1F) * 0x2000 + (a - 0x6000)) % cart.sram.size();
  }
  if (cart.sram[off] != v) { cart.sram[off] = v; dirty = true; }
  return true;
}

// ---------------------------------------------------------------- máquina
SNES::SNES(Cart c) : cart(std::move(c)) {
  if (cart.superfx) gsu = std::make_unique<GSU>(cart.rom, cart.sram);
  if (cart.dsp1 != Cart::DSP_NONE) dsp = std::make_unique<Dsp1>(firmware("dsp1"));
  if (cart.sa1) sa1 = std::make_unique<SA1>(cart.rom, cart.sram);
  if (cart.sdd1) sdd1 = std::make_unique<SDD1>(cart.rom);
  if (cart.spc7110) spc7110 = std::make_unique<SPC7110>(cart.rom, cart.sram, cart.epsonRtc);
  cpu.bus = this;
  reset(true);
}

void SNES::reset(bool power) {
  if (power) {
    std::memset(wram, 0x55, sizeof wram);
    apu.reset();
    ppu.reset();
  } else {
    apu.reset();
  }
  wramAddr = 0;
  mdr = 0;
  hc = 0; vc = 0; vblankLine = 225;
  refreshed = rendered = hdmaDone = false;
  frameDone = false;
  apuDebt = 0;
  nmitimen = 0; wrio = 0xFF; memsel = 0; hdmaen = 0;
  wrmpya = wrdivl = wrdivh = 0xFF;
  htime = vtime = 0x1FF; rddiv = rdmpy = 0;
  nmiFlag = irqFlag = false; autoJoyBusy = false; autoJoyTimer = 0;
  std::memset(joy, 0, sizeof joy);
  joyLatch = 0; joyShift[0] = joyShift[1] = 0;
  for (auto& d : ch) d = Channel{};
  ppu.setRendering(true);
  if (gsu) gsu->reset();
  if (dsp) dsp->reset();
  if (sa1) sa1->reset();
  if (sdd1) sdd1->reset();
  if (spc7110) spc7110->reset(power);
  updateIrq();
  cpu.reset();
}

void SNES::setKeys(uint32_t m, int player) {
  uint16_t b = 0;
  if (m & KEY_B) b |= 0x8000;
  if (m & KEY_Y) b |= 0x4000;
  if (m & KEY_SELECT) b |= 0x2000;
  if (m & KEY_START) b |= 0x1000;
  if (m & KEY_UP) b |= 0x0800;
  if (m & KEY_DOWN) b |= 0x0400;
  if (m & KEY_LEFT) b |= 0x0200;
  if (m & KEY_RIGHT) b |= 0x0100;
  if (m & KEY_A) b |= 0x0080;
  if (m & KEY_X) b |= 0x0040;
  if (m & KEY_L) b |= 0x0020;
  if (m & KEY_R) b |= 0x0010;
  // cima+baixo ou esquerda+direita juntos travam alguns jogos (o controle de verdade não deixa)
  if ((b & 0x0C00) == 0x0C00) b &= ~0x0C00;
  if ((b & 0x0300) == 0x0300) b &= ~0x0300;
  if (player == 1) { pad2 = b; pad2On = true; }
  else pad = b;
}

void SNES::runFrame() {
  frameDone = false;
  for (int guard = 0; !frameDone && guard < 400000; guard++) cpu.step();
  syncApu();
}

// ---------------------------------------------------------------- relógio
int SNES::speed(uint32_t addr) const {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (bank & 0x40) {
    if (bank & 0x80) return (memsel & 1) ? 6 : 8;
    return 8;
  }
  if (a >= 0x8000) return ((bank & 0x80) && (memsel & 1)) ? 6 : 8;
  if (a < 0x2000) return 8;
  if (a < 0x4000) return 6;
  if (a < 0x4200) return 12;
  if (a < 0x6000) return 6;
  return 8;
}

void SNES::syncApu() {
  const int64_t n = apuDebt / MASTER_HZ;
  if (n > 0) {
    apuDebt -= n * MASTER_HZ;
    apu.runCycles(n);
  }
}

void SNES::tick(int clocks) {
  while (clocks > 0) {
    int target = 1364;
    if (!refreshed) target = std::min(target, 536);
    if (!rendered) target = std::min(target, 1096);
    if (!hdmaDone) target = std::min(target, 1104);
    const int irqPos = static_cast<int>(htime) * 4 + 14;
    const bool hIrq = (nmitimen & 0x10) && htime <= 339 && irqPos > hc;
    if (hIrq) target = std::min(target, irqPos);
    if (target <= hc) target = hc + 1;
    const int n = std::min(clocks, target - hc);
    hc += n;
    clocks -= n;
    apuDebt += n * SMP_HZ;
    if (dsp) dsp->run(n);
    if (spc7110) spc7110->run(n);
    if (sa1) {
      sa1->run(n);
      if (sa1->dirty) { dirty = true; sa1->dirty = false; }
      if (sa1->snesIrq() != cpu.irqLine) updateIrq();
    }
    if (gsu && gsu->running()) {
      gsu->run(n);
      if (gsu->irq() && !cpu.irqLine) updateIrq();
    }
    if (hIrq && hc >= irqPos) {
      if (!(nmitimen & 0x20) || vc == static_cast<int>(vtime)) { irqFlag = true; updateIrq(); }
    }
    if (!refreshed && hc >= 536) { refreshed = true; clocks += 40; }  // renovação da DRAM: a CPU fica parada
    if (!rendered && hc >= 1096) {
      rendered = true;
      if (vc >= 1 && vc <= 224) ppu.renderLine(vc);
    }
    if (!hdmaDone && hc >= 1104) {
      hdmaDone = true;
      if (vc < vblankLine) clocks += hdmaRun();
    }
    if (hc >= 1364) {
      hc -= 1364;
      newLine();
      if (vc == 0) clocks += hdmaInit();
    }
  }
}

void SNES::newLine() {
  vc++;
  refreshed = rendered = hdmaDone = false;
  if (vc == 262) {
    vc = 0;
    frameDone = true;
    ppu.field = !ppu.field;
    nmiFlag = false;
    ppu.setRendering(true);
  }
  if (vc == 0) vblankLine = ppu.overscan() ? 240 : 225;  // com overscan, o vblank começa na linha 240
  if (vc == vblankLine) {
    ppu.vblankStart();
    ppu.setRendering(false);
    nmiFlag = true;
    if (nmitimen & 0x80) cpu.nmiPending = true;
    if (nmitimen & 1) autoJoypad();
  }
  if (autoJoyBusy && --autoJoyTimer <= 0) autoJoyBusy = false;
  if ((nmitimen & 0x30) == 0x20 && vc == static_cast<int>(vtime)) { irqFlag = true; updateIrq(); }
  syncApu();
}

void SNES::autoJoypad() {
  joy[0] = pad;
  joy[1] = pad2On ? pad2 : 0;
  joy[2] = joy[3] = 0;
  joyShift[0] = 0xFFFFFFFFu;  // depois da leitura automática, a leitura manual devolve 1
  joyShift[1] = 0xFFFFFFFFu;
  autoJoyBusy = true;
  autoJoyTimer = 3;
}

// ---------------------------------------------------------------- barramento
uint8_t SNES::read(uint32_t addr) {
  tick(speed(addr));
  const uint8_t v = busRead(addr, false);
  mdr = v;
  return v;
}

void SNES::write(uint32_t addr, uint8_t v) {
  tick(speed(addr));
  mdr = v;
  busWrite(addr, v, false);
}

uint8_t SNES::peek(uint32_t addr) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (!(bank & 0x40) && a < 0x2000) return wram[a];
  if ((bank & 0xFE) == 0x7E) return wram[((bank & 1) << 16) | a];
  if (sa1 && !(bank & 0x40) && a >= 0x3000 && a < 0x3800) return sa1->ioRead(a, 0);
  if (sdd1 && (bank >= 0xC0 || (!(bank & 0x40) && a >= 0x8000))) return sdd1->romPeek(addr);
  if (spc7110 && (bank == 0x50 || bank == 0x58)) return 0;  // portas com efeito colateral
  if (!(bank & 0x40) && a < 0x8000 && !(a >= 0x6000)) return 0;
  bool hit;
  const uint8_t v = cartRead(addr, hit);
  return hit ? v : 0;
}

uint8_t SNES::busRead(uint32_t addr, bool dmaA) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (!(bank & 0x40)) {
    if (a < 0x2000) return wram[a];
    if (a < 0x6000) {
      if (dmaA && ((a >= 0x2100 && a < 0x2200) || (a >= 0x4000 && a < 0x4400))) return mdr;
      return ioRead(a);
    }
    if (a < 0x8000) { bool hit; const uint8_t v = cartRead(addr, hit); return hit ? v : mdr; }
  } else if ((bank & 0xFE) == 0x7E) {
    return wram[((bank & 1) << 16) | a];
  }
  bool hit;
  const uint8_t v = cartRead(addr, hit);
  return hit ? v : mdr;
}

void SNES::busWrite(uint32_t addr, uint8_t v, bool dmaA) {
  const int bank = (addr >> 16) & 0xFF;
  const uint16_t a = static_cast<uint16_t>(addr);
  if (!(bank & 0x40)) {
    if (a < 0x2000) { wram[a] = v; return; }
    if (a < 0x6000) {
      if (dmaA && ((a >= 0x2100 && a < 0x2200) || (a >= 0x4000 && a < 0x4400))) return;
      ioWrite(a, v);
      return;
    }
  } else if ((bank & 0xFE) == 0x7E) {
    wram[((bank & 1) << 16) | a] = v;
    return;
  }
  cartWrite(addr, v);
}

void SNES::updateIrq() { cpu.irqLine = irqFlag || (gsu && gsu->irq()) || (sa1 && sa1->snesIrq()); }

uint8_t SNES::ioRead(uint16_t a) {
  if (sa1 && sa1->snesIo(a)) return sa1->ioRead(a, mdr);
  if (sdd1 && a >= 0x4800 && a < 0x4810) return sdd1->ioRead(a, mdr);
  if (spc7110 && spc7110->ownsIo(a)) return spc7110->ioRead(a, mdr);
  if (gsu && a >= 0x3000 && a < 0x3500) { const uint8_t v = gsu->readReg(a); updateIrq(); return v; }
  if (a >= 0x2100 && a < 0x2140) {
    if (a == 0x2137) {
      if (wrio & 0x80) ppu.latchCounters(hc >> 2, vc);
      return mdr;
    }
    return ppu.read(static_cast<uint8_t>(a - 0x2100), mdr);
  }
  if (a >= 0x2140 && a < 0x2180) { syncApu(); return apu.portOut(a & 3); }
  if (a == 0x2180) { const uint8_t v = wram[wramAddr]; wramAddr = (wramAddr + 1) & 0x1FFFF; return v; }
  if (a == 0x4016 || a == 0x4017) {
    const int k = a & 1;
    const uint8_t bit = static_cast<uint8_t>(joyShift[k] >> 31);
    if (!joyLatch) joyShift[k] = (joyShift[k] << 1) | 1;
    return static_cast<uint8_t>((mdr & (k ? 0xE0 : 0xFC)) | (k ? 0x1C : 0) | bit);
  }
  if (a >= 0x4200 && a < 0x4220) {
    switch (a) {
      case 0x4210: { const uint8_t v = static_cast<uint8_t>((nmiFlag ? 0x80 : 0) | (mdr & 0x70) | 0x02); nmiFlag = false; return v; }
      case 0x4211: { const uint8_t v = static_cast<uint8_t>((irqFlag ? 0x80 : 0) | (mdr & 0x7F)); irqFlag = false; updateIrq(); return v; }
      case 0x4212: {
        const bool vblank = vc >= vblankLine, hblank = hc >= 1096 || hc < 4;
        return static_cast<uint8_t>((vblank ? 0x80 : 0) | (hblank ? 0x40 : 0) | (autoJoyBusy ? 1 : 0) | (mdr & 0x3E));
      }
      case 0x4213: return wrio;
      case 0x4214: return static_cast<uint8_t>(rddiv);
      case 0x4215: return static_cast<uint8_t>(rddiv >> 8);
      case 0x4216: return static_cast<uint8_t>(rdmpy);
      case 0x4217: return static_cast<uint8_t>(rdmpy >> 8);
      default:
        if (a >= 0x4218) {
          const uint16_t j = joy[(a - 0x4218) >> 1];
          return static_cast<uint8_t>((a & 1) ? j >> 8 : j);
        }
        return mdr;
    }
  }
  if (a >= 0x4300 && a < 0x4380) {
    const Channel& d = ch[(a >> 4) & 7];
    switch (a & 0xF) {
      case 0x0: return d.ctrl;
      case 0x1: return d.bbad;
      case 0x2: return static_cast<uint8_t>(d.a1t);
      case 0x3: return static_cast<uint8_t>(d.a1t >> 8);
      case 0x4: return d.a1b;
      case 0x5: return static_cast<uint8_t>(d.das);
      case 0x6: return static_cast<uint8_t>(d.das >> 8);
      case 0x7: return d.dasb;
      case 0x8: return static_cast<uint8_t>(d.a2a);
      case 0x9: return static_cast<uint8_t>(d.a2a >> 8);
      case 0xA: return d.ntrl;
      case 0xB: case 0xF: return d.unused;
      default: return mdr;
    }
  }
  return mdr;
}

void SNES::ioWrite(uint16_t a, uint8_t v) {
  if (sa1 && sa1->snesIo(a)) { sa1->ioWrite(a, v); updateIrq(); return; }
  if (sdd1) {
    if (a >= 0x4800 && a < 0x4810) { sdd1->ioWrite(a, v); return; }
    if (a >= 0x4300 && a < 0x4380) sdd1->dmaRegWrite(a, v);  // o chip espia e o registrador grava normalmente
  }
  if (spc7110 && spc7110->ownsIo(a)) { spc7110->ioWrite(a, v); return; }
  if (gsu && a >= 0x3000 && a < 0x3500) { gsu->writeReg(a, v); updateIrq(); return; }
  if (a >= 0x2100 && a < 0x2140) { ppu.write(static_cast<uint8_t>(a - 0x2100), v); return; }
  if (a >= 0x2140 && a < 0x2180) { syncApu(); apu.portIn(a & 3, v); return; }
  switch (a) {
    case 0x2180: wram[wramAddr] = v; wramAddr = (wramAddr + 1) & 0x1FFFF; return;
    case 0x2181: wramAddr = (wramAddr & 0x1FF00) | v; return;
    case 0x2182: wramAddr = (wramAddr & 0x100FF) | (v << 8); return;
    case 0x2183: wramAddr = (wramAddr & 0x0FFFF) | ((v & 1) << 16); return;
    case 0x4016:
      joyLatch = v & 1;
      // porta 2 sem controle devolve 0; com controle, 1 depois dos 16 bits
      if (joyLatch) { joyShift[0] = (uint32_t(pad) << 16) | 0xFFFF; joyShift[1] = pad2On ? (uint32_t(pad2) << 16) | 0xFFFF : 0; }
      return;
    case 0x4200: {
      const uint8_t old = nmitimen;
      nmitimen = v;
      if (!(old & 0x80) && (v & 0x80) && nmiFlag) cpu.nmiPending = true;
      if (!(v & 0x30)) { irqFlag = false; updateIrq(); }
      return;
    }
    case 0x4201:
      if ((wrio & 0x80) && !(v & 0x80)) ppu.latchCounters(hc >> 2, vc);
      wrio = v;
      return;
    case 0x4202: wrmpya = v; return;
    case 0x4203: rdmpy = static_cast<uint16_t>(wrmpya * v); rddiv = v; return;
    case 0x4204: wrdivl = v; return;
    case 0x4205: wrdivh = v; return;
    case 0x4206: {
      const uint16_t dividend = static_cast<uint16_t>((wrdivh << 8) | wrdivl);
      if (!v) { rddiv = 0xFFFF; rdmpy = dividend; }
      else { rddiv = static_cast<uint16_t>(dividend / v); rdmpy = static_cast<uint16_t>(dividend % v); }
      return;
    }
    case 0x4207: htime = static_cast<uint16_t>((htime & 0x100) | v); return;
    case 0x4208: htime = static_cast<uint16_t>((htime & 0xFF) | ((v & 1) << 8)); return;
    case 0x4209: vtime = static_cast<uint16_t>((vtime & 0x100) | v); return;
    case 0x420A: vtime = static_cast<uint16_t>((vtime & 0xFF) | ((v & 1) << 8)); return;
    case 0x420B: runDma(v); return;
    case 0x420C: hdmaen = v; return;
    case 0x420D: memsel = v & 1; return;
    default: break;
  }
  if (a >= 0x4300 && a < 0x4380) {
    Channel& d = ch[(a >> 4) & 7];
    switch (a & 0xF) {
      case 0x0: d.ctrl = v; break;
      case 0x1: d.bbad = v; break;
      case 0x2: d.a1t = static_cast<uint16_t>((d.a1t & 0xFF00) | v); break;
      case 0x3: d.a1t = static_cast<uint16_t>((d.a1t & 0x00FF) | (v << 8)); break;
      case 0x4: d.a1b = v; break;
      case 0x5: d.das = static_cast<uint16_t>((d.das & 0xFF00) | v); break;
      case 0x6: d.das = static_cast<uint16_t>((d.das & 0x00FF) | (v << 8)); break;
      case 0x7: d.dasb = v; break;
      case 0x8: d.a2a = static_cast<uint16_t>((d.a2a & 0xFF00) | v); break;
      case 0x9: d.a2a = static_cast<uint16_t>((d.a2a & 0x00FF) | (v << 8)); break;
      case 0xA: d.ntrl = v; break;
      case 0xB: case 0xF: d.unused = v; break;
      default: break;
    }
  }
}

// ---------------------------------------------------------------- DMA e HDMA
void SNES::runDma(uint8_t mask) {
  if (!mask) return;
  for (int c = 0; c < 8; c++) if ((mask >> c) & 1) ch[c].dmaEnable = true;
  tick(8);
  for (int c = 0; c < 8; c++) {
    Channel& d = ch[c];
    if (!d.dmaEnable) continue;
    tick(8);
    const int mode = d.ctrl & 7;
    const bool toA = d.ctrl & 0x80;
    int i = 0;
    do {
      const uint16_t b = static_cast<uint16_t>(0x2100 | ((d.bbad + DMA_OFF[mode][i & 3]) & 0xFF));
      const uint32_t aa = (uint32_t(d.a1b) << 16) | d.a1t;
      if (!toA) busWrite(b, busRead(aa, true), false);
      else busWrite(aa, busRead(b, false), true);
      tick(8);
      if (!(d.ctrl & 0x08)) d.a1t = static_cast<uint16_t>(d.a1t + ((d.ctrl & 0x10) ? -1 : 1));
      d.das--;
      i++;
    } while (d.dmaEnable && d.das);
    d.dmaEnable = false;
  }
}

int SNES::hdmaInit() {
  int cost = 0;
  for (int c = 0; c < 8; c++) {
    Channel& d = ch[c];
    d.doTransfer = false;
    d.done = false;
    if (!((hdmaen >> c) & 1)) continue;
    d.dmaEnable = false;  // o HDMA para o DMA comum do mesmo canal no meio
    d.a2a = d.a1t;
    d.ntrl = dmaRead((uint32_t(d.a1b) << 16) | d.a2a);
    d.a2a++;
    cost += 8;
    if (d.ctrl & 0x40) {
      const uint8_t lo = dmaRead((uint32_t(d.a1b) << 16) | d.a2a++);
      const uint8_t hi = dmaRead((uint32_t(d.a1b) << 16) | d.a2a++);
      d.das = static_cast<uint16_t>(lo | (hi << 8));
      cost += 16;
    }
    d.done = d.ntrl == 0;
    d.doTransfer = true;
  }
  if (hdmaen) cost += 18;
  return cost;
}

int SNES::hdmaRun() {
  if (!hdmaen) return 0;
  int cost = 18;
  for (int c = 0; c < 8; c++) {
    Channel& d = ch[c];
    if (!((hdmaen >> c) & 1) || d.done) continue;
    d.dmaEnable = false;
    if (!d.doTransfer) continue;
    const int mode = d.ctrl & 7;
    for (int i = 0; i < HDMA_LEN[mode]; i++) {
      uint32_t src;
      if (d.ctrl & 0x40) src = (uint32_t(d.dasb) << 16) | d.das++;
      else src = (uint32_t(d.a1b) << 16) | d.a2a++;
      busWrite(static_cast<uint16_t>(0x2100 | ((d.bbad + DMA_OFF[mode][i]) & 0xFF)), dmaRead(src), false);
      cost += 8;
    }
  }
  for (int c = 0; c < 8; c++) {
    Channel& d = ch[c];
    if (!((hdmaen >> c) & 1) || d.done) continue;
    d.ntrl--;
    d.doTransfer = d.ntrl & 0x80;
    if (!(d.ntrl & 0x7F)) {
      d.ntrl = dmaRead((uint32_t(d.a1b) << 16) | d.a2a);
      d.a2a++;
      cost += 8;
      if (d.ctrl & 0x40) {
        const uint8_t lo = dmaRead((uint32_t(d.a1b) << 16) | d.a2a++);
        const uint8_t hi = dmaRead((uint32_t(d.a1b) << 16) | d.a2a++);
        d.das = static_cast<uint16_t>(lo | (hi << 8));
        cost += 16;
      }
      if (!d.ntrl) d.done = true;
      d.doTransfer = true;
    }
  }
  return cost;
}

// ---------------------------------------------------------------- estados
void SNES::sync(StateIO& io) {
  cpu.sync(io);
  ppu.sync(io);
  apu.sync(io);
  io.raw(wram, sizeof wram);
  io.bytes(cart.sram);
  io.sync(wramAddr); io.sync(mdr); io.sync(hc); io.sync(vc); io.sync(refreshed); io.sync(rendered); io.sync(hdmaDone);
  io.sync(apuDebt);
  io.sync(vblankLine);
  if (gsu) gsu->sync(io);
  if (dsp) dsp->sync(io);
  if (sa1) sa1->sync(io);
  if (sdd1) sdd1->sync(io);
  if (spc7110) spc7110->sync(io);
  io.sync(nmitimen); io.sync(wrio); io.sync(memsel); io.sync(hdmaen); io.sync(wrmpya); io.sync(wrdivl); io.sync(wrdivh);
  io.sync(htime); io.sync(vtime); io.sync(rddiv); io.sync(rdmpy);
  io.sync(nmiFlag); io.sync(irqFlag); io.sync(autoJoyBusy); io.sync(autoJoyTimer);
  io.raw(joy, sizeof joy); io.sync(joyLatch); io.raw(joyShift, sizeof joyShift);
  for (auto& d : ch) {
    io.sync(d.ctrl); io.sync(d.bbad); io.sync(d.a1b); io.sync(d.dasb); io.sync(d.ntrl); io.sync(d.unused);
    io.sync(d.a1t); io.sync(d.das); io.sync(d.a2a); io.sync(d.doTransfer); io.sync(d.done);
  }
  if (io.reading()) {
    wramAddr &= 0x1FFFF;
    clampTo(hc, 0, 1363); clampTo(vc, 0, 261);
    clampTo<int64_t>(apuDebt, 0, MASTER_HZ * 64);
    htime &= 0x1FF; vtime &= 0x1FF;
    clampTo(autoJoyTimer, 0, 3);
    if (vblankLine != 240) vblankLine = 225;
  }
}

std::vector<uint8_t> SNES::saveState() {
  StateIO io;
  uint32_t magic = 0x31534E53;  // "SNS1"
  io.sync(magic);
  sync(io);
  return io.data();
}

void SNES::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint32_t magic = 0;
  io.sync(magic);
  if (magic != 0x31534E53) throw std::runtime_error("Este estado não é de SNES.");
  const std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
  } catch (...) {
    StateIO restore(backup);
    restore.sync(magic);
    sync(restore);
    throw;
  }
}

// ---------------------------------------------------------------- núcleo
namespace {
Cart parseOrThrow(const std::vector<uint8_t>& rom) {
  Cart c;
  std::string err;
  if (!parseRom(rom, c, err)) throw std::runtime_error(err);
  return c;
}
}  // namespace

SNESCore::SNESCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : m_(std::make_unique<SNES>(parseOrThrow(rom))) {
  if (save) importSave(*save);
}

void SNESCore::importSave(const std::vector<uint8_t>& b) {
  if (b.empty() || m_->cart.sram.empty()) return;
  auto& r = m_->cart.sram;
  std::memcpy(r.data(), b.data(), std::min(r.size(), b.size()));
  m_->dirty = false;
}

}  // namespace indigo::snes
