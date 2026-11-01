// Leitor de imagens de CD: monta imagens sintéticas (.cue com vários .bin, faixas de áudio, INDEX 00 e PREGAP,
// .iso, .bin sozinho) numa pasta temporária e confere LBA, faixas e o conteúdo de cada setor; também confere a
// detecção do console (Sega CD, PC Engine CD, Neo Geo CD).
// Uso: indigo-t7_cdrom <pasta-temporária>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "cdrom/cdrom.h"

using namespace indigo::cdrom;

namespace {
int checks = 0, fails = 0;
void check(bool ok, const std::string& what) {
  checks++;
  if (!ok) { fails++; std::printf("  FALHA: %s\n", what.c_str()); }
}
// setor bruto com um marcador: (arquivo, setor no arquivo) nos bytes de dados
void rawSector(uint8_t* s, int fileId, uint32_t sec, int mode) {
  std::memset(s, 0, RAW_SECTOR);
  if (mode == 0) {  // áudio
    for (int i = 0; i < RAW_SECTOR; i++) s[i] = uint8_t(fileId * 31 + sec * 7 + i);
    return;
  }
  std::memset(s + 1, 0xFF, 10);
  s[15] = uint8_t(mode);
  const int off = mode == 2 ? 24 : 16;
  s[off] = uint8_t(fileId); s[off + 1] = uint8_t(sec); s[off + 2] = uint8_t(sec >> 8); s[off + 3] = 0xA5;
}
void writeBin(const std::string& p, int fileId, uint32_t n, int mode) {
  std::ofstream f(p, std::ios::binary);
  uint8_t s[RAW_SECTOR];
  for (uint32_t i = 0; i < n; i++) { rawSector(s, fileId, i, mode); f.write(reinterpret_cast<char*>(s), RAW_SECTOR); }
}
void writeText(const std::string& p, const std::string& t) { std::ofstream(p) << t; }
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s pasta-temporária\n", argv[0]); return 2; }
  const std::string dir = std::string(argv[1]) + "/";

  // 1) .cue com 3 arquivos: dados (300 setores), áudio com INDEX 00 de 2 s, áudio com PREGAP de 1 s
  writeBin(dir + "a (Track 1).bin", 1, 300, 1);
  writeBin(dir + "a (Track 2).bin", 2, 500, 0);
  writeBin(dir + "a (Track 3).bin", 3, 400, 0);
  writeText(dir + "a.cue",
            "FILE \"a (Track 1).bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n"
            "FILE \"a (Track 2).bin\" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n"
            "FILE \"a (Track 3).bin\" BINARY\r\n  TRACK 03 AUDIO\r\n    PREGAP 00:01:00\r\n    INDEX 01 00:00:00\r\n");
  std::string err;
  auto d = open(dir + "a.cue", &err);
  check(d != nullptr, "abrir a.cue: " + err);
  if (d) {
    const auto& t = d->tracks();
    check(t.size() == 3, "a.cue: 3 faixas");
    if (t.size() == 3) {
      check(t[0].start == 0 && t[0].length == 300, "faixa 1: LBA 0, 300 setores");
      check(t[1].start == 450 && t[1].pregap == 150 && t[1].length == 350, "faixa 2: LBA 450, pregap 150, 350 setores");
      check(t[2].start == 875 && t[2].pregap == 75 && t[2].length == 400, "faixa 3: LBA 875, pregap 75 (PREGAP), 400");
      check(d->totalSectors() == 1275, "lead-out em 1275");
      check(t[1].type == TrackType::Audio && t[0].type == TrackType::Mode1, "tipos das faixas");
    }
    uint8_t raw[RAW_SECTOR], exp[RAW_SECTOR], data[DATA_SECTOR];
    check(d->readData(123, data) && data[0] == 1 && (data[1] | data[2] << 8) == 123 && data[3] == 0xA5, "dados LBA 123");
    rawSector(exp, 2, 0, 0);
    check(d->readRaw(300, raw) && std::memcmp(raw, exp, RAW_SECTOR) == 0, "pregap no arquivo (INDEX 00) lido");
    rawSector(exp, 2, 160, 0);
    check(d->readRaw(460, raw) && std::memcmp(raw, exp, RAW_SECTOR) == 0, "áudio da faixa 2");
    std::vector<uint8_t> z(RAW_SECTOR, 0);
    check(d->readRaw(810, raw) && std::memcmp(raw, z.data(), RAW_SECTOR) == 0, "PREGAP fora do arquivo vem zerado");
    rawSector(exp, 3, 10, 0);
    check(d->readRaw(885, raw) && std::memcmp(raw, exp, RAW_SECTOR) == 0, "áudio da faixa 3");
    check(!d->readRaw(1275, raw), "fora do disco: falso");
    check(!d->readData(460, data), "readData em áudio: falso");
    check(d->trackAt(299) == 0 && d->trackAt(300) == 1 && d->trackAt(800) == 2 && d->trackAt(1275) == -1, "trackAt");
  }

  // 2) .cue de um arquivo só com faixa de dados Mode 2 e áudio no mesmo .bin
  {
    std::ofstream f(dir + "b.bin", std::ios::binary);
    uint8_t s[RAW_SECTOR];
    for (uint32_t i = 0; i < 200; i++) { rawSector(s, 9, i, i < 100 ? 2 : 0); f.write(reinterpret_cast<char*>(s), RAW_SECTOR); }
  }
  writeText(dir + "b.cue", "FILE b.bin BINARY\n TRACK 01 MODE2/2352\n  INDEX 01 00:00:00\n"
                           " TRACK 02 AUDIO\n  INDEX 00 00:01:00\n  INDEX 01 00:01:30\n");
  d = open(dir + "b.cue", &err);
  check(d && d->tracks().size() == 2, "abrir b.cue: " + err);
  if (d && d->tracks().size() == 2) {
    const auto& t = d->tracks();
    check(t[0].length == 75 && t[1].start == 105 && t[1].pregap == 30 && t[1].length == 95, "b.cue: faixas no mesmo .bin");
    uint8_t data[DATA_SECTOR];
    check(d->readData(50, data) && data[0] == 9 && data[1] == 50, "Mode 2 Form 1: dados após o subcabeçalho");
  }

  // 3) .iso (2048 por setor) remontado como Mode 1, com MSF no cabeçalho
  {
    std::ofstream f(dir + "c.iso", std::ios::binary);
    std::vector<uint8_t> s(DATA_SECTOR);
    for (uint32_t i = 0; i < 40; i++) { std::fill(s.begin(), s.end(), uint8_t(i)); f.write(reinterpret_cast<char*>(s.data()), DATA_SECTOR); }
  }
  d = open(dir + "c.iso", &err);
  check(d && d->totalSectors() == 40, "abrir c.iso: " + err);
  if (d) {
    uint8_t raw[RAW_SECTOR];
    // LBA 20 -> MSF 00:02:20 -> BCD 00 02 20
    check(d->readRaw(20, raw) && raw[1] == 0xFF && raw[12] == 0x00 && raw[13] == 0x02 && raw[14] == 0x20 && raw[15] == 1 && raw[16] == 20,
          ".iso: cabeçalho Mode 1 e dados");
  }

  // 4) detecção do console
  auto isoWith = [&](const std::string& nome, auto fill) {
    std::vector<uint8_t> img(DATA_SECTOR * 40, 0);
    fill(img);
    std::ofstream(dir + nome, std::ios::binary).write(reinterpret_cast<char*>(img.data()), std::streamsize(img.size()));
    auto disc = open(dir + nome, &err);
    return disc ? detectSystem(*disc) : std::string("erro");
  };
  check(isoWith("sega.iso", [](std::vector<uint8_t>& im) { std::memcpy(im.data(), "SEGADISCSYSTEM  ", 16); }) == "segacd", "detecta Sega CD");
  check(isoWith("pce.iso", [](std::vector<uint8_t>& im) { std::memcpy(im.data() + DATA_SECTOR + 32, "PC Engine CD-ROM SYSTEM", 23); }) == "pcecd",
        "detecta PC Engine CD");
  check(isoWith("ngcd.iso", [](std::vector<uint8_t>& im) {
          uint8_t* pvd = im.data() + 16 * DATA_SECTOR;
          pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5);
          uint8_t* r = pvd + 156; r[0] = 34; r[2] = 20; r[10] = 0x00; r[11] = 0x08;  // raiz no setor 20, 2048 bytes
          uint8_t* e = im.data() + 20 * DATA_SECTOR;
          e[0] = 34; e[32] = 1;                         // "."
          uint8_t* g = e + 34; g[0] = 42; g[32] = 9; std::memcpy(g + 33, "IPL.TXT;1", 9);
        }) == "neogeocd", "detecta Neo Geo CD");
  auto psxRoot = [](const char* nome, int nl) {
    return [nome, nl](std::vector<uint8_t>& im) {
      uint8_t* pvd = im.data() + 16 * DATA_SECTOR;
      pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5);
      uint8_t* r = pvd + 156; r[0] = 34; r[2] = 22; r[10] = 0x00; r[11] = 0x08;
      uint8_t* e = im.data() + 22 * DATA_SECTOR;
      e[0] = 34; e[32] = 1;
      uint8_t* g = e + 34; g[0] = uint8_t(34 + nl); g[32] = uint8_t(nl); std::memcpy(g + 33, nome, size_t(nl));
    };
  };
  check(isoWith("psx1.iso", psxRoot("SYSTEM.CNF;1", 12)) == "ps1", "detecta PlayStation (SYSTEM.CNF)");
  check(isoWith("psx2.iso", psxRoot("PSX.EXE;1", 9)) == "ps1", "detecta PlayStation (PSX.EXE)");
  check(isoWith("psx3.iso", [](std::vector<uint8_t>& im) {
          std::memcpy(im.data() + 4 * DATA_SECTOR + 32, "          Licensed  by          Sony Computer Entertainment Inc.", 64);
        }) == "ps1", "detecta PlayStation (licença no setor 4)");
  check(isoWith("psx4.iso", psxRoot("README.TXT;1", 12)) == "", "ISO 9660 sem SYSTEM.CNF nem PSX.EXE não é PlayStation");
  check(isoWith("nada.iso", [](std::vector<uint8_t>&) {}) == "", "disco desconhecido");

  std::printf("RESULTADO cdrom: %d verificações, %d falhas\n", checks, fails);
  return fails == 0 ? 0 : 1;
}
