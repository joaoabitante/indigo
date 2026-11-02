// Testes do Super Game Boy (indigo::gb::SGB, dentro do núcleo do Game Boy).
//   indigo-t7_sgb <pasta>          pasta preparada por tools/t7_sgb.sh (padrão /root/indigo-env/roms/sgb)
//   indigo-t7_sgb <pasta> <saída>  também grava a imagem final (256x224) em <saída>/sgb_final.ppm
// A ROM sgb_teste.gb é gerada por tools/t7_sgb_rom.py (código próprio): manda pacotes pelo joypad e faz
// transferências pela imagem; depois de cada etapa grava o número dela em $C000. Aqui se confere:
//   registradores do boot do SGB, MLT_REQ (número dos controles), PAL01/PAL23, ATTR_DIV/BLK/LIN/CHR,
//   MASK_EN (0-3, congelar), pacote com bit de parada errado, PAL_TRN + ATTR_TRN + PAL_SET, CHR_TRN + PCT_TRN
//   (moldura com espelhamento e cobrindo a tela), save state determinístico e estado antigo sem o bloco SGB,
//   opção desligada e jogos sem a marca SGB (continuam 160x144), extensão .sgb.
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/gb/gb.h"

using namespace indigo;
using namespace indigo::gb;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// as mesmas cores de t7_sgb_rom.py
static const int X0 = 0x001F;
static const int A[3] = {0x03E0, 0x7C00, 0x0011}, B[3] = {0x0210, 0x4210, 0x1084};
static const int C[3] = {0x2222, 0x3333, 0x4444}, D[3] = {0x5555, 0x6666, 0x7777};
static int palfn(int i, int k) { return ((i * 4 + k) * 37 + 5) & 0x7FFF; }
static uint32_t rgb(int c) { return PPU::rgb(c); }

struct Run {
  std::unique_ptr<GBCore> core;
  GB& g() { return core->gb(); }
  uint32_t px(int x, int y) { return core->framebuffer()[y * 256 + x]; }
  // ponto do bloco (bx, by) da tela do Game Boy com o tom s (tile de faixas: colunas 2s e 2s+1)
  uint32_t gbpx(int bx, int by, int s) { return px(SGB::GX + bx * 8 + 2 * s, SGB::GY + by * 8 + 1); }
  bool until(int step) {
    for (int f = 0; f < 3000; f++) {
      core->runFrame();
      if (g().read(0xC000) == step) { for (int k = 0; k < 3; k++) core->runFrame(); return true; }
    }
    return false;
  }
  void block(int bx, int by, const int* pal, const std::string& what) {
    check(gbpx(bx, by, 0) == rgb(X0), what + ": tom 0 = cor 0 comum");
    for (int s = 1; s < 4; s++) check(gbpx(bx, by, s) == rgb(pal[s - 1]), what + ": tom " + std::to_string(s));
  }
};

static uint64_t hashFb(Core& c) {
  uint64_t h = 1469598103934665603ull;
  const uint32_t* fb = c.framebuffer();
  for (int i = 0; i < c.width() * c.height(); i++) { h ^= fb[i]; h *= 1099511628211ull; }
  return h;
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/sgb";
  const std::vector<uint8_t> rom = readAll(dir + "/sgb_teste.gb");
  if (rom.size() != 0x8000) { std::printf("FALHA: sem %s/sgb_teste.gb (rode tools/t7_sgb.sh)\n", dir.c_str()); return 1; }

  setSgbEnabled(true);
  Run r{std::make_unique<GBCore>(rom, nullptr)};
  check(r.core->width() == 256 && r.core->height() == 224, "tela 256x224 com o SGB");
  check(r.core->system() == "gb", "sistema continua gb");
  SGB& s = *r.g().sgb;

  // 1: início
  check(r.until(1), "etapa 1");
  check(r.g().read(0xC001) == 0x14, "registrador C do boot = 0x14");
  check(r.gbpx(5, 5, 1) == rgb(s.pal[0][1]) && s.pal[0][1] == ((0xD8 >> 3) | ((0x90 >> 3) << 5) | ((0x48 >> 3) << 10)), "paleta inicial");
  check(r.px(0, 0) == rgb((0x28 >> 3) | ((0x2C >> 3) << 5) | ((0x40 >> 3) << 10)), "moldura própria: fundo");
  check(r.px(44, 100) == rgb((0x90 >> 3) | ((0x98 >> 3) << 5) | ((0xB0 >> 3) << 10)), "moldura própria: filete");

  // 2: MLT_REQ
  check(r.until(2), "etapa 2");
  check(r.g().read(0xC002) == 0xFF, "MLT_REQ: controle 1 (0xF)");
  check(r.g().read(0xC003) == 0xFE, "MLT_REQ: P15 sobe -> controle 2 (0xE)");
  check(r.g().read(0xC004) == 0xFF, "MLT_REQ: volta ao controle 1");
  check(r.g().read(0xC005) == 0xFF && s.players == 1, "MLT_REQ 0: um jogador");

  // 3: PAL01/PAL23 + ATTR_DIV
  check(r.until(3), "etapa 3");
  r.block(0, 0, B, "DIV esquerda (pal 1)");
  r.block(10, 3, C, "DIV linha (pal 2)");
  r.block(15, 3, D, "DIV direita (pal 3)");
  check(s.lastCmd == 0x06, "último comando ATTR_DIV");

  // 4: ATTR_BLK
  check(r.until(4), "etapa 4");
  r.block(4, 4, C, "BLK dentro");
  r.block(2, 4, C, "BLK linha automática");
  r.block(0, 0, B, "BLK fora intocado");
  r.block(8, 10, D, "BLK só linha");
  r.block(10, 10, C, "BLK dentro do 2º intocado");
  r.block(13, 13, D, "BLK fora do 2º intocado");

  // 5: ATTR_LIN
  check(r.until(5), "etapa 5");
  r.block(0, 17, A, "LIN horizontal");
  r.block(19, 0, B, "LIN vertical");
  r.block(19, 17, B, "LIN a última vence");

  // 6: ATTR_CHR
  check(r.until(6), "etapa 6");
  const int* chrP[5] = {A, B, C, D, D};
  for (int x = 0; x < 5; x++) r.block(x, 0, chrP[x], "CHR bloco " + std::to_string(x));
  r.block(5, 0, B, "CHR depois do fim intocado");

  // 7-10: MASK_EN
  check(r.until(7), "etapa 7");
  check(r.gbpx(3, 3, 2) == 0xFF000000u && r.gbpx(12, 9, 0) == 0xFF000000u, "MASK_EN 2: preto");
  check(r.px(0, 0) != 0xFF000000u, "MASK_EN 2: moldura continua");
  check(r.until(8), "etapa 8");
  check(r.gbpx(3, 3, 2) == rgb(X0), "MASK_EN 3: cor 0");
  check(r.until(9), "etapa 9");
  check(s.mask == 3 && r.gbpx(3, 3, 2) == rgb(X0), "pacote com bit de parada errado ignorado");
  check(r.until(10), "etapa 10");
  check(s.mask == 0, "MASK_EN 0");
  r.block(0, 0, A, "imagem de volta");
  std::vector<uint32_t> snap;
  for (int y = 0; y < 144; y++) for (int x = 0; x < 160; x++) snap.push_back(r.px(SGB::GX + x, SGB::GY + y));

  // 11: congelado durante PAL_TRN/ATTR_TRN
  check(r.until(11), "etapa 11");
  bool same = true;
  for (int y = 0; y < 144; y++) for (int x = 0; x < 160; x++) same &= r.px(SGB::GX + x, SGB::GY + y) == snap[y * 160 + x];
  check(same && s.mask == 1, "MASK_EN 1: imagem congelada durante as transferências");

  // 12: PAL_SET com ATF 5
  check(r.until(12), "etapa 12");
  check(s.mask == 0, "PAL_SET cancela a máscara");
  const int p20[3] = {palfn(20, 1), palfn(20, 2), palfn(20, 3)}, p40[3] = {palfn(40, 1), palfn(40, 2), palfn(40, 3)};
  check(s.pal[0][1] == palfn(10, 1) && s.pal[2][3] == palfn(30, 3) && s.pal[3][0] == palfn(10, 0), "PAL_SET: paletas do PAL_TRN");
  check(r.gbpx(7, 0, 0) == rgb(palfn(10, 0)), "PAL_SET: cor 0 comum");
  for (int s1 = 1; s1 < 4; s1++) {
    check(r.gbpx(7, 0, s1) == rgb(p20[s1 - 1]), "ATF 5 linha 0 = pal 1, tom " + std::to_string(s1));
    check(r.gbpx(5, 5, s1) == rgb(p40[s1 - 1]), "ATF 5 resto = pal 3, tom " + std::to_string(s1));
  }

  // 13: moldura do jogo
  check(r.until(13), "etapa 13");
  check(r.px(100, 200) == rgb(0x1234), "PCT: tile 1, pal 4, cor 5");
  check(r.px(0, 0) == rgb(0x7001) && r.px(255, 7) == rgb(0x7001), "PCT: linha 0 = tile 2, pal 5, cor 15");
  check(r.px(8, 8) == rgb(0x808) && r.px(15, 8) == rgb(0x101) && r.px(10, 12) == rgb(0x606), "PCT: tile espelhado na horizontal");
  check(r.px(SGB::GX, SGB::GY) == rgb(0x0ABC) && r.px(SGB::GX + 7, SGB::GY + 7) == rgb(0x0ABC), "PCT: tile 0x80 (CHR_TRN 2ª metade) cobre a tela");
  check(r.px(SGB::GX + 12, SGB::GY + 20) == rgb(palfn(40, 2)), "tela do Game Boy aparece no buraco da moldura");
  check(r.g().read(0xC000) == 13 || r.until(0xFF), "fim da ROM");
  if (argc > 2) {
    std::ofstream o(std::string(argv[2]) + "/sgb_final.ppm", std::ios::binary);
    o << "P6\n256 224\n255\n";
    for (int i = 0; i < 256 * 224; i++) { const uint32_t c = r.core->framebuffer()[i]; o.put(char(c & 0xFF)); o.put(char(c >> 8 & 0xFF)); o.put(char(c >> 16 & 0xFF)); }
  }
  r.until(0xFF);

  // controle normal continua funcionando (um jogador)
  r.core->setKeys(KEY_A);
  r.g().write(0xFF00, 0x10);
  check((r.g().read(0xFF00) & 0x0F) == 0x0E, "joypad: botão A com o SGB");
  r.core->setKeys(0);

  // save state determinístico
  {
    Run q{std::make_unique<GBCore>(rom, nullptr)};
    q.until(11);
    const std::vector<uint8_t> st = q.core->saveState();
    for (int f = 0; f < 200; f++) q.core->runFrame();
    const uint64_t h1 = hashFb(*q.core);
    q.core->loadState(st);
    for (int f = 0; f < 200; f++) q.core->runFrame();
    check(hashFb(*q.core) == h1, "save state no meio das transferências: mesma imagem 200 quadros depois");
    // estado sem o bloco do SGB (de antes do SGB existir) ainda carrega
    auto keep = std::move(q.g().sgb);
    const std::vector<uint8_t> old = q.core->saveState();
    q.g().sgb = std::move(keep);
    bool ok = true;
    try { q.core->loadState(old); } catch (...) { ok = false; }
    check(ok, "estado antigo sem o bloco SGB carrega");
    // estado danificado: erro e o jogo segue igual
    std::vector<uint8_t> bad = st;
    bad.resize(bad.size() - 100);
    const uint64_t h2 = hashFb(*q.core);
    bool threw = false;
    try { q.core->loadState(bad); } catch (...) { threw = true; }
    check(threw && hashFb(*q.core) == h2, "estado truncado recusado sem estragar o jogo");
  }

  // robustez: pacotes aleatórios pelo joypad e estados com bytes trocados não podem derrubar o núcleo
  {
    Run q{std::make_unique<GBCore>(rom, nullptr)};
    q.until(3);
    uint32_t x = 12345;
    auto rnd = [&x] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
    for (int f = 0; f < 300; f++) {
      for (int k = 0; k < 4000; k++) {
        // pulso de início às vezes, bits aleatórios com espaços, como um jogo mandaria
        const uint32_t v = rnd();
        q.g().write(0xFF00, (v & 0xFF) < 2 ? 0x00 : (v & 0x100) ? 0x10 : 0x20);
        q.g().write(0xFF00, 0x30);
      }
      q.core->runFrame();
    }
    check(q.g().sgb->cmdCount > 0, "pacotes aleatórios: comandos recebidos (" + std::to_string(q.g().sgb->cmdCount) + ") sem travar");
    const std::vector<uint8_t> st = q.core->saveState();
    int loaded = 0;
    for (int t = 0; t < 300; t++) {
      std::vector<uint8_t> b = st;
      for (int k = 0; k < 8; k++) b[b.size() - 1 - rnd() % 180000 % b.size()] = static_cast<uint8_t>(rnd());
      try { q.core->loadState(b); loaded++; for (int f = 0; f < 3; f++) q.core->runFrame(); } catch (...) {}
    }
    check(true, "estados com bytes trocados: " + std::to_string(loaded) + " carregados, nenhum derrubou o núcleo");
  }

  // opção desligada, jogo sem a marca, jogo de GBC, nome .sgb
  setSgbEnabled(false);
  {
    Run q{std::make_unique<GBCore>(rom, nullptr)};
    check(!q.g().sgb && q.core->width() == 160 && q.core->height() == 144, "opção desligada: 160x144");
    q.until(1);
    check(q.g().read(0xC001) == 0x13, "opção desligada: registrador C de DMG");
  }
  setSgbEnabled(true);
  {
    std::vector<uint8_t> r2 = rom;
    r2[0x146] = 0;
    check(GBCore(r2, nullptr).width() == 160, "sem a marca 0x146=3: 160x144");
    r2 = rom; r2[0x14B] = 0x01;
    check(GBCore(r2, nullptr).width() == 160, "sem o licenciado 0x33: 160x144");
    r2 = rom; r2[0x143] = 0x80;
    GBCore cg(r2, nullptr);
    check(cg.width() == 160 && cg.system() == "gbc", "jogo de GBC continua GBC");
    check(GBCore(rom, nullptr, Mode::Dmg).width() == 160, "modo DMG forçado: sem SGB");
  }
#ifndef T7_SGB_SO_GB  // (para compilar só o núcleo do Game Boy com sanitizadores)
  {
    auto c = createCore(rom, "teste.sgb", nullptr);
    check(c && c->width() == 256 && c->note().find("Super Game Boy") != std::string::npos, "createCore(.sgb) com SGB e aviso");
  }
#endif

  std::printf("RESULTADO sgb: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
