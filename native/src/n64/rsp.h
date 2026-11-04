// RSP (Reality Signal Processor) do Nintendo 64: CPU MIPS escalar de 32 bits (sem multiplicação, divisão nem
// exceções) com 4 KB de DMEM e 4 KB de IMEM, e a unidade vetorial (COP2): 32 registros de 8 elementos de 16 bits,
// acumulador de 48 bits por elemento, VCO/VCC/VCE, recíproco e raiz recíproca por tabela. É LLE: roda qualquer
// microcódigo. Escrito a partir da documentação pública do RSP (n64brew e manuais do SDK), sem código de terceiros.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::n64 {

class N64;

class Rsp {
public:
  explicit Rsp(N64& s);
  void reset();
  // roda até `ate` ciclos (relógio do RSP); nada se parado
  void run(int64_t ate);
  void sync(StateIO& io);
  // SP_PC (escrita pela CPU)
  void setPc(uint32_t v) { pc = v & 0xFFC; npc_ = (pc + 4) & 0xFFC; nextDelay_ = false; }

  uint32_t pc = 0;    // 12 bits (IMEM)
  int64_t cycles = 0;
  uint64_t instrucoes = 0;

  // registros escalares
  uint32_t r[32] = {};
  // unidade vetorial
  struct V { uint16_t e[8]; };
  V vr[32] = {};
  int64_t acc[8] = {};     // 48 bits com sinal
  uint16_t vco = 0, vcc = 0;
  uint8_t vce = 0;
  int16_t divOut = 0, divIn = 0;
  bool divLoaded = false;

private:
  N64& sys;
  uint32_t npc_ = 0, cur_ = 0;
  bool delay_ = false, nextDelay_ = false;

  void step(uint32_t op);
  void cop2(uint32_t op);
  void vload(uint32_t op);
  void vstore(uint32_t op);
  uint8_t dm(uint32_t a) const;
  void dmw(uint32_t a, uint8_t v);
  uint8_t vb(int reg, int b) const { return b & 1 ? uint8_t(vr[reg].e[b >> 1]) : uint8_t(vr[reg].e[b >> 1] >> 8); }
  void vbw(int reg, int b, uint8_t v) {
    uint16_t& e = vr[reg].e[b >> 1];
    e = b & 1 ? uint16_t((e & 0xFF00) | v) : uint16_t((e & 0x00FF) | (v << 8));
  }
  int32_t rcp(int32_t in, bool dp);
  int32_t rsq(int32_t in, bool dp);
};

}  // namespace indigo::n64
