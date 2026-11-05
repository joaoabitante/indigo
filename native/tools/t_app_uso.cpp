// Teste do uso no app (sem janela): botão do disco do Famicom Disk System e teclado numérico do 5200 e do
// Intellivision nos controles de toque (src/app/ui.cpp), e a lista de consoles da estante/biblioteca e do
// diálogo de abrir (src/app/consoles.h), e quem comanda o jogador 2 (src/app/ui.cpp). Com o caminho do
// AndroidManifest.xml como argumento, confere também os tipos de arquivo do "Abrir com" do Android.
// uso: indigo-t_app_uso [native/android/app/src/main/AndroidManifest.xml]
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "consoles.h"
#include "core/core.h"
#include "nes/nes.h"
#include "ui.h"

using namespace indigo;

static int checks = 0, fails = 0;
static void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) fails++;
  std::printf("%s %s\n", ok ? "ok  " : "FALHA", what.c_str());
}

namespace {

struct Tela { const char* nome; int w, h; };
const Tela TELAS[] = {{"celular deitado", 1280, 720}, {"celular em pé", 720, 1280}, {"tablet deitado", 1024, 768},
                      {"tablet em pé", 768, 1024}};

void montar(ui::TouchControls& t, const Tela& s, bool lr, bool xy) {
  t.m.dp = 1;
  t.m.touch = true;
  t.m.safe = {};
  t.layout(s.w, s.h, s.h > s.w, lr, xy);
}

// toca o controle com esse rótulo com o dedo `id`; devolve a ação (None se o rótulo não está na tela)
ui::TouchAction tocar(ui::TouchControls& t, const std::string& label, SDL_FingerID id) {
  float x, y;
  if (!t.find(label, x, y)) return ui::TouchAction::None;
  return t.fingerDown(id, x, y);
}

uint32_t teclaDe(ui::TouchControls& t, const std::string& label) {
  t.clear();
  const auto a = tocar(t, label, 1);
  const uint32_t k = a == ui::TouchAction::Control ? t.keys() : 0xFFFFFFFFu;
  t.fingerUp(1);
  return k;
}

void testarDisco() {
  for (const Tela& s : TELAS) {
    ui::TouchControls t;
    t.disk = true;
    montar(t, s, false, false);
    const std::string onde = std::string(" (") + s.nome + ")";
    float x, y;
    check(t.find("Disco", x, y) && x > 0 && y > 0 && x < s.w && y < s.h, "FDS: botão \"Disco\" na tela" + onde);
    check(tocar(t, "Disco", 1) == ui::TouchAction::Disk && t.keys() == 0, "FDS: tocar \"Disco\" troca o lado e não aperta tecla" + onde);
    t.clear();
    check(teclaDe(t, "A") == KEY_A && teclaDe(t, "B") == KEY_B && teclaDe(t, "Start") == KEY_START &&
              teclaDe(t, "Select") == KEY_SELECT,
          "FDS: A, B, Start e Select continuam no lugar" + onde);
    ui::TouchControls n;
    montar(n, s, false, false);
    check(!n.find("Disco", x, y) && !n.find("123", x, y), "NES comum: sem \"Disco\" e sem teclado" + onde);
  }
}

void testarTeclado(int tipo) {
  const char* nome = tipo == 1 ? "5200" : "Intellivision";
  const char* star = tipo == 1 ? "*" : "Clear";
  const char* hash = tipo == 1 ? "#" : "Enter";
  for (const Tela& s : TELAS) {
    const std::string onde = std::string(" (") + nome + ", " + s.nome + ")";
    ui::TouchControls t;
    t.keypad = tipo;
    const bool lr = tipo == 1, xy = true;  // como o app: o 5200 tem L/R; os dois têm X e Y
    montar(t, s, lr, xy);
    float x, y;
    check(t.find("123", x, y) && !t.find("5", x, y) && !t.keypadOpen(), "botão \"123\" e teclado fechado no começo" + onde);
    check(tocar(t, "123", 1) == ui::TouchAction::Control && t.keypadOpen() && t.keys() == 0, "\"123\" abre o teclado sem apertar tecla" + onde);
    t.fingerUp(1);
    montar(t, s, lr, xy);
    // as 12 teclas: cada uma dá só o seu bit; todas dentro da tela
    bool todas = true, dentro = true;
    std::string ruim;
    for (int n = 0; n <= 9; n++) {
      const std::string l = std::to_string(n);
      if (teclaDe(t, l) != (KEY_KP0 << n)) { todas = false; ruim += l + " "; }
      if (!t.find(l, x, y) || x < 30 || y < 20 || x > s.w - 30 || y > s.h - 20) dentro = false;
    }
    if (teclaDe(t, star) != KEY_KPSTAR) { todas = false; ruim += star; }
    if (teclaDe(t, hash) != KEY_KPHASH) { todas = false; ruim += hash; }
    check(todas, std::string("teclas 0-9, ") + star + " e " + hash + " dão KEY_KP0+n, KEY_KPSTAR e KEY_KPHASH" + onde +
                     (ruim.empty() ? "" : " erradas: " + ruim));
    check(dentro, "teclado inteiro dentro da tela" + onde);
    // os controles de sempre continuam valendo com o teclado aberto
    check(teclaDe(t, "A") == KEY_A && teclaDe(t, "B") == KEY_B && teclaDe(t, "X") == KEY_X && teclaDe(t, "Y") == KEY_Y &&
              teclaDe(t, "Start") == KEY_START && teclaDe(t, "Select") == KEY_SELECT,
          "A, B, X, Y, Start e Select valem com o teclado aberto" + onde);
    // dois dedos: direcional e tecla juntos; fechar o teclado solta a tecla
    t.clear();
    tocar(t, "7", 2);
    check(t.keys() == (KEY_KP0 << 7), "tecla 7 apertada" + onde);
    tocar(t, "123", 3);
    t.fingerUp(3);
    check(!t.keypadOpen() && t.keys() == 0, "\"123\" fecha o teclado e solta a tecla que estava apertada" + onde);
    montar(t, s, lr, xy);
    check(!t.find("7", x, y) && t.find("123", x, y), "teclado fechado: as teclas somem" + onde);
    // trocar para um console sem teclado fecha (o próximo jogo de 5200 começa fechado)
    tocar(t, "123", 4);
    t.fingerUp(4);
    t.keypad = 0;
    montar(t, s, false, false);
    check(!t.keypadOpen() && !t.find("123", x, y), "console sem teclado: sem \"123\" e teclado fechado" + onde);
  }
}

void testarConsoles() {
  // extensões dos consoles novos da 2.4 e as abas
  struct { const char* arq; const char* aba; } casos[] = {
      {"Jogo.fds", "FDS"}, {"jogo.QD", "FDS"}, {"x.a52", "5200"}, {"x.int", "INTV"}, {"x.itv", "INTV"},
      {"x.tgc", "GAMECOM"}, {"x.nds", "NDS"}, {"x.j64", "JAGUAR"}, {"x.jag", "JAGUAR"}, {"x.abs", "JAGUAR"}, {"x.duck", "DUCK"},
      {"x.md1", "DUCK"}, {"x.sv", "SV"}, {"x.sgb", "GB"}, {"x.32x", "32X"}, {"x.rom", "MSX"},
      {"x.z64", "N64"}, {"x.N64", "N64"}, {"x.v64", "N64"}, {"jogo.GDI", "DC"}};
  bool ok = true;
  std::string ruim;
  for (const auto& c : casos)
    if (app::tabOfExt(app::extOf(c.arq)) != c.aba || !app::shelfExtension(c.arq)) { ok = false; ruim += std::string(c.arq) + " "; }
  check(ok, "extensões dos consoles novos (FDS, 5200, Intellivision, Game.com, Jaguar, Mega Duck, Supervision) vão para a aba certa" +
                (ruim.empty() ? "" : ": " + ruim));
  check(!app::shelfExtension("x.bin") && !app::shelfExtension("x.exe") && !app::shelfExtension("leia.txt") &&
            !app::shelfExtension("pasta.fds/arquivo") && app::extOf("a/b.c/d") == "",
        "genéricos (.bin, .exe) e outros arquivos não entram sozinhos na estante");
  // núcleos → aba
  struct { const char* sys; const char* aba; } nucleos[] = {
      {"fds", "FDS"}, {"a5200", "5200"}, {"intv", "INTV"}, {"gamecom", "GAMECOM"}, {"jaguar", "JAGUAR"},
      {"gnw", "G&W"}, {"svision", "SV"}, {"duck", "DUCK"}, {"pcfx", "PCFX"}, {"saturn", "SATURN"}, {"dc", "DC"}, {"gb", "GB"},
      {"cps2", "ARCADE"}, {"sgx", "PCE"}, {"ngpc", "NGP"}, {"wsc", "WS"}, {"segacd", "CD"}, {"n64", "N64"}};
  ok = true;
  ruim.clear();
  for (const auto& n : nucleos) if (app::tabOfSystem(n.sys) != n.aba) { ok = false; ruim += std::string(n.sys) + " "; }
  check(ok, "cada núcleo (inclusive os da 2.4) cai na aba certa ao ser lembrado" + (ruim.empty() ? "" : ": " + ruim));
  check(app::tabOfSystem("inexistente") == "outros", "núcleo desconhecido vai para \"outros\"");
  // toda aba usada está na ordem das abas, uma vez só, e tem cor própria
  std::set<std::string> ordem, vistas;
  bool repetida = false;
  for (const char* t : app::TAB_ORDER) repetida |= !ordem.insert(t).second;
  for (const auto& e : app::EXT_TABS) vistas.insert(e.tab);
  for (const auto& n : nucleos) vistas.insert(n.aba);
  for (const char* t : {"PS1", "3DO", "G&W"}) vistas.insert(t);
  ok = !repetida;
  ruim.clear();
  for (const auto& v : vistas) if (!ordem.count(v)) { ok = false; ruim += v + " "; }
  check(ok, "toda aba usada aparece na ordem das abas, sem repetir" + (ruim.empty() ? "" : ": falta " + ruim));
  ok = true;
  ruim.clear();
  std::set<uint32_t> cores;
  for (const char* t : app::TAB_ORDER) {
    if (std::string(t) == "outros") continue;
    const SDL_Color c = ui::consoleColor(t), m = ui::color::MUTED;
    if (c.r == m.r && c.g == m.g && c.b == m.b) { ok = false; ruim += std::string(t) + " "; }
    cores.insert(static_cast<uint32_t>(c.r) << 16 | c.g << 8 | c.b);
  }
  check(ok && cores.size() == std::size(app::TAB_ORDER) - 1,
        "cada console tem cor própria na capa sem imagem (" + std::to_string(cores.size()) + " cores)" + (ruim.empty() ? "" : "; sem cor: " + ruim));
  // diálogo de abrir do PC: todas as extensões, com as que só abrem (.bin, .exe) e o backup .indigo
  const std::string p = ";" + app::dialogPattern() + ";";
  ok = true;
  ruim.clear();
  for (const char* e : {"fds", "qd", "a52", "int", "itv", "tgc", "nds", "j64", "jag", "abs", "z64", "n64", "v64", "duck", "sv", "sgb", "zip", "cue", "chd", "gdi",
                        "iso", "bin", "exe", "indigo"})
    if (p.find(std::string(";") + e + ";") == std::string::npos) { ok = false; ruim += std::string(e) + " "; }
  check(ok, "diálogo de abrir aceita as extensões dos consoles novos (o Jaguar tinha ficado de fora)" + (ruim.empty() ? "" : ": falta " + ruim));
}

// dois jogadores: quem vai para cada jogador (src/app/ui.cpp, routeKeys) e o caminho até a porta 2 de um NES
void testarJogadores() {
  using ui::Player2;
  const uint32_t A = KEY_A, B = KEY_B, S = KEY_START, X = KEY_X, U = KEY_UP;
  auto ok = [](ui::PlayerKeys k, uint32_t p1, uint32_t p2) { return k.p1 == p1 && k.p2 == p2; };
  check(ok(ui::routeKeys(Player2::SecondPad, 2, A, 0, {B, S}), A | B, S), "2 jogadores: 2º controle no jogador 2, teclado e 1º controle no 1");
  check(ok(ui::routeKeys(Player2::SecondPad, 2, A, 0, {B}), A | B, 0), "2 jogadores: um controle + teclado: os dois no jogador 1 (como antes)");
  check(ok(ui::routeKeys(Player2::SecondPad, 2, 0, 0, {B, S, X}), B | X, S), "2 jogadores: 3º controle volta para o jogador 1");
  check(ok(ui::routeKeys(Player2::Keyboard, 2, A, U, {B}), B | U, A), "2 jogadores (opção teclado): controle e toque no 1, teclado no 2");
  check(ok(ui::routeKeys(Player2::Keyboard, 2, A, 0, {}), A, 0), "2 jogadores (opção teclado): sem controle, o teclado é o jogador 1");
  check(ok(ui::routeKeys(Player2::Off, 2, A, 0, {B, S}), A | B | S, 0), "2 jogadores desligado: tudo no jogador 1");
  check(ok(ui::routeKeys(Player2::SecondPad, 1, A, 0, {B, S}), A | B | S, 0), "console de 1 jogador: tudo no jogador 1");
  check(ok(ui::routeKeys(Player2::SecondPad, 2, 0, U, {B, S}), U | B, S), "2 jogadores: a tela de toque é sempre o jogador 1");
  check(ui::padPlayer(Player2::SecondPad, 2, 1) == 1 && ui::padPlayer(Player2::SecondPad, 2, 0) == 0 &&
            ui::padPlayer(Player2::Off, 2, 1) == 0 && ui::padPlayer(Player2::SecondPad, 1, 1) == 0,
        "2 jogadores: padPlayer");
  check(!ui::player2Present(Player2::SecondPad, 2, 1) && ui::player2Present(Player2::SecondPad, 2, 2) &&
            ui::player2Present(Player2::Keyboard, 2, 1) && !ui::player2Present(Player2::Keyboard, 2, 0) &&
            !ui::player2Present(Player2::Off, 2, 3) && !ui::player2Present(Player2::SecondPad, 1, 2),
        "2 jogadores: o controle 2 do console só liga quando alguém comanda o jogador 2");
  const std::string l0 = ui::player2Label(Player2::Off), l1 = ui::player2Label(Player2::SecondPad), l2 = ui::player2Label(Player2::Keyboard);
  check(l0 != l1 && l1 != l2 && l0 != l2 && l1.find("Jogador 2") == 0, "2 jogadores: texto da opção no menu Controles");
  // de ponta a ponta num NES: 2 controles, o 2º aperta Start e chega só ao $4017
  std::vector<uint8_t> rom = {'N', 'E', 'S', 0x1A, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  rom.resize(16 + 0x4000 + 0x2000, 0xEA);
  auto core = createCore(rom, "t.nes", nullptr);
  auto* n = dynamic_cast<nes::NESCore*>(core.get());
  check(n && core->players() == 2, "2 jogadores: NES aceita 2 jogadores");
  if (!n) return;
  const auto k = ui::routeKeys(Player2::SecondPad, core->players(), 0, 0, {A, S});
  core->setKeys(k.p1);
  core->setKeys2(k.p2);
  auto port = [&](uint16_t a) {
    n->nes().write(0x4016, 1);
    n->nes().write(0x4016, 0);
    int v = 0;
    for (int i = 0; i < 8; i++) v |= (n->nes().read(a) & 1) << i;
    return v;
  };
  check(port(0x4016) == 0x01 && port(0x4017) == 0x08, "2 jogadores: NES recebe A no controle 1 e Start no controle 2");
}

// dois controles de verdade pelo SDL (controles virtuais: o SDL os trata como qualquer controle, Bluetooth ou
// cabo, cada um com seu número). Mesmo caminho do app: SDL_EVENT_GAMEPAD_ADDED/REMOVED, lista com lugar fixo
// (ui::padSlotAdd/padSlotRemove) e as teclas de cada um em routeKeys.
void testarDoisControles() {
  using ui::Player2;
  // a lista com lugar fixo, sem SDL
  {
    int a = 1, b = 2, c = 3;
    std::vector<int*> v;
    const size_t sa = ui::padSlotAdd(v, &a), sb = ui::padSlotAdd(v, &b);
    ui::padSlotRemove(v, &a);
    const bool vaga = v.size() == 2 && !v[0] && v[1] == &b;
    const size_t sc = ui::padSlotAdd(v, &c);
    ui::padSlotRemove(v, &b);
    check(sa == 0 && sb == 1 && vaga && sc == 0 && v.size() == 1 && v[0] == &c,
          "2 controles: quem cai deixa a vaga e quem volta ocupa a mesma posição");
  }
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  if (!SDL_Init(SDL_INIT_GAMEPAD)) { check(false, std::string("2 controles: SDL_Init: ") + SDL_GetError()); return; }
  std::vector<SDL_Gamepad*> pads;
  auto eventos = [&] {
    SDL_UpdateJoysticks();
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_GAMEPAD_ADDED) {
        if (SDL_Gamepad* p = SDL_OpenGamepad(ev.gdevice.which)) ui::padSlotAdd(pads, p);
      } else if (ev.type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (auto* p : pads)
          if (p && SDL_GetGamepadID(p) == ev.gdevice.which) { SDL_CloseGamepad(p); ui::padSlotRemove(pads, p); break; }
      }
    }
  };
  auto liga = [](const char* nome) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.name = nome;
    return SDL_AttachVirtualJoystick(&d);
  };
  // aperta um botão no controle virtual id
  auto aperta = [](SDL_JoystickID id, SDL_GamepadButton b, bool down) {
    if (SDL_Joystick* j = SDL_GetJoystickFromID(id)) SDL_SetJoystickVirtualButton(j, b, down);
  };
  // teclas de cada posição (como gamepadKeys do app: A = botão de baixo, Start = Start)
  auto teclas = [&] {
    std::vector<uint32_t> k(pads.size(), 0);
    for (size_t i = 0; i < pads.size(); i++) {
      if (!pads[i]) continue;
      if (SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_SOUTH)) k[i] |= KEY_A;
      if (SDL_GetGamepadButton(pads[i], SDL_GAMEPAD_BUTTON_START)) k[i] |= KEY_START;
    }
    return k;
  };
  const SDL_JoystickID c1 = liga("Controle Bluetooth 1");
  eventos();
  const SDL_JoystickID c2 = liga("Controle Bluetooth 2");
  eventos();
  check(c1 && c2 && c1 != c2 && pads.size() == 2 && pads[0] && pads[1] && SDL_GetGamepadID(pads[0]) == c1 &&
            SDL_GetGamepadID(pads[1]) == c2,
        "2 controles: o SDL mostra dois controles separados, na ordem em que conectaram");
  aperta(c1, SDL_GAMEPAD_BUTTON_SOUTH, true);
  aperta(c2, SDL_GAMEPAD_BUTTON_START, true);
  eventos();
  auto k = ui::routeKeys(Player2::SecondPad, 2, 0, 0, teclas());
  check(k.p1 == KEY_A && k.p2 == KEY_START, "2 controles: o A do 1º vai ao jogador 1 e o Start do 2º ao jogador 2");
  // o controle 1 cai (bateria, distância): o 2º continua no jogador 2 e o teclado fica com o jogador 1
  SDL_DetachVirtualJoystick(c1);
  eventos();
  k = ui::routeKeys(Player2::SecondPad, 2, KEY_B, 0, teclas());
  check(pads.size() == 2 && !pads[0] && k.p1 == KEY_B && k.p2 == KEY_START,
        "2 controles: o 1º desconecta e o 2º continua no jogador 2");
  const SDL_JoystickID c1b = liga("Controle Bluetooth 1");
  eventos();
  aperta(c1b, SDL_GAMEPAD_BUTTON_SOUTH, true);
  eventos();
  k = ui::routeKeys(Player2::SecondPad, 2, 0, 0, teclas());
  check(pads.size() == 2 && pads[0] && SDL_GetGamepadID(pads[0]) == c1b && k.p1 == KEY_A && k.p2 == KEY_START,
        "2 controles: o 1º reconecta e volta ao jogador 1");
  SDL_DetachVirtualJoystick(c2);
  eventos();
  check(pads.size() == 1 && pads[0], "2 controles: o 2º desconecta e a lista fica só com o 1º");
  for (auto* p : pads)
    if (p) SDL_CloseGamepad(p);
  SDL_DetachVirtualJoystick(c1b);
  SDL_Quit();
}

void testarManifesto(const char* caminho) {
  std::ifstream f(caminho);
  std::stringstream ss;
  ss << f.rdbuf();
  const std::string m = ss.str();
  check(!m.empty(), std::string("AndroidManifest.xml lido: ") + caminho);
  bool ok = true;
  std::string ruim;
  for (const char* t : {"application/octet-stream", "application/zip", "application/x-fds-disk", "application/x-genesis-32x-rom",
                        "application/x-sega-cd-rom", "application/x-saturn-rom", "application/x-mame-chd",
                        "application/x-iso9660-image", "application/x-cue", "application/x-raw-floppy-disk-image"})
    if (m.find(std::string("android:mimeType=\"") + t + "\"") == std::string::npos) { ok = false; ruim += std::string(t) + " "; }
  check(ok, "\"Abrir com\" do Android: tipos de ROM, CD e disco (sem tipo = octet-stream)" + (ruim.empty() ? "" : ": falta " + ruim));
  check(m.find("android:mimeType=\"*/*\"") == std::string::npos, "\"Abrir com\" do Android sem */* (não aparece para APK)");
}

}  // namespace

int main(int argc, char** argv) {
  testarDisco();
  testarTeclado(1);
  testarTeclado(2);
  testarConsoles();
  testarJogadores();
  testarDoisControles();
  if (argc > 1) testarManifesto(argv[1]);
  std::printf("RESULTADO app_uso: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
