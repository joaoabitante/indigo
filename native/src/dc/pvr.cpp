// PowerVR2 CLX2 do Dreamcast, em software.
//   Tile Accelerator: recebe da CPU (store queues ou DMA do canal 2) os parâmetros de 32/64 bytes: controle de
//   lista (fim de lista, recorte do usuário), parâmetros globais de polígono/sprite/modificador e os vértices em
//   tiras. Os triângulos ficam guardados por contexto (endereço PARAM_BASE), sem montar as listas de objetos na
//   VRAM: o STARTRENDER desenha o contexto pedido.
//   Desenho: buffer de profundidade em ponto flutuante (1/w, maior = mais perto) com os 8 modos de comparação,
//   corte de faces, Gouraud ou cor chapada, cor de deslocamento, texturas (1555, 565, 4444, YUV422, paletas de 4
//   e 8 bits, twiddled, VQ, com stride e mipmaps: usa o maior nível), filtro pontual ou bilinear, as 4 instruções
//   de sombreamento, punch-through (PT_ALPHA_REF), translúcido com os fatores de mistura e ordenado de trás para
//   frente por triângulo (aproximação da ordenação por pixel do hardware), plano de fundo (ISP_BACKGND_T/D) e a
//   gravação na VRAM no formato do FB_W_CTRL (com a redução horizontal do SCALER_CTL).
//   Volumes modificadores: só a sombra simples da lista opaca (FPU_SHAD_SCALE); o modo de dois parâmetros não.
//   Não emulados: neblina, a matriz de regiões (cada render desenha a tela toda)
//   e os buffers acumuladores secundários.
//   Saída de vídeo: lê a VRAM pelo FB_R_SOF1/2, FB_R_SIZE e FB_R_CTRL (entrelaçado e linha dobrada).
#include <algorithm>
#include <cmath>

#include "dc.h"

namespace indigo::dc {

namespace {
enum : uint32_t {
  ID = 0x000, REVISION = 0x004, SOFTRESET = 0x008, STARTRENDER = 0x014, PARAM_BASE = 0x020, REGION_BASE = 0x02C,
  VO_BORDER_COL = 0x040, FB_R_CTRL = 0x044, FB_W_CTRL = 0x048, FB_W_LINESTRIDE = 0x04C, FB_R_SOF1 = 0x050,
  FB_R_SOF2 = 0x054, FB_R_SIZE = 0x05C, FB_W_SOF1 = 0x060, FB_W_SOF2 = 0x064, FB_X_CLIP = 0x068, FB_Y_CLIP = 0x06C,
  FPU_CULL_VAL = 0x078, ISP_BACKGND_D = 0x088, ISP_BACKGND_T = 0x08C, ISP_FEED_CFG = 0x098, SPG_HBLANK_INT = 0x0C8,
  SPG_VBLANK_INT = 0x0CC, SPG_CONTROL = 0x0D0, SPG_LOAD = 0x0D8, SPG_VBLANK = 0x0DC, TEXT_CONTROL = 0x0E4,
  VO_CONTROL = 0x0E8, SCALER_CTL = 0x0F4, PAL_RAM_CTRL = 0x108, SPG_STATUS = 0x10C, PT_ALPHA_REF = 0x11C,
  TA_OL_BASE = 0x124, TA_ISP_BASE = 0x128, TA_NEXT_OPB = 0x134, TA_ITP_CURRENT = 0x138, TA_GLOB_TILE_CLIP = 0x13C,
  TA_LIST_INIT = 0x144, TA_LIST_CONT = 0x160, TA_NEXT_OPB_INIT = 0x164,
};
inline float F(uint32_t x) { float f; std::memcpy(&f, &x, 4); return f; }
inline uint32_t c8(float f) { const float v = f * 255.0f + 0.5f; return !(v > 0) ? 0 : v >= 255 ? 255 : uint32_t(v); }
inline uint32_t argbF(float a, float r, float g, float b) { return c8(a) << 24 | c8(r) << 16 | c8(g) << 8 | c8(b); }
inline uint32_t scaleCol(const float* face, float i) {
  return argbF(face[0], face[1] * i, face[2] * i, face[3] * i);
}
uint16_t twEven[1024];  // bits de i espalhados nas posições pares
const uint32_t mipPoint[11] = {0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558};
const uint32_t vqMip[11] = {0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556};
struct TwInit {
  TwInit() {
    for (int i = 0; i < 1024; i++) {
      int v = 0;
      for (int b = 0; b < 10; b++) if (i & (1 << b)) v |= 1 << (2 * b);
      twEven[i] = uint16_t(v);
    }
  }
} twInit;
inline uint32_t twiddle(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  const uint32_t mn = std::min(w, h);
  const uint32_t blk = (x / mn + y / mn) * mn * mn;
  x &= mn - 1; y &= mn - 1;
  return blk + (uint32_t(twEven[x]) << 1 | twEven[y]);
}
inline uint32_t conv16(uint16_t c, int fmt) {
  switch (fmt) {
    case 0: {  // 1555
      const uint32_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
      return ((c & 0x8000) ? 0xFF000000u : 0) | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
    }
    case 1: {  // 565
      const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
      return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
    }
    case 2: {  // 4444
      const uint32_t a = c >> 12, r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
      return (a * 17) << 24 | (r * 17) << 16 | (g * 17) << 8 | (b * 17);
    }
    default: return 0xFF808080u;  // bumpmap: cinza
  }
}
inline uint32_t yuv(int y, int u, int v) {
  const float yy = float(y), uu = float(u - 128), vv = float(v - 128);
  auto cl = [](float f) { return f < 0 ? 0u : f > 255 ? 255u : uint32_t(f); };
  return 0xFF000000u | cl(yy + 1.375f * vv) << 16 | cl(yy - 0.34375f * uu - 0.6875f * vv) << 8 | cl(yy + 1.71875f * uu);
}
// fator de mistura (0..256) para um canal: s = cor da fonte, d = destino
inline int factor(int f, int sc, int dc, int sa, int da, bool isSrc) {
  switch (f) {
    case 0: return 0;
    case 1: return 256;
    case 2: return isSrc ? dc + (dc >> 7) : sc + (sc >> 7);
    case 3: return 256 - (isSrc ? dc + (dc >> 7) : sc + (sc >> 7));
    case 4: return sa + (sa >> 7);
    case 5: return 256 - (sa + (sa >> 7));
    case 6: return da + (da >> 7);
    default: return 256 - (da + (da >> 7));
  }
}
}  // namespace

Pvr::Pvr(System& s) : sys(s) {}

void Pvr::reset() {
  std::fill(std::begin(regs), std::end(regs), 0u);
  regs[ID / 4] = 0x17FD11DB;
  regs[REVISION / 4] = 0x11;
  regs[SPG_LOAD / 4] = (524u << 16) | 857u;
  regs[SPG_VBLANK / 4] = (21u << 16) | 260u;   // fim e começo do apagamento vertical (linhas)
  regs[SPG_VBLANK_INT / 4] = (21u << 16) | 260u;
  regs[FB_R_SIZE / 4] = (479u << 10) | 319u | (1u << 20);
  for (auto& c : ctx_) { c.clear(); c.base = 0xFFFFFFFF; }
  listType_ = -1; taHave_ = 0; taNeed_ = 0; stripN_ = 0; curPoly_ = -1; curCtx_ = 0; taWordCnt_ = 0;
}

uint32_t Pvr::read(uint32_t off) {
  off &= 0x1FFC;
  if (off == 0x150) return yuvCount_;
  if (off == SPG_STATUS) {
    const uint32_t l = uint32_t(sys.line);
    const uint32_t vb = regs[SPG_VBLANK / 4], vbs = vb & 0x3FF, vbe = (vb >> 16) & 0x3FF;
    const bool blank = l >= vbs || l < vbe;
    return (l & 0x3FF) | ((sys.frames & 1) ? 0x400u : 0) | (blank ? 0x2800u : 0);
  }
  return regs[off / 4];
}

int Pvr::ctxFor(uint32_t base, bool create) {
  base &= 0xF00000;
  for (int i = 0; i < 4; i++) if (ctx_[i].base == base) return i;
  if (!create) return -1;
  // reaproveita o mais antigo (rodízio)
  static_assert(sizeof(ctx_) / sizeof(ctx_[0]) == 4);
  int k = 0;
  for (int i = 0; i < 4; i++) if (ctx_[i].base == 0xFFFFFFFF) { k = i; goto found; }
  k = (curCtx_ + 1) & 3;
found:
  ctx_[k].clear();
  ctx_[k].base = base;
  return k;
}

void Pvr::write(uint32_t off, uint32_t v) {
  off &= 0x1FFC;
  switch (off) {
    case ID: case REVISION: case SPG_STATUS: return;
    case SOFTRESET:
      if (v & 1) { listType_ = -1; taHave_ = 0; stripN_ = 0; }
      return;
    case STARTRENDER:
      render();
      sys.schedule(System::EV_RENDER, 200000);
      return;
    case TA_LIST_INIT:
      if (v & 0x80000000u) {
        curCtx_ = ctxFor(regs[TA_ISP_BASE / 4], true);
        ctx_[curCtx_].clear();
        regs[TA_ITP_CURRENT / 4] = regs[TA_ISP_BASE / 4];
        regs[TA_NEXT_OPB / 4] = regs[TA_NEXT_OPB_INIT / 4];
        listType_ = -1; taHave_ = 0; stripN_ = 0; curPoly_ = -1; taWordCnt_ = 0;
      }
      return;
    case 0x148:  // TA_YUV_TEX_BASE: recomeça a imagem
      regs[off / 4] = v; yuvCount_ = 0; yuvHave_ = 0;
      return;
    case 0x150: return;  // TA_YUV_TEX_CNT (só leitura)
    case TA_LIST_CONT:
      if (v & 0x80000000u) { listType_ = -1; taHave_ = 0; stripN_ = 0; }
      return;
    default: regs[off / 4] = v; return;
  }
}

// ---------------------------------------------------------------- Tile Accelerator
void Pvr::taWriteWords(uint32_t a, uint32_t v) {
  taWordBuf_[taWordCnt_++] = v;
  if (taWordCnt_ == 8) {
    taWordCnt_ = 0;
    if (a & 0x00800000) yuvWrite(taWordBuf_); else taWrite(taWordBuf_);
  }
}

// conversor YUV do TA (0x10800000): macroblocos de 16x16 em YUV420 (U 8x8, V 8x8 e Y em quatro blocos 8x8:
// cima-esquerda, cima-direita, baixo-esquerda, baixo-direita = 384 bytes) ou YUV422 (512 bytes) viram uma
// textura YUV422 linear em TA_YUV_TEX_BASE; terminada a imagem, liga o bit 6 do ISTNRM.
void Pvr::yuvWrite(const uint32_t* d) {
  const uint32_t ctrl = regs[0x14C / 4];
  const bool in422 = ctrl & 0x01000000u;
  const uint32_t need = in422 ? 512 : 384;
  std::memcpy(yuvBuf_ + yuvHave_, d, 32);
  yuvHave_ += 32;
  if (yuvHave_ < need) return;
  yuvHave_ = 0;
  const uint32_t mw = (ctrl & 0x3F) + 1, mh = ((ctrl >> 8) & 0x3F) + 1;
  const uint32_t mx = yuvCount_ % mw, my = yuvCount_ / mw;
  const uint32_t base = regs[0x148 / 4] & 0x7FFFF8, pitch = mw * 16 * 2;
  uint8_t* vr = sys.vram.data();
  for (uint32_t y = 0; y < 16; y++)
    for (uint32_t x = 0; x < 16; x += 2) {
      uint8_t u, v, y0, y1;
      if (!in422) {
        u = yuvBuf_[(y / 2) * 8 + x / 2];
        v = yuvBuf_[64 + (y / 2) * 8 + x / 2];
        const uint32_t blk = (y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0);
        y0 = yuvBuf_[128 + blk * 64 + (y & 7) * 8 + (x & 7)];
        y1 = yuvBuf_[128 + blk * 64 + (y & 7) * 8 + (x & 7) + 1];
      } else {
        // YUV422: U 8x16, V 8x16, Y em quatro blocos 8x8
        u = yuvBuf_[y * 8 + x / 2];
        v = yuvBuf_[128 + y * 8 + x / 2];
        const uint32_t blk = (y >= 8 ? 2 : 0) + (x >= 8 ? 1 : 0);
        y0 = yuvBuf_[256 + blk * 64 + (y & 7) * 8 + (x & 7)];
        y1 = yuvBuf_[256 + blk * 64 + (y & 7) * 8 + (x & 7) + 1];
      }
      const uint32_t a = (base + (my * 16 + y) * pitch + (mx * 16 + x) * 2) & 0x7FFFFC;
      vr[a] = u; vr[a + 1] = y0; vr[a + 2] = v; vr[a + 3] = y1;
    }
  if (++yuvCount_ >= mw * mh) { yuvCount_ = 0; sys.raiseNrm(6); }
}

int Pvr::blockWords(uint32_t pcw) const {
  switch (pcw >> 29) {
    case 4: {
      const int lt = listType_ >= 0 ? listType_ : int((pcw >> 24) & 7);
      if (lt == 1 || lt == 3) return 8;
      const bool int1 = ((pcw >> 4) & 3) == 2;
      if (int1 && ((pcw & 0x40) || (pcw & 4))) return 16;
      return 8;
    }
    case 7:
      if (modVol_ || sprite_) return 16;
      return (vtxType_ == 5 || vtxType_ == 6 || (vtxType_ >= 11 && vtxType_ <= 14)) ? 16 : 8;
    default: return 8;
  }
}

void Pvr::taWrite(const uint32_t* d) {
  taBlocks++;
  if (taHave_ == 0) {
    std::memcpy(taBuf_, d, 32);
    taNeed_ = blockWords(d[0]);
    taHave_ = 8;
  } else {
    std::memcpy(taBuf_ + 8, d, 32);
    taHave_ = 16;
  }
  if (taHave_ >= taNeed_) { taHave_ = 0; taBlock(taBuf_); }
}

void Pvr::taBlock(const uint32_t* w) {
  const uint32_t pcw = w[0];
  switch (pcw >> 29) {
    case 0: {  // fim de lista
      static const int bit[5] = {7, 8, 9, 10, 21};
      if (listType_ >= 0 && listType_ < 5) sys.raiseNrm(bit[listType_]);
      listType_ = -1; stripN_ = 0; curPoly_ = -1;
      break;
    }
    case 1:  // recorte do usuário (em tiles de 32x32)
      userClip_[0] = (w[4] & 0x3F) * 32; userClip_[1] = (w[5] & 0xF) * 32;
      userClip_[2] = ((w[6] & 0x3F) + 1) * 32; userClip_[3] = ((w[7] & 0xF) + 1) * 32;
      break;
    case 2: break;  // conjunto de listas de objetos: não usado
    case 4: case 5: taGlobal(w); break;
    case 7: taVertex(w); break;
    default: break;
  }
}

void Pvr::newPoly() {
  TaContext& c = ctx_[curCtx_];
  Poly p{};
  p.pcw = curPcw_; p.isp = curIsp_; p.tsp = curTsp_; p.tcw = curTcw_;
  p.first = uint32_t(c.tris.size()); p.count = 0;
  const uint32_t uc = (curPcw_ >> 16) & 3;
  if (uc >= 2) {
    std::memcpy(p.clip, userClip_, sizeof p.clip);
    p.clipOut = uc == 3;
  } else {
    p.clip[0] = p.clip[1] = 0; p.clip[2] = p.clip[3] = 4096; p.clipOut = false;
  }
  if (listType_ >= 0 && listType_ < 5 && c.polys[listType_].size() < 200000) {
    c.polys[listType_].push_back(p);
    curPoly_ = int(c.polys[listType_].size()) - 1;
  } else {
    curPoly_ = -1;
  }
}

void Pvr::taGlobal(const uint32_t* w) {
  const uint32_t pcw = w[0];
  if (listType_ < 0) listType_ = int((pcw >> 24) & 7);
  if (listType_ > 4) listType_ = 0;
  stripN_ = 0;
  curPcw_ = pcw; curIsp_ = w[1]; curTsp_ = w[2]; curTcw_ = w[3];
  modVol_ = listType_ == 1 || listType_ == 3;
  sprite_ = (pcw >> 29) == 5;
  if (modVol_) { curPoly_ = -1; return; }
  if (sprite_) {
    // sprite: cor-base e de deslocamento empacotadas no parâmetro global
    faceCol_[0] = float(w[4] >> 24) / 255; faceCol_[1] = float((w[4] >> 16) & 0xFF) / 255;
    faceCol_[2] = float((w[4] >> 8) & 0xFF) / 255; faceCol_[3] = float(w[4] & 0xFF) / 255;
    faceOfs_[0] = float(w[5] >> 24) / 255; faceOfs_[1] = float((w[5] >> 16) & 0xFF) / 255;
    faceOfs_[2] = float((w[5] >> 8) & 0xFF) / 255; faceOfs_[3] = float(w[5] & 0xFF) / 255;
    vtxType_ = (pcw & 8) ? 16 : 15;
    newPoly();
    return;
  }
  const int col = (pcw >> 4) & 3;
  const bool tex = pcw & 8, uv16 = pcw & 1, vol = pcw & 0x40;
  if (col == 2) {  // intensidade modo 1: cor da face aqui
    const int o = (vol || (pcw & 4)) ? 8 : 4;
    for (int i = 0; i < 4; i++) faceCol_[i] = F(w[o + i]);
    if (!vol && (pcw & 4)) for (int i = 0; i < 4; i++) faceOfs_[i] = F(w[12 + i]);
  }
  if (!vol) {
    if (!tex) vtxType_ = col == 0 ? 0 : col == 1 ? 1 : 2;
    else if (col == 0) vtxType_ = uv16 ? 4 : 3;
    else if (col == 1) vtxType_ = uv16 ? 6 : 5;
    else vtxType_ = uv16 ? 8 : 7;
  } else {
    if (!tex) vtxType_ = col >= 2 ? 10 : 9;
    else if (col >= 2) vtxType_ = uv16 ? 14 : 13;
    else vtxType_ = uv16 ? 12 : 11;
  }
  newPoly();
}

void Pvr::addTri(const Vtx& a, const Vtx& b, const Vtx& c) {
  if (curPoly_ < 0) return;
  TaContext& cx = ctx_[curCtx_];
  if (cx.tris.size() >= 1000000) return;
  Tri t;
  t.v[0] = a; t.v[1] = b; t.v[2] = c;
  t.poly = uint32_t(curPoly_);
  cx.tris.push_back(t);
  cx.polys[listType_][size_t(curPoly_)].count++;
}

void Pvr::taVertex(const uint32_t* w) {
  if (listType_ < 0) return;
  if (modVol_) {
    // volume modificador: um triângulo por parâmetro (só os da lista opaca são usados)
    TaContext& c = ctx_[curCtx_];
    if (listType_ == 1 && c.mods.size() < 200000) {
      ModTri m;
      for (int i = 0; i < 3; i++) { m.x[i] = F(w[1 + 3 * i]); m.y[i] = F(w[2 + 3 * i]); m.z[i] = F(w[3 + 3 * i]); }
      // instrução do parâmetro global (1 ou 2 no último polígono do volume) e o fim de tira (bit 4)
      m.mode = ((curIsp_ >> 29) & 3) | ((w[0] & 0x10000000) ? 0x10 : 0);
      c.mods.push_back(m);
    }
    return;
  }
  const bool eos = w[0] & 0x10000000;
  if (sprite_) {
    Vtx v[4];
    for (int i = 0; i < 3; i++) { v[i].x = F(w[1 + 3 * i]); v[i].y = F(w[2 + 3 * i]); v[i].z = F(w[3 + 3 * i]); }
    v[3].x = F(w[10]); v[3].y = F(w[11]);
    // z e uv do quarto vértice pelo plano dos outros três (D = A + C - B)
    v[3].z = v[0].z + v[2].z - v[1].z;
    auto uv = [&](uint32_t x, float& u, float& vv) { u = F(x & 0xFFFF0000u); vv = F(x << 16); };
    uv(w[13], v[0].u, v[0].v); uv(w[14], v[1].u, v[1].v); uv(w[15], v[2].u, v[2].v);
    v[3].u = v[0].u + v[2].u - v[1].u; v[3].v = v[0].v + v[2].v - v[1].v;
    const uint32_t col = argbF(faceCol_[0], faceCol_[1], faceCol_[2], faceCol_[3]);
    const uint32_t ofs = argbF(faceOfs_[0], faceOfs_[1], faceOfs_[2], faceOfs_[3]);
    for (auto& x : v) { x.col = col; x.ofs = ofs; }
    addTri(v[0], v[1], v[2]);
    addTri(v[0], v[2], v[3]);
    stripN_ = 0;
    return;
  }
  Vtx v{};
  v.x = F(w[1]); v.y = F(w[2]); v.z = F(w[3]);
  v.ofs = 0;
  auto uv16 = [&](uint32_t x) { v.u = F(x & 0xFFFF0000u); v.v = F(x << 16); };
  auto fcol = [&](int o) { return argbF(F(w[o]), F(w[o + 1]), F(w[o + 2]), F(w[o + 3])); };
  switch (vtxType_) {
    case 0: v.col = w[6]; break;
    case 1: v.col = fcol(4); break;
    case 2: v.col = scaleCol(faceCol_, F(w[6])); break;
    case 3: v.u = F(w[4]); v.v = F(w[5]); v.col = w[6]; v.ofs = w[7]; break;
    case 4: uv16(w[4]); v.col = w[6]; v.ofs = w[7]; break;
    case 5: v.u = F(w[4]); v.v = F(w[5]); v.col = fcol(8); v.ofs = fcol(12); break;
    case 6: uv16(w[4]); v.col = fcol(8); v.ofs = fcol(12); break;
    case 7: v.u = F(w[4]); v.v = F(w[5]); v.col = scaleCol(faceCol_, F(w[6])); v.ofs = scaleCol(faceOfs_, F(w[7])); break;
    case 8: uv16(w[4]); v.col = scaleCol(faceCol_, F(w[6])); v.ofs = scaleCol(faceOfs_, F(w[7])); break;
    case 9: v.col = w[4]; break;
    case 10: v.col = scaleCol(faceCol_, F(w[4])); break;
    case 11: v.u = F(w[4]); v.v = F(w[5]); v.col = w[6]; v.ofs = w[7]; break;
    case 12: uv16(w[4]); v.col = w[6]; v.ofs = w[7]; break;
    case 13: v.u = F(w[4]); v.v = F(w[5]); v.col = scaleCol(faceCol_, F(w[6])); v.ofs = scaleCol(faceOfs_, F(w[7])); break;
    case 14: uv16(w[4]); v.col = scaleCol(faceCol_, F(w[6])); v.ofs = scaleCol(faceOfs_, F(w[7])); break;
    default: v.col = 0xFFFFFFFF; break;
  }
  if (stripN_ >= 2) {
    // tira: os triângulos ímpares trocam a ordem para manter o mesmo sentido
    if (stripParity_) addTri(strip_[1], strip_[0], v); else addTri(strip_[0], strip_[1], v);
    stripParity_ = !stripParity_;
    strip_[0] = strip_[1]; strip_[1] = v;
  } else {
    strip_[stripN_++] = v;
    stripParity_ = false;
  }
  if (eos) { stripN_ = 0; }
}

// ---------------------------------------------------------------- texturas
uint32_t Pvr::texel(const Poly& p, uint32_t tsp, uint32_t tcw, int u, int v) {
  const uint32_t w = 8u << ((tsp >> 3) & 7), h = 8u << (tsp & 7);
  // repetição, espelho e trava (clamp tem prioridade)
  auto wrap = [](int c, uint32_t size, bool clamp, bool flip) -> uint32_t {
    if (clamp) return uint32_t(std::clamp(c, 0, int(size) - 1));
    if (flip) {
      const uint32_t m = uint32_t(c) & (2 * size - 1);
      return m >= size ? 2 * size - 1 - m : m;
    }
    return uint32_t(c) & (size - 1);
  };
  const uint32_t x = wrap(u, w, tsp & 0x10000, tsp & 0x40000), y = wrap(v, h, tsp & 0x8000, tsp & 0x20000);
  (void)p;
  const int fmt = (tcw >> 27) & 7;
  const bool mip = tcw & 0x80000000u, vq = tcw & 0x40000000u;
  uint32_t base = (tcw & 0x1FFFFF) << 3;
  const uint8_t* vr = sys.vram.data();
  auto rd16 = [&](uint32_t a) { a &= 0x7FFFFE; return uint16_t(vr[a] | vr[a + 1] << 8); };
  int lg = 0;
  while ((1u << lg) < w) lg++;
  if (lg > 10) lg = 10;
  if (fmt == 5 || fmt == 6) {  // paletas (sempre twiddled)
    const uint32_t pal = (tcw >> 21) & 0x3F;
    const uint32_t ctrl = sys.pvr.regs[PAL_RAM_CTRL / 4] & 3;
    uint32_t idx;
    if (fmt == 5) {
      uint32_t t = twiddle(x, y, w, h);
      if (mip) t += mipPoint[lg];
      const uint8_t b = vr[(base + t / 2) & 0x7FFFFF];
      idx = (pal << 4) | ((t & 1) ? b >> 4 : b & 15);
    } else {
      uint32_t t = twiddle(x, y, w, h);
      if (mip) t += mipPoint[lg];
      idx = ((pal & 0x30) << 4) | vr[(base + t) & 0x7FFFFF];
    }
    const uint32_t e = regs[(0x1000 / 4) + (idx & 0x3FF)];
    if (ctrl == 3) return e;
    return conv16(uint16_t(e), int(ctrl));
  }
  uint16_t c;
  uint32_t t;
  if (vq) {
    const uint32_t code = vr[(base + 2048 + (mip ? vqMip[lg] : 0) + twiddle(x >> 1, y >> 1, std::max(w >> 1, 1u), std::max(h >> 1, 1u))) & 0x7FFFFF];
    t = (x & 1) << 1 | (y & 1);
    c = rd16(base + code * 8 + t * 2);
    if (fmt == 3) {
      // YUV em VQ: par de texels da mesma linha do código
      const uint16_t c0 = rd16(base + code * 8 + (t & 1) * 2), c1 = rd16(base + code * 8 + ((t & 1) | 2) * 2);
      const int yv = (t & 2) ? c1 >> 8 : c0 >> 8;
      return yuv(yv, c0 & 0xFF, c1 & 0xFF);
    }
    return conv16(c, fmt);
  }
  if (tcw & 0x04000000u) {  // não twiddled (linear), talvez com stride
    const uint32_t stride = (tcw & 0x02000000u) ? (sys.pvr.regs[TEXT_CONTROL / 4] & 31) * 32 : w;
    t = y * stride + x;
    if (fmt == 3) {
      const uint32_t pair = t & ~1u;
      const uint16_t c0 = rd16(base + pair * 2), c1 = rd16(base + pair * 2 + 2);
      return yuv((t & 1) ? c1 >> 8 : c0 >> 8, c0 & 0xFF, c1 & 0xFF);
    }
  } else {
    t = twiddle(x, y, w, h);
    if (mip) t += mipPoint[lg];
    if (fmt == 3) {
      const uint32_t pair = t & ~1u;
      const uint16_t c0 = rd16(base + pair * 2), c1 = rd16(base + pair * 2 + 2);
      return yuv((t & 1) ? c1 >> 8 : c0 >> 8, c0 & 0xFF, c1 & 0xFF);
    }
  }
  c = rd16(base + t * 2);
  return conv16(c, fmt);
}

// ---------------------------------------------------------------- desenho
void Pvr::drawBackground() {
  const uint32_t pb = regs[PARAM_BASE / 4] & 0xF00000;
  const uint32_t bt = regs[ISP_BACKGND_T / 4];
  const uint32_t a = pb + ((bt >> 3) & 0x1FFFFF) * 4;
  const uint32_t isp = sys.vramRead32(a);
  int ci = 3;  // palavra da cor no vértice (x, y, z, [u, v], cor)
  if (isp & 0x02000000u) ci += (isp & 0x00400000u) ? 1 : 2;
  uint32_t col = sys.vramRead32(a + 12 + uint32_t(ci) * 4);
  col |= 0xFF000000u;
  const float d = F(regs[ISP_BACKGND_D / 4]);
  // acumulador em ABGR (como a saída), a cor do TA vem em ARGB
  const uint32_t abgr = (col & 0xFF00FF00u) | (col >> 16 & 0xFF) | (col & 0xFF) << 16;
  std::fill(color_.begin(), color_.end(), abgr);
  std::fill(depth_.begin(), depth_.end(), d);
}

void Pvr::drawTri(const Tri& t, const Poly& p, int list) {
  const Vtx &a = t.v[0], &b = t.v[1], &c = t.v[2];
  float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
  const uint32_t cull = (p.isp >> 27) & 3;
  if (cull) {
    const float cv = std::fabs(F(regs[FPU_CULL_VAL / 4]));
    if (std::fabs(area) < cv) return;
    if (cull == 2 && area < 0) return;
    if (cull == 3 && area > 0) return;
  }
  if (area == 0 || !(area == area)) return;
  // caixa do triângulo, recortada
  // coordenadas fora do razoável (ou NaN) não viram inteiro: recorta antes
  auto ci = [](float f) { return !(f == f) ? 0 : f < -4096.0f ? -4096 : f > 8192.0f ? 8192 : int(std::floor(f)); };
  int x0 = ci(std::min({a.x, b.x, c.x})), x1 = ci(std::max({a.x, b.x, c.x})) + 1;
  int y0 = ci(std::min({a.y, b.y, c.y})), y1 = ci(std::max({a.y, b.y, c.y})) + 1;
  int cx0 = 0, cy0 = 0, cx1 = rw_, cy1 = rh_;
  if (!p.clipOut && p.clip[2] < 4096) {
    cx0 = std::max(cx0, int(p.clip[0])); cy0 = std::max(cy0, int(p.clip[1]));
    cx1 = std::min(cx1, int(p.clip[2])); cy1 = std::min(cy1, int(p.clip[3]));
  }
  x0 = std::max(x0, cx0); y0 = std::max(y0, cy0); x1 = std::min(x1, cx1); y1 = std::min(y1, cy1);
  if (x0 >= x1 || y0 >= y1) return;
  const float inv = 1.0f / area;
  const bool tex = p.pcw & 8, gouraud = p.pcw & 2, offset = p.pcw & 4;
  const uint32_t tsp = p.tsp, tcw = p.tcw;
  const int dmode = int(p.isp >> 29);
  const bool zwrite = !(p.isp & 0x04000000u) && list != 2;
  const int shading = int((tsp >> 6) & 3);
  const bool useAlpha = tsp & 0x00100000u, ignTexA = tsp & 0x00080000u;
  const float tw = float(8u << ((tsp >> 3) & 7)), th = float(8u << (tsp & 7));
  const bool bilinear = ((tsp >> 13) & 3) != 0;
  const int srcF = int(tsp >> 29), dstF = int((tsp >> 26) & 7);
  const uint32_t ptRef = regs[PT_ALPHA_REF / 4] & 0xFF;
  auto ch = [](uint32_t col, int s) { return float((col >> s) & 0xFF); };
  // coordenada de textura para inteiro (NaN e valores enormes viram algo dentro da faixa)
  auto ti = [](float f) { return !(f == f) ? 0 : f < -1e8f ? -100000000 : f > 1e8f ? 100000000 : int(std::floor(f)); };
  // cores em float para interpolar (A, R, G, B)
  float ca[3][4], co[3][4];
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 4; k++) { ca[i][k] = ch(t.v[i].col, 24 - 8 * k); co[i][k] = ch(t.v[i].ofs, 24 - 8 * k); }
  // funções de aresta: w0 para o vértice a (aresta b-c) etc.
  auto edge = [](const Vtx& p0, const Vtx& p1, float px, float py) {
    return (p1.x - p0.x) * (py - p0.y) - (p1.y - p0.y) * (px - p0.x);
  };
  for (int y = y0; y < y1; y++) {
    const float py = float(y) + 0.5f;
    for (int x = x0; x < x1; x++) {
      if (p.clipOut && x >= int(p.clip[0]) && x < int(p.clip[2]) && y >= int(p.clip[1]) && y < int(p.clip[3])) continue;
      const float px = float(x) + 0.5f;
      float w0 = edge(b, c, px, py) * inv, w1 = edge(c, a, px, py) * inv, w2 = edge(a, b, px, py) * inv;
      if (w0 < 0 || w1 < 0 || w2 < 0) {
        // regra de borda: aceita só o que é numericamente zero numa aresta
        if (w0 < -1e-6f || w1 < -1e-6f || w2 < -1e-6f) continue;
      }
      const float z = w0 * a.z + w1 * b.z + w2 * c.z;
      const size_t i = size_t(y) * size_t(rw_) + size_t(x);
      const float old = depth_[i];
      bool pass;
      switch (dmode) {
        case 0: pass = false; break;
        case 1: pass = z < old; break;
        case 2: pass = z == old; break;
        case 3: pass = z <= old; break;
        case 4: pass = z > old; break;
        case 5: pass = z != old; break;
        case 6: pass = z >= old; break;
        default: pass = true; break;
      }
      if (!pass) continue;
      // pesos com correção de perspectiva
      float q0 = w0 * a.z, q1 = w1 * b.z, q2 = w2 * c.z;
      const float qs = q0 + q1 + q2;
      if (qs != 0) { const float r = 1.0f / qs; q0 *= r; q1 *= r; q2 *= r; }
      float col[4], ofs[4];
      if (gouraud) {
        for (int k = 0; k < 4; k++) {
          col[k] = q0 * ca[0][k] + q1 * ca[1][k] + q2 * ca[2][k];
          ofs[k] = q0 * co[0][k] + q1 * co[1][k] + q2 * co[2][k];
        }
      } else {
        for (int k = 0; k < 4; k++) { col[k] = ca[2][k]; ofs[k] = co[2][k]; }
      }
      if (!useAlpha) col[0] = 255;
      float out[4];
      if (tex) {
        const float u = (q0 * a.u + q1 * b.u + q2 * c.u) * tw, v = (q0 * a.v + q1 * b.v + q2 * c.v) * th;
        float tc[4];
        if (bilinear) {
          const float uu = u - 0.5f, vv = v - 0.5f;
          const int iu = ti(uu), iv = ti(vv);
          const float fu = uu - float(iu), fv = vv - float(iv);
          const uint32_t t00 = texel(p, tsp, tcw, iu, iv), t10 = texel(p, tsp, tcw, iu + 1, iv);
          const uint32_t t01 = texel(p, tsp, tcw, iu, iv + 1), t11 = texel(p, tsp, tcw, iu + 1, iv + 1);
          for (int k = 0; k < 4; k++) {
            const int s = 24 - 8 * k;
            tc[k] = (ch(t00, s) * (1 - fu) + ch(t10, s) * fu) * (1 - fv) + (ch(t01, s) * (1 - fu) + ch(t11, s) * fu) * fv;
          }
        } else {
          const uint32_t tx = texel(p, tsp, tcw, ti(u), ti(v));
          for (int k = 0; k < 4; k++) tc[k] = ch(tx, 24 - 8 * k);
        }
        if (ignTexA) tc[0] = 255;
        switch (shading) {
          case 0: for (int k = 0; k < 4; k++) out[k] = tc[k]; break;  // decal
          case 1:  // modulate
            out[0] = tc[0];
            for (int k = 1; k < 4; k++) out[k] = tc[k] * col[k] / 255;
            break;
          case 2:  // decal alpha
            out[0] = col[0];
            for (int k = 1; k < 4; k++) out[k] = (tc[k] * tc[0] + col[k] * (255 - tc[0])) / 255;
            break;
          default: for (int k = 0; k < 4; k++) out[k] = tc[k] * col[k] / 255; break;  // modulate alpha
        }
        if (offset) for (int k = 1; k < 4; k++) out[k] += ofs[k];
      } else {
        for (int k = 0; k < 4; k++) out[k] = col[k];
      }
      auto cl = [](float f) { return !(f > 0) ? 0 : f >= 255.0f ? 255 : int(f); };
      const int sa = cl(out[0]), sr = cl(out[1]), sg = cl(out[2]), sb = cl(out[3]);
      if (list == 4 && uint32_t(sa) < ptRef) continue;  // punch-through
      uint32_t& dst = color_[i];
      if (list == 2) {
        const int dr = int(dst & 0xFF), dg = int((dst >> 8) & 0xFF), db = int((dst >> 16) & 0xFF), da = int(dst >> 24);
        auto mix = [&](int s, int d) {
          const int fs = factor(srcF, s, d, sa, da, true), fd = factor(dstF, s, d, sa, da, false);
          const int r = (s * fs + d * fd) >> 8;
          return r > 255 ? 255 : r;
        };
        const int fa = (sa * factor(srcF, sa, da, sa, da, true) + da * factor(dstF, sa, da, sa, da, false)) >> 8;
        dst = uint32_t(mix(sr, dr)) | uint32_t(mix(sg, dg)) << 8 | uint32_t(mix(sb, db)) << 16 | uint32_t(std::min(fa, 255)) << 24;
      } else {
        dst = uint32_t(sr) | uint32_t(sg) << 8 | uint32_t(sb) << 16 | uint32_t(sa) << 24;
      }
      if (zwrite) depth_[i] = z;
      if (list != 2) stencil_[i] = (p.pcw & 0x80) ? 1 : 0;
    }
  }
}

// sombra simples (FPU_SHAD_SCALE bit 8): cada volume fechado marca os pixels cuja superfície fica dentro dele
// (paridade das faces na frente da superfície); nos polígonos opacos com o bit de sombra, a cor é escalada.
void Pvr::applyShadows(const TaContext& c) {
  const uint32_t ss = regs[0x074 / 4];
  if (c.mods.empty() || !(ss & 0x100)) return;
  const uint32_t scale = ss & 0xFF;
  const size_t n = size_t(rw_) * size_t(rh_);
  std::vector<uint8_t> par(n, 0), inside(n, 0);
  int x0 = rw_, y0 = rh_, x1 = 0, y1 = 0;
  for (size_t k = 0; k < c.mods.size(); k++) {
    const ModTri& m = c.mods[k];
    const float area = (m.x[1] - m.x[0]) * (m.y[2] - m.y[0]) - (m.x[2] - m.x[0]) * (m.y[1] - m.y[0]);
    if (area != 0 && area == area) {
      const float inv = 1.0f / area;
      auto ci = [](float f) { return !(f == f) ? 0 : f < -4096.0f ? -4096 : f > 8192.0f ? 8192 : int(std::floor(f)); };
      const int bx0 = std::max(0, ci(std::min({m.x[0], m.x[1], m.x[2]}))),
                bx1 = std::min(rw_, ci(std::max({m.x[0], m.x[1], m.x[2]})) + 1),
                by0 = std::max(0, ci(std::min({m.y[0], m.y[1], m.y[2]}))),
                by1 = std::min(rh_, ci(std::max({m.y[0], m.y[1], m.y[2]})) + 1);
      x0 = std::min(x0, bx0); y0 = std::min(y0, by0); x1 = std::max(x1, bx1); y1 = std::max(y1, by1);
      for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
          const float px = float(x) + 0.5f, py = float(y) + 0.5f;
          const float w0 = ((m.x[2] - m.x[1]) * (py - m.y[1]) - (m.y[2] - m.y[1]) * (px - m.x[1])) * inv;
          const float w1 = ((m.x[0] - m.x[2]) * (py - m.y[2]) - (m.y[0] - m.y[2]) * (px - m.x[2])) * inv;
          const float w2 = 1.0f - w0 - w1;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          const size_t i = size_t(y) * size_t(rw_) + size_t(x);
          if (w0 * m.z[0] + w1 * m.z[1] + w2 * m.z[2] > depth_[i]) par[i] ^= 1;  // face na frente da superfície
        }
    }
    const uint32_t md = m.mode & 3;
    // fim do volume: último triângulo de uma sequência com a instrução 1 ou 2
    if ((md == 1 || md == 2) && (k + 1 == c.mods.size() || (c.mods[k + 1].mode & 3) != md)) {
      for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
          const size_t i = size_t(y) * size_t(rw_) + size_t(x);
          if ((par[i] != 0) == (md == 1)) inside[i] = 1;
          par[i] = 0;
        }
      x0 = rw_; y0 = rh_; x1 = 0; y1 = 0;
    }
  }
  for (size_t i = 0; i < n; i++) {
    if (!inside[i] || !stencil_[i]) continue;
    uint32_t& col = color_[i];
    const uint32_t r = (col & 0xFF) * scale >> 8, g = ((col >> 8) & 0xFF) * scale >> 8, b = ((col >> 16) & 0xFF) * scale >> 8;
    col = (col & 0xFF000000u) | r | g << 8 | b << 16;
  }
}

void Pvr::render() {
  renders++;
  const int k = ctxFor(regs[PARAM_BASE / 4], false);
  const uint32_t gc = regs[TA_GLOB_TILE_CLIP / 4];
  rw_ = std::min(int((gc & 0x3F) + 1) * 32, 1280);
  rh_ = std::min(int(((gc >> 16) & 0xF) + 1) * 32, 960);
  const size_t n = size_t(rw_) * size_t(rh_);
  if (color_.size() < n) { color_.resize(n); depth_.resize(n); stencil_.resize(n); }
  std::fill(stencil_.begin(), stencil_.begin() + long(n), uint8_t(0));
  drawBackground();
  if (k >= 0) {
    TaContext& c = ctx_[k];
    for (int list : {0, 4}) {
      for (const Poly& p : c.polys[list])
        for (uint32_t t = 0; t < p.count; t++) drawTri(c.tris[p.first + t], p, list);
    }
    applyShadows(c);
    // translúcido: ordenado de trás para frente (menor 1/w primeiro), a não ser no modo de pré-ordenação
    struct Ord { float z; uint32_t tri, poly; };
    std::vector<Ord> order;
    const auto& tl = c.polys[2];
    for (uint32_t pi = 0; pi < tl.size(); pi++)
      for (uint32_t t = 0; t < tl[pi].count; t++) {
        const Tri& tr = c.tris[tl[pi].first + t];
        order.push_back({std::max({tr.v[0].z, tr.v[1].z, tr.v[2].z}), tl[pi].first + t, pi});
      }
    if (!(regs[ISP_FEED_CFG / 4] & 1))
      std::stable_sort(order.begin(), order.end(), [](const Ord& x, const Ord& y) { return x.z < y.z; });
    for (const Ord& o : order) drawTri(c.tris[o.tri], tl[o.poly], 2);
  }
  writeFramebuffer();
}

void Pvr::writeFramebuffer() {
  const uint32_t ctrl = regs[FB_W_CTRL / 4];
  const int fmt = int(ctrl & 7);
  const uint32_t base = regs[FB_W_SOF1 / 4] & 0x1FFFFFC;
  const uint32_t stride = (regs[FB_W_LINESTRIDE / 4] & 0x1FF) * 8;
  const uint32_t xc = regs[FB_X_CLIP / 4], yc = regs[FB_Y_CLIP / 4];
  const bool hs = regs[SCALER_CTL / 4] & 0x10000;
  const int ow = hs ? rw_ / 2 : rw_;
  const int xmin = int(xc & 0x7FF), xmax = std::min(int((xc >> 16) & 0x7FF), ow - 1);
  const int ymin = int(yc & 0x3FF), ymax = std::min(int((yc >> 16) & 0x3FF), rh_ - 1);
  const uint32_t kval = (ctrl >> 16) & 0xFF, thr = (ctrl >> 8) & 0xFF;
  const int bpp = fmt <= 3 ? 2 : fmt == 4 ? 3 : 4;
  for (int y = ymin; y <= ymax; y++) {
    for (int x = xmin; x <= xmax; x++) {
      uint32_t c;
      if (hs) {
        const uint32_t p0 = color_[size_t(y) * size_t(rw_) + size_t(2 * x)], p1 = color_[size_t(y) * size_t(rw_) + size_t(2 * x + 1)];
        c = ((p0 >> 1) & 0x7F7F7F7Fu) + ((p1 >> 1) & 0x7F7F7F7Fu);
      } else {
        c = color_[size_t(y) * size_t(rw_) + size_t(x)];
      }
      const uint32_t r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = c >> 24;
      const uint32_t addr = base + uint32_t(y) * stride + uint32_t(x) * uint32_t(bpp);
      switch (fmt) {
        case 0: { const uint16_t v = uint16_t((r >> 3) << 10 | (g >> 3) << 5 | (b >> 3) | ((kval & 0x80) ? 0x8000 : 0));
                  std::memcpy(&sys.vram[System::vram32(addr & 0x7FFFFE)], &v, 2); break; }
        case 1: { const uint16_t v = uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
                  std::memcpy(&sys.vram[System::vram32(addr & 0x7FFFFE)], &v, 2); break; }
        case 2: { const uint16_t v = uint16_t((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
                  std::memcpy(&sys.vram[System::vram32(addr & 0x7FFFFE)], &v, 2); break; }
        case 3: { const uint16_t v = uint16_t((a >= thr ? 0x8000 : 0) | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3));
                  std::memcpy(&sys.vram[System::vram32(addr & 0x7FFFFE)], &v, 2); break; }
        case 4:
          sys.vram[System::vram32((addr) & 0x7FFFFF)] = uint8_t(b);
          sys.vram[System::vram32((addr + 1) & 0x7FFFFF)] = uint8_t(g);
          sys.vram[System::vram32((addr + 2) & 0x7FFFFF)] = uint8_t(r);
          break;
        case 5: sys.vramWrite32(addr, kval << 24 | r << 16 | g << 8 | b); break;
        default: sys.vramWrite32(addr, a << 24 | r << 16 | g << 8 | b); break;
      }
    }
  }
}

// ---------------------------------------------------------------- saída de vídeo
void Pvr::drawOutput(uint32_t* out, int& w, int& h) {
  const uint32_t ctrl = regs[FB_R_CTRL / 4], size = regs[FB_R_SIZE / 4];
  const int depthMode = int((ctrl >> 2) & 3);
  const uint32_t words = (size & 0x3FF) + 1, ys = ((size >> 10) & 0x3FF) + 1, mod = (size >> 20) & 0x3FF;
  const bool inter = regs[SPG_CONTROL / 4] & 0x10;
  const bool dbl = ctrl & 2;
  int pw = depthMode <= 1 ? int(words) * 2 : depthMode == 2 ? int(words) * 4 / 3 : int(words);
  int ph = int(ys) * ((inter || dbl) ? 2 : 1);
  pw = std::clamp(pw, 16, 1280); ph = std::clamp(ph, 16, 960);
  w = pw; h = ph;
  const uint32_t pitch = (words + (mod ? mod - 1 : 0)) * 4;
  if (!(ctrl & 1) || (regs[VO_CONTROL / 4] & 8)) {
    const uint32_t bc = regs[VO_BORDER_COL / 4];
    const uint32_t c = 0xFF000000u | (bc & 0xFF) << 16 | (bc & 0xFF00) | (bc >> 16 & 0xFF);
    std::fill(out, out + size_t(pw) * size_t(ph), c);
    return;
  }
  const uint8_t* vr = sys.vram.data();
  for (int y = 0; y < ph; y++) {
    uint32_t la;
    if (inter) la = ((y & 1) ? regs[FB_R_SOF2 / 4] : regs[FB_R_SOF1 / 4]) + uint32_t(y >> 1) * pitch;
    else la = regs[FB_R_SOF1 / 4] + uint32_t(dbl ? (y >> 1) : y) * pitch;
    uint32_t* o = out + size_t(y) * size_t(pw);
    for (int x = 0; x < pw; x++) {
      uint32_t r, g, b;
      switch (depthMode) {
        case 0: case 1: {
          const uint32_t a = (la + uint32_t(x) * 2) & 0x7FFFFE;
          const uint32_t q = System::vram32(a);
          const uint16_t c = uint16_t(vr[q] | vr[q + 1] << 8);
          if (depthMode == 0) { r = (c >> 10 & 31) << 3; g = (c >> 5 & 31) << 3; b = (c & 31) << 3; r |= r >> 5; g |= g >> 5; b |= b >> 5; }
          else { r = (c >> 11 & 31) << 3; g = (c >> 5 & 63) << 2; b = (c & 31) << 3; r |= r >> 5; g |= g >> 6; b |= b >> 5; }
          break;
        }
        case 2: {
          const uint32_t a = la + uint32_t(x) * 3;
          b = vr[System::vram32(a & 0x7FFFFF)]; g = vr[System::vram32((a + 1) & 0x7FFFFF)]; r = vr[System::vram32((a + 2) & 0x7FFFFF)];
          break;
        }
        default: {
          const uint32_t c = sys.vramRead32(la + uint32_t(x) * 4);
          r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
          break;
        }
      }
      o[x] = 0xFF000000u | b << 16 | g << 8 | r;
    }
  }
}

void Pvr::sync(StateIO& io) {
  io.raw(regs, sizeof regs);
  io.raw(taBuf_, sizeof taBuf_);
  io.sync(taNeed_); io.sync(taHave_); io.sync(listType_);
  io.sync(curPcw_); io.sync(curIsp_); io.sync(curTsp_); io.sync(curTcw_);
  io.raw(faceCol_, sizeof faceCol_); io.raw(faceOfs_, sizeof faceOfs_); io.raw(userClip_, sizeof userClip_);
  io.sync(vtxType_); io.sync(sprite_); io.sync(modVol_); io.raw(strip_, sizeof strip_); io.sync(stripN_);
  io.sync(stripParity_); io.sync(curPoly_); io.sync(curCtx_);
  io.raw(taWordBuf_, sizeof taWordBuf_); io.sync(taWordCnt_);
  io.sync(renders); io.sync(taBlocks);
  io.raw(yuvBuf_, sizeof yuvBuf_); io.sync(yuvHave_); io.sync(yuvCount_);
  for (auto& c : ctx_) {
    io.sync(c.base);
    for (auto& pl : c.polys) {
      uint32_t n = uint32_t(pl.size());
      io.sync(n);
      if (io.reading()) { if (n > 200000) throw std::runtime_error("Estado salvo inválido (PVR)."); pl.resize(n); }
      if (n) io.raw(pl.data(), n * sizeof(Poly));
    }
    uint32_t nm = uint32_t(c.mods.size());
    io.sync(nm);
    if (io.reading()) { if (nm > 200000) throw std::runtime_error("Estado salvo inválido (PVR)."); c.mods.resize(nm); }
    if (nm) io.raw(c.mods.data(), nm * sizeof(ModTri));
    uint32_t n = uint32_t(c.tris.size());
    io.sync(n);
    if (io.reading()) { if (n > 1000000) throw std::runtime_error("Estado salvo inválido (PVR)."); c.tris.resize(n); }
    if (n) io.raw(c.tris.data(), n * sizeof(Tri));
  }
  if (io.reading()) {
    clampTo(taNeed_, 0, 16); clampTo(taHave_, 0, 8); clampTo(listType_, -1, 4); clampTo(stripN_, 0, 2);
    if (yuvHave_ > 480 || (yuvHave_ & 31)) yuvHave_ = 0;
    if (yuvCount_ > 4096) yuvCount_ = 0;
    clampTo(curCtx_, 0, 3); clampTo(taWordCnt_, 0, 7); clampTo(vtxType_, 0, 17);
    if (taHave_ != 0 && taHave_ != 8) taHave_ = 0;
    // triângulos e polígonos com índices fora do lugar invalidam o estado
    for (auto& c : ctx_) {
      for (int l = 0; l < 5; l++)
        for (auto& p : c.polys[l])
          if (uint64_t(p.first) + p.count > c.tris.size()) throw std::runtime_error("Estado salvo inválido (PVR).");
      for (auto& t : c.tris)
        if (t.poly >= 200000) throw std::runtime_error("Estado salvo inválido (PVR).");
    }
    if (curPoly_ >= 0 && (listType_ < 0 || size_t(curPoly_) >= ctx_[curCtx_].polys[listType_].size())) curPoly_ = -1;
  }
}

}  // namespace indigo::dc
