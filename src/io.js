// Registradores de IO: vídeo (armazenamento), DMA, timers, interrupções, teclado, controle do sistema

export const IRQ_VBLANK = 0x1, IRQ_HBLANK = 0x2, IRQ_VCOUNT = 0x4,
  IRQ_TIMER0 = 0x8, IRQ_DMA0 = 0x100, IRQ_KEYPAD = 0x1000;

const TIMER_SHIFT = [0, 6, 8, 10];
const DMA_SRC_MASK = [0x07FFFFFF, 0x0FFFFFFF, 0x0FFFFFFF, 0x0FFFFFFF];
const DMA_DST_MASK = [0x07FFFFFF, 0x07FFFFFF, 0x07FFFFFF, 0x0FFFFFFF];
const DMA_CNT_MASK = [0x3FFF, 0x3FFF, 0x3FFF, 0xFFFF];

// registradores legíveis (os demais devolvem 0)
const READABLE = new Uint8Array(0x200);
(function () {
  const ranges = [
    [0x000, 0x00E], [0x048, 0x04A], [0x050, 0x052],
    [0x060, 0x064], [0x068, 0x068], [0x06C, 0x06C], [0x070, 0x074], [0x078, 0x078], [0x07C, 0x07C],
    [0x080, 0x084], [0x088, 0x088], [0x090, 0x09E],
    [0x0BA, 0x0BA], [0x0C6, 0x0C6], [0x0D2, 0x0D2], [0x0DE, 0x0DE],
    [0x100, 0x10E], [0x120, 0x12A], [0x130, 0x134], [0x140, 0x140], [0x150, 0x158],
    [0x200, 0x204], [0x208, 0x208], [0x300, 0x300],
  ];
  for (const [a, b] of ranges) for (let x = a; x <= b; x += 2) READABLE[x >>> 1] = 1;
})();

export class IO {
  constructor(gba) {
    this.gba = gba;
    this.regs = new Uint16Array(0x200);
    this.ie = 0; this.if = 0; this.ime = 0;
    this.dispstat = 0; this.vcount = 0;
    this.keys = 0x3FF; // 1 = solta
    // timers
    this.tReload = new Int32Array(4);
    this.tCounter = new Int32Array(4);
    this.tCtrl = new Int32Array(4);
    this.tAcc = new Int32Array(4);
    this.timersActive = false;
    // DMA
    this.dSad = new Int32Array(4); this.dDad = new Int32Array(4);
    this.dCnt = new Int32Array(4); this.dCtrl = new Int32Array(4);
    this.dSrc = new Int32Array(4); this.dDst = new Int32Array(4); this.dCount = new Int32Array(4);
    this.dLatch = new Int32Array(4);
    this.dmaActive = false;
    this.postflg = 0;
  }

  reset() {
    this.regs.fill(0);
    this.ie = this.if = this.ime = 0;
    this.dispstat = 0; this.vcount = 0;
    this.stepTicked = 0;
    this.tReload.fill(0); this.tCounter.fill(0); this.tCtrl.fill(0); this.tAcc.fill(0);
    this.dSad.fill(0); this.dDad.fill(0); this.dCnt.fill(0); this.dCtrl.fill(0);
    this.timersActive = false;
    this.regs[0] = 0x80;
    this.regs[0x20 >> 1] = 0x100; this.regs[0x26 >> 1] = 0x100;
    this.regs[0x30 >> 1] = 0x100; this.regs[0x36 >> 1] = 0x100;
    this.regs[0x88 >> 1] = 0x200;
    this.regs[0x134 >> 1] = 0x8000;
  }

  // ---------------- interrupções ----------------
  requestIrq(bits) {
    this.if |= bits;
  }
  get irqPending() { return (this.ie & this.if & 0x3FFF) !== 0; }

  // ---------------- leitura ----------------
  read8(a) {
    const v = this.read16(a & ~1);
    return (a & 1) ? (v >>> 8) : (v & 0xFF);
  }

  read16(a) {
    if (a >= 0x400) {
      if ((a & 0xFFFE) === 0x800) return this.regs[0x1FF] ; // memctrl (espelho simples)
      return 0;
    }
    switch (a) {
      case 0x004: return this.dispstat;
      case 0x006: return this.vcount;
      case 0x100: case 0x104: case 0x108: case 0x10C: this.timerSync(); return this.tCounter[(a - 0x100) >>> 2] & 0xFFFF;
      case 0x130: return this.keys;
      case 0x200: return this.ie;
      case 0x202: return this.if;
      case 0x208: return this.ime;
      case 0x084: return (this.regs[0x84 >> 1] & 0x80) | this.gba.apu.statusBits();
      case 0x300: return this.postflg;
    }
    if (a >= 0x60 && a < 0xB0) return this.gba.apu.readReg(a, this.regs[a >>> 1]);
    if (!READABLE[a >>> 1]) return 0;
    return this.regs[a >>> 1];
  }

  // ---------------- escrita ----------------
  write8(a, v) {
    if (a >= 0x400) return;
    if (a === 0x301) { this.gba.cpu.halted = true; return; }
    if (a === 0x300) { this.postflg = v & 1; return; }
    if (a === 0x202) { this.if &= ~v; return; }
    if (a === 0x203) { this.if &= ~(v << 8); return; }
    if (a >= 0xA0 && a < 0xA8) { this.gba.apu.fifoWrite8(a, v); return; }
    const old = this.regs[a >>> 1];
    const nv = (a & 1) ? ((old & 0x00FF) | (v << 8)) : ((old & 0xFF00) | v);
    this.write16(a & ~1, nv);
  }

  write32(a, v) {
    if (a === 0xA0 || a === 0xA4) { this.gba.apu.fifoWrite32(a, v); return; }
    this.write16(a, v & 0xFFFF);
    this.write16(a + 2, (v >>> 16) & 0xFFFF);
  }

  write16(a, v) {
    if (a >= 0x400) {
      if ((a & 0xFFFE) === 0x800) this.regs[0x1FF] = v;
      return;
    }
    const idx = a >>> 1;
    switch (a) {
      case 0x004:
        this.dispstat = (this.dispstat & 0x0007) | (v & 0xFFB8);
        return;
      case 0x006: return;
      // referências afins (BG2X/BG2Y/BG3X/BG3Y) — recarregam o registrador interno
      case 0x028: case 0x02A: case 0x02C: case 0x02E:
      case 0x038: case 0x03A: case 0x03C: case 0x03E:
        this.regs[idx] = v;
        this.gba.ppu.latchAffine(a);
        return;
      case 0x0BA: case 0x0C6: case 0x0D2: case 0x0DE:
        this.writeDmaCtrl((a - 0xBA) / 12, v);
        return;
      case 0x100: case 0x104: case 0x108: case 0x10C:
        this.regs[idx] = v;
        this.tReload[(a - 0x100) >>> 2] = v;
        return;
      case 0x102: case 0x106: case 0x10A: case 0x10E:
        this.writeTimerCtrl((a - 0x102) >>> 2, v);
        return;
      case 0x130: return;
      case 0x132: this.regs[idx] = v & 0xC3FF; return;
      case 0x200: this.ie = v & 0x3FFF; return;
      case 0x202: this.if &= ~v; return;
      case 0x204:
        this.regs[idx] = v & 0xDFFF;
        this.gba.bus.setWaitcnt(v);
        return;
      case 0x208: this.ime = v & 1; return;
      case 0x300:
        this.postflg = v & 1;
        if (v & 0xFF00) this.gba.cpu.halted = true;
        return;
    }
    if (a >= 0x60 && a < 0xB0) {
      if (a >= 0xA0 && a < 0xA8) { this.gba.apu.fifoWrite16(a, v); return; }
      this.regs[idx] = v;
      this.gba.apu.writeReg(a, v);
      return;
    }
    if (a >= 0xB0 && a < 0xE0) {
      // endereços / contagem DMA
      const ch = Math.floor((a - 0xB0) / 12), off = (a - 0xB0) % 12;
      this.regs[idx] = v;
      const lo = this.regs[(0xB0 + ch * 12) >>> 1], hi = this.regs[(0xB2 + ch * 12) >>> 1];
      const dlo = this.regs[(0xB4 + ch * 12) >>> 1], dhi = this.regs[(0xB6 + ch * 12) >>> 1];
      if (off < 4) this.dSad[ch] = (lo | (hi << 16)) & DMA_SRC_MASK[ch];
      else if (off < 8) this.dDad[ch] = (dlo | (dhi << 16)) & DMA_DST_MASK[ch];
      else this.dCnt[ch] = v & DMA_CNT_MASK[ch];
      return;
    }
    this.regs[idx] = v;
  }

  // ---------------- teclado ----------------
  setKeys(mask) {
    this.keys = mask & 0x3FF;
    this.checkKeyIrq();
  }
  checkKeyIrq() {
    const cnt = this.regs[0x132 >> 1];
    if (!(cnt & 0x4000)) return;
    const sel = cnt & 0x3FF, pressed = (~this.keys) & 0x3FF;
    const hit = (cnt & 0x8000) ? ((pressed & sel) === sel) : ((pressed & sel) !== 0);
    if (hit) this.requestIrq(IRQ_KEYPAD);
  }

  // ---------------- timers ----------------
  // leva os timers até o ciclo exato do acesso atual (a instrução ainda está no meio)
  timerSync() {
    const now = this.gba.bus.cycles;
    const d = now - this.stepTicked;
    if (d > 0 && this.timersActive) this.tickTimers(d);
    this.stepTicked = now;
  }

  writeTimerCtrl(i, v) {
    this.timerSync();
    const old = this.tCtrl[i];
    this.regs[(0x102 + i * 4) >>> 1] = v & 0xC7;
    if (!(old & 0x80) && (v & 0x80)) {
      this.tCounter[i] = this.tReload[i];
      this.tAcc[i] = 0;
    }
    this.tCtrl[i] = v & 0xC7;
    this.timersActive = (this.tCtrl[0] | this.tCtrl[1] | this.tCtrl[2] | this.tCtrl[3]) & 0x80 ? true : false;
  }

  tickTimers(cycles) {
    for (let i = 0; i < 4; i++) {
      const ctrl = this.tCtrl[i];
      if (!(ctrl & 0x80)) continue;
      if (i > 0 && (ctrl & 0x4)) continue; // cascata
      const shift = TIMER_SHIFT[ctrl & 3];
      const acc = this.tAcc[i] + cycles;
      const inc = acc >>> shift;
      this.tAcc[i] = acc & ((1 << shift) - 1);
      if (inc) this.timerAdd(i, inc);
    }
  }

  timerAdd(i, inc) {
    let c = this.tCounter[i] + inc;
    while (c >= 0x10000) {
      c = c - 0x10000 + this.tReload[i];
      this.timerOverflow(i);
    }
    this.tCounter[i] = c;
  }

  timerOverflow(i) {
    if (this.tCtrl[i] & 0x40) this.requestIrq(IRQ_TIMER0 << i);
    if (i < 2) this.gba.apu.timerOverflow(i);
    if (i < 3) {
      const n = this.tCtrl[i + 1];
      if ((n & 0x80) && (n & 0x4)) this.timerAdd(i + 1, 1);
    }
  }

  cyclesToTimerEvent() {
    let best = 0x7FFFFFFF;
    for (let i = 0; i < 4; i++) {
      const ctrl = this.tCtrl[i];
      if (!(ctrl & 0x80) || (i > 0 && (ctrl & 0x4))) continue;
      const shift = TIMER_SHIFT[ctrl & 3];
      const c = ((0x10000 - this.tCounter[i]) << shift) - this.tAcc[i];
      if (c < best) best = c;
    }
    return best;
  }

  // ---------------- DMA ----------------
  writeDmaCtrl(ch, v) {
    const old = this.dCtrl[ch];
    if (ch !== 3) v &= 0xF7E0; else v &= 0xFFE0;
    this.dCtrl[ch] = v;
    this.regs[(0xBA + ch * 12) >>> 1] = v;
    if (!(old & 0x8000) && (v & 0x8000)) {
      this.dSrc[ch] = this.dSad[ch];
      this.dDst[ch] = this.dDad[ch];
      this.dCount[ch] = this.dCnt[ch];
      if (((v >>> 12) & 3) === 0) this.runDma(ch);
    }
  }

  // timing: 1 = VBlank, 2 = HBlank
  triggerDma(timing) {
    for (let ch = 0; ch < 4; ch++) {
      const c = this.dCtrl[ch];
      if ((c & 0x8000) && ((c >>> 12) & 3) === timing) this.runDma(ch);
    }
  }

  // DMA de som (FIFO A = 0x040000A0, FIFO B = 0x040000A4)
  soundDma(fifoAddr) {
    for (let ch = 1; ch <= 2; ch++) {
      const c = this.dCtrl[ch];
      if ((c & 0x8000) && ((c >>> 12) & 3) === 3 && this.dDad[ch] === fifoAddr) {
        this.runDma(ch, true);
      }
    }
  }

  runDma(ch, sound) {
    const bus = this.gba.bus;
    const ctrl = this.dCtrl[ch];
    const word = sound ? true : (ctrl & 0x400) !== 0;
    let count = sound ? 4 : this.dCount[ch];
    if (count === 0) count = ch === 3 ? 0x10000 : 0x4000;
    const srcCtl = (ctrl >>> 7) & 3, dstCtl = sound ? 2 : (ctrl >>> 5) & 3;
    const size = word ? 4 : 2;
    const sStep = srcCtl === 0 ? size : srcCtl === 1 ? -size : 0;
    const dStep = (dstCtl === 0 || dstCtl === 3) ? size : dstCtl === 1 ? -size : 0;
    let src = this.dSrc[ch], dst = this.dDst[ch];
    const save = bus.save;
    if (ch === 3 && save && save.isEeprom && ((dst >>> 24) === 0xD)) save.dmaHint(count, true);
    if (ch === 3 && save && save.isEeprom && ((src >>> 24) === 0xD)) save.dmaHint(count, false);
    if (word) {
      src &= ~3; dst &= ~3;
      for (let k = 0; k < count; k++) {
        let val;
        if ((src >>> 0) >= 0x02000000) { val = bus.read32(src); this.dLatch[ch] = val; } else val = this.dLatch[ch];
        bus.write32(dst, val);
        src = (src + sStep) | 0; dst = (dst + dStep) | 0;
      }
    } else {
      src &= ~1; dst &= ~1;
      for (let k = 0; k < count; k++) {
        let val;
        if ((src >>> 0) >= 0x02000000) { val = bus.read16(src); this.dLatch[ch] = val * 0x10001; } else val = this.dLatch[ch] & 0xFFFF;
        bus.write16(dst, val);
        src = (src + sStep) | 0; dst = (dst + dStep) | 0;
      }
    }
    bus.cycles += 2;
    this.dSrc[ch] = src;
    if (!sound) this.dDst[ch] = dst;
    if (ctrl & 0x4000) this.requestIrq(IRQ_DMA0 << ch);
    const timing = (ctrl >>> 12) & 3;
    if ((ctrl & 0x200) && timing !== 0) {
      this.dCount[ch] = this.dCnt[ch];
      if (dstCtl === 3) this.dDst[ch] = this.dDad[ch];
    } else {
      this.dCtrl[ch] = ctrl & 0x7FFF;
      this.regs[(0xBA + ch * 12) >>> 1] = ctrl & 0x7FFF;
    }
  }
}
