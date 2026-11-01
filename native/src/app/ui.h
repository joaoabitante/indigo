// Interface do Índigo: biblioteca de jogos (tela inicial), menus, controles na tela e formas arredondadas.
// Desenha só com o SDL_Renderer; o texto é liso (text.h, fonte Poppins). Funciona com toque, mouse, teclado e
// controle; no celular em pé os menus sobem de baixo ("bottom sheet").
#pragma once
#include <SDL3/SDL.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace indigo::ui {

// cores do tema
namespace color {
constexpr SDL_Color BG{0x0E, 0x0F, 0x14, 255};
constexpr SDL_Color SURFACE{0x18, 0x1A, 0x24, 255};
constexpr SDL_Color SURFACE2{0x22, 0x25, 0x33, 255};
constexpr SDL_Color PRIMARY{0x7C, 0x6C, 0xF2, 255};
constexpr SDL_Color TEXT{0xF1, 0xF2, 0xF6, 255};
constexpr SDL_Color MUTED{0x9A, 0xA0, 0xB4, 255};
constexpr SDL_Color STAR{0xF5, 0xC4, 0x51, 255};
}  // namespace color

// mantido por compatibilidade: a fonte nova tem os acentos, então o texto passa quase igual
std::string toAscii(const std::string& utf8);

// texto com altura `px`, canto superior esquerdo da linha em (x, y)
void drawText(SDL_Renderer* r, float x, float y, const std::string& text, float px, SDL_Color c, bool bold = false);
float textWidth(const std::string& text, float px, bool bold = false);
void fillCircle(SDL_Renderer* r, float cx, float cy, float radius, SDL_Color c);
void strokeCircle(SDL_Renderer* r, float cx, float cy, float radius, float width, SDL_Color c);
void fillRoundRect(SDL_Renderer* r, const SDL_FRect& rc, float radius, SDL_Color c);
void strokeRoundRect(SDL_Renderer* r, const SDL_FRect& rc, float radius, float width, SDL_Color c);
// cor de cada console (capas sem imagem, etiquetas)
SDL_Color consoleColor(const std::string& console);

// medidas da tela, iguais para todas as partes da interface. As medidas da interface são em "dp" (ponto
// independente da densidade, ~1/160 de polegada no celular; 1 pixel numa tela de 100% no PC): dp diz quantos
// pixels reais vale cada um. safe: área segura em pixels (sem o entalhe da câmera e a barra de gestos); vazia
// = a tela toda. touch: interface de toque (alvos maiores; abas roláveis em vez de quebradas em linhas).
struct Metrics {
  float dp = 1;
  SDL_FRect safe{};
  bool touch = false;
  SDL_FRect area(int ow, int oh) const {
    return safe.w > 0 && safe.h > 0 ? safe : SDL_FRect{0, 0, static_cast<float>(ow), static_cast<float>(oh)};
  }
};

// ------------------------------------------------------------ controles na tela
enum class TouchAction { None, Menu, Control };
// botões de toque que não existem em core.h (L2 e R2 do PlayStation): o app os entrega ao núcleo do PS1
constexpr uint32_t TOUCH_L2 = 1u << 24, TOUCH_R2 = 1u << 25;

class TouchControls {
public:
  Metrics m;
  // unidade de tamanho dos controles: a altura (deitado) ou a largura (em pé) de um celular comum, em pixels.
  // Num tablet ou tela grande ela para de crescer: os botões ficam do tamanho do dedo, não da tela.
  static float unit(int ow, int oh, bool portrait, const Metrics& m);
  // deitado: largura que os controles ocupam de cada lado (o app tenta deixar a imagem do jogo fora dela)
  static float sideWidth(int ow, int oh, const Metrics& m) { return 0.46f * unit(ow, oh, false, m); }
  // em pé: altura que os controles ocupam embaixo
  static float bottomHeight(int ow, int oh, const Metrics& m) { return 0.92f * unit(ow, oh, true, m); }
  // portrait: celular em pé (controles embaixo do jogo); deitado: dos lados
  // lr: mostra L e R; xy: A, B, X e Y em losango (SNES). game: onde está a imagem do jogo (deitado: os controles
  // ficam no meio da faixa livre de cada lado quando ela é larga)
  // ps1: losango com os símbolos do PlayStation (Triângulo, Círculo, Xis, Quadrado), L1/R1 e L2/R2
  // tdo: os três botões do 3DO numa fileira (A = Y do Índigo, B = A, C = B), P (Start) e X (Select)
  void layout(int ow, int oh, bool portrait, bool lr = true, bool xy = false, SDL_FRect game = {}, bool ps1 = false,
              bool tdo = false);
  TouchAction fingerDown(SDL_FingerID id, float x, float y);
  void fingerMove(SDL_FingerID id, float x, float y);
  void fingerUp(SDL_FingerID id);
  void clear() { fingers_.clear(); }
  uint32_t keys() const;
  bool turbo() const;
  void draw(SDL_Renderer* r) const;

private:
  enum Kind { DPAD, ROUND, PILL };
  enum Special { NONE, MENU, FAST };
  struct Ctl {
    Kind kind;
    float cx, cy, w, h;  // centro; ROUND/DPAD: w = raio; PILL: largura × altura
    const char* label;
    uint32_t key;
    Special special;
  };
  struct Finger { int ctl; uint32_t dir; };
  int hit(float x, float y) const;
  uint32_t dpadDir(const Ctl& c, float x, float y) const;
  bool pressed(int idx) const;

  static constexpr const char* PS_TRIANGLE = "\x01";  // rótulos que viram os símbolos do PlayStation
  static constexpr const char* PS_CIRCLE = "\x02";
  static constexpr const char* PS_CROSS = "\x03";
  static constexpr const char* PS_SQUARE = "\x04";
  bool drawPsSymbol(SDL_Renderer* r, const char* label, float cx, float cy, float s, float w) const;
  std::vector<Ctl> ctls_;
  std::map<SDL_FingerID, Finger> fingers_;
  float minHit_ = 24;  // meia altura mínima da área de toque das pílulas (24 dp: alvo de 48 dp)
};

// ------------------------------------------------------------ menu (toque, controle ou teclado)
class Menu {
public:
  Metrics m;
  struct Item { std::string text; int id; };
  std::string title;
  std::string hint;                // linha de ajuda embaixo do painel (opcional)
  std::vector<Item> items;
  int sel = 0;
  SDL_Texture* preview = nullptr;  // imagem ao lado do menu (miniatura do estado salvo), opcional
  bool sheet = false;              // celular em pé: o painel sobe de baixo e ocupa a largura toda

  void move(int d);
  int selectedId() const { return items.empty() ? -1 : items[sel].id; }
  // id do item tocado; -1 dentro do painel mas fora dos itens; -2 fora do painel
  int hit(float x, float y) const;
  void scrollBy(float dy);
  void draw(SDL_Renderer* r, int ow, int oh, bool dim);

private:
  void layout(int ow, int oh);
  SDL_FRect panel_{}, previewRect_{}, list_{};
  std::vector<SDL_FRect> rects_;  // posição dos itens sem a rolagem
  float textPx_ = 16, rowH_ = 40, scroll_ = 0, contentH_ = 0, dp_ = 1;
  int lastSel_ = -1;
};

// ------------------------------------------------------------ biblioteca (tela inicial)
class Library {
public:
  Metrics m;
  struct Game {
    std::string title, console;  // console: "NES", "GBA"...
    SDL_Texture* cover = nullptr;
    bool favorite = false;
    int ref = -1;                // índice do jogo para o app
  };
  std::vector<std::string> tabs;  // "Todos", "Favoritos", consoles
  int tab = 0;
  std::vector<Game> games;        // já filtrados pela aba
  bool hasContinue = false;
  Game cont;                      // "Continuar"
  std::string hint;               // ajuda de teclado/controle (vazio no toque)
  bool desktopDrop = false;       // PC: mostra "arraste um jogo para cá" quando vazio
  int focus = 0;                  // -1 = Continuar; 0.. = jogos
  bool showFocus = true;          // contorno no jogo escolhido (teclado/controle; no toque some)

  enum Kind { NONE, OPEN, SETTINGS, TAB, CONTINUE, GAME };
  struct Hit { Kind kind = NONE; int index = -1; };
  Hit hit(float x, float y) const;
  void navigate(int dx, int dy);  // direcional do controle / setas
  void scrollBy(float dy);
  // arrastar começando em (x, y) mexe nas abas (rolagem para os lados) e não na lista?
  bool inTabs(float x, float y) const;
  void scrollTabs(float dx);
  void resetScroll() { scroll_ = 0; }
  void draw(SDL_Renderer* r, int ow, int oh);

private:
  void layout(int ow, int oh);
  void ensureVisible();
  float dp_ = 1, scroll_ = 0, contentH_ = 0, tabScroll_ = 0, tabsW_ = 0;
  int cols_ = 2, lastTab_ = -1;
  bool tabsScroll_ = false;  // as abas não cabem numa linha e rolam para os lados (toque)
  SDL_FRect top_{}, openBtn_{}, settingsBtn_{}, content_{}, contRect_{}, tabsBox_{};
  std::vector<SDL_FRect> tabRects_, cards_;  // cards_: sem a rolagem
  float sectionY_ = 0, left_ = 0, right_ = 0, bottom_ = 0;  // left_/right_: margens do conteúdo (centralizado)
  int W_ = 0, H_ = 0;
};

}  // namespace indigo::ui
