// Leitor de .chd: compara setor a setor cada imagem .chd (gerada pelo chdman a partir de um .cue, com vários
// codecs e tamanhos de hunk; ver t7_chd.sh) com o .cue de origem: faixas, LBAs, pregap, tamanho, readRaw e
// readData de todos os setores (e um pouco além do fim). Confere também que cada codec foi de fato usado
// (cdlz, cdzl, cdfl, zlib, lzma, huff, flac, sem compressão, cópia de outro hunk, ECC refeita) e que arquivos
// corrompidos ou cortados são recusados sem travar (rodar também com -fsanitize=address,undefined).
// Uso: indigo-t7_chd <pasta com lista.txt> <pasta-temporária>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "cdrom/cdrom.h"
#include "cdrom/chd.h"

using namespace indigo::cdrom;

namespace {
int checks = 0, fails = 0;
uint64_t sectors = 0;
bool check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("  FALHA: %s\n", what.c_str()); }
  return ok;
}

std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void writeAll(const std::string& p, const std::vector<uint8_t>& d, size_t n) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(n));
}

bool sameTracks(Disc& a, Disc& b) {
  if (a.tracks().size() != b.tracks().size() || a.totalSectors() != b.totalSectors()) return false;
  for (size_t i = 0; i < a.tracks().size(); i++) {
    const Track &x = a.tracks()[i], &y = b.tracks()[i];
    if (x.number != y.number || x.type != y.type || x.start != y.start || x.pregap != y.pregap ||
        x.length != y.length)
      return false;
  }
  return true;
}

std::string describe(Disc& d) {
  std::ostringstream o;
  for (const Track& t : d.tracks())
    o << " [" << t.number << " " << int(t.type) << " lba " << t.start << " pg " << t.pregap << " n " << t.length << "]";
  o << " total " << d.totalSectors();
  return o.str();
}

// conteúdo esperado de cada setor (do .cue), para comparar também os .chd corrompidos
struct Ref {
  std::vector<uint8_t> raw, data;
  std::vector<char> okRaw, okData;
};
Ref snapshot(Disc& d) {
  Ref r;
  const uint32_t n = d.totalSectors() + 3;
  r.raw.resize(size_t(n) * RAW_SECTOR);
  r.data.resize(size_t(n) * DATA_SECTOR);
  r.okRaw.resize(n);
  r.okData.resize(n);
  for (uint32_t l = 0; l < n; l++) {
    r.okRaw[l] = d.readRaw(l, &r.raw[size_t(l) * RAW_SECTOR]);
    r.okData[l] = d.readData(l, &r.data[size_t(l) * DATA_SECTOR]);
  }
  return r;
}

std::map<std::string, uint64_t> codecHunks;  // hunks no mapa por codec, somando todas as imagens
uint64_t eccRestored = 0, selfHunks = 0, noneHunks = 0, bigHunkCd = 0;

void compareImage(const std::string& dir, const std::string& chdName, const std::string& cueName) {
  std::string e1, e2;
  auto cue = open(dir + cueName, &e1);
  auto chd = open(dir + chdName, &e2);
  if (!check(cue != nullptr, cueName + ": " + e1) || !check(chd != nullptr, chdName + ": " + e2)) return;
  if (!check(sameTracks(*cue, *chd), chdName + ": faixas diferentes:\n   cue" + describe(*cue) + "\n   chd" +
                                         describe(*chd)))
    return;
  uint8_t a[RAW_SECTOR], b[RAW_SECTOR], da[DATA_SECTOR], db[DATA_SECTOR];
  int badRaw = 0, badData = 0;
  const uint32_t n = cue->totalSectors() + 3;
  // ordem embaralhada no último quarto (o cache de hunks tem de dar certo fora de ordem também)
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; i++) order[i] = i;
  std::mt19937 rng(n);
  for (uint32_t i = n - n / 4; i + 1 < n; i++) std::swap(order[i], order[i + rng() % (n - i)]);
  for (uint32_t l : order) {
    const bool ra = cue->readRaw(l, a), rb = chd->readRaw(l, b);
    if (ra != rb || std::memcmp(a, b, RAW_SECTOR) != 0) {
      if (badRaw++ < 3) std::printf("  %s: readRaw difere no LBA %u (%d/%d)\n", chdName.c_str(), l, ra, rb);
    }
    const bool qa = cue->readData(l, da), qb = chd->readData(l, db);
    if (qa != qb || std::memcmp(da, db, DATA_SECTOR) != 0) {
      if (badData++ < 3) std::printf("  %s: readData difere no LBA %u\n", chdName.c_str(), l);
    }
    sectors++;
  }
  check(badRaw == 0, chdName + ": readRaw igual em todos os setores");
  check(badData == 0, chdName + ": readData igual em todos os setores");
  check(detectSystem(*cue) == detectSystem(*chd), chdName + ": mesma detecção do console");
  ChdInfo info;
  if (check(chdInfo(*chd, info), chdName + ": informações do .chd")) {
    for (int i = 0; i < 4; i++)
      if (!info.codecs[i].empty()) codecHunks[info.codecs[i]] += info.mapTypes[i];
    eccRestored += info.eccRestored;
    selfHunks += info.mapTypes[5];
    noneHunks += info.mapTypes[4];
    if (info.hunkBytes >= 65536)
      for (int i = 0; i < 4; i++)
        if (info.codecs[i].rfind("cd", 0) == 0) bigHunkCd += info.mapTypes[i];
  }
}

// .chd corrompido: nunca pode travar; se abrir com as mesmas faixas, todo setor lido com sucesso tem de ser o
// certo (o CRC16 de cada hunk pega a corrupção), a não ser no .chd sem compressão, que não tem CRC
int corruptOpened = 0, corruptRefused = 0, corruptReadFail = 0;
void tryCorrupt(const std::string& path, Disc& ref, const Ref& snap, bool hasCrc, const std::string& what,
                bool mustFail) {
  std::string err;
  auto d = open(path, &err);
  if (!d) { corruptRefused++; return; }
  corruptOpened++;
  const bool same = sameTracks(ref, *d);
  uint8_t raw[RAW_SECTOR], data[DATA_SECTOR];
  int wrong = 0, failed = 0;
  const uint32_t n = d->totalSectors() + 3;
  for (uint32_t l = 0; l < n && l < 400000; l++) {
    const bool r = d->readRaw(l, raw);
    const bool q = d->readData(l, data);
    if (!r && l < d->totalSectors()) failed++;
    if (same && hasCrc && l < snap.okRaw.size()) {
      if (r && (!snap.okRaw[l] || std::memcmp(raw, &snap.raw[size_t(l) * RAW_SECTOR], RAW_SECTOR) != 0)) wrong++;
      if (q && (!snap.okData[l] || std::memcmp(data, &snap.data[size_t(l) * DATA_SECTOR], DATA_SECTOR) != 0)) wrong++;
    }
  }
  if (failed) corruptReadFail++;
  check(wrong == 0, what + ": setor errado lido como certo");
  if (mustFail) check(failed > 0 || !same, what + ": arquivo cortado não foi recusado");
}

void corruptTests(const std::string& dir, const std::string& tmp, const std::string& chdName,
                  const std::string& cueName, int flips) {
  auto cue = open(dir + cueName);
  if (!check(cue != nullptr, "abrir " + cueName)) return;
  const Ref snap = snapshot(*cue);
  const std::vector<uint8_t> orig = readAll(dir + chdName);
  if (!check(orig.size() > 200, "ler " + chdName)) return;
  const bool hasCrc = chdName.find("_none") == std::string::npos;
  const std::string p = tmp + "/corrompido.chd";
  // cortado em vários pontos
  const size_t cuts[] = {0, 7, 8, 16, 100, 123, 124, 200, orig.size() / 3, orig.size() / 2, orig.size() - 100,
                         orig.size() - 17, orig.size() - 1};
  for (size_t c : cuts) {
    writeAll(p, orig, c);
    tryCorrupt(p, *cue, snap, hasCrc, chdName + " cortado em " + std::to_string(c), true);
  }
  // um byte trocado por vez: no cabeçalho, no fim (mapa) e em qualquer lugar
  std::mt19937 rng(uint32_t(orig.size()));
  std::vector<uint8_t> buf;
  for (int i = 0; i < flips; i++) {
    buf = orig;
    size_t pos;
    if (i % 4 == 0) pos = rng() % 124;
    else if (i % 4 == 1) pos = orig.size() - 1 - rng() % std::min<size_t>(orig.size(), 600);
    else pos = rng() % orig.size();
    buf[pos] ^= uint8_t(1 + rng() % 255);
    writeAll(p, buf, buf.size());
    tryCorrupt(p, *cue, snap, hasCrc, chdName + " byte " + std::to_string(pos), false);
  }
  // vários bytes aleatórios de uma vez (fuzz)
  for (int i = 0; i < flips / 2; i++) {
    buf = orig;
    const int k = 1 + int(rng() % 40);
    for (int j = 0; j < k; j++) buf[rng() % buf.size()] = uint8_t(rng());
    writeAll(p, buf, buf.size());
    tryCorrupt(p, *cue, snap, hasCrc, chdName + " fuzz " + std::to_string(i), false);
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "uso: %s pasta-com-lista.txt pasta-temporária\n", argv[0]); return 2; }
  const std::string dir = std::string(argv[1]) + "/", tmp = argv[2];
  std::ifstream list(dir + "lista.txt");
  if (!check(bool(list), "abrir lista.txt (rode tools/t7_chd.sh)")) {
    std::printf("RESULTADO chd: %d verificações, %d falhas\n", checks, fails);
    return 1;
  }
  std::vector<std::pair<std::string, std::string>> pairs;
  std::string line;
  while (std::getline(list, line)) {
    std::istringstream in(line);
    std::string a, b;
    if (in >> a >> b) pairs.emplace_back(a, b);
  }
  for (auto& [c, q] : pairs) compareImage(dir, c, q);
  std::printf("  %zu imagens .chd comparadas com o .cue, %llu setores\n", pairs.size(),
              static_cast<unsigned long long>(sectors));

  // cada codec e cada tipo de hunk apareceu em alguma imagem
  for (const char* c : {"cdlz", "cdzl", "cdfl", "zlib", "lzma", "huff", "flac"})
    check(codecHunks[c] > 0, std::string("hunks com o codec ") + c);
  check(noneHunks > 0, "hunks sem compressão");
  check(selfHunks > 0, "hunks copiados de outro hunk");
  check(eccRestored > 0, "setores com a correção (ECC) refeita");
  check(bigHunkCd > 0, "codecs de CD com hunk maior que 64 KiB");
  std::printf("  hunks por codec:");
  for (auto& [c, n] : codecHunks) std::printf(" %s=%llu", c.c_str(), static_cast<unsigned long long>(n));
  std::printf("; sem compressão=%llu, cópias=%llu, ECC refeita em %llu setores\n",
              static_cast<unsigned long long>(noneHunks), static_cast<unsigned long long>(selfHunks),
              static_cast<unsigned long long>(eccRestored));

  // arquivos que não são .chd de CD
  {
    std::string err;
    const std::string p = tmp + "/falso.chd";
    std::vector<uint8_t> junk(5000, 0x41);
    writeAll(p, junk, junk.size());
    check(!open(p, &err) && !err.empty(), "arquivo qualquer com extensão .chd é recusado");
    check(!open(tmp + "/nao-existe.chd", &err), "arquivo inexistente é recusado");
  }

  // metadados antigos (CHTR, sem pregap): o .chd de faixa única com a etiqueta CHT2 trocada por CHTR
  {
    std::vector<uint8_t> d = readAll(dir + "d_padrao.chd");
    int n = 0;
    for (size_t i = 0; i + 4 <= d.size(); i++)
      if (std::memcmp(&d[i], "CHT2", 4) == 0) { d[i + 3] = 'R'; n++; }
    if (check(n == 1, "achar a etiqueta CHT2 em d_padrao.chd")) {
      writeAll(tmp + "/d_chtr.chd", d, d.size());
      std::string err;
      auto a = open(dir + "d.cue"), b = open(tmp + "/d_chtr.chd", &err);
      check(a && b && sameTracks(*a, *b), "faixas do .chd com CHTR: " + err);
      if (a && b) {
        int bad = 0;
        uint8_t x[RAW_SECTOR], y[RAW_SECTOR];
        for (uint32_t l = 0; l < a->totalSectors(); l++)
          if (!a->readRaw(l, x) || !b->readRaw(l, y) || std::memcmp(x, y, RAW_SECTOR) != 0) bad++;
        check(bad == 0, "setores do .chd com CHTR");
      }
    }
  }

  // corrompidos e cortados: imagens pequenas com codecs diferentes (INDIGO_T7_CHD_FUZZ=N multiplica as tentativas)
  int fuzzMul = 1;
  if (const char* e = std::getenv("INDIGO_T7_CHD_FUZZ")) fuzzMul = std::max(1, std::atoi(e));
  for (const char* base : {"f_padrao", "e_padrao", "g_cdfl", "c_dados", "b_hs32", "e_huff", "e_flac", "f_none",
                           "d_cdzl", "a_hs2"}) {
    for (auto& [c, q] : pairs)
      if (c == std::string(base) + ".chd") corruptTests(dir, tmp, c, q, 48 * fuzzMul);
  }
  std::printf("  corrompidos: %d recusados na abertura, %d abertos (%d com setores recusados)\n", corruptRefused,
              corruptOpened, corruptReadFail);
  std::printf("RESULTADO chd: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
