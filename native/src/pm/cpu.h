// CPU do Pokémon Mini: núcleo Epson S1C88 (apelidado "Minx" pela comunidade), 8 bits com operações de 16 bits e
// endereços de 24 bits. Registradores no padrão da documentação da comunidade (pokemon-mini.net / pm-dev-docs):
//   A, B (BA), L, H (HL), I (banco de [HL], [N+nn] e [nnnn]), N (byte do meio de [N+nn]), X/XI, Y/YI, SP, PC,
//   V (banco do PC quando PC >= $8000), U (banco "atrasado": um salto copia U para V), F (flags) e E (exceção).
// Flags: Z=1, C=2, O(V)=4, S=8, BCD=$10, NIBBLE=$20 (modos da soma/subtração de 8 bits), I0=$40, I1=$80.
// Ciclos contados no relógio de 4 MHz (cada ciclo de barramento são 4 relógios), como na tabela de instruções
// da documentação e no núcleo de referência PokeMini. Instruções não documentadas fazem o mesmo que no PokeMini
// (que as mediu no aparelho), para os jogos que tropeçam nelas se comportarem igual.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::pm {

class MinxBus {
public:
  virtual ~MinxBus() = default;
  virtual uint8_t read(uint32_t addr) = 0;   // endereço de 24 bits
  virtual void write(uint32_t addr, uint8_t v) = 0;
  // a CPU mudou algo que pode liberar uma interrupção pendente (flags, U, fim do atraso do U)
  virtual void irqCheck() = 0;
};

class Minx {
public:
  enum : uint8_t { FZ = 1, FC = 2, FO = 4, FS = 8, FBCD = 0x10, FNIB = 0x20 };
  enum Status : uint8_t { RUN = 0, HALT = 1, STOP = 2, IRQ = 3 };

  MinxBus* bus = nullptr;
  uint8_t A = 0, B = 0, L = 0, H = 0, I = 0, N = 0, XI = 0, YI = 0, F = 0xC0, E = 0;
  uint16_t X = 0, Y = 0, SP = 0, PC = 0;
  uint8_t V = 0, U1 = 0, U2 = 0, shiftU = 0;
  uint8_t IR = 0;       // último byte lido da instrução (o barramento "aberto" devolve ele)
  uint8_t status = RUN, irqVector = 0;

  // interrupções só são aceitas fora do atraso do U e com I0/I1 não ambos ligados (como no PokeMini)
  bool irqMaster() const { return shiftU == 0 && (F & 0xC0) != 0xC0; }

  uint16_t BA() const { return (B << 8) | A; }
  uint16_t HL() const { return (H << 8) | L; }

  void reset(bool hard);   // lê o vetor 0 (ligar) ou 1 (reset)
  int step();              // executa uma instrução (ou entra na interrupção pendente); devolve os ciclos
  // pede a interrupção cujo vetor está no byte "vector" (2 × número); ignorado se já há uma esperando
  void requestIrq(uint8_t vector) {
    if (status == IRQ) return;
    status = IRQ;
    irqVector = vector;
  }
  void sync(StateIO& io);

private:
  uint8_t rd(uint32_t a) { return bus->read(a & 0xFFFFFF); }
  void wr(uint32_t a, uint8_t v) { bus->write(a & 0xFFFFFF, v); }
  uint16_t rd16(uint32_t a) { return rd(a) | (rd(a + 1) << 8); }
  uint8_t fetch() {
    const uint32_t a = (PC & 0x8000) ? ((uint32_t(V) << 15) | (PC & 0x7FFF)) : PC;
    PC++;
    IR = rd(a);
    return IR;
  }
  uint16_t fetch16() { const uint8_t lo = fetch(); return lo | (fetch() << 8); }
  void push(uint8_t v) { SP--; wr(SP, v); }
  uint8_t pop() { const uint8_t v = rd(SP); SP++; return v; }

  // endereços
  uint32_t aHL() const { return (uint32_t(I) << 16) | (H << 8) | L; }
  uint32_t aN(uint8_t o) const { return (uint32_t(I) << 16) | (N << 8) | o; }
  uint32_t aAbs(uint16_t a) const { return (uint32_t(I) << 16) | a; }
  uint32_t aX() const { return (uint32_t(XI) << 16) | X; }
  uint32_t aY() const { return (uint32_t(YI) << 16) | Y; }
  uint32_t aXo(int o) const { return (uint32_t(XI) << 16) | uint16_t(X + o); }
  uint32_t aYo(int o) const { return (uint32_t(YI) << 16) | uint16_t(Y + o); }

  void setBA(uint16_t v) { A = v & 0xFF; B = v >> 8; }
  void setHL(uint16_t v) { L = v & 0xFF; H = v >> 8; }
  void setR16(int r, uint16_t v) {  // 0 BA, 1 HL, 2 X, 3 Y
    switch (r & 3) { case 0: setBA(v); break; case 1: setHL(v); break; case 2: X = v; break; default: Y = v; break; }
  }

  void setU(uint8_t v);
  void flagsChanged() { bus->irqCheck(); }

  // aritmética (flags conforme as páginas de instruções da documentação)
  uint8_t add8(uint8_t a, uint8_t b, int c);
  uint8_t sub8(uint8_t a, uint8_t b, int c);
  uint16_t add16(uint16_t a, uint16_t b, int c);
  uint16_t sub16(uint16_t a, uint16_t b, int c);
  uint8_t logic(uint8_t r);  // AND/OR/XOR/TST: Z e S, C e O ficam
  uint8_t inc8(uint8_t v);
  uint8_t dec8(uint8_t v);
  uint16_t inc16(uint16_t v);
  uint16_t dec16(uint16_t v);
  uint8_t shiftRot(int op, uint8_t v);  // CE 80..9F: SAL SHL SAR SHR ROLC ROL RORC ROR
  uint8_t notOp(uint8_t v);
  uint8_t neg(uint8_t v);
  int carry() const { return (F & FC) ? 1 : 0; }

  // saltos: o deslocamento é contado a partir do último byte da instrução; V recebe U
  void jumpRel(uint16_t off) { V = U1; U2 = U1; PC = uint16_t(PC + off - 1); }
  void callRel(uint16_t off) { push(V); push(PC >> 8); push(PC & 0xFF); jumpRel(off); }
  void ret();
  void enterIrq(uint8_t vector, bool pushPc);
  bool cond(int c) const;  // condições do CE E0..FF

  int execCE();
  int execCF();
  int execCFSpecial();
};

}  // namespace indigo::pm
