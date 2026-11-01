// TIA do Atari 2600, ponto a ponto (228 ciclos de cor por linha, 68 de apagamento horizontal + 160 visíveis).
// Cada objeto tem o seu contador de posição de 160 estados, que só anda na parte visível da linha (e com os
// pulsos extras do HMOVE durante o apagamento); quando o contador passa pelos pontos de partida (decodificadores
// de NUSIZ), começa a desenhar uma cópia. Os atrasos de cada registrador (PF 2 ciclos de cor, GRP/ENA/REFP 1,
// HMxx 2, HMOVE 6...) seguem o chip; a régua é o Stella e as notas de hardware do TIA de Andrew Towers.
#include <algorithm>
#include <cstring>

#include "a2600.h"

namespace indigo::a2600 {

namespace {

// bits de colisão: um por par de objetos; o bit 15 indica "objeto aceso neste ponto"
enum : uint16_t {
  C_P0P1 = 1 << 0, C_M0M1 = 1 << 1, C_M0P1 = 1 << 2, C_M0P0 = 1 << 3, C_M1P0 = 1 << 4, C_M1P1 = 1 << 5,
  C_P0PF = 1 << 6, C_P0BL = 1 << 7, C_P1PF = 1 << 8, C_P1BL = 1 << 9, C_M0PF = 1 << 10, C_M0BL = 1 << 11,
  C_M1PF = 1 << 12, C_M1BL = 1 << 13, C_BLPF = 1 << 14, C_ON = 1 << 15,
};
constexpr uint16_t offMask(uint16_t self) { return static_cast<uint16_t>(~self & 0x7FFF); }
constexpr uint16_t OFF_P0 = offMask(C_P0P1 | C_M0P0 | C_M1P0 | C_P0PF | C_P0BL);
constexpr uint16_t OFF_P1 = offMask(C_P0P1 | C_M0P1 | C_M1P1 | C_P1PF | C_P1BL);
constexpr uint16_t OFF_M0 = offMask(C_M0M1 | C_M0P1 | C_M0P0 | C_M0PF | C_M0BL);
constexpr uint16_t OFF_M1 = offMask(C_M0M1 | C_M1P0 | C_M1P1 | C_M1PF | C_M1BL);
constexpr uint16_t OFF_BL = offMask(C_P0BL | C_P1BL | C_M0BL | C_M1BL | C_BLPF);
constexpr uint16_t OFF_PF = offMask(C_P0PF | C_P1PF | C_M0PF | C_M1PF | C_BLPF);

// registradores internos para as trocas atrasadas de GRP0/GRP1/ENABL (VDEL)
enum : uint8_t { SHUFFLE_P0 = 0x40, SHUFFLE_P1 = 0x41, SHUFFLE_BL = 0x42 };

// decodificadores de cópia (NUSIZ): em que valores do contador começa cada cópia, e qual cópia é (1, 2, 3)
struct Decodes {
  uint8_t t[6][160] = {};
  Decodes() {
    for (auto& d : t) d[156] = 1;
    t[1][12] = 2;                 // duas cópias próximas
    t[2][28] = 2;                 // duas cópias a média distância
    t[3][12] = 2; t[3][28] = 3;   // três cópias próximas
    t[4][60] = 2;                 // duas cópias longe
    t[5][28] = 2; t[5][60] = 3;   // três cópias a média distância
  }
};
const Decodes kDec;
// tabela usada por cada valor de NUSIZ (5 e 7 são uma cópia larga)
constexpr uint8_t kTable[8] = {0, 1, 2, 3, 4, 0, 5, 0};
inline uint8_t dec(uint8_t nusiz, unsigned counter) { return kDec.t[kTable[nusiz & 7]][counter % 160]; }

constexpr int8_t P_OFFSET = -5;  // atraso entre o começo da cópia e o primeiro ponto (jogadores)
constexpr int8_t M_OFFSET = -4;  // mísseis e bola

uint8_t reverse8(uint8_t v) {
  v = static_cast<uint8_t>(((v & 0xF0) >> 4) | ((v & 0x0F) << 4));
  v = static_cast<uint8_t>(((v & 0xCC) >> 2) | ((v & 0x33) << 2));
  return static_cast<uint8_t>(((v & 0xAA) >> 1) | ((v & 0x55) << 1));
}

}  // namespace

// ================================================================== jogador
void TIA::Player::reset(uint16_t mask) {
  *this = Player();
  maskOff = mask;
  collision = mask;
}

void TIA::Player::updatePattern() {
  pattern = delaying ? patOld : patNew;
  if (!reflected) pattern = reverse8(pattern);
  if (rendering && renderCounter >= tripPoint) collision = (pattern & (1u << sample)) ? 0xFFFF : maskOff;
}

void TIA::Player::tick() {
  latch();
  const uint8_t d = dec(decodesOffset, counter);
  if (d) {
    rendering = true;
    sample = 0;
    renderCounter = P_OFFSET;
    copy = d;
  } else if (rendering) {
    ++renderCounter;
    if (divider == 1) {
      if (renderCounter > 0) ++sample;
      if (renderCounter >= 0 && dividerChange >= 0 && dividerChange-- == 0) setDivider(dividerPending);
    } else {
      if (renderCounter > 1 && ((static_cast<unsigned>(renderCounter - 1) & (divider - 1u)) == 0)) ++sample;
      if (renderCounter > 0 && dividerChange >= 0 && dividerChange-- == 0) setDivider(dividerPending);
    }
    if (sample > 7) rendering = false;
  }
  if (++counter >= 160) counter = 0;
}

void TIA::Player::nusiz(uint8_t v, bool hblank) {
  const uint8_t oldTable = kTable[decodesOffset];
  decodesOffset = v & 7;
  dividerPending = decodesOffset == 5 ? 2 : decodesOffset == 7 ? 4 : 1;
  const unsigned prev = (counter + 159u) % 160u;
  if (!rendering && dec(decodesOffset, prev)) {
    rendering = true;
    sample = 0;
    renderCounter = P_OFFSET;
    copy = dec(decodesOffset, prev);
  }
  if (kTable[decodesOffset] != oldTable && rendering && (renderCounter - P_OFFSET) < 2 &&
      !dec(decodesOffset, static_cast<unsigned>(counter - renderCounter + P_OFFSET + 159 + 160) % 160u))
    rendering = false;
  if (dividerPending == divider) return;
  if (rendering) {
    const int delta = renderCounter - P_OFFSET;
    switch ((divider << 4) | dividerPending) {
      case 0x12:
      case 0x14:
        if (hblank) {
          if (delta < 4) setDivider(dividerPending);
          else dividerChange = delta < 5 ? 1 : 0;
        } else {
          if (delta < 3) setDivider(dividerPending);
          else dividerChange = 1;
        }
        break;
      case 0x21:
      case 0x41:
        if (delta < (hblank ? 4 : 3)) {
          setDivider(dividerPending);
        } else if (delta < (hblank ? 6 : 5)) {
          setDivider(dividerPending);
          --renderCounter;
        } else {
          dividerChange = hblank ? 0 : 1;
        }
        break;
      case 0x42:
      case 0x24:
        if (renderCounter < 1 || (hblank && (renderCounter % divider == 1))) setDivider(dividerPending);
        else dividerChange = static_cast<int8_t>(divider - (renderCounter - 1) % divider);
        break;
      default:
        setDivider(dividerPending);
        break;
    }
  } else {
    setDivider(dividerPending);
  }
}

void TIA::Player::resp(uint8_t c) {
  counter = c;
  if (rendering && (renderCounter - P_OFFSET) < 4) renderCounter = static_cast<int8_t>(P_OFFSET + (c - 157));
}

uint8_t TIA::Player::respClock() const {
  const int back = divider == 1 ? 5 : divider == 2 ? 8 : 12;
  return static_cast<uint8_t>((counter + 160 - back) % 160);
}

// ================================================================== míssil
void TIA::Missile::reset(uint16_t mask) {
  *this = Missile();
  maskOff = mask;
  collision = mask;
}

void TIA::Missile::tick(int hclock, bool regular) {
  visible = rendering && (renderCounter >= 0 ||
                          (moving && regular && renderCounter == -1 && width < 4 && ((hclock + 1) % 4 == 3)));
  latchOnly();
  if (dec(decodesOffset, counter) && !resmp) {
    rendering = true;
    renderCounter = M_OFFSET;
    copy = dec(decodesOffset, counter);
  } else if (rendering) {
    if (renderCounter == -1) {
      if (moving && regular) {
        switch ((hclock + 1) % 4) {
          case 3:
            effWidth = width == 1 ? 2 : width;
            if (width < 4) ++renderCounter;
            break;
          case 2:
            effWidth = 0;
            break;
          default:
            effWidth = width;
            break;
        }
      } else {
        effWidth = width;
      }
    }
    if (++renderCounter >= static_cast<int8_t>(moving ? effWidth : width)) rendering = false;
  }
  if (++counter >= 160) counter = 0;
}

void TIA::Missile::resm(uint8_t c, bool hblank) {
  counter = c;
  if (!rendering) return;
  if (renderCounter < 0) {
    renderCounter = static_cast<int8_t>(M_OFFSET + (c - 157));
  } else {
    switch (width) {
      case 8: renderCounter = static_cast<int8_t>((c - 157) + (renderCounter >= 4 ? 4 : 0)); break;
      case 4: renderCounter = static_cast<int8_t>(c - 157); break;
      case 2:
        if (hblank) rendering = renderCounter > 1;
        else if (renderCounter == 0) ++renderCounter;
        break;
      default:
        if (hblank) rendering = renderCounter > 0;
        break;
    }
  }
}

void TIA::Missile::nusiz(uint8_t v) {
  static constexpr uint8_t widths[4] = {1, 2, 4, 8};
  decodesOffset = v & 7;
  width = widths[(v >> 4) & 3];
  if (rendering && renderCounter >= static_cast<int8_t>(width)) rendering = false;
}

// ================================================================== bola
void TIA::Ball::reset(uint16_t mask) {
  *this = Ball();
  maskOff = mask;
  collision = mask;
}

void TIA::Ball::tick(bool regular) {
  signal = rendering && renderCounter >= 0;
  latchOnly();
  const bool starfield = moving && regular;
  if (counter == 156) {
    rendering = true;
    renderCounter = M_OFFSET;
    const unsigned delta = (counter + 160u - lastMoveTick) % 4u;
    if (starfield && delta == 3 && width < 4) ++renderCounter;
    switch (delta) {
      case 3: effWidth = width == 1 ? 2 : width; break;
      case 2: effWidth = 0; break;
      default: effWidth = width; break;
    }
  } else if (rendering && ++renderCounter >= static_cast<int8_t>(starfield ? effWidth : width)) {
    rendering = false;
  }
  if (++counter >= 160) counter = 0;
}

// ================================================================== TIA
TIA::TIA() { reset(); }

void TIA::setPal(bool pal) {
  frameLines = pal ? 312 : 262;
  yStart = pal ? 32 : 23;
  frameHeight = pal ? 274 : 228;
}

void TIA::reset() {
  p0_.reset(OFF_P0);
  p1_.reset(OFF_P1);
  m0_.reset(OFF_M0);
  m1_.reset(OFF_M1);
  bl_.reset(OFF_BL);
  pfPattern_ = 0;
  pfMaskOff_ = OFF_PF;
  pfCollision_ = OFF_PF;
  pf0_ = pf1_ = pf2_ = pfX_ = 0;
  pfReflected_ = pfRefp_ = pfScore_ = false;
  pfColor_ = bkColor_ = 0;
  priority_ = 0;
  col0_ = col1_ = 0;
  hctr_ = hctrDelta_ = 0;
  hblank_ = true;
  extendedHblank_ = movementInProgress_ = false;
  movementClock_ = 0;
  collisionMask_ = 0;
  lines_ = 0;
  queued_ = 0;
  latched_ = false;
  latchValue_[0] = latchValue_[1] = 0x80;
  dumped_ = false;
  vsync_ = vblank_ = false;
  fmState_ = 0;
  lineInState_ = vsyncLineCount_ = curFrameLines_ = y_ = lastY_ = 0;
  vsyncPending_ = rendering_ = false;
  vsyncPendingLines_ = 0;
  lastFrameLines = 0;
  totalFrames = 0;
  palVotes = ntscVotes = 0;
  vsyncCount = lastVsyncOffLine = 0;
  frameDone = false;
  wsyncRequested = false;
  for (auto& c : ch_) c = Channel();
  audioCounter_ = 0;
  sum0_ = sum1_ = sumCt_ = 0;
  audioOut.clear();
  std::memset(screen, 0, sizeof screen);
}

// ---------------------------------------------------------------- leitura e escrita
uint8_t TIA::read(uint8_t a, uint8_t bus) {
  uint8_t r = 0;
  const uint16_t c = collisionMask_;
  auto pair = [&](uint16_t b7, uint16_t b6) {
    return static_cast<uint8_t>(((c & b7) ? 0x80 : 0) | ((c & b6) ? 0x40 : 0));
  };
  switch (a & 0x0F) {
    case 0x0: r = pair(C_M0P1, C_M0P0); break;
    case 0x1: r = pair(C_M1P0, C_M1P1); break;
    case 0x2: r = pair(C_P0PF, C_P0BL); break;
    case 0x3: r = pair(C_P1PF, C_P1BL); break;
    case 0x4: r = pair(C_M0PF, C_M0BL); break;
    case 0x5: r = pair(C_M1PF, C_M1BL); break;
    case 0x6: r = (c & C_BLPF) ? 0x80 : 0; break;
    case 0x7: r = pair(C_P0P1, C_M0M1); break;
    case 0xC:
    case 0xD: {
      // botões: 0 = apertado; no modo de trava (VBLANK bit 6) fica em 0 até o modo ser desligado
      const int k = a & 1;
      uint8_t v = fire[k] ? 0 : 0x80;
      if (latched_) {
        latchValue_[k] &= v;
        v = latchValue_[k];
      }
      r = v;
      break;
    }
    default: r = 0; break;  // INPT0-3: sem paddles ligados (capacitor descarregado)
  }
  // os bits 5-0 não são ligados pelo TIA: ficam com o que estava no barramento
  return static_cast<uint8_t>(r | (bus & 0x3F));
}

void TIA::push(uint8_t reg, uint8_t v, int delay) {
  if (queued_ >= static_cast<int>(sizeof queue_ / sizeof queue_[0])) {
    // não acontece com a CPU de verdade (no máximo uns 10 pendentes); por segurança executa o mais antigo
    delayedWrite(queue_[0].reg, queue_[0].value);
    std::memmove(queue_, queue_ + 1, sizeof(Pending) * (queued_ - 1));
    queued_--;
  }
  // executa no começo do (delay+1)-ésimo ciclo de cor depois da escrita
  queue_[queued_++] = {reg, v, static_cast<uint8_t>(delay + 1)};
}

void TIA::write(uint8_t a, uint8_t v) {
  switch (a & 0x3F) {
    case 0x00: fmSetVsync(v & 0x02); break;                       // VSYNC
    case 0x01:                                                    // VBLANK
      if (v & 0x40) latched_ = true;
      else { latched_ = false; latchValue_[0] = latchValue_[1] = 0x80; }
      dumped_ = v & 0x80;
      push(0x01, v, 1);
      break;
    case 0x02: wsyncRequested = true; break;                      // WSYNC
    case 0x03: applyRsync(); break;                               // RSYNC
    case 0x04: m0_.nusiz(v); p0_.nusiz(v, hblank_); break;        // NUSIZ0
    case 0x05: m1_.nusiz(v); p1_.nusiz(v, hblank_); break;        // NUSIZ1
    case 0x06: col0_ = v >> 1; break;                             // COLUP0
    case 0x07: col1_ = v >> 1; break;                             // COLUP1
    case 0x08: pfColor_ = v >> 1; break;                          // COLUPF
    case 0x09: bkColor_ = v >> 1; break;                          // COLUBK
    case 0x0A: {                                                  // CTRLPF
      priority_ = (v & 0x04) ? 2 : (v & 0x02) ? 1 : 0;
      pfReflected_ = v & 0x01;
      pfScore_ = (v & 0x06) == 0x02;
      static constexpr uint8_t widths[4] = {1, 2, 4, 8};
      bl_.width = widths[(v >> 4) & 3];
      break;
    }
    case 0x0B: push(0x0B, v, 1); break;                           // REFP0
    case 0x0C: push(0x0C, v, 1); break;                           // REFP1
    case 0x0D: push(0x0D, v, 2); break;                           // PF0
    case 0x0E: push(0x0E, v, 2); break;                           // PF1
    case 0x0F: push(0x0F, v, 2); break;                           // PF2
    case 0x10: p0_.resp(resxCounter()); break;                    // RESP0
    case 0x11: p1_.resp(resxCounter()); break;                    // RESP1
    case 0x12: m0_.resm(resxCounter(), hblank_); break;           // RESM0
    case 0x13: m1_.resm(resxCounter(), hblank_); break;           // RESM1
    case 0x14:                                                    // RESBL
      bl_.counter = resxCounter();
      bl_.rendering = true;
      bl_.renderCounter = static_cast<int8_t>(M_OFFSET + (bl_.counter - 157));
      break;
    case 0x15: ch_[0].audc = v & 0x0F; break;
    case 0x16: ch_[1].audc = v & 0x0F; break;
    case 0x17: ch_[0].audf = v & 0x1F; break;
    case 0x18: ch_[1].audf = v & 0x1F; break;
    case 0x19: ch_[0].audv = v & 0x0F; break;
    case 0x1A: ch_[1].audv = v & 0x0F; break;
    case 0x1B: push(0x1B, v, 1); push(SHUFFLE_P1, 0, 1); break;   // GRP0 (e o GRP1 "velho" recebe o novo)
    case 0x1C: push(0x1C, v, 1); push(SHUFFLE_P0, 0, 1); push(SHUFFLE_BL, 0, 1); break;  // GRP1
    case 0x1D: push(0x1D, v, 1); break;                           // ENAM0
    case 0x1E: push(0x1E, v, 1); break;                           // ENAM1
    case 0x1F: push(0x1F, v, 1); break;                           // ENABL
    case 0x20: push(0x20, v, 2); break;                           // HMP0
    case 0x21: push(0x21, v, 2); break;                           // HMP1
    case 0x22: push(0x22, v, 2); break;                           // HMM0
    case 0x23: push(0x23, v, 2); break;                           // HMM1
    case 0x24: push(0x24, v, 2); break;                           // HMBL
    case 0x25: {                                                  // VDELP0
      const bool d = v & 1;
      if (d != p0_.delaying) { p0_.delaying = d; p0_.updatePattern(); }
      break;
    }
    case 0x26: {                                                  // VDELP1
      const bool d = v & 1;
      if (d != p1_.delaying) { p1_.delaying = d; p1_.updatePattern(); }
      break;
    }
    case 0x27: {                                                  // VDELBL
      const bool d = v & 1;
      if (d != bl_.delaying) { bl_.delaying = d; bl_.updateEnabled(); }
      break;
    }
    case 0x28: {                                                  // RESMP0
      const bool r = v & 2;
      if (r != m0_.resmp) { m0_.resmp = r; m0_.updateEnabled(); }
      break;
    }
    case 0x29: {                                                  // RESMP1
      const bool r = v & 2;
      if (r != m1_.resmp) { m1_.resmp = r; m1_.updateEnabled(); }
      break;
    }
    case 0x2A: push(0x2A, v, 6); break;                           // HMOVE
    case 0x2B: push(0x2B, v, 2); break;                           // HMCLR
    case 0x2C: collisionMask_ = 0; break;                         // CXCLR
    default: break;
  }
}

void TIA::delayedWrite(uint8_t reg, uint8_t v) {
  switch (reg) {
    case 0x01: vblank_ = v & 0x02; break;
    case 0x0B: case 0x0C: {
      Player& p = reg == 0x0B ? p0_ : p1_;
      const bool r = v & 0x08;
      if (r != p.reflected) { p.reflected = r; p.updatePattern(); }
      break;
    }
    case 0x0D: pf0_ = v >> 4; pfPattern_ = (pfPattern_ & 0xFFFF0u) | pf0_; break;
    case 0x0E: pf1_ = v; pfPattern_ = (pfPattern_ & 0xFF00Fu) | (static_cast<uint32_t>(reverse8(v)) << 4); break;
    case 0x0F: pf2_ = v; pfPattern_ = (pfPattern_ & 0x00FFFu) | (static_cast<uint32_t>(v) << 12); break;
    case 0x1B: case 0x1C: {
      Player& p = reg == 0x1B ? p0_ : p1_;
      const uint8_t old = p.patNew;
      p.patNew = v;
      if (!p.delaying && v != old) p.updatePattern();
      break;
    }
    case SHUFFLE_P0: case SHUFFLE_P1: {
      Player& p = reg == SHUFFLE_P0 ? p0_ : p1_;
      const uint8_t old = p.patOld;
      p.patOld = p.patNew;
      if (p.delaying && p.patOld != old) p.updatePattern();
      break;
    }
    case SHUFFLE_BL: {
      const bool old = bl_.enOld;
      bl_.enOld = bl_.enNew;
      if (bl_.enOld != old && bl_.delaying) bl_.updateEnabled();
      break;
    }
    case 0x1D: case 0x1E: {
      Missile& m = reg == 0x1D ? m0_ : m1_;
      const bool e = v & 0x02;
      if (e != m.enam) { m.enam = e; m.updateEnabled(); }
      break;
    }
    case 0x1F: {
      const bool e = v & 0x02;
      if (e != bl_.enNew) { bl_.enNew = e; if (!bl_.delaying) bl_.updateEnabled(); }
      break;
    }
    case 0x20: p0_.hmm = (v >> 4) ^ 8; break;
    case 0x21: p1_.hmm = (v >> 4) ^ 8; break;
    case 0x22: m0_.hmm = (v >> 4) ^ 8; break;
    case 0x23: m1_.hmm = (v >> 4) ^ 8; break;
    case 0x24: bl_.hmm = (v >> 4) ^ 8; break;
    case 0x2B: p0_.hmm = p1_.hmm = m0_.hmm = m1_.hmm = bl_.hmm = 8; break;
    case 0x2A:  // HMOVE: pulsos extras de movimento e, se cair no apagamento, a barra preta de 8 pontos
      movementClock_ = 0;
      movementInProgress_ = true;
      if (!extendedHblank_) {
        if (rendering_ && hblank_ && y_ < static_cast<uint32_t>(frameHeight))
          std::memset(screen + y_ * 160, 0, 8);
        extendedHblank_ = true;
      }
      p0_.moving = p1_.moving = m0_.moving = m1_.moving = bl_.moving = true;
      break;
    default: break;
  }
}

uint8_t TIA::resxCounter() const {
  // valor do contador de posição logo depois do RESxx (o objeto aparece ~4-5 pontos depois)
  return hblank_ ? (hctr_ >= 73 ? 158 : 159) : 157;
}

// ---------------------------------------------------------------- relógio
void TIA::run(int n) {
  audioRun(n);
  for (int i = 0; i < n; i++) clock();
}

void TIA::clock() {
  if (queued_) {
    int k = 0;
    for (int i = 0; i < queued_; i++) {
      if (--queue_[i].left == 0) delayedWrite(queue_[i].reg, queue_[i].value);
      else queue_[k++] = queue_[i];
    }
    queued_ = k;
  }
  if (movementInProgress_) tickMovement();
  if (hblank_) tickHblank();
  else tickHframe();
  // o VBLANK desliga as colisões
  if (!vblank_)
    collisionMask_ |= static_cast<uint16_t>(p0_.collision & p1_.collision & m0_.collision & m1_.collision &
                                            bl_.collision & pfCollision_);
  if (++hctr_ >= 228) nextLine();
}

void TIA::tickMovement() {
  if (hctr_ & 3) return;
  const bool hb = hblank_;
  const uint8_t mc = movementClock_ > 15 ? 0 : movementClock_;
  auto mv = [&](auto& o, auto&& extra) {
    if (!o.moving) return;
    if (mc == o.hmm) o.moving = false;
    else if (hb) extra();
  };
  mv(m0_, [&] { m0_.tick(hctr_, false); });
  mv(m1_, [&] { m1_.tick(hctr_, false); });
  mv(p0_, [&] { p0_.tick(); });
  mv(p1_, [&] { p1_.tick(); });
  bl_.lastMoveTick = bl_.counter;
  mv(bl_, [&] { bl_.tick(false); });
  movementInProgress_ = m0_.moving || m1_.moving || p0_.moving || p1_.moving || bl_.moving;
  if (movementClock_ < 255) ++movementClock_;  // depois de 15 fica "0" para sempre (Cosmic Ark)
}

void TIA::tickHblank() {
  // o latch de saída de cada objeto continua recebendo o relógio no apagamento
  p0_.latch();
  p1_.latch();
  m0_.visible = m0_.rendering && m0_.renderCounter >= 0; m0_.latchOnly();
  m1_.visible = m1_.rendering && m1_.renderCounter >= 0; m1_.latchOnly();
  bl_.signal = bl_.rendering && bl_.renderCounter >= 0; bl_.latchOnly();
  switch (hctr_) {
    case 0: extendedHblank_ = false; break;
    case 67: if (!extendedHblank_) hblank_ = false; break;
    case 75: if (extendedHblank_) hblank_ = false; break;
    default: break;
  }
  if (extendedHblank_ && hctr_ - hctrDelta_ > 67) pfTick(static_cast<uint32_t>(hctr_ - 68 - hctrDelta_));
}

void TIA::tickHframe() {
  const uint32_t x = static_cast<uint32_t>(hctr_ - 68 - hctrDelta_);
  pfTick(x);
  p0_.tick();
  p1_.tick();
  m0_.tick(hctr_, true);
  if (m0_.resmp && p0_.mainCopyAt4()) m0_.counter = p0_.respClock();
  m1_.tick(hctr_, true);
  if (m1_.resmp && p1_.mainCopyAt4()) m1_.counter = p1_.respClock();
  bl_.tick(true);
  if (rendering_) renderPixel(x);
}

void TIA::pfTick(uint32_t x) {
  pfX_ = static_cast<uint8_t>(x);
  if (x == 79 || x == 0) pfRefp_ = pfReflected_;
  if (x & 3) return;
  bool on;
  if (pfPattern_ == 0 || x >= 160) on = false;
  else if (x < 79) on = pfPattern_ & (1u << (x >> 2));
  else if (pfRefp_) on = pfPattern_ & (1u << (39 - (x >> 2)));
  else on = pfPattern_ & (1u << ((x >> 2) - 20));
  pfCollision_ = on ? 0xFFFF : pfMaskOff_;
}

void TIA::renderPixel(uint32_t x) {
  if (x >= 160 || y_ >= static_cast<uint32_t>(frameHeight)) return;
  uint8_t c = 0;
  if (!vblank_) {
    const bool p0 = p0_.collision & C_ON, m0 = m0_.collision & C_ON, p1 = p1_.collision & C_ON,
               m1 = m1_.collision & C_ON, pf = pfCollision_ & C_ON, bl = bl_.collision & C_ON;
    const uint8_t pfc = pfScore_ ? (pfX_ < 80 ? col0_ : col1_) : pfColor_;
    switch (priority_) {
      case 2:  // playfield e bola na frente
        c = pf ? pfc : bl ? pfColor_ : p0 ? col0_ : m0 ? col0_ : p1 ? col1_ : m1 ? col1_ : bkColor_;
        break;
      case 1:  // modo placar
        c = p0 ? col0_ : m0 ? col0_ : pf ? pfc : p1 ? col1_ : m1 ? col1_ : bl ? pfColor_ : bkColor_;
        break;
      default:
        c = p0 ? col0_ : m0 ? col0_ : p1 ? col1_ : m1 ? col1_ : pf ? pfc : bl ? pfColor_ : bkColor_;
        break;
    }
  }
  screen[y_ * 160 + x] = c;
}

void TIA::applyRsync() {
  const int x = hctr_ > 68 ? hctr_ - 68 : 0;
  hctrDelta_ = 225 - hctr_;
  if (rendering_ && y_ < static_cast<uint32_t>(frameHeight) && x < 160)
    std::memset(screen + y_ * 160 + x, 0, 160 - x);
  hctr_ = 225;
}

void TIA::nextLine() {
  hctr_ = 0;
  hblank_ = true;
  hctrDelta_ = 0;
  lines_++;
  fmNextLine();
  m0_.visible = m0_.rendering && m0_.renderCounter >= 0; m0_.latchOnly();
  m1_.visible = m1_.rendering && m1_.renderCounter >= 0; m1_.latchOnly();
  p0_.latch();
  p1_.latch();
  bl_.signal = bl_.rendering && bl_.renderCounter >= 0; bl_.latchOnly();
  pfCollision_ = pfMaskOff_;
}

// ---------------------------------------------------------------- quadro (como o FrameManager do Stella)
enum { WAIT_VSYNC_START = 0, WAIT_VSYNC_END = 1, WAIT_FRAME_START = 2, FRAME = 3 };

void TIA::fmSetVsync(bool on) {
  if (on == vsync_) return;
  vsync_ = on;
  if (on) {
    if (fmState_ == WAIT_VSYNC_END || vsyncPending_) return;
    vsyncPending_ = true;
    vsyncPendingLines_ = 0;
  } else {
    // votos para a detecção NTSC/PAL: de 0 (262 linhas) a 1 (312 linhas); quadros ímpares valem metade
    const uint32_t n = lines_ - lastVsyncOffLine;
    lastVsyncOffLine = lines_;
    if (++vsyncCount > 10 && n <= 312 + 100) {
      double p = (static_cast<double>(n) - 262.0) / 50.0;
      if (n % 2) p *= 0.5;
      p = std::clamp(p, 0.0, 1.0);
      palVotes += p;
      ntscVotes += 1.0 - p;
    }
    vsyncPending_ = false;
    if (fmState_ != WAIT_VSYNC_END) return;  // pulso curto demais (menos de 2 linhas)
    fmSetState(WAIT_FRAME_START);
  }
}

void TIA::fmSetState(int s) {
  if (fmState_ == s) return;
  fmState_ = s;
  lineInState_ = 0;
  if (s == WAIT_FRAME_START) {
    frameComplete();
    vsyncLineCount_ = 0;
  } else if (s == FRAME) {
    vsyncLineCount_ = 0;
    y_ = 0;
  }
  rendering_ = fmState_ == FRAME;
}

void TIA::fmNextLine() {
  curFrameLines_++;
  const int prev = fmState_;
  ++lineInState_;
  if (vsyncPending_ && ++vsyncPendingLines_ >= 2) {
    vsyncPending_ = false;
    fmSetState(WAIT_VSYNC_END);
  }
  switch (fmState_) {
    case WAIT_VSYNC_START:
      if ((curFrameLines_ > static_cast<uint32_t>(frameLines - 3) || totalFrames == 0) &&
          (vblank_ || curFrameLines_ > 624))
        ++vsyncLineCount_;
      if (vsyncLineCount_ > 50) fmSetState(WAIT_FRAME_START);
      break;
    case WAIT_VSYNC_END:
      if (vblank_ || curFrameLines_ > 624) ++vsyncLineCount_;
      if (vsyncLineCount_ > 50) fmSetState(WAIT_FRAME_START);
      break;
    case WAIT_FRAME_START:
      if (lineInState_ >= static_cast<uint32_t>(yStart)) fmSetState(FRAME);
      break;
    default:  // FRAME
      if (lineInState_ >= static_cast<uint32_t>(frameHeight)) {
        lastY_ = yStart + y_;
        fmSetState(WAIT_VSYNC_START);
      }
      break;
  }
  if (fmState_ == FRAME && prev == FRAME) ++y_;
}

void TIA::frameComplete() {
  lastFrameLines = curFrameLines_;
  curFrameLines_ = 0;
  ++totalFrames;
  // linhas que este quadro não desenhou ficam pretas
  if (lastY_ != yStart + y_ && y_ < static_cast<uint32_t>(frameHeight))
    std::memset(screen + y_ * 160, 0, (frameHeight - y_) * 160);
  frameDone = true;
}

// ---------------------------------------------------------------- som (2 canais com divisor, pulso e ruído)
void TIA::audioPhase0(int n) {
  Channel& c = ch_[n];
  if (c.clockEnable) {
    c.noiseBit4 = c.noise & 0x01;
    switch (c.audc & 0x03) {
      case 0: case 1: c.pulseHold = false; break;
      case 2: c.pulseHold = (c.noise & 0x1E) != 0x02; break;
      default: c.pulseHold = !c.noiseBit4; break;
    }
    if ((c.audc & 0x03) == 0)
      c.noiseFeedback = ((c.pulse ^ c.noise) & 0x01) || !(c.noise || (c.pulse != 0x0A)) || !(c.audc & 0x0C);
    else
      c.noiseFeedback = (((c.noise & 0x04) ? 1 : 0) ^ (c.noise & 0x01)) || c.noise == 0;
  }
  c.clockEnable = c.div == c.audf;
  if (c.div == c.audf || c.div == 0x1F) c.div = 0;
  else ++c.div;
}

void TIA::audioPhase1(int n) {
  Channel& c = ch_[n];
  if (!c.clockEnable) return;
  bool pulseFeedback = false;
  switch (c.audc >> 2) {
    case 0:
      pulseFeedback = ((((c.pulse & 0x02) ? 1 : 0) ^ (c.pulse & 0x01)) != 0) && (c.pulse != 0x0A) && (c.audc & 0x03);
      break;
    case 1: pulseFeedback = !(c.pulse & 0x08); break;
    case 2: pulseFeedback = !c.noiseBit4; break;
    default: pulseFeedback = !((c.pulse & 0x02) || !(c.pulse & 0x0E)); break;
  }
  c.noise >>= 1;
  if (c.noiseFeedback) c.noise |= 0x10;
  if (!c.pulseHold) {
    c.pulse = static_cast<uint8_t>(~(c.pulse >> 1) & 0x07);
    if (pulseFeedback) c.pulse |= 0x08;
  }
}

void TIA::audioRun(int n) {
  // fase 0 nos ciclos de cor 9 e 81 da contagem do som, fase 1 (e uma amostra) em 37 e 149
  while (n > 0) {
    int toEvent, ev;
    if (audioCounter_ < 10) { toEvent = 10 - audioCounter_; ev = 0; }
    else if (audioCounter_ < 38) { toEvent = 38 - audioCounter_; ev = 1; }
    else if (audioCounter_ < 82) { toEvent = 82 - audioCounter_; ev = 0; }
    else if (audioCounter_ < 150) { toEvent = 150 - audioCounter_; ev = 1; }
    else { toEvent = 228 - audioCounter_; ev = 2; }
    const int chunk = std::min(n, toEvent);
    sum0_ += static_cast<uint32_t>((ch_[0].pulse & 1) * ch_[0].audv) * chunk;
    sum1_ += static_cast<uint32_t>((ch_[1].pulse & 1) * ch_[1].audv) * chunk;
    sumCt_ += chunk;
    audioCounter_ += chunk;
    n -= chunk;
    if (chunk != toEvent) break;
    if (ev == 0) {
      audioPhase0(0);
      audioPhase0(1);
    } else if (ev == 1) {
      audioPhase1(0);
      audioPhase1(1);
      if (sumCt_ && audioOut.size() < 4096)
        audioOut.push_back(static_cast<uint8_t>(sum0_ / sumCt_ + sum1_ / sumCt_));
      sum0_ = sum1_ = sumCt_ = 0;
    } else {
      audioCounter_ = 0;
    }
  }
}

// ---------------------------------------------------------------- estado
void TIA::sync(StateIO& io) {
  auto player = [&](Player& p) {
    io.sync(p.collision); io.sync(p.hmm); io.sync(p.counter); io.sync(p.copy); io.sync(p.divider);
    io.sync(p.dividerPending); io.sync(p.sample); io.sync(p.decodesOffset); io.sync(p.patOld); io.sync(p.patNew);
    io.sync(p.pattern); io.sync(p.renderCounter); io.sync(p.tripPoint); io.sync(p.dividerChange);
    io.sync(p.moving); io.sync(p.rendering); io.sync(p.reflected); io.sync(p.delaying);
    if (io.reading()) {
      p.counter %= 160; p.hmm &= 15; if (p.sample > 8) p.sample = 8; p.decodesOffset &= 7; p.copy &= 3;
      if (p.divider != 1 && p.divider != 2 && p.divider != 4) p.divider = 1;
      if (p.dividerPending != 1 && p.dividerPending != 2 && p.dividerPending != 4) p.dividerPending = 1;
    }
  };
  auto missile = [&](Missile& m) {
    io.sync(m.collision); io.sync(m.hmm); io.sync(m.counter); io.sync(m.copy); io.sync(m.width);
    io.sync(m.effWidth); io.sync(m.decodesOffset); io.sync(m.renderCounter); io.sync(m.moving);
    io.sync(m.rendering); io.sync(m.visible); io.sync(m.enabled); io.sync(m.enam); io.sync(m.resmp);
    if (io.reading()) { m.counter %= 160; m.hmm &= 15; m.decodesOffset &= 7; m.width &= 15; m.effWidth &= 15; }
  };
  player(p0_);
  player(p1_);
  missile(m0_);
  missile(m1_);
  io.sync(bl_.collision); io.sync(bl_.hmm); io.sync(bl_.counter); io.sync(bl_.width); io.sync(bl_.effWidth);
  io.sync(bl_.lastMoveTick); io.sync(bl_.renderCounter); io.sync(bl_.moving); io.sync(bl_.rendering);
  io.sync(bl_.signal); io.sync(bl_.enabled); io.sync(bl_.enOld); io.sync(bl_.enNew); io.sync(bl_.delaying);
  io.sync(pfPattern_); io.sync(pfCollision_); io.sync(pf0_); io.sync(pf1_); io.sync(pf2_); io.sync(pfColor_);
  io.sync(pfX_); io.sync(pfReflected_); io.sync(pfRefp_); io.sync(pfScore_); io.sync(bkColor_); io.sync(priority_);
  io.sync(col0_); io.sync(col1_);
  io.sync(hctr_); io.sync(hctrDelta_); io.sync(hblank_); io.sync(extendedHblank_); io.sync(movementInProgress_);
  io.sync(movementClock_); io.sync(collisionMask_); io.sync(lines_);
  io.sync(queued_);
  if (io.reading()) clampTo(queued_, 0, static_cast<int>(sizeof queue_ / sizeof queue_[0]));
  for (int i = 0; i < queued_; i++) { io.sync(queue_[i].reg); io.sync(queue_[i].value); io.sync(queue_[i].left); }
  io.sync(latched_); io.raw(latchValue_, sizeof latchValue_); io.sync(dumped_);
  io.sync(vsync_); io.sync(vblank_);
  io.sync(fmState_); io.sync(lineInState_); io.sync(vsyncLineCount_); io.sync(curFrameLines_); io.sync(y_);
  io.sync(lastY_); io.sync(vsyncPending_); io.sync(rendering_); io.sync(vsyncPendingLines_);
  io.sync(lastFrameLines); io.sync(totalFrames); io.sync(wsyncRequested);
  for (auto& c : ch_) {
    io.sync(c.audc); io.sync(c.audf); io.sync(c.audv); io.sync(c.div); io.sync(c.pulse); io.sync(c.noise);
    io.sync(c.clockEnable); io.sync(c.noiseFeedback); io.sync(c.noiseBit4); io.sync(c.pulseHold);
  }
  io.sync(audioCounter_); io.sync(sum0_); io.sync(sum1_); io.sync(sumCt_);
  io.raw(screen, sizeof screen);
  if (io.reading()) {
    for (int i = 0; i < queued_; i++) {
      if (queue_[i].left == 0 || queue_[i].left > 8) queue_[i].left = 1;
    }
    clampTo(hctr_, 0, 227);
    clampTo(hctrDelta_, -228, 228);
    clampTo(fmState_, 0, 3);
    rendering_ = fmState_ == FRAME;
    if (y_ >= static_cast<uint32_t>(frameHeight)) y_ = frameHeight - 1;
    clampTo(audioCounter_, 0, 227);
    priority_ = priority_ > 2 ? 0 : priority_;
    pfColor_ &= 0x7F; bkColor_ &= 0x7F; col0_ &= 0x7F; col1_ &= 0x7F;
    for (auto& c : ch_) { c.audc &= 15; c.audf &= 31; c.audv &= 15; c.div &= 31; c.pulse &= 15; c.noise &= 31; }
    if (sumCt_ > 228) sum0_ = sum1_ = sumCt_ = 0;
    for (auto& v : screen) v &= 0x7F;
  }
}

}  // namespace indigo::a2600
