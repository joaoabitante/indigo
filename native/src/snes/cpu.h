// CPU do SNES: WDC 65816 (dentro do Ricoh 5A22). Modo nativo (A, X e Y de 8 ou 16 bits, banco de dados e de
// programa, página direta móvel) e modo emulação (6502). Cada acesso ao barramento é um ciclo, na ordem da folha
// de dados; os ciclos internos também passam pelo barramento (idle), com o endereço que o chip põe nos pinos.
// A régua são os SingleStepTests do 65816 (registradores, memória e cada ciclo).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::snes {

class CpuBus {
public:
  virtual ~CpuBus() = default;
  virtual uint8_t read(uint32_t addr) = 0;
  virtual void write(uint32_t addr, uint8_t v) = 0;
  virtual void idle(uint32_t addr) = 0;  // ciclo interno (o 5A22 gasta 6 ciclos do relógio mestre)
};

class CPU {
public:
  enum : uint8_t { C = 1, Z = 2, I = 4, D = 8, X = 0x10, M = 0x20, V = 0x40, N = 0x80 };

  CpuBus* bus = nullptr;
  uint16_t a = 0, x = 0, y = 0, s = 0x1FF, d = 0, pc = 0;
  uint8_t dbr = 0, pbr = 0, p = 0x34;
  bool e = true;
  bool waiting = false, stopped = false;  // WAI e STP
  // linhas de interrupção: o NMI é uma borda (o barramento liga nmiPending); o IRQ é um nível
  bool nmiPending = false, irqLine = false;

  void reset();  // sequência de reset (modo emulação, vetor em $FFFC)
  void step();   // uma instrução, ou a interrupção pendente
  void sync(StateIO& io);
  void fixRegisters();  // força o que o modo exige (8 bits, pilha na página 1) — usada depois de mudar e/p

private:
  uint16_t opPc = 0;  // endereço do código da instrução em andamento
  struct Op { uint32_t addr; uint8_t kind; };  // kind: 0 = 24 bits, 1 = banco 0 (16 bits), 2 = página direta do modo emulação
  bool m8() const { return p & M; }
  bool x8() const { return p & X; }
  uint32_t pcAddr() const { return (uint32_t(pbr) << 16) | pc; }
  uint8_t fetch() { const uint8_t v = bus->read(pcAddr()); pc++; return v; }
  uint16_t fetch16() { const uint8_t lo = fetch(); return static_cast<uint16_t>(lo | (fetch() << 8)); }
  // ciclo interno: o 65816 deixa no barramento o endereço do byte seguinte ao código da instrução
  void idle() { bus->idle((uint32_t(pbr) << 16) | static_cast<uint16_t>(opPc + 1)); }
  void idleAt(uint32_t addr) { bus->idle(addr); }
  uint32_t at(const Op& o, int i) const;
  uint8_t rd(const Op& o, int i) { return bus->read(at(o, i)); }
  void wr(const Op& o, int i, uint8_t v) { bus->write(at(o, i), v); }

  // pilha: no modo emulação as instruções antigas ficam na página 1; as novas (N) usam S inteiro
  void push(uint8_t v);
  uint8_t pull();
  void pushN(uint8_t v) { bus->write(s, v); s--; }
  uint8_t pullN() { s++; return bus->read(s); }

  // modos de endereçamento (já fazem os ciclos de busca e os internos)
  void dpPenalty() { if (d & 0xFF) idle(); }
  Op direct(uint16_t off) const;          // página direta com o comportamento do modo emulação
  Op directN(uint16_t off) const { return {static_cast<uint16_t>(d + off), 1}; }
  Op aDp();
  Op aDpX();
  Op aDpY();
  Op aDpInd();
  Op aDpIndX();
  Op aDpIndY(bool write);
  Op aDpIndLong();
  Op aDpIndLongY();
  Op aAbs();
  Op aAbsX(bool write);
  Op aAbsY(bool write);
  Op aLong();
  Op aLongX();
  Op aSr();
  Op aSrIndY();

  void setZN8(uint8_t v) { p = static_cast<uint8_t>((p & ~(Z | N)) | (v ? 0 : Z) | (v & N)); }
  void setZN16(uint16_t v) { p = static_cast<uint8_t>((p & ~(Z | N)) | (v ? 0 : Z) | ((v >> 8) & N)); }
  void setZN(uint16_t v, bool w8) { if (w8) setZN8(static_cast<uint8_t>(v)); else setZN16(v); }

  // operações
  uint16_t readOp(const Op& o, bool w8) { const uint8_t lo = rd(o, 0); return w8 ? lo : static_cast<uint16_t>(lo | (rd(o, 1) << 8)); }
  void writeOp(const Op& o, bool w8, uint16_t v) { wr(o, 0, static_cast<uint8_t>(v)); if (!w8) wr(o, 1, static_cast<uint8_t>(v >> 8)); }
  uint16_t imm(bool w8) { return w8 ? fetch() : fetch16(); }
  void alu(int kind, uint16_t v);  // 0 ORA 1 AND 2 EOR 3 ADC 5 LDA 6 CMP 7 SBC (como os bits 5–7 do opcode)
  void adc(uint16_t v);
  void sbc(uint16_t v);
  void compare(uint16_t r, uint16_t v, bool w8);
  void bit(uint16_t v, bool immediate);
  uint16_t modify(int kind, uint16_t v);  // 0 ASL 1 ROL 2 LSR 3 ROR 4 INC 5 DEC
  void rmw(const Op& o, int kind);
  void tsbTrb(const Op& o, bool set);
  void branch(bool cond);
  void interrupt(uint16_t nativeVector, uint16_t emuVector, bool software, bool brk);
  void loadReg(uint16_t& r, uint16_t v, bool w8) { if (w8) r = static_cast<uint16_t>((r & 0xFF00) | (v & 0xFF)); else r = v; setZN(v, w8); }
  void execute(uint8_t op);
};

}  // namespace indigo::snes
