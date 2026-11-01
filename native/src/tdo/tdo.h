// Panasonic 3DO (Interactive Multiplayer): CPU ARM60 (arm60.h), MADAM (madam.h: motor de cels, matrizes, VDL),
// CLIO (aqui: timers, interrupções, DMA, XBUS com a unidade de CD de cdrive.h, DSP de dspp.h, controle) e a
// NVRAM de 32 KB (o save). Núcleo escrito do zero a partir da documentação pública do hardware; nenhum código de
// outro emulador foi lido nem traduzido.
//
// Mapa de memória da CPU:
//   0x00000000-0x001FFFFF DRAM (2 MB)          0x00200000-0x002FFFFF VRAM (1 MB)
//   0x03000000-0x030FFFFF BIOS (ROM, 1 MB)     0x03140000-0x0315FFFF NVRAM (32 KB, um byte por palavra)
//   0x03200000-0x0327FFFF SPORT: escrever v em 0x03200000+off preenche a página de 2 KB da VRAM em off com v
//   0x03280000-0x032FFFFF SPORT: escrever v em 0x03280000+off copia a página da VRAM v para off
//   0x03300000 MADAM (madam.h)                 0x03400000 CLIO (abaixo)
// A partida começa na BIOS (0x03000000), no modo supervisor, com IRQ e FIQ desligadas.
//
// CLIO (endereço - 0x03400000), interpretação deste núcleo:
//   0x0000 revisão  0x0008/0x000C linhas das interrupções de vídeo VINT0/VINT1  0x0024 número aleatório
//   0x0030 contador horizontal  0x0034 linha atual (bits 0-10) e campo (bit 11)
//   0x0040/0x0044 liga/limpa interrupções pendentes (palavra 0; bit 31 = há pendência na palavra 1)
//   0x0048/0x004C liga/limpa a máscara 0   0x0060..0x006C o mesmo para a palavra 1
//     palavra 0: bit 0 VINT0, 1 VINT1, 2 XBUS, 3..10 timers 15,13,11,9,7,5,3,1, 11 DSP, 16..28 fim do DMA do
//     DSP (canais 0..12), 29 fim do DMA do XBUS.  palavra 1: bit 0 fim da leitura do controle (PBUS)
//   Todas as interrupções do CLIO chegam à CPU pela FIQ.
//   0x0100+8n contador do timer n (16 bits), 0x0104+8n recarga
//   0x0200/0x0204 liga/limpa o controle dos timers 0-7 (4 bits cada: 1 conta, 2 recarrega, 4 encadeia no
//     anterior); 0x0208/0x020C timers 8-15; 0x0220 SLACK: um passo dos timers a cada 64 + SLACK ciclos
//   0x0304/0x0308 liga/limpa DMA: bits 0..12 canais do DSP, bit 20 XBUS -> RAM, bit 31 leitura do controle
//   0x0500 dispositivo do XBUS escolhido (0 = CD)  0x0540 estado: bit 4 resposta pronta, bit 5 dado pronto,
//     bit 6 aceita comando; bits 0/1 (escrita) ligam a interrupção da resposta/do dado
//   0x0580 comando (escrita, byte a byte) / resposta (leitura)   0x05C0 dado (leitura, byte a byte)
//   0x17D0 semáforo  0x17E8 DSP rodando (bit 0)  0x17F0 reinicia o DSP
//   0x1800-0x1FFF programa do DSP (uma instrução de 16 bits por palavra)
//   0x3000-0x33FC EI do DSP (endereços 0x000-0x0FF)  0x3800-0x383C EO do DSP (endereços 0x3E0-0x3EF)
//
// Controle (PBUS): dois bytes por controle; byte 0 = 1 0 0 baixo cima direita esquerda A, byte 1 = B C P X R L 0 0.
// Teclas do Índigo: Y = A, A = B, B = C (a fileira esquerda/baixo/direita do controle), L/R, Start = P (play),
// Select = X (stop).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"
#include "arm60.h"
#include "cdrive.h"
#include "dspp.h"
#include "madam.h"

namespace indigo::tdo {

// BIOS do 3DO (arquivo do usuário, ex.: panafz10.bin, panafz1.bin, goldstar.bin): 1 MB, começa com uma
// instrução do ARM de condição "sempre" e tem "3DO" (ou "Opera") no texto. Nunca vem com o Índigo.
bool isTdoBios(const std::vector<uint8_t>& bytes);
// disco do 3DO (sistema de arquivos Opera): rótulo do volume no setor 0
bool isOperaDisc(cdrom::Disc& d);

class System : public ArmBus {
public:
  static constexpr double CPU_HZ = 12500000.0;
  static constexpr int LINES = 263, VISIBLE = 240;
  static constexpr int CYCLES_PER_LINE = 793;  // 12,5 MHz / 59,94 / 263
  static constexpr int AUDIO_HZ = 44100;

  System(const std::vector<uint8_t>& bios, cdrom::Disc* disc);

  std::vector<uint8_t> mem;  // DRAM + VRAM
  std::vector<uint8_t> bios;
  std::vector<uint8_t> nvram;
  Arm60 cpu{*this};
  Madam madam{mem};
  Dspp dsp;
  CdDrive cd;
  uint32_t fb[320 * 240] = {};
  std::vector<int16_t> audio;  // pares esquerdo/direito a 44,1 kHz
  bool nvramDirty = false;
  uint64_t frames = 0;

  void reset();
  void runFrame();
  void setPad(uint16_t bits) { pad_ = bits; }
  void sync(StateIO& io);

  // ArmBus
  uint32_t read32(uint32_t a) override;
  uint8_t read8(uint32_t a) override;
  void write32(uint32_t a, uint32_t v) override;
  void write8(uint32_t a, uint8_t v) override;

  // CLIO (exposto para os testes)
  uint32_t clioRead(uint32_t off);
  void clioWrite(uint32_t off, uint32_t v);
  void raise0(uint32_t bits) { pend0_ |= bits; updateFiq(); }
  void raise1(uint32_t bits) { pend1_ |= bits; updateFiq(); }
  uint32_t pending0() const { return pend0_; }
  uint16_t timer(int n) const { return timers_[n & 15]; }
  int line() const { return line_; }
  // avança o hardware (timers, CD, DMA, DSP) sem a CPU, para os testes
  void tickHardware(int cycles);

private:
  void updateFiq();
  void runLine();
  void timersTick(int cycles);
  void xbusDma();
  uint16_t dspFifo(int ch);
  void pbusDma();

  uint32_t pend0_ = 0, pend1_ = 0, mask0_ = 0, mask1_ = 0;
  uint32_t vint0_ = 0, vint1_ = 0;
  uint16_t timers_[16] = {}, reload_[16] = {};
  uint32_t timerCtl_[2] = {};
  uint32_t slack_ = 0, timerFrac_ = 0;
  uint32_t dmaEnable_ = 0;
  uint32_t xbusSel_ = 0, xbusPollEn_ = 0, expctl_ = 0;
  uint32_t semaphore_ = 0, seed_ = 0x13579BDF;
  uint32_t clioOther_[64] = {};  // registradores do CLIO sem efeito (guardados para leitura)
  int line_ = 0, field_ = 0;
  int32_t budget_ = 0;
  int32_t stall_ = 0;
  uint64_t audioAcc_ = 0;
  uint16_t pad_ = 0;
};

class TdoCore : public Core {
public:
  // disco (.cue/.bin, .chd, .iso); bios nulo = firmware("3do"). Lança std::runtime_error sem BIOS.
  TdoCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios = nullptr);

  std::string system() const override { return "3do"; }
  std::string title() const override { return title_; }
  void runFrame() override { sys_->runFrame(); }
  void reset() override { sys_->reset(); }
  void setKeys(uint32_t mask) override;
  const uint32_t* framebuffer() const override { return sys_->fb; }
  int width() const override { return 320; }
  int height() const override { return 240; }
  double fps() const override { return System::CPU_HZ / (double(System::CYCLES_PER_LINE) * System::LINES); }
  void setSampleRate(int rate) override { rate_ = rate > 0 ? rate : 48000; }
  size_t audioAvailable() const override;
  void pullAudio(float* left, float* right, size_t n) override;
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return sys_->nvramDirty; }
  std::vector<uint8_t> exportSave() override { sys_->nvramDirty = false; return sys_->nvram; }
  void importSave(const std::vector<uint8_t>& bytes) override;
  std::string note() const override { return note_; }
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& state) override;

  System& sys() { return *sys_; }
  cdrom::Disc* disc() { return disc_.get(); }

private:
  std::unique_ptr<cdrom::Disc> disc_;
  std::unique_ptr<System> sys_;
  std::string title_, note_;
  int rate_ = 48000;
  double audioPos_ = 0;
};

// abre a imagem e cria o núcleo com a BIOS de setFirmware("3do"); nullptr com a mensagem em err
std::unique_ptr<TdoCore> createTdoCore(const std::string& discPath, const std::vector<uint8_t>* save, std::string* err = nullptr);

}  // namespace indigo::tdo
