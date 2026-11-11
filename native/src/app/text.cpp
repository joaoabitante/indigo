#include "text.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"
#include "fonts.h"  // gerado pelo CMake: indigo_font0 (normal) e indigo_font1 (seminegrito)

namespace indigo::text {

namespace {

constexpr int FIRST = 32, COUNT = 224;  // espaço até ÿ (Latin-1)

struct Face {
  stbtt_fontinfo info{};
  const unsigned char* data = nullptr;
  float ascent = 0, descent = 0, gap = 0;  // em unidades da fonte
  bool ok = false;
};

struct Atlas {
  SDL_Texture* tex = nullptr;
  stbtt_packedchar chars[COUNT]{};
  float px = 0;
};

Face faces[2];
bool loaded = false;
SDL_Renderer* owner = nullptr;
std::map<std::pair<int, int>, std::unique_ptr<Atlas>> atlases;

void load() {
  if (loaded) return;
  loaded = true;
  const unsigned char* data[2] = {indigo_font0, indigo_font1};
  for (int k = 0; k < 2; k++) {
    Face& f = faces[k];
    f.data = data[k];
    if (!stbtt_InitFont(&f.info, f.data, stbtt_GetFontOffsetForIndex(f.data, 0))) continue;
    int a, d, g;
    stbtt_GetFontVMetrics(&f.info, &a, &d, &g);
    f.ascent = static_cast<float>(a); f.descent = static_cast<float>(d); f.gap = static_cast<float>(g);
    f.ok = true;
  }
}

// próximo código Latin-1 do texto UTF-8 (fora do Latin-1 vira "?")
int next(const std::string& s, size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i++]);
  if (c < 0x20 || c == 0x7F) return ' ';  // caractere de controle (título de ROM com lixo): não há desenho para ele
  if (c < 0x80) return c;
  int cp = '?', extra = 0;
  if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
  else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
  else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
  else return '?';
  for (int k = 0; k < extra && i < s.size(); k++) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
  if (cp == 0x2026) return 0x85;  // reticências (desenhadas como três pontos)
  if (cp == 0x2014 || cp == 0x2013) return '-';
  return (cp >= FIRST && cp < FIRST + COUNT) ? cp : '?';
}

// tamanho da letra em pixels inteiros: cada tamanho tem a sua textura, feita já nesse tamanho (nada é esticado)
int pixelSize(float px) { return std::max(6, static_cast<int>(std::lround(px))); }

Atlas* atlas(SDL_Renderer* r, float px, Weight w) {
  load();
  if (owner && owner != r) shutdown();
  owner = r;
  const int size = pixelSize(px);
  auto key = std::make_pair(static_cast<int>(w), size);
  auto it = atlases.find(key);
  if (it != atlases.end()) return it->second.get();
  // a escala da interface muda com a janela e com o monitor: tamanhos antigos se acumulam; passou do limite,
  // começa de novo (as texturas são refeitas na hora, só as usadas)
  if (atlases.size() >= 48) shutdown(), owner = r;
  auto a = std::make_unique<Atlas>();
  a->px = static_cast<float>(size);
  const Face& f = faces[w];
  if (f.ok) {
    const int side = size <= 28 ? 512 : size <= 56 ? 1024 : 2048;
    std::vector<unsigned char> bmp(size_t(side) * side);
    stbtt_pack_context pc;
    if (stbtt_PackBegin(&pc, bmp.data(), side, side, 0, 1, nullptr)) {
      // sem sobreamostragem: cada letra é desenhada em pixel inteiro (abaixo), sem esticar a textura. Com
      // posições quebradas o renderizador por software arredonda cada letra para um lado e o espaçamento fica torto
      stbtt_PackSetOversampling(&pc, 1, 1);
      stbtt_PackFontRange(&pc, f.data, 0, static_cast<float>(size), FIRST, COUNT, a->chars);
      stbtt_PackEnd(&pc);
      std::vector<uint32_t> rgba(bmp.size());
      for (size_t k = 0; k < bmp.size(); k++) rgba[k] = (uint32_t(bmp[k]) << 24) | 0x00FFFFFFu;
      a->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, side, side);
      if (a->tex) {
        SDL_UpdateTexture(a->tex, nullptr, rgba.data(), side * 4);
        SDL_SetTextureBlendMode(a->tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(a->tex, SDL_SCALEMODE_LINEAR);
      }
    }
  }
  Atlas* raw = a.get();
  atlases[key] = std::move(a);
  return raw;
}

float scaleFor(Weight w, float px) {
  const Face& f = faces[w];
  return f.ok ? px / (f.ascent - f.descent) : 0;
}

float advance(Weight w, float px, int cp) {
  const Face& f = faces[w];
  if (!f.ok) return px * 0.5f;
  int adv, lsb;
  stbtt_GetCodepointHMetrics(&f.info, cp == 0x85 ? '.' : cp, &adv, &lsb);
  return adv * scaleFor(w, px) * (cp == 0x85 ? 3 : 1);
}

}  // namespace

float lineHeight(float px) { return px * 1.25f; }

float width(const std::string& s, float px, Weight w) {
  load();
  px = static_cast<float>(pixelSize(px));  // a mesma medida que o desenho usa
  float x = 0;
  for (size_t i = 0; i < s.size();) x += advance(w, px, next(s, i));
  return x;
}

void draw(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c, Weight w) {
  Atlas* a = atlas(r, px, w);
  if (!a->tex) return;
  const Face& f = faces[w];
  // desenha no tamanho exato da textura (sem esticar); a linha continua centrada na altura pedida e a base
  // cai num pixel inteiro, para as letras não ficarem borradas entre duas linhas de pixels
  const float k = 1.0f;
  const float base = std::round(y + (lineHeight(px) - a->px) * 0.5f + f.ascent * scaleFor(w, a->px));
  px = a->px;
  SDL_SetTextureColorMod(a->tex, c.r, c.g, c.b);
  SDL_SetTextureAlphaMod(a->tex, c.a);
  float pen = x;
  for (size_t i = 0; i < s.size();) {
    int cp = next(s, i);
    const int reps = cp == 0x85 ? 3 : 1;
    if (cp == 0x85) cp = '.';
    for (int rep = 0; rep < reps; rep++) {
      const stbtt_packedchar& g = a->chars[cp - FIRST];
      const SDL_FRect src{static_cast<float>(g.x0), static_cast<float>(g.y0), static_cast<float>(g.x1 - g.x0), static_cast<float>(g.y1 - g.y0)};
      const SDL_FRect dst{std::round(pen + g.xoff * k), base + g.yoff * k, (g.xoff2 - g.xoff) * k, (g.yoff2 - g.yoff) * k};
      if (src.w > 0 && src.h > 0) SDL_RenderTexture(r, a->tex, &src, &dst);
      pen += g.xadvance * k;
    }
  }
}

std::string fit(const std::string& s, float px, float maxW, Weight w) {
  if (width(s, px, w) <= maxW) return s;
  const std::string dots = "\xE2\x80\xA6";
  std::string out;
  for (size_t i = 0; i < s.size();) {
    size_t j = i;
    next(s, j);
    const std::string cand = out + s.substr(i, j - i);
    if (width(cand + dots, px, w) > maxW) break;
    out = cand;
    i = j;
  }
  return out + dots;
}

void shutdown() {
  for (auto& [k, a] : atlases) if (a->tex) SDL_DestroyTexture(a->tex);
  atlases.clear();
  owner = nullptr;
}

}  // namespace indigo::text
