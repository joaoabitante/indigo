import fs from 'node:fs';
import { GBA } from '../src/gba.js';
const gba = new GBA();
gba.loadRom(new Uint8Array(fs.readFileSync(process.argv[2])));
const cpu = gba.cpu, hist = [];
for (let i = 0; i < 5e6; i++) {
  const pc = cpu.nextPC >>> 0;
  if ((pc >>> 24) !== 8 && (pc>>>24)!==3 && (pc>>>24)!==2 ) { console.log('saiu da ROM em', pc.toString(16)); break; }
  const ins = cpu.t ? gba.bus.read16(pc) : gba.bus.read32(pc) >>> 0;
  hist.push(`${pc.toString(16)} ${ins.toString(16).padStart(8,'0')} T${cpu.t} M${cpu.mode.toString(16)} r0=${(cpu.r[0]>>>0).toString(16)} r1=${(cpu.r[1]>>>0).toString(16)} r12=${(cpu.r[12]>>>0).toString(16)} sp=${(cpu.r[13]>>>0).toString(16)} lr=${(cpu.r[14]>>>0).toString(16)} cpsr=${(cpu.cpsr>>>0).toString(16)}`);
  if (hist.length > 30) hist.shift();
  gba.bus.cycles = 0; cpu.step();
}
console.log(hist.join('\n'));
