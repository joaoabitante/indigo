// Testes do núcleo do Capcom CPS1/CPS2 (indigo::cps). Uso: indigo-t7_cps <pasta de testes>
//
// Sem ROMs comerciais e sem ROMs de teste públicas prontas, os testes montam pacotes .zip sintéticos com os
// nomes e tamanhos de arquivo de jogos da tabela (sf2 e ssf2) e programas próprios do 68000 e do Z80:
//  1. decifração do CPS2 contra a implementação de referência do MAME (vetores gerados pelo t7_cps.sh) e ida e
//     volta cifra/decifra;
//  2. CPS1 (pacote "sf2"): CPS-A/CPS-B, paleta, camada 8x8 com o PAL da placa STF29, objeto, interrupção de
//     vblank, Z80 + YM2151 tocando um Lá de 440 Hz (verificado pela potência na frequência), OKI;
//  3. CPS2 (pacote "ssf2"): programa cifrado com uma chave .key própria, dados lidos em claro, operando relativo
//     ao PC lido do espaço decifrado, objeto do CPS2 com gráficos embaralhados, EEPROM;
//  4. save state: salvar -> rodar -> carregar -> rodar dá a mesma tela e o mesmo som; salvar -> carregar ->
//     salvar dá bytes idênticos; estados truncados/danificados não derrubam o núcleo;
//  5. detecção: isCpsSet, pacote incompleto, lixo.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "core/zip.h"
#include "cps/cps.h"
#include "cps/cps2crypt.h"
#include "cps/games.h"

using namespace indigo;
using namespace indigo::cps;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
  else std::printf("ok: %s\n", what.c_str());
}

static std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------ .zip sem compressão
static uint32_t crc32(const std::vector<uint8_t>& d) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : d) {
    c ^= b;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
static void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(static_cast<uint8_t>(v)); o.push_back(static_cast<uint8_t>(v >> 8)); }
static void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }
static std::vector<uint8_t> makeZip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> z, cd;
  for (const auto& f : files) {
    const uint32_t off = static_cast<uint32_t>(z.size()), crc = crc32(f.second), sz = static_cast<uint32_t>(f.second.size());
    put32(z, 0x04034b50); put16(z, 20); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0);
    put32(z, crc); put32(z, sz); put32(z, sz); put16(z, static_cast<uint32_t>(f.first.size())); put16(z, 0);
    z.insert(z.end(), f.first.begin(), f.first.end());
    z.insert(z.end(), f.second.begin(), f.second.end());
    put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0);
    put32(cd, crc); put32(cd, sz); put32(cd, sz); put16(cd, static_cast<uint32_t>(f.first.size())); put16(cd, 0); put16(cd, 0);
    put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
    cd.insert(cd.end(), f.first.begin(), f.first.end());
  }
  const uint32_t cdOff = static_cast<uint32_t>(z.size());
  z.insert(z.end(), cd.begin(), cd.end());
  put32(z, 0x06054b50); put16(z, 0); put16(z, 0); put16(z, static_cast<uint32_t>(files.size()));
  put16(z, static_cast<uint32_t>(files.size())); put32(z, static_cast<uint32_t>(cd.size())); put32(z, cdOff); put16(z, 0);
  return z;
}

// monta os arquivos do jogo a partir das regiões desejadas (o inverso das cargas da tabela)
static std::vector<uint8_t> buildSet(const GameDef& g, std::vector<uint8_t> (&rg)[RG_COUNT]) {
  std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
  std::vector<int> idx;
  for (int i = 0; i < g.numLoads; i++) {
    const RomLoad& l = loadAt(g, i);
    if (l.mode >= 7) continue;
    size_t k = 0;
    while (k < idx.size() && idx[k] != l.file) k++;
    if (k == idx.size()) {
      idx.push_back(l.file);
      files.push_back({fileAt(l.file).name, std::vector<uint8_t>(fileAt(l.file).size, 0xFF)});
    }
    std::vector<uint8_t>& f = files[k].second;
    const std::vector<uint8_t>& r = rg[l.region];
    auto get = [&](size_t a) -> uint8_t { return a < r.size() ? r[a] : 0xFF; };
    auto put = [&](size_t a, uint8_t v) { if (a < f.size()) f[a] = v; };
    const size_t s = l.src, d = l.dst;
    for (uint32_t q = 0; q < l.len; q++) {
      switch (l.mode) {
        case 0: case 6: put(s + q, get(d + q)); break;
        case 1: put(s + q, get(d + 2 * q)); break;
        case 2: put(s + q, get(d + (q ^ 1))); break;
        case 3: put(s + q, get(d + (q & ~1u) * 4 + (q & 1))); break;
        case 4: put(s + q, get(d + 8 * q)); break;
        case 5: put(s + q, get(d + 4 * q)); break;
        default: break;
      }
    }
  }
  return makeZip(files);
}

static void w16(std::vector<uint8_t>& m, uint32_t a, uint16_t v) { m[a] = static_cast<uint8_t>(v >> 8); m[a + 1] = static_cast<uint8_t>(v); }
static void w32(std::vector<uint8_t>& m, uint32_t a, uint32_t v) { w16(m, a, static_cast<uint16_t>(v >> 16)); w16(m, a + 2, static_cast<uint16_t>(v)); }
static void prog(std::vector<uint8_t>& m, uint32_t a, std::initializer_list<uint16_t> ws) { for (uint16_t w : ws) { w16(m, a, w); a += 2; } }

static uint64_t hashFrame(const Cps& c) {
  uint64_t h = 1469598103934665603ull;
  const uint32_t* p = c.framebuffer();
  for (int i = 0; i < c.width() * c.height(); i++) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
static uint32_t pixel(const Cps& c, int x, int y) { return c.framebuffer()[y * c.width() + x]; }

// potência de uma frequência (Goertzel)
static double goertzel(const std::vector<float>& s, double f, double rate) {
  const double w = 2 * 3.14159265358979 * f / rate, k = 2 * std::cos(w);
  double a = 0, b = 0;
  for (float x : s) { const double t = x + k * a - b; b = a; a = t; }
  return a * a + b * b - k * a * b;
}

// ------------------------------------------------------------------ 1. decifração do CPS2
static void testCrypt(const std::string& dir) {
  int vec = 0;
  for (int i = 0; i < 16; i++) {
    const std::string base = dir + "/crypt/";
    const auto key = readFile(base + "key" + std::to_string(i) + ".bin");
    if (key.size() != 20) break;
    const auto enc = readFile(base + "enc" + std::to_string(i) + ".bin");
    const auto ref = readFile(base + "dec" + std::to_string(i) + ".bin");
    std::vector<uint8_t> dec;
    const Cps2Key k = cps2ParseKey(key);
    cps2Decrypt(enc, dec, k);
    size_t diff = 0;
    for (size_t j = 0; j < dec.size() && j < ref.size(); j++) diff += dec[j] != ref[j];
    check(k.valid && dec.size() == ref.size() && diff == 0,
          "CPS2: decifração igual à do MAME, chave " + std::to_string(i) + " (" + std::to_string(enc.size() / 2) + " palavras, faixa até 0x" +
              [&] { char b[16]; std::snprintf(b, sizeof b, "%X", k.upper); return std::string(b); }() + ")");
    vec++;
  }
  if (!vec) std::printf("pulei a comparação com o MAME (rode t7_cps.sh para gerar os vetores)\n");
  // ida e volta: cifra(decifra(x)) = x para endereços e palavras variadas
  Cps2Key k;
  k.key[0] = 0x12345678; k.key[1] = 0x9ABCDEF0; k.lower = 0; k.upper = 0x400000; k.valid = true;
  int bad = 0;
  uint32_t x = 1;
  for (int i = 0; i < 20000; i++) {
    x = x * 1103515245 + 12345;
    const uint16_t w = static_cast<uint16_t>(x >> 8);
    const uint32_t a = (x >> 3) & 0x1FFFFF;
    if (cps2DecryptWord(cps2EncryptWord(w, a, k), a, k) != w) bad++;
  }
  check(bad == 0, "CPS2: cifra e decifra são inversas (20000 palavras)");
}

// arquivo .key cuja leitura dá as 10 palavras pedidas (o inverso da leitura embaralhada)
static std::vector<uint8_t> makeKeyFile(const uint16_t dec[10]) {
  std::vector<uint8_t> f(20, 0);
  for (int b = 0; b < 160; b++) {
    if (!(dec[b / 16] & (0x8000 >> (b % 16)))) continue;
    const int bit = (317 - b) % 160;
    f[static_cast<size_t>(bit / 8)] |= static_cast<uint8_t>(1 << ((bit ^ 7) % 8));
  }
  return f;
}

// ------------------------------------------------------------------ 2. CPS1
static std::vector<uint8_t> cps1Set() {
  const GameDef* g = gameByName("sf2");
  std::vector<uint8_t> rg[RG_COUNT];
  for (int r = 0; r < RG_COUNT; r++) rg[r].assign(g->regionSize[r], 0xFF);
  std::vector<uint8_t>& m = rg[RG_MAIN];
  m.assign(0x100000, 0xFF);
  w32(m, 0, 0x00FFFF00); w32(m, 4, 0x400);
  for (uint32_t v = 2; v < 64; v++) w32(m, v * 4, 0x500);
  prog(m, 0x400, {
      0x46FC, 0x2700,                          // move.w #$2700,sr
      0x4FF9, 0x00FF, 0xFF00,                  // lea $FFFF00,a7
      0x33FC, 0x003F, 0x0080, 0x0170,          // CPS-B 0x30: copia as 6 páginas da paleta
      0x33FC, 0x1388, 0x0080, 0x0166,          // CPS-B 0x26: ordem 2,3,objetos,1; scroll1 ligada
      0x33FC, 0x9100, 0x0080, 0x0100,          // objetos em 0x910000
      0x33FC, 0x9000, 0x0080, 0x0102,          // scroll1 em 0x900000
      0x33FC, 0x000E, 0x0080, 0x0122,          // controle de vídeo
      0x33FC, 0xFFC0, 0x0080, 0x010C,          // scroll1 x = -64
      0x33FC, 0xFFF0, 0x0080, 0x010E,          // scroll1 y = -16
      0x33FC, 0xFF00, 0x0091, 0x4402,          // paleta scroll1, cor 0, tinta 1 = vermelho
      0x33FC, 0xF0F0, 0x0091, 0x4022,          // paleta objetos, cor 1, tinta 1 = verde
      0x33FC, 0x9140, 0x0080, 0x010A,          // base da paleta (copia)
      0x33FC, 0x4000, 0x0090, 0x0000,          // tile (0,0) da scroll1 = 0x4000
      0x33FC, 0x0000, 0x0090, 0x0002,
      0x33FC, 0x00A4, 0x0091, 0x0000,          // objeto: x = 164, y = 66 (tela 100,50)
      0x33FC, 0x0042, 0x0091, 0x0002,
      0x33FC, 0x0010, 0x0091, 0x0004,          // código 0x10
      0x33FC, 0x0001, 0x0091, 0x0006,          // cor 1
      0x33FC, 0xFF00, 0x0091, 0x000E,          // fim da lista
      0x46FC, 0x2000,                          // liga as interrupções
      0x3039, 0x00FF, 0x0000,                  // laço: move.w $FF0000,d0
      0x33C0, 0x00FF, 0x0002,                  //        move.w d0,$FF0002
      0x60F2});                                //        bra laço
  prog(m, 0x500, {0x5279, 0x00FF, 0x0000, 0x4E73});  // IRQ2: addq.w #1,$FF0000; rte
  // gráficos: tudo transparente (0xFF = tinta 15), menos o tile 8x8 da scroll1 (código 0x4000 -> 0x500000
  // pelo PAL STF29) e o objeto de código 0x10 (0x800), com tinta 1
  std::vector<uint8_t>& gx = rg[RG_GFX];
  for (int r = 0; r < 8; r++) { gx[0x500000 + r * 8] = 0xFF; for (int k = 1; k < 4; k++) gx[0x500000 + r * 8 + k] = 0; }
  for (int r = 0; r < 16; r++)
    for (int h = 0; h < 2; h++) { gx[0x800 + r * 8 + h * 4] = 0xFF; for (int k = 1; k < 4; k++) gx[0x800 + r * 8 + h * 4 + k] = 0; }
  // Z80: Lá de 440 Hz no canal 1 do YM2151 (4 portadoras, só C2 audível) e uma frase no OKI
  std::vector<uint8_t>& a = rg[RG_AUDIO];
  std::fill(a.begin(), a.end(), 0);
  std::vector<uint8_t> z = {0xF3, 0x31, 0x00, 0xD8};
  const uint8_t regs[][2] = {{0x20, 0xC7}, {0x28, 0x4A}, {0x30, 0x00}, {0x40, 1},    {0x48, 1},    {0x50, 1},    {0x58, 1},
                             {0x60, 0x7F}, {0x68, 0x7F}, {0x70, 0x7F}, {0x78, 0x08}, {0x80, 0x1F}, {0x88, 0x1F}, {0x90, 0x1F},
                             {0x98, 0x1F}, {0xA0, 0},    {0xA8, 0},    {0xB0, 0},    {0xB8, 0},    {0xE0, 0x0F}, {0xE8, 0x0F},
                             {0xF0, 0x0F}, {0xF8, 0x0F}, {0x08, 0x78}};
  for (const auto& rv : regs) {
    for (uint8_t b : {uint8_t(0x3E), rv[0], uint8_t(0x32), uint8_t(0x00), uint8_t(0xF0), uint8_t(0x3E), rv[1], uint8_t(0x32), uint8_t(0x01), uint8_t(0xF0)}) z.push_back(b);
  }
  for (uint8_t b : {0x3E, 0x81, 0x32, 0x02, 0xF0, 0x3E, 0x18, 0x32, 0x02, 0xF0, 0x18, 0xFE}) z.push_back(static_cast<uint8_t>(b));
  std::copy(z.begin(), z.end(), a.begin());
  std::vector<uint8_t>& o = rg[RG_OKI];
  std::fill(o.begin(), o.end(), 0);
  const uint8_t phr[6] = {0x00, 0x04, 0x00, 0x00, 0x7F, 0xFF};
  std::memcpy(&o[8], phr, 6);
  uint32_t x = 7;
  for (size_t i = 0x400; i < 0x8000; i++) { x = x * 1103515245 + 12345; o[i] = static_cast<uint8_t>(x >> 16); }
  return buildSet(*g, rg);
}

static void stateChecks(Cps& c, const std::string& nome) {
  for (int i = 0; i < 5; i++) c.runFrame();
  const std::vector<uint8_t> s1 = c.saveState();
  std::vector<float> l1(4000), r1(4000), l2(4000), r2(4000);
  auto drain = [&] { std::vector<float> a(c.audioAvailable() + 1), b(a.size()); c.pullAudio(a.data(), b.data(), a.size() - 1); };
  drain();
  for (int i = 0; i < 20; i++) c.runFrame();
  const uint64_t h1 = hashFrame(c);
  c.pullAudio(l1.data(), r1.data(), 4000);
  c.loadState(s1);
  const std::vector<uint8_t> s2 = c.saveState();
  if (s1 != s2) {
    size_t d = 0;
    while (d < s1.size() && d < s2.size() && s1[d] == s2[d]) d++;
    std::printf("  (primeira diferença no byte %zu de %zu/%zu)\n", d, s1.size(), s2.size());
  }
  check(s1 == s2, nome + ": salvar -> carregar -> salvar dá bytes idênticos (" + std::to_string(s1.size()) + " bytes)");
  drain();
  for (int i = 0; i < 20; i++) c.runFrame();
  const uint64_t h2 = hashFrame(c);
  c.pullAudio(l2.data(), r2.data(), 4000);
  check(h1 == h2 && l1 == l2 && r1 == r2, nome + ": estado carregado reproduz a mesma tela e o mesmo som");
  // estados danificados e truncados
  bool okAll = true;
  for (size_t cut : {size_t(0), size_t(7), s1.size() / 3, s1.size() - 1}) {
    try { c.loadState(std::vector<uint8_t>(s1.begin(), s1.begin() + static_cast<long>(cut))); okAll = false; } catch (const std::runtime_error&) {}
  }
  uint32_t x = 99;
  for (int t = 0; t < 30; t++) {
    std::vector<uint8_t> bad = s1;
    for (int k = 0; k < 40; k++) { x = x * 1103515245 + 12345; bad[(x >> 8) % bad.size()] ^= static_cast<uint8_t>(x >> 24); }
    try { c.loadState(bad); c.runFrame(); c.runFrame(); } catch (const std::runtime_error&) {}
  }
  c.loadState(s1);
  c.runFrame();
  check(okAll, nome + ": estados truncados são recusados e estados danificados rodam sem travar");
}

static void testCps1() {
  const std::vector<uint8_t> zip = cps1Set();
  check(isCpsSet(zip, "sf2.zip"), "CPS1: pacote sintético sf2.zip reconhecido");
  CpsCore core(zip, "sf2.zip", nullptr);
  Cps& c = core.cps();
  check(core.system() == "cps1" && core.width() == 384 && core.height() == 224, "CPS1: sistema cps1, tela 384x224");
  check(std::fabs(core.fps() - 59.637) < 0.001, "CPS1: 59,637 quadros/s");
  core.setSampleRate(48000);
  for (int i = 0; i < 10; i++) core.runFrame();
  // o contador da IRQ2 sobe uma vez por quadro
  const uint16_t cnt = static_cast<uint16_t>((c.read8(0xFF0000) << 8) | c.read8(0xFF0001));
  check(cnt >= 9 && cnt <= 10, "CPS1: interrupção de vblank uma vez por quadro (contador " + std::to_string(cnt) + ")");
  check(pixel(c, 0, 0) == 0xFF0000FFu && pixel(c, 7, 7) == 0xFF0000FFu && pixel(c, 8, 0) != 0xFF0000FFu,
        "CPS1: tile 8x8 da scroll1 (código 0x4000 pelo PAL STF29) vermelho em (0..7, 0..7)");
  check(pixel(c, 100, 50) == 0xFF00FF00u && pixel(c, 115, 65) == 0xFF00FF00u && pixel(c, 116, 50) != 0xFF00FF00u &&
            pixel(c, 99, 50) != 0xFF00FF00u,
        "CPS1: objeto 16x16 verde em (100..115, 50..65)");
  check(pixel(c, 200, 200) == 0xFF000000u, "CPS1: fundo na tinta 0xBFF (preta)");
  // som: 1 s, Lá de 440 Hz dominante
  std::vector<float> l, r(1), tmp(8192), tr(8192);
  core.pullAudio(tmp.data(), tr.data(), 8192);
  for (int i = 0; i < 60; i++) {
    core.runFrame();
    const size_t n = core.audioAvailable();
    tmp.resize(n); tr.resize(n);
    core.pullAudio(tmp.data(), tr.data(), n);
    l.insert(l.end(), tmp.begin(), tmp.end());
  }
  const double p440 = goertzel(l, 440, 48000), p400 = goertzel(l, 400, 48000), p480 = goertzel(l, 480, 48000),
               p880 = goertzel(l, 880, 48000);
  check(l.size() > 47000 && l.size() < 49500, "CPS1: ~48000 amostras por segundo (" + std::to_string(l.size()) + ")");
  check(p440 > 20 * p400 && p440 > 20 * p480 && p440 > 5 * p880, "CPS1: YM2151 toca o Lá (KC 0x4A) em 440 Hz");
  double rms = 0;
  for (float v : l) rms += v * v;
  rms = std::sqrt(rms / static_cast<double>(l.size()));
  check(rms > 0.01 && rms < 0.9, "CPS1: nível de som razoável (RMS " + std::to_string(rms) + ")");
  stateChecks(c, "CPS1");
}

// ------------------------------------------------------------------ 3. CPS2
static void testCps2() {
  const GameDef* g = gameByName("ssf2");
  std::vector<uint8_t> rg[RG_COUNT];
  for (int r = 0; r < RG_COUNT; r++) rg[r].assign(g->regionSize[r], 0xFF);
  std::vector<uint8_t>& m = rg[RG_MAIN];
  std::fill(m.begin(), m.end(), 0xFF);
  w32(m, 0, 0x00FFFF00); w32(m, 4, 0x400);
  for (uint32_t v = 2; v < 64; v++) w32(m, v * 4, 0x500);
  prog(m, 0x400, {
      0x46FC, 0x2700,                     // move.w #$2700,sr
      0x4FF9, 0x00FF, 0xFF00,             // lea $FFFF00,a7
      0x41F9, 0x0000, 0x2000,             // lea $2000,a0 (dados em claro dentro da faixa cifrada)
      0x43F9, 0x00FF, 0x0010,             // lea $FF0010,a1
      0x303C, 0x0007,                     // move.w #7,d0
      0x32D8,                             // move.w (a0)+,(a1)+
      0x51C8, 0xFFFC,                     // dbra d0
      0x323A, 0x007E,                     // move.w ($4A0,pc),d1  (lido do espaço de programa)
      0x33C1, 0x00FF, 0x0000,             // move.w d1,$FF0000
      0x33FC, 0x003F, 0x0080, 0x4170,     // CPS-B 0x30 (paleta)
      0x33FC, 0x0040, 0x0040, 0x0008,     // objetos: x offset 0x40
      0x33FC, 0x0010, 0x0040, 0x000A,     //          y offset 0x10
      0x33FC, 0xF00F, 0x0091, 0x4022,     // paleta dos objetos, cor 1, tinta 1 = azul
      0x33FC, 0x9140, 0x0080, 0x410A,     // base da paleta
      0x33FC, 0x20A4, 0x0070, 0x8000,     // objeto (banco 1 pela janela 0x708000): x = 164, prioridade 1
      0x33FC, 0x0042, 0x0070, 0x8002,     //   y = 66
      0x33FC, 0x0010, 0x0070, 0x8004,     //   código 0x10
      0x33FC, 0x0001, 0x0070, 0x8006,     //   cor 1
      0x33FC, 0xFF00, 0x0070, 0x800E,     //   fim da lista
      0x33FC, 0x0001, 0x0080, 0x40E0,     // troca os bancos: o banco escrito vira o exibido
      0x60FE});                           // bra *
  const uint32_t codeEnd = 0x4A2;
  w16(m, 0x4A0, 0xABCD);
  for (int i = 0; i < 8; i++) w16(m, 0x2000 + 2 * i, static_cast<uint16_t>(0x1111 * (i + 1)));
  // chave: faixa cifrada de 0 a 0x8000
  const uint16_t dec[10] = {0x1234, 0x5678, 0x9ABC, 0xDEF0, 0, 0, 0, 0x4000, 0x0900, 0x3FE};
  rg[RG_KEY] = makeKeyFile(dec);
  const Cps2Key key = cps2ParseKey(rg[RG_KEY]);
  check(key.valid && key.key[0] == 0x12345678 && key.upper == 0x8000, "CPS2: leitura do arquivo .key (chave e faixa)");
  // cifra só o que é instrução (vetores do reset e o código); os dados ficam em claro
  for (uint32_t a = 0; a < codeEnd; a += 2) {
    if (a >= 8 && a < 0x400) continue;
    const uint16_t p = static_cast<uint16_t>((m[a] << 8) | m[a + 1]);
    w16(m, a, cps2EncryptWord(p, a / 2, key));
  }
  // gráfico do objeto: código 0x10 = 0x800 depois de desembaralhar; aplica o embaralhamento inverso
  std::vector<uint8_t> plain(rg[RG_GFX].size(), 0xFF);
  for (int r = 0; r < 16; r++)
    for (int h = 0; h < 2; h++) { plain[0x800 + r * 8 + h * 4] = 0xFF; for (int k = 1; k < 4; k++) plain[0x800 + r * 8 + h * 4 + k] = 0; }
  {
    const size_t bank = 0x200000 / 8;
    std::vector<uint32_t> perm(bank);
    for (size_t i = 0; i < bank; i++) perm[i] = static_cast<uint32_t>(i);
    std::function<void(uint32_t*, size_t)> un = [&](uint32_t* b, size_t len) {
      if (len == 2) return;
      len /= 2;
      un(b, len); un(b + len, len);
      for (size_t i = 0; i < len / 2; i++) std::swap(b[len / 2 + i], b[len + i]);
    };
    un(perm.data(), bank);  // depois do desembaralhamento, a posição i tem o que estava em perm[i]
    std::vector<uint8_t>& gx = rg[RG_GFX];
    for (size_t off = 0; off + 0x200000 <= gx.size(); off += 0x200000)
      for (size_t i = 0; i < bank; i++) std::memcpy(&gx[off + perm[i] * 8], &plain[off + i * 8], 8);
  }
  std::fill(rg[RG_AUDIO].begin(), rg[RG_AUDIO].end(), 0);
  rg[RG_AUDIO][0] = 0xF3; rg[RG_AUDIO][1] = 0x18; rg[RG_AUDIO][2] = 0xFE;  // di; jr $
  const std::vector<uint8_t> zip = buildSet(*g, rg);
  check(isCpsSet(zip, "ssf2.zip"), "CPS2: pacote sintético ssf2.zip reconhecido");
  CpsCore core(zip, "ssf2.zip", nullptr);
  Cps& c = core.cps();
  check(core.system() == "cps2" && core.hasSave(), "CPS2: sistema cps2, com save (EEPROM)");
  core.setSampleRate(44100);
  for (int i = 0; i < 4; i++) core.runFrame();
  auto rd = [&](uint32_t a) { return static_cast<uint16_t>((c.read8(a) << 8) | c.read8(a + 1)); };
  bool tbl = true;
  for (int i = 0; i < 8; i++) tbl = tbl && rd(0xFF0010 + 2 * i) == 0x1111 * (i + 1);
  check(tbl, "CPS2: programa cifrado roda; dados lidos em claro dentro da faixa cifrada");
  check(rd(0xFF0000) == 0xABCD, "CPS2: operando relativo ao PC vem do espaço decifrado (como no MAME)");
  check(pixel(c, 100, 50) == 0xFFFF0000u && pixel(c, 115, 65) == 0xFFFF0000u && pixel(c, 116, 66) != 0xFFFF0000u,
        "CPS2: objeto (gráficos desembaralhados, banco de objetos trocado) azul em (100..115, 50..65)");
  // EEPROM: grava a palavra 5 pelo protocolo serial e lê de volta
  auto ee = [&](int cs, int clk, int di) { c.write16(0x804040, static_cast<uint16_t>((cs << 14) | (clk << 13) | (di << 12) | 0x0008)); };
  auto sendBits = [&](uint32_t v, int n) {
    for (int i = n - 1; i >= 0; i--) { const int b = (v >> i) & 1; ee(1, 0, b); ee(1, 1, b); }
  };
  ee(0, 0, 0); sendBits(0x13, 5); sendBits(0, 4); ee(0, 0, 0);            // 1 00 11xxxx = EWEN
  sendBits(0x5, 3); sendBits(5, 6); sendBits(0xBEEF, 16); ee(0, 0, 0);    // 1 01 aaaaaa + dado = WRITE
  sendBits(0x6, 3); sendBits(5, 6);                                       // 1 10 aaaaaa = READ
  uint16_t got = 0;
  const bool dummy0 = !(rd(0x804020) & 1);
  for (int i = 0; i < 16; i++) { ee(1, 0, 0); ee(1, 1, 0); got = static_cast<uint16_t>((got << 1) | (rd(0x804020) & 1)); }
  ee(0, 0, 0);
  check(dummy0 && got == 0xBEEF && core.saveDirty(), "CPS2: EEPROM 93C46 grava e lê pela porta serial");
  const auto sv = core.exportSave();
  check(sv.size() == 128 && sv[10] == 0xBE && sv[11] == 0xEF, "CPS2: save da EEPROM exportado (128 bytes)");
  stateChecks(c, "CPS2");
}

// ------------------------------------------------------------------ 5. detecção e lixo
static void testDetect() {
  std::vector<uint8_t> junk(5000);
  uint32_t x = 3;
  for (auto& b : junk) { x = x * 1103515245 + 12345; b = static_cast<uint8_t>(x >> 16); }
  check(!isCpsSet(junk, "sf2.zip"), "detecção: lixo não é pacote de CPS");
  const std::vector<uint8_t> other = makeZip({{"teste.bin", std::vector<uint8_t>(1000, 1)}});
  check(!isCpsSet(other, "abc.zip"), "detecção: .zip desconhecido não é pacote de CPS");
  // pacote com o nome certo mas incompleto: mensagem com os arquivos que faltam
  const std::vector<uint8_t> part = makeZip({{"sf2e_30g.11e", std::vector<uint8_t>(0x20000, 0)}});
  std::string msg;
  try { CpsCore c(part, "sf2.zip", nullptr); } catch (const std::runtime_error& e) { msg = e.what(); }
  check(msg.find("Faltam") != std::string::npos && msg.find("sf2-5m.4a") != std::string::npos,
        "detecção: pacote incompleto explica o que falta");
  check(gameCount() > 300 && gameByName("sf2ce") && gameByName("ffight") && gameByName("ddtod") && gameByName("mshvsf") &&
            gameByName("mvsc") && gameByName("sfa3") && gameByName("xmvsf") && gameByName("forgottn"),
        "tabela: " + std::to_string(gameCount()) + " jogos, incluindo sf2ce, ffight, ssf2, ddtod, mshvsf, mvsc, sfa3, xmvsf");
  // conteúdo aleatório nos arquivos de um jogo de verdade: roda sem travar
  for (const char* name : {"ffight", "sfa3"}) {
    const GameDef* g = gameByName(name);
    std::vector<uint8_t> rg[RG_COUNT];
    for (int r = 0; r < RG_COUNT; r++) {
      rg[r].resize(g->regionSize[r]);
      for (auto& b : rg[r]) { x = x * 1103515245 + 12345; b = static_cast<uint8_t>(x >> 16); }
    }
    bool ok = true;
    try {
      CpsCore c(buildSet(*g, rg), std::string(name) + ".zip", nullptr);
      const auto t0 = std::chrono::steady_clock::now();
      for (int i = 0; i < 300; i++) {
        c.setKeys(static_cast<uint32_t>(i * 37));
        c.runFrame();
      }
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("  (%s: %.2f ms por quadro)\n", name, ms / 300);
    } catch (const std::exception& e) {
      ok = false;
      std::printf("  (%s)\n", e.what());
    }
    check(ok, std::string("robustez: ") + name + " com ROMs aleatórias roda 300 quadros sem travar");
  }
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/cps";
  struct stat st;
  if (stat(dir.c_str(), &st) != 0) {
    std::printf("pasta %s não existe: pulei os testes do CPS\n", dir.c_str());
    return 0;
  }
  try {
    testCrypt(dir);
    testCps1();
    testCps2();
    testDetect();
  } catch (const std::exception& e) {
    fails++;
    std::printf("FALHA: exceção %s\n", e.what());
  }
  std::printf("RESULTADO CPS: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
