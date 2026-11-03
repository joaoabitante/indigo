/* BIOS de teste do Saturn do Índigo: escrita do zero, NÃO é a BIOS da Sega. Serve para os testes rodarem sem a
 * BIOS do usuário. O mestre liga a tela, inicia o bloco de CD, autentica o disco, lê o IP.BIN (16 setores do começo do disco) em
 * 0x06002000, confere "SEGA SEGASATURN ", acha o primeiro arquivo da raiz pelo sistema de arquivos do bloco de
 * CD e o carrega no endereço do IP.BIN (0xF0), depois salta para lá. O escravo espera um endereço em
 * 0x06000250 (convenção desta BIOS, não da Sega) e salta para ele.
 *
 * Área de trabalho (sem cache): 0x26000240 etapa, 0x26000244 marca de exceção, 0x26000248 erro,
 * 0x26000250 entrada do escravo, 0x26000254 endereço do programa, 0x26000258 tamanho do programa. */
#include "t7_saturn_hw.h"

#define ETAPA R32(0x26000240)
#define ERRO R32(0x26000248)
#define ESCRAVO R32(0x26000250)

const char bios_texto[] = "Indigo Saturn: BIOS de teste do Indigo (nao e a BIOS da Sega); le o disco SEGA SEGASATURN ";

void bios_salta(u32 pc, u32 sp);

static void fundo(u16 cor) {
  R16(VDP2_VRAM + 0x7FFFE) = cor;
  VDP2_REG(0xAC) = 0x0003;
  VDP2_REG(0xAE) = 0xFFFF;
}

static void erro(u32 e) {
  ERRO = e;
  fundo(0x001F);  /* vermelho */
  for (;;) {}
}

void bios_escravo(void) {
  for (;;) {
    const u32 e = ESCRAVO;
    if (e) ((void (*)(void))e)();
  }
}

void bios_main(void) {
  CdRes r;
  u32 info[32 * 3];
  int i, words, n;
  u32 dest, fad = 0, size = 0;
  const char* sega = "SEGA SEGASATURN ";
  const u8* ip = (const u8*)0x26002000;

  ETAPA = 1;
  ERRO = 0;
  ESCRAVO = 0;
  VDP2_REG(0x0E) = 0;      /* CRAM em RGB555 */
  VDP2_REG(0x20) = 0;      /* nenhum plano */
  fundo(0x5000);           /* azul escuro enquanto carrega */
  VDP2_REG(0x00) = 0x8000; /* tela ligada, 320x224 */

  ETAPA = 2;
  if (cd_cmd(&r, 0x0401, 0, 0, 0x040F, 0)) erro(0x10);              /* inicia (com reset do software) */
  /* autentica o disco (sem isso o bloco de CD não guarda setores de dados) */
  if (cd_cmd(&r, 0xE000, 0, 0, 0, HIRQ_EFLS)) erro(0x19);
  if (cd_cmd(&r, 0xE100, 0, 0, 0, 0) || r.r[1] != 4) erro(0x1A);   /* 4 = disco do Saturn */
  if (cd_cmd(&r, 0x3000, 0, 0x0000, 0, HIRQ_ESEL)) erro(0x11);      /* CD -> filtro 0 */

  ETAPA = 3;
  if (cd_read(150, 16, (u32*)0x26002000)) erro(0x12);
  for (i = 0; i < 16; i++) if (ip[i] != (u8)sega[i]) erro(0x13);
  dest = *(const u32*)(ip + 0xF0);
  if (!dest) dest = 0x06004000;

  ETAPA = 4;
  if (cd_cmd(&r, 0x7000, 0, 0x17FF, 0xFFFF, HIRQ_EFLS)) erro(0x14);  /* diretório raiz (lido pelo filtro 23) */
  if (cd_cmd(&r, 0x7300, 0, 0x00FF, 0xFFFF, HIRQ_DRDY)) erro(0x15);  /* informação de todos os arquivos */
  words = r.r[1];
  if (words > 32 * 6) words = 32 * 6;
  for (i = 0; i < words / 2; i++) info[i] = CD_DATA;
  if (cd_cmd(&r, 0x0600, 0, 0, 0, 0)) erro(0x16);
  for (i = 0; i < words / 6; i++) {
    if (info[i * 3 + 2] & 0x02) continue;  /* diretório */
    fad = info[i * 3];
    size = info[i * 3 + 1];
    break;
  }
  if (!fad || !size) erro(0x17);

  ETAPA = 5;
  n = (int)((size + 2047) / 2048);
  if (cd_read(fad, n, (u32*)(dest | 0x20000000))) erro(0x18);
  R32(0x26000254) = dest;
  R32(0x26000258) = size;
  ETAPA = 6;
  bios_salta(dest, 0x06002000);
}
