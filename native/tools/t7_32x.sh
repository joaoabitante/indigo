#!/bin/bash
# Prepara os testes do núcleo 32X em /root/indigo-env/roms/32x (ou na pasta dada):
#  1. compilador do SH-2 (gcc 13.2 para sh-elf, t7_32x_tc.sh) e do 68000 (gcc do m68k-linux-gnu do Ubuntu);
#  2. régua: PicoDrive (núcleo libretro, licença não comercial: só como régua, compilado aqui, fora do repositório)
#     com o frontend t7_32x_refpico.c (quadros em RGB565) e o interpretador SH-2 dele (derivado do MAME) com o
#     t7_32x_ref.c, que gera os resultados dos casos aleatórios de instrução (sh2ref.bin, t7_32x_gen.h);
#  3. homebrews livres do 32X compilados aqui: os exemplos 32x-skeleton e 32x-old-skel do Marsdev (MIT, código
#     do Chilly Willy) e os programas de teste do Índigo (native/tests/32x, montados por cima do 32x-skeleton);
#     os quadros do PicoDrive vão para ref/<nome>.ref e o som para ref/<nome>.aud.
# Precisa de git, make, gcc, gcc-m68k-linux-gnu, gcc-sh4-linux-gnu (binutils do SH) e libgmp/mpfr/mpc-dev.
# Leva uns 25 minutos (quase tudo é o gcc do SH-2).
set -e
D=${1:-/root/indigo-env/roms/32x}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D"
cd "$D"

command -v sh4-linux-gnu-as > /dev/null || apt-get install -y -q gcc-sh4-linux-gnu > /dev/null
command -v m68k-linux-gnu-gcc > /dev/null || apt-get install -y -q gcc-m68k-linux-gnu > /dev/null
# bibliotecas para compilar o gcc do SH-2 (máquinas novas não têm)
[ -f /usr/include/mpc.h ] || DEBIAN_FRONTEND=noninteractive apt-get install -y -q libgmp-dev libmpfr-dev libmpc-dev > /dev/null
bash "$HERE/t7_32x_tc.sh" "$D/tc"

mkdir -p src
[ -d src/marsdev ] || { git clone -q --filter=blob:none https://github.com/andwn/marsdev src/marsdev && \
  git -C src/marsdev checkout -q 3318d3f39823154b24ce48bfa5d8fe6e3f6cde3f; }
if [ ! -f picodrive/picodrive_libretro.so ]; then
  [ -d picodrive ] || git clone -q https://github.com/libretro/picodrive picodrive
  git -C picodrive checkout -q 1890c2932234c9d30f4cd3851d02228baae8f09e
  git -C picodrive submodule update -q --init --depth 1 pico/sound/emu2413 pico/cd/libchdr platform/common/dr_libs platform/libpicofe
  make -s -C picodrive -f Makefile.libretro -j"$(nproc)" > picodrive-make.log 2>&1
fi

mkdir -p ref hb
gcc -O2 -w -Ipicodrive -Ipicodrive/pico -I"$HERE" "$HERE/t7_32x_ref.c" -o ref/sh2ref
gcc -O2 -Ipicodrive/platform/libretro/libretro-common/include "$HERE/t7_32x_refpico.c" -o ref/refpico -ldl
[ -f sh2ref.bin ] || ref/sh2ref 0 200000 sh2ref.bin

# homebrews
EX=src/marsdev/examples
[ -f hb/skeleton.32x ] || bash "$HERE/t7_32x_mk.sh" "$EX/32x-skeleton" "$D/hb/skeleton.32x" "$D/tc"
[ -f hb/oldskel.32x ] || bash "$HERE/t7_32x_mk_old.sh" "$EX/32x-old-skel" "$D/hb/oldskel.32x" "$D/tc"
for n in 1 2 3 4 5 6 7; do
  [ -f "hb/teste$n.32x" ] && continue
  rm -rf "hb/src-teste$n" && cp -r "$EX/32x-skeleton" "hb/src-teste$n"
  cp "$HERE/../tests/32x/md_teste.c" "hb/src-teste$n/md_src/md_main.c"
  cp "$HERE/../tests/32x/sh_teste.c" "hb/src-teste$n/sh_src/m_main.c"
  rm -f "hb/src-teste$n/sh_src/s_main.c"
  SHDEFS="-DTESTE=$n" bash "$HERE/t7_32x_mk.sh" "hb/src-teste$n" "$D/hb/teste$n.32x" "$D/tc"
  rm -rf "hb/src-teste$n"
done

# nome  rom  quadros  passo  teclas (máscara Key do Índigo, apertada em períodos alternados de 30 quadros)  [mínimo %]
#   [mínimo de quadros iguais em %: o 32x-old-skel troca o quadro sem esperar e o escravo desenha ao mesmo tempo:
#   o que aparece depende de poucos ciclos de diferença entre os SH-2 dos dois emuladores]
cat > lista.txt <<FIM_DA_LISTA
skeleton hb/skeleton.32x 600 10 1
oldskel hb/oldskel.32x 600 10 8 70
teste1 hb/teste1.32x 300 10 0
teste2 hb/teste2.32x 300 10 0
teste3 hb/teste3.32x 300 10 0
teste4 hb/teste4.32x 300 10 0
teste5 hb/teste5.32x 600 10 0
teste6 hb/teste6.32x 300 10 0
teste7 hb/teste7.32x 600 10 0
FIM_DA_LISTA
while read -r nome rom quadros passo teclas resto; do
  [ -f "ref/$nome.ref" ] || ref/refpico picodrive/picodrive_libretro.so "$rom" "$quadros" "$passo" "ref/$nome.ref" "$teclas" 2> /dev/null
done < lista.txt
# som: envelope do PWM comparado com o do PicoDrive
[ -f ref/teste5.aud ] || AUDOUT=ref/teste5.aud ref/refpico picodrive/picodrive_libretro.so hb/teste5.32x 600 600 /dev/null 0 2> /dev/null
echo "testes do 32X prontos em $D"
