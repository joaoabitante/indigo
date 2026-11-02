#!/bin/bash
# Prepara os testes do núcleo Atari Jaguar em /root/indigo-env/roms/jaguar (ou na pasta dada): monta os cartuchos
# de teste escritos para o Índigo (t7_jaguar_rom.s e t7_jaguar_rom2.s, 68000 + programas do GPU/DSP montados com
# macros) com o binutils do m68k-linux-gnu. Não usa ROM nem BIOS de ninguém. Precisa de binutils-m68k-linux-gnu.
set -e
TOOLS=$(cd "$(dirname "$0")" && pwd)
D=${1:-/root/indigo-env/roms/jaguar}
mkdir -p "$D"
cd "$D"
for n in "" 2; do
  [ -f "$TOOLS/t7_jaguar_rom$n.s" ] || continue
  m68k-linux-gnu-as -m68000 -I "$TOOLS" -o teste$n.o "$TOOLS/t7_jaguar_rom$n.s"
  m68k-linux-gnu-ld -Ttext=0x800000 -e 0x802000 -o teste$n.elf teste$n.o
  m68k-linux-gnu-objcopy -O binary teste$n.elf teste$n.j64
  rm -f teste$n.o teste$n.elf
  echo "cartucho de teste pronto: $D/teste$n.j64"
done
