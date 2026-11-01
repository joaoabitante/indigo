// roda várias ROMs e salva PNG 1x de cada: node tools/batch.js quadros dirSaida rom1 rom2 ...
import fs from 'node:fs';
import path from 'node:path';
import { GBA } from '../src/gba.js';
import { writePng } from './run.js';
const [, , framesArg, outDir, ...roms] = process.argv;
fs.mkdirSync(outDir, { recursive: true });
for (const rom of roms) {
  const name = path.basename(rom, '.gba');
  try {
    const gba = new GBA();
    gba.loadRom(new Uint8Array(fs.readFileSync(rom)));
    const t0 = Date.now();
    for (let f = 0; f < +framesArg; f++) gba.runFrame();
    const fps = (+framesArg / ((Date.now() - t0) / 1000)).toFixed(0);
    writePng(path.join(outDir, name + '.png'), gba.ppu.fb, 240, 160, 1);
    console.log(`${name}: ${fps} fps, PC=${(gba.cpu.nextPC >>> 0).toString(16)}`);
  } catch (e) { console.log(`${name}: ERRO ${e.message}`); }
}
