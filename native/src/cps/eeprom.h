// EEPROM serial 93C46 em modo de 16 bits (64 palavras), o "save" do CPS2: guarda a configuração do jogo e os
// recordes. Protocolo Microwire: CS alto, bit de início 1, código de 2 bits, endereço de 6 bits; leitura com um
// bit 0 de enchimento e depois 16 bits (do mais alto para o mais baixo); gravação só depois de EWEN.
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::cps {

class Eeprom93C46 {
public:
  uint16_t mem[64];
  bool dirty = false;

  Eeprom93C46() { for (auto& w : mem) w = 0xFFFF; }

  void resetLines() { cs_ = clk_ = false; reset(); do_ = true; }

  bool dataOut() const { return do_; }

  void write(bool cs, bool clk, bool di) {
    if (!cs) {
      if (cs_) reset();
      cs_ = false;
      clk_ = clk;
      do_ = true;  // pronto
      return;
    }
    cs_ = true;
    const bool rise = clk && !clk_;
    clk_ = clk;
    if (!rise) return;
    switch (phase_) {
      case 0:  // espera o bit de início
        if (di) { phase_ = 1; bits_ = 0; cmd_ = 0; }
        break;
      case 1:  // código + endereço (8 bits)
        cmd_ = (cmd_ << 1) | (di ? 1 : 0);
        if (++bits_ == 8) command();
        break;
      case 2:  // lendo: um bit por pulso
        do_ = (shift_ & 0x8000) != 0;
        shift_ = static_cast<uint16_t>(shift_ << 1);
        if (++bits_ == 16) { addr_ = (addr_ + 1) & 63; shift_ = mem[addr_]; bits_ = 0; }  // leitura em sequência
        break;
      case 3:  // dado da gravação (16 bits)
        data_ = static_cast<uint16_t>((data_ << 1) | (di ? 1 : 0));
        if (++bits_ == 16) {
          if (writeEnable_) {
            if (op_ == 1) mem[addr_] = data_;
            else for (auto& w : mem) w = data_;  // WRAL
            dirty = true;
          }
          phase_ = 4;
        }
        break;
      default: break;
    }
  }

  void sync(StateIO& io) {
    io.raw(mem, sizeof mem);
    io.sync(cs_); io.sync(clk_); io.sync(do_); io.sync(writeEnable_); io.sync(phase_); io.sync(bits_); io.sync(cmd_);
    io.sync(addr_); io.sync(shift_); io.sync(data_); io.sync(op_);
    if (io.reading()) { clampTo(phase_, 0, 4); clampTo(bits_, 0, 16); addr_ &= 63; cmd_ &= 0xFF; }
  }

private:
  void reset() { phase_ = 0; bits_ = 0; cmd_ = 0; }

  void command() {
    const int op = (cmd_ >> 6) & 3;
    addr_ = cmd_ & 63;
    bits_ = 0;
    switch (op) {
      case 2:  // READ: bit de enchimento 0, depois os dados
        shift_ = mem[addr_];
        do_ = false;
        phase_ = 2;
        break;
      case 1:  // WRITE
        op_ = 1; data_ = 0; phase_ = 3;
        break;
      case 3:  // ERASE
        if (writeEnable_) { mem[addr_] = 0xFFFF; dirty = true; }
        phase_ = 4;
        break;
      default:
        switch ((cmd_ >> 4) & 3) {
          case 0: writeEnable_ = false; phase_ = 4; break;                      // EWDS
          case 1: op_ = 2; data_ = 0; phase_ = 3; break;                        // WRAL
          case 2: if (writeEnable_) { for (auto& w : mem) w = 0xFFFF; dirty = true; } phase_ = 4; break;  // ERAL
          default: writeEnable_ = true; phase_ = 4; break;                      // EWEN
        }
        break;
    }
  }

  bool cs_ = false, clk_ = false, do_ = true, writeEnable_ = false;
  int phase_ = 0, bits_ = 0, cmd_ = 0, addr_ = 0, op_ = 0;
  uint16_t shift_ = 0, data_ = 0;
};

}  // namespace indigo::cps
