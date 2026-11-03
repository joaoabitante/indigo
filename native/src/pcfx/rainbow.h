// RAINBOW (HuC6271), o descompressor de vídeo do PC-FX.
// O KING entrega ao chip, a cada 16 linhas, um bloco de dados comprimidos lido da KRAM; o RAINBOW descomprime uma
// faixa de 256x16 pontos num dos dois buffers enquanto o outro vai para a tela (troca a cada 16 linhas).
// Formatos de bloco (marcador 0xFF, tipo, tamanho de 16 bits em big endian, que conta os 2 bytes do tamanho):
//  - 0xF0..0xF3: paleta com repetição (RLE); cada byte traz o índice nos bits de cima e a contagem nos de baixo
//    (4, 3, 2 ou 1 bit de contagem); contagem 0 = o próximo byte + 1;
//  - 0xF8/0xFF: parecido com JPEG: 16 macroblocos de 16x16 (4 blocos Y + 1 U + 1 V, cor 4:2:0), Huffman com
//    tabelas fixas do chip, DC diferencial, zigue-zague, quantização e IDCT 8x8. O 0xFF traz antes as duas
//    tabelas de quantização (Y e UV, 64 bytes cada). No DC do Y há códigos especiais: 0x0F = "colunas vazias"
//    (pinta macroblocos com a cor dos registros de corrida nula) e 0x10..0x1F = nova escala de quantização.
// Dentro dos dados, um byte 0xFF é seguido de um byte de enchimento que é pulado.
// Algoritmo e tabelas de Huffman como documentados no Mednafen (rainbow.cpp, GPL 2+, com dados de David Michel,
// do MagicEngine-FX); a IDCT é própria (inteira, separável). Veja native/TERCEIROS.md.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::pcfx {

struct RbLut;  // tabela de consulta montada a partir de um RbHuff

// tabela de Huffman canônica: para cada tamanho de código n (2..12), códigos de min[n] a max[n] (0xFFFF = nenhum)
// valem codes[base[n] + (código - min[n])]
struct RbHuff {
  const uint8_t* base;
  const uint8_t* codes;
  const uint16_t* min;
  const uint16_t* max;
};
extern const RbHuff RB_DC_Y, RB_DC_UV, RB_AC_Y, RB_AC_UV;
extern const uint8_t RB_ZIGZAG[63];  // posição (0..63) do coeficiente AC k+1

// IDCT 8x8 no lugar (entrada: coeficientes já multiplicados pela quantização; saída: amostras sem o +128)
void rbIdct(int32_t* blk);

class Rainbow {
public:
  void reset();
  void write8(uint32_t a, uint8_t v);
  void write16(uint32_t a, uint16_t v);
  bool enabled() const { return control_ & 1; }

  // descomprime um bloco no buffer de trás; kram = página da KRAM, pos = endereço em bytes (19 bits), anda
  void decodeBlock(bool first, const uint16_t* kram, uint32_t& pos);
  void swapBuffers() { which_ ^= 1; readPos_ = 0; }
  // linha atual do buffer da frente (256 pontos, YUV de 24 bits + camada; 0 = transparente).
  // pal = paleta YUV (512) e palOff o deslocamento da camada; devolve o formato (-1 = nada, 0 = paleta, 1 = YUV)
  int fetchRaster(uint32_t* line, const uint32_t* pal, uint32_t palOff, uint32_t layerOr);

  void sync(StateIO& io);

  // para os testes
  int format(int b) const { return fmt_[b & 1]; }
  const uint32_t* yuv(int b) const { return buf_[b & 1]; }
  const uint8_t* idx(int b) const { return reinterpret_cast<const uint8_t*>(buf_[b & 1]); }
  int backBuffer() const { return which_ ^ 1; }
  uint8_t quant(int t, int i) const { return quant_[t & 1][i & 63]; }

private:
  uint16_t control_ = 0, nullY_ = 0, nullU_ = 0, nullV_ = 0, hsync_ = 0, hscroll_ = 0;
  uint8_t which_ = 0, readPos_ = 0;
  int8_t fmt_[2] = {-1, -1};
  bool firstDecode_ = true, garbage_ = false;
  uint8_t quant_[2][64] = {}, quantBase_[2][64] = {};
  uint32_t buf_[2][0x2000] = {};  // YUV (256x16) ou índices de paleta (bytes)

  // leitura de bits do bloco atual
  const uint16_t* kram_ = nullptr;
  uint32_t* pos_ = nullptr;
  uint32_t bits_ = 0, nbits_ = 0;
  int32_t left_ = 0;
  uint8_t fetch();
  uint8_t fetchData();
  uint32_t getBits(unsigned n, bool peek = false);
  int32_t getSigned(unsigned n);
  uint32_t nullColor() const;
  int32_t acCoeff(const RbLut& t, int32_t& zeroes);
  int32_t dcY(int32_t& zeroes);
  int32_t dcUV();
  void decodeBlock8(int32_t* dct, const uint8_t* q, int32_t dc, const RbLut& t);
};

}  // namespace indigo::pcfx
