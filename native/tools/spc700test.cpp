// Testa o SPC700 contra os SingleStepTests do SPC700 (github.com/SingleStepTests/spc700, pasta v1): registradores,
// memória e cada ciclo (endereço e tipo; valor quando o teste informa).
// Uso: indigo-spc700test pasta/v1 [opcode hex]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/backup.h"
#include "../src/snes/spc700.h"

using indigo::backup::Json;

struct TestBus : indigo::snes::SpcBus {
  uint8_t mem[65536] = {};
  struct Cyc { int a; int v; char k; };  // k: r, w (escrita), i (interno)
  std::vector<Cyc> log;
  uint8_t read(uint16_t a) override { log.push_back({a, mem[a], 'r'}); return mem[a]; }
  void write(uint16_t a, uint8_t v) override { log.push_back({a, v, 'w'}); mem[a] = v; }
  void idle() override { log.push_back({-1, -1, 'i'}); }
};

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta [opcode]\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  const int only = argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 16)) : -1;
  long total = 0, bad = 0;
  int badOps = 0;
  for (int op = 0; op < 256; op++) {
    if (only >= 0 && op != only) continue;
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
      indigo::snes::SPC700 cpu;
      cpu.bus = &bus;
      const Json* in = t.get("initial");
      const Json* out = t.get("final");
      cpu.pc = static_cast<uint16_t>(in->num("pc")); cpu.a = static_cast<uint8_t>(in->num("a"));
      cpu.x = static_cast<uint8_t>(in->num("x")); cpu.y = static_cast<uint8_t>(in->num("y"));
      cpu.sp = static_cast<uint8_t>(in->num("sp")); cpu.psw = static_cast<uint8_t>(in->num("psw"));
      for (const Json& r : in->get("ram")->a) bus.mem[static_cast<int>(r.a[0].n)] = static_cast<uint8_t>(r.a[1].n);
      cpu.step();
      const Json* cyc = t.get("cycles");
      while (cpu.halted && bus.log.size() < cyc->a.size()) cpu.step();  // SLEEP/STOP: os testes contam os ciclos parados
      std::string why;
      auto chk = [&](const char* k, int got) {
        const int want = static_cast<int>(out->num(k));
        if (got != want && why.empty()) { char b[64]; std::snprintf(b, sizeof b, "%s=%02X (esperado %02X)", k, got, want); why = b; }
      };
      chk("pc", cpu.pc); chk("a", cpu.a); chk("x", cpu.x); chk("y", cpu.y); chk("sp", cpu.sp); chk("psw", cpu.psw);
      for (const Json& r : out->get("ram")->a) {
        const int ad = static_cast<int>(r.a[0].n);
        if (bus.mem[ad] != static_cast<int>(r.a[1].n) && why.empty()) { char b[64]; std::snprintf(b, sizeof b, "mem[%04X]=%02X", ad, bus.mem[ad]); why = b; }
      }
      if (why.empty() && cyc->a.size() != bus.log.size()) why = "ciclos " + std::to_string(bus.log.size()) + " (esperado " + std::to_string(cyc->a.size()) + ")";
      for (size_t k = 0; why.empty() && k < cyc->a.size(); k++) {
        const auto& c = cyc->a[k];
        const char wantK = c.a[2].s == "wait" ? 'i' : c.a[2].s == "write" ? 'w' : 'r';
        const auto& g = bus.log[k];
        bool okC = g.k == wantK;
        if (okC && wantK != 'i' && c.a[0].type != Json::Null) okC = g.a == static_cast<int>(c.a[0].n);
        if (okC && wantK != 'i' && c.a[1].type != Json::Null) okC = g.v == static_cast<int>(c.a[1].n);
        if (!okC) {
          char b[96];
          std::snprintf(b, sizeof b, "ciclo %zu: %04X %c (esperado %04X %c)", k, g.a & 0xFFFF, g.k, c.a[0].type == Json::Null ? 0 : static_cast<int>(c.a[0].n), wantK);
          why = b;
        }
      }
      total++;
      if (!why.empty()) { opBad++; if (firstFail.empty()) firstFail = t.str("name") + ": " + why; }
    }
    if (opBad) { badOps++; bad += opBad; std::printf("  %02X: %ld falhas; ex.: %s\n", op, opBad, firstFail.c_str()); }
  }
  std::printf("SPC700: %ld casos, %ld falhas, %d opcodes com falha\n", total, bad, badOps);
  return bad ? 1 : 0;
}
