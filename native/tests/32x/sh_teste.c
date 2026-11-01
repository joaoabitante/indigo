/* Programas de teste do 32X do Índigo (lado dos SH-2). Compilados pelo tools/t7_32x.sh por cima do exemplo
 * 32x-skeleton do Marsdev (licença MIT), um por valor de TESTE, e comparados quadro a quadro com o PicoDrive.
 * Cada teste desenha uma tela que só depende do número de quadros desde a partida (nada de medir tempo):
 *   1 packed (256 cores): faixas da paleta, retângulos com e sem prioridade sobre o texto do Mega Drive,
 *     deslocamento de 1 ponto (SFT) e preenchimento automático;
 *   2 direct color (32768 cores) com prioridade por ponto e PRI invertido;
 *   3 run-length;
 *   4 contas nos SH-2: divisor (32/32 e 64/32), DMA interno (cópia automática), MAC/DMULS, cache ligada, o escravo
 *     desenhando metade da tela com troca de mensagens pelos registradores COMM;
 *   5 som PWM pela interrupção do PWM (tom e varredura), com a VINT do SH-2 ligada;
 *   6 FIFO de DREQ: o 68000 manda dados pela FIFO, o DMA do mestre põe na SDRAM e o mestre desenha;
 *   7 animação: quadros alternados (troca na volta vertical) seguindo a contagem de quadros do 68000, com a VINT
 *     e a HINT (toda linha) do SH-2 contando. */
#include "mars.h"

#ifndef TESTE
#define TESTE 1
#endif

#define FB8 ((volatile uint8_t*)&MARS_FRAMEBUFFER)
#define FB16 (&MARS_FRAMEBUFFER)
#define PAL (&MARS_CRAM)
#define VBR_M ((volatile uint32_t*)0x26000000)
#define VBR_S ((volatile uint32_t*)0x26000120)

static volatile int fbCur = 0;
volatile uint32_t vcount = 0, hcount = 0, pwmIrqs = 0;
volatile uint16_t slaveDone = 0;

/* fonte de 3x5 pontos para os dígitos hexadecimais */
static const uint16_t FONT[16] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
                                  0x7BEF, 0x7BCF, 0x7BED, 0x6BAE, 0x7927, 0x6B6E, 0x79E7, 0x79E4};

static void flip(void) {
  MARS_VDP_FBCTL = fbCur ^ 1;
  while ((MARS_VDP_FBCTL & MARS_VDP_FS) == fbCur) {}
  fbCur ^= 1;
}

/* contagem de quadros do 68000 (COMM12, somada na volta vertical do Mega Drive): espera a próxima e devolve.
 * Assim a animação só depende dos quadros desde a partida do 68000, e não de quando os SH-2 começaram. */
static uint32_t lastTick = 0;
static uint32_t tick(void) {
  while (MARS_SYS_COMM12 == lastTick) {}
  lastTick = MARS_SYS_COMM12;
  return lastTick;
}

static void lineTable(int words) {
  for (int i = 0; i < 256; i++) FB16[i] = (uint16_t)(0x100 + i * words);
}

/* modo packed: um ponto de 8 bits */
static void px8(int x, int y, int c) { FB8[0x200 + y * 320 + x] = (uint8_t)c; }
static void rect8(int x0, int y0, int w, int h, int c) {
  for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) px8(x, y, c);
}
static void hex8(int x, int y, uint32_t v, int digits, int c) {
  for (int d = digits - 1; d >= 0; d--, x += 4) {
    const uint16_t g = FONT[(v >> (d * 4)) & 15];
    for (int r = 0; r < 5; r++)
      for (int k = 0; k < 3; k++) px8(x + k, y + r, (g >> (14 - r * 3 - k)) & 1 ? c : 0);
  }
}

static void setVector(volatile uint32_t* vbr, int n, void (*f)(void)) { vbr[n] = (uint32_t)f; }

/* ------------------------------------------------------------------ interrupções */
void __attribute__((interrupt_handler)) vintM(void) {
  MARS_SYS_VINT_CLR = 0;
  vcount++;
}
void __attribute__((interrupt_handler)) hintM(void) {
  MARS_SYS_HINT_CLR = 0;
  hcount++;
}

#if TESTE == 5
static volatile int phase = 0;
static volatile int ampL = 0, ampR = 0;  /* volume de cada canal, mudado a cada quadro pelo laço principal */
void __attribute__((interrupt_handler)) pwmM(void) {
  MARS_SYS_PWMI_CLR = 0;
  pwmIrqs++;
  /* enche a FIFO: tom quadrado no esquerdo e dente de serra no direito, com o volume do quadro */
  while (!(MARS_PWM_MONO & 0x8000)) {
    phase += 3;
    MARS_PWM_LEFT = (uint16_t)(524 + ((phase & 0x80) ? ampL : -ampL));
    MARS_PWM_RIGHT = (uint16_t)(524 - ampR + ((phase & 0xFF) * ampR >> 7));
  }
}
#endif

/* ------------------------------------------------------------------ escravo */
#if TESTE == 4
static void slaveDraw(void) {
  /* metade de baixo: padrão de MAC.W/DMULS calculado pelo escravo */
  for (int y = 112; y < 200; y++) {
    for (int x = 0; x < 320; x++) {
      int64_t p = (int64_t)(int32_t)(x * 0x10001 - y * 77) * (int32_t)(y * 31337 + x);
      px8(x, y, 16 + (int)((uint64_t)p >> 40) % 200);
    }
  }
}
#endif

void s_main(void) {
#if TESTE == 4
  /* a cada pedido do mestre (COMM4 = 0x1234) desenha e responde 0x4321 */
  for (;;) {
    while (MARS_SYS_COMM4 != 0x1234) {}
    slaveDraw();
    MARS_SYS_COMM4 = 0x4321;
    while (MARS_SYS_COMM4 == 0x4321) {}
  }
#endif
  for (;;) {}
}

/* ------------------------------------------------------------------ mestre */
static void palette(void) {
  for (int i = 0; i < 256; i++) {
    int r = i & 31, g = (i * 3) & 31, b = (i >> 3) & 31;
    PAL[i] = (uint16_t)(COLOR(r, g, b) | ((i >= 128 && i < 192) ? 0x8000 : 0));
  }
  PAL[0] = 0;
}

static void drawPacked(void) {
  lineTable(160);
  for (int y = 0; y < 224; y++)
    for (int x = 0; x < 320; x++) px8(x, y, ((x + y) & 0x3F) == 0 ? 0 : (y < 112 ? x * 255 / 319 : 0));
  rect8(20, 120, 80, 40, 140);   /* com prioridade (bit 15 na paleta) */
  rect8(120, 120, 80, 40, 60);   /* sem prioridade */
  rect8(220, 120, 80, 40, 0);    /* transparente: fundo do Mega Drive */
  hex8(10, 180, 0x32ABCDEF, 8, 255);
}

#if TESTE == 4
static uint32_t dmaBuf[64];
static void divTests(uint32_t* out) {
  volatile uint32_t* DIVU = (volatile uint32_t*)0xFFFFFF00;
  DIVU[0] = 7; DIVU[1] = 1000;            /* 1000 / 7 */
  out[0] = DIVU[1]; out[1] = DIVU[4];     /* quociente, resto */
  DIVU[0] = (uint32_t)-13; DIVU[1] = 12345;
  out[2] = DIVU[5]; out[3] = DIVU[4];
  DIVU[0] = 100000; DIVU[4] = 3; DIVU[5] = 0x10000000; /* 64/32: 0x310000000 / 100000 */
  out[4] = DIVU[5]; out[5] = DIVU[4];
}
#endif

int m_main(void) {
  while ((MARS_SYS_INTMSK & MARS_SH2_ACCESS_VDP) == 0) {}
  palette();
  setVector(VBR_M, 70, vintM);
  setVector(VBR_M, 69, hintM);
  HwMdPuts("INDIGO 32X TESTE", 0x2000, 2, 1);
  HwMdPutc((char)('0' + TESTE), 0x2000, 19, 1);
  for (int r = 15; r < 20; r++) HwMdPuts("MEGA DRIVE MEGA DRIVE MEGA DRIVE MEGA DR", 0x4000, 0, r);
#if TESTE == 1
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;
  drawPacked();
  flip();
  drawPacked();
  /* preenchimento automático: uma faixa de cor 77 na linha 210 */
  MARS_VDP_FILLEN = 159;
  MARS_VDP_FILADR = 0x100 + 210 * 160;
  MARS_VDP_FILDAT = 0x4D4D;
  while (MARS_VDP_FBCTL & MARS_VDP_FEN) {}
  flip();
  /* deslocamento de um ponto (SFT) */
  *(volatile uint16_t*)0x20004102 = 1;
  for (;;) {}
#elif TESTE == 2
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_32K | MARS_VDP_PRIO_32X;
  for (int k = 0; k < 2; k++) {
    lineTable(320);
    for (int y = 0; y < 200; y++)
      for (int x = 0; x < 320; x++) {
        uint16_t c = COLOR(x / 10, y / 7, (x + y) / 17);
        if ((x / 40 + y / 25) & 1) c |= 0x8000;
        FB16[0x100 + y * 320 + x] = (x > 300 && y > 180) ? 0 : c;
      }
    flip();
  }
  for (;;) {}
#elif TESTE == 3
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_RLE;
  for (int k = 0; k < 2; k++) {
    int p = 0x100;
    for (int y = 0; y < 224; y++) {
      FB16[y] = (uint16_t)p;
      int x = 0, run = 1 + (y & 15);
      while (x < 320) {
        int n = run + ((x / 7) & 7);
        if (x + n > 320) n = 320 - x;
        FB16[p++] = (uint16_t)(((n - 1) << 8) | ((x + y * 3) & 0xFF));
        x += n;
      }
    }
    flip();
  }
  for (;;) {}
#elif TESTE == 4
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;
  uint32_t res[8];
  for (int k = 0; k < 2; k++) {
    lineTable(160);
    for (int i = 0x200; i < 0x200 + 320 * 224; i++) FB8[i] = 0;
    divTests(res);
    for (int i = 0; i < 6; i++) hex8(8, 8 + i * 8, res[i], 8, 200);
    /* DMA interno: 64 palavras longas da SDRAM para o quadro (cópia automática, 32 bits) */
    for (int i = 0; i < 64; i++) dmaBuf[i] = 0x01010101u * (uint32_t)(100 + i);
    SH2_DMA_DMAOR = 0;
    SH2_DMA_CHCR0 = 0;
    SH2_DMA_SAR0 = (uint32_t)dmaBuf | 0x20000000;
    SH2_DMA_DAR0 = 0x24000200 + 60 * 320;
    SH2_DMA_TCR0 = 64;
    SH2_DMA_DMAOR = 1;
    SH2_DMA_CHCR0 = 0x5A01;  /* destino e origem crescentes, 32 bits, automático, ligado */
    while (!(SH2_DMA_CHCR0 & 2)) {}
    hex8(8, 70, SH2_DMA_TCR0, 8, 201);
    /* contas com a cache ligada: MAC.L em laço */
    int64_t acc = 0;
    for (int i = 1; i < 500; i++) acc += (int64_t)(i * 7919) * (int64_t)(i - 250);
    hex8(8, 80, (uint32_t)(acc >> 32), 8, 202);
    hex8(44, 80, (uint32_t)acc, 8, 202);
    /* metade de baixo pelo escravo */
    MARS_SYS_COMM4 = 0x1234;
    while (MARS_SYS_COMM4 != 0x4321) {}
    hex8(200, 8, MARS_SYS_COMM4, 4, 203);
    flip();
    MARS_SYS_COMM4 = 0;
  }
  for (;;) {}
#elif TESTE == 5
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;
  setVector(VBR_M, 67, pwmM);
  MARS_PWM_CYCLE = 1047;                 /* ~22 kHz */
  MARS_PWM_CTRL = 0x0105;                /* L = esquerdo, R = direito, interrupção a cada amostra */
  for (int i = 0; i < 3; i++) { MARS_PWM_LEFT = 500; MARS_PWM_RIGHT = 500; }
  MARS_SYS_INTMSK = (uint16_t)(MARS_SYS_INTMSK | 0x09);  /* VINT e PWM */
  for (;;) {
    const uint32_t v = tick();
    /* envelope do som seguindo os quadros do 68000: pulsos de volume diferentes em cada canal */
    ampL = ((v >> 3) & 1) ? (int)(v & 7) * 60 : 0;
    ampR = ((v >> 4) & 1) ? 200 + (int)(v & 15) * 15 : 0;
    lineTable(160);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 320; x++) px8(x, y, (x + v) & 0xFF);
    hex8(10, 100, v, 8, 255);
    hex8(10, 108, vcount ? 1 : 0, 1, 255);  /* a VINT do SH-2 está chegando */
    flip();
  }
#elif TESTE == 6
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;
  static uint16_t buf[1024];
  /* DMA 0 pelo pedido DREQ: origem = FIFO (20004012), destino = buf, 16 bits, destino crescente */
  SH2_DMA_DMAOR = 0;
  SH2_DMA_CHCR0 = 0;
  SH2_DMA_SAR0 = 0x20004012;
  SH2_DMA_DAR0 = (uint32_t)buf | 0x20000000;
  SH2_DMA_TCR0 = 1024;
  SH2_DMA_DMAOR = 1;
  SH2_DMA_CHCR0 = 0x4405;  /* destino crescente, origem fixa, 16 bits, DREQ por nível, ligado (TE ainda zero) */
  MARS_SYS_COMM0 = 0x0900;  /* pede ao 68000 que mande 1024 palavras pela FIFO */
  while (!(SH2_DMA_CHCR0 & 2)) {}
  while (MARS_SYS_COMM0) {}
  for (int k = 0; k < 2; k++) {
    lineTable(160);
    for (int y = 0; y < 224; y++)
      for (int x = 0; x < 320; x++) px8(x, y, buf[(y * 4 + x / 80) & 1023] & 0xFF);
    hex8(10, 200, buf[1023], 4, 255);
    flip();
  }
  for (;;) {}
#elif TESTE == 7
  MARS_VDP_DISPMODE = MARS_224_LINES | MARS_VDP_MODE_256;
  *(volatile uint16_t*)0x20004004 = 0;   /* HINT em todas as linhas */
  MARS_SYS_INTMSK = (uint16_t)(MARS_SYS_INTMSK | 0x0C);  /* VINT e HINT */
  for (;;) {
    const uint32_t v = tick();
    lineTable(160);
    /* só 64 linhas por quadro: o desenho tem de caber num quadro mesmo com a HINT em toda linha */
    for (int y = 0; y < 64; y++)
      for (int x = 0; x < 320; x++) px8(x, y, ((x + v * 3) ^ (y + v)) & 0xFF);
    hex8(10, 200, v, 8, 255);
    /* a HINT chega em toda linha visível: entre 200 e 260 por quadro */
    hex8(10, 208, (vcount > 4 && hcount / vcount >= 200 && hcount / vcount <= 260) ? 1 : 0, 1, 255);
    flip();
  }
#endif
  return 0;
}
