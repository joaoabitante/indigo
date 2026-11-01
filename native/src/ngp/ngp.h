// Neo Geo Pocket / Pocket Color (porte de src/ngp/ngp.js, bios.js e header.js): TLCS-900H a 6,144 MHz, Z80 de
// som a 3,072 MHz, vídeo K2GE, som T6W28 + DAC, timers de 8 bits, micro-DMA, relógio e flash do cartucho.
// A BIOS é a própria do Índigo (nenhum byte da BIOS da SNK). Cada linha dura 515 ciclos; o quadro tem 199 linhas.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "../sms/z80.h"
#include "k2ge.h"
#include "t6w28.h"
#include "tlcs900h.h"

namespace indigo::ngp {

bool isNgpRom(const std::vector<uint8_t>& bytes);

struct NgpInfo {
  uint32_t start = 0;
  int catalog = 0, sub = 0;
  bool color = false;
  std::string title, gameCode;
};
NgpInfo ngpInfo(const std::vector<uint8_t>& h);

// data e hora usadas pelo relógio do console (o padrão é o relógio do computador)
struct ClockTime { int year, month, day, hour, minute, second, weekday; };
// para testes determinísticos: fixa o relógio de todas as instâncias (nullptr volta ao relógio real)
void setFixedClock(const ClockTime* t);

class NGP : public TlcsBus {
public:
  NGP();

  TLCS900H cpu;
  K2GE gfx;
  T6W28 apu;
  bool color = true;
  std::string title, gameCode;
  bool dirty = false;
  bool requestReset = false;
  int lineCycles = 0;
  int frame = 0;

  void loadRom(const std::vector<uint8_t>& data);
  void reset();
  void setKeys(int mask);
  void runFrame();

  std::vector<uint8_t> exportSave();  // formato .ngf
  void importSave(const std::vector<uint8_t>& b);
  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // barramento (também usado pela BIOS, com endereços sem máscara)
  int mem8(int64_t a);
  int mem16(int64_t a);
  uint32_t mem32(int64_t a);
  void put8(int64_t a, int v);
  void put16(int64_t a, int v);
  void put32(int64_t a, uint32_t v);

  // TlcsBus
  uint32_t read8(uint32_t a) override { return static_cast<uint32_t>(mem8(a)); }
  uint32_t read16(uint32_t a) override { return static_cast<uint32_t>(mem16(a)); }
  uint32_t read32(uint32_t a) override { return mem32(a); }
  void write8(uint32_t a, uint32_t v) override { put8(a, static_cast<int>(v)); }
  void write16(uint32_t a, uint32_t v) override { put16(a, static_cast<int>(v)); }
  void write32(uint32_t a, uint32_t v) override { put32(a, v); }

  // usados pela BIOS própria
  uint8_t intPrio[0x10] = {};
  int64_t chipSize(int64_t base) const;
  void flashProgram(int64_t a, int v);
  void flashErase(int64_t base, int64_t off, int64_t len);

private:
  struct Z80Side : Z80Bus {
    NGP& n;
    explicit Z80Side(NGP& sys) : n(sys) {}
    int read(int a) override;
    void write(int a, int v) override;
    int in(int) override { return 0; }
    void out(int, int) override;
  };

  int io8(int a);
  void ioWrite8(int a, int v);
  int rtc(int a);
  void flashWrite(int64_t a, int v);
  int flashIdRead(int64_t a);
  void onBios();
  int biosCall(int n);  // 1 = 'stay' (fica parado), 0 = volta
  void request(int index, int vec);
  void checkIrq();
  void interrupt(int index, int level);
  void dma(int ch);
  void timers(int n);
  std::vector<uint8_t> buildNgf() const;
  void sync(StateIO& io);

  Z80Side z80bus;
  Z80 z80;
  std::vector<uint8_t> bios, ram, rom, orig;
  uint32_t start = 0;
  int catalog = 0, subCode = 0;
  uint8_t pending[24] = {};
  uint8_t timer[4] = {}, thresh[4] = {}, hdma[4] = {};
  double tclk[4] = {};
  int keys = 0;
  bool z80on = false;
  int z80time = 0;
  int comm = 0, sc0buf = 0, commStatus = 0;
  int trun = 0, t01mod = 0, t23mod = 0, trdc = 0, tffcr = 0;
  bool hint = false;
  int64_t cycles = 0;
  int flashCmd = 0;
  bool flashId = false;
  bool irqCheck = true;
  bool rtcLatched = false;
  int rtcLatch[7] = {};
};

class NGPCore : public Core {
public:
  NGPCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save);
  NGP& ngp() { return *m_; }

  std::string system() const override { return m_->color ? "ngpc" : "ngp"; }
  std::string title() const override { return m_->title; }
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(static_cast<int>(mask)); }
  const uint32_t* framebuffer() const override { return m_->gfx.fb; }
  int width() const override { return W; }
  int height() const override { return H; }
  double fps() const override { return 6144000.0 / (515 * 199); }
  void setSampleRate(int rate) override { m_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->apu.pull(l, r, n); }
  bool hasSave() const override { return true; }
  bool saveDirty() const override { return m_->dirty; }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<NGP> m_;
};

}  // namespace indigo::ngp
