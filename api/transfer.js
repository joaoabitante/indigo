// Transferência temporária de cartucho + save entre aparelhos (ex.: PC → Xbox), por código de 6 dígitos.
// Também leva a estante inteira (kind: 'shelf'): o arquivo de backup .indigo, em partes, pelo mesmo caminho.
// Os arquivos ficam no Vercel Blob só durante a transferência: são apagados quando o outro aparelho
// confirma o recebimento, ou em até 1 hora. O acesso a esta rota exige o PIN da nuvem (api/_auth.js).
//
//   POST   ?action=start                 → { code }
//   POST   ?action=chunk&code=&part=     corpo binário (até 3,5 MB)
//   POST   ?action=finish&code=          corpo JSON (manifesto: nome, partes, save…)
//   GET    ?action=ping                  → { ok, storage }
//   GET    ?action=manifest&code=        → manifesto
//   GET    ?action=chunk&code=&part=     → bytes da parte
//   DELETE ?code=                        apaga a transferência

import { storageReady, listAll, putBlob, readBlob, delBlobs } from './_store.js';
import { requirePin } from './_auth.js';

const MAX_CHUNK = 3.5 * 1024 * 1024;
const MAX_PARTS = 400;           // 400 × 3 MB = 1,2 GB (estante inteira)
const TTL_MS = 60 * 60 * 1000;   // 1 hora

const json = (data, status = 200) => new Response(JSON.stringify(data), {
  status, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' },
});
const fail = (message, status = 400) => json({ error: message }, status);
const validCode = (c) => typeof c === 'string' && /^\d{6}$/.test(c);
const prefixOf = (code) => `transfer/${code}/`;

const blobsOf = (code) => listAll(prefixOf(code));
// nome da parte: part-007-<sufixo do Blob>.bin (o hífen evita que a parte 10 case com a 100)
const partName = (n) => `part-${String(n).padStart(3, '0')}`;

// remove transferências esquecidas (todas as páginas, inclusive partes sem manifesto) e devolve quantos bytes
// das transferências ainda válidas estão guardados
const QUOTA = 2.5 * 1024 * 1024 * 1024;   // no máximo 2,5 GB guardados ao mesmo tempo (duas estantes inteiras)
async function cleanup() {
  let live = 0;
  try {
    const old = [];
    for (const b of await listAll('transfer/')) {
      if (Date.now() - new Date(b.uploadedAt).getTime() > TTL_MS) old.push(b); else live += b.size;
    }
    if (old.length) await delBlobs(old);
  } catch { /* limpeza é oportunista */ }
  return live;
}


export async function GET(request) {
  const url = new URL(request.url);
  const action = url.searchParams.get('action');
  if (action === 'ping') return json({ ok: true, storage: storageReady(), pin: true });
  const denied = await requirePin(request); if (denied) return denied;
  if (action === 'auth') return json({ ok: true });
  if (!storageReady()) return fail('Armazenamento de transferências não configurado no Vercel.', 503);
  const code = url.searchParams.get('code');
  if (!validCode(code)) return fail('Código inválido. Use os 6 dígitos mostrados no outro aparelho.');
  const blobs = await blobsOf(code);
  if (action === 'manifest') {
    const m = blobs.find(b => b.pathname.startsWith(`${prefixOf(code)}manifest`)); // o Vercel acrescenta um sufixo aleatório ao nome
    if (!m) return fail('Nada com esse código. Confira os números ou envie de novo.', 404);
    if (Date.now() - new Date(m.uploadedAt).getTime() > TTL_MS) return fail('Esse código expirou. Envie de novo.', 410);
    return json(JSON.parse(new TextDecoder().decode(await readBlob(m))));
  }
  if (action === 'chunk') {
    const part = Number(url.searchParams.get('part'));
    const b = blobs.find(x => x.pathname.startsWith(`${prefixOf(code)}${partName(part)}-`) || x.pathname === `${prefixOf(code)}${partName(part)}.bin`);
    if (!b) return fail('Parte da transferência não encontrada.', 404);
    return new Response(await readBlob(b), {
      headers: { 'content-type': 'application/octet-stream', 'cache-control': 'no-store' },
    });
  }
  return fail('Ação desconhecida.');
}

export async function POST(request) {
  const denied = await requirePin(request); if (denied) return denied;
  if (!storageReady()) return fail('Armazenamento de transferências não configurado no Vercel.', 503);
  const url = new URL(request.url);
  const action = url.searchParams.get('action');
  if (action === 'start') {
    if (await cleanup() > QUOTA) return fail('O espaço de transferências está cheio agora. Tente de novo em 1 hora.', 507);
    for (let tries = 0; tries < 8; tries++) {
      const code = String(Math.floor(Math.random() * 1e6)).padStart(6, '0');
      if ((await blobsOf(code)).length === 0) return json({ code });
    }
    return fail('Não foi possível gerar um código agora. Tente de novo.', 503);
  }
  const code = url.searchParams.get('code');
  if (!validCode(code)) return fail('Código inválido.');
  if (action === 'chunk') {
    const part = Number(url.searchParams.get('part'));
    if (!Number.isInteger(part) || part < 0 || part >= MAX_PARTS) return fail('Parte inválida.');
    const body = await request.arrayBuffer();
    if (body.byteLength === 0 || body.byteLength > MAX_CHUNK) return fail('Parte com tamanho inválido.');
    // cada código guarda no máximo as suas partes e o manifesto
    if ((await blobsOf(code)).length > MAX_PARTS) return fail('Transferência grande demais.');
    await putBlob(`${prefixOf(code)}${partName(part)}.bin`, body, { contentType: 'application/octet-stream' });
    return json({ ok: true });
  }
  if (action === 'finish') {
    let m;
    try { m = await request.json(); } catch { return fail('Manifesto inválido.'); }
    if (!m || typeof m.name !== 'string' || !Number.isInteger(m.parts) || m.parts < 1 || m.parts > MAX_PARTS) return fail('Manifesto inválido.');
    if (m.save && (typeof m.save !== 'string' || m.save.length > 400000)) return fail('Save grande demais.');
    const manifest = {
      name: m.name.slice(0, 120), fileName: String(m.fileName || '').slice(0, 200),
      title: String(m.title || '').slice(0, 20), gameCode: String(m.gameCode || '').slice(0, 8),
      size: Number(m.size) || 0, parts: m.parts, save: m.save || null, saveAt: Number(m.saveAt) || 0,
      from: String(m.from || '').slice(0, 60), sentAt: Date.now(),
      kind: m.kind === 'shelf' ? 'shelf' : 'cart',
      counts: m.kind === 'shelf' && m.counts && typeof m.counts === 'object'
        ? { roms: Number(m.counts.roms) || 0, saves: Number(m.counts.saves) || 0, states: Number(m.counts.states) || 0 } : undefined,
    };
    await putBlob(`${prefixOf(code)}manifest.json`, JSON.stringify(manifest), { contentType: 'application/json' });
    return json({ ok: true, code });
  }
  return fail('Ação desconhecida.');
}

export async function DELETE(request) {
  const denied = await requirePin(request); if (denied) return denied;
  if (!storageReady()) return json({ ok: true });
  const code = new URL(request.url).searchParams.get('code');
  if (!validCode(code)) return fail('Código inválido.');
  const blobs = await blobsOf(code);
  if (blobs.length) await delBlobs(blobs);
  return json({ ok: true, removed: blobs.length });
}
