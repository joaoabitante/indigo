// Testes do núcleo Atari 2600.
//  1. CPU 6507 (6502 com modo decimal): teste funcional de Klaus Dormann e teste do modo decimal de Bruce Clark
//     (com N, V, Z e C conferidos), versões do Gopher2600.
//  2. Telas: cada ROM de teste (TIA do 6502.ts, exemplos do 8bitworkshop, ROMs do Stella) roda 300 quadros com a
//     mesma sequência de botões usada para gerar a referência no Stella (t7_a2600.sh); cada quadro do Índigo
//     precisa ter o mesmo hash (160 pontos x linhas, cores da paleta do Stella) que o quadro do Stella.
//  3. Save state determinístico, estados danificados e ROMs aleatórias (sem travar, sem laço infinito).
// Uso: indigo-t7_a2600 pasta [--dump rom quadro saida.ppm]
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../src/a2600/a2600.h"

using indigo::a2600::A2600Core;

namespace {

int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- CPU
struct FlatBus : indigo::nes::CpuBus {
  uint8_t mem[65536] = {};
  uint8_t read(uint16_t a) override { return mem[a]; }
  void write(uint16_t a, uint8_t v) override { mem[a] = v; }
};

void cpuTests(const std::string& dir) {
  const std::string k = dir + "/gopher/hardware/cpu/tests/klaus2m5/";
  {
    auto bin = readFile(k + "functional_test/6502_functional_test.bin");
    if (bin.empty()) { std::printf("pulou: teste funcional do 6502 (falta o arquivo)\n"); }
    else {
      auto* bus = new FlatBus;
      std::memcpy(bus->mem + 0x0A, bin.data(), std::min<size_t>(bin.size(), 65536 - 0x0A));
      indigo::nes::CPU cpu;
      cpu.bus = bus;
      cpu.decimalMode = true;
      cpu.powerOn();
      cpu.pc = 0x0400;
      uint16_t last = 0xFFFF;
      long n = 0;
      for (; n < 200000000L; n++) {
        if (cpu.pc == last) break;
        last = cpu.pc;
        cpu.step();
      }
      char b[96];
      std::snprintf(b, sizeof b, "teste funcional do 6502 parou em $%04X (sucesso = $347D)", cpu.pc);
      check(cpu.pc == 0x347D, b);
      std::printf("CPU: %s, %ld instruções\n", b, n);
      delete bus;
    }
  }
  {
    auto bin = readFile(k + "decimal_mode/6502_decimal_test.bin");
    if (bin.empty()) { std::printf("pulou: teste do modo decimal (falta o arquivo)\n"); }
    else {
      auto* bus = new FlatBus;
      std::memcpy(bus->mem + 0x200, bin.data(), std::min<size_t>(bin.size(), 65536 - 0x200));
      indigo::nes::CPU cpu;
      cpu.bus = bus;
      cpu.decimalMode = true;
      cpu.powerOn();
      cpu.pc = 0x0200;
      long n = 0;
      for (; n < 100000000L && cpu.pc != 0x024B; n++) cpu.step();
      check(cpu.pc == 0x024B && bus->mem[0x0B] == 0, "teste do modo decimal (Bruce Clark): ERROR = " +
                                                         std::to_string(bus->mem[0x0B]));
      std::printf("CPU: modo decimal %s (%ld instruções)\n", bus->mem[0x0B] == 0 && cpu.pc == 0x024B ? "ok" : "ERRO", n);
      delete bus;
    }
  }
}

// ---------------------------------------------------------------- telas
struct Script { int a, b; uint32_t mask; };
std::vector<Script> script;
uint32_t keysAt(int f) {
  uint32_t m = 0;
  for (auto& s : script) if (f >= s.a && f <= s.b) m |= s.mask;
  return m;
}

uint64_t screenHash(A2600Core& c) {
  uint64_t f = 1469598103934665603ull;
  const uint8_t* s = c.screenIndices();
  const int h = c.height();
  for (int i = 0; i < 160 * h; i++) {
    const uint32_t rgb = c.paletteRgb(s[i]);
    for (int k = 0; k < 3; k++) { f ^= (rgb >> (8 * k)) & 0xFF; f *= 1099511628211ull; }
  }
  return f;
}

void writePpm(A2600Core& c, const std::string& out) {
  FILE* o = std::fopen(out.c_str(), "wb");
  if (!o) return;
  std::fprintf(o, "P6 160 %d 255\n", c.height());
  for (int i = 0; i < 160 * c.height(); i++) {
    const uint32_t rgb = c.paletteRgb(c.screenIndices()[i]);
    const uint8_t b[3] = {uint8_t(rgb >> 16), uint8_t(rgb >> 8), uint8_t(rgb)};
    std::fwrite(b, 1, 3, o);
  }
  std::fclose(o);
}

void screenTests(const std::string& dir) {
  std::ifstream sf(dir + "/ref/script.txt");
  Script s;
  while (sf >> s.a >> s.b >> s.mask) script.push_back(s);
  std::ifstream lf(dir + "/ref/list.txt");
  std::string name, path;
  int okRoms = 0, total = 0;
  while (lf >> name >> path) {
    for (char& ch : path) if (ch == '?') ch = ' ';
    std::ifstream rf(dir + "/ref/" + name + ".txt");
    std::vector<std::pair<std::string, std::string>> ref;  // dimensões, hash
    int fr;
    std::string dims, hash;
    while (rf >> fr >> dims >> hash) ref.push_back({dims, hash});
    if (ref.empty()) continue;
    total++;
    std::string why;
    try {
      A2600Core core(readFile(dir + "/" + path), nullptr);
      int bad = 0;
      for (size_t f = 0; f < ref.size(); f++) {
        core.setKeys(keysAt(static_cast<int>(f)));
        core.runFrame();
        char d[32], h[32];
        std::snprintf(d, sizeof d, "160x%d", core.height());
        std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(screenHash(core)));
        if (ref[f].first != d) { why = "quadro " + std::to_string(f) + ": tela " + d + " (Stella " + ref[f].first + ")"; break; }
        if (ref[f].second != h && !bad++) why = "quadro " + std::to_string(f) + " diferente do Stella";
      }
      if (bad) why += " (" + std::to_string(bad) + " de " + std::to_string(ref.size()) + " quadros diferentes)";
    } catch (const std::exception& e) {
      why = std::string("erro: ") + e.what();
    }
    check(why.empty(), name + ": " + why);
    if (why.empty()) okRoms++;
  }
  std::printf("Telas: %d de %d ROMs iguais ao Stella em todos os quadros\n", okRoms, total);
}

// ---------------------------------------------------------------- bancos (cartuchos sintéticos do t7_a2600.sh)
void bankTests(const std::string& dir) {
  static const char* names[] = {"F8", "F8SC", "F6", "F6SC", "F4", "F4SC", "FA", "F0", "UA", "FE", "E0", "E7", "3F", "DPC"};
  int ok = 0, total = 0;
  for (const char* n : names) {
    const std::string base = dir + "/synth/bank_" + n;
    auto rom = readFile(base + ".bin");
    std::ifstream ef(base + ".exp");
    std::vector<int> exp;
    std::string hex;
    while (ef >> hex) exp.push_back(std::stoi(hex, nullptr, 16));
    if (rom.empty() || exp.empty()) continue;
    total++;
    std::string why;
    try {
      A2600Core c(rom, nullptr);
      const std::string got = indigo::a2600::bankName(c.machine().cart.type);
      if (got != n) why = "detectado como " + got;
      for (int f = 0; f < 5; f++) c.runFrame();
      for (size_t i = 0; i < exp.size() && why.empty(); i++) {
        const int v = c.machine().ram[i];
        if (v != exp[i]) {
          char b[80];
          std::snprintf(b, sizeof b, "RAM $%02zX = $%02X (esperado $%02X)", 0x80 + i, v, exp[i]);
          why = b;
        }
      }
    } catch (const std::exception& e) {
      why = std::string("erro: ") + e.what();
    }
    check(why.empty(), std::string("banco ") + n + ": " + why);
    if (why.empty()) ok++;
  }
  std::printf("Bancos: %d de %d esquemas certos (detecção e trocas de banco/RAM)\n", ok, total);
}

// ---------------------------------------------------------------- estados e robustez
void stateTests(const std::string& dir) {
  std::ifstream lf(dir + "/ref/list.txt");
  std::string name, path;
  std::vector<std::string> roms;
  while (lf >> name >> path) {
    for (char& ch : path) if (ch == '?') ch = ' ';
    roms.push_back(dir + "/" + path);
  }
  int tested = 0;
  for (size_t i = 0; i < roms.size() && tested < 6; i += std::max<size_t>(1, roms.size() / 6)) {
    std::unique_ptr<A2600Core> c;
    try { c = std::make_unique<A2600Core>(readFile(roms[i]), nullptr); } catch (...) { continue; }
    tested++;
    c->setSampleRate(44100);
    for (int f = 0; f < 90; f++) { c->setKeys(keysAt(f)); c->runFrame(); }
    const auto st = c->saveState();
    auto run = [&](std::vector<uint64_t>& hs, std::vector<float>& snd) {
      for (int f = 90; f < 150; f++) {
        c->setKeys(f & 16 ? indigo::KEY_A | indigo::KEY_RIGHT : 0);
        c->runFrame();
        hs.push_back(screenHash(*c));
        const size_t n = c->audioAvailable();
        std::vector<float> l(n), r(n);
        c->pullAudio(l.data(), r.data(), n);
        snd.insert(snd.end(), l.begin(), l.end());
      }
    };
    std::vector<uint64_t> h1, h2;
    std::vector<float> s1, s2;
    c->pullAudio(nullptr, nullptr, 0);
    { const size_t n = c->audioAvailable(); std::vector<float> l(n), r(n); c->pullAudio(l.data(), r.data(), n); }
    run(h1, s1);
    c->loadState(st);
    { const size_t n = c->audioAvailable(); std::vector<float> l(n), r(n); c->pullAudio(l.data(), r.data(), n); }
    run(h2, s2);
    check(h1 == h2 && s1 == s2, "save state determinístico: " + roms[i]);
    // estados danificados: lança erro ou continua rodando sem travar
    std::mt19937 rng(1234 + static_cast<unsigned>(i));
    for (int k = 0; k < 40; k++) {
      auto bad = st;
      if (k % 3 == 0) bad.resize(rng() % bad.size());
      else for (int j = 0; j < 1 + k; j++) bad[rng() % bad.size()] ^= static_cast<uint8_t>(1 + rng() % 255);
      try { c->loadState(bad); } catch (const std::exception&) {}
      const auto t0 = std::chrono::steady_clock::now();
      for (int f = 0; f < 3; f++) c->runFrame();
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (ms > 2000) { check(false, "estado danificado deixou o quadro lento"); break; }
    }
  }
  check(tested > 0, "ROMs para o teste de estados");
  std::printf("Estados: %d ROMs (determinismo de tela e som, 40 estados danificados cada)\n", tested);
  // ROMs aleatórias
  std::mt19937 rng(99);
  const size_t sizes[] = {2048, 4096, 8192, 10240, 12288, 16384, 32768, 65536, 777};
  int made = 0;
  for (int k = 0; k < 27; k++) {
    std::vector<uint8_t> rom(sizes[k % 9]);
    for (auto& b : rom) b = static_cast<uint8_t>(rng());
    try {
      A2600Core c(rom, nullptr);
      made++;
      for (int f = 0; f < 5; f++) { c.setKeys(rng()); c.runFrame(); }
    } catch (const std::exception&) {}
  }
  check(true, "ROMs aleatórias");
  std::printf("ROMs aleatórias: %d de 27 abriram e rodaram sem travar\n", made);
}

// ---------------------------------------------------------------- som
void soundTest(const std::string& dir) {
  auto rom = readFile(dir + "/8bw-bin/musicplayer.bin");
  if (rom.empty()) return;
  A2600Core c(rom, nullptr);
  c.setSampleRate(48000);
  size_t total = 0;
  double sum = 0, sum2 = 0;
  for (int f = 0; f < 300; f++) {
    c.runFrame();
    const size_t n = c.audioAvailable();
    std::vector<float> l(n), r(n);
    c.pullAudio(l.data(), r.data(), n);
    if (f < 60) continue;
    total += n;
    for (float v : l) { sum += v; sum2 += double(v) * v; }
  }
  const double perFrame = double(total) / 240, rms = std::sqrt(sum2 / std::max<size_t>(total, 1));
  std::printf("Som: %.1f amostras por quadro a 48 kHz (esperado ~801), nível RMS %.3f\n", perFrame, rms);
  check(perFrame > 790 && perFrame < 812, "quantidade de amostras de som por quadro");
  check(rms > 0.02 && rms < 1.0, "o tocador de música do 8bitworkshop faz som");
}

// ---------------------------------------------------------------- velocidade
void speed(const std::string& dir) {
  std::ifstream lf(dir + "/ref/list.txt");
  std::string name, path;
  while (lf >> name >> path) {
    if (name.find("starfield") == std::string::npos) continue;
    A2600Core c(readFile(dir + "/" + path), nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < 600; f++) c.runFrame();
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Velocidade: %.0f quadros/s (%.1fx o tempo real)\n", 600 / s, 600 / s / 60);
    break;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta [--dump rom quadro saida.ppm]\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  struct stat st {};
  if (stat(dir.c_str(), &st) != 0) {
    std::printf("pulou: %s não existe (rode tools/t7_a2600.sh)\nRESULTADO a2600: 0 verificações, 0 falhas\n", dir.c_str());
    return 0;
  }
  if (argc >= 6 && std::string(argv[2]) == "--dump") {
    std::ifstream sf(dir + "/ref/script.txt");
    Script s;
    while (sf >> s.a >> s.b >> s.mask) script.push_back(s);
    A2600Core c(readFile(argv[3]), nullptr);
    const int n = std::atoi(argv[4]);
    for (int f = 0; f <= n; f++) { c.setKeys(keysAt(f)); c.runFrame(); }
    writePpm(c, argv[5]);
    std::printf("%016llx %d linhas/quadro %u\n", static_cast<unsigned long long>(screenHash(c)), c.height(),
                c.machine().tia.lastFrameLines);
    return 0;
  }
  cpuTests(dir);
  screenTests(dir);
  bankTests(dir);
  stateTests(dir);
  soundTest(dir);
  speed(dir);
  std::printf("RESULTADO a2600: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
