// Testes do núcleo Atari Jaguar.
//   indigo-t7_jaguar pasta                    roda as verificações (pasta = /root/indigo-env/roms/jaguar, preparada
//                                             por tools/t7_jaguar.sh: o cartucho de teste teste.j64)
//   indigo-t7_jaguar --png rom quadros saida.png    grava a tela (para conferir a olho)
// O cartucho de teste (tools/t7_jaguar_rom.s) foi escrito para o Índigo; não há ROM nem BIOS comercial aqui.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/jaguar/jaguar.h"

using namespace indigo;
using indigo::jaguar::Jaguar;
using indigo::jaguar::JaguarCore;

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static int checks = 0, fails = 0;
static void check(bool ok, const char* what, const std::string& detail = "") {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s %s\n", what, detail.c_str()); }
}
static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }

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
    f.write((const char*)td.data(), static_cast<long>(td.size()));
    be(crc32(td.data(), td.size()));
  };
  f.write("\x89PNG\r\n\x1a\n", 8);
  std::vector<uint8_t> ih = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                             uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ih);
  chunk("IDAT", z);
  chunk("IEND", {});
}

static uint32_t rgbAt(JaguarCore& c, int x, int y) { return c.framebuffer()[y * c.width() + x] & 0xFFFFFF; }
static uint32_t rgb(int r, int g, int b) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16); }

static uint64_t hashCore(JaguarCore& c) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint32_t v) { h = (h ^ v) * 1099511628211ull; };
  for (int i = 0; i < c.width() * c.height(); i++) mix(c.framebuffer()[i]);
  for (uint32_t a = 0x3000; a < 0x3040; a += 4) mix(c.jag().read32(a));
  mix(c.jag().read32(0xF1C000));
  return h;
}

// ---------------------------------------------------------------- testes do cartucho
static void testCart(const std::vector<uint8_t>& rom) {
  std::vector<uint8_t> save(128, 0xFF);
  save[12] = 0x12; save[13] = 0x34;  // palavra 6 da EEPROM
  auto core = createCore(rom, "teste.j64", &save);
  check(core && core->system() == "jaguar", "reconhece o .j64");
  if (!core) return;
  auto& c = static_cast<JaguarCore&>(*core);
  Jaguar& j = c.jag();
  c.setSampleRate(48000);
  for (int f = 0; f < 60; f++) { c.runFrame(); float l[4096], r[4096]; while (c.audioAvailable()) c.pullAudio(l, r, std::min<size_t>(4096, c.audioAvailable())); }

  // tela
  check(c.width() == 326 && c.height() == 240, "tamanho da tela 326x240", std::to_string(c.width()) + "x" + std::to_string(c.height()));
  const uint32_t BLUE = rgb(0, 0, 254), RED = rgb(254, 0, 0), WHITE = rgb(246, 254, 229), DBLUE = rgb(0, 0, 135), BLACK = 0;
  struct P { int x, y; uint32_t c; const char* what; };
  const P pts[] = {
      {13, 10, BLUE, "bitmap 8 bits (CLUT 1)"},      {44, 29, BLUE, "bitmap 8 bits, fim da metade azul"},
      {45, 10, RED, "bitmap 8 bits (CLUT 2)"},       {76, 20, RED, "bitmap 8 bits, último ponto"},
      {77, 20, BLACK, "fundo depois do bitmap"},     {13, 30, BLACK, "fundo abaixo do bitmap"},
      {12, 20, BLACK, "fundo antes do bitmap"},      {13, 15, WHITE, "bitmap 1 bit (bit 1)"},
      {16, 18, WHITE, "bitmap 1 bit, linha 18"},     {17, 15, BLUE, "bitmap 1 bit transparente (bit 0)"},
      {53, 16, WHITE, "bitmap 1 bit sobre o vermelho"}, {57, 16, RED, "transparente sobre o vermelho"},
      {13, 19, BLUE, "bitmap 1 bit só 4 linhas"},    {103, 50, DBLUE, "bitmap 16 bits CRY"},
      {110, 59, DBLUE, "bitmap 16 bits, canto"},     {111, 55, BLACK, "fundo depois do bitmap 16 bits"},
      {103, 60, BLACK, "bitmap 16 bits só 10 linhas"}, {203, 80, BLUE, "escala 2x: ponto 0"},
      {204, 80, BLUE, "escala 2x: ponto 0 repetido"}, {205, 81, RED, "escala 2x: ponto 1"},
      {218, 87, RED, "escala 2x: último ponto"},      {219, 84, BLACK, "escala 2x: largura 16"},
      {203, 88, BLACK, "escala 2x: altura 8"},        {153, 110, RED, "objeto antes do desvio"},
      {160, 120, RED, "objeto na linha do desvio"},   {153, 121, BLACK, "desvio para a parada"},
      {5, 5, BLACK, "fundo"},                         {300, 200, BLACK, "fundo"},
  };
  for (const auto& p : pts) {
    const uint32_t got = rgbAt(c, p.x, p.y);
    check(got == p.c, p.what, "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ") = " + hex(got) + " esperado " + hex(p.c));
  }

  // GPU
  check(j.read32(0x3030) == 0x600DF00D, "GPU terminou o programa", hex(j.read32(0x3030)));
  const uint32_t a = 0x12345678, b = 0x0F0F0F0F;
  const uint32_t exp[] = {a + b, a - b, a & b, a | b, a ^ b, ~a, 0u - b, uint32_t(0x5678u * 0x0F0Fu),
                          uint32_t(int32_t(int16_t(0x5678)) * int16_t(0x8000)), 142857, 1, 0x23456780, 0x00123456,
                          0xF8000001, 0x23456780, 0x78123456, 0x81234567, 255, 77, 0, 0xFFFFFFE9, 42, a, 55, 7, 1, 2,
                          0xA1, 0xC3D4, 0x15B2C3D4, 0x55AA55AA, 0x55AA55AA, 1, 0x11112222, 0xCAFEBABE, 0, 0x80000001, 1, 2,
                          0x4000};
  static const char* NAMES[] = {"ADD", "SUB", "AND", "OR", "XOR", "NOT", "NEG", "MULT", "IMULT", "DIV", "resto da DIV",
                                "SHLQ", "SHRQ", "SHARQ", "SH", "ROR", "RORQ", "SAT8", "ABS", "NORMI", "NORMI 1",
                                "IMULTN/IMACN/RESMAC", "MOVETA/MOVEFA", "laço com JR", "JUMP com atraso", "CMP/JR PL",
                                "CMP/JR CC", "LOADB", "LOADW", "STOREB", "LOAD (R14+n)", "LOAD (R14+Rm)", "ADDC",
                                "STOREP/LOADP", "HIDATA", "MOVE PC", "BSET", "BCLR", "SUBC", "DIV 16.16"};
  for (int i = 0; i < 40; i++) {
    const uint32_t got = j.read32(0x20000 + i * 4);
    if (i == 35) {  // MOVE PC: o endereço da própria instrução (0xCC04 = MOVE PC,r4) na RAM do GPU
      check(got >= 0xF03000 && got < 0xF04000 && j.read16(got) == 0xCC04, "GPU MOVE PC", hex(got));
      continue;
    }
    check(got == exp[i], (std::string("GPU ") + NAMES[i]).c_str(), hex(got) + " esperado " + hex(exp[i]));
  }

  // Blitter: quadro de 64 pontos de 16 bits em 0x40000
  auto fbp = [&](int x, int y) { return j.read16(0x40000 + (y * 64 + x) * 2); };
  check(j.read32(0x3034) == 0xB1170000, "68000 terminou os blits");
  bool fill = true;
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 16; x++) {
      const bool in = x >= 4 && x < 12 && y >= 2 && y < 7;
      if (fbp(x, y) != (in ? 0xF0FF : 0)) fill = false;
    }
  check(fill, "Blitter: preenchimento em modo phrase (8x5)");
  bool copy = true;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 8; x++)
      if (fbp(20 + x, 10 + y) != ((1 + y) * 256 + 2 + x + 0x1000)) copy = false;
  check(copy && fbp(28, 10) == 0 && fbp(20, 14) == 0, "Blitter: cópia de retângulo (A2 -> A1, passos)");
  bool trans = true;
  for (int x = 0; x < 8; x++) if (fbp(30 + x, 12) != ((x & 1) ? 0x2222 : 0x5555)) trans = false;
  check(trans, "Blitter: transparência (DCOMPEN)");
  bool bits = true;
  for (int x = 0; x < 8; x++) if (fbp(x, 20) != (((0xA5 >> (7 - x)) & 1) ? 0x1234 : 0)) bits = false;
  check(bits, "Blitter: expansão de 1 bit (BCOMPEN)");
  bool gour = true;
  for (int x = 0; x < 8; x++) if (fbp(x, 25) != 0x7710 + x) gour = false;
  check(gour, "Blitter: Gouraud", hex(fbp(0, 25)) + " " + hex(fbp(7, 25)));
  bool clip = true;
  for (int x = 56; x < 64; x++) if (fbp(x, 30) != (x < 62 ? 0x7777 : 0)) clip = false;
  check(clip, "Blitter: recorte da janela A1");
  check(j.blits == 6, "Blitter: 6 blits", std::to_string(j.blits));

  // interrupções do 68000 (contadores do programa)
  const uint32_t vbl = j.read32(0x3000), gpuInt = j.read32(0x3004), pit = j.read32(0x3008), jt2 = j.read32(0x300C);
  check(vbl >= 58 && vbl <= 60, "interrupção de vídeo uma vez por quadro", std::to_string(vbl));
  check(gpuInt == 1, "GPU interrompe o 68000 (CPUINT)", std::to_string(gpuInt));
  check(pit >= 960 && pit <= 1001, "PIT do Tom a ~1000 Hz", std::to_string(pit));
  check(jt2 >= 95 && jt2 <= 101, "temporizador 2 do Jerry a ~100 Hz (68000 pelo Tom)", std::to_string(jt2));

  // EEPROM
  check(j.read16(0x3020) == 0xBEEF, "EEPROM: grava e lê a palavra 5", hex(j.read16(0x3020)));
  check(j.read16(0x3022) == 0x1234, "EEPROM: lê o save importado (palavra 6)", hex(j.read16(0x3022)));
  check(c.hasSave() && c.saveDirty(), "EEPROM: save alterado");
  const auto ex = c.exportSave();
  check(ex.size() == 128 && ex[10] == 0xBE && ex[11] == 0xEF && ex[12] == 0x12, "EEPROM: save exportado");

  // DSP: temporizador 1 e som (onda quadrada de 20774/48 Hz, esquerdo = -direito)
  const uint32_t t1 = j.read32(0xF1C000);
  std::vector<float> L, R;
  for (int f = 0; f < 60; f++) {
    c.runFrame();
    std::vector<float> l(c.audioAvailable()), r(c.audioAvailable());
    c.pullAudio(l.data(), r.data(), l.size());
    L.insert(L.end(), l.begin(), l.end());
    R.insert(R.end(), r.begin(), r.end());
  }
  const uint32_t t1b = j.read32(0xF1C000);
  check(t1 > 0 && t1b - t1 >= 95 && t1b - t1 <= 101, "DSP: temporizador 1 do Jerry a ~100 Hz", std::to_string(t1b - t1));
  check(L.size() >= 47800 && L.size() <= 48200, "som: 48000 amostras por segundo", std::to_string(L.size()));
  int cross = 0;
  float peak = 0;
  bool opposite = true;
  for (size_t i = 1; i < L.size(); i++) {
    if ((L[i] > 0) != (L[i - 1] > 0)) cross++;
    peak = std::max(peak, std::fabs(L[i]));
    if (std::fabs(L[i] + R[i]) > 1e-6f) opposite = false;
  }
  const double freq = cross / 2.0 * 48000.0 / double(L.size());
  check(std::fabs(freq - 26590906.0 / 1280 / 48) < 4, "som: frequência da onda do DSP (I2S)", std::to_string(freq));
  check(std::fabs(peak - 8000 / 32768.0f) < 0.001f, "som: amplitude", std::to_string(peak));
  check(opposite, "som: canais estéreo (esquerdo = -direito)");

  // controle (o programa lê a linha 0 da porta 1 o tempo todo)
  c.setKeys(KEY_UP | KEY_A);
  c.runFrame(); c.runFrame();
  const uint16_t joy = j.read16(0x3010), but = j.read16(0x3012);
  check(!(joy & 0x100) && (joy & 0xE00) == 0xE00, "controle: direcional para cima", hex(joy));
  check(!(but & 2) && (but & 1) && (but & 0x10), "controle: botão A (e NTSC)", hex(but));
  c.setKeys(KEY_START);
  c.runFrame(); c.runFrame();
  check((j.read16(0x3010) & 0xF00) == 0xF00 && !(j.read16(0x3012) & 1) && (j.read16(0x3012) & 2), "controle: Pause");
  c.setKeys(0);

  // save state: o mesmo futuro depois de carregar
  const auto st = c.saveState();
  for (int f = 0; f < 30; f++) c.runFrame();
  const uint64_t h1 = hashCore(c);
  c.loadState(st);
  for (int f = 0; f < 30; f++) c.runFrame();
  check(hashCore(c) == h1, "save state: mesma tela, RAM e DSP 30 quadros depois");
  std::vector<uint8_t> bad = st;
  bad.resize(bad.size() / 2);
  bool threw = false;
  try { c.loadState(bad); } catch (const std::exception&) { threw = true; }
  c.runFrame();
  check(threw && hashCore(c) != 0, "save state truncado é recusado");
}

// ---------------------------------------------------------------- segundo cartucho (RGB16, OP com GPU, Blitter pelo GPU, DSP)
static void testCart2(const std::vector<uint8_t>& rom) {
  auto core = createCore(rom, "teste2.j64", nullptr);
  check(core && core->system() == "jaguar", "reconhece o teste2.j64");
  if (!core) return;
  auto& c = static_cast<JaguarCore&>(*core);
  Jaguar& j = c.jag();
  for (int f = 0; f < 30; f++) c.runFrame();
  const uint32_t R = rgb(255, 0, 0), G = rgb(0, 255, 0), B = rgb(0, 0, 255), W = rgb(255, 255, 255), BG = rgb(0, 125, 0);
  struct P { int x, y; uint32_t c; const char* what; };
  const P pts[] = {
      {23, 10, R, "RGB16: vermelho"}, {24, 10, G, "RGB16: verde"}, {25, 11, B, "RGB16: azul"}, {26, 13, W, "RGB16: branco"},
      {30, 12, W, "RGB16: último ponto"}, {31, 12, BG, "fundo (BGEN, BG escrito por byte)"}, {23, 14, BG, "fundo abaixo"},
      {63, 10, R, "refletido: ponto 0 em x"}, {62, 10, G, "refletido: ponto 1 à esquerda"}, {60, 11, W, "refletido: ponto 3"},
      {56, 13, W, "refletido: ponto 7"}, {64, 12, BG, "refletido: nada à direita"}, {55, 12, BG, "refletido: largura 8"},
      {103, 20, G, "4 bits: CLUT 0x30"}, {104, 20, R, "4 bits: CLUT 0x31"}, {118, 23, R, "4 bits: último ponto"},
      {119, 22, BG, "4 bits: largura 16"}, {5, 5, BG, "fundo"},
  };
  for (const auto& p : pts) {
    const uint32_t got = rgbAt(c, p.x, p.y);
    check(got == p.c, p.what, "(" + std::to_string(p.x) + "," + std::to_string(p.y) + ") = " + hex(got) + " esperado " + hex(p.c));
  }
  check(j.read32(0x3030) == 0x600DF00D, "GPU terminou os blits");
  auto b8 = [&](int x, int y) { return j.read8(0x40000 + y * 64 + x); };
  bool cp = true;
  for (int y = 0; y < 2; y++)
    for (int x = 0; x < 16; x++) if (b8(8 + x, 1 + y) != uint8_t((2 + y) * 64 + 16 + x)) cp = false;
  check(cp && b8(7, 1) == 0 && b8(24, 1) == 0 && b8(8, 3) == 0, "Blitter (GPU): cópia de 8 bits em modo phrase");
  bool inc = true;
  for (int x = 0; x < 8; x++) if (b8(x, 5) != x / 2) inc = false;
  check(inc && b8(8, 5) == 0, "Blitter: passo fracionário (XADDINC, A1 -> A2)");
  const uint16_t zexp[16] = {0, 0, 0, 0, 0, 10, 20, 30, 0x4444, 0x4444, 0x4444, 0x4444, 35, 35, 35, 35};
  bool z = true;
  std::string zs;
  for (int i = 0; i < 16; i++) { const uint16_t v = j.read16(0x42000 + i * 2); zs += hex(v).substr(4) + " "; if (v != zexp[i]) z = false; }
  check(z, "Blitter: Z-buffer (DSTENZ, DSTWRZ, phrases intercaladas)", zs);
  bool x = true;
  for (int i = 0; i < 8; i++) if (j.read16(0x43000 + i * 2) != 0x0FF0) x = false;
  check(x, "Blitter: LFU com o destino (XOR)");
  const uint32_t frames = j.read32(0x3000), lines = j.read32(0x3044);
  check(lines >= (frames - 2) * 240 && lines <= (frames + 1) * 240 + 2, "objeto do GPU: interrupção 3 em cada linha, OBF libera o OP",
        std::to_string(lines) + " em " + std::to_string(frames) + " quadros");
  check(j.read32(0x3040) == 1, "68000 força a interrupção 0 do GPU", std::to_string(j.read32(0x3040)));
  const uint16_t vc = j.read16(0x3010);
  check(vc == 519 || vc == 520, "VC lido na interrupção de vídeo (VI = 519)", std::to_string(vc));
  const uint32_t dexp[] = {0xFFF80008, 1, 0x7FFFFFFF, 0x1E6A2C48, 32767, 0xFFFF8000, 0x100004, 0x1000FC};
  static const char* DN[] = {"MAC 40 bits (RESMAC)", "D_MACHI", "SAT32S", "MIRROR", "SAT16S +", "SAT16S -", "ADDQMOD", "SUBQMOD"};
  for (int i = 0; i < 8; i++) {
    const uint32_t got = j.read32(0xF1C000 + i * 4);
    check(got == dexp[i], (std::string("DSP ") + DN[i]).c_str(), hex(got) + " esperado " + hex(dexp[i]));
  }
  check(j.read32(0x300C) == 1, "DSP interrompe o 68000 (Jerry -> Tom)", std::to_string(j.read32(0x300C)));
}

// ---------------------------------------------------------------- programas para a RAM e reconhecimento
static void testLoaders(const std::vector<uint8_t>& cart) {
  // programa mínimo: move.l #$600DF00D,$3000 / bra.s *
  const uint8_t code[] = {0x23, 0xFC, 0x60, 0x0D, 0xF0, 0x0D, 0x00, 0x00, 0x30, 0x00, 0x60, 0xFE};
  auto put32 = [](std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = uint8_t(v >> (24 - 8 * i)); };
  std::vector<uint8_t> abs(0x24, 0);
  abs[0] = 0x60; abs[1] = 0x1B;
  put32(abs, 2, sizeof code);
  put32(abs, 0x16, 0x5000);
  abs.insert(abs.end(), code, code + sizeof code);
  std::vector<uint8_t> jagr(0x2E, 0);
  jagr[0] = 0x60; jagr[1] = 0x1A; jagr[0x1C] = 'J'; jagr[0x1D] = 'A'; jagr[0x1E] = 'G'; jagr[0x1F] = 'R';
  jagr[0x21] = 3;
  put32(jagr, 0x22, 0x6000); put32(jagr, 0x26, sizeof code); put32(jagr, 0x2A, 0x6000);
  jagr.insert(jagr.end(), code, code + sizeof code);
  for (int k = 0; k < 2; k++) {
    const char* name = k == 0 ? "prog.abs" : "prog.jag";
    auto core = createCore(k == 0 ? abs : jagr, name, nullptr);
    check(core && core->system() == "jaguar", (std::string("reconhece ") + name).c_str());
    if (!core) continue;
    core->runFrame(); core->runFrame();
    auto& c = static_cast<JaguarCore&>(*core);
    check(c.jag().read32(0x3000) == 0x600DF00D, (std::string("roda o programa para a RAM ") + name).c_str(), hex(c.jag().read32(0x3000)));
    check(!c.hasSave() && c.title() == "prog", (std::string("sem EEPROM e título de ") + name).c_str());
  }
  // .rom: só com a marca do cartucho do Jaguar
  auto r1 = createCore(cart, "teste.rom", nullptr);
  check(r1 && r1->system() == "jaguar", "reconhece .rom com a marca 0x04040404");
  std::vector<uint8_t> msx(0x8000, 0);
  msx[0] = 'A'; msx[1] = 'B'; msx[2] = 0x10; msx[3] = 0x40;
  auto r2 = createCore(msx, "jogo.rom", nullptr);
  check(r2 && r2->system() != "jaguar", "não pega o .rom do MSX", r2 ? r2->system() : "nulo");
  std::vector<uint8_t> junk(0x10000, 0x11);
  check(!jaguar::isJaguarRom(junk, "x.jag") && !jaguar::isJaguarRom(junk, "x.bin") && jaguar::isJaguarRom(junk, "x.j64"),
        "reconhecimento pela extensão");
}

// casos que já derrubaram o núcleo (pilha estourada) e bordas da API
static void testRobustez(const std::vector<uint8_t>& cart) {
  {  // GPU numa sequência longa de JR: a instrução de atraso não pode virar recursão sem fim
    Jaguar j(cart, "x.j64");
    for (uint32_t a = 0x10000; a < 0x200000; a += 2) { j.dram[a] = 0xD4; j.dram[a + 1] = 0x00; }
    j.gpu.pc = 0x10000; j.gpu.ctrl = 1;
    j.gpu.run(j.gpu.clk + 400000);  // só o GPU (o programa do cartucho mexeria nele)
    check(j.gpu.pc >= 0x10000 && j.gpu.pc < 0x200000, "GPU com JR em sequência não estoura a pilha", hex(j.gpu.pc));
  }
  {  // Blitter gravando em B_CMD: não pode começar outra cópia no meio da atual
    Jaguar j(cart, "x.j64");
    const uint64_t antes = j.blits;
    j.write32(0xF02200, 0xF02238);        // A1_BASE = B_CMD
    j.write32(0xF02204, 5u << 3);         // A1_FLAGS: 32 bits, modo pixel
    j.write32(0xF02268, 0x00010000); j.write32(0xF0226C, 0x00010000);  // B_PATD = o próprio comando
    j.write32(0xF0223C, 0x00010001);      // B_COUNT = 1x1
    j.write32(0xF02238, 0x00010000);      // B_CMD: PATDSEL
    check(j.blits - antes == 1, "Blitter com destino nos próprios registros roda uma vez", std::to_string(j.blits - antes));
  }
  {  // BIOS de tamanho ímpar: a leitura de 16 bits dá a volta, sem ler além do fim
    Jaguar j(cart, "x.j64");
    j.bios = {0x11, 0x22, 0x33};
    check(j.read16(0xE00002) == 0x3311, "BIOS de tamanho ímpar", hex(j.read16(0xE00002)));
  }
  {  // .j64 é cartucho mesmo começando com 0x601B
    std::vector<uint8_t> c = cart;
    c[0] = 0x60; c[1] = 0x1B;
    Jaguar j(c, "jogo.J64");
    check(!j.ramProgram && !j.cart.empty(), ".j64 que começa com 0x601B continua cartucho");
  }
}

// nenhum arquivo dos outros consoles (pasta das ROMs de teste) pode virar Jaguar
static void testOthers(const std::string& romsDir, const std::string& jagDir) {
  const std::string propria = std::filesystem::weakly_canonical(jagDir).string();
  namespace fs = std::filesystem;
  int seen = 0, stolen = 0;
  std::string first;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(romsDir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    const auto& p = it->path();
    if (p.string().find("/jaguar") != std::string::npos || !it->is_regular_file(ec)) continue;
    // a pasta dos cartuchos do Jaguar pode ter outro nome
    const std::string pai = std::filesystem::weakly_canonical(p.parent_path(), ec).string() + "/";
    if (pai.rfind(propria + "/", 0) == 0) continue;
    std::string e = p.extension().string();
    for (char& ch : e) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (e != ".rom" && e != ".bin" && e != ".jag" && e != ".abs" && e != ".j64") continue;
    if (it->file_size(ec) > (16u << 20)) continue;
    seen++;
    std::string n = p.filename().string();
    for (char& ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (jaguar::isJaguarRom(readAll(p.string()), n)) { stolen++; if (first.empty()) first = p.string(); }
  }
  std::printf("  %d arquivos .rom/.bin dos outros consoles conferidos\n", seen);
  check(stolen == 0, "ROMs dos outros consoles não viram Jaguar", first);
}

int main(int argc, char** argv) {
  if (argc >= 5 && std::strcmp(argv[1], "--png") == 0) {
    const auto rom = readAll(argv[2]);
    auto core = createCore(rom, argv[2], nullptr);
    if (!core) { std::printf("não reconhecido\n"); return 1; }
    const int n = std::atoi(argv[3]);
    for (int f = 0; f < n; f++) core->runFrame();
    writePng(argv[4], core->framebuffer(), core->width(), core->height());
    return 0;
  }
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/jaguar";
  const auto rom = readAll(dir + "/teste.j64");
  if (rom.empty()) { std::printf("falta %s/teste.j64 (rode tools/t7_jaguar.sh)\n", dir.c_str()); return 1; }
  testCart(rom);
  const auto rom2 = readAll(dir + "/teste2.j64");
  check(!rom2.empty(), "cartucho teste2.j64 presente");
  if (!rom2.empty()) testCart2(rom2);
  testLoaders(rom);
  testRobustez(rom);
  testOthers(std::filesystem::path(dir).parent_path().string(), dir);
  std::printf("RESULTADO jaguar: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
