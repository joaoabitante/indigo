// Testes do núcleo NES.
//   indigo-nestest log nestest.nes nestest.log   compara cada instrução com o registro de referência do nestest
//   indigo-nestest rom arquivo.nes [quadros] [saida.ppm]
//       roda uma ROM de teste com o protocolo do blargg ($6000 = situação, $6001-3 = DE B0 61, texto em $6004)
//       e imprime "PASSOU"/"FALHOU <código>" e o texto; opcionalmente grava a tela final
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/nes/nes.h"

using namespace indigo::nes;

static std::vector<uint8_t> readAll(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void shot(const NES& n, const char* path) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fprintf(f, "P6\n256 240\n255\n");
  for (int i = 0; i < 256 * 240; i++) {
    const uint32_t c = n.ppu.fb[i];
    const uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
}

static int logMode(const char* romPath, const char* logPath) {
  Cart c;
  std::string err;
  if (!parseRom(readAll(romPath), c, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  NES n(c);
  n.cpu.pc = 0xC000;  // modo automático do nestest
  std::ifstream log(logPath);
  std::string line;
  int k = 0;
  while (std::getline(log, line)) {
    if (line.size() < 70) continue;
    const int pc = std::stoi(line.substr(0, 4), nullptr, 16);
    auto field = [&](const char* key) {
      const size_t at = line.find(key);
      return std::stoi(line.substr(at + std::strlen(key)), nullptr, 16);
    };
    const int A = field("A:"), X = field("X:"), Y = field("Y:"), P = field("P:"), SP = field("SP:");
    const long cyc = std::stol(line.substr(line.find("CYC:") + 4));
    if (n.cpu.pc != pc || n.cpu.a != A || n.cpu.x != X || n.cpu.y != Y || n.cpu.p != P || n.cpu.s != SP ||
        static_cast<long>(n.cycles) != cyc) {
      std::printf("diferença na linha %d:\n  esperado: %s\n  obtido:   %04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%lu\n", k + 1,
                  line.c_str(), n.cpu.pc, n.cpu.a, n.cpu.x, n.cpu.y, n.cpu.p, n.cpu.s, static_cast<unsigned long>(n.cycles));
      return 1;
    }
    n.cpu.step();
    k++;
  }
  std::printf("nestest: %d instruções iguais ao registro; resultado oficial $02=%02X $03=%02X\n", k, n.peek(2), n.peek(3));
  return (n.peek(2) || n.peek(3)) ? 1 : 0;
}

static int romMode(const char* path, int frames, const char* out) {
  Cart c;
  std::string err;
  if (!parseRom(readAll(path), c, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  std::unique_ptr<NES> n;
  try { n = std::make_unique<NES>(c); } catch (const std::exception& e) { std::printf("NÃO RODA: %s\n", e.what()); return 2; }
  int resetAt = -1, f = 0, lastReset = 0;
  bool started = false, armed = true;
  for (; f < frames; f++) {
    n->runFrame();
    const bool sig = n->peek(0x6001) == 0xDE && n->peek(0x6002) == 0xB0 && n->peek(0x6003) == 0x61;
    if (!sig) continue;
    const uint8_t st = n->peek(0x6000);
    if (st == 0x80) { started = true; continue; }
    if (st == 0x81) {
      // "aperte RESET": um reset por pedido (espera o teste sair do 0x81 antes de aceitar outro)
      if (!armed && f > lastReset + 60) armed = true;  // pediu de novo
      if (armed && resetAt < 0) resetAt = f + 8;
      if (f == resetAt) { n->reset(false); resetAt = -1; armed = false; lastReset = f; }
      continue;
    }
    armed = true;
    if (started || st < 0x80) break;
  }
  if (out) shot(*n, out);
  std::string text;
  for (int a = 0x6004; a < 0x7000; a++) { const uint8_t ch = n->peek(a); if (!ch) break; text += static_cast<char>(ch); }
  const bool sig = n->peek(0x6001) == 0xDE && n->peek(0x6002) == 0xB0 && n->peek(0x6003) == 0x61;
  const uint8_t st = n->peek(0x6000);
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  for (char& ch : text) if (ch == '\n') ch = '|';
  if (!sig) { std::printf("SEM RESULTADO (%d quadros)\n", f); return 3; }
  if (st == 0) { std::printf("PASSOU  %s\n", text.c_str()); return 0; }
  std::printf("FALHOU %d  %s\n", st, text.c_str());
  return 1;
}

int main(int argc, char** argv) {
  if (argc >= 4 && std::strcmp(argv[1], "log") == 0) return logMode(argv[2], argv[3]);
  if (argc >= 3 && std::strcmp(argv[1], "rom") == 0) return romMode(argv[2], argc > 3 ? std::atoi(argv[3]) : 3600, argc > 4 ? argv[4] : nullptr);
  std::fprintf(stderr, "uso: %s log nestest.nes nestest.log | rom arquivo.nes [quadros] [saida.ppm]\n", argv[0]);
  return 2;
}
