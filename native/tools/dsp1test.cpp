// Testes do DSP-1 do SNES (src/snes/dsp1.*):
//  1. µPD7725: programas pequenos montados aqui, rodando no núcleo e conversando pelo DR/SR como o SNES conversa
//     (aritmética, vai-um, multiplicador, ROM de dados, CALL/RT, saltos condicionais, modos de 8 e 16 bits);
//  2. imitação dos comandos: contas conferidas com a matemática (seno, inverso, raiz, matrizes) e coerência da
//     projeção do Mode 7 (um ponto da tela "mirado" no chão e projetado de volta tem de cair no mesmo lugar).
// Sem argumentos; termina com "0 falhas" quando tudo confere.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <vector>

#include "../src/snes/dsp1.h"
#include "../src/snes/snes.h"

using namespace indigo::snes;

static int fails = 0, checks = 0;
static void check(bool ok, const char* what, long got = 0, long want = 0) {
  checks++;
  if (!ok && fails++ < 40) std::printf("  FALHOU: %s (deu %ld, esperado %ld)\n", what, got, want);
}

// ------------------------------------------------------------------ montador mínimo do µPD7725
enum { S_TRB, S_A, S_B, S_TR, S_DP, S_RP, S_ROM, S_SGN, S_DR, S_DRNF, S_SR, S_SIM, S_SIL, S_K, S_L, S_MEM };
enum { D_NON, D_A, D_B, D_TR, D_DP, D_RP, D_DR, D_SR, D_SOL, D_SOM, D_K, D_KLR, D_KLM, D_L, D_TRB, D_MEM };
enum { A_NOP, A_OR, A_AND, A_XOR, A_SUB, A_ADD, A_SBB, A_ADC, A_DEC, A_INC, A_CMP, A_SHR1, A_SHL1, A_SHL2, A_SHL4, A_XCHG };
enum { P_RAM, P_IDB, P_M, P_N };
static uint32_t op(int src, int dst, int alu = 0, int psel = 0, int asl = 0, bool rt = false) {
  return (uint32_t(rt ? 1 : 0) << 22) | (psel << 20) | (alu << 16) | (asl << 15) | (src << 4) | dst;
}
static uint32_t ld(uint16_t imm, int dst) { return (3u << 22) | (uint32_t(imm) << 6) | dst; }
static uint32_t jp(int brch, int to) { return (2u << 22) | (uint32_t(brch) << 13) | (uint32_t(to) << 2); }
enum { JMP = 0x100, CALL = 0x140, JCA = 0x082, JRQM = 0x0BE };

static bool waitRqm(Upd7725& c) {
  for (int i = 0; i < 100000; i++) { if (c.readSR() & 0x80) return true; c.exec(); }
  return false;
}
static void put16(Upd7725& c, uint16_t v) { check(waitRqm(c), "pedido de dado"); c.writeDR(static_cast<uint8_t>(v)); c.writeDR(static_cast<uint8_t>(v >> 8)); }
static uint16_t get16(Upd7725& c) {
  check(waitRqm(c), "resposta");
  const uint8_t lo = c.readDR();
  return static_cast<uint16_t>(lo | (c.readDR() << 8));
}

static void testChip() {
  std::printf("== µPD7725 (programas de teste)\n");
  Upd7725 c;
  for (auto& w : c.program) w = 0;
  int pc = 0;
  auto at = [&](int addr) { pc = addr; };
  auto emit = [&](uint32_t w) { c.program[pc++] = w; };
  emit(ld(0x0000, D_SR));                          // 0: modo de 16 bits
  emit(op(S_DR, D_NON));                           // 1: pede X
  emit(jp(JRQM, 2));                               // 2: espera o SNES
  emit(op(S_DRNF, D_A));                           // 3: A = X
  emit(op(S_DR, D_NON));                           // 4: pede Y
  emit(jp(JRQM, 5));
  emit(op(S_DRNF, D_B));                           // 6: B = Y
  emit(op(S_A, D_K));                              // 7: K = X
  emit(op(S_B, D_L));                              // 8: L = Y
  emit(op(S_A, D_TR, A_INC, P_IDB));               // 9: TR = X; A = X + 1
  emit(op(S_A, D_DR));                             // 10: devolve X + 1
  emit(jp(JRQM, 11));
  emit(op(S_A, D_NON, A_XOR, P_IDB));              // 12: A = 0
  emit(op(S_TRB, D_NON, A_ADD, P_M));              // 13: A = M (X*Y >> 15)
  emit(op(S_A, D_DR));                             // 14
  emit(jp(JRQM, 15));
  emit(op(S_TR, D_A));                             // 16: A = X
  emit(op(S_B, D_NON, A_ADD, P_IDB));              // 17: A = X + Y (vai-um em C)
  emit(op(S_A, D_DR));                             // 18
  emit(jp(JRQM, 19));
  emit(jp(JCA, 23));                               // 20
  emit(ld(0, D_TRB));                              // 21
  emit(jp(JMP, 24));                               // 22
  emit(ld(1, D_TRB));                              // 23
  emit(op(S_TRB, D_DR));                           // 24: devolve o vai-um
  emit(jp(JRQM, 25));
  emit(ld(5, D_RP));                               // 26
  emit(op(S_ROM, D_DR));                           // 27: devolve ROM[5]
  emit(jp(JRQM, 28));
  emit(jp(CALL, 100));                             // 29
  emit(op(S_A, D_DR));                             // 30: devolve X + 0x100 (feito na sub-rotina)
  emit(jp(JRQM, 31));
  emit(jp(JMP, 1));                                // 32
  at(100);
  emit(op(S_TR, D_A));
  emit(ld(0x0100, D_B));
  emit(op(S_B, D_NON, A_ADD, P_IDB, 0, true));     // RT
  c.data[5] = 0xBEEF;
  c.reset();
  const uint16_t pairs[][2] = {{3, 4}, {0x4000, 0x4000}, {0xFFFF, 2}, {0x8000, 0x8000}, {0x1234, 0xEDCC}, {0x7FFF, 0x0001}};
  for (const auto& pr : pairs) {
    const uint16_t x = pr[0], y = pr[1];
    put16(c, x);
    put16(c, y);
    const uint16_t inc = get16(c), mul = get16(c), sum = get16(c), carry = get16(c), rom = get16(c), sub = get16(c);
    check(inc == static_cast<uint16_t>(x + 1), "X+1", inc, static_cast<uint16_t>(x + 1));
    const uint16_t wantMul = static_cast<uint16_t>((int32_t(int16_t(x)) * int16_t(y)) >> 15);
    check(mul == wantMul, "multiplicador", mul, wantMul);
    check(sum == static_cast<uint16_t>(x + y), "soma", sum, static_cast<uint16_t>(x + y));
    check(carry == ((uint32_t(x) + y) > 0xFFFF ? 1 : 0), "vai-um", carry, (uint32_t(x) + y) > 0xFFFF);
    check(rom == 0xBEEF, "ROM de dados", rom, 0xBEEF);
    check(sub == static_cast<uint16_t>(x + 0x100), "CALL/RT", sub, static_cast<uint16_t>(x + 0x100));
  }
  // modo de 8 bits (DRC): devolve o byte com os 4 bits trocados (SHL4 + XCHG mostram as operações de deslocamento)
  Upd7725 d;
  for (auto& w : d.program) w = 0;
  pc = 0;
  auto emit2 = [&](uint32_t w) { d.program[pc++] = w; };
  emit2(ld(0x0400, D_SR));                         // DRC = 1
  emit2(op(S_DR, D_NON));                          // 1: pede
  emit2(jp(JRQM, 2));
  emit2(op(S_DRNF, D_A));
  emit2(op(S_TRB, D_NON, A_SHL4, P_IDB));          // A = A << 4 | 15
  emit2(op(S_A, D_DR));
  emit2(jp(JRQM, 6));
  emit2(jp(JMP, 1));
  d.reset();
  for (int v : {0x12, 0x00, 0xA5}) {
    check(waitRqm(d), "pedido (8 bits)");
    d.writeDR(static_cast<uint8_t>(v));
    check(waitRqm(d), "resposta (8 bits)");
    const int got = d.readDR();
    check(got == ((v << 4 | 15) & 0xFF), "SHL4 em 8 bits", got, (v << 4 | 15) & 0xFF);
  }
  std::printf("  %d conferências\n", checks);
}

// ------------------------------------------------------------------ comandos (imitação)
static std::vector<int16_t> cmd(Dsp1& d, uint8_t c, std::initializer_list<int> in, int outWords) {
  d.write(false, c);
  for (int v : in) { d.write(false, static_cast<uint8_t>(v)); d.write(false, static_cast<uint8_t>(v >> 8)); }
  std::vector<int16_t> out;
  for (int i = 0; i < outWords; i++) {
    const uint8_t lo = d.read(false);
    out.push_back(static_cast<int16_t>(lo | (d.read(false) << 8)));
  }
  return out;
}

static void testHle() {
  const int before = checks;
  std::printf("== DSP-1 sem o arquivo do chip (comandos)\n");
  Dsp1 d(std::vector<uint8_t>{});
  check(d.read(true) == 0x80, "SR sempre pronto", d.read(true), 0x80);
  check(cmd(d, 0x00, {0x4000, 0x4000}, 1)[0] == 0x2000, "multiplicação");
  check(cmd(d, 0x20, {0x4000, 0x4000}, 1)[0] == 0x2001, "multiplicação +1");
  // seno e cosseno em 64 ângulos
  int worst = 0;
  for (int k = 0; k < 64; k++) {
    const int a = k * 1024 - 32768;
    const auto r = cmd(d, 0x04, {a, 0x7FFF}, 2);
    const double ang = a * 2 * M_PI / 65536;
    worst = std::max({worst, std::abs(r[0] - static_cast<int>(std::lround(32767 * std::sin(ang)))),
                      std::abs(r[1] - static_cast<int>(std::lround(32767 * std::cos(ang))))});
  }
  check(worst <= 4, "seno/cosseno (erro máximo)", worst, 4);
  // inverso
  double worstRel = 0;
  for (int c = 0x1000; c < 0x8000; c += 0x123) {
    for (int e : {-3, 0, 5}) {
      const auto r = cmd(d, 0x10, {c, e}, 2);
      const double want = 1.0 / (c / 32768.0 * std::ldexp(1.0, e)), got = r[0] / 32768.0 * std::ldexp(1.0, r[1]);
      worstRel = std::max(worstRel, std::fabs(got - want) / want);
    }
  }
  check(worstRel < 0.001, "inverso (erro relativo × 1000)", static_cast<long>(worstRel * 1000000), 1000);
  // raiz (Distance) e Range
  int worstD = 0;
  for (int i = 0; i < 200; i++) {
    const int x = (i * 977) % 18000 - 9000, y = (i * 613) % 18000 - 9000, z = (i * 271) % 18000 - 9000;
    const int r = cmd(d, 0x28, {x, y, z}, 1)[0];
    const int want = static_cast<int>(std::sqrt(double(x) * x + double(y) * y + double(z) * z));
    worstD = std::max(worstD, std::abs(r - want));
  }
  check(worstD <= 20, "distância (erro máximo)", worstD, 20);
  {
    const auto r = cmd(d, 0x18, {100, 200, 300, 50}, 1);
    const int want = (100 * 100 + 200 * 200 + 300 * 300 - 50 * 50) >> 15;
    check(r[0] == want, "range", r[0], want);
  }
  // rotação 2D de 90°
  {
    const auto r = cmd(d, 0x0C, {0x4000, 1000, 0}, 2);
    check(std::abs(r[0]) <= 2 && std::abs(r[1] + 1000) <= 2, "rotação 2D", r[0] * 100000L + r[1], -1000);
  }
  // atitude: objetivo e subjetivo são inversos (matriz ortogonal com escala m/2)
  int worstM = 0;
  for (int t = 0; t < 30; t++) {
    cmd(d, 0x01, {0x7FFF, t * 2111, t * 1733 - 20000, t * 911}, 0);
    const int x = t * 500 - 7000, y = 3000 - t * 200, z = t * 300;
    const auto f = cmd(d, 0x0D, {x, y, z}, 3);
    const auto v = cmd(d, 0x03, {f[0], f[1], f[2]}, 3);
    worstM = std::max({worstM, std::abs(v[0] - x / 4), std::abs(v[1] - y / 4), std::abs(v[2] - z / 4)});
  }
  check(worstM <= 4, "matriz de atitude (ida e volta)", worstM, 4);
  // projeção do Mode 7: mira (Target) um ponto da tela no chão e projeta (Project) de volta
  int worstP = 0, tried = 0;
  for (int t = 0; t < 12; t++) {
    const int fx = 0x200 + t * 40, fy = 0x300 - t * 30, aas = t * 5000, azs = 0x2C00 + t * 120;
    cmd(d, 0x02, {fx, fy, 0, 0x0100, 0x0100, aas, azs}, 4);  // câmera perto: ~1 unidade do chão por ponto
    for (int v = 0; v <= 90; v += 15) {
      for (int h = -100; h <= 100; h += 25) {
        const auto g = cmd(d, 0x0E, {h, v}, 2);
        const auto s = cmd(d, 0x06, {g[0], g[1], 0}, 3);
        tried++;
        // o ponto do chão é inteiro (perde a fração) e o chip faz as contas com 16 bits
        worstP = std::max({worstP, std::abs(s[0] - h), std::abs(s[1] - v)});
      }
    }
  }
  check(worstP <= 8, "Target e Project voltam ao mesmo ponto da tela", worstP, 8);
  // Raster: saída contínua, uma linha por leitura de 4 palavras
  {
    cmd(d, 0x02, {0x200, 0x200, 0, 0x0100, 0x0400, 0, 0x3000}, 4);
    const auto a = cmd(d, 0x0A, {-100}, 4);
    std::vector<int16_t> b;
    for (int line = 0; line < 200; line++) {
      b.clear();
      for (int i = 0; i < 4; i++) { const uint8_t lo = d.read(false); b.push_back(static_cast<int16_t>(lo | (d.read(false) << 8))); }
    }
    // perto do horizonte a escala é maior: 200 linhas abaixo, o An tem de ter diminuído
    check(a[0] > 0 && b[0] > 0 && b[0] < a[0], "raster dá as linhas seguintes sozinho", b[0], a[0]);
  }
  std::printf("  %d conferências (%d pontos de tela na projeção)\n", checks - before, tried);
}


// ------------------------------------------------------------------ DSP-1 no mapa de memória do SNES
// cartucho de mentira com o cabeçalho de DSP-1 ($FFD6 = $03): o 65816 conversa com o chip pelos endereços do
// cartucho (LoROM de 512 KB em $30:8000/$30:C000, LoROM de 2 MB em $60:0000/$60:4000, HiROM em $00:6000/$00:7000)
static std::vector<uint8_t> fakeRom(bool hirom, size_t size) {
  std::vector<uint8_t> r(size, 0);
  const size_t h = hirom ? 0xFFC0 : 0x7FC0;
  const char* title = "INDIGO DSP1 TESTE    ";
  for (int i = 0; i < 21; i++) r[h + i] = static_cast<uint8_t>(title[i]);
  r[h + 0x15] = hirom ? 0x21 : 0x20;
  r[h + 0x16] = 0x03;  // ROM + DSP
  r[h + 0x17] = 0x09;
  r[h + 0x1C] = 0xFF; r[h + 0x1D] = 0xFF; r[h + 0x1E] = 0x00; r[h + 0x1F] = 0x00;
  r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;  // reset em $8000
  r[hirom ? 0x8000 : 0x0000] = 0x78;       // SEI
  return r;
}

static void testBus() {
  const int before = checks;
  std::printf("== DSP-1 nos endereços do cartucho\n");
  struct Case { bool hirom; size_t size; uint32_t dr, sr; const char* name; };
  const Case cases[] = {{false, 0x80000, 0x308000, 0x30C000, "LoROM 512 KB"}, {false, 0x200000, 0x600000, 0x604000, "LoROM 2 MB"},
                        {true, 0x100000, 0x006000, 0x007000, "HiROM 1 MB"}, {false, 0x80000, 0xB08000, 0xB0C000, "espelho em $B0"}};
  for (const auto& c : cases) {
    indigo::snes::Cart cart;
    std::string err;
    if (!indigo::snes::parseRom(fakeRom(c.hirom, c.size), cart, err)) { check(false, err.c_str()); continue; }
    check(cart.dsp1 != indigo::snes::Cart::DSP_NONE, c.name);
    indigo::snes::SNES s(cart);
    check(s.read(c.sr) == 0x80, "SR pronto");
    s.write(c.dr, 0x00);  // multiplicação
    for (int v : {0x4000, 0x2000}) { s.write(c.dr, static_cast<uint8_t>(v)); s.write(c.dr, static_cast<uint8_t>(v >> 8)); }
    const int lo = s.read(c.dr), hi = s.read(c.dr);
    check(((hi << 8) | lo) == 0x1000, c.name, (hi << 8) | lo, 0x1000);
  }
  // com um "arquivo do chip": programa que devolve a palavra recebida + 1, rodando no relógio do SNES
  std::vector<uint32_t> prog = {ld(0x0000, D_SR), op(S_DR, D_NON), jp(JRQM, 2), op(S_DRNF, D_A), op(S_TRB, D_NON, A_INC, P_IDB),
                                op(S_A, D_DR), jp(JRQM, 6), jp(JMP, 1)};
  std::vector<uint8_t> fw(8192, 0);
  for (size_t i = 0; i < prog.size(); i++) { fw[i * 3] = prog[i] & 0xFF; fw[i * 3 + 1] = (prog[i] >> 8) & 0xFF; fw[i * 3 + 2] = prog[i] >> 16; }
  indigo::setFirmware("dsp1", fw);
  {
    indigo::snes::Cart cart;
    std::string err;
    indigo::snes::parseRom(fakeRom(false, 0x80000), cart, err);
    indigo::snes::SNES s(cart);
    check(s.dsp && s.dsp->exact(), "usa o arquivo do chip");
    auto waitSr = [&] { for (int i = 0; i < 1000; i++) if (s.read(0x30C000) & 0x80) return true; return false; };
    for (int v : {0x1234, 0xFFFF, 0x0000}) {
      check(waitSr(), "pedido pelo SR");
      s.write(0x308000, static_cast<uint8_t>(v)); s.write(0x308000, static_cast<uint8_t>(v >> 8));
      check(waitSr(), "resposta pelo SR");
      const int lo = s.read(0x308000), hi = s.read(0x308000);
      check(((hi << 8) | lo) == ((v + 1) & 0xFFFF), "programa do chip pelo barramento", (hi << 8) | lo, (v + 1) & 0xFFFF);
    }
  }
  indigo::setFirmware("dsp1", {});
  std::printf("  %d conferências\n", checks - before);
}

int main() {
  testChip();
  testHle();
  testBus();
  std::printf("DSP-1: %d conferências, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
