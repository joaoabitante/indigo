// Watara Supervision (1992): 65C02 a 4 MHz, 8 KB de RAM, LCD de 160x160 em 4 tons (8 KB de VRAM, 48 bytes
// por linha), dois canais de onda quadrada, ruído e DMA de áudio (amostras de 4 bits), cartucho de até 512 KB
// em bancos de 16 KB (o último fica fixo em $C000).
//
// Mapa: $0000-$1FFF RAM, $2000-$3FFF registradores, $4000-$5FFF VRAM, $8000-$BFFF banco, $C000-$FFFF fixo.
// Registradores conforme o cc65 (asminc/supervision.inc, zlib) e o driver svision do MAME (GPL-2.0+, Peter
// Trauner), lido como documentação. A CPU é a mesma do Lynx (src/lynx/cpu.h, com as instruções de bit).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../lynx/cpu.h"

namespace indigo::sv {

constexpr int SCREEN = 160;
constexpr int CPU_HZ = 4000000;
constexpr int CYCLES_PER_FRAME = 65536;  // ~61 quadros por segundo, como no aparelho

bool isSvRom(const std::vector<uint8_t>& rom, const std::string& lowerName);

class Supervision {
public:
  Supervision(const std::vector<uint8_t>& rom);

  void reset();
  void runFrame();
  void setKeys(uint32_t mask);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  void setSampleRate(int rate);
  size_t audioAvailable() const { return (wpos + BUF - rpos) % BUF; }
  void pullAudio(float* l, float* r, size_t n);
  void setPalette(const uint32_t* p4);

  // barramento da CPU: cada acesso é um ciclo
  uint8_t read(uint16_t a);
  void write(uint16_t a, uint8_t v);
  uint8_t peek(uint16_t a) const;  // leitura sem efeitos (DMA e testes)

  lynx::Cpu65SC02<Supervision> cpu;
  uint32_t fb[SCREEN * SCREEN];
  uint64_t now = 0;
  uint8_t ram[0x2000], vram[0x2000], reg[0x2000];
  int frame = 0;

private:
  struct Tone { int period = 0, phase = 0, count = 0; };
  struct Noise { int counter = 0, count = 0; uint16_t lfsr = 1; bool out = false; };
  struct Dma { bool on = false; int pos = 0, size = 0, counter = 0; };

  std::vector<uint8_t> rom_;
  int banks_ = 1;
  uint32_t keys_ = 0;
  uint32_t pal_[4];
  uint64_t frameEnd_ = 0, timerAt_ = 0;
  bool timerShot_ = false, dmaDone_ = false, vdmaBusy_ = false;
  Tone tone_[2];
  Noise noise_;
  Dma dma_;

  // som: um passo a cada 16 ciclos (250 kHz), média até a taxa de saída
  static constexpr size_t BUF = 16384;
  std::vector<float> bufL = std::vector<float>(BUF), bufR = std::vector<float>(BUF);
  size_t wpos = 0, rpos = 0;
  double cyclesPerSample_ = CPU_HZ / 48000.0, sampleAcc_ = 0;
  int pendingCycles_ = 0;
  double sumL_ = 0, sumR_ = 0, hpL_ = 0, hpR_ = 0, lastL_ = 0, lastR_ = 0;
  int sumN_ = 0;

  uint8_t romAt(uint32_t bank, uint16_t a) const { return rom_[((bank % banks_) << 14) | (a & 0x3FFF)]; }
  bool irqLine() const;
  void tick() { now++; pendingCycles_++; }
  void soundWrite(int r, uint8_t v);
  void audioRun();
  void audioStep();
  void endFrame();
  void render();
  void sync(StateIO& io);
};

class SupervisionCore : public Core {
public:
  SupervisionCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  std::string system() const override { return "svision"; }
  std::string title() const override { return ""; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->fb; }
  int width() const override { return SCREEN; }
  int height() const override { return SCREEN; }
  double fps() const override { return double(CPU_HZ) / CYCLES_PER_FRAME; }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return false; }
  bool saveDirty() const override { return false; }
  std::vector<uint8_t> exportSave() override { return {}; }
  void importSave(const std::vector<uint8_t>&) override {}
  // a paleta do Game Boy escolhida no menu também vale para os 4 tons do LCD
  void setDmgPalette(const uint32_t* bg, const uint32_t*, const uint32_t*) override { m_->setPalette(bg); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }
  Supervision& machine() { return *m_; }

private:
  std::unique_ptr<Supervision> m_;
};

}  // namespace indigo::sv
