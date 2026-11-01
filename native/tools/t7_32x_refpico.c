/* Frontend mínimo de libretro para o PicoDrive (régua do 32X, compilado pelo t7_32x.sh fora do repositório):
 * roda a ROM e grava os quadros em RGB565 (largura e altura u16, depois os pontos), a cada "passo" quadros.
 * Teclas iguais às do indigo-t7_32x (máscara Key do Índigo, apertada nos períodos ímpares de 30 quadros);
 * controle de 6 botões, interpretador do SH-2 (sem o recompilador).
 * uso: refpico nucleo.so rom.32x quadros passo saida.bin [teclas]   (AUDOUT=arquivo grava o som a 44100 Hz) */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const void* g_fb; static unsigned g_w, g_h; static size_t g_pitch;
static unsigned g_frame, g_keys;
static int g_fmt = 0;
static FILE* g_aud;
static const uint8_t* g_sdram;  /* SDRAM do 32X (palavras de 16 bits na ordem do PC), para PEEK */

static void logf_(enum retro_log_level l, const char* fmt, ...) { (void)l; (void)fmt; }
static bool env(unsigned cmd, void* data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(enum retro_pixel_format*)data; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable* v = data;
      if (!strcmp(v->key, "picodrive_input1")) { v->value = "6 button pad"; return true; }
      if (!strcmp(v->key, "picodrive_drc")) { v->value = "disabled"; return true; }
      if (!strcmp(v->key, "picodrive_renderer")) { v->value = "accurate"; return true; }
      if (!strcmp(v->key, "picodrive_sound_rate")) { v->value = "44100"; return true; }
      if (!strcmp(v->key, "picodrive_audio_filter")) { v->value = "disabled"; return true; }
      v->value = NULL; return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char**)data = "."; return true;
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
      const struct retro_memory_map* mm = data;
      unsigned i;
      for (i = 0; i < mm->num_descriptors; i++)
        if (mm->descriptors[i].start == 0x06000000) g_sdram = mm->descriptors[i].ptr;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = logf_; return true;
    default: return false;
  }
}
static void video(const void* d, unsigned w, unsigned h, size_t pitch) { if (d) { g_fb = d; g_w = w; g_h = h; g_pitch = pitch; } }
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audioBatch(const int16_t* d, size_t n) { if (g_aud) fwrite(d, 4, n, g_aud); return n; }
static void poll(void) {}
/* Key do Índigo: A=1 B=2 SELECT=4 START=8 RIGHT=16 LEFT=32 UP=64 DOWN=128 R=256 L=512 X=1024 Y=2048
 * (A do Índigo = B do Mega Drive = B do RetroPad; B = C = A do RetroPad; Y = A = Y; X = Y = X; L = X; R = Z) */
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  unsigned k, m = 0;
  (void)dev; (void)idx;
  if (port) return 0;
  k = (g_keys && ((g_frame / 30) % 2)) ? g_keys : 0;
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
  void* h;
  FILE *f, *out;
  long n;
  void* rom;
  struct retro_game_info gi;
  int frames, step;
  if (argc < 6) return 2;
  h = dlopen(argv[1], RTLD_NOW);
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
  f = fopen(argv[2], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
  rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);
  gi.path = argv[2]; gi.data = rom; gi.size = (size_t)n; gi.meta = NULL;
  if (!p_retro_load_game(&gi)) { fprintf(stderr, "load falhou\n"); return 1; }
  p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  frames = atoi(argv[3]); step = atoi(argv[4]);
  out = fopen(argv[5], "wb");
  for (g_frame = 0; g_frame < (unsigned)frames; g_frame++) {
    p_retro_run();
    if ((g_frame + 1) % step == 0) {
      uint16_t wh[2] = {(uint16_t)g_w, (uint16_t)g_h};
      unsigned x, y;
      fwrite(wh, 2, 2, out);
      for (y = 0; y < g_h; y++)
        for (x = 0; x < g_w; x++) {
          uint16_t p = 0;
          if (g_fb) {
            const uint8_t* row = (const uint8_t*)g_fb + y * g_pitch;
            if (g_fmt == 2) p = ((const uint16_t*)row)[x];
            else if (g_fmt == 1) {
              const uint32_t q = ((const uint32_t*)row)[x];
              p = (uint16_t)((((q >> 19) & 31) << 11) | (((q >> 10) & 63) << 5) | ((q >> 3) & 31));
            }
          }
          fwrite(&p, 2, 1, out);
        }
    }
  }
  fclose(out);
  if (getenv("PEEK") && g_sdram) {  /* PEEK=060005cc,...: palavras longas da SDRAM no fim */
    char* list = strdup(getenv("PEEK"));
    char* t;
    for (t = strtok(list, ","); t; t = strtok(NULL, ",")) {
      unsigned a = (unsigned)strtoul(t, NULL, 16) & 0x3FFFC;
      printf("[%s] = %02X%02X%02X%02X\n", t, g_sdram[a + 1], g_sdram[a], g_sdram[a + 3], g_sdram[a + 2]);
    }
  }
  return 0;
}
