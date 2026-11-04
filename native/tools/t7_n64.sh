#!/bin/bash
# Prepara os testes do núcleo Nintendo 64 em /root/indigo-env/roms/n64 (ou na pasta dada): monta os programas de
# teste escritos para o Índigo (tools/t7_n64_*.s: CPU, RSP, RDP, som, controle e saves) com o binutils do MIPS
# (binutils-mipsel-linux-gnu, usado em big-endian) e põe o cabeçalho com tools/t7_n64_rom.py. Não usa ROM, BIOS
# nem código de partida de ninguém (a partida do Índigo é HLE).
set -e
TOOLS=$(cd "$(dirname "$0")" && pwd)
D=${1:-/root/indigo-env/roms/n64}
mkdir -p "$D"
cd "$D"
AS="mipsel-linux-gnu-as -EB -march=vr4300 -mabi=o64 -mno-shared -call_nonpic -I $TOOLS -I $D"
LD="mipsel-linux-gnu-ld -EB -m elf32btsmip"
# microcódigo do RSP (endereço 0x04001000 = IMEM)
ucode() {
  $AS -o "$1.o" "$TOOLS/t7_n64_$1.s"
  $LD -Ttext=0x04001000 -e 0x04001000 -o "$1.elf" "$1.o"
  mipsel-linux-gnu-objcopy -O binary -j .text "$1.elf" "$1.bin"
  rm -f "$1.o" "$1.elf"
}
# programa da CPU (carregado em 0x80000400) e o cartucho
prog() {
  $AS -o "$1.o" "$TOOLS/t7_n64_$1.s"
  $LD -Ttext=0x80000400 -e 0x80000400 -o "$1.elf" "$1.o"
  mipsel-linux-gnu-objcopy -O binary -j .text "$1.elf" "$1.bin"
  out=${4:-$1}
  python3 "$TOOLS/t7_n64_rom.py" "$1.bin" "$out.z64" "$2" "$3"
  rm -f "$1.o" "$1.elf" "$1.bin"
  echo "cartucho de teste pronto: $D/$out.z64"
}
[ -f "$TOOLS/t7_n64_rspu.s" ] && ucode rspu
[ -f "$TOOLS/t7_n64_gfxu.s" ] && ucode gfxu
prog cpu "INDIGO TESTE CPU" 0
[ -f "$TOOLS/t7_n64_rsp.s" ] && prog rsp "INDIGO TESTE RSP" 0
[ -f "$TOOLS/t7_n64_gfx.s" ] && prog gfx "INDIGO TESTE RDP" 1
[ -f "$TOOLS/t7_n64_io.s" ] && prog io "INDIGO TESTE IO" 1
prog save "INDIGO TESTE SRAM" 3 sram
prog save "INDIGO TESTE FLASH" 5 flash
rm -f rspu.bin gfxu.bin
exit 0
