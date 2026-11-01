#!/bin/bash
# Prepara os testes do núcleo Neo Geo CD em /root/indigo-env/roms/neogeocd (ou na pasta dada). Nada disto vai para
# o repositório:
#  1. BIOS de teste (t7_neogeocd_bios.s, escrita para o Índigo): montada com o binutils do m68k-linux-gnu. Faz a
#     partida pelo CD usando o hardware (comandos à unidade, LC8951, DMA, janela de upload) e traz as chamadas da
#     BIOS que os programas do ngdevkit usam. A mesma BIOS vai para o Índigo e para a régua.
#  2. CDs de teste: os exemplos do ngdevkit (os pacotes do t7_neogeo.sh, que precisa ter rodado antes) convertidos
#     para CD pelo t7_neogeocd_disco.py (.PRG/.FIX/.SPR/.Z80/.PCM + IPL.TXT num ISO 9660, em .cue/.bin), e um CD
#     com faixa de áudio (CD-DA).
#  3. régua: NeoCD (neocd_libretro, LGPL-3, github.com/libretro/neocd_libretro), que emula o CD no nível do
#     hardware e roda as BIOS originais, com um frontend mínimo (t7_neogeocd_ref.c) que grava os quadros em
#     RGB565 (*.ref) e o som (*.aud). A lista fica em lista.txt.
# Precisa de git, make, g++, binutils-m68k-linux-gnu e python3.
set -e
TOOLS=$(cd "$(dirname "$0")" && pwd)
D=${1:-/root/indigo-env/roms/neogeocd}
NG=${2:-$(dirname "$D")/neogeo}
[ -d "$NG/pacotes" ] || { echo "faltam os pacotes do Neo Geo: rode antes tools/t7_neogeo.sh"; exit 1; }
mkdir -p "$D"
cd "$D"

# ------------------------------------------------------------------ BIOS de teste
mkdir -p bios sistema/neocd
m68k-linux-gnu-as -m68000 -o bios/bios.o "$TOOLS/t7_neogeocd_bios.s"
m68k-linux-gnu-ld -Ttext=0xC00000 -o bios/bios.elf bios/bios.o
m68k-linux-gnu-objcopy -O binary bios/bios.elf bios/bios.bin
python3 - <<'FIM_PY'
d = open('bios/bios.bin', 'rb').read()
assert len(d) <= 0x80000
open('bios/neocd.bin', 'wb').write(d + b'\xff' * (0x80000 - len(d)))
FIM_PY
cp bios/neocd.bin sistema/neocd/neocd.bin
echo "BIOS de teste pronta"

# ------------------------------------------------------------------ régua: NeoCD (libretro)
# versão usada na validação (2026-10)
[ -d neocd ] || { git clone -q https://github.com/libretro/neocd_libretro neocd && \
  git -C neocd checkout -q b1e04c738cb48a1dae0574b8877f6a116d270ca1; }
[ -f neocd/neocd_libretro.so ] || make -s -C neocd -j8 > neocd-make.log 2>&1
mkdir -p ref
gcc -O2 -I"$D/neocd/src/libretro" -I"$D/neocd/deps/libretro-common/include" "$TOOLS/t7_neogeocd_ref.c" -o ref/refneocd -ldl
echo "régua pronta"

# ------------------------------------------------------------------ CDs de teste
# nome  pacote(do t7_neogeo)  nome no CD (até 8 letras)  quadros  passo  teclas  [mínimo de quadros idênticos em %]
# (mesmas teclas do t7_neogeo: máscara Key do Índigo, apertada em períodos alternados de 30 quadros; START começa o
# jogo na BIOS de teste, como num console de casa). Mínimo abaixo de 100%: o 02-sprite e o 06-sound-adpcma
# reescrevem a camada fixa logo depois da interrupção de quadro, que no NeoCD fica no começo do quadro (a tela
# mostra o texto pela metade num quadro)
mkdir -p discos
cat > lista-cd.txt <<FIM_DA_LISTA
helloworld 01_helloworld HELLO 300 20 0
sprite 02_sprite SPRITE 900 10 80 50
sprite-anim 03_sprite_anim ANIM 900 10 16
palette 03_palette PALETTE 900 10 0
scrolling 05_scrolling SCROLL 900 10 16
sound-adpcma 06_sound_adpcma ADPCMA 900 20 3 95
attract-and-game 07_attract_and_game ATTRACT 1200 10 12
soft-dips 08_soft_dips DIPS 600 20 0
hsync 09_hsync HSYNC 900 10 0
credits 10_credits CREDITS 900 10 12
backup-ram 11_backup_ram BACKUP 600 20 8
adpcm-b 15_adpcm_b ADPCMB 900 20 3
sound-music 16_sound_music MUSIC 900 30 0
memory-card 18_memory_card MEMCARD 600 20 2
template ngdevkit TEMPLATE 600 20 0
som-teste somteste SOMTESTE 900 30 0
cdda 01_helloworld CDDA 900 30 0
FIM_DA_LISTA
: > lista.txt
while read -r nome pacote cdnome quadros passo teclas minimo; do
  extra=""
  [ "$nome" = cdda ] && extra="--audio"
  [ -f "discos/$nome.cue" ] || python3 "$TOOLS/t7_neogeocd_disco.py" "$NG/pacotes/$pacote.zip" "discos/$nome" "$cdnome" $extra
  echo "$nome $nome $quadros $passo $teclas $minimo" >> lista.txt
done < lista-cd.txt
echo "CDs prontos"

# ------------------------------------------------------------------ quadros e som da régua
while read -r nome disco quadros passo teclas _; do
  [ -f "ref/$nome.ref" ] || ref/refneocd neocd/neocd_libretro.so "discos/$disco.cue" sistema "$quadros" "$passo" "ref/$nome.ref" "$teclas" > /dev/null 2>&1
done < lista.txt
# som: o CD-DA contra o NeoCD (o som-teste é comparado com o Geolith do t7_neogeo; ver t7_neogeocd.cpp)
for nome in cdda; do
  disco=$(grep "^$nome " lista.txt | cut -d' ' -f2)
  teclas=$(grep "^$nome " lista.txt | cut -d' ' -f5)
  [ -f "ref/$nome.aud" ] || AUDOUT="ref/$nome.aud" ref/refneocd neocd/neocd_libretro.so "discos/$disco.cue" sistema 900 900 /dev/null "$teclas" > /dev/null 2>&1
done
echo "testes do Neo Geo CD prontos em $D"
