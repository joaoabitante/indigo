// CPU Toshiba TLCS-900/H (Neo Geo Pocket): 4 bancos de XWA/XBC/XDE/XHL + XIX/XIY/XIZ/XSP de 32 bits,
// operações de 8/16/32 bits, modos de endereçamento com prefixo (fonte, destino, registrador) e
// registradores de controle do micro-DMA. step() executa uma instrução e devolve os estados gastos.

const FS = 0x80, FZ = 0x40, FH = 0x10, FV = 0x04, FN = 0x02, FC = 0x01;
const MASK = [0, 0xFF, 0xFFFF, 0, 0xFFFFFFFF];
const SIGN = [0, 0x80, 0x8000, 0, 0x80000000];
const PAR = new Uint8Array(256);
for (let v = 0; v < 256; v++) { let p = v; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1; PAR[v] = (p & 1) ? 0 : FV; }
const parity16 = (v) => (PAR[v & 0xFF] === PAR[(v >> 8) & 0xFF] ? FV : 0);

// código de registrador curto → código completo (banco atual)
// estados extras por modo de endereçamento (folha de dados)
const MODE_X = { r: 0, rd8: 2, n: 2, nn: 2, nnn: 3, r32: 3, r32d16: 5, r32r8: 8, r32r16: 8, pre: 3, post: 3, ldar: 5 };
// ciclos das operações sobre registrador (byte/palavra no byte baixo, 32 bits no alto); 0 = calculado no handler
const REG_CYC = new Uint16Array(256);
{
  const set = (ops, bw, l = bw) => { for (const o of ops) REG_CYC[o] = bw | (l << 8); };
  const rg = (a, n) => Array.from({ length: n }, (_, i) => a + i);
  set([0x03], 4, 6); set([0x04], 5, 7); set([0x05], 6, 8); set([0x06], 4); set([0x07], 5);
  set([0x0C], 10); set([0x0D], 8); set([0x0E, 0x0F], 4); set([0x10], 6); set([0x12], 4); set([0x13], 5); set([0x14, 0x16], 4); set([0x19], 31);
  set([...rg(0x20, 5), ...rg(0x28, 5)], 4); set([0x2E, 0x2F], 8); set(rg(0x30, 4), 4); set([0x34], 6);
  set(rg(0x38, 3), 8); set(rg(0x3C, 3), 7); set(rg(0x60, 8), 4); set(rg(0x68, 8), 4, 5); set(rg(0x70, 16), 6);
  for (const b of [0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0]) set(rg(b, 8), 4, 7);
  set([...rg(0x88, 8), ...rg(0x98, 8), ...rg(0xA8, 8), ...rg(0xD8, 8)], 4); set(rg(0xB8, 8), 5); set(rg(0xC8, 8), 4, 7);
}
const MD_CYC = [18, 18, 22, 24]; // MUL MULS DIV DIVS (8 bits; +8 em 16 bits)
const BREG = [0xE1, 0xE0, 0xE5, 0xE4, 0xE9, 0xE8, 0xED, 0xEC]; // W A B C D E H L
const WREG = [0xE0, 0xE4, 0xE8, 0xEC, 0xF0, 0xF4, 0xF8, 0xFC]; // WA BC DE HL IX IY IZ SP

export class TLCS900H {
  constructor(bus) {
    this.bus = bus; // read8/16/32(a), write8/16/32(a, v)
    this.r = new Uint32Array(21); // [banco*4 + n], 16..19 = XIX XIY XIZ XSP, 20 = código inválido (descarte)
    this.dmaS = new Uint32Array(4); this.dmaD = new Uint32Array(4); this.dmaC = new Uint16Array(4); this.dmaM = new Uint8Array(4);
    this.reset();
  }

  reset() {
    this.r.fill(0);
    this.pc = 0; this.sr = 0xF800; this.fd = 0;
    this.intNest = 0; this.halted = false;
    this.dmaS.fill(0); this.dmaD.fill(0); this.dmaC.fill(0); this.dmaM.fill(0);
  }

  get rfp() { return (this.sr >> 8) & 3; }
  get iff() { return (this.sr >> 12) & 7; }
  get f() { return this.sr & 0xFF; }
  set f(v) { this.sr = (this.sr & 0xFF28) | (v & 0xD7); } // bits 5 e 3 não são afetados pelas operações
  setF(v) { this.sr = (this.sr & 0xFF00) | (v & 0xFF); }

  // ------------------------------------------------------------ registradores por código completo
  idx(c) {
    if (c < 0x40) return c >> 2;                                    // bancos 0–3 explícitos
    if (c >= 0xF0) return 16 + ((c >> 2) & 3);                      // XIX XIY XIZ XSP
    if (c >= 0xE0) return this.rfp * 4 + ((c >> 2) & 3);           // banco atual
    if (c >= 0xD0) return this.rfp ? (this.rfp - 1) * 4 + ((c >> 2) & 3) : 20; // banco anterior (no banco 0 não existe)
    return 20;
  }
  getB(c) { const i = this.idx(c); return (this.r[i] >>> (i === 20 ? 0 : (c & 3) << 3)) & 0xFF; }
  setB(c, v) { const i = this.idx(c); const s = i === 20 ? 0 : (c & 3) << 3; this.r[i] = (this.r[i] & ~(0xFF << s)) | ((v & 0xFF) << s); }
  getW(c) { const i = this.idx(c); return (c & 2) && i !== 20 ? this.r[i] >>> 16 : this.r[i] & 0xFFFF; }
  setW(c, v) { const i = this.idx(c); if ((c & 2) && i !== 20) this.r[i] = (this.r[i] & 0xFFFF) | ((v & 0xFFFF) << 16); else this.r[i] = (this.r[i] & 0xFFFF0000) | (v & 0xFFFF); }
  getL(c) { const i = this.idx(c); return i < 0 ? 0 : this.r[i]; }
  setL(c, v) { const i = this.idx(c); if (i >= 0) this.r[i] = v >>> 0; }
  getR(c, sz) { return sz === 1 ? this.getB(c) : sz === 2 ? this.getW(c) : this.getL(c); }
  setR(c, sz, v) { if (sz === 1) this.setB(c, v); else if (sz === 2) this.setW(c, v); else this.setL(c, v); }

  get xsp() { return this.r[19]; } set xsp(v) { this.r[19] = v >>> 0; }

  // ------------------------------------------------------------ memória
  rd(a, sz) { a &= 0xFFFFFF; return sz === 1 ? this.bus.read8(a) : sz === 2 ? this.bus.read16(a) : this.bus.read32(a) >>> 0; }
  wr(a, sz, v) { a &= 0xFFFFFF; if (sz === 1) this.bus.write8(a, v & 0xFF); else if (sz === 2) this.bus.write16(a, v & 0xFFFF); else this.bus.write32(a, v >>> 0); }
  f8() { const v = this.bus.read8(this.pc & 0xFFFFFF); this.pc = (this.pc + 1) & 0xFFFFFF; return v; }
  f16() { const v = this.bus.read16(this.pc & 0xFFFFFF); this.pc = (this.pc + 2) & 0xFFFFFF; return v; }
  f24() { const lo = this.f16(); return lo | (this.f8() << 16); }
  f32() { const v = this.bus.read32(this.pc & 0xFFFFFF) >>> 0; this.pc = (this.pc + 4) & 0xFFFFFF; return v; }
  fimm(sz) { return sz === 1 ? this.f8() : sz === 2 ? this.f16() : this.f32(); }
  push(sz, v) { this.xsp = this.xsp - sz; this.wr(this.xsp, sz, v); }
  pop(sz) { const v = this.rd(this.xsp, sz); this.xsp = this.xsp + sz; return v; }

  cond(cc) {
    const f = this.sr;
    const s = (f >> 7) & 1, z = (f >> 6) & 1, v = (f >> 2) & 1, c = f & 1;
    switch (cc & 15) {
      case 0: return false; case 1: return (s ^ v) === 1; case 2: return ((s ^ v) | z) === 1; case 3: return (c | z) === 1;
      case 4: return v === 1; case 5: return s === 1; case 6: return z === 1; case 7: return c === 1;
      case 8: return true; case 9: return (s ^ v) === 0; case 10: return ((s ^ v) | z) === 0; case 11: return (c | z) === 0;
      case 12: return v === 0; case 13: return s === 0; case 14: return z === 0; default: return c === 0;
    }
  }

  // ------------------------------------------------------------ ULA
  szFlags(v, sz) { return ((v & SIGN[sz]) ? FS : 0) | (((v & MASK[sz]) >>> 0) === 0 ? FZ : 0); }
  add(a, b, sz, carry) {
    const m = MASK[sz], c = carry ? (this.sr & FC) : 0;
    const r = sz === 4 ? (a + b + c) : (a + b + c);
    const res = (r & m) >>> 0;
    let f = this.szFlags(res, sz);
    if (sz < 4 ? r > m : r > 0xFFFFFFFF) f |= FC;
    if (((a ^ ~b) & (a ^ res) & SIGN[sz]) !== 0) f |= FV;
    if (sz !== 4 ? ((a ^ b ^ res) & 0x10) : (this.sr & FH)) f |= FH;
    this.f = f;
    return res;
  }
  sub(a, b, sz, carry, store = true) {
    const m = MASK[sz], c = carry ? (this.sr & FC) : 0;
    const r = a - b - c;
    const res = (r & m) >>> 0;
    let f = this.szFlags(res, sz) | FN;
    if (r < 0) f |= FC;
    if (((a ^ b) & (a ^ res) & SIGN[sz]) !== 0) f |= FV;
    if (sz !== 4 ? ((a ^ b ^ res) & 0x10) : (this.sr & FH)) f |= FH;
    this.f = f;
    return res;
  }
  logic(op, a, b, sz) {
    let res = op === 0 ? a & b : op === 1 ? a ^ b : a | b;
    res = (res & MASK[sz]) >>> 0;
    let f = this.szFlags(res, sz) | (op === 0 ? FH : 0);
    if (sz === 1) f |= PAR[res]; else if (sz === 2) f |= parity16(res); else f |= this.sr & FV;
    this.f = f;
    return res;
  }
  // operação aritmética/lógica por número (0 ADD,1 ADC,2 SUB,3 SBC,4 AND,5 XOR,6 OR,7 CP)
  alu(op, a, b, sz) {
    switch (op) {
      case 0: return this.add(a, b, sz, false);
      case 1: return this.add(a, b, sz, true);
      case 2: return this.sub(a, b, sz, false);
      case 3: return this.sub(a, b, sz, true);
      case 4: return this.logic(0, a, b, sz);
      case 5: return this.logic(1, a, b, sz);
      case 6: return this.logic(2, a, b, sz);
      default: this.sub(a, b, sz, false); return a;
    }
  }
  inc(v, n, sz, flags) {
    const res = ((v + n) & MASK[sz]) >>> 0;
    if (flags) {
      let f = (this.sr & FC) | this.szFlags(res, sz);
      if (((v ^ res) & ~(v ^ n) & SIGN[sz]) !== 0 && ((res & SIGN[sz]) !== (v & SIGN[sz]))) f |= FV;
      if (sz === 1 && ((v ^ n ^ res) & 0x10)) f |= FH;
      if (sz === 2 && ((v ^ n ^ res) & 0x10)) f |= FH;
      this.f = f;
    }
    return res;
  }
  dec(v, n, sz, flags) {
    const res = ((v - n) & MASK[sz]) >>> 0;
    if (flags) {
      let f = (this.sr & FC) | this.szFlags(res, sz) | FN;
      if (((v ^ n) & (v ^ res) & SIGN[sz]) !== 0) f |= FV;
      if (sz !== 4 && ((v ^ n ^ res) & 0x10)) f |= FH;
      this.f = f;
    }
    return res;
  }
  // rotações/deslocamentos: op 0 RLC,1 RRC,2 RL,3 RR,4 SLA,5 SRA,6 SLL,7 SRL
  shift(op, v, n, sz) {
    const bits = sz * 8, m = MASK[sz], sign = SIGN[sz];
    let c = this.sr & FC;
    v = (v & m) >>> 0;
    for (let k = 0; k < n; k++) {
      switch (op) {
        case 0: c = (v & sign) ? 1 : 0; v = ((v << 1) | c) & m; break;
        case 1: c = v & 1; v = (v >>> 1) | (c ? sign : 0); break;
        case 2: { const nc = (v & sign) ? 1 : 0; v = ((v << 1) | c) & m; c = nc; break; }
        case 3: { const nc = v & 1; v = (v >>> 1) | (c ? sign : 0); c = nc; break; }
        case 4: case 6: c = (v & sign) ? 1 : 0; v = (v << 1) & m; break;
        case 5: c = v & 1; v = (v >>> 1) | (v & sign); break;
        default: c = v & 1; v = v >>> 1; break;
      }
      v = v >>> 0;
    }
    let f = this.szFlags(v, sz) | c;
    if (sz === 1) f |= PAR[v]; else if (sz === 2) f |= parity16(v);
    else f |= this.sr & FV; // 32 bits: a paridade não é calculada (V fica como estava)
    this.f = f;
    void bits;
    return v >>> 0;
  }

  // ------------------------------------------------------------ execução
  step() {
    if (this.halted) return 8;
    this.cycles = 0;
    this.op2 = -1; this.grp = 'main'; this.mode = ''; this.sz = 0;
    const op = this.f8();
    this.exec(op);
    return this.cycles;
  }

  exec(op) {
    if (op >= 0x80) { this.prefixed(op); return; }
    const hi = op >> 3, n = op & 7;
    switch (hi) {
      case 0x00: case 0x01: case 0x02: case 0x03: this.single(op); return;
      case 0x04: this.setB(BREG[n], this.f8()); this.cycles = 2; return;               // LD R,n
      case 0x05: this.push(2, this.getW(WREG[n])); this.cycles = 3; return;           // PUSH RR
      case 0x06: this.setW(WREG[n], this.f16()); this.cycles = 3; return;             // LD RR,nn
      case 0x07: this.push(4, this.getL(WREG[n])); this.cycles = 5; return;           // PUSH XRR
      case 0x08: this.setL(WREG[n], this.f32()); this.cycles = 5; return;            // LD XRR,nnnn
      case 0x09: this.setW(WREG[n], this.pop(2)); this.cycles = 4; return;            // POP RR
      case 0x0A: this.cycles = 2; return;                                              // (indefinido)
      case 0x0B: this.setL(WREG[n], this.pop(4)); this.cycles = 6; return;            // POP XRR
      case 0x0C: case 0x0D: {                                                          // JR cc,d
        const d = (this.f8() << 24) >> 24;
        if (this.cond(op & 15)) { this.pc = (this.pc + d) & 0xFFFFFF; this.cycles = 8; } else this.cycles = 4;
        return;
      }
      default: {                                                                       // JRL cc,dd
        const d = (this.f16() << 16) >> 16;
        if (this.cond(op & 15)) { this.pc = (this.pc + d) & 0xFFFFFF; this.cycles = 8; } else this.cycles = 4;
      }
    }
  }

  single(op) {
    switch (op) {
      case 0x00: this.cycles = 2; return;                                        // NOP
      case 0x01: this.cycles = 4; return;                                        // NORMAL (900H: nada)
      case 0x02: this.push(2, this.sr); this.cycles = 4; return;                 // PUSH SR
      case 0x03: this.sr = this.pop(2); this.cycles = 6; return;                 // POP SR
      case 0x04: this.cycles = 4; return;                                        // MAX (nada)
      case 0x05: this.halted = true; this.cycles = 8; return;                    // HALT
      case 0x06: { const n = this.f8() & 7; this.sr = (this.sr & 0x8FFF) | (n << 12); this.cycles = 5; return; } // EI n
      case 0x07: {                                                                // RETI
        this.sr = this.pop(2);
        this.pc = this.pop(4) & 0xFFFFFF;
        this.intNest = (this.intNest - 1) & 0xFFFF;
        this.cycles = 12; return;
      }
      case 0x08: { const a = this.f8(); this.wr(a, 1, this.f8()); this.cycles = 5; return; }   // LD (n),n
      case 0x09: this.push(1, this.f8()); this.cycles = 4; return;                              // PUSH n
      case 0x0A: { const a = this.f8(); this.wr(a, 2, this.f16()); this.cycles = 6; return; }  // LDW (n),nn
      case 0x0B: this.push(2, this.f16()); this.cycles = 5; return;                             // PUSHW nn
      case 0x0C: this.sr = (this.sr & 0xF8FF) | (((this.rfp + 1) & 3) << 8); this.cycles = 2; return; // INCF
      case 0x0D: this.sr = (this.sr & 0xF8FF) | (((this.rfp - 1) & 3) << 8); this.cycles = 2; return; // DECF
      case 0x0E: this.pc = this.pop(4) & 0xFFFFFF; this.cycles = 9; return;                    // RET
      case 0x0F: { const d = (this.f16() << 16) >> 16; this.pc = this.pop(4) & 0xFFFFFF; this.xsp = this.xsp + d; this.cycles = 9; return; } // RETD
      case 0x10: this.f = this.f & ~(FN | FV | FC); this.cycles = 2; return;                   // RCF
      case 0x11: this.f = (this.f & ~(FH | FN)) | FC; this.cycles = 2; return;                 // SCF
      case 0x12: this.f = (this.f & ~FN) ^ FC; this.cycles = 2; return;                        // CCF
      case 0x13: { const z = (this.f & FZ) ? 1 : 0; this.f = (this.f & ~(FN | FC)) | (z ^ 1); this.cycles = 2; return; } // ZCF
      case 0x14: this.push(1, this.getB(0xE0)); this.cycles = 3; return;                       // PUSH A
      case 0x15: this.setB(0xE0, this.pop(1)); this.cycles = 4; return;                        // POP A
      case 0x16: { const t = this.f; this.setF(this.fd); this.fd = t; this.cycles = 2; return; } // EX F,F'
      case 0x17: { const n = this.f8() & 3; this.sr = (this.sr & 0xF8FF) | (n << 8); this.cycles = 2; return; } // LDF n
      case 0x18: this.push(1, this.f); this.cycles = 3; return;                                // PUSH F
      case 0x19: this.setF(this.pop(1)); this.cycles = 4; return;                                // POP F
      case 0x1A: this.pc = this.f16(); this.cycles = 7; return;                                // JP nn
      case 0x1B: this.pc = this.f24(); this.cycles = 7; return;                                // JP nnn
      case 0x1C: { const a = this.f16(); this.push(4, this.pc); this.pc = a; this.cycles = 12; return; } // CALL nn
      case 0x1D: { const a = this.f24(); this.push(4, this.pc); this.pc = a; this.cycles = 12; return; } // CALL nnn
      case 0x1E: { const d = (this.f16() << 16) >> 16; this.push(4, this.pc); this.pc = (this.pc + d) & 0xFFFFFF; this.cycles = 12; return; } // CALR
      default: this.cycles = 2; // 0x1F: chamada da BIOS própria (tratada pelo sistema)
        if (this.onBios) this.onBios();
    }
  }

  // ------------------------------------------------------------ instruções com prefixo
  prefixed(op) {
    if (op >= 0xF8) { // SWI n
      const n = op & 7;
      this.push(4, this.pc); this.push(2, this.sr);
      this.pc = this.rd(0xFFFF00 + n * 4, 4) & 0xFFFFFF;
      this.cycles = 16; return;
    }
    const grp = op >> 4;
    // prefixos de registrador
    this.grp = 'reg'; this.mode = 'short';
    if ((op & 0xF8) === 0xC8) { this.sz = 1; return this.regOp(BREG[op & 7], 1, op); }
    if ((op & 0xF8) === 0xD8) { this.sz = 2; return this.regOp(WREG[op & 7], 2, op); }
    if ((op & 0xF8) === 0xE8) { this.sz = 4; return this.regOp(WREG[op & 7], 4, op); }
    this.mode = 'ext';
    if (op === 0xC7) { this.sz = 1; this.regOp(this.f8(), 1, op); this.cycles++; return; }
    if (op === 0xD7) { this.sz = 2; this.regOp(this.f8(), 2, op); this.cycles++; return; }
    if (op === 0xE7) { this.sz = 4; this.regOp(this.f8(), 4, op); this.cycles++; return; }
    if (op === 0xF7) { // LDX (n),#
      this.f8(); const a = this.f8(); this.f8(); const v = this.f8(); this.f8();
      this.wr(a, 1, v); this.cycles = 8; return;
    }
    // memória: fonte (80–AF, C0–C5, D0–D5, E0–E5) ou destino (B0–BF, F0–F5)
    let sz, dst = false, addr;
    if (op < 0xC0) {
      const xr = WREG[op & 7];
      if (grp >= 0xB) dst = true;
      sz = grp === 8 ? 1 : grp === 9 ? 2 : 4;
      addr = this.getL(xr);
      this.mode = 'r';
      if (op & 8) { addr = addr + ((this.f8() << 24) >> 24); this.mode = 'rd8'; }
    } else {
      const mode = op & 7;
      sz = grp === 0xC ? 1 : grp === 0xD ? 2 : 4;
      if (grp === 0xF) dst = true;
      addr = this.memMode(mode, sz);
    }
    addr = addr & 0xFFFFFF;
    this.grp = dst ? 'dst' : 'src'; this.sz = sz;
    this.cycles = 0;
    if (dst) this.dstOp(addr); else this.srcOp(addr, sz, op);
    this.cycles += MODE_X[this.mode] || 0;
  }

  // endereço dos modos C0–C5 (lê os bytes de endereço; aplica pré-decremento/pós-incremento)
  memMode(mode, sz) {
    switch (mode) {
      case 0: this.mode = 'n'; return this.f8();
      case 1: this.mode = 'nn'; return this.f16();
      case 2: this.mode = 'nnn'; return this.f24();
      case 3: {
        const b = this.f8();
        switch (b & 3) {
          case 0: this.mode = 'r32'; return this.getL(b);
          case 1: this.mode = 'r32d16'; return this.getL(b & 0xFC) + ((this.f16() << 16) >> 16);
          default: {
            if (b === 0x03) { this.mode = 'r32r8'; const rl = this.f8(); const rb = this.f8(); return this.getL(rl) + ((this.getB(rb) << 24) >> 24); }
            if (b === 0x07) { this.mode = 'r32r16'; const rl = this.f8(); const rw = this.f8(); return this.getL(rl) + ((this.getW(rw) << 16) >> 16); }
            if (b === 0x13) { this.mode = 'ldar'; const d = (this.f16() << 16) >> 16; return this.pc + d; } // LDAR
            return 0;
          }
        }
      }
      case 4: { // (-r32)
        this.mode = 'pre';
        const b = this.f8(), step = [1, 2, 4, 0][b & 3];
        const v = (this.getL(b & 0xFC) - step) >>> 0;
        this.setL(b & 0xFC, v);
        return v;
      }
      case 5: { // (r32+)
        this.mode = 'post';
        const b = this.f8(), step = [1, 2, 4, 0][b & 3];
        const v = this.getL(b & 0xFC);
        this.setL(b & 0xFC, v + step);
        return v;
      }
      default: return 0;
    }
  }

  // ------------------------------------------------------------ operações com fonte em memória
  srcOp(a, sz, prefix) {
    const op = this.f8();
    this.op2 = op;
    const R = op & 7;
    const rcode = sz === 1 ? BREG[R] : WREG[R];
    if (op >= 0x80) { // ALU R,(mem) / ALU (mem),R
      const alu = (op >> 4) & 7, toMem = (op & 8) !== 0;
      const m = this.rd(a, sz);
      if (!toMem) { const res = this.alu(alu, this.getR(rcode, sz), m, sz); if (alu !== 7) this.setR(rcode, sz, res); this.cycles += sz === 4 ? 6 : 4; }
      else { const res = this.alu(alu, m, this.getR(rcode, sz), sz); if (alu !== 7) this.wr(a, sz, res); this.cycles += sz === 4 && alu !== 7 ? 10 : 6; }
      return;
    }
    if (op >= 0x78) { // RLC…SRL (mem), 1 bit
      if (sz === 4) { this.cycles += 6; return; }          // não existe em 32 bits
      const hn = this.sr & (FH | FN), k = op & 7;
      const v = this.shift(k, this.rd(a, sz), 1, sz);
      if (k === 2 || k === 3) this.f = (this.f & ~(FH | FN)) | hn; // RL/RR (mem) não mexem em H e N
      this.wr(a, sz, v); this.cycles += 8; return;
    }
    if (op >= 0x60) { // INC/DEC #3,(mem)
      if (sz === 4) { this.cycles += 2; return; }
      let n = R || 8;
      const v = this.rd(a, sz);
      this.wr(a, sz, op < 0x68 ? this.inc(v, n, sz, true) : this.dec(v, n, sz, true));
      this.cycles += 6; return;
    }
    if (op >= 0x40) { // MUL/MULS/DIV/DIVS RR,(mem)
      const kind = (op >> 3) & 3;
      if (sz === 4 || (sz === 1 && !(R & 1))) { this.cycles += 2; return; }
      this.muldiv(kind, sz === 1 ? WREG[R >> 1] : WREG[R], this.rd(a, sz), sz);
      this.cycles += MD_CYC[kind] + (sz === 2 ? 8 : 0); return;
    }
    if (op >= 0x30 && sz === 4) { this.cycles += 2; return; } // ALU (mem),# e EX não existem em 32 bits
    if (op >= 0x38) { // ALU (mem),#
      const imm = this.fimm(sz);
      const alu = op & 7;
      const res = this.alu(alu, this.rd(a, sz), imm, sz);
      if (alu !== 7) this.wr(a, sz, res);
      this.cycles += alu === 7 ? 6 : sz === 1 ? 7 : 8; return;
    }
    if (op >= 0x30) { // EX (mem),R
      const m = this.rd(a, sz), r = this.getR(rcode, sz);
      this.wr(a, sz, r); this.setR(rcode, sz, m); this.cycles += 6; return;
    }
    if (op >= 0x20) { // LD R,(mem)
      this.setR(rcode, sz, this.rd(a, sz)); this.cycles += sz === 4 ? 6 : 4; return;
    }
    switch (op) {
      case 0x04: if (sz !== 4) this.push(sz, this.rd(a, sz)); this.cycles += 7; return;  // PUSH (mem)
      case 0x06: case 0x07: {                                                              // RLD/RRD A,(mem)
        const m = this.rd(a, 1), A = this.getB(0xE0);
        let nm, na;
        if (op === 0x06) { nm = ((m << 4) | (A & 0xF)) & 0xFF; na = (A & 0xF0) | (m >> 4); }
        else { nm = ((A << 4) | (m >> 4)) & 0xFF; na = (A & 0xF0) | (m & 0xF); }
        this.wr(a, 1, nm); this.setB(0xE0, na);
        this.f = (this.f & FC) | this.szFlags(na, 1) | PAR[na];
        this.cycles += 12; return;
      }
      case 0x10: case 0x11: case 0x12: case 0x13: if (sz === 4) { this.cycles += 2; return; } this.blockLd(prefix, sz, op); return;
      case 0x14: case 0x15: case 0x16: case 0x17: if (sz === 4) { this.cycles += 2; return; } this.blockCp(prefix, sz, op); return;
      case 0x19: { if (sz === 4) { this.cycles += 2; return; } const d = this.f16(); this.wr(d, sz, this.rd(a, sz)); this.cycles += 8; return; } // LD (nn),(mem)
      default: this.cycles += 2;
    }
  }

  blockLd(prefix, sz, op) {
    // LDI/LDIR/LDD/LDDR: (XDE),(XHL) ou (XIX),(XIY) conforme o registrador do prefixo
    const ix = (prefix & 7) === 5;
    const dc = ix ? 0xF0 : 0xE8, sc = ix ? 0xF4 : 0xEC;
    const dir = (op & 2) ? -sz : sz;
    let n = 0;
    do {
      const d = this.getL(dc), s = this.getL(sc);
      this.wr(d, sz, this.rd(s, sz));
      this.setL(dc, d + dir); this.setL(sc, s + dir);
      const bc = (this.getW(0xE4) - 1) & 0xFFFF;
      this.setW(0xE4, bc);
      n++;
      if (bc === 0) break;
    } while (op & 1);
    this.f = (this.f & ~(FH | FV | FN)) | (this.getW(0xE4) ? FV : 0);
    this.cycles += 10 + (n - 1) * 14;
  }

  blockCp(prefix, sz, op) {
    // CPI/CPIR/CPD/CPDR: compara A (ou WA) com (XHL) — registrador do prefixo
    const rc = WREG[prefix & 7];
    const dir = (op & 2) ? -sz : sz;
    const acc = sz === 1 ? this.getB(0xE0) : this.getW(0xE0);
    let n = 0, eq = false;
    do {
      const a = this.getL(rc);
      const m = this.rd(a, sz);
      this.setL(rc, a + dir);
      const bc = (this.getW(0xE4) - 1) & 0xFFFF;
      this.setW(0xE4, bc);
      const c = this.f & FC;
      this.sub(acc, m, sz, false);
      this.f = (this.f & ~(FV | FC)) | c | (bc ? FV : 0);
      eq = (this.f & FZ) !== 0;
      n++;
      if (bc === 0 || eq) break;
    } while (op & 1);
    this.cycles += 10 + (n - 1) * 14;
  }

  // ------------------------------------------------------------ operações com destino em memória
  dstOp(a) {
    const op = this.f8();
    this.op2 = op;
    const R = op & 7;
    if (op >= 0xF0) { if (this.cond(op & 15)) { this.pc = this.pop(4) & 0xFFFFFF; this.cycles += 12; } else this.cycles += 6; return; } // RET cc (forma F?)
    if (op >= 0xE0) { if (this.cond(op & 15)) { this.push(4, this.pc); this.pc = a; this.cycles += 12; } else this.cycles += 6; return; } // CALL cc,mem
    if (op >= 0xD0) { if (this.cond(op & 15)) { this.pc = a; this.cycles += 8; } else this.cycles += 6; return; } // JP cc,mem
    if (op >= 0x80) { // bits em memória (byte)
      const b = R, kind = (op >> 3) & 15;
      const m = this.rd(a, 1);
      switch (kind) {
        case 0: this.f = (this.f & ~FC) | ((this.f & FC) & ((m >> b) & 1)); break;           // ANDCF
        case 1: this.f = this.f | ((m >> b) & 1); break;                                      // ORCF
        case 2: this.f = this.f ^ ((m >> b) & 1); break;                                      // XORCF
        case 3: this.f = (this.f & ~FC) | ((m >> b) & 1); break;                              // LDCF
        case 4: this.wr(a, 1, (this.f & FC) ? m | (1 << b) : m & ~(1 << b)); break;          // STCF
        case 5: this.f = (this.f & ~(FZ | FN)) | FH | ((m >> b) & 1 ? 0 : FZ); this.wr(a, 1, m | (1 << b)); break; // TSET
        case 6: this.wr(a, 1, m & ~(1 << b)); break;                                          // RES
        case 7: this.wr(a, 1, m | (1 << b)); break;                                           // SET
        case 8: this.wr(a, 1, m ^ (1 << b)); break;                                           // CHG
        case 9: this.f = (this.f & ~(FZ | FN)) | FH | ((m >> b) & 1 ? 0 : FZ); break;        // BIT
      }
      this.cycles += kind === 5 ? 10 : 8; return;
    }
    if (op >= 0x60 && op < 0x68) { this.wr(a, 4, this.getL(WREG[R])); this.cycles += 6; return; } // LD (mem),XRR
    if (op >= 0x50 && op < 0x58) { this.wr(a, 2, this.getW(WREG[R])); this.cycles += 4; return; } // LD (mem),RR
    if (op >= 0x40 && op < 0x48) { this.wr(a, 1, this.getB(BREG[R])); this.cycles += 4; return; } // LD (mem),R
    if (op >= 0x30 && op < 0x38) { this.setL(WREG[R], a); this.cycles += 4; return; }              // LDA XRR,mem
    if (op >= 0x20 && op < 0x28) { this.setW(WREG[R], a); this.cycles += 4; return; }              // LDA RR,mem
    if (op >= 0x28 && op < 0x2D) { // ANDCF…STCF A,(mem)
      const b = this.getB(0xE0) & 15;
      this.cycles += 8;
      if (b >= 8) { if (op < 0x2B) this.rd(a, 1); return; } // bit fora do byte: nada muda
      const m = this.rd(a, 1);
      switch (op) {
        case 0x28: this.f = (this.f & ~FC) | ((this.f & FC) & ((m >> b) & 1)); break;
        case 0x29: this.f = this.f | ((m >> b) & 1); break;
        case 0x2A: this.f = this.f ^ ((m >> b) & 1); break;
        case 0x2B: this.f = (this.f & ~FC) | ((m >> b) & 1); break;
        case 0x2C: this.wr(a, 1, (this.f & FC) ? m | (1 << b) : m & ~(1 << b)); break;
      }
      return;
    }
    switch (op) {
      case 0x00: this.wr(a, 1, this.f8()); this.cycles += 5; return;                     // LD (mem),#8
      case 0x02: this.wr(a, 2, this.f16()); this.cycles += 6; return;                    // LDW (mem),#16
      case 0x04: this.wr(a, 1, this.pop(1)); this.cycles += 6; return;                   // POP (mem)
      case 0x06: this.wr(a, 2, this.pop(2)); this.cycles += 6; return;                   // POPW (mem)
      case 0x14: { const s = this.f16(); this.wr(a, 1, this.rd(s, 1)); this.cycles += 8; return; } // LD (mem),(nn)
      case 0x16: { const s = this.f16(); this.wr(a, 2, this.rd(s, 2)); this.cycles += 8; return; } // LDW (mem),(nn)
      default: this.cycles += 2;
    }
  }

  // ------------------------------------------------------------ multiplicação e divisão
  // kind: 0 MUL, 1 MULS, 2 DIV, 3 DIVS; dc = registrador de destino (código de palavra); sz = tamanho do operando
  muldiv(kind, dc, v, sz) {
    if (kind < 2) {
      if (sz === 1) {
        let a = this.getW(dc) & 0xFF, b = v & 0xFF;
        if (kind === 1) { a = (a << 24) >> 24; b = (b << 24) >> 24; }
        this.setW(dc, (a * b) & 0xFFFF);
      } else {
        let a = this.getL(dc) & 0xFFFF, b = v & 0xFFFF;
        if (kind === 1) { a = (a << 16) >> 16; b = (b << 16) >> 16; }
        this.setL(dc, Math.imul(a, b) >>> 0);
      }
      return;
    }
    // divisão: quociente na parte baixa, resto na alta; por zero, V=1 e resultado (dividendo << meio) | ~parte alta
    if (sz === 1) {
      const a = this.getW(dc), b = v & 0xFF;
      if (b === 0) { const h = kind === 3 ? (a << 16) >> 24 : a >> 8; this.setW(dc, ((a << 8) | (h ^ 0xFF)) & 0xFFFF); this.f = this.f | FV; return; }
      let q, r;
      if (kind === 2) { q = Math.floor(a / b); r = a % b; }
      else { const sa = (a << 16) >> 16, sb = (b << 24) >> 24; q = Math.trunc(sa / sb); r = sa % sb; }
      this.f = (this.f & ~FV) | (q > 0xFF ? FV : 0);
      this.setW(dc, ((r & 0xFF) << 8) | (q & 0xFF));
    } else {
      const a = this.getL(dc), b = v & 0xFFFF;
      if (b === 0) { const h = kind === 3 ? a >> 16 : a >>> 16; this.setL(dc, ((a << 16) | (h ^ 0xFFFF)) >>> 0); this.f = this.f | FV; return; }
      let q, r;
      if (kind === 2) { q = Math.floor(a / b); r = a % b; }
      else { const sa = a | 0, sb = (b << 16) >> 16; q = Math.trunc(sa / sb); r = sa % sb; }
      this.f = (this.f & ~FV) | (q > 0xFFFF ? FV : 0);
      this.setL(dc, (((r & 0xFFFF) << 16) | (q & 0xFFFF)) >>> 0);
    }
  }

  // ------------------------------------------------------------ operações sobre registrador
  regOp(c, sz, prefix) {
    this.regOpX(c, sz, prefix);
    const t = REG_CYC[this.op2];
    if (t) this.cycles = sz === 4 ? t >> 8 : t & 0xFF;
  }
  regOpX(c, sz, prefix) {
    const op = this.f8();
    this.op2 = op;
    const R = op & 7;
    const Rc = sz === 1 ? BREG[R] : WREG[R];
    const v = this.getR(c, sz);
    this.cycles = 4;
    // operações aritméticas/lógicas de 32 bits entre registradores custam 7
    const L7 = sz === 4 ? 7 : 4;
    if (op >= 0xF8) { // RLC…SRL A,r (contagem em A)
      let n = this.getB(0xE0) & 15; if (!n) n = 16;
      this.setR(c, sz, this.shift(op & 7, v, n, sz)); this.cycles = (sz === 4 ? 8 : 6) + 2 * n; return;
    }
    if (op >= 0xF0) { this.alu(7, this.getR(Rc, sz), v, sz); this.cycles = L7; return; } // CP R,r
    if (op >= 0xE8) { let n = this.f8() & 15; if (!n) n = 16; this.setR(c, sz, this.shift(op & 7, v, n, sz)); this.cycles = (sz === 4 ? 8 : 6) + 2 * n; return; } // RLC… #,r
    if (op >= 0xE0) { this.setR(Rc, sz, this.alu(6, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // OR R,r
    if (op >= 0xD8) { if (sz !== 4) this.alu(7, v, R, sz); return; }                                 // CP r,#3
    if (op >= 0xD0) { this.setR(Rc, sz, this.alu(5, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // XOR R,r
    if (op >= 0xC8) { const imm = this.fimm(sz); const res = this.alu(op & 7, v, imm, sz); if ((op & 7) !== 7) this.setR(c, sz, res); this.cycles = L7; return; } // ALU r,#
    if (op >= 0xC0) { this.setR(Rc, sz, this.alu(4, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // AND R,r
    if (op >= 0xB8) { const t = this.getR(Rc, sz); this.setR(Rc, sz, v); this.setR(c, sz, t); return; } // EX R,r
    if (op >= 0xB0) { this.setR(Rc, sz, this.alu(3, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // SBC R,r
    if (op >= 0xA8) { this.setR(c, sz, R); return; }                                  // LD r,#3
    if (op >= 0xA0) { this.setR(Rc, sz, this.alu(2, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // SUB R,r
    if (op >= 0x98) { this.setR(c, sz, this.getR(Rc, sz)); return; }                 // LD r,R
    if (op >= 0x90) { this.setR(Rc, sz, this.alu(1, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // ADC R,r
    if (op >= 0x88) { this.setR(Rc, sz, v); return; }                                 // LD R,r
    if (op >= 0x80) { this.setR(Rc, sz, this.alu(0, this.getR(Rc, sz), v, sz)); this.cycles = L7; return; } // ADD R,r
    if (op >= 0x70) { if (sz !== 4) this.setR(c, sz, this.cond(op & 15) ? 1 : 0); return; }       // SCC cc,r
    if (op >= 0x68) { this.setR(c, sz, this.dec(v, R || 8, sz, sz === 1)); this.cycles = sz === 4 ? 5 : 4; return; } // DEC #3,r
    if (op >= 0x60) { this.setR(c, sz, this.inc(v, R || 8, sz, sz === 1)); return; } // INC #3,r
    if (op >= 0x40) { // MUL…DIVS RR,r (alvo inválido: registrador de descarte)
      const k = (op >> 3) & 3; if (sz === 4) { this.cycles = 2; return; }
      this.muldiv(k, sz === 1 ? (R & 1 ? WREG[R >> 1] : 0x80) : WREG[R], v, sz); this.cycles = MD_CYC[k] + (sz === 2 ? 8 : 0); return;
    }
    switch (op) {
      case 0x03: this.setR(c, sz, this.fimm(sz)); if (sz === 4) this.cycles = 6; return;      // LD r,#
      case 0x04: this.push(sz, v); this.cycles = sz === 4 ? 7 : 5; return;                     // PUSH r
      case 0x05: this.setR(c, sz, this.pop(sz)); this.cycles = sz === 4 ? 8 : 6; return;       // POP r
      case 0x06: if (sz !== 4) this.setR(c, sz, ~v); this.f = this.f | FH | FN; return;        // CPL
      case 0x07: if (sz !== 4) this.setR(c, sz, this.sub(0, v, sz, false)); return;            // NEG
      case 0x08: case 0x09: case 0x0A: case 0x0B: { // MUL… rr,#: o alvo vem do prefixo
        if (sz === 4) { this.cycles = 2; return; }
        const k = op - 8, n = prefix & 7;
        const t = sz === 1 ? (prefix === 0xC7 ? c & ~1 : n & 1 ? 0xE0 + (n >> 1) * 4 : 0x80) : 0xE0 + n * 4;
        this.muldiv(k, t, this.fimm(sz), sz); this.cycles = MD_CYC[k] + (sz === 2 ? 8 : 0); return;
      }
      case 0x0C: { const d = (this.f16() << 16) >> 16; this.push(4, v); this.setL(c, this.xsp); this.xsp = this.xsp + d; return; } // LINK
      case 0x0D: this.xsp = v; this.setL(c, this.pop(4)); return;                                // UNLK
      case 0x0E: case 0x0F: {                                                                    // BS1F/BS1B A,r
        if (v === 0) { this.f = this.f | FV; return; }
        this.f = this.f & ~FV;
        let i;
        if (op === 0x0E) { i = 0; while (!((v >> i) & 1)) i++; } else { i = 15; while (!((v >> i) & 1)) i--; }
        this.setB(0xE0, i); return;
      }
      case 0x10: { // DAA
        const a0 = v & 0xFF, fl = this.f;
        let a = a0, carry = fl & FC, fix = 0;
        if ((fl & FH) || (a & 0xF) > 9) fix |= 0x06;
        if (carry || a > 0x99) fix |= 0x60;
        a = (fl & FN) ? (a - fix) & 0xFF : (a + fix) & 0xFF;
        if ((fl & FN) ? a > a0 : a < a0) carry = 1;
        this.f = this.szFlags(a, 1) | PAR[a] | (fl & FN) | (((a0 ^ a) & 0x10) ? FH : 0) | carry;
        this.setB(c, a);
        return;
      }
      case 0x12: this.setR(c, sz, sz === 2 ? v & 0xFF : v & 0xFFFF); return;                   // EXTZ
      case 0x13: this.setR(c, sz, sz === 2 ? ((v << 24) >> 24) & 0xFFFF : (v << 16) >> 16); this.cycles = 5; return; // EXTS
      case 0x14: if (sz !== 1) this.setR(c, sz, v & 1 ? v + 1 : v); return;                                   // PAA
      case 0x16: { let m = 0; for (let k = 0; k < 16; k++) if (v & (1 << k)) m |= 1 << (15 - k); this.setR(c, 2, m); return; } // MIRR
      case 0x19: { // MULA rr
        const xhl = this.getL(0xEC);
        const p = Math.imul((this.rd(this.getL(0xE8), 2) << 16) >> 16, (this.rd(xhl, 2) << 16) >> 16);
        const old = this.getL(c), res = (old + p) | 0;
        this.setL(c, res >>> 0);
        this.setL(0xEC, xhl - 2);
        this.f = (this.f & ~(FS | FZ | FV)) | (res < 0 ? FS : 0) | (res === 0 ? FZ : 0) | ((((old ^ ~p) & (old ^ res)) >>> 31) ? FV : 0);
        this.cycles = 31; return;
      }
      case 0x1C: { const d = (this.f8() << 24) >> 24; if (sz === 4) { this.cycles = 7; return; } const nv = (v - 1) & MASK[sz]; this.setR(c, sz, nv); if (nv) { this.pc = (this.pc + d) & 0xFFFFFF; this.cycles = 11; } else this.cycles = 7; return; } // DJNZ
      case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: { const b = this.f8() & 15; if (sz !== 4) this.bitCF(op & 7, c, sz, v, b); return; }
      case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: { const b = this.getB(0xE0) & 15; if (sz !== 4) this.bitCF(op & 7, c, sz, v, b); return; }
      case 0x2E: { const cr = this.f8(); this.setCR(cr, v); return; }                        // LDC cr,r
      case 0x2F: { const cr = this.f8(); this.setR(c, sz, this.getCR(cr)); return; }        // LDC r,cr
      case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: {          // RES/SET/CHG/BIT/TSET #,r (só 8/16 bits)
        const b = this.f8() & 15;
        if (sz === 4) { if (op >= 0x33) this.f = (this.f & ~FN) | FH; return; }
        if (op === 0x30) this.setR(c, sz, v & ~(1 << b));
        else if (op === 0x31) this.setR(c, sz, v | (1 << b));
        else if (op === 0x32) this.setR(c, sz, v ^ (1 << b));
        else { this.f = (this.f & ~(FZ | FN)) | FH | ((v >> b) & 1 ? 0 : FZ); if (op === 0x34) this.setR(c, sz, v | (1 << b)); }
        return;
      }
      case 0x38: case 0x39: case 0x3A: { // MINC1/2/4 (só 16 bits)
        const step = 1 << (op - 0x38), num = (this.f16() + step) & 0xFFFF;
        this.cycles = 8;
        if (sz !== 2 || !num) return;
        const r = (v % num) === num - step ? v - (num - step) : v + step;
        this.setR(c, sz, r); return;
      }
      case 0x3C: case 0x3D: case 0x3E: { // MDEC1/2/4 (só 16 bits)
        const step = 1 << (op - 0x3C), num = (this.f16() + step) & 0xFFFF;
        this.cycles = 7;
        if (sz !== 2 || !num) return;
        const r = (v % num) === 0 ? v + (num - step) : v - step;
        this.setR(c, sz, r); return;
      }
      default: this.cycles = 2;
    }
  }

  bitCF(kind, c, sz, v, b) {
    const bit = (v >> b) & 1;
    switch (kind) {
      case 0: if (b < sz * 8) this.f = (this.f & ~FC) | ((this.f & FC) & bit); break; // ANDCF
      case 1: if (b < sz * 8) this.f = this.f | bit; break;                            // ORCF
      case 2: if (b < sz * 8) this.f = this.f ^ bit; break;                            // XORCF
      case 3: if (b < sz * 8) this.f = (this.f & ~FC) | bit; break;                    // LDCF
      case 4: if (b < sz * 8) this.setR(c, sz, (this.f & FC) ? v | (1 << b) : v & ~(1 << b)); break; // STCF
    }
  }

  // registradores de controle (micro-DMA e aninhamento de interrupções)
  getCR(cr) {
    if (cr < 0x10) return this.dmaS[cr >> 2];
    if (cr < 0x20) return this.dmaD[(cr - 0x10) >> 2];
    if (cr < 0x30) return (cr & 2) ? this.dmaM[(cr - 0x20) >> 2] : this.dmaC[(cr - 0x20) >> 2];
    if (cr === 0x3C) return this.intNest;
    return 0;
  }
  setCR(cr, v) {
    if (cr < 0x10) this.dmaS[cr >> 2] = v;
    else if (cr < 0x20) this.dmaD[(cr - 0x10) >> 2] = v;
    else if (cr < 0x30) { if (cr & 2) this.dmaM[(cr - 0x20) >> 2] = v; else this.dmaC[(cr - 0x20) >> 2] = v; }
    else if (cr === 0x3C) this.intNest = v & 0xFFFF;
  }

  // ------------------------------------------------------------ interrupção (nível > IFF)
  interrupt(vector, level) {
    this.halted = false;
    this.push(4, this.pc); this.push(2, this.sr);
    this.sr = (this.sr & 0x8FFF) | (Math.min(level + 1, 7) << 12);
    this.intNest = (this.intNest + 1) & 0xFFFF;
    this.pc = this.rd(0xFFFF00 + vector, 4) & 0xFFFFFF;
    return 18;
  }
}
