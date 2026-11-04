#include "ui.h"

#include <algorithm>
#include <cmath>
#include <iterator>

#include "core/core.h"
#include "text.h"

namespace indigo::ui {

// ------------------------------------------------------------ texto e formas
std::string toAscii(const std::string& s) { return s; }

float textWidth(const std::string& t, float px, bool bold) { return text::width(t, px, bold ? text::BOLD : text::REGULAR); }

void drawText(SDL_Renderer* r, float x, float y, const std::string& t, float px, SDL_Color c, bool bold) {
  text::draw(r, t, x, y, px, c, bold ? text::BOLD : text::REGULAR);
}

namespace {
SDL_FColor fcol(SDL_Color c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}; }

// contorno de um retângulo arredondado (sentido horário, começando no canto superior esquerdo)
void roundPath(const SDL_FRect& rc, float rad, std::vector<SDL_FPoint>& pts) {
  rad = std::max(0.0f, std::min({rad, rc.w / 2, rc.h / 2}));
  const int n = std::clamp(static_cast<int>(rad / 2), 3, 14);
  pts.clear();
  const float cx[4] = {rc.x + rad, rc.x + rc.w - rad, rc.x + rc.w - rad, rc.x + rad};
  const float cy[4] = {rc.y + rad, rc.y + rad, rc.y + rc.h - rad, rc.y + rc.h - rad};
  for (int k = 0; k < 4; k++) {
    const float a0 = 3.14159265f + k * 1.57079633f;
    for (int j = 0; j <= n; j++) {
      const float a = a0 + j * 1.57079633f / n;
      pts.push_back({cx[k] + rad * std::cos(a), cy[k] + rad * std::sin(a)});
    }
  }
}

void fillPolygonFan(SDL_Renderer* r, const std::vector<SDL_FPoint>& pts, SDL_FPoint center, SDL_Color c) {
  const SDL_FColor fc = fcol(c);
  std::vector<SDL_Vertex> v;
  std::vector<int> idx;
  v.push_back({center, fc, {0, 0}});
  // pontos repetidos (emenda de dois cantos numa pílula) dariam triângulos sem área, que o renderizador por
  // software desenha como um risco do centro até a borda
  for (const auto& p : pts) {
    const SDL_FPoint& q = v.back().position;
    if (v.size() > 1 && std::fabs(p.x - q.x) < 0.01f && std::fabs(p.y - q.y) < 0.01f) continue;
    v.push_back({p, fc, {0, 0}});
  }
  if (v.size() > 2) {
    const SDL_FPoint& a = v[1].position;
    const SDL_FPoint& b = v.back().position;
    if (std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f) v.pop_back();
  }
  const int n = static_cast<int>(v.size()) - 1;
  for (int k = 0; k < n; k++) { idx.push_back(0); idx.push_back(k + 1); idx.push_back((k + 1) % n + 1); }
  SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

void strip(SDL_Renderer* r, const std::vector<SDL_FPoint>& outer, const std::vector<SDL_FPoint>& inner, SDL_Color c) {
  const SDL_FColor fc = fcol(c);
  const int n = static_cast<int>(std::min(outer.size(), inner.size()));
  std::vector<SDL_Vertex> v;
  std::vector<int> idx;
  for (int k = 0; k < n; k++) { v.push_back({outer[k], fc, {0, 0}}); v.push_back({inner[k], fc, {0, 0}}); }
  for (int k = 0; k < n; k++) {
    const int a = k * 2, b = ((k + 1) % n) * 2;
    idx.insert(idx.end(), {a, a + 1, b, b, a + 1, b + 1});
  }
  SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), idx.data(), static_cast<int>(idx.size()));
}

void fillTriangle(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_Color col) {
  const SDL_FColor fc = fcol(col);
  const SDL_Vertex v[3] = {{a, fc, {0, 0}}, {b, fc, {0, 0}}, {c, fc, {0, 0}}};
  SDL_RenderGeometry(r, nullptr, v, 3, nullptr, 0);
}

void fillStar(SDL_Renderer* r, float cx, float cy, float rad, SDL_Color c) {
  std::vector<SDL_FPoint> pts;
  for (int k = 0; k < 10; k++) {
    const float a = -1.5707963f + k * 0.62831853f, rr = (k & 1) ? rad * 0.45f : rad;
    pts.push_back({cx + rr * std::cos(a), cy + rr * std::sin(a)});
  }
  fillPolygonFan(r, pts, {cx, cy}, c);
}

SDL_Color alpha(SDL_Color c, uint8_t a) { c.a = a; return c; }

bool inside(const SDL_FRect& rc, float x, float y) { return x >= rc.x && x < rc.x + rc.w && y >= rc.y && y < rc.y + rc.h; }

// imagem centralizada dentro da caixa, sem deformar
void drawFit(SDL_Renderer* r, SDL_Texture* t, const SDL_FRect& box) {
  float tw = 0, th = 0;
  SDL_GetTextureSize(t, &tw, &th);
  if (tw <= 0 || th <= 0) return;
  const float s = std::min(box.w / tw, box.h / th);
  const SDL_FRect d{box.x + (box.w - tw * s) / 2, box.y + (box.h - th * s) / 2, tw * s, th * s};
  SDL_RenderTexture(r, t, nullptr, &d);
}
}  // namespace

void fillCircle(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color c) {
  std::vector<SDL_FPoint> pts;
  const int n = std::clamp(static_cast<int>(radius), 16, 64);
  for (int k = 0; k < n; k++) {
    const float a = k * 6.2831853f / n;
    pts.push_back({cx + radius * std::cos(a), cy + radius * std::sin(a)});
  }
  fillPolygonFan(r, pts, {cx, cy}, c);
}

void strokeCircle(SDL_Renderer* r, float cx, float cy, float radius, float width, SDL_Color c) {
  std::vector<SDL_FPoint> o, i;
  const int n = std::clamp(static_cast<int>(radius), 16, 64);
  for (int k = 0; k < n; k++) {
    const float a = k * 6.2831853f / n;
    o.push_back({cx + radius * std::cos(a), cy + radius * std::sin(a)});
    i.push_back({cx + (radius - width) * std::cos(a), cy + (radius - width) * std::sin(a)});
  }
  strip(r, o, i, c);
}

void fillRoundRect(SDL_Renderer* r, const SDL_FRect& rc, float radius, SDL_Color c) {
  if (rc.w <= 0 || rc.h <= 0) return;
  std::vector<SDL_FPoint> pts;
  roundPath(rc, radius, pts);
  fillPolygonFan(r, pts, {rc.x + rc.w / 2, rc.y + rc.h / 2}, c);
}

void strokeRoundRect(SDL_Renderer* r, const SDL_FRect& rc, float radius, float width, SDL_Color c) {
  std::vector<SDL_FPoint> o, i;
  roundPath(rc, radius, o);
  const SDL_FRect in{rc.x + width, rc.y + width, rc.w - 2 * width, rc.h - 2 * width};
  // mesmo número de pontos dos dois lados: o raio de dentro segue o de fora
  const float rad = std::max(0.0f, std::min({radius, rc.w / 2, rc.h / 2}));
  std::vector<SDL_FPoint> tmp;
  roundPath(rc, rad, tmp);
  const int n = static_cast<int>(tmp.size()) / 4 - 1;
  const float ri = std::max(0.0f, rad - width);
  const float cx[4] = {in.x + ri, in.x + in.w - ri, in.x + in.w - ri, in.x + ri};
  const float cy[4] = {in.y + ri, in.y + ri, in.y + in.h - ri, in.y + in.h - ri};
  for (int k = 0; k < 4; k++) {
    const float a0 = 3.14159265f + k * 1.57079633f;
    for (int j = 0; j <= n; j++) {
      const float a = a0 + j * 1.57079633f / n;
      i.push_back({cx[k] + ri * std::cos(a), cy[k] + ri * std::sin(a)});
    }
  }
  strip(r, o, i, c);
}

SDL_Color consoleColor(const std::string& c) {
  if (c == "SNES") return {0x7E, 0x5B, 0xD6, 255};
  if (c == "NES") return {0xE4, 0x57, 0x2E, 255};
  if (c == "GB") return {0x8B, 0xAC, 0x0F, 255};
  if (c == "GBC") return {0xA0, 0x6C, 0xD5, 255};
  if (c == "GBA") return {0x5B, 0x6E, 0xE1, 255};
  if (c == "SMS") return {0x1E, 0x88, 0xE5, 255};
  if (c == "GG") return {0x26, 0xA6, 0x9A, 255};
  if (c == "NGP") return {0xF3, 0x9C, 0x12, 255};
  // os outros consoles (capas sem imagem): cores claras, que aparecem sobre o fundo escuro
  static const struct { const char* c; SDL_Color k; } MORE[] = {
      {"MD", {0x5C, 0x7C, 0xFA, 255}}, {"32X", {0xEF, 0x53, 0x50, 255}}, {"FDS", {0xF0, 0x8A, 0x3C, 255}},
      {"PCE", {0xF0, 0x6A, 0x86, 255}}, {"PM", {0xF2, 0x8B, 0x8B, 255}}, {"2600", {0xBC, 0x95, 0x7E, 255}},
      {"5200", {0xC8, 0xA0, 0x6E, 255}}, {"7800", {0x9E, 0xB3, 0xC2, 255}}, {"LYNX", {0xFF, 0xA7, 0x26, 255}},
      {"MSX", {0x79, 0x86, 0xCB, 255}}, {"COLECO", {0x90, 0xA4, 0xAE, 255}}, {"SG", {0x26, 0xC6, 0xDA, 255}},
      {"WS", {0x4D, 0xD0, 0xE1, 255}}, {"VECTREX", {0x66, 0xBB, 0x6A, 255}}, {"INTV", {0xD4, 0xAF, 0x37, 255}},
      {"VB", {0xE5, 0x39, 0x35, 255}}, {"DUCK", {0x9C, 0xCC, 0x65, 255}}, {"SV", {0xA5, 0xB8, 0x8A, 255}},
      {"G&W", {0xFF, 0xD5, 0x4F, 255}}, {"GAMECOM", {0xAB, 0x47, 0xBC, 255}}, {"NDS", {0x78, 0x90, 0x9C, 255}}, {"JAGUAR", {0xFF, 0x70, 0x43, 255}},
      {"N64", {0x43, 0xA0, 0x47, 255}},
      {"ARCADE", {0xF0, 0x62, 0x92, 255}}, {"PS1", {0xB0, 0xBE, 0xC5, 255}}, {"3DO", {0x4D, 0xB6, 0xAC, 255}},
      {"PCFX", {0xCE, 0x93, 0xD8, 255}}, {"SATURN", {0x9F, 0xA8, 0xDA, 255}}, {"DC", {0xFF, 0x8F, 0x00, 255}}, {"CD", {0x95, 0x75, 0xCD, 255}},
  };
  for (const auto& e : MORE) if (c == e.c) return e.k;
  return color::MUTED;
}

// ------------------------------------------------------------ controles na tela
float TouchControls::unit(int ow, int oh, bool portrait, const Metrics& m) {
  const SDL_FRect a = m.area(ow, oh);
  // celular comum: ~400 dp de altura deitado e ~400 dp de largura em pé; tablets e telas maiores param aí
  return portrait ? std::min(a.w, 420 * m.dp) : std::min(a.h, 430 * m.dp);
}

// símbolos dos botões do PlayStation (a fonte só tem o Latin-1): desenhados com traços grossos, nas cores do
// controle (verde, vermelho, azul e rosa)
namespace {
void thickLine(SDL_Renderer* r, float x1, float y1, float x2, float y2, float w, SDL_Color c) {
  const float dx = x2 - x1, dy = y2 - y1, len = std::max(0.001f, std::hypot(dx, dy));
  const float nx = -dy / len * w / 2, ny = dx / len * w / 2;
  const SDL_FColor fc{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
  SDL_Vertex v[4] = {{{x1 + nx, y1 + ny}, fc, {0, 0}}, {{x2 + nx, y2 + ny}, fc, {0, 0}},
                     {{x2 - nx, y2 - ny}, fc, {0, 0}}, {{x1 - nx, y1 - ny}, fc, {0, 0}}};
  const int idx[6] = {0, 1, 2, 0, 2, 3};
  SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
}
}  // namespace

bool TouchControls::drawPsSymbol(SDL_Renderer* r, const char* label, float cx, float cy, float s, float w) const {
  if (label == PS_TRIANGLE) {
    const SDL_Color g{0x5C, 0xE0, 0xB0, 230};
    const float h = s * 0.95f;
    thickLine(r, cx, cy - h, cx + s, cy + h * 0.7f, w, g);
    thickLine(r, cx + s, cy + h * 0.7f, cx - s, cy + h * 0.7f, w, g);
    thickLine(r, cx - s, cy + h * 0.7f, cx, cy - h, w, g);
  } else if (label == PS_CIRCLE) {
    strokeCircle(r, cx, cy, s * 0.9f, w, SDL_Color{0xFF, 0x6B, 0x6B, 230});
  } else if (label == PS_CROSS) {
    const SDL_Color b{0x7C, 0xB4, 0xFF, 230};
    thickLine(r, cx - s * 0.8f, cy - s * 0.8f, cx + s * 0.8f, cy + s * 0.8f, w, b);
    thickLine(r, cx - s * 0.8f, cy + s * 0.8f, cx + s * 0.8f, cy - s * 0.8f, w, b);
  } else if (label == PS_SQUARE) {
    const SDL_Color k{0xF4, 0x9A, 0xD8, 230};
    const float q = s * 0.75f;
    thickLine(r, cx - q - w / 2, cy - q, cx + q + w / 2, cy - q, w, k);
    thickLine(r, cx + q, cy - q, cx + q, cy + q, w, k);
    thickLine(r, cx + q + w / 2, cy + q, cx - q - w / 2, cy + q, w, k);
    thickLine(r, cx - q, cy + q, cx - q, cy - q, w, k);
  } else {
    return false;
  }
  return true;
}

void TouchControls::layout(int ow, int oh, bool portrait, bool lr, bool xy, SDL_FRect game, bool ps1, bool tdo) {
  ctls_.clear();
  minHit_ = 24 * m.dp;
  const SDL_FRect a = m.area(ow, oh);
  const float L = a.x, R = a.x + a.w, T = a.y, B = a.y + a.h;
  const float u = unit(ow, oh, portrait, m);
  if (!portrait) {
    // cada grupo fica a 0,21u da borda; se a faixa livre ao lado da imagem é mais larga, vai para o meio dela
    float xl = L + 0.21f * u, xr = R - 0.21f * u;
    if (game.w > 0) {
      const float freeL = game.x - L, freeR = R - (game.x + game.w);
      // o grupo da direita (botões e Start) fica um pouco à esquerda do seu centro: compensa 0,036u
      if (freeL > 0.42f * u) xl = std::max(xl, L + freeL / 2);
      if (freeR > 0.42f * u) xr = std::min(xr, R - freeR / 2 + 0.036f * u);
    }
    ctls_.push_back({DPAD, xl, B - 0.38f * u, 0.17f * u, 0, nullptr, 0, NONE});
    if (tdo) {
      // 3DO: A, B e C em diagonal, como no controle
      const float cx = xr - 0.04f * u, cy = B - 0.40f * u, d = 0.15f * u, r = 0.07f * u;
      ctls_.push_back({ROUND, cx - d, cy + 0.06f * u, r, 0, "A", KEY_Y, NONE});
      ctls_.push_back({ROUND, cx, cy, r, 0, "B", KEY_A, NONE});
      ctls_.push_back({ROUND, cx + d, cy - 0.06f * u, r, 0, "C", KEY_B, NONE});
    } else if (xy) {
      // losango do controle do SNES: X em cima, Y à esquerda, A à direita, B embaixo (no PS1: Triângulo em cima,
      // Quadrado à esquerda, Círculo à direita, Xis embaixo; o núcleo do PS1 lê A = Círculo, B = Xis)
      const float cx = xr - 0.04f * u, cy = B - 0.40f * u, d = 0.125f * u, r = 0.072f * u;
      ctls_.push_back({ROUND, cx + d, cy, r, 0, ps1 ? PS_CIRCLE : "A", KEY_A, NONE});
      ctls_.push_back({ROUND, cx, cy + d, r, 0, ps1 ? PS_CROSS : "B", KEY_B, NONE});
      ctls_.push_back({ROUND, cx, cy - d, r, 0, ps1 ? PS_TRIANGLE : "X", KEY_X, NONE});
      ctls_.push_back({ROUND, cx - d, cy, r, 0, ps1 ? PS_SQUARE : "Y", KEY_Y, NONE});
    } else {
      ctls_.push_back({ROUND, xr + 0.08f * u, B - 0.48f * u, 0.085f * u, 0, "A", KEY_A, NONE});
      ctls_.push_back({ROUND, xr - 0.12f * u, B - 0.33f * u, 0.085f * u, 0, "B", KEY_B, NONE});
    }
    if (lr && ps1) {
      ctls_.push_back({PILL, xl, T + 0.09f * u, 0.30f * u, 0.085f * u, "L2", TOUCH_L2, NONE});
      ctls_.push_back({PILL, xr, T + 0.09f * u, 0.30f * u, 0.085f * u, "R2", TOUCH_R2, NONE});
      ctls_.push_back({PILL, xl, T + 0.19f * u, 0.30f * u, 0.085f * u, "L1", KEY_L, NONE});
      ctls_.push_back({PILL, xr, T + 0.19f * u, 0.30f * u, 0.085f * u, "R1", KEY_R, NONE});
    } else if (lr) {
      ctls_.push_back({PILL, xl, T + 0.13f * u, 0.30f * u, 0.11f * u, "L", KEY_L, NONE});
      ctls_.push_back({PILL, xr, T + 0.13f * u, 0.30f * u, 0.11f * u, "R", KEY_R, NONE});
    }
    ctls_.push_back({PILL, xl - 0.08f * u, B - 0.08f * u, 0.19f * u, 0.075f * u, tdo ? "X" : "Select", KEY_SELECT, NONE});
    ctls_.push_back({PILL, xr + 0.08f * u, B - 0.08f * u, 0.19f * u, 0.075f * u, tdo ? "P" : "Start", KEY_START, NONE});
    const float my = ps1 ? T + 0.285f * u : T + 0.27f * u;
    ctls_.push_back({PILL, xl, my, 0.17f * u, 0.075f * u, "Menu", 0, MENU});
    ctls_.push_back({PILL, xr, my, 0.17f * u, 0.075f * u, "\xC2\xBB", 0, FAST});
    layoutExtras(false, L, R, T, B, u, xl);
  } else {
    // em pé: grupos presos às bordas de baixo, da esquerda e da direita (num tablet a cruz não vai para o meio)
    const float C = (L + R) / 2;
    ctls_.push_back({DPAD, L + 0.26f * u, B - 0.42f * u, 0.21f * u, 0, nullptr, 0, NONE});
    if (tdo) {
      const float cx = R - 0.27f * u, cy = B - 0.43f * u, d = 0.165f * u, r = 0.08f * u;
      ctls_.push_back({ROUND, cx - d, cy + 0.07f * u, r, 0, "A", KEY_Y, NONE});
      ctls_.push_back({ROUND, cx, cy, r, 0, "B", KEY_A, NONE});
      ctls_.push_back({ROUND, cx + d, cy - 0.07f * u, r, 0, "C", KEY_B, NONE});
    } else if (xy) {
      const float cx = R - 0.27f * u, cy = B - 0.43f * u, d = 0.14f * u, r = 0.085f * u;
      ctls_.push_back({ROUND, cx + d, cy, r, 0, ps1 ? PS_CIRCLE : "A", KEY_A, NONE});
      ctls_.push_back({ROUND, cx, cy + d, r, 0, ps1 ? PS_CROSS : "B", KEY_B, NONE});
      ctls_.push_back({ROUND, cx, cy - d, r, 0, ps1 ? PS_TRIANGLE : "X", KEY_X, NONE});
      ctls_.push_back({ROUND, cx - d, cy, r, 0, ps1 ? PS_SQUARE : "Y", KEY_Y, NONE});
    } else {
      ctls_.push_back({ROUND, R - 0.17f * u, B - 0.50f * u, 0.11f * u, 0, "A", KEY_A, NONE});
      ctls_.push_back({ROUND, R - 0.40f * u, B - 0.36f * u, 0.11f * u, 0, "B", KEY_B, NONE});
    }
    if (lr && ps1) {
      ctls_.push_back({PILL, L + 0.17f * u, B - 0.89f * u, 0.28f * u, 0.075f * u, "L2", TOUCH_L2, NONE});
      ctls_.push_back({PILL, R - 0.17f * u, B - 0.89f * u, 0.28f * u, 0.075f * u, "R2", TOUCH_R2, NONE});
      ctls_.push_back({PILL, L + 0.17f * u, B - 0.79f * u, 0.28f * u, 0.075f * u, "L1", KEY_L, NONE});
      ctls_.push_back({PILL, R - 0.17f * u, B - 0.79f * u, 0.28f * u, 0.075f * u, "R1", KEY_R, NONE});
    } else if (lr) {
      ctls_.push_back({PILL, L + 0.17f * u, B - 0.80f * u, 0.28f * u, 0.09f * u, "L", KEY_L, NONE});
      ctls_.push_back({PILL, R - 0.17f * u, B - 0.80f * u, 0.28f * u, 0.09f * u, "R", KEY_R, NONE});
    }
    ctls_.push_back({PILL, C - 0.12f * u, B - 0.11f * u, 0.20f * u, 0.07f * u, tdo ? "X" : "Select", KEY_SELECT, NONE});
    ctls_.push_back({PILL, C + 0.12f * u, B - 0.11f * u, 0.20f * u, 0.07f * u, tdo ? "P" : "Start", KEY_START, NONE});
    ctls_.push_back({PILL, C - 0.08f * u, B - 0.80f * u, 0.13f * u, 0.07f * u, "Menu", 0, MENU});
    ctls_.push_back({PILL, C + 0.08f * u, B - 0.80f * u, 0.13f * u, 0.07f * u, "\xC2\xBB", 0, FAST});
    layoutExtras(true, L, R, T, B, u, L + 0.17f * u);
  }
}

// botão do disco e teclado numérico. Deitado: "Disco" no lugar do L (o FDS não tem L e R) e "123" embaixo, no
// meio; o teclado abre acima do "123". Em pé: "Disco" no lugar do L, "123" entre a cruz e os botões e o
// teclado logo acima da faixa dos controles.
void TouchControls::layoutExtras(bool portrait, float L, float R, float T, float B, float u, float xl) {
  const float C = (L + R) / 2;
  if (disk) {
    if (portrait) ctls_.push_back({PILL, xl, B - 0.80f * u, 0.24f * u, 0.075f * u, "Disco", 0, DISK});
    else ctls_.push_back({PILL, xl, T + 0.13f * u, 0.26f * u, 0.085f * u, "Disco", 0, DISK});
  }
  if (keypad == 0) { kpOpen_ = false; kpBase_ = ctls_.size(); return; }
  if (portrait) ctls_.push_back({PILL, C, B - 0.67f * u, 0.15f * u, 0.07f * u, "123", 0, KEYPAD});
  else ctls_.push_back({PILL, C, B - 0.08f * u, 0.17f * u, 0.075f * u, "123", 0, KEYPAD});
  kpBase_ = ctls_.size();
  if (!kpOpen_) return;
  static const char* const LABELS[2][12] = {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"},
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "Clear", "0", "Enter"}};
  const char* const* lab = LABELS[keypad == 2 ? 1 : 0];
  const float sx = 0.135f * u, sy = 0.10f * u, kw = 0.12f * u, kh = 0.085f * u;
  const float bottom = portrait ? B - 0.98f * u : B - 0.18f * u;  // centro da fileira de baixo
  for (int i = 0; i < 12; i++) {
    const int row = i / 3, col = i % 3;
    const uint32_t key = i == 9 ? KEY_KPSTAR : i == 11 ? KEY_KPHASH : KEY_KP0 << (i == 10 ? 0 : i + 1);
    ctls_.push_back({PILL, C + (col - 1) * sx, bottom - (3 - row) * sy, kw, kh, lab[i], key, NONE});
  }
}

bool TouchControls::find(const std::string& label, float& x, float& y) const {
  for (const Ctl& c : ctls_)
    if (c.label && label == c.label) { x = c.cx; y = c.cy; return true; }
  return false;
}

int TouchControls::hit(float x, float y) const {
  // procura o controle mais próximo dentro de uma área um pouco maior que o desenho (dedos não são precisos)
  int best = -1;
  float bestD = 1e30f;
  for (int i = 0; i < static_cast<int>(ctls_.size()); i++) {
    const Ctl& c = ctls_[i];
    const float dx = x - c.cx, dy = y - c.cy;
    float d;
    if (c.kind == PILL) {
      const float hx = std::max(c.w * 0.65f, minHit_), hy = std::max(c.h * 0.8f, minHit_);
      if (std::fabs(dx) > hx || std::fabs(dy) > hy) continue;
      d = std::hypot(dx / hx, dy / hy);
    } else {
      const float rr = c.w * (c.kind == DPAD ? 1.35f : 1.3f);
      d = std::hypot(dx, dy) / rr;
      if (d > 1) continue;
    }
    if (d < bestD) { bestD = d; best = i; }
  }
  return best;
}

uint32_t TouchControls::dpadDir(const Ctl& c, float x, float y) const {
  const float dx = x - c.cx, dy = y - c.cy, d = std::hypot(dx, dy);
  if (d < c.w * 0.22f) return 0;
  uint32_t m = 0;
  // inclinação de ~22° para cada lado conta como diagonal
  if (dx > 0.38f * d) m |= KEY_RIGHT;
  if (dx < -0.38f * d) m |= KEY_LEFT;
  if (dy > 0.38f * d) m |= KEY_DOWN;
  if (dy < -0.38f * d) m |= KEY_UP;
  return m;
}

TouchAction TouchControls::fingerDown(SDL_FingerID id, float x, float y) {
  const int i = hit(x, y);
  if (i < 0) return TouchAction::None;
  const Ctl& c = ctls_[i];
  if (c.special == MENU) return TouchAction::Menu;
  if (c.special == DISK) return TouchAction::Disk;
  if (c.special == KEYPAD) {
    // abre ou fecha o teclado; ao fechar, solta as teclas que ainda estavam apertadas
    kpOpen_ = !kpOpen_;
    if (!kpOpen_)
      for (auto it = fingers_.begin(); it != fingers_.end();)
        it = it->second.ctl >= static_cast<int>(kpBase_) ? fingers_.erase(it) : std::next(it);
    return TouchAction::Control;
  }
  fingers_[id] = {i, c.kind == DPAD ? dpadDir(c, x, y) : c.key};
  return TouchAction::Control;
}

void TouchControls::fingerMove(SDL_FingerID id, float x, float y) {
  auto it = fingers_.find(id);
  if (it == fingers_.end() || it->second.ctl >= static_cast<int>(ctls_.size())) return;
  const Ctl& c = ctls_[it->second.ctl];
  if (c.kind == DPAD) { it->second.dir = dpadDir(c, x, y); return; }
  // arrastar o dedo de um botão para outro (de B para A, por exemplo) troca o botão
  const int j = hit(x, y);
  if (j >= 0 && j != it->second.ctl && ctls_[j].kind == ROUND) it->second = {j, ctls_[j].key};
}

void TouchControls::fingerUp(SDL_FingerID id) { fingers_.erase(id); }

uint32_t TouchControls::keys() const {
  uint32_t m = 0;
  for (const auto& [id, f] : fingers_) m |= f.dir;
  return m;
}

bool TouchControls::turbo() const {
  for (const auto& [id, f] : fingers_)
    if (f.ctl < static_cast<int>(ctls_.size()) && ctls_[f.ctl].special == FAST) return true;
  return false;
}

bool TouchControls::pressed(int idx) const {
  for (const auto& [id, f] : fingers_)
    if (f.ctl == idx) return true;
  return false;
}

void TouchControls::draw(SDL_Renderer* r) const {
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  // vidro: fundo bem claro e transparente, contorno fino; apertado fica na cor do Índigo
  const SDL_Color fill{255, 255, 255, 22}, ring{255, 255, 255, 64}, on = alpha(color::PRIMARY, 150), label{255, 255, 255, 200};
  const float line = std::max(1.5f, 1.5f * m.dp);  // contorno com a mesma espessura física em qualquer tela
  for (int i = 0; i < static_cast<int>(ctls_.size()); i++) {
    const Ctl& c = ctls_[i];
    const bool p = pressed(i);
    if (c.kind == DPAD) {
      uint32_t dir = 0;
      for (const auto& [id, f] : fingers_) if (f.ctl == i) dir |= f.dir;
      const float a = c.w * 0.34f, b = c.w * 0.98f;
      const SDL_FRect v{c.cx - a, c.cy - b, 2 * a, 2 * b}, h{c.cx - b, c.cy - a, 2 * b, 2 * a};
      (void)ring;
      // base redonda com contorno e a cruz por cima (sem costuras)
      fillCircle(r, c.cx, c.cy, c.w * 1.08f, SDL_Color{255, 255, 255, 12});
      strokeCircle(r, c.cx, c.cy, c.w * 1.08f, line, SDL_Color{255, 255, 255, 40});
      const SDL_Color cross{0x2A, 0x2D, 0x3B, 255};  // cor cheia: as duas barras se cruzam sem marcar o meio
      fillRoundRect(r, v, a * 0.55f, cross);
      fillRoundRect(r, h, a * 0.55f, cross);
      // lado apertado
      const SDL_FRect arms[4] = {{c.cx - a, c.cy - b, 2 * a, b - a * 0.6f}, {c.cx - a, c.cy + a * 0.6f, 2 * a, b - a * 0.6f},
                                 {c.cx - b, c.cy - a, b - a * 0.6f, 2 * a}, {c.cx + a * 0.6f, c.cy - a, b - a * 0.6f, 2 * a}};
      const uint32_t bits[4] = {KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT};
      for (int k = 0; k < 4; k++) if (dir & bits[k]) fillRoundRect(r, arms[k], a * 0.55f, on);
      // setinhas
      const float t = a * 0.45f, o = b * 0.72f;
      const SDL_Color ar{255, 255, 255, 110};
      fillTriangle(r, {c.cx, c.cy - o - t * 0.6f}, {c.cx - t, c.cy - o + t * 0.6f}, {c.cx + t, c.cy - o + t * 0.6f}, ar);
      fillTriangle(r, {c.cx, c.cy + o + t * 0.6f}, {c.cx + t, c.cy + o - t * 0.6f}, {c.cx - t, c.cy + o - t * 0.6f}, ar);
      fillTriangle(r, {c.cx - o - t * 0.6f, c.cy}, {c.cx - o + t * 0.6f, c.cy + t}, {c.cx - o + t * 0.6f, c.cy - t}, ar);
      fillTriangle(r, {c.cx + o + t * 0.6f, c.cy}, {c.cx + o - t * 0.6f, c.cy - t}, {c.cx + o - t * 0.6f, c.cy + t}, ar);
    } else if (c.kind == ROUND) {
      fillCircle(r, c.cx, c.cy, c.w, p ? on : fill);
      strokeCircle(r, c.cx, c.cy, c.w, line, ring);
      if (drawPsSymbol(r, c.label, c.cx, c.cy, c.w * 0.42f, std::max(2.0f, 2.2f * m.dp))) continue;
      const float px = c.w * 0.8f;
      drawText(r, c.cx - textWidth(c.label, px, true) / 2, c.cy - text::lineHeight(px) / 2, c.label, px, label, true);
    } else {
      const SDL_FRect rc{c.cx - c.w / 2, c.cy - c.h / 2, c.w, c.h};
      // teclas do teclado numérico: fundo escuro para ler por cima do jogo; "123" aceso com o teclado aberto
      const bool kpKey = kpOpen_ && i >= static_cast<int>(kpBase_);
      const SDL_Color base = kpKey ? SDL_Color{0x0E, 0x0F, 0x14, 190} : (c.special == KEYPAD && kpOpen_) ? alpha(color::PRIMARY, 90) : fill;
      fillRoundRect(r, rc, c.h / 2, p ? on : base);
      strokeRoundRect(r, rc, c.h / 2, line, ring);
      float px = c.h * 0.5f;
      const float lw = textWidth(c.label, px, true);
      if (lw > c.w * 0.75f) px *= c.w * 0.75f / lw;  // rótulo comprido ("Select") encolhe para caber na pílula
      drawText(r, c.cx - textWidth(c.label, px, true) / 2, c.cy - text::lineHeight(px) / 2, c.label, px, label, true);
    }
  }
}

// ------------------------------------------------------------ menu
void Menu::move(int d) {
  if (items.empty()) return;
  sel = (sel + d + static_cast<int>(items.size())) % static_cast<int>(items.size());
}

void Menu::layout(int ow, int oh) {
  const float W = static_cast<float>(ow), H = static_cast<float>(oh);
  const SDL_FRect a = m.area(ow, oh);
  dp_ = m.dp;
  const int n = static_cast<int>(items.size());
  rowH_ = (m.touch ? 52 : 46) * dp_;  // toque: alvo de pelo menos 48 dp
  textPx_ = 16 * dp_;
  const float header = 62 * dp_, pad = 12 * dp_, hintH = hint.empty() ? 0 : 34 * dp_;
  float longest = textWidth(title, 19 * dp_, true) + 40 * dp_;
  for (const auto& it : items) longest = std::max(longest, textWidth(it.text, textPx_) + 56 * dp_);
  contentH_ = n * rowH_;
  float tw = 0, th = 0;
  if (preview) SDL_GetTextureSize(preview, &tw, &th);
  previewRect_ = {};
  if (sheet) {
    // em pé: painel embaixo, largura toda (num tablet, no máximo 640 dp no meio); a miniatura fica em cima.
    // A parte de baixo do painel passa por trás da barra de gestos, mas os itens ficam acima dela.
    const float bottomInset = H - (a.y + a.h);
    const float pw = std::min(W, 640 * dp_), px0 = (W - pw) / 2;
    const float maxList = (a.y + a.h - a.y) * (preview ? 0.55f : 0.78f) - header - hintH - pad;
    const float listH = std::min(contentH_, std::max(rowH_ * 3, maxList));
    const float ph = header + listH + pad + hintH + 8 * dp_ + bottomInset;
    panel_ = {px0, H - ph, pw, ph};
    const float lx = std::max(px0 + pad, a.x + pad), rx = std::min(px0 + pw - pad, a.x + a.w - pad);
    list_ = {lx, panel_.y + header, rx - lx, listH};
    if (preview && tw > 0) {
      const float top = a.y + 8 * dp_;
      const float s = std::min(a.w * 0.86f / tw, (panel_.y - top - 24 * dp_) / th);
      if (s > 0) previewRect_ = {(W - tw * s) / 2, top + (panel_.y - top - th * s) / 2, tw * s, th * s};
    }
  } else {
    float panelW = std::clamp(longest, 320 * dp_, 520 * dp_);
    panelW = std::min(panelW, a.w * 0.94f);
    const bool side = preview && tw > 0 && W > H;
    if (side) panelW = std::min(panelW, a.w * 0.5f);
    const float maxList = a.h * 0.9f - header - hintH - pad;
    const float listH = std::min(contentH_, maxList);
    const float panelH = header + listH + pad + hintH;
    // com a miniatura ao lado, o conjunto (painel + miniatura) fica no meio, sem passar de ~1100 dp
    float px0 = a.x + (a.w - panelW) / 2;
    float prevW = 0;
    if (side) {
      const float total = std::min(a.w * 0.92f, std::max(panelW * 2.2f, 1100 * dp_));
      px0 = a.x + (a.w - total) / 2;
      prevW = total - panelW - 32 * dp_;
    }
    panel_ = {px0, a.y + (a.h - panelH) / 2, panelW, panelH};
    list_ = {panel_.x + pad, panel_.y + header, panelW - 2 * pad, listH};
    if (side) {
      const float ah = a.h * 0.8f;
      const float s = std::min(prevW / tw, ah / th);
      const float x0 = panel_.x + panelW + 32 * dp_;
      previewRect_ = {x0 + (prevW - tw * s) / 2, a.y + (a.h - th * s) / 2, tw * s, th * s};
    } else if (preview && tw > 0) {
      // PC em pé (raro): miniatura em cima
      const float s = std::min(a.w * 0.6f / tw, (panel_.y - a.y - 16 * dp_) / th);
      if (s > 0) previewRect_ = {(W - tw * s) / 2, a.y + (panel_.y - a.y - th * s) / 2, tw * s, th * s};
    }
  }
  rects_.clear();
  for (int k = 0; k < n; k++) rects_.push_back({list_.x, list_.y + k * rowH_, list_.w, rowH_});
  // rolagem: mantém o item escolhido à vista quando ele muda
  const float maxScroll = std::max(0.0f, contentH_ - list_.h);
  if (sel != lastSel_ && sel >= 0 && sel < n) {
    const float top = sel * rowH_, bottom = top + rowH_;
    if (top < scroll_) scroll_ = top;
    if (bottom > scroll_ + list_.h) scroll_ = bottom - list_.h;
    lastSel_ = sel;
  }
  scroll_ = std::clamp(scroll_, 0.0f, maxScroll);
}

void Menu::scrollBy(float dy) { scroll_ += dy; }

int Menu::hit(float x, float y) const {
  if (inside(list_, x, y)) {
    for (size_t k = 0; k < rects_.size(); k++) {
      SDL_FRect r = rects_[k];
      r.y -= scroll_;
      if (inside(r, x, y)) return items[k].id;
    }
  }
  return inside(panel_, x, y) ? -1 : -2;
}

void Menu::draw(SDL_Renderer* r, int ow, int oh, bool dim) {
  layout(ow, oh);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  if (dim) {
    SDL_SetRenderDrawColor(r, 0x05, 0x06, 0x0A, 140);
    SDL_RenderFillRect(r, nullptr);
  }
  const float rad = 22 * dp_;
  if (sheet) {
    // cantos de cima arredondados: o retângulo passa da borda de baixo
    fillRoundRect(r, {panel_.x, panel_.y, panel_.w, panel_.h + rad}, rad, color::SURFACE);
    fillRoundRect(r, {panel_.x + panel_.w / 2 - 20 * dp_, panel_.y + 8 * dp_, 40 * dp_, 4 * dp_}, 2 * dp_, {255, 255, 255, 50});
  } else {
    fillRoundRect(r, panel_, rad, color::SURFACE);
    strokeRoundRect(r, panel_, rad, std::max(1.0f, dp_), {255, 255, 255, 18});
  }
  const float titlePx = 19 * dp_;
  drawText(r, list_.x + 12 * dp_, panel_.y + (sheet ? 24 : 18) * dp_, text::fit(title, titlePx, list_.w - 24 * dp_, text::BOLD), titlePx,
           color::TEXT, true);
  const SDL_Rect clip{static_cast<int>(list_.x), static_cast<int>(list_.y), static_cast<int>(list_.w), static_cast<int>(std::ceil(list_.h))};
  SDL_SetRenderClipRect(r, &clip);
  for (size_t k = 0; k < rects_.size(); k++) {
    SDL_FRect rc = rects_[k];
    rc.y -= scroll_;
    if (rc.y + rc.h < list_.y || rc.y > list_.y + list_.h) continue;
    const bool s = static_cast<int>(k) == sel;
    const SDL_FRect row{rc.x, rc.y + 2 * dp_, rc.w, rc.h - 4 * dp_};
    if (s) fillRoundRect(r, row, 12 * dp_, alpha(color::PRIMARY, 70));
    if (s) fillRoundRect(r, {row.x, row.y + row.h * 0.25f, 4 * dp_, row.h * 0.5f}, 2 * dp_, color::PRIMARY);
    const std::string t = text::fit(items[k].text, textPx_, row.w - 32 * dp_);
    drawText(r, row.x + 16 * dp_, row.y + (row.h - text::lineHeight(textPx_)) / 2, t, textPx_, s ? color::TEXT : SDL_Color{0xD4, 0xD6, 0xE0, 255}, s);
  }
  SDL_SetRenderClipRect(r, nullptr);
  // indicação de que há mais itens para rolar
  if (contentH_ > list_.h + 1) {
    const float frac = list_.h / contentH_, pos = scroll_ / std::max(1.0f, contentH_ - list_.h);
    const float bh = list_.h * frac;
    fillRoundRect(r, {list_.x + list_.w + 3 * dp_, list_.y + (list_.h - bh) * pos, 3 * dp_, bh}, 1.5f * dp_, {255, 255, 255, 60});
  }
  if (preview && previewRect_.w > 0) {
    const SDL_FRect border{previewRect_.x - 6 * dp_, previewRect_.y - 6 * dp_, previewRect_.w + 12 * dp_, previewRect_.h + 12 * dp_};
    fillRoundRect(r, border, 12 * dp_, color::SURFACE2);
    SDL_RenderTexture(r, preview, nullptr, &previewRect_);
  }
  if (!hint.empty()) {
    const float px = 12 * dp_;
    const std::string t = text::fit(hint, px, panel_.w - 32 * dp_);
    drawText(r, panel_.x + (panel_.w - textWidth(t, px)) / 2, list_.y + list_.h + 12 * dp_, t, px, color::MUTED);
  }
}

// ------------------------------------------------------------ biblioteca
void Library::layout(int ow, int oh) {
  W_ = ow; H_ = oh;
  const float W = static_cast<float>(ow);
  const SDL_FRect a = m.area(ow, oh);
  dp_ = m.dp;
  const bool narrow = a.w / dp_ < 480;  // celular em pé: margens menores
  const bool shortH = a.h / dp_ < 520;  // celular deitado / janela baixa: cabeçalho mais baixo
  const float pad = (narrow ? 12 : 20) * dp_;
  // conteúdo com no máximo ~1500 dp de largura, no meio (telas muito largas não espalham os cartões)
  left_ = a.x + pad; right_ = a.x + a.w - pad;
  const float maxW = 1500 * dp_;
  if (right_ - left_ > maxW) { const float c = (left_ + right_) / 2; left_ = c - maxW / 2; right_ = c + maxW / 2; }
  bottom_ = a.y + a.h;
  top_ = {0, a.y, W, (shortH ? 54 : 64) * dp_};
  const float bh = (m.touch ? 42 : 38) * dp_, by = top_.y + (top_.h - bh) / 2 + (shortH ? 0 : 4) * dp_;
  const float sw = textWidth("Ajustes", 14 * dp_, true) + 32 * dp_, ow2 = textWidth("+ Abrir", 14 * dp_, true) + 32 * dp_;
  settingsBtn_ = {right_ - sw, by, sw, bh};
  openBtn_ = {settingsBtn_.x - 10 * dp_ - ow2, by, ow2, bh};
  // abas: numa linha se cabem; senão quebram em linhas (mouse/teclado) ou rolam para os lados (toque)
  tabRects_.clear();
  const float th = (m.touch ? 40 : 36) * dp_, tgap = 8 * dp_, ty = top_.y + top_.h + (shortH ? 2 : 6) * dp_;
  float total = 0;
  std::vector<float> widths;
  for (const auto& t : tabs) { widths.push_back(textWidth(t, 14 * dp_, true) + 30 * dp_); total += widths.back() + tgap; }
  total -= tgap;
  const float avail = right_ - left_;
  tabsScroll_ = total > avail && m.touch;
  float x = left_, y = ty;
  for (float w : widths) {
    if (!tabsScroll_ && x > left_ && x + w > right_ + 0.5f) { x = left_; y += th + tgap; }
    tabRects_.push_back({x, y, w, th});
    x += w + tgap;
  }
  const float tabsH = tabRects_.empty() ? th : tabRects_.back().y + th - ty;
  tabsW_ = std::max(0.0f, total);
  // a área das abas roláveis vai de borda a borda da área segura (as abas somem sob o degradê, não cortadas)
  tabsBox_ = {tabsScroll_ ? a.x : left_, ty, tabsScroll_ ? a.w : avail, tabsH};
  if (tab != lastTab_ && tab >= 0 && tab < static_cast<int>(tabRects_.size()) && tabsScroll_) {
    const SDL_FRect& t = tabRects_[tab];
    if (t.x - tabScroll_ < left_) tabScroll_ = t.x - left_;
    if (t.x + t.w - tabScroll_ > right_) tabScroll_ = t.x + t.w - right_;
  }
  lastTab_ = tab;
  tabScroll_ = tabsScroll_ ? std::clamp(tabScroll_, 0.0f, std::max(0.0f, tabsW_ - avail)) : 0;
  const float hintH = hint.empty() ? 0 : 30 * dp_;
  const float cy0 = ty + tabsH + (shortH ? 8 : 12) * dp_;
  content_ = {0, cy0, W, bottom_ - cy0 - hintH};
  // conteúdo (coordenadas a partir do topo do conteúdo, sem a rolagem)
  float yy = 4 * dp_;
  if (hasContinue) {
    contRect_ = {left_, yy, avail, (narrow ? 104 : 120) * dp_};
    yy += contRect_.h + 22 * dp_;
  }
  sectionY_ = yy;
  yy += 32 * dp_;
  // colunas pela largura: cartão de pelo menos 168 dp no PC e 104 dp no toque (3 por linha num celular em pé)
  const float gap = (narrow ? 10 : 14) * dp_;
  const float minCard = (m.touch ? 104 : 168) * dp_;
  cols_ = std::max(2, static_cast<int>((avail + gap) / (minCard + gap)));
  const float cw = (avail - (cols_ - 1) * gap) / cols_;
  const float chh = cw * 0.75f + std::clamp(cw * 0.3f, 46 * dp_, 56 * dp_);
  cards_.clear();
  for (size_t k = 0; k < games.size(); k++) {
    const int row = static_cast<int>(k) / cols_, col = static_cast<int>(k) % cols_;
    cards_.push_back({left_ + col * (cw + gap), yy + row * (chh + gap), cw, chh});
  }
  contentH_ = (cards_.empty() ? yy + 160 * dp_ : cards_.back().y + chh) + 20 * dp_;
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - content_.h));
}

bool Library::inTabs(float x, float y) const { return tabsScroll_ && inside(tabsBox_, x, y); }

void Library::scrollTabs(float dx) {
  if (!tabsScroll_) return;
  tabScroll_ = std::clamp(tabScroll_ + dx, 0.0f, std::max(0.0f, tabsW_ - (right_ - left_)));
}

void Library::ensureVisible() {
  if (W_ == 0) return;
  layout(W_, H_);
  SDL_FRect r;
  if (focus < 0) { if (!hasContinue) return; r = contRect_; r.y = 0; }  // Continuar: volta ao topo
  else if (focus < static_cast<int>(cards_.size())) r = cards_[focus];
  else return;
  if (focus >= 0 && focus < cols_) r.y = 0;  // primeira linha: mostra o título da seção também
  if (r.y < scroll_) scroll_ = r.y;
  if (r.y + r.h > scroll_ + content_.h) scroll_ = r.y + r.h - content_.h + 8 * dp_;
  scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - content_.h));
}

void Library::navigate(int dx, int dy) {
  const int n = static_cast<int>(games.size());
  if (focus < 0) {
    if (dy > 0 && n) focus = 0;
  } else {
    if (dx) {
      const int col = focus % cols_;
      const int to = focus + dx;
      if (col + dx >= 0 && col + dx < cols_ && to >= 0 && to < n) focus = to;
    }
    if (dy < 0) {
      if (focus >= cols_) focus -= cols_;
      else if (hasContinue) focus = -1;
    } else if (dy > 0) {
      if (focus + cols_ < n) focus += cols_;
      else if (focus / cols_ < (n - 1) / cols_) focus = n - 1;
    }
  }
  if (!hasContinue && focus < 0) focus = 0;
  ensureVisible();
}

void Library::scrollBy(float dy) { scroll_ = std::clamp(scroll_ + dy, 0.0f, std::max(0.0f, contentH_ - content_.h)); }

Library::Hit Library::hit(float x, float y) const {
  // no toque os botões de cima valem numa área de pelo menos 48 dp de altura
  auto grow = [&](SDL_FRect r) {
    const float need = 48 * dp_;
    if (m.touch && r.h < need) { r.y -= (need - r.h) / 2; r.h = need; }
    return r;
  };
  if (inside(grow(openBtn_), x, y)) return {OPEN, -1};
  if (inside(grow(settingsBtn_), x, y)) return {SETTINGS, -1};
  for (size_t k = 0; k < tabRects_.size(); k++) {
    SDL_FRect t = grow(tabRects_[k]);
    t.x -= tabScroll_;
    if (inside(t, x, y) && (!tabsScroll_ || inside(tabsBox_, x, y))) return {TAB, static_cast<int>(k)};
  }
  if (!inside(content_, x, y)) return {};
  const float cy = y - content_.y + scroll_;
  if (hasContinue && inside(contRect_, x, cy)) return {CONTINUE, -1};
  for (size_t k = 0; k < cards_.size(); k++)
    if (inside(cards_[k], x, cy)) return {GAME, static_cast<int>(k)};
  return {};
}

void Library::draw(SDL_Renderer* r, int ow, int oh) {
  layout(ow, oh);
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
  const float pad = left_;
  const float line = std::max(1.0f, dp_);
  // barra de cima
  const float logo = (top_.h < 60 * dp_ ? 22 : 25) * dp_;
  drawText(r, pad, top_.y + (top_.h - text::lineHeight(logo)) / 2 + (top_.h < 60 * dp_ ? 0 : 4) * dp_, "\xC3\x8Dndigo", logo, color::TEXT, true);
  fillRoundRect(r, openBtn_, openBtn_.h / 2, color::PRIMARY);
  drawText(r, openBtn_.x + (openBtn_.w - textWidth("+ Abrir", 14 * dp_, true)) / 2, openBtn_.y + (openBtn_.h - text::lineHeight(14 * dp_)) / 2,
           "+ Abrir", 14 * dp_, color::TEXT, true);
  fillRoundRect(r, settingsBtn_, settingsBtn_.h / 2, color::SURFACE2);
  drawText(r, settingsBtn_.x + (settingsBtn_.w - textWidth("Ajustes", 14 * dp_, true)) / 2,
           settingsBtn_.y + (settingsBtn_.h - text::lineHeight(14 * dp_)) / 2, "Ajustes", 14 * dp_, color::TEXT, true);
  // abas por console
  if (tabsScroll_) {
    const SDL_Rect tc{static_cast<int>(tabsBox_.x), static_cast<int>(tabsBox_.y), static_cast<int>(tabsBox_.w), static_cast<int>(std::ceil(tabsBox_.h))};
    SDL_SetRenderClipRect(r, &tc);
  }
  for (size_t k = 0; k < tabRects_.size(); k++) {
    SDL_FRect t = tabRects_[k];
    t.x -= tabScroll_;
    if (t.x > tabsBox_.x + tabsBox_.w || t.x + t.w < tabsBox_.x) continue;
    const bool s = static_cast<int>(k) == tab;
    if (s) fillRoundRect(r, t, t.h / 2, color::TEXT);
    else { fillRoundRect(r, t, t.h / 2, color::SURFACE); strokeRoundRect(r, t, t.h / 2, line, {255, 255, 255, 28}); }
    const float px = 14 * dp_;
    drawText(r, t.x + (t.w - textWidth(tabs[k], px, true)) / 2, t.y + (t.h - text::lineHeight(px)) / 2, tabs[k], px,
             s ? color::BG : color::MUTED, true);
  }
  if (tabsScroll_) {
    // degradê nas pontas que têm mais abas escondidas: mostra que dá para arrastar para o lado
    const float fw = 36 * dp_, maxS = std::max(0.0f, tabsW_ - (right_ - left_));
    auto fade = [&](float x0, bool toRight) {
      const SDL_FColor solid{color::BG.r / 255.0f, color::BG.g / 255.0f, color::BG.b / 255.0f, 1}, clear{solid.r, solid.g, solid.b, 0};
      const SDL_FColor c0 = toRight ? clear : solid, c1 = toRight ? solid : clear;
      const float y0 = tabsBox_.y, y1 = tabsBox_.y + tabsBox_.h;
      const SDL_Vertex v[4] = {{{x0, y0}, c0, {0, 0}}, {{x0 + fw, y0}, c1, {0, 0}}, {{x0 + fw, y1}, c1, {0, 0}}, {{x0, y1}, c0, {0, 0}}};
      const int idx[6] = {0, 1, 2, 0, 2, 3};
      SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
    };
    if (tabScroll_ > 1) fade(tabsBox_.x, false);
    if (tabScroll_ < maxS - 1) fade(tabsBox_.x + tabsBox_.w - fw, true);
    SDL_SetRenderClipRect(r, nullptr);
  }
  // conteúdo com rolagem
  const SDL_Rect clip{0, static_cast<int>(content_.y), ow, static_cast<int>(content_.h)};
  SDL_SetRenderClipRect(r, &clip);
  const float oy = content_.y - scroll_;
  if (hasContinue) {
    const SDL_FRect c{contRect_.x, contRect_.y + oy, contRect_.w, contRect_.h};
    const float k = c.h / (120 * dp_);  // celular em pé: um pouco menor
    fillRoundRect(r, c, 18 * dp_, color::SURFACE2);
    if (showFocus && focus == -1) strokeRoundRect(r, c, 18 * dp_, 3 * dp_, color::PRIMARY);
    const float ch = c.h - 20 * dp_;
    const SDL_FRect box{c.x + 10 * dp_, c.y + 10 * dp_, ch * 4 / 3, ch};
    fillRoundRect(r, box, 12 * dp_, {0x0A, 0x0B, 0x10, 255});
    if (cont.cover) drawFit(r, cont.cover, {box.x + 4 * dp_, box.y + 4 * dp_, box.w - 8 * dp_, box.h - 8 * dp_});
    const float tx = box.x + box.w + 16 * dp_ * k, play = 44 * dp_ * k;
    const float tw = c.x + c.w - tx - play - 24 * dp_;
    const float tp = 17 * dp_ * std::min(1.0f, k + 0.05f);
    drawText(r, tx, c.y + 24 * dp_ * k, "CONTINUAR", 11.5f * dp_, color::PRIMARY, true);
    drawText(r, tx, c.y + 44 * dp_ * k, text::fit(cont.title, tp, tw, text::BOLD), tp, color::TEXT, true);
    fillCircle(r, tx + 4 * dp_, c.y + 82 * dp_ * k + text::lineHeight(12 * dp_) / 2, 4 * dp_, consoleColor(cont.console));
    drawText(r, tx + 14 * dp_, c.y + 82 * dp_ * k, cont.console, 12 * dp_, color::MUTED);
    const float pcx = c.x + c.w - play / 2 - 18 * dp_, pcy = c.y + c.h / 2;
    fillCircle(r, pcx, pcy, play / 2, color::PRIMARY);
    const float t = play * 0.2f;
    fillTriangle(r, {pcx - t * 0.7f, pcy - t}, {pcx + t * 1.1f, pcy}, {pcx - t * 0.7f, pcy + t}, color::TEXT);
  }
  // título da seção
  const std::string section = (tab >= 0 && tab < static_cast<int>(tabs.size()) ? tabs[tab] : std::string("Jogos"));
  const std::string count = std::to_string(games.size()) + (games.size() == 1 ? " jogo" : " jogos");
  drawText(r, pad, sectionY_ + oy, section == "Todos" ? "Seus jogos" : section, 17 * dp_, color::TEXT, true);
  drawText(r, right_ - textWidth(count, 12.5f * dp_), sectionY_ + oy + 3 * dp_, count, 12.5f * dp_, color::MUTED);
  for (size_t k = 0; k < cards_.size(); k++) {
    SDL_FRect c = cards_[k];
    c.y += oy;
    if (c.y > content_.y + content_.h || c.y + c.h < content_.y) continue;
    const Game& g = games[k];
    // medidas de dentro do cartão acompanham a largura dele (cartões pequenos no celular em pé)
    const float u = std::clamp(c.w / (180 * dp_), 0.72f, 1.0f) * dp_;
    const float in = 6 * u, rad = 16 * u;
    fillRoundRect(r, c, rad, color::SURFACE);
    const SDL_FRect box{c.x + in, c.y + in, c.w - 2 * in, (c.w - 2 * in) * 0.75f};
    if (g.cover) {
      fillRoundRect(r, box, rad - in + 1, {0x0A, 0x0B, 0x10, 255});
      drawFit(r, g.cover, box);
    } else {
      // sem capa: cartão com a cor do console e a inicial do jogo
      const SDL_Color cc = consoleColor(g.console);
      fillRoundRect(r, box, rad - in + 1, alpha(cc, 60));
      std::string ini = g.title.empty() ? "?" : g.title.substr(0, 1);
      for (char& ch : ini) ch = static_cast<char>(SDL_toupper(static_cast<unsigned char>(ch)));
      const float ip = box.h * 0.42f;
      drawText(r, box.x + (box.w - textWidth(ini, ip, true)) / 2, box.y + (box.h - text::lineHeight(ip)) / 2 - box.h * 0.05f, ini, ip, alpha(cc, 230), true);
      const float lp = std::max(10.0f * dp_, 11 * u);
      drawText(r, box.x + (box.w - textWidth(g.console, lp, true)) / 2, box.y + box.h - text::lineHeight(lp) - 6 * u, g.console, lp, alpha(cc, 220), true);
    }
    if (g.favorite) {
      fillCircle(r, box.x + box.w - 15 * u, box.y + 15 * u, 11 * u, {0, 0, 0, 150});
      fillStar(r, box.x + box.w - 15 * u, box.y + 15 * u, 7.5f * u, color::STAR);
    }
    // título e console embaixo da capa, centrados na faixa que sobra
    const float tp = std::max(12.5f * dp_, 14 * u), lp = std::max(11.0f * dp_, 11.5f * u);
    const float textH = text::lineHeight(tp) + 2 * u + text::lineHeight(lp);
    const float ty = box.y + box.h + (c.y + c.h - (box.y + box.h) - textH) / 2;
    drawText(r, c.x + 10 * u, ty, text::fit(g.title, tp, c.w - 20 * u, text::BOLD), tp, color::TEXT, true);
    const float ly = ty + text::lineHeight(tp) + 2 * u;
    fillCircle(r, c.x + 13 * u, ly + text::lineHeight(lp) / 2, 3.5f * u, consoleColor(g.console));
    drawText(r, c.x + 22 * u, ly, text::fit(g.console, lp, c.w - 30 * u), lp, color::MUTED);
    if (showFocus && static_cast<int>(k) == focus) strokeRoundRect(r, c, rad, 3 * dp_, color::PRIMARY);
  }
  if (games.empty()) {
    const float y = content_.y + sectionY_ + 44 * dp_ - scroll_;
    const float cx = (left_ + right_) / 2;
    const std::string a = "Nenhum jogo aqui ainda";
    const std::string b = desktopDrop ? "Clique em + Abrir ou arraste um jogo para esta janela" : "Toque em + Abrir para escolher um jogo";
    drawText(r, cx - textWidth(a, 17 * dp_, true) / 2, y, a, 17 * dp_, color::TEXT, true);
    const std::string bf = text::fit(b, 13 * dp_, right_ - left_);
    drawText(r, cx - textWidth(bf, 13 * dp_) / 2, y + 30 * dp_, bf, 13 * dp_, color::MUTED);
  }
  SDL_SetRenderClipRect(r, nullptr);
  if (!hint.empty()) {
    const float px = 12 * dp_;
    const std::string t = text::fit(hint, px, right_ - left_);
    drawText(r, (left_ + right_ - textWidth(t, px)) / 2, bottom_ - 24 * dp_, t, px, color::MUTED);
  }
}

}  // namespace indigo::ui
