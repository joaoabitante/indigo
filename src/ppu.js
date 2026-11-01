// PPU — renderizador por scanline (modos 0-5, sprites, janelas, efeitos de cor, mosaico)

const OBJ_W = [8, 16, 32, 64, 16, 32, 32, 64, 8, 8, 16, 32];
const OBJ_H = [8, 16, 32, 64, 8, 8, 16, 32, 16, 32, 32, 64];

export class PPU {
  constructor(gba) {
    this.gba = gba;
    this.fb = new Uint32Array(240 * 160);
    this.lut = new Uint32Array(32768);
    for (let c = 0; c < 32768; c++) {
      const r = c & 31, g = (c >>> 5) & 31, b = (c >>> 10) & 31;
      const R = (r << 3) | (r >>> 2), G = (g << 3) | (g >>> 2), B = (b << 3) | (b >>> 2);
      this.lut[c] = (0xFF000000 | (B << 16) | (G << 8) | R) >>> 0;
    }
    this.bgLine = [new Int32Array(240), new Int32Array(240), new Int32Array(240), new Int32Array(240)];
    this.objColor = new Int32Array(240);
    this.objPrio = new Uint8Array(240);
    this.objAlpha = new Uint8Array(240);
    this.objWin = new Uint8Array(240);
    this.refX = new Int32Array(2);
    this.refY = new Int32Array(2);
    this.order = new Int32Array(4);
    this.frameReady = false;
  }

  get regs() { return this.gba.io.regs; }

  latchAffine(a) {
    const r = this.gba.io.regs;
    const bg = a >= 0x38 ? 1 : 0;
    const base = bg ? 0x38 : 0x28;
    this.refX[bg] = ((r[base >>> 1] | (r[(base + 2) >>> 1] << 16)) << 4) >> 4;
    this.refY[bg] = ((r[(base + 4) >>> 1] | (r[(base + 6) >>> 1] << 16)) << 4) >> 4;
  }

  vblankLatch() {
    this.latchAffine(0x28);
    this.latchAffine(0x38);
  }

  renderLine(y) {
    const r = this.gba.io.regs;
    const dispcnt = r[0];
    const fb = this.fb;
    const off = y * 240;
    if (dispcnt & 0x80) {
      for (let x = 0; x < 240; x++) fb[off + x] = 0xFFFFFFFF;
      this.advanceAffine();
      return;
    }
    const mode = dispcnt & 7;
    let bgMask = 0;
    switch (mode) {
      case 0: bgMask = 0xF; break;
      case 1: bgMask = 0x7; break;
      case 2: bgMask = 0xC; break;
      case 3: case 4: case 5: bgMask = 0x4; break;
      default: bgMask = 0; break;
    }
    bgMask &= (dispcnt >>> 8) & 0xF;

    const mosaic = r[0x4C >>> 1];
    for (let bg = 0; bg < 4; bg++) {
      if (!(bgMask & (1 << bg))) continue;
      const out = this.bgLine[bg];
      if (mode === 0 || (mode === 1 && bg < 2)) this.renderText(bg, y, out);
      else if (mode <= 2) this.renderAffine(bg, out);
      else this.renderBitmap(mode, dispcnt, out);
      const cnt = r[(8 + bg * 2) >>> 1];
      if ((cnt & 0x40) && (mosaic & 0xF)) {
        const mw = (mosaic & 0xF) + 1;
        for (let x = 0; x < 240; x++) out[x] = out[x - (x % mw)];
      }
    }

    const objOn = (dispcnt & 0x1000) !== 0;
    if (objOn || (dispcnt & 0x8000)) this.renderObjs(y, dispcnt, mode);

    this.compose(y, dispcnt, bgMask, objOn);
    this.advanceAffine();
  }

  advanceAffine() {
    const r = this.gba.io.regs;
    this.refX[0] = (this.refX[0] + ((r[0x22 >>> 1] << 16) >> 16)) | 0;
    this.refY[0] = (this.refY[0] + ((r[0x26 >>> 1] << 16) >> 16)) | 0;
    this.refX[1] = (this.refX[1] + ((r[0x32 >>> 1] << 16) >> 16)) | 0;
    this.refY[1] = (this.refY[1] + ((r[0x36 >>> 1] << 16) >> 16)) | 0;
  }

  renderText(bg, y, out) {
    const r = this.gba.io.regs;
    const bus = this.gba.bus;
    const vram = bus.vram, vram16 = bus.vram16, pal16 = bus.pal16;
    const cnt = r[(8 + bg * 2) >>> 1];
    const hofs = r[(0x10 + bg * 4) >>> 1] & 0x1FF;
    const vofs = r[(0x12 + bg * 4) >>> 1] & 0x1FF;
    const charBase = ((cnt >>> 2) & 3) * 0x4000;
    const screenBase = ((cnt >>> 8) & 31) * 0x800;
    const c256 = (cnt & 0x80) !== 0;
    const size = cnt >>> 14;
    const w = (size & 1) ? 512 : 256, h = (size & 2) ? 512 : 256;
    let yy = y;
    const mosaic = r[0x4C >>> 1];
    if ((cnt & 0x40) && (mosaic & 0xF0)) { const mh = ((mosaic >>> 4) & 0xF) + 1; yy = y - (y % mh); }
    const py = (yy + vofs) & (h - 1);
    const sby = py >>> 8;
    const ty = py & 7;
    const rowOff = ((py & 255) >>> 3) * 64;
    const blocksPerRow = w >>> 8;
    let x = 0;
    while (x < 240) {
      const px = (x + hofs) & (w - 1);
      const block = (px >>> 8) + sby * blocksPerRow;
      const entry = vram16[(screenBase + block * 0x800 + rowOff + ((px & 255) >>> 3) * 2) >>> 1];
      const tile = entry & 0x3FF;
      const tyy = (entry & 0x800) ? 7 - ty : ty;
      const hflip = (entry & 0x400) !== 0;
      // percorre os pixels restantes deste tile
      let tx = px & 7;
      for (; tx < 8 && x < 240; tx++, x++) {
        const sx = hflip ? 7 - tx : tx;
        let color = -1;
        if (c256) {
          const addr = charBase + tile * 64 + tyy * 8 + sx;
          if (addr < 0x10000) { const idx = vram[addr]; if (idx) color = pal16[idx]; }
        } else {
          const addr = charBase + tile * 32 + tyy * 4 + (sx >>> 1);
          if (addr < 0x10000) {
            const b = vram[addr];
            const idx = (sx & 1) ? (b >>> 4) : (b & 15);
            if (idx) color = pal16[((entry >>> 12) << 4) + idx];
          }
        }
        out[x] = color;
      }
    }
  }

  renderAffine(bg, out) {
    const r = this.gba.io.regs;
    const bus = this.gba.bus;
    const vram = bus.vram, pal16 = bus.pal16;
    const cnt = r[(8 + bg * 2) >>> 1];
    const charBase = ((cnt >>> 2) & 3) * 0x4000;
    const screenBase = ((cnt >>> 8) & 31) * 0x800;
    const size = 128 << (cnt >>> 14);
    const wrap = (cnt & 0x2000) !== 0;
    const k = bg - 2;
    const base = k ? 0x30 : 0x20;
    const pa = (r[base >>> 1] << 16) >> 16, pc = (r[(base + 4) >>> 1] << 16) >> 16;
    let fx = this.refX[k], fy = this.refY[k];
    const tilesPerRow = size >>> 3;
    for (let x = 0; x < 240; x++, fx += pa, fy += pc) {
      let tx = fx >> 8, ty = fy >> 8;
      if (wrap) { tx &= size - 1; ty &= size - 1; }
      else if (tx < 0 || ty < 0 || tx >= size || ty >= size) { out[x] = -1; continue; }
      const tile = vram[screenBase + (ty >>> 3) * tilesPerRow + (tx >>> 3)];
      const idx = vram[charBase + tile * 64 + (ty & 7) * 8 + (tx & 7)];
      out[x] = idx ? pal16[idx] : -1;
    }
  }

  renderBitmap(mode, dispcnt, out) {
    const r = this.gba.io.regs;
    const bus = this.gba.bus;
    const pa = (r[0x20 >>> 1] << 16) >> 16, pc = (r[0x24 >>> 1] << 16) >> 16;
    let fx = this.refX[0], fy = this.refY[0];
    const frame = (dispcnt & 0x10) ? 0xA000 : 0;
    if (mode === 3) {
      const v16 = bus.vram16;
      for (let x = 0; x < 240; x++, fx += pa, fy += pc) {
        const tx = fx >> 8, ty = fy >> 8;
        out[x] = (tx < 0 || ty < 0 || tx >= 240 || ty >= 160) ? -1 : (v16[ty * 240 + tx] & 0x7FFF);
      }
    } else if (mode === 4) {
      const v = bus.vram, pal16 = bus.pal16;
      for (let x = 0; x < 240; x++, fx += pa, fy += pc) {
        const tx = fx >> 8, ty = fy >> 8;
        if (tx < 0 || ty < 0 || tx >= 240 || ty >= 160) { out[x] = -1; continue; }
        const idx = v[frame + ty * 240 + tx];
        out[x] = idx ? pal16[idx] : -1;
      }
    } else {
      const v16 = bus.vram16, b = frame >>> 1;
      for (let x = 0; x < 240; x++, fx += pa, fy += pc) {
        const tx = fx >> 8, ty = fy >> 8;
        out[x] = (tx < 0 || ty < 0 || tx >= 160 || ty >= 128) ? -1 : (v16[b + ty * 160 + tx] & 0x7FFF);
      }
    }
  }

  renderObjs(y, dispcnt, mode) {
    const bus = this.gba.bus;
    const oam16 = bus.oam16, vram = bus.vram, pal16 = bus.pal16;
    const objColor = this.objColor, objPrio = this.objPrio, objAlpha = this.objAlpha, objWin = this.objWin;
    objColor.fill(-1); objPrio.fill(4); objAlpha.fill(0); objWin.fill(0);
    const oneD = (dispcnt & 0x40) !== 0;
    const bitmap = mode >= 3;
    const mosaic = this.gba.io.regs[0x4C >>> 1];
    const mosH = ((mosaic >>> 12) & 0xF) + 1;
    for (let i = 0; i < 128; i++) {
      const a0 = oam16[i * 4], a1 = oam16[i * 4 + 1], a2 = oam16[i * 4 + 2];
      const affine = (a0 & 0x100) !== 0;
      if (!affine && (a0 & 0x200)) continue;
      const objMode = (a0 >>> 10) & 3;
      if (objMode === 3) continue;
      const shape = a0 >>> 14;
      if (shape === 3) continue;
      const sz = a1 >>> 14;
      const w = OBJ_W[shape * 4 + sz], h = OBJ_H[shape * 4 + sz];
      let bw = w, bh = h;
      if (affine && (a0 & 0x200)) { bw <<= 1; bh <<= 1; }
      const sy = a0 & 0xFF;
      let dy = (y - sy) & 0xFF;
      if (dy >= bh) continue;
      if (a0 & 0x1000) { dy = dy - (dy % mosH); }
      let sx = a1 & 0x1FF;
      if (sx >= 240) sx -= 512;
      if (sx + bw <= 0) continue;
      const tileBase = a2 & 0x3FF;
      if (bitmap && tileBase < 512) continue;
      const prio = (a2 >>> 10) & 3;
      const c256 = (a0 & 0x2000) !== 0;
      const palBank = 256 + ((a2 >>> 12) << 4);
      const rowStride = oneD ? (c256 ? (w >>> 3) * 2 : (w >>> 3)) : 32;
      const isWin = objMode === 2, semi = objMode === 1 ? 1 : 0;
      let pa = 256, pb = 0, pc = 0, pd = 256;
      if (affine) {
        const p = ((a1 >>> 9) & 31) * 16;
        pa = (oam16[p + 3] << 16) >> 16; pb = (oam16[p + 7] << 16) >> 16;
        pc = (oam16[p + 11] << 16) >> 16; pd = (oam16[p + 15] << 16) >> 16;
      }
      const hw = bw >>> 1, hh = bh >>> 1;
      const iy = dy - hh;
      const hflip = (a1 & 0x1000) !== 0, vflip = (a1 & 0x2000) !== 0;
      for (let bx = 0; bx < bw; bx++) {
        const x = sx + bx;
        if (x < 0) continue;
        if (x >= 240) break;
        let tx, ty;
        if (affine) {
          const ix = bx - hw;
          tx = ((pa * ix + pb * iy) >> 8) + (w >>> 1);
          ty = ((pc * ix + pd * iy) >> 8) + (h >>> 1);
          if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
        } else {
          tx = hflip ? w - 1 - bx : bx;
          ty = vflip ? h - 1 - dy : dy;
        }
        let color = -1;
        if (c256) {
          const tn = (tileBase & ~1) + (ty >>> 3) * rowStride + (tx >>> 3) * 2;
          const idx = vram[0x10000 + ((tn & 0x3FF) << 5) + (ty & 7) * 8 + (tx & 7)];
          if (idx) color = pal16[256 + idx];
        } else {
          const tn = tileBase + (ty >>> 3) * rowStride + (tx >>> 3);
          const b = vram[0x10000 + ((tn & 0x3FF) << 5) + (ty & 7) * 4 + ((tx & 7) >>> 1)];
          const idx = (tx & 1) ? (b >>> 4) : (b & 15);
          if (idx) color = pal16[palBank + idx];
        }
        if (color < 0) continue;
        if (isWin) { objWin[x] = 1; continue; }
        if (objColor[x] < 0 || prio < objPrio[x]) {
          objColor[x] = color; objPrio[x] = prio; objAlpha[x] = semi;
        }
      }
    }
  }

  compose(y, dispcnt, bgMask, objOn) {
    const r = this.gba.io.regs;
    const pal16 = this.gba.bus.pal16;
    const fb = this.fb, lut = this.lut, off = y * 240;
    const backdrop = pal16[0] & 0x7FFF;
    // ordem de prioridade dos BGs
    const prio = [0, 0, 0, 0];
    let nbg = 0;
    for (let p = 0; p < 4; p++) {
      for (let bg = 0; bg < 4; bg++) {
        if (!(bgMask & (1 << bg))) continue;
        const bp = r[(8 + bg * 2) >>> 1] & 3;
        prio[bg] = bp;
        if (bp === p) this.order[nbg++] = bg;
      }
    }
    const order = this.order;
    const bldcnt = r[0x50 >>> 1];
    const effect = (bldcnt >>> 6) & 3;
    const t1 = bldcnt & 0x3F, t2 = (bldcnt >>> 8) & 0x3F;
    const bldalpha = r[0x52 >>> 1];
    const eva = Math.min(16, bldalpha & 0x1F), evb = Math.min(16, (bldalpha >>> 8) & 0x1F);
    const evy = Math.min(16, r[0x54 >>> 1] & 0x1F);

    const winAny = (dispcnt & 0xE000) !== 0;
    const winin = r[0x48 >>> 1], winout = r[0x4A >>> 1];
    let w0y = false, w1y = false;
    let w0x1 = 0, w0x2 = 0, w1x1 = 0, w1x2 = 0;
    if (dispcnt & 0x2000) {
      const h = r[0x40 >>> 1], v = r[0x44 >>> 1];
      w0x1 = h >>> 8; w0x2 = h & 0xFF; if (w0x2 > 240 || (w0x1 > w0x2 && w0x2 === 0)) w0x2 = 240;
      let y1 = v >>> 8, y2 = v & 0xFF; if (y2 > 160) y2 = 160;
      w0y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    }
    if (dispcnt & 0x4000) {
      const h = r[0x42 >>> 1], v = r[0x46 >>> 1];
      w1x1 = h >>> 8; w1x2 = h & 0xFF; if (w1x2 > 240 || (w1x1 > w1x2 && w1x2 === 0)) w1x2 = 240;
      let y1 = v >>> 8, y2 = v & 0xFF; if (y2 > 160) y2 = 160;
      w1y = y1 <= y2 ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    }
    const objWinOn = (dispcnt & 0x8000) !== 0 && objOn;
    const bgl = this.bgLine;
    const objColor = this.objColor, objPrio = this.objPrio, objAlpha = this.objAlpha, objWin = this.objWin;

    for (let x = 0; x < 240; x++) {
      let wm = 0x3F;
      if (winAny) {
        if (w0y && (w0x1 <= w0x2 ? (x >= w0x1 && x < w0x2) : (x >= w0x1 || x < w0x2))) wm = winin & 0x3F;
        else if (w1y && (w1x1 <= w1x2 ? (x >= w1x1 && x < w1x2) : (x >= w1x1 || x < w1x2))) wm = (winin >>> 8) & 0x3F;
        else if (objWinOn && objWin[x]) wm = (winout >>> 8) & 0x3F;
        else wm = winout & 0x3F;
      }
      let c1 = backdrop, l1 = 5, c2 = backdrop, l2 = 5, found = 0;
      const oc = (objOn && (wm & 0x10)) ? objColor[x] : -1;
      let objDone = oc < 0;
      const op = objPrio[x];
      for (let k = 0; k < nbg && found < 2; k++) {
        const bg = order[k];
        if (!objDone && op <= prio[bg]) {
          objDone = true;
          if (found === 0) { c1 = oc; l1 = 4; } else { c2 = oc; l2 = 4; }
          found++;
          if (found === 2) break;
        }
        if (!(wm & (1 << bg))) continue;
        const c = bgl[bg][x];
        if (c < 0) continue;
        if (found === 0) { c1 = c; l1 = bg; } else { c2 = c; l2 = bg; }
        found++;
      }
      if (!objDone && found < 2) {
        if (found === 0) { c1 = oc; l1 = 4; } else { c2 = oc; l2 = 4; }
      }
      let out = c1;
      if (l1 === 4 && objAlpha[x] && (t2 & (1 << l2))) {
        out = blend(c1, c2, eva, evb);
      } else if ((wm & 0x20) && effect && (t1 & (1 << l1))) {
        if (effect === 1) { if (t2 & (1 << l2)) out = blend(c1, c2, eva, evb); }
        else if (effect === 2) out = brighten(c1, evy);
        else out = darken(c1, evy);
      }
      fb[off + x] = lut[out & 0x7FFF];
    }
  }
}

function blend(a, b, eva, evb) {
  let r = ((a & 31) * eva + (b & 31) * evb) >> 4;
  let g = (((a >>> 5) & 31) * eva + ((b >>> 5) & 31) * evb) >> 4;
  let bl = (((a >>> 10) & 31) * eva + ((b >>> 10) & 31) * evb) >> 4;
  if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31;
  return r | (g << 5) | (bl << 10);
}
function brighten(a, evy) {
  let r = a & 31, g = (a >>> 5) & 31, b = (a >>> 10) & 31;
  r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4;
  return r | (g << 5) | (b << 10);
}
function darken(a, evy) {
  let r = a & 31, g = (a >>> 5) & 31, b = (a >>> 10) & 31;
  r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4;
  return r | (g << 5) | (b << 10);
}
