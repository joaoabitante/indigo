// Núcleo do Game Boy / Game Boy Color (porte de src/gb/gb.js): CPU SM83 com tempo por ciclo de máquina
// (cada acesso à memória avança vídeo, som, timer e DMA), memória, timer, DMA de OAM, HDMA, dupla velocidade e serial.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../core/core.h"
#include "../core/state.h"
#include "apu.h"
#include "cart.h"
#include "ppu.h"
#include "sgb.h"

namespace indigo::gb {

constexpr int FRAME_DOTS = 70224;

enum class Mode { Auto, Dmg, Cgb };

class GB {
public:
  GB();

  // ordem importa: dotClock existe antes de o APU ser criado (o APU lê o relógio no reset)
  int64_t dotClock = 0;
  PPU ppu;
  APU apu;
  std::unique_ptr<Cart> cart;
  std::unique_ptr<SGB> sgb;  // Super Game Boy (jogo com a marca SGB, modo DMG e opção ligada)
  bool cgb = false;
  std::string title, gameCode;
  int frame = 0;
  int keys = 0;
  std::string serialOut;
  bool romLoaded = false;
  // Mega Duck / Cougar Boy: mesma CPU e vídeo, registradores de I/O em outros endereços, início em $0000
  bool duck = false;

  // duckKind: 0 = Game Boy; 1 = Mega Duck (MD1/MD2/sem mapeador); 2 = Mega Duck MD0
  void loadRom(const std::vector<uint8_t>& data, const std::vector<uint8_t>* saveBytes, Mode mode = Mode::Auto,
               int duckKind = 0);
  void reset();
  void requestIrq(int bits) { if_ |= bits; }
  void setKeys(int mask);
  void hblankDma() { if (hdmaActive) hdmaPending = true; }
  void runFrame();

  int read(int a);
  void write(int a, int v);

  std::vector<uint8_t> saveState();
  void loadState(const std::vector<uint8_t>& s);

private:
  void tick();
  void incTima();
  int readIO(int a);
  void writeIO(int a, int v);
  int readGbIO(int a);
  void writeGbIO(int a, int v);
  void hdmaBlock();

  int rd(int addr) { tick(); return read(addr); }
  void wr(int addr, int v) { tick(); write(addr, v); }
  int imm() { const int v = rd(pc); pc = (pc + 1) & 0xFFFF; return v; }
  int imm16() { const int lo = imm(); const int hi = imm(); return lo | (hi << 8); }
  void push(int v);
  int pop();

  int bc() const { return (b << 8) | c; }
  int de() const { return (d << 8) | e; }
  int hl() const { return (h << 8) | l; }
  void setBC(int v) { b = (v >> 8) & 0xFF; c = v & 0xFF; }
  void setDE(int v) { d = (v >> 8) & 0xFF; e = v & 0xFF; }
  void setHL(int v) { h = (v >> 8) & 0xFF; l = v & 0xFF; }
  int getR(int i);
  void setR(int i, int v);
  int getRR(int i) const;
  void setRR(int i, int v);
  bool cond(int i) const;

  void alu(int op, int v);
  int inc8(int v);
  int dec8(int v);
  int spOffset();
  void cb();
  void step();
  void interrupt();
  void exec(int op);
  void sync(StateIO& io);

  std::vector<uint8_t> wram, hram;
  int a = 0, f = 0, b = 0, c = 0, d = 0, e = 0, h = 0, l = 0, sp = 0, pc = 0;
  bool ime = false, eiDelay = false, halted = false, haltBug = false, stopped = false, locked = false;
  int ie = 0, if_ = 0;
  int div = 0, tima = 0, tma = 0, tac = 0;
  bool timaPending = false, timaReloaded = false;
  int p1 = 0, sb = 0, sc = 0, serialTimer = 0;
  bool dmaActive = false, dmaBlock = false;
  int dmaDelay = 0, dmaSrc = 0, dmaIdx = 0, dmaReg = 0xFF;
  int svbk = 1, key1 = 0;
  bool doubleSpeed = false;
  int hdmaSrc = 0, hdmaDst = 0, hdmaLen = 0;
  bool hdmaActive = false, hdmaPending = false;
  uint8_t undoc[4] = {};
  int frameDots = 0;
};

// Mega Duck: .duck, .md0, .md1, .md2 (a extensão diz o mapeador; .bin fica para os outros consoles).
// Devolve 0 se não for, 1 ou 2 (o duckKind de GB::loadRom).
int duckKind(const std::vector<uint8_t>& rom, const std::string& lowerName);

// adaptador para a interface comum
class GBCore : public Core {
public:
  GBCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, Mode mode = Mode::Auto, int duckKind = 0);
  GB& gb() { return *gb_; }
  bool sgbActive() const { return gb_->sgb != nullptr; }

  std::string system() const override { return gb_->duck ? "duck" : gb_->cgb ? "gbc" : "gb"; }
  std::string title() const override { return gb_->title; }
  void runFrame() override { gb_->runFrame(); }
  void reset() override { gb_->reset(); }
  void setKeys(uint32_t mask) override { gb_->setKeys(static_cast<int>(mask & 0xFF)); }
  const uint32_t* framebuffer() const override { return gb_->sgb ? gb_->sgb->out : gb_->ppu.fb; }
  int width() const override { return gb_->sgb ? SGB::W : 160; }
  int height() const override { return gb_->sgb ? SGB::H : 144; }
  double fps() const override { return 4194304.0 / FRAME_DOTS; }
  void setSampleRate(int rate) override { gb_->apu.setSampleRate(rate); }
  size_t audioAvailable() const override { return gb_->apu.available(); }
  void pullAudio(float* l, float* r, size_t n) override { gb_->apu.pull(l, r, n); }
  bool hasSave() const override { return gb_->cart->hasSave(); }
  bool saveDirty() const override { return gb_->cart->dirty && gb_->cart->hasSave(); }
  std::vector<uint8_t> exportSave() override;
  std::string note() const override;
  void importSave(const std::vector<uint8_t>& bytes) override { gb_->cart->importSave(bytes); }
  void setDmgPalette(const uint32_t* bg, const uint32_t* obj0, const uint32_t* obj1) override;
  std::vector<uint8_t> saveState() override { return gb_->saveState(); }
  void loadState(const std::vector<uint8_t>& s) override { gb_->loadState(s); }

private:
  std::unique_ptr<GB> gb_;
};

}  // namespace indigo::gb
