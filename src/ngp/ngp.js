// Neo Geo Pocket / Pocket Color: TLCS-900H a 6,144 MHz, Z80 de som a 3,072 MHz, vídeo K2GE, som T6W28 + DAC,
// timers de 8 bits, micro-DMA, relógio e flash do cartucho. A BIOS é a própria do Índigo (bios.js).
// Cada linha de vídeo dura 515 ciclos; o quadro tem 199 linhas (152 visíveis), ~59,95 quadros/s.

import { TLCS900H } from './tlcs900h.js';
import { Z80 } from '../sms/z80.js';
import { K2GE, W, H } from './k2ge.js';
import { T6W28 } from './t6w28.js';
import { ngpInfo } from './header.js';
import { buildBios, biosCall, STUB_BASE, SWI1_STUB, DEFAULT_IRQ } from './bios.js';

export const KEY = { A: 0, B: 1, SELECT: 2, START: 3, RIGHT: 4, LEFT: 5, UP: 6, DOWN: 7, R: 8, L: 9 };
export const NGP_W = W, NGP_H = H, NGP_FPS = 6144000 / (515 * 199);
const LINE = 515;
const T1 = 256, T4 = 1024, T16 = 4096, T256 = 65536; // passos dos prescalers (em ciclos de CPU)

// valores dos registros internos depois da partida (estado em que a BIOS entrega o controle ao jogo)
const SFR_BOOT = { 0x20: 0x80, 0x22: 0x01, 0x23: 0x90, 0x24: 0x03, 0x25: 0xB0, 0x26: 0x90, 0x27: 0x62, 0x28: 0x05, 0x70: 0x02, 0x71: 0x32, 0xB8: 0xAA, 0xB9: 0xAA };

export { isNgpRom } from './header.js';

export class NGP {
  constructor() {
    this.bios = buildBios();
    this.ram = new Uint8Array(0x4000);       // 4000–7FFF (7000–7FFF também é do Z80)
    this.gfx = new K2GE(this);
    this.apu = new T6W28();
    this.cpu = new TLCS900H(this);
    this.cpu.onBios = () => this.onBios();
    this.z80 = new Z80({
      read: (a) => a < 0x1000 ? this.ram[0x3000 + a] : a === 0x8000 ? this.comm : 0,
      write: (a, v) => {
        if (a < 0x1000) { this.ram[0x3000 + a] = v; return; }
        if (a === 0x8000) this.comm = v;
        else if (a === 0x4001) { this.apu.run(this.cycles); this.apu.writeLeft(v); }
        else if (a === 0x4000) { this.apu.run(this.cycles); this.apu.writeRight(v); }
        else if (a === 0xC000) this.request(6, 0x0C);
      },
      in: () => 0,
      out: () => { this.z80.irqLine = false; },
    });
    this.intPrio = new Uint8Array(0x10);
    this.pending = new Uint8Array(24);
    this.timer = new Uint8Array(4); this.thresh = new Uint8Array(4); this.tclk = new Float64Array(4);
    this.hdma = new Uint8Array(4);
    this.keys = 0; this.frame = 0;
    this.title = ''; this.gameCode = '';
    this.color = true;
    this.dirty = false;
  }

  // ------------------------------------------------------------ cartucho
  loadRom(data) {
    const size = Math.max(0x80000, 1 << Math.ceil(Math.log2(Math.max(data.length, 1))));
    this.rom = new Uint8Array(Math.min(size, 0x400000)).fill(0xFF);
    this.rom.set(data.subarray(0, this.rom.length));
    this.orig = this.rom.slice();
    const h = ngpInfo(this.rom);
    this.start = h.start; this.catalog = h.catalog; this.sub = h.sub; this.color = h.color;
    this.title = h.title; this.gameCode = h.gameCode;
    this.reset();
  }

  chipSize(base) {
    const total = this.rom.length;
    if (base === 0x200000) return Math.min(total, 0x200000);
    return Math.max(0, total - 0x200000);
  }

  reset() {
    const cpu = this.cpu;
    this.gfx.power();
    this.apu.reset();
    this.ram.fill(0);
    this.z80.reset(); this.z80on = false; this.z80time = 0;
    this.comm = 0; this.sc0buf = 0; this.commStatus = 0;
    this.intPrio.fill(0); this.pending.fill(0); this.hdma.fill(0);
    this.timer.fill(0); this.thresh.fill(0); this.tclk.fill(0);
    this.trun = 0; this.t01mod = 0; this.t23mod = 0; this.trdc = 0; this.tffcr = 0;
    this.hint = false; this.lineCycles = 0; this.cycles = 0;
    this.flashCmd = 0; this.flashId = false;
    this.requestReset = false;
    this.irqCheck = true;
    for (const [a, v] of Object.entries(SFR_BOOT)) this.write8(+a, v);
    // área de trabalho da BIOS
    const w8 = (a, v) => { this.ram[a - 0x4000] = v & 0xFF; };
    const w16 = (a, v) => { w8(a, v); w8(a + 1, v >> 8); };
    const w32 = (a, v) => { w16(a, v & 0xFFFF); w16(a + 2, v >>> 16); };
    w32(0x6C00, this.start); w16(0x6C04, this.catalog); w16(0x6E82, this.catalog); w8(0x6C06, this.sub); w8(0x6E84, this.sub);
    for (let i = 0; i < 12; i++) w8(0x6C08 + i, this.rom[0x24 + i]);
    w8(0x6C58, 1); w8(0x6C59, this.rom.length > 0x200000 ? 1 : 0); w8(0x6C55, 1);
    w8(0x6F80, 0xFF); w8(0x6F81, 0x03);                    // bateria cheia
    w8(0x6F84, 0x40); w8(0x6F85, 0); w8(0x6F86, 0);        // partida normal
    w8(0x6F87, 1);                                         // idioma: inglês
    w8(0x6F91, this.color ? 0x10 : 0); w8(0x6F95, this.color ? 0x10 : 0);
    for (let i = 0; i < 0x12; i++) w32(0x6FB8 + i * 4, DEFAULT_IRQ);
    // vídeo como a BIOS deixa
    const g = this.gfx;
    g.write8(0x8000, 0xC0); g.write8(0x8002, 0); g.write8(0x8003, 0); g.write8(0x8004, 0xFF); g.write8(0x8005, 0xFF);
    g.write8(0x8006, 0xC6); g.write8(0x8012, 0); g.write8(0x8118, 0x80);
    g.write8(0x83E0, 0xFF); g.write8(0x83E1, 0x0F); g.write8(0x83F0, 0xFF); g.write8(0x83F1, 0x0F);
    g.write8(0x87E2, this.color ? 0 : 0x80);
    g.latch();
    // CPU
    cpu.reset();
    cpu.pc = this.start & 0xFFFFFF;
    cpu.sr = 0xF800;
    cpu.r[16] = 0xFF23C3; cpu.r[17] = 0xFF23DF; cpu.r[18] = 0x006480; cpu.r[19] = 0x006C00;
    this.frame = 0;
  }

  // ------------------------------------------------------------ barramento da CPU principal
  read8(a) {
    if (a >= 0x200000) {
      if (a < 0x400000) {
        if (this.flashId) return this.flashIdRead(a);
        return this.rom[a - 0x200000] ?? 0xFF;
      }
      if (a >= 0xFF0000) return this.bios[a & 0xFFFF];
      if (a >= 0x800000 && a < 0xA00000) return this.rom[a - 0x800000 + 0x200000] ?? 0xFF;
      return 0;
    }
    if (a >= 0x4000 && a < 0x8000) return this.ram[a - 0x4000];
    if (a >= 0x8000 && a < 0xC000) return this.gfx.read8(a);
    if (a < 0x100) return this.io8(a);
    return 0;
  }
  read16(a) {
    if (a >= 0x200000 && a < 0x3FFFFF && !this.flashId) { const o = a - 0x200000; const r = this.rom; return o + 1 < r.length ? r[o] | (r[o + 1] << 8) : 0xFFFF; }
    if (a >= 0x4000 && a < 0x7FFF) { const o = a - 0x4000; return this.ram[o] | (this.ram[o + 1] << 8); }
    return this.read8(a) | (this.read8((a + 1) & 0xFFFFFF) << 8);
  }
  read32(a) { return (this.read16(a) | (this.read16((a + 2) & 0xFFFFFF) << 16)) >>> 0; }

  write8(a, v) {
    if (a >= 0x4000 && a < 0x8000) { this.ram[a - 0x4000] = v; return; }
    if (a >= 0x8000 && a < 0xC000) { this.gfx.write8(a, v); return; }
    if (a < 0x100) { this.ioWrite8(a, v); return; }
    if ((a >= 0x200000 && a < 0x400000) || (a >= 0x800000 && a < 0xA00000)) this.flashWrite(a, v);
  }
  write16(a, v) {
    if (a >= 0x4000 && a < 0x7FFF) { const o = a - 0x4000; this.ram[o] = v & 0xFF; this.ram[o + 1] = v >> 8; return; }
    if (a === 0xB8) { // controle conjunto do som (byte baixo) e do Z80 (byte alto)
      this.ioWrite8(0xB8, v & 0xFF); this.ioWrite8(0xB9, v >> 8); return;
    }
    this.write8(a, v & 0xFF); this.write8((a + 1) & 0xFFFFFF, (v >> 8) & 0xFF);
  }
  write32(a, v) { this.write16(a, v & 0xFFFF); this.write16((a + 2) & 0xFFFFFF, v >>> 16); }

  // ------------------------------------------------------------ registros internos (0000–00FF)
  io8(a) {
    if (a >= 0x70 && a <= 0x7F) {
      switch (a) {
        case 0x71: return (this.pending[5] ? 0x08 : 0) | (this.pending[6] ? 0x80 : 0);
        case 0x73: return (this.pending[7] ? 0x08 : 0) | (this.pending[8] ? 0x80 : 0);
        case 0x74: return (this.pending[9] ? 0x08 : 0) | (this.pending[10] ? 0x80 : 0);
        case 0x77: return (this.pending[11] ? 0x08 : 0) | (this.pending[12] ? 0x80 : 0);
        default: return 0;
      }
    }
    if (a >= 0x90 && a <= 0x97) return this.rtc(a);
    if (a >= 0x20 && a <= 0x29) return a === 0x20 ? this.trun : a === 0x29 ? this.trdc : 0x04;
    switch (a) {
      case 0x50: return this.sc0buf;
      case 0xB0: return this.keys & 0x7F;
      case 0xB1: return 0x02; // sem pedido de desligar
      case 0xBC: return this.comm;
      default: return 0;
    }
  }

  ioWrite8(a, v) {
    if (a >= 0x70 && a <= 0x7F) {
      switch (a) {
        case 0x71: if (!(v & 0x08)) this.pending[5] = 0; if (!(v & 0x80)) this.pending[6] = 0; break;
        case 0x73: if (!(v & 0x08)) this.pending[7] = 0; if (!(v & 0x80)) this.pending[8] = 0; break;
        case 0x74: if (!(v & 0x08)) this.pending[9] = 0; if (!(v & 0x80)) this.pending[10] = 0; break;
        case 0x77: if (!(v & 0x08)) this.pending[11] = 0; if (!(v & 0x80)) this.pending[12] = 0; break;
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: this.hdma[a - 0x7C] = v; break;
      }
      if (a <= 0x7A) { this.intPrio[a - 0x70] = v; this.irqCheck = true; }
      return;
    }
    if (a >= 0x20 && a <= 0x29) {
      switch (a) {
        case 0x20: this.trun = v; for (let t = 0; t < 4; t++) if (!(v & (1 << t))) this.timer[t] = 0; break;
        case 0x22: this.thresh[0] = v; break;
        case 0x23: this.thresh[1] = v; break;
        case 0x24: this.t01mod = v; break;
        case 0x25: this.tffcr = v & 0x33; break;
        case 0x26: this.thresh[2] = v; break;
        case 0x27: this.thresh[3] = v; break;
        case 0x28: this.t23mod = v; break;
        case 0x29: this.trdc = v & 3; break;
      }
      return;
    }
    if (a >= 0xA0 && a <= 0xA3) {
      this.apu.run(this.cycles);
      if (a === 0xA1 && !this.z80on) this.apu.writeLeft(v);
      else if (a === 0xA0 && !this.z80on) this.apu.writeRight(v);
      else if (a === 0xA2) this.apu.dacL = v;
      else if (a === 0xA3) this.apu.dacR = v;
      return;
    }
    switch (a) {
      case 0x50: this.sc0buf = v; break;
      case 0xB2: this.commStatus = v & 1; break;
      case 0xB8: if (v === 0x55) { this.apu.run(this.cycles); this.apu.setEnabled(true); } else if (v === 0xAA) { this.apu.run(this.cycles); this.apu.setEnabled(false); } break;
      case 0xB9: if (v === 0x55) this.z80on = true; else if (v === 0xAA) { this.z80on = false; this.z80.reset(); } break;
      case 0xBA: this.z80.nmiPending = true; break;
      case 0xBC: this.comm = v; break;
      default: break;
    }
  }

  rtc(a) {
    if (a === 0x90) return 0;
    if (a === 0x91 || !this.rtcLatch) {
      const d = new Date(), bcd = (n) => (((n / 10) | 0) << 4) | (n % 10);
      const y = bcd(d.getFullYear() % 100);
      this.rtcLatch = [y, bcd(d.getMonth() + 1), bcd(d.getDate()), bcd(d.getHours()), bcd(d.getMinutes()), bcd(d.getSeconds()), ((y % 4) << 4) | d.getDay()];
    }
    return this.rtcLatch[a - 0x91];
  }

  // ------------------------------------------------------------ flash do cartucho (save)
  flashWrite(a, v) {
    const base = a >= 0x800000 ? 0x800000 : 0x200000, off = a - base, lo = off & 0x7FFF;
    const st = this.flashCmd;
    if (st === 0 && lo === 0x5555 && v === 0xAA) { this.flashCmd = 1; return; }
    if (st === 1 && lo === 0x2AAA && v === 0x55) { this.flashCmd = 2; return; }
    if (st === 2 && lo === 0x5555) {
      if (v === 0xA0) { this.flashCmd = 3; return; }
      if (v === 0x80) { this.flashCmd = 4; return; }
      if (v === 0x90) { this.flashId = true; this.flashCmd = 0; return; }
      if (v === 0xF0) { this.flashId = false; this.flashCmd = 0; return; }
    }
    if (st === 3) { this.flashProgram(a, v); this.flashCmd = 0; return; }
    if (st === 4 && lo === 0x5555 && v === 0xAA) { this.flashCmd = 5; return; }
    if (st === 5 && lo === 0x2AAA && v === 0x55) { this.flashCmd = 6; return; }
    if (st === 6) {
      if (v === 0x10) this.flashErase(base, 0, this.chipSize(base));
      else if (v === 0x30) {
        for (const [s, n] of (this.blocksOf ||= {})[base] || (this.blocksOf[base] = flashBlocksFor(this.chipSize(base)))) if (off >= s && off < s + n) { this.flashErase(base, s, n); break; }
      }
      this.flashCmd = 0; return;
    }
    if (v === 0xF0) this.flashId = false;
    this.flashCmd = 0;
  }
  flashIdRead(a) {
    const lo = a & 0xFF;
    if (lo === 0) return 0x98; // fabricante (Toshiba)
    if (lo === 1) { const n = this.chipSize(0x200000); return n > 0x100000 ? 0x2F : n > 0x80000 ? 0x2C : 0xAB; }
    if (lo === 2) return 0x02;
    return 0x80;
  }
  flashProgram(a, v) {
    const o = a >= 0x800000 ? a - 0x800000 + 0x200000 : a - 0x200000;
    if (o < 0 || o >= this.rom.length) return;
    const nv = this.rom[o] & v;
    if (nv !== this.rom[o]) { this.rom[o] = nv; this.dirty = true; }
  }
  flashErase(base, off, len) {
    const o = (base === 0x800000 ? 0x200000 : 0) + off;
    const end = Math.min(o + len, this.rom.length);
    for (let i = o; i < end; i++) if (this.rom[i] !== 0xFF) { this.rom[i] = 0xFF; this.dirty = true; }
  }

  // ------------------------------------------------------------ BIOS própria
  onBios() {
    const cpu = this.cpu, at = (cpu.pc - 1) & 0xFFFFFF;
    if (at === SWI1_STUB) {
      const n = cpu.getB(0x31) & 0x1F;
      if (biosCall(this, n) === 'stay') { cpu.pc = at; cpu.halted = true; return; }
      cpu.sr = cpu.pop(2); cpu.pc = cpu.pop(4) & 0xFFFFFF; this.irqCheck = true; // volta como RETI
      cpu.cycles = 16; return;
    }
    if (at >= STUB_BASE && at < STUB_BASE + 0x1B * 16 && !(at & 15)) {
      const r = biosCall(this, (at - STUB_BASE) >> 4);
      if (r === 'stay') { cpu.pc = at; cpu.halted = true; return; }
      cpu.pc = cpu.pop(4) & 0xFFFFFF;
      cpu.cycles = 8; return;
    }
    // 0x1F fora da BIOS: instrução inválida, segue adiante
  }

  // ------------------------------------------------------------ interrupções e DMA
  // fonte (índice do vetor na RAM) ou canal de DMA programado para esse vetor do hardware
  request(index, vec) {
    for (let ch = 0; ch < 4; ch++) if (this.hdma[ch] === vec) { this.dma(ch); return; }
    this.pending[index] = 1;
    this.irqCheck = true;
  }

  checkIrq() {
    this.irqCheck = false;
    const cpu = this.cpu;
    let iff = (cpu.sr >> 12) & 7; if (iff === 1) iff = 0;
    const P = this.intPrio, pend = this.pending;
    // ordem fixa: VBL (INT4), Z80 (INT5), timers 0–3, serial
    const src = [[5, P[1] & 7], [6, (P[1] >> 4) & 7], [7, P[3] & 7], [8, (P[3] >> 4) & 7], [9, P[4] & 7], [10, (P[4] >> 4) & 7], [11, P[7] & 7], [12, (P[7] >> 4) & 7]];
    for (const [i, pr] of src) {
      if (pend[i] && pr && pr !== 7 && iff <= pr) { pend[i] = 0; this.interrupt(i, pr); return; }
    }
  }

  interrupt(index, level) {
    const cpu = this.cpu;
    cpu.halted = false;
    cpu.push(4, cpu.pc); cpu.push(2, cpu.sr);
    cpu.sr = (cpu.sr & 0x8FFF) | (Math.min(level + 1, 7) << 12);
    cpu.pc = this.read32(0x6FB8 + index * 4) & 0xFFFFFF;
  }

  dma(ch) {
    const cpu = this.cpu;
    if (!cpu.dmaC[ch]) return;
    const m = cpu.dmaM[ch], mode = (m >> 2) & 7, sz = [1, 2, 4, 4][m & 3];
    const s = cpu.dmaS[ch], d = cpu.dmaD[ch];
    const rd = (a) => sz === 1 ? this.read8(a & 0xFFFFFF) : sz === 2 ? this.read16(a & 0xFFFFFF) : this.read32(a & 0xFFFFFF);
    const wr = (a, v) => sz === 1 ? this.write8(a & 0xFFFFFF, v) : sz === 2 ? this.write16(a & 0xFFFFFF, v) : this.write32(a & 0xFFFFFF, v);
    switch (mode) {
      case 0: wr(d, rd(s)); cpu.dmaD[ch] = d + sz; break;
      case 1: wr(d, rd(s)); cpu.dmaD[ch] = d - sz; break;
      case 2: wr(d, rd(s)); cpu.dmaS[ch] = s + sz; break;
      case 3: wr(d, rd(s)); cpu.dmaS[ch] = s - sz; break;
      case 4: wr(d, rd(s)); break;
      case 5: cpu.dmaS[ch] = s + 1; break;
      default: break;
    }
    cpu.dmaC[ch] = (cpu.dmaC[ch] - 1) & 0xFFFF;
    if (!cpu.dmaC[ch]) { this.interrupt(14 + ch, 7); this.hdma[ch] = 0; }
  }

  // ------------------------------------------------------------ timers
  timers(n) {
    this.tclk[0] += n; this.tclk[1] += n; this.tclk[2] += n; this.tclk[3] += n;
    const tr = this.trun;
    if (!(tr & 0x0F)) return;
    const step = (t, rate) => { let c = 0; while (this.tclk[t] >= rate) { c++; this.tclk[t] -= rate; } return c; };
    let t0 = false, t2 = false;
    if (tr & 1) {
      let inc = 0;
      switch (this.t01mod & 3) {
        case 0: if (this.hint) { inc = 1; this.tclk[0] = 0; this.hint = false; } break;
        case 1: inc = step(0, T1); break;
        case 2: inc = step(0, T4); break;
        default: inc = step(0, T16); break;
      }
      if (inc) { this.timer[0] = (this.timer[0] + inc) & 0xFF; if (this.thresh[0] && this.timer[0] >= this.thresh[0]) { this.timer[0] = 0; t0 = true; this.request(7, 0x10); } }
    }
    if (tr & 2) {
      let inc = 0;
      switch ((this.t01mod >> 2) & 3) {
        case 0: if (t0) { inc = 1; this.tclk[1] = 0; } break;
        case 1: inc = step(1, T1); break;
        case 2: inc = step(1, T16); break;
        default: inc = step(1, T256); break;
      }
      if (inc) { this.timer[1] = (this.timer[1] + inc) & 0xFF; if (this.thresh[1] && this.timer[1] >= this.thresh[1]) { this.timer[1] = 0; this.request(8, 0x11); } }
    }
    if (tr & 4) {
      let inc = 0;
      switch (this.t23mod & 3) {
        case 0: break;
        case 1: inc = step(2, T1 / 2); break;
        case 2: inc = step(2, T4); break;
        default: inc = step(2, T16); break;
      }
      if (inc) { this.timer[2] = (this.timer[2] + inc) & 0xFF; if (this.thresh[2] && this.timer[2] >= this.thresh[2]) { this.timer[2] = 0; t2 = true; this.request(9, 0x12); } }
    }
    if (tr & 8) {
      let inc = 0;
      switch ((this.t23mod >> 2) & 3) {
        case 0: if (t2) { inc = 1; this.tclk[3] = 0; } break;
        case 1: inc = step(3, T1); break;
        case 2: inc = step(3, T16); break;
        default: inc = step(3, T256); break;
      }
      if (inc) { this.timer[3] = (this.timer[3] + inc) & 0xFF; if (this.thresh[3] && this.timer[3] >= this.thresh[3]) { this.timer[3] = 0; this.z80.irqLine = true; this.request(10, 0x13); } }
    }
  }

  // ------------------------------------------------------------ quadro
  setKeys(mask) {
    // ordem do NGP: cima, baixo, esquerda, direita, A, B, Option
    let k = 0;
    if (mask & (1 << KEY.UP)) k |= 0x01;
    if (mask & (1 << KEY.DOWN)) k |= 0x02;
    if (mask & (1 << KEY.LEFT)) k |= 0x04;
    if (mask & (1 << KEY.RIGHT)) k |= 0x08;
    if (mask & (1 << KEY.A)) k |= 0x10;
    if (mask & (1 << KEY.B)) k |= 0x20;
    if (mask & ((1 << KEY.START) | (1 << KEY.SELECT))) k |= 0x40;
    this.keys = k;
  }

  runFrame() {
    if (this.requestReset) this.reset();
    const cpu = this.cpu, gfx = this.gfx;
    this.ram[0x6F82 - 0x4000] = this.keys;
    let done = false, guard = 0;
    while (!done && guard++ < 2000000) {
      if (this.irqCheck || cpu.halted) this.checkIrq();
      // a cada instrução: interrupções pendentes podem ter mudado com EI/RETI/POP SR
      const sr = cpu.sr;
      const n = cpu.step() & 0xFF || 2;
      if (cpu.sr !== sr) this.irqCheck = true;
      this.cycles += n;
      this.lineCycles += n;
      if (this.lineCycles >= LINE) {
        this.lineCycles -= LINE;
        this.hint = gfx.hblank();
        if (gfx.endLine()) { done = true; if (gfx.ctlInt & 0x80) this.request(5, 0x0B); }
      }
      this.timers(n);
      if (this.z80on) {
        this.z80time += n;
        while (this.z80time > 0) this.z80time -= this.z80.step() * 2;
      }
    }
    this.apu.run(this.cycles);
    this.frame++;
  }

  // ------------------------------------------------------------ save (flash) no formato .ngf
  hasSave() { return true; }
  exportSave() {
    this.dirty = false;
    const blocks = [];
    const r = this.rom, o = this.orig;
    for (let p = 0; p < r.length; p += 256) {
      let diff = false;
      for (let i = p; i < p + 256; i++) if (r[i] !== o[i]) { diff = true; break; }
      if (!diff) continue;
      const last = blocks[blocks.length - 1];
      if (last && last[0] + last[1] === p) last[1] += 256; else blocks.push([p, 256]);
    }
    let total = 8;
    for (const [, n] of blocks) total += 6 + n;
    const out = new Uint8Array(total), dv = new DataView(out.buffer);
    dv.setUint16(0, 0x0053, true); dv.setUint16(2, blocks.length, true); dv.setUint32(4, total, true);
    let q = 8;
    for (const [p, n] of blocks) {
      const addr = p < 0x200000 ? 0x200000 + p : 0x800000 + (p - 0x200000);
      dv.setUint32(q, addr, true); dv.setUint16(q + 4, n, true);
      out.set(r.subarray(p, p + n), q + 6);
      q += 6 + n;
    }
    return out;
  }
  importSave(b) {
    if (!b || b.length < 8) return;
    const dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    if (dv.getUint16(0, true) !== 0x0053) return;
    const count = dv.getUint16(2, true);
    let q = 8;
    for (let k = 0; k < count && q + 6 <= b.length; k++) {
      const addr = dv.getUint32(q, true), n = dv.getUint16(q + 4, true);
      const p = addr >= 0x800000 ? addr - 0x800000 + 0x200000 : addr - 0x200000;
      if (p >= 0 && p + n <= this.rom.length && q + 6 + n <= b.length) this.rom.set(b.subarray(q + 6, q + 6 + n), p);
      q += 6 + n;
    }
  }

  // ------------------------------------------------------------ estados salvos
  saveState() {
    const cpu = this.cpu;
    return { v: 1, sys: 'ngp',
      cpu: { r: Array.from(cpu.r), pc: cpu.pc, sr: cpu.sr, fd: cpu.fd, intNest: cpu.intNest, halted: +cpu.halted,
        dmaS: Array.from(cpu.dmaS), dmaD: Array.from(cpu.dmaD), dmaC: Array.from(cpu.dmaC), dmaM: Array.from(cpu.dmaM) },
      z80: this.z80.state(), gfx: this.gfx.state(), apu: this.apu.state(), ram: this.ram.slice(), flash: this.exportSave(),
      s: [this.z80on, this.z80time, this.comm, this.sc0buf, this.commStatus, this.trun, this.t01mod, this.t23mod, this.trdc, this.tffcr, +this.hint, this.lineCycles, this.flashCmd, +this.flashId],
      intPrio: Array.from(this.intPrio), pending: Array.from(this.pending), hdma: Array.from(this.hdma), timer: Array.from(this.timer), thresh: Array.from(this.thresh), tclk: Array.from(this.tclk) };
  }
  loadState(st) {
    if (!st || st.v !== 1 || st.sys !== 'ngp') throw new Error('Estado salvo incompatível com este jogo.');
    const cpu = this.cpu, c = st.cpu;
    cpu.r.set(c.r); cpu.pc = c.pc; cpu.sr = c.sr; cpu.fd = c.fd; cpu.intNest = c.intNest; cpu.halted = !!c.halted;
    cpu.dmaS.set(c.dmaS); cpu.dmaD.set(c.dmaD); cpu.dmaC.set(c.dmaC); cpu.dmaM.set(c.dmaM);
    this.z80.restore(st.z80); this.gfx.restore(st.gfx); this.apu.restore(st.apu); this.ram.set(st.ram);
    this.rom.set(this.orig); this.importSave(st.flash); this.dirty = false;
    let h, id, on;
    [on, this.z80time, this.comm, this.sc0buf, this.commStatus, this.trun, this.t01mod, this.t23mod, this.trdc, this.tffcr, h, this.lineCycles, this.flashCmd, id] = st.s;
    this.z80on = !!on; this.hint = !!h; this.flashId = !!id;
    this.intPrio.set(st.intPrio); this.pending.set(st.pending); this.hdma.set(st.hdma); this.timer.set(st.timer); this.thresh.set(st.thresh); this.tclk.set(st.tclk);
    this.irqCheck = true;
  }

  get ppu() { return this.gfx; }
}

function flashBlocksFor(size) {
  const out = [];
  let a = 0;
  while (a < size - 0x10000) { out.push([a, 0x10000]); a += 0x10000; }
  out.push([a, 0x8000]); out.push([a + 0x8000, 0x2000]); out.push([a + 0xA000, 0x2000]); out.push([a + 0xC000, 0x4000]);
  return out;
}
