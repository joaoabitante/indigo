// Vídeo do WonderSwan: uma linha por vez, com os registradores do começo da linha.
// Ordem: fundo (cor de fundo) → tela 1 → tela 2 (com janela) → sprites (com janela; prioridade 0 fica atrás
// dos pixels opacos da tela 2). Cores: tons de cinza (8 tons escolhidos de 16, paletas de 4) ou, no modo Color,
// 16 paletas × 16 cores de 12 bits na RAM em FE00h.
#include "ws.h"

namespace indigo::ws {

namespace {
inline uint32_t rgba12(uint16_t c) {
  const uint32_t r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
  return 0xFF000000u | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17);
}
}  // namespace

void WS::renderLine(int y) {
  const uint8_t* ram = iram_.data();
  const int mode = dispMode_ >> 5;
  const bool color = colorMode();
  const bool gray = !(mode & 4);
  const bool bpp4 = (mode & 6) == 6 && color;
  const bool packed = mode == 7 && color;
  const uint16_t tileMask = color ? 0x3FF : 0x1FF;

  auto fetch = [&](uint32_t tile, int x, int ty) -> int {
    tile &= tileMask;
    if (!bpp4) {
      const uint32_t a = 0x2000 + (tile << 4) + (ty << 1);
      const int lo = ram[a], hi = ram[a + 1];
      return ((lo >> (7 - x)) & 1) | (((hi >> (7 - x)) & 1) << 1);
    }
    const uint32_t a = 0x4000 + (tile << 5) + (ty << 2);
    if (packed) return (ram[a + (x >> 1)] >> ((x & 1) ? 0 : 4)) & 15;
    const int s = 7 - x;
    return ((ram[a] >> s) & 1) | (((ram[a + 1] >> s) & 1) << 1) | (((ram[a + 2] >> s) & 1) << 2) | (((ram[a + 3] >> s) & 1) << 3);
  };
  auto opaque = [&](int pal, int c) { return c != 0 || (!bpp4 && !(pal & 4)); };
  auto palColor = [&](int pal, int c) -> uint16_t {
    if (gray) {
      const int l = 15 - pool_[monoPal_[pal][c & 3] & 7];
      return static_cast<uint16_t>(l * 0x111);
    }
    const uint32_t a = 0xFE00 + (pal << 5) + (c << 1);
    return static_cast<uint16_t>((ram[a] | (ram[a + 1] << 8)) & 0xFFF);
  };

  uint16_t bg;
  if (gray) bg = static_cast<uint16_t>((15 - pool_[backColor_ & 7]) * 0x111);
  else bg = static_cast<uint16_t>((ram[0xFE00 + (backColor_ << 1)] | (ram[0xFE01 + (backColor_ << 1)] << 8)) & 0xFFF);

  uint16_t line[224];
  bool scr2Opaque[224] = {};
  for (int x = 0; x < 224; x++) line[x] = bg;

  if (lcdCtrl_ & 1) {
    const int mb1 = (mapBase_ & (color ? 0x0F : 0x07)), mb2 = ((mapBase_ >> 4) & (color ? 0x0F : 0x07));
    auto drawScreen = [&](int mb, uint8_t sx, uint8_t sy, bool isScr2) {
      const int yy = (y + sy) & 255;
      const uint32_t rowBase = (static_cast<uint32_t>(mb) << 11) + ((yy >> 3) << 6);
      const bool winOn = isScr2 && (dispCtrl_ & 0x20), invert = (dispCtrl_ & 0x10) != 0;
      for (int x = 0; x < 224; x++) {
        if (winOn) {
          const Window& w = win2_;
          if (!invert) {
            if (x < w.x0 || x > w.x1 || y < w.y0 || y > w.y1) continue;  // fora da janela
          } else {
            const bool inX = (x >= w.x0 && x <= w.x1) || (x >= w.x1 && x <= w.x0);
            const bool inY = (y >= w.y0 && y <= w.y1) || (y >= w.y1 && y <= w.y0);
            if (inX && inY) continue;
          }
        }
        const int xx = (x + sx) & 255;
        const uint32_t a = (rowBase + ((xx >> 3) << 1)) & 0xFFFF;
        const uint16_t attr = static_cast<uint16_t>(ram[a] | (ram[a + 1] << 8));
        const uint32_t tile = (attr & 0x1FF) | (((attr >> 13) & 1) << 9);
        const int pal = (attr >> 9) & 15;
        int px = xx & 7, py = yy & 7;
        if (attr & 0x4000) px ^= 7;
        if (attr & 0x8000) py ^= 7;
        const int c = fetch(tile, px, py);
        if (!opaque(pal, c)) continue;
        line[x] = palColor(pal, c);
        if (isScr2) scr2Opaque[x] = true;
      }
    };
    if (dispCtrl_ & 1) drawScreen(mb1, scr1x_, scr1y_, false);
    if (dispCtrl_ & 2) drawScreen(mb2, scr2x_, scr2y_, true);

    if (dispCtrl_ & 4) {
      // até 32 sprites por linha, na ordem da tabela; o primeiro fica na frente (desenha de trás para frente)
      int list[32], n = 0;
      for (int i = 0; i < oamCount_ && n < 32; i++) {
        const uint8_t sy = static_cast<uint8_t>(oam_[i] >> 16);
        if (static_cast<uint8_t>(y - sy) <= 7) list[n++] = i;
      }
      const bool winOn = (dispCtrl_ & 8) != 0;
      const Window& w = winSpr_;
      for (int k = n - 1; k >= 0; k--) {
        const uint32_t s = oam_[list[k]];
        const uint32_t tile = s & 0x1FF;
        const int pal = ((s >> 9) & 7) | 8;
        const bool region = (s >> 12) & 1, prio = (s >> 13) & 1;
        const uint8_t sy = static_cast<uint8_t>(s >> 16), sx = static_cast<uint8_t>(s >> 24);
        int py = static_cast<uint8_t>(y - sy);
        if (s & 0x8000) py ^= 7;
        for (int i = 0; i < 8; i++) {
          const int x = (sx + i) & 255;
          if (x >= 224) continue;
          if (winOn) {
            const bool outside = x < w.x0 || x > w.x1 || y < w.y0 || y > w.y1;
            if (region != outside) continue;
          }
          const int c = fetch(tile, (s & 0x4000) ? 7 - i : i, py);
          if (!opaque(pal, c)) continue;
          if (!prio && scr2Opaque[x]) continue;
          line[x] = palColor(pal, c);
        }
      }
    }
  }

  uint32_t* out = fb + y * 224;
  for (int x = 0; x < 224; x++) out[x] = rgba12(line[x]);
}

}  // namespace indigo::ws
