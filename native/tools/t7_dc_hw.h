/* Registradores e rotinas comuns dos programas de teste do Dreamcast (BIOS de teste e jogo de teste do Índigo).
 * Compilados com o sh4-linux-gnu-gcc (-m4 -ml -nostdlib). Endereços da documentação pública do hardware. */
#ifndef T7_DC_HW_H
#define T7_DC_HW_H
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

#define R32(a) (*(volatile u32*)(a))
#define R16(a) (*(volatile u16*)(a))
#define R8(a) (*(volatile u8*)(a))

/* SCIF: texto de depuração (o teste lê o que sai daqui) */
#define SCFTDR2 R8(0xFFE8000C)
static void putc_(char c) { SCFTDR2 = (u8)c; }
static void puts_(const char* s) { while (*s) putc_(*s++); }
static void puthex(u32 v) {
  int i;
  for (i = 7; i >= 0; i--) putc_("0123456789ABCDEF"[(v >> (i * 4)) & 15]);
}
static void putdec(u32 v) {
  char b[12];
  int n = 0;
  do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
  while (n) putc_(b[--n]);
}
static void* memcpy_(void* d, const void* s, u32 n) {
  u8* a = (u8*)d; const u8* b = (const u8*)s;
  while (n--) *a++ = *b++;
  return d;
}
static int memcmp_(const void* x, const void* y, u32 n) {
  const u8* a = (const u8*)x; const u8* b = (const u8*)y;
  for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
  return 0;
}

/* Holly */
#define ISTNRM R32(0xA05F6900)
#define ISTEXT R32(0xA05F6904)
#define IML6NRM R32(0xA05F6930)
#define IML6EXT R32(0xA05F6934)
#define IML4NRM R32(0xA05F6920)
#define IML2NRM R32(0xA05F6910)

/* GD-ROM (ATA) */
#define GD_ALTST R8(0xA05F7018)
#define GD_DATA R16(0xA05F7080)
#define GD_FEAT R8(0xA05F7084)
#define GD_IREASON R8(0xA05F7088)
#define GD_BCL R8(0xA05F7090)
#define GD_BCH R8(0xA05F7094)
#define GD_STATUS R8(0xA05F709C)

/* pacote SPI; devolve os bytes recebidos ou -1 (erro). dma = 1: leitura pelo DMA do G1 para 'out'. */
static int gd_packet(const u8* pkt, void* out, int maxlen, int dma) {
  int i, n = 0;
  u8 st;
  while (GD_ALTST & 0x80) {}
  GD_FEAT = (u8)(dma ? 1 : 0);
  GD_BCL = (u8)(maxlen & 0xFF);
  GD_BCH = (u8)((maxlen >> 8) & 0xFF);
  GD_STATUS = 0xA0;
  while (!(GD_ALTST & 0x08)) {}
  for (i = 0; i < 6; i++) GD_DATA = (u16)(pkt[2 * i] | pkt[2 * i + 1] << 8);
  if (dma) {
    while (GD_ALTST & 0x80) {}
    if (!(GD_ALTST & 0x08)) { st = GD_STATUS; return (st & 1) ? -1 : 0; }
    R32(0xA05F7404) = (u32)out & 0x1FFFFFFF; /* SB_GDSTAR */
    R32(0xA05F7408) = (u32)maxlen;           /* SB_GDLEN */
    R32(0xA05F740C) = 1;                     /* para a RAM */
    R32(0xA05F7414) = 1;                     /* SB_GDEN */
    R32(0xA05F7418) = 1;                     /* SB_GDST */
    while (R32(0xA05F7418) & 1) {}
    while (GD_ALTST & 0x88) {}
    st = GD_STATUS;
    return (st & 1) ? -1 : (int)R32(0xA05F74F8);
  }
  for (;;) {
    do st = GD_ALTST; while (st & 0x80);
    if (!(st & 0x08)) break;
    {
      int cnt = GD_BCL | GD_BCH << 8;
      u16* o = (u16*)out;
      for (i = 0; i < cnt; i += 2) {
        u16 w = GD_DATA;
        if (n + i + 1 < maxlen) o[(n + i) / 2] = w;
      }
      n += cnt;
    }
  }
  st = GD_STATUS;
  return (st & 1) ? -1 : n;
}
/* lê setores de dados (2048 bytes) a partir do FAD */
static int gd_read(u32 fad, u32 count, void* out, int dma) {
  u8 p[12] = {0x30, 0x20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  p[2] = (u8)(fad >> 16); p[3] = (u8)(fad >> 8); p[4] = (u8)fad;
  p[8] = (u8)(count >> 16); p[9] = (u8)(count >> 8); p[10] = (u8)count;
  return gd_packet(p, out, (int)(count * 2048), dma);
}
/* TOC da área (0 = baixa densidade, 1 = alta); devolve o FAD da última faixa de dados ou 0 */
static u32 gd_data_fad(int area) {
  static u32 toc[102];
  u8 p[12] = {0x14, 0, 0, 0x01, 0x98, 0, 0, 0, 0, 0, 0, 0};
  u32 best = 0;
  int i;
  p[1] = (u8)area;
  if (gd_packet(p, toc, 408, 0) < 0) return 0;
  for (i = 0; i < 99; i++) {
    u32 e = toc[i];
    if (e == 0xFFFFFFFF) continue;
    if (((e >> 24) & 0xF0) == 0x40) best = ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF);
  }
  return best;
}
/* procura um arquivo na raiz do ISO 9660 da faixa (FAD da faixa); devolve o FAD e o tamanho */
static int iso_find(u32 track_fad, const char* name, u32* fad, u32* size, u8* buf) {
  u32 root, rlen, k;
  int nl = 0;
  while (name[nl]) nl++;
  if (gd_read(track_fad + 16, 1, buf, 0) < 0) return 0;
  if (memcmp_(buf + 1, "CD001", 5)) return 0;
  root = buf[156 + 2] | buf[156 + 3] << 8 | buf[156 + 4] << 16 | (u32)buf[156 + 5] << 24;
  rlen = buf[156 + 10] | buf[156 + 11] << 8 | buf[156 + 12] << 16 | (u32)buf[156 + 13] << 24;
  for (k = 0; k * 2048 < rlen; k++) {
    int o = 0;
    if (gd_read(root + 150 + k, 1, buf, 0) < 0) return 0;
    while (o < 2048 && buf[o]) {
      int l = buf[o], fl = buf[o + 32];
      if (fl >= nl && !memcmp_(buf + o + 33, name, (u32)nl) && (fl == nl || buf[o + 33 + nl] == ';')) {
        *fad = (buf[o + 2] | buf[o + 3] << 8 | buf[o + 4] << 16 | (u32)buf[o + 5] << 24) + 150;
        *size = buf[o + 10] | buf[o + 11] << 8 | buf[o + 12] << 16 | (u32)buf[o + 13] << 24;
        return 1;
      }
      o += l;
    }
  }
  return 0;
}
#endif
