// Engine 3D do DS: geometria (matrizes, pilhas, luzes, listas de polígonos alimentadas pela FIFO de comandos)
// e rasterizador em software (texturas nos 7 formatos, sombreamento Gouraud, modular/decalque/toon/destaque,
// alfa, teste de alfa, buffer de profundidade Z ou W, névoa, marcação de bordas e imagem de fundo).
// Escrito a partir do GBATEK. Os comandos executam na hora (a FIFO nunca enche).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "nds.h"

namespace indigo::nds {

namespace {
// parâmetros de cada comando (0x00..0x7F)
int paramCount(int cmd) {
  switch (cmd) {
    case 0x10: case 0x12: case 0x13: case 0x14: return 1;
    case 0x16: case 0x18: return 16;
    case 0x17: case 0x19: return 12;
    case 0x1A: return 9;
    case 0x1B: case 0x1C: return 3;
    case 0x20: case 0x21: case 0x22: return 1;
    case 0x23: return 2;
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: return 1;
    case 0x30: case 0x31: case 0x32: case 0x33: return 1;
    case 0x34: return 32;
    case 0x40: return 1;
    case 0x50: return 1;
    case 0x60: return 1;
    case 0x70: return 3;
    case 0x71: return 2;
    case 0x72: return 1;
    default: return 0;
  }
}
const int32_t IDENT[16] = {4096, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 4096, 0, 0, 0, 0, 4096};
inline int32_t mulf(int32_t a, int32_t b) { return static_cast<int32_t>((int64_t(a) * b) >> 12); }
inline int32_t sx10(uint32_t v) { return static_cast<int32_t>(v << 22) >> 22; }
inline uint16_t rgb6to5(int r, int g, int b) { return uint16_t((r >> 1) | ((g >> 1) << 5) | ((b >> 1) << 10)); }
}  // namespace

void Gpu3D::reset() {
  std::memset(color, 0, sizeof color);
  std::memset(alpha, 0, sizeof alpha);
  disp3dcnt = 0;
  std::memset(regs, 0, sizeof regs);
  swapPending = false; swapParam = 0;
  packed_ = 0; need_ = got_ = 0; cmd_ = 0;
  portNeed_ = portGot_ = 0; portCmd_ = -1;
  queued_.clear();
  std::memcpy(proj_, IDENT, sizeof proj_); std::memcpy(pos_, IDENT, sizeof pos_);
  std::memcpy(vec_, IDENT, sizeof vec_); std::memcpy(tex_, IDENT, sizeof tex_);
  projSp_ = posSp_ = texSp_ = 0; mtxMode_ = 0; stackErr_ = false; clipDirty_ = true;
  vx_ = vy_ = vz_ = 0; cr_ = cg_ = cb_ = 31 << 4; ts_ = tt_ = 0;
  polyAttr_ = polyAttrPending_ = texParam_ = palBase_ = 0; diffAmb_ = specEmi_ = 0;
  std::memset(shininess_, 0, sizeof shininess_);
  std::memset(lightVec_, 0, sizeof lightVec_);
  std::memset(lightColor_, 0, sizeof lightColor_);
  primType_ = 0; inPrim_ = false; stripN_ = stripCount_ = 0;
  vpX1_ = 0; vpY1_ = 0; vpX2_ = 255; vpY2_ = 191;
  boxResult_ = false;
  for (auto& v : verts_) v.clear();
  for (auto& p : polys_) p.clear();
  cur_ = 0; sortManual_ = wbuf_ = false;
  rendered = 0;
}

// ------------------------------------------------------------------ entrada de comandos
void Gpu3D::writeFifo(uint32_t v) {
  if (swapPending) { if (queued_.size() < (1u << 22)) queued_.push_back(uint64_t(v)); return; }
  fifoWord(v);
  gxIrq();
}

void Gpu3D::writeCmd(int cmd, uint32_t param) {
  if (swapPending) { if (queued_.size() < (1u << 22)) queued_.push_back((uint64_t(cmd + 1) << 32) | param); return; }
  portCmd(cmd, param);
  gxIrq();
}

void Gpu3D::fifoWord(uint32_t v) {
  if (need_ > 0) {
    par_[got_++] = v;
    if (got_ < need_) return;
    need_ = 0;
    exec(cmd_, par_);
  } else {
    packed_ = v;
  }
  while (packed_ && need_ == 0 && !swapPending) {
    const int c = packed_ & 0xFF;
    packed_ >>= 8;
    const int n = paramCount(c);
    if (n == 0) { exec(c, nullptr); continue; }
    cmd_ = c; need_ = n; got_ = 0;
  }
  // comandos que sobraram no pacote depois de um SWAP_BUFFERS esperam a troca
  if (swapPending && packed_) { queued_.push_back(uint64_t(packed_)); packed_ = 0; }
}

void Gpu3D::portCmd(int cmd, uint32_t param) {
  cmd &= 0x7F;
  const int n = paramCount(cmd);
  if (n == 0) { exec(cmd, nullptr); return; }
  if (portCmd_ != cmd) { portCmd_ = cmd; portGot_ = 0; portNeed_ = n; }
  portPar_[portGot_++] = param;
  if (portGot_ >= portNeed_) { portGot_ = 0; portCmd_ = -1; exec(cmd, portPar_); }
}

void Gpu3D::gxIrq() {
  // a FIFO está sempre vazia: as condições "menos da metade" e "vazia" valem o tempo todo
  const uint32_t mode = (static_cast<uint32_t>(regsGx_) >> 30) & 3;
  if (mode == 1 || mode == 2) s.irq9(1u << 21);
}

void Gpu3D::writeGxstat(uint32_t v) {
  if (v & 0x8000) { stackErr_ = false; projSp_ = 0; }
  regsGx_ = (regsGx_ & 0x3FFFFFFF) | (v & 0xC0000000u);
  gxIrq();
}

uint32_t Gpu3D::readReg(uint32_t a) {
  const uint32_t off = a & 0xFFF;
  if (off == 0x320) return 46;  // RDLINES_COUNT (antes da faixa da tabela)
  if (off > 0x320 && off < 0x3C0) {
    const uint32_t o = off - 0x320;
    return regs[o] | (o + 1 < sizeof regs ? regs[o + 1] << 8 : 0) | (o + 3 < sizeof regs ? (regs[o + 2] << 16) | (regs[o + 3] << 24) : 0);
  }
  if ((off & ~3u) == 0x600) {
    uint32_t v = 0x06000000u;  // FIFO vazia e com menos da metade
    if (boxResult_) v |= 2;
    v |= uint32_t(posSp_ & 31) << 8;
    v |= uint32_t(projSp_ & 1) << 13;
    if (stackErr_) v |= 0x8000;
    v |= regsGx_ & 0xC0000000u;
    return (off & 2) ? v >> 16 : v;
  }
  if ((off & ~3u) == 0x604) {
    const uint32_t v = uint32_t(polys_[cur_].size()) | (uint32_t(verts_[cur_].size()) << 16);
    return (off & 2) ? v >> 16 : v;
  }
  if (off >= 0x620 && off < 0x630) {
    const uint32_t v = static_cast<uint32_t>(posResult_[(off - 0x620) >> 2]);
    return (off & 2) ? v >> 16 : v;
  }
  if (off >= 0x630 && off < 0x636) {
    const int32_t v = vecResult_[(off - 0x630) >> 1];
    return static_cast<uint32_t>(v) & 0xFFFF;
  }
  if (off >= 0x640 && off < 0x680) {
    if (clipDirty_) updateClip();
    const uint32_t v = static_cast<uint32_t>(clip_[(off - 0x640) >> 2]);
    return (off & 2) ? v >> 16 : v;
  }
  if (off >= 0x680 && off < 0x6A4) {
    const int i = static_cast<int>((off - 0x680) >> 2);
    const uint32_t v = static_cast<uint32_t>(vec_[(i / 3) * 4 + i % 3]);
    return (off & 2) ? v >> 16 : v;
  }
  return 0;
}

// ------------------------------------------------------------------ matrizes
void Gpu3D::mtxMult(int32_t* dst, const int32_t* m) {
  int32_t r[16];
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++) {
      int64_t acc = 0;
      for (int k = 0; k < 4; k++) acc += int64_t(m[i * 4 + k]) * dst[k * 4 + j];
      r[i * 4 + j] = static_cast<int32_t>(acc >> 12);
    }
  std::memcpy(dst, r, sizeof r);
}

void Gpu3D::curMtxMult(const int32_t* m, bool scale) {
  switch (mtxMode_) {
    case 0: mtxMult(proj_, m); break;
    case 1: mtxMult(pos_, m); break;
    case 2: mtxMult(pos_, m); if (!scale) mtxMult(vec_, m); break;
    default: mtxMult(tex_, m); break;
  }
  clipDirty_ = true;
}

void Gpu3D::curMtxLoad(const int32_t* m) {
  switch (mtxMode_) {
    case 0: std::memcpy(proj_, m, sizeof proj_); break;
    case 1: std::memcpy(pos_, m, sizeof pos_); break;
    case 2: std::memcpy(pos_, m, sizeof pos_); std::memcpy(vec_, m, sizeof vec_); break;
    default: std::memcpy(tex_, m, sizeof tex_); break;
  }
  clipDirty_ = true;
}

void Gpu3D::updateClip() {
  std::memcpy(clip_, proj_, sizeof clip_);
  mtxMult(clip_, pos_);
  clipDirty_ = false;
}

// ------------------------------------------------------------------ execução
void Gpu3D::exec(int cmd, const uint32_t* p) {
  switch (cmd) {
    case 0x10: mtxMode_ = p[0] & 3; break;
    case 0x11:  // MTX_PUSH
      if (mtxMode_ == 0) { if (projSp_ > 0) stackErr_ = true; std::memcpy(projStack_, proj_, sizeof proj_); projSp_ = 1; }
      else if (mtxMode_ == 3) { std::memcpy(texStack_, tex_, sizeof tex_); texSp_ = 1; }
      else {
        if (posSp_ >= 31) stackErr_ = true;
        std::memcpy(posStack_[posSp_ & 31], pos_, sizeof pos_);
        std::memcpy(vecStack_[posSp_ & 31], vec_, sizeof vec_);
        posSp_ = (posSp_ + 1) & 63;
        if (posSp_ > 31) posSp_ = 31;
      }
      break;
    case 0x12: {  // MTX_POP
      if (mtxMode_ == 0) { if (projSp_ == 0) stackErr_ = true; projSp_ = 0; std::memcpy(proj_, projStack_, sizeof proj_); }
      else if (mtxMode_ == 3) { texSp_ = 0; std::memcpy(tex_, texStack_, sizeof tex_); }
      else {
        const int n = static_cast<int32_t>(p[0] << 26) >> 26;
        posSp_ -= n;
        if (posSp_ < 0 || posSp_ > 30) { stackErr_ = true; posSp_ = std::clamp(posSp_, 0, 30); }
        std::memcpy(pos_, posStack_[posSp_], sizeof pos_);
        std::memcpy(vec_, vecStack_[posSp_], sizeof vec_);
      }
      clipDirty_ = true;
      break;
    }
    case 0x13: {  // MTX_STORE
      if (mtxMode_ == 0) std::memcpy(projStack_, proj_, sizeof proj_);
      else if (mtxMode_ == 3) std::memcpy(texStack_, tex_, sizeof tex_);
      else {
        const int i = p[0] & 31;
        if (i == 31) stackErr_ = true;
        std::memcpy(posStack_[i], pos_, sizeof pos_);
        std::memcpy(vecStack_[i], vec_, sizeof vec_);
      }
      break;
    }
    case 0x14: {  // MTX_RESTORE
      if (mtxMode_ == 0) std::memcpy(proj_, projStack_, sizeof proj_);
      else if (mtxMode_ == 3) std::memcpy(tex_, texStack_, sizeof tex_);
      else {
        const int i = p[0] & 31;
        if (i == 31) stackErr_ = true;
        std::memcpy(pos_, posStack_[i], sizeof pos_);
        std::memcpy(vec_, vecStack_[i], sizeof vec_);
      }
      clipDirty_ = true;
      break;
    }
    case 0x15: curMtxLoad(IDENT); break;
    case 0x16: { int32_t m[16]; for (int i = 0; i < 16; i++) m[i] = int32_t(p[i]); curMtxLoad(m); break; }
    case 0x17: {
      int32_t m[16];
      for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = int32_t(p[r * 3 + c]); m[r * 4 + 3] = r == 3 ? 4096 : 0; }
      curMtxLoad(m);
      break;
    }
    case 0x18: { int32_t m[16]; for (int i = 0; i < 16; i++) m[i] = int32_t(p[i]); curMtxMult(m); break; }
    case 0x19: {
      int32_t m[16];
      for (int r = 0; r < 4; r++) { for (int c = 0; c < 3; c++) m[r * 4 + c] = int32_t(p[r * 3 + c]); m[r * 4 + 3] = r == 3 ? 4096 : 0; }
      curMtxMult(m);
      break;
    }
    case 0x1A: {
      int32_t m[16] = {};
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) m[r * 4 + c] = int32_t(p[r * 3 + c]);
      m[15] = 4096;
      curMtxMult(m);
      break;
    }
    case 0x1B: {
      int32_t m[16] = {};
      m[0] = int32_t(p[0]); m[5] = int32_t(p[1]); m[10] = int32_t(p[2]); m[15] = 4096;
      curMtxMult(m, true);
      break;
    }
    case 0x1C: {
      int32_t m[16];
      std::memcpy(m, IDENT, sizeof m);
      m[12] = int32_t(p[0]); m[13] = int32_t(p[1]); m[14] = int32_t(p[2]);
      curMtxMult(m);
      break;
    }
    case 0x20:  // COLOR
      cr_ = (p[0] & 31) << 4; cg_ = ((p[0] >> 5) & 31) << 4; cb_ = ((p[0] >> 10) & 31) << 4;
      break;
    case 0x21: lighting(p[0]); break;
    case 0x22:  // TEXCOORD
      ts_ = static_cast<int16_t>(p[0] & 0xFFFF); tt_ = static_cast<int16_t>(p[0] >> 16);
      if ((texParam_ >> 30) == 1) {
        const int64_t s0 = ts_, t0 = tt_;
        ts_ = static_cast<int32_t>((s0 * tex_[0] + t0 * tex_[4] + tex_[8] + tex_[12]) >> 12);
        tt_ = static_cast<int32_t>((s0 * tex_[1] + t0 * tex_[5] + tex_[9] + tex_[13]) >> 12);
      }
      break;
    case 0x23: vx_ = int16_t(p[0] & 0xFFFF); vy_ = int16_t(p[0] >> 16); vz_ = int16_t(p[1] & 0xFFFF); addVertex(); break;
    case 0x24: vx_ = sx10(p[0]) << 6; vy_ = sx10(p[0] >> 10) << 6; vz_ = sx10(p[0] >> 20) << 6; addVertex(); break;
    case 0x25: vx_ = int16_t(p[0] & 0xFFFF); vy_ = int16_t(p[0] >> 16); addVertex(); break;
    case 0x26: vx_ = int16_t(p[0] & 0xFFFF); vz_ = int16_t(p[0] >> 16); addVertex(); break;
    case 0x27: vy_ = int16_t(p[0] & 0xFFFF); vz_ = int16_t(p[0] >> 16); addVertex(); break;
    case 0x28:
      vx_ = int16_t(vx_ + (sx10(p[0]) << 3)); vy_ = int16_t(vy_ + (sx10(p[0] >> 10) << 3)); vz_ = int16_t(vz_ + (sx10(p[0] >> 20) << 3));
      addVertex();
      break;
    case 0x29: polyAttrPending_ = p[0]; break;
    case 0x2A: texParam_ = p[0]; break;
    case 0x2B: palBase_ = p[0] & 0x1FFF; break;
    case 0x30:
      diffAmb_ = p[0];
      if (p[0] & 0x8000) { cr_ = (p[0] & 31) << 4; cg_ = ((p[0] >> 5) & 31) << 4; cb_ = ((p[0] >> 10) & 31) << 4; }
      break;
    case 0x31: specEmi_ = p[0]; break;
    case 0x32: {  // LIGHT_VECTOR: transformada pela matriz de direção
      const int l = p[0] >> 30;
      const int32_t x = sx10(p[0]) << 3, y = sx10(p[0] >> 10) << 3, z = sx10(p[0] >> 20) << 3;
      for (int c = 0; c < 3; c++) lightVec_[l][c] = static_cast<int32_t>((int64_t(x) * vec_[c] + int64_t(y) * vec_[4 + c] + int64_t(z) * vec_[8 + c]) >> 12);
      break;
    }
    case 0x33: lightColor_[p[0] >> 30] = p[0] & 0x7FFF; break;
    case 0x34: for (int i = 0; i < 32; i++) for (int k = 0; k < 4; k++) shininess_[i * 4 + k] = uint8_t(p[i] >> (8 * k)); break;
    case 0x40:
      primType_ = p[0] & 3; inPrim_ = true; stripN_ = 0; stripCount_ = 0;
      polyAttr_ = polyAttrPending_;
      break;
    case 0x41: inPrim_ = false; break;
    case 0x50: swapPending = true; swapParam = p[0]; break;
    case 0x60:
      vpX1_ = p[0] & 0xFF; vpY1_ = (p[0] >> 8) & 0xFF; vpX2_ = (p[0] >> 16) & 0xFF; vpY2_ = (p[0] >> 24) & 0xFF;
      break;
    case 0x70: {  // BOX_TEST
      if (clipDirty_) updateClip();
      const int32_t x = int16_t(p[0] & 0xFFFF), y = int16_t(p[0] >> 16), z = int16_t(p[1] & 0xFFFF);
      const int32_t w = int16_t(p[1] >> 16), h = int16_t(p[2] & 0xFFFF), d = int16_t(p[2] >> 16);
      // dentro se não estiver inteiramente fora de um mesmo plano
      int outAll = 0x3F;
      for (int k = 0; k < 8; k++) {
        const int64_t vx = x + ((k & 1) ? w : 0), vy = y + ((k & 2) ? h : 0), vz = z + ((k & 4) ? d : 0);
        int64_t c[4];
        for (int j = 0; j < 4; j++) c[j] = (vx * clip_[j] + vy * clip_[4 + j] + vz * clip_[8 + j] + 4096LL * clip_[12 + j]) >> 12;
        int out = 0;
        if (c[0] < -c[3]) out |= 1;
        if (c[0] > c[3]) out |= 2;
        if (c[1] < -c[3]) out |= 4;
        if (c[1] > c[3]) out |= 8;
        if (c[2] < -c[3]) out |= 16;
        if (c[2] > c[3]) out |= 32;
        outAll &= out;
      }
      boxResult_ = outAll == 0;
      break;
    }
    case 0x71: {  // POS_TEST
      if (clipDirty_) updateClip();
      vx_ = int16_t(p[0] & 0xFFFF); vy_ = int16_t(p[0] >> 16); vz_ = int16_t(p[1] & 0xFFFF);
      for (int j = 0; j < 4; j++)
        posResult_[j] = static_cast<int32_t>((int64_t(vx_) * clip_[j] + int64_t(vy_) * clip_[4 + j] + int64_t(vz_) * clip_[8 + j] + 4096LL * clip_[12 + j]) >> 12);
      break;
    }
    case 0x72: {  // VEC_TEST
      const int32_t x = sx10(p[0]) << 3, y = sx10(p[0] >> 10) << 3, z = sx10(p[0] >> 20) << 3;
      for (int c = 0; c < 3; c++) {
        int32_t v = static_cast<int32_t>((int64_t(x) * vec_[c] + int64_t(y) * vec_[4 + c] + int64_t(z) * vec_[8 + c]) >> 12);
        vecResult_[c] = static_cast<int16_t>(static_cast<uint16_t>(v & 0xFFFF) | ((v < 0) ? 0xF000 : 0));
      }
      break;
    }
    default: break;
  }
}

void Gpu3D::lighting(uint32_t normal) {
  const int32_t nx = sx10(normal) << 3, ny = sx10(normal >> 10) << 3, nz = sx10(normal >> 20) << 3;
  if ((texParam_ >> 30) == 2) {
    ts_ += static_cast<int32_t>((int64_t(nx) * tex_[0] + int64_t(ny) * tex_[4] + int64_t(nz) * tex_[8]) >> 20);
    tt_ += static_cast<int32_t>((int64_t(nx) * tex_[1] + int64_t(ny) * tex_[5] + int64_t(nz) * tex_[9]) >> 20);
  }
  int32_t n[3];
  for (int c = 0; c < 3; c++) n[c] = static_cast<int32_t>((int64_t(nx) * vec_[c] + int64_t(ny) * vec_[4 + c] + int64_t(nz) * vec_[8 + c]) >> 12);
  const int dif[3] = {int(diffAmb_ & 31), int((diffAmb_ >> 5) & 31), int((diffAmb_ >> 10) & 31)};
  const int amb[3] = {int((diffAmb_ >> 16) & 31), int((diffAmb_ >> 21) & 31), int((diffAmb_ >> 26) & 31)};
  const int spe[3] = {int(specEmi_ & 31), int((specEmi_ >> 5) & 31), int((specEmi_ >> 10) & 31)};
  const int emi[3] = {int((specEmi_ >> 16) & 31), int((specEmi_ >> 21) & 31), int((specEmi_ >> 26) & 31)};
  int64_t acc[3] = {int64_t(emi[0]) << 12, int64_t(emi[1]) << 12, int64_t(emi[2]) << 12};
  for (int l = 0; l < 4; l++) {
    if (!(polyAttrPending_ & (1u << l)) && !(polyAttr_ & (1u << l))) continue;
    const int32_t* L = lightVec_[l];
    int64_t d = -((int64_t(L[0]) * n[0] + int64_t(L[1]) * n[1] + int64_t(L[2]) * n[2]) >> 12);
    if (d < 0) d = 0;
    if (d > 4096) d = 4096;
    // meio-vetor entre a luz e a linha de visão (0, 0, -1)
    const int64_t h[3] = {L[0] / 2, L[1] / 2, (L[2] - 4096) / 2};
    int64_t sh = -((h[0] * n[0] + h[1] * n[1] + h[2] * n[2]) >> 12);
    if (sh < 0) sh = 0;
    if (sh > 4096) sh = 4096;
    sh = (sh * sh) >> 12;
    if (specEmi_ & 0x8000) sh = int64_t(shininess_[(sh >> 5) & 127]) << 4;
    const uint32_t lc = lightColor_[l];
    const int lcol[3] = {int(lc & 31), int((lc >> 5) & 31), int((lc >> 10) & 31)};
    for (int c = 0; c < 3; c++) {
      acc[c] += (int64_t(spe[c]) * lcol[c] * sh) / 31;
      acc[c] += (int64_t(dif[c]) * lcol[c] * d) / 31;
      acc[c] += (int64_t(amb[c]) * lcol[c] << 12) / 31;
    }
  }
  cr_ = static_cast<int32_t>(std::min<int64_t>(acc[0] >> 12, 31)) << 4;
  cg_ = static_cast<int32_t>(std::min<int64_t>(acc[1] >> 12, 31)) << 4;
  cb_ = static_cast<int32_t>(std::min<int64_t>(acc[2] >> 12, 31)) << 4;
}

void Gpu3D::addVertex() {
  if (!inPrim_) return;
  if (clipDirty_) updateClip();
  Vertex v;
  const int64_t x = vx_, y = vy_, z = vz_;
  v.x = static_cast<int32_t>((x * clip_[0] + y * clip_[4] + z * clip_[8] + 4096LL * clip_[12]) >> 12);
  v.y = static_cast<int32_t>((x * clip_[1] + y * clip_[5] + z * clip_[9] + 4096LL * clip_[13]) >> 12);
  v.z = static_cast<int32_t>((x * clip_[2] + y * clip_[6] + z * clip_[10] + 4096LL * clip_[14]) >> 12);
  v.w = static_cast<int32_t>((x * clip_[3] + y * clip_[7] + z * clip_[11] + 4096LL * clip_[15]) >> 12);
  v.r = cr_; v.g = cg_; v.b = cb_;
  if ((texParam_ >> 30) == 3) {
    v.s = ts_ + static_cast<int32_t>((x * tex_[0] + y * tex_[4] + z * tex_[8]) >> 20);
    v.t = tt_ + static_cast<int32_t>((x * tex_[1] + y * tex_[5] + z * tex_[9]) >> 20);
  } else { v.s = ts_; v.t = tt_; }
  strip_[stripN_++] = v;
  switch (primType_) {
    case 0:  // triângulos
      if (stripN_ == 3) { submit(strip_, 3); stripN_ = 0; }
      break;
    case 1:  // quadriláteros
      if (stripN_ == 4) { submit(strip_, 4); stripN_ = 0; }
      break;
    case 2:  // faixa de triângulos
      if (stripN_ == 3) {
        if (stripCount_ & 1) { Vertex t[3] = {strip_[1], strip_[0], strip_[2]}; submit(t, 3); }
        else submit(strip_, 3);
        stripCount_++;
        strip_[0] = strip_[1]; strip_[1] = strip_[2]; stripN_ = 2;
      }
      break;
    default:  // faixa de quadriláteros
      if (stripN_ == 4) {
        Vertex q[4] = {strip_[0], strip_[1], strip_[3], strip_[2]};
        submit(q, 4);
        strip_[0] = strip_[2]; strip_[1] = strip_[3]; stripN_ = 2;
      }
      break;
  }
}

namespace {
// recorte contra um plano: coordenada k (0 x, 1 y, 2 z), lado sgn (+1: v <= w, -1: v >= -w)
int clipPlane(const Vertex* in, int n, Vertex* out, int k, int sgn) {
  auto dist = [&](const Vertex& v) -> int64_t {
    const int32_t c = k == 0 ? v.x : k == 1 ? v.y : v.z;
    return int64_t(v.w) - int64_t(sgn) * c;
  };
  int m = 0;
  for (int i = 0; i < n; i++) {
    const Vertex& a = in[i];
    const Vertex& b = in[(i + 1) % n];
    const int64_t da = dist(a), db = dist(b);
    if (da >= 0) out[m++] = a;
    if ((da >= 0) != (db >= 0)) {
      const double t = double(da) / double(da - db);
      Vertex v;
      auto lerp = [&](int32_t p, int32_t q) { return static_cast<int32_t>(std::lround(p + (q - p) * t)); };
      v.x = lerp(a.x, b.x); v.y = lerp(a.y, b.y); v.z = lerp(a.z, b.z); v.w = lerp(a.w, b.w);
      v.r = lerp(a.r, b.r); v.g = lerp(a.g, b.g); v.b = lerp(a.b, b.b);
      v.s = lerp(a.s, b.s); v.t = lerp(a.t, b.t);
      out[m++] = v;
    }
    if (m >= 18) break;
  }
  return m;
}
}  // namespace

void Gpu3D::submit(const Vertex* vin, int n) {
  std::vector<Vertex>& vl = verts_[cur_];
  std::vector<Polygon>& pl = polys_[cur_];
  if (pl.size() >= 2048) return;
  const uint32_t attr = polyAttr_;
  // frente/verso pelo determinante em coordenadas homogêneas
  const Vertex &a = vin[0], &b = vin[1], &c = vin[2];
  const double det = double(a.x) * (double(b.y) * c.w - double(c.y) * b.w) - double(a.y) * (double(b.x) * c.w - double(c.x) * b.w) +
                     double(a.w) * (double(b.x) * c.y - double(c.x) * b.y);
  const bool front = det >= 0;
  if (front && !(attr & 0x80)) return;
  if (!front && !(attr & 0x40)) return;
  // recorte nos 6 planos
  Vertex bufA[20], bufB[20];
  int m = n;
  for (int i = 0; i < n; i++) bufA[i] = vin[i];
  bool crossesFar = false;
  for (int i = 0; i < n; i++) if (vin[i].z > vin[i].w) crossesFar = true;
  if (crossesFar && !(attr & 0x1000)) return;
  Vertex* src = bufA;
  Vertex* dst = bufB;
  for (int k = 0; k < 3 && m >= 3; k++)
    for (int sgn = -1; sgn <= 1 && m >= 3; sgn += 2) {
      m = clipPlane(src, m, dst, k, sgn);
      std::swap(src, dst);
    }
  if (m < 3) return;
  if (m > 10) m = 10;
  if (vl.size() + m > 6144) return;
  Polygon p;
  p.attr = attr; p.texParam = texParam_; p.palBase = palBase_;
  p.nv = static_cast<uint8_t>(m);
  const int vw = vpX2_ - vpX1_ + 1, vh = vpY2_ - vpY1_ + 1;
  p.ymin = 1e9f; p.ymax = -1e9f;
  for (int i = 0; i < m; i++) {
    Vertex v = src[i];
    const double w = v.w ? double(v.w) : 1.0;
    v.fx = static_cast<float>(vpX1_ + (double(v.x) + w) * vw / (2 * w));
    v.fy = static_cast<float>((191 - vpY2_) + (w - double(v.y)) * vh / (2 * w));
    v.fz = static_cast<float>(double(v.z) / w);  // -1..1
    v.fw = static_cast<float>(w / 4096.0);
    p.ymin = std::min(p.ymin, v.fy);
    p.ymax = std::max(p.ymax, v.fy);
    p.vtx[i] = static_cast<uint16_t>(vl.size());
    vl.push_back(v);
  }
  const int alphaP = (attr >> 16) & 31;
  const int fmt = (texParam_ >> 26) & 7;
  p.translucent = (alphaP > 0 && alphaP < 31) || fmt == 1 || fmt == 6;
  pl.push_back(p);
}

// ------------------------------------------------------------------ texturas
uint8_t Gpu3D::texByte(uint32_t a) const {
  const int32_t p = s.pgTex[(a >> 14) & 31];
  return p < 0 ? 0 : s.vram[p + (a & 0x3FFF)];
}
uint16_t Gpu3D::palHalf(uint32_t a) const {
  const int32_t p = s.pgTexPal[(a >> 14) & 7];
  if (p < 0) return 0;
  const uint8_t* q = &s.vram[p + (a & 0x3FFE)];
  return uint16_t(q[0] | (q[1] << 8));
}

// devolve a cor (BGR555) e o alfa (0..31) do texel
uint32_t Gpu3D::texel(uint32_t tp, uint32_t palBase, int ss, int tt, int& a) {
  const int w = 8 << ((tp >> 20) & 7), h = 8 << ((tp >> 23) & 7);
  auto wrapc = [](int v, int size, bool rep, bool flip) {
    if (!rep) return v < 0 ? 0 : v >= size ? size - 1 : v;
    if (flip) { const int per = v & (size * 2 - 1); return per >= size ? size * 2 - 1 - per : per; }
    return v & (size - 1);
  };
  const int x = wrapc(ss >> 4, w, tp & (1u << 16), tp & (1u << 18));
  const int y = wrapc(tt >> 4, h, tp & (1u << 17), tp & (1u << 19));
  const uint32_t base = (tp & 0xFFFF) << 3;
  const int fmt = (tp >> 26) & 7;
  const bool zero = tp & (1u << 29);
  const uint32_t i = uint32_t(y) * w + x;
  a = 31;
  switch (fmt) {
    case 1: {  // A3I5
      const uint8_t b = texByte(base + i);
      const int a3 = b >> 5;
      a = a3 * 4 + a3 / 2;
      return palHalf(palBase * 16 + (b & 31) * 2) & 0x7FFF;
    }
    case 2: {  // 4 cores
      const int idx = (texByte(base + i / 4) >> ((i & 3) * 2)) & 3;
      if (zero && !idx) { a = 0; return 0; }
      return palHalf(palBase * 8 + idx * 2) & 0x7FFF;
    }
    case 3: {  // 16 cores
      const int idx = (texByte(base + i / 2) >> ((i & 1) * 4)) & 15;
      if (zero && !idx) { a = 0; return 0; }
      return palHalf(palBase * 16 + idx * 2) & 0x7FFF;
    }
    case 4: {  // 256 cores
      const int idx = texByte(base + i);
      if (zero && !idx) { a = 0; return 0; }
      return palHalf(palBase * 16 + idx * 2) & 0x7FFF;
    }
    case 5: {  // 4x4 comprimido
      const uint32_t blk = uint32_t(y >> 2) * (w >> 2) + (x >> 2);
      const uint32_t addr = base + blk * 4;
      const uint32_t word = texByte(addr + (y & 3));
      const int idx = (word >> ((x & 3) * 2)) & 3;
      const uint32_t slot = addr & 0x60000;
      const uint32_t infoAddr = (slot == 0x40000 ? 0x30000u : 0x20000u) + ((addr & 0x1FFFF) >> 1);
      const uint16_t info = uint16_t(texByte(infoAddr) | (texByte(infoAddr + 1) << 8));
      const uint32_t pa = palBase * 16 + (info & 0x3FFF) * 4;
      const int mode = info >> 14;
      auto pc = [&](int k) { return palHalf(pa + k * 2) & 0x7FFF; };
      auto mix = [](uint16_t c0, uint16_t c1, int w0, int w1, int div) {
        const int r = ((c0 & 31) * w0 + (c1 & 31) * w1) / div;
        const int g = (((c0 >> 5) & 31) * w0 + ((c1 >> 5) & 31) * w1) / div;
        const int b = (((c0 >> 10) & 31) * w0 + ((c1 >> 10) & 31) * w1) / div;
        return uint32_t(r | (g << 5) | (b << 10));
      };
      switch (mode) {
        case 0: if (idx == 3) { a = 0; return 0; } return pc(idx);
        case 1:
          if (idx == 3) { a = 0; return 0; }
          if (idx == 2) return mix(pc(0), pc(1), 1, 1, 2);
          return pc(idx);
        case 2: return pc(idx);
        default:
          if (idx == 2) return mix(pc(0), pc(1), 5, 3, 8);
          if (idx == 3) return mix(pc(0), pc(1), 3, 5, 8);
          return pc(idx);
      }
    }
    case 6: {  // A5I3
      const uint8_t b = texByte(base + i);
      a = b >> 3;
      return palHalf(palBase * 16 + (b & 7) * 2) & 0x7FFF;
    }
    case 7: {  // cor direta
      const uint16_t c = uint16_t(texByte(base + i * 2) | (texByte(base + i * 2 + 1) << 8));
      if (!(c & 0x8000)) { a = 0; return 0; }
      return c & 0x7FFF;
    }
    default: a = 31; return 0x7FFF;
  }
}

// ------------------------------------------------------------------ troca de listas e desenho
void Gpu3D::vblank() {
  if (!swapPending) return;
  swapPending = false;
  sortManual_ = swapParam & 1;
  wbuf_ = swapParam & 2;
  cur_ ^= 1;
  verts_[cur_].clear();
  polys_[cur_].clear();
  render();
  // comandos que esperavam a troca
  std::vector<uint64_t> q;
  q.swap(queued_);
  for (size_t i = 0; i < q.size(); i++) {
    if (swapPending) { queued_.insert(queued_.end(), q.begin() + static_cast<long>(i), q.end()); break; }
    const uint64_t e = q[i];
    if (e >> 32) portCmd(static_cast<int>((e >> 32) - 1), static_cast<uint32_t>(e));
    else fifoWord(static_cast<uint32_t>(e));
  }
}

void Gpu3D::render() {
  rendered++;
  const int N = SCREEN_W * SCREEN_H;
  depth_.assign(N, 0);
  polyId_.assign(N, 0);
  flags_.assign(N, 0);
  disp3dcnt = s.gpuA.reg[0x30];
  // limpeza: cor, alfa, profundidade e ID
  const uint32_t cc = regs[0x30] | (regs[0x31] << 8) | (regs[0x32] << 16) | (uint32_t(regs[0x33]) << 24);
  const uint16_t cd = uint16_t((regs[0x34] | (regs[0x35] << 8)) & 0x7FFF);
  const float clearDepth = float(cd * 0x200 + 0x1FF) / float(0x1000000);
  const uint8_t ca = (cc >> 16) & 31;
  const uint8_t cid = (cc >> 24) & 63;
  if (disp3dcnt & 0x4000) {
    // imagem de fundo: cor no slot 2 e profundidade no slot 3 da textura
    const uint16_t off = uint16_t(regs[0x36] | (regs[0x37] << 8));
    const int ox = off & 0xFF, oy = off >> 8;
    for (int y = 0; y < SCREEN_H; y++)
      for (int x = 0; x < SCREEN_W; x++) {
        const uint32_t a = uint32_t(((y + oy) & 255) * 256 + ((x + ox) & 255)) * 2;
        const uint16_t c = uint16_t(texByte(0x40000 + a) | (texByte(0x40001 + a) << 8));
        const uint16_t d = uint16_t(texByte(0x60000 + a) | (texByte(0x60001 + a) << 8));
        const int i = y * SCREEN_W + x;
        color[i] = (c & 0x7FFF) | 0x8000;
        alpha[i] = (c & 0x8000) ? 31 : 0;
        depth_[i] = float((d & 0x7FFF) * 0x200 + 0x1FF) / float(0x1000000);
        flags_[i] = (d & 0x8000) ? 1 : 0;
        polyId_[i] = cid;
      }
  } else {
    const uint16_t c = uint16_t((cc & 0x7FFF) | 0x8000);
    for (int i = 0; i < N; i++) { color[i] = c; alpha[i] = ca; depth_[i] = clearDepth; polyId_[i] = cid; flags_[i] = (cc & 0x8000) ? 1 : 0; }
  }
  const int rl = cur_ ^ 1;
  const std::vector<Polygon>& pl = polys_[rl];
  const std::vector<Vertex>& vl = verts_[rl];
  // opacos primeiro, depois translúcidos (ordenados por Y se a ordem não for manual)
  std::vector<int> order;
  order.reserve(pl.size());
  for (int i = 0; i < int(pl.size()); i++) if (!pl[i].translucent) order.push_back(i);
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return pl[a].ymax < pl[b].ymax || (pl[a].ymax == pl[b].ymax && pl[a].ymin < pl[b].ymin); });
  const size_t nOpaque = order.size();
  for (int i = 0; i < int(pl.size()); i++) if (pl[i].translucent) order.push_back(i);
  if (!sortManual_)
    std::stable_sort(order.begin() + static_cast<long>(nOpaque), order.end(), [&](int a, int b) { return pl[a].ymax < pl[b].ymax || (pl[a].ymax == pl[b].ymax && pl[a].ymin < pl[b].ymin); });
  for (int i : order) rasterize(pl[i], vl, i);
  finishFrame();
}

void Gpu3D::rasterize(const Polygon& p, const std::vector<Vertex>& vs, int) {
  const int n = p.nv;
  if (n < 3) return;
  const Vertex* v[10];
  for (int i = 0; i < n; i++) v[i] = &vs[p.vtx[i]];
  const uint32_t attr = p.attr;
  const int mode = (attr >> 4) & 3;
  const int polyAlpha = (attr >> 16) & 31;
  const bool wire = polyAlpha == 0;
  const int alphaV = wire ? 31 : polyAlpha;
  const int id = (attr >> 24) & 63;
  const bool depthEq = attr & 0x4000;
  const bool fog = attr & 0x8000;
  const bool texOn = (disp3dcnt & 1) && ((p.texParam >> 26) & 7);
  const bool highlight = disp3dcnt & 2;
  const bool alphaTest = disp3dcnt & 4;
  const bool blending = disp3dcnt & 8;
  const int alphaRef = regs[0x20] & 31;
  const bool updDepthTrans = attr & 0x800;
  int y0 = static_cast<int>(std::ceil(p.ymin - 0.5f)), y1 = static_cast<int>(std::ceil(p.ymax - 0.5f));
  if (y0 < 0) y0 = 0;
  if (y1 > SCREEN_H) y1 = SCREEN_H;
  if (y1 <= y0) y1 = std::min(y0 + 1, SCREEN_H);  // polígonos de uma linha ainda aparecem
  for (int y = y0; y < y1; y++) {
    const float fy = y + 0.5f;
    // interseção da linha com as arestas (polígono convexo)
    float xl = 1e9f, xr = -1e9f;
    float al[7] = {}, ar[7] = {};  // 1/w, z, r/w, g/w, b/w, s/w, t/w
    bool any = false;
    for (int i = 0; i < n; i++) {
      const Vertex* a = v[i];
      const Vertex* b = v[(i + 1) % n];
      float ya = a->fy, yb = b->fy;
      if (ya == yb) continue;
      const Vertex* top = ya < yb ? a : b;
      const Vertex* bot = ya < yb ? b : a;
      if (fy < top->fy || fy >= bot->fy) {
        if (!(y == y0 && y1 == y0 + 1)) continue;
      }
      float t = (fy - top->fy) / (bot->fy - top->fy);
      t = std::clamp(t, 0.0f, 1.0f);
      const float x = top->fx + (bot->fx - top->fx) * t;
      const float iwT = 1.0f / top->fw, iwB = 1.0f / bot->fw;
      float at[7];
      at[0] = iwT + (iwB - iwT) * t;
      at[1] = top->fz + (bot->fz - top->fz) * t;
      at[2] = top->r * iwT + (bot->r * iwB - top->r * iwT) * t;
      at[3] = top->g * iwT + (bot->g * iwB - top->g * iwT) * t;
      at[4] = top->b * iwT + (bot->b * iwB - top->b * iwT) * t;
      at[5] = top->s * iwT + (bot->s * iwB - top->s * iwT) * t;
      at[6] = top->t * iwT + (bot->t * iwB - top->t * iwT) * t;
      if (x < xl) { xl = x; std::memcpy(al, at, sizeof at); }
      if (x > xr) { xr = x; std::memcpy(ar, at, sizeof at); }
      any = true;
    }
    if (!any) continue;
    int x0 = static_cast<int>(std::ceil(xl - 0.5f)), x1 = static_cast<int>(std::ceil(xr - 0.5f));
    if (x1 <= x0) x1 = x0 + 1;
    const float span = xr - xl;
    const int xs = std::max(x0, 0), xe = std::min(x1, SCREEN_W);
    for (int x = xs; x < xe; x++) {
      const bool edge = x == x0 || x == x1 - 1 || y == y0 || y == y1 - 1;
      if (wire && !edge) continue;
      float t = span > 0 ? ((x + 0.5f) - xl) / span : 0.0f;
      t = std::clamp(t, 0.0f, 1.0f);
      const float iw = al[0] + (ar[0] - al[0]) * t;
      const float z = al[1] + (ar[1] - al[1]) * t;
      const float wv = 1.0f / iw;
      const float depth = wbuf_ ? wv * (1.0f / 64.0f) : (z * 0.5f + 0.5f);
      const int idx = y * SCREEN_W + x;
      const float dd = depth_[idx];
      const bool pass = depthEq ? std::fabs(depth - dd) <= (wbuf_ ? 0.01f : 0.00003f) : depth < dd;
      if (!pass) continue;
      int vr = static_cast<int>((al[2] + (ar[2] - al[2]) * t) * wv) >> 3;  // 0..62 (6 bits)
      int vg = static_cast<int>((al[3] + (ar[3] - al[3]) * t) * wv) >> 3;
      int vb = static_cast<int>((al[4] + (ar[4] - al[4]) * t) * wv) >> 3;
      vr = std::clamp(vr, 0, 63); vg = std::clamp(vg, 0, 63); vb = std::clamp(vb, 0, 63);
      int r = vr, g = vg, b = vb, a = alphaV;
      if (texOn || mode == 2) {
        int tr = 63, tg = 63, tb = 63, ta = 31;
        if (texOn) {
          const int ss = static_cast<int>(std::floor((al[5] + (ar[5] - al[5]) * t) * wv));
          const int tt = static_cast<int>(std::floor((al[6] + (ar[6] - al[6]) * t) * wv));
          const uint32_t tc = texel(p.texParam, p.palBase, ss, tt, ta);
          tr = ((tc & 31) << 1) | ((tc & 31) ? 1 : 0);
          tg = (((tc >> 5) & 31) << 1) | (((tc >> 5) & 31) ? 1 : 0);
          tb = (((tc >> 10) & 31) << 1) | (((tc >> 10) & 31) ? 1 : 0);
        }
        if (mode == 1 && texOn) {  // decalque
          if (ta == 0) { r = vr; g = vg; b = vb; }
          else if (ta == 31) { r = tr; g = tg; b = tb; }
          else { r = (tr * ta + vr * (31 - ta)) >> 5; g = (tg * ta + vg * (31 - ta)) >> 5; b = (tb * ta + vb * (31 - ta)) >> 5; }
          a = alphaV;
        } else if (mode == 2) {  // toon / destaque
          const int ti = (vr >> 1) & 31;
          const uint16_t tcol = uint16_t(regs[0x60 + ti * 2] | (regs[0x61 + ti * 2] << 8));
          const int sr = (tcol & 31) << 1, sg = ((tcol >> 5) & 31) << 1, sb = ((tcol >> 10) & 31) << 1;
          const int br = highlight ? vr : sr, bg = highlight ? vr : sg, bb = highlight ? vr : sb;
          r = ((tr + 1) * (br + 1) - 1) >> 6; g = ((tg + 1) * (bg + 1) - 1) >> 6; b = ((tb + 1) * (bb + 1) - 1) >> 6;
          if (highlight) { r = std::min(63, r + sr); g = std::min(63, g + sg); b = std::min(63, b + sb); }
          a = ((ta + 1) * (alphaV + 1) - 1) >> 5;
        } else {  // modular (e sombra, aproximada)
          r = ((tr + 1) * (vr + 1) - 1) >> 6; g = ((tg + 1) * (vg + 1) - 1) >> 6; b = ((tb + 1) * (vb + 1) - 1) >> 6;
          a = ((ta + 1) * (alphaV + 1) - 1) >> 5;
        }
      }
      if (a == 0) continue;
      if (alphaTest && a <= alphaRef) continue;
      const bool trans = a < 31;
      if (trans && (flags_[idx] & 2) && polyId_[idx] == id) continue;  // mesmo ID translúcido não se sobrepõe
      if (trans && blending && (color[idx] & 0x8000) && alpha[idx] > 0) {
        const int dr = (color[idx] & 31) << 1, dg = ((color[idx] >> 5) & 31) << 1, db = ((color[idx] >> 10) & 31) << 1;
        r = (r * (a + 1) + dr * (31 - a)) >> 5;
        g = (g * (a + 1) + dg * (31 - a)) >> 5;
        b = (b * (a + 1) + db * (31 - a)) >> 5;
        alpha[idx] = static_cast<uint8_t>(std::max<int>(a, alpha[idx]));
      } else alpha[idx] = static_cast<uint8_t>(a);
      color[idx] = rgb6to5(std::min(r, 63), std::min(g, 63), std::min(b, 63)) | 0x8000;
      if (!trans || updDepthTrans) depth_[idx] = depth;
      if (trans) { flags_[idx] = static_cast<uint8_t>((flags_[idx] & 1) | 2); polyId_[idx] = static_cast<uint8_t>(id); }
      else {
        polyId_[idx] = static_cast<uint8_t>(id);
        flags_[idx] = static_cast<uint8_t>((fog ? 1 : 0) | (edge ? 4 : 0));
      }
      if (trans && !fog) flags_[idx] &= ~1;
    }
  }
}

void Gpu3D::finishFrame() {
  // marcação de bordas: pixel opaco na borda de um polígono, com vizinho de outro ID mais ao fundo
  if (disp3dcnt & 0x20) {
    std::vector<uint16_t> src(color, color + SCREEN_W * SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++)
      for (int x = 0; x < SCREEN_W; x++) {
        const int i = y * SCREEN_W + x;
        if (!(flags_[i] & 4)) continue;
        const uint8_t id = polyId_[i];
        bool mark = false;
        const int nb[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (auto& d : nb) {
          const int nx = x + d[0], ny = y + d[1];
          if (nx < 0 || ny < 0 || nx >= SCREEN_W || ny >= SCREEN_H) continue;
          const int j = ny * SCREEN_W + nx;
          if (polyId_[j] != id && depth_[i] < depth_[j]) { mark = true; break; }
        }
        if (mark) {
          const int k = (id >> 3) * 2;
          color[i] = uint16_t(((regs[0x10 + k] | (regs[0x11 + k] << 8)) & 0x7FFF) | 0x8000);
        }
      }
  }
  // névoa
  if (disp3dcnt & 0x80) {
    const bool alphaOnly = disp3dcnt & 0x40;
    const int shift = (disp3dcnt >> 8) & 15;
    const uint32_t fc = regs[0x38] | (regs[0x39] << 8) | (regs[0x3A] << 16) | (uint32_t(regs[0x3B]) << 24);
    const int fr = fc & 31, fg = (fc >> 5) & 31, fbl = (fc >> 10) & 31, fa = (fc >> 16) & 31;
    const int offset = (regs[0x3C] | (regs[0x3D] << 8)) & 0x7FFF;
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
      if (!(flags_[i] & 1)) continue;
      const int z = static_cast<int>(depth_[i] * 0x1000000) >> 9;  // 15 bits como o hardware
      const int step = 0x400 >> shift;
      int d;
      if (z < offset) d = regs[0x40] & 127;
      else {
        const int pos = step ? (z - offset) / std::max(step, 1) : 31;
        if (pos >= 31) d = regs[0x40 + 31] & 127;
        else {
          const int frac = step ? ((z - offset) % step) : 0;
          const int d0 = regs[0x40 + pos] & 127, d1 = regs[0x40 + pos + 1] & 127;
          d = step ? d0 + (d1 - d0) * frac / step : d0;
        }
      }
      if (d >= 127) d = 128;
      const uint16_t c = color[i];
      if (!alphaOnly) {
        const int r = ((c & 31) * (128 - d) + fr * d) >> 7, g = (((c >> 5) & 31) * (128 - d) + fg * d) >> 7;
        const int b = (((c >> 10) & 31) * (128 - d) + fbl * d) >> 7;
        color[i] = uint16_t(r | (g << 5) | (b << 10) | (c & 0x8000));
      }
      alpha[i] = static_cast<uint8_t>((alpha[i] * (128 - d) + fa * d) >> 7);
    }
  }
}

void Gpu3D::sync(StateIO& io) {
  io.raw(color, sizeof color); io.raw(alpha, sizeof alpha);
  io.sync(disp3dcnt); io.raw(regs, sizeof regs); io.sync(swapPending); io.sync(swapParam); io.sync(regsGx_);
  io.sync(packed_); io.sync(need_); io.sync(got_); io.sync(cmd_); io.raw(par_, sizeof par_);
  io.sync(portNeed_); io.sync(portGot_); io.sync(portCmd_); io.raw(portPar_, sizeof portPar_);
  uint32_t nq = static_cast<uint32_t>(queued_.size());
  io.sync(nq);
  if (io.reading()) { if (nq > (1u << 22)) throw std::runtime_error("Estado salvo inválido (fila 3D)."); queued_.resize(nq); }
  if (nq) io.raw(queued_.data(), nq * sizeof(uint64_t));
  io.raw(proj_, sizeof proj_); io.raw(pos_, sizeof pos_); io.raw(vec_, sizeof vec_); io.raw(tex_, sizeof tex_);
  io.raw(projStack_, sizeof projStack_); io.raw(posStack_, sizeof posStack_); io.raw(vecStack_, sizeof vecStack_);
  io.raw(texStack_, sizeof texStack_);
  io.sync(projSp_); io.sync(posSp_); io.sync(texSp_); io.sync(mtxMode_); io.sync(stackErr_);
  io.sync(vx_); io.sync(vy_); io.sync(vz_); io.sync(cr_); io.sync(cg_); io.sync(cb_); io.sync(ts_); io.sync(tt_);
  io.sync(polyAttr_); io.sync(polyAttrPending_); io.sync(texParam_); io.sync(palBase_); io.sync(diffAmb_); io.sync(specEmi_);
  io.raw(shininess_, sizeof shininess_); io.raw(lightVec_, sizeof lightVec_); io.raw(lightColor_, sizeof lightColor_);
  io.sync(primType_); io.sync(inPrim_); io.raw(strip_, sizeof strip_); io.sync(stripN_); io.sync(stripCount_);
  io.sync(vpX1_); io.sync(vpY1_); io.sync(vpX2_); io.sync(vpY2_);
  io.raw(posResult_, sizeof posResult_); io.raw(vecResult_, sizeof vecResult_); io.sync(boxResult_);
  io.sync(cur_); io.sync(sortManual_); io.sync(wbuf_);
  if (io.reading()) cur_ &= 1;
  for (int k = 0; k < 2; k++) {
    uint32_t nv = static_cast<uint32_t>(verts_[k].size()), np = static_cast<uint32_t>(polys_[k].size());
    io.sync(nv); io.sync(np);
    if (io.reading()) {
      if (nv > 6144 + 20 || np > 2048) throw std::runtime_error("Estado salvo inválido (listas 3D).");
      verts_[k].resize(nv); polys_[k].resize(np);
    }
    if (nv) io.raw(verts_[k].data(), nv * sizeof(Vertex));
    if (np) io.raw(polys_[k].data(), np * sizeof(Polygon));
    if (io.reading()) {
      for (auto& p : polys_[k]) {
        if (p.nv > 10) p.nv = 0;
        for (int i = 0; i < p.nv; i++) if (p.vtx[i] >= nv) { p.nv = 0; break; }
        // NaN/infinito viram polígono vazio; valores enormes são limitados (float→int sem estouro)
        if (!std::isfinite(p.ymin) || !std::isfinite(p.ymax)) p.nv = 0;
        p.ymin = std::isfinite(p.ymin) ? std::clamp(p.ymin, -1e6f, 1e6f) : 0.0f;
        p.ymax = std::isfinite(p.ymax) ? std::clamp(p.ymax, -1e6f, 1e6f) : 0.0f;
      }
      for (auto& v : verts_[k]) {
        if (!std::isfinite(v.fx) || !std::isfinite(v.fy) || !std::isfinite(v.fz) || !std::isfinite(v.fw) ||
            std::fabs(v.fw) < 1e-6f) { v.fx = v.fy = v.fz = 0; v.fw = 1; }
        v.fx = std::clamp(v.fx, -1e6f, 1e6f); v.fy = std::clamp(v.fy, -1e6f, 1e6f);
        v.fz = std::clamp(v.fz, -1e6f, 1e6f); v.fw = std::clamp(v.fw, -1e6f, 1e6f);
      }
    }
  }
  if (io.reading()) {
    // o próximo parâmetro vai em par_[got_]: got_ < need_ <= 32 (senão escreveria fora do vetor)
    clampTo(need_, 0, 32); clampTo(got_, 0, 31); clampTo(portNeed_, 0, 32); clampTo(portGot_, 0, 31);
    if (got_ >= need_) got_ = 0;
    if (portGot_ >= portNeed_) portGot_ = 0;
    clampTo(projSp_, 0, 1); clampTo(posSp_, 0, 31); clampTo(texSp_, 0, 1); mtxMode_ &= 3;
    primType_ &= 3; cmd_ &= 0x7F;
    // triângulos (tipos 0 e 2) fecham com 3 vértices: nunca ficam 3 guardados entre um vértice e outro
    clampTo(stripN_, 0, (primType_ & 1) ? 3 : 2);
    clipDirty_ = true;
  }
}

}  // namespace indigo::nds
