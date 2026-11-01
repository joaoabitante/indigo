// GTE do PlayStation (COP2). Ver gte.h.
//
// Notas do hardware que este código segue:
// - As somas de MAC1..MAC3 são feitas num acumulador de 44 bits: depois de cada parcela o valor é
//   conferido (bits 30..25 do FLAG) e continua só com os 44 bits de baixo, com sinal.
// - MAC0 é de 32 bits; o estouro liga os bits 16/15 do FLAG, mas as contas seguintes usam o valor inteiro.
// - A divisão do RTPS/RTPT é a do hardware (UNR): uma tabela de 257 recíprocos aproximados e duas
//   passadas de Newton-Raphson; o resultado fica limitado a 0x1FFFF.
// - O bit 31 do FLAG é o OU dos bits 30..23 e 18..13.
#include "ps1/gte.h"

#include <array>

namespace indigo::ps1 {

namespace {

// Tabela de recíprocos da divisão UNR: unr[i] ~ 0x40000 / (i + 0x100), guardada menos 0x101.
constexpr std::array<uint8_t, 257> makeUnr() {
  std::array<uint8_t, 257> t{};
  for (int i = 0; i < 257; i++) {
    int v = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
    t[i] = static_cast<uint8_t>(v < 0 ? 0 : v);
  }
  return t;
}
constexpr std::array<uint8_t, 257> kUnr = makeUnr();

inline uint32_t sext16(int16_t v) { return static_cast<uint32_t>(static_cast<int32_t>(v)); }
inline uint32_t pack16(int16_t lo, int16_t hi) {
  return static_cast<uint16_t>(lo) | (static_cast<uint32_t>(static_cast<uint16_t>(hi)) << 16);
}

// ciclos de cada comando (nocash), indexados pelos 6 bits de baixo do comando
constexpr int cyclesOf(int code) {
  switch (code) {
    case 0x00: case 0x01: return 15;  // RTPS
    case 0x06: return 8;              // NCLIP
    case 0x0C: return 6;              // OP
    case 0x10: return 8;              // DPCS
    case 0x11: return 8;              // INTPL
    case 0x12: return 8;              // MVMVA
    case 0x13: return 19;             // NCDS
    case 0x14: return 13;             // CDP
    case 0x16: return 44;             // NCDT
    case 0x1A: case 0x29: return 8;   // DCPL
    case 0x1B: return 17;             // NCCS
    case 0x1C: return 11;             // CC
    case 0x1E: return 14;             // NCS
    case 0x20: return 30;             // NCT
    case 0x28: return 5;              // SQR
    case 0x2A: return 17;             // DPCT
    case 0x2D: return 5;              // AVSZ3
    case 0x2E: return 6;              // AVSZ4
    case 0x30: return 23;             // RTPT
    case 0x3D: return 5;              // GPF
    case 0x3E: return 5;              // GPL
    case 0x3F: return 39;             // NCCT
    default: return 1;
  }
}

}  // namespace

void Gte::reset() {
  for (auto& a : v_) for (auto& x : a) x = 0;
  for (auto& x : rgbc_) x = 0;
  otz_ = 0;
  for (auto& x : ir_) x = 0;
  for (auto& a : sxy_) a[0] = a[1] = 0;
  for (auto& x : sz_) x = 0;
  for (auto& x : rgbFifo_) x = 0;
  res1_ = 0;
  for (auto& x : mac_) x = 0;
  lzcs_ = 0;
  lzcr_ = 32;
  for (int i = 0; i < 9; i++) rt_[i] = llm_[i] = lcm_[i] = 0;
  for (int i = 0; i < 3; i++) tr_[i] = bk_[i] = fc_[i] = 0;
  ofx_ = ofy_ = 0;
  h_ = 0;
  dqa_ = 0;
  dqb_ = 0;
  zsf3_ = zsf4_ = 0;
  flag_ = 0;
}

// ---------------------------------------------------------------- registradores

uint32_t Gte::readData(int r) {
  switch (r & 31) {
    case 0: return pack16(v_[0][0], v_[0][1]);
    case 1: return sext16(v_[0][2]);
    case 2: return pack16(v_[1][0], v_[1][1]);
    case 3: return sext16(v_[1][2]);
    case 4: return pack16(v_[2][0], v_[2][1]);
    case 5: return sext16(v_[2][2]);
    case 6: return rgbc_[0] | (rgbc_[1] << 8) | (rgbc_[2] << 16) | (static_cast<uint32_t>(rgbc_[3]) << 24);
    case 7: return otz_;
    case 8: case 9: case 10: case 11: return sext16(ir_[r - 8]);
    case 12: case 13: case 14: return pack16(sxy_[r - 12][0], sxy_[r - 12][1]);
    case 15: return pack16(sxy_[2][0], sxy_[2][1]);  // SXYP lê o SXY2
    case 16: case 17: case 18: case 19: return sz_[r - 16];
    case 20: case 21: case 22: return rgbFifo_[r - 20];
    case 23: return res1_;
    case 24: case 25: case 26: case 27: return static_cast<uint32_t>(mac_[r - 24]);
    case 28: case 29: {  // IRGB/ORGB: IR1..IR3 / 0x80 limitados a 0..0x1F
      auto c = [](int16_t x) -> uint32_t {
        int v = x >> 7;
        return v < 0 ? 0 : v > 0x1F ? 0x1F : static_cast<uint32_t>(v);
      };
      return c(ir_[1]) | (c(ir_[2]) << 5) | (c(ir_[3]) << 10);
    }
    case 30: return lzcs_;
    default: return lzcr_;
  }
}

void Gte::writeData(int r, uint32_t v) {
  const int16_t lo = static_cast<int16_t>(v), hi = static_cast<int16_t>(v >> 16);
  switch (r & 31) {
    case 0: case 2: case 4: v_[r >> 1][0] = lo; v_[r >> 1][1] = hi; break;
    case 1: case 3: case 5: v_[r >> 1][2] = lo; break;
    case 6: for (int i = 0; i < 4; i++) rgbc_[i] = static_cast<uint8_t>(v >> (8 * i)); break;
    case 7: otz_ = static_cast<uint16_t>(v); break;
    case 8: case 9: case 10: case 11: ir_[r - 8] = lo; break;
    case 12: case 13: case 14: sxy_[r - 12][0] = lo; sxy_[r - 12][1] = hi; break;
    case 15:  // SXYP: empurra o FIFO
      sxy_[0][0] = sxy_[1][0]; sxy_[0][1] = sxy_[1][1];
      sxy_[1][0] = sxy_[2][0]; sxy_[1][1] = sxy_[2][1];
      sxy_[2][0] = lo; sxy_[2][1] = hi;
      break;
    case 16: case 17: case 18: case 19: sz_[r - 16] = static_cast<uint16_t>(v); break;
    case 20: case 21: case 22: rgbFifo_[r - 20] = v; break;
    case 23: res1_ = v; break;
    case 24: case 25: case 26: case 27: mac_[r - 24] = static_cast<int32_t>(v); break;
    case 28:  // IRGB: cinco bits por cor, multiplicados por 0x80
      ir_[1] = static_cast<int16_t>((v & 0x1F) << 7);
      ir_[2] = static_cast<int16_t>(((v >> 5) & 0x1F) << 7);
      ir_[3] = static_cast<int16_t>(((v >> 10) & 0x1F) << 7);
      break;
    case 29: break;  // ORGB só se lê
    case 30: {
      lzcs_ = v;
      uint32_t x = (v & 0x80000000u) ? ~v : v;
      lzcr_ = x ? static_cast<uint32_t>(__builtin_clz(x)) : 32;
      break;
    }
    default: break;  // LZCR só se lê
  }
}

uint32_t Gte::readCtrl(int r) {
  r &= 31;
  if (r < 24) {
    const int grp = r >> 3, k = r & 7;  // 0..4 matriz, 5..7 vetor
    const int16_t* m = grp == 0 ? rt_ : grp == 1 ? llm_ : lcm_;
    const int32_t* t = grp == 0 ? tr_ : grp == 1 ? bk_ : fc_;
    if (k < 4) return pack16(m[2 * k], m[2 * k + 1]);
    if (k == 4) return sext16(m[8]);
    return static_cast<uint32_t>(t[k - 5]);
  }
  switch (r) {
    case 24: return static_cast<uint32_t>(ofx_);
    case 25: return static_cast<uint32_t>(ofy_);
    case 26: return sext16(static_cast<int16_t>(h_));  // defeito do hardware: H volta com sinal
    case 27: return sext16(dqa_);
    case 28: return static_cast<uint32_t>(dqb_);
    case 29: return sext16(zsf3_);
    case 30: return sext16(zsf4_);
    default: return flag_;
  }
}

void Gte::writeCtrl(int r, uint32_t v) {
  r &= 31;
  const int16_t lo = static_cast<int16_t>(v), hi = static_cast<int16_t>(v >> 16);
  if (r < 24) {
    const int grp = r >> 3, k = r & 7;
    int16_t* m = grp == 0 ? rt_ : grp == 1 ? llm_ : lcm_;
    int32_t* t = grp == 0 ? tr_ : grp == 1 ? bk_ : fc_;
    if (k < 4) { m[2 * k] = lo; m[2 * k + 1] = hi; }
    else if (k == 4) m[8] = lo;
    else t[k - 5] = static_cast<int32_t>(v);
    return;
  }
  switch (r) {
    case 24: ofx_ = static_cast<int32_t>(v); break;
    case 25: ofy_ = static_cast<int32_t>(v); break;
    case 26: h_ = static_cast<uint16_t>(v); break;
    case 27: dqa_ = lo; break;
    case 28: dqb_ = static_cast<int32_t>(v); break;
    case 29: zsf3_ = lo; break;
    case 30: zsf4_ = lo; break;
    default:  // FLAG: os 12 bits de baixo não existem e o bit 31 é calculado
      flag_ = v & 0x7FFFF000u;
      if (flag_ & 0x7F87E000u) flag_ |= 0x80000000u;
      break;
  }
}

// ---------------------------------------------------------------- passos de cálculo

// soma de MAC1..3 (i = 1..3) no acumulador de 44 bits
inline int64_t Gte::chk44(int i, int64_t x) {
  if (x >= (int64_t(1) << 43)) flag_ |= 1u << (31 - i);
  else if (x < -(int64_t(1) << 43)) flag_ |= 1u << (28 - i);
  return static_cast<int64_t>(static_cast<uint64_t>(x) << 20) >> 20;
}

inline int64_t Gte::chk32(int64_t x) {
  if (x > INT32_MAX) flag_ |= 1u << 16;
  else if (x < INT32_MIN) flag_ |= 1u << 15;
  return x;
}

inline int16_t Gte::satIR(int i, int32_t x, bool lm) {
  const int32_t low = lm ? 0 : -0x8000;
  if (x < low) { flag_ |= 1u << (25 - i); return static_cast<int16_t>(low); }
  if (x > 0x7FFF) { flag_ |= 1u << (25 - i); return 0x7FFF; }
  return static_cast<int16_t>(x);
}

inline uint8_t Gte::satC(int i, int32_t x) {
  x >>= 4;
  if (x < 0) { flag_ |= 1u << (22 - i); return 0; }
  if (x > 0xFF) { flag_ |= 1u << (22 - i); return 0xFF; }
  return static_cast<uint8_t>(x);
}

// MAC = (T*0x1000 + M*vec) >> sf; IR = MAC saturado. t pode ser nulo (sem translação).
inline void Gte::mulMat(const int16_t* m, const int16_t* vec, const int32_t* t, int sf, bool lm) {
  const int32_t x = vec[0], y = vec[1], z = vec[2];
  for (int i = 0; i < 3; i++) {
    int64_t a = t ? int64_t(t[i]) * 0x1000 : 0;
    a = chk44(i + 1, a + m[3 * i] * x);
    a = chk44(i + 1, a + m[3 * i + 1] * y);
    a = chk44(i + 1, a + m[3 * i + 2] * z);
    mac_[i + 1] = static_cast<int32_t>(a >> sf);
  }
  for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
}

inline void Gte::pushColor() {
  const uint8_t r = satC(1, mac_[1]), g = satC(2, mac_[2]), b = satC(3, mac_[3]);
  rgbFifo_[0] = rgbFifo_[1];
  rgbFifo_[1] = rgbFifo_[2];
  rgbFifo_[2] = r | (g << 8) | (b << 16) | (static_cast<uint32_t>(rgbc_[3]) << 24);
}

// interpolação até a cor distante: IR = ((FC<<12) - base) >> sf (lm desligado); MAC = (base + IR*IR0) >> sf
inline void Gte::depthCue(const int64_t* base, int sf, bool lm) {
  for (int i = 1; i <= 3; i++)
    ir_[i] = satIR(i, static_cast<int32_t>(chk44(i, int64_t(fc_[i - 1]) * 0x1000 - base[i - 1]) >> sf), false);
  for (int i = 1; i <= 3; i++)
    mac_[i] = static_cast<int32_t>(chk44(i, base[i - 1] + int32_t(ir_[i]) * ir_[0]) >> sf);
  for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
}

// divisão UNR: (H * 0x20000 / SZ3 + 1) / 2, limitada a 0x1FFFF
inline uint32_t Gte::divide() {
  const uint32_t sz = sz_[3];
  if (h_ < sz * 2) {
    const int z = __builtin_clz(sz) - 16;
    const uint32_t n = static_cast<uint32_t>(h_) << z;
    uint32_t d = sz << z;
    const uint32_t u = kUnr[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    const uint64_t q = (uint64_t(n) * d + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFF : static_cast<uint32_t>(q);
  }
  flag_ |= 1u << 17;
  return 0x1FFFF;
}

// perspectiva de um vértice (RTPS/RTPT); last = calcula também a profundidade (IR0)
inline void Gte::rtp(int i, int sf, bool lm, bool last) {
  const int32_t x = v_[i][0], y = v_[i][1], z = v_[i][2];
  int64_t a[3];
  for (int k = 0; k < 3; k++) {
    int64_t s = int64_t(tr_[k]) * 0x1000;
    s = chk44(k + 1, s + rt_[3 * k] * x);
    s = chk44(k + 1, s + rt_[3 * k + 1] * y);
    s = chk44(k + 1, s + rt_[3 * k + 2] * z);
    a[k] = s;
    mac_[k + 1] = static_cast<int32_t>(s >> sf);
  }
  ir_[1] = satIR(1, mac_[1], lm);
  ir_[2] = satIR(2, mac_[2], lm);
  // IR3: o valor sai de MAC3, mas a marca de saturação olha sempre para o valor >> 12
  {
    const int64_t f = a[2] >> 12;
    if (f < -0x8000 || f > 0x7FFF) flag_ |= 1u << 22;
    const int32_t low = lm ? 0 : -0x8000;
    const int32_t m3 = mac_[3];
    ir_[3] = static_cast<int16_t>(m3 < low ? low : m3 > 0x7FFF ? 0x7FFF : m3);
  }
  // FIFO de Z: SZ3 = (soma >> 12) limitado a 0..0xFFFF
  {
    int64_t zz = a[2] >> 12;
    uint16_t sz3;
    if (zz < 0) { flag_ |= 1u << 18; sz3 = 0; }
    else if (zz > 0xFFFF) { flag_ |= 1u << 18; sz3 = 0xFFFF; }
    else sz3 = static_cast<uint16_t>(zz);
    sz_[0] = sz_[1]; sz_[1] = sz_[2]; sz_[2] = sz_[3]; sz_[3] = sz3;
  }
  const int64_t n = divide();
  int16_t sx, sy;
  {
    const int64_t px = chk32(n * ir_[1] + ofx_) >> 16;
    if (px < -0x400) { flag_ |= 1u << 14; sx = -0x400; }
    else if (px > 0x3FF) { flag_ |= 1u << 14; sx = 0x3FF; }
    else sx = static_cast<int16_t>(px);
    const int64_t py = chk32(n * ir_[2] + ofy_) >> 16;
    if (py < -0x400) { flag_ |= 1u << 13; sy = -0x400; }
    else if (py > 0x3FF) { flag_ |= 1u << 13; sy = 0x3FF; }
    else sy = static_cast<int16_t>(py);
    mac_[0] = static_cast<int32_t>(py);
  }
  sxy_[0][0] = sxy_[1][0]; sxy_[0][1] = sxy_[1][1];
  sxy_[1][0] = sxy_[2][0]; sxy_[1][1] = sxy_[2][1];
  sxy_[2][0] = sx; sxy_[2][1] = sy;
  if (last) {
    const int64_t d = chk32(n * dqa_ + dqb_);
    mac_[0] = static_cast<int32_t>(d);
    const int64_t q = d >> 12;
    if (q < 0) { flag_ |= 1u << 12; ir_[0] = 0; }
    else if (q > 0x1000) { flag_ |= 1u << 12; ir_[0] = 0x1000; }
    else ir_[0] = static_cast<int16_t>(q);
  }
}

// parte final dos comandos de cor depois de IR = BK + LCM*IR:
// kind 0 (NCS/NCT): só empurra a cor; 1 (NCC/CC): multiplica por RGBC; 2 (NCD/CDP): multiplica e interpola
inline void Gte::colorStep(int sf, bool lm, int kind) {
  if (kind) {
    int64_t b[3];
    for (int i = 0; i < 3; i++) b[i] = chk44(i + 1, int64_t(int32_t(rgbc_[i]) * ir_[i + 1]) * 16);
    if (kind == 2) {
      depthCue(b, sf, lm);
    } else {
      for (int i = 1; i <= 3; i++) mac_[i] = static_cast<int32_t>(b[i - 1] >> sf);
      for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
    }
  }
  pushColor();
}

inline void Gte::normalColor(int i, int sf, bool lm, int kind) {
  mulMat(llm_, v_[i], nullptr, sf, lm);
  const int16_t ir[3] = {ir_[1], ir_[2], ir_[3]};
  mulMat(lcm_, ir, bk_, sf, lm);
  colorStep(sf, lm, kind);
}

void Gte::mvmva(uint32_t op, int sf, bool lm) {
  const int mx = (op >> 17) & 3, vi = (op >> 15) & 3, cv = (op >> 13) & 3;
  int16_t vec[3];
  if (vi == 3) { vec[0] = ir_[1]; vec[1] = ir_[2]; vec[2] = ir_[3]; }
  else { vec[0] = v_[vi][0]; vec[1] = v_[vi][1]; vec[2] = v_[vi][2]; }
  int16_t garbage[9];
  const int16_t* m;
  if (mx == 0) m = rt_;
  else if (mx == 1) m = llm_;
  else if (mx == 2) m = lcm_;
  else {  // matriz "lixo" do mx=3
    const int16_t r4 = static_cast<int16_t>(rgbc_[0] << 4);
    garbage[0] = static_cast<int16_t>(-r4); garbage[1] = r4; garbage[2] = ir_[0];
    garbage[3] = garbage[4] = garbage[5] = rt_[2];
    garbage[6] = garbage[7] = garbage[8] = rt_[4];
    m = garbage;
  }
  if (cv == 2) {
    // defeito do hardware com a cor distante como translação: a primeira parcela (FC*0x1000 + M[i][0]*X)
    // só serve para as marcas de saturação; o resultado é só M[i][1]*Y + M[i][2]*Z
    for (int i = 0; i < 3; i++) {
      const int64_t a = chk44(i + 1, int64_t(fc_[i]) * 0x1000 + m[3 * i] * int32_t(vec[0]));
      satIR(i + 1, static_cast<int32_t>(a >> sf), false);
      int64_t s = chk44(i + 1, int64_t(m[3 * i + 1] * int32_t(vec[1])));
      s = chk44(i + 1, s + m[3 * i + 2] * int32_t(vec[2]));
      mac_[i + 1] = static_cast<int32_t>(s >> sf);
    }
    for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
    return;
  }
  mulMat(m, vec, cv == 0 ? tr_ : cv == 1 ? bk_ : nullptr, sf, lm);
}

// ---------------------------------------------------------------- comandos

int Gte::command(uint32_t op) {
  const int code = op & 0x3F;
  const int sf = (op & (1u << 19)) ? 12 : 0;
  const bool lm = (op >> 10) & 1;
  flag_ = 0;
  switch (code) {
    case 0x00: case 0x01:  // RTPS
      rtp(0, sf, lm, true);
      break;
    case 0x30:  // RTPT
      rtp(0, sf, lm, false);
      rtp(1, sf, lm, false);
      rtp(2, sf, lm, true);
      break;
    case 0x06: {  // NCLIP
      const int64_t x0 = sxy_[0][0], y0 = sxy_[0][1], x1 = sxy_[1][0], y1 = sxy_[1][1];
      const int64_t x2 = sxy_[2][0], y2 = sxy_[2][1];
      mac_[0] = static_cast<int32_t>(chk32(x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1));
      break;
    }
    case 0x0C: {  // OP: produto vetorial da diagonal de RT com IR
      const int32_t d1 = rt_[0], d2 = rt_[4], d3 = rt_[8];
      const int32_t i1 = ir_[1], i2 = ir_[2], i3 = ir_[3];
      mac_[1] = static_cast<int32_t>(chk44(1, int64_t(i3 * d2) - int64_t(i2 * d3)) >> sf);
      mac_[2] = static_cast<int32_t>(chk44(2, int64_t(i1 * d3) - int64_t(i3 * d1)) >> sf);
      mac_[3] = static_cast<int32_t>(chk44(3, int64_t(i2 * d1) - int64_t(i1 * d2)) >> sf);
      for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
      break;
    }
    case 0x10: {  // DPCS
      const int64_t b[3] = {int64_t(rgbc_[0]) << 16, int64_t(rgbc_[1]) << 16, int64_t(rgbc_[2]) << 16};
      depthCue(b, sf, lm);
      pushColor();
      break;
    }
    case 0x2A:  // DPCT: três vezes, sempre com a cor da frente do FIFO
      for (int k = 0; k < 3; k++) {
        const uint32_t c = rgbFifo_[0];
        const int64_t b[3] = {int64_t(c & 0xFF) << 16, int64_t((c >> 8) & 0xFF) << 16, int64_t((c >> 16) & 0xFF) << 16};
        depthCue(b, sf, lm);
        pushColor();
      }
      break;
    case 0x11: {  // INTPL
      const int64_t b[3] = {int64_t(ir_[1]) << 12, int64_t(ir_[2]) << 12, int64_t(ir_[3]) << 12};
      depthCue(b, sf, lm);
      pushColor();
      break;
    }
    case 0x1A: case 0x29: {  // DCPL
      int64_t b[3];
      for (int i = 0; i < 3; i++) b[i] = chk44(i + 1, int64_t(int32_t(rgbc_[i]) * ir_[i + 1]) * 16);
      depthCue(b, sf, lm);
      pushColor();
      break;
    }
    case 0x12:  // MVMVA
      mvmva(op, sf, lm);
      break;
    case 0x13: normalColor(0, sf, lm, 2); break;  // NCDS
    case 0x16:                                    // NCDT
      normalColor(0, sf, lm, 2);
      normalColor(1, sf, lm, 2);
      normalColor(2, sf, lm, 2);
      break;
    case 0x1B: normalColor(0, sf, lm, 1); break;  // NCCS
    case 0x3F:                                    // NCCT
      normalColor(0, sf, lm, 1);
      normalColor(1, sf, lm, 1);
      normalColor(2, sf, lm, 1);
      break;
    case 0x1E: normalColor(0, sf, lm, 0); break;  // NCS
    case 0x20:                                    // NCT
      normalColor(0, sf, lm, 0);
      normalColor(1, sf, lm, 0);
      normalColor(2, sf, lm, 0);
      break;
    case 0x1C: case 0x14: {  // CC e CDP
      const int16_t ir[3] = {ir_[1], ir_[2], ir_[3]};
      mulMat(lcm_, ir, bk_, sf, lm);
      colorStep(sf, lm, code == 0x14 ? 2 : 1);
      break;
    }
    case 0x28:  // SQR
      for (int i = 1; i <= 3; i++) mac_[i] = (int32_t(ir_[i]) * ir_[i]) >> sf;
      for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
      break;
    case 0x2D: case 0x2E: {  // AVSZ3 e AVSZ4
      const int64_t s = code == 0x2D ? int64_t(zsf3_) * (sz_[1] + sz_[2] + sz_[3])
                                     : int64_t(zsf4_) * (sz_[0] + sz_[1] + sz_[2] + sz_[3]);
      mac_[0] = static_cast<int32_t>(chk32(s));
      const int64_t q = s >> 12;
      if (q < 0) { flag_ |= 1u << 18; otz_ = 0; }
      else if (q > 0xFFFF) { flag_ |= 1u << 18; otz_ = 0xFFFF; }
      else otz_ = static_cast<uint16_t>(q);
      break;
    }
    case 0x3D:  // GPF
      for (int i = 1; i <= 3; i++) mac_[i] = static_cast<int32_t>(chk44(i, int64_t(int32_t(ir_[i]) * ir_[0])) >> sf);
      for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
      pushColor();
      break;
    case 0x3E:  // GPL
      for (int i = 1; i <= 3; i++) {
        const int64_t base = chk44(i, int64_t(mac_[i]) << sf);
        mac_[i] = static_cast<int32_t>(chk44(i, base + int32_t(ir_[i]) * ir_[0]) >> sf);
      }
      for (int i = 1; i <= 3; i++) ir_[i] = satIR(i, mac_[i], lm);
      pushColor();
      break;
    default:  // comandos que não existem: só zeram o FLAG
      break;
  }
  if (flag_ & 0x7F87E000u) flag_ |= 0x80000000u;
  return cyclesOf(code);
}

// ---------------------------------------------------------------- save state

void Gte::sync(StateIO& s) {
  uint32_t ver = 1;
  s.sync(ver);
  if (ver != 1) throw std::runtime_error("Estado salvo do GTE em versão desconhecida.");
  s.sync(v_); s.sync(rgbc_); s.sync(otz_); s.sync(ir_); s.sync(sxy_); s.sync(sz_); s.sync(rgbFifo_);
  s.sync(res1_); s.sync(mac_); s.sync(lzcs_); s.sync(lzcr_);
  s.sync(rt_); s.sync(llm_); s.sync(lcm_); s.sync(tr_); s.sync(bk_); s.sync(fc_);
  s.sync(ofx_); s.sync(ofy_); s.sync(h_); s.sync(dqa_); s.sync(dqb_); s.sync(zsf3_); s.sync(zsf4_);
  s.sync(flag_);
  if (s.reading()) {
    flag_ &= 0xFFFFF000u;
    clampTo<uint32_t>(lzcr_, 1, 32);
  }
}

void Gte::save(StateIO& s) { sync(s); }
void Gte::load(StateIO& s) { sync(s); }

}  // namespace indigo::ps1
