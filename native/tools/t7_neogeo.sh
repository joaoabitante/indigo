#!/bin/bash
# Prepara os testes do núcleo Neo Geo em /root/indigo-env/roms/neogeo (ou na pasta dada). Nada disto vai para o
# repositório:
#  1. ngdevkit (LGPL-3, github.com/dciabrin/ngdevkit) e os exemplos do ngdevkit-examples: compilados aqui com o
#     gcc do m68k-linux-gnu (com a newlib do Ubuntu compilada para o 68000) e o SDCC (Z80), mais a nullbios, a
#     BIOS livre do ngdevkit (vira um neogeo.zip com sp-s2.sp1, sfix.sfix, sm1.sm1 e 000-lo.lo). Cada exemplo é
#     empacotado de dois jeitos: .zip no formato do MAME (arquivos .p1/.s1/.m1/.v1/.c1/.c2) e .neo.
#  2. régua: Geolith (núcleo libretro, BSD-3, github.com/libretro/geolith-libretro) com um frontend mínimo que
#     grava os quadros (*.ref); o indigo-t7_neogeo compara ponto a ponto. A lista fica em lista.txt.
# Precisa de git, make, gcc, gcc-m68k-linux-gnu, sdcc, python3 com PIL e apt-get (para baixar a newlib-source).
set -e
TOOLS=$(cd "$(dirname "$0")" && pwd)
D=${1:-/root/indigo-env/roms/neogeo}
mkdir -p "$D"
cd "$D"

# ------------------------------------------------------------------ fontes
# versões usadas na validação (2026-10)
[ -d ngdevkit ] || { git clone -q https://github.com/dciabrin/ngdevkit ngdevkit && \
  git -C ngdevkit checkout -q 5106cbd1414a86ba02810ee93ac977be3360de4f; }
[ -d ngdevkit-examples ] || { git clone -q https://github.com/dciabrin/ngdevkit-examples ngdevkit-examples && \
  git -C ngdevkit-examples checkout -q 4e7900b79b1555f9f1737549a49c26fbdb1aa589; }
[ -d geolith ] || { git clone -q https://github.com/libretro/geolith-libretro geolith && \
  git -C geolith checkout -q 2dc88e3fa72ff404213a65d6eb5bb33baa687f73; }
# só um arquivo/pasta de um repositório grande, numa versão fixa
sparse() {  # pasta url commit caminho
  [ -d "$1/.git" ] || { git init -q "$1" && git -C "$1" remote add origin "$2"; }
  git -C "$1" sparse-checkout set --no-cone "$4"
  git -C "$1" fetch -q --depth 1 --filter=blob:none origin "$3"
  git -C "$1" checkout -q FETCH_HEAD
}

# ------------------------------------------------------------------ toolchain do 68000 (gcc do Linux + newlib)
TC=$D/tc
NG=$D/ngdk
mkdir -p "$TC/bin"
for t in ar ranlib objcopy ld as nm strip; do
  printf '#!/bin/sh\nexec m68k-linux-gnu-%s "$@"\n' $t > "$TC/bin/m68k-neogeo-elf-$t"
done
LIBGCC_DIR=$(dirname "$(m68k-linux-gnu-gcc -m68000 -print-libgcc-file-name)")
GCC_INC=$(m68k-linux-gnu-gcc -print-file-name=include)
cat > "$TC/bin/m68k-neogeo-elf-gcc" <<EOF
#!/bin/sh
# gcc do m68k-linux-gnu usado como compilador "bare metal" do Neo Geo (com a newlib, sem a libc do Linux)
exec m68k-linux-gnu-gcc -m68000 -mstrict-align -fno-pic -fno-pie -no-pie -fno-stack-protector -Wl,--build-id=none -Wl,-z,noexecstack -nostdinc \\
  -isystem $TC/newlib/m68k-elf/include -isystem $GCC_INC -B$TC/newlib/m68k-elf/lib -L$TC/newlib/m68k-elf/lib \\
  -L$LIBGCC_DIR "\$@" -Os
EOF
for t in sdcc sdasz80 sdldz80 sdar sdranlib sdobjcopy; do
  printf '#!/bin/sh\nexec %s "$@"\n' $t > "$TC/bin/z80-neogeo-ihx-$t"
done
chmod +x "$TC"/bin/*
export PATH=$TC/bin:$PATH
# -mstrict-align: o gcc do Linux supõe que o processador aceita acessos desalinhados (68020+); no 68000 eles são
# erros de endereço. Mesmo assim o gcc 13 (em -O1 e -O2) junta bytes de vetores locais em move.l desalinhados; com -Os
# (posto depois das opções de quem chama) isso não acontece. Se a configuração do compilador mudar, refaz tudo
TCVER=4
if [ "$(cat "$TC/versao" 2>/dev/null)" != "$TCVER" ]; then
  rm -rf "$TC/newlib" "$TC/src/build" "$TC/lb1" "$NG/.pronto"
  echo $TCVER > "$TC/versao"
fi

if [ ! -f "$TC/newlib/m68k-elf/lib/libc.a" ]; then
  mkdir -p "$TC/src"
  ( cd "$TC/src" && [ -f newlib-source.deb ] || { apt-get download newlib-source >/dev/null && mv newlib-source_*.deb newlib-source.deb; } )
  ( cd "$TC/src" && dpkg-deb -x newlib-source.deb x && tar xf x/usr/src/newlib/newlib-*.tar.xz && rm -rf x )
  NL=$(ls -d "$TC"/src/newlib-*/ | head -1)
  mkdir -p "$TC/src/build"
  ( cd "$TC/src/build" && \
    CC_FOR_TARGET="m68k-linux-gnu-gcc -m68000 -mstrict-align -fno-pic -fno-stack-protector -Os" AR_FOR_TARGET=m68k-linux-gnu-ar \
    AS_FOR_TARGET=m68k-linux-gnu-as LD_FOR_TARGET=m68k-linux-gnu-ld RANLIB_FOR_TARGET=m68k-linux-gnu-ranlib \
    NM_FOR_TARGET=m68k-linux-gnu-nm OBJCOPY_FOR_TARGET=m68k-linux-gnu-objcopy \
    "$NL/configure" --target=m68k-elf --prefix="$TC/newlib" --disable-newlib-supplied-syscalls \
      --disable-multilib --enable-newlib-nano-malloc --disable-nls > ../configure.log 2>&1 && \
    make -j8 all-target-newlib > ../make.log 2>&1 && make install-target-newlib >> ../make.log 2>&1 )
fi
echo "newlib pronta"
# a libgcc do m68k-linux-gnu é montada para o 68020 (bsr.l, que no 68000 vira um desvio para endereço ímpar);
# as rotinas de multiplicação/divisão e ponto flutuante (lb1sf68.S do GCC) são remontadas para o 68000 e entram
# numa cópia da libgcc que fica na pasta da newlib (procurada antes da do sistema)
if [ ! -f "$TC/newlib/m68k-elf/lib/libgcc.a" ]; then
  [ -f "$TC/gccsrc/libgcc/config/m68k/lb1sf68.S" ] || \
    sparse "$TC/gccsrc" https://github.com/gcc-mirror/gcc b76d14f773375487e534233fa2b3a5bcdb81da11 /libgcc/config/m68k/
  mkdir -p "$TC/lb1"
  for fn in $(sed -n 's/^LIB1ASMFUNCS *= *//p' "$TC/gccsrc/libgcc/config/m68k/t-floatlib"); do
    m68k-linux-gnu-gcc -m68000 -c -x assembler-with-cpp -DL$fn "$TC/gccsrc/libgcc/config/m68k/lb1sf68.S" -o "$TC/lb1/$fn.o"
  done
  cp "$LIBGCC_DIR/libgcc.a" "$TC/newlib/m68k-elf/lib/libgcc.a"
  m68k-linux-gnu-ar rs "$TC/newlib/m68k-elf/lib/libgcc.a" "$TC"/lb1/*.o
  rm -f "$NG/.pronto"  # recompila o ngdevkit e os exemplos
fi

# ------------------------------------------------------------------ ngdevkit (runtime, nullsound, nullbios, ferramentas)
NG=$D/ngdk
export PKG_CONFIG_PATH=$NG/lib/pkgconfig
export PATH=$NG/bin:$PATH
if [ ! -f "$NG/.pronto" ]; then
  rm -rf "$NG"; make -C ngdevkit clean > /dev/null 2>&1 || true
  ( cd ngdevkit && autoreconf -i > /dev/null 2>&1 && \
    ./configure --prefix="$NG" --enable-external-toolchain --enable-external-gngeo --enable-external-emudbg \
      --disable-examples > ../ngdevkit-configure.log 2>&1 && make -j4 > ../ngdevkit-make.log 2>&1 && \
    make install >> ../ngdevkit-make.log 2>&1 ) && touch "$NG/.pronto"
  rm -rf ngdevkit-examples/[01]*-*/build pacotes ref
fi
# o arquivo de "specs" do ngdevkit é feito a partir do gcc do Linux; troca pelo de um alvo sem sistema operacional
cat > "$NG/m68k-neogeo-elf/lib/ngdevkit" <<'EOF'
*startfile:
-T ngdevkit.ld%s ngdevkit-crt0.o%s ngdevkit-crti.o%s

*endfile:
ngdevkit-crtn.o%s

*lib:
--start-group -lc -lsyscalls -lgcc --end-group

*link:
-static -nostdlib --no-dynamic-linker -z norelro --build-id=none

*libgcc:
-lgcc

EOF
# a libgcc do Linux é compilada como código relocável e cria uma tabela .got; num binário estático ela pode ficar
# na ROM junto com as constantes
grep -q "got.plt" "$NG/m68k-neogeo-elf/lib/ngdevkit.ld" || \
  sed -i 's|^\( *\)\*libgcc.a:\*(.roda .rodata .rodata.\*)|\1*(.got.plt) *(.got)\n&|' "$NG/m68k-neogeo-elf/lib/ngdevkit.ld"
echo "ngdevkit pronto"

# ------------------------------------------------------------------ exemplos do ngdevkit
command -v rsync > /dev/null || apt-get install -y rsync > /dev/null
command -v sox > /dev/null || apt-get install -y sox libsox-fmt-mp3 > /dev/null
grep -q "^SOX=$" ngdevkit-examples/config.mk 2>/dev/null && rm -f ngdevkit-examples/config.mk ngdevkit-examples/.prebuild
if [ ! -f ngdevkit-examples/config.mk ]; then
  ( cd ngdevkit-examples && autoreconf -i > /dev/null 2>&1 && ./configure > ../examples-configure.log 2>&1 )
fi
( cd ngdevkit-examples && [ -f .prebuild ] || make .prebuild ) > examples-make.log 2>&1 || true
# HASH_GNGEO vazio: não gera o arquivo de dados do GnGeo (só os pacotes)
for ex in ngdevkit-examples/[01]*-*/; do make -C "$ex" HASH_GNGEO= >> examples-make.log 2>&1 || echo "falhou: $ex"; done
ls ngdevkit-examples/*/build/rom/*.zip 2>/dev/null | wc -l | xargs echo "exemplos compilados:"

# ------------------------------------------------------------------ pacotes de teste (.zip do MAME e .neo)
# BIOS: a nullbios (o neogeo.zip do ngdevkit, com sp-s2.sp1 = MVS Europa); cada exemplo vai como .zip no formato
# do MAME e como .neo (convertido pelo t7_neogeo_neo.py)
mkdir -p pacotes bios
cp "$NG/share/ngdevkit/neogeo.zip" bios/neogeo.zip
mkdir -p bios-aes
cp "$NG/share/ngdevkit/aes.zip" bios-aes/aes.zip
for z in ngdevkit-examples/[01]*-*/build/rom/*.zip; do
  case "$(basename "$z")" in neogeo.zip|aes.zip) continue;; esac
  nome=$(basename "$z" .zip)
  cp "$z" "pacotes/$nome.zip"
  python3 "$TOOLS/t7_neogeo_neo.py" "$z" "pacotes/$nome.neo" "$nome"
done
# teste de som próprio: o programa do Z80 (t7_neogeo_som.s) toca SSG, FM, ADPCM-A, ADPCM-B e usa a IRQ do
# temporizador; a V ROM é aleatória (vira ruído nos dois ADPCM); o 68000 é o do 06-sound-adpcma (que manda um
# comando ao Z80 depois que a BIOS troca o SM1 pelo M1 do cartucho)
mkdir -p som
sdasz80 -o som/som.rel "$TOOLS/t7_neogeo_som.s"
sdldz80 -i som/som.ihx som/som.rel > /dev/null
python3 - <<'FIM_PY'
import random, zipfile
# Intel HEX -> binário de 128 KB preenchido com 0x00 (NOP)
rom = bytearray(0x20000)
for line in open('som/som.ihx'):
    line = line.strip()
    if not line.startswith(':'):
        continue
    b = bytes.fromhex(line[1:])
    if b[3] == 0:
        a = (b[1] << 8) | b[2]
        rom[a:a + b[0]] = b[4:4 + b[0]]
open('som/somteste-m1.m1', 'wb').write(rom)
random.seed(7)
v = bytes(random.getrandbits(8) for _ in range(0x80000))
src = zipfile.ZipFile('pacotes/06_sound_adpcma.zip')
with zipfile.ZipFile('pacotes/somteste.zip', 'w') as z:
    for n in src.namelist():
        if n.endswith('.m1') or n.endswith('.v1'):
            continue
        z.writestr(n.replace('06_sound_adpcma', 'somteste'), src.read(n))
    z.write('som/somteste-m1.m1', 'somteste-m1.m1')
    z.writestr('somteste-v1.v1', v)
FIM_PY
python3 "$TOOLS/t7_neogeo_neo.py" pacotes/somteste.zip pacotes/somteste.neo somteste

# ------------------------------------------------------------------ régua: Geolith (libretro)
[ -f geolith/libretro/geolith_libretro.so ] || make -s -C geolith/libretro -j8 > geolith-make.log 2>&1
mkdir -p ref
gcc -O2 -I"$D/geolith/libretro" "$TOOLS/t7_neogeo_ref.c" -o ref/refneo -ldl
echo "régua pronta"

# nome  pacote(sem extensão)  quadros  passo  teclas (máscara Key do Índigo, apertada em períodos alternados de
# 30 quadros: A=1 B=2 SELECT=4 START=8 RIGHT=16 LEFT=32 UP=64 DOWN=128 X=1024 Y=2048; B do Índigo = A do Neo Geo)
#   [mínimo de quadros idênticos em %: o 02-sprite reescreve o texto da camada fixa enquanto a tela é desenhada, e o
#   texto pela metade depende de poucos ciclos de CPU (o Geolith não conta os ciclos do 68000 exatamente como o chip)]
cat > lista.txt <<FIM_DA_LISTA
som-teste somteste 900 30 0
helloworld 01_helloworld 120 20 0
sprite 02_sprite 600 10 80 50
sprite-anim 03_sprite_anim 600 10 16
palette 03_palette 600 10 0
scrolling 05_scrolling 600 10 16
sound-adpcma 06_sound_adpcma 600 20 3
attract-and-game 07_attract_and_game 1200 10 12
soft-dips 08_soft_dips 600 20 0
hsync 09_hsync 600 10 0
credits 10_credits 900 10 12
backup-ram 11_backup_ram 600 20 8
prom-1mb 12_prom_1mb 300 20 0
prom-2mb 13_prom_2mb 300 20 0
prom-bankswitch 14_prom_bankswitch 600 20 130
adpcm-b 15_adpcm_b 600 20 3
sound-music 16_sound_music 900 30 0
memory-card 18_memory_card 600 20 2
template ngdevkit 300 20 0
FIM_DA_LISTA
while read -r nome pacote quadros passo teclas _; do
  [ -f "ref/$nome.ref" ] || ref/refneo geolith/libretro/geolith_libretro.so "pacotes/$pacote.neo" bios "$quadros" "$passo" "ref/$nome.ref" "$teclas" > /dev/null 2>&1
done < lista.txt
# pacotes falsos dos jogos comerciais (nomes e tamanhos da lista de software do MAME, conteúdo aleatório)
# (a mesma versão da lista usada para gerar native/src/neogeo/sets.inc com o t7_neogeo_tabela.py)
[ -f mame/hash/neogeo.xml ] || sparse mame https://github.com/mamedev/mame 511e0d59b12f5a8161df1f3c0e03a99613f45d20 /hash/neogeo.xml
python3 "$TOOLS/t7_neogeo_falsos.py" mame/hash/neogeo.xml falsos

# modo AES (BIOS de console da nullbios, neo-epo.bin): alguns exemplos de novo
cat > lista-aes.txt <<FIM_DA_LISTA
helloworld 01_helloworld 120 20 0
palette 03_palette 600 10 0
scrolling 05_scrolling 600 10 16
hsync 09_hsync 600 10 0
memory-card 18_memory_card 600 20 2
som-teste somteste 600 30 0
FIM_DA_LISTA
while read -r nome pacote quadros passo teclas _; do
  [ -f "ref/aes-$nome.ref" ] || REFSYS=aes ref/refneo geolith/libretro/geolith_libretro.so "pacotes/$pacote.neo" bios-aes "$quadros" "$passo" "ref/aes-$nome.ref" "$teclas" > /dev/null 2>&1
done < lista-aes.txt
# som: o volume de cada trecho é comparado com o do Geolith
for nome in som-teste sound-music; do
  pacote=$(grep "^$nome " lista.txt | cut -d' ' -f2)
  teclas=$(grep "^$nome " lista.txt | cut -d' ' -f5)
  [ -f "ref/$nome.aud" ] || AUDOUT="ref/$nome.aud" ref/refneo geolith/libretro/geolith_libretro.so "pacotes/$pacote.neo" bios 900 900 /dev/null "$teclas" > /dev/null 2>&1
done
echo "testes do Neo Geo prontos em $D"
