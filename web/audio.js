// Saída de som: AudioWorklet (roda na thread de áudio do navegador, não trava quando a página está ocupada).
// Sem suporte, cai para o ScriptProcessor antigo. As amostras chegam em blocos vindos do núcleo.

const WORKLET = `
class IndigoOut extends AudioWorkletProcessor {
  constructor() {
    super();
    this.q = []; this.off = 0; this.avail = 0; this.l = 0; this.r = 0; this.vol = 0.7; this.n = 0; this.max = sampleRate * 0.25;
    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.L) {
        this.q.push(d); this.avail += d.L.length;
        while (this.avail > this.max && this.q.length > 1) { const c = this.q.shift(); this.avail -= c.L.length - this.off; this.off = 0; }
      } else if (d.vol !== undefined) this.vol = d.vol;
      else if (d.clear) { this.q = []; this.avail = 0; this.off = 0; }
    };
  }
  process(inputs, outputs) {
    const L = outputs[0][0], R = outputs[0][1] || L;
    for (let i = 0; i < L.length; i++) {
      if (this.q.length) {
        const c = this.q[0];
        this.l = c.L[this.off]; this.r = c.R[this.off];
        this.avail--;
        if (++this.off >= c.L.length) { this.q.shift(); this.off = 0; }
      } else { this.l *= 0.98; this.r *= 0.98; }
      L[i] = this.l * this.vol; R[i] = this.r * this.vol;
    }
    if ((++this.n & 3) === 0) this.port.postMessage(this.avail);
    return true;
  }
}
registerProcessor('indigo-out', IndigoOut);
`;

export class AudioOut {
  constructor() {
    this.ctx = null; this.node = null; this.kind = 'none';
    this.buffered = 0; this.vol = 0.7;
    this.q = []; this.off = 0; // fila do modo antigo
  }

  get sampleRate() { return this.ctx ? this.ctx.sampleRate : 48000; }

  get running() { return !!this.ctx && this.ctx.state === 'running'; }
  get state() { return this.ctx ? this.ctx.state : 'none'; }

  // Chamar DENTRO do toque/clique (sem await antes): o iPhone e o Android só liberam o som se o contexto for
  // criado ou retomado no próprio gesto. Toca um trecho silencioso, que é o que destrava o Safari antigo.
  unlock() {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return Promise.resolve('none');
    // iPhone: toca mesmo com a chave de silencioso ligada (como um app de jogo), quando o Safari permite
    try { if (navigator.audioSession && navigator.audioSession.type !== 'playback') navigator.audioSession.type = 'playback'; } catch { /* ignorado */ }
    if (!this.ctx) {
      try { this.ctx = new AC({ latencyHint: 'interactive' }); } catch { this.ctx = new AC(); }
      this.ctx.onstatechange = () => { if (this.onstate) this.onstate(this.ctx.state); };
      this.ready = this.setup();
    }
    if (this.ctx.state !== 'running') {
      try {
        const b = this.ctx.createBuffer(1, 1, this.ctx.sampleRate);
        const src = this.ctx.createBufferSource();
        src.buffer = b; src.connect(this.ctx.destination); src.start(0);
      } catch { /* ignorado */ }
      const p = this.ctx.resume();
      if (p && p.catch) p.catch(() => {});
    }
    return this.ready;
  }

  async start() { return this.unlock(); }

  async setup() {
    try {
      if (!this.ctx.audioWorklet || typeof AudioWorkletNode === 'undefined') throw new Error('sem worklet');
      const url = URL.createObjectURL(new Blob([WORKLET], { type: 'text/javascript' }));
      await this.ctx.audioWorklet.addModule(url);
      this.node = new AudioWorkletNode(this.ctx, 'indigo-out', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
      this.node.port.onmessage = (e) => { this.buffered = e.data; };
      this.node.port.postMessage({ vol: this.vol });
      this.node.connect(this.ctx.destination);
      this.kind = 'worklet';
    } catch {
      // modo antigo: ScriptProcessor na thread da página
      const sp = this.ctx.createScriptProcessor(2048, 0, 2);
      let l = 0, r = 0;
      sp.onaudioprocess = (e) => {
        const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1);
        for (let i = 0; i < L.length; i++) {
          if (this.q.length) {
            const c = this.q[0];
            l = c.L[this.off]; r = c.R[this.off]; this.buffered--;
            if (++this.off >= c.L.length) { this.q.shift(); this.off = 0; }
          } else { l *= 0.98; r *= 0.98; }
          L[i] = l * this.vol; R[i] = r * this.vol;
        }
      };
      sp.connect(this.ctx.destination);
      this.node = sp;
      this.kind = 'script';
    }
    return this.kind;
  }

  // som de teste independente do jogo: três notas curtas (para conferir se o aparelho está tocando)
  beep() {
    if (!this.ctx) return;
    const t0 = this.ctx.currentTime + 0.03;
    [523.25, 659.25, 783.99].forEach((f, i) => {
      const o = this.ctx.createOscillator(), g = this.ctx.createGain();
      o.type = 'square'; o.frequency.value = f;
      const t = t0 + i * 0.14, v = Math.max(0.0001, 0.18 * this.vol);
      g.gain.setValueAtTime(0.0001, t);
      g.gain.exponentialRampToValueAtTime(v, t + 0.01);
      g.gain.exponentialRampToValueAtTime(0.0001, t + 0.12);
      o.connect(g); g.connect(this.ctx.destination);
      o.start(t); o.stop(t + 0.13);
    });
  }

  push(L, R) {
    if (!this.node || !L || !L.length || this.ctx.state !== 'running') return;
    if (this.kind === 'worklet') {
      this.node.port.postMessage({ L, R }, [L.buffer, R.buffer]);
      this.buffered += L.length; // estimativa até o próximo aviso do worklet
    } else {
      this.q.push({ L, R }); this.buffered += L.length;
      while (this.buffered > this.sampleRate * 0.25 && this.q.length > 1) { const c = this.q.shift(); this.buffered -= c.L.length - this.off; this.off = 0; }
    }
  }

  clear() {
    this.buffered = 0;
    if (this.kind === 'worklet') this.node.port.postMessage({ clear: 1 });
    else { this.q = []; this.off = 0; }
  }

  setVolume(v) {
    this.vol = v;
    if (this.kind === 'worklet' && this.node) this.node.port.postMessage({ vol: v });
  }
}
