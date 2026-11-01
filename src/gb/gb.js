// Núcleo do Game Boy / Game Boy Color: CPU SM83 com tempo por ciclo de máquina (cada acesso à memória
// avança vídeo, som, timer e DMA), memória, timer, DMA de OAM, HDMA do GBC, troca de velocidade e serial.

import { PPU } from './ppu.js';
import { APU } from './apu.js';
import { Cart } from './cart.js';

export const KEY = { A: 0, B: 1, SELECT: 2, START: 3, RIGHT: 4, LEFT: 5, UP: 6, DOWN: 7, R: 8, L: 9 };

const FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10;
const DMA_DELAY = 1; // 1 ciclo de partida antes do primeiro byte
const TIMER_BITS = [0x200, 0x8, 0x20, 0x80];
export const FRAME_DOTS = 70224;

export class GB {
  constructor() {
    this.ppu = new PPU(this);
    this.dotClock = 0;
    this.apu = new APU(this);
    this.wram = new Uint8Array(0x8000);
    this.hram = new Uint8Array(0x7F);
    this.cart = null;
    this.cgb = false;
    this.title = ''; this.gameCode = '';
    this.frame = 0;
    this.keys = 0;
    this.serialOut = '';
    this.romLoaded = false;
  }

  // mode: 'auto' (GBC quando o cartucho suporta) | 'dmg' | 'cgb'
  loadRom(data, saveBytes, mode = 'auto') {
    this.cart = new Cart(data);
    const info = this.cart.info;
    this.cgb = mode === 'cgb' ? true : mode === 'dmg' ? false : info.cgb;
    if (info.cgbOnly) this.cgb = true;
    this.title = info.title;
    this.gameCode = info.code;
    if (saveBytes) this.cart.importSave(saveBytes);
    this.romLoaded = true;
    this.reset();
  }

  get save() { return this.cart; }

  reset() {
    this.wram.fill(0); this.hram.fill(0);
    this.ppu.reset();
    this.apu.reset();
    // registradores como o boot ROM deixa
    if (this.cgb) { this.a = 0x11; this.f = 0x80; this.b = 0x00; this.c = 0x00; this.d = 0xFF; this.e = 0x56; this.h = 0x00; this.l = 0x0D; }
    else { this.a = 0x01; this.f = 0xB0; this.b = 0x00; this.c = 0x13; this.d = 0x00; this.e = 0xD8; this.h = 0x01; this.l = 0x4D; }
    this.sp = 0xFFFE; this.pc = 0x0100;
    this.ime = false; this.eiDelay = false; this.halted = false; this.haltBug = false; this.stopped = false; this.locked = false;
    this.ie = 0; this.if_ = 0x01;
    this.div = this.cgb ? 0x1EA0 : 0xABCC;
    this.tima = 0; this.tma = 0; this.tac = 0xF8; this.timaPending = false; this.timaReloaded = false;
    this.p1 = 0x30;
    this.sb = 0; this.sc = 0x7E; this.serialTimer = 0;
    this.dmaActive = false; this.dmaBlock = false; this.dmaDelay = 0; this.dmaSrc = 0; this.dmaIdx = 0; this.dmaReg = 0xFF;
    this.svbk = 1; this.key1 = 0; this.doubleSpeed = false;
    this.hdmaSrc = 0; this.hdmaDst = 0; this.hdmaLen = 0; this.hdmaActive = false; this.hdmaPending = false;
    this.undoc = new Uint8Array(4);
    this.frameDots = 0;
    this.frame = 0;
    this.serialOut = '';
  }

  requestIrq(bits) { this.if_ |= bits; }

  setKeys(mask) {
    const pressed = mask & ~this.keys;
    this.keys = mask & 0xFF;
    if (pressed & 0xFF) { this.requestIrq(0x10); this.stopped = false; }
  }

  // ------------------------------------------------------------ tempo (1 ciclo de máquina)
  tick() {
    const dots = this.doubleSpeed ? 2 : 4;
    this.dotClock += dots;
    this.frameDots += dots;
    // timer: recarga atrasada do TIMA
    this.timaReloaded = false;
    if (this.timaPending) { this.timaPending = false; this.tima = this.tma; this.if_ |= 4; this.timaReloaded = true; }
    const old = this.div, nw = (old + 4) & 0xFFFF;
    this.div = nw;
    if ((this.tac & 4) && (old & TIMER_BITS[this.tac & 3]) && !(nw & TIMER_BITS[this.tac & 3])) this.incTima();
    const am = this.doubleSpeed ? 0x2000 : 0x1000;
    if ((old & am) && !(nw & am)) this.apu.frameStep();
    this.ppu.tick(dots);
    if (this.dmaActive) {
      if (this.dmaIdx >= 160) { this.dmaActive = false; this.dmaBlock = false; }
      else if (this.dmaDelay) this.dmaDelay--;
      else {
        const s = this.dmaSrc + this.dmaIdx;
        this.dmaBlock = false;
        this.ppu.oam[this.dmaIdx] = this.read(s >= 0xE000 ? s - 0x2000 : s);
        this.dmaBlock = true;
        this.dmaIdx++;
      }
    }
    if (this.serialTimer && --this.serialTimer === 0) {
      this.sb = 0xFF; this.sc &= 0x7F; this.if_ |= 8;
    }
  }

  incTima() {
    this.tima = (this.tima + 1) & 0xFF;
    if (this.tima === 0) this.timaPending = true;
  }

  // ------------------------------------------------------------ memória (sem custo de tempo)
  read(a) {
    if (a < 0x8000) return this.cart.readRom(a);
    if (a < 0xA000) return this.ppu.vramLocked ? 0xFF : this.ppu.vram[(this.ppu.vbk << 13) | (a & 0x1FFF)];
    if (a < 0xC000) return this.cart.readRam(a);
    if (a < 0xD000) return this.wram[a & 0xFFF];
    if (a < 0xE000) return this.wram[(this.svbk << 12) | (a & 0xFFF)];
    if (a < 0xF000) return this.wram[a & 0xFFF];
    if (a < 0xFE00) return this.wram[(this.svbk << 12) | (a & 0xFFF)];
    if (a < 0xFEA0) return (this.dmaBlock || this.ppu.oamLocked) ? 0xFF : this.ppu.oam[a & 0xFF];
    if (a < 0xFF00) return 0x00;
    if (a < 0xFF80) return this.readIO(a);
    if (a < 0xFFFF) return this.hram[a & 0x7F];
    return this.ie;
  }

  write(a, v) {
    if (a < 0x8000) { this.cart.writeRom(a, v); return; }
    if (a < 0xA000) { if (!this.ppu.vramLocked) this.ppu.vram[(this.ppu.vbk << 13) | (a & 0x1FFF)] = v; return; }
    if (a < 0xC000) { this.cart.writeRam(a, v); return; }
    if (a < 0xD000) { this.wram[a & 0xFFF] = v; return; }
    if (a < 0xE000) { this.wram[(this.svbk << 12) | (a & 0xFFF)] = v; return; }
    if (a < 0xF000) { this.wram[a & 0xFFF] = v; return; }
    if (a < 0xFE00) { this.wram[(this.svbk << 12) | (a & 0xFFF)] = v; return; }
    if (a < 0xFEA0) { if (!this.dmaBlock && !this.ppu.oamLocked) this.ppu.oam[a & 0xFF] = v; return; }
    if (a < 0xFF00) return;
    if (a < 0xFF80) { this.writeIO(a, v); return; }
    if (a < 0xFFFF) { this.hram[a & 0x7F] = v; return; }
    this.ie = v;
  }

  readIO(a) {
    const p = this.ppu;
    switch (a) {
      case 0xFF00: {
        let low = 0xF;
        if (!(this.p1 & 0x10)) low &= ~((this.keys >> 4) & 0xF);
        if (!(this.p1 & 0x20)) low &= ~(this.keys & 0xF);
        return 0xC0 | (this.p1 & 0x30) | low;
      }
      case 0xFF01: return this.sb;
      case 0xFF02: return this.sc | (this.cgb ? 0x7C : 0x7E);
      case 0xFF04: return this.div >> 8;
      case 0xFF05: return this.tima;
      case 0xFF06: return this.tma;
      case 0xFF07: return this.tac | 0xF8;
      case 0xFF0F: return this.if_ | 0xE0;
      case 0xFF40: return p.lcdc;
      case 0xFF41: return p.readStat();
      case 0xFF42: return p.scy;
      case 0xFF43: return p.scx;
      case 0xFF44: return p.ly;
      case 0xFF45: return p.lyc;
      case 0xFF46: return this.dmaReg;
      case 0xFF47: return p.bgp;
      case 0xFF48: return p.obp0;
      case 0xFF49: return p.obp1;
      case 0xFF4A: return p.wy;
      case 0xFF4B: return p.wx;
    }
    if (a >= 0xFF10 && a < 0xFF40) return this.apu.read(a);
    if (!this.cgb) return 0xFF;
    switch (a) {
      case 0xFF4D: return (this.doubleSpeed ? 0x80 : 0) | 0x7E | (this.key1 & 1);
      case 0xFF4F: return 0xFE | p.vbk;
      case 0xFF55: return this.hdmaActive ? ((this.hdmaLen - 1) & 0x7F) : this.hdmaLen ? 0x80 | ((this.hdmaLen - 1) & 0x7F) : 0xFF;
      case 0xFF68: return p.bcps | 0x40;
      case 0xFF69: return p.bgPal[p.bcps & 0x3F];
      case 0xFF6A: return p.ocps | 0x40;
      case 0xFF6B: return p.objPal[p.ocps & 0x3F];
      case 0xFF6C: return p.opri | 0xFE;
      case 0xFF70: return this.svbk | 0xF8;
      case 0xFF72: case 0xFF73: return this.undoc[a - 0xFF72];
      case 0xFF74: return this.undoc[2];
      case 0xFF75: return this.undoc[3] | 0x8F;
    }
    return 0xFF;
  }

  writeIO(a, v) {
    const p = this.ppu;
    switch (a) {
      case 0xFF00: this.p1 = v & 0x30; return;
      case 0xFF01: this.sb = v; return;
      case 0xFF02:
        this.sc = v;
        if ((v & 0x81) === 0x81) {
          // sem cabo: o byte enviado é registrado (testes) e o recebido é 0xFF
          if (this.serialOut.length < 4096) this.serialOut += String.fromCharCode(this.sb);
          this.serialTimer = (this.cgb && (v & 2)) ? 8 : 256;
        }
        return;
      case 0xFF04: {
        const old = this.div;
        this.div = 0;
        if ((this.tac & 4) && (old & TIMER_BITS[this.tac & 3])) this.incTima();
        if (old & (this.doubleSpeed ? 0x2000 : 0x1000)) this.apu.frameStep();
        return;
      }
      case 0xFF05: if (!this.timaReloaded) { this.tima = v; this.timaPending = false; } return;
      case 0xFF06: this.tma = v; if (this.timaReloaded) this.tima = v; return;
      case 0xFF07: {
        const oldOut = (this.tac & 4) && (this.div & TIMER_BITS[this.tac & 3]);
        this.tac = v & 7;
        const newOut = (this.tac & 4) && (this.div & TIMER_BITS[this.tac & 3]);
        if (oldOut && !newOut) this.incTima();
        return;
      }
      case 0xFF0F: this.if_ = v & 0x1F; return;
      case 0xFF40: p.writeLcdc(v); return;
      case 0xFF41: p.writeStat(v); return;
      case 0xFF42: p.scy = v; return;
      case 0xFF43: p.scx = v; return;
      case 0xFF44: return;
      case 0xFF45: p.writeLyc(v); return;
      case 0xFF46: this.dmaReg = v; this.dmaSrc = v << 8; this.dmaIdx = 0; this.dmaDelay = DMA_DELAY; this.dmaActive = true; return; // num reinício a OAM segue bloqueada
      case 0xFF47: p.bgp = v; return;
      case 0xFF48: p.obp0 = v; return;
      case 0xFF49: p.obp1 = v; return;
      case 0xFF4A: p.wy = v; return;
      case 0xFF4B: p.wx = v; return;
    }
    if (a >= 0xFF10 && a < 0xFF40) { this.apu.write(a, v); return; }
    if (!this.cgb) return;
    switch (a) {
      case 0xFF4D: this.key1 = v & 1; return;
      case 0xFF4F: p.vbk = v & 1; return;
      case 0xFF51: this.hdmaSrc = (this.hdmaSrc & 0xFF) | (v << 8); return;
      case 0xFF52: this.hdmaSrc = (this.hdmaSrc & 0xFF00) | (v & 0xF0); return;
      case 0xFF53: this.hdmaDst = (this.hdmaDst & 0xFF) | ((v & 0x1F) << 8); return;
      case 0xFF54: this.hdmaDst = (this.hdmaDst & 0x1F00) | (v & 0xF0); return;
      case 0xFF55:
        if (this.hdmaActive && !(v & 0x80)) { this.hdmaActive = false; return; }
        this.hdmaLen = (v & 0x7F) + 1;
        if (v & 0x80) {
          this.hdmaActive = true;
          if (!p.lcdOn || p.mode === 0) this.hdmaPending = true;
        } else {
          // DMA geral: copia tudo de uma vez, com a CPU parada
          while (this.hdmaLen) this.hdmaBlock();
        }
        return;
      case 0xFF68: p.bcps = v & 0xBF; return;
      case 0xFF69: p.writeBcpd(v); return;
      case 0xFF6A: p.ocps = v & 0xBF; return;
      case 0xFF6B: p.writeOcpd(v); return;
      case 0xFF6C: p.opri = v & 1; return;
      case 0xFF70: this.svbk = (v & 7) || 1; return;
      case 0xFF72: case 0xFF73: case 0xFF74: this.undoc[a - 0xFF72] = v; return;
      case 0xFF75: this.undoc[3] = v & 0x70; return;
    }
  }

  hblankDma() { if (this.hdmaActive) this.hdmaPending = true; }

  hdmaBlock() {
    const vram = this.ppu.vram, bank = this.ppu.vbk << 13;
    for (let k = 0; k < 16; k++) {
      const s = (this.hdmaSrc + k) & 0xFFFF;
      vram[bank | ((this.hdmaDst + k) & 0x1FFF)] = this.read(s >= 0xE000 ? s - 0x2000 : s);
    }
    this.hdmaSrc = (this.hdmaSrc + 16) & 0xFFFF;
    this.hdmaDst = (this.hdmaDst + 16) & 0x1FFF;
    this.hdmaLen--;
    if (!this.hdmaLen) this.hdmaActive = false;
    const cycles = this.doubleSpeed ? 16 : 8;
    for (let k = 0; k < cycles; k++) this.tick();
  }

  // ------------------------------------------------------------ acessos com tempo
  rd(a) { this.tick(); return this.read(a); }
  wr(a, v) { this.tick(); this.write(a, v); }
  imm() { const v = this.rd(this.pc); this.pc = (this.pc + 1) & 0xFFFF; return v; }
  imm16() { const lo = this.imm(); return lo | (this.imm() << 8); }
  push(v) {
    this.tick();
    this.sp = (this.sp - 1) & 0xFFFF; this.wr(this.sp, v >> 8);
    this.sp = (this.sp - 1) & 0xFFFF; this.wr(this.sp, v & 0xFF);
  }
  pop() {
    const lo = this.rd(this.sp); this.sp = (this.sp + 1) & 0xFFFF;
    const hi = this.rd(this.sp); this.sp = (this.sp + 1) & 0xFFFF;
    return lo | (hi << 8);
  }

  get bc() { return (this.b << 8) | this.c; }
  get de() { return (this.d << 8) | this.e; }
  get hl() { return (this.h << 8) | this.l; }
  set bc(v) { this.b = (v >> 8) & 0xFF; this.c = v & 0xFF; }
  set de(v) { this.d = (v >> 8) & 0xFF; this.e = v & 0xFF; }
  set hl(v) { this.h = (v >> 8) & 0xFF; this.l = v & 0xFF; }

  getR(i) {
    switch (i) {
      case 0: return this.b; case 1: return this.c; case 2: return this.d; case 3: return this.e;
      case 4: return this.h; case 5: return this.l; case 6: return this.rd(this.hl); default: return this.a;
    }
  }
  setR(i, v) {
    switch (i) {
      case 0: this.b = v; break; case 1: this.c = v; break; case 2: this.d = v; break; case 3: this.e = v; break;
      case 4: this.h = v; break; case 5: this.l = v; break; case 6: this.wr(this.hl, v); break; default: this.a = v;
    }
  }
  getRR(i) { return i === 0 ? this.bc : i === 1 ? this.de : i === 2 ? this.hl : this.sp; }
  setRR(i, v) { if (i === 0) this.bc = v; else if (i === 1) this.de = v; else if (i === 2) this.hl = v; else this.sp = v & 0xFFFF; }
  cond(i) {
    switch (i) { case 0: return !(this.f & FZ); case 1: return (this.f & FZ) !== 0; case 2: return !(this.f & FC); default: return (this.f & FC) !== 0; }
  }

  // ------------------------------------------------------------ ULA
  alu(op, v) {
    const a = this.a;
    switch (op) {
      case 0: case 1: {
        const c = op === 1 && (this.f & FC) ? 1 : 0;
        const r = a + v + c;
        this.f = ((r & 0xFF) ? 0 : FZ) | (((a & 0xF) + (v & 0xF) + c) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
        this.a = r & 0xFF; break;
      }
      case 2: case 3: case 7: {
        const c = op === 3 && (this.f & FC) ? 1 : 0;
        const r = a - v - c;
        this.f = ((r & 0xFF) ? 0 : FZ) | FN | (((a & 0xF) - (v & 0xF) - c) < 0 ? FH : 0) | (r < 0 ? FC : 0);
        if (op !== 7) this.a = r & 0xFF;
        break;
      }
      case 4: this.a = a & v; this.f = (this.a ? 0 : FZ) | FH; break;
      case 5: this.a = a ^ v; this.f = this.a ? 0 : FZ; break;
      case 6: this.a = a | v; this.f = this.a ? 0 : FZ; break;
    }
  }
  inc8(v) { const r = (v + 1) & 0xFF; this.f = (this.f & FC) | (r ? 0 : FZ) | ((v & 0xF) === 0xF ? FH : 0); return r; }
  dec8(v) { const r = (v - 1) & 0xFF; this.f = (this.f & FC) | FN | (r ? 0 : FZ) | ((v & 0xF) === 0 ? FH : 0); return r; }
  spOffset() {
    const e = this.imm();
    const s = (e << 24) >> 24;
    this.f = (((this.sp & 0xF) + (e & 0xF)) > 0xF ? FH : 0) | (((this.sp & 0xFF) + e) > 0xFF ? FC : 0);
    return (this.sp + s) & 0xFFFF;
  }

  cb() {
    const op = this.imm();
    const r = op & 7, x = op >> 6, y = (op >> 3) & 7;
    let v = this.getR(r);
    if (x === 1) { this.f = (this.f & FC) | FH | ((v & (1 << y)) ? 0 : FZ); return; }
    if (x === 2) { this.setR(r, v & ~(1 << y)); return; }
    if (x === 3) { this.setR(r, v | (1 << y)); return; }
    let c = 0;
    switch (y) {
      case 0: c = v >> 7; v = ((v << 1) | c) & 0xFF; break;
      case 1: c = v & 1; v = (v >> 1) | (c << 7); break;
      case 2: c = v >> 7; v = ((v << 1) | ((this.f & FC) ? 1 : 0)) & 0xFF; break;
      case 3: c = v & 1; v = (v >> 1) | ((this.f & FC) ? 0x80 : 0); break;
      case 4: c = v >> 7; v = (v << 1) & 0xFF; break;
      case 5: c = v & 1; v = (v >> 1) | (v & 0x80); break;
      case 6: v = ((v >> 4) | (v << 4)) & 0xFF; break;
      case 7: c = v & 1; v >>= 1; break;
    }
    this.f = (v ? 0 : FZ) | (c ? FC : 0);
    this.setR(r, v);
  }

  // ------------------------------------------------------------ execução
  step() {
    if (this.hdmaPending && !this.halted) { this.hdmaPending = false; if (this.hdmaActive) this.hdmaBlock(); }
    if (this.halted || this.stopped || this.locked) {
      this.tick();
      if (this.locked) return;
      if (this.stopped) return;
      if (!(this.ie & this.if_ & 0x1F)) return;
      this.halted = false;
    }
    if (this.ime && (this.ie & this.if_ & 0x1F)) { this.interrupt(); return; }
    if (this.eiDelay) { this.ime = true; this.eiDelay = false; }

    const op = this.rd(this.pc);
    if (this.haltBug) this.haltBug = false; else this.pc = (this.pc + 1) & 0xFFFF;
    this.exec(op);
  }

  interrupt() {
    this.ime = false;
    this.tick(); this.tick();
    this.sp = (this.sp - 1) & 0xFFFF; this.wr(this.sp, this.pc >> 8);
    // o vetor é escolhido depois do primeiro byte empilhado (se o IE mudou ali, o resultado muda)
    const pend = this.ie & this.if_ & 0x1F;
    this.sp = (this.sp - 1) & 0xFFFF; this.wr(this.sp, this.pc & 0xFF);
    if (!pend) { this.pc = 0; this.tick(); return; }
    const bit = pend & -pend;
    this.if_ &= ~bit;
    this.pc = 0x40 + 8 * (31 - Math.clz32(bit));
    this.tick();
  }

  exec(op) {
    // blocos regulares
    if (op >= 0x40 && op < 0x80) {
      if (op === 0x76) { // HALT
        if (!this.ime && (this.ie & this.if_ & 0x1F)) this.haltBug = true;
        else this.halted = true;
        return;
      }
      this.setR((op >> 3) & 7, this.getR(op & 7));
      return;
    }
    if (op >= 0x80 && op < 0xC0) { this.alu((op >> 3) & 7, this.getR(op & 7)); return; }

    switch (op) {
      case 0x00: return;
      case 0x01: case 0x11: case 0x21: case 0x31: this.setRR(op >> 4, this.imm16()); return;
      case 0x02: this.wr(this.bc, this.a); return;
      case 0x12: this.wr(this.de, this.a); return;
      case 0x22: { const hl = this.hl; this.wr(hl, this.a); this.hl = (hl + 1) & 0xFFFF; return; }
      case 0x32: { const hl = this.hl; this.wr(hl, this.a); this.hl = (hl - 1) & 0xFFFF; return; }
      case 0x0A: this.a = this.rd(this.bc); return;
      case 0x1A: this.a = this.rd(this.de); return;
      case 0x2A: { const hl = this.hl; this.a = this.rd(hl); this.hl = (hl + 1) & 0xFFFF; return; }
      case 0x3A: { const hl = this.hl; this.a = this.rd(hl); this.hl = (hl - 1) & 0xFFFF; return; }
      case 0x03: case 0x13: case 0x23: case 0x33: { const i = op >> 4; this.setRR(i, (this.getRR(i) + 1) & 0xFFFF); this.tick(); return; }
      case 0x0B: case 0x1B: case 0x2B: case 0x3B: { const i = op >> 4; this.setRR(i, (this.getRR(i) - 1) & 0xFFFF); this.tick(); return; }
      case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C: { const i = (op >> 3) & 7; this.setR(i, this.inc8(this.getR(i))); return; }
      case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D: { const i = (op >> 3) & 7; this.setR(i, this.dec8(this.getR(i))); return; }
      case 0x34: { const hl = this.hl; this.wr(hl, this.inc8(this.rd(hl))); return; }
      case 0x35: { const hl = this.hl; this.wr(hl, this.dec8(this.rd(hl))); return; }
      case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E: this.setR((op >> 3) & 7, this.imm()); return;
      case 0x36: { const v = this.imm(); this.wr(this.hl, v); return; }
      case 0x07: { const c = this.a >> 7; this.a = ((this.a << 1) | c) & 0xFF; this.f = c ? FC : 0; return; }
      case 0x0F: { const c = this.a & 1; this.a = (this.a >> 1) | (c << 7); this.f = c ? FC : 0; return; }
      case 0x17: { const c = this.a >> 7; this.a = ((this.a << 1) | ((this.f & FC) ? 1 : 0)) & 0xFF; this.f = c ? FC : 0; return; }
      case 0x1F: { const c = this.a & 1; this.a = (this.a >> 1) | ((this.f & FC) ? 0x80 : 0); this.f = c ? FC : 0; return; }
      case 0x08: { const a = this.imm16(); this.wr(a, this.sp & 0xFF); this.wr((a + 1) & 0xFFFF, this.sp >> 8); return; }
      case 0x09: case 0x19: case 0x29: case 0x39: {
        const hl = this.hl, v = this.getRR(op >> 4), r = hl + v;
        this.f = (this.f & FZ) | (((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0);
        this.hl = r & 0xFFFF; this.tick(); return;
      }
      case 0x10: { // STOP
        this.imm();
        if (this.cgb && (this.key1 & 1)) { this.doubleSpeed = !this.doubleSpeed; this.key1 = 0; this.div = 0; }
        else this.stopped = true;
        return;
      }
      case 0x18: { const e = (this.imm() << 24) >> 24; this.pc = (this.pc + e) & 0xFFFF; this.tick(); return; }
      case 0x20: case 0x28: case 0x30: case 0x38: {
        const e = (this.imm() << 24) >> 24;
        if (this.cond((op >> 3) & 3)) { this.pc = (this.pc + e) & 0xFFFF; this.tick(); }
        return;
      }
      case 0x27: {
        let a = this.a, f = this.f;
        if (!(f & FN)) {
          if ((f & FC) || a > 0x99) { a += 0x60; f |= FC; }
          if ((f & FH) || (a & 0xF) > 9) a += 6;
        } else {
          if (f & FC) a -= 0x60;
          if (f & FH) a -= 6;
        }
        this.a = a & 0xFF;
        this.f = (f & (FN | FC)) | (this.a ? 0 : FZ);
        return;
      }
      case 0x2F: this.a ^= 0xFF; this.f |= FN | FH; return;
      case 0x37: this.f = (this.f & FZ) | FC; return;
      case 0x3F: this.f = (this.f & (FZ | FC)) ^ FC; return;

      case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        this.tick();
        if (this.cond((op >> 3) & 3)) { this.pc = this.pop(); this.tick(); }
        return;
      case 0xC1: this.bc = this.pop(); return;
      case 0xD1: this.de = this.pop(); return;
      case 0xE1: this.hl = this.pop(); return;
      case 0xF1: { const v = this.pop(); this.a = v >> 8; this.f = v & 0xF0; return; }
      case 0xC5: this.push(this.bc); return;
      case 0xD5: this.push(this.de); return;
      case 0xE5: this.push(this.hl); return;
      case 0xF5: this.push((this.a << 8) | this.f); return;
      case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
        const a = this.imm16();
        if (this.cond((op >> 3) & 3)) { this.pc = a; this.tick(); }
        return;
      }
      case 0xC3: this.pc = this.imm16(); this.tick(); return;
      case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
        const a = this.imm16();
        if (this.cond((op >> 3) & 3)) { this.push(this.pc); this.pc = a; }
        return;
      }
      case 0xCD: { const a = this.imm16(); this.push(this.pc); this.pc = a; return; }
      case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE: this.alu((op >> 3) & 7, this.imm()); return;
      case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: this.push(this.pc); this.pc = op & 0x38; return;
      case 0xC9: this.pc = this.pop(); this.tick(); return;
      case 0xD9: this.pc = this.pop(); this.tick(); this.ime = true; return;
      case 0xCB: this.cb(); return;
      case 0xE0: { const a = 0xFF00 | this.imm(); this.wr(a, this.a); return; }
      case 0xF0: { const a = 0xFF00 | this.imm(); this.a = this.rd(a); return; }
      case 0xE2: this.wr(0xFF00 | this.c, this.a); return;
      case 0xF2: this.a = this.rd(0xFF00 | this.c); return;
      case 0xE8: this.sp = this.spOffset(); this.tick(); this.tick(); return;
      case 0xF8: this.hl = this.spOffset(); this.tick(); return;
      case 0xF9: this.sp = this.hl; this.tick(); return;
      case 0xE9: this.pc = this.hl; return;
      case 0xEA: { const a = this.imm16(); this.wr(a, this.a); return; }
      case 0xFA: { const a = this.imm16(); this.a = this.rd(a); return; }
      case 0xF3: this.ime = false; this.eiDelay = false; return;
      case 0xFB: this.eiDelay = true; return;
      default:
        // opcodes inexistentes travam a CPU
        this.locked = true;
    }
  }

  runFrame() {
    this.ppu.frameDone = false;
    this.frameDots = 0;
    const limit = this.ppu.lcdOn ? FRAME_DOTS + 456 : FRAME_DOTS;
    while (!this.ppu.frameDone && this.frameDots < limit) this.step();
    this.apu.catchUp();
    this.frame++;
  }

  // ------------------------------------------------------------ save state
  saveState() {
    const cpu = [this.a, this.f, this.b, this.c, this.d, this.e, this.h, this.l, this.sp, this.pc,
      this.ime, this.eiDelay, this.halted, this.haltBug, this.stopped, this.locked, this.ie, this.if_,
      this.div, this.tima, this.tma, this.tac, this.timaPending, this.p1, this.sb, this.sc, this.serialTimer,
      this.dmaActive, this.dmaDelay, this.dmaSrc, this.dmaIdx, this.dmaReg, this.svbk, this.key1, this.doubleSpeed,
      this.hdmaSrc, this.hdmaDst, this.hdmaLen, this.hdmaActive, this.hdmaPending].map(Number);
    return { v: 1, cgb: this.cgb, cpu, wram: this.wram.slice(), hram: this.hram.slice(), undoc: this.undoc.slice(),
      ppu: this.ppu.state(), apu: this.apu.state(), cart: this.cart.state() };
  }

  loadState(s) {
    if (!s || s.v !== 1 || s.cgb !== this.cgb) throw new Error('Estado salvo incompatível com este jogo.');
    [this.a, this.f, this.b, this.c, this.d, this.e, this.h, this.l, this.sp, this.pc,
      this.ime, this.eiDelay, this.halted, this.haltBug, this.stopped, this.locked, this.ie, this.if_,
      this.div, this.tima, this.tma, this.tac, this.timaPending, this.p1, this.sb, this.sc, this.serialTimer,
      this.dmaActive, this.dmaDelay, this.dmaSrc, this.dmaIdx, this.dmaReg, this.svbk, this.key1, this.doubleSpeed,
      this.hdmaSrc, this.hdmaDst, this.hdmaLen, this.hdmaActive, this.hdmaPending] = s.cpu;
    for (const k of ['ime', 'eiDelay', 'halted', 'haltBug', 'stopped', 'locked', 'timaPending', 'dmaActive', 'doubleSpeed', 'hdmaActive', 'hdmaPending']) this[k] = !!this[k];
    this.wram.set(s.wram); this.hram.set(s.hram); this.undoc.set(s.undoc);
    this.ppu.restore(s.ppu); this.apu.restore(s.apu); this.cart.restore(s.cart);
  }
}
