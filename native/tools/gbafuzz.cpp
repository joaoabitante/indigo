// Teste diferencial do GBA inteiro (lado C++): mesmo gerador de ROMs aleatórias de native/tools/gbafuzz.mjs.
// As duas saídas devem ser idênticas linha a linha.
// uso: indigo-gbafuzz casos [inicio] [rom.gba]   (com rom.gba, grava a ROM do último caso para depuração)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/gba/gba.h"

using namespace indigo::gba;

namespace {
uint32_t x = 0;
uint32_t next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
const uint32_t BASES[8] = {0x02000000, 0x03000000, 0x04000000, 0x05000000, 0x06000000, 0x07000000, 0x08000000, 0x0E000000};

std::vector<uint8_t> makeRom() {
  std::vector<uint8_t> rom(0x8000);
  auto w32 = [&](size_t o, uint32_t v) { rom[o] = v & 0xFF; rom[o + 1] = (v >> 8) & 0xFF; rom[o + 2] = (v >> 16) & 0xFF; rom[o + 3] = v >> 24; };
  for (size_t o = 0; o < rom.size(); o += 4) w32(o, next());
  w32(0, 0xEA00002E);
  for (uint32_t j = 0; j < 13; j++) w32(0xC0 + 4 * j, 0xE59F0000u | (j << 12) | ((0xF8 + 4 * j) - (0xC8 + 4 * j)));
  w32(0xF4, 0xEA00000C);
  for (uint32_t j = 0; j < 13; j++) { const uint32_t r = next(); w32(0xF8 + 4 * j, (r & 1) ? r : BASES[(r >> 1) & 7] + ((r >> 4) & 0x3FFC)); }
  const uint32_t thumb = next() & 1;
  size_t o = 0x12C;
  if (thumb) { w32(o, 0xE28FC001); w32(o + 4, 0xE12FFF1C); o += 8; }
  for (; o < 0x7000; o += 4) {
    uint32_t v = next();
    if (!thumb && (v & 3) != 0) v = (v & 0x0FFFFFFF) | 0xE0000000u;
    w32(o, v);
  }
  return rom;
}

uint32_t fnvW(uint32_t v, uint32_t h) { h ^= v; return h * 0x01000193u; }
uint32_t fnv8(const std::vector<uint8_t>& a, uint32_t h) { for (uint8_t b : a) { h ^= b; h *= 0x01000193u; } return h; }
// Math.round(v * 1048576) | conversão para uint32 do JS (os valores cabem com folga em int32)
uint32_t jsr(float v) { const double d = static_cast<double>(v) * 1048576.0; return static_cast<uint32_t>(static_cast<int64_t>(std::floor(d + 0.5))); }
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s casos [inicio]\n", argv[0]); return 2; }
  const long count = std::atol(argv[1]), start = argc > 2 ? std::atol(argv[2]) : 1;
  std::vector<float> L, R;
  for (long i = start; i < start + count; i++) {
    x = static_cast<uint32_t>(i) * 0x85EBCA6Bu;
    if (!x) x = 1;
    const auto rom = makeRom();
    if (argc > 3) { FILE* f = std::fopen(argv[3], "wb"); if (f) { std::fwrite(rom.data(), 1, rom.size(), f); std::fclose(f); } }
    auto g = std::make_unique<GBA>();
    g->loadRom(rom, nullptr);
    const int keys = static_cast<int>(next() & 0x3FF);
    for (int f = 0; f < 2; f++) { g->setKeys(f ? keys : 0); g->runFrame(); }
    uint32_t h = 0x811C9DC5u;
    for (int k = 0; k < 16; k++) h = fnvW(static_cast<uint32_t>(g->cpu.r[k]), h);
    h = fnvW(static_cast<uint32_t>(g->cpu.cpsr()), h);
    h = fnvW(static_cast<uint32_t>(g->now() % 0x100000000LL), h);
    h = fnvW(g->cpu.halted ? 1 : 0, h);
    Bus& b = g->bus;
    for (const auto* m : {&b.ewram, &b.iwram, &b.pal, &b.vram, &b.oam, &b.save->data}) h = fnv8(*m, h);
    for (int k = 0; k < 0x200; k++) h = fnvW(g->io.regs[k], h);
    h = fnvW(static_cast<uint32_t>(g->io.ie), h); h = fnvW(static_cast<uint32_t>(g->io.ifr), h); h = fnvW(static_cast<uint32_t>(g->io.ime), h);
    for (uint32_t p : g->ppu.fb) h = fnvW(p, h);
    const size_t n = g->apu.available();
    L.resize(n); R.resize(n);
    g->apu.pull(L.data(), R.data(), n);
    for (size_t k = 0; k < n; k++) { h = fnvW(jsr(L[k]), h); h = fnvW(jsr(R[k]), h); }
    std::printf("%ld %08x %zu\n", i, h, n);
  }
  return 0;
}
