// Vídeo do Neo Geo Pocket Color (K2GE): 160×152, dois planos de 32×32 blocos, 64 sprites de 8×8,
// paletas de 12 bits (modo cor) ou 8 tons de cinza (modo K1GE, jogos monocromáticos), janela de recorte.
// Os registros de rolagem/janela/cor de fundo são copiados no fim de cada linha (efeito na linha seguinte).

export const W = 160, H = 152;
const COLOR = new Uint32Array(4096);
for (let i = 0; i < 4096; i++) {
  const r = (i & 15) * 17, g = ((i >> 4) & 15) * 17, b = ((i >> 8) & 15) * 17;
  COLOR[i] = (0xFF000000 | (b << 16) | (g << 8) | r) >>> 0;
}
// tom monocromático (0 = branco … 7 = quase preto) em cor de 12 bits
const shade = (v) => (~(((v & 7) << 1) | ((v & 7) << 5) | ((v & 7) << 9))) & 0xFFF;
// espelhamento horizontal de uma linha de bloco (8 pixels de 2 bits)
const MIRROR16 = new Uint16Array(65536);
for (let i = 0; i < 65536; i++) {
  let m = 0;
  for (let p = 0; p < 8; p++) m |= ((i >> (p * 2)) & 3) << ((7 - p) * 2);
  MIRROR16[i] = m;
}

export class K2GE {
  constructor(sys) {
    this.sys = sys;
    this.fb = new Uint32Array(W * H);
    this.scroll = new Uint8Array(0x1000);   // 9000–9FFF: plano 1 (9000) e plano 2 (9800)
    this.chr = new Uint8Array(0x2000);      // A000–BFFF: 512 blocos de 16 bytes
    this.spr = new Uint8Array(0x100);       // 8800–88FF
    this.sprCol = new Uint8Array(0x40);     // 8C00–8C3F
    this.pal = new Uint8Array(0x200);       // 8200–83FF
    this.monoPal = new Uint8Array(0x20);    // 8100–811F (paletas do modo monocromático)
    this.line = new Uint16Array(256);
    this.z = new Uint8Array(256);
    this.power();
  }

  power() {
    this.scroll.fill(0); this.chr.fill(0); this.spr.fill(0); this.sprCol.fill(0); this.pal.fill(0);
    this.fb.fill(0xFF000000);
    this.reset();
  }

  reset() {
    this.monoPal.fill(7);
    this.raster = 0;
    this.s1x = 0; this.s1y = 0; this.s2x = 0; this.s2y = 0;
    this.wbaH = 0; this.wbaV = 0; this.wsiH = 0xFF; this.wsiV = 0xFF;
    this.cOvr = 0; this.blnk = 0;
    this.poH = 0; this.poV = 0; this.pf = 0; this.bgCol = 7;
    this.ctl2d = 0; this.ctlInt = 0; this.period = 0xC6; this.mode = 0;
    this.latch();
  }

  latch() {
    this.winx = this.wbaH; this.winy = this.wbaV; this.winw = this.wsiH; this.winh = this.wsiV;
    this.sc1x = this.s1x; this.sc1y = this.s1y; this.sc2x = this.s2x; this.sc2y = this.s2y;
    this.spx = this.poH; this.spy = this.poV;
    this.swap = this.pf & 0x80;
    this.bgc = this.bgCol;
    this.oowc = this.ctl2d & 7; this.neg = this.ctl2d & 0x80;
  }

  // ------------------------------------------------------------ registros
  read8(a) {
    if (a >= 0x9000 && a < 0xA000) return this.scroll[a - 0x9000];
    if (a >= 0xA000 && a < 0xC000) return this.chr[a - 0xA000];
    if (a >= 0x8800 && a < 0x8900) return this.spr[a - 0x8800];
    if (a >= 0x8C00 && a < 0x8C40) return this.sprCol[a - 0x8C00];
    if (a >= 0x8200 && a < 0x8400) return this.pal[a - 0x8200];
    if (a >= 0x8100 && a < 0x8118) return this.monoPal[a - 0x8100];
    switch (a) {
      case 0x8000: return this.ctlInt;
      case 0x8002: return this.wbaH;
      case 0x8003: return this.wbaV;
      case 0x8004: return this.wsiH;
      case 0x8005: return this.wsiV;
      case 0x8006: return this.period;
      case 0x8008: return (Math.abs(515 - this.sys.lineCycles) >> 2) & 0xFF; // posição horizontal aproximada
      case 0x8009: return this.raster;
      case 0x8010: return (this.cOvr ? 0x80 : 0) | (this.blnk ? 0x40 : 0);
      case 0x8012: return this.ctl2d;
      case 0x8020: return this.poH;
      case 0x8021: return this.poV;
      case 0x8030: return this.pf;
      case 0x8032: return this.s1x;
      case 0x8033: return this.s1y;
      case 0x8034: return this.s2x;
      case 0x8035: return this.s2y;
      case 0x8118: return this.bgCol;
      case 0x87E2: return this.mode;
      case 0x87FE: return 0x3F; // entrada do controle de expansão (nada ligado)
      default: return 0;
    }
  }

  write8(a, v) {
    if (a >= 0x9000 && a < 0xA000) { this.scroll[a - 0x9000] = v; return; }
    if (a >= 0xA000 && a < 0xC000) { this.chr[a - 0xA000] = v; return; }
    if (a >= 0x8800 && a < 0x8900) { this.spr[a - 0x8800] = v; return; }
    if (a >= 0x8C00 && a < 0x8C40) { this.sprCol[a - 0x8C00] = v & 0x0F; return; }
    if (a >= 0x8200 && a < 0x8400) { this.pal[a - 0x8200] = (a & 1) ? v & 0x0F : v; return; }
    if (a >= 0x8100 && a < 0x8118) { if (a & 3) this.monoPal[a - 0x8100] = v & 7; return; }
    switch (a) {
      case 0x8000: this.ctlInt = v & 0xC0; break;
      case 0x8002: this.wbaH = v; break;
      case 0x8003: this.wbaV = v; break;
      case 0x8004: this.wsiH = v; break;
      case 0x8005: this.wsiV = v; break;
      case 0x8006: this.period = v; break;
      case 0x8012: this.ctl2d = v & 0x87; break;
      case 0x8020: this.poH = v; break;
      case 0x8021: this.poV = v; break;
      case 0x8030: this.pf = v & 0x80; break;
      case 0x8032: this.s1x = v; break;
      case 0x8033: this.s1y = v; break;
      case 0x8034: this.s2x = v; break;
      case 0x8035: this.s2y = v; break;
      case 0x8118: this.bgCol = v & 0xC7; break;
      case 0x87E0: if (v === 0x52) this.sys.requestReset = true; break; // reinício pelo vídeo
      case 0x87E2: this.mode = v & 0x80; break;
      default: break;
    }
  }

  // ------------------------------------------------------------ temporização (chamada a cada 515 ciclos)
  // devolve true se deve haver interrupção de linha (usada pelo timer 0)
  hblank() {
    if (this.raster < H - 1 || this.raster === this.period) {
      this.latch();
      if (this.ctlInt & 0x40) return true;
    }
    return false;
  }

  // desenha a linha atual e avança; devolve true no início do retraço vertical
  endLine() {
    if (this.raster < H) this.drawLine(this.raster);
    this.raster++;
    let vbl = false;
    if (this.raster === H) { this.blnk = 1; vbl = true; }
    if (this.raster === this.period + 1) { this.raster = 0; this.cOvr = 0; this.blnk = 0; }
    return vbl;
  }

  // ------------------------------------------------------------ desenho
  drawLine(y) {
    const line = this.line, z = this.z, mono = this.mode !== 0;
    z.fill(0);
    const neg = this.neg ? 0xFFF : 0;
    const inWin = y >= this.winy && y < this.winy + this.winh;
    const wx0 = this.winx, wx1 = Math.min(this.winx + this.winw, W);
    // cor de fora da janela
    let out;
    if (mono) out = shade(this.oowc) ^ neg;
    else out = ((this.pal[0x1F0 + this.oowc * 2] | (this.pal[0x1F1 + this.oowc * 2] << 8)) & 0xFFF) ^ neg;
    line.fill(out, 0, W);
    if (inWin) {
      let bg;
      if (mono) bg = ((this.bgc & 0xC0) === 0x80 ? shade(this.bgc) : 0xFFF) ^ neg;
      else { const k = 0x1E0 + (this.bgc & 7) * 2; bg = ((this.pal[k] | (this.pal[k + 1] << 8)) & 0xFFF) ^ neg; }
      if (wx1 > wx0) line.fill(bg, wx0, wx1);
      if (this.swap) { this.plane(y, 0, 3, mono); this.plane(y, 1, 5, mono); }
      else { this.plane(y, 1, 3, mono); this.plane(y, 0, 5, mono); }
      this.sprites(y, mono);
    }
    const fb = this.fb, o = y * W;
    for (let x = 0; x < W; x++) fb[o + x] = COLOR[line[x] & 0xFFF];
  }

  // plano 0 = rolagem 1 (9000), plano 1 = rolagem 2 (9800)
  plane(y, p, depth, mono) {
    const sx = p ? this.sc2x : this.sc1x, sy = p ? this.sc2y : this.sc1y;
    const ln = (y + sy) & 0xFF, row = ln & 7, base = (p ? 0x800 : 0) + ((ln >> 3) << 6);
    const sc = this.scroll;
    for (let i = 0; i < 32; i++) {
      const d = sc[base + i * 2] | (sc[base + i * 2 + 1] << 8);
      this.pattern(((i << 3) - sx) & 0xFF, d & 0x1FF, (d & 0x4000) ? 7 - row : row, d & 0x8000,
        mono ? (p ? 0x10 : 0x08) + ((d & 0x2000) ? 4 : 0) : (p ? 0x100 : 0x80) + ((d >> 9) & 15) * 8, depth, mono);
    }
  }

  sprites(y, mono) {
    const s = this.spr;
    let lx = 0, ly = 0;
    for (let n = 0; n < 64; n++) {
      const d = s[n * 4] | (s[n * 4 + 1] << 8);
      let x = s[n * 4 + 2], yy = s[n * 4 + 3];
      if (d & 0x0400) x = lx + x;      // encadeado na horizontal
      if (d & 0x0200) yy = ly + yy;    // encadeado na vertical
      lx = x; ly = yy;
      const prio = (d >> 11) & 3;
      if (!prio) continue;
      x += this.spx; yy += this.spy;
      x = (x > 248 && x < 256) ? x - 256 : x & 0xFF;
      yy = (yy > 248 && yy < 256) ? yy - 256 : yy & 0xFF;
      if (y < yy || y > yy + 7) continue;
      const row = (y - yy) & 7;
      this.pattern(x & 0xFF, d & 0x1FF, (d & 0x4000) ? 7 - row : row, d & 0x8000,
        mono ? ((d & 0x2000) ? 4 : 0) : (this.sprCol[n] & 15) * 8, prio << 1, mono);
    }
  }

  // uma linha de um bloco na posição x (0–255; acima de 248 volta pela esquerda)
  pattern(sx, tile, row, flip, palBase, depth, mono) {
    let x = sx > 0xF8 ? sx - 256 : sx;
    if (x >= W) return;
    const k = tile * 16 + row * 2;
    let d = this.chr[k] | (this.chr[k + 1] << 8);
    if (!d) return;
    if (flip) d = MIRROR16[d];
    const left = Math.max(x, this.winx, 0), right = Math.min(x + 7, Math.min(this.winx + this.winw, W) - 1);
    const line = this.line, z = this.z, neg = this.neg ? 0xFFF : 0;
    for (let xx = left; xx <= right; xx++) {
      const c = (d >> ((7 - (xx - x)) * 2)) & 3;
      if (!c || depth <= z[xx]) continue;
      z[xx] = depth;
      let col;
      if (mono) col = shade(this.monoPal[palBase + c]);
      else col = (this.pal[palBase + c * 2] | (this.pal[palBase + c * 2 + 1] << 8)) & 0xFFF;
      line[xx] = col ^ neg;
    }
  }

  state() {
    return { scroll: this.scroll.slice(), chr: this.chr.slice(), spr: this.spr.slice(), sprCol: this.sprCol.slice(), pal: this.pal.slice(), monoPal: this.monoPal.slice(),
      r: [this.raster, this.s1x, this.s1y, this.s2x, this.s2y, this.wbaH, this.wbaV, this.wsiH, this.wsiV, this.cOvr, this.blnk, this.poH, this.poV, this.pf, this.bgCol, this.ctl2d, this.ctlInt, this.period, this.mode,
        this.winx, this.winy, this.winw, this.winh, this.sc1x, this.sc1y, this.sc2x, this.sc2y, this.spx, this.spy, this.swap, this.bgc, this.oowc, this.neg] };
  }
  restore(st) {
    this.scroll.set(st.scroll); this.chr.set(st.chr); this.spr.set(st.spr); this.sprCol.set(st.sprCol); this.pal.set(st.pal); this.monoPal.set(st.monoPal);
    [this.raster, this.s1x, this.s1y, this.s2x, this.s2y, this.wbaH, this.wbaV, this.wsiH, this.wsiV, this.cOvr, this.blnk, this.poH, this.poV, this.pf, this.bgCol, this.ctl2d, this.ctlInt, this.period, this.mode,
      this.winx, this.winy, this.winw, this.winh, this.sc1x, this.sc1y, this.sc2x, this.sc2y, this.spx, this.spy, this.swap, this.bgc, this.oowc, this.neg] = st.r;
  }
}
