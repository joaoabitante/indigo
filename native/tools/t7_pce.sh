#!/bin/sh
# Baixa e prepara os testes do PC Engine em /root/indigo-env/roms/pce (ou na pasta dada):
#  - SingleStepTests do HuC6280 (huc6280/v1, ~180 MB)
#  - HuC/HuCC (pce-devel/huc): compila o compilador, os testes do compilador (huc-tests/, 4 variantes) e os
#    exemplos e ROMs de teste de hardware do John Brandwood (roms/)
#  - emuladores de referência, compilados só aqui (fora do repositório), que gravam em ref/ o hash de cada
#    quadro (cores de 9 bits) para o indigo-t7_pce comparar:
#      Mesen 2 (núcleo C++ + t7_pce_mesen.cc) -> ref/<rom>.mesen.txt (a régua principal)
#      Geargrafx (núcleo libretro + t7_pce_ref.c) -> ref/<rom>.txt (só informativo)
# Precisa de git, make, g++/cc e python3 (não usa SDL nem .NET).
# Depois: indigo-t7_pce /root/indigo-env/roms/pce
set -e
DEST=${1:-/root/indigo-env/roms/pce}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DEST"
cd "$DEST"

if [ ! -d huc6280/v1 ]; then
  git clone -q --depth 1 https://github.com/SingleStepTests/HuC6280 huc6280
fi

# ---------------------------------------------------------------- HuC
[ -d src/huc ] || { mkdir -p src; git clone -q --depth 1 https://github.com/pce-devel/huc src/huc; }
(cd src/huc && make -s -k >/dev/null 2>&1 || true)
HUC=$DEST/src/huc
export PCE_INCLUDE=$HUC/include/huc PCE_PCEAS=$HUC/bin/pceas
mkdir -p huc-tests build
for d in large small norec noopt; do
  case $d in
    large) opt="-DSTACK_SIZE=1024" ;;
    small) opt="-msmall -DSTACK_SIZE=256 -DSMALL" ;;
    norec) opt="-DSTACK_SIZE=1024 -DNORECURSE -fno-recursive" ;;
    noopt) opt="-DSTACK_SIZE=1024 -O0 -DNOOPT" ;;
  esac
  mkdir -p build/$d
  for i in "$HUC"/test/tests/*.c; do
    n=$(basename "$i" .c)
    [ -f huc-tests/$d-$n.pce ] && continue
    cp "$i" build/$d/
    if (cd build/$d && "$HUC/bin/huc" -DNO_LABEL_VALUES $opt $n.c -lmalloc >/dev/null 2>&1); then
      cp build/$d/$n.pce huc-tests/$d-$n.pce
    fi
  done
done
cp "$HUC"/test/tests/*-huc.bmp huc-tests/

# exemplos e testes de hardware que rodam como HuCARD (os de CD-ROM/ADPCM e de cartuchos especiais ficam de fora)
mkdir -p roms
for f in $(cd "$HUC/examples" && find . -name '*.pce' -o -name '*.sgx'); do
  case $f in *scsitest*|*fastcd*|*sdcard*|*gulliver*|*hwdetect*|*okitest*|*clktest*) continue ;; esac
  cp "$HUC/examples/$f" roms/
done

mkdir -p ref
# ---------------------------------------------------------------- referência principal: Mesen 2
if [ ! -x ref/t7_pce_mesen ]; then
  [ -d src/mesen2 ] || git clone -q --depth 1 https://github.com/SourMesen/Mesen2 src/mesen2
  printf 'mesenobjs: $(SEVENZIPOBJ) $(LUAOBJ) $(UTILOBJ) $(COREOBJ)\n' > src/mesen2/extra.mk
  make -s -C src/mesen2 -f makefile -f extra.mk -j"$(nproc)" USE_GCC=true LTO=false SDL2INC= SDL2LIB= mesenobjs >/dev/null 2>&1
  M=$DEST/src/mesen2
  (cd "$M" && g++ -O2 --std=c++17 -I "$M" -I "$M/Core" -I "$M/Utilities" -o "$DEST/ref/t7_pce_mesen" \
     "$HERE/t7_pce_mesen.cc" $(find Core Utilities SevenZip Lua -name '*.o') -pthread 2>/dev/null)
fi

# ---------------------------------------------------------------- segunda referência: Geargrafx (libretro)
if [ ! -f ref/geargrafx_libretro.so ]; then
  [ -d src/geargrafx ] || git clone -q --depth 1 https://github.com/drhelius/Geargrafx src/geargrafx
  make -s -C src/geargrafx/platforms/libretro -j"$(nproc)" >/dev/null
  cp src/geargrafx/platforms/libretro/geargrafx_libretro.so ref/
fi
cc -O2 -o ref/t7_pce_ref "$HERE/t7_pce_ref.c" -ldl

for r in roms/*.pce roms/*.sgx; do
  n=$(basename "$r")
  [ -f "ref/$n.mesen.txt" ] || ref/t7_pce_mesen "$r" 600 > "ref/$n.mesen.txt" 2>/dev/null
  [ -f "ref/$n.txt" ] || ref/t7_pce_ref ref/geargrafx_libretro.so "$r" 600 > "ref/$n.txt" 2>/dev/null
done
# casos com teclas (o mesmo roteiro de KEY_CASES no t7_pce.cpp)
while read -r rom tag keys; do
  [ -f "ref/$rom-$tag.mesen.txt" ] || ref/t7_pce_mesen "roms/$rom" 600 "$keys" > "ref/$rom-$tag.mesen.txt" 2>/dev/null
  [ -f "ref/$rom-$tag.txt" ] || ref/t7_pce_ref ref/geargrafx_libretro.so "roms/$rom" 600 "$keys" > "ref/$rom-$tag.txt" 2>/dev/null
done <<EOF
rcrtest.pce sel 60:4,64:0,250:4,254:0,400:8,404:0,450:128,460:0
tiatest.pce sel 60:4,64:0,250:4,254:0,400:64,404:0
dmatest.pce sel 60:128,64:0,200:4,204:0,350:4,354:0,500:16,504:0
mwrtest.pce sel 60:4,64:0,250:4,254:0,400:16,404:0
shmup.pce jogo 60:8,66:0,120:17,200:33,260:65,330:129,420:1,480:0
shmup.sgx jogo 60:8,66:0,120:17,200:33,260:65,330:129,420:1,480:0
hugerom.pce jogo 60:8,66:0,100:16,140:32,180:1,200:0
EOF
echo "testes do PC Engine prontos em $DEST"
