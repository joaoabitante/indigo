// Motorola 6809 (a CPU do Vectrex, 1,5 MHz) com contagem exata de ciclos por instrução: todas as instruções
// documentadas, os três modos de página (0, 2 = prefixo 0x10, 3 = prefixo 0x11), todos os modos de
// endereçamento (inclusive os indexados indiretos) e as interrupções NMI/FIRQ/IRQ, SWI/SWI2/SWI3, SYNC e CWAI.
//
// As instruções não documentadas seguem o comportamento medido em processadores de verdade (tabela do
// 6809Decoder de David Banks/hoglet67 e o artigo de David Flamand): 01=NEG, 02=NEG/COM conforme C, 05=LSR,
// 0B=DEC com C, 1B=NOP, 38=ANDCC (4 ciclos), 3E=reset por software, 4E/5E=CLR sem mexer em C, 87/C7 e
// 8F/CF (gravação "imediata"), 14/15/CD param a CPU (só sai com reset), prefixos repetidos custam 1 ciclo
// cada e um código inválido nas páginas 2/3 roda a instrução da página 0 com 1 ciclo a mais.
//
// Os ciclos são somados no início de cada instrução (tabela base) e durante a decodificação (modo indexado,
// desvio longo tomado, cada byte empilhado). Assim, quando a instrução acessa a memória, `cyc` já contém o
// total dela, e o barramento sabe em que ciclo o acesso acontece (o 6809 lê/grava o dado nos últimos ciclos
// da instrução): veja Vectrex::ioTime.
#pragma once
#include <cstdint>

#include "../core/state.h"

namespace indigo::vectrex {

class M6809Bus {
public:
  virtual ~M6809Bus() = default;
  virtual uint8_t read(uint16_t a) = 0;
  virtual void write(uint16_t a, uint8_t v) = 0;
};

class M6809 {
public:
  explicit M6809(M6809Bus& bus) : bus_(bus) {}

  enum : uint8_t { CC_C = 0x01, CC_V = 0x02, CC_Z = 0x04, CC_N = 0x08, CC_I = 0x10, CC_H = 0x20, CC_F = 0x40, CC_E = 0x80 };
  enum : uint8_t { RUN = 0, SYNCING = 1, CWAITING = 2, STOPPED = 3 };

  uint8_t a = 0, b = 0, dp = 0, cc = CC_I | CC_F;
  uint16_t x = 0, y = 0, u = 0, s = 0, pc = 0;
  bool irqLine = false, firqLine = false;  // níveis das linhas (true = pedindo interrupção)
  bool nmiArmed = false, nmiPending = false;
  uint8_t state = RUN;
  int cyc = 0;  // ciclos da instrução em andamento (já inclui os que vêm depois do acesso atual)
  int lag = 0;  // num acesso de 16 bits, o primeiro byte acontece 1 ciclo antes do segundo

  void reset();
  void nmi() { if (nmiArmed) nmiPending = true; }
  // executa uma instrução (ou a entrada numa interrupção, ou 1 ciclo de espera em SYNC/CWAI) e devolve os ciclos
  int step();
  void sync(StateIO& io);

private:
  M6809Bus& bus_;

  uint8_t rd(uint16_t a) { return bus_.read(a); }
  void wr(uint16_t a, uint8_t v) { bus_.write(a, v); }
  uint16_t rd16(uint16_t a) {
    lag = 1;
    const uint8_t h = rd(a);
    lag = 0;
    return static_cast<uint16_t>(h << 8 | rd(static_cast<uint16_t>(a + 1)));
  }
  void wr16(uint16_t a, uint16_t v) {
    lag = 1;
    wr(a, static_cast<uint8_t>(v >> 8));
    lag = 0;
    wr(static_cast<uint16_t>(a + 1), static_cast<uint8_t>(v));
  }
  uint8_t fetch() { return rd(pc++); }
  uint16_t fetch16() { const uint16_t v = rd16(pc); pc += 2; return v; }

  uint16_t D() const { return static_cast<uint16_t>(a << 8 | b); }
  void setD(uint16_t v) { a = static_cast<uint8_t>(v >> 8); b = static_cast<uint8_t>(v); }

  void push8(uint16_t& sp, uint8_t v) { wr(--sp, v); }
  void push16(uint16_t& sp, uint16_t v) { wr(--sp, static_cast<uint8_t>(v)); wr(--sp, static_cast<uint8_t>(v >> 8)); }
  uint8_t pull8(uint16_t& sp) { return rd(sp++); }
  uint16_t pull16(uint16_t& sp) { const uint8_t h = rd(sp++); return static_cast<uint16_t>(h << 8 | rd(sp++)); }

  // endereços efetivos (somam os ciclos extras do modo)
  uint16_t eaDirect() { return static_cast<uint16_t>(dp << 8 | fetch()); }
  uint16_t eaExtended() { return fetch16(); }
  uint16_t eaIndexed();
  uint16_t ea(int mode);  // 1 = direto, 2 = indexado, 3 = estendido

  // flags
  void nz8(uint8_t v) { cc = static_cast<uint8_t>((cc & ~(CC_N | CC_Z)) | (v & 0x80 ? CC_N : 0) | (v ? 0 : CC_Z)); }
  void nz16(uint16_t v) { cc = static_cast<uint8_t>((cc & ~(CC_N | CC_Z)) | (v & 0x8000 ? CC_N : 0) | (v ? 0 : CC_Z)); }
  void setF(uint8_t f, bool on) { cc = static_cast<uint8_t>(on ? (cc | f) : (cc & ~f)); }

  // ULA de 8 bits
  uint8_t add8(uint8_t r, uint8_t m, int carry);
  uint8_t sub8(uint8_t r, uint8_t m, int borrow);
  uint16_t add16(uint16_t r, uint16_t m);
  uint16_t sub16(uint16_t r, uint16_t m);
  uint8_t logic8(uint8_t v) { nz8(v); setF(CC_V, false); return v; }
  uint8_t rmw(int op, uint8_t v);  // operações 0x0..0xF da coluna de leitura-modificação-gravação

  uint16_t regGet(int code);
  void regSet(int code, uint16_t v);
  void exg(uint8_t pb);
  void pushRegs(uint16_t& sp, uint16_t& other, uint8_t mask);
  void pullRegs(uint16_t& sp, uint16_t& other, uint8_t mask);
  bool cond(int c) const;
  void interrupt(bool entire, uint16_t vector, uint8_t mask);

  void execPage0(uint8_t op);
  void execPage23(int page, uint8_t op);
};

// ciclos base de cada código (página 0, página 2 e página 3, já com o byte de prefixo); 0 = sem instrução
// documentada nessa posição. Exportado para o teste comparar com a tabela medida no hardware.
int m6809BaseCycles(int page, int op);

}  // namespace indigo::vectrex
