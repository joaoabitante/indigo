// CPU NEC V810 (Virtual Boy a 20 MHz, PC-FX a 21,48 MHz): 32 registros de 32 bits, registros de sistema (PSW,
// EIPC, ECR...), unidade de ponto flutuante IEEE simples, instruções de cadeia de bits (bit string), as instruções
// próprias da Nintendo (XB, XH, REV, MPYHW, só no Virtual Boy), espaço de E/S separado (IN/OUT) e o cache de
// instruções de 1 KB (só o efeito na temporização). A semântica segue o manual do usuário da NEC (V810 Family
// 32-bit Microprocessor, U10082EJ) e o comportamento conhecido dos consoles (exceções, interrupções de nível 0 a
// 15, valores do PIR e TKCW).
//
// A CPU é um modelo (template) sobre o barramento do console, que precisa ter:
//   int64_t ts, runUntil;                         relógio da CPU e limite da fatia atual
//   read8/16/32, write8/16/32                     memória
//   in8/16/32, out8/16/32                         espaço de E/S (IN.x/OUT.x)
//   fetch16                                       busca de instrução
//   static constexpr uint32_t V810_PIR;           valor do PIR
//   static constexpr bool V810_NINTENDO;          aceita XB/XH/REV/MPYHW
// A implementação fica em v810_impl.h, incluída por um .cpp de cada console (instanciação explícita).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::v810 {

template <class Bus>
class Cpu {
public:
  explicit Cpu(Bus& bus) : bus_(bus) {}

  void reset();
  // executa instruções até o relógio do barramento (Bus::ts) alcançar Bus::runUntil
  void run();
  void sync(StateIO& io);

  // registros de sistema
  enum { EIPC = 0, EIPSW = 1, FEPC = 2, FEPSW = 3, ECR = 4, PSW = 5, PIR = 6, TKCW = 7, CHCW = 24, ADTRE = 25 };
  // bits do PSW
  enum : uint32_t {
    F_Z = 1u << 0, F_S = 1u << 1, F_OV = 1u << 2, F_CY = 1u << 3,
    F_FPR = 1u << 4, F_FUD = 1u << 5, F_FOV = 1u << 6, F_FZD = 1u << 7, F_FIV = 1u << 8, F_FRO = 1u << 9,
    F_ID = 1u << 12, F_AE = 1u << 13, F_EP = 1u << 14, F_NP = 1u << 15,
  };

  uint32_t r[32] = {};
  uint32_t pc = 0;
  uint32_t sr[32] = {};
  uint8_t halt = 0;     // 0 = rodando, 1 = HALT (acorda com interrupção), 2 = exceção fatal (parada definitiva)
  int8_t irqLevel = -1;  // maior nível de interrupção pedido pelos periféricos (-1 = nenhum)
  int8_t lastOp = 0;     // tipo da instrução anterior (cargas/gravações seguidas custam menos)

  // gera uma exceção; o PC atual é o endereço de retorno
  void exception(uint32_t handler, uint16_t code);

private:
  void step();
  void interrupt();
  bool cond(int c) const;
  uint16_t fetch(uint32_t a, bool second);
  void setSZ(uint32_t v) { sr[PSW] = (sr[PSW] & ~(F_Z | F_S)) | (v ? 0u : uint32_t(F_Z)) | (v & 0x80000000u ? uint32_t(F_S) : 0u); }
  void flag(uint32_t f, bool on) { sr[PSW] = on ? (sr[PSW] | f) : (sr[PSW] & ~f); }
  uint32_t add(uint32_t a, uint32_t b);
  uint32_t sub(uint32_t a, uint32_t b);
  void ldsr(int reg, uint32_t v);
  void cacheControl(uint32_t v);
  bool bitString(int sub);  // devolve true se a instrução ainda não terminou (foi interrompida pelo relógio)
  void fpu(int sub, int reg2, int reg1);
  bool fpuReserved(uint32_t a, uint32_t b);
  void fpuResult(int reg2, float f, bool inexact, bool exactZero);

  Bus& bus_;
  // cache de instruções: 128 linhas de 8 bytes; guardamos só as etiquetas (os dados vêm da memória)
  uint32_t cacheTag_[128] = {};
  uint8_t cacheValid_[128] = {};
};

}  // namespace indigo::v810
