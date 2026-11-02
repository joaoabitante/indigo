// Tela de LCD de um Game & Watch a partir do desenho .svg do pacote do MAME: cada segmento é um grupo com
// <title>x.y.z</title> (SM51x: x = grupo a/b/bs/c, y = segmento 1-16, z = comum H1-H4; SM5A: x = grupo O,
// y = segmento 1-4, z = H1/H2). O resto do desenho (sem título) é o fundo. O .svg é lido e rasterizado uma vez
// com o nanosvg (licença zlib); depois cada quadro só copia o fundo e mistura os segmentos acesos.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace indigo::gnw {

struct Segmento {
  uint8_t zx = 0, y = 0;  // posição na saída da CPU (deslocamento e bit)
  int x0 = 0, y0 = 0, w = 0, h = 0;
  std::vector<uint32_t> rgba;  // cores já com o tom do LCD; alfa no byte de cima
};

// medidas do desenho (em unidades do .svg); false com a mensagem em `erro` se o arquivo não servir
bool medirSvg(const std::vector<uint8_t>& svg, float& w, float& h, std::string& erro);

// rasteriza o desenho na escala pedida, na posição (ox, oy) de uma imagem fbW x fbH: o fundo vai para `fundo`
// (misturado ao que já estiver lá) e cada segmento com título "x.y.z" válido vira um item de `segs`.
// sm5a: títulos no formato do SM5A (x até 15, y até 3, z até 1).
void rasterizarSvg(const std::vector<uint8_t>& svg, float escala, int ox, int oy, int fbW, int fbH, bool sm5a,
                   std::vector<uint32_t>& fundo, std::vector<Segmento>& segs);

// mistura um segmento sobre a imagem
void desenharSegmento(const Segmento& s, uint32_t* fb, int fbW);

}  // namespace indigo::gnw
