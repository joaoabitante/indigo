// Chip S-DD1 dos cartuchos de SNES (Star Ocean e Street Fighter Alpha 2). Duas funções:
//   - MMC: os bancos $C0-$FF são quatro janelas de 1 MB trocáveis ($4804-$4807), para ROMs de até 16 MB;
//   - descompressão durante o DMA: com o canal ligado em $4800 e $4801, o DMA que lê da ROM em $C0-$FF recebe os
//     bytes já descomprimidos (codificação de Golomb adaptativa, por planos de bits, com o modelo de contexto do
//     chip). O algoritmo foi decifrado por Andreas Naive; esta é uma implementação própria do Índigo.
// O S-DD1 vê as gravações do SNES em $43x2-$43x6 (endereço e tamanho de cada canal de DMA) para saber qual leitura
// é a do DMA e quando a transferência acaba.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::snes {

class SDD1 {
public:
  explicit SDD1(const std::vector<uint8_t>& rom) : rom_(rom) { reset(); }
  void reset();

  uint8_t ioRead(uint16_t a, uint8_t openBus) const;  // $4800-$480F
  void ioWrite(uint16_t a, uint8_t v);
  void dmaRegWrite(uint16_t a, uint8_t v);  // espia $4300-$437F
  uint8_t romRead(uint32_t addr);           // $00-$3F/$80-$BF:$8000-$FFFF e $C0-$FF:$0000-$FFFF
  uint8_t romPeek(uint32_t addr) const { return (addr & 0x400000) ? mmcRead(addr) : const_cast<SDD1*>(this)->romRead(addr); }
  void sync(StateIO& io);

  // descompressor sozinho (para os testes): começa no endereço de 24 bits dos bancos $C0-$FF
  void startStream(uint32_t addr) { dec_.init(*this, addr); }
  uint8_t streamByte() { return dec_.read(*this); }

private:
  uint8_t mmcRead(uint32_t addr) const;

  struct Decompressor {
    // entrada (bits)
    uint32_t offset = 0;
    uint8_t bitCount = 4;
    // geradores de bits (um por código de Golomb 0-7)
    uint8_t mpsCount[8] = {};
    bool lpsIndex[8] = {};
    // estimativa de probabilidade (32 contextos)
    uint8_t status[32] = {}, mps[32] = {};
    // modelo de contexto
    uint8_t bitplanesInfo = 0, contextBitsInfo = 0, bitNumber = 0, currentBitplane = 0;
    uint16_t prevBits[8] = {};
    // saída
    uint8_t r0 = 1, r1 = 0, r2 = 0;

    void init(const SDD1& c, uint32_t addr);
    uint8_t read(const SDD1& c);
    uint8_t codeWord(const SDD1& c, int length);
    uint8_t bgBit(const SDD1& c, int code, bool& endOfRun);
    uint8_t pemBit(const SDD1& c, int context);
    uint8_t cmBit(const SDD1& c);
    void sync(StateIO& io);
  };

  const std::vector<uint8_t>& rom_;
  uint8_t r4800_ = 0, r4801_ = 0, mmc_[4] = {0, 1, 2, 3};
  uint32_t dmaAddr_[8] = {};
  uint16_t dmaSize_[8] = {};
  bool dmaReady_ = false;
  Decompressor dec_;
};

}  // namespace indigo::snes
