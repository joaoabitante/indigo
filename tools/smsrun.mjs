// Roda uma ROM de Master System / Game Gear sem interface e salva a tela: node tools/smsrun.mjs rom quadros saida.png [gg]
import fs from 'node:fs';
import { SMS, smsKind } from '../src/sms/sms.js';
import { writePng } from './run.js';
const [rom, frames = '120', out = '/tmp/sms.png', force] = process.argv.slice(2);
const data = new Uint8Array(fs.readFileSync(rom));
const gg = force ? force === 'gg' : smsKind(data, rom) === 'gg';
const m = new SMS(gg); m.loadRom(data);
const t0 = Date.now();
for (let f = 0; f < +frames; f++) m.runFrame();
const dt = (Date.now() - t0) / 1000;
writePng(out, m.vdp.fb, m.vdp.w, m.vdp.h, 2);
console.log(JSON.stringify({ gg, mapper: m.mapper, pc: m.cpu.pc.toString(16), speed: (+frames / 60 / dt).toFixed(1) + 'x', audio: m.apu.available() }));
