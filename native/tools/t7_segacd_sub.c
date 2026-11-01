/* BIOS de teste do Sega CD (Índigo): programa do sub-CPU, copiado para a PRG-RAM pelo 68000 principal.
 * Espera pedidos do principal (A12010 = teste, flag do principal = sequência), roda o teste e grava 32 palavras
 * de resultado em PRG-RAM RESULTS + 64 * teste (as 16 primeiras aparecem na tela; as outras só o teste em C++
 * confere, como contagens que dependem do tempo exato). */
#include "t7_segacd_hw.h"

#define GA 0xFF8000UL
#define G8(o) R8(GA + (o))
#define G16(o) R16(GA + (o))
#define PCMREG(r) R8(0xFF0001UL + ((u32)(r) << 1))
#define PCMRAM(i) R8(0xFF2001UL + ((u32)(i) << 1))
#define WRAM 0x80000UL

extern s16 sintab[256];  /* seno em Q14, 256 passos por volta (gerado) */

static volatile u16 cddTicks, int2Count, timerCount, gfxDone, decCount, dteCount;
static volatile u8 cddCmd[10], cddSend, cddStat[10];
static volatile u8 lastHead[4];
static volatile u16 lastPt;
static u16* res;
static u16 track2Lba;

/* ------------------------------------------------------------ interrupções */
void __attribute__((interrupt_handler)) int1(void) { gfxDone++; }
void __attribute__((interrupt_handler)) int2(void) { int2Count++; }
void __attribute__((interrupt_handler)) int3(void) { timerCount++; }

/* CDD (75 vezes por segundo): lê o estado e manda o próximo comando (estado, se não houver outro) */
void __attribute__((interrupt_handler)) int4(void) {
  int i;
  u8 c[10], sum = 0;
  for (i = 0; i < 5; i++) {
    u16 w = G16(0x38 + 2 * i);
    cddStat[2 * i] = w >> 8;
    cddStat[2 * i + 1] = (u8)w;
  }
  for (i = 0; i < 9; i++) { c[i] = cddSend ? cddCmd[i] : 0; sum += c[i]; }
  c[9] = (~sum) & 15;
  G16(0x42) = (u16)(c[0] << 8 | c[1]);
  G16(0x44) = (u16)(c[2] << 8 | c[3]);
  G16(0x46) = (u16)(c[4] << 8 | c[5]);
  G16(0x48) = (u16)(c[6] << 8 | c[7]);
  G16(0x4A) = (u16)(c[8] << 8 | c[9]);
  cddSend = 0;
  cddTicks++;
}

/* CDC: fim de transferência (DTEI) ou setor decodificado (DECI) */
void __attribute__((interrupt_handler)) int5(void) {
  u8 st;
  G8(0x05) = 1;
  st = G8(0x07);
  if (!(st & 0x20)) {
    G8(0x05) = 4;
    lastHead[0] = G8(0x07); lastHead[1] = G8(0x07); lastHead[2] = G8(0x07); lastHead[3] = G8(0x07);
    lastPt = G8(0x07);
    lastPt |= (u16)G8(0x07) << 8;
    G8(0x05) = 0x0F;
    (void)G8(0x07);
    decCount++;
  }
  if (!(st & 0x40)) { G8(0x05) = 7; G8(0x07) = 0; dteCount++; }
}

void __attribute__((interrupt_handler)) int6(void) {}

/* ------------------------------------------------------------ utilidades */
static void waitTicks(u16 n) { u16 t = cddTicks; while ((u16)(cddTicks - t) < n) {} }

static void cdd(u8 c0, u8 c1, u8 c2, u8 c3, u8 c4, u8 c5, u8 c6, u8 c7) {
  cddCmd[0] = c0; cddCmd[1] = c1; cddCmd[2] = c2; cddCmd[3] = c3;
  cddCmd[4] = c4; cddCmd[5] = c5; cddCmd[6] = c6; cddCmd[7] = c7; cddCmd[8] = 0;
  cddSend = 1;
  while (cddSend) {}
}

static u8 bin2(u16 v, u8* t, u8* u) { *t = 0; while (v >= 10) { v -= 10; (*t)++; } *u = (u8)v; return 0; }

/* tocar (ou buscar) a partir do LBA: MSF em dígitos BCD separados */
static void cddGoto(u8 cmd, u16 lba) {
  u16 a = lba + 150, m = 0, s = 0;
  u8 mt, mu, st, su, ft, fu;
  while (a >= 4500) { a -= 4500; m++; }
  while (a >= 75) { a -= 75; s++; }
  bin2(m, &mt, &mu); bin2(s, &st, &su); bin2(a, &ft, &fu);
  cdd(cmd, 0, mt, mu, st, su, ft, fu);
}

static u16 packStat(int i) {
  return (u16)(cddStat[i] << 12 | cddStat[i + 1] << 8 | cddStat[i + 2] << 4 | cddStat[i + 3]);
}

static u16 headLba(void) {
  u16 m = (lastHead[0] >> 4) * 10 + (lastHead[0] & 15), s = (lastHead[1] >> 4) * 10 + (lastHead[1] & 15);
  u16 f = (lastHead[2] >> 4) * 10 + (lastHead[2] & 15), l = f;
  while (s--) l += 75;
  while (m--) l += 4500;
  return l - 150;
}

/* espera o setor lba passar pelo decodificador; devolve o endereço dele no buffer do CDC */
static u16 waitSector(u16 lba) {
  u16 tries = 0;
  for (;;) {
    u16 d = decCount;
    while (decCount == d) {}
    if (headLba() == lba) return lastPt;
    if (++tries > 600) return 0xFFFF;
  }
}

static void cdcReg(u8 r, u8 v) { G8(0x05) = r; G8(0x07) = v; }
static u8 cdcRd(u8 r) { G8(0x05) = r; return G8(0x07); }

static void cdcInit(void) {
  cdcReg(0x0F, 0);         /* reset */
  cdcReg(0x01, 0x62);      /* IFCTRL: DTEIEN, DECIEN, DOUTEN */
  cdcReg(0x0A, 0x84);      /* CTRL0: DECEN, WRRQ */
  cdcReg(0x0B, 0x00);      /* CTRL1 */
}

static void cdcTransfer(u8 dest, u16 dma, u16 src, u16 len) {
  G8(0x04) = dest;
  G16(0x0A) = dma;
  G8(0x05) = 2;
  G8(0x07) = (u8)(len - 1);
  G8(0x07) = (u8)((len - 1) >> 8);
  G8(0x07) = (u8)src;
  G8(0x07) = (u8)(src >> 8);
  G8(0x07) = 0;  /* DTRG */
}

static u16 waitEdt(void) {
  u16 n = 0;
  while (!(G8(0x04) & 0x80)) { if (++n == 60000) return 0xDEAD; }
  return 0;
}

/* confere 1024 palavras contra o padrão do setor */
static u16 checkWords(volatile u16* p, u16 lba, u16 step, u16* sum) {
  u16 k, bad = 0, s = 0;
  for (k = 0; k < 1024; k++) {
    u16 v = *p;
    p += step;
    s += v;
    if (v != PATTERN(lba, k)) bad++;
  }
  *sum = s;
  return bad;
}

static s32 mul16(s16 a, s16 b) {
  s32 r;
  __asm__("muls.w %2,%0" : "=d"(r) : "0"((s32)a), "d"(b));
  return r;
}

/* ------------------------------------------------------------ testes */
static void tCdd(void) {
  G8(0x37) = 4;            /* HOCK: comunicação com o CDD ligada (interrupção de nível 4) */
  waitTicks(4);
  res[0] = G16(0x38); res[1] = G16(0x3A); res[2] = G16(0x3C); res[3] = G16(0x3E); res[4] = G16(0x40);
  res[5] = G16(0x36);
  res[6] = G8(0x33);
  res[7] = packStat(0);
}

static void tToc(void) {
  int i = 0;
  u8 sub[6] = {3, 4, 5, 5, 5, 2}, trk[6] = {0, 0, 1, 2, 3, 0};
  for (i = 0; i < 6; i++) {
    cdd(2, 0, 0, sub[i], 0, trk[i], 0, 0);
    waitTicks(2);
    res[i * 2] = packStat(0);
    res[i * 2 + 1] = packStat(4);
    res[16 + i] = (u16)(cddStat[8] << 4 | cddStat[9]);
    if (i == 3) {
      u16 m = (u16)(cddStat[2] * 10 + cddStat[3]), sec = (u16)(cddStat[4] * 10 + cddStat[5]);
      u16 l = (u16)((cddStat[6] & 7) * 10 + cddStat[7]);
      while (sec--) l += 75;
      while (m--) l += 4500;
      track2Lba = l - 150;
    }
  }
  res[13] = track2Lba;
  res[12] = G16(0x40);
}

static void tHost(void) {
  u16 pt, k, s = 0, bad = 0;
  cdcInit();
  cddGoto(3, 16);           /* tocar a partir do LBA 16 */
  pt = waitSector(20);
  res[0] = lastHead[0]; res[1] = lastHead[1]; res[2] = lastHead[2]; res[3] = lastHead[3];
  cdcTransfer(3, 0, (u16)(pt + 4), 2048);
  for (k = 0; k < 1024; k++) {
    u16 n = 0, v;
    while (!(G8(0x04) & 0x40)) { if (++n == 1000) break; }
    v = G16(0x08);
    s += v;
    if (v != PATTERN(20, k)) bad++;
  }
  res[4] = bad; res[5] = s;
  res[6] = G8(0x04);
  res[7] = cdcRd(2) | (u16)cdcRd(3) << 8;  /* DBC no fim */
  res[17] = cdcRd(1);                        /* IFSTAT (o bit DECI depende do tempo) */
  res[9] = (pt == 0xFFFF);
  res[16] = pt;
}

static void sectorDma(u8 dest, u16 dmaReg, u16 lba, u16* pt) {
  *pt = waitSector(lba);
  res[0] = lastHead[0]; res[1] = lastHead[1]; res[2] = lastHead[2]; res[3] = lastHead[3];
  cdcTransfer(dest, dmaReg, (u16)(*pt + 4), 2048);
  res[4] = waitEdt();
  res[5] = G16(0x0A);   /* endereço do DMA no fim */
  res[6] = G8(0x04);
}

static void tPrgDma(void) {
  u16 pt, s;
  sectorDma(5, 0x40000 >> 3, 21, &pt);
  res[7] = checkWords((volatile u16*)0x40000, 21, 1, &s);
  res[8] = s;
}

static void tWramDma(void) {
  u16 pt, s;
  res[9] = G8(0x03);
  sectorDma(7, 0x10000 >> 3, 22, &pt);
  res[7] = checkWords((volatile u16*)(WRAM + 0x10000), 22, 1, &s);
  res[8] = s;
}

static void tPcmDma(void) {
  u16 pt, k, bad = 0, s = 0;
  PCMREG(7) = 0x02;   /* banco 2 da RAM de ondas, chip parado */
  sectorDma(4, 0, 23, &pt);
  for (k = 0; k < 1024; k++) {
    u16 v = (u16)(PCMRAM(2 * k) << 8 | PCMRAM(2 * k + 1));
    s += v;
    if (v != PATTERN(23, k)) bad++;
  }
  res[7] = bad; res[8] = s;
}

static void tW1mDma(void) {
  u16 pt, s;
  G8(0x03) = 0x04;    /* 1M, RET = 0: banco 0 do principal, banco 1 do sub */
  res[9] = G8(0x03);
  sectorDma(7, 0x4000 >> 3, 24, &pt);
  res[7] = checkWords((volatile u16*)(0xC0000 + 0x4000), 24, 1, &s);
  res[8] = s;
}

static void tMainHost(void) {
  u16 pt = waitSector(25);
  res[0] = lastHead[0]; res[1] = lastHead[1]; res[2] = lastHead[2]; res[3] = lastHead[3];
  cdcTransfer(2, 0, (u16)(pt + 4), 2048);
  res[4] = G8(0x04);
  cdd(1, 0, 0, 0, 0, 0, 0, 0);   /* para o drive (o resto do disco não toca por cima dos testes de som) */
  waitTicks(2);
  res[5] = cddStat[0];
}

static void tW2m(void) {
  u16 i, bad = 0, s = 0;
  volatile u16* w = (volatile u16*)WRAM;
  res[0] = G8(0x03);
  for (i = 0; i < 2048; i++) { u16 v = w[i]; s += v; if (v != (u16)(i * 3 + 1)) bad++; }
  res[1] = bad; res[2] = s;
  for (i = 0; i < 2048; i++) w[2048 + i] = (u16)~i;
  G8(0x03) = 0x01;    /* RET: devolve ao principal */
  res[3] = G8(0x03);
}

static void tW1m(void) {
  volatile u8* dot = (volatile u8*)WRAM;
  volatile u8* raw = (volatile u8*)0xC0000;
  int i;
  G8(0x03) = 0x05;    /* 1M, RET = 1: banco 1 do principal, banco 0 do sub */
  res[0] = G8(0x03);
  for (i = 0; i < 16; i++) dot[i] = (u8)(i & 15);          /* normal */
  G8(0x03) = 0x05 | 0x08;                                    /* underwrite: só onde é 0 */
  for (i = 0; i < 16; i++) dot[i] = (u8)(15 - (i & 15));
  for (i = 16; i < 32; i++) dot[i] = (u8)(i & 7);
  G8(0x03) = 0x05 | 0x10;                                    /* overwrite: só valores não 0 */
  for (i = 32; i < 48; i++) dot[i] = 9;
  for (i = 32; i < 48; i += 2) dot[i] = 0;
  ((volatile u16*)dot)[40] = 0x0A0B;                         /* palavra: 2 pontos */
  G8(0x03) = 0x05;
  for (i = 0; i < 8; i++) res[1 + i] = (u16)(raw[2 * i] << 8 | raw[2 * i + 1]);
  res[9] = ((volatile u16*)dot)[3];
  res[10] = dot[5];
  for (i = 0; i < 64; i++) raw[0x100 + i] = (u8)(0xC0 + i);
}

static void tW1mSwap(void) {
  volatile u16* raw = (volatile u16*)0xC0000;
  u16 i, s = 0;
  G8(0x03) = 0x04;    /* RET = 0: troca os bancos (banco 0 do principal, banco 1 do sub) */
  res[0] = G8(0x03);
  for (i = 0; i < 256; i++) s += raw[i] ^ i;
  res[1] = s;
  res[2] = raw[0]; res[3] = raw[1]; res[4] = raw[255];
}

static void tToMain(void) {
  G8(0x03) = 0x01;    /* RET em 2M: a Word RAM volta para o principal */
  res[0] = G8(0x03);
}

static void tMode2m(void) {
  G8(0x03) = 0x00;    /* volta para 2M (RET forçado: a Word RAM fica com o principal) */
  res[0] = G8(0x03);
}

/* ---- ASIC: selos, mapa, vetores de traço */
static void stamps(void) {
  volatile u8* w = (volatile u8*)WRAM;
  u16 n, b;
  /* selos 16x16 números 1..8 (128 bytes cada, a partir do byte 128) */
  for (n = 1; n <= 8; n++)
    for (b = 0; b < 128; b++) {
      u8 cell = (u8)(b >> 5), row = (u8)((b >> 2) & 7);
      u8 c1 = (u8)(((n + cell + row) & 7) + 1), c2 = (u8)(((n * 3 + (b & 3)) & 7) + 8);
      w[n * 128 + b] = (u8)(c1 << 4 | (((b ^ row) & 1) ? c2 : c1));
    }
  /* selos 32x32 (512 bytes, número múltiplo de 4): números 4, 8 -> bytes 512, 1024 */
  for (n = 1; n <= 2; n++)
    for (b = 0; b < 512; b++) {
      u8 cell = (u8)(b >> 5);
      w[0x2000 + n * 512 + b] = (u8)((((cell + n) & 15) << 4) | ((b + n) & 15));
    }
}

static void map(u32 base, u16 size, u16 stride, u16 mod) {
  volatile u16* m = (volatile u16*)(WRAM + base);
  u16 y, x, row = 0;
  for (y = 0; y < size; y++, row += stride)
    for (x = 0; x < size; x++) {
      u16 v = (u16)(x + y + y + y);
      while (v >= mod) v -= mod;
      m[row + x] = (u16)(v + 1);
    }
}

static void trace(u32 base, u8 ang, s16 scale, u16 lines, s16 cx, s16 cy) {
  volatile u16* t = (volatile u16*)(WRAM + base);
  s16 c = sintab[(u8)(ang + 64)], s = sintab[ang];
  s32 dx = mul16(c, scale) >> 14, dy = mul16(s, scale) >> 14;  /* 5.11 */
  u16 y;
  for (y = 0; y < lines; y++) {
    s16 yy = (s16)y - 32;
    /* início da linha: centro + rotação de (-32, yy), em 13.3 */
    s32 x0 = ((s32)cx << 3) + ((mul16(-32, (s16)dx) - mul16(yy, (s16)dy)) >> 8);
    s32 y0 = ((s32)cy << 3) + ((mul16(-32, (s16)dy) + mul16(yy, (s16)dx)) >> 8);
    t[y * 4] = (u16)x0; t[y * 4 + 1] = (u16)y0; t[y * 4 + 2] = (u16)dx; t[y * 4 + 3] = (u16)dy;
  }
}

static void asicRun(u8 cfg, u16 mapReg, u32 buf, u16 traceReg) {
  u16 g = gfxDone, n = 0;
  G16(0x58) = cfg;
  G16(0x5A) = mapReg;
  G16(0x5C) = 7;                       /* 8 células de altura */
  G16(0x5E) = (u16)(buf >> 2);
  G16(0x60) = 0;
  G16(0x62) = 64;
  G16(0x64) = 64;
  G16(0x66) = traceReg;                /* começa */
  res[8] = G16(0x58);
  while (gfxDone == g) { if (++n == 60000) break; }
  res[0] = G16(0x58); res[1] = G16(0x64); res[2] = (u16)(gfxDone - g);
  {
    volatile u16* b = (volatile u16*)(WRAM + buf);
    u16 i, s = 0, x = 0;
    for (i = 0; i < 1024; i++) { s += b[i]; x ^= (u16)(b[i] + i); }
    res[3] = s; res[4] = x;
    res[5] = b[0]; res[6] = b[100]; res[7] = b[1023];
  }
}

static void tAsic1(void) {
  stamps();
  map(0x10000, 16, 16, 8);
  trace(0x38000, 21, 0x800, 64, 128, 128);
  asicRun(0x00, 0x10000 >> 2, 0x34000, 0x38000 >> 2);
}

static void tAsic2(void) {
  /* 32x32: números 0x44 e 0x48 (selos de 512 bytes em 0x2200 e 0x2400) */
  {
    volatile u16* m = (volatile u16*)(WRAM + 0x11000);
    u16 i;
    for (i = 0; i < 64; i++) m[i] = (u16)(((i & 1) ? 0x44 : 0x48) | ((i & 6) << 12));
  }
  trace(0x39000, 200, 0x600, 64, 40, 40);
  asicRun(0x03, 0x11000 >> 2, 0x35000, 0x39000 >> 2);   /* 32x32, repetir */
}

static void tAsic3(void) {
  volatile u16* b = (volatile u16*)(WRAM + 0x36000);
  u16 i;
  for (i = 0; i < 1024; i++) b[i] = (u16)((i & 0x0F0F) | 0x2020);   /* fundo para o modo overwrite */
  /* mapa 4096x4096 com selos 16x16: 256x256 entradas (128 KB, base 0 ou 0x20000); só a área usada é escrita,
     com giro e espelho (bits 13-15) */
  {
    volatile u16* m = (volatile u16*)(WRAM + 0x20000);
    u16 y, x;
    for (y = 0; y < 24; y++)
      for (x = 0; x < 24; x++)
        m[((u32)y << 8) + x] = (u16)((((x + y) & 7) + 1) | ((u16)((x + x + x + x + x + y) & 7) << 13));
  }
  G8(0x03) = 0x10;   /* prioridade overwrite */
  trace(0x3A000, 240, 0xA00, 64, 100, 90);
  asicRun(0x04, 0x20000 >> 2, 0x36000, 0x3A000 >> 2);
  G8(0x03) = 0x00;
  res[9] = G8(0x03);
}

static void tTimer(void) {
  u16 t0, t1, c, sw;
  G8(0x31) = 100;           /* 100 x 384 ciclos */
  res[1] = G8(0x31);
  waitTicks(1);
  t0 = timerCount;
  G16(0x0C) = 0;             /* zera o cronômetro */
  waitTicks(10);
  sw = G16(0x0C);
  t1 = timerCount;
  c = (u16)(t1 - t0);
  res[0] = (c >= 42 && c <= 45);
  res[16] = c;
  G8(0x31) = 0;
  t0 = timerCount;
  waitTicks(3);
  res[2] = (u16)(timerCount - t0);
  res[3] = (sw >= 234 && sw <= 254);   /* 4340 em 12 bits */
  res[17] = sw;
  res[4] = G8(0x31);
}

static void tPcm(void) {
  u16 i;
  /* banco 0: dente de serra de 256 amostras (sinal-magnitude) e marca de laço */
  PCMREG(7) = 0x00;
  for (i = 0; i < 0x1000; i++) {
    u8 m = (u8)((i & 0xFF) >> 1);
    PCMRAM(i) = (i < 0x100) ? (u8)((i & 0x80) ? (m & 0x7F) : (0x80 | (m & 0x7F))) : 0xFF;
  }
  PCMRAM(0xFE) = 0x40;
  /* banco 1: quadrada */
  PCMREG(7) = 0x01;
  for (i = 0; i < 0x1000; i++) PCMRAM(i) = (i < 0x80) ? (u8)((i & 0x20) ? 0xF0 : 0x70) : 0xFF;
  /* canal 0 */
  PCMREG(7) = 0x40;
  PCMREG(0) = 0xC0; PCMREG(1) = 0x3F; PCMREG(2) = 0x00; PCMREG(3) = 0x04;   /* passo 0,5 */
  PCMREG(4) = 0x00; PCMREG(5) = 0x00; PCMREG(6) = 0x00;
  /* canal 1 */
  PCMREG(7) = 0x41;
  PCMREG(0) = 0x80; PCMREG(1) = 0xC5; PCMREG(2) = 0x00; PCMREG(3) = 0x09;
  PCMREG(4) = 0x00; PCMREG(5) = 0x10; PCMREG(6) = 0x10;
  PCMREG(8) = 0xFF;                /* todos desligados (recarrega os endereços) */
  PCMREG(7) = 0xC0;                /* liga o chip */
  PCMREG(8) = 0xFC;                /* canais 0 e 1 ligados */
  waitTicks(5);
  res[16] = PCMREG(0x11); res[17] = PCMREG(0x13); res[18] = PCMREG(0x10); res[19] = PCMREG(0x12);
  res[0] = (res[16] == 0x00);                     /* canal 0 dá voltas em 0x0000..0x00FF */
  res[1] = (res[17] == 0x10);                     /* canal 1 em 0x1000..0x107F */
  res[2] = PCMREG(7);
}

static void tPcmStop(void) { PCMREG(8) = 0xFF; PCMREG(7) = 0x00; res[0] = 1; }

static void tCdda(void) {
  u16 n = 0;
  G16(0x34) = 0x400 << 4;           /* fader: volume máximo */
  cddGoto(3, track2Lba);            /* começo da faixa 2 (do TOC) */
  while ((cddStat[0] != 1 || (G16(0x36) & 0x100)) && ++n < 60000) waitTicks(1);
  res[0] = cddStat[0]; res[1] = G16(0x36) >> 8; res[2] = (n < 60000);
  waitTicks(20);
  G16(0x34) = 0x200 << 4;           /* metade */
  res[18] = packStat(0); res[19] = packStat(4);
  res[3] = cddStat[0];
}

static void tCddaStop(void) {
  G16(0x34) = 0;                     /* fader até 0 */
  waitTicks(3);
  cdd(6, 0, 0, 0, 0, 0, 0, 0);       /* pausa */
  waitTicks(3);
  res[0] = cddStat[0]; res[1] = G16(0x36) >> 8;
}

static void tInt2(void) {
  u16 n = 0;
  res[1] = G16(0x00);
  while (int2Count < 5 && ++n < 60000) waitTicks(1);
  res[0] = int2Count;
}

static void tMisc(void) {
  int i;
  volatile u8* bram = (volatile u8*)0xFE0001;
  /* fontes */
  G8(0x4D) = 0x5A; G16(0x4E) = 0xA5C3;
  res[0] = G16(0x50); res[1] = G16(0x52); res[2] = G16(0x54); res[3] = G16(0x56);
  res[4] = G8(0x51);
  /* palavras de comunicação: eco das do principal */
  for (i = 0; i < 7; i++) G16(0x22 + 2 * i) = (u16)(G16(0x12 + 2 * i) ^ 0xFFFF);
  /* LED e registradores diversos */
  G8(0x00) = 0x03;
  res[5] = G16(0x00); res[6] = G16(0x02); res[7] = G16(0x36);
  /* RAM de backup: só os bytes ímpares */
  for (i = 0; i < 16; i++) bram[2 * i] = (u8)(0xB0 + i);
  res[8] = (u16)(bram[0] << 8 | bram[2]);
  res[9] = R16(0xFE0000);
  res[10] = R8(0xFE0000);
  res[11] = G16(0x4C);
  res[12] = G8(0x01);
}

static void tWp(void) {
  volatile u16* p = (volatile u16*)0xFF00;
  volatile u16* q = (volatile u16*)0x10100;
  res[0] = G16(0x02);
  *p = 0x1234; *q = 0x5678;
  res[1] = *p; res[2] = *q;
}

/* ------------------------------------------------------------ laço principal */
int main(void) {
  u8 seq = 0;
  u16 i;
  for (i = 0; i < NTESTS * 64; i++) R16(RESULTS + 2 * i) = 0;
  G8(0x33) = 0x7E;          /* níveis 1 a 6 */
  __asm__ volatile("move.w #0x2000,%sr");
  G8(0x0F) = 0xAA;          /* pronto */
  for (;;) {
    u8 f;
    u16 t;
    while ((f = G8(0x0E)) == seq) {}
    seq = f;
    t = G16(0x10);
    res = (u16*)(RESULTS + (u32)t * 64);
    switch (t) {
      case T_CDD: tCdd(); break;
      case T_TOC: tToc(); break;
      case T_HOST: tHost(); break;
      case T_PRGDMA: tPrgDma(); break;
      case T_WRAMDMA: tWramDma(); break;
      case T_PCMDMA: tPcmDma(); break;
      case T_W1MDMA: tW1mDma(); break;
      case T_MAINHOST: tMainHost(); break;
      case T_W2M: tW2m(); break;
      case T_W1M: tW1m(); break;
      case T_W1MB: tW1mSwap(); break;
      case T_MODE2M: tMode2m(); break;
      case T_TOMAIN: tToMain(); break;
      case T_ASIC1: tAsic1(); break;
      case T_ASIC2: tAsic2(); break;
      case T_ASIC3: tAsic3(); break;
      case T_TIMER: tTimer(); break;
      case T_PCM: tPcm(); break;
      case T_PCMSTOP: tPcmStop(); break;
      case T_CDDA: tCdda(); break;
      case T_CDDASTOP: tCddaStop(); break;
      case T_INT2: tInt2(); break;
      case T_MISC: tMisc(); break;
      case T_WP: tWp(); break;
      default: break;
    }
    G8(0x0F) = seq;
  }
  return 0;
}
