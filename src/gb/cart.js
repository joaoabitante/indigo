// Cartucho do Game Boy / Game Boy Color: cabeçalho, controladores de memória (MBC1, MBC2, MBC3 com
// relógio, MBC5) e o save da bateria (RAM + relógio no formato de 48 bytes usado por outros emuladores).

const RAM_SIZES = [0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000];
const BATTERY = new Set([0x03, 0x06, 0x09, 0x0D, 0x0F, 0x10, 0x13, 0x1B, 0x1E, 0xFF]);

export function cartInfo(rom) {
  let title = '';
  const cgbFlag = rom[0x143];
  const titleEnd = (cgbFlag & 0x80) ? 0x143 : 0x144;
  for (let k = 0x134; k < titleEnd; k++) {
    const c = rom[k];
    if (!c) break;
    if (c >= 0x20 && c < 0x7F) title += String.fromCharCode(c);
  }
  const type = rom[0x147];
  let mbc = 'none';
  if (type >= 0x01 && type <= 0x03) mbc = 'mbc1';
  else if (type === 0x05 || type === 0x06) mbc = 'mbc2';
  else if (type >= 0x0F && type <= 0x13) mbc = 'mbc3';
  else if (type >= 0x19 && type <= 0x1E) mbc = 'mbc5';
  else if (type === 0xFF) mbc = 'mbc1'; // HuC1 se comporta como MBC1 para os jogos
  let ramSize = RAM_SIZES[rom[0x149]] || 0;
  if (mbc === 'mbc2') ramSize = 512;
  if (type === 0x08 || type === 0x09) ramSize = ramSize || 0x2000;
  // código de licença + checksum do cabeçalho: identificam a cópia (o título sozinho às vezes se repete)
  const code = (rom[0x14D] | (rom[0x14E] << 8) | (rom[0x14F] << 16)).toString(16).toUpperCase().padStart(6, '0');
  return {
    title: title.trim(),
    cgb: cgbFlag === 0x80 || cgbFlag === 0xC0,
    cgbOnly: cgbFlag === 0xC0,
    type, mbc, ramSize,
    battery: BATTERY.has(type),
    rtc: type === 0x0F || type === 0x10,
    rumble: type >= 0x1C && type <= 0x1E,
    code,
  };
}

export class Cart {
  constructor(rom) {
    // ROM com tamanho potência de 2 (mínimo 32 KB) para as máscaras de banco
    let size = 0x8000;
    while (size < rom.length) size <<= 1;
    this.rom = new Uint8Array(size).fill(0xFF);
    this.rom.set(rom);
    this.romBanks = size >> 14;
    this.info = cartInfo(this.rom);
    this.mbc = this.info.mbc;
    this.ram = new Uint8Array(Math.max(this.info.ramSize, 0)).fill(0xFF);
    this.ramBanks = Math.max(1, this.info.ramSize >> 13);
    this.ramOn = false;
    this.bank1 = 1; this.bank2 = 0; this.mode = 0;
    this.romBank = 1; this.ramBank = 0;
    this.rom0 = 0; this.romX = 0x4000; this.ramOff = 0;
    this.dirty = false;
    this.rumble = false;
    // MBC1 multicart (coletâneas de 1 MB): o logo da Nintendo se repete no início do 2º jogo (banco 0x10)
    this.multicart = this.mbc === 'mbc1' && size === 0x100000 && this.sameLogo(0x40104);
    // relógio (MBC3)
    this.rtc = new Int32Array(5);       // s, m, h, dia (8 bits baixos), dia alto/halt/carry
    this.rtcLatched = new Int32Array(5);
    this.rtcLatchArm = false;
    this.rtcStamp = Date.now();         // momento a que os registradores do relógio se referem
    this.rtcSub = 0;                    // milissegundos acumulados abaixo de 1 s
    this.update();
  }

  update() {
    const mask = this.romBanks - 1;
    if (this.mbc === 'mbc1') {
      const sh = this.multicart ? 4 : 5, b1 = this.multicart ? this.bank1 & 0xF : this.bank1;
      this.rom0 = ((this.mode ? (this.bank2 << sh) : 0) & mask) << 14;
      this.romX = (((this.bank2 << sh) | b1) & mask) << 14;
      this.ramOff = ((this.mode ? this.bank2 : 0) % this.ramBanks) << 13;
    } else if (this.mbc === 'mbc2') {
      this.rom0 = 0; this.romX = (this.romBank & mask) << 14; this.ramOff = 0;
    } else if (this.mbc === 'mbc3') {
      this.rom0 = 0; this.romX = ((this.romBank || 1) & mask) << 14;
      this.ramOff = ((this.ramBank & 3) % this.ramBanks) << 13;
    } else if (this.mbc === 'mbc5') {
      this.rom0 = 0; this.romX = (this.romBank & mask) << 14;
      this.ramOff = ((this.ramBank & (this.info.rumble ? 7 : 0xF)) % this.ramBanks) << 13;
    } else {
      this.rom0 = 0; this.romX = 0x4000 & ((this.romBanks << 14) - 1); this.ramOff = 0;
    }
  }

  sameLogo(off) {
    if (off + 48 > this.rom.length) return false;
    for (let k = 0; k < 48; k++) if (this.rom[off + k] !== this.rom[0x104 + k]) return false;
    return true;
  }

  readRom(a) { return a < 0x4000 ? this.rom[this.rom0 | a] : this.rom[this.romX | (a & 0x3FFF)]; }

  writeRom(a, v) {
    switch (this.mbc) {
      case 'mbc1':
        if (a < 0x2000) this.ramOn = (v & 0xF) === 0xA;
        else if (a < 0x4000) { this.bank1 = (v & 0x1F) || 1; }
        else if (a < 0x6000) { this.bank2 = v & 3; }
        else this.mode = v & 1;
        break;
      case 'mbc2':
        if (a < 0x4000) {
          if (a & 0x100) this.romBank = (v & 0xF) || 1;
          else this.ramOn = (v & 0xF) === 0xA;
        }
        break;
      case 'mbc3':
        if (a < 0x2000) this.ramOn = (v & 0xF) === 0xA;
        else if (a < 0x4000) this.romBank = (v & 0x7F) || 1;
        else if (a < 0x6000) this.ramBank = v & 0xF;
        else {
          if (v === 0 ) this.rtcLatchArm = true;
          else if (v === 1 && this.rtcLatchArm) { this.rtcTick(); this.rtcLatched.set(this.rtc); this.rtcLatchArm = false; }
          else this.rtcLatchArm = false;
        }
        break;
      case 'mbc5':
        if (a < 0x2000) this.ramOn = (v & 0xF) === 0xA;
        else if (a < 0x3000) this.romBank = (this.romBank & 0x100) | v;
        else if (a < 0x4000) this.romBank = (this.romBank & 0xFF) | ((v & 1) << 8);
        else if (a < 0x6000) { this.ramBank = v & 0xF; if (this.info.rumble) this.rumble = (v & 8) !== 0; }
        break;
      default:
        break;
    }
    this.update();
  }

  readRam(a) {
    if (!this.ramOn) return 0xFF;
    if (this.mbc === 'mbc2') return this.ram[a & 0x1FF] | 0xF0;
    if (this.mbc === 'mbc3' && this.ramBank >= 8) {
      if (!this.info.rtc || this.ramBank > 0xC) return 0xFF;
      const r = this.rtcLatched[this.ramBank - 8];
      return [r & 0x3F, r & 0x3F, r & 0x1F, r & 0xFF, r & 0xC1][this.ramBank - 8] | [0xC0, 0xC0, 0xE0, 0, 0x3E][this.ramBank - 8];
    }
    if (!this.ram.length) return 0xFF;
    return this.ram[(this.ramOff | (a & 0x1FFF)) % this.ram.length];
  }

  writeRam(a, v) {
    if (!this.ramOn) return;
    if (this.mbc === 'mbc2') { this.ram[a & 0x1FF] = v & 0xF; this.dirty = true; return; }
    if (this.mbc === 'mbc3' && this.ramBank >= 8) {
      if (!this.info.rtc || this.ramBank > 0xC) return;
      this.rtcTick();
      const k = this.ramBank - 8;
      this.rtc[k] = [v & 0x3F, v & 0x3F, v & 0x1F, v, v & 0xC1][k];
      if (k === 0) this.rtcSub = 0;
      this.rtcLatched[k] = this.rtc[k];
      this.dirty = true;
      return;
    }
    if (!this.ram.length) return;
    this.ram[(this.ramOff | (a & 0x1FFF)) % this.ram.length] = v;
    this.dirty = true;
  }

  // avança o relógio do cartucho pelo tempo real decorrido (como a bateria faria)
  rtcTick() {
    if (!this.info.rtc) return;
    const now = Date.now();
    let ms = now - this.rtcStamp;
    this.rtcStamp = now;
    if (ms <= 0 || (this.rtc[4] & 0x40)) return;
    ms += this.rtcSub;
    let secs = Math.floor(ms / 1000);
    this.rtcSub = ms % 1000;
    this.addSeconds(secs);
  }

  addSeconds(secs) {
    const r = this.rtc;
    while (secs > 0) {
      const step = Math.min(secs, 86400 * 30);
      secs -= step;
      let s = r[0] + step;
      // valores fora da faixa (gravados pelo jogo) dão a volta como no hardware: tratamos por aritmética simples
      let m = r[1] + Math.floor(s / 60); s %= 60;
      let h = r[2] + Math.floor(m / 60); m %= 60;
      let d = ((r[4] & 1) << 8 | r[3]) + Math.floor(h / 24); h %= 24;
      let hi = r[4] & 0xC0;
      if (d > 511) { d &= 511; hi |= 0x80; }
      r[0] = s; r[1] = m; r[2] = h; r[3] = d & 0xFF; r[4] = hi | (d >> 8);
    }
  }

  // ---------------- save da bateria
  hasSave() { return this.info.battery && (this.ram.length > 0 || this.info.rtc); }

  exportSave() {
    const rtcBytes = this.info.rtc ? 48 : 0;
    const out = new Uint8Array(this.ram.length + rtcBytes);
    out.set(this.ram);
    if (rtcBytes) {
      this.rtcTick();
      const dv = new DataView(out.buffer, this.ram.length);
      for (let k = 0; k < 5; k++) { dv.setUint32(k * 4, this.rtc[k], true); dv.setUint32(20 + k * 4, this.rtcLatched[k], true); }
      const t = Math.floor(this.rtcStamp / 1000);
      dv.setUint32(40, t >>> 0, true); dv.setUint32(44, Math.floor(t / 4294967296), true);
    }
    return out;
  }

  importSave(bytes) {
    if (!bytes || !bytes.length) return;
    this.ram.set(bytes.subarray(0, this.ram.length));
    if (this.info.rtc && bytes.length >= this.ram.length + 44) {
      const dv = new DataView(bytes.buffer, bytes.byteOffset + this.ram.length);
      for (let k = 0; k < 5; k++) { this.rtc[k] = dv.getUint32(k * 4, true) & 0xFF; this.rtcLatched[k] = dv.getUint32(20 + k * 4, true) & 0xFF; }
      const t = dv.getUint32(40, true) + (bytes.length >= this.ram.length + 48 ? dv.getUint32(44, true) * 4294967296 : 0);
      this.rtcStamp = t > 0 ? t * 1000 : Date.now();
      this.rtcSub = 0;
      this.rtcTick(); // conta o tempo em que o jogo ficou desligado
    }
  }

  // estado para save state
  state() {
    return { ram: this.ram.slice(), rtc: Array.from(this.rtc), rtcL: Array.from(this.rtcLatched), stamp: this.rtcStamp,
      regs: [this.ramOn ? 1 : 0, this.bank1, this.bank2, this.mode, this.romBank, this.ramBank] };
  }
  restore(s) {
    this.ram.set(s.ram.subarray ? s.ram.subarray(0, this.ram.length) : s.ram);
    this.rtc.set(s.rtc); this.rtcLatched.set(s.rtcL); this.rtcStamp = s.stamp;
    [this.ramOn, this.bank1, this.bank2, this.mode, this.romBank, this.ramBank] = s.regs;
    this.ramOn = !!this.ramOn;
    this.update();
  }
}
