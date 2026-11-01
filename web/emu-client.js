// Fala com o núcleo: num Web Worker quando o navegador deixa; senão, roda o mesmo código na página.
// O código do Worker vem embutido no build (__WORKER_SRC__), então tudo continua num arquivo só.

/* global __WORKER_SRC__ */
const SRC = typeof __WORKER_SRC__ !== 'undefined' ? __WORKER_SRC__ : '';

export class EmuClient {
  constructor(onMessage) {
    this.onMessage = onMessage;
    this.nextId = 1;
    this.pending = new Map();
    this.mode = 'inline';
    this.worker = null;
    this.ready = this.start();
  }

  async start() {
    if (typeof Worker !== 'undefined' && SRC) {
      try {
        const url = URL.createObjectURL(new Blob([SRC], { type: 'text/javascript' }));
        const w = new Worker(url);
        w.onmessage = (e) => this.receive(e.data);
        this.worker = w;
        this.mode = 'worker';
        const ok = await Promise.race([this.request({ t: 'ping' }).then(() => true, () => false), new Promise(r => setTimeout(() => r(false), 3000))]);
        if (ok) return 'worker';
        w.terminate();
      } catch { /* o navegador bloqueou o Worker */ }
    }
    this.worker = null;
    this.mode = 'inline';
    const fake = { postMessage: (m) => queueMicrotask(() => this.receive(m)), onmessage: null };
    try { new Function('self', SRC)(fake); } catch (e) { console.error(e); }
    this.inline = fake;
    // reenvia o que ficou pendurado no Worker que não respondeu
    for (const [id, p] of this.pending) if (p.msg) this.post(p.msg, []);
    return 'inline';
  }

  post(msg, transfer) {
    if (this.worker) this.worker.postMessage(msg, transfer || []);
    else if (this.inline && this.inline.onmessage) { const f = this.inline.onmessage; queueMicrotask(() => f({ data: msg })); }
  }

  request(msg, transfer) {
    const id = this.nextId++;
    msg.id = id;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject, msg: transfer && transfer.length ? null : msg });
      this.post(msg, transfer);
    });
  }

  receive(m) {
    if ((m.t === 'reply' || m.t === 'loaded') && this.pending.has(m.id)) {
      const p = this.pending.get(m.id);
      this.pending.delete(m.id);
      if (m.t === 'loaded') p.resolve(m);
      else if (m.ok) p.resolve(m.value); else p.reject(new Error(m.error || 'erro no núcleo'));
      return;
    }
    this.onMessage(m);
  }
}
