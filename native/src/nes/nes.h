// Núcleo NES / Famicom (NTSC): CPU 2A03 (cpu.h), PPU 2C02 ponto a ponto, APU com os 5 canais, mapeadores mais
// comuns e o barramento com o relógio mestre (12 por ciclo de CPU, 4 por ponto do PPU), DMA de sprites e do DMC.
// Núcleo próprio do Índigo nativo (a versão web não tem NES): a régua são as ROMs de teste públicas.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "cpu.h"

namespace indigo::nes {

class NES;

// ------------------------------------------------------------------ cartucho e mapeadores
struct Cart {
  std::vector<uint8_t> prg, chr, prgRam;
  bool chrRam = false, battery = false, fourScreen = false, vertical = false;
  int mapper = 0, submapper = 0;
  size_t ramSize = 0;  // RAM do cartucho informada no cabeçalho NES 2.0 (0 = não informada)
};
// mapeadores com chips de som ou vídeo próprios (mappers_ext.cpp): 5, 19, 24, 26, 85 e o som do 69
std::unique_ptr<class Mapper> createExtMapper(Cart& c);
// lê um arquivo .nes (iNES / NES 2.0); devolve false com a mensagem se não der
bool parseRom(const std::vector<uint8_t>& data, Cart& out, std::string& error);
bool isNesRom(const std::vector<uint8_t>& data);

class Mapper {
public:
  enum Mirror { HORIZONTAL, VERTICAL, SINGLE0, SINGLE1, FOUR };
  explicit Mapper(Cart& c) : cart(c) {}
  virtual ~Mapper() = default;
  static std::unique_ptr<Mapper> create(Cart& c);  // nullptr se o número não é suportado

  virtual void reset() {}
  virtual void update() {}                       // recalcula os bancos a partir dos registradores
  virtual void writeReg(uint16_t a, uint8_t v) {}  // $4020–$FFFF (a RAM do cartucho já foi tratada)
  virtual void ppuAddress(uint16_t a) {}        // cada endereço posto no barramento do PPU (A12 do MMC3)
  virtual void cpuCycle() {}
  virtual void syncRegs(StateIO& io) {}
  virtual uint8_t chrRead(uint16_t a) { return cart.chr[chrOff[a >> 10] + (a & 0x3FF)]; }
  // com customCpu, a CPU lê e grava $4020–$FFFF só pelo mapeador (cpuRead e writeReg; a RAM do cartucho também)
  virtual uint8_t cpuRead(uint16_t a, uint8_t openBus) { return openBus; }
  virtual uint8_t cpuPeek(uint16_t a) { return cpuRead(a, 0); }
  // com customNt, as tabelas de nomes ($2000–$3EFF) passam pelo mapeador
  virtual uint8_t ntRead(uint16_t a) { return ciram[(nt[(a >> 10) & 3] << 10) | (a & 0x3FF)]; }
  virtual void ntWrite(uint16_t a, uint8_t v) { ciram[(nt[(a >> 10) & 3] << 10) | (a & 0x3FF)] = v; }
  virtual void ppuRegWrite(int r, uint8_t v) {}  // gravações da CPU em $2000–$2007 (o MMC5 escuta $2000 e $2001)
  virtual float audio() { return 0; }            // som do cartucho, somado ao do console a cada ciclo de CPU

  uint8_t prgRead(uint16_t a) const { return cart.prg[prgOff[(a >> 13) & 3] + (a & 0x1FFF)]; }
  virtual void chrWrite(uint16_t a, uint8_t v) { if (cart.chrRam) cart.chr[chrOff[a >> 10] + (a & 0x3FF)] = v; }

  Cart& cart;
  NES* nes = nullptr;
  uint32_t prgOff[4] = {}, chrOff[8] = {};
  uint8_t nt[4] = {0, 1, 0, 1};
  bool ramEnabled = true, ramWritable = true, irq = false;
  bool customCpu = false, customNt = false;
  uint8_t* ciram = nullptr;  // as 2 KB de tabelas de nomes do console (e mais 2 KB nas placas de 4 telas)
  int lowRom = -1;  // banco de PRG ROM mapeado em $6000–$7FFF (-1 = RAM do cartucho)

protected:
  void prg8(int slot, int bank);
  int prgBank8(int bank) const;  // deslocamento de um banco de 8 KB (com módulo)
  void prg16(int slot, int bank);
  void prg32(int bank);
  void chr1(int slot, int bank);
  void chr2(int slot, int bank);
  void chr4(int slot, int bank);
  void chr8(int bank);
  void mirror(int m);
};

// ------------------------------------------------------------------ PPU
class PPU {
public:
  explicit PPU(NES& n) : nes(n) {}
  void reset(bool power);
  void step();  // um ponto
  uint8_t readReg(int r);
  void writeReg(int r, uint8_t v);
  bool nmiLine() const { return (status & 0x80) && (ctrl & 0x80); }
  void sync(StateIO& io);

  uint32_t fb[256 * 240] = {};
  int scanline = 0, dot = 0;
  bool frameDone = false, oddFrame = false;
  uint8_t oam[256] = {};
  uint8_t* vram() { return ciram; }
  bool suppressNmi = false;  // leitura de $2002 bem na hora do vblank: o NMI não acontece

private:
  uint8_t openBus() const;
  void drive(uint8_t val, uint8_t bits);
  uint8_t busRead(uint16_t a);
  void busWrite(uint16_t a, uint8_t v);
  bool rendering() const { return mask & 0x18; }
  void incX();
  void incY();
  void evaluateSprites();
  void fetchSprite(int slot, int phase);
  void pixel();

  NES& nes;
  uint8_t ciram[0x1000] = {}, palette[32] = {};
  uint8_t ctrl = 0, mask = 0, status = 0, oamAddr = 0, readBuf = 0, latch = 0, fineX = 0;
  uint16_t v = 0, t = 0;
  bool w = false, preventVbl = false, skipArmed = false;
  int ignoreRead = 0;
  uint32_t frames = 0, latchAt[8] = {};
  // fundo
  uint16_t patLo = 0, patHi = 0, attLo = 0, attHi = 0;
  uint8_t ntByte = 0, atByte = 0, patLoByte = 0, patHiByte = 0;
  // sprites da linha (avaliados na linha anterior)
  struct Sprite { uint8_t y, tile, attr, x, lo, hi, zero; };
  Sprite next[8] = {}, line[8] = {};
  int nextCount = 0, lineCount = 0, overflowDot = -1;
};

// ------------------------------------------------------------------ APU
class APU {
public:
  explicit APU(NES& n) : nes(n) { setSampleRate(48000); }
  void reset(bool power);
  void clock();  // um ciclo de CPU
  void write(uint16_t a, uint8_t v);
  uint8_t readStatus();
  bool irqLine() const { return frameIrq || dmcIrq; }
  void setSampleRate(int rate);
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);
  void dmcFetched(uint8_t v);
  bool dmcWantsByte() const { return dmcBufferEmpty && dmcRemaining > 0; }
  uint16_t dmcAddress() const { return dmcAddr; }

private:
  struct Envelope { bool start = false, loop = false, constant = false; uint8_t vol = 0, divider = 0, decay = 0; };
  struct Pulse {
    Envelope env;
    uint8_t duty = 0, seq = 0, length = 0;
    uint16_t period = 0;
    int timer = 0;
    bool enabled = false, halt = false;
    bool sweepOn = false, sweepNeg = false, sweepReload = false;
    uint8_t sweepPeriod = 0, sweepShift = 0, sweepDiv = 0;
    // recarga do contador de tamanho é aplicada no fim do ciclo (ignorada se o quadro mexeu nele no mesmo ciclo)
    uint8_t reloadLen = 0, prevLen = 0;
    bool newHalt = false;
  };
  void envClock(Envelope& e);
  int sweepTarget(const Pulse& p, bool second) const;
  void sweepClock(Pulse& p, bool second);
  int pulseOut(const Pulse& p, bool second) const;
  void quarter();
  void half();
  void lengthEnd();
  void emit();

  NES& nes;
  Pulse pulse[2];
  // triângulo
  bool triEnabled = false, triControl = false, triReloadFlag = false, triNewHalt = false;
  uint8_t triLength = 0, triLinear = 0, triLinearLoad = 0, triSeq = 0, triReloadLen = 0, triPrevLen = 0;
  uint16_t triPeriod = 0;
  int triTimer = 0;
  // ruído
  Envelope noiseEnv;
  bool noiseEnabled = false, noiseHalt = false, noiseMode = false, noiseNewHalt = false;
  uint8_t noiseLength = 0, noiseReloadLen = 0, noisePrevLen = 0;
  uint16_t noisePeriod = 4, noiseShift = 1;
  int noiseTimer = 0;
  // DMC
  bool dmcIrqEnable = false, dmcLoop = false, dmcIrq = false, dmcSilence = true, dmcBufferEmpty = true;
  uint8_t dmcRate = 0, dmcLevel = 0, dmcBuffer = 0, dmcShift = 0, dmcBits = 8;
  uint16_t dmcStart = 0xC000, dmcAddr = 0xC000, dmcLen = 1, dmcRemaining = 0;
  int dmcTimer = 0;
  // contador de quadros
  int fcMode = 0, fcCycle = 0, fcStep = 0, fcWriteDelay = 0, fcBlock = 0;
  uint8_t fcNewValue = 0;
  bool fcInhibit = false, frameIrq = false;
  bool odd = false;
  // som de saída
  static constexpr size_t bufSize = 16384;
  std::vector<float> buf = std::vector<float>(bufSize);
  size_t wpos = 0, rpos = 0;
  double cyclesPerSample = 0, frac = 0, acc = 0, capHi = 0, capLo = 0, lowState = 0;
  int accN = 0;
};

// ------------------------------------------------------------------ máquina
class NES : public CpuBus {
public:
  // fazMapeador: cria um mapeador que não vem de um cabeçalho iNES (o adaptador do Famicom Disk System, src/fds)
  explicit NES(Cart cart, const std::function<std::unique_ptr<Mapper>(Cart&)>& fazMapeador = {});
  Cart cart;
  CPU cpu;
  PPU ppu;
  APU apu;
  std::unique_ptr<Mapper> mapper;
  bool dirty = false;
  uint64_t cycles = 0;   // ciclos de CPU desde que ligou
  uint64_t ppuDots = 0;  // pontos do PPU desde que ligou (filtro do A12 do MMC3)

  void reset(bool power);
  void runFrame();
  void setKeys(uint32_t mask);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;
  uint8_t peek(uint16_t a);  // sem efeito colateral (ferramentas de teste)

private:
  void startCycle(bool forRead);
  void endCycle(bool forRead);
  void runPpu(uint64_t to);
  void dma(uint16_t readAddr);
  uint8_t memRead(uint16_t a);
  void memWrite(uint16_t a, uint8_t v);
  void sync(StateIO& io);

  uint8_t ram[0x800] = {};
  uint64_t master = 0, ppuMaster = 0;
  uint8_t openBus = 0;
  // DMA de sprites ($4014) e do DMC
  bool oamDma = false;
  uint8_t oamPage = 0;
  // controles
  uint8_t pad[2] = {}, shift[2] = {};
  bool strobe = false;
};

class NESCore : public Core {
public:
  NESCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  NES& nes() { return *m_; }

  std::string system() const override { return "nes"; }
  std::string title() const override { return ""; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(false); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->ppu.fb; }
  int width() const override { return 256; }
  int height() const override { return 240; }
  double fps() const override { return 236250000.0 / 11.0 / 4.0 / (341.0 * 262 - 0.5); }  // 60,0988
  void setSampleRate(int rate) override { m_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->apu.pull(l, r, n); }
  bool hasSave() const override { return m_->cart.battery; }
  bool saveDirty() const override { return m_->dirty; }
  std::vector<uint8_t> exportSave() override { m_->dirty = false; return m_->cart.prgRam; }
  void importSave(const std::vector<uint8_t>& b) override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<NES> m_;
};

}  // namespace indigo::nes
