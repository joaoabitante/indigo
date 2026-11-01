// Servidor local para testar a nuvem do app nativo com o código de verdade do site: roda api/cloud.js,
// api/transfer.js e api/_auth.js, trocando o Vercel Blob por um depósito na memória.
//   INDIGO_PIN=1234 node native/tools/cloudmock.mjs porta [chave=arquivo@ms ...]
// As chaves passadas já começam na "nuvem" (como se outro aparelho tivesse enviado). GET /__dump lista tudo.
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import http from 'node:http';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const [portArg = '8787', ...seeds] = process.argv.slice(2);
const port = Number(portArg);

// cópia das rotas num diretório com um @vercel/blob de mentira
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'indigo-cloud-'));
fs.mkdirSync(path.join(dir, 'api'));
for (const f of ['cloud.js', 'transfer.js', '_auth.js']) fs.copyFileSync(path.join(here, '../../api', f), path.join(dir, 'api', f));
const blobDir = path.join(dir, 'node_modules/@vercel/blob');
fs.mkdirSync(blobDir, { recursive: true });
fs.writeFileSync(path.join(dir, 'package.json'), '{"type":"module"}');
fs.writeFileSync(path.join(blobDir, 'package.json'), '{"name":"@vercel/blob","type":"module","main":"index.js"}');
fs.writeFileSync(path.join(blobDir, 'index.js'), `
const store = globalThis.__blobStore || (globalThis.__blobStore = new Map());
let n = 0;
export async function put(pathname, body, opts = {}) {
  const buf = Buffer.from(body instanceof ArrayBuffer ? new Uint8Array(body) : body);
  const name = opts.addRandomSuffix ? pathname.replace(/\\.bin$/, '-' + (++n).toString(36) + '.bin') : pathname;
  const url = 'http://127.0.0.1:${port}/__blob/' + encodeURIComponent(name);
  store.set(url, { pathname: name, url, size: buf.length, body: buf });
  return { url, pathname: name };
}
export async function list({ prefix = '' } = {}) {
  return { blobs: [...store.values()].filter(b => b.pathname.startsWith(prefix)).map(({ body, ...b }) => b), hasMore: false };
}
export async function del(urls) { for (const u of [].concat(urls)) store.delete(u); }
`);
process.env.BLOB_READ_WRITE_TOKEN = 'teste';
const cloud = await import(pathToFileURL(path.join(dir, 'api/cloud.js')));
const transfer = await import(pathToFileURL(path.join(dir, 'api/transfer.js')));
const store = globalThis.__blobStore;

// saves que "já estavam na nuvem"
for (const s of seeds) {
  const [key, rest] = [s.slice(0, s.lastIndexOf('=')), s.slice(s.lastIndexOf('=') + 1)];
  const [file, at] = rest.split('@');
  const body = fs.readFileSync(file);
  const req = new Request(`http://127.0.0.1:${port}/api/cloud?action=put&key=${encodeURIComponent(key)}&at=${at}&from=Celular&name=seed&console=gba`,
    { method: 'POST', body, headers: { 'x-indigo-pin': process.env.INDIGO_PIN || '' } });
  const r = await cloud.POST(req);
  console.log('semente', key, r.status);
}

http.createServer(async (req, res) => {
  try {
    const url = new URL(req.url, `http://127.0.0.1:${port}`);
    if (url.pathname.startsWith('/__blob/')) {
      const b = store.get(`http://127.0.0.1:${port}${url.pathname}`);
      if (!b) { res.writeHead(404); res.end(); return; }
      res.writeHead(200, { 'content-type': 'application/octet-stream' }); res.end(b.body); return;
    }
    if (url.pathname === '/__dump') {
      const out = [...store.values()].map(b => `${b.pathname} ${b.size}`).sort();
      res.writeHead(200, { 'content-type': 'text/plain' }); res.end(out.join('\n') + '\n'); return;
    }
    const chunks = [];
    for await (const c of req) chunks.push(c);
    const body = chunks.length ? Buffer.concat(chunks) : undefined;
    const request = new Request(url, { method: req.method, headers: req.headers, body: req.method === 'GET' ? undefined : body });
    const mod = url.pathname === '/api/cloud' ? cloud : url.pathname === '/api/transfer' ? transfer : null;
    const fn = mod && mod[req.method];
    if (!fn) { res.writeHead(404); res.end('nao encontrado'); return; }
    const r = await fn(request);
    const buf = Buffer.from(await r.arrayBuffer());
    res.writeHead(r.status, Object.fromEntries(r.headers.entries()));
    res.end(buf);
    console.log(req.method, url.pathname + url.search.slice(0, 60), r.status);
  } catch (e) {
    console.log('erro', e);
    res.writeHead(500); res.end(String(e));
  }
}).listen(port, '127.0.0.1', () => console.log('nuvem de teste em', port));
