// Backup da estante num arquivo só (.indigo): ROMs (opcional), saves com histórico e estados salvos.
// Formato: "INDIGOB1" + tamanho do cabeçalho (u32) + cabeçalho JSON + blocos binários. Os bytes (ROMs, saves,
// partes dos estados) vão crus depois do JSON; o JSON guarda só a posição de cada um.
// Restaurar nunca apaga nada: junta com o que já existe e, quando os dois lados têm save, fica o mais novo
// e o outro vai para o histórico.

import { library } from './library.js';

const MAGIC = 'INDIGOB1';
const enc = new TextEncoder(), dec = new TextDecoder();

// troca arrays binários por referências a blocos
function pack(v, parts, pos) {
  if (typeof v === 'number' && !Number.isFinite(v)) return { $num: String(v) }; // Infinity/NaN não cabem em JSON
  if (v === null || typeof v !== 'object') return v;
  if (v instanceof ArrayBuffer) { const ref = { $ab: 1, o: pos.n, n: v.byteLength }; parts.push(new Uint8Array(v)); pos.n += v.byteLength; return ref; }
  if (ArrayBuffer.isView(v)) {
    const u = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
    const ref = { $ta: v.constructor.name, o: pos.n, n: u.byteLength };
    parts.push(u.slice()); pos.n += u.byteLength; return ref;
  }
  if (Array.isArray(v)) return v.map(x => pack(x, parts, pos));
  const o = {};
  for (const [k, x] of Object.entries(v)) o[k] = pack(x, parts, pos);
  return o;
}
async function unpack(v, read) {
  if (v === null || typeof v !== 'object') return v;
  if (v.$num) return Number(v.$num);
  if (v.$ab) return (await read(v.o, v.n)).slice(0);
  if (v.$ta) {
    const buf = (await read(v.o, v.n)).slice(0);
    const C = globalThis[v.$ta];
    return C && C.BYTES_PER_ELEMENT ? new C(buf) : new Uint8Array(buf);
  }
  if (Array.isArray(v)) { const out = []; for (const x of v) out.push(await unpack(x, read)); return out; }
  const o = {};
  for (const [k, x] of Object.entries(v)) o[k] = await unpack(x, read);
  return o;
}

export async function makeBackup({ roms = true } = {}, onProgress = () => {}) {
  const parts = [], pos = { n: 0 };
  const metas = await library.list();
  const romsOut = [];
  for (let i = 0; i < metas.length; i++) {
    const m = metas[i];
    const rec = roms ? await library.get(m.id) : null;
    romsOut.push(pack(Object.assign({}, m, { data: rec && rec.data ? rec.data : null }), parts, pos));
    onProgress((i + 1) / (metas.length + 2));
  }
  const saves = [];
  for (const s of await library.listSaves()) {
    const r = await library.getSave(s.key);
    if (r) saves.push(pack(r, parts, pos));
  }
  const hist = (await library.allHistory()).map(h => pack(h, parts, pos));
  const states = (await library.allStates()).map(st => pack(st, parts, pos));
  onProgress((metas.length + 1) / (metas.length + 2));
  const head = { format: 1, app: 'Índigo', createdAt: Date.now(), withRoms: roms, roms: romsOut, saves, hist, states };
  const json = enc.encode(JSON.stringify(head));
  const len = new Uint8Array(4); new DataView(len.buffer).setUint32(0, json.length, true);
  onProgress(1);
  const blob = new Blob([enc.encode(MAGIC), len, json, ...parts], { type: 'application/octet-stream' });
  return { blob, counts: { roms: metas.length, withRoms: roms, saves: saves.length, states: states.length, hist: hist.length } };
}

export async function isBackup(file) {
  const h = new Uint8Array(await file.slice(0, 8).arrayBuffer());
  return dec.decode(h) === MAGIC;
}

export async function restoreBackup(file, onProgress = () => {}) {
  const top = new Uint8Array(await file.slice(0, 12).arrayBuffer());
  if (dec.decode(top.subarray(0, 8)) !== MAGIC) throw new Error('Esse arquivo não é um backup do Índigo.');
  const jlen = new DataView(top.buffer).getUint32(8, true);
  const head = JSON.parse(dec.decode(new Uint8Array(await file.slice(12, 12 + jlen).arrayBuffer())));
  const base = 12 + jlen;
  const read = async (o, n) => file.slice(base + o, base + o + n).arrayBuffer();
  const out = { romsAdded: 0, romsKept: 0, romsNoData: 0, savesNew: 0, savesNewer: 0, savesKeptLocal: 0, states: 0 };
  const local = new Map((await library.list()).map(m => [m.id, m]));
  const total = head.roms.length + head.saves.length + head.states.length + 1;
  let done = 0;
  const tick = () => onProgress(++done / total);

  for (const r0 of head.roms) {
    const r = await unpack(r0, read);
    const have = local.get(r.id);
    if (have) {
      const patch = {};
      if (!have.cover && r.cover) patch.cover = r.cover;
      if ((r.playTime || 0) > (have.playTime || 0)) patch.playTime = r.playTime;
      if ((r.lastPlayed || 0) > (have.lastPlayed || 0)) patch.lastPlayed = r.lastPlayed;
      if (r.fav && !have.fav) patch.fav = true;
      if (r.tags && r.tags.length) { const u = [...new Set([...(have.tags || []), ...r.tags])]; if (u.length !== (have.tags || []).length) patch.tags = u; }
      if (r.autoAt && (r.autoAt > (have.autoAt || 0))) patch.autoAt = r.autoAt;
      if (Object.keys(patch).length) await library.update(r.id, patch);
      out.romsKept++;
    } else if (r.data) {
      const { data, ...meta } = r;
      await library.add(meta, new Uint8Array(data));
      out.romsAdded++;
    } else out.romsNoData++;
    tick();
  }
  for (const s0 of head.saves) {
    const s = await unpack(s0, read);
    const cur = await library.getSave(s.key);
    const bytes = new Uint8Array(s.data);
    if (!cur) { await library.putSave(s.key, bytes, { at: s.at, source: 'backup', reason: 'restaurado do backup', force: true }); out.savesNew++; }
    else if (s.at > cur.at) {
      await library.snapshotSave(s.key, 'antes de restaurar backup');
      await library.putSave(s.key, bytes, { at: s.at, source: 'backup', reason: 'restaurado do backup', force: true });
      out.savesNewer++;
    } else {
      // o deste aparelho é mais novo: o do backup fica guardado no histórico
      await library.putHistory({ hid: `${s.key}@${s.at}#backup`, key: s.key, at: s.at, size: bytes.byteLength, data: bytes, reason: 'do backup (mais antigo)' });
      out.savesKeptLocal++;
    }
    tick();
  }
  for (const h0 of head.hist || []) await library.putHistory(await unpack(h0, read));
  const states = new Map((await library.allStates()).map(s => [s.key, s]));
  for (const st0 of head.states) {
    const st = await unpack(st0, read);
    const cur = states.get(st.key);
    if (!cur || st.at > cur.at) { await library.putStateRecord(st); out.states++; }
    tick();
  }
  onProgress(1);
  return out;
}
