// Núcleo do Atari Lynx (Lynx II): CPU 65SC02 (cpu.h), Mikey (temporizadores encadeados, interrupções, som de
// 4 canais LFSR com estéreo/atenuação, paleta, DMA da tela 160x102, UART), Suzy (motor de sprites, multiplicação
// e divisão), cartucho com o contador de endereços e EEPROM 93Cxx.
// Referência de comportamento: documentação da Epyx ("Handy specification") e o emulador Handy (K. Wilkins,
// licença zlib), usado também como régua nos testes; o código aqui é próprio.
//
// Arquivos: .lnx (cabeçalho "LYNX" de 64 bytes), .lyx (imagem crua do cartucho) e .o (programa do BLL com
// cabeçalho "BS93", carregado direto na RAM).
// Partida: com a ROM do Lynx (firmware "lynx", lynxboot.img, 512 bytes) o boot é o de verdade. Sem ela, o núcleo
// imita o que a ROM faz (como o Handy): decifra o primeiro quadro do carregador do cartucho (RSA, expoente 3,
// chave pública conhecida), coloca em $0200 e pula para lá, deixando o hardware como a ROM deixa (tela em $2000,
// paleta em tons de cinza, temporizadores de 60 Hz). As rotinas da ROM em $FE00/$FE19/$FE4A/$FF80 também são
// imitadas para os carregadores que as chamam.
//
// Teclas: direcional = direcional; A = A (externo), B = B (interno); L = Opção 1; R = Opção 2; START = Pause.
// Rotação: quando o cabeçalho .lnx pede (jogos "em pé"), a imagem sai girada (largura e altura trocadas) e o
// direcional é girado junto, para "cima" continuar sendo o cima da tela.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "cpu.h"

namespace indigo::lynx {

constexpr int SCREEN_W = 160, SCREEN_H = 102;
constexpr uint32_t TICKS_PER_FRAME = 16 * 159 * 105;  // linha de 159 µs, 105 linhas: 59,898 Hz

bool isLynxRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);

class Lynx;

// ------------------------------------------------------------------ EEPROM 93C46/56/66/76/86 (3 fios)
// CS = linha A7 do contador do cartucho, CLK = A1, DI/DO = pino AUDIN (bit 4 de IODAT).
class Eeprom {
public:
  void configure(uint8_t type);  // byte "eeprom" do cabeçalho .lnx
  bool present() const { return addrMask != 0; }
  size_t size() const { return present() ? (eightBit ? addrMask + 1 : (addrMask + 1) * 2) : 0; }
  void reset();
  void counterChanged(uint16_t counter, uint8_t iodir, uint8_t iodat);
  bool outputBit();  // leitura de IODAT com AUDIN como entrada
  void sync(StateIO& io);

  std::vector<uint8_t> data = std::vector<uint8_t>(2048, 0xFF);
  bool dirty = false;

private:
  enum State : uint8_t { IDLE, CMD, DATA, WAIT, BUSY };
  uint16_t addrMask = 0;
  int cmdBits = 1, addrBits = 1;
  bool eightBit = false;
  uint8_t state = IDLE;
  uint8_t busyCount = 0;
  bool writeEnabled = false, dout = true, lastClk = false;
  int bitsLeft = 0;
  uint32_t shift = 0, readData = 0;
  uint16_t addr = 0;
  uint16_t word(uint16_t a) const;
  void setWord(uint16_t a, uint16_t v);
};

// ------------------------------------------------------------------ cartucho
struct Cart {
  std::vector<uint8_t> bank[2], bankA[2];  // bankA: segundo par de bancos dos cartuchos com AUDIN
  uint32_t mask[2] = {0, 0};
  int shiftBits[2] = {0, 0};
  bool audin = false;
  int rotation = 0;  // 0 nenhuma, 1 esquerda, 2 direita
  uint8_t eepromType = 0;
  std::string name;
  // contador de endereços (ripple counter) e registrador de página
  uint16_t counter = 0;
  uint8_t shifter = 0;
  bool addrData = false, strobe = false;

  uint8_t read(int b, bool useA);
  void write(int b, bool useA, uint8_t v);
};

// ------------------------------------------------------------------ Mikey
struct Timer {
  uint8_t bkup = 0, cnt = 0, linking = 0;
  bool irqEn = false, reload = false, count = false, done = false;
  bool lastClock = false, borrowIn = false, borrowOut = false;
  uint64_t next = 0;  // instante (tique de 16 MHz) da próxima contagem dos temporizadores com relógio próprio
};

struct Audio {
  int8_t vol = 0, out = 0;
  uint16_t lfsr = 0;   // 12 bits
  uint16_t taps = 0;   // 12 bits (máscara das realimentações, bits 0-5, 7, 10, 11)
  bool integrate = false;
};

// ------------------------------------------------------------------ máquina
class Lynx {
public:
  Lynx(const std::vector<uint8_t>& file, const std::vector<uint8_t>* save);

  void reset();
  void runFrame();
  void setKeys(uint32_t mask);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);
  void setSampleRate(int rate);
  size_t audioAvailable() const { return (wpos + BUF - rpos) % BUF; }
  void pullAudio(float* l, float* r, size_t n);

  // barramento da CPU (cada acesso conta os tiques do ciclo)
  uint8_t read(uint16_t a);
  void write(uint16_t a, uint8_t v);

  Cart cart;
  Eeprom eeprom;
  Cpu65SC02<Lynx> cpu;
  std::string title;
  bool homebrew = false;  // .o (BS93)
  bool realBoot = false;  // ROM do Lynx fornecida
  std::vector<uint32_t> fb;  // imagem já girada
  int outW = SCREEN_W, outH = SCREEN_H;
  uint64_t now = 0;

private:
  // memória
  uint8_t ram[65536];
  uint8_t rom[512];
  uint8_t mapctl = 0;
  uint16_t lastPage = 0xFFFF;
  // CPU dormindo (Suzy desenhando)
  uint64_t sleepUntil = 0, suzyBusyUntil = 0;
  bool sleeping = false;
  uint64_t nextEvent = 0, frameEnd = 0;

  // ---------------- Mikey
  Timer tim[12];  // 0-7 temporizadores, 8-11 canais de som
  Audio aud[4];
  uint8_t atten[4] = {0xFF, 0xFF, 0xFF, 0xFF}, pan = 0, stereo = 0;
  uint8_t intStatus = 0;
  uint8_t dispctl = 0, pbkup = 0;
  uint16_t dispadr = 0, lynxAddr = 0;
  int lynxLine = 0, dmaLines = 0, curRow = 0;
  uint8_t green[16] = {}, bluered[16] = {};
  uint8_t iodir = 0, iodat = 0, sysctl1 = 0;
  bool rest = false;
  // UART (ComLynx sem cabo: o que sai volta para a entrada)
  bool txIrqEn = false, rxIrqEn = false, parEn = false, parEven = false, sendBreak = false;
  bool rxReady = false, overrun = false, framing = false;
  int txCountdown = -1, rxCountdown = -1;
  uint16_t txData = 0, rxData = 0, rxPending = 0;
  bool rxHasPending = false;
  uint32_t lcd[SCREEN_W * SCREEN_H] = {};

  void mikeyWrite(uint8_t r, uint8_t v);
  uint8_t mikeyRead(uint8_t r);
  void updateTimers();
  void countTimer(int i, uint64_t when);
  void borrow(int i, uint64_t when);
  void scheduleNext();
  void writeCtla(int i, uint8_t v);
  void hbl();
  void vbl();
  void uartTick();
  bool irqLine() const;
  bool uartIrq() const;

  // ---------------- som
  static constexpr size_t BUF = 16384;
  std::vector<float> bufL = std::vector<float>(BUF), bufR = std::vector<float>(BUF);
  size_t wpos = 0, rpos = 0;
  double ticksPerSample = 16000000.0 / 48000, sampleTick = 0;
  double accL = 0, accR = 0, hpL = 0, hpR = 0;
  int curL = 0, curR = 0;
  uint64_t audioTick = 0;
  void audioAdvance(uint64_t t);
  void audioMix();

  // ---------------- Suzy
  uint16_t sreg[0x30 / 2] = {};  // $FC00-$FC2F (palavras)
  uint8_t mathABCD[4] = {}, mathEFGH[4] = {}, mathJKLM[4] = {}, mathNP[2] = {};  // [0] = byte baixo
  int signAB = 1, signCD = 1;
  uint8_t sprctl0 = 0, sprctl1 = 0, sprcoll = 0, sprinit = 0;
  bool suzyBusEn = false, sprGo = false, everOn = false;
  bool stopOnCurrent = false, leftHand = false, vStretch = false, noCollide = false, accumulate = false,
       signedMath = false, lastCarry = false, mathBit = false, unsafe = false;
  uint8_t joystick = 0, switches = 0;
  uint8_t penIndex[16];
  // estado de uma linha de sprite
  uint32_t lineShift = 0;
  int lineShiftCount = 0, lineRepeat = 0, lineType = 0, linePixel = 0;
  uint32_t lineBitsLeft = 0;
  uint16_t lineTmp = 0, lineBase = 0, lineColl = 0;
  int collision = 0;
  uint32_t spriteCost = 0;

  void suzyWrite(uint8_t r, uint8_t v);
  uint8_t suzyRead(uint8_t r);
  uint16_t& sw(int off) { return sreg[off >> 1]; }
  void multiply();
  void divide();
  uint32_t paintSprites();
  int lineInit(int voff);
  int lineGetPixel();
  uint32_t lineGetBits(int bits);
  void processPixel(int hoff, int pixel);
  uint8_t ramPeek(uint16_t a) { spriteCost += 3; return ram[a]; }

  // ---------------- cartucho / boot
  uint32_t keys = 0;
  void cartStrobe(bool s);
  uint8_t cartRead(int b);
  void cartWrite(int b, uint8_t v);
  void hleBoot();
  void hleLoad();
  void hleCall(uint16_t pc);
  void sync(StateIO& io);
  void presentFrame();

  std::vector<uint8_t> homebrewImage;
  uint16_t homebrewLoad = 0;
};

class LynxCore : public Core {
public:
  LynxCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  std::string system() const override { return "lynx"; }
  std::string title() const override { return m_->title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->fb.data(); }
  int width() const override { return m_->outW; }
  int height() const override { return m_->outH; }
  double fps() const override { return 16000000.0 / TICKS_PER_FRAME; }  // 59,898
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return m_->eeprom.present(); }
  bool saveDirty() const override { return m_->eeprom.dirty; }
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& b) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }
  Lynx& machine() { return *m_; }

private:
  std::unique_ptr<Lynx> m_;
};

}  // namespace indigo::lynx
