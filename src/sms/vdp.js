// Vídeo do Master System / Game Gear (VDP 315-5124/5246, modo 4): tiles de 8×8 em 4 bits, 2 paletas de 16,
// rolagem com travas (topo/direita), coluna esquerda apagável, sprites 8×8/8×16 com zoom, 8 por linha,
// colisão, interrupções de quadro e de linha. No Game Gear a paleta tem 12 bits e a tela é a janela 160×144.

export class VDP {
  constructor(gg) {
    this.gg = gg;
    this.vram = new Uint8Array(0x4000);
    this.cram = new Uint8Array(gg ? 64 : 32);
    this.regs = new Uint8Array(16);
    this.rgb = new Uint32Array(32);       // cores prontas (ABGR)
    this.line = new Uint32Array(256);
    this.bgPri = new Uint8Array(256);     // pixel do fundo com prioridade e cor ≠ 0
    this.sprCol = new Uint8Array(256);
    this.w = gg ? 160 : 256; this.h = gg ? 144 : 192;
    this.fb = new Uint32Array(256 * 192);
    this.reset();
  }

  reset() {
    this.vram.fill(0); this.cram.fill(0); this.regs.fill(0);
    // valores deixados pela BIOS
    const init = [0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF];
    init.forEach((v, i) => { this.regs[i] = v; });
    this.addr = 0; this.code = 0; this.latch = false; this.first = 0; this.buffer = 0; this.ggLatch = 0;
    this.status = 0; this.lineIrq = false;
    this.lineCounter = 0xFF; this.vscrollLatch = 0;
    this.vcount = 0; this.hcount = 0;
    for (let i = 0; i < 32; i++) this.rgb[i] = 0xFF000000;
    this.fb.fill(0xFF000000);
  }

  irq() { return ((this.status & 0x80) && (this.regs[1] & 0x20)) || (this.lineIrq && (this.regs[0] & 0x10)); }

  // ------------------------------------------------------------ portas
  readData() {
    this.latch = false;
    const v = this.buffer;
    this.buffer = this.vram[this.addr];
    this.addr = (this.addr + 1) & 0x3FFF;
    return v;
  }
  writeData(v) {
    this.latch = false;
    if (this.code === 3) {
      if (this.gg) {
        if (!(this.addr & 1)) this.ggLatch = v;
        else {
          const i = this.addr & 0x3E;
          this.cram[i] = this.ggLatch; this.cram[i + 1] = v & 0x0F;
          this.color(i >> 1);
        }
      } else { const i = this.addr & 0x1F; this.cram[i] = v & 0x3F; this.color(i); }
    } else this.vram[this.addr] = v;
    this.buffer = v;
    this.addr = (this.addr + 1) & 0x3FFF;
  }
  readStatus() {
    const v = this.status | 0x1F;
    this.status = 0; this.lineIrq = false; this.latch = false;
    return v;
  }
  writeControl(v) {
    if (!this.latch) { this.first = v; this.addr = (this.addr & 0x3F00) | v; this.latch = true; return; }
    this.latch = false;
    this.code = v >> 6;
    this.addr = ((v & 0x3F) << 8) | this.first;
    if (this.code === 0) { this.buffer = this.vram[this.addr]; this.addr = (this.addr + 1) & 0x3FFF; }
    else if (this.code === 2) this.regs[v & 0x0F] = this.first;
  }

  color(i) {
    let r, g, b;
    if (this.gg) {
      const w = this.cram[i * 2] | (this.cram[i * 2 + 1] << 8);
      r = (w & 0xF) * 17; g = ((w >> 4) & 0xF) * 17; b = ((w >> 8) & 0xF) * 17;
    } else {
      const c = this.cram[i];
      r = (c & 3) * 85; g = ((c >> 2) & 3) * 85; b = ((c >> 4) & 3) * 85;
    }
    this.rgb[i] = (0xFF000000 | (b << 16) | (g << 8) | r) >>> 0;
  }

  // contador vertical lido na porta 0x7E (NTSC, 192 linhas: 0–DA, depois D5–FF)
  readV() { return this.vcount <= 0xDA ? this.vcount : this.vcount - 6; }

  // ------------------------------------------------------------ por linha
  // início da linha: contador de linha (gera a interrupção de linha) e rolagem vertical
  lineStart(line) {
    this.vcount = line;
    if (line === 0) this.vscrollLatch = this.regs[9];
    if (line <= 192) {
      if (this.lineCounter === 0) { this.lineCounter = this.regs[10]; this.lineIrq = true; }
      else this.lineCounter = (this.lineCounter - 1) & 0xFF;
    } else this.lineCounter = this.regs[10];
    if (line === 193) this.status |= 0x80; // fim da imagem: interrupção de quadro
  }

  renderLine(y) {
    const r0 = this.regs[0], r1 = this.regs[1];
    const out = this.line, back = this.rgb[16 + (this.regs[7] & 15)];
    if (!(r1 & 0x40)) { out.fill(back); this.store(y); return; }
    if (!(r0 & 0x04)) { out.fill(0xFF000000); this.store(y); return; } // modos TMS antigos: não suportados
    this.bgLine(y);
    this.sprites(y);
    if (r0 & 0x20) for (let x = 0; x < 8; x++) out[x] = back;
    this.store(y);
  }

  store(y) {
    if (this.gg) {
      if (y < 24 || y >= 168) return;
      this.fb.set(this.line.subarray(48, 208), (y - 24) * 160);
    } else this.fb.set(this.line, y * 256);
  }

  bgLine(y) {
    const r0 = this.regs[0], vram = this.vram, out = this.line, pri = this.bgPri, rgb = this.rgb;
    const nameBase = (this.regs[2] & 0x0E) << 10;
    const hs = (r0 & 0x40) && y < 16 ? 0 : this.regs[8];
    const vyScroll = (y + this.vscrollLatch) % 224;
    let lastKey = -1, p0 = 0, p1 = 0, p2 = 0, p3 = 0, pal = 0, prio = 0, hflip = 0;
    for (let x = 0; x < 256; x++) {
      // as 8 colunas da direita podem ignorar a rolagem vertical (placar fixo)
      const vy = (r0 & 0x80) && x >= 192 ? y : vyScroll;
      const bx = (x - hs) & 255;
      const key = ((vy >> 3) << 5) | (bx >> 3);
      if (key !== lastKey) {
        lastKey = key;
        const ent = nameBase + (key << 1);
        const w = vram[ent] | (vram[ent + 1] << 8);
        hflip = w & 0x200; pal = (w & 0x800) ? 16 : 0; prio = w & 0x1000;
        let ty = vy & 7; if (w & 0x400) ty = 7 - ty;
        const a = ((w & 0x1FF) << 5) + (ty << 2);
        p0 = vram[a]; p1 = vram[a + 1]; p2 = vram[a + 2]; p3 = vram[a + 3];
      }
      const px = bx & 7, bit = hflip ? px : 7 - px;
      const c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
      out[x] = rgb[pal + c];
      pri[x] = prio && c ? 1 : 0;
    }
  }

  sprites(y) {
    const r0 = this.regs[0], r1 = this.regs[1], vram = this.vram, out = this.line;
    const sat = (this.regs[5] & 0x7E) << 7;
    const big = r1 & 2, zoom = r1 & 1;
    const h = (big ? 16 : 8) << (zoom ? 1 : 0);
    const patBase = (this.regs[6] & 4) ? 256 : 0;
    const shift = (r0 & 8) ? 8 : 0;
    const col = this.sprCol; col.fill(0);
    const list = [];
    for (let i = 0; i < 64; i++) {
      const sy = vram[sat + i];
      if (sy === 0xD0) break;
      let top = sy + 1; if (top > 240) top -= 256;
      if (y >= top && y < top + h) {
        if (list.length === 8) { this.status |= 0x40; break; }
        list.push(i);
      }
    }
    // o primeiro da lista tem prioridade: desenha de trás para frente
    for (let k = list.length - 1; k >= 0; k--) {
      const i = list[k];
      let top = vram[sat + i] + 1; if (top > 240) top -= 256;
      const sx = vram[sat + 0x80 + i * 2] - shift;
      let tile = vram[sat + 0x81 + i * 2] + patBase;
      if (big) tile &= ~1;
      let ty = (y - top) >> (zoom ? 1 : 0);
      if (ty >= 8) { tile += 1; ty -= 8; }
      const a = ((tile & 0x1FF) << 5) + (ty << 2);
      const p0 = vram[a], p1 = vram[a + 1], p2 = vram[a + 2], p3 = vram[a + 3];
      const wpx = zoom ? 16 : 8;
      for (let px = 0; px < wpx; px++) {
        const x = sx + px;
        if (x < 0 || x > 255) continue;
        const bit = 7 - (zoom ? px >> 1 : px);
        const c = ((p0 >> bit) & 1) | (((p1 >> bit) & 1) << 1) | (((p2 >> bit) & 1) << 2) | (((p3 >> bit) & 1) << 3);
        if (!c) continue;
        if (col[x]) this.status |= 0x20;
        col[x] = 1;
        if (!this.bgPri[x]) out[x] = this.rgb[16 + c];
      }
    }
  }

  // ------------------------------------------------------------ save state
  state() {
    return { vram: this.vram.slice(), cram: this.cram.slice(), regs: this.regs.slice(),
      s: [this.addr, this.code, this.latch, this.first, this.buffer, this.ggLatch, this.status, this.lineIrq, this.lineCounter, this.vscrollLatch].map(Number) };
  }
  restore(st) {
    this.vram.set(st.vram); this.cram.set(st.cram); this.regs.set(st.regs);
    [this.addr, this.code, this.latch, this.first, this.buffer, this.ggLatch, this.status, this.lineIrq, this.lineCounter, this.vscrollLatch] = st.s;
    this.latch = !!this.latch; this.lineIrq = !!this.lineIrq;
    for (let i = 0; i < 32; i++) this.color(i);
  }
}
