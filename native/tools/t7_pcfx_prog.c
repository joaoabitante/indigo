/* Programa de teste do Índigo para o PC-FX, carregado do CD pela BIOS de teste (t7_pcfx_bios.c).
   Liga as interrupções (VDC A, KING, temporizador e controle) com prioridades, monta uma tela com os dois VDCs
   (fundo de blocos, sprite, VDC B por cima) e dois fundos do KING (bitmap de 256 cores e blocos de 16 cores com
   rolagem infinita), lê um arquivo do CD por DMA e por PIO, grava na memória de save e toca, em fases, o PSG,
   o CD-DA (faixa 2) e o ADPCM. Resultados em 0x100000 (RES) para o indigo-t7_pcfx conferir. */
#include "t7_pcfx_scsi.h"

#define RES ((volatile uint32_t*)0x00100000)
enum { R_DONE, R_FRAMES, R_VBL, R_TIMER, R_RASTER, R_PAD, R_SUM, R_DSTAT, R_CPU, R_BRAM, R_KING, R_PHASE, R_PADIRQ,
       R_PIOSUM, R_TOC, R_CDCMD, R_SPRX, R_ADPCM, R_SUBQ, R_GO2, R_DONE2 };

uint32_t cputest(void);
extern void irq_stub(void);

volatile uint32_t frame;
static volatile uint32_t padv;

/* cor da paleta i (o teste no PC usa a mesma fórmula) */
static uint16_t palc(int i) {
  const uint16_t y = (uint16_t)(0x30 + ((i * 37) & 0x7F));
  return (uint16_t)(y << 8 | ((i * 5) & 15) << 4 | ((i * 11) & 15));
}

static void vram_addr(int chip, uint16_t a) { dw(chip, 0, a); out16(0x400 + chip * 0x100, 2); }
static void vram_put(int chip, uint16_t v) { out16(0x404 + chip * 0x100, v); }

/* bloco de 8x8 do VDC com a cor dada por f(x, y) (planos 0/1 nas palavras 0-7, 2/3 nas 8-15) */
static void vdc_tile(int chip, uint16_t addr, int kind) {
  uint16_t w[16];
  for (int y = 0; y < 8; y++) {
    uint8_t p[4] = {0, 0, 0, 0};
    for (int x = 0; x < 8; x++) {
      const int c = kind == 0 ? 1 : kind == 1 ? ((x & 1) ? 3 : 2) : 7;
      for (int b = 0; b < 4; b++) if (c & (1 << b)) p[b] |= (uint8_t)(0x80 >> x);
    }
    w[y] = (uint16_t)(p[0] | p[1] << 8);
    w[y + 8] = (uint16_t)(p[2] | p[3] << 8);
  }
  vram_addr(chip, addr);
  for (int i = 0; i < 16; i++) vram_put(chip, w[i]);
}

static void vdc_timing(int chip) {
  dw(chip, 0x09, 0x0000);  /* MWR: mapa 32x32 */
  dw(chip, 0x0A, 0x0202);  /* HSW 2, HDS 2 */
  dw(chip, 0x0B, 0x041F);  /* HDW 31 (256 pontos), HDE 4 */
  dw(chip, 0x0C, 0x1102);  /* VSW 2, VDS 17: imagem a partir da linha 22 */
  dw(chip, 0x0D, 0x00EF);  /* 240 linhas */
  dw(chip, 0x0E, 0x0003);
  dw(chip, 0x07, 0); dw(chip, 0x08, 0);
}

static void kram_write(uint32_t addr, const uint16_t* src, uint32_t n, uint16_t fill) {
  kw32(0x0D, addr | (1u << 18));
  out16(0x600, 0x0E);
  for (uint32_t i = 0; i < n; i++) out16(0x604, src ? src[i] : fill);
}

static void install_irq(int level) {
  const uint32_t slot = 0x400 + (uint32_t)level * 16;
  const uint32_t disp = (uint32_t)irq_stub - slot;
  *(volatile uint16_t*)slot = (uint16_t)(0xA800 | ((disp >> 16) & 0x3FF));
  *(volatile uint16_t*)(slot + 2) = (uint16_t)disp;
}

static int psg_on;
static void psg(int on) {
  out16(0x100 + 0 * 2, 0);           /* canal 0 */
  out16(0x100 + 1 * 2, 0xFF);        /* volume geral */
  out16(0x100 + 4 * 2, 0x00);        /* desliga para gravar a onda */
  for (int i = 0; i < 32; i++) out16(0x100 + 6 * 2, i < 16 ? 0x1F : 0x00);
  out16(0x100 + 2 * 2, 112);         /* 3579545 / 32 / 112 = 998,7 Hz */
  out16(0x100 + 3 * 2, 0);
  out16(0x100 + 5 * 2, 0xFF);
  out16(0x100 + 4 * 2, on ? 0x9F : 0x00);
  psg_on = on;
}

/* rotina comum das interrupções (chamada pelo irq_stub com o nível) */
void irq_dispatch(uint32_t level) {
  const uint16_t act = in16(0xE00);
  if (act & 0x04) {  /* KING */
    const uint16_t k = in16(0x600);
    if (k & 0x800) RES[R_RASTER]++;
    RES[R_KING]++;
    out16(0x600, k & 0x7F);
  }
  if (act & 0x08) {  /* VDC A */
    const uint16_t st = dstat(0);
    if (st & 0x20) {
      frame++;
      RES[R_FRAMES] = frame;
      RES[R_VBL]++;
      /* anima: rola o fundo 1 do KING e anda o sprite */
      const uint32_t fz = frame < 200 ? frame : 200;  /* depois do quadro 200 a tela fica parada */
      const uint16_t ar = in16(0x600) & 0x7F;  /* lido também zera a marca de linha */
      kw(0x32, (uint16_t)(fz & 0x3FF));
      out16(0x600, ar);
      const uint16_t sx = (uint16_t)(32 + 40 + (fz & 63));
      vram_addr(0, 0x7F01);
      vram_put(0, sx);
      RES[R_SPRX] = sx;
      out16(0x00, 0); out16(0x00, 1);  /* pede o controle */
    }
  }
  if (act & 0x40) { RES[R_TIMER]++; out16(0xF00, 0x3); }
  if (act & 0x10) { (void)in16(0x00); padv = (uint32_t)in16(0x40) | (uint32_t)in16(0x42) << 16; RES[R_PADIRQ]++; }
  (void)level;
}

static void video_init(void) {
  /* HuC6261: paleta, deslocamentos e prioridades */
  vw(1, 0);
  for (int i = 0; i < 512; i++) vw(2, palc(i));
  vw(4, 0x8000);  /* VDC: fundo em 0, sprites em 256 */
  vw(5, 0x4000);  /* KING: fundo 0 em 0, fundo 1 em 128 */
  vw(8, 0x0041);  /* VDC fundo 1, VDC sprite 4 */
  vw(9, 0x0032);  /* KING fundo 0 = 2, fundo 1 = 3 */
  vw(0, 0x0F00);  /* 5,37 MHz, 263 linhas, VDC e fundos 0/1 do KING ligados */
  /* VDC A: metade esquerda cor 1, direita listras 2/3 (paleta 1); sprite 16x16 cor 5 */
  vdc_timing(0);
  vdc_tile(0, 0x1000, 0);
  vdc_tile(0, 0x1010, 1);
  vram_addr(0, 0);
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) vram_put(0, x < 16 ? 0x0100 : (0x1000 | 0x0101));
  vram_addr(0, 0x2000);
  for (int pl = 0; pl < 4; pl++)
    for (int r = 0; r < 16; r++) vram_put(0, (5 & (1 << pl)) ? 0xFFFF : 0);
  vram_addr(0, 0x7F00);
  vram_put(0, 64 + 150); vram_put(0, 32 + 40); vram_put(0, 0x0100); vram_put(0, 0x0082);  /* frente, paleta 2 */
  for (int i = 4; i < 256; i++) vram_put(0, 0);
  dw(0, 0x0F, 0x0010);  /* DMA da tabela de sprites a cada quadro */
  dw(0, 0x13, 0x7F00);
  dw(0, 0x05, 0x00C8);  /* fundo e sprites ligados, interrupção de vblank */
  /* VDC B: só um quadrado 32x32 de cor 7 (paleta 2) em (16,16); o resto transparente */
  vdc_timing(1);
  vdc_tile(1, 0x1000, 2);
  vram_addr(1, 0);
  for (int i = 0; i < 16; i++) vram_put(1, 0);
  vram_addr(1, 0);
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) vram_put(1, (x >= 2 && x < 6 && y >= 2 && y < 6) ? 0x2100 : 0x0000);
  dw(1, 0x05, 0x0080);
  /* KING: microprograma (fundo 0 = 4 palavras de bitmap; fundo 1 = BAT + 2 palavras do bloco) */
  static const uint16_t mp[8] = {0x000, 0x001, 0x002, 0x003, 0x050, 0x048, 0x049, 0x100};
  kw(0x15, 0);
  kw(0x13, 0);
  for (int i = 0; i < 16; i++) kw(0x14, mp[i & 7]);
  kw(0x0F, 0);           /* tudo na página 0 */
  kw(0x10, 0x00A3);      /* fundo 0: 256 cores (bitmap); fundo 1: 16 cores com BAT */
  kw(0x12, 0x0011);      /* prioridades dentro do KING */
  kw(0x16, 0x0002);      /* fundo 1 com rolagem infinita */
  kw(0x2C, 0x8888); kw(0x2D, 0x0088);
  kw(0x20, 0); kw(0x21, 0x40); kw(0x22, 0); kw(0x23, 0x40);
  kw(0x24, 0x60); kw(0x25, 0x64);
  kw(0x30, 0); kw(0x31, 0); kw(0x32, 0); kw(0x33, 0);
  /* bitmap do fundo 0: retângulo (160..223, 40..103) com 8 cores em faixas verticais */
  static uint16_t row[128];
  for (int y = 0; y < 256; y++) {
    for (int x = 0; x < 128; x++) {
      uint16_t w = 0;
      for (int h = 0; h < 2; h++) {
        const int px = x * 2 + h;
        const uint8_t c = (px >= 160 && px < 224 && y >= 40 && y < 104) ? (uint8_t)(0x20 + ((px >> 3) & 7)) : 0;
        w |= (uint16_t)(c << (h ? 0 : 8));
      }
      row[x] = w;
    }
    kram_write(0x10000 + (uint32_t)y * 128, row, 128, 0);
  }
  /* fundo 1: BAT com uma faixa diagonal do bloco 1 (paleta 1); bloco 0 transparente */
  static uint16_t bat[1024];
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) bat[y * 32 + x] = (uint16_t)((((x - y) & 31) < 4) ? 0x1001 : 0);
  kram_write(0x18000, bat, 1024, 0);
  kram_write(0x19000, 0, 16, 0);
  uint16_t t[16];
  for (int y = 0; y < 8; y++)
    for (int h = 0; h < 2; h++) {
      uint16_t w = 0;
      for (int k = 0; k < 4; k++) w |= (uint16_t)((((h * 4 + k + y) & 15) | 1) << (12 - k * 4));
      t[y * 2 + h] = w;
    }
  kram_write(0x19010, t, 16, 0);
  kw(0x15, 1);           /* liga o microprograma */
  kw(0x44, 100);         /* interrupção na linha 100 */
  kw(0x40, 0x2);
}

static void cd_audio(int on) {
  uint8_t c[10] = {0xD8, 1, 0x02, 0, 0, 0, 0, 0, 0, 0x80};  /* começa a tocar a faixa 2 */
  if (on) {
    RES[R_CDCMD] = (uint32_t)scsi_pio(c, 10, 0, 0, 0);
    uint8_t e[10] = {0xD9, 4, 0x03, 0, 0, 0, 0, 0, 0, 0x80};  /* até o fim do disco, repetindo */
    RES[R_CDCMD] |= (uint32_t)scsi_pio(e, 10, 0, 0, 0) << 8;
  } else {
    uint8_t p[10] = {0xDA, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    RES[R_CDCMD] |= (uint32_t)scsi_pio(p, 10, 0, 0, 0) << 16;
    uint8_t q[10] = {0xDD, 10, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t sq[10];
    scsi_pio(q, 10, sq, 10, 0);
    RES[R_SUBQ] = (uint32_t)sq[0] | (uint32_t)sq[2] << 8 | (uint32_t)sq[8] << 16;
  }
}

static void adpcm(int on) {
  if (on) {
    static uint16_t d[256];
    for (int i = 0; i < 256; i++) d[i] = (i & 8) ? 0xCCCC : 0x4444;
    kram_write(0x8000, d, 256, 0);
    kw(0x58, 0x80);           /* início 0x8000 */
    kw32(0x59, 0x80FF);       /* fim */
    kw(0x5A, 0x80FF >> 6);
    kw(0x51, 1);              /* repete */
    out16(0x122, 0x3F); out16(0x124, 0x3F);
    out16(0x120, 0x00);
    kw(0x50, 0x05);           /* canal 0, 15,7 kHz */
  } else {
    kw(0x50, 0);
    out16(0x122, 0); out16(0x124, 0);
  }
}

int main(void) {
  RES[R_DONE] = 0;
  for (int i = 1; i < 32; i++) RES[i] = 0;
  RES[R_CPU] = cputest();
  /* interrupções: VDC A (7), KING (6), temporizador (5), controle (4) */
  for (int lv = 8; lv < 16; lv++) install_irq(lv);
  out16(0xE40, 0x7F);
  out16(0xE80, (7 << 9) | (6 << 6));
  out16(0xEC0, (5 << 6) | 4);
  video_init();
  /* arquivo de dados do CD (setor 100 da faixa, 64 KB): DMA para a KRAM e soma; um setor também por PIO */
  const int32_t lba = first_data_track();
  RES[R_TOC] = (uint32_t)lba;
  uint8_t c[10];
  cdb10(c, 0x28, (uint32_t)lba + 100, 32);
  RES[R_DSTAT] = (uint32_t)scsi_dma(c, 10, 0x20000, 32 * 2048);
  uint32_t sum = 0;
  kw32(0x0C, 0x20000 | (1u << 18));
  out16(0x600, 0x0E);
  for (int i = 0; i < 32 * 1024; i++) sum += in16(0x604);
  RES[R_SUM] = sum;
  static uint8_t sec[2048];
  cdb10(c, 0x28, (uint32_t)lba + 101, 1);
  uint32_t got = 0, s2 = 0;
  RES[R_DSTAT] |= (uint32_t)scsi_pio(c, 10, sec, 2048, &got) << 8;
  for (int i = 0; i < 2048; i += 2) s2 += (uint32_t)(sec[i] | sec[i + 1] << 8);
  RES[R_PIOSUM] = s2 + (got << 24);
  /* memória de save: grava com a proteção aberta, depois tenta com ela fechada */
  out16(0xC80, 1);
  volatile uint8_t* bram = (volatile uint8_t*)0xE0000000;
  const char* w = "INDO";
  for (int i = 0; i < 4; i++) bram[0x1000 * 2 + i * 2] = (uint8_t)w[i];
  out16(0xC80, 0);
  bram[0x1000 * 2 + 8] = 0x55;
  RES[R_BRAM] = (uint32_t)bram[0x2000] | (uint32_t)bram[0x2002] << 8 | (uint32_t)bram[0x2004] << 16 | (uint32_t)bram[0x2006] << 24;
  RES[R_BRAM + 0] ^= (bram[0x2008] == 0x55) ? 0xFFFFFFFF : 0;  /* não podia ter gravado */
  /* temporizador: 1000 x 15 ciclos */
  out16(0xF80, 1000);
  out16(0xF00, 0x3);
  out16(0x00, 1);
  out16(0xE40, 0x7F & ~(0x08 | 0x04 | 0x40 | 0x10));
  __asm__ volatile("cli");
  int phase = 0;
  while (phase < 5) {
    const uint32_t f = frame;
    const int want = f < 60 ? 0 : f < 120 ? 1 : f < 180 ? 2 : f < 240 ? 3 : 5;
    if (f == 35) RES[R_PAD] = padv;
    if (want != phase) {
      if (want == 1) psg(1);
      if (want == 2) { psg(0); out16(0x12A, 0x3F); out16(0x12C, 0x3F); cd_audio(1); }
      if (want == 3) { cd_audio(0); adpcm(1); }
      if (want == 5) { adpcm(0); RES[R_ADPCM] = kr(0x53); }
      phase = want;
      RES[R_PHASE] = (uint32_t)phase;
    }
  }
  RES[R_DONE] = 0x454E4F44;  /* "DONE" */
  /* segunda tela, quando o teste pedir: relógio de 7,16 MHz e o fundo 0 com rotação/escala (zoom 2x em
     volta de (192, 72)) */
  while (RES[R_GO2] != 1) {}
  __asm__ volatile("sei");
  kw(0x38, 0x80); kw(0x39, 0); kw(0x3A, 0); kw(0x3B, 0x80);
  kw(0x3C, 192); kw(0x3D, 72);
  kw(0x12, 0x1011);
  vw(0, 0x0F08);
  dw(0, 0x05, 0x0088);  /* sprites desligados (o VDC continua com o tempo de 5,37 MHz) */
  __asm__ volatile("cli");
  RES[R_DONE2] = 0x32454E4F;
  for (;;) __asm__ volatile("halt");
  return 0;
}
