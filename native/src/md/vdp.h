// VDP do Mega Drive (modo 5): planos A/B com rolagem por tela/célula/linha e por coluna de 2 células, janela,
// 80 sprites (limites de 20/16 por linha e 320/256 pontos, máscara com X = 0), prioridades, sombra/realce,
// DMA 68k->VRAM/CRAM/VSRAM, preenchimento e cópia, interrupções de linha e de quadro, contador HV,
// H32/H40, V28/V30 e entrelaçado (modo 2 = 448 linhas, usado no Sonic 2 em dois jogadores).
//
// A linha é desenhada inteira quando começa (registradores mudados na interrupção de linha valem para a
// linha seguinte, como no aparelho).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::md {

class VdpHost {
public:
  virtual ~VdpHost() = default;
  virtual uint16_t dmaRead(uint32_t addr) = 0;  // leitura do 68k para o DMA (endereço par)
  virtual void lineDrawn(int, uint32_t*, const uint8_t*, int) {}  // linha desenhada (só com idxOut ligado)
};

class VDP {
public:
  static constexpr int MCLK_PER_LINE = 3420;
  static constexpr int FB_W = 320, FB_H = 480;

  explicit VDP(VdpHost& h);
  void reset(bool pal);

  // portas
  void writeData(uint16_t v, int64_t now);
  void writeCtrl(uint16_t v, int64_t now);
  uint16_t readData();
  uint16_t readStatus(int64_t now, int mclkInLine);
  uint16_t hvCounter(int mclkInLine) const;

  // quadro
  void startLine(int line, int64_t now);   // começo da linha (desenha, conta a interrupção de linha)
  void vint();                             // interrupção de quadro (linha da tela + 788 ciclos)
  int irqLevel() const;
  void ack(int level);
  int linesPerFrame() const { return pal ? 313 : 262; }
  // modo 4 (compatibilidade com o Master System, bit M5 desligado): 192 linhas; não é desenhado (tela preta)
  bool mode4() const { return !(reg[1] & 4); }
  int activeLines() const { return mode4() ? 192 : (pal && (reg[1] & 8)) ? 240 : 224; }
  bool interlace2() const { return (reg[12] & 6) == 6; }
  bool h40() const { return (reg[12] & 0x81) != 0; }
  int64_t takeStall() { const int64_t s = stall_; stall_ = 0; return s; }

  uint32_t fb[FB_W * FB_H] = {};
  uint8_t* idxOut = nullptr;  // se ligado: índice de cor (0-63) de cada ponto da linha, e o host é avisado
  int outW = 320, outH = 224;
  bool pal = false;

  std::vector<uint8_t> vram;
  uint16_t cram[64] = {}, vsram[40] = {};
  uint8_t reg[32] = {};

  void sync(StateIO& io);

private:
  void writeReg(int r, int v);
  void portWrite(uint16_t v);
  void vramWrite(uint32_t a, uint8_t v);
  void dma68k(int64_t now);
  void dmaFill(uint16_t v, int64_t now);
  void dmaCopy(int64_t now);
  int dmaLength() const { const int n = reg[19] | (reg[20] << 8); return n ? n : 0x10000; }
  int64_t dmaEnd(int64_t now, int units, int type) const;
  int64_t dmaEndFrom(int line, int64_t lineStart, int64_t now, int units, int type) const;
  void advanceDma(int64_t now);
  int64_t dmaHorizon() const;  // type 0 CRAM/VSRAM, 1 VRAM, 2 preenchimento, 3 cópia
  void renderLine(int line);
  int renderLineBuf(int line, uint32_t* out, int& row);
  void frameStart();
  void renderPlane(uint8_t* buf, int line, int y2, bool planeB, int x0, int x1);
  void renderWindow(uint8_t* buf, int y2, int x0, int x1);
  void renderSprites(uint8_t* buf, int y2);
  void parseSprites(int y2);
  void updateColor(int i);

  VdpHost& host;
  bool pending_ = false;
  uint8_t code_ = 0;
  uint16_t addr_ = 0;
  bool fillPending_ = false;
  uint16_t status_ = 0x3400;
  bool vintPending_ = false, hintPending_ = false;
  int hcount_ = 0;
  int line_ = 0;
  bool oddField_ = false;
  bool lastLineDotOverflow_ = false;
  int64_t dmaBusyUntil_ = 0, lineStart_ = 0;
  int64_t fifo_[4] = {};
  int fifoIdx_ = 0;
  uint32_t dmaSrc_ = 0;
  int dmaLeft_ = 0, dmaDone_ = 0, dmaL0_ = 0, dmaType_ = 0;
  int64_t dmaT0_ = 0, dmaLS0_ = 0;
  int64_t stall_ = 0;
  uint16_t readBuf_ = 0;
  uint8_t satCache_[0x400] = {};
  struct Spr { int row; int size; uint16_t name, x; };
  Spr spr_[20] = {};
  int sprN_ = 0;
  uint32_t lut_[3][64] = {};   // normal, sombra, realce

  // buffers de uma linha (com 32 pontos de folga de cada lado)
  uint8_t lineA_[320 + 64] = {}, lineB_[320 + 64] = {}, lineS_[320 + 64] = {};
};

}  // namespace indigo::md
