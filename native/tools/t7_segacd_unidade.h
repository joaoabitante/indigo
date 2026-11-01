/* Casos aleatórios dos testes de unidade do Sega CD (ASIC de gráficos e PCM), em C puro: o mesmo arquivo é usado
 * pelo indigo-t7_segacd (C++) e pelo refunidade (C, com o gfx.c e o pcm.c do Genesis Plus GX), então os dois lados
 * recebem exatamente as mesmas entradas. */
#ifndef T7_SEGACD_UNIDADE_H
#define T7_SEGACD_UNIDADE_H
#include <stdint.h>
#include <string.h>

#define UN_ASIC_CASOS 300
#define UN_PCM_CASOS 200

static uint32_t un_rnd(uint32_t* s) {
  uint32_t x = *s;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  return *s = x;
}

static uint64_t un_hash(uint64_t h, uint32_t v) { return (h ^ v) * 1099511628211ull; }

/* ASIC: preenche a Word RAM 2M (bytes na ordem do 68000) e os registradores; r[] =
 * {58 (tamanho/repetição), 5A (mapa), 5C (células V), 5E (buffer), 60 (deslocamento), 62 (pontos H), 64 (linhas),
 *  66 (vetores de traço), PM (prioridade 0..3)} */
static void un_asic(int k, uint8_t* w, uint16_t* r) {
  uint32_t s = 0x9E3779B9u ^ (uint32_t)(k * 2654435761u), i;
  for (i = 0; i < 0x40000; i += 4) {
    uint32_t v = un_rnd(&s);
    w[i] = (uint8_t)v; w[i + 1] = (uint8_t)(v >> 8); w[i + 2] = (uint8_t)(v >> 16); w[i + 3] = (uint8_t)(v >> 24);
  }
  r[0] = (uint16_t)(un_rnd(&s) & 7);
  r[1] = (uint16_t)un_rnd(&s);
  r[2] = (uint16_t)(un_rnd(&s) & 0x1F);
  r[3] = (uint16_t)un_rnd(&s);
  r[4] = (uint16_t)(un_rnd(&s) & 0x3F);
  r[5] = (uint16_t)(1 + un_rnd(&s) % ((k & 3) ? 96 : 511));
  r[6] = (uint16_t)(1 + un_rnd(&s) % ((k & 3) ? 32 : 255));
  r[7] = (uint16_t)un_rnd(&s);
  r[8] = (uint16_t)(un_rnd(&s) & 3);
  /* vetores de traço com posições dentro do mapa na maioria dos casos (senão quase tudo sai 0) */
  {
    const uint32_t dots = (r[0] & 4) ? 4096 : 256;
    uint32_t t = ((uint32_t)r[7] << 2) & 0x3FFF8, line;
    for (line = 0; line < r[6]; line++) {
      uint32_t x = un_rnd(&s) % (dots * 8), y = un_rnd(&s) % (dots * 8);
      int32_t dx = (int32_t)(un_rnd(&s) % 0x2000) - 0x1000, dy = (int32_t)(un_rnd(&s) % 0x2000) - 0x1000;
      uint16_t v[4];
      int j;
      if ((un_rnd(&s) & 7) == 0) { x = un_rnd(&s) & 0xFFFF; y = un_rnd(&s) & 0xFFFF; }
      v[0] = (uint16_t)x; v[1] = (uint16_t)y; v[2] = (uint16_t)dx; v[3] = (uint16_t)dy;
      for (j = 0; j < 4; j++) { w[t] = (uint8_t)(v[j] >> 8); w[t + 1] = (uint8_t)v[j]; t = (t + 2) & 0x3FFFF; }
    }
  }
}

/* PCM: uma sequência de operações: tipo 0 = escrita de registrador (a < 0x1000) ou da RAM pela janela
 * (0x1000..0x1FFF), tipo 1 = gerar n amostras */
typedef struct { uint8_t tipo; uint16_t a; uint16_t v; } un_op;

static int un_pcm(int k, un_op* ops, int max) {
  uint32_t s = 0x1234567u + (uint32_t)k * 7919u;
  int n = 0, i;
  /* RAM: 16 bancos com ondas aleatórias e marcas de laço (0xFF) */
  for (i = 0; i < 16 && n + 2 < max; i++) {
    int j, len = 64 + (int)(un_rnd(&s) % 512);
    ops[n].tipo = 0; ops[n].a = 7; ops[n].v = (uint16_t)i; n++;
    for (j = 0; j < len && n < max; j++) {
      uint8_t b = (uint8_t)un_rnd(&s);
      if (j == len - 1 || (un_rnd(&s) % 97) == 0) b = 0xFF;
      ops[n].tipo = 0; ops[n].a = (uint16_t)(0x1000 + j); ops[n].v = b; n++;
    }
  }
  /* registradores e trechos de som */
  for (i = 0; i < 60 && n + 12 < max; i++) {
    int c = (int)(un_rnd(&s) % 10);
    if (c < 6) { ops[n].tipo = 0; ops[n].a = (uint16_t)(un_rnd(&s) % 9); ops[n].v = (uint8_t)un_rnd(&s); n++; }
    if (c == 6) { ops[n].tipo = 0; ops[n].a = 7; ops[n].v = (uint16_t)(0xC0 | (un_rnd(&s) & 7)); n++; }
    if (c == 7) { ops[n].tipo = 0; ops[n].a = 8; ops[n].v = (uint8_t)un_rnd(&s); n++; }
    if (c == 8) { ops[n].tipo = 0; ops[n].a = 3; ops[n].v = (uint16_t)(un_rnd(&s) & 0x1F); n++; }
    ops[n].tipo = 1; ops[n].a = 0; ops[n].v = (uint16_t)(1 + un_rnd(&s) % 700); n++;
  }
  return n;
}

#endif
