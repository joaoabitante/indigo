// Testes do núcleo Tiger Game.com.
//   indigo-t7_gamecom [pasta]          roda todas as verificações (pasta = /root/indigo-env/roms/gamecom)
//   indigo-t7_gamecom gerar <pasta>    grava a BIOS de teste (própria) e o cartucho de teste na pasta
//
// Não há BIOS livre nem ROMs de teste públicas do Game.com: a BIOS de teste (ROM interna + ROM do sistema) e o
// cartucho de teste são montados aqui mesmo, com um montador mínimo de SM8500. As instruções são conferidas
// uma a uma contra resultados calculados à mão (não existe suíte pública como a SingleStepTests para o SM8500).
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../src/core/core.h"
#include "../src/gamecom/gamecom.h"

using namespace indigo;
using namespace indigo::gamecom;

namespace {
int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) {
    fails++;
    std::printf("FALHA: %s\n", what.c_str());
  }
}
std::string hx(unsigned v) {
  char b[16];
  std::snprintf(b, sizeof b, "%X", v);
  return b;
}

// ------------------------------------------------------------------ montador mínimo
struct Asm {
  uint16_t org;
  std::vector<uint8_t> out;
  std::map<std::string, uint16_t> labels;
  struct Fix { size_t at; std::string label; bool rel; };
  std::vector<Fix> fixes;
  explicit Asm(uint16_t o) : org(o) {}
  uint16_t here() const { return uint16_t(org + out.size()); }
  Asm& b(std::initializer_list<int> bs) { for (int x : bs) out.push_back(uint8_t(x)); return *this; }
  Asm& label(const std::string& l) { labels[l] = here(); return *this; }
  Asm& w(uint16_t v) { out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v)); return *this; }  // palavra (alto primeiro)
  Asm& abs(const std::string& l) { fixes.push_back({out.size(), l, false}); return w(0); }
  Asm& rel(const std::string& l) { fixes.push_back({out.size(), l, true}); out.push_back(0); return *this; }
  // instruções usadas
  Asm& mov(int r, int imm) { return b({0x58, imm, r}); }            // MOV R,#i
  Asm& movr(int d, int s) { return b({0x48, s, d}); }               // MOV Rd,Rs
  Asm& movw(int r, uint16_t v) { b({0x4B, r}); return w(v); }       // MOVW RR,#w
  Asm& ldAbs(int r, uint16_t a) { b({0x38, 0x80 | (r << 3)}); return w(a); }  // MOV r(0-7),@ww
  Asm& stAbs(int r, uint16_t a) { b({0x39, 0x80 | (r << 3)}); return w(a); }  // MOV @ww,r(0-7)
  Asm& call(const std::string& l) { b({0x49}); return abs(l); }
  Asm& callA(uint16_t a) { b({0x49}); return w(a); }
  Asm& jmp(int cc, const std::string& l) { b({0x90 | cc}); return abs(l); }
  Asm& br(int cc, const std::string& l) { b({0xD0 | cc}); return rel(l); }
  Asm& dbnz(int r, const std::string& l) { b({0x70 | r}); return rel(l); }
  Asm& cmp(int r, int imm) { return b({0x50, imm, r}); }
  Asm& inc(int r) { return b({0x08, r}); }
  void finish() {
    for (const Fix& f : fixes) {
      const auto it = labels.find(f.label);
      if (it == labels.end()) { std::printf("FALHA: rótulo %s\n", f.label.c_str()); fails++; continue; }
      if (f.rel) {
        out[f.at] = uint8_t(it->second - (org + f.at + 1));
      } else {
        out[f.at] = uint8_t(it->second >> 8);
        out[f.at + 1] = uint8_t(it->second);
      }
    }
  }
};
enum { CC_F = 0, CC_LT, CC_LE, CC_ULE, CC_OV, CC_MI, CC_EQ, CC_C, CC_T, CC_GE, CC_GT, CC_UGT, CC_NOV, CC_PL, CC_NE, CC_NC };

// ------------------------------------------------------------------ barramento plano para a CPU sozinha
struct FlatBus : Sm8500Bus {
  uint8_t mem[0x10000] = {};
  int ticks = 0;
  uint8_t read(uint16_t a) override { return mem[a]; }
  void write(uint16_t a, uint8_t v) override { mem[a] = v; }
  void tick(int c, bool) override { ticks += c; }
};

struct CpuRig {
  FlatBus bus;
  Sm8500 cpu{&bus};
  CpuRig() { cpu.reset(); cpu.pc = 0x4000; cpu.sys = 0x40; cpu.sph = 0x03; cpu.spl = 0x00; }
  // roda as instruções em bytes; devolve os ciclos da última
  int run(std::initializer_list<int> code, int steps = 1) {
    uint16_t a = cpu.pc;
    for (int x : code) bus.mem[a++] = uint8_t(x);
    int c = 0;
    for (int i = 0; i < steps; i++) c = cpu.step();
    return c;
  }
  uint8_t r(int n) { return cpu.rd(uint16_t(n)); }
  void set(int n, uint8_t v) { cpu.wr(uint16_t(n), v); }
};
constexpr uint8_t C = 0x80, Z = 0x40, S = 0x20, V = 0x10, D = 0x08, H = 0x04, B = 0x02, I = 0x01;
const uint8_t ARITH = C | Z | S | V | D | H;

// confere resultado e flags aritméticas (máscara ARITH)
void op8(const std::string& name, std::initializer_list<int> code, uint8_t a, uint8_t bv, uint8_t flagsIn, uint8_t expect,
         uint8_t flagsOut, int cyc) {
  CpuRig t;
  t.set(0x80, a);
  t.set(0x81, bv);
  t.cpu.ps1 = flagsIn;
  const int c = t.run(code);
  check(t.r(0x80) == expect, name + ": resultado " + hx(t.r(0x80)) + " (esperado " + hx(expect) + ")");
  check((t.cpu.ps1 & ARITH) == flagsOut, name + ": flags " + hx(t.cpu.ps1) + " (esperado " + hx(flagsOut) + ")");
  check(c == cyc, name + ": ciclos " + std::to_string(c));
}

void cpuTests() {
  // ---- ULA de 8 bits (operandos em $80 = destino, $81 = origem): 41 = ADD Rd,Rs (origem primeiro)
  op8("ADD 7F+01", {0x41, 0x81, 0x80}, 0x7F, 0x01, 0, 0x80, S | V | H, 6);
  op8("ADD FF+01", {0x41, 0x81, 0x80}, 0xFF, 0x01, 0, 0x00, C | Z | H, 6);
  op8("ADD 80+80", {0x41, 0x81, 0x80}, 0x80, 0x80, D, 0x00, C | Z | V, 6);
  op8("ADC 10+20+C", {0x43, 0x81, 0x80}, 0x10, 0x20, C, 0x31, 0, 6);
  op8("ADC 7F+00+C (V)", {0x43, 0x81, 0x80}, 0x7F, 0x00, C, 0x80, S | V | H, 6);
  op8("SUB 10-01", {0x42, 0x81, 0x80}, 0x10, 0x01, 0, 0x0F, D | H, 6);
  op8("SUB 00-01", {0x42, 0x81, 0x80}, 0x00, 0x01, 0, 0xFF, C | S | D | H, 6);
  op8("SUB 80-01 (V)", {0x42, 0x81, 0x80}, 0x80, 0x01, 0, 0x7F, V | D | H, 6);
  op8("SBC 05-02-C", {0x44, 0x81, 0x80}, 0x05, 0x02, C, 0x02, D, 6);
  op8("CMP 05,05 (Z, D mantido)", {0x40, 0x81, 0x80}, 0x05, 0x05, D | H, 0x05, Z | D | H, 6);
  op8("CMP 01,02 (C)", {0x40, 0x81, 0x80}, 0x01, 0x02, 0, 0x01, C | S, 6);
  op8("AND F0&3C (C mantido)", {0x45, 0x81, 0x80}, 0xF0, 0x3C, C | V, 0x30, C, 6);
  op8("OR 80|01", {0x46, 0x81, 0x80}, 0x80, 0x01, 0, 0x81, S, 6);
  op8("XOR AA^AA", {0x47, 0x81, 0x80}, 0xAA, 0xAA, 0, 0x00, Z, 6);
  op8("MOV Rd,Rs", {0x48, 0x81, 0x80}, 0x11, 0x22, C | Z, 0x22, C | Z, 6);
  // com imediato (5x imm reg)
  op8("ADD #", {0x51, 0x05, 0x80}, 0x0B, 0, 0, 0x10, H, 6);
  op8("SUB #", {0x52, 0x05, 0x80}, 0x05, 0, 0, 0x00, Z | D, 6);
  op8("CMP # (GT)", {0x50, 0x03, 0x80}, 0x05, 0, 0, 0x05, 0, 6);
  op8("AND #", {0x55, 0x0F, 0x80}, 0x3C, 0, 0, 0x0C, 0, 6);
  op8("MOV #", {0x58, 0x99, 0x80}, 0x00, 0, 0, 0x99, 0, 6);
  // unárias
  op8("INC 7F (V)", {0x08, 0x80}, 0x7F, 0, C, 0x80, C | S | V, 4);
  op8("INC FF (Z)", {0x08, 0x80}, 0xFF, 0, 0, 0x00, Z, 4);
  op8("DEC 80 (V)", {0x09, 0x80}, 0x80, 0, 0, 0x7F, V, 4);
  op8("DEC 01 (Z)", {0x09, 0x80}, 0x01, 0, 0, 0x00, Z, 4);
  op8("NEG 01", {0x01, 0x80}, 0x01, 0, 0, 0xFF, S, 5);
  op8("NEG 00", {0x01, 0x80}, 0x00, 0, 0, 0x00, C | Z, 5);
  op8("NEG 80 (V)", {0x01, 0x80}, 0x80, 0, 0, 0x80, S | V, 5);
  op8("COM 0F", {0x02, 0x80}, 0x0F, 0, C, 0xF0, C | S, 4);
  op8("CLR", {0x00, 0x80}, 0x5A, 0, C | Z, 0x00, C | Z, 4);
  op8("RR 01", {0x03, 0x80}, 0x01, 0, 0, 0x80, C | S, 4);
  op8("RL 81", {0x04, 0x80}, 0x81, 0, 0, 0x03, C | V, 4);
  op8("RRC 02 com C", {0x05, 0x80}, 0x02, 0, C, 0x81, S, 4);
  op8("RLC 80 sem C", {0x06, 0x80}, 0x80, 0, 0, 0x00, C | Z | V, 4);
  op8("SRL 81", {0x07, 0x80}, 0x81, 0, S, 0x40, C, 4);
  op8("SRA 81", {0x0A, 0x80}, 0x81, 0, 0, 0xC0, C | S, 4);
  op8("SLL 41", {0x0B, 0x80}, 0x41, 0, 0, 0x82, S, 4);
  op8("SWAP 1E", {0x0D, 0x80}, 0x1E, 0, Z, 0xE1, Z, 7);
  op8("DA 0A (depois de soma)", {0x0C, 0x80}, 0x0A, 0, 0, 0x10, 0, 4);
  op8("DA 9A (depois de soma)", {0x0C, 0x80}, 0x9A, 0, 0, 0x00, C | Z, 4);
  op8("DA 0F (subtração, H)", {0x0C, 0x80}, 0x0F, 0, D | H, 0x09, D | H, 4);

  // ---- forma curta "Rr,Rs" (registradores 0-7): 11 = ADD r1,r2  (byte = r1<<3 | r2)
  {
    CpuRig t;
    t.set(1, 0x30);
    t.set(2, 0x12);
    const int c = t.run({0x11, (1 << 3) | 2});
    check(t.r(1) == 0x42 && c == 5, "ADD curto r1,r2");
  }
  // ---- janela dos registradores (PS0)
  {
    CpuRig t;
    t.run({0x2E, 0x28});  // MOV PS0,#$28
    check(t.cpu.ps0 == 0x28, "MOV PS0,#");
    t.run({0xC3, 0x77});  // MOV r3,#$77
    check(t.cpu.regs[0x28 + 3] == 0x77 && t.r(3) == 0x77, "r3 com PS0=$28 fica no banco $2B");
    t.cpu.ps0 = 0;
    check(t.r(3) == 0, "r3 com PS0=0 é outro byte");
  }
  // ---- 16 bits
  {
    CpuRig t;
    t.run({0x78, 0x12, 0x34});  // MOVW rr0,#$1234
    check(t.r(0) == 0x12 && t.r(1) == 0x34, "MOVW rr0,#w (alto em r0)");
    t.run({0x7A, 0x00, 0x01});  // MOVW rr2,#1
    t.cpu.ps1 = 0;
    int c = t.run({0x61, 0x02, 0x00});  // ADDW RR0,RR2
    check(t.cpu.rdw(0) == 0x1235 && c == 10, "ADDW RR0,RR2");
    t.run({0x69, 0x00, 0xED, 0xCB});  // ADDW RR0,#$EDCB -> $0000, C
    check(t.cpu.rdw(0) == 0x0000 && (t.cpu.ps1 & (C | Z)) == (C | Z), "ADDW # com estouro: Z e C");
    t.run({0x6A, 0x00, 0x00, 0x01});  // SUBW RR0,#1 -> FFFF
    check(t.cpu.rdw(0) == 0xFFFF && (t.cpu.ps1 & (C | S | D)) == (C | S | D), "SUBW # com empréstimo");
    t.run({0x68, 0x00, 0xFF, 0xFF});  // CMPW RR0,#$FFFF
    check(t.cpu.rdw(0) == 0xFFFF && (t.cpu.ps1 & Z), "CMPW # igual");
    t.run({0x6D, 0x00, 0x0F, 0xF0});  // ANDW RR0,#$0FF0
    check(t.cpu.rdw(0) == 0x0FF0 && !(t.cpu.ps1 & Z), "ANDW #");
    t.run({0x18, 0x00});  // INCW RR0
    check(t.cpu.rdw(0) == 0x0FF1, "INCW");
    t.run({0x19, 0x02});  // DECW RR2 (1 -> 0)
    check(t.cpu.rdw(2) == 0 && (t.cpu.ps1 & Z), "DECW até zero");
    t.run({0x7A, 0x00, 0x85});
    t.run({0x2C, 0x02});  // EXTS RR2
    check(t.cpu.rdw(2) == 0xFF85, "EXTS negativo");
    t.run({0x4A, 0x00, 0x04});  // MOVW RR4,RR0
    check(t.cpu.rdw(4) == 0x0FF1, "MOVW RR4,RR0");
    t.run({0x3C, (6 << 3) | 4});  // MOVW curto rr6 (índice 6), rr4 (índice 4)
    check(t.cpu.rdw(6) == 0x0FF1, "MOVW curto");
    t.run({0x4B, 0x80, 0xBE, 0xEF});  // MOVW $80,#$BEEF
    check(t.bus.mem[0x80] == 0xBE && t.bus.mem[0x81] == 0xEF, "MOVW R,#w na RAM");
  }
  // ---- multiplicação e divisão
  {
    CpuRig t;
    t.run({0x78, 0x99, 0x10});        // rr0 = $9910 (byte baixo = $10)
    t.run({0x4D, 0x20, 0x00});        // MULT RR0,#$20 -> $0200
    check(t.cpu.rdw(0) == 0x0200 && !(t.cpu.ps1 & Z), "MULT RR0,#");
    t.run({0x78, 0x00, 0x64});        // rr0 = 100
    t.run({0x5D, 0x07, 0x00});        // DIV RR0,#7
    check(t.cpu.rdw(0) == 14, "DIV RR0,# (100/7)");
    t.run({0x78, 0x00, 0x64});
    t.run({0x7A, 0x00, 0x09});        // rr2 = 9 (divisor no byte baixo)
    t.run({0x5C, 0x02, 0x00});        // DIV RR0,RR2
    check(t.cpu.rdw(0) == 11 && t.r(2) == 1, "DIV RR0,RR2 (quociente 11, resto 1 no byte alto)");
    t.run({0x7A, 0x00, 0x00});
    t.run({0x5C, 0x02, 0x00});
    check(t.cpu.ps1 & V, "DIV por zero liga V");
  }
  // ---- modos de endereçamento de 8 bits (28 = MOV r,mem)
  {
    CpuRig t;
    for (int i = 0; i < 8; i++) t.bus.mem[0x90 + i] = uint8_t(0xA0 + i);
    t.set(2, 0x92);
    t.run({0x28, (1 << 3) | 2});              // MOV r1,@r2
    check(t.r(1) == 0xA2, "MOV r,@r");
    t.run({0x28, 0x40 | (1 << 3) | 2});       // MOV r1,(r2)+
    check(t.r(1) == 0xA2 && t.r(2) == 0x93, "MOV r,(r)+");
    t.run({0x28, 0xC0 | (1 << 3) | 2});       // MOV r1,-(r2)
    check(t.r(1) == 0xA2 && t.r(2) == 0x92, "MOV r,-(r) (pré-decremento)");
    t.run({0x28, 0x80 | (1 << 3) | 2, 0x03}); // MOV r1,3(r2)
    check(t.r(1) == 0xA5, "MOV r,d(r)");
    t.run({0x28, 0x80 | (1 << 3), 0x96});     // MOV r1,@$96
    check(t.r(1) == 0xA6, "MOV r,@w");
    t.set(3, 0x55);
    t.run({0x29, (3 << 3) | 2});              // MOV @r2,r3
    check(t.bus.mem[0x92] == 0x55, "MOV @r,r");
    t.set(4, 0x01);
    t.run({0x21, (4 << 3) | 2});              // ADD r4,@r2
    check(t.r(4) == 0x56, "ADD r,@r");
  }
  // ---- modos de 16 bits (38 = MOV r,mem; 3A = MOVW)
  {
    CpuRig t;
    for (int i = 0; i < 8; i++) t.bus.mem[0x5000 + i] = uint8_t(0xB0 + i);
    t.run({0x7C, 0x50, 0x02});                 // rr4 (índice 4) = $5002
    t.run({0x38, (1 << 3) | 4});               // MOV r1,@rr8
    check(t.r(1) == 0xB2, "MOV r,@rr");
    t.run({0x38, 0x40 | (1 << 3) | 4});        // MOV r1,(rr8)+
    check(t.r(1) == 0xB2 && t.cpu.rdw(4) == 0x5003, "MOV r,(rr)+");
    t.run({0x38, 0xC0 | (1 << 3) | 4});        // MOV r1,-(rr8)
    check(t.r(1) == 0xB2 && t.cpu.rdw(4) == 0x5002, "MOV r,-(rr)");
    t.run({0x38, 0x80 | (1 << 3) | 4, 0x00, 0x04});  // MOV r1,$0004(rr8)
    check(t.r(1) == 0xB6, "MOV r,ww(rr)");
    t.run({0x38, 0x80 | (1 << 3), 0x50, 0x07});      // MOV r1,@$5007
    check(t.r(1) == 0xB7, "MOV r,@ww");
    t.run({0x3A, (0 << 3) | 4});               // MOVW rr0,@rr8
    check(t.cpu.rdw(0) == 0xB2B3, "MOVW rr,@rr");
    t.run({0x3A, 0x40 | (0 << 3) | 4});        // MOVW rr0,(rr8)+  (avança 2)
    check(t.cpu.rdw(4) == 0x5004, "MOVW rr,(rr)+ avança 2");
    t.set(3, 0x99);
    t.run({0x39, 0x40 | (3 << 3) | 4});        // MOV (rr8)+,r3
    check(t.bus.mem[0x5004] == 0x99 && t.cpu.rdw(4) == 0x5005, "MOV (rr)+,r");
  }
  // ---- pilha, chamadas e desvios
  {
    CpuRig t;
    t.set(0x80, 0x5A);
    t.run({0x0E, 0x80});  // PUSH $80
    check(t.cpu.sp() == 0x02FF && t.bus.mem[0x02FF] == 0x5A, "PUSH (pilha de 16 bits)");
    t.run({0x0F, 0x81});  // POP $81
    check(t.cpu.sp() == 0x0300 && t.bus.mem[0x81] == 0x5A, "POP");
    t.cpu.pc = 0x4000;
    t.run({0x49, 0x45, 0x00});  // CALL $4500
    check(t.cpu.pc == 0x4500 && t.bus.mem[0x2FE] == 0x40 && t.bus.mem[0x2FF] == 0x03, "CALL empilha PC (alto no topo)");
    int c = t.run({0xF8});  // RET
    check(t.cpu.pc == 0x4003 && t.cpu.sp() == 0x300 && c == 10, "RET");
    t.run({0xE3, 0x21});  // CALS $1321
    check(t.cpu.pc == 0x1321, "CALS $13xx");
    t.cpu.pc = 0x4000;
    t.cpu.ps1 = Z;
    t.run({0x96, 0x47, 0x00});  // JMP EQ,$4700
    check(t.cpu.pc == 0x4700, "JMP EQ tomado");
    t.cpu.pc = 0x4000;
    t.run({0x9E, 0x47, 0x00});  // JMP NE (não)
    check(t.cpu.pc == 0x4003, "JMP NE não tomado");
    t.cpu.pc = 0x4000;
    c = t.run({0xD8, 0xFE});  // BR T,-2
    check(t.cpu.pc == 0x4000 && c == 8, "BR T para trás");
    t.cpu.pc = 0x4000;
    t.set(3, 2);
    t.run({0x73, 0xFE});  // DBNZ r3,-2
    check(t.r(3) == 1 && t.cpu.pc == 0x4000, "DBNZ com resto");
    t.run({0x73, 0xFE});
    check(t.r(3) == 0 && t.cpu.pc == 0x4002, "DBNZ chega a zero");
    t.cpu.pc = 0x4000;
    t.set(0x80, 0x04);
    t.run({0x8A, 0x80, 0x10});  // BBS $80,#2,+16
    check(t.cpu.pc == 0x4013, "BBS bit ligado");
    t.cpu.pc = 0x4000;
    t.run({0x82, 0x80, 0x10});  // BBC $80,#2,+16
    check(t.cpu.pc == 0x4003, "BBC bit ligado não desvia");
    t.cpu.pc = 0x4000;
    t.run({0xAF, 0x80});  // BSET $80,#7
    t.run({0xA2, 0x80});  // BCLR $80,#2
    check(t.bus.mem[0x80] == 0x80, "BSET/BCLR R,#b");
    t.bus.mem[0xFF20] = 0;
    t.run({0x1D, 0x03, 0x20});  // BSET $FF20,#3
    check(t.bus.mem[0xFF20] == 0x08, "BSET $FFdd,#b");
    t.set(2, 0x10);
    t.run({0x1C, (2 << 3) | 3, 0x10});  // BCLR $10(r2),#3 -> $0020? (endereço $20)
    t.bus.mem[0xA0] = 0x08;
    t.run({0x2B, (2 << 3) | 3, 0x90, 0x05});  // BBS $90(r2),#3,+5 -> lê $A0
    check(t.cpu.pc == 0x4013, "BBS d(r),#b");
    t.cpu.pc = 0x4000;
    t.set(0x80, 0x00);
    t.run({0x2F, 0x80, 0x01});  // BTST $80,#1
    check(t.cpu.ps1 & Z, "BTST bit zerado");
    t.set(0x82, 0xFF);
    t.run({0x5F, 0x82, 0x0F, 0x50});  // MOVM $82,#$0F,#$50
    check(t.bus.mem[0x82] == 0x5F, "MOVM R,#mask,#i");
    t.cpu.ps1 = 0;
    t.set(0x83, 0x10);
    t.run({0x4E, 0x04, 0x83});  // BMOV BF,$83,#4
    check(t.cpu.ps1 & B, "BMOV BF <- bit");
    t.run({0x4E, 0x40 | 0x01, 0x84});  // BMOV $84,#1,BF
    check(t.bus.mem[0x84] == 0x02, "BMOV bit <- BF");
    t.run({0x1E, 0x82});  // PUSHW $82
    t.run({0x1F, 0x86});  // POPW $86
    check(t.bus.mem[0x86] == 0x5F && t.bus.mem[0x87] == t.bus.mem[0x83], "PUSHW/POPW");
    t.run({0xFC});
    check(t.cpu.ps1 & C, "SETC");
    t.run({0xFB});
    check(!(t.cpu.ps1 & C), "COMC");
    // JMP @RR: 3E com byte 00000nnn
    t.run({0x7E, 0x46, 0x66});  // rr6 (índice 6)
    t.run({0x3E, 0x06});
    check(t.cpu.pc == 0x4666, "JMP RRn");
    // modo indireto 5B (MOV (r)+,#i como em 5B 42 00)
    t.cpu.pc = 0x4000;
    t.set(2, 0xE9);
    t.bus.mem[0xE9] = 0x77;
    t.run({0x5B, 0x42, 0x00});
    check(t.bus.mem[0xE9] == 0x00 && t.r(2) == 0xEA, "5B 42 00: MOV (r2)+,#0");
    t.set(2, 0x09);
    t.bus.mem[0x09 + 0] = 0;  // (endereço 9 é o registrador r9)
    t.set(9, 0x33);
    t.run({0x5A, 0x02, 0x33});  // CMP @r2,#$33
    check(t.cpu.ps1 & Z, "5A 02: CMP @r2,#i");
  }
  // ---- condições (todas as 16)
  {
    struct Cc { int cc; uint8_t flags; bool expect; };
    const Cc cases[] = {
      {CC_F, 0, false}, {CC_T, 0, true}, {CC_LT, S, true}, {CC_LT, S | V, false}, {CC_LE, Z, true},
      {CC_LE, 0, false}, {CC_ULE, C, true}, {CC_ULE, 0, false}, {CC_OV, V, true}, {CC_MI, S, true},
      {CC_EQ, Z, true}, {CC_C, C, true}, {CC_GE, S | V, true}, {CC_GE, S, false}, {CC_GT, 0, true},
      {CC_GT, Z, false}, {CC_UGT, 0, true}, {CC_UGT, C, false}, {CC_NOV, 0, true}, {CC_PL, S, false},
      {CC_NE, Z, false}, {CC_NC, 0, true},
    };
    for (const Cc& k : cases) {
      CpuRig t;
      t.cpu.ps1 = k.flags;
      t.run({0xD0 | k.cc, 0x10});
      check((t.cpu.pc == 0x4012) == k.expect, "condição " + std::to_string(k.cc) + " com flags " + hx(k.flags));
    }
  }
  // ---- interrupções
  {
    CpuRig t;
    t.bus.mem[0x100E] = 0x48;  // vetor do LCDC -> $4800
    t.bus.mem[0x100F] = 0x00;
    t.cpu.ie0 = 0x01;
    t.cpu.ps1 = I | C;
    for (int k = 0; k < 4; k++) t.bus.mem[0x4000 + k] = 0xFF;
    t.bus.mem[0x4800] = 0xF9;  // IRET
    t.cpu.irq(Sm8500::IRQ_LCDC);
    check(t.cpu.ir0 & 0x01, "pedido marca IR0 bit 0");
    t.cpu.step();  // desvia e executa o IRET
    check(t.cpu.pc == 0x4000 && t.cpu.ps1 == (I | C) && t.cpu.sp() == 0x300, "interrupção + IRET restauram PC e PS1");
    t.cpu.ps1 = 0;  // I desligado: fica pendente
    t.cpu.irq(Sm8500::IRQ_LCDC);
    t.cpu.step();
    check(t.cpu.pc == 0x4001 && t.cpu.pending, "com I=0 o pedido espera");
    t.cpu.ps1 = I;
    t.cpu.ps0 = 5;  // prioridade 5 bloqueia o LCDC (precisa < 5)
    t.cpu.step();
    check(t.cpu.pc == 0x4002, "prioridade em PS0 bloqueia o LCDC");
    t.cpu.ps0 = 0;
    t.bus.mem[0x4002] = 0xFF;
    t.cpu.step();
    check(t.cpu.pc == 0x4000 + 2, "atendido depois de liberar (e voltou do IRET)");
    t.cpu.ie0 = 0;
    t.cpu.irq(Sm8500::IRQ_LCDC);
    t.cpu.step();
    check(!t.cpu.pending, "fonte desligada em IE0: pedido descartado");
    t.bus.mem[0x4003] = 0xF1;  // HALT
    t.cpu.pc = 0x4003;
    t.cpu.step();
    check(t.cpu.halted, "HALT");
    t.cpu.step();
    check(t.cpu.halted && t.cpu.pc == 0x4004, "parado sem pedido");
    t.cpu.ie0 = 0x01;
    t.cpu.irq(Sm8500::IRQ_LCDC);
    t.cpu.step();
    check(!t.cpu.halted && t.cpu.pc == 0x4000 + 4, "pedido acorda do HALT e volta");
    // pedido esperando (I = 0) some quando o programa zera o bit em IR0
    t.cpu.ps1 = 0;
    t.cpu.irq(Sm8500::IRQ_LCDC);
    t.bus.mem[0x4004] = 0xCA;  // MOV IR0,#0
    t.bus.mem[0x4005] = 0x00;
    t.cpu.step();
    check(!t.cpu.pending && t.cpu.ir0 == 0, "zerar IR0 cancela o pedido pendente");
    t.cpu.ps1 = I;
    t.bus.mem[0x4006] = 0xFF;
    t.cpu.step();
    check(t.cpu.pc == 0x4007, "nada é atendido depois de cancelado");
  }
}

// ------------------------------------------------------------------ BIOS e cartucho de teste (próprios)
// RAM usada: $90 contador de DMA, $91 quadros, $92 timer 0, $93 rotina da ROM do sistema, $94 relógio
std::vector<uint8_t> buildInternal() {
  Asm a(0x1000);
  // vetores (palavras com o byte alto primeiro)
  a.abs("isr_dma").abs("isr_tim0").w(0).abs("isr_none").abs("isr_none").w(0).w(0).abs("isr_lcdc");  // $1000-$100E
  a.w(0).abs("isr_none").w(0).abs("isr_ck").w(0).abs("isr_none").abs("isr_none").abs("isr_none");  // $1010-$101E
  while (a.here() < 0x1020) a.b({0xFF});
  a.label("reset");
  a.mov(0x19, 0x40);           // SYS: pilha de 16 bits
  a.mov(0x1C, 0x04).mov(0x1D, 0x00);  // SP = $0400
  a.b({0x2E, 0x00});           // PS0 = 0
  a.b({0xCF, 0x40});           // P3 = $40: cartucho 1
  a.mov(0x26, 0x20);           // MMU2 = $20: $4000 = cartucho ($40000)
  // confere "TigerDMGC" em $4005
  const char* sig = "TigerDMGC";
  for (int i = 0; i < 9; i++) {
    a.ldAbs(0, uint16_t(0x4005 + i));
    a.cmp(0x00, sig[i]);
    a.jmp(CC_NE, "semcart");
  }
  // entrada: palavra em $4002 (byte baixo primeiro, como no cabeçalho)
  a.ldAbs(1, 0x4002).ldAbs(0, 0x4003);  // r0 = alto, r1 = baixo -> rr0
  a.b({0xC2, 0x01});            // r2 = 1 (entrou pelo menu)
  a.b({0x3F, 0x00});            // CALL rr0
  a.label("fim").b({0xF1}).jmp(CC_T, "fim");
  a.label("semcart");
  a.b({0xC0, 0xEE}).stAbs(0, 0xE000);  // sem cartucho válido: marca erro
  a.label("loopx").jmp(CC_T, "loopx");
  a.label("isr_dma").inc(0x90).b({0xF9});
  a.label("isr_tim0").inc(0x92).b({0xF9});
  a.label("isr_lcdc").inc(0x91).b({0xF9});
  a.label("isr_ck").inc(0x94).b({0xF9});
  a.label("isr_none").b({0xF9});
  a.finish();
  std::vector<uint8_t> rom(INT_ROM_SIZE, 0xFF);
  std::memcpy(rom.data(), a.out.data(), a.out.size());
  return rom;
}

std::vector<uint8_t> buildExternal() {
  std::vector<uint8_t> rom(EXT_ROM_SIZE, 0x00);
  Asm a(0x2000);  // banco 0, visto em $2000
  a.mov(0x93, 0x5A).b({0xF8});  // "chamada do sistema" de teste
  a.finish();
  std::memcpy(rom.data(), a.out.data(), a.out.size());
  // banco de 16 KB nº 1: imagem de 2 bits (64 bytes por linha): um quadrado de pixels 2 (bytes $AA)
  for (int y = 0; y < 8; y++)
    for (int x = 0; x < 2; x++) rom[0x4000 + size_t(y) * 64 + size_t(x)] = 0xAA;
  return rom;
}

// cartucho de 64 KB; cabeçalho em 0 (a BIOS de teste o vê em $4000 pelo espelho em $40000)
std::vector<uint8_t> buildCart() {
  std::vector<uint8_t> rom(0x10000, 0x00);
  Asm a(0x4000);
  a.b({0x08, 0x20}).b({0x20, 0x40});  // tamanho/banco, entrada $4020 (byte baixo primeiro)
  a.b({0x03});
  for (char ch : std::string("TigerDMGC")) a.b({ch});
  a.b({0x24, 0, 160});
  for (char ch : std::string("TESTE IND")) a.b({ch});
  a.b({0x1A, 0x18, 0x97, 0, 0, 0});
  while (a.here() < 0x4020) a.b({0});
  a.label("entry");
  a.cmp(0x02, 1).br(CC_EQ, "go").b({0xF8});
  a.label("go");
  a.mov(0x30, 0x80);  // LCDC: ligado, página 0, paleta 0
  a.mov(0x31, 0x07);
  // enche a página 0 com $1B (pixels 0,1,2,3)
  a.movw(0x08, 0xA000);            // rr8 = $A000
  a.b({0xC3, 0x1B});               // r3 = $1B
  a.movw(0x0A, 8000);              // rr10 = contador
  a.label("fill");
  a.b({0x39, 0x40 | (3 << 3) | 1}); // MOV (rr8)+,r3 (índice 1 = RR8)
  a.b({0x19, 0x0A});               // DECW rr10
  a.br(CC_NE, "fill");
  // interrupções: DMA, timer 0, LCDC; relógio
  a.mov(0x10, 0x80 | 0x40 | 0x01);
  a.mov(0x11, 0x10);
  a.b({0xFD});                      // EI
  // DMA ROM do cartucho -> vídeo: banco $22 (deslocamento $88000 = $8000 no espelho), bloco 16x16,
  // origem (0,0), destino (8,10) na página 0, paleta identidade, sem sobrescrever o 0
  a.mov(0x3C, 0x22).mov(0x35, 0).mov(0x36, 0).mov(0x37, 15).mov(0x38, 15).mov(0x39, 8).mov(0x3A, 10);
  a.mov(0x3B, 0xE4).mov(0x3D, 0x00);
  a.mov(0x34, 0x82);  // DMC: começa, modo ROM->vídeo
  a.b({0xF1});        // HALT: a DMA acontece e a interrupção acorda
  // DMA da ROM do sistema (DMBR = 1): quadrado 8x8 de pixels 2 em (100,40), paleta que troca 2 -> 1
  a.mov(0x3C, 0x01).mov(0x35, 0).mov(0x36, 0).mov(0x37, 7).mov(0x38, 7).mov(0x39, 100).mov(0x3A, 40);
  a.mov(0x3B, 0xD4);  // 0->0, 1->1, 2->1, 3->3
  a.mov(0x34, 0x83);  // sobrescrevendo
  a.b({0xF1});
  // som: SG0 ligado, onda quadrada, volume máximo
  for (int i = 0; i < 16; i++) a.mov(0x60 + i, i < 8 ? 0x77 : 0x88);  // amostras com sinal: +7 / -8
  a.mov(0x40, 0x81).mov(0x42, 0x1F).mov(0x46, 0x00).mov(0x47, 0x40);
  // timer 0: pré-divisor 512, limite 10 (interrupção a cada 5120 ciclos)
  a.mov(0x51, 10).mov(0x50, 0x81);
  a.mov(0x54, 0x80);   // relógio de 1 s
  a.callA(0x2000);     // rotina na ROM do sistema
  a.b({0xC0, 0x42}).stAbs(0, 0xE000);  // marca "pronto" na RAM estendida
  // laço principal: espera o quadro e lê as teclas e a caneta
  a.label("main");
  a.b({0xF1});
  a.mov(0x15, 0xFF).mov(0x16, 0x7F);       // mux = $7FFF: teclas 1
  a.b({0xB0, 0x14}).stAbs(0, 0xE010);      // P0
  a.b({0xB0, 0x15}).stAbs(0, 0xE011);      // P1
  a.mov(0x15, 0xFF).mov(0x16, 0xFF);       // mux = $FFFF: teclas 2
  a.b({0xB0, 0x14}).stAbs(0, 0xE012);
  // varre as 13 colunas da tela de toque
  a.movw(0x0A, 0xFFFB);                    // rr10: r10 = P2, r11 = P1
  a.b({0xC4, 0x00});                       // r4 = coluna
  a.b({0xC6, 0xFF});                       // r6 = coluna achada (nenhuma); vai para $E013 no fim da varredura
  a.b({0xC5, 13});                         // r5 = contador
  a.label("col");
  a.movr(0x15, 0x0B).movr(0x16, 0x0A);
  a.cmp(0x14, 0xFF).br(CC_NE, "achou");
  a.b({0xB0, 0x15}).b({0x55, 0x03, 0x00}).cmp(0x00, 0x03).br(CC_EQ, "nada");  // linhas 8-9 ficam em P1
  a.label("achou");
  a.b({0x48, 0x04, 0x06});                 // MOV r6,r4
  a.b({0xB0, 0x14}).stAbs(0, 0xE014);
  a.b({0xB0, 0x15}).stAbs(0, 0xE015);      // P1 (linhas 8 e 9 nos bits 0-1)
  a.label("nada");
  a.b({0xFC});                             // SETC
  a.b({0x06, 0x0B}).b({0x06, 0x0A});       // RLC r11, RLC r10
  a.inc(0x04);
  a.dbnz(5, "col");
  a.stAbs(6, 0xE013);
  a.jmp(CC_T, "main");
  a.finish();
  std::memcpy(rom.data(), a.out.data(), a.out.size());
  // imagem para a DMA em $8000: pixels 3 numa faixa de 16x16 (64 bytes por linha)
  for (int y = 0; y < 16; y++)
    for (int x = 0; x < 4; x++) rom[0x8000 + size_t(y) * 64 + size_t(x)] = 0xFF;
  return rom;
}

uint32_t px(const GameCom& g, int x, int y) { return g.fb[y * SCREEN_W + x]; }

void machineTests() {
  const auto ir = buildInternal(), er = buildExternal(), cart = buildCart();
  check(isGamecomRom(cart, "teste.bin"), "cartucho reconhecido pelo TigerDMGC");
  check(!isGamecomRom(std::vector<uint8_t>(0x10000, 0), "x.bin"), "arquivo qualquer não é Game.com");
  check(gamecomFirmwareKind(ir, "gamecom_teste_int.bin") == "gamecom_int", "BIOS interna de teste reconhecida pelo nome");
  check(gamecomFirmwareKind(er, "gamecom_teste_ext.bin") == "gamecom_ext", "ROM do sistema de teste reconhecida pelo nome");
  check(gamecomFirmwareKind(er, "external.bin").empty(), "ROM de 256 KB qualquer não vira BIOS");
  {
    // cartucho de 256 KB com "game.com" no nome ou numa pasta "gamecom" continua cartucho
    std::vector<uint8_t> c256;
    while (c256.size() < EXT_ROM_SIZE) c256.insert(c256.end(), cart.begin(), cart.end());
    c256.resize(EXT_ROM_SIZE);
    check(gamecomFirmwareKind(c256, "jogo (game.com).tgc").empty(), "cartucho .tgc de 256 KB com 'game.com' no nome não vira BIOS");
    check(gamecomFirmwareKind(c256, "jogo (game.com).bin").empty(), "cartucho com TigerDMGC não vira BIOS");
    check(gamecomFirmwareKind(er, "/roms/gamecom/sistema.bin").empty(), "pasta 'gamecom' não conta como nome de BIOS");
    check(gamecomFirmwareKind(er, "/roms/gamecom/gamecom_ext.bin") == "gamecom_ext", "nome do arquivo numa pasta ainda vale");
    check(gamecomFirmwareKind(std::vector<uint8_t>(INT_ROM_SIZE, 0), "game_com.tgc").empty(), ".tgc de 4 KB não vira BIOS interna");
    setFirmware("gamecom_int", ir);
    setFirmware("gamecom_ext", er);
    auto c = createCore(c256, "/roms/gamecom/Jogo (Game.com).tgc", nullptr);
    check(c && c->system() == "gamecom", "cartucho de 256 KB numa pasta gamecom abre como jogo");
  }

  // sem BIOS: o núcleo explica o que falta
  setFirmware("gamecom_int", {});
  setFirmware("gamecom_ext", {});
  bool threw = false;
  try { createCore(cart, "teste.tgc", nullptr); } catch (const std::exception& e) {
    threw = std::string(e.what()).find("internal.bin") != std::string::npos;
  }
  check(threw, "sem as ROMs do console: erro pedindo internal.bin/external.bin");
  check(createCore(ir, "gamecom_teste_int.bin", nullptr) == nullptr, "a BIOS não abre como jogo");

  setFirmware("gamecom_int", ir);
  setFirmware("gamecom_ext", er);
  auto core = createCore(cart, "teste.tgc", nullptr);
  check(core && core->system() == "gamecom", "createCore escolhe o Game.com");
  if (!core) return;
  check(core->title() == "TESTE IND", "título do cabeçalho: " + core->title());
  check(core->width() == 200 && core->height() == 160, "tela 200x160");
  auto* gcc = dynamic_cast<GameComCore*>(core.get());
  GameCom& g = gcc->gc();
  core->setSampleRate(48000);
  for (int i = 0; i < 30; i++) core->runFrame();

  check(g.peekExt(0) == 0x42, "programa do cartucho chegou ao fim da preparação (RAM estendida $E000)");
  check(g.peekRam(0x93) == 0x5A, "chamada da rotina na ROM do sistema (MMU1 = banco 0)");
  check(g.dmaCount() == 2 && g.peekRam(0x90) == 2, "duas DMAs feitas e duas interrupções de DMA");
  const int fr = g.peekRam(0x91);
  check(fr >= 27 && fr <= 31, "interrupção LCDC uma vez por quadro (" + std::to_string(fr) + " em 30)");
  const uint64_t cyc = g.cycleCount();
  check(cyc >= uint64_t(FRAME_CYCLES) * 30 - 600 && cyc <= uint64_t(FRAME_CYCLES) * 30 + 600, "ciclos por quadro");
  const int t0 = g.peekRam(0x92);
  const int t0exp = int(uint64_t(FRAME_CYCLES) * 28 / 5120 % 256);
  check(t0 > 0, "timer 0 gera interrupções (" + std::to_string(t0) + ", ~" + std::to_string(t0exp) + "+)");
  // tela: colunas com o padrão $1B -> tons 4,3,2,0 de cima para baixo
  check(px(g, 0, 0) == 0xFF8FFFDFu && px(g, 0, 1) == 0xFF8FCF8Fu && px(g, 0, 2) == 0xFF4F8F6Fu && px(g, 0, 3) == 0xFF000000u,
        "padrão $1B: quatro tons na coluna 0");
  check(px(g, 199, 156) == 0xFF8FFFDFu && px(g, 199, 159) == 0xFF000000u, "última coluna");
  // DMA do cartucho: pixels 3 em x = 10..25 (linha do LCD), y = 8..23
  bool dmaOk = true;
  for (int x = 10; x < 26; x++)
    for (int y = 8; y < 24; y++) dmaOk = dmaOk && px(g, x, y) == 0xFF000000u;
  check(dmaOk, "DMA ROM do cartucho -> vídeo (bloco 16x16 preto)");
  check(px(g, 9, 9) == 0xFF8FCF8Fu && px(g, 26, 9) == 0xFF8FCF8Fu, "DMA não passa da borda");
  // DMA da ROM do sistema com paleta 2->1: tom 3 em x=40..47, y=100..107
  bool dma2 = true;
  for (int x = 40; x < 48; x++)
    for (int y = 100; y < 108; y++) dma2 = dma2 && px(g, x, y) == 0xFF8FCF8Fu;
  check(dma2, "DMA da ROM do sistema com troca de paleta e sobrescrita");

  // teclas
  core->setKeys(KEY_A | KEY_LEFT | KEY_B | KEY_SELECT);
  core->runFrame();
  core->runFrame();
  check(g.peekExt(0x10) == (0xFF & ~0x80 & ~0x04 & ~0x10), "P0 com A, esquerda e Menu: " + hx(g.peekExt(0x10)));
  check((g.peekExt(0x11) & 3) == 2, "P1 com B");
  core->setKeys(KEY_Y | KEY_X);
  core->runFrame();
  core->runFrame();
  check((g.peekExt(0x12) & 3) == 1, "D na segunda linha de teclas: " + hx(g.peekExt(0x12)));
  check((g.peekExt(0x11) & 3) == 1, "C (KEY_X)");
  core->setKeys(0);
  core->setPointer(40, 50, true);  // coluna 2, linha 3
  core->runFrame();
  core->runFrame();
  check(g.peekExt(0x13) == 2 && g.peekExt(0x14) == uint8_t(~0x08), "caneta em (40,50): coluna 2, bit 3 de P0");
  core->setPointer(0, 0, false);
  core->runFrame();
  core->runFrame();
  check(g.peekExt(0x13) == 0xFF, "caneta solta: nenhuma coluna");
  // 2.5: cantos da tela de toque (grade de 13 x 10 áreas; a última coluna tem 8 pixels, como no MAME)
  {
    struct P { int x, y, col; uint8_t p0, p1; };
    const P ps[] = {{0, 0, 0, 0xFE, 3}, {199, 159, 12, 0xFF, 1}, {191, 127, 11, 0x7F, 3}, {192, 128, 12, 0xFF, 2},
                    {15, 16, 0, 0xFD, 3}, {16, 15, 1, 0xFE, 3}};
    for (const P& q : ps) {
      core->setPointer(q.x, q.y, true);
      core->runFrame();
      core->runFrame();
      check(g.peekExt(0x13) == q.col && g.peekExt(0x14) == q.p0 && (g.peekExt(0x15) & 3) == q.p1,
            "caneta em (" + std::to_string(q.x) + "," + std::to_string(q.y) + "): coluna " + std::to_string(g.peekExt(0x13)) +
                ", P0 " + hx(g.peekExt(0x14)) + ", P1 " + hx(g.peekExt(0x15) & 3));
    }
    core->setPointer(500, -20, true);  // fora da tela: o núcleo prende na borda
    core->runFrame();
    core->runFrame();
    check(g.peekExt(0x13) == 12 && g.peekExt(0x14) == 0xFE, "ponto fora da tela vai para a borda mais próxima");
    core->setPointer(0, 0, false);
    core->runFrame();
    core->runFrame();
  }
  // 2.5: toque do app -> pixel do LCD (o mesmo cálculo do app no PC e no Android)
  {
    int px = -1, py = -1;
    // imagem 3x em (10, 20): 600 x 480
    check(pointerToLcd(10, 20, 10, 20, 600, 480, px, py) && px == 0 && py == 0, "toque no canto da imagem = pixel (0,0)");
    check(pointerToLcd(10 + 599.9f, 20 + 479.9f, 10, 20, 600, 480, px, py) && px == 199 && py == 159, "último pixel");
    check(!pointerToLcd(9.5f, 100, 10, 20, 600, 480, px, py), "meio pixel à esquerda da imagem fica fora (arredonda para baixo)");
    check(!pointerToLcd(610, 100, 10, 20, 600, 480, px, py), "borda direita fica fora");
    check(pointerToLcd(10 + 3 * 100 + 2.9f, 20 + 3 * 50 + 0.1f, 10, 20, 600, 480, px, py) && px == 100 && py == 50,
          "escala 3: cada pixel do LCD tem 3 pixels da tela");
    // escala quebrada (celular): 200x160 em 731 x 584,8
    check(pointerToLcd(100 + 731 * 0.5f, 7 + 584.8f * 0.25f, 100, 7, 731, 584.8f, px, py) && px == 100 && py == 40,
          "escala quebrada: meio da largura = coluna 100");
    check(!pointerToLcd(5, 5, 0, 0, 0, 0, px, py), "imagem sem tamanho: nenhum toque");
    check(!pointerToLcd(NAN, 5, 0, 0, 200, 160, px, py), "coordenada inválida: nenhum toque");
  }
  // caneta pelo direcional (L liga)
  core->setKeys(KEY_L);
  core->runFrame();
  core->setKeys(KEY_A);  // cursor no meio (100,80): coluna 6, linha 5
  core->runFrame();
  core->runFrame();
  check(g.peekExt(0x13) == 6 && g.peekExt(0x14) == uint8_t(~0x20), "caneta pelo direcional (cursor em 100,80)");
  check((g.peekExt(0x10) & 0x80) != 0, "com a caneta ligada, A não é o botão A");
  core->setKeys(KEY_L);
  core->runFrame();
  core->setKeys(0);
  core->runFrame();

  // som: onda quadrada no SG0 -> amostras saindo
  std::vector<float> l(4096), r(4096);
  const size_t n = std::min<size_t>(core->audioAvailable(), 4096);
  core->pullAudio(l.data(), r.data(), n);
  float mx = 0;
  for (size_t i = 0; i < n; i++) mx = std::max(mx, std::abs(l[i]));
  check(n > 700 && mx > 0.05f, "som do SG0 (" + std::to_string(n) + " amostras, pico " + std::to_string(mx) + ")");
  // relógio de 1 s (CK)
  for (int i = 0; i < 60; i++) core->runFrame();
  check(g.peekRam(0x94) >= 1, "relógio de 1 segundo (CLKT)");

  // save state determinístico
  const auto st = core->saveState();
  for (int i = 0; i < 20; i++) core->runFrame();
  const uint32_t h1 = crc32(reinterpret_cast<const uint8_t*>(core->framebuffer()), SCREEN_W * SCREEN_H * 4);
  const uint64_t c1 = g.cycleCount();
  const auto st1 = core->saveState();
  core->loadState(st);
  for (int i = 0; i < 20; i++) core->runFrame();
  check(g.cycleCount() == c1 && core->saveState() == st1, "estado salvo: mesma continuação");
  check(crc32(reinterpret_cast<const uint8_t*>(core->framebuffer()), SCREEN_W * SCREEN_H * 4) == h1, "estado salvo: mesma tela");
  bool bad = false;
  try { core->loadState(std::vector<uint8_t>(st.begin(), st.begin() + 100)); } catch (...) { bad = true; }
  check(bad && g.cycleCount() == c1, "estado truncado é recusado sem estragar o jogo");
  // estado danificado: contadores enormes são limitados ao carregar (sem quadro de segundos)
  {
    const auto keep = core->saveState();
    g.pokeTimers(0xFFFFFFF0u, 1);
    g.pokeSoundAcc(0xFFFFFFF0u);
    const auto st2 = core->saveState();
    core->loadState(st2);
    check(g.timerPre(0) < 2 && g.timerPre(1) < 2, "estado: prescaler do timer limitado ao período");
    check(g.soundAcc(0) < 0x20000 && g.soundAcc(1) < 0x20000 && g.soundAcc(2) < 0x20000, "estado: acumuladores do som limitados");
    const auto t0 = std::chrono::steady_clock::now();
    core->runFrame();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(ms < 500, "estado danificado: o quadro seguinte é rápido (" + std::to_string(int(ms)) + " ms)");
    core->loadState(keep);
  }
  // RAM estendida de 8 KB é o save (agenda e recordes)
  {
    check(core->hasSave(), "Game.com tem save (RAM estendida)");
    auto sv = core->exportSave();
    check(sv.size() == 0x2000 && sv[0] == 0x42, "save exporta os 8 KB da RAM estendida");
    check(!core->saveDirty(), "exportar limpa o aviso de save alterado");
    sv[0x100] = 0x77;
    sv[0] = 0;  // o cartucho grava $42 aqui de novo
    auto c2 = createCore(cart, "teste.tgc", &sv);
    check(c2 && dynamic_cast<GameComCore*>(c2.get())->gc().peekExt(0x100) == 0x77, "save volta ao abrir o jogo");
    for (int i = 0; i < 30; i++) c2->runFrame();
    check(c2->saveDirty(), "jogo gravou na RAM estendida: save alterado");
  }
  core->reset();
  check(g.peekRam(0x93) == 0, "reset limpa a RAM interna");
  for (int i = 0; i < 5; i++) core->runFrame();
  check(g.peekRam(0x93) == 0x5A, "reset volta a rodar a BIOS e o cartucho");
}

// conteúdo aleatório (BIOS e cartucho de lixo): não pode travar nem acessar memória fora do lugar
void fuzzTests() {
  uint32_t x = 12345;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
  for (int round = 0; round < 6; round++) {
    std::vector<uint8_t> ir(INT_ROM_SIZE), er(EXT_ROM_SIZE), cart(size_t(0x8000) << (round % 4));
    for (auto& v : ir) v = uint8_t(rnd());
    for (auto& v : er) v = uint8_t(rnd());
    for (auto& v : cart) v = uint8_t(rnd());
    GameCom g(cart, ir, er);
    g.setSampleRate(44100);
    for (int f = 0; f < 40; f++) {
      g.setKeys(rnd());
      g.setPointer(int(rnd() % 300) - 50, int(rnd() % 300) - 50, rnd() & 1);
      g.runFrame();
    }
    const auto st = g.saveState();
    g.runFrame();
    g.loadState(st);
    // estado com bytes trocados ao acaso: carrega com limites ou é recusado
    auto bad = st;
    for (int k = 0; k < 64; k++) bad[8 + rnd() % (bad.size() - 8)] = uint8_t(rnd());
    try { g.loadState(bad); } catch (...) {}
    for (int f = 0; f < 5; f++) g.runFrame();
  }
  check(true, "6 rodadas com BIOS, cartucho, teclas e estados aleatórios sem travar");
}

// ------------------------------------------------------------------ 2.5: tabela de ciclos completa
// Os 256 códigos, em todos os modos de endereçamento, com pilha de 16 e de 8 bits onde muda. Não há manual do
// SM85CPU disponível: os números são os do código do núcleo sm8500 do MAME (a única tabela pública); onde o
// comentário do MAME discorda do próprio código (modos @ww/ww(rr) de $30-$39), vale o código, e a dúvida fica
// registrada no relatório. A tabela trava o comportamento: qualquer mudança de ciclo aparece aqui.
struct CycRow { std::vector<int> code; int cyc; bool sp8 = false; uint8_t ps1 = 0; int reg = -1; uint8_t val = 0; };

void cycleTableTests() {
  std::vector<CycRow> rows;
  auto add = [&](std::vector<int> c, int cyc, bool sp8 = false, uint8_t ps1 = 0, int reg = -1, uint8_t val = 0) {
    rows.push_back({std::move(c), cyc, sp8, ps1, reg, val});
  };
  for (int op = 0; op <= 0x0D; op++) add({op, 0x80}, op == 1 ? 5 : op == 0x0D ? 7 : 4);
  add({0x0E, 0x80}, 12); add({0x0E, 0x80}, 10, true);
  add({0x0F, 0x80}, 9); add({0x0F, 0x80}, 8, true);
  for (int op = 0x10; op <= 0x17; op++) add({op, 0x0A}, 5);
  add({0x18, 0x80}, 8); add({0x19, 0x80}, 8);
  static const int C1A[8] = {7, 8, 7, 7, 7, 7, 7, 6};
  for (int k = 0; k < 8; k++) add({0x1A, (1 << 3) | k}, C1A[k]);
  static const int C1B[6] = {7, 7, 6, 6, 7, 9};
  for (int k = 0; k < 6; k++) add({0x1B, (1 << 3) | k}, C1B[k]);
  add({0x1B, (1 << 3) | 6}, 13); add({0x1B, (1 << 3) | 6}, 8, true);
  add({0x1B, (1 << 3) | 7}, 12); add({0x1B, (1 << 3) | 7}, 11, true);
  for (int op : {0x1C, 0x1D}) { add({op, 0x03, 0x20}, 12); add({op, (2 << 3) | 3, 0x10}, 8); }
  add({0x1E, 0x80}, 12); add({0x1E, 0x80}, 9, true);
  add({0x1F, 0x80}, 12); add({0x1F, 0x80}, 13, true);
  // modos: @r, (r)+, @w, w(r), -(r)
  auto modes8 = [&](int op, int a, int b, int cAbs, int cIdx, int d) {
    add({op, (1 << 3) | 2}, a); add({op, 0x40 | (1 << 3) | 2}, b); add({op, 0x80 | (1 << 3), 0x90}, cAbs);
    add({op, 0x80 | (1 << 3) | 2, 0x03}, cIdx); add({op, 0xC0 | (1 << 3) | 2}, d);
  };
  for (int op = 0x20; op <= 0x27; op++) modes8(op, 7, 8, 10, 8, 9);
  modes8(0x28, 6, 7, 10, 7, 8);
  modes8(0x29, 8, 8, 10, 9, 9);
  add({0x2A, 0x00, 0x20, 0x05}, 16); add({0x2B, 0x00, 0x20, 0x05}, 12);           // $FFdd: desvia / não
  add({0x2A, 0x08, 0x20, 0x05}, 14); add({0x2B, 0x08, 0x20, 0x05}, 10);           // dd(r)
  add({0x2C, 0x02}, 6); add({0x2D}, 4); add({0x2E, 0x00}, 4); add({0x2F, 0x80, 0x01}, 6);
  // modos de 16 bits: @rr, (rr)+, @ww, ww(rr), -(rr)
  auto modes16 = [&](int op, int a, int b, int cAbs, int cIdx, int d) {
    add({op, (1 << 3) | 2}, a); add({op, 0x40 | (1 << 3) | 2}, b); add({op, 0x80 | (1 << 3), 0x50, 0x00}, cAbs);
    add({op, 0x80 | (1 << 3) | 2, 0x00, 0x03}, cIdx); add({op, 0xC0 | (1 << 3) | 2}, d);
  };
  for (int op = 0x30; op <= 0x39; op++) modes16(op, 8, 13, 15, 11, 13);
  modes16(0x3A, 11, 16, 14, 18, 16);
  modes16(0x3B, 11, 16, 14, 18, 16);
  add({0x3C, (1 << 3) | 2}, 7); add({0x3D}, 4);
  add({0x3E, 0x00}, 7); add({0x3E, 0x40, 0x50, 0x00}, 15); add({0x3E, 0x48, 0x50, 0x00}, 19);
  add({0x3F, 0x00}, 14); add({0x3F, 0x40, 0x50, 0x00}, 22); add({0x3F, 0x48, 0x50, 0x00}, 26);
  add({0x3F, 0x00}, 11, true); add({0x3F, 0x40, 0x50, 0x00}, 19, true); add({0x3F, 0x48, 0x50, 0x00}, 23, true);
  for (int op = 0x40; op <= 0x48; op++) add({op, 0x81, 0x80}, 6);
  add({0x49, 0x50, 0x00}, 12); add({0x49, 0x50, 0x00}, 10, true);
  add({0x4A, 0x00, 0x04}, 8); add({0x4B, 0x80, 0x12, 0x34}, 9);
  add({0x4C, 0x81, 0x00}, 24); add({0x4D, 0x05, 0x00}, 24);
  add({0x4E, 0x04, 0x83}, 6); add({0x4E, 0x44, 0x83}, 6); add({0x4F, 0x04, 0x83}, 6); add({0x4F, 0xC4, 0x83}, 6);
  for (int op = 0x50; op <= 0x58; op++) add({op, 0x05, 0x80}, 6);
  add({0x59}, 2);
  add({0x5A, 0x02, 0x33}, 7); add({0x5A, 0x42, 0x33}, 7); add({0x5A, 0x82, 0xC6, 0x00}, 11); add({0x5A, 0xC2, 0x33}, 7);
  add({0x5B, 0x02, 0x33}, 6); add({0x5B, 0x42, 0x33}, 6); add({0x5B, 0x82, 0xC6, 0x00}, 10); add({0x5B, 0xC2, 0x33}, 6);
  add({0x5C, 0x02, 0x00}, 47); add({0x5D, 0x07, 0x00}, 44); add({0x5E, 0x80, 0x0F, 0x81}, 9); add({0x5F, 0x80, 0x0F, 0x50}, 8);
  static const int CW[8] = {9, 10, 10, 10, 10, 14, 14, 14}, CWI[8] = {9, 10, 10, 10, 10, 13, 13, 13};
  for (int k = 0; k < 8; k++) { add({0x60 + k, 0x02, 0x00}, CW[k]); add({0x68 + k, 0x00, 0x12, 0x34}, CWI[k]); }
  for (int r = 0; r < 8; r++) { add({0x70 + r, 0xFE}, 10, false, 0, r, 2); add({0x70 + r, 0xFE}, 6, false, 0, r, 1); }
  for (int op = 0x78; op <= 0x7F; op++) add({op, 0x12, 0x34}, 6);
  for (int b = 0; b < 8; b++) { add({0x80 + b, 0x90, 0x05}, 10); add({0x88 + b, 0x90, 0x05}, 6); }  // bit 0: BBC desvia
  for (int op = 0x90; op <= 0x9F; op++) add({op, 0x50, 0x00}, 6);
  for (int op = 0xA0; op <= 0xAF; op++) add({op, 0x80}, 4);
  for (int op = 0xB0; op <= 0xBF; op++) add({op, 0x80}, 4);
  for (int op = 0xC0; op <= 0xCF; op++) add({op, 0x00}, 4);
  for (int op = 0xD0; op <= 0xDF; op++) add({op, 0x05}, op == 0xD0 ? 4 : op == 0xD8 ? 8 : (op & 7) == 0 ? 0 : -1);
  for (int op = 0xE0; op <= 0xEF; op++) { add({op, 0x21}, 12); add({op, 0x21}, 9, true); }
  for (int op = 0xF0; op <= 0xF7; op++) add({op}, 2);
  add({0xF8}, 10); add({0xF8}, 8, true); add({0xF9}, 12); add({0xF9}, 10, true);
  for (int op = 0xFA; op <= 0xFF; op++) add({op}, 2);

  bool seen[256] = {};
  int bad = 0;
  for (const CycRow& row : rows) {
    int expect = row.cyc;
    if (expect <= 0) {
      // BR cc com flags zeradas: desvia nas condições verdadeiras com C=Z=S=V=0 (8), senão 4
      static const bool TAKEN0[16] = {false, false, false, false, false, false, false, false,
                                      true, true, true, true, true, true, true, true};
      expect = TAKEN0[row.code[0] & 15] ? 8 : 4;
    }
    CpuRig t;
    if (row.sp8) { t.cpu.sys = 0; t.cpu.spl = 0xF0; }
    t.cpu.ps1 = row.ps1;
    if (row.reg >= 0) t.set(row.reg, row.val);
    uint16_t a = t.cpu.pc;
    for (int x : row.code) t.bus.mem[a++] = uint8_t(x);
    const int c = t.cpu.step();
    seen[row.code[0] & 0xFF] = true;
    if (c != expect) {
      bad++;
      std::string bytes;
      for (int x : row.code) bytes += hx(unsigned(x)) + " ";
      check(false, "ciclos de " + bytes + (row.sp8 ? "(pilha de 8 bits) " : "") + "= " + std::to_string(c) +
                       " (tabela " + std::to_string(expect) + ")");
    }
    if (t.bus.ticks != c) { bad++; check(false, "ciclos passados ao barramento iguais aos devolvidos"); }
  }
  int covered = 0;
  for (bool b : seen) covered += b;
  check(covered == 256, "tabela de ciclos cobre os 256 códigos (" + std::to_string(covered) + ")");
  check(bad == 0 && rows.size() > 400, "tabela de ciclos: " + std::to_string(rows.size()) + " formas conferidas");
  // HALT: 2 ciclos na instrução e 4 por passo parado (como no MAME)
  {
    CpuRig t;
    const int c1 = t.run({0xF1});
    const int c2 = t.cpu.step();
    check(c1 == 2 && c2 == 4 && t.cpu.halted && t.bus.ticks == 6, "HALT: 2 ciclos e depois 4 por passo parado");
  }
}

// ------------------------------------------------------------------ 2.5: CKC e STOP
void clockTests() {
  CpuRig t;
  check(t.r(0x1A) == 0, "CKC começa em 0");
  t.run({0xC8 + 0, 0x00});  // (só para andar) MOV IE0,#0
  t.run({0x58, 0x20, 0x1A});  // MOV CKC,#$20
  check(t.r(0x1A) == 0x20 && t.cpu.ckcPending && t.cpu.ckcApplied == 0, "CKC escrito fica pendente até o STOP");
  check(t.bus.mem[0x1A] == 0, "CKC é registrador da CPU (não vai ao barramento)");
  const int c = t.run({0xF0});
  check(c == 2 && !t.cpu.ckcPending && t.cpu.ckcApplied == 0x20 && t.cpu.stops == 1, "STOP aplica o CKC pendente");
  const uint16_t pc = t.cpu.pc;
  t.run({0xF0});
  check(t.cpu.pc == pc + 1 && !t.cpu.halted && t.cpu.stops == 2, "STOP sem troca pendente segue adiante (como no MAME)");
  t.run({0x58, 0x20, 0x1A});
  check(!t.cpu.ckcPending, "escrever o mesmo CKC não pede troca");
  // estado: CKC entra no save state da CPU
  StateIO w;
  t.cpu.sync(w);
  CpuRig u;
  StateIO rd(w.data());
  u.cpu.sync(rd);
  check(u.cpu.ckc == 0x20 && u.cpu.ckcApplied == 0x20 && u.cpu.stops == 2, "CKC e STOPs no save state da CPU");
}

// ------------------------------------------------------------------ 2.5: máquina "quieta" (CPU parada em HALT)
std::vector<uint8_t> quietInternal() {
  std::vector<uint8_t> ir(INT_ROM_SIZE, 0xFF);
  ir[0x20] = 0xF1;            // $1020: HALT
  ir[0x21] = 0xD8; ir[0x22] = 0xFD;  // BR T,$1020
  return ir;
}

void soundTests(const std::vector<uint8_t>& er, const std::vector<uint8_t>& cart) {
  GameCom g(cart, quietInternal(), er);
  g.setSampleRate(48000);
  g.runFrame();
  // período de cada passo = (SGnT + 1) contagens de 2 ciclos
  for (int i = 0; i < 16; i++) g.write(uint16_t(0x60 + i), uint8_t(i < 8 ? 0x77 : 0x88));
  g.write(0x42, 0x1F);
  g.write(0x46, 0x00);
  g.write(0x47, 99);
  g.write(0x40, 0x81);
  g.tick(200 * 5, false);
  check(g.soundStepPos(0) == 5, "SG0 com SG0T=99: um passo a cada 200 ciclos (" + std::to_string(g.soundStepPos(0)) + " em 1000)");
  g.tick(199, false);
  check(g.soundStepPos(0) == 5, "SG0: ainda não passou o sexto passo em 1199 ciclos");
  g.tick(1, false);
  check(g.soundStepPos(0) == 6, "SG0: sexto passo em 1200 ciclos");
  // SG0T = 0 também toca (período de 2 ciclos)
  g.write(0x40, 0x00);
  g.write(0x47, 0);
  g.write(0x40, 0x81);
  const int p0 = g.soundStepPos(0);
  g.tick(20, false);
  check((g.soundStepPos(0) - p0 + 32) % 32 == 10, "SG0T = 0: um passo a cada 2 ciclos");
  // registradores com a largura certa
  g.write(0x46, 0xF3);
  check(g.read(0x46) == 0x03, "SG0TH guarda 4 bits (período de 12 bits)");
  g.write(0x42, 0xFF);
  check(g.read(0x42) == 0x1F, "SG0L guarda 5 bits");
  g.write(0x4E, 0x12);
  check(g.read(0x4E) == 0, "SGDA é só escrita");
  // RAM da onda presa com o canal tocando
  check(g.read(0x60) == 0, "onda do SG0 lê 0 com o canal tocando");
  g.write(0x60, 0x11);
  g.write(0x40, 0x00);
  check(g.read(0x60) == 0x77, "escrita na onda com o canal tocando não pega");
  g.write(0x60, 0x11);
  check(g.read(0x60) == 0x11, "com o canal parado a onda aceita escrita");
  g.write(0x60, 0x77);
  // amostras com sinal: 7 -> +108, 8 -> -124 (volume 31, metade)
  g.write(0x46, 0x00); g.write(0x47, 0xFF);  // passo lento (512 ciclos)
  g.write(0x40, 0x81);
  int mn = 999, mx = -999;
  for (int i = 0; i < 400; i++) { g.tick(100, false); mn = std::min(mn, g.mixOut()); mx = std::max(mx, g.mixOut()); }
  check(mx == 108 && mn == -124, "onda com sinal: +7 e -8 com volume 31 dão +108/-124 (" + std::to_string(mx) + "/" + std::to_string(mn) + ")");
  // dois canais no mínimo: corte em -128
  for (int i = 0; i < 16; i++) g.write(uint16_t(0x70 + i), 0x88);
  g.write(0x40, 0x00);
  for (int i = 0; i < 16; i++) { g.write(uint16_t(0x60 + i), 0x88); g.write(uint16_t(0x70 + i), 0x88); }
  g.write(0x44, 0x1F); g.write(0x48, 0x00); g.write(0x49, 0x10);
  g.write(0x40, 0x83);
  g.tick(200, false);
  check(g.mixOut() == -128, "mistura de 8 bits com corte (" + std::to_string(g.mixOut()) + ")");
  // DAC direto: toca sozinho, sem os geradores
  g.write(0x4E, 0xC0);
  g.write(0x40, 0x8B);
  g.tick(200, false);
  check(g.mixOut() == 0x40, "SGC bit 3: só o DAC (SGDA - $80) (" + std::to_string(g.mixOut()) + ")");
  g.write(0x40, 0x08);
  g.tick(200, false);
  check(g.mixOut() == 0, "sem o bit 7 do SGC nada toca");
  // ruído: só +108 ou -124 com volume 31
  g.write(0x4A, 0x1F); g.write(0x4C, 0x00); g.write(0x4D, 0x03);
  g.write(0x40, 0x84);
  g.tick(400, false);
  bool hi = false, lo = false, other = false;
  for (int i = 0; i < 300; i++) {
    g.tick(37, false);
    const int m = g.mixOut();
    if (m == 108) hi = true; else if (m == -124) lo = true; else other = true;
  }
  check(hi && lo && !other, "SG2: ruído de dois níveis (+7/-8 vezes o volume)");
  // estado com som: mesma continuação
  const auto st = g.saveState();
  g.tick(5000, false);
  const int m1 = g.mixOut();
  const auto st1 = g.saveState();
  g.loadState(st);
  g.tick(5000, false);
  check(g.mixOut() == m1 && g.saveState() == st1, "estado salvo do som: mesma continuação");
}

void slotTests(const std::vector<uint8_t>& er, const std::vector<uint8_t>& cart) {
  GameCom g(cart, quietInternal(), er);
  g.runFrame();
  std::vector<uint8_t> c2(0x8000);
  for (size_t i = 0; i < c2.size(); i++) c2[i] = uint8_t(0xC0 ^ i);
  g.write(0x26, 0x20);  // $4000 = banco $20 do cartucho
  g.write(0x17, 0x40);
  check(g.read(0x4005) == 'T' && g.read(0x4000) == cart[0], "P3 = $40: encaixe 1 na janela");
  g.write(0x17, 0x80);
  check(g.read(0x4000) == 0xFF && g.read(0x4005) == 0xFF, "P3 = $80 sem segundo cartucho: janela lê $FF");
  check(!g.hasCart2(), "sem segundo cartucho");
  g.setCart2(c2);
  check(g.hasCart2() && g.read(0x4000) == c2[0] && g.read(0x4123) == c2[0x123], "P3 = $80: segundo cartucho na janela");
  g.write(0x17, 0xC0);
  check(g.read(0x4000) == 0xFF, "P3 = $C0: nenhum encaixe (lê $FF)");
  g.write(0x17, 0x00);
  check(g.read(0x4000) == 0xFF, "P3 = $00: nenhum encaixe");
  g.write(0x25, 0x01);  // $2000 = banco 1 da ROM do sistema
  check(g.read(0x2000) == er[0x2000], "banco < $20 é da ROM do sistema com qualquer P3");
  g.write(0x27, 0x21);  // MMU3 escrito sem encaixe ligado: vale quando ligar
  g.write(0x17, 0x40);
  check(g.read(0x6000) == cart[0x2000 % cart.size()], "MMU escrito sem encaixe ligado vale ao ligar (banco fica no MMU)");
  g.write(0x17, 0x80);
  check(g.read(0x6000) == c2[0x2000], "a mesma janela troca de cartucho só pelo P3");
  // DMA da ROM: encaixe 2, e $FF (pixel 3) sem encaixe
  auto dmaRom = [&](uint8_t p3) {
    g.write(0x17, p3);
    g.write(0x30, 0x80); g.write(0x31, 0x07);
    g.write(0x3C, 0x20); g.write(0x35, 0); g.write(0x36, 0); g.write(0x37, 3); g.write(0x38, 0);
    g.write(0x39, 0); g.write(0x3A, 0); g.write(0x3B, 0xE4); g.write(0x3D, 0);
    g.write(0x34, 0x83);
    for (int i = 0; i < 20 && (g.peekRam(0x34) & 0x80); i++) g.tick(445, true);
    return g.peekVram(0);
  };
  check(dmaRom(0x80) == c2[0], "DMA da ROM lê o segundo cartucho com P3 = $80");
  check(dmaRom(0xC0) == 0xFF, "DMA da ROM sem encaixe ligado lê $FF");
  check(dmaRom(0x40) == cart[0], "DMA da ROM volta ao encaixe 1");
  // o segundo cartucho não entra no estado; o estado guarda o P3
  g.write(0x17, 0x80);
  const auto st = g.saveState();
  g.write(0x17, 0x40);
  g.loadState(st);
  check(g.read(0x4000) == c2[0], "estado guarda a escolha do encaixe (P3)");
  g.setCart2({});
  check(!g.hasCart2() && g.read(0x4000) == 0xFF, "tirar o segundo cartucho");
  // pelo createCore: arquivo "gamecom_cart2"
  setFirmware("gamecom_cart2", c2);
  setFirmware("gamecom_int", quietInternal());
  setFirmware("gamecom_ext", er);
  {
    auto core = createCore(cart, "teste.tgc", nullptr);
    auto* gc = core ? dynamic_cast<GameComCore*>(core.get()) : nullptr;
    check(gc && gc->gc().hasCart2(), "createCore põe o arquivo gamecom_cart2 no segundo encaixe");
  }
  setFirmware("gamecom_cart2", {});
  {
    auto core = createCore(cart, "teste.tgc", nullptr);
    auto* gc = core ? dynamic_cast<GameComCore*>(core.get()) : nullptr;
    check(gc && !gc->gc().hasCart2(), "sem o arquivo, o segundo encaixe fica vazio");
  }
  // estado salvo com o segundo cartucho: sem ele (ou com outro), é recusado
  {
    g.setCart2(c2);
    const auto withC2 = g.saveState();
    g.setCart2({});
    bool refused = false;
    try { g.loadState(withC2); } catch (...) { refused = true; }
    check(refused && g.read(0x4000) == 0xFF, "estado com segundo cartucho recusado sem ele");
    auto other = c2;
    other[0] ^= 1;
    g.setCart2(other);
    refused = false;
    try { g.loadState(withC2); } catch (...) { refused = true; }
    check(refused, "estado com segundo cartucho recusado com outro cartucho");
    g.setCart2(c2);
    bool ok = true;
    try { g.loadState(withC2); } catch (...) { ok = false; }
    check(ok, "estado com segundo cartucho carrega com o mesmo cartucho");
    g.setCart2({});
  }
  // DMBR adulterado no estado ($FF): limitado a 7 bits; a DMA da ROM não sai do cartucho
  for (uint8_t p3 : {uint8_t(0x40), uint8_t(0x80)}) {
    if (p3 == 0x80) g.setCart2(c2);
    g.write(0x17, p3);
    g.write(0x30, 0x80); g.write(0x31, 0x07);
    g.write(0x3C, 0x7F); g.write(0x35, 0); g.write(0x36, 0); g.write(0x37, 159); g.write(0x38, 199);
    g.write(0x39, 0); g.write(0x3A, 0); g.write(0x3B, 0xE4); g.write(0x3D, 0);
    g.write(0x34, 0x83);  // modo 2 (ROM -> vídeo), sobrescrevendo
    auto st = g.saveState();
    // acha a RAM no estado pelos registradores da DMA recém-escritos
    size_t at = 0;
    for (size_t i = 0; i + 0x400 <= st.size(); i++)
      if (st[i + 0x3C] == 0x7F && st[i + 0x34] == 0x83 && st[i + 0x17] == p3 && st[i + 0x37] == 159 && st[i + 0x38] == 199) { at = i; break; }
    st[at + 0x3C] = 0xFF;
    bool ok = true;
    try { g.loadState(st); } catch (...) { ok = false; }
    check(at && ok && g.peekRam(0x3C) == 0x7F, "DMBR = $FF no estado vira $7F na carga");
    for (int i = 0; i < 20 && (g.peekRam(0x34) & 0x80); i++) g.tick(445, true);
    check(!(g.peekRam(0x34) & 0x80), "DMA da ROM com DMBR adulterado termina sem sair do cartucho");
    g.setCart2({});
  }
  bool threw = false;
  try { g.setCart2(std::vector<uint8_t>(CART_SPACE + 1)); } catch (...) { threw = true; }
  check(threw, "segundo cartucho grande demais é recusado");
}

// timers 0 e 1: pré-divisor da tabela do MAME (metade do valor, em ciclos da CPU) e contador até TMnD
void timerTests(const std::vector<uint8_t>& er, const std::vector<uint8_t>& cart) {
  static const int PRE[8] = {1, 512, 1024, 2048, 4096, 8192, 16384, 32768};
  for (int t = 0; t < 2; t++) {
    const uint16_t ctl = uint16_t(0x50 + 2 * t), dat = uint16_t(ctl + 1);
    auto flag = [&](GameCom& g) { return t ? (g.cpu.ir1 & 0x40) != 0 : (g.cpu.ir0 & 0x40) != 0; };
    for (int sel : {0, 1, 3}) {
      GameCom g(cart, quietInternal(), er);
      g.write(dat, 3);
      g.write(ctl, uint8_t(0x80 | sel));
      g.tick(3 * PRE[sel] - 1, false);
      const bool early = flag(g);
      const uint8_t cnt = g.read(dat);
      g.tick(1, false);
      check(!early && cnt == 2 && flag(g) && g.read(dat) == 0,
            "timer " + std::to_string(t) + " seleção " + std::to_string(sel) + ": interrupção depois de 3 x " +
                std::to_string(PRE[sel]) + " ciclos e o contador volta a 0");
    }
    GameCom g(cart, quietInternal(), er);
    g.write(dat, 5);
    g.write(ctl, 0x01);  // bit 7 desligado: parado
    g.tick(5000, false);
    check(!flag(g) && g.read(dat) == 0, "timer " + std::to_string(t) + " parado não conta");
    g.write(ctl, 0x81);
    g.tick(700, false);
    g.write(ctl, 0x81);  // reescrever o controle zera o pré-divisor e o contador
    check(g.read(dat) == 0, "timer " + std::to_string(t) + ": escrever o controle zera o contador");
    g.tick(511, false);
    check(g.read(dat) == 0, "timer " + std::to_string(t) + ": pré-divisor recomeça do zero");
    g.tick(1, false);
    check(g.read(dat) == 1, "timer " + std::to_string(t) + ": conta depois de 512 ciclos");
  }
  // os dois timers são independentes e o HALT não para a contagem
  {
    GameCom g(cart, quietInternal(), er);
    g.write(0x51, 2); g.write(0x50, 0x80);  // timer 0: 2 ciclos
    g.write(0x53, 4); g.write(0x52, 0x81);  // timer 1: 2048 ciclos
    g.tick(2047, true);
    check((g.cpu.ir0 & 0x40) && !(g.cpu.ir1 & 0x40), "timers independentes (timer 1 ainda não chegou)");
    g.tick(1, true);
    check(g.cpu.ir1 & 0x40, "timer 1 conta com a CPU em HALT");
  }
}

// estado da 2.4 ('GCM1') montado a partir de um 'GCM2': ainda carrega igual
void oldStateTests(const std::vector<uint8_t>& ir, const std::vector<uint8_t>& er, const std::vector<uint8_t>& cart) {
  GameCom g(cart, ir, er);
  for (int i = 0; i < 10; i++) g.runFrame();
  const auto st = g.saveState();
  const size_t cpuExtra = 4 + 4 + 7;          // magia + CRC + PC/PS0/PS1/SYS/SPH/SPL
  const size_t mark = 4;                      // marca do segundo cartucho (só no GCM2)
  const size_t afterMem = cpuExtra + 7 + 4 + 1 + 2 + 0x108 + 0x400 + 0x4000 + 0x2000;  // + CKC(7) + IE/IR + HALT + pendentes + regs
  std::vector<uint8_t> v1(st.begin(), st.begin() + 8);
  v1.insert(v1.end(), st.begin() + long(8 + mark), st.begin() + long(cpuExtra + mark));
  v1.insert(v1.end(), st.begin() + long(cpuExtra + mark + 7), st.begin() + long(afterMem + mark));
  v1.insert(v1.end(), 21, 0);                                  // janelas e "cartucho ligado" da 2.4
  v1.insert(v1.end(), st.begin() + long(afterMem + mark), st.end() - 16 - 4);  // até o DAC (sem a mistura)
  v1.insert(v1.end(), st.end() - 16, st.end());                // sampleAcc e hpCap
  v1[3] = '1';
  GameCom h(cart, ir, er);
  // sessão anterior com CKC, STOPs e mistura diferentes de zero: o GCM1 não pode herdar nada disso
  auto dirty = st;
  dirty[cpuExtra + mark] = 0x5A; dirty[cpuExtra + mark + 1] = 0x5A; dirty[cpuExtra + mark + 2] = 1;
  dirty[cpuExtra + mark + 3] = 9;
  for (int k = 0; k < 4; k++) dirty[st.size() - 20 + size_t(k)] = k ? 0 : 0x50;  // mistura (int de 4 bytes)
  h.loadState(dirty);
  check(h.cpu.ckc == 0x5A && h.cpu.stops == 9 && h.mixOut() == 0x50, "estado com CKC, STOPs e mistura carregado");
  bool ok = true;
  try { h.loadState(v1); } catch (...) { ok = false; }
  check(ok && h.cycleCount() == g.cycleCount() && h.peekRam(0x91) == g.peekRam(0x91), "estado da 2.4 (GCM1) ainda carrega");
  check(h.cpu.ckc == 0 && h.cpu.ckcApplied == 0 && !h.cpu.ckcPending && h.cpu.stops == 0 && h.mixOut() == 0,
        "GCM1 zera CKC, STOPs e mistura (não herda da sessão anterior)");
  GameCom h2(cart, ir, er);
  h2.loadState(v1);
  check(h.saveState() == h2.saveState(), "GCM1: mesmo estado partindo de máquinas diferentes");
  for (int i = 0; i < 5; i++) { g.runFrame(); h.runFrame(); }
  check(std::memcmp(g.fb, h.fb, sizeof g.fb) == 0 && g.cycleCount() == h.cycleCount(), "estado da 2.4: mesma continuação");
  auto bad = st;
  bad[3] = '9';
  bool refused = false;
  try { h.loadState(bad); } catch (...) { refused = true; }
  check(refused, "versão desconhecida do estado é recusada");
}

bool writeFile(const std::string& p, const std::vector<uint8_t>& d) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
  return bool(f);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "gerar") {
    const std::string d = argv[2];
    const bool ok = writeFile(d + "/gamecom_teste_int.bin", buildInternal()) &&
                    writeFile(d + "/gamecom_teste_ext.bin", buildExternal()) && writeFile(d + "/teste.tgc", buildCart());
    std::printf(ok ? "gerados em %s\n" : "falha ao gravar em %s\n", d.c_str());
    return ok ? 0 : 1;
  }
  cpuTests();
  cycleTableTests();
  clockTests();
  machineTests();
  {
    const auto ir = buildInternal(), er = buildExternal(), cart = buildCart();
    soundTests(er, cart);
    slotTests(er, cart);
    timerTests(er, cart);
    oldStateTests(ir, er, cart);
  }
  fuzzTests();
  std::printf("RESULTADO gamecom: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
