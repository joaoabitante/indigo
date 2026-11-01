// VDC HuC6270 do PC Engine: 64 KB de VRAM, fundo com rolagem (mapa de 32 a 128 x 32 ou 64 blocos), 64 sprites
// de 16x16 a 32x64 (16 por linha, colisão do sprite 0, estouro), interrupções de linha (RCR) e de vblank, DMA
// VRAM-VRAM e da tabela de sprites (SATB). O tempo segue o modelo do Mesen 2: fases horizontais HSW/HDS/HDW/HDE
// contadas em pontos do VDC (2, 3 ou 4 ciclos mestres, conforme o VCE) e fases verticais VSW/VDS/VDW/VCR, com a
// linha de 1365 ciclos e o vsync impostos pelo VCE. Os pixels saem como cores de 9 bits já passadas pela paleta
// (mais as marcas de "transparente" e "sprite" que o VPC do SuperGrafx usa).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::pce {

struct Vce {
  uint16_t pal[512] = {};
  uint16_t addr = 0;
  uint8_t ctrl = 0;
  uint8_t div = 4;        // ciclos mestres por pixel (4 = 5,37 MHz, 3 = 7,16 MHz, 2 = 10,7 MHz)
  uint16_t lines = 262;   // linhas por quadro (bit 2 do controle: 263)
  bool gray = false;
};

class Pce;

class Vdc {
public:
  static constexpr int LINE = 1365;
  static constexpr int ROW = 683;               // pixels por linha no modo mais fino (1365/2)
  static constexpr uint16_t SPR_FLAG = 0x8000, TRANSP_FLAG = 0x4000;

  uint16_t vram[0x8000] = {};
  uint16_t sat[256] = {};
  uint16_t row[ROW + 1] = {};  // linha sendo desenhada

  Vce* vce = nullptr;
  Pce* sys = nullptr;
  int index = 0;               // 0 = VDC1, 1 = VDC2 (SuperGrafx)

  void power();
  void runTo(int64_t t);       // avança até o relógio mestre t
  int64_t nextTime() const;    // próximo instante em que algo precisa acontecer
  bool irq() const { return st_.irq; }
  uint16_t scanline() const { return st_.scanline; }
  uint32_t frames() const { return st_.frameCount; }
  uint16_t hclock() const { return st_.hclock; }
  int64_t now() const { return st_.now; }
  // depois de carregar um estado: o relógio do VDC tem de andar junto com o da CPU
  void fixTime(int64_t clk) {
    if (st_.now > clk + LINE * 4 || st_.now < clk - LINE * 4) st_.now = clk;
    st_.lineStart = st_.now - st_.hclock;
    if (st_.hsyncStart > st_.now || st_.hsyncStart < st_.now - (int64_t(1) << 40)) st_.hsyncStart = st_.now - LINE;
  }
  void draw();                 // desenha a linha até o instante atual

  uint8_t read(int a);
  void write(int a, uint8_t v);
  void sync(StateIO& io);

private:
  struct Tile { uint16_t d0, d1, addr; uint8_t pal, pad; };
  struct Spr {
    uint16_t d[4];
    int16_t x;
    uint16_t addr;
    uint8_t idx, pal;
    uint8_t hflip, fg, sp23, pad;
  };
  struct Hv {
    uint16_t bxr, byr;
    uint8_t cols, rows, sprMode, vramMode, cg;
    uint8_t hsw, hds, hdw, hde, vsw, vds, pad;
    uint16_t vdw;
    uint8_t vcr, pad2;
  };
  enum : uint8_t { H_HDS, H_HDW, H_HDE, H_HSW };
  enum : uint8_t { V_VDS, V_VDW, V_VDE, V_VSW };
  enum : uint8_t { E_NONE, E_LATCH_Y, E_LATCH_X, E_HDS_IRQ, E_RCR };

  // tudo que muda com o tempo fica num bloco só (gravado inteiro no save state)
  struct State {
    int64_t now, lineStart, hsyncStart;
    uint32_t frameCount;
    uint16_t hclock, scanline, rcrCounter;
    uint8_t reg;
    uint16_t marW, marR, readBuf, vwr, incr, rcr;
    uint8_t ieColl, ieOver, ieRcr, ieVbl, bgOn, sprOn, nextBg, nextSpr, burst;
    uint8_t stVbl, stDma, stSatb, stRcr, stOver, stColl, irq;
    Hv reg_, lat;
    uint8_t byrPending;
    uint8_t dmaSatbIrq, dmaVramIrq, dmaDecSrc, dmaDecDst, dmaRepeat;
    uint16_t sour, desr, lenr, satb;
    uint8_t satbPending, satbRunning, satbOffset, vdmaRunning, vdmaRead, allowDma;
    int32_t satbCounter, vdmaPending;
    uint16_t vdmaBuf;
    uint8_t hMode, vMode, ev;
    int32_t hModeCounter, vModeCounter, evCounter;
    uint8_t needRcr, needVClock, needVblIrq, vblDone;
    uint16_t latchClkX, latchClkY;
    // sprites
    uint8_t sprCount, drawCount, totalCount, rowHasSpr0, hasOverflow, evalDone;
    uint16_t sprRow, evalStart, evalEnd, loadSprStart;
    // fundo
    uint16_t bgStart, bgEnd;
    uint8_t tileCount, pad3;
    uint16_t bgCycle;
    // desenho
    uint16_t xStart, lastDraw, screenX;
    // acesso da CPU à VRAM
    uint8_t pendRead, pendWrite, allowVram;
    int32_t transferDelay;
    uint16_t openBus;
    uint8_t collisionLine;
  } st_{};

  Spr spr_[64] = {};
  Spr drawSpr_[64] = {};
  Tile tiles_[100] = {};
  uint8_t xHasSpr_[1024 + 64] = {};

  int div() const { return vce->div; }
  int dots(int n) const { return n * vce->div; }
  uint16_t vramRead(uint16_t a) { if (a < 0x8000) return st_.openBus = vram[a]; return st_.openBus; }

  bool needsStepping() const;
  void chunk();
  void advance(int n, bool jump = false);  // jump: para quando algo passa a precisar de passos de 3 ciclos
  void transitionH();
  void event();
  void setH(uint8_t m);
  void setV(uint8_t m);
  void clockV();
  void incRcr();
  void incScrollY();
  void endOfLine();
  void hsyncStartLatch();
  void hdsIrqs();
  void vblank();
  void setIrq() { st_.irq = 1; }
  void evalSprites();
  void loadSprites();
  void loadTiles();
  void drawPixels();
  bool dmaAllowed() const;
  void satbStep();
  void vdmaStep();
  void vramAccess();
  void queueRead();
  void queueWrite();
  void waitVram();
  bool latchWindow(uint16_t clock) const;
};

}  // namespace indigo::pce
