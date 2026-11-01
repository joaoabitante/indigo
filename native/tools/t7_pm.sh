#!/bin/bash
# Baixa e prepara os testes do Pokémon Mini em /root/indigo-env/roms/pm (ou na pasta do 1º argumento):
#  - roms/: as ROMs de exemplo públicas do projeto PokeMini (dev-misc/examples: bouncing, framebuffer, mapview,
#    sound_tst, template), com o código-fonte junto;
#  - cpu_vectors.bin: vetores de teste da CPU (estado antes/depois de uma instrução aleatória) gerados com a
#    CPU do emulador de referência PokeMini (GPLv3, compilada só aqui, fora do repositório do Índigo).
set -e
DEST="${1:-/root/indigo-env/roms/pm}"
mkdir -p "$DEST/roms" "$DEST/build"
cd "$DEST/build"
[ -d PokeMini ] || git clone -q --depth 1 https://github.com/libretro/PokeMini PokeMini
if [ ! -d PokeMiniDS ]; then
  git clone -q --depth 1 --filter=blob:none --sparse https://github.com/DS-Homebrew/PokeMini PokeMiniDS
  git -C PokeMiniDS sparse-checkout set dev-misc/examples
fi
for d in bouncing framebuffer mapview sound_tst template; do
  cp PokeMiniDS/dev-misc/examples/$d/*.min PokeMiniDS/dev-misc/examples/$d/*.asm "$DEST/roms/" 2>/dev/null || true
done

cat > cpugen.c <<'EOF'
// Gera vetores de teste da CPU com o núcleo do PokeMini (referência). Memória: valor pseudoaleatório fixo por
// endereço (o mesmo no teste do Índigo), mais os 8 bytes da instrução no endereço do PC.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "MinxCPU.h"
TMinxCPU MinxCPU;
static uint32_t wa[64]; static uint8_t wv[64]; static int nw;
static uint8_t memv(uint32_t a) { uint32_t h = a * 2654435761u; h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13; return (uint8_t)h; }
static uint8_t prog[8]; static uint32_t progAddr;
uint8_t MinxCPU_OnRead(int cpu, uint32_t a) {
  a &= 0xFFFFFF;
  for (int i = nw - 1; i >= 0; i--) if (wa[i] == a) return wv[i];
  if (a >= progAddr && a < progAddr + 8) return prog[a - progAddr];
  return memv(a);
}
void MinxCPU_OnWrite(int cpu, uint32_t a, uint8_t d) { a &= 0xFFFFFF; if (nw < 64) { wa[nw] = a; wv[nw] = d; nw++; } }
void MinxCPU_OnIRQHandle(uint8_t f, uint8_t s) {}
void MinxCPU_OnIRQAct(uint8_t i) {}
static uint64_t st = 88172645463325252ull;
static uint32_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return (uint32_t)st; }
static uint8_t buf[64]; static int p;
static void P8(unsigned v) { buf[p++] = (uint8_t)v; }
static void P16(unsigned v) { buf[p++] = (uint8_t)v; buf[p++] = (uint8_t)(v >> 8); }
static void dump(FILE* f) {
  p = 0;
  P16(MinxCPU.BA.W.L); P16(MinxCPU.HL.W.L); P8(MinxCPU.HL.B.I); P8(MinxCPU.N.B.H);
  P16(MinxCPU.X.W.L); P8(MinxCPU.X.B.I); P16(MinxCPU.Y.W.L); P8(MinxCPU.Y.B.I); P16(MinxCPU.SP.W.L);
  P16(MinxCPU.PC.W.L); P8(MinxCPU.PC.B.I); P8(MinxCPU.U1); P8(MinxCPU.U2); P8(MinxCPU.Shift_U);
  P8(MinxCPU.F); P8(MinxCPU.E); P8(MinxCPU.Status);
  fwrite(buf, 1, p, f);
}
int main(int argc, char** argv) {
  int n = atoi(argv[1]);
  FILE* f = fopen(argv[2], "wb");
  for (int k = 0; k < n; k++) {
    memset(&MinxCPU, 0, sizeof MinxCPU);
    MinxCPU.BA.W.L = rnd(); MinxCPU.HL.W.L = rnd(); MinxCPU.HL.B.I = rnd(); MinxCPU.N.B.H = rnd();
    MinxCPU.N.B.I = MinxCPU.HL.B.I;
    MinxCPU.X.W.L = rnd(); MinxCPU.X.B.I = rnd(); MinxCPU.Y.W.L = rnd(); MinxCPU.Y.B.I = rnd();
    MinxCPU.SP.W.L = rnd(); MinxCPU.PC.W.L = rnd(); MinxCPU.PC.B.I = rnd(); MinxCPU.F = rnd(); MinxCPU.E = rnd();
    MinxCPU.U1 = MinxCPU.U2 = MinxCPU.PC.B.I;
    if ((rnd() & 7) == 0) { MinxCPU.U1 = MinxCPU.U2 = rnd(); MinxCPU.Shift_U = 1 + (rnd() & 1); }
    if ((MinxCPU.PC.W.L & 0x7FFF) > 0x7FF0) MinxCPU.PC.W.L &= 0xF0FF;
    for (int i = 0; i < 8; i++) prog[i] = rnd();
    int r = rnd() % 10;
    if (r < 3) prog[0] = 0xCE; else if (r < 5) prog[0] = 0xCF;
    if (prog[0] == 0xCF && (rnd() & 3) == 0) prog[1] = 0x79 + (rnd() % 0x37);  // sub-tabela CF 7x..Bx
    progAddr = (MinxCPU.PC.W.L & 0x8000) ? (((uint32_t)MinxCPU.PC.B.I << 15) | (MinxCPU.PC.W.L & 0x7FFF)) : MinxCPU.PC.W.L;
    dump(f); fwrite(prog, 1, 8, f);
    nw = 0;
    int cyc = MinxCPU_Exec();
    dump(f);
    uint8_t c = (uint8_t)cyc, w = (uint8_t)nw;
    fwrite(&c, 1, 1, f); fwrite(&w, 1, 1, f);
    for (int i = 0; i < nw; i++) { uint8_t b[4] = {wa[i], wa[i] >> 8, wa[i] >> 16, wv[i]}; fwrite(b, 1, 4, f); }
  }
  fclose(f);
  return 0;
}
EOF
S=PokeMini/source
gcc -O1 -w -I$S -IPokeMini/libretro/libretro-common/include cpugen.c $S/MinxCPU_XX.c $S/MinxCPU_CE.c \
  $S/MinxCPU_CF.c $S/MinxCPU_SP.c -o cpugen
./cpugen 300000 "$DEST/cpu_vectors.bin"
echo "Testes do Pokémon Mini prontos em $DEST"
