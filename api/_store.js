// Armazenamento dos saves da nuvem e das transferências (Vercel Blob).
// Com INDIGO_BLOB_PRIVADO (o id de um Blob store PRIVADO ligado a este projeto), tudo o que é gravado vai para ele
// e só sai pelas rotas que pedem o PIN: o endereço do arquivo, sozinho, não abre nada. O store público antigo
// (BLOB_STORE_ID) continua sendo lido e limpo: as versões antigas dos saves somem sozinhas no rodízio (6 por save)
// e as transferências em 1 hora, então não é preciso copiar nada. Sem a variável, tudo fica como antes.

import { put, list, del, get } from '@vercel/blob';

const privateId = () => process.env.INDIGO_BLOB_PRIVADO || '';

// o primeiro da lista é onde se grava
function stores() {
  const out = [];
  if (privateId()) out.push({ access: 'private', opts: { storeId: privateId() } });
  if (process.env.BLOB_STORE_ID || process.env.BLOB_READ_WRITE_TOKEN) out.push({ access: 'public', opts: {} });
  return out;
}

export const storageReady = () => stores().length > 0;

// todos os arquivos com o prefixo, de todos os stores; cada um leva o store de onde veio
export async function listAll(prefix) {
  const out = [];
  for (const store of stores()) {
    let cursor;
    do {
      const r = await list({ prefix, limit: 1000, cursor, ...store.opts });
      for (const b of r.blobs) out.push({ ...b, store });
      cursor = r.hasMore ? r.cursor : undefined;
    } while (cursor);
  }
  return out;
}

export function putBlob(pathname, body, options) {
  const store = stores()[0];
  return put(pathname, body, { ...options, access: store.access, addRandomSuffix: true, ...store.opts });
}

// conteúdo de um arquivo vindo de listAll
export async function readBlob(b) {
  if (b.store.access === 'private') {
    const r = await get(b.url, { access: 'private', useCache: false, ...b.store.opts });
    if (!r || r.statusCode !== 200 || !r.stream) throw new Error('Arquivo da nuvem não encontrado.');
    return new Response(r.stream).arrayBuffer();
  }
  const r = await fetch(b.url, { cache: 'no-store' });
  if (!r.ok) throw new Error('Arquivo da nuvem não encontrado.');
  return r.arrayBuffer();
}

// apaga arquivos vindos de listAll (cada um no seu store, em lotes de até 1000)
export async function delBlobs(blobs) {
  for (const store of stores()) {
    const urls = blobs.filter(b => b.store.access === store.access).map(b => b.url);
    for (let i = 0; i < urls.length; i += 1000) await del(urls.slice(i, i + 1000), store.opts);
  }
}
