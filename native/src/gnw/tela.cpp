#include "tela.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../third_party/nanosvg/nanosvg.h"
#include "../third_party/nanosvg/nanosvgrast.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace indigo::gnw {

namespace {

// tom do vidro do LCD (como a camada "multiply" do MAME): fundo cinza-claro, segmentos escuros
constexpr float TOM_R = 0.70f, TOM_G = 0.71f, TOM_B = 0.72f;

struct Imagem {
  NSVGimage* img = nullptr;
  ~Imagem() { if (img) nsvgDelete(img); }
};

NSVGimage* abrir(const std::vector<uint8_t>& svg) {
  std::vector<char> txt(svg.begin(), svg.end());
  txt.push_back(0);
  return nsvgParse(txt.data(), "px", 72);
}

// "x.y.z" -> deslocamento e bit; false se o título não for de segmento
bool lerTitulo(const char* t, bool sm5a, uint8_t& zx, uint8_t& y) {
  int a, b, c;
  char resto;
  if (std::sscanf(t, "%d.%d.%d%c", &a, &b, &c, &resto) != 3) return false;
  if (sm5a) {
    if (a < 0 || a > 15 || b < 0 || b > 3 || c < 0 || c > 1) return false;
    zx = static_cast<uint8_t>(a << 1 | c);
  } else {
    if (a < 0 || a > 3 || b < 0 || b > 15 || c < 0 || c > 3) return false;
    zx = static_cast<uint8_t>(a << 2 | c);
  }
  y = static_cast<uint8_t>(b);
  return true;
}

uint32_t tingir(const uint8_t* p) {
  const int r = static_cast<int>(p[0] * TOM_R + 0.5f), g = static_cast<int>(p[1] * TOM_G + 0.5f),
            b = static_cast<int>(p[2] * TOM_B + 0.5f);
  return static_cast<uint32_t>(r | g << 8 | b << 16) | static_cast<uint32_t>(p[3]) << 24;
}

inline uint32_t misturar(uint32_t dst, uint32_t src) {
  const uint32_t a = src >> 24;
  if (a == 0) return dst;
  if (a == 255) return src | 0xFF000000u;
  const uint32_t ia = 255 - a;
  const uint32_t r = ((src & 0xFF) * a + (dst & 0xFF) * ia) / 255;
  const uint32_t g = ((src >> 8 & 0xFF) * a + (dst >> 8 & 0xFF) * ia) / 255;
  const uint32_t b = ((src >> 16 & 0xFF) * a + (dst >> 16 & 0xFF) * ia) / 255;
  return r | g << 8 | b << 16 | 0xFF000000u;
}

}  // namespace

bool medirSvg(const std::vector<uint8_t>& svg, float& w, float& h, std::string& erro) {
  Imagem im;
  im.img = abrir(svg);
  if (!im.img || !(im.img->width > 1 && im.img->height > 1) || im.img->width > 100000 || im.img->height > 100000) {
    erro = "desenho da tela (.svg) inválido";
    return false;
  }
  w = im.img->width;
  h = im.img->height;
  return true;
}

void rasterizarSvg(const std::vector<uint8_t>& svg, float escala, int ox, int oy, int fbW, int fbH, bool sm5a,
                   std::vector<uint32_t>& fundo, std::vector<Segmento>& segs) {
  Imagem im;
  im.img = abrir(svg);
  if (!im.img) return;
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  if (!rast) return;
  const int rw = std::clamp(static_cast<int>(std::ceil(im.img->width * escala)), 1, fbW - ox);
  const int rh = std::clamp(static_cast<int>(std::ceil(im.img->height * escala)), 1, fbH - oy);

  // formas por título
  std::map<std::string, std::vector<NSVGshape*>> grupos;
  std::vector<NSVGshape*> semTitulo;
  std::vector<uint8_t> visivel;
  for (NSVGshape* s = im.img->shapes; s; s = s->next) {
    visivel.push_back(s->flags & NSVG_FLAGS_VISIBLE);
    if (s->title[0]) grupos[std::string(s->title, strnlen(s->title, sizeof s->title))].push_back(s);
    else semTitulo.push_back(s);
  }

  // fundo: só as formas sem título (misturado depois que se sabe a cor dos segmentos)
  std::vector<uint8_t> buf(static_cast<size_t>(rw) * rh * 4);
  {
    size_t k = 0;
    for (NSVGshape* s = im.img->shapes; s; s = s->next, k++)
      s->flags = static_cast<char>(s->title[0] ? 0 : visivel[k]);
    nsvgRasterize(rast, im.img, 0, 0, escala, buf.data(), rw, rh, rw * 4);
  }
  uint64_t somaLuz = 0, nLuz = 0;  // brilho dos segmentos

  // cada segmento sozinho, num retângulo do tamanho dele
  for (auto& [titulo, formas] : grupos) {
    Segmento seg;
    if (!lerTitulo(titulo.c_str(), sm5a, seg.zx, seg.y)) continue;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (NSVGshape* s = im.img->shapes; s; s = s->next) s->flags = 0;
    size_t k = 0;
    for (NSVGshape* s = im.img->shapes; s; s = s->next, k++) {
      if (std::find(formas.begin(), formas.end(), s) == formas.end()) continue;
      s->flags = static_cast<char>(visivel[k]);
      const float sw = s->strokeWidth * 0.5f + 1;
      x0 = std::min(x0, s->bounds[0] - sw); y0 = std::min(y0, s->bounds[1] - sw);
      x1 = std::max(x1, s->bounds[2] + sw); y1 = std::max(y1, s->bounds[3] + sw);
    }
    if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1))) continue;
    x0 = std::clamp(x0 * escala, -1.0f, static_cast<float>(rw)); x1 = std::clamp(x1 * escala, -1.0f, static_cast<float>(rw));
    y0 = std::clamp(y0 * escala, -1.0f, static_cast<float>(rh)); y1 = std::clamp(y1 * escala, -1.0f, static_cast<float>(rh));
    const int bx0 = std::max(0, static_cast<int>(std::floor(x0)) - 1);
    const int by0 = std::max(0, static_cast<int>(std::floor(y0)) - 1);
    const int bx1 = std::min(rw, static_cast<int>(std::ceil(x1)) + 1);
    const int by1 = std::min(rh, static_cast<int>(std::ceil(y1)) + 1);
    if (bx1 <= bx0 || by1 <= by0) continue;
    seg.x0 = ox + bx0; seg.y0 = oy + by0; seg.w = bx1 - bx0; seg.h = by1 - by0;
    std::vector<uint8_t> sb(static_cast<size_t>(seg.w) * seg.h * 4);
    nsvgRasterize(rast, im.img, static_cast<float>(-bx0), static_cast<float>(-by0), escala, sb.data(), seg.w, seg.h, seg.w * 4);
    seg.rgba.resize(static_cast<size_t>(seg.w) * seg.h);
    bool algum = false;
    for (size_t i = 0; i < seg.rgba.size(); i++) {
      seg.rgba[i] = tingir(&sb[i * 4]);
      algum |= sb[i * 4 + 3] != 0;
      if (sb[i * 4 + 3] > 128) { somaLuz += sb[i * 4] + sb[i * 4 + 1] + sb[i * 4 + 2]; nLuz += 3; }
    }
    if (algum) segs.push_back(std::move(seg));
  }
  nsvgDeleteRasterizer(rast);

  // desenho sem fundo próprio: o MAME desenha sobre preto; com segmentos escuros isso some, então o vidro do LCD
  // fica claro (cinza do tom); com segmentos claros fica preto, como no MAME
  size_t opacos = 0;
  for (size_t i = 0; i < buf.size(); i += 4) opacos += buf[i + 3] > 128;
  const bool semFundo = opacos < static_cast<size_t>(rw) * rh / 2;
  const bool segEscuros = nLuz == 0 || somaLuz / nLuz < 128;
  const uint8_t branco[4] = {255, 255, 255, 255};
  const uint32_t vidro = tingir(branco);
  for (int y = 0; y < rh; y++)
    for (int x = 0; x < rw; x++) {
      uint32_t& d = fundo[static_cast<size_t>(oy + y) * fbW + ox + x];
      if (semFundo && segEscuros) d = vidro;
      d = misturar(d, tingir(&buf[(static_cast<size_t>(y) * rw + x) * 4]));
    }
}

void desenharSegmento(const Segmento& s, uint32_t* fb, int fbW) {
  const uint32_t* p = s.rgba.data();
  for (int y = 0; y < s.h; y++) {
    uint32_t* d = fb + static_cast<size_t>(s.y0 + y) * fbW + s.x0;
    for (int x = 0; x < s.w; x++, p++) d[x] = misturar(d[x], *p);
  }
}

}  // namespace indigo::gnw
