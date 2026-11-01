// Testa a CPU do SNES contra os SingleStepTests do 65816 (github.com/SingleStepTests/65816, pasta v1): para cada
// caso confere os registradores, a memória e cada ciclo do barramento (endereço; valor e leitura/escrita nos
// ciclos que acessam a memória; ciclos internos no lugar certo).
// Uso: indigo-snes65816test pasta/v1 [opcode hex] [e|n] [máximo de casos por arquivo]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/core/backup.h"
#include "../src/snes/cpu.h"

using indigo::backup::Json;

struct TestBus : indigo::snes::CpuBus {
  std::unordered_map<uint32_t, uint8_t> mem;
  struct Cyc { uint32_t a; int v; bool w; };  // v = -1: ciclo interno
  std::vector<Cyc> log;
  uint8_t read(uint32_t a) override { const auto it = mem.find(a); const uint8_t v = it == mem.end() ? 0 : it->second; log.push_back({a, v, false}); return v; }
  void write(uint32_t a, uint8_t v) override { log.push_back({a, v, true}); mem[a] = v; }
  void idle(uint32_t a) override { log.push_back({a, -1, false}); }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta [opcode] [e|n] [casos]\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  const int only = argc > 2 && argv[2][0] != '-' ? static_cast<int>(std::strtol(argv[2], nullptr, 16)) : -1;
  const char modeOnly = argc > 3 ? argv[3][0] : '-';
  const long limit = argc > 4 ? std::atol(argv[4]) : 0;
  long total = 0, bad = 0;
  int badFiles = 0;
  for (int op = 0; op < 256; op++) {
    if (only >= 0 && op != only) continue;
    for (const char mode : {'n', 'e'}) {
      if (modeOnly != '-' && mode != modeOnly) continue;
      char name[24];
      std::snprintf(name, sizeof name, "/%02x.%c.json", op, mode);
      std::ifstream f(dir + name, std::ios::binary);
      if (!f) { std::fprintf(stderr, "falta %s\n", name); return 2; }
      std::stringstream ss; ss << f.rdbuf();
      Json tests;
      if (!indigo::backup::parseJson(ss.str(), tests) || tests.type != Json::Arr) { std::fprintf(stderr, "json %s\n", name); return 2; }
      long fileBad = 0, n = 0;
      std::string firstFail;
      for (const Json& t : tests.a) {
        if (limit && n++ >= limit) break;
        TestBus bus;
        indigo::snes::CPU cpu;
        cpu.bus = &bus;
        const Json* in = t.get("initial");
        const Json* out = t.get("final");
        cpu.pc = static_cast<uint16_t>(in->num("pc")); cpu.s = static_cast<uint16_t>(in->num("s"));
        cpu.a = static_cast<uint16_t>(in->num("a")); cpu.x = static_cast<uint16_t>(in->num("x")); cpu.y = static_cast<uint16_t>(in->num("y"));
        cpu.p = static_cast<uint8_t>(in->num("p")); cpu.d = static_cast<uint16_t>(in->num("d"));
        cpu.dbr = static_cast<uint8_t>(in->num("dbr")); cpu.pbr = static_cast<uint8_t>(in->num("pbr")); cpu.e = in->num("e") != 0;
        cpu.fixRegisters();
        for (const Json& r : in->get("ram")->a) bus.mem[static_cast<uint32_t>(r.a[0].n)] = static_cast<uint8_t>(r.a[1].n);
        const uint8_t opcode = bus.mem[(uint32_t(cpu.pbr) << 16) | cpu.pc];
        const uint16_t startPc = cpu.pc;
        cpu.step();
        // MVN/MVP: os testes rodam a cópia até o fim; WAI/STP: contam o primeiro ciclo já parado
        if (opcode == 0x44 || opcode == 0x54) { while (cpu.pc == startPc && bus.log.size() < 100000) cpu.step(); }
        if (opcode == 0xCB || opcode == 0xDB) cpu.step();
        std::string why;
        auto chk = [&](const char* k, int got) {
          const int want = static_cast<int>(out->num(k));
          if (got != want && why.empty()) { char b[80]; std::snprintf(b, sizeof b, "%s=%04X (esperado %04X)", k, got, want); why = b; }
        };
        chk("pc", cpu.pc); chk("s", cpu.s); chk("p", cpu.p); chk("a", cpu.a); chk("x", cpu.x); chk("y", cpu.y);
        chk("d", cpu.d); chk("dbr", cpu.dbr); chk("pbr", cpu.pbr); chk("e", cpu.e);
        for (const Json& r : out->get("ram")->a) {
          const uint32_t ad = static_cast<uint32_t>(r.a[0].n);
          if (bus.mem[ad] != static_cast<int>(r.a[1].n) && why.empty()) { char b[64]; std::snprintf(b, sizeof b, "mem[%06X]=%02X", ad, bus.mem[ad]); why = b; }
        }
        const Json* cyc = t.get("cycles");
        if (why.empty() && cyc->a.size() != bus.log.size()) why = "ciclos " + std::to_string(bus.log.size()) + " (esperado " + std::to_string(cyc->a.size()) + ")";
        for (size_t k = 0; why.empty() && k < cyc->a.size(); k++) {
          const auto& c = cyc->a[k];
          const bool internal = c.a[1].type == Json::Null;
          const bool w = c.a[2].s.size() > 3 && c.a[2].s[3] == 'w';
          const uint32_t wantA = c.a[0].type == Json::Null ? bus.log[k].a : static_cast<uint32_t>(c.a[0].n);
          const auto& g = bus.log[k];
          const bool okC = g.a == wantA && (internal ? g.v < 0 : (g.v == static_cast<int>(c.a[1].n) && g.w == w));
          if (!okC) {
            char b[120];
            std::snprintf(b, sizeof b, "ciclo %zu: %06X %s%c (esperado %06X %s%c)", k, g.a, g.v < 0 ? "--" : "", g.v < 0 ? 'i' : (g.w ? 'w' : 'r'),
                          wantA, internal ? "--" : "", internal ? 'i' : (w ? 'w' : 'r'));
            why = b;
          }
        }
        total++;
        if (!why.empty()) { fileBad++; if (firstFail.empty()) firstFail = t.str("name") + ": " + why; }
      }
      if (fileBad) { badFiles++; bad += fileBad; std::printf("  %02X %c: %ld falhas; ex.: %s\n", op, mode, fileBad, firstFail.c_str()); }
    }
  }
  std::printf("65816: %ld casos, %ld falhas, %d arquivos com falha\n", total, bad, badFiles);
  return bad ? 1 : 0;
}
