/* Registradores do Saturn usados pela BIOS de teste e pelo homebrew de teste do Índigo (t7_saturn_bios.c e
 * t7_saturn_jogo.c). Escrito do zero a partir da documentação pública do hardware. */
#ifndef T7_SATURN_HW_H
#define T7_SATURN_HW_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

#define R8(a) (*(volatile u8*)(a))
#define R16(a) (*(volatile u16*)(a))
#define R32(a) (*(volatile u32*)(a))

/* bloco de CD (endereços sem cache) */
#define CD_HIRQ R16(0x25890008)
#define CD_MASK R16(0x2589000C)
#define CD_CR1 R16(0x25890018)
#define CD_CR2 R16(0x2589001C)
#define CD_CR3 R16(0x25890020)
#define CD_CR4 R16(0x25890024)
#define CD_DATA R32(0x25818000)
#define HIRQ_CMOK 0x0001
#define HIRQ_DRDY 0x0002
#define HIRQ_CSCT 0x0004
#define HIRQ_PEND 0x0010
#define HIRQ_ESEL 0x0040
#define HIRQ_EHST 0x0080
#define HIRQ_EFLS 0x0200

/* SMPC */
#define SMPC_IREG(n) R8(0x20100001 + (n) * 2)
#define SMPC_COMREG R8(0x2010001F)
#define SMPC_OREG(n) R8(0x20100021 + (n) * 2)
#define SMPC_SR R8(0x20100061)
#define SMPC_SF R8(0x20100063)

/* SCU */
#define SCU_REG(o) R32(0x25FE0000 + (o))
#define SCU_IMS SCU_REG(0xA0)
#define SCU_IST SCU_REG(0xA4)

/* VDP1 / VDP2 */
#define VDP1_VRAM 0x25C00000
#define VDP1_FB 0x25C80000
#define VDP1_REG(o) R16(0x25D00000 + (o))
#define VDP2_VRAM 0x25E00000
#define VDP2_CRAM 0x25F00000
#define VDP2_REG(o) R16(0x25F80000 + (o))

/* SCSP */
#define SND_RAM 0x25A00000
#define SCSP_REG(o) R16(0x25B00000 + (o))

/* resposta do último comando do CD */
typedef struct { u16 r[4]; } CdRes;

static inline int cd_cmd(CdRes* res, u16 c1, u16 c2, u16 c3, u16 c4, u16 wait) {
  int t;
  for (t = 0; t < 2000000 && !(CD_HIRQ & HIRQ_CMOK); t++) {}
  CD_HIRQ = (u16)~(HIRQ_CMOK | wait);  /* escrever 0 apaga o bit */
  CD_CR1 = c1; CD_CR2 = c2; CD_CR3 = c3; CD_CR4 = c4;
  for (t = 0; t < 2000000 && !(CD_HIRQ & HIRQ_CMOK); t++) {}
  if (t >= 2000000) return -1;
  res->r[0] = CD_CR1; res->r[1] = CD_CR2; res->r[2] = CD_CR3; res->r[3] = CD_CR4;
  if ((res->r[0] >> 8) == 0xFF) return -2;  /* recusado */
  if (wait) {
    for (t = 0; t < 20000000 && (CD_HIRQ & wait) != wait; t++) {}
    if (t >= 20000000) return -3;
  }
  return 0;
}

/* setores na partição p */
static inline int cd_count(int p) {
  CdRes r;
  if (cd_cmd(&r, 0x5100, 0, (u16)(p << 8), 0, 0)) return -1;
  return r.r[3];
}

/* tira n setores (2048 bytes) da partição p para dst; devolve 0 se deu certo */
static inline int cd_take(int p, int n, u32* dst) {
  CdRes r;
  int i;
  if (cd_cmd(&r, 0x6300, 0, (u16)(p << 8), (u16)n, HIRQ_DRDY)) return -1;
  for (i = 0; i < n * 512; i++) dst[i] = CD_DATA;
  if (cd_cmd(&r, 0x0600, 0, 0, 0, HIRQ_EHST)) return -2;
  return 0;
}

/* lê n setores a partir do FAD para dst pela partição 0 (filtro 0 ligado ao CD) */
static inline int cd_read(u32 fad, int n, u32* dst) {
  CdRes r;
  int got = 0, t;
  if (cd_cmd(&r, 0x1000 | 0x80 | (u16)(fad >> 16), (u16)fad, 0x0080 | (u16)(n >> 16), (u16)n, 0)) return -1;
  for (t = 0; got < n && t < 400000; t++) {
    int c = cd_count(0);
    if (c < 0) return -2;
    if (c > 0) {
      if (c > n - got) c = n - got;
      if (cd_take(0, c, dst + got * 512)) return -3;
      got += c;
    }
  }
  return got == n ? 0 : -4;
}

#endif
