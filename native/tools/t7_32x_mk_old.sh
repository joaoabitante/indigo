#!/bin/bash
# Compila um exemplo de 32X no formato antigo do Chilly Willy (32x-old-skel do Marsdev: boot.s com o
# m68k_crt0.bin/m68k_crt1.bin embutidos, código do SH-2 em src/ e do 68000 em src_md/). Chamado pelo t7_32x.sh.
# uso: t7_32x_mk_old.sh pasta saida.32x pasta_do_toolchain
set -e
SRC=$1; OUT=$2; TC=$3
export PATH="$TC/bin:$PATH"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cp -r "$SRC"/. "$W"/
cd "$W"
# a libgcc do m68k-linux-gnu é do 68020: as rotinas de multiplicar/dividir do 68000 vêm do lb1sf68.S do gcc
LB1=$TC/src-gcc/libgcc/config/m68k/lb1sf68.S
mkdir -p lb1
for fn in mulsi3 divsi3 udivsi3 modsi3 umodsi3; do
  m68k-linux-gnu-gcc -m68000 -c -x assembler-with-cpp -DL$fn "$LB1" -o lb1/$fn.o
done
m68k-linux-gnu-ar rcs lb1/libgcc68.a lb1/*.o
MDAS="m68k-linux-gnu-as -m68000 --register-prefix-optional"
$MDAS m68k_crt0.s -o crt0.o
m68k-linux-gnu-ld -T m68k_crt0.ld -nostdlib --oformat=binary crt0.o -o m68k_crt0.bin
mdobjs=()
for f in src_md/*.c; do
  [ -f "$f" ] || continue
  m68k-linux-gnu-gcc -m68000 -Os -fomit-frame-pointer -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -Iinc_md -c "$f" -o "$f.o"
  mdobjs+=("$f.o")
done
$MDAS m68k_crt1.s -o crt1.o
m68k-linux-gnu-ld -T m68k_crt1.ld -nostdlib --oformat=binary crt1.o "${mdobjs[@]}" lb1/libgcc68.a -o m68k_crt1.bin
shobjs=(boot.o)
sh-elf-as --small boot.s -o boot.o
for f in src/*.s; do [ -f "$f" ] || continue; sh-elf-as --small "$f" -o "$f.o"; shobjs+=("$f.o"); done
for f in src/*.c; do
  [ -f "$f" ] || continue
  sh-elf-gcc -m2 -mb -O2 -fomit-frame-pointer -ffreestanding -Iinc -c "$f" -o "$f.o"
  shobjs+=("$f.o")
done
sh-elf-gcc -m2 -mb -nostdlib -T mars.ld "${shobjs[@]}" -L"$TC/lib/gcc/sh-elf/13.2.0" -lgcc -o rom.elf
sh-elf-objcopy -O binary rom.elf rom.bin
dd if=rom.bin of="$OUT" bs=8192 conv=sync status=none
cp rom.elf "${OUT%.32x}.elf"
