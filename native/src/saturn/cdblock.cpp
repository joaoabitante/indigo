// Bloco de CD do Saturn (alto nível): os comandos que o SH-2 escreve em CR1-CR4, o HIRQ, a unidade de CD com
// os tempos de busca e de leitura (2x para dados, 1x para áudio), o estado periódico, a varredura (SCAN), os 200
// buffers de setor com filtros e partições, a busca por FAD, a transferência pela porta de dados nos dois
// sentidos, TOC, subcódigo Q, sistema de arquivos ISO 9660 (diretórios, informação e leitura de arquivo),
// autenticação e a saída de áudio das faixas de CD.
//
// O chip SH-1 do bloco não é emulado (o programa dele é da Sega). Os comandos, os tempos e a máquina de estados
// da unidade seguem a documentação pública e o comportamento descrito no Mednafen (GPL-2.0 ou posterior; ver
// TERCEIROS.md), reescritos aqui. O relógio do bloco é 11,2896 MHz; os tempos abaixo já estão em ciclos do SH-2.
// MPEG não existe.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "saturn.h"

namespace indigo::saturn {

namespace {
enum : uint16_t {
  CMOK = 0x0001, DRDY = 0x0002, CSCT = 0x0004, BFUL = 0x0008, PEND = 0x0010, DCHG = 0x0020, ESEL = 0x0040,
  EHST = 0x0080, ECPY = 0x0100, EFLS = 0x0200, SCDQ = 0x0400, MPED = 0x0800
};
enum : uint8_t { ST_BUSY = 0, ST_PAUSE = 1, ST_STANDBY = 2, ST_PLAY = 3, ST_SEEK = 4, ST_SCAN = 5, ST_OPEN = 6,
                 ST_NODISC = 7, ST_PERIODIC = 0x20, ST_DTREQ = 0x40, ST_WAIT = 0x80, ST_REJECT = 0xFF };
constexpr int NUM_BUFFERS = 200;
// relógio do bloco de CD (11,2896 MHz) para ciclos do SH-2 (26,87 MHz)
constexpr int32_t clk(int64_t n) { return int32_t(n * 26874100 / 11289600); }
constexpr int32_t SECTOR_2X = clk(44100 * 256 / 150);  // 1/150 s
constexpr int32_t PERIOD = clk(187065);                // estado periódico parado (~16,6 ms)
constexpr int32_t AFTER_SECTOR = clk(17712);           // estado logo depois de cada setor lido
constexpr int32_t SPIN = clk(256000);                  // preparar a busca (~22,7 ms)
constexpr int32_t TO_STANDBY = clk(380000);            // parar o motor
constexpr int32_t STARTUP = clk(44100 * 256);          // ligar com disco: 1 s ocupado
constexpr int32_t CMD_BASE = clk(84);                  // resposta de um comando simples
constexpr size_t AUDIO_MAX = 588 * 2 * 16;             // fila do CD de áudio: até 16 setores
inline uint8_t bcd(int v) { return uint8_t((v / 10) << 4 | (v % 10)); }
inline int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
inline uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint32_t headerFad(const uint8_t* s) { return uint32_t(cdrom::msfToLba(unbcd(s[12]), unbcd(s[13]), unbcd(s[14]))) + 150; }
// bytes que GET SECTOR DATA entrega de um setor bruto
inline void secSpan(const uint8_t* s, int len, int& from, int& n) {
  switch (len) {
    case 2336: from = 16; n = 2336; break;
    case 2340: from = 12; n = 2340; break;
    case 2352: from = 0; n = 2352; break;
    default:
      if (s[15] == 1) { from = 16; n = 2048; }          // Mode 1 (os outros modos são lidos como Mode 2)
      else if (s[18] & 0x20) { from = 24; n = 2324; }   // Mode 2 Form 2
      else { from = 24; n = 2048; }
      break;
  }
}
}  // namespace

void CdBlock::softReset() {
  for (int i = 0; i < 24; i++) { filt_[i] = Filter{}; filt_[i].trueConn = uint8_t(i); }
  for (auto& p : part_) p.clear();
  cdConn_ = lastDest_ = 0xFF;
  getSecLen_ = putSecLen_ = 2048;
  dt_ = Xfer{};
  lastXfer_ &= 0x80000000u;
  srchPos_ = 0; srchPart_ = 0; srchFad_ = 0;
  endIrq_ = 0;
  playEnd_ = 0;
  repeatMax_ = 0;
  held_ = false;
  fsOp_ = 0;
  dir_.clear();
  dirOffset_ = 2;
  curDirFad_ = curDirSize_ = 0;
}

void CdBlock::reset() {
  hirq_ = CMOK | DCHG | ESEL | EHST | ECPY | EFLS | MPED;
  mask_ = 0;
  std::memset(cr_, 0, sizeof cr_);
  // assinatura "CDBLOCK" que a BIOS procura ao ligar
  res_[0] = 0x0043; res_[1] = 0x4442; res_[2] = 0x4C4F; res_[3] = 0x434B;
  cmdPending_ = false;
  cmdClock_ = 0;
  softReset();
  // com disco: o motor liga (ocupado ~1 s) e a cabeça vai para o começo (FAD 150) em pausa
  drive_ = disc_ ? F_INICIO : F_PARADO;
  stat_ = disc_ ? ST_BUSY : ST_NODISC;
  driveClock_ = disc_ ? STARTUP : (1 << 30);
  periodClock_ = PERIOD;
  fad_ = cur_ = 150;
  playStart_ = playEnd_ = 0;
  playCmdStart_ = playCmdEnd_ = 0;
  playCmdRep_ = 0;
  repeat_ = repeatMax_ = 0;
  secDone_ = pauseFull_ = false;
  scan_ = -1;
  scanCount_ = 0;
  std::memset(subq_, 0, sizeof subq_);
  lastXfer_ = 0x80000000u;  // a assinatura conta como lida: o estado periódico começa sozinho
  authType_ = 0;
  audioQ_.clear();
  audioPos_ = 0;
}

int CdBlock::freeBuffers() const {
  size_t used = 0;
  for (const auto& p : part_) used += p.size();
  if (dt_.active && dt_.writing) used += size_t(dt_.count);   // PUT em andamento: os buffers já estão reservados
  return NUM_BUFFERS - int(used);
}

bool CdBlock::trackOf(uint32_t fad, int& tno, uint8_t& ctl) const {
  tno = 0xFF; ctl = 0xFF;
  if (!disc_ || fad < 150 || fad >= 0x800000) return false;
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
  if (ts.empty()) return 150;
  int first = 99, last = 1;
  for (const auto& tr : ts) { first = std::min(first, tr.number); last = std::max(last, tr.number); }
  t = std::clamp(t, first, last);
  for (size_t i = 0; i < ts.size(); i++) {
    if (ts[i].number != t) continue;
    return end ? ts[i].start + ts[i].length + 150 : ts[i].start + 150;
  }
  return end ? disc_->totalSectors() + 150 : 150;
}

int32_t CdBlock::sectorTime(uint32_t fad) const {
  int tno; uint8_t ctl;
  return (trackOf(fad, tno, ctl) && ctl == 0x01) ? 2 * SECTOR_2X : SECTOR_2X;  // áudio a 1x
}

void CdBlock::status(uint8_t extra, bool reject) {
  int tno; uint8_t ctl;
  const bool ok = trackOf(fad_, tno, ctl);
  const uint8_t st = reject ? uint8_t(ST_REJECT) : uint8_t(stat_ | extra);
  const int rep = drive_ == F_PARADO ? 0x7F : (repeat_ & 0x0F);
  res_[0] = uint16_t(st << 8 | ((ok && ctl == 0x41) ? 0x80 : 0) | rep);
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
    case 0x24: lastXfer_ |= 0x80000000u; return res_[3];  // resultado lido: o estado periódico volta
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
    case 0x24:
      cr_[3] = v;
      cmdPending_ = true;
      cmdClock_ = commandDelay(uint8_t(cr_[0] >> 8));
      break;
    default: break;
  }
}

// tempo até o CMOK (o SH-1 do bloco processa o comando)
int CdBlock::commandDelay(uint8_t cmd) const {
  switch (cmd) {
    case 0x02: case 0x20: return CMD_BASE + clk(128);
    case 0x04: return CMD_BASE + clk(180);
    case 0x48: case 0x52: case 0x55: case 0x64: case 0x70: case 0x71: case 0x74: return CMD_BASE + clk(300);
    default: return CMD_BASE;
  }
}

uint16_t CdBlock::readData() {
  if (!dt_.active || dt_.writing || dt_.pos >= dt_.data.size()) return 0;
  const uint16_t v = uint16_t(dt_.data[dt_.pos] << 8 | (dt_.pos + 1 < dt_.data.size() ? dt_.data[dt_.pos + 1] : 0));
  dt_.pos += 2;
  return v;
}

uint32_t CdBlock::readData32() {
  const uint32_t hi = readData();
  return hi << 16 | readData();
}

void CdBlock::writeData(uint16_t v) {
  if (!dt_.active || !dt_.writing || dt_.pos + 1 >= dt_.data.size()) return;
  dt_.data[dt_.pos] = uint8_t(v >> 8);
  dt_.data[dt_.pos + 1] = uint8_t(v);
  dt_.pos += 2;
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
    int from, len;
    secSpan(s.data(), getSecLen_, from, len);
    dt_.data.insert(dt_.data.end(), s.begin() + from, s.begin() + from + len);
  }
  if (del) p.erase(p.begin() + offset, p.begin() + offset + count);
  dt_.active = true;
  dt_.part = part;
}

// ------------------------------------------------------------------------------------------------ unidade

// começa uma busca (PLAY, SEEK, ler arquivo): ocupado, buscando e então lendo a partir de start
void CdBlock::startSeek(uint32_t start, uint32_t end, int rep, uint16_t endIrq, bool keepPickup) {
  if (!disc_) return;
  repeat_ = 0;
  if (!keepPickup) held_ = false;
  playStart_ = start;
  playEnd_ = end;
  repeatMax_ = rep;
  endIrq_ = endIrq;
  if (keepPickup && drive_ == F_TOCA && scan_ < 0) return;   // continua lendo de onde está
  scan_ = -1;
  pauseFull_ = false;
  stat_ = ST_BUSY;
  drive_ = F_GIRO;
  driveClock_ = SPIN;
  periodClock_ = PERIOD;
  fad_ = start;
}

// o setor atual já passou do fim pedido (ou saiu do disco)
bool CdBlock::endMet() const {
  int tno; uint8_t ctl;
  if (!trackOf(fad_, tno, ctl)) return true;
  return fad_ >= playEnd_ || fad_ < playStart_;
}

// entrega o setor lido (cur_): dados vão para os filtros, áudio para a fila de som; false = sem buffer livre
bool CdBlock::processSector() {
  int tno; uint8_t ctl;
  if (!trackOf(cur_, tno, ctl)) return true;
  uint8_t raw[cdrom::RAW_SECTOR];
  if (!disc_->readRaw(cur_ - 150, raw)) std::memset(raw, 0, sizeof raw);
  if (ctl == 0x01) {
    // faixa de áudio: 588 pares de amostras de 16 bits (little-endian); na varredura sai a 1/4 do volume
    const int sh = scan_ >= 0 ? 2 : 0;
    if (audioQ_.size() - audioPos_ < AUDIO_MAX)  // fila limitada: se ninguém consome, descarta
      for (int i = 0; i < 588 * 2; i++) audioQ_.push_back(int16_t(int16_t(raw[i * 2] | raw[i * 2 + 1] << 8) >> sh));
    return true;
  }
  if (fsOp_ == 2) return true;          // autenticando: o bloco só confere o setor, não precisa de buffer
  if (freeBuffers() <= 0) return false;
  if (authType_) route(cdConn_, raw);   // antes da autenticação o bloco não guarda setores de dados
  irq(CSCT);
  if (freeBuffers() <= 0) irq(BFUL);
  return true;
}

void CdBlock::driveStep() {
  switch (drive_) {
    case F_PARADO:
      if (disc_) stat_ = ST_STANDBY;
      driveClock_ += 1 << 30;
      break;
    case F_INICIO:  // motor ligado: vai para o começo do disco e pausa
      startSeek(150, 0, 0, 0, false);
      break;
    case F_GIRO: {  // tempo de busca: base + distância (para trás é mais lento) + ajuste longo
      const int64_t d = int64_t(fad_) - int64_t(cur_);
      int64_t t = 12 * int64_t(SECTOR_2X) + clk(std::llabs(d) * (d < 0 ? 28 : 26));
      if (d < 0 || d >= 150) t += SECTOR_2X;
      stat_ = ST_SEEK;
      drive_ = F_BUSCA;
      driveClock_ += int32_t(std::min<int64_t>(t, 1 << 30));
      cur_ = fad_;
      break;
    }
    case F_BUSCA:
      drive_ = F_TOCA;
      held_ = false;
      secDone_ = false;
      driveClock_ += sectorTime(cur_);
      break;
    case F_TOCA:
      if (held_ && !endMet()) {
        if (processSector()) {
          held_ = false;
          secDone_ = true;
          if (scan_ >= 0) {
            int tno; uint8_t ctl;
            const bool data = trackOf(cur_, tno, ctl) && ctl == 0x41;
            scanCount_ = uint8_t(scanCount_ + (data ? 1 : 2));
            if (scanCount_ >= 6) {  // a varredura pula ~100 setores a cada 6 lidos
              scanCount_ = 0;
              if (!scan_) cur_ += 102 + uint32_t((uint64_t(1773936) * cur_ + (1ull << 31)) >> 32);
              else cur_ -= std::min<uint32_t>(cur_, 104 + uint32_t((uint64_t(2180000) * cur_ + (1ull << 31)) >> 32));
            } else {
              cur_++;
            }
          } else {
            cur_++;
          }
        }
      }
      if (!held_) { held_ = true; fad_ = cur_; }
      periodClock_ = AFTER_SECTOR;
      driveClock_ += sectorTime(cur_);
      break;
    case F_PAUSA:
      fad_ = cur_;
      periodClock_ = std::min(periodClock_, AFTER_SECTOR);
      driveClock_ += sectorTime(cur_);
      break;
    default:
      drive_ = F_PARADO;
      driveClock_ += 1 << 30;
      break;
  }
}

// estado periódico: fim da leitura, buffer cheio, repetição, relatório em CR1-CR4 e o subcódigo Q
void CdBlock::periodic() {
  periodClock_ = PERIOD;
  if (drive_ == F_TOCA && held_) {
    if (endMet()) {
      held_ = false;
      if (repeat_ < repeatMax_ && fsOp_ == 0) {   // repete (0xF = sem fim: o contador para em 0xE)
        repeat_ = std::min(repeat_ + 1, 0xE);
        const int r = repeat_;
        startSeek(playStart_, playEnd_, repeatMax_, endIrq_, false);
        repeat_ = r;
      } else {
        drive_ = F_PAUSA;
        stat_ = ST_PAUSE;
        pauseFull_ = false;
        cur_ = fad_;
        if (endIrq_) irq(endIrq_);
        endIrq_ = 0;
        if (fsOp_) finishFs();
      }
    } else if (freeBuffers() <= 0 && sectorTime(fad_) == SECTOR_2X && fsOp_ != 2) {
      held_ = false;              // sem buffer: pausa e volta quando houver espaço
      drive_ = F_PAUSA;
      stat_ = ST_PAUSE;
      pauseFull_ = true;
      cur_ = fad_;
    } else if (secDone_) {
      stat_ = scan_ >= 0 ? ST_SCAN : ST_PLAY;
      secDone_ = false;
    }
  } else if (drive_ == F_PAUSA) {
    stat_ = ST_PAUSE;
    if (pauseFull_ && freeBuffers() > 0) {   // há espaço: busca de novo o mesmo setor e continua
      const uint32_t s = playStart_, e = playEnd_;
      const int r = repeat_, m = repeatMax_;
      const uint16_t q = endIrq_;
      const int8_t sc = scan_;
      startSeek(cur_, e, m, q, false);
      playStart_ = s;
      repeat_ = r;
      scan_ = sc;
    }
  }
  if (lastXfer_ & 0x80000000u) {
    if (fsOp_ == 2) { res_[0] = uint16_t(ST_PERIODIC << 8 | 0xFF); res_[1] = res_[2] = res_[3] = 0xFFFF; }
    else status(ST_PERIODIC);
  }
  int tno; uint8_t ctl;
  const bool ok = trackOf(fad_, tno, ctl);
  const uint32_t rel = ok ? fad_ - fadOfTrack(tno, false) : 0xFFFFFF;
  const uint8_t q[10] = {ctl, uint8_t(tno), uint8_t(ok ? 1 : 0xFF), uint8_t(rel >> 16), uint8_t(rel >> 8), uint8_t(rel), 0,
                         uint8_t(fad_ >> 16), uint8_t(fad_ >> 8), uint8_t(fad_)};
  std::memcpy(subq_, q, sizeof q);
  irq(SCDQ);
}

// fim de uma operação do sistema de arquivos (os setores foram lidos do disco)
void CdBlock::finishFs() {
  if (fsOp_ == 1) {
    dirRead(fsFad_, fsSize_);
    curDirFad_ = fsFad_;
    curDirSize_ = fsSize_;
    dirOffset_ = std::max<uint32_t>(2, fsOffset_);
  } else if (fsOp_ == 2) {
    uint8_t s[cdrom::DATA_SECTOR];
    authType_ = (disc_ && disc_->readData(0, s) && std::memcmp(s, "SEGA SEGASATURN ", 16) == 0) ? 4 : 2;
    cdConn_ = 0xFF;
  }
  part_[fsPart_].clear();
  fsOp_ = 0;
  irq(EFLS);
}

void CdBlock::run(int cycles) {
  if (cmdPending_) {
    cmdClock_ -= cycles;
    if (cmdClock_ <= 0) { cmdPending_ = false; cmdClock_ = 0; command(); }
  }
  driveClock_ -= cycles;
  for (int guard = 0; driveClock_ <= 0 && guard < 64; guard++) driveStep();
  if (driveClock_ <= 0) driveClock_ = 1;
  periodClock_ -= cycles;
  if (periodClock_ <= 0) periodic();
}

// passa um setor pelos filtros a partir de f (condições de FAD e do subcabeçalho do Mode 2) até uma partição
void CdBlock::route(uint8_t f, const uint8_t* raw) {
  for (int guard = 0; f < 24 && guard < 24; guard++) {
    const Filter& fl = filt_[f];
    bool pass = true;
    if (fl.mode & 0x40) {
      const uint32_t sf = headerFad(raw);
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
      }
      break;
    }
    f = fl.falseConn;
  }
}

// desliga as entradas do filtro f (a conexão do CD e as conexões falsas que apontam para ele)
void CdBlock::disconnectInput(int f) {
  if (cdConn_ == f) cdConn_ = 0xFF;
  for (auto& fl : filt_) if (fl.falseConn == f) fl.falseConn = 0xFF;
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
  lastXfer_ &= ~0x80000000u;  // resultado novo, ainda não lido
  irq(CMOK);
  // autenticando: tudo responde 0xFFFF, menos o INIT com reinício (cancela a autenticação)
  if (fsOp_ == 2 && !(cmd == 0x04 && (cr_[0] & 1))) { basic(0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF); return; }
  switch (cmd) {
    case 0x00: status(); break;                                       // estado
    case 0x01: basic(st8, 0x0002, 0, 0x0600); break;                  // informação do hardware
    case 0x02: {                                                      // TOC (102 palavras de 32 bits)
      if (drive_ == F_INICIO || dt_.active) { status(ST_WAIT); break; }
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
      if (drive_ == F_INICIO) { status(ST_WAIT); break; }
      const uint32_t lo = disc_ ? disc_->totalSectors() + 150 : 0;
      const int sess = cr_[0] & 0xFF;
      if (!sess) basic(st8, 0, uint16_t(0x01 << 8 | (lo >> 16)), uint16_t(lo));
      else if (sess == 1) basic(st8, 0, 0x0100, 0);
      else basic(st8, 0, 0xFFFF, 0xFFFF);
      break;
    }
    case 0x04:                                                        // inicializar
      held_ = false;
      playEnd_ = 0;            // a leitura em andamento pausa no próximo estado periódico
      repeatMax_ = 0;
      if (disc_ && drive_ != F_PARADO) stat_ = ST_BUSY;
      status();
      if (cr_[0] & 1) {
        if (fsOp_ == 2) authType_ = 0;   // autenticação cancelada: o disco fica sem autenticar
        softReset();
        irq(MPED | EFLS | ECPY | EHST | ESEL);
      }
      break;
    case 0x05:                                                        // abrir a bandeja
      held_ = false; drive_ = F_PARADO; driveClock_ = 1 << 30; stat_ = ST_OPEN; authType_ = 0;
      status(); irq(DCHG | EFLS); break;
    case 0x06: {                                                      // fim da transferência
      if (dt_.active) {
        const uint32_t words = uint32_t(dt_.pos / 2);
        basic(uint16_t(st8 | ((words >> 16) & 0xFF)), uint16_t(words), 0, 0);
        dt_.active = false;
        if (dt_.writing) {      // PUT SECTOR DATA: os setores escritos vão direto para a partição (buffers reservados)
          const size_t len = dt_.data.size() / size_t(dt_.count);
          for (int i = 0; i < dt_.count; i++) {
            std::vector<uint8_t> raw(cdrom::RAW_SECTOR, 0);
            std::memcpy(raw.data() + dt_.offset, &dt_.data[size_t(i) * len], len);
            part_[dt_.part].push_back(std::move(raw));
          }
          lastDest_ = uint8_t(dt_.part);
          if (freeBuffers() <= 0) irq(BFUL);
          irq(EHST);
        } else if (dt_.part >= 0) {
          irq(EHST);
        }
        dt_ = Xfer{};
      } else {
        basic(uint16_t(st8 | 0xFF), 0xFFFF, 0, 0);
      }
      break;
    }
    case 0x10: {                                                      // tocar
      uint32_t ps = uint32_t(cr_[0] & 0xFF) << 16 | cr_[1];
      uint32_t pe = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      const int pm = cr_[2] >> 8;
      if (ps == 0xFFFFFF) ps = playCmdStart_;   // "sem mudança": a última posição pedida
      if (pe == 0xFFFFFF) pe = playCmdEnd_;
      else if ((ps & 0x800000) && (pe & 0x800000)) pe = 0x800000 | ((ps + pe) & 0x7FFFFF);
      if (((ps ^ pe) & 0x800000) && pe != 0) { status(0, true); break; }   // FAD e faixa misturados
      playCmdStart_ = ps;
      playCmdEnd_ = pe;
      if (!(pm & 0x70)) playCmdRep_ = uint8_t(pm & 0x0F);
      uint32_t start, end;
      if (ps & 0x800000) start = ps & 0x7FFFFF;
      else start = fadOfTrack(int(ps >> 8), false);
      if (pe & 0x800000) end = pe & 0x7FFFFF;
      else if (pe == 0) end = disc_ ? disc_->totalSectors() + 150 : start;
      else end = fadOfTrack(int(pe >> 8), true);
      if (disc_) stat_ = ST_BUSY;
      status();
      startSeek(start, end, playCmdRep_, PEND, (pm & 0x80) != 0);
      break;
    }
    case 0x11: {                                                      // buscar / pausar / parar
      const uint32_t sp = uint32_t(cr_[0] & 0xFF) << 16 | cr_[1];
      if (disc_) stat_ = ST_BUSY;
      status();
      if (!disc_) break;
      if (!sp) {                // parar: o motor desliga (parado depois de ~0,9 s)
        held_ = false;
        drive_ = F_PARADO;
        driveClock_ = TO_STANDBY;
        fad_ = 0xFFFFFF;
        endIrq_ = 0;
      } else if (sp == 0xFFFFFF) {   // pausar onde está
        if (drive_ == F_PARADO) startSeek(150, 0, 0, 0, false);
        playEnd_ = 0;
        repeatMax_ = 0;
        endIrq_ = 0;
      } else {
        const uint32_t to = (sp & 0x800000) ? (sp & 0x7FFFFF) : fadOfTrack(int(sp >> 8), false);
        startSeek(to, 0, 0, 0, false);
      }
      break;
    }
    case 0x12: {                                                      // varredura (avanço/retrocesso rápido)
      const uint8_t dir = uint8_t(cr_[0]);
      if (dir >= 2) { status(0, true); break; }
      if (disc_) stat_ = ST_BUSY;
      status();
      if (!disc_) break;
      held_ = false;
      secDone_ = false;
      scan_ = int8_t(dir);
      scanCount_ = 0;
      repeatMax_ = 0;
      if (drive_ != F_TOCA) {   // parte da posição atual
        const int8_t sc = scan_;
        const uint32_t s = playStart_, e = playEnd_;
        startSeek(cur_, e, 0, endIrq_, false);
        playStart_ = s;
        scan_ = sc;
      }
      break;
    }
    case 0x20: {                                                      // subcódigo Q (0) ou R-W (1)
      if (dt_.active) { status(ST_WAIT); break; }
      const int type = cr_[0] & 0xFF;
      if (type >= 2) { status(0, true); break; }
      if (type == 0) {
        startRawTransfer(std::vector<uint8_t>(subq_, subq_ + 10));
        basic(uint16_t((stat_ | ST_DTREQ) << 8), 5, 0, 0);
      } else {
        startRawTransfer(std::vector<uint8_t>(24, 0xFF));
        basic(uint16_t((stat_ | ST_DTREQ) << 8), 12, 0, 0);
      }
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
        if (rf & 0x10) for (int i = 0; i < 24; i++) { const uint8_t t = filt_[i].trueConn, f = filt_[i].falseConn; filt_[i] = Filter{}; filt_[i].trueConn = t; filt_[i].falseConn = f; }
        if (rf & 0x20) { cdConn_ = 0xFF; for (auto& fl : filt_) if (fl.falseConn < 24) fl.falseConn = 0xFF; }
        if (rf & 0x40) for (int i = 0; i < 24; i++) filt_[i].trueConn = uint8_t(i);
        if (rf & 0x80) for (auto& fl : filt_) fl.falseConn = 0xFF;
      }
      status(); irq(ESEL); break;
    }
    case 0x50: basic(st8, uint16_t(std::max(0, freeBuffers())), 0x18 << 8, NUM_BUFFERS); break;
    case 0x51:
      if (fnum >= 24) { status(0, true); break; }
      basic(st8, 0, 0, uint16_t(part_[fnum].size())); break;
    case 0x52: {                                                      // calcular tamanho real (palavras)
      if (fnum >= 24) { status(0, true); break; }
      int offs = cr_[1], n = cr_[3];
      const int cnt = int(part_[fnum].size());
      if (offs == 0xFFFF) offs = cnt - 1;
      if (n == 0xFFFF) n = cnt - offs;
      if ((dt_.active && dt_.writing) || n <= 0 || offs < 0 || offs + n > cnt) { status(ST_WAIT); break; }
      uint32_t acc = 0;
      for (int i = 0; i < n; i++) {
        const auto& s = part_[fnum][size_t(offs + i)];
        if (getSecLen_ == 2048) acc += (s[15] == 2 && (s[18] & 0x20)) ? 1162 : 1024;
        else acc += uint32_t(getSecLen_ / 2);
      }
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
      const uint32_t f = headerFad(s.data());
      const bool m2 = s[15] == 2;
      basic(uint16_t(st8 | (f >> 16)), uint16_t(f), uint16_t((m2 ? s[16] : 0) << 8 | (m2 ? s[17] : 0)), uint16_t((m2 ? s[18] : 0) << 8 | (m2 ? s[19] : 0)));
      break;
    }
    case 0x55: {                                                      // busca por FAD na partição
      const int offs = cr_[1];
      const uint32_t sfad = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      if (fnum >= 24 || part_[fnum].empty() || (offs != 0xFFFF && offs >= int(part_[fnum].size()))) { status(0, true); break; }
      // o maior FAD <= sfad a partir do setor offs (no empate fica o primeiro)
      const int from = offs == 0xFFFF ? int(part_[fnum].size()) - 1 : offs;
      srchPos_ = 0xFFFF; srchPart_ = fnum; srchFad_ = 0;
      bool found = false;
      for (int i = from; i < int(part_[fnum].size()); i++) {
        const uint32_t f = headerFad(part_[fnum][size_t(i)].data());
        if (f <= sfad && f >= srchFad_ + (found ? 1u : 0u)) { srchPos_ = uint16_t(i); srchFad_ = f; found = true; }
      }
      status(); irq(ESEL); break;
    }
    case 0x56: basic(st8, srchPos_, uint16_t(srchPart_ << 8 | (srchFad_ >> 16)), uint16_t(srchFad_)); break;
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
    case 0x64: {                                                      // pôr setores (o SH-2 escreve)
      const int n = cr_[3];
      if (fnum >= 24) { status(0, true); break; }
      if (n == 0 || n > freeBuffers() || dt_.active) { status(ST_WAIT); break; }
      static const int OFS[4] = {24, 16, 12, 0};
      const int idx = putSecLen_ == 2336 ? 1 : putSecLen_ == 2340 ? 2 : putSecLen_ == 2352 ? 3 : 0;
      // os dados de cada setor entram no buffer a partir do deslocamento do tamanho escolhido
      dt_ = Xfer{};
      dt_.data.assign(size_t(n) * size_t(putSecLen_), 0);
      dt_.writing = true;
      dt_.active = true;
      dt_.part = fnum;
      dt_.count = n;
      dt_.offset = OFS[idx];
      disconnectInput(fnum);
      status(ST_DTREQ);
      irq(DRDY);
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
    case 0x70: case 0x71: {                                           // mudar / ler diretório (lê o disco)
      if (fsOp_) { status(ST_WAIT); break; }
      if (fnum >= 24 || !disc_) { status(0, true); break; }
      const uint32_t fid = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      uint32_t fad = 0, size = 0, offset = 2;
      if (cmd == 0x70) {
        if (fid == 0) { status(); irq(EFLS); break; }   // o diretório atual: nada a ler
        if (fid == 0xFFFFFF) {
          uint8_t pvd[cdrom::DATA_SECTOR];
          if (!disc_->readData(16, pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) { status(0, true); break; }
          fad = le32(pvd + 156 + 2) + 150;
          size = le32(pvd + 156 + 10);
        } else if (fid < dir_.size() && (dir_[fid].attr & 2)) {
          fad = dir_[fid].fad;
          size = dir_[fid].size;
        } else { status(0, true); break; }
      } else {
        if (dir_.empty() || !curDirFad_) { status(0, true); break; }
        fad = curDirFad_;
        size = curDirSize_;
        offset = fid;
      }
      status();
      // os setores do diretório passam pelo filtro fnum (como no aparelho); o EFLS vem no fim da leitura
      const uint32_t n = std::max<uint32_t>(1, std::min<uint32_t>((size + 2047) / 2048, 64));
      cdConn_ = fnum;
      filt_[fnum] = Filter{};
      filt_[fnum].trueConn = fnum;
      filt_[fnum].fad = fad;
      filt_[fnum].range = n;
      filt_[fnum].mode = 0x40;
      part_[fnum].clear();
      fsOp_ = 1; fsPart_ = fnum; fsFad_ = fad; fsSize_ = size; fsOffset_ = offset;
      startSeek(fad, fad + n, 0, 0, false);
      if (drive_ != F_GIRO) finishFs();   // sem disco girando: termina já
      break;
    }
    case 0x72: {                                                      // escopo do sistema de arquivos
      if (fsOp_) { status(ST_WAIT); break; }
      if (dir_.empty()) { status(0, true); break; }
      const uint32_t total = uint32_t(dir_.size());
      const uint32_t cnt = total > dirOffset_ ? std::min<uint32_t>(254, total - dirOffset_) : 0;
      const bool more = dirOffset_ + cnt < total;
      basic(st8, uint16_t(cnt), uint16_t((!more ? 1 : 0) << 8 | (dirOffset_ >> 16)), uint16_t(dirOffset_));
      break;
    }
    case 0x73: {                                                      // informação do arquivo (12 bytes cada)
      const uint32_t fid = uint32_t(cr_[2] & 0xFF) << 16 | cr_[3];
      if (fsOp_ || dt_.active) { status(ST_WAIT); break; }
      if (dir_.empty()) { status(0, true); break; }
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
      if (fsOp_) { status(ST_WAIT); break; }
      if (fnum >= 24 || fid >= dir_.size()) { status(0, true); break; }
      const FileEnt& e = dir_[fid];
      const uint32_t total = (e.size + 2047) / 2048;
      const uint32_t start = (e.fad + off) & 0xFFFFFF, n = total - std::min(off, total);
      status();
      part_[fnum].clear();
      cdConn_ = fnum;
      filt_[fnum] = Filter{};
      filt_[fnum].trueConn = fnum;
      filt_[fnum].fad = start;
      filt_[fnum].range = n;
      filt_[fnum].mode = 0x41;
      filt_[fnum].file = e.fileNo;
      startSeek(start, start + n, 0, EFLS, false);
      break;
    }
    case 0x75:                                                        // abortar arquivo
      status();
      if (fsOp_) { part_[fsPart_].clear(); fsOp_ = 0; }
      playEnd_ = 0; repeatMax_ = 0; endIrq_ = 0;
      irq(EFLS);
      break;
    case 0xE0: {                                                      // autenticar o disco (ou o MPEG)
      if (cr_[1] == 1) { status(); irq(MPED); break; }   // não há cartão MPEG
      if (fnum >= 24) { status(0, true); break; }
      if (fsOp_) { status(ST_WAIT); break; }
      status();
      if (!disc_) break;
      const auto& ts = disc_->tracks();
      if (ts.empty() || ts[0].type == cdrom::TrackType::Audio) { authType_ = 1; irq(EFLS); break; }
      // lê o começo do disco pela partição fnum e confere o cabeçalho do Saturn
      cdConn_ = fnum;
      filt_[fnum].trueConn = fnum;
      filt_[fnum].falseConn = 0xFF;
      filt_[fnum].fad = filt_[fnum].range = 0;
      filt_[fnum].mode = 0;
      authType_ = 0xFF;
      fsOp_ = 2; fsPart_ = fnum;
      startSeek(150, 151, 0, 0, false);
      break;
    }
    case 0xE1:                                                        // tipo do disco autenticado
      basic(st8, uint16_t(cr_[1] ? 0 : authType_), 0, 0); break;
    default: status(0, true); break;
  }
}

void CdBlock::sync(StateIO& io) {
  io.sync(hirq_); io.sync(mask_); io.raw(cr_, sizeof cr_); io.raw(res_, sizeof res_);
  io.sync(stat_); io.sync(cmdPending_); io.sync(cmdClock_);
  io.sync(drive_); io.sync(driveClock_); io.sync(periodClock_);
  io.sync(fad_); io.sync(cur_); io.sync(playStart_); io.sync(playEnd_);
  io.sync(playCmdStart_); io.sync(playCmdEnd_); io.sync(playCmdRep_);
  io.sync(repeat_); io.sync(repeatMax_); io.sync(endIrq_);
  io.sync(held_); io.sync(secDone_); io.sync(pauseFull_); io.sync(scan_); io.sync(scanCount_);
  io.raw(subq_, sizeof subq_);
  io.sync(getSecLen_); io.sync(putSecLen_);
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
    io.sync(dt_.active); io.sync(dt_.part); io.sync(dt_.count); io.sync(dt_.offset); io.sync(dt_.writing);
  }
  io.sync(lastXfer_); io.sync(srchPos_); io.sync(srchPart_); io.sync(srchFad_);
  {
    uint32_t n = uint32_t(dir_.size());
    io.sync(n);
    if (io.reading()) { if (n > 100000) throw std::runtime_error("Estado salvo inválido (CD)."); dir_.assign(n, FileEnt{}); }
    for (auto& e : dir_) { io.sync(e.fad); io.sync(e.size); io.sync(e.attr); io.sync(e.unit); io.sync(e.gap); io.sync(e.fileNo); }
  }
  io.sync(dirOffset_); io.sync(curDirFad_); io.sync(curDirSize_);
  io.sync(fsOp_); io.sync(fsPart_); io.sync(fsFad_); io.sync(fsSize_); io.sync(fsOffset_); io.sync(authType_);
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
    // valores adulterados: recusa o que quebraria as contas e limita os relógios
    if (drive_ >= F_FIM || fsOp_ > 2 || fsPart_ >= 24 || scan_ < -1 || scan_ > 1 ||
        (getSecLen_ != 2048 && getSecLen_ != 2336 && getSecLen_ != 2340 && getSecLen_ != 2352) ||
        (putSecLen_ != 2048 && putSecLen_ != 2336 && putSecLen_ != 2340 && putSecLen_ != 2352) ||
        dt_.part < -1 || dt_.part >= 24 || dt_.count < 0 || dt_.count > NUM_BUFFERS ||
        repeat_ < 0 || repeat_ > 0xF || repeatMax_ < 0 || repeatMax_ > 0xF)
      throw std::runtime_error("Estado salvo inválido (CD).");
    if (dt_.writing) {  // escrita em andamento: tamanho e deslocamento precisam caber no setor
      const size_t len = dt_.count ? dt_.data.size() / size_t(dt_.count) : 0;
      if (!dt_.count || dt_.part < 0 || len * size_t(dt_.count) != dt_.data.size() || dt_.offset < 0 ||
          (len != 2048 && len != 2336 && len != 2340 && len != 2352) || size_t(dt_.offset) + len > cdrom::RAW_SECTOR)
        throw std::runtime_error("Estado salvo inválido (CD).");
    }
    if (freeBuffers() < 0) throw std::runtime_error("Estado salvo inválido (CD).");   // setores + reserva do PUT
    cmdClock_ = std::clamp(cmdClock_, 0, CMD_BASE + clk(300));
    driveClock_ = std::clamp(driveClock_, 1, 1 << 30);
    periodClock_ = std::clamp(periodClock_, 1, PERIOD);
  }
}

}  // namespace indigo::saturn
