// Sanyo LC8951 (CDC: decodificador de CD-ROM com buffer), escrito pela folha de dados do chip:
//  - 16 registradores atrás de um ponteiro (AR, que avança sozinho a cada acesso, menos no registrador 0);
//    escrita: 1 IFCTRL, 2/3 DBC, 4/5 DAC, 6 DTRG, 7 DTACK, 8/9 WA, A CTRL0, B CTRL1, C/D PT, F RESET;
//    leitura: 1 IFSTAT, 2/3 DBC, 4-7 HEAD0-3, 8/9 PT, A/B WA, C-F STAT0-STAT3;
//  - IFSTAT tem os bits ativos em 0: DTEI (fim de transferência), DECI (setor decodificado), DTBSY, DTEN;
//  - a cada setor (com DECEN) o header chega em HEAD0-3, STAT3.VALST fica ativo e o DECI pulsa; com WRRQ o setor
//    vai para o buffer na posição WA/PT, que avançam 2352 bytes;
//  - na saída de dados o DAC aponta o buffer e o DBC conta os bytes (menos 1) até a transferência acabar.
// Este chip não sabe para onde vão os dados: quem faz o DMA e o registrador de dados do host é o gate array.
#include <cstring>

#include "segacd.h"

namespace indigo::segacd {

namespace {
// duração do pulso de DECI sem leitura do STAT3 (~5,4 ms, valor medido do comportamento de caixa-preta)
constexpr int64_t DECI_PULSE = 269000;
}  // namespace

void Lc8951::reset() {
  ar_ = 0;
  ifstat_ = 0xFF;
  ifctrl_ = ctrl0_ = ctrl1_ = 0;
  stat0_ = stat2_ = 0;
  stat3_ = 0x80;
  std::memset(header_, 0, sizeof header_);
  std::memset(subheader_, 0, sizeof subheader_);
  header_[3] = 0x01;
  deciUntil_ = 0;
}

void Lc8951::expireDeci(int64_t now) {
  if (!(ifstat_ & DECI) && now > deciUntil_) ifstat_ |= DECI;
}

bool Lc8951::interruptActive(int64_t now) {
  expireDeci(now);
  return (~ifstat_ & ifctrl_ & (DTEI | DECI)) != 0;
}

// STAT2: bit 3 = MODE (o pedido do CTRL1), bit 2 = FORM (com AUTORQ, o bit Form 2 do submodo do subheader)
uint8_t Lc8951::modeStatus() const {
  const uint8_t form = (ctrl0_ & AUTORQ) ? ((subheader_[2] & 0x20) ? 0x04 : 0x00) : (ctrl1_ & FORMRQ);
  return static_cast<uint8_t>((ctrl1_ & MODRQ) | form);
}

Lc8951::Effect Lc8951::writeRegister(uint8_t v, int64_t now) {
  Effect fx = Effect::None;
  const uint8_t r = ar_;
  auto lo = [](uint16_t& w, uint8_t b) { w = static_cast<uint16_t>((w & 0xFF00) | b); };
  auto hi = [](uint16_t& w, uint8_t b) { w = static_cast<uint16_t>((w & 0x00FF) | (b << 8)); };
  switch (r) {
    case 0x1:
      expireDeci(now);
      ifctrl_ = v;
      if (!(v & DOUTEN)) { ifstat_ |= DTBSY | DTEN; fx = Effect::StopTransfer; }
      break;
    case 0x2: lo(dbc_, v); break;
    case 0x3: hi(dbc_, v & 0x0F); break;
    case 0x4: lo(dac_, v); break;
    case 0x5: hi(dac_, v); break;
    case 0x6:
      if (ifctrl_ & DOUTEN) { ifstat_ &= static_cast<uint8_t>(~(DTBSY | DTEN)); fx = Effect::StartTransfer; }
      break;
    case 0x7: ifstat_ |= DTEI; break;
    case 0x8: lo(wa_, v); break;
    case 0x9: hi(wa_, v); break;
    case 0xA:
      ctrl0_ = v;
      stat0_ = v & DECEN;  // sem erros de CRC: CRCOK acompanha o decodificador ligado
      if (!(v & DECEN)) ifstat_ |= DECI;
      stat2_ = modeStatus();
      break;
    case 0xB: ctrl1_ = v; stat2_ = modeStatus(); break;
    case 0xC: lo(pt_, v); break;
    case 0xD: hi(pt_, v); break;
    case 0xF: reset(); fx = Effect::ChipReset; break;
    default: break;
  }
  if (r) ar_ = static_cast<uint8_t>((r + 1) & 0x0F);
  return fx;
}

uint8_t Lc8951::readRegister(int64_t now) {
  const uint8_t r = ar_;
  uint8_t v = 0xFF;
  switch (r) {
    case 0x1: expireDeci(now); v = ifstat_; break;
    case 0x2: v = static_cast<uint8_t>(dbc_); break;
    case 0x3: v = static_cast<uint8_t>(dbc_ >> 8); break;
    case 0x4: case 0x5: case 0x6: case 0x7: v = (ctrl1_ & SHDREN) ? subheader_[r - 4] : header_[r - 4]; break;
    case 0x8: v = static_cast<uint8_t>(pt_); break;
    case 0x9: v = static_cast<uint8_t>(pt_ >> 8); break;
    case 0xA: v = static_cast<uint8_t>(wa_); break;
    case 0xB: v = static_cast<uint8_t>(wa_ >> 8); break;
    case 0xC: v = stat0_; break;
    case 0xD: v = 0; break;
    case 0xE: v = stat2_; break;
    case 0xF: v = stat3_; stat3_ = 0x80; ifstat_ |= DECI; break;  // ler STAT3 encerra o DECI
    default: break;
  }
  if (r) ar_ = static_cast<uint8_t>((r + 1) & 0x0F);
  return v;
}

uint8_t Lc8951::outByte() {
  const uint8_t v = buf_[dac_ & (BUFFER - 1)];
  dac_++;
  dbc_--;
  return v;
}

uint16_t Lc8951::outWord() {
  const uint32_t a = dac_ & (BUFFER - 2);
  const uint16_t v = static_cast<uint16_t>((buf_[a] << 8) | buf_[a + 1]);
  dac_ = static_cast<uint16_t>(dac_ + 2);
  dbc_ = static_cast<uint16_t>(dbc_ - 2);
  return v;
}

void Lc8951::endTransfer(int64_t when) {
  expireDeci(when);
  dbc_ = 0xFFFF;
  ifstat_ = static_cast<uint8_t>((ifstat_ | DTBSY | DTEN) & ~DTEI);
}

void Lc8951::sectorArrived(const uint8_t header[4], const uint8_t* raw, int64_t now) {
  if (!(ctrl0_ & DECEN)) return;
  std::memcpy(header_, header, 4);
  stat3_ = 0;  // VALST ativo
  ifstat_ &= static_cast<uint8_t>(~DECI);
  deciUntil_ = now + DECI_PULSE;
  if (ctrl0_ & WRRQ) {
    pt_ = static_cast<uint16_t>(pt_ + 2352);
    wa_ = static_cast<uint16_t>(wa_ + 2352);
    uint32_t a = pt_;
    for (int i = 0; i < 4; i++) put(a++, header[i]);
    auto copy = [&](const uint8_t* src, int n) { for (int i = 0; i < n; i++) put(a++, src[i]); };
    if (header_[3] == 0x01) {
      if (raw) copy(raw + 16, 2048);  // Mode 1: os 2048 bytes de dados
    } else if (ctrl1_ & MODRQ) {
      // Mode 2 pedido: subheader (repetido) e os 2328 bytes seguintes
      if (raw) std::memcpy(subheader_, raw + 20, 4);
      copy(subheader_, 4);
      copy(subheader_, 4);
      if (raw) copy(raw + 24, 2328);
    } else {
      if (raw) { std::memcpy(subheader_, raw + 20, 4); copy(raw + 24, 2328); }
    }
    if (ctrl0_ & AUTORQ) stat2_ = modeStatus();
  }
}

void Lc8951::sync(StateIO& io) {
  io.bytes(buf_);
  io.sync(ar_); io.sync(ifstat_); io.sync(ifctrl_); io.sync(ctrl0_); io.sync(ctrl1_);
  io.raw(header_, 4); io.raw(subheader_, 4);
  io.sync(stat0_); io.sync(stat2_); io.sync(stat3_);
  io.sync(dbc_); io.sync(dac_); io.sync(pt_); io.sync(wa_); io.sync(deciUntil_);
  if (io.reading()) {
    ar_ &= 0x0F;
    if (buf_.size() != BUFFER) buf_.assign(BUFFER, 0);
  }
}

}  // namespace indigo::segacd
