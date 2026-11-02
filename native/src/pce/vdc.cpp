#include "vdc.h"

#include <algorithm>
#include <cstring>

#include "pce.h"

namespace indigo::pce {

namespace {
constexpr uint16_t NEVER = 0xFFFF;
}

void Vdc::power() {
  std::memset(vram, 0, sizeof vram);
  std::memset(sat, 0, sizeof sat);
  std::memset(row, 0, sizeof row);
  std::memset(&st_, 0, sizeof st_);
  std::memset(spr_, 0, sizeof spr_);
  std::memset(drawSpr_, 0, sizeof drawSpr_);
  std::memset(tiles_, 0, sizeof tiles_);
  std::memset(xHasSpr_, 0, sizeof xHasSpr_);
  st_.reg_.cols = st_.lat.cols = 32;
  st_.reg_.rows = st_.lat.rows = 32;
  st_.reg_.hdw = st_.lat.hdw = 0x1F;
  st_.reg_.vdw = st_.lat.vdw = 239;
  st_.incr = 1;
  st_.hMode = H_HDS;
  st_.hModeCounter = 1 << 30;  // a primeira linha não tem fases até o fim dela
  st_.evCounter = -1;
  st_.vMode = V_VDS;
  st_.latchClkX = st_.latchClkY = NEVER;
  st_.bgStart = st_.evalStart = NEVER;
  st_.allowDma = 1;
}

// ------------------------------------------------------------ tempo
bool Vdc::needsStepping() const {
  if ((st_.satbRunning || st_.vdmaRunning) && (st_.allowDma || st_.burst)) return true;
  if (st_.pendRead || st_.pendWrite) return true;
  return st_.rowHasSpr0 && st_.collisionLine && st_.ieColl;
}

int64_t Vdc::nextTime() const {
  if (needsStepping()) return st_.now + 3 - st_.now % 3;
  int k = LINE - st_.hclock;
  if (st_.hModeCounter > 0 && st_.hModeCounter < k) k = st_.hModeCounter;
  if (st_.evCounter > 0 && st_.evCounter < k) k = st_.evCounter;
  return st_.now + k;
}

void Vdc::runTo(int64_t t) {
  while (st_.now < t) {
    if (needsStepping()) {
      const int mis = static_cast<int>(st_.now % 3);
      if (mis) { advance(3 - mis); continue; }
      chunk();
    } else {
      advance(static_cast<int>(std::min<int64_t>(t - st_.now, 1 << 20)), true);
    }
  }
}

// um passo de 3 ciclos mestres com DMA e acesso à VRAM, como o chip faz a cada ciclo da CPU
void Vdc::chunk() {
  if (st_.satbRunning) satbStep();
  else if (st_.vdmaRunning) vdmaStep();
  if (st_.pendRead || st_.pendWrite || (st_.rowHasSpr0 && st_.collisionLine)) {
    draw();
    if (st_.pendRead || st_.pendWrite) vramAccess();
  }
  advance(3);
}

void Vdc::advance(int n, bool jump) {
  while (n > 0) {
    int k = n;
    if (st_.hModeCounter > 0 && st_.hModeCounter < k) k = st_.hModeCounter;
    if (st_.evCounter > 0 && st_.evCounter < k) k = st_.evCounter;
    if (LINE - st_.hclock < k) k = LINE - st_.hclock;
    if (k <= 0) k = 1;
    st_.hclock = static_cast<uint16_t>(st_.hclock + k);
    st_.now += k;
    n -= k;
    st_.hModeCounter -= k;
    if (st_.evCounter > 0) st_.evCounter -= k;
    if (st_.hModeCounter == 0) {
      draw();
      setH(static_cast<uint8_t>((st_.hMode + 1) & 3));
    }
    if (st_.evCounter == 0) { st_.evCounter = -1; event(); }
    if (st_.hclock >= LINE) endOfLine();
    if (jump && needsStepping()) return;
  }
}

void Vdc::event() {
  draw();
  switch (st_.ev) {
    case E_LATCH_Y:
      st_.needVClock = 1;
      st_.latchClkY = st_.hclock;
      incScrollY();
      if (!st_.burst) { st_.bgOn = st_.nextBg; st_.sprOn = st_.nextSpr; }
      st_.ev = E_LATCH_X;
      st_.evCounter = dots(2);
      break;
    case E_LATCH_X:
      st_.lat.bxr = st_.reg_.bxr;
      st_.latchClkX = st_.hclock;
      st_.ev = E_HDS_IRQ;
      st_.evCounter = dots(6);
      break;
    case E_HDS_IRQ:
      if (st_.evalStart >= LINE) st_.sprCount = 0;  // a avaliação da próxima linha não teve tempo de começar
      hdsIrqs();
      st_.ev = E_NONE;
      break;
    case E_RCR:
      incRcr();
      st_.ev = E_NONE;
      break;
    default: break;
  }
}

void Vdc::setH(uint8_t m) {
  st_.hMode = m;
  switch (m) {
    case H_HDS: st_.hModeCounter = dots((st_.lat.hds + 1) * 8); break;
    case H_HDW: {
      st_.needVClock = 1;
      st_.needRcr = 1;
      st_.ev = E_RCR;
      const int c = dots((st_.lat.hdw - 1) * 8 + 2);
      st_.evCounter = c > 0 ? c : -1;
      st_.hModeCounter = dots((st_.lat.hdw + 1) * 8);
      break;
    }
    case H_HDE:
      st_.bgStart = NEVER;
      st_.evalStart = NEVER;
      st_.loadSprStart = st_.hclock;
      st_.hModeCounter = dots((st_.lat.hde + 1) * 8);
      break;
    default:
      st_.bgStart = NEVER;
      st_.evalStart = NEVER;
      st_.hModeCounter = dots((st_.lat.hsw + 1) * 8);
      st_.hsyncStart = (st_.now + 2) / 3 * 3;  // o relógio que o chip vê é o do fim do ciclo de 3
      hsyncStartLatch();
      break;
  }
}

void Vdc::hsyncStartLatch() {
  st_.lat.hsw = st_.reg_.hsw; st_.lat.hds = st_.reg_.hds; st_.lat.hdw = st_.reg_.hdw; st_.lat.hde = st_.reg_.hde;
  st_.ev = E_NONE;
  st_.evCounter = -1;
  st_.tileCount = 0;
  st_.bgCycle = 0;
  st_.screenX = 0;

  const int displayStart = st_.hclock + st_.hModeCounter + dots((st_.lat.hds + 1) * 8);
  if (displayStart - dots(24) >= LINE) return;

  if (st_.vMode == V_VDW || st_.rcrCounter == vce->lines - 1) {
    const int width = dots((st_.lat.hdw + 1) * 8);
    const int evalStart = displayStart - dots(16);
    st_.evalStart = static_cast<uint16_t>(std::max(0, evalStart));
    st_.evalDone = 0;
    st_.evalEnd = static_cast<uint16_t>(std::min(LINE, evalStart + width + dots(8)));
    if (st_.vMode == V_VDW) {
      st_.bgStart = static_cast<uint16_t>(std::max(0, evalStart));
      st_.bgEnd = static_cast<uint16_t>(std::min(LINE, evalStart + width + dots(16)));
    }
  }

  int evClocks;
  if (st_.vMode == V_VDW) {
    st_.ev = E_LATCH_Y;
    evClocks = dots(34);
  } else {
    st_.ev = E_HDS_IRQ;
    evClocks = dots(25);
  }
  if (displayStart - evClocks <= st_.hclock) event();
  else st_.evCounter = displayStart - evClocks - st_.hclock;
}

void Vdc::setV(uint8_t m) {
  st_.vMode = m;
  switch (m) {
    case V_VDS: st_.vModeCounter = st_.lat.vds + 2; break;
    case V_VDW: st_.allowDma = 0; st_.vModeCounter = st_.lat.vdw + 1; st_.rcrCounter = 0; break;
    case V_VDE: st_.vModeCounter = st_.lat.vcr; break;
    default: {
      // o começo do vsync trava os registradores verticais e o MWR
      const uint8_t hsw = st_.lat.hsw, hds = st_.lat.hds, hdw = st_.lat.hdw, hde = st_.lat.hde;
      const uint16_t bxr = st_.lat.bxr, byr = st_.lat.byr;
      st_.lat = st_.reg_;
      st_.lat.hsw = hsw; st_.lat.hds = hds; st_.lat.hdw = hdw; st_.lat.hde = hde;
      st_.lat.bxr = bxr; st_.lat.byr = byr;
      st_.vModeCounter = st_.lat.vsw + 1;
      break;
    }
  }
}

void Vdc::clockV() {
  st_.vModeCounter--;
  if (st_.vModeCounter == 0) setV(static_cast<uint8_t>((st_.vMode + 1) & 3));
  st_.needVClock = 0;
}

void Vdc::incRcr() {
  st_.rcrCounter++;
  st_.needRcr = 0;
  clockV();
  if (st_.vMode == V_VDE && st_.rcrCounter == st_.lat.vdw + 1) { st_.needVblIrq = 1; st_.vblDone = 1; }
  if (st_.ieRcr && st_.rcrCounter == static_cast<int>(st_.rcr) - 0x40) { st_.stRcr = 1; setIrq(); }
}

void Vdc::incScrollY() {
  if (st_.rcrCounter == 0) {
    st_.lat.byr = st_.reg_.byr;
  } else {
    if (st_.byrPending) { st_.lat.byr = st_.reg_.byr; st_.byrPending = 0; }
    st_.lat.byr++;
  }
}

void Vdc::hdsIrqs() {
  if (st_.needVblIrq) vblank();
  if (st_.hasOverflow && st_.ieOver) { st_.stOver = 1; setIrq(); }
  st_.hasOverflow = 0;
}

void Vdc::vblank() {
  if (st_.ieVbl) { st_.stVbl = 1; setIrq(); }
  st_.needVblIrq = 0;
  // o DMA pendente da tabela de sprites (ou o repetido) começa junto com a interrupção de vblank
  st_.allowDma = 1;
  if (st_.satbPending || st_.dmaRepeat) {
    st_.satbPending = 0;
    st_.satbRunning = 1;
    st_.satbCounter = 0;
    st_.satbOffset = 0;
  }
}

void Vdc::endOfLine() {
  draw();
  if (st_.scanline >= drawFirst && st_.scanline < drawEnd) {
    sys->lineDone(index, st_.scanline);
    std::memset(row, 0, sizeof row);
  }
  st_.hclock = 0;
  st_.lineStart = st_.now;
  st_.scanline++;
  st_.latchClkX = st_.latchClkY = NEVER;
  if (st_.scanline == drawEnd) {
    st_.frameCount++;
    sys->frameDone(index);
  } else if (st_.scanline >= vce->lines) {
    st_.scanline = 0;
    st_.vblDone = 0;
    st_.burst = !st_.nextBg && !st_.nextSpr;
    st_.bgOn = st_.nextBg;
    st_.sprOn = st_.nextSpr;
  }
  sys->lineBegin(index, st_.scanline);

  if (st_.needRcr) incRcr();
  else if (st_.needVClock) clockV();

  if (st_.hMode == H_HDW) st_.loadSprStart = static_cast<uint16_t>(dots(36));
  if (st_.hMode != H_HSW) { st_.hMode = H_HSW; st_.hsyncStart = st_.now; }
  st_.bgStart = st_.evalStart = NEVER;
  st_.hModeCounter = dots(div() == 3 ? 32 : 24);
  st_.xStart = 0;
  st_.lastDraw = 0;
  hsyncStartLatch();

  if (st_.scanline == vce->lines - 3) setV(V_VSW);
  else if (st_.scanline == vce->lines - 2 && !st_.vblDone) st_.needVblIrq = 1;
}

// ------------------------------------------------------------ sprites e fundo
void Vdc::evalSprites() {
  if (st_.hclock < st_.evalStart || st_.evalDone || st_.burst) return;
  if ((std::min(st_.evalEnd, st_.hclock) - st_.evalStart) / div() / 4 < 1) return;
  st_.evalDone = 1;
  loadSprites();
  st_.sprCount = 0;
  st_.sprRow = static_cast<uint16_t>((st_.rcrCounter + 1) % vce->lines);
  const int n = std::min(64, (st_.evalEnd - st_.evalStart) / div() / 4);
  for (int i = 0; i < n; i++) {
    const int y = static_cast<int>(sat[i * 4] & 0x3FF) - 64;
    if (st_.sprRow < y) continue;
    const uint16_t flags = sat[i * 4 + 3];
    const int h = ((flags >> 12) & 3) == 0 ? 16 : ((flags >> 12) & 3) == 1 ? 32 : 64;
    if (st_.sprRow >= y + h) continue;
    const int srow = st_.sprRow - y;
    const bool vflip = (flags & 0x8000) != 0, hflip = (flags & 0x800) != 0;
    const int yo = vflip ? ((h - srow - 1) & 15) : (srow & 15);
    const int ro = vflip ? ((h - srow - 1) >> 4) : (srow >> 4);
    uint16_t tile = static_cast<uint16_t>((sat[i * 4 + 2] & 0x7FF) >> 1);
    const bool sp23 = (sat[i * 4 + 2] & 1) != 0;
    const int w = (flags & 0x100) ? 32 : 16;
    if (w == 32) tile &= static_cast<uint16_t>(~1);
    if (h == 32) tile &= static_cast<uint16_t>(~2);
    else if (h == 64) tile &= static_cast<uint16_t>(~6);
    const uint16_t tileY = static_cast<uint16_t>(tile | (ro << 1));
    for (int x = 0; x < w; x += 16) {
      if (st_.sprCount >= 16) { st_.hasOverflow = 1; break; }
      const int col = hflip ? ((w - x - 1) >> 4) : (x >> 4);
      Spr& s = spr_[st_.sprCount++];
      s.idx = static_cast<uint8_t>(i);
      s.x = static_cast<int16_t>(static_cast<int>(sat[i * 4 + 1] & 0x3FF) - 32 + x);
      s.addr = static_cast<uint16_t>((tileY | col) * 64 + yo);
      s.hflip = hflip;
      s.fg = (flags & 0x80) != 0;
      s.pal = flags & 0x0F;
      s.sp23 = sp23;
    }
  }
}

void Vdc::loadSprites() {
  st_.drawCount = 0;
  st_.totalCount = 0;
  st_.rowHasSpr0 = 0;
  st_.collisionLine = 0;
  if (st_.burst || (st_.loadSprStart >= st_.bgStart && st_.loadSprStart < st_.bgEnd)) return;
  const uint16_t clocks = static_cast<uint16_t>(st_.loadSprStart > st_.bgStart ? (LINE - st_.loadSprStart) + st_.bgStart
                                                                               : (st_.bgStart - st_.loadSprStart));
  const int per = st_.lat.sprMode == 1 ? 4 : st_.lat.sprMode == 2 ? 8 : st_.lat.sprMode == 3 ? 16 : 4;
  st_.drawCount = static_cast<uint8_t>(std::min<int>(st_.sprCount, clocks / div() / per));
  st_.totalCount = st_.drawCount;
  std::memset(xHasSpr_, 0, sizeof xHasSpr_);
  bool has0 = false;
  for (int i = 0; i < st_.totalCount; i++) {
    Spr& s = drawSpr_[i];
    s = spr_[i];
    std::memset(xHasSpr_ + s.x + 32, 1, 16);
    if (st_.lat.sprMode != 1) {
      s.d[0] = vramRead(s.addr);
      s.d[1] = vramRead(static_cast<uint16_t>(s.addr + 16));
      s.d[2] = vramRead(static_cast<uint16_t>(s.addr + 32));
      s.d[3] = vramRead(static_cast<uint16_t>(s.addr + 48));
    } else {
      // modo 1: sprites de 2 bits (SP0/SP1 ou SP2/SP3)
      const uint16_t a = static_cast<uint16_t>(s.addr + (s.sp23 ? 32 : 0));
      s.d[0] = vramRead(a);
      s.d[1] = vramRead(static_cast<uint16_t>(a + 16));
      s.d[2] = s.d[3] = 0;
    }
    has0 |= s.idx == 0;
  }
  if (has0 && st_.drawCount > 1) {
    st_.rowHasSpr0 = 1;
    // a colisão só pode acontecer se um pixel do sprite 0 cobrir o de outro sprite
    const Spr& z = drawSpr_[0];
    if (z.idx == 0) {
      auto opaque = [](const Spr& s, int px) {
        int o = px - s.x;
        if (o < 0 || o >= 16) return false;
        if (!s.hflip) o = 15 - o;
        return (((s.d[0] >> o) | (s.d[1] >> o) | (s.d[2] >> o) | (s.d[3] >> o)) & 1) != 0;
      };
      for (int i = 1; i < st_.drawCount && !st_.collisionLine; i++)
        for (int px = z.x; px < z.x + 16; px++)
          if (opaque(z, px) && opaque(drawSpr_[i], px)) { st_.collisionLine = 1; break; }
    }
  }
}

// o fundo é lido aos poucos (um bloco a cada 8 pontos), então escritas na VRAM durante a linha aparecem
void Vdc::loadTiles() {
  if (st_.hclock < st_.bgStart || st_.burst) return;
  const int end = (std::min(st_.bgEnd, st_.hclock) - st_.bgStart) / div();
  if (end <= st_.bgCycle) return;
  const uint16_t mask = static_cast<uint16_t>(st_.lat.cols - 1);
  const uint16_t scroll = st_.lat.bxr >> 3;
  const uint16_t r = st_.lat.byr & ((st_.lat.rows * 8) - 1);
  const bool m3 = st_.lat.vramMode == 3;
  for (int i = st_.bgCycle; i < end; i++) {
    const int ph = i & 7;
    Tile& t = tiles_[st_.tileCount < 99 ? st_.tileCount : 99];
    if (ph == (m3 ? 3 : 1)) {
      const uint16_t col = (scroll + st_.tileCount) & mask;
      const uint16_t bat = vramRead(static_cast<uint16_t>((r >> 3) * st_.lat.cols + col));
      t.pal = static_cast<uint8_t>(bat >> 12);
      t.addr = static_cast<uint16_t>((bat & 0xFFF) * 16);
    } else if (ph == 5 && !m3) {
      t.d0 = vramRead(static_cast<uint16_t>(t.addr + (r & 7)));
    } else if (ph == 7) {
      if (m3) {
        t.d0 = vramRead(static_cast<uint16_t>(t.addr + (r & 7) + (st_.lat.cg ? 8 : 0)));
        t.d1 = 0;
      } else {
        t.d1 = vramRead(static_cast<uint16_t>(t.addr + (r & 7) + 8));
      }
      if (st_.tileCount < 99) st_.tileCount++;
    }
  }
  st_.bgCycle = static_cast<uint16_t>(end);
}

void Vdc::draw() {
  if (st_.scanline < drawFirst || st_.scanline >= drawEnd) return;
  evalSprites();
  loadTiles();
  drawPixels();
}

void Vdc::drawPixels() {
  const int d = div();
  int xs = st_.xStart;
  int xMax = xs + (st_.hclock - st_.lastDraw) / d;
  if (xMax > ROW) xMax = ROW;
  const bool inPicture = st_.hMode == H_HDW && st_.tileCount > 0;
  const uint16_t gray = vce->gray ? 0x200 : 0;
  if (inPicture && (st_.bgOn || st_.sprOn)) {
    const bool hasSpr = st_.totalCount > 0;
    for (; xs < xMax; xs++) {
      uint16_t out = TRANSP_FLAG | vce->pal[0];
      uint8_t bgc = 0;
      const unsigned sx = st_.screenX;
      if (st_.bgOn) {
        const unsigned px = (st_.lat.bxr & 7) + sx;
        const unsigned c = px >> 3;
        if (c < 100) {
          const Tile& t = tiles_[c];
          const int sh = 7 - (px & 7);
          const unsigned v = ((t.d0 >> sh) & 0x101) | (((t.d1 >> sh) & 0x101) << 2);
          bgc = static_cast<uint8_t>(v | ((v & 0x500) >> 7));
          if (bgc) out = vce->pal[t.pal * 16 + bgc];
        }
      }
      if (hasSpr && sx + 32 < sizeof xHasSpr_ && xHasSpr_[sx + 32]) {
        bool check0 = false;
        for (int i = 0; i < st_.totalCount; i++) {
          const Spr& s = drawSpr_[i];
          int o = static_cast<int>(sx) - s.x;
          if (o < 0 || o >= 16) continue;
          if (!s.hflip) o = 15 - o;
          const uint8_t c = static_cast<uint8_t>(((s.d[0] >> o) & 1) | (((s.d[1] >> o) & 1) << 1) |
                                                 (((s.d[2] >> o) & 1) << 2) | (((s.d[3] >> o) & 1) << 3));
          if (!c) continue;
          if (check0) {
            if (st_.ieColl) { st_.stColl = 1; setIrq(); }
            break;
          }
          if (st_.sprOn && (bgc == 0 || s.fg)) out = SPR_FLAG | vce->pal[256 + s.pal * 16 + c];
          if (s.idx == 0 && st_.rowHasSpr0) check0 = true;
          else break;
        }
      }
      row[xs] = out | gray;
      st_.screenX++;
    }
  } else if (inPicture) {
    const uint16_t c = TRANSP_FLAG | vce->pal[0] | gray;
    for (; xs < xMax; xs++) row[xs] = c;
  } else {
    const uint16_t c = TRANSP_FLAG | vce->pal[256] | gray;
    for (; xs < xMax; xs++) row[xs] = c;
  }
  st_.xStart = static_cast<uint16_t>(xs);
  st_.lastDraw = static_cast<uint16_t>(st_.hclock / d * d);
}

// ------------------------------------------------------------ DMA
bool Vdc::dmaAllowed() const {
  if (!st_.allowDma && !st_.burst) return false;
  if (st_.hMode == H_HSW && st_.now + 3 - st_.hsyncStart <= dots(8)) return false;
  return true;
}

void Vdc::satbStep() {
  if (!dmaAllowed()) return;
  st_.satbCounter += 3;
  if (st_.satbCounter / div() >= 4) {
    st_.satbCounter -= 4 * div();
    const uint8_t i = st_.satbOffset;
    sat[i] = vramRead(static_cast<uint16_t>(st_.satb + i));
    st_.satbOffset++;
    if (st_.satbOffset == 0) {
      st_.satbRunning = 0;
      if (st_.dmaSatbIrq) { st_.stSatb = 1; setIrq(); }
    }
  }
}

void Vdc::vdmaStep() {
  if (!dmaAllowed()) return;
  st_.vdmaPending += 3;
  const int per = div() * 2;
  while (st_.vdmaPending >= per) {
    if (st_.vdmaRead) {
      st_.vdmaBuf = vramRead(st_.sour);
      st_.vdmaRead = 0;
    } else {
      st_.lenr--;
      if (st_.desr < vramWords) vram[st_.desr] = st_.vdmaBuf;
      st_.sour = static_cast<uint16_t>(st_.sour + (st_.dmaDecSrc ? -1 : 1));
      st_.desr = static_cast<uint16_t>(st_.desr + (st_.dmaDecDst ? -1 : 1));
      st_.vdmaRead = 1;
      if (st_.lenr == 0xFFFF) {
        st_.vdmaRunning = 0;
        st_.vdmaPending = 0;
        if (st_.dmaVramIrq) { st_.stDma = 1; setIrq(); }
        break;
      }
    }
    st_.vdmaPending -= per;
  }
}

// ------------------------------------------------------------ acesso da CPU à VRAM
void Vdc::vramAccess() {
  if (st_.transferDelay) {
    st_.transferDelay -= 3;
    if (st_.transferDelay > 0) return;
  }
  st_.transferDelay = 0;
  const int d = div();
  const bool inBg = !st_.burst && st_.hclock >= st_.bgStart && st_.hclock < st_.bgEnd && st_.scanline >= drawFirst && st_.scanline < drawEnd;
  bool blocked;
  if (st_.vMode != V_VDW || st_.burst || ((!st_.sprOn || st_.sprCount == 0) && !inBg)) {
    blocked = st_.satbRunning || st_.vdmaRunning || ((st_.hclock / d) & 1);
  } else {
    bool allow = st_.allowVram != 0;
    if (inBg) {
      const int end = (std::min(st_.bgEnd, st_.hclock) - st_.bgStart) / d;
      if (end > 0) {
        const int i = (end - 1) & 7;
        if (st_.lat.vramMode == 0) allow = (i & 1) == 0;
        else if (st_.lat.vramMode == 3) allow = false;
        else allow = i == 2 || i == 3;
        st_.allowVram = allow;
      }
    }
    blocked = inBg && !allow;
    if (!blocked && !inBg && st_.sprOn) {
      const int clocks = st_.hclock > st_.bgEnd ? (st_.hclock - st_.bgEnd) : (LINE - st_.bgEnd + st_.hclock);
      const int per = st_.lat.sprMode == 2 ? 8 : st_.lat.sprMode == 3 ? 16 : 4;
      if (clocks / d < st_.sprCount * per) blocked = true;
      else blocked = (st_.hclock / d) & 1;
    }
  }
  if (blocked) return;
  if (st_.hMode == H_HSW && st_.now + 3 - st_.hsyncStart < 8 * d) return;
  if (st_.pendRead) {
    st_.readBuf = vramRead(st_.marR);
    st_.marR = static_cast<uint16_t>(st_.marR + st_.incr);
    st_.pendRead = 0;
  } else if (st_.pendWrite) {
    if (st_.marW < vramWords) vram[st_.marW] = st_.vwr;
    st_.marW = static_cast<uint16_t>(st_.marW + st_.incr);
    st_.pendWrite = 0;
  }
}

void Vdc::queueRead() {
  st_.pendRead = 1;
  st_.transferDelay = div() == 2 ? 15 : 24;
}

void Vdc::queueWrite() {
  st_.pendWrite = 1;
  st_.transferDelay = div() == 2 ? 12 : div() == 3 ? 18 : 21;
}

void Vdc::waitVram() {
  // a CPU fica parada até o acesso pendente acontecer (limite de segurança: ~2 quadros)
  for (int n = 0; (st_.pendRead || st_.pendWrite) && n < 250000; n++) {
    sys->stall();
    draw();
  }
}

bool Vdc::latchWindow(uint16_t clock) const {
  if (clock == NEVER) return false;
  const int start = clock / div() * div();
  const int end = start + (div() + 1) / 2;
  return st_.hclock > start && st_.hclock < end;
}

// ------------------------------------------------------------ registradores
uint8_t Vdc::read(int a) {
  draw();
  switch (a & 3) {
    case 0: {
      const uint8_t r = static_cast<uint8_t>(((st_.pendRead || st_.pendWrite) ? 0x40 : 0) | (st_.stVbl ? 0x20 : 0) |
                                             (st_.stDma ? 0x10 : 0) | (st_.stSatb ? 0x08 : 0) | (st_.stRcr ? 0x04 : 0) |
                                             (st_.stOver ? 0x02 : 0) | (st_.stColl ? 0x01 : 0));
      st_.stVbl = st_.stDma = st_.stSatb = st_.stRcr = st_.stOver = st_.stColl = 0;
      st_.irq = 0;
      return r;
    }
    case 1: return 0;
    case 2:
      if (st_.pendRead) waitVram();
      return static_cast<uint8_t>(st_.readBuf);
    default: {
      if (st_.pendRead) waitVram();
      const uint8_t v = static_cast<uint8_t>(st_.readBuf >> 8);
      if (st_.reg == 2) queueRead();
      return v;
    }
  }
}

void Vdc::write(int a, uint8_t v) {
  draw();
  if ((a & 3) == 0) { st_.reg = v & 0x1F; return; }
  if ((a & 3) == 1) return;
  const bool msb = (a & 3) == 3;
  auto upd = [&](uint16_t& r, uint16_t mask) {
    r = static_cast<uint16_t>((msb ? ((r & 0xFF) | (v << 8)) : ((r & 0xFF00) | v)) & mask);
  };
  switch (st_.reg) {
    case 0x00: waitVram(); upd(st_.marW, 0xFFFF); break;
    case 0x01: waitVram(); upd(st_.marR, 0xFFFF); if (msb) queueRead(); break;
    case 0x02: waitVram(); upd(st_.vwr, 0xFFFF); if (msb) queueWrite(); break;
    case 0x05:
      if (msb) {
        static const uint16_t inc[4] = {1, 0x20, 0x40, 0x80};
        st_.incr = inc[(v >> 3) & 3];
      } else {
        st_.ieColl = v & 1; st_.ieOver = (v >> 1) & 1; st_.ieRcr = (v >> 2) & 1; st_.ieVbl = (v >> 3) & 1;
        st_.nextSpr = (v >> 6) & 1; st_.nextBg = (v >> 7) & 1;
        if (latchWindow(st_.latchClkY) && !st_.burst) { st_.sprOn = st_.nextSpr; st_.bgOn = st_.nextBg; }
      }
      break;
    case 0x06: upd(st_.rcr, 0x3FF); break;
    case 0x07:
      upd(st_.reg_.bxr, 0x3FF);
      if (latchWindow(st_.latchClkX)) st_.lat.bxr = st_.reg_.bxr;
      break;
    case 0x08:
      upd(st_.reg_.byr, 0x1FF);
      st_.byrPending = 1;
      if (latchWindow(st_.latchClkY)) incScrollY();
      break;
    case 0x09:
      if (!msb) {
        static const uint8_t cols[4] = {32, 64, 128, 128};
        st_.reg_.cols = cols[(v >> 4) & 3];
        st_.reg_.rows = (v & 0x40) ? 64 : 32;
        st_.reg_.vramMode = v & 3;
        st_.reg_.sprMode = (v >> 2) & 3;
        st_.reg_.cg = (v >> 7) & 1;
      }
      break;
    case 0x0A: if (msb) st_.reg_.hds = v & 0x7F; else st_.reg_.hsw = v & 0x1F; break;
    case 0x0B: if (msb) st_.reg_.hde = v & 0x7F; else st_.reg_.hdw = v & 0x7F; break;
    case 0x0C: if (msb) st_.reg_.vds = v; else st_.reg_.vsw = v & 0x1F; break;
    case 0x0D: upd(st_.reg_.vdw, 0x1FF); break;
    case 0x0E: if (!msb) st_.reg_.vcr = v; break;
    case 0x0F:
      if (!msb) {
        st_.dmaSatbIrq = v & 1; st_.dmaVramIrq = (v >> 1) & 1; st_.dmaDecSrc = (v >> 2) & 1;
        st_.dmaDecDst = (v >> 3) & 1; st_.dmaRepeat = (v >> 4) & 1;
      }
      break;
    case 0x10: upd(st_.sour, 0xFFFF); break;
    case 0x11: upd(st_.desr, 0xFFFF); break;
    case 0x12: upd(st_.lenr, 0xFFFF); if (msb) { st_.vdmaRunning = 1; st_.vdmaRead = 1; } break;
    case 0x13: upd(st_.satb, 0xFFFF); if (msb) st_.satbPending = 1; break;
    default: break;
  }
}

// ------------------------------------------------------------ save state
void Vdc::sync(StateIO& io) {
  io.raw(vram, vramWords * 2);
  io.raw(sat, sizeof sat);
  io.raw(row, sizeof row);
  io.raw(&st_, sizeof st_);
  io.raw(spr_, sizeof spr_);
  io.raw(drawSpr_, sizeof drawSpr_);
  io.raw(tiles_, sizeof tiles_);
  io.raw(xHasSpr_, sizeof xHasSpr_);
  if (io.reading()) {
    // só valores impossíveis são corrigidos (índices de tabelas e contadores que o hardware nunca tem)
    auto fixHv = [](Hv& h) {
      if (h.cols != 32 && h.cols != 64 && h.cols != 128) h.cols = 32;
      if (h.rows != 32 && h.rows != 64) h.rows = 32;
      h.vramMode &= 3; h.sprMode &= 3; h.cg &= 1;
      h.hsw &= 0x1F; h.hds &= 0x7F; h.hdw &= 0x7F; h.hde &= 0x7F; h.vsw &= 0x1F;
      h.vdw &= 0x1FF; h.bxr &= 0x3FF; h.byr &= 0x1FF;
    };
    fixHv(st_.reg_); fixHv(st_.lat);
    st_.reg &= 0x1F;
    st_.rcr &= 0x3FF;
    if (st_.incr != 1 && st_.incr != 0x20 && st_.incr != 0x40 && st_.incr != 0x80) st_.incr = 1;
    if (st_.hclock > LINE) st_.hclock = 0;
    if (st_.scanline > 263) st_.scanline = 0;
    st_.hMode &= 3; st_.vMode &= 3;
    if (st_.ev > E_RCR) st_.ev = E_NONE;
    clampTo<int32_t>(st_.hModeCounter, -8192, 1 << 30);
    clampTo<int32_t>(st_.vModeCounter, -1024, 1024);
    clampTo<int32_t>(st_.evCounter, -1, 8192);
    clampTo<int32_t>(st_.satbCounter, 0, 64);
    clampTo<int32_t>(st_.vdmaPending, 0, 64);
    clampTo<int32_t>(st_.transferDelay, -8, 64);
    if (st_.sprCount > 64) st_.sprCount = 64;
    if (st_.drawCount > 64) st_.drawCount = 64;
    if (st_.totalCount > 64) st_.totalCount = 64;
    if (st_.tileCount > 99) st_.tileCount = 99;
    if (st_.xStart > ROW) st_.xStart = ROW;
    for (Spr& s : spr_) clampTo<int16_t>(s.x, -32, 1023);
    for (Spr& s : drawSpr_) clampTo<int16_t>(s.x, -32, 1023);
    if (st_.now < st_.lineStart || st_.now - st_.lineStart > LINE) st_.lineStart = st_.now - st_.hclock;
  }
}

}  // namespace indigo::pce
