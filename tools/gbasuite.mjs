// Roda os testes do tipo jsmolka/alyosha (tela "All tests passed") e diz quais passam.
// node tools/gbasuite.mjs referencia_que_passa.gba rom1.gba rom2.gba ...
import fs from 'node:fs';
import { GBA } from '../src/gba.js';
const [ref, ...roms] = process.argv.slice(2);
// compara só o desenho do texto (pixel diferente do fundo), não as cores
const hash = (fb) => { let h = 0x811C9DC5; const bg = fb[0]; for (let i = 0; i < fb.length; i++) { h ^= fb[i] === bg ? 0 : 1; h = Math.imul(h, 0x01000193); } return h >>> 0; };
function run(rom) {
  const g = new GBA(); g.loadRom(new Uint8Array(fs.readFileSync(rom)));
  for (let f = 0; f < 120; f++) g.runFrame();
  return g;
}
const passHash = hash(run(ref).ppu.fb);
let ok = 0, bad = [];
for (const r of roms) {
  let res;
  try { res = hash(run(r).ppu.fb) === passHash; } catch (e) { res = false; }
  if (res) ok++; else bad.push(r.split('/').slice(-2).join('/'));
}
console.log(`passam ${ok}/${roms.length}`);
for (const b of bad) console.log('  FALHA', b);
