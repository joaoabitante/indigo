// Memória de save do cartucho: SRAM, Flash (64K/128K) e EEPROM (512B/8K)

export function detectSaveType(rom) {
  // procura as assinaturas deixadas pela biblioteca da Nintendo
  const tags = ['EEPROM_V', 'SRAM_V', 'SRAM_F_V', 'FLASH_V', 'FLASH512_V', 'FLASH1M_V'];
  const firsts = tags.map(t => t.charCodeAt(0));
  for (let i = 0; i < rom.length - 12; i += 4) {
    const c = rom[i];
    if (c !== 0x45 && c !== 0x53 && c !== 0x46) continue;
    for (let t = 0; t < tags.length; t++) {
      if (c !== firsts[t]) continue;
      const tag = tags[t];
      let ok = true;
      for (let k = 1; k < tag.length; k++) if (rom[i + k] !== tag.charCodeAt(k)) { ok = false; break; }
      if (ok) {
        if (tag.startsWith('EEPROM')) return 'eeprom';
        if (tag.startsWith('SRAM')) return 'sram';
        if (tag === 'FLASH1M_V') return 'flash128';
        return 'flash64';
      }
    }
  }
  return 'sram'; // padrão inofensivo
}

export class Save {
  constructor(type) {
    this.type = type;
    this.isEeprom = type === 'eeprom';
    this.dirty = false;
    if (type === 'sram') this.data = new Uint8Array(0x8000).fill(0xFF);
    else if (type === 'flash64') this.data = new Uint8Array(0x10000).fill(0xFF);
    else if (type === 'flash128') this.data = new Uint8Array(0x20000).fill(0xFF);
    else this.data = new Uint8Array(0x2000).fill(0xFF);
    // flash
    this.fState = 0; this.fCmd = 0; this.fId = false; this.fBank = 0;
    // eeprom
    this.eBits = [];
    this.eAddrBits = 0; // 6 ou 14 (definido pelo primeiro DMA)
    this.eReadBuf = null; this.eReadPos = 0;
  }

  load(bytes) {
    this.data.set(bytes.subarray(0, this.data.length));
  }

  // ---------------- SRAM / Flash ----------------
  read8(addr) {
    if (this.type === 'sram') return this.data[addr & 0x7FFF];
    if (this.type.startsWith('flash')) {
      if (this.fId && addr < 2) {
        if (this.type === 'flash128') return addr === 0 ? 0x62 : 0x13;
        return addr === 0 ? 0xBF : 0xD4;
      }
      return this.data[(this.fBank << 16) | addr];
    }
    return 0xFF;
  }

  write8(addr, v) {
    if (this.type === 'sram') { this.data[addr & 0x7FFF] = v; this.dirty = true; return; }
    if (!this.type.startsWith('flash')) return;
    if (this.fCmd === 0xA0) { // gravação de um byte
      this.data[(this.fBank << 16) | addr] &= v;
      this.fCmd = 0; this.dirty = true; return;
    }
    if (this.fCmd === 0xB0 && addr === 0) { this.fBank = (this.type === 'flash128') ? (v & 1) : 0; this.fCmd = 0; return; }
    if (this.fState === 0 && addr === 0x5555 && v === 0xAA) { this.fState = 1; return; }
    if (this.fState === 1 && addr === 0x2AAA && v === 0x55) { this.fState = 2; return; }
    if (this.fState === 2) {
      this.fState = 0;
      if (this.fCmd === 0x80) { // apagar
        if (addr === 0x5555 && v === 0x10) { this.data.fill(0xFF); this.dirty = true; }
        else if (v === 0x30) {
          const base = (this.fBank << 16) | (addr & 0xF000);
          this.data.fill(0xFF, base, base + 0x1000); this.dirty = true;
        }
        this.fCmd = 0; return;
      }
      if (addr === 0x5555) {
        switch (v) {
          case 0x90: this.fId = true; break;
          case 0xF0: this.fId = false; break;
          case 0x80: this.fCmd = 0x80; break;
          case 0xA0: this.fCmd = 0xA0; break;
          case 0xB0: this.fCmd = 0xB0; break;
        }
      }
      return;
    }
    this.fState = 0;
  }

  // ---------------- EEPROM (serial, 1 bit por acesso) ----------------
  dmaHint(count, isWrite) {
    if (this.eAddrBits) return;
    if (isWrite) {
      if (count === 9 || count === 73) this.resizeEeprom(6);
      else if (count === 17 || count === 81) this.resizeEeprom(14);
    }
  }

  resizeEeprom(bits) {
    this.eAddrBits = bits;
    if (bits === 6 && this.data.length !== 0x200) {
      const d = new Uint8Array(0x200); d.set(this.data.subarray(0, 0x200)); this.data = d;
    }
  }

  read16() {
    if (this.eReadBuf) {
      const bit = this.eReadBuf[this.eReadPos++];
      if (this.eReadPos >= this.eReadBuf.length) this.eReadBuf = null;
      return bit;
    }
    return 1;
  }

  write16(addr, v) {
    this.eBits.push(v & 1);
    const bits = this.eBits;
    if (bits.length < 2) return;
    const ab = this.eAddrBits || 14;
    const cmd = (bits[0] << 1) | bits[1];
    if (cmd === 3) { // leitura
      if (bits.length < 2 + ab + 1) return;
      let a = 0;
      for (let k = 0; k < ab; k++) a = (a << 1) | bits[2 + k];
      a = (a & 0x3FF) * 8;
      const out = [0, 0, 0, 0];
      for (let k = 0; k < 8; k++) {
        const byte = this.data[(a + k) % this.data.length];
        for (let b = 7; b >= 0; b--) out.push((byte >>> b) & 1);
      }
      this.eReadBuf = out; this.eReadPos = 0;
      this.eBits = [];
    } else if (cmd === 2) { // escrita
      if (bits.length < 2 + ab + 64 + 1) return;
      let a = 0;
      for (let k = 0; k < ab; k++) a = (a << 1) | bits[2 + k];
      a = (a & 0x3FF) * 8;
      for (let k = 0; k < 8; k++) {
        let byte = 0;
        for (let b = 0; b < 8; b++) byte = (byte << 1) | bits[2 + ab + k * 8 + b];
        this.data[(a + k) % this.data.length] = byte;
      }
      this.dirty = true;
      this.eBits = [];
    } else {
      this.eBits = [];
    }
  }
}
