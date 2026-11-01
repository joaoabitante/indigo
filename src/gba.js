// Núcleo do Game Boy Advance: liga CPU, memória, vídeo, som e o escalonador por scanline

import { CPU } from './cpu.js';
import { Bus } from './bus.js';
import { IO, IRQ_VBLANK, IRQ_HBLANK, IRQ_VCOUNT } from './io.js';
import { PPU } from './ppu.js';
import { APU } from './apu.js';
import { HleBios, installHleBios } from './bios.js';
import { Save, detectSaveType } from './save.js';
import { CartRtc, hasRtc } from './gpio.js';

export const KEY = { A: 0, B: 1, SELECT: 2, START: 3, RIGHT: 4, LEFT: 5, UP: 6, DOWN: 7, R: 8, L: 9 };

const HDRAW = 960, HBLANK = 272, LINES = 228;

export class GBA {
  constructor() {
    this.bus = new Bus(this);
    this.io = new IO(this);
    this.cpu = new CPU(this.bus);
    this.ppu = new PPU(this);
    this.apu = new APU(this);
    this.hle = new HleBios(this);
    this.frameCycles = 0;     // ciclos acumulados no quadro atual (base de tempo)
    this.totalBase = 0;       // ciclos totais até o início do trecho corrente
    this.chunkCycles = 0;     // ciclos executados no trecho corrente
    this.frame = 0;
    this.romLoaded = false;
    this.realBios = false;
    this.title = '';
    this.gameCode = '';
    installHleBios(this.bus);
  }

  loadBios(data) {
    this.bus.bios.fill(0);
    this.bus.bios.set(data.subarray(0, 0x4000));
    this.realBios = true;
  }

  loadRom(data, saveBytes) {
    this.bus.loadRom(data);
    const type = detectSaveType(data);
    this.bus.save = new Save(type);
    if (saveBytes) this.bus.save.load(saveBytes);
    let t = '';
    for (let k = 0xA0; k < 0xAC; k++) { const c = data[k]; if (c) t += String.fromCharCode(c); }
    this.title = t.trim();
    let g = '';
    for (let k = 0xAC; k < 0xB0; k++) g += String.fromCharCode(data[k] || 32);
    this.gameCode = g;
    this.bus.gpio = hasRtc(data, g) ? new CartRtc() : null;
    this.romLoaded = true;
    this.reset();
  }

  reset() {
    this.io.reset();
    this.apu.reset();
    this.bus.ewram.fill(0); this.bus.iwram.fill(0);
    this.bus.pal.fill(0); this.bus.vram.fill(0); this.bus.oam.fill(0);
    this.bus.setWaitcnt(0);
    if (!this.realBios) installHleBios(this.bus);
    this.cpu.reset(!this.realBios);
    this.cpu.swiHandler = this.realBios ? null : (n, addr) => this.hle.handle(n, addr);
    if (!this.realBios) {
      this.io.regs[0] = 0x0080;
      this.io.write16(0, 0);
    }
    this.frameCycles = 0; this.totalBase = 0; this.chunkCycles = 0;
    this.frame = 0;
    this.ppu.vblankLatch();
  }

  // relógio global em ciclos (usado pelo som)
  now() { return this.totalBase + this.chunkCycles + this.bus.cycles; }

  setKeys(pressedMask) { this.io.setKeys((~pressedMask) & 0x3FF); }

  run(cycles) {
    const cpu = this.cpu, bus = this.bus, io = this.io;
    let done = 0;
    this.chunkCycles = 0;
    while (done < cycles) {
      if (io.ime && (io.ie & io.if & 0x3FFF) && !cpu.i) { cpu.irq(); }
      if (cpu.halted) {
        if (io.ie & io.if & 0x3FFF) { cpu.halted = false; continue; }
        let skip = cycles - done;
        if (io.timersActive) {
          const t = io.cyclesToTimerEvent();
          if (t > 0 && t < skip) skip = t;
          if (skip < 1) skip = 1;
          io.tickTimers(skip);
        }
        done += skip; this.chunkCycles = done;
        continue;
      }
      bus.cycles = 0; io.stepTicked = 0;
      const extra = cpu.step();
      const c = bus.cycles + extra;
      if (io.timersActive) io.tickTimers(c - io.stepTicked);
      bus.cycles = 0; io.stepTicked = 0;
      done += c; this.chunkCycles = done;
    }
    this.totalBase += done;
    this.chunkCycles = 0;
    return done - cycles; // excedente
  }

  runFrame() {
    const io = this.io, ppu = this.ppu, bus = this.bus;
    let over = 0;
    for (let line = 0; line < LINES; line++) {
      io.vcount = line;
      let ds = io.dispstat & ~0x7;
      if (line >= 160 && line < 227) ds |= 1;
      const lyc = io.dispstat >>> 8;
      if (line === lyc) { ds |= 4; if (io.dispstat & 0x20) io.requestIrq(IRQ_VCOUNT); }
      io.dispstat = ds;
      if (line === 160) {
        if (ds & 0x8) io.requestIrq(IRQ_VBLANK);
        bus.cycles = 0; io.stepTicked = 0;
        io.triggerDma(1);
        if (io.timersActive) io.tickTimers(bus.cycles - io.stepTicked);
        over += bus.cycles; bus.cycles = 0; io.stepTicked = 0;
        ppu.vblankLatch();
      }
      over = this.run(HDRAW - over);
      io.dispstat |= 2;
      if (line < 160) {
        ppu.renderLine(line);
        bus.cycles = 0; io.stepTicked = 0;
        io.triggerDma(2);
        if (io.timersActive) io.tickTimers(bus.cycles - io.stepTicked);
        over += bus.cycles; bus.cycles = 0; io.stepTicked = 0;
      }
      if (io.dispstat & 0x10) io.requestIrq(IRQ_HBLANK);
      over = this.run(HBLANK - over);
      io.dispstat &= ~2;
    }
    this.apu.catchUp();
    this.frame++;
  }
}
