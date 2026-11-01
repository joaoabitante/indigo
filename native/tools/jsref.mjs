// Referência para o porte: roda a ROM no núcleo JavaScript (src/) e imprime, a cada quadro, os mesmos
// hashes de imagem e som que native/tools/runrom.cpp imprime. Saídas iguais = comportamento igual.
// uso: node native/tools/jsref.mjs rom quadros [auto|dmg|cgb|sms|gg|gba] [teclasACada]
import fs from 'node:fs';
import { GB } from '../../src/gb/gb.js';
import { SMS, smsKind } from '../../src/sms/sms.js';
import { NGP, isNgpRom } from '../../src/ngp/ngp.js';
import { GBA } from '../../src/gba.js';

// relógio fixo (o Neo Geo Pocket lê data e hora; runrom.cpp fixa o mesmo instante): 19/10/2026 12:34:56, segunda
const RealDate = Date;
globalThis.Date = class extends RealDate { constructor(...a) { if (a.length) super(...a); else super(2026, 9, 19, 12, 34, 56); } };
// o relógio dos cartuchos de GBA usa Date.now(): o mesmo instante
const FIXED_NOW = new RealDate(2026, 9, 19, 12, 34, 56).getTime();
globalThis.Date.now = () => FIXED_NOW;

const [rom, framesArg, mode = 'auto', everyArg = '0'] = process.argv.slice(2);
const frames = parseInt(framesArg, 10), every = Number(everyArg);
const keys = (f) => (every && (f % every) < 3) ? (1 << (f % 8)) : 0;
const fnv = (words, n, h = 0x811C9DC5) => { for (let i = 0; i < n; i++) { h ^= words[i]; h = Math.imul(h, 0x01000193); } return h >>> 0; };
const bits = new Uint32Array(1), fl = new Float32Array(bits.buffer);
const hex = (v) => (v >>> 0).toString(16).padStart(8, '0');

const data = new Uint8Array(fs.readFileSync(rom));
const gbLogo = data.length >= 0x150 && data[0x104] === 0xCE && data[0x105] === 0xED && data[0x106] === 0x66 && data[0x107] === 0x66;
const n = rom.toLowerCase();
const gbaLogo = data.length >= 0xC0 && data[4] === 0x24 && data[5] === 0xFF && data[6] === 0xAE && data[7] === 0x51;
const isGba = mode === 'gba' || (mode === 'auto' && !isNgpRom(data) && (gbaLogo || data[0xB2] === 0x96 && /\.(gba|agb|bin)$/.test(n) ||
  (!gbLogo && !smsKind(data, n) && /\.(gba|agb)$/.test(n))));
const kind = isGba ? 'gba' : isNgpRom(data) ? 'ngp' : mode === 'sms' || mode === 'gg' ? mode : (mode === 'dmg' || mode === 'cgb' || gbLogo) ? 'gb' : smsKind(data, rom.toLowerCase()) || 'gb';

let emu, w, h;
if (kind === 'gb') { emu = new GB(); emu.loadRom(data, null, mode === 'dmg' || mode === 'cgb' ? mode : 'auto'); w = 160; h = 144; }
else if (kind === 'gba') {
  emu = new GBA();
  if (process.env.INDIGO_GBA_BIOS) emu.loadBios(new Uint8Array(fs.readFileSync(process.env.INDIGO_GBA_BIOS)));
  emu.loadRom(data); w = 240; h = 160;
}
else if (kind === 'ngp') { emu = new NGP(); emu.loadRom(data); w = 160; h = 152; }
else { emu = new SMS(kind === 'gg'); emu.loadRom(data); w = emu.vdp.w; h = emu.vdp.h; }
const apu = emu.apu;
const out = [];
for (let f = 0; f < frames; f++) {
  emu.setKeys(keys(f));
  emu.runFrame();
  let ah = 0x811C9DC5, n = 0;
  while (apu.rpos !== apu.wpos) {
    fl[0] = apu.bufL[apu.rpos]; ah ^= bits[0]; ah = Math.imul(ah, 0x01000193);
    fl[0] = apu.bufR[apu.rpos]; ah ^= bits[0]; ah = Math.imul(ah, 0x01000193);
    apu.rpos = (apu.rpos + 1) % apu.bufSize; n++;
  }
  out.push(`${f} ${hex(fnv(emu.ppu.fb, w * h))} ${hex(ah)} ${n}`);
}
out.push('serial: ' + (emu.serialOut || '').replace(/[\r\n]/g, ' '));
// GBA: mesmo formato de web/emu-core.js (dados + "RTC1" + ajuste do relógio quando houver)
function gbaSave() {
  const s = emu.bus.save, rtc = emu.bus.gpio;
  if (!rtc || !rtc.offset) return s.data.slice();
  const o = new Uint8Array(s.data.length + 16);
  o.set(s.data); o.set([0x52, 0x54, 0x43, 0x31], s.data.length);
  new DataView(o.buffer).setFloat64(s.data.length + 4, rtc.offset, true);
  o[s.data.length + 12] = rtc.status;
  return o;
}
const save = kind === 'gba' ? gbaSave() : kind === 'ngp' ? emu.exportSave() : kind === 'gb' ? (emu.cart.hasSave() ? emu.cart.exportSave() : new Uint8Array(0)) : (emu.hasSave() ? emu.exportSave() : new Uint8Array(0));
let sh = 0x811C9DC5; for (const b of save) { sh ^= b; sh = Math.imul(sh, 0x01000193); }
out.push(`save: ${save.length} ${hex(sh)}`);
process.stdout.write(out.join('\n') + '\n');
