// Sega Dreamcast (experimental). Escrito a partir da documentação pública do hardware (manual do SH7750 da Hitachi,
// documentos de engenharia reversa da comunidade sobre Holly, PowerVR2 CLX2, AICA, GD-ROM e Maple):
//   sh4.cpp    CPU Hitachi SH-4 (SH7091, 200 MHz): instruções, FPU com bancos FR/XF, FIPR/FTRV/FSCA/FSRRA,
//              exceções e interrupções; periféricos internos em sh4io.cpp (TMU, INTC, DMAC, store queues, cache
//              como RAM, SCIF, BSC, CPG). Sem MMU: a tradução de endereços (MMUCR.AT) não é feita.
//   bus.cpp    mapa de memória, Holly (registradores SB, interrupções, DMA do canal 2, G1/G2/PVR-DMA) e Maple
//              (controle e VMU como cartão de memória).
//   pvr.cpp    PowerVR2: Tile Accelerator (listas de polígonos e vértices) e o desenho em software (profundidade,
//              Gouraud, texturas twiddled/VQ/paleta/stride, opaco, punch-through, translúcido ordenado), saída de
//              vídeo lida da VRAM e o gerador de sincronismo (SPG).
//   aica.cpp   AICA: CPU ARM7 (a ARM60 do 3DO, ARMv3, com o barramento little-endian do AICA), 64 canais PCM 16/8
//              bits e ADPCM com envelope, temporizadores, interrupções e o relógio (RTC). O DSP de efeitos não é
//              emulado.
//   gdrom.cpp  unidade de GD-ROM (ATA + pacotes SPI: TOC, sessões, leitura PIO e DMA, CD-DA, subcódigo).
//   dc.cpp     o console inteiro, save state e a interface Core.
//
// A BIOS (dc_boot.bin, 2 MB) e a flash (dc_flash.bin, 128 KB) são do console do usuário e nunca vêm com o Índigo.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"
#include "../tdo/arm60.h"

namespace indigo::dc {

constexpr uint32_t RAM_SIZE = 16u << 20, VRAM_SIZE = 8u << 20, ARAM_SIZE = 2u << 20;
constexpr uint32_t BOOT_SIZE = 2u << 20, FLASH_SIZE = 128u << 10, VMU_SIZE = 128u << 10;
constexpr double SH4_HZ = 200000000.0;

// BIOS do Dreamcast (2 MB, do usuário: dc_boot.bin). Reconhecida pelo tamanho e pelo nome (dc_boot, dc_bios,
// dcboot...), sem o "SEGA" de cartucho de Mega Drive em 0x100.
bool isDcBoot(const std::vector<uint8_t>& b, const std::string& nameLower);
// flash do Dreamcast (128 KB, dc_flash.bin): tem "KATANA_FLASH" no começo de uma partição
bool isDcFlash(const std::vector<uint8_t>& b, const std::string& nameLower);
// disco de Dreamcast: "SEGA SEGAKATANA " no começo de uma faixa de dados (o IP.BIN)
bool isDcDisc(cdrom::Disc& d);
// VMU vazio já formatado (128 KB), no formato do sistema de arquivos do VMU
std::vector<uint8_t> blankVmu();

class System;

// ================================================================================================ SH-4
class Sh4 {
public:
  explicit Sh4(System& s);
  System& sys;

  // registradores (r[0..7] do banco atual; rb = o outro banco de R0-R7)
  uint32_t r[16] = {}, rb[8] = {};
  uint32_t sr = 0x700000F0, gbr = 0, vbr = 0, ssr = 0, spc = 0, sgr = 0, dbr = 0;
  uint32_t mach = 0, macl = 0, pr = 0, pc = 0xA0000000, fpscr = 0x00040001, fpul = 0;
  uint32_t fr[16] = {}, xf[16] = {};  // banco atual (FR) e o outro (XF)
  int64_t cycles = 0;
  bool sleeping = false;

  // periféricos internos
  uint32_t mmucr = 0, ccr = 0, qacr[2] = {}, expevt = 0, intevt = 0, tra = 0, pteh = 0, ptel = 0, ttb = 0, tea = 0;
  uint16_t icr = 0, ipra = 0, iprb = 0, iprc = 0;
  uint8_t tocr = 0, tstr = 0;
  uint32_t tcor[3] = {}, tcnt[3] = {}, tcpr2 = 0;
  uint16_t tcr[3] = {};
  int64_t tmuLast_ = 0;
  uint32_t tmuFrac_[3] = {};
  uint32_t sar[4] = {}, dar[4] = {}, dmatcr[4] = {}, chcr[4] = {}, dmaor = 0;
  uint32_t bcr1 = 0, mcr = 0, pctra = 0, pctrb = 0;
  uint16_t bcr2 = 0x3FFC, wcr1 = 0x7777, wcr2 = 0xFFFE, wcr3 = 0x7777, pcr = 0, rtcsr = 0, rtcnt = 0, rtcor = 0, rfcr = 0;
  uint16_t pdtra = 0, pdtrb = 0, gpioic = 0, frqcr = 0x0E0A, wtcnt = 0;
  uint8_t stbcr = 0, stbcr2 = 0, wtcsr = 0;
  uint16_t scsmr2 = 0, scscr2 = 0, scfsr2 = 0x0060, scfcr2 = 0, scsptr2 = 0, sclsr2 = 0;
  uint8_t scbrr2 = 0xFF;
  uint32_t sq[16] = {};
  uint8_t ocram[8192] = {};
  std::string serial;  // bytes enviados pela SCIF (saída de depuração dos programas de teste)

  void reset();
  void run(int64_t until);
  void setIrl(int prio) { if (irl_ != prio) { irl_ = prio; updateIrq(); } }
  void updateIrq();
  void tmuSync();                    // atualiza os temporizadores até o ciclo atual
  int64_t tmuNextEvent() const;      // próximo estouro de temporizador (ciclo), ou muito longe
  void sync(StateIO& io);

  // acesso à memória visto pela CPU (endereços virtuais; MMU desligada)
  uint8_t rd8(uint32_t a);
  uint16_t rd16(uint32_t a);
  uint32_t rd32(uint32_t a);
  void wr8(uint32_t a, uint8_t v);
  void wr16(uint32_t a, uint16_t v);
  void wr32(uint32_t a, uint32_t v);
  uint64_t rd64(uint32_t a) { return uint64_t(rd32(a)) | uint64_t(rd32(a + 4)) << 32; }
  void wr64(uint32_t a, uint64_t v) { wr32(a, uint32_t(v)); wr32(a + 4, uint32_t(v >> 32)); }
  uint32_t p4Read(uint32_t a, int size);
  void p4Write(uint32_t a, uint32_t v, int size);

  // estado interno usado pelas instruções
  void setSr(uint32_t v);
  void setFpscr(uint32_t v);
  void exception(uint32_t code, uint32_t vecOff, uint32_t retPc);
  void delaySlot(uint32_t target);
  void prefetch(uint32_t a);
  void execOne(uint16_t op);
  int irl_ = 15;          // prioridade pedida pelo Holly (15 = nenhuma: o pino IRL vem invertido)
  int pendPrio_ = 0;      // prioridade da melhor interrupção pendente (0 = nenhuma)
  uint32_t pendCode_ = 0;
  bool inSlot_ = false;
  bool exitRun_ = false;  // pede para run() voltar (escrita que muda o agendamento)
  uint32_t illegal = 0;   // instruções ilegais encontradas (diagnóstico)
  uint32_t idleSkips = 0;
  uint64_t executed = 0;   // instruções executadas (fora os slots), para medir a velocidade  // laços de espera pulados (diagnóstico)
  void idlePoll(uint32_t a);
  uint32_t idleAddr_ = 0, idlePc_ = 0, idleN_ = 0;
  int64_t idleCyc_ = 0;
  bool idleSkip_ = false;

private:
  void checkInterrupt();
};

// ================================================================================================ PowerVR2
struct Vtx {
  float x, y, z, u, v;
  uint32_t col, ofs;  // ARGB
};
struct Poly {
  uint32_t isp, tsp, tcw, pcw;
  uint32_t first, count;  // triângulos em tris[]
  uint32_t clip[4];       // recorte do usuário em pixels (x0, y0, x1, y1), ou tudo
  uint8_t clipOut;
};
struct Tri {
  Vtx v[3];
  uint32_t poly;
};
// triângulo de volume modificador; mode = instrução de volume (0 continua, 1 fecha "dentro", 2 fecha "fora")
struct ModTri {
  float x[3], y[3], z[3];
  uint32_t mode;
};
struct TaContext {
  uint32_t base = 0xFFFFFFFF;  // PARAM_BASE (TA_ISP_BASE & 0xF00000) a que pertence
  std::vector<Poly> polys[5];  // opaco, modificador opaco, translúcido, modificador translúcido, punch-through
  std::vector<Tri> tris;
  std::vector<ModTri> mods;    // volumes modificadores da lista opaca
  void clear() { for (auto& p : polys) p.clear(); tris.clear(); mods.clear(); }
};

class Pvr {
public:
  explicit Pvr(System& s);
  System& sys;
  uint32_t regs[0x2000 / 4] = {};  // 0x005F8000-0x005F9FFF (registradores, tabela de neblina, paleta)

  void reset();
  uint32_t read(uint32_t off);
  void write(uint32_t off, uint32_t v);
  void taWrite(const uint32_t* data32);  // 32 bytes para o TA
  void taWriteWords(uint32_t a, uint32_t v);  // escrita de 32 bits avulsa na FIFO do TA
  void yuvWrite(const uint32_t* data32);      // 32 bytes para o conversor YUV
  void render();
  void drawOutput(uint32_t* fb, int& w, int& h);
  void sync(StateIO& io);
  uint32_t renders = 0, taBlocks = 0;
  uint32_t taWordBuf_[16] = {};
  uint8_t yuvBuf_[512] = {};
  uint32_t yuvHave_ = 0, yuvCount_ = 0;
  int taWordCnt_ = 0;

private:
  // estado do TA
  uint32_t taBuf_[16] = {};
  int taNeed_ = 0;     // palavras esperadas no bloco atual (8 ou 16)
  int taHave_ = 0;
  int listType_ = -1;  // lista aberta (-1 = nenhuma)
  uint32_t curPcw_ = 0, curIsp_ = 0, curTsp_ = 0, curTcw_ = 0;
  float faceCol_[4] = {1, 1, 1, 1}, faceOfs_[4] = {0, 0, 0, 0};
  uint32_t userClip_[4] = {0, 0, 0, 0};
  int vtxType_ = 0;
  bool sprite_ = false, modVol_ = false;
  Vtx strip_[2];
  int stripN_ = 0;
  bool stripParity_ = false;
  int curPoly_ = -1;
  TaContext ctx_[4];
  int curCtx_ = 0;
  void taBlock(const uint32_t* w);
  void taGlobal(const uint32_t* w);
  void taVertex(const uint32_t* w);
  void addTri(const Vtx& a, const Vtx& b, const Vtx& c);
  void newPoly();
  int ctxFor(uint32_t base, bool create);
  int blockWords(uint32_t pcw) const;

  // desenho
  std::vector<float> depth_;
  std::vector<uint32_t> color_;  // ABGR do acumulador
  std::vector<uint8_t> stencil_;
  int rw_ = 0, rh_ = 0;
  void drawTri(const Tri& t, const Poly& p, int list);
  void drawBackground();
  void applyShadows(const TaContext& c);
  uint32_t texel(const Poly& p, uint32_t tsp, uint32_t tcw, int u, int v);
  void writeFramebuffer();
};

// ================================================================================================ AICA
class Aica : public tdo::ArmBus {
public:
  explicit Aica(System& s);
  System& sys;
  tdo::Arm60 arm{*this};
  uint16_t chan[64][32] = {};  // registradores de canal (um de 16 bits a cada 4 bytes)
  uint16_t common[0x400] = {};      // 0x2000-0x2FFF (um de 16 bits a cada 4 bytes)
  uint16_t dspRegs_[0x800] = {};    // 0x3000-0x7FFF (guardados, sem efeito)
  uint32_t rtc = 0;
  bool rtcWrite_ = false;
  bool armReset = true;
  std::vector<int16_t> out;  // pares esquerdo/direito a 44,1 kHz

  struct Voice {
    bool on = false;
    uint32_t pos = 0, frac = 0;   // amostra atual e fração (16 bits)
    int env = 0x3FF, stage = 3;   // atenuação (0 = máximo, 0x3FF = mudo) e etapa (0 A, 1 D1, 2 D2, 3 R)
    int adpcmS = 0, adpcmStep = 127, loopS = 0, loopStep = 127;
    bool loopSaved = false, looped = false;
    uint32_t envFrac = 0;
  } voice[64];

  void reset();
  uint32_t regRead(uint32_t off, int size);   // off = 0..0xFFFF (visto pelo SH-4 em 0x00700000)
  void regWrite(uint32_t off, uint32_t v, int size);
  void sample();  // gera uma amostra (e anda temporizadores)
  void runArm(int cycles);
  void updateArmIrq();
  void sync(StateIO& io);
  int timerCnt_[3] = {};

  // ArmBus
  uint32_t read32(uint32_t a) override;
  uint8_t read8(uint32_t a) override;
  void write32(uint32_t a, uint32_t v) override;
  void write8(uint32_t a, uint8_t v) override;

private:
  void keyOnEx();
  int voiceSample(int ch);
  void raiseSci(uint32_t bits);
  void raiseMci(uint32_t bits);
  uint16_t& com(uint32_t off) { return common[((off - 0x2000) >> 2) & 0x3FF]; }
  int armDebt_ = 0, rtcFrac_ = 0;
};

// ================================================================================================ GD-ROM
class Gdrom {
public:
  explicit Gdrom(System& s);
  System& sys;
  cdrom::Disc* disc = nullptr;
  bool gd = false;  // disco GD (área de alta densidade a partir do LBA 45000)

  void reset();
  uint32_t read(uint32_t off, int size);
  void write(uint32_t off, uint32_t v, int size);
  void event();  // fim do tempo de processamento do comando
  void dmaTransfer();  // DMA do G1 (SB_GDST)
  void dmaDone();      // fim do DMA: termina o comando se a leitura acabou
  void cddaSamples(int16_t* lr, int n);  // áudio de CD tocando (mistura no AICA)
  void sync(StateIO& io);
  uint32_t reads = 0;

  uint8_t status = 0x50, error = 0, features = 0, ireason = 0, sectorCount = 0, drvSel = 0, devCtrl = 0;
  uint16_t byteCount = 0;
  uint8_t packet[12] = {};
  int packetPos = 0;
  int state = 0;  // 0 parado, 1 recebendo pacote, 2 dados para a CPU (PIO), 3 dados da CPU, 4 esperando DMA
  std::vector<uint8_t> buf;
  uint32_t bufPos = 0, phaseEnd = 0;
  uint32_t readFad = 0, readLeft = 0, readSecSize = 2048;
  uint8_t readSel = 0;
  bool dmaMode = false;
  uint8_t driveState = 1;  // 1 pausa, 2 em espera, 3 tocando
  uint32_t curFad = 150, playStart = 0, playEnd = 0;
  int playRepeat = 0;
  uint8_t mode_[32] = {};
  uint8_t sense = 0, asc = 0;
  int16_t cddaBuf_[1176] = {};
  int cddaPos_ = 588;
  bool pendingPacket_ = false;
  uint32_t setModeOff_ = 0;

private:
  void command(uint8_t c);
  void spi();
  void sendData(const uint8_t* d, size_t n, size_t alloc);
  void complete(bool check = false);
  void nextPhase();
  bool fillRead();
  void raise();
  int discFormat() const;
};

// ================================================================================================ Maple
class Maple {
public:
  explicit Maple(System& s);
  System& sys;
  uint32_t keys = 0;  // teclas do Índigo
  uint32_t keys2 = 0;  // controle da porta B (jogador 2, sem VMU); só responde com pad2On
  bool pad2On = false;
  std::vector<uint8_t> vmu;
  bool vmuDirty = false;
  uint32_t dmas = 0;
  void dma(uint32_t list);
  void sync(StateIO& io);

private:
  uint32_t device(int port, int unit, uint32_t* req, int words, uint32_t* resp, uint32_t& dest);
};

// ================================================================================================ console
class System {
public:
  System(const std::vector<uint8_t>& boot, const std::vector<uint8_t>& flash, cdrom::Disc* disc);
  ~System();

  std::vector<uint8_t> ram, vram, aram, boot, flash;
  Sh4 cpu{*this};
  Pvr pvr{*this};
  Aica aica{*this};
  Gdrom gdrom{*this};
  Maple maple{*this};

  // páginas de 64 KB do espaço físico de 29 bits (nulo = registradores, sai pelo caminho lento)
  uint8_t* rmap[0x2000] = {};
  uint8_t* wmap[0x2000] = {};

  // Holly (registradores SB, 0x005F6800-0x005F7FFF)
  uint32_t sb[0x1800 / 4] = {};
  uint32_t istnrm = 0, istext = 0, isterr = 0;
  void raiseNrm(int bit) { istnrm |= 1u << bit; updateIrq(); }
  void setExt(int bit, bool on) { if (on) istext |= 1u << bit; else istext &= ~(1u << bit); updateIrq(); }
  void updateIrq();

  uint32_t fb[1280 * 960] = {};
  int fbW = 640, fbH = 480;
  int line = 0, lines = 263;
  uint64_t frames = 0;
  int64_t frameStart = 0;
  bool flashDirty = false;

  // agendamento: eventos com hora marcada em ciclos do SH-4
  enum Ev { EV_RENDER, EV_MAPLE, EV_CH2, EV_GDDMA, EV_GDROM, EV_G2DMA, EV_PVRDMA, EV_COUNT };
  int64_t evAt[EV_COUNT];
  void schedule(Ev e, int64_t delay) { evAt[e] = cpu.cycles + delay; cpu.exitRun_ = true; }
  void runFrame();
  void reset();
  void sync(StateIO& io);

  uint8_t read8(uint32_t pa);
  uint16_t read16(uint32_t pa);
  uint32_t read32(uint32_t pa);
  void write8(uint32_t pa, uint8_t v);
  void write16(uint32_t pa, uint16_t v);
  void write32(uint32_t pa, uint32_t v);
  void write32B(uint32_t pa, const uint32_t* d);  // rajada de 32 bytes (store queue, DMA)
  // VRAM vista pelo caminho de 32 bits (endereço de 0 a 8 MB -> posição no arranjo de 64 bits)
  static uint32_t vram32(uint32_t a) { return ((a & 0x3FFFFC) << 1) | ((a >> 20) & 4) | (a & 3); }
  uint32_t vramRead32(uint32_t a32) { uint32_t v; std::memcpy(&v, &vram[vram32(a32 & 0x7FFFFC)], 4); return v; }
  void vramWrite32(uint32_t a32, uint32_t v) { std::memcpy(&vram[vram32(a32 & 0x7FFFFC)], &v, 4); }

  // programa carregado direto na RAM (testes): PC e o modo de partida sem BIOS
  void loadProgram(const std::vector<uint8_t>& bin, uint32_t addr, uint32_t entry);

  void buildMap();

private:
  void runLine();
  void doEvent(int e);
  uint32_t sbRead(uint32_t off);
  void sbWrite(uint32_t off, uint32_t v);
  void ch2Dma();
  void g2Dma(int ch);
  void pvrDma();
  // DMA em andamento: escritas que ele mesmo faça nos registradores de disparo
  // não começam outro DMA (senão um destino em cima do SB vira recursão sem fim)
  bool dmaBusy_ = false;
  template <class F> void runDma(F&& f) {
    if (dmaBusy_) return;
    dmaBusy_ = true;
    f();
    dmaBusy_ = false;
  }
  void flashWrite(uint32_t off, uint8_t v);
  int flashState_ = 0;
  uint32_t g2Pending_ = 0;
  int audioFrac_ = 0;
};

// ================================================================================================ Core
class DcCore : public Core {
public:
  // boot/flash nulos = os do usuário (setFirmware "dc_boot" e "dc_flash")
  DcCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save,
         const std::vector<uint8_t>* boot = nullptr, const std::vector<uint8_t>* flash = nullptr);
  std::string system() const override { return "dc"; }
  std::string title() const override { return title_; }
  void runFrame() override;
  void reset() override;
  void setKeys(uint32_t mask) override { sys_->maple.keys = mask; }
  void setKeys2(uint32_t mask) override { sys_->maple.keys2 = mask; sys_->maple.pad2On = true; }
  int players() const override { return 2; }
  const uint32_t* framebuffer() const override { return sys_->fb; }
  int width() const override { return sys_->fbW; }
  int height() const override { return sys_->fbH; }
  double fps() const override { return 59.94; }
  void setSampleRate(int rate) override { rate_ = rate; }
  size_t audioAvailable() const override;
  void pullAudio(float* left, float* right, size_t n) override;
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return sys_->maple.vmuDirty || sys_->flashDirty; }
  std::vector<uint8_t> exportSave() override;
  void importSave(const std::vector<uint8_t>& bytes) override;
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& state) override;
  std::string note() const override;
  System& sys() { return *sys_; }

private:
  std::unique_ptr<cdrom::Disc> disc_;
  std::unique_ptr<System> sys_;
  std::vector<uint8_t> boot_, flash_;
  std::string title_;
  int rate_ = 48000;
  double audioPos_ = 0;
};

}  // namespace indigo::dc
