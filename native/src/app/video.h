// Filtros de tela do Índigo: desenham a imagem do console ampliada com um efeito.
// Feitos só com o SDL_Renderer (funcionam igual no PC e no celular): sobreposições repetidas por pixel (grade de
// LCD, linhas de TV) e ampliação EPX na CPU.
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace indigo::video {

enum Filter { NITIDO, SUAVE, LCD, TV, EPX, NUM_FILTERS };
const char* filterName(int f);

class Screen {
public:
  ~Screen();
  // imagem nova do console (pixels 0xAABBGGRR) e o filtro escolhido
  void update(SDL_Renderer* r, const uint32_t* fb, int w, int h, int filter);
  // desenha no retângulo de destino (já calculado pelo app)
  void draw(SDL_Renderer* r, const SDL_FRect& dst);
  void reset();

private:
  SDL_Texture* make(SDL_Renderer* r, int w, int h, SDL_TextureAccess access);
  SDL_Texture* overlay(SDL_Renderer* r, int cell, int filter);
  SDL_Texture* tex_ = nullptr;   // imagem do jogo (ou a versão EPX 2×)
  SDL_Texture* over_ = nullptr;  // célula da sobreposição (um pixel do console ampliado)
  int texW_ = 0, texH_ = 0, w_ = 0, h_ = 0, filter_ = -1, overCell_ = 0, overFilter_ = -1;
  std::vector<uint32_t> epx_;
};

}  // namespace indigo::video
