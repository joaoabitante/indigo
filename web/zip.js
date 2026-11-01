// Leitor mínimo de .zip: acha a ROM dentro do arquivo e descompacta com o DecompressionStream do navegador.

const ROM_EXT = /\.(gba|agb|gbc|gb|sgb|cgb|gg|sms|sg|ngp|ngc|npc)$/i;

export function isZip(bytes) { return bytes.length > 22 && bytes[0] === 0x50 && bytes[1] === 0x4B && bytes[2] === 0x03 && bytes[3] === 0x04; }

export async function extractRom(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  // fim do diretório central
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 65557); i--) if (dv.getUint32(i, true) === 0x06054b50) { eocd = i; break; }
  if (eocd < 0) throw new Error('Arquivo .zip danificado.');
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const entries = [];
  for (let n = 0; n < count && p + 46 <= bytes.length; n++) {
    if (dv.getUint32(p, true) !== 0x02014b50) break;
    const method = dv.getUint16(p + 10, true), csize = dv.getUint32(p + 20, true), size = dv.getUint32(p + 24, true);
    const nlen = dv.getUint16(p + 28, true), xlen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = new TextDecoder().decode(bytes.subarray(p + 46, p + 46 + nlen));
    entries.push({ name, method, csize, size, local });
    p += 46 + nlen + xlen + clen;
  }
  const pick = entries.filter(e => ROM_EXT.test(e.name)).sort((a, b) => b.size - a.size)[0];
  if (!pick) throw new Error('Não há ROM de console portátil dentro deste .zip.');
  const lh = pick.local;
  const start = lh + 30 + dv.getUint16(lh + 26, true) + dv.getUint16(lh + 28, true);
  const raw = bytes.subarray(start, start + pick.csize);
  let data;
  if (pick.method === 0) data = raw.slice();
  else if (pick.method === 8) {
    if (typeof DecompressionStream === 'undefined') throw new Error('Este navegador não descompacta .zip. Extraia a ROM antes.');
    const stream = new Blob([raw]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    data = new Uint8Array(await new Response(stream).arrayBuffer());
  } else throw new Error('Compressão do .zip não suportada.');
  return { name: pick.name.split('/').pop(), data };
}
