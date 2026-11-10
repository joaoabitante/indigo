// Índigo para PC e Android: janela, som e controles com SDL3 em volta dos núcleos de src/.
//
// Teclado (igual à versão web): setas = direcional, X = A, Z = B, A = L, S = R, Enter = Start,
// Backspace/Shift direito = Select, Espaço (segurar) = acelerar 4×. Master System: Z = botão 1, X = botão 2,
// Enter = Pause; Game Gear: Enter = Start; Neo Geo Pocket: X = A, Z = B, Enter ou Backspace = Option.
// Game Boy Advance: usa a BIOS própria do Índigo; se o arquivo gba_bios.bin (16 KB, a BIOS original do seu
// aparelho) estiver na pasta de dados, ela é usada. Abrir esse arquivo no Índigo copia ele para lá.
// Atalhos: Ctrl+O abrir jogo · F5 salvar estado · F8 carregar estado · 1–9 escolher espaço de estado ·
// P pausar · Ctrl+R reiniciar · F3 trocar paleta do Game Boy · F4 trocar o lado do disco (Famicom Disk System) · F11 ou Alt+Enter tela cheia · Esc menu.
// Teclado numérico do PC = teclado do controle do Atari 5200 e do Intellivision (* e # = Clear e Enter).
// Controle: botão Guia (ou Start + Select juntos) abre o menu. No celular: controles na tela (somem quando há um
// controle ligado), botão MENU ou "voltar" do Android; "Disco" troca o lado no Famicom Disk System e "123" abre o
// teclado numérico do 5200 e do Intellivision.
//
// Tela inicial: "Continuar de onde parou" (estado automático gravado ao fechar o jogo), abrir jogo e os últimos
// jogos abertos. Arquivos .zip são descompactados na hora. Cada save guarda versões anteriores (no máximo uma a
// cada 10 minutos, as 12 mais recentes), que podem ser restauradas pelo menu.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/core.h"
#include "core/backup.h"
#include "core/safe.h"
#include "cloud.h"
#include "core/zip.h"
#include "files.h"
#include "platform.h"
#include "consoles.h"
#include "shelf.h"
#include "neogeo/neogeo.h"
#include "pcecd/pcecd.h"
#include "gb/gb.h"
#include "vb/vb.h"
#include "a5200/a5200.h"
#include "vectrex/vectrex.h"
#include "intv/intv.h"
#include "jaguar/jaguar.h"
#include "neogeocd/neogeocd.h"
#include "ps1/ps1.h"
#include "tdo/tdo.h"
#include "fds/fds.h"
#include "gamecom/gamecom.h"
#include "nds/nds.h"
#include "pcfx/pcfx.h"
#include "saturn/saturn.h"
#include "dc/dc.h"
#include "cdrom/cdrom.h"
#include "segacd/segacd.h"
#ifdef INDIGO_WINDOW_ICON
#include "icon.h"  // gerado pelo CMake no Linux: indigo_icon0 (PNG 256x256)
#endif
#include "ui.h"
#include "text.h"
#include "video.h"
#include "novidades.h"

using indigo::Core;

namespace {

// ------------------------------------------------------------ paletas do Game Boy (as mesmas da versão web)
constexpr uint32_t hexc(uint32_t c) { return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF); }
struct Palette { const char* label; uint32_t bg[4]; uint32_t obj0[4]; uint32_t obj1[4]; bool hasObj; };
const Palette PALETTES[] = {
  {"Verde clássico", {hexc(0xE0F8D0), hexc(0x88C070), hexc(0x346856), hexc(0x081820)}, {}, {}, false},
  {"Pocket (cinza esverdeado)", {hexc(0xC4CFA1), hexc(0x8B956D), hexc(0x4D533C), hexc(0x1F1F1F)}, {}, {}, false},
  {"Tons de cinza", {hexc(0xFFFFFF), hexc(0xAAAAAA), hexc(0x555555), hexc(0x000000)}, {}, {}, false},
  {"Light (tela acesa)", {hexc(0x7CF0D0), hexc(0x3CBF9C), hexc(0x1A7A66), hexc(0x08332D)}, {}, {}, false},
  {"Índigo", {hexc(0xEEECFF), hexc(0xA9A1EE), hexc(0x5D50B8), hexc(0x19133F)}, {}, {}, false},
  {"Âmbar", {hexc(0xFFE9B8), hexc(0xE6A456), hexc(0x8C4B1E), hexc(0x2B1206)}, {}, {}, false},
  {"Colorida (estilo GBC)", {hexc(0xFFFFFF), hexc(0x7BFF31), hexc(0x0063C6), hexc(0x000000)},
   {hexc(0xFFFFFF), hexc(0xFF8484), hexc(0x943939), hexc(0x000000)},
   {hexc(0xFFFFFF), hexc(0x63A5FF), hexc(0x0000FF), hexc(0x000000)}, true},
};
constexpr int NUM_PALETTES = sizeof(PALETTES) / sizeof(PALETTES[0]);

constexpr int SAMPLE_RATE = 48000;
constexpr size_t MAX_RECENT = 12;
constexpr int HIST_MAX = 12;                     // versões guardadas por save (como na web)
constexpr int64_t HIST_EVERY_NS = 600000000000;  // no máximo uma versão automática a cada 10 min
Uint32 EVENT_OPEN_FILE = 0;                      // registrados depois do SDL_Init
Uint32 EVENT_SAVE_BACKUP = 0;
Uint32 EVENT_ADD_FOLDER = 0;

// páginas e itens do menu
enum Page { P_MAIN, P_SAVE_STATE, P_LOAD_STATE, P_HISTORY, P_BACKUP, P_CLOUD, P_PIN, P_CONTROLS, P_CAPTURE, P_SHELF, P_FOLDERS, P_ABOUT, P_SETTINGS };
constexpr size_t START_RECENT = 5;   // jogos recentes na tela inicial (o resto fica na estante)
constexpr int SHELF_PAGE = 8;        // jogos por página da estante

// ações que podem ser remapeadas (o direcional do controle fica fixo; no teclado tudo é remapeável)
enum Action { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_A, ACT_B, ACT_L, ACT_R, ACT_START, ACT_SELECT, ACT_X, ACT_Y, ACT_TURBO, NUM_ACTIONS };
const char* const ACTION_NAMES[NUM_ACTIONS] = {"Cima", "Baixo", "Esquerda", "Direita", "A", "B", "L", "R", "Start", "Select", "X (SNES)", "Y (SNES)", "Acelerar (segurar)"};
const char* const ACTION_KEYS[NUM_ACTIONS] = {"up", "down", "left", "right", "a", "b", "l", "r", "start", "select", "x", "y", "turbo"};
const uint32_t ACTION_BITS[NUM_ACTIONS] = {indigo::KEY_UP, indigo::KEY_DOWN, indigo::KEY_LEFT, indigo::KEY_RIGHT, indigo::KEY_A, indigo::KEY_B,
                                           indigo::KEY_L, indigo::KEY_R, indigo::KEY_START, indigo::KEY_SELECT, indigo::KEY_X, indigo::KEY_Y, 0};
constexpr const char* CLOUD_URL = "https://indigo-gba.vercel.app";  // o site do Índigo (a mesma nuvem dos saves)
constexpr uint64_t CLOUD_UPLOAD_DELAY = 20000;                       // sobe o save 20 s depois da última gravação
enum MenuId {
  M_RESUME = 1, M_SAVE_PAGE, M_LOAD_PAGE, M_FAST, M_RESET, M_PALETTE, M_HISTORY_PAGE, M_OPEN, M_CLOSE, M_QUIT,
  M_CONTINUE, M_BACK, M_BACKUP_PAGE, M_EXPORT_ROMS, M_EXPORT_SAVES, M_IMPORT,
  M_CLOUD_PAGE, M_CLOUD_SYNC, M_CLOUD_TOGGLE, M_CLOUD_PIN, M_PIN_DEL, M_PIN_OK, M_PIN_SHOW,
  M_FILTER, M_CONTROLS_PAGE, M_MAP_PAD, M_MAP_KEYS, M_MAP_DEFAULT, M_CAPTURE_SKIP, M_CAPTURE_CANCEL, M_CAPTURE_INFO,
  M_SHELF_PAGE, M_SHELF_CONSOLE, M_SHELF_NEXT, M_SHELF_PREV, M_FAV, M_COVER, M_FOLDERS_PAGE, M_FOLDER_ADD, M_ABOUT_PAGE, M_ABOUT_NEWER, M_ABOUT_OLDER, M_ASSOCIATE, M_START_FULL, M_VB_3D, M_INFO, M_SETTINGS_PAGE, M_TEST_SOUND,
  M_INTEGER, M_UI_SIZE, M_PS1_RES, M_DISK_SIDE, M_SGB, M_PLAYER2, M_DS_LAYOUT,
  M_PIN_DIGIT = 80, M_SLOT = 100, M_FOLDER_DEL = 900, M_SHELF_ITEM = 1000, M_RECENT = 200, M_VERSION = 300
};

// ------------------------------------------------------------ arquivos (UTF-8, funciona com acentos no Windows)
bool readFile(const std::string& path, std::vector<uint8_t>& out) {
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (!data) return false;
  const auto* p = static_cast<const uint8_t*>(data);
  out.assign(p, p + size);
  SDL_free(data);
  return true;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
  return SDL_SaveFile(path.c_str(), data.data(), data.size());
}

// copia em pedaços (CDs têm centenas de MB); funciona com os endereços content:// do Android
bool copyFileStream(const std::string& from, const std::string& to) {
  SDL_IOStream* in = SDL_IOFromFile(from.c_str(), "rb");
  if (!in) return false;
  const std::string tmp = to + ".parcial";
  SDL_IOStream* out = SDL_IOFromFile(tmp.c_str(), "wb");
  if (!out) { SDL_CloseIO(in); return false; }
  std::vector<uint8_t> buf(1 << 20);
  bool ok = true;
  for (;;) {
    const size_t n = SDL_ReadIO(in, buf.data(), buf.size());
    if (n == 0) { ok = SDL_GetIOStatus(in) == SDL_IO_STATUS_EOF; break; }
    if (SDL_WriteIO(out, buf.data(), n) != n) { ok = false; break; }
  }
  SDL_CloseIO(in);
  ok = SDL_CloseIO(out) && ok;
  if (ok) { SDL_RemovePath(to.c_str()); ok = SDL_RenamePath(tmp.c_str(), to.c_str()); }
  if (!ok) SDL_RemovePath(tmp.c_str());
  return ok;
}

// extensão em minúsculas, com o ponto ("" se não houver)
std::string extOf(const std::string& name) {
  const auto dot = name.find_last_of('.');
  const auto sl = name.find_last_of("/\\");
  if (dot == std::string::npos || (sl != std::string::npos && dot < sl)) return "";
  std::string e = name.substr(dot);
  for (char& c : e) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  return e;
}

bool fileTime(const std::string& path, SDL_Time& t) {
  SDL_PathInfo info{};
  if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return false;
  t = info.modify_time;
  return true;
}

// "21/10 17:45"
std::string shortDate(SDL_Time t) {
  SDL_DateTime d{};
  if (!SDL_TimeToDateTime(t, &d, true)) return "?";
  char b[32];
  SDL_snprintf(b, sizeof b, "%02d/%02d %02d:%02d", d.day, d.month, d.hour, d.minute);
  return b;
}

std::string baseName(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

// nome do arquivo com a extensão ("Sonic.gg"): é a chave dos saves e estados, para que Sonic.sms e Sonic.gg
// (ou Zelda.gb e Zelda.gbc) não dividam o mesmo save
std::string fileName(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string withoutExt(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  const auto dot = path.find_last_of('.');
  return (dot == std::string::npos || (slash != std::string::npos && dot < slash)) ? path : path.substr(0, dot);
}

// Intellivision: o .cfg do jzIntv ao lado do .bin traz o mapa de memória (o núcleo só o usa em .bin/.int/.itv).
// Vale só enquanto este objeto existe: ao sair, o firmware "intv_cfg" é limpo, para o .cfg de um jogo não
// mudar a identificação dos outros arquivos (.bin de outro console virando Intellivision)
struct IntvCfgBeside {
  explicit IntvCfgBeside(const std::string& path) {
    std::vector<uint8_t> cfg;
    if (!readFile(withoutExt(path) + ".cfg", cfg) || cfg.size() > 65536) cfg.clear();
    indigo::setFirmware("intv_cfg", std::move(cfg));
  }
  ~IntvCfgBeside() { indigo::setFirmware("intv_cfg", {}); }
  IntvCfgBeside(const IntvCfgBeside&) = delete;
  IntvCfgBeside& operator=(const IntvCfgBeside&) = delete;
};

// BIOS do Pokémon Mini (bios.min do aparelho do usuário): 4 KB e .min (os jogos têm pelo menos 8,5 KB)
bool isPmBios(const std::string& path, const std::vector<uint8_t>& data) {
  if (data.size() != 4096) return false;
  std::string n = fileName(path);
  for (char& c : n) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  return n.size() > 4 && n.compare(n.size() - 4, 4, ".min") == 0;
}

// BIOS do Atari 5200 (5200.rom do aparelho do usuário): 2 KB, nome com "5200"
bool isA5200Bios(const std::string& path, const std::vector<uint8_t>& data) {
  std::string n = fileName(path);
  for (char& c : n) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  return indigo::a5200::isA5200Bios(data, n);
}

// BIOS do ColecoVision (coleco.rom do aparelho do usuário): 8 KB, nome com "coleco"
bool isColecoBios(const std::string& path, const std::vector<uint8_t>& data) {
  if (data.size() != 8192) return false;
  std::string n = fileName(path);
  for (char& c : n) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  return n.find("coleco") != std::string::npos && n.find(".col") != n.size() - 4;
}

// BIOS do 32X (32X_G_BIOS.BIN 256 bytes, 32X_M_BIOS.BIN 2 KB, 32X_S_BIOS.BIN 1 KB): nome com "32x"
const char* md32xBios(const std::string& path, const std::vector<uint8_t>& data) {
  std::string n = fileName(path);
  for (char& c : n) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  if (n.find("32x") == std::string::npos || n.find(".32x") == n.size() - 4) return nullptr;
  return data.size() == 256 ? "32x_m68k" : data.size() == 2048 ? "32x_master" : data.size() == 1024 ? "32x_slave" : nullptr;
}

// programa do chip DSP-1 do SNES (do cartucho do usuário): 8 KB, nome com "dsp1" (dsp1b.rom, dsp1.rom...)
bool isDsp1Firmware(const std::string& path, const std::vector<uint8_t>& data) {
  if (data.size() != 8192) return false;
  std::string n = fileName(path);
  for (char& c : n) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  return n.find("dsp1") != std::string::npos;
}

// BIOS original do GBA: 16 KB, nome com "bios" (gba_bios.bin, bios.bin...) e sem o cabeçalho de um jogo
bool isGbaBios(const std::string& path, const std::vector<uint8_t>& data) {
  if (data.size() != 0x4000) return false;
  std::string n = fileName(path);
  for (char& c : n) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
  const bool gameLogo = data[4] == 0x24 && data[5] == 0xFF && data[6] == 0xAE && data[7] == 0x51;
  return n.find("bios") != std::string::npos && !gameLogo;
}

// ------------------------------------------------------------ aplicativo
class App {
  // identidade de uma ROM conhecida (chave do save no app e na web)
  struct Ident { std::string fileKey, saveKey, sys; SDL_Time mtime = 0; };

public:
  bool init() {
    mobile_ = indigo::platform::isMobile();
    int winW = 160 * 5, winH = 144 * 5;
    simulated_ = indigo::platform::simulatedPhone(&winW, &winH);
    if (simulated_) SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");  // teste no PC: o mouse faz papel de dedo
    if (mobile_ && !simulated_) SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");  // celular: o toque não vira clique de mouse
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");  // ícone do .exe também na janela e na barra de tarefas
    SDL_SetAppMetadata("Índigo", INDIGO_VERSION, "net.abitante.indigo");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) return fail("Não foi possível iniciar o SDL");
    dataDir_ = indigo::platform::dataDir();
    configDir_ = indigo::platform::configDir();
    // alta densidade: o desenho é feito nos pixels reais da tela (texto nítido em 125%/150% no Windows e no
    // celular); a escala da interface vem do sistema (uiMetrics)
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (mobile_ && !simulated_) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Rect place{static_cast<int>(SDL_WINDOWPOS_CENTERED), static_cast<int>(SDL_WINDOWPOS_CENTERED), winW, winH};
    bool maximize = false;
    if (!mobile_) {
      // PC: a janela de antes (tamanho e lugar), se ainda cabe numa tela; senão ~70% da área útil da tela
      loadSettings();
      maximize = winMax_;
      place = initialWindow();
      flags |= SDL_WINDOW_HIDDEN;  // aparece já no tamanho e no lugar certos
    }
    if (!SDL_CreateWindowAndRenderer("Índigo", place.w, place.h, flags, &win_, &ren_))
      return fail("Não foi possível abrir a janela");
#ifdef INDIGO_WINDOW_ICON
    // Linux: ícone da janela (a barra de tarefas usa o do arquivo .desktop quando ele está instalado)
    if (SDL_Surface* icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(indigo_icon0, indigo_icon0_size), true)) {
      SDL_SetWindowIcon(win_, icon);
      SDL_DestroySurface(icon);
    }
#endif
    if (!mobile_) {
      SDL_SetWindowPosition(win_, place.x, place.y);
      if (maximize) SDL_MaximizeWindow(win_);
      SDL_ShowWindow(win_);
    }
    EVENT_OPEN_FILE = SDL_RegisterEvents(3);
    EVENT_SAVE_BACKUP = EVENT_OPEN_FILE + 1;
    EVENT_ADD_FOLDER = EVENT_OPEN_FILE + 2;
    SDL_SetWindowMinimumSize(win_, 320, 288);
    SDL_SetRenderVSync(ren_, 0);  // o ritmo é dado pelo relógio do console, não pelo monitor

    ensureAudio();

    for (const char* d : {"saves", "saves/historico", "states", "roms", "capas"}) SDL_CreateDirectory((dataDir_ + d).c_str());
    loadFavorites();
    loadFolders();
    loadPlayedSys();
    // arquivo do DSP-1 que o usuário instalou (os jogos com o chip passam a rodar o programa original)
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "dsp1b.rom", fw) && fw.size() == 8192) indigo::setFirmware("dsp1", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "pm_bios.min", fw) && fw.size() == 4096) indigo::setFirmware("pm", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "coleco.rom", fw) && fw.size() == 8192) indigo::setFirmware("coleco", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "disksys.rom", fw) && fw.size() == 8192) indigo::setFirmware("fds", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "5200.rom", fw) && fw.size() == 2048) indigo::setFirmware("a5200", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "vectrex.rom", fw) && fw.size() == 8192) indigo::setFirmware("vectrex", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "intv_exec.bin", fw) && fw.size() == 8192) indigo::setFirmware("intv_exec", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "intv_grom.bin", fw) && fw.size() == 2048) indigo::setFirmware("intv_grom", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "lynxboot.img", fw) && fw.size() == 512) indigo::setFirmware("lynx", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "neogeo.zip", fw) && !fw.empty()) indigo::setFirmware("neogeo", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "neocd.bin", fw) && indigo::neogeocd::isNeoGeoCdBios(fw)) indigo::setFirmware("neogeocd", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "ps1_bios.bin", fw) && indigo::ps1::isPs1Bios(fw)) indigo::setFirmware("ps1", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "3do_bios.bin", fw) && indigo::tdo::isTdoBios(fw)) indigo::setFirmware("3do", std::move(fw));
    // ROMs do Game.com (interna de 4 KB e do sistema de 256 KB, do console da pessoa)
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "gamecom_int.bin", fw) && fw.size() == indigo::gamecom::INT_ROM_SIZE) indigo::setFirmware("gamecom_int", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "gamecom_ext.bin", fw) && fw.size() == indigo::gamecom::EXT_ROM_SIZE) indigo::setFirmware("gamecom_ext", std::move(fw));
    // BIOS do DS (opcional: sem ela o núcleo usa as funções próprias)
    for (const char* k : {"nds_bios9", "nds_bios7"})
      if (std::vector<uint8_t> fw; readFile(dataDir_ + k + ".bin", fw) && indigo::nds::ndsBiosKind(fw) == k) indigo::setFirmware(k, std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "pcfx.rom", fw) && indigo::pcfx::isPcfxBios(fw, "pcfx.rom")) indigo::setFirmware("pcfx", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "saturn_bios.bin", fw) && indigo::saturn::isSaturnBios(fw)) indigo::setFirmware("saturn", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "dc_boot.bin", fw) && indigo::dc::isDcBoot(fw, "dc_boot.bin")) indigo::setFirmware("dc_boot", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "dc_flash.bin", fw) && indigo::dc::isDcFlash(fw, "dc_flash.bin")) indigo::setFirmware("dc_flash", std::move(fw));
    if (std::vector<uint8_t> fw; readFile(dataDir_ + "syscard.pce", fw) && indigo::pcecd::isPceCdBios(fw)) indigo::setFirmware("pcecd", std::move(fw));
    for (const char* r : {"segacd_us", "segacd_eu", "segacd_jp"})
      if (std::vector<uint8_t> fw; readFile(dataDir_ + r + ".bin", fw) && indigo::segacd::isSegaCdBios(fw)) indigo::setFirmware(r, std::move(fw));
    for (const char* k : {"32x_m68k", "32x_master", "32x_slave"})
      if (std::vector<uint8_t> fw; readFile(dataDir_ + k + ".bin", fw) && !fw.empty()) indigo::setFirmware(k, std::move(fw));
    {
      int nt = 0;
      SDL_TouchID* t = SDL_GetTouchDevices(&nt);
      SDL_free(t);
      startFullscreen_ = nt > 0;  // padrão; a escolha gravada nas opções vale por cima
    }
    loadSettings();
    if (!mobile_ && (startFullscreen_ || forceFullscreen_) && !fullscreen_) toggleFullscreen();
    refreshLibrary();
    startCloud();
    rebuildMenu();
    return true;
  }

  // PC: tamanho e lugar da janela ao abrir. Usa o que ficou gravado ao fechar, se ainda cabe numa das telas
  // (o monitor pode ter sido desligado); senão ~70% da área útil da tela principal, com a proporção dela.
  SDL_Rect initialWindow() {
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    SDL_Rect area{0, 0, 1280, 720};
    SDL_GetDisplayUsableBounds(primary, &area);
    if (winW_ >= 320 && winH_ >= 288) {
      const SDL_Rect want{winX_, winY_, winW_, winH_};
      SDL_Rect ub{}, cut{};
      const SDL_DisplayID d = SDL_GetDisplayForRect(&want);
      // a barra de título (faixa de cima) precisa estar visível para dar para arrastar a janela
      const SDL_Rect title{winX_, winY_, winW_, 40};
      if (d && SDL_GetDisplayUsableBounds(d, &ub) && SDL_GetRectIntersection(&title, &ub, &cut) && cut.w >= 120)
        return {winX_, winY_, std::min(winW_, ub.w), std::min(winH_, ub.h)};
    }
    const int w = std::max(area.w * 7 / 10, std::min(area.w, 640)), h = std::max(area.h * 7 / 10, std::min(area.h, 480));
    const int c = static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(primary));
    return {c, c, w, h};
  }

  // guarda o tamanho e o lugar da janela normal (não maximizada nem em tela cheia) para a próxima vez
  void rememberWindow() {
    if (mobile_ || !win_) return;
    const SDL_WindowFlags f = SDL_GetWindowFlags(win_);
    if (f & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) return;
    if (!(f & SDL_WINDOW_FULLSCREEN)) winMax_ = (f & SDL_WINDOW_MAXIMIZED) != 0;
    if (f & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED)) return;
    SDL_GetWindowPosition(win_, &winX_, &winY_);
    SDL_GetWindowSize(win_, &winW_, &winH_);
  }

  // medidas da interface: quantos pixels vale 1 dp, a área segura e se a interface é de toque
  indigo::ui::Metrics uiMetrics(int ow, int oh) const {
    indigo::ui::Metrics m;
    m.touch = mobile_;
    float dp = SDL_GetWindowDisplayScale(win_);  // densidade × escala do sistema (125% = 1,25)
    const float small = static_cast<float>(std::min(ow, oh));
    if (simulated_) {
      dp = small / 400;  // teste no PC: como um celular de ~400 dp de largura
    } else if (mobile_) {
      if (!(dp > 1.01f)) dp = small / 400;  // o sistema não informou: estima pelo tamanho
    } else {
      if (!(dp > 0)) dp = 1;
      // telas grandes vistas de longe (TV, monitor 1440p/4K em 100%, portáteis): cresce um pouco com a janela
      dp = std::max(dp, small / 1000);
    }
    static const float SIZES[] = {1.0f, 0.85f, 1.15f, 1.3f};
    dp *= SIZES[std::clamp(uiSize_, 0, 3)];
    // nunca grande demais para a janela: a interface precisa de ~320 × 300 dp
    dp = std::min(dp, std::min(ow / 320.0f, oh / 300.0f));
    dp = std::max(0.5f, std::round(dp * 20) / 20);  // degraus de 5%: menos tamanhos de letra diferentes
    m.dp = dp;
    if (simulated_) {
      // INDIGO_SAFE=cima,baixo,esquerda,direita (pixels): simula o entalhe e a barra de gestos no teste
      int t = 0, b = 0, l = 0, r = 0;
      if (const char* v = SDL_getenv("INDIGO_SAFE"); v && SDL_sscanf(v, "%d,%d,%d,%d", &t, &b, &l, &r) == 4)
        m.safe = {static_cast<float>(l), static_cast<float>(t), static_cast<float>(ow - l - r), static_cast<float>(oh - t - b)};
    } else if (mobile_) {
      SDL_Rect sa{};
      int ww = 0, wh = 0;
      SDL_GetWindowSize(win_, &ww, &wh);
      if (SDL_GetWindowSafeArea(win_, &sa) && ww > 0 && wh > 0 && sa.w > 0 && sa.h > 0) {
        // a área segura vem em pontos da janela; o desenho é em pixels
        const float kx = static_cast<float>(ow) / ww, ky = static_cast<float>(oh) / wh;
        m.safe = {sa.x * kx, sa.y * ky, sa.w * kx, sa.h * ky};
      }
    }
    return m;
  }

  void applyMetrics(int ow, int oh) {
    const indigo::ui::Metrics m = uiMetrics(ow, oh);
    lib_.m = menu_.m = touch_.m = m;
  }

  bool hasLR() const {
    const std::string s = core_->system();
    return s == "gba" || s == "snes" || s == "pm" || s == "ws" || s == "wsc" || s == "md" || s == "32x" || s == "segacd" || s == "ps1" || s == "3do" || s == "gnw" || s == "gamecom" || s == "a5200" || s == "pcfx" || s == "saturn" || s == "nds" || s == "dc";
  }
  bool hasXY() const {
    const std::string s = core_->system();
    return s == "snes" || s == "ws" || s == "wsc" || s == "md" || s == "32x" || s == "segacd" || s == "ps1" || s == "3do" || s == "gnw" || s == "gamecom" || s == "a5200" || s == "pcfx" || s == "saturn" || s == "intv" || s == "nds" || s == "dc";
  }
  // controles de toque: medidas da tela e a imagem do jogo (deitado, eles ficam na faixa livre ao lado dela)
  bool ps1AnalogHeld_ = false;
  uint16_t rumbleLo_ = 0, rumbleHi_ = 0;
  std::map<std::string, std::string> discSys_;  // console de cada CD já olhado (a estante separa o PS1)
  std::map<std::string, std::string> romSys_;   // console de cada .rom já olhado (MSX, Intellivision ou Jaguar)
  std::map<std::string, std::string> playedSys_;  // aba de cada jogo já aberto (consoles.txt)
  void layoutTouch(int ow, int oh, bool touchUi) {
    applyMetrics(ow, oh);
    const std::string sys = core_ ? core_->system() : "";
    touch_.disk = core_ && core_->diskSides() > 0;  // Famicom Disk System: botão "Disco"
    touch_.keypad = sys == "a5200" ? 1 : sys == "intv" ? 2 : 0;  // teclado numérico do controle
    touch_.layout(ow, oh, oh > ow, hasLR(), hasXY(), gameRect(ow, oh, touchUi || touchUiInUse()), core_ && core_->system() == "ps1",
                  core_ && core_->system() == "3do");
  }

  // abre um jogo; com resume, continua do estado automático gravado ao fechar da última vez
  void setLaunchOptions(bool fullscreen, bool frontend) { forceFullscreen_ = fullscreen; frontend_ = frontend; }

  // lado do disco do Famicom Disk System: "disco 1, lado A"...
  static std::string diskSideName(int s) {
    if (s < 0) return "nenhum no drive";
    return "disco " + std::to_string(s / 2 + 1) + ", lado " + static_cast<char>('A' + s % 2);
  }
  // tira o disco do drive e põe o próximo lado (depois do último, volta ao primeiro)
  void nextDiskSide() {
    const int n = core_->diskSides();
    if (n <= 0) { status("Este jogo não usa disco"); return; }
    const int next = (std::max(core_->diskSide(), -1) + 1) % n;
    core_->setDiskSide(next);
    status("Trocando o disco: " + diskSideName(next));
  }

  void openFile(std::string path, bool resume = false) {
    if (indigo::cdrom::isDiscPath(indigo::platform::displayName(path))) { openDisc(path, resume); return; }
    std::vector<uint8_t> rom;
    if (!readFile(path, rom)) { error("Não foi possível ler o arquivo:\n" + path); return; }
    // nome de verdade do arquivo (no Android o seletor devolve "content://..."); é por ele que se acha o
    // console e se dá nome ao save. Vem de outro app no Android: sem pastas nem ".." (src/core/safe.h)
    std::string name = indigo::safeFileName(indigo::platform::displayName(path));
    const std::string origin = path;  // o que entra na lista de recentes do PC (o próprio .zip, se for um)
    if (indigo::backup::isBackup(rom)) { importBackup(rom); return; }
    {
      // BIOS do Neo Geo (neogeo.zip do usuário): fica guardada para os jogos de Neo Geo
      std::string low = name;
      for (char& c : low) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
      if (indigo::zip::isZip(rom) && indigo::neogeo::isNeoGeoBiosZip(rom, low)) {
        if (writeFile(dataDir_ + "neogeo.zip", rom)) {
          indigo::setFirmware("neogeo", rom);
          status("BIOS do Neo Geo instalada: os jogos de Neo Geo já abrem");
        } else {
          error("Não foi possível copiar a BIOS do Neo Geo para:\n" + dataDir_);
        }
        return;
      }
    }
    if (indigo::zip::isZip(rom) && !indigo::isArcadeSet(rom, name)) {
      std::string inner, err;
      std::vector<uint8_t> data;
      if (!indigo::zip::extractRom(rom, inner, data, err)) { error(err); return; }
      rom.swap(data);
      name = inner;
    }
    if (indigo::segacd::isSegaCdBios(rom)) {
      // BIOS do Sega CD / Mega-CD (128 KB, do usuário): uma por região (EUA, Europa, Japão)
      const std::string fw = indigo::segacd::segaCdFirmwareName(rom);
      if (writeFile(dataDir_ + fw + ".bin", rom)) {
        indigo::setFirmware(fw, rom);
        status("BIOS do Sega CD instalada: vale para os CDs de Sega CD");
      } else {
        error("Não foi possível copiar a BIOS do Sega CD para:\n" + dataDir_);
      }
      return;
    }
    std::string lowName = name;
    for (char& c : lowName) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    if (const std::string gk = indigo::gamecom::gamecomFirmwareKind(rom, lowName); !gk.empty()) {
      // ROMs do Game.com (do console da pessoa): a interna (internal.bin, 4 KB) e a do sistema (external.bin, 256 KB)
      if (writeFile(dataDir_ + gk + ".bin", rom)) {
        indigo::setFirmware(gk, rom);
        const bool both = !indigo::firmware("gamecom_int").empty() && !indigo::firmware("gamecom_ext").empty();
        status(std::string(gk == "gamecom_int" ? "ROM interna" : "ROM do sistema") + " do Game.com instalada" +
               (both ? ": os cartuchos de Game.com já abrem" : ": falta a outra (internal.bin e external.bin)"));
      } else {
        error("Não foi possível copiar a ROM do Game.com para:\n" + dataDir_);
      }
      return;
    }
    if (const std::string nk = indigo::nds::ndsBiosKind(rom); !nk.empty()) {
      // BIOS do DS (do console da pessoa, opcional): as funções da BIOS passam a usar o código dela
      if (writeFile(dataDir_ + nk + ".bin", rom)) {
        indigo::setFirmware(nk, rom);
        const bool both = !indigo::firmware("nds_bios9").empty() && !indigo::firmware("nds_bios7").empty();
        status(std::string("BIOS do DS (") + (nk == "nds_bios9" ? "ARM9" : "ARM7") + ") instalada" +
               (both ? ": os jogos de DS passam a usá-la" : ": falta a outra (ARM9 e ARM7); até lá o DS usa as funções próprias"));
      } else {
        error("Não foi possível copiar a BIOS do DS para:\n" + dataDir_);
      }
      return;
    }
    if (indigo::ps1::isPs1Bios(rom)) {
      // BIOS do PlayStation (512 KB do usuário, por exemplo SCPH1001.BIN): fica guardada para os CDs e os .exe
      if (writeFile(dataDir_ + "ps1_bios.bin", rom)) {
        indigo::setFirmware("ps1", rom);
        status("BIOS do PlayStation instalada: os CDs de PlayStation já abrem");
      } else {
        error("Não foi possível copiar a BIOS do PlayStation para:\n" + dataDir_);
      }
      return;
    }
    if (indigo::tdo::isTdoBios(rom) && (extOf(name) == ".bin" || extOf(name) == ".rom" || extOf(name).empty())) {
      // BIOS do 3DO (1 MB do usuário, por exemplo panafz10.bin): fica guardada para os CDs de 3DO
      if (writeFile(dataDir_ + "3do_bios.bin", rom)) {
        indigo::setFirmware("3do", rom);
        status("BIOS do 3DO instalada: os CDs de 3DO já abrem");
      } else {
        error("Não foi possível copiar a BIOS do 3DO para:\n" + dataDir_);
      }
      return;
    }
    if (!indigo::tdo::isTdoBios(rom) && indigo::pcfx::isPcfxBios(rom, name)) {
      // BIOS do PC-FX (1 MB do usuário, pcfx.rom; reconhecida pelo nome e tamanho): fica guardada para os CDs
      if (writeFile(dataDir_ + "pcfx.rom", rom)) {
        indigo::setFirmware("pcfx", rom);
        status("BIOS do PC-FX instalada: os CDs de PC-FX já abrem");
      } else {
        error("Não foi possível copiar a BIOS do PC-FX para:\n" + dataDir_);
      }
      return;
    }
    if (indigo::dc::isDcBoot(rom, name) || indigo::dc::isDcFlash(rom, name)) {
      // BIOS (dc_boot.bin, 2 MB) ou flash (dc_flash.bin, 128 KB) do Dreamcast do usuário: ficam guardadas
      const bool boot = indigo::dc::isDcBoot(rom, name);
      const char* file = boot ? "dc_boot.bin" : "dc_flash.bin";
      if (writeFile(dataDir_ + file, rom)) {
        indigo::setFirmware(boot ? "dc_boot" : "dc_flash", rom);
        // a flash é opcional (sem ela o núcleo usa uma em branco); só a BIOS é exigida
        const bool hasBoot = !indigo::firmware("dc_boot").empty();
        status(boot ? "BIOS do Dreamcast instalada: os discos de Dreamcast já abrem"
                    : hasBoot ? "Flash do Dreamcast instalada"
                              : "Flash do Dreamcast instalada; abra também a BIOS (dc_boot.bin)");
      } else {
        error(std::string("Não foi possível copiar o arquivo do Dreamcast para:\n") + dataDir_);
      }
      return;
    }
    if (indigo::saturn::isSaturnBios(rom)) {
      // BIOS do Saturn (512 KB do usuário, por exemplo sega_101.bin): fica guardada para os CDs de Saturn
      if (writeFile(dataDir_ + "saturn_bios.bin", rom)) {
        indigo::setFirmware("saturn", rom);
        status("BIOS do Saturn instalada: os CDs de Saturn já abrem");
      } else {
        error("Não foi possível copiar a BIOS do Saturn para:\n" + dataDir_);
      }
      return;
    }
    if (indigo::neogeocd::isNeoGeoCdBios(rom)) {
      // BIOS do Neo Geo CD (512 KB, do usuário; reconhecida pelo conteúdo): fica guardada para os CDs
      if (writeFile(dataDir_ + "neocd.bin", rom)) {
        indigo::setFirmware("neogeocd", rom);
        status("BIOS do Neo Geo CD instalada: vale para os CDs de Neo Geo CD");
      } else {
        error("Não foi possível copiar a BIOS do Neo Geo CD para:\n" + dataDir_);
      }
      return;
    }
    if (std::string low = name; true) {
      for (char& c : low) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
      if (indigo::fds::isFdsBios(rom, fileName(low))) {
        // BIOS do Famicom Disk System (disksys.rom, 8 KB, do usuário): fica guardada para os discos .fds/.qd
        if (writeFile(dataDir_ + "disksys.rom", rom)) {
          indigo::setFirmware("fds", rom);
          status("BIOS do Famicom Disk System instalada: os discos .fds e .qd já abrem");
        } else {
          error("Não foi possível copiar a BIOS do Famicom Disk System para:\n" + dataDir_);
        }
        return;
      }
      if (indigo::fds::isFdsDisk(rom, fileName(low)) && indigo::firmware("fds").empty()) {
        error("O Famicom Disk System precisa da BIOS do console. Abra o arquivo da BIOS (disksys.rom, 8 KB) uma vez no Índigo.");
        return;
      }
    }
    {
      // BIOS do Intellivision (do console do usuário): EXEC (exec.bin, 8 KB) e GROM (grom.bin, 2 KB)
      std::string ln = fileName(name);
      for (char& c : ln) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
      if (const int k = indigo::intv::intvBiosKind(rom, ln)) {
        const char* fw = k == 1 ? "intv_exec" : "intv_grom";
        if (writeFile(dataDir_ + fw + ".bin", rom)) {
          indigo::setFirmware(fw, rom);
          const bool both = !indigo::firmware("intv_exec").empty() && !indigo::firmware("intv_grom").empty();
          status(std::string(k == 1 ? "EXEC" : "GROM") + " do Intellivision instalada" +
                 (both ? ": os jogos de Intellivision já abrem com a BIOS original"
                       : (k == 1 ? ". Falta abrir também o grom.bin" : ". Falta abrir também o exec.bin")));
        } else {
          error("Não foi possível copiar a BIOS do Intellivision para:\n" + dataDir_);
        }
        return;
      }
    }
    if (indigo::vectrex::isVectrexBios(rom)) {
      // BIOS do Vectrex: fica guardada para os cartuchos e já liga o console no Mine Storm (que vem nela)
      if (writeFile(dataDir_ + "vectrex.rom", rom)) indigo::setFirmware("vectrex", rom);
      else { error("Não foi possível copiar a BIOS do Vectrex para:\n" + dataDir_); return; }
    }
    if (rom.size() == 512 && fileName(name).find("lynx") != std::string::npos) {
      if (writeFile(dataDir_ + "lynxboot.img", rom)) {
        indigo::setFirmware("lynx", rom);
        status("ROM de partida do Lynx instalada: vale para os próximos jogos de Lynx");
      } else {
        error("Não foi possível copiar a ROM de partida do Lynx para:\n" + dataDir_);
      }
      return;
    }
    if (indigo::pcecd::isPceCdBios(rom)) {
      // System Card do PC Engine CD (1.0/2.x/3.0 do usuário): fica guardado para os CDs de PC Engine (sem ele
      // o arquivo abriria como um HuCard que só mostra a tela do System Card)
      if (writeFile(dataDir_ + "syscard.pce", rom)) {
        indigo::setFirmware("pcecd", rom);
        status("System Card do PC Engine CD instalado: vale para os CDs de PC Engine");
      } else {
        error("Não foi possível copiar o System Card para:\n" + dataDir_);
      }
      return;
    }
    if (const char* k = md32xBios(name, rom)) {
      // BIOS do 32X (as três: 68000 de 256 bytes, SH-2 mestre de 2 KB e escravo de 1 KB): usadas quando as três estão lá
      if (writeFile(dataDir_ + k + ".bin", rom)) {
        indigo::setFirmware(k, rom);
        status("BIOS do 32X instalada: com as três, os jogos de 32X usam as BIOS originais");
      } else {
        error("Não foi possível copiar a BIOS do 32X para:\n" + dataDir_);
      }
      return;
    }
    if (isA5200Bios(name, rom)) {
      if (writeFile(dataDir_ + "5200.rom", rom)) {
        indigo::setFirmware("a5200", rom);
        status("BIOS do Atari 5200 instalada: os jogos de Atari 5200 já abrem");
      } else {
        error("Não foi possível copiar a BIOS do Atari 5200 para:\n" + dataDir_);
      }
      return;
    }
    if (isColecoBios(name, rom)) {
      if (writeFile(dataDir_ + "coleco.rom", rom)) {
        indigo::setFirmware("coleco", rom);
        status("BIOS do ColecoVision instalada: os jogos de ColecoVision já abrem");
      } else {
        error("Não foi possível copiar a BIOS do ColecoVision para:\n" + dataDir_);
      }
      return;
    }
    if (isPmBios(name, rom)) {
      if (writeFile(dataDir_ + "pm_bios.min", rom)) {
        indigo::setFirmware("pm", rom);
        status("BIOS do Pokémon Mini instalada: vale para os próximos jogos de Pokémon Mini abertos");
      } else {
        error("Não foi possível copiar a BIOS do Pokémon Mini para:\n" + dataDir_);
      }
      return;
    }
    if (isDsp1Firmware(name, rom)) {
      if (writeFile(dataDir_ + "dsp1b.rom", rom)) {
        indigo::setFirmware("dsp1", rom);
        status("Arquivo do DSP-1 instalado: Super Mario Kart e Pilotwings usam o programa original do chip");
      } else {
        error("Não foi possível copiar o arquivo do DSP-1 para:\n" + dataDir_);
      }
      return;
    }
    if (isGbaBios(name, rom)) {
      if (writeFile(dataDir_ + "gba_bios.bin", rom))
        status("BIOS do GBA instalada: vale para os próximos jogos de GBA abertos");
      else
        error("Não foi possível copiar a BIOS para:\n" + dataDir_);
      return;
    }
    flushSave();  // se for o mesmo jogo de novo, o save mais recente tem de estar no disco antes de ler
    // celular: a cópia vai para a biblioteca do app (gravada só depois que o núcleo aceitar, lá embaixo)
    const bool toLibrary = mobile_ && path.rfind(dataDir_ + "roms/", 0) != 0;
    const std::string lib = dataDir_ + "roms/" + name, beside = withoutExt(toLibrary ? lib : path);
    const std::string base = baseName(name), key = fileName(name);
    std::vector<uint8_t> save;
    const std::string savePath = dataDir_ + "saves/" + key + ".sav";
    IntvCfgBeside intvCfg(path);  // .cfg ao lado do .bin (Intellivision): vale para a identificação e o núcleo
    const auto ident = indigo::backup::identify(rom, name);
    pullBeforeBoot(key, ident.saveKey);  // save mais novo na nuvem (outro aparelho)? traz antes de ligar o jogo
    // save do Índigo; depois o nome usado até a versão 0.3 (sem a extensão); por fim um .sav/.ngf ao lado da
    // ROM (vindo de outro emulador). O save é sempre regravado com o nome novo.
    bool haveSave = readFile(savePath, save) || readFile(dataDir_ + "saves/" + base + ".sav", save) ||
                    readFile(beside + ".sav", save) ||
                    readFile(beside + ".ngf", save) ||  // .ngf: save de Neo Geo Pocket de outros emuladores
                    (indigo::ps1::isPsxExe(rom) && (readFile(beside + ".mcr", save) || readFile(beside + ".mcd", save)));
    std::unique_ptr<Core> core;
    std::vector<uint8_t> bios;
    const bool haveBios = readFile(dataDir_ + "gba_bios.bin", bios) && bios.size() == 0x4000;
    if (indigo::ps1::isPsxExe(rom) && indigo::firmware("ps1").empty()) {
      error("O programa de PlayStation precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo SCPH1001.BIN) uma vez no Índigo.");
      return;
    }
    try {
      core = indigo::createCore(rom, name, haveSave ? &save : nullptr, haveBios ? &bios : nullptr);
    } catch (const std::exception& e) {
      error(std::string("Erro ao abrir o jogo: ") + e.what());
      return;
    }
    if (!core) {
      error("Este arquivo não é um jogo de SNES, NES, Game Boy, Game Boy Color, Game Boy Advance, Nintendo DS, Nintendo 64, Master System, "
            "Game Gear, SG-1000, Neo Geo Pocket, Pokémon Mini, Atari 2600, Atari 5200, MSX, ColecoVision, WonderSwan, Atari 7800, Vectrex, Atari Lynx, Virtual Boy, Mega Duck, Watara Supervision, PC Engine, Mega Drive, 32X, Famicom Disk System, Intellivision, Neo Geo ou arcade Capcom (CPS1/CPS2). CDs: Sega CD, PC Engine CD, Neo Geo CD, PlayStation, 3DO, PC-FX e Saturn em .cue, .chd ou .iso; Dreamcast em .gdi, .chd ou .cue.");
      return;
    }
    // celular: guarda uma cópia (já descompactada) na biblioteca do app, para o jogo aparecer na tela inicial.
    // Só depois que o núcleo aceitou o arquivo (antes disso não se grava nada vindo de outro app)
    if (toLibrary && writeFile(lib, rom)) path = lib;
    startGame(std::move(core), ident.saveKey, ident.sys, base, key, savePath, haveSave, haveBios, resume, mobile_ ? path : origin);
  }

  // abre um CD (.cue/.chd/.iso): o núcleo lê a imagem do arquivo aos poucos, sem carregar o disco na memória
  void openDisc(std::string path, bool resume) {
    const std::string name = indigo::safeFileName(indigo::platform::displayName(path));
    const std::string origin = path;
    std::string ext = extOf(name);
    if (path.rfind("content://", 0) == 0) {
      // Android: o seletor só dá acesso ao arquivo escolhido. O .cue precisa dos .bin ao lado, então no celular o
      // CD vai em .chd (um arquivo só); o .chd/.iso é copiado para a biblioteca do app
      if (ext == ".cue") {
        error("No celular, abra o CD em .chd (um arquivo só): o Android não deixa o Índigo ler os .bin ao lado do "
              ".cue. No computador, converta com: chdman createcd -i jogo.cue -o jogo.chd");
        return;
      }
      if (ext == ".gdi") {
        error("No celular, abra o disco de Dreamcast em .chd (um arquivo só): o Android não deixa o Índigo ler as "
              "faixas ao lado do .gdi. No computador, converta com: chdman createcd -i jogo.gdi -o jogo.chd");
        return;
      }
      // copia primeiro para um nome provisório; só vira o arquivo da biblioteca se for um CD reconhecido
      const std::string lib = dataDir_ + "roms/" + name, tmp = dataDir_ + "cd-copiando" + ext;  // fora de roms/ (não aparece na estante)
      status("Copiando o CD para o Índigo...");
      render();
      if (!copyFileStream(path, tmp)) { SDL_RemovePath(tmp.c_str()); error("Não foi possível copiar o CD para:\n" + dataDir_ + "roms/"); return; }
      std::string err;
      std::string sys;
      if (auto disc = indigo::cdrom::open(tmp, &err)) sys = indigo::cdrom::detectSystem(*disc);
      if (sys.empty()) {
        SDL_RemovePath(tmp.c_str());
        error(err.empty() ? "Este CD não é de Sega CD, PC Engine CD, Neo Geo CD, PlayStation, 3DO, PC-FX, Saturn nem Dreamcast." : "Não foi possível abrir o CD: " + err);
        return;
      }
      SDL_RemovePath(lib.c_str());
      if (!SDL_RenamePath(tmp.c_str(), lib.c_str())) { SDL_RemovePath(tmp.c_str()); error("Não foi possível copiar o CD para:\n" + dataDir_ + "roms/"); return; }
      path = lib;
    }
    std::string err;
    auto disc = indigo::cdrom::open(path, &err);
    if (!disc) { error("Não foi possível abrir o CD: " + err); return; }
    const std::string sys = indigo::cdrom::detectSystem(*disc);
    if (sys.empty()) {
      error("Este CD não é de Sega CD, PC Engine CD, Neo Geo CD, PlayStation, 3DO, PC-FX, Saturn nem Dreamcast.");
      return;
    }
    flushSave();
    const std::string base = baseName(name), key = fileName(name);
    const std::string savePath = dataDir_ + "saves/" + key + ".sav";
    std::vector<uint8_t> save;
    // PlayStation: o save é o cartão de memória (128 KB, formato .mcr); um .mcr/.mcd ao lado do CD (de outro
    // emulador) entra quando o Índigo ainda não tem save desse jogo
    const bool haveSave = readFile(savePath, save) ||
                          (sys == "ps1" && (readFile(withoutExt(path) + ".mcr", save) || readFile(withoutExt(path) + ".mcd", save)));
    std::unique_ptr<Core> core;
    try {
      if (sys == "pcfx") {
        // PC-FX: o save é a memória de save interna + a do cartão FX-BMP (64 KB)
        core = std::make_unique<indigo::pcfx::PcfxCore>(std::move(disc), haveSave ? &save : nullptr);
      } else if (sys == "dc") {
        // Dreamcast: o save é o VMU (128 KB) com a flash do console junto
        if (indigo::firmware("dc_boot").empty()) throw std::runtime_error(
            "o Dreamcast precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo dc_boot.bin) uma vez no Índigo.");
        core = std::make_unique<indigo::dc::DcCore>(std::move(disc), haveSave ? &save : nullptr);
      } else if (sys == "saturn") {
        // Saturn: o save é a RAM de backup do console (32 KB)
        if (indigo::firmware("saturn").empty()) throw std::runtime_error(
            "o Saturn precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo sega_101.bin) uma vez no Índigo.");
        core = std::make_unique<indigo::saturn::SaturnCore>(std::move(disc), haveSave ? &save : nullptr);
      } else if (sys == "3do") {
        // 3DO: o save é a NVRAM do console (32 KB)
        if (indigo::firmware("3do").empty()) throw std::runtime_error(
            "o 3DO precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo panafz10.bin) uma vez no Índigo.");
        core = std::make_unique<indigo::tdo::TdoCore>(std::move(disc), haveSave ? &save : nullptr);
      } else if (sys == "ps1") {
        if (indigo::firmware("ps1").empty()) throw std::runtime_error(
            "o PlayStation precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo SCPH1001.BIN) uma vez no Índigo.");
        core = std::make_unique<indigo::ps1::Ps1Core>(std::move(disc), haveSave ? &save : nullptr);
      } else if (sys == "segacd") {
        core = indigo::segacd::createSegaCdCore(std::move(disc), haveSave ? &save : nullptr);
      } else if (sys == "pcecd") {
        if (indigo::firmware("pcecd").empty()) throw std::runtime_error(
            "o PC Engine CD precisa do System Card. Abra o arquivo do System Card (ex.: syscard3.pce) uma vez no Índigo.");
        disc.reset();
        core = indigo::pcecd::createPceCdCore(path, haveSave ? &save : nullptr, &err);
        if (!core) throw std::runtime_error(err);
      } else {
        if (indigo::firmware("neogeocd").empty()) throw std::runtime_error(
            "o Neo Geo CD precisa da BIOS do aparelho. Abra o arquivo da BIOS (ex.: neocd.bin) uma vez no Índigo.");
        core = std::make_unique<indigo::neogeocd::NeoGeoCdCore>(std::move(disc), haveSave ? &save : nullptr);
      }
    } catch (const std::exception& e) {
      error(std::string("Erro ao abrir o CD: ") + e.what());
      return;
    }
    if (!core) { error("Não foi possível abrir o CD."); return; }
    discSys_[displayKey(mobile_ ? path : origin)] = sys;
    // save da nuvem ainda não vale para CDs (a chave da nuvem vem do conteúdo do cartucho)
    startGame(std::move(core), "", sys, base, key, savePath, haveSave, false, resume, mobile_ ? path : origin);
  }

  // liga o núcleo já criado (cartucho ou CD): fecha o jogo anterior, aplica o estado automático e avisa
  void startGame(std::unique_ptr<Core> core, const std::string& saveKey, const std::string& sys, const std::string& base,
                 const std::string& key, const std::string& savePath, bool haveSave, bool haveBios, bool resume,
                 const std::string& recent) {
    closeGame();  // o jogo anterior só fecha (com estado automático) depois que o novo abriu
    core_ = std::move(core);
    openSaveKey_ = saveKey;
    openConsole_ = sys;
    romBase_ = base;
    romKey_ = key;
    savePath_ = savePath;
    core_->setSampleRate(SAMPLE_RATE);
    applyPalette();
    applyPs1Res();
    recreateTexture();
    paused_ = false;
    menuOpen_ = false;
    frameCount_ = 0;
    p2On_ = false;
    std::string note;
    if (resume) {
      std::vector<uint8_t> st;
      try {
        if (readFile(autoStatePath(), st)) { core_->loadState(st); core_->followHostClock(); note = " · continuando de onde parou"; }
      } catch (const std::exception&) {
        note = " · não deu para continuar (estado incompatível)";
      }
    }
    addRecent(recent);
    rememberConsole(recent);
    shelfDirty_ = true;
    rebuildMenu();
    if (audio_) SDL_ClearAudioStream(audio_);
    ensureAudio();
    const std::string t = core_->title().empty() ? base : core_->title();
    status(t + (haveSave && note.empty() ? " · save carregado" : "") + note + core_->note() +
           (haveBios && core_->system() == "gba" ? " · BIOS original" : ""));
  }

  void run() {
    uint64_t next = SDL_GetTicksNS();
    while (running_) {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) handleEvent(ev);
      if (frameCount_ % 15 == 0 || !core_) {
        const std::string rom = indigo::platform::takePendingRom();
        if (!rom.empty()) { frontend_ = true; openFile(rom); }
      }
      if (menuVisible() || libraryMode()) stickNavigate();
      const double frameNs = core_ ? 1e9 / core_->fps() : 1e9 / 60;

      std::vector<uint32_t> padKeys = gamepadKeys();
      if (core_ && !paused_ && !menuOpen_) {
        // dois jogadores: o 2º controle (ou o teclado, pela opção) vai para o jogador 2
        for (auto& k : padKeys) k = adaptKeys(k, true);
        const int players = core_->players();
        const auto pk = indigo::ui::routeKeys(player2_, players, adaptKeys(keyboard_, false), adaptKeys(touch_.keys(), false), padKeys);
        core_->setKeys(pk.p1);
        // o controle 2 só é ligado no console quando alguém comanda o jogador 2; depois fica ligado até fechar o jogo
        if (indigo::ui::player2Present(player2_, players, pads_.size())) p2On_ = true;
        if (p2On_) core_->setKeys2(pk.p2);
        if (auto* p = dynamic_cast<indigo::ps1::Ps1Core*>(core_.get())) ps1Input(*p, touch_.keys());
        if (core_->system() == "gamecom" || core_->system() == "nds") penInput();
        if (core_->system() == "pcfx") mouseInput();
        const bool turbo = turboKey_ || padTurbo_ || touch_.turbo() || fastOn_;
        const int frames = turbo ? 4 : 1;
        for (int i = 0; i < frames; i++) {
          core_->runFrame();
          drainAudio(!turbo);
        }
        if (auto* p = dynamic_cast<indigo::ps1::Ps1Core*>(core_.get())) ps1Rumble(*p);
        if (++frameCount_ % 120 == 0) flushSave(true);
      } else {
        ++frameCount_;
      }
      pollCloud();
      render();
      if (!mobile_) updateTitle();

      next += static_cast<uint64_t>(frameNs);
      const uint64_t now = SDL_GetTicksNS();
      if (now > next + static_cast<uint64_t>(frameNs * 4)) next = now;  // atrasou demais (janela arrastada etc.)
      else if (next > now) SDL_DelayPrecise(next - now);
    }
    closeGame();
    saveSettings();
  }

  ~App() {
    clearThumbs();
    for (auto& [k, t] : covers_) if (t) SDL_DestroyTexture(t);
    screen_.reset();
    if (audio_) SDL_DestroyAudioStream(audio_);
    for (auto* p : pads_) if (p) SDL_CloseGamepad(p);
    indigo::text::shutdown();
    if (ren_) SDL_DestroyRenderer(ren_);
    if (win_) SDL_DestroyWindow(win_);
    SDL_Quit();
  }

private:
  bool fail(const std::string& what) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Índigo", (what + ": " + SDL_GetError()).c_str(), nullptr);
    return false;
  }
  void error(const std::string& msg) { SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Índigo", msg.c_str(), win_); }

  void status(const std::string& msg) { status_ = msg; statusUntil_ = SDL_GetTicks() + 3000; }

  void updateTitle() {
    std::string t = "Índigo";
    if (core_) t += " — " + (core_->title().empty() ? romBase_ : core_->title());
    if (paused_) t += " — pausado";
    if (!status_.empty() && SDL_GetTicks() < statusUntil_) t += " — " + status_;
    if (t != lastTitle_) { SDL_SetWindowTitle(win_, t.c_str()); lastTitle_ = t; }
  }

  // fecha o jogo atual: grava o save e o estado automático ("continuar de onde parou")
  void closeGame() {
    if (!core_) return;
    if (rumbleLo_ | rumbleHi_) { for (auto* p : pads_) if (p) SDL_RumbleGamepad(p, 0, 0, 0); rumbleLo_ = rumbleHi_ = 0; }
    flushSave();
    if (uploadAt_) { uploadAt_ = 0; uploadOpenSave(); }
    writeFile(autoStatePath(), core_->saveState());
    // capa automática: a tela do jogo na hora de fechar, se o jogo ainda não tem capa
    if (SDL_Time t; !fileTime(coverPath(romKey_), t)) writeCover();
    core_.reset();
    clearThumbs();
  }

  // ---------------- entrada
  void handleEvent(const SDL_Event& ev) {
    switch (ev.type) {
      case SDL_EVENT_QUIT: running_ = false; break;
      case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_MOTION: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_CANCELED:
        onFinger(ev.tfinger, ev.type);
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
        // clique (no PC) vale ao soltar o botão, no mesmo lugar em que apertou
        if (!simulated_ && !mobile_ && ev.button.which != SDL_TOUCH_MOUSEID && ev.button.button == SDL_BUTTON_LEFT && menuVisible()) {
          SDL_Event c = ev;
          SDL_ConvertEventToRenderCoordinates(ren_, &c);
          usingPad_ = false;
          if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) { mouseDown_ = true; mx0_ = c.button.x; my0_ = c.button.y; }
          else if (mouseDown_) {
            mouseDown_ = false;
            if (std::hypot(c.button.x - mx0_, c.button.y - my0_) < 12 * lib_.m.dp) uiTap(c.button.x, c.button.y);
          }
        }
        break;
      case SDL_EVENT_MOUSE_WHEEL:
        if (menuVisible()) {
          const float dy = -ev.wheel.y * 70 * lib_.m.dp;  // a rolagem anda a mesma distância em qualquer escala
          if (libraryMode()) lib_.scrollBy(dy); else menu_.scrollBy(dy);
        }
        break;
      case SDL_EVENT_MOUSE_MOTION:
        if (!mobile_ && ev.motion.which != SDL_TOUCH_MOUSEID && !SDL_CursorVisible()) SDL_ShowCursor();
        if (!simulated_ && ev.motion.which != SDL_TOUCH_MOUSEID && menuVisible()) {
          SDL_Event c = ev;
          SDL_ConvertEventToRenderCoordinates(ren_, &c);
          const int id = menu_.hit(c.motion.x, c.motion.y);
          for (size_t k = 0; id >= 0 && k < menu_.items.size(); k++) if (menu_.items[k].id == id) menu_.sel = static_cast<int>(k);
          if (id >= 0) updateSlotPreview();
        }
        break;
      // o Android tirou o app da tela (ligação, bloqueio, troca de app): grava o save e o estado e pausa
      case SDL_EVENT_LOW_MEMORY:
        // pouca memória: só garante o save (pausar aqui deixava o som parado, porque nenhum "voltou" vem depois)
        flushSave();
        break;
      case SDL_EVENT_AUDIO_DEVICE_REMOVED:
        // a saída de som sumiu (fone, controle USB, Bluetooth): abre de novo na saída padrão
        if (audio_ && ev.adevice.which == SDL_GetAudioStreamDevice(audio_)) { SDL_DestroyAudioStream(audio_); audio_ = nullptr; ensureAudio(); }
        break;
      case SDL_EVENT_WILL_ENTER_BACKGROUND: case SDL_EVENT_DID_ENTER_BACKGROUND: case SDL_EVENT_TERMINATING:
        flushSave();
        if (core_) writeFile(autoStatePath(), core_->saveState());
        saveSettings();
        keyboard_ = 0; turboKey_ = false; touch_.clear();
        if (core_ && !menuOpen_) openMenu();
        if (audio_) SDL_PauseAudioStreamDevice(audio_);
        break;
      case SDL_EVENT_DID_ENTER_FOREGROUND:
        if (audio_) SDL_ClearAudioStream(audio_);
        ensureAudio();
        break;
      case SDL_EVENT_DROP_FILE: if (ev.drop.data) openFile(ev.drop.data); break;
      case SDL_EVENT_GAMEPAD_ADDED: {
        if (SDL_Gamepad* p = SDL_OpenGamepad(ev.gdevice.which)) {
          const size_t slot = indigo::ui::padSlotAdd(pads_, p);
          const bool p2 = slot == 1 && player2_ != indigo::ui::Player2::Off && (!core_ || core_->players() >= 2);
          status(p2 ? "Controle 2 conectado (jogador 2)" : "Controle conectado");
        }
        break;
      }
      case SDL_EVENT_GAMEPAD_REMOVED: {
        for (auto* p : pads_)
          if (p && SDL_GetGamepadID(p) == ev.gdevice.which) { SDL_CloseGamepad(p); indigo::ui::padSlotRemove(pads_, p); break; }
        break;
      }
      case SDL_EVENT_GAMEPAD_BUTTON_DOWN: onPadButton(ev.gbutton); break;
      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        // configurando o controle: o gatilho direito vale como botão de acelerar
        if (page_ == P_CAPTURE && menuVisible() && !captureKeys_ && captureStep_ == ACT_TURBO &&
            ev.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER && ev.gaxis.value > 16000) {
          captureTrigger_ = true;
          captureNext();
        }
        break;
      case SDL_EVENT_TEXT_INPUT:
        if (page_ == P_PIN && ev.text.text) {
          for (const char* c = ev.text.text; *c; c++)
            if (static_cast<unsigned char>(*c) >= 0x20 && *c != 0x7F && pinDraft_.size() < 64) pinDraft_ += *c;
          rebuildMenu();
        }
        break;
      case SDL_EVENT_KEY_DOWN:
        if (page_ == P_PIN && menuVisible()) {
          if (ev.key.scancode == SDL_SCANCODE_BACKSPACE) { if (!pinDraft_.empty()) pinDraft_.pop_back(); rebuildMenu(); break; }
          if (ev.key.scancode == SDL_SCANCODE_RETURN || ev.key.scancode == SDL_SCANCODE_KP_ENTER) { activate(M_PIN_OK); break; }
          if (ev.key.scancode == SDL_SCANCODE_ESCAPE || ev.key.scancode == SDL_SCANCODE_AC_BACK) { menuBack(); break; }
          if (ev.key.scancode != SDL_SCANCODE_UP && ev.key.scancode != SDL_SCANCODE_DOWN) break;  // letras vêm pelo TEXT_INPUT
        }
        onKey(ev.key, true);
        break;
      case SDL_EVENT_KEY_UP: onKey(ev.key, false); break;
      case SDL_EVENT_WINDOW_FOCUS_LOST: keyboard_ = 0; turboKey_ = false; flushSave(); break;
      case SDL_EVENT_WINDOW_MOVED: case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_MAXIMIZED: case SDL_EVENT_WINDOW_RESTORED:
        rememberWindow();
        break;
      default:
        if (EVENT_OPEN_FILE && ev.type == EVENT_OPEN_FILE) {
          auto* path = static_cast<std::string*>(ev.user.data1);
          openFile(*path);
          delete path;
        } else if (EVENT_ADD_FOLDER && ev.type == EVENT_ADD_FOLDER) {
          auto* path = static_cast<std::string*>(ev.user.data1);
          addFolder(*path);
          delete path;
        } else if (EVENT_SAVE_BACKUP && ev.type == EVENT_SAVE_BACKUP) {
          auto* path = static_cast<std::string*>(ev.user.data1);
          writeBackup(*path);
          delete path;
        }
        break;
    }
  }

  uint32_t keyFor(SDL_Scancode sc) const {
    for (int a = 0; a < NUM_ACTIONS; a++)
      for (SDL_Scancode k : keyMap_[a]) if (k == sc) return ACTION_BITS[a];
    return 0;
  }
  // teclado numérico do PC = teclado do controle do Atari 5200 (0–9, * e #) e do Intellivision (0–9, * = Clear,
  // # = Enter); nos outros consoles fica livre
  uint32_t keypadFor(SDL_Scancode sc) const {
    if (!core_ || (core_->system() != "a5200" && core_->system() != "intv")) return 0;
    if (sc == SDL_SCANCODE_KP_0) return indigo::KEY_KP0;
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return indigo::KEY_KP0 << (1 + (sc - SDL_SCANCODE_KP_1));
    if (sc == SDL_SCANCODE_KP_MULTIPLY) return indigo::KEY_KPSTAR;
    if (sc == SDL_SCANCODE_KP_DIVIDE || sc == SDL_SCANCODE_KP_PERIOD) return indigo::KEY_KPHASH;
    return 0;
  }
  bool isTurboKey(SDL_Scancode sc) const {
    for (SDL_Scancode k : keyMap_[ACT_TURBO]) if (k == sc) return true;
    return false;
  }

  void onKey(const SDL_KeyboardEvent& k, bool down) {
    const bool ctrl = (k.mod & SDL_KMOD_CTRL) != 0, alt = (k.mod & SDL_KMOD_ALT) != 0;
    if (down && k.scancode == SDL_SCANCODE_AC_BACK) {  // "voltar" do Android
      menuBack();
      return;
    }
    if (alt && k.scancode == SDL_SCANCODE_RETURN) { if (down && !k.repeat) toggleFullscreen(); return; }
    if (page_ == P_CAPTURE && menuVisible()) {
      if (!down || k.repeat) return;
      if (k.scancode == SDL_SCANCODE_ESCAPE) { showPage(P_CONTROLS); return; }
      if (captureKeys_) captureAssign(-1, k.scancode);
      else if (k.scancode == SDL_SCANCODE_RETURN) activate(menu_.selectedId());
      else if (k.scancode == SDL_SCANCODE_UP) moveSel(-1);
      else if (k.scancode == SDL_SCANCODE_DOWN) moveSel(1);
      return;
    }
    if (libraryMode()) {
      if (!down) return;
      usingPad_ = false;
      const bool shift = (k.mod & SDL_KMOD_SHIFT) != 0;
      switch (k.scancode) {
        case SDL_SCANCODE_UP: lib_.navigate(0, -1); break;
        case SDL_SCANCODE_DOWN: lib_.navigate(0, 1); break;
        case SDL_SCANCODE_LEFT: lib_.navigate(-1, 0); break;
        case SDL_SCANCODE_RIGHT: lib_.navigate(1, 0); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_X: libFocusActivate(); break;
        case SDL_SCANCODE_TAB: libSetTab(lib_.tab + (shift ? -1 : 1)); break;
        case SDL_SCANCODE_PAGEDOWN: case SDL_SCANCODE_RIGHTBRACKET: libSetTab(lib_.tab + 1); break;
        case SDL_SCANCODE_PAGEUP: case SDL_SCANCODE_LEFTBRACKET: libSetTab(lib_.tab - 1); break;
        case SDL_SCANCODE_F: libToggleFav(); break;
        case SDL_SCANCODE_ESCAPE: showPage(P_SETTINGS); break;
        case SDL_SCANCODE_O: if (ctrl) showOpenDialog(); break;
        case SDL_SCANCODE_F11: toggleFullscreen(); break;
        default: break;
      }
      return;
    }
    if (menuVisible()) {
      if (!down) return;
      switch (k.scancode) {
        case SDL_SCANCODE_UP: moveSel(-1); break;
        case SDL_SCANCODE_DOWN: moveSel(1); break;
        case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: case SDL_SCANCODE_X: activate(menu_.selectedId()); break;
        case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_Z: case SDL_SCANCODE_BACKSPACE: menuBack(); break;
        case SDL_SCANCODE_O: if (ctrl) showOpenDialog(); break;
        case SDL_SCANCODE_F11: toggleFullscreen(); break;
        default: break;
      }
      return;
    }
    if (const uint32_t bit = keyFor(k.scancode); bit && !ctrl && !alt) {
      if (down) keyboard_ |= bit; else keyboard_ &= ~bit;
      return;
    }
    if (const uint32_t bit = keypadFor(k.scancode); bit && !ctrl && !alt) {
      if (down) keyboard_ |= bit; else keyboard_ &= ~bit;
      return;
    }
    if (isTurboKey(k.scancode)) { turboKey_ = down; return; }
    if (!down || k.repeat) return;
    switch (k.scancode) {
      case SDL_SCANCODE_O: if (ctrl) showOpenDialog(); break;
      case SDL_SCANCODE_R: if (ctrl && core_) { core_->reset(); status("Jogo reiniciado"); } break;
      case SDL_SCANCODE_F4: if (core_) nextDiskSide(); break;  // durante o jogo (na biblioteca não há disco)
      case SDL_SCANCODE_F5: saveState(slot_); break;
      case SDL_SCANCODE_F8: loadState(slot_); break;
      case SDL_SCANCODE_F3:
        if (core_ && !monoPalette()) { status("A paleta vale para o Game Boy clássico, o Mega Duck e o Supervision"); break; }
        palette_ = (palette_ + 1) % NUM_PALETTES; applyPalette(); status(std::string("Paleta: ") + PALETTES[palette_].label);
        break;
      case SDL_SCANCODE_F11: toggleFullscreen(); break;
      case SDL_SCANCODE_ESCAPE:
        if (fullscreen_ && !mobile_ && !frontend_ && !menuOpen_ && !core_) toggleFullscreen();
        else if (core_ && !menuOpen_) openMenu();
        else if (core_) menuBack();
        else if (fullscreen_ && !mobile_ && !frontend_) toggleFullscreen();
        break;
      case SDL_SCANCODE_P: togglePause(); break;
      default:
        if (k.scancode >= SDL_SCANCODE_1 && k.scancode <= SDL_SCANCODE_9) {
          slot_ = 1 + (k.scancode - SDL_SCANCODE_1);
          status("Espaço de estado " + std::to_string(slot_));
        }
        break;
    }
  }

  // teclas de cada controle (na ordem de pads_)
  std::vector<uint32_t> gamepadKeys() {
    // padrão = layout "letters" da versão web (A do Xbox é o A do Game Boy; X também faz B); remapeável no menu
    std::vector<uint32_t> keys(pads_.size(), 0);
    padTurbo_ = false;
    bool selectHeld = false;
    for (size_t i = 0; i < pads_.size(); i++) {
      SDL_Gamepad* p = pads_[i];
      if (!p) continue;  // vaga de um controle desconectado
      uint32_t& m = keys[i];
      auto btn = [p](SDL_GamepadButton b) { return b != SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(p, b); };
      if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= indigo::KEY_UP;
      if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= indigo::KEY_DOWN;
      if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= indigo::KEY_LEFT;
      if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= indigo::KEY_RIGHT;
      for (int a = ACT_A; a <= ACT_Y; a++)
        for (SDL_GamepadButton b : padMap_[a]) if (btn(b)) m |= ACTION_BITS[a];
      for (SDL_GamepadButton b : padMap_[ACT_TURBO]) if (btn(b)) padTurbo_ = true;
      for (SDL_GamepadButton b : padMap_[ACT_SELECT]) if (btn(b)) selectHeld = true;
      const float dz = 0.45f * 32767;
      const float x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
      // DualShock analógico do jogador 1: o analógico esquerdo vai para o núcleo, não para o direcional
      if (auto* c = dynamic_cast<indigo::ps1::Ps1Core*>(core_.get()); c && c->analogMode() && !padP2(i)) continue;
      if (x < -dz) m |= indigo::KEY_LEFT;
      if (x > dz) m |= indigo::KEY_RIGHT;
      if (y < -dz) m |= indigo::KEY_UP;
      if (y > dz) m |= indigo::KEY_DOWN;
      if (padTriggerTurbo_ && SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) padTurbo_ = true;
    }
    // depois de um atalho (Select + botão), o Select e o botão não chegam ao jogo até serem soltos
    if (hotkeyLatched_) {
      if (!selectHeld) hotkeyLatched_ = false;
      else for (auto& m : keys) m &= ~(indigo::KEY_SELECT | indigo::KEY_L | indigo::KEY_R | indigo::KEY_LEFT | indigo::KEY_RIGHT | indigo::KEY_START);
    }
    return keys;
  }
  // o controle i comanda o jogador 2?
  bool padP2(size_t i) const { return core_ && indigo::ui::padPlayer(player2_, core_->players(), i) == 1; }

  // PlayStation: L2/R2 (gatilhos), L3/R3 (apertar os analógicos), os dois analógicos e o botão ANALOG (L3 + R3
  // juntos, ou L2 + R2 + Select). No toque, L2 e R2 vêm dos botões da tela.
  void ps1Input(indigo::ps1::Ps1Core& c, uint32_t touchKeys) {
    bool l2 = (touchKeys & indigo::ui::TOUCH_L2) != 0, r2 = (touchKeys & indigo::ui::TOUCH_R2) != 0, l3 = false, r3 = false;
    int lx = 128, ly = 128, rx = 128, ry = 128;
    auto axis = [](int v) { return std::clamp((v + 32768) >> 8, 0, 255); };
    for (size_t i = 0; i < pads_.size(); i++) {
      SDL_Gamepad* p = pads_[i];
      if (!p || padP2(i)) continue;  // o jogador 2 tem controle digital
      l2 = l2 || SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000;
      r2 = r2 || (!padTriggerTurbo_ && SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000);
      l3 = l3 || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_LEFT_STICK);
      r3 = r3 || SDL_GetGamepadButton(p, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
      const int ax = axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX)), ay = axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY));
      const int bx = axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX)), by = axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
      if (std::abs(ax - 128) > std::abs(lx - 128)) lx = ax;
      if (std::abs(ay - 128) > std::abs(ly - 128)) ly = ay;
      if (std::abs(bx - 128) > std::abs(rx - 128)) rx = bx;
      if (std::abs(by - 128) > std::abs(ry - 128)) ry = by;
    }
    const bool toggle = l3 && r3;
    if (toggle && !ps1AnalogHeld_) {
      c.pressAnalogButton();
      status(c.analogMode() ? "Controle analógico (DualShock) ligado" : "Controle analógico desligado (digital)");
    }
    ps1AnalogHeld_ = toggle;
    c.setExtraButtons(l2, r2, l3 && !toggle, r3 && !toggle);
    c.setAnalog(uint8_t(lx), uint8_t(ly), uint8_t(rx), uint8_t(ry));
  }
  // vibração do DualShock: motor grande no grave (esquerdo), o pequeno no agudo
  void ps1Rumble(indigo::ps1::Ps1Core& c) {
    const uint16_t lo = uint16_t(c.rumbleLarge() * 257), hi = uint16_t(c.rumbleSmall() * 257);
    if (lo == rumbleLo_ && hi == rumbleHi_ && (!(lo | hi) || frameCount_ % 6)) return;
    rumbleLo_ = lo;
    rumbleHi_ = hi;
    for (size_t i = 0; i < pads_.size(); i++)
      if (pads_[i] && !padP2(i)) SDL_RumbleGamepad(pads_[i], lo, hi, (lo | hi) ? 200 : 0);
  }

  // X e Y só existem no SNES: nos outros consoles, X vale A e Y vale B (como antes, o botão da esquerda do
  // controle faz B). No controle, o SNES segue a posição dos botões do original (B embaixo, A à direita, Y à
  // esquerda, X em cima): troca A e B, que no layout do Xbox ficam embaixo e à direita.
  uint32_t adaptKeys(uint32_t m, bool fromPad) const {
    if (core_ && (core_->system() == "snes" || core_->system() == "ps1")) {
      if (fromPad) {
        const uint32_t a = m & indigo::KEY_A, b = m & indigo::KEY_B;
        m &= ~(indigo::KEY_A | indigo::KEY_B);
        if (a) m |= indigo::KEY_B;
        if (b) m |= indigo::KEY_A;
      }
      return m;
    }
    if (core_ && core_->system() == "a5200") return m;  // X faz de "shift" do teclado numérico (ver a5200.h)
    if (core_ && core_->system() == "intv") return m;   // X = botões de cima, Y = tecla 0 (ver intv.h)
    if (m & indigo::KEY_X) m |= indigo::KEY_A;
    if (m & indigo::KEY_Y) m |= indigo::KEY_B;
    return m & ~(indigo::KEY_X | indigo::KEY_Y);
  }

  // ---------------- remapeamento
  void defaultPadMap() {
    for (auto& v : padMap_) v.clear();
    padMap_[ACT_A] = {SDL_GAMEPAD_BUTTON_SOUTH};
    padMap_[ACT_B] = {SDL_GAMEPAD_BUTTON_EAST};
    padMap_[ACT_X] = {SDL_GAMEPAD_BUTTON_NORTH};
    padMap_[ACT_Y] = {SDL_GAMEPAD_BUTTON_WEST};
    padMap_[ACT_L] = {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    padMap_[ACT_R] = {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    padMap_[ACT_START] = {SDL_GAMEPAD_BUTTON_START};
    padMap_[ACT_SELECT] = {SDL_GAMEPAD_BUTTON_BACK};
    padTriggerTurbo_ = true;  // gatilho direito acelera (é um eixo, não um botão)
  }
  void defaultKeyMap() {
    for (auto& v : keyMap_) v.clear();
    keyMap_[ACT_UP] = {SDL_SCANCODE_UP}; keyMap_[ACT_DOWN] = {SDL_SCANCODE_DOWN};
    keyMap_[ACT_LEFT] = {SDL_SCANCODE_LEFT}; keyMap_[ACT_RIGHT] = {SDL_SCANCODE_RIGHT};
    keyMap_[ACT_A] = {SDL_SCANCODE_X}; keyMap_[ACT_B] = {SDL_SCANCODE_Z};
    keyMap_[ACT_L] = {SDL_SCANCODE_A}; keyMap_[ACT_R] = {SDL_SCANCODE_S};
    keyMap_[ACT_START] = {SDL_SCANCODE_RETURN}; keyMap_[ACT_SELECT] = {SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_RSHIFT};
    keyMap_[ACT_X] = {SDL_SCANCODE_D}; keyMap_[ACT_Y] = {SDL_SCANCODE_C};
    keyMap_[ACT_TURBO] = {SDL_SCANCODE_SPACE};
  }

  // controles.txt: "pad a=a" / "key a=X" (nomes do SDL), uma ação por linha
  void loadMaps() {
    defaultPadMap();
    defaultKeyMap();
    std::vector<uint8_t> b;
    if (!readFile(configDir_ + "controles.txt", b)) return;
    std::string t(b.begin(), b.end()), line;
    bool padTouched[NUM_ACTIONS] = {}, keyTouched[NUM_ACTIONS] = {};
    size_t pos = 0;
    while (pos <= t.size()) {
      const size_t nl = t.find('\n', pos);
      line = t.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? t.size() + 1 : nl + 1;
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const auto sp = line.find(' '), eq = line.find('=');
      if (sp == std::string::npos || eq == std::string::npos || eq < sp) continue;
      const std::string kind = line.substr(0, sp), act = line.substr(sp + 1, eq - sp - 1), val = line.substr(eq + 1);
      int a = -1;
      for (int k = 0; k < NUM_ACTIONS; k++) if (act == ACTION_KEYS[k]) a = k;
      if (a < 0) continue;
      if (kind == "pad") {
        if (!padTouched[a]) { padMap_[a].clear(); padTouched[a] = true; }
        if (a == ACT_TURBO && val == "gatilho") continue;  // tratado no fim
        const SDL_GamepadButton btn = SDL_GetGamepadButtonFromString(val.c_str());
        if (btn != SDL_GAMEPAD_BUTTON_INVALID) padMap_[a].push_back(btn);
      } else if (kind == "key") {
        if (!keyTouched[a]) { keyMap_[a].clear(); keyTouched[a] = true; }
        const SDL_Scancode sc = SDL_GetScancodeFromName(val.c_str());
        if (sc != SDL_SCANCODE_UNKNOWN) keyMap_[a].push_back(sc);
      }
    }
    // acelerar no controle: o gatilho direito vale se o arquivo disser "pad turbo=gatilho" (ou se não falar do turbo)
    if (padTouched[ACT_TURBO]) padTriggerTurbo_ = t.find("pad turbo=gatilho") != std::string::npos;
    // arquivos de antes do SNES não falam de X e Y: o botão padrão deles não pode repetir um já usado (o
    // botão da esquerda fazia B)
    for (int a : {ACT_X, ACT_Y}) {
      if (padTouched[a]) continue;
      auto& v = padMap_[a];
      v.erase(std::remove_if(v.begin(), v.end(), [&](SDL_GamepadButton b) {
                for (int o = ACT_A; o < NUM_ACTIONS; o++) if (o != a && std::find(padMap_[o].begin(), padMap_[o].end(), b) != padMap_[o].end()) return true;
                return false;
              }), v.end());
      if (padMap_[a].empty() && a == ACT_Y && !padTouched[ACT_B]) padMap_[a] = {SDL_GAMEPAD_BUTTON_WEST};
    }
  }

  void saveMaps() {
    std::string t;
    for (int a = ACT_A; a < NUM_ACTIONS; a++) {
      for (SDL_GamepadButton b : padMap_[a]) if (const char* n = SDL_GetGamepadStringForButton(b)) t += std::string("pad ") + ACTION_KEYS[a] + "=" + n + "\n";
      if (a == ACT_TURBO && padTriggerTurbo_) t += "pad turbo=gatilho\n";
    }
    for (int a = 0; a < NUM_ACTIONS; a++)
      for (SDL_Scancode sc : keyMap_[a]) t += std::string("key ") + ACTION_KEYS[a] + "=" + SDL_GetScancodeName(sc) + "\n";
    writeFile(configDir_ + "controles.txt", std::vector<uint8_t>(t.begin(), t.end()));
  }

  // começa a configurar: uma ação de cada vez ("aperte o botão para A")
  void startCapture(bool keyboard) {
    captureKeys_ = keyboard;
    captureStep_ = keyboard ? ACT_UP : ACT_A;
    captureNewPad_ = padMap_;
    captureNewKeys_ = keyMap_;
    for (int a = captureStep_; a < NUM_ACTIONS; a++) { if (keyboard) captureNewKeys_[a].clear(); else captureNewPad_[a].clear(); }
    captureTrigger_ = false;
    showPage(P_CAPTURE);
  }
  void captureAssign(int pad, int key) {
    if (page_ != P_CAPTURE) return;
    if (captureKeys_ && key >= 0) captureNewKeys_[captureStep_] = {static_cast<SDL_Scancode>(key)};
    if (!captureKeys_ && pad >= 0) captureNewPad_[captureStep_] = {static_cast<SDL_GamepadButton>(pad)};
    captureNext();
  }
  void captureNext() {
    if (++captureStep_ < NUM_ACTIONS) { rebuildMenu(); return; }
    // fim: grava (o que ficou sem botão volta ao padrão)
    if (captureKeys_) {
      keyMap_ = captureNewKeys_;
      std::array<std::vector<SDL_Scancode>, NUM_ACTIONS> def;
      std::swap(def, keyMap_); defaultKeyMap(); std::swap(def, keyMap_);
      for (int a = 0; a < NUM_ACTIONS; a++) if (keyMap_[a].empty()) keyMap_[a] = def[a];
    } else {
      padMap_ = captureNewPad_;
      std::array<std::vector<SDL_GamepadButton>, NUM_ACTIONS> def;
      std::swap(def, padMap_); defaultPadMap(); std::swap(def, padMap_);
      for (int a = ACT_A; a < NUM_ACTIONS; a++) if (padMap_[a].empty() && a != ACT_TURBO) padMap_[a] = def[a];
      padTriggerTurbo_ = padMap_[ACT_TURBO].empty() || captureTrigger_;
    }
    saveMaps();
    status(captureKeys_ ? "Teclado configurado" : "Controle configurado");
    showPage(P_CONTROLS);
  }

  // atalhos do controle, como num portátil Anbernic: Select + R salva estado, Select + L carrega,
  // Select + direita/esquerda troca o espaço de estado
  bool padHotkey(SDL_GamepadButton b, SDL_Gamepad* pad) {
    if (!core_ || menuOpen_ || !pad) return false;
    bool select = false;  // o Select do mesmo controle (com dois jogadores, um não dispara o atalho do outro)
    for (SDL_GamepadButton s : padMap_[ACT_SELECT]) if (SDL_GetGamepadButton(pad, s)) select = true;
    if (!select) return false;
    auto is = [&](int act) { for (SDL_GamepadButton x : padMap_[act]) if (x == b) return true; return false; };
    if (is(ACT_R)) saveState(slot_);
    else if (is(ACT_L)) loadState(slot_);
    else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) { slot_ = slot_ % 9 + 1; status("Espaço de estado " + std::to_string(slot_)); }
    else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) { slot_ = (slot_ + 7) % 9 + 1; status("Espaço de estado " + std::to_string(slot_)); }
    else return false;
    hotkeyLatched_ = true;
    return true;
  }

  void showOpenDialog() {
    static const SDL_DialogFileFilter filters[] = {
      {"Jogos (SNES, NES, Famicom Disk System, Game Boy, Game Boy Color, Game Boy Advance, Nintendo DS, Master System, Game Gear, Neo Geo Pocket, Pokémon Mini, Atari 2600, Atari 5200, MSX, ColecoVision, SG-1000, WonderSwan, Atari 7800, Vectrex, Intellivision, Atari Lynx, Virtual Boy, Mega Duck, Watara Supervision, PC Engine, Mega Drive, 32X, Neo Geo, arcade Capcom, Game & Watch, Sega CD, PC Engine CD, Neo Geo CD, PlayStation, 3DO, Game.com, Jaguar, Nintendo 64, PC-FX, Saturn, Dreamcast)",
       indigo::app::dialogPattern().c_str()},
      {"Todos os arquivos", "*"},
    };
    SDL_ShowOpenFileDialog(
        [](void*, const char* const* list, int) {
          if (!list || !list[0]) return;
          // a resposta pode chegar em outra thread: entrega para o laço principal
          SDL_Event ev{};
          ev.type = EVENT_OPEN_FILE;
          ev.user.data1 = new std::string(list[0]);
          SDL_PushEvent(&ev);
        },
        nullptr, win_, mobile_ ? nullptr : filters, mobile_ ? 0 : 2, nullptr, false);
  }

  // ---------------- estados
  std::string statePath(int slot) const { return dataDir_ + "states/" + romKey_ + "." + std::to_string(slot) + ".state"; }
  // nome usado até a versão 0.3 (sem a extensão da ROM), ainda aceito ao carregar
  std::string legacyStatePath(int slot) const { return dataDir_ + "states/" + romBase_ + "." + std::to_string(slot) + ".state"; }
  std::string thumbPath(int slot) const { return dataDir_ + "states/" + romKey_ + "." + std::to_string(slot) + ".thumb"; }
  std::string autoStatePath() const { return dataDir_ + "states/" + romKey_ + ".auto.state"; }
  static std::string autoStateFor(const std::string& dataDir, const std::string& romPath) {
    return dataDir + "states/" + fileName(romPath) + ".auto.state";
  }

  void saveState(int slot) {
    if (!core_) return;
    slot_ = slot;
    if (!writeFile(statePath(slot), core_->saveState())) { status("Não foi possível salvar o estado"); return; }
    // miniatura: a tela do jogo no momento ("ITH1", largura, altura, pixels RGBA)
    const std::vector<uint8_t> th = thumbBytes();
    writeFile(thumbPath(slot), th);
    thumbs_.erase(slot);  // a miniatura antiga sai do cache
    status("Estado " + std::to_string(slot) + " salvo");
  }

  void loadState(int slot) {
    if (!core_) return;
    slot_ = slot;
    std::vector<uint8_t> st;
    if (!readFile(statePath(slot), st) && !readFile(legacyStatePath(slot), st)) { status("Espaço " + std::to_string(slot) + " vazio"); return; }
    try {
      core_->loadState(st);
      core_->followHostClock();
      if (audio_) SDL_ClearAudioStream(audio_);
      status("Estado " + std::to_string(slot) + " carregado");
    } catch (const std::exception& e) {
      status(e.what());
    }
  }

  SDL_Texture* loadThumb(const std::string& path) {
    SDL_Texture* t = nullptr;
    std::vector<uint8_t> b;
    if (readFile(path, b) && b.size() > 12 && std::memcmp(b.data(), "ITH1", 4) == 0) {
      int w = 0, h = 0;
      for (int k = 0; k < 4; k++) { w |= b[4 + k] << (8 * k); h |= b[8 + k] << (8 * k); }
      if (w > 0 && h > 0 && w <= 1024 && h <= 1024 && b.size() >= 12 + size_t(w) * h * 4) {
        t = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (t) { SDL_UpdateTexture(t, nullptr, b.data() + 12, w * 4); SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST); }
      }
    }
    return t;
  }

  SDL_Texture* thumb(int slot) {
    if (auto it = thumbs_.find(slot); it != thumbs_.end()) return it->second;
    SDL_Texture* t = loadThumb(thumbPath(slot));
    thumbs_[slot] = t;
    return t;
  }
  void clearThumbs() {
    for (auto& [k, t] : thumbs_) if (t) SDL_DestroyTexture(t);
    thumbs_.clear();
  }

  // ---------------- save do cartucho, com versões anteriores
  std::string historyPrefix() const { return dataDir_ + "saves/historico/" + romKey_ + "."; }

  std::vector<std::pair<std::string, long long>> saveVersions() const { return indigo::shelf::saveVersions(dataDir_, romKey_); }

  // guarda a versão atual do save no histórico (force: mesmo que a última tenha menos de 10 min)
  void snapshotSave(bool force) {
    SDL_Time cur = 0, now = 0;
    if (!fileTime(savePath_, cur)) return;
    SDL_GetCurrentTime(&now);
    auto list = saveVersions();
    if (!force && !list.empty() && now - list.front().second * 1000000000LL < HIST_EVERY_NS) return;
    std::vector<uint8_t> data;
    if (!readFile(savePath_, data) || data.empty()) return;
    // não guarda duas versões iguais seguidas
    std::vector<uint8_t> last;
    if (!list.empty() && readFile(list.front().first, last) && last == data) return;
    writeFile(historyPrefix() + std::to_string(now / 1000000000) + ".sav", data);
    indigo::shelf::pruneVersions(dataDir_, romKey_, HIST_MAX);
  }

  // periodica = gravação de rotina durante o jogo; o resto (fechar, trocar, perder o foco) grava tudo o que falta
  void flushSave(bool periodica = false) {
    if (!core_ || !(periodica ? core_->saveDirty() : core_->savePendente())) return;
    const auto data = core_->exportSave();
    if (data.empty()) return;
    snapshotSave(false);
    if (!writeFile(savePath_, data)) { status("Não foi possível gravar o save!"); return; }
    indigo::shelf::setSaveAt(dataDir_, romKey_, indigo::shelf::nowMs());
    uploadAt_ = SDL_GetTicks() + CLOUD_UPLOAD_DELAY;  // sobe para a nuvem daqui a pouco (junta gravações seguidas)
  }

  void restoreVersion(const std::string& path) {
    std::vector<uint8_t> data;
    if (!core_ || !readFile(path, data)) return;
    flushSave();
    snapshotSave(true);  // o save atual não se perde: vira uma versão anterior
    if (!writeFile(savePath_, data)) { status("Não foi possível restaurar o save"); return; }
    indigo::shelf::setSaveAt(dataDir_, romKey_, indigo::shelf::nowMs());
    uploadAt_ = SDL_GetTicks() + CLOUD_UPLOAD_DELAY;
    core_->importSave(data);
    core_->reset();  // o jogo lê o save ao ligar
    if (audio_) SDL_ClearAudioStream(audio_);
    status("Save restaurado; o jogo foi reiniciado");
  }

  // ---------------- opções e jogos recentes
  // consoles de 4 tons que usam a paleta do Game Boy clássico
  bool monoPalette() const {
    if (!core_) return false;
    const std::string s = core_->system();
    return s == "gb" || s == "duck" || s == "svision";
  }

  void applyPalette() {
    if (!core_) return;
    const Palette& p = PALETTES[palette_];
    core_->setDmgPalette(p.bg, p.hasObj ? p.obj0 : nullptr, p.hasObj ? p.obj1 : nullptr);
  }

  // resolução interna do PlayStation (1x, 2x ou 4x). Sem escolha do usuário: 2x no PC e 1x no celular (medido: 4x
  // custa cerca de 2,5 vezes o tempo de 1x no disco de teste, e num jogo com mais polígonos a diferença cresce)
  int ps1Res() const { return ps1Res_ ? ps1Res_ : (mobile_ ? 1 : 2); }
  void applyPs1Res() {
    if (auto* p = dynamic_cast<indigo::ps1::Ps1Core*>(core_.get())) { p->setResolution(ps1Res()); recreateTexture(); }
  }
  // miniatura dos estados e capa: a tela do jogo ("ITH1", largura, altura, pixels RGBA); com o PlayStation em
  // resolução interna maior, reduzida para a do console (média de cada bloco), para não crescer 4 ou 16 vezes
  std::vector<uint8_t> thumbBytes() const {
    int n = 1;
    if (auto* p = dynamic_cast<const indigo::ps1::Ps1Core*>(core_.get())) n = p->resolution();
    const int fw = core_->width(), w = fw / n, h = core_->height() / n;
    const uint32_t* f = core_->framebuffer();
    std::vector<uint8_t> th(12 + size_t(w) * h * 4);
    std::memcpy(th.data(), "ITH1", 4);
    for (int k = 0; k < 4; k++) { th[4 + k] = uint8_t(w >> (8 * k)); th[8 + k] = uint8_t(h >> (8 * k)); }
    if (n == 1) { std::memcpy(th.data() + 12, f, size_t(w) * h * 4); return th; }
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        uint32_t sum[4] = {0, 0, 0, 0};
        for (int j = 0; j < n; j++)
          for (int i = 0; i < n; i++) {
            const uint32_t px = f[size_t(y * n + j) * size_t(fw) + size_t(x * n + i)];
            for (int k = 0; k < 4; k++) sum[k] += (px >> (8 * k)) & 0xFF;
          }
        uint8_t* d = th.data() + 12 + (size_t(y) * w + x) * 4;
        for (int k = 0; k < 4; k++) d[k] = uint8_t(sum[k] / uint32_t(n * n));
      }
    return th;
  }

  std::string ps1ResLabel() const {
    const int r = ps1Res();
    return "Resolução do PlayStation: " + std::to_string(r) + "x" + (r == 1 ? " (original)" : "");
  }

  void loadSettings() {
    std::vector<uint8_t> s;
    if (readFile(configDir_ + "opcoes.txt", s) && !s.empty()) {
      const std::string t(s.begin(), s.end());
      const int p = std::atoi(t.c_str());
      if (p >= 0 && p < NUM_PALETTES) palette_ = p;
      const auto nl = t.find('\n');
      if (nl != std::string::npos) {
        const int f = std::atoi(t.c_str() + nl + 1);
        if (f >= 0 && f < indigo::video::NUM_FILTERS) filter_ = f;
        const auto nl2 = t.find('\n', nl + 1);
        if (nl2 != std::string::npos && nl2 + 1 < t.size() && (t[nl2 + 1] == '0' || t[nl2 + 1] == '1'))
          startFullscreen_ = t[nl2 + 1] == '1';
      }
      // chaves novas (1.9), uma por linha, depois das três linhas antigas: "nome=valor"
      size_t pos = 0;
      while (pos < t.size()) {
        size_t end = t.find('\n', pos);
        if (end == std::string::npos) end = t.size();
        const std::string ln = t.substr(pos, end - pos);
        pos = end + 1;
        int a = 0, b = 0, c = 0, d = 0, e = 0;
        if (ln.rfind("janela=", 0) == 0 && SDL_sscanf(ln.c_str() + 7, "%d,%d,%d,%d,%d", &a, &b, &c, &d, &e) == 5) {
          winX_ = a; winY_ = b; winW_ = c; winH_ = d; winMax_ = e != 0;
        } else if (ln.rfind("inteira=", 0) == 0) {
          integerScale_ = std::atoi(ln.c_str() + 8) != 0;
        } else if (ln.rfind("interface=", 0) == 0) {
          uiSize_ = std::clamp(std::atoi(ln.c_str() + 10), 0, 3);
        } else if (ln.rfind("sgb=", 0) == 0) {
          sgb_ = std::atoi(ln.c_str() + 4) != 0;
        } else if (ln.rfind("ps1res=", 0) == 0) {
          const int r = std::atoi(ln.c_str() + 7);
          ps1Res_ = (r == 1 || r == 2 || r == 4) ? r : 0;
        } else if (ln.rfind("telasds=", 0) == 0) {
          dsMode_ = static_cast<indigo::ui::DsLayout>(std::clamp(std::atoi(ln.c_str() + 8), 0, 3));
        } else if (ln.rfind("jogador2=", 0) == 0) {
          player2_ = static_cast<indigo::ui::Player2>(std::clamp(std::atoi(ln.c_str() + 9), 0, 2));
        }
      }
    }
    indigo::gb::setSgbEnabled(sgb_);
    loadMaps();
  }
  void saveSettings() {
    rememberWindow();
    std::string s = std::to_string(palette_) + "\n" + std::to_string(filter_) + "\n" + (startFullscreen_ ? "1" : "0") + "\n";
    s += std::string("inteira=") + (integerScale_ ? "1" : "0") + "\ninterface=" + std::to_string(uiSize_) + "\n";
    if (!sgb_) s += "sgb=0\n";
    if (ps1Res_) s += "ps1res=" + std::to_string(ps1Res_) + "\n";
    if (dsMode_ != indigo::ui::DsLayout::Auto) s += "telasds=" + std::to_string(static_cast<int>(dsMode_)) + "\n";
    if (player2_ != indigo::ui::Player2::SecondPad) s += "jogador2=" + std::to_string(static_cast<int>(player2_)) + "\n";
    if (winW_ > 0) s += "janela=" + std::to_string(winX_) + "," + std::to_string(winY_) + "," + std::to_string(winW_) + "," +
                        std::to_string(winH_) + "," + (winMax_ ? "1" : "0") + "\n";
    writeFile(configDir_ + "opcoes.txt", std::vector<uint8_t>(s.begin(), s.end()));
  }

  // celular: os jogos copiados para a pasta do app; PC: a lista "recentes.txt" (caminhos dos arquivos)
  void refreshLibrary() {
    library_.clear();
    if (mobile_) {
      struct Entry { std::string path; SDL_Time t; };
      std::vector<Entry> list;
      const std::string dir = dataDir_ + "roms/";
      int count = 0;
      char** names = SDL_GlobDirectory(dir.c_str(), nullptr, 0, &count);
      for (int k = 0; names && k < count; k++) {
        SDL_Time t = 0;
        const std::string p = dir + names[k];
        if (fileTime(p, t)) list.push_back({p, t});
      }
      SDL_free(names);
      std::sort(list.begin(), list.end(), [](const Entry& a, const Entry& b) { return a.t > b.t; });
      for (size_t k = 0; k < list.size() && k < MAX_RECENT; k++) library_.push_back(list[k].path);
      return;
    }
    std::vector<uint8_t> s;
    if (readFile(dataDir_ + "recentes.txt", s)) {
      std::string cur;
      for (uint8_t c : s) {
        if (c == '\n') { if (!cur.empty()) library_.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += static_cast<char>(c);
      }
      if (!cur.empty()) library_.push_back(cur);
    }
    // depois dos recentes, os jogos guardados na pasta do app (vindos de um backup), do mais novo ao mais antigo
    std::vector<std::pair<SDL_Time, std::string>> extra;
    for (const auto& n : indigo::files::list(dataDir_ + "roms/")) {
      const std::string p = dataDir_ + "roms/" + n;
      SDL_Time t = 0;
      if (std::find(library_.begin(), library_.end(), p) == library_.end() && fileTime(p, t)) extra.push_back({t, p});
    }
    std::sort(extra.begin(), extra.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& e : extra) library_.push_back(e.second);
    if (library_.size() > MAX_RECENT) library_.resize(MAX_RECENT);
  }

  // todas as ROMs conhecidas (para o backup): recentes do PC e a pasta do app inteira
  std::vector<std::string> allLocalRoms() {
    std::vector<std::string> out = library_;
    std::vector<uint8_t> s;
    if (!mobile_ && readFile(dataDir_ + "recentes.txt", s)) {
      std::string cur;
      for (uint8_t c : s) {
        if (c == '\n') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += static_cast<char>(c);
      }
      if (!cur.empty()) out.push_back(cur);
    }
    for (const auto& n : indigo::files::list(dataDir_ + "roms/")) out.push_back(dataDir_ + "roms/" + n);
    std::vector<std::string> uniq;
    for (auto& p : out) if (std::find(uniq.begin(), uniq.end(), p) == uniq.end()) uniq.push_back(p);
    return uniq;
  }

  // ---------------- nuvem dos saves (a mesma do site, com o mesmo PIN)
  void startCloud() {
    std::vector<uint8_t> b;
    if (readFile(configDir_ + "nuvem.txt", b)) {
      // linha 1: ligada (1/0); linha 2: PIN
      std::string t(b.begin(), b.end());
      const auto nl = t.find('\n');
      cloudOn_ = t.substr(0, nl) != "0";
      if (nl != std::string::npos) {
        cloudPin_ = t.substr(nl + 1);
        while (!cloudPin_.empty() && (cloudPin_.back() == '\n' || cloudPin_.back() == '\r')) cloudPin_.pop_back();
      }
    }
    // testes: servidor local. Só https:// (o PIN vai no cabeçalho); http:// só para este computador
    const char* env = SDL_getenv("INDIGO_CLOUD_URL");
    const bool envOk = env && *env && indigo::cloudUrlAllowed(env);
    if (env && *env && !envOk) SDL_Log("INDIGO_CLOUD_URL ignorado (use https://, ou http:// só para localhost/127.0.0.1): %s", env);
    cloud_ = std::make_unique<indigo::cloud::Client>(envOk ? env : CLOUD_URL);
    cloud_->setPin(cloudPin_);
    cloudState_ = cloudOn_ ? CS_IDLE : CS_OFF;
    if (cloudOn_) cloud_->ping();
  }

  void saveCloudSettings() {
    const std::string t = std::string(cloudOn_ ? "1" : "0") + "\n" + cloudPin_ + "\n";
    writeFile(configDir_ + "nuvem.txt", std::vector<uint8_t>(t.begin(), t.end()));
  }

  bool cloudUsable() const { return cloud_ && cloudOn_ && cloudState_ != CS_UNAVAILABLE && cloudState_ != CS_OFF; }

  std::string cloudStatusText() const {
    std::string last;
    if (cloudLast_ > 0) {
      SDL_DateTime d{};
      if (SDL_TimeToDateTime(static_cast<SDL_Time>(cloudLast_) * 1000000, &d, true)) {
        char b[16];
        SDL_snprintf(b, sizeof b, "%02d:%02d", d.hour, d.minute);
        last = b;
      }
    }
    switch (cloudState_) {
      case CS_OFF: return "Nuvem desligada neste aparelho";
      case CS_UNAVAILABLE: return "Nuvem fora do ar ou sem internet";
      case CS_SYNCING: return "Sincronizando...";
      case CS_LOCKED: return "Digite o PIN da nuvem (o mesmo do site)";
      case CS_ERROR: return "Problema: " + cloudError_;
      default: return last.empty() ? "Pronta" : "Última sincronização às " + last;
    }
  }

  // identidade (igual à web) de uma ROM conhecida, com cache por caminho
  const Ident* identOf(const std::string& path) {
    SDL_Time t = 0;
    if (!fileTime(path, t)) return nullptr;
    auto it = idents_.find(path);
    if (it != idents_.end() && it->second.mtime == t) return it->second.saveKey.empty() ? nullptr : &it->second;
    Ident id;
    id.mtime = t;
    std::vector<uint8_t> b;
    std::string key;
    if (indigo::shelf::loadRom(path, b, key)) {
      IntvCfgBeside cfg(path);
      const auto r = indigo::backup::identify(b, key);
      if (!r.sys.empty()) { id.fileKey = key; id.saveKey = r.saveKey; id.sys = r.sys; }
    }
    const auto& ref = idents_[path] = id;
    return ref.saveKey.empty() ? nullptr : &ref;
  }

  void cloudSync(const std::string& reason, bool interactive) {
    if (!cloud_ || !cloudOn_) { if (interactive) status("A nuvem está desligada (menu Nuvem dos saves)"); return; }
    if (cloudState_ == CS_LOCKED && !interactive) return;
    if (cloudState_ == CS_LOCKED && cloudPin_.empty()) { pinDraft_.clear(); menuOpen_ = true; showPage(P_PIN); SDL_StartTextInput(win_); return; }
    if (cloudState_ == CS_SYNCING) return;
    cloudInteractive_ = interactive;
    cloudState_ = CS_SYNCING;
    cloudUp_ = cloudDown_ = 0;
    cloud_->list(reason);
  }

  void uploadOpenSave() {
    if (!cloudUsable() || !core_ || openSaveKey_.empty()) return;
    std::vector<uint8_t> data;
    if (!readFile(savePath_, data) || data.empty()) return;
    cloud_->put(openSaveKey_, indigo::shelf::saveAt(dataDir_, romKey_), std::move(data), mobile_ ? "Celular" : "Computador",
                core_->title().empty() ? romBase_ : core_->title(), openConsole_);
  }

  // antes de ligar um jogo: se a nuvem tem um save mais novo (de outro aparelho), traz (tempo limite curto)
  void pullBeforeBoot(const std::string& fileKey, const std::string& saveKey) {
    if (!cloudUsable() || cloudState_ == CS_LOCKED) return;
    indigo::cloud::RemoteSave top;
    bool pinError = false;
    if (!cloud_->latest(saveKey, top, pinError, 4000)) { if (pinError) cloudState_ = CS_LOCKED; return; }
    if (indigo::shelf::saveAt(dataDir_, fileKey) >= top.at) return;
    std::vector<uint8_t> data;
    if (!cloud_->fetch(saveKey, top.at, data, 8000)) return;
    applyDownload(fileKey, data, top.at);
    status("Save trazido da nuvem (" + (top.from.empty() ? std::string("outro aparelho") : top.from) + ")");
  }

  // grava um save que veio da nuvem; o que estava aqui vira versão anterior
  void applyDownload(const std::string& fileKey, const std::vector<uint8_t>& data, double at) {
    const std::string path = dataDir_ + "saves/" + fileKey + ".sav";
    std::vector<uint8_t> cur;
    if (readFile(path, cur)) {
      if (cur == data) { indigo::shelf::setSaveAt(dataDir_, fileKey, at); return; }
      writeFile(dataDir_ + "saves/historico/" + fileKey + "." + std::to_string(static_cast<long long>(indigo::shelf::saveAt(dataDir_, fileKey) / 1000)) + ".sav", cur);
      indigo::shelf::pruneVersions(dataDir_, fileKey, HIST_MAX);
    }
    if (writeFile(path, data)) indigo::shelf::setSaveAt(dataDir_, fileKey, at);
  }

  void pollCloud() {
    if (!cloud_) return;
    if (uploadAt_ && SDL_GetTicks() >= uploadAt_) { uploadAt_ = 0; uploadOpenSave(); }
    indigo::cloud::Result r;
    while (cloud_->poll(r)) {
      using K = indigo::cloud::Result;
      if (r.kind == K::Ping) {
        if (!cloudOn_) continue;
        if (r.ok && r.storage) { cloudState_ = CS_IDLE; cloudSync("abertura", false); }
        else cloudState_ = CS_UNAVAILABLE;
      } else if (r.kind == K::Auth) {
        if (r.ok) {
          cloudPin_ = r.tag;
          cloud_->setPin(cloudPin_);
          saveCloudSettings();
          cloudState_ = CS_IDLE;
          status("PIN certo: sincronizando os saves");
          cloudSync("PIN digitado", true);
        } else {
          status(r.pinError ? "PIN incorreto" : r.error);
          if (!r.pinError) { cloudState_ = CS_ERROR; cloudError_ = r.error; }
        }
      } else if (r.kind == K::List) {
        if (!r.ok) { cloudFail(r); continue; }
        onRemoteList(r.list, r.tag);
      } else if (r.kind == K::Put) {
        if (!r.ok) { cloudFail(r); continue; }
        cloudLast_ = indigo::shelf::nowMs();
        if (cloudPending_ > 0 && --cloudPending_ == 0) finishSync();
      } else if (r.kind == K::Get) {
        if (!r.ok) { cloudFail(r); continue; }
        // descobre de qual ROM é esta chave
        for (const auto& [path, id] : idents_)
          if (id.saveKey == r.key) { applyDownload(id.fileKey, r.data, r.at); break; }
        if (cloudPending_ > 0 && --cloudPending_ == 0) finishSync();
      }
    }
    if (cloudState_ != CS_SYNCING) cloudInteractive_ = false;
  }

  void cloudFail(const indigo::cloud::Result& r) {
    cloudPending_ = 0;
    if (r.pinError) {
      cloudState_ = CS_LOCKED;
      if (cloudInteractive_) { status(cloudPin_.empty() ? "Digite o PIN da nuvem" : "PIN incorreto"); pinDraft_.clear(); menuOpen_ = true; showPage(P_PIN); SDL_StartTextInput(win_); }
    } else {
      cloudState_ = CS_ERROR;
      cloudError_ = r.error;
      if (cloudInteractive_) status("Nuvem: " + r.error);
    }
    if (page_ == P_CLOUD) rebuildMenu();
  }

  void onRemoteList(const std::vector<indigo::cloud::RemoteSave>& remote, const std::string& reason) {
    std::map<std::string, const indigo::cloud::RemoteSave*> rmap;
    for (const auto& r : remote) rmap[r.key] = &r;
    cloudPending_ = 0;
    std::vector<std::string> seen;
    for (const auto& path : allLocalRoms()) {
      const Ident* id = identOf(path);
      if (!id || std::find(seen.begin(), seen.end(), id->saveKey) != seen.end()) continue;
      seen.push_back(id->saveKey);
      const bool isOpen = core_ && id->saveKey == openSaveKey_;
      const std::string savPath = dataDir_ + "saves/" + id->fileKey + ".sav";
      std::vector<uint8_t> data;
      const bool haveLocal = readFile(savPath, data) && !data.empty();
      const double localAt = haveLocal ? indigo::shelf::saveAt(dataDir_, id->fileKey) : 0;
      auto it = rmap.find(id->saveKey);
      const indigo::cloud::RemoteSave* rem = it == rmap.end() ? nullptr : it->second;
      if (haveLocal && (!rem || localAt > rem->at)) {
        std::string nm = id->fileKey;
        if (const auto dot = nm.find_last_of('.'); dot != std::string::npos) nm = nm.substr(0, dot);
        cloud_->put(id->saveKey, localAt, std::move(data), mobile_ ? "Celular" : "Computador", nm, id->sys);
        cloudPending_++; cloudUp_++;
      } else if (rem && (!haveLocal || rem->at > localAt) && !isOpen) {
        cloud_->get(id->saveKey, rem->at);  // o jogo aberto recebe na próxima vez que for aberto
        cloudPending_++; cloudDown_++;
      }
    }
    lastSyncReason_ = reason;
    if (!cloudPending_) finishSync();
  }

  void finishSync() {
    cloudState_ = CS_IDLE;
    cloudLast_ = indigo::shelf::nowMs();
    if (cloudUp_ || cloudDown_)
      status("Nuvem: " + std::to_string(cloudUp_) + " save(s) enviado(s), " + std::to_string(cloudDown_) + " recebido(s)");
    else if (cloudInteractive_)
      status("Nuvem em dia: nada para trocar");
    if (page_ == P_CLOUD) rebuildMenu();
  }

  // ---------------- backup (.indigo, o mesmo formato do site)
  void importBackup(const std::vector<uint8_t>& bytes) {
    flushSave();
    std::vector<uint8_t> saveAntes;
    if (core_ && core_->hasSave()) readFile(savePath_, saveAntes);
    indigo::shelf::ImportResult r;
    try {
      r = indigo::shelf::importBackup(dataDir_, bytes, allLocalRoms());
    } catch (const std::exception& e) {
      // backup estranho não pode derrubar o app
      error(std::string("Backup danificado: ") + e.what());
      return;
    }
    if (!r.error.empty()) { error(r.error); return; }
    if (!mobile_) for (auto it = r.newPaths.rbegin(); it != r.newPaths.rend(); ++it) addRecent(*it);
    refreshLibrary();
    rebuildMenu();
    std::string msg = "Backup restaurado: " + std::to_string(r.romsAdded) + " jogo(s) novo(s), " +
                      std::to_string(r.romsKnown) + " que já estavam aqui";
    if (r.romsMissing) msg += ", " + std::to_string(r.romsMissing) + " sem a ROM no backup (abra a ROM e importe de novo)";
    msg += ".\nSaves: " + std::to_string(r.savesNew) + " novo(s), " + std::to_string(r.savesNewer) + " atualizado(s), " +
           std::to_string(r.savesSame) + " igual(is)";
    if (r.savesOlder) msg += ", " + std::to_string(r.savesOlder) + " mais antigo(s) que o daqui (guardado(s) nas versões anteriores)";
    msg += ".";
    if (r.versions) msg += "\nVersões anteriores trazidas: " + std::to_string(r.versions) + ".";
    if (r.webStates) msg += "\nOs estados salvos do site não vêm junto: o formato é outro.";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Índigo", msg.c_str(), win_);
    if (core_ && core_->hasSave()) {
      // o jogo aberto pode ter recebido um save novo: recarrega só se o arquivo do disco mudou com o backup
      // (comparar com exportSave() falharia em núcleos cujo save muda a cada quadro, como o Game & Watch)
      std::vector<uint8_t> sv;
      if (readFile(savePath_, sv) && sv != saveAntes) { core_->importSave(sv); core_->reset(); status("Save do backup carregado; o jogo foi reiniciado"); }
    }
  }

  void askExport(bool withRoms) {
    static const SDL_DialogFileFilter filter[] = {{"Backup do Índigo", "indigo"}};
    SDL_DateTime d{};
    SDL_Time now = 0;
    SDL_GetCurrentTime(&now);
    SDL_TimeToDateTime(now, &d, true);
    char name[64];
    SDL_snprintf(name, sizeof name, "indigo-backup-%04d-%02d-%02d.indigo", d.year, d.month, d.day);
    exportWithRoms_ = withRoms;
    SDL_ShowSaveFileDialog(
        [](void*, const char* const* list, int) {
          if (!list || !list[0]) return;
          SDL_Event ev{};
          ev.type = EVENT_SAVE_BACKUP;
          ev.user.data1 = new std::string(list[0]);
          SDL_PushEvent(&ev);
        },
        nullptr, win_, mobile_ ? nullptr : filter, mobile_ ? 0 : 1, mobile_ ? nullptr : name);
  }

  void writeBackup(std::string path) {
    flushSave();
    int games = 0;
    const auto data = indigo::shelf::exportBackup(dataDir_, allLocalRoms(), exportWithRoms_, games);
    if (path.rfind("content://", 0) != 0) {
      std::string low = path;
      for (char& c : low) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
      if (low.size() < 7 || low.compare(low.size() - 7, 7, ".indigo") != 0) path += ".indigo";
    }
    if (writeFile(path, data))
      status("Backup salvo: " + std::to_string(games) + " jogo(s)" + (exportWithRoms_ ? " com as ROMs" : ", só saves"));
    else
      error("Não foi possível gravar o backup em:\n" + path);
  }

  void addRecent(const std::string& path) {
    if (mobile_) {
      // a data do arquivo da biblioteca marca o último uso (a cópia é regravada ao abrir de fora)
      std::vector<uint8_t> b;
      if (path.rfind(dataDir_ + "roms/", 0) == 0 && readFile(path, b)) writeFile(path, b);
      refreshLibrary();
      return;
    }
    if (path.rfind("content://", 0) == 0) return;
    library_.erase(std::remove(library_.begin(), library_.end(), path), library_.end());
    library_.insert(library_.begin(), path);
    if (library_.size() > MAX_RECENT) library_.resize(MAX_RECENT);
    std::string s;
    for (const auto& p : library_) s += p + "\n";
    writeFile(dataDir_ + "recentes.txt", std::vector<uint8_t>(s.begin(), s.end()));
  }

  void togglePause() { if (core_) { paused_ = !paused_; if (audio_) SDL_ClearAudioStream(audio_); } }
  void toggleFullscreen() {
    if (mobile_) return;
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreen(win_, fullscreen_);
  }

  // ---------------- menu
  bool touchUiVisible() const { return core_ && !menuOpen_ && (touchActive_ || (mobile_ && pads_.empty())); }
  // o jogo usa os controles de toque (mesmo com o menu aberto por cima): a imagem fica no mesmo lugar
  bool touchUiInUse() const { return core_ && (touchActive_ || (mobile_ && pads_.empty())); }
  bool menuVisible() const { return menuOpen_ || !core_; }

  void openMenu() {
    menuOpen_ = true;
    page_ = P_MAIN;
    touch_.clear();
    rebuildMenu();
    menu_.sel = 0;
    if (audio_) SDL_ClearAudioStream(audio_);
  }
  void closeMenu() { menuOpen_ = false; page_ = P_MAIN; rebuildMenu(); ensureAudio(); }

  // ---------------- som: abre (ou reabre) a saída e garante que ela está tocando
  void ensureAudio() {
    if (!audio_) {
      SDL_AudioSpec spec{SDL_AUDIO_F32, 2, SAMPLE_RATE};
      audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      if (!audio_) { audioError_ = SDL_GetError(); SDL_Log("Índigo: som não abriu: %s", audioError_.c_str()); return; }
      audioError_.clear();
    }
    SDL_ResumeAudioStreamDevice(audio_);
  }

  std::string audioInfo() const {
    const char* drv = SDL_GetCurrentAudioDriver();
    std::string t = std::string("Som: ") + (drv ? drv : "sem driver");
    if (!audio_) return t + " · não abriu" + (audioError_.empty() ? "" : " (" + audioError_ + ")");
    t += SDL_AudioStreamDevicePaused(audio_) ? " · pausado" : " · tocando";
    return t;
  }

  // bipe de meio segundo (lá, 440 Hz) para conferir se o som sai
  void testBeep() {
    ensureAudio();
    if (!audio_) { status("O som não abriu: " + audioError_); return; }
    std::vector<float> b(SAMPLE_RATE / 2 * 2);
    for (size_t i = 0; i < b.size() / 2; i++) {
      const float env = std::min(1.0f, std::min(i, b.size() / 2 - i) / 800.0f);
      b[i * 2] = b[i * 2 + 1] = 0.25f * env * std::sin(6.2831853f * 440.0f * static_cast<float>(i) / SAMPLE_RATE);
    }
    SDL_PutAudioStreamData(audio_, b.data(), static_cast<int>(b.size() * sizeof(float)));
    status(audioInfo());
  }
  void showPage(Page p) { page_ = p; menu_.sel = 0; rebuildMenu(); }

  // "voltar": de uma página para o menu principal; do menu principal para o jogo
  void menuBack() {
    if (page_ == P_PIN) { SDL_StopTextInput(win_); showPage(P_CLOUD); return; }
    if (!core_) {
      // as páginas abertas pelos Ajustes voltam para os Ajustes
      if (page_ == P_BACKUP || page_ == P_CLOUD || page_ == P_CONTROLS || page_ == P_FOLDERS || page_ == P_ABOUT) showPage(P_SETTINGS);
      else if (page_ != P_MAIN) showPage(P_MAIN);
      return;
    }
    if (!menuOpen_) { openMenu(); return; }
    if (page_ != P_MAIN) showPage(P_MAIN); else closeMenu();
  }

  void rebuildMenu() {
    const int keep = menu_.sel;
    menu_.items.clear();
    menu_.preview = nullptr;
    menu_.hint.clear();
    if (core_ && page_ == P_SAVE_STATE) {
      menu_.title = "Salvar estado";
      addSlotItems();
    } else if (core_ && page_ == P_LOAD_STATE) {
      menu_.title = "Carregar estado";
      addSlotItems();
    } else if (core_ && page_ == P_HISTORY) {
      menu_.title = "Versões anteriores do save";
      versions_ = saveVersions();
      for (size_t k = 0; k < versions_.size(); k++)
        menu_.items.push_back({"Voltar ao save de " + shortDate(versions_[k].second * 1000000000LL), M_VERSION + static_cast<int>(k)});
      if (versions_.empty()) menu_.hint = "Ainda não há versões anteriores deste save";
      else menu_.hint = "O save atual tambem fica guardado";
      menu_.items.push_back({"Voltar", M_BACK});
    } else if (page_ == P_CLOUD) {
      menu_.title = "Nuvem dos saves";
      menu_.items = {{"Sincronizar agora", M_CLOUD_SYNC},
                     {std::string("Nuvem: ") + (cloudOn_ ? "ligada" : "desligada"), M_CLOUD_TOGGLE},
                     {cloudPin_.empty() ? "Digitar o PIN..." : "Trocar o PIN...", M_CLOUD_PIN}, {"Voltar", M_BACK}};
      menu_.hint = indigo::ui::toAscii(cloudStatusText());
    } else if (page_ == P_PIN) {
      menu_.title = "PIN da nuvem (o mesmo do site)";
      menu_.items.push_back({"PIN: " + (pinDraft_.empty() ? std::string("(vazio)") : std::string(pinDraft_.size(), '*')), M_PIN_SHOW});
      for (int d = 1; d <= 9; d++) menu_.items.push_back({"Digito " + std::to_string(d), M_PIN_DIGIT + d});
      menu_.items.push_back({"Digito 0", M_PIN_DIGIT});
      menu_.items.push_back({"Apagar o ultimo", M_PIN_DEL});
      menu_.items.push_back({"Confirmar", M_PIN_OK});
      menu_.items.push_back({"Voltar", M_BACK});
      menu_.hint = "Pode digitar no teclado tambem; Enter confirma";
    } else if (page_ == P_SHELF) {
      buildShelf();
      const int total = static_cast<int>(shelfView_.size());
      const int pages = std::max(1, (total + SHELF_PAGE - 1) / SHELF_PAGE);
      shelfPageNo_ = std::clamp(shelfPageNo_, 0, pages - 1);
      menu_.title = "Estante (" + std::to_string(total) + " jogos)" + (pages > 1 ? "  " + std::to_string(shelfPageNo_ + 1) + "/" + std::to_string(pages) : "");
      menu_.items.push_back({"Console: " + (shelfConsole_.empty() ? std::string("todos") : shelfConsole_), M_SHELF_CONSOLE});
      for (int k = shelfPageNo_ * SHELF_PAGE; k < total && k < (shelfPageNo_ + 1) * SHELF_PAGE; k++) {
        const std::string key = displayKey(shelfView_[k]);
        menu_.items.push_back({(favorites_.count(key) ? "* " : "") + indigo::ui::toAscii(key), M_SHELF_ITEM + k});
      }
      if (shelfPageNo_ + 1 < pages) menu_.items.push_back({"Mais jogos >", M_SHELF_NEXT});
      if (shelfPageNo_ > 0) menu_.items.push_back({"< Jogos anteriores", M_SHELF_PREV});
      menu_.items.push_back({"Voltar", M_BACK});
      menu_.hint = total ? "* = favorito (marque no menu do jogo)" : (mobile_ ? "Abra um jogo para ele aparecer aqui" : "Abra um jogo ou adicione uma pasta de jogos");
    } else if (page_ == P_ABOUT) {
      namespace nv = indigo::novidades;
      const int nVers = static_cast<int>(std::size(nv::VERSOES));
      aboutPage_ = std::clamp(aboutPage_, 0, nVers - 1);
      const auto& v = nv::VERSOES[aboutPage_];
      menu_.title = aboutPage_ == 0 ? std::string("Índigo ") + INDIGO_VERSION : std::string("Novidades da ") + v.numero;
      if (aboutPage_ == 0) menu_.items.push_back({std::string("Novidades da ") + v.numero + " (" + v.data + "):", M_INFO});
      for (const char* const* l = v.linhas; *l; l++) menu_.items.push_back({std::string("- ") + indigo::ui::toAscii(*l), M_INFO});
      if (aboutPage_ + 1 < nVers) menu_.items.push_back({"< Versão anterior", M_ABOUT_OLDER});
      if (aboutPage_ > 0) menu_.items.push_back({"Versão seguinte >", M_ABOUT_NEWER});
      if (aboutPage_ == 0) {
        menu_.items.push_back({"Testar o som (bipe)", M_TEST_SOUND});
        menu_.items.push_back({audioInfo(), M_INFO});
      }
      if (aboutPage_ == 0 && indigo::platform::canAssociate())
        menu_.items.push_back({std::string("Abrir jogos com dois cliques: ") + (indigo::platform::filesAssociated() ? "sim" : "não"), M_ASSOCIATE});
      menu_.items.push_back({"Voltar", M_BACK});
      menu_.hint = "Emulador de SNES, NES, GB, GBC, GBA, Master System, Game Gear, Neo Geo Pocket, Pokémon Mini, Atari 2600, Atari 5200, MSX, ColecoVision, SG-1000, WonderSwan, Atari 7800, Vectrex, Atari Lynx, Virtual Boy, Mega Duck, Watara Supervision, PC Engine e Mega Drive";
    } else if (page_ == P_FOLDERS) {
      menu_.title = "Pastas de jogos";
      menu_.items.push_back({"Adicionar pasta...", M_FOLDER_ADD});
      for (size_t k = 0; k < folders_.size(); k++) menu_.items.push_back({"Tirar " + indigo::ui::toAscii(folders_[k]), M_FOLDER_DEL + static_cast<int>(k)});
      menu_.items.push_back({"Voltar", M_BACK});
      menu_.hint = "Os jogos dessas pastas (e subpastas) aparecem na estante";
    } else if (page_ == P_CONTROLS) {
      menu_.title = "Controles";
      menu_.items = {{"Configurar o controle", M_MAP_PAD}, {"Configurar o teclado", M_MAP_KEYS},
                     {indigo::ui::player2Label(player2_), M_PLAYER2},
                     {"Voltar ao padrão (controle e teclado)", M_MAP_DEFAULT}, {"Voltar", M_BACK}};
      menu_.hint = "Atalhos no controle: Select+R salva estado, Select+L carrega, Select+setas troca o espaço";
    } else if (page_ == P_CAPTURE) {
      menu_.title = std::string(captureKeys_ ? "Aperte a tecla para: " : "Aperte o botão para: ") + ACTION_NAMES[captureStep_];
      menu_.items = {{"(esperando...)", M_CAPTURE_INFO}, {"Pular (manter o atual)", M_CAPTURE_SKIP}, {"Cancelar", M_CAPTURE_CANCEL}};
      if (!captureKeys_ && captureStep_ == ACT_TURBO) menu_.hint = "Pode ser o gatilho direito; Pular deixa o gatilho";
      else menu_.hint = captureKeys_ ? "Esc cancela" : "Use o direcional para Pular ou Cancelar";
    } else if (page_ == P_BACKUP) {
      menu_.title = "Backup (.indigo)";
      menu_.items = {{"Exportar com as ROMs", M_EXPORT_ROMS}, {"Exportar só os saves", M_EXPORT_SAVES},
                     {"Importar backup...", M_IMPORT}, {"Voltar", M_BACK}};
      menu_.hint = "O mesmo formato do site: leva jogos, saves e versões anteriores";
    } else if (core_) {
      page_ = P_MAIN;
      menu_.title = indigo::ui::toAscii(core_->title().empty() ? romBase_ : core_->title());
      menu_.items = {{"Voltar ao jogo", M_RESUME}, {"Salvar estado", M_SAVE_PAGE}, {"Carregar estado", M_LOAD_PAGE},
                     {std::string("Acelerar 4x: ") + (fastOn_ ? "ligado" : "desligado"), M_FAST}, {"Reiniciar o jogo", M_RESET}};
      const auto* gbc = dynamic_cast<indigo::gb::GBCore*>(core_.get());
      if (monoPalette() && !(gbc && gbc->sgbActive()))
        menu_.items.push_back({"Paleta: " + indigo::ui::toAscii(PALETTES[palette_].label), M_PALETTE});
      menu_.items.push_back({std::string("Filtro de tela: ") + indigo::video::filterName(filter_), M_FILTER});
      menu_.items.push_back({std::string("Tamanho da imagem: ") + (integerScale_ ? "pixels perfeitos" : "preencher a tela"), M_INTEGER});
      if (core_->system() == "ps1") menu_.items.push_back({ps1ResLabel(), M_PS1_RES});
      if (core_->system() == "nds") menu_.items.push_back({indigo::ui::dsLayoutLabel(dsMode_), M_DS_LAYOUT});
      if (core_->diskSides() > 0)
        menu_.items.push_back({"Disco: " + diskSideName(core_->diskSide()) + " (trocar: F4)", M_DISK_SIDE});
      if (auto* vbc = dynamic_cast<indigo::vb::VBCore*>(core_.get())) {
        static const char* const MODES[] = {"olho esquerdo", "3D anaglifo (óculos vermelho/ciano)", "lado a lado"};
        menu_.items.push_back({std::string("Tela do Virtual Boy: ") + MODES[std::clamp(vbc->stereoMode(), 0, 2)], M_VB_3D});
      }
      menu_.items.push_back({std::string("Favorito: ") + (favorites_.count(romKey_) ? "sim" : "não"), M_FAV});
      menu_.items.push_back({"Usar esta tela como capa", M_COVER});
      menu_.items.push_back({"Controles...", M_CONTROLS_PAGE});
      if (core_->hasSave()) menu_.items.push_back({"Versões anteriores do save", M_HISTORY_PAGE});
      menu_.items.push_back({"Nuvem dos saves...", M_CLOUD_PAGE});
      menu_.items.push_back({"Abrir outro jogo", M_OPEN});
      menu_.items.push_back({frontend_ ? "Fechar o jogo e voltar" : "Fechar o jogo", M_CLOSE});
      if (!mobile_) menu_.items.push_back({"Sair do Índigo", M_QUIT});
    } else if (page_ == P_SETTINGS) {
      menu_.title = "Ajustes";
      menu_.items.push_back({mobile_ ? "Abrir jogo..." : "Abrir jogo... (Ctrl+O)", M_OPEN});
      if (!mobile_) menu_.items.push_back({"Pastas de jogos...", M_FOLDERS_PAGE});
      menu_.items.push_back({"Backup (.indigo)...", M_BACKUP_PAGE});
      menu_.items.push_back({"Nuvem dos saves...", M_CLOUD_PAGE});
      menu_.items.push_back({"Controles...", M_CONTROLS_PAGE});
      if (!mobile_) menu_.items.push_back({std::string("Tela cheia ao abrir: ") + (startFullscreen_ ? "sim" : "não"), M_START_FULL});
      menu_.items.push_back({std::string("Tamanho da imagem: ") + (integerScale_ ? "pixels perfeitos" : "preencher a tela"), M_INTEGER});
      menu_.items.push_back({ps1ResLabel(), M_PS1_RES});
      menu_.items.push_back({indigo::ui::dsLayoutLabel(dsMode_), M_DS_LAYOUT});
      menu_.items.push_back({std::string("Super Game Boy (cores e moldura): ") + (sgb_ ? "sim" : "não"), M_SGB});
      {
        static const char* const SIZES[] = {"automático", "pequeno", "grande", "maior"};
        menu_.items.push_back({std::string("Tamanho da interface: ") + SIZES[std::clamp(uiSize_, 0, 3)], M_UI_SIZE});
      }
      menu_.items.push_back({"Sobre e novidades...", M_ABOUT_PAGE});
      if (!mobile_) menu_.items.push_back({"Sair do Índigo", M_QUIT});
      menu_.items.push_back({"Voltar", M_BACK});
    } else {
      // tela inicial: a biblioteca (desenhada por ui::Library)
      page_ = P_MAIN;
      buildLibrary();
    }
    menu_.sel = std::clamp(keep, 0, std::max(0, static_cast<int>(menu_.items.size()) - 1));
    updateSlotPreview();
  }

  // nome que identifica o jogo de um caminho da lista (no PC um .zip guarda o nome da ROM de dentro)
  std::string displayKey(const std::string& path) {
    std::string n = fileName(path);
    std::string low = n;
    for (char& c : low) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
    if (low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0) {
      auto it = zipNames_.find(path);
      if (it != zipNames_.end()) return it->second;
      std::vector<uint8_t> b, data;
      std::string inner, err;
      // jogo de arcade: o nome do próprio .zip (sf2.zip) identifica o jogo
      if (readFile(path, b) && !indigo::isArcadeSet(b, n) && indigo::zip::extractRom(b, inner, data, err)) n = inner;
      zipNames_[path] = n;
    }
    return n;
  }

  void addSlotItems() {
    for (int s = 1; s <= 9; s++) {
      SDL_Time t = 0;
      const bool has = fileTime(statePath(s), t) || fileTime(legacyStatePath(s), t);
      menu_.items.push_back({std::to_string(s) + "  " + (has ? shortDate(t) : std::string("vazio")), M_SLOT + s});
    }
    menu_.items.push_back({"Voltar", M_BACK});
  }

  // imagem ao lado do menu: miniatura do estado (páginas de estado) ou capa do jogo (tela inicial e estante)
  void updateSlotPreview() {
    const int id = menu_.selectedId();
    if (id > M_SLOT && id <= M_SLOT + 9) { menu_.preview = thumb(id - M_SLOT); return; }
    std::string path;
    if (id == M_CONTINUE && !library_.empty()) path = library_[0];
    else if (id >= M_RECENT && id < M_VERSION && id - M_RECENT < static_cast<int>(library_.size())) path = library_[id - M_RECENT];
    else if (id >= M_SHELF_ITEM && id - M_SHELF_ITEM < static_cast<int>(shelfView_.size())) path = shelfView_[id - M_SHELF_ITEM];
    menu_.preview = path.empty() ? nullptr : cover(displayKey(path));
  }

  void moveSel(int d) {
    menu_.move(d);
    updateSlotPreview();
  }

  // ---------------- estante: capas, favoritos e pastas de jogos
  std::string coverPath(const std::string& key) const { return dataDir_ + "capas/" + key + ".thumb"; }

  void writeCover() {
    if (!core_) return;
    const std::vector<uint8_t> th = thumbBytes();
    writeFile(coverPath(romKey_), th);
    if (auto it = covers_.find(romKey_); it != covers_.end()) { if (it->second) SDL_DestroyTexture(it->second); covers_.erase(it); }
  }

  SDL_Texture* cover(const std::string& key) {
    if (auto it = covers_.find(key); it != covers_.end()) return it->second;
    SDL_Texture* t = loadThumb(coverPath(key));
    covers_[key] = t;
    return t;
  }

  void loadFavorites() {
    favorites_.clear();
    std::vector<uint8_t> b;
    if (!readFile(dataDir_ + "favoritos.txt", b)) return;
    std::string cur;
    for (uint8_t c : b) { if (c == '\n') { if (!cur.empty()) favorites_.insert(cur); cur.clear(); } else if (c != '\r') cur += static_cast<char>(c); }
    if (!cur.empty()) favorites_.insert(cur);
  }
  void saveFavorites() {
    std::string t;
    for (const auto& f : favorites_) t += f + "\n";
    writeFile(dataDir_ + "favoritos.txt", std::vector<uint8_t>(t.begin(), t.end()));
  }

  void loadFolders() {
    folders_.clear();
    std::vector<uint8_t> b;
    if (!readFile(configDir_ + "pastas.txt", b)) return;
    std::string cur;
    for (uint8_t c : b) { if (c == '\n') { if (!cur.empty()) folders_.push_back(cur); cur.clear(); } else if (c != '\r') cur += static_cast<char>(c); }
    if (!cur.empty()) folders_.push_back(cur);
  }
  void saveFolders() {
    std::string t;
    for (const auto& f : folders_) t += f + "\n";
    writeFile(configDir_ + "pastas.txt", std::vector<uint8_t>(t.begin(), t.end()));
  }
  void addFolder(const std::string& dir) {
    if (std::find(folders_.begin(), folders_.end(), dir) == folders_.end()) folders_.push_back(dir);
    saveFolders();
    shelfDirty_ = true;
    status("Pasta adicionada: os jogos dela estão na estante");
    rebuildMenu();
  }

  static bool romExtension(const std::string& name) { return indigo::app::shelfExtension(name); }

  // console de um CD: olha a imagem uma vez (o resultado fica guardado); no celular, só os da biblioteca do app
  std::string discConsole(const std::string& path) {
    const std::string k = displayKey(path);
    auto it = discSys_.find(k);
    if (it != discSys_.end()) return it->second;
    std::string sys;
    if (path.rfind("content://", 0) != 0) {
      if (auto d = indigo::cdrom::open(path, nullptr)) sys = indigo::cdrom::detectSystem(*d);
    }
    discSys_[k] = sys;
    return sys;
  }

  // console pela extensão do arquivo da ROM (rápido: não lê o arquivo; .zip olha o nome de dentro)
  std::string consoleOf(const std::string& path) {
    const std::string key = displayKey(path);
    // console de um jogo já aberto: vale para .bin, .rom e .zip, que a extensão não decide
    if (auto it = playedSys_.find(key); it != playedSys_.end()) return it->second;
    const std::string e = indigo::app::extOf(key);
    if (e == ".zip") return indigo::app::lowerAscii(key).rfind("gnw_", 0) == 0 ? "G&W" : "ARCADE";  // Game & Watch: pacotes gnw_* do MAME
    if (e == ".gdi") return "DC";
    if (e == ".cue" || e == ".chd" || e == ".iso") {
      const std::string d = discConsole(path);
      return d == "ps1" ? "PS1" : d == "3do" ? "3DO" : d == "pcfx" ? "PCFX" : d == "saturn" ? "SATURN" : d == "dc" ? "DC" : "CD";
    }
    if (e == ".rom") return romConsole(path);
    const std::string t = indigo::app::tabOfExt(e);
    return t.empty() ? "outros" : t;
  }

  // .rom é do MSX, do Intellicart (Intellivision) ou do Jaguar: olha o arquivo uma vez (até 8 MB; no celular,
  // só os da biblioteca do app)
  std::string romConsole(const std::string& path) {
    const std::string k = displayKey(path);
    if (auto it = romSys_.find(k); it != romSys_.end()) return it->second;
    std::string tab = "MSX";
    std::vector<uint8_t> b;
    SDL_PathInfo info{};
    if (path.rfind("content://", 0) != 0 && SDL_GetPathInfo(path.c_str(), &info) && info.size <= 8u * 1024 * 1024 &&
        readFile(path, b)) {
      const std::string n = indigo::app::lowerAscii(k);
      if (indigo::jaguar::isJaguarRom(b, n)) tab = "JAGUAR";
      else if (indigo::intv::isIntvRom(b, n)) tab = "INTV";
    }
    romSys_[k] = tab;
    return tab;
  }

  // lembra o console de cada jogo aberto (arquivo consoles.txt: "nome<TAB>aba" por linha)
  void loadPlayedSys() {
    std::vector<uint8_t> b;
    if (!readFile(dataDir_ + "consoles.txt", b)) return;
    std::string line;
    for (size_t i = 0; i <= b.size(); i++) {
      if (i == b.size() || b[i] == '\n') {
        const auto t = line.find('\t');
        if (t != std::string::npos && t > 0 && playedSys_.size() < 5000) playedSys_[line.substr(0, t)] = line.substr(t + 1);
        line.clear();
      } else if (b[i] != '\r') {
        line += static_cast<char>(b[i]);
      }
    }
  }
  void rememberConsole(const std::string& recent) {
    if (!core_ || recent.empty()) return;
    const std::string key = displayKey(recent), tab = indigo::app::tabOfSystem(core_->system());
    if (key.empty() || key.find_first_of("\t\n\r") != std::string::npos) return;
    auto it = playedSys_.find(key);
    if (it != playedSys_.end() && it->second == tab) return;
    playedSys_[key] = tab;
    std::string s;
    for (const auto& [k, v] : playedSys_) s += k + "\t" + v + "\n";
    writeFile(dataDir_ + "consoles.txt", std::vector<uint8_t>(s.begin(), s.end()));
  }

  // todos os jogos conhecidos: recentes, pasta do app e pastas de jogos; favoritos primeiro, depois pelo nome
  void buildShelf() {
    if (shelfDirty_) {
      std::vector<std::string> all = allLocalRoms();
      for (const auto& dir : folders_) {
        int count = 0;
        char** names = SDL_GlobDirectory(dir.c_str(), nullptr, 0, &count);  // inclui subpastas
        for (int k = 0; names && k < count && all.size() < 5000; k++)
          if (romExtension(names[k])) all.push_back(dir + "/" + names[k]);
        SDL_free(names);
      }
      shelfAll_.clear();
      std::vector<std::string> keys;
      for (const auto& p : all) {
        const std::string k = displayKey(p);
        if (std::find(keys.begin(), keys.end(), k) != keys.end()) continue;
        keys.push_back(k);
        shelfAll_.push_back(p);
      }
      auto lowerKey = [&](const std::string& p) {
        std::string k = displayKey(p);
        for (char& c : k) c = static_cast<char>(SDL_tolower(static_cast<unsigned char>(c)));
        return k;
      };
      std::stable_sort(shelfAll_.begin(), shelfAll_.end(), [&](const std::string& a, const std::string& b) {
        const bool fa = favorites_.count(displayKey(a)) > 0, fb = favorites_.count(displayKey(b)) > 0;
        if (fa != fb) return fa;
        return lowerKey(a) < lowerKey(b);
      });
      shelfDirty_ = false;
    }
    shelfView_.clear();
    for (const auto& p : shelfAll_) if (shelfConsole_.empty() || consoleOf(p) == shelfConsole_) shelfView_.push_back(p);
  }

  void activate(int id) {
    if (id < 0) return;
    indigo::platform::haptic();
    if (id > M_SLOT && id <= M_SLOT + 9) {
      if (page_ == P_SAVE_STATE) saveState(id - M_SLOT); else loadState(id - M_SLOT);
      closeMenu();
      return;
    }
    if (id >= M_VERSION && id < M_FOLDER_DEL && id - M_VERSION < static_cast<int>(versions_.size())) {
      restoreVersion(versions_[id - M_VERSION].first);
      closeMenu();
      return;
    }
    if (id >= M_PIN_DIGIT && id <= M_PIN_DIGIT + 9) {
      if (pinDraft_.size() < 64) pinDraft_ += static_cast<char>('0' + (id - M_PIN_DIGIT));
      rebuildMenu();
      return;
    }
    if (id >= M_SHELF_ITEM) {
      if (id - M_SHELF_ITEM < static_cast<int>(shelfView_.size())) openFile(shelfView_[id - M_SHELF_ITEM]);
      return;
    }
    if (id >= M_FOLDER_DEL && id < M_SHELF_ITEM) {
      if (id - M_FOLDER_DEL < static_cast<int>(folders_.size())) { folders_.erase(folders_.begin() + (id - M_FOLDER_DEL)); saveFolders(); shelfDirty_ = true; rebuildMenu(); }
      return;
    }
    if (id >= M_RECENT && id < M_VERSION) {
      if (id - M_RECENT < static_cast<int>(library_.size())) openFile(library_[id - M_RECENT]);
      return;
    }
    switch (id) {
      case M_RESUME: closeMenu(); break;
      case M_SAVE_PAGE: showPage(P_SAVE_STATE); menu_.sel = slot_ - 1; updateSlotPreview(); break;
      case M_LOAD_PAGE: showPage(P_LOAD_STATE); menu_.sel = slot_ - 1; updateSlotPreview(); break;
      case M_HISTORY_PAGE: showPage(P_HISTORY); break;
      case M_BACK:
        if (page_ == P_PIN) { SDL_StopTextInput(win_); showPage(P_CLOUD); break; }
        menuBack();
        break;
      case M_BACKUP_PAGE: showPage(P_BACKUP); break;
      case M_CLOUD_PAGE: showPage(P_CLOUD); break;
      case M_FILTER:
        filter_ = (filter_ + 1) % indigo::video::NUM_FILTERS;
        saveSettings();
        rebuildMenu();
        break;
      case M_INTEGER:
        // escala inteira: todos os pixels do console do mesmo tamanho (pode sobrar borda); senão preenche a tela
        integerScale_ = !integerScale_;
        saveSettings();
        rebuildMenu();
        break;
      case M_SGB:
        // vale para os jogos abertos depois (a tela muda de tamanho)
        sgb_ = !sgb_;
        indigo::gb::setSgbEnabled(sgb_);
        saveSettings();
        rebuildMenu();
        status(sgb_ ? "Super Game Boy ligado: vale ao abrir o jogo" : "Super Game Boy desligado: vale ao abrir o jogo");
        break;
      case M_UI_SIZE:
        uiSize_ = (uiSize_ + 1) % 4;
        saveSettings();
        rebuildMenu();
        break;
      case M_PS1_RES:
        // 1x -> 2x -> 4x -> 1x; vale na hora para o jogo aberto (o estado salvo não muda)
        ps1Res_ = ps1Res() == 4 ? 1 : ps1Res() * 2;
        applyPs1Res();
        saveSettings();
        rebuildMenu();
        break;
      case M_CONTROLS_PAGE: showPage(P_CONTROLS); break;
      case M_SHELF_PAGE: shelfPageNo_ = 0; shelfDirty_ = true; showPage(P_SHELF); break;
      case M_SHELF_NEXT: shelfPageNo_++; menu_.sel = 0; rebuildMenu(); break;
      case M_SHELF_PREV: shelfPageNo_--; menu_.sel = 0; rebuildMenu(); break;
      case M_SHELF_CONSOLE: {
        // passa para o próximo console que tem jogo na estante
        std::vector<std::string> cons = {""};
        for (const auto& p : shelfAll_) { const std::string c = consoleOf(p); if (std::find(cons.begin(), cons.end(), c) == cons.end()) cons.push_back(c); }
        auto it = std::find(cons.begin(), cons.end(), shelfConsole_);
        shelfConsole_ = (it == cons.end() || it + 1 == cons.end()) ? cons[0] : *(it + 1);
        shelfPageNo_ = 0;
        rebuildMenu();
        break;
      }
      case M_DISK_SIDE: nextDiskSide(); rebuildMenu(); break;
      case M_VB_3D:
        if (auto* vbc = dynamic_cast<indigo::vb::VBCore*>(core_.get())) { vbc->setStereoMode((vbc->stereoMode() + 1) % 3); rebuildMenu(); }
        break;
      case M_FAV:
        if (favorites_.count(romKey_)) favorites_.erase(romKey_); else favorites_.insert(romKey_);
        saveFavorites();
        rebuildMenu();
        break;
      case M_COVER: writeCover(); status("Capa trocada"); break;
      case M_FOLDERS_PAGE: showPage(P_FOLDERS); break;
      case M_SETTINGS_PAGE: showPage(P_SETTINGS); break;
      case M_ABOUT_PAGE: aboutPage_ = 0; showPage(P_ABOUT); break;
      case M_ABOUT_OLDER: aboutPage_++; menu_.sel = 0; rebuildMenu(); break;
      case M_ABOUT_NEWER: aboutPage_--; menu_.sel = 0; rebuildMenu(); break;
      case M_INFO: break;
      case M_TEST_SOUND: testBeep(); rebuildMenu(); break;
      case M_START_FULL:
        startFullscreen_ = !startFullscreen_;
        saveSettings();
        if (startFullscreen_ != fullscreen_) toggleFullscreen();
        rebuildMenu();
        break;
      case M_ASSOCIATE: {
        const bool on = !indigo::platform::filesAssociated();
        if (indigo::platform::associateFiles(on))
          status(on ? "Pronto: dois cliques num jogo abrem o Índigo" : "Os jogos não abrem mais com o Índigo");
        else
          status("O Windows não deixou mudar (tente de novo)");
        rebuildMenu();
        break;
      }
      case M_FOLDER_ADD:
        SDL_ShowOpenFolderDialog(
            [](void*, const char* const* list, int) {
              if (!list || !list[0]) return;
              SDL_Event ev{};
              ev.type = EVENT_ADD_FOLDER;
              ev.user.data1 = new std::string(list[0]);
              SDL_PushEvent(&ev);
            },
            nullptr, win_, nullptr, false);
        break;
      case M_MAP_PAD:
        if (pads_.empty()) { status("Ligue um controle primeiro"); break; }
        startCapture(false);
        break;
      case M_MAP_KEYS: startCapture(true); break;
      case M_DS_LAYOUT:
        // automático -> empilhadas -> lado a lado -> a de cima grande -> automático
        dsMode_ = static_cast<indigo::ui::DsLayout>((static_cast<int>(dsMode_) + 1) % 4);
        saveSettings();
        rebuildMenu();
        break;
      case M_PLAYER2:
        // 2º controle -> teclado -> desligado -> 2º controle
        player2_ = player2_ == indigo::ui::Player2::SecondPad ? indigo::ui::Player2::Keyboard
                 : player2_ == indigo::ui::Player2::Keyboard ? indigo::ui::Player2::Off : indigo::ui::Player2::SecondPad;
        saveSettings();
        rebuildMenu();
        break;
      case M_MAP_DEFAULT: defaultPadMap(); defaultKeyMap(); saveMaps(); status("Controle e teclado no padrão"); break;
      case M_CAPTURE_SKIP:
        // mantém o que já estava para esta ação
        if (captureKeys_) captureNewKeys_[captureStep_] = keyMap_[captureStep_];
        else { captureNewPad_[captureStep_] = padMap_[captureStep_]; if (captureStep_ == ACT_TURBO) captureTrigger_ = padTriggerTurbo_; }
        captureNext();
        break;
      case M_CAPTURE_CANCEL: showPage(P_CONTROLS); break;
      case M_CAPTURE_INFO: break;
      case M_CLOUD_SYNC: cloudSync("pedido no menu", true); rebuildMenu(); break;
      case M_CLOUD_TOGGLE:
        cloudOn_ = !cloudOn_;
        saveCloudSettings();
        if (cloudOn_) { cloudState_ = CS_IDLE; cloud_->ping(); } else cloudState_ = CS_OFF;
        rebuildMenu();
        break;
      case M_CLOUD_PIN: pinDraft_.clear(); showPage(P_PIN); SDL_StartTextInput(win_); break;
      case M_PIN_DEL: if (!pinDraft_.empty()) pinDraft_.pop_back(); rebuildMenu(); break;
      case M_PIN_OK:
        if (pinDraft_.empty()) break;
        SDL_StopTextInput(win_);
        cloud_->auth(pinDraft_);
        status("Conferindo o PIN...");
        showPage(P_CLOUD);
        break;
      case M_PIN_SHOW: break;
      case M_EXPORT_ROMS: askExport(true); showPage(P_MAIN); break;
      case M_EXPORT_SAVES: askExport(false); showPage(P_MAIN); break;
      case M_IMPORT: showOpenDialog(); showPage(P_MAIN); break;
      case M_FAST: fastOn_ = !fastOn_; rebuildMenu(); break;
      case M_RESET: if (core_) { core_->reset(); status("Jogo reiniciado"); } closeMenu(); break;
      case M_PALETTE: palette_ = (palette_ + 1) % NUM_PALETTES; applyPalette(); rebuildMenu(); break;
      case M_OPEN: showOpenDialog(); break;
      case M_CONTINUE: if (!library_.empty()) openFile(library_[0], true); break;
      case M_CLOSE:
        closeGame();
        if (frontend_) { running_ = false; break; }  // aberto por um frontend (ES-DE, Daijisho, Playnite): volta para ele
        menuOpen_ = false;
        page_ = P_MAIN;
        refreshLibrary();
        rebuildMenu();
        menu_.sel = 0;
        break;
      case M_QUIT: running_ = false; break;
      default: break;
    }
  }

  void menuTap(float x, float y) {
    const int id = menu_.hit(x, y);
    if (id >= 0) {
      for (size_t k = 0; k < menu_.items.size(); k++) if (menu_.items[k].id == id) menu_.sel = static_cast<int>(k);
      activate(id);
    } else if (id == -2 && (mobile_ || !core_)) {
      menuBack();  // toque fora do painel volta (no PC, durante o jogo, o clique fora não faz nada, para não fechar sem querer)
    }
  }

  void onFinger(const SDL_TouchFingerEvent& f, Uint32 type) {
    if (f.touchID == SDL_MOUSE_TOUCHID && !simulated_) return;  // toque simulado pelo mouse: o mouse já é tratado
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(ren_, &ow, &oh);
    const float x = f.x * static_cast<float>(ow), y = f.y * static_cast<float>(oh);
    if (type == SDL_EVENT_FINGER_DOWN) {
      touchActive_ = true;
      if (usingPad_) { usingPad_ = false; if (libraryMode()) buildLibrary(); }
      if (menuVisible()) {
        // menus e biblioteca: o toque vale ao soltar o dedo sem arrastar (arrastar rola a lista)
        if (!gActive_) {
          gActive_ = true; gFinger_ = f.fingerID; gx0_ = x; gy0_ = y; gLast_ = y; gLastX_ = x; gDrag_ = false;
          applyMetrics(ow, oh);
          gTabs_ = libraryMode() && lib_.inTabs(x, y);  // arrastar nas abas rola as abas para os lados
        }
        return;
      }
      if (!core_) return;
      layoutTouch(ow, oh, true);
      const auto a = touch_.fingerDown(f.fingerID, x, y);
      if (a == indigo::ui::TouchAction::None && (core_->system() == "gamecom" || core_->system() == "nds") && !penActive_) {
        // tela de toque (Game.com, tela de baixo do DS): o dedo na imagem do jogo é a caneta
        const SDL_FRect g = gameRect(ow, oh, true);
        if (x >= g.x && y >= g.y && x < g.x + g.w && y < g.y + g.h) { penActive_ = true; penFinger_ = f.fingerID; penX_ = x; penY_ = y; }
      }
      if (a == indigo::ui::TouchAction::Menu) { indigo::platform::haptic(); openMenu(); }
      else if (a == indigo::ui::TouchAction::Disk) { indigo::platform::haptic(); nextDiskSide(); }
      else if (a == indigo::ui::TouchAction::Control) indigo::platform::haptic();
    } else if (type == SDL_EVENT_FINGER_MOTION) {
      if (gActive_ && f.fingerID == gFinger_) {
        if (!gDrag_ && std::hypot(x - gx0_, y - gy0_) > std::min(ow, oh) * 0.025f) gDrag_ = true;
        if (gDrag_) {
          if (gTabs_ && libraryMode()) lib_.scrollTabs(gLastX_ - x);
          else if (libraryMode()) lib_.scrollBy(gLast_ - y);
          else menu_.scrollBy(gLast_ - y);
        }
        gLast_ = y;
        gLastX_ = x;
        return;
      }
      if (penActive_ && f.fingerID == penFinger_) { penX_ = x; penY_ = y; }
      touch_.fingerMove(f.fingerID, x, y);
    } else {
      if (gActive_ && f.fingerID == gFinger_) {
        gActive_ = false;
        if (type == SDL_EVENT_FINGER_UP && !gDrag_ && menuVisible()) uiTap(gx0_, gy0_);
        return;
      }
      if (penActive_ && f.fingerID == penFinger_) penActive_ = false;
      touch_.fingerUp(f.fingerID);
    }
  }

  // caneta do Game.com: dedo na imagem do jogo ou, no PC, o botão esquerdo do mouse sobre ela
  bool penActive_ = false;
  SDL_FingerID penFinger_ = 0;
  float penX_ = 0, penY_ = 0;
  void penInput() {
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(ren_, &ow, &oh);
    float x = penX_, y = penY_;
    bool down = penActive_;
    if (!down && !mobile_) {
      float wx = 0, wy = 0;
      if (SDL_GetMouseState(&wx, &wy) & SDL_BUTTON_LMASK) {
        SDL_RenderCoordinatesFromWindow(ren_, wx, wy, &x, &y);
        down = true;
      }
    }
    const SDL_FRect g = gameRect(ow, oh, touchUiInUse());
    int px = 0, py = 0;  // arredonda para baixo: meio pixel fora da borda não toca na tela
    if (core_->system() == "nds") {
      // DS: o ponto na imagem montada vira o ponto na tela de baixo (o núcleo recebe y de 192 a 383)
      const float fx = std::floor((x - g.x) * float(viewW()) / g.w), fy = std::floor((y - g.y) * float(viewH()) / g.h);
      int tx = 0, ty = 0;
      const bool inside = g.w > 0 && g.h > 0 && indigo::ui::dsToTouch(dsUsed_, int(fx), int(fy), tx, ty);
      px = inside ? tx : 0;
      py = inside ? ty + 192 : 0;
      down = inside && down;
    } else {
      down = indigo::gamecom::pointerToLcd(x, y, g.x, g.y, g.w, g.h, px, py) && down;
    }
    core_->setPointer(px, py, down);
  }

  // mouse do PC-FX: o mouse do PC sobre a imagem do jogo (movimento em pixels da imagem, botões esquerdo e direito)
  int mouseLastX_ = 0, mouseLastY_ = 0;
  bool mouseInside_ = false;
  void mouseInput() {
    if (mobile_) return;
    float wx = 0, wy = 0, x = 0, y = 0;
    const SDL_MouseButtonFlags b = SDL_GetMouseState(&wx, &wy);
    SDL_RenderCoordinatesFromWindow(ren_, wx, wy, &x, &y);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(ren_, &ow, &oh);
    const SDL_FRect g = gameRect(ow, oh, touchUiInUse());
    if (g.w <= 0 || g.h <= 0) return;
    const int px = static_cast<int>((x - g.x) * static_cast<float>(core_->width()) / g.w);
    const int py = static_cast<int>((y - g.y) * static_cast<float>(core_->height()) / g.h);
    const bool inside = px >= 0 && py >= 0 && px < core_->width() && py < core_->height() &&
                        (SDL_GetWindowFlags(win_) & SDL_WINDOW_INPUT_FOCUS);
    int dx = 0, dy = 0;
    uint32_t buttons = 0;
    if (inside) {
      if (mouseInside_) { dx = px - mouseLastX_; dy = py - mouseLastY_; }
      if (b & SDL_BUTTON_LMASK) buttons |= 1;
      if (b & SDL_BUTTON_RMASK) buttons |= 2;
    }
    mouseInside_ = inside;
    mouseLastX_ = px;
    mouseLastY_ = py;
    core_->setMouse(dx, dy, buttons);
  }

  // toque/clique em menu ou biblioteca; um segundo toque logo em seguida é ignorado (toque duplo sem querer)
  void uiTap(float x, float y) {
    const uint64_t now = SDL_GetTicks();
    if (now - lastTap_ < 280) return;
    lastTap_ = now;
    if (libraryMode()) libActivate(lib_.hit(x, y));
    else menuTap(x, y);
  }

  // ---------------- biblioteca (tela inicial)
  bool libraryMode() const { return !core_ && page_ == P_MAIN; }

  static std::string prettyName(const std::string& key) {
    std::string n = key;
    const auto dot = n.find_last_of('.');
    if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);
    for (char& c : n) if (c == '_') c = ' ';
    return n;
  }

  void buildLibrary() {
    buildShelf();
    // ordem: jogados por último primeiro; depois o resto (favoritos primeiro, por nome)
    std::vector<std::string> all;
    std::set<std::string> keys;
    auto add = [&](const std::string& p) { if (keys.insert(displayKey(p)).second) all.push_back(p); };
    for (const auto& p : library_) add(p);
    for (const auto& p : shelfAll_) add(p);
    std::vector<std::string> tabs = {"Todos"};
    bool anyFav = false;
    for (const auto& p : all) if (favorites_.count(displayKey(p))) { anyFav = true; break; }
    if (anyFav) tabs.push_back("Favoritos");
    for (const char* c : indigo::app::TAB_ORDER)
      for (const auto& p : all) if (consoleOf(p) == c) { tabs.push_back(c); break; }
    int ti = 0;
    for (size_t k = 0; k < tabs.size(); k++) if (tabs[k] == libTabName_) ti = static_cast<int>(k);
    libTabName_ = tabs[ti];
    lib_.tabs = tabs;
    lib_.tab = ti;
    lib_.games.clear();
    libPaths_.clear();
    for (const auto& p : all) {
      const std::string key = displayKey(p), con = consoleOf(p);
      const bool fav = favorites_.count(key) > 0;
      if (libTabName_ == "Favoritos" ? !fav : (libTabName_ != "Todos" && con != libTabName_)) continue;
      lib_.games.push_back({prettyName(key), con, cover(key), fav, static_cast<int>(libPaths_.size())});
      libPaths_.push_back(p);
    }
    lib_.hasContinue = false;
    if (!library_.empty()) {
      SDL_Time t = 0;
      const std::string key = displayKey(library_[0]), con = consoleOf(library_[0]);
      const bool fav = favorites_.count(key) > 0;
      const bool inTab = libTabName_ == "Todos" || con == libTabName_ || (libTabName_ == "Favoritos" && fav);
      if (inTab && fileTime(autoStateFor(dataDir_, key), t)) {
        lib_.hasContinue = true;
        lib_.cont = {prettyName(key), con, cover(key), fav, -1};
      }
    }
    const int n = static_cast<int>(lib_.games.size());
    lib_.focus = std::clamp(lib_.focus, lib_.hasContinue ? -1 : 0, std::max(0, n - 1));
    if (n == 0 && lib_.hasContinue) lib_.focus = -1;
    lib_.desktopDrop = !mobile_;
    lib_.showFocus = usingPad_ || !mobile_;
    if (usingPad_) lib_.hint = "A: jogar  ·  L/R: console  ·  X: favorito  ·  Start: ajustes";
    else if (!mobile_) lib_.hint = "Enter: jogar  ·  Tab: console  ·  F: favorito  ·  Esc: ajustes";
    else lib_.hint.clear();
  }

  void libSetTab(int i) {
    if (lib_.tabs.empty()) return;
    i = (i + static_cast<int>(lib_.tabs.size())) % static_cast<int>(lib_.tabs.size());
    libTabName_ = lib_.tabs[i];
    lib_.focus = 0;
    lib_.resetScroll();
    buildLibrary();
  }

  void libActivate(indigo::ui::Library::Hit h) {
    using L = indigo::ui::Library;
    switch (h.kind) {
      case L::OPEN: showOpenDialog(); break;
      case L::SETTINGS: showPage(P_SETTINGS); break;
      case L::TAB: libSetTab(h.index); break;
      case L::CONTINUE: if (!library_.empty()) openFile(library_[0], true); break;
      case L::GAME:
        if (h.index >= 0 && h.index < static_cast<int>(libPaths_.size())) { const std::string p = libPaths_[h.index]; openFile(p); }
        break;
      default: break;
    }
  }

  void libFocusActivate() {
    if (lib_.focus < 0) libActivate({indigo::ui::Library::CONTINUE, -1});
    else libActivate({indigo::ui::Library::GAME, lib_.focus});
  }

  void libToggleFav() {
    if (lib_.focus < 0 || lib_.focus >= static_cast<int>(libPaths_.size())) return;
    const std::string key = displayKey(libPaths_[lib_.focus]);
    if (favorites_.count(key)) { favorites_.erase(key); status("Tirado dos favoritos"); }
    else { favorites_.insert(key); status("Favorito"); }
    saveFavorites();
    shelfDirty_ = true;
    buildLibrary();
  }

  // analógico esquerdo nos menus e na biblioteca: vale como o direcional (portáteis sem direcional bom e
  // controles em que o direcional é o analógico). Primeiro passo na hora; segurando, repete.
  void stickNavigate() {
    int dx = 0, dy = 0;
    for (auto* p : pads_) {
      if (!p) continue;
      const int x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
      if (std::abs(y) > 22000 && std::abs(y) >= std::abs(x)) dy = y > 0 ? 1 : -1;
      else if (std::abs(x) > 22000) dx = x > 0 ? 1 : -1;
    }
    const int dir = dy ? (dy > 0 ? 2 : 1) : dx ? (dx > 0 ? 4 : 3) : 0;
    const uint64_t now = SDL_GetTicks();
    if (!dir) { stickDir_ = 0; return; }
    if (dir == stickDir_ && now < stickNext_) return;
    stickNext_ = now + (dir == stickDir_ ? 120 : 380);
    stickDir_ = dir;
    SDL_GamepadButtonEvent b{};
    b.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    b.which = pads_.empty() || !pads_[0] ? 0 : SDL_GetGamepadID(pads_[0]);
    b.button = static_cast<Uint8>(dir == 1 ? SDL_GAMEPAD_BUTTON_DPAD_UP : dir == 2 ? SDL_GAMEPAD_BUTTON_DPAD_DOWN
                                  : dir == 3 ? SDL_GAMEPAD_BUTTON_DPAD_LEFT : SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    b.down = true;
    onPadButton(b);
  }

  void onPadButton(const SDL_GamepadButtonEvent& b) {
    touchActive_ = false;  // usando o controle: os botões da tela somem
    touch_.clear();
    if (!mobile_ && SDL_CursorVisible()) SDL_HideCursor();  // portáteis (Legion Go, ROG Ally): sem a seta na tela
    if (page_ == P_CAPTURE && menuVisible() && !captureKeys_) {
      // o direcional navega (Pular/Cancelar); os outros botões são os que estão sendo configurados
      if (b.button == SDL_GAMEPAD_BUTTON_DPAD_UP) moveSel(-1);
      else if (b.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) moveSel(1);
      else if ((b.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) && menu_.selectedId() != M_CAPTURE_INFO)
        activate(menu_.selectedId());
      else if (b.button != SDL_GAMEPAD_BUTTON_DPAD_LEFT && b.button != SDL_GAMEPAD_BUTTON_DPAD_RIGHT) captureAssign(b.button, -1);
      return;
    }
    if (padHotkey(static_cast<SDL_GamepadButton>(b.button), SDL_GetGamepadFromID(b.which))) return;
    SDL_Gamepad* pad = SDL_GetGamepadFromID(b.which);
    const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
    const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
    if (b.button == SDL_GAMEPAD_BUTTON_GUIDE || (start && back)) {
      if (menuOpen_ && core_) closeMenu(); else if (core_) openMenu();
      return;
    }
    if (libraryMode()) {
      if (!usingPad_) { usingPad_ = true; buildLibrary(); }
      switch (b.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: lib_.navigate(0, -1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: lib_.navigate(0, 1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: lib_.navigate(-1, 0); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: lib_.navigate(1, 0); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: libFocusActivate(); break;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: libSetTab(lib_.tab - 1); break;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: libSetTab(lib_.tab + 1); break;
        case SDL_GAMEPAD_BUTTON_WEST: case SDL_GAMEPAD_BUTTON_NORTH: libToggleFav(); break;
        case SDL_GAMEPAD_BUTTON_START: showPage(P_SETTINGS); break;
        default: break;
      }
      return;
    }
    if (menuVisible()) {
      switch (b.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: moveSel(-1); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: moveSel(1); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: activate(menu_.selectedId()); break;
        case SDL_GAMEPAD_BUTTON_EAST: menuBack(); break;
        default: break;
      }
      return;
    }
    if (b.button == SDL_GAMEPAD_BUTTON_LEFT_STICK) togglePause();
  }

  // onde a imagem do jogo fica: no meio, sem deformar, do maior tamanho que cabe (ou na maior escala inteira,
  // com o ajuste "pixels perfeitos"). No celular fica dentro da área segura; com os controles de toque, em pé,
  // sobe para deixar espaço embaixo e, deitado, se afasta dos lados quando dá sem encolher muito.
  // tamanho da imagem mostrada (no DS, a das duas telas no arranjo escolhido)
  int viewW() const { if (core_->system() == "nds") { int w, h; indigo::ui::dsCanvas(dsUsed_, w, h); return w; } return core_->width(); }
  int viewH() const { if (core_->system() == "nds") { int w, h; indigo::ui::dsCanvas(dsUsed_, w, h); return h; } return core_->height(); }

  SDL_FRect gameRect(int ow, int oh, bool touchUi) const {
    const indigo::ui::Metrics m = uiMetrics(ow, oh);
    const SDL_FRect a = mobile_ ? m.area(ow, oh) : SDL_FRect{0, 0, static_cast<float>(ow), static_cast<float>(oh)};
    const bool portrait = oh > ow && touchUi;
    // DS: o arranjo das duas telas depende da área (automático: lado a lado com a tela deitada, empilhadas em pé)
    if (core_->system() == "nds") dsUsed_ = indigo::ui::dsResolve(dsMode_, a.w, portrait ? a.h * 0.6f : a.h);
    const float w = static_cast<float>(viewW()), h = static_cast<float>(viewH());
    float left = a.x, top = a.y, availW = a.w, availH = a.h;
    if (portrait) {
      const float bottom = indigo::ui::TouchControls::bottomHeight(ow, oh, m);
      top = a.y + std::min(a.h * 0.04f, 24 * m.dp);
      availH = a.y + a.h - bottom - top;
    } else if (touchUi) {
      const float side = indigo::ui::TouchControls::sideWidth(ow, oh, m);
      const float full = std::min(availW / w, availH / h), narrow = std::min((availW - 2 * side) / w, availH / h);
      if (narrow >= full * 0.8f) { left += side; availW -= 2 * side; }
    }
    float s = std::min(availW / w, availH / h);
    if (integerScale_ && s >= 1) s = std::floor(s);
    const float gw = std::round(w * s), gh = std::round(h * s);
    // canto em pixel inteiro: a imagem não fica meio pixel deslocada (bordas dos pixels nítidas)
    return {std::round(left + (availW - gw) / 2), std::round(top + (availH - gh) / 2), gw, gh};
  }

  // ---------------- som
  void drainAudio(bool play) {
    const size_t n = core_->audioAvailable();
    if (!n) return;
    left_.resize(n); right_.resize(n); mixed_.resize(n * 2);
    core_->pullAudio(left_.data(), right_.data(), n);
    if (!play || !audio_) return;
    for (size_t i = 0; i < n; i++) { mixed_[i * 2] = left_[i]; mixed_[i * 2 + 1] = right_[i]; }
    SDL_PutAudioStreamData(audio_, mixed_.data(), static_cast<int>(n * 2 * sizeof(float)));
    // mantém a fila perto de ~3 quadros ajustando de leve a velocidade de reprodução (sem estalos)
    const double queued = SDL_GetAudioStreamQueued(audio_) / (2.0 * sizeof(float));
    const double target = SAMPLE_RATE * 3 / core_->fps();
    if (queued > target * 4) { SDL_ClearAudioStream(audio_); return; }
    const double ratio = 1.0 + std::clamp((queued - target) / target, -1.0, 1.0) * 0.005;
    SDL_SetAudioStreamFrequencyRatio(audio_, static_cast<float>(ratio));
  }

  // ---------------- vídeo
  void recreateTexture() { screen_.reset(); }

  void render() {
    SDL_SetRenderDrawColor(ren_, 0x0E, 0x0F, 0x14, 0xFF);
    SDL_RenderClear(ren_);
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(ren_, &ow, &oh);
    applyMetrics(ow, oh);
    menu_.sheet = mobile_ && oh > ow;  // celular em pé: menus sobem de baixo
    if (core_) {
      const bool touchUi = touchUiVisible();
      const SDL_FRect dst = gameRect(ow, oh, touchUiInUse());
      if (core_->system() == "nds") {
        indigo::ui::dsCompose(dsUsed_, core_->framebuffer(), dsBuf_);
        screen_.update(ren_, dsBuf_.data(), viewW(), viewH(), filter_);
      } else {
        screen_.update(ren_, core_->framebuffer(), core_->width(), core_->height(), filter_);
      }
      screen_.draw(ren_, dst);
      if (touchUi) { layoutTouch(ow, oh, touchUi); touch_.draw(ren_); }
      if (menuOpen_) menu_.draw(ren_, ow, oh, true);
    } else {
      // sem jogo: a biblioteca no fundo; Ajustes e as outras páginas aparecem por cima
      lib_.draw(ren_, ow, oh);
      if (page_ != P_MAIN) menu_.draw(ren_, ow, oh, true);
    }
    // aviso por 3 s (no PC também vai para a barra de título)
    if (!status_.empty() && SDL_GetTicks() < statusUntil_) {
      const float px = 14.5f * lib_.m.dp;
      const std::string t = indigo::text::fit(status_, px, ow * 0.86f);
      const float tw = indigo::ui::textWidth(t, px);
      SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
      const SDL_FRect sa = lib_.m.area(ow, oh);
      const SDL_FRect box{(ow - tw) / 2 - px * 1.2f, sa.y + sa.h - px * 4.2f, tw + px * 2.4f, px * 2.4f};
      indigo::ui::fillRoundRect(ren_, box, box.h / 2, {0x22, 0x25, 0x33, 235});
      indigo::ui::drawText(ren_, (ow - tw) / 2, box.y + (box.h - indigo::text::lineHeight(px)) / 2, t, px, indigo::ui::color::TEXT);
    }
    SDL_RenderPresent(ren_);
  }

  SDL_Window* win_ = nullptr;
  SDL_Renderer* ren_ = nullptr;
  SDL_AudioStream* audio_ = nullptr;
  std::string audioError_;
  std::vector<SDL_Gamepad*> pads_;
  std::unique_ptr<Core> core_;
  std::string dataDir_, configDir_, romBase_, romKey_, savePath_;  // configDir_: opções, controles, pastas e PIN (Linux: XDG)
  std::string status_, lastTitle_;
  uint64_t statusUntil_ = 0;
  uint32_t keyboard_ = 0;
  bool turboKey_ = false, padTurbo_ = false;
  bool running_ = true, paused_ = false, fullscreen_ = false;
  // portáteis e frontends: frontend_ = o jogo veio de um frontend (fechar o jogo fecha o app);
  // startFullscreen_ = abrir em tela cheia (padrão nos PCs com tela de toque, como Legion Go e ROG Ally)
  bool frontend_ = false, startFullscreen_ = false, forceFullscreen_ = false;
  int stickDir_ = 0;
  uint64_t stickNext_ = 0;
  bool mobile_ = false, simulated_ = false, menuOpen_ = false, touchActive_ = false, fastOn_ = false;
  bool exportWithRoms_ = true;
  // tela e controles
  indigo::video::Screen screen_;
  indigo::ui::DsLayout dsMode_ = indigo::ui::DsLayout::Auto;      // escolha do menu ("telasds=N" nas opções)
  mutable indigo::ui::DsLayout dsUsed_ = indigo::ui::DsLayout::Stacked;  // arranjo em uso (calculado em gameRect)
  std::vector<uint32_t> dsBuf_;
  int filter_ = indigo::video::NITIDO;
  int ps1Res_ = 0;
  bool sgb_ = true;  // Super Game Boy nos jogos que têm a marca (padrão: ligado)  // resolução do PlayStation escolhida (0 = a padrão do aparelho)
  std::array<std::vector<SDL_GamepadButton>, NUM_ACTIONS> padMap_, captureNewPad_;
  std::array<std::vector<SDL_Scancode>, NUM_ACTIONS> keyMap_, captureNewKeys_;
  indigo::ui::Player2 player2_ = indigo::ui::Player2::SecondPad;  // quem é o jogador 2 (menu Controles)
  bool p2On_ = false;  // o controle 2 já foi ligado no jogo aberto
  bool padTriggerTurbo_ = true, hotkeyLatched_ = false, captureKeys_ = false, captureTrigger_ = false;
  int captureStep_ = 0;
  // estante
  std::set<std::string> favorites_;
  std::vector<std::string> folders_, shelfAll_, shelfView_;
  std::string shelfConsole_;
  int shelfPageNo_ = 0;
  int aboutPage_ = 0;
  bool shelfDirty_ = true;
  std::map<std::string, SDL_Texture*> covers_;
  // biblioteca e gestos
  indigo::ui::Library lib_;
  std::vector<std::string> libPaths_;
  std::string libTabName_ = "Todos";
  bool usingPad_ = false, gActive_ = false, gDrag_ = false, gTabs_ = false, mouseDown_ = false;
  float gLastX_ = 0;
  // janela do PC (gravada nas opções) e ajustes de tela
  int winX_ = 0, winY_ = 0, winW_ = 0, winH_ = 0, uiSize_ = 0;
  bool winMax_ = false, integerScale_ = false;
  SDL_FingerID gFinger_ = 0;
  float gx0_ = 0, gy0_ = 0, gLast_ = 0, mx0_ = 0, my0_ = 0;
  uint64_t lastTap_ = 0;
  // nuvem
  enum CloudState { CS_OFF, CS_UNAVAILABLE, CS_IDLE, CS_SYNCING, CS_LOCKED, CS_ERROR };
  std::unique_ptr<indigo::cloud::Client> cloud_;
  CloudState cloudState_ = CS_OFF;
  bool cloudOn_ = true, cloudInteractive_ = false;
  std::string cloudPin_, cloudError_, pinDraft_, openSaveKey_, openConsole_;
  double cloudLast_ = 0;
  uint64_t uploadAt_ = 0;
  int cloudUp_ = 0, cloudDown_ = 0, cloudPending_ = 0;
  std::map<std::string, Ident> idents_;
  std::string lastSyncReason_;
  int slot_ = 1, palette_ = 0;
  Page page_ = P_MAIN;
  uint64_t frameCount_ = 0;
  std::vector<float> left_, right_, mixed_;
  indigo::ui::TouchControls touch_;
  indigo::ui::Menu menu_;
  std::vector<std::string> library_;
  std::map<std::string, std::string> zipNames_;
  std::map<int, SDL_Texture*> thumbs_;
  std::vector<std::pair<std::string, long long>> versions_;
};

}  // namespace

// Linha de comando (frontends como Playnite, LaunchBox, ES-DE e Steam; no Android a IndigoActivity monta os
// mesmos argumentos a partir do pedido do frontend):
//   indigo [--tela-cheia | --fullscreen | -f] [--frontend] [jogo]
// --frontend: tela cheia, e fechar o jogo fecha o Índigo (volta para o frontend). Com um jogo e sem opções
// ("Abrir com" do Windows, arrastar sobre o .exe) o app abre normalmente com o jogo.
int main(int argc, char** argv) {
  App app;
  std::string rom;
  bool full = false, frontend = false;
  for (int k = 1; k < argc; k++) {
    const std::string a = argv[k] ? argv[k] : "";
    if (a == "--tela-cheia" || a == "--fullscreen" || a == "-f") full = true;
    else if (a == "--frontend") frontend = full = true;
    else if (!a.empty() && a[0] != '-' && rom.empty()) rom = a;
  }
  app.setLaunchOptions(full, frontend && !rom.empty());
  if (!app.init()) return 1;
  if (!rom.empty()) app.openFile(rom);
  app.run();
  return 0;
}
