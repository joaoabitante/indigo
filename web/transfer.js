// Cliente do "Enviar para o Xbox": manda cartucho + save para outro aparelho por um código de 6 dígitos

import { pinFetch } from './pin.js';

const API = '/api/transfer';
const CHUNK = 3 * 1024 * 1024;

async function call(method, params, body, headers) {
  const qs = new URLSearchParams(params).toString();
  const r = await pinFetch(`${API}?${qs}`, { method, body, headers, credentials: 'same-origin', cache: 'no-store' }, true);
  const type = r.headers.get('content-type') || '';
  if (!r.ok) {
    let msg = `Erro ${r.status} no servidor.`;
    if (type.includes('json')) { try { msg = (await r.json()).error || msg; } catch { /* mantém */ } }
    throw new Error(msg);
  }
  return type.includes('json') ? r.json() : r.arrayBuffer();
}

// só existe quando a página roda no Vercel com o armazenamento configurado
export async function transferStatus() {
  try {
    const r = await fetch(`${API}?action=ping`, { cache: 'no-store', credentials: 'same-origin' });
    if (!r.ok || !(r.headers.get('content-type') || '').includes('json')) return 'off';
    const j = await r.json();
    return j.storage ? 'ready' : 'nostorage';
  } catch { return 'off'; }
}

export async function sendCartridge({ bytes, name, fileName, title, gameCode, save, saveAt }, onProgress) {
  const { code } = await call('POST', { action: 'start' });
  const parts = Math.ceil(bytes.length / CHUNK);
  for (let i = 0; i < parts; i++) {
    const slice = bytes.subarray(i * CHUNK, Math.min(bytes.length, (i + 1) * CHUNK));
    await call('POST', { action: 'chunk', code, part: String(i) }, slice, { 'content-type': 'application/octet-stream' });
    onProgress((i + 1) / (parts + 1));
  }
  const from = /Xbox/i.test(navigator.userAgent) ? 'Xbox' : /Android|iPhone|iPad/i.test(navigator.userAgent) ? 'celular' : 'computador';
  await call('POST', { action: 'finish', code }, JSON.stringify({ name, fileName, title, gameCode, size: bytes.length, parts, save, saveAt, from }),
    { 'content-type': 'application/json' });
  onProgress(1);
  return code;
}

// estante inteira: o arquivo de backup (.indigo) sobe em partes; o outro aparelho restaura juntando com o que tem
export async function sendShelf(blob, counts, onProgress) {
  const { code } = await call('POST', { action: 'start' });
  const parts = Math.max(1, Math.ceil(blob.size / CHUNK));
  if (parts > 400) throw new Error('A estante passa de 1,2 GB. Use o Backup (arquivo) para levar tudo.');
  for (let i = 0; i < parts; i++) {
    const slice = new Uint8Array(await blob.slice(i * CHUNK, Math.min(blob.size, (i + 1) * CHUNK)).arrayBuffer());
    let tries = 0;
    for (;;) {
      try { await call('POST', { action: 'chunk', code, part: String(i) }, slice, { 'content-type': 'application/octet-stream' }); break; }
      catch (e) { if (++tries >= 3) throw e; await new Promise(r => setTimeout(r, 1500 * tries)); }
    }
    onProgress((i + 1) / (parts + 1));
  }
  const from = /Xbox/i.test(navigator.userAgent) ? 'Xbox' : /Android|iPhone|iPad/i.test(navigator.userAgent) ? 'celular' : 'computador';
  await call('POST', { action: 'finish', code }, JSON.stringify({ kind: 'shelf', name: 'Estante inteira', size: blob.size, parts, counts, from }),
    { 'content-type': 'application/json' });
  onProgress(1);
  return code;
}

export async function receiveCartridge(code, onProgress) {
  const m = await call('GET', { action: 'manifest', code });
  if (m.kind === 'shelf') {
    // guarda as partes como Blobs (o navegador pode mantê-las fora da memória da página)
    const pieces = [];
    let got = 0;
    for (let i = 0; i < m.parts; i++) {
      let buf, tries = 0;
      for (;;) {
        try { buf = await call('GET', { action: 'chunk', code, part: String(i) }); break; }
        catch (e) { if (++tries >= 3) throw e; await new Promise(r => setTimeout(r, 1500 * tries)); }
      }
      pieces.push(new Blob([buf])); got += buf.byteLength;
      onProgress((i + 1) / m.parts);
    }
    if (got < m.size) throw new Error('A transferência veio incompleta. Envie de novo.');
    return { manifest: m, blob: new Blob(pieces).slice(0, m.size) };
  }
  const out = new Uint8Array(m.size);
  let off = 0;
  for (let i = 0; i < m.parts; i++) {
    const buf = new Uint8Array(await call('GET', { action: 'chunk', code, part: String(i) }));
    out.set(buf.subarray(0, Math.min(buf.length, out.length - off)), off);
    off += buf.length;
    onProgress((i + 1) / m.parts);
  }
  if (off < m.size) throw new Error('A transferência veio incompleta. Envie de novo.');
  // confirma o recebimento: apaga do servidor
  call('DELETE', { code }).catch(() => {});
  return { manifest: m, bytes: out };
}

export function cancelTransfer(code) { return call('DELETE', { code }).catch(() => {}); }
