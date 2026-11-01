// Roda o ZEXDOC/ZEXALL (programas de CP/M) no Z80 do Índigo: node tools/zextest.mjs zexdoc.com
import fs from 'node:fs';
import { Z80 } from '../src/sms/z80.js';
const mem = new Uint8Array(0x10000);
mem.set(fs.readFileSync(process.argv[2]), 0x100);
mem[0] = 0x76; // fim: HALT
mem[5] = 0xC9; mem[6] = 0x00; mem[7] = 0xF0; // BDOS em 5; topo da memória em F000
let out = '';
const cpu = new Z80({ read: a => mem[a], write: (a, v) => { mem[a] = v; }, in: () => 0xFF, out: () => {} });
cpu.pc = 0x100; cpu.sp = 0xF000;
const t0 = Date.now(); let cycles = 0;
for (;;) {
  if (cpu.pc === 5) {
    if (cpu.c === 2) out += String.fromCharCode(cpu.e);
    else if (cpu.c === 9) { let a = cpu.de; while (mem[a] !== 0x24) out += String.fromCharCode(mem[a++]); }
    if (out.includes('\n')) { process.stdout.write(out); out = ''; }
  }
  if (cpu.pc === 0 || cpu.halted) break;
  cycles += cpu.step();
}
process.stdout.write(out);
console.log(`\n${(cycles / 1e6).toFixed(0)} M ciclos em ${((Date.now() - t0) / 1000).toFixed(1)} s (${(cycles / 3.58e6 / ((Date.now() - t0) / 1000)).toFixed(1)}× o Master System)`);
