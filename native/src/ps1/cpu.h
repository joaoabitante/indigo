// CPU R3000A do PlayStation (interpretador): MIPS I com atraso de carga (load delay slot) e de desvio
// (branch delay slot), exceções e COP0 (SR, CAUSE, EPC, BadVaddr, interrupções de software e de hardware),
// cache de instruções aproximada (só para o tempo, com o "isolate cache" da BIOS) e o COP2 (GTE) ligado pelo
// System. Tempo em ciclos de 33,8688 MHz: 1 por instrução mais a busca (fora da cache) e os acessos à memória
// pelos tempos de cada região (System::readCost).
#pragma once
#include <cstdint>
#include <string>

#include "../core/state.h"

namespace indigo::ps1 {

class System;

class Cpu {
public:
  explicit Cpu(System& s) : sys_(s) {}
  void reset();
  // executa até sys.now >= sys.nextEvent
  void run();
  // linha de interrupção de hardware (I_STAT & I_MASK != 0): bit 10 do CAUSE
  void setHwIrq(bool on) {
    if (on) cause |= 0x400; else cause &= ~0x400u;
    updateIrq();
  }
  // posição de partida de um programa (carga direta do PS-X EXE)
  void jump(uint32_t addr) { pc = addr; npc = addr + 4; ldReg = 0; inDelay = false; branched = false; }
  void sync(StateIO& io);

  uint32_t r[32] = {};
  uint32_t hi = 0, lo = 0, pc = 0, npc = 0;
  // COP0
  uint32_t sr = 0, cause = 0, epc = 0, badv = 0;
  uint32_t bpc = 0, bda = 0, tar = 0, dcic = 0, bdam = 0, bpcm = 0;
  // caixa de saída da TTY da BIOS (putchar), usada pelos testes
  std::string tty;
  // endereço que dispara o gancho (carga do .exe depois da BIOS chegar ao menu); 1 = nenhum
  uint32_t hookPc = 1;

private:
  System& sys_;
  uint32_t ldReg = 0, ldVal = 0;      // carga pendente (registrador 0 = nenhuma)
  uint32_t curPc = 0;
  bool inDelay = false, branched = false;
  bool irqPending = false;
  uint64_t hiloReady = 0;             // multiplicação/divisão em andamento: MFHI/MFLO esperam
  uint64_t wbReady = 0;               // buffer de escrita da RAM ocupado até este ciclo
  uint64_t gteReady = 0;              // comando do GTE em andamento: o próximo acesso ao COP2 espera
  void gteWait();
  void gteStart(uint32_t cmd);
  // cache de instruções: 256 linhas de 16 bytes; etiqueta = endereço >> 12, bits válidos por palavra
  uint32_t itag[256] = {};
  uint8_t ivalid[256] = {};

  void updateIrq() { irqPending = (sr & 1) && (cause & sr & 0xFF00); }
  void exception(uint32_t code, uint32_t copn = 0);
  void execute(uint32_t op);
  void execSpecial(uint32_t op);
  void execCop0(uint32_t op);
  void execCop2(uint32_t op);
  void hook();
  uint32_t fetch(uint32_t addr);
  void setReg(uint32_t i, uint32_t v) {
    r[i] = v;
    if (i == ldReg) ldReg = 0;  // escrita na janela do atraso vence a carga pendente
  }
  void branch(uint32_t target) { npc = target; branched = true; }
  void load(uint32_t reg, uint32_t v) {
    // carga atrasada: vale depois da próxima instrução
    nextLdReg = reg; nextLdVal = v;
  }
  uint32_t nextLdReg = 0, nextLdVal = 0;
  bool memRead(uint32_t addr, int size, uint32_t& out);
  bool memWrite(uint32_t addr, uint32_t v, int size);
};

}  // namespace indigo::ps1
