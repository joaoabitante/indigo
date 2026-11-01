// Testes do Vectrex: (1) programa de validação do 6809 de W. Schwotzer (cputest, conferido num EF6809P);
// (2) ciclos de cada instrução e de cada pós-byte indexado contra a tabela medida no hardware do 6809Decoder
// (hoglet67); (3) com a BIOS: Mine Storm liga e desenha (tela conferida por hash, PNG gravado), joystick
// mexe a nave, save state determinístico, salvar->carregar->salvar idêntico e estados danificados.
// uso: indigo-t7_vectrex /root/indigo-env/roms/vectrex [pasta-para-png]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/vectrex/m6809.h"
#include "../src/vectrex/vectrex.h"

using namespace indigo;
using namespace indigo::vectrex;

namespace {
int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) fails++;
  std::printf("%s %s\n", ok ? "ok  " : "FALHA", what.c_str());
}

std::vector<uint8_t> readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// memória plana de 64 KB para testar a CPU sozinha
struct FlatBus : M6809Bus {
  uint8_t m[65536] = {};
  uint8_t read(uint16_t a) override { return m[a]; }
  void write(uint16_t a, uint8_t v) override { m[a] = v; }
};

bool loadHex(const std::string& path, FlatBus& bus) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    if (line.size() < 11 || line[0] != ':') continue;
    auto hx = [&](size_t p) { return static_cast<int>(std::strtol(line.substr(p, 2).c_str(), nullptr, 16)); };
    const int len = hx(1), addr = hx(3) << 8 | hx(5), type = hx(7);
    if (type != 0) continue;
    for (int i = 0; i < len; i++) bus.m[(addr + i) & 0xFFFF] = static_cast<uint8_t>(hx(9 + i * 2));
    n += len;
  }
  return n > 0;
}

// ------------------------------------------------------------------ (1) cputest
void cpuTest(const std::string& dir) {
  auto bus = std::make_unique<FlatBus>();
  if (!loadHex(dir + "/cputest/ct.hex", *bus)) { std::printf("pulei: cputest/ct.hex não existe (rode t7_vectrex.sh)\n"); return; }
  M6809 cpu(*bus);
  // rotinas do FLEX que o programa chama: RTS em cada uma; o teste intercepta pelo PC
  const uint16_t WARMS = 0xCD03, PUTCHR = 0xCD18, PSTRNG = 0xCD1E, PCRLF = 0xCD24, OUTDEC = 0xCD39;
  for (uint16_t a : {WARMS, PUTCHR, PSTRNG, PCRLF, OUTDEC}) bus->m[a] = 0x39;
  bus->m[0xFFFE] = 0x81; bus->m[0xFFFF] = 0x00;
  cpu.reset();
  cpu.s = 0x7F00;
  std::string out;
  long steps = 0;
  bool done = false;
  while (steps++ < 20000000) {
    const uint16_t pc = cpu.pc;
    if (pc == WARMS) { done = true; break; }
    if (pc == PSTRNG) {
      out += "\n";
      for (uint16_t p = cpu.x; bus->m[p] != 4 && out.size() < 100000; p++) out += static_cast<char>(bus->m[p]);
    } else if (pc == PUTCHR) {
      out += static_cast<char>(cpu.a);
    } else if (pc == PCRLF) {
      out += "\n";
    }
    cpu.step();
  }
  std::printf("cputest diz:%s\n", out.c_str());
  check(done && out.find("All Tests succeded") != std::string::npos && out.find("Failed") == std::string::npos,
        "6809: programa de validação de W. Schwotzer (EF6809P) passa inteiro");
}

// ------------------------------------------------------------------ (2) ciclos
struct RefTables {
  int cyc[768] = {};
  bool illegal[768] = {};
  int post[256] = {};
  bool ok = false;
};

RefTables parseHoglet(const std::string& path) {
  RefTables t;
  std::ifstream f(path);
  if (!f) return t;
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string src = ss.str();
  size_t st = src.find("static opcode_t instr_table_6809[] = {");
  if (st == std::string::npos) return t;
  const size_t en = src.find("};", st);
  const std::string body = src.substr(st, en - st);
  std::regex re(R"(/\*\s*([0-9A-Fa-f]+)\s*\*/\s*\{\s*&op_(\w+)\s*,\s*(\w+)\s*,\s*(\d)\s*,\s*(\w+))");
  int i = 0;
  for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator() && i < 768; ++it, ++i) {
    t.cyc[i] = static_cast<int>(std::strtol((*it)[5].str().c_str(), nullptr, 0));
    t.illegal[i] = (*it)[3].str() == "ILLEGAL";
  }
  if (i != 768) return t;
  st = src.find("static int postbyte_cycles_6809[0x100] = {");
  if (st == std::string::npos) return t;
  st = src.find('\n', st);
  int k = 0;
  for (size_t p = st; k < 256 && p < src.size();) {
    if (src.compare(p, 2, "//") == 0) { p = src.find('\n', p); continue; }
    if (src[p] == '-' || std::isdigit(static_cast<unsigned char>(src[p]))) {
      char* e;
      t.post[k++] = static_cast<int>(std::strtol(src.c_str() + p, &e, 10));
      p = static_cast<size_t>(e - src.c_str());
    } else {
      p++;
    }
  }
  t.ok = k == 256;
  // códigos inválidos das páginas 2/3: rodam o da página 0 com 1 ciclo a mais (como o próprio decodificador faz)
  for (int j = 256; j < 768; j++) if (t.illegal[j]) t.cyc[j] = t.cyc[j & 0xFF] + 1;
  return t;
}

// roda uma instrução isolada e devolve os ciclos medidos
int runOne(int page, int op, int post, uint8_t cc) {
  auto bus = std::make_unique<FlatBus>();
  M6809 cpu(*bus);
  uint16_t p = 0x1000;
  if (page) bus->m[p++] = page == 2 ? 0x10 : 0x11;
  bus->m[p++] = static_cast<uint8_t>(op);
  bus->m[p++] = static_cast<uint8_t>(post);  // pós-byte / operando
  bus->m[p++] = 0x00;
  bus->m[p++] = 0x00;
  for (int v = 0xFFF0; v < 0x10000; v += 2) { bus->m[v] = 0x20; bus->m[v + 1] = 0x00; }
  cpu.reset();
  cpu.pc = 0x1000; cpu.s = 0x8000; cpu.u = 0x9000; cpu.x = 0x3000; cpu.y = 0x4000; cpu.cc = cc;
  return cpu.step();
}

void cycleTest(const std::string& dir) {
  const RefTables ref = parseHoglet(dir + "/6809Decoder/src/em_6809.c");
  if (!ref.ok) { std::printf("pulei: 6809Decoder/src/em_6809.c não existe (rode t7_vectrex.sh)\n"); return; }
  int tested = 0, bad = 0;
  for (int i = 0; i < 768; i++) {
    const int page = i < 256 ? 0 : i < 512 ? 2 : 3, op = i & 0xFF;
    if (page == 0 && (op == 0x10 || op == 0x11)) continue;  // prefixos
    if (page && (op == 0x10 || op == 0x11)) continue;
    if (ref.cyc[i] >= 0x10000) continue;                      // trava a CPU (HCF)
    const int base = op & 0xFF;
    if (base == 0x13 || base == 0x3C) {                       // SYNC/CWAI: só a tabela base
      tested++;
      if (m6809BaseCycles(page, op) != ref.cyc[i]) { bad++; std::printf("  ciclos %X:%02X tabela %d, hardware %d\n", page, op, m6809BaseCycles(page, op), ref.cyc[i]); }
      continue;
    }
    // CC = 0: desvios "BRA BHI BCC BNE BVC BPL BGE BGT" são tomados; o longo tomado custa 1 a mais
    const int got = runOne(page, op, 0x84 /* ,X : sem custo extra; PSH/PUL de X */, 0);
    int expect = ref.cyc[i];
    const int lo = op & 0x0F;
    const bool isBranch = (op & 0xF0) == 0x20;
    const bool takenCC0 = lo == 0 || lo == 2 || lo == 4 || lo == 6 || lo == 8 || lo == 0xA || lo == 0xC || lo == 0xE;
    if (page == 2 && isBranch && op != 0x20 && takenCC0 && !ref.illegal[i]) expect += 1;
    if ((op == 0x34 || op == 0x35 || op == 0x36 || op == 0x37) && (page == 0 || ref.illegal[i])) expect += 3;  // 0x84 = PC(2) + DP(1)
    tested++;
    if (got != expect) { bad++; std::printf("  ciclos %X:%02X medido %d, hardware %d\n", page, op, got, expect); }
  }
  check(bad == 0, "6809: ciclos de " + std::to_string(tested) + " códigos (páginas 0/2/3) iguais aos do hardware");
  bad = 0;
  for (int pb = 0; pb < 256; pb++) {
    const int got = runOne(0, 0xA6, pb, 0);  // LDA indexado: 4 + custo do pós-byte
    const int expect = 4 + std::abs(ref.post[pb]);
    if (got != expect) { bad++; std::printf("  pós-byte %02X medido %d, hardware %d\n", pb, got, expect); }
  }
  check(bad == 0, "6809: ciclos dos 256 pós-bytes indexados iguais aos do hardware");
}

// ------------------------------------------------------------------ PNG
void putBE(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back(static_cast<uint8_t>(x >> (i * 8))); }
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0 - (c & 1))); }
  return ~c;
}
void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) { const uint32_t c = px[y * w + x]; raw.push_back(c & 255); raw.push_back((c >> 8) & 255); raw.push_back((c >> 16) & 255); }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size(); i += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - i);
    z.push_back(i + n == raw.size() ? 1 : 0);
    z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
  }
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  putBE(z, b << 16 | a);
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
    putBE(f, static_cast<uint32_t>(d.size()));
    std::vector<uint8_t> c(t, t + 4);
    c.insert(c.end(), d.begin(), d.end());
    f.insert(f.end(), c.begin(), c.end());
    putBE(f, crc32(c.data(), c.size()));
  };
  std::vector<uint8_t> ih;
  putBE(ih, w); putBE(ih, h);
  ih.insert(ih.end(), {8, 2, 0, 0, 0});
  chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), f.size());
}

uint32_t hashFb(const Core& c) {
  return crc32(reinterpret_cast<const uint8_t*>(c.framebuffer()), static_cast<size_t>(c.width()) * c.height() * 4);
}
int litPixels(const Core& c, int x0 = 0, int y0 = 0, int x1 = -1, int y1 = -1) {
  if (x1 < 0) { x1 = c.width(); y1 = c.height(); }
  int n = 0;
  for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) if ((c.framebuffer()[y * c.width() + x] & 0xFF) > 60) n++;
  return n;
}

// ------------------------------------------------------------------ (2b) régua do vecx
// a BIOS sem cartucho por 15 milhões de instruções (abertura, música, tela de escolha e o começo do Mine
// Storm), com a E/S no início da instrução como o vecx; estado da CPU a cada 1000 instruções
void vecxTest(const std::string& dir, const std::vector<uint8_t>& bios) {
  std::ifstream f(dir + "/vecx_trace.txt");
  if (!f || bios.size() != 8192) { std::printf("pulei: vecx_trace.txt não existe (rode t7_vectrex.sh)\n"); return; }
  VectrexCore v(bios, nullptr);
  v.accessAtStart = true;
  std::string line;
  long i = 0, n = 0;
  bool ok = true;
  char buf[64];
  while (std::getline(f, line)) {
    for (; i < n * 1000; i++) v.stepInstr();
    const M6809& c = v.cpu();
    std::snprintf(buf, sizeof buf, "%04X %02X %02X %04X %04X %04X %04X %02X %02X", c.pc, c.a, c.b, c.x, c.y, c.u, c.s, c.dp, c.cc & 0xDF);
    if (line != buf) { std::printf("  instrução %ld: vecx %s, aqui %s\n", n * 1000, line.c_str(), buf); ok = false; break; }
    n++;
  }
  check(ok && n >= 15000, "BIOS igual ao vecx (CPU, timers da VIA, AY e comparador) por " + std::to_string(n * 1000) + " instruções");
}

// ------------------------------------------------------------------ (3) console com a BIOS
void consoleTest(const std::string& dir, const std::string& pngDir) {
  const std::vector<uint8_t> bios = readFile(dir + "/bios/System.bin");
  if (bios.size() != 8192) { std::printf("pulei: bios/System.bin não existe (rode t7_vectrex.sh)\n"); return; }
  check(isVectrexBios(bios) && isVectrexRom(bios, "system.bin"), "a BIOS é reconhecida (vetores) e aceita como jogo");
  try { VectrexCore v(std::vector<uint8_t>(1024, 0x12), nullptr); check(false, "sem BIOS: pede o arquivo"); }
  catch (const std::runtime_error& e) { check(std::string(e.what()).find("BIOS do Vectrex") != std::string::npos, "sem BIOS: pede o arquivo da BIOS"); }

  // a própria BIOS aberta como jogo: Mine Storm
  VectrexCore v(bios, nullptr);
  check(v.title() == "MINE STORM", "título lido do cabeçalho: \"" + v.title() + "\"");
  for (int f = 0; f < 150; f++) v.runFrame();  // tela de abertura (logo + nome)
  const int lit0 = litPixels(v);
  if (!pngDir.empty()) writePng(pngDir + "/vectrex_abertura.png", v.framebuffer(), v.width(), v.height());
  check(lit0 > 800, "abertura da BIOS desenhada (" + std::to_string(lit0) + " pixels acesos)");
  for (int f = 0; f < 1050; f++) v.runFrame();  // depois da música de abertura e da tela de escolha, o Mine Storm começa
  if (!pngDir.empty()) writePng(pngDir + "/vectrex_minestorm.png", v.framebuffer(), v.width(), v.height());
  const uint32_t h1 = hashFb(v);
  std::printf("hash da tela do Mine Storm (quadro 1200): %08X\n", h1);
  check(litPixels(v) > 300 && h1 == 0x2A5D5FBCu, "Mine Storm desenhando, tela conferida (PNG) e fixada por hash");

  // determinismo: dois consoles iguais dão a mesma tela
  {
    VectrexCore w(bios, nullptr);
    for (int f = 0; f < 1200; f++) w.runFrame();
    check(hashFb(w) == h1, "mesma tela em duas execuções");
  }
  // save state: salvar, rodar 60 quadros com o joystick, carregar, rodar de novo = mesma tela e mesmo som
  const std::vector<uint8_t> st = v.saveState();
  v.setSampleRate(48000);
  std::vector<float> l1(4096), r1(4096), l2(4096), r2(4096);
  while (v.audioAvailable()) v.pullAudio(l1.data(), r1.data(), std::min<size_t>(4096, v.audioAvailable()));
  auto runInput = [&](std::vector<float>& L, std::vector<float>& R) {
    for (int f = 0; f < 60; f++) { v.setKeys(f < 30 ? (KEY_LEFT | KEY_A) : KEY_UP); v.runFrame(); }
    v.setKeys(0);
    const size_t n = std::min<size_t>(4096, v.audioAvailable());
    v.pullAudio(L.data(), R.data(), n);
    while (v.audioAvailable()) { float t[256], u[256]; v.pullAudio(t, u, std::min<size_t>(256, v.audioAvailable())); }
    return hashFb(v);
  };
  const uint32_t ha = runInput(l1, r1);
  v.loadState(st);
  const uint32_t hb = runInput(l2, r2);
  check(ha == hb && l1 == l2, "save state determinístico (tela e som iguais depois de carregar)");
  check(ha != h1, "o joystick e o botão mudam o jogo");
  v.loadState(st);
  check(v.saveState() == st, "salvar -> carregar -> salvar dá os mesmos bytes");
  // estados danificados: truncado, lixo e de outro tamanho; o jogo continua
  int recusados = 0;
  std::vector<uint8_t> tr(st.begin(), st.begin() + st.size() / 2);
  try { v.loadState(tr); } catch (const std::runtime_error&) { recusados++; }
  std::vector<uint8_t> lixo(st.size(), 0xA5);
  try { v.loadState(lixo); } catch (const std::runtime_error&) { recusados++; }
  check(recusados == 2 && v.saveState() == st, "estado truncado/lixo recusado e o jogo segue igual");
  // estado com bytes trocados (após o cabeçalho): carrega ou recusa, mas roda sem travar
  uint32_t s = 7;
  for (int k = 0; k < 200; k++) {
    std::vector<uint8_t> c = st;
    for (int j = 0; j < 8; j++) { s = s * 1103515245 + 12345; c[12 + (s >> 8) % (c.size() - 12)] ^= static_cast<uint8_t>(s >> 3); }
    try { v.loadState(c); v.runFrame(); } catch (const std::runtime_error&) {}
  }
  check(true, "200 estados adulterados carregados/recusados sem travar");

  // som: a música da abertura toca entre os quadros 255 e 340
  {
    VectrexCore w(bios, nullptr);
    w.setSampleRate(48000);
    double e = 0;
    for (int f = 0; f < 320; f++) {
      if (f < 250) { w.runFrame(); float L[2048], R[2048]; while (w.audioAvailable()) w.pullAudio(L, R, std::min<size_t>(2048, w.audioAvailable())); continue; }
      w.runFrame();
      float L[2048], R[2048];
      while (w.audioAvailable()) { const size_t n = std::min<size_t>(2048, w.audioAvailable()); w.pullAudio(L, R, n); for (size_t i = 0; i < n; i++) e += L[i] * L[i]; }
    }
    check(e > 10.0, "som do AY na música de abertura (energia " + std::to_string(e) + ")");
  }

  // ROMs de lixo com cabeçalho (código sem sentido, inclusive pulando para a RAM e a VIA): nada pode travar
  {
    setFirmware("vectrex", bios);
    uint32_t r = 99;
    for (int k = 0; k < 40; k++) {
      const size_t size = (k % 3 == 0) ? 0x10000 : (k % 3 == 1) ? 0x2000 : 300;
      std::vector<uint8_t> rom(size);
      for (auto& x : rom) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; x = static_cast<uint8_t>(r); }
      std::memcpy(rom.data(), "g GCE 1982\x80", 11);
      VectrexCore w(rom, nullptr);
      w.setKeys(r & 0xFFF);
      for (int f = 0; f < 40; f++) w.runFrame();
      // com a CPU pulando direto para o lixo (sem a abertura da BIOS)
      w.cpu().pc = static_cast<uint16_t>(r);
      for (int f = 0; f < 20; f++) w.runFrame();
    }
    setFirmware("vectrex", {});
    check(true, "40 ROMs de lixo rodaram sem travar");
  }

  // cartucho: um programa mínimo (cabeçalho GCE) que usa as rotinas da BIOS para desenhar um quadrado no
  // centro com escala 0x40; confere a posição e o tamanho do quadrado na tela
  {
    std::vector<uint8_t> cart = {'g', ' ', 'G', 'C', 'E', ' ', '1', '9', '8', '2', 0x80, 0xFD, 0x0D,  // música da BIOS
                                 0xF8, 0x50, 0x20, 0xC0, 'T', 'E', 'S', 'T', 'E', 0x80, 0x00};
    const size_t start = cart.size();
    const uint8_t prog[] = {
        0xBD, 0xF1, 0x92,        // laço: JSR Wait_Recal
        0xBD, 0xF2, 0xA9,        //       JSR Intensity_7F
        0x86, 0x40, 0xB7, 0xD0, 0x04,  // LDA #$40 ; STA VIA_t1_cnt_lo (escala)
        0xCC, 0xE0, 0xE0,        //       LDD #$E0E0 (y=-32, x=-32)
        0xBD, 0xF3, 0x12,        //       JSR Moveto_d
        0x8E, 0x00, 0x00,        //       LDX #lista (corrigido abaixo)
        0xBD, 0xF3, 0xDF,        //       JSR Draw_VL_a... (Draw_VLc em F3CE)
        0x20, 0x00};             //       BRA laço (corrigido abaixo)
    cart.insert(cart.end(), prog, prog + sizeof prog);
    const size_t list = cart.size();
    const uint8_t vl[] = {0x03, 0x40, 0x00, 0x00, 0x40, 0xC0, 0x00, 0x00, 0xC0};  // contagem-1, (y,x)...
    cart.insert(cart.end(), vl, vl + sizeof vl);
    cart[start + 18] = static_cast<uint8_t>(list >> 8); cart[start + 19] = static_cast<uint8_t>(list);
    cart[start + 21] = 0xF3; cart[start + 22] = 0xCE;  // Draw_VLc
    cart[start + 24] = static_cast<uint8_t>(-25);       // volta para o JSR Wait_Recal
    if (!pngDir.empty()) std::ofstream(pngDir + "/vectrex_teste.bin", std::ios::binary).write(reinterpret_cast<const char*>(cart.data()), cart.size());
    setFirmware("vectrex", bios);
    VectrexCore w(cart, nullptr);
    setFirmware("vectrex", {});
    for (int f = 0; f < 700; f++) w.runFrame();  // a BIOS mostra a abertura (512 quadros) e o título do cartucho
    if (!pngDir.empty()) writePng(pngDir + "/vectrex_quadrado.png", w.framebuffer(), w.width(), w.height());
    // caixa acesa
    int minx = 9999, maxx = -1, miny = 9999, maxy = -1;
    for (int y = 0; y < w.height(); y++)
      for (int x = 0; x < w.width(); x++)
        if ((w.framebuffer()[y * w.width() + x] & 0xFF) > 60) { minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); }
    std::printf("quadrado aceso em x %d..%d, y %d..%d\n", minx, maxx, miny, maxy);
    // lado = 0x40 * 0x40 = 4096 unidades = ~41 px; começa em (-32,-32)*0x40 = -20 px do centro (165,205)
    const bool ok = std::abs((maxx - minx) - 41) <= 4 && std::abs((maxy - miny) - 41) <= 4 &&
                    std::abs((minx + maxx) / 2 - 165) <= 4 && std::abs((miny + maxy) / 2 - 205) <= 4;
    check(w.title() == "TESTE" && ok, "cartucho próprio: quadrado de 41 px centrado (escala, DAC, RAMP e ZERO)");
  }
}
}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) { std::printf("uso: indigo-t7_vectrex pasta [pasta-png]\n"); return 2; }
  const std::string dir = argv[1];
  if (!std::ifstream(dir + "/bios/System.bin") && !std::ifstream(dir + "/cputest/ct.hex")) {
    std::printf("pulei: pasta %s sem os testes do Vectrex\n", dir.c_str());
    return 0;
  }
  cpuTest(dir);
  cycleTest(dir);
  vecxTest(dir, readFile(dir + "/bios/System.bin"));
  consoleTest(dir, argc > 2 ? argv[2] : "");
  std::printf("RESULTADO vectrex: %d verificações, %d falhas\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
