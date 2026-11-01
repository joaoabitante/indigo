// Testes do núcleo MSX (indigo::msx).
//   indigo-t7_msx <pasta>                      roda as verificações (pasta = /root/indigo-env/roms/msx)
//   indigo-t7_msx --png rom quadros saida.png [1|2] [tecla@quadro...]   grava a tela para conferir
//     (tecla: start, select, a, b, x, y, l, r, up, down, left, right; ex.: start@120 aperta START
//      do quadro 120 ao 129)
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/msx/msx.h"

using namespace indigo;

namespace {

std::vector<uint8_t> readAll(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// ---------------------------------------------------------------- PNG sem compressão (blocos "stored")
uint32_t crcTab[256];
void crcInit() {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTab[n] = c;
  }
}
uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < n; i++) c = crcTab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
void be32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24); v.push_back(x >> 16); v.push_back(x >> 8); v.push_back(x);
}
void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  be32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  be32(out, crc(td.data(), td.size()) ^ 0xFFFFFFFFu);
}
bool writePng(const std::string& path, const uint32_t* px, int w, int h) {
  crcInit();
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const uint32_t c = px[y * w + x];
      raw.push_back(c & 0xFF); raw.push_back((c >> 8) & 0xFF); raw.push_back((c >> 16) & 0xFF);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n >= raw.size() ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
  }
  be32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  std::vector<uint8_t> ihdr;
  be32(ihdr, w); be32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  return static_cast<bool>(f);
}

// ---------------------------------------------------------------- utilidades
uint32_t keyBit(const std::string& n) {
  static const struct { const char* n; uint32_t k; } T[] = {
    {"a", KEY_A}, {"b", KEY_B}, {"select", KEY_SELECT}, {"start", KEY_START}, {"right", KEY_RIGHT},
    {"left", KEY_LEFT}, {"up", KEY_UP}, {"down", KEY_DOWN}, {"r", KEY_R}, {"l", KEY_L}, {"x", KEY_X}, {"y", KEY_Y}};
  for (auto& t : T) if (n == t.n) return t.k;
  return 0;
}

struct Press { uint32_t key; int frame; };
std::vector<Press> parsePresses(const std::string& s) {  // "start@120,a@300"
  std::vector<Press> v;
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find(',', p);
    if (e == std::string::npos) e = s.size();
    const std::string it = s.substr(p, e - p);
    const size_t at = it.find('@');
    if (at != std::string::npos) v.push_back({keyBit(it.substr(0, at)), std::atoi(it.c_str() + at + 1)});
    p = e + 1;
  }
  return v;
}

void runWith(msx::MSXCore& c, int frames, const std::vector<Press>& presses, int from = 0) {
  for (int f = from; f < from + frames; f++) {
    uint32_t m = 0;
    for (auto& p : presses) if (f >= p.frame && f < p.frame + 10) m |= p.key;
    c.setKeys(m);
    c.runFrame();
  }
}

uint32_t screenHash(const Core& c) {
  uint32_t h = 0x811C9DC5u;
  const int n = c.width() * c.height();
  const uint32_t* p = c.framebuffer();
  for (int i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  h ^= static_cast<uint32_t>(c.width() * 1000 + c.height());
  return h * 0x01000193u;
}

uint32_t audioHash(Core& c) {
  std::vector<float> l(c.audioAvailable()), r(l.size());
  c.pullAudio(l.data(), r.data(), l.size());
  uint32_t h = 0x811C9DC5u;
  for (float v : l) { uint32_t b; std::memcpy(&b, &v, 4); h ^= b; h *= 0x01000193u; }
  return h;
}

int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) fails++;
  std::printf("%s %s\n", ok ? "ok   " : "FALHA", what.c_str());
}

// ---------------------------------------------------------------- ROMs sintéticas (código Z80 montado aqui)
struct Code {
  std::vector<uint8_t> b;
  Code& op(std::initializer_list<int> v) { for (int x : v) b.push_back(static_cast<uint8_t>(x)); return *this; }
  Code& ldA(int n) { return op({0x3E, n & 0xFF}); }
  Code& store(int a) { return op({0x32, a & 0xFF, a >> 8}); }
  Code& load(int a) { return op({0x3A, a & 0xFF, a >> 8}); }
  Code& wr(int a, int v) { return ldA(v).store(a); }
  int results = 0;
  Code& probe(int a) { return load(a).store(0xC000 + results++); }  // guarda (a) em C000h+n
};

// cada banco de 8 KB cheio com o próprio número; cabeçalho "AB" com INIT = 4010h; o código liga o slot do
// cartucho também na página 2 (8000h–BFFFh) e termina num laço
std::vector<uint8_t> makeRom(size_t size, const Code& body) {
  std::vector<uint8_t> rom(size);
  for (size_t i = 0; i < size; i++) rom[i] = static_cast<uint8_t>(i >> 13);
  Code c;
  c.op({0xF3, 0xDB, 0xA8, 0x47, 0xE6, 0x0C, 0x07, 0x07, 0x4F, 0x78, 0xE6, 0xCF, 0xB1, 0xD3, 0xA8});
  c.b.insert(c.b.end(), body.b.begin(), body.b.end());
  c.op({0x18, 0xFE});
  rom[0] = 'A'; rom[1] = 'B'; rom[2] = 0x10; rom[3] = 0x40;
  for (int k = 4; k < 16; k++) rom[k] = 0;
  std::copy(c.b.begin(), c.b.end(), rom.begin() + 0x10);
  return rom;
}

std::vector<int> runProbe(msx::MSXCore& c, int n, int frames = 300) {
  for (int f = 0; f < frames; f++) c.runFrame();
  std::vector<int> r;
  for (int k = 0; k < n; k++) r.push_back(c.msx().read(0xC000 + k));
  return r;
}

std::string hex(const std::vector<int>& v) {
  std::string s;
  char buf[8];
  for (int x : v) { std::snprintf(buf, sizeof buf, "%02X ", x); s += buf; }
  return s;
}

void testMappers() {
  {  // Konami sem SCC: 4000h fixo, 6000h/8000h/A000h trocam a janela
    Code k;
    k.wr(0x6000, 5).wr(0x8000, 7).wr(0xA000, 9).wr(0x6000, 11);
    k.probe(0x4100).probe(0x6100).probe(0x8100).probe(0xA100);
    k.wr(0x4000, 3).probe(0x4100);  // gravar em 4000h não troca nada
    msx::MSXCore c(makeRom(0x20000, k), nullptr, 1);
    const auto r = runProbe(c, k.results);
    check(c.msx().cart.mapper == msx::Mapper::KONAMI && r == std::vector<int>({0, 11, 7, 9, 0}),
          "mapeador Konami: detectado e bancos " + hex(r));
  }
  {  // Konami SCC + leitura/escrita da onda do SCC
    Code k;
    k.wr(0x5000, 0).wr(0x7000, 4).wr(0x9000, 6).wr(0xB000, 8);  // 4000h é onde o código roda: fica no banco 0
    k.probe(0x4100).probe(0x6100).probe(0x8100).probe(0xA100);
    k.wr(0x9000, 0x3F).wr(0x9800, 0x55).wr(0x987F, 0x81).probe(0x9800).probe(0x98BF).probe(0x9900);
    k.wr(0x9000, 2).probe(0x9800);  // SCC desligado: volta a ROM
    msx::MSXCore c(makeRom(0x20000, k), nullptr, 1);
    const auto r = runProbe(c, k.results);
    check(c.msx().cart.mapper == msx::Mapper::KONAMI_SCC &&
              r == std::vector<int>({0, 4, 6, 8, 0x55, 0x81, 0x55, 2}),
          "mapeador Konami SCC: detectado, bancos e registradores do SCC " + hex(r));
  }
  {  // ASCII 8 KB (com espelho de banco fora da ROM)
    Code k;
    k.wr(0x6800, 5).wr(0x7000, 6).wr(0x7800, 7);
    k.probe(0x4100).probe(0x6100).probe(0x8100).probe(0xA100);
    k.wr(0x7800, 0x13).probe(0xA100);  // 13h numa ROM de 16 bancos = banco 3 (sem SRAM gravada)
    msx::MSXCore c(makeRom(0x20000, k), nullptr, 1);
    const auto r = runProbe(c, k.results);
    check(c.msx().cart.mapper == msx::Mapper::ASCII8 && r == std::vector<int>({0, 5, 6, 7, 3}) && !c.hasSave(),
          "mapeador ASCII 8 KB: detectado, bancos e espelho " + hex(r));
  }
  {  // ASCII 16 KB
    Code k;
    k.wr(0x77FF, 3).probe(0x4100).probe(0x6100).probe(0x8100).probe(0xA100);
    k.wr(0x77FF, 1).probe(0x8100).probe(0xA100);
    msx::MSXCore c(makeRom(0x20000, k), nullptr, 1);
    const auto r = runProbe(c, k.results);
    check(c.msx().cart.mapper == msx::Mapper::ASCII16 && r == std::vector<int>({0, 1, 6, 7, 2, 3}),
          "mapeador ASCII 16 KB: detectado e bancos " + hex(r));
  }
  {  // ASCII 8 KB com SRAM: o bit 10h (ROM de 16 bancos) liga a SRAM ao gravar na janela
    Code k;
    k.wr(0x6800, 1).wr(0x7000, 2).wr(0x7800, 3).wr(0x7000, 0x10).probe(0x8100);
    k.wr(0x8100, 0xA5).probe(0x8100).wr(0x7000, 2).probe(0x8100).wr(0x7000, 0x10).probe(0x8100);
    msx::MSXCore c(makeRom(0x20000, k), nullptr, 1);
    const auto r = runProbe(c, k.results);
    const auto save = c.exportSave();
    check(r == std::vector<int>({0, 0xA5, 2, 0xA5}) && c.hasSave() && save.size() == 0x2000 && save[0x100] == 0xA5,
          "SRAM do ASCII 8 KB: liga, grava e vira save " + hex(r));
    Code k2;  // com o save importado, a SRAM já aparece na primeira leitura
    k2.wr(0x6800, 1).wr(0x7800, 3).wr(0x7000, 0x10).probe(0x8100);
    msx::MSXCore c2(makeRom(0x20000, k2), &save, 1);
    const auto r2 = runProbe(c2, k2.results);
    check(r2 == std::vector<int>({0xA5}) && c2.hasSave(), "SRAM do ASCII 8 KB: save importado " + hex(r2));
  }
  {  // ROM simples de 16 KB com INIT em 8000h fica em 8000h–BFFFh
    Code k;
    std::vector<uint8_t> rom(0x4000, 0x77);
    rom[0] = 'A'; rom[1] = 'B'; rom[2] = 0x10; rom[3] = 0x80;
    std::fill(rom.begin() + 4, rom.begin() + 16, 0);
    const uint8_t code[] = {0xF3, 0x3A, 0x00, 0x80, 0x32, 0x00, 0xC0, 0x18, 0xFE};
    std::copy(code, code + sizeof code, rom.begin() + 0x10);
    int start = 0;
    const auto m = msx::guessMapper(rom, start);
    msx::MSXCore c(rom, nullptr, 1);
    const auto r = runProbe(c, 1);
    check(m == msx::Mapper::PLAIN && start == 0x8000 && r[0] == 0x41, "ROM simples de 16 KB com INIT em 8000h fica em 8000h");
  }
}

// mapeador detectado nas ROMs de exemplo do MSXgl (o nome do arquivo diz o tipo)
void testDetection(const std::string& dir) {
  const struct { const char* file; msx::Mapper m; } T[] = {
    {"gt/s_target_ROM_8K.rom", msx::Mapper::PLAIN}, {"gt/s_target_ROM_16K.rom", msx::Mapper::PLAIN},
    {"gt/s_target_ROM_16K_P2.rom", msx::Mapper::PLAIN}, {"gt/s_target_ROM_32K.rom", msx::Mapper::PLAIN},
    {"gt/s_target_ROM_48K.rom", msx::Mapper::PLAIN}, {"gt/s_target_ROM_64K.rom", msx::Mapper::PLAIN},
    {"gt/s_target_ROM_KONAMI_64K.rom", msx::Mapper::KONAMI}, {"gt/s_target_ROM_KONAMI_128K.rom", msx::Mapper::KONAMI},
    {"gt/s_target_ROM_KONAMI_256K.rom", msx::Mapper::KONAMI},
    {"gt/s_target_ROM_KONAMI_SCC_64K.rom", msx::Mapper::KONAMI_SCC},
    {"gt/s_target_ROM_KONAMI_SCC_128K.rom", msx::Mapper::KONAMI_SCC},
    {"gt/s_target_ROM_KONAMI_SCC_256K.rom", msx::Mapper::KONAMI_SCC},
    {"gt/s_target_ROM_ASCII8_64K.rom", msx::Mapper::ASCII8}, {"gt/s_target_ROM_ASCII8_128K.rom", msx::Mapper::ASCII8},
    {"gt/s_target_ROM_ASCII8_256K.rom", msx::Mapper::ASCII8},
    {"gt/s_target_ROM_ASCII16_64K.rom", msx::Mapper::ASCII16},
    {"gt/s_target_ROM_ASCII16_128K.rom", msx::Mapper::ASCII16},
    {"gt/s_target_ROM_ASCII16_256K.rom", msx::Mapper::ASCII16},
    {"gl/s_trilo.rom", msx::Mapper::KONAMI_SCC}, {"gl/s_vgm.rom", msx::Mapper::ASCII16},
    {"gl/s_arkos.rom", msx::Mapper::ASCII8}, {"gl/s_mapper.rom", msx::Mapper::ASCII8},
  };
  int ok = 0, n = 0;
  std::string bad;
  for (auto& t : T) {
    const auto rom = readAll(dir + "/" + t.file);
    if (rom.empty()) { bad += std::string(" (faltou ") + t.file + ")"; n++; continue; }
    int start = 0;
    const auto m = msx::guessMapper(rom, start);
    n++;
    if (m == t.m) ok++;
    else bad += std::string(" ") + t.file + "=" + msx::mapperName(m);
  }
  check(ok == n, "detecção do mapeador em " + std::to_string(n) + " ROMs do MSXgl" + bad);
}

void testTiming() {
  // laço INC HL / JP: INC HL = 6+1 ciclos, JP = 10+1 (cada instrução tem 1 espera de M1) -> 18 por volta
  std::vector<uint8_t> rom(0x4000, 0);
  rom[0] = 'A'; rom[1] = 'B'; rom[2] = 0x10; rom[3] = 0x40;
  const uint8_t code[] = {0xF3, 0x21, 0x00, 0x00, 0x23, 0xC3, 0x14, 0x40};
  std::copy(code, code + sizeof code, rom.begin() + 0x10);
  msx::MSXCore c(rom, nullptr, 1);
  for (int f = 0; f < 300; f++) c.runFrame();
  const int h0 = c.msx().cpu.hl();
  c.runFrame();
  const int d = (c.msx().cpu.hl() - h0) & 0xFFFF;
  check(d == 3318 || d == 3319, "espera de M1 do MSX: " + std::to_string(d) + " voltas de 18 ciclos num quadro de 59736");
  check(std::abs(c.fps() - 59.922743) < 1e-5, "quadros por segundo NTSC");
}

void testSound() {
  {  // AY: TP=254 -> 1789772,5/(16·254) = 440,4 Hz
    msx::AY8910 ay;
    ay.reset();
    ay.writeAddr(0); ay.writeData(254); ay.writeAddr(1); ay.writeData(0);
    ay.writeAddr(7); ay.writeData(0xBE); ay.writeAddr(8); ay.writeData(15);
    int edges = 0;
    float prev = 0;
    for (int i = 0; i < 223722; i++) { const float v = ay.tick(); if (v > 0.5f && prev < 0.5f) edges++; prev = v; }
    check(edges >= 439 && edges <= 441, "PSG: tom de 440 Hz (" + std::to_string(edges) + " ciclos em 1 s)");
    // ruído com volume fixo: mistura liga só o ruído do canal A
    ay.writeAddr(7); ay.writeData(0xB7); ay.writeAddr(6); ay.writeData(1);
    int changes = 0;
    for (int i = 0; i < 22372; i++) { const float v = ay.tick(); if ((v > 0.5f) != (prev > 0.5f)) changes++; prev = v; }
    check(changes > 1000, "PSG: gerador de ruído (" + std::to_string(changes) + " trocas em 0,1 s)");
    // envelope 0Ch (dente de serra repetido), EP=1: 16 passos de 32 ciclos do PSG
    ay.writeAddr(7); ay.writeData(0xBF); ay.writeAddr(8); ay.writeData(0x10);
    ay.writeAddr(11); ay.writeData(16); ay.writeAddr(12); ay.writeData(0); ay.writeAddr(13); ay.writeData(0x0C);
        int rises = 0;
    for (int i = 0; i < 223722; i++) { const float v = ay.tick(); if (v + 0.3f < prev) rises++; prev = v; }
    // f = 1789772,5/(256·16) = 437 Hz
    check(rises >= 430 && rises <= 445, "PSG: envelope dente de serra 437 Hz (" + std::to_string(rises) + ")");
  }
  {  // SCC: P=0FFh -> 3579545/(32·256) = 437 Hz
    msx::SCC scc;
    scc.reset();
    for (int k = 0; k < 32; k++) scc.write(0x9800 + k, k < 16 ? 100 : -100);
    scc.write(0x9880, 0xFF); scc.write(0x9881, 0x00); scc.write(0x988A, 15); scc.write(0x988F, 1);
    int edges = 0;
    float prev = 0;
    for (int i = 0; i < 223722; i++) { const float v = scc.tick(); if (v > 0 && prev <= 0) edges++; prev = v; }
    check(edges >= 435 && edges <= 438, "SCC: onda de 437 Hz (" + std::to_string(edges) + ")");
  }
}

// salvar, rodar, carregar, rodar de novo: mesma tela e mesmo som; estados estragados não podem travar
void testStates(const std::string& dir) {
  const char* roms[] = {"stevedore.rom", "gl/s_trilo.rom", "gl/s_vdpcmd.rom", "gl/s_scroll.rom"};
  for (const char* f : roms) {
    const auto rom = readAll(dir + "/" + f);
    if (rom.empty()) { check(false, std::string("faltou ") + f); continue; }
    msx::MSXCore c(rom, nullptr, 0);
    c.setSampleRate(48000);
    for (int i = 0; i < 400; i++) { c.setKeys(i % 50 < 5 ? uint32_t(KEY_START) : 0u); c.runFrame(); }
    audioHash(c);
    const auto st = c.saveState();
    auto run = [&]() {
      uint32_t h = 0;
      for (int i = 0; i < 120; i++) { c.setKeys(i % 30 < 3 ? uint32_t(KEY_A | KEY_RIGHT) : 0u); c.runFrame(); h = h * 31 + screenHash(c); }
      return h ^ audioHash(c);
    };
    const uint32_t a = run();
    c.loadState(st);
    const uint32_t b = run();
    check(a == b, std::string("save state determinístico: ") + f);
    // estados danificados
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return seed >> 8; };
    int threw = 0, ran = 0;
    for (int t = 0; t < 40; t++) {
      std::vector<uint8_t> bad = st;
      if (t < 10) bad.resize(rnd() % bad.size());
      else for (int k = 0; k < 1 + t; k++) bad[rnd() % bad.size()] = static_cast<uint8_t>(rnd());
      try { c.loadState(bad); ran++; } catch (const std::exception&) { threw++; }
      for (int i = 0; i < 3; i++) c.runFrame();
    }
    check(threw + ran == 40, std::string("estados danificados sem travar (") + std::to_string(threw) + " recusados): " + f);
  }
}

void testGarbage() {
  uint32_t seed = 777;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
  int ok = 0;
  const size_t sizes[] = {0x2000, 0x4000, 0x8000, 0xC000, 0x10000, 0x20000, 0x40000, 0x100000};
  for (int t = 0; t < 16; t++) {
    std::vector<uint8_t> rom(sizes[t % 8]);
    for (auto& b : rom) b = static_cast<uint8_t>(rnd());
    rom[0] = 'A'; rom[1] = 'B';
    for (int m = 1; m <= 2; m++) {
      msx::MSXCore c(rom, nullptr, m);
      for (int f = 0; f < 200; f++) { c.setKeys(rnd()); c.runFrame(); }
      ok++;
    }
  }
  check(ok == 32, "ROMs de lixo (32 rodadas de 200 quadros sem travar)");
}

// ---------------------------------------------------------------- telas conferidas (hash fixado depois de ver o PNG)
struct Screen {
  const char* path;  // relativo à pasta de testes
  int machine;       // 1 ou 2 (0 = automático)
  int frames;
  const char* keys;
  uint32_t hash;
  const char* what;
};
const Screen SCREENS[] = {
    {"gl/s_hello.rom", 0, 400, "", 0x3AC156E9, "MSXgl hello (MSX1, texto 32 colunas)"},
    {"gl/s_mapper.rom", 0, 400, "", 0x9D0D275B, "MSXgl mapper (ASCII 8 KB, espelho de bancos)"},
    {"gl/s_text.rom", 0, 400, "", 0x3C3F0808, "MSXgl texto 40 colunas"},
    {"gl/s_text.rom", 0, 500, "right@300,right@330,right@360", 0xECBCB0AF, "MSXgl texto 80 colunas (V9938)"},
    {"gl/s_sm1.rom", 0, 400, "", 0xD55F8583, "MSXgl sprites modo 1"},
    {"gl/s_sprite.rom", 0, 400, "", 0x286E0FE1, "MSXgl sprites modo 2 (SCREEN 5, CC/OR)"},
    {"gl/s_scroll.rom", 2, 500, "", 0xA7A7DB87, "MSXgl rolagem com interrupção de linha (SCREEN 4)"},
    {"gl/s_draw.rom", 0, 400, "", 0xE2243ACE, "MSXgl desenho (SCREEN 5)"},
    {"gl/s_gm3.rom", 0, 400, "", 0xA60D3D1A, "MSXgl SCREEN 4"},
    {"gl/s_swtile.rom", 0, 400, "", 0x6C19643E, "MSXgl tiles"},
    {"gl/s_swsprt.rom", 0, 400, "", 0x3ED313E7, "MSXgl SCREEN 8 (ASCII 8 KB)"},
    {"gl/s_vdpcmd.rom", 2, 600, "", 0x67881DCD, "MSXgl comandos do VDP em SCREEN 8"},
    {"gl/s_vdpcmd.rom", 2, 600, "down@400", 0xB90855E9, "MSXgl comandos do VDP em SCREEN 5"},
    {"gl/s_vdpcmd.rom", 2, 700, "down@400,down@500", 0x80F35A37, "MSXgl comandos do VDP em SCREEN 6"},
    {"gl/s_vdpcmd.rom", 2, 800, "down@400,down@500,down@600", 0x8D5DC93B, "MSXgl comandos do VDP em SCREEN 7"},
    {"gl/s_trilo.rom", 0, 400, "", 0xB5C49436, "MSXgl Trilo (Konami SCC)"},
    {"gl/s_psg.rom", 0, 400, "", 0x7AD1DC68, "MSXgl PSG"},
    {"gl/s_game.rom", 0, 400, "", 0xC00E5001, "MSXgl jogo"},
    {"gt/s_target_ROM_KONAMI_SCC_256K.rom", 0, 400, "", 0x496B3342, "MSXgl Konami SCC 256 KB"},
    {"gt/s_target_ROM_ASCII16_256K.rom", 0, 400, "", 0xFADEEA93, "MSXgl ASCII 16 KB 256 KB"},
    {"gt/s_target_ROM_48K.rom", 0, 400, "", 0x4BBE01D8, "MSXgl ROM de 48 KB"},
    {"gt/s_target_ROM_16K_P2.rom", 0, 400, "", 0x85AA1F30, "MSXgl ROM de 16 KB em 8000h"},
    {"stevedore.rom", 0, 3000, "start@700,start@800,start@900,right@1000,right@1010,right@1020,a@1100", 0xA402B430, "Stevedore jogando"},
    {"pyramid.rom", 0, 600, "", 0xB6FFCC94, "Pyramid Warp EX"},
    {"run23.rom", 0, 600, "", 0x3D8C83C3, "Run23 (MSXlib)"},
    {"snake.rom", 0, 600, "", 0xFA41FC83, "Snake (MSXlib)"},
};

void testScreens(const std::string& dir, bool list) {
  for (const auto& s : SCREENS) {
    const auto rom = readAll(dir + "/" + s.path);
    if (rom.empty()) { check(false, std::string("faltou ") + s.path); continue; }
    msx::MSXCore c(rom, nullptr, s.machine);
    runWith(c, s.frames, parsePresses(s.keys));
    const uint32_t h = screenHash(c);
    if (list) std::printf("    {\"%s\", %d, %d, \"%s\", 0x%08X, \"%s\"},\n", s.path, s.machine, s.frames, s.keys, h, s.what);
    char buf[32];
    std::snprintf(buf, sizeof buf, " (%08X)", h);
    check(h == s.hash, std::string("tela: ") + s.what + buf);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 5 && std::string(argv[1]) == "--png") {
    const auto rom = readAll(argv[2]);
    if (rom.empty()) { std::printf("não abriu %s\n", argv[2]); return 2; }
    const int machine = argc >= 6 ? std::atoi(argv[5]) : 0;
    const auto presses = parsePresses(argc >= 7 ? argv[6] : "");
    // BIOS do usuário para conferir (fora dos testes automáticos): MSXFW=msx.rom MSXFW2=msx2.rom MSXFW2SUB=sub.rom
    const char* const FW[3][2] = {{"msx", "MSXFW"}, {"msx2", "MSXFW2"}, {"msx2sub", "MSXFW2SUB"}};
    for (const auto& fw : FW)
      if (const char* f = std::getenv(fw[1])) setFirmware(fw[0], readAll(f));
    msx::MSXCore c(rom, nullptr, machine);
    if (const char* wav = std::getenv("MSXWAV")) {  // grava também o som (mono, 16 bits, 48 kHz)
      std::vector<int16_t> pcm;
      for (int f = 0; f < std::atoi(argv[3]); f++) {
        runWith(c, 1, presses, f);
        std::vector<float> l(c.audioAvailable()), r(l.size());
        c.pullAudio(l.data(), r.data(), l.size());
        for (float v : l) pcm.push_back(static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, v)) * 32767));
      }
      std::ofstream w(wav, std::ios::binary);
      auto u32 = [&](uint32_t x) { w.write(reinterpret_cast<const char*>(&x), 4); };
      auto u16 = [&](uint16_t x) { w.write(reinterpret_cast<const char*>(&x), 2); };
      const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
      w.write("RIFF", 4); u32(36 + bytes); w.write("WAVEfmt ", 8); u32(16); u16(1); u16(1); u32(48000);
      u32(96000); u16(2); u16(16); w.write("data", 4); u32(bytes);
      w.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(bytes));
    } else {
      runWith(c, std::atoi(argv[3]), presses);
    }
    writePng(argv[4], c.framebuffer(), c.width(), c.height());
    std::printf("%s %dx%d hash %08X (%s)\n", argv[4], c.width(), c.height(), screenHash(c), c.description().c_str());
    return 0;
  }
  if (argc < 2) { std::printf("uso: indigo-t7_msx <pasta>\n"); return 2; }
  const std::string dir = argv[1];
  if (readAll(dir + "/stevedore.rom").empty()) {
    std::printf("pasta %s sem os testes do MSX (rode tools/t7_msx.sh): pulou\nRESULTADO msx: 0 verificações, 0 falhas\n",
                dir.c_str());
    return 0;
  }
  testTiming();
  testSound();
  testMappers();
  testDetection(dir);
  testScreens(dir, argc >= 3 && std::string(argv[2]) == "--lista");
  testStates(dir);
  testGarbage();
  std::printf("RESULTADO msx: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
