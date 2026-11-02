// Testes do núcleo Game & Watch (indigo::gnw).
//   indigo-t7_gnw [pasta]   pasta preparada por tools/t7_gnw.sh (padrão /root/indigo-env/roms/gnw); o teste
//                           grava lá os pacotes .zip que monta e a imagem da tela de cada um (PPM).
// Nenhum jogo nem desenho de tela de verdade: os programas das CPUs são escritos aqui mesmo (um montador
// mínimo que segue o contador de programa em LFSR) e os desenhos .svg são grades de retângulos com títulos.
// Verificações:
//   1. SM510: LAX seguidos, ADX com e sem pulo, ADD11/ROT, TM (chamada indireta) + RTN1, SBM, pilha de 2
//      níveis, CEND acordado pelo sinal de 1 s e pela tecla, contador de segundos, teclas pela saída S, LCD
//      (RAM 0x60/0x70 e L -> segmentos a/b/bs), som do piezo (R1 = divisor de 4096 Hz);
//   2. SM5A: ADX, bancos de RAM espelhados, INCB (pula no 8), TRS + RTN1, registradores W'/W do LCD, teclas
//      pela saída R, pino BA, contador TG;
//   3. SM511: melodia da ROM (PRE/SME/TMEL), troca de relógio 8/16 kHz, PTW, KTA;
//   4. pacotes da lista do MAME (com programa e desenhos próprios): Donkey Kong (SM510, duas telas) e Ball
//      (SM5A com linha de teclas fixa e BA/B ativos em 0);
//   5. tela: pixels dos segmentos acesos e apagados; detecção do .zip; save state determinístico, estado de
//      outro jogo e estado truncado; .svg e .zip danificados não derrubam o núcleo;
//   6. régua (vetores.txt, gerado por t7_gnw.sh): milhares de programas aleatórios para SM5A, SM510, SM511 e
//      SM512 rodados ciclo a ciclo no núcleo do MAME (compilado fora do repositório) e no do Índigo: escritas
//      em S e R, LCD a cada 1 ms, registros internos e RAM têm de ser iguais.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/gnw/gnw.h"

using namespace indigo;
using namespace indigo::gnw;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

// ------------------------------------------------------------------ .zip sem compressão
static uint32_t crc32(const std::vector<uint8_t>& d) {
  uint32_t c = 0xFFFFFFFF;
  for (uint8_t b : d) {
    c ^= b;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
static void put16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(v & 0xFF); o.push_back(v >> 8 & 0xFF); }
static void put32(std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); }
static std::vector<uint8_t> makeZip(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
  std::vector<uint8_t> o, cd;
  for (const auto& [name, data] : files) {
    const uint32_t off = static_cast<uint32_t>(o.size()), crc = crc32(data), n = static_cast<uint32_t>(data.size());
    put32(o, 0x04034B50); put16(o, 20); put16(o, 0); put16(o, 0); put16(o, 0); put16(o, 0);
    put32(o, crc); put32(o, n); put32(o, n); put16(o, static_cast<uint32_t>(name.size())); put16(o, 0);
    o.insert(o.end(), name.begin(), name.end());
    o.insert(o.end(), data.begin(), data.end());
    put32(cd, 0x02014B50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0);
    put32(cd, crc); put32(cd, n); put32(cd, n); put16(cd, static_cast<uint32_t>(name.size())); put16(cd, 0); put16(cd, 0);
    put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
    cd.insert(cd.end(), name.begin(), name.end());
  }
  const uint32_t cdOff = static_cast<uint32_t>(o.size());
  o.insert(o.end(), cd.begin(), cd.end());
  put32(o, 0x06054B50); put16(o, 0); put16(o, 0); put16(o, static_cast<uint32_t>(files.size()));
  put16(o, static_cast<uint32_t>(files.size())); put32(o, static_cast<uint32_t>(cd.size())); put32(o, cdOff); put16(o, 0);
  return o;
}

// ------------------------------------------------------------------ desenho de tela de teste
// grade de retângulos pretos com <title>x.y.z</title> sobre um fundo branco
struct Grade {
  int nx, ny, nz;  // grupos, segmentos, comuns
  int w() const { return ny * 24 + 20; }
  int h() const { return nx * nz * 17 + 20; }
  // centro do retângulo (unidades do desenho)
  double cx(int, int y, int) const { return 10 + y * 24 + 10; }
  double cy(int x, int, int z) const { return 10 + (x * nz + z) * 17 + 7; }
};
static std::vector<uint8_t> makeSvg(const Grade& g, bool fundo = true) {
  std::string s = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(g.w()) +
                  "\" height=\"" + std::to_string(g.h()) + "\">\n";
  if (fundo)
    s += "<rect x=\"0\" y=\"0\" width=\"" + std::to_string(g.w()) + "\" height=\"" + std::to_string(g.h()) + "\" fill=\"#ffffff\"/>\n";
  for (int x = 0; x < g.nx; x++)
    for (int z = 0; z < g.nz; z++)
      for (int y = 0; y < g.ny; y++) {
        char b[256];
        // metade dos segmentos como <path>, metade como <rect> (os dois aparecem nos desenhos do MAME)
        const int px = 10 + y * 24, py = 10 + (x * g.nz + z) * 17;
        if ((x + y + z) & 1)
          std::snprintf(b, sizeof b, "<g><title>%d.%d.%d</title><path d=\"M%d %dh20v14h-20z\" fill=\"#000000\"/></g>\n", x, y, z, px, py);
        else
          std::snprintf(b, sizeof b, "<g><title>%d.%d.%d</title><rect x=\"%d\" y=\"%d\" width=\"20\" height=\"14\" style=\"fill:#000\"/></g>\n", x, y, z, px, py);
        s += b;
      }
  s += "</svg>\n";
  return std::vector<uint8_t>(s.begin(), s.end());
}

// ------------------------------------------------------------------ montador mínimo
// PL (6 bits de baixo do PC) é um LFSR: a n-ésima instrução de uma página fica em seq[n].
struct Asm {
  enum Tipo { SM510, SM511, SM5A } tipo;
  std::vector<uint8_t> rom;
  int page = 0, idx = 0;
  int seq[64];
  std::map<std::string, int> labels;
  std::vector<std::function<void()>> fix;
  explicit Asm(Tipo t) : tipo(t), rom(t == SM5A ? 0x740 : 0x1000, 0) {
    int pc = 0;
    for (int i = 0; i < 64; i++) {
      seq[i] = pc;
      const int feed = ((pc >> 1 ^ pc) & 1) ? 0 : 0x20;
      pc = feed | (pc >> 1 & 0x1F);
    }
  }
  void org(int p) { page = p; idx = 0; }
  int here() const { return page << 6 | seq[idx]; }
  void b(uint8_t v) {
    if (idx >= 63) throw std::runtime_error("página cheia");
    rom.at(here()) = v;
    idx++;
  }
  void label(const std::string& n) { labels[n] = here(); }
  int at(const std::string& n) const { auto it = labels.find(n); if (it == labels.end()) throw std::runtime_error("rótulo " + n); return it->second; }
  // salto curto na mesma página (T no SM51x; TR no SM5A)
  void t(const std::string& n) {
    const int a = here(), pg = page;
    b(0x80);
    fix.push_back([=, this] { const int d = at(n); if ((d >> 6) != pg) throw std::runtime_error("T fora da página: " + n); rom[a] = 0x80 | (d & 0x3F); });
  }
  void tl(const std::string& n) {  // SM51x: salto longo
    const int a = here(); b(0x70); const int a2 = here(); b(0);
    fix.push_back([=, this] { const int d = at(n); rom[a] = 0x70 | (d >> 6 & 0xF); rom[a2] = static_cast<uint8_t>((d >> 10 & 3) << 6 | (d & 0x3F)); });
  }
  void tml(const std::string& n) {  // SM51x: chamada longa (PM até 3)
    const int a = here(); b(tipo == SM511 ? 0x68 : 0x7C); const int a2 = here(); b(0);
    const uint8_t base = tipo == SM511 ? 0x68 : 0x7C;
    fix.push_back([=, this] { const int d = at(n); if ((d >> 6 & 0xF) > 3) throw std::runtime_error("TML longe"); rom[a] = base | (d >> 6 & 3); rom[a2] = static_cast<uint8_t>((d >> 10 & 3) << 6 | (d & 0x3F)); });
  }
  void lbl(uint8_t v) { b(0x5F); b(v); }
  void setW(uint8_t v) { for (int i = 7; i >= 0; i--) b((v >> i & 1) ? 0x63 : 0x62); }
  void done() { for (auto& f : fix) f(); }
};

// ------------------------------------------------------------------ programas
// SM510: testes de instrução na partida, depois o laço do relógio com CEND (ver o cabeçalho)
static std::vector<uint8_t> progSm510() {
  Asm a(Asm::SM510);
  // TM 0x10: ponteiro na página 0 -> sub-rotina em PU=0, PM=4 (0x100 + PL)
  a.org(0x04);  // 0x100
  a.label("tmsub");
  a.b(0x26);  // LAX 6
  a.b(0x6F);  // RTN1
  a.rom[0x10] = static_cast<uint8_t>(0 << 6 | (a.labels["tmsub"] & 0x3F));
  // sub-rotinas da pilha: 0x080 chama 0x0C0
  a.org(0x03);  // 0x0C0
  a.label("sub2");
  a.b(0x21);  // LAX 1
  a.b(0x6E);  // RTN0
  a.org(0x02);  // 0x080
  a.label("sub1");
  a.tml("sub2");
  a.b(0x27);  // LAX 7 (carrega: a anterior é TML)
  a.lbl(0x06);
  a.b(0x10);  // EXC 0
  a.b(0x6E);
  // contador de segundos na RAM 0x10
  a.org(0x01);  // 0x040
  a.label("inc");
  a.lbl(0x10);
  a.b(0x18);  // LDA 0
  a.b(0x31);  // ADX 1
  a.b(0x00);  // SKIP (absorve o pulo do vai-um)
  a.b(0x10);  // EXC 0
  a.b(0x6E);  // RTN0

  // partida em 0xDC0
  a.org(0x37);
  a.b(0x25); a.b(0x27);  // LAX 5; LAX 7 (ignorado)
  a.lbl(0x00); a.b(0x10);  // RAM0 = 5
  a.b(0x29); a.b(0x39);  // LAX 9; ADX 9 -> 2, vai-um: pula
  a.b(0x20);  // LAX 0 (pulado)
  a.lbl(0x01); a.b(0x10);  // RAM1 = 2
  a.b(0x28); a.b(0x3A);  // LAX 8; ADX 10 -> 2 sem pulo
  a.b(0x2F);  // LAX 15 (executa)
  a.lbl(0x02); a.b(0x10);  // RAM2 = 15
  a.b(0x67); a.b(0x29); a.lbl(0x00);  // SC; LAX 9
  a.b(0x09);  // ADD11 -> 9+5+1 = 15, sem vai-um
  a.b(0x6B); a.b(0x6B);  // ROT; ROT -> 0xB
  a.lbl(0x03); a.b(0x10);  // RAM3 = 0xB
  a.b(0xC0 | 0x10);  // TM 0x10 -> LAX 6; RTN1
  a.b(0x2E);  // LAX 14 (pulado pelo RTN1)
  a.lbl(0x04); a.b(0x10);  // RAM4 = 6
  a.lbl(0x05); a.b(0x2A); a.b(0x02); a.b(0x10);  // LAX 10; SBM; EXC 0 -> RAM 0x45 = 10
  a.tml("sub1");  // RAM6 = 7 (pilha de 2 níveis)
  a.lbl(0x60); a.b(0x0C);  // SM 0: segmento 0.0.0
  a.b(0x21); a.b(0x59);  // LAX 1; ATL -> bs 2.0.0
  a.b(0x65);  // IDIV
  a.setW(0x03);
  a.tl("main");
  a.label("main0");

  // laço principal em 0x400 (vetor de despertar)
  a.org(0x10);
  a.label("main");
  a.b(0x58);  // TIS: pula se não passou 1 s
  a.tml("inc");
  a.setW(0x01);
  a.b(0x6A);  // KTA (linha 1)
  a.lbl(0x70); a.b(0x10);  // RAM 0x70 = K -> segmentos 1.0.z
  a.setW(0x02);
  a.b(0x6A);  // KTA (linha 2)
  a.b(0x5A);  // TA0
  a.t("r1");
  a.b(0x20);  // LAX 0
  a.t("atr");
  a.label("r1");
  a.b(0x21);  // LAX 1
  a.label("atr");
  a.b(0x61);  // ATR
  a.setW(0x03);  // linhas 1 e 2 acordam a CPU
  a.b(0x5D);  // CEND
  a.done();
  return a.rom;
}

// SM5A: partida em 0x3C0 (página 0xF); TRS chama 0x400 + x
static std::vector<uint8_t> progSm5a() {
  Asm a(Asm::SM5A);
  a.org(0x10);  // 0x400
  a.label("trs");
  a.b(0x23);  // LAX 3
  a.b(0x6F);  // RTN1
  a.org(0x0F);
  a.b(0x25); a.lbl(0x00); a.b(0x10);  // RAM0 = 5
  a.b(0x29); a.lbl(0x4E); a.b(0x10);  // LAX 9 -> RAM 0x4C (D-F leem C)
  a.lbl(0x7C); a.b(0x18);  // LDA (banco 7 = banco 4)
  a.lbl(0x01); a.b(0x10);  // RAM1 = 9
  a.b(0x24); a.lbl(0x07); a.b(0x64);  // LAX 4; INCB -> BL = 8: pula
  a.b(0x20);  // LAX 0 (pulado)
  a.lbl(0x02); a.b(0x10);  // RAM2 = 4
  a.b(0xC0 | (a.labels["trs"] & 0x3F));  // TRS -> LAX 3; RTN1
  a.b(0x2E);  // pulado
  a.lbl(0x03); a.b(0x10);  // RAM3 = 3
  a.b(0x2F); a.b(0x63); a.b(0x5C);  // LAX 15; WS; TW -> W[8] = 15
  a.b(0x2D); a.b(0x01);  // LAX 13; ATR -> R = 0b0010: linha 1 das teclas, piezo parado
  a.label("loop");
  a.b(0x58);  // TG: pula se não passou 1 s
  a.t("inc");
  a.t("teclas");
  a.label("inc");
  a.lbl(0x12); a.b(0x18); a.b(0x31); a.b(0x00); a.b(0x10);  // RAM 0x12 += 1
  a.label("teclas");
  a.b(0x6A);  // KTA
  a.lbl(0x10); a.b(0x10); a.b(0x18);  // RAM 0x10 = K (e de volta no ACC)
  a.b(0x62); a.b(0x5C);  // WR; TW -> W[8] = K & 7
  a.b(0x20); a.b(0x50); a.b(0x21);  // LAX 0; TA (pula se BA = 1); LAX 1
  a.lbl(0x11); a.b(0x10);  // RAM 0x11 = BA apertado
  a.t("loop");
  a.done();
  return a.rom;
}

// SM511: melodia, troca de relógio e teclas pelo PTW
static std::vector<uint8_t> progSm511() {
  Asm a(Asm::SM511);
  a.org(0x37);
  a.b(0x61); a.b(0x00);  // PRE 0
  a.b(0x60); a.b(0x31);  // SME
  a.label("espera");
  a.b(0x60); a.b(0x32);  // TMEL: pula quando a melodia acabou
  a.t("espera");
  a.b(0x21); a.lbl(0x00); a.b(0x10);  // RAM0 = 1
  a.b(0x60); a.b(0x30);  // RME
  a.b(0x60); a.b(0x36);  // 16 kHz
  a.label("teclas");
  a.setW(0x04);
  a.b(0x6D);  // PTW -> linha 3
  a.b(0x50);  // KTA
  a.lbl(0x71); a.b(0x10);  // RAM 0x71
  a.t("teclas");
  a.done();
  return a.rom;
}

// ------------------------------------------------------------------ ajudantes
static uint32_t hashFb(const Core& c, uint32_t h = 0x811C9DC5u) {
  const uint32_t* p = c.framebuffer();
  for (int i = 0; i < c.width() * c.height(); i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}
static int brilho(const Core& c, double x, double y) {
  const int ix = static_cast<int>(x), iy = static_cast<int>(y);
  if (ix < 0 || iy < 0 || ix >= c.width() || iy >= c.height()) return -1;
  const uint32_t p = c.framebuffer()[iy * c.width() + ix];
  return static_cast<int>((p & 0xFF) + (p >> 8 & 0xFF) + (p >> 16 & 0xFF)) / 3;
}
static double energia(Core& c) {
  std::vector<float> l(c.audioAvailable()), r(l.size());
  c.pullAudio(l.data(), r.data(), l.size());
  double e = 0;
  for (float v : l) e += v * v;
  return l.empty() ? 0 : e / l.size();
}
static void writePpm(const std::string& path, const Core& c) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%d %d\n255\n", c.width(), c.height());
  for (int i = 0; i < c.width() * c.height(); i++) {
    const uint32_t p = c.framebuffer()[i];
    const uint8_t px[3] = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16)};
    std::fwrite(px, 1, 3, f);
  }
  std::fclose(f);
}
static void saveFile(const std::string& p, const std::vector<uint8_t>& d) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}
static void frames(Core& c, int n, uint32_t keys = 0) {
  c.setKeys(keys);
  for (int i = 0; i < n; i++) c.runFrame();
}
static GnwCore& G(Core& c) { return dynamic_cast<GnwCore&>(c); }

// save state: o mesmo trecho de jogo depois de carregar o estado dá a mesma tela e o mesmo som
static void testState(Core& c, const std::string& nome) {
  frames(c, 30);
  const auto st = c.saveState();
  auto trecho = [&]() {
    uint32_t h = 0x811C9DC5u;
    for (int f = 0; f < 120; f++) {
      c.setKeys((f / 10) % 3 == 1 ? (KEY_START | KEY_RIGHT) : (f / 10) % 3 == 2 ? uint32_t(KEY_A) : 0u);
      c.runFrame();
      h = hashFb(c, h);
      std::vector<float> l(c.audioAvailable()), r(l.size());
      c.pullAudio(l.data(), r.data(), l.size());
      for (float v : l) { uint32_t u; std::memcpy(&u, &v, 4); h = (h ^ u) * 0x01000193u; }
    }
    return h;
  };
  energia(c);
  const uint32_t h1 = trecho();
  c.loadState(st);
  energia(c);
  const uint32_t h2 = trecho();
  check(h1 == h2, nome + ": save state determinístico");
  bool lancou = false;
  try { c.loadState(std::vector<uint8_t>(st.begin(), st.begin() + st.size() / 2)); } catch (const std::exception&) { lancou = true; }
  check(lancou, nome + ": estado truncado é recusado");
  // estado danificado (bytes trocados) não pode travar nem sair da memória
  std::mt19937 rng(7);
  for (int k = 0; k < 50; k++) {
    auto ruim = st;
    for (int j = 0; j < 20; j++) ruim[8 + rng() % (ruim.size() - 8)] = static_cast<uint8_t>(rng());
    try { c.loadState(ruim); frames(c, 3); } catch (const std::exception&) {}
  }
  c.loadState(st);
}

// ------------------------------------------------------------------ régua: núcleo do MAME com programas aleatórios
// vetores.txt (gerado por t7_gnw.sh com o núcleo SM510 do MAME compilado fora do repositório): cada linha é um
// programa aleatório (semente) rodado N ciclos do oscilador, com reset (ACL) a cada 8192 ciclos. Confere os hashes
// das escritas em S e R (com o ciclo), do LCD a cada 1 ms, os registros internos e a RAM no fim.
struct Xs { uint32_t s; uint32_t operator()() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; } };
static uint32_t fnv4(uint32_t h, uint32_t v) { for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFF; h *= 0x01000193u; } return h; }

struct IoRegua : SmIo {
  uint32_t t = 0, lastS = 0, lastR = 0, hS = 0x811C9DC5u, hR = 0x811C9DC5u;
  bool log = false;
  uint8_t readK() override { return static_cast<uint8_t>(((lastS * 7) ^ (lastR * 13) ^ (t >> 10)) & 0xF); }
  int readBA() override { return static_cast<int>(t >> 8 & 1); }
  int readB() override { return static_cast<int>((t >> 11 & 1) ^ 1); }
  void writeS(uint8_t v) override { lastS = v; if (log) hS = fnv4(fnv4(hS, t), v); }
  void writeR(uint8_t v) override { lastR = v; if (log) hR = fnv4(fnv4(hR, t), v); }
};

static void testRegua(const std::string& arq) {
  std::ifstream f(arq);
  if (!f) { std::printf("(sem %s: rode tools/t7_gnw.sh para comparar com o MAME)\n", arq.c_str()); return; }
  int casos = 0, iguais = 0, ciclos = 40000;
  std::string linha;
  int porTipo[4] = {}, okTipo[4] = {};
  while (std::getline(f, linha)) {
    if (linha.rfind("#ciclos ", 0) == 0) { ciclos = std::atoi(linha.c_str() + 8); continue; }
    std::istringstream is(linha);
    int tipo; uint32_t seed; std::string eS, eR, eL;
    if (!(is >> tipo >> seed >> eS >> eR >> eL)) continue;
    std::vector<int> regs(51);
    for (auto& r : regs) is >> r;
    std::string eRam;
    is >> eRam;
    Xs rng{seed};
    std::vector<uint8_t> rom(0x1000), mel(0x100), ram(0x80);
    const int prgSize = tipo == 0 ? 0x740 : 0x1000;
    for (int i = 0; i < 0x1000; i++) {
      uint32_t b = rng();
      if ((b & 0x80) && (rng() & 3)) b &= 0x7F;
      rom[i] = i < prgSize ? static_cast<uint8_t>(b) : 0;
    }
    for (auto& v : mel) v = static_cast<uint8_t>(rng() & 0x3F);
    for (auto& v : ram) v = static_cast<uint8_t>(rng() & 0xF);
    IoRegua io;
    const Cpu cpu = tipo == 0 ? Cpu::SM5A : tipo == 1 ? Cpu::SM510 : tipo == 2 ? Cpu::SM511 : Cpu::SM512;
    Sm5xx m(cpu, &io);
    rom.resize(prgSize);
    m.setRom(rom, tipo >= 2 ? mel : std::vector<uint8_t>{});
    m.setRMask(tipo == 1 ? 2 : -1);
    m.powerOn();
    for (int i = 0; i < 0x80; i++) m.setRam(i, ram[i]);
    io.log = true;
    uint32_t hL = 0x811C9DC5u;
    uint16_t segs[32];
    for (io.t = 0; io.t < static_cast<uint32_t>(ciclos); io.t++) {
      if (io.t && (io.t & 8191) == 0) m.reset();
      m.run(1, [] {});
      if ((io.t & 31) == 31) {
        m.lcdOutputs(segs);
        for (int i = 0; i < 32; i++) hL = fnv4(hL, segs[i]);
      }
    }
    char h[3][16];
    std::snprintf(h[0], 16, "%08X", io.hS); std::snprintf(h[1], 16, "%08X", io.hR); std::snprintf(h[2], 16, "%08X", hL);
    std::string mram;
    for (int i = 0; i < 0x80; i++) { char c[4]; std::snprintf(c, 4, "%X", m.ram(i)); mram += c; }
    const bool ok = eS == h[0] && eR == h[1] && eL == h[2] && regs == m.registros() && mram == eRam;
    if (!ok && casos - iguais < 5) {
      std::printf("  régua: CPU %d semente %u difere (S %s/%s R %s/%s LCD %s/%s)\n", tipo, seed, eS.c_str(), h[0], eR.c_str(), h[1], eL.c_str(), h[2]);
      const auto mr = m.registros();
      for (size_t i = 0; i < regs.size() && i < mr.size(); i++)
        if (regs[i] != mr[i]) std::printf("    registro %zu: MAME %d, Índigo %d\n", i, regs[i], mr[i]);
    }
    casos++; iguais += ok;
    if (tipo >= 0 && tipo < 4) { porTipo[tipo]++; okTipo[tipo] += ok; }
  }
  std::printf("régua MAME: %d de %d programas aleatórios iguais (SM5A %d/%d, SM510 %d/%d, SM511 %d/%d, SM512 %d/%d), %d ciclos cada\n",
              iguais, casos, okTipo[0], porTipo[0], okTipo[1], porTipo[1], okTipo[2], porTipo[2], okTipo[3], porTipo[3], ciclos);
  check(casos > 0 && iguais == casos, "régua MAME: programas aleatórios iguais");
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/gnw";
  const Grade g51{4, 16, 4}, g5a{9, 4, 2};
  const auto svg51 = makeSvg(g51), svg5a = makeSvg(g5a);

  // ---------------------------------------------------------------- SM510 (pacote desconhecido)
  {
    const auto zip = makeZip({{"teste510.svg", svg51}, {"teste510.program", progSm510()}});
    saveFile(dir + "/teste510.zip", zip);
    check(isArcadeSet(zip, "teste510.zip"), "SM510: .zip vai inteiro ao núcleo");
    auto c = createCore(zip, "teste510.zip", nullptr);
    check(c && c->system() == "gnw", "SM510: detectado como Game & Watch");
    if (c) {
      auto& n = G(*c);
      check(n.cpu().tipo() == Cpu::SM510, "SM510: CPU pelo tamanho do programa");
      check(n.title() == "teste510", "SM510: título pelo nome do arquivo");
      check(n.segmentosDesenhados() == 256, "SM510: 256 segmentos no desenho (" + std::to_string(n.segmentosDesenhados()) + ")");
      frames(*c, 1);
      const int esperado[8] = {5, 2, 15, 0xB, 6, 0, 7, 0};
      for (int i = 0; i < 7; i++)
        check(n.cpu().ram(i) == esperado[i], "SM510: RAM " + std::to_string(i) + " = " + std::to_string(n.cpu().ram(i)) + " (esperado " + std::to_string(esperado[i]) + ")");
      check(n.cpu().ram(0x45) == 10, "SM510: SBM grava na RAM 0x45");
      check(n.cpu().halted(), "SM510: CPU parada no CEND");
      frames(*c, 330 - 1);  // 5,5 s
      check(n.cpu().ram(0x10) == 5, "SM510: 5 s contados pelo sinal de 1 s (" + std::to_string(n.cpu().ram(0x10)) + ")");
      check(n.segmentoAceso(0, 0) && !n.segmentoAceso(0, 1), "SM510: segmento 0.0.0 aceso, 0.1.0 apagado");
      check(n.segmentoAceso(8, 0) && !n.segmentoAceso(9, 0), "SM510: segmento bs 2.0.0 (registrador L)");
      // pixels: aceso escuro, apagado claro (escala do desenho para a tela)
      const double esc = std::min(800.0 / g51.w(), 600.0 / g51.h());
      const int on = brilho(*c, g51.cx(0, 0, 0) * esc, g51.cy(0, 0, 0) * esc);
      const int off = brilho(*c, g51.cx(0, 1, 0) * esc, g51.cy(0, 1, 0) * esc);
      check(on >= 0 && on < 40, "SM510: pixel do segmento aceso escuro (" + std::to_string(on) + ")");
      check(off > 150, "SM510: pixel do segmento apagado claro (" + std::to_string(off) + ")");
      writePpm(dir + "/teste510.ppm", *c);
      // desenho sem fundo próprio: vidro claro, segmento escuro
      auto c2 = createCore(makeZip({{"semfundo.svg", makeSvg(g51, false)}, {"semfundo.program", progSm510()}}), "semfundo.zip", nullptr);
      frames(*c2, 2);
      const int on2 = brilho(*c2, g51.cx(0, 0, 0) * esc, g51.cy(0, 0, 0) * esc), off2 = brilho(*c2, g51.cx(0, 1, 0) * esc, g51.cy(0, 1, 0) * esc);
      check(on2 < 40 && off2 > 150, "SM510: desenho sem fundo fica com o vidro claro (" + std::to_string(on2) + "/" + std::to_string(off2) + ")");
      check(!n.segmentoAceso(1 << 2 | 2, 0), "SM510: Game A solto");
      energia(*c);
      frames(*c, 10, KEY_START);  // Game A = linha 1, K3: acorda a CPU
      check(n.cpu().ram(0x70) == 4, "SM510: Game A lido na linha S1 (" + std::to_string(n.cpu().ram(0x70)) + ")");
      check(n.segmentoAceso(1 << 2 | 2, 0), "SM510: Game A acende o segmento 1.0.2");
      check(energia(*c) < 1e-6, "SM510: piezo quieto");
      frames(*c, 10, KEY_RIGHT);  // direcional = linha 2: liga o R1
      const double e = energia(*c);
      check(e > 1e-3, "SM510: piezo toca com o direcional (energia " + std::to_string(e) + ")");
      frames(*c, 10, 0);
      check(!n.segmentoAceso(1 << 2 | 2, 0), "SM510: segmento apaga quando solta");
      // frequência do piezo: R1 = bit 2 do divisor (4096 Hz)
      c->setSampleRate(32768);
      energia(*c);
      frames(*c, 30, KEY_RIGHT);
      std::vector<float> l(c->audioAvailable()), r(l.size());
      c->pullAudio(l.data(), r.data(), l.size());
      int cruz = 0;
      for (size_t i = 1000; i + 1 < l.size(); i++) cruz += (l[i] < 0) != (l[i + 1] < 0);
      const double hz = cruz / 2.0 / ((l.size() - 1001) / 32768.0);
      check(std::fabs(hz - 4096) < 100, "SM510: piezo a 4096 Hz (" + std::to_string(hz) + ")");
      c->setSampleRate(48000);
      testState(*c, "SM510");
      // save = máquina inteira (o aparelho fica sempre ligado), gravado no máximo uma vez por minuto
      check(c->hasSave(), "SM510: tem save");
      c->exportSave();
      frames(*c, 600);
      check(!c->saveDirty(), "SM510: save não grava antes de 1 minuto");
      frames(*c, 3000);
      check(c->saveDirty(), "SM510: save pronto depois de 1 minuto com a RAM mudando");
      const auto sv = c->exportSave();
      check(!c->saveDirty(), "SM510: save limpo depois de exportar");
      auto c3 = createCore(zip, "teste510.zip", &sv);
      check(c3 && G(*c3).cpu().ram(0x10) == n.cpu().ram(0x10), "SM510: save volta com o contador de segundos");
      if (c3) {
        frames(*c, 50); frames(*c3, 50);
        check(hashFb(*c) == hashFb(*c3) && G(*c3).cpu().registros() == n.cpu().registros(), "SM510: aparelho continua igual depois do save");
      }
      const std::vector<uint8_t> lixo(100, 0x55);
      auto c4 = createCore(zip, "teste510.zip", &lixo);
      check(c4 && G(*c4).cpu().ram(0) == 0, "SM510: save inválido é ignorado");
      frames(*c, 120);
      check(!c->saveDirty() && c->savePendente(), "SM510: menos de 1 minuto: não grava sozinho, mas grava ao fechar");
      {
        auto cn = createCore(zip, "outro nome.zip", &sv);
        auto co = createCore(zip, "teste510.zip", &sv);
        check(cn && co && cn->title() == "outro nome" && G(*cn).cpu().registros() == G(*co).cpu().registros() &&
                  G(*cn).cpu().ram(0x10) == G(*co).cpu().ram(0x10) && G(*co).cpu().ram(0x10) != 0,
              "SM510: .zip renomeado aceita o save antigo");
      }
      c->reset();
      frames(*c, 1);
      check(n.cpu().ram(0) == 5 && n.cpu().halted(), "SM510: ACL (reset) recomeça o programa");
    }
  }

  // ---------------------------------------------------------------- SM5A (pacote desconhecido)
  {
    const auto zip = makeZip({{"teste5a.svg", svg5a}, {"t5a", progSm5a()}});
    saveFile(dir + "/teste5a.zip", zip);
    auto c = createCore(zip, "teste5a.zip", nullptr);
    check(c && c->system() == "gnw", "SM5A: detectado");
    if (c) {
      auto& n = G(*c);
      check(n.cpu().tipo() == Cpu::SM5A, "SM5A: CPU pelo tamanho do programa (0x740)");
      check(n.segmentosDesenhados() == 72, "SM5A: 72 segmentos (" + std::to_string(n.segmentosDesenhados()) + ")");
      frames(*c, 1);
      const int esperado[4] = {5, 9, 4, 3};
      for (int i = 0; i < 4; i++)
        check(n.cpu().ram(i) == esperado[i], "SM5A: RAM " + std::to_string(i) + " = " + std::to_string(n.cpu().ram(i)));
      check(n.cpu().ram(0x4C) == 9, "SM5A: RAM 0x4E vira 0x4C");
      frames(*c, 329);
      check(n.cpu().ram(0x12) == 6, "SM5A: contador TG (1 na partida + 5 s) = " + std::to_string(n.cpu().ram(0x12)));
      check(!n.segmentoAceso(8 << 1, 2), "SM5A: W[8] bit 2 apagado sem tecla");
      frames(*c, 10, KEY_START);
      check(n.cpu().ram(0x10) == 4, "SM5A: Game A na linha R2 (" + std::to_string(n.cpu().ram(0x10)) + ")");
      check(n.segmentoAceso(8 << 1, 2), "SM5A: Game A acende o segmento 8.2.0 (W)");
      check(n.cpu().ram(0x11) == 0, "SM5A: BA solto (sem ligação)");
      frames(*c, 10, KEY_UP);
      check(n.cpu().ram(0x10) == 0, "SM5A: direcional é de outra linha");
      writePpm(dir + "/teste5a.ppm", *c);
      testState(*c, "SM5A");
    }
  }

  // ---------------------------------------------------------------- SM511 (melodia)
  {
    std::vector<uint8_t> mel(0x100, 0);
    mel[0] = mel[1] = mel[2] = mel[3] = 0x12;  // tom 2, 4 passos
    mel[4] = 0x01;  // fim
    const auto zip = makeZip({{"t511.svg", svg51}, {"t511.program", progSm511()}, {"t511.melody", mel}});
    auto c = createCore(zip, "t511.zip", nullptr);
    check(c && c->system() == "gnw", "SM511: detectado");
    if (c) {
      auto& n = G(*c);
      check(n.cpu().tipo() == Cpu::SM511, "SM511: CPU pelo arquivo de melodia");
      energia(*c);
      frames(*c, 3);
      check(n.cpu().clkDiv() == 4 && n.cpu().ram(0) == 0, "SM511: começa a 8 kHz, melodia tocando");
      const double e = energia(*c);
      check(e > 1e-3, "SM511: melodia sai no piezo (energia " + std::to_string(e) + ")");
      frames(*c, 27);
      check(n.cpu().ram(0) == 1, "SM511: TMEL vê o fim da melodia");
      check(n.cpu().clkDiv() == 2, "SM511: CLKHI troca para 16 kHz");
      energia(*c);
      frames(*c, 10);
      check(energia(*c) < 1e-6, "SM511: silêncio depois do RME");
      frames(*c, 5, KEY_X);  // linha 3 K3 = X
      check(n.cpu().ram(0x71) == 4, "SM511: tecla pelo PTW (" + std::to_string(n.cpu().ram(0x71)) + ")");
      testState(*c, "SM511");
    }
  }

  // ---------------------------------------------------------------- pacote da lista: Donkey Kong (SM510, 2 telas)
  {
    const auto zip = makeZip({{"dk-52", progSm510()}, {"gnw_dkong_top.svg", svg51}, {"gnw_dkong_bottom.svg", svg51}});
    saveFile(dir + "/gnw_dkong_teste.zip", zip);
    auto c = createCore(zip, "gnw_dkong.zip", nullptr);
    check(c && c->system() == "gnw", "Donkey Kong: detectado");
    if (c) {
      auto& n = G(*c);
      check(n.nomeJogo() == "gnw_dkong" && n.title() == "Game & Watch: Donkey Kong", "Donkey Kong: achado na lista (" + n.title() + ")");
      check(c->height() > c->width(), "Donkey Kong: duas telas uma sobre a outra");
      check(n.segmentosDesenhados() == 512, "Donkey Kong: segmentos das duas telas");
      frames(*c, 5, KEY_A);  // Jump = S1 K4 no Donkey Kong
      check(n.cpu().ram(0x70) == 8, "Donkey Kong: Jump na linha S1, K4 (" + std::to_string(n.cpu().ram(0x70)) + ")");
      frames(*c, 70, KEY_START);  // Game A = S3: não acorda a CPU nem aparece nas linhas lidas (espera o 1 s)
      check(n.cpu().ram(0x70) == 0, "Donkey Kong: Game A fica em outra linha");
      energia(*c);
      frames(*c, 5, KEY_UP);  // S2 K2 -> piezo
      check(energia(*c) > 1e-3, "Donkey Kong: cima na linha S2");
      writePpm(dir + "/gnw_dkong_teste.ppm", *c);
      testState(*c, "Donkey Kong");
    }
  }

  // ---------------------------------------------------------------- Ball (SM5A, linha fixa, BA ativo em 0)
  {
    const auto zip = makeZip({{"AC-01", progSm5a()}, {"gnw_ball.svg", svg5a}});
    auto c = createCore(zip, "gnw_ball.zip", nullptr);
    check(c && c->system() == "gnw", "Ball: detectado (nome do programa sem diferenciar maiúsculas)");
    if (c) {
      auto& n = G(*c);
      check(n.nomeJogo() == "gnw_ball", "Ball: achado na lista");
      frames(*c, 5, KEY_START);
      check(n.cpu().ram(0x10) == 4, "Ball: Game A na linha fixa");
      check(n.cpu().ram(0x11) == 0, "Ball: BA solto = 1");
      frames(*c, 5, KEY_RIGHT);
      check(n.cpu().ram(0x11) == 1, "Ball: direita puxa o BA para 0");
    }
  }

  // ---------------------------------------------------------------- detecção e arquivos ruins
  {
    const auto semSvg = makeZip({{"x.program", progSm510()}});
    check(!isArcadeSet(semSvg, "x.zip"), "zip sem .svg não é Game & Watch");
    const auto semProg = makeZip({{"x.svg", svg51}, {"leia.txt", std::vector<uint8_t>(10, 'a')}});
    check(!isArcadeSet(semProg, "x.zip"), "zip sem programa não é Game & Watch");
    // .zip de outro console com arte .svg não é roubado
    const std::vector<uint8_t> rom4k(0x1000, 0xEA);
    check(!isArcadeSet(makeZip({{"combat.a26", rom4k}, {"label.svg", svg51}}), "combat.zip"), "zip de 2600 com .svg não é Game & Watch");
    check(!isArcadeSet(makeZip({{"jogo.bin", rom4k}, {"capa.svg", svg51}}), "jogo.zip"), "zip com .bin de 4 KB e .svg não é Game & Watch");
    check(!isArcadeSet(makeZip({{"x.program", progSm510()}, {"x.svg", svg51}, {"leia.txt", std::vector<uint8_t>(10, 'a')}}), "x.zip"),
          "zip com arquivo estranho junto não é pacote desconhecido");
    check(!isArcadeSet(makeZip({{"a", rom4k}, {"b", rom4k}, {"x.svg", svg51}}), "x.zip"), "zip com dois programas não é pacote desconhecido");
    check(isArcadeSet(makeZip({{"dk-99", rom4k}, {"x.svg", svg51}}), "x.zip"), "programa sem extensão (como no MAME) é aceito");
    check(jogos().size() >= 59, "lista com os 59 Game & Watch do MAME");
    int sm5a = 0, sm510 = 0, sm511 = 0;
    for (const auto& j : jogos()) {
      sm5a += j.cpu == Cpu::SM5A; sm510 += j.cpu == Cpu::SM510; sm511 += j.cpu == Cpu::SM511 || j.cpu == Cpu::SM512;
      check(j.prog && j.svg[0], std::string("lista: ") + j.nome + " tem programa e tela");
    }
    check(sm5a > 10 && sm510 > 10 && sm511 > 10, "lista: as quatro CPUs aparecem");
    // .svg danificados: o núcleo abre (ou recusa com erro) sem cair
    std::mt19937 rng(11);
    int ok = 0;
    for (int k = 0; k < 40; k++) {
      std::vector<uint8_t> s = svg51;
      if (k < 20) for (int j = 0; j < 30; j++) s[rng() % s.size()] = static_cast<uint8_t>(rng());
      else s.resize(rng() % s.size());
      if (k == 39) { const std::string w = "<svg width=\"1e30\" height=\"nan\"><path d=\"M1e38 1e38L-1e38 5z\"/><path><title>0.0.0</title></path></svg>"; s.assign(w.begin(), w.end()); }
      try {
        auto c = createCore(makeZip({{"r.svg", s}, {"r.program", progSm510()}}), "r.zip", nullptr);
        if (c) { frames(*c, 2); ok++; }
      } catch (const std::exception&) { ok++; }
    }
    check(ok == 40, ".svg danificados não derrubam o núcleo");
  }

  testRegua(dir + "/vetores.txt");

  std::printf("RESULTADO gnw: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
