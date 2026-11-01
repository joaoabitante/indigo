#!/bin/bash
# Prepara os testes do núcleo Virtual Boy em /root/indigo-env/roms/vb (ou na pasta dada):
#  1. homebrews públicas (VBeat, VBeatJukebox, Doom para Virtual Boy, compilado da fonte);
#  2. compilador v810-gcc (pasta linux/gcc do VUEngine Studio Tools) e as ROMs de teste próprias
#     (cputeste.vb, vipteste.vb) e o resultado esperado do cputeste calculado no PC;
#  3. régua: núcleo Mednafen (beetle-vb, libretro) com um frontend mínimo que grava os quadros dos dois
#     olhos (*.ref), comparados ponto a ponto pelo indigo-t7_vb.
# Precisa de git, make, gcc e g++. Leva alguns minutos (o Doom e o beetle-vb são compilados).
set -e
D=${1:-/root/indigo-env/roms/vb}
mkdir -p "$D"
cd "$D"

pega() {  # pega <pasta> <url> <commit>
  if [ ! -d "$1/.git" ]; then
    git init -q "$1"
    git -C "$1" fetch -q --depth 1 "$2" "$3"
    git -C "$1" checkout -q FETCH_HEAD
  fi
}
pega VBeatJukebox https://github.com/enthusi/VBeatJukebox e1ce97a85ff84aa01f7d2379b6241274cf84c8d8
pega vbeat https://github.com/enthusi/VBeat 4dd77da7abf526c4728bec3af20e9d21d4b76ea7
pega doom https://github.com/BrutalTurok/Doom-virtual-boy 878ff51d2404d8eb240e78d8f6b547484d160fb6
pega beetle-vb https://github.com/libretro/beetle-vb-libretro d5ae54c8e23b573a3b34e888b2d48a588027e92c
if [ ! -x vutools/linux/gcc/bin/v810-gcc ]; then
  rm -rf vutools
  git init -q vutools
  git -C vutools remote add origin https://github.com/VUEngine/vuengine-studio-tools
  git -C vutools config core.sparseCheckout true
  echo "linux/gcc/" > vutools/.git/info/sparse-checkout
  git -C vutools fetch -q --depth 1 --filter=blob:none origin 7c07ca15fff85166213cdc66f1d10c2e77b3ae9e
  git -C vutools checkout -q FETCH_HEAD
  chmod -R +x vutools/linux/gcc/bin vutools/linux/gcc/libexec vutools/linux/gcc/v810/bin 2>/dev/null || true
fi
TC="$D/vutools/linux/gcc/bin"

[ -f doom/doom_vb.vb ] || make -s -C doom -j8 TC="$TC" > doom/compilacao.log 2>&1

mkdir -p teste
cd teste
cat > crt0.s <<'FIM_DO_ARQUIVO'
/* inicialização mínima das ROMs de teste do Índigo (Virtual Boy) */
	.section .text
	.align	1
	.global	_start
_start:
	movhi	hi(__stack), r0, sp
	movea	lo(__stack), sp, sp
	movhi	hi(__gp), r0, gp
	movea	lo(__gp), gp, gp
	movhi	hi(__data_lma), r0, r6
	movea	lo(__data_lma), r6, r6
	movhi	hi(__data_start), r0, r7
	movea	lo(__data_start), r7, r7
	movhi	hi(__data_end), r0, r8
	movea	lo(__data_end), r8, r8
1:	cmp	r8, r7
	bnl	2f
	ld.b	0[r6], r9
	st.b	r9, 0[r7]
	add	1, r6
	add	1, r7
	br	1b
2:	movhi	hi(__bss_end), r0, r8
	movea	lo(__bss_end), r8, r8
3:	cmp	r8, r7
	bnl	4f
	st.b	r0, 0[r7]
	add	1, r7
	br	3b
4:	jal	_main
5:	br	5b

	.section .vectors, "ax"
	.align	1
_reset:
	movhi	hi(_start), r0, r1
	movea	lo(_start), r1, r1
	jmp	[r1]
FIM_DO_ARQUIVO
cat > vb.ld <<'FIM_DO_ARQUIVO'
OUTPUT_FORMAT("elf32-v810", "elf32-v810", "elf32-v810")
OUTPUT_ARCH(v810)
ENTRY(_start)
MEMORY
{
  wram (!r) : ORIGIN = 0x05000000, LENGTH = 48k
  rom (rx)  : ORIGIN = 0x07000000, LENGTH = 64k
}
__stack = 0x0500BFF0;
SECTIONS
{
  .text 0x07000000 : { *(.text*) *(.rodata*) } >rom = 0xFF
  __data_lma = .;
  .data 0x05000000 : AT(__data_lma) { __data_start = .; *(.data*) *(.sdata*) __data_end = .; } >wram
  __gp = __data_start + 0x8000;
  .bss (NOLOAD) : { *(.sbss*) *(.bss*) *(COMMON) __bss_end = .; } >wram
  .vectors 0x0700FFF0 : AT(0x0700FFF0) { KEEP(*(.vectors)) . = 0x10; } >rom = 0xFF
}
FIM_DO_ARQUIVO
cat > cputeste.c <<'FIM_DO_ARQUIVO'
/* Teste de CPU do Índigo para o Virtual Boy: faz milhares de contas de inteiros (32 e 64 bits), divisões,
   deslocamentos, ponto flutuante simples (ADDF/SUBF/MULF/DIVF/CMPF/CVT/TRNC)
   com entradas pseudoaleatórias e guarda os resultados. O mesmo arquivo compilado no PC (-DHOST) gera os
   valores esperados: as duas CPUs seguem as mesmas regras de C e do IEEE 754. */
#include <stdint.h>
#ifdef HOST
#include <math.h>
#include <stdio.h>
/* o v810-gcc 4.7 converte real -> inteiro com CVT.SW (arredonda para o mais próximo), não com TRNC.SW */
#define F2I(x) ((int32_t)nearbyintf(x))
#include <string.h>
static uint32_t res_buf[4096];
#define RES res_buf
#else
#define RES ((volatile uint32_t*)0x0500C000)
#define DONE (*(volatile uint32_t*)0x0500BFF8)
#define F2I(x) ((int32_t)(x))
#endif

static uint32_t seed = 0x12345678;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static uint32_t n;
static void put(uint32_t v) { RES[n++ & 4095] = v; }
static uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }
/* real "comportado": sem infinito, NaN ou desnormalizado (no V810 seriam operandos reservados) */
static float rf(void) {
  uint32_t r = rnd();
  float m = (float)(int32_t)(r & 0xFFFFFF) / 4096.0f;
  if (r & 0x1000000) m = -m;
  return m * (float)(1 << ((r >> 26) & 15)) / 1024.0f + 0.5f;
}

int main(void) {
  volatile int32_t vs[2];
  volatile uint32_t vu[2];
  n = 0;
  for (int i = 0; i < 100; i++) {
    uint32_t a = rnd(), b = rnd();
    if (i & 1) b &= 0xFFFF;
    if ((i & 7) == 3) b = 0xFFFFFFFF;
    vu[0] = a; vu[1] = b ? b : 1;
    vs[0] = (int32_t)a; vs[1] = (int32_t)(b ? b : 3);
    uint32_t ua = vu[0], ub = vu[1];
    int32_t sa = vs[0], sb = vs[1];
    if (sa == (int32_t)0x80000000 && sb == -1) sb = 7;
    put(ua + ub); put(ua - ub); put(ua * ub); put(ua / ub); put(ua % ub);
    put((uint32_t)(sa / sb)); put((uint32_t)(sa % sb));
    put(ua << (ub & 31)); put(ua >> (ub & 31)); put((uint32_t)(sa >> (ub & 31)));
    put(ua & ub); put(ua | ub); put(ua ^ ub); put(~ua);
    put((uint32_t)(sa < sb) | (uint32_t)(ua < ub) << 1 | (uint32_t)(sa <= sb) << 2 | (uint32_t)(ua >= ub) << 3);
    uint64_t p = (uint64_t)ua * ub;
    int64_t q = (int64_t)sa * sb;
    put((uint32_t)p); put((uint32_t)(p >> 32)); put((uint32_t)q); put((uint32_t)((uint64_t)q >> 32));
    put((uint32_t)(p / (ub | 1))); put((uint32_t)((int64_t)q % 1000003));
    put((uint32_t)(int8_t)a); put((uint32_t)(int16_t)b); put((a >> 16) | (b << 16));
  }
  for (int i = 0; i < 120; i++) {
    volatile float vf[2];
    vf[0] = rf(); vf[1] = rf();
    float x = vf[0], y = vf[1];
    if (y == 0.0f) y = 1.5f;
    put(fbits(x + y)); put(fbits(x - y)); put(fbits(x * y)); put(fbits(x / y));
    put((uint32_t)(x < y) | (uint32_t)(x == y) << 1 | (uint32_t)(x >= y) << 2);
    put((uint32_t)F2I(x * 16.0f));
    put(fbits((float)(int32_t)rnd()));            /* CVT.WS (com arredondamento) */
    put(fbits((float)(int32_t)(rnd() >> 8) / 3.0f));
    float z = x * y + x / y - y;
    put(fbits(z)); put((uint32_t)F2I(z / 65536.0f));
    put(fbits(z * 0.001f - x)); put(fbits((x + 1.0f) / (y * y + 1.0f)));
  }
  put(n);
#ifdef HOST
  FILE* f = fopen(HOST, "wb");
  uint32_t m = n;
  fwrite(res_buf, 4, m, f);
  fclose(f);
#else
  /* mostra os resultados como pontos no olho esquerdo (para comparar com o Mednafen pela tela) */
  volatile uint8_t* fb = (volatile uint8_t*)0x00000000;
  for (uint32_t k = 0; k < 0x6000; k++) fb[k] = ((volatile uint8_t*)RES)[k % (n * 4)];
  *(volatile uint16_t*)0x0005F824 = 0x20;  /* BRTA */
  *(volatile uint16_t*)0x0005F826 = 0x40;  /* BRTB */
  *(volatile uint16_t*)0x0005F828 = 0x20;  /* BRTC */
  *(volatile uint16_t*)0x0005F822 = 0x0302; /* DPCTRL: exibição ligada */
  DONE = 0x454E4F44; /* "DONE" */
#endif
  return 0;
}
FIM_DO_ARQUIVO
cat > vipteste.c <<'FIM_DO_ARQUIVO'
/* Teste do VIP do Índigo: mundos normal (com paralaxe e mapa 2x1), H-bias, afim (rotação e escala, com
   "overplane"), dois grupos de objetos (espelhados, paletas, paralaxe, parcialmente fora da tela), janela com X
   negativo, cor de fundo, brilho e repetição da tabela de colunas, tudo animado quadro a quadro. */
#include <stdint.h>

#define REG(o) (*(volatile uint16_t*)(0x0005F800 + (o)))
#define CHR ((volatile uint16_t*)0x00078000)
#define BGMAP(n) ((volatile uint16_t*)(0x00020000 + (n) * 0x2000))
#define WORLD(n) ((volatile uint16_t*)(0x0003D800 + (n) * 32))
#define CLM ((volatile uint16_t*)0x0003DC00)
#define OAM ((volatile uint16_t*)0x0003E000)
#define PARAM_BASE 0xA000 /* meia palavra na DRAM = endereço 0x00034000 */
#define PARAM ((volatile int16_t*)0x00034000)

/* seno em ponto fixo 1.9 (512 = 1,0) para 64 passos de volta */
static const int16_t SIN[16] = {0, 50, 100, 147, 196, 241, 284, 324, 362, 396, 426, 452, 473, 490, 502, 510};
static int sin64(int a) {
  a &= 63;
  if (a < 16) return SIN[a];
  if (a < 32) return a == 16 ? 512 : SIN[32 - a];
  if (a < 48) return -SIN[a - 32];
  return a == 48 ? -512 : -SIN[64 - a];
}

static void world(int w, uint16_t head, int gx, int gp, int gy, int mx, int mp, int my, int width, int height,
                  uint16_t param, uint16_t over) {
  volatile uint16_t* p = WORLD(w);
  p[0] = head; p[1] = (uint16_t)gx; p[2] = (uint16_t)gp; p[3] = (uint16_t)gy; p[4] = (uint16_t)mx;
  p[5] = (uint16_t)mp; p[6] = (uint16_t)my; p[7] = (uint16_t)width; p[8] = (uint16_t)height; p[9] = param;
  p[10] = over;
}

int main(void) {
  REG(0x02) = 0;      /* INTENB */
  REG(0x04) = 0xFFFF; /* INTCLR */
  /* 64 caracteres com desenhos variados */
  for (int t = 0; t < 64; t++)
    for (int r = 0; r < 8; r++) {
      uint16_t v = 0;
      for (int x = 0; x < 8; x++) {
        int c = ((x * (t + 1) + r * (t + 3)) >> 2) & 3;
        if (t < 4) c = (x == r || x == 7 - r) ? 3 : (t & 3);
        v |= (uint16_t)(c << (x * 2));
      }
      CHR[t * 8 + r] = v;
    }
  /* mapas 0 a 3 (64x64 células cada) */
  for (int m = 0; m < 4; m++)
    for (int i = 0; i < 64 * 64; i++) {
      int x = i & 63, y = i >> 6;
      uint16_t ch = (uint16_t)(((x * 7 + y * 3 + m * 11) & 63));
      uint16_t pal = (uint16_t)(((x + y + m) & 3) << 14);
      uint16_t fl = (uint16_t)((((x >> 2) & 1) << 13) | (((y >> 3) & 1) << 12));
      BGMAP(m)[i] = ch | pal | fl;
    }
  REG(0x60) = 0xE4; REG(0x62) = 0x93; REG(0x64) = 0x4C; REG(0x66) = 0x1C;
  REG(0x68) = 0xE4; REG(0x6A) = 0x6C; REG(0x6C) = 0xB4; REG(0x6E) = 0x38;
  REG(0x24) = 0x18; REG(0x26) = 0x30; REG(0x28) = 0x28; REG(0x2A) = 0x00;
  REG(0x70) = 1; /* BKCOL */
  for (int i = 0; i < 256; i++) CLM[i] = (uint16_t)(((i / 24) % 3) << 8 | 0xE0);
  REG(0x48) = 6; REG(0x4A) = 14; REG(0x4C) = 30; REG(0x4E) = 52; /* SPT0..3 */
  REG(0x2E) = 0;      /* FRMCYC */
  REG(0x22) = 0x0302; /* DPCTRL: exibição */
  REG(0x42) = 0x0002; /* XPCTRL: desenho */
  for (int f = 0;; f++) {
    /* espera o começo do próximo quadro do jogo */
    while (!(REG(0x00) & 0x0008)) {}
    REG(0x04) = 0x0008;
    /* mundo 31: fundo normal em mapa 2x1 (segmentos 0 e 1) com paralaxe */
    world(31, 0xC000 | (0 << 12) | (1 << 10) | 0, 0, 2, 0, f * 3, 3, f, 383, 223, 0, 0);
    /* mundo 30: H-bias no mapa 2 */
    world(30, 0xC000 | (1 << 12) | 2, 24, -1, 30, f, 0, 5, 220, 63, PARAM_BASE, 0);
    for (int y = 0; y < 64; y++) {
      PARAM[y * 2] = (int16_t)(sin64(y * 2 + f) / 32);
      PARAM[y * 2 + 1] = (int16_t)(-sin64(y * 3 + f) / 40);
    }
    /* mundo 29: afim no mapa 3 com "overplane" (célula 5 do mapa 0), rotação e escala */
    {
      const int a = f, s = 384 + sin64(f * 2) / 2;
      const int c = sin64(a + 16) * s / 512, sn = sin64(a) * s / 512;
      volatile int16_t* ap = PARAM + 0x100;
      for (int y = 0; y <= 110; y++) {
        int yy = y - 55;
        ap[y * 8 + 0] = (int16_t)((256 * 8) + ((-90 * c - yy * sn) >> 6)); /* MX (13.3) */
        ap[y * 8 + 1] = (int16_t)((f & 7) - 3);                              /* MP */
        ap[y * 8 + 2] = (int16_t)((256 * 8) + ((-90 * sn + yy * c) >> 6)); /* MY (13.3) */
        ap[y * 8 + 3] = (int16_t)c;                                          /* DX (7.9) */
        ap[y * 8 + 4] = (int16_t)sn;                                         /* DY (7.9) */
      }
    }
    world(29, 0xC000 | 0x80 | (2 << 12) | 3, 140 + (f & 31), 1, 90, 0, 0, 0, 180, 110, PARAM_BASE + 0x100, 5);
    /* mundos 28 e 27: grupos de objetos 3 e 2 */
    world(28, 0xC000 | (3 << 12), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    world(27, 0x8000 | (3 << 12), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    for (int o = 0; o < 53; o++) {
      volatile uint16_t* p = OAM + o * 4;
      int x = (o * 37 + f * (1 + (o & 3))) % 420 - 20;
      int y = (o * 23 + f * 2) % 240 - 8;
      p[0] = (uint16_t)(x & 0x3FF);
      p[1] = (uint16_t)(0xC000 | ((o & 3) - 1) & 0x3FFF) | ((o % 5 == 0) ? 0x8000 : 0xC000);
      p[2] = (uint16_t)(y & 0xFF);
      p[3] = (uint16_t)((o & 63) | ((o & 3) << 14) | ((o & 4) << 11) | ((o & 8) << 9));
    }
    /* mundo 26: normal com X negativo, janela pequena, mapa 1x2 (segmentos 2 e 3) */
    world(26, 0x4000 | 0x80 | (1 << 8) | 2, -40 + (f & 63), 0, 150, f, 0, 300 + f, 200, 50, 0, 7);
    world(25, 0x8000 | 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); /* END */
  }
  return 0;
}
FIM_DO_ARQUIVO
cat > refvb.c <<'FIM_DO_ARQUIVO'
// Frontend mínimo de libretro: roda a ROM no núcleo de referência e grava o canal vermelho dos dois olhos
// (lado a lado, 768x224) a cada "passo" quadros. uso: refvb nucleo.so rom.vb quadros passo saida.bin
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const void* g_fb; static unsigned g_w, g_h; static size_t g_pitch;
static unsigned g_frame;

static void logf_(enum retro_log_level l, const char* fmt, ...) { (void)l; (void)fmt; }
static bool env(unsigned cmd, void* data) {
  switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return *(enum retro_pixel_format*)data == RETRO_PIXEL_FORMAT_XRGB8888;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable* v = data;
      if (!strcmp(v->key, "vb_3dmode")) { v->value = "side-by-side"; return true; }
      if (!strcmp(v->key, "vb_cpu_emulation")) { v->value = "accurate"; return true; }
      if (!strcmp(v->key, "vb_color_mode")) { v->value = "black & red"; return true; }
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
static size_t audioBatch(const int16_t* d, size_t n) { (void)d; return n; }
static void poll(void) {}
// mesmo roteiro de teclas do teste do Índigo: A, depois START, alternando a cada segundo
static int16_t input(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void)dev; (void)idx;
  if (port) return 0;
  const unsigned phase = (g_frame / 50) % 4;
  if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (phase == 1 ? 1 << RETRO_DEVICE_ID_JOYPAD_A : 0) | (phase == 3 ? 1 << RETRO_DEVICE_ID_JOYPAD_START : 0);
  if (id == RETRO_DEVICE_ID_JOYPAD_A) return phase == 1;
  if (id == RETRO_DEVICE_ID_JOYPAD_START) return phase == 3;
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 6) return 2;
  void* h = dlopen(argv[1], RTLD_NOW);
  if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
#define SYM(n) typeof(&n) p_##n = (typeof(&n))dlsym(h, #n)
  SYM(retro_set_environment); SYM(retro_set_video_refresh); SYM(retro_set_audio_sample); SYM(retro_set_audio_sample_batch);
  SYM(retro_set_input_poll); SYM(retro_set_input_state); SYM(retro_init); SYM(retro_load_game); SYM(retro_run);
  p_retro_set_environment(env); p_retro_init();
  p_retro_set_video_refresh(video); p_retro_set_audio_sample(audio); p_retro_set_audio_sample_batch(audioBatch);
  p_retro_set_input_poll(poll); p_retro_set_input_state(input);
  FILE* f = fopen(argv[2], "rb"); if (!f) return 1;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  void* rom = malloc(n); if (fread(rom, 1, n, f) != (size_t)n) return 1; fclose(f);
  struct retro_game_info gi = {argv[2], rom, (size_t)n, NULL};
  if (!p_retro_load_game(&gi)) { fprintf(stderr, "load falhou\n"); return 1; }
  const int frames = atoi(argv[3]), step = atoi(argv[4]);
  FILE* out = fopen(argv[5], "wb");
  for (g_frame = 0; g_frame < (unsigned)frames; g_frame++) {
    p_retro_run();
    if ((g_frame + 1) % step == 0) {
      for (unsigned y = 0; y < 224; y++)
        for (unsigned x = 0; x < 768; x++) {
          uint8_t v = 0;
          if (g_fb && x < g_w && y < g_h) v = (uint8_t)(((const uint32_t*)((const uint8_t*)g_fb + y * g_pitch))[x] >> 16);
          fputc(v, out);
        }
    }
  }
  fclose(out);
  return 0;
}
FIM_DO_ARQUIVO
"$TC/v810-as" -o crt0.o crt0.s
for p in cputeste vipteste; do
  "$TC/v810-gcc" -O2 -mv810 -std=gnu99 -ffreestanding -c $p.c -o $p.o
  "$TC/v810-gcc" -nostartfiles -T vb.ld crt0.o $p.o -o $p.elf -Wl,--start-group -lm -lc -lgcc -Wl,--end-group
  "$TC/v810-objcopy" -O binary $p.elf ../$p.vb
done
gcc -O2 -ffp-contract=off -DHOST='"../cputeste.esperado"' cputeste.c -o cputeste_pc -lm
./cputeste_pc
gcc -O2 -I../beetle-vb/libretro-common/include refvb.c -o refvb -ldl
cd ..

make -s -C beetle-vb -j8 > beetle-vb/compilacao.log 2>&1
R=beetle-vb/mednafen_vb_libretro.so
# quadros e intervalo de cada comparação (o indigo-t7_vb usa a mesma tabela)
teste/refvb $R VBeatJukebox/vbeatjukebox.vb 3000 25 vbeatjukebox.ref
teste/refvb $R vbeat/VBeat16k_PriorArt.vb 3000 25 priorart.ref
teste/refvb $R doom/doom_vb.vb 3000 25 doom.ref
teste/refvb $R cputeste.vb 50 10 cputeste.ref
teste/refvb $R vipteste.vb 600 3 vipteste.ref
echo "testes do Virtual Boy prontos em $D"
