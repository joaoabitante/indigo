#!/bin/sh
# Prepara os testes do 3DO (etapa 9b) em /root/indigo-env/roms/3do (ou na pasta dada). Nada disto entra no repositório.
#  - arm7tdmi/: SingleStepTests ARM7TDMI (MIT), só os arquivos das instruções do ARMv3 (régua da CPU ARM60)
#  - bios/teste.bin: a BIOS de teste do Índigo (t9b_3do_bios.s, escrita do zero), para partir sem a BIOS do usuário
#  - disco/homebrew.{iso,cue,bin,chd}: o homebrew próprio (t9b_3do_jogo.s) no sistema de arquivos Opera, montado
#    pelo t9b_3do_disco.py
# A BIOS do usuário (nunca no repositório), se existir em /root/indigo-env/bios-joao/3do/, liga os testes com ela.
# Depois: indigo-t9b_3do /root/indigo-env/roms/3do
set -e
DEST=${1:-/root/indigo-env/roms/3do}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DEST/bios" "$DEST/disco" "$DEST/obj"
cd "$DEST"
command -v arm-none-eabi-as >/dev/null || apt-get install -y --no-install-recommends binutils-arm-none-eabi >/dev/null

# régua da CPU: só os arquivos usados (cerca de 250 MB)
if [ ! -f arm7tdmi/v1/arm_swp.json.bin ]; then
  rm -rf arm7tdmi
  git init -q arm7tdmi
  git -C arm7tdmi remote add origin https://github.com/SingleStepTests/ARM7TDMI
  git -C arm7tdmi config core.sparseCheckout true
  printf 'LICENSE\nv1/arm_b_bl.json.bin\nv1/arm_data_proc_*\nv1/arm_ldm_stm.json.bin\nv1/arm_ldr_str_*\nv1/arm_mrs.json.bin\nv1/arm_msr_*\nv1/arm_mul_mla.json.bin\nv1/arm_swi.json.bin\nv1/arm_swp.json.bin\n' > arm7tdmi/.git/info/sparse-checkout
  git -C arm7tdmi fetch -q --depth 1 --filter=blob:none origin e3097d88d428752736b949d94c094d5da50f0db6
  git -C arm7tdmi -c advice.detachedHead=false checkout -q FETCH_HEAD
fi

# BIOS de teste e homebrew (ARMv3, big-endian)
AS="arm-none-eabi-as -mbig-endian -march=armv3"
$AS -o obj/bios.o "$HERE/t9b_3do_bios.s"
arm-none-eabi-ld -EB -Ttext=0x03000000 -e inicio -o obj/bios.elf obj/bios.o
arm-none-eabi-objcopy -O binary -j .text obj/bios.elf obj/bios.bin
python3 -I -c "import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(d+bytes(1024*1024-len(d)))" obj/bios.bin bios/teste.bin
python3 -I "$HERE/t9b_3do_disco.py" dados obj/t9b_3do_dados.inc
$AS -I obj -o obj/jogo.o "$HERE/t9b_3do_jogo.s"
arm-none-eabi-ld -EB -Ttext=0x00010000 -e inicio -o obj/jogo.elf obj/jogo.o
arm-none-eabi-objcopy -O binary -j .text obj/jogo.elf obj/LaunchMe
python3 -I "$HERE/t9b_3do_disco.py" disco obj/LaunchMe disco
if command -v chdman >/dev/null; then
  rm -f disco/homebrew.chd
  chdman createcd -i disco/homebrew.cue -o disco/homebrew.chd >/dev/null 2>&1 || true
fi
echo "testes do 3DO prontos em $DEST"
