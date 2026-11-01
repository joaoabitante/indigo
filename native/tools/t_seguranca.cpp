// Segurança: arquivos maliciosos montados aqui mesmo (backup com NaN, .zip com "..\..\", nomes enormes no Neo
// Geo, .chd com mapa gigante, .cue com tempo estourado ou fora da pasta) e nomes vindos de outro app. Tudo tem
// de ser recusado sem travar, sem exceção solta e sem ler/gravar fora da pasta certa.
// Uso: indigo-t_seguranca <pasta-temporária>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <sys/resource.h>
#endif

#include "cdrom/cdrom.h"
#include "core/backup.h"
#include "core/safe.h"
#include "core/zip.h"
#include "neogeo/cart.h"

using namespace indigo;

namespace {
int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("  FALHA: %s\n", what.c_str()); }
}

void put16(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
void put32(std::vector<uint8_t>& b, uint32_t v) { put16(b, v & 0xFFFF); put16(b, v >> 16); }

// .zip sem compressão; `declared` troca o tamanho gravado no diretório (0 = o verdadeiro)
struct ZEntry { std::string name; std::vector<uint8_t> data; uint32_t declared = 0; };
std::vector<uint8_t> makeZip(const std::vector<ZEntry>& es) {
  std::vector<uint8_t> z, dir;
  for (const auto& e : es) {
    const uint32_t off = uint32_t(z.size()), n = uint32_t(e.data.size()), sz = e.declared ? e.declared : n;
    put32(z, 0x04034b50); put16(z, 20); put16(z, 0); put16(z, 0); put32(z, 0); put32(z, 0);
    put32(z, n); put32(z, sz); put16(z, uint32_t(e.name.size())); put16(z, 0);
    z.insert(z.end(), e.name.begin(), e.name.end());
    z.insert(z.end(), e.data.begin(), e.data.end());
    put32(dir, 0x02014b50); put16(dir, 20); put16(dir, 20); put16(dir, 0); put16(dir, 0); put32(dir, 0); put32(dir, 0);
    put32(dir, n); put32(dir, sz); put16(dir, uint32_t(e.name.size())); put16(dir, 0); put16(dir, 0); put16(dir, 0);
    put16(dir, 0); put32(dir, 0); put32(dir, off);
    dir.insert(dir.end(), e.name.begin(), e.name.end());
  }
  const uint32_t dirOff = uint32_t(z.size());
  z.insert(z.end(), dir.begin(), dir.end());
  put32(z, 0x06054b50); put16(z, 0); put16(z, 0); put16(z, uint32_t(es.size())); put16(z, uint32_t(es.size()));
  put32(z, uint32_t(dir.size())); put32(z, dirOff); put16(z, 0);
  return z;
}

// cartucho de Game Boy mínimo (32 KB) para o .zip ter uma "ROM"
std::vector<uint8_t> gbRom() { return std::vector<uint8_t>(0x8000, 0); }

// backup .indigo com o JSON dado e 4 bytes de dados depois
std::vector<uint8_t> makeBackup(const std::string& json) {
  std::vector<uint8_t> b(std::begin("INDIGOB1"), std::begin("INDIGOB1") + 8);
  put32(b, uint32_t(json.size()));
  b.insert(b.end(), json.begin(), json.end());
  for (int k = 0; k < 4; k++) b.push_back('A');
  return b;
}

// pico de memória do processo (KB); 0 onde não há getrusage
long peakKb() {
#ifdef __linux__
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return u.ru_maxrss;
#else
  return 0;
#endif
}

void writeBytes(const std::string& p, const std::vector<uint8_t>& d) {
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
}
void writeText(const std::string& p, const std::string& t) { std::ofstream(p) << t; }
void be32(std::vector<uint8_t>& b, size_t o, uint32_t v) { for (int k = 0; k < 4; k++) b[o + k] = uint8_t(v >> (24 - 8 * k)); }
void be64(std::vector<uint8_t>& b, size_t o, uint64_t v) { be32(b, o, uint32_t(v >> 32)); be32(b, o + 4, uint32_t(v)); }

// ---------------------------------------------------------------- nomes de arquivo (A1, A4)
void nomes() {
  check(safeFileName("../../nuvem.txt") == ".._.._nuvem.txt", "\"../../nuvem.txt\" perde as barras");
  check(safeFileName("..\\..\\x.gb") == ".._.._x.gb", "barra invertida vira _");
  check(safeFileName("..") == "jogo" && safeFileName(".") == "jogo" && safeFileName("") == "jogo", "\"..\", \".\" e vazio viram \"jogo\"");
  check(safeFileName(std::string("a\0b.gb", 6)) == "a_b.gb", "NUL vira _");
  check(safeFileName("a\nb\x7f.gb") == "a_b_.gb", "controles viram _");
  check(safeFileName("C:\\Windows\\x.gb").find_first_of(":\\/") == std::string::npos, "letra de unidade sai");
  check(safeFileName("jogo. . ") == "jogo", "pontos e espaços no fim saem (Windows)");
  check(safeFileName("Zelda (BR).gbc") == "Zelda (BR).gbc", "nome comum fica igual");
  check(safeFileName("Pokémon Edição Ouro.gbc") == "Pokémon Edição Ouro.gbc", "acentos ficam");
  const std::string longo = safeFileName(std::string(5000, 'a') + ".gba");
  check(longo.size() <= 200 && longo.size() > 100 && longo.compare(longo.size() - 4, 4, ".gba") == 0, "nome enorme é cortado e mantém a extensão");
  std::string utf;
  for (int k = 0; k < 150; k++) utf += "é";
  const std::string cortado = safeFileName(utf + ".gb");
  check(cortado.size() <= 200 && (static_cast<unsigned char>(cortado[cortado.size() - 4]) & 0xC0) != 0xC0, "corte não deixa letra UTF-8 pela metade");
  // .zip com caminho do Windows: o nome da ROM (chave do save) fica sem pastas
  std::string inner, err;
  std::vector<uint8_t> data;
  const auto z = makeZip({{"..\\..\\x.gb", gbRom()}});
  check(zip::extractRom(z, inner, data, err) && inner == "x.gb", "zip \"..\\..\\x.gb\" vira \"x.gb\" (veio \"" + inner + "\")");
  const auto z2 = makeZip({{"pasta/../\x01..gb", gbRom()}});
  check(zip::extractRom(z2, inner, data, err) && inner.find_first_of("/\\\x01") == std::string::npos && inner != "..", "zip com controle no nome");
  std::vector<zip::Entry> list;
  check(zip::listEntries(z, list) && list.size() == 1 && list[0].name == "x.gb", "listEntries também separa por barra invertida");
}

// ---------------------------------------------------------------- .zip: limites (A9)
void zipLimites() {
  std::string inner, err;
  std::vector<uint8_t> data;
  // tamanho gravado diferente do real: danificado
  check(!zip::extractRom(makeZip({{"a.gb", gbRom(), 0x10000}}), inner, data, err), "tamanho do diretório diferente do real é recusado");
  // cinco arquivos dizendo 250 MB cada: passa de 1 GB no pacote
  std::vector<ZEntry> es;
  for (int k = 0; k < 5; k++) es.push_back({"p" + std::to_string(k) + ".gb", {1, 2, 3}, 250u * 1024 * 1024});
  const auto big = makeZip(es);
  std::vector<zip::Entry> list;
  check(!zip::listEntries(big, list), "pacote com mais de 1 GB descompactado é recusado");
  check(!zip::extractRom(big, inner, data, err), "extractRom recusa o pacote de mais de 1 GB");
  check(!zip::extractEntry(big, "p0.gb", data), "extractEntry recusa o pacote de mais de 1 GB");
  // pacote normal continua abrindo
  check(zip::extractRom(makeZip({{"b.gb", gbRom()}, {"leia.txt", {1}}}), inner, data, err) && inner == "b.gb" && data.size() == 0x8000, "zip normal abre");
}

// ---------------------------------------------------------------- backup (A2, A7)
void backups() {
  const char* bad[] = {"nan", "-nan", "inf", "-inf", "1e300", "-1", "1.5", "4", "5", "18446744073709551615", "9007199254740993"};
  for (const char* v : bad) {
    for (int campo = 0; campo < 2; campo++) {
      const std::string o = campo == 0 ? v : "0", n = campo == 0 ? "4" : v;
      if (std::string(v) == "4" && campo == 1) continue;  // n = 4 é válido
      const std::string json = "{\"saves\":[{\"key\":\"k\",\"at\":1,\"data\":{\"$ab\":1,\"o\":" + o + ",\"n\":" + n + "}}]}";
      backup::Backup bk;
      std::string err;
      bool ok = false;
      try {
        ok = backup::read(makeBackup(json), bk, err);
      } catch (const std::exception& e) {
        check(false, std::string("backup com ") + (campo ? "n=" : "o=") + v + " soltou exceção: " + e.what());
        continue;
      }
      const bool valid = campo == 0 && std::string(v) == "0";
      check(ok && bk.saves.size() == 1 && (valid || bk.saves[0].data.empty()),
            std::string("backup com ") + (campo ? "n=" : "o=") + v + " não lê fora do arquivo");
    }
  }
  // o certo continua lendo os 4 bytes
  backup::Backup bk;
  std::string err;
  check(backup::read(makeBackup("{\"saves\":[{\"key\":\"k\",\"at\":1,\"data\":{\"$ab\":1,\"o\":0,\"n\":4}}]}"), bk, err) &&
            bk.saves.size() == 1 && bk.saves[0].data.size() == 4,
        "backup certo lê o bloco");
  // horário estranho vira 0 (depois vira long long no nome das versões)
  for (const char* at : {"nan", "inf", "-1e300", "1e300"}) {
    backup::Backup b2;
    check(backup::read(makeBackup(std::string("{\"saves\":[{\"key\":\"k\",\"at\":") + at + ",\"data\":null}],\"hist\":[{\"key\":\"k\",\"at\":" + at + "}]}"), b2, err) &&
              b2.saves.size() == 1 && b2.saves[0].at == 0 && b2.hist.size() == 1 && b2.hist[0].at == 0,
          std::string("\"at\": ") + at + " vira 0");
  }
  // conversões
  const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
  check(!wholeIn(nan, 0, 10) && !wholeIn(inf, 0, 1e300) && !wholeIn(1.5, 0, 10) && !wholeIn(-1, 0, 10) && wholeIn(10, 0, 10), "wholeIn");
  check(safeMs(nan) == 0 && safeMs(inf) == 0 && safeMs(1e300) == 0 && safeMs(-5) == 0 && safeMs(1730000000000.0) == 1730000000000.0, "safeMs");
  check(safeSize(nan) == 0 && safeSize(1e300) == 0 && safeSize(-1) == 0 && safeSize(2.5) == 0 && safeSize(4096) == 4096, "safeSize");
}

// ---------------------------------------------------------------- Neo Geo: nomes enormes (A3)
void neoGeoNomes() {
  const std::vector<uint8_t> d(1024, 0x11);
  const std::string longo(65000, 'a');
  struct Caso { const char* what; std::vector<ZEntry> es; };
  const Caso casos[] = {
      {"nomes de 65 mil letras", {{longo + ".p1", d}, {longo + ".c1", d}, {longo + ".s1", d}}},
      {"número enorme no sufixo", {{"x.p99999999999999999999", d}, {"x.c99999999999999999999", d}}},
      {"muitos dígitos", {{"x.p" + std::string(60000, '9'), d}, {"x.c" + std::string(60000, '1'), d}}},
  };
  for (const auto& c : casos) {
    const auto z = makeZip(c.es);
    bool threw = false, other = false;
    try {
      neogeo::looksLikeNeoGeoZip(z, "teste.zip");
      neogeo::loadZipSet(z, "teste.zip");
    } catch (const std::runtime_error&) {
      threw = true;  // "pacote desconhecido": é a recusa esperada
    } catch (...) {
      other = true;
    }
    check(threw && !other, std::string("Neo Geo com ") + c.what + " é recusado com a mensagem de pacote desconhecido");
  }
  // homebrew pelos sufixos continua sendo reconhecido
  check(neogeo::looksLikeNeoGeoZip(makeZip({{"x.p1", d}, {"x.c1", d}}), "homebrew.zip"), "homebrew com .p1/.c1 é reconhecido");
  check(!neogeo::looksLikeNeoGeoZip(makeZip({{"x.p1", d}, {"x.m1", d}}), "outro.zip"), "sem .c/.s não é Neo Geo");
}

// ---------------------------------------------------------------- .chd com mapa gigante (A5)
void chdMapa(const std::string& tmp) {
  std::vector<uint8_t> h(124 + 16 + 64, 0);
  std::memcpy(h.data(), "MComprHD", 8);
  be32(h, 8, 124); be32(h, 12, 5);
  be32(h, 16, uint32_t('c') << 24 | uint32_t('d') << 16 | uint32_t('l') << 8 | 'z');
  be64(h, 32, 2448ull * 8 * 4);  // 4 hunks
  be64(h, 40, 124);             // mapa logo depois do cabeçalho
  be32(h, 56, 2448 * 8);
  be32(h, 60, 2448);
  be32(h, 124, 0xFFFFFF00u);    // tamanho do mapa: quase 4 GB num arquivo de 204 bytes
  h[124 + 12] = 8; h[124 + 13] = 8; h[124 + 14] = 0;
  const std::string p = tmp + "/mapa.chd";
  writeBytes(p, h);
  std::string err;
  const long antes = peakKb();
  auto d = cdrom::open(p, &err);
  check(!d && err.find("cortado") != std::string::npos, ".chd com mapa maior que o arquivo é recusado (" + err + ")");
  check(peakKb() - antes < 256 * 1024, ".chd com mapa gigante não reserva a memória antes de conferir o arquivo");
}

// ---------------------------------------------------------------- .cue (A6, A8)
void cueArquivos(const std::string& tmp) {
  namespace fs = std::filesystem;
  const std::string dir = tmp + "/cd", sub = dir + "/sub";
  fs::create_directories(sub);
  std::vector<uint8_t> sector(2352 * 20, 0);
  writeBytes(tmp + "/fora.bin", sector);  // fora da pasta do .cue
  writeBytes(dir + "/dentro.bin", sector);
  writeBytes(sub + "/abaixo.bin", sector);
  auto opens = [&](const std::string& body, std::string& err) {
    writeText(dir + "/t.cue", body);
    err.clear();
    return cdrom::open(dir + "/t.cue", &err) != nullptr;
  };
  std::string err;
  const std::string track = "  TRACK 01 MODE1/2352\n";
  check(opens("FILE \"dentro.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue normal abre (" + err + ")");
  check(opens("FILE \"sub/abaixo.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com arquivo em subpasta abre (" + err + ")");
  check(!opens("FILE \"../fora.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com \"../\" não lê fora da pasta");
  check(!opens("FILE \"sub/../../fora.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com \"sub/../../\" não lê fora da pasta");
  check(!opens("FILE \"..\\fora.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com \"..\\\" não lê fora da pasta");
  check(!opens("FILE \"" + fs::absolute(tmp + "/fora.bin").string() + "\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com caminho absoluto não lê fora da pasta");
  // caminho absoluto de outro computador: vale o nome do arquivo na pasta do .cue
  check(opens("FILE \"C:\\jogos\\dentro.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com C:\\...\\dentro.bin usa o da pasta (" + err + ")");
  check(opens("FILE \"/outro/lugar/dentro.bin\" BINARY\n" + track + "    INDEX 01 00:00:00\n", err), "cue com /outro/lugar/dentro.bin usa o da pasta (" + err + ")");
  const char* msf[] = {"99999999:00:00", "2147483647:59:74", "-1:00:00", "00:-5:00", "00:60:00", "00:00:75", "100:00:00"};
  for (const char* t : msf)
    check(!opens("FILE \"dentro.bin\" BINARY\n" + track + "    INDEX 01 " + t + "\n", err), std::string("INDEX 01 ") + t + " é recusado");
  check(!opens("FILE \"dentro.bin\" BINARY\n" + track + "    INDEX 00 00:00:10\n    INDEX 01 00:00:05\n", err), "INDEX 00 depois do INDEX 01 é recusado");
  check(opens("FILE \"dentro.bin\" BINARY\n" + track + "    INDEX 00 00:00:00\n    INDEX 01 00:00:05\n", err), "INDEX 00 antes do 01 abre (" + err + ")");
  std::string many = "FILE \"dentro.bin\" BINARY\n";
  for (int k = 1; k <= 120; k++) many += "  TRACK " + std::to_string(k) + " MODE1/2352\n    INDEX 01 00:00:" + (k < 10 ? "0" : "") + std::to_string(k % 75) + "\n";
  check(!opens(many, err), "mais de 99 faixas é recusado");
}

// ---------------------------------------------------------------- endereço da nuvem (INDIGO_CLOUD_URL)
void nuvem() {
  check(cloudUrlAllowed("https://indigo.example"), "https:// vale");
  check(cloudUrlAllowed("http://127.0.0.1:8787") && cloudUrlAllowed("http://localhost:8787/") && cloudUrlAllowed("HTTP://LOCALHOST"), "http:// local vale (cloudmock)");
  check(!cloudUrlAllowed("http://indigo.example") && !cloudUrlAllowed("http://127.0.0.1.evil.example") && !cloudUrlAllowed("http://localhost.evil.example:80"),
        "http:// de fora não vale");
  check(!cloudUrlAllowed("ftp://x") && !cloudUrlAllowed("https://") && !cloudUrlAllowed("") && !cloudUrlAllowed("file:///etc/passwd"), "outros esquemas não valem");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta-temporária\n", argv[0]); return 2; }
  std::setvbuf(stdout, nullptr, _IOLBF, 0);  // as falhas aparecem mesmo se algo derrubar o teste
  const std::string tmp = argv[1];
  std::filesystem::create_directories(tmp);
  nomes();
  zipLimites();
  backups();
  neoGeoNomes();
  chdMapa(tmp);
  cueArquivos(tmp);
  nuvem();
  std::printf("RESULTADO segurança: %d verificações, %d falhas\n", checks, fails);
  return fails ? 1 : 0;
}
