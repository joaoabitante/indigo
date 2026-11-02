// Super Game Boy dentro do núcleo do Game Boy (sem emular o SNES): pacotes de comando pelos bits 4 e 5 do
// registrador do joypad, paletas por área da tela (ATTR_*), máscara (MASK_EN), transferências pela imagem
// (PAL_TRN, ATTR_TRN, CHR_TRN, PCT_TRN), moldura de 256x224 e vários controles (MLT_REQ).
// Referência: Pan Docs (gbdev, CC0), capítulos "SGB". Sem os sons do SNES (SOUND/SOU_TRN são ignorados).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::gb {

// opção dos Ajustes (vale para os jogos abertos depois); padrão: ligado
void setSgbEnabled(bool on);
bool sgbEnabled();
// cabeçalho com as marcas de jogo de Super Game Boy (0x146 = 0x03 e licenciado antigo 0x33)
bool isSgbCart(const std::vector<uint8_t>& rom);

class SGB {
public:
  static constexpr int W = 256, H = 224, GX = 48, GY = 40;  // tela do Game Boy no meio da moldura

  SGB();
  void reset();

  // escrita em FF00 (bits 4-5): recebe os pacotes e troca o controle no modo de vários jogadores.
  // ly/lcdOn dizem em que ponto do quadro o comando chegou (para as transferências pela imagem).
  void writeJoyp(int v, int ly, bool lcdOn);
  // FF00 com P14 e P15 em 1 no modo de vários jogadores: número do controle (0xF = 1º, 0xE = 2º...)
  bool multi() const { return players > 1; }
  int player = 0;

  // fim de cada quadro: faz as transferências pendentes e monta a imagem 256x224
  void frameEnd(const uint8_t* shade, bool lcdOn);
  uint32_t out[W * H];

  void sync(StateIO& io);

  // para os testes
  uint16_t pal[4][4];       // paletas da tela (BGR555); a cor 0 vale para todas (pal[0][0])
  uint8_t attr[20 * 18];    // paleta de cada bloco 8x8
  int mask = 0, players = 1;
  int lastCmd = -1, cmdCount = 0;

private:
  void command();
  void setPalPair(int p0, int p1);
  void attrBlk();
  void attrLin();
  void attrDiv();
  void attrChr();
  void applyAtf(int n);
  void transfer(const uint8_t* shade);
  void drawBorder();
  uint32_t color(int c) const;

  // recepção dos pacotes
  uint8_t buf[7 * 16];
  int bit = 0, packet = 0;
  bool receiving = false, armed = false;
  int lastJoyp = 0x30;

  // transferência pela imagem: comando esperando o próximo quadro inteiro
  int trnCmd = -1, trnParam = 0, trnWait = 0;

  std::vector<uint8_t> sysPal, atf, chr, pct;
  bool borderDirty = true;
  uint32_t border[W * H];
  uint8_t borderOn[W * H];
  uint32_t gbLayer[160 * 144];
};

}  // namespace indigo::gb
