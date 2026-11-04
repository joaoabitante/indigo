// Testes do Dreamcast: CPU SH-4 (o mesmo C no SH-4 emulado e no PC, saídas iguais), BIOS de teste do Índigo dando
// a partida pelo GD-ROM (.gdi, .iso e .chd), jogo de teste (TA/PVR conferidos pixel a pixel na VRAM, controle pelo
// Maple, VMU, AICA com ARM7 e som, TMU, TRAPA, DMA do GD-ROM e do canal 2), save state determinístico e recusa de
// estados adulterados, reconhecimento de BIOS/flash/disco e a velocidade.
//   indigo-t7_dc pasta   (pasta = /root/indigo-env/roms/dc, montada pelo t7_dc.sh)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/dc/dc.h"

using namespace indigo;
using namespace indigo::dc;

static int fails = 0, checks = 0;
#define CHECK(cond, ...)                    \
  do {                                      \
    checks++;                               \
    if (!(cond)) {                          \
      fails++;                              \
      std::printf("  FALHA: " __VA_ARGS__); \
      std::printf("\n");                    \
    }                                       \
  } while (0)

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string dir;

static uint16_t px(System& s, int x, int y) {
  const uint32_t a = 0x200000 + uint32_t(y * 640 + x) * 2;
  uint16_t v;
  std::memcpy(&v, &s.vram[System::vram32(a)], 2);
  return v;
}
static uint64_t hashOf(const std::vector<uint8_t>& v, uint64_t h = 1469598103934665603ull) {
  for (uint8_t b : v) h = (h ^ b) * 1099511628211ull;
  return h;
}
static uint64_t sysHash(System& s) {
  uint64_t h = hashOf(s.ram);
  h = hashOf(s.vram, h);
  h = hashOf(s.aram, h);
  for (size_t i = 0; i < size_t(s.fbW) * size_t(s.fbH); i++) h = (h ^ s.fb[i]) * 1099511628211ull;
  return h ^ uint64_t(s.cpu.pc) ^ uint64_t(s.cpu.cycles) << 1;
}
static bool runUntil(DcCore& c, const char* text, int maxFrames) {
  for (int i = 0; i < maxFrames; i++) {
    if (c.sys().cpu.serial.find(text) != std::string::npos) return true;
    c.runFrame();
  }
  return c.sys().cpu.serial.find(text) != std::string::npos;
}

// ------------------------------------------------------------------------------------------------ partes isoladas
static void testBasics(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash) {
  std::printf("== partes isoladas\n");
  CHECK(isDcBoot(boot, "dc_boot.bin"), "BIOS reconhecida pelo nome");
  CHECK(!isDcBoot(boot, "jogo.bin"), "BIOS com outro nome não é reconhecida");
  std::vector<uint8_t> md(BOOT_SIZE, 0);
  std::memcpy(&md[0x100], "SEGA MEGA DRIVE", 15);
  CHECK(!isDcBoot(md, "dc_boot.bin"), "cartucho de Mega Drive de 2 MB não é BIOS");
  CHECK(isDcFlash(flash, "qualquer.bin"), "flash reconhecida pelo KATANA_FLASH");
  CHECK(!isDcFlash(std::vector<uint8_t>(FLASH_SIZE, 0xFF), "x.bin"), "128 KB vazios não são flash");
  CHECK(createCore(boot, "dc_boot.bin", nullptr) == nullptr, "a BIOS não vira jogo");
  CHECK(createCore(flash, "dc_flash.bin", nullptr) == nullptr, "a flash não vira jogo");
  // VRAM: caminho de 32 bits e de 64 bits
  System s(boot, flash, nullptr);
  s.write32(0x05000000, 0x11223344);
  s.write32(0x05400000, 0x55667788);
  s.write32(0x05000004, 0x99AABBCC);
  CHECK(s.read32(0x04000000) == 0x11223344 && s.read32(0x04000004) == 0x55667788 && s.read32(0x04000008) == 0x99AABBCC,
        "VRAM: palavras de 32 bits alternam os bancos no caminho de 64 bits");
  CHECK(s.read32(0x0C000000) == 0 && (s.write32(0x0E000010, 0xCAFE), s.read32(0x0C000010) == 0xCAFE), "RAM espelhada");
  CHECK(s.read32(0x005F689C) == 0x0B && s.read32(0x005F8000) == 0x17FD11DB, "identificação do Holly e da PVR");
  // VMU formatado
  const auto v = blankVmu();
  CHECK(v.size() == VMU_SIZE && v[255 * 512] == 0x55 && v[255 * 512 + 0x46] == 254 && v[254 * 512 + 2 * 241] == 0xFA,
        "VMU vazio formatado (raiz, FAT, diretório)");
  // sem BIOS: mensagem padrão
  std::string msg;
  try { DcCore c(nullptr, nullptr); } catch (const std::exception& e) { msg = e.what(); }
  CHECK(msg.find("precisa da BIOS") != std::string::npos && msg.find("dc_boot.bin") != std::string::npos,
        "sem a BIOS: mensagem padrão (%s)", msg.c_str());
}

// ------------------------------------------------------------------------------------------------ CPU
static void testCpu(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash) {
  std::printf("== CPU SH-4 (mesmo C no SH-4 e no PC)\n");
  const auto prog = readAll(dir + "/cpu.bin");
  std::ifstream pf(dir + "/cpu_pc.txt");
  std::string expect((std::istreambuf_iterator<char>(pf)), std::istreambuf_iterator<char>());
  CHECK(!prog.empty() && !expect.empty(), "cpu.bin e cpu_pc.txt existem");
  if (prog.empty()) return;
  System s(boot, flash, nullptr);
  s.loadProgram(prog, 0x8C010000, 0x8C010000);
  for (int i = 0; i < 600 && s.cpu.serial.find("FIM") == std::string::npos; i++) s.runFrame();
  std::printf("%s", s.cpu.serial.c_str());
  // linha a linha
  size_t a = 0, b = 0;
  int lines = 0;
  while (a < expect.size()) {
    const size_t ea = expect.find('\n', a), eb = s.cpu.serial.find('\n', b);
    const std::string le = expect.substr(a, ea - a), ls = eb == std::string::npos ? "" : s.cpu.serial.substr(b, eb - b);
    CHECK(le == ls, "CPU: esperado \"%s\", veio \"%s\"", le.c_str(), ls.c_str());
    lines++;
    if (ea == std::string::npos || eb == std::string::npos) break;
    a = ea + 1; b = eb + 1;
  }
  CHECK(lines == 9, "CPU: 9 linhas (%d)", lines);
  CHECK(s.cpu.illegal == 0, "CPU: nenhuma instrução ilegal (%u)", s.cpu.illegal);
}

// ------------------------------------------------------------------------------------------------ jogo de teste
static void checkScene(System& s, uint32_t sqx) {
  CHECK(px(s, 70, 30) == 0xF800, "opaco vermelho (%04X)", px(s, 70, 30));
  const uint16_t t = px(s, 85, 85);
  const int r = t >> 11, g = (t >> 5) & 63, b = t & 31;
  CHECK(r >= 14 && r <= 16 && g >= 31 && g <= 33 && b == 0, "translúcido 50%% verde sobre vermelho (%04X)", t);
  const uint16_t gv = px(s, 190, 26), gr = px(s, 146, 117);
  CHECK(((gv >> 5) & 63) > 50 && (gv >> 11) < 6, "Gouraud perto do vértice verde (%04X)", gv);
  CHECK((gr >> 11) > 25 && ((gr >> 5) & 63) < 12, "Gouraud perto do vértice vermelho (%04X)", gr);
  CHECK(px(s, 40, 150) == 0x001F && px(s, 100, 200) == 0x001F && px(s, 150, 250) == 0x07E0,
        "profundidade GEQUAL (%04X %04X %04X)", px(s, 40, 150), px(s, 100, 200), px(s, 150, 250));
  int bad[6] = {};
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 8; x++) {
      auto texcol = [](int tx, int ty) { return uint16_t((tx * 4) << 11 | (ty * 8) << 5 | (tx + ty)); };
      if (px(s, 260 + 8 * x + 4, 20 + 8 * y + 4) != texcol(x, y)) bad[0]++;
      if (px(s, 340 + 8 * x + 4, 20 + 8 * y + 4) != texcol(7 - x, y)) bad[1]++;
      if (px(s, 500 + 8 * x + 4, 20 + 8 * y + 4) != uint16_t(0xF800 | (y * 8 + x))) bad[2]++;
      if (px(s, 260 + 8 * x + 4, 100 + 8 * y + 4) != uint16_t(((x + y) & 15) << 1)) bad[3]++;
      if (px(s, 340 + 8 * x + 4, 100 + 8 * y + 4) != (((x + y) & 1) ? 0x07E0 : 0x0008)) bad[4]++;
    }
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 16; x++) {
      const uint32_t code = uint32_t((y / 2) * 8 + x / 2 + 16), tt = uint32_t((x & 1) << 1 | (y & 1));
      const uint16_t e = tt == 0 ? uint16_t(code << 8) : tt == 1 ? uint16_t(code << 3) : tt == 2 ? uint16_t(code << 8 | 0x1F) : 0xFFFF;
      if (px(s, 420 + 4 * x + 2, 20 + 4 * y + 2) != e) bad[5]++;
    }
  if (bad[0] && std::getenv("DC_DETALHE"))
    for (int y = 0; y < 8; y++) {
      for (int x = 0; x < 8; x++) std::printf(" %04X/%04X", px(s, 260 + 8 * x + 4, 20 + 8 * y + 4), uint16_t((x * 4) << 11 | (y * 8) << 5 | (x + y)));
      std::printf("\n");
    }
  CHECK(!bad[0], "textura 565 twiddled (%d texels errados)", bad[0]);
  CHECK(!bad[1], "textura 565 linear (%d)", bad[1]);
  CHECK(!bad[2], "textura de paleta de 8 bits (%d)", bad[2]);
  CHECK(!bad[3], "textura de paleta de 4 bits (%d)", bad[3]);
  CHECK(!bad[4], "punch-through 1555 (%d)", bad[4]);
  CHECK(!bad[5], "textura VQ (%d)", bad[5]);
  CHECK(px(s, 220, 340) == 0x4208 && px(s, 190, 310) == 0x8410 && px(s, 250, 370) == 0x8410,
        "volume modificador: sombra simples só dentro da caixa (%04X %04X)", px(s, 220, 340), px(s, 190, 310));
  CHECK(px(s, 425, 325) == 0xFFE0, "sprite amarelo (%04X)", px(s, 425, 325));
  CHECK(px(s, 600, 460) == 0x0008, "plano de fundo (%04X)", px(s, 600, 460));
  CHECK(px(s, int(sqx) + 8, 408) == 0xFFFF, "quadrado do controle em x=%u (%04X)", sqx, px(s, int(sqx) + 8, 408));
}

static uint32_t lastX(const std::string& serial) {
  const size_t p = serial.rfind(" x ");
  return p == std::string::npos ? 0 : uint32_t(std::strtoul(serial.c_str() + p + 3, nullptr, 10));
}

static void testGame(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash) {
  std::printf("== BIOS de teste + GD de teste (.gdi)\n");
  std::string err;
  auto disc = cdrom::open(dir + "/disco/teste.gdi", &err);
  CHECK(disc != nullptr, "abre o .gdi (%s)", err.c_str());
  if (!disc) return;
  CHECK(cdrom::detectSystem(*disc) == "dc", "detectSystem(.gdi) = dc");
  CHECK(disc->tracks().size() == 3 && disc->tracks()[2].start == 45000 && disc->tracks()[1].type == cdrom::TrackType::Audio,
        "faixas do .gdi");
  DcCore c(std::move(disc), nullptr, &boot, &flash);
  CHECK(c.title() == "INDIGO TESTE DC", "título do IP.BIN (%s)", c.title().c_str());
  System& s = c.sys();
  const bool started = runUntil(c, "JOGO: quadro 32", 200);
  std::printf("%s", s.cpu.serial.substr(0, 900).c_str());
  CHECK(started, "o jogo de teste chegou ao quadro 32");
  const std::string& o = s.cpu.serial;
  CHECK(o.find("BIOS: flash ok") != std::string::npos, "BIOS leu a flash");
  CHECK(o.find("BIOS: dados no FAD 45150") != std::string::npos, "TOC da área de alta densidade");
  CHECK(o.find("BIOS: IP.BIN ok, partida 1ST_READ.BIN") != std::string::npos, "IP.BIN lido do GD");
  CHECK(o.find("JOGO: trapa 1 tra 00000088") != std::string::npos, "TRAPA e o registrador TRA");
  CHECK(o.find("JOGO: vmu ok") != std::string::npos, "VMU: gravar e ler bloco pelo Maple, nome do controle");
  CHECK(o.find("JOGO: dma do gd ok") != std::string::npos, "leitura do disco pelo DMA do GD-ROM");
  CHECK(o.find("JOGO: yuv ok") != std::string::npos, "conversor YUV terminou com a interrupção");
  {
    // macrobloco YUV420 -> YUV422 linear: pixel (x, y) tem U/V da amostra (x/2, y/2) e Y do bloco 8x8 dele
    int bad = 0;
    const uint8_t* t = &s.vram[0x608000];
    for (int y = 0; y < 16; y++)
      for (int x = 0; x < 16; x += 2) {
        const uint8_t* p = t + y * 32 + x * 2;
        const int blk = (y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0);
        const int y0 = (blk * 64 + (y & 7) * 8 + (x & 7)) & 0xFF;
        if (p[0] != 0x40 + (y / 2) * 8 + x / 2 || p[1] != y0 || p[2] != 0x80 || p[3] != ((y0 + 1) & 0xFF)) bad++;
      }
    CHECK(bad == 0, "conversor YUV420 -> YUV422 (%d pares errados)", bad);
  }
  const size_t tp = o.find("JOGO: tmu ");
  CHECK(tp != std::string::npos, "relatório do quadro 30");
  if (tp != std::string::npos) {
    unsigned tmu = 0, arm = 0, irqs = 0, sq = 0;
    std::sscanf(o.c_str() + tp, "JOGO: tmu %u arm %X %u sq %u", &tmu, &arm, &irqs, &sq);
    CHECK(tmu >= 400 && tmu <= 700, "TMU0 a 1 kHz por ~0,5 s (%u interrupções)", tmu);
    CHECK(arm == 0xC0FFEE11, "ARM7 do AICA rodou o programa (%08X)", arm);
    CHECK(irqs == 1, "interrupção do ARM para o SH-4 pelo Holly (%u)", irqs);
    CHECK(sq > 100, "store queues para o TA (%u)", sq);
  }
  uint32_t arm2 = 0;
  std::memcpy(&arm2, &s.aram[0x10004], 4);
  CHECK(arm2 > 1000, "ARM7 segue contando (%u)", arm2);
  checkScene(s, lastX(o));
  CHECK(s.fbW == 640 && s.fbH == 480, "saída de vídeo 640x480 (%dx%d)", s.fbW, s.fbH);
  CHECK(s.fb[30 * 640 + 70] == 0xFF0000FFu, "saída de vídeo: vermelho em ABGR (%08X)", s.fb[30 * 640 + 70]);
  // som: onda quadrada de 441 Hz (período de 100 amostras)
  {
    std::vector<int16_t> a;
    s.aica.out.clear();
    for (int f = 0; f < 60; f++) { c.runFrame(); a.insert(a.end(), s.aica.out.begin(), s.aica.out.end()); s.aica.out.clear(); }
    int mx = 0, cross = 0;
    for (size_t i = 2; i < a.size(); i += 2) {
      mx = std::max(mx, std::abs(int(a[i])));
      if ((a[i] >= 0) != (a[i - 2] >= 0)) cross++;
    }
    const double secs = double(a.size() / 2) / 44100.0;
    CHECK(a.size() / 2 > 40000 && a.size() / 2 < 48000, "44,1 kHz (%zu amostras em 60 quadros)", a.size() / 2);
    CHECK(mx > 3000, "som do canal 0 (pico %d)", mx);
    CHECK(std::fabs(cross / secs / 2 - 441) < 15, "frequência da onda quadrada (%.1f Hz)", cross / secs / 2);
  }
  // controle: direita por 20 quadros
  const uint32_t x0 = lastX(s.cpu.serial);
  c.setKeys(KEY_RIGHT);
  for (int f = 0; f < 20; f++) c.runFrame();
  c.setKeys(0);
  for (int f = 0; f < 18; f++) c.runFrame();
  const uint32_t x1 = lastX(s.cpu.serial);
  CHECK(s.cpu.serial.find("JOGO: botoes 0000FF7F") != std::string::npos, "Maple: direita apertada chega ao jogo");
  CHECK(x1 >= x0 + 36 && x1 <= x0 + 42, "quadrado andou com o controle (%u -> %u)", x0, x1);
  checkScene(s, x1);
  c.setKeys(KEY_A | KEY_START | KEY_L);
  for (int f = 0; f < 3; f++) c.runFrame();
  c.setKeys(0);
  CHECK(s.cpu.serial.find("JOGO: botoes 0000FFF3") != std::string::npos || s.cpu.serial.find("JOGO: botoes 0000FFF3") != std::string::npos,
        "Maple: A e Start (botões FFF3)");
  // save do VMU
  CHECK(c.saveDirty(), "VMU gravado: save pendente");
  const auto sv = c.exportSave();
  bool vmuOk = sv.size() == VMU_SIZE + FLASH_SIZE;
  for (uint32_t i = 0; i < 32 && vmuOk; i++) {
    uint32_t w;
    std::memcpy(&w, &sv[5 * 512 + i * 4], 4);
    if (w != 0x494E4400u + i) vmuOk = false;
  }
  CHECK(vmuOk, "save = VMU (bloco 5 gravado pelo jogo) + flash");
  CHECK(!c.saveDirty(), "save exportado");

  // save state: determinismo
  std::printf("== save state\n");
  const auto st = c.saveState();
  for (int f = 0; f < 10; f++) c.runFrame();
  const uint64_t h1 = sysHash(s);
  const std::string ser1 = s.cpu.serial;
  c.loadState(st);
  for (int f = 0; f < 10; f++) c.runFrame();
  CHECK(sysHash(s) == h1, "mesmo estado depois de carregar e rodar 10 quadros");
  // adulterados: recusados sem derrubar
  bool threw = false;
  try { c.loadState(std::vector<uint8_t>(st.begin(), st.begin() + long(st.size() / 2))); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "estado truncado recusado");
  threw = false;
  try { auto x = st; x[0] ^= 0xFF; c.loadState(x); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "estado de outro console recusado");
  threw = false;
  try { auto x = st; x.push_back(1); c.loadState(x); } catch (const std::exception&) { threw = true; }
  CHECK(threw, "estado com bytes sobrando recusado");
  // bytes aleatórios trocados (fora das memórias grandes): carrega ou recusa, e o console continua rodando
  std::mt19937 rng(7);
  const size_t tail = RAM_SIZE + VRAM_SIZE + ARAM_SIZE + FLASH_SIZE + 4 * 5;
  int loaded = 0, refused = 0;
  for (int k = 0; k < 60; k++) {
    auto x = st;
    for (int j = 0; j < 8; j++) {
      const size_t p = tail + rng() % (x.size() - tail);
      x[p] = uint8_t(rng());
    }
    try { c.loadState(x); loaded++; } catch (const std::exception&) { refused++; }
    c.runFrame();
  }
  CHECK(loaded + refused == 60, "60 estados com bytes trocados: %d carregados, %d recusados, sem cair", loaded, refused);
  c.loadState(st);
  c.runFrame();
  CHECK(true, "volta ao estado bom");
}

static void testIso(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash) {
  std::printf("== CD comum (.iso) e .chd\n");
  std::string err;
  auto disc = cdrom::open(dir + "/disco/teste_cd.iso", &err);
  CHECK(disc && cdrom::detectSystem(*disc) == "dc", "detectSystem(.iso) = dc");
  if (disc) {
    DcCore c(std::move(disc), nullptr, &boot, &flash);
    CHECK(runUntil(c, "JOGO: quadro 0", 200), "BIOS de teste dá a partida no .iso (TOC de CD)");
    CHECK(c.sys().cpu.serial.find("BIOS: dados no FAD 150") != std::string::npos, "CD: dados no FAD 150");
  }
  auto chd = cdrom::open(dir + "/disco/teste.chd", &err);
  if (!chd) { std::printf("  .chd não existe (sem o chdman): pulado\n"); return; }
  auto gdi = cdrom::open(dir + "/disco/teste.gdi", &err);
  CHECK(cdrom::detectSystem(*chd) == "dc", "detectSystem(.chd do GD) = dc");
  CHECK(chd->tracks().size() == 3 && chd->tracks()[2].start == 45000, "faixa 3 do .chd no LBA 45000 (%u)",
        chd->tracks().size() == 3 ? chd->tracks()[2].start : 0);
  int diff = 0;
  uint8_t a[2352], b[2352];
  for (uint32_t l : {0u, 16u, 100u, 45000u, 45016u, 45020u, 45021u, 45025u}) {
    gdi->readRaw(l, a); chd->readRaw(l, b);
    if (std::memcmp(a + 16, b + 16, 2048)) diff++;
  }
  CHECK(diff == 0, ".chd e .gdi com os mesmos setores (%d diferentes)", diff);
  DcCore c(std::move(chd), nullptr, &boot, &flash);
  CHECK(runUntil(c, "JOGO: quadro 0", 200), "BIOS de teste dá a partida no .chd");
}

// BIOS de código aleatório (com registradores apontando para o hardware) e disco de verdade: não pode cair
static void testFuzz(const std::vector<uint8_t>& flash) {
  std::printf("== robustez: código aleatório\n");
  std::mt19937 rng(99);
  int runs = 0;
  for (int k = 0; k < 24; k++) {
    std::vector<uint8_t> boot(BOOT_SIZE);
    for (auto& b : boot) b = uint8_t(rng());
    std::string err;
    auto disc = cdrom::open(dir + "/disco/teste.gdi", &err);
    DcCore c(std::move(disc), nullptr, &boot, &flash);
    System& s = c.sys();
    static const uint32_t bases[] = {0xA05F6800, 0xA05F8000, 0xA05F7000, 0xA0700000, 0xA0800000, 0xA4000000,
                                     0xA5000000, 0x10000000, 0xFFD80000, 0xFFA00000, 0x8C000000, 0xE0000000,
                                     0xA05F9000, 0xA05F7400, 0xA05F6C00, 0xFF000000};
    for (int i = 0; i < 16; i++) s.cpu.r[i] = bases[(i + k) & 15] + (rng() & 0xFC);
    for (int f = 0; f < 20; f++) {
      c.runFrame();
      // de vez em quando volta para o código aleatório (o programa pode ter pulado para longe)
      if (f % 5 == 4) { s.cpu.pc = 0xA0000000 + (rng() & 0x1FFFFE); s.cpu.sleeping = false; }
    }
    auto st = c.saveState();
    c.loadState(st);
    runs++;
  }
  CHECK(runs == 24, "24 BIOS aleatórias rodaram 20 quadros sem cair");
}

// defeitos achados na revisão: .cue com número de faixa fora de 1..99, DMA que grava no próprio registrador de
// disparo, estado do GD-ROM com bufPos no fim do buffer, BIOS sem flash e som que ninguém puxa
static void testRevisao(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash) {
  std::printf("== revisão\n");
  int bad = 0;
  for (const char* n : {"0", "129", "-50", "100"}) {
    const std::string cue = dir + "/disco/_revisao.cue";
    if (FILE* f = std::fopen(cue.c_str(), "w")) {
      std::fprintf(f, "FILE \"teste_cd.iso\" BINARY\n  TRACK %s MODE1/2048\n    INDEX 01 00:00:00\n", n);
      std::fclose(f);
    }
    std::string err;
    if (cdrom::open(cue, &err)) bad++;
    std::remove(cue.c_str());
  }
  CHECK(bad == 0, ".cue com TRACK 0, 129, -50 ou 100 recusado (%d aceitos)", bad);
  {
    System s(boot, flash, nullptr);
    // canal 2: 32 bytes da RAM para o próprio SB (C2DSTAT, C2DLEN, C2DST = 1 de novo)
    s.write32(0x0C000000, 0x005F6800); s.write32(0x0C000004, 32); s.write32(0x0C000008, 1);
    s.cpu.sar[2] = 0x0C000000;
    s.write32(0x005F6800, 0x005F6800); s.write32(0x005F6804, 32);
    s.write32(0x005F6808, 1);
    // PVR-DMA: VRAM para os registradores dela mesma (PDEN e PDST ligados)
    static const uint32_t pv[8] = {0x05000000, 0x005F7C00, 32, 1, 0, 1, 1, 0};  // PDSTAP..PDST
    for (uint32_t i = 0; i < 8; i++) s.write32(0x05000000 + 4 * i, pv[i]);
    s.write32(0x005F7C00, 0x05000000); s.write32(0x005F7C04, 0x005F7C00); s.write32(0x005F7C08, 32);
    s.write32(0x005F7C0C, 1); s.write32(0x005F7C14, 1);
    s.write32(0x005F7C18, 1);
    for (int f = 0; f < 3; f++) s.runFrame();
    CHECK(true, "DMA do canal 2 e da PVR em cima do próprio registrador de disparo não entra em recursão");
  }
  {
    std::string err;
    DcCore c(cdrom::open(dir + "/disco/teste.gdi", &err), nullptr, &boot, &flash);
    System& s = c.sys();
    s.gdrom.state = 3; s.gdrom.buf.assign(1, 0); s.gdrom.bufPos = 1;
    const auto st = c.saveState();
    c.loadState(st);
    s.write16(0x005F7080, 0x1234);
    s.gdrom.state = 3; s.gdrom.buf.assign(1, 0); s.gdrom.bufPos = 1;  // mesmo sem passar pelo estado
    s.write16(0x005F7080, 0x1234);
    CHECK(s.gdrom.bufPos <= s.gdrom.buf.size(), "GD-ROM: estado com o buffer de entrada já cheio não grava fora");
    for (int f = 0; f < 120; f++) c.runFrame();
    CHECK(s.aica.out.size() <= 44100 * 2, "som sem ninguém puxar fica em até 1 s (%zu amostras)", s.aica.out.size() / 2);
  }
  {
    std::string err;
    std::vector<uint8_t> semFlash;
    DcCore c(cdrom::open(dir + "/disco/teste.gdi", &err), nullptr, &boot, &semFlash);
    for (int f = 0; f < 5; f++) c.runFrame();
    CHECK(true, "sem a flash o núcleo roda (flash em branco)");
  }
  std::vector<uint8_t> b2(BOOT_SIZE, 0);
  CHECK(isDcBoot(b2, "boot.bin") && isDcBoot(b2, "dc/boot.bin") && !isDcBoot(b2, "reboot.bin"),
        "BIOS chamada boot.bin é reconhecida");
}

static void testSpeed(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash) {
  std::printf("== velocidade\n");
  std::string err;
  auto disc = cdrom::open(dir + "/disco/teste.gdi", &err);
  if (!disc) return;
  DcCore c(std::move(disc), nullptr, &boot, &flash);
  runUntil(c, "JOGO: quadro 16", 200);
  const auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < 300; f++) c.runFrame();
  const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("  velocidade: %.0f quadros/s (%.2fx) no jogo de teste (SH-4 a 200 MHz, 1 instrução por ciclo)\n",
              300 / sec, 300 / sec / 59.94);
  CHECK(300 / sec > 1, "roda");
  // CPU sem laços de espera: o teste da CPU (só conta)
  const auto prog = readAll(dir + "/cpu.bin");
  System s(boot, flash, nullptr);
  s.loadProgram(prog, 0x8C010000, 0x8C010000);
  const auto t1 = std::chrono::steady_clock::now();
  int fr = 0;
  while (fr < 600 && s.cpu.serial.find("FIM") == std::string::npos) { s.runFrame(); fr++; }
  const double sec2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
  std::printf("  SH-4 contando sem parar: %.0f milhões de instruções/s (%.0f%% dos 200 MHz)\n",
              double(s.cpu.executed) / sec2 / 1e6, double(s.cpu.executed) / sec2 / 2e6);
}

int main(int argc, char** argv) {
  dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/dc";
  const auto boot = readAll(dir + "/bios/teste_boot.bin");
  const auto flash = readAll(dir + "/bios/teste_flash.bin");
  if (boot.size() != BOOT_SIZE || flash.size() != FLASH_SIZE) {
    std::printf("material de teste ausente: rode tools/t7_dc.sh %s\n", dir.c_str());
    return 1;
  }
  testBasics(boot, flash);
  testCpu(boot, flash);
  testGame(boot, flash);
  testIso(boot, flash);
  testFuzz(flash);
  testRevisao(boot, flash);
  testSpeed(boot, flash);
  std::printf("RESULTADO dc: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
