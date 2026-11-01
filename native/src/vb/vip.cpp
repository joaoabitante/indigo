#include "vip.h"

#include <cstring>

#include "vb.h"

namespace indigo::vb {

namespace {
enum : uint16_t {
  INT_SCANERR = 0x0001, INT_LFBEND = 0x0002, INT_RFBEND = 0x0004, INT_GAMESTART = 0x0008, INT_FRAMESTART = 0x0010,
  INT_SBHIT = 0x2000,  /* não é gerado, como no Mednafen */ INT_XPEND = 0x4000, INT_TIMEERR = 0x8000,
};
inline int32_t sx(uint32_t v, int bits) { return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }
}  // namespace

void VIP::reset() {
  std::memset(fb, 0, sizeof fb);
  std::memset(chr, 0, sizeof chr);
  std::memset(dram, 0, sizeof dram);
  std::memset(lum, 0, sizeof lum);
  intPending_ = intEnable_ = 0;
  dpctrl_ = 2; xpctrl_ = 0; frmcyc_ = 0; bkcol_ = 0;
  brta_ = brtb_ = brtc_ = rest_ = 0; sbcmp_ = 0; repeat_ = 0;
  for (int i = 0; i < 4; i++) { spt_[i] = 0; gplt_[i] = 0; jplt_[i] = 0; }
  lastTs_ = 0; sboutUntil_ = -1;
  column_ = 0; columnCounter_ = COLUMN_CYCLES; region_ = 0; gameFrame_ = 0;
  drawCounter_ = 0; drawBlock_ = 0; sbLatch_ = 0;
  displayActive_ = false; displayFb_ = false; drawActive_ = false; drawFb_ = false;
  frameStarted = false;
  recalcBrightness();
}

void VIP::checkIrq() { vb_.setIrq(4, (intPending_ & intEnable_) != 0); }

// brilho de cada nível: o LED fica aceso BRTA (nível 1), BRTA+1+BRTB... ciclos de um período de 128, repetido
// (tabela de colunas) até o limite
void VIP::recalcBrightness() {
  const int cum = brta_ + 1 + brtb_ + 1 + brtc_ + 1 + rest_ + 1 + 1;
  const int maxT = 128;
  int b[4] = {0, 0, 0, 0};
  for (int i = 0; i < repeat_ + 1; i++) {
    const int base = i * cum;
    if (base >= maxT) break;
    auto part = [&](int end, int start) {
      if (end > maxT) end = maxT;
      end -= start;
      return end < 0 ? 0 : end;
    };
    b[1] += part(base + brta_, base);
    b[2] += part(base + brta_ + 1 + brtb_, base + brta_ + 1);
    b[3] += part(base + brta_ + brtb_ + brtc_ + 1, base + 1);
  }
  for (int i = 0; i < 4; i++) {
    const int v = 255 * b[i] / maxT;
    bright_[i] = static_cast<uint8_t>(v > 255 ? 255 : v);
  }
}

// ---------------------------------------------------------------------------------------------- memória
uint8_t VIP::read8(uint32_t a) {
  a &= 0x7FFFF;
  if (a < 0x20000) {
    if ((a & 0x7FFF) >= 0x6000) return chr[((a >> 2) & 0x6000) | (a & 0x1FFF)];
    return fb[(a >> 15) & 1][(a >> 16) & 1][a & 0x7FFF];
  }
  if (a < 0x40000) return dram[a - 0x20000];
  if (a < 0x60000) {
    if (a >= 0x5E000) { const uint16_t v = readReg(a); return static_cast<uint8_t>((a & 1) ? v >> 8 : v); }
    return 0;
  }
  if (a >= 0x78000) return chr[a & 0x7FFF];
  return 0;
}

uint16_t VIP::read16(uint32_t a) {
  a &= 0x7FFFE;
  if (a >= 0x40000 && a < 0x60000) return a >= 0x5E000 ? readReg(a) : 0;
  return static_cast<uint16_t>(read8(a) | (read8(a + 1) << 8));
}

void VIP::write8(uint32_t a, uint8_t v) {
  a &= 0x7FFFF;
  if (a < 0x20000) {
    if ((a & 0x7FFF) >= 0x6000) chr[((a >> 2) & 0x6000) | (a & 0x1FFF)] = v;
    else fb[(a >> 15) & 1][(a >> 16) & 1][a & 0x7FFF] = v;
  } else if (a < 0x40000) {
    dram[a - 0x20000] = v;
  } else if (a < 0x60000) {
    if (a >= 0x5E000) writeReg(a, v);
  } else if (a >= 0x78000) {
    chr[a & 0x7FFF] = v;
  }
}

void VIP::write16(uint32_t a, uint16_t v) {
  a &= 0x7FFFE;
  if (a >= 0x40000 && a < 0x60000) {
    if (a >= 0x5E000) writeReg(a, v);
    return;
  }
  write8(a, static_cast<uint8_t>(v));
  write8(a + 1, static_cast<uint8_t>(v >> 8));
}

// o estado do VIP só avança nos eventos (fim de coluna ou de bloco), como no aparelho visto pela CPU
uint16_t VIP::readReg(uint32_t a) {
  switch (a & 0xFE) {
    case 0x00: return intPending_;
    case 0x02: return intEnable_;
    case 0x20: {  // DPSTTS
      uint16_t v = dpctrl_ & 0x702;
      if ((region_ & 1) && displayActive_) {
        unsigned busy = 1u << ((region_ >> 1) & 1);
        if (displayFb_) busy <<= 2;
        v |= static_cast<uint16_t>(busy << 2);
      }
      return v | 0x40;  // FCLK
    }
    case 0x24: return brta_;
    case 0x26: return brtb_;
    case 0x28: return brtc_;
    case 0x2A: return rest_;
    case 0x30: return 0xFFFF;  // CTA
    case 0x40: {  // XPSTTS
      uint16_t v = xpctrl_ & 2;
      if (drawActive_) v |= static_cast<uint16_t>((1 + drawFb_) << 2);
      if (vb_.ts < sboutUntil_) v |= static_cast<uint16_t>(0x8000 | (sbLatch_ << 8));
      return v;
    }
    case 0x44: return 2;  // versão do VIP
    case 0x48: case 0x4A: case 0x4C: case 0x4E: return spt_[(a >> 1) & 3];
    case 0x60: case 0x62: case 0x64: case 0x66: return gplt_[(a >> 1) & 3];
    case 0x68: case 0x6A: case 0x6C: case 0x6E: return jplt_[(a >> 1) & 3];
    case 0x70: return bkcol_;
    default: return 0;
  }
}

void VIP::writeReg(uint32_t a, uint16_t v) {
  switch (a & 0xFE) {
    case 0x02: intEnable_ = v & 0xE01F; checkIrq(); break;
    case 0x04: intPending_ &= ~v; checkIrq(); break;
    case 0x22:  // DPCTRL
      dpctrl_ = v & 0x703;
      if (v & 1) {
        displayActive_ = false;
        intPending_ &= ~(INT_TIMEERR | INT_FRAMESTART | INT_GAMESTART | INT_RFBEND | INT_LFBEND | INT_SCANERR);
        checkIrq();
      }
      break;
    case 0x24: brta_ = static_cast<uint8_t>(v); recalcBrightness(); break;
    case 0x26: brtb_ = static_cast<uint8_t>(v); recalcBrightness(); break;
    case 0x28: brtc_ = static_cast<uint8_t>(v); recalcBrightness(); break;
    case 0x2A: rest_ = static_cast<uint8_t>(v); recalcBrightness(); break;
    case 0x2E: frmcyc_ = v & 0xF; break;
    case 0x42:  // XPCTRL
      xpctrl_ = v & 2;
      sbcmp_ = static_cast<uint8_t>((v >> 8) & 0x1F);
      if (v & 1) {
        drawFb_ = displayFb_;
        displayFb_ = !displayFb_;
        drawActive_ = false;
        drawCounter_ = 0;
        intPending_ &= ~(INT_SBHIT | INT_XPEND | INT_TIMEERR);
        checkIrq();
      }
      break;
    case 0x48: case 0x4A: case 0x4C: case 0x4E: spt_[(a >> 1) & 3] = v & 0x3FF; break;
    case 0x60: case 0x62: case 0x64: case 0x66: gplt_[(a >> 1) & 3] = v & 0xFC; break;
    case 0x68: case 0x6A: case 0x6C: case 0x6E: jplt_[(a >> 1) & 3] = v & 0xFC; break;
    case 0x70: bkcol_ = v & 3; break;
    default: break;
  }
}

// ---------------------------------------------------------------------------------------------- relógio
// o fim de um bloco de desenho só é notado no fim da coluna seguinte (os dois relógios são vistos juntos)
int64_t VIP::nextEvent() const { return lastTs_ + columnCounter_; }

void VIP::copyColumn(int eye, int column) {
  if (!(column & 3)) {
    // tabela de colunas (de trás para a frente): o byte alto é a repetição do brilho
    const uint32_t off = 0x1DFFE - (column >> 2) * 2 - (eye ? 0 : 0x200);
    const uint8_t rep = dram[off + 1];
    if (rep != repeat_) { repeat_ = rep; recalcBrightness(); }
  }
  uint8_t* dst = &lum[eye][column];
  if (!displayActive_) {
    for (int y = 0; y < H; y++) dst[y * W] = 0;
    return;
  }
  const uint8_t* src = &fb[displayFb_][eye][64 * column];
  for (int y = 0; y < 56; y++) {
    unsigned bits = src[y];
    for (int k = 0; k < 4; k++, bits >>= 2) dst[(y * 4 + k) * W] = bright_[bits & 3];
  }
}

void VIP::update(int64_t ts) {
  int64_t clocks = ts - lastTs_;
  int64_t now = lastTs_;
  while (clocks > 0) {
    int32_t chunk = clocks > columnCounter_ ? columnCounter_ : static_cast<int32_t>(clocks);
    if (drawCounter_ > 0 && chunk > drawCounter_) chunk = drawCounter_;
    now += chunk;
    if (drawCounter_ > 0) {
      drawCounter_ -= chunk;
      if (drawCounter_ <= 0) {
        drawBlock(drawBlock_);
        sboutUntil_ = now + 1120;
        sbLatch_ = drawBlock_;
        drawBlock_++;
        if (drawBlock_ >= 28) {
          drawActive_ = false;
          drawCounter_ = 0;
          drawBlock_ = 0;
          intPending_ |= INT_XPEND;
        } else {
          drawCounter_ += BLOCK_CYCLES;
        }
        checkIrq();
      }
    }
    columnCounter_ -= chunk;
    if (columnCounter_ <= 0) {
      columnCounter_ = COLUMN_CYCLES;
      if (++column_ >= W) {
        column_ = 0;
        if (displayActive_ && (region_ & 1)) {
          intPending_ |= (region_ & 2) ? INT_RFBEND : INT_LFBEND;
          checkIrq();
        }
        region_ = (region_ + 1) & 3;
        if (region_ == 0) {  // começo de um quadro de exibição
          displayActive_ = dpctrl_ & 2;
          if (displayActive_) intPending_ |= INT_FRAMESTART;
          if (++gameFrame_ > frmcyc_) {  // começo de um quadro do jogo: troca os framebuffers e desenha
            intPending_ |= INT_GAMESTART;
            if (xpctrl_ & 2) {
              displayFb_ = drawFb_;
              drawFb_ = !drawFb_;
              drawBlock_ = 0;
              drawActive_ = true;
              drawCounter_ = BLOCK_CYCLES;
            }
            gameFrame_ = 0;
          }
          checkIrq();
          // a imagem que será exibida neste quadro é montada de uma vez (como o "instant display" do
          // Mednafen): mesma imagem do aparelho, um quadro antes, com o brilho do começo do quadro
          const uint8_t keep = repeat_;
          for (int e = 0; e < 2; e++)
            for (int c = 0; c < W; c++) copyColumn(e, c);
          if (repeat_ != keep) { repeat_ = keep; recalcBrightness(); }
          frameStarted = true;
        }
      }
    }
    clocks -= chunk;
  }
  lastTs_ = ts;
}

// ---------------------------------------------------------------------------------------------- desenho
// mundo normal ou H-bias numa linha de um olho
void VIP::drawNormal(uint8_t* line, int y, int eye, uint16_t w0, const uint16_t* wd) {
  const int gy = sx(wd[3], 11);
  const uint16_t height = wd[8] & 0x3FF;
  if (static_cast<uint16_t>(y - gy) > height) return;
  const int gx = sx(wd[1], 11), gp = sx(wd[2], 9), mp = sx(wd[5], 9);
  const uint16_t width = static_cast<uint16_t>(sx(wd[7], 11));
  const bool over = w0 & 0x80;
  const int scy = (w0 >> 8) & 3, scx = (w0 >> 10) & 3;
  uint16_t srcX = static_cast<uint16_t>(wd[4] + (eye ? mp : -mp));
  const uint16_t srcY = static_cast<uint16_t>(wd[6] + (y - gy));
  if (((w0 >> 12) & 3) == 1)  // H-bias: deslocamento horizontal por linha (HOFSTL / HOFSTR)
    srcX = static_cast<uint16_t>(srcX + static_cast<int16_t>(dramH((wd[9] + (y - gy) * 2) | eye)));

  const int destX = sx(static_cast<uint16_t>(gx + (eye ? gp : -gp)), 10);
  uint32_t sourceX = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(srcX)));
  uint32_t sourceY = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(srcY)));
  if (destX < 0) sourceX -= static_cast<uint32_t>(destX);
  int startX = destX < 0 ? 0 : destX;
  int finalX = destX + width;
  if (finalX > W - 1) finalX = W - 1;
  if (startX > finalX) return;

  const uint32_t sizeX = 512u << scx, sizeY = 512u << scy;
  const uint32_t maskX = over ? 0x1FFF : sizeX - 1, maskY = over ? 0x1FFF : sizeY - 1;
  const uint16_t overCell = dramH(wd[10]);
  sourceY &= maskY;
  const bool rowInside = sourceY < sizeY;
  const uint32_t base = (static_cast<uint32_t>(w0 & 0xF) << 12) | (((sourceY >> 3) & 0x3F) * 0x40) |
                        (((sourceY << 3) & ~0xFFFu) << scx);
  const uint32_t rowY = sourceY & 7;
  for (int x = startX; x <= finalX;) {
    sourceX &= maskX;
    const uint16_t cell = (rowInside && sourceX < sizeX)
                              ? dramH(base | ((sourceX << 3) & ~0xFFFu) | ((sourceX >> 3) & 0x3F))
                              : overCell;
    const uint16_t pix = chrRow(cell, (cell & 0x1000) ? 7 - rowY : rowY);
    const uint16_t plt = gplt_[cell >> 14];
    const bool hflip = cell & 0x2000;
    if (!(sourceX & 7) && x + 7 <= finalX) {
      for (int k = 0; k < 8; k++) {
        const unsigned p = (pix >> ((hflip ? 7 - k : k) * 2)) & 3;
        if (p) line[x + k] = static_cast<uint8_t>((plt >> (p * 2)) & 3);
      }
      x += 8;
      sourceX += 8;
    } else {
      const unsigned col = sourceX & 7;
      const unsigned p = (pix >> ((hflip ? 7 - col : col) * 2)) & 3;
      if (p) line[x] = static_cast<uint8_t>((plt >> (p * 2)) & 3);
      x++;
      sourceX++;
    }
  }
}

// mundo afim: cada linha tem MX, MP, MY (13.3), DX e DY (7.9) na tabela de parâmetros
void VIP::drawAffine(uint8_t* line, int y, int eye, uint16_t w0, const uint16_t* wd) {
  const int gy = sx(wd[3], 11);
  const uint16_t height = wd[8] & 0x3FF;
  if (static_cast<uint16_t>(y - gy) > height) return;
  const int gx = sx(wd[1], 11), gp = sx(wd[2], 9);
  const uint16_t width = static_cast<uint16_t>(sx(wd[7], 11));
  const bool over = w0 & 0x80;
  const int scy = (w0 >> 8) & 3, scx = (w0 >> 10) & 3;
  const uint32_t pb = wd[9] + 8u * static_cast<uint32_t>(y - gy);
  const int16_t mx = static_cast<int16_t>(dramH(pb)), mp = static_cast<int16_t>(dramH(pb + 1));
  const int16_t my = static_cast<int16_t>(dramH(pb + 2)), dx = static_cast<int16_t>(dramH(pb + 3));
  const int16_t dy = static_cast<int16_t>(dramH(pb + 4));

  const int destX = sx(static_cast<uint16_t>(gx + (eye ? gp : -gp)), 10);
  uint32_t sourceX = static_cast<uint32_t>(static_cast<int32_t>(mx)) << 6;
  uint32_t sourceY = static_cast<uint32_t>(static_cast<int32_t>(my)) << 6;
  const uint32_t udx = static_cast<uint32_t>(static_cast<int32_t>(dx)), udy = static_cast<uint32_t>(static_cast<int32_t>(dy));
  if (destX < 0) { sourceX += udx * static_cast<uint32_t>(-destX); sourceY += udy * static_cast<uint32_t>(-destX); }
  // paralaxe: o olho do lado do deslocamento começa MP pontos adiante
  if (mp >= 0 && eye) { sourceX += udx * static_cast<uint32_t>(mp); sourceY += udy * static_cast<uint32_t>(mp); }
  else if (mp < 0 && !eye) { sourceX += udx * static_cast<uint32_t>(-mp); sourceY += udy * static_cast<uint32_t>(-mp); }

  const uint32_t sizeX = (512u << scx) << 9, sizeY = (512u << scy) << 9;
  const uint32_t maskX = over ? 0x3FFFFFF : sizeX - 1, maskY = over ? 0x3FFFFFF : sizeY - 1;
  int startX = destX < 0 ? 0 : destX;
  int finalX = destX + width;
  if (finalX > W - 1) finalX = W - 1;
  const uint16_t overCell = dramH(wd[10]);
  const uint32_t base = static_cast<uint32_t>(w0 & 0xF) << 12;
  for (int x = startX; x <= finalX; x++) {
    const uint32_t px = sourceX & maskX, py = sourceY & maskY;
    sourceX += udx;
    sourceY += udy;
    uint16_t cell = overCell;
    if (px < sizeX && py < sizeY)
      cell = dramH(base | (((px >> 6) & ~0xFFFu) + (((py >> 6) & ~0xFFFu) << scx) + ((px >> 12) & 0x3F) +
                           ((py >> 12) & 0x3F) * 0x40));
    const unsigned row = ((cell & 0x1000) ? 7 : 0) ^ ((py >> 9) & 7);
    const unsigned col = ((cell & 0x2000) ? 7 : 0) ^ ((px >> 9) & 7);
    const unsigned p = (chrRow(cell, row) >> (col * 2)) & 3;
    if (p) line[x] = static_cast<uint8_t>((gplt_[cell >> 14] >> (p * 2)) & 3);
  }
}

// grupo de objetos (SPTn até SPTn-1 + 1, do maior para o menor índice: o menor fica na frente)
void VIP::drawObjects(int y, int group, const bool on[2]) {
  const int yb = y & 7;
  int oam = spt_[group];
  const int end = group ? spt_[group - 1] : 1023;
  do {
    const uint32_t o = (0x1E000 >> 1) + oam * 4;
    const uint16_t jy = dramH(o + 2);
    const unsigned tileY = static_cast<unsigned>(y - jy) & 0xFF;
    if (tileY < 8) {
      const uint16_t w1 = dramH(o + 1), w3 = dramH(o + 3);
      const uint16_t jx = dramH(o);
      const uint16_t jp = w1 & 0x3FFF;
      const uint16_t pix = chrRow(w3, (w3 & 0x1000) ? 7 - tileY : tileY);
      const uint16_t plt = jplt_[w3 >> 14];
      const bool hflip = w3 & 0x2000;
      for (int eye = 0; eye < 2; eye++) {
        if (!on[eye] || !(w1 & (eye ? 0x4000 : 0x8000))) continue;
        const int x = sx(static_cast<uint32_t>(jx + (eye ? jp : -jp)), 10);
        if (x < -7 || x >= W) continue;
        uint8_t* t = &line_[eye][yb][8 + x];
        for (int k = 0; k < 8; k++) {
          const unsigned p = (pix >> ((hflip ? 7 - k : k) * 2)) & 3;
          if (p) t[k] = static_cast<uint8_t>((plt >> (p * 2)) & 3);
        }
      }
    }
    oam = (oam - 1) & 1023;
  } while (oam != end);
}

void VIP::drawBlock(int block) {
  for (int e = 0; e < 2; e++)
    for (int y = 0; y < 8; y++) std::memset(line_[e][y], bkcol_, sizeof line_[e][y]);
  int group = 3;
  for (int world = 31; world >= 0; world--) {
    uint16_t wd[11];
    for (int k = 0; k < 11; k++) wd[k] = dramH((0x1D800 >> 1) + world * 16 + k);
    const uint16_t w0 = wd[0];
    if (w0 & 0x40) break;  // END
    const int bgm = (w0 >> 12) & 3;
    const bool on[2] = {(w0 & 0x8000) != 0, (w0 & 0x4000) != 0};
    if (on[0] || on[1]) {
      for (int y = 0; y < 8; y++) {
        const int ry = block * 8 + y;
        if (bgm == 3) { drawObjects(ry, group, on); continue; }
        for (int e = 0; e < 2; e++) {
          if (!on[e]) continue;
          if (bgm == 2) drawAffine(line_[e][y] + 8, ry, e, w0, wd);
          else drawNormal(line_[e][y] + 8, ry, e, w0, wd);
        }
      }
    }
    if (bgm == 3 && group > 0) group--;
  }
  // empacota as 8 linhas no framebuffer (2 bits por ponto, colunas de 64 bytes)
  for (int e = 0; e < 2; e++) {
    uint8_t* t = fb[drawFb_][e] + block * 2;
    for (int x = 0; x < W; x++) {
      const int c = x + 8;
      t[64 * x] = static_cast<uint8_t>(line_[e][0][c] | (line_[e][1][c] << 2) | (line_[e][2][c] << 4) | (line_[e][3][c] << 6));
      t[64 * x + 1] = static_cast<uint8_t>(line_[e][4][c] | (line_[e][5][c] << 2) | (line_[e][6][c] << 4) | (line_[e][7][c] << 6));
    }
  }
}

// ---------------------------------------------------------------------------------------------- estado
void VIP::sync(StateIO& io) {
  io.raw(fb, sizeof fb);
  io.raw(chr, sizeof chr);
  io.raw(dram, sizeof dram);
  io.raw(lum, sizeof lum);
  io.sync(intPending_); io.sync(intEnable_); io.sync(dpctrl_); io.sync(xpctrl_); io.sync(frmcyc_); io.sync(bkcol_);
  io.sync(brta_); io.sync(brtb_); io.sync(brtc_); io.sync(rest_); io.sync(sbcmp_); io.sync(repeat_);
  io.raw(spt_, sizeof spt_); io.raw(gplt_, sizeof gplt_); io.raw(jplt_, sizeof jplt_);
  io.sync(lastTs_); io.sync(sboutUntil_);
  io.sync(column_); io.sync(columnCounter_); io.sync(region_); io.sync(gameFrame_);
  io.sync(drawCounter_); io.sync(drawBlock_); io.sync(sbLatch_);
  io.sync(displayActive_); io.sync(displayFb_); io.sync(drawActive_); io.sync(drawFb_);
  if (io.reading()) {
    intPending_ &= 0xE01F; intEnable_ &= 0xE01F; dpctrl_ &= 0x703; xpctrl_ &= 2; frmcyc_ &= 0xF; bkcol_ &= 3;
    sbcmp_ &= 0x1F;
    for (int i = 0; i < 4; i++) { spt_[i] &= 0x3FF; gplt_[i] &= 0xFC; jplt_[i] &= 0xFC; }
    clampTo(column_, 0, W - 1);
    clampTo(columnCounter_, 1, COLUMN_CYCLES);
    region_ &= 3;
    clampTo(gameFrame_, 0, 16);
    clampTo(drawCounter_, 0, BLOCK_CYCLES);
    clampTo(drawBlock_, 0, 27);
    clampTo(sbLatch_, 0, 27);
    recalcBrightness();
  }
}

}  // namespace indigo::vb
