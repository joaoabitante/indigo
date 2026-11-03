// Teste do uso no app (sem janela): botão do disco do Famicom Disk System e teclado numérico do 5200 e do
// Intellivision nos controles de toque (src/app/ui.cpp), e a lista de consoles da estante/biblioteca e do
// diálogo de abrir (src/app/consoles.h). Com o caminho do AndroidManifest.xml como argumento, confere também os
// tipos de arquivo do "Abrir com" do Android.
// uso: indigo-t_app_uso [native/android/app/src/main/AndroidManifest.xml]
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "consoles.h"
#include "core/core.h"
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
      {"x.tgc", "GAMECOM"}, {"x.j64", "JAGUAR"}, {"x.jag", "JAGUAR"}, {"x.abs", "JAGUAR"}, {"x.duck", "DUCK"},
      {"x.md1", "DUCK"}, {"x.sv", "SV"}, {"x.sgb", "GB"}, {"x.32x", "32X"}, {"x.rom", "MSX"}};
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
      {"gnw", "G&W"}, {"svision", "SV"}, {"duck", "DUCK"}, {"pcfx", "PCFX"}, {"saturn", "SATURN"}, {"gb", "GB"},
      {"cps2", "ARCADE"}, {"sgx", "PCE"}, {"ngpc", "NGP"}, {"wsc", "WS"}, {"segacd", "CD"}};
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
  for (const char* e : {"fds", "qd", "a52", "int", "itv", "tgc", "j64", "jag", "abs", "duck", "sv", "sgb", "zip", "cue", "chd",
                        "iso", "bin", "exe", "indigo"})
    if (p.find(std::string(";") + e + ";") == std::string::npos) { ok = false; ruim += std::string(e) + " "; }
  check(ok, "diálogo de abrir aceita as extensões dos consoles novos (o Jaguar tinha ficado de fora)" + (ruim.empty() ? "" : ": falta " + ruim));
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
  if (argc > 1) testarManifesto(argv[1]);
  std::printf("RESULTADO app_uso: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
