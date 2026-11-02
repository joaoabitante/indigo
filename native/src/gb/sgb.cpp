#include "sgb.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <stdexcept>

#include "ppu.h"

namespace indigo::gb {

namespace {
std::atomic<bool> g_enabled{true};
const char SGB_MAGIC[4] = {'S', 'G', 'B', '1'};
constexpr int TRN_SIZE = 4096;
// paleta inicial (tons de sépia parecidos com os do aparelho, em BGR555)
inline uint16_t bgr(int r, int g, int b) { return static_cast<uint16_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)); }
}  // namespace

void setSgbEnabled(bool on) { g_enabled = on; }
bool sgbEnabled() { return g_enabled; }
bool isSgbCart(const std::vector<uint8_t>& rom) { return rom.size() >= 0x150 && rom[0x146] == 0x03 && rom[0x14B] == 0x33; }

SGB::SGB() : sysPal(TRN_SIZE), atf(TRN_SIZE), chr(256 * 32), pct(TRN_SIZE) { reset(); }

void SGB::reset() {
  const uint16_t def[4] = {bgr(0xF8, 0xE8, 0xC8), bgr(0xD8, 0x90, 0x48), bgr(0xA8, 0x28, 0x20), bgr(0x30, 0x18, 0x50)};
  for (auto& p : pal) std::copy(def, def + 4, p);
  std::memset(attr, 0, sizeof attr);
  mask = 0; players = 1; player = 0; lastCmd = -1; cmdCount = 0;
  std::memset(buf, 0, sizeof buf);
  bit = 0; packet = 0; receiving = false; armed = false; lastJoyp = 0x30;
  trnCmd = -1; trnParam = 0; trnWait = 0;
  std::fill(sysPal.begin(), sysPal.end(), 0);
  std::fill(atf.begin(), atf.end(), 0);
  // moldura própria até o jogo mandar a dele: fundo escuro e um filete claro em volta da tela
  std::fill(chr.begin(), chr.end(), 0);
  for (int r = 0; r < 8; r++) { chr[32 + 2 * r] = 0xFF; chr[64 + 2 * r + 1] = 0xFF; }  // tile 1 = cor 1, tile 2 = cor 2
  std::fill(pct.begin(), pct.end(), 0);
  for (int ty = 0; ty < 28; ty++)
    for (int tx = 0; tx < 32; tx++) {
      const bool inside = tx >= 6 && tx < 26 && ty >= 5 && ty < 23;
      const bool ring = !inside && tx >= 5 && tx < 27 && ty >= 4 && ty < 24;
      const int tile = inside ? 0 : ring ? 1 : 2;
      pct[(ty * 32 + tx) * 2] = static_cast<uint8_t>(tile);
      pct[(ty * 32 + tx) * 2 + 1] = 4 << 2;  // paleta 4
    }
  const uint16_t c1 = bgr(0x90, 0x98, 0xB0), c2 = bgr(0x28, 0x2C, 0x40);
  pct[0x802] = c1 & 0xFF; pct[0x803] = c1 >> 8;
  pct[0x804] = c2 & 0xFF; pct[0x805] = c2 >> 8;
  borderDirty = true;
  std::fill(gbLayer, gbLayer + 160 * 144, color(pal[0][0]));
  std::fill(out, out + W * H, 0xFF000000u);
}

uint32_t SGB::color(int c) const { return PPU::rgb(c); }

// ------------------------------------------------------------ pacotes
void SGB::writeJoyp(int v, int ly, bool lcdOn) {
  v &= 0x30;
  // vários jogadores: o próximo controle é escolhido quando P15 sobe
  if (players > 1 && (v & 0x20) && !(lastJoyp & 0x20)) player = (player + 1) & (players - 1);
  lastJoyp = v;
  if (v == 0x00) {  // pulso de início de pacote
    receiving = true; armed = false; bit = 0;
    if (packet == 0) std::memset(buf, 0, sizeof buf);
    else std::memset(buf + packet * 16, 0, 16);
    return;
  }
  if (v == 0x30) { if (receiving) armed = true; return; }
  if (!receiving || !armed) return;
  armed = false;
  const bool one = v == 0x10;
  if (bit < 128) {
    if (one) buf[packet * 16 + (bit >> 3)] |= static_cast<uint8_t>(1 << (bit & 7));
    bit++;
    return;
  }
  // bit de parada (tem de ser 0)
  receiving = false;
  if (one) { packet = 0; return; }
  packet++;
  const int total = std::max(1, buf[0] & 7);
  if (packet < total) return;
  packet = 0;
  // os pacotes do boot ($F1, $F3...) só levam o cabeçalho do cartucho
  if ((buf[0] & 0xF1) == 0xF1) return;
  command();
  if (trnCmd >= 0 && trnWait == 0) trnWait = (!lcdOn || ly >= 144) ? 1 : 2;
}

void SGB::command() {
  const int cmd = buf[0] >> 3;
  lastCmd = cmd; cmdCount++;
  switch (cmd) {
    case 0x00: setPalPair(0, 1); break;  // PAL01
    case 0x01: setPalPair(2, 3); break;  // PAL23
    case 0x02: setPalPair(0, 3); break;  // PAL03
    case 0x03: setPalPair(1, 2); break;  // PAL12
    case 0x04: attrBlk(); break;
    case 0x05: attrLin(); break;
    case 0x06: attrDiv(); break;
    case 0x07: attrChr(); break;
    case 0x0A: {  // PAL_SET
      for (int i = 0; i < 4; i++) {
        const int id = (buf[1 + 2 * i] | (buf[2 + 2 * i] << 8)) & 511;
        for (int k = 0; k < 4; k++) pal[i][k] = static_cast<uint16_t>(sysPal[id * 8 + k * 2] | (sysPal[id * 8 + k * 2 + 1] << 8));
      }
      for (int i = 1; i < 4; i++) pal[i][0] = pal[0][0];
      if (buf[9] & 0x80) applyAtf(buf[9] & 0x3F);
      if (buf[9] & 0x40) mask = 0;
      break;
    }
    case 0x0B: case 0x13: case 0x14: case 0x15:  // PAL_TRN, CHR_TRN, PCT_TRN, ATTR_TRN
      trnCmd = cmd; trnParam = buf[1]; trnWait = 0;
      break;
    case 0x11: {  // MLT_REQ
      const int m = buf[1] & 3;
      players = m == 3 ? 4 : (m & 1) ? 2 : 1;
      player &= players - 1;
      break;
    }
    case 0x16:  // ATTR_SET
      applyAtf(buf[1] & 0x3F);
      if (buf[1] & 0x40) mask = 0;
      break;
    case 0x17: mask = buf[1] & 3; break;  // MASK_EN
    default: break;  // som, ícones, dados para a RAM do SNES etc.: sem efeito aqui
  }
}

void SGB::setPalPair(int p0, int p1) {
  auto rd = [this](int i) { return static_cast<uint16_t>((buf[i] | (buf[i + 1] << 8)) & 0x7FFF); };
  const uint16_t c0 = rd(1);
  for (auto& p : pal) p[0] = c0;
  for (int k = 0; k < 3; k++) { pal[p0][k + 1] = rd(3 + 2 * k); pal[p1][k + 1] = rd(9 + 2 * k); }
}

void SGB::attrBlk() {
  const int n = std::min<int>(buf[1], 18);
  for (int s = 0; s < n; s++) {
    const uint8_t* d = buf + 2 + s * 6;
    int ctl = d[0] & 7;
    const int pin = d[1] & 3, pline = (d[1] >> 2) & 3, pout = (d[1] >> 4) & 3;
    const int x1 = d[2] & 31, y1 = d[3] & 31, x2 = d[4] & 31, y2 = d[5] & 31;
    int lineP = pline;
    // só dentro ou só fora: a linha em volta fica com a mesma paleta
    if (ctl == 1) { ctl |= 2; lineP = pin; }
    else if (ctl == 4) { ctl |= 2; lineP = pout; }
    for (int y = 0; y < 18; y++)
      for (int x = 0; x < 20; x++) {
        const bool in = x > x1 && x < x2 && y > y1 && y < y2;
        const bool on = !in && x >= x1 && x <= x2 && y >= y1 && y <= y2;
        if (in && (ctl & 1)) attr[y * 20 + x] = static_cast<uint8_t>(pin);
        else if (on && (ctl & 2)) attr[y * 20 + x] = static_cast<uint8_t>(lineP);
        else if (!in && !on && (ctl & 4)) attr[y * 20 + x] = static_cast<uint8_t>(pout);
      }
  }
}

void SGB::attrLin() {
  const int n = std::min<int>(buf[1], 110);
  for (int s = 0; s < n; s++) {
    const int d = buf[2 + s];
    const int line = d & 31, p = (d >> 5) & 3;
    if (d & 0x80) { if (line < 18) for (int x = 0; x < 20; x++) attr[line * 20 + x] = static_cast<uint8_t>(p); }
    else if (line < 20) for (int y = 0; y < 18; y++) attr[y * 20 + line] = static_cast<uint8_t>(p);
  }
}

void SGB::attrDiv() {
  const int d = buf[1], c = buf[2] & 31;
  const int pRight = d & 3, pLeft = (d >> 2) & 3, pLine = (d >> 4) & 3;
  const bool horiz = d & 0x40;
  for (int y = 0; y < 18; y++)
    for (int x = 0; x < 20; x++) {
      const int k = horiz ? y : x;
      attr[y * 20 + x] = static_cast<uint8_t>(k < c ? pLeft : k == c ? pLine : pRight);
    }
}

void SGB::attrChr() {
  int x = buf[1] % 20, y = buf[2] % 18;
  const int n = std::min(buf[3] | (buf[4] << 8), 360);
  const bool down = buf[5] & 1;
  for (int s = 0; s < n && 6 + (s >> 2) < static_cast<int>(sizeof buf); s++) {
    const int p = (buf[6 + (s >> 2)] >> (6 - 2 * (s & 3))) & 3;
    attr[y * 20 + x] = static_cast<uint8_t>(p);
    if (down) { if (++y == 18) { y = 0; x = (x + 1) % 20; } }
    else if (++x == 20) { x = 0; y = (y + 1) % 18; }
  }
}

void SGB::applyAtf(int n) {
  if (n > 44) return;
  const uint8_t* f = atf.data() + n * 90;
  for (int i = 0; i < 360; i++) attr[i] = (f[i >> 2] >> (6 - 2 * (i & 3))) & 3;
}

// ------------------------------------------------------------ transferências pela imagem
void SGB::transfer(const uint8_t* shade) {
  // a imagem mostrada volta a ser os bytes de 8000-8FFF (tiles 0-255 em ordem, 20 por linha)
  uint8_t data[TRN_SIZE];
  for (int t = 0; t < 256; t++) {
    const int tx = t % 20, ty = t / 20;
    for (int r = 0; r < 8; r++) {
      const uint8_t* px = shade + (ty * 8 + r) * 160 + tx * 8;
      int lo = 0, hi = 0;
      for (int k = 0; k < 8; k++) { lo = (lo << 1) | (px[k] & 1); hi = (hi << 1) | ((px[k] >> 1) & 1); }
      data[t * 16 + r * 2] = static_cast<uint8_t>(lo);
      data[t * 16 + r * 2 + 1] = static_cast<uint8_t>(hi);
    }
  }
  switch (trnCmd) {
    case 0x0B: std::copy(data, data + TRN_SIZE, sysPal.begin()); break;
    case 0x13: std::copy(data, data + TRN_SIZE, chr.begin() + ((trnParam & 1) ? TRN_SIZE : 0)); borderDirty = true; break;
    case 0x14: std::copy(data, data + TRN_SIZE, pct.begin()); borderDirty = true; break;
    case 0x15: std::copy(data, data + TRN_SIZE, atf.begin()); break;
  }
  trnCmd = -1;
}

void SGB::drawBorder() {
  borderDirty = false;
  for (int ty = 0; ty < 28; ty++)
    for (int tx = 0; tx < 32; tx++) {
      const int e = pct[(ty * 32 + tx) * 2] | (pct[(ty * 32 + tx) * 2 + 1] << 8);
      const uint8_t* t = chr.data() + (e & 0xFF) * 32;
      const int pn = ((e >> 10) - 4) & 3;
      const uint8_t* pd = pct.data() + 0x800 + pn * 32;
      for (int r = 0; r < 8; r++) {
        const int sr = (e & 0x8000) ? 7 - r : r;
        const int p0 = t[sr * 2], p1 = t[sr * 2 + 1], p2 = t[16 + sr * 2], p3 = t[17 + sr * 2];
        for (int k = 0; k < 8; k++) {
          const int b = (e & 0x4000) ? k : 7 - k;
          const int ci = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3);
          const int i = (ty * 8 + r) * W + tx * 8 + k;
          borderOn[i] = ci != 0;
          border[i] = ci ? color(pd[ci * 2] | (pd[ci * 2 + 1] << 8)) : 0;
        }
      }
    }
}

void SGB::frameEnd(const uint8_t* shade, bool lcdOn) {
  if (trnCmd >= 0 && trnWait > 0 && --trnWait == 0) transfer(shade);
  // a tela do Game Boy (congelada com MASK_EN 1 ou com o LCD desligado)
  if (mask == 2 || mask == 3) {
    std::fill(gbLayer, gbLayer + 160 * 144, mask == 2 ? 0xFF000000u : color(pal[0][0]));
  } else if (mask == 0 && lcdOn) {
    uint32_t lut[4][4];
    for (int p = 0; p < 4; p++)
      for (int c = 0; c < 4; c++) lut[p][c] = color(c ? pal[p][c] : pal[0][0]);
    for (int y = 0; y < 144; y++)
      for (int x = 0; x < 160; x++) gbLayer[y * 160 + x] = lut[attr[(y >> 3) * 20 + (x >> 3)]][shade[y * 160 + x] & 3];
  }
  if (borderDirty) drawBorder();
  const uint32_t back = color(pal[0][0]);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const int i = y * W + x;
      if (borderOn[i]) out[i] = border[i];
      else if (x >= GX && x < GX + 160 && y >= GY && y < GY + 144) out[i] = gbLayer[(y - GY) * 160 + x - GX];
      else out[i] = back;
    }
}

// ------------------------------------------------------------ save state
void SGB::sync(StateIO& io) {
  char magic[4];
  std::copy(SGB_MAGIC, SGB_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, SGB_MAGIC)) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  io.raw(pal, sizeof pal);
  io.raw(attr, sizeof attr);
  io.raw(buf, sizeof buf);
  for (int* p : {&mask, &players, &player, &lastCmd, &cmdCount, &bit, &packet, &lastJoyp, &trnCmd, &trnParam, &trnWait}) io.sync(*p);
  io.sync(receiving); io.sync(armed);
  io.bytes(sysPal); io.bytes(atf); io.bytes(chr); io.bytes(pct);
  io.raw(gbLayer, sizeof gbLayer);
  if (io.reading()) {
    for (auto& p : pal) for (auto& c : p) c &= 0x7FFF;
    for (auto& a : attr) a &= 3;
    mask &= 3;
    players = players == 4 ? 4 : players == 2 ? 2 : 1;
    player &= players - 1;
    clampTo(bit, 0, 128); clampTo(packet, 0, 6); lastJoyp &= 0x30;
    if (trnCmd != 0x0B && trnCmd != 0x13 && trnCmd != 0x14 && trnCmd != 0x15) trnCmd = -1;
    clampTo(trnWait, 0, 2);
    for (auto& c : gbLayer) c |= 0xFF000000u;
    borderDirty = true;
  }
}

}  // namespace indigo::gb
