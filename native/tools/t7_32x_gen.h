/* Gerador dos casos de teste do SH-2 (C puro: usado pelo indigo-t7_32x e pelo programa de referência que o
 * t7_32x.sh compila com o núcleo SH-2 do PicoDrive, fora do repositório).
 * Cada caso: registradores aleatórios, 4 KB de memória aleatória em 06000000, uma instrução válida do SH-2 em
 * 06000100 (e, se for desvio atrasado, uma instrução de conta no slot em 06000102). Os registradores usados como
 * endereço apontam para dentro dos 4 KB (alinhados), a pilha (R15) também e VBR = 06000000. */
#ifndef T7_32X_GEN_H
#define T7_32X_GEN_H
#include <stdint.h>
#include <string.h>

typedef struct {
  uint32_t r[16], pc, pr, sr, gbr, vbr, mach, macl;
  uint8_t mem[4096];
} Sh2Case;

/* tipos de acesso à memória de cada modelo de instrução */
enum { G_ALU = 0, G_RN = 1, G_RM = 2, G_R0RN = 4, G_R0RM = 8, G_GBR = 16, G_GBRR0 = 32, G_STACK = 64, G_BRANCH = 128, G_BOTH = 256 };

typedef struct { uint16_t mask, value; int kind; } Sh2Pat;

/* máscara = bits fixos; valor = o que eles valem; os outros bits são aleatórios */
static const Sh2Pat SH2_PATS[] = {
  {0xF0FF, 0x0002, G_ALU}, {0xF0FF, 0x0012, G_ALU}, {0xF0FF, 0x0022, G_ALU},              /* STC SR/GBR/VBR */
  {0xF0FF, 0x0003, G_BRANCH}, {0xF0FF, 0x0023, G_BRANCH},                                 /* BSRF BRAF */
  {0xF00F, 0x0004, G_R0RN}, {0xF00F, 0x0005, G_R0RN}, {0xF00F, 0x0006, G_R0RN},          /* MOV.x Rm,@(R0,Rn) */
  {0xF00F, 0x0007, G_ALU},                                                                /* MUL.L */
  {0xFFFF, 0x0008, G_ALU}, {0xFFFF, 0x0018, G_ALU}, {0xFFFF, 0x0028, G_ALU},              /* CLRT SETT CLRMAC */
  {0xFFFF, 0x0009, G_ALU}, {0xFFFF, 0x0019, G_ALU}, {0xF0FF, 0x0029, G_ALU},              /* NOP DIV0U MOVT */
  {0xF0FF, 0x000A, G_ALU}, {0xF0FF, 0x001A, G_ALU}, {0xF0FF, 0x002A, G_ALU},              /* STS */
  {0xFFFF, 0x000B, G_BRANCH}, {0xFFFF, 0x002B, G_BRANCH | G_STACK},                       /* RTS RTE */
  {0xF00F, 0x000C, G_R0RM}, {0xF00F, 0x000D, G_R0RM}, {0xF00F, 0x000E, G_R0RM},          /* MOV.x @(R0,Rm),Rn */
  {0xF00F, 0x000F, G_BOTH},                                                               /* MAC.L */
  {0xF000, 0x1000, G_RN},                                                                 /* MOV.L Rm,@(d,Rn) */
  {0xF00F, 0x2000, G_RN}, {0xF00F, 0x2001, G_RN}, {0xF00F, 0x2002, G_RN},                /* MOV.x Rm,@Rn */
  {0xF00F, 0x2004, G_RN}, {0xF00F, 0x2005, G_RN}, {0xF00F, 0x2006, G_RN},                /* MOV.x Rm,@-Rn */
  {0xF00F, 0x2007, G_ALU}, {0xF00F, 0x2008, G_ALU}, {0xF00F, 0x2009, G_ALU}, {0xF00F, 0x200A, G_ALU},
  {0xF00F, 0x200B, G_ALU}, {0xF00F, 0x200C, G_ALU}, {0xF00F, 0x200D, G_ALU}, {0xF00F, 0x200E, G_ALU},
  {0xF00F, 0x200F, G_ALU},
  {0xF00F, 0x3000, G_ALU}, {0xF00F, 0x3002, G_ALU}, {0xF00F, 0x3003, G_ALU}, {0xF00F, 0x3004, G_ALU},
  {0xF00F, 0x3005, G_ALU}, {0xF00F, 0x3006, G_ALU}, {0xF00F, 0x3007, G_ALU}, {0xF00F, 0x3008, G_ALU},
  {0xF00F, 0x300A, G_ALU}, {0xF00F, 0x300B, G_ALU}, {0xF00F, 0x300C, G_ALU}, {0xF00F, 0x300D, G_ALU},
  {0xF00F, 0x300E, G_ALU}, {0xF00F, 0x300F, G_ALU},
  {0xF0FF, 0x4000, G_ALU}, {0xF0FF, 0x4010, G_ALU}, {0xF0FF, 0x4020, G_ALU}, {0xF0FF, 0x4001, G_ALU},
  {0xF0FF, 0x4011, G_ALU}, {0xF0FF, 0x4021, G_ALU},
  {0xF0FF, 0x4002, G_RN}, {0xF0FF, 0x4012, G_RN}, {0xF0FF, 0x4022, G_RN},                /* STS.L */
  {0xF0FF, 0x4003, G_RN}, {0xF0FF, 0x4013, G_RN}, {0xF0FF, 0x4023, G_RN},                /* STC.L */
  {0xF0FF, 0x4004, G_ALU}, {0xF0FF, 0x4024, G_ALU}, {0xF0FF, 0x4005, G_ALU}, {0xF0FF, 0x4015, G_ALU},
  {0xF0FF, 0x4025, G_ALU},
  {0xF0FF, 0x4006, G_RN}, {0xF0FF, 0x4016, G_RN}, {0xF0FF, 0x4026, G_RN},                /* LDS.L */
  {0xF0FF, 0x4007, G_RN}, {0xF0FF, 0x4017, G_RN}, {0xF0FF, 0x4027, G_RN},                /* LDC.L */
  {0xF0FF, 0x4008, G_ALU}, {0xF0FF, 0x4018, G_ALU}, {0xF0FF, 0x4028, G_ALU}, {0xF0FF, 0x4009, G_ALU},
  {0xF0FF, 0x4019, G_ALU}, {0xF0FF, 0x4029, G_ALU},
  {0xF0FF, 0x400A, G_ALU}, {0xF0FF, 0x401A, G_ALU}, {0xF0FF, 0x402A, G_ALU},              /* LDS */
  {0xF0FF, 0x400B, G_BRANCH}, {0xF0FF, 0x401B, G_RN}, {0xF0FF, 0x402B, G_BRANCH},         /* JSR TAS JMP */
  {0xF0FF, 0x400E, G_ALU}, {0xF0FF, 0x401E, G_ALU}, {0xF0FF, 0x402E, G_ALU},              /* LDC */
  {0xF00F, 0x400F, G_BOTH},                                                               /* MAC.W */
  {0xF000, 0x5000, G_RM},                                                                 /* MOV.L @(d,Rm),Rn */
  {0xF00F, 0x6000, G_RM}, {0xF00F, 0x6001, G_RM}, {0xF00F, 0x6002, G_RM}, {0xF00F, 0x6003, G_ALU},
  {0xF00F, 0x6004, G_RM}, {0xF00F, 0x6005, G_RM}, {0xF00F, 0x6006, G_RM}, {0xF00F, 0x6007, G_ALU},
  {0xF00F, 0x6008, G_ALU}, {0xF00F, 0x6009, G_ALU}, {0xF00F, 0x600A, G_ALU}, {0xF00F, 0x600B, G_ALU},
  {0xF00F, 0x600C, G_ALU}, {0xF00F, 0x600D, G_ALU}, {0xF00F, 0x600E, G_ALU}, {0xF00F, 0x600F, G_ALU},
  {0xF000, 0x7000, G_ALU},                                                                /* ADD #imm */
  {0xFF00, 0x8000, G_RM}, {0xFF00, 0x8100, G_RM}, {0xFF00, 0x8400, G_RM}, {0xFF00, 0x8500, G_RM},
  {0xFF00, 0x8800, G_ALU},                                                                /* CMP/EQ #imm */
  {0xFF00, 0x8900, G_BRANCH}, {0xFF00, 0x8B00, G_BRANCH}, {0xFF00, 0x8D00, G_BRANCH}, {0xFF00, 0x8F00, G_BRANCH},
  {0xF000, 0x9000, G_ALU},                                                                /* MOV.W @(d,PC) */
  {0xF000, 0xA000, G_BRANCH}, {0xF000, 0xB000, G_BRANCH},                                 /* BRA BSR */
  {0xFF00, 0xC000, G_GBR}, {0xFF00, 0xC100, G_GBR}, {0xFF00, 0xC200, G_GBR},
  {0xFF00, 0xC300, G_STACK},                                                              /* TRAPA */
  {0xFF00, 0xC400, G_GBR}, {0xFF00, 0xC500, G_GBR}, {0xFF00, 0xC600, G_GBR}, {0xFF00, 0xC700, G_ALU},
  {0xFF00, 0xC800, G_ALU}, {0xFF00, 0xC900, G_ALU}, {0xFF00, 0xCA00, G_ALU}, {0xFF00, 0xCB00, G_ALU},
  {0xFF00, 0xCC00, G_GBRR0}, {0xFF00, 0xCD00, G_GBRR0}, {0xFF00, 0xCE00, G_GBRR0}, {0xFF00, 0xCF00, G_GBRR0},
  {0xF000, 0xD000, G_ALU},                                                                /* MOV.L @(d,PC) */
  {0xF000, 0xE000, G_ALU},                                                                /* MOV #imm */
};
#define SH2_NPATS ((int)(sizeof(SH2_PATS) / sizeof(SH2_PATS[0])))

static uint32_t sh2g_next(uint32_t* s) { /* xorshift32 */
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}

static uint16_t sh2g_op(uint32_t* s, int* kind, int aluOnly) {
  for (;;) {
    const Sh2Pat* p = &SH2_PATS[sh2g_next(s) % SH2_NPATS];
    if (aluOnly && p->kind != G_ALU) continue;
    uint16_t op = (uint16_t)((p->value & p->mask) | (sh2g_next(s) & ~p->mask));
    if (p->kind & (G_RN | G_RM | G_R0RN | G_R0RM | G_BOTH)) {
      /* registradores de endereço entre R1 e R14 (R0 é índice, R15 é a pilha) */
      int n = 1 + (int)(sh2g_next(s) % 14), m = 1 + (int)(sh2g_next(s) % 14);
      if (p->mask & 0x0F00) n = (op >> 8) & 15;
      if (p->mask & 0x00F0) m = (op >> 4) & 15;
      op = (uint16_t)((op & ~p->mask & 0xF000 ? op : op));
      if (!(p->mask & 0x0F00)) op = (uint16_t)((op & 0xF0FF) | (n << 8));
      if (!(p->mask & 0x00F0)) op = (uint16_t)((op & 0xFF0F) | (m << 4));
    }
    *kind = p->kind;
    return op;
  }
}

/* monta o caso de número `idx` */
static void sh2_gen_case(uint32_t idx, Sh2Case* c) {
  uint32_t s = idx * 2654435761u + 0x9E3779B9u;
  int i, kind, k2;
  if (!s) s = 1;
  for (i = 0; i < 8; i++) sh2g_next(&s);
  for (i = 0; i < 4096; i += 4) {
    uint32_t v = sh2g_next(&s);
    c->mem[i] = (uint8_t)(v >> 24); c->mem[i + 1] = (uint8_t)(v >> 16); c->mem[i + 2] = (uint8_t)(v >> 8); c->mem[i + 3] = (uint8_t)v;
  }
  for (i = 0; i < 16; i++) {
    uint32_t v = sh2g_next(&s);
    switch (sh2g_next(&s) % 4) {  /* valores pequenos e de borda aparecem mais */
      case 0: v &= 0xFF; break;
      case 1: v = (sh2g_next(&s) & 1) ? 0x80000000u - (v & 3) : 0xFFFFFFFFu - (v & 3); break;
      default: break;
    }
    c->r[i] = v;
  }
  c->pc = 0x06000100;
  c->pr = sh2g_next(&s);
  c->sr = sh2g_next(&s) & 0x3F3;
  c->gbr = 0x06000600;
  c->vbr = 0x06000000;
  c->mach = sh2g_next(&s);
  c->macl = sh2g_next(&s);
  if (sh2g_next(&s) & 1) c->mach = (sh2g_next(&s) & 1) ? 0x7FFF - (c->mach & 3) : 0xFFFF8000u + (c->mach & 3);
  {
    uint16_t op = sh2g_op(&s, &kind, 0);
    uint16_t slot = sh2g_op(&s, &k2, 1);
    int n = (op >> 8) & 15, m = (op >> 4) & 15;
    c->mem[0x100] = (uint8_t)(op >> 8); c->mem[0x101] = (uint8_t)op;
    c->mem[0x102] = (uint8_t)(slot >> 8); c->mem[0x103] = (uint8_t)slot;
    c->r[15] = 0x06000F00 - (sh2g_next(&s) & 0x3C);
    if (kind & (G_RN | G_BOTH)) c->r[n] = 0x06000800 + (sh2g_next(&s) & 0x3FC);
    if (kind & (G_RM | G_BOTH)) c->r[m] = 0x06000800 + (sh2g_next(&s) & 0x3FC);
    if (kind & (G_R0RN | G_R0RM)) {
      c->r[0] = sh2g_next(&s) & 0xFC;
      c->r[(kind & G_R0RN) ? n : m] = 0x06000800 + (sh2g_next(&s) & 0x3FC);
    }
    if (kind & G_GBRR0) c->r[0] = sh2g_next(&s) & 0xFF;
    /* MOV.B/W R0,@(d,Rn) e MOV.B/W @(d,Rm),R0 usam o campo m como base */
    if ((op & 0xFA00) == 0x8000 || (op & 0xFA00) == 0x8400) c->r[m] = 0x06000800 + (sh2g_next(&s) & 0x3FC);
  }
}

#endif
