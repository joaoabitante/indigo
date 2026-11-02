#!/bin/bash
# Prepara /root/indigo-env/roms/svision para o indigo-t7_svision:
#   - cc65demo.sv: tools/t7_svision_demo.c compilado com o cc65 (alvo supervision, crt0 e driver de joystick
#     do próprio cc65, licença zlib). Usa o cc65 já compilado pelo t7_lynx.sh (roms/lynx/cc65) ou o do sistema.
# Os demais programas de teste (bancos, LCD, timer, som) são montados pelo próprio indigo-t7_svision, que os
# grava nesta pasta como .sv para abrir no app. Nenhuma ROM comercial é usada.
set -e
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=/root/indigo-env/roms/svision
mkdir -p "$OUT"
CC65=/root/indigo-env/roms/lynx/cc65
if [ -x "$CC65/bin/cl65" ]; then CL65="$CC65/bin/cl65"; export CC65_HOME="$CC65"
elif command -v cl65 >/dev/null; then CL65=cl65
else echo "sem cc65: rode tools/t7_lynx.sh antes (ou instale o cc65)"; exit 1; fi
TMP=$(mktemp -d)
cp "$REPO/native/tools/t7_svision_demo.c" "$TMP/demo.c"
(cd "$TMP" && "$CL65" -O -t supervision -o cc65demo.sv demo.c)
cp "$TMP/cc65demo.sv" "$OUT/"
rm -rf "$TMP"
ls -l "$OUT"
