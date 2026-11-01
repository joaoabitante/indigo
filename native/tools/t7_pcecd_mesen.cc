// Régua do teste do PC Engine CD: o núcleo do Mesen 2 rodando um .cue com um System Card dado (o System
// Card de teste do Índigo, nunca o de verdade). Compilado só pelo t7_pcecd.sh, fora do repositório (extensão
// .cc para o CMake do Índigo não pegar). Imprime "quadro largura altura hash" como o t7_pce_mesen.cc e, se
// pedido, grava o som (estéreo, int16, 48 kHz) num arquivo .raw.
// Uso: t7_pcecd_mesen syscard.pce disco.cue quadros tipo [teclas [som.raw]]
//      tipo: 0 = CD-ROM², 1 = Super CD-ROM², 2 = Arcade CD-ROM²
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/Audio/SoundMixer.h"
#include "Core/Shared/Interfaces/IAudioDevice.h"
#include "Core/Shared/Interfaces/IKeyManager.h"
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Core/Shared/KeyManager.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Shared/SettingTypes.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"

namespace {
constexpr int MAXW = 682;
std::atomic<int> g_frames{0};
int g_want = 0;
std::vector<std::string> g_lines;
const char* g_keys = "";
FILE* g_audio = nullptr;

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

struct Audio : IAudioDevice {
  void PlayBuffer(int16_t* buf, uint32_t n, uint32_t, bool stereo) override {
    if (g_audio && g_frames.load() < g_want && stereo) std::fwrite(buf, 4, n, g_audio);
  }
  void Stop() override {}
  void Pause() override {}
  void ProcessEndOfFrame() override {}
  string GetAvailableDevices() override { return ""; }
  void SetAudioDevice(string) override {}
  AudioStatistics GetStatistics() override { return {}; }
};

struct Listener : INotificationListener {
  void ProcessNotification(ConsoleNotificationType type, void* param) override {
    if (type != ConsoleNotificationType::PpuFrameDone || !param) return;
    const int f = g_frames.load();
    if (f >= g_want) return;
    const uint16_t* buf = static_cast<const uint16_t*>(param);
    const uint16_t* div = buf + MAXW * 242;
    int count[5] = {};
    for (int r = 1; r <= 240; r++) { int d = div[r]; count[(d >= 2 && d <= 4) ? d : 4]++; }
    int m = 4;
    if (count[3] > count[m]) m = 3;
    if (count[2] > count[m]) m = 2;
    const int start = m == 4 ? 48 : m == 3 ? 72 : 120, w = m == 4 ? 256 : m == 3 ? 341 : 512;
    uint32_t hash = 2166136261u;
    for (int r = 1; r <= 240; r++) {
      int d = div[r];
      if (d < 2 || d > 4) d = 4;
      for (int x = 0; x < w; x++) {
        int sx = (start + x) * m / d;
        if (sx >= MAXW) sx = MAXW - 1;
        hash ^= buf[r * MAXW + sx] & 0x1FF;
        hash *= 16777619u;
      }
    }
    // PPM do último quadro, para depuração (variável T7_PPM)
    if (f == g_want - 1 && std::getenv("T7_PPM")) {
      if (FILE* o = std::fopen(std::getenv("T7_PPM"), "wb")) {
        std::fprintf(o, "P6 %d 240 255\n", w);
        for (int r = 1; r <= 240; r++)
          for (int x = 0; x < w; x++) {
            int d = div[r];
            if (d < 2 || d > 4) d = 4;
            int sx = (start + x) * m / d;
            if (sx >= MAXW) sx = MAXW - 1;
            const unsigned c = buf[r * MAXW + sx] & 0x1FF;
            const unsigned char px[3] = {static_cast<unsigned char>(((c >> 3) & 7) * 255 / 7),
                                         static_cast<unsigned char>(((c >> 6) & 7) * 255 / 7),
                                         static_cast<unsigned char>((c & 7) * 255 / 7)};
            std::fwrite(px, 1, 3, o);
          }
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
  if (argc < 5) { std::fprintf(stderr, "uso: %s syscard.pce disco.cue quadros tipo [teclas [som.raw]]\n", argv[0]); return 1; }
  g_want = std::atoi(argv[3]);
  const int type = std::atoi(argv[4]);
  if (argc > 5) g_keys = argv[5];
  if (argc > 6) g_audio = std::fopen(argv[6], "wb");
  // pasta própria por processo, com o System Card de teste no lugar do syscard3.pce
  const std::string home = "/tmp/mesen-pcecd-" + std::to_string(getpid());
  FolderUtilities::SetHomeFolder(home);
  const std::string fw = FolderUtilities::GetFirmwareFolder();
  {
    FILE* in = std::fopen(argv[1], "rb");
    FILE* out = std::fopen((fw + "/syscard3.pce").c_str(), "wb");
    if (!in || !out) { std::fprintf(stderr, "não copiou o System Card\n"); return 1; }
    char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, in)) > 0) std::fwrite(b, 1, n, out);
    std::fclose(in);
    std::fclose(out);
  }
  static Keys keys;
  KeyManager::RegisterKeyManager(&keys);
  std::unique_ptr<Emulator> emu(new Emulator());
  emu->Initialize(false);
  KeyManager::SetSettings(emu->GetSettings());
  PcEngineConfig& cfg = emu->GetSettings()->GetPcEngineConfig();
  cfg.DisableFrameSkipping = true;
  cfg.RamPowerOnState = RamState::AllZeros;
  cfg.EnableRandomPowerOnState = false;
  cfg.CdRomType = type == 0 ? PceCdRomType::CdRom : type == 1 ? PceCdRomType::SuperCdRom : PceCdRomType::Arcade;
  cfg.Port1.Type = ControllerType::PceController;
  KeyMapping& km = cfg.Port1.Keys.Mapping1;
  km.A = 1; km.B = 2; km.Select = 3; km.Start = 4; km.Up = 5; km.Down = 6; km.Left = 7; km.Right = 8;
  emu->GetSettings()->SetPcEngineConfig(cfg);
  AudioConfig ac = emu->GetSettings()->GetAudioConfig();
  ac.EnableAudio = true;
  ac.SampleRate = 48000;
  ac.MasterVolume = 100;
  ac.EnableEqualizer = false;
  ac.ReverbEnabled = false;
  ac.CrossFeedEnabled = false;
  emu->GetSettings()->SetAudioConfig(ac);
  static Audio audio;
  if (g_audio) emu->GetSoundMixer()->RegisterAudioDevice(&audio);
  auto listener = std::make_shared<Listener>();
  emu->GetNotificationManager()->RegisterNotificationListener(listener);
  // velocidade sem limite (o gerenciador de rewind desliga a flag MaximumSpeed ao carregar)
  EmulationConfig ec = emu->GetSettings()->GetEmulationConfig();
  ec.EmulationSpeed = 0;
  emu->GetSettings()->SetEmulationConfig(ec);
  emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
  if (!emu->LoadRom(VirtualFile(argv[2]), VirtualFile())) { std::fprintf(stderr, "não carregou\n"); return 1; }
  emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
  for (int i = 0; i < 60000 && g_frames.load() < g_want; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  emu->Stop(false);
  emu->Release();
  if (g_audio) std::fclose(g_audio);
  for (auto& l : g_lines) std::printf("%s\n", l.c_str());
  std::string rm = "rm -rf " + home;
  if (std::system(rm.c_str())) {}
  return 0;
}
