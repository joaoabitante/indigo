// Suzy: motor de sprites (8 tipos, literal/empacotado, 1-4 bits por ponto, escala, esticamento, inclinação,
// quadrantes, colisão) e a matemática (16x16 com sinal/acúmulo, 32/16).
// O desenho é feito de uma vez quando o motor é ligado; a CPU fica dormindo pelo tempo que a Suzy levaria
// (3 tiques por acesso à RAM, como no Handy).
#include <algorithm>

#include "lynx.h"

namespace indigo::lynx {

namespace {
enum { BG_SHADOW = 0, BG_NONCOLL, BOUNDARY_SHADOW, BOUNDARY, NORMAL, NONCOLL, XOR_SHADOW, SHADOW };
enum { LINE_ERROR = 0, LINE_ABS_LITERAL, LINE_LITERAL, LINE_PACKED };
constexpr int LINE_END = 0x80;
constexpr uint32_t WORK_LIMIT = 4000000;  // proteção contra listas de sprites malformadas (laço sem fim)

// registradores de 16 bits da Suzy ($FC00-$FC2F)
enum {
  TMPADR = 0x00, TILTACUM = 0x02, HOFF = 0x04, VOFF = 0x06, VIDBAS = 0x08, COLLBAS = 0x0A, VIDADR = 0x0C,
  COLLADR = 0x0E, SCBNEXT = 0x10, SPRDLINE = 0x12, HPOSSTRT = 0x14, VPOSSTRT = 0x16, SPRHSIZ = 0x18,
  SPRVSIZ = 0x1A, STRETCH = 0x1C, TILT = 0x1E, SPRDOFF = 0x20, SPRVPOS = 0x22, COLLOFF = 0x24,
  VSIZACUM = 0x26, HSIZOFF = 0x28, VSIZOFF = 0x2A, SCBADR = 0x2C, PROCADR = 0x2E
};
}  // namespace

// ---------------------------------------------------------------- matemática
static uint32_t get32(const uint8_t* b) { return b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<uint32_t>(b[3]) << 24); }
static void put32(uint8_t* b, uint32_t v) { b[0] = static_cast<uint8_t>(v); b[1] = static_cast<uint8_t>(v >> 8); b[2] = static_cast<uint8_t>(v >> 16); b[3] = static_cast<uint8_t>(v >> 24); }

void Lynx::multiply() {
  // ABCD: [0]=D [1]=C [2]=B [3]=A; AB x CD = EFGH, acumulando em JKLM
  mathBit = false;
  const uint32_t cd = mathABCD[0] | (mathABCD[1] << 8);
  const uint32_t abv = mathABCD[2] | (mathABCD[3] << 8);
  uint32_t r = abv * cd;
  if (signedMath && signAB + signCD == 0) r = ~r + 1;
  put32(mathEFGH, r);
  if (accumulate) put32(mathJKLM, get32(mathJKLM) + r);
}

void Lynx::divide() {
  mathBit = false;
  const uint32_t np = mathNP[0] | (mathNP[1] << 8);
  const uint32_t efgh = get32(mathEFGH);
  if (np) {
    put32(mathABCD, efgh / np);
    put32(mathJKLM, efgh % np);
  } else {
    put32(mathABCD, 0xFFFFFFFFu);
    put32(mathJKLM, 0);
    mathBit = true;
  }
}

// ---------------------------------------------------------------- dados de uma linha de sprite
uint32_t Lynx::lineGetBits(int bits) {
  if (lineBitsLeft <= static_cast<uint32_t>(bits)) return 0;
  if (lineShiftCount < bits) {
    lineShift = (lineShift << 24) | (ramPeek(lineTmp) << 16) | (ramPeek(static_cast<uint16_t>(lineTmp + 1)) << 8) |
                ramPeek(static_cast<uint16_t>(lineTmp + 2));
    lineTmp = static_cast<uint16_t>(lineTmp + 3);
    lineShiftCount += 24;
  }
  const uint32_t v = (lineShift >> (lineShiftCount - bits)) & ((1u << bits) - 1);
  lineShiftCount -= bits;
  lineBitsLeft -= bits;
  return v;
}

int Lynx::lineInit(int voff) {
  lineShift = 0;
  lineShiftCount = 0;
  lineRepeat = 0;
  linePixel = 0;
  lineType = LINE_ERROR;
  lineBitsLeft = 0xFFFF;
  lineTmp = sw(SPRDLINE);
  const int offset = static_cast<int>(lineGetBits(8));
  lineBitsLeft = static_cast<uint32_t>((offset - 1) * 8);
  const int bpp = ((sprctl0 >> 6) & 3) + 1;
  if (sprctl1 & 0x80) {  // literal puro
    lineType = LINE_ABS_LITERAL;
    lineRepeat = ((offset - 1) * 8) / bpp;
  }
  if (voff < 0 || voff > 101) voff = 0;
  lineBase = static_cast<uint16_t>(sw(VIDBAS) + voff * (SCREEN_W / 2));
  lineColl = static_cast<uint16_t>(sw(COLLBAS) + voff * (SCREEN_W / 2));
  return offset;
}

int Lynx::lineGetPixel() {
  const int bpp = ((sprctl0 >> 6) & 3) + 1;
  if (!lineRepeat) {
    if (lineType != LINE_ABS_LITERAL) lineType = lineGetBits(1) ? LINE_LITERAL : LINE_PACKED;
    switch (lineType) {
      case LINE_ABS_LITERAL: linePixel = LINE_END; return linePixel;
      case LINE_LITERAL: lineRepeat = static_cast<int>(lineGetBits(4)) + 1; break;
      case LINE_PACKED:
        lineRepeat = static_cast<int>(lineGetBits(4));
        linePixel = lineRepeat ? penIndex[lineGetBits(bpp)] : LINE_END;
        lineRepeat++;
        break;
      default: return 0;
    }
  }
  if (linePixel != LINE_END) {
    lineRepeat--;
    switch (lineType) {
      case LINE_ABS_LITERAL: {
        const int p = static_cast<int>(lineGetBits(bpp));
        linePixel = (!lineRepeat && !p) ? LINE_END : penIndex[p];
        break;
      }
      case LINE_LITERAL: linePixel = penIndex[lineGetBits(bpp)]; break;
      default: break;
    }
  }
  return linePixel;
}

// ---------------------------------------------------------------- um ponto na tela / no buffer de colisão
void Lynx::processPixel(int hoff, int pixel) {
  const uint16_t sa = static_cast<uint16_t>(lineBase + (hoff >> 1));
  const uint16_t ca = static_cast<uint16_t>(lineColl + (hoff >> 1));
  const bool hi = !(hoff & 1);
  auto writePix = [&](int p) {
    uint8_t d = ram[sa];
    d = hi ? static_cast<uint8_t>((d & 0x0F) | (p << 4)) : static_cast<uint8_t>((d & 0xF0) | p);
    ram[sa] = d;
    spriteCost += 6;
  };
  auto readPix = [&]() { spriteCost += 3; return hi ? ram[sa] >> 4 : ram[sa] & 0x0F; };
  const bool collOn = !(sprcoll & 0x20) && !noCollide;
  const int num = sprcoll & 0x0F;
  auto writeColl = [&]() {
    uint8_t d = ram[ca];
    d = hi ? static_cast<uint8_t>((d & 0x0F) | (num << 4)) : static_cast<uint8_t>((d & 0xF0) | num);
    ram[ca] = d;
    spriteCost += 6;
  };
  auto collide = [&]() {
    spriteCost += 3;
    const int c = hi ? ram[ca] >> 4 : ram[ca] & 0x0F;
    if (c > collision) collision = c;
    writeColl();
  };
  switch (sprctl0 & 7) {
    case BG_SHADOW:
      writePix(pixel);
      if (collOn && pixel != 0x0E) writeColl();
      break;
    case BG_NONCOLL: writePix(pixel); break;
    case NONCOLL: if (pixel) writePix(pixel); break;
    case BOUNDARY:
      if (pixel && pixel != 0x0F) writePix(pixel);
      if (pixel && collOn) collide();
      break;
    case NORMAL:
      if (pixel) { writePix(pixel); if (collOn) collide(); }
      break;
    case BOUNDARY_SHADOW:
      if (pixel && pixel != 0x0E && pixel != 0x0F) writePix(pixel);
      if (pixel && pixel != 0x0E && collOn) collide();
      break;
    case SHADOW:
      if (pixel) writePix(pixel);
      if (pixel && pixel != 0x0E && collOn) collide();
      break;
    case XOR_SHADOW:
      if (pixel) writePix(readPix() ^ pixel);
      if (pixel && pixel != 0x0E && collOn) collide();
      break;
  }
}

// ---------------------------------------------------------------- lista de sprites (SCB)
uint32_t Lynx::paintSprites() {
  if (!suzyBusEn || !sprGo) return 0;
  spriteCost = 0;
  uint32_t work = 0;
  int count = 0;
  auto peekw = [&](uint16_t a) { return static_cast<uint16_t>(ramPeek(a) | (ramPeek(static_cast<uint16_t>(a + 1)) << 8)); };
  for (;;) {
    if (!(sw(SCBNEXT) & 0xFF00)) break;  // só o byte alto conta para o fim da lista
    if (++count > 4096 || work > WORK_LIMIT) break;
    bool everOnScreen = false;
    uint16_t tmp = sw(SCBNEXT);
    sw(SCBADR) = tmp;
    sprctl0 = ramPeek(tmp++);
    sprctl1 = ramPeek(tmp++);
    sprcoll = ramPeek(tmp++);
    sw(SCBNEXT) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2);
    collision = 0;
    if (!(sprctl1 & 0x04)) {  // sprite não pulado
      sw(SPRDLINE) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2);
      sw(HPOSSTRT) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2);
      sw(VPOSSTRT) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2);
      const int depth = (sprctl1 >> 4) & 3;
      const bool enStretch = depth >= 2, enTilt = depth == 3;
      if (depth >= 1) { sw(SPRHSIZ) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2); sw(SPRVSIZ) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2); }
      if (depth >= 2) { sw(STRETCH) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2); }
      if (depth >= 3) { sw(TILT) = peekw(tmp); tmp = static_cast<uint16_t>(tmp + 2); }
      if (!(sprctl1 & 0x08)) {  // recarrega a paleta de canetas
        for (int k = 0; k < 8; k++) {
          const uint8_t d = ramPeek(tmp++);
          penIndex[2 * k] = d >> 4;
          penIndex[2 * k + 1] = d & 0x0F;
        }
      }
      sw(TMPADR) = tmp;
      const int hStart = static_cast<int16_t>(sw(HOFF)), hEnd = hStart + SCREEN_W;
      const int vStart = static_cast<int16_t>(sw(VOFF)), vEnd = vStart + SCREEN_H;
      const int hMid = hStart + SCREEN_W / 2, vMid = vStart + SCREEN_H / 2;
      // quadrantes: 0 = SE, 1 = NE, 2 = NW, 3 = SW
      int quadrant = (sprctl1 & 1) ? ((sprctl1 & 2) ? 2 : 3) : ((sprctl1 & 2) ? 1 : 0);
      const bool superclip = static_cast<int16_t>(sw(HPOSSTRT)) < hStart || static_cast<int16_t>(sw(HPOSSTRT)) >= hEnd ||
                             static_cast<int16_t>(sw(VPOSSTRT)) < vStart || static_cast<int16_t>(sw(VPOSSTRT)) >= vEnd;
      int vquadoff = 0, hquadoff = 0;
      for (int loop = 0; loop < 4; loop++) {
        const int sprV = static_cast<int16_t>(sw(VPOSSTRT));
        const int sprH = static_cast<int16_t>(sw(HPOSSTRT));
        int hsign = (quadrant == 0 || quadrant == 1) ? 1 : -1;
        int vsign = (quadrant == 0 || quadrant == 3) ? 1 : -1;
        if (sprctl0 & 0x10) vsign = -vsign;
        if (sprctl0 & 0x20) hsign = -hsign;
        bool render = true;
        if (superclip) {
          // só desenha o quadrante se a tela estiver nele
          static const int vflip[4] = {1, 0, 3, 2}, hflip[4] = {3, 2, 1, 0};
          int q = quadrant;
          if (sprctl0 & 0x10) q = vflip[q];
          if (sprctl0 & 0x20) q = hflip[q];
          switch (q) {
            case 3: render = (sprH >= hStart || sprH <= hMid) && (sprV < vEnd || sprV >= vMid); break;
            case 2: render = (sprH >= hStart || sprH <= hMid) && (sprV >= vStart || sprV <= vMid); break;
            case 1: render = (sprH < hEnd || sprH >= hMid) && (sprV >= vStart || sprV <= vMid); break;
            default: render = (sprH < hEnd || sprH >= hMid) && (sprV < vEnd || sprV >= vMid); break;
          }
        }
        if (render) {
          int voff = static_cast<int16_t>(sw(VPOSSTRT)) - vStart;
          sw(TILTACUM) = 0;
          sw(VSIZACUM) = vsign == 1 ? sw(VSIZOFF) : 0;
          if (loop == 0) vquadoff = vsign;
          if (vsign != vquadoff) voff += vsign;
          for (;;) {
            if (++work > WORK_LIMIT) { loop = 4; break; }
            sw(VSIZACUM) = static_cast<uint16_t>(sw(VSIZACUM) + sw(SPRVSIZ));
            const int pixelHeight = sw(VSIZACUM) >> 8;
            sw(VSIZACUM) &= 0x00FF;
            const int off = lineInit(0);
            sw(SPRDOFF) = static_cast<uint16_t>(off);
            if (off == 1) { sw(SPRDLINE) = static_cast<uint16_t>(sw(SPRDLINE) + 1); break; }
            if (off == 0) { loop = 4; break; }
            for (int vl = 0; vl < pixelHeight; vl++) {
              if (vsign == 1 && voff >= SCREEN_H) break;
              if (vsign == -1 && voff < 0) break;
              if (voff >= 0 && voff < SCREEN_H) {
                sw(HPOSSTRT) = static_cast<uint16_t>(sw(HPOSSTRT) + (static_cast<int16_t>(sw(TILTACUM)) >> 8));
                sw(TILTACUM) &= 0x00FF;
                int hoff = static_cast<int16_t>(sw(HPOSSTRT)) - hStart;
                uint16_t hsizacum = sw(HSIZOFF);
                if (loop == 0) hquadoff = hsign;
                if (hsign != hquadoff) hoff += hsign;
                lineInit(voff);
                bool onscreen = false;
                int pixel;
                while ((pixel = lineGetPixel()) != LINE_END) {
                  hsizacum = static_cast<uint16_t>(hsizacum + sw(SPRHSIZ));
                  const int pw = hsizacum >> 8;
                  hsizacum &= 0x00FF;
                  work += static_cast<uint32_t>(pw) + 1;
                  for (int hl = 0; hl < pw; hl++) {
                    if (hoff >= 0 && hoff < SCREEN_W) {
                      processPixel(hoff, pixel);
                      onscreen = everOnScreen = true;
                    } else if (onscreen) {
                      break;
                    }
                    hoff += hsign;
                  }
                  if (work > WORK_LIMIT) break;
                }
              }
              voff += vsign;
              if (enStretch) sw(SPRHSIZ) = static_cast<uint16_t>(sw(SPRHSIZ) + sw(STRETCH));
              if (enTilt) sw(TILTACUM) = static_cast<uint16_t>(sw(TILTACUM) + sw(TILT));
            }
            if (vStretch) sw(SPRVSIZ) = static_cast<uint16_t>(sw(SPRVSIZ) + sw(STRETCH) * pixelHeight);
            sw(SPRDLINE) = static_cast<uint16_t>(sw(SPRDLINE) + sw(SPRDOFF));
          }
        } else {
          // pula os dados do quadrante
          for (;;) {
            if (++work > WORK_LIMIT) { loop = 4; break; }
            const int off = lineInit(0);
            sw(SPRDOFF) = static_cast<uint16_t>(off);
            sw(SPRDLINE) = static_cast<uint16_t>(sw(SPRDLINE) + off);
            if (off == 1) break;
            if (off == 0) { loop = 4; break; }
          }
        }
        quadrant = (quadrant + 1) & 3;
      }
      // depósito de colisão do sprite
      if (!(sprcoll & 0x20) && !noCollide) {
        switch (sprctl0 & 7) {
          case XOR_SHADOW: case BOUNDARY: case NORMAL: case BOUNDARY_SHADOW: case SHADOW:
            ram[static_cast<uint16_t>(sw(SCBADR) + sw(COLLOFF))] = static_cast<uint8_t>(collision);
            break;
          default: break;
        }
      }
      if (everOn) {
        const uint16_t a = static_cast<uint16_t>(sw(SCBADR) + sw(COLLOFF));
        ram[a] = everOnScreen ? (ram[a] & 0x7F) : (ram[a] | 0x80);
      }
    }
  }
  sprGo = false;
  // pelo menos um tique por passo do laço: uma lista malformada não pode prender a CPU emulada sem tempo passar
  return std::max(spriteCost, work);
}

// ---------------------------------------------------------------- registradores
void Lynx::suzyWrite(uint8_t r, uint8_t v) {
  if (r < 0x30) {
    uint16_t& w = sw(r & 0x3E);
    if (r & 1) w = static_cast<uint16_t>((w & 0x00FF) | (v << 8));
    else w = v;  // gravar o byte baixo zera o alto
    return;
  }
  auto signFix = [&](uint8_t* lo, int& sign) {
    if (!signedMath) return;
    uint16_t w = static_cast<uint16_t>(lo[0] | (lo[1] << 8));
    // defeito do chip: $8000 conta como positivo e $0000 como negativo
    if (static_cast<uint16_t>(w - 1) & 0x8000) {
      w = static_cast<uint16_t>(~w + 1);
      sign = -1;
      lo[0] = static_cast<uint8_t>(w);
      lo[1] = static_cast<uint8_t>(w >> 8);
    } else {
      sign = 1;
    }
  };
  switch (r) {
    case 0x52: mathABCD[0] = v; mathABCD[1] = 0; signFix(&mathABCD[0], signCD); break;  // D (zera C)
    case 0x53: mathABCD[1] = v; signFix(&mathABCD[0], signCD); break;                   // C
    case 0x54: mathABCD[2] = v; mathABCD[3] = 0; break;                                 // B
    case 0x55: mathABCD[3] = v; signFix(&mathABCD[2], signAB); multiply(); break;       // A
    case 0x56: mathNP[0] = v; mathNP[1] = 0; break;
    case 0x57: mathNP[1] = v; break;
    case 0x60: mathEFGH[0] = v; mathEFGH[1] = 0; break;
    case 0x61: mathEFGH[1] = v; break;
    case 0x62: mathEFGH[2] = v; mathEFGH[3] = 0; break;
    case 0x63: mathEFGH[3] = v; divide(); break;
    case 0x6C: mathJKLM[0] = v; mathJKLM[1] = 0; mathBit = false; break;
    case 0x6D: mathJKLM[1] = v; break;
    case 0x6E: mathJKLM[2] = v; mathJKLM[3] = 0; break;
    case 0x6F: mathJKLM[3] = v; break;
    case 0x80: sprctl0 = v; break;
    case 0x81: sprctl1 = v; break;
    case 0x82: sprcoll = v; break;
    case 0x83: sprinit = v; break;
    case 0x90: suzyBusEn = v & 1; break;
    case 0x91:
      sprGo = v & 1;
      everOn = v & 4;
      if (sprGo && suzyBusEn && suzyBusyUntil <= now) suzyBusyUntil = now + paintSprites();
      break;
    case 0x92:
      stopOnCurrent = v & 2;
      if (v & 4) unsafe = false;
      leftHand = v & 8; vStretch = v & 0x10; noCollide = v & 0x20; accumulate = v & 0x40; signedMath = v & 0x80;
      break;
    case 0xB2: cartWrite(0, v); break;
    case 0xB3: cartWrite(1, v); break;
    default: break;
  }
}

uint8_t Lynx::suzyRead(uint8_t r) {
  if (r < 0x30) {
    const uint16_t w = sw(r & 0x3E);
    return static_cast<uint8_t>((r & 1) ? w >> 8 : w);
  }
  switch (r) {
    case 0x52: return mathABCD[0];
    case 0x53: return mathABCD[1];
    case 0x54: return mathABCD[2];
    case 0x55: return mathABCD[3];
    case 0x56: return mathNP[0];
    case 0x57: return mathNP[1];
    case 0x60: return mathEFGH[0];
    case 0x61: return mathEFGH[1];
    case 0x62: return mathEFGH[2];
    case 0x63: return mathEFGH[3];
    case 0x6C: return mathJKLM[0];
    case 0x6D: return mathJKLM[1];
    case 0x6E: return mathJKLM[2];
    case 0x6F: return mathJKLM[3];
    case 0x88: return 0x01;  // SUZYHREV
    case 0x92:
      return static_cast<uint8_t>((suzyBusyUntil > now ? 1 : 0) | (stopOnCurrent ? 2 : 0) | (unsafe ? 4 : 0) |
                                  (leftHand ? 8 : 0) | (vStretch ? 0x10 : 0) | (lastCarry ? 0x20 : 0) | (mathBit ? 0x40 : 0));
    case 0xB0: {
      uint8_t j = joystick;
      if (leftHand) {
        j = static_cast<uint8_t>((j & 0x0F) | ((j & 0x80) >> 1) | ((j & 0x40) << 1) | ((j & 0x20) >> 1) | ((j & 0x10) << 1));
      }
      return j;
    }
    case 0xB1: return switches;
    case 0xB2: return cartRead(0);
    case 0xB3: return cartRead(1);
    default: return 0xFF;
  }
}

}  // namespace indigo::lynx
