// Teste do leitor de .zip: imprime o nome da ROM escolhida, o tamanho e um hash (ou o erro).
// uso: indigo-ziptest arquivo.zip...
#include <cstdio>
#include <fstream>
#include <iterator>

#include "../src/core/zip.h"

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    std::ifstream f(argv[i], std::ios::binary);
    const std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    std::string name, err;
    std::vector<uint8_t> data;
    if (!indigo::zip::isZip(b)) { std::printf("%s: nao e zip\n", argv[i]); continue; }
    if (!indigo::zip::extractRom(b, name, data, err)) { std::printf("%s: ERRO %s\n", argv[i], err.c_str()); continue; }
    uint32_t h = 0x811C9DC5u;
    for (uint8_t x : data) { h ^= x; h *= 0x01000193u; }
    std::printf("%s: %s %zu %08x\n", argv[i], name.c_str(), data.size(), h);
  }
}
