// Neo Geo MVS/AES (cartucho): 68000 a 12 MHz (o mesmo núcleo do Mega Drive, validado com os SingleStepTests),
// Z80 a 4 MHz para o som, vídeo LSPC2 (lspc.h) e YM2610 (ym2610.h). Relógio mestre de 24 MHz (MVS; o AES usa
// 24,167829 MHz): 264 linhas x 768 ciclos do 68000 por quadro = 59,1856 quadros/s (AES 59,5995).
//
// BIOS (obrigatória): indigo::firmware("neogeo") = bytes do neogeo.zip do usuário (o pacote de BIOS do MAME).
// Escolha, nesta ordem: Universe BIOS (uni-bios_*.rom, a versão mais nova) -> MVS (sp-s2.sp1, sp-s3.sp1,
// sp-s.sp1, sp-u2.sp1, sp1-u2, sp-e.sp1, sp-45.sp1, sp-j2.sp1, vs-bios.rom, japan-j3.bin...) -> AES (neo-epo.bin,
// neo-po.bin). Universe BIOS e as BIOS de MVS rodam o console no modo MVS (fliperama: moedas, RAM de backup com
// as configurações e recordes); as de AES, no modo AES (console de casa, salva no cartão de memória). Do mesmo
// .zip vêm sfix.sfix (camada fixa da BIOS), sm1.sm1 (som da BIOS) e 000-lo.lo (tabela da redução vertical; se
// faltar é gerada). A nullbios do ngdevkit (LGPL-3, não embutida) também funciona como neogeo.zip.
//
// Jogos: .neo (arquivo único) ou o .zip do MAME entregue inteiro (ver cart.h e isNeoGeoSet).
//
// Teclas (bits de Key do Índigo, na posição dos botões de um controle de SNES/Xbox): o controle do Neo Geo tem
// A B C D em linha; no Índigo:
//   B do Índigo (embaixo)       = A do Neo Geo
//   A do Índigo (à direita)     = B do Neo Geo
//   Y do Índigo (à esquerda)    = C do Neo Geo
//   X do Índigo (em cima)       = D do Neo Geo
//   START = Start; SELECT = moeda (MVS) ou Select (AES); L = botão de serviço/teste do MVS (abre o menu da BIOS);
//   direcional = alavanca. O jogador 2 fica sem nada apertado.
//
// Save (hasSave): RAM de backup do MVS (64 KB) + cartão de memória (2 KB) [+ 8 KB da RAM do cartucho nos jogos
// que têm], nessa ordem.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../md/m68k.h"
#include "../sms/z80.h"
#include "cart.h"
#include "lspc.h"
#include "ym2610.h"

namespace indigo::neogeo {

// .neo, ou .zip de jogo de Neo Geo (o app deve entregar o .zip inteiro)
bool isNeoGeoRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// .zip no formato do MAME com um jogo de Neo Geo (pela tabela de CRCs/nomes ou pelos sufixos .p1/.c1...)
bool isNeoGeoSet(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo);
// .zip com a BIOS do Neo Geo (neogeo.zip do MAME/FBNeo/Geolith, aes.zip ou a nullbios): o app guarda os bytes
// com setFirmware("neogeo", ...) em vez de abrir como jogo
bool isNeoGeoBiosZip(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo);

class NeoGeo : public md::M68kBus, public Z80Bus {
public:
  // rom = .neo ou .zip; nome = nome do arquivo (para escolher o pacote do .zip)
  NeoGeo(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save);
  ~NeoGeo() override;

  md::M68k cpu;
  Z80 z80;
  LSPC lspc;
  YM2610 ym;
  Cart cart;
  bool mvs = true;
  std::string biosName;
  int frame = 0;

  void reset();
  void setKeys(uint32_t k) { keys_ = k; }
  void setKeys2(uint32_t k) { keys2_ = k; }
  void runFrame();
  double masterClock() const { return mvs ? 24000000.0 : 24167829.0; }
  double fps() const { return masterClock() / (2.0 * LSPC::CYC_PER_LINE * LSPC::LINES); }

  void setSampleRate(int rate);
  size_t audioAvailable() const { return ym.available(); }
  void pullAudio(float* l, float* r, size_t n) { ym.pull(l, r, n); }

  bool saveDirty() const { return dirty_; }
  std::vector<uint8_t> exportSave();
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // M68kBus
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int port) override;
  void out(int port, int v) override;

  const std::vector<uint8_t>& workRam() const { return ram_; }  // usado pelos testes

private:
  void loadBios();
  void softReset();  // vigia (watchdog): o aparelho reinicia sem apagar as memórias
  void sync(StateIO& io);
  void step();
  void syncZ80(int64_t target68k);
  int64_t now() const { return cyc_ + cpu.cycles; }
  int irqLevel() const;
  void selectFix();

  // 68000: áreas
  uint16_t romWord(uint32_t a);
  uint16_t bankRead16(uint32_t a);
  uint8_t bankRead8(uint32_t a);
  void bankWrite(uint32_t a, uint16_t v, bool byte);
  uint16_t ioRead16(uint32_t a);
  uint8_t ioRead8(uint32_t a);
  void ioWrite(uint32_t a, uint16_t v, bool byte);
  void systemLatch(int reg);
  void soundCommand(uint8_t v);
  uint8_t statusA();
  uint8_t statusB();
  uint8_t p1() { return pad(keys_); }
  uint8_t pad(uint32_t k);  // REG_P1CNT / REG_P2CNT (1 = solto)

  // RTC uPD4990A (só MVS)
  void rtcWrite(uint8_t v);
  void rtcTick(int cycles);
  uint8_t rtcRead() const;

  std::vector<uint8_t> bios_, sfix_, sm1_, l0_;
  std::vector<uint8_t> ram_, nvram_, memcard_, zram_, cartRam_;
  uint32_t pFixedMask_ = 0;
  bool hasCartRam_ = false;

  // registradores do sistema
  bool vecBios_ = true, sramLock_ = false, crtFix_ = true;
  uint8_t crdLock_[2] = {}, crdRegSel_ = 0, pOutput_ = 0;
  uint8_t soundCode_ = 0, soundReply_ = 0;
  bool nmiEnabled_ = false, nmiPending_ = false;
  uint32_t zbank_[4] = {0x8000, 0xC000, 0xE000, 0xF000};
  // interrupções: bits 0 = quadro (nível 1), 1 = temporizador (2), 2 = reset (3)
  int irqPending_ = 0;
  uint8_t timerCtrl_ = 0;
  uint32_t timerReload_ = 0, timerCounter_ = 0, timerFrag_ = 0;
  uint32_t watchdog_ = 0;
  // cartucho
  uint32_t bankBase_ = 0;
  uint16_t cartReg_[2] = {};
  uint32_t protReg_ = 0;
  int kof98State_ = 0;
  uint16_t smaRng_ = 0x2345;
  std::vector<uint16_t> pvcRam_;
  // RTC
  uint8_t rtcCmd_ = 0, rtcClk_ = 0, rtcStb_ = 0, rtcTp_ = 0, rtcTpMode_ = 0, rtcFmode_ = 0, rtc1Hz_ = 0;
  uint64_t rtcData_ = 0;
  uint32_t rtcCycles_ = 0, rtcTpInterval_ = 12000000, rtcTpCounter_ = 0;
  int rtcTime_[7] = {};  // segundo, minuto, hora, dia, dia da semana, mês, ano

  int64_t cyc_ = 0, frameEnd_ = 0, z80Cyc_ = 0;
  int resetStall_ = 0;
  uint32_t keys_ = 0, keys2_ = 0;
  bool dirty_ = false;
};

class NeoGeoCore : public Core {
public:
  NeoGeoCore(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save)
      : m_(std::make_unique<NeoGeo>(rom, nome, save)) {}
  // assinatura comum dos núcleos (sem o nome: o .neo não precisa; o .zip escolhe o pacote pelos CRCs)
  NeoGeoCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) : NeoGeoCore(rom, "", save) {}
  NeoGeo& ng() { return *m_; }

  std::string system() const override { return "neogeo"; }
  std::string title() const override { return m_->cart.title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  void setKeys2(uint32_t mask) override { m_->setKeys2(mask); }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return m_->lspc.fb; }
  int width() const override { return LSPC::OUT_W; }
  int height() const override { return LSPC::OUT_H; }
  double fps() const override { return m_->fps(); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return m_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::string note() const override { return m_->cart.note; }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<NeoGeo> m_;
};

}  // namespace indigo::neogeo
