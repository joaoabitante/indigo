// Capcom CPS1 e CPS2 (CP System e CP System II), a partir dos pacotes .zip no formato do MAME.
//
// CPS1: 68000 a 10 ou 12 MHz, Z80 a 3,58 MHz com YM2151 (FM) e OKI MSM6295 (ADPCM), placas A/B/C com os
// chips CPS-A e CPS-B. CPS2: 68000 a 16 MHz (efetivos ~11,8 MHz por causa dos estados de espera, como no MAME),
// programa cifrado (chave .key do pacote), Z80 a 8 MHz com o QSound, EEPROM 93C46 para a configuração e os
// recordes (é o "save" do jogo), objetos próprios do CPS2 com prioridade e interrupção por linha (raster).
//
// Vídeo (CPS-A/CPS-B): 3 camadas de tiles (8x8, 16x16 e 32x32, mapas de 64x64) com rolagem, rolagem por linha
// na camada 2, objetos de 16x16 em blocos, paleta de 0xC00 cores de 12 bits + brilho, ordem das camadas e máscaras
// de prioridade das tintas pelo CPS-B (os endereços dos registradores mudam de placa para placa: tabela do MAME),
// faixas de tiles por placa (PALs), estrelas (Forgotten Worlds). Tela de 384x224 a 59,637 quadros/s
// (8 MHz / 512 / 262); jogos verticais saem já girados (224x384).
//
// Ainda não: CPS1.5 (QSound no CPS1: Warriors of Fate, Cadillacs and Dinosaurs, The Punisher, Slam Masters —
// o Z80 deles é cifrado com o Kabuki), Pang! 3 (programa cifrado), bootlegs, jogos com botões giratórios além
// do Forgotten Worlds, pacotes "split" de clones (que precisam do .zip do pai): use pacotes "merged" ou
// "non-merged".
//
// Teclas do Índigo (bits de Key, com os nomes nas posições do controle do SNES/Índigo: Y à esquerda, X em cima,
// B embaixo, A à direita). Layout clássico de Street Fighter (no controle de Xbox: X, Y, RB em cima; A, B, RT
// embaixo):
//   Y = soco fraco (botão 1)     X = soco médio (botão 2)    R = soco forte (botão 3)
//   B = chute fraco (botão 4)    A = chute médio (botão 5)   L = chute forte (botão 6)
// Jogos com até 4 botões: botão 1 = Y ou B, botão 2 = X ou A, botão 3 = R, botão 4 = L.
// Forgotten Worlds: L/R giram o personagem (o botão giratório do arcade).
// START = Start do jogador 1, SELECT = ficha (moeda 1). Direcional = alavanca do jogador 1.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../sms/z80.h"
#include "cps2crypt.h"
#include "eeprom.h"
#include "games.h"
#include "m68k.h"
#include "oki6295.h"
#include "qsound.h"
#include "ym2151.h"

namespace indigo::cps {

// pacote .zip de CPS1/CPS2 conhecido (pelo nome do pacote ou pelos CRCs dos arquivos de dentro)
bool isCpsSet(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo);

class Cps : public M68kBus, public Z80Bus {
public:
  Cps(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo, const std::vector<uint8_t>* save);
  ~Cps() override;

  const GameDef& game;
  bool cps2;
  M68k cpu;
  Z80 z80;

  void reset();
  void setKeys(uint32_t mask) { keys_ = mask; }
  void setKeys2(uint32_t mask) { keys2_ = mask; }
  void runFrame();
  double fps() const { return 8000000.0 / (512.0 * 262.0); }
  std::string title() const { return game.title; }
  const uint32_t* framebuffer() const { return out_.data(); }
  int width() const { return (game.rot & 1) ? 224 : 384; }
  int height() const { return (game.rot & 1) ? 384 : 224; }

  void setSampleRate(int rate);
  size_t audioAvailable() const { return (outW_ + OUTBUF - outRd_) % OUTBUF; }
  void pullAudio(float* l, float* r, size_t n);

  bool hasSave() const { return cps2; }
  bool saveDirty() const { return eeprom_.dirty; }
  std::vector<uint8_t> exportSave();
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // M68kBus
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  uint16_t fetch16(uint32_t a) override;
  int intAck(int level) override;
  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int) override { return 0xFF; }
  void out(int, int) override {}

  // para os testes
  const std::vector<uint8_t>& region(int r) const { return rg_[r]; }
  const std::vector<uint8_t>& decrypted() const { return dec_; }
  uint16_t cpsA(int i) const { return cpsA_[i & 0x1F]; }
  uint16_t cpsB(int i) const { return cpsB_[i & 0x1F]; }
  uint64_t frameCount() const { return frame_; }

private:
  void sync(StateIO& io);
  // memória
  uint16_t readWord(uint32_t a, uint16_t mask);
  void writeWord(uint32_t a, uint16_t v, uint16_t mask);
  uint16_t ioRead(uint32_t a);
  void ioWrite(uint32_t a, uint16_t v, uint16_t mask);
  uint16_t cpsbRead(int off);
  void cpsbWrite(int off, uint16_t v, uint16_t mask);
  void cpsaWrite(int off, uint16_t v, uint16_t mask);
  uint16_t gfxWord(uint32_t base, uint32_t idx) const { return gfxram_[(base / 2 + idx) & 0x1FFFF]; }
  uint32_t cpsBase(int reg, uint32_t boundary) const;
  uint16_t inputWord(int which) const;
  void updateIrq() { cpu.ipl = (irq2_ ? 2 : 0) | (irq4_ ? 4 : 0); }
  // vídeo
  void buildPalette();
  void colorOf(int i);
  int mapCode(int type, int code) const;
  void latchObjects();
  void drawSprites();
  void renderLine(int line);
  void finishFrame();
  // tempo
  void runCpu(int64_t pixelTarget);
  void runZ80(int64_t pixelTarget);
  void runSound(int64_t pixelTarget);
  int64_t z80Pix() const;           // momento atual do Z80 em pixels
  void pushSample(float l, float r, double chipRate);
  void renderUpTo(int line);

  const CpsbConfig& cfg_;
  std::vector<uint8_t> rg_[RG_COUNT];
  std::vector<uint8_t> dec_;  // programa decifrado (CPS2)
  uint32_t mainMask_ = 0;
  uint32_t cpuNum_ = 0;  // ciclos do 68000 por 8 MHz (pixel), em 1/1000

  // memórias
  std::vector<uint8_t> ram_, ram2_;   // RAM principal (64 KB); CPS2: RAM extra 0x660000 (16 KB)
  std::vector<uint16_t> gfxram_;      // 0x900000-0x92FFFF (com folga até 256 KB, endereços mascarados)
  std::vector<uint16_t> obj2_[2];     // CPS2: RAM de objetos (2 bancos de 4 K palavras)
  std::vector<uint8_t> zram_, zram2_; // Z80: CPS1 0xD000 (2 KB); CPS2 0xC000 (4 KB, compartilhada) e 0xF000
  uint16_t cpsA_[0x20] = {}, cpsB_[0x20] = {};
  uint16_t out2_[6] = {};             // CPS2: registradores de objetos (0x400000)
  uint16_t pal_[0xC00] = {};          // RAM da paleta (dentro do CPS-B)
  uint32_t color_[0xC00] = {};        // paleta convertida para 0xAABBGGRR
  std::vector<uint16_t> objBuf_;      // lista de objetos guardada no fim do quadro anterior
  int priCtrl_ = 0, objBank_ = 0;
  // E/S
  uint32_t keys_ = 0, keys2_ = 0;
  uint8_t latch_[2] = {}, sndBank_ = 0;
  int dial_ = 0;
  Eeprom93C46 eeprom_;
  bool z80Reset_ = false;
  // interrupções e linhas de raster (CPS2)
  bool irq2_ = false, irq4_ = false;
  uint16_t rasterCnt_[3] = {}, rasterReload_[3] = {};
  int64_t rasterAt_ = -1;  // momento (em pixels) da interrupção de raster pendente
  int curLine_ = 0;
  // relógios: tudo contado desde o começo; "pixel" = 1/8 MHz (512 por linha, 262 linhas por quadro)
  int64_t pix_ = 0;        // início da linha atual
  int64_t cpuCyc_ = 0;     // ciclos do 68000
  int64_t z80Cyc_ = 0;     // ciclos do Z80
  int64_t sndDone_ = 0;    // amostras geradas (YM2151 no CPS1, QSound no CPS2)
  int64_t okiNext_ = 0;    // momento (pixel) da próxima amostra do OKI
  int64_t z80IrqNext_ = 0; // CPS2: próxima interrupção periódica do Z80 (em ciclos do Z80)
  bool z80IrqHold_ = false;
  uint64_t frame_ = 0;
  int dialBase_ = 0;
  // som
  YM2151 ym_;
  Oki6295 oki_;
  QSound qs_;
  int okiOut_ = 0;
  static constexpr size_t OUTBUF = 16384;
  std::vector<float> bufL_ = std::vector<float>(OUTBUF), bufR_ = std::vector<float>(OUTBUF);
  size_t outW_ = 0, outRd_ = 0;
  double outRate_ = 48000, resFrac_ = 0;
  float accL_ = 0, accR_ = 0;
  // vídeo
  std::vector<uint32_t> out_;          // imagem final (já girada nos jogos verticais)
  std::vector<uint16_t> fbIdx_;        // índices de cor do quadro (convertidos no fim, com a paleta da hora)
  std::vector<uint16_t> spr_;          // objetos do quadro: cor | prioridade << 12 | 0x8000 (384x224)
  int lastLine_ = 15;                  // última linha já desenhada
};

class CpsCore : public Core {
public:
  CpsCore(const std::vector<uint8_t>& zip, const std::string& nome, const std::vector<uint8_t>* save)
      : m_(std::make_unique<Cps>(zip, nome, save)) {}
  Cps& cps() { return *m_; }

  std::string system() const override { return m_->cps2 ? "cps2" : "cps1"; }
  std::string title() const override { return m_->title(); }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  void setKeys2(uint32_t mask) override { m_->setKeys2(mask); }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return m_->framebuffer(); }
  int width() const override { return m_->width(); }
  int height() const override { return m_->height(); }
  double fps() const override { return m_->fps(); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return m_->hasSave(); }
  bool saveDirty() const override { return m_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }
  std::string note() const override;

private:
  std::unique_ptr<Cps> m_;
};

}  // namespace indigo::cps
