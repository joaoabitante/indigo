// Sega Saturn: dois SH-2 (o mesmo SH-2 do 32X, src/md32x/sh2.h), SCU (interrupções, DMA, temporizadores e o
// DSP), VDP1 (sprites e polígonos), VDP2 (planos de fundo e a imagem final), SCSP (som, com o 68000 de
// src/md/m68k.h), SMPC (controle, liga/desliga as CPUs) e o bloco de CD (por cima de src/cdrom).
// Núcleo escrito do zero a partir da documentação pública do hardware; o Mednafen (GPL-2.0 ou posterior) foi
// consultado como documentação de registradores e comportamentos (ver TERCEIROS.md), sem cópia de código.
//
// Mapa de memória (endereço físico, 27 bits):
//   0x00000000 BIOS (512 KB)   0x00100000 SMPC (bytes ímpares)   0x00180000 RAM de backup (32 KB, bytes ímpares)
//   0x00200000 RAM baixa (1 MB)   0x01000000/0x01800000 escrita = pulso FTI no SH-2 escravo/mestre
//   0x02000000-0x04FFFFFF cartucho (vazio)   0x05800000 bloco de CD   0x05A00000 RAM de som (512 KB)
//   0x05B00000 SCSP   0x05C00000 VRAM do VDP1   0x05C80000 quadro do VDP1   0x05D00000 VDP1 (registradores)
//   0x05E00000 VRAM do VDP2   0x05F00000 CRAM   0x05F80000 VDP2 (registradores)   0x05FE0000 SCU
//   0x06000000 RAM alta (1 MB, espelhada até 0x07FFFFFF)
//
// Interrupções: a SCU dá o vetor ao SH-2 mestre (0x40 + fonte); o escravo recebe direto o V-blank (nível 6,
// vetor 0x43) e o H-blank (nível 2, vetor 0x41), como no aparelho.
#pragma once
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../core/core.h"
#include "../core/state.h"
#include "../md/m68k.h"
#include "../md32x/sh2.h"

namespace indigo::saturn {

using md32x::Sh2;

// BIOS do Saturn (arquivo do usuário, 512 KB, ex.: sega_101.bin): vetores do SH-2 no começo e o texto
// "SEGA SEGASATURN" (que ela compara com o cabeçalho do disco). Nunca vem com o Índigo.
bool isSaturnBios(const std::vector<uint8_t>& bytes);
// disco do Saturn: "SEGA SEGASATURN " no começo do primeiro setor de dados
bool isSaturnDisc(cdrom::Disc& d);

class System;

// ------------------------------------------------------------------------------------------------ SCU
// fontes internas de interrupção (bit no IST/IMS; vetor 0x40 + fonte)
enum ScuInt { INT_VBIN = 0, INT_VBOUT, INT_HBIN, INT_TIMER0, INT_TIMER1, INT_DSP, INT_SCSP, INT_SMPC, INT_PAD,
              INT_DMA2, INT_DMA1, INT_DMA0, INT_DMAILL, INT_VDP1 = 13 };
// fatores de partida do DMA (D0MD bits 2-0)
enum DmaFactor { DF_VBIN = 0, DF_VBOUT, DF_HBIN, DF_TIMER0, DF_TIMER1, DF_SOUND, DF_SPRITE, DF_NOW };

class Scu {
public:
  explicit Scu(System& s) : sys_(s) {}
  void reset();
  uint32_t read(uint32_t off);
  void write(uint32_t off, uint32_t v, uint32_t mask);
  void raise(int src);              // pulso da fonte interna
  void factor(int f);               // evento que pode iniciar um DMA
  int masterVector();               // o SH-2 mestre aceitou a interrupção
  void lineStart(int line);         // começo da linha: temporizador 0 conta as linhas
  void vblankOut();
  uint32_t pending() const { return ist_; }
  void sync(StateIO& io);

  // DSP da SCU (exposto para os testes)
  struct Dsp {
    uint32_t prog[256] = {};
    uint32_t data[4][64] = {};
    uint8_t pc = 0, ct[4] = {}, top = 0, pda = 0;
    uint16_t lop = 0;          // contador de laço (12 bits)
    uint32_t rx = 0, ry = 0, ra0 = 0, wa0 = 0;
    int64_t acc = 0, p = 0;   // ACH:ACL e PH:PL (48 bits)
    bool run = false, step = false, end = false, s = false, z = false, c = false, v = false, t0 = false;
    bool jumpPending = false; uint8_t jumpTo = 0;
  } dsp;
  void dspRun(int cycles);

private:
  void check();
  bool dmaBusy_ = false, dspBusy_ = false;  // travas contra reentrada (não vão para o estado)
  void startDma(int level);
  void dmaTransfer(int level, uint32_t src, uint32_t dst, uint32_t count);
  void dspExec(uint32_t op);
  void dspDma(uint32_t op);
  uint32_t dspSrc(int sel);
  void dspDst(int sel, uint32_t v);

  System& sys_;
  uint32_t ist_ = 0, ims_ = 0xBFFF;
  int level_ = 0, vec_ = 0;
  struct Level { uint32_t r = 0, w = 0, c = 0, add = 0x101, en = 0, md = 0; } dma_[3];
  uint32_t t0cmp_ = 0, t0cnt_ = 0, t1rel_ = 0, t1md_ = 0;
  uint32_t asr0_ = 0, asr1_ = 0, aref_ = 0, rsel_ = 0;
};

// ------------------------------------------------------------------------------------------------ SMPC
class Smpc {
public:
  explicit Smpc(System& s) : sys_(s) {}
  void reset();
  uint8_t read(uint32_t reg);
  void write(uint32_t reg, uint8_t v);
  void vblankIn();
  void setPad(uint16_t buttons) { pad_ = buttons; }   // bits ativos em 1, formato do controle padrão
  void setRtc(const uint8_t bcd[7]);
  void sync(StateIO& io);
  bool slaveOn = false, soundOn = false;

private:
  void command(uint8_t c);
  void peripheral();
  System& sys_;
  uint8_t ireg_[7] = {}, oreg_[32] = {}, sr_ = 0, sf_ = 0, comreg_ = 0;
  uint8_t pdr_[2] = {}, ddr_[2] = {}, iosel_ = 0, exle_ = 0;
  uint8_t smem_[4] = {};
  uint8_t rtc_[7] = {0x20, 0x26, 0x11, 0x02, 0x12, 0x00, 0x00};  // ano (2 bytes BCD), dia da semana/mês, dia, h, m, s
  uint16_t pad_ = 0;
  bool pendingPad_ = false;  // INTBACK: os dados do controle vêm depois do "continue"
};

// ------------------------------------------------------------------------------------------------ VDP1
class Vdp1 {
public:
  explicit Vdp1(System& s) : sys_(s) {}
  std::vector<uint8_t> vram = std::vector<uint8_t>(0x80000);
  uint16_t fb[2][512 * 256] = {};   // os dois quadros (16 bits por ponto)
  int drawBuf = 0;                  // quadro em que o VDP1 desenha (o outro é mostrado pelo VDP2)
  void reset();
  uint16_t readReg(uint32_t off);
  void writeReg(uint32_t off, uint16_t v);
  uint16_t fbRead16(uint32_t off) const;
  void fbWrite16(uint32_t off, uint16_t v);
  void fbWrite8(uint32_t off, uint8_t v);
  void vblankIn();                  // troca de quadro e desenho automático
  void draw();                      // executa a lista de comandos
  const uint16_t* display() const { return fb[drawBuf ^ 1]; }
  bool eightBit() const { return tvmr_ & 1; }
  int commandsRun = 0;              // comandos do último desenho (para os testes)
  void sync(StateIO& io);

private:
  struct Cmd;
  uint16_t vr16(uint32_t a) const { a &= 0x7FFFE; return uint16_t(vram[a] << 8 | vram[a + 1]); }
  void plot(int x, int y, uint16_t color, const Cmd& c);
  bool texel(const Cmd& c, int u, int v, uint16_t& out, bool& endCode) const;
  void sprite(const Cmd& c);
  void quad(const Cmd& c, const int* xs, const int* ys, bool textured);
  void line(const Cmd& c, int x0, int y0, int x1, int y1, int v, bool textured, int texW, const int* g0, const int* g1);
  void eraseFb(int buf);

  System& sys_;
  uint16_t tvmr_ = 0, fbcr_ = 0, ptmr_ = 0, ewdr_ = 0, ewlr_ = 0, ewrr_ = 0x50DF;
  uint16_t edsr_ = 0, lopr_ = 0, copr_ = 0, modr_ = 0;
  bool manualSwap_ = false, manualErase_ = false;
  int sysClipX_ = 0, sysClipY_ = 0, userX0_ = 0, userY0_ = 0, userX1_ = 0, userY1_ = 0, localX_ = 0, localY_ = 0;
  bool drawPending_ = false;
  int64_t budget_ = 0;
};

// ------------------------------------------------------------------------------------------------ VDP2
class Vdp2 {
public:
  explicit Vdp2(System& s) : sys_(s) {}
  std::vector<uint8_t> vram = std::vector<uint8_t>(0x80000);
  uint8_t cram[0x1000] = {};
  uint16_t regs[0x90] = {};          // registradores de 0x000 a 0x11E (palavras)
  uint32_t out[704 * 512] = {};      // imagem final 0xAABBGGRR
  int outW = 320, outH = 224;
  void reset();
  uint16_t readReg(uint32_t off);
  void writeReg(uint32_t off, uint16_t v);
  int visibleLines() const;           // 224, 240 ou 256
  int hres() const;                   // 320, 352, 640 ou 704
  void setStatus(bool vblank, bool hblank, int line, bool odd) { vb_ = vblank; hb_ = hblank; line_ = line; odd_ = odd; }
  bool pal = false;
  void render();                      // desenha o quadro inteiro (no começo do V-blank)
  void sync(StateIO& io);

private:
  struct Pix { uint32_t rgb; uint8_t pri; uint8_t cc; uint8_t layer; bool ccOn; };
  struct Tile;                          // como ler um ponto de um plano (NBG ou RBG)
  struct LineScroll { uint32_t lsa; uint32_t x, yBase, yAcc, zx; };
  uint16_t reg(uint32_t off) const { return (off >> 1) < 0x90 ? regs[off >> 1] : 0; }
  uint16_t vr16(uint32_t byteAddr) const { byteAddr &= 0x7FFFE; return uint16_t(vram[byteAddr] << 8 | vram[byteAddr + 1]); }
  uint32_t color(uint32_t idx) const;   // cor da CRAM (0x00BBGGRR com 8 bits por canal)
  Tile tileNbg(int n) const;
  Tile tileRbg(int which) const;
  bool dot(const Tile& t, uint32_t ix, uint32_t iy, uint32_t& rgb) const;
  bool windowed(uint8_t ctrl, int x, int y) const;  // ponto escondido pela janela W0/W1
  void renderNbg(int n, int y, Pix* line, LineScroll* ls);
  void renderSprite(int y, Pix* line);
  void renderRbg(int y, Pix* line);
  uint32_t backColor(int y) const;
  System& sys_;
  bool vb_ = false, hb_ = false, odd_ = false;
  int line_ = 0;
  uint8_t swin_[704] = {};    // janela de sprite da linha (bit SD dos sprites)
  uint8_t shadow_[704] = {};  // sombra normal da linha (código 0x7FE dos sprites)
};

// ------------------------------------------------------------------------------------------------ SCSP
class Scsp : public md::M68kBus {
public:
  explicit Scsp(System& s);
  std::vector<uint8_t> ram = std::vector<uint8_t>(0x80000);
  md::M68k cpu{*this};
  bool cpuOn = false;
  bool dmaBusy_ = false;  // trava: o DMA do SCSP não dispara outro DMA
  void reset();
  uint16_t readReg(uint32_t off);
  void writeReg(uint32_t off, uint16_t v, uint16_t mask);
  void run68k(int cycles);
  void sample(int16_t& l, int16_t& r);   // uma amostra a 44,1 kHz
  void sync(StateIO& io);
  void cdda(int16_t l, int16_t r) { exl_ = l; exr_ = r; }

  // M68kBus
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  int intAck(int level) override { return 24 + level; }

private:
  struct Slot {
    uint16_t r[16] = {};
    uint32_t pos = 0;          // posição na onda (20.12 em amostras)
    int env = 0x3FF;           // atenuação do envelope (0 = máximo, 0x3FF = mudo)
    int phase = 3;             // 0 ataque, 1 decaimento 1, 2 decaimento 2, 3 soltura
    bool on = false, back = false;
    int32_t envAcc = 0;
    int16_t out = 0;
  };
  void keyOnOff();
  int16_t slotStep(int n, int32_t& l, int32_t& r);  // devolve a saída do canal (para a pilha de som)
  void timersStep();
  void updateIrq();
  System& sys_;
  Slot slot_[32];
  uint16_t common_[0x18] = {};
  uint16_t scieb_ = 0, scipd_ = 0, mcieb_ = 0, mcipd_ = 0, scilv_[3] = {};
  uint8_t timer_[3] = {}, tctl_[3] = {};
  uint32_t timerSub_[3] = {};
  uint16_t stack_[64] = {};
  int16_t exl_ = 0, exr_ = 0;
  uint32_t sampleCount_ = 0;
  int64_t cycleDebt_ = 0;
  uint32_t lfsr_ = 1;
  int16_t delay_[4] = {};
};

// ------------------------------------------------------------------------------------------------ bloco de CD
class CdBlock {
public:
  explicit CdBlock(System& s) : sys_(s) {}
  void setDisc(cdrom::Disc* d) { disc_ = d; }
  void reset();
  uint16_t read(uint32_t off);         // registradores (0x25890000 + off)
  void write(uint32_t off, uint16_t v);
  uint16_t readData();                 // 0x25818000: dados de 16 bits
  uint32_t readData32();
  void run(int cycles);                // avança a leitura do disco
  uint16_t hirq() const { return hirq_; }
  bool audioOut(int16_t& l, int16_t& r);   // amostra de CD de áudio (44,1 kHz)
  void sync(StateIO& io);

private:
  void command();
  void status(uint8_t extra = 0, bool reject = false);
  void irq(uint16_t bits) { hirq_ |= bits; }
  void startPlay(uint32_t start, uint32_t end, int repeat);
  void readSector();
  void route(uint8_t filter, const uint8_t* raw);
  uint32_t fadOfTrack(int t, bool end) const;
  void startTransfer(int part, int offset, int count, bool del);
  void startRawTransfer(const std::vector<uint8_t>& bytes);
  void dirRead(uint32_t fad, uint32_t size);
  int freeBuffers() const;
  bool trackOf(uint32_t fad, int& tno, uint8_t& ctl) const;

  System& sys_;
  cdrom::Disc* disc_ = nullptr;
  uint16_t hirq_ = 0, mask_ = 0, cr_[4] = {}, res_[4] = {};
  uint8_t stat_ = 1;                   // 1 pausa, 3 tocando, 0 ocupado...
  uint32_t fad_ = 150, playStart_ = 0, playEnd_ = 0;
  int repeat_ = 0, repeatMax_ = 0;
  bool playing_ = false, audioTrack_ = false;
  int32_t sectorClock_ = 0, periodClock_ = 0;
  int getSecLen_ = 2048, putSecLen_ = 2048;
  // filtros, partições e buffers (200 setores de 2352)
  struct Filter { uint32_t fad = 0, range = 0; uint8_t mode = 0, trueConn = 0, falseConn = 0xFF, file = 0, chan = 0, sm = 0, smMask = 0, ci = 0, ciMask = 0; };
  Filter filt_[24];
  uint8_t cdConn_ = 0xFF, lastDest_ = 0xFF;
  std::vector<std::vector<uint8_t>> part_[24];   // setores brutos (2352) em cada partição
  struct Xfer { std::vector<uint8_t> data; size_t pos = 0; bool active = false; int part = -1; int count = 0; bool del = false; int offset = 0; } dt_;
  uint32_t lastXfer_ = 0;
  // sistema de arquivos (ISO 9660)
  struct FileEnt { uint32_t fad, size; uint8_t attr, unit, gap, fileNo; };
  std::vector<FileEnt> dir_;
  uint32_t dirOffset_ = 0;
  bool fileRead_ = false;
  uint32_t fileFad_ = 0, fileEnd_ = 0;
  std::vector<int16_t> audioQ_;
  size_t audioPos_ = 0;
  bool authOk_ = false;
};

// ------------------------------------------------------------------------------------------------ sistema
class System : public md32x::Sh2Bus {
public:
  static constexpr double CPU_HZ = 26874100.0;    // modo de 320 pontos (o de 352 usa 28,6 MHz)
  static constexpr int LINES = 263;
  static constexpr int CYCLES_PER_LINE = 1708;     // 427 pontos x 4
  static constexpr int HBLANK_AT = 1280;           // ciclo da linha em que começa o H-blank
  static constexpr int M68K_PER_LINE = 717;        // 11,2896 MHz
  static constexpr int AUDIO_HZ = 44100;

  System(const std::vector<uint8_t>& bios, cdrom::Disc* disc);

  std::vector<uint8_t> bios, lwram, hwram;
  std::vector<uint8_t> backup;      // RAM de backup (32 KB): o save
  bool backupDirty = false;
  Sh2 master{*this, 0}, slave{*this, 1};
  Scu scu{*this};
  Smpc smpc{*this};
  Vdp1 vdp1{*this};
  Vdp2 vdp2{*this};
  Scsp scsp{*this};
  CdBlock cd{*this};
  std::vector<int16_t> audio;       // pares esquerdo/direito a 44,1 kHz
  uint64_t frames = 0;
  int line = 0;
  // região do aparelho, escolhida pelo disco (IP.BIN): código de área do SMPC; Europa e Ásia PAL = 50 Hz
  uint8_t area = 4;
  bool pal = false;
  int lines() const { return pal ? 313 : LINES; }

  void reset();
  void runFrame();
  size_t audioCut = 0;  // quadros de som descartados no último runFrame
  void setPad(uint16_t b) { smpc.setPad(b); }
  void slavePower(bool on);
  void soundPower(bool on);
  void slaveIrl();                  // nível da IRL do escravo pelo V-blank/H-blank
  void sync(StateIO& io);

  // acesso do barramento (usado pelo DMA da SCU e pelos testes); endereço físico
  uint32_t busRead(uint32_t a, int size);
  void busWrite(uint32_t a, int size, uint32_t v);

  // Sh2Bus
  uint8_t rd8(Sh2& c, uint32_t a) override { return uint8_t(busRead(a, 1)); }
  uint16_t rd16(Sh2& c, uint32_t a) override { return uint16_t(busRead(a, 2)); }
  uint32_t rd32(Sh2& c, uint32_t a) override { return busRead(a, 4); }
  void wr8(Sh2& c, uint32_t a, uint8_t v) override { busWrite(a, 1, v); }
  void wr16(Sh2& c, uint32_t a, uint16_t v) override { busWrite(a, 2, v); }
  void wr32(Sh2& c, uint32_t a, uint32_t v) override { busWrite(a, 4, v); }
  int irlVector(Sh2& c, int level) override;

  bool inVblank = false, inHblank = false;

private:
  void mapMemory();
  void runCpus(int64_t until);
  int64_t lineBase_ = 0;
  uint32_t audioFrac_ = 0;
};

class SaturnCore : public Core {
public:
  // disco (.cue/.bin, .chd, .iso); bios nulo = firmware("saturn"). Lança std::runtime_error sem BIOS.
  SaturnCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios = nullptr);

  std::string system() const override { return "saturn"; }
  std::string title() const override { return title_; }
  void runFrame() override {
    sys_->runFrame();
    // se a fila foi cortada (o app não consumiu o som), a posição de leitura acompanha
    audioPos_ = std::max(0.0, audioPos_ - double(sys_->audioCut));
  }
  void reset() override { sys_->reset(); }
  void setKeys(uint32_t mask) override;
  const uint32_t* framebuffer() const override { return sys_->vdp2.out; }
  int width() const override { return sys_->vdp2.outW; }
  int height() const override { return sys_->vdp2.outH; }
  double fps() const override { return System::CPU_HZ / (double(System::CYCLES_PER_LINE) * sys_->lines()); }
  void setSampleRate(int rate) override { rate_ = rate > 0 ? rate : 48000; }
  size_t audioAvailable() const override;
  void pullAudio(float* left, float* right, size_t n) override;
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return sys_->backupDirty; }
  std::vector<uint8_t> exportSave() override { sys_->backupDirty = false; return sys_->backup; }
  void importSave(const std::vector<uint8_t>& bytes) override;
  std::string note() const override { return note_; }
  std::vector<uint8_t> saveState() override;
  void loadState(const std::vector<uint8_t>& state) override;

  System& sys() { return *sys_; }

private:
  std::unique_ptr<cdrom::Disc> disc_;
  std::unique_ptr<System> sys_;
  std::string title_, note_;
  int rate_ = 48000;
  double audioPos_ = 0;
};

}  // namespace indigo::saturn
