// Montador mínimo do CP1610, em C++: serve para a EXEC livre do Índigo (freebios.cpp) e para os programas
// de teste (tools/t7_intv.cpp). Cada método emite uma instrução; rótulos são resolvidos em finish().
// Sintaxe dos nomes igual à do manual da GI (MVII #valor, Rn → mvii(valor, n); MVO Rn, endereço → mvo(n, end)).
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace indigo::intv {

class Asm1610 {
public:
  enum Cond { B = 0, BC = 1, BOV = 2, BPL = 3, BEQ = 4, BLT = 5, BLE = 6, BUSC = 7,
              NOPP = 8, BNC = 9, BNOV = 10, BMI = 11, BNEQ = 12, BGE = 13, BGT = 14, BESC = 15 };

  explicit Asm1610(uint16_t org) : org_(org) {}

  uint16_t here() const { return static_cast<uint16_t>(org_ + w_.size()); }
  uint16_t org() const { return org_; }
  void label(const std::string& n) {
    if (labels_.count(n)) throw std::runtime_error("rótulo repetido: " + n);
    labels_[n] = here();
  }
  void dw(uint16_t v) { w_.push_back(v); }
  void dwl(const std::string& l) { fix(l, ABS); dw(0); }  // endereço de um rótulo
  // BIDECLE do cabeçalho do cartucho: byte baixo e byte alto em duas palavras
  void bidecle(const std::string& l) { fix(l, LO8); dw(0); fix(l, HI8); dw(0); }
  void text(const char* s) { while (*s) dw(static_cast<uint8_t>(*s++)); dw(0); }
  void fillTo(uint16_t a) { while (here() < a) dw(0); }

  // sem operando
  void hlt() { dw(0); }
  void sdbd() { dw(1); }
  void eis() { dw(2); }
  void dis() { dw(3); }
  void tci() { dw(5); }
  void clrc() { dw(6); }
  void setc() { dw(7); }
  // um registro
  void incr(int r) { dw(0x008 | r); }
  void decr(int r) { dw(0x010 | r); }
  void comr(int r) { dw(0x018 | r); }
  void negr(int r) { dw(0x020 | r); }
  void adcr(int r) { dw(0x028 | r); }
  void gswd(int r) { dw(0x030 | (r & 3)); }
  void nop() { dw(0x034); }
  void sin() { dw(0x036); }
  void rswd(int r) { dw(0x038 | r); }
  // deslocamentos (R0..R3, por 1 ou 2)
  void swap(int r, int n = 1) { sh(0, r, n); }
  void sll(int r, int n = 1) { sh(1, r, n); }
  void rlc(int r, int n = 1) { sh(2, r, n); }
  void sllc(int r, int n = 1) { sh(3, r, n); }
  void slr(int r, int n = 1) { sh(4, r, n); }
  void sar(int r, int n = 1) { sh(5, r, n); }
  void rrc(int r, int n = 1) { sh(6, r, n); }
  void sarc(int r, int n = 1) { sh(7, r, n); }
  // registro, registro (origem, destino)
  void movr(int s, int d) { dw(0x080 | (s << 3) | d); }
  void addr(int s, int d) { dw(0x0C0 | (s << 3) | d); }
  void subr(int s, int d) { dw(0x100 | (s << 3) | d); }
  void cmpr(int s, int d) { dw(0x140 | (s << 3) | d); }
  void andr(int s, int d) { dw(0x180 | (s << 3) | d); }
  void xorr(int s, int d) { dw(0x1C0 | (s << 3) | d); }
  void tstr(int r) { movr(r, r); }
  void clrr(int r) { xorr(r, r); }
  void jr(int r) { movr(r, 7); }
  // desvio relativo
  void br(int cond, const std::string& l) { dw(0x200 | cond); fix(l, BR); dw(0); }
  void b(const std::string& l) { br(B, l); }
  // salto absoluto: modo 0 = normal, 1 = liga interrupção (JE), 2 = desliga (JD)
  void j(const std::string& l, int mode = 0) { dw(4); fix(l, JMP); dw(static_cast<uint16_t>(0x300 | mode)); dw(0); }
  void jsr(int reg, const std::string& l, int mode = 0) {
    dw(4); fix(l, JMP); dw(static_cast<uint16_t>(((reg - 4) & 3) << 8 | mode)); dw(0);
  }
  void j(uint16_t a, int mode = 0) { dw(4); dw(static_cast<uint16_t>(0x300 | ((a >> 8) & 0xFC) | mode)); dw(a & 0x3FF); }
  void jsr(int reg, uint16_t a, int mode = 0) {
    dw(4); dw(static_cast<uint16_t>(((reg - 4) & 3) << 8 | ((a >> 8) & 0xFC) | mode)); dw(a & 0x3FF);
  }
  // memória: direto
  void mvo(int r, uint16_t a) { dw(0x240 | r); dw(a); }
  void mvi(uint16_t a, int r) { dw(0x280 | r); dw(a); }
  void add(uint16_t a, int r) { dw(0x2C0 | r); dw(a); }
  void sub(uint16_t a, int r) { dw(0x300 | r); dw(a); }
  void cmp(uint16_t a, int r) { dw(0x340 | r); dw(a); }
  void and_(uint16_t a, int r) { dw(0x380 | r); dw(a); }
  void xor_(uint16_t a, int r) { dw(0x3C0 | r); dw(a); }
  void mvo(int r, const std::string& l) { dw(0x240 | r); dwl(l); }
  void mvi(const std::string& l, int r) { dw(0x280 | r); dwl(l); }
  // memória: indireto pelo registro a (1..6)
  void mvoat(int r, int a) { dw(0x240 | (a << 3) | r); }
  void mviat(int a, int r) { dw(0x280 | (a << 3) | r); }
  void addat(int a, int r) { dw(0x2C0 | (a << 3) | r); }
  void subat(int a, int r) { dw(0x300 | (a << 3) | r); }
  void cmpat(int a, int r) { dw(0x340 | (a << 3) | r); }
  void andat(int a, int r) { dw(0x380 | (a << 3) | r); }
  void xorat(int a, int r) { dw(0x3C0 | (a << 3) | r); }
  void pshr(int r) { mvoat(r, 6); }
  void pulr(int r) { mviat(6, r); }
  // imediato (@R7)
  void mvii(uint16_t v, int r) { dw(0x2B8 | r); dw(v); }
  void addi(uint16_t v, int r) { dw(0x2F8 | r); dw(v); }
  void subi(uint16_t v, int r) { dw(0x338 | r); dw(v); }
  void cmpi(uint16_t v, int r) { dw(0x378 | r); dw(v); }
  void andi(uint16_t v, int r) { dw(0x3B8 | r); dw(v); }
  void xori(uint16_t v, int r) { dw(0x3F8 | r); dw(v); }
  void mvii(const std::string& l, int r) { dw(0x2B8 | r); dwl(l); }

  std::vector<uint16_t> finish() {
    for (const auto& f : fixes_) {
      auto it = labels_.find(f.label);
      if (it == labels_.end()) throw std::runtime_error("rótulo desconhecido: " + f.label);
      const uint16_t t = it->second;
      if (f.kind == ABS) {
        w_[f.pos] = t;
      } else if (f.kind == LO8) {
        w_[f.pos] = t & 0xFF;
      } else if (f.kind == HI8) {
        w_[f.pos] = t >> 8;
      } else if (f.kind == BR) {
        const uint16_t next = static_cast<uint16_t>(org_ + f.pos + 1);
        if (t >= next) w_[f.pos] = static_cast<uint16_t>(t - next);
        else { w_[f.pos - 1] |= 0x20; w_[f.pos] = static_cast<uint16_t>(next - t - 1); }
      } else {
        w_[f.pos] = static_cast<uint16_t>(w_[f.pos] | ((t >> 8) & 0xFC));
        w_[f.pos + 1] = t & 0x3FF;
      }
    }
    fixes_.clear();
    return w_;
  }
  uint16_t addressOf(const std::string& l) const {
    auto it = labels_.find(l);
    if (it == labels_.end()) throw std::runtime_error("rótulo desconhecido: " + l);
    return it->second;
  }

private:
  enum Kind { ABS, BR, JMP, LO8, HI8 };
  struct Fix { size_t pos; std::string label; Kind kind; };
  void fix(const std::string& l, Kind k) { fixes_.push_back({w_.size(), l, k}); }
  void sh(int op, int r, int n) { dw(static_cast<uint16_t>(0x040 | (op << 3) | (n == 2 ? 4 : 0) | (r & 3))); }

  uint16_t org_;
  std::vector<uint16_t> w_;
  std::map<std::string, uint16_t> labels_;
  std::vector<Fix> fixes_;
};

}  // namespace indigo::intv
