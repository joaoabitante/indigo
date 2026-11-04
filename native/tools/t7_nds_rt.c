// Rotinas mínimas que o compilador pode chamar nos programas de teste do DS (sem biblioteca C).
typedef unsigned int size_t;
void* memcpy(void* d, const void* s, size_t n) { char* a = d; const char* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int v, size_t n) { char* a = d; while (n--) *a++ = (char)v; return d; }
void __aeabi_memcpy(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy8(void* d, const void* s, size_t n) { memcpy(d, s, n); }
void __aeabi_memset(void* d, size_t n, int v) { memset(d, v, n); }
void __aeabi_memset4(void* d, size_t n, int v) { memset(d, v, n); }
void __aeabi_memclr(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void* d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr8(void* d, size_t n) { memset(d, 0, n); }
unsigned __aeabi_uidiv(unsigned a, unsigned b) {
  unsigned q = 0, r = 0;
  if (!b) return 0;
  for (int i = 31; i >= 0; i--) { r = (r << 1) | ((a >> i) & 1); if (r >= b) { r -= b; q |= 1u << i; } }
  return q;
}
int __aeabi_idiv(int a, int b) {
  int neg = (a < 0) != (b < 0);
  unsigned q = __aeabi_uidiv(a < 0 ? -a : a, b < 0 ? -b : b);
  return neg ? -(int)q : (int)q;
}
