// MDEC do PlayStation 1 (decodificador de macroblocos, usado nos vídeos .STR), em 0x1F801820:
//  - +0 escrita: comando/parâmetros (a FIFO de entrada tem 32 palavras); +0 leitura: dados de saída;
//  - +4 escrita: controle (bit 31 reinicia, bit 30 liga o pedido de DMA0, bit 29 o de DMA1);
//    +4 leitura: status (31 saída vazia, 30 entrada cheia, 29 ocupado, 28/27 pedidos de DMA, 26-23 formato,
//    18-16 bloco atual, 15-0 palavras restantes menos 1).
// Comandos: 1 decodifica macroblocos (saída de 4, 8, 24 ou 15 bits, com ou sem sinal, bit 15 de máscara),
// 2 carrega as tabelas de quantização (luminância e, se o bit 0 estiver ligado, cor), 3 carrega a tabela de
// escala da IDCT; os demais só refletem os bits no status.
// A decodificação segue o hardware: RLE com a desquantização e a saturação do chip, IDCT em duas passadas com
// os coeficientes de 13 bits e arredondamento de 15 bits, saturação de 9 para 8 bits e a conversão YCbCr->RGB
// com os coeficientes e truncamentos do chip.
// Fluxo das FIFOs como no console (medido pelo ps1-tests mdec/step-by-step-log, status igual em todas as
// leituras): a decodificação só começa quando a FIFO de entrada enche (32 palavras) ou chega a última palavra, e
// volta a esperar encher sempre que ela esvazia. Um bloco pronto passa por um estágio (a IDCT) e de lá vai para a
// FIFO de saída quando ela baixa de meio bloco; enquanto o estágio está ocupado, a leitura do bloco seguinte para
// antes da palavra que o terminaria (e o status mostra a entrada "cheia"). Cr e Cb saem do estágio quando a saída
// baixa de 1 bloco e 1/4. O campo do bloco atual mostra o bloco do estágio, ou o que está sendo lido.
// A saída sai pela porta de dados na ordem do chip (blocos de 8x8) e o DMA1 recebe junto o deslocamento de cada
// palavra dentro do macrobloco colorido (que o canal usa para gravar a RAM em linhas de 16 pixels).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::ps1 {

class Mdec {
public:
  Mdec() { reset(); }
  void reset();

  uint32_t read(uint32_t offset, int size);
  void write(uint32_t offset, uint32_t value, int size);
  void run(int cycles);

  // DMA0 (entrada) e DMA1 (saída); off recebe o deslocamento (em palavras) da posição na RAM
  void dmaWrite(uint32_t v);
  uint32_t dmaRead(int32_t& off);
  bool dmaInRequest() const;
  bool dmaOutRequest() const;

  void save(StateIO& io) { sync(io); }
  void load(StateIO& io) { sync(io); }

  uint32_t status() const;

private:
  static constexpr int IN_FIFO = 32;
  static constexpr int OUT_MAX = 96;       // a saída nunca passa de meio bloco mais um bloco (24 bits: 73)
  static constexpr int BLOCK_CYCLES = 512; // tempo médio de um bloco de 8x8 no chip

  void sync(StateIO& io);
  void process();
  void startCommand(uint32_t cmd);
  bool decodeHalf(uint16_t h);   // true quando um bloco terminou
  bool endsBlock(uint32_t w) const;  // a palavra termina o bloco em decodificação?
  void finishBlock();
  void idct(const int16_t* in, int8_t* out) const;
  void emitMono();
  void emitColor(int blk);
  void pop();
  int depth() const { return (cmd_ >> 27) & 3; }
  int blockWords() const;     // palavras de um bloco de 8x8 no formato atual
  bool full() const { return inCount_ >= IN_FIFO || (stalled_ && inCount_ > 0); }
  bool decoding() const { return inCommand_ && (cmd_ >> 29) == 1; }

  // FIFO de entrada (comandos e parâmetros, na ordem em que chegaram)
  uint32_t in_[IN_FIFO] = {};
  uint8_t inRd_ = 0, inCount_ = 0;
  bool started_ = false;         // a FIFO encheu (ou chegou a última palavra): a decodificação anda
  bool stalled_ = false;         // a decodificação parou esperando o estágio

  // quem escreve: palavras de parâmetro que ainda faltam chegar e o campo 15-0 do status
  uint32_t wLeft_ = 0;
  uint16_t wStatus_ = 0xFFFF;

  // estado do comando no decodificador
  uint32_t cmd_ = 0, control_ = 0;
  uint32_t remaining_ = 0;       // palavras de parâmetro que o decodificador ainda vai consumir
  bool inCommand_ = false;
  uint8_t tableIndex_ = 0;       // posição na carga das tabelas
  int32_t busy_ = 0;             // ciclos até o chip aceitar a próxima palavra

  // tabelas
  uint8_t quant_[2][64] = {};    // [0] luminância, [1] cor
  int16_t scale_[64] = {};       // tabela de escala (já com os 13 bits usados), [linha*8+coluna]

  // bloco em decodificação
  int16_t coef_[64] = {};
  uint8_t coefIndex_ = 0, qscale_ = 0, block_ = 0;  // block_: 0 Cr, 1 Cb, 2..5 Y1..Y4
  int8_t cr_[64] = {}, cb_[64] = {}, y_[4][64] = {};

  // estágio: bloco pronto esperando a saída (-1 = vazio; senão o número do status: 0..3 Y1..Y4, 4 Cr/mono, 5 Cb)
  int8_t stage_ = -1;
  bool stageOut_ = false;        // o estágio tem palavras para a saída (Y ou mono)
  uint8_t stageWords_ = 0;
  uint32_t stageBuf_[48] = {};
  int16_t stageOff_[48] = {};

  // saída: palavras na ordem do chip (8x8) e o deslocamento de cada uma para o DMA1
  uint32_t out_[OUT_MAX] = {};
  int16_t outOff_[OUT_MAX] = {};
  uint8_t outRd_ = 0, outCount_ = 0;
};

}  // namespace indigo::ps1
