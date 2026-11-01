// Vídeo do Master System / Game Gear (porte de src/sms/vdp.js): modo 4, rolagem com travas, coluna
// esquerda apagável, sprites 8×8/8×16 com zoom, 8 por linha, colisão, interrupções de quadro e de linha.
// No Game Gear a paleta tem 12 bits e a tela é a janela 160×144.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::sms {

class VDP {
public:
  explicit VDP(bool gg);

  bool gg;
  int w, h;
  std::vector<uint8_t> vram, cram;
  uint8_t regs[16] = {};
  uint32_t fb[256 * 192];
  int vcount = 0, hcount = 0;

  void reset();
  bool irq() const { return ((status & 0x80) && (regs[1] & 0x20)) || (lineIrq && (regs[0] & 0x10)); }
  int readData();
  void writeData(int v);
  int readStatus();
  void writeControl(int v);
  int readV() const { return vcount <= 0xDA ? vcount : vcount - 6; }
  void lineStart(int line);
  void renderLine(int y);

  void sync(StateIO& io);

private:
  void color(int i);
  void store(int y);
  void bgLine(int y);
  void sprites(int y);
  void fillLine(uint32_t c) { for (auto& p : line) p = c; }

  uint32_t rgb[32];
  uint32_t line[256];
  uint8_t bgPri[256] = {}, sprCol[256] = {};
  int addr = 0, code = 0;
  bool latch = false;
  int first = 0, buffer = 0, ggLatch = 0;
  int status = 0;
  bool lineIrq = false;
  int lineCounter = 0xFF, vscrollLatch = 0;
};

}  // namespace indigo::sms
