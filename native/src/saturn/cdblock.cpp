// Bloco de CD do Saturn (alto nível): os comandos que o SH-2 escreve em CR1-CR4, o HIRQ, a leitura do disco
// a 2x (150 setores/s) para os 200 buffers de setor, filtros e partições, transferência pela porta de dados,
// TOC, subcódigo Q, sistema de arquivos ISO 9660 (diretórios, informação e leitura de arquivo), autenticação e
// a saída de áudio das faixas de CD.
//
// O chip SH-1 do bloco não é emulado (o programa dele é da Sega); os comandos seguem a documentação pública e
// o comportamento descrito no Mednafen. Tempos: os comandos respondem na hora; MPEG não existe.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "saturn.h"

namespace indigo::saturn {

namespace {
enum : uint16_t {
  CMOK = 0x0001, DRDY = 0x0002, CSCT = 0x0004, BFUL = 0x0008, PEND = 0x0010, DCHG = 0x0020, ESEL = 0x0040,
  EHST = 0x0080, ECPY = 0x0100, EFLS = 0x0200, SCDQ = 0x0400, MPED = 0x0800
};
enum : uint8_t { ST_BUSY = 0, ST_PAUSE = 1, ST_STANDBY = 2, ST_PLAY = 3, ST_SEEK = 4, ST_NODISC = 7,
                 ST_PERIODIC = 0x20, ST_DTREQ = 0x40, ST_WAIT = 0x80, ST_REJECT = 0xFF };
constexpr int NUM_BUFFERS = 200;
constexpr int SECTOR_CYCLES = 179161;  // 26,87 MHz / 150 setores por segundo (2x)
constexpr size_t AUDIO_MAX = 588 * 2 * 16;  // fila do CD de áudio: até 16 setores
inline uint8_t bcd(int v) { return uint8_t((v / 10) << 4 | (v % 10)); }
inline int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
}  // namespace

void CdBlock::reset() {
  hirq_ = CMOK | DCHG | ESEL | EHST | ECPY | EFLS | MPED;
  mask_ = 0;
  std::memset(cr_, 0, sizeof cr_);
  // assinatura "CDBLOCK" que a BIOS procura ao ligar
  res_[0] = 0x0043; res_[1] = 0x4442; res_[2] = 0x4C4F; res_[3] = 0x434B;
  stat_ = disc_ ? ST_PAUSE : ST_NODISC;
  fad_ = 150;
  playStart_ = playEnd_ = 0;
  repeat_ = repeatMax_ = 0;
  playing_ = audioTrack_ = false;
  sectorClock_ = periodClock_ = 0;
  getSecLen_ = putSecLen_ = 2048;
  for (int i = 0; i < 24; i++) { filt_[i] = Filter{}; filt_[i].trueConn = uint8_t(i); }
  cdConn_ = lastDest_ = 0xFF;
  for (auto& p : part_) p.clear();
  dt_ = Xfer{};
  lastXfer_ = 0;
  dir_.clear();
  dirOffset_ = 2;
  fileRead_ = false;
  fileFad_ = fileEnd_ = 0;
  audioQ_.clear();
  audioPos_ = 0;
  authOk_ = false;
}

int CdBlock::freeBuffers() const {
  size_t used = 0;
  for (const auto& p : part_) used += p.size();
  return NUM_BUFFERS - int(used);
}

bool CdBlock::trackOf(uint32_t fad, int& tno, uint8_t& ctl) const {
  tno = 0xFF; ctl = 0xFF;
  if (!disc_ || fad < 150) return false;
  const int t = disc_->trackAt(fad - 150);
  if (t < 0) return false;
  const auto& tr = disc_->tracks()[size_t(t)];
  tno = tr.number;
  ctl = tr.type == cdrom::TrackType::Audio ? 0x01 : 0x41;
  return true;
}

uint32_t CdBlock::fadOfTrack(int t, bool end) const {
  if (!disc_) return 150;
  const auto& ts = disc_->tracks();
  for (size_t i = 0; i < ts.size(); i++) {
    if (ts[i].number != t) continue;
    return end ? ts[i].start + ts[i].length + 150 : ts[i].start + 150;
  }
  return end ? disc_->totalSectors() + 150 : 150;
}

void CdBlock::status(uint8_t extra, bool reject) {
  int tno; uint8_t ctl;
  const bool ok = trackOf(fad_, tno, ctl);
  const uint8_t st = reject ? ST_REJECT : uint8_t(stat_ | extra);
  res_[0] = uint16_t(st << 8 | ((ok && ctl == 0x41) ? 0x80 : 0) | (repeat_ & 0x7F));
  res_[1] = uint16_t(ctl << 8 | (tno & 0xFF));
  res_[2] = uint16_t((ok ? 1 : 0xFF) << 8 | ((fad_ >> 16) & 0xFF));
  res_[3] = uint16_t(fad_);
}

uint16_t CdBlock::read(uint32_t off) {
  switch (off & 0x3C) {
    case 0x08: return hirq_;
    case 0x0C: return mask_;
    case 0x18: return res_[0];
    case 0x1C: return res_[1];
    case 0x20: return res_[2];
    case 0x24: lastXfer_ |= 0x80000000u; return res_[3];  // resultado lido
    default: return 0;
  }
}

void CdBlock::write(uint32_t off, uint16_t v) {
  switch (off & 0x3C) {
    case 0x08: hirq_ &= v; break;
    case 0x0C: mask_ = v; break;
    case 0x18: cr_[0] = v; break;
    case 0x1C: cr_[1] = v; break;
    case 0x20: cr_[2] = v; break;
    case 0x24: cr_[3] = v; lastXfer_ &= ~0x80000000u; command(); break;
    default: break;
  }
}

uint16_t CdBlock::readData() {
  if (!dt_.active || dt_.pos >= dt_.data.size()) return 0;
  const uint16_t v = uint16_t(dt_.data[dt_.pos] << 8 | (dt_.pos + 1 < dt_.data.size() ? dt_.data[dt_.pos + 1] : 0));
  dt_.pos += 2;
  return v;
}

uint32_t CdBlock::readData32() {
  const uint32_t hi = readData();
  return hi << 16 | readData();
}

void CdBlock::startRawTransfer(const std::vector<uint8_t>& bytes) {
  dt_ = Xfer{};
  dt_.data = bytes;
  dt_.active = true;
}

void CdBlock::startTransfer(int part, int offset, int count, bool del) {
  dt_ = Xfer{};
  auto& p = part_[part];
  for (int i = 0; i < count; i++) {
    const auto& s = p[size_t(offset + i)];
    const bool mode2 = s[15] == 2;
    int from, len;
    switch (getSecLen_) {
      case 2336: from = 16; len = 2336; break;
      case 2340: from = 12; len = 2340; break;
      case 2352: from = 0; len = 2352; break;
      default: from = mode2 ? 24 : 16; len = 2048; break;
    }
    dt_.data.insert(dt_.data.end(), s.begin() + from, s.begin() + from + len);
  }
  if (del) p.erase(p.begin() + offset, p.begin() + offset + count);
  dt_.active = true;
  dt_.part = part;
}

void CdBlock::startPlay(uint32_t start, uint32_t end, int rep) {
  playStart_ = start;
  playEnd_ = end;
  fad_ = start;
  repeat_ = 0;
  repeatMax_ = rep;
  playing_ = disc_ != nullptr;
  stat_ = playing_ ? ST_PLAY : ST_NODISC;
  sectorClock_ = SECTOR_CYCLES;
  audioQ_.clear();
  audioPos_ = 0;
}

void CdBlock::readSector() {
  int tno; uint8_t ctl;
  if (!trackOf(fad_, tno, ctl)) { playing_ = false; stat_ = ST_PAUSE; irq(PEND); return; }
  uint8_t raw[cdrom::RAW_SECTOR];
  disc_->readRaw(fad_ - 150, raw);
  if (ctl == 0x01) {
    // faixa de áudio: 588 pares de amostras de 16 bits (little-endian)
    audioTrack_ = true;
    if (audioQ_.size() - audioPos_ < AUDIO_MAX)  // fila limitada: se ninguém consome, descarta
      for (int i = 0; i < 588 * 2; i++) audioQ_.push_back(int16_t(raw[i * 2] | raw[i * 2 + 1] << 8));
  } else {
    audioTrack_ = false;
    if (freeBuffers() <= 0) { irq(BFUL); return; }  // espera espaço (não avança)
    route(cdConn_, raw);
  }
  irq(SCDQ);
  fad_++;
  if (fad_ >= playEnd_) {
    if (repeatMax_ == 0xF || repeat_ < repeatMax_) {
      if (repeatMax_ != 0xF) repeat_++;
      fad_ = playStart_;
    } else {
      playing_ = false;
      stat_ = ST_PAUSE;
      irq(PEND);
      if (fileRead_) { fileRead_ = false; irq(EFLS); }
    }
  }
}

// passa um setor pelos filtros a partir de f (condições de FAD e do subcabeçalho do Mode 2) até uma partição
void CdBlock::route(uint8_t f, const uint8_t* raw) {
  {
    for (int guard = 0; f < 24 && guard < 24; guard++) {
      const Filter& fl = filt_[f];
      bool pass = true;
      if (fl.mode & 0x40) {
        const uint32_t sf = uint32_t(cdrom::msfToLba(unbcd(raw[12]), unbcd(raw[13]), unbcd(raw[14]))) + 150;
        pass = sf >= fl.fad && sf < fl.fad + fl.range;
      }
      if (pass && raw[15] == 2) {
        const bool inv = (fl.mode & 0x10) && (fl.mode & 0x0F);
        bool ok = true;
        if ((fl.mode & 1) && raw[16] != fl.file) ok = false;
        if ((fl.mode & 2) && raw[17] != fl.chan) ok = false;
        if ((fl.mode & 4) && (raw[18] & fl.smMask) != fl.sm) ok = false;
        if ((fl.mode & 8) && (raw[19] & fl.ciMask) != fl.ci) ok = false;
        pass = inv ? !ok : ok;
      }
      if (pass) {
        if (fl.trueConn < 24) {
          part_[fl.trueConn].emplace_back(raw, raw + cdrom::RAW_SECTOR);
          lastDest_ = fl.trueConn;
          irq(CSCT);
          if (freeBuffers() <= 0) irq(BFUL);
        }
        break;
      }
      f = fl.falseConn;
    }
  }
}

void CdBlock::run(int cycles) {
  if (playing_) {
    sectorClock_ -= cycles;
    while (playing_ && sectorClock_ <= 0) {
      readSector();
      // dados a 2x; faixa de áudio em tempo real (1x, 75 setores/s = 44.100 amostras/s)
      sectorClock_ += audioTrack_ ? 2 * SECTOR_CYCLES : SECTOR_CYCLES;
    }
  }
  // resposta periódica (a cada ~1/60 s) quando o programa já leu a última
  periodClock_ += cycles;
  if (periodClock_ >= 447900) {
    periodClock_ = 0;
    if (lastXfer_ & 0x80000000u) status(ST_PERIODIC);
  }
}

bool CdBlock::audioOut(int16_t& l, int16_t& r) {
  if (audioPos_ + 1 >= audioQ_.size()) {
    if (!audioQ_.empty()) { audioQ_.clear(); audioPos_ = 0; }
    return false;
  }
  l = audioQ_[audioPos_];
  r = audioQ_[audioPos_ + 1];
  audioPos_ += 2;
  if (audioPos_ > 8192) { audioQ_.erase(audioQ_.begin(), audioQ_.begin() + long(audioPos_)); audioPos_ = 0; }
  return true;
}

void CdBlock::dirRead(uint32_t fad, uint32_t size) {
  dir_.clear();
  uint8_t buf[cdrom::DATA_SECTOR];
  const uint32_t secs = std::min<uint32_t>((size + 2047) / 2048, 64);
  for (uint32_t k = 0; k < secs && disc_; k++) {
    if (!disc_->readData(fad - 150 + k, buf)) break;
    for (int o = 0; o < 2048;) {
      const int len = buf[o];
      if (!len) break;
      if (o + len > 2048 || len < 34) break;
      FileEnt e{};
      e.fad = le32(buf + o + 2) + 150;
      e.size = le32(buf + o + 10);
      e.attr = buf[o + 25];
      e.unit = buf[o + 26];
      e.gap = buf[o + 27];
      e.fileNo = 0;
      dir_.push_back(e);
      o += len;
    }
  }
}

void CdBlock::command() {
  const uint8_t cmd = uint8_t(cr_[0] >> 8);
  auto basic = [&](uint16_t a, uint16_t b, uint16_t c, uint16_t d) { res_[0] = a; res_[1] = b; res_[2] = c; res_[3] = d; };
  const uint16_t st8 = uint16_t(stat_ << 8);
  const uint8_t fnum = uint8_t(cr_[2] >> 8);
  switch (cmd) {
    case 0x00: status(); break;                                       // estado
    case 0x01: basic(st8, 0x0002, 0, 0x0600); break;                  // informação do hardware
    case 0x02: {                                                      // TOC (102 palavras de 32 bits)
      std::vector<uint8_t> toc(408, 0xFF);
      auto put = [&](int i, uint32_t v) { toc[size_t(i * 4)] = uint8_t(v >> 24); toc[size_t(i * 4 + 1)] = uint8_t(v >> 16); toc[size_t(i * 4 + 2)] = uint8_t(v >> 8); toc[size_t(i * 4 + 3)] = uint8_t(v); };
      int first = 99, last = 1;
      uint8_t firstCtl = 0x41, lastCtl = 0x41;
      if (disc_) {
        for (const auto& t : disc_->tracks()) {
          const uint8_t ctl = t.type == cdrom::TrackType::Audio ? 0x01 : 0x41;
          if (t.number >= 1 && t.number <= 99) put(t.number - 1, uint32_t(ctl) << 24 | (t.start + 150));
          if (t.number < first) { first = t.number; firstCtl = ctl; }
          if (t.number > last) { last = t.number; lastCtl = ctl; }
        }
        put(99, uint32_t(firstCtl) << 24 | uint32_t(first) << 16);
        put(100, uint32_t(lastCtl) << 24 | uint32_t(last) << 16);
        put(101, uint32_t(lastCtl) << 24 | (disc_->totalSectors() + 150));
      }
      startRawTransfer(toc);
      basic(uint16_t((stat_ | ST_DTREQ) << 8), 0xCC, 0, 0);
      irq(DRDY);
      break;
    }
    case 0x03: {                                                      // sessão
      const uint32_t lo = disc_ ? disc_->totalSectors() + 150 : 0;
      const int sess = cr_[0] & 0xFF;
      if (!sess) basic(st8, 0, uint16_t(0x01 << 8 | (lo >> 16)), uint16_t(lo));
      else if (sess == 1) basic(st8, 0, 0x0100, 0);
      else basic(st8, 0, 0xFFFF, 0xFFFF);
      break;
    }
    case 0x04:                                                        // inicializar
      if (cr_[0] & 1) {
        for (int i = 0; i < 24; i++) { filt_[i] = Filter{}; filt_[i].trueConn = uint8_t(i); }
        for (auto& p : part_) p.clear();
        cdConn_ = lastDest_ = 0xFF;
        getSecLen_ = putSecLen_ = 2048;
        dt_ = Xfer{};
      }
      playing_ = false;
      if (disc_) stat_ = ST_PAUSE;
      status();
      break;
    case 0x05: playing_ = false; stat_ = 6; status(); break;          // abrir a bandeja
    case 0x06: {                                                      // fim da transferência
      if (dt_.active) {
        const uint32_t words = uint32_t(dt_.pos / 2);
        basic(uint16_t(st8 | ((words >> 16) & 0xFF)), uint16_t(words), 0, 0);
        dt_.active = false;
        if (dt_.part >= 0) irq(EHST);
      } else {
        basic(uint16_t(st8 | 0xFF), 0xFFFF, 0, 0);
      }
      break;
    }
    case 0x10: {                                                      // tocar
      uint32_t ps = uint32_t(cr_[0] & 0xFF) << 16 | cr_[1];
      uint32_t pe = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      const int pm = cr_[2] >> 8;
      if (ps == 0xFFFFFF) ps = 0x800000 | fad_;
      uint32_t start, end;
      if (ps & 0x800000) start = ps & 0x7FFFFF;
      else start = fadOfTrack(int(ps >> 8) ? int(ps >> 8) : 1, false);
      if (pe == 0xFFFFFF) end = playEnd_ ? playEnd_ : (disc_ ? disc_->totalSectors() + 150 : start);
      else if (pe & 0x800000) end = start + (pe & 0x7FFFFF);
      else if (pe == 0) end = disc_ ? disc_->totalSectors() + 150 : start;
      else end = fadOfTrack(int(pe >> 8), true);
      startPlay(start, end, pm & 0xF);
      status();
      break;
    }
    case 0x11: {                                                      // buscar / pausar / parar
      const uint32_t sp = uint32_t(cr_[0] & 0xFF) << 16 | cr_[1];
      playing_ = false;
      if (!sp) stat_ = ST_STANDBY;
      else if (sp == 0xFFFFFF) stat_ = ST_PAUSE;
      else { fad_ = (sp & 0x800000) ? (sp & 0x7FFFFF) : fadOfTrack(int(sp >> 8), false); stat_ = ST_PAUSE; }
      status();
      break;
    }
    case 0x12: status(); break;                                       // varredura: não implementada
    case 0x20: {                                                      // subcódigo Q
      int tno; uint8_t ctl;
      trackOf(fad_, tno, ctl);
      uint32_t rel = 0;
      if (tno != 0xFF) rel = fad_ - fadOfTrack(tno, false);
      uint8_t am, as, af;
      cdrom::lbaToMsf(fad_ - 150, am, as, af);
      const int m = int(rel / 4500), sec = int(rel / 75 % 60), f = int(rel % 75);
      const uint8_t q[10] = {ctl, bcd(tno & 0x7F), 1, bcd(m), bcd(sec), bcd(f), 0, bcd(am), bcd(as), bcd(af)};
      std::vector<uint8_t> d(q, q + 10);
      startRawTransfer(d);
      basic(uint16_t((stat_ | ST_DTREQ) << 8), 5, 0, 0);
      irq(DRDY);
      break;
    }
    case 0x30:                                                        // conexão do CD
      if (fnum >= 24 && fnum != 0xFF) { status(0, true); break; }
      cdConn_ = fnum; status(); irq(ESEL); break;
    case 0x31: basic(st8, 0, uint16_t(cdConn_ << 8), 0); break;
    case 0x32: basic(st8, 0, uint16_t(lastDest_ << 8), 0); break;
    case 0x40:                                                        // faixa do filtro
      if (fnum >= 24) { status(0, true); break; }
      filt_[fnum].fad = uint32_t(cr_[0] & 0xFF) << 16 | cr_[1];
      filt_[fnum].range = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      status(); irq(ESEL); break;
    case 0x41:
      if (fnum >= 24) { status(0, true); break; }
      basic(uint16_t(st8 | (filt_[fnum].fad >> 16)), uint16_t(filt_[fnum].fad), uint16_t(fnum << 8 | (filt_[fnum].range >> 16)), uint16_t(filt_[fnum].range));
      break;
    case 0x42:                                                        // subcabeçalho do filtro
      if (fnum >= 24) { status(0, true); break; }
      filt_[fnum].chan = uint8_t(cr_[0]); filt_[fnum].smMask = uint8_t(cr_[1] >> 8); filt_[fnum].ciMask = uint8_t(cr_[1]);
      filt_[fnum].file = uint8_t(cr_[2]); filt_[fnum].sm = uint8_t(cr_[3] >> 8); filt_[fnum].ci = uint8_t(cr_[3]);
      status(); irq(ESEL); break;
    case 0x43:
      if (fnum >= 24) { status(0, true); break; }
      basic(uint16_t(st8 | filt_[fnum].chan), uint16_t(filt_[fnum].smMask << 8 | filt_[fnum].ciMask), uint16_t(fnum << 8 | filt_[fnum].file), uint16_t(filt_[fnum].sm << 8 | filt_[fnum].ci));
      break;
    case 0x44:                                                        // modo do filtro
      if (fnum >= 24) { status(0, true); break; }
      filt_[fnum].mode = uint8_t(cr_[0]);
      if (cr_[0] & 0x80) { const uint8_t t = filt_[fnum].trueConn, f = filt_[fnum].falseConn; filt_[fnum] = Filter{}; filt_[fnum].trueConn = t; filt_[fnum].falseConn = f; }
      status(); irq(ESEL); break;
    case 0x45:
      if (fnum >= 24) { status(0, true); break; }
      basic(uint16_t(st8 | filt_[fnum].mode), 0, uint16_t(fnum << 8), 0); break;
    case 0x46:                                                        // conexões do filtro
      if (fnum >= 24) { status(0, true); break; }
      if (cr_[0] & 1) filt_[fnum].trueConn = uint8_t(cr_[1] >> 8);
      if (cr_[0] & 2) filt_[fnum].falseConn = uint8_t(cr_[1]);
      status(); irq(ESEL); break;
    case 0x47:
      if (fnum >= 24) { status(0, true); break; }
      basic(st8, uint16_t(filt_[fnum].trueConn << 8 | filt_[fnum].falseConn), uint16_t(fnum << 8), 0); break;
    case 0x48: {                                                      // reiniciar seletor
      const int rf = cr_[0] & 0xFF;
      if (!rf) {
        if (fnum >= 24) { status(0, true); break; }
        part_[fnum].clear();
      } else {
        if (rf & 0x04) for (auto& p : part_) p.clear();
        if (rf & 0x08) for (int i = 0; i < 24; i++) { const uint8_t t = filt_[i].trueConn, f = filt_[i].falseConn; filt_[i] = Filter{}; filt_[i].trueConn = t; filt_[i].falseConn = f; }
        if (rf & 0x10) for (int i = 0; i < 24; i++) filt_[i].trueConn = uint8_t(i);
        if (rf & 0x20) for (int i = 0; i < 24; i++) filt_[i].falseConn = 0xFF;
        if (rf & 0x40) cdConn_ = 0xFF;
      }
      status(); irq(ESEL); break;
    }
    case 0x50: basic(st8, uint16_t(freeBuffers()), 0x18 << 8, NUM_BUFFERS); break;
    case 0x51:
      if (fnum >= 24) { status(0, true); break; }
      basic(st8, 0, 0, uint16_t(part_[fnum].size())); break;
    case 0x52: {                                                      // calcular tamanho real
      if (fnum >= 24) { status(0, true); break; }
      int offs = cr_[1], n = cr_[3];
      const int cnt = int(part_[fnum].size());
      if (offs == 0xFFFF) offs = cnt - 1;
      if (n == 0xFFFF) n = cnt - offs;
      if (n <= 0 || offs < 0 || offs + n > cnt) { status(ST_WAIT); break; }
      uint32_t acc = 0;
      for (int i = 0; i < n; i++) acc += getSecLen_ == 2048 ? 1024 : uint32_t(getSecLen_ / 2);
      lastXfer_ = (lastXfer_ & 0x80000000u) | (acc & 0x7FFFFFFF);
      status(); irq(ESEL); break;
    }
    case 0x53: { const uint32_t a = lastXfer_ & 0x7FFFFFFF; basic(uint16_t(st8 | ((a >> 16) & 0xFF)), uint16_t(a), 0, 0); break; }
    case 0x54: {                                                      // informação do setor
      if (fnum >= 24 || part_[fnum].empty()) { status(0, true); break; }
      int offs = cr_[1];
      if (offs == 0xFFFF) offs = int(part_[fnum].size()) - 1;
      if (offs >= int(part_[fnum].size())) { status(0, true); break; }
      const auto& s = part_[fnum][size_t(offs)];
      const uint32_t f = uint32_t(cdrom::msfToLba(unbcd(s[12]), unbcd(s[13]), unbcd(s[14]))) + 150;
      const bool m2 = s[15] == 2;
      basic(uint16_t(st8 | (f >> 16)), uint16_t(f), uint16_t((m2 ? s[16] : 0) << 8 | (m2 ? s[17] : 0)), uint16_t((m2 ? s[18] : 0) << 8 | (m2 ? s[19] : 0)));
      break;
    }
    case 0x60: {                                                      // tamanho do setor
      static const int LEN[4] = {2048, 2336, 2340, 2352};
      const int g = cr_[0] & 0xFF, p = cr_[1] >> 8;
      if ((g != 0xFF && g > 3) || (p != 0xFF && p > 3)) { status(0, true); break; }
      if (g != 0xFF) getSecLen_ = LEN[g];
      if (p != 0xFF) putSecLen_ = LEN[p];
      status(); irq(ESEL); break;
    }
    case 0x61: case 0x62: case 0x63: {                                // pegar / apagar setores
      if (fnum >= 24) { status(0, true); break; }
      int offs = cr_[1], n = cr_[3];
      const int cnt = int(part_[fnum].size());
      if (offs == 0xFFFF) offs = cnt - 1;
      if (n == 0xFFFF) n = cnt - offs;
      if ((dt_.active && cmd != 0x62) || n <= 0 || offs < 0 || offs + n > cnt) { status(ST_WAIT); break; }
      if (cmd == 0x62) {
        part_[fnum].erase(part_[fnum].begin() + offs, part_[fnum].begin() + offs + n);
        status();
        irq(EHST);
      } else {
        startTransfer(fnum, offs, n, cmd == 0x63);
        status(ST_DTREQ);
        irq(DRDY);
      }
      break;
    }
    case 0x65: case 0x66: {                                           // copiar / mover setores para um filtro
      const uint8_t dst = uint8_t(cr_[0]);
      if (fnum >= 24 || dst >= 24) { status(0, true); break; }
      int offs = cr_[1], n = cr_[3];
      const int cnt = int(part_[fnum].size());
      if (offs == 0xFFFF) offs = cnt - 1;
      if (n == 0xFFFF) n = cnt - offs;
      if (dt_.active || n <= 0 || offs < 0 || offs + n > cnt || (cmd == 0x65 && n > freeBuffers())) { status(ST_WAIT); break; }
      std::vector<std::vector<uint8_t>> moved(part_[fnum].begin() + offs, part_[fnum].begin() + offs + n);
      if (cmd == 0x66) part_[fnum].erase(part_[fnum].begin() + offs, part_[fnum].begin() + offs + n);
      for (const auto& sct : moved) route(dst, sct.data());
      status();
      if (freeBuffers() <= 0) irq(BFUL);
      irq(ECPY);
      break;
    }
    case 0x67: basic(st8, 0, 0, 0); break;                            // erro de cópia: nenhum
    case 0x70: case 0x71: {                                           // mudar / ler diretório
      if (fnum >= 24 || !disc_) { status(0, true); break; }
      const uint32_t fid = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      if (cmd == 0x70) {
        if (fid == 0xFFFFFF || dir_.empty()) {
          uint8_t pvd[cdrom::DATA_SECTOR];
          if (!disc_->readData(16, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) { status(0, true); break; }
          dirRead(le32(pvd + 156 + 2) + 150, le32(pvd + 156 + 10));
        } else if (fid < dir_.size() && (dir_[fid].attr & 2)) {
          const FileEnt e = dir_[fid];
          dirRead(e.fad, e.size);
        } else { status(0, true); break; }
        dirOffset_ = 2;
      } else {
        dirOffset_ = std::max<uint32_t>(2, fid);
      }
      status();
      irq(EFLS);
      break;
    }
    case 0x72: {                                                      // escopo do sistema de arquivos
      if (dir_.empty()) { status(0, true); break; }
      const uint32_t total = uint32_t(dir_.size());
      const uint32_t cnt = total > dirOffset_ ? std::min<uint32_t>(254, total - dirOffset_) : 0;
      const bool more = dirOffset_ + cnt < total;
      basic(st8, uint16_t(cnt), uint16_t((!more ? 1 : 0) << 8 | (dirOffset_ >> 16)), uint16_t(dirOffset_));
      break;
    }
    case 0x73: {                                                      // informação do arquivo (12 bytes cada)
      const uint32_t fid = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      if (dir_.empty() || dt_.active) { status(dt_.active ? ST_WAIT : 0, !dt_.active); break; }
      std::vector<uint8_t> d;
      auto add = [&](const FileEnt& e) {
        const uint8_t b[12] = {uint8_t(e.fad >> 24), uint8_t(e.fad >> 16), uint8_t(e.fad >> 8), uint8_t(e.fad),
                               uint8_t(e.size >> 24), uint8_t(e.size >> 16), uint8_t(e.size >> 8), uint8_t(e.size),
                               e.unit, e.gap, e.fileNo, e.attr};
        d.insert(d.end(), b, b + 12);
      };
      if (fid == 0xFFFFFF) {
        for (uint32_t i = dirOffset_; i < dir_.size() && i < dirOffset_ + 254; i++) add(dir_[i]);
      } else if (fid < dir_.size()) {
        add(dir_[fid]);
      } else { status(0, true); break; }
      startRawTransfer(d);
      basic(uint16_t((stat_ | ST_DTREQ) << 8), uint16_t(d.size() / 2), 0, 0);
      irq(DRDY);
      break;
    }
    case 0x74: {                                                      // ler arquivo para a partição fnum
      const uint32_t off = uint32_t(cr_[0] & 0xFF) << 16 | cr_[1];
      const uint32_t fid = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      if (fnum >= 24 || fid >= dir_.size()) { status(0, true); break; }
      const FileEnt& e = dir_[fid];
      const uint32_t start = e.fad + off, n = (e.size + 2047) / 2048 - std::min(off, (e.size + 2047) / 2048);
      part_[fnum].clear();
      cdConn_ = fnum;
      filt_[fnum] = Filter{};
      filt_[fnum].trueConn = fnum;
      filt_[fnum].falseConn = 0xFF;
      filt_[fnum].fad = start;
      filt_[fnum].range = n;
      filt_[fnum].mode = 0x40;
      startPlay(start, start + n, 0);
      fileRead_ = true;
      status();
      break;
    }
    case 0x75: fileRead_ = false; playing_ = false; stat_ = ST_PAUSE; status(); irq(EFLS); break;
    case 0xE0: authOk_ = disc_ != nullptr; status(); irq(EFLS | CSCT); break;   // autenticar o disco
    case 0xE1: basic(st8, uint16_t(authOk_ ? 4 : 0), 0, 0); break;
    default: status(0, true); break;
  }
  irq(CMOK);
}

void CdBlock::sync(StateIO& io) {
  io.sync(hirq_); io.sync(mask_); io.raw(cr_, sizeof cr_); io.raw(res_, sizeof res_);
  io.sync(stat_); io.sync(fad_); io.sync(playStart_); io.sync(playEnd_);
  io.sync(repeat_); io.sync(repeatMax_); io.sync(playing_); io.sync(audioTrack_);
  io.sync(sectorClock_); io.sync(periodClock_); io.sync(getSecLen_); io.sync(putSecLen_);
  for (auto& f : filt_) {
    io.sync(f.fad); io.sync(f.range); io.sync(f.mode); io.sync(f.trueConn); io.sync(f.falseConn);
    io.sync(f.file); io.sync(f.chan); io.sync(f.sm); io.sync(f.smMask); io.sync(f.ci); io.sync(f.ciMask);
  }
  io.sync(cdConn_); io.sync(lastDest_);
  uint32_t total = 0;
  for (auto& p : part_) {
    uint32_t n = uint32_t(p.size());
    io.sync(n);
    total += n;
    if (io.reading()) {
      if (n > NUM_BUFFERS || total > NUM_BUFFERS) throw std::runtime_error("Estado salvo inválido (CD).");
      p.assign(n, std::vector<uint8_t>(cdrom::RAW_SECTOR));
    }
    for (auto& s : p) io.raw(s.data(), cdrom::RAW_SECTOR);
  }
  {
    uint32_t n = uint32_t(dt_.data.size());
    io.sync(n);
    if (io.reading()) { if (n > NUM_BUFFERS * cdrom::RAW_SECTOR) throw std::runtime_error("Estado salvo inválido (CD)."); dt_.data.assign(n, 0); }
    if (n) io.raw(dt_.data.data(), n);
    uint64_t pos = dt_.pos;
    io.sync(pos);
    dt_.pos = size_t(std::min<uint64_t>(pos, n));
    io.sync(dt_.active); io.sync(dt_.part);
  }
  io.sync(lastXfer_);
  {
    uint32_t n = uint32_t(dir_.size());
    io.sync(n);
    if (io.reading()) { if (n > 100000) throw std::runtime_error("Estado salvo inválido (CD)."); dir_.assign(n, FileEnt{}); }
    for (auto& e : dir_) { io.sync(e.fad); io.sync(e.size); io.sync(e.attr); io.sync(e.unit); io.sync(e.gap); io.sync(e.fileNo); }
  }
  io.sync(dirOffset_); io.sync(fileRead_); io.sync(fileFad_); io.sync(fileEnd_); io.sync(authOk_);
  {  // fila do CD de áudio (só o que ainda não tocou)
    uint32_t n = uint32_t(audioQ_.size() - std::min(audioPos_, audioQ_.size()));
    io.sync(n);
    if (io.reading()) {
      if (n > AUDIO_MAX + 588 * 2 || (n & 1)) throw std::runtime_error("Estado salvo inválido (CD).");
      audioQ_.assign(n, 0);
      audioPos_ = 0;
    }
    if (n) io.raw(audioQ_.data() + (audioQ_.size() - n), n * sizeof(int16_t));
  }
  if (io.reading()) {
    sectorClock_ = std::clamp(sectorClock_, -2 * SECTOR_CYCLES, 2 * SECTOR_CYCLES);
    periodClock_ = std::clamp(periodClock_, 0, 447900);
  }
}

}  // namespace indigo::saturn
