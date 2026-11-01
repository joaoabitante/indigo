#include "ngp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <stdexcept>
#include <utility>

namespace indigo::ngp {

namespace {
// mesmos bits de Key em core.h
enum { K_A = 0, K_B = 1, K_SELECT = 2, K_START = 3, K_RIGHT = 4, K_LEFT = 5, K_UP = 6, K_DOWN = 7 };
constexpr int LINE = 515;
constexpr double T1 = 256, T4 = 1024, T16 = 4096, T256 = 65536;  // passos dos prescalers (em ciclos de CPU)
const char STATE_MAGIC[4] = {'I', 'N', 'G', '1'};

// ------------------------------------------------------------ BIOS própria (src/ngp/bios.js)
constexpr int CALLS = 0x1B;               // VECT_SHUTDOWN … VECT_COMGETBUFDATA
constexpr uint32_t STUB_BASE = 0xFF1000;  // trecho de cada chamada: STUB_BASE + n*16
constexpr uint32_t SWI1_STUB = 0xFF0F00;  // SWI 1: chamada do sistema pelo número em W3
constexpr uint32_t DEFAULT_IRQ = 0xFF23DF;  // tratador padrão de interrupção (RETI)

// fonte própria do Índigo: 8 bytes por caractere, bit 7 = coluna da esquerda
const std::pair<char, const char*> GLYPHS[] = {
  {' ', "0000000000000000"}, {'!', "1010101010001000"}, {'"', "2828000000000000"}, {'#', "287C28287C280000"},
  {'$', "103C503814781000"}, {'%', "64640810204C4C00"}, {'&', "3048305448340000"}, {'\'', "1010000000000000"},
  {'(', "0810202020100800"}, {')', "2010080808102000"}, {'*', "0028107C10280000"}, {'+', "0010107C10100000"},
  {',', "0000000000101020"}, {'-', "0000007C00000000"}, {'.', "0000000000303000"}, {'/', "0404081020404000"},
  {'0', "38444C5464443800"}, {'1', "1030101010103800"}, {'2', "3844041820407C00"}, {'3', "7C08100804443800"},
  {'4', "081828487C080800"}, {'5', "7C40780404443800"}, {'6', "1820407844443800"}, {'7', "7C04081020202000"},
  {'8', "3844443844443800"}, {'9', "3844443C04083000"}, {':', "0030300030300000"}, {';', "0030300030102000"},
  {'<', "0810204020100800"}, {'=', "00007C007C000000"}, {'>', "2010080408102000"}, {'?', "3844040810001000"},
  {'@', "38445C545C403800"}, {'A', "3844447C44444400"}, {'B', "7844447844447800"}, {'C', "3844404040443800"},
  {'D', "7048444444487000"}, {'E', "7C40407840407C00"}, {'F', "7C40407840404000"}, {'G', "3844405C44443C00"},
  {'H', "4444447C44444400"}, {'I', "3810101010103800"}, {'J', "1C08080808483000"}, {'K', "4448506050484400"},
  {'L', "4040404040407C00"}, {'M', "446C545444444400"}, {'N', "444464544C444400"}, {'O', "3844444444443800"},
  {'P', "7844447840404000"}, {'Q', "3844444454483400"}, {'R', "7844447850484400"}, {'S', "3C40403804047800"},
  {'T', "7C10101010101000"}, {'U', "4444444444443800"}, {'V', "4444444444281000"}, {'W', "4444445454542800"},
  {'X', "4444281028444400"}, {'Y', "4444281010101000"}, {'Z', "7C04081020407C00"}, {'[', "3820202020203800"},
  {'\\', "4040201008040400"}, {']', "3808080808083800"}, {'^', "1028440000000000"}, {'_', "0000000000007C00"},
  {'`', "2010080000000000"}, {'a', "000038043C443C00"}, {'b', "4040586444447800"}, {'c', "0000384040443800"},
  {'d', "0404344C44443C00"}, {'e', "000038447C403800"}, {'f', "1824207020202000"}, {'g', "00003C44443C0438"},
  {'h', "4040586444444400"}, {'i', "1000301010103800"}, {'j', "0800180808084830"}, {'k', "4040485060504800"},
  {'l', "3010101010103800"}, {'m', "0000685454544400"}, {'n', "0000586444444400"}, {'o', "0000384444443800"},
  {'p', "0000784444784040"}, {'q', "0000344C443C0404"}, {'r', "0000586440404000"}, {'s', "00003C4038047800"},
  {'t', "2020702020241800"}, {'u', "00004444444C3400"}, {'v', "0000444444281000"}, {'w', "0000444454542800"},
  {'x', "0000442810284400"}, {'y', "00004444443C0438"}, {'z', "00007C0810207C00"}, {'{', "0C10102010100C00"},
  {'|', "1010101010101000"}, {'}', "6010100810106000"}, {'~', "0000205408000000"},
};

struct Font {
  uint8_t f[0x800] = {};
  Font() {
    auto hexv = [](char c) { return c <= '9' ? c - '0' : c - 'A' + 10; };
    for (const auto& [ch, hex] : GLYPHS) {
      const int c = static_cast<unsigned char>(ch);
      for (int r = 0; r < 8; r++) f[c * 8 + r] = static_cast<uint8_t>(hexv(hex[r * 2]) * 16 + hexv(hex[r * 2 + 1]));
    }
    for (int r = 0; r < 8; r++) f[8 + r] = 0xFF;  // bloco cheio no caractere 1 (molduras/cursores)
  }
};
const Font FONT;

std::vector<uint8_t> buildBios() {
  std::vector<uint8_t> b(0x10000, 0);
  auto put32 = [&](uint32_t o, uint32_t v) { b[o] = v & 0xFF; b[o + 1] = (v >> 8) & 0xFF; b[o + 2] = (v >> 16) & 0xFF; b[o + 3] = (v >> 24) & 0xFF; };
  for (int n = 0; n < CALLS; n++) {
    const uint32_t a = STUB_BASE + n * 16;
    put32(0xFE00 + n * 4, a);
    b[a & 0xFFFF] = 0x1F;
  }
  // vetores de SWI / interrupções do hardware: SWI 1 = chamada do sistema; o resto volta direto
  for (int n = 0; n < 64; n++) put32(0xFF00 + n * 4, DEFAULT_IRQ);
  put32(0xFF04, SWI1_STUB);
  b[SWI1_STUB & 0xFFFF] = 0x1F;
  b[DEFAULT_IRQ & 0xFFFF] = 0x07;  // RETI
  b[0x23C3] = 0x07;
  b[0xFFFE] = 0x68; b[0xFFFF] = 0xFE;  // JR $ (sem cartucho)
  return b;
}

// tamanhos dos blocos da flash (chip com o bloco de partida no topo): 64 KB… depois 32K, 8K, 8K, 16K
std::vector<std::pair<int64_t, int64_t>> flashBlocks(int64_t chipSize) {
  std::vector<std::pair<int64_t, int64_t>> out;
  int64_t a = 0;
  while (a < chipSize - 0x10000) { out.push_back({a, 0x10000}); a += 0x10000; }
  out.push_back({a, 0x8000}); out.push_back({a + 0x8000, 0x2000}); out.push_back({a + 0xA000, 0x2000}); out.push_back({a + 0xC000, 0x4000});
  return out;
}

const ClockTime* g_fixedClock = nullptr;
ClockTime g_fixedStore;

ClockTime nowClock() {
  if (g_fixedClock) return *g_fixedClock;
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  return {tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday};
}
}  // namespace

void setFixedClock(const ClockTime* t) {
  if (t) { g_fixedStore = *t; g_fixedClock = &g_fixedStore; } else g_fixedClock = nullptr;
}

// ------------------------------------------------------------ cabeçalho (src/ngp/header.js)
bool isNgpRom(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 0x40) return false;
  const std::string s(bytes.begin(), bytes.begin() + 28);
  return s == "COPYRIGHT BY SNK CORPORATION" || s == " LICENSED BY SNK CORPORATION";
}

NgpInfo ngpInfo(const std::vector<uint8_t>& h) {
  NgpInfo i;
  i.catalog = h[0x20] | (h[0x21] << 8);
  i.sub = h[0x22];
  std::string t;
  for (int k = 0; k < 12; k++) { const int c = h[0x24 + k]; t += (c >= 32 && c < 127) ? static_cast<char>(c) : ' '; }
  const auto b = t.find_first_not_of(" \t\r\n"), e = t.find_last_not_of(" \t\r\n");
  i.title = b == std::string::npos ? "" : t.substr(b, e - b + 1);
  i.start = h[0x1C] | (h[0x1D] << 8) | (h[0x1E] << 16);
  i.color = h[0x23] == 0x10;
  char buf[32];
  std::snprintf(buf, sizeof buf, "NGP-%04X-%02X", i.catalog, i.sub);
  i.gameCode = buf;
  return i;
}

// ------------------------------------------------------------ console
NGP::NGP() : cpu(*this), gfx(*this), z80bus(*this), z80(z80bus), bios(buildBios()), ram(0x4000) {
  cpu.onBios = [this] { onBios(); };
}

int NGP::Z80Side::read(int a) {
  if (a < 0x1000) return n.ram[0x3000 + a];
  if (a == 0x8000) return n.comm;
  return 0;
}
void NGP::Z80Side::write(int a, int v) {
  if (a < 0x1000) { n.ram[0x3000 + a] = static_cast<uint8_t>(v); return; }
  if (a == 0x8000) n.comm = v;
  else if (a == 0x4001) { n.apu.run(n.cycles); n.apu.writeLeft(v); }
  else if (a == 0x4000) { n.apu.run(n.cycles); n.apu.writeRight(v); }
  else if (a == 0xC000) n.request(6, 0x0C);
}
void NGP::Z80Side::out(int, int) { n.z80.irqLine = false; }

void NGP::loadRom(const std::vector<uint8_t>& data) {
  const size_t len = std::max<size_t>(data.size(), 1);
  size_t pow2 = 1;
  while (pow2 < len) pow2 <<= 1;
  const size_t size = std::min<size_t>(std::max<size_t>(0x80000, pow2), 0x400000);
  rom.assign(size, 0xFF);
  std::copy(data.begin(), data.begin() + std::min(data.size(), size), rom.begin());
  orig = rom;
  const NgpInfo h = ngpInfo(rom);
  start = h.start; catalog = h.catalog; subCode = h.sub; color = h.color;
  title = h.title; gameCode = h.gameCode;
  reset();
}

int64_t NGP::chipSize(int64_t base) const {
  const int64_t total = static_cast<int64_t>(rom.size());
  if (base == 0x200000) return std::min<int64_t>(total, 0x200000);
  return std::max<int64_t>(0, total - 0x200000);
}

void NGP::reset() {
  gfx.power();
  apu.reset();
  std::fill(ram.begin(), ram.end(), 0);
  z80.reset(); z80on = false; z80time = 0;
  comm = 0; sc0buf = 0; commStatus = 0;
  std::fill(std::begin(intPrio), std::end(intPrio), 0);
  std::fill(std::begin(pending), std::end(pending), 0);
  std::fill(std::begin(hdma), std::end(hdma), 0);
  std::fill(std::begin(timer), std::end(timer), 0);
  std::fill(std::begin(thresh), std::end(thresh), 0);
  std::fill(std::begin(tclk), std::end(tclk), 0.0);
  trun = 0; t01mod = 0; t23mod = 0; trdc = 0; tffcr = 0;
  hint = false; lineCycles = 0; cycles = 0;
  flashCmd = 0; flashId = false;
  requestReset = false;
  irqCheck = true;
  // registros internos depois da partida (estado em que a BIOS entrega o controle ao jogo)
  static const std::pair<int, int> SFR_BOOT[] = {{0x20, 0x80}, {0x22, 0x01}, {0x23, 0x90}, {0x24, 0x03}, {0x25, 0xB0}, {0x26, 0x90},
                                                 {0x27, 0x62}, {0x28, 0x05}, {0x70, 0x02}, {0x71, 0x32}, {0xB8, 0xAA}, {0xB9, 0xAA}};
  for (const auto& [a, v] : SFR_BOOT) put8(a, v);
  // área de trabalho da BIOS
  auto w8 = [this](int a, int v) { ram[a - 0x4000] = static_cast<uint8_t>(v & 0xFF); };
  auto w16 = [&](int a, int v) { w8(a, v); w8(a + 1, v >> 8); };
  auto w32 = [&](int a, uint32_t v) { w16(a, v & 0xFFFF); w16(a + 2, static_cast<int>(v >> 16)); };
  w32(0x6C00, start); w16(0x6C04, catalog); w16(0x6E82, catalog); w8(0x6C06, subCode); w8(0x6E84, subCode);
  for (int i = 0; i < 12; i++) w8(0x6C08 + i, rom[0x24 + i]);
  w8(0x6C58, 1); w8(0x6C59, rom.size() > 0x200000 ? 1 : 0); w8(0x6C55, 1);
  w8(0x6F80, 0xFF); w8(0x6F81, 0x03);                  // bateria cheia
  w8(0x6F84, 0x40); w8(0x6F85, 0); w8(0x6F86, 0);      // partida normal
  w8(0x6F87, 1);                                       // idioma: inglês
  w8(0x6F91, color ? 0x10 : 0); w8(0x6F95, color ? 0x10 : 0);
  for (int i = 0; i < 0x12; i++) w32(0x6FB8 + i * 4, DEFAULT_IRQ);
  // vídeo como a BIOS deixa
  K2GE& g = gfx;
  g.write8(0x8000, 0xC0); g.write8(0x8002, 0); g.write8(0x8003, 0); g.write8(0x8004, 0xFF); g.write8(0x8005, 0xFF);
  g.write8(0x8006, 0xC6); g.write8(0x8012, 0); g.write8(0x8118, 0x80);
  g.write8(0x83E0, 0xFF); g.write8(0x83E1, 0x0F); g.write8(0x83F0, 0xFF); g.write8(0x83F1, 0x0F);
  g.write8(0x87E2, color ? 0 : 0x80);
  g.latch();
  // CPU
  cpu.reset();
  cpu.pc = start & 0xFFFFFF;
  cpu.sr = 0xF800;
  cpu.r[16] = 0xFF23C3; cpu.r[17] = 0xFF23DF; cpu.r[18] = 0x006480; cpu.r[19] = 0x006C00;
  frame = 0;
}

// ------------------------------------------------------------ barramento da CPU principal
int NGP::mem8(int64_t a) {
  if (a >= 0x200000) {
    if (a < 0x400000) {
      if (flashId) return flashIdRead(a);
      const int64_t o = a - 0x200000;
      return o < static_cast<int64_t>(rom.size()) ? rom[o] : 0xFF;
    }
    if (a >= 0xFF0000) return bios[a & 0xFFFF];
    if (a >= 0x800000 && a < 0xA00000) {
      const int64_t o = a - 0x800000 + 0x200000;
      return o < static_cast<int64_t>(rom.size()) ? rom[o] : 0xFF;
    }
    return 0;
  }
  if (a >= 0x4000 && a < 0x8000) return ram[a - 0x4000];
  if (a >= 0x8000 && a < 0xC000) return gfx.read8(static_cast<uint32_t>(a));
  if (a < 0x100) return io8(static_cast<int>(a));
  return 0;
}

int NGP::mem16(int64_t a) {
  if (a >= 0x200000 && a < 0x3FFFFF && !flashId) {
    const int64_t o = a - 0x200000;
    return o + 1 < static_cast<int64_t>(rom.size()) ? rom[o] | (rom[o + 1] << 8) : 0xFFFF;
  }
  if (a >= 0x4000 && a < 0x7FFF) { const int64_t o = a - 0x4000; return ram[o] | (ram[o + 1] << 8); }
  const int lo = mem8(a);
  const int hi = mem8((a + 1) & 0xFFFFFF);
  return lo | (hi << 8);
}

uint32_t NGP::mem32(int64_t a) {
  const uint32_t lo = static_cast<uint32_t>(mem16(a));
  const uint32_t hi = static_cast<uint32_t>(mem16((a + 2) & 0xFFFFFF));
  return lo | (hi << 16);
}

void NGP::put8(int64_t a, int v) {
  if (a >= 0x4000 && a < 0x8000) { ram[a - 0x4000] = static_cast<uint8_t>(v); return; }
  if (a >= 0x8000 && a < 0xC000) { gfx.write8(static_cast<uint32_t>(a), v); return; }
  if (a < 0x100) { ioWrite8(static_cast<int>(a), v); return; }
  if ((a >= 0x200000 && a < 0x400000) || (a >= 0x800000 && a < 0xA00000)) flashWrite(a, v);
}

void NGP::put16(int64_t a, int v) {
  if (a >= 0x4000 && a < 0x7FFF) { const int64_t o = a - 0x4000; ram[o] = v & 0xFF; ram[o + 1] = static_cast<uint8_t>(v >> 8); return; }
  if (a == 0xB8) {  // controle conjunto do som (byte baixo) e do Z80 (byte alto)
    ioWrite8(0xB8, v & 0xFF); ioWrite8(0xB9, v >> 8); return;
  }
  put8(a, v & 0xFF); put8((a + 1) & 0xFFFFFF, (v >> 8) & 0xFF);
}

void NGP::put32(int64_t a, uint32_t v) {
  put16(a, static_cast<int>(v & 0xFFFF));
  put16((a + 2) & 0xFFFFFF, static_cast<int>(v >> 16));
}

// ------------------------------------------------------------ registros internos (0000–00FF)
int NGP::io8(int a) {
  if (a >= 0x70 && a <= 0x7F) {
    switch (a) {
      case 0x71: return (pending[5] ? 0x08 : 0) | (pending[6] ? 0x80 : 0);
      case 0x73: return (pending[7] ? 0x08 : 0) | (pending[8] ? 0x80 : 0);
      case 0x74: return (pending[9] ? 0x08 : 0) | (pending[10] ? 0x80 : 0);
      case 0x77: return (pending[11] ? 0x08 : 0) | (pending[12] ? 0x80 : 0);
      default: return 0;
    }
  }
  if (a >= 0x90 && a <= 0x97) return rtc(a);
  if (a >= 0x20 && a <= 0x29) return a == 0x20 ? trun : a == 0x29 ? trdc : 0x04;
  switch (a) {
    case 0x50: return sc0buf;
    case 0xB0: return keys & 0x7F;
    case 0xB1: return 0x02;  // sem pedido de desligar
    case 0xBC: return comm;
    default: return 0;
  }
}

void NGP::ioWrite8(int a, int v) {
  if (a >= 0x70 && a <= 0x7F) {
    switch (a) {
      case 0x71: if (!(v & 0x08)) pending[5] = 0; if (!(v & 0x80)) pending[6] = 0; break;
      case 0x73: if (!(v & 0x08)) pending[7] = 0; if (!(v & 0x80)) pending[8] = 0; break;
      case 0x74: if (!(v & 0x08)) pending[9] = 0; if (!(v & 0x80)) pending[10] = 0; break;
      case 0x77: if (!(v & 0x08)) pending[11] = 0; if (!(v & 0x80)) pending[12] = 0; break;
      case 0x7C: case 0x7D: case 0x7E: case 0x7F: hdma[a - 0x7C] = static_cast<uint8_t>(v); break;
    }
    if (a <= 0x7A) { intPrio[a - 0x70] = static_cast<uint8_t>(v); irqCheck = true; }
    return;
  }
  if (a >= 0x20 && a <= 0x29) {
    switch (a) {
      case 0x20: trun = v; for (int t = 0; t < 4; t++) if (!(v & (1 << t))) timer[t] = 0; break;
      case 0x22: thresh[0] = static_cast<uint8_t>(v); break;
      case 0x23: thresh[1] = static_cast<uint8_t>(v); break;
      case 0x24: t01mod = v; break;
      case 0x25: tffcr = v & 0x33; break;
      case 0x26: thresh[2] = static_cast<uint8_t>(v); break;
      case 0x27: thresh[3] = static_cast<uint8_t>(v); break;
      case 0x28: t23mod = v; break;
      case 0x29: trdc = v & 3; break;
    }
    return;
  }
  if (a >= 0xA0 && a <= 0xA3) {
    apu.run(cycles);
    if (a == 0xA1 && !z80on) apu.writeLeft(v);
    else if (a == 0xA0 && !z80on) apu.writeRight(v);
    else if (a == 0xA2) apu.dacL = v;
    else if (a == 0xA3) apu.dacR = v;
    return;
  }
  switch (a) {
    case 0x50: sc0buf = v; break;
    case 0xB2: commStatus = v & 1; break;
    case 0xB8:
      if (v == 0x55) { apu.run(cycles); apu.setEnabled(true); }
      else if (v == 0xAA) { apu.run(cycles); apu.setEnabled(false); }
      break;
    case 0xB9: if (v == 0x55) z80on = true; else if (v == 0xAA) { z80on = false; z80.reset(); } break;
    case 0xBA: z80.nmiPending = true; break;
    case 0xBC: comm = v; break;
    default: break;
  }
}

int NGP::rtc(int a) {
  if (a == 0x90) return 0;
  if (a == 0x91 || !rtcLatched) {
    const ClockTime d = nowClock();
    auto bcd = [](int n) { return ((n / 10) << 4) | (n % 10); };
    const int y = bcd(d.year % 100);
    const int vals[7] = {y, bcd(d.month), bcd(d.day), bcd(d.hour), bcd(d.minute), bcd(d.second), ((y % 4) << 4) | d.weekday};
    std::copy(vals, vals + 7, rtcLatch);
    rtcLatched = true;
  }
  return rtcLatch[a - 0x91];
}

// ------------------------------------------------------------ flash do cartucho (save)
void NGP::flashWrite(int64_t a, int v) {
  const int64_t base = a >= 0x800000 ? 0x800000 : 0x200000, off = a - base, lo = off & 0x7FFF;
  const int st = flashCmd;
  if (st == 0 && lo == 0x5555 && v == 0xAA) { flashCmd = 1; return; }
  if (st == 1 && lo == 0x2AAA && v == 0x55) { flashCmd = 2; return; }
  if (st == 2 && lo == 0x5555) {
    if (v == 0xA0) { flashCmd = 3; return; }
    if (v == 0x80) { flashCmd = 4; return; }
    if (v == 0x90) { flashId = true; flashCmd = 0; return; }
    if (v == 0xF0) { flashId = false; flashCmd = 0; return; }
  }
  if (st == 3) { flashProgram(a, v); flashCmd = 0; return; }
  if (st == 4 && lo == 0x5555 && v == 0xAA) { flashCmd = 5; return; }
  if (st == 5 && lo == 0x2AAA && v == 0x55) { flashCmd = 6; return; }
  if (st == 6) {
    if (v == 0x10) flashErase(base, 0, chipSize(base));
    else if (v == 0x30) {
      for (const auto& [s, n] : flashBlocks(chipSize(base))) if (off >= s && off < s + n) { flashErase(base, s, n); break; }
    }
    flashCmd = 0; return;
  }
  if (v == 0xF0) flashId = false;
  flashCmd = 0;
}

int NGP::flashIdRead(int64_t a) {
  const int lo = static_cast<int>(a & 0xFF);
  if (lo == 0) return 0x98;  // fabricante (Toshiba)
  if (lo == 1) { const int64_t n = chipSize(0x200000); return n > 0x100000 ? 0x2F : n > 0x80000 ? 0x2C : 0xAB; }
  if (lo == 2) return 0x02;
  return 0x80;
}

void NGP::flashProgram(int64_t a, int v) {
  const int64_t o = a >= 0x800000 ? a - 0x800000 + 0x200000 : a - 0x200000;
  if (o < 0 || o >= static_cast<int64_t>(rom.size())) return;
  const uint8_t nv = rom[o] & v;
  if (nv != rom[o]) { rom[o] = nv; dirty = true; }
}

void NGP::flashErase(int64_t base, int64_t off, int64_t len) {
  const int64_t o = (base == 0x800000 ? 0x200000 : 0) + off;
  const int64_t end = std::min<int64_t>(o + len, static_cast<int64_t>(rom.size()));
  for (int64_t i = o; i < end; i++) if (rom[i] != 0xFF) { rom[i] = 0xFF; dirty = true; }
}

// ------------------------------------------------------------ BIOS própria
void NGP::onBios() {
  const uint32_t at = (cpu.pc - 1) & 0xFFFFFF;
  if (at == SWI1_STUB) {
    const int n = cpu.getB(0x31) & 0x1F;
    if (biosCall(n)) { cpu.pc = at; cpu.halted = true; return; }
    cpu.sr = static_cast<int>(cpu.pop(2)); cpu.pc = cpu.pop(4) & 0xFFFFFF; irqCheck = true;  // volta como RETI
    cpu.cycles = 16; return;
  }
  if (at >= STUB_BASE && at < STUB_BASE + CALLS * 16 && !(at & 15)) {
    if (biosCall(static_cast<int>((at - STUB_BASE) >> 4))) { cpu.pc = at; cpu.halted = true; return; }
    cpu.pc = cpu.pop(4) & 0xFFFFFF;
    cpu.cycles = 8; return;
  }
  // 0x1F fora da BIOS: instrução inválida, segue adiante
}

int NGP::biosCall(int n) {
  constexpr int RA3 = 0x30, RC3 = 0x34, RB3 = 0x35, XDE3 = 0x38, XHL3 = 0x3C;
  switch (n) {
    case 0x00: return 1;  // VECT_SHUTDOWN
    case 0x01: break;     // VECT_CLOCKGEARSET
    case 0x02: {          // VECT_RTCGET
      const int64_t d = cpu.getL(XHL3);
      if (d < 0xC000) for (int i = 0; i < 7; i++) { const int v = mem8(0x91 + i); put8(d + i, v); }
      break;
    }
    case 0x04: {  // VECT_INTLVSET (RC3 = fonte, RB3 = nível)
      const int lv = cpu.getB(RB3) & 7, src = static_cast<int>(cpu.getB(RC3));
      static const int MAP[10][2] = {{0x70, 0}, {0x71, 4}, {0x73, 0}, {0x73, 4}, {0x74, 0}, {0x74, 4}, {0x79, 0}, {0x79, 4}, {0x7A, 0}, {0x7A, 4}};
      if (src < 10) {
        const int reg = MAP[src][0], sh = MAP[src][1];
        put8(reg, (intPrio[reg - 0x70] & ~(0x0F << sh)) | (lv << sh));
      }
      break;
    }
    case 0x05: {  // VECT_SYSFONTSET: RA3 = cor do traço (bits 0–1) e do fundo (bits 4–5)
      const int v = static_cast<int>(cpu.getB(RA3)), fg = v & 3, bg = (v >> 4) & 3;
      for (int i = 0; i < 0x800; i++) {
        const int c = FONT.f[i];
        int w = 0;
        for (int k = 0; k < 8; k++) w |= ((c & (0x80 >> k)) ? fg : bg) << (14 - k * 2);
        gfx.chr[i * 2] = static_cast<uint8_t>(w & 0xFF); gfx.chr[i * 2 + 1] = static_cast<uint8_t>(w >> 8);
      }
      break;
    }
    case 0x06: {  // VECT_FLASHWRITE: RA3 banco, BC3 blocos de 256, XHL3 origem, XDE3 destino
      const int64_t base = cpu.getB(RA3) == 1 ? 0x800000 : 0x200000;
      const int64_t count = int64_t(cpu.getW(RC3)) * 256, src = cpu.getL(XHL3), dst = cpu.getL(XDE3);
      for (int64_t i = 0; i < count; i++) { const int v = mem8(src + i); flashProgram(base + dst + i, v); }
      cpu.setB(RA3, 0);
      break;
    }
    case 0x07: {  // VECT_FLASHALLERS
      const int64_t base = cpu.getB(RA3) == 1 ? 0x800000 : 0x200000;
      flashErase(base, 0, chipSize(base));
      cpu.setB(RA3, 0);
      break;
    }
    case 0x08: {  // VECT_FLASHERS: RA3 banco, RB3 bloco
      const int64_t base = cpu.getB(RA3) == 1 ? 0x800000 : 0x200000;
      const auto blocks = flashBlocks(chipSize(base));
      const size_t k = cpu.getB(RB3);
      if (k < blocks.size()) flashErase(base, blocks[k].first, blocks[k].second);
      cpu.setB(RA3, 0);
      break;
    }
    case 0x09: case 0x0B: case 0x0D: cpu.setB(RA3, 0); break;  // ALARMSET, ALARMDOWNSET, FLASHPROTECT
    case 0x0E: break;                                          // VECT_GEMODESET
    case 0x10: cpu.setB(RA3, 0); break;                        // VECT_COMINIT
    case 0x13: cpu.setB(RA3, 0); break;                        // VECT_COMCREATEDATA (cabo não ligado)
    case 0x14: cpu.setB(RA3, 1); break;                        // VECT_COMGETDATA: buffer vazio
    case 0x15: put8(0xB2, 0); break;                           // VECT_COMONRTS
    case 0x16: put8(0xB2, 1); break;                           // VECT_COMOFFRTS
    case 0x17: case 0x18: cpu.setW(RA3, 0); break;             // COMSENDSTATUS / COMRECIVESTATUS
    case 0x19:                                                 // VECT_COMCREATEBUFDATA: consome os bytes
      cpu.setL(XHL3, int64_t(cpu.getL(XHL3)) + cpu.getB(RB3)); cpu.setB(RB3, 0);
      break;
    default: break;
  }
  return 0;
}

// ------------------------------------------------------------ interrupções e DMA
// fonte (índice do vetor na RAM) ou canal de DMA programado para esse vetor do hardware
void NGP::request(int index, int vec) {
  for (int ch = 0; ch < 4; ch++) if (hdma[ch] == vec) { dma(ch); return; }
  pending[index] = 1;
  irqCheck = true;
}

void NGP::checkIrq() {
  irqCheck = false;
  int iff = (cpu.sr >> 12) & 7; if (iff == 1) iff = 0;
  const uint8_t* P = intPrio;
  // ordem fixa: VBL (INT4), Z80 (INT5), timers 0–3, serial
  const int src[8][2] = {{5, P[1] & 7}, {6, (P[1] >> 4) & 7}, {7, P[3] & 7}, {8, (P[3] >> 4) & 7},
                         {9, P[4] & 7}, {10, (P[4] >> 4) & 7}, {11, P[7] & 7}, {12, (P[7] >> 4) & 7}};
  for (const auto& s : src) {
    const int i = s[0], pr = s[1];
    if (pending[i] && pr && pr != 7 && iff <= pr) { pending[i] = 0; interrupt(i, pr); return; }
  }
}

void NGP::interrupt(int index, int level) {
  cpu.halted = false;
  cpu.push(4, cpu.pc); cpu.push(2, cpu.sr);
  cpu.sr = (cpu.sr & 0x8FFF) | (std::min(level + 1, 7) << 12);
  cpu.pc = mem32(0x6FB8 + index * 4) & 0xFFFFFF;
}

void NGP::dma(int ch) {
  if (!cpu.dmaC[ch]) return;
  const int m = cpu.dmaM[ch], mode = (m >> 2) & 7;
  static const int SZ[4] = {1, 2, 4, 4};
  const int sz = SZ[m & 3];
  const uint32_t s = cpu.dmaS[ch], d = cpu.dmaD[ch];
  auto rd = [&](uint32_t a) -> uint32_t {
    a &= 0xFFFFFF;
    return sz == 1 ? static_cast<uint32_t>(mem8(a)) : sz == 2 ? static_cast<uint32_t>(mem16(a)) : mem32(a);
  };
  auto wr = [&](uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (sz == 1) put8(a, static_cast<int>(v)); else if (sz == 2) put16(a, static_cast<int>(v)); else put32(a, v);
  };
  switch (mode) {
    case 0: { const uint32_t v = rd(s); wr(d, v); cpu.dmaD[ch] = d + sz; break; }
    case 1: { const uint32_t v = rd(s); wr(d, v); cpu.dmaD[ch] = d - sz; break; }
    case 2: { const uint32_t v = rd(s); wr(d, v); cpu.dmaS[ch] = s + sz; break; }
    case 3: { const uint32_t v = rd(s); wr(d, v); cpu.dmaS[ch] = s - sz; break; }
    case 4: { const uint32_t v = rd(s); wr(d, v); break; }
    case 5: cpu.dmaS[ch] = s + 1; break;
    default: break;
  }
  cpu.dmaC[ch] = static_cast<uint16_t>((cpu.dmaC[ch] - 1) & 0xFFFF);
  if (!cpu.dmaC[ch]) { interrupt(14 + ch, 7); hdma[ch] = 0; }
}

// ------------------------------------------------------------ timers
void NGP::timers(int n) {
  tclk[0] += n; tclk[1] += n; tclk[2] += n; tclk[3] += n;
  const int tr = trun;
  if (!(tr & 0x0F)) return;
  auto step = [this](int t, double rate) { int c = 0; while (tclk[t] >= rate) { c++; tclk[t] -= rate; } return c; };
  bool t0 = false, t2 = false;
  if (tr & 1) {
    int inc = 0;
    switch (t01mod & 3) {
      case 0: if (hint) { inc = 1; tclk[0] = 0; hint = false; } break;
      case 1: inc = step(0, T1); break;
      case 2: inc = step(0, T4); break;
      default: inc = step(0, T16); break;
    }
    if (inc) { timer[0] = static_cast<uint8_t>((timer[0] + inc) & 0xFF); if (thresh[0] && timer[0] >= thresh[0]) { timer[0] = 0; t0 = true; request(7, 0x10); } }
  }
  if (tr & 2) {
    int inc = 0;
    switch ((t01mod >> 2) & 3) {
      case 0: if (t0) { inc = 1; tclk[1] = 0; } break;
      case 1: inc = step(1, T1); break;
      case 2: inc = step(1, T16); break;
      default: inc = step(1, T256); break;
    }
    if (inc) { timer[1] = static_cast<uint8_t>((timer[1] + inc) & 0xFF); if (thresh[1] && timer[1] >= thresh[1]) { timer[1] = 0; request(8, 0x11); } }
  }
  if (tr & 4) {
    int inc = 0;
    switch (t23mod & 3) {
      case 0: break;
      case 1: inc = step(2, T1 / 2); break;
      case 2: inc = step(2, T4); break;
      default: inc = step(2, T16); break;
    }
    if (inc) { timer[2] = static_cast<uint8_t>((timer[2] + inc) & 0xFF); if (thresh[2] && timer[2] >= thresh[2]) { timer[2] = 0; t2 = true; request(9, 0x12); } }
  }
  if (tr & 8) {
    int inc = 0;
    switch ((t23mod >> 2) & 3) {
      case 0: if (t2) { inc = 1; tclk[3] = 0; } break;
      case 1: inc = step(3, T1); break;
      case 2: inc = step(3, T16); break;
      default: inc = step(3, T256); break;
    }
    if (inc) { timer[3] = static_cast<uint8_t>((timer[3] + inc) & 0xFF); if (thresh[3] && timer[3] >= thresh[3]) { timer[3] = 0; z80.irqLine = true; request(10, 0x13); } }
  }
}

// ------------------------------------------------------------ quadro
void NGP::setKeys(int mask) {
  // ordem do NGP: cima, baixo, esquerda, direita, A, B, Option
  int k = 0;
  if (mask & (1 << K_UP)) k |= 0x01;
  if (mask & (1 << K_DOWN)) k |= 0x02;
  if (mask & (1 << K_LEFT)) k |= 0x04;
  if (mask & (1 << K_RIGHT)) k |= 0x08;
  if (mask & (1 << K_A)) k |= 0x10;
  if (mask & (1 << K_B)) k |= 0x20;
  if (mask & ((1 << K_START) | (1 << K_SELECT))) k |= 0x40;
  keys = k;
}

void NGP::runFrame() {
  if (requestReset) reset();
  ram[0x6F82 - 0x4000] = static_cast<uint8_t>(keys);
  bool done = false;
  int guard = 0;
  while (!done && guard++ < 2000000) {
    if (irqCheck || cpu.halted) checkIrq();
    // a cada instrução: interrupções pendentes podem ter mudado com EI/RETI/POP SR
    const int sr = cpu.sr;
    int n = cpu.step() & 0xFF;
    if (!n) n = 2;
    if (cpu.sr != sr) irqCheck = true;
    cycles += n;
    lineCycles += n;
    if (lineCycles >= LINE) {
      lineCycles -= LINE;
      hint = gfx.hblank();
      if (gfx.endLine()) { done = true; if (gfx.ctlInt & 0x80) request(5, 0x0B); }
    }
    timers(n);
    if (z80on) {
      z80time += n;
      while (z80time > 0) z80time -= z80.step() * 2;
    }
  }
  apu.run(cycles);
  frame++;
}

// ------------------------------------------------------------ save (flash) no formato .ngf
std::vector<uint8_t> NGP::buildNgf() const {
  std::vector<std::pair<size_t, uint32_t>> blocks;
  for (size_t p = 0; p < rom.size(); p += 256) {
    bool diff = false;
    for (size_t i = p; i < p + 256; i++) if (rom[i] != orig[i]) { diff = true; break; }
    if (!diff) continue;
    // o tamanho de cada bloco é gravado em 16 bits: blocos contíguos maiores que isso são divididos (um bloco
    // de 64 KB ou mais perdia o excesso ao voltar do save ou de um estado)
    if (!blocks.empty() && blocks.back().first + blocks.back().second == p && blocks.back().second < 0xFF00) blocks.back().second += 256;
    else blocks.push_back({p, 256});
  }
  uint32_t total = 8;
  for (const auto& b : blocks) total += 6 + b.second;
  std::vector<uint8_t> out(total, 0);
  auto u16 = [&](size_t o, uint32_t v) { out[o] = v & 0xFF; out[o + 1] = (v >> 8) & 0xFF; };
  auto u32 = [&](size_t o, uint32_t v) { u16(o, v & 0xFFFF); u16(o + 2, v >> 16); };
  u16(0, 0x0053); u16(2, static_cast<uint32_t>(blocks.size())); u32(4, total);
  size_t q = 8;
  for (const auto& [p, n] : blocks) {
    const uint32_t addr = p < 0x200000 ? static_cast<uint32_t>(0x200000 + p) : static_cast<uint32_t>(0x800000 + (p - 0x200000));
    u32(q, addr); u16(q + 4, n);  // tamanho em 16 bits, como na versão web
    std::copy(rom.begin() + p, rom.begin() + p + n, out.begin() + q + 6);
    q += 6 + n;
  }
  return out;
}

std::vector<uint8_t> NGP::exportSave() {
  dirty = false;
  return buildNgf();
}

void NGP::importSave(const std::vector<uint8_t>& b) {
  if (b.size() < 8) return;
  auto u16 = [&](size_t o) { return static_cast<uint32_t>(b[o] | (b[o + 1] << 8)); };
  auto u32 = [&](size_t o) { return u16(o) | (u16(o + 2) << 16); };
  if (u16(0) != 0x0053) return;
  const uint32_t count = u16(2);
  size_t q = 8;
  for (uint32_t k = 0; k < count && q + 6 <= b.size(); k++) {
    const uint32_t addr = u32(q), n = u16(q + 4);
    const int64_t p = addr >= 0x800000 ? int64_t(addr) - 0x800000 + 0x200000 : int64_t(addr) - 0x200000;
    if (p >= 0 && p + n <= static_cast<int64_t>(rom.size()) && q + 6 + n <= b.size())
      std::copy(b.begin() + q + 6, b.begin() + q + 6 + n, rom.begin() + p);
    q += 6 + n;
  }
}

// ------------------------------------------------------------ estados salvos
void NGP::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC)) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  z80.sync(io);
  gfx.sync(io);
  apu.sync(io);
  io.bytes(ram);
  std::vector<uint8_t> flash = io.reading() ? std::vector<uint8_t>{} : buildNgf();
  uint32_t flen = static_cast<uint32_t>(flash.size());
  io.sync(flen);
  if (io.reading()) flash.resize(flen);
  if (flen) io.raw(flash.data(), flen);
  for (int* p : {&z80time, &comm, &sc0buf, &commStatus, &trun, &t01mod, &t23mod, &trdc, &tffcr, &lineCycles, &flashCmd}) io.sync(*p);
  io.sync(z80on); io.sync(hint); io.sync(flashId); io.sync(cycles);
  io.raw(intPrio, sizeof intPrio); io.raw(pending, sizeof pending); io.raw(hdma, sizeof hdma);
  io.raw(timer, sizeof timer); io.raw(thresh, sizeof thresh); io.raw(tclk, sizeof tclk);
  if (io.reading()) {
    rom = orig; importSave(flash); dirty = false;
    irqCheck = true;
    clampTo(z80time, -128, 0); clampTo(lineCycles, 0, LINE - 1); clampTo(flashCmd, 0, 6);
    clampTo<int64_t>(cycles, 0, int64_t(1) << 60);
    apu.clampClock(cycles);
    for (int* p : {&comm, &sc0buf, &trun, &t01mod, &t23mod}) *p &= 0xFF;
    commStatus &= 1; trdc &= 3; tffcr &= 0x33;
    for (double& t : tclk) { finiteOr(t, 0); clampTo(t, 0.0, 1e12); }  // acumula com o timer desligado (1e12 ≈ 45 h)
  }
}

std::vector<uint8_t> NGP::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void NGP::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  const bool wasDirty = dirty;
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    dirty = wasDirty;
    throw;
  }
}

// ------------------------------------------------------------ adaptador
NGPCore::NGPCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) : m_(std::make_unique<NGP>()) {
  m_->loadRom(rom);
  if (save) m_->importSave(*save);
}

}  // namespace indigo::ngp
