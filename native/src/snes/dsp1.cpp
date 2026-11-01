#include "dsp1.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace indigo::snes {

// =====================================================================================================================
// NEC µPD7725
// =====================================================================================================================
bool Upd7725::load(const std::vector<uint8_t>& f) {
  if (f.size() != 8192) return false;
  for (int i = 0; i < 2048; i++) program[i] = f[i * 3] | (f[i * 3 + 1] << 8) | (uint32_t(f[i * 3 + 2]) << 16);
  for (int i = 0; i < 1024; i++) data[i] = static_cast<uint16_t>(f[6144 + i * 2] | (f[6144 + i * 2 + 1] << 8));
  return true;
}

void Upd7725::reset() {
  pc = 0; sp = 0; rp = 0x3FF; dp = 0;
  a = b = tr = trb = dr = k = l = m = n = so = 0;
  sr = 0;
  fa = Flags{}; fb = Flags{};
  for (auto& s : stack) s = 0;
}

void Upd7725::exec() {
  const uint32_t op = program[pc & 0x7FF];
  pc = (pc + 1) & 0x7FF;
  switch (op >> 22) {
    case 0: execOp(op); break;
    case 1: execOp(op); sp = (sp - 1) & 3; pc = stack[sp] & 0x7FF; break;  // RT: OP e volta
    case 2: execJp(op); break;
    case 3: execLd(op); break;
  }
  // o multiplicador trabalha sozinho: M e N sempre têm K × L
  const int32_t r = int32_t(int16_t(k)) * int16_t(l);
  m = static_cast<uint16_t>(r >> 15);
  n = static_cast<uint16_t>(r << 1);
}

void Upd7725::execOp(uint32_t op) {
  const int pselect = (op >> 20) & 3, alu = (op >> 16) & 15, asl = (op >> 15) & 1;
  const int dpl = (op >> 13) & 3, dphm = (op >> 9) & 15, rpdcr = (op >> 8) & 1, src = (op >> 4) & 15, dstReg = op & 15;
  uint16_t idb = 0;
  switch (src) {
    case 0: idb = trb; break;
    case 1: idb = a; break;
    case 2: idb = b; break;
    case 3: idb = tr; break;
    case 4: idb = dp; break;
    case 5: idb = rp; break;
    case 6: idb = data[rp & 0x3FF]; break;
    case 7: idb = fa.s1 ? 0x7FFF : 0x8000; break;  // SGN
    case 8: idb = dr; sr |= RQM; break;
    case 9: idb = dr; break;
    case 10: idb = sr; break;
    case 11: case 12: idb = 0; break;  // entrada serial (não ligada no cartucho)
    case 13: idb = k; break;
    case 14: idb = l; break;
    case 15: idb = ram[dp & 0xFF]; break;
  }
  if (alu) {
    uint16_t p = 0;
    switch (pselect) {
      case 0: p = ram[dp & 0xFF]; break;
      case 1: p = idb; break;
      case 2: p = m; break;
      case 3: p = n; break;
    }
    uint16_t& q = asl ? b : a;
    Flags& f = asl ? fb : fa;
    const bool carry = asl ? fa.c : fb.c;  // ADC/SBB usam o vai-um do outro acumulador
    uint16_t r = 0;
    switch (alu) {
      case 1: r = q | p; break;
      case 2: r = q & p; break;
      case 3: r = q ^ p; break;
      case 4: r = q - p; break;
      case 5: r = q + p; break;
      case 6: r = q - p - carry; break;
      case 7: r = q + p + carry; break;
      case 8: r = q - 1; p = 1; break;
      case 9: r = q + 1; p = 1; break;
      case 10: r = ~q; break;
      case 11: r = (q >> 1) | (q & 0x8000); break;
      case 12: r = static_cast<uint16_t>((q << 1) | carry); break;
      case 13: r = static_cast<uint16_t>((q << 2) | 3); break;
      case 14: r = static_cast<uint16_t>((q << 4) | 15); break;
      case 15: r = static_cast<uint16_t>((q << 8) | (q >> 8)); break;
    }
    f.s0 = r & 0x8000;
    f.z = r == 0;
    switch (alu) {
      case 4: case 5: case 6: case 7: case 8: case 9:
        if (alu & 1) { f.ov0 = (q ^ r) & (p ^ r) & 0x8000; f.c = r < q; }
        else { f.ov0 = (q ^ r) & (q ^ p) & 0x8000; f.c = r > q; }
        if (f.ov0) { f.s1 = f.ov1 ^ !(r & 0x8000); f.ov1 = !f.ov1; }
        break;
      case 11: f.c = q & 1; f.ov0 = f.ov1 = false; break;
      case 12: f.c = q >> 15; f.ov0 = f.ov1 = false; break;
      default: f.c = false; f.ov0 = f.ov1 = false; break;
    }
    q = r;
  }
  dst(dstReg, idb);
  switch (dpl) {
    case 1: dp = (dp & 0xF0) | ((dp + 1) & 0x0F); break;
    case 2: dp = (dp & 0xF0) | ((dp - 1) & 0x0F); break;
    case 3: dp &= 0xF0; break;
  }
  dp = (dp ^ (dphm << 4)) & 0xFF;
  if (rpdcr) rp = (rp - 1) & 0x3FF;
}

void Upd7725::dst(int d, uint16_t idb) {
  switch (d) {
    case 0: break;
    case 1: a = idb; break;
    case 2: b = idb; break;
    case 3: tr = idb; break;
    case 4: dp = idb & 0xFF; break;
    case 5: rp = idb & 0x3FF; break;
    case 6: dr = idb; sr |= RQM; break;
    case 7: sr = (sr & 0x907C) | (idb & ~0x907C); break;
    case 8: {  // saída serial, bit menos significativo primeiro
      uint16_t r = 0;
      for (int i = 0; i < 16; i++) if (idb & (1 << i)) r |= 1 << (15 - i);
      so = r;
      break;
    }
    case 9: so = idb; break;
    case 10: k = idb; break;
    case 11: k = idb; l = data[rp & 0x3FF]; break;
    case 12: l = idb; k = ram[(dp | 0x40) & 0xFF]; break;
    case 13: l = idb; break;
    case 14: trb = idb; break;
    case 15: ram[dp & 0xFF] = idb; break;
  }
}

void Upd7725::execJp(uint32_t op) {
  const int brch = (op >> 13) & 0x1FF;
  const uint16_t na = (op >> 2) & 0x7FF;
  bool r = false;
  switch (brch) {
    case 0x100: r = true; break;                                       // JMP
    case 0x140: stack[sp] = pc; sp = (sp + 1) & 3; r = true; break;   // CALL
    case 0x080: r = !fa.c; break;
    case 0x082: r = fa.c; break;
    case 0x084: r = !fb.c; break;
    case 0x086: r = fb.c; break;
    case 0x088: r = !fa.z; break;
    case 0x08A: r = fa.z; break;
    case 0x08C: r = !fb.z; break;
    case 0x08E: r = fb.z; break;
    case 0x090: r = !fa.ov0; break;
    case 0x092: r = fa.ov0; break;
    case 0x094: r = !fb.ov0; break;
    case 0x096: r = fb.ov0; break;
    case 0x098: r = !fa.ov1; break;
    case 0x09A: r = fa.ov1; break;
    case 0x09C: r = !fb.ov1; break;
    case 0x09E: r = fb.ov1; break;
    case 0x0A0: r = !fa.s0; break;
    case 0x0A2: r = fa.s0; break;
    case 0x0A4: r = !fb.s0; break;
    case 0x0A6: r = fb.s0; break;
    case 0x0A8: r = !fa.s1; break;
    case 0x0AA: r = fa.s1; break;
    case 0x0AC: r = !fb.s1; break;
    case 0x0AE: r = fb.s1; break;
    case 0x0B0: r = (dp & 0x0F) == 0x00; break;
    case 0x0B1: r = (dp & 0x0F) != 0x00; break;
    case 0x0B2: r = (dp & 0x0F) == 0x0F; break;
    case 0x0B3: r = (dp & 0x0F) != 0x0F; break;
    case 0x0B4: case 0x0B8: r = true; break;   // serial sempre pronto (não usado)
    case 0x0B6: case 0x0BA: r = false; break;
    case 0x0BC: r = !(sr & RQM); break;
    case 0x0BE: r = sr & RQM; break;
    default: break;
  }
  if (r) pc = na;
}

void Upd7725::execLd(uint32_t op) { dst(op & 15, static_cast<uint16_t>(op >> 6)); }

// lado do SNES
uint8_t Upd7725::readDR() {
  if (sr & DRC) { sr &= ~RQM; return static_cast<uint8_t>(dr); }  // modo de 8 bits
  if (!(sr & DRS)) { sr |= DRS; return static_cast<uint8_t>(dr); }
  sr &= ~(RQM | DRS);
  return static_cast<uint8_t>(dr >> 8);
}

void Upd7725::writeDR(uint8_t v) {
  if (sr & DRC) { sr &= ~RQM; dr = (dr & 0xFF00) | v; return; }
  if (!(sr & DRS)) { sr |= DRS; dr = (dr & 0xFF00) | v; return; }
  sr &= ~(RQM | DRS);
  dr = static_cast<uint16_t>((v << 8) | (dr & 0x00FF));
}

void Upd7725::sync(StateIO& io) {
  io.raw(ram, sizeof ram);
  io.sync(pc); io.sync(rp); io.sync(dp); io.sync(a); io.sync(b); io.sync(tr); io.sync(trb); io.sync(dr); io.sync(sr);
  io.sync(k); io.sync(l); io.sync(m); io.sync(n); io.sync(so);
  io.raw(stack, sizeof stack); io.sync(sp);
  for (Flags* f : {&fa, &fb}) { io.sync(f->ov0); io.sync(f->ov1); io.sync(f->z); io.sync(f->c); io.sync(f->s0); io.sync(f->s1); }
  if (io.reading()) { pc &= 0x7FF; rp &= 0x3FF; dp &= 0xFF; sp &= 3; }
}

// =====================================================================================================================
// DSP-1 sem o arquivo: tabelas calculadas
// =====================================================================================================================
namespace {

struct Tables {
  int16_t sin[256];     // seno em 256 passos (Q15)
  int16_t mul[256];     // passo fino do ângulo em radianos × 32768 (interpolação)
  int16_t guess[128];   // primeiro palpite do inverso (depois refinado por Newton)
  int16_t root[66];     // raiz quadrada em 64 passos (comando Distance)
  Tables() {
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < 256; i++) {
      sin[i] = static_cast<int16_t>(std::clamp(static_cast<int>(32768.0 * std::sin(2 * pi * i / 256)), -32767, 32767));
      mul[i] = static_cast<int16_t>(std::floor(i * pi + 1e-4));
    }
    for (int k = 0; k < 128; k++) guess[k] = static_cast<int16_t>(std::min(32767L, std::lround(536870912.0 / (16384 + 128 * k))));
    for (int p = 0; p < 66; p++) root[p] = static_cast<int16_t>(p > 64 ? 0 : std::min(32767L, std::lround(32767.0 * std::sqrt(p / 64.0))));
  }
};
const Tables T;

// potências de 2 usadas para deslocar (índices como na tabela do chip, $21 a $40); duas entradas do chip não são
// potências exatas ($31 = $7FFF e $3C = 1) e isso aparece nos resultados, então ficam iguais aqui
int16_t pow2(int idx) {
  if (idx <= 0x21) return 0;
  if (idx <= 0x30) return static_cast<int16_t>(1 << (idx - 0x22));
  if (idx == 0x31) return 0x7FFF;
  if (idx == 0x3C) return 1;
  if (idx <= 0x40) return static_cast<int16_t>(0x4000 >> (idx - 0x32));
  return 0;
}

int16_t dspSin(int16_t a) {
  if (a < 0) { if (a == -32768) return 0; return static_cast<int16_t>(-dspSin(static_cast<int16_t>(-a))); }
  int32_t s = T.sin[a >> 8] + (T.mul[a & 0xFF] * T.sin[0x40 + (a >> 8)] >> 15);
  return static_cast<int16_t>(std::min(s, 32767));
}

int16_t dspCos(int16_t a) {
  if (a < 0) { if (a == -32768) return -32768; a = static_cast<int16_t>(-a); }
  int32_t s = T.sin[0x40 + (a >> 8)] - (T.mul[a & 0xFF] * T.sin[a >> 8] >> 15);
  if (s < -32768) s = -32767;
  return static_cast<int16_t>(s);
}

int16_t i16(int32_t v) { return static_cast<int16_t>(v); }

// quantos bits de sinal sobram em m (0 a 15)
int leadingBits(int16_t m) {
  int16_t x = m < 0 ? static_cast<int16_t>(~m) : m;
  if (x == 0) return 15;
  int e = 0;
  for (int bit = 0x4000; bit && !(x & bit); bit >>= 1) e++;
  return e;
}

constexpr int16_t MAX_AZS[16] = {0x38b4, 0x38b7, 0x38ba, 0x38be, 0x38c0, 0x38c4, 0x38c7, 0x38ca,
                                 0x38ce, 0x38d0, 0x38d4, 0x38d7, 0x38da, 0x38dd, 0x38e0, 0x38e4};
}  // namespace

void Dsp1::inverse(int16_t c, int16_t e, int16_t& ic, int16_t& ie) const {
  if (c == 0) { ic = 0x7FFF; ie = 0x002F; return; }
  int sign = 1;
  if (c < 0) { if (c < -32767) c = -32767; c = static_cast<int16_t>(-c); sign = -1; }
  while (c < 0x4000) { c = static_cast<int16_t>(c << 1); e--; }
  if (c == 0x4000) {
    if (sign == 1) ic = 0x7FFF;
    else { ic = -0x4000; e--; }
  } else {
    int16_t i = T.guess[(c - 0x4000) >> 7];
    // Newton para 1/(2x), duas vezes
    i = i16((i + (-i * (c * i >> 15) >> 15)) << 1);
    i = i16((i + (-i * (c * i >> 15) >> 15)) << 1);
    ic = i16(i * sign);
  }
  ie = static_cast<int16_t>(1 - e);
}

void Dsp1::normalize(int16_t m, int16_t& c, int16_t& e) const {
  const int s = leadingBits(m);
  c = s > 0 ? i16((m * pow2(0x21 + s)) << 1) : m;
  e = static_cast<int16_t>(e - s);
}

void Dsp1::normalizeDouble(int32_t p, int16_t& c, int16_t& e) const {
  const int16_t lo = static_cast<int16_t>(p & 0x7FFF), hi = static_cast<int16_t>(p >> 15);
  int s = leadingBits(hi);
  if (s > 0) {
    c = i16((hi * pow2(0x21 + s)) << 1);
    if (s < 15) {
      c = i16(c + (lo * pow2(0x40 - s) >> 15));
    } else {
      int16_t t = hi < 0 ? static_cast<int16_t>(~(lo | 0x8000)) : lo;
      s += t == 0 ? 15 : leadingBits(t);
      if (s > 15) c = i16((lo * pow2(0x12 + s)) << 1);
      else c = i16(c + lo);
    }
  } else {
    c = hi;
  }
  e = static_cast<int16_t>(s);
}

int16_t Dsp1::truncate(int16_t c, int16_t e) const {
  if (e > 0) { if (c > 0) return 32767; if (c < 0) return -32767; return c; }
  if (e < 0) return i16(c * pow2(0x31 + e) >> 15);
  return c;
}

void Dsp1::attitude(int16_t (&mt)[3][3], int16_t m, int16_t zr, int16_t yr, int16_t xr) {
  const int sz = dspSin(zr), cz = dspCos(zr), sy = dspSin(yr), cy = dspCos(yr), sx = dspSin(xr), cx = dspCos(xr);
  m = static_cast<int16_t>(m >> 1);
  const int mcz = m * cz >> 15, msz = m * sz >> 15, msx = m * sx >> 15, mcx = m * cx >> 15;
  mt[0][0] = i16(mcz * cy >> 15);
  mt[0][1] = i16(-(msz * cy >> 15));
  mt[0][2] = i16(m * sy >> 15);
  mt[1][0] = i16((msz * cx >> 15) + ((mcz * sx >> 15) * sy >> 15));
  mt[1][1] = i16((mcz * cx >> 15) - ((msz * sx >> 15) * sy >> 15));
  mt[1][2] = i16(-(msx * cy >> 15));
  mt[2][0] = i16((msz * sx >> 15) - ((mcz * cx >> 15) * sy >> 15));
  mt[2][1] = i16((mcz * sx >> 15) + ((msz * cx >> 15) * sy >> 15));
  mt[2][2] = i16(mcx * cy >> 15);
}

// projeção em perspectiva (para o Mode 7): F = ponto de referência no chão, Lfe = olho até a tela, Les = olho até
// F, Aas = azimute, Azs = ângulo do zênite. Saídas: Vof, Vva (linha do horizonte), Cx, Cy (centro no chão)
void Dsp1::parameter(int16_t fx, int16_t fy, int16_t fz, int16_t lfe, int16_t les, int16_t aas, int16_t azs) {
  int16_t AZS = azs, c, e = 0, aux;
  sinAas_ = dspSin(aas); cosAas_ = dspCos(aas);
  sinAzs_ = dspSin(azs); cosAzs_ = dspCos(azs);
  nx_ = i16(sinAzs_ * -sinAas_ >> 15);
  ny_ = i16(sinAzs_ * cosAas_ >> 15);
  nz_ = i16(cosAzs_ * 0x7FFF >> 15);
  centreX_ = i16(fx + (lfe * nx_ >> 15));
  centreY_ = i16(fy + (lfe * ny_ >> 15));
  const int16_t centreZ = i16(fz + (lfe * nz_ >> 15));
  gx_ = i16(centreX_ - (les * nx_ >> 15));
  gy_ = i16(centreY_ - (les * ny_ >> 15));
  gz_ = i16(centreZ - (les * nz_ >> 15));
  eLes_ = 0;
  normalize(les, cLes_, eLes_);
  gLes_ = les;
  normalize(centreZ, c, e);
  vPlaneC_ = c;
  vPlaneE_ = e;
  // o ângulo do zênite é limitado conforme a altura (o horizonte não passa da tela)
  int16_t maxAZS = MAX_AZS[std::clamp(-e, 0, 15)];
  if (AZS < 0) { maxAZS = static_cast<int16_t>(-maxAZS); if (AZS < maxAZS + 1) AZS = static_cast<int16_t>(maxAZS + 1); }
  else if (AZS > maxAZS) AZS = maxAZS;
  sinAZS_ = dspSin(AZS);
  cosAZS_ = dspCos(AZS);
  inverse(cosAZS_, 0, secC1_, secE1_);
  normalize(i16(c * secC1_ >> 15), c, e);
  e = static_cast<int16_t>(e + secE1_);
  c = i16(truncate(c, e) * sinAZS_ >> 15);
  centreX_ = i16(centreX_ + (c * sinAas_ >> 15));
  centreY_ = i16(centreY_ - (c * cosAas_ >> 15));
  int16_t vof = 0;
  if (azs != AZS || azs == maxAZS) {
    // ângulo cortado: correções de tangente e secante (séries de Taylor de tan(πa/4) e sec(πa/4))
    if (azs == -32768) azs = -32767;
    c = static_cast<int16_t>(azs - maxAZS);
    if (c >= 0) c--;
    aux = static_cast<int16_t>(~(c << 2));
    c = i16(aux * 5292 >> 15);          // (π/4)³/3
    c = i16((c * aux >> 15) + 25736);   // π/4
    vof = i16(vof - (((c * aux >> 15) * les) >> 15));
    c = i16(aux * aux >> 15);
    aux = i16((c * 2598 >> 15) + 10106);  // 5(π/4)⁴/24 e (π/4)²/2
    cosAZS_ = i16(cosAZS_ + (((c * aux >> 15) * cosAZS_) >> 15));
  }
  vOffset_ = i16(les * cosAZS_ >> 15);
  int16_t csec, esec = 0;
  inverse(sinAZS_, 0, csec, esec);
  e = esec;
  normalize(vOffset_, c, e);
  normalize(i16(c * csec >> 15), c, e);
  if (c == -32768) { c = static_cast<int16_t>(c >> 1); e++; }
  const int16_t vva = truncate(static_cast<int16_t>(-c), e);
  inverse(cosAZS_, 0, secC2_, secE2_);
  out(0, vof);
  out(1, vva);
  out(2, centreX_);
  out(3, centreY_);
}

void Dsp1::rasterLine(int16_t vs, int16_t& an, int16_t& bn, int16_t& cn, int16_t& dn) const {
  int16_t c, e, c1, e1;
  inverse(i16((vs * sinAzs_ >> 15) + vOffset_), 7, c, e);
  e = static_cast<int16_t>(e + vPlaneE_);
  c1 = i16(c * vPlaneC_ >> 15);
  e1 = static_cast<int16_t>(e + secE2_);
  normalize(c1, c, e);
  c = truncate(c, e);
  an = i16(c * cosAas_ >> 15);
  cn = i16(c * sinAas_ >> 15);
  normalize(i16(c1 * secC2_ >> 15), c, e1);
  c = truncate(c, e1);
  bn = i16(c * -sinAas_ >> 15);
  dn = i16(c * cosAas_ >> 15);
}

void Dsp1::project(int16_t x, int16_t y, int16_t z, int16_t& h, int16_t& v, int16_t& mm) const {
  int16_t ex = 0, ey = 0, ez = 0, e2 = 0, px, py, pz;
  normalizeDouble(int32_t(x) - gx_, px, ex);
  normalizeDouble(int32_t(y) - gy_, py, ey);
  normalizeDouble(int32_t(z) - gz_, pz, ez);
  px = static_cast<int16_t>(px >> 1); ex--;
  py = static_cast<int16_t>(py >> 1); ey--;
  pz = static_cast<int16_t>(pz >> 1); ez--;
  int16_t refE = std::min({ey, ez, ex});
  auto shiftR = [](int16_t c, int16_t e) { return i16(c * pow2(0x31 + e) >> 15); };
  px = shiftR(px, static_cast<int16_t>(ex - refE));
  py = shiftR(py, static_cast<int16_t>(ey - refE));
  pz = shiftR(pz, static_cast<int16_t>(ez - refE));
  const int16_t dot = i16(-(px * nx_ >> 15) - (py * ny_ >> 15) - (pz * nz_ >> 15));
  int32_t aux4 = dot;
  refE = static_cast<int16_t>(16 - refE);
  if (refE >= 0) aux4 = static_cast<int32_t>(static_cast<uint32_t>(aux4) << refE);
  else aux4 >>= -refE;
  if (aux4 == -1) aux4 = 0;
  aux4 >>= 1;
  const int32_t aux = static_cast<int32_t>(static_cast<uint16_t>(gLes_)) + aux4;
  int16_t c10, c4, e4 = 0;
  normalizeDouble(aux, c10, e2);
  e2 = static_cast<int16_t>(15 - e2);
  inverse(c10, 0, c4, e4);
  const int16_t c2 = i16(c4 * cLes_ >> 15);  // fator de escala
  int16_t cc, ee = 0;
  const int16_t hh = i16((px * (cosAas_ * 0x7FFF >> 15) >> 15) + (py * (sinAas_ * 0x7FFF >> 15) >> 15));
  normalize(i16(hh * c2 >> 15), cc, ee);
  h = truncate(cc, static_cast<int16_t>(eLes_ - e2 + refE + ee));
  ee = 0;
  const int16_t vv = i16((px * (cosAzs_ * -sinAas_ >> 15) >> 15) + (py * (cosAzs_ * cosAas_ >> 15) >> 15) +
                         (pz * (-sinAzs_ * 0x7FFF >> 15) >> 15));
  normalize(i16(vv * c2 >> 15), cc, ee);
  v = truncate(cc, static_cast<int16_t>(eLes_ - e2 + refE + ee));
  normalize(c2, cc, e4);
  mm = truncate(cc, static_cast<int16_t>(e4 + eLes_ - e2 - 7));
}

void Dsp1::target(int16_t h, int16_t v, int16_t& x, int16_t& y) const {
  int16_t c, e, c1, e1;
  inverse(i16((v * sinAzs_ >> 15) + vOffset_), 8, c, e);
  e = static_cast<int16_t>(e + vPlaneE_);
  c1 = i16(c * vPlaneC_ >> 15);
  e1 = static_cast<int16_t>(e + secE1_);
  h = static_cast<int16_t>(h << 8);
  normalize(c1, c, e);
  c = i16(truncate(c, e) * h >> 15);
  // eixo horizontal da tela no chão = (cos, sen), o mesmo do Raster e do Project
  x = i16(centreX_ + (c * cosAas_ >> 15));
  y = i16(centreY_ + (c * sinAas_ >> 15));
  v = static_cast<int16_t>(v << 8);
  normalize(i16(c1 * secC1_ >> 15), c, e1);
  c = i16(truncate(c, e1) * v >> 15);
  x = i16(x + (c * -sinAas_ >> 15));
  y = i16(y + (c * cosAas_ >> 15));
}

// =====================================================================================================================
// DSP-1: protocolo
// =====================================================================================================================
Dsp1::Dsp1(const std::vector<uint8_t>& firmware) {
  lle_ = !firmware.empty() && chip_.load(firmware);
  reset();
}

void Dsp1::reset() {
  chip_.reset();
  debt_ = 0;
  cmd_ = 0;
  waiting_ = true;
  firstParam_ = true;
  inCount_ = inIndex_ = outCount_ = outIndex_ = 0;
}

void Dsp1::run(int clocks) {
  if (!lle_) return;
  // 7,6 MHz contra o relógio mestre de 21,477 MHz
  debt_ += int64_t(clocks) * 7600000;
  while (debt_ >= 21477272) { debt_ -= 21477272; chip_.exec(); }
}

uint8_t Dsp1::read(bool status) {
  if (lle_) return status ? chip_.readSR() : chip_.readDR();
  if (status || !outCount_) return 0x80;
  const uint8_t t = output_[outIndex_++];
  if (--outCount_ == 0) {
    if (cmd_ == 0x0A || cmd_ == 0x1A) raster();  // o Raster continua dando linhas até chegar outro comando
  }
  waiting_ = true;
  return t;
}

void Dsp1::write(bool status, uint8_t v) {
  if (lle_) { if (!status) chip_.writeDR(v); return; }
  if (status) return;
  if ((cmd_ == 0x0A || cmd_ == 0x1A) && outCount_ != 0) { outCount_--; outIndex_++; return; }
  if (waiting_) {
    command(v);
  } else {
    if (inIndex_ < static_cast<int>(sizeof param_)) param_[inIndex_] = v;
    inIndex_++;
    firstParam_ = false;
  }
  if (waiting_) { firstParam_ = false; return; }
  if (firstParam_) return;  // acabou de chegar o comando: espera os parâmetros
  if (inCount_ && --inCount_ == 0) {
    waiting_ = true;
    outIndex_ = 0;
    execute();
  }
}

void Dsp1::command(uint8_t c) {
  cmd_ = c;
  inIndex_ = 0;
  waiting_ = false;
  firstParam_ = true;
  int words;
  switch (c) {
    case 0x00: case 0x10: case 0x30: case 0x20: case 0x04: case 0x24: case 0x0E: case 0x1E: case 0x2E: case 0x3E: words = 2; break;
    case 0x08: case 0x28: case 0x0C: case 0x2C: case 0x06: case 0x16: case 0x26: case 0x36:
    case 0x09: case 0x39: case 0x3D: case 0x0D: case 0x19: case 0x1D: case 0x29: case 0x2D:
    case 0x03: case 0x33: case 0x13: case 0x23: case 0x0B: case 0x3B: case 0x1B: case 0x2B: words = 3; break;
    case 0x18: case 0x38: case 0x01: case 0x05: case 0x31: case 0x35: case 0x11: case 0x15: case 0x21: case 0x25: words = 4; break;
    case 0x1C: case 0x3C: case 0x14: case 0x34: words = 6; break;
    case 0x02: case 0x12: case 0x22: case 0x32: words = 7; break;
    case 0x0A: words = 1; break;
    case 0x1A: case 0x2A: case 0x3A: cmd_ = 0x1A; words = 1; break;
    case 0x07: case 0x0F: case 0x27: case 0x2F: words = 1; break;
    case 0x17: case 0x37: case 0x3F: case 0x1F: cmd_ = 0x1F; words = 1; break;
    default: words = 0; waiting_ = true; firstParam_ = true; break;  // $80 e desconhecidos: nada
  }
  inCount_ = words * 2;
}

void Dsp1::raster() {
  int16_t an, bn, cn, dn;
  rasterLine(rasterVs_, an, bn, cn, dn);
  rasterVs_++;
  out(0, an); out(1, bn); out(2, cn); out(3, dn);
  outCount_ = 8;
  outIndex_ = 0;
}

void Dsp1::execute() {
  // X² + Y² + Z² com os 32 bits do acumulador do chip (pode dar a volta, como lá)
  auto sq3 = [&] { return int64_t(in(0)) * in(0) + int64_t(in(1)) * in(1) + int64_t(in(2)) * in(2); };
  auto mat = [&](int16_t (&mt)[3][3], bool transposed) {
    const int16_t x = in(0), y = in(1), z = in(2);
    for (int r = 0; r < 3; r++) {
      const int16_t a0 = transposed ? mt[0][r] : mt[r][0], a1 = transposed ? mt[1][r] : mt[r][1], a2 = transposed ? mt[2][r] : mt[r][2];
      out(r, i16((x * a0 >> 15) + (y * a1 >> 15) + (z * a2 >> 15)));
    }
    outCount_ = 6;
  };
  switch (cmd_) {
    case 0x00: out(0, i16(in(0) * in(1) >> 15)); outCount_ = 2; break;
    case 0x20: out(0, i16((in(0) * in(1) >> 15) + 1)); outCount_ = 2; break;
    case 0x10: case 0x30: { int16_t c, e; inverse(in(0), in(1), c, e); out(0, c); out(1, e); outCount_ = 4; break; }
    case 0x04: case 0x24: {
      const int16_t a = in(0);
      const int r = static_cast<uint16_t>(in(1));
      out(0, i16(dspSin(a) * r >> 15)); out(1, i16(dspCos(a) * r >> 15)); outCount_ = 4;
      break;
    }
    case 0x08: {
      const int32_t s = static_cast<int32_t>(static_cast<uint32_t>(sq3()) << 1);
      out(0, static_cast<int16_t>(s & 0xFFFF)); out(1, static_cast<int16_t>((s >> 16) & 0xFFFF)); outCount_ = 4;
      break;
    }
    case 0x18: case 0x38: {
      const int32_t d = static_cast<int32_t>(static_cast<uint32_t>(sq3() - int64_t(in(3)) * in(3)));
      out(0, i16((d >> 15) + (cmd_ == 0x38 ? 1 : 0))); outCount_ = 2;
      break;
    }
    case 0x28: {
      const int32_t radius = static_cast<int32_t>(static_cast<uint32_t>(sq3()));
      int16_t r = 0;
      if (radius != 0) {
        int16_t c, e;
        normalizeDouble(radius, c, e);
        if (e & 1) c = i16(c * 0x4000 >> 15);
        const int pos = std::clamp(c * 0x40 >> 15, 0, 64);
        const int16_t n1 = T.root[pos], n2 = T.root[pos + 1];
        r = i16(((n2 - n1) * (c & 0x1FF) >> 9) + n1);
        r = static_cast<int16_t>(r >> (e >> 1));
      }
      out(0, r); outCount_ = 2;
      break;
    }
    case 0x0C: case 0x2C: {
      const int16_t a = in(0), x = in(1), y = in(2);
      out(0, i16((y * dspSin(a) >> 15) + (x * dspCos(a) >> 15)));
      out(1, i16((y * dspCos(a) >> 15) - (x * dspSin(a) >> 15)));
      outCount_ = 4;
      break;
    }
    case 0x1C: case 0x3C: {
      const int16_t az = in(0), ay = in(1), ax = in(2);
      int16_t xb = in(3), yb = in(4), zb = in(5);
      int16_t x1 = i16((yb * dspSin(az) >> 15) + (xb * dspCos(az) >> 15));
      int16_t y1 = i16((yb * dspCos(az) >> 15) - (xb * dspSin(az) >> 15));
      xb = x1; yb = y1;
      int16_t z1 = i16((xb * dspSin(ay) >> 15) + (zb * dspCos(ay) >> 15));
      x1 = i16((xb * dspCos(ay) >> 15) - (zb * dspSin(ay) >> 15));
      const int16_t xa = x1;
      zb = z1;
      y1 = i16((zb * dspSin(ax) >> 15) + (yb * dspCos(ax) >> 15));
      z1 = i16((zb * dspCos(ax) >> 15) - (yb * dspSin(ax) >> 15));
      out(0, xa); out(1, y1); out(2, z1); outCount_ = 6;
      break;
    }
    case 0x02: case 0x12: case 0x22: case 0x32:
      parameter(in(0), in(1), in(2), in(3), in(4), in(5), in(6));
      outCount_ = 8;
      break;
    case 0x0A: case 0x1A:
      rasterVs_ = in(0);
      inIndex_ = 0;
      raster();
      break;
    case 0x06: case 0x16: case 0x26: case 0x36: {
      int16_t h, v, mm;
      project(in(0), in(1), in(2), h, v, mm);
      out(0, h); out(1, v); out(2, mm); outCount_ = 6;
      break;
    }
    case 0x0E: case 0x1E: case 0x2E: case 0x3E: {
      int16_t x, y;
      target(in(0), in(1), x, y);
      out(0, x); out(1, y); outCount_ = 4;
      break;
    }
    case 0x01: case 0x05: case 0x31: case 0x35: attitude(matA_, in(0), in(1), in(2), in(3)); break;
    case 0x11: case 0x15: attitude(matB_, in(0), in(1), in(2), in(3)); break;
    case 0x21: case 0x25: attitude(matC_, in(0), in(1), in(2), in(3)); break;
    case 0x0D: case 0x09: case 0x39: case 0x3D: mat(matA_, false); break;  // Objective: (F, L, U) = matriz × (X, Y, Z)
    case 0x1D: case 0x19: mat(matB_, false); break;
    case 0x2D: case 0x29: mat(matC_, false); break;
    case 0x03: case 0x33: mat(matA_, true); break;  // Subjective: (X, Y, Z) = transposta × (F, L, U)
    case 0x13: mat(matB_, true); break;
    case 0x23: mat(matC_, true); break;
    case 0x0B: case 0x3B: case 0x1B: case 0x2B: {
      int16_t (&mt)[3][3] = cmd_ == 0x1B ? matB_ : cmd_ == 0x2B ? matC_ : matA_;
      out(0, i16((in(0) * mt[0][0] + in(1) * mt[0][1] + in(2) * mt[0][2]) >> 15)); outCount_ = 2;
      break;
    }
    case 0x14: case 0x34: {
      const int16_t zr = in(0), xr = in(1), yr = in(2), u = in(3), f = in(4), l = in(5);
      int16_t csec, esec = 0, c, e = 0, csin, ctan;
      inverse(dspCos(xr), 0, csec, esec);
      normalizeDouble(u * dspCos(yr) - f * dspSin(yr), c, e);
      e = static_cast<int16_t>(esec - e);
      normalize(i16(c * csec >> 15), c, e);
      const int16_t zrr = i16(zr + truncate(c, e));
      const int16_t xrr = i16(xr + (u * dspSin(yr) >> 15) + (f * dspCos(yr) >> 15));
      normalizeDouble(u * dspCos(yr) + f * dspSin(yr), c, e);
      e = static_cast<int16_t>(esec - e);
      normalize(dspSin(xr), csin, e);
      ctan = i16(csec * csin >> 15);
      normalize(i16(-(c * ctan >> 15)), c, e);
      const int16_t yrr = i16(yr + truncate(c, e) + l);
      out(0, zrr); out(1, xrr); out(2, yrr); outCount_ = 6;
      break;
    }
    case 0x07: case 0x0F: out(0, 0x0000); outCount_ = 2; break;   // teste da memória: passou
    case 0x27: case 0x2F: out(0, 0x0100); outCount_ = 2; break;   // tamanho da memória
    case 0x1F: outCount_ = 0; break;  // cópia da ROM de dados: sem o arquivo do chip não há o que mandar
    default: break;
  }
}

void Dsp1::sync(StateIO& io) {
  io.sync(debt_);
  if (io.reading() && (debt_ < 0 || debt_ >= 21477272)) debt_ = 0;
  if (lle_) { chip_.sync(io); return; }
  io.sync(cmd_); io.sync(waiting_); io.sync(firstParam_);
  io.sync(inCount_); io.sync(inIndex_); io.sync(outCount_); io.sync(outIndex_);
  io.raw(param_, sizeof param_); io.raw(output_, sizeof output_);
  io.sync(rasterVs_);
  io.raw(matA_, sizeof matA_); io.raw(matB_, sizeof matB_); io.raw(matC_, sizeof matC_);
  int16_t* f[] = {&sinAas_, &cosAas_, &sinAzs_, &cosAzs_, &sinAZS_, &cosAZS_, &nx_, &ny_, &nz_, &gx_, &gy_, &gz_, &cLes_, &eLes_,
                  &gLes_, &centreX_, &centreY_, &vOffset_, &vPlaneC_, &vPlaneE_, &secC1_, &secE1_, &secC2_, &secE2_};
  for (int16_t* p : f) io.sync(*p);
  if (io.reading()) {
    inCount_ = std::clamp(inCount_, 0, 14);
    inIndex_ = std::clamp(inIndex_, 0, 16);
    outCount_ = std::clamp(outCount_, 0, 16);
    outIndex_ = std::clamp(outIndex_, 0, 16 - outCount_);
  }
}

}  // namespace indigo::snes
