// Atari Lynx com a ROM de partida verdadeira (lynxboot.img, 512 bytes). A ROM tem direitos autorais e fica
// fora do repositório: o teste procura <pasta>/bios/lynxboot.img e pula (saída 0) se não existir.
// Cada demo liga pela ROM verdadeira, roda 300 quadros e a tela é comparada com o hash fixado (telas
// conferidas a olho em PNG em 24/10/2026); também confere que a partida é determinística e que
// salvar -> carregar -> salvar volta igual.
// Uso: indigo-t7_lynxboot /root/indigo-env/roms/lynx
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/core.h"

using namespace indigo;

namespace {
std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
uint32_t screenHash(const Core& c) {
  uint32_t h = 2166136261u;
  const uint32_t* p = c.framebuffer();
  for (int i = 0; i < c.width() * c.height(); i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}
struct Case { const char* rom; uint32_t hash; const char* what; };
const Case CASES[] = {
    {"lynx_hacking/nostalgia/nostalgia.lnx", 0x13FED185, "Lynx Nostalgia (estrada 3D)"},
    {"lynx_hacking/monalynx_no_o/monalynx.lnx", 0x569696ED, "Mona Lisa"},
    {"lynx_hacking/raycast/raycast.lnx", 0x4B91EBAC, "raycast"},
    {"lynx_hacking/248b/plasma3.1/plasma.lnx", 0x0E1F0887, "plasma"},
    {"lynx_hacking/248b/aMAZEing/maze.lnx", 0x2077A894, "labirinto"},
    {"lynx_hacking/248b/gol249/gol249.lnx", 0xC53F11C5, "Game of Life"},
    {"lynx_hacking/248b/Mars_Attacks/mars_attacks.lnx", 0x5A2DA1F1, "Mars Attacks"},
    {"lynx_hacking/248b/skywalk/skywalk.lnx", 0x655462E3, "skywalk"},
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta-dos-testes-do-lynx\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  const auto boot = readAll(dir + "/bios/lynxboot.img");
  if (boot.size() != 512) {
    std::printf("Sem %s/bios/lynxboot.img: teste da partida verdadeira pulado\n", dir.c_str());
    return 0;
  }
  int checks = 0, fails = 0;
  auto check = [&](bool ok, const std::string& what) {
    checks++;
    if (!ok) { fails++; std::printf("  FALHA: %s\n", what.c_str()); }
  };
  setFirmware("lynx", boot);
  for (const Case& c : CASES) {
    const auto rom = readAll(dir + "/" + c.rom);
    if (rom.empty()) { check(false, std::string("ROM não encontrada: ") + c.rom); continue; }
    auto a = createCore(rom, c.rom, nullptr);
    auto b = createCore(rom, c.rom, nullptr);
    for (int f = 0; f < 300; f++) { a->runFrame(); b->runFrame(); }
    const uint32_t h = screenHash(*a);
    check(h == c.hash, std::string(c.what) + ": tela diferente da conferida");
    check(h == screenHash(*b), std::string(c.what) + ": partida não determinística");
    const auto st = a->saveState();
    a->loadState(st);
    check(a->saveState() == st, std::string(c.what) + ": salvar/carregar/salvar não volta igual");
    std::printf("  %s: tela %08X\n", c.what, h);
  }
  setFirmware("lynx", {});
  std::printf("RESULTADO lynx (ROM de partida verdadeira): %d verificações, %d falhas\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
