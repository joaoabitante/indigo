#include "games.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "../core/zip.h"

namespace indigo::cps {

namespace {
#include "tabela.inc"

constexpr int kGameCount = static_cast<int>(sizeof(kGames) / sizeof(kGames[0]));

std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}

// nome do pacote sem pastas e sem ".zip"
std::string setName(const std::string& nomeMinusculo) {
  std::string n = lower(nomeMinusculo);
  const size_t slash = n.find_last_of("/\\");
  if (slash != std::string::npos) n = n.substr(slash + 1);
  if (n.size() > 4 && n.compare(n.size() - 4, 4, ".zip") == 0) n.resize(n.size() - 4);
  return n;
}

struct ZipIndex {
  std::vector<zip::Entry> entries;
  std::unordered_multimap<uint32_t, size_t> byCrc;
  std::unordered_map<std::string, size_t> byName;
  bool ok = false;

  explicit ZipIndex(const std::vector<uint8_t>& z) {
    if (!zip::isZip(z) || !zip::listEntries(z, entries)) return;
    for (size_t i = 0; i < entries.size(); i++) {
      byCrc.emplace(entries[i].crc, i);
      byName.emplace(lower(entries[i].name), i);
    }
    ok = true;
  }

  // arquivo da tabela presente no .zip: pelo CRC e tamanho; na falta, pelo nome e tamanho
  const zip::Entry* find(const RomFile& f) const {
    auto range = byCrc.equal_range(f.crc);
    for (auto it = range.first; it != range.second; ++it)
      if (entries[it->second].size == f.size) return &entries[it->second];
    auto n = byName.find(lower(f.name));
    if (n != byName.end() && entries[n->second].size == f.size) return &entries[n->second];
    return nullptr;
  }
};

// arquivos (sem repetição) de um jogo
void gameFiles(const GameDef& g, std::vector<uint16_t>& out) {
  out.clear();
  for (int i = 0; i < g.numLoads; i++) {
    const RomLoad& l = kLoads[g.firstLoad + i];
    if (l.mode >= 7) continue;
    if (std::find(out.begin(), out.end(), l.file) == out.end()) out.push_back(l.file);
  }
}
}  // namespace

const CpsbConfig& cpsbConfig(int i) {
  const int n = static_cast<int>(sizeof(kCpsb) / sizeof(kCpsb[0]));
  return kCpsb[i < 0 || i >= n ? 0 : i];
}
const GfxMapper& gfxMapper(int i) {
  const int n = static_cast<int>(sizeof(kMappers) / sizeof(kMappers[0]));
  return kMappers[i < 0 || i >= n ? 0 : i];
}
const GfxRange& gfxRange(int i) {
  const int n = static_cast<int>(sizeof(kRanges) / sizeof(kRanges[0]));
  return kRanges[i < 0 || i >= n ? 0 : i];
}
int gameCount() { return kGameCount; }
const GameDef& gameAt(int i) { return kGames[i < 0 || i >= kGameCount ? 0 : i]; }

const GameDef* gameByName(const std::string& name) {
  for (const GameDef& g : kGames)
    if (name == g.name) return &g;
  return nullptr;
}
const RomLoad& loadAt(const GameDef& g, int i) { return kLoads[g.firstLoad + (i < 0 || i >= g.numLoads ? 0 : i)]; }
const RomFile& fileAt(int i) {
  const int n = static_cast<int>(sizeof(kFiles) / sizeof(kFiles[0]));
  return kFiles[i < 0 || i >= n ? 0 : i];
}

const GameDef* identify(const std::vector<uint8_t>& zipData, const std::string& nomeMinusculo, std::string* faltando) {
  const ZipIndex zi(zipData);
  if (!zi.ok) return nullptr;
  const std::string base = setName(nomeMinusculo);
  std::vector<uint16_t> files;
  const GameDef* best = nullptr;
  int bestScore = -1;
  const GameDef* named = nullptr;
  std::string namedMissing;
  for (int gi = 0; gi < kGameCount; gi++) {
    const GameDef& g = kGames[gi];
    gameFiles(g, files);
    int have = 0;
    std::string missing;
    for (uint16_t f : files) {
      if (zi.find(kFiles[f])) have++;
      else missing += std::string(missing.empty() ? "" : ", ") + kFiles[f].name;
    }
    const bool complete = have == static_cast<int>(files.size()) && !files.empty();
    if (base == g.name) { named = &g; namedMissing = missing; }
    if (!complete) continue;
    // prioridade: o jogo com o nome do pacote; depois um clone do pacote (pacote "merged"); depois o que tiver
    // mais arquivos (o mais específico)
    int score = have;
    if (base == g.name) score += 1000000;
    else if (base == g.parent) score += 100000;
    if (score > bestScore) { bestScore = score; best = &g; }
  }
  if (!best && named && faltando) *faltando = namedMissing;
  return best;
}

std::string parentSetFor(const std::vector<uint8_t>& zipData, const std::string& nomeMinusculo) {
  const ZipIndex zi(zipData);
  if (!zi.ok) return "";
  const GameDef* g = gameByName(setName(nomeMinusculo));
  if (!g || !g->parent || !*g->parent) return "";
  std::vector<uint16_t> files;
  gameFiles(*g, files);
  for (uint16_t f : files)
    if (!zi.find(kFiles[f])) return g->parent;  // falta algo: no pacote "split" o resto está no do jogo principal
  return "";
}

void loadRegions(const GameDef& g, const std::vector<uint8_t>& zipData, std::vector<uint8_t> (&regions)[RG_COUNT]) {
  const ZipIndex zi(zipData);
  if (!zi.ok) throw std::runtime_error("Arquivo .zip danificado.");
  for (int r = 0; r < RG_COUNT; r++) regions[r].assign(g.regionSize[r], 0);
  // a região do programa do CPS1 vem com 4 MB no MAME; aqui fica só do tamanho usado (potência de 2)
  if (g.kind == 1) {
    uint32_t top = 0;
    for (int i = 0; i < g.numLoads; i++) {
      const RomLoad& l = kLoads[g.firstLoad + i];
      if (l.region != RG_MAIN) continue;
      const uint32_t span = l.mode == 1 ? l.len * 2 : l.mode == 5 ? l.len * 4 : l.mode == 3 ? l.len * 4 : l.mode == 4 ? l.len * 8 : l.len;
      top = std::max(top, l.dst + span);
    }
    uint32_t sz = 0x10000;
    while (sz < top && sz < 0x400000) sz <<= 1;
    regions[RG_MAIN].assign(sz, 0);
  }
  int curFile = -1;
  std::vector<uint8_t> data;
  for (int i = 0; i < g.numLoads; i++) {
    const RomLoad& l = kLoads[g.firstLoad + i];
    if (l.region >= RG_COUNT) continue;
    std::vector<uint8_t>& dst = regions[l.region];
    auto put = [&](size_t at, uint8_t v) { if (at < dst.size()) dst[at] = v; };
    if (l.mode == 7) {
      for (uint32_t k = 0; k < l.len; k++) put(static_cast<size_t>(l.dst) + k, static_cast<uint8_t>(l.file));
      continue;
    }
    if (l.mode == 8) {
      if (l.file >= RG_COUNT) continue;
      const std::vector<uint8_t>& s = regions[l.file];
      for (uint32_t k = 0; k < l.len; k++) {
        const size_t from = static_cast<size_t>(l.src) + k;
        put(static_cast<size_t>(l.dst) + k, from < s.size() ? s[from] : 0);
      }
      continue;
    }
    if (curFile != l.file) {
      const RomFile& rf = kFiles[l.file];
      const zip::Entry* e = zi.find(rf);
      if (!e) throw std::runtime_error(std::string("Falta o arquivo ") + rf.name + " no pacote.");
      data.clear();
      if (!zip::extractEntry(zipData, e->name, data) || data.size() != rf.size) {
        const int m = zip::entryMethod(zipData, e->name);
        if (m != 0 && m != 8 && m != 9)
          throw std::runtime_error(std::string("O arquivo ") + rf.name + " do pacote está comprimido em " + zip::methodName(m) +
                                   ", que o Índigo não lê. Refaça o .zip no formato comum (Deflate), por exemplo pelo "
                                   "\"Enviar para > Pasta compactada\" do Windows.");
        throw std::runtime_error(std::string("Não consegui ler ") + rf.name + " do pacote (o .zip parece danificado).");
      }
      curFile = l.file;
    }
    auto get = [&](size_t at) -> uint8_t { return at < data.size() ? data[at] : 0; };
    const size_t s = l.src, d = l.dst;
    switch (l.mode) {
      case 0: case 6: for (uint32_t k = 0; k < l.len; k++) put(d + k, get(s + k)); break;
      case 1: for (uint32_t k = 0; k < l.len; k++) put(d + 2 * k, get(s + k)); break;
      case 2: for (uint32_t k = 0; k + 1 < l.len; k += 2) { put(d + k, get(s + k + 1)); put(d + k + 1, get(s + k)); } break;
      case 3: for (uint32_t k = 0; k + 1 < l.len; k += 2) { put(d + k * 4, get(s + k)); put(d + k * 4 + 1, get(s + k + 1)); } break;
      case 4: for (uint32_t k = 0; k < l.len; k++) put(d + 8 * static_cast<size_t>(k), get(s + k)); break;
      case 5: for (uint32_t k = 0; k < l.len; k++) put(d + 4 * static_cast<size_t>(k), get(s + k)); break;
      default: break;
    }
  }
}

}  // namespace indigo::cps
