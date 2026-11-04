// CPU ARM do Nintendo DS: a mesma classe serve ao ARM7TDMI (ARMv4T) e ao ARM946E-S (ARMv5TE).
// Partiu do interpretador do núcleo do GBA (src/gba/cpu.cpp), com a memória atrás de uma interface (cada CPU do
// DS tem o seu mapa) e as instruções novas do ARMv5TE: BLX, CLZ, QADD/QSUB/QDADD/QDSUB, SMLAxy/SMULxy/SMLAWy/
// SMULWy/SMLALxy, LDRD/STRD, PLD, BKPT, MRC/MCR (CP15) e o salto com troca de modo em LDR/LDM/POP para o PC.
// Convenção do pipeline: durante a execução de uma instrução no endereço A, r[15] = A + 8 (ARM) ou A + 4 (Thumb).
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::nds {

enum : int { M_USR = 0x10, M_FIQ = 0x11, M_IRQ = 0x12, M_SVC = 0x13, M_ABT = 0x17, M_UND = 0x1B, M_SYS = 0x1F };

// memória vista por uma CPU
class ArmBus {
public:
  virtual ~ArmBus() = default;
  virtual uint32_t read32(uint32_t a) = 0;
  virtual uint32_t read16(uint32_t a) = 0;
  virtual uint32_t read8(uint32_t a) = 0;
  virtual void write32(uint32_t a, uint32_t v) = 0;
  virtual void write16(uint32_t a, uint32_t v) = 0;
  virtual void write8(uint32_t a, uint32_t v) = 0;
  // trecho de memória comum onde o código pode ser lido direto (RAM, TCM, WRAM, BIOS); nullptr = passa por read
  // [lo, hi] = faixa de endereços válida (hi inclusive, hi - lo >= 3); mask = espelhamento dentro do trecho
  virtual const uint8_t* codePage(uint32_t a, uint32_t& lo, uint32_t& hi, uint32_t& mask) = 0;
  // SWI tratado em alto nível; devolve false se a BIOS real deve tratar
  virtual bool swi(int n, uint32_t addrOfSwi) = 0;
  // coprocessador 15 (só o ARM9)
  virtual uint32_t cp15Read(uint32_t, uint32_t, uint32_t) { return 0; }
  virtual void cp15Write(uint32_t, uint32_t, uint32_t, uint32_t) {}
};

class Arm {
public:
  Arm(ArmBus& b, bool armv5) : bus(b), v5(armv5) {}

  ArmBus& bus;
  bool v5;
  uint32_t r[16] = {};
  uint32_t bankR13[6] = {}, bankR14[6] = {}, spsrBank[6] = {};
  uint32_t usrHi[5] = {}, fiqHi[5] = {};
  int n = 0, z = 0, c = 0, v = 0, q = 0, i = 1, f = 1, t = 0, mode = M_SVC;
  uint32_t nextPC = 0;
  bool branched = false;
  bool halted = false;
  bool irqLine = false;      // IME && (IE & IF)
  bool forceIrq = false;     // IntrWait em alto nível: aceita a IRQ mesmo com o bit I ligado (a BIOS real liga)
  int costShift = 1;         // ciclos por instrução << costShift (aproxima esperas de memória)
  uint32_t excBase = 0;      // 0 ou 0xFFFF0000 (vetores altos do ARM9)
  uint32_t undefCount = 0;   // instruções indefinidas executadas (depuração)
  int shC = 0;
  uint32_t mapGen = ~0u;     // geração do mapa de memória do código guardado em cp*

  uint32_t cpsr() const;
  void setCpsr(uint32_t val);
  uint32_t spsr() const;
  void setSpsr(uint32_t val);
  void switchMode(int newMode);
  void reset();
  bool checkCond(int cond) const;
  // executa instruções até gastar "budget" ciclos (ou parar); devolve os ciclos gastos
  int run(int budget, uint32_t gen);
  int step();
  void setPC(uint32_t val) { r[15] = val; branched = true; }
  void irq();
  void sync(StateIO& io);

  using Op = int (Arm::*)(uint32_t);

private:
  uint32_t fetch32(uint32_t a);
  uint32_t fetch16(uint32_t a);
  void refill();
  void exception(int newMode, uint32_t vec, uint32_t ret);
  int undefinedInstr(uint32_t);
  uint32_t shiftImm(int type, uint32_t v, int amt);
  uint32_t shiftReg(int type, uint32_t v, int amt);
  void setNZ(uint32_t res) { n = res >> 31; z = res == 0; }
  uint32_t add(uint32_t a, uint32_t b, bool s);
  uint32_t adc(uint32_t a, uint32_t b, bool s);
  uint32_t sub(uint32_t a, uint32_t b, bool s);
  uint32_t sbc(uint32_t a, uint32_t b, bool s);
  uint32_t ldrWord(uint32_t addr);
  uint32_t ldrHalf(uint32_t addr);
  uint32_t ldrSHalf(uint32_t addr);
  void loadPC(uint32_t val);  // LDR/LDM/POP no PC: no ARMv5 o bit 0 escolhe Thumb

  static Op decodeArm(int key, bool v5);
  static Op decodeThumb(uint32_t ins, bool v5);
  const Op* armLut_;
  const Op* thumbLut_;
  static const Op* armLut(bool v5);
  static const Op* thumbLut(bool v5);

  // código lido direto da memória
  const uint8_t* cp_ = nullptr;
  uint32_t cpLo_ = 0, cpLen_ = 0, cpMask_ = 0;  // [lo, lo+len) com folga de 4 bytes no fim

  int armDP(uint32_t ins);
  int armMrs(uint32_t ins);
  int armMsr(uint32_t ins);
  int armBx(uint32_t ins);
  int armBlxReg(uint32_t ins);
  int armClz(uint32_t ins);
  int armQ(uint32_t ins);
  int armDspMul(uint32_t ins);
  int armBkpt(uint32_t ins);
  int armMul(uint32_t ins);
  int armMulLong(uint32_t ins);
  int armSwp(uint32_t ins);
  int armHalf(uint32_t ins);
  int armSdt(uint32_t ins);
  int armBlock(uint32_t ins);
  int armBranch(uint32_t ins);
  int armCop(uint32_t ins);
  int armSwi(uint32_t ins);
  int thShift(uint32_t ins);
  int thAddSub(uint32_t ins);
  int thImm(uint32_t ins);
  int thAlu(uint32_t ins);
  int thHi(uint32_t ins);
  int thLdrPc(uint32_t ins);
  int thLsReg(uint32_t ins);
  int thLsSign(uint32_t ins);
  int thLsImm(uint32_t ins);
  int thLsHalf(uint32_t ins);
  int thLsSp(uint32_t ins);
  int thAddr(uint32_t ins);
  int thAddSp(uint32_t ins);
  int thPushPop(uint32_t ins);
  int thLdmStm(uint32_t ins);
  int thBcond(uint32_t ins);
  int thSwi(uint32_t ins);
  int thB(uint32_t ins);
  int thBlHi(uint32_t ins);
  int thBlLo(uint32_t ins);
  int thBlxLo(uint32_t ins);
  int thBkpt(uint32_t ins);
};

}  // namespace indigo::nds
