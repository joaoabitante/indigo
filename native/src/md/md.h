// Mega Drive / Genesis: 68000 a 7,67 MHz (relógio mestre / 7), Z80 a 3,58 MHz (/15) para o som, VDP, YM2612 e
// PSG SN76489. NTSC: 262 linhas × 3420 ciclos mestres (59,92 quadros/s); PAL: 313 linhas (49,70 quadros/s).
//
// Cartucho: até 4 MB linear; acima disso o mapeador do Super Street Fighter II ("SEGA SSF", páginas de 512 KB
// em A130F3-A130FF). SRAM de save pelo cabeçalho ("RA" em 0x1B0, normalmente em 0x200001), ligada pelo
// registrador A130F1 quando a ROM passa de 2 MB. Região pelo cabeçalho (0x1F0, JUE ou o código novo em hexa):
// escolhe NTSC/PAL e o registrador de versão (A10001). TMSS: o console é emulado sem a trava (versão 0), então
// nenhuma BIOS é necessária. Arquivos .smd (cabeçalho de 512 bytes e blocos de 16 KB intercalados) são
// desintercalados.
//
// Controle de 6 botões no controle 1 (o 2 fica desligado). Teclas do Índigo (bits de Key), pensando num controle
// de Xbox (A embaixo, B à direita, X à esquerda, Y em cima) e na fileira de baixo do controle do Mega Drive:
//   Y do Índigo (X do Xbox, à esquerda)  = A do Mega Drive
//   A do Índigo (A do Xbox, embaixo)     = B do Mega Drive
//   B do Índigo (B do Xbox, à direita)   = C do Mega Drive
//   L = X, X do Índigo (Y do Xbox, em cima) = Y, R = Z      (fileira de cima)
//   START = Start, SELECT = Mode, direcional = direcional
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../sms/psg.h"
#include "../sms/z80.h"
#include "m68k.h"
#include "vdp.h"
#include "ym2612.h"

namespace indigo::md {

// ROM do Mega Drive pela extensão (.md, .gen, .smd, .bin) e pelo cabeçalho "SEGA" em 0x100
bool isMdRom(const std::vector<uint8_t>& rom, const std::string& nomeMinusculo);
// tira o cabeçalho/intercalação do formato .smd quando houver
std::vector<uint8_t> mdNormalize(const std::vector<uint8_t>& data);

// Hardware ligado ao Mega Drive pela porta de expansão ou de cartucho (Sega CD, 32X): o MD entrega a ele os
// acessos do 68000, do DMA do VDP e da janela do Z80 nas páginas de 64 KB marcadas em page[] (na página A1, a
// de E/S do MD, só a faixa ioLo..ioHi) e o avisa do fim de cada linha (mclk = ciclos mestres desde o começo)
// para ele andar junto. Sem expansão, nada muda no MD.
class MdExpansion {
public:
  virtual ~MdExpansion() = default;
  bool page[256] = {};
  uint32_t ioLo = 1, ioHi = 0;
  virtual uint8_t read8(uint32_t a) = 0;
  virtual uint16_t read16(uint32_t a) = 0;
  virtual void write8(uint32_t a, uint8_t v) = 0;
  virtual void write16(uint32_t a, uint16_t v) = 0;
  virtual uint16_t dmaRead(uint32_t a) { return read16(a); }
  virtual void runTo(int64_t mclk) { (void)mclk; }
  virtual bool diskUnit() const { return false; }  // unidade de disco ligada: bit 5 do A10001 vai a 0
};

// Barramento externo: um acessório (o 32X no conector do cartucho) é consultado antes do mapa normal nos acessos
// do 68000, do DMA do VDP e da janela do Z80 (endereços de 24 bits, size 1 ou 2), e avisado no começo de cada linha
// e quando o VDP termina de desenhar uma linha (pontos e índices de cor, para compor a imagem dele por cima).
class MdBus {
public:
  virtual ~MdBus() = default;
  virtual bool busRead(uint32_t a, int size, uint16_t& v) = 0;   // true = o acessório respondeu
  virtual bool busWrite(uint32_t a, int size, uint16_t v) = 0;
  virtual void busLine(int line, int64_t mclk) = 0;              // antes de o VDP desenhar a linha
  virtual void busLineDrawn(int row, uint32_t* px, const uint8_t* idx, int w) = 0;
};

class MD : public M68kBus, public Z80Bus, public VdpHost {
public:
  MD(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  // definido em md.cpp: com três bases virtuais, o destrutor implícito gerava a mesma "thunk" em dois
  // objetos e o link do Windows (MinGW com LTO) recusava a definição duplicada
  ~MD() override;

  M68k cpu;
  Z80 z80;
  VDP vdp;
  YM2612 ym;
  sms::PSG psg;
  bool pal = false, overseas = true;
  std::string title;
  int frame = 0;

  void reset();
  void attach(MdExpansion* e) { ext_ = e; }
  int64_t cpuTime() const { return now(); }  // hora do 68000 em ciclos mestres (para a expansão)
  void setKeys(uint32_t mask) { keys_ = mask; }
  void runFrame();
  double fps() const { return masterClock() / (3420.0 * vdp.linesPerFrame()); }
  double masterClock() const { return pal ? 53203424.0 : 53693175.0; }

  void setSampleRate(int rate);
  size_t audioAvailable() const;
  void pullAudio(float* l, float* r, size_t n);

  bool hasSave() const { return !sram_.empty(); }
  bool saveDirty() const { return dirty_; }
  std::vector<uint8_t> exportSave() { dirty_ = false; return sram_; }
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // M68kBus
  uint8_t read8(uint32_t a) override;
  uint16_t read16(uint32_t a) override;
  void write8(uint32_t a, uint8_t v) override;
  void write16(uint32_t a, uint16_t v) override;
  int intAck(int level) override { vdp.ack(level); return 24 + level; }
  bool tasWrites() override { return false; }
  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int) override { return 0xFF; }
  void out(int, int) override {}
  const std::vector<uint8_t>& workRam() const { return ram_; }  // usado pelos testes
  // VdpHost
  uint16_t dmaRead(uint32_t a) override;
  void lineDrawn(int row, uint32_t* px, const uint8_t* idx, int w) override { if (ext) ext->busLineDrawn(row, px, idx, w); }

  MdBus* ext = nullptr;                          // acessório no barramento (nulo = Mega Drive sozinho)
  int64_t clock() const { return now(); }        // hora atual do 68000 (ciclos mestres)

private:
  void sync(StateIO& io);
  int64_t now() const { return m68Mclk_ + static_cast<int64_t>(cpu.cycles) * 7; }
  void runUntil(int64_t target);
  void syncZ80(int64_t target);
  void z80BusAccess();
  uint8_t romByte(uint32_t a) const;
  bool sramMapped(uint32_t a) const;
  uint8_t ioRead(int r);
  void ioWrite(int r, uint8_t v);
  uint8_t padRead(int port);
  uint8_t z80AreaRead(uint32_t a);
  uint8_t ymStatus(uint32_t a, int64_t t);
  void ymWrite(uint32_t a, uint8_t v, int64_t t);
  void z80AreaWrite(uint32_t a, uint8_t v);
  uint16_t vdpRead(uint32_t a);
  void vdpWrite(uint32_t a, uint16_t v, bool byte);

  MdExpansion* ext_ = nullptr;
  bool extArea(uint32_t a) const {
    const uint32_t p = (a >> 16) & 0xFF;
    return ext_->page[p] && (p != 0xA1 || ((a & 0xFFFF) >= ext_->ioLo && (a & 0xFFFF) <= ext_->ioHi));
  }
  std::vector<uint8_t> rom_, ram_, zram_, sram_;
  uint32_t romMask_ = 0;
  bool ssf2_ = false;
  uint8_t bank_[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  uint32_t sramStart_ = 0, sramEnd_ = 0;
  uint8_t sramCtl_ = 0;  // A130F1: bit 0 = SRAM no lugar da ROM, bit 1 = protegida
  bool dirty_ = false;

  // relógios (ciclos mestres desde o começo)
  int64_t mclk_ = 0, lineStart_ = 0, m68Mclk_ = 0, z80Mclk_ = 0, refreshAt_ = 0, ymBusyUntil_ = 0, m68kStall_ = 0;
  bool z80Reset_ = true, z80BusReq_ = false, firstFrame_ = true;
  int z80Bank_ = 0;

  // E/S
  uint8_t ioData_[3] = {}, ioCtrl_[3] = {};
  int padPhase_[2] = {};
  int64_t padLastTh_[2] = {};
  uint32_t keys_ = 0;
  void mixAudio();
  static constexpr size_t OUTBUF = 16384;
  std::vector<float> ymL_, ymR_, psgL_, psgR_;
  std::vector<float> outL_ = std::vector<float>(OUTBUF), outR_ = std::vector<float>(OUTBUF);
  size_t outW_ = 0, outR0_ = 0;
  float psgLpL_ = 0, psgLpR_ = 0, psgLpK_ = 0.5f;
};

class MDCore : public Core {
public:
  MDCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) : m_(std::make_unique<MD>(rom, save)) {}
  MD& md() { return *m_; }

  std::string system() const override { return "md"; }
  std::string title() const override { return m_->title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(mask); }
  const uint32_t* framebuffer() const override { return m_->vdp.fb; }
  int width() const override { return m_->vdp.outW; }
  int height() const override { return m_->vdp.outH; }
  double fps() const override { return m_->fps(); }
  void setSampleRate(int rate) override { m_->setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->audioAvailable(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->pullAudio(l, r, n); }
  bool hasSave() const override { return m_->hasSave(); }
  bool saveDirty() const override { return m_->saveDirty(); }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<MD> m_;
};

}  // namespace indigo::md
