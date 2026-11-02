// Núcleo Intellivision (Mattel, 1979): CPU GI CP1610 (cp1610.h), vídeo STIC (stic.h), som AY-3-8914 (psg.h),
// 240 bytes de RAM de 8 bits, 352 palavras de RAM de 16 bits (com o BACKTAB) e o cartucho.
//
// BIOS: a EXEC (exec.bin, 8 KB) e a GROM (grom.bin, 2 KB) são do console do usuário (setFirmware("intv_exec")
// e setFirmware("intv_grom")). Sem elas, cartuchos só de 10 bits (os jogos da época, que usam as rotinas da EXEC)
// são recusados com uma mensagem pedindo os dois arquivos; os de 16 bits (jogos caseiros) rodam com a EXEC e a
// GROM livres do Índigo (freebios.cpp), e note() avisa.
//
// Cartuchos: .rom (formato do Intellicart/jzIntv, com o mapa de memória dentro), .bin/.int/.itv (palavras de
// 16 bits big-endian no mapa padrão: $5000-$6FFF, $D000-$DFFF, $F000-$FFFF). O .cfg do jzIntv não é lido.
//
// Tempo: quadro NTSC de 14934 ciclos de 894,886 kHz, com as fases do STIC (retraço, janela de acesso aos
// registros e à GRAM, pedidos de barramento a cada fileira de cartões) seguindo o jzIntv.
//
// Controles: direcional = disco (8 direções); A = botão de baixo à direita, B = de baixo à esquerda,
// X = botões de cima; START = Enter, SELECT = Clear, L = tecla 1, R = tecla 2, Y = tecla 0 do teclado.
// setKeypad() aperta qualquer tecla do teclado numérico (0-9, 10 = Clear, 11 = Enter).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "cp1610.h"
#include "psg.h"
#include "stic.h"

namespace indigo::intv {

bool isIntvRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// 1 = EXEC (exec.bin), 2 = GROM (grom.bin), 0 = outro arquivo
int intvBiosKind(const std::vector<uint8_t>& data, const std::string& nomeMinusculo);
std::string intvTitle(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);  // do cabeçalho em $5000
std::vector<uint16_t> freeExec();  // EXEC livre (4096 palavras)
std::vector<uint8_t> freeGrom();   // GROM livre (2048 bytes)

// cartucho: imagem de 64K palavras e atributos por página de 256 palavras
struct Cart {
  std::vector<uint16_t> mem = std::vector<uint16_t>(65536, 0xFFFF);
  uint8_t page[256] = {};  // bit 0 = leitura, 1 = escrita (RAM), 2 = 8 bits
  std::string format;
};
// lê .rom ou .bin; devolve false com a explicação em err
bool loadCart(const std::vector<uint8_t>& data, const std::string& nomeMinusculo, Cart& cart, std::string& err);

class Intv : public Cp1610Bus {
public:
  static constexpr int FRAME_CYCLES = 14934;
  static constexpr double CPU_HZ = 3579545.0 / 4.0;

  Intv(const Cart& cart, const std::vector<uint16_t>& exec, const std::vector<uint8_t>& grom);
  void reset();
  void runFrame();  // até o próximo começo de retraço (quando a imagem fica pronta)

  uint16_t read(uint16_t a) override;
  void write(uint16_t a, uint16_t v) override;

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  CP1610 cpu{*this};
  Stic stic;
  PSG psg;
  uint8_t padL = 0, padR = 0;  // bits do controle esquerdo ($1FF) e direito ($1FE), 1 = apertado
  int64_t now = 0;
  uint64_t frames = 0;
  bool displayOn = false;      // a tela esteve ligada no último quadro
  uint8_t scratch[240] = {};
  uint16_t sysram[352] = {};

private:
  void phaseEvent();
  bool sticOpen(int64_t t) const { return t <= sticAcc_ && t >= intak_; }
  bool gmemOpen(int64_t t) const { return t <= gmemAcc_ && t >= intak_; }
  uint16_t sticRead(uint16_t a);
  void sticWrite(uint16_t a, uint16_t v);
  void sync(StateIO& io);

  Cart cart_;
  std::vector<uint16_t> exec_;
  uint16_t btab_[240] = {};  // cópia do BACKTAB que o STIC buscou nesta passada
  // fases do STIC
  int phase_ = 0;
  int64_t nextPhase_ = 57;
  bool vidEnable_ = false, frameReady_ = false;
  int cpRow_ = 0;
  int64_t sticAcc_ = 0, gmemAcc_ = 0, intak_ = 0;
  bool intrq_ = false, busrq_ = false;
  int64_t intrqUntil_ = 0, busrqUntil_ = 0;
};

class IntvCore : public Core {
public:
  IntvCore(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
  Intv& machine() { return *m_; }

  void setKeypad(int key) { keypad_ = key; applyKeys(); }  // -1 = nenhuma
  void setSwapControllers(bool s) { swap_ = s; applyKeys(); }

  std::string system() const override { return "intv"; }
  std::string title() const override { return title_; }
  void runFrame() override;
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { keys_ = mask; applyKeys(); }
  const uint32_t* framebuffer() const override { return fb_.data(); }
  int width() const override { return Stic::OUT_W * 2; }
  int height() const override { return Stic::OUT_H; }
  double fps() const override { return Intv::CPU_HZ / Intv::FRAME_CYCLES; }
  void setSampleRate(int rate) override { m_->psg.setSampleRate(rate, Intv::CPU_HZ); }
  size_t audioAvailable() const override { return m_->psg.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->psg.pull(l, r, n); }
  bool hasSave() const override { return false; }
  bool saveDirty() const override { return false; }
  std::vector<uint8_t> exportSave() override { return {}; }
  void importSave(const std::vector<uint8_t>&) override {}
  std::string note() const override { return note_; }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); present(); }

  static uint8_t padBits(uint32_t keys, int keypad);

private:
  void applyKeys();
  void present();

  std::unique_ptr<Intv> m_;
  std::vector<uint32_t> fb_;
  std::string title_, note_;
  uint32_t keys_ = 0;
  int keypad_ = -1;
  bool swap_ = false;
};

}  // namespace indigo::intv
