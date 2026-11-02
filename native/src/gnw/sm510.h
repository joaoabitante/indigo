// CPUs Sharp SM5xx de 4 bits dos Game & Watch: SM5A (os primeiros, 1980-81), SM510 (a maioria), SM511 e SM512
// (com controlador de melodia). Relógio de 32768 Hz; uma instrução a cada 2 ciclos do oscilador (16 kHz) ou a
// cada 4 (8 kHz, padrão do SM511/SM512).
//
// Comportamento das instruções, do divisor, do driver do LCD e da melodia baseado no núcleo SM510 do MAME
// (src/devices/cpu/sm510, licença BSD-3-Clause, de hap) e nos manuais da Sharp; código reescrito aqui.
//
// Particularidades:
//  - o contador de programa baixo (PL, 6 bits) é um LFSR: as instruções de uma página não ficam em sequência;
//  - LAX seguidos: só o primeiro carrega; SBM vale só para a instrução seguinte (SM510/SM511);
//  - CEND para a CPU até o sinal de 1 s (gama) ou uma tecla K apertada.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::gnw {

enum class Cpu : uint8_t { SM5A = 0, SM510 = 1, SM511 = 2, SM512 = 3 };

// ligações da CPU com o aparelho
class SmIo {
public:
  virtual ~SmIo() = default;
  virtual uint8_t readK() = 0;       // teclas K1-K4 (1 = ativa)
  virtual int readBA() = 0;          // pino BA (alfa); sem ligação = 1
  virtual int readB() = 0;           // pino B (beta); sem ligação = 1
  virtual void writeS(uint8_t) = 0;  // saída S (SM51x: trava W)
  virtual void writeR(uint8_t) = 0;  // saída R (buzzer; no SM5A também multiplexa as teclas)
};

class Sm5xx {
public:
  Sm5xx(Cpu tipo, SmIo* io) : tipo_(tipo), io_(io) {}
  Cpu tipo() const { return tipo_; }
  void setTipo(Cpu t) { tipo_ = t; }

  // programa (SM5A: 0x740 bytes; SM51x: 4 KB) e ROM de melodia (SM511/SM512, 256 bytes; vazia = sem melodia)
  void setRom(const std::vector<uint8_t>& prog, const std::vector<uint8_t>& melody);
  void setRMask(int bit) { rMask_ = bit; }  // opção de fábrica da saída R: -1 = direta, n = bit n do divisor

  void powerOn();  // zera tudo (como ao pôr as pilhas) e faz o reset
  void reset();    // pino ACL
  // avança `ticks` ciclos do oscilador de 32768 Hz; a cada ciclo chama porTick (pode ser nulo)
  template <typename F>
  void run(int ticks, F&& porTick) {
    for (int t = 0; t < ticks; t++) {
      tickDivider();
      if (--icount_ <= 0) cpuStep();
      porTick();
    }
  }

  // segmentos do LCD (SM51x: grupo a/b/bs/c x comum H1-H4 -> 16 bits; SM5A: grupo O x H1/H2 -> 4 bits).
  // out[deslocamento] como no MAME: SM51x deslocamento = grupo*4 + h; SM5A = grupo*2 + h.
  void lcdOutputs(uint16_t out[32]) const;

  void sync(StateIO& s);

  // depuração e testes
  uint8_t ram(int a) const { return ram_[a & 0x7F]; }
  void setRam(int a, uint8_t v) { ram_[a & 0x7F] = v & 0xF; }
  uint16_t pc() const { return pc_; }
  bool halted() const { return halt_; }
  uint16_t divider() const { return div_; }
  uint8_t acc() const { return acc_; }
  int clkDiv() const { return clkDiv_; }
  // registros internos em ordem fixa (comparação com a régua nos testes)
  std::vector<int> registros() const;

private:
  Cpu tipo_;
  SmIo* io_;
  std::vector<uint8_t> rom_, melody_;
  int rMask_ = -1;
  int prgMask_ = 0xFFF;
  int stackLevels_ = 2;

  // estado
  uint8_t ram_[128] = {};
  uint16_t pc_ = 0, prevPc_ = 0, op_ = 0, prevOp_ = 0;
  uint8_t param_ = 0;
  uint16_t stack_[2] = {};
  uint8_t acc_ = 0, bl_ = 0, bm_ = 0, bmask_ = 0, c_ = 0;
  bool skip_ = false, halt_ = false;
  uint8_t w_ = 0, r_ = 0, rOut_ = 0;
  uint16_t div_ = 0;
  uint8_t gamma_ = 0;
  uint8_t l_ = 0, x_ = 0, y_ = 0, bp_ = 0;
  bool bc_ = false;
  int clkDiv_ = 2;
  int32_t icount_ = 0;
  uint8_t melRd_ = 0, melStep_ = 0, melDutyCount_ = 0, melDutyIndex_ = 0, melAddr_ = 0;
  // SM5A
  uint8_t ox_[9] = {}, o_[9] = {};
  uint8_t mx_ = 0, cb_ = 0;
  bool rsub_ = false;

  bool sm5a() const { return tipo_ == Cpu::SM5A; }
  uint8_t fetch(uint16_t a) const;
  int ramAddr() const;
  uint8_t ramR() const { return ram_[ramAddr()]; }
  void ramW(uint8_t v) { ram_[ramAddr()] = v & 0xF; }
  void incPc();
  void push();
  void pop();
  void branch(uint8_t pu, uint8_t pm, uint8_t pl);
  void tickDivider();
  void clockMelody();
  void cpuStep();
  void exec510();
  void exec511();
  void exec5a();
  bool twoBytes() const;
  uint8_t digit() const;

  // instruções comuns
  void opExc() { const uint8_t a = acc_; acc_ = ramR(); ramW(a); bm_ ^= (op_ & 3); }
  void opIncb();
  void opDecb() { bl_ = (bl_ - 1) & 0xF; skip_ = (bl_ == 0xF); }
  void opRtn0();
  void opLax() { if ((op_ & ~0xF) != (prevOp_ & ~0xF)) acc_ = op_ & 0xF; }
  void opAtr() { r_ = acc_; clockMelody(); }
};

}  // namespace indigo::gnw
