// CPU Motorola 68000 do Mega Drive, com a fila de pré-busca (IR/IRC) e a contagem de ciclos de cada instrução
// (cada acesso ao barramento = 4 ciclos, mais os ciclos internos do microcódigo). Validada contra os
// SingleStepTests do 68000 (registradores, memória, ciclos), inclusive as exceções de erro de endereço (grupo 0)
// e as bandeiras "indefinidas" de ABCD/SBCD/NBCD, CHK e divisões, que seguem o chip de verdade.
//
// Convenção do PC (igual à dos testes): no começo de cada instrução, `pc` é o endereço da instrução que está
// em IR, e IRC tem a palavra seguinte (pc + 2). O PC tem 32 bits; o barramento usa só os 24 de baixo.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::md {

class M68kBus {
public:
  virtual ~M68kBus() = default;
  // endereços já limitados a 24 bits; read16/write16 sempre com endereço par
  virtual uint8_t read8(uint32_t a) = 0;
  virtual uint16_t read16(uint32_t a) = 0;
  virtual void write8(uint32_t a, uint8_t v) = 0;
  virtual void write16(uint32_t a, uint16_t v) = 0;
  // reconhecimento de interrupção: devolve o vetor (o Mega Drive usa autovetor 24 + nível)
  virtual int intAck(int level) { return 24 + level; }
  // instrução RESET (sinal de reset dos periféricos)
  virtual void resetDevices() {}
  // TAS: no Mega Drive o ciclo de escrita do TAS não chega à memória (o barramento não aceita
  // leitura-modificação-escrita); jogos como Gargoyles dependem disso
  virtual bool tasWrites() { return true; }
};

class M68k {
public:
  explicit M68k(M68kBus& b);

  uint32_t d[8] = {}, a[8] = {};  // a[7] = pilha ativa
  uint32_t otherSp = 0;           // a pilha inativa (USP no modo supervisor, SSP no modo usuário)
  uint32_t pc = 0;
  uint16_t ir = 0, irc = 0;
  // bandeiras (0/1) e o byte de cima do SR
  int fx = 0, fn = 0, fz = 0, fv = 0, fc = 0;
  int sMode = 1, tMode = 0, mask = 7;
  bool stopped = false, halted = false;
  int ipl = 0;            // nível de interrupção pedido pelos periféricos (0 = nenhum)
  int cycles = 0;         // ciclos da instrução em andamento (o barramento lê para saber a hora exata)

  void reset();           // lê SSP e PC dos vetores 0 e 1
  int step();             // executa uma instrução (ou uma exceção); devolve os ciclos gastos

  uint16_t getSR() const;
  void setSR(uint16_t v);
  uint32_t usp() const { return sMode ? otherSp : a[7]; }
  uint32_t ssp() const { return sMode ? a[7] : otherSp; }
  void setUsp(uint32_t v) { if (sMode) otherSp = v; else a[7] = v; }
  void setSsp(uint32_t v) { if (sMode) a[7] = v; else otherSp = v; }
  void sync(StateIO& io);

private:
  struct AddressError { uint32_t addr; bool read; bool program; };

  // barramento
  uint32_t rd(uint32_t ad, int sz);
  void wr(uint32_t ad, int sz, uint32_t v);
  uint16_t rdProg(uint32_t ad);
  void idle(int n) { cycles += n; }
  uint16_t ext();                 // palavra de extensão (consome IRC e busca a próxima)
  uint32_t ext32() { const uint32_t h = ext(); return (h << 16) | ext(); }
  void prefetch();                // fim de instrução: IR <- IRC, busca a próxima palavra
  void jump(uint32_t to);         // desvio: enche a fila no destino (2 leituras)
  void push16(uint16_t v) { a[7] -= 2; wr(a[7], 2, v); }
  void push32(uint32_t v) { a[7] -= 4; wr(a[7], 4, v); }
  uint32_t pop16() { const uint32_t v = rd(a[7], 2); a[7] += 2; return v; }
  uint32_t pop32() { const uint32_t v = rd(a[7], 4); a[7] += 4; return v; }

  // modos de endereçamento
  uint32_t eaAddr(int mode, int reg, int sz, bool moveDst = false);
  uint32_t ctrlAddr(int mode, int reg, int& extWords);
  uint32_t indexed(uint32_t base, uint16_t e) const;
  uint32_t readEA(int mode, int reg, int sz);
  void writeReg(int reg, int sz, uint32_t v);

  // exceções
  void exception(int vec, uint32_t pushPc, int idleFirst);
  void addressError(const AddressError& e);
  void illegal(int vec = 4) { exception(vec, pc, 4); }
  bool privileged();
  void setSupervisor(bool s);
  void interrupt(int level);

  bool cond(int cc) const;
  void setNZ(uint32_t r, int sz);
  uint32_t add(uint32_t s, uint32_t dd, int sz, int x, bool ext);
  uint32_t sub(uint32_t s, uint32_t dd, int sz, int x, bool ext, bool setX = true);
  void execute();
  void line0(); void line4(); void line5(); void line8(); void line9D(bool isAdd); void lineB(); void lineC();
  void lineE(); void move(int sz);
  void movem(); void shiftReg(); void shiftMem();
  uint32_t shift(int type, bool left, uint32_t v, int sz, int cnt);
  void divu(int mode, int reg); void divs(int mode, int reg);
  void mulu(int mode, int reg); void muls(int mode, int reg);
  uint8_t abcd(uint8_t s, uint8_t dd); uint8_t sbcd(uint8_t s, uint8_t dd);

  M68kBus& bus;
  uint32_t faultPc_ = 0;   // PC guardado no quadro do erro de endereço
  bool nmiPrev_ = false;
  uint16_t opcode_ = 0;   // instrução em execução (vai no quadro do erro de endereço)
  int traceNow_ = 0;
};

}  // namespace indigo::md
