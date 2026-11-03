// DSPP: o processador de som do 3DO (dentro do CLIO). A cada amostra de som (44.100 por segundo) roda o
// programa do começo (PC 0) até o SLEEP, com 568 ciclos de 25 MHz por amostra, e entrega ao DAC o par
// esquerdo/direito escrito em 0x3FE/0x3FF. Conjunto de instruções e mapa conferidos com a documentação pública do
// 3DO e com o DSPP do MAME (licença BSD-3-Clause, de Philip Bennett), reescritos aqui.
//
// Instrução aritmética (bit 15 = 0): 14-13 NOPS (operandos a seguir; 0 com MUXA/MUXB pedindo operando = 4),
//   12 M2SEL (2º fator do multiplicador: operando em vez do ACC), 11-10 MUXA, 9-8 MUXB (0 = ACC, 1/2 = operando,
//   3 = produto), 7-4 ULA, 3-0 deslocador (8 = o deslocamento vem num operando a mais).
//   ACC de 20 bits (16.4); o operando entra deslocado 4 bits; produto = (a * b) >> 11.
//   ULA: 0 TRA 1 NEG(B) 2 A+B 3 A+C 4 A-B 5 A-C 6 A+1 7 A-1 8 TRL 9 NOT 10 AND 11 NAND 12 OR 13 NOR 14 XOR 15 XNOR
//   Deslocador: 0-5 e 6 = <<8 (bits 0-2: 0,1,2,3,4,5,8; 7 = satura em 16 bits), bit 3 = para a direita
//   (aritmético nas operações 0-7, lógico nas outras).
//   O resultado (ACC >> 4) vai para o endereço marcado com "escreve de volta" ou, se sobrar, para o operando
//   seguinte.
// Operandos: 11J vvvvvvvvvvvvv imediato de 13 bits (J = 1: deslocado 3 bits para a esquerda; senão com sinal);
//   100 W I aaaaaaaaaa endereço (I = indireto, W = escreve de volta); 101 W W n rrrrr rrrrr um ou dois
//   registradores; 0 rrrrr rrrrr rrrrr três registradores. Registrador: bits 0-3 número, bit 4 indireto.
//   Registradores pelo RMAP/RBASE: no RMAP 0 (o do sistema) R0-R3 = 0x000, R4-R7 = 0x104, R8-R11 = 0x200,
//   R12-R15 = 0x304, com o RBASE (bits 2-7) por XOR.
// Controle (bit 15 = 1), bits 14-13: 00 especial (bits 12-10: 0 superespecial (bits 9-7: 1 BAC, 2 RBASE,
//   3 RMAP, 4 RTS, 5 OP_MASK, 7 SLEEP), 1 JUMP, 2 JSR, 3 BFM, 4 MOVEREG, 5 RBASE, 6 MOVED, 7 MOVEI);
//   01/10 desvio (bit 12 escolhe N,V ou C,Z; bits 11-10 quais exigir; 10 = condição negada);
//   11 desvio composto (bits 12-10: BLT BLE BGE BGT BHI BLS BXS BXC). Alvo nos bits 9-0.
//
// Memória de dados (palavras de 16 bits, endereços de 10 bits):
//   0x000-0x06F EI (escrito pela CPU)  0x070-0x07E cabeça do FIFO do canal 0-14 sem tirar  0x080-0x0CF RAM
//   0x0D0-0x0DE quantos no FIFO de entrada  0x0E0-0x0E3 quantos no FIFO de saída  0x0EA ruído
//   0x0EC/0x0ED estado/dado do semáforo  0x0EE PC  0x0EF relógio  0x0F0-0x0FE tira uma amostra do FIFO
//   0x100-0x1FF I (RAM; 0x200-0x2FF é a mesma)  0x300-0x3EA EO (lido pela CPU)  0x3EB AUDLOCK (bit 15)
//   0x3EC/0x3ED confirma/escreve o semáforo  0x3EE escrita: interrompe a CPU (DSPPINT) com o valor
//   0x3EF relógio  0x3F0-0x3F3 FIFOs de saída (canais 16-19)  0x3FE/0x3FF DAC esquerdo/direito
// DMA (ligado pelo CLIO): canais 0-12 da RAM para o DSP e 16-19 do DSP para a RAM, cada um com FIFO de 8
// amostras, endereço/contagem atuais e "próximos" (contagem em bytes menos 4; o próximo, uma vez escrito,
// recarrega para sempre).
#pragma once
#include <cstdint>
#include <functional>

#include "../core/state.h"

namespace indigo::tdo {

class Dspp {
public:
  static constexpr int CODE_WORDS = 1024;
  static constexpr int DATA_WORDS = 1024;
  static constexpr int FRAME_TICKS = 568;  // ciclos de 25 MHz por amostra de 44,1 kHz
  static constexpr int CHANNELS = 20;
  static constexpr int FIFO_DEPTH = 8;

  uint16_t code[CODE_WORDS] = {};
  uint16_t data[DATA_WORDS] = {};
  bool running = false;  // GW (bit 0 de 0x17FC)
  uint16_t pc = 0;
  int32_t acc = 0;       // 20 bits com sinal
  bool fz = false, fn = false, fc = false, fv = false, fx = false;
  bool sleeping = false, audlock = false;
  uint16_t stack[4] = {};
  uint8_t sp = 0;
  uint16_t rbase[4] = {};
  uint8_t rmap = 0;
  uint16_t rbaseXor = 0;
  uint16_t tclock = 0, tick = 0;
  uint16_t semStatus = 0, semData = 0;
  uint32_t noise = 0x1234567;
  uint16_t outputs[2] = {};
  int16_t outL = 0, outR = 0;

  struct Fifo {
    uint32_t curAddr = 0, nextAddr = 0;
    int32_t curCount = 0, nextCount = 0;  // em amostras de 16 bits
    bool nextValid = false;
    uint16_t buf[FIFO_DEPTH] = {};
    uint8_t dmaPtr = 0, dspPtr = 0, depth = 0;
    uint16_t prev = 0;
  } fifo[CHANNELS];
  uint32_t chEnable = 0, chComplete = 0;

  // ligação com o resto do sistema: memória (bytes, big-endian), interrupção para a CPU e troca de bloco do DMA
  std::function<uint8_t(uint32_t)> readMem;
  std::function<void(uint32_t, uint8_t)> writeMem;
  std::function<void()> irq;
  std::function<void(int)> rollover;

  void reset();  // o DSPRESET: PC, pilha, semáforo, RMAP (não mexe na memória nem no DMA)
  void clearAll();
  // uma amostra: roda o programa do começo até o SLEEP (ou o fim dos ciclos) e atualiza outL/outR
  void runFrame();
  void sync(StateIO& io);

  uint16_t read(uint16_t a);
  void write(uint16_t a, uint16_t v);

  // lado da CPU
  void hostDmaWrite(int ch, int reg, uint32_t v);
  uint32_t hostDmaRead(int ch, int reg) const;
  void hostEnable(uint32_t set, uint32_t clr);
  void hostFifoInit(uint32_t mask);
  void hostSemaphore(uint16_t v) { semStatus = 8; semData = v; }
  void hostSemaphoreAck() { semStatus |= 2; }
  uint16_t fifoDepth(int ch) const { return ch >= 0 && ch < CHANNELS ? fifo[ch].depth : 0; }

private:
  struct Operand { int32_t value, addr; };
  Operand ops_[8] = {};
  int opIdx_ = 0, nOps_ = 0;
  int32_t writeback_ = -1;
  int ticks_ = 0;

  uint16_t fetch() { const uint16_t w = code[pc & (CODE_WORDS - 1)]; pc = uint16_t((pc + 1) & (CODE_WORDS - 1)); ticks_++; tclock--; return w; }
  void step();
  void parseOperands(int n);
  uint16_t nextOperand();
  void writeNextOperand(uint16_t v);
  uint16_t reg(int r) const { return uint16_t(rbase[(r >> 2) & 3] + (r & 3)); }
  void setRmap(uint8_t map, uint16_t base);
  void setRbase(int base, uint16_t addr);
  void arith(uint16_t op);
  void control(uint16_t op);
  uint16_t popFifo(int ch);
  bool serviceDma();
  void nextBlock(int ch);
};

}  // namespace indigo::tdo
