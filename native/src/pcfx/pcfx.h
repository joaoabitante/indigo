// Núcleo PC-FX (NEC, 1994): CPU NEC V810 a 21,48 MHz (src/v810, a mesma do Virtual Boy), 2 MB de RAM, BIOS de
// 1 MB do usuário (pcfx.rom, obrigatória), dois VDCs HuC6270 (o VDC do PC Engine, src/pce/vdc, com 64 K
// palavras de VRAM cada), o KING HuC6272 (KRAM, planos de fundo, CD por SCSI, DMA, ADPCM) e o misturador
// HuC6261 (king.h), som HuC6230 (soundbox.h: PSG + ADPCM + CD-DA), temporizador, controlador de interrupções
// (7 fontes com prioridade programável), controle (porta 0 e 1) e a memória de save (32 KB internos + 32 KB do
// cartão FX-BMP).
//
// Tela: 256 pontos (5,37 MHz) ou 341 (7,16 MHz) por 240 linhas (linhas 22 a 261 do HuC6261).
// Teclas: A = I, B = II, X = III, Y = IV, L = V, R = VI, SELECT = Select, START = Run.
// Ainda não emulado: o decodificador de vídeo comprimido HuC6271 (RAINBOW, o plano dele fica vazio), o HuC6273
// (3D do PC-FXGA), o mouse e o multitap.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"
#include "../pce/vdc.h"
#include "../v810/v810.h"
#include "king.h"
#include "scsicd.h"
#include "soundbox.h"

namespace indigo::pcfx {

// BIOS do PC-FX: 1 MB; o nome do arquivo tem de lembrar o console (pcfx.rom, pcfxbios.bin...) porque a BIOS não
// tem cabeçalho que a identifique
bool isPcfxBios(const std::vector<uint8_t>& b, const std::string& lowerName);
// "PC-FX:Hu_CD-ROM" no primeiro setor da primeira faixa de dados
bool isPcfxDisc(cdrom::Disc& d);

class Pcfx : public pce::VdcHost {
public:
  static constexpr double CPU_HZ = 236250000.0 / 11.0;
  static constexpr int LINE = 1365;
  static constexpr int MAXW = 341, H = 240;

  Pcfx(const std::vector<uint8_t>& bios, cdrom::Disc* disc);
  void reset();
  void runFrame();
  void setPad(int port, uint16_t bits) { pad_[port & 1] = bits; }

  // ---- barramento da CPU (veja src/v810/v810.h)
  int64_t ts = 0, runUntil = 0;
  static constexpr uint32_t V810_PIR = 0x00008100;
  static constexpr bool V810_NINTENDO = false;
  uint8_t read8(uint32_t a);
  uint16_t read16(uint32_t a);
  uint32_t read32(uint32_t a);
  void write8(uint32_t a, uint8_t v);
  void write16(uint32_t a, uint16_t v);
  void write32(uint32_t a, uint32_t v);
  uint8_t in8(uint32_t a) { return portRead8(a); }
  uint16_t in16(uint32_t a) { return portRead16(a & ~1u); }
  uint32_t in32(uint32_t a) { return portRead16(a & ~3u) | uint32_t(portRead16((a & ~3u) | 2)) << 16; }
  void out8(uint32_t a, uint8_t v) { portWrite8(a, v); }
  void out16(uint32_t a, uint16_t v) { portWrite16(a & ~1u, v); }
  void out32(uint32_t a, uint32_t v) { portWrite16(a & ~3u, uint16_t(v)); portWrite16((a & ~3u) | 2, uint16_t(v >> 16)); }
  uint16_t fetch16(uint32_t a) {
    if (a < 0x200000) return uint16_t(ram[a] | ram[a + 1] << 8);
    if (a >= 0xF0000000) return uint16_t(bios_[a & 0xFFFFF] | bios_[(a & 0xFFFFF) + 1] << 8);
    return read16(a);
  }

  // ---- VdcHost
  void stall() override;
  void lineBegin(int idx, int scanline) override {}
  void lineDone(int idx, int scanline) override;
  void frameDone(int idx) override;

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  v810::Cpu<Pcfx> cpu;
  pce::Vce vdcVce;
  pce::Vdc vdc[2];
  FxVce vce;
  ScsiCd cd;
  King king;
  SoundBox sound;
  std::vector<uint8_t> ram = std::vector<uint8_t>(0x200000);
  std::vector<uint8_t> bram = std::vector<uint8_t>(0x10000);  // 32 KB internos + 32 KB do cartão externo
  bool bramDirty = false;

  uint32_t fb[MAXW * H] = {};
  int fbWidth = 256;
  uint32_t frames = 0;

  // para os testes
  uint16_t irqAsserted() const { return irqAsserted_; }

private:
  std::vector<uint8_t> bios_;
  uint16_t pad_[2] = {};
  // interrupções
  uint16_t irqAsserted_ = 0, irqMask_ = 0xFFFF, irqPrio_[2] = {};
  // temporizador
  uint16_t tmrCtrl_ = 0, tmrPeriod_ = 0;
  int32_t tmrCounter_ = 0;
  int64_t tmrLast_ = 0;
  // controle
  uint8_t padCtrl_[2] = {};
  bool padLatched_[2] = {};
  int64_t padLatchAt_[2] = {INT64_MAX, INT64_MAX};
  uint32_t padData_[2] = {};
  // diversos
  uint16_t backupCtrl_ = 0, lastVdcAr_[2] = {};
  uint8_t exBusReset_ = 0;
  uint32_t ramPage_ = 0;
  // linha do HuC6261
  int64_t lineBase_ = 0, nextLineEvt_ = 0;
  uint8_t lineEvt_ = 0;  // 0 = próximo evento é a troca de linha (48), 1 = começo da imagem (341)
  uint16_t lines_ = 263;
  bool vdcIrq_[2] = {};
  bool frameReady_ = false;
  // linha em montagem
  uint32_t bgLine_[272] = {};
  struct LineCache {
    uint16_t pictureMode, priority[2], palOffset[4], ble, spbl, ccr, coef[6];
    uint8_t layerPrio[8];
  } lc_{};
  uint16_t vdcRow_[2][pce::Vdc::ROW + 1] = {};
  int16_t rowDone_[2] = {-1, -1};
  uint32_t lineOut_[H][MAXW] = {};
  uint16_t lineW_[H] = {};

  void irqAssert(int src, bool on);
  void irqUpdate();
  uint16_t irqRead16(uint32_t a);
  void irqWrite16(uint32_t a, uint16_t v);
  void timerUpdate(int64_t t);
  int64_t timerNext() const;
  void padUpdate(int64_t t);
  uint16_t padRead16(uint32_t a);
  void padWrite16(uint32_t a, uint16_t v);
  uint8_t portRead8(uint32_t a);
  uint16_t portRead16(uint32_t a);
  void portWrite8(uint32_t a, uint8_t v);
  void portWrite16(uint32_t a, uint16_t v);
  uint16_t vdcRead16(int chip, uint32_t a);
  void vdcWrite16(int chip, uint32_t a, uint16_t v);
  void runVdcs(int64_t t);
  void runLine(int64_t t);
  void syncAll(int64_t t);
  int64_t nextEvent() const;
  void activeStart();
  void mixLine(int raster);
  void buildFrame();
  void ramTiming(uint32_t a) {
    if ((a & ~0x7FFu) != ramPage_) { ts += 3; ramPage_ = a & ~0x7FFu; }
  }
  void sync(StateIO& io);
};

class PcfxCore : public Core {
public:
  // disco (.cue/.bin, .chd); bios nulo = firmware("pcfx"). Lança std::runtime_error sem a BIOS.
  PcfxCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios = nullptr);

  std::string system() const override { return "pcfx"; }
  std::string title() const override { return title_; }
  void runFrame() override { sys_->runFrame(); }
  void reset() override { sys_->reset(); }
  void setKeys(uint32_t mask) override;
  const uint32_t* framebuffer() const override { return sys_->fb; }
  int width() const override { return sys_->fbWidth; }
  int height() const override { return Pcfx::H; }
  double fps() const override { return Pcfx::CPU_HZ / (double(Pcfx::LINE) * 263); }
  void setSampleRate(int rate) override { sys_->sound.setSampleRate(rate); }
  size_t audioAvailable() const override { return sys_->sound.available(); }
  void pullAudio(float* l, float* r, size_t n) override { sys_->sound.pull(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return sys_->bramDirty; }
  std::vector<uint8_t> exportSave() override { sys_->bramDirty = false; return sys_->bram; }
  void importSave(const std::vector<uint8_t>& bytes) override;
  std::string note() const override { return note_; }
  std::vector<uint8_t> saveState() override { return sys_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { sys_->loadState(s); }

  Pcfx& sys() { return *sys_; }

private:
  std::unique_ptr<cdrom::Disc> disc_;
  std::unique_ptr<Pcfx> sys_;
  std::string title_, note_;
};

}  // namespace indigo::pcfx
