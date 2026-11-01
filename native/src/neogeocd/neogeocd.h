// Neo Geo CD: a mesma placa do Neo Geo (68000 a 12 MHz, Z80 a 4 MHz, vídeo LSPC2 e YM2610, reaproveitados de
// src/neogeo e src/md) com as memórias em RAM, carregadas do CD pela BIOS:
//   $000000-$1FFFFF  2 MB de RAM de programa (os $80 bytes dos vetores vêm da BIOS até $3A0013)
//   $800000-$8FFFFF  8 KB de RAM de backup (bytes ímpares) = o save
//   $C00000-$CFFFFF  BIOS de 512 KB (espelhada)
//   $E00000-$EFFFFF  janela de upload: sprites (4 MB, bancos de 1 MB), camada fixa (128 KB), PCM do YM2610
//                    (1 MB, bancos de 512 KB) e programa do Z80 (64 KB), escolhidos em $FF0105 e liberados em
//                    $FF0121..$FF0149 (as três últimas só nos bytes ímpares)
//   $FF0000-$FF01FF  registradores do CD: interrupções ($FF0002/$FF0004/$FF000F), DMA ($FF0061..$FF008E),
//                    LC8951 ($FF0101/$FF0103), comunicação com a unidade ($FF0161..$FF0181), chaves de vídeo,
//                    reset do Z80 ($FF0183), bancos ($FF01A1/$FF01A3), som do CD ($FF0188/$FF018A)
// Interrupções: quadro = nível 1 (vetor $68), CD = nível 2 (vetor $54 decodificador, $58 comunicação), linha
// (temporizador da LSPC) = nível 3 (vetor $64). Controle da unidade, LC8951 e CD-DA em cdunit.h.
//
// Régua: o NeoCD (neocd_libretro, LGPL-3), que emula o CD no nível do hardware e roda as BIOS originais; o
// comportamento dos registradores segue o dele (ver native/tools/t7_neogeocd.sh).
//
// BIOS (obrigatória): indigo::firmware("neogeocd") = a BIOS do Neo Geo CD do usuário (512 KB; front-loader,
// top-loader, CDZ, ou as modificações conhecidas: Universe BIOS CD, SMKDan). Reconhecida pelo conteúdo
// (isNeoGeoCdBios), em qualquer ordem de bytes. Como o NeoCD, aplica na BIOS original o remendo que pula o
// reconhecimento do disco (uma checagem de proteção que as imagens de CD não reproduzem).
//
// Teclas: as mesmas do Neo Geo de cartucho (B do Índigo = A, A = B, Y = C, X = D; START; SELECT = Select).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"
#include "../md/m68k.h"
#include "../neogeo/lspc.h"
#include "../neogeo/ym2610.h"
#include "../sms/z80.h"
#include "cdunit.h"

namespace indigo::neogeocd {

// BIOS do Neo Geo CD (512 KB, em qualquer ordem de bytes): pilha inicial $10F300, reset na BIOS e os vetores das
// interrupções apontando para a BIOS. family: 0 desconhecida (só a estrutura bate), 1 front-loader, 2 top-loader,
// 3 CDZ.
bool isNeoGeoCdBios(const std::vector<uint8_t>& bytes, int* family = nullptr);

class NeoGeoCd : public md::M68kBus, public Z80Bus {
public:
  NeoGeoCd(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save);
  ~NeoGeoCd() override;

  md::M68k cpu;
  Z80 z80;
  neogeo::LSPC lspc;
  neogeo::YM2610 ym;
  std::string biosName, title;
  int frame = 0;

  void reset();
  void setKeys(uint32_t k) { keys_ = k; }
  void runFrame();
  static double masterClock() { return 24167829.0; }
  double fps() const { return masterClock() / (2.0 * neogeo::LSPC::CYC_PER_LINE * neogeo::LSPC::LINES); }

  void setSampleRate(int rate);
  size_t audioAvailable() const { return ym.available(); }
  void pullAudio(float* l, float* r, size_t n);

  bool saveDirty() const { return dirty_; }
  std::vector<uint8_t> exportSave() { dirty_ = false; return backup_; }
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // M68kBus
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  int intAck(int level) override;
  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int port) override;
  void out(int port, int v) override;

  const std::vector<uint8_t>& workRam() const { return ram_; }  // usado pelos testes
  const std::vector<uint8_t>& backupRam() const { return backup_; }
  const std::vector<uint8_t>& sprRam() const { return spr_; }
  const std::vector<uint8_t>& fixRam() const { return fix_; }
  const std::vector<uint8_t>& pcmRam() const { return pcm_; }
  const std::vector<uint8_t>& z80Ram() const { return zram_; }

private:
  enum Area { AREA_SPR = 1, AREA_PCM = 2, AREA_Z80 = 4, AREA_FIX = 8 };
  enum Irq { IRQ_VBL = 1, IRQ_DECODER = 2, IRQ_COMM = 4, IRQ_RASTER = 8 };

  void loadBios();
  void sync(StateIO& io);
  void step();
  void syncZ80(int64_t target68k);
  int64_t now() const { return cyc_ + cpu.cycles; }
  int irqLevel() const;
  void cdTick();

  // 68000: áreas
  uint8_t inputRead(uint32_t a);  // byte de cima de $300000/$340000/$380000
  uint16_t lspcRead(uint32_t a);
  void lspcWrite(uint32_t a, uint16_t v);
  uint8_t windowRead8(uint32_t a);
  uint16_t windowRead16(uint32_t a);
  void windowWrite8(uint32_t a, uint8_t v);
  void windowWrite16(uint32_t a, uint16_t v);
  uint8_t cdRead8(uint32_t a);
  uint16_t cdRead16(uint32_t a);
  void cdWrite8(uint32_t a, uint8_t v);
  void cdWrite16(uint32_t a, uint16_t v);
  void soundCommand(uint8_t v);
  void switchWrite(uint32_t a);
  void backupWrite(uint32_t a, uint8_t v);

  // DMA
  void dmaStart();
  void dmaReset();
  bool dmaRegion(uint32_t a, uint32_t& base, uint32_t& mask) const;
  uint16_t dmaRead(uint32_t base, uint32_t mask, uint32_t& off);
  void dmaWrite(uint32_t base, uint32_t mask, uint32_t& off, uint16_t v);
  void dmaFromCd(bool oddBytes);

  std::unique_ptr<cdrom::Disc> disc_;
  CdUnit cd_;
  std::vector<uint8_t> bios_, l0_;
  std::vector<uint8_t> ram_, spr_, fix_, pcm_, zram_, backup_;
  int biosFamily_ = 0;

  // registradores
  bool vecRom_ = true;
  uint8_t area_ = 0, busReq_ = 0, sprBank_ = 0, pcmBank_ = 0, selector_ = 0;
  uint16_t irqMask1_ = 0, irqMask2_ = 0;
  int irqPending_ = 0;
  bool commOn_ = false, z80On_ = false, wdOn_ = false;
  uint32_t wdCounter_ = 0;
  uint16_t dmaCfg_[9] = {};
  uint32_t dmaSrc_ = 0, dmaDst_ = 0, dmaLen_ = 0;
  uint16_t dmaPattern_ = 0;
  bool inDma_ = false;
  // som
  uint8_t soundCode_ = 0, soundReply_ = 0;
  bool nmiEnabled_ = false;
  // temporizador da interrupção de linha (o mesmo do cartucho)
  uint8_t timerCtrl_ = 0;
  uint32_t timerReload_ = 0, timerCounter_ = 0, timerFrag_ = 0;
  int32_t cdCounter_ = 0;

  int64_t cyc_ = 0, frameEnd_ = 0, z80Cyc_ = 0;
  uint32_t keys_ = 0;
  bool dirty_ = false;
  int outRate_ = 48000;
  std::vector<float> cdL_, cdR_;  // CD-DA já na taxa de saída, alinhado com a fila do YM2610
  size_t cdW_ = 0, cdRd_ = 0;
};

class NeoGeoCdCore : public Core {
public:
  NeoGeoCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save)
      : m_(std::make_unique<NeoGeoCd>(std::move(disc), save)) {}
  NeoGeoCd& ngcd() { return *m_; }

  std::string system() const override { return "neogeocd"; }
  std::string title() const override { return m_->title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->lspc.fb; }
  int width() const override { return neogeo::LSPC::OUT_W; }
  int height() const override { return neogeo::LSPC::OUT_H; }
  double fps() const override { return m_->fps(); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return m_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::string note() const override { return "BIOS: " + m_->biosName; }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<NeoGeoCd> m_;
};

}  // namespace indigo::neogeocd
