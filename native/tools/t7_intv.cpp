// Testes do Intellivision (tudo montado aqui mesmo, sem BIOS nem jogos de terceiros):
//  (1) CPU CP1610: cada grupo de instruções com resultados e flags conferidos à mão contra o manual da GI
//      (inclusive SDBD, pilha, MVOI, desvios para trás, J/JSR com interrupção, ciclos e interrupções);
//  (2) console com a EXEC e a GROM livres do Índigo e um cartucho de teste próprio (cabeçalho padrão da EXEC,
//      código depois do título): texto da GROM, pilha de cores, quadrados coloridos, GRAM, MOBs (tamanho,
//      espelho, prioridade), colisões MOB-MOB/fundo/borda, janela de acesso ao STIC, pedidos de barramento,
//      controles 1 e 2 (dois jogadores), som (frequência do tom), save state determinístico e estados danificados;
//  (3) formatos: o mesmo cartucho em .rom (Intellicart, com página de RAM), .int e .bin de 10 bits; a EXEC
//      livre como "BIOS do usuário"; detecção sem roubar as ROMs de teste dos outros consoles.
//  (4) .bin + .cfg do jzIntv (mapa, [memattr] de 8/16 bits, [vars]), páginas do ECS, troca de bancos do
//      Intellicart, PAL (50 Hz, quadro de 19968 ciclos) e .cfg de lixo; antes de tudo, a tabela de ciclos
//      das 1024 instruções contra o manual da GI.
// uso: indigo-t7_intv [/root/indigo-env/roms] [pasta-para-png]   (--gerar PASTA grava os cartuchos de teste)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "../src/core/backup.h"
#include "../src/core/core.h"
#include "../src/intv/asm1610.h"
#include "../src/intv/intv.h"

using namespace indigo;
using namespace indigo::intv;

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
void writeFile(const std::string& p, const std::vector<uint8_t>& d) {
  std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

// ------------------------------------------------------------------ (1) CPU
struct Flat : Cp1610Bus {
  std::vector<uint16_t> m = std::vector<uint16_t>(65536, 0);
  uint16_t read(uint16_t a) override { return m[a]; }
  void write(uint16_t a, uint16_t v) override { m[a] = v; }
};

struct Rig {
  Flat bus;
  CP1610 c{bus};
  int cycles = 0;
  explicit Rig(const std::function<void(Asm1610&)>& prog) {
    c.reset();
    Asm1610 a(0x1000);
    prog(a);
    auto w = a.finish();
    for (size_t i = 0; i < w.size(); i++) bus.m[0x1000 + i] = w[i];
  }
  Rig& run(int n = 1) { for (int i = 0; i < n; i++) cycles += c.step(); return *this; }
  std::string flags() const {
    std::string s;
    s += c.S ? 'S' : '-'; s += c.Z ? 'Z' : '-'; s += c.O ? 'O' : '-'; s += c.C ? 'C' : '-';
    return s;
  }
};

std::string hex(int v) { char b[16]; std::snprintf(b, sizeof b, "%04X", v & 0xFFFF); return b; }

// uma instrução de dois registros com flags conferidas
void alu(const char* nome, const std::function<void(Asm1610&)> op, uint16_t r1, uint16_t r2, bool cin,
         uint16_t esperado, const char* flags) {
  Rig g(op);
  g.c.r[1] = r1; g.c.r[2] = r2; g.c.C = cin;
  g.run();
  check(g.c.r[2] == esperado && g.flags() == flags,
        std::string(nome) + " " + hex(r1) + "," + hex(r2) + (cin ? " C=1" : "") + " = " + hex(g.c.r[2]) + " " + g.flags() +
        " (esperado " + hex(esperado) + " " + flags + ")");
}
void sh(const char* nome, const std::function<void(Asm1610&)> op, uint16_t v, bool cin, bool oin, uint16_t esperado,
        const char* flags) {
  Rig g(op);
  g.c.r[0] = v; g.c.C = cin; g.c.O = oin;
  g.run();
  check(g.c.r[0] == esperado && g.flags() == flags && g.c.intr == 0,
        std::string(nome) + " " + hex(v) + " = " + hex(g.c.r[0]) + " " + g.flags() + " (esperado " + hex(esperado) + " " + flags + ")");
}

void cpuTests() {
  using A = Asm1610;
  // aritmética registro-registro: ADDR/SUBR/CMPR (C = sem empréstimo na subtração)
  alu("ADDR", [](A& a) { a.addr(1, 2); }, 0x0001, 0x7FFF, false, 0x8000, "S-O-");
  alu("ADDR", [](A& a) { a.addr(1, 2); }, 0x0001, 0xFFFF, false, 0x0000, "-Z-C");
  alu("ADDR", [](A& a) { a.addr(1, 2); }, 0x8000, 0x8000, false, 0x0000, "-ZOC");
  alu("SUBR", [](A& a) { a.subr(1, 2); }, 0x0003, 0x0005, false, 0x0002, "---C");
  alu("SUBR", [](A& a) { a.subr(1, 2); }, 0x0005, 0x0003, false, 0xFFFE, "S---");
  alu("SUBR", [](A& a) { a.subr(1, 2); }, 0x0001, 0x8000, false, 0x7FFF, "--OC");
  alu("CMPR", [](A& a) { a.cmpr(1, 2); }, 0x0005, 0x0005, false, 0x0005, "-Z-C");
  alu("ANDR", [](A& a) { a.andr(1, 2); }, 0xF0F0, 0x8F0F, true, 0x8000, "S--C");
  alu("XORR", [](A& a) { a.xorr(1, 2); }, 0x1234, 0x1234, false, 0x0000, "-Z--");
  alu("MOVR", [](A& a) { a.movr(1, 2); }, 0x8001, 0x0000, false, 0x8001, "S---");
  // um registro
  alu("INCR", [](A& a) { a.incr(2); }, 0, 0xFFFF, true, 0x0000, "-Z-C");
  alu("DECR", [](A& a) { a.decr(2); }, 0, 0x0000, false, 0xFFFF, "S---");
  alu("COMR", [](A& a) { a.comr(2); }, 0, 0x00FF, false, 0xFF00, "S---");
  alu("NEGR", [](A& a) { a.negr(2); }, 0, 0x0001, false, 0xFFFF, "S---");
  alu("NEGR", [](A& a) { a.negr(2); }, 0, 0x0000, false, 0x0000, "-Z-C");
  alu("NEGR", [](A& a) { a.negr(2); }, 0, 0x8000, false, 0x8000, "S-O-");
  alu("ADCR", [](A& a) { a.adcr(2); }, 0, 0xFFFF, true, 0x0000, "-Z-C");
  alu("ADCR", [](A& a) { a.adcr(2); }, 0, 0x7FFF, true, 0x8000, "S-O-");
  // deslocamentos (S do bit 15 à esquerda, do bit 7 à direita e no SWAP)
  sh("SWAP", [](A& a) { a.swap(0); }, 0x1280, false, false, 0x8012, "----");
  sh("SWAP", [](A& a) { a.swap(0); }, 0x8012, false, false, 0x1280, "S---");
  sh("SWAP,2", [](A& a) { a.swap(0, 2); }, 0x12B4, false, false, 0xB4B4, "S---");
  sh("SLL", [](A& a) { a.sll(0); }, 0x4001, true, false, 0x8002, "S--C");
  sh("SLL,2", [](A& a) { a.sll(0, 2); }, 0x4001, false, false, 0x0004, "----");
  sh("SLLC", [](A& a) { a.sllc(0); }, 0x8001, false, false, 0x0002, "---C");
  sh("SLLC,2", [](A& a) { a.sllc(0, 2); }, 0xC000, false, false, 0x0000, "-ZOC");
  sh("RLC", [](A& a) { a.rlc(0); }, 0x8000, true, false, 0x0001, "---C");
  sh("RLC,2", [](A& a) { a.rlc(0, 2); }, 0x4000, true, false, 0x0002, "--O-");
  sh("SLR", [](A& a) { a.slr(0); }, 0x0100, true, false, 0x0080, "S--C");
  sh("SLR,2", [](A& a) { a.slr(0, 2); }, 0x8000, false, false, 0x2000, "----");
  sh("SAR", [](A& a) { a.sar(0); }, 0x8000, false, false, 0xC000, "----");
  sh("SAR,2", [](A& a) { a.sar(0, 2); }, 0x8200, false, false, 0xE080, "S---");
  sh("RRC", [](A& a) { a.rrc(0); }, 0x0001, true, false, 0x8000, "---C");
  sh("RRC,2", [](A& a) { a.rrc(0, 2); }, 0x0002, true, false, 0x4000, "--O-");
  sh("SARC", [](A& a) { a.sarc(0); }, 0x0003, false, false, 0x0001, "---C");
  sh("SARC,2", [](A& a) { a.sarc(0, 2); }, 0x8003, false, false, 0xE000, "--OC");
  {
    Rig g([](A& a) { a.sll(0); a.sll(0, 2); });
    g.run();
    const int c1 = g.cycles;
    g.run();
    check(c1 == 6 && g.cycles - c1 == 8, "deslocamento: 6 ciclos por 1, 8 por 2");
  }
  // GSWD / RSWD
  {
    Rig g([](A& a) { a.gswd(0); a.rswd(1); });
    g.c.S = true; g.c.O = true; g.c.C = true; g.c.r[1] = 0x0050;
    g.run();
    const uint16_t w = g.c.r[0];
    g.run();
    check(w == 0xB0B0 && g.flags() == "-Z-C", "GSWD = " + hex(w) + " (B0B0); RSWD $50 → " + g.flags());
  }
  // imediato, direto, indireto
  {
    Rig g([](A& a) { a.mvii(0x1234, 1); a.mvo(1, 0x0300); a.mvi(0x0300, 2); a.addi(0x0001, 2); a.xori(0x1235, 2); });
    g.run();
    const int c1 = g.cycles;
    g.run();
    const int c2 = g.cycles - c1;
    g.run();
    const int c3 = g.cycles - c1 - c2;
    g.run(2);
    check(g.c.r[1] == 0x1234 && g.bus.m[0x300] == 0x1234 && g.c.r[2] == 0 && g.c.Z && c1 == 8 && c2 == 11 && c3 == 10,
          "MVII/MVO/MVI/ADDI/XORI e ciclos 8/11/10 (" + std::to_string(c1) + "/" + std::to_string(c2) + "/" + std::to_string(c3) + ")");
  }
  {
    Rig g([](A& a) { a.mviat(4, 0); a.mviat(4, 3); a.mviat(1, 2); });
    g.c.r[4] = 0x400; g.c.r[1] = 0x500;
    g.bus.m[0x400] = 0x11; g.bus.m[0x401] = 0x22; g.bus.m[0x500] = 0x33;
    g.run(3);
    check(g.c.r[0] == 0x11 && g.c.r[3] == 0x22 && g.c.r[4] == 0x402 && g.c.r[2] == 0x33 && g.c.r[1] == 0x500,
          "MVI@ R4 incrementa, MVI@ R1 não");
  }
  {  // pilha: MVO@ R6 pós-incrementa, MVI@ R6 pré-decrementa (11 ciclos)
    Rig g([](A& a) { a.pshr(1); a.pshr(2); a.pulr(3); a.pulr(0); });
    g.c.r[6] = 0x2F0; g.c.r[1] = 0xAAAA; g.c.r[2] = 0x5555;
    g.run(2);
    const int c = g.cycles;
    g.run();
    check(g.bus.m[0x2F0] == 0xAAAA && g.bus.m[0x2F1] == 0x5555 && g.cycles - c == 11, "PSHR (MVO@ R6) e PULR com 11 ciclos");
    g.run();
    check(g.c.r[3] == 0x5555 && g.c.r[0] == 0xAAAA && g.c.r[6] == 0x2F0, "PULR devolve na ordem inversa");
  }
  {  // SDBD: dois bytes, menos significativo primeiro
    Rig g([](A& a) { a.sdbd(); a.dw(0x2B8 | 1); a.dw(0x34); a.dw(0x12); a.sdbd(); a.mviat(4, 2); a.sdbd(); a.mviat(3, 0); a.mvii(7, 5); });
    g.c.r[4] = 0x600; g.bus.m[0x600] = 0x1256; g.bus.m[0x601] = 0x3478;
    g.c.r[3] = 0x700; g.bus.m[0x700] = 0x0099;
    g.run(2);
    const int c = g.cycles;
    g.run(4);
    check(g.c.r[1] == 0x1234 && c == 14 && g.c.r[2] == 0x7856 && g.c.r[4] == 0x602 && g.c.r[0] == 0x9999 && g.c.r[3] == 0x700,
          "SDBD com imediato (" + hex(g.c.r[1]) + ", 4+10 ciclos), @R4 (" + hex(g.c.r[2]) + ") e @R3 (" + hex(g.c.r[0]) + ")");
    g.run();
    check(g.c.r[5] == 7 && g.c.D == 0, "SDBD vale só para a instrução seguinte");
  }
  {  // MVOI escreve na própria instrução
    Rig g([](A& a) { a.dw(0x240 | (7 << 3) | 1); a.dw(0); a.mvii(5, 2); });
    g.c.r[1] = 0xBEEF;
    g.run(2);
    check(g.bus.m[0x1001] == 0xBEEF && g.c.r[2] == 5, "MVOI grava na palavra seguinte e pula");
  }
  // desvios: para frente e para trás
  {
    Rig g([](A& a) {
      a.mvii(3, 1); a.clrr(2);
      a.label("volta"); a.incr(2); a.decr(1); a.br(A::BNEQ, "volta");
      a.b("fim"); a.mvii(0xDEAD, 3); a.label("fim"); a.nop();
    });
    g.run(2 + 9 + 1);
    check(g.c.r[2] == 3 && g.c.r[1] == 0 && g.c.r[3] == 0 && g.bus.m[g.c.r[7]] == 0x034,
          "BNEQ para trás (laço de 3) e B para frente");
  }
  {
    struct Caso { int cond; bool S, Z, O, C, taken; };
    const Caso casos[] = {
      {A::BC, 0, 0, 0, 1, 1}, {A::BNC, 0, 0, 0, 1, 0}, {A::BOV, 0, 0, 1, 0, 1}, {A::BNOV, 0, 0, 1, 0, 0},
      {A::BPL, 1, 0, 0, 0, 0}, {A::BMI, 1, 0, 0, 0, 1}, {A::BEQ, 0, 1, 0, 0, 1}, {A::BNEQ, 0, 1, 0, 0, 0},
      {A::BLT, 1, 0, 0, 0, 1}, {A::BLT, 1, 0, 1, 0, 0}, {A::BGE, 0, 0, 1, 0, 0}, {A::BGE, 1, 0, 1, 0, 1},
      {A::BLE, 0, 1, 0, 0, 1}, {A::BLE, 0, 0, 0, 0, 0}, {A::BGT, 0, 0, 0, 0, 1}, {A::BGT, 1, 0, 0, 0, 0},
      {A::BUSC, 1, 0, 0, 0, 1}, {A::BUSC, 1, 0, 0, 1, 0}, {A::BESC, 0, 0, 0, 0, 1}, {A::NOPP, 0, 0, 0, 0, 0},
    };
    int ok = 0, n = 0;
    for (const Caso& k : casos) {
      Rig g([&](A& a) { a.br(k.cond, "alvo"); a.nop(); a.nop(); a.label("alvo"); a.nop(); });
      g.c.S = k.S; g.c.Z = k.Z; g.c.O = k.O; g.c.C = k.C;
      g.run();
      const bool t = g.c.r[7] == 0x1004;
      n++;
      if (t == k.taken && g.cycles == (k.taken ? 9 : 7)) ok++;
    }
    check(ok == n, "20 condições de desvio com S/Z/O/C e 9/7 ciclos (" + std::to_string(ok) + "/" + std::to_string(n) + ")");
  }
  {  // J / JSR / JSRE / JD e retorno por JR
    Rig g([](A& a) {
      a.jsr(5, "sub", 1); a.mvii(0x77, 0); a.j("fim", 2);
      a.label("sub"); a.mvii(0x55, 1); a.jr(5);
      a.label("fim"); a.nop();
    });
    g.run();
    const bool ok1 = g.c.r[5] == 0x1003 && g.c.r[7] == g.c.r[7] && g.c.I && g.cycles == 12;
    g.run(2);
    const bool ok2 = g.c.r[7] == 0x1003 && g.c.r[1] == 0x55;
    g.run(2);
    check(ok1 && ok2 && !g.c.I && g.c.r[0] == 0x77 && g.bus.m[g.c.r[7]] == 0x034, "JSRE R5 (12 ciclos), JR R5 e JD");
  }
  {  // interrupção: só depois de instrução interrompível e com I = 1
    Rig g([](A& a) { a.eis(); a.mvo(0, 0x300); a.nop(); });
    g.c.r[6] = 0x2F0;
    g.run();
    const bool afterEis = (g.c.intr & 1) == 0;
    g.run();
    const bool afterMvo = (g.c.intr & 3) == 0;
    g.run();
    const bool afterNop = g.c.intr == 3;
    const int cyc = g.c.interrupt();
    check(afterEis && afterMvo && afterNop && g.c.r[7] == 0x1004 && g.bus.m[0x2F0] == 0x1004 && g.c.r[6] == 0x2F1 && cyc == 12,
          "interrupção: não logo depois de EIS/MVO; empilha o PC e vai para $1004");
  }
  {
    Rig g([](A& a) { a.hlt(); a.nop(); });
    g.run(3);
    check(g.c.halted && g.c.r[7] == 0x1001, "HLT para a CPU");
    g.c.r[6] = 0x2F0;
    g.c.interrupt();
    check(!g.c.halted && g.bus.m[0x2F0] == 0x1001, "interrupção depois do HLT empilha o endereço seguinte ao HLT");
  }
}

// ------------------------------------------------------------------ (1a) tabela de ciclos
// Ciclos esperados de cada uma das 1024 instruções, escritos a partir da tabela do manual da GI
// (jzIntv doc/programming/cp1600_summary.txt), e não do código da CPU:
//   HLT, SDBD, EIS, DIS, TCI, CLRC, SETC = 4; J/JSR/JE/JD/JSRE/JSRD = 12;
//   INCR/DECR/COMR/NEGR/ADCR = 6 (7 se o registro é R6 ou R7); GSWD, NOP, SIN, RSWD = 6;
//   deslocamentos = 6 (8 com 2 posições); registro-registro = 6 (7 se o destino é R6 ou R7);
//   desvios = 7 (9 se desviou); MVO = 11 direto, 9 nos outros modos;
//   MVI/ADD/SUB/CMP/AND/XOR = 10 direto, 8 imediato/indireto, 11 pela pilha (@R6), 10 com SDBD
//   (imediato/indireto). Como no jzIntv, +1 quando o destino da leitura é R6 ou R7 (o manual só cita isso
//   nas instruções de registro, mas é a mesma escrita no PC/SP). O jzIntv usa 13 no J; o manual diz 12.
int expectedCycles(uint16_t op, bool sdbd, bool taken) {
  if (op < 0x008) return op == 4 ? 12 : 4;
  if (op < 0x030) return (op & 7) >= 6 ? 7 : 6;
  if (op < 0x040) return 6;
  if (op < 0x080) return (op & 4) ? 8 : 6;
  if (op < 0x200) return (op & 7) >= 6 ? 7 : 6;
  if (op < 0x240) return taken ? 9 : 7;
  const int opc = (op >> 6) & 7, m = (op >> 3) & 7, reg = op & 7;
  if (opc == 1) return m == 0 ? 11 : 9;
  int c = m == 0 ? 10 : m == 6 ? 11 : (sdbd ? 10 : 8);
  if (reg >= 6) c++;
  return c;
}

void cycleTable() {
  int ok = 0, n = 0;
  std::string erros;
  for (int pass = 0; pass < 2; pass++) {
    for (int op = 0; op < 0x400; op++) {
      const bool sdbd = pass == 1;
      if (sdbd && op < 0x240) continue;  // SDBD só muda as instruções de memória
      Flat bus;
      CP1610 c(bus);
      c.reset();
      c.r[6] = 0x2F0; c.r[1] = c.r[2] = c.r[3] = c.r[4] = c.r[5] = 0x400;
      uint16_t pc = 0x1000;
      if (sdbd) bus.m[pc++] = 0x001;
      bus.m[pc] = static_cast<uint16_t>(op);
      bus.m[pc + 1] = 0x0010; bus.m[pc + 2] = 0x0020;
      if (sdbd) c.step();
      const int cyc = c.step();
      // desvio tomado: o PC não caiu logo depois das duas palavras
      const bool taken = op >= 0x200 && op < 0x240 && c.r[7] != pc + 2;
      const int exp = expectedCycles(static_cast<uint16_t>(op), sdbd, taken);
      n++;
      if (cyc == exp) ok++;
      else if (erros.size() < 200) erros += " " + std::string(sdbd ? "SDBD+" : "") + hex(op) + "=" + std::to_string(cyc) + "/" + std::to_string(exp);
    }
  }
  check(ok == n, "tabela de ciclos do manual da GI: " + std::to_string(ok) + "/" + std::to_string(n) + " instruções" + erros);
  // casos citados como suspeitos, um a um
  auto cyc1 = [](std::initializer_list<uint16_t> ws, int steps = 1) {
    Flat bus;
    CP1610 c(bus);
    c.reset();
    c.r[6] = 0x2F0;
    uint16_t a = 0x1000;
    for (uint16_t w : ws) bus.m[a++] = w;
    int t = 0;
    for (int i = 0; i < steps; i++) t = c.step();
    return t;
  };
  check(cyc1({0x004, 0x0110, 0x0000}) == 12 && cyc1({0x004, 0x0310, 0x0000}) == 12, "JSR R5 e J: 12 ciclos (manual da GI)");
  check(cyc1({0x2B0}) == 11 && cyc1({0x2B6}) == 12 && cyc1({0x001, 0x2B0}, 2) == 11, "PULR R0 (MVI@ R6): 11; PULR R6: 12; SDBD não muda @R6");
  check(cyc1({0x2A0}) == 8 && cyc1({0x001, 0x2A0}, 2) == 10 && cyc1({0x2BF, 0x1234}) == 9 && cyc1({0x280, 0x0300}) == 10,
        "MVI@ R4: 8 (10 com SDBD); MVII para R7: 9; MVI direto: 10");
  check(cyc1({0x0B7}) == 7 && cyc1({0x00F}) == 7 && cyc1({0x036}) == 6 && cyc1({0x03F}) == 6, "JR/MOVR para R7 e INCR R7: 7; SIN e RSWD R7: 6");
}

// ------------------------------------------------------------------ (1b) som: envelope e ruído
void psgTests() {
  // envelope: período 16 → um passo a cada 32 ticks (128 ciclos da CPU); 16 passos por rampa
  auto env = [](int shape, std::vector<int> at) {
    PSG p;
    p.reset();
    p.write(3, 0x10, 0);
    p.write(7, 0x00, 0);
    p.write(10, static_cast<uint8_t>(shape), 0);
    std::vector<int> v;
    for (int t : at) { p.run(t); v.push_back(p.envLevel()); }
    return v;
  };
  const std::vector<int> t = {64, 128 * 8 + 64, 128 * 16 + 64, 128 * 24 + 64, 128 * 40 + 64};
  auto a = env(0x00, t);  // \___
  check(a[0] == 15 && a[1] == 7 && a[2] == 0 && a[3] == 0 && a[4] == 0, "envelope \\___ desce de 15 a 0 e fica");
  auto b = env(0x0D, t);  // /¯¯¯ (continua, ataque, segura)
  check(b[0] == 0 && b[1] == 8 && b[2] == 15 && b[3] == 15 && b[4] == 15, "envelope /¯¯¯ sobe e segura no 15");
  auto c = env(0x0E, t);  // /\/\ (continua, ataque, alterna)
  check(c[0] == 0 && c[1] == 8 && c[2] == 15 && c[3] == 7 && c[4] == 8, "envelope /\\/\\ sobe, desce e sobe de novo");
  auto d = env(0x08, t);  // \\\ (continua, repete a descida)
  check(d[0] == 15 && d[1] == 7 && d[3] == 7, "envelope \\\\ repete a descida");
  // ruído no canal A (tom desligado): a saída muda de forma irregular
  {
    PSG p;
    p.reset();
    p.setSampleRate(48000, Intv::CPU_HZ);
    p.write(9, 0x1F, 0);
    p.write(11, 0x0F, 0);
    p.write(8, 0x37, 0);
    p.run(894886 / 4);
    std::vector<float> l(12000), r(12000);
    p.pull(l.data(), r.data(), l.size());
    int changes = 0;
    std::vector<int> runs;
    int run = 0;
    for (size_t i = 1; i < l.size(); i++) {
      if ((l[i] > 0) != (l[i - 1] > 0)) { changes++; runs.push_back(run); run = 0; } else run++;
    }
    int minRun = 1 << 30, maxRun = 0;
    for (int x : runs) { minRun = std::min(minRun, x); maxRun = std::max(maxRun, x); }
    check(changes > 200 && maxRun > 3 * std::max(minRun, 1), "ruído: " + std::to_string(changes) + " trocas irregulares em 1/4 s");
  }
  // volume pelo envelope com os bits 4-5 (inteiro, /2, /4)
  {
    float lv[3];
    for (int k = 0; k < 3; k++) {
      PSG p;
      p.reset();
      p.setSampleRate(48000, Intv::CPU_HZ);
      p.write(0, 0x40, 0);
      p.write(8, 0x3E, 0);
      p.write(3, 0x01, 0);
      p.write(10, 0x0D, 0);  // sobe e segura em 15
      p.write(11, static_cast<uint8_t>(k == 0 ? 0x30 : k == 1 ? 0x20 : 0x10), 0);
      p.run(894886 / 2);
      std::vector<float> l(24000), r(24000);
      p.pull(l.data(), r.data(), l.size());
      float pk = 0;
      for (size_t i = 12000; i < l.size(); i++) pk = std::max(pk, std::fabs(l[i]));
      lv[k] = pk;
    }
    check(lv[0] > 1.8f * lv[1] && lv[1] > 1.8f * lv[2] && lv[2] > 0.01f, "volume do envelope inteiro, /2 e /4 (bits 4-5): " + std::to_string(lv[0]) + " " + std::to_string(lv[1]) + " " + std::to_string(lv[2]));
  }
}

// ------------------------------------------------------------------ (2) cartucho de teste
// RAM usada pelo programa
constexpr uint16_t QUADRO = 0x350, PAD = 0x351, CONTA = 0x352, CONTA_ANT = 0x353, LEU_STIC = 0x354, SEM_TELA = 0x355,
                   COLISOES = 0x340;
// MOBs: X, Y, A de cada um (ordem dos registros do STIC)
const uint16_t MOBS[24] = {
  // X: bit 8 INTR, 9 VISB, 10 largura dupla
  80 | 0x300, 84 | 0x300, 4 | 0x300, 40 | 0x300, 120 | 0x700, 0, 0, 0,
  // Y: bit 7 = 16 linhas, 8-9 = altura, 10 = espelho X, 11 = espelho Y
  60 | 0x100, 62 | 0x100, 40 | 0x100, 8 | 0x100, 70 | 0x200 | 0x800, 0, 0, 0,
  // A: cor (bits 0-2, 12), cartão (3-11; bit 11 = GRAM), bit 13 = atrás do fundo
  0x800 | (1 << 3) | 6, 0x800 | (1 << 3) | 5, 0x800 | (1 << 3) | 3, 0x2000 | 0x800 | (1 << 3) | 2, 0x800 | (3 << 3) | 0x1000 | 2, 0, 0, 0,
};
const uint16_t CORES[5] = {1, 2, 3, 4, 6};  // pilha de cores 0..3 e borda

std::vector<uint16_t> buildCart(uint16_t org, Asm1610* out = nullptr) {
  Asm1610 a(org);
  a.bidecle("zero"); a.bidecle("zero"); a.bidecle("main"); a.bidecle("zero"); a.bidecle("cores"); a.bidecle("titulo");
  a.dw(0x03C0);
  a.label("zero"); a.dw(0); a.dw(0);
  a.label("cores"); for (uint16_t c : CORES) a.dw(c);
  a.label("titulo"); a.dw(126); a.text("TESTE INDIGO");
  // ---- programa (roda dentro da primeira interrupção, como na EXEC)
  a.label("main");
  a.dis();
  a.mvii(0x300, 6);
  a.mvii("isr", 0); a.mvo(0, 0x100); a.swap(0); a.mvo(0, 0x101);
  // GRAM: cartão 0 xadrez, 1 cheio, 3 seta (assimétrica, para os espelhos)
  a.mvii(0x3800, 4);
  a.mvii(8, 1);
  a.label("g0"); a.mvii(0x55, 0); a.mvoat(0, 4); a.mvii(0xAA, 0); a.mvoat(0, 4); a.decr(1); a.decr(1); a.br(Asm1610::BNEQ, "g0");
  a.mvii(8, 1); a.mvii(0xFF, 0);
  a.label("g1"); a.mvoat(0, 4); a.decr(1); a.br(Asm1610::BNEQ, "g1");
  a.mvii(0x3818, 4);
  for (uint16_t v : {0x80, 0xC0, 0xE0, 0xF0, 0x00, 0x00, 0x00, 0x00}) { a.mvii(v, 0); a.mvoat(0, 4); }
  // BACKTAB: linha 0 = título em branco
  a.mvii("titulo", 5); a.incr(5); a.mvii(0x200, 4);
  a.label("t"); a.mviat(5, 0); a.tstr(0); a.br(Asm1610::BEQ, "t_fim"); a.subi(32, 0); a.sll(0, 2); a.sll(0); a.addi(7, 0);
  a.mvoat(0, 4); a.b("t");
  a.label("t_fim");
  // linha 2, coluna 0: quadrados coloridos 1, 2, 3, 4 (o 4 usa o bit 13)
  a.mvii(0x1000 | 0x2000 | 1 | (2 << 3) | (3 << 6), 0); a.mvo(0, 0x200 + 40);
  // linha 3: coluna 0 avança a pilha (cartão 0 da GRAM, cor 5); colunas 1.. ficam com o fundo da pilha 1
  a.mvii(0x2000 | 0x800 | 5, 0); a.mvo(0, 0x200 + 60);
  // som: canal A com período $100 no volume máximo; só o tom A ligado; portas como entrada
  a.mvii(0x00, 0); a.mvo(0, 0x1F0);
  a.mvii(0x01, 0); a.mvo(0, 0x1F4);
  a.mvii(0x0F, 0); a.mvo(0, 0x1FB);
  a.mvii(0x3E, 0); a.mvo(0, 0x1F8);
  a.eis();
  // ---- laço principal: espera o quadro, gasta ~4500 ciclos e tenta mexer no STIC (fora da janela); depois conta
  a.label("laco");
  a.mvi(QUADRO, 2);
  a.label("espera"); a.mvi(QUADRO, 1); a.cmpr(1, 2); a.br(Asm1610::BEQ, "espera");
  a.movr(1, 2);
  a.mvii(300, 1);
  a.label("atraso"); a.decr(1); a.br(Asm1610::BNEQ, "atraso");
  a.mvii(9, 0); a.mvo(0, 0x2C);
  a.mvii(0xFF, 0); a.mvo(0, 0x3810);
  a.label("conta");
  a.mvi(CONTA, 0); a.incr(0); a.mvo(0, CONTA);
  a.mvi(QUADRO, 1); a.cmpr(1, 2); a.br(Asm1610::BEQ, "conta");
  a.movr(1, 2);
  a.b("conta");
  // ---- interrupção
  a.label("isr");
  a.mvi(SEM_TELA, 0); a.tstr(0); a.br(Asm1610::BNEQ, "sem_tela");
  a.mvo(0, 0x20);
  a.label("sem_tela");
  a.mvii(0x18, 4); a.mvii(COLISOES, 3); a.mvii(8, 1);
  a.label("cl"); a.mviat(4, 0); a.mvoat(0, 3); a.incr(3); a.decr(1); a.br(Asm1610::BNEQ, "cl");
  a.clrr(0); a.mvii(0x18, 4); a.mvii(8, 1);
  a.label("cz"); a.mvoat(0, 4); a.decr(1); a.br(Asm1610::BNEQ, "cz");
  a.mvii("mobs", 4); a.clrr(3); a.mvii(24, 1);
  a.label("m"); a.mviat(4, 0); a.mvoat(0, 3); a.incr(3); a.decr(1); a.br(Asm1610::BNEQ, "m");
  a.mvii("cores", 4); a.mvii(0x28, 3); a.mvii(5, 1);
  a.label("c"); a.mviat(4, 0); a.mvoat(0, 3); a.incr(3); a.decr(1); a.br(Asm1610::BNEQ, "c");
  a.mvi(0x28, 0); a.mvo(0, LEU_STIC);
  a.mvi(QUADRO, 0); a.incr(0); a.mvo(0, QUADRO);
  a.mvi(0x1FF, 0); a.mvo(0, PAD);
  a.mvi(CONTA, 0); a.mvo(0, CONTA_ANT);
  a.jr(5);
  a.label("mobs"); for (uint16_t v : MOBS) a.dw(v);
  auto w = a.finish();
  if (out) *out = a;
  return w;
}

std::vector<uint8_t> toBin(const std::vector<uint16_t>& w) {
  std::vector<uint8_t> b;
  for (uint16_t x : w) { b.push_back(static_cast<uint8_t>(x >> 8)); b.push_back(static_cast<uint8_t>(x)); }
  return b;
}

uint16_t crc16(const uint8_t* d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= static_cast<uint16_t>(d[i] << 8);
    for (int k = 0; k < 8; k++) c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
  }
  return c;
}

// .rom do Intellicart: um segmento em $5000 e uma página de RAM de 16 bits em $8000-$80FF (atributos R+W)
std::vector<uint8_t> toRom(const std::vector<uint16_t>& w) {
  std::vector<uint8_t> r = {0xA8, 1, 0xFE};
  const size_t start = r.size();
  const int pages = static_cast<int>((w.size() + 255) / 256);
  r.push_back(0x50);
  r.push_back(static_cast<uint8_t>(0x50 + pages - 1));
  for (int i = 0; i < pages * 256; i++) {
    const uint16_t x = i < static_cast<int>(w.size()) ? w[i] : 0;
    r.push_back(static_cast<uint8_t>(x >> 8));
    r.push_back(static_cast<uint8_t>(x));
  }
  const uint16_t c = crc16(&r[start], r.size() - start);
  r.push_back(static_cast<uint8_t>(c >> 8)); r.push_back(static_cast<uint8_t>(c));
  uint8_t attr[16] = {}, fine[32] = {};
  for (int i = 0; i < 32; i++) fine[(i >> 1) | ((i & 1) << 4)] = 0x07;  // bloco de 2K inteiro
  attr[5] = 0x01 | (0x01 << 4);  // $5000-$5FFF leitura (blocos 10 e 11)
  attr[8] = 0x03;                // $8000-$87FF leitura e escrita (bloco 16)
  fine[8] = 0x00;                // ...só a primeira página de 256 (faixa fina 0..0)
  const size_t t0 = r.size();
  r.insert(r.end(), attr, attr + 16);
  r.insert(r.end(), fine, fine + 32);
  const uint16_t c2 = crc16(&r[t0], 48);
  r.push_back(static_cast<uint8_t>(c2 >> 8)); r.push_back(static_cast<uint8_t>(c2));
  return r;
}

// ------------------------------------------------------------------ PNG e hash
void putBE(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back(static_cast<uint8_t>(x >> (i * 8))); }
uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
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
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
}
uint32_t hashFb(const Core& c) {
  return crc32(reinterpret_cast<const uint8_t*>(c.framebuffer()), static_cast<size_t>(c.width()) * c.height() * 4);
}

// cor (índice do STIC) no ponto do fundo (coluna c, linha r do cartão, ponto px,py) e num ponto da tela
int bgPix(const Intv& m, int col, int row, int px, int py) {
  return m.stic.out[(16 + 2 * (row * 8 + py)) * Stic::OUT_W + 8 + col * 8 + px];
}
int scr(const Intv& m, int sx, int sy) { return m.stic.out[sy * Stic::OUT_W + sx]; }

void consoleTest(const std::string& pngDir) {
  setFirmware("intv_exec", {});
  setFirmware("intv_grom", {});
  const std::vector<uint16_t> words = buildCart(0x5000);
  const std::vector<uint8_t> romFile = toRom(words);
  std::unique_ptr<Core> core = createCore(romFile, "teste.rom", nullptr);
  check(core && core->system() == "intv", "createCore reconhece o .rom do Intellicart");
  if (!core) return;
  auto* ic = static_cast<IntvCore*>(core.get());
  Intv& m = ic->machine();
  check(core->title() == "TESTE INDIGO", "título lido do cabeçalho: \"" + core->title() + "\"");
  check(!core->note().empty(), "sem exec.bin/grom.bin: aviso de que roda com a EXEC livre");
  core->setSampleRate(48000);

  for (int i = 0; i < 3; i++) core->runFrame();  // (o 1º quadro é mais longo: a EXEC zera o atraso vertical no meio dele)
  int64_t t0 = m.now;
  for (int i = 0; i < 30; i++) core->runFrame();
  check(m.now - t0 >= 30 * Intv::FRAME_CYCLES - 20 && m.now - t0 <= 30 * Intv::FRAME_CYCLES + 20,
        "30 quadros = 30 × 14934 ciclos (" + std::to_string(m.now - t0) + ")");
  const uint16_t q0 = m.sysram[QUADRO - 0x200];
  core->runFrame();
  check(m.sysram[QUADRO - 0x200] == q0 + 1 && m.displayOn, "uma interrupção por quadro e tela ligada pelo programa");
  if (!pngDir.empty()) writePng(pngDir + "/intv_teste.png", core->framebuffer(), core->width(), core->height());

  // texto da GROM livre: "T" na coluna 0 (linha 0 do glifo = 11111 nas colunas 1..5), em branco sobre azul
  const std::vector<uint8_t> grom = freeGrom();
  bool txt = true;
  for (int c = 0; c < 12 && txt; c++) {
    if (c == 4) continue;  // o MOB 3 está por cima desta letra
    const char ch = "TESTE INDIGO"[c];
    for (int py = 0; py < 8; py++)
      for (int px = 0; px < 8; px++) {
        const bool on = grom[(ch - 32) * 8 + py] & (0x80 >> px);
        if (bgPix(m, c, 0, px, py) != (on ? 7 : 1)) txt = false;
      }
  }
  check(txt, "texto do título no BACKTAB com a fonte da GROM (branco sobre a pilha de cores 0, azul)");
  // quadrados coloridos
  check(bgPix(m, 0, 2, 1, 1) == 1 && bgPix(m, 0, 2, 5, 1) == 2 && bgPix(m, 0, 2, 1, 5) == 3 && bgPix(m, 0, 2, 5, 5) == 4,
        "quadrados coloridos (1, 2, 3 e 4 com o bit 13)");
  // pilha de cores: linha 3 avança para a cor 1 da pilha (vermelho); cartão 0 da GRAM (xadrez) na cor 5
  check(bgPix(m, 0, 3, 1, 0) == 5 && bgPix(m, 0, 3, 0, 0) == 2 && bgPix(m, 5, 3, 3, 3) == 2 && bgPix(m, 5, 4, 3, 3) == 2,
        "avanço da pilha de cores e cartão da GRAM");
  // borda (cor 6) e coluna 159 em cor de borda
  check(scr(m, 2, 100) == 6 && scr(m, 170, 100) == 6 && scr(m, 100, 4) == 6 && scr(m, 167, 100) == 6,
        "borda na cor do registro $2C (a escrita fora da janela de acesso foi ignorada)");
  check(m.stic.gmem[0x810] == 0, "escrita na GRAM fora da janela foi ignorada");
  check(m.sysram[LEU_STIC - 0x200] == (0x3FF0 | CORES[0]), "leitura de registro do STIC na janela: bits que não existem em 1 (" + hex(m.sysram[LEU_STIC - 0x200]) + ")");
  // MOB 0 (amarelo) por cima do MOB 1 (verde); 8 pontos × 4 meias linhas por linha... altura dupla (bit 8): 16 meias linhas
  check(scr(m, 80, 121) == 6 && scr(m, 86, 135) == 6 && scr(m, 89, 135) == 5 && scr(m, 90, 139) == 5 && scr(m, 80, 136) != 6,
        "MOB 0 por cima do MOB 1, altura dupla (8 linhas × 2 meias linhas)");
  // MOB 3 atrás do fundo (bit 13): sobre o texto aparecem os pontos do texto, nos vãos aparece o MOB
  {
    int behind = 0, shown = 0;
    for (int py = 0; py < 8; py++)
      for (int px = 0; px < 8; px++) {
        const int sx = 40 + px, sy = 16 + 2 * py;  // MOB em x=40 (coluna 4 do fundo), y=8
        const int c = scr(m, sx, sy);
        const bool fg = grom[('E' - 32) * 8 + py] & (0x80 >> px);  // coluna 4 = 'E'
        if (fg && c == 7) behind++;
        if (!fg && c == 2) shown++;
      }
    check(behind > 5 && shown > 20, "prioridade: MOB atrás dos pontos do texto e na frente do fundo (" +
          std::to_string(behind) + "/" + std::to_string(shown) + ")");
  }
  // MOB 4: largura dupla, altura x4, espelho em Y, cor 10 (laranja), cartão da seta
  {
    // seta (linhas 0..3 = 80,C0,E0,F0; 4..7 vazias) espelhada em Y: aparece embaixo, com a linha cheia (F0)
    // na linha 4 e a ponta (80) na linha 7; cada linha do cartão = 4 meias linhas, cada ponto = 2 de largura
    const int x = 120, y = 140;
    const bool topRow = scr(m, x, y + 16) == 10 && scr(m, x + 7, y + 16) == 10 && scr(m, x + 8, y + 16) != 10 && scr(m, x, y) != 10;
    const bool lastLines = scr(m, x, y + 28) == 10 && scr(m, x + 1, y + 28) == 10 && scr(m, x + 2, y + 28) != 10;
    check(topRow && lastLines, "MOB com largura dupla, altura ×4 e espelho vertical");
  }
  // colisões (o programa copia os registros $18-$1F para a RAM a cada quadro)
  const uint16_t* col = &m.sysram[COLISOES - 0x200];
  check((col[0] & 0x3FF) == 0x002 && (col[1] & 0x3FF) == 0x001, "colisão MOB 0 × MOB 1 (" + hex(col[0]) + ", " + hex(col[1]) + ")");
  check((col[2] & 0x200) != 0 && (col[0] & 0x200) == 0, "colisão do MOB 2 com a borda esquerda (" + hex(col[2]) + ")");
  check((col[3] & 0x100) != 0 && (col[0] & 0x100) == 0, "colisão do MOB 3 com o fundo (" + hex(col[3]) + ")");
  check((col[4] & 0x3FF) == 0, "MOB 4 sem colisões (" + hex(col[4]) + ")");

  // controle: disco para cima e botão de cima
  core->setKeys(KEY_UP | KEY_X);
  core->runFrame(); core->runFrame();
  check((m.sysram[PAD - 0x200] & 0xFF) == (static_cast<uint8_t>(~(0x04 | 0xA0))), "controle esquerdo em $1FF (disco N + botão de cima) = " + hex(m.sysram[PAD - 0x200]));
  ic->setKeypad(5);
  core->setKeys(0);
  core->runFrame(); core->runFrame();
  check((m.sysram[PAD - 0x200] & 0xFF) == static_cast<uint8_t>(~0x42), "teclado: tecla 5 = $42 (" + hex(m.sysram[PAD - 0x200]) + ")");
  ic->setKeypad(-1);
  // teclado numérico pelos bits KEY_KP* (teclado do PC e teclado na tela de toque do app)
  core->setKeys(KEY_KP0 << 7);
  core->runFrame(); core->runFrame();
  check((m.sysram[PAD - 0x200] & 0xFF) == static_cast<uint8_t>(~0x84), "teclado pelos bits KEY_KP*: tecla 7 = $84 (" + hex(m.sysram[PAD - 0x200]) + ")");
  core->setKeys(KEY_KPSTAR);
  core->runFrame(); core->runFrame();
  const uint16_t clr = m.sysram[PAD - 0x200];
  core->setKeys(KEY_KPHASH);
  core->runFrame(); core->runFrame();
  check((clr & 0xFF) == static_cast<uint8_t>(~0x88) && (m.sysram[PAD - 0x200] & 0xFF) == static_cast<uint8_t>(~0x28),
        "teclado: * = Clear ($88) e # = Enter ($28) (" + hex(clr) + ", " + hex(m.sysram[PAD - 0x200]) + ")");
  check(IntvCore::keypadFromKeys(KEY_A | KEY_START) == -1 && IntvCore::keypadFromKeys(KEY_KP0) == 0 &&
            IntvCore::keypadFromKeys((KEY_KP0 << 9) | KEY_KPHASH) == 9,
        "teclado: sem bits KEY_KP* nenhuma tecla; o algarismo tem prioridade");
  ic->setKeypad(4);
  core->setKeys(KEY_KP0 << 7);
  core->runFrame(); core->runFrame();
  check((m.sysram[PAD - 0x200] & 0xFF) == static_cast<uint8_t>(~0x82), "setKeypad() tem prioridade sobre os bits KEY_KP* (" + hex(m.sysram[PAD - 0x200]) + ")");
  ic->setKeypad(-1);
  core->setKeys(0);
  // dois jogadores: o controle 2 é o da direita (o da esquerda com os controles trocados)
  check(core->players() == 2, "2 jogadores: players() == 2");
  core->setKeys(KEY_A);
  core->setKeys2(KEY_UP | KEY_B);
  check(m.padL == 0xC0 && m.padR == 0x64, "2 jogadores: controle 1 à esquerda e 2 à direita");
  ic->setSwapControllers(true);
  check(m.padL == 0x64 && m.padR == 0xC0, "2 jogadores: controles trocados");
  ic->setSwapControllers(false);
  core->setKeys(0);
  core->setKeys2(0);
  check(m.padL == 0 && m.padR == 0, "2 jogadores: os dois soltos");

  // som: período $100 → 111860,8 / 256 = 437 Hz
  {
    std::vector<float> l(4096), r(4096);
    while (core->audioAvailable()) core->pullAudio(l.data(), r.data(), std::min<size_t>(4096, core->audioAvailable()));
    std::vector<float> all;
    for (int i = 0; i < 60; i++) {
      core->runFrame();
      const size_t n = core->audioAvailable();
      core->pullAudio(l.data(), r.data(), std::min<size_t>(n, 4096));
      all.insert(all.end(), l.begin(), l.begin() + static_cast<long>(std::min<size_t>(n, 4096)));
    }
    int up = 0;
    float peak = 0;
    for (size_t i = 1; i < all.size(); i++) { if (all[i - 1] < 0 && all[i] >= 0) up++; peak = std::max(peak, std::fabs(all[i])); }
    const double secs = all.size() / 48000.0, f = up / secs;
    check(std::fabs(f - 436.97) < 6 && peak > 0.2, "tom do AY-3-8914: " + std::to_string(static_cast<int>(f)) + " Hz (esperado 437), " +
          std::to_string(all.size()) + " amostras");
  }

  // pedidos de barramento: com a tela ligada a CPU perde ~1400 ciclos por quadro (12 fileiras de 108 + 68 + 38)
  {
    // o programa guarda o contador do laço a cada interrupção; a diferença entre dois quadros = voltas
    auto perFrame = [&] {
      core->runFrame();
      const uint16_t a0 = m.sysram[CONTA_ANT - 0x200];
      core->runFrame();
      return static_cast<uint16_t>(m.sysram[CONTA_ANT - 0x200] - a0);
    };
    for (int i = 0; i < 3; i++) core->runFrame();
    const int on = perFrame();
    m.sysram[SEM_TELA - 0x200] = 1;
    for (int i = 0; i < 3; i++) core->runFrame();
    const int off = perFrame();
    check(!m.displayOn && off - on >= 20 && off - on <= 34,
          "tela desligada sem $20; voltas do laço (52 ciclos) por quadro: " + std::to_string(on) + " com tela, " + std::to_string(off) +
          " sem (diferença esperada ~27)");
    m.sysram[SEM_TELA - 0x200] = 0;
    for (int i = 0; i < 3; i++) core->runFrame();
    check(m.displayOn, "tela volta quando o programa escreve em $20 de novo");
  }

  // save state: determinístico e salvar->carregar->salvar idêntico
  {
    core->setKeys(KEY_RIGHT);
    for (int i = 0; i < 5; i++) core->runFrame();
    const std::vector<uint8_t> st = core->saveState();
    const uint32_t h0 = hashFb(*core);
    for (int i = 0; i < 20; i++) core->runFrame();
    const uint32_t h1 = hashFb(*core);
    const uint16_t q1 = m.sysram[QUADRO - 0x200];
    std::memset(m.stic.out, 7, sizeof m.stic.out);  // imagem diferente da salva
    core->loadState(st);
    check(hashFb(*core) == h0, "logo depois de carregar, a imagem é a do momento do salvamento");
    core->loadState(st);
    const std::vector<uint8_t> st2 = core->saveState();
    for (int i = 0; i < 20; i++) core->runFrame();
    check(hashFb(*core) == h1 && m.sysram[QUADRO - 0x200] == q1, "save state: 20 quadros depois de carregar, a mesma tela");
    check(st == st2, "salvar → carregar → salvar dá o mesmo estado (" + std::to_string(st.size()) + " bytes)");
    std::vector<uint8_t> bad(st.begin(), st.begin() + static_cast<long>(st.size() / 2));
    const std::vector<uint8_t> before = core->saveState();
    bool threw = false;
    try { core->loadState(bad); } catch (const std::exception&) { threw = true; }
    std::vector<uint8_t> junk = st;
    junk[0] ^= 0xFF;
    bool threw2 = false;
    try { core->loadState(junk); } catch (const std::exception&) { threw2 = true; }
    check(threw && threw2 && core->saveState() == before, "estado truncado ou de outro console: erro e nada muda");
    // estado com valores absurdos (bytes trocados no meio): carrega sem travar
    std::vector<uint8_t> noisy = st;
    for (size_t i = 8; i < noisy.size(); i += 7) noisy[i] = static_cast<uint8_t>(noisy[i] * 31 + 7);
    bool ok = true;
    try { core->loadState(noisy); for (int i = 0; i < 10; i++) core->runFrame(); } catch (const std::exception&) { ok = true; }
    check(ok, "estado com bytes trocados não trava o núcleo");
    // estado feito de propósito: pedidos de interrupção/barramento com prazo 2^60 (o fim do estado é
    // intrq_, busrq_, intrqUntil_, busrqUntil_, padL, padR)
    {
      std::vector<uint8_t> evil = st;
      const size_t e = evil.size();
      const int64_t far = int64_t(1) << 60;
      evil[e - 20] = 1;
      evil[e - 19] = 1;
      std::memcpy(&evil[e - 18], &far, 8);
      std::memcpy(&evil[e - 10], &far, 8);
      core->loadState(evil);
      const int64_t t0 = m.now;
      const auto c0 = std::chrono::steady_clock::now();
      for (int i = 0; i < 5; i++) core->runFrame();
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
      check(secs < 2.0 && m.now - t0 <= 5 * 2 * Intv::FRAME_CYCLES,
            "estado com prazos de barramento absurdos: 5 quadros em " + std::to_string(secs) + " s, sem pular o relógio");
    }
  }
}

// ------------------------------------------------------------------ (3) formatos, BIOS e detecção
void formatTests(const std::string& romsDir) {
  setFirmware("intv_exec", {});
  setFirmware("intv_grom", {});
  const std::vector<uint16_t> words = buildCart(0x5000);
  std::unique_ptr<Core> a = createCore(toRom(words), "teste.rom", nullptr);
  std::unique_ptr<Core> b = createCore(toBin(words), "teste.int", nullptr);
  for (int i = 0; i < 40; i++) { a->runFrame(); b->runFrame(); }
  check(b && b->system() == "intv" && hashFb(*a) == hashFb(*b), "o mesmo programa em .int (mapa padrão) dá a mesma tela do .rom");
  // RAM de 16 bits do .rom em $8000
  {
    auto& m = static_cast<IntvCore*>(a.get())->machine();
    m.write(0x8010, 0xBEEF);
    m.write(0x8110, 0xBEEF);  // fora da faixa fina: não é RAM
    check(m.read(0x8010) == 0xBEEF && m.read(0x8110) == 0xFFFF && m.read(0x5000) == words[0],
          ".rom: página de RAM pela tabela de atributos (faixa fina) e ROM em $5000");
  }
  // .bin de 10 bits: um cartucho só com palavras < $400 (detectado como Intellivision; e só ele)
  {
    Asm1610 p(0x5000);
    p.bidecle("z"); p.bidecle("z"); p.bidecle("m"); p.bidecle("z"); p.bidecle("z"); p.bidecle("t");
    p.dw(0x03C0);
    p.label("z"); p.dw(0); p.dw(0); p.dw(1); p.dw(1); p.dw(1); p.dw(1); p.dw(1);
    p.label("t"); p.dw(100); p.text("DEZ BITS");
    p.label("m"); p.mvii(0x3A, 0); p.mvo(0, 0x200); p.label("x"); p.b("x");
    std::vector<uint16_t> w = p.finish();
    w.resize(0x1000, 0x034);  // NOP até completar 8 KB
    bool tenBit = true;
    for (uint16_t x : w) if (x >= 0x400) tenBit = false;
    const std::vector<uint8_t> bin = toBin(w);
    bool refused = false;
    std::string msg;
    try { createCore(bin, "dez.bin", nullptr); } catch (const std::exception& e) { refused = true; msg = e.what(); }
    check(refused && msg.find("exec.bin") != std::string::npos, "cartucho de 10 bits (jogo da época) sem a BIOS: pede o exec.bin e o grom.bin");
    std::vector<uint8_t> execBytes;
    for (uint16_t x : freeExec()) { execBytes.push_back(static_cast<uint8_t>(x >> 8)); execBytes.push_back(static_cast<uint8_t>(x)); }
    setFirmware("intv_exec", execBytes);  // a EXEC livre faz o papel da BIOS do usuário
    setFirmware("intv_grom", freeGrom());
    std::unique_ptr<Core> c = createCore(bin, "dez.bin", nullptr);
    setFirmware("intv_exec", {});
    setFirmware("intv_grom", {});
    if (c) for (int i = 0; i < 5; i++) c->runFrame();
    check(tenBit && c && c->system() == "intv" && static_cast<IntvCore*>(c.get())->machine().sysram[0] == 0x3A,
          ".bin de 10 bits reconhecido e rodando (título \"" + (c ? c->title() : std::string()) + "\")");
    check(!isIntvRom(std::vector<uint8_t>(8192, 0xEA), "a.bin"), ".bin de outro console não é Intellivision");
    // .bin de 16 bits (como os do IntyBASIC): reconhecido pelo cabeçalho e pelo título legível
    std::vector<uint16_t> w16 = words;
    w16.resize(0x1000, 0);
    std::unique_ptr<Core> d = createCore(toBin(w16), "dezesseis.bin", nullptr);
    check(d && d->system() == "intv" && d->title() == "TESTE INDIGO", ".bin de 16 bits com cabeçalho e título");
  }
  // BIOS do usuário: a EXEC/GROM abertas no app não viram jogo; com elas instaladas, o aviso some
  {
    std::vector<uint8_t> exec;
    for (uint16_t x : freeExec()) { exec.push_back(static_cast<uint8_t>(x >> 8)); exec.push_back(static_cast<uint8_t>(x)); }
    const std::vector<uint8_t> grom = freeGrom();
    check(intvBiosKind(exec, "exec.bin") == 1 && intvBiosKind(grom, "grom.bin") == 2 && !createCore(exec, "exec.bin", nullptr) &&
              !createCore(grom, "grom.bin", nullptr),
          "exec.bin e grom.bin são reconhecidos como BIOS (não como jogo)");
    check(intvBiosKind(exec, "intv_exec.bin") == 1 && intvBiosKind(grom, "GROM (1).bin") == 2 &&
              !intvBiosKind(exec, "Executioner.bin") && !intvBiosKind(grom, "progromania.bin"),
          "BIOS pelo nome só com \"exec\"/\"grom\" como palavra solta (Executioner.bin não é BIOS)");
    setFirmware("intv_exec", exec);
    setFirmware("intv_grom", grom);
    std::unique_ptr<Core> c = createCore(toRom(words), "teste.rom", nullptr);
    for (int i = 0; i < 40; i++) c->runFrame();
    check(c->note().empty() && hashFb(*c) == hashFb(*a), "com exec.bin/grom.bin do usuário: sem aviso e a mesma tela");
    setFirmware("intv_exec", {});
    setFirmware("intv_grom", {});
  }
  // sem cartucho válido
  {
    bool threw = false;
    try { IntvCore x(std::vector<uint8_t>(3, 0), "x.int"); } catch (const std::exception&) { threw = true; }
    check(threw, "cartucho de tamanho ímpar: erro claro");
  }
  // ROMs de teste dos outros consoles: nenhuma vira Intellivision
  if (!romsDir.empty() && std::filesystem::is_directory(romsDir)) {
    int total = 0, stolen = 0;
    std::string first;
    for (const auto& e : std::filesystem::recursive_directory_iterator(romsDir, std::filesystem::directory_options::skip_permission_denied)) {
      if (!e.is_regular_file() || e.file_size() > 4u * 1024 * 1024) continue;
      const std::string p = e.path().string();
      if (p.find("/intv/") != std::string::npos) continue;
      std::string n = e.path().filename().string();
      for (char& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      const std::vector<uint8_t> d = readFile(p);
      total++;
      if (isIntvRom(d, n) || intvBiosKind(d, n)) { stolen++; if (first.empty()) first = p; }
    }
    check(total > 100 && stolen == 0, "detecção: " + std::to_string(total) + " arquivos de teste dos outros consoles, " +
          std::to_string(stolen) + " confundidos com Intellivision " + first);
  } else {
    std::printf("pulei: pasta de ROMs dos outros consoles não existe\n");
  }
}

// ------------------------------------------------------------------ (4) .cfg, páginas do ECS, bancos, PAL
std::unique_ptr<Core> withCfg(const std::vector<uint8_t>& bin, const std::string& name, const std::string& cfg) {
  setFirmware("intv_cfg", std::vector<uint8_t>(cfg.begin(), cfg.end()));
  std::unique_ptr<Core> c;
  try { c = createCore(bin, name, nullptr); } catch (...) {}
  setFirmware("intv_cfg", {});
  return c;
}

void cfgTests() {
  setFirmware("intv_exec", {});
  setFirmware("intv_grom", {});
  const std::vector<uint16_t> words = buildCart(0x5000);
  std::unique_ptr<Core> ref = createCore(toRom(words), "teste.rom", nullptr);
  for (int i = 0; i < 40; i++) ref->runFrame();
  // (a) .bin com 256 palavras de lixo antes do programa: só abre certo com o .cfg
  {
    std::vector<uint16_t> w(0x100, 0x1234);
    w.insert(w.end(), words.begin(), words.end());
    const std::vector<uint8_t> bin = toBin(w);
    char m[160];
    std::snprintf(m, sizeof m, "; teste\n[mapping]\n$0100 - $%04X = $5000   ; programa\n\n[vars]\nname = \"Teste do CFG\"\n[keys]\nlixo = 1\n",
                  static_cast<unsigned>(w.size() - 1));
    const bool semCfg = isIntvRom(bin, "reloc.bin");
    auto c = withCfg(bin, "reloc.bin", m);
    bool ok = c && c->system() == "intv";
    if (ok) for (int i = 0; i < 40; i++) c->runFrame();
    check(!semCfg && ok && hashFb(*c) == hashFb(*ref), ".bin com o mapa no .cfg ($0100 do arquivo em $5000): mesma tela do .rom; sem o .cfg não é reconhecido");
    check(ok && c->title() == "Teste do CFG", "título do [vars] do .cfg: \"" + (c ? c->title() : std::string()) + "\"");
    setFirmware("intv_cfg", std::vector<uint8_t>(m, m + std::strlen(m)));
    const bool viaRom = createCore(toRom(words), "teste.rom", nullptr) != nullptr && intvTitle(toRom(words), "teste.rom") == "TESTE INDIGO";
    setFirmware("intv_cfg", {});
    check(viaRom, "o .cfg só vale para .bin/.int/.itv (um .rom ignora)");
  }
  // (b) [memattr]: RAM de 8 e 16 bits, WOM; RAM começa zerada; escrita fora da faixa não pega
  {
    auto c = withCfg(toBin(words), "attr.int",
                     "[mapping]\n$0000 - $0FFF = $5000\n[memattr]\n$D000 - $D0FF = RAM 8\n$C040 - $C0FF = RAM 16\n$9000 - $90FF = WOM 16\n");
    bool ok = c != nullptr;
    if (ok) {
      auto& m = static_cast<IntvCore*>(c.get())->machine();
      const bool zero = m.read(0xD000) == 0 && m.read(0xC050) == 0;
      m.write(0xD010, 0x1234); m.write(0xC050, 0x1234); m.write(0xD110, 0x1234); m.write(0x9010, 0x5555);
      ok = zero && m.read(0xD010) == 0x34 && m.read(0xC050) == 0x1234 && m.read(0xD110) == 0xFFFF && m.read(0x9010) == 0xFFFF &&
           m.read(0xC03F) == 0xFFFF;
    }
    check(ok, "[memattr]: RAM 8 guarda só o byte de baixo, RAM 16 inteira (a partir de $C040, sem cobrir o espelho do STIC), WOM não é lida");
  }
  // (c) páginas do ECS: duas páginas em $A000, uma página de RAM em $B000; o programa troca com $xA5y em $xFFF
  {
    Asm1610 p(0x5000);
    p.bidecle("z"); p.bidecle("z"); p.bidecle("m"); p.bidecle("z"); p.bidecle("z"); p.bidecle("t");
    p.dw(0x03C0);
    p.label("z"); p.dw(0); p.dw(0); p.dw(1); p.dw(1); p.dw(1); p.dw(1); p.dw(1);
    p.label("t"); p.dw(100); p.text("PAGINAS");
    p.label("m");
    p.mvi(0xA000, 0); p.mvo(0, 0x300);                       // página 0
    p.mvii(0xAA51, 0); p.mvo(0, 0xAFFF);                      // vira para a página 1
    p.mvi(0xA000, 0); p.mvo(0, 0x301);
    p.mvii(0xBA50, 0); p.mvo(0, 0xAFFF);                      // segmento errado: não muda
    p.mvi(0xA000, 0); p.mvo(0, 0x302);
    p.mvii(0xBA51, 0); p.mvo(0, 0xBFFF);                      // RAM da página 1 de $B000
    p.mvii(0x4321, 0); p.mvo(0, 0xB000);
    p.mvii(0xBA50, 0); p.mvo(0, 0xBFFF);                      // volta à página 0 de $B000 (sem nada)
    p.mvi(0xB000, 0); p.mvo(0, 0x303);
    p.mvii(0xBA51, 0); p.mvo(0, 0xBFFF);
    p.mvi(0xB000, 0); p.mvo(0, 0x304);
    p.mvii(0xAA50, 0); p.mvo(0, 0xAFFF);
    p.mvi(0xA000, 0); p.mvo(0, 0x305);
    p.label("x"); p.b("x");
    std::vector<uint16_t> w = p.finish();
    const size_t prog = w.size();
    w.resize(0x1000, 0);
    std::vector<uint16_t> pg0(0x100, 0x0A00), pg1(0x100, 0x0A01);
    w.insert(w.end(), pg0.begin(), pg0.end());
    w.insert(w.end(), pg1.begin(), pg1.end());
    char m[256];
    std::snprintf(m, sizeof m,
                  "[mapping]\n$0000 - $%04X = $5000\n$1000 - $10FF = $A000 PAGE 0\n$1100 - $11FF = $A000 PAGE 1\n"
                  "[memattr]\n$B000 - $B0FF = RAM 16 PAGE 1\n",
                  static_cast<unsigned>(prog - 1));
    auto c = withCfg(toBin(w), "paginas.bin", m);
    bool ok = c != nullptr;
    std::string got;
    if (ok) {
      for (int i = 0; i < 10; i++) c->runFrame();
      auto& mm = static_cast<IntvCore*>(c.get())->machine();
      for (int i = 0; i < 6; i++) got += " " + hex(mm.sysram[0x100 + i]);
      ok = mm.sysram[0x100] == 0x0A00 && mm.sysram[0x101] == 0x0A01 && mm.sysram[0x102] == 0x0A01 && mm.sysram[0x103] == 0xFFFF &&
           mm.sysram[0x104] == 0x4321 && mm.sysram[0x105] == 0x0A00 && mm.page(0xA) == 0 && mm.page(0xB) == 1;
      // save state guarda a página e a RAM das páginas
      const std::vector<uint8_t> st = c->saveState();
      mm.write(0xBFFF, 0xBA50);
      mm.write(0xAFFF, 0xAA51);
      const bool mudou = mm.page(0xA) == 1 && mm.read(0xB000) == 0xFFFF;
      c->loadState(st);
      const bool volta = mm.page(0xA) == 0 && mm.page(0xB) == 1 && mm.read(0xB000) == 0x4321 && mm.read(0xA000) == 0x0A00;
      check(mudou && volta && c->saveState() == st, "save state guarda a página de cada segmento e a RAM das páginas");
      c->reset();
      for (int i = 0; i < 10; i++) c->runFrame();
      check(mm.sysram[0x104] == 0x4321 && mm.page(0xB) == 1, "reset volta às páginas 0 e o programa refaz tudo igual");
    }
    check(ok, "páginas do ECS ($xA5y em $xFFF): A000 p0/p1, segmento errado ignorado, RAM só na página 1 de $B000:" + got);
  }
  // (d) troca de bancos do Intellicart ([bankswitch] e registros $40-$5F)
  {
    std::vector<uint16_t> w = words;
    w.resize(0x1000, 0);
    for (int i = 0; i < 0x800; i++) w.push_back(static_cast<uint16_t>(0x6000 + i));  // vista direta de $6000
    for (int i = 0; i < 0x100; i++) w.push_back(static_cast<uint16_t>(0x9000 + i));  // vai para $9000 do Intellicart
    auto c = withCfg(toBin(w), "bancos.bin",
                     "[mapping]\n$0000 - $0FFF = $5000\n$1000 - $17FF = $6000\n[preload]\n$1800 - $18FF = $9000\n"
                     "[bankswitch]\n$6000 - $67FF\n[memattr]\n$8800 - $8FFF = RAM 16\n[bankswitch]\n$8800 - $8FFF\n");
    bool ok = c != nullptr;
    if (ok) {
      auto& m = static_cast<IntvCore*>(c.get())->machine();
      const bool direto = m.read(0x6010) == 0x6010 && m.read(0x9010) == 0xFFFF;  // preload não fica legível
      m.write(0x0046, 0x90);
      const bool banco = m.read(0x6010) == 0x9010 && m.read(0x6100) == 0xFFFF;  // $9100 não tem dados
      m.write(0x0046, 0x60);
      const bool volta = m.read(0x6010) == 0x6010;
      m.write(0x0058 + 0, 0x20);  // $8800-$8FFF (registro $58) aponta para $2000 do Intellicart
      m.write(0x8805, 0x7777);
      m.write(0x0058, 0x88);
      const bool ram = m.read(0x8805) == 0 && (m.write(0x0058, 0x20), m.read(0x8805) == 0x7777);
      const std::vector<uint8_t> st = c->saveState();
      m.write(0x8805, 0x1111); m.write(0x0046, 0x90);
      c->loadState(st);
      ok = direto && banco && volta && ram && m.read(0x8805) == 0x7777 && m.read(0x6010) == 0x6010;
    }
    check(ok, "troca de bancos do Intellicart: $46 = $90 leva $6000 para $9000; RAM com banco e save state");
  }
  // (d2) .rom com o bit de banco (8) na tabela de atributos
  {
    std::vector<uint16_t> w = words;
    w.resize(0x2000, 0);
    for (int i = 0; i < 0x1000; i++) w[0x1000 + i] = static_cast<uint16_t>(0x6000 + i);
    std::vector<uint8_t> r = toRom(w);
    const size_t t0 = r.size() - 50;
    r[t0 + 6] |= 0x09;  // bloco 12 ($6000-$67FF): leitura e banco
    const uint16_t c2 = crc16(&r[t0], 48);
    r[t0 + 48] = static_cast<uint8_t>(c2 >> 8); r[t0 + 49] = static_cast<uint8_t>(c2);
    auto c = createCore(r, "banco.rom", nullptr);
    bool ok = c != nullptr;
    if (ok) {
      auto& m = static_cast<IntvCore*>(c.get())->machine();
      const bool direto = m.read(0x6010) == 0x6010 && m.read(0x6810) == 0x6810;
      m.write(0x0046, 0x50);
      ok = direto && m.read(0x6010) == words[0x10] && m.read(0x6810) == 0x6810;  // $6800 é outro bloco (registro $56)
    }
    check(ok, ".rom com bloco de banco: $46 = $50 mostra $5000 do Intellicart em $6000; $6800 não muda");
  }
  // (e) PAL: 19968 ciclos por quadro, 50 Hz, a mesma imagem e um retraço mais longo
  {
    std::unique_ptr<Core> pal = createCore(toRom(words), "teste (pal).rom", nullptr);
    auto& m = static_cast<IntvCore*>(pal.get())->machine();
    auto& n = static_cast<IntvCore*>(ref.get())->machine();
    for (int i = 0; i < 10; i++) pal->runFrame();
    const int64_t t0 = m.now;
    const uint16_t q0 = m.sysram[QUADRO - 0x200];
    for (int i = 0; i < 30; i++) pal->runFrame();
    const int64_t dt = m.now - t0;
    check(m.pal() && !n.pal() && std::fabs(pal->fps() - 50.08) < 0.01 && dt >= 30 * Intv::FRAME_CYCLES_PAL - 20 &&
              dt <= 30 * Intv::FRAME_CYCLES_PAL + 20 && m.sysram[QUADRO - 0x200] == q0 + 30,
          "PAL pelo nome: " + std::to_string(pal->fps()) + " Hz, 30 quadros = " + std::to_string(dt) + " ciclos, uma interrupção por quadro");
    for (int i = 0; i < 10; i++) ref->runFrame();
    check(hashFb(*pal) == hashFb(*ref) && !pal->note().empty(), "PAL: mesma imagem do NTSC e aviso no note()");
    // o laço do programa conta mais por quadro no PAL (quadro de 19968 ciclos contra 14934)
    const uint16_t cp = m.sysram[CONTA - 0x200], cn = n.sysram[CONTA - 0x200];
    pal->runFrame(); ref->runFrame();
    const int dp = static_cast<uint16_t>(m.sysram[CONTA - 0x200] - cp), dn = static_cast<uint16_t>(n.sysram[CONTA - 0x200] - cn);
    check(dp > dn + 60, "PAL: mais voltas do laço por quadro (" + std::to_string(dp) + " contra " + std::to_string(dn) + " no NTSC)");
    // som: o AY-3-8914 anda no relógio de 1 MHz (período $100 → 1000000 / 2048 = 488 Hz)
    {
      pal->setSampleRate(48000);
      std::vector<float> l(4096), r(4096), all;
      while (pal->audioAvailable()) pal->pullAudio(l.data(), r.data(), std::min<size_t>(4096, pal->audioAvailable()));
      for (int i = 0; i < 50; i++) {
        pal->runFrame();
        const size_t k = std::min<size_t>(pal->audioAvailable(), 4096);
        pal->pullAudio(l.data(), r.data(), k);
        all.insert(all.end(), l.begin(), l.begin() + static_cast<long>(k));
      }
      int up = 0;
      for (size_t i = 1; i < all.size(); i++) if (all[i - 1] < 0 && all[i] >= 0) up++;
      const double f = up / (all.size() / 48000.0);
      check(std::fabs(f - 488.28) < 7, "PAL: tom do AY-3-8914 com o relógio de 1 MHz: " + std::to_string(static_cast<int>(f)) + " Hz (esperado 488)");
    }
    // estado de NTSC não entra no PAL
    bool threw = false;
    try { pal->loadState(ref->saveState()); } catch (const std::exception&) { threw = true; }
    check(threw, "estado salvo no NTSC é recusado no PAL");
  }
  // (f) .cfg ruim: não trava; .cfg sem nada mapeado: erro claro
  {
    auto c = withCfg(toBin(words), "nada.bin", "[mapping]\n$0000 - $00FF = $5000\n[memattr]\n$9000 - $90FF = RAM 16 PAGE 99\n");  // linha com PAGE inválido: ignorada
    std::string err;
    Cart k;
    const bool vazio = !parseCfg("[mapping]\n; nada\n[memattr]\n", {}, k, err) && err.find(".cfg") != std::string::npos;
    uint32_t seed = 12345;
    auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFF; };
    const char* pecas[] = {"[mapping]", "[memattr]", "[bankswitch]", "[preload]", "[vars]", "$", "-", "=", "PAGE", "RAM", "8",
                           "16", "\n", " ", "$FFFF", "$0000", "$5000", "\"", "name", ";", "0x", "99999999", "$A000"};
    int vivos = 0;
    for (int i = 0; i < 300; i++) {
      std::string cfg;
      for (int j = 0; j < 60; j++) cfg += pecas[rnd() % (sizeof pecas / sizeof *pecas)];
      Cart cc;
      std::string e;
      if (parseCfg(cfg, toBin(words), cc, e)) {
        Intv mq(cc, freeExec(), freeGrom());
        mq.runFrame();
        vivos++;
      }
    }
    check(c != nullptr && vazio && vivos > 0, "300 .cfg de lixo sem travar (" + std::to_string(vivos) + " rodaram); .cfg sem mapa: erro");
  }
  // (g) PAGE em hex (A-F) vai para a página; PAGE inválido descarta a linha (não cai no mapa direto) e avisa
  {
    std::vector<uint16_t> w(0x200, 0x0123);
    Cart k;
    std::string e;
    const bool hexOk = parseCfg("[mapping]\n$0000 - $00FF = $5000\n$0100 - $01FF = $A000 PAGE A\n", toBin(w), k, e) &&
                       k.attr[0xA000] == 0 && !k.pmem.empty() && (k.pattr[Cart::pidx(0xA, 10, 0)] & 1) &&
                       k.pmem[Cart::pidx(0xA, 10, 0)] == 0x0123 && k.cfgWarn.empty();
    Cart k2;
    const bool bad = parseCfg("[mapping]\n$0000 - $00FF = $5000\n$0100 - $01FF = $A000 PAGE 99\n"
                              "[memattr]\n$B000 - $B0FF = RAM 16 PAGE G\n", toBin(w), k2, e) &&
                     k2.attr[0xA000] == 0 && k2.attr[0xB000] == 0 && k2.pmem.empty() &&
                     k2.cfgWarn.find("2 linha") != std::string::npos;
    Cart k3;
    std::string e3;
    const bool soRuim = !parseCfg("[mapping]\n$0000 - $00FF = $5000 PAGE Z\n", toBin(w), k3, e3) && e3.find("PAGE") != std::string::npos;
    check(hexOk && bad && soRuim, "PAGE A-F vai para as páginas 10-15; PAGE inválido descarta a linha e avisa (" + k2.cfgWarn + ")");
  }
  // (h) .cfg feito para demorar (milhares de faixas de 64K): recusado logo, sem travar a abertura
  {
    std::string cfg = "[mapping]\n$0000 - $00FF = $5000\n[memattr]\n";
    while (cfg.size() < 65000) cfg += "0 - $FFFF = RAM 8 PAGE 1\n";
    Cart k;
    std::string e;
    const auto t0 = std::chrono::steady_clock::now();
    const bool recusou = !parseCfg(cfg, toBin(std::vector<uint16_t>(0x100, 1)), k, e) && e.find("faixas") != std::string::npos;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // 16 páginas de 64K de RAM (o maior mapa que faz sentido) ainda cabem
    std::string grande = "[mapping]\n$0000 - $00FF = $5000\n[memattr]\n";
    for (int pg = 0; pg < 16; pg++) grande += "$0000 - $FFFF = RAM 16 PAGE " + std::to_string(pg) + "\n";
    Cart k2;
    const bool cabe = parseCfg(grande, toBin(std::vector<uint16_t>(0x100, 1)), k2, e);
    check(recusou && cabe && ms < 200, ".cfg com milhares de faixas de 64K recusado em " + std::to_string(static_cast<int>(ms)) +
                                           " ms; 16 páginas inteiras de RAM ainda abrem");
  }
  // (i) espelho da GRAM em $7800: a ROM de uma página do ECS bloqueia como a ROM direta
  {
    std::vector<uint16_t> w(0x100, 0x0123);
    w.resize(0x200, 0x0456);
    Cart k;
    std::string e;
    bool ok = parseCfg("[mapping]\n$0000 - $00FF = $5000\n$0100 - $01FF = $7800 PAGE 1\n", toBin(w), k, e);
    if (ok) {
      Intv m(k, freeExec(), freeGrom());
      m.write(0x7900, 0x55);  // página 0 (sem nada): vai para a GRAM
      const bool livre = m.stic.gmem[0x900] == 0x55;
      m.write(0x7FFF, 0x7A51);  // troca para a página 1 (ROM em $7800-$78FF)
      m.write(0x7810, 0x66);
      ok = livre && m.page(7) == 1 && m.stic.gmem[0x810] != 0x66 && m.read(0x7810) == 0x0456;
    }
    check(ok, "GRAM em $7800: página 0 vazia deixa escrever, página 1 com ROM bloqueia");
  }
  // (j) .cfg só vale com o firmware: sem ele, um .bin que não é de Intellivision não vira Intellivision
  {
    std::vector<uint8_t> a26(4096, 0xEA);
    a26[0xFFC] = 0x00; a26[0xFFD] = 0xF0;
    const std::string cfg = "[mapping]\n$0000 - $07FF = $5000\n";
    setFirmware("intv_cfg", std::vector<uint8_t>(cfg.begin(), cfg.end()));
    const bool com = backup::identify(a26, "jogo.bin").sys == "intv";
    setFirmware("intv_cfg", {});
    const std::string sem = backup::identify(a26, "jogo.bin").sys;
    check(com && sem != "intv", ".bin com .cfg é Intellivision; sem o .cfg (firmware limpo) volta a ser " + sem);
  }
  // (k) PAL só com "(PAL)" no nome: o cartucho não tem região, "(Europe)" continua NTSC
  check(isPalName("jogo (pal).bin") && !isPalName("jogo (europe).bin") && !isPalName("jogo (usa, europe).bin") &&
            !isPalName("jogo (e).bin"),
        "PAL só pelo \"(PAL)\" no nome; \"(Europe)\" e \"(E)\" rodam a 60 Hz");
}

// cartuchos de lixo (palavras aleatórias, cabeçalho apontando para o lixo) e estados com bytes trocados:
// o núcleo não pode travar nem ler fora da memória
void fuzzTest() {
  uint32_t x = 12345;
  auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
  int frames = 0;
  for (int k = 0; k < 40; k++) {
    std::vector<uint8_t> d((0x2000 + (rnd() % 0x2000)) * 2);
    for (auto& b : d) b = static_cast<uint8_t>(rnd());
    for (int i = 0; i < 12; i++) { d[i * 2] = 0; }
    d[9] = 0x50; d[21] = 0x50;  // início e título dentro do cartucho
    std::unique_ptr<Core> c = createCore(d, "lixo.int", nullptr);
    if (!c) continue;
    c->setSampleRate(44100);
    for (int f = 0; f < 60; f++) {
      c->setKeys(rnd() & 0xFFF);
      c->runFrame();
      frames++;
      if (f == 30) {
        std::vector<uint8_t> st = c->saveState();
        for (int j = 0; j < 20; j++) st[rnd() % st.size()] ^= static_cast<uint8_t>(1u << (rnd() % 8));
        try { c->loadState(st); } catch (const std::exception&) {}
      }
    }
    std::vector<float> l(4096), r(4096);
    c->pullAudio(l.data(), r.data(), std::min<size_t>(4096, c->audioAvailable()));
  }
  check(frames == 40 * 60, "40 cartuchos de lixo × 60 quadros (com estados danificados) sem travar");
}

void gerar(const std::string& dir) {
  std::filesystem::create_directories(dir);
  const std::vector<uint16_t> words = buildCart(0x5000);
  writeFile(dir + "/teste_indigo.rom", toRom(words));
  writeFile(dir + "/teste_indigo.int", toBin(words));
  std::printf("cartuchos de teste gravados em %s\n", dir.c_str());
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "--gerar") { gerar(argv[2]); return 0; }
  const std::string roms = argc > 1 ? argv[1] : "/root/indigo-env/roms";
  const std::string png = argc > 2 ? argv[2] : "";
  cpuTests();
  cycleTable();
  psgTests();
  consoleTest(png);
  formatTests(roms);
  cfgTests();
  fuzzTest();
  std::printf("RESULTADO intv: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
