/* Referência para os testes do PC Engine: roda uma ROM no núcleo libretro do Geargrafx (compilado só pelo
 * t7_pce.sh, fora do repositório) e imprime, para cada quadro, "quadro largura altura hash" com o hash
 * (FNV-1a) das cores RGB565 das 240 linhas centrais (linhas 1 a 240 das 242 visíveis), sem bordas laterais.
 * Uso: t7_pce_ref geargrafx_libretro.so rom.pce quadros [teclas-por-quadro]
 * Teclas: lista "quadro:máscara,..." com a máscara nos bits de Key do Índigo (vale do quadro dado em diante). */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int bool_t;
struct retro_game_info { const char* path; const void* data; size_t size; const char* meta; };
struct retro_variable { const char* key; const char* value; };

static uint32_t g_hash;
static unsigned g_w, g_h;
static unsigned g_keys;

static int environ_cb(unsigned cmd, void* data) {
  switch (cmd) {
    case 10: return 1;                                  /* SET_PIXEL_FORMAT */
    case 9: case 31: *(const char**)data = "/tmp"; return 1; /* diretórios de sistema e de save */
    case 15: {                                          /* GET_VARIABLE */
      struct retro_variable* v = (struct retro_variable*)data;
      v->value = NULL;
      if (!strcmp(v->key, "geargrafx_overscan")) v->value = "Disabled";
      else if (!strcmp(v->key, "geargrafx_scanline_count")) v->value = "Manual";
      else if (!strcmp(v->key, "geargrafx_scanline_start")) v->value = "1";
      else if (!strcmp(v->key, "geargrafx_scanline_end")) v->value = "240";
      else if (!strcmp(v->key, "geargrafx_aspect_ratio")) v->value = "1:1 PAR";
      else if (!strcmp(v->key, "geargrafx_palette")) v->value = "Standard RGB";
      else if (!strcmp(v->key, "geargrafx_lowpass_filter")) v->value = "Disabled";
      else if (!strcmp(v->key, "geargrafx_backup_ram")) v->value = "Enabled";
      return v->value != NULL;
    }
    case 17: *(bool_t*)data = 0; return 1;              /* GET_VARIABLE_UPDATE */
    case 3: *(bool_t*)data = 1; return 1;               /* GET_CAN_DUPE */
    default: return 0;
  }
}

static int g_frame, g_dumpFrame = -1;
static const char* g_dumpPath;

static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
  if (!data) return;
  if (g_frame == g_dumpFrame) { /* depuração: REF_DUMP=quadro:arquivo grava largura, altura e pixels RGB565 */
    FILE* d = fopen(g_dumpPath, "wb");
    if (d) {
      fwrite(&w, 4, 1, d); fwrite(&h, 4, 1, d);
      for (unsigned y = 0; y < h; y++) fwrite((const uint8_t*)data + y * pitch, 2, w, d);
      fclose(d);
    }
  }
  uint32_t hash = 2166136261u;
  for (unsigned y = 0; y < h; y++) {
    const uint16_t* row = (const uint16_t*)((const uint8_t*)data + y * pitch);
    for (unsigned x = 0; x < w; x++) {
      /* volta para 3 bits por cor (cada emulador converte para RGB do seu jeito) */
      const unsigned v = row[x];
      const unsigned r = ((v >> 11) * 7 + 15) / 31, g = (((v >> 5) & 63) * 7 + 31) / 63, b = ((v & 31) * 7 + 15) / 31;
      hash ^= (g << 6) | (r << 3) | b;
      hash *= 16777619u;
    }
  }
  g_hash = hash; g_w = w; g_h = h;
}
static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t* d, size_t n) { (void)d; return n; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)dev; (void)idx;
  if (port != 0) return 0;
  if (id == 256) return (int16_t)g_keys; /* RETRO_DEVICE_ID_JOYPAD_MASK */
  return (g_keys >> id) & 1;
}

int main(int argc, char** argv) {
  if (argc < 4) { fprintf(stderr, "uso: %s núcleo.so rom quadros [quadro:máscara,...]\n", argv[0]); return 1; }
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) void* p_##n = dlsym(h, #n); if (!p_##n) { fprintf(stderr, "falta %s\n", #n); return 1; }
  SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample) SYM(retro_set_audio_sample_batch)
  SYM(retro_set_input_poll) SYM(retro_set_input_state) SYM(retro_init) SYM(retro_load_game) SYM(retro_run)
  ((void (*)(int (*)(unsigned, void*)))p_retro_set_environment)(environ_cb);
  ((void (*)(void (*)(const void*, unsigned, unsigned, size_t)))p_retro_set_video_refresh)(video_cb);
  ((void (*)(void (*)(int16_t, int16_t)))p_retro_set_audio_sample)(audio_cb);
  ((void (*)(size_t (*)(const int16_t*, size_t)))p_retro_set_audio_sample_batch)(audio_batch_cb);
  ((void (*)(void (*)(void)))p_retro_set_input_poll)(input_poll_cb);
  ((void (*)(int16_t (*)(unsigned, unsigned, unsigned, unsigned)))p_retro_set_input_state)(input_state_cb);
  ((void (*)(void))p_retro_init)();
  FILE* f = fopen(argv[2], "rb");
  if (!f) { fprintf(stderr, "sem ROM\n"); return 1; }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  void* buf = malloc((size_t)n);
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
  fclose(f);
  struct retro_game_info gi = {argv[2], buf, (size_t)n, NULL};
  if (!((int (*)(const struct retro_game_info*))p_retro_load_game)(&gi)) { fprintf(stderr, "não carregou\n"); return 1; }
  const int frames = atoi(argv[3]);
  const char* dump = getenv("REF_DUMP");
  if (dump && strchr(dump, ':')) { g_dumpFrame = atoi(dump); g_dumpPath = strchr(dump, ':') + 1; }
  const char* keys = argc > 4 ? argv[4] : "";
  for (int i = 0; i < frames; i++) {
    /* teclas: "quadro:máscara" vale do quadro dado em diante */
    const char* p = keys;
    while (*p) {
      int fr = atoi(p);
      const char* c = strchr(p, ':');
      if (!c) break;
      if (fr <= i) {
        /* máscara do Índigo (A, B, SELECT, START, direita, esquerda, cima, baixo) para os bits do libretro */
        static const int RETRO[8] = {8, 0, 2, 3, 7, 6, 4, 5};
        const unsigned m = (unsigned)strtoul(c + 1, NULL, 0);
        g_keys = 0;
        for (int b = 0; b < 8; b++) if (m & (1u << b)) g_keys |= 1u << RETRO[b];
      }
      p = strchr(c, ',');
      if (!p) break;
      p++;
    }
    g_frame = i;
    ((void (*)(void))p_retro_run)();
    printf("%d %u %u %08x\n", i, g_w, g_h, g_hash);
  }
  return 0;
}
