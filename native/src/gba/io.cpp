// Registradores de E/S do GBA (porte de src/io.js): vídeo (armazenamento), DMA, timers, interrupções, teclado
#include <algorithm>

#include "gba.h"

namespace indigo::gba {

namespace {
const int TIMER_SHIFT[4] = {0, 6, 8, 10};
const uint32_t DMA_SRC_MASK[4] = {0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF};
const uint32_t DMA_DST_MASK[4] = {0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF};
const int DMA_CNT_MASK[4] = {0x3FFF, 0x3FFF, 0x3FFF, 0xFFFF};

// registradores legíveis (os demais devolvem 0)
struct Readable {
  uint8_t t[0x200] = {};
  Readable() {
    const int ranges[][2] = {
      {0x000, 0x00E}, {0x048, 0x04A}, {0x050, 0x052},
      {0x060, 0x064}, {0x068, 0x068}, {0x06C, 0x06C}, {0x070, 0x074}, {0x078, 0x078}, {0x07C, 0x07C},
      {0x080, 0x084}, {0x088, 0x088}, {0x090, 0x09E},
      {0x0BA, 0x0BA}, {0x0C6, 0x0C6}, {0x0D2, 0x0D2}, {0x0DE, 0x0DE},
      {0x100, 0x10E}, {0x120, 0x12A}, {0x130, 0x134}, {0x140, 0x140}, {0x150, 0x158},
      {0x200, 0x204}, {0x208, 0x208}, {0x300, 0x300},
    };
    for (const auto& rg : ranges) for (int x = rg[0]; x <= rg[1]; x += 2) t[x >> 1] = 1;
  }
};
const Readable READABLE;
}  // namespace

void IO::reset() {
  std::fill(std::begin(regs), std::end(regs), 0);
  ie = ifr = ime = 0;
  dispstat = 0; vcount = 0;
  stepTicked = 0;
  for (int k = 0; k < 4; k++) { tReload[k] = tCounter[k] = tCtrl[k] = tAcc[k] = 0; dSad[k] = dDad[k] = dCnt[k] = dCtrl[k] = 0; }
  timersActive = false;
  regs[0] = 0x80;
  regs[0x20 >> 1] = 0x100; regs[0x26 >> 1] = 0x100;
  regs[0x30 >> 1] = 0x100; regs[0x36 >> 1] = 0x100;
  regs[0x88 >> 1] = 0x200;
  regs[0x134 >> 1] = 0x8000;
}

// ---------------- leitura ----------------
int IO::read8(int a) {
  const int v = read16(a & ~1);
  return (a & 1) ? (v >> 8) & 0xFF : (v & 0xFF);
}

int IO::read16(int a) {
  if (a >= 0x400) {
    if ((a & 0xFFFE) == 0x800) return regs[0x1FF];  // memctrl (espelho simples)
    return 0;
  }
  switch (a) {
    case 0x004: return dispstat;
    case 0x006: return vcount;
    case 0x100: case 0x104: case 0x108: case 0x10C: timerSync(); return tCounter[(a - 0x100) >> 2] & 0xFFFF;
    case 0x130: return keys;
    case 0x200: return ie;
    case 0x202: return ifr;
    case 0x208: return ime;
    case 0x084: return (regs[0x84 >> 1] & 0x80) | gba.apu.statusBits();
    case 0x300: return postflg;
  }
  if (a >= 0x60 && a < 0xB0) return gba.apu.readReg(a, regs[a >> 1]);
  if (!READABLE.t[a >> 1]) return 0;
  return regs[a >> 1];
}

// ---------------- escrita ----------------
void IO::write8(int a, int v) {
  if (a >= 0x400) return;
  if (a == 0x301) { gba.cpu.halted = true; return; }
  if (a == 0x300) { postflg = v & 1; return; }
  if (a == 0x202) { ifr &= ~v; return; }
  if (a == 0x203) { ifr &= ~(v << 8); return; }
  if (a >= 0xA0 && a < 0xA8) { gba.apu.fifoWrite8(a, v); return; }
  const int old = regs[a >> 1];
  const int nv = (a & 1) ? ((old & 0x00FF) | (v << 8)) : ((old & 0xFF00) | v);
  write16(a & ~1, nv);
}

void IO::write32(int a, int32_t v) {
  if (a == 0xA0 || a == 0xA4) { gba.apu.fifoWrite32(a, v); return; }
  write16(a, static_cast<int>(U32(v) & 0xFFFF));
  write16(a + 2, static_cast<int>((U32(v) >> 16) & 0xFFFF));
}

void IO::write16(int a, int v) {
  if (a >= 0x400) {
    if ((a & 0xFFFE) == 0x800) regs[0x1FF] = static_cast<uint16_t>(v);
    return;
  }
  const int idx = a >> 1;
  switch (a) {
    case 0x004:
      dispstat = (dispstat & 0x0007) | (v & 0xFFB8);
      return;
    case 0x006: return;
    // referências afins (BG2X/BG2Y/BG3X/BG3Y): recarregam o registrador interno
    case 0x028: case 0x02A: case 0x02C: case 0x02E:
    case 0x038: case 0x03A: case 0x03C: case 0x03E:
      regs[idx] = static_cast<uint16_t>(v);
      gba.ppu.latchAffine(a);
      return;
    case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
      writeDmaCtrl((a - 0xBA) / 12, v);
      return;
    case 0x100: case 0x104: case 0x108: case 0x10C:
      regs[idx] = static_cast<uint16_t>(v);
      tReload[(a - 0x100) >> 2] = v;
      return;
    case 0x102: case 0x106: case 0x10A: case 0x10E:
      writeTimerCtrl((a - 0x102) >> 2, v);
      return;
    case 0x130: return;
    case 0x132: regs[idx] = static_cast<uint16_t>(v & 0xC3FF); return;
    case 0x200: ie = v & 0x3FFF; return;
    case 0x202: ifr &= ~v; return;
    case 0x204:
      regs[idx] = static_cast<uint16_t>(v & 0xDFFF);
      gba.bus.setWaitcnt(v);
      return;
    case 0x208: ime = v & 1; return;
    case 0x300:
      postflg = v & 1;
      if (v & 0xFF00) gba.cpu.halted = true;
      return;
  }
  if (a >= 0x60 && a < 0xB0) {
    if (a >= 0xA0 && a < 0xA8) { gba.apu.fifoWrite16(a, v); return; }
    regs[idx] = static_cast<uint16_t>(v);
    gba.apu.writeReg(a, v);
    return;
  }
  if (a >= 0xB0 && a < 0xE0) {
    // endereços / contagem do DMA
    const int ch = (a - 0xB0) / 12, off = (a - 0xB0) % 12;
    regs[idx] = static_cast<uint16_t>(v);
    const uint32_t lo = regs[(0xB0 + ch * 12) >> 1], hi = regs[(0xB2 + ch * 12) >> 1];
    const uint32_t dlo = regs[(0xB4 + ch * 12) >> 1], dhi = regs[(0xB6 + ch * 12) >> 1];
    if (off < 4) dSad[ch] = static_cast<int32_t>((lo | (hi << 16)) & DMA_SRC_MASK[ch]);
    else if (off < 8) dDad[ch] = static_cast<int32_t>((dlo | (dhi << 16)) & DMA_DST_MASK[ch]);
    else dCnt[ch] = v & DMA_CNT_MASK[ch];
    return;
  }
  regs[idx] = static_cast<uint16_t>(v);
}

// ---------------- teclado ----------------
void IO::setKeys(int mask) {
  keys = mask & 0x3FF;
  checkKeyIrq();
}

void IO::checkKeyIrq() {
  const int cnt = regs[0x132 >> 1];
  if (!(cnt & 0x4000)) return;
  const int sel = cnt & 0x3FF, pressed = (~keys) & 0x3FF;
  const bool hit = (cnt & 0x8000) ? ((pressed & sel) == sel) : ((pressed & sel) != 0);
  if (hit) requestIrq(IRQ_KEYPAD);
}

// ---------------- timers ----------------
// leva os timers até o ciclo exato do acesso atual (a instrução ainda está no meio)
void IO::timerSync() {
  const int64_t now = gba.bus.cycles;
  const int64_t d = now - stepTicked;
  if (d > 0 && timersActive) tickTimers(d);
  stepTicked = now;
}

void IO::writeTimerCtrl(int i, int v) {
  timerSync();
  const int old = tCtrl[i];
  regs[(0x102 + i * 4) >> 1] = static_cast<uint16_t>(v & 0xC7);
  if (!(old & 0x80) && (v & 0x80)) {
    tCounter[i] = tReload[i];
    tAcc[i] = 0;
  }
  tCtrl[i] = v & 0xC7;
  timersActive = ((tCtrl[0] | tCtrl[1] | tCtrl[2] | tCtrl[3]) & 0x80) != 0;
}

void IO::tickTimers(int64_t cycles) {
  for (int i = 0; i < 4; i++) {
    const int ctrl = tCtrl[i];
    if (!(ctrl & 0x80)) continue;
    if (i > 0 && (ctrl & 0x4)) continue;  // cascata
    const int shift = TIMER_SHIFT[ctrl & 3];
    const int64_t acc = int64_t(tAcc[i]) + cycles;
    const uint32_t inc = U32(acc) >> shift;
    tAcc[i] = I32(acc) & ((1 << shift) - 1);
    if (inc) timerAdd(i, inc);
  }
}

void IO::timerAdd(int i, int64_t inc) {
  int64_t c = int64_t(tCounter[i]) + inc;
  while (c >= 0x10000) {
    c = c - 0x10000 + tReload[i];
    timerOverflow(i);
  }
  tCounter[i] = I32(c);
}

void IO::timerOverflow(int i) {
  if (tCtrl[i] & 0x40) requestIrq(IRQ_TIMER0 << i);
  if (i < 2) gba.apu.timerOverflow(i);
  if (i < 3) {
    const int n = tCtrl[i + 1];
    if ((n & 0x80) && (n & 0x4)) timerAdd(i + 1, 1);
  }
}

int32_t IO::cyclesToTimerEvent() const {
  int32_t best = 0x7FFFFFFF;
  for (int i = 0; i < 4; i++) {
    const int ctrl = tCtrl[i];
    if (!(ctrl & 0x80) || (i > 0 && (ctrl & 0x4))) continue;
    const int shift = TIMER_SHIFT[ctrl & 3];
    const int32_t c = I32(int64_t(I32(int64_t(0x10000 - tCounter[i]) << shift)) - tAcc[i]);
    if (c < best) best = c;
  }
  return best;
}

// ---------------- DMA ----------------
void IO::writeDmaCtrl(int ch, int v) {
  const int old = dCtrl[ch];
  if (ch != 3) v &= 0xF7E0; else v &= 0xFFE0;
  dCtrl[ch] = v;
  regs[(0xBA + ch * 12) >> 1] = static_cast<uint16_t>(v);
  if (!(old & 0x8000) && (v & 0x8000)) {
    dSrc[ch] = dSad[ch];
    dDst[ch] = dDad[ch];
    dCount[ch] = dCnt[ch];
    if (((v >> 12) & 3) == 0) runDma(ch);
  }
}

// timing: 1 = VBlank, 2 = HBlank
void IO::triggerDma(int timing) {
  for (int ch = 0; ch < 4; ch++) {
    const int c = dCtrl[ch];
    if ((c & 0x8000) && ((c >> 12) & 3) == timing) runDma(ch);
  }
}

// DMA de som (FIFO A = 0x040000A0, FIFO B = 0x040000A4)
void IO::soundDma(int32_t fifoAddr) {
  for (int ch = 1; ch <= 2; ch++) {
    const int c = dCtrl[ch];
    if ((c & 0x8000) && ((c >> 12) & 3) == 3 && dDad[ch] == fifoAddr) runDma(ch, true);
  }
}

void IO::runDma(int ch, bool sound) {
  Bus& bus = gba.bus;
  const int ctrl = dCtrl[ch];
  const bool word = sound ? true : (ctrl & 0x400) != 0;
  int count = sound ? 4 : dCount[ch];
  if (count == 0) count = ch == 3 ? 0x10000 : 0x4000;
  const int srcCtl = (ctrl >> 7) & 3, dstCtl = sound ? 2 : (ctrl >> 5) & 3;
  const int size = word ? 4 : 2;
  const int sStep = srcCtl == 0 ? size : srcCtl == 1 ? -size : 0;
  const int dStep = (dstCtl == 0 || dstCtl == 3) ? size : dstCtl == 1 ? -size : 0;
  int32_t src = dSrc[ch], dst = dDst[ch];
  Save* save = bus.save.get();
  if (ch == 3 && save && save->isEeprom && ((U32(dst) >> 24) == 0xD)) save->dmaHint(count, true);
  if (ch == 3 && save && save->isEeprom && ((U32(src) >> 24) == 0xD)) save->dmaHint(count, false);
  if (word) {
    src &= ~3; dst &= ~3;
    for (int k = 0; k < count; k++) {
      int32_t val;
      if (U32(src) >= 0x02000000) { val = bus.read32(src); dLatch[ch] = val; } else val = dLatch[ch];
      bus.write32(dst, val);
      src = I32(int64_t(src) + sStep); dst = I32(int64_t(dst) + dStep);
    }
  } else {
    src &= ~1; dst &= ~1;
    for (int k = 0; k < count; k++) {
      int32_t val;
      if (U32(src) >= 0x02000000) { val = bus.read16(src); dLatch[ch] = static_cast<int32_t>(U32(val) * 0x10001u); }
      else val = dLatch[ch] & 0xFFFF;
      bus.write16(dst, val);
      src = I32(int64_t(src) + sStep); dst = I32(int64_t(dst) + dStep);
    }
  }
  bus.cycles += 2;
  dSrc[ch] = src;
  if (!sound) dDst[ch] = dst;
  if (ctrl & 0x4000) requestIrq(IRQ_DMA0 << ch);
  const int timing = (ctrl >> 12) & 3;
  if ((ctrl & 0x200) && timing != 0) {
    dCount[ch] = dCnt[ch];
    if (dstCtl == 3) dDst[ch] = dDad[ch];
  } else {
    dCtrl[ch] = ctrl & 0x7FFF;
    regs[(0xBA + ch * 12) >> 1] = static_cast<uint16_t>(ctrl & 0x7FFF);
  }
}

void IO::sync(StateIO& io) {
  io.raw(regs, sizeof regs);
  for (int* p : {&ie, &ifr, &ime, &dispstat, &vcount, &keys, &postflg}) io.sync(*p);
  io.raw(tReload, sizeof tReload); io.raw(tCounter, sizeof tCounter); io.raw(tCtrl, sizeof tCtrl); io.raw(tAcc, sizeof tAcc);
  io.sync(timersActive);
  io.raw(dSad, sizeof dSad); io.raw(dDad, sizeof dDad); io.raw(dCnt, sizeof dCnt); io.raw(dCtrl, sizeof dCtrl);
  io.raw(dSrc, sizeof dSrc); io.raw(dDst, sizeof dDst); io.raw(dCount, sizeof dCount); io.raw(dLatch, sizeof dLatch);
  io.sync(stepTicked);
  if (io.reading()) {
    ie &= 0x3FFF; ifr &= 0xFFFF; ime &= 1; dispstat &= 0xFFFF; vcount &= 0xFF; keys &= 0x3FF; postflg &= 1;
    for (int k = 0; k < 4; k++) {
      tReload[k] &= 0xFFFF; tCounter[k] &= 0xFFFF; tCtrl[k] &= 0xC7; tAcc[k] &= 0x3FF;
      dCnt[k] &= DMA_CNT_MASK[k]; dCount[k] &= 0xFFFF; dCtrl[k] &= 0xFFE0;
    }
  }
}

}  // namespace indigo::gba
