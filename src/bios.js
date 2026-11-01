// BIOS em alto nível (HLE): substitui a BIOS proprietária da Nintendo.
// Instala um pequeno tratador de IRQ em código ARM e emula as chamadas SWI em JavaScript.

const IRQ_STUB = [
  // 0x00 reset, 0x04 und, 0x08 swi, 0x0C pabt, 0x10 dabt, 0x14 -, 0x18 irq, 0x1C fiq
  [0x00, 0xEA00001E], // b 0x80 (laço de segurança)
  [0x18, 0xEA000042], // b 0x128
  [0x80, 0xEAFFFFFE], // b .
  [0x128, 0xE92D500F], // stmfd sp!, {r0-r3, r12, lr}
  [0x12C, 0xE3A00301], // mov r0, #0x04000000
  [0x130, 0xE28FE000], // add lr, pc, #0
  [0x134, 0xE510F004], // ldr pc, [r0, #-4]
  [0x138, 0xE8BD500F], // ldmfd sp!, {r0-r3, r12, lr}
  [0x13C, 0xE25EF004], // subs pc, lr, #4
  [0x144, 0xE55EC002], // (nunca executada; é o valor de prefetch após o retorno da IRQ)
];

export function installHleBios(bus) {
  bus.bios.fill(0);
  for (const [addr, op] of IRQ_STUB) bus.bios32[addr >>> 2] = op | 0;
}

export class HleBios {
  constructor(gba) {
    this.gba = gba;
    this.waiting = false;
  }

  get cpu() { return this.gba.cpu; }
  get bus() { return this.gba.bus; }

  handle(n, addr) {
    const cpu = this.cpu, r = cpu.r, bus = this.bus;
    bus.cycles += 8;
    bus.biosLast = 0xE3A02004 | 0; // valor que a BIOS real deixa no barramento após um SWI
    switch (n) {
      case 0x00: this.softReset(); break;
      case 0x01: this.registerRamReset(r[0]); break;
      case 0x02: cpu.halted = true; break;
      case 0x03: cpu.halted = true; break;
      case 0x04: this.intrWait(r[0], r[1], addr); break;
      case 0x05: r[0] = 1; r[1] = 1; this.intrWait(1, 1, addr); break;
      case 0x06: this.div(r[0], r[1]); break;
      case 0x07: this.div(r[1], r[0]); break;
      case 0x08: r[0] = Math.floor(Math.sqrt(r[0] >>> 0)); break;
      case 0x09: this.arcTan(); break;
      case 0x0A: this.arcTan2(); break;
      case 0x0B: this.cpuSet(); break;
      case 0x0C: this.cpuFastSet(); break;
      case 0x0D: r[0] = 0xBAAE187F | 0; break;
      case 0x0E: this.bgAffineSet(); break;
      case 0x0F: this.objAffineSet(); break;
      case 0x10: this.bitUnPack(); break;
      case 0x11: this.lz77(false); break;
      case 0x12: this.lz77(true); break;
      case 0x13: this.huffman(); break;
      case 0x14: this.rle(false); break;
      case 0x15: this.rle(true); break;
      case 0x16: this.diff(8, false); break;
      case 0x17: this.diff(8, true); break;
      case 0x18: this.diff(16, true); break;
      case 0x19: break; // SoundBias
      case 0x1F: this.midiKey2Freq(); break;
      default: break;
    }
  }

  softReset() {
    const bus = this.bus, cpu = this.cpu;
    const flag = bus.iwram[0x7FFA];
    for (let a = 0x7E00; a < 0x8000; a++) bus.iwram[a] = 0;
    cpu.switchMode(0x13); cpu.r[13] = 0x03007FE0; cpu.r[14] = 0; cpu.spsr = 0;
    cpu.switchMode(0x12); cpu.r[13] = 0x03007FA0; cpu.r[14] = 0; cpu.spsr = 0;
    cpu.switchMode(0x1F); cpu.r[13] = 0x03007F00;
    for (let k = 0; k < 13; k++) cpu.r[k] = 0;
    cpu.t = 0;
    cpu.setPC(flag ? 0x02000000 : 0x08000000);
  }

  registerRamReset(flags) {
    const bus = this.bus, io = this.gba.io;
    if (flags & 0x01) bus.ewram.fill(0);
    if (flags & 0x02) bus.iwram.fill(0, 0, 0x7E00);
    if (flags & 0x04) bus.pal.fill(0);
    if (flags & 0x08) bus.vram.fill(0);
    if (flags & 0x10) bus.oam.fill(0);
    if (flags & 0x80) {
      for (let a = 0x8; a < 0x60; a += 2) io.write16(a, 0);
      io.write16(0x20, 0x100); io.write16(0x26, 0x100); io.write16(0x30, 0x100); io.write16(0x36, 0x100);
    }
    io.write16(0, 0x80);
  }

  intrWait(discard, mask, addr) {
    const bus = this.bus, io = this.gba.io, cpu = this.cpu;
    if (!this.waiting) {
      this.waiting = true;
      if (discard) bus.iwram16[0x7FF8 >>> 1] &= ~mask;
    }
    io.ime = 1;
    const flags = bus.iwram16[0x7FF8 >>> 1];
    if (flags & mask) {
      bus.iwram16[0x7FF8 >>> 1] = flags & ~mask;
      this.waiting = false;
      return;
    }
    cpu.halted = true;
    cpu.setPC(addr); // reexecuta o SWI ao acordar
  }

  div(num, den) {
    const r = this.cpu.r;
    if (den === 0) { r[0] = num < 0 ? -1 : 1; r[1] = num; r[3] = 1; return; }
    const q = (num / den) | 0;
    r[0] = q;
    r[1] = (num - Math.imul(q, den)) | 0;
    r[3] = Math.abs(q);
  }

  arcTan() {
    const r = this.cpu.r;
    const x = r[0];
    const a = -(Math.imul(x, x) >> 14);
    let b = ((Math.imul(0xA9, a)) >> 14) + 0x390;
    b = (Math.imul(b, a) >> 14) + 0x91C;
    b = (Math.imul(b, a) >> 14) + 0xFB6;
    b = (Math.imul(b, a) >> 14) + 0x16AA;
    b = (Math.imul(b, a) >> 14) + 0x2081;
    b = (Math.imul(b, a) >> 14) + 0x3651;
    b = (Math.imul(b, a) >> 14) + 0xA2F9;
    r[0] = Math.imul(x, b) >> 16;
  }

  arcTan2() {
    const r = this.cpu.r;
    const x = (r[0] << 16) >> 16, y = (r[1] << 16) >> 16;
    let ang = Math.atan2(y, x);
    if (ang < 0) ang += 2 * Math.PI;
    r[0] = Math.round(ang / (2 * Math.PI) * 0x10000) & 0xFFFF;
  }

  cpuSet() {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0], dst = r[1];
    const ctl = r[2];
    const count = ctl & 0x1FFFFF;
    const fixed = (ctl & 0x01000000) !== 0;
    if (ctl & 0x04000000) {
      src &= ~3; dst &= ~3;
      const val = fixed ? bus.read32(src) : 0;
      for (let k = 0; k < count; k++) {
        bus.write32(dst, fixed ? val : bus.read32(src));
        if (!fixed) src += 4;
        dst += 4;
      }
    } else {
      src &= ~1; dst &= ~1;
      const val = fixed ? bus.read16(src) : 0;
      for (let k = 0; k < count; k++) {
        bus.write16(dst, fixed ? val : bus.read16(src));
        if (!fixed) src += 2;
        dst += 2;
      }
    }
  }

  cpuFastSet() {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0] & ~3, dst = r[1] & ~3;
    const ctl = r[2];
    const count = ((ctl & 0x1FFFFF) + 7) & ~7;
    const fixed = (ctl & 0x01000000) !== 0;
    const val = fixed ? bus.read32(src) : 0;
    for (let k = 0; k < count; k++) {
      bus.write32(dst, fixed ? val : bus.read32(src));
      if (!fixed) src += 4;
      dst += 4;
    }
  }

  sinCos(angle) {
    const theta = ((angle >>> 8) & 0xFF) / 256 * 2 * Math.PI;
    return [Math.round(Math.sin(theta) * 16384), Math.round(Math.cos(theta) * 16384)];
  }

  bgAffineSet() {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0], dst = r[1];
    for (let k = 0; k < r[2]; k++) {
      const ox = bus.read32(src), oy = bus.read32(src + 4);
      const dx = (bus.read16(src + 8) << 16) >> 16, dy = (bus.read16(src + 10) << 16) >> 16;
      const sx = (bus.read16(src + 12) << 16) >> 16, sy = (bus.read16(src + 14) << 16) >> 16;
      const ang = bus.read16(src + 16);
      const [sin, cos] = this.sinCos(ang);
      const pa = Math.imul(sx, cos) >> 14, pb = -(Math.imul(sx, sin) >> 14);
      const pc = Math.imul(sy, sin) >> 14, pd = Math.imul(sy, cos) >> 14;
      bus.write16(dst, pa); bus.write16(dst + 2, pb); bus.write16(dst + 4, pc); bus.write16(dst + 6, pd);
      bus.write32(dst + 8, ox - (Math.imul(pa, dx) + Math.imul(pb, dy)));
      bus.write32(dst + 12, oy - (Math.imul(pc, dx) + Math.imul(pd, dy)));
      src += 20; dst += 16;
    }
  }

  objAffineSet() {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0], dst = r[1];
    const off = r[3];
    for (let k = 0; k < r[2]; k++) {
      const sx = (bus.read16(src) << 16) >> 16, sy = (bus.read16(src + 2) << 16) >> 16;
      const ang = bus.read16(src + 4);
      const [sin, cos] = this.sinCos(ang);
      bus.write16(dst, Math.imul(sx, cos) >> 14);
      bus.write16(dst + off, -(Math.imul(sx, sin) >> 14));
      bus.write16(dst + off * 2, Math.imul(sy, sin) >> 14);
      bus.write16(dst + off * 3, Math.imul(sy, cos) >> 14);
      src += 8; dst += off * 4;
    }
  }

  bitUnPack() {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0], dst = r[1];
    const info = r[2];
    const len = bus.read16(info);
    const sw = bus.read8(info + 2), dw = bus.read8(info + 3);
    const offRaw = bus.read32(info + 4);
    const offset = offRaw & 0x7FFFFFFF, zeroFlag = (offRaw >>> 31) !== 0;
    let out = 0, outBits = 0;
    const smask = (1 << sw) - 1;
    for (let k = 0; k < len; k++) {
      const byte = bus.read8(src + k);
      for (let b = 0; b < 8; b += sw) {
        let v = (byte >>> b) & smask;
        if (v !== 0 || zeroFlag) v = (v + offset) | 0;
        out |= (dw === 32 ? v : (v & ((1 << dw) - 1))) << outBits;
        outBits += dw;
        if (outBits >= 32) {
          bus.write32(dst, out); dst += 4; out = 0; outBits = 0;
        }
      }
    }
  }

  // escreve um fluxo de bytes descomprimidos (em VRAM somente em unidades de 16 bits)
  writer(dst, vram) {
    const bus = this.bus;
    let pending = -1;
    let d = dst;
    const buf = [];
    return {
      put(b) {
        buf.push(b);
        if (!vram) { bus.write8(d++, b); return; }
        if (pending < 0) pending = b;
        else { bus.write16(d, pending | (b << 8)); d += 2; pending = -1; }
      },
      get(back) { return buf[buf.length - back]; },
      get length() { return buf.length; },
    };
  }

  lz77(vram) {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0];
    const header = bus.read32(src); src += 4;
    const size = header >>> 8;
    const w = this.writer(r[1], vram);
    while (w.length < size) {
      const flags = bus.read8(src++);
      for (let b = 7; b >= 0 && w.length < size; b--) {
        if (flags & (1 << b)) {
          const b1 = bus.read8(src++), b2 = bus.read8(src++);
          const len = (b1 >>> 4) + 3;
          const disp = (((b1 & 0xF) << 8) | b2) + 1;
          for (let k = 0; k < len && w.length < size; k++) w.put(w.get(disp));
        } else {
          w.put(bus.read8(src++));
        }
      }
    }
  }

  rle(vram) {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0];
    const header = bus.read32(src); src += 4;
    const size = header >>> 8;
    const w = this.writer(r[1], vram);
    while (w.length < size) {
      const f = bus.read8(src++);
      if (f & 0x80) {
        const len = (f & 0x7F) + 3, v = bus.read8(src++);
        for (let k = 0; k < len && w.length < size; k++) w.put(v);
      } else {
        const len = (f & 0x7F) + 1;
        for (let k = 0; k < len && w.length < size; k++) w.put(bus.read8(src++));
      }
    }
  }

  huffman() {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0];
    let dst = r[1];
    const header = bus.read32(src);
    const bits = header & 0xF;
    const size = header >>> 8;
    const treeSize = bus.read8(src + 4);
    const treeBase = src + 5;
    let stream = src + 4 + (treeSize + 1) * 2;
    let outWord = 0, outBits = 0, written = 0;
    let node = treeBase, nodeVal = bus.read8(node);
    while (written < size) {
      const word = bus.read32(stream); stream += 4;
      for (let b = 31; b >= 0 && written < size; b--) {
        const dir = (word >>> b) & 1;
        const offset = nodeVal & 0x3F;
        const next = (node & ~1) + offset * 2 + 2;
        const isLeaf = dir ? (nodeVal & 0x40) : (nodeVal & 0x80);
        node = next + dir;
        nodeVal = bus.read8(node);
        if (isLeaf) {
          outWord |= (nodeVal & ((1 << bits) - 1)) << outBits;
          outBits += bits;
          if (outBits >= 32) { bus.write32(dst, outWord); dst += 4; written += 4; outWord = 0; outBits = 0; }
          node = treeBase; nodeVal = bus.read8(node);
        }
      }
    }
  }

  diff(width, vram) {
    const r = this.cpu.r, bus = this.bus;
    let src = r[0], dst = r[1];
    const header = bus.read32(src); src += 4;
    const size = header >>> 8;
    if (width === 8) {
      const w = this.writer(dst, vram);
      let acc = 0;
      for (let k = 0; k < size; k++) { acc = (acc + bus.read8(src++)) & 0xFF; w.put(acc); }
    } else {
      let acc = 0;
      for (let k = 0; k < size; k += 2) { acc = (acc + bus.read16(src)) & 0xFFFF; src += 2; bus.write16(dst, acc); dst += 2; }
    }
  }

  midiKey2Freq() {
    const r = this.cpu.r, bus = this.bus;
    const freq = bus.read32(r[0] + 4) >>> 0;
    const v = freq / Math.pow(2, (180 - r[1] - r[2] / 256) / 12);
    r[0] = v >>> 0;
  }
}
