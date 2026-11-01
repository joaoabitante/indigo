// Testes do 32X (pasta de testes como argumento, normalmente /root/indigo-env/roms/32x):
//  1. CPU SH-2 contra o interpretador do PicoDrive/MAME: casos aleatórios do t7_32x_gen.h (uma instrução válida
//     com registradores e memória aleatórios), comparando registradores, memória e ciclos (sh2ref.bin, gerado pelo
//     t7_32x.sh); mais testes à mão dos periféricos (cache, DIVU, DMAC, FRT, WDT, interrupções).
//  2. homebrews do 32X comparados quadro a quadro com o PicoDrive (lista.txt e ref/*.ref);
//  3. cartuchos montados aqui, save state e robustez.
// Uso: indigo-t7_32x pasta [cpu|peri|ref|estado|cart] [nome]
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/md32x/mars.h"
#include "../src/md32x/sh2.h"
#include "t7_32x_gen.h"

namespace {

long g_checks = 0, g_fails = 0;

void check(bool ok, const std::string& what) {
  g_checks++;
  if (!ok) { g_fails++; std::printf("FALHA: %s\n", what.c_str()); }
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0) {
  c = ~c;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}

// ------------------------------------------------------------ SH-2 contra a referência
struct FlatBus : indigo::md32x::Sh2Bus {
  uint8_t mem[4096] = {};
  bool in(uint32_t a) const { return (a & 0x1FFFF000u) == 0x06000000u; }
  uint8_t rd8(indigo::md32x::Sh2&, uint32_t a) override { return in(a) ? mem[a & 0xFFF] : 0; }
  uint16_t rd16(indigo::md32x::Sh2&, uint32_t a) override { return in(a) ? uint16_t((mem[a & 0xFFF] << 8) | mem[(a + 1) & 0xFFF]) : 0; }
  void wr8(indigo::md32x::Sh2&, uint32_t a, uint8_t v) override { if (in(a)) mem[a & 0xFFF] = v; }
  void wr16(indigo::md32x::Sh2&, uint32_t a, uint16_t v) override { if (in(a)) { mem[a & 0xFFF] = uint8_t(v >> 8); mem[(a + 1) & 0xFFF] = uint8_t(v); } }
};

const char* RN[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
                    "pc", "pr", "sr", "gbr", "vbr", "mach", "macl", "ciclos", "memória"};

void cpuTests(const std::string& dir, const std::string& only) {
  std::vector<uint8_t> ref;
  if (!readFile(dir + "/sh2ref.bin", ref) || ref.size() < 100) {
    std::printf("SH-2 contra a referência: pulado (sem %s/sh2ref.bin)\n", dir.c_str());
    return;
  }
  const size_t n = ref.size() / 100;
  FlatBus bus;
  indigo::md32x::Sh2 cpu(bus, 0);
  Sh2Case c;
  std::map<std::string, long> badByOp;
  std::map<std::string, std::string> example;
  long bad = 0, skipped = 0;
  for (size_t i = 0; i < n; i++) {
    sh2_gen_case(uint32_t(i), &c);
    std::memcpy(bus.mem, c.mem, 4096);
    for (int k = 0; k < 16; k++) cpu.r[k] = c.r[k];
    cpu.pc = c.pc; cpu.pr = c.pr; cpu.sr = c.sr; cpu.gbr = c.gbr; cpu.vbr = c.vbr; cpu.mach = c.mach; cpu.macl = c.macl;
    cpu.sleeping = false;
    const uint16_t op = uint16_t((c.mem[0x100] << 8) | c.mem[0x101]);
    const uint16_t slot = uint16_t((c.mem[0x102] << 8) | c.mem[0x103]);
    // DIV1 Rn,Rn: o MAME lê Rm depois de deslocar Rn; o código C do manual (e o Índigo) lê antes
    auto div1Same = [](uint16_t o) { return (o & 0xF00F) == 0x3004 && ((o >> 8) & 15) == ((o >> 4) & 15); };
    if (div1Same(op) || div1Same(slot)) { skipped++; continue; }
    int cyc = cpu.step();
    const bool taken = ((op >> 9) & 1) ? !(c.sr & 1) : (c.sr & 1);
    if (((op & 0xFD00) == 0x8D00) && !taken) cyc += cpu.step();  // BT/S, BF/S sem desvio: a referência roda o slot junto
    uint32_t got[25];
    for (int k = 0; k < 16; k++) got[k] = cpu.r[k];
    got[16] = cpu.pc; got[17] = cpu.pr; got[18] = cpu.sr; got[19] = cpu.gbr; got[20] = cpu.vbr; got[21] = cpu.mach; got[22] = cpu.macl;
    got[23] = uint32_t(cyc); got[24] = crc32(bus.mem, 4096);
    uint32_t exp[25];
    std::memcpy(exp, &ref[i * 100], 100);
    for (int k = 0; k < 25; k++) {
      if (got[k] == exp[k]) continue;
      char key[16];
      std::snprintf(key, sizeof key, "%04X", op & 0xF00F);
      if (!badByOp[key]) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "caso %zu op %04X: %s=%08X esperado %08X", i, op, RN[k], got[k], exp[k]);
        example[key] = buf;
      }
      badByOp[key]++;
      bad++;
      break;
    }
  }
  int shown = 0;
  for (auto& kv : badByOp) if (shown++ < 40 || !only.empty()) std::printf("  %s: %ld diferentes (ex.: %s)\n", kv.first.c_str(), kv.second, example[kv.first].c_str());
  std::printf("SH-2 (casos aleatórios contra o PicoDrive/MAME): %zu casos, %ld diferentes (%ld com DIV1 Rn,Rn, pulados)\n", n, bad, skipped);
  check(bad == 0, "SH-2: todos os casos iguais à referência");
}

// ------------------------------------------------------------ periféricos do SH7604 (pelo manual)
// 64 KB em 06000000; o programa em 06000100 é "bra $ / nop" (espera), as rotinas de interrupção em 06000200+
// também; o teste confere para onde o PC foi.
struct PeriBus : indigo::md32x::Sh2Bus {
  std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000);
  int irlAcks = 0;
  bool in(uint32_t a) const { return (a & 0x1FFF0000u) == 0x06000000u; }
  uint8_t rd8(indigo::md32x::Sh2&, uint32_t a) override { return in(a) ? mem[a & 0xFFFF] : 0; }
  uint16_t rd16(indigo::md32x::Sh2&, uint32_t a) override { return in(a) ? uint16_t((mem[a & 0xFFFF] << 8) | mem[(a + 1) & 0xFFFF]) : 0; }
  void wr8(indigo::md32x::Sh2&, uint32_t a, uint8_t v) override { if (in(a)) mem[a & 0xFFFF] = v; }
  void wr16(indigo::md32x::Sh2&, uint32_t a, uint16_t v) override { if (in(a)) { mem[a & 0xFFFF] = uint8_t(v >> 8); mem[(a + 1) & 0xFFFF] = uint8_t(v); } }
  void irlAck(indigo::md32x::Sh2&, int) override { irlAcks++; }
  void put16(uint32_t a, uint16_t v) { mem[a & 0xFFFF] = uint8_t(v >> 8); mem[(a + 1) & 0xFFFF] = uint8_t(v); }
  void put32(uint32_t a, uint32_t v) { put16(a, uint16_t(v >> 16)); put16(a + 2, uint16_t(v)); }
};

void periTests() {
  using indigo::md32x::Sh2;
  PeriBus bus;
  Sh2 cpu(bus, 0);
  auto fresh = [&]() {
    std::fill(bus.mem.begin(), bus.mem.end(), 0);
    bus.put32(0x06000000, 0x06000100);  // vetores 0 e 1 (lidos com VBR = 0: espelho em 0 não existe; o teste
    bus.put32(0x06000004, 0x0600F000);  //  põe PC e SP à mão)
    for (uint32_t a = 0x06000100; a < 0x06000400; a += 4) bus.put32(a, 0xAFFE0009);  // bra $ / nop
    cpu.powerOn();
    cpu.pc = 0x06000100; cpu.r[15] = 0x0600F000; cpu.vbr = 0x06008000; cpu.sr = 0xF0;
    cpu.clock = 0;
    for (int v = 0; v < 128; v++) bus.put32(0x06008000 + v * 4, 0x06000200 + v * 4);  // vetor v -> 06000200 + 4v
  };
  auto vectorTaken = [&]() -> int { return cpu.pc >= 0x06000200 && cpu.pc < 0x06000400 ? int((cpu.pc - 0x06000200) / 4) : -1; };

  // --- divisor
  fresh();
  cpu.write32(0xFFFFFF00, 7); cpu.write32(0xFFFFFF04, uint32_t(-1000));
  check(cpu.read32(0xFFFFFF14) == uint32_t(-142) && cpu.read32(0xFFFFFF10) == uint32_t(-6) && cpu.read32(0xFFFFFF04) == uint32_t(-142),
        "DIVU 32/32 com sinal (-1000/7 = -142 resto -6)");
  cpu.write32(0xFFFFFF00, 100000); cpu.write32(0xFFFFFF10, 3); cpu.write32(0xFFFFFF14, 0x10000000);
  check(cpu.read32(0xFFFFFF14) == 0x310000000ull / 100000 && cpu.read32(0xFFFFFF10) == 0x310000000ull % 100000 &&
            cpu.read32(0xFFFFFF34) == 0x310000000ull / 100000,
        "DIVU 64/32 e o espelho em FFFFFF20-3F");
  check(!(cpu.read32(0xFFFFFF08) & 1), "DIVU sem estouro");
  cpu.write32(0xFFFFFF0C, 0x50); cpu.write16(0xFFFFFEE2, 0x5000);   // vetor 0x50, prioridade 5
  cpu.write32(0xFFFFFF08, 2);                                        // OVFIE
  cpu.write32(0xFFFFFF00, 2); cpu.write32(0xFFFFFF10, 0x10); cpu.write32(0xFFFFFF14, 0);  // 0x1000000000/2: estoura
  check((cpu.read32(0xFFFFFF08) & 1) && cpu.read32(0xFFFFFF14) == 0x7FFFFFFF, "DIVU 64/32: estouro satura e liga OVF");
  cpu.sr = 0x40;  // máscara 4 < 5
  cpu.run(cpu.clock + 40);
  check(vectorTaken() == 0x50 && ((cpu.sr >> 4) & 15) == 5, "DIVU: interrupção de estouro (VCRDIV, IPRA)");
  fresh();
  cpu.write32(0xFFFFFF00, 0); cpu.write32(0xFFFFFF04, 5);
  check(cpu.read32(0xFFFFFF08) & 1, "DIVU: divisão por zero liga OVF");

  // --- DMA interno: cópia automática de 16 bits, TE, interrupção do fim
  fresh();
  for (int i = 0; i < 32; i++) bus.put16(0x06004000 + i * 2, uint16_t(0x1111 * (i & 15) + i));
  cpu.write32(0xFFFFFF80, 0x26004000); cpu.write32(0xFFFFFF84, 0x26005000); cpu.write32(0xFFFFFF88, 32);
  cpu.write32(0xFFFFFFA0, 0x48); cpu.write16(0xFFFFFEE2, 0x0300);  // vetor 0x48, prioridade 3
  cpu.write32(0xFFFFFFB0, 1);
  cpu.write32(0xFFFFFF8C, 0x5605);  // destino+, origem+, 16 bits, automático, IE, DE
  bool same = true;
  for (int i = 0; i < 64; i++) same &= bus.mem[0x5000 + i] == bus.mem[0x4000 + i];
  check(same && (cpu.read32(0xFFFFFF8C) & 2) && cpu.read32(0xFFFFFF88) == 0, "DMAC: cópia automática de 16 bits, TE e TCR = 0");
  cpu.sr = 0x00;
  cpu.run(cpu.clock + 40);
  check(vectorTaken() == 0x48, "DMAC: interrupção de fim (VCRDMA0, IPRA)");
  // 16 bytes por vez, decrementando o destino não vale (só crescente); TCR conta palavras longas
  fresh();
  for (int i = 0; i < 64; i++) bus.mem[0x4000 + i] = uint8_t(i * 7);
  cpu.write32(0xFFFFFF90, 0x26004000); cpu.write32(0xFFFFFF94, 0x26006000); cpu.write32(0xFFFFFF98, 16);
  cpu.write32(0xFFFFFFB0, 1);
  cpu.write32(0xFFFFFF9C, 0x5E01);  // canal 1: 16 bytes, automático
  same = true;
  for (int i = 0; i < 64; i++) same &= bus.mem[0x6000 + i] == uint8_t(i * 7);
  check(same && (cpu.read32(0xFFFFFF9C) & 2), "DMAC: unidade de 16 bytes no canal 1");
  // pedido externo (DREQ): só anda quando o pedido chega
  fresh();
  bus.put16(0x06004000, 0xBEEF);
  cpu.write32(0xFFFFFF80, 0x26004000); cpu.write32(0xFFFFFF84, 0x26005000); cpu.write32(0xFFFFFF88, 4);
  cpu.write32(0xFFFFFFB0, 1);
  cpu.write32(0xFFFFFF8C, 0x4401);  // destino+, origem fixa, 16 bits, externo
  check(bus.mem[0x5000] == 0 && cpu.dmaActive(0), "DMAC: pedido externo espera o DREQ");
  cpu.dreq(0, 3);
  check(cpu.read32(0xFFFFFF88) == 1 && bus.mem[0x5004] == 0xBE && !(cpu.read32(0xFFFFFF8C) & 2), "DMAC: DREQ transfere as unidades pedidas");
  cpu.dreq(0, 3);
  check(cpu.read32(0xFFFFFF88) == 0 && (cpu.read32(0xFFFFFF8C) & 2), "DMAC: TE no fim do DREQ");

  // --- FRT: comparação A com limpeza do contador e interrupção OCI
  fresh();
  cpu.write8(0xFFFFFE16, 0);       // relógio / 8
  cpu.write8(0xFFFFFE17, 0xE0);    // OCRA selecionado
  cpu.write8(0xFFFFFE14, 0); cpu.write8(0xFFFFFE15, 100);
  cpu.write8(0xFFFFFE11, 1);       // CCLRA
  cpu.write16(0xFFFFFE60, 0x0600); cpu.write16(0xFFFFFE66, 0x0049);  // FRT prioridade 6, OCI no vetor 0x49
  cpu.write8(0xFFFFFE10, 0x08);    // OCIAE
  cpu.sr = 0xF0;
  cpu.run(cpu.clock + 8 * 100 + 50);
  check((cpu.read8(0xFFFFFE11) & 0x08) != 0, "FRT: comparação A liga OCFA");
  {
    const int hi = cpu.read8(0xFFFFFE12), lo = cpu.read8(0xFFFFFE13);
    check(hi == 0 && lo < 20, "FRT: CCLRA zera o contador na comparação");
  }
  cpu.sr = 0x00;
  cpu.run(cpu.clock + 20);
  check(vectorTaken() == 0x49, "FRT: interrupção OCI (VCRC, IPRB)");
  cpu.write8(0xFFFFFE11, 0x01);   // zera OCFA (lido como 1 antes)
  check(!(cpu.read8(0xFFFFFE11) & 0x08), "FRT: OCFA zerado pelo programa");
  // estouro com o divisor de 32
  fresh();
  cpu.write8(0xFFFFFE16, 1);
  cpu.write8(0xFFFFFE12, 0xFF); cpu.write8(0xFFFFFE13, 0xF0);  // FRC = FFF0
  cpu.run(cpu.clock + 32 * 20);
  check((cpu.read8(0xFFFFFE11) & 0x02) && cpu.read8(0xFFFFFE12) == 0, "FRT: estouro (OVF)");

  // --- WDT em modo de intervalo
  fresh();
  cpu.write16(0xFFFFFEE2, 0x0070); cpu.write16(0xFFFFFEE4, 0x4A00);  // WDT prioridade 7, vetor 0x4A
  cpu.write16(0xFFFFFE80, 0x5A00 | 200);                             // WTCNT = 200
  cpu.write16(0xFFFFFE80, 0xA520 | 0);                               // TME, intervalo, relógio / 2
  cpu.sr = 0x00;
  cpu.run(cpu.clock + 2 * 56 + 30);
  check(vectorTaken() == 0x4A && (cpu.read8(0xFFFFFE80) & 0x80), "WDT: estouro no modo de intervalo (ITI, VCRWDT)");
  check(cpu.read8(0xFFFFFE81) < 40, "WDT: contador continua depois do estouro");

  // --- cache: gravação direta, dado velho visível até o descarte, área sem cache, matriz de endereços
  fresh();
  bus.put32(0x06004000, 0x11112222);
  cpu.write8(0xFFFFFE92, 0x11);    // descarta tudo e liga
  check(cpu.read32(0x06004000) == 0x11112222, "cache: primeira leitura (falta, enche a linha)");
  bus.put32(0x06004000, 0x33334444);  // outro processador muda a memória
  check(cpu.read32(0x06004000) == 0x11112222, "cache: a leitura com cache ainda vê o valor velho");
  check(cpu.read32(0x26004000) == 0x33334444, "cache: a área 2xxxxxxx lê a memória");
  check((cpu.read32(0x60004000) & 0x1FFFFC04) == 0x06004004 || (cpu.read32(0x60004000 | (0 << 0)) & 4) != 0 ||
            [&] { for (int w = 0; w < 4; w++) { cpu.write8(0xFFFFFE92, uint8_t(0x01 | (w << 6))); if (cpu.read32(0x60004000) & 4) { cpu.write8(0xFFFFFE92, 0x01); return true; } } cpu.write8(0xFFFFFE92, 0x01); return false; }(),
        "cache: matriz de endereços mostra a linha válida");
  cpu.write32(0x46004000, 0);      // descarte associativo da linha
  check(cpu.read32(0x06004000) == 0x33334444, "cache: descarte (4xxxxxxx) faz reler a memória");
  cpu.write32(0x06004000, 0x55556666);
  check(bus.mem[0x4000] == 0x55 && cpu.read32(0x06004000) == 0x55556666, "cache: gravação direta (memória e linha)");
  cpu.write8(0xFFFFFE92, 0x19);    // duas vias: as vias 0 e 1 viram RAM em C0000000
  cpu.write32(0xC0000010, 0xCAFEF00D);
  check(cpu.read32(0xC0000010) == 0xCAFEF00D, "cache: modo de duas vias, RAM em C0000000");
  cpu.write8(0xFFFFFE92, 0x00);
  bus.put32(0x06004000, 0x77778888);
  check(cpu.read32(0x06004000) == 0x77778888, "cache desligada: lê a memória");

  // --- interrupções: IRL acima da máscara, SLEEP acordado, ordem IRL antes das internas do mesmo nível
  fresh();
  bus.put16(0x06000100, 0x001B);   // sleep
  cpu.sr = 0x70;
  cpu.run(cpu.clock + 10);
  check(cpu.sleeping, "SLEEP para o processador");
  cpu.setIrl(6);
  cpu.run(cpu.clock + 10);
  check(cpu.sleeping, "IRL 6 com máscara 7 não acorda");
  cpu.setIrl(10);
  cpu.run(cpu.clock + 20);
  check(!cpu.sleeping && vectorTaken() == 64 + 5 && ((cpu.sr >> 4) & 15) == 10 && bus.irlAcks == 1, "IRL 10: vetor automático 69, máscara 10");
  {
    const uint32_t savedPc = (uint32_t(bus.mem[0xF000 - 8]) << 24) | (bus.mem[0xF000 - 7] << 16) | (bus.mem[0xF000 - 6] << 8) | bus.mem[0xF000 - 5];
    check(savedPc == 0x06000102, "interrupção depois do SLEEP volta para a instrução seguinte");
  }
  // instrução ilegal no slot
  fresh();
  bus.put16(0x06000100, 0xA002); bus.put16(0x06000102, 0xA000);  // bra / bra no slot
  cpu.step();
  check(vectorTaken() == 6, "desvio no slot de outro desvio: exceção de slot ilegal (vetor 6)");
  fresh();
  bus.put16(0x06000100, 0xFFFF);
  cpu.step();
  check(vectorTaken() == 4, "instrução ilegal (vetor 4)");
  // serial entre os dois SH-2
  {
    PeriBus b2;
    Sh2 a(bus, 0), c(b2, 1);
    a.other = &c; c.other = &a;
    a.powerOn(); c.powerOn();
    c.write8(0xFFFFFE02, 0x10);    // RE no outro
    a.write8(0xFFFFFE03, 0x5A);    // TDR
    a.write8(0xFFFFFE04, 0x04);    // zera TDRE
    a.write8(0xFFFFFE02, 0x20);    // TE: transmite
    check(c.read8(0xFFFFFE05) == 0x5A && (c.read8(0xFFFFFE04) & 0x40) && (a.read8(0xFFFFFE04) & 0x80), "SCI: byte de um SH-2 para o outro");
  }
}

// ------------------------------------------------------------ cartuchos montados aqui
std::vector<uint8_t> makeRom(size_t size, const std::vector<uint16_t>& code) {
  std::vector<uint8_t> r(size, 0xFF);
  auto w32 = [&](size_t a, uint32_t v) { r[a] = uint8_t(v >> 24); r[a + 1] = uint8_t(v >> 16); r[a + 2] = uint8_t(v >> 8); r[a + 3] = uint8_t(v); };
  w32(0, 0x00FFFE00); w32(4, 0x200);
  std::memcpy(&r[0x100], "SEGA 32X        ", 16);
  std::memcpy(&r[0x150], "TESTE DO INDIGO", 15);
  std::memset(&r[0x1F0], ' ', 16);
  std::memcpy(&r[0x1F0], "JUE", 3);
  for (size_t i = 0; i < code.size(); i++) { r[0x200 + i * 2] = uint8_t(code[i] >> 8); r[0x201 + i * 2] = uint8_t(code[i]); }
  return r;
}

void cartTests() {
  // 68000: liga o adaptador, banco 1 em 900000, cartucho em 880000, "MARS", vetor da BIOS, quadro e COMM
  auto rom = makeRom(0x200000, {
      0x13FC, 0x0001, 0x00A1, 0x5101,   // move.b #1,$A15101   (ADEN)
      0x13FC, 0x0001, 0x00A1, 0x5105,   // move.b #1,$A15105   (banco 1)
      0x1039, 0x0090, 0x0000,           // move.b $900000,d0
      0x13C0, 0x00FF, 0x0000,           // move.b d0,$FF0000
      0x1039, 0x0088, 0x0100,           // move.b $880100,d0
      0x13C0, 0x00FF, 0x0001,           // move.b d0,$FF0001
      0x2039, 0x00A1, 0x30EC,           // move.l $A130EC,d0
      0x23C0, 0x00FF, 0x0004,           // move.l d0,$FF0004
      0x2039, 0x0000, 0x0004,           // move.l $000004,d0   (vetor de reset da BIOS)
      0x23C0, 0x00FF, 0x0008,           // move.l d0,$FF0008
      0x33FC, 0x1234, 0x0084, 0x0000,   // move.w #$1234,$840000
      0x33FC, 0x0056, 0x0086, 0x0000,   // move.w #$0056,$860000 (sobreposição: o zero não grava)
      0x3039, 0x0084, 0x0000,           // move.w $840000,d0
      0x33C0, 0x00FF, 0x000C,           // move.w d0,$FF000C
      0x33FC, 0xABCD, 0x00A1, 0x5120,   // move.w #$ABCD,$A15120 (COMM0)
      0x13FC, 0x0004, 0x00A1, 0x5107,   // move.b #4,$A15107   (RV = 0, 68S)
      0x1039, 0x0000, 0x0004,           // move.b $000004,d0   (sem RV: BIOS)
      0x13C0, 0x00FF, 0x0010,           // move.b d0,$FF0010
      0x13FC, 0x0001, 0x00A1, 0x5107,   // move.b #1,$A15107   (RV = 1: cartucho em 0)
      0x1039, 0x0000, 0x0004,           // move.b $000004,d0
      0x13C0, 0x00FF, 0x0011,           // move.b d0,$FF0011
      0x60FE});
  rom[0x100000] = 0x5A;
  indigo::md32x::Md32xCore c(rom, nullptr);
  c.runFrame(); c.runFrame();
  const auto& ram = c.md().workRam();
  check(ram[0] == 0x5A && ram[1] == 'S', "68000: banco de 1 MB em 900000 e cartucho fixo em 880000");
  check(ram[4] == 'M' && ram[5] == 'A' && ram[6] == 'R' && ram[7] == 'S', "68000: identificação \"MARS\" em A130EC");
  check(ram[8] == 0x00 && ram[9] == 0x88 && ram[10] == 0x02 && ram[11] == 0x00, "68000: vetores da BIOS (HLE) apontam para 880200");
  check(ram[12] == 0x12 && ram[13] == 0x56, "68000: quadro do 32X em 840000 e sobreposição em 860000");
  check(c.mars().comm(0) == 0xABCD, "68000: registrador de comunicação COMM0");
  check(ram[0x10] == 0x00 && ram[0x11] == 0x00, "68000: RV liga o cartucho em 000000");
  check(c.system() == "32x", "núcleo do 32X");
  // detecção
  check(indigo::md32x::isMd32xRom(rom, "x.bin") && indigo::md32x::isMd32xRom(std::vector<uint8_t>(0x8000, 0), "jogo.32x"), "detecção: \"SEGA 32X\" no cabeçalho e extensão .32x");
  {
    auto md = rom;
    std::memcpy(&md[0x100], "SEGA MEGA DRIVE ", 16);
    check(!indigo::md32x::isMd32xRom(md, "x.bin") && !indigo::md32x::isMd32xRom(md, "x.md"), "detecção: cartucho de Mega Drive não é 32X");
    std::memcpy(&md[0x3C0], "MARS CHECK MODE ", 16);
    check(indigo::md32x::isMd32xRom(md, "x.bin"), "detecção: \"MARS CHECK MODE\" em 3C0");
    auto core = indigo::createCore(rom, "jogo.bin", nullptr);
    check(core && core->system() == "32x", "registro: o 32X vem antes do Mega Drive");
  }
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
  const auto* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
  return h;
}

void stateTests(const std::string& dir) {
  std::vector<uint8_t> rom;
  if (!readFile(dir + "/hb/teste5.32x", rom)) { std::printf("estado: pulado (sem hb/teste5.32x)\n"); return; }
  indigo::md32x::Md32xCore c(rom, nullptr);
  c.setSampleRate(48000);
  std::vector<float> l(8192), r(8192);
  std::vector<uint64_t> trace;
  auto run = [&](int n, uint32_t keys) {
    uint64_t h = 1469598103934665603ull;
    trace.clear();
    for (int f = 0; f < n; f++) {
      c.setKeys((f / 20) % 2 ? keys : 0);
      c.runFrame();
      h = fnv(c.framebuffer(), size_t(c.width()) * c.height() * 4, h);
      trace.push_back(h);
      size_t k = c.audioAvailable();
      while (k) { const size_t m = std::min<size_t>(k, 8192); c.pullAudio(l.data(), r.data(), m); h = fnv(l.data(), m * 4, h); k -= m; }
      trace.push_back(h);
    }
    return h;
  };
  run(200, 17);
  const auto st = c.saveState();
  const uint64_t a = run(120, 17);
  const auto ta = trace;
  c.loadState(st);
  const auto st2 = c.saveState();
  const uint64_t b = run(120, 17);
  for (size_t i = 0; i < ta.size(); i++)
    if (ta[i] != trace[i]) { std::printf("  estado: primeira diferença no quadro %zu (%s)\n", i / 2, (i & 1) ? "som" : "imagem"); break; }
  check(a == b, "save state determinístico (imagem e som iguais depois de carregar)");
  check(st == st2, "salvar -> carregar -> salvar dá os mesmos bytes");
  std::mt19937 rng(4321);
  int ok = 0;
  for (int i = 0; i < 300; i++) {
    std::vector<uint8_t> bad = st;
    if (i % 3 == 0) bad.resize(rng() % bad.size());
    else for (int k = 0; k < 1 + int(rng() % 40); k++) bad[rng() % bad.size()] ^= uint8_t(1 + rng() % 255);
    const auto before = c.saveState();
    try { c.loadState(bad); } catch (const std::exception&) { if (c.saveState() != before) { check(false, "estado recusado mudou o jogo"); break; } }
    run(3, 0xFFF);
    ok++;
  }
  check(ok == 300, "300 estados danificados sem travar");
  int roms = 0;
  for (int i = 0; i < 40; i++) {
    std::vector<uint8_t> g(size_t(0x400) << (rng() % 12));
    for (auto& v : g) v = uint8_t(rng());
    std::memcpy(&g[0x100], "SEGA 32X", 8);
    if (i % 2) { g[4] = 0; g[5] = 0; g[6] = 0x03; g[7] = 0xF0; }
    try {
      indigo::md32x::Md32xCore gc(g, nullptr);
      for (int f = 0; f < 30; f++) { gc.setKeys(uint32_t(rng())); gc.runFrame(); }
      gc.saveState();
    } catch (const std::exception&) {}
    roms++;
  }
  check(roms == 40, "40 ROMs de lixo sem travar");
}

// ------------------------------------------------------------ som (PWM) comparado com o PicoDrive
void audioTests(const std::string& dir) {
  std::vector<uint8_t> rom, ref;
  if (!readFile(dir + "/ref/teste5.aud", ref) || !readFile(dir + "/hb/teste5.32x", rom)) { std::printf("som: pulado\n"); return; }
  indigo::md32x::Md32xCore core(rom, nullptr);
  core.setSampleRate(44100);
  std::vector<float> mineL, mineR, l(8192), r(8192);
  for (int f = 0; f < 600; f++) {
    core.runFrame();
    size_t n = core.audioAvailable();
    while (n) {
      const size_t k = std::min<size_t>(n, 8192);
      core.pullAudio(l.data(), r.data(), k);
      for (size_t i = 0; i < k; i++) { mineL.push_back(l[i] * 32767.0f); mineR.push_back(r[i] * 32767.0f); }
      n -= k;
    }
  }
  for (int ch = 0; ch < 2; ch++) {
    const std::vector<float>& mine = ch ? mineR : mineL;
    const size_t W = 735;
    std::vector<double> ea, eb;
    for (size_t i = 0; i + W <= ref.size() / 4 && i + W <= mine.size(); i += W) {
      double sa = 0, sb = 0, ma = 0, mb = 0;
      for (size_t k = 0; k < W; k++) {
        ma += int16_t(ref[(i + k) * 4 + ch * 2] | (ref[(i + k) * 4 + ch * 2 + 1] << 8));
        mb += mine[i + k];
      }
      ma /= W; mb /= W;
      for (size_t k = 0; k < W; k++) {
        const double v = int16_t(ref[(i + k) * 4 + ch * 2] | (ref[(i + k) * 4 + ch * 2 + 1] << 8)) - ma, w = mine[i + k] - mb;
        sa += v * v; sb += w * w;
      }
      ea.push_back(std::sqrt(sa / W)); eb.push_back(std::sqrt(sb / W));
    }
    double best = -1;
    for (int sh = -3; sh <= 3; sh++) {
      double ma = 0, mb = 0; int n = 0;
      for (size_t i = 3; i + 3 < ea.size(); i++) { ma += ea[i]; mb += eb[i + sh]; n++; }
      if (!n) continue;
      ma /= n; mb /= n;
      double cc = 0, va = 0, vb = 0;
      for (size_t i = 3; i + 3 < ea.size(); i++) { const double x = ea[i] - ma, y = eb[i + sh] - mb; cc += x * y; va += x * x; vb += y * y; }
      if (va > 0 && vb > 0) best = std::max(best, cc / std::sqrt(va * vb));
    }
    double ta = 0, tb = 0;
    for (size_t i = 0; i < ea.size(); i++) { ta += ea[i]; tb += eb[i]; }
    const double ratio = ta > 0 ? tb / ta : 0;
    char buf[200];
    std::snprintf(buf, sizeof buf, "som PWM (%s): correlação do envelope %.3f, volume %.2f do PicoDrive", ch ? "direito" : "esquerdo", best, ratio);
    std::printf("  %s\n", buf);
    check(best >= 0.9 && ratio > 0.8 && ratio < 1.25, buf);
  }
}

// ------------------------------------------------------------ PNG sem compressão
void writePng(const std::string& path, const uint32_t* px, int w, int h) {
  std::vector<uint8_t> raw;
  for (int y = 0; y < h; y++) {
    raw.push_back(0);
    for (int x = 0; x < w; x++) { const uint32_t c = px[y * w + x]; raw.push_back(c & 255); raw.push_back((c >> 8) & 255); raw.push_back((c >> 16) & 255); }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
  for (size_t o = 0; o < raw.size(); o += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - o);
    z.push_back(o + n == raw.size() ? 1 : 0);
    z.push_back(n & 255); z.push_back(n >> 8); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
    z.insert(z.end(), raw.begin() + static_cast<long>(o), raw.begin() + static_cast<long>(o + n));
  }
  const uint32_t ad = (b << 16) | a;
  for (int k = 3; k >= 0; k--) z.push_back((ad >> (k * 8)) & 255);
  std::vector<uint8_t> f = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  auto chunk = [&](const char* t, const std::vector<uint8_t>& d) {
    for (int k = 3; k >= 0; k--) f.push_back((d.size() >> (k * 8)) & 255);
    std::vector<uint8_t> td(t, t + 4);
    td.insert(td.end(), d.begin(), d.end());
    f.insert(f.end(), td.begin(), td.end());
    const uint32_t c = crc32(td.data(), td.size());
    for (int k = 3; k >= 0; k--) f.push_back((c >> (k * 8)) & 255);
  };
  std::vector<uint8_t> ih = {0, 0, uint8_t(w >> 8), uint8_t(w), 0, 0, uint8_t(h >> 8), uint8_t(h), 8, 2, 0, 0, 0};
  chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<long>(f.size()));
}

// ------------------------------------------------------------ comparação com o PicoDrive
// lista.txt: nome rom quadros passo teclas [mínimo %]; ref/<nome>.ref: para cada quadro amostrado, largura e
// altura (u16) e os pontos em RGB565. Um ponto é igual quando cada componente fica a no máximo 1 nível (5 bits,
// o verde de 6 bits do PicoDrive vira 5) do PicoDrive: as duas conversões das cores de 3 bits do Mega Drive arredondam diferente.
bool samePixel(uint32_t c, uint16_t p) {
  const int r = (c & 255) >> 3, g = ((c >> 8) & 255) >> 3, b = ((c >> 16) & 255) >> 3;
  const int pr = p >> 11, pg = (p >> 6) & 31, pb = p & 31;
  return std::abs(r - pr) <= 1 && std::abs(g - pg) <= 1 && std::abs(b - pb) <= 1;
}

void refTests(const std::string& dir, const std::string& only, const std::string& pngDir) {
  std::ifstream lst(dir + "/lista.txt");
  if (!lst) { std::printf("comparação com o PicoDrive: pulada (sem %s/lista.txt)\n", dir.c_str()); return; }
  std::string linha;
  while (std::getline(lst, linha)) {
    std::istringstream ls(linha);
    std::string nome, romPath;
    int quadros, passo, minimo = 100;
    unsigned long teclas;
    if (!(ls >> nome >> romPath >> quadros >> passo >> teclas)) continue;
    ls >> minimo;
    if (!only.empty() && only != nome) continue;
    std::vector<uint8_t> rom, ref;
    if (!readFile(romPath[0] == '/' ? romPath : dir + "/" + romPath, rom) || !readFile(dir + "/ref/" + nome + ".ref", ref)) {
      check(false, nome + ": faltam arquivos");
      continue;
    }
    indigo::md32x::Md32xCore core(rom, nullptr);
    core.setSampleRate(44100);
    std::vector<float> l(8192), r(8192);
    size_t pos = 0;
    int iguais = 0, total = 0, primeiro = -1;
    long difPrimeiro = 0;
    for (int f = 0; f < quadros; f++) {
      core.setKeys((teclas && (f / 30) % 2) ? static_cast<uint32_t>(teclas) : 0);
      core.runFrame();
      size_t n = core.audioAvailable();
      while (n) { const size_t k = std::min<size_t>(n, 8192); core.pullAudio(l.data(), r.data(), k); n -= k; }
      if ((f + 1) % passo) continue;
      total++;
      if (pos + 4 > ref.size()) break;
      const int w = ref[pos] | (ref[pos + 1] << 8), h = ref[pos + 2] | (ref[pos + 3] << 8);
      pos += 4;
      const uint8_t* rp = &ref[pos];
      pos += size_t(w) * h * 2;
      if (pos > ref.size()) break;
      long dif = 0;
      if (w != core.width() || h != core.height()) {
        dif = -1;
        if (primeiro < 0) std::printf("    tamanho: picodrive %dx%d, índigo %dx%d\n", w, h, core.width(), core.height());
      } else {
        const uint32_t* fb = core.framebuffer();
        for (int i = 0; i < w * h; i++) {
          const uint16_t p = uint16_t(rp[i * 2] | (rp[i * 2 + 1] << 8));
          if (!samePixel(fb[i], p)) {
            if (!pngDir.empty() && primeiro < 0 && dif < 6)
              std::printf("    (%d,%d) picodrive %04X índigo %08X\n", i % w, i / w, p, fb[i]);
            dif++;
          }
        }
      }
      if (std::getenv("VERB")) std::printf("    quadro %d: %ld\n", f + 1, dif);
      if (dif == 0) iguais++;
      else if (primeiro < 0) {
        primeiro = f + 1; difPrimeiro = dif;
        if (!pngDir.empty()) {
          writePng(pngDir + "/" + nome + "-indigo.png", core.framebuffer(), core.width(), core.height());
          if (w > 0 && h > 0) {
            std::vector<uint32_t> px(size_t(w) * h);
            for (int i = 0; i < w * h; i++) {
              const uint16_t p = uint16_t(rp[i * 2] | (rp[i * 2 + 1] << 8));
              px[i] = 0xFF000000u | ((p >> 11) << 3) | (((p >> 5) & 63) << 10) | ((p & 31) << 19);
            }
            writePng(pngDir + "/" + nome + "-picodrive.png", px.data(), w, h);
          }
        }
      }
    }
    char buf[200];
    if (primeiro < 0) std::snprintf(buf, sizeof buf, "%s: %d de %d quadros iguais ao PicoDrive", nome.c_str(), iguais, total);
    else if (minimo < 100) std::snprintf(buf, sizeof buf, "%s: %d de %d quadros iguais (mínimo %d%%)", nome.c_str(), iguais, total, minimo);
    else std::snprintf(buf, sizeof buf, "%s: %d de %d quadros iguais (primeira diferença no quadro %d: %ld pontos%s)",
                       nome.c_str(), iguais, total, primeiro, difPrimeiro, difPrimeiro < 0 ? ", tamanho" : "");
    std::printf("  %s\n", buf);
    check(primeiro < 0 || iguais * 100 >= minimo * total, buf);
  }
}

// ferramenta: indigo-t7_32x rom.32x shot saida.png [quadros] [teclas]
int shot(int argc, char** argv) {
  std::vector<uint8_t> rom;
  if (!readFile(argv[1], rom)) { std::printf("não abriu %s\n", argv[1]); return 1; }
  indigo::md32x::Md32xCore core(rom, nullptr);
  core.setSampleRate(44100);
  const int frames = argc > 4 ? std::atoi(argv[4]) : 300;
  const uint32_t keys = argc > 5 ? static_cast<uint32_t>(std::strtoul(argv[5], nullptr, 0)) : 0;
  std::vector<float> l(4096), r(4096);
  const auto t0 = std::chrono::steady_clock::now();
  for (int f = 0; f < frames; f++) {
    core.setKeys((keys && (f / 30) % 2) ? keys : 0);
    core.runFrame();
    size_t n = core.audioAvailable();
    while (n) { const size_t k = std::min<size_t>(n, 4096); core.pullAudio(l.data(), r.data(), k); n -= k; }
  }
  writePng(argv[3], core.framebuffer(), core.width(), core.height());
  if (const char* pk = std::getenv("PEEK")) {  // PEEK=060005cc,060005c4: palavras longas da SDRAM
    std::istringstream ps(pk);
    std::string item;
    while (std::getline(ps, item, ',')) {
      const uint32_t a = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16)) & 0x3FFFC;
      const auto& s = core.mars().sdram();
      std::printf("[%s] = %08X\n", item.c_str(), (uint32_t(s[a]) << 24) | (s[a + 1] << 16) | (s[a + 2] << 8) | s[a + 3]);
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  auto& m = core.mars();
  std::printf("%d quadros em %.2f s (%.0f quadros/s)\n", frames, secs, frames / (secs > 0 ? secs : 1));
  std::printf("%s: %dx%d \"%s\" 68k pc=%06X msh2 pc=%08X ssh2 pc=%08X comm=%04X %04X %04X %04X %04X %04X %04X %04X\n", argv[3],
              core.width(), core.height(), core.title().c_str(), core.md().cpu.pc, m.msh2.pc, m.ssh2.pc, m.comm(0), m.comm(1), m.comm(2),
              m.comm(3), m.comm(4), m.comm(5), m.comm(6), m.comm(7));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::printf("uso: indigo-t7_32x pasta [cpu|peri|ref|estado|cart] [nome]\n"); return 2; }
  if (argc >= 4 && std::string(argv[2]) == "shot") return shot(argc, argv);
  const std::string dir = argv[1];
  const std::string part = argc > 2 ? argv[2] : "";
  if (part.empty() || part == "cpu") cpuTests(dir, argc > 3 ? argv[3] : "");
  if (part.empty() || part == "peri") periTests();
  if (part.empty() || part == "cart") cartTests();
  if (part.empty() || part == "estado") stateTests(dir);
  if (part.empty() || part == "som") audioTests(dir);
  if (part.empty() || part == "ref") refTests(dir, argc > 3 ? argv[3] : "", argc > 4 ? argv[4] : "");
  std::printf("RESULTADO 32x: %ld verificações, %ld falhas\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
