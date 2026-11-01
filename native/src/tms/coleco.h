// Núcleo ColecoVision (NTSC): Z80 a 3,58 MHz, vídeo TMS9918A (interrupção de quadro ligada ao NMI da CPU),
// som SN76489, BIOS de 8 KB em 0000–1FFF, 1 KB de RAM em 6000–7FFF (espelhado 8 vezes) e o cartucho em
// 8000–FFFF. Também tem:
//  - Super Game Module (sempre ligado, como nos emuladores de referência): +24 KB de RAM em 2000–7FFF
//    (porta 53h, bit 0), 8 KB de RAM no lugar da BIOS (porta 7Fh, bit 1 = 0) e o AY-3-8910 (portas 50h
//    endereço, 51h escrita, 52h leitura);
//  - cartuchos MegaCart (mais de 32 KB): o último banco de 16 KB fica fixo em 8000–BFFF e ler (ou gravar)
//    FFC0–FFFF escolhe o banco de C000–FFFF.
//
// BIOS obrigatória: indigo::firmware("coleco") (coleco.rom, 8 KB, protegida por direitos autorais; não há
// substituta livre compatível com os jogos comerciais). Sem ela o construtor lança runtime_error.
//
// Teclas (controle 1; o controle 2 fica solto):
//   direcional = joystick   A = botão esquerdo (tiro principal)   B = botão direito
//   START = tecla 1   SELECT = tecla 2   X = tecla 3   Y = tecla 4   L = tecla *   R = tecla #
//   (as teclas 5–9 e 0 do teclado numérico não têm botão no controle)
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "machine.h"

namespace indigo::tms {

// .col (ou .cv); a extensão .bin fica de fora (é usada por vários consoles)
bool isColecoRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

class Coleco : public Machine {
public:
  Coleco(const std::vector<uint8_t>& bios, const std::vector<uint8_t>& rom);

  void reset();
  void setKeys(uint32_t mask) { keys = mask; }
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  bool megaCart() const { return mega; }

  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int port) override;
  void out(int port, int v) override;

protected:
  void updateIrq() override;

private:
  void sync(StateIO& io);
  int pad(int which) const;

  std::vector<uint8_t> bios, rom, ram, sgmRam;
  bool mega = false;
  int banks = 2, bank = 0;
  bool joyMode = true;   // portas 80h/C0h: teclado ou joystick
  bool sgmUpper = false, sgmLower = false;
  bool nmiLine = false;
  uint32_t keys = 0;
};

class ColecoCore : public Core {
public:
  ColecoCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  Coleco& machine() { return *m_; }

  std::string system() const override { return "coleco"; }
  std::string title() const override { return ""; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
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
  std::unique_ptr<Coleco> m_;
};

}  // namespace indigo::tms
