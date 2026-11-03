// Partes adaptadas do Mednafen (Beetle PC-FX, Mednafen Team, GPL-2.0 ou posterior); veja native/TERCEIROS.md.
#include "rainbow.h"

#include <cmath>
#include <cstring>

namespace indigo::pcfx {

namespace {
// ---- tabelas de Huffman fixas do chip
constexpr uint16_t N = 0xFFFF;
constexpr uint8_t dcYBase[17] = {0, 0, 0, 0, 7, 0, 8, 9, 0, 11, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t dcYCodes[27] = {0x00, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x01, 0x08, 0x09, 0x0F, 0x10, 0x11, 0x12,
                                  0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
constexpr uint16_t dcYMin[17] = {0, 0, 0, 0, 0x0E, 0, 0x3C, 0x7A, 0, 0x1F0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint16_t dcYMax[17] = {N, N, N, 0x06, 0x0E, N, 0x3C, 0x7B, N, 0x1FF, N, N, N, N, N, N, N};

constexpr uint8_t dcUVBase[17] = {0, 0, 0, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint8_t dcUVCodes[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
constexpr uint16_t dcUVMin[17] = {0, 0, 0, 0x06, 0x0E, 0x1E, 0x3E, 0x7E, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr uint16_t dcUVMax[17] = {N, N, 0x02, 0x06, 0x0E, 0x1E, 0x3E, 0x7E, 0xFF, N, N, N, N, N, N, N, N};

// AC: cada valor é (corrida de zeros << 4) | tamanho do coeficiente em bits
#define RB_AC_TAIL                                                                                                    \
  0x08, 0x14, 0x15, 0x16, 0x17, 0x18, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,   \
      0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x62, 0x63, 0x64, 0x65, 0x66, \
      0x67, 0x68, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78
#define RB_AC_HIGH                                                                                                    \
  0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xB1, 0xB2, 0xB3,   \
      0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, \
      0xD7, 0xD8, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0x10
#define RB_AC_NINE 0x09, 0x19, 0x29, 0x39, 0x49, 0x59, 0x69, 0x79, 0x89, 0x99, 0xA9, 0xB9, 0xC9, 0xD9, 0xE9, 0xF9
constexpr uint8_t acYBase[17] = {0, 0, 0, 2, 3, 5, 9, 13, 0, 16, 0, 18, 34, 0, 0, 0, 0};
constexpr uint8_t acYCodes[146] = {0x01, 0x02, 0x03, 0x04, 0x11, 0x05, 0x12, 0x21, 0x00, 0x06, 0x31, 0x41, 0x51, 0x13, 0x22,
                                   0x61, 0x07, 0x71, RB_AC_NINE, RB_AC_TAIL, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
                                   RB_AC_HIGH};
constexpr uint16_t acYMin[17] = {0, 0, 0, 0x004, 0x00A, 0x018, 0x036, 0x074, 0, 0x1DC, 0, 0x778, 0xF10, 0, 0, 0, 0};
constexpr uint16_t acYMax[17] = {N, N, 0x001, 0x004, 0x00B, 0x01A, 0x039, 0x076, N, 0x1DD, N, 0x787, 0xF7F, N, N, N, N};

constexpr uint8_t acUVBase[17] = {0, 0, 0, 2, 3, 5, 10, 11, 0, 16, 0, 18, 34, 0, 0, 0, 0};
constexpr uint8_t acUVCodes[146] = {0x01, 0x02, 0x11, 0x03, 0x21, 0x04, 0x12, 0x31, 0x41, 0x00, 0x51, 0x05, 0x13, 0x22, 0x61,
                                    0x71, 0x32, 0x81, RB_AC_NINE, 0x06, 0x07, 0x08, 0x14, 0x15, 0x16, 0x17, 0x18, 0x23, 0x24,
                                    0x25, 0x26, 0x27, 0x28, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x42, 0x43, 0x44, 0x45, 0x46,
                                    0x47, 0x48, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
                                    0x68, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
                                    RB_AC_HIGH};
constexpr uint16_t acUVMin[17] = {0, 0, 0, 0x004, 0x00A, 0x018, 0x038, 0x072, 0, 0x1DC, 0, 0x778, 0xF10, 0, 0, 0, 0};
constexpr uint16_t acUVMax[17] = {N, N, 0x001, 0x004, 0x00B, 0x01B, 0x038, 0x076, N, 0x1DD, N, 0x787, 0xF7F, N, N, N, N};
#undef RB_AC_TAIL
#undef RB_AC_HIGH
#undef RB_AC_NINE

inline uint8_t clampU8(int32_t v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
}  // namespace

const RbHuff RB_DC_Y = {dcYBase, dcYCodes, dcYMin, dcYMax};
const RbHuff RB_DC_UV = {dcUVBase, dcUVCodes, dcUVMin, dcUVMax};
const RbHuff RB_AC_Y = {acYBase, acYCodes, acYMin, acYMax};
const RbHuff RB_AC_UV = {acUVBase, acUVCodes, acUVMin, acUVMax};
const uint8_t RB_ZIGZAG[63] = {1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                               41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15,
                               23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// tabela de consulta: os "bits" primeiros bits do fluxo dão o valor e o tamanho do código (0 = inválido)
struct RbLut {
  int bits;
  uint8_t val[4096];
  uint8_t len[4096];
  RbLut(const RbHuff& t, int b) : bits(b), val{}, len{} {
    for (int n = 2; n <= b; n++) {
      if (t.max[n] == 0xFFFF) continue;
      for (uint32_t c = t.min[n]; c <= t.max[n]; c++)
        for (uint32_t r = 0; r < (1u << (b - n)); r++) {
          val[(c << (b - n)) + r] = t.codes[t.base[n] + (c - t.min[n])];
          len[(c << (b - n)) + r] = uint8_t(n);
        }
    }
  }
};

namespace {
const RbLut& lutDcY() { static const RbLut t(RB_DC_Y, 9); return t; }
const RbLut& lutDcUV() { static const RbLut t(RB_DC_UV, 8); return t; }
const RbLut& lutAcY() { static const RbLut t(RB_AC_Y, 12); return t; }
const RbLut& lutAcUV() { static const RbLut t(RB_AC_UV, 12); return t; }

// cossenos da IDCT em ponto fixo de 13 bits: c[x][u] = C(u)/2 * cos((2x+1)u*pi/16), C(0) = 1/raiz(2)
struct IdctTable {
  int32_t c[8][8];
  IdctTable() {
    const double pi = 3.14159265358979323846;
    for (int x = 0; x < 8; x++)
      for (int u = 0; u < 8; u++) {
        const double cu = u ? 0.5 : 0.5 / std::sqrt(2.0);
        c[x][u] = int32_t(std::lround(8192.0 * cu * std::cos((2 * x + 1) * u * pi / 16.0)));
      }
  }
};
const IdctTable& idctTable() { static const IdctTable t; return t; }
}  // namespace

void rbIdct(int32_t* blk) {
  const auto& c = idctTable().c;
  int64_t tmp[64];
  for (int v = 0; v < 8; v++)  // linhas: tmp[v][x] = soma em u
    for (int x = 0; x < 8; x++) {
      int64_t s = 0;
      for (int u = 0; u < 8; u++) s += int64_t(c[x][u]) * blk[v * 8 + u];
      tmp[v * 8 + x] = s;
    }
  for (int x = 0; x < 8; x++)  // colunas
    for (int y = 0; y < 8; y++) {
      int64_t s = 0;
      for (int v = 0; v < 8; v++) s += int64_t(c[y][v]) * tmp[v * 8 + x];
      blk[y * 8 + x] = int32_t((s + (int64_t(1) << 25)) >> 26);
    }
}

// ------------------------------------------------------------------ registros (portas 0x200..0x2FF)
void Rainbow::reset() {
  control_ = nullY_ = nullU_ = nullV_ = hsync_ = hscroll_ = 0;
  which_ = readPos_ = 0;
  fmt_[0] = fmt_[1] = -1;
  firstDecode_ = true;
  garbage_ = false;
  std::memset(quant_, 0, sizeof quant_);
  std::memset(quantBase_, 0, sizeof quantBase_);
  std::memset(buf_, 0, sizeof buf_);
}

namespace {
inline void setByte(uint16_t& r, uint8_t v, int which) { r = uint16_t((r & ~(0xFF << (which * 8))) | (v << (which * 8))); }
}  // namespace

// a rolagem horizontal é montada byte a byte: D0-D7 em 0x200 dão o byte de baixo e em 0x202 o de cima
void Rainbow::write8(uint32_t a, uint8_t v) {
  if ((a & 2) && (a & 0x1C)) return;  // metade de cima dos outros registros: não existe
  switch (a & 0x1C) {
    case 0x00: setByte(hscroll_, v, (a & 2) >> 1); hscroll_ &= 0x1FF; break;
    case 0x04: setByte(control_, v, a & 1); break;
    case 0x08: setByte(nullY_, v, a & 1); break;
    case 0x0C: setByte(nullU_, v, a & 1); break;
    case 0x10: setByte(nullV_, v, a & 1); break;
    case 0x14: setByte(hsync_, v, a & 1); break;
    default: break;
  }
}

void Rainbow::write16(uint32_t a, uint16_t v) {
  if (a & 2) {  // metade de cima dos registros de 32 bits: só a rolagem usa
    if ((a & 0x1C) == 0) { setByte(hscroll_, uint8_t(v), 1); hscroll_ &= 0x1FF; }
    return;
  }
  switch (a & 0x1C) {
    case 0x00: setByte(hscroll_, uint8_t(v), 0); hscroll_ &= 0x1FF; break;
    case 0x04: control_ = v; break;
    case 0x08: nullY_ = v; break;
    case 0x0C: nullU_ = v; break;
    case 0x10: nullV_ = v; break;
    case 0x14: hsync_ = v; break;
    default: break;
  }
}

// cor dos macroblocos "vazios": registros de corrida nula + 0x80 (Y, U, V)
uint32_t Rainbow::nullColor() const {
  return uint32_t(uint8_t(nullY_ + 0x80)) << 16 | uint32_t(uint8_t(nullU_ + 0x80)) << 8 | uint8_t(nullV_ + 0x80);
}

// ------------------------------------------------------------------ leitura do fluxo
uint8_t Rainbow::fetch() {
  const uint32_t p = *pos_;
  const uint8_t r = uint8_t(kram_[(p >> 1) & 0x3FFFF] >> ((p & 1) * 8));
  *pos_ = ((p + 1) & 0x3FFFF) | (p & 0x40000);
  return r;
}

// byte de dados: depois de 0xFF vem um byte de enchimento; acabado o bloco, só zeros
uint8_t Rainbow::fetchData() {
  if (left_ <= 0) return 0;
  const uint8_t r = fetch();
  if (r == 0xFF) fetch();
  left_--;
  return r;
}

uint32_t Rainbow::getBits(unsigned n, bool peek) {
  while (nbits_ < n) { bits_ = (bits_ << 8) | fetchData(); nbits_ += 8; }
  const uint32_t r = (bits_ >> (nbits_ - n)) & ((1u << n) - 1);
  if (!peek) nbits_ -= n;
  return r;
}

// valor com sinal no estilo JPEG: n bits; abaixo de 2^(n-1) é negativo
int32_t Rainbow::getSigned(unsigned n) {
  if (!n) return 0;
  const int32_t r = int32_t(getBits(n));
  return r < (1 << (n - 1)) ? r + 1 - (1 << n) : r;
}

int32_t Rainbow::acCoeff(const RbLut& t, int32_t& zeroes) {
  const uint32_t raw = getBits(12, true);
  if ((raw & 0xF80) == 0xF80) { nbits_ -= 5; zeroes = 0; return 0; }  // 11111 = fim do bloco
  const uint8_t code = t.val[raw];
  nbits_ -= t.len[raw];
  zeroes = code >> 4;
  return getSigned(code & 0xF);
}

int32_t Rainbow::dcY(int32_t& zeroes) {
  const RbLut& t = lutDcY();
  for (;;) {
    const uint32_t raw = getBits(9, true);
    const uint8_t code = t.val[raw];
    nbits_ -= t.len[raw];
    if (code < 0x0F) { zeroes = 0; return getSigned(code); }
    if (code == 0x0F) {  // colunas vazias: a contagem vem num código AC
      acCoeff(lutAcY(), zeroes);
      zeroes++;
      return 0;
    }
    // nova escala da quantização (o DC do UV não muda de escala)
    const uint32_t s = code - 0x10u;
    for (int i = 0; i < 64; i++) {
      uint32_t q = (quantBase_[0][i] * s) >> 2;
      quant_[0][i] = uint8_t(q < 1 ? 1 : q > 0xFE ? 0xFE : q);
      q = i ? (quantBase_[1][i] * s) >> 2 : quantBase_[1][i] >> 2u;
      quant_[1][i] = uint8_t(q < 1 ? 1 : q > 0xFE ? 0xFE : q);
    }
  }
}

int32_t Rainbow::dcUV() {
  const RbLut& t = lutDcUV();
  const uint32_t raw = getBits(8, true);
  nbits_ -= t.len[raw];
  return getSigned(t.val[raw]);
}

void Rainbow::decodeBlock8(int32_t* dct, const uint8_t* q, int32_t dc, const RbLut& t) {
  dct[0] = int16_t(uint16_t(uint32_t(q[0]) * uint32_t(dc)));
  int count = 0;
  while (count < 63) {
    int32_t zeroes = 0;
    const int32_t coef = acCoeff(t, zeroes);
    if (!coef) {
      if (!zeroes) {  // fim do bloco
        while (count < 63) dct[RB_ZIGZAG[count++]] = 0;
        break;
      }
      if (zeroes == 1) zeroes = 15;  // 16 zeros seguidos
    }
    while (zeroes-- > 0 && count < 63) dct[RB_ZIGZAG[count++]] = 0;
    if (count < 63) {
      const int i = RB_ZIGZAG[count++];
      dct[i] = int16_t(uint16_t(uint32_t(q[i]) * uint32_t(coef)));
    }
  }
}

// ------------------------------------------------------------------ descompressão de um bloco
void Rainbow::decodeBlock(bool first, const uint16_t* kram, uint32_t& pos) {
  if (!(control_ & 1)) return;
  kram_ = kram;
  pos_ = &pos;
  const int wb = which_ ^ 1;
  if (first) { firstDecode_ = true; garbage_ = false; }
  // procura o cabeçalho (marcador 0xFF + tipo) em até 512 bytes
  int icount = garbage_ ? 0 : 0x200;
  uint8_t type = 0;
  int32_t size = 0;
  do {
    do {
      while (fetch() != 0xFF && icount > 0) icount--;
      type = fetch();
      icount--;
    } while (type != 0xF0 && type != 0xF1 && type != 0xF2 && type != 0xF3 && type != 0xF8 && type != 0xFF && icount > 0);
    const uint8_t hi = fetch(), lo = fetch();
    size = int16_t(uint16_t(hi << 8 | lo)) - 2;
    if (type == 0xFF && size <= 0)
      for (int i = 0; i < 128; i++, icount--) fetch();
  } while (size <= 0 && icount > 0);

  if (icount <= 0) {  // lixo: buffer vazio de paleta
    garbage_ = true;
    fmt_[wb] = 0;
    std::memset(buf_[wb], 0, sizeof buf_[wb]);
    pos_ = nullptr;
    return;
  }

  if (type == 0xF8 || type == 0xFF) {
    fmt_[wb] = 1;
    if (type == 0xFF) {
      for (int t = 0; t < 2; t++)
        for (int i = 0; i < 64; i++) quant_[t][i] = quantBase_[t][i] = fetch();
      size -= 128;
    }
    left_ = size;
    bits_ = nbits_ = 0;
    int32_t dcy = 0, dcu = 0, dcv = 0;
    uint32_t* px = buf_[wb];
    for (int col = 0; col < 16; col++) {
      int32_t zeroes = 0;
      dcy += dcY(zeroes);
      if (zeroes) {  // colunas vazias com a cor da corrida nula
        const uint32_t nc = nullColor();
        for (; zeroes > 0; zeroes--, col++)
          if (col < 16)
            for (int y = 0; y < 16; y++)
              for (int x = 0; x < 16; x++) px[y * 256 + col * 16 + x] = nc;
        col--;
        dcy = dcu = dcv = 0;
        continue;
      }
      int32_t y4[256], u[64], v[64];
      decodeBlock8(&y4[0], quant_[0], dcy, lutAcY());     // A: em cima, à esquerda
      dcy += dcY(zeroes);
      decodeBlock8(&y4[64], quant_[0], dcy, lutAcY());    // B: embaixo, à esquerda
      dcy += dcY(zeroes);
      decodeBlock8(&y4[128], quant_[0], dcy, lutAcY());   // C: em cima, à direita
      dcy += dcY(zeroes);
      decodeBlock8(&y4[192], quant_[0], dcy, lutAcY());   // D: embaixo, à direita
      dcu += dcUV();
      decodeBlock8(u, quant_[1], dcu, lutAcUV());
      dcv += dcUV();
      decodeBlock8(v, quant_[1], dcv, lutAcUV());
      for (int b = 0; b < 4; b++) rbIdct(&y4[b * 64]);
      rbIdct(u);
      rbIdct(v);
      uint32_t* d = px + col * 16;
      for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++) {
          const int c = (y >> 1) * 8 + (x >> 1);
          d[y * 256 + x] = uint32_t(clampU8(y4[y * 8 + (x & 7) + ((x & 8) << 4)] + 0x80)) << 16 |
                           uint32_t(clampU8(u[c] + 0x80)) << 8 | clampU8(v[c] + 0x80);
        }
    }
  } else {
    // paleta com repetição: F0 = 4 bits de contagem, F1 = 3, F2 = 2, F3 = 1
    fmt_[wb] = 0;
    const unsigned shift = 4 - (type & 3);
    const unsigned mask = (1u << shift) - 1;
    uint8_t* out = reinterpret_cast<uint8_t*>(buf_[wb]);
    int x = 0;
    while (size > 0) {
      const uint8_t b = fetch();
      size--;
      if (b == 0xFF) { fetch(); size--; }
      unsigned n;
      if (!(b & mask)) {
        n = fetch();
        size--;
        if (n == 0xFF) { fetch(); size--; }
        n++;
      } else {
        n = b & mask;
      }
      for (unsigned i = 0; i < n && x < 0x2000; i++) out[x++] = uint8_t(b >> shift);
    }
  }
  pos_ = nullptr;
}

int Rainbow::fetchRaster(uint32_t* line, const uint32_t* pal, uint32_t palOff, uint32_t layerOr) {
  const int f = fmt_[which_];
  if (line) {
    const bool endless = control_ & 2;
    if (f == 1) {
      uint32_t* in = buf_[which_] + readPos_ * 256;
      uint32_t s = hscroll_;
      for (int x = 0; x < 256; x++, s++) {
        if (endless) line[x] = in[s & 0xFF] | layerOr;
        else line[x] = (s & 0x1FF) < 256 ? (in[s & 0x1FF] | layerOr) : 0;
      }
      std::memset(in, 0, 256 * 4);  // a linha lida volta a zero
    } else if (f == 0) {
      const uint8_t* in = reinterpret_cast<const uint8_t*>(buf_[which_]) + readPos_ * 256;
      uint32_t s = hscroll_;
      for (int x = 0; x < 256; x++, s++) {
        const uint32_t i = endless ? (s & 0xFF) : (s & 0x1FF);
        line[x] = (i < 256 && in[i]) ? (pal[(palOff + in[i]) & 0x1FF] | layerOr) : 0;
      }
    } else {
      std::memset(line, 0, 256 * 4);
    }
  }
  readPos_ = (readPos_ + 1) & 15;
  if (!readPos_) fmt_[which_] = -1;  // as 16 linhas do buffer foram usadas
  return f;
}

void Rainbow::sync(StateIO& io) {
  io.sync(control_); io.sync(nullY_); io.sync(nullU_); io.sync(nullV_); io.sync(hsync_); io.sync(hscroll_);
  io.sync(which_); io.sync(readPos_); io.raw(fmt_, sizeof fmt_);
  io.sync(firstDecode_); io.sync(garbage_);
  io.raw(quant_, sizeof quant_); io.raw(quantBase_, sizeof quantBase_);
  io.raw(buf_, sizeof buf_);
  if (io.reading()) {
    which_ &= 1;
    readPos_ &= 15;
    hscroll_ &= 0x1FF;
    for (auto& f : fmt_) if (f < -1 || f > 1) f = -1;
  }
}

}  // namespace indigo::pcfx
