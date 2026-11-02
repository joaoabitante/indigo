/* Teste de CPU do Índigo para o PC-FX: contas de inteiros (32 e 64 bits), divisões, deslocamentos e ponto
   flutuante simples com entradas pseudoaleatórias. O mesmo arquivo compilado no PC (-DHOST=arquivo) grava os
   valores esperados: as duas CPUs seguem as mesmas regras de C e do IEEE 754. No console, cputest() é chamada
   pelo programa de teste (t7_pcfx_prog.c) e guarda os resultados em 0x110000. */
#include <stdint.h>
#ifdef HOST
#include <math.h>
#include <stdio.h>
/* o v810-gcc 4.7 converte real -> inteiro com CVT.SW (arredonda para o mais próximo) */
#define F2I(x) ((int32_t)nearbyintf(x))
static uint32_t res_buf[4096];
#define RES res_buf
#else
#define RES ((volatile uint32_t*)0x00110000)
#define F2I(x) ((int32_t)(x))
#endif

static uint32_t seed;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static uint32_t n;
static void put(uint32_t v) { RES[n++ & 4095] = v; }
static uint32_t fbits(float f) { union { float f; uint32_t u; } x; x.f = f; return x.u; }
static float rf(void) {
  uint32_t r = rnd();
  float m = (float)(int32_t)(r & 0xFFFFFF) / 4096.0f;
  if (r & 0x1000000) m = -m;
  return m * (float)(1 << ((r >> 26) & 15)) / 1024.0f + 0.5f;
}

uint32_t cputest(void) {
  volatile int32_t vs[2];
  volatile uint32_t vu[2];
  seed = 0x2468ACE1;
  n = 0;
  for (int i = 0; i < 80; i++) {
    uint32_t a = rnd(), b = rnd();
    if (i & 1) b &= 0xFFFF;
    if ((i & 7) == 3) b = 0xFFFFFFFF;
    vu[0] = a; vu[1] = b ? b : 1;
    vs[0] = (int32_t)a; vs[1] = (int32_t)(b ? b : 3);
    uint32_t ua = vu[0], ub = vu[1];
    int32_t sa = vs[0], sb = vs[1];
    if (sa == (int32_t)0x80000000 && sb == -1) sb = 7;
    put(ua + ub); put(ua - ub); put(ua * ub); put(ua / ub); put(ua % ub);
    put((uint32_t)(sa / sb)); put((uint32_t)(sa % sb));
    put(ua << (ub & 31)); put(ua >> (ub & 31)); put((uint32_t)(sa >> (ub & 31)));
    put(ua & ub); put(ua | ub); put(ua ^ ub); put(~ua);
    put((uint32_t)(sa < sb) | (uint32_t)(ua < ub) << 1 | (uint32_t)(sa <= sb) << 2 | (uint32_t)(ua >= ub) << 3);
    uint64_t p = (uint64_t)ua * ub;
    int64_t q = (int64_t)sa * sb;
    put((uint32_t)p); put((uint32_t)(p >> 32)); put((uint32_t)q); put((uint32_t)((uint64_t)q >> 32));
    put((uint32_t)(p / (ub | 1))); put((uint32_t)((int64_t)q % 1000003));
    put((uint32_t)(int8_t)a); put((uint32_t)(int16_t)b); put((a >> 16) | (b << 16));
  }
  for (int i = 0; i < 80; i++) {
    volatile float vf[2];
    vf[0] = rf(); vf[1] = rf();
    float x = vf[0], y = vf[1];
    if (y == 0.0f) y = 1.5f;
    put(fbits(x + y)); put(fbits(x - y)); put(fbits(x * y)); put(fbits(x / y));
    put((uint32_t)(x < y) | (uint32_t)(x == y) << 1 | (uint32_t)(x >= y) << 2);
    put((uint32_t)F2I(x * 16.0f));
    put(fbits((float)(int32_t)rnd()));
    float z = x * y + x / y - y;
    put(fbits(z)); put((uint32_t)F2I(z / 65536.0f));
  }
  put(n);
#ifdef HOST
  FILE* f = fopen(HOST, "wb");
  uint32_t m = n;
  fwrite(res_buf, 4, m, f);
  fclose(f);
#endif
  return n;
}

#ifdef HOST
int main(void) { cputest(); return 0; }
#endif
