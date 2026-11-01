// GPU do PlayStation por software: comandos GP0 (desenho e transferências) e GP1 (controle da tela), VRAM de
// 1 MB (1024 x 512 pontos de 16 bits), polígonos planos / gouraud / texturizados (CLUT de 4 e 8 bits, 15 bits),
// semitransparência, pontilhado (dithering), bit de máscara, janela de textura, retângulos, linhas e polilinhas,
// área de desenho e deslocamento. Saída de vídeo em 15/24 bits nas resoluções 256/320/368/512/640 x 240/480
// (entrelaçado), NTSC e PAL, com o tempo de linha (hblank) e de quadro (vblank) que alimentam os timers.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::ps1 {

class System;

class Gpu {
public:
  explicit Gpu(System& s);
  void reset();
  void setPal(bool pal) { pal_ = pal; }
  bool pal() const { return pal_; }

  // registradores (1F801810 / 1F801814)
  void gp0(uint32_t v);
  void gp1(uint32_t v);
  uint32_t readData();
  uint32_t status();

  // tempo: linhas e quadros
  void sync();                    // avança as linhas até sys.now
  uint64_t nextEvent() const;     // ciclo da próxima virada de linha
  uint64_t busyUntil() const { return busyUntil_; }  // fim do desenho em andamento
  int dotDivider() const;         // divisor do relógio de pontos (10/8/7/5/4)
  double videoPerCpu() const { return pal_ ? 53203425.0 / 33868800.0 : 53693175.0 / 33868800.0; }
  bool inVblank() const { return inVblank_; }

  // imagem do último quadro (0xAABBGGRR)
  const uint32_t* frame() const { return out_.data(); }
  int frameWidth() const { return outW_; }
  int frameHeight() const { return outH_; }
  void renderFrame();             // monta out_ a partir da VRAM (chamado no início do vblank)

  const uint16_t* vram() const { return vram_.data(); }
  void sync(StateIO& io);

  // resolução interna (1, 2 ou 4): a VRAM de 1024 x 512 continua sendo a de verdade (leituras da CPU, DMA, cópias,
  // texturas, estado salvo); com 2x ou 4x os polígonos também são desenhados numa cópia ampliada da VRAM, que dá
  // a imagem. A cópia não entra no estado salvo e não muda nada do que o programa vê (tempo, GPUSTAT, leituras).
  void setScale(int n);
  int scale() const { return 1 << sh_; }
  const uint16_t* hiVram() const { return sh_ ? hi_.data() : vram_.data(); }

private:
  System& sys_;
  std::vector<uint16_t> vram_;
  std::vector<uint32_t> out_;
  int outW_ = 320, outH_ = 240;
  bool pal_ = false;
  int sh_ = 0;                    // escala = 1 << sh_
  std::vector<uint16_t> hi_;      // VRAM ampliada ((1024 << sh_) x (512 << sh_)); vazia em 1x

  // GP0: fila do comando atual
  uint32_t fifo_[16] = {};
  int fifoLen_ = 0, need_ = 0;
  enum Mode : uint8_t { CMD, POLYLINE, TO_VRAM, FROM_VRAM };
  uint8_t mode_ = CMD;
  // transferências CPU <-> VRAM
  int tx_ = 0, ty_ = 0, tw_ = 0, th_ = 0, tcx_ = 0, tcy_ = 0;
  uint32_t readLatch_ = 0;

  // estado de desenho (E1..E6)
  uint32_t texPage_ = 0;      // bits 0..10 do E1 e o 11 (textura desligada)
  bool rectFlipX_ = false, rectFlipY_ = false;
  uint32_t texWindow_ = 0;
  int drawX1_ = 0, drawY1_ = 0, drawX2_ = 0, drawY2_ = 0;
  int offX_ = 0, offY_ = 0;
  bool setMask_ = false, checkMask_ = false;
  uint32_t areaTL_ = 0, areaBR_ = 0, offsetReg_ = 0;  // como escritos (GP1(10h))

  // GP1
  bool displayOff_ = true, irq_ = false;
  uint32_t dmaDir_ = 0;
  int dispX_ = 0, dispY_ = 0;
  int hStart_ = 0x200, hEnd_ = 0xC00, vStart_ = 0x10, vEnd_ = 0x100;
  uint32_t dispMode_ = 0;     // GP1(08h)
  uint32_t lastInfo_ = 0;

  // tempo de vídeo
  uint64_t lineStart_ = 0;    // ciclo (em 1/65536) do início da linha atual
  int line_ = 0;
  bool inVblank_ = false, field_ = false, frameVblankSeen_ = false;

  // polilinha
  uint32_t plColor_ = 0, plPrev_ = 0, plPrevC_ = 0;
  bool plShaded_ = false, plSemi_ = false, plWantColor_ = false;
  bool texDisableAllowed_ = false;
  uint64_t busyUntil_ = 0;          // desenho em andamento (ciclo em que termina)
  bool backlog_ = false;
  uint16_t clutCache_[256] = {};
  uint32_t clutKey_ = 0xFFFFFFFF;
  int clutCount_ = 0;
  uint32_t work_ = 0;               // custo do comando atual em 1/16 de ciclo

  uint64_t lineLen16() const;     // duração da linha em ciclos da CPU * 65536
  int totalLines() const { return pal_ ? 314 : 263; }
  bool interlaced480() const { return (dispMode_ & 0x24) == 0x24; }
  void command(uint32_t* p);
  int commandLength(uint32_t first) const;
  void endLine();

  // desenho
  struct Vtx { int x, y; uint32_t c; int u, v; };
  void drawTriangle(const Vtx* v, bool shaded, bool textured, bool raw, bool semi, uint16_t clut, uint32_t page);
  void drawRect(int x, int y, int w, int h, uint32_t c, bool textured, bool raw, bool semi, int u, int v, uint16_t clut);
  void drawLine(int x0, int y0, uint32_t c0, int x1, int y1, uint32_t c1, bool shaded, bool semi);
  void fill(uint32_t* p);
  void copyVram(uint32_t* p);
  uint16_t texel(int u, int v, uint16_t clut, uint32_t page) const;
  void loadClut(uint16_t clut, uint32_t page);
  void plot(int x, int y, uint16_t c, bool semi, uint32_t page);
  void plotHi(int x, int y, uint16_t c, bool semi, uint32_t page);     // ponto da VRAM ampliada
  void plotBlock(int x, int y, uint16_t c, bool semi, uint32_t page);  // o bloco ampliado de um ponto da VRAM
  void setBlock(int x, int y, uint16_t v);                             // idem, sem mistura nem máscara
  template <int SH> void rasterTriangle(const Vtx* v, bool shaded, bool textured, bool raw, bool semi, uint16_t clut, uint32_t page);
  void rebuildHi();
  void renderFrameHi(int w, int h, bool rgb24);
  bool skipLine(int y) const;
  void writeVramWord(uint16_t v);
};

}  // namespace indigo::ps1
