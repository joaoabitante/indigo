#!/bin/sh
# Prepara os testes do Nintendo DS em /root/indigo-env/roms/nds (ou na pasta dada). Nada disto entra no repositório.
#  - arm7tdmi/: SingleStepTests ARM7TDMI (MIT) nas instruções que a régua do 3DO não baixa (Thumb, meia palavra,
#    BX, multiplicação longa); as do ARM vêm de ../3do/arm7tdmi (t9b_3do.sh) se existirem
#  - teste.nds: o homebrew de teste próprio (t7_nds_arm9.c + t7_nds_arm7.c, escritos do zero), compilado com o
#    clang (alvo ARM) e ligado com o binutils arm-none-eabi
#  - cpu5.bin: teste das instruções do ARMv5TE (t7_nds_cpu5.s)
# Depois: indigo-t7_nds /root/indigo-env/roms/nds
set -e
DEST=${1:-/root/indigo-env/roms/nds}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DEST/obj"
O="$DEST/obj"
command -v arm-none-eabi-as >/dev/null || apt-get install -y --no-install-recommends binutils-arm-none-eabi >/dev/null

if [ ! -f "$DEST/arm7tdmi/v1/thumb_push_pop.json.bin" ]; then
  rm -rf "$DEST/arm7tdmi"
  git init -q "$DEST/arm7tdmi"
  git -C "$DEST/arm7tdmi" remote add origin https://github.com/SingleStepTests/ARM7TDMI
  git -C "$DEST/arm7tdmi" config core.sparseCheckout true
  printf 'LICENSE\nv1/arm_bx.json.bin\nv1/arm_ldrh_strh.json.bin\nv1/arm_ldrsb_ldrsh.json.bin\nv1/arm_mull_mlal.json.bin\nv1/thumb_*\n' > "$DEST/arm7tdmi/.git/info/sparse-checkout"
  git -C "$DEST/arm7tdmi" fetch -q --depth 1 --filter=blob:none origin e3097d88d428752736b949d94c094d5da50f0db6
  git -C "$DEST/arm7tdmi" -c advice.detachedHead=false checkout -q FETCH_HEAD
  rm -rf "$DEST/arm7tdmi/.git"
fi

python3 -I "$HERE/t7_nds_rom.py" dados "$O/t7_nds_dados.h"
CF="-O2 -ffreestanding -fno-builtin -nostdlib -fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables -Wall -I$HERE -I$O"
CC9="clang --target=armv5te-none-eabi -mcpu=arm946e-s -mthumb -mfloat-abi=soft $CF"
CC7="clang --target=armv4t-none-eabi -mcpu=arm7tdmi -marm -mfloat-abi=soft $CF"
clang --target=armv5te-none-eabi -mcpu=arm946e-s -DARM9 -c -o "$O/crt9.o" -x assembler-with-cpp "$HERE/t7_nds_crt.s"
clang --target=armv4t-none-eabi -mcpu=arm7tdmi -c -o "$O/crt7.o" -x assembler-with-cpp "$HERE/t7_nds_crt.s"
$CC9 -c -o "$O/arm9.o" "$HERE/t7_nds_arm9.c"
$CC9 -c -o "$O/rt9.o" "$HERE/t7_nds_rt.c"
$CC7 -c -o "$O/arm7.o" "$HERE/t7_nds_arm7.c"
$CC7 -c -o "$O/rt7.o" "$HERE/t7_nds_rt.c"
arm-none-eabi-ld -z noexecstack -T "$HERE/t7_nds.ld" --defsym=ORG=0x02000000 -o "$O/arm9.elf" "$O/crt9.o" "$O/arm9.o" "$O/rt9.o"
arm-none-eabi-ld -z noexecstack -T "$HERE/t7_nds.ld" --defsym=ORG=0x037F8000 -o "$O/arm7.elf" "$O/crt7.o" "$O/arm7.o" "$O/rt7.o"
arm-none-eabi-objcopy -O binary "$O/arm9.elf" "$O/arm9.bin"
arm-none-eabi-objcopy -O binary "$O/arm7.elf" "$O/arm7.bin"
python3 -I "$HERE/t7_nds_rom.py" rom "$O/arm9.bin" "$O/arm7.bin" "$DEST/teste.nds"

# instruções do ARMv5TE
arm-none-eabi-as -march=armv5te -o "$O/cpu5.o" "$HERE/t7_nds_cpu5.s"
arm-none-eabi-ld -z noexecstack -Ttext=0x02000000 -e inicio -o "$O/cpu5.elf" "$O/cpu5.o"
arm-none-eabi-objcopy -O binary -j .text "$O/cpu5.elf" "$O/cpu5.bin"
python3 -I "$HERE/t7_nds_rom.py" rom "$O/cpu5.bin" "$O/arm7.bin" "$DEST/cpu5.nds"
echo "testes do Nintendo DS prontos em $DEST"
