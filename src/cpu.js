// ARM7TDMI — interpretador ARM (32 bits) + Thumb (16 bits)
// Convenção do pipeline: durante a execução de uma instrução no endereço A,
// r[15] = A + 8 (ARM) ou A + 4 (Thumb).

export const MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
  MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F;

function bankOf(mode) {
  switch (mode) {
    case MODE_FIQ: return 1;
    case MODE_IRQ: return 2;
    case MODE_SVC: return 3;
    case MODE_ABT: return 4;
    case MODE_UND: return 5;
    default: return 0;
  }
}

export class CPU {
  constructor(bus) {
    this.bus = bus;
    this.r = new Int32Array(16);
    this.bankR13 = new Int32Array(6);
    this.bankR14 = new Int32Array(6);
    this.spsrBank = new Int32Array(6);
    this.usrHi = new Int32Array(5); // r8-r12 fora do FIQ
    this.fiqHi = new Int32Array(5); // r8-r12 do FIQ
    this.n = 0; this.z = 0; this.c = 0; this.v = 0;
    this.i = 1; this.f = 1; this.t = 0; this.mode = MODE_SVC;
    this.nextPC = 0;
    this.branched = false;
    this.shC = 0;
    this.halted = false;
    this.swiHandler = null; // BIOS em alto nível (HLE)
    this.repeat = false;
    this.armLut = new Array(4096);
    this.thumbLut = new Array(1024);
    this.buildLuts();
  }

  // ---------------- estado / modos ----------------
  get cpsr() {
    return (this.n << 31) | (this.z << 30) | (this.c << 29) | (this.v << 28) |
      (this.i << 7) | (this.f << 6) | (this.t << 5) | this.mode;
  }
  set cpsr(val) {
    this.n = (val >>> 31) & 1; this.z = (val >>> 30) & 1;
    this.c = (val >>> 29) & 1; this.v = (val >>> 28) & 1;
    this.i = (val >>> 7) & 1; this.f = (val >>> 6) & 1; this.t = (val >>> 5) & 1;
    this.switchMode(val & 0x1F);
  }
  get spsr() {
    const b = bankOf(this.mode);
    return b === 0 ? this.cpsr : this.spsrBank[b];
  }
  set spsr(val) {
    const b = bankOf(this.mode);
    if (b !== 0) this.spsrBank[b] = val;
  }

  switchMode(newMode) {
    newMode |= 0x10;
    const oldMode = this.mode;
    if (oldMode === newMode) return;
    const ob = bankOf(oldMode), nb = bankOf(newMode);
    if (ob !== nb) {
      this.bankR13[ob] = this.r[13]; this.bankR14[ob] = this.r[14];
      if (ob === 1) {
        for (let k = 0; k < 5; k++) { this.fiqHi[k] = this.r[8 + k]; this.r[8 + k] = this.usrHi[k]; }
      }
      if (nb === 1) {
        for (let k = 0; k < 5; k++) { this.usrHi[k] = this.r[8 + k]; this.r[8 + k] = this.fiqHi[k]; }
      }
      this.r[13] = this.bankR13[nb]; this.r[14] = this.bankR14[nb];
    }
    this.mode = newMode;
  }

  reset(skipBios) {
    this.r.fill(0);
    this.bankR13.fill(0); this.bankR14.fill(0); this.spsrBank.fill(0);
    this.usrHi.fill(0); this.fiqHi.fill(0);
    this.mode = MODE_SVC;
    this.n = this.z = this.c = this.v = 0;
    this.i = 1; this.f = 1; this.t = 0;
    this.halted = false;
    if (skipBios) {
      // Estado deixado pela BIOS oficial ao saltar para o cartucho
      this.switchMode(MODE_IRQ); this.r[13] = 0x03007FA0;
      this.switchMode(MODE_SVC); this.r[13] = 0x03007FE0;
      this.switchMode(MODE_SYS); this.r[13] = 0x03007F00;
      this.i = 0; this.f = 0;
      this.nextPC = 0x08000000;
    } else {
      this.nextPC = 0;
    }
  }

  // ---------------- execução ----------------
  checkCond(cond) {
    switch (cond) {
      case 0x0: return this.z === 1;
      case 0x1: return this.z === 0;
      case 0x2: return this.c === 1;
      case 0x3: return this.c === 0;
      case 0x4: return this.n === 1;
      case 0x5: return this.n === 0;
      case 0x6: return this.v === 1;
      case 0x7: return this.v === 0;
      case 0x8: return this.c === 1 && this.z === 0;
      case 0x9: return this.c === 0 || this.z === 1;
      case 0xA: return this.n === this.v;
      case 0xB: return this.n !== this.v;
      case 0xC: return this.z === 0 && this.n === this.v;
      case 0xD: return this.z === 1 || this.n !== this.v;
      case 0xE: return true;
      default: return false;
    }
  }

  // Executa uma instrução; retorna ciclos de "trabalho interno" (acessos de memória são contados pelo barramento)
  step() {
    const pc = this.nextPC;
    const bus = this.bus;
    let cyc;
    this.branched = false;
    if (this.t) {
      const ins = bus.fetch16(pc);
      bus.codeCost(pc + 4, false); // o pipeline busca a instrução 2 à frente no 1º ciclo
      this.r[15] = pc + 4;
      cyc = this.thumbLut[ins >>> 6].call(this, ins);
      if (this.branched) this.refill();
      else this.nextPC = pc + 2;
    } else {
      const ins = bus.fetch32(pc);
      bus.codeCost(pc + 8, true);
      this.r[15] = pc + 8;
      const cond = ins >>> 28;
      if (cond === 0xE || this.checkCond(cond)) {
        cyc = this.armLut[((ins >>> 16) & 0xFF0) | ((ins >>> 4) & 0xF)].call(this, ins);
      } else cyc = 0;
      if (this.branched) this.refill();
      else this.nextPC = pc + 4;
    }
    return cyc;
  }

  setPC(val) { this.r[15] = val; this.branched = true; }

  // desvio: o pipeline busca o destino (N) e a instrução seguinte (S)
  refill() {
    const t = this.t;
    const target = t ? (this.r[15] & ~1) : (this.r[15] & ~3);
    this.nextPC = target;
    this.bus.codeCost(target, !t);
    this.bus.codeCost(target + (t ? 2 : 4), !t);
  }

  irq() {
    const ret = this.nextPC + 4;
    const old = this.cpsr;
    this.switchMode(MODE_IRQ);
    this.spsrBank[2] = old;
    this.r[14] = ret;
    this.t = 0; this.i = 1;
    this.nextPC = 0x18;
    this.halted = false;
    this.bus.codeCost(0x18, true); this.bus.codeCost(0x1C, true);
  }

  swiException(retAddr) {
    const old = this.cpsr;
    this.switchMode(MODE_SVC);
    this.spsrBank[3] = old;
    this.r[14] = retAddr;
    this.t = 0; this.i = 1;
    this.setPC(0x08);
  }

  undefinedInstr() { return 1; }

  // ---------------- barrel shifter ----------------
  shiftImm(type, v, amt) {
    switch (type) {
      case 0: // LSL
        if (amt === 0) { this.shC = this.c; return v; }
        this.shC = (v >>> (32 - amt)) & 1; return v << amt;
      case 1: // LSR
        if (amt === 0) { this.shC = v >>> 31; return 0; }
        this.shC = (v >>> (amt - 1)) & 1; return v >>> amt;
      case 2: // ASR
        if (amt === 0) { this.shC = v >>> 31; return v >> 31; }
        this.shC = (v >> (amt - 1)) & 1; return v >> amt;
      default: // ROR / RRX
        if (amt === 0) { this.shC = v & 1; return (this.c << 31) | (v >>> 1); }
        this.shC = (v >>> (amt - 1)) & 1; return (v >>> amt) | (v << (32 - amt));
    }
  }

  shiftReg(type, v, amt) {
    if (amt === 0) { this.shC = this.c; return v; }
    switch (type) {
      case 0:
        if (amt < 32) { this.shC = (v >>> (32 - amt)) & 1; return v << amt; }
        if (amt === 32) { this.shC = v & 1; return 0; }
        this.shC = 0; return 0;
      case 1:
        if (amt < 32) { this.shC = (v >>> (amt - 1)) & 1; return v >>> amt; }
        if (amt === 32) { this.shC = v >>> 31; return 0; }
        this.shC = 0; return 0;
      case 2:
        if (amt < 32) { this.shC = (v >> (amt - 1)) & 1; return v >> amt; }
        this.shC = v >>> 31; return v >> 31;
      default: {
        const a = amt & 31;
        if (a === 0) { this.shC = v >>> 31; return v; }
        this.shC = (v >>> (a - 1)) & 1; return (v >>> a) | (v << (32 - a));
      }
    }
  }

  // ---------------- aritmética com flags ----------------
  setNZ(r) { this.n = r >>> 31; this.z = r === 0 ? 1 : 0; }
  add(a, b, s) {
    const r = (a + b) | 0;
    if (s) {
      this.n = r >>> 31; this.z = r === 0 ? 1 : 0;
      this.c = ((a >>> 0) + (b >>> 0)) > 0xFFFFFFFF ? 1 : 0;
      this.v = (~(a ^ b) & (a ^ r)) >>> 31;
    }
    return r;
  }
  adc(a, b, s) {
    const cin = this.c;
    const r = (a + b + cin) | 0;
    if (s) {
      this.n = r >>> 31; this.z = r === 0 ? 1 : 0;
      this.c = ((a >>> 0) + (b >>> 0) + cin) > 0xFFFFFFFF ? 1 : 0;
      this.v = (~(a ^ b) & (a ^ r)) >>> 31;
    }
    return r;
  }
  sub(a, b, s) {
    const r = (a - b) | 0;
    if (s) {
      this.n = r >>> 31; this.z = r === 0 ? 1 : 0;
      this.c = (a >>> 0) >= (b >>> 0) ? 1 : 0;
      this.v = ((a ^ b) & (a ^ r)) >>> 31;
    }
    return r;
  }
  sbc(a, b, s) {
    const borrow = 1 - this.c;
    const r = (a - b - borrow) | 0;
    if (s) {
      this.n = r >>> 31; this.z = r === 0 ? 1 : 0;
      this.c = (a >>> 0) >= ((b >>> 0) + borrow) ? 1 : 0;
      this.v = ((a ^ b) & (a ^ r)) >>> 31;
    }
    return r;
  }

  mulCycles(rs) {
    const v = rs >>> 0;
    if ((v & 0xFFFFFF00) === 0 || (v & 0xFFFFFF00) === 0xFFFFFF00) return 1;
    if ((v & 0xFFFF0000) === 0 || (v & 0xFFFF0000) === 0xFFFF0000) return 2;
    if ((v & 0xFF000000) === 0 || (v & 0xFF000000) === 0xFF000000) return 3;
    return 4;
  }

  // leitura com rotação (LDR desalinhado)
  ldrWord(addr) {
    const v = this.bus.read32(addr & ~3);
    const rot = (addr & 3) << 3;
    return rot ? ((v >>> rot) | (v << (32 - rot))) : v;
  }
  ldrHalf(addr) {
    const v = this.bus.read16(addr & ~1);
    return (addr & 1) ? ((v >>> 8) | (v << 24)) : v;
  }
  ldrSHalf(addr) {
    if (addr & 1) return (this.bus.read8(addr) << 24) >> 24;
    return (this.bus.read16(addr) << 16) >> 16;
  }

  // =====================================================================
  //                              ARM
  // =====================================================================
  buildLuts() {
    for (let key = 0; key < 4096; key++) this.armLut[key] = this.decodeArm(key);
    for (let key = 0; key < 1024; key++) this.thumbLut[key] = this.decodeThumb(key << 6);
  }

  decodeArm(key) {
    const hi = key >>> 4, lo = key & 0xF;
    switch (hi >>> 5) {
      case 0:
        if (lo === 9) {
          if ((hi & 0xFC) === 0x00) return this.armMul;
          if ((hi & 0xF8) === 0x08) return this.armMulLong;
          if ((hi & 0xFB) === 0x10) return this.armSwp;
          return this.undefinedInstr;
        }
        if ((lo & 9) === 9) return this.armHalf;
        if (hi === 0x12 && lo === 1) return this.armBx;
        if ((hi & 0xF9) === 0x10) {
          if (lo !== 0) return this.undefinedInstr;
          return (hi & 2) ? this.armMsr : this.armMrs;
        }
        return this.armDP;
      case 1:
        if ((hi & 0xF9) === 0x30) return (hi & 2) ? this.armMsr : this.undefinedInstr;
        return this.armDP;
      case 2: return this.armSdt;
      case 3: return (lo & 1) ? this.undefinedInstr : this.armSdt;
      case 4: return this.armBlock;
      case 5: return this.armBranch;
      case 6: return this.undefinedInstr;
      default: return (hi & 0x10) ? this.armSwi : this.undefinedInstr;
    }
  }

  armDP(ins) {
    const op = (ins >>> 21) & 15, s = (ins >>> 20) & 1;
    const rn = (ins >>> 16) & 15, rd = (ins >>> 12) & 15;
    let op2, rnv, cyc = 0;
    if (ins & 0x02000000) {
      const imm = ins & 0xFF, rot = ((ins >>> 8) & 15) << 1;
      if (rot) { op2 = (imm >>> rot) | (imm << (32 - rot)); this.shC = op2 >>> 31; }
      else { op2 = imm; this.shC = this.c; }
      rnv = this.r[rn];
    } else if (ins & 0x10) {
      const rm = ins & 15;
      let rmv = this.r[rm]; if (rm === 15) rmv += 4;
      rnv = this.r[rn]; if (rn === 15) rnv += 4;
      op2 = this.shiftReg((ins >>> 5) & 3, rmv, this.r[(ins >>> 8) & 15] & 0xFF);
      cyc = 1;
    } else {
      op2 = this.shiftImm((ins >>> 5) & 3, this.r[ins & 15], (ins >>> 7) & 31);
      rnv = this.r[rn];
    }
    let res;
    const sf = s && rd !== 15; // com Rd=PC e S=1, as flags vêm do SPSR
    switch (op) {
      case 0x0: res = rnv & op2; if (sf) { this.setNZ(res); this.c = this.shC; } break;
      case 0x1: res = rnv ^ op2; if (sf) { this.setNZ(res); this.c = this.shC; } break;
      case 0x2: res = this.sub(rnv, op2, sf); break;
      case 0x3: res = this.sub(op2, rnv, sf); break;
      case 0x4: res = this.add(rnv, op2, sf); break;
      case 0x5: res = this.adc(rnv, op2, sf); break;
      case 0x6: res = this.sbc(rnv, op2, sf); break;
      case 0x7: res = this.sbc(op2, rnv, sf); break;
      case 0x8: res = rnv & op2; this.setNZ(res); this.c = this.shC; if (rd === 15) this.cpsr = this.spsr; return cyc;
      case 0x9: res = rnv ^ op2; this.setNZ(res); this.c = this.shC; if (rd === 15) this.cpsr = this.spsr; return cyc;
      case 0xA: this.sub(rnv, op2, 1); if (rd === 15) this.cpsr = this.spsr; return cyc;
      case 0xB: this.add(rnv, op2, 1); if (rd === 15) this.cpsr = this.spsr; return cyc;
      case 0xC: res = rnv | op2; if (sf) { this.setNZ(res); this.c = this.shC; } break;
      case 0xD: res = op2; if (sf) { this.setNZ(res); this.c = this.shC; } break;
      case 0xE: res = rnv & ~op2; if (sf) { this.setNZ(res); this.c = this.shC; } break;
      default: res = ~op2; if (sf) { this.setNZ(res); this.c = this.shC; } break;
    }
    if (rd === 15) {
      if (s) this.cpsr = this.spsr;
      this.setPC(res);
      return cyc + 2;
    }
    this.r[rd] = res;
    return cyc;
  }

  armMrs(ins) {
    this.r[(ins >>> 12) & 15] = (ins & 0x00400000) ? this.spsr : this.cpsr;
    return 0;
  }

  armMsr(ins) {
    let val;
    if (ins & 0x02000000) {
      const imm = ins & 0xFF, rot = ((ins >>> 8) & 15) << 1;
      val = rot ? ((imm >>> rot) | (imm << (32 - rot))) : imm;
    } else val = this.r[ins & 15];
    let mask = 0;
    if (ins & 0x00080000) mask |= 0xFF000000;
    if (ins & 0x00010000) mask |= 0x000000FF;
    if (ins & 0x00400000) {
      if (bankOf(this.mode) !== 0) this.spsr = (this.spsr & ~mask) | (val & mask);
    } else {
      if (this.mode === MODE_USR) mask &= 0xFF000000;
      const nv = (this.cpsr & ~mask) | (val & mask);
      this.cpsr = (nv & ~0x20) | (this.t << 5); // T não muda por MSR
    }
    return 0;
  }

  armBx(ins) {
    const v = this.r[ins & 15];
    this.t = v & 1;
    this.setPC(v & ~1);
    return 2;
  }

  armMul(ins) {
    const rd = (ins >>> 16) & 15, rn = (ins >>> 12) & 15, rs = (ins >>> 8) & 15, rm = ins & 15;
    const rsv = this.r[rs];
    let res = Math.imul(this.r[rm], rsv);
    let cyc = this.mulCycles(rsv);
    if (ins & 0x00200000) { res = (res + this.r[rn]) | 0; cyc++; }
    if (ins & 0x00100000) this.setNZ(res);
    this.r[rd] = res;
    return cyc;
  }

  armMulLong(ins) {
    const rdHi = (ins >>> 16) & 15, rdLo = (ins >>> 12) & 15, rs = (ins >>> 8) & 15, rm = ins & 15;
    const signed = (ins & 0x00400000) !== 0;
    const a = this.r[rm], b = this.r[rs];
    const au = a >>> 0, bu = b >>> 0;
    const al = au & 0xFFFF, ah = au >>> 16, bl = bu & 0xFFFF, bh = bu >>> 16;
    const ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    const mid = (ll >>> 16) + (lh & 0xFFFF) + (hl & 0xFFFF);
    let lo = (((mid & 0xFFFF) << 16) | (ll & 0xFFFF)) >>> 0;
    let hi = (hh + (lh >>> 16) + (hl >>> 16) + Math.floor(mid / 65536)) >>> 0;
    if (signed) {
      if (a < 0) hi = (hi - bu) >>> 0;
      if (b < 0) hi = (hi - au) >>> 0;
    }
    let cyc = this.mulCycles(b) + 1;
    if (ins & 0x00200000) {
      const lo2 = lo + (this.r[rdLo] >>> 0);
      const carry = lo2 > 0xFFFFFFFF ? 1 : 0;
      lo = lo2 >>> 0;
      hi = (hi + (this.r[rdHi] >>> 0) + carry) >>> 0;
      cyc++;
    }
    if (ins & 0x00100000) {
      this.n = hi >>> 31;
      this.z = (lo === 0 && hi === 0) ? 1 : 0;
    }
    this.r[rdLo] = lo;
    this.r[rdHi] = hi;
    return cyc;
  }

  armSwp(ins) {
    const rn = (ins >>> 16) & 15, rd = (ins >>> 12) & 15, rm = ins & 15;
    const addr = this.r[rn];
    const src = this.r[rm];
    let tmp;
    if (ins & 0x00400000) {
      tmp = this.bus.read8(addr);
      this.bus.write8(addr, src);
    } else {
      tmp = this.ldrWord(addr);
      this.bus.write32(addr, src);
    }
    this.r[rd] = tmp;
    return 1;
  }

  armHalf(ins) {
    const p = ins & 0x01000000, u = ins & 0x00800000, w = ins & 0x00200000, l = ins & 0x00100000;
    const rn = (ins >>> 16) & 15, rd = (ins >>> 12) & 15;
    const sh = (ins >>> 5) & 3;
    const off = (ins & 0x00400000) ? (((ins >>> 4) & 0xF0) | (ins & 0xF)) : this.r[ins & 15];
    const base = this.r[rn];
    const offAddr = u ? (base + off) | 0 : (base - off) | 0;
    const addr = p ? offAddr : base;
    if (l) {
      let val;
      if (sh === 1) val = this.ldrHalf(addr);
      else if (sh === 2) val = (this.bus.read8(addr) << 24) >> 24;
      else val = this.ldrSHalf(addr);
      if (!p || w) this.r[rn] = offAddr;
      if (rd === 15) { this.setPC(val); return 3; }
      this.r[rd] = val;
      return 1;
    } else {
      if (sh === 1) {
        let val = this.r[rd]; if (rd === 15) val += 4;
        this.bus.write16(addr, val);
      }
      if (!p || w) this.r[rn] = offAddr;
      return 0;
    }
  }

  armSdt(ins) {
    const p = ins & 0x01000000, u = ins & 0x00800000, b = ins & 0x00400000,
      w = ins & 0x00200000, l = ins & 0x00100000;
    const rn = (ins >>> 16) & 15, rd = (ins >>> 12) & 15;
    let off;
    if (ins & 0x02000000) {
      const saveC = this.shC;
      off = this.shiftImm((ins >>> 5) & 3, this.r[ins & 15], (ins >>> 7) & 31);
      this.shC = saveC;
    } else off = ins & 0xFFF;
    const base = this.r[rn];
    const offAddr = u ? (base + off) | 0 : (base - off) | 0;
    const addr = p ? offAddr : base;
    if (l) {
      const val = b ? this.bus.read8(addr) : this.ldrWord(addr);
      if (!p || w) this.r[rn] = offAddr;
      if (rd === 15) { this.setPC(val); return 3; }
      this.r[rd] = val;
      return 1;
    } else {
      let val = this.r[rd]; if (rd === 15) val += 4;
      if (b) this.bus.write8(addr, val); else this.bus.write32(addr, val);
      if (!p || w) this.r[rn] = offAddr;
      return 0;
    }
  }

  armBlock(ins) {
    const p = (ins & 0x01000000) !== 0, u = (ins & 0x00800000) !== 0, s = (ins & 0x00400000) !== 0,
      w = (ins & 0x00200000) !== 0, l = (ins & 0x00100000) !== 0;
    const rn = (ins >>> 16) & 15;
    let rlist = ins & 0xFFFF;
    const base = this.r[rn];
    let count = 0;
    for (let k = 0; k < 16; k++) if (rlist & (1 << k)) count++;
    let empty = false;
    if (rlist === 0) { empty = true; rlist = 0x8000; count = 16; }
    let addr, finalBase;
    if (u) { addr = p ? base + 4 : base; finalBase = (base + count * 4) | 0; }
    else { addr = p ? base - count * 4 : base - count * 4 + 4; finalBase = (base - count * 4) | 0; }
    addr |= 0;
    const userBank = s && !(l && (rlist & 0x8000));
    const oldMode = this.mode;
    if (userBank) this.switchMode(MODE_USR);
    const bus = this.bus;
    if (l) {
      if (w) this.r[rn] = finalBase;
      for (let k = 0; k < 16; k++) {
        if (!(rlist & (1 << k))) continue;
        const val = bus.read32(addr & ~3);
        addr = (addr + 4) | 0;
        if (k === 15) {
          if (s) this.cpsr = this.spsr;
          this.setPC(val);
        } else this.r[k] = val;
      }
      if (userBank) this.switchMode(oldMode);
      return 1;
    } else {
      let first = true;
      for (let k = 0; k < 16; k++) {
        if (!(rlist & (1 << k))) continue;
        let val;
        if (k === 15) val = this.r[15] + 4;
        else if (k === rn) val = first ? base : finalBase;
        else val = this.r[k];
        bus.write32(addr, val);
        addr = (addr + 4) | 0;
        first = false;
      }
      if (userBank) this.switchMode(oldMode);
      if (w) this.r[rn] = finalBase;
      return 0;
    }
  }

  armBranch(ins) {
    const off = (ins << 8) >> 6;
    if (ins & 0x01000000) this.r[14] = this.r[15] - 4;
    this.setPC((this.r[15] + off) | 0);
    return 2;
  }

  armSwi(ins) {
    const ret = this.r[15] - 4;
    if (this.swiHandler) { this.swiHandler((ins >>> 16) & 0xFF, ret - 4); return 2; }
    this.swiException(ret);
    return 2;
  }

  // =====================================================================
  //                              THUMB
  // =====================================================================
  decodeThumb(ins) {
    const top = ins >>> 8;
    if ((ins & 0xF800) === 0x1800) return this.thAddSub;
    if ((ins & 0xE000) === 0x0000) return this.thShift;
    if ((ins & 0xE000) === 0x2000) return this.thImm;
    if ((ins & 0xFC00) === 0x4000) return this.thAlu;
    if ((ins & 0xFC00) === 0x4400) return this.thHi;
    if ((ins & 0xF800) === 0x4800) return this.thLdrPc;
    if ((ins & 0xF200) === 0x5000) return this.thLsReg;
    if ((ins & 0xF200) === 0x5200) return this.thLsSign;
    if ((ins & 0xE000) === 0x6000) return this.thLsImm;
    if ((ins & 0xF000) === 0x8000) return this.thLsHalf;
    if ((ins & 0xF000) === 0x9000) return this.thLsSp;
    if ((ins & 0xF000) === 0xA000) return this.thAddr;
    if (top === 0xB0) return this.thAddSp;
    if ((ins & 0xF600) === 0xB400) return this.thPushPop;
    if ((ins & 0xF000) === 0xC000) return this.thLdmStm;
    if (top === 0xDF) return this.thSwi;
    if ((ins & 0xF000) === 0xD000) return this.thBcond;
    if ((ins & 0xF800) === 0xE000) return this.thB;
    if ((ins & 0xF800) === 0xF000) return this.thBlHi;
    if ((ins & 0xF800) === 0xF800) return this.thBlLo;
    return this.undefinedInstr;
  }

  thShift(ins) {
    const rd = ins & 7, rs = (ins >>> 3) & 7, amt = (ins >>> 6) & 31;
    const res = this.shiftImm((ins >>> 11) & 3, this.r[rs], amt);
    this.r[rd] = res;
    this.setNZ(res); this.c = this.shC;
    return 0;
  }

  thAddSub(ins) {
    const rd = ins & 7, rs = (ins >>> 3) & 7, x = (ins >>> 6) & 7;
    const op2 = (ins & 0x400) ? x : this.r[x];
    this.r[rd] = (ins & 0x200) ? this.sub(this.r[rs], op2, 1) : this.add(this.r[rs], op2, 1);
    return 0;
  }

  thImm(ins) {
    const rd = (ins >>> 8) & 7, imm = ins & 0xFF;
    switch ((ins >>> 11) & 3) {
      case 0: this.r[rd] = imm; this.setNZ(imm); break;
      case 1: this.sub(this.r[rd], imm, 1); break;
      case 2: this.r[rd] = this.add(this.r[rd], imm, 1); break;
      default: this.r[rd] = this.sub(this.r[rd], imm, 1); break;
    }
    return 0;
  }

  thAlu(ins) {
    const rd = ins & 7, rs = (ins >>> 3) & 7;
    const a = this.r[rd], b = this.r[rs];
    let res;
    switch ((ins >>> 6) & 15) {
      case 0x0: res = a & b; this.setNZ(res); this.r[rd] = res; return 0;
      case 0x1: res = a ^ b; this.setNZ(res); this.r[rd] = res; return 0;
      case 0x2: res = this.shiftReg(0, a, b & 0xFF); this.setNZ(res); this.c = this.shC; this.r[rd] = res; return 1;
      case 0x3: res = this.shiftReg(1, a, b & 0xFF); this.setNZ(res); this.c = this.shC; this.r[rd] = res; return 1;
      case 0x4: res = this.shiftReg(2, a, b & 0xFF); this.setNZ(res); this.c = this.shC; this.r[rd] = res; return 1;
      case 0x5: this.r[rd] = this.adc(a, b, 1); return 0;
      case 0x6: this.r[rd] = this.sbc(a, b, 1); return 0;
      case 0x7: res = this.shiftReg(3, a, b & 0xFF); this.setNZ(res); this.c = this.shC; this.r[rd] = res; return 1;
      case 0x8: this.setNZ(a & b); return 0;
      case 0x9: this.r[rd] = this.sub(0, b, 1); return 0;
      case 0xA: this.sub(a, b, 1); return 0;
      case 0xB: this.add(a, b, 1); return 0;
      case 0xC: res = a | b; this.setNZ(res); this.r[rd] = res; return 0;
      case 0xD: res = Math.imul(a, b); this.setNZ(res); this.r[rd] = res; return this.mulCycles(a);
      case 0xE: res = a & ~b; this.setNZ(res); this.r[rd] = res; return 0;
      default: res = ~b; this.setNZ(res); this.r[rd] = res; return 0;
    }
  }

  thHi(ins) {
    const rd = (ins & 7) | ((ins >>> 4) & 8), rs = ((ins >>> 3) & 15);
    const sv = this.r[rs];
    switch ((ins >>> 8) & 3) {
      case 0: {
        const res = (this.r[rd] + sv) | 0;
        if (rd === 15) { this.setPC(res & ~1); return 2; }
        this.r[rd] = res; return 0;
      }
      case 1: this.sub(this.r[rd], sv, 1); return 0;
      case 2:
        if (rd === 15) { this.setPC(sv & ~1); return 2; }
        this.r[rd] = sv; return 0;
      default:
        this.t = sv & 1;
        this.setPC(sv & ~1);
        return 2;
    }
  }

  thLdrPc(ins) {
    const rd = (ins >>> 8) & 7;
    this.r[rd] = this.bus.read32(((this.r[15] & ~2) + ((ins & 0xFF) << 2)) & ~3);
    return 1;
  }

  thLsReg(ins) {
    const rd = ins & 7, rb = (ins >>> 3) & 7, ro = (ins >>> 6) & 7;
    const addr = (this.r[rb] + this.r[ro]) | 0;
    switch ((ins >>> 10) & 3) {
      case 0: this.bus.write32(addr, this.r[rd]); return 0;
      case 1: this.bus.write8(addr, this.r[rd]); return 0;
      case 2: this.r[rd] = this.ldrWord(addr); return 1;
      default: this.r[rd] = this.bus.read8(addr); return 1;
    }
  }

  thLsSign(ins) {
    const rd = ins & 7, rb = (ins >>> 3) & 7, ro = (ins >>> 6) & 7;
    const addr = (this.r[rb] + this.r[ro]) | 0;
    switch ((ins >>> 10) & 3) {
      case 0: this.bus.write16(addr, this.r[rd]); return 0;
      case 1: this.r[rd] = (this.bus.read8(addr) << 24) >> 24; return 1;
      case 2: this.r[rd] = this.ldrHalf(addr); return 1;
      default: this.r[rd] = this.ldrSHalf(addr); return 1;
    }
  }

  thLsImm(ins) {
    const rd = ins & 7, rb = (ins >>> 3) & 7, off = (ins >>> 6) & 31;
    if (ins & 0x1000) { // byte
      const addr = (this.r[rb] + off) | 0;
      if (ins & 0x800) { this.r[rd] = this.bus.read8(addr); return 1; }
      this.bus.write8(addr, this.r[rd]); return 0;
    }
    const addr = (this.r[rb] + (off << 2)) | 0;
    if (ins & 0x800) { this.r[rd] = this.ldrWord(addr); return 1; }
    this.bus.write32(addr, this.r[rd]); return 0;
  }

  thLsHalf(ins) {
    const rd = ins & 7, rb = (ins >>> 3) & 7;
    const addr = (this.r[rb] + (((ins >>> 6) & 31) << 1)) | 0;
    if (ins & 0x800) { this.r[rd] = this.ldrHalf(addr); return 1; }
    this.bus.write16(addr, this.r[rd]); return 0;
  }

  thLsSp(ins) {
    const rd = (ins >>> 8) & 7;
    const addr = (this.r[13] + ((ins & 0xFF) << 2)) | 0;
    if (ins & 0x800) { this.r[rd] = this.ldrWord(addr); return 1; }
    this.bus.write32(addr, this.r[rd]); return 0;
  }

  thAddr(ins) {
    const rd = (ins >>> 8) & 7, off = (ins & 0xFF) << 2;
    this.r[rd] = (ins & 0x800) ? (this.r[13] + off) | 0 : ((this.r[15] & ~2) + off) | 0;
    return 0;
  }

  thAddSp(ins) {
    const off = (ins & 0x7F) << 2;
    this.r[13] = (ins & 0x80) ? (this.r[13] - off) | 0 : (this.r[13] + off) | 0;
    return 0;
  }

  thPushPop(ins) {
    const bus = this.bus;
    let rlist = ins & 0xFF;
    const extra = (ins & 0x100) !== 0;
    if (ins & 0x800) { // POP
      let addr = this.r[13];
      if (rlist === 0 && !extra) { // lista vazia: carrega PC, SP += 0x40
        const v = bus.read32(addr & ~3);
        this.r[13] = addr + 0x40;
        this.setPC(v & ~1);
        return 2;
      }
      for (let k = 0; k < 8; k++) {
        if (rlist & (1 << k)) { this.r[k] = bus.read32(addr & ~3); addr += 4; }
      }
      if (extra) {
        const v = bus.read32(addr & ~3); addr += 4;
        this.setPC(v & ~1);
      }
      this.r[13] = addr;
      return extra ? 3 : 1;
    } else { // PUSH
      if (rlist === 0 && !extra) {
        this.r[13] = this.r[13] - 0x40;
        bus.write32(this.r[13] & ~3, this.r[15] + 2);
        return 0;
      }
      let count = extra ? 1 : 0;
      for (let k = 0; k < 8; k++) if (rlist & (1 << k)) count++;
      let addr = (this.r[13] - count * 4) | 0;
      this.r[13] = addr;
      for (let k = 0; k < 8; k++) {
        if (rlist & (1 << k)) { bus.write32(addr, this.r[k]); addr += 4; }
      }
      if (extra) bus.write32(addr, this.r[14]);
      return 0;
    }
  }

  thLdmStm(ins) {
    const bus = this.bus;
    const rb = (ins >>> 8) & 7;
    const rlist = ins & 0xFF;
    let addr = this.r[rb];
    if (rlist === 0) {
      if (ins & 0x800) { const v = bus.read32(addr & ~3); this.setPC(v & ~1); }
      else bus.write32(addr, this.r[15] + 2);
      this.r[rb] = addr + 0x40;
      return 1;
    }
    let count = 0;
    for (let k = 0; k < 8; k++) if (rlist & (1 << k)) count++;
    const finalAddr = (addr + count * 4) | 0;
    if (ins & 0x800) {
      for (let k = 0; k < 8; k++) {
        if (rlist & (1 << k)) { this.r[k] = bus.read32(addr & ~3); addr += 4; }
      }
      if (!(rlist & (1 << rb))) this.r[rb] = finalAddr;
      return 1;
    } else {
      let first = true;
      for (let k = 0; k < 8; k++) {
        if (rlist & (1 << k)) {
          const val = (k === rb) ? (first ? this.r[rb] : finalAddr) : this.r[k];
          bus.write32(addr, val); addr += 4; first = false;
        }
      }
      this.r[rb] = finalAddr;
      return 0;
    }
  }

  thBcond(ins) {
    if (!this.checkCond((ins >>> 8) & 15)) return 0;
    const off = ((ins & 0xFF) << 24) >> 23;
    this.setPC((this.r[15] + off) | 0);
    return 2;
  }

  thSwi(ins) {
    const ret = this.r[15] - 2;
    if (this.swiHandler) { this.swiHandler(ins & 0xFF, ret - 2); return 2; }
    this.swiException(ret);
    return 2;
  }

  thB(ins) {
    const off = ((ins & 0x7FF) << 21) >> 20;
    this.setPC((this.r[15] + off) | 0);
    return 2;
  }

  thBlHi(ins) {
    const off = ((ins & 0x7FF) << 21) >> 9;
    this.r[14] = (this.r[15] + off) | 0;
    return 0;
  }

  thBlLo(ins) {
    const next = (this.r[15] - 2) | 1;
    this.setPC((this.r[14] + ((ins & 0x7FF) << 1)) | 0);
    this.r[14] = next;
    return 2;
  }
}
