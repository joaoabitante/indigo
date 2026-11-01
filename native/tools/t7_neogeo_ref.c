// Frontend mínimo de libretro para o Geolith (régua do núcleo Neo Geo; compilado só pelo t7_neogeo.sh, fora do
// indigo): Neo Geo MVS, região Europa (BIOS sp-s2.sp1 do neogeo.zip da pasta dada), paleta "raw", 304x224
// (8 pontos de borda cortados de cada lado, como o Índigo). Grava os quadros em RGB de 8 bits a cada "passo"
// quadros. Teclas iguais às do indigo-t7_neogeo (máscara Key do Índigo, apertada nos períodos ímpares de 30
// quadros): B do Índigo = A do Neo Geo, A = B, Y = C, X = D, START = start, SELECT = moeda.
// uso: refneo nucleo.so jogo.neo pasta_bios quadros passo saida.bin [teclas]
// AUDOUT=arquivo grava o som (estéreo, 16 bits, na taxa do Geolith depois da reamostragem); REFSYS=aes roda no modo
// AES (BIOS neo-epo.bin do aes.zip).
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

static const void* g_fb;
static unsigned g_w, g_h;
static size_t g_pitch;
static unsigned g_frame, g_keys;
static int g_fmt = 2;
static const char* g_sysdir = ".";
static FILE* g_aud;

static void logf_(enum retro_log_level l, const char* fmt, ...) {
  if (!getenv("REFLOG") || l < RETRO_LOG_INFO) return;
  va_list va;
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

static bool env(unsigned cmd, void* data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: g_fmt = *(enum retro_pixel_format*)data; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable* v = data;
      static const char* kv[][2] = {
          {"geolith_system_type", "mvs"}, {"geolith_region", "eu"},        {"geolith_memcard", "on"},
          {"geolith_overscan_t", "8"},    {"geolith_overscan_b", "8"},     {"geolith_overscan_l", "8"},
          {"geolith_overscan_r", "8"},    {"geolith_palette", "raw"},      {"geolith_sprlimit", "96"},
          {"geolith_oc", "off"},          {"geolith_freeplay", "off"},     {"geolith_settingmode", "off"},
          {"geolith_4player", "off"},     {"geolith_memcard_wp", "off"},   {"geolith_disable_adpcm_wrap", "off"}};
      if (!strcmp(v->key, "geolith_system_type") && getenv("REFSYS")) { v->value = getenv("REFSYS"); return true; }
      for (unsigned i = 0; i < sizeof kv / sizeof kv[0]; i++)
        if (!strcmp(v->key, kv[i][0])) { v->value = kv[i][1]; return true; }
      v->value = NULL;
      return false;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *(const char**)data = g_sysdir; return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = "/nonexistent"; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = logf_; return true;
    default: return false;
  }
}

static void video(const void* d, unsigned w, unsigned h, size_t pitch) {
  if (d) { g_fb = d; g_w = w; g_h = h; g_pitch = pitch; }
}
static void audio(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audioBatch(const int16_t* d, size_t n) {
  if (g_aud) fwrite(d, 4, n, g_aud);
  return n;
}
static void poll(void) {}

// Key do Índigo: A=1 B=2 SELECT=4 START=8 RIGHT=16 LEFT=32 UP=64 DOWN=128 R=256 L=512 X=1024 Y=2048
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)dev; (void)idx;
  if (port) return 0;
  unsigned k = (g_keys && ((g_frame / 30) % 2)) ? g_keys : 0;
  unsigned m = 0;
  if (k & 1) m |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
  if (k & 2) m |= 1 << RETRO_DEVICE_ID_JOYPAD_B;
  if (k & 4) m |= 1 << RETRO_DEVICE_ID_JOYPAD_SELECT;
  if (k & 8) m |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
  if (k & 16) m |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
  if (k & 32) m |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
  if (k & 64) m |= 1 << RETRO_DEVICE_ID_JOYPAD_UP;
  if (k & 128) m |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;
  if (k & 1024) m |= 1 << RETRO_DEVICE_ID_JOYPAD_X;
  if (k & 2048) m |= 1 << RETRO_DEVICE_ID_JOYPAD_Y;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)m;
  return (m >> id) & 1;
}

int main(int argc, char** argv) {
  if (argc < 7) return 2;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) typeof(&n) p_##n = (typeof(&n))dlsym(h, #n)
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample);
  SYM(retro_set_audio_sample_batch); SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_init);
  SYM(retro_load_game); SYM(retro_run); SYM(retro_set_controller_port_device);
  g_sysdir = argv[3];
  g_keys = argc > 7 ? (unsigned)strtoul(argv[7], NULL, 0) : 0;
  if (getenv("AUDOUT")) g_aud = fopen(getenv("AUDOUT"), "wb");
  p_retro_set_environment(env);
  p_retro_init();
  p_retro_set_video_refresh(video);
  p_retro_set_audio_sample(audio);
  p_retro_set_audio_sample_batch(audioBatch);
  p_retro_set_input_poll(poll);
  p_retro_set_input_state(input);
  struct retro_game_info gi = {argv[2], NULL, 0, NULL};
  if (!p_retro_load_game(&gi)) { fprintf(stderr, "load falhou\n"); return 1; }
  p_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
  const int frames = atoi(argv[4]), step = atoi(argv[5]);
  FILE* out = fopen(argv[6], "wb");
  if (!out) return 1;
  for (g_frame = 0; g_frame < (unsigned)frames; g_frame++) {
    p_retro_run();
    if ((g_frame + 1) % step == 0) {
      uint16_t wh[2] = {(uint16_t)g_w, (uint16_t)g_h};
      fwrite(wh, 2, 2, out);
      for (unsigned y = 0; y < g_h; y++)
        for (unsigned x = 0; x < g_w; x++) {
          uint8_t c[3] = {0, 0, 0};
          if (g_fb && g_fmt == 1) {
            const uint32_t p = ((const uint32_t*)((const uint8_t*)g_fb + y * g_pitch))[x];
            c[0] = (p >> 16) & 255; c[1] = (p >> 8) & 255; c[2] = p & 255;
          }
          fwrite(c, 1, 3, out);
        }
    }
  }
  fclose(out);
  return 0;
}
