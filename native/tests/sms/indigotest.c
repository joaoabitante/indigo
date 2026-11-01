// ROM de teste do Índigo para Master System / Game Gear (devkitSMS + SDCC).
// Exercita o que os exemplos do devkitSMS não cobrem: troca de bancos numa ROM de 128 KB (mapeador Sega
// ou Codemasters), RAM do cartucho (save), interrupção de linha mudando a rolagem, travas de rolagem,
// coluna esquerda apagada, sprites 8×16 com zoom e mais de 8 por linha (estouro e colisão), os 4 canais
// do som com ruído branco/periódico e o estéreo do Game Gear. Lê o controle e reage aos botões.
// Os bancos 2 a 7 são preenchidos depois da compilação por mkbanks.py com padrões conhecidos.
#include <stdbool.h>
#include "SMSlib.h"

#ifdef TARGET_GG
#define SET_BG_COLOR(i, c) GG_setBGPaletteColor((i), (c))
#define SET_SPR_COLOR(i, c) GG_setSpritePaletteColor((i), (c))
#define LEVEL(x) ((x) * 5)   /* 0–3 → 0–15 no Game Gear */
#else
#define SET_BG_COLOR(i, c) SMS_setBGPaletteColor((i), (c))
#define SET_SPR_COLOR(i, c) SMS_setSpritePaletteColor((i), (c))
#define LEVEL(x) (x)
#endif

#ifdef CODEMASTERS
#define SELECT_BANK2(n) (*(volatile unsigned char*)0x8000 = (n))
#else
#define SELECT_BANK2(n) (*(volatile unsigned char*)0xFFFF = (n))
#endif

__sfr __at 0x7F PSG_PORT;
#ifdef TARGET_GG
__sfr __at 0x06 GG_STEREO;
#endif

static unsigned char scrollX, scrollY, lineScroll, frame, bank;
static unsigned int checksum;

static void lineHandler(void) {
  // a cada interrupção de linha a rolagem horizontal muda (efeito de ondas)
  lineScroll += 3;
  SMS_setBGScrollX(scrollX + lineScroll);
}

static void psgTone(unsigned char ch, unsigned int period, unsigned char vol) {
  PSG_PORT = 0x80 | (ch << 5) | (period & 0x0F);
  PSG_PORT = (period >> 4) & 0x3F;
  PSG_PORT = 0x90 | (ch << 5) | (vol & 0x0F);
}

static void psgNoise(unsigned char mode, unsigned char vol) {
  PSG_PORT = 0xE0 | (mode & 7);
  PSG_PORT = 0xF0 | (vol & 0x0F);
}

// copia 32 tiles de um banco da ROM para a VRAM (lidos pela janela 0x8000, que muda de banco)
static void loadTilesFromBank(unsigned char b, unsigned int dstTile) {
  SELECT_BANK2(b);
  SMS_loadTiles((const void*)0x8000, dstTile, 32 * 32);
  // soma de verificação dos bytes do banco (prova que a troca de bancos lê o lugar certo)
  {
    const volatile unsigned char* p = (const volatile unsigned char*)0x8000;
    unsigned int i;
    for (i = 0; i < 256; i++) checksum += p[i];
  }
}

void main(void) {
  unsigned char i, keys, prevKeys = 0, y;
  unsigned int k;

  SMS_VRAMmemsetW(0, 0, 16 * 1024);
  // paletas: degradês diferentes para fundo e sprites
  for (i = 0; i < 16; i++) {
    SET_BG_COLOR(i, RGB(LEVEL(i & 3), LEVEL((i >> 2) & 3), LEVEL((i + 1) & 3)));
    SET_SPR_COLOR(i, RGB(LEVEL((i + 2) & 3), LEVEL(i & 3), LEVEL((i >> 2) & 3)));
  }
  for (bank = 2; bank < 8; bank++) loadTilesFromBank(bank, (bank - 2) * 32);

  // mapa de fundo 32×28 com tiles dos bancos e atributos variados (flip, paleta, prioridade)
  for (k = 0; k < 32 * 28; k++) {
    // atributos montados num byte à parte (contorna um defeito do SDCC 4.2 com "set n, iyh")
    volatile unsigned char attr = 0;
    if (k & 0x10) attr |= TILE_FLIPPED_X >> 8;
    if (k & 0x40) attr |= TILE_FLIPPED_Y >> 8;
    if ((k % 5) == 0) attr |= TILE_USE_SPRITE_PALETTE >> 8;
    if ((k % 11) == 0) attr |= TILE_PRIORITY >> 8;
    SMS_setTileatXY(k & 31, k >> 5, ((k * 7) % 192) | ((unsigned int)attr << 8));
  }

  SMS_useFirstHalfTilesforSprites(true);
  SMS_setSpriteMode(SPRITEMODE_TALL_ZOOMED);
  SMS_VDPturnOnFeature(VDPFEATURE_HIDEFIRSTCOL);
  SMS_VDPturnOnFeature(VDPFEATURE_LOCKHSCROLL);
  SMS_VDPturnOnFeature(VDPFEATURE_LOCKVSCROLL);
  SMS_setLineInterruptHandler(lineHandler);
  SMS_setLineCounter(15);
  SMS_enableLineInterrupt();

  // RAM do cartucho: liga, lê o contador salvo e incrementa a cada segundo (gera save)
  SMS_enableSRAM();
  {
    volatile unsigned char* sram = (volatile unsigned char*)0x8000;
    sram[0] = 'I'; sram[1] = 'N'; sram[2] = 'D';
  }
  SMS_disableSRAM();

#ifdef TARGET_GG
  GG_STEREO = 0xF0;
#endif
  SMS_displayOn();

  for (;;) {
    SMS_waitForVBlank();
    lineScroll = 0;
    SMS_setBGScrollY(scrollY);
    SMS_setBGScrollX(scrollX);
    SMS_copySpritestoSAT();

    keys = SMS_getKeysStatus();
    if (keys & PORT_A_KEY_RIGHT) scrollX++;
    if (keys & PORT_A_KEY_LEFT) scrollX--;
    if (keys & PORT_A_KEY_UP) scrollY = (scrollY == 0) ? 223 : scrollY - 1;
    if (keys & PORT_A_KEY_DOWN) scrollY = (scrollY >= 223) ? 0 : scrollY + 1;
    if ((keys & PORT_A_KEY_1) && !(prevKeys & PORT_A_KEY_1)) bank = (bank + 1) & 7;
    if ((keys & PORT_A_KEY_2) && !(prevKeys & PORT_A_KEY_2)) {
      SMS_enableSRAM();
      ((volatile unsigned char*)0x8000)[4 + (frame & 31)]++;
      SMS_disableSRAM();
    }
    prevKeys = keys;
    scrollX++;
    if ((frame & 3) == 0) scrollY = (scrollY >= 223) ? 0 : scrollY + 1;

    // troca de banco em plena execução e leitura (o resultado entra na posição dos sprites)
    SELECT_BANK2(2 + (frame % 6));
    y = *(volatile unsigned char*)(0x8000 + frame);

    // 12 sprites na mesma faixa: estouro de 8 por linha e colisões
    SMS_initSprites();
    for (i = 0; i < 12; i++) SMS_addSprite((i * 18 + frame) & 0xFF, 60 + (i & 1) * 4, i * 2);
    for (i = 0; i < 6; i++) SMS_addSprite((y + i * 30) & 0xFF, 120 + i * 8, 24 + i * 2);

    // som: 3 tons subindo, ruído alternando entre branco e periódico
    psgTone(0, 0x100 + frame * 3, frame & 15);
    psgTone(1, 0x3F0 - frame * 2, (frame >> 2) & 15);
    psgTone(2, 0x040 + (frame & 0x7F), (frame >> 1) & 15);
    psgNoise((frame >> 4) & 7, (frame >> 3) & 15);
#ifdef TARGET_GG
    GG_STEREO = (frame & 0x40) ? 0x5A : 0xA5;
#endif

    if ((frame & 63) == 0) {
      SMS_enableSRAM();
      ((volatile unsigned char*)0x8000)[3]++;
      SMS_disableSRAM();
    }
    frame++;
  }
}

SMS_EMBED_SEGA_ROM_HEADER(9999, 0);
SMS_EMBED_SDSC_HEADER_AUTO_DATE(1, 0, "Indigo", "Teste SMS", "ROM de teste do Indigo");
