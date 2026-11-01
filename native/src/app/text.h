// Texto liso da interface (fonte Poppins, licença OFL, embutida no programa) desenhado com o stb_truetype:
// cada tamanho de letra vira uma textura com os caracteres do Latin-1 (acentos do português incluídos).
#pragma once
#include <SDL3/SDL.h>

#include <string>

namespace indigo::text {

enum Weight { REGULAR, BOLD };

// largura do texto UTF-8 com letra de `px` pixels de altura
float width(const std::string& s, float px, Weight w = REGULAR);
// desenha com o canto superior esquerdo da caixa da linha em (x, y); a linha tem `lineHeight(px)` de altura
void draw(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c, Weight w = REGULAR);
float lineHeight(float px);
// corta com "…" para caber em maxW
std::string fit(const std::string& s, float px, float maxW, Weight w = REGULAR);
// libera as texturas (antes de destruir o renderer)
void shutdown();

}  // namespace indigo::text
