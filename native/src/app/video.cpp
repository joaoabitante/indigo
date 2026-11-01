#include "video.h"

#include <algorithm>
#include <cmath>

namespace indigo::video {

const char* filterName(int f) {
  switch (f) {
    case NITIDO: return "Nítido (pixels)";
    case SUAVE: return "Suave";
    case LCD: return "LCD (grade)";
    case TV: return "TV de tubo (linhas)";
    case EPX: return "EPX (contornos)";
    default: return "?";
  }
}

Screen::~Screen() { reset(); }

void Screen::reset() {
  if (tex_) SDL_DestroyTexture(tex_);
  if (over_) SDL_DestroyTexture(over_);
  tex_ = over_ = nullptr;
  texW_ = texH_ = 0;
  filter_ = overFilter_ = -1;
  overCell_ = 0;
}

SDL_Texture* Screen::make(SDL_Renderer* r, int w, int h, SDL_TextureAccess access) {
  SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, access, w, h);
  if (t) SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
  return t;
}

void Screen::update(SDL_Renderer* r, const uint32_t* fb, int w, int h, int filter) {
  const int tw = filter == EPX ? w * 2 : w, th = filter == EPX ? h * 2 : h;
  if (!tex_ || tw != texW_ || th != texH_) {
    if (tex_) SDL_DestroyTexture(tex_);
    tex_ = make(r, tw, th, SDL_TEXTUREACCESS_STREAMING);
    texW_ = tw; texH_ = th;
  }
  w_ = w; h_ = h;
  if (tex_ && filter != filter_) SDL_SetTextureScaleMode(tex_, filter == SUAVE ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
  filter_ = filter;
  if (!tex_) return;
  if (filter != EPX) { SDL_UpdateTexture(tex_, nullptr, fb, w * 4); return; }
  // EPX / Scale2x: cada pixel vira 4, arredondando as diagonais quando os vizinhos combinam
  epx_.resize(size_t(tw) * th);
  for (int y = 0; y < h; y++) {
    const uint32_t* row = fb + size_t(y) * w;
    const uint32_t* up = y > 0 ? row - w : row;
    const uint32_t* dn = y + 1 < h ? row + w : row;
    uint32_t* o0 = epx_.data() + size_t(2 * y) * tw;
    uint32_t* o1 = o0 + tw;
    for (int x = 0; x < w; x++) {
      const uint32_t p = row[x], a = up[x], d = dn[x];
      const uint32_t c = row[x > 0 ? x - 1 : x], b = row[x + 1 < w ? x + 1 : x];
      uint32_t e0 = p, e1 = p, e2 = p, e3 = p;
      if (c != b && a != d) {
        if (a == c) e0 = c;
        if (a == b) e1 = b;
        if (d == c) e2 = c;
        if (d == b) e3 = b;
      }
      o0[2 * x] = e0; o0[2 * x + 1] = e1; o1[2 * x] = e2; o1[2 * x + 1] = e3;
    }
  }
  SDL_UpdateTexture(tex_, nullptr, epx_.data(), tw * 4);
}

// célula da sobreposição: um pixel do console ampliado `cell` vezes, com a grade (LCD) ou a linha escura (TV)
SDL_Texture* Screen::overlay(SDL_Renderer* r, int cell, int filter) {
  if (over_ && cell == overCell_ && filter == overFilter_) return over_;
  if (over_) SDL_DestroyTexture(over_);
  over_ = make(r, cell, cell, SDL_TEXTUREACCESS_STATIC);
  overCell_ = cell; overFilter_ = filter;
  if (!over_) return nullptr;
  std::vector<uint32_t> px(size_t(cell) * cell, 0);
  const int line = std::max(1, cell / 5);
  for (int y = 0; y < cell; y++)
    for (int x = 0; x < cell; x++) {
      uint32_t a = 0;
      if (filter == LCD) {
        if (x >= cell - line || y >= cell - line) a = 0x70;       // vão entre os pixels da tela de cristal líquido
        else if (x == 0 || y == 0) a = 0x18;                      // borda levemente mais escura
      } else if (filter == TV) {
        const int dark = std::max(1, cell / 3);
        if (y >= cell - dark) a = 0x80;                           // linha de varredura
        else if (y == cell - dark - 1) a = 0x30;
      }
      px[size_t(y) * cell + x] = a << 24;  // preto com transparência (ABGR: alfa nos bits altos)
    }
  SDL_UpdateTexture(over_, nullptr, px.data(), cell * 4);
  SDL_SetTextureBlendMode(over_, SDL_BLENDMODE_BLEND);
  return over_;
}

void Screen::draw(SDL_Renderer* r, const SDL_FRect& dst) {
  if (!tex_) return;
  SDL_RenderTexture(r, tex_, nullptr, &dst);
  if ((filter_ == LCD || filter_ == TV) && w_ > 0) {
    // a grade só faz sentido quando cada pixel do console ocupa vários pixels da tela. Com escala inteira a
    // célula tem o tamanho exato; com escala quebrada (imagem preenchendo a tela) a célula mais próxima é
    // repetida na medida certa, alinhada com os pixels do jogo
    const float s = dst.w / static_cast<float>(w_);
    const int cell = static_cast<int>(std::lround(s));
    if (cell >= 3) {
      if (SDL_Texture* o = overlay(r, cell, filter_)) SDL_RenderTextureTiled(r, o, nullptr, s / cell, &dst);
    }
  }
}

}  // namespace indigo::video
