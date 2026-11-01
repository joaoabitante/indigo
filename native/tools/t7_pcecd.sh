#!/bin/sh
# Prepara os testes do PC Engine CD em /root/indigo-env/roms/pcecd (ou na pasta dada). Precisa do t7_pce.sh
# antes (usa o pceas do HuC e o Mesen 2 já compilados em roms/pce/src). Nenhum System Card de verdade entra:
#  - disco/teste.cue + .bin: disco sintético (t7_pcecd_disco.py): dados em padrão, ADPCM, faixas de áudio
#  - cards/card1-4.pce: System Cards de teste próprios (t7_pcecd_card.asm + t7_pcecd_t1-4.asm), montados com o pceas
#  - elmer/: scsitest e okitest do John Brandwood (HuC, licença Boost), HuCards que acionam o CD sem BIOS,
#    com os discos deles (o .wav do scsitest vira .bin)
#  - ref/t7_pcecd_mesen: o Mesen 2 com o System Card dado no lugar do syscard3.pce (t7_pcecd_mesen.cc), que
#    grava em ref/<caso>.mesen.txt o hash de cada quadro e em ref/<caso>.mesen.raw o som
# Depois: indigo-t7_pcecd /root/indigo-env/roms/pcecd
set -e
DEST=${1:-/root/indigo-env/roms/pcecd}
PCE=$(dirname "$DEST")/pce
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DEST/cards" "$DEST/build" "$DEST/ref" "$DEST/elmer"
cd "$DEST"

[ -f disco/teste.bin ] || python3 -I "$HERE/t7_pcecd_disco.py" disco

# ---------------------------------------------------------------- System Cards de teste
for n in 1 2 3 4; do
  printf 'TEST = %d\n\t\tinclude "t7_pcecd_card.asm"\n' $n > build/card$n.asm
  (cd build && "$PCE/src/huc/bin/pceas" --raw -I "$HERE" card$n.asm >/dev/null)
  cp build/card$n.pce cards/
done

# ---------------------------------------------------------------- testes do John Brandwood (sem BIOS)
E=$PCE/src/huc/examples/asm/elmer
if [ ! -f elmer/scsitest.cue ]; then
  cp "$E/cd-core-scsitest/scsitest.pce" "$E/cd-core-scsitest/core-scsitest.iso" elmer/
  cp "$E/rom-core-okitest/okitest.pce" "$E/rom-core-okitest/core-okitest.iso" elmer/
  # .wav (44,1 kHz, 16 bits, estéreo) -> .bin: só as amostras, completando o último setor
  python3 -I - "$E/cd-core-scsitest/test-tone.wav" elmer/test-tone.bin <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
i = d.find(b'data')
n = int.from_bytes(d[i + 4:i + 8], 'little')
s = d[i + 8:i + 8 + n]
s += bytes((-len(s)) % 2352)
open(sys.argv[2], 'wb').write(s)
EOF
  printf 'FILE "test-tone.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\nFILE "core-scsitest.iso" BINARY\n  TRACK 02 MODE1/2048\n    INDEX 01 00:00:00\n' > elmer/scsitest.cue
  printf 'FILE "core-okitest.iso" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n' > elmer/okitest.cue
fi
# HuCards de 16/40 KB espelhados até 256 KB (o tamanho do System Card)
python3 -I - elmer/scsitest.pce elmer/scsitest-card.pce elmer/okitest.pce elmer/okitest-card.pce <<'EOF'
import sys
a = sys.argv[1:]
for i in range(0, len(a), 2):
    d = open(a[i], 'rb').read()
    open(a[i + 1], 'wb').write((d * (0x40000 // len(d) + 1))[:0x40000])
EOF

# ---------------------------------------------------------------- régua: Mesen 2
M=$PCE/src/mesen2
if [ ! -x ref/t7_pcecd_mesen ] || [ "$HERE/t7_pcecd_mesen.cc" -nt ref/t7_pcecd_mesen ]; then
  (cd "$M" && g++ -O2 --std=c++17 -I "$M" -I "$M/Core" -I "$M/Utilities" -o "$DEST/ref/t7_pcecd_mesen" \
     "$HERE/t7_pcecd_mesen.cc" $(find Core Utilities SevenZip Lua -name '*.o') -pthread 2>/dev/null)
fi

# casos: nome cartão disco tipo quadros [teclas] (o mesmo roteiro de CASES no t7_pcecd.cpp)
while read -r name card cue type frames keys; do
  [ -n "$name" ] || continue
  if [ ! -f "ref/$name.mesen.txt" ] || [ "$card" -nt "ref/$name.mesen.txt" ]; then
    ref/t7_pcecd_mesen "$card" "$cue" "$frames" "$type" "${keys:-}" "ref/$name.mesen.raw" > "ref/$name.mesen.txt" 2>/dev/null
  fi
done <<EOF
t1-cd cards/card1.pce disco/teste.cue 0 400
t1-super cards/card1.pce disco/teste.cue 1 400
t1-arcade cards/card1.pce disco/teste.cue 2 400
t2 cards/card2.pce disco/teste.cue 2 700
t3 cards/card3.pce disco/teste.cue 2 700
t4 cards/card4.pce disco/teste.cue 2 200
t4-super cards/card4.pce disco/teste.cue 1 200
scsitest elmer/scsitest-card.pce elmer/scsitest.cue 2 9900 600:8,606:0,1200:8,1206:0,1800:8,1806:0,2400:8,2406:0,3000:8,3006:0,3600:8,3606:0,4200:8,4206:0,4800:8,4806:0,5400:8,5406:0,6000:8,6006:0,6600:8,6606:0,7200:8,7206:0,7800:8,7806:0,8400:8,8406:0,9000:8,9006:0,9600:8,9606:0
okitest elmer/okitest-card.pce elmer/okitest.cue 2 2100 760:8,766:0,1160:8,1166:0,1560:8,1566:0,1960:8,1966:0
EOF
rm -f ref/t1-*.mesen.raw ref/t4*.mesen.raw ref/scsitest.mesen.raw ref/okitest.mesen.raw
echo "testes do PC Engine CD prontos em $DEST"
