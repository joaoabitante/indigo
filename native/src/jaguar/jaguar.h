// Núcleo Atari Jaguar (1993): 68000 (o mesmo do Mega Drive, src/md/m68k.h) a 13,3 MHz, Tom (GPU RISC, Object
// Processor, Blitter, vídeo) e Jerry (DSP RISC, som I2S, temporizadores, controle) a 26,6 MHz, 2 MB de RAM.
//
// Cartuchos: .j64 / .rom / .jag (imagem do cartucho em 0x800000, com a marca 0x04040404 em 0x400 e o endereço de
// partida em 0x404) e programas para a RAM (.abs/.jag: formatos ABS 0x601B/0x0150 e "JAGR" do Jaguar Server).
// Partida: a ROM de partida do console não é necessária. O núcleo faz o que ela deixa pronto (HLE: vetores,
// MEMCON, vídeo NTSC, lista de objetos vazia, marca de autenticação do GPU) e começa direto no cartucho.
//
// O que existe: 68000; Object Processor (bitmap, bitmap com escala, objeto do GPU, desvio, parada; CLUT, CRY,
// RGB16, RGB24, transparência, reflexo, RMW); Blitter (por pixel, com modo phrase, LFU, padrão, comparação de
// dados e de bits, recorte, Gouraud, Z, sombra); GPU e DSP (as 64 instruções, bancos, interrupções, divisão,
// MAC, MMULT); temporizadores do Tom e do Jerry; DAC I2S (som estéreo de 16 bits); controle (porta 1);
// EEPROM 93C46 do cartucho (128 bytes, vira o save); save state.
// Tempo (aproximado): o GPU e o DSP pagam o acesso fora da RAM local e usam o placar (LOAD de fora e DIV); o
// Blitter leva um tempo que depende do que lê e grava, segura o barramento (o 68000 e os RISC esperam), mostra
// "ocupado" em B_CMD e interrompe o GPU no fim; o Object Processor também ocupa o barramento no começo da linha.
// Vídeo NTSC ou PAL (PAL pelo nome do arquivo: "(Europe)", "(E)", "(PAL)"...); modo entrelaçado (VP par).
// O que falta: tempo exato de cada ciclo de memória, CD (Jaguar CD), segundo controle e acessórios, ROM de ondas
// exata do Jerry (gerada aqui).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../md/m68k.h"
#include "risc.h"

namespace indigo::jaguar {

bool isJaguarRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

// botões do controle do Jaguar (1 = apertado)
enum Pad : uint32_t {
  J_UP = 1u << 0, J_DOWN = 1u << 1, J_LEFT = 1u << 2, J_RIGHT = 1u << 3, J_A = 1u << 4, J_B = 1u << 5,
  J_C = 1u << 6, J_PAUSE = 1u << 7, J_OPTION = 1u << 8, J_STAR = 1u << 9, J_HASH = 1u << 10,
  J_N0 = 1u << 11,  // teclas numéricas 0..9 = bits 11..20
};

class Jaguar : public md::M68kBus {
public:
  static constexpr int64_t CLOCK = 26590906;      // relógio do sistema (NTSC)
  static constexpr int64_t CLOCK_PAL = 26593900;  // relógio do sistema (PAL)
  static constexpr int FB_W = 1304, FB_H = 576;   // altura: 288 linhas, o dobro no modo entrelaçado

  Jaguar(const std::vector<uint8_t>& file, const std::string& nome, bool pal = false);
  const bool pal;           // aparelho PAL (50 Hz)
  int64_t clock() const { return pal ? CLOCK_PAL : CLOCK; }
  double frameRate() const;  // quadros (campos) por segundo com os registros de vídeo atuais
  bool interlaced() const { return !(reg16(0x3E) & 1); }  // VP par = entrelaçado
  int64_t accessTime = 0;    // relógio de quem está acessando o barramento agora (68000, GPU, DSP, OP)
  int64_t busFree() const { return busFree_; }  // até quando o Blitter/OP seguram o barramento
  bool blitBusy() const { return accessTime < blitEnd_; }
  void reset();
  void runFrame();

  // barramento (endereços de 24 bits, big-endian)
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  uint32_t read32(uint32_t a);
  void write32(uint32_t a, uint32_t v);
  int intAck(int) override { return 64; }  // o Tom fornece o vetor 64 (endereço 0x100)

  void riscCpuInt(bool fromDsp);  // GPU/DSP pedem interrupção ao 68000

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  md::M68k cpu;
  Risc gpu, dsp;
  std::vector<uint8_t> dram;  // 2 MB
  std::vector<uint8_t> cart;  // ROM do cartucho (vazio em programas para a RAM)
  std::vector<uint8_t> bios;  // ROM de partida do usuário (opcional, só fica mapeada em 0xE00000)
  uint8_t tom[0x4000] = {};   // registros, CLUT e buffers de linha do Tom
  uint8_t blitRegs[0x100] = {};
  uint32_t pad = 0;           // botões apertados (Pad)
  uint16_t eeprom[64];
  bool eepromDirty = false;
  std::string title;
  bool ramProgram = false;    // programa carregado na RAM (não cartucho)
  uint32_t entry = 0;

  // vídeo: quadro pronto (0xAABBGGRR)
  std::vector<uint32_t> fb;
  int outW = 326, outH = 240;

  // som (estéreo, na taxa pedida)
  void setSampleRate(int r) { sampleRate_ = r > 0 ? r : 48000; }
  size_t audioAvailable() const { return (audW_ - audR_) / 2; }
  void pullAudio(float* l, float* r, size_t n);
  uint64_t blits = 0;  // contagem (diagnóstico/testes)
  int64_t lastBlitCost = 0;  // tiques do último blit (diagnóstico/testes)
  int64_t now() const { return now_; }

private:
  bool blitting_ = false;  // Blitter no meio de uma cópia
  int64_t busFree_ = 0, blitEnd_ = 0;
  bool blitIrq_ = false;   // interrupção de fim do blit ainda por dar ao GPU
  int opPhrases_ = 0;      // phrases lidas pelo OP na linha (tempo de barramento)
  bool j64_ = false;       // .j64: sempre cartucho, mesmo que comece com 0x601B/0x0150
  // Tom/Jerry
  uint16_t tomRead16(uint32_t off);
  void tomWrite16(uint32_t off, uint16_t v);
  uint16_t jerryRead16(uint32_t off);
  void jerryWrite16(uint32_t off, uint16_t v);
  uint16_t reg16(uint32_t off) const { return uint16_t((tom[off] << 8) | tom[off + 1]); }
  void setReg16(uint32_t off, uint16_t v) { tom[off] = uint8_t(v >> 8); tom[off + 1] = uint8_t(v); }
  void tomIrq(int bit);    // pede a interrupção do Tom (bit 0 vídeo, 1 GPU, 2 OP, 3 PIT, 4 Jerry)
  void jerryIrq(int bit);  // interrupção do Jerry para o 68000 (via Tom)
  void updateIpl();
  uint8_t tomPending() const;
  // vídeo
  void opLine(int hl, int field);
  void drawBitmap(uint64_t p0, uint64_t p1, uint32_t hscale);
  void renderRow(int row);
  void beginFrame();
  uint64_t phrase(uint32_t a) { return (uint64_t(read32(a)) << 32) | read32(a + 4); }
  void storePhrase(uint32_t a, uint64_t p) { write32(a, uint32_t(p >> 32)); write32(a + 4, uint32_t(p)); }
  // blitter
  void blit(uint32_t cmd);
  void blitAlias(uint32_t off);
  // tempo
  void runTo(int64_t t);
  void timers();
  void resetTomPit();
  void resetJerryPit(int n);
  // EEPROM
  uint16_t eepromDo();
  void eepromClock();
  void eepromDi(int bit);
  void eepromCs();
  // controle
  uint16_t joyRead(bool buttons);
  void sync(StateIO& io);
  void hleBoot();

  std::vector<uint8_t> file_;
  uint8_t jmisc_[0x40] = {};  // registros simples do Jerry (0xF10000..3F)
  uint8_t wave_[0x1000];      // ROM de ondas (0xF1D000)
  int64_t now_ = 0, m68k_ = 0, hlStart_ = 0;
  int vc_ = 0;
  uint64_t frame_ = 0;
  uint8_t tomEn_ = 0, tomPend_ = 0, jEn_ = 0, jPend_ = 0;
  uint64_t ob_ = 0;     // objeto atual do OP (registros OB0-3)
  bool obf_ = false;    // OBF escrito (libera o OP depois de um objeto do GPU)
  uint16_t opFlag_ = 0;
  // temporizadores (0 = desligado)
  int64_t tomPitNext_ = 0, jPitNext_[2] = {0, 0};
  uint16_t jPit_[4] = {};
  int64_t i2sNext_ = 0;
  uint32_t sclk_ = 19, smode_ = 0, ltxd_ = 0, rtxd_ = 0;
  uint16_t joySel_ = 0xFFFF;
  // EEPROM 93C46 (comandos de 9 bits: partida, operação, endereço de 6 bits)
  int eeState_ = 0, eeBits_ = 0, eeOutCnt_ = 0;
  uint32_t eeShift_ = 0;
  uint8_t eeAddr_ = 0, eeOp_ = 0;
  bool eeWen_ = false;
  // som
  std::vector<float> aud_;
  size_t audW_ = 0, audR_ = 0;
  int sampleRate_ = 48000;
  int64_t audNext_ = 0;  // próxima amostra (em ciclos x taxa)
  // vídeo
  int top_ = 31, rows_ = 240;
  uint32_t cry_[0x10000];
};

class JaguarCore : public Core {
public:
  JaguarCore(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save);
  Jaguar& jag() { return *m_; }

  std::string system() const override { return "jaguar"; }
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
  bool hasSave() const override { return !m_->ramProgram; }
  bool saveDirty() const override { return m_->eepromDirty; }
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& b) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<Jaguar> m_;
};

}  // namespace indigo::jaguar
