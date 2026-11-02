/* Homebrew de teste do Saturn do Índigo (escrito do zero, domínio do projeto, GPL-3.0): carregado pela BIOS de
 * teste a partir do CD, confere as partes do aparelho e mostra o resultado na tela (um quadrado por teste:
 * verde passou, vermelho falhou). O bloco de resultados fica em 0x26080000 para o teste em C++ ler:
 *   [0] 'SAT1'  [1] número de testes  [2] máscara dos que passaram  [3] dos que falharam  [4] 0xD0D0D0D0 no fim
 *   [5] quadros contados  [6] controle (OREG2..3 do INTBACK)  [8..] valores medidos de cada teste */
#include "t7_saturn_hw.h"

#define RES(n) R32(0x26080000 + (n) * 4)
#define NTESTES 12

extern const unsigned char som68k[];
extern const unsigned int som68k_tam;

void ent_vblank(void);
void ent_timer0(void);
void ent_dma0(void);
void ent_escravo_vb(void);
void ent_padrao(void);
void escravo_partida(void);

static volatile u32 vblanks, timer0s, timer0linha, dma0s;
static u32 imsAtual = 0xBFFF;

static void setSR(u32 v) { __asm__ volatile("ldc %0, sr" : : "r"(v)); }
static void setVBR(u32 v) { __asm__ volatile("ldc %0, vbr" : : "r"(v)); }

void int_vblank(void) { vblanks++; SCU_IMS = imsAtual; }
void int_timer0(void) { timer0s++; timer0linha = VDP2_REG(0x0A); SCU_IMS = imsAtual; }
void int_dma0(void) { dma0s++; SCU_IMS = imsAtual; }
void int_padrao(void) { RES(31) = RES(31) + 1; SCU_IMS = imsAtual; }

static void resultado(int n, int ok, u32 valor) {
  if (ok) RES(2) |= 1u << n; else RES(3) |= 1u << n;
  RES(8 + n) = valor;
}

static void espera(int quadros) {
  const u32 alvo = vblanks + (u32)quadros;
  int t;
  for (t = 0; t < 50000000 && (s32)(vblanks - alvo) < 0; t++) {}
}

/* ---------------------------------------------------------------- escravo */
void int_escravo_vb(void) { R32(0x26080104) = R32(0x26080104) + 1; }

void escravo_main(void) {
  u32 i, soma = 0;
  u32* tab = (u32*)0x06000400;
  for (i = 0; i < 128; i++) tab[i] = (u32)ent_padrao;
  tab[0x43] = (u32)ent_escravo_vb;
  tab[0x41] = (u32)ent_padrao;
  setVBR(0x06000400);
  for (i = 1; i <= 1000; i++) soma += i;
  R32(0x26080100) = soma;
  setSR(0x50);  /* aceita o V-blank (nível 6), ignora o H-blank (nível 2) */
  while (R32(0x26080104) < 2) {}
  R32(0x26080108) = 0xE5C7A40u;
  R8(0x21800000) = 0;  /* pulso FTI no mestre */
  for (;;) {}
}

/* ---------------------------------------------------------------- vídeo */
static void video(void) {
  int i, x, y;
  volatile u16* cram = (volatile u16*)VDP2_CRAM;
  volatile u32* chr = (volatile u32*)VDP2_VRAM;
  volatile u16* mapa = (volatile u16*)(VDP2_VRAM + 0x4000);
  volatile u16* bmp = (volatile u16*)(VDP2_VRAM + 0x40000);
  volatile u16* v1 = (volatile u16*)VDP1_VRAM;

  VDP2_REG(0x00) = 0;              /* tela desligada durante a montagem */
  VDP2_REG(0x0E) = 0;              /* CRAM RGB555, 1024 cores */
  cram[0] = 0; cram[1] = 0x7FFF; cram[2] = 0x03E0; cram[3] = 0x001F; cram[4] = 0x7C00;
  /* NBG0: células 8x8 de 16 cores; 0 = vazia, 1 = branca, 2 = verde, 3 = vermelha */
  for (i = 0; i < 8; i++) { chr[i] = 0; chr[8 + i] = 0x11111111; chr[16 + i] = 0x22222222; chr[24 + i] = 0x33333333; }
  for (i = 0; i < 64 * 64; i++) mapa[i] = 0;
  /* NBG1: bitmap 512x256 RGB de 15 bits; faixa em degradê nas linhas 180..199 */
  for (y = 0; y < 256; y++)
    for (x = 0; x < 512; x++)
      bmp[y * 512 + x] = (y >= 180 && y < 200) ? (u16)(0x8000 | ((x >> 3) & 31) | (((y - 180) & 31) << 5)) : 0;
  VDP2_REG(0x28) = 0x3200;         /* CHCTLA: NBG1 bitmap 512x256 32K cores; NBG0 16 cores 8x8 */
  VDP2_REG(0x30) = 0x8000;         /* PNCN0: nome de 1 palavra */
  VDP2_REG(0x3A) = 0;              /* planos 1x1 */
  VDP2_REG(0x3C) = 0x0020;         /* MPOFN: NBG1 no deslocamento 2 (0x40000) */
  VDP2_REG(0x40) = 0x0202;         /* NBG0: páginas em 0x4000 */
  VDP2_REG(0x42) = 0x0202;
  VDP2_REG(0x70) = 0; VDP2_REG(0x74) = 0;
  VDP2_REG(0x78) = 1; VDP2_REG(0x7A) = 0; VDP2_REG(0x7C) = 1; VDP2_REG(0x7E) = 0;
  VDP2_REG(0x80) = 0; VDP2_REG(0x84) = 0;
  VDP2_REG(0x88) = 1; VDP2_REG(0x8A) = 0; VDP2_REG(0x8C) = 1; VDP2_REG(0x8E) = 0;
  VDP2_REG(0xF8) = 0x0204;         /* PRINA: NBG0 = 4, NBG1 = 2 */
  VDP2_REG(0xE0) = 0x0020;         /* SPCTL: sprites RGB misturados, tipo 0 */
  VDP2_REG(0xF0) = 0x0006;         /* PRISA: sprite = 6 */
  R16(VDP2_VRAM + 0x7FFFE) = 0x2842;  /* fundo azul-acinzentado */
  VDP2_REG(0xAC) = 0x0003; VDP2_REG(0xAE) = 0xFFFF;
  VDP2_REG(0x20) = 0x0003;         /* BGON: NBG0 e NBG1 */

  /* VDP1: textura RGB 16x16 xadrez em 0x1000; lista de comandos em 0 */
  for (y = 0; y < 16; y++)
    for (x = 0; x < 16; x++) v1[0x800 + y * 16 + x] = (((x >> 2) ^ (y >> 2)) & 1) ? 0xFFFF : 0x801F;
  for (i = 0; i < 0x400; i++) v1[i] = 0;
#define CMD(n) (v1 + (n) * 16)
  /* 0: recorte do sistema 319x223; 1: coordenadas locais (0,0) */
  CMD(0)[0] = 0x0009; CMD(0)[10] = 319; CMD(0)[11] = 223;
  CMD(1)[0] = 0x000A; CMD(1)[6] = 0; CMD(1)[7] = 0;
  /* 2: polígono vermelho (200,20)-(239,59) */
  CMD(2)[0] = 0x0004; CMD(2)[2] = 0x00C0; CMD(2)[3] = 0x801F;
  CMD(2)[6] = 200; CMD(2)[7] = 20; CMD(2)[8] = 239; CMD(2)[9] = 20; CMD(2)[10] = 239; CMD(2)[11] = 59; CMD(2)[12] = 200; CMD(2)[13] = 59;
  /* 3: sprite normal RGB 16x16 em (250,20) */
  CMD(3)[0] = 0x0000; CMD(3)[2] = 0x0028; CMD(3)[4] = 0x1000 / 8; CMD(3)[5] = 0x0210; CMD(3)[6] = 250; CMD(3)[7] = 20;
  /* 4: sprite ampliado para 32x32 em (270,20)-(301,51) */
  CMD(4)[0] = 0x0001; CMD(4)[2] = 0x0028; CMD(4)[4] = 0x1000 / 8; CMD(4)[5] = 0x0210;
  CMD(4)[6] = 270; CMD(4)[7] = 20; CMD(4)[10] = 301; CMD(4)[11] = 51;
  /* 5: sprite distorcido (losango) em torno de (230,100) */
  CMD(5)[0] = 0x0002; CMD(5)[2] = 0x0028; CMD(5)[4] = 0x1000 / 8; CMD(5)[5] = 0x0210;
  CMD(5)[6] = 230; CMD(5)[7] = 80; CMD(5)[8] = 250; CMD(5)[9] = 100; CMD(5)[10] = 230; CMD(5)[11] = 120; CMD(5)[12] = 210; CMD(5)[13] = 100;
  /* 6: linha azul de (10,210) a (300,215) */
  CMD(6)[0] = 0x0006; CMD(6)[2] = 0x00C0; CMD(6)[3] = 0xFC00;
  CMD(6)[6] = 10; CMD(6)[7] = 210; CMD(6)[8] = 300; CMD(6)[9] = 215;
  /* 7: polígono meio transparente sobre o vermelho */
  CMD(7)[0] = 0x0004; CMD(7)[2] = 0x00C3; CMD(7)[3] = 0xFC00;
  CMD(7)[6] = 220; CMD(7)[7] = 40; CMD(7)[8] = 259; CMD(7)[9] = 40; CMD(7)[10] = 259; CMD(7)[11] = 69; CMD(7)[12] = 220; CMD(7)[13] = 69;
  CMD(8)[0] = 0x8000;  /* fim */
  VDP1_REG(0x00) = 0;      /* TVMR: 16 bits */
  VDP1_REG(0x02) = 0;      /* FBCR: troca automática */
  VDP1_REG(0x04) = 2;      /* PTMR: desenha a cada quadro */
  VDP1_REG(0x08) = 0;      /* EWLR */
  VDP1_REG(0x0A) = 0x50DF; /* EWRR: 320x224 */
  VDP1_REG(0x06) = 0;      /* EWDR: apaga com 0 (transparente) */
  VDP2_REG(0x00) = 0x8000;
}

static void quadrados(void) {
  volatile u16* mapa = (volatile u16*)(VDP2_VRAM + 0x4000);
  int i;
  for (i = 0; i < NTESTES; i++) {
    const u16 c = (RES(2) & (1u << i)) ? 2 : (RES(3) & (1u << i)) ? 3 : 1;
    const int x = 2 + i * 3, y = 2;
    mapa[y * 64 + x] = c; mapa[y * 64 + x + 1] = c;
    mapa[(y + 1) * 64 + x] = c; mapa[(y + 1) * 64 + x + 1] = c;
  }
}

/* ---------------------------------------------------------------- testes */
static void t_cpu(void) {
  volatile s32 a = 1234567, b = -89;
  volatile u32 c = 0xFFFFFFF0u, d = 7;
  const s32 q = a / b;
  const u32 r = c % d;
  resultado(0, q == -13871 && r == 0xFFFFFFF0u % 7u, (u32)q);
}

static void t_dma(void) {
  volatile u8* src = (volatile u8*)0x26090000;
  volatile u8* dst = (volatile u8*)0x20200000;
  int i, ok = 1;
  u32* tab = (u32*)0x26090800;
  for (i = 0; i < 256; i++) { src[i] = (u8)(i * 3 + 1); dst[i] = 0; }
  SCU_REG(0x00) = 0x06090000;
  SCU_REG(0x04) = 0x00200000;
  SCU_REG(0x08) = 256;
  SCU_REG(0x0C) = 0x101;
  SCU_REG(0x14) = 0x00000007;
  SCU_REG(0x10) = 0x101;
  for (i = 0; i < 100000 && !dma0s; i++) {}
  for (i = 0; i < 256; i++) if (dst[i] != (u8)(i * 3 + 1)) ok = 0;
  resultado(1, ok && dma0s == 1, dma0s);

  /* indireto no nível 1: duas cópias para a VRAM do VDP2 */
  for (i = 0; i < 128; i++) src[0x100 + i] = (u8)(0xA0 ^ i);
  tab[0] = 64; tab[1] = 0x05E20000; tab[2] = 0x06090100;
  tab[3] = 64; tab[4] = 0x05E20100; tab[5] = 0x80000000u | 0x06090140;
  SCU_REG(0x24) = 0x06090800;
  SCU_REG(0x2C) = 0x101;
  SCU_REG(0x34) = 0x01000007;
  SCU_REG(0x30) = 0x101;
  for (i = 0; i < 100000 && !(SCU_IST & (1u << 10)); i++) {}
  ok = 1;
  for (i = 0; i < 64; i++) {
    if (R8(0x25E20000 + i) != (u8)(0xA0 ^ i)) ok = 0;
    if (R8(0x25E20100 + i) != (u8)(0xA0 ^ (i + 64))) ok = 0;
  }
  resultado(2, ok, SCU_IST);
}

static void t_smpc(void) {
  int t;
  u32 v;
  SMPC_SF = 1;
  SMPC_IREG(0) = 0x00; SMPC_IREG(1) = 0x0A; SMPC_IREG(2) = 0xF0;
  SMPC_COMREG = 0x10;
  for (t = 0; t < 100000 && (SMPC_SF & 1); t++) {}
  v = (u32)SMPC_OREG(0) << 24 | (u32)SMPC_OREG(1) << 16 | (u32)SMPC_OREG(2) << 8 | SMPC_OREG(3);
  resultado(4, SMPC_OREG(0) == 0xF1 && SMPC_OREG(1) == 0x02 && SMPC_OREG(31) == 0x10, v);
}

static void t_escravo(void) {
  int t;
  R32(0x26080100) = 0; R32(0x26080104) = 0; R32(0x26080108) = 0;
  R32(0x26000250) = (u32)escravo_partida;
  SMPC_SF = 1; SMPC_COMREG = 0x03; for (t = 0; t < 100000 && (SMPC_SF & 1); t++) {}
  SMPC_SF = 1; SMPC_COMREG = 0x02; for (t = 0; t < 100000 && (SMPC_SF & 1); t++) {}
  espera(10);
  resultado(5, R32(0x26080100) == 500500 && R32(0x26080108) == 0xE5C7A40u && (R8(0xFFFFFE11) & 0x80),
            R32(0x26080104));
}

static void t_cd(void) {
  CdRes r;
  u32 buf[1024];
  u32 toc[102];
  int i, n = 0, ok = 1;
  /* TOC: faixa 1 de dados no FAD 150, faixa 2 de áudio */
  if (cd_cmd(&r, 0x0200, 0, 0, 0, HIRQ_DRDY)) ok = 0;
  for (i = 0; i < 102; i++) toc[i] = CD_DATA;
  cd_cmd(&r, 0x0600, 0, 0, 0, 0);
  if (toc[0] != 0x41000096u || (toc[1] >> 24) != 0x01 || (toc[99] >> 16) != 0x4101 || (toc[100] >> 16) != 0x0102) ok = 0;
  /* DADOS.BIN (arquivo 3 da raiz) pela leitura de arquivo do bloco de CD, na partição 1 */
  if (cd_cmd(&r, 0x7000, 0, 0x00FF, 0xFFFF, HIRQ_EFLS)) ok = 0;
  if (cd_cmd(&r, 0x7400, 0, 0x0100, 3, HIRQ_EFLS)) ok = 0;
  n = cd_count(1);
  if (n != 2 || cd_take(1, 2, buf)) ok = 0;
  else for (i = 0; i < 4096; i++) if (((const u8*)buf)[i] != (u8)(i * 7 + (i >> 8))) { ok = 0; break; }
  resultado(6, ok, (u32)n << 16 | toc[1] >> 24);
}

static void t_dsp(void) {
  static const u32 prog[] = {
    0x80000005u,  /* MVI #5, MC0 */
    0x84000007u,  /* MVI #7, MC1 */
    0x00001C00u,  /* MOV #0, CT0 */
    0x00001D00u,  /* MOV #0, CT1 */
    0x01960000u,  /* MOV MC1(M1),P ; MOV M0,A */
    0x10040000u,  /* ADD ; MOV ALU,A */
    0x00003209u,  /* MOV ALL, MC2 */
    0x9D824080u,  /* MVI #(0x06090200 >> 2), WA0 */
    0x00001E00u,  /* MOV #0, CT2 */
    0xC0011201u,  /* DMA MC2, D0, 1 */
    0xF8000000u,  /* ENDI */
  };
  int i, t;
  R32(0x26090200) = 0;
  SCU_REG(0x80) = 0x8000;
  for (i = 0; i < (int)(sizeof prog / 4); i++) SCU_REG(0x84) = prog[i];
  SCU_REG(0x80) = 0x8000;
  SCU_REG(0x80) = 0x10000;
  for (t = 0; t < 1000000 && (SCU_REG(0x80) & 0x10000); t++) {}
  resultado(7, R32(0x26090200) == 12 && (SCU_IST & (1u << 5)), R32(0x26090200));
}

static void t_som(void) {
  volatile u8* ram = (volatile u8*)SND_RAM;
  volatile u16* pcm = (volatile u16*)(SND_RAM + 0x10000);
  u32 i;
  int t;
  SMPC_SF = 1; SMPC_COMREG = 0x07; for (t = 0; t < 100000 && (SMPC_SF & 1); t++) {}
  for (i = 0; i < som68k_tam; i++) ram[i] = som68k[i];
  for (i = 0; i < 64; i++) pcm[i] = i < 32 ? 0x2000 : (u16)0xE000;
  R32(SND_RAM + 0x7F100) = 0; R32(SND_RAM + 0x7F104) = 0;
  SMPC_SF = 1; SMPC_COMREG = 0x06; for (t = 0; t < 100000 && (SMPC_SF & 1); t++) {}
  espera(5);
  resultado(8, R32(SND_RAM + 0x7F100) == 0x12345678u && R32(SND_RAM + 0x7F104) > 50, R32(SND_RAM + 0x7F104));
}

static void t_vdp1(void) {
  espera(2);
  resultado(9, (VDP1_REG(0x10) & 2) != 0, VDP1_REG(0x10));
}

static void t_backup(void) {
  int i, ok = 1;
  static const char m[] = "INDIGO-SATURN";
  for (i = 0; m[i]; i++) R8(0x20180001 + 0x100 + i * 2) = (u8)m[i];
  for (i = 0; m[i]; i++) if (R8(0x20180001 + 0x100 + i * 2) != (u8)m[i]) ok = 0;
  resultado(10, ok && R8(0x20180000 + 0x100) == 0xFF, R8(0x20180001));
}

int main(void) {
  u32* tab = (u32*)0x06000000;
  int i;
  RES(0) = 0x53415431u;
  RES(1) = NTESTES;
  RES(2) = RES(3) = RES(4) = RES(5) = 0;
  for (i = 0; i < 128; i++) tab[i] = (u32)ent_padrao;
  tab[0x40] = (u32)ent_vblank;
  tab[0x43] = (u32)ent_timer0;
  tab[0x4B] = (u32)ent_dma0;
  setVBR(0x06000000);
  imsAtual = 0xBFFF & ~(1u << 0) & ~(1u << 3) & ~(1u << 11);  /* V-blank, temporizador 0, DMA 0 */
  SCU_IMS = imsAtual;
  setSR(0x00);
  video();

  t_cpu();
  t_dma();
  /* interrupções: V-blank contado e o temporizador 0 na linha 100 */
  SCU_REG(0x90) = 100;
  SCU_REG(0x98) = 1;
  espera(4);
  resultado(3, vblanks >= 4 && timer0s >= 3 && timer0linha >= 99 && timer0linha <= 101, timer0linha);
  t_smpc();
  t_escravo();
  t_cd();
  t_dsp();
  t_som();
  t_vdp1();
  t_backup();
  resultado(11, RES(31) == 0, RES(31));  /* nenhuma interrupção inesperada */
  quadrados();
  RES(4) = 0xD0D0D0D0u;
  /* laço: lê o controle a cada quadro */
  for (;;) {
    int t;
    const u32 v = vblanks;
    while (vblanks == v) {}
    RES(5) = vblanks;
    SMPC_SF = 1;
    SMPC_IREG(0) = 0x00; SMPC_IREG(1) = 0x0A; SMPC_IREG(2) = 0xF0;
    SMPC_COMREG = 0x10;
    for (t = 0; t < 100000 && (SMPC_SF & 1); t++) {}
    RES(6) = (u32)SMPC_OREG(2) << 8 | SMPC_OREG(3);
  }
  return 0;
}
