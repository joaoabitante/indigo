#!/bin/sh
# Baixa/monta os arquivos de teste do núcleo do Atari Lynx em /root/indigo-env/roms/lynx (fora do repositório).
# Uso: sh native/tools/t7_lynx.sh   (depois: indigo-t7_lynx /root/indigo-env/roms/lynx)
set -e
D=${1:-/root/indigo-env/roms/lynx}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D"
cd "$D"

# SingleStepTests do 65x02: synertek65c02 (= 65SC02) inteiro e, do wdc65c02, só RMB/SMB/BBR/BBS ($x7/$xF)
if [ ! -d 65x02 ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/SingleStepTests/65x02
  (cd 65x02 && git sparse-checkout set --no-cone /synertek65c02/ '/wdc65c02/v1/?7.json' '/wdc65c02/v1/?f.json')
fi

# demos e homebrew públicos do Bastian Schick (lynx_hacking: Apache 2.0; new_bll: demos do BLL)
[ -d lynx_hacking ] || git clone -q --depth 1 https://github.com/42Bastian/lynx_hacking
[ -d new_bll ] || git clone -q --depth 1 https://github.com/42Bastian/new_bll

# cc65 (para os exemplos do Lynx e a ROM de teste do Índigo)
if [ ! -x cc65/bin/cl65 ]; then
  [ -d cc65 ] || git clone -q --depth 1 https://github.com/cc65/cc65
  make -C cc65 -j4 >/dev/null
fi
mkdir -p build
cp "$HERE/t7_lynx_rom.c" build/
(cd build &&
  ../cc65/bin/cl65 -t lynx -O -o lynxteste.lnx t7_lynx_rom.c &&
  ../cc65/bin/cl65 -t lynx -Oris -o hello.lnx ../cc65/samples/lynx/hello.c &&
  ../cc65/bin/cl65 -t lynx -Oris -o tgidemo.lnx ../cc65/samples/lynx/tgidemo.c &&
  rm -f *.o)
# cabeçalho: EEPROM 93C46 (byte 60) na ROM de teste; uma cópia "girada para a esquerda" (byte 58)
python3 -I - "$D/build" <<'EOF'
import sys
d = sys.argv[1]
b = bytearray(open(d + '/lynxteste.lnx', 'rb').read())
b[60] = 1
open(d + '/lynxteste.lnx', 'wb').write(b)
b[58] = 1
open(d + '/lynxteste_rot.lnx', 'wb').write(b)
EOF
echo "Lynx: arquivos de teste prontos em $D"
