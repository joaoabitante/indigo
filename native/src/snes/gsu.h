// Super FX (GSU-1 / GSU-2 / MARIO chip): o processador RISC do cartucho de Star Fox, Stunt Race FX, Yoshi's Island,
// Doom e Vortex. 16 registradores de 16 bits, instruções de 1 byte com os prefixos ALT1/ALT2/ALT3, WITH/FROM/TO,
// pipeline de 1 byte (a instrução depois de um desvio sempre roda), cache de instruções de 512 bytes, buffers de
// leitura da ROM (R14) e da RAM, e o PLOT com o cache de pixels que grava telhas de 2, 4 ou 8 bits na RAM do
// cartucho. A régua são as ROMs de teste públicas do PeterLemon (CHIP/GSU: uma por instrução, com PASS/FAIL, e as
// demos de PLOT com as telas de referência).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::snes {

class GSU {
public:
  GSU(const std::vector<uint8_t>& rom, std::vector<uint8_t>& ram) : rom_(rom), ram_(ram) {}
  void reset();
  void run(int64_t masterClocks);  // anda junto com a CPU (o relógio do GSU é o mestre de 21,477 MHz)
  bool running() const { return sfr & G; }
  bool irq() const { return (sfr & IRQ) && !(cfgr & 0x80); }
  bool romBusy() const { return (sfr & G) && (scmr & 0x10); }  // a CPU do SNES não lê a ROM
  bool ramBusy() const { return (sfr & G) && (scmr & 0x08); }

  // registradores vistos pelo SNES ($3000–$34FF nos bancos $00–$3F e $80–$BF)
  uint8_t readReg(uint16_t a);
  void writeReg(uint16_t a, uint8_t v);
  void sync(StateIO& io);

  // leitura da ROM do jeito que o GSU vê (bancos $00–$3F em blocos de 32 KB, $40–$5F inteiros)
  uint8_t romAt(uint32_t addr) const;

private:
  enum : uint16_t { Z = 0x0002, CY = 0x0004, S = 0x0008, OV = 0x0010, G = 0x0020, R = 0x0040,
                    ALT1 = 0x0100, ALT2 = 0x0200, B = 0x1000, IRQ = 0x8000 };
  struct PixelCache { uint16_t offset = 0xFFFF; uint8_t bitpend = 0; uint8_t data[8] = {}; };

  void execute(uint8_t op);
  uint8_t pipe();          // operando: o byte do pipeline, e busca o seguinte
  uint8_t fetch(uint16_t addr);
  uint8_t busRead(uint32_t addr);
  void busWrite(uint32_t addr, uint8_t v);
  uint8_t ramRead(uint16_t a) { step(clsr ? 5 : 6); return busRead((uint32_t(0x70 + rambr) << 16) | a); }
  void ramWrite(uint16_t a, uint8_t v) { busWrite((uint32_t(0x70 + rambr) << 16) | a, v); }
  void step(int n) { clocks_ -= n; }
  void flushCache() { for (bool& v : cacheValid) v = false; }
  void setReg(int n, uint16_t v);
  uint16_t& sr() { return r[sreg]; }
  void dr(uint16_t v) { setReg(dreg, v); }
  void resetPrefix() { sfr &= static_cast<uint16_t>(~(B | ALT1 | ALT2)); sreg = dreg = 0; }
  void flags(uint16_t v) { setFlag(S, v & 0x8000); setFlag(Z, v == 0); }
  void setFlag(uint16_t f, bool on) { sfr = static_cast<uint16_t>(on ? (sfr | f) : (sfr & ~f)); }
  bool alt1() const { return sfr & ALT1; }
  bool alt2() const { return sfr & ALT2; }
  uint8_t color(uint8_t source) const;
  void plot(uint8_t x, uint8_t y);
  uint8_t rpix(uint8_t x, uint8_t y);
  uint32_t tileAddr(uint8_t x, uint8_t y) const;
  int bpp() const { const int md = scmr & 3; return 2 << (md - (md >> 1)); }
  void flushPixels(PixelCache& c);
  void updateRomBuffer() { romdr = busRead((uint32_t(rombr) << 16) | r[14]); }

  const std::vector<uint8_t>& rom_;
  std::vector<uint8_t>& ram_;
  int64_t clocks_ = 0;

  uint16_t r[16] = {};
  uint16_t sfr = 0;
  uint8_t pbr = 0, rombr = 0, rambr = 0, cfgr = 0, scbr = 0, clsr = 0, scmr = 0, colr = 0, por = 0;
  uint16_t cbr = 0, ramaddr = 0;
  uint8_t romdr = 0, pipeline = 0x01;
  uint8_t sreg = 0, dreg = 0;
  bool r15Modified = false, r14Modified = false;
  uint8_t cache[512] = {};
  bool cacheValid[32] = {};
  PixelCache pixel[2];
};

}  // namespace indigo::snes
