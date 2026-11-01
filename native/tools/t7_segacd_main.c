/* BIOS de teste do Sega CD (Índigo): programa do 68000 principal (fica na "BIOS", em 000000).
 * Copia o programa do sub-CPU para a PRG-RAM, solta o sub-CPU e pede os testes um a um. Os resultados ficam
 * numa tabela na RAM principal (TABELA, 16 palavras por teste lidas da PRG-RAM + as do lado principal) e na tela,
 * em páginas que trocam em contagens fixas de interrupções de quadro (iguais em qualquer emulador). */
#include "t7_segacd_hw.h"
#include "t7_segacd_fonte.h"

#define VDP_DATA R16(0xC00000)
#define VDP_CTRL R16(0xC00004)
#define VDP_CTRL32 R32(0xC00004)
#define GA 0xA12000UL
#define G8(o) R8(GA + (o))
#define G16(o) R16(GA + (o))
#define WRAM 0x200000UL

/* tabela de resultados: 32 palavras por teste (16 na tela + 16 extras) */
#define TABELA ((volatile u16*)0xFF8000)
#define MAINRES(t, i) TABELA[(t) * 32 + 16 + (i)]

extern const u8 sub_bin[];
extern const u8 sub_bin_end[];
static volatile u16 vblanks;
static u8 seq;

void __attribute__((interrupt_handler)) vint(void) { vblanks++; }

static void vdpReg(u8 r, u8 v) { VDP_CTRL = (u16)(0x8000 | (r << 8) | v); }
static void vramAddr(u16 a) { VDP_CTRL32 = ((u32)(0x4000 | (a & 0x3FFF)) << 16) | (a >> 14); }
static void vramRead(u16 a) { VDP_CTRL32 = ((u32)(a & 0x3FFF) << 16) | (a >> 14); }

static void waitV(u16 n) { u16 v = vblanks; while ((u16)(vblanks - v) < n) {} }
static void waitUntil(u16 t) { while ((s16)(vblanks - t) < 0) {} }

/* ------------------------------------------------------------ texto */
static u16 page = 0xC000;
static void putc_(u16 x, u16 y, char c) {
  u16 t = 0;
  if (c >= '0' && c <= '9') t = (u16)(1 + c - '0');
  else if (c >= 'A' && c <= 'Z') t = (u16)(11 + c - 'A');
  else if (c == ':') t = 37;
  else if (c == '-') t = 38;
  else if (c == '.') t = 39;
  vramAddr((u16)(page + (y << 7) + (x << 1)));
  VDP_DATA = t;
}
static void puts_(u16 x, u16 y, const char* s) { while (*s) putc_(x++, y, *s++); }
static void hex4(u16 x, u16 y, u16 v) {
  int i;
  for (i = 3; i >= 0; i--) { u8 d = (u8)((v >> (i * 4)) & 15); putc_(x++, y, (char)(d < 10 ? '0' + d : 'A' + d - 10)); }
}

/* mostra as 16 palavras de um teste em duas linhas */
static void show(u16 t, u16 row) {
  int i;
  hex4(0, row, t);
  for (i = 0; i < 8; i++) hex4((u16)(5 + i * 4), row, TABELA[t * 32 + i]);
  for (i = 0; i < 8; i++) hex4((u16)(5 + i * 4), (u16)(row + 1), TABELA[t * 32 + 8 + i]);
}

/* ------------------------------------------------------------ comunicação com o sub-CPU */
static void readResults(u16 t) {
  /* para o sub-CPU (BUSREQ) e lê os resultados pela janela da PRG-RAM (banco 0, sem mexer no A12003) */
  volatile u16* src = (volatile u16*)(0x020000 + RESULTS + (u32)t * 64);
  int i;
  G8(0x01) = 0x03;
  while (!(G8(0x01) & 0x02)) {}
  for (i = 0; i < 32; i++) TABELA[t * 32 + i] = src[i];
  G8(0x01) = 0x01;
}

static void run(u16 t) {
  G16(0x10) = t;
  seq++;
  G8(0x0E) = seq;
  while (G8(0x0F) != seq) {}
  readResults(t);
}

/* ------------------------------------------------------------ testes do lado principal */
static void mainHost(void) {
  u16 k, bad = 0, s = 0, n;
  for (k = 0; k < 1024; k++) {
    u16 v;
    n = 0;
    while (!(G8(0x04) & 0x40)) { if (++n == 2000) break; }
    v = G16(0x08);
    s += v;
    if (v != PATTERN(25, k)) bad++;
  }
  MAINRES(T_MAINHOST, 0) = bad;
  MAINRES(T_MAINHOST, 1) = s;
  MAINRES(T_MAINHOST, 2) = G8(0x04);
  TABELA[T_MAINHOST * 32 + 8] = bad;
  TABELA[T_MAINHOST * 32 + 9] = s;
  TABELA[T_MAINHOST * 32 + 10] = G16(0x04);
}

static void w2mPrepare(void) {
  volatile u16* w = (volatile u16*)WRAM;
  u16 i;
  for (i = 0; i < 2048; i++) w[i] = (u16)(i * 3 + 1);
  G8(0x03) = 0x02;   /* DMNA: Word RAM para o sub */
  while (!(G8(0x03) & 0x02) || (G8(0x03) & 0x01)) {}
  TABELA[T_W2M * 32 + 8] = G8(0x03);
}

static void w2mCheck(void) {
  volatile u16* w = (volatile u16*)WRAM;
  u16 i, bad = 0;
  while (!(G8(0x03) & 0x01)) {}
  for (i = 0; i < 2048; i++) if (w[2048 + i] != (u16)~i) bad++;
  TABELA[T_W2M * 32 + 9] = bad;
  TABELA[T_W2M * 32 + 10] = G8(0x03);
  TABELA[T_W2M * 32 + 11] = w[0];
  /* DMA do VDP a partir da Word RAM: a origem vai com +2 (o dado chega uma palavra atrasado) */
  for (i = 0; i < 32; i++) w[i] = (u16)(0xA000 + i);
  vramAddr(0x7000);
  for (i = 0; i < 32; i++) VDP_DATA = 0x1111;
  vdpReg(0x0F, 2);
  vdpReg(0x13, 16); vdpReg(0x14, 0);
  vdpReg(0x15, (u8)((WRAM + 2) >> 1)); vdpReg(0x16, (u8)((WRAM + 2) >> 9)); vdpReg(0x17, (u8)(((WRAM + 2) >> 17) & 0x7F));
  VDP_CTRL32 = ((u32)(0x4000 | (0x7000 & 0x3FFF)) << 16) | 0x80 | (0x7000 >> 14);
  vramRead(0x7002);
  {
    u16 s = 0;
    for (i = 1; i < 16; i++) { u16 v = VDP_DATA; if (v != (u16)(0xA000 + i)) s++; }
    TABELA[T_W2M * 32 + 12] = s;   /* palavras 1..15 iguais à origem */
  }
  vramRead(0x701E);
  TABELA[T_W2M * 32 + 13] = VDP_DATA;   /* depois do fim: intacta */
}

static void w1mMain(void) {
  volatile u16* w = (volatile u16*)WRAM;
  volatile u8* cell = (volatile u8*)(WRAM + 0x20000);
  u16 i, s = 0;
  /* banco 1 do principal: escreve linear e lê pela imagem de células */
  for (i = 0; i < 0x800; i++) w[i] = (u16)(i * 0x0101 + 0x1234);
  TABELA[T_W1M * 32 + 11] = G8(0x03);
  TABELA[T_W1M * 32 + 12] = cell[0];
  TABELA[T_W1M * 32 + 13] = cell[1];
  TABELA[T_W1M * 32 + 14] = cell[4];
  TABELA[T_W1M * 32 + 15] = *(volatile u16*)(WRAM + 0x20000 + 0x204);
  for (i = 0; i < 256; i++) s += *(volatile u16*)(WRAM + 0x20000 + ((u32)i << 3));
  MAINRES(T_W1M, 0) = s;
  for (i = 0; i < 256; i++) w[0x1000 + i] = (u16)(0x7700 + i);
}

static void w1mSwapMain(void) {
  volatile u16* w = (volatile u16*)WRAM;
  /* agora o banco 0 (do sub antes) é do principal: deve ter os pontos que o sub escreveu */
  TABELA[T_W1MB * 32 + 8] = G8(0x03);
  TABELA[T_W1MB * 32 + 9] = w[0];
  TABELA[T_W1MB * 32 + 10] = w[1];
  TABELA[T_W1MB * 32 + 11] = w[0x80];
  TABELA[T_W1MB * 32 + 12] = w[0x81];
  TABELA[T_W1MB * 32 + 13] = w[0x1000];
}

static void int2Main(void) {
  int i;
  for (i = 0; i < 5; i++) { G8(0x00) = 0x01; waitV(2); }
}

/* mostra as 3 imagens do ASIC (64x64, 8x8 células em colunas) a partir da Word RAM 2M */
static void asicImages(void) {
  u16 img, cx, cy;
  /* origem sem o +2 e destino 2 bytes antes, com uma palavra a mais: a palavra perdida cai fora da imagem
     (por isso da última imagem para a primeira) */
  for (img = 3; img-- > 0;) {
    const u32 src = WRAM + 0x34000 + ((u32)img << 12);
    const u16 dst = (u16)(0x1000 + (img << 11));
    const u16 d2 = (u16)(dst - 2);
    vdpReg(0x0F, 2);
    vdpReg(0x13, 0x01); vdpReg(0x14, 0x04);   /* 1025 palavras */
    vdpReg(0x15, (u8)(src >> 1)); vdpReg(0x16, (u8)(src >> 9)); vdpReg(0x17, (u8)((src >> 17) & 0x7F));
    VDP_CTRL32 = ((u32)(0x4000 | (d2 & 0x3FFF)) << 16) | 0x80 | (d2 >> 14);
    /* página 3 (0xE000): imagem img na coluna 2 + 10*img, linha 16; células em colunas */
    for (cx = 0; cx < 8; cx++)
      for (cy = 0; cy < 8; cy++) {
        vramAddr((u16)(0xE000 + ((16 + cy) << 7) + ((2 + img * 10 + cx) << 1)));
        VDP_DATA = (u16)((dst >> 5) + cx * 8 + cy);
      }
  }
}

/* ------------------------------------------------------------ início */
int main(void) {
  u16 i;
  /* VDP */
  vdpReg(0x00, 0x04); vdpReg(0x01, 0x34); vdpReg(0x02, 0xC000 >> 10); vdpReg(0x03, 0x00);
  vdpReg(0x04, 0xB000 >> 13); vdpReg(0x05, 0xF800 >> 9); vdpReg(0x07, 0x00); vdpReg(0x0A, 0xFF);
  vdpReg(0x0B, 0x00); vdpReg(0x0C, 0x81); vdpReg(0x0D, 0xFC00 >> 10); vdpReg(0x0F, 0x02);
  vdpReg(0x10, 0x01); vdpReg(0x11, 0x00); vdpReg(0x12, 0x00);
  vramAddr(0);
  for (i = 0; i < 0x8000; i++) VDP_DATA = 0;
  /* paleta: 0 fundo azul escuro, 1 texto branco; 2..15 cores para as imagens do ASIC */
  VDP_CTRL32 = 0xC0000000UL;
  {
    static const u16 pal[16] = {0x0400, 0x0EEE, 0x000E, 0x00E0, 0x0E00, 0x00EE, 0x0E0E, 0x0EE0,
                                0x0888, 0x0008, 0x0080, 0x0800, 0x0088, 0x0808, 0x0880, 0x0246};
    for (i = 0; i < 16; i++) VDP_DATA = pal[i];
  }
  /* fonte: 40 caracteres de 8x8 (1 bit) viram células de 4 bits com a cor 1 */
  vramAddr(32);
  for (i = 0; i < 39 * 8; i++) {
    u8 b = fonte[i];
    u16 hi = 0, lo = 0;
    int k;
    for (k = 0; k < 4; k++) { if (b & (0x80 >> k)) hi |= (u16)(1 << (12 - 4 * k)); }
    for (k = 0; k < 4; k++) { if (b & (0x08 >> k)) lo |= (u16)(1 << (12 - 4 * k)); }
    VDP_DATA = hi; VDP_DATA = lo;
  }
  for (i = 0; i < NTESTS * 32; i++) TABELA[i] = 0;
  vdpReg(0x01, 0x74);
  __asm__ volatile("move.w #0x2000,%sr");
  puts_(0, 0, "INDIGO SEGA CD BIOS DE TESTE");

  /* sub-CPU: em reset e parado ao ligar; copia o programa para a PRG-RAM (banco 0) e solta */
  G8(0x01) = 0x02;
  G8(0x02) = 0x00;   /* sem proteção de escrita */
  G8(0x03) = 0x00;
  {
    const u8* p = sub_bin;
    volatile u8* d = (volatile u8*)0x020000;
    while (p < sub_bin_end) *d++ = *p++;
  }
  TABELA[0] = G16(0x00); TABELA[1] = G16(0x02); TABELA[2] = R16(0x000070); TABELA[3] = R16(0x000072);
  G16(0x06) = 0x1234;  /* vetor da interrupção de linha */
  TABELA[4] = R16(0x000072); TABELA[5] = G16(0x06);
  G8(0x01) = 0x01;   /* solta o reset: o sub-CPU começa */
  while (G8(0x0F) != 0xAA) {}
  seq = 0xAA;
  TABELA[6] = G16(0x00); TABELA[7] = G16(0x02);
  TABELA[8] = R8(0xA10001);   /* versão: bit 5 = 0 com a unidade de CD ligada */
  for (i = 0; i < 7; i++) G16(0x12 + 2 * i) = (u16)(0x1111 * (i + 1));

  run(T_CDD);
  run(T_TOC);
  run(T_HOST);
  run(T_PRGDMA);
  G8(0x03) = 0x02;   /* Word RAM 2M para o sub */
  while (G8(0x03) & 0x01) {}
  run(T_WRAMDMA);
  run(T_PCMDMA);
  run(T_W1MDMA);
  run(T_MAINHOST);
  mainHost();
  run(T_MODE2M);
  TABELA[T_MODE2M * 32 + 8] = G8(0x03);
  run(T_TOMAIN);
  TABELA[T_MODE2M * 32 + 9] = G8(0x03);
  w2mPrepare();
  run(T_W2M);
  w2mCheck();
  run(T_W1M);
  w1mMain();
  run(T_W1MB);
  w1mSwapMain();
  run(T_MODE2M);
  G8(0x03) = 0x02;
  while (G8(0x03) & 0x01) {}
  run(T_ASIC1);
  run(T_ASIC2);
  run(T_ASIC3);
  run(T_TIMER);
  run(T_MISC);
  for (i = 0; i < 7; i++) TABELA[T_MISC * 32 + 13] ^= G16(0x22 + 2 * i);
  TABELA[T_MISC * 32 + 14] = G16(0x0E);
  G8(0x02) = 0x80;   /* protege a PRG-RAM abaixo de 0x10000 */
  run(T_WP);
  TABELA[T_WP * 32 + 8] = G16(0x02);
  G8(0x02) = 0x00;
  {
    u16 seqI = (u16)(seq + 1);
    G16(0x10) = T_INT2;
    seq++;
    G8(0x0E) = seq;
    int2Main();
    while (G8(0x0F) != seq) {}
    readResults(T_INT2);
    TABELA[T_INT2 * 32 + 8] = G16(0x00);
    (void)seqI;
  }
  TABELA[0x1F] = vblanks;   /* quadro em que os testes acabaram (só informativo, fora da tela) */

  /* página 1: testes 1-12; página 2: 13-24; página 3: imagens do ASIC */
  for (i = 1; i <= 12; i++) show(i, (u16)(2 + (i - 1) * 2));
  page = 0xA000;
  puts_(0, 0, "PAGINA 2");
  for (i = 13; i <= 24; i++) show(i, (u16)(2 + (i - 13) * 2));
  page = 0xE000;
  puts_(0, 0, "PAGINA 3 - ASIC");
  show(0, 2);
  /* devolve a Word RAM 2M ao principal para mostrar as imagens */
  run(T_TOMAIN);
  TABELA[T_TOMAIN * 32 + 8] = G8(0x03);
  asicImages();

  /* som: PCM e CD-DA em momentos fixos */
  waitUntil(900);
  run(T_PCM);
  waitUntil(1080);
  run(T_PCMSTOP);
  run(T_CDDA);
  waitUntil(1500);
  run(T_CDDASTOP);
  show(15, 26);

  /* páginas */
  waitUntil(1700);
  vdpReg(0x02, 0xA000 >> 10);
  waitUntil(1800);
  vdpReg(0x02, 0xE000 >> 10);
  for (;;) {}
  return 0;
}
