// Decodificadores do .chd: deflate, LZMA, FLAC, huffman do MAME, CRC16 e ECC de CD.
// Escritos do zero para o Índigo a partir das especificações dos formatos (RFC 1951; LZMA como descrito no
// LzmaSpec do LZMA SDK; formato FLAC de xiph.org; huffman/mapa do CHD como no MAME). Ver chd_codecs.h.
#include "cdrom/chd_codecs.h"

#include <algorithm>
#include <cstring>

namespace indigo::cdrom::chd {

// ---------------------------------------------------------------- CRC16 (CCITT, início 0xFFFF)

uint16_t crc16(const uint8_t* p, size_t n) {
  static uint16_t tab[256];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < 256; i++) {
      uint16_t c = uint16_t(i << 8);
      for (int k = 0; k < 8; k++) c = uint16_t(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
      tab[i] = c;
    }
    ready = true;
  }
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) crc = uint16_t((crc << 8) ^ tab[((crc >> 8) ^ p[i]) & 0xFF]);
  return crc;
}

// ---------------------------------------------------------------- BitReader / huffman do MAME

uint32_t BitReader::peek(int bits) const {
  if (bits <= 0) return 0;
  const size_t at = pos_ >> 3;
  if (at < n_ && n_ - at >= 8) {  // caminho rápido: 8 bytes inteiros disponíveis
    uint64_t w = 0;
    for (int i = 0; i < 8; i++) w = w << 8 | p_[at + size_t(i)];
    return uint32_t((w << (pos_ & 7)) >> (64 - bits));
  }
  uint32_t v = 0;
  size_t pos = pos_;
  for (int got = 0; got < bits;) {
    const size_t byte = pos >> 3;
    const int bit = int(pos & 7);
    const int take = bits - got < 8 - bit ? bits - got : 8 - bit;
    const uint32_t b = byte < n_ ? p_[byte] : 0;
    v = (v << take) | ((b >> (8 - bit - take)) & ((1u << take) - 1));
    got += take;
    pos += size_t(take);
  }
  return v;
}

bool Huffman::assignCodes() {
  uint32_t histo[33] = {};
  for (int i = 0; i < num_; i++) {
    if (bits_[size_t(i)] > maxBits_) return false;
    histo[bits_[size_t(i)]]++;
  }
  // códigos canônicos do MAME: os mais longos ficam com os números menores
  uint32_t start = 0;
  for (int len = 32; len > 0; len--) {
    const uint32_t next = (start + histo[len]) >> 1;
    if (len != 1 && next * 2 != start + histo[len]) return false;
    histo[len] = start;
    start = next;
  }
  for (int i = 0; i < num_; i++)
    if (bits_[size_t(i)] > 0) code_[size_t(i)] = histo[bits_[size_t(i)]]++;
  return true;
}

void Huffman::buildLookup() {
  std::fill(lookup_.begin(), lookup_.end(), uint16_t(0));
  for (int i = 0; i < num_; i++) {
    const int nb = bits_[size_t(i)];
    if (nb == 0) continue;
    const int shift = maxBits_ - nb;
    const size_t first = size_t(code_[size_t(i)]) << shift, last = (size_t(code_[size_t(i)] + 1) << shift) - 1;
    for (size_t k = first; k <= last && k < lookup_.size(); k++) lookup_[k] = uint16_t(i << 5 | nb);
  }
}

bool Huffman::importRle(BitReader& br) {
  const int numBits = maxBits_ >= 16 ? 5 : maxBits_ >= 8 ? 4 : 3;
  int cur = 0;
  while (cur < num_) {
    int nb = int(br.read(numBits));
    if (nb != 1) { bits_[size_t(cur++)] = uint8_t(nb); continue; }
    nb = int(br.read(numBits));
    if (nb == 1) { bits_[size_t(cur++)] = 1; continue; }
    int rep = int(br.read(numBits)) + 3;
    while (rep--) {
      if (cur >= num_) return false;
      bits_[size_t(cur++)] = uint8_t(nb);
    }
  }
  if (!assignCodes()) return false;
  buildLookup();
  return !br.overflow();
}

bool Huffman::importHuffman(BitReader& br) {
  Huffman small(24, 6);
  small.bits_[0] = uint8_t(br.read(3));
  const int start = int(br.read(3)) + 1;
  int count = 0;
  for (int i = 1; i < 24; i++) {
    if (i < start || count == 7) small.bits_[size_t(i)] = 0;
    else {
      count = int(br.read(3));
      small.bits_[size_t(i)] = uint8_t(count == 7 ? 0 : count);
    }
  }
  if (!small.assignCodes()) return false;
  small.buildLookup();
  uint32_t temp = uint32_t(num_ - 9);
  int rleBits = 0;
  while (temp) { temp >>= 1; rleBits++; }
  int last = 0, cur = 0;
  while (cur < num_) {
    const int v = int(small.decode(br));
    if (v != 0) { bits_[size_t(cur++)] = uint8_t(last = v - 1); continue; }
    int n = int(br.read(3)) + 2;
    if (n == 9) n += int(br.read(rleBits));
    for (; n != 0 && cur < num_; n--) bits_[size_t(cur++)] = uint8_t(last);
    if (br.overflow()) return false;
  }
  if (!assignCodes()) return false;
  buildLookup();
  return !br.overflow();
}

bool huffman8(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
  BitReader br(in, inLen);
  Huffman h(256, 16);
  if (!h.importHuffman(br)) return false;
  for (size_t i = 0; i < outLen; i++) out[i] = uint8_t(h.decode(br));
  return !br.overflow();
}

// ---------------------------------------------------------------- deflate (RFC 1951)

namespace {

struct LsbBits {
  const uint8_t* p;
  size_t n, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool err = false;
  LsbBits(const uint8_t* d, size_t l) : p(d), n(l) {}
  uint32_t bits(int k) {
    while (cnt < k) {
      if (pos >= n) { err = true; return 0; }
      buf |= uint32_t(p[pos++]) << cnt;
      cnt += 8;
    }
    const uint32_t v = buf & ((1u << k) - 1);
    buf >>= k;
    cnt -= k;
    return v;
  }
};

// tabela canônica: contagem por comprimento + símbolos ordenados; tabela rápida de 9 bits na frente
struct InfHuff {
  uint16_t count[16];
  uint16_t sym[320];
  uint16_t fast[512];  // símbolo << 4 | comprimento (0 = ir pelo caminho lento)
  bool build(const uint8_t* len, int n) {
    std::memset(count, 0, sizeof(count));
    for (int i = 0; i < n; i++) count[len[i]]++;
    if (count[0] == n) { std::memset(fast, 0, sizeof(fast)); return true; }  // tabela vazia: decode falha
    int left = 1;
    for (int l = 1; l < 16; l++) {
      left <<= 1;
      left -= count[l];
      if (left < 0) return false;  // códigos demais
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = uint16_t(offs[l] + count[l]);
    for (int i = 0; i < n; i++)
      if (len[i]) sym[offs[len[i]]++] = uint16_t(i);
    // tabela rápida: para cada código de até 9 bits, o padrão de bits invertido (o deflate lê do bit menos
    // significativo)
    std::memset(fast, 0, sizeof(fast));
    int code = 0, idx = 0;
    for (int l = 1; l <= 9; l++) {
      for (int k = 0; k < count[l]; k++, code++, idx++) {
        int rev = 0;
        for (int b = 0; b < l; b++) rev |= ((code >> b) & 1) << (l - 1 - b);
        for (int fill = rev; fill < 512; fill += 1 << l) fast[fill] = uint16_t(sym[idx] << 4 | l);
      }
      code <<= 1;
    }
    return true;
  }
  int decode(LsbBits& br) const {
    // tenta a tabela rápida com os bits que já estão no buffer (ou que dá para carregar)
    while (br.cnt < 9 && br.pos < br.n) { br.buf |= uint32_t(br.p[br.pos++]) << br.cnt; br.cnt += 8; }
    const uint16_t f = fast[br.buf & 511];
    if (f && (f & 15) <= br.cnt) {
      br.buf >>= (f & 15);
      br.cnt -= (f & 15);
      return f >> 4;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
      code |= int(br.bits(1));
      if (br.err) return -1;
      const int c = count[l];
      if (code - c < first) return sym[index + (code - first)];
      index += c;
      first += c;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }
};

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                               131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                12, 13, 13};

}  // namespace

bool inflateRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
  LsbBits br(in, inLen);
  size_t op = 0;
  InfHuff lit, dist;
  bool last = false;
  while (!last) {
    last = br.bits(1) != 0;
    const uint32_t type = br.bits(2);
    if (br.err) return false;
    if (type == 0) {  // bloco guardado
      // descarta os bits até o próximo byte; bytes inteiros já carregados no buffer voltam para a entrada
      br.pos -= size_t(br.cnt >> 3);
      br.buf = 0;
      br.cnt = 0;
      if (br.pos + 4 > br.n) return false;
      const uint32_t len = uint32_t(br.p[br.pos]) | uint32_t(br.p[br.pos + 1]) << 8;
      const uint32_t nlen = uint32_t(br.p[br.pos + 2]) | uint32_t(br.p[br.pos + 3]) << 8;
      br.pos += 4;
      if ((len ^ 0xFFFF) != nlen || br.pos + len > br.n || op + len > outLen) return false;
      std::memcpy(out + op, br.p + br.pos, len);
      br.pos += len;
      op += len;
      continue;
    }
    if (type == 3) return false;
    uint8_t lens[320];
    if (type == 1) {
      int i = 0;
      for (; i < 144; i++) lens[i] = 8;
      for (; i < 256; i++) lens[i] = 9;
      for (; i < 280; i++) lens[i] = 7;
      for (; i < 288; i++) lens[i] = 8;
      if (!lit.build(lens, 288)) return false;
      for (i = 0; i < 30; i++) lens[i] = 5;
      if (!dist.build(lens, 30)) return false;
    } else {
      const int nlen = int(br.bits(5)) + 257, ndist = int(br.bits(5)) + 1, ncode = int(br.bits(4)) + 4;
      if (br.err || nlen > 286 || ndist > 30) return false;
      static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t cl[19] = {};
      for (int i = 0; i < ncode; i++) cl[order[i]] = uint8_t(br.bits(3));
      InfHuff clh;
      if (br.err || !clh.build(cl, 19)) return false;
      int i = 0;
      while (i < nlen + ndist) {
        const int s = clh.decode(br);
        if (s < 0 || br.err) return false;
        if (s < 16) { lens[i++] = uint8_t(s); continue; }
        int rep = 0;
        uint8_t v = 0;
        if (s == 16) {
          if (i == 0) return false;
          v = lens[i - 1];
          rep = 3 + int(br.bits(2));
        } else if (s == 17) rep = 3 + int(br.bits(3));
        else rep = 11 + int(br.bits(7));
        if (br.err || i + rep > nlen + ndist) return false;
        while (rep--) lens[i++] = v;
      }
      if (lens[256] == 0) return false;
      if (!lit.build(lens, nlen) || !dist.build(lens + nlen, ndist)) return false;
    }
    for (;;) {
      const int s = lit.decode(br);
      if (s < 0 || br.err) return false;
      if (s < 256) {
        if (op >= outLen) return false;
        out[op++] = uint8_t(s);
        continue;
      }
      if (s == 256) break;
      const int li = s - 257;
      if (li >= 29) return false;
      const size_t len = kLenBase[li] + br.bits(kLenExtra[li]);
      const int ds = dist.decode(br);
      if (ds < 0 || ds >= 30 || br.err) return false;
      const size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
      if (br.err || d > op || op + len > outLen) return false;
      for (size_t k = 0; k < len; k++, op++) out[op] = out[op - d];
    }
  }
  return op == outLen;
}

// ---------------------------------------------------------------- LZMA

namespace {

struct RangeDec {
  const uint8_t* p;
  size_t n, pos = 0;
  uint32_t range = 0xFFFFFFFF, code = 0;
  bool err = false;
  uint8_t next() {
    if (pos >= n) { err = true; return 0; }
    return p[pos++];
  }
  bool init() {
    if (next() != 0) return false;
    for (int i = 0; i < 4; i++) code = code << 8 | next();
    return !err && code != range;
  }
  void norm() {
    if (range < (1u << 24)) { range <<= 8; code = code << 8 | next(); }
  }
  int bit(uint16_t& prob) {
    const uint32_t bound = (range >> 11) * prob;
    int b;
    if (code < bound) { prob = uint16_t(prob + ((2048 - prob) >> 5)); range = bound; b = 0; }
    else { prob = uint16_t(prob - (prob >> 5)); code -= bound; range -= bound; b = 1; }
    norm();
    return b;
  }
  uint32_t direct(int nb) {
    uint32_t res = 0;
    while (nb--) {
      range >>= 1;
      code -= range;
      const uint32_t t = 0u - (code >> 31);
      code += range & t;
      norm();
      res = (res << 1) + (t + 1);
    }
    return res;
  }
  uint32_t tree(uint16_t* probs, int nb) {
    uint32_t m = 1;
    for (int i = 0; i < nb; i++) m = (m << 1) + uint32_t(bit(probs[m]));
    return m - (1u << nb);
  }
  uint32_t treeRev(uint16_t* probs, int nb) {
    uint32_t m = 1, sym = 0;
    for (int i = 0; i < nb; i++) {
      const int b = bit(probs[m]);
      m = (m << 1) + uint32_t(b);
      sym |= uint32_t(b) << i;
    }
    return sym;
  }
};

struct LenDec {
  uint16_t choice = 1024, choice2 = 1024;
  uint16_t low[16][8], mid[16][8], high[256];
  LenDec() {
    for (auto& a : low) for (auto& v : a) v = 1024;
    for (auto& a : mid) for (auto& v : a) v = 1024;
    for (auto& v : high) v = 1024;
  }
  uint32_t decode(RangeDec& rc, int posState) {
    if (!rc.bit(choice)) return rc.tree(low[posState], 3);
    if (!rc.bit(choice2)) return 8 + rc.tree(mid[posState], 3);
    return 16 + rc.tree(high, 8);
  }
};

}  // namespace

bool lzmaRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen) {
  constexpr int lc = 3, lp = 0, pb = 2;
  RangeDec rc{in, inLen};
  if (!rc.init()) return false;
  std::vector<uint16_t> lit(size_t(0x300) << (lc + lp), 1024);
  uint16_t isMatch[12 << 4], isRep[12], isRepG0[12], isRepG1[12], isRepG2[12], isRep0Long[12 << 4];
  uint16_t posSlot[4][64], posDec[115], align[16];
  for (auto& v : isMatch) v = 1024;
  for (auto& v : isRep0Long) v = 1024;
  for (int i = 0; i < 12; i++) isRep[i] = isRepG0[i] = isRepG1[i] = isRepG2[i] = 1024;
  for (auto& a : posSlot) for (auto& v : a) v = 1024;
  for (auto& v : posDec) v = 1024;
  for (auto& v : align) v = 1024;
  LenDec lenDec, repLenDec;
  uint32_t rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
  unsigned state = 0;
  size_t op = 0;
  while (op < outLen) {
    if (rc.err) return false;
    const int posState = int(op & ((1u << pb) - 1));
    if (!rc.bit(isMatch[(state << 4) + unsigned(posState)])) {
      const unsigned prev = op ? out[op - 1] : 0;
      uint16_t* probs = &lit[size_t(0x300) * (((op & ((1u << lp) - 1)) << lc) + (prev >> (8 - lc)))];
      unsigned sym = 1;
      if (state >= 7) {
        if (rep0 >= op) return false;
        unsigned matchByte = out[op - rep0 - 1];
        do {
          const unsigned mb = (matchByte >> 7) & 1;
          matchByte <<= 1;
          const unsigned b = unsigned(rc.bit(probs[((1 + mb) << 8) + sym]));
          sym = (sym << 1) | b;
          if (mb != b) break;
        } while (sym < 0x100);
      }
      while (sym < 0x100) sym = (sym << 1) | unsigned(rc.bit(probs[sym]));
      out[op++] = uint8_t(sym - 0x100);
      state = state < 4 ? 0 : state < 10 ? state - 3 : state - 6;
      continue;
    }
    uint32_t len;
    if (rc.bit(isRep[state])) {
      if (op == 0) return false;
      if (!rc.bit(isRepG0[state])) {
        if (!rc.bit(isRep0Long[(state << 4) + unsigned(posState)])) {
          state = state < 7 ? 9 : 11;
          if (rep0 >= op) return false;
          out[op] = out[op - rep0 - 1];
          op++;
          continue;
        }
      } else {
        uint32_t d;
        if (!rc.bit(isRepG1[state])) d = rep1;
        else {
          if (!rc.bit(isRepG2[state])) d = rep2;
          else { d = rep3; rep3 = rep2; }
          rep2 = rep1;
        }
        rep1 = rep0;
        rep0 = d;
      }
      len = repLenDec.decode(rc, posState);
      state = state < 7 ? 8 : 11;
    } else {
      rep3 = rep2; rep2 = rep1; rep1 = rep0;
      len = lenDec.decode(rc, posState);
      state = state < 7 ? 7 : 10;
      const uint32_t ls = len < 3 ? len : 3;
      const uint32_t slot = rc.tree(posSlot[ls], 6);
      if (slot < 4) rep0 = slot;
      else {
        const int nd = int(slot >> 1) - 1;
        uint32_t dist = (2 | (slot & 1)) << nd;
        if (slot < 14) dist += rc.treeRev(posDec + dist - slot, nd);
        else {
          dist += rc.direct(nd - 4) << 4;
          dist += rc.treeRev(align, 4);
        }
        rep0 = dist;
      }
      if (rep0 == 0xFFFFFFFF) break;  // marcador de fim
      if (rep0 >= op) return false;
    }
    len += 2;
    if (rep0 >= op) return false;
    for (uint32_t k = 0; k < len && op < outLen; k++, op++) out[op] = out[op - rep0 - 1];
  }
  return op == outLen && !rc.err;
}

// ---------------------------------------------------------------- FLAC

namespace {

struct FlacBits {
  const uint8_t* p;
  size_t n, pos = 0;  // pos em bits
  bool err = false;
  uint32_t read(int k) {  // até 32 bits
    if (k == 0) return 0;
    if (pos + size_t(k) > n * 8) { err = true; pos = n * 8; return 0; }
    const size_t at = pos >> 3;
    if (n - at >= 8) {  // caminho rápido: 8 bytes inteiros disponíveis
      uint64_t w = 0;
      for (int i = 0; i < 8; i++) w = w << 8 | p[at + size_t(i)];
      const uint32_t v = uint32_t((w << (pos & 7)) >> (64 - k));
      pos += size_t(k);
      return v;
    }
    uint64_t v = 0;
    int got = 0;
    while (got < k) {
      const size_t byte = pos >> 3;
      const int bit = int(pos & 7);
      const int take = k - got < 8 - bit ? k - got : 8 - bit;
      v = (v << take) | ((p[byte] >> (8 - bit - take)) & ((1u << take) - 1));
      got += take;
      pos += size_t(take);
    }
    return uint32_t(v);
  }
  int32_t readSigned(int k) {
    if (k == 0) return 0;
    const uint32_t v = read(k);
    if (k == 32) return int32_t(v);
    const uint32_t sign = 1u << (k - 1);
    return int32_t((v ^ sign) - sign);
  }
  uint32_t unary() {  // conta zeros até o 1
    uint32_t z = 0;
    for (;;) {
      if (pos >= n * 8) { err = true; return 0; }
      const size_t byte = pos >> 3;
      const int bit = int(pos & 7);
      const uint8_t b = uint8_t(p[byte] << bit);
      if (b == 0) { z += uint32_t(8 - bit); pos += size_t(8 - bit); continue; }
      int lead = 0;
      while (!(b & (0x80 >> lead))) lead++;
      z += uint32_t(lead);
      pos += size_t(lead) + 1;
      return z;
    }
  }
  void align() { pos = (pos + 7) & ~size_t(7); }
};

uint8_t crc8(const uint8_t* p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = uint8_t(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
  }
  return c;
}
uint16_t crc16flac(const uint8_t* p, size_t n) {
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c = uint16_t(c ^ (p[i] << 8));
    for (int k = 0; k < 8; k++) c = uint16_t(c & 0x8000 ? (c << 1) ^ 0x8005 : c << 1);
  }
  return c;
}

bool flacResidual(FlacBits& br, int order, int bs, int32_t* res) {
  const uint32_t method = br.read(2);
  if (method > 1) return false;
  const int pbits = method == 0 ? 4 : 5, esc = method == 0 ? 15 : 31;
  const int porder = int(br.read(4));
  const int parts = 1 << porder;
  if ((bs & (parts - 1)) != 0) return false;
  const int psz = bs >> porder;
  if (psz < order) return false;
  int i = 0;
  for (int pi = 0; pi < parts; pi++) {
    const int cnt = pi == 0 ? psz - order : psz;
    const int param = int(br.read(pbits));
    if (br.err) return false;
    if (param == esc) {
      const int nb = int(br.read(5));
      for (int k = 0; k < cnt; k++) res[i++] = br.readSigned(nb);
    } else {
      for (int k = 0; k < cnt; k++) {
        const uint32_t q = br.unary();
        if (br.err || q > (0xFFFFFFFFu >> param)) return false;
        const uint32_t v = (q << param) | br.read(param);
        res[i++] = int32_t(v >> 1) ^ -int32_t(v & 1);
      }
    }
    if (br.err) return false;
  }
  return true;
}

bool flacSubframe(FlacBits& br, int bps, int bs, int32_t* s) {
  if (br.read(1) != 0) return false;
  const int type = int(br.read(6));
  int wasted = 0;
  if (br.read(1)) {
    wasted = int(br.unary()) + 1;
    if (wasted >= bps) return false;
    bps -= wasted;
  }
  if (br.err) return false;
  if (type == 0) {
    const int32_t v = br.readSigned(bps);
    for (int i = 0; i < bs; i++) s[i] = v;
  } else if (type == 1) {
    for (int i = 0; i < bs; i++) s[i] = br.readSigned(bps);
  } else if (type >= 8 && type <= 12) {
    const int order = type - 8;
    if (order > bs) return false;
    for (int i = 0; i < order; i++) s[i] = br.readSigned(bps);
    if (!flacResidual(br, order, bs, s + order)) return false;
    for (int i = order; i < bs; i++) {
      int64_t p = 0;
      switch (order) {
        case 1: p = s[i - 1]; break;
        case 2: p = 2 * int64_t(s[i - 1]) - s[i - 2]; break;
        case 3: p = 3 * int64_t(s[i - 1]) - 3 * int64_t(s[i - 2]) + s[i - 3]; break;
        case 4: p = 4 * int64_t(s[i - 1]) - 6 * int64_t(s[i - 2]) + 4 * int64_t(s[i - 3]) - s[i - 4]; break;
        default: break;
      }
      s[i] = int32_t(p + s[i]);
    }
  } else if (type >= 32) {
    const int order = type - 31;
    if (order > bs) return false;
    for (int i = 0; i < order; i++) s[i] = br.readSigned(bps);
    const int prec = int(br.read(4)) + 1;
    if (prec == 16) return false;
    const int shift = br.readSigned(5);
    if (shift < 0) return false;
    int32_t coef[32];
    for (int i = 0; i < order; i++) coef[i] = br.readSigned(prec);
    if (br.err || !flacResidual(br, order, bs, s + order)) return false;
    for (int i = order; i < bs; i++) {
      int64_t sum = 0;
      for (int j = 0; j < order; j++) sum += int64_t(coef[j]) * s[i - 1 - j];
      s[i] = int32_t((sum >> shift) + s[i]);
    }
  } else {
    return false;
  }
  if (wasted)
    for (int i = 0; i < bs; i++) s[i] = int32_t(uint32_t(s[i]) << wasted);
  return !br.err;
}

}  // namespace

bool flacFrames(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, bool bigEndian, size_t* consumed) {
  FlacBits br{in, inLen};
  const size_t want = outLen / 4;  // amostras estéreo
  size_t done = 0;
  std::vector<int32_t> ch[2];
  while (done < want) {
    const size_t start = br.pos >> 3;
    if (br.read(15) != 0x7FFC) return false;  // sincronismo (14 bits) + bit reservado
    br.read(1);                               // estratégia de bloco (fixo/variável): não importa aqui
    const int bsCode = int(br.read(4)), srCode = int(br.read(4));
    const int chan = int(br.read(4)), ssCode = int(br.read(3));
    if (br.read(1) != 0 || br.err) return false;
    // número do quadro/amostra em "UTF-8"
    uint32_t b0 = br.read(8);
    int extra = 0;
    if (b0 & 0x80) {
      if ((b0 & 0xE0) == 0xC0) extra = 1;
      else if ((b0 & 0xF0) == 0xE0) extra = 2;
      else if ((b0 & 0xF8) == 0xF0) extra = 3;
      else if ((b0 & 0xFC) == 0xF8) extra = 4;
      else if ((b0 & 0xFE) == 0xFC) extra = 5;
      else if (b0 == 0xFE) extra = 6;
      else return false;
    }
    for (int i = 0; i < extra; i++)
      if ((br.read(8) & 0xC0) != 0x80) return false;
    int bs;
    if (bsCode == 0) return false;
    else if (bsCode == 1) bs = 192;
    else if (bsCode <= 5) bs = 576 << (bsCode - 2);
    else if (bsCode == 6) bs = int(br.read(8)) + 1;
    else if (bsCode == 7) bs = int(br.read(16)) + 1;
    else bs = 256 << (bsCode - 8);
    if (srCode == 12) br.read(8);
    else if (srCode == 13 || srCode == 14) br.read(16);
    else if (srCode == 15) return false;
    int bps;
    switch (ssCode) {
      case 0: case 4: bps = 16; break;  // 0 = o do STREAMINFO, que no CHD é sempre 16
      default: return false;            // o CHD só usa 16 bits
    }
    if (br.err) return false;
    const size_t hdrEnd = br.pos >> 3;
    if (uint8_t(br.read(8)) != crc8(in + start, hdrEnd - start) || br.err) return false;
    if (chan != 1 && chan < 8) return false;  // só estéreo
    if (chan > 10) return false;
    for (auto& c : ch) c.resize(size_t(bs));
    for (int c = 0; c < 2; c++) {
      const bool side = (chan == 8 && c == 1) || (chan == 9 && c == 0) || (chan == 10 && c == 1);
      if (!flacSubframe(br, bps + (side ? 1 : 0), bs, ch[c].data())) return false;
    }
    br.align();
    const size_t crcPos = br.pos >> 3;
    const uint32_t crc = br.read(16);
    if (br.err || crc != crc16flac(in + start, crcPos - start)) return false;
    int32_t* a = ch[0].data();
    int32_t* b = ch[1].data();
    for (int i = 0; i < bs && done < want; i++, done++) {
      int32_t l, r;
      if (chan == 8) { l = a[i]; r = a[i] - b[i]; }
      else if (chan == 9) { r = b[i]; l = a[i] + b[i]; }
      else if (chan == 10) {
        const int32_t mid = int32_t(uint32_t(a[i]) << 1) | (b[i] & 1);
        l = (mid + b[i]) >> 1;
        r = (mid - b[i]) >> 1;
      } else { l = a[i]; r = b[i]; }
      uint8_t* o = out + done * 4;
      const uint16_t ul = uint16_t(l), ur = uint16_t(r);
      if (bigEndian) { o[0] = uint8_t(ul >> 8); o[1] = uint8_t(ul); o[2] = uint8_t(ur >> 8); o[3] = uint8_t(ur); }
      else { o[0] = uint8_t(ul); o[1] = uint8_t(ul >> 8); o[2] = uint8_t(ur); o[3] = uint8_t(ur >> 8); }
    }
  }
  if (consumed) *consumed = br.pos >> 3;
  return true;
}

// ---------------------------------------------------------------- ECC de setor Mode 1

namespace {
struct EccTables {
  uint8_t f[256], b[256];
  EccTables() {
    for (int i = 0; i < 256; i++) {
      const int j = (i << 1) ^ (i & 0x80 ? 0x11D : 0);
      f[i] = uint8_t(j);
      b[i ^ j] = uint8_t(i);
    }
  }
};
const EccTables& ecc() {
  static const EccTables t;
  return t;
}
void eccBlock(const uint8_t* src, uint32_t majorCount, uint32_t minorCount, uint32_t majorMult, uint32_t minorInc,
              uint8_t* dest) {
  const EccTables& t = ecc();
  const uint32_t size = majorCount * minorCount;
  for (uint32_t major = 0; major < majorCount; major++) {
    uint32_t index = (major >> 1) * majorMult + (major & 1);
    uint8_t ea = 0, eb = 0;
    for (uint32_t minor = 0; minor < minorCount; minor++) {
      const uint8_t v = src[index];
      index += minorInc;
      if (index >= size) index -= size;
      ea ^= v;
      eb ^= v;
      ea = t.f[ea];
    }
    ea = t.b[t.f[ea] ^ eb];
    dest[major] = ea;
    dest[major + majorCount] = uint8_t(ea ^ eb);
  }
}
}  // namespace

void eccGenerate(uint8_t* s) {
  // no Mode 2 o cabeçalho (endereço e modo) entra na conta como zeros
  uint8_t hdr[4];
  const bool mode2 = s[15] == 2;
  if (mode2) { std::memcpy(hdr, s + 12, 4); std::memset(s + 12, 0, 4); }
  eccBlock(s + 0xC, 86, 24, 2, 86, s + 0x81C);   // P
  eccBlock(s + 0xC, 52, 43, 86, 88, s + 0x8C8);  // Q (cobre também os bytes P)
  if (mode2) std::memcpy(s + 12, hdr, 4);
}

}  // namespace indigo::cdrom::chd
