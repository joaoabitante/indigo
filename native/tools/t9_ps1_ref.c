/* Régua do PlayStation (etapa 9): frontend mínimo de libretro que carrega o Beetle PSX (mednafen_psx_libretro.so,
 * compilado fora do repositório pelo t9_ps1.sh; GPL, só como referência) e grava o quadro final em PPM.
 * Uso: t9_ps1_ref core.so pasta_da_bios programa.exe|disco.cue quadros saida.ppm
 * A pasta da BIOS precisa ter scph5501.bin (o Beetle procura por esse nome; pode ser um link para outra BIOS
 * americana). Nada do Beetle é copiado: o núcleo é aberto com dlopen. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRO_ENVIRONMENT_GET_CAN_DUPE 3
#define RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY 9
#define RETRO_ENVIRONMENT_SET_PIXEL_FORMAT 10
#define RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE 13
#define RETRO_ENVIRONMENT_GET_VARIABLE 15
#define RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE 17
#define RETRO_ENVIRONMENT_GET_LOG_INTERFACE 27
#define RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY 31

struct retro_game_info { const char* path; const void* data; size_t size; const char* meta; };
struct retro_variable { const char* key; const char* value; };
typedef void (*log_printf_t)(int level, const char* fmt, ...);
struct retro_log_callback { log_printf_t log; };

static const char* sysdir;
static uint32_t* frame;
static unsigned fw, fh;
static int xrgb = 0;
/* controle do disco: REF_EJECT=1 abre a tampa logo depois de carregar (a BIOS vai para o menu) */
struct disk_cb { bool (*set_eject_state)(bool); bool (*get_eject_state)(void); void* rest[5]; };
static struct disk_cb disk;
static int have_disk = 0;

static void logf_(int level, const char* fmt, ...) {
  if (level < 2 && !getenv("REF_VERBOSE")) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

static bool env(unsigned cmd, void* data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool*)data = true; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *(const char**)data = sysdir; return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: xrgb = *(int*)data == 1; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable* v = data;
      v->value = NULL;
      if (!strcmp(v->key, "beetle_psx_renderer")) v->value = "software";
      else if (!strcmp(v->key, "beetle_psx_skip_bios")) v->value = "disabled";
      else if (!strcmp(v->key, "beetle_psx_crop_overscan")) v->value = "disabled";
      else if (!strcmp(v->key, "beetle_psx_cpu_dynarec")) v->value = "disabled";
      return v->value != NULL;
    }
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: memcpy(&disk, data, sizeof disk); have_disk = 1; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *(bool*)data = false; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: ((struct retro_log_callback*)data)->log = logf_; return true;
    default: return false;
  }
}

static void video(const void* d, unsigned w, unsigned h, size_t pitch) {
  if (!d) return;
  if (w * h > fw * fh || !frame) frame = realloc(frame, (size_t)w * h * 4);
  fw = w; fh = h;
  for (unsigned y = 0; y < h; y++) {
    const uint8_t* row = (const uint8_t*)d + y * pitch;
    for (unsigned x = 0; x < w; x++) {
      uint32_t p;
      if (xrgb) memcpy(&p, row + x * 4, 4);
      else {
        uint16_t q; memcpy(&q, row + x * 2, 2);
        p = ((q >> 11) & 31) << 19 | ((q >> 5) & 63) << 10 | (q & 31) << 3;
      }
      frame[y * w + x] = p;
    }
  }
}
static void audio1(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audioN(const int16_t* d, size_t n) { (void)d; return n; }
static void poll(void) {}
static int16_t input(unsigned p, unsigned d, unsigned i, unsigned id) { (void)p; (void)d; (void)i; (void)id; return 0; }

int main(int argc, char** argv) {
  if (argc < 6) { fprintf(stderr, "uso: %s core.so pasta_bios conteudo quadros saida.ppm\n", argv[0]); return 2; }
  sysdir = argv[2];
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
  typedef void (*set_env_t)(bool (*)(unsigned, void*));
  typedef void (*set_video_t)(void (*)(const void*, unsigned, unsigned, size_t));
  typedef void (*set_a1_t)(void (*)(int16_t, int16_t));
  typedef void (*set_an_t)(size_t (*)(const int16_t*, size_t));
  typedef void (*set_poll_t)(void (*)(void));
  typedef void (*set_input_t)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
  typedef void (*void_t)(void);
  typedef bool (*load_t)(const struct retro_game_info*);
#define SYM(t, n) t n = (t)dlsym(h, #n)
  SYM(set_env_t, retro_set_environment);
  SYM(set_video_t, retro_set_video_refresh);
  SYM(set_a1_t, retro_set_audio_sample);
  SYM(set_an_t, retro_set_audio_sample_batch);
  SYM(set_poll_t, retro_set_input_poll);
  SYM(set_input_t, retro_set_input_state);
  SYM(void_t, retro_init);
  SYM(load_t, retro_load_game);
  SYM(void_t, retro_run);
  retro_set_environment(env);
  retro_set_video_refresh(video);
  retro_set_audio_sample(audio1);
  retro_set_audio_sample_batch(audioN);
  retro_set_input_poll(poll);
  retro_set_input_state(input);
  retro_init();
  struct retro_game_info gi = {argv[3], NULL, 0, NULL};
  if (!retro_load_game(&gi)) { fprintf(stderr, "o Beetle não carregou %s\n", argv[3]); return 1; }
  const int frames = atoi(argv[4]);
  if (getenv("REF_EJECT") && have_disk) disk.set_eject_state(true);
  for (int f = 0; f < frames; f++) retro_run();
  FILE* o = fopen(argv[5], "wb");
  if (!o || !frame) return 1;
  fprintf(o, "P6\n%u %u\n255\n", fw, fh);
  for (unsigned i = 0; i < fw * fh; i++) {
    const uint8_t rgb[3] = {(uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i]};
    fwrite(rgb, 1, 3, o);
  }
  fclose(o);
  return 0;
}
