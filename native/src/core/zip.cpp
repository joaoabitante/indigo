#include "zip.h"

#include <algorithm>
#include <cctype>

#include "safe.h"

namespace indigo::zip {

namespace {
// ------------------------------------------------------------ DEFLATE (RFC 1951)
struct Bits {
  const uint8_t* p;
  size_t n, pos = 0;
  uint32_t buf = 0;
  int cnt = 0;
  bool over = false;
  uint32_t get(int k) {
    while (cnt < k) {
      if (pos >= n) { over = true; return 0; }
      buf |= uint32_t(p[pos++]) << cnt;
      cnt += 8;
    }
    const uint32_t v = buf & ((1u << k) - 1);
    buf >>= k;
    cnt -= k;
    return v;
  }
};

// tabela de Huffman canônica: contagem por comprimento + símbolos em ordem
struct Huff {
  uint16_t count[16] = {};
  uint16_t sym[320] = {};
  bool build(const uint8_t* lens, int n) {
    std::fill(count, count + 16, 0);
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= count[l]; if (left < 0) return false; }
    uint16_t offs[16] = {};
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + count[l];
    for (int i = 0; i < n; i++) if (lens[i]) sym[offs[lens[i]]++] = static_cast<uint16_t>(i);
    return true;
  }
  int decode(Bits& b) const {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
      code |= static_cast<int>(b.get(1));
      if (b.over) return -1;
      const int c = count[l];
      if (code - c < first) return sym[index + (code - first)];
      index += c; first += c; first <<= 1; code <<= 1;
    }
    return -1;
  }
};

const uint16_t LBASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t LEXT[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
// as duas últimas distâncias (30 e 31) só existem no Deflate64 (janela de 64 KB)
const uint16_t DBASE[32] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 32769, 49153};
const uint8_t DEXT[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14};

constexpr size_t MAX_OUT = 256u * 1024 * 1024;  // nenhuma ROM portátil passa de 64 MB; protege contra "zip bomb"
constexpr uint64_t MAX_TOTAL = 1024ull * 1024 * 1024;  // soma de todos os arquivos de um pacote

// d64: Deflate64 (método 9 do .zip, o que o Windows usa em arquivos grandes): o comprimento 285 tem 16 bits a
// mais e há as distâncias 30 e 31
bool block(Bits& b, const Huff& lit, const Huff& dist, std::vector<uint8_t>& out, bool d64) {
  for (;;) {
    const int s = lit.decode(b);
    if (s < 0) return false;
    if (s < 256) { out.push_back(static_cast<uint8_t>(s)); if (out.size() > MAX_OUT) return false; continue; }
    if (s == 256) return true;
    const int li = s - 257;
    if (li >= 29) return false;
    const size_t len = (d64 && li == 28) ? 3 + b.get(16) : LBASE[li] + b.get(LEXT[li]);
    const int ds = dist.decode(b);
    if (ds < 0 || ds >= (d64 ? 32 : 30)) return false;
    const size_t d = DBASE[ds] + b.get(DEXT[ds]);
    if (b.over || d > out.size() || out.size() + len > MAX_OUT) return false;
    const size_t from = out.size() - d;
    for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
  }
}
}  // namespace

namespace {
bool inflateAny(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected, bool d64) {
  out.clear();
  if (expected && expected <= MAX_OUT) out.reserve(expected);
  Bits b{src, n};
  int last = 0;
  do {
    last = static_cast<int>(b.get(1));
    const uint32_t type = b.get(2);
    if (b.over) return false;
    if (type == 0) {  // sem compressão
      b.buf = 0; b.cnt = 0;  // alinha no byte
      if (b.pos + 4 > n) return false;
      const uint32_t len = src[b.pos] | (src[b.pos + 1] << 8), nlen = src[b.pos + 2] | (src[b.pos + 3] << 8);
      b.pos += 4;
      if ((len ^ 0xFFFF) != nlen || b.pos + len > n || out.size() + len > MAX_OUT) return false;
      out.insert(out.end(), src + b.pos, src + b.pos + len);
      b.pos += len;
    } else if (type == 1) {  // Huffman fixo
      static Huff flit, fdist;
      static bool ready = false;
      if (!ready) {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        flit.build(l, 288);
        uint8_t d[32];
        std::fill(d, d + 32, 5);
        fdist.build(d, 32);
        ready = true;
      }
      if (!block(b, flit, fdist, out, d64)) return false;
    } else if (type == 2) {  // Huffman dinâmico
      const int hlit = static_cast<int>(b.get(5)) + 257, hdist = static_cast<int>(b.get(5)) + 1, hclen = static_cast<int>(b.get(4)) + 4;
      if (b.over || hlit > (d64 ? 288 : 286) || hdist > (d64 ? 32 : 30)) return false;
      static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      uint8_t cl[19] = {};
      for (int i = 0; i < hclen; i++) cl[ORDER[i]] = static_cast<uint8_t>(b.get(3));
      Huff clh;
      if (!clh.build(cl, 19)) return false;
      uint8_t lens[320] = {};
      int i = 0;
      while (i < hlit + hdist) {
        const int s = clh.decode(b);
        if (s < 0) return false;
        if (s < 16) { lens[i++] = static_cast<uint8_t>(s); continue; }
        int rep = 0;
        uint8_t val = 0;
        if (s == 16) { if (i == 0) return false; val = lens[i - 1]; rep = 3 + static_cast<int>(b.get(2)); }
        else if (s == 17) rep = 3 + static_cast<int>(b.get(3));
        else rep = 11 + static_cast<int>(b.get(7));
        if (b.over || i + rep > hlit + hdist) return false;
        while (rep--) lens[i++] = val;
      }
      Huff lit, dist;
      if (!lens[256] || !lit.build(lens, hlit) || !dist.build(lens + hlit, hdist)) return false;
      if (!block(b, lit, dist, out, d64)) return false;
    } else {
      return false;
    }
  } while (!last);
  return true;
}
}  // namespace

bool inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected) {
  return inflateAny(src, n, out, expected, false);
}

bool isZip(const std::vector<uint8_t>& b) {
  return b.size() > 22 && b[0] == 0x50 && b[1] == 0x4B && b[2] == 0x03 && b[3] == 0x04;
}

namespace {
uint32_t u16(const std::vector<uint8_t>& b, size_t o) { return o + 2 <= b.size() ? b[o] | (b[o + 1] << 8) : 0; }
uint32_t u32(const std::vector<uint8_t>& b, size_t o) {
  return o + 4 <= b.size() ? b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24) : 0;
}
uint64_t u64(const std::vector<uint8_t>& b, size_t o) {
  return o + 8 <= b.size() ? uint64_t(u32(b, o)) | uint64_t(u32(b, o + 4)) << 32 : 0;
}
bool romExt(std::string n) {
  for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const char* EXT[] = {".gba", ".agb", ".gbc", ".gb", ".sgb", ".cgb", ".gg", ".sms", ".sg", ".ngp", ".ngc", ".npc", ".nes", ".sfc", ".smc", ".min", ".a26", ".a52", ".mx1", ".mx2", ".rom", ".col", ".cv", ".sc", ".ws", ".wsc", ".pc2", ".a78", ".vec", ".gam", ".int", ".itv", ".lnx", ".lyx", ".vb", ".duck", ".md0", ".md1", ".md2", ".sv", ".pce", ".sgx", ".md", ".gen", ".smd", ".fds", ".qd", ".j64", ".jag", ".abs", ".z64", ".n64", ".v64",
                              ".tgc", ".nds", ".neo", ".img", ".bin"};  // por último: ROM de partida do Lynx (lynxboot.img), BIOS e cartuchos .bin
  for (const char* e : EXT) {
    const size_t l = std::char_traits<char>::length(e);
    if (n.size() >= l && n.compare(n.size() - l, l, e) == 0) return true;
  }
  return false;
}
}  // namespace

namespace {
struct RawEntry { std::string name; uint32_t method, csize, size, local, crc; };

// diretório central do .zip; false se o arquivo estiver danificado
bool readDirectory(const std::vector<uint8_t>& bytes, std::vector<RawEntry>& entries) {
  long eocd = -1;
  const long sz = static_cast<long>(bytes.size());
  for (long i = sz - 22; i >= std::max(0L, sz - 65557); i--)
    if (u32(bytes, static_cast<size_t>(i)) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) return false;
  uint64_t count = u16(bytes, eocd + 10);
  uint64_t p = u32(bytes, eocd + 16);
  // ZIP64 (alguns programas gravam assim até em pacotes pequenos): o localizador fica logo antes do fim do
  // diretório e aponta o registro de 64 bits com o número de arquivos e o começo do diretório
  if ((count == 0xFFFF || p == 0xFFFFFFFFu) && eocd >= 20 && u32(bytes, static_cast<size_t>(eocd - 20)) == 0x07064b50) {
    const uint64_t z = u64(bytes, static_cast<size_t>(eocd - 20 + 8));
    if (z <= bytes.size() && bytes.size() - z >= 56 && u32(bytes, static_cast<size_t>(z)) == 0x06064b50) {
      count = u64(bytes, static_cast<size_t>(z + 32));
      p = u64(bytes, static_cast<size_t>(z + 48));
    }
  }
  for (uint64_t n = 0; n < count && p <= bytes.size() && bytes.size() - p >= 46; n++) {
    if (u32(bytes, p) != 0x02014b50) break;
    const uint32_t nlen = u16(bytes, p + 28), xlen = u16(bytes, p + 30), clen = u16(bytes, p + 32);
    if (p + 46 + nlen + xlen > bytes.size()) break;
    RawEntry e{std::string(bytes.begin() + static_cast<long>(p + 46), bytes.begin() + static_cast<long>(p + 46 + nlen)),
               u16(bytes, p + 10), u32(bytes, p + 20), u32(bytes, p + 24), u32(bytes, p + 42), u32(bytes, p + 16)};
    // campo extra de ZIP64 (0x0001): traz, nesta ordem, só os tamanhos e a posição que estão em 0xFFFFFFFF
    for (uint64_t x = p + 46 + nlen; x + 4 <= p + 46 + nlen + xlen;) {
      const uint32_t id = u16(bytes, x), len = u16(bytes, x + 2);
      if (id == 0x0001) {
        uint64_t q = x + 4;
        const uint64_t fim = x + 4 + len;
        auto pega = [&](uint32_t& campo) {
          if (campo != 0xFFFFFFFFu || q + 8 > fim) return;
          const uint64_t v = u64(bytes, q);
          q += 8;
          campo = v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(v);  // acima de 4 GB não serve mesmo
        };
        pega(e.size); pega(e.csize); pega(e.local);
      }
      x += 4 + len;
    }
    entries.push_back(std::move(e));
    p += 46 + nlen + xlen + clen;
  }
  // limite do pacote inteiro (além dos 256 MB por arquivo): 1 GB descompactado
  uint64_t total = 0;
  for (const auto& e : entries) total += e.size;
  return total <= MAX_TOTAL;
}

// sem as pastas (o .zip pode vir com barra invertida do Windows: "..\\..\\x.gb")
std::string baseName(const std::string& n) {
  const auto slash = n.find_last_of("/\\");
  return slash == std::string::npos ? n : n.substr(slash + 1);
}

// 0 = certo, 1 = danificado, 2 = compressão sem suporte
int readEntry(const std::vector<uint8_t>& bytes, const RawEntry& e, std::vector<uint8_t>& data) {
  const size_t lh = e.local;
  if (lh + 30 > bytes.size()) return 1;
  const size_t start = lh + 30 + u16(bytes, lh + 26) + u16(bytes, lh + 28);
  if (start > bytes.size() || e.csize > bytes.size() - start) return 1;
  const uint8_t* raw = bytes.data() + start;
  // o tamanho descompactado tem de bater com o do diretório: é ele que conta para o limite do pacote
  if (e.method == 0) { if (e.csize != e.size) return 1; data.assign(raw, raw + e.csize); return 0; }
  if (e.method == 8) return inflate(raw, e.csize, data, e.size) && data.size() == e.size ? 0 : 1;
  if (e.method == 9) return inflateAny(raw, e.csize, data, e.size, true) && data.size() == e.size ? 0 : 1;
  return 2;
}
}  // namespace

bool listEntries(const std::vector<uint8_t>& bytes, std::vector<Entry>& out) {
  std::vector<RawEntry> entries;
  if (!readDirectory(bytes, entries)) return false;
  out.clear();
  for (const auto& e : entries)
    if (!e.name.empty() && e.name.back() != '/') out.push_back({baseName(e.name), e.size, e.crc});
  return true;
}

bool extractEntry(const std::vector<uint8_t>& bytes, const std::string& name, std::vector<uint8_t>& data) {
  std::vector<RawEntry> entries;
  if (!readDirectory(bytes, entries)) return false;
  auto low = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
  const std::string want = low(name);
  for (const auto& e : entries)
    if (low(baseName(e.name)) == want) return readEntry(bytes, e, data) == 0;
  return false;
}

bool extractRom(const std::vector<uint8_t>& bytes, std::string& name, std::vector<uint8_t>& data, std::string& error) {
  std::vector<RawEntry> entries;
  if (!readDirectory(bytes, entries)) { error = "Arquivo .zip danificado."; return false; }
  const RawEntry* pick = nullptr;
  // extensões que também são de outras coisas (.md é Mega Drive e também texto Markdown; .bin/.rom/.img servem a
  // muitos consoles) só valem se não houver no .zip um arquivo de extensão inconfundível; e LEIA-ME/README/LICENSE
  // nunca são o jogo. Sem isso, um "README.md" maior que a ROM era aberto como jogo de Mega Drive.
  auto low = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
  auto duvidosa = [&](const std::string& n) {
    const std::string l = low(n);
    for (const char* x : {".md", ".bin", ".rom", ".img"}) {
      const size_t k = std::char_traits<char>::length(x);
      if (l.size() >= k && l.compare(l.size() - k, k, x) == 0) return true;
    }
    return false;
  };
  auto documento = [&](const std::string& n) {
    const std::string l = low(baseName(n));
    for (const char* x : {"readme", "leia-me", "leiame", "license", "licence", "licenca", "changelog", "copying", "notes"})
      if (l.rfind(x, 0) == 0) return true;
    return false;
  };
  for (const bool certas : {true, false}) {
    for (const auto& e : entries)
      if (romExt(e.name) && !documento(e.name) && (!certas || !duvidosa(e.name)) && (!pick || e.size > pick->size)) pick = &e;
    if (pick) break;
  }
  if (!pick) { error = "Não há ROM de console portátil dentro deste .zip."; return false; }
  const int r = readEntry(bytes, *pick, data);
  if (r == 1) { error = "Arquivo .zip danificado."; return false; }
  if (r == 2) { error = "Compressão do .zip não suportada."; return false; }
  name = safeFileName(baseName(pick->name));  // vira o nome do save: nada de pastas nem ".."
  return true;
}


int entryMethod(const std::vector<uint8_t>& bytes, const std::string& name) {
  std::vector<RawEntry> entries;
  if (!readDirectory(bytes, entries)) return -1;
  auto low = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
  const std::string want = low(name);
  for (const auto& e : entries)
    if (low(baseName(e.name)) == want) return static_cast<int>(e.method);
  return -1;
}

const char* methodName(int m) {
  switch (m) {
    case 0: return "sem compressão";
    case 8: return "Deflate";
    case 9: return "Deflate64";
    case 12: return "BZIP2";
    case 14: return "LZMA";
    case 93: return "Zstandard";
    case 95: return "XZ";
    case 98: return "PPMd";
    case 99: return "criptografia AES";
    default: return "desconhecida";
  }
}

std::vector<uint8_t> merge(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second) {
  std::vector<uint8_t> out, cd;
  std::vector<std::string> seen;
  auto low = [](std::string t) { for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return t; };
  auto put16 = [](std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v)); o.push_back(uint8_t(v >> 8)); };
  auto put32 = [&](std::vector<uint8_t>& o, uint32_t v) { put16(o, v & 0xFFFF); put16(o, v >> 16); };
  uint32_t n = 0;
  for (const auto* src : {&first, &second}) {
    std::vector<RawEntry> entries;
    if (!readDirectory(*src, entries)) continue;
    for (const auto& e : entries) {
      if (e.name.empty() || e.name.back() == '/') continue;
      const std::string key = low(baseName(e.name));
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;  // o primeiro pacote tem a preferência
      // o nome fica reservado mesmo se a entrada estiver estragada: um arquivo danificado da versão não pode ser
      // trocado em silêncio pelo do jogo principal (o jogo abriria com ROMs misturadas)
      seen.push_back(key);
      if (n >= 0xFFFE) continue;  // limite do .zip comum
      const size_t lh = e.local;
      if (lh + 30 > src->size() || u32(*src, lh) != 0x04034b50) continue;
      const size_t start = lh + 30 + u16(*src, lh + 26) + u16(*src, lh + 28);
      if (start > src->size() || e.csize > src->size() - start) continue;
      if (out.size() + (start - lh) + e.csize > 0xF0000000u) continue;  // o pacote junto fica no .zip comum (até 4 GB)
      const std::string nome = baseName(e.name);
      // cabeçalho local novo (sem campos extras); os dados comprimidos vão como estão
      const uint32_t off = static_cast<uint32_t>(out.size());
      put32(out, 0x04034b50); put16(out, 20); put16(out, 0); put16(out, e.method); put16(out, 0); put16(out, 0);
      put32(out, e.crc); put32(out, e.csize); put32(out, e.size); put16(out, static_cast<uint32_t>(nome.size())); put16(out, 0);
      out.insert(out.end(), nome.begin(), nome.end());
      out.insert(out.end(), src->begin() + static_cast<long>(start), src->begin() + static_cast<long>(start + e.csize));
      put32(cd, 0x02014b50); put16(cd, 20); put16(cd, 20); put16(cd, 0); put16(cd, e.method); put16(cd, 0); put16(cd, 0);
      put32(cd, e.crc); put32(cd, e.csize); put32(cd, e.size); put16(cd, static_cast<uint32_t>(nome.size()));
      put16(cd, 0); put16(cd, 0); put16(cd, 0); put16(cd, 0); put32(cd, 0); put32(cd, off);
      cd.insert(cd.end(), nome.begin(), nome.end());
      n++;
    }
  }
  const uint32_t cdOff = static_cast<uint32_t>(out.size());
  out.insert(out.end(), cd.begin(), cd.end());
  put32(out, 0x06054b50); put16(out, 0); put16(out, 0); put16(out, n); put16(out, n);
  put32(out, static_cast<uint32_t>(cd.size())); put32(out, cdOff); put16(out, 0);
  return out;
}

}  // namespace indigo::zip
