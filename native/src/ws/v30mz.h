// NEC V30MZ (CPU do WonderSwan): compatível com o 80186 em modo real, mas sem as instruções extras do V20/V30
// e com temporização própria (fila de pré-busca de 16 bytes, a maioria das instruções em 1 ciclo).
//
// Modelo de tempo: cada ciclo de espera interno também é um ciclo de pré-busca (a fila puxa 1 ou 2 bytes da
// memória do código), e cada acesso à memória/porta custa 1 ou 2 ciclos conforme a largura e a espera da região.
// O desenho segue o comportamento documentado do ares (ISC) e dos testes WSCPUTest/WSTimingTest de Fredrik
// Ahlström (bandeiras "indefinidas" do V30MZ, divisões, BCD, opcodes não documentados). Código escrito do zero.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::ws {

class V30Bus {
public:
  virtual ~V30Bus() = default;
  virtual uint8_t memRead(uint32_t a) = 0;  // endereço de 20 bits
  virtual void memWrite(uint32_t a, uint8_t v) = 0;
  virtual uint8_t ioRead(uint16_t port) = 0;
  virtual void ioWrite(uint16_t port, uint8_t v) = 0;
  // tempo de acesso: bits 0-3 = ciclos por acesso (1 ou 2); bit 4 = região com barramento de 16 bits
  virtual int memTiming(uint32_t a) = 0;
  virtual int ioTiming(uint16_t port) = 0;
};

class V30MZ {
public:
  enum : uint16_t {
    CF = 0x0001, PF = 0x0004, AF = 0x0010, ZF = 0x0040, SF = 0x0080,
    TF = 0x0100, IF = 0x0200, DF = 0x0400, OF = 0x0800, MD = 0x8000,
  };
  enum { AX, CX, DX, BX, SP, BP, SI, DI };
  enum { ES, CS, SS, DS };

  explicit V30MZ(V30Bus& bus) : bus_(bus) { power(); }

  void power();  // estado de reset: CS=FFFF, IP=0, fila vazia
  void step();   // uma instrução (ou um prefixo, ou um ciclo parado em HLT)
  // pedido de interrupção mascarável (vetor já com a base); tira do HLT mesmo se IF=0. true = aceitou
  bool irq(uint8_t vector);
  void flush();  // esvazia a fila de pré-busca (depois de mudar CS:IP por fora)
  // lê/grava as bandeiras como a instrução PUSHF/POPF enxerga
  uint16_t psw() const { return static_cast<uint16_t>((flags & 0x8FD5) | 0x7002); }
  void sync(StateIO& io);

  // registradores (ordem da codificação x86: AW CW DW BW SP BP IX IY; DS1 PS SS DS0)
  uint16_t r[8] = {};
  uint16_t sreg[4] = {};
  uint16_t ip = 0;
  uint16_t flags = 0;

  bool aswan = false;   // WonderSwan monocromático: MUL sem sinal zera Z (no Color, liga)
  int64_t cycles = 0;   // ciclos de CPU (3,072 MHz) desde o início
  bool halted = false;
  bool prefixActive = false;  // a última "instrução" foi um prefixo ou uma volta de REP (sem interrupção no meio)
  bool pollOk = true;         // falso logo depois de prefixo ou de carregar SS (adia interrupções)

  // só para os testes: chamado em toda interrupção de software (vetor, AL)
  void (*swiHook)(void* ctx, uint8_t vector, uint8_t al) = nullptr;
  void* swiCtx = nullptr;

  // fila de pré-busca (exposta para os testes)
  uint16_t pfp = 0;  // ponteiro de pré-busca (offset em CS)
  int pfCount = 0;

private:
  struct ModRM { int mod = 0, reg = 0, mem = 0; uint16_t seg = 0, addr = 0; bool useAddr = false; };

  V30Bus& bus_;
  uint8_t pf_[16] = {};
  int pfHead_ = 0;
  int pfWait_ = 0;
  uint8_t opcode_ = 0;
  int prefixCount_ = 0;
  uint8_t prefixRepeat_ = 0, prefixSeg_ = 0;
  bool ieSnap_ = false, brkSnap_ = false;
  bool mulCV_ = false;  // "carry/overflow" interno da última multiplicação (vaza nas bandeiras do DIV 16/8)
  ModRM m_;

  // ciclos e fila
  void wait(int n) { while (n-- > 0) prefetch(); }
  void prefetch();
  uint8_t fetch8();
  uint16_t fetch16();
  void loopQueue();
  void prefixFlush() { prefixCount_ = 0; prefixRepeat_ = 0; prefixSeg_ = 0; }

  // memória
  static uint32_t lin(uint16_t seg, uint16_t off) { return ((static_cast<uint32_t>(seg) << 4) + off) & 0xFFFFF; }
  uint16_t rd8(uint16_t seg, uint16_t off);
  uint16_t rd16(uint16_t seg, uint16_t off);
  void wr8(uint16_t seg, uint16_t off, uint16_t v);
  void wr16(uint16_t seg, uint16_t off, uint16_t v);
  template <bool W> uint16_t rd(uint16_t seg, uint16_t off) { return W ? rd16(seg, off) : rd8(seg, off); }
  template <bool W> void wr(uint16_t seg, uint16_t off, uint16_t v) { if (W) wr16(seg, off, v); else wr8(seg, off, v); }
  uint16_t in8(uint16_t port);
  uint16_t in16(uint16_t port);
  void out8(uint16_t port, uint16_t v);
  void out16(uint16_t port, uint16_t v);
  template <bool W> uint16_t in(uint16_t p) { return W ? in16(p) : in8(p); }
  template <bool W> void out(uint16_t p, uint16_t v) { if (W) out16(p, v); else out8(p, v); }
  void push(uint16_t v);
  uint16_t pop();
  uint16_t seg(int def) const;

  // registradores de 8 bits: AL CL DL BL AH CH DH BH
  uint8_t reg8(int i) const { return i < 4 ? static_cast<uint8_t>(r[i]) : static_cast<uint8_t>(r[i - 4] >> 8); }
  void setReg8(int i, uint16_t v) {
    if (i < 4) r[i] = static_cast<uint16_t>((r[i] & 0xFF00) | (v & 0xFF));
    else r[i - 4] = static_cast<uint16_t>((r[i - 4] & 0x00FF) | ((v & 0xFF) << 8));
  }
  template <bool W> uint16_t acc() const { return W ? r[AX] : static_cast<uint16_t>(r[AX] & 0xFF); }
  template <bool W> void setAcc(uint16_t v) { if (W) r[AX] = v; else setReg8(0, v); }

  // ModRM
  void modrm(bool forceAddress = false);
  template <bool W> uint16_t getReg() const { return W ? r[m_.reg] : reg8(m_.reg); }
  template <bool W> void setReg(uint16_t v) { if (W) r[m_.reg] = v; else setReg8(m_.reg, v); }
  template <bool W> uint16_t getMem(uint16_t offset = 0);
  template <bool W> void setMem(uint16_t v);

  // bandeiras e ALU
  void setFlag(uint16_t f, bool on) { flags = on ? static_cast<uint16_t>(flags | f) : static_cast<uint16_t>(flags & ~f); }
  bool flag(uint16_t f) const { return (flags & f) != 0; }
  static bool parity(uint8_t v);
  template <bool W> void setSZP(uint16_t res);
  template <bool W> uint16_t ADD(uint16_t x, uint16_t y, int c = 0);
  template <bool W> uint16_t SUB(uint16_t x, uint16_t y, int c = 0);
  template <bool W> uint16_t LOGIC(uint16_t res);
  template <bool W> uint16_t INC(uint16_t x);
  template <bool W> uint16_t DEC(uint16_t x);
  template <bool W> uint16_t NEG(uint16_t x);
  template <bool W> uint16_t ROL(uint16_t x, int n);
  template <bool W> uint16_t ROR(uint16_t x, int n);
  template <bool W> uint16_t RCL(uint16_t x, int n);
  template <bool W> uint16_t RCR(uint16_t x, int n);
  template <bool W> uint16_t SHL(uint16_t x, int n);
  template <bool W> uint16_t SHR(uint16_t x, int n);
  template <bool W> uint16_t SAR(uint16_t x, int n);
  template <bool W> uint32_t MULU(uint16_t x, uint16_t y);
  template <bool W> uint32_t MULI(uint16_t x, uint16_t y);

  // interrupções: origem 0 = da própria CPU (INT, divisão...), 1 = NMI, 2 = pedido externo
  void interrupt(uint8_t vector, int source);

  // grupos de instruções
  void execute(uint8_t op);
  template <bool W> void aluMemReg(int kind);
  template <bool W> void aluRegMem(int kind);
  template <bool W> void aluAccImm(int kind);
  template <bool W> uint16_t alu(int kind, uint16_t x, uint16_t y);
  template <bool W> void group1(bool signExtend);
  template <bool W> void group2(int clocks, int count);  // count < 0: lê o imediato
  template <bool W> void group3();
  template <bool W> void group4();
  template <bool W> void divu();
  template <bool W> void divs();
  void daa(bool sub);
  void aaa(bool sub);
  void aam();
  void aad();
  void jumpIf(bool cond);
  template <bool W> void movs();
  template <bool W> void cmps();
  template <bool W> void stos();
  template <bool W> void lods();
  template <bool W> void scas();
  template <bool W> void ins();
  template <bool W> void outs();
  bool repeatContinue(bool compare);
  void enter();
};

}  // namespace indigo::ws
