// Roda o ZEXDOC/ZEXALL (programas de CP/M) no Z80 do Índigo, como tools/zextest.mjs:
//   indigo-zextest zexall.com
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/sms/z80.h"

namespace {
struct Mem : indigo::Z80Bus {
  uint8_t m[0x10000] = {};
  int read(int a) override { return m[a]; }
  void write(int a, int v) override { m[a] = static_cast<uint8_t>(v); }
  int in(int) override { return 0xFF; }
  void out(int, int) override {}
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s zexall.com\n", argv[0]); return 2; }
  std::ifstream f(argv[1], std::ios::binary);
  const std::vector<uint8_t> prog((std::istreambuf_iterator<char>(f)), {});
  Mem mem;
  std::memcpy(mem.m + 0x100, prog.data(), std::min<size_t>(prog.size(), 0x10000 - 0x100));
  mem.m[0] = 0x76;  // fim: HALT
  mem.m[5] = 0xC9; mem.m[6] = 0x00; mem.m[7] = 0xF0;  // BDOS em 5; topo da memória em F000
  indigo::Z80 cpu(mem);
  cpu.pc = 0x100; cpu.sp = 0xF000;
  std::string out;
  uint64_t cycles = 0;
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    if (cpu.pc == 5) {
      if (cpu.c == 2) out += static_cast<char>(cpu.e);
      else if (cpu.c == 9) { int a = cpu.de(); while (mem.m[a] != 0x24) out += static_cast<char>(mem.m[a++]); }
      if (out.find('\n') != std::string::npos) { std::fputs(out.c_str(), stdout); std::fflush(stdout); out.clear(); }
    }
    if (cpu.pc == 0 || cpu.halted) break;
    cycles += cpu.step();
  }
  std::fputs(out.c_str(), stdout);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("\n%.0f M ciclos em %.1f s (%.1fx o Master System)\n", cycles / 1e6, secs, cycles / 3.58e6 / secs);
  return 0;
}
