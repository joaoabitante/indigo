// Roda uma ROM de teste sem interface: node tools/gbtest.mjs rom.gb [segundos] [dmg|cgb|auto] [saida.rgba]
// Mostra o texto enviado pela serial (testes do Blargg) e os registradores no fim (testes do Mooneye).
import fs from 'node:fs';
import { GB } from '../src/gb/gb.js';
const [rom, secs = '10', mode = 'auto', out] = process.argv.slice(2);
const gb = new GB();
gb.loadRom(new Uint8Array(fs.readFileSync(rom)), null, mode);
const frames = Math.round(Number(secs) * 59.73);
const t0 = Date.now();
let i = 0;
for (; i < frames; i++) {
  gb.runFrame();
  if (/Passed|Failed/.test(gb.serialOut) && i > 60) break;
  const ram = gb.cart.ram;
  if (ram.length > 4 && ram[1] === 0xDE && ram[2] === 0xB0 && ram[3] === 0x61 && ram[0] !== 0x80 && i > 30) break;
  // Mooneye: LD B,B (0x40) ao terminar
  if (gb.read(gb.pc) === 0x40 && gb.b === 3 && gb.c === 5 && gb.d === 8) break;
}
const ms = Date.now() - t0;
if (out) fs.writeFileSync(out, Buffer.from(gb.ppu.fb.buffer));
const ram = gb.cart.ram;
let blargg = '';
if (ram.length > 4 && ram[1] === 0xDE && ram[2] === 0xB0 && ram[3] === 0x61) { blargg = 'status=' + ram[0]; let k = 4; while (k < ram.length && ram[k]) blargg += String.fromCharCode(ram[k++]); blargg = blargg.replace(/\s+/g, ' ').trim(); }
const regs = `B=${gb.b} C=${gb.c} D=${gb.d} E=${gb.e} H=${gb.h} L=${gb.l}`;
const moon = gb.b === 3 && gb.c === 5 && gb.d === 8 && gb.e === 13 && gb.h === 21 && gb.l === 34;
console.log(JSON.stringify({ rom: rom.split('/').slice(-2).join('/'), frames: i, speed: (i / 59.73 / (ms / 1000)).toFixed(1) + 'x', serial: gb.serialOut.trim().replace(/\s+/g, ' ').slice(-120), moon, regs, blargg }));
