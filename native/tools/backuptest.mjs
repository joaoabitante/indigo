// Lado web dos testes do backup (native/tools/backuptest.cpp), usando o código do próprio site:
// web/backup.js (formato .indigo), web/library.js (romId) e as funções de identificação de web/main.js.
//   node backuptest.mjs ids rom...                       mesma saída de "indigo-backuptest ids"
//   node backuptest.mjs make saida.indigo comRoms rom[=save] ...   faz um backup como o site faria
//   node backuptest.mjs restore backup.indigo            restaura numa estante vazia de mentira e imprime o
//                                                        resultado no formato de "indigo-backuptest dump"
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { smsKind, smsNormalize, smsCode } from '../../src/sms/header.js';
import { isNgpRom, ngpInfo } from '../../src/ngp/header.js';
import { library, romId } from '../../web/library.js';
import { makeBackup, restoreBackup } from '../../web/backup.js';

const here = path.dirname(fileURLToPath(import.meta.url));
// detectSystem, romHeader e cartInfoOf saem do próprio web/main.js (que depende do navegador e não pode ser
// importado inteiro)
const mainSrc = fs.readFileSync(path.join(here, '../../web/main.js'), 'utf8');
const piece = mainSrc.slice(mainSrc.indexOf('function detectSystem'), mainSrc.indexOf('const SAVE_PREFIX'));
const { detectSystem, romHeader } = new Function('smsKind', 'smsNormalize', 'smsCode', 'isNgpRom', 'ngpInfo',
  piece.replace(/^\/\/.*$/gm, '') + '\nreturn { detectSystem, romHeader };')(smsKind, smsNormalize, smsCode, isNgpRom, ngpInfo);

const fnv = (b) => { let h = 0x811C9DC5; for (const x of b) { h ^= x; h = Math.imul(h, 0x01000193); } return (h >>> 0).toString(16).padStart(8, '0'); };
const [cmd, ...args] = process.argv.slice(2);

function ident(bytes, name) {
  let sys = detectSystem(bytes, name);
  if (sys === 'nds' || !sys) return { sys: '', title: '', gameCode: '', id: romId(bytes, '', '') };
  const h = romHeader(bytes, name, sys);
  return { sys, title: h.title || '', gameCode: h.gameCode || '', id: romId(bytes, h.gameCode || '', h.title || '') };
}

if (cmd === 'ids') {
  const out = [];
  for (const f of args) {
    const b = new Uint8Array(fs.readFileSync(f));
    const name = path.basename(f), r = ident(b, name);
    out.push(`${name}|${r.sys}|${r.title}|${r.gameCode}|${r.id}`);
  }
  process.stdout.write(out.join('\n') + '\n');
} else if (cmd === 'make') {
  // estante de mentira com as ROMs (e saves) dados; datas fixas para a saída ser sempre a mesma
  const [outFile, withRoms, ...items] = args;
  const metas = [], datas = new Map(), saves = new Map(), hist = [];
  items.forEach((it, i) => {
    const [romPath, savePath] = it.split('=');
    const b = new Uint8Array(fs.readFileSync(romPath));
    const name = path.basename(romPath), r = ident(b, name);
    const key = 'rom:' + r.id;
    metas.push({ id: r.id, name: name.replace(/\.[^.]+$/, ''), fileName: name, title: r.title, gameCode: r.gameCode, console: r.sys,
      size: b.length, saveKey: key, added: 1792000000000 + i, lastPlayed: 1792500000000 + i, fav: i === 0, playTime: 1000 * i });
    datas.set(r.id, b);
    if (savePath) {
      const s = new Uint8Array(fs.readFileSync(savePath));
      saves.set(key, { key, at: 1792600000000 + i, size: s.length, data: s, source: 'jogo' });
      const old = s.slice(); old[0] ^= 0xFF;
      hist.push({ hid: `${key}@${1792400000000 + i}`, key, at: 1792400000000 + i, size: old.length, data: old.buffer, reason: 'automático' });
    }
  });
  library.list = async () => metas;
  library.get = async (id) => Object.assign({}, metas.find(m => m.id === id), { data: datas.get(id) });
  library.listSaves = async () => [...saves.values()].map(({ data, ...m }) => m);
  library.getSave = async (k) => saves.get(k) || null;
  library.allHistory = async () => hist;
  library.allStates = async () => [{ key: metas[0] ? metas[0].id + '#1' : 'x#1', id: 'x', slot: 1, at: 1, thumb: null, state: { v: 1, sys: 'gba', top: { frame: 1 } } }];
  const realNow = Date.now; Date.now = () => 1792700000000;
  const { blob } = await makeBackup({ roms: withRoms === '1' });
  Date.now = realNow;
  fs.writeFileSync(outFile, new Uint8Array(await blob.arrayBuffer()));
} else if (cmd === 'restore') {
  // estante vazia de mentira que anota o que o restaurador grava
  const added = [], saves = new Map(), hist = [];
  library.list = async () => [];
  library.add = async (meta, bytes) => { added.push({ meta, bytes }); return true; };
  library.update = async () => {};
  library.getSave = async (k) => saves.get(k) || null;
  library.putSave = async (key, bytes, opts) => { saves.set(key, { key, at: opts.at, data: bytes }); };
  library.snapshotSave = async () => true;
  library.putHistory = async (rec) => { hist.push(rec); };
  library.putStateRecord = async () => {};
  library.allStates = async () => [];
  const buf = fs.readFileSync(args[0]);
  const file = new Blob([buf]);
  const res = await restoreBackup(file);
  const out = [`withRoms ? states ${res.states}`];
  for (const { meta, bytes } of added) out.push(`rom ${meta.id}|${meta.name}|${meta.console}|${meta.saveKey}|1|${bytes.length}|${fnv(bytes)}`);
  for (const s of saves.values()) out.push(`save ${s.key}|${s.at}|${s.data.length}|${fnv(s.data)}`);
  for (const h of hist) out.push(`hist ${h.key}|${h.at}|${h.size}|${fnv(new Uint8Array(h.data))}|${h.reason}`);
  out.push('resultado ' + JSON.stringify(res));
  process.stdout.write(out.join('\n') + '\n');
}
