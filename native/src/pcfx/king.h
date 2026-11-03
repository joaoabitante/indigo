// KING (HuC6272) e o misturador de vídeo HuC6261 do PC-FX.
//  - KING: 1 MB de KRAM (duas páginas de 256 K palavras), portas de leitura/escrita da KRAM com incremento,
//    controlador SCSI do CD (sinais, fase, DMA do CD para a KRAM e "pseudo-DMA" pela porta de dados), quatro
//    planos de fundo (4, 16, 256 e 64 K cores; BAT+CG ou bitmap; plano 0 com sub-tela e rotação/escala) guiados
//    pelo microprograma, endereços do ADPCM (dois canais na KRAM, com fim, meio e repetição), interrupção de
//    linha e do CD/DMA/subcanal/ADPCM, e a transferência para o RAINBOW (rainbow.h): a cada 16 linhas um
//    bloco comprimido sai da KRAM para o descompressor e a faixa anterior vai para a tela.
//  - HuC6261: paleta de 512 cores YUV (Y 8 bits, U e V 4 bits), modo de imagem (relógio de 5,37 ou 7,16 MHz,
//    camadas ligadas), prioridades das 7 camadas, deslocamentos de paleta por camada, combinação dos dois VDCs,
//    celofane (mistura de cores) e chroma key; converte o resultado YUV em RGB.
// O comportamento segue o conhecido do aparelho, como documentado no código do Mednafen (king.cpp, GPL 2+).
#pragma once
#include <cstdint>

#include "../core/state.h"
#include "rainbow.h"
#include "scsicd.h"

namespace indigo::pcfx {

// camadas (os 4 bits de cima de cada pixel YUV de 24 bits)
enum Layer : uint32_t { L_NONE = 0, L_BG0, L_BG1, L_BG2, L_BG3, L_VDC_BG, L_VDC_SPR, L_RAINBOW };

struct FxVce {
  uint8_t ar = 0;
  uint16_t pictureMode = 0;
  uint16_t palOffsetRw = 0, palLatch = 0;
  uint16_t palOffset[4] = {};
  uint16_t priority[2] = {};
  uint16_t chromaY = 0, chromaU = 0, chromaV = 0;
  uint16_t ccr = 0, ble = 0, spbl = 0;
  uint16_t coef[6] = {};
  uint16_t pal[512] = {};
  // estado do quadro (mantido pelo sistema)
  uint16_t raster = 0;
  bool inHblank = false, oddField = false;

  uint32_t yuv[512] = {};  // paleta já em YUV de 24 bits

  void reset();
  uint16_t read16(uint32_t a);
  void write16(uint32_t a, uint16_t v);
  void sync(StateIO& io);
  void rebuild() { for (int i = 0; i < 512; i++) palCache(i); }
  void palCache(int i) {
    const uint16_t c = pal[i];
    yuv[i] = uint32_t(c >> 8) << 16 | uint32_t(c & 0xF0) << 8 | uint32_t(c & 0x0F) << 4;
  }
};

class King {
public:
  static constexpr int64_t NONE = INT64_MAX;

  King(ScsiCd& cd, FxVce& vce) : cd_(cd), vce_(vce) {}
  void reset(int64_t now);

  uint16_t read16(int64_t now, uint32_t a);  // a = porta 0x600..0x6FF
  void write16(int64_t now, uint32_t a, uint16_t v);
  int64_t update(int64_t t);  // avança o SCSI e o DMA; devolve o próximo evento
  bool irq() const { return irq_; }
  int64_t next() const { return cdNext_ < dmaNext_ ? cdNext_ : dmaNext_; }

  // começo de uma linha visível do HuC6261 (interrupção de linha)
  void rasterLine(int raster);
  // desenha os 4 planos na linha (YUV de 24 bits com a camada nos 4 bits de cima), 256 pontos + 8 de cada lado
  void drawBgLine(int raster, uint32_t* line272);

  // RAINBOW: começo de cada linha (0..261); preenche line (256 pontos, camada 7 nos bits de cima, 0 =
  // transparente) e devolve o formato da faixa mostrada (-1 = nenhuma, 0 = paleta, 1 = YUV)
  int rainbowLine(int raster, uint32_t* line);
  Rainbow rainbow;

  // ADPCM: o som pede meia palavra (4 amostras) do canal
  uint16_t adpcmHalfWord(int ch);
  uint16_t adpcmControl() const { return adpcmControl_; }

  uint16_t kram[2][262144];
  void sync(StateIO& io);
  bool temposOk(int64_t agora) const {
    return tempoOk(last_, agora) && tempoOk(cdNext_, agora, true) && tempoOk(dmaNext_, agora, true) && cd_.temposOk(agora);
  }

private:
  ScsiCd& cd_;
  FxVce& vce_;
  bool irq_ = false;
  int64_t last_ = 0, cdNext_ = 0, dmaNext_ = NONE;

  uint8_t ar_ = 0;
  uint32_t kramRA_ = 0, kramWA_ = 0;
  uint8_t kramMode_ = 0;
  uint32_t page_ = 0;
  uint16_t bgMode_ = 0, priority_ = 0, scrollMode_ = 0;
  uint16_t bgSize_[4] = {};
  uint8_t batAddr_[4] = {}, cgAddr_[4] = {}, subBat_ = 0, subCg_ = 0;
  uint16_t xScroll_[4] = {}, yScroll_[4] = {};
  uint16_t affA_ = 0, affB_ = 0, affC_ = 0, affD_ = 0, affX_ = 0, affY_ = 0;
  uint16_t mprogCtrl_ = 0, mprogAddr_ = 0, mprog_[16] = {};
  // ADPCM
  uint16_t adpcmControl_ = 0, adpcmBufMode_[2] = {}, adpcmSal_[2] = {}, adpcmMid_[2] = {}, adpcmStatus_[2] = {};
  uint32_t adpcmEnd_[2] = {}, adpcmPlay_[2] = {};
  bool adpcmIrq_ = false;
  // RAINBOW e linha
  uint16_t rbCtrl_ = 0, rbStart_ = 0, rbBlocks_ = 0, rasterLine_ = 0;
  uint32_t rbKram_ = 0, rbReadPos_ = 0;
  bool rasterIrq_ = false, rbPending_ = false;
  uint8_t rbBusy_ = 0, rbLeft_ = 0;
  // SCSI e DMA
  uint8_t reg01_ = 0, reg02_ = 0, reg03_ = 0;
  uint8_t subCtrl_ = 0, subBuf_ = 0, dataCache_ = 0, dmaLatch_ = 0;
  bool cdIrq_ = false, subIrq_ = false, dmaIrq_ = false;
  bool drq_ = false, dmaRecv_ = false, dmaSend_ = false, dmaFlip_ = false;
  uint32_t dmaAddr_ = 0, dmaSize_ = 0;
  uint16_t dmaStatus_ = 0;

  void redoIrq();
  void cdStep(int64_t t);  // roda o leitor e trata a subida do REQ
  void dmaStep(int64_t t);
  void realDma(uint8_t b);
  uint16_t scsiSignals() const;
  bool phaseMatch() const;
  void drawBg(uint32_t* target, int n, int raster);
};

}  // namespace indigo::pcfx
