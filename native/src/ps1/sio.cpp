#include "sio.h"

#include <algorithm>
#include <cstring>

#include "system.h"

namespace indigo::ps1 {

namespace {
constexpr uint32_t CARD_SIZE = 128 * 1024;
constexpr uint64_t PAD_ACK_DELAY = 340, CARD_ACK_DELAY = 170;
// o /ACK do aparelho é um pulso curto (poucos microssegundos), não um nível que fica baixo até o próximo byte:
// o remendo que a biblioteca do cartão de memória dos jogos instala no tratador de interrupção do BIOS espera o
// JOY_STAT.7 voltar a 0 antes de sair (com o nível preso, NASCAR 2001 parava no logo do PlayStation)
constexpr uint64_t ACK_LOW = 100;
}  // namespace

Sio0::Sio0(System& s) : card(CARD_SIZE, 0), sys_(s) {}

void Sio0::reset() {
  ctrl_ = 0; mode_ = 0; baud_ = 0x88;
  rx_ = 0xFF; rxFull_ = false; busy_ = false; ackLevel_ = false; irqFlag_ = false;
  doneAt_ = ackAt_ = 0;
  txByte_ = 0; pendingAck_ = false;
  device_ = 0; step_ = 0;
  padCmd_ = 0; configMode_ = false;
  std::memset(rumbleMap_, 0xFF, sizeof rumbleMap_);
  std::memset(padBuf_, 0, sizeof padBuf_);
  padParam_ = 0;
  cardCmd_ = 0; cardFlag_ = 0x08; cardSector_ = 0; cardChk_ = 0; cardPrev_ = 0;
  motorSmall = motorLarge = 0;
  analogLocked = false;
}

uint32_t Sio0::read(uint32_t off, int size) {
  sync();
  switch (off) {
    case 0x0: {
      const uint8_t v = rxFull_ ? rx_ : 0xFF;
      rxFull_ = false;
      return v;
    }
    case 0x4: case 0x5: case 0x6: case 0x7: {
      const uint32_t v = (busy_ ? 0u : 1u) | (rxFull_ ? 2u : 0u) | (busy_ ? 0u : 4u) | (ackLevel_ ? 0x80u : 0u) |
                         (irqFlag_ ? 0x200u : 0u);
      return v >> ((off & 3) * 8);
    }
    case 0x8: return mode_ | (size == 4 ? uint32_t(ctrl_) << 16 : 0);
    case 0xA: return ctrl_;
    case 0xE: return baud_;
    default: return 0;
  }
}

void Sio0::write(uint32_t off, uint32_t v, int size) {
  sync();
  switch (off) {
    case 0x0: {
      txByte_ = uint8_t(v);
      ackLevel_ = false;
      busy_ = true;
      const uint32_t factor = (mode_ & 3) == 2 ? 16 : (mode_ & 3) == 3 ? 64 : 1;
      doneAt_ = sys_.now + std::max<uint64_t>(uint64_t(baud_) * factor * 8, 64);
      ackAt_ = 0;
      break;
    }
    case 0x8:
      mode_ = uint16_t(v & 0x13F);
      if (size == 4) write(0xA, v >> 16, 2);
      break;
    case 0xA:
      if (v & 0x40) {  // reset da porta
        ctrl_ = 0; mode_ = 0; rxFull_ = false; busy_ = false; irqFlag_ = false; ackLevel_ = false; ackAt_ = 0;
        deselect();
        break;
      }
      if (v & 0x10) irqFlag_ = false;  // reconhece
      ctrl_ = uint16_t(v & ~0x50u);
      if (!(ctrl_ & 2)) { deselect(); ackAt_ = 0; ackLevel_ = false; }  // sem /JOY ninguém segura o /ACK
      break;
    case 0xE: baud_ = uint16_t(v); break;
    default: break;
  }
}

void Sio0::sync() {
  const uint64_t now = sys_.now;
  if (busy_ && now >= doneAt_) {
    busy_ = false;
    bool ack = false;
    rx_ = transfer(txByte_, ack);
    rxFull_ = true;
    if (ack) ackAt_ = doneAt_ + (device_ == 2 ? CARD_ACK_DELAY : PAD_ACK_DELAY);
  }
  if (ackAt_ && now >= ackAt_) {
    ackLevel_ = true;
    doneAt_ = ackAt_ + ACK_LOW;
    ackAt_ = 0;
    if ((ctrl_ & 0x1000) && !irqFlag_) {
      irqFlag_ = true;
      sys_.raiseIrq(IRQ_SIO0);
    }
  }
  // fim do pulso do /ACK (sem byte em curso, doneAt_ guarda quando o pulso termina)
  if (ackLevel_ && !busy_ && now >= doneAt_) ackLevel_ = false;
}

uint8_t Sio0::transfer(uint8_t b, bool& ack) {
  ack = false;
  if (!(ctrl_ & 2) || (ctrl_ & 0x2000)) return 0xFF;  // sem seleção, ou porta 2 (vazia)
  if (device_ == 0) {
    if (b == 0x01) { device_ = 1; step_ = 0; }
    else if (b == 0x81 && cardPresent) { device_ = 2; step_ = 0; }
    else return 0xFF;
  }
  return device_ == 1 ? padByte(b, ack) : cardByte(b, ack);
}

uint8_t Sio0::padByte(uint8_t b, bool& ack) {
  const PadState& p = pad[0];
  const int s = step_++;
  const uint8_t id = configMode_ ? 0xF3 : analogMode ? 0x73 : 0x41;
  const int len = (id & 0x0F) * 2;  // bytes de dados depois do 5Ah
  if (s == 0) { ack = true; return 0xFF; }
  if (s == 1) {
    padCmd_ = b;
    std::memset(padBuf_, 0, sizeof padBuf_);
    const bool known = b == 0x42 || b == 0x43 || (configMode_ && b >= 0x40 && b <= 0x4F);
    if (!known) { device_ = 0; return 0xFF; }
    if (b == 0x42 || (b == 0x43 && !configMode_)) {
      padBuf_[0] = uint8_t(p.buttons); padBuf_[1] = uint8_t(p.buttons >> 8);
      padBuf_[2] = p.rx; padBuf_[3] = p.ry; padBuf_[4] = p.lx; padBuf_[5] = p.ly;
      if (b == 0x42 && configMode_) { padBuf_[2] = padBuf_[3] = padBuf_[4] = padBuf_[5] = 0x80; }
    } else if (b == 0x45) {
      const uint8_t st[6] = {0x01, 0x02, uint8_t(analogMode ? 1 : 0), 0x02, 0x01, 0x00};
      std::memcpy(padBuf_, st, 6);
    } else if (b == 0x47) {
      const uint8_t st[6] = {0x00, 0x00, 0x02, 0x00, 0x01, 0x00};
      std::memcpy(padBuf_, st, 6);
    } else if (b == 0x4D) {
      std::memcpy(padBuf_, rumbleMap_, 6);
    }
    ack = true;
    return id;
  }
  if (s == 2) { ack = true; return 0x5A; }
  const int i = s - 3;  // índice dos dados
  if (i >= len) { device_ = 0; return 0xFF; }
  uint8_t out = padBuf_[i];
  switch (padCmd_) {
    case 0x42:
      if (i < 6) {
        if (rumbleMap_[i] == 0x00) motorSmall = (b & 1) ? 0xFF : 0;
        else if (rumbleMap_[i] == 0x01) motorLarge = b;
      }
      break;
    case 0x43:
      if (i == 0) padParam_ = b;
      if (i == len - 1) configMode_ = padParam_ == 1;
      break;
    case 0x44:
      if (i == 0) { analogMode = b == 1; motorSmall = motorLarge = 0; }
      if (i == 1) analogLocked = b == 3;
      break;
    case 0x46:
      if (i == 0) {
        static const uint8_t a[6] = {0x00, 0x00, 0x01, 0x02, 0x00, 0x0A}, c[6] = {0x00, 0x00, 0x01, 0x01, 0x01, 0x14};
        std::memcpy(padBuf_, b == 1 ? c : a, 6);
        out = padBuf_[0];
      }
      break;
    case 0x4C:
      if (i == 0) {
        std::memset(padBuf_, 0, 6);
        padBuf_[3] = b == 1 ? 0x07 : 0x04;
      }
      break;
    case 0x4D:
      rumbleMap_[i] = b;
      if (i == len - 1) { motorSmall = motorLarge = 0; }
      break;
    default: break;
  }
  if (i < len - 1) ack = true; else device_ = 0;
  return out;
}

uint8_t Sio0::cardByte(uint8_t b, bool& ack) {
  const int s = step_++;
  ack = true;
  if (s == 0) return 0xFF;
  if (s == 1) {
    cardCmd_ = b;
    if (b != 'R' && b != 'W' && b != 'S') { device_ = 0; ack = false; return cardFlag_; }
    return cardFlag_;
  }
  if (s == 2) return 0x5A;
  if (s == 3) return 0x5D;
  if (cardCmd_ == 'S') {
    static const uint8_t id[6] = {0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80};
    const int i = s - 4;
    if (i >= 5) { ack = false; device_ = 0; }
    return i < 6 ? id[i] : 0xFF;
  }
  if (s == 4) { cardSector_ = uint16_t(b << 8); cardPrev_ = b; return 0x00; }
  if (s == 5) {
    cardSector_ |= b;
    const uint8_t r = cardPrev_;
    cardPrev_ = b;
    cardChk_ = uint8_t((cardSector_ >> 8) ^ (cardSector_ & 0xFF));
    return r;
  }
  const bool bad = cardSector_ > 0x3FF;
  if (cardCmd_ == 'R') {
    switch (s) {
      case 6: return 0x5C;
      case 7: return 0x5D;
      case 8: if (bad) { ack = false; device_ = 0; return 0xFF; } return uint8_t(cardSector_ >> 8);
      case 9: return uint8_t(cardSector_);
      default: break;
    }
    if (s < 138) {
      const uint8_t d = card[size_t(cardSector_) * 128 + size_t(s - 10)];
      cardChk_ ^= d;
      return d;
    }
    if (s == 138) return cardChk_;
    ack = false; device_ = 0;
    return 0x47;
  }
  // 'W'
  if (s < 134) {
    const uint8_t r = cardPrev_;
    cardPrev_ = b;
    if (!bad) card[size_t(cardSector_) * 128 + size_t(s - 6)] = b;
    cardChk_ ^= b;
    return r;
  }
  if (s == 134) { const uint8_t r = cardPrev_; cardPrev_ = b; cardChk_ ^= b; return r; }  // b = soma; cardChk_ fica 0 se certa
  if (s == 135) return 0x5C;
  if (s == 136) return 0x5D;
  ack = false; device_ = 0;
  if (bad) return 0xFF;
  cardFlag_ &= ~0x08u;
  cardDirty = true;
  return cardChk_ == 0 ? 0x47 : 0x4E;
}

void Sio0::sync(StateIO& io) {
  io.sync(ctrl_); io.sync(mode_); io.sync(baud_);
  io.sync(rx_); io.sync(rxFull_); io.sync(busy_); io.sync(ackLevel_); io.sync(irqFlag_);
  io.sync(doneAt_); io.sync(ackAt_); io.sync(txByte_); io.sync(pendingAck_);
  io.sync(device_); io.sync(step_);
  io.sync(padCmd_); io.sync(configMode_); io.raw(rumbleMap_, sizeof rumbleMap_); io.raw(padBuf_, sizeof padBuf_);
  io.sync(padParam_);
  io.sync(cardCmd_); io.sync(cardFlag_); io.sync(cardSector_); io.sync(cardChk_); io.sync(cardPrev_);
  io.sync(analogMode); io.sync(analogLocked); io.sync(motorSmall); io.sync(motorLarge);
  io.bytes(card);
  if (io.reading()) {
    if (device_ > 2) device_ = 0;
    step_ = std::clamp(step_, 0, 200);
    const uint64_t lim = sys_.now + 0x1000000;
    if (doneAt_ > lim) doneAt_ = lim;
    if (ackAt_ > lim) ackAt_ = lim;
  }
}

}  // namespace indigo::ps1
