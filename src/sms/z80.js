// CPU Zilog Z80 (Master System / Game Gear): conjunto completo, incluindo instruções e flags não
// documentadas (bits 3 e 5, IXH/IXL, SLL, DDCB com cópia no registrador) e MEMPTR para BIT n,(HL).
// Cada step() executa uma instrução e devolve os T-states gastos.

const FC = 0x01, FN = 0x02, FP = 0x04, F3 = 0x08, FH = 0x10, F5 = 0x20, FZ = 0x40, FS = 0x80;

const SZ53 = new Uint8Array(256), SZ53P = new Uint8Array(256), PAR = new Uint8Array(256);
for (let v = 0; v < 256; v++) {
  let p = v; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
  PAR[v] = (p & 1) ? 0 : FP;
  SZ53[v] = (v & (FS | F3 | F5)) | (v === 0 ? FZ : 0);
  SZ53P[v] = SZ53[v] | PAR[v];
}

// T-states das instruções sem prefixo (condicionais somam o extra no código)
const CYC = [
  4, 10, 7, 6, 4, 4, 7, 4, 4, 11, 7, 6, 4, 4, 7, 4,
  8, 10, 7, 6, 4, 4, 7, 4, 12, 11, 7, 6, 4, 4, 7, 4,
  7, 10, 16, 6, 4, 4, 7, 4, 7, 11, 16, 6, 4, 4, 7, 4,
  7, 10, 13, 6, 11, 11, 10, 4, 7, 11, 13, 6, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  7, 7, 7, 7, 7, 7, 4, 7, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  4, 4, 4, 4, 4, 4, 7, 4, 4, 4, 4, 4, 4, 4, 7, 4,
  5, 10, 10, 10, 10, 11, 7, 11, 5, 10, 10, 0, 10, 17, 7, 11,
  5, 10, 10, 11, 10, 11, 7, 11, 5, 4, 10, 11, 10, 0, 7, 11,
  5, 10, 10, 19, 10, 11, 7, 11, 5, 4, 10, 4, 10, 0, 7, 11,
  5, 10, 10, 4, 10, 11, 7, 11, 5, 6, 10, 4, 10, 0, 7, 11,
];

export class Z80 {
  constructor(bus) {
    this.bus = bus; // read(a), write(a, v), in(port), out(port, v)
    this.reset();
  }

  reset() {
    this.a = 0xFF; this.f = 0xFF; this.b = 0; this.c = 0; this.d = 0; this.e = 0; this.h = 0; this.l = 0;
    this.a2 = 0; this.f2 = 0; this.b2 = 0; this.c2 = 0; this.d2 = 0; this.e2 = 0; this.h2 = 0; this.l2 = 0;
    this.ix = 0xFFFF; this.iy = 0xFFFF; this.sp = 0xDFF0; this.pc = 0;
    this.i = 0; this.r = 0; this.wz = 0;
    this.iff1 = 0; this.iff2 = 0; this.im = 1; this.halted = false; this.eiDelay = false;
    this.irqLine = false; this.nmiPending = false;
  }

  // ------------------------------------------------------------ acesso
  rd(a) { return this.bus.read(a & 0xFFFF); }
  wr(a, v) { this.bus.write(a & 0xFFFF, v & 0xFF); }
  rd16(a) { return this.rd(a) | (this.rd(a + 1) << 8); }
  wr16(a, v) { this.wr(a, v); this.wr(a + 1, v >> 8); }
  imm() { const v = this.rd(this.pc); this.pc = (this.pc + 1) & 0xFFFF; return v; }
  imm16() { const v = this.rd16(this.pc); this.pc = (this.pc + 2) & 0xFFFF; return v; }
  disp() { const d = this.imm(); return d < 128 ? d : d - 256; }
  push(v) { this.sp = (this.sp - 2) & 0xFFFF; this.wr16(this.sp, v); }
  pop() { const v = this.rd16(this.sp); this.sp = (this.sp + 2) & 0xFFFF; return v; }
  incR() { this.r = (this.r & 0x80) | ((this.r + 1) & 0x7F); }

  get bc() { return (this.b << 8) | this.c; } set bc(v) { this.b = (v >> 8) & 0xFF; this.c = v & 0xFF; }
  get de() { return (this.d << 8) | this.e; } set de(v) { this.d = (v >> 8) & 0xFF; this.e = v & 0xFF; }
  get hl() { return (this.h << 8) | this.l; } set hl(v) { this.h = (v >> 8) & 0xFF; this.l = v & 0xFF; }
  get af() { return (this.a << 8) | this.f; } set af(v) { this.a = (v >> 8) & 0xFF; this.f = v & 0xFF; }

  // ------------------------------------------------------------ interrupções
  // IRQ mascarável (nível): o VDP mantém a linha enquanto houver pedido pendente
  interrupt() {
    if (this.nmiPending) {
      this.nmiPending = false;
      this.halted = false;
      this.iff1 = 0;
      this.incR();
      this.push(this.pc); this.pc = 0x66;
      return 11;
    }
    if (!this.irqLine || !this.iff1 || this.eiDelay) return 0;
    this.halted = false;
    this.iff1 = this.iff2 = 0;
    this.incR();
    if (this.im === 2) {
      this.push(this.pc);
      this.pc = this.rd16((this.i << 8) | 0xFF);
      this.wz = this.pc;
      return 19;
    }
    this.push(this.pc); this.pc = 0x38; this.wz = 0x38;
    return this.im === 0 ? 13 : 13;
  }

  step() {
    const t = this.interrupt();
    if (t) return t;
    this.eiDelay = false;
    if (this.halted) { this.incR(); return 4; }
    this.incR();
    const op = this.imm();
    return this.exec(op, 0);
  }

  // ------------------------------------------------------------ registradores por índice (0 B,1 C,2 D,3 E,4 H,5 L,6 (HL),7 A)
  // idx: 0 = HL, 1 = IX, 2 = IY (os prefixos DD/FD trocam H/L por IXH/IXL)
  getR(i, idx) {
    switch (i) {
      case 0: return this.b; case 1: return this.c; case 2: return this.d; case 3: return this.e;
      case 4: return idx === 0 ? this.h : (idx === 1 ? this.ix : this.iy) >> 8;
      case 5: return idx === 0 ? this.l : (idx === 1 ? this.ix : this.iy) & 0xFF;
      case 7: return this.a;
    }
    return 0;
  }
  setR(i, idx, v) {
    v &= 0xFF;
    switch (i) {
      case 0: this.b = v; break; case 1: this.c = v; break; case 2: this.d = v; break; case 3: this.e = v; break;
      case 4: if (idx === 0) this.h = v; else if (idx === 1) this.ix = (this.ix & 0xFF) | (v << 8); else this.iy = (this.iy & 0xFF) | (v << 8); break;
      case 5: if (idx === 0) this.l = v; else if (idx === 1) this.ix = (this.ix & 0xFF00) | v; else this.iy = (this.iy & 0xFF00) | v; break;
      case 7: this.a = v; break;
    }
  }
  getHL(idx) { return idx === 0 ? this.hl : idx === 1 ? this.ix : this.iy; }
  setHL(idx, v) { v &= 0xFFFF; if (idx === 0) this.hl = v; else if (idx === 1) this.ix = v; else this.iy = v; }
  // endereço de (HL) ou (IX+d)/(IY+d); lê o deslocamento
  addrHL(idx) {
    if (idx === 0) return this.hl;
    const a = ((idx === 1 ? this.ix : this.iy) + this.disp()) & 0xFFFF;
    this.wz = a;
    return a;
  }
  getRR(p, idx) { return p === 0 ? this.bc : p === 1 ? this.de : p === 2 ? this.getHL(idx) : this.sp; }
  setRR(p, idx, v) { v &= 0xFFFF; if (p === 0) this.bc = v; else if (p === 1) this.de = v; else if (p === 2) this.setHL(idx, v); else this.sp = v; }
  cond(c) {
    const f = this.f;
    switch (c) {
      case 0: return !(f & FZ); case 1: return !!(f & FZ); case 2: return !(f & FC); case 3: return !!(f & FC);
      case 4: return !(f & FP); case 5: return !!(f & FP); case 6: return !(f & FS); default: return !!(f & FS);
    }
  }

  // ------------------------------------------------------------ ULA
  alu(op, v) {
    const a = this.a;
    let r;
    switch (op) {
      case 0: case 1: {
        const c = op === 1 ? this.f & FC : 0;
        r = a + v + c;
        this.f = SZ53[r & 0xFF] | ((a ^ v ^ r) & FH) | (r >> 8 & 1) | (((a ^ ~v) & (a ^ r) & 0x80) >> 5);
        this.a = r & 0xFF; return;
      }
      case 2: case 3: case 7: {
        const c = op === 3 ? this.f & FC : 0;
        r = a - v - c;
        const f = (SZ53[r & 0xFF] & ~(F3 | F5)) | FN | ((a ^ v ^ r) & FH) | ((r >> 8) & 1) | (((a ^ v) & (a ^ r) & 0x80) >> 5);
        if (op === 7) { this.f = f | (v & (F3 | F5)); return; }
        this.f = f | (r & (F3 | F5));
        this.a = r & 0xFF; return;
      }
      case 4: this.a = a & v; this.f = SZ53P[this.a] | FH; return;
      case 5: this.a = (a ^ v) & 0xFF; this.f = SZ53P[this.a]; return;
      case 6: this.a = (a | v) & 0xFF; this.f = SZ53P[this.a]; return;
    }
  }
  inc8(v) { const r = (v + 1) & 0xFF; this.f = (this.f & FC) | SZ53[r] | ((r & 0xF) === 0 ? FH : 0) | (r === 0x80 ? FP : 0); return r; }
  dec8(v) { const r = (v - 1) & 0xFF; this.f = (this.f & FC) | FN | SZ53[r] | ((r & 0xF) === 0xF ? FH : 0) | (r === 0x7F ? FP : 0); return r; }
  add16(a, v) {
    const r = a + v;
    this.wz = (a + 1) & 0xFFFF;
    this.f = (this.f & (FS | FZ | FP)) | ((r >> 8) & (F3 | F5)) | (((a ^ v ^ r) >> 8) & FH) | ((r >> 16) & 1);
    return r & 0xFFFF;
  }
  adc16(v) {
    const a = this.hl, r = a + v + (this.f & FC);
    this.wz = (a + 1) & 0xFFFF;
    this.f = ((r >> 8) & (FS | F3 | F5)) | ((r & 0xFFFF) ? 0 : FZ) | (((a ^ v ^ r) >> 8) & FH) | ((r >> 16) & 1) | ((((a ^ ~v) & (a ^ r)) >> 13) & FP);
    this.hl = r & 0xFFFF;
  }
  sbc16(v) {
    const a = this.hl, r = a - v - (this.f & FC);
    this.wz = (a + 1) & 0xFFFF;
    this.f = ((r >> 8) & (FS | F3 | F5)) | ((r & 0xFFFF) ? 0 : FZ) | FN | (((a ^ v ^ r) >> 8) & FH) | ((r >> 16) & 1) | ((((a ^ v) & (a ^ r)) >> 13) & FP);
    this.hl = r & 0xFFFF;
  }
  rot(op, v) {
    let c, r;
    switch (op) {
      case 0: c = v >> 7; r = ((v << 1) | c) & 0xFF; break;            // RLC
      case 1: c = v & 1; r = (v >> 1) | (c << 7); break;               // RRC
      case 2: c = v >> 7; r = ((v << 1) | (this.f & FC)) & 0xFF; break; // RL
      case 3: c = v & 1; r = (v >> 1) | ((this.f & FC) << 7); break;    // RR
      case 4: c = v >> 7; r = (v << 1) & 0xFF; break;                   // SLA
      case 5: c = v & 1; r = (v >> 1) | (v & 0x80); break;              // SRA
      case 6: c = v >> 7; r = ((v << 1) | 1) & 0xFF; break;             // SLL (não documentada)
      default: c = v & 1; r = v >> 1; break;                            // SRL
    }
    this.f = SZ53P[r] | c;
    return r;
  }
  bit(n, v, xy) {
    const set = v & (1 << n);
    this.f = (this.f & FC) | FH | (xy & (F3 | F5)) | (set ? 0 : (FZ | FP)) | (n === 7 && set ? FS : 0);
  }
  daa() {
    let a = this.a, f = this.f, t = 0, c = f & FC;
    const h = f & FH, n = f & FN;
    if (h || (a & 0xF) > 9) t++;
    if (c || a > 0x99) { t += 2; c = 1; }
    let hf;
    if (n && !h) hf = 0;
    else if (n && h) hf = (a & 0xF) < 6 ? FH : 0;
    else hf = (a & 0xF) >= 0xA ? FH : 0;
    switch (t) {
      case 1: a += n ? 0xFA : 0x06; break;
      case 2: a += n ? 0xA0 : 0x60; break;
      case 3: a += n ? 0x9A : 0x66; break;
    }
    a &= 0xFF;
    this.a = a;
    this.f = SZ53P[a] | hf | n | c;
  }

  // ------------------------------------------------------------ execução (idx: 0 sem prefixo, 1 DD, 2 FD)
  exec(op, idx) {
    let t = CYC[op];
    if (idx) t += 4;
    const x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;

    if (x === 1) {
      if (op === 0x76) { this.halted = true; return t; }
      if (z === 6) { const a = this.addrHL(idx); if (idx) t += 8; this.setR(y, 0, this.rd(a)); return t; }
      if (y === 6) { const a = this.addrHL(idx); if (idx) t += 8; this.wr(a, this.getR(z, 0)); return t; }
      this.setR(y, idx, this.getR(z, idx));
      return t;
    }
    if (x === 2) {
      if (z === 6) { const a = this.addrHL(idx); if (idx) t += 8; this.alu(y, this.rd(a)); return t; }
      this.alu(y, this.getR(z, idx));
      return t;
    }
    if (x === 0) {
      switch (z) {
        case 0:
          switch (y) {
            case 0: return t;
            case 1: { // EX AF,AF'
              let v = this.a; this.a = this.a2; this.a2 = v; v = this.f; this.f = this.f2; this.f2 = v; return t;
            }
            case 2: { const d = this.disp(); this.b = (this.b - 1) & 0xFF; if (this.b) { this.pc = (this.pc + d) & 0xFFFF; this.wz = this.pc; return t + 5; } return t; }
            case 3: { const d = this.disp(); this.pc = (this.pc + d) & 0xFFFF; this.wz = this.pc; return t; }
            default: { const d = this.disp(); if (this.cond(y - 4)) { this.pc = (this.pc + d) & 0xFFFF; this.wz = this.pc; return t + 5; } return t; }
          }
        case 1:
          if (!q) { this.setRR(p, idx, this.imm16()); return t; }
          this.setHL(idx, this.add16(this.getHL(idx), this.getRR(p, idx)));
          return t;
        case 2:
          switch (y) {
            case 0: this.wr(this.bc, this.a); this.wz = ((this.bc + 1) & 0xFF) | (this.a << 8); return t;
            case 1: this.a = this.rd(this.bc); this.wz = (this.bc + 1) & 0xFFFF; return t;
            case 2: this.wr(this.de, this.a); this.wz = ((this.de + 1) & 0xFF) | (this.a << 8); return t;
            case 3: this.a = this.rd(this.de); this.wz = (this.de + 1) & 0xFFFF; return t;
            case 4: { const a = this.imm16(); this.wr16(a, this.getHL(idx)); this.wz = (a + 1) & 0xFFFF; return t; }
            case 5: { const a = this.imm16(); this.setHL(idx, this.rd16(a)); this.wz = (a + 1) & 0xFFFF; return t; }
            case 6: { const a = this.imm16(); this.wr(a, this.a); this.wz = ((a + 1) & 0xFF) | (this.a << 8); return t; }
            default: { const a = this.imm16(); this.a = this.rd(a); this.wz = (a + 1) & 0xFFFF; return t; }
          }
        case 3:
          this.setRR(p, idx, this.getRR(p, idx) + (q ? -1 : 1));
          return t;
        case 4: case 5: {
          if (y === 6) {
            const a = this.addrHL(idx); if (idx) t += 8;
            this.wr(a, z === 4 ? this.inc8(this.rd(a)) : this.dec8(this.rd(a)));
            return t;
          }
          const v = this.getR(y, idx);
          this.setR(y, idx, z === 4 ? this.inc8(v) : this.dec8(v));
          return t;
        }
        case 6: {
          if (y === 6) { const a = this.addrHL(idx); if (idx) t += 5; this.wr(a, this.imm()); return t; }
          this.setR(y, idx, this.imm());
          return t;
        }
        default:
          switch (y) {
            case 0: { const c = this.a >> 7; this.a = ((this.a << 1) | c) & 0xFF; this.f = (this.f & (FS | FZ | FP)) | (this.a & (F3 | F5)) | c; return t; }
            case 1: { const c = this.a & 1; this.a = (this.a >> 1) | (c << 7); this.f = (this.f & (FS | FZ | FP)) | (this.a & (F3 | F5)) | c; return t; }
            case 2: { const c = this.a >> 7; this.a = ((this.a << 1) | (this.f & FC)) & 0xFF; this.f = (this.f & (FS | FZ | FP)) | (this.a & (F3 | F5)) | c; return t; }
            case 3: { const c = this.a & 1; this.a = (this.a >> 1) | ((this.f & FC) << 7); this.f = (this.f & (FS | FZ | FP)) | (this.a & (F3 | F5)) | c; return t; }
            case 4: this.daa(); return t;
            case 5: this.a ^= 0xFF; this.f = (this.f & (FS | FZ | FP | FC)) | (this.a & (F3 | F5)) | FH | FN; return t;
            case 6: this.f = (this.f & (FS | FZ | FP)) | (this.a & (F3 | F5)) | FC; return t;
            default: this.f = ((this.f & (FS | FZ | FP)) | ((this.f & FC) ? FH : 0) | (this.a & (F3 | F5)) | ((this.f & FC) ^ FC)); return t;
          }
      }
    }
    // x === 3
    switch (z) {
      case 0: if (this.cond(y)) { this.pc = this.pop(); this.wz = this.pc; return t + 6; } return t;
      case 1:
        if (!q) {
          const v = this.pop();
          if (p === 3) this.af = v; else this.setRR(p, idx, v);
          return t;
        }
        switch (p) {
          case 0: this.pc = this.pop(); this.wz = this.pc; return t;
          case 1: { // EXX
            let v;
            v = this.b; this.b = this.b2; this.b2 = v; v = this.c; this.c = this.c2; this.c2 = v;
            v = this.d; this.d = this.d2; this.d2 = v; v = this.e; this.e = this.e2; this.e2 = v;
            v = this.h; this.h = this.h2; this.h2 = v; v = this.l; this.l = this.l2; this.l2 = v;
            return t;
          }
          case 2: this.pc = this.getHL(idx); return t;
          default: this.sp = this.getHL(idx); return t;
        }
      case 2: { const a = this.imm16(); this.wz = a; if (this.cond(y)) this.pc = a; return t; }
      case 3:
        switch (y) {
          case 0: this.pc = this.imm16(); this.wz = this.pc; return t;
          case 1: return idx ? this.execIndexCB(idx) : this.execCB();
          case 2: { const n = this.imm(); this.bus.out((this.a << 8) | n, this.a); this.wz = ((n + 1) & 0xFF) | (this.a << 8); return t; }
          case 3: { const n = this.imm(); const port = (this.a << 8) | n; this.a = this.bus.in(port); this.wz = (port + 1) & 0xFFFF; return t; }
          case 4: { // EX (SP),HL
            const v = this.rd16(this.sp); this.wr16(this.sp, this.getHL(idx)); this.setHL(idx, v); this.wz = v; return t;
          }
          case 5: { const v = this.de; this.de = this.hl; this.hl = v; return t; }
          case 6: this.iff1 = this.iff2 = 0; return t;
          default: this.iff1 = this.iff2 = 1; this.eiDelay = true; return t;
        }
      case 4: { const a = this.imm16(); this.wz = a; if (this.cond(y)) { this.push(this.pc); this.pc = a; return t + 7; } return t; }
      case 5:
        if (!q) { this.push(p === 3 ? this.af : this.getRR(p, idx)); return t; }
        switch (p) {
          case 0: { const a = this.imm16(); this.push(this.pc); this.pc = a; this.wz = a; return t; }
          case 1: this.incR(); return this.exec(this.imm(), 1);
          case 2: this.incR(); return this.execED();
          default: this.incR(); return this.exec(this.imm(), 2);
        }
      case 6: this.alu(y, this.imm()); return t;
      default: this.push(this.pc); this.pc = y * 8; this.wz = this.pc; return t;
    }
  }

  execCB() {
    this.incR();
    const op = this.imm();
    const x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    if (z === 6) {
      const a = this.hl, v = this.rd(a);
      if (x === 0) { this.wr(a, this.rot(y, v)); return 15; }
      if (x === 1) { this.bit(y, v, this.wz >> 8); return 12; }
      this.wr(a, x === 2 ? v & ~(1 << y) : v | (1 << y));
      return 15;
    }
    const v = this.getR(z, 0);
    if (x === 0) this.setR(z, 0, this.rot(y, v));
    else if (x === 1) this.bit(y, v, v);
    else this.setR(z, 0, x === 2 ? v & ~(1 << y) : v | (1 << y));
    return 8;
  }

  // DDCB/FDCB: deslocamento vem antes do opcode; o resultado também vai para o registrador (não documentado)
  execIndexCB(idx) {
    const a = ((idx === 1 ? this.ix : this.iy) + this.disp()) & 0xFFFF;
    this.wz = a;
    const op = this.imm();
    const x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    const v = this.rd(a);
    if (x === 1) { this.bit(y, v, a >> 8); return 20; }
    const r = x === 0 ? this.rot(y, v) : x === 2 ? v & ~(1 << y) : v | (1 << y);
    this.wr(a, r);
    if (z !== 6) this.setR(z, 0, r);
    return 23;
  }

  execED() {
    const op = this.imm();
    const x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1, q = y & 1;
    if (x === 1) {
      switch (z) {
        case 0: { // IN r,(C)
          const v = this.bus.in(this.bc); this.wz = (this.bc + 1) & 0xFFFF;
          if (y !== 6) this.setR(y, 0, v);
          this.f = (this.f & FC) | SZ53P[v];
          return 12;
        }
        case 1: this.bus.out(this.bc, y === 6 ? 0 : this.getR(y, 0)); this.wz = (this.bc + 1) & 0xFFFF; return 12;
        case 2: if (q) this.adc16(this.getRR(p, 0)); else this.sbc16(this.getRR(p, 0)); return 15;
        case 3: {
          const a = this.imm16(); this.wz = (a + 1) & 0xFFFF;
          if (q) this.setRR(p, 0, this.rd16(a)); else this.wr16(a, this.getRR(p, 0));
          return 20;
        }
        case 4: { const v = this.a; this.a = 0; this.alu(2, v); return 8; } // NEG
        case 5: this.pc = this.pop(); this.wz = this.pc; this.iff1 = this.iff2; return 14; // RETN/RETI
        case 6: this.im = [0, 0, 1, 2, 0, 0, 1, 2][y]; return 8;
        default:
          switch (y) {
            case 0: this.i = this.a; return 9;
            case 1: this.r = this.a; return 9;
            case 2: this.a = this.i; this.f = (this.f & FC) | SZ53[this.a] | (this.iff2 ? FP : 0); return 9;
            case 3: this.a = this.r; this.f = (this.f & FC) | SZ53[this.a] | (this.iff2 ? FP : 0); return 9;
            case 4: { // RRD
              const a = this.hl, v = this.rd(a);
              this.wr(a, ((this.a << 4) | (v >> 4)) & 0xFF);
              this.a = (this.a & 0xF0) | (v & 0x0F);
              this.f = (this.f & FC) | SZ53P[this.a]; this.wz = (a + 1) & 0xFFFF;
              return 18;
            }
            case 5: { // RLD
              const a = this.hl, v = this.rd(a);
              this.wr(a, ((v << 4) | (this.a & 0x0F)) & 0xFF);
              this.a = (this.a & 0xF0) | (v >> 4);
              this.f = (this.f & FC) | SZ53P[this.a]; this.wz = (a + 1) & 0xFFFF;
              return 18;
            }
            default: return 8;
          }
      }
    }
    if (x === 2 && z <= 3 && y >= 4) return this.block(y, z);
    return 8; // NOP estendido
  }

  // LDI/LDD/LDIR/LDDR, CPI…, INI…, OUTI…
  block(y, z) {
    const dec = y & 1, rep = y >= 6;
    const dir = dec ? -1 : 1;
    if (z === 0) { // LD
      const v = this.rd(this.hl);
      this.wr(this.de, v);
      this.hl = this.hl + dir; this.de = this.de + dir; this.bc = this.bc - 1;
      const n = (v + this.a) & 0xFF;
      this.f = (this.f & (FS | FZ | FC)) | (this.bc ? FP : 0) | (n & F3) | ((n & 0x02) << 4);
      if (rep && this.bc) { this.pc = (this.pc - 2) & 0xFFFF; this.wz = (this.pc + 1) & 0xFFFF; return 21; }
      return 16;
    }
    if (z === 1) { // CP
      const v = this.rd(this.hl), r = (this.a - v) & 0xFF;
      const h = (this.a ^ v ^ r) & FH;
      this.hl = this.hl + dir; this.bc = this.bc - 1;
      const n = (r - (h ? 1 : 0)) & 0xFF;
      this.f = (this.f & FC) | FN | (SZ53[r] & (FS | FZ)) | h | (this.bc ? FP : 0) | (n & F3) | ((n & 0x02) << 4);
      this.wz = (this.wz + dir) & 0xFFFF;
      if (rep && this.bc && r !== 0) { this.pc = (this.pc - 2) & 0xFFFF; this.wz = (this.pc + 1) & 0xFFFF; return 21; }
      return 16;
    }
    if (z === 2) { // IN
      const v = this.bus.in(this.bc);
      this.wz = (this.bc + dir) & 0xFFFF;
      this.wr(this.hl, v);
      this.b = (this.b - 1) & 0xFF; this.hl = this.hl + dir;
      const k = v + ((this.c + dir) & 0xFF);
      this.f = SZ53[this.b] | ((v & 0x80) ? FN : 0) | (k > 255 ? (FH | FC) : 0) | PAR[(k & 7) ^ this.b];
      if (rep && this.b) { this.pc = (this.pc - 2) & 0xFFFF; return 21; }
      return 16;
    }
    // OUT
    const v = this.rd(this.hl);
    this.b = (this.b - 1) & 0xFF;
    this.wz = (this.bc + dir) & 0xFFFF;
    this.bus.out(this.bc, v);
    this.hl = this.hl + dir;
    const k = v + this.l;
    this.f = SZ53[this.b] | ((v & 0x80) ? FN : 0) | (k > 255 ? (FH | FC) : 0) | PAR[(k & 7) ^ this.b];
    if (rep && this.b) { this.pc = (this.pc - 2) & 0xFFFF; return 21; }
    return 16;
  }

  // ------------------------------------------------------------ save state
  state() {
    return ['a', 'f', 'b', 'c', 'd', 'e', 'h', 'l', 'a2', 'f2', 'b2', 'c2', 'd2', 'e2', 'h2', 'l2', 'ix', 'iy', 'sp', 'pc',
      'i', 'r', 'wz', 'iff1', 'iff2', 'im', 'halted', 'eiDelay', 'irqLine', 'nmiPending'].map(k => +this[k]);
  }
  restore(s) {
    const k = ['a', 'f', 'b', 'c', 'd', 'e', 'h', 'l', 'a2', 'f2', 'b2', 'c2', 'd2', 'e2', 'h2', 'l2', 'ix', 'iy', 'sp', 'pc',
      'i', 'r', 'wz', 'iff1', 'iff2', 'im', 'halted', 'eiDelay', 'irqLine', 'nmiPending'];
    k.forEach((n, j) => { this[n] = s[j]; });
    this.halted = !!this.halted; this.eiDelay = !!this.eiDelay; this.irqLine = !!this.irqLine; this.nmiPending = !!this.nmiPending;
  }
}
