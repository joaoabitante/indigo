#!/bin/sh
# Prepara os testes do PlayStation em /root/indigo-env/roms/ps1 (ou na pasta dada). Nada disto entra no repositório.
#  - ps1-tests (JaCzekanski): testes de CPU, DMA, GPU... com psx.log e vram.png tirados do console de verdade;
#    montados pelo t9_ps1_build.py com o PSn00bSDK de 2020 (o mesmo da época dos testes) e o gcc mipsel do sistema
#    (precisa do código do GCC baixado pelo t7_32x.sh, para o ponto flutuante por software)
#  - PeterLemon/PSX: testes de CPU prontos (.exe) que mostram PASS/FAIL na tela, sem BIOS
#  - bios/teste.bin: a BIOS de teste do Índigo (t9_ps1_bios.s), para partir sem a BIOS da Sony
#  - ref/: Beetle PSX (Mednafen, libretro; GPL, só como régua) e o frontend t9_ps1_ref; com a BIOS do João em
#    /root/indigo-env/bios-joao/scph1001.bin (nunca no repositório), grava os quadros de referência ref/*.ppm
# Depois: indigo-t9_ps1 /root/indigo-env/roms/ps1
set -e
DEST=${1:-/root/indigo-env/roms/ps1}
HERE=$(cd "$(dirname "$0")" && pwd)
BIOS=${PS1_BIOS:-/root/indigo-env/bios-joao/scph1001.bin}
mkdir -p "$DEST/bios" "$DEST/ref/bin" "$DEST/ref/sys"
cd "$DEST"

command -v mipsel-linux-gnu-g++ >/dev/null || apt-get install -y --no-install-recommends gcc-mipsel-linux-gnu g++-mipsel-linux-gnu binutils-mipsel-linux-gnu >/dev/null

[ -d ps1-tests ] || git clone -q --depth 1 https://github.com/JaCzekanski/ps1-tests ps1-tests
[ -d PeterLemon ] || git clone -q --depth 1 https://github.com/PeterLemon/PSX PeterLemon
if [ ! -d psn00bsdk ]; then
  git clone -q https://github.com/Lameguy64/PSn00bSDK psn00bsdk
  git -C psn00bsdk checkout -q 3d38a12
fi

# ---------------------------------------------------------------- testes do ps1-tests
[ -f build/exe/gpu-triangle.exe ] || python3 -I "$HERE/t9_ps1_build.py" "$DEST" | tail -1

# ---------------------------------------------------------------- BIOS de teste
mipsel-linux-gnu-as -march=r3000 -EL -o bios/bios.o "$HERE/t9_ps1_bios.s"
mipsel-linux-gnu-ld -EL -Ttext=0xBFC00000 -e inicio -o bios/bios.elf bios/bios.o
mipsel-linux-gnu-objcopy -O binary -j .text bios/bios.elf bios/bios.bin
python3 -I -c "import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'wb').write(d+bytes(512*1024-len(d)))" bios/bios.bin bios/teste.bin

# ---------------------------------------------------------------- régua: Beetle PSX
if [ ! -f ref/beetle/mednafen_psx_libretro.so ]; then
  [ -d ref/beetle ] || git clone -q --depth 1 https://github.com/libretro/beetle-psx-libretro ref/beetle
  make -C ref/beetle -j4 HAVE_HW=0 HAVE_LIGHTREC=0 HAVE_CHD=1 HAVE_OPENGL=0 HAVE_VULKAN=0 >/dev/null
fi
gcc -O2 -o ref/bin/t9_ps1_ref "$HERE/t9_ps1_ref.c" -ldl
if [ -f "$BIOS" ]; then
  ln -sf "$BIOS" ref/sys/scph5501.bin
  # quadros finais dos testes de GPU estáticos (o Beetle carrega o .exe depois da BIOS, como o Índigo)
  for t in clipping gp0-e1 gpustat lines mask-bit quad rectangles texture-flip texture-overflow transparency \
           triangle uv-interpolation version-detect vram-to-vram-overlap; do
    [ -f "ref/gpu-$t.ppm" ] || ref/bin/t9_ps1_ref ref/beetle/mednafen_psx_libretro.so ref/sys build/exe/gpu-$t.exe 600 "ref/gpu-$t.ppm" 2>/dev/null
  done
  # menu da BIOS com a tampa aberta (disco de áudio vazio, aberto logo depois de ligar)
  if [ ! -f ref/menu.ppm ]; then
    python3 -I -c "open('ref/silencio.bin','wb').write(bytes(2352*300)); open('ref/silencio.cue','w').write('FILE \"silencio.bin\" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n')"
    REF_EJECT=1 ref/bin/t9_ps1_ref ref/beetle/mednafen_psx_libretro.so ref/sys ref/silencio.cue 1100 ref/menu.ppm 2>/dev/null
  fi
else
  echo "sem a BIOS em $BIOS: os quadros de referência do Beetle ficam para depois"
fi
echo "testes do PlayStation prontos em $DEST"
