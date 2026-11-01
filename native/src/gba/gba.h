// Núcleo do Game Boy Advance (porte de src/cpu.js, bus.js, io.js, ppu.js, apu.js, bios.js, save.js, gpio.js e gba.js).
//
// Convenção numérica: o JavaScript guarda registradores em Int32Array e calcula endereços como Number. Para o
// porte se comportar igual bit a bit, registradores e valores são int32_t (com estouro em complemento de 2) e
// os endereços passados ao barramento são int64_t ("Number"), mascarados lá dentro como o JS faz com >>> e &.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"

namespace indigo::gba {

class GBA;

// conversões no estilo do JS
inline int32_t I32(int64_t x) { return static_cast<int32_t>(static_cast<uint32_t>(x)); }  // x | 0
inline uint32_t U32(int64_t x) { return static_cast<uint32_t>(x); }                         // x >>> 0

// ------------------------------------------------------------ CPU ARM7TDMI
enum : int { MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13, MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F };

class CPU {
public:
  explicit CPU(GBA& g);

  int32_t r[16] = {};
  int32_t bankR13[6] = {}, bankR14[6] = {}, spsrBank[6] = {};
  int32_t usrHi[5] = {}, fiqHi[5] = {};
  int n = 0, z = 0, c = 0, v = 0, i = 1, f = 1, t = 0, mode = MODE_SVC;
  int64_t nextPC = 0;
  bool branched = false;
  int shC = 0;
  bool halted = false;
  bool hle = true;  // SWI tratado pela BIOS própria (HleBios)

  int32_t cpsr() const;
  void setCpsr(int32_t val);
  int32_t spsr() const;
  void setSpsr(int32_t val);
  void switchMode(int newMode);
  void reset(bool skipBios);
  bool checkCond(int cond) const;
  int step();
  void setPC(int64_t val) { r[15] = I32(val); branched = true; }
  void irq();
  void sync(StateIO& io);

  using Op = int (CPU::*)(uint32_t);

private:
  void refill();
  void swiException(int32_t retAddr);
  int undefinedInstr(uint32_t) { return 1; }
  int32_t shiftImm(int type, int32_t v, int amt);
  int32_t shiftReg(int type, int32_t v, int amt);
  void setNZ(int32_t res) { n = static_cast<int>(U32(res) >> 31); z = res == 0 ? 1 : 0; }
  int32_t add(int32_t a, int32_t b, bool s);
  int32_t adc(int32_t a, int32_t b, bool s);
  int32_t sub(int32_t a, int32_t b, bool s);
  int32_t sbc(int32_t a, int32_t b, bool s);
  static int mulCycles(int32_t rs);
  int32_t ldrWord(int64_t addr);
  int32_t ldrHalf(int64_t addr);
  int32_t ldrSHalf(int64_t addr);

  static Op decodeArm(int key);
  static Op decodeThumb(uint32_t ins);
  static const Op* armLut();
  static const Op* thumbLut();

  int armDP(uint32_t ins);
  int armMrs(uint32_t ins);
  int armMsr(uint32_t ins);
  int armBx(uint32_t ins);
  int armMul(uint32_t ins);
  int armMulLong(uint32_t ins);
  int armSwp(uint32_t ins);
  int armHalf(uint32_t ins);
  int armSdt(uint32_t ins);
  int armBlock(uint32_t ins);
  int armBranch(uint32_t ins);
  int armSwi(uint32_t ins);
  int thShift(uint32_t ins);
  int thAddSub(uint32_t ins);
  int thImm(uint32_t ins);
  int thAlu(uint32_t ins);
  int thHi(uint32_t ins);
  int thLdrPc(uint32_t ins);
  int thLsReg(uint32_t ins);
  int thLsSign(uint32_t ins);
  int thLsImm(uint32_t ins);
  int thLsHalf(uint32_t ins);
  int thLsSp(uint32_t ins);
  int thAddr(uint32_t ins);
  int thAddSp(uint32_t ins);
  int thPushPop(uint32_t ins);
  int thLdmStm(uint32_t ins);
  int thBcond(uint32_t ins);
  int thSwi(uint32_t ins);
  int thB(uint32_t ins);
  int thBlHi(uint32_t ins);
  int thBlLo(uint32_t ins);

  GBA& gba;
};

// ------------------------------------------------------------ save do cartucho (SRAM, Flash, EEPROM)
enum class SaveType { Sram, Flash64, Flash128, Eeprom };
SaveType detectSaveType(const std::vector<uint8_t>& rom);

class Save {
public:
  explicit Save(SaveType t);
  SaveType type;
  bool isEeprom;
  bool dirty = false;
  std::vector<uint8_t> data;

  void load(const std::vector<uint8_t>& bytes);
  int read8(int64_t addr);
  void write8(int64_t addr, int v);
  void dmaHint(int count, bool isWrite);
  int read16();
  void write16(int v);
  void sync(StateIO& io);

private:
  bool isFlash() const { return type == SaveType::Flash64 || type == SaveType::Flash128; }
  void resizeEeprom(int bits);
  int fState = 0, fCmd = 0;
  bool fId = false;
  int fBank = 0;
  std::vector<uint8_t> eBits;
  int eAddrBits = 0;
  bool eReading = false;
  std::vector<uint8_t> eReadBuf;
  int eReadPos = 0;
};

// ------------------------------------------------------------ relógio do cartucho (GPIO + S-3511A)
bool hasRtc(const std::vector<uint8_t>& rom, const std::string& gameCode);
// para testes determinísticos: fixa "agora" (ms desde 1970) de todos os relógios; 0 volta ao relógio real
void setFixedNowMs(double ms);

class CartRtc {
public:
  double offset = 0;  // ms somados à hora real (ajuste feito pelo jogo)
  int status = 0x40;  // modo 24 horas
  bool readable() const { return (ctrl & 1) != 0; }
  int read(int off) const;
  void write(int off, int v);
  void sync(StateIO& io);

private:
  void command();
  void fillTime(bool withDate);
  void commit();
  int dir = 0, ctrl = 0, pins = 0, out = 0;
  int state = 0, bits = 0, cmd = 0;
  uint8_t data[8] = {};
  int len = 0, reg = 0;
};

// ------------------------------------------------------------ barramento
class Bus {
public:
  explicit Bus(GBA& g);

  std::vector<uint8_t> bios, ewram, iwram, pal, vram, oam, rom;
  int64_t romSize = 0;
  int32_t biosLast = static_cast<int32_t>(0xE129F000u);
  int32_t lastFetch = 0;
  int64_t cycles = 0;
  uint8_t wait16[16] = {}, wait32[16] = {};
  std::unique_ptr<Save> save;
  std::unique_ptr<CartRtc> gpio;

  void loadRom(const std::vector<uint8_t>& data);
  void setWaitcnt(int v);
  void codeCost(int64_t addr, bool is32);
  int32_t fetch16(int64_t addr);
  int32_t fetch32(int64_t addr);
  int32_t read8(int64_t addr);
  int32_t read16(int64_t addr);
  int32_t read32(int64_t addr);
  void write8(int64_t addr, int64_t val);
  void write16(int64_t addr, int64_t val);
  void write32(int64_t addr, int64_t val);
  void sync(StateIO& io);

  // acesso direto às memórias (little-endian, alinhado como as visões tipadas do JS)
  static uint16_t ld16(const std::vector<uint8_t>& m, size_t off) { return static_cast<uint16_t>(m[off] | (m[off + 1] << 8)); }
  static int32_t ld32(const std::vector<uint8_t>& m, size_t off) {
    return static_cast<int32_t>(m[off] | (m[off + 1] << 8) | (m[off + 2] << 16) | (uint32_t(m[off + 3]) << 24));
  }
  static void st16(std::vector<uint8_t>& m, size_t off, uint32_t v) { m[off] = v & 0xFF; m[off + 1] = (v >> 8) & 0xFF; }
  static void st32(std::vector<uint8_t>& m, size_t off, uint32_t v) {
    m[off] = v & 0xFF; m[off + 1] = (v >> 8) & 0xFF; m[off + 2] = (v >> 16) & 0xFF; m[off + 3] = (v >> 24) & 0xFF;
  }

private:
  void romCost(int64_t addr, bool is32, bool isCode);
  static size_t vramIndex(int64_t addr);
  GBA& gba;
  uint8_t romN[16] = {}, romS[16] = {};
  bool prefetch = false;
  int64_t seqNext = -1, pfNext = -1, pfTime = 0;
  int pfCount = 0;
};

// ------------------------------------------------------------ registradores de E/S, DMA, timers, interrupções
enum : int { IRQ_VBLANK = 0x1, IRQ_HBLANK = 0x2, IRQ_VCOUNT = 0x4, IRQ_TIMER0 = 0x8, IRQ_DMA0 = 0x100, IRQ_KEYPAD = 0x1000 };

class IO {
public:
  explicit IO(GBA& g) : gba(g) {}

  uint16_t regs[0x200] = {};
  int ie = 0, ifr = 0, ime = 0;
  int dispstat = 0, vcount = 0;
  int keys = 0x3FF;  // 1 = solta
  int32_t tReload[4] = {}, tCounter[4] = {}, tCtrl[4] = {}, tAcc[4] = {};
  bool timersActive = false;
  int32_t dSad[4] = {}, dDad[4] = {}, dCnt[4] = {}, dCtrl[4] = {}, dSrc[4] = {}, dDst[4] = {}, dCount[4] = {}, dLatch[4] = {};
  int postflg = 0;
  int64_t stepTicked = 0;

  void reset();
  void requestIrq(int bits) { ifr |= bits; }
  int read8(int a);
  int read16(int a);
  void write8(int a, int v);
  void write16(int a, int v);
  void write32(int a, int32_t v);
  void setKeys(int mask);
  void tickTimers(int64_t cycles);
  int32_t cyclesToTimerEvent() const;
  void triggerDma(int timing);
  void soundDma(int32_t fifoAddr);
  void sync(StateIO& io);

private:
  void checkKeyIrq();
  void timerSync();
  void writeTimerCtrl(int i, int v);
  void timerAdd(int i, int64_t inc);
  void timerOverflow(int i);
  void writeDmaCtrl(int ch, int v);
  void runDma(int ch, bool sound = false);
  GBA& gba;
};

// ------------------------------------------------------------ vídeo
class PPU {
public:
  explicit PPU(GBA& g);
  uint32_t fb[240 * 160];

  void latchAffine(int a);
  void vblankLatch() { latchAffine(0x28); latchAffine(0x38); }
  void renderLine(int y);
  void sync(StateIO& io);

private:
  void advanceAffine();
  void renderText(int bg, int y, int32_t* out);
  void renderAffine(int bg, int32_t* out);
  void renderBitmap(int mode, int dispcnt, int32_t* out);
  void renderObjs(int y, int dispcnt, int mode);
  void compose(int y, int dispcnt, int bgMask, bool objOn);

  GBA& gba;
  int32_t bgLine[4][240] = {};
  int32_t objColor[240] = {};
  uint8_t objPrio[240] = {}, objAlpha[240] = {}, objWin[240] = {};
  int32_t refX[2] = {}, refY[2] = {};
  int32_t order[4] = {};
};

// ------------------------------------------------------------ som (4 canais PSG + DirectSound A/B)
struct Fifo {
  int8_t buf[32] = {};
  int r = 0, w = 0, count = 0;
  int out = 0;
  void push(int b) { if (count >= 32) return; buf[w] = static_cast<int8_t>(b); w = (w + 1) & 31; count++; }
  int pop() { if (count == 0) return out; out = buf[r]; r = (r + 1) & 31; count--; return out; }
  void reset() { r = w = count = 0; }
};

struct PsgChannel {
  bool on = false;
  int vol = 0, envDir = 0, envStep = 0, envTimer = 0;
  int length = 0;
  bool lenEnable = false;
  int freq = 0;
  double timer = 0;
  int pos = 0, duty = 0;
  int sweepTime = 0, sweepDir = 0, sweepShift = 0, sweepTimer = 0, shadow = 0;
  int lfsr = 0x7FFF;
  bool width7 = false;
};

class APU {
public:
  explicit APU(GBA& g);
  void setSampleRate(int rate) { cyclesPerSample = 16777216.0 / rate; }
  void reset();
  int statusBits() const;
  int readReg(int a, int raw) const;
  void writeReg(int a, int v);
  void fifoWrite32(int a, int32_t v);
  void fifoWrite16(int a, int v);
  void fifoWrite8(int a, int v);
  void timerOverflow(int t);
  void catchUp();
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* L, float* R, size_t n);
  void sync(StateIO& io);

private:
  void trigger(int n);
  int noisePeriod() const;
  void sweepCalc(bool write);
  void sequencer();
  void advanceChannels(double cyc);
  void genSample(double cyc);

  GBA& gba;
  double cyclesPerSample = 16777216.0 / 48000;
  PsgChannel ch[4];
  uint8_t wave[32] = {};
  Fifo fifoA, fifoB;
  double sampleCycle = 0, seqCycle = 0;
  int seqStep = 0;
  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL, bufR;
  size_t wpos = 0, rpos = 0;
};

// ------------------------------------------------------------ BIOS própria (HLE)
void installHleBios(Bus& bus);

class HleBios {
public:
  explicit HleBios(GBA& g) : gba(g) {}
  bool waiting = false;
  void handle(int n, int64_t addr);

private:
  void softReset();
  void registerRamReset(int32_t flags);
  void intrWait(int32_t discard, int32_t mask, int64_t addr);
  void div(int32_t num, int32_t den);
  void arcTan();
  void arcTan2();
  void cpuSet();
  void cpuFastSet();
  void bgAffineSet();
  void objAffineSet();
  void bitUnPack();
  void lz77(bool vram);
  void rle(bool vram);
  void huffman();
  void diff(int width, bool vram);
  void midiKey2Freq();
  GBA& gba;
};

// ------------------------------------------------------------ console
class GBA {
public:
  GBA();

  // ordem de construção: o barramento antes da CPU (a CPU não lê nada na construção, mas o reset usa os dois)
  Bus bus;
  IO io;
  CPU cpu;
  PPU ppu;
  APU apu;
  HleBios hle;
  int64_t totalBase = 0, chunkCycles = 0;
  int frame = 0;
  bool romLoaded = false, realBios = false;
  std::string title, gameCode;

  void loadBios(const std::vector<uint8_t>& data);
  void loadRom(const std::vector<uint8_t>& data, const std::vector<uint8_t>* saveBytes);
  void reset();
  int64_t now() const { return totalBase + chunkCycles + bus.cycles; }
  void setKeys(int pressedMask) { io.setKeys((~pressedMask) & 0x3FF); }
  int64_t run(int64_t cycles);
  void runFrame();

  // depuração: chamado antes de cada instrução (ferramentas de comparação com o núcleo JS)
  static inline void (*traceHook)(GBA&) = nullptr;

  std::vector<uint8_t> exportSave();  // dados do save + ajuste do relógio ("RTC1"), como web/emu-core.js
  void importSave(const std::vector<uint8_t>& bytes);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

private:
  void sync(StateIO& io);
};

class GBACore : public Core {
public:
  GBACore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios = nullptr);
  GBA& gba() { return *g_; }

  std::string system() const override { return "gba"; }
  std::string title() const override { return g_->title; }
  void runFrame() override { g_->runFrame(); }
  void reset() override { g_->reset(); }
  void setKeys(uint32_t mask) override { g_->setKeys(static_cast<int>(mask & 0x3FF)); }
  const uint32_t* framebuffer() const override { return g_->ppu.fb; }
  int width() const override { return 240; }
  int height() const override { return 160; }
  double fps() const override { return 16777216.0 / (228 * 1232); }
  void setSampleRate(int rate) override { g_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return g_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { g_->apu.pull(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return g_->bus.save->dirty; }
  std::vector<uint8_t> exportSave() override { return g_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { g_->importSave(b); }
  std::vector<uint8_t> saveState() override { return g_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { g_->loadState(s); }

private:
  std::unique_ptr<GBA> g_;
};

}  // namespace indigo::gba
