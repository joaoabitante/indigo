#!/bin/bash
# Prepara os testes do Sega CD em /root/indigo-env/roms/segacd (ou na pasta dada). Sem a BIOS de verdade (que é
# do usuário), a régua é:
#  1. uma BIOS de teste escrita aqui (t7_segacd_main.c/sub.c/crt.s, montada com o gcc do m68k-linux-gnu): o 68000
#     principal sobe o sub-CPU, que lê setores do disco pelo CDD/CDC (host e DMA para PRG-RAM, Word RAM 2M/1M e
#     PCM), testa a Word RAM 2M/1M, o ASIC de rotação, o temporizador, o PCM e o CD-DA;
#  2. um disco sintético (teste.cue: faixa de dados Mode 1 e duas de áudio com pregap) gerado por t7_segacd_gera.py;
#  3. o Genesis Plus GX (o mesmo de /root/indigo-env/roms/md/gpgx, preparado pelo t7_md.sh) rodando essa BIOS e
#     esse disco, com um frontend mínimo que grava os quadros (*.ref, 4 bits por componente) e o som (*.aud).
# O indigo-t7_segacd compara quadros e som com o GPGX e confere os valores que a BIOS de teste deixa na RAM.
# Precisa de gcc-m68k-linux-gnu, python3, gcc e do GPGX compilado (t7_md.sh).
set -e
D=${1:-/root/indigo-env/roms/segacd}
SRC=$(cd "$(dirname "$0")" && pwd)
GPGX=${GPGX:-/root/indigo-env/roms/md/gpgx}
mkdir -p "$D/build" "$D/ref"
cd "$D/build"

python3 -I "$SRC/t7_segacd_gera.py" "$D/build" > /dev/null
cp teste.cue teste1.bin teste2.bin teste3.bin "$D/"

CC="m68k-linux-gnu-gcc -m68000 -Os -ffreestanding -fno-builtin -nostdlib -fomit-frame-pointer -fno-pic -fno-pie
    -fno-stack-protector -fno-tree-loop-distribute-patterns -fno-asynchronous-unwind-tables -I$SRC -I$D/build"

cat > lib.c <<'FIM'
/* memcpy/memset que o gcc pode chamar (cópia de vetores iniciados) */
void* memcpy(void* d, const void* s, unsigned long n) { char* a = d; const char* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { char* a = d; while (n--) *a++ = (char)c; return d; }
FIM

cat > sub.ld <<'FIM'
/* sub-CPU: código em PRG-RAM 0 (vetores primeiro), variáveis em 0x70000 (fora da área protegida do teste) */
MEMORY { PRG (rwx) : ORIGIN = 0x0, LENGTH = 0x1E000  VAR (rw) : ORIGIN = 0x70000, LENGTH = 0xC000 }
SECTIONS {
  .text : { *(.vectors) *(.text*) *(.rodata*) *(.data*) } > PRG
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(2); __bss_end = .; } > VAR
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
FIM
cat > main.ld <<'FIM'
/* principal: BIOS de 128 KB em 0, variáveis na RAM principal */
MEMORY { ROM (rx) : ORIGIN = 0x0, LENGTH = 0x20000  RAM (rw) : ORIGIN = 0xFF0000, LENGTH = 0x8000 }
SECTIONS {
  .text : { *(.vectors) *(.text*) *(.rodata*) . = ALIGN(2); } > ROM
  .data : { __data_start = .; *(.data*) . = ALIGN(2); __data_end = .; } > RAM AT > ROM
  __data_load = LOADADDR(.data);
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) . = ALIGN(2); __bss_end = .; } > RAM
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
FIM

# sub-CPU
$CC -c "$SRC/t7_segacd_sub.c" -o sub.o
$CC -c t7_segacd_seno.c -o seno.o
$CC -c lib.c -o lib.o
m68k-linux-gnu-as -m68000 --defsym SUB=1 "$SRC/t7_segacd_crt.s" -o crt_sub.o
m68k-linux-gnu-ld -T sub.ld crt_sub.o sub.o seno.o lib.o -o sub.elf
m68k-linux-gnu-objcopy -O binary sub.elf sub.bin
# principal (com o sub.bin dentro)
$CC -c "$SRC/t7_segacd_main.c" -o main.o
m68k-linux-gnu-as -m68000 -I . "$SRC/t7_segacd_crt.s" -o crt_main.o
m68k-linux-gnu-ld -T main.ld crt_main.o main.o lib.o -o bios.elf
m68k-linux-gnu-objcopy -O binary bios.elf bios.bin
python3 -I -c "
import sys
b = open('bios.bin','rb').read()
assert len(b) <= 0x20000, 'BIOS de teste maior que 128 KB'
b = b + b'\xff' * (0x20000 - len(b))
assert b[0x100:0x104] == b'SEGA' and b[0x180:0x182] == b'BR' and b[0x1F0:0x1F1] == b'U', 'cabeçalho errado'
open('$D/bios_teste.bin','wb').write(b)
"
# o GPGX procura a BIOS pela região do disco (EUA) na pasta de sistema
cp "$D/bios_teste.bin" "$D/bios_CD_U.bin"

# frontend mínimo do GPGX (mesmo esquema do refmd do t7_md.sh): grava quadros e som do Sega CD
cat > refscd.c <<'FIM'
// uso: refscd nucleo.so disco.cue quadros passo saida.ref  (AUDOUT=arquivo: som estéreo 16 bits a 44100 Hz)
// A pasta atual é a pasta de sistema (onde fica bios_CD_U.bin).
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const void* g_fb; static unsigned g_w, g_h; static size_t g_pitch;
static int g_fmt = 2;
static FILE* g_aud;
static void logf_(enum retro_log_level l, const char* fmt, ...) { (void)l; (void)fmt; }
static bool env(unsigned cmd, void* data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(enum retro_pixel_format*)data; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable* v = data;
      if (!strcmp(v->key, "genesis_plus_gx_render")) { v->value = "double field"; return true; }
      if (!strcmp(v->key, "genesis_plus_gx_overscan")) { v->value = "disabled"; return true; }
      if (!strcmp(v->key, "genesis_plus_gx_blargg_ntsc_filter")) { v->value = "disabled"; return true; }
      v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char**)data = "."; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = logf_; return true;
    default: return false;
  }
}
static void video(const void* d, unsigned w, unsigned h, size_t pitch) { if (d) { g_fb = d; g_w = w; g_h = h; g_pitch = pitch; } }
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audioBatch(const int16_t* d, size_t n) { if (g_aud) fwrite(d, 4, n, g_aud); return n; }
static void poll(void) {}
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) { (void)port; (void)dev; (void)idx; (void)id; return 0; }

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) typeof(&n) p_##n = (typeof(&n))dlsym(h, #n)
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
  SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_init); SYM(retro_load_game); SYM(retro_run);
  if (getenv("AUDOUT")) g_aud = fopen(getenv("AUDOUT"), "wb");
  p_retro_set_environment(env); p_retro_init();
  p_retro_set_video_refresh(video); p_retro_set_audio_sample(audio); p_retro_set_audio_sample_batch(audioBatch);
  p_retro_set_input_poll(poll); p_retro_set_input_state(input);
  struct retro_game_info gi = {argv[2], NULL, 0, NULL};
  if (!p_retro_load_game(&gi)) { fprintf(stderr, "load falhou\n"); return 1; }
  const int frames = atoi(argv[3]), step = atoi(argv[4]);
  FILE* out = fopen(argv[5], "wb");
  for (int f = 0; f < frames; f++) {
    p_retro_run();
    if ((f + 1) % step == 0) {
      uint16_t wh[2] = {(uint16_t)g_w, (uint16_t)g_h};
      fwrite(wh, 2, 2, out);
      for (unsigned y = 0; y < g_h; y++)
        for (unsigned x = 0; x < g_w; x++) {
          uint8_t c[3] = {0, 0, 0};
          if (g_fb) {
            const uint8_t* row = (const uint8_t*)g_fb + y * g_pitch;
            if (g_fmt == 2) { const uint16_t p = ((const uint16_t*)row)[x]; c[0] = (p >> 12) & 15; c[1] = (p >> 7) & 15; c[2] = (p >> 1) & 15; }
            else if (g_fmt == 1) { const uint32_t p = ((const uint32_t*)row)[x]; c[0] = ((p >> 16) & 255) / 17; c[1] = ((p >> 8) & 255) / 17; c[2] = (p & 255) / 17; }
          }
          fwrite(c, 1, 3, out);
        }
    }
  }
  fclose(out);
  return 0;
}
FIM
gcc -O2 -I"$GPGX/libretro/libretro-common/include" refscd.c -o refscd -ldl

# testes de unidade: o gfx.c e o pcm.c do GPGX com um shared.h mínimo, rodando os casos de t7_segacd_unidade.h
mkdir -p calco
cat > calco/shared.h <<'FIM'
/* shared.h mínimo para compilar só o gfx.c e o pcm.c do Genesis Plus GX (testes de unidade do Índigo) */
#ifndef _SHARED_H_
#define _SHARED_H_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "types.h"
#include "macros.h"
typedef FILE cdStream;
#include "blip_buf.h"
#include "scd.h"
typedef struct { cd_hw_t cd_hw; } calco_ext_t;
typedef struct { unsigned int cycles, stopped, cycle_end, pc; } calco_s68k_t;
typedef struct { int pcm_volume, cdda_volume; } calco_config_t;
typedef struct { blip_t* blips[4]; } calco_snd_t;
extern calco_ext_t ext;
extern calco_s68k_t s68k;
extern calco_config_t config;
extern calco_snd_t snd;
extern int8 audio_hard_disable;
void s68k_update_irq(unsigned int mask);
void error(char* fmt, ...);
#define save_param(p, n) (bufferptr += (int)(n))
#define load_param(p, n) (bufferptr += (int)(n))
#endif
FIM
gcc -O1 -w -DLSB_FIRST -Icalco -I"$SRC" -I"$GPGX/core" -I"$GPGX/core/cd_hw" -I"$GPGX/core/sound" \
    "$SRC/t7_segacd_refunidade.c" "$GPGX/core/cd_hw/gfx.c" "$GPGX/core/cd_hw/pcm.c" -o refunidade
./refunidade > "$D/ref/unidade.txt"

# quadros 1..2100 a cada 50 e o som inteiro
cd "$D"
rm -f ref/teste.ref ref/teste.aud
AUDOUT=ref/teste.aud build/refscd "$GPGX/genesis_plus_gx_libretro.so" teste.cue 2100 50 ref/teste.ref
echo "testes do Sega CD prontos em $D"
