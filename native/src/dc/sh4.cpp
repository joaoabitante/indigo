// CPU Hitachi SH-4 do Dreamcast: instruções inteiras, FPU (precisão simples e dupla, bancos FR/XF, FMOV de 64 bits
// com SZ, FIPR, FTRV, FSCA, FSRRA), desvios com slot, exceções e interrupções. Escrito a partir do manual de
// programação do SH-4 (Hitachi/Renesas). Cada instrução conta 1 ciclo.
//
// Convenção: durante uma instrução, `pc` já aponta para a seguinte (endereço da instrução + 2); a base dos
// endereços relativos ao PC é pc + 2 (endereço da instrução + 4), também no slot de um desvio.
#include <cmath>
#include <cstring>

#include "dc.h"

namespace indigo::dc {

namespace {
constexpr uint32_t SR_T = 1, SR_S = 2, SR_Q = 0x100, SR_M = 0x200, SR_FD = 0x8000, SR_BL = 0x10000000,
                   SR_RB = 0x20000000, SR_MD = 0x40000000;
constexpr uint32_t FP_PR = 0x80000, FP_SZ = 0x100000, FP_FR = 0x200000;

using Op = void (*)(Sh4&, uint32_t);
Op table[65536];
float sinTab[65536];

inline float F(const uint32_t& x) { float f; std::memcpy(&f, &x, 4); return f; }
inline uint32_t B(float f) { uint32_t x; std::memcpy(&x, &f, 4); return x; }
inline double getD(const uint32_t* a, int n) {
  const uint64_t b = uint64_t(a[n]) << 32 | a[n + 1];
  double d; std::memcpy(&d, &b, 8); return d;
}
inline void setD(uint32_t* a, int n, double d) {
  uint64_t b; std::memcpy(&b, &d, 8);
  a[n] = uint32_t(b >> 32); a[n + 1] = uint32_t(b);
}
inline void setT(Sh4& c, bool t) { c.sr = (c.sr & ~SR_T) | (t ? SR_T : 0); }
inline uint32_t T(const Sh4& c) { return c.sr & SR_T; }

#define N ((op >> 8) & 15)
#define M ((op >> 4) & 15)
#define I8 (op & 0xFF)
#define SI8 int32_t(int8_t(op & 0xFF))

// ---------------------------------------------------------------- transferências
void movRR(Sh4& c, uint32_t op) { c.r[N] = c.r[M]; }
void movI(Sh4& c, uint32_t op) { c.r[N] = uint32_t(SI8); }
void movwPc(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int16_t(c.rd16(c.pc + 2 + I8 * 2))); }
void movlPc(Sh4& c, uint32_t op) { c.r[N] = c.rd32(((c.pc + 2) & ~3u) + I8 * 4); }
void mova(Sh4& c, uint32_t op) { c.r[0] = ((c.pc + 2) & ~3u) + I8 * 4; }
void movbS(Sh4& c, uint32_t op) { c.wr8(c.r[N], uint8_t(c.r[M])); }
void movwS(Sh4& c, uint32_t op) { c.wr16(c.r[N], uint16_t(c.r[M])); }
void movlS(Sh4& c, uint32_t op) { c.wr32(c.r[N], c.r[M]); }
void movbL(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int8_t(c.rd8(c.r[M]))); }
void movwL(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int16_t(c.rd16(c.r[M]))); }
void movlL(Sh4& c, uint32_t op) { c.r[N] = c.rd32(c.r[M]); }
void movbM(Sh4& c, uint32_t op) { const uint32_t a = c.r[N] - 1; c.wr8(a, uint8_t(c.r[M])); c.r[N] = a; }
void movwM(Sh4& c, uint32_t op) { const uint32_t a = c.r[N] - 2; c.wr16(a, uint16_t(c.r[M])); c.r[N] = a; }
void movlM(Sh4& c, uint32_t op) { const uint32_t a = c.r[N] - 4; c.wr32(a, c.r[M]); c.r[N] = a; }
void movbP(Sh4& c, uint32_t op) { const uint32_t v = uint32_t(int8_t(c.rd8(c.r[M]))); if (N != M) c.r[M] += 1; c.r[N] = v; }
void movwP(Sh4& c, uint32_t op) { const uint32_t v = uint32_t(int16_t(c.rd16(c.r[M]))); if (N != M) c.r[M] += 2; c.r[N] = v; }
void movlP(Sh4& c, uint32_t op) { const uint32_t v = c.rd32(c.r[M]); if (N != M) c.r[M] += 4; c.r[N] = v; }
void movbS0(Sh4& c, uint32_t op) { c.wr8(c.r[N] + c.r[0], uint8_t(c.r[M])); }
void movwS0(Sh4& c, uint32_t op) { c.wr16(c.r[N] + c.r[0], uint16_t(c.r[M])); }
void movlS0(Sh4& c, uint32_t op) { c.wr32(c.r[N] + c.r[0], c.r[M]); }
void movbL0(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int8_t(c.rd8(c.r[M] + c.r[0]))); }
void movwL0(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int16_t(c.rd16(c.r[M] + c.r[0]))); }
void movlL0(Sh4& c, uint32_t op) { c.r[N] = c.rd32(c.r[M] + c.r[0]); }
void movlS4(Sh4& c, uint32_t op) { c.wr32(c.r[N] + (op & 15) * 4, c.r[M]); }
void movlL4(Sh4& c, uint32_t op) { c.r[N] = c.rd32(c.r[M] + (op & 15) * 4); }
void movbS4(Sh4& c, uint32_t op) { c.wr8(c.r[M] + (op & 15), uint8_t(c.r[0])); }
void movwS4(Sh4& c, uint32_t op) { c.wr16(c.r[M] + (op & 15) * 2, uint16_t(c.r[0])); }
void movbL4(Sh4& c, uint32_t op) { c.r[0] = uint32_t(int8_t(c.rd8(c.r[M] + (op & 15)))); }
void movwL4(Sh4& c, uint32_t op) { c.r[0] = uint32_t(int16_t(c.rd16(c.r[M] + (op & 15) * 2))); }
void movbSG(Sh4& c, uint32_t op) { c.wr8(c.gbr + I8, uint8_t(c.r[0])); }
void movwSG(Sh4& c, uint32_t op) { c.wr16(c.gbr + I8 * 2, uint16_t(c.r[0])); }
void movlSG(Sh4& c, uint32_t op) { c.wr32(c.gbr + I8 * 4, c.r[0]); }
void movbLG(Sh4& c, uint32_t op) { c.r[0] = uint32_t(int8_t(c.rd8(c.gbr + I8))); }
void movwLG(Sh4& c, uint32_t op) { c.r[0] = uint32_t(int16_t(c.rd16(c.gbr + I8 * 2))); }
void movlLG(Sh4& c, uint32_t op) { c.r[0] = c.rd32(c.gbr + I8 * 4); }
void movt(Sh4& c, uint32_t op) { c.r[N] = T(c); }
void swapb(Sh4& c, uint32_t op) { const uint32_t m = c.r[M]; c.r[N] = (m & 0xFFFF0000) | (m & 0xFF) << 8 | (m >> 8 & 0xFF); }
void swapw(Sh4& c, uint32_t op) { const uint32_t m = c.r[M]; c.r[N] = m >> 16 | m << 16; }
void xtrct(Sh4& c, uint32_t op) { c.r[N] = c.r[N] >> 16 | c.r[M] << 16; }
void movcal(Sh4& c, uint32_t op) { c.wr32(c.r[N], c.r[0]); }

// ---------------------------------------------------------------- aritméticas
void add(Sh4& c, uint32_t op) { c.r[N] += c.r[M]; }
void addI(Sh4& c, uint32_t op) { c.r[N] += uint32_t(SI8); }
void addc(Sh4& c, uint32_t op) {
  const uint32_t t1 = c.r[N] + c.r[M], t0 = c.r[N];
  c.r[N] = t1 + T(c);
  setT(c, t0 > t1 || t1 > c.r[N]);
}
void addv(Sh4& c, uint32_t op) {
  const int32_t a = int32_t(c.r[N]), b = int32_t(c.r[M]);
  const int32_t s = int32_t(uint32_t(a) + uint32_t(b));
  setT(c, ((a >= 0) == (b >= 0)) && ((s >= 0) != (a >= 0)));
  c.r[N] = uint32_t(s);
}
void cmpeqI(Sh4& c, uint32_t op) { setT(c, c.r[0] == uint32_t(SI8)); }
void cmpeq(Sh4& c, uint32_t op) { setT(c, c.r[N] == c.r[M]); }
void cmphs(Sh4& c, uint32_t op) { setT(c, c.r[N] >= c.r[M]); }
void cmpge(Sh4& c, uint32_t op) { setT(c, int32_t(c.r[N]) >= int32_t(c.r[M])); }
void cmphi(Sh4& c, uint32_t op) { setT(c, c.r[N] > c.r[M]); }
void cmpgt(Sh4& c, uint32_t op) { setT(c, int32_t(c.r[N]) > int32_t(c.r[M])); }
void cmppz(Sh4& c, uint32_t op) { setT(c, int32_t(c.r[N]) >= 0); }
void cmppl(Sh4& c, uint32_t op) { setT(c, int32_t(c.r[N]) > 0); }
void cmpstr(Sh4& c, uint32_t op) {
  const uint32_t x = c.r[N] ^ c.r[M];
  setT(c, !(x & 0xFF) || !(x & 0xFF00) || !(x & 0xFF0000) || !(x & 0xFF000000));
}
void div0s(Sh4& c, uint32_t op) {
  const bool q = c.r[N] >> 31, m = c.r[M] >> 31;
  c.sr = (c.sr & ~(SR_Q | SR_M | SR_T)) | (q ? SR_Q : 0) | (m ? SR_M : 0) | (q != m ? SR_T : 0);
}
void div0u(Sh4& c, uint32_t) { c.sr &= ~(SR_Q | SR_M | SR_T); }
void div1(Sh4& c, uint32_t op) {
  uint32_t& rn = c.r[N];
  const uint32_t rm = c.r[M];
  const bool oldQ = c.sr & SR_Q, mm = c.sr & SR_M;
  bool q = rn >> 31;
  rn = rn << 1 | T(c);
  const uint32_t t0 = rn;
  bool t1;
  if (oldQ == mm) { rn -= rm; t1 = rn > t0; } else { rn += rm; t1 = rn < t0; }
  // Q novo (tabela do manual: dá o mesmo para os dois valores do Q antigo)
  q = mm ? (q ? t1 : !t1) : (q ? !t1 : t1);
  c.sr = (c.sr & ~(SR_Q | SR_T)) | (q ? SR_Q : 0) | (q == mm ? SR_T : 0);
}
void dmuls(Sh4& c, uint32_t op) {
  const int64_t r = int64_t(int32_t(c.r[N])) * int32_t(c.r[M]);
  c.mach = uint32_t(uint64_t(r) >> 32); c.macl = uint32_t(r);
}
void dmulu(Sh4& c, uint32_t op) {
  const uint64_t r = uint64_t(c.r[N]) * c.r[M];
  c.mach = uint32_t(r >> 32); c.macl = uint32_t(r);
}
void dt(Sh4& c, uint32_t op) { c.r[N]--; setT(c, c.r[N] == 0); }
void extsb(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int8_t(c.r[M])); }
void extsw(Sh4& c, uint32_t op) { c.r[N] = uint32_t(int16_t(c.r[M])); }
void extub(Sh4& c, uint32_t op) { c.r[N] = c.r[M] & 0xFF; }
void extuw(Sh4& c, uint32_t op) { c.r[N] = c.r[M] & 0xFFFF; }
void macl(Sh4& c, uint32_t op) {
  const int32_t a = int32_t(c.rd32(c.r[N])); c.r[N] += 4;
  const int32_t b = int32_t(c.rd32(c.r[M])); c.r[M] += 4;
  int64_t acc = int64_t(uint64_t(c.mach) << 32 | c.macl);
  acc = int64_t(uint64_t(acc) + uint64_t(int64_t(a) * b));  // soma sem sinal: sem UB
  if (c.sr & SR_S) {
    if (acc > 0x00007FFFFFFFFFFFLL) acc = 0x00007FFFFFFFFFFFLL;
    if (acc < -0x0000800000000000LL) acc = -0x0000800000000000LL;
  }
  c.mach = uint32_t(uint64_t(acc) >> 32); c.macl = uint32_t(acc);
}
void macw(Sh4& c, uint32_t op) {
  const int16_t a = int16_t(c.rd16(c.r[N])); c.r[N] += 2;
  const int16_t b = int16_t(c.rd16(c.r[M])); c.r[M] += 2;
  const int32_t p = int32_t(a) * b;
  if (c.sr & SR_S) {
    int64_t s = int64_t(int32_t(c.macl)) + p;
    if (s > 0x7FFFFFFF) { s = 0x7FFFFFFF; c.mach |= 1; }
    else if (s < -0x80000000LL) { s = -0x80000000LL; c.mach |= 1; }
    c.macl = uint32_t(s);
  } else {
    const uint64_t acc = (uint64_t(c.mach) << 32 | c.macl) + uint64_t(int64_t(p));
    c.mach = uint32_t(uint64_t(acc) >> 32); c.macl = uint32_t(acc);
  }
}
void mull(Sh4& c, uint32_t op) { c.macl = c.r[N] * c.r[M]; }
void mulsw(Sh4& c, uint32_t op) { c.macl = uint32_t(int32_t(int16_t(c.r[N])) * int16_t(c.r[M])); }
void muluw(Sh4& c, uint32_t op) { c.macl = (c.r[N] & 0xFFFF) * (c.r[M] & 0xFFFF); }
void neg(Sh4& c, uint32_t op) { c.r[N] = 0u - c.r[M]; }
void negc(Sh4& c, uint32_t op) {
  const uint32_t t = 0u - c.r[M];
  c.r[N] = t - T(c);
  setT(c, 0 < t || t < c.r[N]);
}
void sub(Sh4& c, uint32_t op) { c.r[N] -= c.r[M]; }
void subc(Sh4& c, uint32_t op) {
  const uint32_t t1 = c.r[N] - c.r[M], t0 = c.r[N];
  c.r[N] = t1 - T(c);
  setT(c, t0 < t1 || t1 < c.r[N]);
}
void subv(Sh4& c, uint32_t op) {
  const int32_t a = int32_t(c.r[N]), b = int32_t(c.r[M]);
  const int32_t s = int32_t(uint32_t(a) - uint32_t(b));
  setT(c, ((a >= 0) != (b >= 0)) && ((s >= 0) != (a >= 0)));
  c.r[N] = uint32_t(s);
}

// ---------------------------------------------------------------- lógicas e deslocamentos
void and_(Sh4& c, uint32_t op) { c.r[N] &= c.r[M]; }
void andI(Sh4& c, uint32_t op) { c.r[0] &= I8; }
void andB(Sh4& c, uint32_t op) { const uint32_t a = c.gbr + c.r[0]; c.wr8(a, c.rd8(a) & I8); }
void not_(Sh4& c, uint32_t op) { c.r[N] = ~c.r[M]; }
void or_(Sh4& c, uint32_t op) { c.r[N] |= c.r[M]; }
void orI(Sh4& c, uint32_t op) { c.r[0] |= I8; }
void orB(Sh4& c, uint32_t op) { const uint32_t a = c.gbr + c.r[0]; c.wr8(a, c.rd8(a) | I8); }
void tasb(Sh4& c, uint32_t op) { const uint8_t v = c.rd8(c.r[N]); setT(c, v == 0); c.wr8(c.r[N], v | 0x80); }
void tst(Sh4& c, uint32_t op) { setT(c, (c.r[N] & c.r[M]) == 0); }
void tstI(Sh4& c, uint32_t op) { setT(c, (c.r[0] & I8) == 0); }
void tstB(Sh4& c, uint32_t op) { setT(c, (c.rd8(c.gbr + c.r[0]) & I8) == 0); }
void xor_(Sh4& c, uint32_t op) { c.r[N] ^= c.r[M]; }
void xorI(Sh4& c, uint32_t op) { c.r[0] ^= I8; }
void xorB(Sh4& c, uint32_t op) { const uint32_t a = c.gbr + c.r[0]; c.wr8(a, c.rd8(a) ^ I8); }
void rotl(Sh4& c, uint32_t op) { const uint32_t t = c.r[N] >> 31; c.r[N] = c.r[N] << 1 | t; setT(c, t); }
void rotr(Sh4& c, uint32_t op) { const uint32_t t = c.r[N] & 1; c.r[N] = c.r[N] >> 1 | t << 31; setT(c, t); }
void rotcl(Sh4& c, uint32_t op) { const uint32_t t = c.r[N] >> 31; c.r[N] = c.r[N] << 1 | T(c); setT(c, t); }
void rotcr(Sh4& c, uint32_t op) { const uint32_t t = c.r[N] & 1; c.r[N] = c.r[N] >> 1 | T(c) << 31; setT(c, t); }
void shad(Sh4& c, uint32_t op) {
  const int32_t s = int32_t(c.r[M]);
  int32_t& n = reinterpret_cast<int32_t&>(c.r[N]);
  if (s >= 0) c.r[N] <<= (s & 31);
  else if ((s & 31) == 0) n = n < 0 ? -1 : 0;
  else n >>= ((~s & 31) + 1);
}
void shld(Sh4& c, uint32_t op) {
  const int32_t s = int32_t(c.r[M]);
  if (s >= 0) c.r[N] <<= (s & 31);
  else if ((s & 31) == 0) c.r[N] = 0;
  else c.r[N] >>= ((~s & 31) + 1);
}
void shal(Sh4& c, uint32_t op) { setT(c, c.r[N] >> 31); c.r[N] <<= 1; }
void shar(Sh4& c, uint32_t op) { setT(c, c.r[N] & 1); c.r[N] = uint32_t(int32_t(c.r[N]) >> 1); }
void shlr(Sh4& c, uint32_t op) { setT(c, c.r[N] & 1); c.r[N] >>= 1; }
void shll2(Sh4& c, uint32_t op) { c.r[N] <<= 2; }
void shlr2(Sh4& c, uint32_t op) { c.r[N] >>= 2; }
void shll8(Sh4& c, uint32_t op) { c.r[N] <<= 8; }
void shlr8(Sh4& c, uint32_t op) { c.r[N] >>= 8; }
void shll16(Sh4& c, uint32_t op) { c.r[N] <<= 16; }
void shlr16(Sh4& c, uint32_t op) { c.r[N] >>= 16; }

// ---------------------------------------------------------------- desvios
inline uint32_t disp8(Sh4& c, uint32_t op) { return c.pc + 2 + uint32_t(SI8 * 2); }
inline uint32_t disp12(Sh4& c, uint32_t op) {
  int32_t d = int32_t(op & 0xFFF); if (d & 0x800) d -= 0x1000;
  return c.pc + 2 + uint32_t(d * 2);
}
void bf(Sh4& c, uint32_t op) { if (!T(c)) { c.pc = disp8(c, op); c.cycles++; } }
void bt(Sh4& c, uint32_t op) { if (T(c)) { c.pc = disp8(c, op); c.cycles++; } }
void bfs(Sh4& c, uint32_t op) { if (!T(c)) c.delaySlot(disp8(c, op)); }
void bts(Sh4& c, uint32_t op) { if (T(c)) c.delaySlot(disp8(c, op)); }
void bra(Sh4& c, uint32_t op) { c.delaySlot(disp12(c, op)); }
void bsr(Sh4& c, uint32_t op) { const uint32_t t = disp12(c, op); c.pr = c.pc + 2; c.delaySlot(t); }
void braf(Sh4& c, uint32_t op) { c.delaySlot(c.pc + 2 + c.r[N]); }
void bsrf(Sh4& c, uint32_t op) { const uint32_t t = c.pc + 2 + c.r[N]; c.pr = c.pc + 2; c.delaySlot(t); }
void jmp(Sh4& c, uint32_t op) { c.delaySlot(c.r[N]); }
void jsr(Sh4& c, uint32_t op) { const uint32_t t = c.r[N]; c.pr = c.pc + 2; c.delaySlot(t); }
void rts(Sh4& c, uint32_t) { c.delaySlot(c.pr); }
void rte(Sh4& c, uint32_t) {
  const uint32_t t = c.spc;
  c.setSr(c.ssr);
  c.delaySlot(t);
}

// ---------------------------------------------------------------- sistema
void clrmac(Sh4& c, uint32_t) { c.mach = c.macl = 0; }
void clrs(Sh4& c, uint32_t) { c.sr &= ~SR_S; }
void clrt(Sh4& c, uint32_t) { c.sr &= ~SR_T; }
void sets(Sh4& c, uint32_t) { c.sr |= SR_S; }
void sett(Sh4& c, uint32_t) { c.sr |= SR_T; }
void nop(Sh4&, uint32_t) {}
void sleep(Sh4& c, uint32_t) { c.sleeping = true; c.exitRun_ = true; }
void ldtlb(Sh4&, uint32_t) {}
void ocb(Sh4&, uint32_t) {}
void pref(Sh4& c, uint32_t op) { c.prefetch(c.r[N]); }
void trapa(Sh4& c, uint32_t op) { c.tra = I8 << 2; c.exception(0x160, 0x100, c.pc); }
void illegal(Sh4& c, uint32_t) {
  c.illegal++;
  c.exception(c.inSlot_ ? 0x1A0 : 0x180, 0x100, c.pc - 2);
}

void ldcSr(Sh4& c, uint32_t op) { c.setSr(c.r[N]); }
void ldcGbr(Sh4& c, uint32_t op) { c.gbr = c.r[N]; }
void ldcVbr(Sh4& c, uint32_t op) { c.vbr = c.r[N]; }
void ldcSsr(Sh4& c, uint32_t op) { c.ssr = c.r[N]; }
void ldcSpc(Sh4& c, uint32_t op) { c.spc = c.r[N]; }
void ldcDbr(Sh4& c, uint32_t op) { c.dbr = c.r[N]; }
void ldcBank(Sh4& c, uint32_t op) { c.rb[(op >> 4) & 7] = c.r[N]; }
void ldclSr(Sh4& c, uint32_t op) { const uint32_t v = c.rd32(c.r[N]); c.r[N] += 4; c.setSr(v); }
void ldclGbr(Sh4& c, uint32_t op) { c.gbr = c.rd32(c.r[N]); c.r[N] += 4; }
void ldclVbr(Sh4& c, uint32_t op) { c.vbr = c.rd32(c.r[N]); c.r[N] += 4; }
void ldclSsr(Sh4& c, uint32_t op) { c.ssr = c.rd32(c.r[N]); c.r[N] += 4; }
void ldclSpc(Sh4& c, uint32_t op) { c.spc = c.rd32(c.r[N]); c.r[N] += 4; }
void ldclDbr(Sh4& c, uint32_t op) { c.dbr = c.rd32(c.r[N]); c.r[N] += 4; }
void ldclBank(Sh4& c, uint32_t op) { c.rb[(op >> 4) & 7] = c.rd32(c.r[N]); c.r[N] += 4; }
void ldsMach(Sh4& c, uint32_t op) { c.mach = c.r[N]; }
void ldsMacl(Sh4& c, uint32_t op) { c.macl = c.r[N]; }
void ldsPr(Sh4& c, uint32_t op) { c.pr = c.r[N]; }
void ldsFpul(Sh4& c, uint32_t op) { c.fpul = c.r[N]; }
void ldsFpscr(Sh4& c, uint32_t op) { c.setFpscr(c.r[N]); }
void ldslMach(Sh4& c, uint32_t op) { c.mach = c.rd32(c.r[N]); c.r[N] += 4; }
void ldslMacl(Sh4& c, uint32_t op) { c.macl = c.rd32(c.r[N]); c.r[N] += 4; }
void ldslPr(Sh4& c, uint32_t op) { c.pr = c.rd32(c.r[N]); c.r[N] += 4; }
void ldslFpul(Sh4& c, uint32_t op) { c.fpul = c.rd32(c.r[N]); c.r[N] += 4; }
void ldslFpscr(Sh4& c, uint32_t op) { const uint32_t v = c.rd32(c.r[N]); c.r[N] += 4; c.setFpscr(v); }
void stcSr(Sh4& c, uint32_t op) { c.r[N] = c.sr; }
void stcGbr(Sh4& c, uint32_t op) { c.r[N] = c.gbr; }
void stcVbr(Sh4& c, uint32_t op) { c.r[N] = c.vbr; }
void stcSsr(Sh4& c, uint32_t op) { c.r[N] = c.ssr; }
void stcSpc(Sh4& c, uint32_t op) { c.r[N] = c.spc; }
void stcSgr(Sh4& c, uint32_t op) { c.r[N] = c.sgr; }
void stcDbr(Sh4& c, uint32_t op) { c.r[N] = c.dbr; }
void stcBank(Sh4& c, uint32_t op) { c.r[N] = c.rb[(op >> 4) & 7]; }
inline void push(Sh4& c, uint32_t op, uint32_t v) { const uint32_t a = c.r[N] - 4; c.wr32(a, v); c.r[N] = a; }
void stclSr(Sh4& c, uint32_t op) { push(c, op, c.sr); }
void stclGbr(Sh4& c, uint32_t op) { push(c, op, c.gbr); }
void stclVbr(Sh4& c, uint32_t op) { push(c, op, c.vbr); }
void stclSsr(Sh4& c, uint32_t op) { push(c, op, c.ssr); }
void stclSpc(Sh4& c, uint32_t op) { push(c, op, c.spc); }
void stclSgr(Sh4& c, uint32_t op) { push(c, op, c.sgr); }
void stclDbr(Sh4& c, uint32_t op) { push(c, op, c.dbr); }
void stclBank(Sh4& c, uint32_t op) { push(c, op, c.rb[(op >> 4) & 7]); }
void stsMach(Sh4& c, uint32_t op) { c.r[N] = c.mach; }
void stsMacl(Sh4& c, uint32_t op) { c.r[N] = c.macl; }
void stsPr(Sh4& c, uint32_t op) { c.r[N] = c.pr; }
void stsFpul(Sh4& c, uint32_t op) { c.r[N] = c.fpul; }
void stsFpscr(Sh4& c, uint32_t op) { c.r[N] = c.fpscr; }
void stslMach(Sh4& c, uint32_t op) { push(c, op, c.mach); }
void stslMacl(Sh4& c, uint32_t op) { push(c, op, c.macl); }
void stslPr(Sh4& c, uint32_t op) { push(c, op, c.pr); }
void stslFpul(Sh4& c, uint32_t op) { push(c, op, c.fpul); }
void stslFpscr(Sh4& c, uint32_t op) { push(c, op, c.fpscr); }

// ---------------------------------------------------------------- FPU
inline bool PR(const Sh4& c) { return c.fpscr & FP_PR; }
inline bool SZ(const Sh4& c) { return c.fpscr & FP_SZ; }
// registrador de 64 bits do FMOV com SZ=1: número par = DRn, ímpar = XDn
inline uint32_t* pairOf(Sh4& c, int n) { return (n & 1) ? &c.xf[n & 14] : &c.fr[n & 14]; }

void fmov(Sh4& c, uint32_t op) {
  if (!SZ(c)) { c.fr[N] = c.fr[M]; return; }
  const uint32_t* s = pairOf(c, M);
  uint32_t* d = pairOf(c, N);
  const uint32_t a = s[0], b = s[1];
  d[0] = a; d[1] = b;
}
inline void fload(Sh4& c, int n, uint32_t a) {
  if (!SZ(c)) { c.fr[n] = c.rd32(a); return; }
  uint32_t* d = pairOf(c, n);
  d[0] = c.rd32(a); d[1] = c.rd32(a + 4);
}
inline void fstore(Sh4& c, int m, uint32_t a) {
  if (!SZ(c)) { c.wr32(a, c.fr[m]); return; }
  const uint32_t* s = pairOf(c, m);
  c.wr32(a, s[0]); c.wr32(a + 4, s[1]);
}
void fmovL(Sh4& c, uint32_t op) { fload(c, N, c.r[M]); }
void fmovP(Sh4& c, uint32_t op) { const uint32_t a = c.r[M]; c.r[M] += SZ(c) ? 8 : 4; fload(c, N, a); }
void fmovL0(Sh4& c, uint32_t op) { fload(c, N, c.r[M] + c.r[0]); }
void fmovS(Sh4& c, uint32_t op) { fstore(c, M, c.r[N]); }
void fmovM(Sh4& c, uint32_t op) { const uint32_t a = c.r[N] - (SZ(c) ? 8 : 4); fstore(c, M, a); c.r[N] = a; }
void fmovS0(Sh4& c, uint32_t op) { fstore(c, M, c.r[N] + c.r[0]); }

#define FBIN(name, OPX)                                                                        \
  void name(Sh4& c, uint32_t op) {                                                             \
    if (PR(c)) { const int n = N & 14, m = M & 14; const double a = getD(c.fr, n), b = getD(c.fr, m); setD(c.fr, n, OPX); } \
    else { const float a = F(c.fr[N]), b = F(c.fr[M]); c.fr[N] = B(OPX); }                    \
  }
FBIN(fadd, a + b)
FBIN(fsub, a - b)
FBIN(fmul, a * b)
FBIN(fdiv, a / b)
#undef FBIN
void fcmpeq(Sh4& c, uint32_t op) {
  if (PR(c)) setT(c, getD(c.fr, N & 14) == getD(c.fr, M & 14));
  else setT(c, F(c.fr[N]) == F(c.fr[M]));
}
void fcmpgt(Sh4& c, uint32_t op) {
  if (PR(c)) setT(c, getD(c.fr, N & 14) > getD(c.fr, M & 14));
  else setT(c, F(c.fr[N]) > F(c.fr[M]));
}
void fmac(Sh4& c, uint32_t op) {
  if (PR(c)) return;  // indefinida com PR=1
  float t = F(c.fr[0]) * F(c.fr[M]);
  t = t + F(c.fr[N]);
  c.fr[N] = B(t);
}
void fsts(Sh4& c, uint32_t op) { c.fr[N] = c.fpul; }
void flds(Sh4& c, uint32_t op) { c.fpul = c.fr[N]; }
void float_(Sh4& c, uint32_t op) {
  if (PR(c)) setD(c.fr, N & 14, double(int32_t(c.fpul)));
  else c.fr[N] = B(float(int32_t(c.fpul)));
}
inline uint32_t trunc32(double d) {
  if (d != d) return 0x80000000u;
  if (d >= 2147483647.0) return 0x7FFFFFFFu;
  if (d <= -2147483648.0) return 0x80000000u;
  return uint32_t(int32_t(d));
}
void ftrc(Sh4& c, uint32_t op) { c.fpul = PR(c) ? trunc32(getD(c.fr, N & 14)) : trunc32(double(F(c.fr[N]))); }
void fneg(Sh4& c, uint32_t op) { c.fr[PR(c) ? (N & 14) : N] ^= 0x80000000u; }
void fabs_(Sh4& c, uint32_t op) { c.fr[PR(c) ? (N & 14) : N] &= 0x7FFFFFFFu; }
void fsqrt(Sh4& c, uint32_t op) {
  if (PR(c)) setD(c.fr, N & 14, std::sqrt(getD(c.fr, N & 14)));
  else c.fr[N] = B(std::sqrt(F(c.fr[N])));
}
void fsrra(Sh4& c, uint32_t op) { if (!PR(c)) c.fr[N] = B(1.0f / std::sqrt(F(c.fr[N]))); }
void fldi0(Sh4& c, uint32_t op) { if (!PR(c)) c.fr[N] = 0; }
void fldi1(Sh4& c, uint32_t op) { if (!PR(c)) c.fr[N] = 0x3F800000u; }
void fcnvsd(Sh4& c, uint32_t op) { if (PR(c)) setD(c.fr, N & 14, double(F(c.fpul))); }
void fcnvds(Sh4& c, uint32_t op) { if (PR(c)) c.fpul = B(float(getD(c.fr, N & 14))); }
void fipr(Sh4& c, uint32_t op) {
  if (PR(c)) return;
  const int n = (op >> 8) & 12, m = (op >> 6) & 12;
  float s = F(c.fr[n]) * F(c.fr[m]);
  s = s + F(c.fr[n + 1]) * F(c.fr[m + 1]);
  s = s + F(c.fr[n + 2]) * F(c.fr[m + 2]);
  s = s + F(c.fr[n + 3]) * F(c.fr[m + 3]);
  c.fr[n + 3] = B(s);
}
void ftrv(Sh4& c, uint32_t op) {
  if (PR(c)) return;
  const int n = (op >> 8) & 12;
  const float v0 = F(c.fr[n]), v1 = F(c.fr[n + 1]), v2 = F(c.fr[n + 2]), v3 = F(c.fr[n + 3]);
  for (int i = 0; i < 4; i++) {
    float s = F(c.xf[i]) * v0;
    s = s + F(c.xf[4 + i]) * v1;
    s = s + F(c.xf[8 + i]) * v2;
    s = s + F(c.xf[12 + i]) * v3;
    c.fr[n + i] = B(s);
  }
}
void fsca(Sh4& c, uint32_t op) {
  if (PR(c)) return;
  const int n = N & 14;
  const uint32_t a = c.fpul & 0xFFFF;
  c.fr[n] = B(sinTab[a]);
  c.fr[n + 1] = B(sinTab[(a + 0x4000) & 0xFFFF]);
}
void fschg(Sh4& c, uint32_t) { c.setFpscr(c.fpscr ^ FP_SZ); }
void frchg(Sh4& c, uint32_t) { c.setFpscr(c.fpscr ^ FP_FR); }

#undef N
#undef M
#undef I8
#undef SI8

struct Pat { uint16_t mask, val; Op fn; };
const Pat pats[] = {
    // exatas
    {0xFFFF, 0x0008, clrt}, {0xFFFF, 0x0018, sett}, {0xFFFF, 0x0028, clrmac}, {0xFFFF, 0x0038, ldtlb},
    {0xFFFF, 0x0048, clrs}, {0xFFFF, 0x0058, sets}, {0xFFFF, 0x0009, nop}, {0xFFFF, 0x0019, div0u},
    {0xFFFF, 0x000B, rts}, {0xFFFF, 0x001B, sleep}, {0xFFFF, 0x002B, rte},
    {0xFFFF, 0xF3FD, fschg}, {0xFFFF, 0xFBFD, frchg},
    // 0000
    {0xF0FF, 0x0002, stcSr}, {0xF0FF, 0x0012, stcGbr}, {0xF0FF, 0x0022, stcVbr}, {0xF0FF, 0x0032, stcSsr},
    {0xF0FF, 0x0042, stcSpc}, {0xF08F, 0x0082, stcBank}, {0xF0FF, 0x0003, bsrf}, {0xF0FF, 0x0023, braf},
    {0xF0FF, 0x0083, pref}, {0xF0FF, 0x0093, ocb}, {0xF0FF, 0x00A3, ocb}, {0xF0FF, 0x00B3, ocb},
    {0xF0FF, 0x00C3, movcal}, {0xF00F, 0x0004, movbS0}, {0xF00F, 0x0005, movwS0}, {0xF00F, 0x0006, movlS0},
    {0xF00F, 0x0007, mull}, {0xF0FF, 0x0029, movt}, {0xF0FF, 0x000A, stsMach}, {0xF0FF, 0x001A, stsMacl},
    {0xF0FF, 0x002A, stsPr}, {0xF0FF, 0x005A, stsFpul}, {0xF0FF, 0x006A, stsFpscr}, {0xF0FF, 0x00FA, stcDbr},
    {0xF0FF, 0x003A, stcSgr}, {0xF00F, 0x000C, movbL0}, {0xF00F, 0x000D, movwL0}, {0xF00F, 0x000E, movlL0},
    {0xF00F, 0x000F, macl},
    {0xF000, 0x1000, movlS4},
    // 0010
    {0xF00F, 0x2000, movbS}, {0xF00F, 0x2001, movwS}, {0xF00F, 0x2002, movlS}, {0xF00F, 0x2004, movbM},
    {0xF00F, 0x2005, movwM}, {0xF00F, 0x2006, movlM}, {0xF00F, 0x2007, div0s}, {0xF00F, 0x2008, tst},
    {0xF00F, 0x2009, and_}, {0xF00F, 0x200A, xor_}, {0xF00F, 0x200B, or_}, {0xF00F, 0x200C, cmpstr},
    {0xF00F, 0x200D, xtrct}, {0xF00F, 0x200E, muluw}, {0xF00F, 0x200F, mulsw},
    // 0011
    {0xF00F, 0x3000, cmpeq}, {0xF00F, 0x3002, cmphs}, {0xF00F, 0x3003, cmpge}, {0xF00F, 0x3004, div1},
    {0xF00F, 0x3005, dmulu}, {0xF00F, 0x3006, cmphi}, {0xF00F, 0x3007, cmpgt}, {0xF00F, 0x3008, sub},
    {0xF00F, 0x300A, subc}, {0xF00F, 0x300B, subv}, {0xF00F, 0x300C, add}, {0xF00F, 0x300D, dmuls},
    {0xF00F, 0x300E, addc}, {0xF00F, 0x300F, addv},
    // 0100
    {0xF0FF, 0x4000, shal}, {0xF0FF, 0x4010, dt}, {0xF0FF, 0x4020, shal}, {0xF0FF, 0x4001, shlr},
    {0xF0FF, 0x4011, cmppz}, {0xF0FF, 0x4021, shar}, {0xF0FF, 0x4002, stslMach}, {0xF0FF, 0x4012, stslMacl},
    {0xF0FF, 0x4022, stslPr}, {0xF0FF, 0x4052, stslFpul}, {0xF0FF, 0x4062, stslFpscr}, {0xF0FF, 0x4032, stclSgr},
    {0xF0FF, 0x40F2, stclDbr}, {0xF0FF, 0x4003, stclSr}, {0xF0FF, 0x4013, stclGbr}, {0xF0FF, 0x4023, stclVbr},
    {0xF0FF, 0x4033, stclSsr}, {0xF0FF, 0x4043, stclSpc}, {0xF08F, 0x4083, stclBank}, {0xF0FF, 0x4004, rotl},
    {0xF0FF, 0x4024, rotcl}, {0xF0FF, 0x4005, rotr}, {0xF0FF, 0x4015, cmppl}, {0xF0FF, 0x4025, rotcr},
    {0xF0FF, 0x4006, ldslMach}, {0xF0FF, 0x4016, ldslMacl}, {0xF0FF, 0x4026, ldslPr}, {0xF0FF, 0x4056, ldslFpul},
    {0xF0FF, 0x4066, ldslFpscr}, {0xF0FF, 0x40F6, ldclDbr}, {0xF0FF, 0x4007, ldclSr}, {0xF0FF, 0x4017, ldclGbr},
    {0xF0FF, 0x4027, ldclVbr}, {0xF0FF, 0x4037, ldclSsr}, {0xF0FF, 0x4047, ldclSpc}, {0xF08F, 0x4087, ldclBank},
    {0xF0FF, 0x4008, shll2}, {0xF0FF, 0x4018, shll8}, {0xF0FF, 0x4028, shll16}, {0xF0FF, 0x4009, shlr2},
    {0xF0FF, 0x4019, shlr8}, {0xF0FF, 0x4029, shlr16}, {0xF0FF, 0x400A, ldsMach}, {0xF0FF, 0x401A, ldsMacl},
    {0xF0FF, 0x402A, ldsPr}, {0xF0FF, 0x405A, ldsFpul}, {0xF0FF, 0x406A, ldsFpscr}, {0xF0FF, 0x40FA, ldcDbr},
    {0xF0FF, 0x400B, jsr}, {0xF0FF, 0x401B, tasb}, {0xF0FF, 0x402B, jmp}, {0xF00F, 0x400C, shad},
    {0xF00F, 0x400D, shld}, {0xF0FF, 0x400E, ldcSr}, {0xF0FF, 0x401E, ldcGbr}, {0xF0FF, 0x402E, ldcVbr},
    {0xF0FF, 0x403E, ldcSsr}, {0xF0FF, 0x404E, ldcSpc}, {0xF08F, 0x408E, ldcBank}, {0xF00F, 0x400F, macw},
    {0xF000, 0x5000, movlL4},
    // 0110
    {0xF00F, 0x6000, movbL}, {0xF00F, 0x6001, movwL}, {0xF00F, 0x6002, movlL}, {0xF00F, 0x6003, movRR},
    {0xF00F, 0x6004, movbP}, {0xF00F, 0x6005, movwP}, {0xF00F, 0x6006, movlP}, {0xF00F, 0x6007, not_},
    {0xF00F, 0x6008, swapb}, {0xF00F, 0x6009, swapw}, {0xF00F, 0x600A, negc}, {0xF00F, 0x600B, neg},
    {0xF00F, 0x600C, extub}, {0xF00F, 0x600D, extuw}, {0xF00F, 0x600E, extsb}, {0xF00F, 0x600F, extsw},
    {0xF000, 0x7000, addI},
    // 1000
    {0xFF00, 0x8000, movbS4}, {0xFF00, 0x8100, movwS4}, {0xFF00, 0x8400, movbL4}, {0xFF00, 0x8500, movwL4},
    {0xFF00, 0x8800, cmpeqI}, {0xFF00, 0x8900, bt}, {0xFF00, 0x8B00, bf}, {0xFF00, 0x8D00, bts},
    {0xFF00, 0x8F00, bfs},
    {0xF000, 0x9000, movwPc}, {0xF000, 0xA000, bra}, {0xF000, 0xB000, bsr},
    // 1100
    {0xFF00, 0xC000, movbSG}, {0xFF00, 0xC100, movwSG}, {0xFF00, 0xC200, movlSG}, {0xFF00, 0xC300, trapa},
    {0xFF00, 0xC400, movbLG}, {0xFF00, 0xC500, movwLG}, {0xFF00, 0xC600, movlLG}, {0xFF00, 0xC700, mova},
    {0xFF00, 0xC800, tstI}, {0xFF00, 0xC900, andI}, {0xFF00, 0xCA00, xorI}, {0xFF00, 0xCB00, orI},
    {0xFF00, 0xCC00, tstB}, {0xFF00, 0xCD00, andB}, {0xFF00, 0xCE00, xorB}, {0xFF00, 0xCF00, orB},
    {0xF000, 0xD000, movlPc}, {0xF000, 0xE000, movI},
    // 1111 (FPU)
    {0xF1FF, 0xF0FD, fsca}, {0xF3FF, 0xF1FD, ftrv}, {0xF0FF, 0xF0ED, fipr},
    {0xF0FF, 0xF00D, fsts}, {0xF0FF, 0xF01D, flds}, {0xF0FF, 0xF02D, float_}, {0xF0FF, 0xF03D, ftrc},
    {0xF0FF, 0xF04D, fneg}, {0xF0FF, 0xF05D, fabs_}, {0xF0FF, 0xF06D, fsqrt}, {0xF0FF, 0xF07D, fsrra},
    {0xF0FF, 0xF08D, fldi0}, {0xF0FF, 0xF09D, fldi1}, {0xF0FF, 0xF0AD, fcnvsd}, {0xF0FF, 0xF0BD, fcnvds},
    {0xF00F, 0xF000, fadd}, {0xF00F, 0xF001, fsub}, {0xF00F, 0xF002, fmul}, {0xF00F, 0xF003, fdiv},
    {0xF00F, 0xF004, fcmpeq}, {0xF00F, 0xF005, fcmpgt}, {0xF00F, 0xF006, fmovL0}, {0xF00F, 0xF007, fmovS0},
    {0xF00F, 0xF008, fmovL}, {0xF00F, 0xF009, fmovP}, {0xF00F, 0xF00A, fmovS}, {0xF00F, 0xF00B, fmovM},
    {0xF00F, 0xF00C, fmov}, {0xF00F, 0xF00E, fmac},
};

struct Init {
  Init() {
    for (uint32_t op = 0; op < 65536; op++) {
      table[op] = illegal;
      for (const Pat& p : pats)
        if ((op & p.mask) == p.val) { table[op] = p.fn; break; }
    }
    for (int i = 0; i < 65536; i++) sinTab[i] = float(std::sin(double(i) * 6.283185307179586 / 65536.0));
    // valores exatos nos eixos
    sinTab[0] = 0; sinTab[0x4000] = 1; sinTab[0x8000] = 0; sinTab[0xC000] = -1;
  }
} init;
}  // namespace

Sh4::Sh4(System& s) : sys(s) {}

void Sh4::reset() {
  std::memset(r, 0, sizeof r); std::memset(rb, 0, sizeof rb);
  std::memset(fr, 0, sizeof fr); std::memset(xf, 0, sizeof xf);
  sr = 0x700000F0; gbr = vbr = ssr = spc = sgr = dbr = mach = macl = pr = fpul = 0;
  fpscr = 0x00040001;
  pc = 0xA0000000;
  sleeping = false;
  mmucr = ccr = qacr[0] = qacr[1] = expevt = intevt = tra = pteh = ptel = ttb = tea = 0;
  icr = ipra = iprb = iprc = 0;
  tocr = tstr = 0;
  for (int i = 0; i < 3; i++) { tcor[i] = tcnt[i] = 0xFFFFFFFF; tcr[i] = 0; tmuFrac_[i] = 0; }
  tcpr2 = 0;
  tmuLast_ = cycles;
  for (int i = 0; i < 4; i++) sar[i] = dar[i] = dmatcr[i] = chcr[i] = 0;
  dmaor = 0;
  bcr1 = mcr = pctra = pctrb = 0;
  bcr2 = 0x3FFC; wcr1 = 0x7777; wcr2 = 0xFFFE; wcr3 = 0x7777; pcr = rtcsr = rtcnt = rtcor = rfcr = 0;
  pdtra = pdtrb = gpioic = 0; frqcr = 0x0E0A; wtcnt = 0; stbcr = stbcr2 = wtcsr = 0;
  scsmr2 = scscr2 = scfcr2 = scsptr2 = sclsr2 = 0; scfsr2 = 0x0060; scbrr2 = 0xFF;
  std::memset(sq, 0, sizeof sq);
  irl_ = 15; pendPrio_ = 0; pendCode_ = 0;
}

// ---------------------------------------------------------------- memória
uint8_t Sh4::rd8(uint32_t a) {
  if (a < 0xE0000000u) {
    if (const uint8_t* p = sys.rmap[(a >> 16) & 0x1FFF]) return p[a & 0xFFFF];
    if ((a & 0xFC000000u) == 0x7C000000u && (ccr & 0x20)) return ocram[((ccr & 0x80) ? (a >> 13 & 0x1000) : (a & 0x1000)) | (a & 0xFFF)];
    const uint32_t pa = a & 0x1FFFFFFF;
    idlePoll(a);
    if (pa >= 0x1C000000u) return uint8_t(p4Read(pa | 0xE0000000u, 1));
    return sys.read8(pa);
  }
  idlePoll(a);
  return uint8_t(p4Read(a, 1));
}
uint16_t Sh4::rd16(uint32_t a) {
  a &= ~1u;
  if (a < 0xE0000000u) {
    if (const uint8_t* p = sys.rmap[(a >> 16) & 0x1FFF]) { uint16_t v; std::memcpy(&v, p + (a & 0xFFFF), 2); return v; }
    if ((a & 0xFC000000u) == 0x7C000000u && (ccr & 0x20)) {
      uint16_t v; std::memcpy(&v, &ocram[((ccr & 0x80) ? (a >> 13 & 0x1000) : (a & 0x1000)) | (a & 0xFFF)], 2); return v;
    }
    const uint32_t pa = a & 0x1FFFFFFF;
    idlePoll(a);
    if (pa >= 0x1C000000u) return uint16_t(p4Read(pa | 0xE0000000u, 2));
    return sys.read16(pa);
  }
  idlePoll(a);
  return uint16_t(p4Read(a, 2));
}
uint32_t Sh4::rd32(uint32_t a) {
  a &= ~3u;
  if (a < 0xE0000000u) {
    if (const uint8_t* p = sys.rmap[(a >> 16) & 0x1FFF]) { uint32_t v; std::memcpy(&v, p + (a & 0xFFFF), 4); return v; }
    if ((a & 0xFC000000u) == 0x7C000000u && (ccr & 0x20)) {
      uint32_t v; std::memcpy(&v, &ocram[((ccr & 0x80) ? (a >> 13 & 0x1000) : (a & 0x1000)) | (a & 0xFFF)], 4); return v;
    }
    const uint32_t pa = a & 0x1FFFFFFF;
    idlePoll(a);
    if (pa >= 0x1C000000u) return p4Read(pa | 0xE0000000u, 4);
    return sys.read32(pa);
  }
  if (a < 0xE4000000u) return sq[(a >> 2) & 15];
  idlePoll(a);
  return p4Read(a, 4);
}

// laço de espera: a mesma instrução lendo o mesmo registrador de hardware em poucos ciclos, muitas vezes seguidas.
// O hardware só muda nos eventos agendados (ou no fim da linha), então o tempo até lá é pulado.
void Sh4::idlePoll(uint32_t a) {
  if (a == idleAddr_ && pc == idlePc_ && cycles - idleCyc_ < 48) {
    if (++idleN_ >= 16) { idleN_ = 0; idleSkip_ = true; exitRun_ = true; idleSkips++; }
  } else {
    idleAddr_ = a; idlePc_ = pc; idleN_ = 0;
  }
  idleCyc_ = cycles;
}
void Sh4::wr8(uint32_t a, uint8_t v) {
  if (a < 0xE0000000u) {
    if (uint8_t* p = sys.wmap[(a >> 16) & 0x1FFF]) { p[a & 0xFFFF] = v; return; }
    if ((a & 0xFC000000u) == 0x7C000000u && (ccr & 0x20)) { ocram[((ccr & 0x80) ? (a >> 13 & 0x1000) : (a & 0x1000)) | (a & 0xFFF)] = v; return; }
    const uint32_t pa = a & 0x1FFFFFFF;
    if (pa >= 0x1C000000u) { p4Write(pa | 0xE0000000u, v, 1); return; }
    sys.write8(pa, v);
    return;
  }
  p4Write(a, v, 1);
}
void Sh4::wr16(uint32_t a, uint16_t v) {
  a &= ~1u;
  if (a < 0xE0000000u) {
    if (uint8_t* p = sys.wmap[(a >> 16) & 0x1FFF]) { std::memcpy(p + (a & 0xFFFF), &v, 2); return; }
    if ((a & 0xFC000000u) == 0x7C000000u && (ccr & 0x20)) {
      std::memcpy(&ocram[((ccr & 0x80) ? (a >> 13 & 0x1000) : (a & 0x1000)) | (a & 0xFFF)], &v, 2); return;
    }
    const uint32_t pa = a & 0x1FFFFFFF;
    if (pa >= 0x1C000000u) { p4Write(pa | 0xE0000000u, v, 2); return; }
    sys.write16(pa, v);
    return;
  }
  p4Write(a, v, 2);
}
void Sh4::wr32(uint32_t a, uint32_t v) {
  a &= ~3u;
  if (a < 0xE0000000u) {
    if (uint8_t* p = sys.wmap[(a >> 16) & 0x1FFF]) { std::memcpy(p + (a & 0xFFFF), &v, 4); return; }
    if ((a & 0xFC000000u) == 0x7C000000u && (ccr & 0x20)) {
      std::memcpy(&ocram[((ccr & 0x80) ? (a >> 13 & 0x1000) : (a & 0x1000)) | (a & 0xFFF)], &v, 4); return;
    }
    const uint32_t pa = a & 0x1FFFFFFF;
    if (pa >= 0x1C000000u) { p4Write(pa | 0xE0000000u, v, 4); return; }
    sys.write32(pa, v);
    return;
  }
  if (a < 0xE4000000u) { sq[(a >> 2) & 15] = v; return; }
  p4Write(a, v, 4);
}

// store queue: PREF num endereço de 0xE0000000-0xE3FFFFFF manda os 32 bytes para a memória externa
void Sh4::prefetch(uint32_t a) {
  if ((a & 0xFC000000u) != 0xE0000000u) return;
  const int q = (a >> 5) & 1;
  const uint32_t ext = (a & 0x03FFFFE0u) | ((qacr[q] & 0x1C) << 24);
  sys.write32B(ext, &sq[q * 8]);
}

// ---------------------------------------------------------------- estado
void Sh4::setSr(uint32_t v) {
  v &= 0x700083F3;
  if ((v ^ sr) & SR_RB) for (int i = 0; i < 8; i++) std::swap(r[i], rb[i]);
  sr = v;
}
void Sh4::setFpscr(uint32_t v) {
  v &= 0x003FFFFF;
  if ((v ^ fpscr) & FP_FR) for (int i = 0; i < 16; i++) std::swap(fr[i], xf[i]);
  fpscr = v;
}

void Sh4::exception(uint32_t code, uint32_t vecOff, uint32_t retPc) {
  if (vecOff == 0x600) intevt = code; else expevt = code;
  spc = retPc;
  ssr = sr;
  sgr = r[15];
  setSr(sr | SR_MD | SR_RB | SR_BL);
  pc = vbr + vecOff;
}

void Sh4::delaySlot(uint32_t target) {
  const uint32_t slot = pc;
  const uint16_t op = rd16(slot);
  pc = slot + 2;
  inSlot_ = true;
  table[op](*this, op);
  inSlot_ = false;
  pc = target;
  cycles++;
}

void Sh4::execOne(uint16_t op) { table[op](*this, op); }

void Sh4::checkInterrupt() {
  if (sr & SR_BL) return;
  if (pendPrio_ <= int((sr >> 4) & 15)) return;
  sleeping = false;
  exception(pendCode_, 0x600, pc);
}

void Sh4::run(int64_t until) {
  exitRun_ = false;
  while (cycles < until) {
    if (pendPrio_) checkInterrupt();
    if (sleeping) { cycles = until; break; }
    const uint32_t a = pc;
    uint16_t op;
    if (const uint8_t* p = sys.rmap[(a >> 16) & 0x1FFF]; p && a < 0xE0000000u) std::memcpy(&op, p + (a & 0xFFFF), 2);
    else op = rd16(a);
    pc = a + 2;
    table[op](*this, op);
    cycles++;
    executed++;
    if (exitRun_) {
      if (idleSkip_) { idleSkip_ = false; if (cycles < until) cycles = until; }
      break;
    }
  }
}

void Sh4::sync(StateIO& io) {
  io.raw(r, sizeof r); io.raw(rb, sizeof rb);
  io.sync(sr); io.sync(gbr); io.sync(vbr); io.sync(ssr); io.sync(spc); io.sync(sgr); io.sync(dbr);
  io.sync(mach); io.sync(macl); io.sync(pr); io.sync(pc); io.sync(fpscr); io.sync(fpul);
  io.raw(fr, sizeof fr); io.raw(xf, sizeof xf);
  io.sync(cycles); io.sync(sleeping);
  io.sync(mmucr); io.sync(ccr); io.raw(qacr, sizeof qacr); io.sync(expevt); io.sync(intevt); io.sync(tra);
  io.sync(pteh); io.sync(ptel); io.sync(ttb); io.sync(tea);
  io.sync(icr); io.sync(ipra); io.sync(iprb); io.sync(iprc);
  io.sync(tocr); io.sync(tstr); io.raw(tcor, sizeof tcor); io.raw(tcnt, sizeof tcnt); io.sync(tcpr2);
  io.raw(tcr, sizeof tcr); io.sync(tmuLast_); io.raw(tmuFrac_, sizeof tmuFrac_);
  io.raw(sar, sizeof sar); io.raw(dar, sizeof dar); io.raw(dmatcr, sizeof dmatcr); io.raw(chcr, sizeof chcr);
  io.sync(dmaor);
  io.sync(bcr1); io.sync(mcr); io.sync(pctra); io.sync(pctrb);
  io.sync(bcr2); io.sync(wcr1); io.sync(wcr2); io.sync(wcr3); io.sync(pcr); io.sync(rtcsr); io.sync(rtcnt);
  io.sync(rtcor); io.sync(rfcr); io.sync(pdtra); io.sync(pdtrb); io.sync(gpioic); io.sync(frqcr); io.sync(wtcnt);
  io.sync(stbcr); io.sync(stbcr2); io.sync(wtcsr);
  io.sync(scsmr2); io.sync(scscr2); io.sync(scfsr2); io.sync(scfcr2); io.sync(scsptr2); io.sync(sclsr2);
  io.sync(scbrr2);
  io.raw(sq, sizeof sq); io.raw(ocram, sizeof ocram);
  io.sync(irl_);
  io.sync(idleAddr_); io.sync(idlePc_); io.sync(idleN_); io.sync(idleCyc_);
  if (io.reading()) {
    if (idleCyc_ < 0 || idleCyc_ > cycles) idleCyc_ = cycles;
    if (idleN_ > 16) idleN_ = 0;
    // estado lido: valores que viram índice ou laço voltam para a faixa do hardware
    sr &= 0x700083F3; fpscr &= 0x003FFFFF;
    clampTo(irl_, 0, 15);
    inSlot_ = false;
    updateIrq();
  }
}

}  // namespace indigo::dc
