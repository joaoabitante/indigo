// DSPP: o processador de som do 3DO (dentro do CLIO). Roda um programa (microcódigo) uma vez por amostra de som
// (44.100 por segundo) e entrega um par esquerdo/direito ao DAC. Escrito a partir da descrição pública da
// arquitetura (palavra de instrução de 16 bits; instruções aritméticas com até 3 operandos, seletores MUXA/MUXB,
// multiplicador 16x16, ULA de 16 operações e deslocador; instruções de controle com desvio condicional, desvio
// pelo acumulador, sub-rotina, base de registradores e SLEEP no fim da amostra). Os detalhes de codificação abaixo
// são os deste núcleo; conferir com o microcódigo do Portfolio quando houver BIOS e disco de sistema do usuário.
//
// Memória de dados (palavras de 16 bits, endereços de 10 bits):
//   0x000-0x0FF  EI: palavras escritas pela CPU (CLIO 0x3000 + 4*i)
//   0x100-0x2FF  RAM de trabalho
//   0x300-0x30C  leitura: próxima amostra de 16 bits do canal de DMA 0..12 (FIFO de entrada)
//   0x3E0-0x3EF  EO: palavras lidas pela CPU (CLIO 0x3800 + 4*i)
//   0x3F0/0x3F1  escrita: saída esquerda/direita para o DAC
//   0x3F8        leitura: ruído (pseudoaleatório)  0x3FA leitura: contador de amostras
//   0x3FE        escrita: interrompe a CPU (DSPPINT)
//
// Instrução aritmética (bit 15 = 0):
//   14-13 NOPS (palavras de operando a seguir), 12 M2SEL (2º fator do multiplicador = ACC), 11-10 MUXA,
//   9-8 MUXB (0 = ACC, 1 = operando seguinte, 2 = zero, 3 = produto), 7-4 ULA, 3-0 deslocador.
//   Ordem de consumo dos operandos: fatores do multiplicador (se MUXA ou MUXB = 3), A (se MUXA = 1), B (MUXB = 1);
//   um operando de endereço que sobrar recebe o resultado. Um operando com o bit 13 também recebe o resultado.
//   ULA: 0 TRA(A) 1 NEG 2 A+B 3 A+B+C 4 A-B 5 A-B-!C 6 A+1 7 A-1 8 TRL(B) 9 NOT 10 AND 11 NAND 12 OR 13 NOR
//        14 XOR 15 XNOR.  Deslocador: 0 nada, 1..5 <<1..5, 6 <<8, 7 saturação em 16 bits, 8 >>8, 9..15 >>1..7
//        (aritmético).
// Operando: bit 15 = 1 imediato de 13 bits com sinal (bit 13 = deslocado 3 bits para a esquerda);
//   bits 15-14 = 01 registrador (RBASE + bits 3-0); bits 15-14 = 00 endereço (bits 9-0, bit 10 indireto).
// Controle (bit 15 = 1), bits 14-13:
//   00 especial (bits 12-10): 0 NOP, 1 BAC (PC = ACC), 2 RTS, 3 RBASE = bits 9-0, 4 SLEEP (fim da amostra)
//   01 desvio condicional (bits 12-10: 0 sempre, 1 Z, 2 N, 3 C, 4 !Z, 5 !N, 6 !C, 7 V) para bits 9-0
//   10 MOVE: memória[bits 9-0] = próxima palavra
//   11 JUMP (bit 12 = JSR, guarda o retorno numa pilha de 4) para bits 9-0
#pragma once
#include <cstdint>
#include <functional>

#include "../core/state.h"

namespace indigo::tdo {

class Dspp {
public:
  static constexpr int CODE_WORDS = 512;
  static constexpr int DATA_WORDS = 1024;
  static constexpr int MAX_STEPS = 565;  // instruções por amostra (25 MHz / 44,1 kHz)

  uint16_t code[CODE_WORDS] = {};
  uint16_t data[DATA_WORDS] = {};
  bool running = false;
  uint16_t pc = 0, rbase = 0;
  int32_t acc = 0;
  bool fz = false, fn = false, fc = false, fv = false;
  uint16_t stack[4] = {};
  uint8_t sp = 0;
  uint32_t frameCount = 0, noise = 0x1234567;
  int16_t outL = 0, outR = 0;

  // ligação com o resto do CLIO: leitura do FIFO do canal de DMA e pedido de interrupção
  std::function<uint16_t(int)> readFifo;
  std::function<void()> irq;

  void reset();
  // roda o programa de uma amostra (do PC 0 até o SLEEP)
  void runSample();
  void sync(StateIO& io);

  uint16_t read(uint16_t a);
  void write(uint16_t a, uint16_t v);

private:
  uint16_t fetch() { const uint16_t w = code[pc & (CODE_WORDS - 1)]; pc = uint16_t((pc + 1) & (CODE_WORDS - 1)); return w; }
  bool arith(uint16_t ins);
  bool control(uint16_t ins);
};

}  // namespace indigo::tdo
