#!/bin/bash
# Prepara os testes do núcleo Mega Drive em /root/indigo-env/roms/md (ou na pasta dada):
#  1. SingleStepTests do 68000 (680x0/68000/v1, ~200 MB);
#  2. ROMs de exemplo já compiladas do SGDK (licença MIT): homebrews públicas que usam rolagem por linha,
#     interrupção de linha, sombra/realce, máscara de sprites, DMA, som XGM (Z80 + YM2612 + PSG), mapeador SSF
#     (bad-apple, 9,5 MB) e SRAM;
#  3. régua: Genesis Plus GX (núcleo libretro, compilado aqui, fora do repositório) com um frontend mínimo que
#     grava os quadros (*.ref) em níveis de 4 bits por componente; o indigo-t7_md compara ponto a ponto.
#     A lista de ROMs/quadros/teclas fica em lista.txt (o teste lê a mesma lista).
# Precisa de git, make, gcc. Leva alguns minutos.
set -e
D=${1:-/root/indigo-env/roms/md}
mkdir -p "$D"
cd "$D"

if [ ! -d 680x0/68000/v1 ]; then
  git clone -q --filter=blob:none --no-checkout https://github.com/SingleStepTests/680x0 680x0
  git -C 680x0 sparse-checkout set 68000
  git -C 680x0 checkout -q e0d5ece9670205cc84a0101081837deb446f86a3
fi
if [ ! -f sgdk/sample/game/sonic/out/release/rom.bin ]; then
  git clone -q --filter=blob:none --no-checkout https://github.com/Stephane-D/SGDK sgdk
  git -C sgdk sparse-checkout set --no-cone "rom.bin" "/license.txt"
  git -C sgdk checkout -q 9a78a44143a867cd8c5053b95cd061a21f0b73c2
fi
if [ ! -f gpgx/genesis_plus_gx_libretro.so ]; then
  [ -d gpgx ] || git clone -q https://github.com/libretro/Genesis-Plus-GX gpgx
  git -C gpgx checkout -q 60bee4c724859710ce05672239e5b601ad01382e
  make -s -C gpgx -f Makefile.libretro -j8 > gpgx/compilacao.log 2>&1
fi

mkdir -p ref
cat > ref/refmd.c <<'FIM_DO_ARQUIVO'
// Frontend mínimo de libretro para o Genesis Plus GX: roda a ROM e grava os quadros em níveis de 4 bits por
// componente (0..15), a cada "passo" quadros. Teclas iguais às do indigo-t7_md (máscara Key do Índigo,
// apertada nos períodos ímpares de 30 quadros).
// uso: refmd nucleo.so rom.bin quadros passo saida.bin [teclas]
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const void* g_fb; static unsigned g_w, g_h; static size_t g_pitch;
static unsigned g_frame; static unsigned g_keys;
static int g_fmt = 2;

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
static FILE* g_aud;  // som (opcional): AUDOUT=arquivo grava o som estéreo de 16 bits a 44100 Hz
static size_t audioBatch(const int16_t* d, size_t n) { if (g_aud) fwrite(d, 4, n, g_aud); return n; }
static void poll(void) {}
// Key do Índigo: A=1 B=2 SELECT=4 START=8 RIGHT=16 LEFT=32 UP=64 DOWN=128 R=256 L=512 X=1024 Y=2048
// (A do Índigo = B do Mega Drive = B do RetroPad; B = C = A do RetroPad; Y = A = Y; X = Y = X; L = X; R = Z)
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)dev; (void)idx;
  if (port) return 0;
  unsigned k = (g_keys && ((g_frame / 30) % 2)) ? g_keys : 0;
  unsigned m = 0;
  if (k & 1) m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
  if (k & 2) m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
  if (k & 4) m |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
  if (k & 8) m |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
  if (k & 16) m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  if (k & 32) m |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (k & 64) m |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
  if (k & 128) m |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (k & 256) m |= 1 << RETRO_DEVICE_ID_JOYPAD_R;
  if (k & 512) m |= 1 << RETRO_DEVICE_ID_JOYPAD_L;
  if (k & 1024) m |= 1 << RETRO_DEVICE_ID_JOYPAD_X;
  if (k & 2048) m |= 1 << RETRO_DEVICE_ID_JOYPAD_Y;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)m;
  return (m >> id) & 1;
}

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) typeof(&n) p_##n = (typeof(&n))dlsym(h, #n)
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
  SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_init); SYM(retro_load_game); SYM(retro_run);
  SYM(retro_set_controller_port_device);
  g_keys = argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0;
  if (getenv("AUDOUT")) g_aud = fopen(getenv("AUDOUT"), "wb");
  p_retro_set_environment(env); p_retro_init();
  p_retro_set_video_refresh(video); p_retro_set_audio_sample(audio); p_retro_set_audio_sample_batch(audioBatch);
  p_retro_set_input_poll(poll); p_retro_set_input_state(input);
  FILE* f = fopen(argv[2], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  void* rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);
  struct retro_game_info gi = {argv[2], rom, (size_t)n, NULL};
  if (!p_retro_load_game(&gi)) { fprintf(stderr, "load falhou\n"); return 1; }
  p_retro_set_controller_port_device(0, RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 1));  // 6 botões
  const int frames = atoi(argv[3]), step = atoi(argv[4]);
  FILE* out = fopen(argv[5], "wb");
  for (g_frame = 0; g_frame < (unsigned)frames; g_frame++) {
    p_retro_run();
    if ((g_frame + 1) % step == 0) {
      uint16_t wh[2] = {(uint16_t)g_w, (uint16_t)g_h};
      fwrite(wh, 2, 2, out);
      for (unsigned y = 0; y < g_h; y++)
        for (unsigned x = 0; x < g_w; x++) {
          uint8_t c[3] = {0, 0, 0};
          if (g_fb) {
            const uint8_t* row = (const uint8_t*)g_fb + y * g_pitch;
            if (g_fmt == 2) {  // RGB565
              const uint16_t p = ((const uint16_t*)row)[x];
              c[0] = (p >> 12) & 15; c[1] = (p >> 7) & 15; c[2] = (p >> 1) & 15;
            } else if (g_fmt == 1) {  // XRGB8888
              const uint32_t p = ((const uint32_t*)row)[x];
              c[0] = ((p >> 16) & 255) / 17; c[1] = ((p >> 8) & 255) / 17; c[2] = (p & 255) / 17;
            }
          }
          fwrite(c, 1, 3, out);
        }
    }
  }
  fclose(out);
  return 0;
}
FIM_DO_ARQUIVO
gcc -O2 -I"$D/gpgx/libretro/libretro-common/include" ref/refmd.c -o ref/refmd -ldl

# nome  rom  quadros  passo  teclas (máscara Key do Índigo, apertada em períodos alternados de 30 quadros)
#   [mínimo de quadros iguais em %: só nas ROMs sensíveis a poucos ciclos de CPU (contadores de quadros
#   por segundo, carga da CPU, contas no tempo livre, interrupção de linha terminando no limite da linha)]
S=sgdk/sample
cat > lista.txt <<FIM_DA_LISTA
hello-world $S/basics/hello-world/out/release/rom.bin 120 20 0
image $S/basics/image/out/release/rom.bin 120 20 0
pools $S/basics/pools/out/release/rom.bin 600 10 0
sprites-sharing-tiles $S/advanced/sprites-sharing-tiles/out/release/rom.bin 600 10 0
tile-animation $S/advanced/tile-animation/out/release/rom.bin 600 10 0
benchmark $S/benchmark/out/release/rom.bin 1200 20 8 50
cube-3d $S/bitmap/cube-3D/out/release/rom.bin 600 10 0
partic $S/bitmap/partic/out/release/rom.bin 600 10 0 50
starfield-donut $S/demo/starfield-donut/out/release/rom.bin 600 10 0
bad-apple $S/demo/bad-apple/out/release/rom.bin 1800 30 0
hint-scaling $S/fx/h-int/scaling/out/release/rom.bin 600 10 0
hint-wobble $S/fx/h-int/wobble/out/release/rom.bin 600 10 0 50
hilight-shadow $S/fx/hilight-shadow/out/release/rom.bin 600 10 16
linescroll $S/fx/scroll/linescroll/out/release/rom.bin 600 10 0
silhouette $S/fx/silhouette/out/release/rom.bin 600 10 16
sprite-masking $S/fx/sprite-masking/out/release/rom.bin 600 10 16
gentris $S/game/gentris/out/release/rom.bin 1200 20 8
platformer $S/game/platformer/out/release/rom.bin 1200 20 17
sonic $S/game/sonic/out/release/rom.bin 1200 20 17 70
joy-test $S/joy-test/out/release/rom.bin 600 10 3087
sound-test $S/snd/sound-test/out/release/rom.bin 900 30 136
xgm-player $S/snd/xgm-player/out/release/rom.bin 900 30 0 50
console $S/sys/console/out/release/rom.bin 600 20 0
multitasking $S/sys/multitasking/out/release/rom.bin 600 20 0 0
tmx-map $S/tiled/tmx-map/out/release/rom.bin 600 10 16
tmx-objects $S/tiled/tmx-objects/out/release/rom.bin 600 10 16
tmx-collisions $S/tiled/tmx-simple-collisions/out/release/rom.bin 600 10 17
FIM_DA_LISTA
while read -r nome rom quadros passo teclas; do
  [ -f "ref/$nome.ref" ] || ref/refmd gpgx/genesis_plus_gx_libretro.so "$rom" "$quadros" "$passo" "ref/$nome.ref" "$teclas"
done < lista.txt
# som: envelope de volume comparado com o do GPGX
for nome in sonic platformer; do
  rom=$(grep "^$nome " lista.txt | cut -d' ' -f2)
  [ -f "ref/$nome.aud" ] || AUDOUT="ref/$nome.aud" ref/refmd gpgx/genesis_plus_gx_libretro.so "$rom" 900 900 /dev/null 0
done
echo "testes do Mega Drive prontos em $D"
