// Roda uma ROM de Neo Geo Pocket sem interface: node tools/ngprun.mjs rom quadros saida.png [ref.rgb]
// Com ref.rgb (saída do harness da referência) mostra a diferença de pixels.
import fs from 'node:fs';
import { NGP } from '../src/ngp/ngp.js';
import { writePng } from './run.js';
const [rom, frames = '120', out = '/tmp/ngp.png', ref] = process.argv.slice(2);
const m = new NGP(); m.loadRom(new Uint8Array(fs.readFileSync(rom)));
const keys = process.env.KEYS ? process.env.KEYS.split(',').map(s => s.split(':').map(Number)) : [];
const t0 = Date.now();
for (let f = 0; f < +frames; f++) { let k = 0; for (const [ff, mk] of keys) if (ff <= f) k = mk; m.setKeys(k); m.runFrame(); }
const dt = (Date.now() - t0) / 1000;
writePng(out, m.gfx.fb, 160, 152, 2);
const info = { title: m.title, color: m.color, pc: m.cpu.pc.toString(16), speed: (+frames / 60 / dt).toFixed(1) + 'x', audio: m.apu.available(), z80: m.z80on };
if (ref) {
  const d = fs.readFileSync(ref); const w = d[0] | (d[1] << 8), h = d[2] | (d[3] << 8);
  let diff = 0;
  for (let y = 0; y < Math.min(h, 152); y++) for (let x = 0; x < Math.min(w, 160); x++) {
    const p = m.gfx.fb[y * 160 + x], o = 4 + (y * w + x) * 3;
    const r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF;
    if (Math.abs(r - d[o]) > 24 || Math.abs(g - d[o + 1]) > 24 || Math.abs(b - d[o + 2]) > 24) diff++;
  }
  info.refSize = `${w}x${h}`; info.diffPixels = diff;
}
console.log(JSON.stringify(info));
