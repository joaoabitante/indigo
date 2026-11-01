// Cabeçalho do cartucho de Neo Geo Pocket: assinatura da SNK, endereço de partida, catálogo, modo de cor e nome.
export function isNgpRom(bytes) {
  if (!bytes || bytes.length < 0x40) return false;
  let s = '';
  for (let i = 0; i < 28; i++) s += String.fromCharCode(bytes[i]);
  return s === 'COPYRIGHT BY SNK CORPORATION' || s === ' LICENSED BY SNK CORPORATION';
}

export function ngpInfo(h) {
  const catalog = h[0x20] | (h[0x21] << 8), sub = h[0x22];
  let t = '';
  for (let i = 0; i < 12; i++) { const c = h[0x24 + i]; t += c >= 32 && c < 127 ? String.fromCharCode(c) : ' '; }
  return {
    start: (h[0x1C] | (h[0x1D] << 8) | (h[0x1E] << 16)) >>> 0,
    catalog, sub,
    color: h[0x23] === 0x10,
    title: t.trim(),
    gameCode: 'NGP-' + catalog.toString(16).toUpperCase().padStart(4, '0') + '-' + sub.toString(16).toUpperCase().padStart(2, '0'),
  };
}
