// Cópia de segurança dos saves na nuvem (Vercel Blob), compartilhada entre os aparelhos que abrem este site.
// Só saves de cartucho: as ROMs nunca sobem. Guarda as 6 versões mais recentes de cada save.
// O acesso a esta rota exige o PIN da nuvem (api/_auth.js).
//
//   GET  ?action=list                         → { saves: [{ key, at, size, from, name, console }] } (só a mais nova de cada)
//   GET  ?action=versions&key=                → { versions: [{ at, size, from }] }
//   GET  ?action=get&key=&at=                 → bytes do save
//   POST ?action=put&key=&at=&from=&name=&console=   corpo binário (até 1 MB)

import { put, list, del } from '@vercel/blob';
import { requirePin } from './_auth.js';

const MAX_SAVE = 1024 * 1024;
const KEEP = 6;
const ROOT = 'cloud/saves/';

const json = (data, status = 200) => new Response(JSON.stringify(data), {
  status, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' },
});
const fail = (message, status = 400) => json({ error: message }, status);
const storageReady = () => !!(process.env.BLOB_STORE_ID || process.env.BLOB_READ_WRITE_TOKEN);

// chave do save → pasta (base64url, sem caracteres especiais no caminho)
const enc = (k) => Buffer.from(k, 'utf8').toString('base64url');
const decKey = (s) => Buffer.from(s, 'base64url').toString('utf8');
const validKey = (k) => typeof k === 'string' && k.length > 4 && k.length < 300 && k.startsWith('rom:');
const clean = (s, n) => String(s || '').replace(/[^\p{L}\p{N} ._()\-+!&',]/gu, '').slice(0, n);

// nome do arquivo: <at>~<from>~<console>~<nome em base64url>~.bin (o Blob acrescenta um sufixo aleatório no fim,
// por isso o último pedaço depois do ~ é descartado)
function parse(b) {
  const rest = b.pathname.slice(ROOT.length);
  const slash = rest.indexOf('/');
  if (slash < 0) return null;
  const [at, from, cons, name] = rest.slice(slash + 1).split('~');
  let key;
  try { key = decKey(rest.slice(0, slash)); } catch { return null; }
  return { key, at: Number(at) || 0, size: b.size, from: from || '', console: cons || '', name: name ? Buffer.from(name, 'base64url').toString('utf8') : '', url: b.url };
}

async function listAll(prefix) {
  const out = [];
  let cursor;
  do {
    const r = await list({ prefix, limit: 1000, cursor });
    out.push(...r.blobs);
    cursor = r.hasMore ? r.cursor : undefined;
  } while (cursor);
  return out;
}

export async function GET(request) {
  const denied = await requirePin(request); if (denied) return denied;
  if (!storageReady()) return fail('Armazenamento na nuvem não configurado no Vercel.', 503);
  const url = new URL(request.url);
  const action = url.searchParams.get('action');
  if (action === 'list') {
    const latest = new Map();
    for (const b of await listAll(ROOT)) {
      const p = parse(b);
      if (!p) continue;
      const cur = latest.get(p.key);
      if (!cur || p.at > cur.at) latest.set(p.key, p);
    }
    return json({ saves: [...latest.values()].map(({ url: _u, ...m }) => m) });
  }
  const key = url.searchParams.get('key');
  if (!validKey(key)) return fail('Chave de save inválida.');
  const all = (await listAll(ROOT + enc(key) + '/')).map(parse).filter(Boolean).sort((a, b) => b.at - a.at);
  if (action === 'versions') return json({ versions: all.map(({ url: _u, key: _k, ...m }) => m) });
  if (action === 'get') {
    const at = Number(url.searchParams.get('at'));
    const v = at ? all.find(x => x.at === at) : all[0];
    if (!v) return fail('Esse save não está na nuvem.', 404);
    const r = await fetch(v.url, { cache: 'no-store' });
    return new Response(await r.arrayBuffer(), { headers: { 'content-type': 'application/octet-stream', 'cache-control': 'no-store', 'x-save-at': String(v.at) } });
  }
  return fail('Ação desconhecida.');
}

export async function POST(request) {
  const denied = await requirePin(request); if (denied) return denied;
  if (!storageReady()) return fail('Armazenamento na nuvem não configurado no Vercel.', 503);
  const url = new URL(request.url);
  if (url.searchParams.get('action') !== 'put') return fail('Ação desconhecida.');
  const key = url.searchParams.get('key');
  if (!validKey(key)) return fail('Chave de save inválida.');
  const at = Number(url.searchParams.get('at'));
  // data no futuro (mais de 5 min à frente do servidor) passaria na frente de todos os saves verdadeiros
  if (!Number.isFinite(at) || at <= 0 || at > Date.now() + 5 * 60 * 1000) return fail('Data do save inválida.');
  const body = await request.arrayBuffer();
  if (!body.byteLength || body.byteLength > MAX_SAVE) return fail('Save com tamanho inválido.');
  const folder = ROOT + enc(key) + '/';
  const existing = (await listAll(folder)).map(b => ({ b, p: parse(b) })).filter(x => x.p);
  if (existing.some(x => x.p.at === at)) return json({ ok: true, dup: true });
  const name = Buffer.from(clean(url.searchParams.get('name'), 80), 'utf8').toString('base64url');
  const file = `${folder}${at}~${clean(url.searchParams.get('from'), 20)}~${clean(url.searchParams.get('console'), 6)}~${name}~.bin`;
  await put(file, body, { access: 'public', addRandomSuffix: true, contentType: 'application/octet-stream' });
  // mantém só as versões mais novas
  const old = existing.sort((a, b) => b.p.at - a.p.at).slice(KEEP - 1).map(x => x.b.url);
  if (old.length) await del(old);
  return json({ ok: true });
}
