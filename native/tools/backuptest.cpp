// Testes do backup (.indigo) e da identificação de jogos contra a versão web (native/tools/backuptest.mjs).
//   indigo-backuptest ids rom...            imprime "arquivo console id" de cada ROM
//   indigo-backuptest dump backup.indigo    resumo do conteúdo (ROMs, saves, histórico) com hashes
//   indigo-backuptest copy in.indigo out    lê e grava de novo (o resultado tem de abrir igual na web)
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/core/backup.h"

using namespace indigo::backup;

namespace {
std::vector<uint8_t> readFile(const char* p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}
uint32_t fnv(const std::vector<uint8_t>& b) {
  uint32_t h = 0x811C9DC5u;
  for (uint8_t x : b) { h ^= x; h *= 0x01000193u; }
  return h;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "uso: %s ids|dump|copy ...\n", argv[0]); return 2; }
  const std::string cmd = argv[1];
  if (cmd == "ids") {
    for (int i = 2; i < argc; i++) {
      const auto b = readFile(argv[i]);
      std::string name = argv[i];
      const auto slash = name.find_last_of('/');
      if (slash != std::string::npos) name = name.substr(slash + 1);
      const auto r = identify(b, name);
      std::printf("%s|%s|%s|%s|%s\n", name.c_str(), r.sys.c_str(), r.title.c_str(), r.gameCode.c_str(), r.id.c_str());
    }
    return 0;
  }
  Backup bk;
  std::string err;
  if (!read(readFile(argv[2]), bk, err)) { std::printf("ERRO %s\n", err.c_str()); return 1; }
  if (cmd == "dump") {
    std::printf("withRoms %d states %zu\n", bk.withRoms ? 1 : 0, bk.states);
    for (const auto& r : bk.roms)
      std::printf("rom %s|%s|%s|%s|%d|%zu|%08x\n", r.meta.str("id").c_str(), r.meta.str("name").c_str(), r.meta.str("console").c_str(),
                  r.meta.str("saveKey").c_str(), r.hasData ? 1 : 0, r.data.size(), fnv(r.data));
    for (const auto& s : bk.saves) std::printf("save %s|%.0f|%zu|%08x\n", s.key.c_str(), s.at, s.data.size(), fnv(s.data));
    for (const auto& s : bk.hist) std::printf("hist %s|%.0f|%zu|%08x|%s\n", s.key.c_str(), s.at, s.data.size(), fnv(s.data), s.reason.c_str());
    return 0;
  }
  if (cmd == "copy" && argc > 3) {
    const auto out = write(bk, 1792600000000.0);
    std::ofstream f(argv[3], std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return 0;
  }
  return 2;
}
