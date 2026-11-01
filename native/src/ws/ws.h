// WonderSwan / WonderSwan Color / SwanCrystal: NEC V30MZ a 3,072 MHz, 159 linhas × 256 ciclos (75,47 quadros/s),
// tela 224×144 (2 fundos + 128 sprites, janelas, 2bpp/4bpp, planar/compacto, tons de cinza ou 12 bits de cor),
// som de 4 canais de tabela de onda (voz PCM no canal 2, sweep no 3, ruído no 4) + Hyper Voice e DMA de som no
// Color, EEPROM interna e do cartucho, SRAM e relógio (mapeador 2003) do cartucho.
//
// Sem BIOS: o núcleo começa no estado em que a ROM de boot deixa o console (CPU em FFFF:0000, que é o rodapé de
// 16 bytes no fim da ROM). firmware("ws")/("wsc") não é necessário.
//
// Rodapé (16 bytes finais): +7 bit 0 = jogo do Color; +10 tamanho da ROM; +11 tipo de save (01-05 SRAM,
// 10/20/50 EEPROM); +12 bit 0 = tela em pé, bit 1 = ROM de 8 bits, bit 2 = ROM rápida; +13 = 1: mapeador 2003
// com relógio. Jogo em pé: a imagem sai girada (144×224) e o aviso aparece em note().
//
// Teclas (bits de Key):
//   jogo deitado: direcional = X1-X4 (cima, direita, baixo, esquerda); A = A; B = B; START = START;
//                 X = Y1 (cima), R = Y2 (direita), L = Y3 (baixo), Y = Y4 (esquerda).
//   jogo em pé (console girado 90° para a esquerda): direcional = Y-pad girado (cima = Y2, direita = Y3,
//                 baixo = Y4, esquerda = Y1); botões A/B/X/Y = X-pad na mesma posição (X = X2 em cima,
//                 A = X3 à direita, B = X4 embaixo, Y = X1 à esquerda); L = botão B, R = botão A; START = START.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "v30mz.h"

namespace indigo::ws {

bool isWsRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

// EEPROM serial 93Cx6 vista pela interface do WonderSwan (comando de 16 bits + dado de 16 bits)
struct Eeprom {
  std::vector<uint8_t> data;
  int addrBits = 0;
  uint16_t rdData = 0, wrData = 0, command = 0;
  uint8_t control = 0;
  bool readDone = false, writable = true, protect = false;
  bool internal = false, internalColor = false;
  bool dirty = false;

  void init(size_t bytes, bool isInternal, bool color);
  uint8_t read(int port, bool colorMode) const;
  void write(int port, uint8_t v, bool colorMode);
  void sync(StateIO& io);

private:
  void execute(bool colorMode);
  bool canWrite(uint16_t cmd) const;
};

struct Rtc {
  uint8_t reg[18] = {};  // 0-6 data/hora (BCD), 7 estado, 16-17 alarme
  uint8_t command = 0, index = 0, fetched = 0xFF;
  bool active = false, ready = false;
  int64_t secondClock = 0;  // ciclos de CPU acumulados para o próximo segundo

  void power();
  void tickSecond();
  uint8_t controlRead() const { return static_cast<uint8_t>((command & 15) | (active ? 0x10 : 0) | (ready ? 0x80 : 0)); }
  void controlWrite(uint8_t v);
  uint8_t dataRead();
  void dataWrite(uint8_t v);
  void sync(StateIO& io);

private:
  void fetch();
};

class WS : public V30Bus {
public:
  WS(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);

  V30MZ cpu;
  bool colorHw = false;   // SPHINX (WonderSwan Color); falso = ASWAN (monocromático)
  bool vertical = false;  // orientação atual da tela
  uint32_t fb[224 * 144] = {};
  uint32_t fbRot[224 * 144] = {};
  int frame = 0;

  void reset();
  void setKeys(uint32_t mask) { keys_ = mask; }
  void runFrame();
  const uint32_t* video() const { return vertical ? fbRot : fb; }

  // som
  void setSampleRate(int rate);
  size_t audioAvailable() const { return (aw_ + ABUF - ar_) % ABUF; }
  void pullAudio(float* l, float* r, size_t n);

  bool hasSave() const { return !sram_.empty() || !cartEeprom_.data.empty(); }
  bool saveDirty() const { return dirty_ || cartEeprom_.dirty || intEeprom_.dirty; }
  std::vector<uint8_t> exportSave();
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);
  std::string note() const;

  // V30Bus
  uint8_t memRead(uint32_t a) override;
  void memWrite(uint32_t a, uint8_t v) override;
  uint8_t ioRead(uint16_t port) override;
  void ioWrite(uint16_t port, uint8_t v) override;
  int memTiming(uint32_t a) override;
  int ioTiming(uint16_t port) override;

  // usado pelos testes
  const uint8_t* iram() const { return iram_.data(); }

private:
  struct Window { uint8_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

  void sync(StateIO& io);
  void renderLine(int y);
  void raise(int irq) { if (intEnable_ & (1 << irq)) intStatus_ |= static_cast<uint8_t>(1 << irq); }
  void pollInterrupts();
  void timerStep(uint16_t& counter, uint16_t freq, int ctrl, int irq);
  void runCpu(int64_t until);
  uint8_t keypadRead() const;
  void gdma();
  void apuRun();
  void apuSample();
  void soundDma();
  void hyperWrite(uint8_t v);
  bool colorMode() const { return colorHw && (dispMode_ & 0x80); }
  void updateVertical();

  // cartucho
  std::vector<uint8_t> rom_, sram_;
  uint32_t romMask_ = 0;
  bool romWord_ = true, hasRtc_ = false, footerVertical_ = false;
  Eeprom cartEeprom_, intEeprom_;
  Rtc rtc_;
  bool dirty_ = false;
  uint16_t bankRom0_ = 0xFF, bankRom1_ = 0xFF, bankRom2_ = 0xFF, bankSram_ = 0xFF;

  // console
  std::vector<uint8_t> iram_;
  uint8_t hwFlags_ = 0;  // porta A0: bit 2 ROM de 16 bits, bit 3 espera da ROM
  uint8_t dispMode_ = 0; // porta 60
  uint8_t intBase_ = 0, intEnable_ = 0, intStatus_ = 0, keyMatrix_ = 0;
  uint8_t serialCtrl_ = 0, nmiCtrl_ = 0;
  uint16_t lastKeys_ = 0;
  uint32_t keys_ = 0;
  uint32_t gdmaSrc_ = 0;
  uint16_t gdmaDst_ = 0, gdmaLen_ = 0;
  uint8_t gdmaCtrl_ = 0;
  uint8_t gpoEn_ = 0, gpoData_ = 0;
  int64_t frameStart_ = 0;

  // vídeo
  uint8_t dispCtrl_ = 0, backColor_ = 0, line_ = 0, lineCmp_ = 0, sprBase_ = 0, sprFirst_ = 0, sprCount_ = 0;
  uint8_t mapBase_ = 0, scr1x_ = 0, scr1y_ = 0, scr2x_ = 0, scr2y_ = 0, lcdCtrl_ = 1, lcdIcon_ = 0;
  uint8_t vtotal_ = 158, vsync_ = 155;
  Window win2_, winSpr_;
  uint8_t pool_[8] = {};
  uint8_t monoPal_[16][4] = {};
  uint8_t tmrCtrl_ = 0;
  uint16_t htFreq_ = 0, htCnt_ = 0, vtFreq_ = 0, vtCnt_ = 0;
  uint32_t oam_[128] = {};
  int oamCount_ = 0;

  // som
  struct Channel { uint16_t pitch = 0, period = 0; uint8_t volL = 0, volR = 0, offset = 0; };
  Channel ch_[4];
  uint8_t sndCtrl_ = 0, sndOut_ = 0, waveBase_ = 0, voiceCtrl_ = 0, noiseCtrl_ = 0, sweepTime_ = 0, sndDbg_ = 0;
  int8_t sweepValue_ = 0;
  int32_t sweepCounter_ = 0;
  uint16_t noiseLfsr_ = 0;
  bool noiseOut_ = false;
  uint8_t masterVol_ = 3;
  int64_t apuClock_ = 0;  // ciclos já processados pelo som
  int sweepClock_ = 0;
  uint16_t outL_ = 0, outR_ = 0;  // última saída dos canais 1-4 (portas 96-9B)
  // DMA de som e Hyper Voice (Color)
  uint32_t sdmaSrc_ = 0, sdmaLen_ = 0, sdmaSrcReload_ = 0, sdmaLenReload_ = 0;
  uint8_t sdmaCtrl_ = 0;
  int sdmaClock_ = 0;
  uint8_t hyperCtrl_ = 0, hyperChan_ = 0;
  uint8_t hvLeft_ = 0, hvRight_ = 0;
  bool hvLeftChanged_ = false, hvRightChanged_ = false, hvChannel_ = false;
  int hvClock_ = 0;
  int16_t hvOutL_ = 0, hvOutR_ = 0;
  // saída para o computador
  static constexpr size_t ABUF = 16384;
  std::vector<float> abL_ = std::vector<float>(ABUF), abR_ = std::vector<float>(ABUF);
  size_t aw_ = 0, ar_ = 0;
  int hostRate_ = 48000;
  int64_t outPhase_ = 0;
  double hpL_ = 0, hpR_ = 0;
};

class WSCore : public Core {
public:
  WSCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) : m_(std::make_unique<WS>(rom, save)) {}
  WS& ws() { return *m_; }

  std::string system() const override { return m_->colorHw ? "wsc" : "ws"; }
  std::string title() const override { return ""; }  // o rodapé não traz título
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->video(); }
  int width() const override { return m_->vertical ? 144 : 224; }
  int height() const override { return m_->vertical ? 224 : 144; }
  double fps() const override { return 3072000.0 / (256 * 159); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return m_->hasSave(); }
  bool saveDirty() const override { return m_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::string note() const override { return m_->note(); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<WS> m_;
};

}  // namespace indigo::ws
