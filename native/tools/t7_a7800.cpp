// Testes do núcleo Atari 7800.
//   indigo-t7_a7800 pasta                       roda as verificações (pasta = /root/indigo-env/roms/a7800,
//                                               preparada por tools/t7_a7800.sh)
//   indigo-t7_a7800 --png rom.a78 quadros saida.png [teclas@quadro ...]   grava a tela (para conferir a olho)
//   indigo-t7_a7800 --hash rom.a78 quadros [teclas@quadro ...]           imprime o hash da tela
// Teclas: letras A B S(start) E(select) U D L R X, aplicadas a partir do quadro indicado, ex.: S@60 A@120.
// As verificações incluem os dois jogadores (joystick 2 na porta 2).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "../src/a7800/a7800.h"

using namespace indigo;
using indigo::a7800::A7800Core;

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- PNG sem compressão
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
static void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) {
      const uint32_t c = px[y * w + x];
      raw.push_back(c & 0xFF); raw.push_back((c >> 8) & 0xFF); raw.push_back((c >> 16) & 0xFF);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size());
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + static_cast<long>(i), raw.begin() + static_cast<long>(i + n));
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  const uint32_t ad = (b << 16) | a;
  for (int k = 3; k >= 0; k--) z.push_back((ad >> (8 * k)) & 0xFF);
  std::ofstream f(path, std::ios::binary);
  auto be = [&](uint32_t v) { uint8_t t[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; f.write((char*)t, 4); };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& d) {
    be(static_cast<uint32_t>(d.size()));
    std::vector<uint8_t> td(type, type + 4);
    td.insert(td.end(), d.begin(), d.end());
    f.write((char*)td.data(), static_cast<long>(td.size()));
    be(crc32(td.data(), td.size()));
  };
  f.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ih = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                             uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ih);
  chunk("IDAT", z);
  chunk("IEND", {});
}

static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}
static uint64_t screenHash(Core& c) { return fnv(c.framebuffer(), size_t(c.width()) * c.height() * 4); }

// teclas: "S@60" = Start apertado do quadro 60 em diante; "-S@70" solta
struct Script { std::vector<std::pair<int, std::string>> ev; };
static uint32_t keyBits(char ch) {
  switch (ch) {
    case 'A': return KEY_A; case 'B': return KEY_B; case 'S': return KEY_START; case 'E': return KEY_SELECT;
    case 'U': return KEY_UP; case 'D': return KEY_DOWN; case 'L': return KEY_LEFT; case 'R': return KEY_RIGHT;
    case 'X': return KEY_X; default: return 0;
  }
}
static void run(Core& c, int frames, const std::vector<std::string>& keys, std::vector<float>* audio = nullptr) {
  uint32_t mask = 0;
  std::vector<float> l(4096), r(4096);
  for (int f = 0; f < frames; f++) {
    for (const auto& k : keys) {
      const bool rel = k[0] == '-';
      const size_t at = k.find('@');
      if (at == std::string::npos) continue;
      if (std::atoi(k.c_str() + at + 1) != f) continue;
      uint32_t b = 0;
      for (size_t i = rel ? 1 : 0; i < at; i++) b |= keyBits(k[i]);
      mask = rel ? (mask & ~b) : (mask | b);
    }
    c.setKeys(mask);
    c.runFrame();
    const size_t n = std::min<size_t>(c.audioAvailable(), l.size());
    c.pullAudio(l.data(), r.data(), n);
    if (audio) audio->insert(audio->end(), l.begin(), l.begin() + static_cast<long>(n));
  }
}

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

// telas conferidas a olho (PNG) e fixadas por hash: rom, quadros, teclas, hash
struct Ref { const char* rom; int frames; std::vector<std::string> keys; uint64_t hash; };
// (conferidas no PNG: texto, sprites, 320A, 160A, banksets = mesma tela do adventurer, POKEY em $450 achado,
// 101 sprites, área segura de 224 linhas inteira na imagem de 240)
static const std::vector<Ref> kRefs = {
    {"helloworld.a78", 120, {}, 0xff523ccea18dbdc3ull},
    {"smasteroids.a78", 240, {"S@100", "-S@105"}, 0xcb3c2ebd0606bc5full},
    {"adventurer.a78", 240, {"S@100", "-S@105"}, 0x356fa1392dfcaf5bull},
    {"banksets.a78", 240, {"S@100", "-S@105"}, 0x356fa1392dfcaf5bull},
    {"splitmodedemotop.a78", 240, {"S@100", "-S@105"}, 0x84fa59fb448d98f3ull},
    {"fontcheck.a78", 240, {"S@100", "-S@105"}, 0x442e59f765be1fe3ull},
    {"eightysix.a78", 240, {"S@100", "-S@105"}, 0x87526fdce454e292ull},
    {"multisprite.a78", 240, {"S@100", "-S@105"}, 0x134b7ceca033d613ull},
    {"onehundredandone.a78", 240, {"S@100", "-S@105"}, 0xb665a447d8203a72ull},
    {"pokey.a78", 240, {"S@100", "-S@105"}, 0xb5f3c497ff5c84a3ull},
    {"rmtromdemo.a78", 240, {"S@100", "-S@105"}, 0x976d3d2f000c8e5eull},
    {"underthehood.a78", 240, {"S@100", "-S@105"}, 0x864d919890f59c07ull},
    {"screensafe.a78", 240, {"S@100", "-S@105"}, 0x63ad0f62956e40cbull},
};


// ---------------------------------------------------------------- ROMs de teste montadas aqui (6502 à mão)
// ROM plana de 16 KB em $C000; código em $C000, NMI em $C100; cabeçalho opcional com o tipo de cartucho
static std::vector<uint8_t> makeRom(const std::vector<uint8_t>& code, const std::vector<uint8_t>& nmi,
                                    const std::vector<std::pair<int, std::vector<uint8_t>>>& data, int type = -1) {
  std::vector<uint8_t> r(0x4000, 0xEA);
  std::copy(code.begin(), code.end(), r.begin());
  std::copy(nmi.begin(), nmi.end(), r.begin() + 0x100);
  for (const auto& d : data) std::copy(d.second.begin(), d.second.end(), r.begin() + (d.first - 0xC000));
  r[0x3FFA] = 0x00; r[0x3FFB] = 0xC1;  // NMI
  r[0x3FFC] = 0x00; r[0x3FFD] = 0xC0;  // reset
  r[0x3FFE] = 0x00; r[0x3FFF] = 0xC1;
  if (type < 0) return r;
  std::vector<uint8_t> h(128, 0);
  h[0] = 3;
  std::memcpy(h.data() + 1, "ATARI7800", 9);
  h[51] = 0x40;  // 16 KB
  h[53] = uint8_t(type >> 8); h[54] = uint8_t(type);
  h[55] = h[56] = 1;
  h.insert(h.end(), r.begin(), r.end());
  return h;
}
// frequência da onda (cruzamentos de zero para cima por segundo, a 48 kHz)
static double toneHz(const std::vector<float>& a, size_t skip) {
  int n = 0;
  size_t first = 0, last = 0;
  for (size_t i = skip + 1; i < a.size(); i++)
    if (a[i - 1] < 0 && a[i] >= 0) { if (!n) first = i; last = i; n++; }
  return n > 1 ? (n - 1) * 48000.0 / double(last - first) : 0;
}
static void syntheticTests() {
  // tom puro da TIA: AUDC0=4 (divisão por 2), AUDF0=31: 3579545/114/32/2 = 490,6 Hz
  {
    A7800Core c(makeRom({0x78, 0xD8, 0xA9, 0x04, 0x85, 0x15, 0xA9, 0x1F, 0x85, 0x17, 0xA9, 0x0F, 0x85, 0x19,
                         0x4C, 0x0E, 0xC0}, {0x40}, {}), nullptr);
    std::vector<float> a;
    run(c, 120, {}, &a);
    const double f = toneHz(a, 12000);
    char b[96]; std::snprintf(b, sizeof b, "tom da TIA: %.1f Hz (esperado 490,6)", f);
    std::printf("%s\n", b);
    check(std::fabs(f - 490.6) < 2, b);
  }
  // tom puro do POKEY em $4000: AUDF1=99, AUDC1=$AF, base de 64 kHz: 1789772/28/100/2 = 319,6 Hz
  {
    A7800Core c(makeRom({0xA9, 0x03, 0x8D, 0x0F, 0x40, 0xA9, 0x00, 0x8D, 0x08, 0x40, 0xA9, 0x63, 0x8D, 0x00, 0x40,
                         0xA9, 0xAF, 0x8D, 0x01, 0x40, 0x4C, 0x14, 0xC0}, {0x40}, {}, 0x0001), nullptr);
    std::vector<float> a;
    run(c, 120, {}, &a);
    const double f = toneHz(a, 12000);
    char b[96]; std::snprintf(b, sizeof b, "tom do POKEY: %.1f Hz (esperado 319,6)", f);
    std::printf("%s\n", b);
    check(std::fabs(f - 319.6) < 2, b);
  }
  // velocidade da CPU: laço "LDA zp; JMP" = 6 acessos. Na RAM ($80) 24 ciclos do mestre; na TIA ($08) um acesso
  // é lento (6 em vez de 4): 26. Quadro = 454 x 263 ciclos do mestre.
  for (int slow = 0; slow < 2; slow++) {
    A7800Core c(makeRom({0x78, 0xA5, uint8_t(slow ? 0x08 : 0x80), 0x4C, 0x01, 0xC0}, {0x40}, {}), nullptr);
    run(c, 2, {});
    const uint64_t c0 = c.m().cpuCycles;
    run(c, 10, {});
    const double perFrame = double(c.m().cpuCycles - c0) / 10;
    const double want = 454.0 * 263 * 6 / (slow ? 26 : 24);
    char b[128]; std::snprintf(b, sizeof b, "ciclos de CPU por quadro %s: %.1f (esperado %.1f)",
                               slow ? "lendo a TIA (1,19 MHz)" : "na RAM (1,79 MHz)", perFrame, want);
    std::printf("%s\n", b);
    check(std::fabs(perFrame - want) < 2, b);
  }
  // DMA do MARIA: DLL em $D000 com 16 zonas de 16 linhas, cada zona com 1 objeto direto de 8 bytes. Cada linha
  // custa 16 (partida) + 8 (cabeçalho) + 8x3 (gráficos) = 48 ciclos, mais 8 na última linha da zona. A zona 3
  // tem DLI: o NMI soma 1 em $80 a cada quadro. O laço principal faz STA WSYNC e soma 1 em $81 por linha.
  {
    std::vector<uint8_t> dll;
    for (int z = 0; z < 16; z++) { dll.push_back(uint8_t((z == 3 ? 0x80 : 0) | 15)); dll.push_back(0xD1); dll.push_back(0x00); }
    const std::vector<uint8_t> dlist = {0x00, 0x18, 0xE0, 0x10, 0x00, 0x00};
    const std::vector<uint8_t> code = {0x78, 0xD8, 0xA9, 0xD0, 0x85, 0x2C, 0xA9, 0x00, 0x85, 0x30, 0xA9, 0x40, 0x85, 0x3C,
                                       0x85, 0x24, 0xE6, 0x81, 0x4C, 0x0E, 0xC0};  // laço em $C00E
    A7800Core c(makeRom(code, {0xE6, 0x80, 0x40}, {{0xD000, dll}, {0xD100, dlist}}), nullptr);
    run(c, 3, {});
    const uint8_t n0 = c.m().peek(0x80), l0 = c.m().peek(0x81);
    run(c, 5, {});
    const int nmis = uint8_t(c.m().peek(0x80) - n0), wl = uint8_t(c.m().peek(0x81) - l0);
    check(nmis == 5, "DLI: 1 NMI por quadro (" + std::to_string(nmis) + " em 5 quadros)");
    check(wl == (263 * 5) % 256, "WSYNC: 1 volta por linha (" + std::to_string(wl) + ", esperado " +
                                     std::to_string((263 * 5) % 256) + ")");
    check(c.m().lastDmaCycles == 48 || c.m().lastDmaCycles == 56,
          "ciclos de DMA da linha: " + std::to_string(c.m().lastDmaCycles) + " (esperado 48 ou 56)");
    std::printf("DMA: %d ciclos na última linha; NMIs %d; voltas de WSYNC %d\n", c.m().lastDmaCycles, nmis, wl);
  }
}

// dois jogadores: o joystick 2 chega aos bits baixos do SWCHA e aos INPT2/3/5, sem mexer no joystick 1
static void player2Test() {
  std::vector<uint8_t> rom(0x8000, 0xEA);
  rom[0x7FFC] = 0x00; rom[0x7FFD] = 0x80;
  A7800Core c(rom, nullptr);
  a7800::A7800& m = c.m();
  check(c.players() == 2, "2 jogadores: players() == 2");
  c.setKeys(0);
  c.setKeys2(KEY_UP | KEY_LEFT | KEY_A);
  check(m.read(0x280) == 0xFA, "2 jogadores: SWCHA com o jogador 2 em cima+esquerda (lido " + std::to_string(m.read(0x280)) + ")");
  check(m.read(0x0B) == 0x80 && m.read(0x0A) == 0 && m.read(0x09) == 0 && m.read(0x08) == 0,
        "2 jogadores: botão esquerdo do jogador 2 só no INPT3");
  check(m.read(0x0D) == 0 && m.read(0x0C) == 0x80, "2 jogadores: modo de um botão: INPT5 apertado, INPT4 solto");
  m.write(0x283, 0x14);  // SWCHB: pinos 6 das duas portas em saída baixa = joysticks de dois botões
  m.write(0x282, 0x00);
  check(m.read(0x0D) == 0x80, "2 jogadores: modo de dois botões: INPT5 solto");
  c.setKeys(KEY_RIGHT | KEY_B);
  c.setKeys2(0);
  check(m.read(0x280) == 0x7F, "2 jogadores: jogador 1 sozinho não mexe nos bits do jogador 2");
  check(m.read(0x08) == 0x80 && m.read(0x0A) == 0, "2 jogadores: botão direito do jogador 1 só no INPT0");
}

int main(int argc, char** argv) {
  if (argc >= 5 && !std::strcmp(argv[1], "--png")) {
    const auto rom = readAll(argv[2]);
    A7800Core c(rom, nullptr);
    std::vector<std::string> keys(argv + 5, argv + argc);
    run(c, std::atoi(argv[3]), keys);
    writePng(argv[4], c.framebuffer(), c.width(), c.height());
    std::printf("%016llx\n", (unsigned long long)screenHash(c));
    return 0;
  }
  if (argc >= 4 && !std::strcmp(argv[1], "--hash")) {
    const auto rom = readAll(argv[2]);
    A7800Core c(rom, nullptr);
    std::vector<std::string> keys(argv + 4, argv + argc);
    run(c, std::atoi(argv[3]), keys);
    std::printf("%016llx\n", (unsigned long long)screenHash(c));
    return 0;
  }
  if (argc >= 4 && !std::strcmp(argv[1], "--wav")) {  // grava o som (mono 16 bits, 48 kHz)
    const auto rom = readAll(argv[2]);
    A7800Core c(rom, nullptr);
    std::vector<std::string> keys(argv + 5, argv + argc);
    std::vector<float> a;
    run(c, std::atoi(argv[3]), keys, &a);
    double e = 0;
    for (float v : a) e += double(v) * v;
    std::printf("%zu amostras, rms %.4f\n", a.size(), a.empty() ? 0 : std::sqrt(e / a.size()));
    if (argc >= 5) {
      std::ofstream f(argv[4], std::ios::binary);
      auto le = [&](uint32_t v, int n) { for (int k = 0; k < n; k++) f.put(char((v >> (8 * k)) & 0xFF)); };
      f.write("RIFF", 4); le(36 + uint32_t(a.size()) * 2, 4); f.write("WAVEfmt ", 8); le(16, 4); le(1, 2); le(1, 2);
      le(48000, 4); le(96000, 4); le(2, 2); le(16, 2); f.write("data", 4); le(uint32_t(a.size()) * 2, 4);
      for (float v : a) le(uint32_t(int16_t(std::max(-1.f, std::min(1.f, v)) * 32000)), 2);
    }
    return 0;
  }
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta\n", argv[0]); return 2; }
  const std::string dir = argv[1];
  if (!std::filesystem::is_directory(dir)) {
    std::printf("Atari 7800: pasta %s não existe, testes pulados\n", dir.c_str());
    return 0;
  }
  syntheticTests();
  player2Test();
  // 1) telas de referência
  for (const Ref& r : kRefs) {
    const std::string p = dir + "/" + r.rom;
    const auto rom = readAll(p);
    if (rom.empty()) { check(false, std::string("falta ") + r.rom); continue; }
    A7800Core c(rom, nullptr);
    run(c, r.frames, r.keys);
    const uint64_t h = screenHash(c);
    char b[160];
    std::snprintf(b, sizeof b, "tela de %s (quadro %d): %016llx, esperado %016llx", r.rom, r.frames,
                  (unsigned long long)h, (unsigned long long)r.hash);
    check(h == r.hash, b);
    std::printf("%s %s\n", h == r.hash ? "ok  " : "ERRO", b);
  }
  // 2) todas as ROMs da pasta: rodam, mostram algo, estado determinístico e salvar→carregar→salvar igual
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    if (e.path().extension() != ".a78") continue;
    const std::string name = e.path().filename().string();
    const auto rom = readAll(e.path().string());
    A7800Core c(rom, nullptr);
    run(c, 120, {"S@30", "-S@34"});
    const auto st = c.saveState();
    std::vector<float> a1, a2;
    run(c, 60, {"A@10", "-A@20", "R@25"}, &a1);
    const uint64_t h1 = screenHash(c);
    c.loadState(st);
    run(c, 60, {"A@10", "-A@20", "R@25"}, &a2);
    const uint64_t h2 = screenHash(c);
    check(h1 == h2 && a1 == a2, name + ": estado salvo determinístico");
    c.loadState(st);
    {
      const auto st2 = c.saveState();
      size_t d = 0;
      while (d < st.size() && d < st2.size() && st[d] == st2[d]) d++;
      check(st2 == st, name + ": salvar->carregar->salvar dá os mesmos bytes (difere no byte " + std::to_string(d) +
                           " de " + std::to_string(st.size()) + ")");
    }
    // estados danificados: lança erro ou continua sem travar
    std::vector<uint8_t> bad = st;
    for (size_t i = 8; i < bad.size(); i += 97) bad[i] ^= 0x5A;
    try { c.loadState(bad); } catch (const std::exception&) {}
    run(c, 3, {});
    std::vector<uint8_t> cut(st.begin(), st.begin() + static_cast<long>(st.size() / 2));
    bool threw = false;
    try { c.loadState(cut); } catch (const std::exception&) { threw = true; }
    check(threw, name + ": estado truncado recusado");
    run(c, 2, {});
  }
  // 3) SaveKey (EEPROM I2C): a tabela de recordes do 7800basic grava o placar depois do quadro 256
  {
    const auto rom = readAll(dir + "/hiscoretable.a78");
    if (!rom.empty()) {
      A7800Core c(rom, nullptr);
      std::vector<std::string> keys;
      for (int f = 300; f < 1500; f += 40) { keys.push_back("A@" + std::to_string(f)); keys.push_back("-A@" + std::to_string(f + 5)); }
      run(c, 1500, keys);
      const auto e = c.exportSave();
      int used = 0;
      for (uint8_t b : e) used += b != 0xFF;
      std::printf("SaveKey: %d bytes gravados na EEPROM\n", used);
      check(c.hasSave() && used > 0, "SaveKey: o jogo gravou na EEPROM");
      // relida: a tabela do próximo jogo já mostra o recorde gravado (tela diferente de sem save)
      A7800Core fresh(rom, nullptr), saved(rom, &e);
      run(fresh, 300, {});
      run(saved, 300, {});
      check(screenHash(fresh) != screenHash(saved), "SaveKey: recorde gravado aparece ao ligar de novo");
      if (std::getenv("A7800_PNG")) writePng(std::getenv("A7800_PNG"), saved.framebuffer(), saved.width(), saved.height());
    }
  }
  // 4) ROM sem cabeçalho e lixo
  {
    std::vector<uint8_t> junk(0x8000);
    uint32_t s = 12345;
    for (auto& b : junk) { s = s * 1103515245 + 12345; b = static_cast<uint8_t>(s >> 16); }
    A7800Core c(junk, nullptr);  // sem cabeçalho: o registro só aceita pela extensão .a78
    run(c, 10, {});
    check(true, "ROM de lixo roda 10 quadros sem travar");
    // todos os tipos de cartucho com conteúdo aleatório, e estados aleatórios
    const int types[] = {0x0000, 0x0001, 0x0002, 0x0006, 0x000A, 0x0012, 0x0022, 0x0040, 0x0046, 0x0080, 0x0100,
                         0x0200, 0x1000, 0x1003, 0x2000, 0x2002, 0x6002, 0x6000, 0x8000, 0xA002};
    const size_t sizes[] = {0x2000, 0xC000, 0xD000, 0x10000, 0x20000, 0x24000, 0x80000};
    int k = 0;
    for (int t : types) {
      for (size_t sz : sizes) {
        std::vector<uint8_t> r(128 + sz);
        for (auto& b : r) { s = s * 1103515245 + 12345; b = static_cast<uint8_t>(s >> 16); }
        std::memcpy(r.data() + 1, "ATARI7800", 9);
        r[49] = uint8_t(sz >> 24); r[50] = uint8_t(sz >> 16); r[51] = uint8_t(sz >> 8); r[52] = uint8_t(sz);
        r[53] = uint8_t(t >> 8); r[54] = uint8_t(t);
        A7800Core g(r, nullptr);
        run(g, 4, {"A@1", "S@2"});
        auto st = g.saveState();
        for (size_t i = 4; i < st.size(); i++) { s = s * 1103515245 + 12345; if ((s >> 20) % 7 == 0) st[i] = uint8_t(s >> 8); }
        try { g.loadState(st); } catch (const std::exception&) {}
        run(g, 2, {});
        k++;
      }
    }
    check(true, "cartuchos aleatórios de todos os tipos e estados aleatórios (" + std::to_string(k) + " casos) sem travar");
  }
  std::printf("RESULTADO a7800: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
