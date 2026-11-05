// Panasonic 3DO (Interactive Multiplayer): CPU ARM60 (arm60.h), MADAM (madam.h: motor de cels, matrizes, VDL),
// CLIO (aqui: timers, interrupções, DMA, XBUS com a unidade de CD de cdrive.h, DSP de dspp.h, controle) e a
// NVRAM de 32 KB (o save). Núcleo escrito a partir da documentação pública do hardware; CLIO, MADAM, DSPP e a unidade
// de CD foram reescritos depois lendo o código do MAME (BSD-3-Clause), como dito em TERCEIROS.md e em cada arquivo.
//
// Mapa de memória da CPU:
//   0x00000000-0x00FFFFFF DRAM e VRAM, na ordem que o MADAM recebe em MSYSBITS (0x03300004): conjunto 0 da DRAM,
//     conjunto 1 e a VRAM, um atrás do outro, cada um com o tamanho declarado (0, 1, 4 ou 16 MB por conjunto) e não
//     com o que existe; cada conjunto tem só 1 MB montado (repete) e só o primeiro MB da VRAM existe. A BIOS testa
//     do maior para o menor e fica no primeiro em que cada MB guarda o seu dado; no 3DO de 2 MB + 1 MB isso é 0x29
//     (DRAM em 0-2 MB, VRAM em 2-3 MB). Na partida (MSYSBITS = 0x51) a BIOS aparece por cima de 0-2 MB; a primeira
//     escrita nessa faixa tira a BIOS de lá (e não grava nada).
//   0x03000000-0x030FFFFF BIOS (ROM, 1 MB; com o bit 2 do ADBIO ligado, o banco de kanji, aqui vazio = 0xFF)
//   0x03140000-0x0315FFFF NVRAM (32 KB, um byte por palavra; espelho em 0x03160000)
//   0x03180000 porta serial de diagnóstico (lê 0, 0, 1, 0...: sem o modo de diagnóstico)
//   0x03200000-0x0320FFFF SPORT da VRAM: página de 2 KB nos bits 2-10; 0x0000 ler = copia a página para o
//     registrador de transferência, escrever = a página recebe o registrador nos bits da máscara escrita;
//     0x2000 cor; 0x4000 escrever = preenche a página com a cor nos bits da máscara
//   0x03300000 MADAM (madam.h)                 0x03400000 CLIO (abaixo)
// A partida começa no endereço 0 (a BIOS por cima da DRAM), no modo supervisor, com IRQ e FIQ desligadas.
// O motor de cels, a VDL e os DMA veem a memória já arrumada como 2 MB de DRAM + 1 MB de VRAM.
//
// CLIO (endereço - 0x03400000), interpretação deste núcleo (conferida com a documentação pública do hardware e
// com o driver do 3DO no MAME, licença BSD-3-Clause):
//   0x0000 revisão (0x02020000)  0x0004 CSYSBITS  0x0008/0x000C linhas das interrupções de vídeo VINT0/VINT1
//     (0xFFFFFFFF = desligada)  0x0020 AUDIN  0x0024 AUDOUT  0x0028 CSTATBITS (bit 0 = ligou agora, bit 6 =
//     reinício pelo DIPIR; escrever o bit 4 reinicia a CPU sem apagar os registradores, ver Arm60::requestReset; o bit 5 apaga o
//     pedido do DIPIR)  0x002C watchdog
//   0x0030 contador horizontal  0x0034 linha atual (bits 0-10) e campo (bit 11)  0x0038 semente  0x003C aleatório
//   0x0040/0x0044 liga/limpa interrupções pendentes (palavra 0)  0x0048/0x004C liga/limpa a máscara 0 (o bit 31
//     da máscara é sempre lido ligado)  0x0050/0x0054 liga/limpa MODE (bits 16-28: interrupção quando o canal de
//     DMA do DSP passa para o próximo bloco)  0x0058 BADBITS  0x0060..0x006C o mesmo para a palavra 1
//     palavra 0: bit 0 VINT0, 1 VINT1, 2 XBUS, 3..10 timers 15,13,11,9,7,5,3,1, 11 DSP, 12..15 DMA DSP -> RAM,
//     16..28 DMA RAM -> DSP (canais 0..12), 29 fim do DMA do XBUS, 31 há pendência ligada na palavra 1.
//     palavra 1: bit 0 fim do DMA do controle (PBUS), bit 1 DIPIR (troca de disco)
//   Todas as interrupções do CLIO chegam à CPU pela FIQ.
//   0x0080 HDELAY  0x0084 ADBIO (bits 4-7 escolhem as saídas, bits 0-3 os níveis; saída 2 = banco da ROM)
//   0x0088 ADBCTL
//   0x0100+8n contador do timer n (16 bits), 0x0104+8n recarga
//   0x0200/0x0204 liga/limpa o controle dos timers 0-7 (4 bits cada: 1 conta, 2 recarrega, 4 encadeia no
//     anterior); 0x0208/0x020C timers 8-15; 0x0220 SLACK: um passo dos timers a cada 64 + SLACK ciclos do relógio
//     de 25 MHz (o dobro do da CPU). Timer sem recarga para ao estourar (fica 0xFFFF).
//   0x0304/0x0308 liga/limpa DMA: bits 0..12 canais RAM -> DSP, bit 20 XBUS -> RAM (só anda com o bit 11 do
//     EXPCTL)
//   0x0400/0x0404 liga/limpa EXPCTL (bits 7, 9, 11, 14, 15; bit 7 = a CPU tem o barramento: volta a 1 sozinho
//     quando não há DMA do XBUS pedido)  0x0408 TYPE0_4
//   0x040C XFRCNT  0x0410/0x0414 DIPIR1/DIPIR2 (bit 15 = pedido ativo, bits 0-7 = dispositivo)
//   0x0500 dispositivo do XBUS escolhido (0 = CD; só 0 e 0x8F respondem: nos outros a CPU leva um "data abort"
//     e o bit 2 do ABORTBITS do MADAM liga)
//   0x0540 estado: bit 4 resposta pronta, bit 5 dado pronto, bit 6 aceita comando, bit 7 troca de disco (apaga ao
//     escrever com o bit 7); bits 0-2 (escrita) ligam a interrupção da resposta/do dado/da escrita
//   0x0580 comando (escrita, byte a byte) / resposta (leitura)   0x05C0 dado (leitura, byte a byte)
//   0x0300 esvazia os FIFOs do DSP (máscara de canais)  0x0380-0x03FC quantas amostras em cada FIFO
//   0x17D0 semáforo (escrita: dado da CPU; leitura: estado << 16 | dado)  0x17D4 confirma o semáforo
//   0x17E4/0x17E8 recomeça a amostra do DSP  0x17F0 ruído  0x17F4 PC do DSP
//   0x17FC controle do DSP (bit 0 roda, bit 2 reinicia)
//   0x1800-0x1FFF programa do DSP (duas instruções por palavra)  0x2000-0x2FFF (uma por palavra)
//   0x3000-0x31FF EI do DSP (duas palavras por escrita)  0x3400-0x37FF EI (uma por escrita)
//   0x3800-0x39FF EO do DSP (0x300-0x3FF, duas por leitura)  0x3C00-0x3FFF (uma por leitura)
//   Os canais de DMA 0..12 e 16..19 do MADAM (0x400 + 16n) são os do DSP (dspp.h).
//   0xC000 UNCLE (expansão de vídeo): revisão 0x03800000, 0xC008 endereço
//
// Controle (PBUS): dois bytes por controle; byte 0 = 1 0 0 baixo cima direita esquerda A, byte 1 = B C P X R L 0 0.
// Teclas do Índigo: Y = A, A = B, B = C (a fileira esquerda/baixo/direita do controle), L/R, Start = P (play),
// Select = X (stop). O controle do jogador 2 vem logo depois do 1 na corrente.
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
  void setPad2(uint16_t bits) { pad2_ = bits; pad2On_ = true; }  // 2º controle da corrente (jogador 2)
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
  uint32_t pending1() const { return pend1_; }
  uint16_t timer(int n) const { return timers_[n & 15]; }
  int line() const { return line_; }
  bool overlay() const { return overlay_; }
  // refaz o mapa da DRAM/VRAM depois de uma escrita em MSYSBITS
  void memoryConfig(uint32_t msysbits);
  // endereço físico (em mem) de um endereço da CPU abaixo de 16 MB; -1 = nada responde
  int32_t phys(uint32_t a) const {
    const int8_t m = mbMap_[(a >> 20) & 15];
    return m < 0 ? -1 : int32_t(uint32_t(m) << 20 | (a & 0xFFFFF));
  }
  // avança o hardware (timers, CD, DMA, DSP) sem a CPU, para os testes
  void tickHardware(int cycles);

private:
  void updateFiq();
  void runLine();
  void timersTick(int cycles);
  void xbusDma();
  void expBus();
  // canal de DMA do DSP de um registrador do MADAM (0x400 + 16n): 0..12 e 16..19; -1 = outro
  static int dspDmaChannel(uint32_t off) {
    if (off < 0x400 || off >= 0x600) return -1;
    const int ch = int((off - 0x400) >> 4);
    return (ch <= 12 || (ch >= 16 && ch <= 19)) ? ch : -1;
  }
  void pbusDma();

  void mediaChange();
  void xbusTimeout();
  uint32_t svfRead(uint32_t off);
  void svfWrite(uint32_t off, uint32_t v);

  uint32_t pend0_ = 0, pend1_ = 0, mask0_ = 0, mask1_ = 0;
  uint32_t vint0_ = 0, vint1_ = 0;
  uint16_t timers_[16] = {}, reload_[16] = {};
  uint32_t timerCtl_[2] = {};
  uint32_t slack_ = 0, timerFrac_ = 0;  // timerFrac_ em ciclos de 25 MHz
  uint32_t dmaEnable_ = 0;
  uint32_t xbusSel_ = 0, xbusPollEn_ = 0, expctl_ = 0, xbusMedia_ = 0;
  uint32_t seed_ = 0x13579BDF;
  uint32_t csysbits_ = 0, audin_ = 0, audout_ = 0, cstat_ = 0, wdog_ = 0, mode_ = 0, badbits_ = 0;
  uint32_t hdelay_ = 0, adbio_ = 0, adbctl_ = 0, type04_ = 0, xfrcnt_ = 0, dipir1_ = 0, dipir2_ = 0;
  uint32_t clioOther_[64] = {};  // registradores do CLIO sem efeito (guardados para leitura)
  bool overlay_ = true;          // BIOS por cima da DRAM (até a primeira escrita)
  uint32_t sport_[512] = {};     // registrador de transferência da VRAM (uma página de 2 KB)
  uint32_t svfColor_ = 0, uncleAddr_ = 0, slowIn_ = 0, slowOut_ = 4;
  int8_t mbMap_[16] = {};        // MB da CPU -> MB de mem (0, 1 = DRAM; 2 = VRAM; -1 = nada)
  int line_ = 0, field_ = 0;
  int32_t budget_ = 0;
  int32_t stall_ = 0;
  uint64_t audioAcc_ = 0;
  uint16_t pad_ = 0;
  uint16_t pad2_ = 0;    // não vai no estado: o app manda a cada quadro
  bool pad2On_ = false;
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
  void setKeys2(uint32_t mask) override;
  int players() const override { return 2; }
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
