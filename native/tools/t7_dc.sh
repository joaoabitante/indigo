#!/bin/sh
# Prepara o material de teste do Dreamcast em /root/indigo-env/roms/dc (ou na pasta dada), tudo feito aqui:
#   bios/teste_boot.bin  BIOS de teste do Índigo (2 MB; t7_dc_bios.c), NÃO a da Sega
#   bios/teste_flash.bin flash de teste (128 KB, só o cabeçalho "KATANA_FLASH" das partições)
#   cpu.bin + cpu_pc.txt teste da CPU (t7_dc_cpu.c) para o SH-4 e a saída do mesmo C no PC
#   disco/teste.gdi      GD de teste (t7_dc_disco.py) com o jogo de teste (t7_dc_jogo.c) e o programa do ARM7
#   disco/teste_cd.iso   o mesmo jogo num .iso de CD comum
# Ferramentas: sh4-linux-gnu-gcc (Debian/Ubuntu: gcc-sh4-linux-gnu), arm-none-eabi-as, python3 e o gcc do PC.
set -eu
DEST=${1:-/root/indigo-env/roms/dc}
T=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DEST/obj" "$DEST/bios" "$DEST/disco"
O="$DEST/obj"
CC="sh4-linux-gnu-gcc -m4 -ml -O2 -ffreestanding -nostdlib -fno-builtin -fno-math-errno -ffp-contract=off -fno-pic -fno-pie -I$T"
LIBGCC=$(sh4-linux-gnu-gcc -print-libgcc-file-name)

ld_script() {  # $1 = origem do código, $2 = origem do .bss (vazio = logo depois)
  if [ -n "$2" ]; then BSS=". = $2;"; else BSS=""; fi
  cat <<EOF
ENTRY(_start)
SECTIONS {
  . = $1;
  .text : { *(.text.start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) *(.sdata*) *(.got*) }
  $BSS
  __bss_start = .;
  .bss (NOLOAD) : { *(.bss*) *(.sbss*) *(COMMON) }
  . = ALIGN(4);
  _end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu*) }
}
EOF
}

# BIOS de teste: código em 0xA0000000, .bss na RAM
ld_script 0xA0000000 0x8C000100 > "$O/bios.ld"
$CC -DSTACK_TOP=0x8C007F00 -c "$T/t7_dc_crt0.S" -o "$O/crt0_bios.o"
$CC -c "$T/t7_dc_bios.c" -o "$O/bios.o"
sh4-linux-gnu-ld -z noexecstack --no-warn-rwx-segments -T "$O/bios.ld" "$O/crt0_bios.o" "$O/bios.o" "$LIBGCC" -o "$O/bios.elf"
sh4-linux-gnu-objcopy -O binary -j .text -j .rodata "$O/bios.elf" "$O/bios.raw"
python3 - "$O/bios.raw" "$DEST/bios/teste_boot.bin" "$DEST/bios/teste_flash.bin" <<'EOF'
import sys
b = open(sys.argv[1], 'rb').read()
assert len(b) < 0x200000
open(sys.argv[2], 'wb').write(b + b'\xff' * (0x200000 - len(b)))
f = bytearray(b'\xff' * 0x20000)
for off in (0x1A000, 0x1C000):
    f[off:off + 16] = b'KATANA_FLASH____'
open(sys.argv[3], 'wb').write(bytes(f))
EOF

# programa do ARM7 do AICA
arm-none-eabi-as -march=armv4 "$T/t7_dc_arm.s" -o "$O/arm.o"
arm-none-eabi-objcopy -O binary "$O/arm.o" "$O/arm.bin"
cat > "$O/armbin.S" <<EOF
	.section .rodata
	.balign 4
	.global arm_bin
	.global arm_bin_len
arm_bin:
	.incbin "$O/arm.bin"
arm_bin_end:
	.balign 4
arm_bin_len:
	.long arm_bin_end - arm_bin
EOF

# jogo de teste e teste da CPU: em 0x8C010000
ld_script 0x8C010000 "" > "$O/prog.ld"
$CC -DSTACK_TOP=0x8D000000 -c "$T/t7_dc_crt0.S" -o "$O/crt0.o"
$CC -c "$T/t7_dc_vec.S" -o "$O/vec.o"
$CC -c "$O/armbin.S" -o "$O/armbin.o"
$CC -c "$T/t7_dc_jogo.c" -o "$O/jogo.o"
sh4-linux-gnu-ld -z noexecstack --no-warn-rwx-segments -T "$O/prog.ld" "$O/crt0.o" "$O/jogo.o" "$O/vec.o" "$O/armbin.o" "$LIBGCC" -o "$O/jogo.elf"
sh4-linux-gnu-objcopy -O binary -j .text -j .rodata -j .data "$O/jogo.elf" "$O/jogo.bin"
$CC -c "$T/t7_dc_cpu.c" -o "$O/cpu.o"
sh4-linux-gnu-ld -z noexecstack --no-warn-rwx-segments -T "$O/prog.ld" "$O/crt0.o" "$O/cpu.o" "$LIBGCC" -o "$O/cpu.elf"
sh4-linux-gnu-objcopy -O binary -j .text -j .rodata -j .data "$O/cpu.elf" "$DEST/cpu.bin"
gcc -O1 -fno-math-errno -ffp-contract=off "$T/t7_dc_cpu.c" -lm -o "$O/cpu_pc"
"$O/cpu_pc" > "$DEST/cpu_pc.txt"

python3 "$T/t7_dc_disco.py" "$O/jogo.bin" "$DEST/disco"
# o mesmo GD em .chd (metadados CHGD), se o chdman (mame-tools) existir
if command -v chdman >/dev/null 2>&1; then
  chdman createcd -f -i "$DEST/disco/teste.gdi" -o "$DEST/disco/teste.chd" >/dev/null 2>&1 || echo "chdman falhou: .chd pulado"
fi
touch "$DEST/v1"
echo "material de teste do Dreamcast pronto em $DEST"
