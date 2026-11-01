// Vídeo do Game Boy / Game Boy Color: máquina de modos por ponto (OAM → desenho → HBlank → VBlank),
// interrupções de STAT com o bloqueio do hardware e desenho da linha no fim do modo 3.

// tempos calibrados pelos testes do Mooneye (intr_2_*, hblank_ly_scx_timing)
const OAM_LEN = 84, M3_BASE = 169, SPR_COST = 6;
const S_OAM = 0, S_DRAW = 1, S_HBLANK = 2, S_LINE_END = 3, S_VLINE_END = 4, S_LY153 = 5, S_LINE_END_FRAME = 6, S_HBLANK_IRQ = 7;

// cores padrão do DMG (tons de cinza, trocados pela paleta escolhida no front-end)
const GREY = [0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000];

export class PPU {
  constructor(gb) {
    this.gb = gb;
    this.fb = new Uint32Array(160 * 144);
    this.vram = new Uint8Array(0x4000);
    this.oam = new Uint8Array(0xA0);
    this.bgPal = new Uint8Array(64);
    this.objPal = new Uint8Array(64);
    this.bgRGB = new Uint32Array(32);
    this.objRGB = new Uint32Array(32);
    // paleta DMG: 4 cores para o fundo e 4 para cada paleta de sprite
    this.dmgBg = Uint32Array.from(GREY);
    this.dmgObj0 = Uint32Array.from(GREY);
    this.dmgObj1 = Uint32Array.from(GREY);
    this.lineIdx = new Uint8Array(168);   // índice de cor do fundo (0–3) para prioridade
    this.linePri = new Uint8Array(168);   // atributo de prioridade do fundo (CGB)
    this.objX = new Int16Array(10); this.objN = new Uint8Array(10);
    this.reset();
  }

  reset() {
    this.lcdc = 0x91; this.stat = 0x80; this.scy = 0; this.scx = 0; this.ly = 0; this.lyc = 0;
    this.bgp = 0xFC; this.obp0 = 0xFF; this.obp1 = 0xFF; this.wy = 0; this.wx = 0;
    this.vbk = 0; this.bcps = 0; this.ocps = 0; this.opri = 0;
    this.mode = 2; this.dot = 0; this.nextAt = 80; this.stage = S_DRAW; this.line = 0;
    this.statLine = false; this.winLine = 0; this.wyHit = false; this.hblankIrq = false;
    this.frameDone = false; this.skipFrame = false;
    this.vram.fill(0); this.oam.fill(0);
    for (let k = 0; k < 64; k++) { this.bgPal[k] = 0xFF; this.objPal[k] = 0; }
    for (let k = 0; k < 32; k++) { this.bgRGB[k] = this.rgb(0x7FFF); this.objRGB[k] = this.rgb(0); }
    this.fb.fill(0xFFFFFFFF);
  }

  // a CPU não enxerga a OAM nos modos 2 e 3 nem a VRAM no modo 3
  get oamLocked() { return (this.lcdc & 0x80) !== 0 && this.mode >= 2; }
  get vramLocked() { return (this.lcdc & 0x80) !== 0 && this.mode === 3; }

  get lcdOn() { return (this.lcdc & 0x80) !== 0; }

  rgb(c) {
    const r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000 | (((b << 3) | (b >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((r << 3) | (r >> 2));
  }

  // ------------------------------------------------------------ tempo
  tick(n) {
    if (!(this.lcdc & 0x80)) return;
    this.dot += n;
    while (this.dot >= this.nextAt) this.step();
  }

  step() {
    switch (this.stage) {
      case S_OAM:
        this.setMode(2);
        this.nextAt = OAM_LEN; this.stage = S_DRAW;
        break;
      case S_DRAW: {
        this.setMode(3);
        if (this.ly === this.wy) this.wyHit = true;
        this.nextAt = OAM_LEN + this.mode3Length() - 1;
        this.stage = S_HBLANK_IRQ;
        break;
      }
      case S_HBLANK_IRQ:
        // a interrupção de HBlank sai 1 ponto antes de o STAT mostrar o modo 0
        this.hblankIrq = true; this.updateStat(); this.hblankIrq = false;
        this.nextAt++; this.stage = S_HBLANK;
        break;
      case S_HBLANK:
        if (!this.skipFrame) this.renderLine(this.ly);
        this.setMode(0);
        this.gb.hblankDma();
        this.nextAt = 456; this.stage = S_LINE_END;
        break;
      case S_LINE_END:
        this.dot -= 456;
        this.ly++;
        if (this.ly === 144) {
          this.setMode(1);
          this.gb.requestIrq(0x01);
          this.frameDone = true;
          this.skipFrame = false;
          this.nextAt = 456; this.stage = S_VLINE_END;
        } else { this.nextAt = 0; this.stage = S_OAM; }
        this.updateStat();
        break;
      case S_VLINE_END:
        this.dot -= 456;
        this.ly++;
        if (this.ly === 153) { this.nextAt = 4; this.stage = S_LY153; }
        else this.nextAt = 456;
        this.updateStat();
        break;
      case S_LY153:
        // na linha 153 o LY já volta a 0 depois de poucos pontos
        this.ly = 0;
        this.updateStat();
        this.nextAt = 456; this.stage = S_LINE_END_FRAME;
        break;
      case S_LINE_END_FRAME:
        this.dot -= 456;
        this.ly = 0; this.winLine = 0; this.wyHit = false;
        this.nextAt = 0; this.stage = S_OAM;
        this.updateStat();
        break;
    }
  }

  mode3Length() {
    // 172 pontos + descarte do SCX fino + custo dos sprites (6 cada, mais o atraso de alinhamento com o
    // tile do fundo para o primeiro sprite de cada coluna de 8 pixels; regra da Pan Docs)
    let len = M3_BASE + (this.scx & 7);
    if (this.lcdc & 2) {
      const h = (this.lcdc & 4) ? 16 : 8;
      const xs = [];
      for (let k = 0; k < 40 && xs.length < 10; k++) {
        const y = this.oam[k * 4] - 16;
        if (this.ly >= y && this.ly < y + h) xs.push(this.oam[k * 4 + 1]);
      }
      xs.sort((a, b) => a - b);
      const seen = new Set();
      for (const x of xs) {
        if (x >= 168) continue;
        len += SPR_COST;
        const key = x === 0 ? -1 : (x + this.scx) >> 3;
        if (seen.has(key)) continue;
        seen.add(key);
        len += x === 0 ? 5 : Math.max(0, 5 - ((x + this.scx) & 7));
      }
    }
    return len;
  }

  setMode(m) { this.mode = m; this.updateStat(); }

  // linha de interrupção de STAT: só a borda de subida gera pedido (bloqueio do hardware)
  updateStat() {
    const s = this.stat;
    const lyMatch = this.ly === this.lyc;
    const on = (this.lcdc & 0x80) !== 0;
    const line = on && (
      ((s & 0x40) && lyMatch) ||
      ((s & 0x08) && (this.mode === 0 || this.hblankIrq)) ||
      ((s & 0x20) && (this.mode === 2 || (this.mode === 1 && this.ly === 144 && this.dot < 4))) ||
      ((s & 0x10) && this.mode === 1));
    if (line && !this.statLine) this.gb.requestIrq(0x02);
    this.statLine = !!line;
  }

  // ------------------------------------------------------------ registradores
  readStat() {
    const on = (this.lcdc & 0x80) !== 0;
    return 0x80 | (this.stat & 0x78) | (on && this.ly === this.lyc ? 4 : 0) | (on ? this.mode : 0);
  }
  writeStat(v) { this.stat = (this.stat & 0x87) | (v & 0x78); this.updateStat(); }
  writeLyc(v) { this.lyc = v; this.updateStat(); }

  writeLcdc(v) {
    const was = this.lcdc & 0x80;
    this.lcdc = v;
    if (was && !(v & 0x80)) {
      // tela desligada: LY zera e o modo vai a 0; a imagem fica branca
      this.ly = 0; this.dot = 0; this.mode = 0; this.statLine = false;
      this.winLine = 0; this.wyHit = false;
      this.fb.fill(this.gb.cgb ? 0xFFFFFFFF : this.dmgBg[0]);
      this.frameDone = true;
    } else if (!was && (v & 0x80)) {
      // ao religar, a primeira linha começa sem a busca de OAM e o primeiro quadro não aparece
      this.ly = 0; this.dot = 0; this.mode = 0;
      this.nextAt = 80; this.stage = S_DRAW;
      this.skipFrame = true;
      this.updateStat();
    }
  }

  writeBcpd(v) {
    const i = this.bcps & 0x3F;
    this.bgPal[i] = v;
    const p = i >> 1;
    this.bgRGB[p] = this.rgb(this.bgPal[p * 2] | (this.bgPal[p * 2 + 1] << 8));
    if (this.bcps & 0x80) this.bcps = 0x80 | ((i + 1) & 0x3F);
  }
  writeOcpd(v) {
    const i = this.ocps & 0x3F;
    this.objPal[i] = v;
    const p = i >> 1;
    this.objRGB[p] = this.rgb(this.objPal[p * 2] | (this.objPal[p * 2 + 1] << 8));
    if (this.ocps & 0x80) this.ocps = 0x80 | ((i + 1) & 0x3F);
  }

  // ------------------------------------------------------------ desenho
  renderLine(ly) {
    const cgb = this.gb.cgb, lcdc = this.lcdc;
    const fb = this.fb, row = ly * 160;
    const idx = this.lineIdx, pri = this.linePri;
    const bgOn = cgb || (lcdc & 1);
    const winOn = (lcdc & 0x20) && this.wyHit && this.wx <= 166 && bgOn;
    const winX = this.wx - 7;
    const bgEnd = winOn ? Math.max(0, winX) : 160;

    if (bgOn) {
      const mapBase = (lcdc & 0x08) ? 0x1C00 : 0x1800;
      const y = (ly + this.scy) & 255;
      this.drawTiles(row, 0, bgEnd, mapBase, y, this.scx);
    } else {
      const c = this.dmgBg[0];
      for (let x = 0; x < 160; x++) { fb[row + x] = c; idx[x] = 0; pri[x] = 0; }
    }
    if (winOn) {
      const mapBase = (lcdc & 0x40) ? 0x1C00 : 0x1800;
      this.drawTiles(row, bgEnd, 160, mapBase, this.winLine, -winX);
      this.winLine++;
    }
    if (lcdc & 2) this.drawSprites(ly, row);
  }

  // desenha um trecho [x0, x1) da linha com o mapa de tiles indicado; sx = deslocamento horizontal
  drawTiles(row, x0, x1, mapBase, y, sx) {
    if (x1 <= x0) return;
    const cgb = this.gb.cgb, lcdc = this.lcdc, vram = this.vram, fb = this.fb;
    const idx = this.lineIdx, pri = this.linePri;
    const unsigned = (lcdc & 0x10) !== 0;
    const mapRow = mapBase + ((y >> 3) << 5);
    let x = x0;
    while (x < x1) {
      const px = (x + sx) & 255;
      const col = px >> 3;
      const tile = vram[mapRow + col];
      const attr = cgb ? vram[0x2000 + mapRow + col] : 0;
      let ty = y & 7;
      if (attr & 0x40) ty = 7 - ty;
      const tileAddr = (unsigned ? tile << 4 : 0x1000 + (((tile << 24) >> 24) << 4)) + (ty << 1) + ((attr & 0x08) ? 0x2000 : 0);
      const lo = vram[tileAddr], hi = vram[tileAddr + 1];
      const flipX = (attr & 0x20) !== 0;
      const palBase = (attr & 7) << 2;
      let fx = px & 7;
      for (; fx < 8 && x < x1; fx++, x++) {
        const bit = flipX ? fx : 7 - fx;
        const ci = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        idx[x] = ci;
        pri[x] = attr & 0x80;
        fb[row + x] = cgb ? this.bgRGB[palBase + ci] : this.dmgBg[(this.bgp >> (ci << 1)) & 3];
      }
    }
  }

  drawSprites(ly, row) {
    const cgb = this.gb.cgb, lcdc = this.lcdc, oam = this.oam, vram = this.vram, fb = this.fb;
    const idx = this.lineIdx, pri = this.linePri;
    const h = (lcdc & 4) ? 16 : 8;
    // até 10 sprites por linha, na ordem da OAM
    let n = 0;
    for (let k = 0; k < 40 && n < 10; k++) {
      const y = oam[k * 4] - 16;
      if (ly >= y && ly < y + h) { this.objN[n] = k; this.objX[n] = oam[k * 4 + 1]; n++; }
    }
    // ordem de prioridade: no DMG, menor X vence (empate: menor índice); no CGB, menor índice
    const order = [];
    for (let k = 0; k < n; k++) order.push(k);
    const byX = !cgb || (this.opri & 1);
    if (byX) order.sort((a, b) => (this.objX[a] - this.objX[b]) || (this.objN[a] - this.objN[b]));
    // desenha do menos para o mais prioritário
    const masterPri = !cgb || (lcdc & 1);
    for (let o = order.length - 1; o >= 0; o--) {
      const k = this.objN[order[o]] * 4;
      const sy = oam[k] - 16, sx = oam[k + 1] - 8, attr = oam[k + 3];
      let tile = oam[k + 2];
      if (h === 16) tile &= 0xFE;
      let ty = ly - sy;
      if (attr & 0x40) ty = h - 1 - ty;
      const addr = (tile << 4) + (ty << 1) + (cgb && (attr & 0x08) ? 0x2000 : 0);
      const lo = vram[addr], hi = vram[addr + 1];
      for (let fx = 0; fx < 8; fx++) {
        const x = sx + fx;
        if (x < 0 || x >= 160) continue;
        const bit = (attr & 0x20) ? fx : 7 - fx;
        const ci = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1);
        if (!ci) continue;
        if (masterPri && idx[x] && ((attr & 0x80) || (cgb && pri[x]))) continue;
        if (cgb) fb[row + x] = this.objRGB[((attr & 7) << 2) + ci];
        else {
          const pal = (attr & 0x10) ? this.obp1 : this.obp0;
          fb[row + x] = ((attr & 0x10) ? this.dmgObj1 : this.dmgObj0)[(pal >> (ci << 1)) & 3];
        }
      }
    }
  }

  // ------------------------------------------------------------ save state
  state() {
    return {
      vram: this.vram.slice(), oam: this.oam.slice(), bgPal: this.bgPal.slice(), objPal: this.objPal.slice(),
      r: [this.lcdc, this.stat, this.scy, this.scx, this.ly, this.lyc, this.bgp, this.obp0, this.obp1, this.wy, this.wx,
        this.vbk, this.bcps, this.ocps, this.opri, this.mode, this.dot, this.nextAt, this.stage, this.statLine ? 1 : 0,
        this.winLine, this.wyHit ? 1 : 0],
    };
  }
  restore(s) {
    this.vram.set(s.vram); this.oam.set(s.oam); this.bgPal.set(s.bgPal); this.objPal.set(s.objPal);
    [this.lcdc, this.stat, this.scy, this.scx, this.ly, this.lyc, this.bgp, this.obp0, this.obp1, this.wy, this.wx,
      this.vbk, this.bcps, this.ocps, this.opri, this.mode, this.dot, this.nextAt, this.stage, this.statLine,
      this.winLine, this.wyHit] = s.r;
    this.statLine = !!this.statLine; this.wyHit = !!this.wyHit;
    for (let p = 0; p < 32; p++) {
      this.bgRGB[p] = this.rgb(this.bgPal[p * 2] | (this.bgPal[p * 2 + 1] << 8));
      this.objRGB[p] = this.rgb(this.objPal[p * 2] | (this.objPal[p * 2 + 1] << 8));
    }
  }
}

