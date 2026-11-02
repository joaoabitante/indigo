#!/usr/bin/env bash
# Monta do zero o ambiente de trabalho do Índigo nativo numa máquina Linux nova (Ubuntu 24.04), como as sessões
# agendadas da madrugada: compiladores, SDL3 para Windows, ROMs de teste e os builds.
#   native/tools/setup-env.sh [pasta_do_ambiente]     (padrão: ~/indigo-env)
# Ao terminar imprime as variáveis para usar com validate.sh. Pode ser rodado de novo: pula o que já existe.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
ENV_DIR=${1:-$HOME/indigo-env}
mkdir -p "$ENV_DIR"
cd "$ENV_DIR"
log() { echo "== $*"; }

# 1. pacotes do sistema
need=()
for p in x86_64-w64-mingw32-g++:mingw-w64 wine:wine64 Xvfb:xvfb xdotool:xdotool convert:imagemagick cmake:cmake ninja:ninja-build sdcc:sdcc makensis:nsis nasm:nasm flex:flex; do
  command -v "${p%%:*}" >/dev/null 2>&1 || need+=("${p##*:}")
done
if [ ${#need[@]} -gt 0 ]; then
  log "instalando ${need[*]}"
  SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
  $SUDO apt-get update -qq && $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "${need[@]}" >/dev/null
fi

# 2. ROMs de teste públicas (nas mesmas versões usadas na validação)
clone() { # pasta url commit (completo): busca só esse commit; se não der, o mais recente
  [ -n "$(git -C "$1" rev-parse --verify -q HEAD 2>/dev/null)" ] && return 0
  rm -rf "$1"; git init -q "$1"; git -C "$1" remote add origin "$2"
  if ! git -C "$1" fetch -q --depth 1 origin "$3" 2>/dev/null; then git -C "$1" fetch -q --depth 1 origin HEAD; fi
  git -C "$1" -c advice.detachedHead=false checkout -q FETCH_HEAD
}
mkdir -p roms/gba
clone roms/gb-test-roms https://github.com/retrio/gb-test-roms c240dd7d700e5c0b00a7bbba52b53e4ee67b5f15
clone roms/gba/jsmolka_gba-tests https://github.com/jsmolka/gba-tests a7113b67e63f83a9b321696ddd7042ccfad6c881
clone roms/gba/alyosha-tas_gba-tests https://github.com/alyosha-tas/gba-tests 66f5f1d60cd7030b600606886c76f5c821ecaf9d
clone roms/gba/PeterLemon_GBA https://github.com/PeterLemon/GBA efdb535c9c6212c154379c947e758e173fd1e03d
clone roms/nes-test-roms https://github.com/christopherpow/nes-test-roms 95d8f621ae55cee0d09b91519a8989ae0e64753b
# SNES: testes de referência das CPUs (SingleStepTests) e as ROMs de teste do PeterLemon (só as pastas usadas)
mkdir -p roms/ss roms/snes
clone roms/ss/spc700 https://github.com/SingleStepTests/spc700 HEAD
clone roms/ss/65816 https://github.com/SingleStepTests/65816 HEAD
if [ ! -d roms/snes/peterlemon ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/PeterLemon/SNES roms/snes/peterlemon
  git -C roms/snes/peterlemon sparse-checkout set CPUTest PPU SPC700 BANK Games HelloWorld Compress CHIP
fi
# CHIP (Super FX) entrou depois: ambientes antigos acrescentam a pasta
[ -d roms/snes/peterlemon/CHIP ] || git -C roms/snes/peterlemon sparse-checkout add CHIP
# Pokémon Mini: ROMs de exemplo do PokeMini e vetores da CPU
[ -f roms/pm/cpu_vectors.bin ] || bash "$REPO/native/tools/t7_pm.sh" "$PWD/roms/pm"
# Atari 2600: ROMs de teste e telas de referência do Stella (compila o Stella; uns 10 minutos)
[ -d roms/a2600 ] || bash "$REPO/native/tools/t7_a2600.sh"
# MSX: homebrew livre e exemplos do MSXgl em versões fixas
[ -d roms/msx ] || sh "$REPO/native/tools/t7_msx.sh"
# ColecoVision e SG-1000: programas do CVBasic montados aqui (versões fixas)
[ -d roms/tms ] || bash "$REPO/native/tools/t7_tms.sh"
# WonderSwan: SingleStepTests do V20 (~560 MB), WSCPUTest e WSHWTest (montadas com o nasm)
[ -d roms/ws ] || bash "$REPO/native/tools/t7_ws.sh"
# Atari 5200: BIOS e cartucho de teste próprios (montados com o dasm) e a BIOS livre do Altirra
[ -d roms/a5200 ] || sh "$REPO/native/tools/t7_a5200.sh"
# Atari 7800: exemplos do 7800basic montados aqui (precisa de gcc e flex)
[ -d roms/a7800 ] || bash "$REPO/native/tools/t7_a7800.sh"
# Vectrex: cputest do 6809, tabela de ciclos, vecx como régua (a BIOS, com direitos autorais, só para o teste local)
[ -d roms/vectrex ] || bash "$REPO/native/tools/t7_vectrex.sh"
# Atari Lynx: SingleStepTests do 65x02, cc65, demos do BLL, Handy como régua
[ -d roms/lynx ] || bash "$REPO/native/tools/t7_lynx.sh"
# (opcional) ROM de partida do Lynx do João em roms/lynx/bios/lynxboot.img: liga o teste da partida verdadeira
# Virtual Boy: homebrew, v810-gcc, ROMs de teste e o Mednafen (beetle-vb) como régua
[ -d roms/vb ] || bash "$REPO/native/tools/t7_vb.sh"
# Mega Duck e Watara Supervision: ROMs de teste próprias (o Supervision também compila um programa com o cc65)
[ -d roms/duck ] || bash "$REPO/native/tools/t7_duck.sh"
[ -f roms/svision/cc65demo.sv ] || bash "$REPO/native/tools/t7_svision.sh"
[ -d roms/pcfx ] || bash "$REPO/native/tools/t7_pcfx.sh"  # precisa do v810-gcc baixado pelo t7_vb.sh
# PC Engine: SingleStepTests do HuC6280, HuC, Mesen 2 e Geargrafx como réguas
[ -d roms/pce ] || bash "$REPO/native/tools/t7_pce.sh"
# Famicom Disk System: BIOS e discos de teste próprios (ca65/ld65 do cc65; régua do Mesen 2 do t7_pce.sh)
[ -f roms/fds/teste.fds ] || bash "$REPO/native/tools/t7_fds.sh" "$PWD/roms/fds"
# Mega Drive: SingleStepTests do 68000, ROMs do SGDK e o Genesis Plus GX como régua
[ -d roms/md ] || bash "$REPO/native/tools/t7_md.sh"
# 32X: gcc do SH-2, homebrew do Marsdev e programas de teste próprios, PicoDrive como régua
[ -d roms/32x ] || bash "$REPO/native/tools/t7_32x.sh"
# Capcom CPS1/CPS2: vetores da decifração do CPS2 gerados com o código do MAME (fora do repositório)
[ -d roms/cps ] || bash "$REPO/native/tools/t7_cps.sh"
# Neo Geo: ngdevkit (gcc m68k, nullbios, exemplos) e o Geolith como régua
[ -d roms/neogeo ] || bash "$REPO/native/tools/t7_neogeo.sh"
# CD (.chd): imagens sintéticas .cue/.bin e as de teste do libcdio convertidas com o chdman (mame-tools)
[ -f roms/chd/lista.txt ] || bash "$REPO/native/tools/t7_chd.sh" "$PWD/roms/chd"
# Neo Geo CD (depois do Neo Geo): BIOS de teste do Índigo, exemplos do ngdevkit em CD e o NeoCD como régua
[ -d roms/neogeocd ] || bash "$REPO/native/tools/t7_neogeocd.sh"
# Sega CD: BIOS de teste própria (gcc do m68k) e disco sintético, comparados com o GPGX do t7_md.sh
[ -f roms/segacd/ref/teste.ref ] || bash "$REPO/native/tools/t7_segacd.sh"
# PC Engine CD: disco sintético, System Cards de teste (pceas), scsitest/okitest e o Mesen 2 como régua (depois do pce)
[ -d roms/pcecd ] || sh "$REPO/native/tools/t7_pcecd.sh"
# PlayStation: ps1-tests montados com o PSn00bSDK (depois do 32x, que baixa o código do GCC), PeterLemon, BIOS de
# teste própria e o Beetle PSX como régua; a BIOS do João (bios-joao/scph1001.bin) liga os testes com a BIOS verdadeira
[ -d roms/ps1/build ] || sh "$REPO/native/tools/t9_ps1.sh"
# PS1 GTE: o gte.cpp do Mednafen compilado como régua (libgteref.so) e os vetores de GTE do ps1-tests
[ -f roms/ps1gte/libgteref.so ] || bash "$REPO/native/tools/t9_gte.sh"
# PlayStation SPU: o SPU do Mednafen (Beetle PSX) como régua numa biblioteca à parte e o ps1-tests (spu/)
[ -f roms/ps1spu/libspuref.so ] || bash "$REPO/native/tools/t9_spu.sh" "$PWD/roms/ps1spu"
# PS1 CD-ROM e MDEC: discos sintéticos, Beetle PSX (mdec.c/cdc.c) como régua compilada fora do repositório e o ps1-tests
[ -f roms/ps1cd/ref/mdec.bin ] || bash "$REPO/native/tools/t9_cdmdec.sh" "$PWD/roms/ps1cd"
# PlayStation inteiro: variantes do MDEC, o disco de teste (PSn00bSDK + ISO 9660 próprio) e o quadro do Beetle (depois do ps1)
[ -f roms/ps1int/disco/teste.cue ] || sh "$REPO/native/tools/t9_ps1int.sh" "$PWD/roms/ps1int"
# 3DO: SingleStepTests ARM7TDMI (régua da CPU), BIOS de teste e homebrew próprios no disco Opera (binutils do ARM)
[ -f roms/3do/disco/homebrew.cue ] || sh "$REPO/native/tools/t9b_3do.sh" "$PWD/roms/3do"
# Game & Watch: núcleo SM510 do MAME (BSD-3-Clause) compilado fora do repositório como régua da CPU
[ -f roms/gnw/vetores.txt ] || bash "$REPO/native/tools/t7_gnw.sh" "$PWD/roms/gnw"
# Atari Jaguar: cartuchos de teste próprios (68000 + GPU/DSP), montados com o binutils do m68k
[ -f roms/jaguar/teste2.j64 ] || bash "$REPO/native/tools/t7_jaguar.sh" "$PWD/roms/jaguar"
# Saturn: BIOS de teste e CD homebrew próprios (precisa do sh-elf-gcc do 32X, montado acima)
[ -f roms/saturn/disco/homebrew.cue ] || sh "$REPO/native/tools/t7_saturn.sh" "$PWD/roms/saturn"
cp -rn "$REPO/native/tests/roms/smsroms" "$REPO/native/tests/roms/ngproms" roms/ 2>/dev/null || true
cp -n "$REPO/native/tests/roms/zexall.com" "$REPO/native/tests/roms/zexdoc.com" roms/ 2>/dev/null || true

# 3. SDL3 estático para Windows (MinGW), a mesma versão do app
if [ ! -f deps/sdl3-win/lib/libSDL3.a ]; then
  log "compilando SDL3 para Windows"
  [ -d deps/SDL ] || git clone -q --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL.git deps/SDL
  cmake -S deps/SDL -B deps/b-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="$REPO/native/toolchain-mingw.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
    -DCMAKE_INSTALL_PREFIX="$ENV_DIR/deps/sdl3-win" >/dev/null
  cmake --build deps/b-win >/dev/null && cmake --install deps/b-win >/dev/null
fi

# 3b. SDL3 compartilhado para o app do Linux (vai no pacote em lib/, tools/pacote-linux.sh). Os cabeçalhos do X11,
# Wayland, PipeWire, PulseAudio e ALSA só servem para compilar: o SDL abre cada um com dlopen ao rodar.
if [ ! -f deps/sdl3-linux/lib/libSDL3.so.0 ]; then
  log "compilando SDL3 para Linux"
  SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO=sudo
  $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends libx11-dev libxext-dev \
    libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev libwayland-dev libxkbcommon-dev \
    libdecor-0-dev wayland-protocols libegl-dev libgl-dev libgles-dev libasound2-dev libpulse-dev \
    libpipewire-0.3-dev libdbus-1-dev libudev-dev libusb-1.0-0-dev >/dev/null
  [ -d deps/SDL ] || git clone -q --depth 1 --branch release-3.4.18 https://github.com/libsdl-org/SDL.git deps/SDL
  cmake -S deps/SDL -B deps/b-lin -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
    -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DCMAKE_INSTALL_PREFIX="$ENV_DIR/deps/sdl3-linux" \
    -DCMAKE_C_FLAGS="-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong" \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,relro,-z,now" >/dev/null
  cmake --build deps/b-lin >/dev/null && cmake --install deps/b-lin >/dev/null && rm -rf deps/b-lin
fi

# 4. builds do Índigo: Linux (ferramentas), Windows (app + ferramentas) e sanitizadores (robustez)
log "compilando o Índigo"
cmake -S "$REPO/native" -B build-lin -G Ninja -DINDIGO_APP=OFF >/dev/null && cmake --build build-lin >/dev/null
cmake -S "$REPO/native" -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE="$REPO/native/toolchain-mingw.cmake" \
  -DCMAKE_PREFIX_PATH="$ENV_DIR/deps/sdl3-win" >/dev/null && cmake --build build-win >/dev/null
cmake -S "$REPO/native" -B build-san -G Ninja -DINDIGO_APP=OFF -DINDIGO_LTO=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" >/dev/null
cmake --build build-san --target indigo-robustez indigo-run indigo-ziptest indigo-backuptest >/dev/null

mkdir -p wine
cat <<INFO
== pronto
build Linux:    $ENV_DIR/build-lin
build Windows:  $ENV_DIR/build-win   (indigo.exe)
app do Linux:   cmake -S native -B build-app -DCMAKE_PREFIX_PATH=$ENV_DIR/deps/sdl3-linux (tools/pacote-linux.sh)
sanitizadores:  $ENV_DIR/build-san
ROMs de teste:  $ENV_DIR/roms
revalidar:      $REPO/native/tools/validate.sh $ENV_DIR/build-lin $ENV_DIR/roms
Wine:           WINEPREFIX=$ENV_DIR/wine WINEDEBUG=-all wine $ENV_DIR/build-win/indigo-run.exe ...
INFO
