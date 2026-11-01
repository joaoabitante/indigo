// Segunda referência para os testes do PC Engine: o núcleo do Mesen 2 (o emulador de PC Engine mais preciso
// hoje; os SingleStepTests do HuC6280 saíram dele). Compilado só pelo t7_pce.sh, fora do repositório
// (extensão .cc para o CMake do Índigo não pegar). Imprime "quadro largura altura hash" como o t7_pce_ref.c,
// com o hash (FNV-1a) das cores de 9 bits das linhas 1 a 240 das 242 visíveis, sem as bordas laterais.
// Uso: t7_pce_mesen rom quadros [teclas [arquivo.raw quadro]]   (teclas: "quadro:máscara,..." com os bits de
// Key do Índigo; o .raw é um quadro em RGB565 para depuração)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <thread>
#include <vector>

#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/Interfaces/IKeyManager.h"
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Core/Shared/KeyManager.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Shared/SettingTypes.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"

namespace {
constexpr int MAXW = 682;  // PceConstants::MaxScreenWidth
std::atomic<int> g_frames{0};
int g_want = 0, g_dump = -1;
const char* g_dumpPath = nullptr;
std::vector<std::string> g_lines;

const char* g_keys = "";

// teclas no formato do Índigo ("quadro:máscara,..."; vale do quadro dado em diante)
unsigned keysAt(int frame) {
  unsigned k = 0;
  const char* p = g_keys;
  while (p && *p) {
    const int fr = std::atoi(p);
    const char* c = std::strchr(p, ':');
    if (!c) break;
    if (fr <= frame) k = static_cast<unsigned>(std::strtoul(c + 1, nullptr, 0));
    p = std::strchr(c, ',');
    if (p) p++;
  }
  return k;
}

// códigos 1-8 = A, B, Select, Start, cima, baixo, esquerda, direita
struct Keys : IKeyManager {
  static constexpr unsigned BIT[9] = {0, 1, 2, 4, 8, 64, 128, 32, 16};
  void RefreshState() override {}
  void UpdateDevices() override {}
  bool IsMouseButtonPressed(MouseButton) override { return false; }
  bool IsKeyPressed(uint16_t code) override { return code >= 1 && code <= 8 && (keysAt(g_frames.load()) & BIT[code]); }
  std::vector<uint16_t> GetPressedKeys() override { return {}; }
  std::string GetKeyName(uint16_t) override { return ""; }
  uint16_t GetKeyCode(std::string) override { return 0; }
  bool SetKeyState(uint16_t, bool) override { return false; }
  void ResetKeyState() override {}
  void SetDisabled(bool) override {}
};

struct Listener : INotificationListener {
  void ProcessNotification(ConsoleNotificationType type, void* param) override {
    if (type != ConsoleNotificationType::PpuFrameDone || !param) return;
    const int f = g_frames.load();
    if (f >= g_want) return;
    const uint16_t* buf = static_cast<const uint16_t*>(param);
    const uint16_t* div = buf + MAXW * 242;
    // modo da maioria das linhas
    int count[5] = {};
    for (int r = 1; r <= 240; r++) { int d = div[r]; count[(d >= 2 && d <= 4) ? d : 4]++; }
    int m = 4;
    if (count[3] > count[m]) m = 3;
    if (count[2] > count[m]) m = 2;
    const int start = m == 4 ? 48 : m == 3 ? 72 : 120, w = m == 4 ? 256 : m == 3 ? 341 : 512;
    uint32_t hash = 2166136261u;
    std::vector<uint16_t> dump;
    for (int r = 1; r <= 240; r++) {
      int d = div[r];
      if (d < 2 || d > 4) d = 4;
      for (int x = 0; x < w; x++) {
        int sx = (start + x) * m / d;
        if (sx >= MAXW) sx = MAXW - 1;
        const unsigned c = buf[r * MAXW + sx] & 0x1FF;
        hash ^= c;
        hash *= 16777619u;
        if (f == g_dump) {
          const unsigned g = (c >> 6) & 7, rr = (c >> 3) & 7, b = c & 7;
          dump.push_back(static_cast<uint16_t>(((rr * 31 / 7) << 11) | ((g * 63 / 7) << 5) | (b * 31 / 7)));
        }
      }
    }
    if (f == g_dump && g_dumpPath) {
      if (FILE* o = std::fopen(g_dumpPath, "wb")) {
        uint32_t hw[2] = {uint32_t(w), 240};
        std::fwrite(hw, 4, 2, o);
        std::fwrite(dump.data(), 2, dump.size(), o);
        std::fclose(o);
      }
    }
    char line[64];
    std::snprintf(line, sizeof line, "%d %d %d %08x", f, w, 240, hash);
    g_lines.push_back(line);
    g_frames++;
  }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "uso: %s rom quadros [arquivo.raw quadro]\n", argv[0]); return 1; }
  g_want = std::atoi(argv[2]);
  if (argc > 3) g_keys = argv[3];
  if (argc > 5) { g_dumpPath = argv[4]; g_dump = std::atoi(argv[5]); }
  FolderUtilities::SetHomeFolder("/tmp/mesen-home");
  static Keys keys;
  KeyManager::RegisterKeyManager(&keys);
  std::unique_ptr<Emulator> emu(new Emulator());
  emu->Initialize(false);
  KeyManager::SetSettings(emu->GetSettings());
  PcEngineConfig& cfg = emu->GetSettings()->GetPcEngineConfig();
  cfg.DisableFrameSkipping = true;
  cfg.RamPowerOnState = RamState::AllZeros;
  cfg.EnableRandomPowerOnState = false;
  cfg.Port1.Type = ControllerType::PceController;
  KeyMapping& km = cfg.Port1.Keys.Mapping1;
  km.A = 1; km.B = 2; km.Select = 3; km.Start = 4; km.Up = 5; km.Down = 6; km.Left = 7; km.Right = 8;
  emu->GetSettings()->SetPcEngineConfig(cfg);
  auto listener = std::make_shared<Listener>();
  emu->GetNotificationManager()->RegisterNotificationListener(listener);
  emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
  if (!emu->LoadRom(VirtualFile(argv[1]), VirtualFile())) { std::fprintf(stderr, "não carregou\n"); return 1; }
  for (int i = 0; i < 6000 && g_frames.load() < g_want; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  emu->Stop(false);
  emu->Release();
  for (auto& l : g_lines) std::printf("%s\n", l.c_str());
  return 0;
}
