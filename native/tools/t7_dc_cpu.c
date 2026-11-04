/* Teste da CPU do Dreamcast: o mesmo C roda no SH-4 emulado (sh4-linux-gnu-gcc -m4) e no PC; o teste do Índigo
 * compara as duas saídas linha a linha. Cobre o que o GCC gera para inteiros (divisão, 64 bits, deslocamentos,
 * extensão de sinal, tabelas de switch, ponteiros de função, recursão), ponto flutuante simples e duplo (com as
 * trocas de PR/SZ do FPSCR) e, com asm no SH-4 e o equivalente em C no PC, FIPR, FTRV, FSRRA, FSCA, MAC.L, MAC.W,
 * TAS.B, XTRCT, SWAP, CMP/STR, ROTCL/ROTCR, SHAD/SHLD, NEGC e os bancos de registradores. */
#ifdef __SH4__
#include "t7_dc_hw.h"
#define OUT(s) puts_(s)
#define OUTH(v) puthex(v)
#else
#include <stdio.h>
#include <math.h>
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
#define OUT(s) fputs(s, stdout)
#define OUTH(v) printf("%08X", (unsigned)(v))
#endif
typedef unsigned long long u64;
typedef long long s64;

static u32 acc;
static void mix(u32 v) { acc = (acc ^ v) * 0x01000193u + (acc >> 7); }
static void report(const char* n) { OUT(n); OUT(" "); OUTH(acc); OUT("\n"); acc = 0; }

static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 8) ^ (seed << 13); }

static volatile u32 vin[64];
static volatile float vf[16];
static volatile double vd[8];

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static u32 op_add(u32 a, u32 b) { return a + b; }
static u32 op_sub(u32 a, u32 b) { return a - b; }
static u32 op_xor(u32 a, u32 b) { return a ^ b * 3; }
static u32 (*const ops[3])(u32, u32) = {op_add, op_sub, op_xor};

static u32 sw(u32 x) {
  switch (x & 15) {
    case 0: return x * 3;
    case 1: return x ^ 0x55;
    case 2: return x >> 3;
    case 3: return x + 1000;
    case 4: return ~x;
    case 5: return x * x;
    case 6: return x / 7;
    case 7: return x % 13;
    case 8: return x << 5;
    case 9: return (u32)((s32)x >> 4);
    case 10: return x | 0x8000;
    case 11: return x & 0xFF00;
    default: return x - 77;
  }
}

struct Big { u32 a[13]; u8 b[7]; u16 c[5]; };

/* ---- instruções especiais: asm no SH-4, C no PC */
static float fipr4(const float* a, const float* b) {
#ifdef __SH4__
  register float f0 __asm__("fr0") = a[0], f1 __asm__("fr1") = a[1], f2 __asm__("fr2") = a[2], f3 __asm__("fr3") = a[3];
  register float f4 __asm__("fr4") = b[0], f5 __asm__("fr5") = b[1], f6 __asm__("fr6") = b[2], f7 __asm__("fr7") = b[3];
  u32 tmp, sav;
  __asm__ volatile("sts fpscr, %9\n\tmov %9, %8\n\tmov.l 1f, r0\n\tand r0, %8\n\tlds %8, fpscr\n\tfipr fv4, fv0\n\tlds %9, fpscr\n\tbra 2f\n\tnop\n\t.align 2\n1:\t.long 0xFFE7FFFF\n2:"
                   : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3), "+f"(f4), "+f"(f5), "+f"(f6), "+f"(f7), "=&r"(tmp), "=&r"(sav)
                   :
                   : "r0");
  return f3;
#else
  float s = a[0] * b[0];
  s = s + a[1] * b[1];
  s = s + a[2] * b[2];
  s = s + a[3] * b[3];
  return s;
#endif
}

#ifdef __SH4__
/* FTRV: matriz em XF0-XF15 (carregada com FRCHG), vetor em FV0 */
static void ftrv4(const float* m, float* v) {
  __asm__ volatile(
      "sts fpscr, r1\n\t"
      "mov r1, r4\n\t"
      "mov.l 1f, r2\n\t"
      "and r2, r1\n\t"
      "lds r1, fpscr\n\t"   /* PR = 0, SZ = 0 */
      "frchg\n\t"
      "fmov.s @%0+, fr0\n\tfmov.s @%0+, fr1\n\tfmov.s @%0+, fr2\n\tfmov.s @%0+, fr3\n\t"
      "fmov.s @%0+, fr4\n\tfmov.s @%0+, fr5\n\tfmov.s @%0+, fr6\n\tfmov.s @%0+, fr7\n\t"
      "fmov.s @%0+, fr8\n\tfmov.s @%0+, fr9\n\tfmov.s @%0+, fr10\n\tfmov.s @%0+, fr11\n\t"
      "fmov.s @%0+, fr12\n\tfmov.s @%0+, fr13\n\tfmov.s @%0+, fr14\n\tfmov.s @%0+, fr15\n\t"
      "frchg\n\t"
      "fmov.s @%1, fr0\n\tmov %1, r3\n\tadd #4, r3\n\tfmov.s @r3, fr1\n\tadd #4, r3\n\tfmov.s @r3, fr2\n\tadd #4, r3\n\tfmov.s @r3, fr3\n\t"
      "ftrv xmtrx, fv0\n\t"
      "fmov.s fr0, @%1\n\tmov %1, r3\n\tadd #4, r3\n\tfmov.s fr1, @r3\n\tadd #4, r3\n\tfmov.s fr2, @r3\n\tadd #4, r3\n\tfmov.s fr3, @r3\n\t"
      "lds r4, fpscr\n\t"
      "bra 3f\n\tnop\n\t.align 2\n1:\t.long 0xFFE7FFFF\n3:"
      : "+r"(m)
      : "r"(v)
      : "r1", "r2", "r3", "r4", "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12",
        "fr13", "fr14", "fr15", "memory");
}
#else
static void ftrv4(const float* m, float* v) {
  float r[4];
  int i;
  for (i = 0; i < 4; i++) {
    float s = m[i] * v[0];
    s = s + m[4 + i] * v[1];
    s = s + m[8 + i] * v[2];
    s = s + m[12 + i] * v[3];
    r[i] = s;
  }
  for (i = 0; i < 4; i++) v[i] = r[i];
}
#endif

static u32 fsrra_bits(float x) {
  union { float f; u32 u; } r;
#ifdef __SH4__
  register float f __asm__("fr4") = x;
  u32 tmp, sav;
  __asm__ volatile("sts fpscr, %2\n\tmov %2, %1\n\tmov.l 1f, r0\n\tand r0, %1\n\tlds %1, fpscr\n\tfsrra fr4\n\tlds %2, fpscr\n\tbra 3f\n\tnop\n\t.align 2\n1:\t.long 0xFFE7FFFF\n3:"
                   : "+f"(f), "=&r"(tmp), "=&r"(sav) : : "r0");
  r.f = f;
#else
  r.f = 1.0f / sqrtf(x);
#endif
  return r.u;
}
static u32 fsca_bits(u32 a, int cosv) {
  union { float f; u32 u; } r;
#ifdef __SH4__
  register float s __asm__("fr2"), c __asm__("fr3");
  u32 tmp, sav;
  __asm__ volatile("sts fpscr, %3\n\tmov %3, %2\n\tmov.l 1f, r0\n\tand r0, %2\n\tlds %2, fpscr\n\tlds %4, fpul\n\tfsca fpul, dr2\n\tlds %3, fpscr\n\tbra 3f\n\tnop\n\t.align 2\n1:\t.long 0xFFE7FFFF\n3:"
                   : "=f"(s), "=f"(c), "=&r"(tmp), "=&r"(sav) : "r"(a) : "r0");
  r.f = cosv ? c : s;
#else
  double ang = (double)(a & 0xFFFF) * 6.283185307179586 / 65536.0;
  if ((a & 0xFFFF) % 0x4000 == 0) {
    static const float ex[4] = {0, 1, 0, -1};
    r.f = ex[((a & 0xFFFF) / 0x4000 + (cosv ? 1 : 0)) & 3];
  } else {
    r.f = (float)sin(cosv ? (double)((a + 0x4000) & 0xFFFF) * 6.283185307179586 / 65536.0 : ang);
  }
#endif
  return r.u;
}

static u64 macl_t(const s32* a, const s32* b, int n) {
#ifdef __SH4__
  u32 hi, lo;
  const s32* pa = a;
  const s32* pb = b;
  __asm__ volatile("clrmac\n\tclrs\n" : : : "macl", "mach");
  while (n--) __asm__ volatile("mac.l @%0+, @%1+" : "+r"(pa), "+r"(pb) : : "macl", "mach", "memory");
  __asm__ volatile("sts mach, %0\n\tsts macl, %1" : "=r"(hi), "=r"(lo));
  return (u64)hi << 32 | lo;
#else
  s64 s = 0;
  while (n--) s += (s64)*a++ * *b++;
  return (u64)s;
#endif
}
static u64 macw_t(const short* a, const short* b, int n) {
#ifdef __SH4__
  u32 hi, lo;
  const short* pa = a;
  const short* pb = b;
  __asm__ volatile("clrmac\n\tclrs\n" : : : "macl", "mach");
  while (n--) __asm__ volatile("mac.w @%0+, @%1+" : "+r"(pa), "+r"(pb) : : "macl", "mach", "memory");
  __asm__ volatile("sts mach, %0\n\tsts macl, %1" : "=r"(hi), "=r"(lo));
  return (u64)hi << 32 | lo;
#else
  s64 s = 0;
  while (n--) s += (s64)*a++ * *b++;
  return (u64)s;
#endif
}
static u32 misc_t(u32 x, u32 y) {
  u32 r = 0, t;
#ifdef __SH4__
  __asm__ volatile("xtrct %1, %0" : "=r"(t) : "r"(y), "0"(x)); r ^= t;
  __asm__ volatile("swap.b %1, %0" : "=r"(t) : "r"(x)); r = r * 31 + t;
  __asm__ volatile("swap.w %1, %0" : "=r"(t) : "r"(x)); r = r * 31 + t;
  __asm__ volatile("cmp/str %1, %2\n\tmovt %0" : "=r"(t) : "r"(x), "r"(y) : "t"); r = r * 31 + t;
  __asm__ volatile("shad %2, %0" : "=r"(t) : "0"(x), "r"(y) : ); r = r * 31 + t;
  __asm__ volatile("shld %2, %0" : "=r"(t) : "0"(x), "r"(y) : ); r = r * 31 + t;
  __asm__ volatile("sett\n\trotcl %0" : "=r"(t) : "0"(x) : "t"); r = r * 31 + t;
  __asm__ volatile("clrt\n\trotcr %0" : "=r"(t) : "0"(x) : "t"); r = r * 31 + t;
  __asm__ volatile("sett\n\tnegc %1, %0" : "=r"(t) : "r"(x) : "t"); r = r * 31 + t;
#else
  t = (x >> 16) | (y << 16); r ^= t;
  t = (x & 0xFFFF0000) | (x & 0xFF) << 8 | (x >> 8 & 0xFF); r = r * 31 + t;
  t = x >> 16 | x << 16; r = r * 31 + t;
  { u32 d = x ^ y; t = !(d & 0xFF) || !(d & 0xFF00) || !(d & 0xFF0000) || !(d & 0xFF000000); } r = r * 31 + t;
  { s32 s = (s32)y; if (s >= 0) t = x << (s & 31); else if ((s & 31) == 0) t = ((s32)x < 0) ? 0xFFFFFFFF : 0; else t = (u32)((s32)x >> ((~s & 31) + 1)); } r = r * 31 + t;
  { s32 s = (s32)y; if (s >= 0) t = x << (s & 31); else if ((s & 31) == 0) t = 0; else t = x >> ((~s & 31) + 1); } r = r * 31 + t;
  t = x << 1 | 1; r = r * 31 + t;
  t = x >> 1; r = r * 31 + t;
  t = 0u - x - 1; r = r * 31 + t;
#endif
  return r;
}
static u32 tas_t(volatile u8* p) {
#ifdef __SH4__
  u32 t;
  __asm__ volatile("tas.b @%1\n\tmovt %0" : "=r"(t) : "r"(p) : "t", "memory");
  return t | (u32)*p << 8;
#else
  u32 t = *p == 0;
  *p |= 0x80;
  return t | (u32)*p << 8;
#endif
}
static u32 bank_t(u32 x) {
#ifdef __SH4__
  u32 r;
  /* grava no banco que não está em uso e lê de volta */
  __asm__ volatile("ldc %1, r3_bank\n\tstc r3_bank, %0" : "=r"(r) : "r"(x));
  return r ^ 0x5A5A5A5A;
#else
  return x ^ 0x5A5A5A5A;
#endif
}

int main(void) {
  u32 i, j;
  for (i = 0; i < 64; i++) vin[i] = rnd();
  vin[3] = 0; vin[4] = 1; vin[5] = 0x80000000u; vin[6] = 0xFFFFFFFFu; vin[7] = 7;
  /* inteiros básicos */
  for (i = 0; i < 64; i++)
    for (j = 0; j < 64; j++) {
      u32 a = vin[i], b = vin[j];
      s32 sa = (s32)a, sb = (s32)b;
      mix(a + b); mix(a - b); mix(a * b); mix(a & b); mix(a | ~b);
      mix(a << (b & 31)); mix(a >> (b & 31)); mix((u32)(sa >> (b & 31)));
      mix(a < b); mix(sa < sb); mix(a >= b); mix(sa > sb);
      mix((u32)(s32)(signed char)a); mix((u32)(s32)(short)b); mix((u8)a); mix((u16)b);
    }
  report("inteiros");
  for (i = 0; i < 64; i++)
    for (j = 0; j < 64; j++) {
      u32 a = vin[i], b = vin[j];
      s32 sa = (s32)a, sb = (s32)b;
      if (b) { mix(a / b); mix(a % b); }
      if (sb && !(sa == (s32)0x80000000 && sb == -1)) { mix((u32)(sa / sb)); mix((u32)(sa % sb)); }
      mix(a / 3); mix(a / 10); mix((u32)(sa / 7)); mix(a % 1000);
    }
  report("divisao");
  for (i = 0; i < 32; i++)
    for (j = 0; j < 32; j++) {
      u64 a = (u64)vin[i] << 32 | vin[j], b = (u64)vin[j + 1] << 17 | vin[i + 2];
      s64 sa = (s64)a, sb = (s64)b;
      mix((u32)(a * b)); mix((u32)((a * b) >> 32)); mix((u32)(a + b)); mix((u32)((a - b) >> 32));
      if (b) { mix((u32)(a / b)); mix((u32)(a % b)); }
      if (sb && sb != -1) { mix((u32)(sa / sb)); mix((u32)((u64)(sa % sb) >> 3)); }
      mix((u32)(a >> (j & 63))); mix((u32)((u64)(sa >> (i + j & 63)) >> 32)); mix(a < b); mix(sa < sb);
      mix((u32)(((u64)vin[i] * vin[j]) >> 32)); mix((u32)(((s64)(s32)vin[i] * (s32)vin[j]) >> 32));
    }
  report("64bits");
  /* controle */
  mix((u32)fib(18));
  for (i = 0; i < 300; i++) { mix(sw(vin[i & 63] + i)); mix(ops[i % 3](vin[i & 63], i)); }
  {
    struct Big x, y;
    for (i = 0; i < 13; i++) x.a[i] = vin[i];
    for (i = 0; i < 7; i++) x.b[i] = (u8)vin[i + 20];
    for (i = 0; i < 5; i++) x.c[i] = (u16)vin[i + 30];
    y = x;
    for (i = 0; i < 13; i++) mix(y.a[i]);
    for (i = 0; i < 7; i++) mix(y.b[i]);
    for (i = 0; i < 5; i++) mix(y.c[i]);
  }
  {
    u32 arr[100];
    for (i = 0; i < 100; i++) arr[i] = rnd() % 1000;
    for (i = 1; i < 100; i++) { u32 k = arr[i]; int p = (int)i - 1; while (p >= 0 && arr[p] > k) { arr[p + 1] = arr[p]; p--; } arr[p + 1] = k; }
    for (i = 0; i < 100; i++) mix(arr[i]);
  }
  {
    u32 crc = 0xFFFFFFFFu;
    for (i = 0; i < 2000; i++) {
      crc ^= (u8)(i * 13);
      for (j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    mix(crc);
  }
  report("controle");
  /* ponto flutuante simples */
  for (i = 0; i < 16; i++) vf[i] = (float)(s32)(vin[i] >> 8) / 1024.0f;
  vf[0] = 0.0f; vf[1] = -1.5f; vf[2] = 3.25f;
  for (i = 0; i < 16; i++)
    for (j = 0; j < 16; j++) {
      float a = vf[i], b = vf[j];
      union { float f; u32 u; } r;
      r.f = a + b; mix(r.u); r.f = a - b; mix(r.u); r.f = a * b; mix(r.u);
      if (b != 0.0f) { r.f = a / b; mix(r.u); }
      mix(a < b); mix(a == b); mix((u32)(s32)a); mix((u32)(s32)(a * 1000.0f));
      r.f = a * b + a; mix(r.u);
      r.f = (float)(s32)vin[i] ; mix(r.u);
      if (a >= 0) { r.f = __builtin_sqrtf(a); mix(r.u); }
      r.f = a < 0 ? -a : a; mix(r.u);
    }
  report("float");
  for (i = 0; i < 8; i++) vd[i] = (double)(s32)vin[i + 8] / 3.0;
  for (i = 0; i < 8; i++)
    for (j = 0; j < 8; j++) {
      double a = vd[i], b = vd[j];
      union { double d; u64 u; } r;
      union { float f; u32 u; } q;
      r.d = a + b; mix((u32)r.u); mix((u32)(r.u >> 32));
      r.d = a * b; mix((u32)r.u); mix((u32)(r.u >> 32));
      if (b != 0) { r.d = a / b; mix((u32)r.u); mix((u32)(r.u >> 32)); }
      if (a >= 0) { r.d = __builtin_sqrt(a); mix((u32)r.u); }
      mix(a < b); mix((u32)(s32)(a / 1e3)); q.f = (float)(a * b); mix(q.u);
      r.d = (double)vf[i] * vf[j]; mix((u32)r.u); mix((u32)(r.u >> 32));
    }
  report("double");
  /* instruções especiais */
  {
    float a[4], b[4], m[16], v[4];
    union { float f; u32 u; } r;
    for (i = 0; i < 4; i++) { a[i] = vf[i + 3]; b[i] = vf[i + 8]; }
    r.f = fipr4(a, b); mix(r.u);
    for (i = 0; i < 16; i++) m[i] = vf[i] * 0.5f + (float)i;
    for (i = 0; i < 4; i++) v[i] = vf[i + 4];
    ftrv4(m, v);
    for (i = 0; i < 4; i++) { r.f = v[i]; mix(r.u); }
    for (i = 1; i < 20; i++) mix(fsrra_bits((float)i * 0.75f));
    for (i = 0; i < 0x10000; i += 0x0FFF) { mix(fsca_bits(i, 0)); mix(fsca_bits(i, 1)); }
    mix(fsca_bits(0x4000, 0)); mix(fsca_bits(0x8000, 1));
  }
  report("fpu");
  {
    s32 la[16], lb[16];
    short wa[16], wb[16];
    volatile u8 t[2] = {0, 5};
    u64 r;
    for (i = 0; i < 16; i++) { la[i] = (s32)vin[i]; lb[i] = (s32)vin[i + 16]; wa[i] = (short)vin[i + 32]; wb[i] = (short)vin[i + 48]; }
    r = macl_t(la, lb, 16); mix((u32)r); mix((u32)(r >> 32));
    r = macw_t(wa, wb, 16); mix((u32)r); mix((u32)(r >> 32));
    for (i = 0; i < 40; i++) mix(misc_t(vin[i], (i & 1) ? vin[i + 1] : (u32)((s32)i - 20)));
    mix(tas_t(&t[0])); mix(tas_t(&t[0])); mix(tas_t(&t[1]));
    mix(bank_t(0x12345678));
  }
  report("especiais");
  OUT("FIM\n");
  return 0;
}
