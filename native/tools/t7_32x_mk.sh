#!/bin/bash
# Compila um exemplo de 32X no formato do Marsdev/Chilly Willy (pastas md_src/ e sh_src/, ou os arquivos do
# 68000 e do SH-2 juntos) com o gcc do m68k-linux-gnu e o sh-elf-gcc do t7_32x_tc.sh. Chamado pelo t7_32x.sh.
# uso: t7_32x_mk.sh pasta_do_exemplo saida.32x pasta_do_toolchain [libgcc_68000.a]
set -e
SRC=$1; OUT=$2; TC=$3; LIBGCC68=$4
export PATH="$TC/bin:$PATH"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
cp -r "$SRC"/. "$W"/
cd "$W"
# o gcc do Linux mantém uint32_t = unsigned int com -mshort (int de 16 bits); o m68k-elf usa long
for t in INT32 INT_LEAST32 INT_FAST32 INTPTR; do
  printf '#undef __%s_TYPE__\n#define __%s_TYPE__ long int\n#undef __U%s_TYPE__\n#define __U%s_TYPE__ long unsigned int\n' $t $t $t $t
done > "$W/t32.h"
MDCC="m68k-linux-gnu-gcc -m68000 -mshort -include $W/t32.h -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -O2 -fomit-frame-pointer"
SHCC="sh-elf-gcc -m2 -mb -O2 -ffreestanding -fomit-frame-pointer $SHDEFS"
SHLIB="-L$TC/lib/gcc/sh-elf/13.2.0 -lgcc"
if [ -d md_src ]; then MD=md_src; SH=sh_src; else MD=.; SH=.; fi
# 68000: o código que vai em 3F0 (ou 880000+) do cartucho
mdobjs=()
for f in $MD/*.s; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in sh2_*|mars_*|lzss_decode.s) continue;; esac
  $MDCC -x assembler-with-cpp -I$MD -Wa,--register-prefix-optional -c "$f" -o "$f.o"
  mdobjs+=("$f.o")
done
for f in $MD/*.c; do
  [ -f "$f" ] || continue
  $MDCC -I$MD -Iinc_md -c "$f" -o "$f.o"
  mdobjs+=("$f.o")
done
MDLD=$(ls $MD/*.ld 2>/dev/null | head -1)
m68k-linux-gnu-gcc -m68000 -nostdlib -no-pie -Wl,--build-id=none -Wl,-z,noexecstack -T "$MDLD" "${mdobjs[@]}" ${LIBGCC68:+"$LIBGCC68"} -o md_start.elf
m68k-linux-gnu-objcopy -O binary md_start.elf md_start.bin
cp md_start.bin $SH/ 2>/dev/null || true
# SH-2
shobjs=()
first=$(ls $SH/*_start.s $SH/sh2_crt0.s 2>/dev/null | head -1)
for f in "$first" $SH/*.s; do
  [ -f "$f" ] || continue
  case " ${shobjs[*]} " in *" $f.o "*) continue;; esac
  sh-elf-gcc -m2 -mb -x assembler-with-cpp -I$SH -c "$f" -o "$f.o"
  shobjs+=("$f.o")
done
for f in $SH/*.c; do
  [ -f "$f" ] || continue
  $SHCC -I$SH -Iinc -c "$f" -o "$f.o"
  shobjs+=("$f.o")
done
SHLD=$(ls $SH/mars*.ld 2>/dev/null | head -1)
sh-elf-gcc -m2 -mb -nostdlib -T "$SHLD" "${shobjs[@]}" $SHLIB -o rom.elf
sh-elf-objcopy -O binary rom.elf rom.bin
dd if=rom.bin of="$OUT" bs=8192 conv=sync status=none
cp rom.elf "${OUT%.32x}.elf"
