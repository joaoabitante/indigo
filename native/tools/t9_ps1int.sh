#!/bin/sh
# Prepara os testes do PlayStation inteiro em /root/indigo-env/roms/ps1int (ou na pasta dada). Nada disto entra
# no repositório. Precisa do que o t9_ps1.sh montou em ../ps1 (bibliotecas do PSn00bSDK, ps1-tests, Beetle PSX).
#  - build/: variantes dos testes do MDEC (frame 15/24 bits com e sem DMA, movie) e o programa do disco de teste
#    (t9_ps1int_build.py);
#  - disco/: o disco de teste (t9_ps1int_disco.py): teste.cue/.bin e sem_licenca.cue/.bin;
#  - ref/beetle-parado.ppm: o quadro do disco de teste no Beetle PSX com a BIOS do João (cubos parados enquanto a
#    faixa de CD-DA toca; quadro 1950 do console, que cai no meio dessa parte do programa).
# Depois: indigo-t9_ps1int /root/indigo-env/roms/ps1int
set -e
DEST=${1:-/root/indigo-env/roms/ps1int}
HERE=$(cd "$(dirname "$0")" && pwd)
PS1=$(cd "$DEST/.." && pwd)/ps1
BIOS=${PS1_BIOS:-/root/indigo-env/bios-joao/scph1001.bin}
[ -d "$PS1/build/lib" ] || sh "$HERE/t9_ps1.sh" "$PS1"
mkdir -p "$DEST/build" "$DEST/disco" "$DEST/ref"
cd "$DEST"
python3 -I "$HERE/t9_ps1int_build.py" "$PS1" "$DEST/build"
python3 -I "$HERE/t9_ps1int_disco.py" "$DEST/build/jogo.exe" "$DEST/disco"
if [ -f "$BIOS" ]; then
  [ -x "$PS1/ref/bin/t9_ps1_ref" ] || gcc -O2 -o "$PS1/ref/bin/t9_ps1_ref" "$HERE/t9_ps1_ref.c" -ldl
  "$PS1/ref/bin/t9_ps1_ref" "$PS1/ref/beetle/mednafen_psx_libretro.so" "$PS1/ref/sys" disco/teste.cue 1950 ref/beetle-parado.ppm 2>/dev/null
else
  echo "sem a BIOS em $BIOS: o quadro de referência do Beetle fica para depois"
fi
echo "testes do PlayStation inteiro prontos em $DEST"
