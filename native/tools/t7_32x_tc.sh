#!/bin/bash
# Compilador do SH-2 para os testes do 32X (chamado pelo t7_32x.sh): gcc 13.2 para sh-elf (só C, SH-2
# big-endian, com a libgcc), compilado aqui a partir do espelho do GitHub, fora do repositório; binutils do Ubuntu.
# O gcc do Ubuntu para sh4-linux-gnu não gera código de SH-2 (-m2 fica de fora da configuração dele).
# uso: t7_32x_tc.sh pasta_de_destino   (leva uns 20 minutos com 4 núcleos; ~1,5 GB durante a compilação)
set -e
TC=${1:-/root/indigo-env/roms/32x/tc}
mkdir -p "$TC"
cd "$TC"
[ -x bin/sh-elf-gcc ] && [ -f lib/gcc/sh-elf/13.2.0/libgcc.a ] && { echo "sh-elf-gcc já pronto em $TC"; exit 0; }
sparse() {  # pasta url tag caminhos...
  local d=$1 u=$2 t=$3; shift 3
  [ -d "$d/.git" ] && return 0
  git clone -q --depth 1 --branch "$t" --filter=blob:none --no-checkout "$u" "$d"
  git -C "$d" sparse-checkout set --no-cone "$@"
  git -C "$d" checkout -q
}
sparse src-gcc https://github.com/gcc-mirror/gcc releases/gcc-13.2.0 \
  '/*' '!/gcc/testsuite/' '/gcc/testsuite/selftests/' '!/libstdc++-v3/' '!/libgo/' '!/libgfortran/' '!/libphobos/' '!/gcc/ada/' \
  '!/libgm2/' '!/libsanitizer/' \
  '!/libjava/' '!/gotools/' '!/libada/' '!/gnattools/' '!/c++tools/' '!/libcody/' '!/fixincludes/tests/' '!/libgomp/testsuite/'
J=$(nproc)
# binutils: as do Ubuntu (sh4-linux-gnu) já montam e ligam SH-2 big-endian (--isa=sh2 --big, ld -EB); o espelho do
# binutils no GitHub não está acessível daqui. Envoltórios com os nomes de sh-elf:
mkdir -p bin sh-elf/bin
for t in ar ranlib nm objcopy objdump readelf strip size; do ln -sf "$(command -v sh4-linux-gnu-$t)" bin/sh-elf-$t; ln -sf "$(command -v sh4-linux-gnu-$t)" sh-elf/bin/$t; done
printf '#!/bin/sh\nexec sh4-linux-gnu-as --isa=sh2 --big "$@"\n' > bin/sh-elf-as
# o gcc de sh-elf pede a emulação "shelf", que o ld do Linux não tem: tira o -m e liga em big-endian
printf '#!/bin/bash\na=()\nwhile [ $# -gt 0 ]; do if [ "$1" = -m ]; then shift 2; continue; fi; case "$1" in -m*) shift; continue;; esac; a+=("$1"); shift; done\nexec sh4-linux-gnu-ld -EB "${a[@]}"\n' > bin/sh-elf-ld
chmod +x bin/sh-elf-as bin/sh-elf-ld
cp bin/sh-elf-as sh-elf/bin/as; cp bin/sh-elf-ld sh-elf/bin/ld
export PATH="$TC/bin:$PATH"
rm -rf b-gcc && mkdir b-gcc && cd b-gcc
../src-gcc/configure --target=sh-elf --prefix="$TC" --enable-languages=c --disable-nls --disable-multilib \
  --with-cpu=m2 --with-endian=big --without-headers --with-newlib --disable-libssp --disable-libquadmath \
  --disable-libgomp --disable-threads --disable-shared --disable-lto --disable-bootstrap \
  --with-as="$TC/bin/sh-elf-as" --with-ld="$TC/bin/sh-elf-ld" > ../gcc-configure.log 2>&1
make -j"$J" all-gcc MAKEINFO=true > ../gcc-make.log 2>&1
make -j"$J" all-target-libgcc MAKEINFO=true >> ../gcc-make.log 2>&1
make install-gcc install-target-libgcc MAKEINFO=true >> ../gcc-make.log 2>&1
cd .. && rm -rf b-gcc
echo "sh-elf-gcc pronto em $TC"
