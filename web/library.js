// Biblioteca local: guarda as ROMs do usuário no IndexedDB deste navegador (nada sai do aparelho).
// Os dados de cada jogo (nome, capa, última vez jogado) ficam separados dos bytes da ROM, para que listar
// a estante não carregue todas as ROMs na memória. Os save states ficam num depósito próprio.
// Versão 3: os saves de cartucho saíram do localStorage (limite de ~5 MB, gravação que podia falhar calada)
// e vieram para cá, com histórico de versões anteriores de cada save.

const DB_NAME = 'indigo';
const META = 'meta', DATA = 'data', STATES = 'states', SAVES = 'saves', HIST = 'savehist', OLD = 'roms';
const HIST_MAX = 12;              // versões guardadas por save
const HIST_EVERY = 10 * 60 * 1000; // no máximo uma versão automática a cada 10 min
let dbPromise = null;

function openDb() {
  if (dbPromise) return dbPromise;
  dbPromise = new Promise((resolve, reject) => {
    let req;
    try { req = indexedDB.open(DB_NAME, 3); } catch (e) { reject(e); return; }
    req.onupgradeneeded = () => {
      const db = req.result, t = req.transaction;
      if (!db.objectStoreNames.contains(META)) db.createObjectStore(META, { keyPath: 'id' });
      if (!db.objectStoreNames.contains(DATA)) db.createObjectStore(DATA);
      if (!db.objectStoreNames.contains(STATES)) db.createObjectStore(STATES, { keyPath: 'key' }).createIndex('id', 'id');
      if (!db.objectStoreNames.contains(SAVES)) db.createObjectStore(SAVES, { keyPath: 'key' });
      if (!db.objectStoreNames.contains(HIST)) db.createObjectStore(HIST, { keyPath: 'hid' }).createIndex('key', 'key');
      // versão 1: tudo num registro só → separa dados e ROM
      if (db.objectStoreNames.contains(OLD)) {
        const old = t.objectStore(OLD), meta = t.objectStore(META), data = t.objectStore(DATA);
        old.openCursor().onsuccess = (e) => {
          const cur = e.target.result;
          if (!cur) { db.deleteObjectStore(OLD); return; }
          const { data: bytes, ...m } = cur.value;
          meta.put(m);
          if (bytes) data.put(bytes, m.id);
          cur.continue();
        };
      }
    };
    req.onsuccess = () => {
      const db = req.result;
      // outra aba precisa atualizar o banco: libera a conexão e reabre na próxima operação
      db.onversionchange = () => { db.close(); dbPromise = null; };
      resolve(db);
    };
    req.onerror = () => reject(req.error);
    req.onblocked = () => reject(new Error('Feche as outras abas do Índigo e recarregue.'));
  });
  return dbPromise;
}

function tx(stores, mode, fn) {
  return openDb().then(db => new Promise((resolve, reject) => {
    const t = db.transaction(stores, mode);
    let result;
    Promise.resolve(fn(t)).then(r => { result = r; });
    t.oncomplete = () => resolve(result);
    t.onerror = () => reject(t.error);
    t.onabort = () => reject(t.error);
  }));
}
const wrap = (req) => new Promise((res, rej) => { req.onsuccess = () => res(req.result); req.onerror = () => rej(req.error); });

// identificador estável: código do jogo + título + tamanho + hash FNV-1a amostrado
export function romId(bytes, gameCode, title) {
  let h = 0x811C9DC5;
  const step = Math.max(1, Math.floor(bytes.length / 65536));
  for (let i = 0; i < bytes.length; i += step) { h ^= bytes[i]; h = Math.imul(h, 0x01000193); }
  return `${gameCode.trim() || 'XXXX'}-${title.replace(/\W+/g, '') || 'rom'}-${bytes.length}-${(h >>> 0).toString(16)}`;
}

export const library = {
  available() {
    try { return typeof indexedDB !== 'undefined' && !!indexedDB; } catch { return false; }
  },

  // lista só os dados (sem os bytes da ROM)
  async list() {
    const all = await tx([META], 'readonly', t => wrap(t.objectStore(META).getAll()));
    return (all || []).sort((a, b) => (b.lastPlayed || b.added) - (a.lastPlayed || a.added));
  },

  async meta(id) { return tx([META], 'readonly', t => wrap(t.objectStore(META).get(id))); },

  // dados + bytes da ROM
  async get(id) {
    return tx([META, DATA], 'readonly', async t => {
      const [m, data] = await Promise.all([wrap(t.objectStore(META).get(id)), wrap(t.objectStore(DATA).get(id))]);
      return m ? Object.assign({}, m, { data }) : null;
    });
  },

  async add(meta, bytes) {
    const existing = await this.meta(meta.id).catch(() => null);
    const rec = Object.assign({ added: Date.now(), lastPlayed: 0, cover: null }, existing || {}, meta);
    delete rec.data;
    const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength);
    await tx([META, DATA], 'readwrite', t => { t.objectStore(META).put(rec); t.objectStore(DATA).put(buf, rec.id); });
    return !existing;
  },

  async update(id, patch) {
    return tx([META], 'readwrite', async t => {
      const s = t.objectStore(META);
      const rec = await wrap(s.get(id));
      if (!rec) return;
      Object.assign(rec, patch);
      delete rec.data;
      s.put(rec);
    });
  },

  // remove o cartucho; com withSave também apaga o save (fica uma cópia no histórico)
  async removeAll(id, saveKey) {
    await this.remove(id);
    if (saveKey) await this.deleteSave(saveKey);
  },

  async remove(id) {
    return tx([META, DATA, STATES], 'readwrite', async t => {
      t.objectStore(META).delete(id);
      t.objectStore(DATA).delete(id);
      const keys = await wrap(t.objectStore(STATES).index('id').getAllKeys(id));
      for (const k of keys) t.objectStore(STATES).delete(k);
    });
  },

  // ---------------- save states (por jogo e por espaço)
  async putState(id, slot, state, thumb) {
    const rec = { key: `${id}#${slot}`, id, slot, at: Date.now(), thumb: thumb || null, state };
    await tx([STATES], 'readwrite', t => { t.objectStore(STATES).put(rec); });
    return rec.at;
  },
  async getState(id, slot) { return tx([STATES], 'readonly', t => wrap(t.objectStore(STATES).get(`${id}#${slot}`))); },
  async listStates(id) {
    const all = await tx([STATES], 'readonly', t => wrap(t.objectStore(STATES).index('id').getAll(id)));
    return (all || []).map(({ state, ...m }) => m);
  },

  // ---------------- saves de cartucho (bytes crus, por chave de save)
  async getSave(key) {
    if (!key) return null;
    const r = await tx([SAVES], 'readonly', t => wrap(t.objectStore(SAVES).get(key)));
    return r ? Object.assign({}, r, { data: toU8(r.data) }) : null;
  },
  // lista sem os bytes
  async listSaves() {
    const all = await tx([SAVES], 'readonly', t => wrap(t.objectStore(SAVES).getAll()));
    return (all || []).map(({ data, ...m }) => Object.assign(m, { size: m.size ?? (data ? data.byteLength : 0) }));
  },
  // grava o save atual; guarda uma versão no histórico quando muda (no máximo a cada 10 min, ou sempre com force)
  async putSave(key, bytes, opts = {}) {
    const data = toBuf(bytes), at = opts.at || Date.now();
    await tx([SAVES, HIST], 'readwrite', async t => {
      t.objectStore(SAVES).put({ key, at, size: data.byteLength, data, source: opts.source || 'jogo' });
      const hist = ((await wrap(t.objectStore(HIST).index('key').getAll(key))) || []).sort((a, b) => b.at - a.at);
      const last = hist[0];
      const changed = !last || !sameBytes(last.data, data);
      if (changed && (opts.force || !last || at - last.at >= HIST_EVERY)) {
        t.objectStore(HIST).put({ hid: `${key}@${at}`, key, at, size: data.byteLength, data, reason: opts.reason || 'automático' });
        hist.unshift({ hid: `${key}@${at}` });
      }
      for (const old of hist.slice(HIST_MAX)) t.objectStore(HIST).delete(old.hid);
    });
    return at;
  },
  // guarda a versão atual no histórico antes de uma troca (importar, restaurar, receber de outro aparelho)
  async snapshotSave(key, reason) {
    const cur = await this.getSave(key);
    if (!cur) return false;
    await tx([HIST], 'readwrite', t => {
      t.objectStore(HIST).put({ hid: `${key}@${cur.at}#${reason}`, key, at: cur.at, size: cur.data.byteLength, data: toBuf(cur.data), reason });
    });
    return true;
  },
  async saveHistory(key) {
    const all = await tx([HIST], 'readonly', t => wrap(t.objectStore(HIST).index('key').getAll(key)));
    return (all || []).sort((a, b) => b.at - a.at).map(r => Object.assign({}, r, { data: toU8(r.data) }));
  },
  async deleteSave(key) {
    await this.snapshotSave(key, 'antes de apagar');
    await tx([SAVES], 'readwrite', t => { t.objectStore(SAVES).delete(key); });
  },
  async allHistory() {
    const all = await tx([HIST], 'readonly', t => wrap(t.objectStore(HIST).getAll()));
    return all || [];
  },
  async putHistory(rec) { await tx([HIST], 'readwrite', t => { t.objectStore(HIST).put(Object.assign({}, rec, { data: toBuf(rec.data) })); }); },
  async allStates() { return (await tx([STATES], 'readonly', t => wrap(t.objectStore(STATES).getAll()))) || []; },
  async putStateRecord(rec) { await tx([STATES], 'readwrite', t => { t.objectStore(STATES).put(rec); }); },
  async renameState(id, slot, name) {
    await tx([STATES], 'readwrite', async t => {
      const s = t.objectStore(STATES), rec = await wrap(s.get(`${id}#${slot}`));
      if (rec) { rec.name = name; s.put(rec); }
    });
  },
  async deleteState(id, slot) { await tx([STATES], 'readwrite', t => { t.objectStore(STATES).delete(`${id}#${slot}`); }); },

  async usage() {
    try {
      if (navigator.storage && navigator.storage.estimate) return await navigator.storage.estimate();
    } catch { /* indisponível */ }
    return null;
  },

  async persist() {
    try { if (navigator.storage && navigator.storage.persist) return await navigator.storage.persist(); } catch { /* ignorado */ }
    return false;
  },
};

function toBuf(b) {
  if (b instanceof ArrayBuffer) return b.slice(0);
  return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
}
function toU8(b) { return b instanceof Uint8Array ? b : new Uint8Array(b || 0); }
function sameBytes(a, b) {
  const x = toU8(a), y = toU8(b);
  if (x.length !== y.length) return false;
  for (let i = 0; i < x.length; i++) if (x[i] !== y[i]) return false;
  return true;
}
