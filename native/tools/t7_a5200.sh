#!/bin/sh
# Prepara os testes do Atari 5200 em /root/indigo-env/roms/a5200 (usado por indigo-t7_a5200).
# Tudo é feito aqui, sem nada da Atari: a BIOS de teste (tools/t7_a5200_bios.asm) e o cartucho de teste
# (tools/t7_a5200_cart.asm) são montados com o dasm (código aberto, GPL-2). Se houver rede, também extrai a
# BIOS livre do Altirra para o 5200 (Avery Lee, licença permissiva: "Copying and distribution of this file, with
# or without modification, are permitted...") do arquivo src/roms/altirra_5200_os.c do Atari800, para um teste a
# mais; sem ela o teste pula essa parte.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${1:-/root/indigo-env/roms/a5200}
SRC=${SRC7800:-/root/indigo-env/src7800}
mkdir -p "$DEST" "$SRC"
DASM=$(command -v dasm || true)
if [ -z "$DASM" ]; then
  [ -d "$SRC/dasm" ] || git clone --depth 1 https://github.com/dasm-assembler/dasm "$SRC/dasm"
  [ -x "$SRC/dasm/bin/dasm" ] || make -C "$SRC/dasm" >/dev/null
  DASM="$SRC/dasm/bin/dasm"
fi
"$DASM" "$HERE/t7_a5200_bios.asm" -f3 -o"$DEST/bios_teste.rom" >/dev/null
"$DASM" "$HERE/t7_a5200_cart.asm" -f3 -o"$DEST/cart.a52" >/dev/null
if [ ! -f "$DEST/altirra5200.rom" ]; then
  T=$(mktemp -d)
  if curl -sSfL -o "$T/os.c" https://raw.githubusercontent.com/atari800/atari800/master/src/roms/altirra_5200_os.c; then
    python3 -I - "$T/os.c" "$DEST/altirra5200.rom" <<'PY' || true
import re, sys
s = open(sys.argv[1]).read()
s = s[s.index('ROM_altirra_5200_os[0x800]'):]
b = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b', s)[:0x800])
assert len(b) == 0x800
open(sys.argv[2], 'wb').write(b)
PY
  fi
  rm -rf "$T"
fi
ls -l "$DEST"
