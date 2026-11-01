// Vídeo do Neo Geo: LSPC2 (+ NEO-B1). Sprites de 16 pontos de largura e até 32 tiles de altura (381 sprites,
// no máximo 96 por linha), encadeamento ("sticky": o sprite seguinte continua à direita do anterior e herda a
// posição Y, a altura e a redução vertical), redução horizontal (16 padrões de pontos pulados) e vertical (tabela
// da ROM L0 da BIOS, 256 níveis), espelhamento, auto-animação (contador de 3 bits trocando os 2 ou 3 bits de baixo
// do número do tile), camada fixa de 40x32 tiles de 8x8 (S ROM do cartucho ou SFIX da BIOS, com a troca de bancos
// por linha ou por tile dos cartuchos que usam a S ROM de 512 KB), paleta de 2 bancos x 256 paletas x 16 cores
// (5 bits por componente + bit "escuro" comum) e o modo sombra (REG_SHADOW).
//
// Tempo (como no Geolith, que serve de régua): linha = 384 pontos = 768 ciclos do 68000; 264 linhas (8 de
// apagamento vertical, 16 de borda, 224 visíveis, 16 de borda). Em cada linha, no ciclo 29: auto-animação (linha
// 8) e interrupção de quadro (linha 249); no ciclo 573 a linha é desenhada (os sprites são calculados 2 linhas
// antes, num par de memórias de linha, como no aparelho); no ciclo 712 o contador de linhas avança.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::neogeo {

class LSPC {
public:
  static constexpr int OUT_W = 304, OUT_H = 224;  // 320x224 sem as 8 colunas de borda de cada lado
  static constexpr int LINES = 264, CYC_PER_LINE = 768;
  enum FixBank { FIX_NONE = 0, FIX_LINE = 1, FIX_TILE = 2 };

  LSPC();
  void reset();  // registradores e posição na tela (a memória fica)

  // memórias de gráficos (não pertencem ao estado; o núcleo é dono dos vetores)
  void setCrom(const uint8_t* c, size_t size);
  void setFix(const uint8_t* s, size_t size, int bankType);
  void setL0(const uint8_t* l0) { l0_ = l0; }
  // Neo Geo CD: a RAM de sprites guarda os 4 planos de cada linha na ordem 2, 1, 4, 3 (formato .SPR), e não na
  // ordem das C ROMs intercaladas (1, 3, 2, 4)
  void setCdSpriteLayout(bool cd);
  // Neo Geo CD: chaves de vídeo do CD ($FF0111, $FF0115, $FF0119). Ficam fora do estado da LSPC (o núcleo do CD
  // guarda); no cartucho continuam sempre ligadas.
  bool sprOn = true, fixOn = true, videoOn = true;

  // portas do 68000
  void writeAddr(uint16_t v) { vramAddr_ = v; }
  void writeData(uint16_t v);
  uint16_t readData() const { return vram[vramIndex(vramAddr_)]; }
  void writeMod(uint16_t v) { vramMod_ = static_cast<int16_t>(v); }
  uint16_t readMod() const { return static_cast<uint16_t>(vramMod_); }
  uint16_t readMode() const { return static_cast<uint16_t>(((scanline + 0xF8) << 7) | aaCounter_); }
  void writeMode(uint16_t v);  // velocidade/desligamento da auto-animação; o resto (temporizador) é do núcleo
  uint16_t palRead(uint32_t a) const { return pal[palBank_ * 4096 + ((a >> 1) & 0xFFF)]; }
  void palWrite(uint32_t a, uint16_t v);
  void setPalBank(int b) { palBank_ = b & 1; }
  void setShadow(bool s) { shadow_ = s; }

  // avança n ciclos do 68000; devolve os eventos que aconteceram (bits EV_*)
  enum { EV_VBLANK = 1, EV_RELOAD_TIMER = 2 };
  int run(int cycles);

  void sync(StateIO& io);
  void vramChanged() { sprDirty_ = true; }  // a VRAM foi mexida por fora das portas (reset)

  uint16_t vram[0x8800] = {};  // 32K palavras (SCB1 e mapa fixo) + 2K palavras (SCB2-4 e listas)
  uint16_t pal[0x2000] = {};   // 2 bancos de 4096 cores
  uint32_t fb[OUT_W * OUT_H] = {};
  int scanline = 0, cyc = 0;

private:
  static int vramIndex(uint16_t a) { return (a & 0x8000) ? 0x8000 | (a & 0x7FF) : a; }
  void convert(int i);
  void drawLine();
  void spriteLine(int line);
  void fixLine(int line, uint32_t* out);

  const uint8_t* crom_ = nullptr;
  size_t cromSize_ = 0;
  uint32_t tileMask_ = 0;
  const uint8_t* fix_ = nullptr;
  uint32_t fixMask_ = 0;
  int fixBank_ = FIX_NONE;
  const uint8_t* l0_ = nullptr;
  uint8_t plane_[4] = {0, 2, 1, 3};  // byte da linha do tile com os planos 1, 2, 3 e 4

  int palBank_ = 0;
  bool shadow_ = false;
  uint16_t vramAddr_ = 0;
  int16_t vramMod_ = 0;
  uint8_t aaCounter_ = 0, aaReload_ = 0, aaTimer_ = 0;
  bool aaDisable_ = false;
  uint16_t lineBuf_[2][320] = {};  // índice da cor (0 = transparente)
  int lbActive_ = 0;
  // posição/tamanho de cada sprite já com o encadeamento resolvido; refeito só quando a SCB2-4 muda
  struct SprInfo { uint16_t x, y, size, vs, hs; };
  SprInfo spr_[382] = {};
  bool sprDirty_ = true;
  void buildSprites();

  uint32_t rgb_[0x2000] = {}, rgbShadow_[0x2000] = {};  // paleta convertida (refeita ao carregar o estado)
};

// tabela da redução vertical (000-lo.lo), igual à da placa: usada quando a BIOS não traz a sua e pelo Neo Geo CD
std::vector<uint8_t> makeL0();

}  // namespace indigo::neogeo
