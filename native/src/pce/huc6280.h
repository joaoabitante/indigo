// CPU HuC6280 do PC Engine: 65C02 da Hudson com instruções extras (transferência em bloco TII/TDD/TIN/TIA/TAI,
// ST0/ST1/ST2, TAM/TMA, CSL/CSH, SET, SAX/SAY/SXY, CLA/CLX/CLY, BSR, TST, BBR/BBS/RMB/SMB), MMU de 8 bancos de
// 8 KB (endereço físico de 21 bits) e duas velocidades (1,79 MHz = 12 ciclos do relógio mestre por ciclo,
// 7,16 MHz = 3 ciclos). Ciclo a ciclo como os SingleStepTests do HuC6280 (gerados pelo Mesen 2).
//
// O tempo é contado em ciclos do relógio mestre (21,477 MHz). Cada acesso ao barramento avança o relógio
// antes de acontecer; quando o relógio passa de nextEvent, o sistema é chamado (sync) para pôr em dia o
// vídeo, o temporizador etc. As interrupções são amostradas no começo de cada ciclo, como no chip.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::pce {

// bancos sem ponteiro direto (registradores de E/S, mapeadores, BRAM) passam pelo sistema
struct CpuBus {
  virtual ~CpuBus() = default;
  virtual uint8_t slowRead(uint8_t bank, uint16_t off) = 0;
  virtual void slowWrite(uint8_t bank, uint16_t off, uint8_t v) = 0;
  virtual void stWrite(int port, uint8_t v) = 0;  // ST0/ST1/ST2: porta 0, 2 ou 3 do VDC
  virtual void sync() = 0;                        // o relógio chegou a nextEvent
};

class HuC6280 {
public:
  enum : uint8_t { FC = 1, FZ = 2, FI = 4, FD = 8, FB = 16, FT = 32, FV = 64, FN = 128 };
  enum : uint8_t { IRQ2 = 1, IRQ1 = 2, TIQ = 4 };

  uint8_t a = 0, x = 0, y = 0, s = 0, p = FI;
  uint16_t pc = 0;
  uint8_t mpr[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t mprBuf = 0;           // último valor de TAM/TMA (TMA #0 devolve ele)
  bool fast = false;            // CSH: 7,16 MHz
  int64_t clk = 0;              // relógio mestre
  int64_t nextEvent = INT64_MAX;
  uint8_t irqActive = 0, irqDisabled = 0;  // linhas ativas e máscara ($1402)

  const uint8_t* rd[256] = {};  // leitura direta por banco físico (nulo = slowRead)
  uint8_t* wr[256] = {};        // escrita direta (nulo = slowWrite)
  CpuBus* bus = nullptr;

  void reset();
  void step();                  // uma instrução (e a interrupção pendente, se houver)

  int cycle() const { return fast ? 3 : 12; }
  // ciclo extra sem amostrar interrupções (atraso do VDC/VCE na leitura)
  void penalty() { clk += cycle(); if (clk >= nextEvent) bus->sync(); }
  // ciclo inteiro sem acesso (atraso do VDC/VCE depois da escrita)
  void idle() { tick(); }
  // espera de 3 ciclos mestres (CPU parada esperando a VRAM)
  void stall() { clk += 3; if (clk >= nextEvent) bus->sync(); }

  uint8_t peek(uint16_t addr);  // leitura sem efeitos para depuração/testes (só bancos diretos)
  void sync(StateIO& io);

private:
  uint8_t pend_ = 0;
  bool tflag_ = false;
  uint16_t op_ = 0, op2_ = 0, op3_ = 0;

  void tick() {
    clk += fast ? 3 : 12;
    if (clk >= nextEvent) bus->sync();
    pend_ = (p & FI) ? 0 : static_cast<uint8_t>(irqActive & ~irqDisabled);
  }
  uint8_t read(uint16_t addr) {
    tick();
    const uint8_t bank = mpr[addr >> 13];
    if (const uint8_t* r = rd[bank]) return r[addr & 0x1FFF];
    return bus->slowRead(bank, addr & 0x1FFF);
  }
  void write(uint16_t addr, uint8_t v) {
    tick();
    const uint8_t bank = mpr[addr >> 13];
    if (uint8_t* w = wr[bank]) w[addr & 0x1FFF] = v;
    else bus->slowWrite(bank, addr & 0x1FFF, v);
  }
  uint8_t fetch() { return read(pc++); }
  uint16_t fetch16() { uint8_t lo = fetch(); return static_cast<uint16_t>(lo | (fetch() << 8)); }
  void dummy() { read(pc); }
  uint16_t zpWord(uint8_t zp) {
    uint8_t lo = read(0x2000 | zp);
    return static_cast<uint16_t>(lo | (read(0x2000 | static_cast<uint8_t>(zp + 1)) << 8));
  }
  void push(uint8_t v) { write(0x2100 | s, v); s--; }
  uint8_t pop() { s++; return read(0x2100 | s); }
  void nz(uint8_t v) { p = static_cast<uint8_t>((p & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ)); }

  void interrupt(bool brk);
  void branch(bool c);
  void blockStart();
  void blockEnd();
  uint8_t operand(int mode);
  void logic(int kind, uint8_t v);
  void adc(uint8_t v);
  void sbc(uint8_t v);
  void cmp(uint8_t r, uint8_t v);
  void rmw(int kind);
};

}  // namespace indigo::pce
