// VDP do MSX: TMS9918A (MSX1, 16 KB de VRAM) e V9938 (MSX2, 128 KB), desenhados linha a linha.
//   Modos do TMS9918A: Gráfico 1 e 2 (SCREEN 1/2), Multicor (3), Texto 1 (0, 40 colunas); sprites modo 1
//   (4 por linha, 5º sprite e colisão).
//   Modos novos do V9938: Texto 2 (80 colunas, piscar), Gráfico 3 (SCREEN 4), bitmaps G4–G7 (SCREEN 5–8,
//   G6/G7 com a VRAM entrelaçada), 192/212 linhas, rolagem vertical (R23), interrupção de linha (R19),
//   paleta de 512 cores, sprites modo 2 (8 por linha, cor por linha, CC/IC/EC) e o processador de comandos
//   (HMMC, YMMM, HMMM, HMMV, LMMC, LMCM, LMMM, LMMV, LINE, SRCH, PSET, POINT com as 10 operações lógicas).
// A tela sai com 256 pontos de largura (512 quando algum quadro usa os modos de alta resolução: aí cada
// linha também é dobrada, para manter a proporção) e 192 ou 212 linhas, sem a borda.
// Simplificações: os comandos terminam na hora (o bit CE cai logo, menos nas transferências pela CPU);
// sem as esperas de acesso à VRAM; sem entrelaçado nem troca de páginas por R13; R18 (ajuste) ignorado.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::msx {

class VDP {
public:
  explicit VDP(bool v9938);
  const bool v9938;

  void reset();
  // portas 98h–9Bh (cycle = ciclo da CPU dentro da linha atual, para o bit HR)
  int readData();
  void writeData(int v);
  int readStatus(int cycle);
  void writeControl(int v);
  void writePalette(int v);
  void writeIndirect(int v);
  bool irq() const;

  // linha 0 = primeira linha visível; 262 linhas por quadro (NTSC)
  void startLine(int line);
  void hblank(int line);  // fim da parte visível da linha: interrupção de linha (V9938)
  void endFrame();        // monta a imagem final em fb
  int activeLines() const { return (v9938 && (reg[9] & 0x80)) ? 212 : 192; }

  static constexpr int HBLANK_CYCLE = 172;  // ciclo da CPU em que começa o apagamento horizontal
  uint32_t fb[512 * 424] = {};
  int outW = 256, outH = 192;

  void sync(StateIO& io);

  // para testes
  const std::vector<uint8_t>& vramData() const { return vram; }
  int regValue(int r) const { return reg[r & 63]; }

private:
  // modo de tela: bits M5 M4 M3 M2 M1
  int mode() const;
  bool bitmap() const;
  int planarAddr(int a) const { return ((a >> 1) | ((a & 1) << 16)) & vramMask; }
  int cpuAddr() const;
  void incAddr();
  void setReg(int r, int v);
  void updatePalette(int i);

  void renderLine(int line);
  void renderText(int y, uint8_t* pix, bool text2);
  void renderG1(int y, uint8_t* pix);
  void renderG2(int y, uint8_t* pix);
  void renderMC(int y, uint8_t* pix);
  void spritesMode1(int y, uint8_t* pix, int line);
  void spritesMode2(int y, int line);

  // processador de comandos (vdpcmd.cpp)
  struct CmdGeo { int width, ppb, bpp, lines; int kind; };  // kind: 0=G4 1=G5 2=G6 3=G7 4=outros
  CmdGeo geo() const;
  int pixAddr(const CmdGeo& g, int x, int y) const;
  int point(const CmdGeo& g, int x, int y) const;
  void pset(const CmdGeo& g, int x, int y, int color, int lop);
  void startCommand();
  void cmdTransfer(int v);  // gravação em R44 durante HMMC/LMMC
  int cmdReadS7();          // leitura de S7 durante LMCM
  void cmdAdvance();        // próximo ponto/byte das transferências
  void cmdFinish();
  int getReg16(int r, int mask) const { return (reg[r] | (reg[r + 1] << 8)) & mask; }
  void setReg16(int r, int v) { reg[r] = v & 0xFF; reg[r + 1] = (v >> 8) & 0xFF; }

  std::vector<uint8_t> vram;
  int vramMask;
  uint8_t reg[64] = {};
  uint8_t stat[10] = {};
  uint16_t pal[16] = {};    // 0RRR0BBB 00000GGG -> (r<<6)|(g<<3)|b
  uint32_t palRGB[16] = {};
  int ptr = 0;               // ponteiro da VRAM (14 bits; o resto vem de R14)
  int latch = 0;
  bool latchFull = false;
  int readAhead = 0;
  int palLatch = 0;
  bool palLatchFull = false;
  bool hrIrq = false;        // FH: interrupção de linha pendente
  int blinkCount = 0;
  bool blinkOn = false;
  int frameLines = 192;
  int scanLine = 0;

  // comando em andamento (transferências pela CPU)
  int cmd = 0, cmdLop = 0;
  bool ce = false, tr = false, bd = false;
  int cx = 0, cy = 0, cnx = 0, cny = 0, cxStart = 0, cdix = 1, cdiy = 1, cleft = 0;

  // linhas desenhadas no quadro
  struct Line { uint32_t px[512]; bool hi; };
  std::vector<Line> lines;
  uint8_t sprCol[256] = {};
  uint8_t sprGroup[256] = {};
  uint16_t idx[512] = {};
};

}  // namespace indigo::msx
