// NEC VR4300 (CPU do Nintendo 64): MIPS III de 64 bits, interpretador.
// Inteiros (todas as instruções do MIPS III), COP0 (TLB de 32 entradas, exceções, interrupções, Count/Compare)
// e COP1 (FPU com os 4 modos de arredondamento, flags, causas e exceções; FR=0 e FR=1).
// Sem caches (a instrução CACHE não faz nada). Tempo: 1 tique por instrução, 1 tique = 1,5 ciclo do VR4300
// (62,5 MHz, o mesmo relógio do RSP; a média de ciclos por instrução do VR4300 de verdade fica perto disso).
// Count anda 0,75 por tique (metade do relógio de 93,75 MHz).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::n64 {

class N64;

class Vr4300 {
public:
  explicit Vr4300(N64& s) : sys(s) {}
  void reset();
  // roda até o relógio chegar a `ate` (tiques)
  void run(int64_t ate);
  void sync(StateIO& io);

  // linhas de interrupção externas (bits IP2..IP7 do Cause); a do timer (IP7) é interna
  void setIp(int bit, bool on);
  void updateIrq();

  // pula direto para um endereço (partida HLE)
  void jump(uint64_t pc) { pc_ = pc; npc_ = pc + 4; delay_ = false; }

  uint64_t gpr[32] = {};
  uint64_t hi = 0, lo = 0;
  uint64_t fpr[32] = {};
  uint32_t fcr31 = 0;
  // COP0
  uint64_t cop0[32] = {};
  struct Tlb { uint32_t mask = 0; uint64_t hi = 0; uint32_t lo0 = 0, lo1 = 0; bool g = false; };
  Tlb tlb[32];
  int64_t cycles = 0;       // relógio da CPU (tiques de 62,5 MHz)
  uint64_t pc() const { return pc_; }
  uint32_t count() const { return uint32_t((uint64_t(cycles) * 3 + countBase_) >> 2); }
  uint64_t instrucoes = 0;  // diagnóstico

private:
  N64& sys;
  uint64_t pc_ = 0, npc_ = 0, curPc_ = 0;
  bool delay_ = false, nextDelay_ = false;
  bool llbit_ = false;
  bool irq_ = false;          // há interrupção para atender
  int64_t compareAt_ = 0;     // ciclo em que Count == Compare
  uint64_t countBase_ = 0;   // em quartos de Count
  uint64_t fpage_ = 1;       // página de 4 KB da última busca de instrução (1 = nenhuma)
  const uint8_t* fptr_ = nullptr;
  bool exc_ = false;          // a instrução atual gerou exceção

  void step(uint32_t op);
  void special(uint32_t op);
  void regimm(uint32_t op);
  void cop0op(uint32_t op);
  void cop1op(uint32_t op);
  void exception(int code, int ce = 0);
  void tlbException(int code, uint64_t va, bool refill);
  void addrError(int code, uint64_t va);
  void branch(bool cond, uint32_t op, bool likely);
  void scheduleCompare();
  uint64_t readCop0(int r);
  void writeCop0(int r, uint64_t v);

  // tradução de endereço: devolve false (e gera a exceção) se não der
  bool translate(uint64_t va, uint32_t& pa, bool write);
  bool tlbLookup(uint64_t va, uint32_t& pa, bool write);
  // memória
  bool load8(uint64_t va, uint64_t& v);
  bool load16(uint64_t va, uint64_t& v);
  bool load32(uint64_t va, uint64_t& v);
  bool load64(uint64_t va, uint64_t& v);
  bool store8(uint64_t va, uint32_t v);
  bool store16(uint64_t va, uint32_t v);
  bool store32(uint64_t va, uint32_t v);
  bool store64(uint64_t va, uint64_t v);
  bool fetch(uint64_t va, uint32_t& op);
  bool fetchSlow(uint64_t va, uint32_t& op);

  // FPU
  bool fr() const { return (cop0[12] >> 26) & 1; }
  uint32_t fget32(int r) const;
  void fset32(int r, uint32_t v);
  uint64_t fget64(int r) const;
  void fset64(int r, uint64_t v);
  bool fpuCheck(uint32_t flags);  // atualiza causa/flags; true = exceção
  bool fpuUnimpl();
};

}  // namespace indigo::n64
