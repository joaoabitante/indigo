// PPU 2C02 ponto a ponto: 341 pontos × 262 linhas, busca de fundo em 8 pontos por bloco com registradores de
// deslocamento, avaliação de sprites por linha e busca dos padrões nos pontos 257–320 (o MMC3 conta essas
// buscas pelo A12), vblank na linha 241, linha pré-render 261 com o ponto pulado nos quadros ímpares.
#include <algorithm>
#include <cstring>

#include "nes.h"

namespace indigo::nes {

namespace {
// paleta 2C02 (RGB)
constexpr uint8_t BASE[64][3] = {
    {84, 84, 84},    {0, 30, 116},    {8, 16, 144},    {48, 0, 136},    {68, 0, 100},    {92, 0, 48},
    {84, 4, 0},      {60, 24, 0},     {32, 42, 0},     {8, 58, 0},      {0, 64, 0},      {0, 60, 0},
    {0, 50, 60},     {0, 0, 0},       {0, 0, 0},       {0, 0, 0},       {152, 150, 152}, {8, 76, 196},
    {48, 50, 236},   {92, 30, 228},   {136, 20, 176},  {160, 20, 100},  {152, 34, 32},   {120, 60, 0},
    {84, 90, 0},     {40, 114, 0},    {8, 124, 0},     {0, 118, 40},    {0, 102, 120},   {0, 0, 0},
    {0, 0, 0},       {0, 0, 0},       {236, 238, 236}, {76, 154, 236},  {120, 124, 236}, {176, 98, 236},
    {228, 84, 236},  {236, 88, 180},  {236, 106, 100}, {212, 136, 32},  {160, 170, 0},   {116, 196, 0},
    {76, 208, 32},   {56, 204, 108},  {56, 180, 204},  {60, 60, 60},    {0, 0, 0},       {0, 0, 0},
    {236, 238, 236}, {168, 204, 236}, {188, 188, 236}, {212, 178, 236}, {236, 174, 236}, {236, 174, 212},
    {236, 180, 176}, {228, 196, 144}, {204, 210, 120}, {180, 222, 120}, {168, 226, 144}, {152, 226, 180},
    {160, 214, 228}, {160, 162, 160}, {0, 0, 0},       {0, 0, 0},
};
// 64 cores × 8 combinações de ênfase (bits 5–7 de $2001: vermelho, verde, azul)
struct Colors {
  uint32_t c[512];
  Colors() {
    for (int e = 0; e < 8; e++)
      for (int i = 0; i < 64; i++) {
        double r = BASE[i][0], g = BASE[i][1], b = BASE[i][2];
        if (e && (i & 0xF) < 0xE) {
          if (!(e & 1)) r *= 0.75;
          if (!(e & 2)) g *= 0.75;
          if (!(e & 4)) b *= 0.75;
        }
        c[e * 64 + i] = 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
      }
  }
};
const Colors COLORS;

uint8_t flip8(uint8_t b) {
  b = static_cast<uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
  b = static_cast<uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
  return static_cast<uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
}
}  // namespace

void PPU::reset(bool power) {
  ctrl = mask = 0;
  w = false;
  readBuf = 0;
  oddFrame = false;
  scanline = 0;
  dot = 0;
  if (power) {
    status = 0;
    oamAddr = 0;
    v = t = 0;
    fineX = 0;
    std::memset(ciram, 0, sizeof ciram);
    // valores típicos de quando o console liga
    static const uint8_t pal0[32] = {0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
                                     0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};
    std::memcpy(palette, pal0, 32);
    std::memset(oam, 0, sizeof oam);
    std::fill(std::begin(fb), std::end(fb), 0xFF000000u);
  }
}

// ---------------------------------------------------------------- barramento do PPU
uint8_t PPU::busRead(uint16_t a) {
  a &= 0x3FFF;
  nes.mapper->ppuAddress(a);
  if (a < 0x2000) return nes.mapper->chrRead(a);
  if (a < 0x3F00) {
    if (nes.mapper->customNt) return nes.mapper->ntRead(a);
    return ciram[(nes.mapper->nt[(a >> 10) & 3] << 10) | (a & 0x3FF)];
  }
  int p = a & 0x1F;
  if ((p & 3) == 0) p &= 0x0F;
  return palette[p];
}

void PPU::busWrite(uint16_t a, uint8_t val) {
  a &= 0x3FFF;
  nes.mapper->ppuAddress(a);
  if (a < 0x2000) { nes.mapper->chrWrite(a, val); return; }
  if (a < 0x3F00) {
    if (nes.mapper->customNt) nes.mapper->ntWrite(a, val);
    else ciram[(nes.mapper->nt[(a >> 10) & 3] << 10) | (a & 0x3FF)] = val;
    return;
  }
  int p = a & 0x1F;
  if ((p & 3) == 0) p &= 0x0F;
  palette[p] = val & 0x3F;
}

// ---------------------------------------------------------------- barramento aberto do PPU
// cada bit guarda o último valor posto nele e some (vira 0) uns 600 ms depois se ninguém renovar
uint8_t PPU::openBus() const {
  uint8_t v = latch;
  for (int b = 0; b < 8; b++) if (frames - latchAt[b] > 36) v &= ~(1 << b);
  return v;
}
void PPU::drive(uint8_t val, uint8_t bits) {
  latch = (latch & ~bits) | (val & bits);
  for (int b = 0; b < 8; b++) if (bits & (1 << b)) latchAt[b] = frames;
}

// ---------------------------------------------------------------- registradores
uint8_t PPU::readReg(int r) {
  switch (r & 7) {
    case 2: {
      // ler bem antes do vblank ligar impede o flag e o NMI deste quadro
      // (aqui "dot" é o próximo ponto a executar)
      if (scanline == 241 && dot == 1) preventVbl = true;
      if (scanline == 241 && (dot == 2 || dot == 3)) suppressNmi = true;
      const uint8_t val = (status & 0xE0) | (openBus() & 0x1F);
      drive(val, 0xE0);
      status &= 0x7F;
      w = false;
      return val;
    }
    case 4: {
      uint8_t val = oam[oamAddr];
      if ((oamAddr & 3) == 2) val &= 0xE3;
      drive(val, 0xFF);
      return val;
    }
    case 7: {
      // duas leituras em ciclos seguidos de CPU (DMA): a segunda não tem efeito
      if (ignoreRead) return openBus();
      ignoreRead = 6;
      uint8_t val;
      if ((v & 0x3FFF) >= 0x3F00) {
        val = (busRead(v) & 0x3F) | (openBus() & 0xC0);
        readBuf = busRead(v - 0x1000);
      } else {
        val = readBuf;
        readBuf = busRead(v);
      }
      if (rendering() && (scanline < 240 || scanline == 261)) { incX(); incY(); }
      else v = (v + ((ctrl & 4) ? 32 : 1)) & 0x7FFF;
      nes.mapper->ppuAddress(v & 0x3FFF);
      drive(val, (v & 0x3FFF) >= 0x3F00 ? 0x3F : 0xFF);
      return val;
    }
    default: return openBus();
  }
}

void PPU::writeReg(int r, uint8_t val) {
  drive(val, 0xFF);
  switch (r & 7) {
    case 0:
      ctrl = val;
      t = (t & 0xF3FF) | ((val & 3) << 10);
      break;
    case 1: mask = val; break;
    case 3: oamAddr = val; break;
    case 4:
      if (rendering() && (scanline < 240 || scanline == 261)) oamAddr += 4;  // durante o desenho a escrita não vale
      else oam[oamAddr++] = val;
      break;
    case 5:
      if (!w) { t = (t & 0xFFE0) | (val >> 3); fineX = val & 7; }
      else t = (t & 0x8C1F) | ((val & 7) << 12) | ((val & 0xF8) << 2);
      w = !w;
      break;
    case 6:
      if (!w) t = (t & 0x80FF) | ((val & 0x3F) << 8);
      else { t = (t & 0xFF00) | val; v = t; nes.mapper->ppuAddress(v & 0x3FFF); }
      w = !w;
      break;
    case 7:
      busWrite(v, val);
      if (rendering() && (scanline < 240 || scanline == 261)) { incX(); incY(); }
      else v = (v + ((ctrl & 4) ? 32 : 1)) & 0x7FFF;
      nes.mapper->ppuAddress(v & 0x3FFF);
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- rolagem (registrador v)
void PPU::incX() {
  if ((v & 0x1F) == 31) { v &= ~0x1F; v ^= 0x400; }
  else v++;
}

void PPU::incY() {
  if ((v & 0x7000) != 0x7000) { v += 0x1000; return; }
  v &= ~0x7000;
  int y = (v & 0x3E0) >> 5;
  if (y == 29) { y = 0; v ^= 0x800; }
  else if (y == 31) y = 0;
  else y++;
  v = (v & ~0x3E0) | (y << 5);
}

// ---------------------------------------------------------------- sprites
void PPU::evaluateSprites() {
  // feita no ponto 65 com o tempo do chip: 2 pontos por sprite olhado, +6 para copiar um que está na linha.
  // Depois de 8, a busca continua com o defeito do hardware (o índice do byte também anda) e o flag de
  // excesso liga no ponto em que o chip o acharia.
  nextCount = 0;
  overflowDot = -1;
  if (scanline == 261) return;  // a linha 0 não tem sprites
  const int h = (ctrl & 0x20) ? 16 : 8;
  int at = 65, n = 0;
  for (; n < 64 && nextCount < 8; n++) {
    const int y = oam[n * 4];
    const int diff = scanline - y;
    at += 2;
    if (diff < 0 || diff >= h) continue;
    Sprite& s = next[nextCount++];
    s.y = static_cast<uint8_t>(y);
    s.tile = oam[n * 4 + 1];
    s.attr = oam[n * 4 + 2];
    s.x = oam[n * 4 + 3];
    s.zero = n == 0 ? 1 : 0;
    at += 6;
  }
  int m = 0;
  while (nextCount == 8 && n < 64) {
    const int diff = scanline - oam[n * 4 + m];
    at += 2;
    if (diff >= 0 && diff < h) { overflowDot = at - 1; break; }
    n++;
    m = (m + 1) & 3;
  }
  if (overflowDot > 256) overflowDot = 256;
}

void PPU::fetchSprite(int slot, int phase) {
  // fases: 0 e 2 = leituras de tabela de nomes sem uso, 4 = padrão (plano 0), 6 = padrão (plano 1)
  if (phase == 0 || phase == 2) { busRead(0x2000 | (v & 0xFFF)); return; }
  if (phase != 4 && phase != 6) return;
  const bool tall = ctrl & 0x20;
  uint16_t addr;
  Sprite* s = slot < nextCount ? &next[slot] : nullptr;
  if (s) {
    int row = scanline - s->y;
    if (tall) {
      if (s->attr & 0x80) row = 15 - row;
      int tile = s->tile & 0xFE;
      if (row > 7) { tile++; row -= 8; }
      addr = static_cast<uint16_t>(((s->tile & 1) << 12) | (tile << 4) | row);
    } else {
      if (s->attr & 0x80) row = 7 - row;
      addr = static_cast<uint16_t>(((ctrl & 8) << 9) | (s->tile << 4) | row);
    }
  } else {
    addr = tall ? 0x1FF0 : static_cast<uint16_t>(((ctrl & 8) << 9) | 0xFF0);  // espaço vazio: busca o padrão $FF
  }
  if (phase == 6) addr += 8;
  uint8_t b = busRead(addr);
  if (!s) return;
  if (s->attr & 0x40) b = flip8(b);
  if (phase == 4) s->lo = b; else s->hi = b;
}

// ---------------------------------------------------------------- um pixel (pontos 1–256 das linhas visíveis)
void PPU::pixel() {
  const int x = dot - 1;
  uint8_t bg = 0, bgPal = 0;
  if ((mask & 0x08) && (x >= 8 || (mask & 0x02))) {
    const uint16_t bit = 0x8000 >> fineX;
    bg = static_cast<uint8_t>(((patLo & bit) ? 1 : 0) | ((patHi & bit) ? 2 : 0));
    bgPal = static_cast<uint8_t>(((attLo & bit) ? 1 : 0) | ((attHi & bit) ? 2 : 0));
  }
  uint8_t sp = 0, spPal = 0;
  bool spFront = false;
  if ((mask & 0x10) && (x >= 8 || (mask & 0x04))) {
    for (int i = 0; i < lineCount; i++) {
      const int off = x - line[i].x;
      if (off < 0 || off > 7) continue;
      const uint8_t c = static_cast<uint8_t>(((line[i].lo >> (7 - off)) & 1) | (((line[i].hi >> (7 - off)) & 1) << 1));
      if (!c) continue;
      if (line[i].zero && bg && x != 255) status |= 0x40;
      sp = c; spPal = line[i].attr & 3; spFront = !(line[i].attr & 0x20);
      break;  // o primeiro sprite opaco ganha
    }
  }
  int idx;
  if (!sp && !bg) idx = 0;
  else if (!sp) idx = (bgPal << 2) | bg;
  else if (!bg) idx = 0x10 | (spPal << 2) | sp;
  else idx = spFront ? (0x10 | (spPal << 2) | sp) : ((bgPal << 2) | bg);
  if ((idx & 3) == 0) idx = (idx & 0x10) ? (idx & 0x0F) : 0;
  uint8_t c = palette[idx & 0x1F];
  if (mask & 1) c &= 0x30;
  fb[scanline * 256 + x] = COLORS.c[((mask >> 5) & 7) * 64 + (c & 0x3F)];
}

// ---------------------------------------------------------------- um ponto
void PPU::step() {
  if (ignoreRead) ignoreRead--;
  const bool render = rendering();
  const bool visible = scanline < 240;
  const bool pre = scanline == 261;

  if (visible || pre) {
    if (render) {
      if ((dot >= 2 && dot <= 257) || (dot >= 322 && dot <= 337)) {
        patLo <<= 1; patHi <<= 1; attLo <<= 1; attHi <<= 1;
      }
      if ((dot >= 1 && dot <= 256) || (dot >= 321 && dot <= 337)) {
        switch ((dot - 1) & 7) {
          case 0:
            patLo = (patLo & 0xFF00) | patLoByte;
            patHi = (patHi & 0xFF00) | patHiByte;
            attLo = (attLo & 0xFF00) | ((atByte & 1) ? 0xFF : 0);
            attHi = (attHi & 0xFF00) | ((atByte & 2) ? 0xFF : 0);
            ntByte = busRead(0x2000 | (v & 0xFFF));
            break;
          case 2: {
            const uint8_t at = busRead(0x23C0 | (v & 0xC00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
            atByte = static_cast<uint8_t>((at >> (((v >> 4) & 4) | (v & 2))) & 3);
            break;
          }
          case 4: patLoByte = busRead(static_cast<uint16_t>(((ctrl & 0x10) << 8) | (ntByte << 4) | ((v >> 12) & 7))); break;
          case 6: patHiByte = busRead(static_cast<uint16_t>(((ctrl & 0x10) << 8) | (ntByte << 4) | ((v >> 12) & 7) | 8)); break;
          case 7: if (dot != 337) incX(); break;
          default: break;
        }
      }
      if (dot == 256) incY();
      if (dot == 65) evaluateSprites();
      if (dot == overflowDot && visible) status |= 0x20;
      if (dot == 257) {
        patLo = (patLo & 0xFF00) | patLoByte;
        patHi = (patHi & 0xFF00) | patHiByte;
        attLo = (attLo & 0xFF00) | ((atByte & 1) ? 0xFF : 0);
        attHi = (attHi & 0xFF00) | ((atByte & 2) ? 0xFF : 0);
        v = (v & ~0x41F) | (t & 0x41F);
      }
      if (pre && dot >= 280 && dot <= 304) v = (v & ~0x7BE0) | (t & 0x7BE0);
      if (dot >= 257 && dot <= 320) { fetchSprite((dot - 257) >> 3, (dot - 257) & 7); oamAddr = 0; }
      if (dot == 339) busRead(0x2000 | (v & 0xFFF));
    } else if (dot == 65) {
      nextCount = 0;
      overflowDot = -1;
    }
    if (visible && dot >= 1 && dot <= 256) {
      if (render) pixel();
      else {
        // desenho desligado: cor de fundo (ou a cor da paleta apontada por v)
        uint8_t c = ((v & 0x3F00) == 0x3F00) ? palette[(v & 3) ? (v & 0x1F) : (v & 0x0C)] : palette[0];
        if (mask & 1) c &= 0x30;
        fb[scanline * 256 + dot - 1] = COLORS.c[((mask >> 5) & 7) * 64 + (c & 0x3F)];
      }
    }
    if (dot == 320) {
      // os sprites buscados agora são os da próxima linha
      std::memcpy(line, next, sizeof line);
      lineCount = render ? nextCount : 0;
    }
  }

  if (scanline == 241 && dot == 1) {
    if (!preventVbl) status |= 0x80;
    preventVbl = false;
    frameDone = true;
    frames++;
  }
  if (pre && dot == 1) status &= 0x1F;

  // próximo ponto (o pré-render dos quadros ímpares pula o último ponto quando o desenho está ligado)
  // (a decisão usa o $2001 de um ponto antes: a escrita no registrador demora um ponto para valer)
  if (pre && dot == 338) skipArmed = render;
  if (pre && dot == 339 && oddFrame && skipArmed) dot = 340;
  if (++dot > 340) {
    dot = 0;
    if (++scanline > 261) { scanline = 0; oddFrame = !oddFrame; }
  }
}

void PPU::sync(StateIO& io) {
  io.raw(fb, sizeof fb);
  io.raw(ciram, sizeof ciram); io.raw(palette, sizeof palette); io.raw(oam, sizeof oam);
  io.sync(ctrl); io.sync(mask); io.sync(status); io.sync(oamAddr); io.sync(readBuf); io.sync(latch); io.raw(latchAt, sizeof latchAt); io.sync(frames); io.sync(fineX);
  io.sync(v); io.sync(t); io.sync(w); io.sync(preventVbl); io.sync(suppressNmi);
  io.sync(patLo); io.sync(patHi); io.sync(attLo); io.sync(attHi);
  io.sync(ntByte); io.sync(atByte); io.sync(patLoByte); io.sync(patHiByte);
  io.raw(next, sizeof next); io.raw(line, sizeof line);
  io.sync(nextCount); io.sync(lineCount); io.sync(overflowDot);
  io.sync(scanline); io.sync(dot); io.sync(frameDone); io.sync(oddFrame); io.sync(skipArmed); io.sync(ignoreRead);
  if (io.reading()) {
    for (uint8_t& p : palette) p &= 0x3F;
    clampTo(ignoreRead, 0, 6);
    clampTo(nextCount, 0, 8); clampTo(lineCount, 0, 8);
    clampTo(scanline, 0, 261); clampTo(dot, 0, 340);
    fineX &= 7; v &= 0x7FFF; t &= 0x7FFF;
  }
}

}  // namespace indigo::nes
