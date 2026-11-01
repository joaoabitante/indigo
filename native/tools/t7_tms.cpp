// Testes do vídeo TMS9918A e dos núcleos ColecoVision / SG-1000:
//   indigo-t7_tms <pasta de testes> [pasta para gravar as telas .ppm]
// 1) verificações do vídeo direto no chip (modos, sprites, 5º sprite, colisão, máscaras do Graphics II,
//    janelas de acesso à VRAM), sem arquivos;
// 2) programas do CVBasic (montados pelo t7_tms.sh) nos dois consoles: tela depois de N quadros comparada
//    por hash com a referência (conferida a olho), nenhum acesso à VRAM rápido demais (os programas são
//    feitos para o aparelho real), MegaCart, mapeador Sega e Super Game Module (som do AY);
// 3) save state determinístico; ROMs e estados de lixo não travam.
// O ColecoVision usa aqui uma BIOS mínima de teste (só desvia para o cartucho e traz a fonte do CVBasic):
// a BIOS original tem direitos autorais e não pode ser baixada.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../src/core/core.h"
#include "../src/tms/coleco.h"
#include "../src/tms/sg1000.h"
#include "../src/tms/sound.h"
#include "../src/tms/tms9918.h"

using namespace indigo;
using namespace indigo::tms;

namespace {
int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("  FALHOU: %s\n", what.c_str()); }
}

std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
}
bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

uint32_t fnv(const void* d, size_t n, uint32_t h = 0x811C9DC5u) {
  const auto* b = static_cast<const uint8_t*>(d);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x01000193u; }
  return h;
}

void writePpm(const std::string& path, const uint32_t* fb, int w, int h) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    const uint32_t c = fb[i];
    const uint8_t rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
}

// ------------------------------------------------------------------ 1) vídeo direto no chip
struct Vdp {
  TMS9918 v;
  int64_t t = 0;  // relógio: cada acesso 100 ciclos depois do anterior (sempre dentro das janelas)
  void reg(int r, int val) { v.writeControl(val, t += 100); v.writeControl(0x80 | r, t += 100); }
  void at(int a, bool write) { v.writeControl(a & 0xFF, t += 100); v.writeControl(((a >> 8) & 0x3F) | (write ? 0x40 : 0), t += 100); }
  void poke(int a, std::initializer_list<int> bytes) { at(a, true); for (int b : bytes) v.writeData(b, t += 100); }
  void fill(int a, int n, int val) { at(a, true); for (int i = 0; i < n; i++) v.writeData(val, t += 100); }
  void line(int y) { t += 228; v.startLine(y, t); }
  uint32_t px(int x, int y) const { return v.fb[y * 256 + x]; }
};
uint32_t C(int i) { return TMS_PALETTE[i]; }

void vdpTests() {
  std::printf("vídeo TMS9918A (verificações diretas no chip)\n");
  {  // Graphics I: padrão + cor por grupo de 8 caracteres; cor 0 = cor de fundo
    Vdp d;
    d.reg(0, 0x00); d.reg(1, 0xC0); d.reg(2, 0x06); d.reg(3, 0x80); d.reg(4, 0x00); d.reg(7, 0x05);
    d.fill(0x1800, 768, 0);
    d.poke(0x1800 + 1, {9});         // coluna 1, linha 0: caractere 9
    d.poke(0x0000 + 9 * 8 + 2, {0xF0});  // linha 2 do caractere 9
    d.poke(0x2000 + 1, {0xA0});      // cores dos caracteres 8–15: frente 10, fundo 0 (transparente)
    d.line(2);
    check(d.px(8, 2) == C(10) && d.px(11, 2) == C(10) && d.px(12, 2) == C(5) && d.px(0, 2) == C(5),
          "Graphics I: padrão, cor do grupo e cor 0 mostrando o fundo");
    d.reg(1, 0x80); d.line(2);
    check(d.px(8, 2) == C(5), "tela desligada mostra só a cor de fundo");
  }
  {  // Graphics II: terços e máscaras dos registradores 3 e 4
    Vdp d;
    d.reg(0, 0x02); d.reg(1, 0xC0); d.reg(2, 0x06); d.reg(3, 0xFF); d.reg(4, 0x03); d.reg(7, 0x01);
    d.fill(0x1800, 768, 0);
    d.poke(0x0000 + 3, {0x81});          // caractere 0, linha 3, terço 0
    d.poke(0x0800 + 3, {0x3C});          // caractere 0, linha 3, terço 1
    d.poke(0x2000 + 3, {0x6E});          // cor terço 0
    d.poke(0x2800 + 3, {0xF4});          // cor terço 1
    d.line(3); d.line(64 + 3);
    check(d.px(0, 3) == C(6) && d.px(1, 3) == C(14) && d.px(7, 3) == C(6), "Graphics II: terço de cima");
    check(d.px(0, 67) == C(4) && d.px(2, 67) == C(15), "Graphics II: terço do meio usa a sua tabela");
    d.reg(4, 0x00); d.reg(3, 0x9F);      // máscaras: todos os terços usam a tabela do primeiro
    d.line(64 + 3);
    check(d.px(0, 67) == C(6) && d.px(1, 67) == C(14), "Graphics II: máscaras dos registradores 3 e 4");
  }
  {  // Multicolor: blocos de 4×4 pontos
    Vdp d;
    d.reg(0, 0x00); d.reg(1, 0xC8); d.reg(2, 0x06); d.reg(4, 0x00); d.reg(7, 0x01);
    d.fill(0x1800, 768, 0);
    d.poke(0x1800 + 32 * 1 + 2, {5});    // linha de caracteres 1, coluna 2: nome 5
    // linha 13 da tela: (13>>2)&7 = 3 → byte 3 do padrão
    d.poke(5 * 8 + 3, {0x7C});
    d.line(13);
    check(d.px(16, 13) == C(7) && d.px(19, 13) == C(7) && d.px(20, 13) == C(12), "Multicolor");
  }
  {  // Text: 40 colunas de 6 pontos, borda de 8, cores do registrador 7
    Vdp d;
    d.reg(0, 0x00); d.reg(1, 0xD0); d.reg(2, 0x00); d.reg(4, 0x01); d.reg(7, 0xF4);
    d.fill(0x0000, 960, 0);
    d.poke(0x0000 + 1, {65});
    d.poke(0x0800 + 65 * 8, {0x84});
    // sprite que não pode aparecer no modo texto
    d.reg(5, 0x20); d.reg(6, 0x00); d.poke(0x1000, {0xFF, 0, 0, 0x0F}); d.poke(0x0000, {0});
    d.line(0);
    check(d.px(0, 0) == C(4) && d.px(8 + 6, 0) == C(15) && d.px(8 + 6 + 4, 0) == C(4) &&
              d.px(8 + 6 + 1, 0) == C(4) && d.px(250, 0) == C(4),
          "Text: célula de 6 pontos, borda e cores");
    // padrão 0x84: pontos 0 e 5 acesos → x=14 e x=19
    check(d.px(19, 0) == C(15), "Text: 6º ponto da célula vem do bit 2");
  }
  // sprites
  auto spr = [](Vdp& d, int i, int y, int x, int name, int attr) { d.poke(0x1B00 + i * 4, {y, x, name, attr}); };
  auto base = [](Vdp& d, int r1) {
    d.reg(0, 0x00); d.reg(1, r1); d.reg(2, 0x06); d.reg(3, 0x80); d.reg(4, 0x00); d.reg(5, 0x36); d.reg(6, 0x07);
    d.reg(7, 0x01);
    d.fill(0x1800, 768, 0); d.fill(0x0000, 8, 0); d.fill(0x2000, 32, 0);
    d.fill(0x3800, 64, 0xFF);  // padrões 0–7 cheios
    d.fill(0x1B00, 128, 0xD0);
  };
  {
    Vdp d; base(d, 0xC0);
    spr(d, 0, 9, 20, 0, 0x08);
    d.line(9); d.line(10); d.line(17); d.line(18);
    check(d.px(20, 9) == C(1) && d.px(20, 10) == C(8) && d.px(27, 17) == C(8) && d.px(28, 17) == C(1) && d.px(20, 18) == C(1),
          "sprite 8×8 aparece a partir da linha Y+1");
    check((d.v.readStatus() & 0x1F) == 1, "sem 5º sprite: status guarda o número do último examinado (o de Y=208)");
    spr(d, 0, 9, 40, 0, 0x88); d.line(10);
    check(d.px(8, 10) == C(8) && d.px(15, 10) == C(8) && d.px(16, 10) == C(1), "early clock desloca 32 pontos à esquerda");
  }
  {  // 16×16 ampliado
    Vdp d; base(d, 0xC3);
    d.poke(0x3800 + 8 * 8, {0x80});  // padrão 8 (quadrante de cima à esquerda), linha 0: só o 1º ponto
    for (int i = 1; i < 32; i++) d.poke(0x3800 + 8 * 8 + i, {0x00});
    d.poke(0x3800 + 8 * 8 + 16 + 15, {0x01});  // quadrante de baixo à direita, linha 15: último ponto
    spr(d, 0, 0xFF, 100, 9, 0x0F);              // nome 9 vira 8 no modo 16×16; Y=255 começa na linha 0
    d.line(0); d.line(1); d.line(30); d.line(31); d.line(32);
    check(d.px(100, 0) == C(15) && d.px(101, 0) == C(15) && d.px(102, 0) == C(1) && d.px(100, 1) == C(15),
          "16×16 ampliado: ponto vira 2×2 e o nome perde os 2 bits baixos");
    check(d.px(130, 30) == C(15) && d.px(131, 31) == C(15) && d.px(129, 31) == C(1) && d.px(131, 32) == C(1),
          "16×16 ampliado: quadrante de baixo à direita");
  }
  {  // 5º sprite, prioridade, colisão
    Vdp d; base(d, 0xC0);
    for (int i = 0; i < 6; i++) spr(d, i, 49, 10 + i * 20, 0, 2 + i);
    d.line(50);
    int st = d.v.readStatus();
    check((st & 0xDF) == (0x40 | 4), "5º sprite: sinal e número (4)");
    check(d.px(10 + 60, 50) == C(5) && d.px(10 + 80, 50) == C(1), "só 4 sprites por linha");
    d.line(51);
    st = d.v.readStatus();
    check((st & 0x5F) == (0x40 | 4), "5º sprite de novo na linha seguinte");
    // com o sinal de quadro (F) aceso o 5º sprite não é registrado
    d.line(192); d.fill(0x1B00, 128, 0xD0);
    for (int i = 0; i < 6; i++) spr(d, i, 49, 10 + i * 20, 0, 2 + i);
    d.line(50);
    st = d.v.readStatus();
    check((st & 0xC0) == 0x80, "5º sprite não é registrado com F aceso");
    // prioridade e colisão (inclusive cor 0)
    d.fill(0x1B00, 128, 0xD0);
    spr(d, 0, 99, 50, 0, 0x00);  // transparente por cima
    spr(d, 1, 99, 54, 0, 0x06);
    d.v.readStatus();
    d.line(100);
    check(d.px(50, 100) == C(1) && d.px(54, 100) == C(6) && d.px(57, 100) == C(6) && d.px(58, 100) == C(6),
          "sprite de cor 0 é transparente para os de trás");
    check((d.v.readStatus() & 0x20) != 0, "colisão conta sprite de cor 0");
    check((d.v.readStatus() & 0x20) == 0, "ler o status apaga a colisão");
    spr(d, 1, 99, 58, 0, 0x06);  // encostados, sem sobrepor
    d.line(100);
    check((d.v.readStatus() & 0x20) == 0, "sprites lado a lado não colidem");
    spr(d, 0, 99, 0, 0, 0x82); spr(d, 1, 99, 4, 0, 0x86);  // os dois fora da tela à esquerda, sobrepostos
    d.line(100);
    check((d.v.readStatus() & 0x20) == 0, "sobreposição fora da tela (à esquerda) não conta colisão");
    spr(d, 0, 99, 28, 0, 0x82); spr(d, 1, 99, 30, 0, 0x86);  // -4..3 e -2..5: sobrepõem em 0..3 na tela
    d.line(100);
    check((d.v.readStatus() & 0x20) != 0, "sobreposição parcialmente na tela conta colisão");
  }
  {  // sinal de quadro e interrupção
    Vdp d; base(d, 0xC0);
    for (int y = 0; y < 192; y++) d.line(y);
    check(!(d.v.readStatus() & 0x80), "F apagado durante a imagem");
    d.line(192);
    check(!d.v.irq(), "sem interrupção com IE desligado");
    d.reg(1, 0xE0);
    check(d.v.irq(), "ligar IE com F aceso pede interrupção");
    const int st = d.v.readStatus();
    check((st & 0x80) && !d.v.irq(), "ler o status apaga F e a interrupção");
  }
  {  // leitura antecipada e auto-incremento
    Vdp d;
    d.poke(0x1234, {0x11, 0x22, 0x33});
    d.at(0x1234, false);
    const int a = d.v.readData(d.t += 100), b = d.v.readData(d.t += 100), c = d.v.readData(d.t += 100);
    check(a == 0x11 && b == 0x22 && c == 0x33, "leitura antecipada da VRAM");
  }
  {  // janelas de acesso à VRAM
    bool all = true;
    for (int start = 0; start < 228; start++) {
      Vdp d; base(d, 0xC0);
      d.at(0x0100, true);
      d.t = 228 * 1000; d.v.startLine(50, d.t);
      const uint32_t before = d.v.tooFast;
      int64_t t = d.t + start;
      for (int i = 0; i < 6; i++) d.v.writeData(i + 1, t += 24);
      d.v.startLine(51, d.t + 228 * 2);
      bool ok = d.v.tooFast == before;
      for (int i = 0; i < 6; i++) ok = ok && d.v.vram[0x100 + i] == i + 1;
      all = all && ok;
    }
    check(all, "tela ligada: 1 acesso a cada 24 ciclos nunca se perde (qualquer posição da linha)");
    {
      Vdp d; base(d, 0xC0);
      d.fill(0x0100, 4, 0);
      d.at(0x0100, true);
      d.t = 228 * 1000; d.v.startLine(50, d.t);
      const uint32_t before = d.v.tooFast;
      d.v.writeData(0xAA, d.t + 40); d.v.writeData(0xBB, d.t + 45);
      d.v.startLine(51, d.t + 228);
      check(d.v.vram[0x100] == 0xBB && d.v.vram[0x101] == 0 && d.v.tooFast == before + 1,
            "tela ligada: 2ª gravação colada substitui a 1ª (um byte só)");
    }
    {
      Vdp d; base(d, 0x80);  // tela desligada: janelas a cada 2–4 pontos
      d.at(0x0100, true);
      d.t = 228 * 1000; d.v.startLine(50, d.t);
      const uint32_t before = d.v.tooFast;
      int64_t t = d.t;
      for (int i = 0; i < 40; i++) d.v.writeData(i + 1, t += 6);
      d.v.startLine(51, d.t + 228 * 2);
      check(d.v.tooFast == before && d.v.vram[0x100 + 39] == 40, "tela desligada: 1 gravação a cada 6 ciclos passa");
    }
    {
      Vdp d; base(d, 0xC0);  // retraço vertical: rápido também
      d.at(0x0100, true);
      d.t = 228 * 1000; d.v.startLine(200, d.t);
      const uint32_t before = d.v.tooFast;
      int64_t t = d.t;
      for (int i = 0; i < 30; i++) d.v.writeData(i + 1, t += 6);
      d.v.startLine(201, d.t + 228 * 2);
      check(d.v.tooFast == before && d.v.vram[0x100 + 29] == 30, "retraço vertical: 1 gravação a cada 6 ciclos passa");
    }
  }
}

// ------------------------------------------------------------------ som (SN76489 e AY-3-8910 direto no chip)
// conta as trocas de sinal de 1 s de som e a energia de um trecho
struct Measure { int crossings; double energy; };
Measure measure(Sound& s, int64_t& clock, double seconds) {
  std::vector<float> L(4096), R(4096);
  Measure m{0, 0};
  float prev = 0;
  const int64_t end = clock + static_cast<int64_t>(3579545 * seconds);
  while (clock < end) {
    clock = std::min(end, clock + 20000);
    s.run(clock);
    size_t n = s.available();
    while (n) {
      const size_t k = std::min(n, L.size());
      s.pull(L.data(), R.data(), k);
      for (size_t i = 0; i < k; i++) {
        if ((L[i] >= 0) != (prev >= 0)) m.crossings++;
        prev = L[i];
        m.energy += double(L[i]) * L[i];
      }
      n -= k;
    }
  }
  return m;
}

void soundTests() {
  std::printf("som (SN76489 e AY-3-8910)\n");
  {
    Sound s; s.setSampleRate(48000);
    int64_t clk = 0;
    s.snWrite(0x80 | 0x0E); s.snWrite(254 >> 4);  // canal 0: período 254 → 3579545/32/254 = 440,4 Hz
    s.snWrite(0x90);                                // volume máximo
    measure(s, clk, 0.1);
    const Measure m = measure(s, clk, 1.0);
    check(std::abs(m.crossings / 2 - 440) <= 3, "SN76489: tom de 440 Hz (" + std::to_string(m.crossings / 2) + ")");
    s.snWrite(0x9F);
    measure(s, clk, 0.1);
    check(measure(s, clk, 0.2).energy < 1e-3, "SN76489: volume 15 é mudo");
  }
  {
    Sound s; s.setSampleRate(48000);
    int64_t clk = 0;
    auto ay = [&](int r, int v) { s.ayAddress(r); s.ayWrite(v); };
    ay(0, 0x00); ay(1, 0x01);  // canal A: período 256 → 1789772,5/16/256 = 437 Hz
    ay(7, 0x3E);               // só o tom do canal A
    ay(8, 15);
    s.ayAddress(1);
    check(s.ayRead() == 0x01, "AY-3-8910: leitura do registrador");
    measure(s, clk, 0.1);
    const Measure m = measure(s, clk, 1.0);
    check(std::abs(m.crossings / 2 - 437) <= 3, "AY-3-8910: tom de 437 Hz (" + std::to_string(m.crossings / 2) + ")");
    // envelope "desce e fica em 0" (forma 0): período 1000 → 16 passos de 256×1000 ciclos do AY = 0,14 s
    ay(11, 1000 & 0xFF); ay(12, 1000 >> 8); ay(8, 0x10); ay(13, 0x00);
    const double e1 = measure(s, clk, 0.05).energy;
    measure(s, clk, 0.25);
    const double e2 = measure(s, clk, 0.05).energy;
    check(e1 > 1.0 && e2 < 1e-3, "AY-3-8910: envelope desce e fica mudo");
    // forma 0x0C (dente de serra subindo, repetindo): segue com som
    ay(13, 0x0C);
    measure(s, clk, 0.3);
    check(measure(s, clk, 0.1).energy > 1.0, "AY-3-8910: envelope repetido continua");
    // ruído no canal B
    ay(7, 0x2F); ay(9, 15); ay(8, 0); ay(6, 8);
    measure(s, clk, 0.05);
    const Measure n = measure(s, clk, 0.5);
    check(n.energy > 1.0 && n.crossings > 500, "AY-3-8910: ruído");
  }
}

// ------------------------------------------------------------------ 2) programas
std::vector<uint8_t> testBios(const std::vector<uint8_t>& font) {
  // BIOS mínima só para os testes (NÃO é a do aparelho): desvia para o cartucho, vetores RST/NMI do
  // cabeçalho do cartucho, byte de 60 Hz em 0069h e ponteiro da fonte em 006Ch (como a original)
  std::vector<uint8_t> b(0x2000, 0xC9);
  const uint8_t boot[] = {0xF3, 0x31, 0xB9, 0x73, 0x2A, 0x0A, 0x80, 0xE9};
  std::memcpy(b.data(), boot, sizeof boot);
  const int vec[][2] = {{0x08, 0x800C}, {0x10, 0x800F}, {0x18, 0x8012}, {0x20, 0x8015},
                        {0x28, 0x8018}, {0x30, 0x801B}, {0x38, 0x801E}, {0x66, 0x8021}};
  for (auto& v : vec) { b[v[0]] = 0xC3; b[v[0] + 1] = v[1] & 0xFF; b[v[0] + 2] = v[1] >> 8; }
  b[0x69] = 60;
  b[0x6C] = 0x80; b[0x6D] = 0x10;  // fonte do caractere 30h em 1080h (a do 20h fica em 1000h)
  for (size_t i = 0; i < font.size() && i < 0x300; i++) b[0x1000 + i] = font[i];
  return b;
}

struct Press { int from, to; uint32_t keys; };
struct RomTest {
  const char* sys;   // "coleco" ou "sg1000"
  const char* file;
  int frames;
  std::vector<Press> script;
  uint32_t hash;     // tela no último quadro (0 = só mostra o valor)
  bool sound;        // espera som audível
};

std::unique_ptr<Core> makeCore(const std::string& sys, const std::vector<uint8_t>& rom) {
  if (sys == "coleco") return std::make_unique<ColecoCore>(rom, nullptr);
  return std::make_unique<SG1000Core>(rom, nullptr);
}
uint32_t tooFastOf(Core& c) {
  if (auto* k = dynamic_cast<ColecoCore*>(&c)) return k->machine().vdp.tooFast;
  return static_cast<SG1000Core&>(c).machine().vdp.tooFast;
}

void runScript(Core& core, const RomTest& t, double* energy) {
  std::vector<float> L(4096), R(4096);
  double e = 0;
  for (int f = 0; f < t.frames; f++) {
    uint32_t k = 0;
    for (auto& p : t.script) if (f >= p.from && f < p.to) k |= p.keys;
    core.setKeys(k);
    core.runFrame();
    size_t n = core.audioAvailable();
    while (n) {
      const size_t m = std::min<size_t>(n, L.size());
      core.pullAudio(L.data(), R.data(), m);
      for (size_t i = 0; i < m; i++) e += double(L[i]) * L[i];
      n -= m;
    }
  }
  if (energy) *energy = e;
}

void romTests(const std::string& dir, const std::string& outDir) {
  const std::vector<uint8_t> font = readFile(dir + "/font.bin");
  setFirmware("coleco", testBios(font));
  // teclas: A = botão esquerdo no ColecoVision; B = botão 1 no SG-1000; START = tecla 1 / pausa
  const std::vector<RomTest> tests = {
      {"coleco", "demo.rom", 300, {}, 0xA02D79CC, false},
      {"coleco", "happy_face.rom", 200, {}, 0x20247B5C, false},
      {"coleco", "face_joystick.rom", 200, {{60, 120, KEY_RIGHT}, {120, 160, KEY_DOWN}}, 0xE3E1FE55, false},
      {"coleco", "controller.rom", 120, {{100, 120, KEY_START | KEY_A}}, 0xFC503A81, false},
      {"coleco", "space_attack.rom", 400, {{100, 110, KEY_A}, {200, 260, KEY_LEFT | KEY_A}}, 0x005F3234, true},
      {"coleco", "viboritas.rom", 400, {{100, 110, KEY_A}, {150, 300, KEY_UP}}, 0x201D7B9D, true},
      {"coleco", "oscar.rom", 400, {}, 0x7CC6EA0B, false},
      {"coleco", "plot.rom", 200, {}, 0x78A931DC, false},
      {"coleco", "vramcopy.rom", 200, {}, 0x0CB3E535, false},
      {"coleco", "test1.rom", 200, {}, 0xD569C945, false},
      {"coleco", "test2.rom", 200, {}, 0xEE92EF44, false},
      {"coleco", "music.rom", 300, {{100, 110, KEY_A}}, 0xC3A0DA45, true},
      {"coleco", "bank.rom", 300, {}, 0x7CC6EA0B, true},
      {"coleco", "sgm_music.rom", 300, {{100, 110, KEY_A}}, 0xC3A0DA45, true},
      {"coleco", "sgm_viboritas.rom", 300, {{100, 110, KEY_A}}, 0xDE543FDA, true},
      {"sg1000", "demo.rom", 300, {}, 0x4AA69482, false},
      {"sg1000", "happy_face.rom", 200, {}, 0x9C2898DC, false},
      {"sg1000", "face_joystick.rom", 200, {{60, 120, KEY_RIGHT}, {120, 160, KEY_DOWN}}, 0x4CF34845, false},
      {"sg1000", "controller.rom", 120, {{100, 120, KEY_B}}, 0x21168535, false},
      {"sg1000", "space_attack.rom", 400, {{100, 110, KEY_B}, {200, 260, KEY_LEFT | KEY_B}}, 0xAEC33D68, true},
      {"sg1000", "viboritas.rom", 400, {{100, 110, KEY_B}, {150, 300, KEY_UP}}, 0xBD13B740, true},
      {"sg1000", "oscar.rom", 400, {}, 0x7CC6EA0B, false},
      {"sg1000", "plot.rom", 200, {}, 0x47FCE134, false},
      {"sg1000", "vramcopy.rom", 200, {}, 0x28D5D255, false},
      {"sg1000", "test1.rom", 200, {}, 0xF9B951D5, false},
      {"sg1000", "test2.rom", 200, {}, 0xEE92EF44, false},
      {"sg1000", "music.rom", 300, {{100, 200, KEY_B}}, 0xC3A0DA45, true},
      {"sg1000", "bank.rom", 300, {}, 0x2A92E29F, true},
  };
  std::printf("programas do CVBasic (%s)\n", dir.c_str());
  for (const auto& t : tests) {
    const std::string name = std::string(t.sys) + "/" + t.file;
    const std::vector<uint8_t> rom = readFile(dir + "/" + name);
    if (rom.empty()) { check(false, name + ": arquivo não encontrado (rode o t7_tms.sh)"); continue; }
    std::unique_ptr<Core> core;
    try { core = makeCore(t.sys, rom); } catch (const std::exception& e) { check(false, name + ": " + e.what()); continue; }
    double energy = 0;
    runScript(*core, t, &energy);
    const uint32_t h = fnv(core->framebuffer(), 256 * 192 * 4);
    if (!outDir.empty()) {
      const std::string f = std::string(t.sys) + "_" + t.file;
      writePpm(outDir + "/" + f.substr(0, f.size() - 4) + ".ppm", core->framebuffer(), 256, 192);
    }
    std::printf("  %-26s tela %08X  som %8.1f  acessos rápidos demais %u\n", name.c_str(), h, energy, tooFastOf(*core));
    if (t.hash) check(h == t.hash, name + ": tela diferente da referência");
    check(tooFastOf(*core) == 0, name + ": nenhum acesso à VRAM rápido demais");
    if (t.sound) check(energy > 1.0, name + ": tem som");
  }
}

// ------------------------------------------------------------------ 3) save state e robustez
void stateTests(const std::string& dir) {
  std::printf("save state e robustez\n");
  for (const char* which : {"coleco/sgm_viboritas.rom", "sg1000/viboritas.rom", "coleco/bank.rom", "sg1000/bank.rom"}) {
    const std::vector<uint8_t> rom = readFile(dir + "/" + which);
    if (rom.empty()) continue;
    const std::string sys = std::string(which).substr(0, std::string(which).find('/'));
    auto core = makeCore(sys, rom);
    const RomTest warm{"", "", 150, {{100, 110, KEY_A | KEY_B}}, 0, false};
    runScript(*core, warm, nullptr);
    const std::vector<uint8_t> st = core->saveState();
    auto runN = [&](uint32_t& vh, uint32_t& ah) {
      std::vector<float> L(8192), R(8192);
      vh = 0x811C9DC5u; ah = 0x811C9DC5u;
      for (int f = 0; f < 90; f++) {
        core->setKeys(f < 40 ? KEY_UP | KEY_A | KEY_B : KEY_LEFT);
        core->runFrame();
        vh = fnv(core->framebuffer(), 256 * 192 * 4, vh);
        const size_t n = std::min(core->audioAvailable(), L.size());
        core->pullAudio(L.data(), R.data(), n);
        ah = fnv(L.data(), n * sizeof(float), ah);
      }
    };
    uint32_t v1, a1, v2, a2;
    runN(v1, a1);
    core->loadState(st);
    runN(v2, a2);
    check(v1 == v2 && a1 == a2, std::string(which) + ": salvar/carregar estado é determinístico");
    core->loadState(st);
    check(core->saveState() == st,std::string(which) + ": salvar → carregar → salvar dá os mesmos bytes");
    // estados danificados: lança erro ou segue rodando
    std::mt19937 rng(1234);
    for (int k = 0; k < 40; k++) {
      std::vector<uint8_t> bad = st;
      if (k % 4 == 0) bad.resize(rng() % bad.size());
      else for (int i = 0; i < 64; i++) bad[rng() % bad.size()] = static_cast<uint8_t>(rng());
      if (k % 4 == 3) for (auto& b : bad) b = static_cast<uint8_t>(rng());
      try { core->loadState(bad); } catch (const std::exception&) {}
      core->runFrame();
    }
    check(true, std::string(which) + ": estados danificados não travam");
  }
  // ROMs de lixo
  std::mt19937 rng(99);
  for (int k = 0; k < 12; k++) {
    std::vector<uint8_t> rom((1 + rng() % 64) * 1024);
    for (auto& b : rom) b = static_cast<uint8_t>(rng());
    if (k == 5) rom.assign(0x40000, 0xFF);
    for (const char* sys : {"coleco", "sg1000"}) {
      auto core = makeCore(sys, rom);
      for (int f = 0; f < 60; f++) { core->setKeys(rng()); core->runFrame(); }
    }
  }
  check(true, "ROMs de lixo rodam sem travar");
  // sem BIOS
  setFirmware("coleco", {});
  bool threw = false;
  try { ColecoCore c(std::vector<uint8_t>(0x8000, 0xFF), nullptr); } catch (const std::runtime_error& e) {
    threw = std::string(e.what()).find("coleco.rom") != std::string::npos;
  }
  check(threw, "sem a BIOS: pede o coleco.rom");
  check(isColecoRom({}, "jogo.col") && !isColecoRom({}, "jogo.bin") && isSg1000Rom({}, "jogo.sg") &&
            isSg1000Rom({}, "jogo.sc") && !isSg1000Rom({}, "jogo.sms"),
        "detecção pela extensão");
}
}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/tms";
  const std::string outDir = argc > 2 ? argv[2] : "";
  vdpTests();
  soundTests();
  if (!exists(dir)) {
    std::printf("pasta %s não existe: pulei os programas de teste\n", dir.c_str());
  } else {
    romTests(dir, outDir);
    stateTests(dir);
  }
  std::printf("RESULTADO ColecoVision/SG-1000: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
