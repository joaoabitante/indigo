// Régua do Famicom Disk System: o núcleo do Mesen 2 (GPL-3.0) rodando a BIOS e o disco de teste do t7_fds.sh.
// Este fonte fica no repositório (só inclui headers do Mesen 2); o binário é compilado pelo t7_fds.sh fora do
// repositório (extensão .cc para o CMake do Índigo não pegar), com os .o do
// Mesen 2 que o t7_pce.sh já compila. Faz a mesma sequência do indigo-t7_fds: espera o jogo gravar o SAVE
// (estado 1), 120 quadros depois tira o disco e, 37 quadros depois (~1,1 milhão de ciclos), põe o lado B.
// Imprime por quadro: "quadro estado irqs quadrosDoJogo erroDaBios".
// Uso: t7_fds_mesen pasta_home disco quadros
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Core/NES/Mappers/FDS/Fds.h"
#include "Core/NES/NesConsole.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/Interfaces/INotificationListener.h"
#include "Core/Shared/NotificationManager.h"
#include "Core/Shared/SettingTypes.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/VirtualFile.h"

namespace {
std::atomic<int> g_frames{0};
int g_want = 0, g_estado1 = -1;
Emulator* g_emu = nullptr;
std::vector<std::string> g_lines;

struct Listener : INotificationListener {
  void ProcessNotification(ConsoleNotificationType type, void*) override {
    if (type != ConsoleNotificationType::PpuFrameDone) return;
    const int f = g_frames.load();
    if (f >= g_want) return;
    const uint8_t* ram = static_cast<const uint8_t*>(g_emu->GetMemory(MemoryType::NesInternalRam).Memory);
    char line[80];
    std::snprintf(line, sizeof line, "%d %d %d %d %d", f + 1, ram[0x30], ram[0x10] | (ram[0x11] << 8), ram[0x12], ram[0xFF]);
    g_lines.push_back(line);
    if (ram[0x30] == 1 && g_estado1 < 0) g_estado1 = f;
    auto* nes = dynamic_cast<NesConsole*>(g_emu->GetConsoleUnsafe());
    auto* fds = nes ? dynamic_cast<Fds*>(nes->GetMapper()) : nullptr;
    if (fds && g_estado1 >= 0 && f == g_estado1 + 120) fds->EjectDisk();
    if (fds && g_estado1 >= 0 && f == g_estado1 + 120 + 37) fds->InsertDisk(1);
    g_frames++;
  }
};
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "uso: %s pasta_home disco quadros\n", argv[0]); return 1; }
  g_want = std::atoi(argv[3]);
  FolderUtilities::SetHomeFolder(argv[1]);  // a BIOS de teste fica em <home>/Firmware/disksys.rom
  std::unique_ptr<Emulator> emu(new Emulator());
  g_emu = emu.get();
  emu->Initialize(false);
  NesConfig& cfg = emu->GetSettings()->GetNesConfig();
  cfg.RamPowerOnState = RamState::AllZeros;
  cfg.FdsAutoInsertDisk = false;
  cfg.FdsAutoLoadDisk = true;
  cfg.FdsFastForwardOnLoad = false;
  emu->GetSettings()->SetNesConfig(cfg);
  auto listener = std::make_shared<Listener>();
  emu->GetNotificationManager()->RegisterNotificationListener(listener);
  emu->GetSettings()->SetFlag(EmulationFlags::MaximumSpeed);
  if (!emu->LoadRom(VirtualFile(argv[2]), VirtualFile())) { std::fprintf(stderr, "não carregou\n"); return 1; }
  for (int i = 0; i < 12000 && g_frames.load() < g_want; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  emu->Stop(false);
  emu->Release();
  for (auto& l : g_lines) std::printf("%s\n", l.c_str());
  return 0;
}
