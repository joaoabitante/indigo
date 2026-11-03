#!/bin/sh
# Prepara os testes do Saturn em /root/indigo-env/roms/saturn (ou na pasta dada). Nada disto entra no repositório.
#  - bios/teste.bin: a BIOS de teste do Índigo (t7_saturn_bios*.{s,c}, escrita do zero; não é a BIOS da Sega)
#  - disco/homebrew.{cue,iso} + .bin: o homebrew próprio (t7_saturn_jogo.c, t7_saturn_som.s) num CD com IP.BIN,
#    ISO 9660 e uma faixa de áudio, montado pelo t7_saturn_disco.py
# Usa o sh-elf-gcc do 32X (t7_32x_tc.sh) e o binutils do m68k do Ubuntu.
# A BIOS do usuário (nunca no repositório), se existir em /root/indigo-env/bios-joao/saturn/, liga um teste extra.
# Depois: indigo-t7_saturn /root/indigo-env/roms/saturn
set -e
DEST=${1:-/root/indigo-env/roms/saturn}
HERE=$(cd "$(dirname "$0")" && pwd)
TC=${SH_TC:-/root/indigo-env/roms/32x/tc}
[ -x "$TC/bin/sh-elf-gcc" ] || sh "$HERE/t7_32x_tc.sh" "$TC"
command -v m68k-linux-gnu-as >/dev/null || apt-get install -y --no-install-recommends binutils-m68k-linux-gnu >/dev/null
export PATH="$TC/bin:$PATH"
mkdir -p "$DEST/bios" "$DEST/disco" "$DEST/obj"
O="$DEST/obj"
CC="sh-elf-gcc -m2 -mb -O2 -ffreestanding -fno-builtin -nostdlib -Wall -I$HERE"
LIBGCC=$(sh-elf-gcc -m2 -mb -print-libgcc-file-name)

# BIOS de teste (512 KB)
sh-elf-as -o "$O/bios_ini.o" "$HERE/t7_saturn_bios_ini.s"
$CC -c -o "$O/bios.o" "$HERE/t7_saturn_bios.c"
sh-elf-ld -T "$HERE/t7_saturn_bios.ld" -o "$O/bios.elf" "$O/bios_ini.o" "$O/bios.o" "$LIBGCC"
sh-elf-objcopy -O binary -j .text "$O/bios.elf" "$O/bios.bin"
python3 -I -c "import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(d+bytes(512*1024-len(d)))" "$O/bios.bin" "$DEST/bios/teste.bin"

# programa do 68000 (vira um vetor em C)
m68k-linux-gnu-as -m68000 -o "$O/som.o" "$HERE/t7_saturn_som.s"
m68k-linux-gnu-ld -e 0 -Ttext=0 -o "$O/som.elf" "$O/som.o"
m68k-linux-gnu-objcopy -O binary "$O/som.elf" "$O/som.bin"
python3 -I -c "
import sys
d=open(sys.argv[1],'rb').read()
open(sys.argv[2],'w').write('const unsigned char som68k[] = {%s};\nconst unsigned int som68k_tam = %d;\n' % (','.join(str(b) for b in d), len(d)))
" "$O/som.bin" "$O/som68k.c"

# homebrew (1ST_READ.BIN em 0x06004000)
sh-elf-as -o "$O/jogo_ini.o" "$HERE/t7_saturn_jogo_ini.s"
$CC -c -o "$O/jogo.o" "$HERE/t7_saturn_jogo.c"
$CC -c -o "$O/som68k.o" "$O/som68k.c"
sh-elf-ld -T "$HERE/t7_saturn_jogo.ld" -o "$O/jogo.elf" "$O/jogo_ini.o" "$O/jogo.o" "$O/som68k.o" "$LIBGCC"
sh-elf-objcopy -O binary -j .text "$O/jogo.elf" "$O/1st_read.bin"
python3 -I "$HERE/t7_saturn_disco.py" "$O/1st_read.bin" "$DEST/disco"
echo "BIOS de teste que autentica o disco (2.5)" > "$DEST/v2"   # marca para o setup-env.sh refazer as antigas
echo "testes do Saturn prontos em $DEST"
