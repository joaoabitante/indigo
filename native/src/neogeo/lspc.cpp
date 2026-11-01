#include "lspc.h"

#include <algorithm>
#include <cstring>

namespace indigo::neogeo {

namespace {
// colunas desenhadas (bit p = coluna p) do sprite para os 16 níveis de redução horizontal (tabela da neogeodev)
const uint16_t HSHRINK[16] = {
    0x0100, 0x0110, 0x1110, 0x1114, 0x5114, 0x5154, 0x5554, 0x5555,
    0x5755, 0x575D, 0xD75D, 0xD7DD, 0xF7DD, 0xF7DF, 0xFFDF, 0xFFFF};
bool shrinkCol(int level, int p) { return (HSHRINK[level] >> p) & 1; }

// 4 planos de um byte cada -> 8 pontos de 4 bits (ponto i no nibble i)
struct Planar {
  uint32_t spread[256];
  Planar() {
    for (int b = 0; b < 256; b++) {
      uint32_t v = 0;
      for (int i = 0; i < 8; i++) if (b & (1 << i)) v |= 1u << (i * 4);
      spread[b] = v;
    }
  }
};
const Planar PL;
}  // namespace

LSPC::LSPC() {
  for (int i = 0; i < 0x2000; i++) convert(i);
}

void LSPC::reset() {
  palBank_ = 0;
  shadow_ = false;
  vramAddr_ = 0;
  vramMod_ = 0;
  aaCounter_ = aaReload_ = aaTimer_ = 0;
  aaDisable_ = false;
  scanline = 0;
  cyc = 0;
}

void LSPC::setCrom(const uint8_t* c, size_t size) {
  crom_ = size >= 128 ? c : nullptr;
  cromSize_ = size;
  uint32_t tiles = static_cast<uint32_t>(size >> 7), m = 1;
  while (m < tiles) m <<= 1;
  tileMask_ = m - 1;
}

void LSPC::setFix(const uint8_t* s, size_t size, int bankType) {
  fix_ = size >= 32 ? s : nullptr;
  uint32_t m = 32;
  while (m < size) m <<= 1;
  fixMask_ = (size & (size - 1)) == 0 ? static_cast<uint32_t>(size - 1) : m - 1;  // o núcleo completa até potência de 2
  fixBank_ = bankType;
}

void LSPC::setCdSpriteLayout(bool cd) {
  static const uint8_t CART[4] = {0, 2, 1, 3}, CD[4] = {1, 0, 3, 2};
  std::memcpy(plane_, cd ? CD : CART, 4);
}

void LSPC::writeData(uint16_t v) {
  const int i = vramIndex(vramAddr_);
  if (i >= 0x8000 && i < 0x8600 && vram[i] != v) sprDirty_ = true;
  vram[i] = v;
  vramAddr_ = static_cast<uint16_t>(((vramAddr_ + vramMod_) & 0x7FFF) | (vramAddr_ & 0x8000));
}

void LSPC::writeMode(uint16_t v) {
  aaReload_ = static_cast<uint8_t>(v >> 8);
  aaDisable_ = (v & 0x08) != 0;
}

// cor: D R0 G0 B0 R4 R3 R2 R1 G4 G3 G2 G1 B4 B3 B2 B1 (D = "escuro", invertido no aparelho); 6 bits por
// componente -> 8 bits como a paleta "raw" do Geolith; a sombra divide por 2
void LSPC::convert(int i) {
  const uint16_t d = pal[i];
  auto comp = [&](int hi4, int lo1) {
    const unsigned raw = ((hi4 << 2) | (lo1 << 1) | (d >> 15)) ^ 1;
    return (raw * 259 + 33) >> 6;
  };
  const unsigned r = comp((d >> 8) & 15, (d >> 14) & 1);
  const unsigned g = comp((d >> 4) & 15, (d >> 13) & 1);
  const unsigned b = comp(d & 15, (d >> 12) & 1);
  rgb_[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
  rgbShadow_[i] = 0xFF000000u | ((b >> 1) << 16) | ((g >> 1) << 8) | (r >> 1);
}

void LSPC::palWrite(uint32_t a, uint16_t v) {
  const int i = palBank_ * 4096 + ((a >> 1) & 0xFFF);
  pal[i] = v;
  convert(i);
}

int LSPC::run(int cycles) {
  int ev = 0;
  while (cycles > 0) {
    const int start = cyc;
    const int n = std::min(cycles, CYC_PER_LINE - start);
    const int end = start + n;
    if (start <= 29 && end > 29) {
      if (scanline == 8) {  // auto-animação: temporizador de 8 bits por quadro
        if (--aaTimer_ == 0xFF) {
          aaCounter_ = (aaCounter_ + 1) & 7;
          aaTimer_ = aaReload_;
        }
      } else if (scanline == 249) {
        ev |= EV_VBLANK;
      }
    }
    if (start <= 573 && end > 573) {
      drawLine();
      if (scanline == 248) ev |= EV_RELOAD_TIMER;
    }
    if (start <= 712 && end > 712) scanline = (scanline + 1) % LINES;
    cyc = end % CYC_PER_LINE;
    cycles -= n;
  }
  return ev;
}

void LSPC::drawLine() {
  if (scanline >= 8 && scanline < 248) {
    const int row = scanline - 24;
    uint16_t* lb = lineBuf_[lbActive_];
    if (row >= 0 && row < OUT_H) {
      const uint32_t* p = shadow_ ? rgbShadow_ : rgb_;
      const uint32_t bd = p[palBank_ * 4096 + 4095];
      if (videoOn) {
        uint32_t line[320];
        for (int x = 0; x < 320; x++) line[x] = lb[x] ? p[lb[x]] : bd;
        if (fixOn) fixLine(scanline - 8, line);
        std::memcpy(&fb[row * OUT_W], line + 8, OUT_W * sizeof(uint32_t));
      } else {
        std::fill(&fb[row * OUT_W], &fb[(row + 1) * OUT_W], 0xFF000000u);  // vídeo desligado (CD): preto
      }
    }
    std::memset(lb, 0, sizeof lineBuf_[0]);
    spriteLine(scanline - 6);
  } else if (scanline == 6 || scanline == 7) {
    spriteLine(scanline - 6);
  }
}

void LSPC::buildSprites() {
  unsigned xpos = 0, ypos = 0, size = 0, hs = 15, vs = 255;
  for (int i = 1; i < 382; i++) {
    const uint16_t scb3 = vram[0x8200 + i], scb2 = vram[0x8000 + i];
    if (scb3 & 0x40) {  // encadeado: continua à direita do anterior (com a largura reduzida dele)
      xpos = (xpos + hs + 1) & 0x1FF;
    } else {
      xpos = (vram[0x8400 + i] >> 7) & 0x1FF;
      ypos = (scb3 >> 7) & 0x1FF;
      size = scb3 & 0x3F;
      vs = scb2 & 0xFF;
    }
    hs = (scb2 >> 8) & 15;
    spr_[i] = {static_cast<uint16_t>(xpos), static_cast<uint16_t>(ypos), static_cast<uint16_t>(size),
               static_cast<uint16_t>(vs), static_cast<uint16_t>(hs)};
  }
  sprDirty_ = false;
}

void LSPC::spriteLine(int line) {
  uint16_t* buf = lineBuf_[lbActive_];
  lbActive_ ^= 1;
  if (!crom_ || !l0_ || !sprOn) return;
  if (sprDirty_) buildSprites();
  int count = 0;
  for (int i = 1; i < 382; i++) {
    const SprInfo& sp = spr_[i];
    const unsigned size = sp.size, vs = sp.vs, hs = sp.hs, xpos = sp.x;
    if (size == 0) continue;
    unsigned srow = static_cast<unsigned>(line - (0x200 - static_cast<int>(sp.y))) & 0x1FF;
    if (srow >= (size << 4)) continue;
    if (count++ == 96) break;
    bool invert = srow > 0xFF;
    unsigned zrow = srow & 0xFF;
    if (invert) zrow ^= 0xFF;
    if (size == 33) {  // tamanho 33: a borda se repete quando o sprite é reduzido
      zrow %= (vs + 1) << 1;
      if (zrow > vs) {
        zrow = ((vs + 1) << 1) - 1 - zrow;
        invert = !invert;
      }
    }
    srow = l0_[(vs << 8) + zrow];
    if (invert) srow ^= 0x1FF;
    const int tmap = (i << 6) + ((srow >> 3) & 0x3E);
    const uint16_t attr = vram[tmap + 1];
    uint32_t tnum = (vram[tmap] | ((attr & 0xF0u) << 12)) & tileMask_;
    if (!aaDisable_) {
      if (attr & 8) tnum = (tnum & ~7u) | (aaCounter_ & 7);
      else if (attr & 4) tnum = (tnum & ~3u) | (aaCounter_ & 3);
    }
    size_t toff = static_cast<size_t>(tnum) << 7;
    if (toff >= cromSize_) toff %= cromSize_;
    const unsigned y = (attr & 2) ? 15 - (srow & 15) : (srow & 15);
    const uint16_t poff = static_cast<uint16_t>(((attr >> 4) & 0xFF0) + palBank_ * 4096);
    // pontos da linha do tile: metade esquerda no bloco +64, direita no +0; 4 bytes por linha (planos em plane_)
    const uint8_t* rl = crom_ + toff + 64 + y * 4;
    const uint8_t* rr = crom_ + toff + y * 4;
    uint32_t half[2];
    const uint8_t* pl = plane_;
    half[0] = PL.spread[rl[pl[0]]] | (PL.spread[rl[pl[1]]] << 1) | (PL.spread[rl[pl[2]]] << 2) | (PL.spread[rl[pl[3]]] << 3);
    half[1] = PL.spread[rr[pl[0]]] | (PL.spread[rr[pl[1]]] << 1) | (PL.spread[rr[pl[2]]] << 2) | (PL.spread[rr[pl[3]]] << 3);
    if (!(half[0] | half[1])) continue;  // linha transparente (só a largura conta, e ela não depende dos pontos)
    const bool hflip = attr & 1;
    if (hs == 15) {  // largura cheia: 16 pontos seguidos
      for (int p = 0; p < 16; p++) {
        const int q = hflip ? 15 - p : p;
        const unsigned c = (half[q >> 3] >> ((q & 7) * 4)) & 15;
        const unsigned x = (xpos + p) & 0x1FF;
        if (c && x < 320) buf[x] = static_cast<uint16_t>(poff + c);
      }
      continue;
    }
    unsigned draw = 0;
    for (int p = 0; p < 16; p++) {
      if (!shrinkCol(hs, p)) continue;
      const int q = hflip ? 15 - p : p;
      const unsigned c = (half[q >> 3] >> ((q & 7) * 4)) & 15;
      const unsigned x = (xpos + draw) & 0x1FF;
      if (c && x < 320) buf[x] = static_cast<uint16_t>(poff + c);
      draw++;
    }
  }
}

void LSPC::fixLine(int line, uint32_t* out) {
  if (!fix_) return;
  const unsigned trow = static_cast<unsigned>(line) >> 3, row = line & 7;
  const uint32_t* p = shadow_ ? rgbShadow_ : rgb_;
  unsigned offsets[34] = {};
  if (fixBank_ == FIX_LINE) {  // troca de banco por linha (pares de linhas de tiles); regra do MAME/Geolith
    unsigned bank = 0, k = 0, y = 0;
    while (y < 32) {
      if (vram[0x7500 + k] == 0x0200 && (vram[0x7580 + k] & 0xFF00) == 0xFF00) {
        bank = (~vram[0x7580 + k] & 3) * 4096;
        offsets[y++] = bank;
      }
      offsets[y++] = bank;
      k += 2;
    }
  }
  for (unsigned x = 0; x < 40; x++) {
    const uint16_t e = vram[0x7000 + trow + (x << 5)];
    uint32_t tnum = e & 0xFFF;
    if (fixBank_ == FIX_LINE) {
      tnum += offsets[(trow - 2) & 0x1F];
    } else if (fixBank_ == FIX_TILE) {
      const unsigned w = vram[0x7500 + ((trow - 1) & 0x1F) + 32 * (x / 6)];
      tnum += ((~(w >> ((5 - x % 6) * 2))) & 3) * 4096;
    }
    const uint8_t* t = fix_ + ((tnum << 5) & fixMask_);
    const unsigned poff = ((e >> 8) & 0xF0) + palBank_ * 4096;
    uint32_t* o = out + x * 8;
    static const uint8_t COL[4] = {0x10, 0x18, 0x00, 0x08};
    for (int k = 0; k < 4; k++) {
      const uint8_t b = t[COL[k] + row];
      if (b & 15) o[k * 2] = p[poff + (b & 15)];
      if (b >> 4) o[k * 2 + 1] = p[poff + (b >> 4)];
    }
  }
}

void LSPC::sync(StateIO& io) {
  io.raw(vram, sizeof vram);
  io.raw(pal, sizeof pal);
  io.raw(lineBuf_, sizeof lineBuf_);
  io.sync(palBank_); io.sync(shadow_); io.sync(vramAddr_); io.sync(vramMod_);
  io.sync(aaCounter_); io.sync(aaReload_); io.sync(aaTimer_); io.sync(aaDisable_);
  io.sync(lbActive_); io.sync(scanline); io.sync(cyc);
  if (io.reading()) {
    palBank_ &= 1;
    aaCounter_ &= 7;
    lbActive_ &= 1;
    for (auto& b : lineBuf_) for (auto& v : b) v &= 0x1FFF;
    if (scanline < 0 || scanline >= LINES) throw std::runtime_error("Estado salvo danificado.");
    if (cyc < 0 || cyc >= CYC_PER_LINE) throw std::runtime_error("Estado salvo danificado.");
    for (int i = 0; i < 0x2000; i++) convert(i);
    sprDirty_ = true;
  }
}

}  // namespace indigo::neogeo
