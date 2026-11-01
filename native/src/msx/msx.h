// Núcleo MSX1 / MSX2 (cartuchos). Z80 a 3,58 MHz com a espera de 1 ciclo em cada M1 (busca de opcode)
// que todo MSX tem; 262 linhas × 228 ciclos (NTSC, 59,92 quadros/s).
//   Slots: 0 = BIOS (0000h–7FFFh) + logotipo da C-BIOS (8000h–BFFFh); 1 = cartucho; 2 = vazio;
//   3 = RAM de 64 KB (MSX1) ou slot expandido (MSX2: 3-0 sub-ROM, 3-2 RAM de 256 KB com mapeador de
//   memória nas portas FCh–FFh). PPI 8255 nas portas A8h–ABh (slots, teclado, clique), PSG em A0h–A2h,
//   VDP em 98h–9Bh, relógio RP5C01 (só os registradores, sem contar o tempo) em B4h/B5h no MSX2.
//   BIOS: a C-BIOS livre embutida (cbios.h); se o usuário der a BIOS original, ela é usada:
//   firmware("msx") = 32 KB (MSX1); firmware("msx2") = 48 KB (principal + sub-ROM) ou 32 KB com
//   firmware("msx2sub") = 16 KB.
//   Máquina: ".mx1" = MSX1, ".mx2" = MSX2; ".rom" e outros: MSX2 se o código usar recursos do MSX2
//   (portas 9Ah/9Bh do V9938, chamada EXTROM da sub-ROM ou leitura da versão em 002Dh), senão MSX1.
//
// Teclas (joystick na porta 1 e teclado ao mesmo tempo, para servir jogos dos dois tipos):
//   direcional = alavanca do joystick e setas do teclado
//   A = gatilho A do joystick + barra de espaço    B = gatilho B do joystick + tecla M
//   START = barra de espaço    SELECT = F5    X = Enter (RETURN)    Y = Esc
//   L = F1 (pausa na maioria dos jogos Konami)    R = F2
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../sms/z80.h"
#include "audio.h"
#include "cart.h"
#include "vdp.h"

namespace indigo::msx {

// ROM de cartucho de MSX: extensão .mx1/.mx2, ou cabeçalho "AB" no começo da ROM ou de um banco de 16 KB
bool isMsxRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// 1 = MSX1, 2 = MSX2 (pela extensão; senão pelo código da ROM)
int msxMachineFor(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

class MSX : public Z80Bus {
public:
  MSX(const std::vector<uint8_t>& rom, bool msx2);
  const bool msx2;
  Z80 cpu;
  VDP vdp;
  AY8910 psg;
  Mixer mixer;
  Cart cart;
  bool userBios = false;
  int frame = 0;

  void reset();
  void runFrame();
  void setKeys(uint32_t mask);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  int read(int a) override;
  void write(int a, int v) override;
  int in(int port) override;
  void out(int port, int v) override;

private:
  void remap();
  void runUntil(int cycle);
  void audioTo(int64_t cycle);
  int joystick() const;
  void sync(StateIO& io);

  std::vector<uint8_t> mainRom, logoRom, subRom, ram;
  int segments = 4;  // segmentos de 16 KB da RAM (MSX2)
  const uint8_t* rd[8] = {};
  uint8_t* wr[8] = {};
  int slotOf[8] = {};
  int psl = 0, ssl3 = 0, ppiC = 0;
  int mapReg[4] = {3, 2, 1, 0};
  uint8_t matrix[11] = {};
  uint8_t rtcReg[4][13] = {};
  int rtcAddr = 0, rtcMode = 0;
  uint32_t keys = 0;
  int64_t cycles = 0, audioCycle = 0;
  int lineCycle = 0, over = 0;
};

class MSXCore : public Core {
public:
  // machine: 0 = automático (pelo código), 1 = MSX1, 2 = MSX2
  MSXCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, int machine = 0);
  MSX& msx() { return *m_; }

  std::string system() const override { return "msx"; }
  std::string title() const override { return ""; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->vdp.fb; }
  int width() const override { return m_->vdp.outW; }
  int height() const override { return m_->vdp.outH; }
  double fps() const override { return 3579545.0 / (262 * 228); }
  void setSampleRate(int rate) override { m_->mixer.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->mixer.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->mixer.pull(l, r, n); }
  bool hasSave() const override { return m_->cart.sramUsed; }
  bool saveDirty() const override { return m_->cart.sramDirty; }
  std::vector<uint8_t> exportSave() override { m_->cart.sramDirty = false; return m_->cart.sram; }
  void importSave(const std::vector<uint8_t>& b) override;
  std::string note() const override;
  std::string description() const;  // máquina, mapeador e BIOS (para testes)
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<MSX> m_;
};

}  // namespace indigo::msx
