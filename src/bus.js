// Barramento de memória do GBA
// 0x00 BIOS | 0x02 EWRAM 256K | 0x03 IWRAM 32K | 0x04 IO | 0x05 Paleta | 0x06 VRAM | 0x07 OAM
// 0x08-0x0D ROM (3 estados de espera) | 0x0E SRAM/Flash

const N_TABLE = [4, 3, 2, 8];

export class Bus {
  constructor(gba) {
    this.gba = gba;
    this.bios = new Uint8Array(0x4000);
    this.bios16 = new Uint16Array(this.bios.buffer);
    this.bios32 = new Int32Array(this.bios.buffer);
    this.ewram = new Uint8Array(0x40000);
    this.ewram16 = new Uint16Array(this.ewram.buffer);
    this.ewram32 = new Int32Array(this.ewram.buffer);
    this.iwram = new Uint8Array(0x8000);
    this.iwram16 = new Uint16Array(this.iwram.buffer);
    this.iwram32 = new Int32Array(this.iwram.buffer);
    this.pal = new Uint8Array(0x400);
    this.pal16 = new Uint16Array(this.pal.buffer);
    this.pal32 = new Int32Array(this.pal.buffer);
    this.vram = new Uint8Array(0x18000);
    this.vram16 = new Uint16Array(this.vram.buffer);
    this.vram32 = new Int32Array(this.vram.buffer);
    this.oam = new Uint8Array(0x400);
    this.oam16 = new Uint16Array(this.oam.buffer);
    this.oam32 = new Int32Array(this.oam.buffer);
    this.rom = new Uint8Array(0);
    this.rom16 = new Uint16Array(0);
    this.rom32 = new Int32Array(0);
    this.romSize = 0;
    this.biosLast = 0xE129F000 | 0;
    this.lastFetch = 0;
    this.cycles = 0;
    this.wait16 = new Uint8Array(16);
    this.wait32 = new Uint8Array(16);
    this.save = null; // SRAM/Flash/EEPROM
    this.gpio = null; // relógio do cartucho (GPIO), quando o jogo tem
    // tempo de acesso à ROM: N (não sequencial) e S (sequencial) por região, com o buffer de prefetch
    this.romN = new Uint8Array(16); this.romS = new Uint8Array(16);
    this.prefetch = false;
    this.seqNext = -1;      // próximo endereço que seria um acesso sequencial na ROM
    this.pfNext = -1;       // próximo endereço que o prefetch entrega (−1: buffer vazio/desligado)
    this.pfCount = 0;       // meias-palavras já no buffer
    this.pfTime = 0;        // instante em que o prefetch começou a buscar a próxima
    this.setWaitcnt(0);
  }

  loadRom(data) {
    // tamanho arredondado para múltiplo de 4 para as vistas tipadas
    const size = (data.length + 3) & ~3;
    const buf = new Uint8Array(size);
    buf.set(data);
    this.rom = buf;
    this.rom16 = new Uint16Array(buf.buffer);
    this.rom32 = new Int32Array(buf.buffer);
    this.romSize = data.length;
  }

  setWaitcnt(v) {
    const sram = N_TABLE[v & 3];
    const ws0n = N_TABLE[(v >>> 2) & 3], ws0s = (v & 0x10) ? 1 : 2;
    const ws1n = N_TABLE[(v >>> 5) & 3], ws1s = (v & 0x80) ? 1 : 4;
    const ws2n = N_TABLE[(v >>> 8) & 3], ws2s = (v & 0x400) ? 1 : 8;
    this.prefetch = (v & 0x4000) !== 0;
    if (!this.prefetch) this.pfNext = -1;
    const w = this.wait16, d = this.wait32;
    w[0] = 1; d[0] = 1; w[1] = 1; d[1] = 1;
    w[2] = 3; d[2] = 6; w[3] = 1; d[3] = 1; w[4] = 1; d[4] = 1;
    w[5] = 1; d[5] = 2; w[6] = 1; d[6] = 2; w[7] = 1; d[7] = 1;
    const ws = [[ws0n, ws0s], [ws1n, ws1s], [ws2n, ws2s]];
    for (let k = 0; k < 3; k++) {
      const [n, sq] = ws[k];
      for (const r of [8 + 2 * k, 9 + 2 * k]) {
        this.romN[r] = 1 + n; this.romS[r] = 1 + sq;
        w[r] = 0; d[r] = 0; // a ROM é cobrada por romCost
      }
    }
    w[14] = w[15] = d[14] = d[15] = 1 + sram;
  }

  // custo de um acesso à ROM: N ou S conforme a sequência (que quebra a cada 128 KB) e o prefetch
  romCost(addr, is32, isCode) {
    const reg = (addr >>> 24) & 0xF;
    const n = this.romN[reg], sq = this.romS[reg];
    const seq = addr === this.seqNext && (addr & 0x1FFFF) !== 0;
    this.seqNext = addr + (is32 ? 4 : 2);
    let cost;
    if (isCode && this.prefetch) {
      const now = this.gba.now();
      const need = is32 ? 2 : 1;
      if (seq && addr === this.pfNext) {
        const span = now - this.pfTime;
        const made = span > 0 ? Math.floor(span / sq) : 0;
        let avail = this.pfCount + made;
        const full = avail >= 8;
        if (full) avail = 8;
        if (avail >= need) {
          cost = 1;
          this.pfCount = avail - need;
          this.pfTime = full ? now + 1 : this.pfTime + made * sq;
        } else {
          const partial = span - made * sq;
          cost = Math.max(1, (need - avail) * sq - partial);
          this.pfCount = 0;
          this.pfTime = now + cost;
        }
      } else {
        cost = is32 ? n + sq : n;
        this.pfCount = 0;
        this.pfTime = now + cost;
      }
      this.pfNext = addr + (is32 ? 4 : 2);
    } else {
      cost = seq ? (is32 ? 2 * sq : sq) : (is32 ? n + sq : n);
      if (!isCode) this.pfNext = -1; // leitura de dados na ROM interrompe o prefetch
    }
    this.cycles += cost;
  }

  // custo de uma busca de instrução (ciclo S/N do pipeline)
  codeCost(addr, is32) {
    const reg = (addr >>> 24) & 0xF;
    if (reg >= 8 && reg <= 0xD) this.romCost(addr, is32, true);
    else this.cycles += is32 ? this.wait32[reg] : this.wait16[reg];
  }

  // valor da instrução (o custo da busca é cobrado à parte, no endereço que o pipeline realmente busca)
  fetch16(addr) {
    const reg = (addr >>> 24) & 0xF;
    let v;
    switch (reg) {
      case 0x0: v = this.bios16[(addr & 0x3FFF) >>> 1]; this.biosLast = this.bios32[((addr + 4) & 0x3FFC) >>> 2]; break;
      case 0x2: v = this.ewram16[(addr & 0x3FFFF) >>> 1]; break;
      case 0x3: v = this.iwram16[(addr & 0x7FFF) >>> 1]; break;
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
        const o = addr & 0x1FFFFFF;
        v = o < this.romSize ? this.rom16[o >>> 1] : ((addr >>> 1) & 0xFFFF);
        break;
      }
      default: { const c = this.cycles; v = this.read16(addr); this.cycles = c; break; }
    }
    this.lastFetch = v | (v << 16);
    return v;
  }

  fetch32(addr) {
    const reg = (addr >>> 24) & 0xF;
    let v;
    switch (reg) {
      case 0x0: v = this.bios32[(addr & 0x3FFF) >>> 2]; this.biosLast = this.bios32[((addr + 8) & 0x3FFC) >>> 2]; break;
      case 0x2: v = this.ewram32[(addr & 0x3FFFF) >>> 2]; break;
      case 0x3: v = this.iwram32[(addr & 0x7FFF) >>> 2]; break;
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
        const o = addr & 0x1FFFFFF;
        v = o < this.romSize ? this.rom32[o >>> 2] : ((((addr >>> 1) & 0xFFFF) | ((((addr >>> 1) + 1) & 0xFFFF) << 16)));
        break;
      }
      default: { const c = this.cycles; v = this.read32(addr); this.cycles = c; break; }
    }
    this.lastFetch = v;
    return v;
  }

  vramIndex(addr) {
    let o = addr & 0x1FFFF;
    if (o >= 0x18000) o -= 0x8000;
    return o;
  }

  // ---------------- leitura ----------------
  read8(addr) {
    const reg = (addr >>> 24) & 0xF;
    this.cycles += this.wait16[reg];
    switch (reg) {
      case 0x0:
        if (addr >= 0x4000) return (this.lastFetch >>> ((addr & 3) << 3)) & 0xFF;
        if ((this.gba.cpu.nextPC >>> 24) !== 0) return (this.biosLast >>> ((addr & 3) << 3)) & 0xFF;
        return this.bios[addr];
      case 0x2: return this.ewram[addr & 0x3FFFF];
      case 0x3: return this.iwram[addr & 0x7FFF];
      case 0x4: return this.gba.io.read8(addr & 0xFFFFFF);
      case 0x5: return this.pal[addr & 0x3FF];
      case 0x6: return this.vram[this.vramIndex(addr)];
      case 0x7: return this.oam[addr & 0x3FF];
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
        this.romCost(addr & ~1, false, false);
        const o = addr & 0x1FFFFFF;
        if (this.gpio !== null && o >= 0xC4 && o < 0xCA && this.gpio.readable) return (o & 1) ? 0 : this.gpio.read(o);
        if (o < this.romSize) return this.rom[o];
        if (reg === 0xD && this.save && this.save.isEeprom) return this.save.read16(addr) & 0xFF;
        return ((addr >>> 1) >>> ((addr & 1) << 3)) & 0xFF;
      }
      case 0xE: case 0xF:
        return this.save ? this.save.read8(addr & 0xFFFF) : 0xFF;
      default:
        return (this.lastFetch >>> ((addr & 3) << 3)) & 0xFF;
    }
  }

  read16(addr) {
    const reg = (addr >>> 24) & 0xF;
    this.cycles += this.wait16[reg];
    switch (reg) {
      case 0x0:
        if (addr >= 0x4000) return (this.lastFetch >>> ((addr & 2) << 3)) & 0xFFFF;
        if ((this.gba.cpu.nextPC >>> 24) !== 0) return (this.biosLast >>> ((addr & 2) << 3)) & 0xFFFF;
        return this.bios16[addr >>> 1];
      case 0x2: return this.ewram16[(addr & 0x3FFFF) >>> 1];
      case 0x3: return this.iwram16[(addr & 0x7FFF) >>> 1];
      case 0x4: return this.gba.io.read16(addr & 0xFFFFFE);
      case 0x5: return this.pal16[(addr & 0x3FF) >>> 1];
      case 0x6: return this.vram16[this.vramIndex(addr) >>> 1];
      case 0x7: return this.oam16[(addr & 0x3FF) >>> 1];
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
        this.romCost(addr & ~1, false, false);
        if (reg === 0xD && this.save && this.save.isEeprom && (this.romSize <= 0x1000000 || (addr & 0x1FFFFFF) >= 0x1FFFF00)) {
          return this.save.read16(addr);
        }
        const o = addr & 0x1FFFFFF;
        if (this.gpio !== null && o >= 0xC4 && o < 0xCA && this.gpio.readable) return this.gpio.read(o & ~1);
        if (o < this.romSize) return this.rom16[o >>> 1];
        return (addr >>> 1) & 0xFFFF;
      }
      case 0xE: case 0xF: {
        const b = this.save ? this.save.read8(addr & 0xFFFF) : 0xFF;
        return b * 0x101;
      }
      default:
        return (this.lastFetch >>> ((addr & 2) << 3)) & 0xFFFF;
    }
  }

  read32(addr) {
    const reg = (addr >>> 24) & 0xF;
    this.cycles += this.wait32[reg];
    switch (reg) {
      case 0x0:
        if (addr >= 0x4000) return this.lastFetch;
        if ((this.gba.cpu.nextPC >>> 24) !== 0) return this.biosLast;
        return this.bios32[addr >>> 2];
      case 0x2: return this.ewram32[(addr & 0x3FFFF) >>> 2];
      case 0x3: return this.iwram32[(addr & 0x7FFF) >>> 2];
      case 0x4: {
        const a = addr & 0xFFFFFC;
        return this.gba.io.read16(a) | (this.gba.io.read16(a + 2) << 16);
      }
      case 0x5: return this.pal32[(addr & 0x3FF) >>> 2];
      case 0x6: return this.vram32[this.vramIndex(addr) >>> 2];
      case 0x7: return this.oam32[(addr & 0x3FF) >>> 2];
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
        this.romCost(addr & ~3, true, false);
        const o = addr & 0x1FFFFFF;
        if (this.gpio !== null && o >= 0xC4 && o < 0xCA && this.gpio.readable) { const a = o & ~3; return this.gpio.read(a) | (this.gpio.read(a + 2) << 16); }
        if (o < this.romSize) return this.rom32[o >>> 2];
        return (((addr >>> 1) & 0xFFFF) | ((((addr >>> 1) + 1) & 0xFFFF) << 16));
      }
      case 0xE: case 0xF: {
        const b = this.save ? this.save.read8(addr & 0xFFFF) : 0xFF;
        return Math.imul(b, 0x01010101);
      }
      default:
        return this.lastFetch;
    }
  }

  // ---------------- escrita ----------------
  write8(addr, val) {
    const reg = (addr >>> 24) & 0xF;
    this.cycles += this.wait16[reg];
    val &= 0xFF;
    switch (reg) {
      case 0x2: this.ewram[addr & 0x3FFFF] = val; return;
      case 0x3: this.iwram[addr & 0x7FFF] = val; return;
      case 0x4: this.gba.io.write8(addr & 0xFFFFFF, val); return;
      case 0x5: this.pal16[(addr & 0x3FE) >>> 1] = val * 0x101; return;
      case 0x6: {
        const o = this.vramIndex(addr);
        const bitmap = (this.gba.io.regs[0] & 7) >= 3;
        if (o < (bitmap ? 0x14000 : 0x10000)) this.vram16[o >>> 1] = val * 0x101;
        return;
      }
      case 0x7: return; // OAM ignora escritas de 8 bits
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        this.romCost(addr & ~1, false, false);
        if (this.gpio !== null && reg === 8 && (addr & 0x1FFFFFF) >= 0xC4 && (addr & 0x1FFFFFF) < 0xCA && !(addr & 1)) this.gpio.write(addr & 0xFE, val);
        return;
      case 0xE: case 0xF:
        if (this.save) this.save.write8(addr & 0xFFFF, val);
        return;
      default: return;
    }
  }

  write16(addr, val) {
    const reg = (addr >>> 24) & 0xF;
    this.cycles += this.wait16[reg];
    val &= 0xFFFF;
    switch (reg) {
      case 0x2: this.ewram16[(addr & 0x3FFFF) >>> 1] = val; return;
      case 0x3: this.iwram16[(addr & 0x7FFF) >>> 1] = val; return;
      case 0x4: this.gba.io.write16(addr & 0xFFFFFE, val); return;
      case 0x5: this.pal16[(addr & 0x3FF) >>> 1] = val; return;
      case 0x6: this.vram16[this.vramIndex(addr) >>> 1] = val; return;
      case 0x7: this.oam16[(addr & 0x3FF) >>> 1] = val; return;
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        this.romCost(addr & ~1, false, false);
        if (reg === 0xD && this.save && this.save.isEeprom) this.save.write16(addr, val);
        else if (this.gpio !== null && reg === 8) { const o = addr & 0x1FFFFFE; if (o >= 0xC4 && o < 0xCA) this.gpio.write(o, val); }
        return;
      case 0xE: case 0xF:
        if (this.save) this.save.write8(addr & 0xFFFF, (val >>> ((addr & 1) << 3)) & 0xFF);
        return;
      default: return;
    }
  }

  write32(addr, val) {
    const reg = (addr >>> 24) & 0xF;
    this.cycles += this.wait32[reg];
    switch (reg) {
      case 0x2: this.ewram32[(addr & 0x3FFFF) >>> 2] = val; return;
      case 0x3: this.iwram32[(addr & 0x7FFF) >>> 2] = val; return;
      case 0x4: {
        const a = addr & 0xFFFFFC;
        this.gba.io.write32(a, val);
        return;
      }
      case 0x5: this.pal32[(addr & 0x3FF) >>> 2] = val; return;
      case 0x6: this.vram32[this.vramIndex(addr) >>> 2] = val; return;
      case 0x7: this.oam32[(addr & 0x3FF) >>> 2] = val; return;
      case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
        this.romCost(addr & ~3, true, false);
        if (this.gpio !== null && reg === 8) {
          const o = addr & 0x1FFFFFC;
          if (o >= 0xC4 && o < 0xCA) this.gpio.write(o, val & 0xFFFF);
          if (o + 2 >= 0xC4 && o + 2 < 0xCA) this.gpio.write(o + 2, (val >>> 16) & 0xFFFF);
        }
        return;
      case 0xE: case 0xF:
        if (this.save) this.save.write8(addr & 0xFFFF, (val >>> ((addr & 3) << 3)) & 0xFF);
        return;
      default: return;
    }
  }
}
