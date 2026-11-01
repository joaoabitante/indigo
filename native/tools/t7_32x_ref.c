/* Referência dos testes de instrução do SH-2: o interpretador SH-2 do PicoDrive (derivado do MAME), compilado
 * pelo t7_32x.sh fora do repositório (licença do MAME: só como régua). Para cada caso do t7_32x_gen.h executa
 * uma instrução (e o slot, se for desvio atrasado) e grava os registradores finais, os ciclos e o CRC-32 dos 4 KB.
 * uso: t7_32x_ref primeiro quantidade saida.bin   (compilar com -I<picodrive> -I<picodrive>/pico) */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "cpu/sh2/sh2.h"
#include "t7_32x_gen.h"

static uint8_t g_mem[4096];

static uint32_t rd(uint32_t a, int sz) {
  uint32_t v = 0;
  int i;
  if ((a & 0xDFFFF000u) != 0x06000000u) return 0;
  for (i = 0; i < sz; i++) v = (v << 8) | g_mem[(a + i) & 0xFFF];
  return v;
}
static void wr(uint32_t a, uint32_t v, int sz) {
  int i;
  if ((a & 0xDFFFF000u) != 0x06000000u) return;
  for (i = sz - 1; i >= 0; i--, v >>= 8) g_mem[(a + i) & 0xFFF] = (uint8_t)v;
}
u32 REGPARM(2) p32x_sh2_read8(u32 a, SH2* sh2) { (void)sh2; return (u32)(s32)(s8)rd(a, 1); }
u32 REGPARM(2) p32x_sh2_read16(u32 a, SH2* sh2) { (void)sh2; return (u32)(s32)(s16)rd(a & ~1u, 2); }
u32 REGPARM(2) p32x_sh2_read32(u32 a, SH2* sh2) { (void)sh2; return rd(a & ~3u, 4); }
void REGPARM(3) p32x_sh2_write8(u32 a, u32 d, SH2* sh2) { (void)sh2; wr(a, d, 1); }
void REGPARM(3) p32x_sh2_write16(u32 a, u32 d, SH2* sh2) { (void)sh2; wr(a & ~1u, d, 2); }
void REGPARM(3) p32x_sh2_write32(u32 a, u32 d, SH2* sh2) { (void)sh2; wr(a & ~3u, d, 4); }
void lprintf(const char* fmt, ...) { (void)fmt; }

#define RB(sh2, a) p32x_sh2_read8(a, sh2)
#define RW(sh2, a) p32x_sh2_read16(a, sh2)
#define RL(sh2, a) p32x_sh2_read32(a, sh2)
#define WB(sh2, a, d) p32x_sh2_write8(a, d, sh2)
#define WW(sh2, a, d) p32x_sh2_write16(a, d, sh2)
#define WL(sh2, a, d) p32x_sh2_write32(a, d, sh2)
#define BUSY_LOOP_HACKS 0
typedef s8 INT8; typedef s16 INT16; typedef s32 INT32; typedef u32 UINT32; typedef u16 UINT16; typedef u8 UINT8;
#define T 0x00000001
#define S 0x00000002
#define I 0x000000f0
#define Q 0x00000100
#define M 0x00000200
#define AM 0xc7ffffff
#define FLAGS (M | Q | I | S | T)
#define Rn ((opcode >> 8) & 15)
#define Rm ((opcode >> 4) & 15)
#define sh2_state SH2
#define logerror lprintf
#define rlog(x)
#define rlog1(...)
#define rlog2(...)
#include "cpu/sh2/mame/sh2.c"

/* mesmo laço do interpretador do PicoDrive, sem os atalhos de laço ocioso */
static int run1(SH2* sh2) {
  UINT32 opcode;
  sh2->icount = 1;
  do {
    if (sh2->delay) {
      sh2->ppc = sh2->delay;
      opcode = (UINT32)(UINT16)RW(sh2, sh2->delay);
      sh2->pc -= 2;
    } else {
      sh2->ppc = sh2->pc;
      opcode = (UINT32)(UINT16)RW(sh2, sh2->pc);
    }
    sh2->delay = 0;
    sh2->pc += 2;
    switch ((opcode >> 12) & 15) {
      case 0: op0000(sh2, opcode); break;
      case 1: op0001(sh2, opcode); break;
      case 2: op0010(sh2, opcode); break;
      case 3: op0011(sh2, opcode); break;
      case 4: op0100(sh2, opcode); break;
      case 5: op0101(sh2, opcode); break;
      case 6: op0110(sh2, opcode); break;
      case 7: op0111(sh2, opcode); break;
      case 8: op1000(sh2, opcode); break;
      case 9: op1001(sh2, opcode); break;
      case 10: op1010(sh2, opcode); break;
      case 11: op1011(sh2, opcode); break;
      case 12: op1100(sh2, opcode); break;
      case 13: op1101(sh2, opcode); break;
      case 14: op1110(sh2, opcode); break;
      default: op1111(sh2, opcode); break;
    }
    sh2->icount--;
  } while (sh2->icount > 0 || sh2->delay);
  return 1 - sh2->icount;
}

static uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = ~0u;
  size_t i;
  int k;
  for (i = 0; i < n; i++) { c ^= p[i]; for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
  return ~c;
}

int main(int argc, char** argv) {
  static SH2 sh2;
  static Sh2Case c;
  uint32_t first, count, i;
  FILE* out;
  if (argc < 4) return 2;
  first = (uint32_t)strtoul(argv[1], NULL, 0);
  count = (uint32_t)strtoul(argv[2], NULL, 0);
  out = fopen(argv[3], "wb");
  if (!out) return 1;
  for (i = 0; i < count; i++) {
    uint32_t res[25];
    int k;
    sh2_gen_case(first + i, &c);
    memcpy(g_mem, c.mem, 4096);
    memset(&sh2, 0, sizeof sh2);
    for (k = 0; k < 16; k++) sh2.r[k] = c.r[k];
    sh2.pc = c.pc; sh2.pr = c.pr; sh2.sr = c.sr; sh2.gbr = c.gbr; sh2.vbr = c.vbr; sh2.mach = c.mach; sh2.macl = c.macl;
    res[23] = (uint32_t)run1(&sh2);
    for (k = 0; k < 16; k++) res[k] = sh2.r[k];
    res[16] = sh2.pc; res[17] = sh2.pr; res[18] = sh2.sr; res[19] = sh2.gbr; res[20] = sh2.vbr;
    res[21] = sh2.mach; res[22] = sh2.macl; res[24] = crc32(g_mem, 4096);
    fwrite(res, 4, 25, out);
  }
  fclose(out);
  return 0;
}
