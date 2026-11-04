// Leitor de .chd (MAME, versão 5) para CD. Formato (como gravado pelo chdman do MAME):
//   cabeçalho de 124 bytes ("MComprHD", versão 5, 4 codecs, tamanho lógico, posição do mapa e dos metadados,
//   bytes por hunk); mapa com uma entrada por hunk (comprimido com huffman + CRC16 na versão 5); metadados em
//   lista encadeada (as faixas ficam nos textos CHT2/CHTR).
// Cada hunk tem N quadros de 2352 bytes de setor + 96 de subcódigo. Cada faixa começa num quadro múltiplo de 4
// (o chdman completa a faixa anterior com quadros vazios), então o quadro dentro do CHD não é o LBA.
// O pregap com PGTYPE começando com "V" está dentro da imagem; sem o "V" (e o POSTGAP) não está, e é lido como
// zeros, como o PREGAP/POSTGAP do .cue. Áudio fica em big-endian no CHD e sai em little-endian, como no .bin.
#include "cdrom/chd.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include "cdrom/chd_codecs.h"

namespace indigo::cdrom {

namespace {

constexpr uint32_t FRAME = 2448;  // 2352 de setor + 96 de subcódigo
constexpr uint32_t SUB = 96;
constexpr uint32_t PAD = 4;       // faixas alinhadas em múltiplos de 4 quadros

constexpr uint32_t tag(const char* s) {
  return uint32_t(uint8_t(s[0])) << 24 | uint32_t(uint8_t(s[1])) << 16 | uint32_t(uint8_t(s[2])) << 8 | uint8_t(s[3]);
}
uint32_t be16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t be24(const uint8_t* p) { return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2]; }
uint32_t be32(const uint8_t* p) { return be16(p) << 16 | be16(p + 2); }
uint64_t be48(const uint8_t* p) { return uint64_t(be16(p)) << 32 | be32(p + 2); }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

enum Codec { C_NONE, C_ZLIB, C_LZMA, C_HUFF, C_FLAC, C_CDZL, C_CDLZ, C_CDFL, C_UNKNOWN };
Codec codecOf(uint32_t t) {
  switch (t) {
    case 0: return C_NONE;
    case tag("zlib"): return C_ZLIB;
    case tag("lzma"): return C_LZMA;
    case tag("huff"): return C_HUFF;
    case tag("flac"): return C_FLAC;
    case tag("cdzl"): return C_CDZL;
    case tag("cdlz"): return C_CDLZ;
    case tag("cdfl"): return C_CDFL;
    default: return C_UNKNOWN;
  }
}
std::string tagName(uint32_t t) {
  if (t == 0) return "";
  std::string s;
  for (int i = 3; i >= 0; i--) {
    const char c = char(t >> (8 * i));
    s += (c >= 32 && c < 127) ? c : '?';
  }
  return s;
}

// tipos de compressão das entradas do mapa da versão 5
enum : uint8_t {
  M_TYPE0 = 0, M_TYPE3 = 3, M_NONE = 4, M_SELF = 5, M_PARENT = 6, M_RLE_SMALL = 7, M_RLE_LARGE = 8,
  M_SELF_0 = 9, M_SELF_1 = 10, M_PARENT_SELF = 11, M_PARENT_0 = 12, M_PARENT_1 = 13
};

// como cada quadro da faixa vira o setor bruto de 2352 bytes
enum class Kind { Raw, Mode1Cooked, Mode2_2336, Form1Cooked, Form2_2324 };

struct ChdTrack {
  uint32_t dataLba = 0;   // LBA do primeiro quadro guardado (inclui o pregap "V")
  uint32_t frames = 0;    // quadros guardados
  uint32_t chdFrame = 0;  // quadro dentro do CHD
  Kind kind = Kind::Raw;
  bool audio = false;
};

uint8_t bcd(int v) { return uint8_t((v / 10) << 4 | (v % 10)); }
void header(uint32_t lba, uint8_t mode, uint8_t* raw) {
  std::memset(raw + 1, 0xFF, 10);
  uint8_t m, s, f;
  lbaToMsf(lba, m, s, f);
  raw[12] = bcd(m); raw[13] = bcd(s); raw[14] = bcd(f); raw[15] = mode;
}

class ChdDisc : public Disc {
public:
  bool open(const std::string& path, std::string* err);
  bool readRaw(uint32_t lba, uint8_t* out) override;
  ChdInfo info;

private:
  bool fail(std::string* err, const std::string& m) { if (err) *err = m; return false; }
  bool readFile(uint64_t off, void* dst, size_t n);
  bool readMap(std::string* err);
  bool readTracks(uint64_t metaOff, std::string* err);
  const uint8_t* hunk(uint32_t h);
  bool decodeHunk(uint32_t h, uint8_t* dst, int depth);
  bool decompress(Codec c, const uint8_t* src, size_t len, uint8_t* dst);
  bool decompressCd(Codec c, const uint8_t* src, size_t len, uint8_t* dst);

  std::ifstream f_;
  uint64_t fileSize_ = 0;
  uint32_t hunkBytes_ = 0, hunkCount_ = 0, framesPerHunk_ = 0;
  uint64_t logical_ = 0, mapOff_ = 0;
  Codec codec_[4] = {};
  bool compressed_ = false;
  std::vector<uint8_t> map_;  // comprimido: 12 bytes por hunk (tipo, tamanho 24, posição 48, crc 16); senão 4
  std::vector<ChdTrack> ct_;
  struct Slot { int64_t hunk = -1; uint64_t age = 0; std::vector<uint8_t> data; };
  Slot cache_[4];
  uint64_t clock_ = 0;
  int64_t badHunk_ = -1;
  std::vector<uint8_t> comp_, sectors_, subs_;
};

bool ChdDisc::readFile(uint64_t off, void* dst, size_t n) {
  if (off > fileSize_ || n > fileSize_ - off) return false;
  f_.clear();
  f_.seekg(std::streamoff(off));
  f_.read(static_cast<char*>(dst), std::streamsize(n));
  return size_t(f_.gcount()) == n;
}

bool ChdDisc::open(const std::string& path, std::string* err) {
  f_.open(path, std::ios::binary);
  if (!f_) return fail(err, "não abriu o .chd");
  f_.seekg(0, std::ios::end);
  fileSize_ = uint64_t(f_.tellg());
  uint8_t h[124];
  if (fileSize_ < 16 || !readFile(0, h, 16) || std::memcmp(h, "MComprHD", 8) != 0)
    return fail(err, "o arquivo não é um .chd");
  const uint32_t version = be32(h + 12);
  if (version != 5) return fail(err, ".chd versão " + std::to_string(version) + " não suportado (só a 5; "
                                     "converta com chdman copy)");
  if (be32(h + 8) != 124 || !readFile(0, h, 124)) return fail(err, "cabeçalho do .chd inválido");
  for (int i = 0; i < 4; i++) {
    const uint32_t t = be32(h + 16 + 4 * i);
    codec_[i] = codecOf(t);
    info.codecs[i] = tagName(t);
    if (codec_[i] == C_UNKNOWN) return fail(err, "codec \"" + tagName(t) + "\" do .chd não suportado");
  }
  compressed_ = codec_[0] != C_NONE;
  logical_ = be64(h + 32);
  mapOff_ = be64(h + 40);
  const uint64_t metaOff = be64(h + 48);
  hunkBytes_ = be32(h + 56);
  for (int i = 0; i < 20; i++)
    if (h[104 + i]) return fail(err, "o .chd depende de um CHD pai (não suportado)");
  if (hunkBytes_ == 0 || hunkBytes_ % FRAME || hunkBytes_ > FRAME * 512)
    return fail(err, "o .chd não é de CD (tamanho de hunk " + std::to_string(hunkBytes_) + ")");
  framesPerHunk_ = hunkBytes_ / FRAME;
  const uint64_t hc = (logical_ + hunkBytes_ - 1) / hunkBytes_;
  if (logical_ == 0 || hc > (1u << 22)) return fail(err, "tamanho do .chd inválido");
  hunkCount_ = uint32_t(hc);
  info.hunkBytes = hunkBytes_;
  info.hunkCount = hunkCount_;
  if (!readMap(err) || !readTracks(metaOff, err)) return false;
  for (auto& s : cache_) s.data.resize(hunkBytes_);
  sectors_.resize(size_t(framesPerHunk_) * RAW_SECTOR);
  subs_.resize(size_t(framesPerHunk_) * SUB);
  return true;
}

bool ChdDisc::readMap(std::string* err) {
  if (!compressed_) {
    map_.resize(size_t(hunkCount_) * 4);
    if (!readFile(mapOff_, map_.data(), map_.size())) return fail(err, "mapa do .chd cortado");
    info.mapTypes[M_NONE] = hunkCount_;
    return true;
  }
  uint8_t mh[16];
  if (!readFile(mapOff_, mh, 16)) return fail(err, "mapa do .chd cortado");
  const uint32_t mapBytes = be32(mh);
  uint64_t cur = be48(mh + 4);
  const uint16_t mapCrc = uint16_t(be16(mh + 10));
  const int lengthBits = mh[12], selfBits = mh[13], parentBits = mh[14];
  if (lengthBits > 24 || selfBits > 32 || parentBits > 32) return fail(err, "mapa do .chd inválido");
  // o tamanho do mapa vem do cabeçalho: confere com o arquivo antes de reservar a memória
  if (mapOff_ + 16 > fileSize_ || mapBytes > fileSize_ - mapOff_ - 16) return fail(err, "mapa do .chd cortado");
  std::vector<uint8_t> cm(mapBytes);
  if (!readFile(mapOff_ + 16, cm.data(), cm.size())) return fail(err, "mapa do .chd cortado");
  chd::BitReader br(cm.data(), cm.size());
  chd::Huffman dec(16, 8);
  if (!dec.importRle(br)) return fail(err, "mapa do .chd inválido");
  map_.assign(size_t(hunkCount_) * 12, 0);
  uint8_t last = 0;
  uint32_t rep = 0;
  for (uint32_t i = 0; i < hunkCount_; i++) {
    uint8_t* m = &map_[size_t(i) * 12];
    if (rep > 0) { m[0] = last; rep--; continue; }
    const uint32_t v = dec.decode(br);
    if (v == M_RLE_SMALL) { m[0] = last; rep = 2 + dec.decode(br); }
    else if (v == M_RLE_LARGE) { m[0] = last; rep = 2 + 16 + (dec.decode(br) << 4); rep += dec.decode(br); }
    else m[0] = last = uint8_t(v);
    if (br.overflow()) return fail(err, "mapa do .chd cortado");
  }
  uint32_t lastSelf = 0;
  for (uint32_t i = 0; i < hunkCount_; i++) {
    uint8_t* m = &map_[size_t(i) * 12];
    uint64_t off = cur;
    uint32_t len = 0, crc = 0;
    switch (m[0]) {
      case 0: case 1: case 2: case 3:
        len = br.read(lengthBits);
        cur += len;
        crc = br.read(16);
        break;
      case M_NONE:
        len = hunkBytes_;
        cur += len;
        crc = br.read(16);
        break;
      case M_SELF: off = lastSelf = br.read(selfBits); break;
      case M_SELF_1: lastSelf++; [[fallthrough]];
      case M_SELF_0: m[0] = M_SELF; off = lastSelf; break;
      case M_PARENT: case M_PARENT_SELF: case M_PARENT_0: case M_PARENT_1:
        return fail(err, "o .chd depende de um CHD pai (não suportado)");
      default: return fail(err, "mapa do .chd inválido");
    }
    m[1] = uint8_t(len >> 16); m[2] = uint8_t(len >> 8); m[3] = uint8_t(len);
    for (int k = 0; k < 6; k++) m[4 + k] = uint8_t(off >> (40 - 8 * k));
    m[10] = uint8_t(crc >> 8); m[11] = uint8_t(crc);
    if (m[0] < 8) info.mapTypes[m[0]]++;
  }
  if (br.overflow() || chd::crc16(map_.data(), map_.size()) != mapCrc) return fail(err, "mapa do .chd corrompido");
  return true;
}

bool ChdDisc::readTracks(uint64_t metaOff, std::string* err) {
  struct T { int num; std::string type, pgtype; uint32_t frames, pregap, postgap; };
  std::vector<T> list;
  bool gdrom = false;
  uint64_t off = metaOff;
  for (int guard = 0; off != 0; guard++) {
    if (guard > 4096) return fail(err, "metadados do .chd em ciclo");
    uint8_t e[16];
    if (!readFile(off, e, 16)) return fail(err, "metadados do .chd cortados");
    const uint32_t t = be32(e), len = be24(e + 5);
    const uint64_t next = be64(e + 8);
    if (t == tag("CHGD")) gdrom = true;  // GD-ROM do Dreamcast: mesmo texto das faixas, área alta no LBA 45000
    if (t == tag("CHCD")) return fail(err, "metadados de CD antigos (CHCD) não suportados; converta com chdman");
    if (t == tag("CHT2") || t == tag("CHTR") || t == tag("CHGD")) {
      if (len > 1024) return fail(err, "metadados do .chd inválidos");
      std::string s(len, '\0');
      if (len && !readFile(off + 16, &s[0], len)) return fail(err, "metadados do .chd cortados");
      s = s.c_str();  // termina no \0
      T tr{0, "", "", 0, 0, 0};
      bool hasFrames = false;
      std::istringstream in(s);
      std::string tok;
      while (in >> tok) {
        const size_t c = tok.find(':');
        if (c == std::string::npos) continue;
        const std::string k = tok.substr(0, c), v = tok.substr(c + 1);
        auto num = [&](uint32_t& out) {
          if (v.empty() || v.size() > 9 || v.find_first_not_of("0123456789") != std::string::npos) return false;
          out = uint32_t(std::stoul(v));
          return true;
        };
        uint32_t n = 0;
        if (k == "TRACK") { if (!num(n)) return fail(err, "faixa inválida no .chd"); tr.num = int(n); }
        else if (k == "TYPE") tr.type = v;
        else if (k == "FRAMES") { if (!num(tr.frames)) return fail(err, "faixa inválida no .chd"); hasFrames = true; }
        else if (k == "PREGAP") { if (!num(tr.pregap)) return fail(err, "faixa inválida no .chd"); }
        else if (k == "POSTGAP") { if (!num(tr.postgap)) return fail(err, "faixa inválida no .chd"); }
        else if (k == "PGTYPE") tr.pgtype = v;
      }
      if (tr.num < 1 || tr.num > 99 || !hasFrames) return fail(err, "faixa inválida no .chd");
      list.push_back(tr);
    }
    off = next;
  }
  if (list.empty()) return fail(err, "o .chd não tem faixas de CD");
  std::sort(list.begin(), list.end(), [](const T& a, const T& b) { return a.num < b.num; });
  uint64_t lba = 0, chdFrame = 0;
  const uint64_t totalFrames = uint64_t(hunkCount_) * framesPerHunk_;
  for (size_t i = 0; i < list.size(); i++) {
    const T& l = list[i];
    if (i > 0 && l.num == list[i - 1].num) return fail(err, "faixa repetida no .chd");
    Track t;
    ChdTrack c;
    t.number = l.num;
    if (l.type == "AUDIO") { t.type = TrackType::Audio; c.audio = true; }
    else if (l.type == "MODE1_RAW") t.type = TrackType::Mode1;
    else if (l.type == "MODE1") { t.type = TrackType::Mode1; c.kind = Kind::Mode1Cooked; }
    else if (l.type == "MODE2_RAW") t.type = TrackType::Mode2;
    else if (l.type == "MODE2" || l.type == "MODE2_FORM_MIX") { t.type = TrackType::Mode2; c.kind = Kind::Mode2_2336; }
    else if (l.type == "MODE2_FORM1") { t.type = TrackType::Mode2; c.kind = Kind::Form1Cooked; }
    else if (l.type == "MODE2_FORM2") { t.type = TrackType::Mode2; c.kind = Kind::Form2_2324; }
    else return fail(err, "tipo de faixa \"" + l.type + "\" do .chd não suportado");
    const bool pgInData = !l.pgtype.empty() && l.pgtype[0] == 'V';
    if (pgInData && l.pregap > l.frames) return fail(err, "pregap maior que a faixa no .chd");
    if (!pgInData) lba += l.pregap;
    // GD-ROM: a faixa 3 (início da área de alta densidade) fica no LBA 45000
    if (gdrom && l.num == 3 && lba < 45000) lba = 45000 - (pgInData ? l.pregap : 0);
    c.dataLba = uint32_t(lba);
    c.frames = l.frames;
    c.chdFrame = uint32_t(chdFrame);
    if (chdFrame + l.frames > totalFrames) return fail(err, "faixa passa do fim do .chd");
    t.pregap = l.pregap;
    t.start = uint32_t(lba + (pgInData ? l.pregap : 0));
    t.length = l.frames - (pgInData ? l.pregap : 0) + l.postgap;
    lba += uint64_t(l.frames) + l.postgap;
    chdFrame += (uint64_t(l.frames) + PAD - 1) / PAD * PAD;
    if (lba > 10000000) return fail(err, "disco grande demais no .chd");
    tracks_.push_back(t);
    ct_.push_back(c);
  }
  total_ = uint32_t(lba);
  return true;
}

bool ChdDisc::decompressCd(Codec c, const uint8_t* src, size_t len, uint8_t* dst) {
  const uint32_t frames = framesPerHunk_;
  uint8_t* sec = sectors_.data();
  uint8_t* sub = subs_.data();
  const size_t secBytes = size_t(frames) * RAW_SECTOR, subBytes = size_t(frames) * SUB;
  const uint8_t* ecc = src;
  bool useEcc = false;
  if (c == C_CDFL) {
    size_t used = 0;
    if (!chd::flacFrames(src, len, sec, secBytes, true, &used) || used > len) return false;
    if (!chd::inflateRaw(src + used, len - used, sub, subBytes)) return false;
  } else {
    const size_t eccBytes = (frames + 7) / 8, lenBytes = hunkBytes_ < 65536 ? 2 : 3;
    const size_t hdr = eccBytes + lenBytes;
    if (len < hdr) return false;
    size_t baseLen = be16(src + eccBytes);
    if (lenBytes == 3) baseLen = baseLen << 8 | src[eccBytes + 2];
    if (baseLen > len - hdr) return false;
    const bool ok = c == C_CDLZ ? chd::lzmaRaw(src + hdr, baseLen, sec, secBytes)
                                : chd::inflateRaw(src + hdr, baseLen, sec, secBytes);
    if (!ok || !chd::inflateRaw(src + hdr + baseLen, len - hdr - baseLen, sub, subBytes)) return false;
    useEcc = true;
  }
  static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
  for (uint32_t f = 0; f < frames; f++) {
    uint8_t* d = dst + size_t(f) * FRAME;
    std::memcpy(d, sec + size_t(f) * RAW_SECTOR, RAW_SECTOR);
    std::memcpy(d + RAW_SECTOR, sub + size_t(f) * SUB, SUB);
    if (useEcc && (ecc[f / 8] >> (f % 8) & 1)) {  // o codec tirou o sincronismo e a correção: refaz
      std::memcpy(d, sync, 12);
      chd::eccGenerate(d);
      info.eccRestored++;
    }
  }
  return true;
}

bool ChdDisc::decompress(Codec c, const uint8_t* src, size_t len, uint8_t* dst) {
  switch (c) {
    case C_ZLIB: return chd::inflateRaw(src, len, dst, hunkBytes_);
    case C_LZMA: return chd::lzmaRaw(src, len, dst, hunkBytes_);
    case C_HUFF: return chd::huffman8(src, len, dst, hunkBytes_);
    case C_FLAC:
      if (len < 1 || (src[0] != 'L' && src[0] != 'B') || hunkBytes_ % 4) return false;
      return chd::flacFrames(src + 1, len - 1, dst, hunkBytes_, src[0] == 'B', nullptr);
    case C_CDZL: case C_CDLZ: case C_CDFL: return decompressCd(c, src, len, dst);
    default: return false;
  }
}

bool ChdDisc::decodeHunk(uint32_t h, uint8_t* dst, int depth) {
  if (h >= hunkCount_ || depth > 8) return false;
  info.hunksDecoded++;
  if (!compressed_) {
    const uint64_t off = uint64_t(be32(&map_[size_t(h) * 4])) * hunkBytes_;
    if (off == 0) { std::memset(dst, 0, hunkBytes_); return true; }
    return readFile(off, dst, hunkBytes_);
  }
  const uint8_t* m = &map_[size_t(h) * 12];
  const uint32_t len = be24(m + 1);
  const uint64_t off = be48(m + 4);
  const uint16_t crc = uint16_t(be16(m + 10));
  if (m[0] == M_SELF) {
    if (off >= h) return false;  // o chdman só aponta para hunks anteriores
    for (auto& s : cache_)
      if (s.hunk == int64_t(off)) { std::memcpy(dst, s.data.data(), hunkBytes_); return true; }
    return decodeHunk(uint32_t(off), dst, depth + 1);
  }
  if (m[0] == M_NONE) {
    if (!readFile(off, dst, hunkBytes_)) return false;
  } else {
    if (m[0] > M_TYPE3) return false;
    const Codec c = codec_[m[0]];
    if (c == C_NONE) return false;
    comp_.resize(len);
    if (!readFile(off, comp_.data(), len) || !decompress(c, comp_.data(), len, dst)) return false;
  }
  return chd::crc16(dst, hunkBytes_) == crc;
}

const uint8_t* ChdDisc::hunk(uint32_t h) {
  Slot* victim = &cache_[0];
  for (auto& s : cache_) {
    if (s.hunk == int64_t(h)) { s.age = ++clock_; return s.data.data(); }
    if (s.age < victim->age) victim = &s;
  }
  if (int64_t(h) == badHunk_) return nullptr;  // o último hunk com defeito não é decodificado de novo
  victim->hunk = -1;
  if (!decodeHunk(h, victim->data.data(), 0)) { badHunk_ = int64_t(h); return nullptr; }
  victim->hunk = int64_t(h);
  victim->age = ++clock_;
  return victim->data.data();
}

bool ChdDisc::readRaw(uint32_t lba, uint8_t* out) {
  std::memset(out, 0, RAW_SECTOR);
  const int ti = trackAt(lba);
  if (ti < 0) return false;
  const ChdTrack& c = ct_[size_t(ti)];
  if (lba < c.dataLba || lba - c.dataLba >= c.frames) return true;  // pregap/postgap fora da imagem: zeros
  const uint64_t frame = uint64_t(c.chdFrame) + (lba - c.dataLba);
  const uint8_t* hp = hunk(uint32_t(frame / framesPerHunk_));
  if (!hp) return false;
  const uint8_t* s = hp + size_t(frame % framesPerHunk_) * FRAME;
  switch (c.kind) {
    case Kind::Raw:
      if (c.audio) {
        for (int i = 0; i < RAW_SECTOR; i += 2) { out[i] = s[i + 1]; out[i + 1] = s[i]; }
      } else {
        std::memcpy(out, s, RAW_SECTOR);
      }
      break;
    case Kind::Mode1Cooked: header(lba, 1, out); std::memcpy(out + 16, s, DATA_SECTOR); break;
    case Kind::Mode2_2336: header(lba, 2, out); std::memcpy(out + 16, s, 2336); break;
    case Kind::Form1Cooked: header(lba, 2, out); std::memcpy(out + 24, s, DATA_SECTOR); break;
    case Kind::Form2_2324: header(lba, 2, out); std::memcpy(out + 24, s, 2324); break;
  }
  return true;
}

}  // namespace

std::unique_ptr<Disc> openChd(const std::string& path, std::string* err) {
  auto d = std::make_unique<ChdDisc>();
  if (!d->open(path, err)) return nullptr;
  return d;
}

bool chdInfo(const Disc& d, ChdInfo& out) {
  const auto* c = dynamic_cast<const ChdDisc*>(&d);
  if (!c) return false;
  out = c->info;
  return true;
}

}  // namespace indigo::cdrom
