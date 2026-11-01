// PC Engine / TurboGrafx-16 (HuCard) e SuperGrafx.
// CPU HuC6280 a 7,16 MHz (huc6280.h), VDC HuC6270 (vdc.h; dois no SuperGrafx, misturados pelo VPC HuC6202),
// VCE HuC6260 (paleta de 512 cores, pixel de 5,37/7,16/10,7 MHz), PSG de 6 canais (psg.h), temporizador e
// controle de interrupções do HuC6280, controle de 2 botões (e o de 6 botões no Street Fighter II').
// Relógio mestre de 21,477 MHz, linha de 1365 ciclos, 262 ou 263 linhas (o jogo escolhe pelo VCE).
//
// Cartucho: ROM de até 2,5 MB (.pce; o cabeçalho de 512 bytes, quando existe, é removido), espelhada como no
// hardware (384/512/768 KB), mapeador do Street Fighter II' (bancos de 512 KB em $40-$7F, trocados escrevendo
// em $1FF0-$1FF3 do banco 0), 32 KB de RAM no Populous. SuperGrafx: extensão .sgx ou um dos jogos da lista.
// Save: a BRAM de 2 KB do Tennokoe 2 / Turbo Booster Plus / CD-ROM (banco $F7, liberada por $1807 e travada
// pela leitura de $1803), que alguns HuCards usam para gravar. Não há BIOS.
//
// Imagem: 256 x 240 no modo de 5,37 MHz, 341 x 240 no de 7,16 MHz, 512 x 480 (linhas dobradas) no de 10,7 MHz;
// as 240 linhas centrais das 242 visíveis. Se o quadro mistura modos, vale o da maioria das linhas.
//
// Teclas: direcional = direcional; A = botão I; B = botão II; SELECT = Select; START = Run.
// Controle de 6 botões (só no Street Fighter II' Champion Edition): L = III, Y = IV, X = V, R = VI.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "huc6280.h"
#include "psg.h"
#include "vdc.h"

namespace indigo::pce {

bool isPceRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

// Ponto de extensão para o CD-ROM (src/pcecd): a área $1800-$1BFF, bancos de memória extras (RAM do CD, do
// Super System Card, portas do Arcade Card), a linha IRQ2 e o tempo. Sem extensão o PC Engine fica como era.
struct PceExpansion {
  virtual ~PceExpansion() = default;
  virtual void mapBanks(HuC6280& cpu) = 0;                         // depois do mapeamento normal
  virtual uint8_t ioRead(uint16_t off) = 0;                        // off = $1800-$1BFF (já sincronizado)
  virtual void ioWrite(uint16_t off, uint8_t v) = 0;
  virtual uint8_t bankRead(uint8_t bank, uint16_t off) = 0;        // bancos que mapBanks deixou nulos
  virtual void bankWrite(uint8_t bank, uint16_t off, uint8_t v) = 0;
  virtual void runTo(int64_t clk) = 0;                             // anda até o relógio mestre dado
  virtual int64_t nextEvent() const = 0;                           // próximo instante em que pode mudar a IRQ2
};

class Pce : public CpuBus {
public:
  Pce(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, bool sgxByName);

  HuC6280 cpu;
  Vdc vdc[2];
  Vce vce;
  bool sgx = false;

  void reset();
  // liga a extensão (CD-ROM); o mapa de bancos e o tempo passam a incluí-la
  void setExpansion(PceExpansion* e) { ext_ = e; mapBanks(); schedule(); }
  void reschedule() { schedule(); }
  void stateSync(StateIO& io) { sync(io); }  // estado do PC Engine dentro de um estado maior
  void setKeys(uint32_t mask) { keys_ = mask; }
  void runFrame();
  const uint32_t* video() const { return fb_.data(); }
  int width() const { return outW_; }
  int height() const { return outH_; }
  double fps() const { return 21477272.727272 / (1365.0 * vce.lines); }

  void setSampleRate(int rate) { psg_.setSampleRate(rate); }
  size_t audioAvailable() const { return psg_.available(); }
  void pullAudio(float* l, float* r, size_t n) { psg_.pull(l, r, n); }

  bool saveDirty() const { return bramDirty_; }
  std::vector<uint8_t> exportSave() { bramDirty_ = false; return bram_; }
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);
  std::string note() const;
  uint32_t frameCount() const { return vdc[0].frames(); }
  const uint16_t* rawLine(int r) const { return lines_[r]; }  // linhas de 9 bits (testes)
  uint8_t rawDiv(int r) const { return lineDiv_[r]; }

  // chamados pelo VDC
  void stall();
  void lineBegin(int idx, int scanline);
  void lineDone(int idx, int scanline);
  void frameDone(int idx);

  // CpuBus
  uint8_t slowRead(uint8_t bank, uint16_t off) override;
  void slowWrite(uint8_t bank, uint16_t off, uint8_t v) override;
  void stWrite(int port, uint8_t v) override;
  void sync() override;

private:
  std::vector<uint8_t> rom_, ram_, bram_, cardRam_;
  std::vector<uint8_t> ffBank_ = std::vector<uint8_t>(0x2000, 0xFF), sink_ = std::vector<uint8_t>(0x2000);
  int romBanks_ = 0;
  PceExpansion* ext_ = nullptr;
  bool sf2_ = false, sixButton_ = false;
  uint8_t sf2Bank_ = 0;
  bool bramLocked_ = true, bramDirty_ = false;
  uint8_t ioBuf_ = 0;
  // temporizador
  bool timerOn_ = false;
  uint8_t timerReload_ = 0, timerCounter_ = 0;
  int64_t timerNext_ = 0;
  // controle
  uint32_t keys_ = 0;
  uint8_t padSel_ = 0, padClr_ = 0, padExtra_ = 0;
  // VPC (SuperGrafx)
  uint8_t vpcPrio_[2] = {0x11, 0x11};
  uint16_t vpcWin_[2] = {0, 0};
  uint8_t vpcSt2_ = 0;
  // vídeo
  uint16_t lines_[242][Vdc::ROW] = {};
  uint8_t lineDiv_[242] = {};
  uint16_t row2_[Vdc::ROW] = {};
  std::vector<uint32_t> fb_ = std::vector<uint32_t>(512 * 480);
  int outW_ = 256, outH_ = 240;
  uint32_t lut_[1024] = {};
  bool frameReady_ = false;
  // som
  Psg psg_;
  int64_t psgClk_ = 0;

  void mapBanks();
  void power();
  void schedule();
  void updateIrq() {
    const bool v = vdc[0].irq() || (sgx && vdc[1].irq());
    cpu.irqActive = static_cast<uint8_t>((cpu.irqActive & ~HuC6280::IRQ1) | (v ? HuC6280::IRQ1 : 0));
  }
  void runTimer(int64_t t);
  uint8_t timerRead() const;
  void psgRun();
  uint8_t padRead() const;
  void padWrite(uint8_t v);
  uint8_t vdcRead(uint16_t off);
  void vdcWrite(uint16_t off, uint8_t v);
  uint8_t vceRead(uint16_t off);
  void vceWrite(uint16_t off, uint8_t v);
  void drawAll() { vdc[0].draw(); if (sgx) vdc[1].draw(); }
  void composeSgx(uint16_t* out);
  void buildFrame();
  void sync(StateIO& io);
};

class PceCore : public Core {
public:
  PceCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, bool sgxByName = false)
      : m_(std::make_unique<Pce>(rom, save, sgxByName)) {}
  Pce& pce() { return *m_; }

  std::string system() const override { return m_->sgx ? "sgx" : "pce"; }
  std::string title() const override { return ""; }  // o HuCard não traz título
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->video(); }
  int width() const override { return m_->width(); }
  int height() const override { return m_->height(); }
  double fps() const override { return m_->fps(); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return m_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::string note() const override { return m_->note(); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<Pce> m_;
};

// cria o núcleo a partir do nome do arquivo (".sgx" liga o SuperGrafx)
std::unique_ptr<PceCore> createPceCore(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo,
                                       const std::vector<uint8_t>* save);

}  // namespace indigo::pce
