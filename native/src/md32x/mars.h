// Sega 32X ("Mars"): acessório do Mega Drive com dois SH-2 a 23 MHz (relógio mestre × 3/7), 256 KB de SDRAM,
// VDP próprio (dois quadros de 128 KB, paleta de 256 cores, modos packed/direct color/run-length, preenchimento
// automático) composto por cima da imagem do Mega Drive pela prioridade, som PWM (dois canais com FIFO de 3) e os
// registradores de comunicação (8 palavras), interrupções (VRES/VINT/HINT/CMD/PWM), FIFO de DREQ (DMA do 68000
// para o SH-2) e o mapeamento do cartucho para o 68000 (880000 fixo, 900000 em bancos de 1 MB, RV).
//
// Partida sem BIOS (HLE, como o PicoDrive): a BIOS do 68000 vira uma tabela de vetores que aponta para a tabela
// de saltos do cartucho (880200 + 6·n) e as dos SH-2 são programas pequenos escritos aqui. Quando o 68000 solta o
// reset dos SH-2, o mestre recebe o código do cartucho na SDRAM (cabeçalho em 3D4), VBR/GBR, a soma do cartucho
// em COMM8, espera um pouco, escreve "M_OK" e salta; o escravo espera o "M_OK", escreve "S_OK" e salta. Com as
// BIOS do usuário (setFirmware "32x_m68k", "32x_master", "32x_slave") elas são usadas no lugar.
//
// Sincronia: os SH-2 rodam atrás do 68000, em fatias curtas, alcançando-o a cada linha e a cada acesso do 68000
// aos registradores do 32X (comunicação, PWM, VDP, quadro).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../md/md.h"
#include "sh2.h"

namespace indigo::md32x {

// ROM do 32X: extensão .32x, ou "MARS CHECK MODE"/"SEGA 32X" no cabeçalho
bool isMd32xRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

class Mars : public md::MdBus, public Sh2Bus {
public:
  Mars(md::MD& md, const std::vector<uint8_t>& rom);
  ~Mars() override;

  md::MD& md;
  Sh2 msh2, ssh2;

  void power();                       // estado de ligar: adaptador desligado, SH-2 parados
  void frameEnd(size_t count);        // alcança o 68000 e produz `count` amostras de som do quadro
  bool usingBios() const { return realBios_; }

  // som (PWM) na taxa de saída
  void setSampleRate(int rate);
  size_t audioAvailable() const;
  void mixInto(float* l, float* r, size_t n);  // soma o PWM ao som do Mega Drive

  void sync(StateIO& io);

  // MdBus
  bool busRead(uint32_t a, int size, uint16_t& v) override;
  bool busWrite(uint32_t a, int size, uint16_t v) override;
  void busLine(int line, int64_t mclk) override;
  void busLineDrawn(int row, uint32_t* px, const uint8_t* idx, int w) override;

  // Sh2Bus
  uint8_t rd8(Sh2& c, uint32_t a) override;
  uint16_t rd16(Sh2& c, uint32_t a) override;
  void wr8(Sh2& c, uint32_t a, uint8_t v) override;
  void wr16(Sh2& c, uint32_t a, uint16_t v) override;
  void dmaKick(Sh2& c, int ch) override;

  // usados pelos testes
  const std::vector<uint8_t>& sdram() const { return sdram_; }
  uint16_t comm(int i) const { return comm_[i & 7]; }
  int64_t sh2Now() const { return now_; }

private:
  bool running() const { return (aden_ && nres_); }
  int64_t sh2Time(int64_t mclk) const { return mclk * 3 / 7; }
  void syncTo(int64_t t);
  void notePoll(Sh2& c, uint32_t a, uint16_t v, int64_t until);
  void wake(int i);
  void sync68k() { syncTo(sh2Time(md.clock())); }
  void resetSh2s();
  void buildBios();
  void mapPages();
  void updateIrl();
  void raise(int bit);              // pedido de interrupção nos SH-2 que o liberam na máscara
  // registradores
  uint16_t sysRead68k(uint32_t a);
  void sysWrite68k(uint32_t a, int size, uint16_t v);
  uint16_t sysReadSh2(Sh2& c, uint32_t a);
  void sysWriteSh2(Sh2& c, uint32_t a, int size, uint16_t v);
  uint16_t vdpRead(uint32_t a);
  uint16_t vdpReadAt(uint32_t a, int64_t t);
  void vdpWrite(uint32_t a, int size, uint16_t v);
  void vdpWriteAt(uint32_t a, int size, uint16_t v, int64_t t, bool fromSh2);
  void palWrite(uint32_t a, int size, uint16_t v);
  void fbWrite(uint32_t a, int size, uint16_t v);
  uint16_t fbRead(uint32_t a, int size);
  // DREQ (FIFO do 68000 para o SH-2)
  void fifoPush(uint16_t v);
  uint16_t fifoPop();
  void dreq0();
  // PWM
  void pwmTick();
  uint16_t pwmRead(uint32_t a);
  void pwmWrite(uint32_t a, uint16_t v);
  void pwmPush(int ch, uint16_t v);
  void pwmByteWrite(uint32_t a, int size, uint16_t v);
  void pwmCtlChanged();
  // quadro
  void vblankStart();
  void swapFb();

  std::vector<uint8_t> rom_, sdram_ = std::vector<uint8_t>(0x40000);
  std::vector<uint8_t> fb_[2] = {std::vector<uint8_t>(0x20000), std::vector<uint8_t>(0x20000)};
  uint8_t biosM_[2048] = {}, biosS_[1024] = {}, bios68_[256] = {};
  bool realBios_ = false;
  bool bypass_ = false;             // acesso repassado ao Mega Drive (cartucho em 880000/900000)
  uint32_t romMask_ = 0;

  // estado do adaptador
  bool aden_ = false, nres_ = true, fm_ = false, rv_ = false, s68_ = false, dmaBit_ = false, hen_ = false;
  uint8_t intCmd_ = 0;              // A15102: bit 0 = mestre, bit 1 = escravo
  uint8_t bank_ = 0;
  uint8_t mask_[2] = {};            // máscara de interrupções de cada SH-2 (V H CMD PWM)
  uint8_t pend_[2] = {};            // pedidos (bits: 0 PWM, 1 CMD, 2 HINT, 3 VINT, 4 VRES)
  uint8_t hcount_ = 0;              // linhas entre as HINT do 32X
  int hcnt_ = 0;
  uint16_t comm_[8] = {};
  uint16_t dreqSrcH_ = 0, dreqSrcL_ = 0, dreqDstH_ = 0, dreqDstL_ = 0, dreqLen_ = 0;
  uint16_t fifo_[8] = {};
  int fifoN_ = 0;
  uint8_t fifoLatch_ = 0;
  uint16_t tv_ = 0;
  uint8_t hintVec_[4] = {};         // vetor da HINT do 68000 (endereço 70, gravável)

  // VDP
  uint8_t vmode_ = 0;               // bits 0-1 modo, 6 = 240 linhas, 7 = PRI
  uint8_t shift_ = 0, fillLen_ = 0;
  uint16_t fillAddr_ = 0, fillData_ = 0;
  uint8_t fs_ = 0, fsPending_ = 0;
  bool vblank_ = true;
  int64_t fillEnd_ = 0;             // hora (SH-2) em que o preenchimento termina
  uint16_t pal_[256] = {};
  int64_t lineStart_ = 0;           // começo da linha atual (SH-2)

  // PWM
  uint16_t pwmCtl_ = 0, pwmCycle_ = 0;
  uint16_t pwmFifo_[2][3] = {};
  int pwmN_[2] = {};
  int pwmTimer_ = 16;
  int64_t pwmNext_ = INT64_MAX;
  int pwmPeriod_ = 0;
  float pwmOut_[2] = {};
  float held_[2] = {};              // último valor do PWM já entregue ao som
  uint8_t pwmLatch_ = 0;
  struct PwmSample { int64_t t; float l, r; };
  std::vector<PwmSample> pwmLog_;
  int64_t audioT0_ = 0;             // hora (SH-2) do começo do som ainda não gerado
  int rate_ = 48000;
  static constexpr size_t OUTBUF = 16384;
  std::vector<float> outL_ = std::vector<float>(OUTBUF), outR_ = std::vector<float>(OUTBUF);
  size_t outW_ = 0, outR0_ = 0;

  int64_t now_ = 0;                 // até onde os SH-2 foram (ciclos do SH-2)
  struct Poll { uint32_t pc[4] = {}, addr[4] = {}; uint16_t val[4] = {}; int next = 0, cnt = 0; bool on = false; int64_t wake = INT64_MAX; };
  Poll poll_[2];                    // detecção de laço de espera de cada SH-2
  uint8_t idx_[320] = {};           // índices de cor da linha do Mega Drive
};

class Md32xCore : public Core {
public:
  Md32xCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  md::MD& md() { return *md_; }
  Mars& mars() { return *mars_; }

  std::string system() const override { return "32x"; }
  std::string title() const override { return md_->title; }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override { md_->setKeys(mask); }
  void setKeys2(uint32_t mask) override { md_->setKeys2(mask); }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return md_->vdp.fb; }
  int width() const override { return md_->vdp.outW; }
  int height() const override { return md_->vdp.outH; }
  double fps() const override { return md_->fps(); }
  void setSampleRate(int rate) override { md_->setSampleRate(rate); mars_->setSampleRate(rate); }
  size_t audioAvailable() const override;
  void pullAudio(float* l, float* r, size_t n) override;
  bool hasSave() const override { return md_->hasSave(); }
  bool saveDirty() const override { return md_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return md_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { md_->importSave(b); }
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& s) override;
  std::string note() const override;

private:
  std::unique_ptr<md::MD> md_;
  std::unique_ptr<Mars> mars_;
};

}  // namespace indigo::md32x
