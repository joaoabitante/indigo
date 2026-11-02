// Processadores RISC do Atari Jaguar: o GPU (dentro do Tom, 4 KB de RAM local em 0xF03000) e o DSP (dentro do
// Jerry, 8 KB em 0xF1B000). Os dois têm o mesmo conjunto de 64 instruções de 16 bits (com poucas diferenças),
// 32 registros em dois bancos, bandeiras Z/C/N, desvios com uma instrução de atraso e 5 (GPU) ou 6 (DSP)
// interrupções. Cada instrução conta 1 ciclo (DIV e MMULT contam mais); a fila (pipeline) não é imitada.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::jaguar {

class Jaguar;

class Risc {
public:
  Risc(Jaguar& j, bool dsp);

  void reset();
  // roda até o relógio (em ciclos do sistema, 26,59 MHz) alcançar `until`
  void run(int64_t until);
  bool running() const { return ctrl & 1; }
  void setIrq(int n);  // pede a interrupção n (só é registrada com o processador rodando)

  // registros de controle (offset & 0x1F a partir de 0xF02100 / 0xF1A100)
  uint32_t regRead(uint32_t off);
  void regWrite(uint32_t off, uint32_t v);  // escrita de 32 bits
  // meia palavra (o 68000 escreve os registros de 32 bits em duas metades): `lo` = metade de baixo
  void regWrite16(uint32_t off, uint16_t v, bool lo);

  bool local(uint32_t a) const { return a >= ramBase && a < ramBase + static_cast<uint32_t>(ram.size()); }
  uint32_t ramRead32(uint32_t a) const {
    const uint8_t* p = &ram[(a - ramBase) & ramMask & ~3u];
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }
  void ramWrite32(uint32_t a, uint32_t v) {
    uint8_t* p = &ram[(a - ramBase) & ramMask & ~3u];
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
  }

  void sync(StateIO& io);

  const bool dsp;
  int64_t clk = 0;
  uint32_t r[64] = {};  // banco 0 = r[0..31], banco 1 = r[32..63]
  uint32_t pc = 0;
  uint32_t flags = 0;   // bits fora de Z/C/N (habilitações, IMASK, REGPAGE)
  int fz = 0, fc = 0, fn = 0;
  uint32_t ctrl = 0;    // G_CTRL/D_CTRL (só os bits que se escrevem)
  uint32_t lat = 0;     // interrupções pendentes (bit n = interrupção n)
  uint32_t hidata = 0, remain = 0, divctrl = 0, mtxc = 0, mtxa = 0, endian = 0, modulo = 0;
  int64_t acc = 0;      // acumulador do MAC (32 bits no GPU, 40 no DSP)
  std::vector<uint8_t> ram;
  uint32_t ramBase, ramMask;

private:
  uint32_t& R(int n) { return r[bank_ + n]; }
  void updateBank();
  uint32_t enables() const;
  void checkIrq();
  void exec(uint16_t op);
  uint16_t fetch(uint32_t a);
  uint32_t load32(uint32_t a);
  uint32_t load16(uint32_t a);
  uint32_t load8(uint32_t a);
  void store32(uint32_t a, uint32_t v);
  void store16(uint32_t a, uint32_t v);
  void store8(uint32_t a, uint32_t v);
  bool cond(int cc) const;
  void zn(uint32_t v) { fz = v == 0; fn = v >> 31; }

  Jaguar& j_;
  int bank_ = 0;
  int extra_ = 0;  // ciclos a mais da instrução atual
  bool inDelay_ = false;  // rodando a instrução de atraso de um desvio
};

}  // namespace indigo::jaguar
