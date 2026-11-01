// Testes do núcleo do Atari Lynx (etapa 7).
// Uso: indigo-t7_lynx <pasta> [opcode hex]
//   <pasta>/65x02/synertek65c02/v1/*.json  SingleStepTests do 65SC02 (todos os opcodes menos $x7/$xF)
//   <pasta>/65x02/wdc65c02/v1/?7.json, ?f.json  RMB/SMB/BBR/BBS (Lynx II) conferidos contra o 65C02 da WDC
//   <pasta>/roms/*.lnx|*.o   demos e homebrew públicos (ver t7_lynx.sh); telas conferidas por hash
// Termina com "RESULTADO lynx: N verificações, M falhas" e código 0 só sem falhas.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../src/core/backup.h"
#include "../src/lynx/cpu.h"
#include "../src/lynx/lynx.h"

using indigo::backup::Json;

namespace {

long checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("  FALHA: %s\n", what.c_str()); }
}

bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}

struct TestBus {
  uint8_t mem[65536] = {};
  struct Cyc { uint16_t a; uint8_t v; bool w; };
  Cyc log[64];
  int n = 0;
  uint8_t read(uint16_t a) { if (n < 64) log[n] = {a, mem[a], false}; n++; return mem[a]; }
  void write(uint16_t a, uint8_t v) { if (n < 64) log[n] = {a, v, true}; n++; mem[a] = v; }
};

// roda um arquivo de testes; devolve o número de casos com falha
long runCpuFile(const std::string& file, int op, long& total) {
  Json tests;
  if (!indigo::backup::parseJson(readFile(file), tests) || tests.type != Json::Arr) {
    std::printf("  não consegui ler %s\n", file.c_str());
    return 1;
  }
  static TestBus bus;
  indigo::lynx::Cpu65SC02<TestBus> cpu(bus);
  long bad = 0;
  std::string firstFail;
  for (const Json& t : tests.a) {
    const Json* in = t.get("initial");
    const Json* out = t.get("final");
    cpu.pc = static_cast<uint16_t>(in->num("pc"));
    cpu.s = static_cast<uint8_t>(in->num("s"));
    cpu.a = static_cast<uint8_t>(in->num("a"));
    cpu.x = static_cast<uint8_t>(in->num("x"));
    cpu.y = static_cast<uint8_t>(in->num("y"));
    cpu.p = static_cast<uint8_t>(in->num("p"));
    for (const Json& r : in->get("ram")->a) bus.mem[static_cast<int>(r.a[0].n)] = static_cast<uint8_t>(r.a[1].n);
    bus.n = 0;
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
    if (why.empty() && cyc->a.size() != static_cast<size_t>(bus.n))
      why = "ciclos " + std::to_string(bus.n) + " (esperado " + std::to_string(cyc->a.size()) + ")";
    for (size_t k = 0; why.empty() && k < cyc->a.size() && k < 64; k++) {
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
    if (!why.empty()) { bad++; if (firstFail.empty()) firstFail = t.str("name") + ": " + why; }
  }
  if (bad) std::printf("  CPU %02X: %ld falhas; ex.: %s\n", op, bad, firstFail.c_str());
  return bad;
}

void cpuTests(const std::string& dir, int only) {
  const std::string syn = dir + "/65x02/synertek65c02/v1";
  const std::string wdc = dir + "/65x02/wdc65c02/v1";
  if (!exists(syn)) { std::printf("CPU: pulei (falta %s)\n", syn.c_str()); return; }
  long total = 0, bad = 0;
  int badOps = 0;
  for (int op = 0; op < 256; op++) {
    if (only >= 0 && op != only) continue;
    char name[16];
    std::snprintf(name, sizeof name, "/%02x.json", op);
    const bool bitOp = (op & 7) == 7;
    const std::string file = (bitOp ? wdc : syn) + name;
    if (!exists(file)) { if (bitOp) continue; check(false, "falta " + file); continue; }
    const long b = runCpuFile(file, op, total);
    if (b) { badOps++; bad += b; }
  }
  std::printf("CPU 65SC02: %ld casos, %ld falhas, %d opcodes com falha\n", total, bad, badOps);
  check(bad == 0 && total > 0, "SingleStepTests do 65SC02");
}

std::vector<uint8_t> readBytes(const std::string& p) {
  const std::string s = readFile(p);
  return std::vector<uint8_t>(s.begin(), s.end());
}

void writePpm(const std::string& p, const uint32_t* px, int w, int h) {
  FILE* f = std::fopen(p.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) { const uint32_t c = px[i]; std::fputc(c & 0xFF, f); std::fputc((c >> 8) & 0xFF, f); std::fputc((c >> 16) & 0xFF, f); }
  std::fclose(f);
}

uint32_t hashFrame(const uint32_t* px, int n) {
  uint32_t h = 0x811C9DC5u;
  for (int i = 0; i < n; i++) { h ^= px[i] & 0xFFFFFF; h *= 0x01000193u; }
  return h;
}

// teclas programadas: "quadro:máscara hex,quadro:máscara"
uint32_t keysAt(const std::string& sched, int frame, uint32_t cur) {
  size_t p = 0;
  while (p < sched.size()) {
    const int fr = std::atoi(sched.c_str() + p);
    const size_t c = sched.find(':', p);
    if (c == std::string::npos) break;
    const uint32_t m = static_cast<uint32_t>(std::strtoul(sched.c_str() + c + 1, nullptr, 16));
    if (fr == frame) cur = m;
    const size_t n = sched.find(',', c);
    if (n == std::string::npos) break;
    p = n + 1;
  }
  return cur;
}

using indigo::lynx::LynxCore;

struct Run {
  std::unique_ptr<LynxCore> core;
  std::vector<float> audio;
};

void runFrames(LynxCore& c, int n, std::vector<float>* audio = nullptr) {
  for (int f = 0; f < n; f++) {
    c.runFrame();
    std::vector<float> l(c.audioAvailable()), r(l.size());
    c.pullAudio(l.data(), r.data(), l.size());
    if (audio) for (size_t i = 0; i < l.size(); i++) { audio->push_back(l[i]); audio->push_back(r[i]); }
  }
}

uint32_t screenHash(const LynxCore& c) { return hashFrame(c.framebuffer(), c.width() * c.height()); }

// telas fixadas por hash (conferidas a olho em PNG e, onde a tela é estática, ponto a ponto contra o Handy)
struct Shot { const char* file; int frames; uint32_t hash; const char* what; };
const Shot SHOTS[] = {
    {"build/lynxteste.lnx", 120, 0x00DA5438, "ROM de teste: sprites, matemática, EEPROM, colisão (igual ao Handy)"},
    {"build/hello.lnx", 300, 0x09157CC3, "cc65 hello (igual ao Handy)"},
    {"new_bll/demos/kingtut/show_tut.o", 300, 0x3D3C2227, "BLL show_tut, imagem 4 bits (igual ao Handy)"},
    {"new_bll/demos/spriteed/spriteed.o", 300, 0x236FEF57, "BLL spriteed (igual ao Handy)"},
    {"new_bll/demos/drawtest.o", 300, 0xC6589083, "BLL drawtest (igual ao Handy)"},
    {"new_bll/demos/depacker/depacker.o", 300, 0x7E205863, "BLL depacker"},
    {"new_bll/demos/demo0006.o", 200, 0x07D4D986, "BLL demo0006: polígonos"},
    {"new_bll/demos/logon/logon.o", 200, 0x968ED30A, "BLL logon: ComLynx + rasters por HBL"},
    {"lynx_hacking/248b/gol249/gol249.lnx", 200, 0x873B7BC5, "boot de 249 bytes: Game of Life"},
    {"lynx_hacking/248b/timewarp/timewarp.lnx", 200, 0xEED2DF37, "boot de 249 bytes: estrelas"},
    {"lynx_hacking/248b/tuef/tuef.lnx", 200, 0xF91FBF2E, "boot de 249 bytes: túnel"},
    {"lynx_hacking/248b/rotz/rotz.lnx", 200, 0xC5EBE285, "boot de 249 bytes: rotozoom com som"},
    {"lynx_hacking/248b/mandel2/mandel.lnx", 200, 0x1494F18A, "boot de 249 bytes: Mandelbrot"},
    {"lynx_hacking/248b/aMAZEing/maze.lnx", 200, 0x1C1BCBC8, "boot de 249 bytes: labirinto"},
    {"lynx_hacking/248b/snake/snake.lnx", 100, 0x621B8319, "boot de 249 bytes: snake (RMB/SMB/BBR do Lynx II)"},
    {"lynx_hacking/monalynx_no_o/monalynx.lnx", 300, 0x949696ED, "monalynx"},
    {"lynx_hacking/raycast/raycast.lnx", 300, 0xEF91EBAC, "raycast"},
    {"lynx_hacking/nostalgia/nostalgia.lnx", 300, 0xE773B725, "nostalgia"},
};

void romTests(const std::string& dir) {
  bool any = false;
  for (const Shot& s : SHOTS) {
    const std::string p = dir + "/" + s.file;
    if (!exists(p)) { std::printf("  (falta %s)\n", s.file); continue; }
    any = true;
    try {
      LynxCore c(readBytes(p), nullptr);
      runFrames(c, s.frames);
      const uint32_t h = screenHash(c);
      if (std::getenv("LYNXPPM")) writePpm(std::string(std::getenv("LYNXPPM")) + "/" + std::to_string(&s - SHOTS) + ".ppm", c.framebuffer(), c.width(), c.height());
      char b[256];
      std::snprintf(b, sizeof b, "%s: tela %08X (esperado %08X)", s.what, h, s.hash);
      check(h == s.hash, b);
      if (h == s.hash) std::printf("  ok  %s\n", s.what);
    } catch (const std::exception& e) {
      check(false, std::string(s.file) + ": " + e.what());
    }
  }
  if (!any) { std::printf("ROMs: pulei (rode t7_lynx.sh)\n"); return; }

  // EEPROM: a ROM de teste grava $BEEF na célula 3 e $1234 na 10 (depois apagada)
  const std::string tp = dir + "/build/lynxteste.lnx";
  if (exists(tp)) {
    LynxCore c(readBytes(tp), nullptr);
    runFrames(c, 120);
    const std::vector<uint8_t> e = c.exportSave();
    check(c.hasSave() && e.size() == 128, "EEPROM 93C46 com 128 bytes");
    check(e.size() == 128 && e[6] == 0xEF && e[7] == 0xBE && e[20] == 0xFF && e[21] == 0xFF, "EEPROM: célula 3 = BEEF, célula 10 apagada");
    // o save volta: um núcleo novo com o save lê o mesmo conteúdo
    LynxCore c2(readBytes(tp), &e);
    check(c2.exportSave() == e, "EEPROM: save importado igual");
    std::printf("  ok  EEPROM\n");

    // rotação: mesma ROM com o cabeçalho "girada para a esquerda" sai 102x160, a imagem girada 90°
    const std::string rp = dir + "/build/lynxteste_rot.lnx";
    if (exists(rp)) {
      LynxCore r(readBytes(rp), nullptr);
      runFrames(r, 120);
      bool same = r.width() == 102 && r.height() == 160;
      for (int y = 0; same && y < 102; y++)
        for (int x = 0; x < 160; x++)
          if (r.framebuffer()[x * 102 + (101 - y)] != c.framebuffer()[y * 160 + x]) { same = false; break; }
      check(same, "rotação à esquerda: 102x160 e imagem girada");
      check(!r.note().empty(), "rotação informada em note()");
    }

    // save state: salvar, rodar, carregar, rodar = mesma tela e mesmo som; salvar→carregar→salvar = bytes iguais
    for (const char* f : {"build/lynxteste.lnx", "lynx_hacking/vgmplay/vgmplay.lnx", "lynx_hacking/248b/rotz/rotz.lnx"}) {
      const std::string sp = dir + "/" + f;
      if (!exists(sp)) continue;
      LynxCore a(readBytes(sp), nullptr);
      a.setSampleRate(48000);
      runFrames(a, 90);
      const std::vector<uint8_t> st = a.saveState();
      std::vector<float> s1, s2;
      runFrames(a, 120, &s1);
      const uint32_t h1 = screenHash(a);
      a.loadState(st);
      runFrames(a, 120, &s2);
      check(screenHash(a) == h1 && s1 == s2, std::string("save state determinístico: ") + f);
      a.loadState(st);
      check(a.saveState() == st, std::string("salvar→carregar→salvar igual: ") + f);
      double rms = 0;
      for (float v : s1) rms += v * v;
      if (std::string(f).find("vgmplay") != std::string::npos) check(!s1.empty() && rms / s1.size() > 1e-4, "vgmplay: música toca (som não mudo)");
    }
    std::printf("  ok  save state\n");
  }
}

// estados danificados e ROMs de lixo: nada pode travar, ler fora da memória ou demorar demais
uint32_t rs = 0x9E3779B9u;
uint32_t rnd() { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

void robustTests(const std::string& dir) {
  const std::string tp = dir + "/build/lynxteste.lnx";
  if (exists(tp)) {
    LynxCore c(readBytes(tp), nullptr);
    runFrames(c, 60);
    const std::vector<uint8_t> st = c.saveState();
    int bad = 0;
    for (int i = 0; i < 60; i++) {
      std::vector<uint8_t> d = st;
      if (i % 3 == 0) d.resize(rnd() % d.size());
      else for (int k = 0; k < 1 + static_cast<int>(rnd() % 64); k++) d[4 + rnd() % (d.size() - 4)] = static_cast<uint8_t>(rnd());
      try { c.loadState(d); } catch (const std::exception&) {}
      try { runFrames(c, 3); } catch (...) { bad++; }
    }
    check(bad == 0, "estados danificados não travam");
  }
  int bad = 0;
  for (int i = 0; i < 40; i++) {
    std::vector<uint8_t> r(64 + (rnd() % 3) * 0x10000 + rnd() % 4096);
    for (auto& b : r) b = static_cast<uint8_t>(rnd());
    std::string name = "lixo.lnx";
    if (i % 3 == 0) { r[0] = 'L'; r[1] = 'Y'; r[2] = 'N'; r[3] = 'X'; r[4] = 0; r[5] = static_cast<uint8_t>(1 << (rnd() % 4)); r[60] = static_cast<uint8_t>(rnd()); }
    else if (i % 3 == 1) { r[0] = 0x80; r[1] = 0x08; r[6] = 'B'; r[7] = 'S'; r[8] = '9'; r[9] = '3'; }
    try {
      LynxCore c(r, nullptr);
      runFrames(c, 20);
      c.loadState(c.saveState());
    } catch (const std::runtime_error&) {
    } catch (...) { bad++; }
  }
  check(bad == 0, "ROMs de lixo não travam");
  std::printf("  ok  robustez\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta [opcode]\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  if (dir == "--dump" && argc >= 5) {  // --dump rom quadros saida.ppm [teclas]: grava a tela (e o som em $LYNXWAV)
    LynxCore core(readBytes(argv[2]), nullptr);
    const int frames = std::atoi(argv[3]);
    const std::string sched = argc > 5 ? argv[5] : "";
    uint32_t k = 0;
    core.setSampleRate(48000);
    std::vector<float> wav;
    for (int f = 0; f < frames; f++) {
      k = keysAt(sched, f, k);
      core.setKeys(k);
      runFrames(core, 1, &wav);
    }
    if (const char* wp = std::getenv("LYNXWAV")) {
      FILE* f = std::fopen(wp, "wb");
      if (f) { std::fwrite(wav.data(), sizeof(float), wav.size(), f); std::fclose(f); }
    }
    writePpm(argv[4], core.framebuffer(), core.width(), core.height());
    std::printf("%08X %s\n", screenHash(core), core.note().c_str());
    return 0;
  }
  if (!exists(dir)) {
    std::printf("Lynx: pasta %s não existe; pulei.\nRESULTADO lynx: 0 verificações, 0 falhas\n", dir.c_str());
    return 0;
  }
  const int only = argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 16)) : -1;
  cpuTests(dir, only);
  if (only < 0) {
    romTests(dir);
    robustTests(dir);
  }
  std::printf("RESULTADO lynx: %ld verificações, %ld falhas\n", checks, fails);
  return fails ? 1 : 0;
}
