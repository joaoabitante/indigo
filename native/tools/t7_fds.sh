#!/bin/bash
# Prepara os testes do Famicom Disk System em /root/indigo-env/roms/fds (ou na pasta dada). Nada disto vai para o
# repositório e nada vem da Nintendo:
#  - BIOS de teste (t7_fds_bios.s, escrita para o Índigo; 8 KB em $E000) -> testbios.rom
#  - programa de teste (t7_fds_jogo.s, em $6000) e os discos montados pelo t7_fds_disco.py:
#    teste.fds (cabeçalho fwNES, 2 lados), teste_semcab.fds, teste.qd e teste_crcruim.qd
#  - régua (opcional): o núcleo do Mesen 2 (GPL-3.0) com t7_fds_mesen.cc roda a mesma BIOS e o mesmo disco e
#    grava ref/mesen.txt (estado do jogo quadro a quadro). Usa os .o do Mesen 2 que o t7_pce.sh compila em
#    roms/pce/src/mesen2; sem eles, a régua fica de fora (o indigo-t7_fds pula essa parte).
# Monta com o ca65/ld65 do cc65 (zlib): usa o do PATH, o de /root/ref/cc65 ou baixa e compila em <pasta>/cc65.
# Depois: indigo-t7_fds /root/indigo-env/roms/fds
set -e
TOOLS=$(cd "$(dirname "$0")" && pwd)
D=${1:-/root/indigo-env/roms/fds}
mkdir -p "$D"
cd "$D"

if command -v ca65 > /dev/null; then CC65=$(dirname "$(command -v ca65)")
elif [ -x /root/ref/cc65/bin/ca65 ]; then CC65=/root/ref/cc65/bin
else
  [ -d cc65 ] || git clone -q --depth 1 https://github.com/cc65/cc65 cc65
  [ -x cc65/bin/ca65 ] || make -s -C cc65 -j2 bin > cc65-make.log 2>&1
  CC65=$D/cc65/bin
fi

cat > bios.cfg <<'FIM'
MEMORY { ROM: start = $E000, size = $2000, fill = yes, fillval = $FF; }
SEGMENTS { BIOS: load = ROM, type = ro; VECTORS: load = ROM, type = ro, start = $FFFA; }
FIM
cat > prog.cfg <<'FIM'
MEMORY { RAM: start = $6000, size = $1000; }
SEGMENTS { PROG: load = RAM, type = rw; }
FIM
"$CC65/ca65" -o bios.o "$TOOLS/t7_fds_bios.s"
"$CC65/ld65" -C bios.cfg -o testbios.rom bios.o
"$CC65/ca65" -o prog.o "$TOOLS/t7_fds_jogo.s"
"$CC65/ld65" -C prog.cfg -o prog.bin prog.o
python3 "$TOOLS/t7_fds_disco.py" prog.bin .
ls -l testbios.rom prog.bin teste.fds teste_semcab.fds teste.qd teste_crcruim.qd

# ------------------------------------------------------------------ régua: Mesen 2
M=${MESEN2:-$(dirname "$D")/pce/src/mesen2}
if [ -f "$M/Core/NES/Mappers/FDS/Fds.o" ]; then
  mkdir -p ref mesen-home/Firmware
  cp testbios.rom mesen-home/Firmware/disksys.rom
  (cd "$M" && g++ -O2 --std=c++17 -I "$M" -I "$M/Core" -I "$M/Utilities" -o "$D/ref/t7_fds_mesen" \
     "$TOOLS/t7_fds_mesen.cc" $(find Core Utilities SevenZip Lua -name '*.o') -pthread 2>/dev/null)
  ref/t7_fds_mesen "$D/mesen-home" "$D/teste.fds" 400 > ref/mesen.txt 2>/dev/null
  echo "régua do Mesen 2: $(wc -l < ref/mesen.txt) quadros"
else
  echo "sem o Mesen 2 compilado em $M (rode antes tools/t7_pce.sh): régua pulada"
fi
