// Teste diferencial da CPU TLCS-900H (lado JavaScript): executa instruções aleatórias com registradores e memória
// aleatórios e imprime um hash por caso (registradores, flags, DMA, gravações na memória e ciclos).
// native/tools/tlcsfuzz.cpp faz o mesmo com a CPU em C++; as duas saídas devem ser idênticas.
// uso: node native/tools/tlcsfuzz.mjs casos [inicio] [detalhe]
import { TLCS900H } from '../../src/ngp/tlcs900h.js';

const [countArg, startArg = '1', detailArg] = process.argv.slice(2);
const count = +countArg, start = +startArg, detail = detailArg !== undefined;
let seed = 0, mem = new Map(), writes = [], reads = 0;
const def = (a) => (Math.imul((a ^ seed) >>> 0, 0x9E3779B1) >>> 24);
const rb = (a) => { if (++reads > 60000) throw new Error('HANG'); a &= 0xFFFFFF; return mem.has(a) ? mem.get(a) : def(a); };
const wb = (a, v) => { a &= 0xFFFFFF; mem.set(a, v & 0xFF); writes.push([a, v & 0xFF]); };
const bus = {
  read8: (a) => rb(a),
  read16: (a) => rb(a) | (rb(a + 1) << 8),
  read32: (a) => (rb(a) | (rb(a + 1) << 8) | (rb(a + 2) << 16) | (rb(a + 3) << 24)) >>> 0,
  write8: (a, v) => wb(a, v),
  write16: (a, v) => { wb(a, v); wb(a + 1, v >> 8); },
  write32: (a, v) => { wb(a, v); wb(a + 1, v >> 8); wb(a + 2, v >> 16); wb(a + 3, v >>> 24); },
};
const cpu = new TLCS900H(bus);
cpu.onBios = () => {};
let x = 0;
const next = () => { x ^= x << 13; x ^= x >>> 17; x ^= x << 5; x >>>= 0; return x; };
const hex = (v, n = 8) => (v >>> 0).toString(16).padStart(n, '0');
const out = [];
for (let i = start; i < start + count; i++) {
  seed = Math.imul(i, 0x85EBCA6B) >>> 0; x = seed || 1;
  mem = new Map(); writes = [];
  cpu.reset();
  for (let k = 0; k < 20; k++) cpu.r[k] = next();
  cpu.r[20] = 0;
  cpu.pc = next() & 0xFFFFFF; cpu.sr = next() & 0xFFFF; cpu.fd = next() & 0xFF;
  for (let k = 0; k < 4; k++) { cpu.dmaS[k] = next(); cpu.dmaD[k] = next(); cpu.dmaC[k] = next(); cpu.dmaM[k] = next(); }
  cpu.intNest = next() & 0xFFFF;
  reads = 0;
  let cyc;
  try { cyc = cpu.step(); } catch (e) { if (e.message !== 'HANG') throw e; if (detail) console.log('HANG'); else out.push('HANG'); continue; }
  const st = [cpu.pc & 0xFFFFFF, cpu.sr & 0xFFFF, cpu.fd & 0xFF, ...cpu.r, ...cpu.dmaS, ...cpu.dmaD, ...cpu.dmaC, ...cpu.dmaM, cpu.intNest, +cpu.halted, cyc];
  for (const [a, v] of writes) st.push(a, v);
  if (detail) { console.log(st.map((v) => hex(v)).join(' ')); continue; }
  let h = 0x811C9DC5;
  for (const v of st) { h ^= v >>> 0; h = Math.imul(h, 0x01000193); }
  out.push(hex(h));
}
if (!detail) process.stdout.write(out.join('\n') + '\n');
