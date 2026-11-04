// RSP: CPU escalar e unidade vetorial (COP2). Semântica das instruções vetoriais conforme a documentação pública
// do RSP (manuais do SDK do N64 e n64brew.dev); as tabelas do recíproco e da raiz recíproca são calculadas aqui.
#include "rsp.h"

#include <cmath>
#include <cstring>

#include "n64.h"

namespace indigo::n64 {

namespace {
uint16_t RCP_ROM[512], RSQ_ROM[512];
bool tabelas = false;

void montaTabelas() {
  if (tabelas) return;
  for (int i = 0; i < 512; i++) {
    // recíproco: 2^26 / (512 + i), sem o bit 1.0 implícito
    const uint64_t v = (((1ull << 34) / uint64_t(i + 512)) + 1) >> 8;
    RCP_ROM[i] = v >= 0x20000 ? 0xFFFF : uint16_t(v);
    // raiz recíproca: a entrada ímpar tem metade do valor (expoente ímpar)
    const uint64_t a = uint64_t(i + 512) >> ((i % 2 == 1) ? 1 : 0);
    // maior b com a * b^2 < 2^44 (começa pela raiz e acerta)
    uint64_t b = uint64_t(std::sqrt(double(1ull << 44) / double(a)));
    while (b > 0 && a * b * b >= (1ull << 44)) b--;
    while (a * (b + 1) * (b + 1) < (1ull << 44)) b++;
    if (b < (1ull << 17)) b = 1ull << 17;
    RSQ_ROM[i] = uint16_t(b >> 1);
  }
  tabelas = true;
}

// seleção de elementos (campo e das instruções vetoriais)
constexpr uint8_t SEL[16][8] = {
  {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7},
  {0, 0, 2, 2, 4, 4, 6, 6}, {1, 1, 3, 3, 5, 5, 7, 7},
  {0, 0, 0, 0, 4, 4, 4, 4}, {1, 1, 1, 1, 5, 5, 5, 5}, {2, 2, 2, 2, 6, 6, 6, 6}, {3, 3, 3, 3, 7, 7, 7, 7},
  {0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3, 3, 3},
  {4, 4, 4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7, 7, 7},
};

inline int64_t wrap48(int64_t v) { return int64_t(uint64_t(v) << 16) >> 16; }
inline uint16_t clampS(int64_t acc) {
  const int64_t v = acc >> 16;
  if (v < -32768) return 0x8000;
  if (v > 32767) return 0x7FFF;
  return uint16_t(v);
}
inline uint16_t clampLow(int64_t acc) {
  const int64_t v = acc >> 16;
  if (v < -32768) return 0;
  if (v > 32767) return 0xFFFF;
  return uint16_t(acc);
}
inline uint16_t clampU(int64_t acc) {
  const int64_t v = acc >> 16;
  if (v < 0) return 0;
  if (v > 32767) return 0xFFFF;
  return uint16_t(v);
}
inline uint16_t sat16(int32_t v) { return uint16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
inline int clz32(uint32_t v) { return v ? __builtin_clz(v) : 32; }
}  // namespace

Rsp::Rsp(N64& s) : sys(s) { montaTabelas(); }

void Rsp::reset() {
  pc = 0; npc_ = 4;
  delay_ = nextDelay_ = false;
  std::memset(r, 0, sizeof r);
  std::memset(vr, 0, sizeof vr);
  std::memset(acc, 0, sizeof acc);
  vco = vcc = 0; vce = 0;
  divOut = divIn = 0; divLoaded = false;
  cycles = 0;
}

uint8_t Rsp::dm(uint32_t a) const { return sys.spmem[(a & 0xFFF) ^ 3]; }
void Rsp::dmw(uint32_t a, uint8_t v) { sys.spmem[(a & 0xFFF) ^ 3] = v; }

void Rsp::run(int64_t ate) {
  while (cycles < ate) {
    if (sys.sp[4] & 1) { cycles = ate; return; }  // parado (HALT)
    uint32_t op;
    std::memcpy(&op, &sys.spmem[0x1000 + (pc & 0xFFC)], 4);
    delay_ = nextDelay_;
    nextDelay_ = false;
    cur_ = pc & 0xFFC;
    pc = npc_ & 0xFFC;
    npc_ = pc + 4;
    step(op);
    r[0] = 0;
    cycles++;
    instrucoes++;
    if (sys.sp[4] & 0x20) sys.sp[4] |= 1;  // passo a passo
  }
}

void Rsp::step(uint32_t op) {
  const int rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  const uint32_t imm = uint32_t(int32_t(int16_t(op)));
  const uint32_t cur = cur_;
  auto br = [&](bool c) {
    if (c) npc_ = (cur + 4 + (imm << 2)) & 0xFFC;
    nextDelay_ = true;
  };
  switch (op >> 26) {
    case 0x00:
      switch (op & 63) {
        case 0x00: r[rd] = r[rt] << sa; break;
        case 0x02: r[rd] = r[rt] >> sa; break;
        case 0x03: r[rd] = uint32_t(int32_t(r[rt]) >> sa); break;
        case 0x04: r[rd] = r[rt] << (r[rs] & 31); break;
        case 0x06: r[rd] = r[rt] >> (r[rs] & 31); break;
        case 0x07: r[rd] = uint32_t(int32_t(r[rt]) >> (r[rs] & 31)); break;
        case 0x08: npc_ = r[rs] & 0xFFC; nextDelay_ = true; break;
        case 0x09: { const uint32_t t = r[rs]; r[rd] = (cur + 8) & 0xFFF; npc_ = t & 0xFFC; nextDelay_ = true; break; }
        case 0x0D:  // BREAK
          sys.sp[4] |= 3;
          if (sys.sp[4] & 0x40) sys.raise(0);
          break;
        case 0x20: case 0x21: r[rd] = r[rs] + r[rt]; break;
        case 0x22: case 0x23: r[rd] = r[rs] - r[rt]; break;
        case 0x24: r[rd] = r[rs] & r[rt]; break;
        case 0x25: r[rd] = r[rs] | r[rt]; break;
        case 0x26: r[rd] = r[rs] ^ r[rt]; break;
        case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
        case 0x2A: r[rd] = int32_t(r[rs]) < int32_t(r[rt]); break;
        case 0x2B: r[rd] = r[rs] < r[rt]; break;
        default: break;
      }
      break;
    case 0x01: {
      const int32_t a = int32_t(r[rs]);
      switch (rt) {
        case 0x00: br(a < 0); break;
        case 0x01: br(a >= 0); break;
        case 0x10: r[31] = (cur + 8) & 0xFFF; br(a < 0); break;
        case 0x11: r[31] = (cur + 8) & 0xFFF; br(a >= 0); break;
        default: break;
      }
      break;
    }
    case 0x02: npc_ = (op << 2) & 0xFFC; nextDelay_ = true; break;
    case 0x03: r[31] = (cur + 8) & 0xFFF; npc_ = (op << 2) & 0xFFC; nextDelay_ = true; break;
    case 0x04: br(r[rs] == r[rt]); break;
    case 0x05: br(r[rs] != r[rt]); break;
    case 0x06: br(int32_t(r[rs]) <= 0); break;
    case 0x07: br(int32_t(r[rs]) > 0); break;
    case 0x08: case 0x09: r[rt] = r[rs] + imm; break;
    case 0x0A: r[rt] = int32_t(r[rs]) < int32_t(imm); break;
    case 0x0B: r[rt] = r[rs] < imm; break;
    case 0x0C: r[rt] = r[rs] & (op & 0xFFFF); break;
    case 0x0D: r[rt] = r[rs] | (op & 0xFFFF); break;
    case 0x0E: r[rt] = r[rs] ^ (op & 0xFFFF); break;
    case 0x0F: r[rt] = op << 16; break;
    case 0x10:
      if (rs == 0) r[rt] = rd < 8 ? sys.spRead(rd) : sys.dpRead(rd & 7);
      else if (rs == 4) { if (rd < 8) sys.spWrite(rd, r[rt]); else sys.dpWrite(rd & 7, r[rt]); }
      break;
    case 0x12: cop2(op); break;
    case 0x20: r[rt] = uint32_t(int32_t(int8_t(dm(r[rs] + imm)))); break;
    case 0x21: { const uint32_t a = r[rs] + imm; r[rt] = uint32_t(int32_t(int16_t((dm(a) << 8) | dm(a + 1)))); break; }
    case 0x23: case 0x27: {
      const uint32_t a = r[rs] + imm;
      r[rt] = (uint32_t(dm(a)) << 24) | (dm(a + 1) << 16) | (dm(a + 2) << 8) | dm(a + 3);
      break;
    }
    case 0x24: r[rt] = dm(r[rs] + imm); break;
    case 0x25: { const uint32_t a = r[rs] + imm; r[rt] = (dm(a) << 8) | dm(a + 1); break; }
    case 0x28: dmw(r[rs] + imm, uint8_t(r[rt])); break;
    case 0x29: { const uint32_t a = r[rs] + imm; dmw(a, uint8_t(r[rt] >> 8)); dmw(a + 1, uint8_t(r[rt])); break; }
    case 0x2B: {
      const uint32_t a = r[rs] + imm;
      dmw(a, uint8_t(r[rt] >> 24)); dmw(a + 1, uint8_t(r[rt] >> 16)); dmw(a + 2, uint8_t(r[rt] >> 8)); dmw(a + 3, uint8_t(r[rt]));
      break;
    }
    case 0x32: vload(op); break;
    case 0x3A: vstore(op); break;
    default: break;
  }
}

// --------------------------------------------------------------------------------------- loads/stores vetoriais
void Rsp::vload(uint32_t op) {
  const int base = (op >> 21) & 31, vt = (op >> 16) & 31, kind = (op >> 11) & 31, e = (op >> 7) & 15;
  const int32_t off = int32_t(op << 25) >> 25;
  const uint32_t b = r[base];
  switch (kind) {
    case 0: case 1: case 2: case 3: {  // LBV LSV LLV LDV
      const int n = 1 << kind;
      const uint32_t a = b + uint32_t(off * n);
      for (int i = 0; i < n && e + i < 16; i++) vbw(vt, e + i, dm(a + i));
      break;
    }
    case 4: {  // LQV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t fim = (a & ~15u) + 16;
      for (uint32_t x = a, i = e; x < fim && i < 16; x++, i++) vbw(vt, int(i), dm(x));
      break;
    }
    case 5: {  // LRV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t ini = a & ~15u;
      int i = e + (16 - int(a & 15));
      for (uint32_t x = ini; x < a; x++, i++) if (i < 16) vbw(vt, i, dm(x));
      break;
    }
    case 6: case 7: {  // LPV LUV
      const uint32_t a = b + uint32_t(off * 8);
      const uint32_t al = a & ~7u;
      const int sh = kind == 6 ? 8 : 7;
      for (int i = 0; i < 8; i++) vr[vt].e[i] = uint16_t(dm(al + ((a - e + i) & 15)) << sh);
      break;
    }
    case 8: {  // LHV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      for (int i = 0; i < 8; i++) vr[vt].e[i] = uint16_t(dm(al + ((a - e + i * 2) & 15)) << 7);
      break;
    }
    case 9: {  // LFV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      uint16_t tmp[8];
      for (int i = 0; i < 4; i++) {
        tmp[i] = uint16_t(dm(al + ((a - e + i * 4) & 15)) << 7);
        tmp[i + 4] = uint16_t(dm(al + ((a - e + i * 4 + 8) & 15)) << 7);
      }
      for (int i = e >> 1; i < 8 && i < (e >> 1) + 4; i++) vr[vt].e[i] = tmp[i];
      break;
    }
    case 11: {  // LTV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      for (int i = 0; i < 8; i++) {
        const int reg = (vt & 0x18) | ((vt + i) & 7);
        const int el = (i + (e >> 1)) & 7;
        const uint32_t x = al + ((i * 2 + (a & 8)) & 15);
        vr[reg].e[el] = uint16_t((dm(x) << 8) | dm(al + ((i * 2 + 1 + (a & 8)) & 15)));
      }
      break;
    }
    default: break;
  }
}

void Rsp::vstore(uint32_t op) {
  const int base = (op >> 21) & 31, vt = (op >> 16) & 31, kind = (op >> 11) & 31, e = (op >> 7) & 15;
  const int32_t off = int32_t(op << 25) >> 25;
  const uint32_t b = r[base];
  switch (kind) {
    case 0: case 1: case 2: case 3: {  // SBV SSV SLV SDV
      const int n = 1 << kind;
      const uint32_t a = b + uint32_t(off * n);
      for (int i = 0; i < n; i++) dmw(a + i, vb(vt, (e + i) & 15));
      break;
    }
    case 4: {  // SQV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t fim = (a & ~15u) + 16;
      for (uint32_t x = a; x < fim; x++) dmw(x, vb(vt, int(e + (x - a)) & 15));
      break;
    }
    case 5: {  // SRV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t ini = a & ~15u;
      const int base_ = e + (16 - int(a & 15));
      for (uint32_t x = ini; x < a; x++) dmw(x, vb(vt, int(base_ + (x - ini)) & 15));
      break;
    }
    case 6: case 7: {  // SPV SUV
      const uint32_t a = b + uint32_t(off * 8);
      for (int i = 0; i < 8; i++) {
        const int idx = (e + i) & 15;
        const bool alto = idx < 8;
        const uint16_t v = vr[vt].e[idx & 7];
        uint8_t x;
        if (kind == 6) x = alto ? uint8_t(v >> 8) : uint8_t(v >> 7);
        else x = alto ? uint8_t(v >> 7) : uint8_t(v >> 8);
        dmw(a + i, x);
      }
      break;
    }
    case 8: {  // SHV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      for (int i = 0; i < 8; i++) {
        const int idx = e + i * 2;
        const uint8_t v = uint8_t((vb(vt, idx & 15) << 1) | (vb(vt, (idx + 1) & 15) >> 7));
        dmw(al + ((a + i * 2) & 15), v);
      }
      break;
    }
    case 9: {  // SFV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      const int ini = (e >> 1) & 4;
      for (int i = 0; i < 4; i++) dmw(al + ((a + i * 4) & 15), uint8_t(vr[vt].e[(ini + i) & 7] >> 7));
      break;
    }
    case 10: {  // SWV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      for (int i = 0; i < 16; i++) dmw(al + ((a + i) & 15), vb(vt, (e + i) & 15));
      break;
    }
    case 11: {  // STV
      const uint32_t a = b + uint32_t(off * 16);
      const uint32_t al = a & ~7u;
      for (int i = 0; i < 8; i++) {
        const int reg = (vt & 0x18) | ((vt + i) & 7);
        const int el = (i + (e >> 1)) & 7;
        const uint16_t v = vr[reg].e[el];
        dmw(al + ((i * 2 + (a & 8)) & 15), uint8_t(v >> 8));
        dmw(al + ((i * 2 + 1 + (a & 8)) & 15), uint8_t(v));
      }
      break;
    }
    default: break;
  }
}

// --------------------------------------------------------------------------------------- recíproco
int32_t Rsp::rcp(int32_t in, bool dp) {
  if (in == 0) return 0x7FFFFFFF;
  const int32_t mask = in >> 31;
  uint32_t data = uint32_t(in ^ mask);
  if (in > -32768 || dp) data -= uint32_t(mask);
  if (dp && in == INT32_MIN) data = 0x80000000u;
  if (data == 0) return 0x7FFFFFFF;
  const int shift = clz32(data);
  const uint32_t addr = uint32_t((uint64_t(data) << shift) & 0x7FC00000) >> 22;
  uint32_t res = RCP_ROM[addr];
  res = uint32_t((uint64_t(0x10000 | res) << 14) >> (31 - shift));
  return int32_t(res ^ uint32_t(mask));
}

int32_t Rsp::rsq(int32_t in, bool dp) {
  if (in == 0) return 0x7FFFFFFF;
  const int32_t mask = in >> 31;
  uint32_t data = uint32_t(in ^ mask);
  if (in > -32768 || dp) data -= uint32_t(mask);
  if (dp && in == INT32_MIN) data = 0x80000000u;
  if (data == 0) return 0x7FFFFFFF;
  const int shift = clz32(data);
  uint32_t addr = uint32_t((uint64_t(data) << shift) & 0x7FC00000) >> 22;
  addr = ((addr | 0x200) & 0x3FE) | (shift & 1);
  uint32_t res = RSQ_ROM[addr & 0x1FF];
  res = uint32_t((uint64_t(0x10000 | res) << 14) >> ((31 - shift) >> 1));
  return int32_t(res ^ uint32_t(mask));
}

// --------------------------------------------------------------------------------------- COP2
void Rsp::cop2(uint32_t op) {
  const int vt = (op >> 16) & 31, vs = (op >> 11) & 31, vd = (op >> 6) & 31;
  if (!(op & (1u << 25))) {
    const int e = (op >> 7) & 15;
    switch ((op >> 21) & 31) {
      case 0x00:  // MFC2
        r[vt] = uint32_t(int32_t(int16_t((vb(vs, e) << 8) | vb(vs, (e + 1) & 15))));
        break;
      case 0x02:  // CFC2
        switch (vs & 3) {
          case 0: r[vt] = uint32_t(int32_t(int16_t(vco))); break;
          case 1: r[vt] = uint32_t(int32_t(int16_t(vcc))); break;
          default: r[vt] = vce; break;
        }
        break;
      case 0x04:  // MTC2
        vbw(vs, e, uint8_t(r[vt] >> 8));
        if (e < 15) vbw(vs, e + 1, uint8_t(r[vt]));
        break;
      case 0x06:  // CTC2
        switch (vs & 3) {
          case 0: vco = uint16_t(r[vt]); break;
          case 1: vcc = uint16_t(r[vt]); break;
          default: vce = uint8_t(r[vt]); break;
        }
        break;
      default: break;
    }
    return;
  }
  const int e = (op >> 21) & 15;
  const uint8_t* sel = SEL[e];
  int16_t S[8], T[8];
  for (int i = 0; i < 8; i++) { S[i] = int16_t(vr[vs].e[i]); T[i] = int16_t(vr[vt].e[sel[i]]); }
  uint16_t R[8];
  auto setLow = [&](int i, uint16_t v) { acc[i] = (acc[i] & ~int64_t(0xFFFF)) | v; };
  switch (op & 63) {
    case 0x00:  // VMULF
      for (int i = 0; i < 8; i++) { acc[i] = int64_t(S[i]) * T[i] * 2 + 0x8000; R[i] = clampS(acc[i]); }
      break;
    case 0x01:  // VMULU
      for (int i = 0; i < 8; i++) { acc[i] = int64_t(S[i]) * T[i] * 2 + 0x8000; R[i] = clampU(acc[i]); }
      break;
    case 0x02: case 0x0A: {  // VRNDP / VRNDN
      const bool p = (op & 63) == 0x02;
      for (int i = 0; i < 8; i++) {
        int64_t v = (vs & 1) ? int64_t(T[i]) << 16 : int64_t(T[i]);
        if (p ? acc[i] >= 0 : acc[i] < 0) acc[i] = wrap48(acc[i] + v);
        R[i] = clampS(acc[i]);
      }
      break;
    }
    case 0x03:  // VMULQ
      for (int i = 0; i < 8; i++) {
        int64_t v = int64_t(S[i]) * T[i];
        if (v < 0) v += 31;
        acc[i] = wrap48(v << 16);
        R[i] = uint16_t(sat16(int32_t(acc[i] >> 17)) & ~15);
      }
      break;
    case 0x04:  // VMUDL
      for (int i = 0; i < 8; i++) { acc[i] = int64_t((uint32_t(uint16_t(S[i])) * uint16_t(T[i])) >> 16); R[i] = clampLow(acc[i]); }
      break;
    case 0x05:  // VMUDM
      for (int i = 0; i < 8; i++) { acc[i] = int64_t(S[i]) * uint16_t(T[i]); R[i] = clampS(acc[i]); }
      break;
    case 0x06:  // VMUDN
      for (int i = 0; i < 8; i++) { acc[i] = int64_t(uint16_t(S[i])) * T[i]; R[i] = clampLow(acc[i]); }
      break;
    case 0x07:  // VMUDH
      for (int i = 0; i < 8; i++) { acc[i] = wrap48((int64_t(S[i]) * T[i]) << 16); R[i] = clampS(acc[i]); }
      break;
    case 0x08:  // VMACF
      for (int i = 0; i < 8; i++) { acc[i] = wrap48(acc[i] + int64_t(S[i]) * T[i] * 2); R[i] = clampS(acc[i]); }
      break;
    case 0x09:  // VMACU
      for (int i = 0; i < 8; i++) { acc[i] = wrap48(acc[i] + int64_t(S[i]) * T[i] * 2); R[i] = clampU(acc[i]); }
      break;
    case 0x0B:  // VMACQ
      for (int i = 0; i < 8; i++) {
        int64_t a = acc[i] >> 16;
        if (a < 0 && !(a & 0x20)) a += 32;
        else if (a >= 32 && !(a & 0x20)) a -= 32;
        acc[i] = wrap48((a << 16) | (acc[i] & 0xFFFF));
        R[i] = uint16_t(sat16(int32_t(acc[i] >> 17)) & ~15);
      }
      break;
    case 0x0C:  // VMADL
      for (int i = 0; i < 8; i++) {
        acc[i] = wrap48(acc[i] + int64_t((uint32_t(uint16_t(S[i])) * uint16_t(T[i])) >> 16));
        R[i] = clampLow(acc[i]);
      }
      break;
    case 0x0D:  // VMADM
      for (int i = 0; i < 8; i++) { acc[i] = wrap48(acc[i] + int64_t(S[i]) * uint16_t(T[i])); R[i] = clampS(acc[i]); }
      break;
    case 0x0E:  // VMADN
      for (int i = 0; i < 8; i++) { acc[i] = wrap48(acc[i] + int64_t(uint16_t(S[i])) * T[i]); R[i] = clampLow(acc[i]); }
      break;
    case 0x0F:  // VMADH
      for (int i = 0; i < 8; i++) { acc[i] = wrap48(acc[i] + ((int64_t(S[i]) * T[i]) << 16)); R[i] = clampS(acc[i]); }
      break;
    case 0x10:  // VADD
      for (int i = 0; i < 8; i++) {
        const int32_t v = S[i] + T[i] + ((vco >> i) & 1);
        setLow(i, uint16_t(v));
        R[i] = sat16(v);
      }
      vco = 0;
      break;
    case 0x11:  // VSUB
      for (int i = 0; i < 8; i++) {
        const int32_t v = S[i] - T[i] - ((vco >> i) & 1);
        setLow(i, uint16_t(v));
        R[i] = sat16(v);
      }
      vco = 0;
      break;
    case 0x13:  // VABS
      for (int i = 0; i < 8; i++) {
        int32_t v;
        if (S[i] < 0) v = -int32_t(T[i]);
        else if (S[i] == 0) v = 0;
        else v = T[i];
        setLow(i, uint16_t(v));
        R[i] = sat16(v);
      }
      break;
    case 0x14: {  // VADDC
      uint16_t c = 0;
      for (int i = 0; i < 8; i++) {
        const uint32_t v = uint32_t(uint16_t(S[i])) + uint16_t(T[i]);
        R[i] = uint16_t(v);
        setLow(i, R[i]);
        if (v >> 16) c |= 1u << i;
      }
      vco = c;
      break;
    }
    case 0x15: {  // VSUBC
      uint16_t c = 0;
      for (int i = 0; i < 8; i++) {
        const int32_t v = int32_t(uint16_t(S[i])) - int32_t(uint16_t(T[i]));
        R[i] = uint16_t(v);
        setLow(i, R[i]);
        if (v < 0) c |= 1u << i;
        if (v != 0) c |= 0x100u << i;
      }
      vco = c;
      break;
    }
    case 0x1D:  // VSAR
      for (int i = 0; i < 8; i++) {
        switch (e) {
          case 8: R[i] = uint16_t(acc[i] >> 32); break;
          case 9: R[i] = uint16_t(acc[i] >> 16); break;
          case 10: R[i] = uint16_t(acc[i]); break;
          default: R[i] = 0; break;
        }
      }
      break;
    case 0x20: case 0x21: case 0x22: case 0x23: {  // VLT VEQ VNE VGE
      uint16_t c = 0;
      for (int i = 0; i < 8; i++) {
        const bool eq = S[i] == T[i];
        const bool ne = (vco >> (8 + i)) & 1, ca = (vco >> i) & 1;
        bool r_;
        switch (op & 63) {
          case 0x20: r_ = S[i] < T[i] || (eq && ne && ca); break;
          case 0x21: r_ = eq && !ne; break;
          case 0x22: r_ = !eq || ne; break;
          default: r_ = S[i] > T[i] || (eq && !(ne && ca)); break;
        }
        if (r_) c |= 1u << i;
        R[i] = uint16_t(r_ ? S[i] : T[i]);
        setLow(i, R[i]);
      }
      vcc = c;
      vco = 0;
      break;
    }
    case 0x24: {  // VCL
      uint16_t ncc = 0;
      for (int i = 0; i < 8; i++) {
        const uint16_t s = uint16_t(S[i]), t = uint16_t(T[i]);
        const bool ca = (vco >> i) & 1, ne = (vco >> (8 + i)) & 1, ce = (vce >> i) & 1;
        bool le = (vcc >> i) & 1, ge = (vcc >> (8 + i)) & 1;
        if (ca) {
          if (!ne) {
            const uint32_t sum = uint32_t(s) + t;
            const bool lz = (sum & 0xFFFF) == 0, uz = (sum >> 16) == 0;
            le = ce ? (lz || uz) : (lz && uz);
          }
          R[i] = le ? uint16_t(-int32_t(t)) : s;
        } else {
          if (!ne) ge = s >= t;
          R[i] = ge ? t : s;
        }
        if (le) ncc |= 1u << i;
        if (ge) ncc |= 0x100u << i;
        setLow(i, R[i]);
      }
      vcc = ncc;
      vco = 0;
      vce = 0;
      break;
    }
    case 0x25: {  // VCH
      uint16_t nco = 0, ncc = 0;
      uint8_t nce = 0;
      for (int i = 0; i < 8; i++) {
        const int32_t s = S[i], t = T[i];
        bool le, ge, ne, ce;
        if ((s ^ t) < 0) {
          const int32_t sum = s + t;
          ge = t < 0;
          le = sum <= 0;
          ce = sum == -1;
          ne = sum != 0 && sum != -1;
          R[i] = uint16_t(le ? -t : s);
          nco |= 1u << i;
        } else {
          const int32_t dif = s - t;
          le = t < 0;
          ge = dif >= 0;
          ce = false;
          ne = dif != 0;
          R[i] = uint16_t(ge ? t : s);
        }
        if (ne) nco |= 0x100u << i;
        if (le) ncc |= 1u << i;
        if (ge) ncc |= 0x100u << i;
        if (ce) nce |= 1u << i;
        setLow(i, R[i]);
      }
      vco = nco; vcc = ncc; vce = nce;
      break;
    }
    case 0x26: {  // VCR
      uint16_t ncc = 0;
      for (int i = 0; i < 8; i++) {
        const int32_t s = S[i], t = T[i];
        bool le, ge;
        if ((s ^ t) < 0) {
          ge = t < 0;
          le = s + t + 1 <= 0;
          R[i] = uint16_t(le ? ~t : s);
        } else {
          le = t < 0;
          ge = s - t >= 0;
          R[i] = uint16_t(ge ? t : s);
        }
        if (le) ncc |= 1u << i;
        if (ge) ncc |= 0x100u << i;
        setLow(i, R[i]);
      }
      vcc = ncc; vco = 0; vce = 0;
      break;
    }
    case 0x27:  // VMRG
      for (int i = 0; i < 8; i++) { R[i] = uint16_t(((vcc >> i) & 1) ? S[i] : T[i]); setLow(i, R[i]); }
      break;
    case 0x28: for (int i = 0; i < 8; i++) { R[i] = uint16_t(S[i] & T[i]); setLow(i, R[i]); } break;
    case 0x29: for (int i = 0; i < 8; i++) { R[i] = uint16_t(~(S[i] & T[i])); setLow(i, R[i]); } break;
    case 0x2A: for (int i = 0; i < 8; i++) { R[i] = uint16_t(S[i] | T[i]); setLow(i, R[i]); } break;
    case 0x2B: for (int i = 0; i < 8; i++) { R[i] = uint16_t(~(S[i] | T[i])); setLow(i, R[i]); } break;
    case 0x2C: for (int i = 0; i < 8; i++) { R[i] = uint16_t(S[i] ^ T[i]); setLow(i, R[i]); } break;
    case 0x2D: for (int i = 0; i < 8; i++) { R[i] = uint16_t(~(S[i] ^ T[i])); setLow(i, R[i]); } break;
    case 0x30: case 0x31: case 0x34: case 0x35: {  // VRCP VRCPL VRSQ VRSQL
      const bool l = (op & 1), sq = (op & 63) >= 0x34;
      const int de = vs & 7;
      int32_t in;
      const bool dp = l && divLoaded;
      if (dp) in = int32_t((uint32_t(uint16_t(divIn)) << 16) | uint16_t(T[e & 7]));
      else in = T[e & 7];
      const int32_t res = sq ? rsq(in, dp) : rcp(in, dp);
      divLoaded = false;
      divOut = int16_t(uint32_t(res) >> 16);
      for (int i = 0; i < 8; i++) setLow(i, uint16_t(T[i]));
      vr[vd].e[de] = uint16_t(res);
      return;
    }
    case 0x32: case 0x36: {  // VRCPH VRSQH
      const int de = vs & 7;
      divIn = T[e & 7];
      divLoaded = true;
      for (int i = 0; i < 8; i++) setLow(i, uint16_t(T[i]));
      vr[vd].e[de] = uint16_t(divOut);
      return;
    }
    case 0x33: {  // VMOV
      const int de = vs & 7;
      for (int i = 0; i < 8; i++) setLow(i, uint16_t(T[i]));
      vr[vd].e[de] = uint16_t(T[e & 7]);
      return;
    }
    case 0x37: case 0x3F: return;  // VNOP VNULL
    default:
      for (int i = 0; i < 8; i++) R[i] = 0;
      break;
  }
  std::memcpy(vr[vd].e, R, sizeof R);
}

void Rsp::sync(StateIO& io) {
  io.sync(pc); io.sync(npc_); io.sync(nextDelay_); io.sync(cycles);
  io.raw(r, sizeof r);
  io.raw(vr, sizeof vr);
  io.raw(acc, sizeof acc);
  io.sync(vco); io.sync(vcc); io.sync(vce); io.sync(divOut); io.sync(divIn); io.sync(divLoaded);
  if (io.reading()) {
    pc &= 0xFFC; npc_ &= 0xFFC;
    for (auto& a : acc) a = wrap48(a);
    r[0] = 0;
  }
}

}  // namespace indigo::n64
