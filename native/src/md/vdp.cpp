#include "vdp.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>

namespace indigo::md {

namespace {
// níveis de cada componente (0..14) em 8 bits; o realce chega a 14 também
inline uint32_t level8(int l) { return static_cast<uint32_t>((l * 255 + 7) / 14); }
inline uint32_t rgb(int r, int g, int b) { return 0xFF000000u | (level8(b) << 16) | (level8(g) << 8) | level8(r); }
constexpr int OFS = 32;  // folga no começo dos buffers de linha
// Janelas de acesso externo (CPU/DMA) à VRAM durante a tela ativa. Fonte: o mapa de slots de acesso do VDP
// medido por Nemesis (fórum SpritesMind, "VDP VRAM access timing", gendev.spritesmind.net/forum/viewtopic.php?t=851)
// e as taxas de DMA do "Genesis Software Manual" da Sega (16 palavras por linha em H32, 18 em H40 com a tela
// ligada). O padrão: na parte visível da linha o VDP dá uma janela externa a cada 2 células (16 pontos), em grupos
// de três, e a quarta posição de cada grupo vai para o refresh da DRAM; o resto das janelas cai no apagamento
// horizontal. As posições estão em ciclos mestres contados a partir do começo de linha usado em md.cpp (H32: 10
// ciclos por ponto; H40: 8 ciclos por ponto na parte visível).
struct AccessSlots {
  int at[64];
  int count = 0;
  // grid: primeira janela da parte visível, distância de 16 pontos, número de grupos de 3; extra: janelas do
  // apagamento horizontal. A tabela cobre a linha atual e a seguinte (uma escrita pode cair na linha seguinte).
  AccessSlots(int first, int sixteenDots, int groups, std::initializer_list<int> extra) {
    int line[32], n = 0;
    for (int g = 0; g < groups; g++)
      for (int k = 0; k < 3; k++) line[n++] = first + (g * 4 + k) * sixteenDots;
    for (int e : extra) line[n++] = e;
    std::sort(line, line + n);
    for (int rep = 0; rep < 2; rep++)
      for (int i = 0; i < n; i++) at[count++] = line[i] + rep * VDP::MCLK_PER_LINE;
  }
};
const AccessSlots SLOTS_H32(810, 160, 4, {230, 510, 3350, 3370});  // 12 + 4 = 16 janelas
const AccessSlots SLOTS_H40(820, 128, 5, {352, 3364, 3380});        // 15 + 3 = 18 janelas
}  // namespace

VDP::VDP(VdpHost& h) : vram(0x10000), host(h) { reset(false); }

void VDP::reset(bool isPal) {
  pal = isPal;
  std::fill(vram.begin(), vram.end(), 0);
  std::memset(cram, 0, sizeof cram); std::memset(vsram, 0, sizeof vsram); std::memset(reg, 0, sizeof reg);
  std::memset(satCache_, 0, sizeof satCache_);
  pending_ = false; code_ = 0; addr_ = 0; fillPending_ = false;
  status_ = 0x3400 | (pal ? 1 : 0);
  vintPending_ = hintPending_ = false;
  hcount_ = 0; line_ = 0; oddField_ = false; lastLineDotOverflow_ = false;
  dmaLeft_ = dmaDone_ = 0;
  dmaBusyUntil_ = 0; stall_ = 0; readBuf_ = 0; lineStart_ = 0; fifoIdx_ = 0;
  for (auto& f : fifo_) f = 0;
  for (int i = 0; i < 64; i++) updateColor(i);
  std::fill(std::begin(fb), std::end(fb), 0xFF000000u);
  outW = 256; outH = 192;
}

void VDP::updateColor(int i) {
  const int c = cram[i];
  const int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
  lut_[0][i] = rgb(r * 2, g * 2, b * 2);
  lut_[1][i] = rgb(r, g, b);
  lut_[2][i] = rgb(r + 7, g + 7, b + 7);
}

// ------------------------------------------------------------ portas
void VDP::writeReg(int r, int v) {
  if (r > 23) return;
  if (mode4() && r > 10) return;  // no modo 4 os registradores 11 a 23 não mudam
  reg[r] = static_cast<uint8_t>(v);
}

void VDP::vramWrite(uint32_t a, uint8_t v) {
  a &= 0xFFFF;
  vram[a] = v;
  const uint32_t sat = (reg[5] & (h40() ? 0x7E : 0x7F)) << 9;
  const uint32_t off = (a - sat) & 0xFFFF;
  if (off < 0x400 && (off & 7) < 4) satCache_[off] = v;
}

void VDP::portWrite(uint16_t v) {
  switch (code_ & 0xF) {
    case 1: {
      const uint16_t w = (addr_ & 1) ? static_cast<uint16_t>((v >> 8) | (v << 8)) : v;
      vramWrite(addr_ & 0xFFFE, static_cast<uint8_t>(w >> 8));
      vramWrite((addr_ & 0xFFFE) | 1, static_cast<uint8_t>(w));
      break;
    }
    case 3: { const int i = (addr_ >> 1) & 0x3F; cram[i] = v & 0xEEE; updateColor(i); break; }
    case 5: { const int i = (addr_ >> 1) & 0x3F; if (i < 40) vsram[i] = v & 0x7FF; break; }
    default: break;
  }
  addr_ = static_cast<uint16_t>(addr_ + reg[15]);
}

void VDP::writeData(uint16_t v, int64_t now) {
  advanceDma(now);
  pending_ = false;
  // fila de 4 escritas: na tela ativa o VDP só aceita uma palavra em certos momentos da linha; com a fila
  // cheia a CPU espera
  if (!(status_ & 8) && (reg[1] & 0x40) && !mode4()) {
    int64_t c = now;
    if (c < fifo_[(fifoIdx_ + 3) & 3]) {
      if (c < fifo_[fifoIdx_]) stall_ += (fifo_[fifoIdx_] + 6) / 7 * 7 - now;
      c = fifo_[(fifoIdx_ + 3) & 3];
    }
    const AccessSlots& S = h40() ? SLOTS_H40 : SLOTS_H32;
    const int* T = S.at;
    const int n = S.count;
    const int64_t rel = c - lineStart_;
    int slot = 0;
    while (slot < n - 2 && rel >= T[slot]) slot++;
    const int byteAccess = (code_ & 0xF) < 3 ? 1 : 0;
    fifo_[fifoIdx_] = lineStart_ + T[slot + byteAccess];
  }
  fifoIdx_ = (fifoIdx_ + 1) & 3;
  portWrite(v);
  if (fillPending_) { fillPending_ = false; dmaFill(v, now); }
}

void VDP::writeCtrl(uint16_t v, int64_t now) {
  advanceDma(now);
  if (pending_) {
    pending_ = false;
    addr_ = static_cast<uint16_t>((addr_ & 0x3FFF) | ((v & 3) << 14));
    code_ = static_cast<uint8_t>((code_ & 3) | ((v >> 2) & 0x3C));
    if ((code_ & 0x20) && (reg[1] & 0x10)) {
      switch (reg[23] >> 6) {
        case 2: fillPending_ = true; dmaBusyUntil_ = INT64_MAX; break;
        case 3: dmaCopy(now); break;
        default: dma68k(now); break;
      }
    }
    return;
  }
  if ((v & 0xC000) == 0x8000) { writeReg((v >> 8) & 0x1F, v & 0xFF); return; }
  pending_ = true;
  addr_ = static_cast<uint16_t>((addr_ & 0xC000) | (v & 0x3FFF));
  code_ = static_cast<uint8_t>((code_ & 0x3C) | (v >> 14));
}

uint16_t VDP::readData() {
  pending_ = false;
  uint16_t v = readBuf_;
  switch (code_ & 0xF) {
    case 0: v = static_cast<uint16_t>((vram[addr_ & 0xFFFE] << 8) | vram[addr_ | 1]); break;
    case 4: { const int i = (addr_ >> 1) & 0x3F; v = static_cast<uint16_t>((readBuf_ & 0xF800) | (vsram[i < 40 ? i : 0] & 0x7FF)); break; }
    case 8: v = static_cast<uint16_t>((readBuf_ & 0xF111) | cram[(addr_ >> 1) & 0x3F]); break;
    case 0xC: v = static_cast<uint16_t>((readBuf_ & 0xFF00) | vram[addr_ ^ 1]); break;
    default: break;
  }
  readBuf_ = v;
  addr_ = static_cast<uint16_t>(addr_ + reg[15]);
  return v;
}

uint16_t VDP::readStatus(int64_t now, int mclk) {
  pending_ = false;
  uint16_t s = static_cast<uint16_t>(status_ & ~0x0316);
  if (now >= fifo_[(fifoIdx_ + 3) & 3]) s |= 0x200;
  else if (now < fifo_[fifoIdx_]) s |= 0x100;
  if (now < dmaBusyUntil_) s |= 2;
  if (!(reg[1] & 0x40)) s |= 8;
  if (interlace2() || (reg[12] & 2)) { if (oddField_) s |= 0x10; }
  if (h40() ? (mclk >= 228 && mclk < 872) : (mclk >= 280 && mclk < 860)) s |= 4;
  status_ &= 0xFF9F;
  return s;
}

uint16_t VDP::hvCounter(int mclk) const {
  int hc;
  if (h40()) {
    // H40: 16 ciclos por contagem, mas na sincronia horizontal (0xE6-0xF7) o relógio dos pontos é mais lento
    if (mclk < 272) hc = 0xA5 + mclk / 16;
    else if (mclk < 280) hc = 0xB6;
    else if (mclk < 288) hc = 0xE4;
    else if (mclk < 304) hc = 0xE5;
    else if (mclk < 652) hc = 0xE6 + (mclk - 304) * 18 / 348;
    else hc = (0xF8 + (mclk - 652) / 16) & 0xFF;
  } else {
    const int i = (133 + mclk * 171 / MCLK_PER_LINE) % 171;
    hc = i < 148 ? i : i + 0x55;
  }
  // a contagem vertical muda no começo da linha (em hc 0xA5/0x85), que é o mclk 0 da nossa linha
  int v = line_;
  const int total = linesPerFrame();
  const int jump = mode4() ? (pal ? 0xF2 : 0xDA) : pal ? ((reg[1] & 8) ? 0x10A : 0x102) : ((reg[1] & 8) ? 0x1FF : 0xEA);
  if (v > jump) v = v - total + 0x100;
  v &= 0x1FF;
  if (interlace2()) v = ((v << 1) | (v >> 8)) & 0xFF;
  else if (reg[12] & 2) v = (v & 0xFE) | ((v >> 8) & 1);
  else v &= 0xFF;
  return static_cast<uint16_t>((v << 8) | (hc & 0xFF));
}

// ------------------------------------------------------------ DMA
// tempo que o DMA leva: acessos por linha (tabela de capacidade de DMA do "Genesis Software Manual" da Sega e o
// mapa de slots acima): 68k->VDP 16/18 na tela e
// 166/204 fora dela (VRAM gasta 2 acessos por palavra; CRAM/VSRAM fora da tela perde os de atualização),
// preenchimento igual em bytes, cópia metade disso
int64_t VDP::dmaEnd(int64_t now, int units, int type) const { return dmaEndFrom(line_, lineStart_, now, units, type); }

int64_t VDP::dmaEndFrom(int line, int64_t lineStart, int64_t now, int units, int type) const {
  int64_t t = now;
  int64_t pos = now - lineStart;
  if (pos >= MCLK_PER_LINE) {
    const int64_t k = pos / MCLK_PER_LINE;
    pos -= k * MCLK_PER_LINE;
    line = static_cast<int>((line + k % linesPerFrame()) % linesPerFrame());
  }
  if (pos < 0) pos = 0;
  if (line < 0 || line >= linesPerFrame()) line = 0;
  int64_t remaining = units;
  for (int guard = 0; guard < 4000 && remaining > 0; guard++) {
    const bool blank = line >= activeLines() || !(reg[1] & 0x40);
    int rate;
    rate = h40() ? (blank ? 204 : 18) : (blank ? 166 : 16);
    if (type == 1 || type == 3) rate >>= 1;
    else if (type == 0 && blank) rate = h40() ? 198 : 161;
    const int64_t left = MCLK_PER_LINE - pos;
    const int64_t cap = left * rate / MCLK_PER_LINE;
    if (remaining <= cap) { t += remaining * MCLK_PER_LINE / rate; break; }
    remaining -= cap;
    t += left;
    pos = 0;
    line = (line + 1) % linesPerFrame();
  }
  return t;
}

// DMA 68k->VDP: a CPU fica parada até o fim; as palavras chegam à VRAM no ritmo do VDP (uma imagem desenhada
// durante um DMA longo mostra o que já chegou, como no aparelho)
void VDP::dma68k(int64_t now) {
  advanceDma(now);
  const int len = dmaLength();
  dmaSrc_ = static_cast<uint32_t>(reg[21] | (reg[22] << 8) | ((reg[23] & 0x7F) << 16)) << 1;
  dmaLeft_ = len; dmaDone_ = 0;
  dmaT0_ = now; dmaL0_ = line_; dmaLS0_ = lineStart_;
  dmaType_ = (code_ & 0xF) == 1 ? 1 : 0;
  uint32_t end = dmaSrc_;
  for (int i = 0; i < len; i++) end = (end & 0xFE0000) | ((end + 2) & 0x1FFFF);
  reg[19] = reg[20] = 0;
  const uint32_t s = end >> 1;
  reg[21] = static_cast<uint8_t>(s); reg[22] = static_cast<uint8_t>(s >> 8);
  reg[23] = static_cast<uint8_t>((reg[23] & 0x80) | ((s >> 16) & 0x7F));
  const int64_t fim = dmaEndFrom(dmaL0_, dmaLS0_, now, len, dmaType_);
  stall_ += fim - now;  // a CPU fica parada enquanto o VDP usa o barramento
  advanceDma(now);
}

// o VDP adianta o DMA até o fim da linha atual (ou do período de vblank) de uma vez (simplificação: a imagem
// é desenhada linha a linha, então andar palavra a palavra dentro da linha não muda o que aparece)
int64_t VDP::dmaHorizon() const {
  if (status_ & 8) {
    const int left = linesPerFrame() - 1 - line_;
    return lineStart_ + static_cast<int64_t>(left > 0 ? left : 1) * MCLK_PER_LINE;
  }
  return lineStart_ + MCLK_PER_LINE;
}

void VDP::advanceDma(int64_t now) {
  if (dmaLeft_ <= 0) return;
  if (now < dmaHorizon()) now = dmaHorizon();
  const int total = dmaDone_ + dmaLeft_;
  int k;
  if (dmaEndFrom(dmaL0_, dmaLS0_, dmaT0_, total, dmaType_) <= now) k = total;
  else {
    int lo = dmaDone_, hi = total;  // maior k com fim(k) <= now
    while (lo < hi) {
      const int mid = (lo + hi + 1) / 2;
      if (dmaEndFrom(dmaL0_, dmaLS0_, dmaT0_, mid, dmaType_) <= now) lo = mid; else hi = mid - 1;
    }
    k = lo;
  }
  for (; dmaDone_ < k; dmaDone_++, dmaLeft_--) {
    portWrite(host.dmaRead(dmaSrc_));
    dmaSrc_ = (dmaSrc_ & 0xFE0000) | ((dmaSrc_ + 2) & 0x1FFFF);
  }
}

void VDP::dmaFill(uint16_t v, int64_t now) {
  const int len = dmaLength();
  for (int i = 0; i < len; i++) {
    if ((code_ & 0xF) == 1) vramWrite(addr_ ^ 1, static_cast<uint8_t>(v >> 8));
    else if ((code_ & 0xF) == 3) { const int k = (addr_ >> 1) & 0x3F; cram[k] = v & 0xEEE; updateColor(k); }
    else if ((code_ & 0xF) == 5) { const int k = (addr_ >> 1) & 0x3F; if (k < 40) vsram[k] = v & 0x7FF; }
    addr_ = static_cast<uint16_t>(addr_ + reg[15]);
  }
  reg[19] = reg[20] = 0;
  dmaBusyUntil_ = dmaEnd(now, len, 2);
}

void VDP::dmaCopy(int64_t now) {
  const int len = dmaLength();
  uint16_t src = static_cast<uint16_t>(reg[21] | (reg[22] << 8));
  for (int i = 0; i < len; i++) {
    vramWrite(addr_ ^ 1, vram[src ^ 1]);
    src++;
    addr_ = static_cast<uint16_t>(addr_ + reg[15]);
  }
  reg[19] = reg[20] = 0;
  reg[21] = static_cast<uint8_t>(src); reg[22] = static_cast<uint8_t>(src >> 8);
  dmaBusyUntil_ = dmaEnd(now, len, 3);
}

// ------------------------------------------------------------ interrupções e linhas
int VDP::irqLevel() const {
  if (vintPending_ && (reg[1] & 0x20)) return 6;
  if (hintPending_ && (reg[0] & 0x10)) return 4;
  return 0;
}

void VDP::ack(int level) {
  if (level == 6) { vintPending_ = false; status_ &= ~0x80; }
  else if (level == 4) hintPending_ = false;
}

void VDP::vint() {
  status_ |= 0x80;
  vintPending_ = true;
}

void VDP::startLine(int line, int64_t now) {
  lineStart_ = now;
  line_ = line;
  const int act = activeLines();
  if (line == act) status_ |= 8;
  else if (line == linesPerFrame() - 1) status_ &= ~8;
  advanceDma(now);
  if (line < act) {
    renderLine(line);
    if (line + 1 < act) parseSprites(interlace2() ? (line + 1) * 2 + (oddField_ ? 1 : 0) : line + 1);
    if (hcount_ == 0) { hcount_ = reg[10]; hintPending_ = true; }
    else hcount_--;
  } else if (line == act) {
    frameStart();
    status_ |= 8;
    if (reg[12] & 2) oddField_ = !oddField_; else oddField_ = false;
    if (hcount_ == 0) hintPending_ = true;
  } else if (line == linesPerFrame() - 1) {
    parseSprites(interlace2() ? (oddField_ ? 1 : 0) : 0);
    hcount_ = reg[10];
    status_ &= ~8;
  }
}

// ------------------------------------------------------------ desenho
void VDP::renderPlane(uint8_t* buf, int line, int y2, bool planeB, int x0, int x1) {
  static const int SIZES[4] = {32, 64, 32, 128};
  int wc = SIZES[reg[16] & 3], hc = SIZES[(reg[16] >> 4) & 3];
  if (wc * hc > 4096) hc = 4096 / wc;
  const bool im2 = interlace2();
  const int tileH = im2 ? 16 : 8;
  const int hpx = hc * tileH;
  const uint32_t nt = planeB ? ((reg[4] & 7) << 13) : ((reg[2] & 0x38) << 10);
  // rolagem horizontal
  const uint32_t hsBase = (reg[13] & 0x3F) << 10;
  int hl = 0;
  switch (reg[11] & 3) {
    case 0: hl = 0; break;
    case 1: hl = line & 7; break;
    case 2: hl = line & ~7; break;
    default: hl = line; break;
  }
  const uint32_t ha = (hsBase + hl * 4 + (planeB ? 2 : 0)) & 0xFFFF;
  const int hs = ((vram[ha] << 8) | vram[(ha + 1) & 0xFFFF]) & 0x3FF;
  const int W = h40() ? 320 : 256;
  const int s = hs & 15;
  const int cStart = s ? -1 : 0;
  const bool colScroll = (reg[11] & 4) != 0;
  for (int c = cStart; c < W / 16; c++) {
    const int xs = c * 16 + s;
    if (xs + 16 <= x0 || xs >= x1) continue;
    int vs;
    if (!colScroll) vs = vsram[planeB ? 1 : 0];
    else if (c < 0) vs = h40() ? (vsram[38] & vsram[39]) : 0;
    else vs = vsram[c * 2 + (planeB ? 1 : 0)];
    vs &= im2 ? 0x7FF : 0x3FF;
    const int y = ((im2 ? y2 : line) + vs) & (hpx - 1);
    const int row = y / tileH, ty = y % tileH;
    for (int k = 0; k < 2; k++) {
      const int sx = xs + k * 8;
      const int px = (sx - hs) & (wc * 8 - 1);
      const uint32_t na = (nt + (row * wc + (px >> 3)) * 2) & 0xFFFF;
      const int name = (vram[na] << 8) | vram[na + 1];
      const int pal = (name >> 9) & 0x30, prio = (name >> 9) & 0x40;
      const int yy = (name & 0x1000) ? tileH - 1 - ty : ty;
      const uint32_t ta = im2 ? (((name & 0x3FF) << 6) + yy * 4) : (((name & 0x7FF) << 5) + yy * 4);
      const uint8_t* t = &vram[ta & 0xFFFC];
      uint8_t* o = buf + OFS + sx;
      const uint32_t row = (uint32_t(t[0]) << 24) | (t[1] << 16) | (t[2] << 8) | t[3];
      const int pa = prio | pal;
      if (sx >= x0 && sx + 8 <= x1) {
        if (name & 0x800) {
          for (int p = 0; p < 8; p++) { const int ci = (row >> (p * 4)) & 15; o[p] = static_cast<uint8_t>(ci ? (pa | ci) : prio); }
        } else {
          for (int p = 0; p < 8; p++) { const int ci = (row >> (28 - p * 4)) & 15; o[p] = static_cast<uint8_t>(ci ? (pa | ci) : prio); }
        }
        continue;
      }
      for (int p = 0; p < 8; p++) {
        const int x = sx + p;
        if (x < x0 || x >= x1) continue;
        const int ci = (row >> ((name & 0x800) ? p * 4 : 28 - p * 4)) & 15;
        o[p] = static_cast<uint8_t>(ci ? (pa | ci) : prio);
      }
    }
  }
}

void VDP::renderWindow(uint8_t* buf, int y2, int x0, int x1) {
  const bool im2 = interlace2();
  const int tileH = im2 ? 16 : 8;
  const int wc = h40() ? 64 : 32;
  const uint32_t nt = (reg[3] & (h40() ? 0x3C : 0x3E)) << 10;
  const int row = y2 / tileH, ty = y2 % tileH;
  for (int tx = x0 / 8; tx < (x1 + 7) / 8; tx++) {
    const uint32_t na = (nt + (row * wc + tx) * 2) & 0xFFFF;
    const int name = (vram[na] << 8) | vram[na + 1];
    const int pal = (name >> 9) & 0x30, prio = (name >> 9) & 0x40;
    const int yy = (name & 0x1000) ? tileH - 1 - ty : ty;
    const uint32_t ta = im2 ? (((name & 0x3FF) << 6) + yy * 4) : (((name & 0x7FF) << 5) + yy * 4);
    const uint8_t* t = &vram[ta & 0xFFFC];
    const bool hf = (name & 0x800) != 0;
    for (int p = 0; p < 8; p++) {
      const int x = tx * 8 + p;
      if (x < x0 || x >= x1) continue;
      const int q = hf ? 7 - p : p;
      const int ci = (q & 1) ? (t[q >> 1] & 15) : (t[q >> 1] >> 4);
      buf[OFS + x] = static_cast<uint8_t>(prio | (ci ? (pal | ci) : 0));
    }
  }
}

// lista de sprites da linha seguinte: montada no fim da linha anterior (como o VDP faz), com Y/tamanho/elo do
// cache interno e o padrão/X lidos da VRAM nesse momento
void VDP::parseSprites(int y2) {
  const bool H40 = h40(), im2 = interlace2();
  const int maxSpr = H40 ? 80 : 64, maxLine = H40 ? 20 : 16;
  const uint32_t sat = (reg[5] & (H40 ? 0x7E : 0x7F)) << 9;
  const int ln = y2 + (im2 ? 256 : 128);
  sprN_ = 0;
  int link = 0;
  for (int k = 0; k < maxSpr; k++) {
    const uint8_t* e = &satCache_[link * 8];
    const int y = ((e[0] << 8) | e[1]) & (im2 ? 0x3FF : 0x1FF);
    const int vsz = ((e[2] & 3) + 1) * (im2 ? 16 : 8);
    if (ln >= y && ln < y + vsz) {
      if (sprN_ == maxLine) { status_ |= 0x40; break; }
      const uint32_t va = (sat + link * 8) & 0xFFFF;
      Spr& o = spr_[sprN_++];
      o.row = ln - y;
      o.size = e[2] & 15;
      o.name = static_cast<uint16_t>((vram[(va + 4) & 0xFFFF] << 8) | vram[(va + 5) & 0xFFFF]);
      o.x = static_cast<uint16_t>(((vram[(va + 6) & 0xFFFF] << 8) | vram[(va + 7) & 0xFFFF]) & 0x1FF);
    }
    link = e[3] & 0x7F;
    if (!link || link >= maxSpr) break;
  }
}

void VDP::renderSprites(uint8_t* buf, int y2) {
  (void)y2;
  const bool H40 = h40(), im2 = interlace2();
  const int maxDots = H40 ? 320 : 256;
  const int W = H40 ? 320 : 256;
  const int n = sprN_;
  int dots = 0;
  bool masked = false, seenNonZeroX = false;
  bool dotOverflow = false;
  for (int i = 0; i < n && !dotOverflow; i++) {
    const Spr& sp = spr_[i];
    const int name = sp.name, xr = sp.x;
    const int hsz = ((sp.size >> 2) & 3) + 1, vcells = (sp.size & 3) + 1;
    const int tileH = im2 ? 16 : 8;
    if (xr == 0) {
      if (seenNonZeroX || lastLineDotOverflow_) masked = true;
    } else {
      seenNonZeroX = true;
    }
    int width = hsz * 8;
    if (dots + width > maxDots) { width = maxDots - dots; dotOverflow = true; }
    dots += width;
    if (masked) continue;
    int row = sp.row;
    if (name & 0x1000) row = vcells * tileH - 1 - row;
    const int pal = (name >> 9) & 0x30, prio = (name >> 9) & 0x40;
    const bool hf = (name & 0x800) != 0;
    const int x = xr - 128;
    for (int px = 0; px < width; px++) {
      const int sx = x + px;
      if (sx < 0 || sx >= W) continue;
      const int col = hf ? (hsz * 8 - 1 - px) : px;
      const int tile = im2 ? (((name & 0x3FF) + (col >> 3) * vcells + (row >> 4)) & 0x3FF)
                           : (((name & 0x7FF) + (col >> 3) * vcells + (row >> 3)) & 0x7FF);
      const uint32_t ta = im2 ? ((tile << 6) + (row & 15) * 4) : ((tile << 5) + (row & 7) * 4);
      const int q = col & 7;
      const uint8_t b = vram[(ta + (q >> 1)) & 0xFFFF];
      const int ci = (q & 1) ? (b & 15) : (b >> 4);
      if (!ci) continue;
      uint8_t& o = buf[OFS + sx];
      if (o & 0x80) { status_ |= 0x20; continue; }
      o = static_cast<uint8_t>(0x80 | prio | pal | ci);
    }
  }
  lastLineDotOverflow_ = dotOverflow;
}

void VDP::frameStart() {
  // o tamanho da imagem é fixado no começo do quadro (mudanças no meio do quadro valem para o próximo)
  if (mode4()) { outW = 256; outH = 192; return; }
  outW = h40() ? 320 : 256;
  outH = interlace2() ? activeLines() * 2 : activeLines();
}

void VDP::renderLine(int line) {
  uint32_t buf[320];
  int row = line;
  const int W = renderLineBuf(line, buf, row);
  if (row < 0 || row >= outH || row * outW + outW > FB_W * FB_H) return;
  uint32_t* out = &fb[row * outW];
  const int n = W < outW ? W : outW;
  std::memcpy(out, buf, sizeof(uint32_t) * static_cast<size_t>(n));
  for (int x = n; x < outW; x++) out[x] = 0xFF000000u;
  if (idxOut) host.lineDrawn(row, out, idxOut, n);
}

int VDP::renderLineBuf(int line, uint32_t* out, int& row) {
  if (mode4()) {
    for (int x = 0; x < 256; x++) out[x] = 0xFF000000u;
    if (idxOut) std::memset(idxOut, reg[7] & 0x3F, 256);
    row = line;
    return 256;
  }
  const bool H40 = h40(), im2 = interlace2();
  const int W = H40 ? 320 : 256;
  const int y2 = im2 ? line * 2 + (oddField_ ? 1 : 0) : line;
  row = im2 ? y2 : line;
  const uint32_t bg = lut_[0][reg[7] & 0x3F];
  if (!(reg[1] & 0x40)) {
    for (int x = 0; x < W; x++) out[x] = bg;
    if (idxOut) std::memset(idxOut, reg[7] & 0x3F, static_cast<size_t>(W));
    lastLineDotOverflow_ = false;
    return W;
  }
  std::memset(lineA_, 0, sizeof lineA_);
  std::memset(lineB_, 0, sizeof lineB_);
  std::memset(lineS_, 0, sizeof lineS_);
  renderPlane(lineB_, line, y2, true, 0, W);
  // janela
  const int wv = (reg[18] & 0x1F) * 8;
  const bool winLine = (reg[18] & 0x80) ? (line >= wv) : (line < wv);
  if (winLine) {
    renderWindow(lineA_, y2, 0, W);
  } else {
    int wx = (reg[17] & 0x1F) * 16;
    if (wx > W) wx = W;
    if (reg[17] & 0x80) {
      renderPlane(lineA_, line, y2, false, 0, wx);
      if (wx < W) renderWindow(lineA_, y2, wx, W);
    } else {
      if (wx > 0) renderWindow(lineA_, y2, 0, wx);
      renderPlane(lineA_, line, y2, false, wx, W);
    }
  }
  renderSprites(lineS_, y2);
  const bool sh = (reg[12] & 8) != 0;
  const uint8_t* A = lineA_ + OFS;
  const uint8_t* B = lineB_ + OFS;
  const uint8_t* S = lineS_ + OFS;
  const int bgi = reg[7] & 0x3F;
  if (!sh) {
    // caminho rápido sem sombra/realce: tabela de 64 casos (visível e prioridade de cada camada)
    static const struct Pick { uint8_t v[64]; Pick() {
      for (int i = 0; i < 64; i++) {
        const bool sp = i & 1, sh_ = i & 2, ap = i & 4, ah = i & 8, bp = i & 16, bh = i & 32;
        v[i] = (sp && sh_) ? 3 : (ap && ah) ? 2 : (bp && bh) ? 1 : sp ? 3 : ap ? 2 : bp ? 1 : 0;
      }
    } } PICK;
    const uint32_t* L = lut_[0];
    for (int x = 0; x < W; x++) {
      const int a = A[x], b = B[x], s = S[x];
      const int k = ((s & 15) ? 1 : 0) | ((s >> 5) & 2) | ((a & 15) ? 4 : 0) | ((a >> 3) & 8) | ((b & 15) ? 16 : 0) | ((b >> 1) & 32);
      const int w = PICK.v[k];
      const int ci = (w == 3 ? s : w == 2 ? a : w == 1 ? b : bgi) & 0x3F;
      out[x] = L[ci];
      if (idxOut) idxOut[x] = static_cast<uint8_t>(ci);
    }
    if (reg[0] & 0x20) for (int x = 0; x < 8; x++) { out[x] = bg; if (idxOut) idxOut[x] = static_cast<uint8_t>(bgi); }
    return W;
  }
  for (int x = 0; x < W; x++) {
    const int a = A[x], b = B[x], s = S[x];
    int col, mode = 0;  // 0 normal, 1 sombra, 2 realce
    const bool sp = (s & 15) != 0, ap = (a & 15) != 0, bp = (b & 15) != 0;
    bool fromSprite = false;
    if (sp && (s & 0x40)) { col = s; fromSprite = true; }
    else if (ap && (a & 0x40)) col = a;
    else if (bp && (b & 0x40)) col = b;
    else if (sp) { col = s; fromSprite = true; }
    else if (ap) col = a;
    else if (bp) col = b;
    else col = bgi;
    if (sh) {
      const bool shadow = !((a & 0x40) || (b & 0x40));
      if (fromSprite && (s & 0x3F) >= 0x3E) {
        // operadores de sombra/realce: mostram a camada de baixo
        if (ap && (a & 0x40)) col = a;
        else if (bp && (b & 0x40)) col = b;
        else if (ap) col = a;
        else if (bp) col = b;
        else col = bgi;
        if ((s & 0x3F) == 0x3E) mode = shadow ? 0 : 2;
        else mode = 1;
      } else if (fromSprite) {
        mode = (s & 0x40) ? 0 : (shadow ? 1 : 0);
        if ((s & 0x0F) == 0x0E) mode = 0;  // cor 14 dos sprites nunca escurece
      } else {
        mode = shadow ? 1 : 0;
      }
    }
    out[x] = lut_[mode][col & 0x3F];
    if (idxOut) idxOut[x] = static_cast<uint8_t>(col & 0x3F);
  }
  if (reg[0] & 0x20) for (int x = 0; x < 8; x++) { out[x] = bg; if (idxOut) idxOut[x] = static_cast<uint8_t>(bgi); }
  return W;
}

// ------------------------------------------------------------ estado
void VDP::sync(StateIO& io) {
  io.bytes(vram);
  io.raw(cram, sizeof cram); io.raw(vsram, sizeof vsram); io.raw(reg, sizeof reg);
  io.sync(pending_); io.sync(code_); io.sync(addr_); io.sync(fillPending_); io.sync(status_);
  io.sync(vintPending_); io.sync(hintPending_); io.sync(hcount_); io.sync(line_); io.sync(oddField_);
  io.sync(lastLineDotOverflow_); io.sync(dmaBusyUntil_); io.sync(lineStart_); io.raw(fifo_, sizeof fifo_); io.sync(fifoIdx_);
  io.sync(dmaSrc_); io.sync(dmaLeft_); io.sync(dmaDone_); io.sync(dmaT0_); io.sync(dmaL0_); io.sync(dmaLS0_); io.sync(dmaType_); io.sync(stall_); io.sync(readBuf_);
  io.raw(satCache_, sizeof satCache_); io.raw(spr_, sizeof spr_); io.sync(sprN_);
  io.sync(pal); io.sync(outW); io.sync(outH);
  if (io.reading()) {
    for (auto& c : cram) c &= 0xEEE;
    for (auto& v : vsram) v &= 0x7FF;
    clampTo(hcount_, 0, 255);
    fifoIdx_ &= 3;
    clampTo(sprN_, 0, 20);
    for (auto& o : spr_) { clampTo(o.row, 0, 63); o.size &= 15; o.x &= 0x1FF; }
    clampTo(dmaLeft_, 0, 0x10000); clampTo(dmaDone_, 0, 0x10000 - dmaLeft_); clampTo(dmaL0_, 0, 312); dmaType_ &= 1;
    clampTo(line_, 0, 312);
    if (!((outW == 256 || outW == 320) && (outH == 192 || outH == 224 || outH == 240 || outH == 448 || outH == 480))) frameStart();
    clampTo<int64_t>(stall_, 0, int64_t(1) << 30);
    for (int i = 0; i < 64; i++) updateColor(i);
  }
}

}  // namespace indigo::md
