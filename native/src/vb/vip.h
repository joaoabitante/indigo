// VIP (Visual Image Processor) do Virtual Boy: desenha 32 "mundos" (planos normais, com deslocamento por linha
// — H-bias —, afins com rotação/escala, ou grupos de objetos) em blocos de 8 linhas nos framebuffers duplos de
// cada olho (2 bits por ponto, em colunas), e mostra as colunas nos espelhos com o brilho de BRTA/BRTB/BRTC e a
// repetição da tabela de colunas. Tempo de um quadro: 4 regiões de 384 colunas × 259 ciclos = 397 824 ciclos
// (50,27 Hz): espera, olho esquerdo, espera, olho direito.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::vb {

class VB;

class VIP {
public:
  static constexpr int W = 384, H = 224;
  static constexpr int COLUMN_CYCLES = 259;
  static constexpr int BLOCK_CYCLES = 1120 * 4;
  static constexpr int FRAME_CYCLES = COLUMN_CYCLES * W * 4;

  explicit VIP(VB& vb) : vb_(vb) {}
  void reset();
  void update(int64_t ts);       // processa as colunas e blocos até o ciclo ts
  int64_t nextEvent() const;      // ciclo do próximo evento
  uint8_t read8(uint32_t a);
  uint16_t read16(uint32_t a);
  void write8(uint32_t a, uint8_t v);
  void write16(uint32_t a, uint16_t v);
  void sync(StateIO& io);
  void clampClock(int64_t ts) { if (lastTs_ > ts || lastTs_ < ts - FRAME_CYCLES) lastTs_ = ts; if (sboutUntil_ > ts + 1120) sboutUntil_ = -1; }

  bool frameStarted = false;  // um quadro novo começou (o console termina runFrame aqui)

  // intensidade de cada ponto (0–255, já com brilho e repetição) do último quadro mostrado, por olho
  uint8_t lum[2][W * H] = {};

  // memória
  uint8_t fb[2][2][0x6000] = {};  // [quadro 0/1][olho esquerdo/direito], 64 bytes por coluna
  uint8_t chr[0x8000] = {};       // 2048 caracteres de 8×8 (16 bytes cada)
  uint8_t dram[0x20000] = {};     // mapas de fundo, parâmetros, mundos, tabela de colunas e objetos

private:
  uint16_t dramH(uint32_t halfIndex) const {
    const uint32_t i = (halfIndex & 0xFFFF) << 1;
    return static_cast<uint16_t>(dram[i] | (dram[i + 1] << 8));
  }
  uint16_t chrRow(uint32_t ch, uint32_t row) const {
    const uint32_t i = ((ch & 0x7FF) << 4) | (row << 1);
    return static_cast<uint16_t>(chr[i] | (chr[i + 1] << 8));
  }
  uint16_t readReg(uint32_t a);
  void writeReg(uint32_t a, uint16_t v);
  void checkIrq();
  void recalcBrightness();
  void drawBlock(int block);
  void drawNormal(uint8_t* line, int y, int eye, uint16_t w0, const uint16_t* wd);
  void drawAffine(uint8_t* line, int y, int eye, uint16_t w0, const uint16_t* wd);
  void drawObjects(int y, int group, const bool on[2]);
  void copyColumn(int eye, int column);

  VB& vb_;
  // registros
  uint16_t intPending_ = 0, intEnable_ = 0, dpctrl_ = 0, xpctrl_ = 0, frmcyc_ = 0, bkcol_ = 0;
  uint8_t brta_ = 0, brtb_ = 0, brtc_ = 0, rest_ = 0, sbcmp_ = 0, repeat_ = 0;
  uint16_t spt_[4] = {}, gplt_[4] = {}, jplt_[4] = {};
  // relógio de exibição e desenho
  int64_t lastTs_ = 0, sboutUntil_ = -1;
  int32_t column_ = 0, columnCounter_ = COLUMN_CYCLES, region_ = 0, gameFrame_ = 0;
  int32_t drawCounter_ = 0, drawBlock_ = 0, sbLatch_ = 0;
  bool displayActive_ = false, displayFb_ = false, drawActive_ = false, drawFb_ = false;
  // derivados (recalculados ao carregar)
  uint8_t bright_[4] = {};
  // linhas de trabalho do bloco em desenho: 8 linhas por olho, com folga de 8 pontos de cada lado
  uint8_t line_[2][8][W + 16] = {};
};

}  // namespace indigo::vb
