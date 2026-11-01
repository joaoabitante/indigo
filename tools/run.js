// Executa uma ROM sem interface e salva a tela em PNG
// uso: node tools/run.js rom.gba [quadros] [saida.png] [teclas=frame:mask,...]
import fs from 'node:fs';
import zlib from 'node:zlib';
import { GBA } from '../src/gba.js';

function crc32(buf) {
  let c, crc = 0xFFFFFFFF;
  for (let n = 0; n < buf.length; n++) {
    c = (crc ^ buf[n]) & 0xFF;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    crc = (crc >>> 8) ^ c;
  }
  return (crc ^ 0xFFFFFFFF) >>> 0;
}

export function writePng(path, fb, w, h, scale = 2) {
  const W = w * scale, H = h * scale;
  const raw = Buffer.alloc((W * 3 + 1) * H);
  for (let y = 0; y < H; y++) {
    raw[y * (W * 3 + 1)] = 0;
    for (let x = 0; x < W; x++) {
      const p = fb[Math.floor(y / scale) * w + Math.floor(x / scale)];
      const o = y * (W * 3 + 1) + 1 + x * 3;
      raw[o] = p & 0xFF; raw[o + 1] = (p >>> 8) & 0xFF; raw[o + 2] = (p >>> 16) & 0xFF;
    }
  }
  const chunk = (type, data) => {
    const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
    const td = Buffer.concat([Buffer.from(type), data]);
    const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
    return Buffer.concat([len, td, crc]);
  };
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(W, 0); ihdr.writeUInt32BE(H, 4);
  ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
  const png = Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0)),
  ]);
  fs.writeFileSync(path, png);
}

if (process.argv[1] && process.argv[1].endsWith('run.js')) {
  const [, , romPath, framesArg, outArg, keysArg] = process.argv;
  const gba = new GBA();
  gba.loadRom(new Uint8Array(fs.readFileSync(romPath)));
  const frames = parseInt(framesArg || '120', 10);
  const keyEvents = (keysArg || '').split(',').filter(Boolean).map(s => s.split(':').map(Number));
  const t0 = Date.now();
  let mask = 0;
  for (let f = 0; f < frames; f++) {
    for (const [fr, m] of keyEvents) if (fr === f) mask = m;
    gba.setKeys(mask);
    gba.runFrame();
  }
  const dt = (Date.now() - t0) / 1000;
  console.log(`${gba.title} [${gba.gameCode}] ${frames} quadros em ${dt.toFixed(2)}s (${(frames / dt).toFixed(1)} fps)`);
  console.log('PC=' + (gba.cpu.nextPC >>> 0).toString(16), 'T=' + gba.cpu.t, 'mode=' + gba.cpu.mode.toString(16),
    'r=' + Array.from(gba.cpu.r).map(v => (v >>> 0).toString(16)).join(' '));
  writePng(outArg || 'out.png', gba.ppu.fb, 240, 160);
}
