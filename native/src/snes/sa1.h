// Chip SA-1 dos cartuchos de SNES (Super Mario RPG, Kirby Super Star, Kirby's Dream Land 3, PGA Tour 96...).
// É um segundo 65816 a 10,74 MHz dentro do cartucho, com:
//   - I-RAM de 2 KB (rápida, dos dois lados) e BW-RAM (a RAM do cartucho com bateria, também vista como bitmap
//     de 2 ou 4 bits por ponto pelo SA-1);
//   - "Super MMC": a ROM é dividida em blocos de 1 MB trocáveis (até 8 MB);
//   - mensagens e interrupções entre as duas CPUs (com troca dos vetores de NMI/IRQ do SNES);
//   - DMA (ROM/BW-RAM/I-RAM), conversão de bitmap para o formato de blocos do SNES (tipos 1 e 2),
//     multiplicação, divisão, soma de produtos de 40 bits, leitura de bits de tamanho variável e um timer H/V.
// O processador é o mesmo 65816 da CPU do SNES (cpu.h), ligado a um barramento próprio.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"
#include "cpu.h"

namespace indigo::snes {

class SA1 : public CpuBus {
public:
  SA1(const std::vector<uint8_t>& rom, std::vector<uint8_t>& bwram);
  void reset();
  void run(int masterClocks);  // anda junto com o relógio do SNES

  // lado do SNES: devolvem false se o endereço não é do cartucho com SA-1
  bool snesRead(uint32_t addr, uint8_t& v);
  bool snesWrite(uint32_t addr, uint8_t v, bool& bwramChanged);
  bool snesIo(uint16_t a) const { return (a >= 0x2200 && a < 0x2400) || (a >= 0x3000 && a < 0x3800); }
  uint8_t ioRead(uint16_t a, uint8_t openBus);
  void ioWrite(uint16_t a, uint8_t v);
  bool snesIrq() const { return scpuFlags_ & scpuSie_ & 0xA0; }
  void sync(StateIO& io);

  CPU cpu;
  bool dirty = false;  // o SA-1 mudou a BW-RAM (save da bateria)

  // barramento do SA-1
  uint8_t read(uint32_t addr) override;
  void write(uint32_t addr, uint8_t v) override;
  void idle(uint32_t) override { debt_ -= 2; }

private:
  bool halted() const { return ctrl_ & 0x60; }
  uint8_t romRead(uint32_t addr) const;
  uint8_t bwramRead(uint32_t off, bool bitmap) const;
  void bwramWrite(uint32_t off, uint8_t v, bool bitmap);
  uint8_t iramRead(uint32_t off) const { return iram_[off & 0x7FF]; }
  uint8_t sa1RegRead(uint16_t a);
  void sa1RegWrite(uint16_t a, uint8_t v);
  void sharedWrite(uint16_t a, uint8_t v);
  void updateIrqs();
  void dma();
  void charConv2();
  uint8_t charConv1(uint32_t off);
  uint8_t varRead(uint32_t addr);
  void timerTick(int ticks);

  const std::vector<uint8_t>& rom_;
  std::vector<uint8_t>& bw_;
  uint8_t iram_[0x800] = {};
  int64_t debt_ = 0;  // ciclos do relógio mestre que o SA-1 ainda pode gastar
  int timerFrac_ = 0;

  // registradores
  uint8_t ctrl_ = 0x20, scpuSie_ = 0, sa1Sie_ = 0, scpuCtrl_ = 0, timerCtrl_ = 0;
  uint8_t scpuFlags_ = 0, sa1Flags_ = 0;
  bool resetFlag_ = true, nmiPrev_ = false;
  uint16_t crv_ = 0, cnv_ = 0, civ_ = 0, snv_ = 0, siv_ = 0;
  uint16_t hpos_ = 0, vpos_ = 0, hcount_ = 0, vcount_ = 0, hcr_ = 0, vcr_ = 0;
  bool bankHi_[4] = {};
  uint8_t bankRom_[4] = {0, 1, 2, 3};
  uint8_t bwSnes_ = 0, bwSa1_ = 0, iramWriteSnes_ = 0, iramWriteSa1_ = 0;
  bool bwBitmap_ = false, bw2bpp_ = false;
  uint8_t dmaCtrl_ = 0, ccParam_ = 0;
  uint32_t src_ = 0, dst_ = 0;
  uint16_t dmaCount_ = 0;
  bool cc1Active_ = false;
  uint8_t cc2Line_ = 0, brf_[16] = {};
  uint8_t mathCtrl_ = 0, mathOverflow_ = 0;
  uint16_t mathA_ = 0, mathB_ = 0;
  uint64_t mathRes_ = 0;
  uint32_t vda_ = 0;
  uint8_t vbit_ = 0, vlen_ = 16;
  bool vauto_ = false;
};

}  // namespace indigo::snes
