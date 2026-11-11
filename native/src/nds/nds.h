// Núcleo do Nintendo DS (ARM9 + ARM7, duas engines 2D, engine 3D, som de 16 canais, toque, RTC, cartucho).
//
// Parte sem BIOS: "direct boot" (os binários ARM9/ARM7 do cabeçalho vão direto para a memória, como o firmware
// faria) e as funções SWI da BIOS em alto nível (hle.cpp). A BIOS do usuário é opcional (só as chamadas SWI
// passam a usar o código dela). Os dados de usuário do firmware (nome, calibração do toque, idioma) são gerados
// aqui; nenhum firmware real é usado.
//
// Referência de hardware: GBATEK (Martin Korth), documentação pública de engenharia reversa.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "arm.h"

namespace indigo::nds {

constexpr int SCREEN_W = 256, SCREEN_H = 192;
constexpr int LINE_CYCLES = 2130;   // ciclos do ARM7 (33,51 MHz) por linha
constexpr int LINES = 263;
constexpr int HBLANK_AT = 1536;
constexpr double ARM7_HZ = 33513982.0;
constexpr int MAIN_RAM = 4 * 1024 * 1024;

bool isNdsRom(const std::vector<uint8_t>& rom, const std::string& lowerName);
// BIOS do usuário (ARM9 4 KB ou ARM7 16 KB, conferidas pelo CRC32): "" se não for
std::string ndsBiosKind(const std::vector<uint8_t>& data);

class NDS;

// ------------------------------------------------------------------ barramentos das duas CPUs
class Bus9 final : public ArmBus {
public:
  explicit Bus9(NDS& s) : s(s) {}
  uint32_t read32(uint32_t a) override;
  uint32_t read16(uint32_t a) override;
  uint32_t read8(uint32_t a) override;
  void write32(uint32_t a, uint32_t v) override;
  void write16(uint32_t a, uint32_t v) override;
  void write8(uint32_t a, uint32_t v) override;
  const uint8_t* codePage(uint32_t a, uint32_t& lo, uint32_t& hi, uint32_t& mask) override;
  bool swi(int n, uint32_t addr) override;
  uint32_t cp15Read(uint32_t crn, uint32_t crm, uint32_t op2) override;
  void cp15Write(uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v) override;
  NDS& s;
};

class Bus7 final : public ArmBus {
public:
  explicit Bus7(NDS& s) : s(s) {}
  uint32_t read32(uint32_t a) override;
  uint32_t read16(uint32_t a) override;
  uint32_t read8(uint32_t a) override;
  void write32(uint32_t a, uint32_t v) override;
  void write16(uint32_t a, uint32_t v) override;
  void write8(uint32_t a, uint32_t v) override;
  const uint8_t* codePage(uint32_t a, uint32_t& lo, uint32_t& hi, uint32_t& mask) override;
  bool swi(int n, uint32_t addr) override;
  uint8_t* wifiRam(uint32_t a);
  NDS& s;
};

// ------------------------------------------------------------------ periféricos de cada CPU
struct Timer {
  uint16_t reload = 0, ctrl = 0;
  uint32_t counter = 0;  // 16 bits
  uint32_t sub = 0;      // ciclos acumulados do prescaler
};

struct Dma {
  uint32_t sad = 0, dad = 0;
  uint32_t cnt = 0;  // palavra de controle (bits 0-20 contagem, 21-31 controle)
  uint32_t src = 0, dst = 0, count = 0;
  bool running = false;
};

struct Fifo32 {
  uint32_t d[16] = {};
  int r = 0, n = 0;
  uint32_t last = 0;
  bool full() const { return n == 16; }
  bool empty() const { return n == 0; }
  void push(uint32_t v) { if (n < 16) { d[(r + n) & 15] = v; n++; } }
  uint32_t pop() { if (n) { last = d[r]; r = (r + 1) & 15; n--; } return last; }
  void clear() { r = n = 0; }
};

struct CpuIo {
  uint32_t ie = 0, ifr = 0, ime = 0;
  uint16_t dispstat = 0;
  Timer tm[4];
  Dma dma[4];
  uint32_t dmaFill[4] = {};
  uint16_t ipcSync = 0, ipcFifoCnt = 0x0101;
  Fifo32 recv;           // o que esta CPU recebe
  uint16_t keycnt = 0;
  bool hle = true;       // SWI em alto nível
  bool intrWaitArmed = false;  // IntrWait já descartou as flags (segunda passada não descarta)
};

// ------------------------------------------------------------------ vídeo 2D (engine A ou B)
class Gpu2D {
public:
  Gpu2D(NDS& s, bool a) : s(s), isA(a) {}
  NDS& s;
  bool isA;
  uint16_t reg[0x38] = {};  // 0x04000000..0x0400006F (ou 0x04001000.. para a engine B)
  int32_t refX[2] = {}, refY[2] = {};  // referência interna das BGs afins (BG2, BG3)
  uint16_t line[SCREEN_W] = {};        // linha final (BGR555, antes do brilho mestre)

  bool capturing = false;

  void reset();
  uint32_t dispcnt() const { return reg[0] | (uint32_t(reg[1]) << 16); }
  void writeReg(uint32_t off, uint32_t v, int size);
  uint32_t readReg(uint32_t off, int size) const;
  void latchAffine(int bg);
  void vblank() { latchAffine(2); latchAffine(3); }
  void renderLine(int y);
  void sync(StateIO& io);

private:
  // pixel de camada: bits 0-14 cor, bit 15 opaco; para o 3D, alfa em alpha3d
  uint16_t bgLine[4][SCREEN_W] = {};
  uint8_t alpha3d[SCREEN_W] = {};
  uint16_t objColor[SCREEN_W] = {};
  uint8_t objPrio[SCREEN_W] = {}, objMode[SCREEN_W] = {}, objAlpha[SCREEN_W] = {}, objWin[SCREEN_W] = {};
  bool objOpaque[SCREEN_W] = {};
  void renderText(int bg, int y);
  void renderAffine(int bg, int y);
  void renderExtended(int bg, int y);
  void renderLarge(int bg, int y);
  void render3d(int y);
  void renderObjs(int y);
  void compose(int y);
  void capture(int y, const uint16_t* srcA2d);
  uint8_t bgVram8(uint32_t a) const;
  uint16_t bgVram16(uint32_t a) const;
  uint8_t objVram8(uint32_t a) const;
  uint16_t objVram16(uint32_t a) const;
  uint16_t pal(int idx) const;
  uint16_t objPal(int idx) const;
  uint16_t bgExtPal(int slot, int idx) const;
  uint16_t objExtPal(int idx) const;
};

// ------------------------------------------------------------------ vídeo 3D
struct Vertex {
  int32_t x = 0, y = 0, z = 0, w = 0;  // coordenadas de recorte (20.12)
  int32_t r = 0, g = 0, b = 0;          // cor 0..31 (5 bits) << 4
  int32_t s = 0, t = 0;                 // textura (12.4)
  // tela (depois do recorte)
  float fx = 0, fy = 0, fz = 0, fw = 1;
};

struct Polygon {
  uint16_t vtx[10] = {};
  uint8_t nv = 0;
  uint32_t attr = 0, texParam = 0, palBase = 0;
  bool translucent = false;
  float ymin = 0, ymax = 0;
};

class Gpu3D {
public:
  explicit Gpu3D(NDS& s) : s(s) {}
  NDS& s;
  // saída da última renderização: BGR555 + bit 15 = pixel desenhado; alfa 0..31 em alpha
  uint16_t color[SCREEN_W * SCREEN_H] = {};
  uint8_t alpha[SCREEN_W * SCREEN_H] = {};
  uint16_t disp3dcnt = 0;
  uint8_t regs[0xA0] = {};        // 0x04000320..0x040003BF (cor de borda, névoa, toon, limpeza...)
  bool swapPending = false;
  uint32_t swapParam = 0;
  uint32_t regsGx_ = 0;           // bits 30-31 do GXSTAT (condição da IRQ da FIFO)
  uint64_t rendered = 0;          // quadros 3D desenhados

  void reset();
  void writeFifo(uint32_t v);                 // 0x04000400 (comandos empacotados)
  void writeCmd(int cmd, uint32_t param);     // 0x04000440..0x040005FF
  uint32_t readReg(uint32_t a);               // GXSTAT, resultados
  void writeGxstat(uint32_t v);
  void gxIrq();                                // IRQ por nível da FIFO (também depois que o jogo limpa o IF)
  void vblank();                               // troca as listas e desenha
  void render();
  void sync(StateIO& io);
  size_t polyCount() const { return polys_[cur_ ^ 1].size(); }

private:
  void fifoWord(uint32_t v);
  void portCmd(int cmd, uint32_t param);
  void exec(int cmd, const uint32_t* p);
  void mtxLoad(int32_t* dst, const int32_t* m);
  void mtxMult(int32_t* dst, const int32_t* m);  // dst = m * dst
  void curMtxMult(const int32_t* m, bool scale = false);
  void curMtxLoad(const int32_t* m);
  void updateClip();
  void addVertex();
  void submit(const Vertex* v, int n);
  void lighting(uint32_t normal);
  void rasterize(const Polygon& p, const std::vector<Vertex>& vs, int idx);
  uint32_t texel(uint32_t texParam, uint32_t palBase, int s, int t, int& a);
  uint8_t texByte(uint32_t a) const;
  uint16_t palHalf(uint32_t a) const;
  void finishFrame();

  // FIFO empacotado e portas diretas
  uint32_t packed_ = 0;
  int need_ = 0, got_ = 0, cmd_ = 0;
  uint32_t par_[32] = {};
  int portNeed_ = 0, portGot_ = 0, portCmd_ = -1;
  uint32_t portPar_[32] = {};
  std::vector<uint64_t> queued_;   // comandos que chegaram depois do SWAP_BUFFERS (rodam na troca)

  // matrizes (20.12): projeção, posição, direção (vetor), textura, recorte
  int32_t proj_[16] = {}, pos_[16] = {}, vec_[16] = {}, tex_[16] = {}, clip_[16] = {};
  int32_t projStack_[16] = {}, posStack_[32][16] = {}, vecStack_[32][16] = {}, texStack_[16] = {};
  int projSp_ = 0, posSp_ = 0, texSp_ = 0;
  int mtxMode_ = 0;
  bool stackErr_ = false, clipDirty_ = true;

  // estado de vértice/polígono
  int32_t vx_ = 0, vy_ = 0, vz_ = 0;
  int32_t cr_ = 0, cg_ = 0, cb_ = 0;    // cor atual (5 bits << 4)
  int32_t ts_ = 0, tt_ = 0;             // coordenada de textura (12.4)
  uint32_t polyAttr_ = 0, polyAttrPending_ = 0, texParam_ = 0, palBase_ = 0;
  uint32_t diffAmb_ = 0, specEmi_ = 0;
  uint8_t shininess_[128] = {};
  int32_t lightVec_[4][3] = {};
  uint32_t lightColor_[4] = {};
  int primType_ = 0;
  bool inPrim_ = false;
  Vertex strip_[4];
  int stripN_ = 0, stripCount_ = 0;
  int vpX1_ = 0, vpY1_ = 0, vpX2_ = 255, vpY2_ = 191;
  int32_t posResult_[4] = {}, vecResult_[3] = {};
  bool boxResult_ = false;

  // listas (uma em montagem, outra pronta para desenhar)
  std::vector<Vertex> verts_[2];
  std::vector<Polygon> polys_[2];
  int cur_ = 0;
  bool sortManual_ = false, wbuf_ = false;

  // buffers do rasterizador
  std::vector<float> depth_;
  std::vector<uint8_t> polyId_, flags_;   // flags: bit0 névoa, bit1 translúcido, bit2 borda
};

// ------------------------------------------------------------------ som (16 canais) no ARM7
struct SoundChannel {
  uint32_t cnt = 0, sad = 0, len = 0;
  uint16_t tmr = 0, pnt = 0;
  bool on = false;
  uint32_t pos = 0;        // posição em amostras (PCM) ou nibbles (ADPCM)
  uint32_t timer = 0;      // contador de 16 bits << 0 (avança 512 por amostra de saída... ver spu.cpp)
  int32_t sample = 0;
  int32_t adpcmVal = 0, adpcmIdx = 0, loopVal = 0, loopIdx = 0;
  uint16_t lfsr = 0x7FFF;
};

class Spu {
public:
  explicit Spu(NDS& s) : s(s) {}
  NDS& s;
  SoundChannel ch[16];
  uint16_t soundcnt = 0, bias = 0x200;
  uint8_t capCnt[2] = {};
  uint32_t capDad[2] = {}, capLen[2] = {};

  void reset();
  void write(uint32_t a, uint32_t v, int size);
  uint32_t read(uint32_t a, int size) const;
  void run(int cycles);  // avança os ciclos do ARM7
  void setSampleRate(int rate) { hostRate_ = rate; }
  size_t available() const { return (wpos_ + BUF - rpos_) % BUF; }
  void pull(float* l, float* r, size_t n);
  void sync(StateIO& io);

private:
  void start(int i);
  void mix();          // gera uma amostra a 32768 Hz
  int64_t cyc_ = 0;    // ciclos acumulados até a próxima amostra
  int hostRate_ = 48000;
  double resamp_ = 0;
  int32_t lastL_ = 0, lastR_ = 0;
  static constexpr size_t BUF = 16384;
  std::vector<float> bufL_ = std::vector<float>(BUF), bufR_ = std::vector<float>(BUF);
  size_t wpos_ = 0, rpos_ = 0;
};

// ------------------------------------------------------------------ cartucho (ROM + save por SPI)
class Cart {
public:
  explicit Cart(NDS& s) : s(s) {}
  NDS& s;
  std::vector<uint8_t> rom;
  uint32_t romMask = 0;
  uint32_t chipId = 0;
  uint16_t auxspicnt = 0;
  uint32_t romctrl = 0;
  uint8_t cmd[8] = {};
  // transferência de dados em curso
  uint32_t xferAddr = 0, xferLeft = 0, xferPos = 0;
  int xferKind = 0;  // 0 nada, 1 dados, 2 ID, 3 cabeçalho, 4 só FF
  uint32_t dataLatch = 0;

  // save (EEPROM / FLASH / FRAM por SPI)
  std::vector<uint8_t> save;
  int addrBytes = 0;     // 0 = tipo ainda não detectado, 1, 2 ou 3
  bool dirty = false;
  uint8_t spiOut = 0xFF;
  bool csHold = false;
  int spiCmd = 0, spiPos = 0;
  uint32_t spiAddr = 0;
  uint8_t status = 0;     // bit 1 = escrita permitida
  std::vector<uint8_t> probe;  // bytes do primeiro comando enquanto o tipo não é conhecido
  // chip de infravermelho na frente da FLASH (Pokémon HeartGold/SoulSilver, Black/White, Black 2/White 2):
  // o primeiro byte de cada conversa é do IR; 0x00 repassa o resto para a FLASH
  bool ir = false;
  int irPos = 0, irCmd = 0;
  void syncIr(StateIO& io);  // infravermelho e sondagem (fica no fim do estado)

  void load(const std::vector<uint8_t>& data);
  void setSave(const std::vector<uint8_t>& data);
  void writeRomctrl(uint32_t v);
  uint32_t readData();
  void writeAuxSpiCnt(uint16_t v);
  void writeAuxSpiData(uint8_t v);
  void sync(StateIO& io);

private:
  void finishProbe();
  void resizeFor(int ab);
  uint8_t spiByte(uint8_t v);
};

// ------------------------------------------------------------------ SPI do ARM7: energia, firmware, toque
class Spi {
public:
  explicit Spi(NDS& s) : s(s) {}
  NDS& s;
  uint16_t cnt = 0;
  uint8_t data = 0;
  std::vector<uint8_t> firmware;  // 256 KB gerados (dados de usuário próprios)
  // firmware
  int fwCmd = 0, fwPos = 0;
  uint32_t fwAddr = 0;
  uint8_t fwStatus = 0;
  // toque
  int tscCmd = 0;
  uint16_t tscVal = 0;
  int tscPos = 0;
  // energia
  uint8_t pmReg[8] = {};
  int pmPos = 0, pmIdx = 0;
  bool pmRead = false;

  void buildFirmware();
  void writeCnt(uint16_t v);
  void writeData(uint8_t v);
  void sync(StateIO& io);
};

class Rtc {
public:
  uint8_t pins = 0;       // último valor escrito em 0x04000138
  int bits = 0, byteIdx = 0;
  uint8_t cur = 0, cmd = 0;
  bool haveCmd = false;
  uint8_t data[8] = {};
  int len = 0;
  uint8_t stat1 = 0x02, stat2 = 0;  // 24 horas
  int64_t offsetSec = 0;
  void write(uint16_t v);
  uint16_t read() const;
  void sync(StateIO& io);

private:
  void command();
  void fillTime(bool date);
  bool outBit_ = false;
};

// ------------------------------------------------------------------ console
class NDS {
public:
  NDS();
  ~NDS();

  Bus9 bus9{*this};
  Bus7 bus7{*this};
  Arm arm9{bus9, true};
  Arm arm7{bus7, false};
  CpuIo io9, io7;
  Gpu2D gpuA{*this, true}, gpuB{*this, false};
  Gpu3D gpu3d{*this};
  Spu spu{*this};
  Cart cart{*this};
  Spi spi{*this};
  Rtc rtc;

  std::vector<uint8_t> mainRam, wram, wram7, vram, pal, oam, itcm, dtcm, bios9, bios7, wifiRam;
  // CP15
  uint32_t cpCtrl = 0x00052078, dtcmReg = 0x0080000A, itcmReg = 0x00000020;
  uint32_t dtcmBase = 0x00800000, dtcmSize = 0x4000, itcmSize = 0x02000000;
  uint32_t cpRegion[8] = {}, cpMisc[8] = {};
  uint32_t mapGen = 1;

  // controle de memória
  uint8_t vramcnt[9] = {};
  uint8_t wramcnt = 3;
  uint16_t exmemcnt = 0x6000, exmemstat = 0;
  uint16_t powcnt1 = 0x820F, powcnt2 = 1;
  uint8_t postflg9 = 1, postflg7 = 1, haltcnt = 0;
  uint16_t keys = 0x3FF, extkeys = 0x7F;
  int touchX = 0, touchY = 0;
  bool touchDown = false;
  uint16_t vcount = 0;
  // divisor e raiz
  uint16_t divcnt = 0, sqrtcnt = 0;
  int64_t divNumer = 0, divDenom = 0, divResult = 0, divRem = 0;
  uint64_t sqrtParam = 0;
  uint32_t sqrtResult = 0;
  uint32_t biosProt = 0;

  // páginas da VRAM (16 KB) por região; -1 = nada mapeado. Valor = deslocamento em vram
  int32_t pgBgA[32], pgObjA[16], pgBgB[8], pgObjB[8], pgArm7[2 * 8], pgTex[32], pgTexPal[8];
  int32_t pgBgExtA[4], pgObjExtA, pgBgExtB[4], pgObjExtB;  // páginas de 8 KB
  void remapVram();
  uint32_t vramOffset(uint32_t a) const;  // deslocamento em vram ou ~0u (endereço do ARM9)

  int64_t frameCount = 0;
  uint32_t fb[SCREEN_W * SCREEN_H * 2] = {};
  std::string titleStr, gameCode;
  bool loaded = false;

  void loadRom(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  void reset();
  void runFrame();
  void setKeys(uint32_t mask);
  void setTouch(int x, int y, bool down);

  // E/S
  uint32_t io9Read(uint32_t a, int size);
  void io9Write(uint32_t a, uint32_t v, int size);
  uint32_t io7Read(uint32_t a, int size);
  void io7Write(uint32_t a, uint32_t v, int size);
  void irq9(uint32_t bits) { io9.ifr |= bits; updateIrq(); }
  void irq7(uint32_t bits) { io7.ifr |= bits; updateIrq(); }
  void updateIrq();
  void ipcFifoUpdate();
  void dmaStart(int cpu, int mode);   // dispara canais esperando pelo modo (vblank, hblank...)
  void runDma(int cpu, int ch);
  int dmaDepth_ = 0;  // níveis de runDma em andamento (não vai para o estado: é zero entre instruções)
  void cartDataReady();
  void cartXferDone();
  void gxFifoDma();
  void writeVramcnt(int bank, uint8_t v);
  void writeWramcnt(uint8_t v);
  void updateTcm();
  // memória vista pelo ARM9 / ARM7 sem efeitos de E/S (cópias de boot, HLE)
  uint8_t* wramPtr9(uint32_t a);
  uint8_t* wramPtr7(uint32_t a);

  // HLE da BIOS
  bool hleSwi(int cpu, int n, uint32_t addr);
  uint32_t callGuest(int cpu, uint32_t fn, uint32_t a0, uint32_t a1 = 0, uint32_t a2 = 0);  // chama uma rotina do jogo
  bool guestStuck() const { return guestStuck_; }  // alguma rotina do jogo não voltou: a SWI desiste

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // tempo
  int64_t now7 = 0;    // ciclos do ARM7 desde o início
  int lineCycle = 0;
  int64_t cyc9 = 0, cyc7 = 0;  // créditos de ciclo de cada CPU dentro da fatia

private:
  void directBoot();
  void runCycles(int cycles);
  void tickTimers(CpuIo& io, int cpu, int cycles);
  void timerOverflow(CpuIo& io, int cpu, int i);
  void startLine(int line);
  void hblank(int line);
  void finishFrame();
  void sync(StateIO& io);
  Arm& cpu(int c) { return c ? arm7 : arm9; }
  CpuIo& cio(int c) { return c ? io7 : io9; }
  uint32_t readTimer(CpuIo& io, int i) const;
  void writeTimerCtrl(CpuIo& io, int cpu, int i, uint16_t v);
  void writeDmaCnt(int cpu, int ch, uint32_t v);
  void divide();
  void squareRoot();
  uint32_t ipcRead(int cpu, uint32_t a, int size);
  void ipcWrite(int cpu, uint32_t a, uint32_t v, int size);
  int guestDepth_ = 0;
  int64_t guestBudget_ = 0;  // passos restantes para as rotinas do jogo dentro de uma SWI
  bool guestStuck_ = false;
};

class NDSCore : public Core {
public:
  NDSCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  NDS& nds() { return *s_; }

  std::string system() const override { return "nds"; }
  std::string title() const override { return s_->titleStr; }
  void runFrame() override { s_->runFrame(); }
  void reset() override { s_->reset(); }
  void setKeys(uint32_t mask) override { s_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return s_->fb; }
  int width() const override { return SCREEN_W; }
  int height() const override { return SCREEN_H * 2; }
  double fps() const override { return ARM7_HZ / (LINE_CYCLES * LINES); }
  void setSampleRate(int rate) override { s_->spu.setSampleRate(rate); }
  size_t audioAvailable() const override { return s_->spu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { s_->spu.pull(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return s_->cart.dirty; }
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& b) override;
  void setPointer(int x, int y, bool down) override;
  std::string note() const override;
  std::vector<uint8_t> saveState() override { return s_->saveState(); }
  void loadState(const std::vector<uint8_t>& st) override { s_->loadState(st); }
  void followHostClock() override { s_->rtc.offsetSec = 0; }

private:
  std::unique_ptr<NDS> s_;
};

}  // namespace indigo::nds
