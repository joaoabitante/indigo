// Testes do GTE do PlayStation (COP2).
//   indigo-t9_gte <pasta> [casos]   pasta = /root/indigo-env/roms/ps1gte (preparada pelo t9_gte.sh)
// 1. vetores do ps1-tests (gte/test-all/tests.c, 1150 casos medidos num console de verdade);
// 2. teste diferencial contra o GTE do Mednafen (libgteref.so, compilada fora do repositório pelo
//    t9_gte.sh): registradores aleatórios + comandos aleatórios (com os bits sf/mx/v/cv/lm), todos os
//    64 registradores comparados depois das escritas e depois de cada comando;
// 3. save state: salvar -> carregar -> salvar dá os mesmos bytes e a execução continua igual;
// 4. velocidade do RTPT e do NCDT.
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/ps1/gte.h"

using namespace indigo;
using namespace indigo::ps1;

namespace {

int g_checks = 0, g_fails = 0;
void check(bool ok, const char* what) {
  g_checks++;
  if (!ok) { g_fails++; std::printf("  FALHOU: %s\n", what); }
}

// leitura/escrita pelo índice do ps1-tests: 0..31 dados, 32..63 controle
uint32_t rd(Gte& g, int r) { return r < 32 ? g.readData(r) : g.readCtrl(r - 32); }
void wr(Gte& g, int r, uint32_t v) { if (r < 32) g.writeData(r, v); else g.writeCtrl(r - 32, v); }

// ---- régua
struct Ref {
  void (*init)();
  void (*power)();
  int (*cmd)(uint32_t);
  void (*wd)(unsigned, uint32_t);
  void (*wc)(unsigned, uint32_t);
  uint32_t (*rdd)(unsigned);
  uint32_t (*rdc)(unsigned);
  uint32_t rd(int r) const { return r < 32 ? rdd(r) : rdc(r - 32); }
  void wr(int r, uint32_t v) const { if (r < 32) wd(r, v); else wc(r - 32, v); }
};

// a régua é uma biblioteca compartilhada (no Windows, gteref.dll, se alguém a compilar lá)
void* abreBiblioteca(const std::string& dir) {
#ifdef _WIN32
  return reinterpret_cast<void*>(LoadLibraryA((dir + "/gteref.dll").c_str()));
#else
  return dlopen((dir + "/libgteref.so").c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}
void* simbolo(void* h, const char* nome) {
#ifdef _WIN32
  return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(h), nome));
#else
  return dlsym(h, nome);
#endif
}

bool loadRef(const std::string& dir, Ref& f) {
  void* h = abreBiblioteca(dir);
  if (!h) return false;
  f.init = reinterpret_cast<void (*)()>(simbolo(h, "ref_init"));
  f.power = reinterpret_cast<void (*)()>(simbolo(h, "ref_power"));
  f.cmd = reinterpret_cast<int (*)(uint32_t)>(simbolo(h, "ref_cmd"));
  f.wd = reinterpret_cast<void (*)(unsigned, uint32_t)>(simbolo(h, "ref_wd"));
  f.wc = reinterpret_cast<void (*)(unsigned, uint32_t)>(simbolo(h, "ref_wc"));
  f.rdd = reinterpret_cast<uint32_t (*)(unsigned)>(simbolo(h, "ref_rd"));
  f.rdc = reinterpret_cast<uint32_t (*)(unsigned)>(simbolo(h, "ref_rc"));
  return f.init && f.power && f.cmd && f.wd && f.wc && f.rdd && f.rdc;
}

// ---- gerador (xorshift, semente fixa: o teste é reprodutível)
uint64_t g_rng = 0x9E3779B97F4A7C15ull;
uint32_t rnd() {
  g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
  return static_cast<uint32_t>(g_rng >> 16);
}

// valor para um registrador: mistura de aleatório puro, pequeno, extremos e "realista"
uint32_t rndVal(int r) {
  const uint32_t k = rnd() % 16;
  auto h16 = [](uint32_t lo, uint32_t hi) { return (lo & 0xFFFF) | (hi << 16); };
  switch (k) {
    case 0: case 1: case 2: case 3: return rnd();
    case 4: { static const uint32_t ext[] = {0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x7FFF7FFF, 0x80008000,
                                             0x00007FFF, 0x00008000, 0xFFFF8000, 0x00001000, 0x10001000,
                                             0x0000FFFF, 0xFFFF0000, 0x01000100, 0x000003FF, 0xFFFFFC00};
              return ext[rnd() % (sizeof(ext) / sizeof(ext[0]))]; }
    case 5: case 6: return h16(rnd() % 0x2000 - 0x1000, rnd() % 0x2000 - 0x1000);   // ±4096
    case 7: return h16(rnd() % 0x200 - 0x100, rnd() % 0x200 - 0x100);               // ±256
    case 8: return rnd() % 0x10000;
    case 9: return rnd() % 0x400;
    case 10: return (rnd() % 0x20000) - 0x10000;
    default: break;
  }
  // "realista" pelo registrador (projeção que não satura)
  switch (r) {
    case 1: case 3: case 5: return rnd() % 0x4000;          // VZ
    case 6: return rnd();                                    // RGBC
    case 8: return rnd() % 0x1001;                           // IR0
    case 16: case 17: case 18: case 19: return rnd() % 0x8000;
    case 32 + 5: case 32 + 6: return rnd() % 0x800 - 0x400;  // TRX/TRY
    case 32 + 7: return rnd() % 0x4000;                      // TRZ
    case 32 + 24: case 32 + 25: return (rnd() % 0x400) << 16;  // OFX/OFY
    case 32 + 26: return rnd() % 0x400;                      // H
    case 32 + 27: return rnd() % 0x10000;                    // DQA
    case 32 + 28: return rnd() % 0x2000000;                  // DQB
    case 32 + 29: case 32 + 30: return rnd() % 0x800;        // ZSF3/ZSF4
    default: return h16(rnd() % 0x2000 - 0x1000, rnd() % 0x2000 - 0x1000);
  }
}

const char* opName(int code) {
  switch (code) {
    case 0x00: case 0x01: return "RTPS"; case 0x06: return "NCLIP"; case 0x0C: return "OP";
    case 0x10: return "DPCS"; case 0x11: return "INTPL"; case 0x12: return "MVMVA"; case 0x13: return "NCDS";
    case 0x14: return "CDP"; case 0x16: return "NCDT"; case 0x1A: case 0x29: return "DCPL";
    case 0x1B: return "NCCS"; case 0x1C: return "CC"; case 0x1E: return "NCS"; case 0x20: return "NCT";
    case 0x28: return "SQR"; case 0x2A: return "DPCT"; case 0x2D: return "AVSZ3"; case 0x2E: return "AVSZ4";
    case 0x30: return "RTPT"; case 0x3D: return "GPF"; case 0x3E: return "GPL"; case 0x3F: return "NCCT";
    default: return "?";
  }
}
const int kCodes[] = {0x01, 0x30, 0x06, 0x0C, 0x10, 0x2A, 0x11, 0x12, 0x13, 0x16, 0x14, 0x1B, 0x3F,
                      0x1C, 0x1E, 0x20, 0x28, 0x29, 0x2D, 0x2E, 0x3D, 0x3E, 0x00, 0x1A};

// ---- 1. ps1-tests
void testPs1Tests(const std::string& dir) {
  std::ifstream f(dir + "/ps1-tests/gte/test-all/tests.c");
  if (!f) { std::printf("  ps1-tests não encontrado (rode t9_gte.sh)\n"); check(false, "ps1-tests/gte"); return; }
  // cada caso: ".input = {" 64 números, ".opcode = X", ".output = {" 64 números
  std::string line;
  std::vector<uint32_t> in, out;
  uint32_t opc = 0;
  int mode = 0, total = 0, ok = 0, shown = 0;
  while (std::getline(f, line)) {
    if (line.find(".input") != std::string::npos) { mode = 1; in.clear(); continue; }
    if (line.find(".output") != std::string::npos) { mode = 2; out.clear(); continue; }
    if (line.find(".opcode") != std::string::npos) {
      opc = static_cast<uint32_t>(std::strtoul(line.substr(line.find('=') + 1).c_str(), nullptr, 16));
      mode = 0;
      continue;
    }
    const size_t p = line.find("0x");
    if (mode && p != std::string::npos && line.find("// r[") != std::string::npos) {
      const uint32_t v = static_cast<uint32_t>(std::strtoul(line.c_str() + p, nullptr, 16));
      (mode == 1 ? in : out).push_back(v);
      if (mode == 2 && out.size() == 64) {
        Gte g;
        for (int r = 0; r < 64; r++) wr(g, r, 0);
        for (int r = 0; r < 64; r++) wr(g, r, in[r]);
        if (opc != 0xFFFFFFFFu) g.command(opc & 0x1FFFFFF);
        bool good = true;
        for (int r = 0; r < 64; r++) {
          const uint32_t v2 = rd(g, r);
          if (v2 != out[r]) {
            good = false;
            if (shown < 10) {
              shown++;
              std::printf("  ps1-tests caso %d (op %08x %s) r%d: %08x, console %08x\n", total + 1, opc,
                          opc == 0xFFFFFFFFu ? "-" : opName(opc & 0x3F), r, v2, out[r]);
            }
          }
        }
        total++;
        ok += good;
        mode = 0;
      }
    }
  }
  std::printf("  ps1-tests/gte/test-all: %d de %d casos iguais ao console\n", ok, total);
  check(total == 1150 && ok == total, "ps1-tests/gte/test-all");
}

// ---- divergência conhecida: o quociente da divisão UNR pode dar 0x20000 quando H = 2*SZ3 - 1 (e vizinhos).
// O Mednafen deixa passar 0x20000; o psx-spx (e o DuckStation) limitam a 0x1FFFF, já que o divisor do
// hardware tem 17 bits. O Índigo segue o psx-spx; o teste conta esses casos à parte.
uint32_t unrSemLimite(uint32_t h, uint32_t sz) {
  const int z = __builtin_clz(sz) - 16;
  const uint32_t n = h << z;
  uint32_t d = sz << z;
  const int i = static_cast<int>((d - 0x7FC0) >> 7);
  const uint32_t u = static_cast<uint32_t>((0x40000 / (i + 0x100) + 1) / 2 - 0x101 < 0 ? 0 : (0x40000 / (i + 0x100) + 1) / 2 - 0x101) + 0x101;
  d = (0x2000080 - d * u) >> 8;
  d = (0x0000080 + d * u) >> 8;
  return static_cast<uint32_t>((uint64_t(n) * d + 0x8000) >> 16);
}

bool tocaLimiteDaDivisao(const Gte& g, uint32_t op) {
  const int code = op & 0x3F;
  if (code != 0x00 && code != 0x01 && code != 0x30) return false;
  Gte t = g;
  t.command(op);
  const uint32_t h = t.readCtrl(26) & 0xFFFF;
  for (int r = code == 0x30 ? 17 : 19; r <= 19; r++) {
    const uint32_t sz = t.readData(r);
    if (h < sz * 2 && unrSemLimite(h, sz) > 0x1FFFF) return true;
  }
  return false;
}

// copia o estado do Índigo para a régua (depois de uma divergência conhecida)
void ressincroniza(const Ref& ref, Gte& g) {
  for (int r = 0; r < 31; r++)
    if (r != 15 && r != 28 && r != 29) ref.wr(r, rd(g, r));
  for (int r = 32; r < 64; r++) ref.wr(r, rd(g, r));
}

// ---- 2. diferencial
void testDiff(const Ref& ref, long cases) {
  ref.init();
  Gte g;
  long same = 0, cmds = 0, cmdsSame = 0, cyclesSame = 0, known = 0;
  int shown = 0;
  long perOp[64] = {}, badOp[64] = {};
  uint32_t mine[64], theirs[64];
  auto compare = [&](const char* when, uint32_t op, long caseNo) {
    bool good = true;
    for (int r = 0; r < 64; r++) {
      mine[r] = rd(g, r);
      theirs[r] = ref.rd(r);
      if (mine[r] != theirs[r]) good = false;
    }
    if (!good && shown < 12) {
      shown++;
      std::printf("  caso %ld %s op %07x (%s sf=%d mx=%d v=%d cv=%d lm=%d):", caseNo, when, op, opName(op & 0x3F),
                  (op >> 19) & 1, (op >> 17) & 3, (op >> 15) & 3, (op >> 13) & 3, (op >> 10) & 1);
      for (int r = 0; r < 64; r++)
        if (mine[r] != theirs[r]) std::printf(" r%d=%08x/ref %08x", r, mine[r], theirs[r]);
      std::printf("\n");
    }
    return good;
  };
  for (long c = 0; c < cases; c++) {
    // escritas: todos os registradores (com SXYP e LZCS no meio) ou só alguns
    const bool all = (c & 3) != 3;
    for (int r = 0; r < 64; r++) {
      if (!all && (rnd() & 3)) continue;
      const uint32_t v = rndVal(r);
      wr(g, r, v);
      ref.wr(r, v);
    }
    bool good = compare("depois das escritas", 0, c);
    // de 1 a 4 comandos seguidos sobre o mesmo estado
    const int n = 1 + (rnd() & 3);
    for (int k = 0; k < n && good; k++) {
      uint32_t op = rnd() & 0x1FFFFC0;
      const uint32_t pick = rnd() % 100;
      int code = pick < 92 ? kCodes[rnd() % (sizeof(kCodes) / sizeof(kCodes[0]))] : static_cast<int>(rnd() & 0x3F);
      op |= static_cast<uint32_t>(code);
      const bool conhecida = tocaLimiteDaDivisao(g, op);
      const int cy = g.command(op);
      const int cyRef = ref.cmd(op);
      cyclesSame += cy == cyRef + 1;  // o Mednafen devolve os ciclos menos 1
      cmds++;
      perOp[code]++;
      if (conhecida) { known++; ressincroniza(ref, g); continue; }
      const bool ok = compare("depois do comando", op, c);
      if (ok) cmdsSame++; else badOp[code]++;
      good = ok;
    }
    same += good;
  }
  for (int i = 0; i < 64; i++)
    if (badOp[i]) std::printf("  %s (0x%02x): %ld de %ld diferentes\n", opName(i), i, badOp[i], perOp[i]);
  std::printf("  diferencial contra o Mednafen: %ld de %ld casos iguais (%ld de %ld comandos; %ld com a divergência"
              " conhecida do limite 0x1FFFF da divisão)\n", same, cases, cmdsSame, cmds, known);
  std::printf("  ciclos: %ld de %ld comandos com a mesma contagem do Mednafen\n", cyclesSame, cmds);
  check(same == cases && cmdsSame + known == cmds, "diferencial contra o Mednafen");
}

// divisão UNR: com RT = 0, TR = (0, 0, sz), DQA = 1 e DQB = 0 o RTPS deixa SZ3 = sz e MAC0 = quociente.
// Todos os divisores de 1 a 0xFFFF, cada um com os H vizinhos de 2*sz (onde o limite 0x1FFFF aparece),
// pequenos e aleatórios.
void testDivide(const Ref& ref) {
  Gte g;
  for (int r = 0; r < 64; r++) { wr(g, r, 0); ref.wr(r, 0); }
  for (int r : {32 + 27}) { wr(g, r, 1); ref.wr(r, 1); }
  long n = 0, same = 0, known = 0;
  int shown = 0;
  for (uint32_t sz = 1; sz <= 0xFFFF; sz++) {
    wr(g, 32 + 7, sz); ref.wr(32 + 7, sz);
    uint32_t hs[24];
    int k = 0;
    for (int d = 1; d <= 6; d++) hs[k++] = 2 * sz > static_cast<uint32_t>(d) ? 2 * sz - d : 0;
    hs[k++] = 2 * sz; hs[k++] = sz; hs[k++] = sz - 1; hs[k++] = sz + 1; hs[k++] = 0; hs[k++] = 1; hs[k++] = 0xFFFF;
    while (k < 24) hs[k++] = rnd() % 0x10000;
    for (int i = 0; i < 24; i++) {
      const uint32_t h = hs[i] & 0xFFFF;
      wr(g, 32 + 26, h); ref.wr(32 + 26, h);
      g.command(0x00080001);
      ref.cmd(0x00080001);
      const bool ok = rd(g, 24) == ref.rd(24) && rd(g, 63) == ref.rd(63) && rd(g, 19) == ref.rd(19);
      n++;
      same += ok;
      if (!ok && ref.rd(24) == 0x20000 && rd(g, 24) == 0x1FFFF && rd(g, 63) == ref.rd(63)) { known++; continue; }
      if (!ok && shown++ < 5)
        std::printf("  divisão H=%04x SZ3=%04x: %08x (FLAG %08x), ref %08x (FLAG %08x)\n", h, sz, rd(g, 24), rd(g, 63),
                    ref.rd(24), ref.rd(63));
    }
  }
  std::printf("  divisão UNR: %ld de %ld quocientes iguais ao Mednafen; %ld onde ele dá 0x20000 e o Índigo limita a 0x1FFFF\n",
              same, n, known);
  check(same + known == n, "divisão UNR contra o Mednafen");
}

// ---- 3. save state
void testState() {
  Gte a;
  for (int r = 0; r < 64; r++) wr(a, r, rndVal(r));
  a.command(0x30 | (1 << 19));
  StateIO s1;
  a.save(s1);
  Gte b;
  StateIO in(s1.data());
  b.load(in);
  StateIO s2;
  b.save(s2);
  check(s1.data() == s2.data(), "save state: salvar->carregar->salvar dá os mesmos bytes");
  bool same = true;
  for (int i = 0; i < 1000; i++) {
    const uint32_t op = (rnd() & 0x1FFFFC0) | kCodes[rnd() % (sizeof(kCodes) / sizeof(kCodes[0]))];
    a.command(op);
    b.command(op);
    for (int r = 0; r < 64; r++) same &= rd(a, r) == rd(b, r);
  }
  check(same, "save state: execução continua igual depois de carregar");
  // estado truncado não pode carregar
  std::vector<uint8_t> cut(s1.data().begin(), s1.data().end() - 3);
  bool threw = false;
  try { StateIO t(cut); Gte c; c.load(t); } catch (const std::exception&) { threw = true; }
  check(threw, "save state truncado é recusado");
  std::printf("  save state: %zu bytes\n", s1.data().size());
}

// ---- 4. velocidade
void testSpeed() {
  Gte g;
  // cena típica: matriz de rotação perto da identidade, vértices pequenos, projeção sem saturar
  g.writeCtrl(0, 0x00000F00); g.writeCtrl(1, 0x00000100); g.writeCtrl(2, 0x00000F00);
  g.writeCtrl(3, 0x01000000); g.writeCtrl(4, 0x0F00);
  g.writeCtrl(5, 10); g.writeCtrl(6, -20); g.writeCtrl(7, 2000);
  for (int r = 8; r <= 12; r++) g.writeCtrl(r, 0x08000800);
  for (int r = 16; r <= 20; r++) g.writeCtrl(r, 0x0C000C00);
  g.writeCtrl(13, 0x100); g.writeCtrl(14, 0x80); g.writeCtrl(15, 0x40);
  g.writeCtrl(21, 0x400); g.writeCtrl(22, 0x800); g.writeCtrl(23, 0xC00);
  g.writeCtrl(24, 160 << 16); g.writeCtrl(25, 120 << 16); g.writeCtrl(26, 300);
  g.writeCtrl(27, 0xFFFF8000); g.writeCtrl(28, 0x01400000);
  g.writeData(6, 0x30808080);
  g.writeData(8, 0x800);
  auto bench = [&](uint32_t op, const char* name) {
    const long n = 5000000;
    volatile uint32_t sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; i++) {
      const uint32_t k = static_cast<uint32_t>(i);
      g.writeData(0, ((k & 0xFF) - 0x80) | (((k >> 8) & 0xFF) << 16));
      g.writeData(1, 0x100 + (k & 0x3FF));
      g.writeData(2, 0x00400040);
      g.writeData(3, 0x200);
      g.writeData(4, 0xFFC0FFC0);
      g.writeData(5, 0x300);
      g.command(op);
      sink = sink + g.readData(14);
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  %s: %.1f ns por comando (%.1f milhões/s)\n", name, s * 1e9 / n, n / s / 1e6);
    return s * 1e9 / n;
  };
  const double rtpt = bench(0x00080030, "RTPT");
  const double ncdt = bench(0x00080416, "NCDT");
  // um jogo pesado faz uns 3 mil polígonos por quadro: 60 quadros x 3000 RTPT e NCDT precisam caber folgados
  check(rtpt < 500 && ncdt < 500, "velocidade do RTPT e do NCDT");
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "/root/indigo-env/roms/ps1gte";
  const long cases = argc > 2 ? std::atol(argv[2]) : 2000000;
  std::printf("== ps1-tests (vetores do console)\n");
  testPs1Tests(dir);
  std::printf("== diferencial (régua: GTE do Mednafen)\n");
  Ref ref{};
  if (loadRef(dir, ref)) { testDiff(ref, cases); testDivide(ref); }
  else { std::printf("  %s/libgteref.so não encontrada (rode t9_gte.sh)\n", dir.c_str()); check(false, "régua"); }
  std::printf("== save state\n");
  testState();
  std::printf("== velocidade\n");
  testSpeed();
  std::printf("RESULTADO gte: %d verificações, %d falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
