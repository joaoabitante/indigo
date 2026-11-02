// Núcleo Virtual Boy (Nintendo, 1995): CPU NEC V810 a 20 MHz (v810.h), VIP com os dois olhos (vip.h), som VSU
// (vsu.h), temporizador, controle e SRAM do cartucho. Não precisa de BIOS: o console começa direto na ROM
// (vetor de reset em 0xFFFFFFF0).
//
// Tela: 384×224. Modos (setStereoMode): 0 = só o olho esquerdo em vermelho e preto (padrão, como o aparelho),
// 1 = anáglifo (esquerdo em vermelho, direito em ciano), 2 = os dois olhos lado a lado (768×224).
//
// Teclas (o controle do Virtual Boy tem dois direcionais):
//   direcional do Índigo = direcional ESQUERDO; A = A; B = B; START = Start; SELECT = Select; L / R = gatilhos;
//   X = direcional direito para CIMA; Y = direcional direito para BAIXO;
//   SELECT + X = direcional direito para a ESQUERDA; SELECT + Y = direcional direito para a DIREITA
//   (enquanto SELECT é usado como modificador junto com X ou Y, o Select não chega ao jogo).
//   setRightPad() permite ligar um segundo direcional virtual de verdade (bits de direção de Key).
// Save: a RAM do cartucho (64 KB, como no Mednafen) só vira save depois que o jogo grava nela.
// Régua: a temporização da CPU, do VIP, do temporizador e do controle segue o Mednafen (modo "accurate"); os
// testes (native/tools/t7_vb.cpp) comparam a imagem dos dois olhos ponto a ponto com o beetle-vb.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "v810.h"
#include "vip.h"
#include "vsu.h"

namespace indigo::vb {

bool isVbRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

class VB {
public:
  explicit VB(const std::vector<uint8_t>& rom);

  void reset();
  void runFrame();
  void setPad(uint16_t bits) { pad_ = bits; }  // bits do registro SDR (1 = apertado), já com os bits fixos
  void setIrq(int source, bool on);

  // barramento (endereços de 27 bits; a CPU acessa palavras como duas meias palavras)
  uint8_t read8(uint32_t a) {
    switch ((a >> 24) & 7) {
      case 5: return wram_[a & 0xFFFF];
      case 7: return rom_[a & romMask_];
      default: return ioRead8(a);
    }
  }
  uint16_t read16(uint32_t a) {
    switch ((a >> 24) & 7) {
      case 5: { const uint8_t* p = &wram_[a & 0xFFFE]; return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
      case 7: { const uint8_t* p = &rom_[a & romMask_ & ~1u]; return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
      default: return ioRead16(a);
    }
  }
  uint32_t read32(uint32_t a) { return read16(a) | (static_cast<uint32_t>(read16(a | 2)) << 16); }
  void write8(uint32_t a, uint8_t v) {
    if (((a >> 24) & 7) == 5) wram_[a & 0xFFFF] = v;
    else ioWrite8(a, v);
  }
  void write16(uint32_t a, uint16_t v) {
    if (((a >> 24) & 7) == 5) { wram_[a & 0xFFFE] = static_cast<uint8_t>(v); wram_[(a & 0xFFFE) + 1] = static_cast<uint8_t>(v >> 8); }
    else ioWrite16(a, v);
  }
  void write32(uint32_t a, uint32_t v) { write16(a, static_cast<uint16_t>(v)); write16(a | 2, static_cast<uint16_t>(v >> 16)); }
  // no Virtual Boy, IN/OUT e a busca de instruções usam o mesmo barramento da memória
  uint8_t in8(uint32_t a) { return read8(a); }
  uint16_t in16(uint32_t a) { return read16(a); }
  uint32_t in32(uint32_t a) { return read32(a); }
  void out8(uint32_t a, uint8_t v) { write8(a, v); }
  void out16(uint32_t a, uint16_t v) { write16(a, v); }
  void out32(uint32_t a, uint32_t v) { write32(a, v); }
  uint16_t fetch16(uint32_t a) { return read16(a); }
  static constexpr uint32_t V810_PIR = 0x00005346;  // "NVC"
  static constexpr bool V810_NINTENDO = true;

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  int64_t ts = 0;        // relógio da CPU (ciclos de 20 MHz desde que ligou)
  int64_t runUntil = 0;  // a CPU para quando ts alcança este ciclo
  V810 cpu;
  VIP vip;
  VSU vsu;
  std::vector<uint8_t> sram;  // 64 KB
  bool sramUsed = false, sramDirty = false;
  std::string title;
  uint8_t wram_[0x10000] = {};

private:
  uint8_t ioRead8(uint32_t a);
  uint16_t ioRead16(uint32_t a);
  void ioWrite8(uint32_t a, uint8_t v);
  void ioWrite16(uint32_t a, uint16_t v);
  uint8_t hwRead(uint32_t a);
  void hwWrite(uint32_t a, uint8_t v);
  void timerUpdate();
  int64_t timerNext() const;
  void inputUpdate();
  void sync(StateIO& io);
  void limit(int64_t at) { if (at < runUntil) runUntil = at; }

  std::vector<uint8_t> rom_;
  uint32_t romMask_ = 0;
  uint8_t irqLines_ = 0, wcr_ = 0;
  // temporizador (TLR/THR/TCR)
  uint8_t tcr_ = 0;
  uint16_t tReload_ = 0xFFFF, tCounter_ = 0xFFFF;
  int32_t tDivider_ = 2000;
  bool tStatus_ = false, tShadow_ = false, tReloadPending_ = false;
  int64_t tLast_ = 0;
  // controle (SCR/SDLR/SDHR)
  uint16_t pad_ = 2, padLatched_ = 0, sdr_ = 0;
  uint8_t scr_ = 0;
  int32_t readBit_ = 0, readCounter_ = 0;
  bool keyIrq_ = false;
  int64_t inLast_ = 0;
};

class VBCore : public Core {
public:
  VBCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  VB& vb() { return *m_; }

  // 0 = olho esquerdo em vermelho e preto, 1 = anáglifo vermelho/ciano, 2 = lado a lado (768×224)
  void setStereoMode(int mode);
  int stereoMode() const { return stereo_; }
  // segundo direcional (bits KEY_UP/DOWN/LEFT/RIGHT), somado ao mapeamento por X/Y
  void setRightPad(uint32_t mask) { rightPad_ = mask; applyKeys(); }

  std::string system() const override { return "vb"; }
  std::string title() const override { return m_->title; }
  void runFrame() override;
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { keys_ = mask; applyKeys(); }
  const uint32_t* framebuffer() const override { return fb_.data(); }
  int width() const override { return stereo_ == 2 ? 768 : 384; }
  int height() const override { return 224; }
  double fps() const override { return 20000000.0 / VIP::FRAME_CYCLES; }
  void setSampleRate(int rate) override { m_->vsu.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->vsu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->vsu.pull(l, r, n); }
  bool hasSave() const override { return m_->sramUsed; }
  bool saveDirty() const override { return m_->sramDirty; }
  std::vector<uint8_t> exportSave() override { m_->sramDirty = false; return m_->sram; }
  void importSave(const std::vector<uint8_t>& b) override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); compose(); }

private:
  void applyKeys();
  void compose();

  std::unique_ptr<VB> m_;
  std::vector<uint32_t> fb_;
  uint32_t keys_ = 0, rightPad_ = 0;
  int stereo_ = 0;
  uint32_t gamma_[256];
};

}  // namespace indigo::vb
