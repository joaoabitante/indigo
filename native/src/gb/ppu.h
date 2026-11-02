// Vídeo do Game Boy / Game Boy Color (porte de src/gb/ppu.js): máquina de modos por ponto,
// interrupções de STAT com o bloqueio do hardware e desenho da linha no fim do modo 3.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::gb {

class GB;

class PPU {
public:
  explicit PPU(GB& gb);

  uint32_t fb[160 * 144];
  uint8_t shade[160 * 144];  // tom de cinza de cada ponto no modo DMG (0-3, depois do BGP/OBP): o que o Super Game Boy recebe
  std::vector<uint8_t> vram, oam;
  uint8_t bgPal[64], objPal[64];
  uint32_t bgRGB[32], objRGB[32];
  // paleta DMG: 4 cores para o fundo e 4 para cada paleta de sprite (trocadas pelo front-end)
  uint32_t dmgBg[4], dmgObj0[4], dmgObj1[4];

  int lcdc, stat, scy, scx, ly, lyc, bgp, obp0, obp1, wy, wx;
  int vbk, bcps, ocps, opri;
  int mode, dot, nextAt, stage, line;
  bool statLine, wyHit, hblankIrq, frameDone, skipFrame;
  int winLine;

  void reset();
  bool oamLocked() const { return (lcdc & 0x80) != 0 && mode >= 2; }
  bool vramLocked() const { return (lcdc & 0x80) != 0 && mode == 3; }
  bool lcdOn() const { return (lcdc & 0x80) != 0; }
  static uint32_t rgb(int c);

  void tick(int n);
  int readStat() const;
  void writeStat(int v);
  void writeLyc(int v);
  void writeLcdc(int v);
  void writeBcpd(int v);
  void writeOcpd(int v);
  void updateStat();

  void sync(StateIO& io);

private:
  void step();
  int mode3Length();
  void setMode(int m) { mode = m; updateStat(); }
  void renderLine(int ly);
  void drawTiles(int row, int x0, int x1, int mapBase, int y, int sx);
  void drawSprites(int ly, int row);

  GB& gb;
  uint8_t lineIdx[168], linePri[168];
  int16_t objX[10];
  uint8_t objN[10];
};

}  // namespace indigo::gb
