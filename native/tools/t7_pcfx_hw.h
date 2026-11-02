/* Registradores do PC-FX usados pela BIOS de teste e pelo programa de teste do Índigo (v810-gcc).
   Espaço de E/S (instruções IN/OUT): 0x000 controle, 0x100 som, 0x300 HuC6261, 0x400/0x500 VDC A/B,
   0x600 KING, 0xC80 controle da memória de save, 0xE00 interrupções, 0xF00 temporizador. */
#ifndef T7_PCFX_HW_H
#define T7_PCFX_HW_H
#include <stdint.h>

/* o compilador chama memcpy/memset para iniciar vetores (incluído por um só arquivo de cada programa) */
void* memcpy(void* d, const void* s, unsigned long n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int v, unsigned long n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)v; return d; }

static inline void out16(uint32_t p, uint16_t v) { __asm__ volatile("out.h %0, 0[%1]" :: "r"(v), "r"(p) : "memory"); }
static inline uint16_t in16(uint32_t p) { uint16_t v; __asm__ volatile("in.h 0[%1], %0" : "=r"(v) : "r"(p) : "memory"); return v; }
static inline void out8(uint32_t p, uint8_t v) { __asm__ volatile("out.b %0, 0[%1]" :: "r"(v), "r"(p) : "memory"); }
static inline uint8_t in8(uint32_t p) { uint8_t v; __asm__ volatile("in.b 0[%1], %0" : "=r"(v) : "r"(p) : "memory"); return v; }

/* KING: registro AR em 0x600, dados em 0x604 (metade de baixo) e 0x606 (de cima) */
static inline void kw(int r, uint16_t v) { out16(0x600, (uint16_t)r); out16(0x604, v); }
static inline void kw32(int r, uint32_t v) { out16(0x600, (uint16_t)r); out16(0x604, (uint16_t)v); out16(0x606, (uint16_t)(v >> 16)); }
static inline uint16_t kr(int r) { out16(0x600, (uint16_t)r); return in16(0x604); }
static inline uint16_t kstat(void) { return in16(0x602); } /* sinais do SCSI */
static inline uint16_t kirq(void) { return in16(0x600); }  /* fontes de interrupção do KING */
/* HuC6261 */
static inline void vw(int r, uint16_t v) { out16(0x300, (uint16_t)r); out16(0x304, v); }
/* VDC (chip 0 = A, 1 = B) */
static inline void dw(int chip, int r, uint16_t v) { uint32_t b = 0x400 + chip * 0x100; out16(b, (uint16_t)r); out16(b + 4, v); }
static inline uint16_t dstat(int chip) { return in16(0x400 + chip * 0x100); }

/* sinais SCSI lidos em 0x602 */
#define S_SEL 0x02
#define S_IO 0x04
#define S_CD 0x08
#define S_MSG 0x10
#define S_REQ 0x20
#define S_BSY 0x40

#endif
