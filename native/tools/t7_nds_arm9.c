// Programa de teste do ARM9 (Índigo, núcleo do Nintendo DS). Escrito do zero; testa divisor/raiz, SWI da BIOS
// (em alto nível no Índigo), timers, DMA, IPC com o ARM7, IRQ de VBlank, cartucho, save por SPI, 2D e 3D.
// Os resultados vão para RES (0x02300000); o indigo-t7_nds confere.
#include "t7_nds_hw.h"
#include "t7_nds_dados.h"  // gerado pelo t7_nds_rom.py: texto, LZ77, RLE e Huffman dele

u32 swi_div(s32 a, s32 b);
u32 swi_divrem(s32 a, s32 b);
u32 swi_sqrt(u32 v);
u32 swi_crc16(u32 crc, const void* p, u32 len);
void swi_cpuset(const void* s, void* d, u32 mode);
void swi_cpufastset(const void* s, void* d, u32 mode);
void swi_lz77(const void* s, void* d);
void swi_lz77cb(const void* s, void* d, u32 param, const void* cb);
void swi_rle(const void* s, void* d);
void swi_huffcb(const void* s, void* d, u32 param, const void* cb);
void swi_vblank(void);
void swi_intrwait(u32 discard, u32 mask);
void swi_bitunpack(const void* s, void* d, const void* info);

static volatile u32 vblanks, timerIrqs, ipcIrqs;
#define IRQ_FLAGS R32(0x0B003FF8)
#define IRQ_HANDLER R32(0x0B003FFC)

__attribute__((target("arm"))) void irq9(void) {
  const u32 f = IE & IF;
  if (f & 1) vblanks++;
  if (f & 8) timerIrqs++;
  if (f & (1 << 16)) ipcIrqs++;
  IF = f;
  IRQ_FLAGS |= f;
}

static u32 ipcCall(u32 v) {
  while (IPCFIFOCNT & 2) {}
  IPCFIFOSEND = v;
  while (IPCFIFOCNT & 0x100) {}
  return IPCFIFORECV;
}

// callbacks da descompressão por callback (SWI 0x12): abre devolve o cabeçalho, lê byte a byte
static int cbOpen(const u8* src, void* dst, u32 param) { (void)dst; RES[63] = param; return src[0] | (src[1] << 8) | (src[2] << 16) | (src[3] << 24); }
static void cbClose(const u8* src) { RES[62] = (u32)src; }
static u8 cbGet8(const u8* src) { return *src; }
static u32 cbGet32(const u8* src) { return src[0] | (src[1] << 8) | (src[2] << 16) | ((u32)src[3] << 24); }
static const void* CALLBACKS[5] = {(void*)cbOpen, (void*)cbClose, (void*)cbGet8, 0, (void*)cbGet32};

static void cardRead(u32 addr, u32* dst, int useDma) {
  while (ROMCTRL & 0x80000000) {}
  CARDCMD(0) = 0xB7;
  CARDCMD(1) = addr >> 24; CARDCMD(2) = addr >> 16; CARDCMD(3) = addr >> 8; CARDCMD(4) = addr;
  CARDCMD(5) = 0; CARDCMD(6) = 0; CARDCMD(7) = 0;
  if (useDma) {
    DMA_SAD(0) = 0x04100010;
    DMA_DAD(0) = (u32)dst;
    DMA_CNT(0) = 0x80000000 | (5u << 27) | 0x04000000 | 0x02000000 | (2u << 23) | 1;  // cartucho, 32 bits, repetir
  }
  ROMCTRL = 0xA1586000;  // bloco de 0x200 bytes
  if (useDma) {
    while (ROMCTRL & 0x80000000) {}
    DMA_CNT(0) = 0;
  } else {
    u32 n = 0;
    while (ROMCTRL & 0x80000000) {
      if (ROMCTRL & 0x800000) { u32 w = CARDDATA; if (n < 128) dst[n] = w; n++; }
    }
  }
}

static u8 spi(u8 v, int hold) {
  AUXSPICNT = 0xA000 | (hold ? 0x40 : 0);
  AUXSPIDATA = v;
  while (AUXSPICNT & 0x80) {}
  return AUXSPIDATA;
}

static void gfxVtx(s16 x, s16 y, s16 z) {
  GFX(0x23) = (u16)x | ((u32)(u16)y << 16);
  GFX(0x23) = (u16)z;
}

static u8 big[1024] __attribute__((aligned(4)));
static u32 words[256] __attribute__((aligned(4)));

int main(void) {
  for (int i = 0; i < 256; i++) RES[i] = 0;
  RES[0] = 0x4E445339;
  POWCNT1 = 0x820F;
  IRQ_HANDLER = (u32)irq9;
  // 1) divisor: 32/32, 64/32, 64/64 e divisão por zero
  DIVCNT = 0; DIV_NUMER = (u64)(s64)-1000; DIV_DENOM = 7;
  while (DIVCNT & 0x8000) {}
  RES[1] = (u32)DIV_RESULT; RES[2] = (u32)DIVREM_RESULT;
  DIVCNT = 1; DIV_NUMER = 0x123456789ABull; DIV_DENOM = 1000;
  RES[3] = (u32)DIV_RESULT; RES[4] = (u32)(DIV_RESULT >> 32); RES[5] = (u32)DIVREM_RESULT;
  DIVCNT = 2; DIV_NUMER = 0x7FFFFFFFFFFFFFFFull; DIV_DENOM = 0x100000000ull;
  RES[6] = (u32)DIV_RESULT;
  DIVCNT = 0; DIV_NUMER = 5; DIV_DENOM = 0;
  RES[7] = DIVCNT & 0x4000;
  // 2) raiz quadrada
  SQRTCNT = 0; SQRT_PARAM = 1000000; RES[8] = SQRT_RESULT;
  SQRTCNT = 1; SQRT_PARAM = 0x10000000000ull; RES[9] = SQRT_RESULT;
  // 3) SWI
  RES[10] = swi_div(100, 7); RES[11] = swi_divrem(-100, 7); RES[12] = swi_sqrt(144);
  RES[13] = swi_crc16(0xFFFF, texto, sizeof texto);
  for (int i = 0; i < 64; i++) words[i] = 0;
  { u32 fill = 0xCAFEBABE; swi_cpuset(&fill, words, 16 | (1u << 24) | (1u << 26)); }
  RES[14] = words[0] ^ words[15] ^ (words[16] == 0 ? 0x1 : 0);
  swi_cpufastset(words, words + 32, 16);
  RES[15] = words[32] == 0xCAFEBABE && words[47] == 0xCAFEBABE && words[48] == 0;
  swi_lz77(lz77, big);
  { u32 ok = 1; for (u32 i = 0; i < sizeof texto; i++) if (big[i] != texto[i]) ok = 0; RES[16] = ok; }
  for (int i = 0; i < 1024; i++) big[i] = 0;
  swi_rle(rle, big);
  { u32 ok = 1; for (u32 i = 0; i < sizeof texto; i++) if (big[i] != texto[i]) ok = 0; RES[17] = ok; }
  // LZ77 por callback escrevendo em 16 bits na VRAM (banco A em modo LCDC)
  VRAMCNT(0) = 0x80;
  swi_lz77cb(lz77, (void*)0x06800000, 0x1234, CALLBACKS);
  { u32 ok = 1; const volatile u8* v = (const volatile u8*)0x06800000; for (u32 i = 0; i < sizeof texto; i++) if (v[i] != texto[i]) ok = 0; RES[18] = ok; }
  // Huffman (só existe por callback no DS)
  for (int i = 0; i < 1024; i++) big[i] = 0;
  swi_huffcb(huff, big, 0x1234, CALLBACKS);
  { u32 ok = 1; for (u32 i = 0; i < sizeof texto; i++) if (big[i] != texto[i]) ok = 0; RES[64] = ok; }
  { // BitUnPack: 1 bit -> 4 bits com deslocamento 1
    static const u8 src[2] = {0xA5, 0x0F};
    static const struct { u16 len; u8 sw, dw; u32 off; } info = {2, 1, 4, 1};
    u32 out[2] = {0, 0};
    swi_bitunpack(src, out, &info);
    RES[19] = out[0]; RES[20] = out[1];
  }
  // 4) timers: cascata 0 -> 1
  TM_CNT(0) = 0; TM_CNT(1) = 0;
  TM_VAL(0) = 0xFF00; TM_VAL(1) = 0;
  TM_CNT(1) = 0x84; TM_CNT(0) = 0x80;
  for (volatile int i = 0; i < 20000; i++) {}
  RES[21] = TM_VAL(1);
  TM_CNT(0) = 0; TM_CNT(1) = 0;
  // 5) DMA: cópia de 32 bits e preenchimento pelo registrador de preenchimento
  for (int i = 0; i < 64; i++) words[i] = i * 0x01010101u;
  DMA_SAD(3) = (u32)words; DMA_DAD(3) = (u32)(words + 64); DMA_CNT(3) = 0x84000000 | 32;
  while (DMA_CNT(3) & 0x80000000) {}
  DMA_FILL(3) = 0x55AA55AA;
  DMA_SAD(3) = 0x040000EC; DMA_DAD(3) = (u32)(words + 128); DMA_CNT(3) = 0x84000000 | (2u << 23) | 16;
  RES[22] = words[64 + 31] == 31 * 0x01010101u && words[64] == 0;
  RES[23] = words[128] == 0x55AA55AA && words[143] == 0x55AA55AA && words[144] != 0x55AA55AA;
  // 6) IPC: sincronismo e FIFO com o ARM7
  IPCFIFOCNT = 0x8008 | 0x4000;
  for (volatile int i = 0; i < 1000 && (IPCSYNC & 0xF) != 7; i++) {}
  RES[24] = IPCSYNC & 0xF;
  RES[25] = ipcCall(0x100 | 20);   // eco: 20*2+1
  // 7) IRQ de VBlank e VBlankIntrWait
  DISPSTAT = 0x8;
  IE = 1 | (1 << 16);
  IME = 1;
  IPCSYNC = 0x4000;
  const u32 v0 = vblanks;
  swi_vblank(); swi_vblank(); swi_vblank();
  RES[26] = vblanks - v0;
  // IRQ de IPCSYNC vinda do ARM7
  ipcCall(0x700);
  for (volatile int i = 0; i < 2000 && !ipcIrqs; i++) {}
  RES[27] = ipcIrqs;
  // 8) cartucho: leitura por E/S direta e por DMA; ID do chip
  cardRead(0x20000, words, 0);
  RES[28] = words[0]; RES[29] = words[127];
  for (int i = 0; i < 128; i++) words[i] = 0;
  cardRead(0x20200, words, 1);
  RES[30] = words[0]; RES[31] = words[127];
  while (ROMCTRL & 0x80000000) {}
  for (int i = 0; i < 8; i++) CARDCMD(i) = i == 0 ? 0xB8 : 0;
  ROMCTRL = 0xA7586000;
  while (!(ROMCTRL & 0x800000)) {}
  RES[32] = CARDDATA;
  RES[33] = R32(0x027FF800);
  // 9) save (EEPROM com 2 bytes de endereço): leitura inicial de 256 bytes, escrita e releitura
  spi(0x03, 1); spi(0, 1); spi(0, 1);
  for (int i = 0; i < 255; i++) spi(0, 1);
  RES[34] = spi(0, 0);  // chip apagado: 0xFF
  spi(0x06, 0);
  spi(0x02, 1); spi(0, 1); spi(0x10, 1); spi('S', 1); spi('A', 1); spi('V', 1); spi('E', 0);
  spi(0x03, 1); spi(0, 1); spi(0x10, 1);
  { u32 v = spi(0, 1); v |= spi(0, 1) << 8; v |= spi(0, 1) << 16; v |= spi(0, 0) << 24; RES[35] = v; }
  // 10) toque e relógio pelo ARM7
  RES[36] = ipcCall(0x200);
  RES[37] = ipcCall(0x300);
  RES[38] = ipcCall(0x500);
  RES[39] = ipcCall(0x600);
  ipcCall(0x400);  // som
  // 11) vídeo 2D na engine B (tela de baixo): bitmap de cor direta e um sprite
  VRAMCNT(2) = 0x84;  // C -> BG da engine B
  VRAMCNT(3) = 0x84;  // D -> OBJ da engine B
  for (int y = 0; y < 192; y++)
    for (int x = 0; x < 256; x++) R16(0x06200000 + (y * 256 + x) * 2) = 0x8000 | (x >> 3) | ((y >> 3) << 5);
  BG3CNT_B = 0x4084;
  BG3PA_B = 0x100; BG3PB_B = 0; BG3PC_B = 0; BG3PD_B = 0x100; BG3X_B = 0; BG3Y_B = 0;
  for (int i = 0; i < 32; i++) R32(0x06600000 + i * 4) = 0x11111111;  // 16x16, 4 bits, cor 1
  R16(0x05000602) = 0x7FFF;
  R16(0x07000400) = 30 | (0 << 14);          // y = 30, quadrado
  R16(0x07000402) = 40 | (1 << 14);          // x = 40, 16x16
  R16(0x07000404) = 0;                        // tile 0
  for (int i = 1; i < 128; i++) R16(0x07000400 + i * 8) = 0x200;  // escondidos
  DISPCNT_B = 0x10000 | 5 | 0x800 | 0x1000 | 0x10;
  // 12) engine A: 3D na BG0 e uma BG de texto atrás
  VRAMCNT(0) = 0x81;  // A -> BG da engine A
  VRAMCNT(1) = 0x80;  // B em LCDC para gravar a textura
  for (int i = 0; i < 16; i++) R32(0x06004000 + 64 + i * 4) = 0x02020202;  // tile 1, 256 cores, cor 2
  for (int i = 0; i < 64; i++) R16(0x06000000 + i * 2) = 1;               // 2 linhas de tiles
  for (int i = 64; i < 1024; i++) R16(0x06000000 + i * 2) = 0;
  R16(0x05000004) = 0x03FF;  // amarelo
  R16(0x05000000) = 0;
  BG1CNT_A = 1 | (1 << 2) | 0x80;
  BG0CNT_A = 0;
  for (int i = 0; i < 64; i++) R16(0x06820000 + i * 2) = 0x83E0;  // textura 8x8 verde (cor direta)
  VRAMCNT(1) = 0x83;  // B -> textura (slot 0)
  DISPCNT_A = 0x10000 | 0x8 | 0x100 | 0x200;
  DISP3DCNT = 1;
  CLEAR_COLOR = 0x2800 | (0u << 16);  // transparente
  CLEAR_DEPTH = 0x7FFF;
  GFX(0x60) = 0 | (0 << 8) | (255 << 16) | (191u << 24);  // VIEWPORT
  GFX(0x10) = 0; GFX(0x15) = 0;  // projeção = identidade
  GFX(0x10) = 2; GFX(0x15) = 0;  // posição = identidade
  GFX(0x29) = (31u << 16) | 0xC0 | (1u << 24);  // POLYGON_ATTR
  GFX(0x2A) = 0;
  GFX(0x40) = 0;  // triângulos
  GFX(0x20) = 0x001F;
  gfxVtx(-4096, 4096, 0); gfxVtx(-4096, -4096, 0); gfxVtx(0, -4096, 0);
  GFX(0x41) = 0;
  GFX(0x29) = (31u << 16) | 0xC0 | (2u << 24);
  GFX(0x2A) = (0 >> 3) | (1u << 16) | (1u << 17) | (0u << 20) | (0u << 23) | (7u << 26);
  GFX(0x20) = 0x7FFF;
  GFX(0x40) = 1;  // quadriláteros
  GFX(0x22) = 0;               gfxVtx(819, 2048, 0);
  GFX(0x22) = (128u << 16);    gfxVtx(819, -2048, 0);
  GFX(0x22) = 128 | (128u << 16); gfxVtx(3276, -2048, 0);
  GFX(0x22) = 128;             gfxVtx(3276, 2048, 0);
  GFX(0x41) = 0;
  GFX(0x50) = 0;  // SWAP_BUFFERS
  RES[40] = GXSTAT;
  swi_vblank(); swi_vblank();
  RES[41] = vblanks;
  RES[255] = 0xD0D0D0D0;
  // cena 2 (depois do botão A): efeitos 2D na engine A e brilho mestre na engine B
  while (KEYINPUT & 1) swi_vblank();
  DISPCNT_A = 0;
  VRAMCNT(0) = 0x81;                    // A -> BG da engine A
  VRAMCNT(5) = 0x80;                    // F em LCDC para gravar a paleta estendida de sprites
  VRAMCNT(4) = 0x82;                    // E -> OBJ da engine A
  for (int i = 0; i < 8; i++) R32(0x06000000 + 32 + i * 4) = 0x11111111;  // tile 1 (16 cores): cor 1
  for (int i = 0; i < 8; i++) R32(0x06000000 + 64 + i * 4) = 0x22222222;  // tile 2: cor 2
  for (int i = 0; i < 1024; i++) { R16(0x06008000 + i * 2) = 1; R16(0x06008800 + i * 2) = 2; }  // mapas
  R16(0x05000002) = 0x001F;             // vermelho
  R16(0x05000004) = 0x7C00;             // azul
  R16(0x05000000) = 0;
  BG0CNT_A = 1 | (0 << 2) | (16 << 8);  // prioridade 1, mapa em 0x8000
  BG1CNT_A = 0 | (0 << 2) | (17 << 8);  // prioridade 0, mapa em 0x8800
  BLDCNT_A = 0x02 | (0x01 << 8) | (1 << 6);  // BG1 sobre BG0, alfa
  R16(0x04000052) = 8 | (8 << 8);       // EVA = EVB = 8
  R16(0x04000040) = (0 << 8) | 128;     // WIN0H: x 0..127
  R16(0x04000044) = (0 << 8) | 96;      // WIN0V: y 0..95
  R16(0x04000048) = 0x3F;               // dentro: tudo
  R16(0x0400004A) = 0x01;               // fora: só a BG0
  for (int i = 0; i < 16; i++) R32(0x06400000 + i * 4) = 0x03030303;  // sprite 8x8, 256 cores: índice 3
  R16(0x06890000 + (1 * 256 + 3) * 2) = 0x03E0;  // paleta estendida de sprite 1, cor 3: verde
  VRAMCNT(5) = 0x85;                    // F -> paleta estendida de sprites da engine A
  R16(0x07000000) = 40 | 0x100 | 0x200 | 0x2000;  // afim, tamanho dobrado, 256 cores
  R16(0x07000002) = 40 | (0 << 9);      // grupo de parâmetros 0, 8x8
  R16(0x07000004) = 0 | (0 << 10) | (1 << 12);  // tile 0, prioridade 0, paleta 1
  R16(0x07000006) = 0x80; R16(0x0700000E) = 0; R16(0x07000016) = 0; R16(0x0700001E) = 0x80;  // escala 2x
  for (int i = 1; i < 128; i++) R16(0x07000000 + i * 8) = 0x200;
  DISPCNT_A = 0x10000 | 0 | 0x100 | 0x200 | 0x1000 | 0x2000 | 0x10 | (1u << 31);
  R16(0x0400106C) = (2 << 14) | 8;      // engine B: escurece pela metade
  swi_vblank();
  // captura da tela da engine A (256x192) no banco B
  VRAMCNT(1) = 0x80;
  DISPCAPCNT = 0x80000000u | (1u << 16) | (3u << 20);
  swi_vblank(); swi_vblank();
  RES[60] = R16(0x06820000 + (80 * 256 + 100) * 2);
  RES[61] = DISPCAPCNT >> 31;
  RES[254] = 0xD0D0D002;
  // cena 3 (depois do botão B): 3D com translucidez, toon e teste de alfa
  while (KEYINPUT & 2) swi_vblank();
  R16(0x0400106C) = 0;
  BLDCNT_A = 0;
  DISPCNT_A = 0x10000 | 0x8 | 0x100;
  BG0CNT_A = 0;
  DISP3DCNT = 0x8 | 0x4;                 // alfa ligado, teste de alfa ligado
  R8(0x04000340) = 10;                   // ALPHA_TEST_REF
  CLEAR_COLOR = 0 | (31u << 16);        // preto opaco
  for (int i = 0; i < 32; i++) R16(0x04000380 + i * 2) = i == 5 ? 0x03FF : 0;  // toon: entrada 5 amarela
  GFX(0x10) = 0; GFX(0x15) = 0;
  GFX(0x10) = 2; GFX(0x15) = 0;
  GFX(0x2A) = 0;
  // quadrado vermelho opaco no fundo (z = 0.5), à esquerda
  GFX(0x29) = (31u << 16) | 0xC0 | (1u << 24);
  GFX(0x20) = 0x001F;
  GFX(0x40) = 1;
  gfxVtx(-4096, 4096, 2048); gfxVtx(-4096, -4096, 2048); gfxVtx(0, -4096, 2048); gfxVtx(0, 4096, 2048);
  // quadrado azul translúcido (alfa 15) na frente, cobrindo o meio
  GFX(0x29) = (15u << 16) | 0xC0 | (2u << 24);
  GFX(0x40) = 1;  // o atributo novo vale a partir do próximo BEGIN_VTXS
  GFX(0x20) = 0x7C00;
  gfxVtx(-2048, 2048, 0); gfxVtx(-2048, -2048, 0); gfxVtx(2048, -2048, 0); gfxVtx(2048, 2048, 0);
  // quadrado toon (vermelho 5 -> entrada 5 da tabela) à direita embaixo
  GFX(0x29) = (31u << 16) | 0xC0 | (3u << 24) | (2 << 4);
  GFX(0x40) = 1;
  GFX(0x20) = 5;
  gfxVtx(2458, -1024, 0); gfxVtx(2458, -4096, 0); gfxVtx(4096, -4096, 0); gfxVtx(4096, -1024, 0);
  // quadrado com alfa 5 (abaixo da referência 10): some pelo teste de alfa, à direita em cima
  GFX(0x29) = (5u << 16) | 0xC0 | (4u << 24);
  GFX(0x40) = 1;
  GFX(0x20) = 0x7FFF;
  gfxVtx(2458, 4096, 0); gfxVtx(2458, 1024, 0); gfxVtx(4096, 1024, 0); gfxVtx(4096, 4096, 0);
  GFX(0x41) = 0;
  GFX(0x50) = 0;
  swi_vblank(); swi_vblank(); swi_vblank();
  RES[253] = 0xD0D0D003;
  for (;;) swi_vblank();
}
