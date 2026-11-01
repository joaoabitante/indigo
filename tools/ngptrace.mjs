// Compara a CPU TLCS-900H do Índigo com o rastro do emulador de referência, instrução por instrução.
// Cada instrução recebe exatamente os bytes que a referência leu; comparamos registradores e gravações.
// node tools/ngptrace.mjs rastro.txt [maxErros]
import fs from 'node:fs';
import readline from 'node:readline';
import { TLCS900H } from '../src/ngp/tlcs900h.js';

const [file, maxErrArg = '20'] = process.argv.slice(2);
const maxErr = +maxErrArg;
let mem = new Map(), writes = new Map(), badRead = null;
const rb = (a) => { a &= 0xFFFFFF; if (!mem.has(a)) { badRead = badRead || a; return 0; } return mem.get(a); };
const bus = {
  read8: (a) => rb(a),
  read16: (a) => rb(a) | (rb(a + 1) << 8),
  read32: (a) => (rb(a) | (rb(a + 1) << 8) | (rb(a + 2) << 16) | (rb(a + 3) << 24)) >>> 0,
  write8: (a, v) => { writes.set(a & 0xFFFFFF, v & 0xFF); },
  write16: (a, v) => { bus.write8(a, v); bus.write8(a + 1, v >> 8); },
  write32: (a, v) => { bus.write16(a, v & 0xFFFF); bus.write16(a + 2, v >>> 16); },
};
const cpu = new TLCS900H(bus);
let onBios = false; cpu.onBios = () => { onBios = true; };
const hex = (v, n) => (v >>> 0).toString(16).toUpperCase().padStart(n, '0');
const parseState = (t) => ({ pc: parseInt(t[0], 16), sr: parseInt(t[1], 16), fd: parseInt(t[2], 16), r: t.slice(3, 23).map(x => parseInt(x, 16) >>> 0) });
const NAMES = ['XWA0', 'XBC0', 'XDE0', 'XHL0', 'XWA1', 'XBC1', 'XDE1', 'XHL1', 'XWA2', 'XBC2', 'XDE2', 'XHL2', 'XWA3', 'XBC3', 'XDE3', 'XHL3', 'XIX', 'XIY', 'XIZ', 'XSP'];
let n = 0, errs = 0, skipped = 0, cycErr = 0;
const byOp = new Map(), cycByOp = new Map();
const rl = readline.createInterface({ input: fs.createReadStream(file) });
for await (const line of rl) {
  const [pre, ops, post] = line.split(' | ');
  if (!post) continue;
  const A = parseState(pre.trim().split(' '));
  const pt = post.trim().split(' ');
  const B = parseState(pt);
  const refCycles = +pt[23];
  mem = new Map(); writes = new Map(); badRead = null;
  const refWrites = new Map();
  for (const o of ops.trim().split(' ').filter(Boolean)) {
    const m = /^([RW])(\d):([0-9A-F]+)=([0-9A-F]+)$/.exec(o);
    const kind = m[1], sz = +m[2], a = parseInt(m[3], 16), v = parseInt(m[4], 16);
    for (let k = 0; k < sz; k++) {
      const b = (v >>> (8 * k)) & 0xFF, ad = (a + k) & 0xFFFFFF;
      if (kind === 'R') { if (!mem.has(ad)) mem.set(ad, b); } else refWrites.set(ad, b);
    }
  }
  cpu.pc = A.pc; cpu.sr = A.sr; cpu.fd = A.fd; cpu.r.set(A.r); cpu.halted = false;
  const op0 = mem.get(A.pc);
  onBios = false;
  n++;
  if (op0 === 0x1F) { skipped++; continue; } // chamada da BIOS da referência (HLE)
  let cyc = 0;
  try { cyc = cpu.step(); } catch (e) { cyc = -1; }
  const diffs = [];
  if ((cpu.pc & 0xFFFFFF) !== B.pc) diffs.push(`PC ${hex(cpu.pc, 6)}≠${hex(B.pc, 6)}`);
  if ((cpu.sr & 0xFFFF) !== B.sr) diffs.push(`SR ${hex(cpu.sr, 4)}≠${hex(B.sr, 4)}`);
  if ((cpu.fd & 0xFF) !== B.fd) diffs.push(`F' ${hex(cpu.fd, 2)}≠${hex(B.fd, 2)}`);
  for (let k = 0; k < 20; k++) if (cpu.r[k] !== B.r[k]) diffs.push(`${NAMES[k]} ${hex(cpu.r[k], 8)}≠${hex(B.r[k], 8)}`);
  for (const [a, v] of refWrites) if (writes.get(a) !== v) diffs.push(`W ${hex(a, 6)} ${writes.has(a) ? hex(writes.get(a), 2) : '--'}≠${hex(v, 2)}`);
  for (const [a, v] of writes) if (!refWrites.has(a)) diffs.push(`W extra ${hex(a, 6)}=${hex(v, 2)}`);
  if (badRead !== null) diffs.push(`leitura inesperada ${hex(badRead, 6)}`);
  // bytes da instrução (para agrupar erros por opcode)
  const key = hex(op0, 2) + (cpu.op2 >= 0 ? ' ' + hex(cpu.op2, 2) : '');
  if (diffs.length) {
    errs++;
    byOp.set(key, (byOp.get(key) || 0) + 1);
    if (errs <= maxErr && (!process.env.OP || process.env.OP === key)) console.log(`#${n} ${hex(A.pc, 6)} [${ops.trim().split(' ').filter(o => o.startsWith('R1:' + hex(A.pc, 6)) || o.includes(':' + hex(A.pc, 6))).slice(0, 1)}] op ${key}: ${diffs.slice(0, 6).join(', ')}`);
  } else if (cyc !== refCycles) {
    cycErr++;
    const k2 = key + ` (${cyc} vs ${refCycles})`;
    cycByOp.set(k2, (cycByOp.get(k2) || 0) + 1);
  }
}
console.log(`\n${n} instruções, ${skipped} chamadas de BIOS puladas, ${errs} com diferença de estado, ${cycErr} só com ciclos diferentes`);
console.log('erros por opcode:', [...byOp].sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => `${k}×${v}`).join('  '));
console.log('ciclos por opcode:', [...cycByOp].sort((a, b) => b[1] - a[1]).slice(0, 25).map(([k, v]) => `${k}×${v}`).join('  '));
