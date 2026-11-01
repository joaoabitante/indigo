// Identificação de ROMs de Master System / Game Gear (sem carregar o núcleo): console e código da cópia.

// Game Gear ou Master System pela extensão ou pelo cabeçalho "TMR SEGA" (região 5–7 = Game Gear)
export function smsKind(rom, name) {
  const n = (name || '').toLowerCase();
  if (/\.gg$/.test(n)) return 'gg';
  if (/\.(sms|sg)$/.test(n)) return 'sms';
  for (const base of [0x7FF0, 0x3FF0, 0x1FF0]) {
    if (base + 16 > rom.length) continue;
    let ok = true;
    const tag = 'TMR SEGA';
    for (let k = 0; k < 8; k++) if (rom[base + k] !== tag.charCodeAt(k)) { ok = false; break; }
    if (ok) return (rom[base + 15] >> 4) >= 5 ? 'gg' : 'sms';
  }
  return null;
}

// ROM sem cabeçalho de copiadora e com tamanho potência de 2 (como o núcleo enxerga)
export function smsNormalize(data) {
  if ((data.length & 0x3FFF) === 512) data = data.subarray(512);
  let size = 0x4000;
  while (size < data.length) size <<= 1;
  const rom = new Uint8Array(size).fill(0xFF);
  rom.set(data);
  return rom;
}

// código estável da cópia (o Master System não tem código de jogo no cabeçalho)
export function smsCode(rom) {
  let h = 0x811C9DC5;
  const step = Math.max(1, Math.floor(rom.length / 4096));
  for (let i = 0; i < rom.length; i += step) { h ^= rom[i]; h = Math.imul(h, 0x01000193); }
  return (h >>> 0).toString(16).toUpperCase().padStart(8, '0');
}
