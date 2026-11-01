// SIO0 (1F801040..1F80104F): porta dos controles e dos cartões de memória.
//  - controle: DualShock (SCPH-1200), que liga como digital (ID 41h) e vira analógico (73h) pelo botão ANALOG
//    ou pelo próprio jogo (modo de configuração 43h/44h/45h/46h/47h/4Ch/4Dh); vibração pelos motores pequeno e
//    grande, exposta para o app;
//  - cartão de memória de 128 KB (1024 setores de 128 bytes): comandos de leitura (52h), escrita (57h) e
//    identificação (53h). O cartão da porta 1 é o save do jogo.
// Cada byte leva o tempo da taxa de transmissão (JOY_BAUD * 8 ciclos) e o /ACK do aparelho chega pouco depois,
// pedindo a interrupção 7 se o JOY_CTRL deixar; o /ACK é um pulso curto (JOY_STAT.7 volta a 0 sozinho).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::ps1 {

class System;

struct PadState {
  uint16_t buttons = 0xFFFF;     // ativo em 0, na ordem do PS1 (Select, L3, R3, Start, Cima, Direita, Baixo, Esquerda,
                                 // L2, R2, L1, R1, Triângulo, Círculo, Xis, Quadrado)
  uint8_t rx = 0x80, ry = 0x80, lx = 0x80, ly = 0x80;
};

class Sio0 {
public:
  explicit Sio0(System& s);
  void reset();
  uint32_t read(uint32_t off, int size);
  void write(uint32_t off, uint32_t v, int size);
  void sync();
  uint64_t nextEvent() const { return busy_ ? doneAt_ : ackAt_ ? ackAt_ : ~0ull; }

  PadState pad[2];
  bool analogMode = false;       // DualShock no modo analógico (luz vermelha)
  bool analogLocked = false;
  uint8_t motorSmall = 0, motorLarge = 0;  // vibração atual
  void toggleAnalog() { if (!analogLocked) analogMode = !analogMode; }

  std::vector<uint8_t> card;     // 128 KB do cartão da porta 1
  bool cardDirty = false;
  bool cardPresent = true;

  void sync(StateIO& io);

private:
  System& sys_;
  uint16_t ctrl_ = 0, mode_ = 0, baud_ = 0x88;
  uint8_t rx_ = 0xFF;
  bool rxFull_ = false, busy_ = false, ackLevel_ = false, irqFlag_ = false;
  uint64_t doneAt_ = 0, ackAt_ = 0;
  uint8_t txByte_ = 0;
  bool pendingAck_ = false;

  // aparelho selecionado: 0 nenhum, 1 controle, 2 cartão
  uint8_t device_ = 0;
  int step_ = 0;
  // controle
  uint8_t padCmd_ = 0;
  bool configMode_ = false;
  uint8_t rumbleMap_[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  uint8_t padBuf_[8] = {};
  uint8_t padParam_ = 0;
  // cartão
  uint8_t cardCmd_ = 0, cardFlag_ = 0x08;
  uint16_t cardSector_ = 0;
  uint8_t cardChk_ = 0, cardPrev_ = 0;

  uint8_t transfer(uint8_t b, bool& ack);
  uint8_t padByte(uint8_t b, bool& ack);
  uint8_t cardByte(uint8_t b, bool& ack);
  void deselect() { device_ = 0; step_ = 0; }
};

}  // namespace indigo::ps1
