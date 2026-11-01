// Determinismo do save state: roda, salva, roda M quadros; carrega num núcleo NOVO e roda M quadros; compara
import fs from 'node:fs';
import { GBA } from '../src/gba.js';
import { GB } from '../src/gb/gb.js';
import { gbaSaveState, gbaLoadState } from '../src/snapshot.js';
import { NGP } from '../src/ngp/ngp.js';
const [rom, keysEvery = '0'] = process.argv.slice(2);
const bytes = new Uint8Array(fs.readFileSync(rom));
const isGba = /\.gba$/i.test(rom), isNgp = /\.(ngp|ngc)$/i.test(rom);
const mk = () => { const e = isGba ? new GBA() : isNgp ? new NGP() : new GB(); e.loadRom(bytes.slice()); return e; };
const save = (e) => isGba ? gbaSaveState(e) : e.saveState();
const load = (e, s) => isGba ? gbaLoadState(e, s) : e.loadState(s);
const hash = (fb) => { let h = 0x811C9DC5; for (let i = 0; i < fb.length; i++) { h ^= fb[i]; h = Math.imul(h, 0x01000193); } return (h >>> 0).toString(16); };
const keys = (f) => (Number(keysEvery) && (f % Number(keysEvery)) < 3) ? (1 << (f % 8)) : 0;
const a = mk();
for (let f = 0; f < 300; f++) { a.setKeys(keys(f)); a.runFrame(); }
const st = structuredClone(save(a));
const ha = [];
for (let f = 300; f < 600; f++) { a.setKeys(keys(f)); a.runFrame(); if (f % 50 === 49) ha.push(hash(a.ppu.fb)); }
const b = mk();
b.runFrame();
load(b, st);
const hb = [];
for (let f = 300; f < 600; f++) { b.setKeys(keys(f)); b.runFrame(); if (f % 50 === 49) hb.push(hash(b.ppu.fb)); }
const bytesLen = JSON.stringify(st, (k, v) => ArrayBuffer.isView(v) ? v.length : v).length;
console.log(rom.split('/').pop(), ha.join(',') === hb.join(',') ? 'IGUAL' : 'DIFERENTE', ha.join(','), '|', hb.join(','));
