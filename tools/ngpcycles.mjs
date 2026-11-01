// Tabela de ciclos observados na referência: (grupo, tamanho, opcode, modo) → ciclos (com contagem)
import fs from 'node:fs';
import readline from 'node:readline';
import { TLCS900H } from '../src/ngp/tlcs900h.js';
let mem = new Map();
const rb = (a) => mem.get(a & 0xFFFFFF) ?? 0;
const bus = { read8: rb, read16: (a) => rb(a) | (rb(a + 1) << 8), read32: (a) => (rb(a) | (rb(a + 1) << 8) | (rb(a + 2) << 16) | (rb(a + 3) << 24)) >>> 0, write8() {}, write16() {}, write32() {} };
const cpu = new TLCS900H(bus);
const table = new Map();
for (const file of process.argv.slice(2)) {
  const rl = readline.createInterface({ input: fs.createReadStream(file) });
  for await (const line of rl) {
    const [pre, ops, post] = line.split(' | ');
    if (!post) continue;
    const t = pre.trim().split(' '), pt = post.trim().split(' ');
    mem = new Map();
    for (const o of ops.trim().split(' ').filter(Boolean)) {
      const m = /^R(\d):([0-9A-F]+)=([0-9A-F]+)$/.exec(o); if (!m) continue;
      const sz = +m[1], a = parseInt(m[2], 16), v = parseInt(m[3], 16);
      for (let k = 0; k < sz; k++) if (!mem.has((a + k) & 0xFFFFFF)) mem.set((a + k) & 0xFFFFFF, (v >>> (8 * k)) & 0xFF);
    }
    cpu.pc = parseInt(t[0], 16); cpu.sr = parseInt(t[1], 16); cpu.r.set(t.slice(3, 23).map(x => parseInt(x, 16) >>> 0));
    const op0 = mem.get(cpu.pc);
    if (op0 === 0x1F) continue;
    cpu.step();
    const taken = parseInt(pt[0], 16) !== ((parseInt(t[0], 16) + 0) & 0) ? '' : '';
    const key = cpu.grp === 'main' ? `main ${op0.toString(16).padStart(2, '0')}` : `${cpu.grp} s${cpu.sz} ${cpu.op2.toString(16).padStart(2, '0')} ${cpu.mode}`;
    const refc = +pt[23];
    if (!table.has(key)) table.set(key, new Map());
    const m = table.get(key); m.set(refc, (m.get(refc) || 0) + 1);
    void taken;
  }
}
const out = [...table].sort((a, b) => a[0] < b[0] ? -1 : 1).map(([k, m]) => `${k}: ${[...m].map(([c, n]) => `${c}×${n}`).join(' ')}`);
fs.writeFileSync('/tmp/gt/cycles.txt', out.join('\n'));
console.log(out.length, 'chaves');
