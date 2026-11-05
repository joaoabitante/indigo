// Núcleo Master System / Game Gear (porte de src/sms/sms.js e header.js): Z80 a 3,58 MHz, 262 linhas ×
// 228 ciclos (NTSC), mapeadores Sega e Codemasters, RAM do cartucho (save), portas de E/S.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "psg.h"
#include "vdp.h"
#include "z80.h"

namespace indigo::sms {

// "gg", "sms" ou "" (não reconhecido) pela extensão ou pelo cabeçalho "TMR SEGA"
std::string smsKind(const std::vector<uint8_t>& rom, const std::string& name);
std::vector<uint8_t> smsNormalize(const std::vector<uint8_t>& data);
std::string smsCode(const std::vector<uint8_t>& rom);

class SMS : public Z80Bus {
public:
  explicit SMS(bool gg);

  bool gg;
  Z80 cpu;
  VDP vdp;
  PSG apu;
  std::string gameCode;
  bool dirty = false;
  int frame = 0;

  void loadRom(const std::vector<uint8_t>& data);
  void reset();
  void setKeys(int mask);
  void setKeys2(int mask) { if (!gg) keys2 = mask; }  // controle 2 (o Game Gear não tem)
  void runFrame();

  bool hasSave() const { return cartRamUsed; }
  std::vector<uint8_t> exportSave() { dirty = false; return cartRam; }
  void importSave(const std::vector<uint8_t>& b);

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

  // Z80Bus
  int read(int a) override;
  void write(int a, int v) override;
  int in(int port) override;
  void out(int port, int v) override;

private:
  void sync(StateIO& io);

  std::vector<uint8_t> rom, ram, cartRam;
  int banks = 1;
  bool codemasters = false;
  int slot[3] = {0, 1, 2};
  int ramCtl = 0;
  bool cartRamUsed = false;
  int ioCtl = 0xFF, memCtl = 0;
  int64_t cycles = 0;
  int over = 0, cpuCyclesInLine = 0;
  bool pausePrev = false;
  int keys = 0, keys2 = 0;
};

class SMSCore : public Core {
public:
  SMSCore(const std::vector<uint8_t>& rom, bool gg, const std::vector<uint8_t>* save);
  SMS& sms() { return *m_; }

  std::string system() const override { return m_->gg ? "gg" : "sms"; }
  std::string title() const override { return ""; }  // o cartucho não traz título
  void runFrame() override { m_->runFrame(); }
  void reset() override { m_->reset(); }
  void setKeys(uint32_t mask) override { m_->setKeys(static_cast<int>(mask)); }
  void setKeys2(uint32_t mask) override { m_->setKeys2(static_cast<int>(mask)); }
  int players() const override { return m_->gg ? 1 : 2; }
  const uint32_t* framebuffer() const override { return m_->vdp.fb; }
  int width() const override { return m_->vdp.w; }
  int height() const override { return m_->vdp.h; }
  double fps() const override { return 3579545.0 / (262 * 228); }
  void setSampleRate(int rate) override { m_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return m_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { m_->apu.pull(l, r, n); }
  bool hasSave() const override { return m_->hasSave(); }
  bool saveDirty() const override { return m_->dirty; }
  std::vector<uint8_t> exportSave() override { return m_->exportSave(); }
  void importSave(const std::vector<uint8_t>& b) override { m_->importSave(b); }
  std::vector<uint8_t> saveState() override { return m_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { m_->loadState(s); }

private:
  std::unique_ptr<SMS> m_;
};

}  // namespace indigo::sms
