#!/usr/bin/env bash
# Prepara os testes do controlador de CD-ROM e do MDEC do PS1 em /root/indigo-env/roms/ps1cd (ou na pasta dada):
#  - disco/: discos sintéticos (t9_cdmdec_disco.py): dados Mode 2 com licença, setores XA-ADPCM em quatro
#    formatos, faixas de áudio com pregap; discos europeu, japonês e de música
#  - roteiros/: os roteiros de comandos (t9_cdmdec_roteiros/*.txt)
#  - ref/beetle: Beetle PSX (Mednafen, GPL) numa versão fixa, só mdec.c, cdc.c e o que eles incluem — usado só
#    como régua, compilado aqui fora do repositório junto com t9_cdmdec_ref.c (nada dele entra no Índigo)
#  - ref/mdec.bin: fluxos aleatórios do MDEC e a saída do Beetle; ref/cd/<roteiro>.log (+ .a<n> do som)
#  - ps1-tests/: os testes do MDEC e do CD-ROM do ps1-tests (JaCzekanski) numa versão fixa, com os resultados
#    tirados de um console de verdade (psx.log e vram-*.png, estes convertidos para 15 bits em *.raw)
# Depois: indigo-t9_cdmdec /root/indigo-env/roms/ps1cd
set -euo pipefail
DEST=${1:-/root/indigo-env/roms/ps1cd}
HERE=$(cd "$(dirname "$0")" && pwd)
BEETLE_COMMIT=53246dc2d00f3c00cc56baa1061902da7a6651e4
CASES=${T9_MDEC_CASES:-2000}
mkdir -p "$DEST/ref/bin" "$DEST/ref/cd" "$DEST/roteiros"
cd "$DEST"

[ -f disco/teste.bin ] || python3 -I "$HERE/t9_cdmdec_disco.py" disco
cp "$HERE"/t9_cdmdec_roteiros/*.txt roteiros/

B=ref/beetle
if [ ! -f "$B/mednafen/psx/cdc.c" ]; then
  rm -rf "$B"
  git init -q "$B"
  git -C "$B" remote add origin https://github.com/libretro/beetle-psx-libretro
  git -C "$B" config core.sparseCheckout true
  printf '/mednafen/psx/\n/mednafen/cdrom/\n/mednafen/*.h\n/libretro-common/include/\n/osd_message.h\n' > "$B/.git/info/sparse-checkout"
  git -C "$B" fetch -q --depth 1 --filter=blob:none origin "$BEETLE_COMMIT" || git -C "$B" fetch -q --depth 1 --filter=blob:none origin HEAD
  git -C "$B" -c advice.detachedHead=false checkout -q FETCH_HEAD
fi

if [ ! -x ref/bin/t9_cdmdec_ref ] || [ "$HERE/t9_cdmdec_ref.c" -nt ref/bin/t9_cdmdec_ref ] || [ "$HERE/t9_cdmdec_driver.h" -nt ref/bin/t9_cdmdec_ref ]; then
  gcc -std=gnu99 -O2 -w -I"$B" -I"$B/mednafen" -I"$B/libretro-common/include" -I"$HERE" -o ref/bin/t9_cdmdec_ref \
    "$HERE/t9_cdmdec_ref.c" "$B/mednafen/psx/cdc.c" "$B/mednafen/psx/mdec.c" "$B/mednafen/cdrom/CDUtility.c" -lm
  rm -f ref/mdec.bin ref/cd/*
fi

[ -f ref/mdec.bin ] || ref/bin/t9_cdmdec_ref mdec ref/mdec.bin "$CASES" 20270311

T=ps1-tests
if [ ! -f "$T/mdec/frame/sunset.mdec" ]; then
  rm -rf "$T"
  git init -q "$T"
  git -C "$T" remote add origin https://github.com/JaCzekanski/ps1-tests
  git -C "$T" config core.sparseCheckout true
  printf '/mdec/4bit/\n/mdec/8bit/\n/mdec/frame/\n/mdec/step-by-step-log/\n/common/mdec/\n/cdrom/\n' > "$T/.git/info/sparse-checkout"
  git -C "$T" fetch -q --depth 1 --filter=blob:none origin f727802fead11f1daa7549285548392ef87749cb || git -C "$T" fetch -q --depth 1 --filter=blob:none origin HEAD
  git -C "$T" -c advice.detachedHead=false checkout -q FETCH_HEAD
fi
for v in 15bit 24bit; do
  [ -f "$T/mdec/frame/vram-$v.raw" ] || python3 -I - "$T/mdec/frame/vram-$v.png" "$T/mdec/frame/vram-$v.raw" <<'PY'
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
w, h, bd, ct = struct.unpack('>IIBB', d[16:26])
assert bd == 8 and ct == 2, 'só PNG RGB de 8 bits'
p, idat = 8, b''
while p < len(d):
    n, t = struct.unpack('>I4s', d[p:p + 8])
    if t == b'IDAT':
        idat += d[p + 8:p + 8 + n]
    p += 12 + n
raw = zlib.decompress(idat)
st, prev, out = w * 3, bytearray(w * 3), bytearray()
for y in range(h):
    f = raw[y * (st + 1)]
    line = bytearray(raw[y * (st + 1) + 1:(y + 1) * (st + 1)])
    for i in range(st):
        a = line[i - 3] if i >= 3 else 0
        b = prev[i]
        c = prev[i - 3] if i >= 3 else 0
        if f == 1: line[i] = (line[i] + a) & 255
        elif f == 2: line[i] = (line[i] + b) & 255
        elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
        elif f == 4:
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            line[i] = (line[i] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
    for x in range(w):
        r, g, bl = line[x * 3:x * 3 + 3]
        out += struct.pack('<H', (r >> 3) | (g >> 3) << 5 | (bl >> 3) << 10)
    prev = line
open(sys.argv[2], 'wb').write(out)
PY
done

for s in roteiros/*.txt; do
  n=$(basename "$s" .txt)
  if [ ! -f "ref/cd/$n.log" ] || [ "$s" -nt "ref/cd/$n.log" ]; then
    d=$(sed -n 's/^# disco: //p' "$s")
    rm -f "ref/cd/$n.log" ref/cd/"$n".log.a* ref/cd/"$n".log.x*
    ref/bin/t9_cdmdec_ref cd disco "$d" "$s" "ref/cd/$n.log"
  fi
done
echo "testes do CD-ROM e do MDEC do PS1 prontos em $DEST"
