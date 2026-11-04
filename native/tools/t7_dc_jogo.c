/* Jogo de teste do Índigo para o Dreamcast (1ST_READ.BIN do disco de teste). Monta uma cena pelo Tile Accelerator
 * (store queues e DMA do canal 2) com polígonos opacos, Gouraud, texturas (565 twiddled, VQ, paletas de 4 e 8 bits,
 * linear), profundidade, punch-through, translúcido e sprite; lê o controle pelo Maple e move um quadrado; grava e
 * lê um bloco do VMU; liga o ARM7 do AICA (que chama o SH-4 por interrupção) e toca uma onda quadrada; conta
 * interrupções do TMU0; testa TRAPA; lê um arquivo do disco pelo DMA do GD-ROM. Mensagens pela SCIF. */
#include "t7_dc_hw.h"

extern const u8 arm_bin[];
extern const u32 arm_bin_len;
extern char vbr_base[];

volatile u32 tmu_ticks, arm_irqs, traps, last_tra;
static u32 sq_count;

void trap_handler(void) {
  last_tra = R32(0xFF000020);
  traps++;
}
void irq_handler(void) {
  u32 code = R32(0xFF000028);
  if (code == 0x400) {  /* TMU0 */
    R16(0xFFD80010) = (u16)(R16(0xFFD80010) & ~0x100);
    tmu_ticks++;
  } else if (code == 0x320) {  /* Holly nível 6 */
    if (ISTEXT & 2) {
      R32(0xA07028BC) = 0x20;  /* MCIRE: apaga o pedido do ARM */
      arm_irqs++;
    }
  }
}

/* ---------------------------------------------------------------- TA */
static void ta_send(const u32* w) {
  volatile u32* sq = (volatile u32*)0xE0000000;
  int i;
  for (i = 0; i < 8; i++) sq[i] = w[i];
  __asm__ volatile("pref @%0" : : "r"(0xE0000000) : "memory");
  sq_count++;
}
static u32 fbits(float f) { union { float f; u32 u; } x; x.f = f; return x.u; }

/* lista em RAM para o DMA do canal 2 */
static u32 dmabuf[512] __attribute__((aligned(32)));
static int dman;
static u32* out_target;  /* 0 = store queue, senão o buffer de DMA */
static void emit(const u32* w) {
  if (out_target) { int i; for (i = 0; i < 8; i++) dmabuf[dman * 8 + i] = w[i]; dman++; }
  else ta_send(w);
}

static void poly_hdr(u32 list, u32 pcwbits, u32 isp, u32 tsp, u32 tcw) {
  u32 w[8] = {0};
  w[0] = 0x80000000u | list << 24 | pcwbits;
  w[1] = isp; w[2] = tsp; w[3] = tcw;
  emit(w);
}
static void vtx(float x, float y, float z, float u, float v, u32 col, int last) {
  u32 w[8] = {0};
  w[0] = last ? 0xF0000000u : 0xE0000000u;
  w[1] = fbits(x); w[2] = fbits(y); w[3] = fbits(z);
  w[4] = fbits(u); w[5] = fbits(v); w[6] = col; w[7] = 0;
  emit(w);
}
static void quad(float x0, float y0, float x1, float y1, float z, u32 col) {
  vtx(x0, y0, z, 0, 0, col, 0);
  vtx(x1, y0, z, 1, 0, col, 0);
  vtx(x0, y1, z, 0, 1, col, 0);
  vtx(x1, y1, z, 1, 1, col, 1);
}
static void eol(void) { u32 w[8] = {0}; emit(w); }

#define ISP_GEQ 0xC0000000u
#define ISP_ALWAYS 0xE0000000u
#define TSP_OPAQUE (1u << 29 | 0u << 26 | 2u << 22)
#define TSP_BLEND (4u << 29 | 5u << 26 | 2u << 22 | 1u << 20)
#define TEX_8X8 0  /* U = V = 8 */
#define TW 0x00600000u  /* texturas a partir de 6 MB (endereço de 64 bits, longe do framebuffer) */

static void wr_tex16(u32 off, u32 i, u16 c) { R16(0xA4000000 + TW + off + i * 2) = c; }
static u32 tw8(u32 x, u32 y) {  /* twiddle de 8x8: y nos bits pares */
  u32 r = 0, b;
  for (b = 0; b < 3; b++) r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
  return r;
}
static u16 texcol(u32 x, u32 y) { return (u16)((x * 4) << 11 | (y * 8) << 5 | (x + y)); }

static void setup_textures(void) {
  u32 x, y;
  /* 0x0000: 565 twiddled 8x8 */
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) wr_tex16(0, tw8(x, y), texcol(x, y));
  /* 0x0800: 565 linear 8x8 */
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) wr_tex16(0x800, y * 8 + x, texcol(7 - x, y));
  /* 0x1000: VQ 16x16: livro de 256 códigos (2x2), índices 8x8 twiddled em +2048 */
  for (x = 0; x < 256; x++) {
    wr_tex16(0x1000, x * 4 + 0, (u16)(x << 8));
    wr_tex16(0x1000, x * 4 + 1, (u16)(x << 3));
    wr_tex16(0x1000, x * 4 + 2, (u16)(x << 8 | 0x1F));
    wr_tex16(0x1000, x * 4 + 3, (u16)0xFFFF);
  }
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) R8(0xA4000000 + TW + 0x1000 + 2048 + tw8(x, y)) = (u8)(y * 8 + x + 16);
  /* 0x2000: paleta de 8 bits 8x8 (índice = y*8+x), banco 1 (entradas 256..511) */
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) R8(0xA4000000 + TW + 0x2000 + tw8(x, y)) = (u8)(y * 8 + x);
  for (x = 0; x < 64; x++) R32(0xA05F9000 + (256 + x) * 4) = (u32)(0xF800 | x);
  /* 0x3000: paleta de 4 bits 8x8, banco 3 (entradas 48..63) */
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x += 2) {
    u32 t = tw8(x, y);  /* x par: o vizinho de x+1 é t+2, então monta byte a byte pelo índice */
    (void)t;
  }
  for (x = 0; x < 32; x++) R8(0xA4000000 + TW + 0x3000 + x) = 0;
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) {
    u32 t = tw8(x, y);
    u8 old = R8(0xA4000000 + TW + 0x3000 + t / 2);
    u8 v = (u8)((x + y) & 15);
    R8(0xA4000000 + TW + 0x3000 + t / 2) = (u8)((t & 1) ? (old & 0x0F) | v << 4 : (old & 0xF0) | v);
  }
  for (x = 0; x < 16; x++) R32(0xA05F9000 + (48 + x) * 4) = (u32)(x << 1);
  R32(0xA05F8108) = 1;  /* PAL_RAM_CTRL: 565 */
  /* 0x4000: 1555 twiddled 8x8 com alfa em xadrez (punch-through) */
  for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) wr_tex16(0x4000, tw8(x, y), (u16)(((x + y) & 1) ? 0x83E0 : 0x03E0));
}

static void tex_quad(float x0, float y0, u32 tcw, u32 list, u32 tspx, float sz) {
  poly_hdr(list, 0x08 | 0x02, ISP_ALWAYS, TSP_OPAQUE | tspx, tcw);
  vtx(x0, y0, 1.0f, 0, 0, 0xFFFFFFFF, 0);
  vtx(x0 + sz, y0, 1.0f, 1, 0, 0xFFFFFFFF, 0);
  vtx(x0, y0 + sz, 1.0f, 0, 1, 0xFFFFFFFF, 0);
  vtx(x0 + sz, y0 + sz, 1.0f, 1, 1, 0xFFFFFFFF, 1);
}

static void pvr_setup(void) {
  /* fundo: polígono em 0xFFF00 da área de parâmetros (VRAM de 32 bits) */
  volatile u32* bg = (volatile u32*)(0xA5000000 + 0xFFF00);
  int i;
  bg[0] = 0x90800000; bg[1] = 0x20800440; bg[2] = 0;
  for (i = 0; i < 3; i++) { bg[3 + i * 4] = 0; bg[4 + i * 4] = 0; bg[5 + i * 4] = fbits(0.0001f); bg[6 + i * 4] = 0xFF000040; }
  R32(0xA05F808C) = 1u << 24 | ((0xFFF00u >> 2) << 3);  /* ISP_BACKGND_T */
  R32(0xA05F8088) = fbits(0.0001f);                      /* ISP_BACKGND_D */
  R32(0xA05F8048) = 1;               /* FB_W_CTRL: 565 */
  R32(0xA05F8060) = 0x00200000;      /* FB_W_SOF1 */
  R32(0xA05F804C) = 640 * 2 / 8;     /* FB_W_LINESTRIDE */
  R32(0xA05F8068) = 639u << 16;      /* FB_X_CLIP */
  R32(0xA05F806C) = 479u << 16;      /* FB_Y_CLIP */
  R32(0xA05F8044) = 1 | 1 << 2;      /* FB_R_CTRL: liga, 565 */
  R32(0xA05F8050) = 0x00200000;      /* FB_R_SOF1 */
  R32(0xA05F805C) = 319 | 479u << 10 | 1u << 20;
  R32(0xA05F811C) = 0x80;            /* PT_ALPHA_REF */
  R32(0xA05F8074) = 0x100 | 0x80;    /* FPU_SHAD_SCALE: sombra simples, metade */
  R32(0xA05F80E4) = 8;               /* TEXT_CONTROL: stride 256 (não usado) */
  /* TA */
  R32(0xA05F8124) = 0x00100000;      /* TA_OL_BASE */
  R32(0xA05F8128) = 0x00000000;      /* TA_ISP_BASE */
  R32(0xA05F812C) = 0x00180000;      /* TA_OL_LIMIT */
  R32(0xA05F8130) = 0x000F0000;      /* TA_ISP_LIMIT */
  R32(0xA05F813C) = (14u << 16) | 19; /* TA_GLOB_TILE_CLIP: 20x15 tiles */
  R32(0xA05F8140) = 0x00100002;
  R32(0xA05F8164) = 0x00100000;
  R32(0xA05F8020) = 0x00000000;      /* PARAM_BASE */
  R32(0xA05F802C) = 0x00180000;      /* REGION_BASE */
  /* store queues para o TA (área 4) */
  R32(0xFF000038) = 0x10;
  R32(0xFF00003C) = 0x10;
}

static u32 sq_x = 100;

static void scene(void) {
  u32 w;
  R32(0xA05F8144) = 0x80000000u;  /* TA_LIST_INIT */
  out_target = 0;
  /* opacos */
  poly_hdr(0, 0x02, ISP_GEQ, TSP_OPAQUE, 0);
  quad(20, 20, 120, 120, 1.0f, 0xFFFF0000);
  poly_hdr(0, 0x02, ISP_GEQ, TSP_OPAQUE, 0);
  vtx(140, 120, 1.0f, 0, 0, 0xFFFF0000, 0);
  vtx(190, 20, 1.0f, 0, 0, 0xFF00FF00, 0);
  vtx(240, 120, 1.0f, 0, 0, 0xFF0000FF, 1);
  /* profundidade: azul perto (z=2) primeiro, verde longe (z=1) por cima com GEQUAL */
  poly_hdr(0, 0x02, ISP_GEQ, TSP_OPAQUE, 0);
  quad(20, 140, 120, 240, 2.0f, 0xFF0000FF);
  poly_hdr(0, 0x02, ISP_GEQ, TSP_OPAQUE, 0);
  quad(70, 160, 170, 260, 1.0f, 0xFF00FF00);
  /* texturas: 565 twiddled, linear, VQ (16x16), paleta de 8 bits, paleta de 4 bits */
  tex_quad(260, 20, 1u << 27 | ((TW + 0x0000) >> 3), 0, TEX_8X8, 64);
  tex_quad(340, 20, 1u << 27 | 1u << 26 | ((TW + 0x0800) >> 3), 0, TEX_8X8, 64);
  tex_quad(420, 20, 1u << 30 | 1u << 27 | ((TW + 0x1000) >> 3), 0, 1u << 3 | 1u, 64);
  tex_quad(500, 20, 6u << 27 | 1u << 25 | ((TW + 0x2000) >> 3), 0, TEX_8X8, 64);
  tex_quad(260, 100, 5u << 27 | 3u << 21 | ((TW + 0x3000) >> 3), 0, TEX_8X8, 64);
  /* sprite amarelo */
  {
    u32 h[8] = {0}, v[8] = {0}, v2[8] = {0};
    h[0] = 0xA0000000u; h[1] = ISP_GEQ; h[2] = TSP_OPAQUE; h[3] = 0; h[4] = 0xFFFFFF00; h[5] = 0;
    emit(h);
    v[0] = 0xF0000000u;
    v[1] = fbits(400); v[2] = fbits(300); v[3] = fbits(1.0f);
    v[4] = fbits(450); v[5] = fbits(300); v[6] = fbits(1.0f);
    v[7] = fbits(450);
    v2[0] = fbits(350); v2[1] = fbits(1.0f); v2[2] = fbits(400); v2[3] = fbits(350);
    emit(v); emit(v2);
  }
  /* quadrado do controle */
  poly_hdr(0, 0x02, ISP_GEQ, TSP_OPAQUE, 0);
  quad((float)sq_x, 400, (float)sq_x + 16, 416, 1.0f, 0xFFFFFFFF);
  /* cinza que aceita sombra (bit 7 do PCW) */
  poly_hdr(0, 0x02 | 0x80, ISP_GEQ, TSP_OPAQUE, 0);
  quad(180, 300, 260, 380, 1.0f, 0xFF808080);
  eol();
  /* volume modificador (lista 1): face da frente (z=3) e de trás (z=0,5) de uma caixa em 200..240 x 320..360 */
  {
    static const float f[4][9] = {
      {200, 320, 3, 240, 320, 3, 200, 360, 3}, {240, 320, 3, 240, 360, 3, 200, 360, 3},
      {200, 320, 0.5f, 240, 320, 0.5f, 200, 360, 0.5f}, {240, 320, 0.5f, 240, 360, 0.5f, 200, 360, 0.5f}};
    int t, k;
    for (t = 0; t < 4; t++) {
      u32 g[8] = {0}, v[16] = {0};
      if (t == 0 || t == 3) {  /* o último polígono leva a instrução 1 (dentro) */
        g[0] = 0x80000000u | 1u << 24; g[1] = t == 3 ? 1u << 29 : 0;
        emit(g);
      }
      v[0] = 0xF0000000u;
      for (k = 0; k < 9; k++) v[1 + k] = fbits(f[t][k]);
      emit(v); emit(v + 8);
    }
  }
  eol();
  /* punch-through: 1555 com alfa em xadrez */
  tex_quad(340, 100, 0u << 27 | ((TW + 0x4000) >> 3), 4, 1u << 20, 64);
  eol();
  /* translúcido pelo DMA do canal 2: verde com metade de alfa por cima do vermelho */
  out_target = dmabuf; dman = 0;
  poly_hdr(2, 0x02, ISP_GEQ, TSP_BLEND, 0);
  quad(60, 60, 110, 110, 1.5f, 0x8000FF00);
  eol();
  out_target = 0;
  w = (u32)dmabuf & 0x1FFFFFFF;
  R32(0xFFA00020) = w;                /* SAR2 */
  R32(0xFFA00028) = (u32)dman;        /* DMATCR2 (unidades de 32 bytes) */
  R32(0xFFA0002C) = 0x12C1;           /* CHCR2 */
  R32(0xFFA00040) = 0x8201;           /* DMAOR */
  R32(0xA05F6800) = 0x10000000;       /* SB_C2DSTAT */
  R32(0xA05F6804) = (u32)dman * 32;   /* SB_C2DLEN */
  R32(0xA05F6808) = 1;                /* SB_C2DST */
  while (R32(0xA05F6808) & 1) {}
  /* desenha */
  R32(0xA05F8014) = 1;
  while (!(ISTNRM & 4)) {}
  ISTNRM = 0x7 | 0x780 | 0x200000 | (1u << 19);
}

/* ---------------------------------------------------------------- Maple */
static u32 mbuf[64] __attribute__((aligned(32)));
static u32 mrecv[160] __attribute__((aligned(32)));
static void maple_run(void) {
  R32(0xA05F6C04) = (u32)mbuf & 0x1FFFFFFF;
  R32(0xA05F6C10) = 0;
  R32(0xA05F6C14) = 1;
  R32(0xA05F6C18) = 1;
  while (R32(0xA05F6C18) & 1) {}
}
static u32 read_pad(void) {
  mbuf[0] = 0x80000000u | 1;
  mbuf[1] = (u32)mrecv & 0x1FFFFFFF;
  mbuf[2] = 9 | 0x20u << 8 | 0x00u << 16 | 1u << 24;
  mbuf[3] = 0x01000000;
  mrecv[0] = 0;
  maple_run();
  if ((mrecv[0] & 0xFF) != 8) return 0xFFFFFFFF;
  return mrecv[2];
}
static int vmu_test(void) {
  int i;
  mbuf[0] = 0x80000000u | 34;
  mbuf[1] = (u32)mrecv & 0x1FFFFFFF;
  mbuf[2] = 0xC | 0x01u << 8 | 0x00u << 16 | 34u << 24;
  mbuf[3] = 0x02000000;
  mbuf[4] = 5u << 24;  /* bloco 5, fase 0 */
  for (i = 0; i < 32; i++) mbuf[5 + i] = 0x494E4400u + (u32)i;
  maple_run();
  if ((mrecv[0] & 0xFF) != 7) return 0;
  mbuf[0] = 0x80000000u | 2;
  mbuf[2] = 0xB | 0x01u << 8 | 0x00u << 16 | 2u << 24;
  mbuf[4] = 5u << 24;
  maple_run();
  if ((mrecv[0] & 0xFF) != 8) return 0;
  for (i = 0; i < 32; i++) if (mrecv[3 + i] != 0x494E4400u + (u32)i) return 0;
  /* informação do aparelho principal: nome do controle */
  mbuf[0] = 0x80000000u;
  mbuf[2] = 1 | 0x20u << 8;
  maple_run();
  if ((mrecv[0] & 0xFF) != 5 || memcmp_((u8*)mrecv + 4 + 18, "Dreamcast Controller", 20)) return 0;
  return 1;
}

/* ---------------------------------------------------------------- AICA */
static void aica_test(void) {
  u32 i;
  R32(0xA0702C00) = 1;  /* ARM parado */
  for (i = 0; i < (arm_bin_len + 3) / 4; i++) R32(0xA0800000 + i * 4) = ((const u32*)arm_bin)[i];
  /* onda quadrada de 16 bits em 0x20000: período de 100 amostras */
  for (i = 0; i < 1000; i++) R16(0xA0820000 + i * 2) = (u16)(((i % 100) < 50) ? 8000 : -8000);
  R32(0xA0702800) = 15;           /* volume geral */
  R32(0xA0700000) = 0x4000 | 0x0200 | 0x0002;  /* KYONB, laço, PCM16, SA alto = 2 */
  R32(0xA0700004) = 0;
  R32(0xA0700008) = 0;
  R32(0xA070000C) = 1000;
  R32(0xA0700010) = 0x001F;       /* AR = 31 */
  R32(0xA0700014) = 0x001F;
  R32(0xA0700018) = 0;            /* OCT 0, FNS 0: 44,1 kHz */
  R32(0xA0700024) = 0x0F00;       /* DISDL 15, centro */
  R32(0xA0700028) = 0;
  R32(0xA0700000) = 0x8000 | 0x4000 | 0x0200 | 0x0002;  /* KYONEX */
  R32(0xA07028B4) = 0x20;         /* MCIEB: o ARM pode chamar o SH-4 */
  IML6EXT = 2;
  R32(0xA0702C00) = 0;            /* solta o ARM */
}

/* ---------------------------------------------------------------- principal */
static u8 secbuf[4096] __attribute__((aligned(32)));

int main(void) {
  u32 frame, pad, old = 0xFFFF, dfad, dsize;
  int vmu;
  puts_("JOGO: comecou\n");
  __asm__ volatile("ldc %0, vbr" : : "r"(vbr_base));
  pvr_setup();
  setup_textures();
  /* TRAPA */
  __asm__ volatile("trapa #0x22");
  puts_("JOGO: trapa "); putdec(traps); puts_(" tra "); puthex(last_tra); puts_("\n");
  /* TMU0 a 1 kHz, prioridade 10 */
  R16(0xFFD80010) = 0x20;              /* TCR0: UNIE, Pφ/4 */
  R32(0xFFD80008) = 12500 - 1;         /* TCOR0 */
  R32(0xFFD8000C) = 12500 - 1;         /* TCNT0 */
  R16(0xFFD00004) = 0xA000;            /* IPRA */
  R8(0xFFD80004) = 1;                  /* TSTR */
  {
    u32 sr;
    __asm__ volatile("stc sr, %0" : "=r"(sr));
    sr &= ~0xF0u;
    __asm__ volatile("ldc %0, sr" : : "r"(sr));
  }
  vmu = vmu_test();
  puts_(vmu ? "JOGO: vmu ok\n" : "JOGO: vmu falhou\n");
  /* arquivo do disco pelo DMA do GD-ROM */
  {
    u32 fad = gd_data_fad(1);
    if (fad && iso_find(fad, "DADOS.BIN", &dfad, &dsize, secbuf) && gd_read(dfad, 2, secbuf, 1) == 4096) {
      u32 i, ok = 1;
      for (i = 0; i < 4096; i++) if (secbuf[i] != (u8)(i * 7 + 3)) ok = 0;
      puts_(ok ? "JOGO: dma do gd ok\n" : "JOGO: dma do gd com dados errados\n");
    } else {
      puts_("JOGO: dma do gd falhou\n");
    }
  }
  /* conversor YUV: um macrobloco 16x16 em YUV420 para TW + 0x8000 */
  {
    static u8 mb[384] __attribute__((aligned(32)));
    u32 i, k;
    for (i = 0; i < 64; i++) { mb[i] = (u8)(0x40 + i); mb[64 + i] = 0x80; }
    for (i = 0; i < 256; i++) mb[128 + i] = (u8)i;
    R32(0xA05F814C) = 0;               /* TA_YUV_TEX_CTRL: 1x1 macrobloco, YUV420 */
    R32(0xA05F8148) = TW + 0x8000;     /* TA_YUV_TEX_BASE */
    ISTNRM = 0x40;
    for (k = 0; k < 12; k++) {
      volatile u32* sq = (volatile u32*)0xE0800000;
      for (i = 0; i < 8; i++) sq[i] = ((const u32*)mb)[k * 8 + i];
      __asm__ volatile("pref @%0" : : "r"(0xE0800000) : "memory");
    }
    puts_((ISTNRM & 0x40) ? "JOGO: yuv ok\n" : "JOGO: yuv sem interrupcao\n");
    ISTNRM = 0x40;
  }
  aica_test();
  for (frame = 0;; frame++) {
    while (!(ISTNRM & 8)) {}
    ISTNRM = 8;
    pad = read_pad();
    if (!(pad & 0x80) && sq_x < 600) sq_x += 2;  /* direita */
    if (!(pad & 0x40) && sq_x > 2) sq_x -= 2;    /* esquerda */
    if ((pad & 0xFFFF) != old) { old = pad & 0xFFFF; puts_("JOGO: botoes "); puthex(old); puts_("\n"); }
    scene();
    if (frame == 30) {
      puts_("JOGO: tmu "); putdec(tmu_ticks);
      puts_(" arm "); puthex(R32(0xA0810000)); puts_(" "); putdec(arm_irqs);
      puts_(" sq "); putdec(sq_count); puts_("\n");
    }
    if ((frame & 15) == 0) { puts_("JOGO: quadro "); putdec(frame); puts_(" x "); putdec(sq_x); puts_("\n"); }
  }
  return 0;
}
