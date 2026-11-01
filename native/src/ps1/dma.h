// DMA (1F801080..1F8010FF): 7 canais (0 MDEC entrada, 1 MDEC saída, 2 GPU, 3 CD-ROM, 4 SPU, 5 PIO, 6 OTC), nos
// modos bloco único (manual), blocos sob pedido (sync) e lista ligada (GPU), com DPCR e as interrupções do DICR.
// O bloco único acontece de uma vez e a CPU fica parada o tempo da transferência. Nos blocos sob pedido, os canais
// do MDEC (0 entrada, 1 saída), do CD (3) e da SPU (4) só passam um bloco quando o aparelho pede (dmaInRequest,
// dmaOutRequest, dmaRequest); entre um bloco e outro a CPU anda. A lista ligada da GPU é lida de uma vez na partida
// (como no console, o DMA chega aos pacotes antes de a CPU reescrevê-los para o quadro seguinte) e o canal termina
// quando a GPU acaba de desenhar; uma lista sem fim (que aponta para si mesma) passa a andar aos poucos, dividindo
// o barramento com a CPU, e não trava o console. Com o "chopping" (bit 8 do CHCR, bloco único), a DMA passa
// janelas de 2^N palavras e a CPU anda 2^M ciclos entre uma e outra; MADR e BCR andam junto.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::ps1 {

class System;

class Dma {
public:
  explicit Dma(System& s) : sys_(s) {}
  void reset();
  uint32_t read(uint32_t off);
  void write(uint32_t off, uint32_t v);
  void sync();                   // continua a lista ligada da GPU e os canais que esperam pedido do aparelho
  uint64_t nextEvent() const;
  void sync(StateIO& io);

private:
  struct Ch { uint32_t madr, bcr, chcr; };
  System& sys_;
  Ch ch_[7] = {};
  uint32_t dpcr_ = 0x07654321, dicr_ = 0;
  uint64_t last_ = 0;
  bool listActive_ = false;
  uint8_t waiting_ = 0;          // canais em blocos sob pedido esperando o aparelho (bit por canal)
  uint64_t readyAt_[7] = {};
  uint64_t listEndAt_ = 0;       // lista lida inteira: o canal 2 termina neste ciclo (0 = nada pendente)
  uint8_t chopping_ = 0;         // canais em bloco único com "chopping" (bit por canal)
  uint64_t chopAt_[7] = {};      // ciclo em que a próxima janela da DMA começa (a CPU anda até lá)

  void start(int c);
  void finish(int c);
  void runList(uint32_t budget, bool eager);
  bool request(int c) const;
  void pump(int c);
  void chop(int c);
  uint32_t transfer(int c, uint32_t a, uint32_t words);
  void updateIrq();
  bool enabled(int c) const { return (dpcr_ >> (c * 4 + 3)) & 1; }
};

}  // namespace indigo::ps1
