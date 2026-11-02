#include "cdrom/cdrom.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include "cdrom/chd.h"

namespace indigo::cdrom {

int Disc::trackAt(uint32_t lba) const {
  for (size_t i = 0; i < tracks_.size(); i++) {
    const Track& t = tracks_[i];
    if (lba + t.pregap >= t.start && lba < t.start + t.length) return int(i);
  }
  return -1;
}

bool Disc::readData(uint32_t lba, uint8_t* out) {
  const int ti = trackAt(lba);
  if (ti < 0 || tracks_[size_t(ti)].type == TrackType::Audio) { std::memset(out, 0, DATA_SECTOR); return false; }
  uint8_t raw[RAW_SECTOR];
  if (!readRaw(lba, raw)) { std::memset(out, 0, DATA_SECTOR); return false; }
  // Mode 2: subcabeçalho de 8 bytes antes dos dados (Form 1); Mode 1: dados logo após o cabeçalho
  const int off = raw[15] == 2 ? 24 : 16;
  std::memcpy(out, raw + off, DATA_SECTOR);
  return true;
}

namespace {

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}
std::string extOf(const std::string& p) {
  const size_t d = p.find_last_of('.'), s = p.find_last_of("/\\");
  if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
  return lower(p.substr(d));
}
std::string dirOf(const std::string& p) {
  const size_t s = p.find_last_of("/\\");
  return s == std::string::npos ? "" : p.substr(0, s + 1);
}
std::string stemOf(const std::string& p) {
  const size_t s = p.find_last_of("/\\");
  std::string n = s == std::string::npos ? p : p.substr(s + 1);
  const size_t d = n.find_last_of('.');
  return d == std::string::npos ? n : n.substr(0, d);
}
uint8_t bcd(int v) { return uint8_t((v / 10) << 4 | (v % 10)); }

// cabeçalho de setor Mode 1 (sincronismo + MSF em BCD + modo) para faixas guardadas só com os 2048 bytes
void makeMode1(uint32_t lba, const uint8_t* data, uint8_t* raw) {
  std::memset(raw, 0, RAW_SECTOR);
  raw[0] = 0;
  std::memset(raw + 1, 0xFF, 10);
  raw[11] = 0;
  uint8_t m, s, f;
  lbaToMsf(lba, m, s, f);
  raw[12] = bcd(m); raw[13] = bcd(s); raw[14] = bcd(f); raw[15] = 1;
  std::memcpy(raw + 16, data, DATA_SECTOR);
}

struct FileTrack {
  int file = 0;
  int sectorSize = RAW_SECTOR;
  uint32_t fileSector0 = 0;  // setor (no arquivo) do INDEX 01
  uint32_t filePregap = 0;   // setores de pregap que estão no arquivo (INDEX 00 .. INDEX 01)
  uint32_t postgap = 0;      // POSTGAP: setores no fim da faixa que não estão no arquivo (lidos como zero)
};

class FileDisc : public Disc {
public:
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, RAW_SECTOR);
    const int ti = trackAt(lba);
    if (ti < 0) return false;
    const Track& t = tracks_[size_t(ti)];
    const FileTrack& ft = ft_[size_t(ti)];
    int64_t sec = int64_t(ft.fileSector0) - (int64_t(t.start) - int64_t(lba));
    if (sec < int64_t(ft.fileSector0) - int64_t(ft.filePregap)) return true;  // pregap fora do arquivo: zeros
    if (lba >= t.start + t.length - ft.postgap) return true;                  // postgap: zeros
    std::ifstream& f = *files_[size_t(ft.file)];
    f.clear();
    f.seekg(sec * ft.sectorSize);
    if (ft.sectorSize == DATA_SECTOR) {
      uint8_t d[DATA_SECTOR] = {};
      f.read(reinterpret_cast<char*>(d), DATA_SECTOR);
      makeMode1(lba, d, out);
    } else if (ft.sectorSize == RAW_SECTOR) {
      f.read(reinterpret_cast<char*>(out), RAW_SECTOR);
    } else {  // 2336: Mode 2 sem sincronismo/cabeçalho
      makeMode1(lba, out + 16, out);
      out[15] = 2;
      f.read(reinterpret_cast<char*>(out + 16), ft.sectorSize);
    }
    return true;
  }

  bool addFile(const std::string& path, uint64_t& size) {
    auto f = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*f) return false;
    f->seekg(0, std::ios::end);
    size = uint64_t(f->tellg());
    files_.push_back(std::move(f));
    return true;
  }
  void add(const Track& t, const FileTrack& ft) { tracks_.push_back(t); ft_.push_back(ft); }
  void finish(uint32_t total, const std::string& name) { total_ = total; name_ = name; }
  std::vector<Track>& tr() { return tracks_; }

private:
  std::vector<std::unique_ptr<std::ifstream>> files_;
  std::vector<FileTrack> ft_;
};

// mm:ss:ff de um CD: minutos até 99, segundos até 59 e quadros até 74 (fora disso o .cue está errado)
bool parseMsf(const std::string& s, uint32_t& out) {
  long m, sec, f;
  char c1, c2;
  std::istringstream in(s);
  if (!(in >> m >> c1 >> sec >> c2 >> f) || c1 != ':' || c2 != ':') return false;
  if (m < 0 || m > 99 || sec < 0 || sec > 59 || f < 0 || f > 74) return false;
  out = uint32_t((m * 60 + sec) * 75 + f);
  return true;
}

// arquivo citado no .cue: só na pasta do .cue ou abaixo dela (sem caminho absoluto nem "..")
bool cueNameSafe(const std::string& n) {
  if (n.empty() || n[0] == '/' || n[0] == '\\' || (n.size() >= 2 && n[1] == ':')) return false;
  size_t a = 0;
  while (a <= n.size()) {
    const size_t b = n.find_first_of("/\\", a);
    const std::string part = n.substr(a, b == std::string::npos ? std::string::npos : b - a);
    if (part == "..") return false;
    if (b == std::string::npos) break;
    a = b + 1;
  }
  return n.find('\0') == std::string::npos;
}

std::unique_ptr<Disc> openCue(const std::string& path, std::string* err) {
  std::ifstream in(path);
  if (!in) { if (err) *err = "não abriu o .cue"; return nullptr; }
  auto d = std::make_unique<FileDisc>();
  struct Pending {
    Track t;
    FileTrack ft;
    uint32_t idx0 = UINT32_MAX, idx1 = UINT32_MAX, cmdPregap = 0, cmdPostgap = 0;
  };
  std::vector<Pending> cur;  // faixas do arquivo atual
  int fileIdx = -1;
  uint64_t fileBytes = 0;
  uint32_t cursor = 0;  // LBA onde começa o próximo arquivo
  std::string line;

  auto flushFile = [&]() -> bool {
    if (fileIdx < 0) return true;
    if (cur.empty()) return true;
    const int ss = cur[0].ft.sectorSize;
    const uint32_t fileSectors = uint32_t(fileBytes / uint64_t(ss));
    const uint32_t base = cursor;
    uint32_t shift = 0;
    for (size_t i = 0; i < cur.size(); i++) {
      Pending& p = cur[i];
      if (p.idx1 == UINT32_MAX) { if (err) *err = "faixa sem INDEX 01 no .cue"; return false; }
      if (p.idx0 != UINT32_MAX && p.idx0 > p.idx1) { if (err) *err = "INDEX 00 depois do INDEX 01 no .cue"; return false; }
      shift += p.cmdPregap;
      const uint32_t idx0 = p.idx0 == UINT32_MAX ? p.idx1 : p.idx0;
      p.t.start = base + shift + p.idx1;
      p.ft.fileSector0 = p.idx1;
      p.ft.filePregap = p.idx1 - idx0;
      p.t.pregap = p.ft.filePregap + p.cmdPregap;
      p.ft.postgap = p.cmdPostgap;
      shift += p.cmdPostgap;  // o POSTGAP fica no fim da faixa e empurra as seguintes
    }
    const uint32_t fileEnd = base + shift + fileSectors;
    for (size_t i = 0; i < cur.size(); i++) {
      const uint32_t end = i + 1 < cur.size() ? cur[i + 1].t.start - cur[i + 1].t.pregap : fileEnd;
      cur[i].t.length = end > cur[i].t.start ? end - cur[i].t.start : 0;
      d->add(cur[i].t, cur[i].ft);
    }
    cursor = fileEnd;
    cur.clear();
    return true;
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string cmd;
    ls >> cmd;
    cmd = lower(cmd);
    if (cmd == "file") {
      if (!flushFile()) return nullptr;
      std::string rest;
      std::getline(ls, rest);
      std::string fname;
      const size_t q1 = rest.find('"'), q2 = rest.rfind('"');
      if (q1 != std::string::npos && q2 > q1) fname = rest.substr(q1 + 1, q2 - q1 - 1);
      else { std::istringstream r(rest); r >> fname; }
      // .cue costuma vir com o nome do .bin sem pasta; caminhos com barra invertida do Windows também
      // (com caminho absoluto ou "..", vale só o nome do arquivo, procurado na pasta do .cue)
      const size_t sl = fname.find_last_of("/\\");
      const std::string leaf = sl == std::string::npos ? fname : fname.substr(sl + 1);
      const bool safe = cueNameSafe(fname);
      uint64_t sz = 0;
      if (!(safe && d->addFile(dirOf(path) + fname, sz)) &&
          !(sl != std::string::npos && cueNameSafe(leaf) && d->addFile(dirOf(path) + leaf, sz))) {
        if (err) *err = "não achou o arquivo " + fname + " citado no .cue";
        return nullptr;
      }
      fileIdx++;
      fileBytes = sz;
    } else if (cmd == "track") {
      if (fileIdx < 0) { if (err) *err = "TRACK antes de FILE no .cue"; return nullptr; }
      if (d->tr().size() + cur.size() >= 99) { if (err) *err = "mais de 99 faixas no .cue"; return nullptr; }
      int n = 0;
      std::string mode;
      ls >> n >> mode;
      mode = lower(mode);
      Pending p;
      p.t.number = n;
      p.ft.file = fileIdx;
      if (mode == "audio") { p.t.type = TrackType::Audio; p.ft.sectorSize = RAW_SECTOR; }
      else if (mode == "mode1/2048") { p.t.type = TrackType::Mode1; p.ft.sectorSize = DATA_SECTOR; }
      else if (mode == "mode1/2352") { p.t.type = TrackType::Mode1; p.ft.sectorSize = RAW_SECTOR; }
      else if (mode == "mode2/2352") { p.t.type = TrackType::Mode2; p.ft.sectorSize = RAW_SECTOR; }
      else if (mode == "mode2/2336") { p.t.type = TrackType::Mode2; p.ft.sectorSize = 2336; }
      else { if (err) *err = "tipo de faixa não suportado no .cue: " + mode; return nullptr; }
      if (!cur.empty() && cur[0].ft.sectorSize != p.ft.sectorSize) {
        if (err) *err = "faixas com tamanhos de setor diferentes no mesmo arquivo";
        return nullptr;
      }
      cur.push_back(p);
    } else if (cmd == "index" && !cur.empty()) {
      int n = -1;
      std::string t;
      ls >> n >> t;
      uint32_t v;
      if (!parseMsf(t, v)) { if (err) *err = "INDEX inválido no .cue"; return nullptr; }
      if (n == 0) cur.back().idx0 = v;
      else if (n == 1) cur.back().idx1 = v;
    } else if (cmd == "pregap" && !cur.empty()) {
      std::string t;
      ls >> t;
      uint32_t v;
      if (parseMsf(t, v)) cur.back().cmdPregap = v;
    } else if (cmd == "postgap" && !cur.empty()) {
      std::string t;
      ls >> t;
      uint32_t v;
      if (parseMsf(t, v)) cur.back().cmdPostgap = v;
    }
  }
  if (!flushFile()) return nullptr;
  if (d->tracks().empty()) { if (err) *err = "o .cue não tem faixas"; return nullptr; }
  d->finish(cursor, stemOf(path));
  return d;
}

std::unique_ptr<Disc> openSingle(const std::string& path, int sectorSize, std::string* err) {
  auto d = std::make_unique<FileDisc>();
  uint64_t sz = 0;
  if (!d->addFile(path, sz)) { if (err) *err = "não abriu o arquivo"; return nullptr; }
  Track t;
  t.number = 1;
  t.type = TrackType::Mode1;
  t.start = 0;
  t.length = uint32_t(sz / uint64_t(sectorSize));
  FileTrack ft;
  ft.sectorSize = sectorSize;
  d->add(t, ft);
  if (sectorSize == RAW_SECTOR) {  // .bin sozinho: o modo vem do cabeçalho do primeiro setor
    uint8_t raw[RAW_SECTOR];
    if (d->readRaw(0, raw) && raw[15] == 2) d->tr()[0].type = TrackType::Mode2;
  }
  d->finish(t.length, stemOf(path));
  return d;
}

}  // namespace

bool isDiscPath(const std::string& path) {
  const std::string e = extOf(path);
  return e == ".cue" || e == ".chd" || e == ".iso";
}

std::unique_ptr<Disc> open(const std::string& path, std::string* err) {
  const std::string e = extOf(path);
  if (e == ".cue") return openCue(path, err);
  if (e == ".chd") return openChd(path, err);
  if (e == ".iso") return openSingle(path, DATA_SECTOR, err);
  if (e == ".bin" || e == ".img") return openSingle(path, RAW_SECTOR, err);
  if (err) *err = "extensão de imagem de CD desconhecida";
  return nullptr;
}

std::string detectSystem(Disc& d) {
  int first = -1;
  for (size_t i = 0; i < d.tracks().size(); i++)
    if (d.tracks()[i].type != TrackType::Audio) { first = int(i); break; }
  if (first < 0) return "";
  const uint32_t s0 = d.tracks()[size_t(first)].start;
  uint8_t buf[DATA_SECTOR], raw[RAW_SECTOR];
  if (d.readData(s0, buf)) {
    if (std::memcmp(buf, "PC-FX:Hu_CD-ROM", 15) == 0) return "pcfx";
    if (std::memcmp(buf, "SEGA SEGASATURN ", 16) == 0) return "saturn";
    if (std::memcmp(buf, "SEGADISCSYSTEM", 14) == 0 || std::memcmp(buf, "SEGABOOTDISC", 12) == 0) return "segacd";
    // 3DO: rótulo do volume do sistema de arquivos Opera (tipo 1, cinco bytes 0x5A, versão 1, "CD-ROM")
    if (buf[0] == 1 && buf[1] == 0x5A && buf[2] == 0x5A && buf[3] == 0x5A && buf[4] == 0x5A && buf[5] == 0x5A &&
        buf[6] == 1 && std::memcmp(buf + 40, "CD-ROM", 6) == 0)
      return "3do";
  }
  if (d.readData(s0 + 1, buf) && std::memcmp(buf + 32, "PC Engine CD-ROM SYSTEM", 23) == 0) return "pcecd";
  bool psx = false;
  // ISO 9660: descritor primário no setor 16, diretório raiz no byte 156
  if (d.readData(s0 + 16, buf) && buf[0] == 1 && std::memcmp(buf + 1, "CD001", 5) == 0) {
    const uint8_t* r = buf + 156;
    const uint32_t ext = uint32_t(r[2]) | uint32_t(r[3]) << 8 | uint32_t(r[4]) << 16 | uint32_t(r[5]) << 24;
    const uint32_t size = uint32_t(r[10]) | uint32_t(r[11]) << 8 | uint32_t(r[12]) << 16 | uint32_t(r[13]) << 24;
    const uint32_t secs = std::min<uint32_t>((size + DATA_SECTOR - 1) / DATA_SECTOR, 16);
    for (uint32_t k = 0; k < secs; k++) {
      if (!d.readData(s0 + ext + k, buf)) break;
      for (int o = 0; o < DATA_SECTOR;) {
        const int len = buf[o];
        if (len == 0) break;
        if (o + 33 > DATA_SECTOR || o + len > DATA_SECTOR) break;
        const int nl = buf[o + 32];
        if (o + 33 + nl > DATA_SECTOR) break;
        std::string nm(reinterpret_cast<const char*>(buf + o + 33), size_t(nl));
        const std::string l = lower(nm);
        if (l.rfind("ipl.txt", 0) == 0) return "neogeocd";
        if (l.rfind("system.cnf", 0) == 0 || l.rfind("psx.exe", 0) == 0) psx = true;  // PlayStation
        o += len;
      }
    }
  }
  if (psx) return "ps1";
  // PlayStation sem os arquivos de partida na raiz: a licença da Sony no setor 4 (área do sistema)
  if (d.readRaw(s0 + 4, raw)) {
    static const char lic[] = "Sony Computer Entertainment";
    const uint8_t* b = raw;
    const uint8_t* e = raw + RAW_SECTOR;
    const uint8_t* p = std::search(b, e, lic, lic + sizeof lic - 1);
    if (p != e) {
      const char by[] = "Licensed";
      if (std::search(b, p, by, by + sizeof by - 1) != p) return "ps1";
    }
  }
  return "";
}

}  // namespace indigo::cdrom
