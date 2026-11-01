// Testa a CPU do NES contra os SingleStepTests do 6502 (github.com/SingleStepTests/65x02, pasta 6502/v1):
// para cada caso confere registradores, memória e cada ciclo do barramento (endereço, valor, leitura/escrita).
// Uso: indigo-nes6502test pasta/6502/v1 [opcode hex]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/backup.h"
#include "../src/nes/cpu.h"

using indigo::backup::Json;

struct TestBus : indigo::nes::CpuBus {
  uint8_t mem[65536] = {};
  struct Cyc { uint16_t a; uint8_t v; bool w; };
  std::vector<Cyc> log;
  uint8_t read(uint16_t a) override { log.push_back({a, mem[a], false}); return mem[a]; }
  void write(uint16_t a, uint8_t v) override { log.push_back({a, v, true}); mem[a] = v; }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta [opcode]\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  int only = argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 16)) : -1;
  // JAM trava a CPU de verdade (os testes registram leituras que dependem do chip); fica de fora
  const int jams[] = {0x02, 0x12, 0x22, 0x32, 0x42, 0x52, 0x62, 0x72, 0x92, 0xB2, 0xD2, 0xF2};
  long total = 0, bad = 0;
  int badOps = 0;
  for (int op = 0; op < 256; op++) {
    if (only >= 0 && op != only) continue;
    bool jam = false;
    for (int j : jams) jam |= j == op;
    if (jam) continue;
    char name[16];
    std::snprintf(name, sizeof name, "/%02x.json", op);
    std::ifstream f(dir + name, std::ios::binary);
    if (!f) { std::fprintf(stderr, "falta %s\n", name); return 2; }
    std::stringstream ss; ss << f.rdbuf();
    Json tests;
    if (!indigo::backup::parseJson(ss.str(), tests) || tests.type != Json::Arr) { std::fprintf(stderr, "json %s\n", name); return 2; }
    long opBad = 0;
    std::string firstFail;
    for (const Json& t : tests.a) {
      TestBus bus;
      indigo::nes::CPU cpu;
      cpu.bus = &bus;
      cpu.decimalMode = true;
      cpu.magic = 0xEE;
      const Json* in = t.get("initial");
      const Json* out = t.get("final");
      cpu.pc = static_cast<uint16_t>(in->num("pc"));
      cpu.s = static_cast<uint8_t>(in->num("s"));
      cpu.a = static_cast<uint8_t>(in->num("a"));
      cpu.x = static_cast<uint8_t>(in->num("x"));
      cpu.y = static_cast<uint8_t>(in->num("y"));
      cpu.p = static_cast<uint8_t>(in->num("p"));
      for (const Json& r : in->get("ram")->a) bus.mem[static_cast<int>(r.a[0].n)] = static_cast<uint8_t>(r.a[1].n);
      cpu.step();
      std::string why;
      auto chk = [&](const char* k, int got) {
        const int want = static_cast<int>(out->num(k));
        if (got != want && why.empty()) { char b[64]; std::snprintf(b, sizeof b, "%s=%02X (esperado %02X)", k, got, want); why = b; }
      };
      chk("pc", cpu.pc); chk("s", cpu.s); chk("a", cpu.a); chk("x", cpu.x); chk("y", cpu.y); chk("p", cpu.p);
      for (const Json& r : out->get("ram")->a) {
        const int ad = static_cast<int>(r.a[0].n);
        if (bus.mem[ad] != static_cast<int>(r.a[1].n) && why.empty()) { char b[64]; std::snprintf(b, sizeof b, "mem[%04X]=%02X", ad, bus.mem[ad]); why = b; }
      }
      const Json* cyc = t.get("cycles");
      if (why.empty() && cyc->a.size() != bus.log.size()) why = "ciclos " + std::to_string(bus.log.size()) + " (esperado " + std::to_string(cyc->a.size()) + ")";
      for (size_t k = 0; why.empty() && k < cyc->a.size(); k++) {
        const auto& c = cyc->a[k];
        const bool w = c.a[2].s == "write";
        if (bus.log[k].a != static_cast<int>(c.a[0].n) || bus.log[k].v != static_cast<int>(c.a[1].n) || bus.log[k].w != w) {
          char b[96];
          std::snprintf(b, sizeof b, "ciclo %zu: %04X %02X %c (esperado %04X %02X %c)", k, bus.log[k].a, bus.log[k].v,
                        bus.log[k].w ? 'w' : 'r', static_cast<int>(c.a[0].n), static_cast<int>(c.a[1].n), w ? 'w' : 'r');
          why = b;
        }
      }
      total++;
      if (!why.empty()) { opBad++; if (firstFail.empty()) firstFail = t.str("name") + ": " + why; }
    }
    if (opBad) { badOps++; bad += opBad; std::printf("  %02X: %ld falhas; ex.: %s\n", op, opBad, firstFail.c_str()); }
  }
  std::printf("6502: %ld casos, %ld falhas, %d opcodes com falha\n", total, bad, badOps);
  return bad ? 1 : 0;
}
