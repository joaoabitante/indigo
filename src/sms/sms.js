// Núcleo Master System / Game Gear: Z80 a 3,58 MHz, 262 linhas × 228 ciclos (NTSC), mapeadores Sega e
// Codemasters, RAM do cartucho (save), portas de E/S (controle, VDP, som, portas extras do Game Gear).

import { Z80 } from './z80.js';
import { VDP } from './vdp.js';
import { PSG } from './psg.js';
import { smsNormalize, smsCode } from './header.js';

export const KEY = { A: 0, B: 1, SELECT: 2, START: 3, RIGHT: 4, LEFT: 5, UP: 6, DOWN: 7, R: 8, L: 9 };
const LINES = 262, CYCLES_PER_LINE = 228;

export { smsKind } from './header.js';

export class SMS {
  constructor(gg) {
    this.gg = !!gg;
    this.cpu = new Z80(this);
    this.vdp = new VDP(this.gg);
    this.apu = new PSG();
    this.ram = new Uint8Array(0x2000);
    this.cartRam = new Uint8Array(0x8000);
    this.keys = 0;
    this.frame = 0;
    this.title = ''; this.gameCode = '';
    this.romLoaded = false;
  }

  get ppu() { return this.vdp; }

  loadRom(data) {
    this.rom = smsNormalize(data);
    this.banks = this.rom.length >> 14;
    const size = this.rom.length;
    this.gameCode = smsCode(this.rom);
    const cs = this.rom[0x7FE6] | (this.rom[0x7FE7] << 8), inv = this.rom[0x7FE8] | (this.rom[0x7FE9] << 8);
    this.mapper = size >= 0x8000 && cs && ((cs + inv) & 0xFFFF) === 0 ? 'codemasters' : 'sega';
    this.title = '';
    this.romLoaded = true;
    this.reset();
  }

  reset() {
    this.cpu.reset();
    this.vdp.reset();
    this.apu.reset();
    this.ram.fill(0);
    this.slot = [0, 1, 2];
    this.ramCtl = 0;        // FFFC
    this.cartRamUsed = false; this.dirty = false;
    this.ioCtl = 0xFF; this.memCtl = 0;
    this.cycles = 0;        // ciclos totais (para o som)
    this.over = 0;
    this.pausePrev = false;
    this.frame = 0;
    if (this.mapper === 'codemasters') this.slot = [0, 1, 0];
  }

  // ------------------------------------------------------------ memória
  read(a) {
    if (a < 0x400) return this.rom[a];
    if (a < 0x4000) return this.rom[((this.slot[0] % this.banks) << 14) | a];
    if (a < 0x8000) return this.rom[((this.slot[1] % this.banks) << 14) | (a & 0x3FFF)];
    if (a < 0xC000) {
      if (this.ramCtl & 0x08) return this.cartRam[((this.ramCtl & 4) << 12) | (a & 0x3FFF)];
      return this.rom[((this.slot[2] % this.banks) << 14) | (a & 0x3FFF)];
    }
    return this.ram[a & 0x1FFF];
  }

  write(a, v) {
    if (a >= 0xC000) {
      this.ram[a & 0x1FFF] = v;
      if (a >= 0xFFFC && this.mapper === 'sega') {
        switch (a) {
          case 0xFFFC: this.ramCtl = v; if (v & 0x08) this.cartRamUsed = true; break;
          case 0xFFFD: this.slot[0] = v; break;
          case 0xFFFE: this.slot[1] = v; break;
          case 0xFFFF: this.slot[2] = v; break;
        }
      }
      return;
    }
    if (a >= 0x8000 && (this.ramCtl & 0x08)) {
      this.cartRam[((this.ramCtl & 4) << 12) | (a & 0x3FFF)] = v;
      this.dirty = true;
      return;
    }
    if (this.mapper === 'codemasters') {
      if (a === 0x0000) this.slot[0] = v;
      else if (a === 0x4000) this.slot[1] = v;
      else if (a === 0x8000) this.slot[2] = v;
    }
  }

  // ------------------------------------------------------------ portas
  in(port) {
    const p = port & 0xFF;
    if (this.gg && p <= 0x06) {
      if (p === 0x00) return ((this.keys & (1 << KEY.START)) ? 0 : 0x80) | 0x40; // Start + região exportação
      if (p === 0x01 || p === 0x05) return 0x00;
      if (p === 0x02) return 0x7F;
      if (p === 0x03) return 0x00;
      if (p === 0x04) return 0xFF;
      return 0x00;
    }
    switch (p & 0xC1) {
      case 0x00: case 0x01: return 0xFF;
      case 0x40: this.syncVcount(); return this.vdp.readV();
      case 0x41: return this.vdp.hcount;
      case 0x80: return this.vdp.readData();
      case 0x81: { const v = this.vdp.readStatus(); this.cpu.irqLine = !!this.vdp.irq(); return v; }
      case 0xC0: { // controle 1
        const k = this.keys;
        let v = 0xFF;
        if (k & (1 << KEY.UP)) v &= ~0x01;
        if (k & (1 << KEY.DOWN)) v &= ~0x02;
        if (k & (1 << KEY.LEFT)) v &= ~0x04;
        if (k & (1 << KEY.RIGHT)) v &= ~0x08;
        if (k & (1 << KEY.B)) v &= ~0x10;   // botão 1
        if (k & (1 << KEY.A)) v &= ~0x20;   // botão 2
        return v;
      }
      default: { // controle 2 + reset + linhas TH (console de exportação: TH lê de volta o que foi escrito)
        if (this.gg) return 0xFF;
        const io = this.ioCtl;
        const thA = (io & 0x02) ? 1 : (io >> 5) & 1;
        const thB = (io & 0x08) ? 1 : (io >> 7) & 1;
        return 0x3F | (thA << 6) | (thB << 7);
      }
    }
  }

  out(port, v) {
    const p = port & 0xFF;
    if (this.gg && p <= 0x06) {
      if (p === 0x06) { this.apu.run(this.cycles + this.cpuCyclesInLine); this.apu.stereo = v; }
      return;
    }
    switch (p & 0xC1) {
      case 0x00: this.memCtl = v; return;
      case 0x01: this.ioCtl = v; this.vdp.hcount = 0x80; return;
      case 0x40: case 0x41: this.apu.run(this.cycles + this.cpuCyclesInLine); this.apu.write(v); return;
      case 0x80: this.vdp.writeData(v); return;
      case 0x81: this.vdp.writeControl(v); this.cpu.irqLine = !!this.vdp.irq(); return;
      default: return; // som FM (Japão) não emulado
    }
  }

  syncVcount() { /* o contador vertical é atualizado a cada linha */ }

  setKeys(mask) {
    // Master System: Start é o botão de pausa do console (NMI na borda)
    const pause = !this.gg && (mask & (1 << KEY.START)) !== 0;
    if (pause && !this.pausePrev) this.cpu.nmiPending = true;
    this.pausePrev = pause;
    this.keys = mask;
  }

  // ------------------------------------------------------------ quadro
  runFrame() {
    const cpu = this.cpu, vdp = this.vdp;
    for (let line = 0; line < LINES; line++) {
      vdp.lineStart(line);
      if (line < 192) vdp.renderLine(line);
      cpu.irqLine = !!vdp.irq();
      let c = this.over;
      this.cpuCyclesInLine = c;
      while (c < CYCLES_PER_LINE) {
        c += cpu.step();
        this.cpuCyclesInLine = c;
      }
      this.over = c - CYCLES_PER_LINE;
      this.cycles += CYCLES_PER_LINE;
      this.cpuCyclesInLine = 0;
    }
    this.apu.run(this.cycles);
    this.frame++;
  }

  // ------------------------------------------------------------ save (RAM do cartucho)
  hasSave() { return this.cartRamUsed; }
  exportSave() { this.dirty = false; return this.cartRam.slice(); }
  importSave(b) { if (b && b.length) { this.cartRam.set(b.subarray(0, this.cartRam.length)); this.cartRamUsed = true; } }

  saveState() {
    return { v: 1, gg: this.gg, cpu: this.cpu.state(), vdp: this.vdp.state(), psg: this.apu.state(), ram: this.ram.slice(), cartRam: this.cartRam.slice(),
      s: [this.slot[0], this.slot[1], this.slot[2], this.ramCtl, this.ioCtl, this.memCtl, this.over] };
  }
  loadState(st) {
    if (!st || st.v !== 1 || st.gg !== this.gg) throw new Error('Estado salvo incompatível com este jogo.');
    this.cpu.restore(st.cpu); this.vdp.restore(st.vdp); this.apu.restore(st.psg);
    this.ram.set(st.ram); this.cartRam.set(st.cartRam);
    [this.slot[0], this.slot[1], this.slot[2], this.ramCtl, this.ioCtl, this.memCtl, this.over] = st.s;
  }
}
