// CPU ARM60 do 3DO: ARMv3 (32 bits, sem Thumb, sem multiplicação longa nem cargas de meia palavra), big-endian,
// 12,5 MHz. Escrito a partir do manual de arquitetura ARM (ARMv3): modos USR/FIQ/IRQ/SVC/ABT/UND com os bancos
// de registradores, CPSR/SPSR, deslocador, LDR/STR com rotação do endereço desalinhado, LDM/STM (com ^),
// MUL/MLA, SWP/SWPB, MRS/MSR, SWI e instrução indefinida (coprocessador inexistente no 3DO).
// As configurações de 26 bits do ARM60 não são usadas pelo 3DO: os bits de modo valem sempre como 32 bits.
//
// O PC lido por uma instrução é o endereço dela + 8 (+12 quando o deslocamento vem de registrador e ao guardar
// o PC com STR/STM), como no ARM6.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::tdo {

class ArmBus {
public:
  virtual ~ArmBus() = default;
  virtual uint32_t read32(uint32_t a) = 0;  // a alinhado em 4
  virtual uint8_t read8(uint32_t a) = 0;
  virtual void write32(uint32_t a, uint32_t v) = 0;  // a alinhado em 4
  virtual void write8(uint32_t a, uint8_t v) = 0;
};

class Arm60 {
public:
  enum Mode : uint32_t { USR = 0x10, FIQ = 0x11, IRQ = 0x12, SVC = 0x13, ABT = 0x17, UND = 0x1B };
  enum : uint32_t { FN = 1u << 31, FZ = 1u << 30, FC = 1u << 29, FV = 1u << 28, FI = 1u << 7, FF = 1u << 6 };

  explicit Arm60(ArmBus& bus) : bus_(bus) { reset(); }

  void reset(uint32_t pc = 0);
  // roda até completar pelo menos 'cycles' ciclos; devolve os ciclos gastos
  int run(int cycles);
  int step();  // uma instrução (ou a entrada numa exceção); devolve os ciclos

  void setIrq(bool on) { irqLine_ = on; }
  void setFiq(bool on) { fiqLine_ = on; }

  // registradores do modo atual (r[15] = endereço da próxima instrução a buscar)
  uint32_t r[16] = {};
  uint32_t cpsr = uint32_t(SVC) | uint32_t(FI) | uint32_t(FF);
  uint32_t spsr() const { return spsrFor(cpsr & 0x1F); }
  uint32_t& spsrRef();
  uint64_t cycles = 0;  // total de ciclos desde o reset
  uint32_t undefinedCount = 0;

  void setMode(uint32_t mode);  // troca de banco (sem mexer no resto do CPSR)
  void writeCpsr(uint32_t v);   // CPSR inteiro, com troca de banco
  void sync(StateIO& io);

private:
  ArmBus& bus_;
  bool irqLine_ = false, fiqLine_ = false;
  // bancos: [0] USR/SYS, [1] FIQ, [2] IRQ, [3] SVC, [4] ABT, [5] UND
  uint32_t bankR13_[6] = {}, bankR14_[6] = {};
  uint32_t fiqR8_[5] = {}, usrR8_[5] = {};
  uint32_t spsr_[6] = {};
  int extra_ = 0;  // ciclos além do básico na instrução atual
  bool branched_ = false;

  static int bankOf(uint32_t mode);
  uint32_t spsrFor(uint32_t mode) const { const int b = bankOf(mode); return b ? spsr_[b] : cpsr; }
  bool cond(uint32_t c) const;
  void exception(uint32_t vector, uint32_t mode, uint32_t lr, bool fiq);
  uint32_t reg(int n, uint32_t pcAdd = 8) const { return n == 15 ? r[15] - 4 + pcAdd : r[n]; }
  void setReg(int n, uint32_t v) { if (n == 15) { r[15] = v & ~3u; branched_ = true; } else r[n] = v; }
  uint32_t& userReg(int n);  // registrador do banco de usuário (LDM/STM com ^)

  uint32_t load32(uint32_t a);
  void dataProc(uint32_t op);
  void multiply(uint32_t op);
  void swap(uint32_t op);
  void singleTransfer(uint32_t op);
  void blockTransfer(uint32_t op);
  void psrTransfer(uint32_t op);
  uint32_t shiftOperand(uint32_t op, bool& carry);
  void undefined();
};

}  // namespace indigo::tdo
