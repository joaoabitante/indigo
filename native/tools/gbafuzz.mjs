// Teste diferencial do GBA inteiro (lado JavaScript): monta ROMs aleatórias (código ARM/Thumb aleatório com
// registradores apontando para as memórias), roda alguns quadros e imprime um hash por caso (registradores,
// memórias, E/S, imagem, som e relógio). native/tools/gbafuzz.cpp faz o mesmo com o núcleo em C++.
// uso: node native/tools/gbafuzz.mjs casos [inicio]
import { GBA } from '../../src/gba.js';

const [countArg, startArg = '1'] = process.argv.slice(2);
const count = +countArg, start = +startArg;
let x = 0;
const next = () => { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x; };
const BASES = [0x02000000, 0x03000000, 0x04000000, 0x05000000, 0x06000000, 0x07000000, 0x08000000, 0x0E000000];

function makeRom() {
  const rom = new Uint8Array(0x8000);
  const w32 = (o, v) => { rom[o] = v; rom[o + 1] = v >>> 8; rom[o + 2] = v >>> 16; rom[o + 3] = v >>> 24; };
  for (let o = 0; o < rom.length; o += 4) w32(o, next());
  w32(0, 0xEA00002E); // b 0xC0
  for (let j = 0; j < 13; j++) w32(0xC0 + 4 * j, (0xE59F0000 | (j << 12) | ((0xF8 + 4 * j) - (0xC8 + 4 * j))) >>> 0); // ldr rj, [pc, #..]
  w32(0xF4, 0xEA00000C); // b 0x12C
  for (let j = 0; j < 13; j++) { const r = next(); w32(0xF8 + 4 * j, (r & 1) ? r : (BASES[(r >>> 1) & 7] + ((r >>> 4) & 0x3FFC)) >>> 0); }
  const thumb = next() & 1;
  let o = 0x12C;
  if (thumb) { w32(o, 0xE28FC001); w32(o + 4, 0xE12FFF1C); o += 8; } // add r12, pc, #1; bx r12
  for (; o < 0x7000; o += 4) {
    let v = next();
    if (!thumb && (v & 3) !== 0) v = ((v & 0x0FFFFFFF) | 0xE0000000) >>> 0; // 3/4 com condição "sempre"
    w32(o, v);
  }
  return rom;
}

const fnv8 = (a, h) => { for (let i = 0; i < a.length; i++) { h ^= a[i]; h = Math.imul(h, 0x01000193); } return h; };
const fnvW = (v, h) => { h ^= v >>> 0; return Math.imul(h, 0x01000193); };
const hex = (v) => (v >>> 0).toString(16).padStart(8, '0');
const out = [];
for (let i = start; i < start + count; i++) {
  x = (Math.imul(i, 0x85EBCA6B) >>> 0) || 1;
  const rom = makeRom();
  const g = new GBA();
  g.loadRom(rom);
  const keys = next() & 0x3FF;
  for (let f = 0; f < 2; f++) { g.setKeys(f ? keys : 0); g.runFrame(); }
  let h = 0x811C9DC5;
  for (let k = 0; k < 16; k++) h = fnvW(g.cpu.r[k], h);
  h = fnvW(g.cpu.cpsr, h); h = fnvW(g.now() % 0x100000000, h); h = fnvW(+g.cpu.halted, h);
  const b = g.bus;
  for (const m of [b.ewram, b.iwram, b.pal, b.vram, b.oam, b.save.data]) h = fnv8(m, h);
  for (let k = 0; k < 0x200; k++) h = fnvW(g.io.regs[k], h);
  h = fnvW(g.io.ie, h); h = fnvW(g.io.if, h); h = fnvW(g.io.ime, h);
  for (let k = 0; k < g.ppu.fb.length; k++) h = fnvW(g.ppu.fb[k], h);
  const a = g.apu; let n = 0;
  while (a.rpos !== a.wpos) { h = fnvW(Math.round(a.bufL[a.rpos] * 1048576), h); h = fnvW(Math.round(a.bufR[a.rpos] * 1048576), h); a.rpos = (a.rpos + 1) % a.bufSize; n++; }
  out.push(`${i} ${hex(h)} ${n}`);
  if (out.length >= 100) { process.stdout.write(out.join('\n') + '\n'); out.length = 0; }
}
if (out.length) process.stdout.write(out.join('\n') + '\n');
