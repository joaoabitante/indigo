// Núcleo SNES / Super Famicom (NTSC). Peças:
//   cpu.h     65816 (5A22) com o tempo de cada acesso (6, 8 ou 12 ciclos do relógio mestre de 21,477 MHz)
//   spc700.h  processador de som; apu.cpp junta a RAM de 64 KB, o IPL, os timers e o DSP (8 vozes BRR, eco)
//   ppu.cpp   vídeo por linha: modos 0–7, sprites, janelas, soma/subtração de cores, mosaico, Mode 7
//   bus.cpp   mapa de memória (LoROM, HiROM, ExHiROM), WRAM, registradores, multiplicação/divisão, DMA, HDMA,
//             contadores H/V, NMI, IRQ, leitura automática dos controles
// Núcleo próprio do Índigo nativo (a versão web não tem SNES): a régua são os SingleStepTests e as ROMs de teste
// públicas (PeterLemon, undisbeliever).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "cpu.h"
#include "dsp1.h"
#include "sa1.h"
#include "sdd1.h"
#include "spc7110.h"
#include "gsu.h"
#include "spc700.h"

namespace indigo::snes {

// ------------------------------------------------------------------ cartucho
struct Cart {
  enum Map { LOROM, HIROM, EXHIROM };
  std::vector<uint8_t> rom, sram;
  Map map = LOROM;
  bool fastRom = false;
  std::string title;
  int chip = 0;  // tipo do cartucho ($FFD6)
  bool superfx = false;  // Super FX: sram é a RAM do cartucho (de 32 a 128 KB)
  enum DspMap { DSP_NONE, DSP_LOROM_SMALL, DSP_LOROM_LARGE, DSP_HIROM };
  DspMap dsp1 = DSP_NONE;  // DSP-1 e onde ficam os registradores dele
  bool sa1 = false;      // SA-1: sram é a BW-RAM
  bool sdd1 = false;     // S-DD1: MMC de $C0-$FF e descompressão no DMA
  bool spc7110 = false;  // SPC7110: o mapa de memória é todo do chip
  bool epsonRtc = false; // com o relógio RTC-4513 (Far East of Eden Zero)
  bool battery = true;   // a RAM do cartucho é gravada como save
};
bool isSnesRom(const std::vector<uint8_t>& data, const std::string& fileName);
bool parseRom(std::vector<uint8_t> data, Cart& out, std::string& error);
size_t romMirror(size_t addr, size_t size);  // espelha ROMs com tamanho que não é potência de 2

// ------------------------------------------------------------------ DSP de som
class DSP {
public:
  explicit DSP(uint8_t* aram) : ram(aram) {}
  void reset();
  void write(uint8_t addr, uint8_t v);
  uint8_t read(uint8_t addr) const { return regs[addr & 0x7F]; }
  void sample(int16_t& left, int16_t& right);  // uma amostra (32 kHz)
  void sync(StateIO& io);

private:
  struct Voice {
    uint16_t brrAddr = 0;
    int brrOffset = 1;
    int16_t buf[12] = {};
    int bufPos = 0;
    int interp = 0;
    int env = 0;
    int envMode = 3;  // 0 ataque, 1 decaimento, 2 sustentação, 3 soltura
    int konDelay = 0;
    int hiddenEnv = 0;
    int out = 0;
  };
  void decodeBrr(Voice& v, int n);
  void runEnvelope(Voice& v, int n);
  bool rateTick(int rate) const;
  int voiceOutput(Voice& v, int n, int pitchModIn);

  uint8_t* ram;
  uint8_t regs[128] = {};
  Voice voice[8];
  int counter = 0;
  int noise = 0x4000;
  int echoPos = 0, echoLength = 0;
  int16_t echoHist[2][8] = {};
  int echoHistPos = 0;
  uint8_t konLatch = 0, koffLatch = 0;
  bool everyOther = false;
};

// ------------------------------------------------------------------ S-SMP (SPC700 + 64 KB + timers + DSP)
class APU : public SpcBus {
public:
  APU();
  void reset();
  void runCycles(int64_t n);  // anda n ciclos do SPC700 (1,024 MHz)
  uint8_t portOut(int i) const { return out[i & 3]; }  // lido pela CPU em $2140–$2143
  void portIn(int i, uint8_t v) { in[i & 3] = v; }      // gravado pela CPU
  void setSampleRate(int rate);
  size_t available() const { return (wpos + bufSize - rpos) % bufSize; }
  void pull(float* l, float* r, size_t n);
  void sync(StateIO& io);

  uint8_t read(uint16_t a) override;
  void write(uint16_t a, uint8_t v) override;
  void idle() override { cycle(); }

  uint8_t ram[0x10000] = {};

private:
  void cycle();
  void emit(int16_t l, int16_t r);
  SPC700 smp;
  DSP dsp;
  int64_t debt = 0;  // ciclos ainda a andar (negativo: o SPC700 passou do ponto)
  uint8_t in[4] = {}, out[4] = {};
  uint8_t control = 0xB0, dspAddr = 0, test = 0x0A;
  struct Timer { uint8_t target = 0, stage = 0, outv = 0; bool on = false; };
  Timer timer[3];
  int div128 = 0, div16 = 0, dspDiv = 0;
  // saída (32 kHz → taxa do aparelho)
  static constexpr size_t bufSize = 16384;
  std::vector<float> bufL = std::vector<float>(bufSize), bufR = std::vector<float>(bufSize);
  size_t wpos = 0, rpos = 0;
  double step = 32000.0 / 48000.0, phase = 0;
  float lastL = 0, lastR = 0;
};

// ------------------------------------------------------------------ PPU
class PPU {
public:
  void reset();
  void write(uint8_t reg, uint8_t v);
  uint8_t read(uint8_t reg, uint8_t mdr);
  void renderLine(int y);  // linha visível 1–224
  void vblankStart();
  void latchCounters(int h, int v) { hLatch = static_cast<uint16_t>(h); vLatch = static_cast<uint16_t>(v); counterLatched = true; }
  bool forceBlank() const { return inidisp & 0x80; }
  bool overscan() const { return setini & 4; }
  void sync(StateIO& io);
  void setRendering(bool r) { rendering = r; }

  uint32_t fb[256 * 224] = {};
  // alta resolução (modos 5 e 6): colunas pares, que vão para a tela secundária
  uint16_t hiresColor[4][256] = {};
  uint8_t hiresPrio[4][256] = {};
  bool hiresOpaque[4][256] = {};
  uint8_t vram[0x10000] = {};
  uint16_t cgram[256] = {};
  uint8_t oam[544] = {};
  bool field = false;

private:
  struct Layer { uint16_t color[256]; uint8_t prio[256]; bool opaque[256]; bool mathOk[256]; };
  void renderBg(int bg, int y, int bpp, Layer& out);
  void renderMode7(int y, Layer& bg1, Layer& bg2);
  void renderSprites(int y, Layer& out);
  void windowMask(int layer, bool mask[256]) const;
  uint16_t vramWord(uint16_t addr) const { return static_cast<uint16_t>(vram[(addr & 0x7FFF) * 2] | (vram[(addr & 0x7FFF) * 2 + 1] << 8)); }
  uint16_t vramAddrRemap() const;
  void vramPrefetch();

  uint8_t inidisp = 0x80, obsel = 0, bgmode = 0, mosaic = 0, bgsc[4] = {}, bgnba[2] = {};
  uint16_t hofs[4] = {}, vofs[4] = {};
  uint8_t ofsPrev = 0, ofsPrev2 = 0;
  uint8_t vmain = 0;
  uint16_t vaddr = 0, vlatch = 0;
  uint8_t m7sel = 0, m7prev = 0;
  int16_t m7a = 0, m7b = 0, m7c = 0, m7d = 0, m7x = 0, m7y = 0, m7hofs = 0, m7vofs = 0;
  uint16_t oamAddr = 0, oamAddrReload = 0;
  uint8_t oamLatch = 0;
  bool oamPriority = false;
  uint16_t cgAddr = 0;
  uint8_t cgLatch = 0;
  uint8_t wsel[3] = {}, wpos[4] = {}, wbglog = 0, wobjlog = 0, tm = 0, ts = 0, tmw = 0, tsw = 0;
  uint8_t cgwsel = 0, cgadsub = 0, setini = 0;
  uint16_t fixedColor = 0;
  uint16_t hLatch = 0, vLatch = 0;
  bool counterLatched = false, hFlip = false, vFlip = false;
  uint8_t ppu1Mdr = 0, ppu2Mdr = 0;
  bool timeOver = false, rangeOver = false;
  bool rendering = false;
  int mosaicStartY = 1;
};

// ------------------------------------------------------------------ máquina
class SNES : public CpuBus {
public:
  explicit SNES(Cart cart);
  Cart cart;
  CPU cpu;
  PPU ppu;
  APU apu;
  bool dirty = false;
  std::unique_ptr<GSU> gsu;  // só nos cartuchos com Super FX
  std::unique_ptr<Dsp1> dsp;  // só nos cartuchos com DSP-1
  std::unique_ptr<SA1> sa1;   // só nos cartuchos com SA-1
  std::unique_ptr<SDD1> sdd1; // só nos cartuchos com S-DD1
  std::unique_ptr<SPC7110> spc7110;  // só nos cartuchos com SPC7110

  void reset(bool power);
  void runFrame();
  void setKeys(uint32_t mask);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);
  uint8_t peek(uint32_t addr);  // sem efeito colateral (testes)
  void followHostClock() { if (spc7110) spc7110->followHostClock(); }

  uint8_t read(uint32_t addr) override;
  void write(uint32_t addr, uint8_t v) override;
  void idle(uint32_t addr) override { tick(6); }

private:
  int speed(uint32_t addr) const;
  void tick(int clocks);
  void newLine();
  void syncApu();
  uint8_t busRead(uint32_t addr, bool dmaA);  // sem tempo
  void busWrite(uint32_t addr, uint8_t v, bool dmaA);
  uint8_t cartRead(uint32_t addr, bool& hit);
  int dspPort(uint32_t addr) const;  // -1 fora; 0 = DR; 1 = SR
  bool cartWrite(uint32_t addr, uint8_t v);
  uint8_t ioRead(uint16_t a);
  void ioWrite(uint16_t a, uint8_t v);
  void runDma(uint8_t channels);
  int hdmaInit();  // devolvem os ciclos gastos
  int hdmaRun();
  uint8_t dmaRead(uint32_t addr) { return busRead(addr, true); }
  void updateIrq();
  void checkHvIrq(int fromH, int toH);
  void autoJoypad();
  void sync(StateIO& io);

  uint8_t wram[0x20000] = {};
  uint32_t wramAddr = 0;
  uint8_t mdr = 0;
  // relógio
  int hc = 0, vc = 0;          // ciclos do mestre na linha (0–1363) e linha (0–261)
  int vblankLine = 225;        // 240 com overscan
  bool refreshed = false, rendered = false, hdmaDone = false;
  bool frameDone = false;
  int64_t apuDebt = 0;         // (ciclos do mestre × 1.024.000) ainda não entregues ao SPC700
  // registradores da CPU ($4200–$421F)
  uint8_t nmitimen = 0, wrio = 0xFF, memsel = 0, hdmaen = 0;
  uint8_t wrmpya = 0xFF, wrdivl = 0xFF, wrdivh = 0xFF;
  uint16_t htime = 0x1FF, vtime = 0x1FF, rddiv = 0, rdmpy = 0;
  bool nmiFlag = false, irqFlag = false, nmiLine = false, autoJoyBusy = false;
  int autoJoyTimer = 0;
  uint16_t joy[4] = {};
  uint16_t pad = 0;
  uint8_t joyLatch = 0;
  uint32_t joyShift[2] = {};
  // DMA / HDMA
  struct Channel {
    uint8_t ctrl = 0xFF, bbad = 0xFF, a1b = 0xFF, dasb = 0xFF, ntrl = 0xFF, unused = 0xFF;
    uint16_t a1t = 0xFFFF, das = 0xFFFF, a2a = 0xFFFF;
    bool doTransfer = false, done = false;
    bool dmaEnable = false;  // DMA comum em andamento (o HDMA no mesmo canal interrompe)
  };
  Channel ch[8];
};

class SNESCore : public Core {
public:
  SNESCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  SNES& snes() { return *m_; }

  std::string system() const override { return "snes"; }
  std::string title() const override { return m_->cart.title; }
  std::string note() const override {
    if (!m_->dsp) return "";
    return m_->dsp->exact() ? " · DSP-1 com o arquivo do chip" : " · DSP-1 imitado (sem o arquivo do chip)";
  }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(false); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->ppu.fb; }
  int width() const override { return 256; }
  int height() const override { return 224; }
  double fps() const override { return 21477272.0 / (1364.0 * 262 - 2); }  // 60,0988
  void setSampleRate(int rate) override { m_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->apu.pull(l, r, n); }
  bool hasSave() const override { return m_->cart.battery && !m_->cart.sram.empty(); }
  bool saveDirty() const override { return m_->dirty; }
  std::vector<uint8_t> exportSave() override { m_->dirty = false; return m_->cart.sram; }
  void importSave(const std::vector<uint8_t>& b) override;
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }
  void followHostClock() override { m_->followHostClock(); }

private:
  std::unique_ptr<SNES> m_;
};

}  // namespace indigo::snes
