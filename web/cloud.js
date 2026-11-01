// Saves na nuvem: cada save gravado neste aparelho sobe para o Vercel Blob (rota /api/cloud) e os saves mais
// novos feitos em outros aparelhos descem para cá. ROMs nunca sobem. Em conflito vale o mais novo, e o outro
// vai para as versões anteriores do save (nada se perde).

import { pinFetch, askPin } from './pin.js';

const API = '/api/cloud';
const DEVICE = /Xbox/i.test(navigator.userAgent) ? 'Xbox' : /Android|iPhone|iPad/i.test(navigator.userAgent) ? 'Celular' : 'Computador';

async function call(params, init, interactive = false) {
  const qs = new URLSearchParams(params).toString();
  const r = await pinFetch(`${API}?${qs}`, Object.assign({ credentials: 'same-origin', cache: 'no-store' }, init || {}), interactive);
  const type = r.headers.get('content-type') || '';
  if (!r.ok) {
    let msg = `Erro ${r.status} na nuvem.`;
    if (type.includes('json')) { try { msg = (await r.json()).error || msg; } catch { /* mantém */ } }
    throw new Error(msg);
  }
  return type.includes('json') ? r.json() : r;
}
const withTimeout = (p, ms) => Promise.race([p, new Promise((_, rej) => setTimeout(() => rej(new Error('A nuvem demorou para responder.')), ms))]);

export class Cloud {
  // deps: library, saveInfo(key) → { name, console } | null, isBusy(key) → true se o jogo está aberto, log(texto), onChange()
  constructor(deps) {
    this.d = deps;
    this.state = 'off';   // off | idle | syncing | error | unavailable
    this.lastSync = Number(localStorage.getItem('indigo-cloud-last') || 0) || 0;
    this.lastError = '';
    this.count = 0;
    this.queue = new Set();
    this.timer = 0;
    this.running = null;
    this.waiting = new Map(); // saves mais novos na nuvem para o jogo aberto (aplicados ao reabrir)
  }

  get enabled() { try { return localStorage.getItem('indigo-cloud-on') !== '0'; } catch { return true; } }
  set enabled(v) { try { localStorage.setItem('indigo-cloud-on', v ? '1' : '0'); } catch { /* ignorado */ } this.state = v ? 'idle' : 'off'; this.d.onChange(); if (v) this.sync('ligada'); }

  async init() {
    if (!/^https?:$/.test(location.protocol)) { this.state = 'unavailable'; return; }
    try {
      const r = await fetch('/api/transfer?action=ping', { cache: 'no-store', credentials: 'same-origin' });
      const j = r.ok && (r.headers.get('content-type') || '').includes('json') ? await r.json() : null;
      this.state = j && j.storage ? (this.enabled ? 'idle' : 'off') : 'unavailable';
    } catch { this.state = 'unavailable'; }
    this.d.onChange();
    if (this.state === 'idle') this.sync('abertura');
  }
  get usable() { return this.state !== 'unavailable' && this.enabled; }

  // pede o envio de um save (agrupa gravações seguidas: sobe 20 s depois da última)
  touch(key) {
    if (!this.usable || !key.startsWith('rom:')) return;
    this.queue.add(key);
    clearTimeout(this.timer);
    this.timer = setTimeout(() => this.flush(), 20000);
  }
  async flush() {
    clearTimeout(this.timer);
    if (!this.queue.size || !this.usable) return;
    const keys = [...this.queue]; this.queue.clear();
    for (const k of keys) { try { await this.upload(k); } catch (e) { this.fail(e); this.queue.add(k); } }
    this.d.onChange();
  }

  async upload(key) {
    const s = await this.d.library.getSave(key);
    if (!s) return false;
    const meta = this.d.saveInfo(key) || {};
    await call({ action: 'put', key, at: String(s.at), from: DEVICE, name: meta.name || '', console: meta.console || '' },
      { method: 'POST', body: s.data, headers: { 'content-type': 'application/octet-stream' } });
    this.lastSync = Date.now(); localStorage.setItem('indigo-cloud-last', String(this.lastSync));
    return true;
  }

  async download(key, at, from) {
    const r = await call({ action: 'get', key, at: String(at) });
    const bytes = new Uint8Array(await r.arrayBuffer());
    const lib = this.d.library;
    await lib.snapshotSave(key, 'antes de receber da nuvem').catch(() => {});
    await lib.putSave(key, bytes, { at, source: 'nuvem', reason: `veio da nuvem${from ? ' (' + from + ')' : ''}`, force: true });
    return bytes;
  }

  // antes de abrir um jogo: traz da nuvem se lá houver uma versão mais nova (com tempo limite curto)
  async pullBeforeBoot(key) {
    if (!this.usable || !key.startsWith('rom:')) return false;
    try {
      const { versions } = await withTimeout(call({ action: 'versions', key }), 4000);
      const top = versions && versions[0];
      if (!top) return false;
      const local = await this.d.library.getSave(key);
      if (local && local.at >= top.at) return false;
      await withTimeout(this.download(key, top.at, top.from), 8000);
      this.waiting.delete(key);
      this.d.log(`Save de ${top.name || 'um jogo'} trazido da nuvem (${top.from || 'outro aparelho'}) ao abrir`);
      return true;
    } catch { return false; }
  }

  async versions(key) {
    if (!this.usable) return [];
    try { return (await withTimeout(call({ action: 'versions', key }), 6000)).versions || []; } catch { return []; }
  }

  // sincronização completa: sobe o que é mais novo aqui, desce o que é mais novo lá
  sync(reason, interactive = false) {
    if (!this.usable) return Promise.resolve(null);
    if (this.state === 'locked' && !interactive) return Promise.resolve(null);
    if (this.running) return this.running;
    this.state = 'syncing'; this.d.onChange();
    this.running = (async () => {
      let up = 0, down = 0;
      try {
        const { saves: remote } = await withTimeout(call({ action: 'list' }, null, interactive), interactive ? 120000 : 10000);
        const rmap = new Map(remote.map(r => [r.key, r]));
        const local = (await this.d.library.listSaves()).filter(s => s.key.startsWith('rom:'));
        const lmap = new Map(local.map(s => [s.key, s]));
        for (const s of local) {
          const r = rmap.get(s.key);
          if (!r || s.at > r.at) { if (await this.upload(s.key)) up++; }
        }
        for (const r of remote) {
          const s = lmap.get(r.key);
          if (s && s.at >= r.at) continue;
          if (this.d.isBusy(r.key)) { this.waiting.set(r.key, r); continue; }
          await this.download(r.key, r.at, r.from); down++;
        }
        this.count = new Set([...rmap.keys(), ...lmap.keys()]).size;
        this.lastSync = Date.now(); localStorage.setItem('indigo-cloud-last', String(this.lastSync));
        this.state = 'idle'; this.lastError = '';
        if (up || down) this.d.log(`Nuvem sincronizada (${reason}): ${up} save(s) enviado(s), ${down} recebido(s)`);
        return { up, down };
      } catch (e) { this.fail(e); return null; }
      finally { this.running = null; this.d.onChange(); }
    })();
    return this.running;
  }

  fail(e) { this.state = e && e.pin ? 'locked' : 'error'; this.lastError = (e && e.message) || 'erro'; this.d.onChange(); }
  async unlock() { if (await askPin()) { this.state = 'idle'; this.d.onChange(); return this.sync('PIN digitado'); } return null; }

  statusText() {
    const t = this.lastSync ? new Date(this.lastSync).toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' }) : '';
    switch (this.state) {
      case 'unavailable': return '';
      case 'off': return 'Saves na nuvem desligados';
      case 'syncing': return 'Sincronizando saves com a nuvem…';
      case 'error': return `Nuvem com problema: ${this.lastError}`;
      case 'locked': return 'Nuvem bloqueada: digite o PIN neste aparelho';
      default: return `Saves na nuvem${t ? `: sincronizados às ${t}` : ''}${this.count ? ` · ${this.count} jogo(s)` : ''}`;
    }
  }
}

export const DEVICE_NAME = DEVICE;
