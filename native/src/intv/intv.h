// Núcleo Intellivision (Mattel, 1979): CPU GI CP1610 (cp1610.h), vídeo STIC (stic.h), som AY-3-8914 (psg.h),
// 240 bytes de RAM de 8 bits, 352 palavras de RAM de 16 bits (com o BACKTAB) e o cartucho.
//
// BIOS: a EXEC (exec.bin, 8 KB) e a GROM (grom.bin, 2 KB) são do console do usuário (setFirmware("intv_exec")
// e setFirmware("intv_grom")). Sem elas, cartuchos só de 10 bits (os jogos da época, que usam as rotinas da EXEC)
// são recusados com uma mensagem pedindo os dois arquivos; os de 16 bits (jogos caseiros) rodam com a EXEC e a
// GROM livres do Índigo (freebios.cpp), e note() avisa.
//
// Cartuchos: .rom (formato do Intellicart/jzIntv, com o mapa de memória dentro), .bin/.int/.itv (palavras de
// 16 bits big-endian). Com um .cfg do jzIntv ao lado do .bin (o app passa o texto em firmware("intv_cfg")),
// o mapa vem dele: [mapping] (com PAGE n das páginas do ECS), [preload], [memattr] (RAM/WOM de 8 ou 16 bits),
// [bankswitch] (troca de bancos do Intellicart, registros $40-$5F) e o nome em [vars]. Sem .cfg: mapa padrão
// $5000-$6FFF, $D000-$DFFF, $F000-$FFFF.
//
// Tempo: quadro NTSC de 14934 ciclos de 894,886 kHz, com as fases do STIC (retraço, janela de acesso aos
// registros e à GRAM, pedidos de barramento a cada fileira de cartões) seguindo o jzIntv. PAL (nome do arquivo
// com "(pal)", "(europe)" ou "(e)"): 1 MHz, 312 linhas de 64 ciclos (19968 por quadro, 50,08 Hz); a parte
// visível dura o mesmo número de linhas do NTSC e o resto vira retraço (aproximação: sem medida do aparelho).
//
// Controles: direcional = disco (8 direções); A = botão de baixo à direita, B = de baixo à esquerda,
// X = botões de cima; START = Enter, SELECT = Clear, L = tecla 1, R = tecla 2, Y = tecla 0 do teclado.
// setKeypad() aperta qualquer tecla do teclado numérico (0-9, 10 = Clear, 11 = Enter); os bits KEY_KP* de
// setKeys() também (teclado numérico do PC e da tela de toque: * = Clear, # = Enter).
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

// cartucho: imagem de 64K palavras (o espaço do Intellicart) e atributos por endereço do Intellivision
//   bit 0 = leitura, 1 = escrita (RAM), 2 = 8 bits (RAM estreita), 3 = troca de bancos do Intellicart
// Páginas no estilo do ECS (PAGE n no .cfg): cada segmento de 4K pode ter até 16 páginas; escrever $xA5y em
// $xFFF (x = segmento) mostra a página y daquele segmento.
struct Cart {
  std::vector<uint16_t> mem = std::vector<uint16_t>(65536, 0xFFFF);
  std::vector<uint8_t> attr = std::vector<uint8_t>(65536, 0);
  std::vector<uint16_t> pmem;   // páginas: [segmento][página][4096 palavras] (vazio se o cartucho não usa)
  std::vector<uint8_t> pattr;   // atributos das páginas (bits 0-2 como acima), mesmo índice de pmem
  uint16_t pagedSegs = 0;       // bit s = o segmento s tem páginas
  std::string format;
  std::string cfgTitle;         // name = "..." do .cfg
  std::string cfgWarn;          // aviso de linhas do .cfg ignoradas (vai para o note())

  static size_t pidx(int seg, int pg, int off) { return (size_t(seg) * 16 + size_t(pg)) * 4096 + size_t(off); }
  void usePages() { if (pmem.empty()) { pmem.assign(16 * 16 * 4096, 0xFFFF); pattr.assign(16 * 16 * 4096, 0); } }
};
// lê .rom ou .bin (com o .cfg do jzIntv, se veio); devolve false com a explicação em err
bool loadCart(const std::vector<uint8_t>& data, const std::string& nomeMinusculo, Cart& cart, std::string& err,
              const std::string& cfg = "");
// lê só o .cfg (mapa de um .bin): seções [mapping], [preload], [memattr], [bankswitch] e [vars]
bool parseCfg(const std::string& cfg, const std::vector<uint8_t>& bin, Cart& cart, std::string& err);
std::string cfgFor(const std::string& nomeMinusculo);  // texto de firmware("intv_cfg") para .bin/.int/.itv
bool isPalName(const std::string& nomeMinusculo);      // "(pal)" no nome (o cartucho não tem região)

class Intv : public Cp1610Bus {
public:
  static constexpr int FRAME_CYCLES = 14934;         // NTSC: 262 linhas de 57 ciclos
  static constexpr double CPU_HZ = 3579545.0 / 4.0;
  static constexpr int FRAME_CYCLES_PAL = 19968;     // PAL: 312 linhas de 64 ciclos
  static constexpr double CPU_HZ_PAL = 4000000.0 / 4.0;

  Intv(const Cart& cart, const std::vector<uint16_t>& exec, const std::vector<uint8_t>& grom, bool pal = false);
  bool pal() const { return pal_; }
  int frameCycles() const { return pal_ ? FRAME_CYCLES_PAL : FRAME_CYCLES; }
  double cpuHz() const { return pal_ ? CPU_HZ_PAL : CPU_HZ; }
  int page(int seg) const { return curPage_[seg & 15]; }  // página do ECS visível no segmento
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

  uint16_t cartRead(uint16_t a, bool* hit);
  std::vector<int> ramBlocks_;             // blocos de 256 palavras do cartucho que vão no save state
  std::vector<uint32_t> pageRamBlocks_;    // idem, nas páginas do ECS
  bool cartWrite(uint16_t a, uint16_t v);

  Cart cart_;
  std::vector<uint16_t> initMem_, initPmem_;  // imagem do cartucho ao ligar (o reset volta a ela)
  std::vector<uint16_t> exec_;
  bool pal_ = false;
  bool icartBanks_ = false;   // algum endereço usa a troca de bancos do Intellicart
  uint8_t bank_[32] = {};     // registros $40-$5F do Intellicart (um por bloco de 2K)
  uint8_t curPage_[16] = {};  // página do ECS de cada segmento de 4K
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
  double fps() const override { return m_->cpuHz() / m_->frameCycles(); }
  void setSampleRate(int rate) override { m_->psg.setSampleRate(rate, m_->cpuHz()); }
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
  static int keypadFromKeys(uint32_t keys);

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
