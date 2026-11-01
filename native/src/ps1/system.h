// O console inteiro: memória (RAM de 2 MB com espelhos, BIOS de 512 KB, scratchpad de 1 KB), barramento com o
// memory control (tempos de acesso por região), controlador de interrupções (I_STAT/I_MASK), escalonador e
// todos os aparelhos. A CPU acessa a RAM e a BIOS por tabelas de páginas de 64 KB (caminho rápido) e o resto
// por ioRead/ioWrite.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../core/state.h"
#include "cpu.h"
#include "dma.h"
#include "gpu.h"
#include "perifericos.h"
#include "sio.h"
#include "timers.h"

namespace indigo::ps1 {

enum IrqBit { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5, IRQ_TMR2 = 6,
              IRQ_SIO0 = 7, IRQ_SIO1 = 8, IRQ_SPU = 9, IRQ_PIO = 10 };

constexpr uint32_t RAM_SIZE = 2 * 1024 * 1024;
constexpr uint32_t BIOS_SIZE = 512 * 1024;
constexpr uint64_t CPU_HZ = 33868800;

class System {
public:
  explicit System(const std::vector<uint8_t>& bios);
  System(const System&) = delete;
  System& operator=(const System&) = delete;
  void reset();

  // tempo em ciclos da CPU
  uint64_t now = 0, nextEvent = 0;
  bool frameDone = false;

  // memória
  std::vector<uint8_t> ramBuf, biosBuf;
  uint8_t* ram = nullptr;
  uint8_t* biosMem = nullptr;
  uint8_t scratch[1024] = {};
  uint8_t* rpage[0x2000] = {};   // páginas de 64 KB do espaço físico (512 MB) para leitura; nulo = E/S
  uint8_t* wpage[0x2000] = {};
  uint8_t rcost[0x2000][3] = {}; // ciclos extras por leitura de 8/16/32 bits
  uint8_t fcost[0x2000] = {};    // ciclos extras por busca de instrução fora da cache
  uint32_t cacheCtrl = 0;
  uint32_t memCtrl[9] = {}, ramSizeReg = 0;
  uint8_t cdCost[3] = {}, spuCost[3] = {}, exp1Cost[3] = {}, exp2Cost[3] = {};

  uint32_t ioRead(uint32_t ph, int size);
  void ioWrite(uint32_t ph, uint32_t v, int size);

  // interrupções
  uint32_t istat = 0, imask = 0;
  void raiseIrq(int bit) { istat |= 1u << bit; updateIrq(); }
  void updateIrq() { cpu.setHwIrq((istat & imask) != 0); }
  void reschedule() { nextEvent = now; }

  // aparelhos
  Cpu cpu;
  Gpu gpu;
  Dma dma;
  Timers timers;
  Sio0 sio;
  Gte gte;
  Spu spu;
  CdRom cdrom;
  Mdec mdec;

  // roda até o próximo vblank (um quadro)
  void runFrame();
  // leva CD/SPU/MDEC até agora (antes de acessar os registradores deles); as IRQ deles chegam pelos onIrq
  void syncPeriph();

  // programa PS-X EXE: direto (sem BIOS) ou depois da BIOS chegar ao menu (side-load)
  bool loadExe(const std::vector<uint8_t>& exe);
  void sideload(const std::vector<uint8_t>& exe) { pendingExe_ = exe; cpu.hookPc = 0x80030000; }
  void onHook();
  // núcleo mínimo na RAM para programas sem BIOS: trata exceções e devolve as chamadas A0/B0/C0
  void installMiniKernel();

  void sync(StateIO& io);

private:
  uint64_t periphLast_ = 0;
  bool dbg_ = false;
  std::vector<uint8_t> pendingExe_;
  void buildPages();
  void updateMemTiming();
  void syncAll();
};

}  // namespace indigo::ps1
