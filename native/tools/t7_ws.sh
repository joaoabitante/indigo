#!/bin/sh
# Baixa e prepara os testes do WonderSwan em /root/indigo-env/roms/ws (ou na pasta dada):
#  - SingleStepTests do NEC V20 (só os arquivos MOO, ~560 MB)
#  - WSCPUTest e WSHWTest (Fredrik Ahlström), montados com o nasm
#  - ROMs de teste do núcleo MiSTer (janelas, prioridade de sprites), já montadas
# Depois: indigo-t7_ws /root/indigo-env/roms/ws
set -e
DEST=${1:-/root/indigo-env/roms/ws}
mkdir -p "$DEST"
cd "$DEST"
if [ ! -d v20/v1_native_binary ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/SingleStepTests/v20 v20
  git -C v20 sparse-checkout set v1_native_binary
fi
[ -d WSCPUTest ] || git clone -q --depth 1 https://github.com/FluBBaOfWard/WSCPUTest
[ -d WSHWTest ] || git clone -q --depth 1 https://github.com/FluBBaOfWard/WSHWTest
if [ ! -d mister ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/MiSTer-devel/WonderSwan_MiSTer mister
  git -C mister sparse-checkout set testroms
fi
command -v nasm >/dev/null || { echo "precisa do nasm (apt install nasm)"; exit 1; }
(cd WSCPUTest && nasm -f bin -o WSCpuTest.wsc WSCpuTest.asm)
(cd WSHWTest && nasm -f bin -o WSHWTest.wsc WSHWTest.asm)
echo "testes do WonderSwan prontos em $DEST"
