/* Régua do PlayStation (etapa 9): frontend mínimo de libretro que carrega o Beetle PSX (mednafen_psx_libretro.so,
 * compilado fora do repositório pelo t9_ps1.sh; GPL, só como referência) e grava o quadro final em PPM.
 * Uso: t9_ps1_ref core.so pasta_da_bios programa.exe|disco.cue quadros saida.ppm
 * A pasta da BIOS precisa ter scph5501.bin (o Beetle procura por esse nome; pode ser um link para outra BIOS
 * americana). Nada do Beetle é copiado: o núcleo é aberto com dlopen. */
#include <dlfcn.h>
#include <math.h>
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
/* som: soma dos quadrados desde o último quadro gravado (RMS no REF_SHOTS) */
static double sl, sr;
static long sn;
static FILE* wav; /* REF_WAV: s16 estéreo cru */
static void audio1(int16_t l, int16_t r) {
  sl += (double)l * l; sr += (double)r * r; sn++;
  if (wav) { const int16_t o[2] = {l, r}; fwrite(o, 2, 2, wav); }
}
static size_t audioN(const int16_t* d, size_t n) {
  if (wav) fwrite(d, 4, n, wav);
  for (size_t i = 0; i < n; i++) { sl += (double)d[2 * i] * d[2 * i]; sr += (double)d[2 * i + 1] * d[2 * i + 1]; }
  sn += (long)n;
  return n;
}
static void poll(void) {}
/* roteiro de botões REF_KEYS, mesmo formato do PS1KEYS do indigo-t9_ps1int:
 * "quadro:botões[:duração]" ou "de-até/passo:botões[:duração]"; S Start, s Select, X Xis, O Bola, T Triângulo,
 * Q Quadrado, U D L R, 1 L1, 2 R1, 3 L2, 4 R2 */
struct step { int from, to, stepn, dur; unsigned mask; };
static struct step steps[256];
static int nsteps, curframe;
static void parse_keys(const char* t) {
  while (t && *t && nsteps < 256) {
    struct step k = {0, 0, 1, 6, 0};
    k.from = k.to = atoi(t);
    while (*t && *t != ':' && *t != '-' && *t != ',') t++;
    if (*t == '-') {
      k.to = atoi(++t);
      while (*t && *t != ':' && *t != '/' && *t != ',') t++;
      if (*t == '/') { k.stepn = atoi(++t); if (k.stepn < 1) k.stepn = 1; while (*t && *t != ':' && *t != ',') t++; }
    }
    if (*t != ':') { while (*t && *t != ',') t++; if (*t) t++; continue; }
    t++;
    /* ids do RETRO_DEVICE_JOYPAD: B=0 Y=1 SELECT=2 START=3 UP=4 DOWN=5 LEFT=6 RIGHT=7 A=8 X=9 L=10 R=11 L2=12 R2=13 */
    for (; *t && *t != ':' && *t != ','; t++) {
      switch (*t) {
        case 'S': k.mask |= 1u << 3; break;
        case 's': k.mask |= 1u << 2; break;
        case 'X': k.mask |= 1u << 0; break;
        case 'O': k.mask |= 1u << 8; break;
        case 'T': k.mask |= 1u << 9; break;
        case 'Q': k.mask |= 1u << 1; break;
        case 'U': k.mask |= 1u << 4; break;
        case 'D': k.mask |= 1u << 5; break;
        case 'L': k.mask |= 1u << 6; break;
        case 'R': k.mask |= 1u << 7; break;
        case '1': k.mask |= 1u << 10; break;
        case '2': k.mask |= 1u << 11; break;
        case '3': k.mask |= 1u << 12; break;
        case '4': k.mask |= 1u << 13; break;
      }
    }
    if (*t == ':') k.dur = atoi(++t);
    while (*t && *t != ',') t++;
    if (*t) t++;
    steps[nsteps++] = k;
  }
}
static unsigned keys_at(int f) {
  unsigned m = 0;
  for (int i = 0; i < nsteps; i++) {
    const struct step* k = &steps[i];
    if (f < k->from || f >= k->to + k->dur) continue;
    const int s = k->from + ((f < k->to ? f : k->to) - k->from) / k->stepn * k->stepn; /* último aperto já começado */
    if (f - s < k->dur) m |= k->mask;
  }
  return m;
}
static int16_t input(unsigned p, unsigned d, unsigned i, unsigned id) {
  (void)i;
  if (p != 0 || d != 1) return 0;
  const unsigned m = keys_at(curframe);
  if (id == 256) return (int16_t)m; /* RETRO_DEVICE_ID_JOYPAD_MASK */
  return id < 16 ? (m >> id) & 1 : 0;
}
static void write_ppm(const char* path) {
  FILE* o = fopen(path, "wb");
  if (!o || !frame) { if (o) fclose(o); return; }
  fprintf(o, "P6\n%u %u\n255\n", fw, fh);
  for (unsigned i = 0; i < fw * fh; i++) {
    const uint8_t rgb[3] = {(uint8_t)(frame[i] >> 16), (uint8_t)(frame[i] >> 8), (uint8_t)frame[i]};
    fwrite(rgb, 1, 3, o);
  }
  fclose(o);
}

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
  parse_keys(getenv("REF_KEYS"));
  if (getenv("REF_WAV")) wav = fopen(getenv("REF_WAV"), "wb");
  const char* shots = getenv("REF_SHOTS");
  const char* pre = getenv("REF_SHOT") ? getenv("REF_SHOT") : "ref";
  for (int f = 0; f < frames; f++) {
    curframe = f;
    retro_run();
    for (const char* q = shots; q && *q;) {
      if (atoi(q) == f + 1) {
        char path[1024];
        snprintf(path, sizeof path, "%s-%d.ppm", pre, f + 1);
        write_ppm(path);
        printf("[quadro %d: %ux%u; som L %.4f R %.4f]\n", f + 1, fw, fh, sn ? sqrt(sl / sn) / 32768 : 0.0, sn ? sqrt(sr / sn) / 32768 : 0.0);
        fflush(stdout);
        sl = sr = 0; sn = 0;
      }
      while (*q && *q != ',') q++;
      if (*q) q++;
    }
  }
  if (wav) fclose(wav);
  if (!frame) return 1;
  write_ppm(argv[5]);
  return 0;
}
