// Testes do núcleo Pokémon Mini (indigo::pm).
//   indigo-t7_pm <pasta>          pasta preparada por tools/t7_pm.sh (padrão /root/indigo-env/roms/pm)
//   indigo-t7_pm <pasta> <saída>  também grava as telas das ROMs de exemplo em PNG na pasta <saída>
// Verificações:
//   1. CPU contra os exemplos das páginas de instruções da documentação (pm-dev-docs: ADD/SUB/BCD/nibble/MUL/
//      DIV/NEG/pilha/saltos) e contra a tabela de ciclos;
//   2. CPU contra 300 mil vetores gerados com a CPU do emulador de referência PokeMini (cpu_vectors.bin):
//      registradores, flags, ciclos e gravações na memória. Diferenças aceitas (contadas à parte): o carry do
//      ADC/SBC com carry de entrada e o modo BCD, onde o PokeMini calcula em 8 bits e erra (o Índigo segue a
//      documentação);
//   3. ROMs de exemplo do PokeMini: tela depois de N quadros (hash conferido olhando a imagem), som do
//      sound_tst, determinismo do save state, estado corrompido, ROMs aleatórias;
//   4. EEPROM I2C: grava e lê de volta pelos pinos, como um jogo faria.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/pm/pm.h"

using namespace indigo;
using namespace indigo::pm;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// ------------------------------------------------------------------ PNG mínimo (deflate sem compressão)
static uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFF) {
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return c;
}
static void writePng(const std::string& path, const uint32_t* px, int w, int h, int scale) {
  const int W = w * scale, H = h * scale;
  std::vector<uint8_t> raw;
  for (int y = 0; y < H; y++) {
    raw.push_back(0);
    for (int x = 0; x < W; x++) {
      const uint32_t c = px[(y / scale) * w + x / scale];
      raw.push_back(c & 0xFF); raw.push_back((c >> 8) & 0xFF); raw.push_back((c >> 16) & 0xFF);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw.size(); off += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n == raw.size() ? 1 : 0);
    z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
  }
  const uint32_t ad = (b << 16) | a;
  for (int s = 24; s >= 0; s -= 8) z.push_back((ad >> s) & 0xFF);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
    const uint32_t n = uint32_t(d.size());
    for (int s = 24; s >= 0; s -= 8) out.push_back((n >> s) & 0xFF);
    std::vector<uint8_t> td(t, t + 4);
    td.insert(td.end(), d.begin(), d.end());
    out.insert(out.end(), td.begin(), td.end());
    const uint32_t c = ~crc32(td.data(), td.size());
    for (int s = 24; s >= 0; s -= 8) out.push_back((c >> s) & 0xFF);
  };
  std::vector<uint8_t> ihdr;
  for (int v : {W, H}) for (int s = 24; s >= 0; s -= 8) ihdr.push_back((v >> s) & 0xFF);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(out.data()), out.size());
}

static uint32_t fnv(const void* d, size_t n) {
  uint32_t h = 0x811C9DC5u;
  for (size_t i = 0; i < n; i++) { h ^= static_cast<const uint8_t*>(d)[i]; h *= 0x01000193u; }
  return h;
}

// ------------------------------------------------------------------ barramento de teste da CPU
static uint8_t memv(uint32_t a) {
  uint32_t h = a * 2654435761u;
  h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
  return uint8_t(h);
}
struct TestBus : MinxBus {
  std::vector<std::pair<uint32_t, uint8_t>> writes;
  uint8_t prog[8] = {};
  uint32_t progAddr = 0;
  bool flat = false;
  std::vector<uint8_t> mem;  // modo "plano": 64 KB por banco 0..1
  uint8_t read(uint32_t a) override {
    if (flat) return mem[a & 0x1FFFF];
    for (size_t i = writes.size(); i-- > 0;)
      if (writes[i].first == a) return writes[i].second;
    if (a >= progAddr && a < progAddr + 8) return prog[a - progAddr];
    return memv(a);
  }
  void write(uint32_t a, uint8_t v) override {
    if (flat) mem[a & 0x1FFFF] = v;
    else writes.push_back({a, v});
  }
  void irqCheck() override {}
};

// executa uma sequência de bytes a partir de $1000 numa memória plana
struct Mini {
  TestBus bus;
  Minx cpu;
  Mini() { bus.flat = true; bus.mem.assign(0x20000, 0); cpu.bus = &bus; cpu.PC = 0x1000; cpu.SP = 0x2000; cpu.F = 0; }
  int run(std::initializer_list<uint8_t> code) {
    uint32_t a = cpu.PC;
    for (uint8_t b : code) bus.mem[a++] = b;
    return cpu.step();
  }
  uint8_t fz() const { return cpu.F & 1; }
  uint8_t fc() const { return (cpu.F >> 1) & 1; }
  uint8_t fo() const { return (cpu.F >> 2) & 1; }
  uint8_t fs() const { return (cpu.F >> 3) & 1; }
};

static void manualTests() {
  int before = fails;
  {  // ADD A,$80 com A=$55 -> $D5, O=1 S=1
    Mini m; m.cpu.A = 0x55; int c = m.run({0x02, 0x80});
    // a página da documentação diz O=1, mas 85 + (-128) = -43 não estoura (a regra da própria página dá O=0)
    check(m.cpu.A == 0xD5 && !m.fz() && !m.fc() && !m.fo() && m.fs() && c == 8, "ADD A,#nn (exemplo 1)");
  }
  {  // ADD A,B: $CF+$31 -> $00 Z C
    Mini m; m.cpu.A = 0xCF; m.cpu.B = 0x31; m.run({0x01});
    check(m.cpu.A == 0 && m.fz() && m.fc() && !m.fo() && !m.fs(), "ADD A,B (exemplo 2)");
  }
  {  // ADD A,[HL]: $CF+$DE -> $AD C S
    Mini m; m.cpu.A = 0xCF; m.cpu.H = 0x30; m.cpu.L = 0x00; m.bus.mem[0x3000] = 0xDE; m.run({0x03});
    check(m.cpu.A == 0xAD && !m.fz() && m.fc() && !m.fo() && m.fs(), "ADD A,[HL] (exemplo 3)");
  }
  {  // BCD: 09 + 01 = 10
    Mini m; m.cpu.F = 0x10; m.cpu.A = 0x09; m.run({0x02, 0x01});
    check(m.cpu.A == 0x10 && !m.fc() && !m.fz(), "ADD BCD 09+01");
  }
  {  // BCD: 99 + 99 = 98 com carry (decimal 198)
    Mini m; m.cpu.F = 0x10; m.cpu.A = 0x99; m.run({0x02, 0x99});
    check(m.cpu.A == 0x98 && m.fc(), "ADD BCD 99+99");
  }
  {  // BCD: 00 - 01 = 99 com borrow
    Mini m; m.cpu.F = 0x10; m.cpu.A = 0x00; m.run({0x12, 0x01});
    check(m.cpu.A == 0x99 && m.fc(), "SUB BCD 00-01");
  }
  {  // nibble: $3F + 1 -> $00 Z C
    Mini m; m.cpu.F = 0x20; m.cpu.A = 0x3F; m.run({0x02, 0x01});
    check(m.cpu.A == 0x00 && m.fz() && m.fc() && !m.fo() && !m.fs(), "ADD nibble $3F+1");
  }
  {  // ADC com carry: $05 + $FF + 1 = $105 -> carry
    Mini m; m.cpu.F = 0x02; m.cpu.A = 0x05; m.run({0x0A, 0xFF});
    check(m.cpu.A == 0x05 && m.fc(), "ADC A,#nn com carry de entrada");
  }
  {  // SUB A,#: $10-$20 -> $F0 C S
    Mini m; m.cpu.A = 0x10; m.run({0x12, 0x20});
    check(m.cpu.A == 0xF0 && m.fc() && m.fs() && !m.fo(), "SUB A,#nn com borrow");
  }
  {  // SUB overflow: $80-1 = $7F O
    Mini m; m.cpu.A = 0x80; m.run({0x12, 0x01});
    check(m.cpu.A == 0x7F && m.fo() && !m.fc(), "SUB overflow");
  }
  {  // DIV HL,A: 7/2 -> L=3 H=1
    Mini m; m.cpu.A = 2; m.cpu.H = 0; m.cpu.L = 7; int c = m.run({0xCE, 0xD9});
    check(m.cpu.L == 3 && m.cpu.H == 1 && !m.fz() && !m.fc() && !m.fo() && !m.fs() && c == 52, "DIV 7/2");
  }
  {  // DIV $FFFD/2: quociente não cabe -> sem mudança, O=1 S=1
    Mini m; m.cpu.A = 2; m.cpu.H = 0xFF; m.cpu.L = 0xFD; m.run({0xCE, 0xD9});
    check(m.cpu.H == 0xFF && m.cpu.L == 0xFD && m.fo() && m.fs(), "DIV estouro");
  }
  {  // MUL L,A: $80*$02 = $0100
    Mini m; m.cpu.A = 2; m.cpu.L = 0x80; int c = m.run({0xCE, 0xD8});
    check(m.cpu.H == 0x01 && m.cpu.L == 0x00 && !m.fz() && !m.fs() && c == 48, "MUL");
  }
  {  // NEG A: $01 -> $FF C S; $80 -> $80 O
    Mini m; m.cpu.A = 1; m.run({0xCE, 0xA4});
    check(m.cpu.A == 0xFF && m.fc() && m.fs(), "NEG 1");
    Mini n; n.cpu.A = 0x80; n.run({0xCE, 0xA4});
    check(n.cpu.A == 0x80 && n.fo() && n.fc(), "NEG $80");
  }
  {  // PUSH BA / POP HL: B em cima, A embaixo
    Mini m; m.cpu.A = 0x34; m.cpu.B = 0x12; m.run({0xA0});
    check(m.cpu.SP == 0x1FFE && m.bus.mem[0x1FFF] == 0x12 && m.bus.mem[0x1FFE] == 0x34, "PUSH BA");
    m.run({0xA9});
    check(m.cpu.H == 0x12 && m.cpu.L == 0x34 && m.cpu.SP == 0x2000, "POP HL");
  }
  {  // CALL/RET: empilha V, PC alto, PC baixo
    Mini m; int c = m.run({0xF2, 0x10, 0x00});  // CALL +$10 (relativo ao último byte)
    check(m.cpu.PC == 0x1012 && c == 24 && m.bus.mem[0x1FFD] == 0x03 && m.bus.mem[0x1FFE] == 0x10, "CALL #ssss");
    m.bus.mem[0x1012] = 0xF8;
    c = m.cpu.step();
    check(m.cpu.PC == 0x1003 && c == 16 && m.cpu.SP == 0x2000, "RET");
  }
  {  // JRS para trás: F1 FE -> fica no mesmo lugar? (PC+2-2-1 = PC-1)
    Mini m; m.run({0xF1, 0xFE});
    check(m.cpu.PC == 0x0FFF, "JMP #ss negativo");
  }
  {  // JDBNZ: B=2 -> salta; B=1 -> não salta
    Mini m; m.cpu.B = 2; m.run({0xF5, 0x05});
    check(m.cpu.B == 1 && m.cpu.PC == 0x1006, "JDBNZ salta");
    Mini n; n.cpu.B = 1; n.run({0xF5, 0x05});
    check(n.cpu.B == 0 && n.cpu.PC == 0x1002 && n.fz(), "JDBNZ não salta");
  }
  {  // MOV U + salto longo: o banco novo vale no salto; PC>=$8000 usa V
    Mini m; m.cpu.A = 2; m.run({0xCE, 0xCC});        // MOV U,A
    m.run({0xF3, 0xFD, 0x6F});                        // JMP +$6FFD -> $8001 (relativo ao último byte)
    check(m.cpu.V == 2 && m.cpu.PC == 0x8001, "MOV U + JMP: V recebe U");
    m.bus.mem[0x8001] = 0xB0; m.bus.mem[0x8002] = 0x42;  // banco 2: $8001 -> (2 << 15) | $0001 = $10001
    m.bus.mem[0x10001] = 0xB0; m.bus.mem[0x10002] = 0x77;
    m.cpu.step();
    check(m.cpu.A == 0x77, "busca no banco V");
  }
  {  // SAL: $40 -> $80 O S
    Mini m; m.cpu.A = 0x40; m.run({0xCE, 0x80});
    check(m.cpu.A == 0x80 && m.fo() && m.fs() && !m.fc(), "SAL");
  }
  {  // ciclos de alguns MOV (tabela da documentação)
    Mini m; check(m.run({0x40}) == 4, "ciclos MOV A,A");
    check(m.run({0x44, 0x00}) == 12, "ciclos MOV A,[N+nn]");
    check(m.run({0xB8, 0x00, 0x20}) == 20, "ciclos MOV BA,[nnnn]");
    check(m.run({0xCF, 0xB8}) == 48, "ciclos PUSHA");
  }
  std::printf("CPU (exemplos da documentação): %d verificações, %d falhas\n", checks, fails - before);
}

// ------------------------------------------------------------------ vetores do PokeMini
static void vectorTests(const std::string& path) {
  const std::vector<uint8_t> d = readAll(path);
  if (d.empty()) { std::printf("cpu_vectors.bin ausente: vetores pulados\n"); return; }
  size_t p = 0;
  int n = 0, bad = 0, known = 0;
  TestBus bus;
  Minx cpu;
  cpu.bus = &bus;
  auto rd8 = [&]() { return d[p++]; };
  auto rd16 = [&]() { uint16_t v = d[p] | (d[p + 1] << 8); p += 2; return v; };
  struct Regs { uint16_t BA, HL, X, Y, SP, PC; uint8_t I, N, XI, YI, V, U1, U2, SU, F, E, ST; };
  auto readRegs = [&]() {
    Regs r;
    r.BA = rd16(); r.HL = rd16(); r.I = rd8(); r.N = rd8(); r.X = rd16(); r.XI = rd8(); r.Y = rd16(); r.YI = rd8();
    r.SP = rd16(); r.PC = rd16(); r.V = rd8(); r.U1 = rd8(); r.U2 = rd8(); r.SU = rd8(); r.F = rd8(); r.E = rd8();
    r.ST = rd8();
    return r;
  };
  const size_t REC = 26;
  while (p + REC + 8 + REC + 2 <= d.size()) {
    const Regs a = readRegs();
    uint8_t prog[8];
    std::memcpy(prog, &d[p], 8); p += 8;
    const Regs b = readRegs();
    const int cyc = rd8();
    const int nw = rd8();
    std::vector<std::pair<uint32_t, uint8_t>> ws;
    for (int k = 0; k < nw; k++) { ws.push_back({uint32_t(d[p] | (d[p + 1] << 8) | (d[p + 2] << 16)), d[p + 3]}); p += 4; }
    cpu.A = a.BA & 0xFF; cpu.B = a.BA >> 8; cpu.L = a.HL & 0xFF; cpu.H = a.HL >> 8; cpu.I = a.I; cpu.N = a.N;
    cpu.X = a.X; cpu.XI = a.XI; cpu.Y = a.Y; cpu.YI = a.YI; cpu.SP = a.SP; cpu.PC = a.PC; cpu.V = a.V;
    cpu.U1 = a.U1; cpu.U2 = a.U2; cpu.shiftU = a.SU; cpu.F = a.F; cpu.E = a.E; cpu.status = a.ST;
    bus.writes.clear();
    std::memcpy(bus.prog, prog, 8);
    bus.progAddr = (a.PC & 0x8000) ? ((uint32_t(a.V) << 15) | (a.PC & 0x7FFF)) : a.PC;
    const int c = cpu.step();
    const bool same = cpu.BA() == b.BA && cpu.HL() == b.HL && cpu.I == b.I && cpu.N == b.N && cpu.X == b.X &&
                      cpu.XI == b.XI && cpu.Y == b.Y && cpu.YI == b.YI && cpu.SP == b.SP && cpu.PC == b.PC &&
                      cpu.V == b.V && cpu.U1 == b.U1 && cpu.U2 == b.U2 && cpu.shiftU == b.SU && cpu.F == b.F &&
                      cpu.E == b.E && cpu.status == b.ST && c == cyc && bus.writes == ws;
    n++;
    if (!same) {
      // diferenças conhecidas: PokeMini erra o carry do ADC/SBC com carry de entrada e faz o BCD em 8 bits
      // só aceita se o resto (endereços, PC, pilha, índices, ciclos, nº de gravações) bate
      const bool carryIn = a.F & 2, bcd = a.F & 0x10;
      bool rest = cpu.PC == b.PC && cpu.SP == b.SP && cpu.X == b.X && cpu.Y == b.Y && cpu.V == b.V && c == cyc &&
                  bus.writes.size() == ws.size() && cpu.HL() == b.HL;
      for (size_t k = 0; rest && k < ws.size(); k++) rest = bus.writes[k].first == ws[k].first;
      const bool onlyCarry = ((cpu.F ^ b.F) & ~2) == 0 && cpu.BA() == b.BA && bus.writes == ws;
      if (rest && (bcd || (carryIn && onlyCarry))) {
        known++;
      } else {
        if (bad < 15) {
          std::printf("vetor %d: %02X %02X %02X %02X  BA %04X/%04X HL %04X/%04X F %02X/%02X PC %04X/%04X SP %04X/%04X "
                      "X %04X/%04X Y %04X/%04X ciclos %d/%d grav %zu/%zu\n",
                      n, prog[0], prog[1], prog[2], prog[3], cpu.BA(), b.BA, cpu.HL(), b.HL, cpu.F, b.F, cpu.PC,
                      b.PC, cpu.SP, b.SP, cpu.X, b.X, cpu.Y, b.Y, c, cyc, bus.writes.size(), ws.size());
        }
        bad++;
      }
    }
  }
  std::printf("CPU contra o PokeMini: %d vetores, %d iguais, %d diferenças conhecidas (carry/BCD), %d erradas\n", n,
              n - bad - known, known, bad);
  check(n > 0 && bad == 0, "vetores da CPU contra o PokeMini");
}

// ------------------------------------------------------------------ ROMs
static double audioEnergy(PMCore& core, int frames, std::vector<float>* keep = nullptr) {
  double e = 0;
  std::vector<float> l(4096), r(4096);
  for (int f = 0; f < frames; f++) {
    core.runFrame();
    size_t n = core.audioAvailable();
    while (n) {
      const size_t k = std::min<size_t>(n, l.size());
      core.pullAudio(l.data(), r.data(), k);
      for (size_t i = 0; i < k; i++) { e += double(l[i]) * l[i]; if (keep) keep->push_back(l[i]); }
      n -= k;
    }
  }
  return e;
}

static int litPixels(PMCore& core) {
  int lit = 0;
  const uint32_t* fb = core.framebuffer();
  for (int i = 0; i < 96 * 64; i++) if ((fb[i] & 0xFF) < 150) lit++;
  return lit;
}

static void romTests(const std::string& dir, const std::string& outDir) {
  struct Case { const char* file; int frames; uint32_t keys; uint32_t hash; bool blank; };
  // hashes conferidos olhando as imagens gravadas (ver relatório)
  const Case cases[] = {
      {"main.min", 300, 0, 0xE4A01DC5, true},  // o modelo vazio só liga as interrupções: tela limpa
      {"framebuffer.min", 300, 0, 0x57349347, false},
      {"bouncing.min", 300, 0, 0x5A4593C9, false},
      {"mapview.min", 300, KEY_RIGHT | KEY_DOWN, 0x31CA4E73, false},
      {"sound_tst.min", 300, KEY_A, 0x352C5595, false},  // A toca a música
  };
  int tested = 0;
  for (const Case& c : cases) {
    const std::vector<uint8_t> rom = readAll(dir + "/roms/" + c.file);
    if (rom.empty()) { std::printf("%s ausente: pulado\n", c.file); continue; }
    tested++;
    check(isPMRom(rom, c.file), std::string("detecção ") + c.file);
    PMCore core(rom, nullptr);
    core.setSampleRate(48000);
    double e = 0;
    for (int f = 0; f < c.frames; f++) {
      core.setKeys(f > 150 && (f < 160 || c.keys != KEY_A) ? c.keys : 0u);
      e += audioEnergy(core, 1);
    }
    const uint32_t h = fnv(core.framebuffer(), 96 * 64 * 4);
    const int lit = litPixels(core);
    std::printf("%-16s título \"%s\"  pixels escuros %d  hash %08X  energia do som %.2f\n", c.file,
                core.title().c_str(), lit, h, e);
    check(c.blank ? lit == 0 : lit > 20, std::string("tela: ") + c.file);
    if (c.hash) check(h == c.hash, std::string("tela esperada: ") + c.file);
    if (std::string(c.file) == "sound_tst.min") check(e > 1.0, "som do sound_tst");
    if (!outDir.empty()) writePng(outDir + "/" + c.file + ".png", core.framebuffer(), 96, 64, 4);

    // determinismo do save state
    const std::vector<uint8_t> st = core.saveState();
    std::vector<float> a1, a2;
    for (int f = 0; f < 60; f++) { core.setKeys(f & 8 ? uint32_t(KEY_A) : 0u); audioEnergy(core, 1, &a1); }
    const uint32_t h1 = fnv(core.framebuffer(), 96 * 64 * 4);
    core.loadState(st);
    for (int f = 0; f < 60; f++) { core.setKeys(f & 8 ? uint32_t(KEY_A) : 0u); audioEnergy(core, 1, &a2); }
    const uint32_t h2 = fnv(core.framebuffer(), 96 * 64 * 4);
    check(h1 == h2 && a1 == a2, std::string("save state determinístico: ") + c.file);
  }
  if (!tested) std::printf("ROMs de exemplo ausentes: puladas\n");
}

// ------------------------------------------------------------------ robustez
static void robustTests(const std::string& dir) {
  std::mt19937 rng(1234);
  // ROMs aleatórias
  for (int k = 0; k < 20; k++) {
    std::vector<uint8_t> rom(0x4000 << (k % 4));
    for (auto& b : rom) b = uint8_t(rng());
    PMCore core(rom, nullptr);
    for (int f = 0; f < 30; f++) { core.setKeys(rng()); core.runFrame(); }
    std::vector<float> l(64), r(64);
    core.pullAudio(l.data(), r.data(), 64);
  }
  check(true, "ROMs aleatórias rodam sem travar");
  // estados corrompidos e truncados
  std::vector<uint8_t> rom = readAll(dir + "/roms/bouncing.min");
  if (rom.empty()) { rom.assign(0x4000, 0xFF); }
  PMCore core(rom, nullptr);
  for (int f = 0; f < 30; f++) core.runFrame();
  const std::vector<uint8_t> st = core.saveState();
  int threw = 0;
  for (int k = 0; k < 300; k++) {
    std::vector<uint8_t> bad = st;
    if (k % 3 == 0) bad.resize(rng() % bad.size());
    else for (int j = 0; j < 40; j++) bad[rng() % bad.size()] = uint8_t(rng());
    try { core.loadState(bad); } catch (const std::exception&) { threw++; }
    for (int f = 0; f < 2; f++) core.runFrame();
  }
  check(true, "estados corrompidos não travam");
  std::printf("estados corrompidos: %d recusados, os outros rodaram\n", threw);
  bool wrong = false;
  try { PMCore tiny(std::vector<uint8_t>(100, 0), nullptr); } catch (const std::runtime_error&) { wrong = true; }
  check(wrong, "ROM pequena demais é recusada");
}

// ------------------------------------------------------------------ EEPROM pelos pinos (I2C)
struct I2C {
  PM& pm;
  uint8_t dir = 0x0C, data = 0x0C;
  explicit I2C(PM& p) : pm(p) { set(1, 1); }
  void set(int scl, int sda) {
    data = uint8_t((data & ~0x0C) | (scl ? 8 : 0) | (sda ? 4 : 0));
    pm.write(0x2060, dir);
    pm.write(0x2061, data);
  }
  void start() { set(1, 1); set(1, 0); set(0, 0); }
  void stop() { set(0, 0); set(1, 0); set(1, 1); }
  bool sendByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) { set(0, (b >> i) & 1); set(1, (b >> i) & 1); set(0, (b >> i) & 1); }
    dir = 0x08; set(0, 1); set(1, 1);  // solta SDA e lê a confirmação
    const bool ack = !(pm.read(0x2061) & 4);
    set(0, 1); dir = 0x0C;
    return ack;
  }
  uint8_t recvByte(bool ack) {
    dir = 0x08;
    uint8_t v = 0;
    for (int i = 0; i < 8; i++) { set(0, 1); set(1, 1); v = uint8_t((v << 1) | ((pm.read(0x2061) >> 2) & 1)); }
    set(0, 1);
    dir = 0x0C;
    set(0, ack ? 0 : 1); set(1, ack ? 0 : 1); set(0, ack ? 0 : 1);
    return v;
  }
};

static void eepromTests() {
  std::vector<uint8_t> rom(0x4000, 0xFF);
  PMCore core(rom, nullptr);
  PM& pm = core.pm();
  I2C bus(pm);
  bus.start();
  bool ok = bus.sendByte(0xA0) && bus.sendByte(0x01) && bus.sendByte(0x23);
  const uint8_t msg[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  for (uint8_t b : msg) ok = ok && bus.sendByte(b);
  bus.stop();
  check(ok, "EEPROM confirma (ACK) os bytes gravados");
  check(pm.eeprom[0x123] == 0xDE && pm.eeprom[0x126] == 0xEF && core.saveDirty(), "EEPROM gravada pelo I2C");
  // leitura aleatória: endereço, novo START, A1, lê 4 bytes
  bus.start();
  ok = bus.sendByte(0xA0) && bus.sendByte(0x01) && bus.sendByte(0x23);
  bus.start();
  ok = ok && bus.sendByte(0xA1);
  uint8_t got[4];
  for (int k = 0; k < 4; k++) got[k] = bus.recvByte(k < 3);
  bus.stop();
  check(ok && std::memcmp(got, msg, 4) == 0, "EEPROM lida de volta pelo I2C");
  const std::vector<uint8_t> sv = core.exportSave();
  check(sv.size() == 8192 && sv[0x124] == 0xAD && !core.saveDirty(), "exportSave da EEPROM");
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/pm";
  const std::string outDir = argc > 2 ? argv[2] : "";
  {
    std::ifstream probe(dir);
    if (!probe) {
      std::printf("pasta %s não existe: testes do Pokémon Mini pulados\n", dir.c_str());
      return 0;
    }
  }
  manualTests();
  vectorTests(dir + "/cpu_vectors.bin");
  romTests(dir, outDir);
  eepromTests();
  robustTests(dir);
  std::printf("RESULTADO pm: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
