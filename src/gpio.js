// Portas GPIO do cartucho (0x080000C4–C9) com o relógio S-3511A: Pokémon Ruby/Sapphire/Emerald, Boktai,
// Rockman EXE 4.5. O relógio segue a hora real do aparelho mais um deslocamento que o jogo pode ajustar.
//
//   C4 dados (bit0 SCK, bit1 SIO, bit2 CS)   C6 direção (1 = saída do GBA)   C8 controle (bit0 = leitura liberada)

const RTC_CODES = ['AXV', 'AXP', 'BPE', 'U3I', 'U32', 'U33', 'BR4'];

export function hasRtc(rom, gameCode) {
  if (RTC_CODES.includes((gameCode || '').slice(0, 3))) return true;
  // assinatura da biblioteca de relógio da Nintendo
  const tag = [0x53, 0x49, 0x49, 0x52, 0x54, 0x43, 0x5F, 0x56]; // SIIRTC_V
  for (let i = 0; i < rom.length - 8; i++) {
    if (rom[i] !== 0x53) continue;
    let k = 1;
    while (k < 8 && rom[i + k] === tag[k]) k++;
    if (k === 8) return true;
  }
  return false;
}

const bcd = (v) => ((Math.floor(v / 10) % 10) << 4) | (v % 10);
const unbcd = (b) => (b >> 4) * 10 + (b & 0xF);
const IDLE = 0, COMMAND = 1, READ = 2, WRITE = 3;

export class CartRtc {
  constructor() {
    this.offset = 0;        // ms somados à hora real (ajuste feito pelo jogo)
    this.status = 0x40;     // modo 24 horas
    this.dir = 0; this.ctrl = 0; this.pins = 0; this.out = 0;
    this.state = IDLE; this.bits = 0; this.cmd = 0;
    this.data = new Uint8Array(8); this.len = 0; this.reg = 0;
  }

  get readable() { return (this.ctrl & 1) !== 0; }

  read(off) {
    if (off === 0xC4) return ((this.pins & this.dir) | (this.out & ~this.dir)) & 0xF;
    if (off === 0xC6) return this.dir & 0xF;
    if (off === 0xC8) return this.ctrl & 1;
    return 0;
  }

  write(off, v) {
    if (off === 0xC6) { this.dir = v & 0xF; return; }
    if (off === 0xC8) { this.ctrl = v & 1; return; }
    if (off !== 0xC4) return;
    const prev = this.pins;
    this.pins = (prev & ~this.dir) | (v & this.dir) & 0xF;
    const sck = this.pins & 1, sio = (this.pins >> 1) & 1, cs = (this.pins >> 2) & 1;
    if (!cs) { this.state = IDLE; return; }
    if (!(prev & 4)) { this.state = COMMAND; this.bits = 0; this.cmd = 0; return; } // CS subiu: começa um comando
    if (!(!(prev & 1) && sck)) return; // só a borda de subida do SCK move bits
    switch (this.state) {
      case COMMAND:
        this.cmd = (this.cmd << 1) | sio;
        if (++this.bits === 8) this.command();
        break;
      case READ: {
        const b = this.bits;
        this.out = (this.out & ~2) | (((this.data[b >> 3] >> (b & 7)) & 1) << 1);
        if (++this.bits >= this.len * 8) this.state = IDLE;
        break;
      }
      case WRITE: {
        const b = this.bits;
        if (sio) this.data[b >> 3] |= 1 << (b & 7);
        if (++this.bits >= this.len * 8) { this.commit(); this.state = IDLE; }
        break;
      }
    }
  }

  command() {
    let c = this.cmd & 0xFF;
    // alguns jogos mandam o comando com os bits invertidos (0110 no fim): normaliza
    if ((c >> 4) !== 6 && (c & 0xF) === 6) { let r = 0; for (let k = 0; k < 8; k++) r |= ((c >> k) & 1) << (7 - k); c = r; }
    this.bits = 0;
    if ((c >> 4) !== 6) { this.state = IDLE; return; }
    const reg = (c >> 1) & 7, read = c & 1;
    this.reg = reg;
    this.data.fill(0);
    const LEN = [0, 1, 7, 3, 0, 0, 0, 0];
    this.len = LEN[reg];
    if (reg === 0) { this.status = 0x40; this.offset = 0; this.state = IDLE; return; } // reset
    if (!this.len) { this.state = IDLE; return; }
    if (read) {
      if (reg === 1) this.data[0] = this.status;
      else this.fillTime(reg === 2);
      this.state = READ;
    } else this.state = WRITE;
  }

  fillTime(withDate) {
    const t = new Date(Date.now() + this.offset);
    let h = t.getHours();
    let hb = bcd(h);
    if (!(this.status & 0x40)) { hb = bcd(h % 12) | (h >= 12 ? 0x80 : 0); }
    const parts = withDate
      ? [bcd(t.getFullYear() % 100), bcd(t.getMonth() + 1), bcd(t.getDate()), t.getDay(), hb, bcd(t.getMinutes()), bcd(t.getSeconds())]
      : [hb, bcd(t.getMinutes()), bcd(t.getSeconds())];
    this.data.set(parts);
  }

  commit() {
    if (this.reg === 1) { this.status = this.data[0] & 0x6A; return; }
    const d = this.data;
    const now = new Date(Date.now() + this.offset);
    let y = now.getFullYear(), mo = now.getMonth(), da = now.getDate(), i = 0;
    if (this.reg === 2) { y = 2000 + unbcd(d[0]); mo = unbcd(d[1] & 0x1F) - 1; da = unbcd(d[2] & 0x3F); i = 4; }
    let h = unbcd(d[i] & 0x3F);
    if (!(this.status & 0x40) && (d[i] & 0x80)) h += 12;
    const target = new Date(y, mo, da, h, unbcd(d[i + 1] & 0x7F), unbcd(d[i + 2] & 0x7F));
    if (!isNaN(target)) this.offset = target.getTime() - Date.now();
  }
}
