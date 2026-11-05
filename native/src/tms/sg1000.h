// Núcleo Sega SG-1000 / SC-3000 (NTSC): Z80 a 3,58 MHz, vídeo TMS9918A (interrupção de quadro na INT da
// CPU, modo 1), SN76489 nas portas 40h–7Fh, controles nas portas DCh/DDh. Não precisa de BIOS.
// Memória: cartucho em 0000–BFFF (até 48 KB direto; maiores usam o mapeador Sega em FFFD–FFFF — essas
// gravações não chegam à RAM, senão a pilha em C3FEh dos programas do CVBasic seria apagada),
// 1 KB de RAM em C000–FFFF (espelhado); cartuchos de até 32 KB ganham 8 KB de RAM em 8000–BFFF (como os
// cartuchos com RAM extra: The Castle, Othello e outros).
// O teclado do SC-3000 não é emulado (só o controle; programas BASIC precisam do teclado).
//
// Teclas (iguais às do Master System no Índigo):
//   direcional = joystick   B = botão 1   A = botão 2   START = botão de pausa do console (NMI)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "machine.h"

namespace indigo::tms {

// .sg (SG-1000) ou .sc (SC-3000)
bool isSg1000Rom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

class SG1000 : public Machine {
public:
  explicit SG1000(const std::vector<uint8_t>& rom);

  void reset();
  void setKeys(uint32_t mask);
  void setKeys2(uint32_t mask) { keys2 = mask; }
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int port) override;
  void out(int port, int v) override;

protected:
  void updateIrq() override { cpu.irqLine = vdp.irq(); }

private:
  void sync(StateIO& io);

  std::vector<uint8_t> rom, ram, cartRam;
  bool mapper = false, extraRam = false;
  int banks = 1;
  int slot[3] = {0, 1, 2};
  bool pausePrev = false;
  uint32_t keys = 0, keys2 = 0;
};

class SG1000Core : public Core {
public:
  SG1000Core(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  SG1000& machine() { return *m_; }

  std::string system() const override { return "sg1000"; }
  std::string title() const override { return ""; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  void setKeys2(uint32_t mask) override { m_->setKeys2(mask); }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return m_->vdp.fb; }
  int width() const override { return TMS9918::W; }
  int height() const override { return TMS9918::H; }
  double fps() const override { return Machine::FPS; }
  void setSampleRate(int rate) override { m_->snd.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->snd.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->snd.pull(l, r, n); }
  bool hasSave() const override { return false; }
  bool saveDirty() const override { return false; }
  std::vector<uint8_t> exportSave() override { return {}; }
  void importSave(const std::vector<uint8_t>&) override {}
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<SG1000> m_;
};

}  // namespace indigo::tms
