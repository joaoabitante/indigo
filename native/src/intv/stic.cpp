#include "stic.h"

#include <cstring>

namespace indigo::intv {

namespace {
// bits que existem em cada registro (os outros leem 1), como medido no aparelho (tabela do jzIntv)
struct Mask { uint16_t andM, orM; };
Mask regMask(int a) {
  if (a < 0x08) return {0x07FF, 0x3800};                    // MOB X
  if (a < 0x10) return {0x0FFF, 0x3000};                    // MOB Y
  if (a < 0x18) return {0x3FFF, 0x0000};                    // MOB A
  if (a < 0x20) return {static_cast<uint16_t>(0x03FF & ~(1 << (a - 0x18))), 0x3C00};  // colisões (sem o próprio bit)
  if (a >= 0x28 && a <= 0x2C) return {0x000F, 0x3FF0};      // pilha de cores e borda
  if (a == 0x30 || a == 0x31) return {0x0007, 0x3FF8};      // atrasos
  if (a == 0x32) return {0x0003, 0x3FFC};                   // extensão da borda
  return {0x0000, 0x3FFF};
}
const int MOB_HGT[8] = {8, 16, 16, 32, 32, 64, 64, 128};

uint16_t reverse8(uint16_t b) {
  uint16_t r = 0;
  for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= static_cast<uint16_t>(0x80 >> i);
  return r;
}
uint16_t double8(uint16_t b) {
  uint16_t r = 0;
  for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= static_cast<uint16_t>(3 << (2 * i));
  return r;
}
}  // namespace

void Stic::reset() {
  std::memset(reg, 0, sizeof reg);
  // o chip liga com os registros em 1 (escrever $21 deixa no modo frente/fundo até a EXEC ler $21)
  for (int a = 0; a < 0x40; a++)
    if (a != 0x20) writeReg(a, 0xFFFF);
  fgbg = false;
  enableWrite = false;
  std::memset(gmem + 0x800, 0, 0x200);
  std::memset(out, 0, sizeof out);
}

uint16_t Stic::readReg(int a) {
  a &= 0x3F;
  if (a == 0x21) fgbgNext = false;
  const Mask m = regMask(a);
  return static_cast<uint16_t>((reg[a] & m.andM) | m.orM);
}

void Stic::writeReg(int a, uint16_t v) {
  a &= 0x3F;
  if (a == 0x20) enableWrite = true;
  if (a == 0x21) fgbgNext = true;
  const Mask m = regMask(a);
  reg[a] = static_cast<uint16_t>((v & m.andM) | m.orM);
}

void Stic::blank() { std::memset(out, 0, sizeof out); }

void Stic::drawBackground(const uint16_t* bt) {
  int cs = 0;
  const int stack[4] = {reg[0x28] & 15, reg[0x29] & 15, reg[0x2A] & 15, reg[0x2B] & 15};
  for (int r = 0; r < 12; r++) {
    int bg = stack[cs];
    for (int c = 0; c < 20; c++) {
      const uint16_t card = bt[r * 20 + c] & 0x3FFF;
      const int y0 = r * 8, x0 = c * 8;
      if (fgbg) {
        const int fg = card & 7;
        bg = ((card >> 9) & 0xB) | ((card >> 11) & 4);
        const int idx = card & 0x9F8;
        for (int y = 0; y < 8; y++) {
          const uint8_t px = gmem[idx + y];
          for (int x = 0; x < 8; x++) {
            const bool on = px & (0x80 >> x);
            bgCol_[y0 + y][x0 + x] = static_cast<uint8_t>(on ? fg : bg);
            bgFg_[y0 + y][x0 + x] = on;
          }
        }
        continue;
      }
      if ((card & 0x1800) == 0x1000) {  // quadrados coloridos: 4 blocos de 4×4; cor 7 = cor da pilha
        int q[4] = {card & 7, (card >> 3) & 7, (card >> 6) & 7, ((card >> 9) & 3) | ((card >> 11) & 4)};
        for (int k = 0; k < 4; k++) {
          const bool on = q[k] != 7;
          const int col = on ? q[k] : bg;
          const int qx = x0 + (k & 1) * 4, qy = y0 + (k >> 1) * 4;
          for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
              bgCol_[qy + y][qx + x] = static_cast<uint8_t>(col);
              bgFg_[qy + y][qx + x] = on;
            }
        }
        continue;
      }
      if (card & 0x2000) { cs = (cs + 1) & 3; bg = stack[cs]; }
      int idx = card & 0xFF8;
      if (idx & 0x800) idx &= 0x9F8;
      const int fg = ((card >> 9) & 8) | (card & 7);
      for (int y = 0; y < 8; y++) {
        const uint8_t px = gmem[idx + y];
        for (int x = 0; x < 8; x++) {
          const bool on = px & (0x80 >> x);
          bgCol_[y0 + y][x0 + x] = static_cast<uint8_t>(on ? fg : bg);
          bgFg_[y0 + y][x0 + x] = on;
        }
      }
    }
    lastBg_[r] = static_cast<uint8_t>(bg);
  }
}

// figura de cada MOB (16 pontos por linha do cartão, já com largura dupla e espelhos)
void Stic::drawMobs() {
  for (int i = 0; i < 8; i++) {
    const uint16_t xr = reg[i], yr = reg[i + 8], ar = reg[i + 0x10];
    const int rows = (yr & 0x80) ? 16 : 8;
    mobRows_[i] = rows;
    int idx = ar & 0xFF8;
    if (fgbg || (idx & 0x800)) idx &= 0x9F8;
    if (rows == 16) idx &= 0xFF0;
    const int flip = (yr & 0x800) ? rows - 1 : 0;
    for (int y = 0; y < 16; y++) mobBmp_[i][y] = 0;
    for (int y = 0; y < rows; y++) {
      uint16_t b = gmem[(idx + y) & 0xFFF];
      if (yr & 0x400) b = reverse8(b);
      mobBmp_[i][y ^ flip] = (xr & 0x400) ? double8(b) : static_cast<uint16_t>(b << 8);
    }
  }
}

void Stic::render(const uint16_t* bt) {
  fgbg = fgbgNext;
  drawBackground(bt);
  drawMobs();
  const int hd = reg[0x30] & 7, vd = (reg[0x31] & 7) * 2;
  const int bord = reg[0x2C] & 15;
  const bool extL = reg[0x32] & 1, extT = reg[0x32] & 2;

  // plano dos MOBs visíveis (coordenadas sem atraso: x 0..191, meias linhas 0..207); o MOB 0 fica por cima
  static thread_local uint8_t mcol[208][192], mvis[208][192];
  std::memset(mvis, 0, sizeof mvis);
  for (int i = 7; i >= 0; i--) {
    const uint16_t xr = reg[i], yr = reg[i + 8], ar = reg[i + 0x10];
    const int xp = xr & 0xFF, yp = (yr & 0x7F) * 2;
    if (xp == 0 || xp >= 167 || !(xr & 0x300) || yp >= 208 || !(xr & 0x200)) continue;
    const int ysh = (yr >> 8) & 3, hgt = MOB_HGT[(yr >> 7) & 7];
    const uint8_t col = static_cast<uint8_t>(((ar >> 9) & 8) | (ar & 7));
    const uint8_t pri = (ar & 0x2000) ? 2 : 0;
    for (int row = 0; row < hgt && yp + row < 208; row++) {
      const uint16_t bits = mobBmp_[i][(row >> ysh) & 15];
      if (!bits) continue;
      for (int b = 0; b < 16; b++)
        if (bits & (0x8000 >> b)) {
          const int x = xp + b;
          if (x >= 192) break;
          mcol[yp + row][x] = col;
          mvis[yp + row][x] = static_cast<uint8_t>(1 | pri);
        }
    }
  }

  for (int sy = 0; sy < OUT_H; sy++) {
    uint8_t* o = &out[sy * OUT_W];
    const bool rowBorder = sy < (extT ? 32 : 16) || sy >= 208;
    for (int sx = 0; sx < OUT_W; sx++) {
      if (rowBorder || sx < 8 || sx >= 167 || (extL && sx < 16)) { o[sx] = static_cast<uint8_t>(bord); continue; }
      const int x = sx - hd, hl = sy - vd;
      if (x < 0 || hl < 0) { o[sx] = 0; continue; }
      int c;
      bool fg = false;
      if (x >= 8 && x < 168 && hl >= 16 && hl < 208) {
        c = bgCol_[(hl - 16) >> 1][x - 8];
        fg = bgFg_[(hl - 16) >> 1][x - 8];
      } else if (x < 8 && hl >= 16) {
        const int y = hl >> 1;  // linha do fundo (8..103): a cor vem do último cartão da linha anterior
        c = y == 8 ? lastBg_[11] : lastBg_[((y - 9) >> 3) % 12];
      } else {
        c = lastBg_[11];
      }
      if (hl < 208 && x < 192) {
        const uint8_t v = mvis[hl][x];
        if ((v & 1) && !((v & 2) && fg)) c = mcol[hl][x];
      }
      o[sx] = static_cast<uint8_t>(c);
    }
  }
  collisions();
}

// colisões entre MOBs com o bit INTR, com o fundo (pontos de primeiro plano) e com a borda
void Stic::collisions() {
  const int hd = reg[0x30] & 7, vd = (reg[0x31] & 7) * 2;
  const bool extL = reg[0x32] & 1, extT = reg[0x32] & 2;
  static thread_local uint8_t cm[224][192];
  std::memset(cm, 0, sizeof cm);
  int any = 0;
  for (int i = 0; i < 8; i++) {
    const uint16_t xr = reg[i], yr = reg[i + 8];
    const int xp = xr & 0xFF, yp = (yr & 0x7F) * 2;
    if (!(xr & 0x100) || xp == 0 || xp + hd >= 168 || yp > 0xD8 - vd) continue;
    const int ysh = (yr >> 8) & 3, hgt = MOB_HGT[(yr >> 7) & 7];
    uint16_t bitsFound = 0;
    for (int row = 0; row < hgt && yp + row < 224; row++) {
      const uint16_t bits = mobBmp_[i][(row >> ysh) & 15];
      if (!bits) continue;
      const int hl = yp + row, sy = hl + vd;
      for (int b = 0; b < 16; b++) {
        if (!(bits & (0x8000 >> b))) continue;
        const int x = xp + b, sx = x + hd;
        if (sx < 7 || sx > 167 || x >= 192) continue;
        cm[hl][x] |= static_cast<uint8_t>(1 << i);
        bitsFound = 1;
        // fundo
        if (x >= 8 && x < 168 && hl >= 16 && hl < 208 && hl <= 206 - vd && bgFg_[(hl - 16) >> 1][x - 8] &&
            !(x >= 160 && x > 166 - hd))
          reg[0x18 + i] |= 0x100;
        // borda
        if (sy >= 15 && sy <= 208 &&
            (sx == 7 || sx == 167 || (extL && sx <= 15) || sy == 15 || (extT && sy <= 31) || sy == 208))
          reg[0x18 + i] |= 0x200;
      }
    }
    any += bitsFound;
  }
  if (any < 2) return;
  for (int hl = 0; hl < 224; hl++) {
    const int sy = hl + vd;
    if (sy < 15 || sy > 208) continue;
    for (int x = 0; x < 192; x++) {
      const uint8_t m = cm[hl][x];
      if (!(m & (m - 1))) continue;  // menos de dois MOBs neste ponto
      for (int i = 0; i < 8; i++)
        if (m & (1 << i)) reg[0x18 + i] |= static_cast<uint16_t>(m & ~(1 << i));
    }
  }
}

void Stic::sync(StateIO& io) {
  io.raw(reg, sizeof reg);
  io.raw(gmem + 0x800, 0x200);
  io.sync(fgbg); io.sync(fgbgNext); io.sync(enableWrite);
  io.raw(out, sizeof out);  // a imagem mostrada também, para o quadro logo após carregar ser o mesmo
  if (io.reading()) {
    for (uint8_t& c : out) c &= 15;
    for (int a = 0; a < 0x40; a++) {
      const Mask m = regMask(a);
      reg[a] = static_cast<uint16_t>((reg[a] & m.andM) | m.orM);
    }
  }
}

}  // namespace indigo::intv
