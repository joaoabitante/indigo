#!/bin/bash
# Prepara o teste do núcleo PC-FX em /root/indigo-env/roms/pcfx (ou na pasta dada), só com material próprio:
#  - BIOS de teste do Índigo (t7_pcfx_bios.c + t7_pcfx_crt0.s), 1 MB, gravada como pcfx_bios_teste.rom;
#  - programa de teste (t7_pcfx_prog.c + t7_pcfx_cpu.c) e o CD (pcfxteste.cue + 2 .bin, t7_pcfx_disco.py);
#  - os resultados esperados do teste de CPU, calculados no PC (cputeste.esperado).
# Precisa do v810-gcc (o mesmo do teste do Virtual Boy: rode antes o t7_vb.sh, que baixa o VUEngine Studio Tools),
# de gcc e de python3. Depois: indigo-t7_pcfx /root/indigo-env/roms/pcfx
set -e
D=${1:-/root/indigo-env/roms/pcfx}
HERE=$(cd "$(dirname "$0")" && pwd)
TC=${V810_TC:-/root/indigo-env/roms/vb/vutools/linux/gcc/bin}
[ -x "$TC/v810-gcc" ] || { echo "falta o v810-gcc em $TC (rode tools/t7_vb.sh antes)"; exit 1; }
mkdir -p "$D/build"
B=$D/build
CF="-O2 -std=gnu99 -fno-builtin -I$HERE"

cat > "$B/bios.ld" <<'FIM'
OUTPUT_FORMAT("elf32-v810", "elf32-v810", "elf32-v810")
OUTPUT_ARCH(v810)
ENTRY(_start)
SECTIONS
{
  .text 0xFFF00000 : { *(.text.start) *(.text*) *(.rodata*) *(.data*) *(.sdata*) }
  .bss 0x00001000 (NOLOAD) : { __bss_start = .; *(.sbss*) *(.bss*) *(COMMON) __bss_end = .; }
  .vectors 0xFFFFFE00 : { KEEP(*(.vectors)) }
}
__stack = 0x001FFE00;
__gp = 0x00009000;
FIM
cat > "$B/prog.ld" <<'FIM'
OUTPUT_FORMAT("elf32-v810", "elf32-v810", "elf32-v810")
OUTPUT_ARCH(v810)
ENTRY(_start)
SECTIONS
{
  .text 0x00008000 : { *(.text.start) *(.text*) *(.rodata*) }
  .data : { *(.data*) *(.sdata*) }
  __gp = . + 0x8000;
  .bss (NOLOAD) : { __bss_start = .; *(.sbss*) *(.bss*) *(COMMON) __bss_end = .; }
}
__stack = 0x000F0000;
FIM

"$TC/v810-gcc" -x assembler-with-cpp -DBIOS -c "$HERE/t7_pcfx_crt0.s" -o "$B/bios_crt0.o"
"$TC/v810-gcc" $CF -c "$HERE/t7_pcfx_bios.c" -o "$B/bios.o"
"$TC/v810-gcc" -nostdlib -T "$B/bios.ld" "$B/bios_crt0.o" "$B/bios.o" -lgcc -o "$B/bios.elf"
"$TC/v810-objcopy" -O binary --gap-fill 0xFF "$B/bios.elf" "$D/pcfx_bios_teste.rom"

"$TC/v810-gcc" -x assembler-with-cpp -c "$HERE/t7_pcfx_crt0.s" -o "$B/prog_crt0.o"
"$TC/v810-gcc" $CF -c "$HERE/t7_pcfx_prog.c" -o "$B/prog.o"
"$TC/v810-gcc" $CF -c "$HERE/t7_pcfx_cpu.c" -o "$B/cpu.o"
"$TC/v810-gcc" -nostdlib -T "$B/prog.ld" "$B/prog_crt0.o" "$B/prog.o" "$B/cpu.o" -lgcc -o "$B/prog.elf"
"$TC/v810-objcopy" -O binary -j .text -j .data "$B/prog.elf" "$B/prog.bin"

python3 "$HERE/t7_pcfx_disco.py" "$B/prog.bin" "$D"
gcc -O1 -DHOST="\"$D/cputeste.esperado\"" "$HERE/t7_pcfx_cpu.c" -o "$B/cpu_pc" -lm
"$B/cpu_pc"
ls -la "$D/pcfx_bios_teste.rom" "$D/pcfxteste.cue" "$D/cputeste.esperado"
