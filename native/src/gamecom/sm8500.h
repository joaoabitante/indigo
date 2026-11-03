// CPU Sharp SM8500 (o núcleo SM85 do microcontrolador SM8521 do Tiger Game.com).
//
// Não há manual público das instruções: a decodificação, os modos de endereçamento e os ciclos seguem o que a
// comunidade descobriu olhando os binários, como estão no núcleo sm8500 do MAME (Wilbert Pol, BSD-3-Clause; ver
// TERCEIROS.md). Diferenças conscientes em relação a ele (estão nos comentários do .cpp):
//   - o estouro (V) de ADC, ADDW, ADCW, INC, INCW, DEC e DECW usa a definição de sempre (o MAME usa a fórmula
//     da subtração em alguns e o sinal invertido em outros, sem documento que confirme);
//   - um pedido de interrupção que chega com as interrupções desligadas (I = 0 ou prioridade em PS0) fica
//     pendente até ser atendido ou até o programa zerar o bit em IR0/IR1, em vez de ser descartado; com o bit
//     de IE0/IE1 desligado ele é descartado (o bit de IR continua marcado, como no MAME).
//
// Mapa visto pela CPU:
//   $00-$0F  registradores R0-R15: janela de 16 bytes num banco interno de $108 bytes, a partir de PS0 & $F8
//   $10-$13  IE0, IE1, IR0, IR1 (interrupções)       $19 SYS (bit 6 = pilha de 16 bits)
//   $1A      CKC (troca de relógio: o valor novo só vale depois de um STOP, como no MAME)
//   $1C/$1D  SPH/SPL (pilha)   $1E PS0 (ponteiro dos registradores + prioridade)   $1F PS1 (flags)
//   o resto vai para o barramento (Sm8500Bus).
//
// Flags em PS1: C=$80 Z=$40 S=$20 V=$10 D=$08 H=$04 B=$02 I=$01.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::gamecom {

class Sm8500Bus {
public:
  virtual ~Sm8500Bus() = default;
  virtual uint8_t read(uint16_t a) = 0;
  virtual void write(uint16_t a, uint8_t v) = 0;
  // ciclos gastos (a cada instrução ou passo parado), com o estado de HALT; devolve nada
  virtual void tick(int cycles, bool halted) = 0;
};

class Sm8500 {
public:
  // linhas de interrupção (mesma numeração do MAME)
  enum Irq { IRQ_ILL = 0, IRQ_DMA, IRQ_TIM0, IRQ_EXT, IRQ_UART, IRQ_LCDC, IRQ_TIM1, IRQ_CK, IRQ_PIO, IRQ_WDT, IRQ_NMI };
  static constexpr uint8_t FC = 0x80, FZ = 0x40, FS = 0x20, FV = 0x10, FD = 0x08, FH = 0x04, FB = 0x02, FI = 0x01;

  explicit Sm8500(Sm8500Bus* bus) : bus_(bus) {}

  void reset();
  int step();                 // executa uma instrução (ou um passo parado); devolve os ciclos gastos
  void irq(int line);         // pede a interrupção (borda)
  void clearIrq(int line);

  // estado visível (testes e depuração)
  uint16_t pc = 0x1020;
  uint8_t ps0 = 0, ps1 = 0, sys = 0, sph = 0, spl = 0;
  uint8_t ie0 = 0, ie1 = 0, ir0 = 0, ir1 = 0;
  uint8_t ckc = 0;            // CKC escrito pelo programa
  uint8_t ckcApplied = 0;     // CKC em vigor (copiado no STOP que segue a escrita)
  bool ckcPending = false;    // CKC mudou e ainda não passou por um STOP
  uint32_t stops = 0;         // STOPs executados (testes e depuração)
  bool halted = false;
  uint16_t pending = 0;       // linhas pedidas e ainda não atendidas
  uint8_t regs[0x108] = {};   // banco dos registradores

  uint16_t sp() const { return uint16_t(spl | ((sys & 0x40) ? sph << 8 : 0)); }
  void setSp(uint16_t v) { spl = uint8_t(v); if (sys & 0x40) sph = uint8_t(v >> 8); }

  // acesso como a CPU vê (registradores, internos e barramento)
  uint8_t rd(uint16_t a);
  void wr(uint16_t a, uint8_t v);
  uint16_t rdw(uint16_t a) { return uint16_t(rd(a) << 8 | rd(uint16_t(a + 1))); }
  void wrw(uint16_t a, uint16_t v) { wr(a, uint8_t(v >> 8)); wr(uint16_t(a + 1), uint8_t(v)); }

  void sync(StateIO& s, int version = 2);  // version 1 = estados da 2.4 (sem CKC)

private:
  Sm8500Bus* bus_;

  uint8_t fetch() { return rd(pc++); }
  uint16_t fetchw() { const uint16_t v = rdw(pc); pc = uint16_t(pc + 2); return v; }
  void push(uint8_t v);
  uint8_t pop();
  bool checkIrqs();
  void dropCleared();
  void takeIrq(uint16_t vector);
  bool cond(uint8_t op) const;

  // operações da ULA (devolvem o resultado e acertam PS1)
  uint8_t sub8(uint8_t a, uint8_t b, int carry, bool cmp);
  uint16_t sub16(uint16_t a, uint16_t b, int carry, bool cmp);
  uint8_t add8(uint8_t a, uint8_t b, int carry);
  uint16_t add16(uint16_t a, uint16_t b, int carry);
  uint8_t logic8(uint8_t r);
  uint16_t logic16(uint16_t r);
  uint8_t unary(int which, uint8_t v);  // 0 CLR 1 NEG 2 COM 3 RR 4 RL 5 RRC 6 RLC 7 SRL 8 INC 9 DEC 10 SRA 11 SLL 12 DA 13 SWAP
  uint8_t alu8(int op, uint8_t a, uint8_t b, bool& store);  // op 0..8: CMP ADD SUB ADC SBC AND OR XOR MOV
};

}  // namespace indigo::gamecom
