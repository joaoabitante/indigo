// Núcleo Nintendo 64 (1996): CPU NEC VR4300 (vr4300.h), RCP = RSP (rsp.h, LLE) + RDP (rdp.h, em software),
// 8 MB de RDRAM (com o Expansion Pak), interfaces MI/VI/AI/PI/SI/RI e o PIF (controle por joybus, Controller Pak,
// EEPROM do cartucho).
//
// Partida sem BIOS: a ROM do PIF (que não é do usuário e não está aqui) e o IPL3 do cartucho não rodam. O núcleo
// faz o que eles deixam pronto (HLE): copia os 4 KB iniciais do cartucho para a DMEM, copia 1 MB do cartucho
// (a partir de 0x1000) para a RDRAM no endereço de partida do cabeçalho (com o ajuste dos CIC 6103/6106),
// arruma os registros como o IPL deixa (semente do CIC achada pelo CRC do código de partida do cartucho, tipo de
// TV, tamanho da memória em 0x80000318/0x800003F0) e começa no cartucho. Por isso o N64 não pede BIOS.
//
// Cartuchos .z64 (big-endian), .v64 (bytes trocados de 2 em 2) e .n64 (palavras little-endian): a ordem é
// achada pelos 4 primeiros bytes do cabeçalho e convertida.
// Saves: EEPROM 4K/16K (joybus, canal 4), SRAM 32K/96K e FlashRAM 128K (domínio 2 do PI) e Controller Pak
// (32 KB). O tipo vem do cabeçalho "homebrew" (ID "ED", byte 0x3F) ou de uma tabela pelos 2 caracteres do código do
// jogo; sem informação: EEPROM 4K e SRAM/Flash conforme o jogo usar.
//
// Memória: palavras de 32 bits guardadas na ordem do PC (little-endian); byte = endereço ^ 3, meia palavra = ^ 2.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "rdp.h"
#include "rsp.h"
#include "vr4300.h"

namespace indigo::n64 {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "o núcleo do N64 supõe máquina little-endian");

bool isN64Rom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// converte para big-endian (.z64); vazio se não for cartucho de N64
std::vector<uint8_t> toZ64(const std::vector<uint8_t>& rom);

enum class SaveType : uint8_t { Nenhum, Eeprom4k, Eeprom16k, Sram32k, Sram96k, Flash, Auto };

// botões do controle do N64 (como na leitura do joybus)
enum Pad : uint16_t {
  P_CR = 1u << 0, P_CL = 1u << 1, P_CD = 1u << 2, P_CU = 1u << 3, P_R = 1u << 4, P_L = 1u << 5,
  P_DR = 1u << 8, P_DL = 1u << 9, P_DD = 1u << 10, P_DU = 1u << 11, P_START = 1u << 12, P_Z = 1u << 13,
  P_B = 1u << 14, P_A = 1u << 15,
};

class N64 {
public:
  static constexpr int64_t CPU_CLOCK = 62500000;  // tiques por segundo (vr4300.h: 1 tique = 1,5 ciclo da CPU)
  static constexpr uint32_t RDRAM_SIZE = 8u << 20;

  explicit N64(const std::vector<uint8_t>& z64);
  void reset();
  void runFrame();

  // memória física (endereços de 32 bits)
  uint32_t read32(uint32_t pa);
  void write32(uint32_t pa, uint32_t v, uint32_t mask = 0xFFFFFFFF);
  uint8_t read8(uint32_t pa) { return uint8_t(read32(pa & ~3u) >> (8 * (3 - (pa & 3)))); }
  uint16_t read16(uint32_t pa) { return uint16_t(read32(pa & ~3u) >> (8 * (2 - (pa & 2)))); }
  void write8(uint32_t pa, uint8_t v) {
    const int sh = 8 * (3 - (pa & 3));
    write32(pa & ~3u, uint32_t(v) << sh, 0xFFu << sh);
  }
  void write16(uint32_t pa, uint16_t v) {
    const int sh = 8 * (2 - (pa & 2));
    write32(pa & ~3u, uint32_t(v) << sh, 0xFFFFu << sh);
  }

  // interrupções do MI (bit 0 SP, 1 SI, 2 AI, 3 VI, 4 PI, 5 DP)
  void raise(int bit);
  void clear(int bit);

  // RSP: registros do SP (usados pelo COP0 do RSP e pela CPU)
  uint32_t spRead(int reg);
  void spWrite(int reg, uint32_t v);
  uint32_t dpRead(int reg);
  void dpWrite(int reg, uint32_t v);

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  Vr4300 cpu;
  Rsp rsp;
  Rdp rdp;
  std::vector<uint8_t> rdram;
  uint8_t spmem[0x2000];  // DMEM (0-0xFFF) e IMEM (0x1000-0x1FFF)
  std::vector<uint8_t> rom;  // palavras na ordem do PC
  uint32_t romSize = 0;
  std::string title, gameId;
  uint8_t cicSeed = 0x3F;
  int cic = 6102;

  // registros
  uint32_t sp[8] = {};    // MEM_ADDR, DRAM_ADDR, RD_LEN, WR_LEN, STATUS, DMA_FULL, DMA_BUSY, SEMAPHORE
  uint32_t dpc[8] = {};   // START, END, CURRENT, STATUS, CLOCK, BUFBUSY, PIPEBUSY, TMEM
  uint32_t miMode = 0, miIntr = 0, miMask = 0;
  uint32_t vi[14] = {};
  uint32_t ai[6] = {};
  uint32_t pi[13] = {};
  uint32_t ri[8] = {};
  uint32_t si[7] = {};
  uint8_t pifRam[64] = {};

  // controles (1 = apertado) e direcional analógico
  uint16_t pad[4] = {};
  int8_t stickX[4] = {}, stickY[4] = {};
  bool padOn[4] = {true, false, false, false};

  // saves
  SaveType saveType = SaveType::Auto;
  std::vector<uint8_t> eeprom, sram, flash, mempak;
  bool eepromUsed = false, sramUsed = false, flashUsed = false, mempakUsed = false;
  bool saveDirty = false;

  // vídeo
  std::vector<uint32_t> fb;
  int outW = 320, outH = 240;
  double frameRate() const { return pal ? 50.0 : 60.0; }
  bool pal = false;

  // som
  void setSampleRate(int r) { sampleRate_ = r > 0 ? r : 48000; }
  size_t audioAvailable() const { return (audW_ - audR_) / 2; }
  void pullAudio(float* l, float* r, size_t n);

  uint64_t frames = 0;
  int64_t now() const { return cpu.cycles; }

private:
  void sync(StateIO& io);
  void hleBoot();
  void detectSave();
  void spDma(bool toRdram);
  void piDma(bool toRdram);
  void siDma(bool toRdram);
  void pifRun();
  void joybus(int ch, uint8_t* tx, int txn, uint8_t* rx, int rxn, uint8_t& rxflag);
  uint32_t cartRead(uint32_t pa);
  void cartWrite(uint32_t pa, uint32_t v);
  uint32_t flashStatus();
  void flashCommand(uint32_t v);
  void events();
  void aiAdvance(int64_t ciclos);
  void aiPop();
  void renderVi();
  void runSlice(int64_t ate);

  int line_ = 0;
  int field_ = 0;
  int64_t piDoneAt_ = 0, siDoneAt_ = 0;  // 0 = nada pendente
  bool pifPending_ = false;
  // AI: FIFO de 2 buffers
  struct AiBuf { uint32_t addr = 0, len = 0; };
  AiBuf aiQ_[2];
  int aiN_ = 0;
  uint32_t aiPos_ = 0;    // bytes já tocados do buffer atual
  int64_t aiFrac_ = 0;    // fração de amostra (em ciclos * freq)
  int16_t aiL_ = 0, aiR_ = 0;
  int64_t outFrac_ = 0;
  // FlashRAM
  int flashMode_ = 0;     // 0 leitura de array, 1 estado, 2 apagar, 3 escrever
  uint32_t flashOffset_ = 0, flashEraseOffset_ = 0;
  uint64_t flashStatus_ = 0;
  uint8_t flashPage_[128] = {};
  // som
  std::vector<float> aud_;
  size_t audW_ = 0, audR_ = 0;
  int sampleRate_ = 48000;
  void audioPush(float l, float r);
};

class N64Core : public Core {
public:
  N64Core(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save);
  N64& sys() { return *m_; }

  std::string system() const override { return "n64"; }
  std::string title() const override { return m_->title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override;
  const uint32_t* framebuffer() const override { return m_->fb.data(); }
  int width() const override { return m_->outW; }
  int height() const override { return m_->outH; }
  double fps() const override { return m_->frameRate(); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return m_->saveDirty; }
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& b) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<N64> m_;
};

}  // namespace indigo::n64
