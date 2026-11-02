// Testes do Watara Supervision (indigo::sv).
//   indigo-t7_svision [pasta]   pasta preparada por tools/t7_svision.sh (padrão /root/indigo-env/roms/svision);
//                               os programas de teste são montados aqui e gravados lá (.sv) para abrir no app
// Verificações:
//   1. bancos de 16 KB em $8000 (bits 5-7 de $2026) e o último fixo em $C000;
//   2. LCD: 4 pixels por byte (bits 0-1 à esquerda), 48 bytes por linha, rolagem em X (pixels) e Y (linhas),
//      largura em $2000, LCD desligado; tela conferida contra uma conta feita à parte;
//   3. DMA de vídeo ($2008-$200D), joypad ativo em 0, timer (IRQ, $2027, reconhecimento em $2024), NMI por quadro;
//   4. som: frequência e lado dos canais de onda quadrada, duração pelo contador de quadros, ruído só no lado
//      escolhido, DMA de áudio lendo o banco certo, com IRQ no fim;
//   5. ROM feita com o cc65 (crt0 do Supervision, linguagem C), quando tools/t7_svision.sh a preparou;
//   6. detecção (.sv; .ws continua no WonderSwan), save state determinístico e estado truncado recusado.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <sys/stat.h>

#include "../src/core/core.h"
#include "../src/svision/svision.h"

using namespace indigo;
using namespace indigo::sv;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}
static std::string hex(int v) { char b[8]; std::snprintf(b, sizeof b, "%02X", v); return b; }

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------ montador mínimo de 65C02
// o arquivo tem 'banks' bancos de 16 KB; o código fica no último (endereços $C000-$FFFF)
struct Asm {
  std::vector<uint8_t>& rom;
  size_t base;  // posição no arquivo do endereço $C000
  uint16_t pc = 0xC000;
  void b(std::initializer_list<int> v) { for (int x : v) rom[base + (pc++ - 0xC000)] = static_cast<uint8_t>(x); }
  void lda(int n) { b({0xA9, n}); }
  void ldaA(int a) { b({0xAD, a & 0xFF, a >> 8}); }
  void sta(int a) { b({0x8D, a & 0xFF, a >> 8}); }
  void inc(int a) { b({0xEE, a & 0xFF, a >> 8}); }
  void jmp(int a) { b({0x4C, a & 0xFF, a >> 8}); }
  void set(int a, int n) { lda(n); sta(a); }
  void bne(uint16_t to) { b({0xD0, static_cast<int>(static_cast<uint8_t>(int(to) - int(pc + 2)))}); }
  void vec(uint16_t addr, uint16_t to) { rom[base + (addr - 0xC000)] = to & 0xFF; rom[base + (addr - 0xC000) + 1] = to >> 8; }
};

static std::vector<uint8_t> newRom(int banks) {
  std::vector<uint8_t> rom(size_t(banks) * 0x4000, 0xEA);
  for (int k = 0; k < banks; k++) rom[size_t(k) * 0x4000 + 0x3000] = static_cast<uint8_t>(0xA0 + k);  // marcador em $x000+$3000
  return rom;
}

static uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t k = 0; k < n; k++) { h ^= b[k]; h *= 1099511628211ull; }
  return h;
}

static void save(const std::string& dir, const std::string& name, const std::vector<uint8_t>& rom) {
  if (!dir.empty()) std::ofstream(dir + "/" + name, std::ios::binary).write(reinterpret_cast<const char*>(rom.data()), rom.size());
}

// tela esperada, calculada direto da descrição do LCD
static std::vector<uint32_t> expectedScreen(const Supervision& m, const uint32_t* pal) {
  std::vector<uint32_t> out(160 * 160, pal[0]);
  if (!(m.reg[0x26] & 8)) return out;
  const int xpos = m.reg[2], ypos = m.reg[3];
  const int visible = std::min(160, (m.reg[0] | 3) - 3);
  for (int y = 0; y < 160; y++) {
    // a linha volta ao começo da VRAM quando passa de $1FE0 (como no MAME)
    int j = xpos / 4 + ypos * 48;
    for (int k = 0; k < y; k++) { j += 48; if (j >= 0x1FE0) j = 0; }
    const int start = j;
    for (int x = 0; x < visible; x++) {
      const int p = x + (xpos & 3);
      out[y * 160 + x] = pal[(m.vram[(start + p / 4) & 0x1FFF] >> ((p & 3) * 2)) & 3];
    }
  }
  return out;
}

// ------------------------------------------------------------------ 1-3: sistema
static std::vector<uint8_t> systemRom() {
  auto rom = newRom(4);
  Asm a{rom, 3 * 0x4000};
  // dados para o DMA de vídeo em $E000 (32 bytes)
  for (int k = 0; k < 32; k++) rom[3 * 0x4000 + 0x2000 + k] = static_cast<uint8_t>(k * 7 + 3);
  a.b({0x78, 0xA2, 0xFF, 0x9A, 0xD8});  // SEI; LDX #$FF; TXS; CLD
  for (int k = 0; k < 4; k++) { a.set(0x2026, (k << 5) | 8); a.ldaA(0xB000); a.sta(0x0300 + k); }
  a.ldaA(0xF000); a.sta(0x0310);
  // VRAM: byte i = (i & $FF) ^ (i >> 8), com ($00),Y
  a.set(0x00, 0x00); a.set(0x01, 0x40); a.b({0xA0, 0x00});
  { const uint16_t l = a.pc; a.b({0x98, 0x45, 0x01, 0x91, 0x00, 0xC8}); a.bne(l); a.b({0xE6, 0x01, 0xA5, 0x01, 0xC9, 0x60}); a.bne(l); }
  a.set(0x4000, 0xE4);  // primeiros 4 pixels: tons 0, 1, 2, 3
  a.set(0x2000, 0xA0); a.set(0x2002, 0); a.set(0x2003, 0);
  // DMA de vídeo: 32 bytes de $E000 para $5E00
  a.set(0x2008, 0x00); a.set(0x2009, 0xE0); a.set(0x200A, 0x00); a.set(0x200B, 0x5E); a.set(0x200C, 2); a.set(0x200D, 0x80);
  // timer: IRQ a cada 10 x 256 ciclos (rearmado na interrupção); NMI por quadro; LCD ligado
  a.set(0x2026, 0x0B); a.set(0x2023, 10);
  a.b({0x58});  // CLI
  { const uint16_t l = a.pc; a.ldaA(0x2020); a.sta(0x0320); a.jmp(l); }
  const uint16_t irq = a.pc;
  a.b({0x48}); a.ldaA(0x2027); a.sta(0x0321); a.ldaA(0x2024); a.inc(0x0322);
  { a.b({0xD0, 0x03}); a.inc(0x0324); }  // contador de 16 bits ($0322/$0324)
  a.set(0x2023, 10); a.b({0x68, 0x40});
  const uint16_t nmi = a.pc;
  a.inc(0x0323); a.b({0x40});
  a.vec(0xFFFA, nmi); a.vec(0xFFFC, 0xC000); a.vec(0xFFFE, irq);
  return rom;
}

static void testSystem(const std::string& dir) {
  const auto rom = systemRom();
  save(dir, "sistema.sv", rom);
  auto core = createCore(rom, "sistema.sv", nullptr);
  check(core && core->system() == "svision", "detecta .sv");
  if (!core) return;
  auto& sc = static_cast<SupervisionCore&>(*core);
  Supervision& m = sc.machine();
  core->setKeys(KEY_A | KEY_UP);
  for (int f = 0; f < 4; f++) core->runFrame();  // preencher a VRAM leva ~2 quadros
  std::string got;
  bool ok = true;
  for (int k = 0; k < 4; k++) { got += hex(m.ram[0x300 + k]) + " "; if (m.ram[0x300 + k] != 0xA0 + k) ok = false; }
  check(ok, "bancos em $8000: " + got);
  check(m.ram[0x310] == 0xA3, "último banco fixo em $C000");
  bool vr = true;
  for (int i = 1; i < 0x1E00; i++) if (m.vram[i] != ((i & 0xFF) ^ (0x40 + (i >> 8)))) { vr = false; break; }
  check(vr, "VRAM escrita pela CPU");
  bool dm = true;
  for (int k = 0; k < 32; k++) if (m.vram[0x1E00 + k] != static_cast<uint8_t>(k * 7 + 3)) dm = false;
  check(dm, "DMA de vídeo copiou 32 bytes da ROM para a VRAM");
  const uint32_t* fb = core->framebuffer();
  check(fb[0] != fb[1] && fb[1] != fb[2] && fb[2] != fb[3] && fb[0] != fb[3], "4 tons nos primeiros pixels ($E4)");
  uint32_t pal[4] = {fb[0], fb[1], fb[2], fb[3]};
  check(std::vector<uint32_t>(fb, fb + 160 * 160) == expectedScreen(m, pal), "tela igual à conta (sem rolagem)");
  check(m.ram[0x320] == 0xD7, "joypad ativo em 0 (A + cima = $D7): $" + hex(m.ram[0x320]));
  // rolagem e largura
  m.reg[2] = 6; m.reg[3] = 150; m.reg[0] = 0x90;
  core->runFrame();
  check(std::vector<uint32_t>(fb, fb + 160 * 160) == expectedScreen(m, pal), "tela igual à conta (rolagem 6/150, largura $90)");
  m.reg[0] = 0xA0; m.reg[2] = 0; m.reg[3] = 0;
  // timer e NMI: 10 quadros
  const int t0 = m.ram[0x322] | (m.ram[0x324] << 8), n0 = m.ram[0x323];
  for (int f = 0; f < 10; f++) core->runFrame();
  const int timers = (m.ram[0x322] | (m.ram[0x324] << 8)) - t0, nmis = m.ram[0x323] - n0;
  // cada disparo leva 2560 ciclos mais o tempo de rearmar (~30 ciclos): ~253 em 10 quadros de 65536 ciclos
  check(timers >= 245 && timers <= 256, "IRQ do timer: " + std::to_string(timers) + " em 10 quadros");
  check(nmis == 10, "NMI por quadro: " + std::to_string(nmis));
  check((m.ram[0x321] & 1) == 1, "$2027 mostra o timer na interrupção");
  // LCD desligado: tela no tom 0
  m.reg[0x26] &= ~8;
  core->runFrame();
  bool blank = true;
  for (int i = 0; i < 160 * 160; i++) if (fb[i] != pal[0]) { blank = false; break; }
  check(blank, "LCD desligado mostra a tela vazia");
  m.reg[0x26] |= 8;

  // save state
  const auto st = core->saveState();
  uint64_t h1 = 0, h2 = 0;
  std::vector<float> l(4096), r(4096);
  for (int f = 0; f < 20; f++) { core->runFrame(); h1 = fnv(fb, 160 * 160 * 4, h1); h1 = fnv(m.ram, sizeof m.ram, h1); }
  const auto st2 = core->saveState();
  core->loadState(st);
  for (int f = 0; f < 20; f++) { core->runFrame(); h2 = fnv(fb, 160 * 160 * 4, h2); h2 = fnv(m.ram, sizeof m.ram, h2); }
  check(h1 == h2 && core->saveState() == st2, "save state determinístico");
  bool threw = false;
  try { std::vector<uint8_t> bad(st2.begin(), st2.begin() + st2.size() / 2); core->loadState(bad); } catch (...) { threw = true; }
  check(threw && core->saveState() == st2, "estado truncado recusado sem mudar o jogo");
  threw = false;
  try { std::vector<uint8_t> bad(st2); bad[0] = 'X'; core->loadState(bad); } catch (...) { threw = true; }
  check(threw, "estado de outro console recusado");
  // bytes trocados ao acaso: carrega (com limites) ou recusa, mas o jogo continua rodando
  uint32_t x = 777;
  for (int t = 0; t < 200; t++) {
    std::vector<uint8_t> bad(st2);
    for (int k = 0; k < 8; k++) { x = x * 1103515245 + 12345; bad[4 + (x >> 8) % (bad.size() - 4)] ^= static_cast<uint8_t>(x >> 3); }
    try { core->loadState(bad); } catch (...) {}
    core->runFrame();
  }
  check(true, "200 estados danificados sem cair");
}

// ------------------------------------------------------------------ 4: som
static int crossings(const std::vector<float>& v) {
  int n = 0;
  double mean = 0;
  for (float x : v) mean += x;
  mean /= std::max<size_t>(1, v.size());
  for (size_t i = 1; i < v.size(); i++) if (v[i - 1] < mean && v[i] >= mean) n++;
  return n;
}
static double rms(const std::vector<float>& v, size_t from, size_t to) {
  double s = 0, m = 0;
  to = std::min(to, v.size());
  if (to <= from) return 0;
  for (size_t i = from; i < to; i++) m += v[i];
  m /= double(to - from);
  for (size_t i = from; i < to; i++) s += (v[i] - m) * (v[i] - m);
  return std::sqrt(s / double(to - from));
}

struct Capture { std::vector<float> l, r; };
static Capture runAudio(const std::vector<uint8_t>& rom, int frames, Supervision** out = nullptr, std::unique_ptr<Core>* keep = nullptr) {
  auto core = createCore(rom, "som.sv", nullptr);
  core->setSampleRate(48000);
  Capture c;
  for (int f = 0; f < frames; f++) {
    core->runFrame();
    const size_t n = core->audioAvailable();
    std::vector<float> l(n), r(n);
    core->pullAudio(l.data(), r.data(), n);
    c.l.insert(c.l.end(), l.begin(), l.end());
    c.r.insert(c.r.end(), r.begin(), r.end());
  }
  if (out) *out = &static_cast<SupervisionCore&>(*core).machine();
  if (keep) *keep = std::move(core);
  return c;
}

static std::vector<uint8_t> soundRom(int which) {
  auto rom = newRom(2);
  Asm a{rom, 0x4000};
  a.b({0x78, 0xA2, 0xFF, 0x9A, 0xD8});
  a.set(0x2026, 0x08);
  uint16_t irq = 0;
  if (which == 0) {  // canal 1 (direita): 125000 / 125 = 1000 Hz, contínuo, 50%, volume 15
    a.set(0x2010, 125); a.set(0x2011, 0); a.set(0x2012, 0x6F); a.set(0x2013, 0);
  } else if (which == 1) {  // canal 2 (esquerda): 500 Hz por 10 quadros (contador 9)
    a.set(0x2014, 250); a.set(0x2015, 0); a.set(0x2016, 0x2F); a.set(0x2017, 9);
  } else if (which == 2) {  // ruído contínuo só à esquerda
    a.set(0x2028, 0x2F); a.set(0x2029, 0); a.set(0x202A, 0x1A);
  } else {  // DMA de áudio: 4 x 32 amostras de 4 bits do banco 0 (em $8000), à direita, com IRQ no fim
    for (int k = 0; k < 64; k++) rom[0x100 + k] = 0xF0;  // F, 0, F, 0...
    a.set(0x2026, 0x0C);
    a.set(0x2018, 0x00); a.set(0x2019, 0x81); a.set(0x201A, 4); a.set(0x201B, 0x04); a.set(0x201C, 0x80);
    a.b({0x58});
  }
  { const uint16_t l = a.pc; a.jmp(l); }
  irq = a.pc;
  a.b({0x48}); a.ldaA(0x2027); a.sta(0x0330); a.ldaA(0x2025); a.inc(0x0331); a.b({0x68, 0x40});
  a.vec(0xFFFA, irq); a.vec(0xFFFC, 0xC000); a.vec(0xFFFE, irq);
  return rom;
}

static void testSound(const std::string& dir) {
  {
    const auto rom = soundRom(0);
    save(dir, "som_tom.sv", rom);
    const Capture c = runAudio(rom, 62);
    const size_t sec = 48000;
    const std::vector<float> r(c.r.begin() + 1000, c.r.begin() + std::min(c.r.size(), 1000 + sec));
    const int hz = crossings(r);
    check(hz >= 990 && hz <= 1010, "canal 1 a 1000 Hz: " + std::to_string(hz));
    check(rms(c.r, 1000, 1000 + sec) > 0.05 && rms(c.l, 1000, 1000 + sec) < 1e-3, "canal 1 só à direita");
  }
  {
    const auto rom = soundRom(1);
    save(dir, "som_curto.sv", rom);
    const Capture c = runAudio(rom, 30);
    const size_t perFrame = 786;  // 48000 x 65536 / 4000000
    check(rms(c.l, perFrame * 2, perFrame * 8) > 0.05, "canal 2 toca à esquerda (" + std::to_string(rms(c.l, perFrame * 2, perFrame * 8)) + " " + std::to_string(c.l.size()) + ")");
    const std::vector<float> l(c.l.begin() + perFrame * 2, c.l.begin() + perFrame * 8);
    const double hz = crossings(l) * 48000.0 / l.size();
    check(hz > 480 && hz < 520, "canal 2 a 500 Hz: " + std::to_string(int(hz)));
    check(rms(c.l, perFrame * 13, perFrame * 30) < 1e-3, "canal 2 para depois de 10 quadros");
    check(rms(c.r, 0, c.r.size()) < 1e-3, "canal 2 não toca à direita");
  }
  {
    const auto rom = soundRom(2);
    save(dir, "som_ruido.sv", rom);
    const Capture c = runAudio(rom, 20);
    check(rms(c.l, 2000, c.l.size()) > 0.02 && rms(c.r, 0, c.r.size()) < 1e-3, "ruído só à esquerda");
    std::vector<float> l(c.l.begin() + 2000, c.l.end());
    const int cr = crossings(l);
    check(cr > 100, "ruído muda de nível (" + std::to_string(cr) + " subidas)");
  }
  {
    const auto rom = soundRom(3);
    save(dir, "som_dma.sv", rom);
    Supervision* m = nullptr;
    std::unique_ptr<Core> keep;
    const Capture c = runAudio(rom, 3, &m, &keep);
    // 128 amostras a 4 MHz / 256 = 15625 por segundo: 8,2 ms (~393 amostras de saída), alternando F e 0
    check(m->ram[0x331] == 1, "IRQ no fim do DMA de áudio (" + std::to_string(m->ram[0x331]) + ")");
    check((m->ram[0x330] & 2) == 2, "$2027 mostra o DMA");
    check(rms(c.r, 20, 380) > 0.05 && rms(c.l, 0, c.l.size()) < 1e-3, "DMA de áudio toca à direita");
    double jump = 0;  // depois do fim só sobra a volta lenta do filtro de componente contínua
    for (size_t i = 501; i < c.r.size(); i++) jump = std::max(jump, double(std::fabs(c.r[i] - c.r[i - 1])));
    check(jump < 1e-3, "DMA de áudio para no fim");
    std::vector<float> r(c.r.begin() + 20, c.r.begin() + 380);
    const double hz = crossings(r) * 48000.0 / r.size();
    check(hz > 7300 && hz < 8300, "DMA a 15625 amostras/s (onda de " + std::to_string(int(hz)) + " Hz)");
  }
}

// ------------------------------------------------------------------ 5: ROM do cc65
static void testCc65(const std::string& dir) {
  const auto rom = readAll(dir + "/cc65demo.sv");
  if (rom.empty()) { std::printf("  (sem cc65demo.sv: rode tools/t7_svision.sh)\n"); return; }
  auto core = createCore(rom, "cc65demo.sv", nullptr);
  check(core && core->system() == "svision", "ROM do cc65 detectada");
  if (!core) return;
  Supervision& m = static_cast<SupervisionCore&>(*core).machine();
  for (int f = 0; f < 30; f++) core->runFrame();
  // o programa em C desenha faixas na VRAM e conta NMIs (sv_nmi_counter, do crt0) e quadros em $1C00/$1C01
  const uint32_t* fb = core->framebuffer();
  uint32_t pal[4];
  for (int k = 0; k < 4; k++) pal[k] = fb[k * 40 * 160];  // faixas horizontais de 40 linhas com os tons 0-3
  check(pal[0] != pal[1] && pal[1] != pal[2] && pal[2] != pal[3], "faixas com os 4 tons");
  check(std::vector<uint32_t>(fb, fb + 160 * 160) == expectedScreen(m, pal), "tela do programa em C igual à conta");
  check(m.ram[0x1C00] == 0x5A, "programa em C chegou ao laço principal");
  const int n0 = m.ram[0x1C01];
  core->setKeys(KEY_START);
  for (int f = 0; f < 10; f++) core->runFrame();
  const int n1 = m.ram[0x1C01];
  check(((n1 - n0) & 0xFF) >= 9 && ((n1 - n0) & 0xFF) <= 11, "laço do C sincronizado pela NMI do crt0 (" + std::to_string((n1 - n0) & 0xFF) + " em 10 quadros)");
  check(m.ram[0x1C02] == 0x80, "joy_read do cc65 viu o START");
}

// ------------------------------------------------------------------ 6: detecção
static void testDetect() {
  auto rom = newRom(2);
  Asm a{rom, 0x4000};
  { const uint16_t l = a.pc; a.jmp(l); }
  a.vec(0xFFFC, 0xC000);
  auto c = createCore(rom, "x.sv", nullptr);
  check(c && c->system() == "svision", ".sv vira Supervision");
  auto b = createCore(rom, "x.bin", nullptr);
  check(!b || b->system() != "svision", ".bin não vira Supervision");
  std::vector<uint8_t> ws(0x10000, 0);
  auto w = createCore(ws, "x.ws", nullptr);
  check(!w || w->system() != "svision", ".ws não vira Supervision");
  std::vector<uint8_t> tiny(8, 0);
  check(createCore(tiny, "x.sv", nullptr) == nullptr, "arquivo minúsculo recusado");
  // ROM de 8 KB (menor que um banco) e lixo aleatório não derrubam
  std::vector<uint8_t> small(0x2000, 0x00);
  small[0x1FFC] = 0x00; small[0x1FFD] = 0xE0;
  auto s = createCore(small, "p.sv", nullptr);
  if (s) for (int f = 0; f < 5; f++) s->runFrame();
  check(s != nullptr, "ROM de 8 KB espelhada");
  uint32_t x = 12345;
  std::vector<uint8_t> junk(0x8000);
  for (auto& v : junk) { x = x * 1103515245 + 12345; v = static_cast<uint8_t>(x >> 16); }
  auto j = createCore(junk, "j.sv", nullptr);
  for (int f = 0; f < 60; f++) j->runFrame();
  check(true, "ROM de lixo roda 60 quadros sem cair");
}

int main(int argc, char** argv) {
  std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/svision";
  struct stat st {};
  if (stat(dir.c_str(), &st) != 0) dir.clear();
  std::printf("== sistema (bancos, LCD, DMA de vídeo, joypad, timer, NMI, save state)\n");
  testSystem(dir);
  std::printf("== som (tons, ruído, DMA de áudio)\n");
  testSound(dir);
  std::printf("== ROM do cc65\n");
  if (!dir.empty()) testCc65(dir);
  std::printf("== detecção\n");
  testDetect();
  std::printf("RESULTADO svision: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
