// Save state genérico para o núcleo do GBA: percorre os componentes e copia números, textos, booleanos,
// memórias (typed arrays) e objetos de estado. Pula referências entre componentes, a ROM, tabelas derivadas
// e funções. Visões diferentes da mesma memória (8/16/32 bits) são copiadas uma vez só.

const SKIP = new Set(['gba', 'bus', 'cpu', 'io', 'ppu', 'apu', 'hle', 'rom', 'rom16', 'rom32', 'bios', 'bios16', 'bios32',
  'lut', 'armLut', 'thumbLut', 'bufL', 'bufR', 'swiHandler', 'fb']);
const isTyped = (v) => ArrayBuffer.isView(v) && !(v instanceof DataView);

function snap(obj, depth = 0) {
  const out = {};
  const seen = new Set();
  for (const k of Object.keys(obj)) {
    if (SKIP.has(k)) continue;
    const v = obj[k];
    if (v === null || typeof v === 'number' || typeof v === 'boolean' || typeof v === 'string') out[k] = v;
    else if (isTyped(v)) { if (seen.has(v.buffer)) continue; seen.add(v.buffer); out[k] = v.slice(); }
    else if (Array.isArray(v)) out[k] = v.map(e => (isTyped(e) ? e.slice() : (e && typeof e === 'object') ? snap(e, depth + 1) : e));
    else if (typeof v === 'object' && depth < 4) out[k] = snap(v, depth + 1);
  }
  return out;
}

function apply(obj, data) {
  for (const k of Object.keys(data)) {
    const v = data[k], cur = obj[k];
    if (isTyped(v)) {
      if (isTyped(cur) && cur.length === v.length) cur.set(v);
      else if (isTyped(cur) && cur.buffer.byteLength === v.byteLength) new Uint8Array(cur.buffer).set(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
      else obj[k] = v.slice();
    } else if (Array.isArray(v)) {
      if (!Array.isArray(cur) || cur.length !== v.length) { obj[k] = v.map(e => (isTyped(e) ? e.slice() : e)); continue; }
      for (let i = 0; i < v.length; i++) {
        const e = v[i];
        if (isTyped(e) && isTyped(cur[i]) && cur[i].length === e.length) cur[i].set(e);
        else if (e && typeof e === 'object' && !isTyped(e) && cur[i] && typeof cur[i] === 'object') apply(cur[i], e);
        else cur[i] = isTyped(e) ? e.slice() : e;
      }
    } else if (v && typeof v === 'object') {
      if (cur && typeof cur === 'object') apply(cur, v);
    } else obj[k] = v;
  }
}

const PARTS = ['cpu', 'bus', 'io', 'ppu', 'apu', 'hle'];

export function gbaSaveState(gba) {
  const s = { v: 1, sys: 'gba', top: snap(gba) };
  for (const p of PARTS) if (gba[p]) s[p] = snap(gba[p]);
  return s;
}

export function gbaLoadState(gba, s) {
  if (!s || s.sys !== 'gba' || s.v !== 1) throw new Error('Estado salvo incompatível com este jogo.');
  for (const p of PARTS) if (s[p] && gba[p]) apply(gba[p], s[p]);
  apply(gba, s.top);
}
