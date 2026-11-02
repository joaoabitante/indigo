// Testes do Mega Duck (variante do núcleo do Game Boy em src/gb).
//   indigo-t7_duck [pasta]   pasta preparada por tools/t7_duck.sh (padrão /root/indigo-env/roms/duck); as ROMs
//                            de teste são geradas aqui mesmo e gravadas lá, para abrir no app
// Verificações:
//   1. o mesmo programa montado duas vezes, uma com os endereços e bits do Game Boy e outra com os do Mega Duck
//      (vídeo, janela, objetos, DMA, LYC/STAT, timer, joypad e os 4 canais de som): as telas de todos os quadros
//      e todas as amostras de som têm de sair iguais nos dois núcleos;
//   2. início em $0000, sem cabeçalho;
//   3. bancos dos cartuchos MD1 ($B000, 32 KB), MD2 ($0001, 16 KB em $4000) e MD0 ($1000);
//   4. leitura de volta do LCDC e do NR32 com os bits do Duck; endereços vazios lêem $FF;
//   5. escolha pelo nome do arquivo (.duck/.md0/.md1/.md2), sem roubar .md nem .bin; save state determinístico.
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "../src/core/core.h"
#include "../src/gb/gb.h"

using namespace indigo;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

// ------------------------------------------------------------------ montador mínimo de SM83
struct Asm {
  std::vector<uint8_t>& rom;
  size_t pc;
  void b(std::initializer_list<int> v) { for (int x : v) rom[pc++] = static_cast<uint8_t>(x); }
  void ldA(int n) { b({0x3E, n}); }
  void ldhW(int r) { b({0xE0, r & 0xFF}); }        // LDH (r),A
  void ldhR(int r) { b({0xF0, r & 0xFF}); }        // LDH A,(r)
  void ldMemA(int a) { b({0xEA, a & 0xFF, a >> 8}); }
  void ldAMem(int a) { b({0xFA, a & 0xFF, a >> 8}); }
  void ldHL(int a) { b({0x21, a & 0xFF, a >> 8}); }
  void ldDE(int a) { b({0x11, a & 0xFF, a >> 8}); }
  void ldBC(int a) { b({0x01, a & 0xFF, a >> 8}); }
  void jp(int a) { b({0xC3, a & 0xFF, a >> 8}); }
  void jrBack(int op, size_t to) { b({op, static_cast<int>(static_cast<uint8_t>(int(to) - int(pc + 2)))}); }
  // escreve n no registrador (endereço já traduzido)
  void set(int r, int n) { ldA(n); ldhW(r); }
};

// endereços e bits de cada aparelho
struct Regs {
  bool duck;
  int LCDC, STAT, SCY, SCX, LY, LYC, DMA, BGP, OBP0, OBP1, WY, WX;
  int NR10, NR11, NR12, NR13, NR14, NR21, NR22, NR23, NR24, NR30, NR31, NR32, NR33, NR34, NR41, NR42, NR43, NR44,
      NR50, NR51, NR52;
  int lcdc(int gb) const {  // valor do LCDC nos bits do aparelho
    if (!duck) return gb;
    return (gb & 0xB0) | ((gb & 1) << 6) | ((gb >> 1) & 1) | ((gb >> 1) & 2) | ((gb >> 1) & 4) | ((gb >> 3) & 8);
  }
  int env(int gb) const { return duck ? (((gb << 4) | (gb >> 4)) & 0xFF) : gb; }  // NR12/22/42/43
  int nr32(int gb) const { return duck ? (gb ^ ((gb << 1) & 0x40)) : gb; }
};
static const Regs GBR = {false, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B,
                         0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E,
                         0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26};
static const Regs DKR = {true, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x14, 0x15, 0x16, 0x17,
                         0x20, 0x22, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2E, 0x2D,
                         0x40, 0x42, 0x41, 0x43, 0x44, 0x46, 0x45};

// programa de demonstração: ladrilhos, dois mapas, janela, 8 objetos (16 de altura) por DMA, troca da paleta na
// linha 64 (LYC), rolagem e troca do tamanho dos objetos na VBlank, timer, joypad e os 4 canais de som
static std::vector<uint8_t> demoRom(const Regs& R) {
  std::vector<uint8_t> rom(0x8000, 0xFF);
  Asm a{rom, 0};
  if (R.duck) { a.b({0x00}); a.jp(0x150); }  // mesmo número de ciclos até $150 nos dois
  else { a.pc = 0x100; a.b({0x00}); a.jp(0x150); rom[0x147] = 0; }
  // dados: 16 ladrilhos em $1000
  for (int t = 0; t < 16; t++)
    for (int y = 0; y < 8; y++) {
      rom[0x1000 + t * 16 + y * 2] = static_cast<uint8_t>((t * 37 + y * 11) ^ (y & 1 ? 0xAA : 0x55));
      rom[0x1000 + t * 16 + y * 2 + 1] = static_cast<uint8_t>((t * 0x11) ^ (0x81 >> (y & 3)) ^ (y * 3));
    }
  // objetos em $1100 (copiados para $C100 e daí para a OAM pelo DMA)
  for (int k = 0; k < 8; k++) {
    const int o = 0x1100 + k * 4;
    rom[o] = static_cast<uint8_t>(24 + k * 14); rom[o + 1] = static_cast<uint8_t>(12 + k * 17);
    rom[o + 2] = static_cast<uint8_t>(k * 2); rom[o + 3] = static_cast<uint8_t>((k & 1 ? 0x10 : 0) | (k & 2 ? 0x20 : 0) | (k & 4 ? 0x80 : 0));
  }
  // VBlank ($40) e LCD STAT ($48)
  a.pc = 0x40; a.jp(0x400);
  a.pc = 0x48; a.jp(0x480);
  a.pc = 0x50; a.jp(0x4C0);  // timer
  a.pc = 0x400;
  a.b({0xF5});  // PUSH AF
  a.ldhR(R.SCX); a.b({0x3C}); a.ldhW(R.SCX);  // SCX++
  a.ldhR(R.SCY); a.b({0x3D}); a.ldhW(R.SCY);  // SCY--
  a.set(R.BGP, 0xE4);
  a.ldAMem(0xC000); a.b({0x3C}); a.ldMemA(0xC000);
  a.b({0xE6, 0x3F});  // AND $3F
  { const size_t j = a.pc; a.b({0x20, 0}); // JR NZ, pula
    a.ldhR(R.LCDC); a.b({0xEE, R.lcdc(0x04)}); a.ldhW(R.LCDC);  // troca 8x8/8x16 lendo de volta o LCDC
    a.ldA(0x87); a.ldhW(R.NR14);  // reinicia o canal 1
    rom[j + 1] = static_cast<uint8_t>(a.pc - (j + 2)); }
  a.ldhR(0x00); a.ldMemA(0xC002);  // joypad
  a.b({0xF1, 0xD9});  // POP AF; RETI
  a.pc = 0x480;
  a.b({0xF5}); a.set(R.BGP, 0x1B); a.ldhR(R.LY); a.ldMemA(0xC001); a.b({0xF1, 0xD9});
  a.pc = 0x4C0;
  a.b({0xF5}); a.ldAMem(0xC003); a.b({0x3C}); a.ldMemA(0xC003); a.ldhR(R.WX); a.b({0x3C}); a.ldhW(R.WX); a.b({0xF1, 0xD9});

  // ---- início
  a.pc = 0x150;
  a.b({0xF3, 0x31, 0xFE, 0xFF});  // DI; LD SP,$FFFE
  a.set(R.LCDC, 0);
  a.ldHL(0x8000); a.ldDE(0x1000); a.b({0x06, 0x00});  // B = 256
  { const size_t l = a.pc; a.b({0x1A, 0x22, 0x13, 0x05}); a.jrBack(0x20, l); }
  // mapas: $9800-$9FFF com (L ^ H) & 15
  a.ldHL(0x9800);
  { const size_t l = a.pc; a.b({0x7D, 0xAC, 0xE6, 0x0F, 0x22, 0x7C, 0xFE, 0xA0}); a.jrBack(0x20, l); }
  // objetos: copia para $C100 e DMA a partir de $C1
  a.ldHL(0xC100); a.ldDE(0x1100); a.b({0x06, 0xA0});
  { const size_t l = a.pc; a.b({0x1A, 0x22, 0x13, 0x05}); a.jrBack(0x20, l); }
  a.set(R.DMA, 0xC1);
  a.b({0x06, 0x30});  // espera o DMA (160 ciclos)
  { const size_t l = a.pc; a.b({0x05}); a.jrBack(0x20, l); }
  a.set(R.BGP, 0xE4); a.set(R.OBP0, 0xD2); a.set(R.OBP1, 0x1B); a.set(R.WY, 0x50); a.set(R.WX, 0x60);
  a.set(R.SCX, 3); a.set(R.SCY, 5); a.set(R.LYC, 0x40); a.set(R.STAT, 0x40);
  // som
  a.set(R.NR52, 0x80); a.set(R.NR50, 0x77); a.set(R.NR51, 0xF3);
  a.set(R.NR10, 0x15); a.set(R.NR11, 0x80); a.set(R.NR12, R.env(0xF3)); a.set(R.NR13, 0x00); a.set(R.NR14, 0x87);
  a.set(R.NR21, 0x40); a.set(R.NR22, R.env(0xA7)); a.set(R.NR23, 0x50); a.set(R.NR24, 0x86);
  a.set(R.NR30, 0x00);
  for (int k = 0; k < 16; k++) a.set(0x30 + k, (k * 0x37 + 0x12) & 0xFF);
  a.set(R.NR30, 0x80); a.set(R.NR31, 0x00); a.set(R.NR32, R.nr32(0x20)); a.set(R.NR33, 0x20); a.set(R.NR34, 0x85);
  a.set(R.NR41, 0x00); a.set(R.NR42, R.env(0xC2)); a.set(R.NR43, R.env(0x35)); a.set(R.NR44, 0x80);
  // timer: 4096 Hz / 64
  a.set(0x06, 0xC0); a.set(0x07, 0x04);
  a.set(R.LCDC, R.lcdc(0xF7));
  a.set(0x0F, 0x00); a.set(0xFF, 0x07);  // IF = 0; IE = VBlank + STAT + timer
  a.set(0x00, 0x20);  // joypad: direcionais
  a.b({0xFB});  // EI
  { const size_t l = a.pc; a.b({0x76, 0x00}); a.jrBack(0x18, l); }  // HALT; JR
  return rom;
}

static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t k = 0; k < n; k++) { h ^= b[k]; h *= 1099511628211ull; }
  return h;
}

static void testMirror(const std::string& dir) {
  const auto gbRom = demoRom(GBR), dkRom = demoRom(DKR);
  gb::GBCore g(gbRom, nullptr, gb::Mode::Dmg), d(dkRom, nullptr, gb::Mode::Dmg, 1);
  check(d.system() == "duck" && g.system() == "gb", "sistema dos dois núcleos");
  g.setSampleRate(48000); d.setSampleRate(48000);
  int sameFrames = 0, sameAudio = 0, frames = 300;
  std::set<uint32_t> colors;
  double energy = 0;
  bool firstDiff = true;
  for (int f = 0; f < frames; f++) {
    const uint32_t k = (f / 40) & 1 ? (KEY_LEFT | KEY_UP) : KEY_RIGHT;
    g.setKeys(k); d.setKeys(k);
    g.runFrame(); d.runFrame();
    const bool same = std::memcmp(g.framebuffer(), d.framebuffer(), 160 * 144 * 4) == 0;
    if (same) sameFrames++;
    else if (firstDiff) { firstDiff = false; std::printf("  primeiro quadro diferente: %d\n", f); }
    if (f == 120) for (int i = 0; i < 160 * 144; i++) colors.insert(g.framebuffer()[i]);
    const size_t na = g.audioAvailable(), nb = d.audioAvailable();
    std::vector<float> l1(na), r1(na), l2(nb), r2(nb);
    g.pullAudio(l1.data(), r1.data(), na); d.pullAudio(l2.data(), r2.data(), nb);
    if (na == nb && l1 == l2 && r1 == r2) sameAudio++;
    for (float v : l1) energy += v * v;
  }
  check(sameFrames == frames, "telas iguais Game Boy x Mega Duck em todos os quadros (" + std::to_string(sameFrames) + "/" + std::to_string(frames) + ")");
  check(sameAudio == frames, "som igual Game Boy x Mega Duck em todos os quadros (" + std::to_string(sameAudio) + "/" + std::to_string(frames) + ")");
  check(colors.size() == 4, "a tela do programa usa os 4 tons (" + std::to_string(colors.size()) + ")");
  check(energy > 1.0, "o programa toca som");
  // contadores do programa: VBlank, LYC na linha 64, timer e joypad
  gb::GB& m = d.gb();
  check(m.read(0xC000) + 8 >= (frames & 0xFF) && m.read(0xC000) <= (frames & 0xFF), "VBlank a cada quadro no Duck (" + std::to_string(m.read(0xC000)) + ")");
  check(m.read(0xC001) == 0x40, "interrupção do LYC na linha 64 no Duck");
  check(m.read(0xC003) > 0, "interrupção do timer no Duck");
  check(m.read(0xC002) == g.gb().read(0xC002) && (m.read(0xC002) & 0x0F) != 0x0F, "joypad lido no Duck");
  std::printf("  %d quadros e o som iguais; tela de 4 tons; contadores VBlank=%d timer=%d\n", sameFrames, m.read(0xC000), m.read(0xC003));
  if (!dir.empty()) { std::ofstream(dir + "/demo.duck", std::ios::binary).write(reinterpret_cast<const char*>(dkRom.data()), dkRom.size()); }

  // save state: voltar a um estado e andar de novo dá as mesmas telas; salvar-carregar-salvar dá os mesmos bytes
  // (o som recomeça a contagem das amostras ao carregar, como no Game Boy)
  const auto st = d.saveState();
  uint64_t h1 = 0, h2 = 0;
  for (int f = 0; f < 30; f++) { d.runFrame(); h1 = fnv(d.framebuffer(), 160 * 144 * 4, h1); }
  d.loadState(st);
  for (int f = 0; f < 30; f++) { d.runFrame(); h2 = fnv(d.framebuffer(), 160 * 144 * 4, h2); }
  check(h1 == h2, "save state: as 30 telas seguintes se repetem no Duck");
  const auto st2 = d.saveState();
  d.loadState(st2);
  check(d.saveState() == st2, "save state: salvar, carregar e salvar dá o mesmo arquivo");
  bool threw = false;
  try { std::vector<uint8_t> bad(st.begin(), st.begin() + st.size() / 2); d.loadState(bad); } catch (...) { threw = true; }
  check(threw && d.saveState() == st2, "estado truncado recusado sem estragar o jogo");
  // ROM de lixo como Mega Duck (os dois mapeadores) e estados danificados não derrubam
  uint32_t x = 4242;
  std::vector<uint8_t> junk(0x20000);
  for (auto& v : junk) { x = x * 1103515245 + 12345; v = static_cast<uint8_t>(x >> 16); }
  for (const char* n : {"lixo.md2", "lixo.md0"}) {
    auto j = createCore(junk, n, nullptr);
    for (int f = 0; f < 60; f++) { j->setKeys(f * 37 & 0xFF); j->runFrame(); }
    const auto js = j->saveState();
    for (int t = 0; t < 100; t++) {
      std::vector<uint8_t> bad(js);
      for (int k = 0; k < 8; k++) { x = x * 1103515245 + 12345; bad[(x >> 8) % bad.size()] ^= static_cast<uint8_t>(x >> 3); }
      try { j->loadState(bad); } catch (...) {}
      j->runFrame();
    }
  }
  check(true, "ROM de lixo e estados danificados no Duck sem cair");
}

// cartuchos com bancos: cada banco de 16 KB tem o número dele em $x000 (início) e o programa grava o que lê
static std::vector<uint8_t> bankRom(int kind, size_t size) {
  std::vector<uint8_t> rom(size, 0x00);
  for (size_t b = 0; b < size / 0x4000; b++) { rom[b * 0x4000 + 0x3F00] = static_cast<uint8_t>(0xB0 + b); }
  // o código fica repetido no começo de cada bloco de 32 KB (o MD1/MD0 troca a metade de baixo também)
  for (size_t base = 0; base < size; base += 0x8000) {
    Asm a{rom, base};
    a.b({0x31, 0xFE, 0xFF});  // LD SP
    // código copiado para a WRAM ($C800) e executado de lá, já que a troca de 32 KB muda a ROM debaixo da CPU
    const size_t src = base + 0x100;
    a.ldHL(0xC800); a.ldDE(0x100); a.b({0x06, 0x80});
    { const size_t l = a.pc; a.b({0x1A, 0x22, 0x13, 0x05}); a.jrBack(0x20, l); }
    a.jp(0xC800);
    Asm c{rom, src};
    // grava em $C000+i o marcador visto em $7F00 depois de cada troca, e em $C010+i o de $3F00
    const int reg = kind == 2 ? 0x1000 : kind == 1 ? 0xB000 : 0x0001;
    const int n = kind == 3 ? 7 : (kind == 2 ? 4 : 2);
    for (int i = 0; i < n; i++) {
      const int v = kind == 3 ? i + 1 : i;
      c.ldA(v); c.ldMemA(reg);
      c.ldAMem(0x7F00); c.ldMemA(0xC000 + i);
      c.ldAMem(0x3F00); c.ldMemA(0xC010 + i);
    }
    c.b({0x76, 0x00, 0x18, 0xFC});
  }
  return rom;
}

static void testBanks(const std::string& dir) {
  struct Case { const char* name; int kind; size_t size; int coreKind; };
  for (const Case& cs : {Case{"md1.md1", 1, 0x10000, 1}, Case{"md0.md0", 2, 0x20000, 2}, Case{"md2.md2", 3, 0x20000, 1}}) {
    const auto rom = bankRom(cs.kind, cs.size);
    if (!dir.empty()) std::ofstream(dir + "/" + cs.name, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()), rom.size());
    auto core = createCore(rom, cs.name, nullptr);
    check(core && core->system() == "duck", std::string("detecta ") + cs.name);
    if (!core) continue;
    for (int f = 0; f < 3; f++) core->runFrame();
    auto& m = static_cast<gb::GBCore&>(*core).gb();
    bool ok = true;
    std::string got;
    const int n = cs.kind == 3 ? 7 : (cs.kind == 2 ? 4 : 2);
    for (int i = 0; i < n; i++) {
      const int v = cs.kind == 3 ? i + 1 : i;
      const int hi = cs.kind == 3 ? 0xB0 + v : 0xB0 + v * 2 + 1, lo = cs.kind == 3 ? 0xB0 : 0xB0 + v * 2;
      if (m.read(0xC000 + i) != hi || m.read(0xC010 + i) != lo) ok = false;
      char buf[16]; std::snprintf(buf, sizeof buf, "%02X/%02X ", m.read(0xC010 + i), m.read(0xC000 + i)); got += buf;
    }
    check(ok, std::string("bancos de ") + cs.name + ": " + got);
  }
}

static void testRegs() {
  // programa curto: escreve LCDC/NR32 com valores do Duck e lê de volta; lê endereços vazios
  std::vector<uint8_t> rom(0x8000, 0xFF);
  Asm a{rom, 0};
  a.b({0x31, 0xFE, 0xFF});
  a.set(0x10, 0x4B); a.ldhR(0x10); a.ldMemA(0xC000);   // LCDC
  a.set(0x2C, 0x60); a.ldhR(0x2C); a.ldMemA(0xC001);   // NR32 (100% no Duck)
  a.ldhR(0x1C); a.ldMemA(0xC002);                      // vazio
  a.ldhR(0x26); a.ldMemA(0xC003);                      // vazio (NR52 do Game Boy)
  a.ldhR(0x47); a.ldMemA(0xC004);                      // vazio (BGP do Game Boy)
  a.set(0x1B, 0x93); a.ldhR(0x1B); a.ldMemA(0xC005);   // BGP do Duck
  a.b({0x76, 0x00, 0x18, 0xFC});
  gb::GBCore d(rom, nullptr, gb::Mode::Dmg, 1);
  for (int f = 0; f < 3; f++) d.runFrame();  // desligar o LCD encerra o quadro na hora
  gb::GB& m = d.gb();
  check(m.read(0xC000) == 0x4B, "LCDC lido de volta com os bits do Duck");
  // 0x4B no Duck = bits 6 (BG), 3 (mapa da janela), 1 (objetos 8x16), 0 (objetos): no Game Boy, 0x01|0x40|0x04|0x02
  check(m.ppu.lcdc == 0x47, "LCDC traduzido para os bits do Game Boy (" + std::to_string(m.ppu.lcdc) + ")");
  check(m.read(0xC001) == (0x60 | 0x9F), "NR32 lido de volta no formato do Duck");
  check(m.read(0xC002) == 0xFF && m.read(0xC003) == 0xFF && m.read(0xC004) == 0xFF, "endereços vazios lêem $FF");
  check(m.read(0xC005) == 0x93 && m.ppu.bgp == 0x93, "BGP em $FF1B");
}

static void testDetect() {
  std::vector<uint8_t> rom(0x8000, 0);
  rom[0] = 0x18; rom[1] = 0xFE;
  for (const char* n : {"x.duck", "x.md1", "x.md2", "X.MD0"}) {
    std::string low = n; for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto c = createCore(rom, low, nullptr);
    check(c && c->system() == "duck", std::string("extensão ") + n);
  }
  auto md = createCore(rom, "x.md", nullptr);
  check(md && md->system() != "duck", ".md continua no Mega Drive");
  auto bin = createCore(rom, "x.bin", nullptr);
  check(!bin || bin->system() != "duck", ".bin não vira Mega Duck");
  // arquivo minúsculo não derruba
  std::vector<uint8_t> tiny(16, 0);
  check(createCore(tiny, "x.duck", nullptr) == nullptr, "arquivo minúsculo recusado");
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/duck";
  struct stat st {};
  if (stat(dir.c_str(), &st) != 0) dir.clear();
  std::printf("== espelho Game Boy x Mega Duck\n");
  testMirror(dir);
  std::printf("== bancos MD0/MD1/MD2\n");
  testBanks(dir);
  std::printf("== registradores\n");
  testRegs();
  std::printf("== detecção\n");
  testDetect();
  std::printf("RESULTADO duck: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
