// Famicom Disk System (Nintendo, 1986): o adaptador RAM ligado no Famicom. É uma extensão do núcleo do NES
// (src/nes): o adaptador entra como um mapeador próprio, com
//   - 32 KB de RAM de programa ($6000–$DFFF), 8 KB de RAM de padrões (CHR) e a BIOS em $E000–$FFFF;
//   - a unidade de disco ($4024–$4033): leitura e gravação byte a byte no ritmo do drive (~96 kbit/s), com os
//     intervalos entre os blocos, a marca de início ($80) e o CRC de cada bloco, como o disco de verdade;
//   - o temporizador de IRQ ($4020–$4022);
//   - o som de tabela de onda (64 amostras de 6 bits) com a unidade de modulação ($4040–$408A).
// Discos: .fds (com ou sem o cabeçalho fwNES de 16 bytes; cada lado tem 65500 bytes, sem os intervalos nem o
// CRC) e .qd (cada lado com 64 KB, já com o CRC de cada bloco). A BIOS (disksys.rom, 8 KB) é do usuário.
// Save: o que o jogo grava no disco fica num save separado (só as diferenças em relação ao disco original);
// o arquivo do disco nunca é alterado.
// A troca de lado/disco é uma ação do app (setDiskSide): o disco sai do drive e o novo lado entra um instante
// depois, como na mão do jogador.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../nes/nes.h"

namespace indigo::fds {

// imagem de disco do FDS: .fds (com ou sem cabeçalho fwNES) ou .qd; nomeMinusculo decide o formato
bool isFdsDisk(const std::vector<uint8_t>& data, const std::string& nomeMinusculo);
// BIOS do FDS (disksys.rom, 8 KB): vetores de NMI/RESET/IRQ dentro de $E000–$FFFF e o texto "*NINTENDO-HVC*",
// que a BIOS compara com o cabeçalho de cada disco; ou 8 KB com "disksys" no nome
bool isFdsBios(const std::vector<uint8_t>& data, const std::string& nomeMinusculo);

// lados do disco no formato do drive (intervalos, marca $80, blocos e CRC); false com a mensagem se não der
bool rawSides(const std::vector<uint8_t>& file, bool qd, std::vector<std::vector<uint8_t>>& sides, std::string& erro);
// CRC dos blocos (polinômio 0x8408, como o chip do adaptador; a marca $80 entra no cálculo)
uint16_t blockCrc(const uint8_t* bloco, size_t n);

class FdsMapper;

class FdsCore : public Core {
public:
  // nome = nome do arquivo (".qd" escolhe o formato); bios vazia = usa firmware("fds") (lança erro se faltar)
  FdsCore(const std::vector<uint8_t>& disco, const std::string& nome, const std::vector<uint8_t>* save,
          const std::vector<uint8_t>* bios = nullptr);
  ~FdsCore() override;
  nes::NES& nes() { return *m_; }
  FdsMapper& adapter() { return *map_; }

  std::string system() const override { return "fds"; }
  std::string title() const override { return title_; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(false); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->ppu.fb; }
  int width() const override { return 256; }
  int height() const override { return 240; }
  double fps() const override { return 236250000.0 / 11.0 / 4.0 / (341.0 * 262 - 0.5); }
  void setSampleRate(int rate) override { m_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->apu.pull(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override;
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& b) override;
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& s) override;
  std::string note() const override;
  int diskSides() const override;
  int diskSide() const override;
  void setDiskSide(int lado) override;

private:
  std::unique_ptr<nes::NES> m_;
  FdsMapper* map_ = nullptr;
  std::string title_;
};

// o adaptador RAM (mapeador do núcleo do NES)
class FdsMapper : public nes::Mapper {
public:
  FdsMapper(nes::Cart& c, std::vector<std::vector<uint8_t>> lados, bool qd);

  void reset() override;
  void update() override;
  void writeReg(uint16_t a, uint8_t v) override;
  uint8_t cpuRead(uint16_t a, uint8_t openBus) override;
  uint8_t cpuPeek(uint16_t a) override;
  void cpuCycle() override;
  float audio() override { return audioOut_; }
  void syncRegs(StateIO& io) override;

  // disco no drive: -1 = nenhum. trocar() ejeta e põe o lado pedido depois de ~0,6 s (lado -1 = só ejeta)
  int sides() const { return static_cast<int>(sides_.size()); }
  int side() const { return disk_ >= 0 ? disk_ : -1; }
  int pendingSide() const { return insertDelay_ > 0 ? pending_ : -1; }
  void trocar(int lado);
  void inserirJa(int lado) { if (lado >= 0 && lado < sides()) { disk_ = lado; pending_ = -1; insertDelay_ = 0; } }  // testes
  // save: diferenças dos lados em relação ao disco original
  std::vector<uint8_t> diff() const;
  bool applyDiff(const std::vector<uint8_t>& d);
  uint32_t origHash() const;  // assinatura do disco original (confere o save)
  bool dirty = false;
  // para os testes: posição da cabeça, motor e os lados (formato do drive)
  const std::vector<std::vector<uint8_t>>& rawSides() const { return sides_; }
  bool motorOn() const { return motor_; }
  uint32_t headPos() const { return pos_; }
  // som (para os testes): nível atual de 0 a 63 antes do filtro
  int waveLevel() const { return level_; }
  int modCounter() const { return modCounter_; }
  int wavePos() const { return wavePos_; }

private:
  void setIrq();
  void clockDisk();
  void clockAudio();
  void crc(uint8_t v);
  int modOutput() const;
  void updateLevel();
  uint8_t audioRead(uint16_t a, uint8_t openBus);
  void audioWrite(uint16_t a, uint8_t v);

  std::vector<std::vector<uint8_t>> sides_, orig_;
  bool qd_ = false;
  // temporizador de IRQ
  uint16_t irqReload_ = 0, irqCounter_ = 0;
  bool irqOn_ = false, irqRepeat_ = false, timerIrq_ = false, diskIrq_ = false;
  bool diskRegs_ = true, soundRegs_ = true;
  // drive
  int disk_ = 0, pending_ = -1;
  int32_t insertDelay_ = 0;
  uint32_t pos_ = 0, delay_ = 0;
  uint16_t crc_ = 0;
  uint8_t writeData_ = 0, readData_ = 0, ctrl_ = 0, ext_ = 0;
  bool motor_ = false, resetXfer_ = false, readMode_ = false, crcCtl_ = false, ready_ = false, diskIrqOn_ = false;
  bool endOfHead_ = true, scanning_ = false, gapEnded_ = false, prevCrc_ = false, transfer_ = false, badCrc_ = false;
  // som
  struct Env {
    uint8_t speed = 0, gain = 0;
    bool off = false, up = false;
    uint16_t freq = 0;
    uint32_t timer = 0;
  };
  bool tickEnv(Env& e);
  void writeEnv(Env& e, uint8_t v);
  Env vol_, mod_;
  uint8_t master_ = 0xE8;  // multiplicador das envoltórias ($408A); a BIOS põe $E8
  uint8_t wave_[64] = {}, modTable_[64] = {};
  uint8_t wavePos_ = 0, modPos_ = 0, masterVol_ = 0;
  int8_t modCounter_ = 0;
  uint16_t waveAcc_ = 0, modAcc_ = 0;
  int32_t modOut_ = 0;
  bool waveWrite_ = false, envHalt_ = false, waveHalt_ = false, modHalt_ = true;
  int level_ = 0;
  float audioOut_ = 0, lowpass_ = 0;
};

}  // namespace indigo::fds
