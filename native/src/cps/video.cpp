// Vídeo do CPS1/CPS2 (CPS-A e CPS-B): camadas de tiles, objetos, estrelas, paleta e prioridades.
// Coordenadas: o quadro do hardware tem 512x262 pixels; a parte visível vai de x = 64 a 447 e y = 16 a 239.
// O desenho segue o MAME: as linhas são desenhadas "em lote" no começo do vblank (ou antes de uma interrupção
// de raster do CPS2, até a linha atual) com os registradores daquele momento, e as cores saem da paleta no fim
// do quadro.
#include <cstring>

#include "cps.h"

namespace indigo::cps {

namespace {
constexpr int W = 384, H = 224;

// byte de um plano -> 8 pixels (pixel x no nibble x, bit 0)
struct Expand {
  uint32_t t[256];
  Expand() {
    for (int b = 0; b < 256; b++) {
      uint32_t v = 0;
      for (int x = 0; x < 8; x++)
        if (b & (0x80 >> x)) v |= 1u << (4 * x);
      t[b] = v;
    }
  }
};
const Expand EXP;

// 8 pixels de 4 bits a partir de 4 bytes (um por plano; o byte 3 é o bit mais alto)
inline uint32_t row8(const std::vector<uint8_t>& g, size_t o) {
  if (o + 4 > g.size()) return 0xFFFFFFFFu;  // fora da ROM: transparente
  return EXP.t[g[o]] | (EXP.t[g[o + 1]] << 1) | (EXP.t[g[o + 2]] << 2) | (EXP.t[g[o + 3]] << 3);
}
}  // namespace

// ------------------------------------------------------------------ paleta
void Cps::colorOf(int i) {
  const uint16_t p = pal_[i];
  // brilho de 4 bits; com brilho 0 a cor fica com 1/3 (esquemático da placa, como no MAME)
  const int bright = 0x0F + ((p >> 12) << 1);
  const uint32_t r = static_cast<uint32_t>(((p >> 8) & 15) * 0x11 * bright / 0x2D);
  const uint32_t gc = static_cast<uint32_t>(((p >> 4) & 15) * 0x11 * bright / 0x2D);
  const uint32_t b = static_cast<uint32_t>((p & 15) * 0x11 * bright / 0x2D);
  color_[i] = 0xFF000000u | (b << 16) | (gc << 8) | r;
}

void Cps::buildPalette() {
  const int ctrl = cfg_.palCtrl >= 0 ? cpsB_[(cfg_.palCtrl / 2) & 0x1F] : 0x3F;
  uint32_t src = cpsBase(5, 0x400) / 2;
  bool copied = false;
  for (int page = 0; page < 6; page++) {
    if (ctrl & (1 << page)) {
      for (int i = 0; i < 0x200; i++) {
        pal_[page * 0x200 + i] = gfxram_[(src + static_cast<uint32_t>(i)) & 0x1FFFF];
        colorOf(page * 0x200 + i);
      }
      src += 0x200;
      copied = true;
    } else if (copied) {
      // página pulada; mas se nenhuma foi copiada ainda, a primeira página da RAM vai para a próxima ligada
      src += 0x200;
    }
  }
}

// ------------------------------------------------------------------ faixas de tiles da placa B (PALs)
int Cps::mapCode(int type, int code) const {
  const GfxMapper& m = gfxMapper(game.mapper);
  int shift = 0;
  switch (type) {
    case 1: shift = 1; break;  // objetos
    case 2: shift = 0; break;  // scroll1
    case 4: shift = 1; break;  // scroll2
    case 8: shift = 3; break;  // scroll3
    default: break;
  }
  const uint32_t c = static_cast<uint32_t>(code) << shift;
  for (int i = 0; i < m.count; i++) {
    const GfxRange& r = gfxRange(m.first + i);
    if (c >= r.start && c <= r.end && (r.type & type)) {
      uint32_t base = 0;
      for (int b = 0; b < r.bank && b < 4; b++) base += m.bankSize[b];
      const uint32_t bs = r.bank < 4 ? m.bankSize[r.bank] : 0;
      if (!bs) return -1;
      return static_cast<int>((base + (c & (bs - 1))) >> shift);
    }
  }
  return -1;  // fora da faixa: o PAL não liga nenhuma ROM e o tile fica transparente
}

// ------------------------------------------------------------------ objetos
void Cps::latchObjects() {
  if (cps2) {
    priCtrl_ = out2_[2];
    const std::vector<uint16_t>& src = obj2_[objBank_ & 1];
    std::copy(src.begin(), src.end(), objBuf_.begin());
  } else {
    const uint32_t base = cpsBase(0, 0x800) / 2;
    for (size_t i = 0; i < objBuf_.size(); i++) objBuf_[i] = gfxram_[(base + i) & 0x1FFFF];
  }
}

void Cps::drawSprites() {
  std::fill(spr_.begin(), spr_.end(), 0);
  const std::vector<uint8_t>& g = rg_[RG_GFX];
  const uint32_t elements = static_cast<uint32_t>(g.size() / 128);
  if (!elements) return;
  const uint16_t* base = objBuf_.data();
  const int n = static_cast<int>(objBuf_.size());
  // fim da lista
  int last = n - 4;
  for (int off = 0; off < n; off += 4) {
    if (cps2 ? (base[off + 1] >= 0x8000 || base[off + 3] >= 0xFF00) : ((base[off + 3] & 0xFF00) == 0xFF00)) {
      last = off - 4;
      break;
    }
  }
  auto plot = [&](int code, int col, bool fx, bool fy, int sx, int sy, int prio) {
    const size_t t = static_cast<size_t>(static_cast<uint32_t>(code) % elements) * 128;
    const uint16_t tag = static_cast<uint16_t>(0x8000 | (prio << 12) | (col << 4));
    for (int r = 0; r < 16; r++) {
      const int y = sy + r;
      if (y < 16 || y >= 240) continue;
      const int srow = fy ? 15 - r : r;
      const uint32_t lo = row8(g, t + static_cast<size_t>(srow) * 8), hi = row8(g, t + static_cast<size_t>(srow) * 8 + 4);
      uint16_t* d = &spr_[static_cast<size_t>(y - 16) * W];
      for (int px = 0; px < 16; px++) {
        const int x = sx + px - 64;
        if (x < 0 || x >= W) continue;
        const int scol = fx ? 15 - px : px;
        const int pen = static_cast<int>(((scol < 8 ? lo : hi) >> (4 * (scol & 7))) & 15);
        if (pen != 15) d[x] = static_cast<uint16_t>(tag | pen);
      }
    }
  };
  if (!cps2) {
    for (int i = last; i >= 0; i -= 4) {
      const int x = base[i], y = base[i + 1];
      int code = mapCode(1, base[i + 2]);
      const int colour = base[i + 3];
      const int col = colour & 0x1F;
      const bool flipx = colour & 0x20, flipy = colour & 0x40;
      if (code == -1) continue;
      if (colour & 0xFF00) {
        const int nx = ((colour >> 8) & 15) + 1, ny = ((colour >> 12) & 15) + 1;
        for (int nys = 0; nys < ny; nys++) {
          const int sy = (y + nys * 16) & 0x1FF;
          for (int nxs = 0; nxs < nx; nxs++) {
            const int sx = (x + nxs * 16) & 0x1FF;
            const int cx = flipx ? ((code + (nx - 1) - nxs) & 0xF) : ((code + nxs) & 0xF);
            const int cy = flipy ? (ny - 1 - nys) : nys;
            plot((code & ~0xF) + cx + 0x10 * cy, col, flipx, flipy, sx, sy, 0);
          }
        }
      } else {
        plot(code, col, flipx, flipy, x & 0x1FF, y & 0x1FF, 0);
      }
    }
    return;
  }
  const int xoffs = 64 - out2_[4], yoffs = 16 - out2_[5];
  for (int i = last; i >= 0; i -= 4) {
    int x = base[i], y = base[i + 1];
    const int prio = (x >> 13) & 7;
    const int code = base[i + 2] + ((y & 0x6000) << 3);
    const int colour = base[i + 3];
    const int col = colour & 0x1F;
    const bool flipx = colour & 0x20, flipy = colour & 0x40;
    if (colour & 0x80) { x += out2_[4]; y += out2_[5]; }
    if (colour & 0xFF00) {
      const int nx = ((colour >> 8) & 15) + 1, ny = ((colour >> 12) & 15) + 1;
      for (int nys = 0; nys < ny; nys++) {
        const int sy = (y + nys * 16 + yoffs) & 0x3FF;
        for (int nxs = 0; nxs < nx; nxs++) {
          const int sx = (x + nxs * 16 + xoffs) & 0x3FF;
          int c;
          if (!flipy && !flipx) c = (code & ~0xF) + ((code + nxs) & 0xF) + 0x10 * nys;
          else c = code + (flipx ? (nx - 1) - nxs : nxs) + 0x10 * (flipy ? ny - 1 - nys : nys);
          plot(c, col, flipx, flipy, sx, sy, prio);
        }
      }
    } else {
      plot(code, col, flipx, flipy, (x + xoffs) & 0x3FF, (y + yoffs) & 0x3FF, prio);
    }
  }
}

// ------------------------------------------------------------------ camadas e composição
void Cps::renderUpTo(int toLine) {
  if (toLine > 239) toLine = 239;
  const int from = lastLine_ + 1 < 16 ? 16 : lastLine_ + 1;
  if (toLine < from) return;
  lastLine_ = toLine;
  // fundo: tinta 0xBFF (as placas mostram essa cor onde nada é desenhado)
  for (int y = from; y <= toLine; y++) std::fill_n(&fbIdx_[static_cast<size_t>(y - 16) * W], W, static_cast<uint16_t>(0xBFF));
  // estrelas (Forgotten Worlds), embaixo de tudo
  const std::vector<uint8_t>& st = rg_[RG_STARS];
  const uint16_t lc = cfg_.layerCtrl >= 0 ? cpsB_[(cfg_.layerCtrl / 2) & 0x1F] : 0;
  if (!cps2 && st.size() >= 0x8000) {
    for (int s = 0; s < 2; s++) {
      if (!(lc & cfg_.layerMask[3 + s])) continue;
      // a camada 1 usa o byte +4 e a rolagem do campo 2 (e vice-versa), como no MAME
      const int byteOff = s == 0 ? 4 : 0;
      const int scx = cpsA_[s == 0 ? 14 : 12], scy = cpsA_[s == 0 ? 15 : 13];
      const int palBase = s == 0 ? 0xA00 : 0x800;
      for (int offs = 0; offs < 0x1000; offs++) {
        int col = st[static_cast<size_t>(8 * offs + byteOff)];
        if ((col & 0x1F) == 0x0F) continue;
        const int sy = ((offs % 256) - scy) & 0xFF;
        if (sy < from || sy > toLine) continue;
        const int sx = (((offs / 256) * 32) - scx + (col & 0x1F)) & 0x1FF;
        if (sx < 64 || sx >= 448) continue;
        const int cnt = static_cast<int>((frame_ / 16) % ((col & 0x80) ? 15 : 16));
        col = ((col & 0xE0) >> 1) + cnt;
        fbIdx_[static_cast<size_t>(sy - 16) * W + static_cast<size_t>(sx - 64)] = static_cast<uint16_t>(palBase + col);
      }
    }
  }
  for (int y = from; y <= toLine; y++) renderLine(y);
}

void Cps::renderLine(int y) {
  uint16_t lay[3][W];
  uint8_t high[3][W];
  bool on[3];
  const uint16_t lc = cfg_.layerCtrl >= 0 ? cpsB_[(cfg_.layerCtrl / 2) & 0x1F] : 0;
  const uint16_t vc = cpsA_[0x11];
  on[0] = (lc & cfg_.layerMask[0]) != 0;
  on[1] = (lc & cfg_.layerMask[1]) && (vc & 4);
  on[2] = (lc & cfg_.layerMask[2]) && (vc & 8);
  uint16_t prioMask[4];
  for (int i = 0; i < 4; i++) prioMask[i] = cfg_.prio[i] >= 0 ? cpsB_[(cfg_.prio[i] / 2) & 0x1F] : 0;
  const std::vector<uint8_t>& g = rg_[RG_GFX];
  for (int L = 0; L < 3; L++) {
    if (!on[L]) continue;
    uint16_t* col = lay[L];
    uint8_t* hi = high[L];
    const int size = 8 << L;                 // 8, 16, 32
    const int mask = 64 * size - 1;          // mapa de 64x64 tiles
    const uint32_t base = cpsBase(1 + L, 0x4000);
    int scx = cpsA_[6 + 2 * L];
    const int scy = cpsA_[7 + 2 * L];
    if (L == 1 && (vc & 1)) {
      // rolagem por linha da camada 2: tabela na área "other" (base no registrador 4, início no 0x10)
      const uint32_t other = cpsBase(4, 0x800) / 2;
      scx += gfxram_[(other + ((static_cast<uint32_t>(y) + cpsA_[0x10]) & 0x3FF)) & 0x1FFFF];
    }
    const int ty = (y + scy) & mask;
    const int row = ty / size;
    const int type = 2 << L;
    const uint32_t tileBytes = L == 0 ? 64 : L == 1 ? 128 : 512;
    const uint32_t elements = static_cast<uint32_t>(g.size() / tileBytes);
    const int palBase = 0x200 * (L + 1);
    int x = 0;
    int tx = (64 + scx) & mask;
    while (x < W) {
      const int colIdx = tx / size;
      const int fx0 = tx & (size - 1);
      int ti;
      if (L == 0) ti = (row & 0x1F) + ((colIdx & 0x3F) << 5) + ((row & 0x20) << 6);
      else if (L == 1) ti = (row & 0x0F) + ((colIdx & 0x3F) << 4) + ((row & 0x30) << 6);
      else ti = (row & 0x07) + ((colIdx & 0x3F) << 3) + ((row & 0x38) << 6);
      int code = gfxWord(base, static_cast<uint32_t>(2 * ti));
      const uint16_t attr = gfxWord(base, static_cast<uint32_t>(2 * ti + 1));
      if (L == 2) code &= 0x3FFF;
      const int mapped = mapCode(type, code);
      const bool flipx = attr & 0x20, flipy = attr & 0x40;
      const uint16_t pm = prioMask[(attr >> 7) & 3];
      const uint16_t cbase = static_cast<uint16_t>(palBase + (attr & 0x1F) * 16);
      // uma linha do tile (até 32 pixels) decodificada
      uint8_t pens[32];
      if (mapped < 0 || !elements) {
        std::memset(pens, 15, sizeof pens);
      } else {
        const int fy = flipy ? size - 1 - (ty & (size - 1)) : (ty & (size - 1));
        const size_t t = static_cast<size_t>(static_cast<uint32_t>(mapped) % elements) * tileBytes;
        if (L == 0) {
          // 8x8: metade esquerda ou direita do bloco de 16 pixels, pela coluna (par/ímpar)
          const uint32_t r8 = row8(g, t + static_cast<size_t>(fy) * 8 + ((colIdx & 1) ? 4 : 0));
          for (int p = 0; p < 8; p++) pens[p] = static_cast<uint8_t>((r8 >> (4 * p)) & 15);
        } else {
          const size_t stride = L == 1 ? 8 : 16;
          for (int grp = 0; grp < size / 8; grp++) {
            const uint32_t r8 = row8(g, t + static_cast<size_t>(fy) * stride + static_cast<size_t>(grp) * 4);
            for (int p = 0; p < 8; p++) pens[grp * 8 + p] = static_cast<uint8_t>((r8 >> (4 * p)) & 15);
          }
        }
      }
      for (int px = fx0; px < size && x < W; px++, x++) {
        const int pen = pens[flipx ? size - 1 - px : px];
        col[x] = pen == 15 ? 0xFFFF : static_cast<uint16_t>(cbase + pen);
        hi[x] = static_cast<uint8_t>((pm >> pen) & 1);
      }
      tx = (tx + (size - fx0)) & mask;
    }
  }
  uint16_t* dst = &fbIdx_[static_cast<size_t>(y - 16) * W];
  const uint16_t* spr = &spr_[static_cast<size_t>(y - 16) * W];
  int l[4] = {(lc >> 6) & 3, (lc >> 8) & 3, (lc >> 10) & 3, (lc >> 12) & 3};
  if (!cps2) {
    for (int s = 0; s < 4; s++) {
      if (l[s] == 0) {
        // objetos; as tintas "altas" da camada logo abaixo ficam por cima deles
        const int below = s > 0 && l[s - 1] != 0 && on[l[s - 1] - 1] ? l[s - 1] - 1 : -1;
        for (int x = 0; x < W; x++) {
          if (!(spr[x] & 0x8000)) continue;
          if (below >= 0 && high[below][x]) continue;
          dst[x] = spr[x] & 0x0FFF;
        }
      } else if (on[l[s] - 1]) {
        const uint16_t* c = lay[l[s] - 1];
        for (int x = 0; x < W; x++)
          if (c[x] != 0xFFFF) dst[x] = c[x];
      }
    }
    return;
  }
  // CPS2: tira a posição dos objetos da ordem; prioridade de cada camada pelo registrador de objetos
  int lp[4];
  for (int s = 0; s < 4; s++) lp[s] = (priCtrl_ >> (4 * l[s])) & 15;
  if (l[0] == 0) { l[0] = l[1]; l[1] = 0; lp[0] = lp[1]; }
  if (l[1] == 0) { l[1] = l[2]; l[2] = 0; lp[1] = lp[2]; }
  if (l[2] == 0) { l[2] = l[3]; l[3] = 0; lp[2] = lp[3]; }
  uint32_t primasks[8];
  {
    uint32_t mask0 = 0xAA, mask1 = 0xCC;
    if (lp[0] > lp[1]) mask0 &= ~0x88u;
    if (lp[0] > lp[2]) mask0 &= ~0xA0u;
    if (lp[1] > lp[2]) mask1 &= ~0xC0u;
    primasks[0] = 0xFF;
    for (int i = 1; i < 8; i++) {
      if (i <= lp[0] && i <= lp[1] && i <= lp[2]) { primasks[i] = 0xFE; continue; }
      primasks[i] = 0;
      if (i <= lp[0]) primasks[i] |= mask0;
      if (i <= lp[1]) primasks[i] |= mask1;
      if (i <= lp[2]) primasks[i] |= 0xF0;
    }
  }
  uint8_t pri[W];
  std::memset(pri, 0, sizeof pri);
  for (int s = 0; s < 3; s++) {
    if (l[s] == 0 || !on[l[s] - 1]) continue;
    const uint16_t* c = lay[l[s] - 1];
    for (int x = 0; x < W; x++)
      if (c[x] != 0xFFFF) { dst[x] = c[x]; pri[x] |= static_cast<uint8_t>(1 << s); }
  }
  for (int x = 0; x < W; x++) {
    const uint16_t sp = spr[x];
    if (!(sp & 0x8000)) continue;
    if ((primasks[(sp >> 12) & 7] >> pri[x]) & 1) continue;
    dst[x] = sp & 0x0FFF;
  }
}

void Cps::finishFrame() {
  const bool flip = (cpsA_[0x11] & 0x8000) != 0;
  const int rot = game.rot & 3;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const int sx = flip ? W - 1 - x : x, sy = flip ? H - 1 - y : y;
      const uint32_t c = color_[fbIdx_[static_cast<size_t>(sy) * W + static_cast<size_t>(sx)] % 0xC00];
      size_t d;
      switch (rot) {
        case 1: d = static_cast<size_t>(x) * H + static_cast<size_t>(H - 1 - y); break;   // 90° horário
        case 2: d = static_cast<size_t>(H - 1 - y) * W + static_cast<size_t>(W - 1 - x); break;
        case 3: d = static_cast<size_t>(W - 1 - x) * H + static_cast<size_t>(y); break;   // 270° horário
        default: d = static_cast<size_t>(y) * W + static_cast<size_t>(x); break;
      }
      out_[d] = c;
    }
  }
}

}  // namespace indigo::cps
