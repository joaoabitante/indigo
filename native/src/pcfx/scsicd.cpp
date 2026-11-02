#include "scsicd.h"

#include <algorithm>
#include <cstring>

namespace indigo::pcfx {

namespace {
constexpr int64_t SECTOR_CLOCKS = 143182;          // 2048 bytes a 300 KB/s no relógio de 21,48 MHz
constexpr uint64_t MASTER_NUM = 236250000;         // relógio mestre = 236250000 / 11 Hz
constexpr uint64_t CDDA_DEN = 11ull * 44100;
enum : uint8_t { ST_GOOD = 0, ST_CHECK = 1, ST_MET = 2 };
enum : uint8_t { SK_NOT_READY = 2, SK_MEDIUM = 3, SK_ILLEGAL = 5 };
enum : uint8_t { ASC_INVALID_CMD = 0x20, ASC_END_OF_VOLUME = 0x21, ASC_INVALID_PARAM = 0x26, ASC_NOT_AUDIO = 0x64,
                 ASC_AUDIO_NOT_PLAYING = 0x2C, ASC_NOT_DATA = 0x64 };
const uint8_t CDB_LEN[16] = {6, 6, 10, 10, 10, 10, 10, 10, 10, 10, 12, 12, 10, 10, 10, 10};
uint8_t bcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }
bool bcdOk(uint8_t v) { return (v >> 4) < 10 && (v & 15) < 10; }
uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
}  // namespace

ScsiCd::ScsiCd(cdrom::Disc* disc) : disc_(disc) {}

uint32_t ScsiCd::leadout() const { return disc_ ? disc_->totalSectors() : 0; }

int ScsiCd::trackOf(uint32_t lba) const {
  if (!disc_) return 0;
  const int i = disc_->trackAt(lba);
  return i < 0 ? 0 : disc_->tracks()[size_t(i)].number;
}

bool ScsiCd::isData(int track) const {
  for (const auto& t : disc_->tracks())
    if (t.number == track) return t.type != cdrom::TrackType::Audio;
  return false;
}

uint32_t ScsiCd::trackStart(int track) const {
  for (const auto& t : disc_->tracks())
    if (t.number == track) return t.start;
  return leadout();
}

void ScsiCd::power(int64_t now) {
  last_ = now;
  sig_ = 0;
  db_ = 0;
  lastRst_ = false;
  cmdPos_ = 0;
  statusSent_ = msgSent_ = false;
  msgPending_ = senseKey_ = senseAsc_ = 0;
  virtualReset();
}

void ScsiCd::virtualReset() {
  fifo_.clear();
  fifoPos_ = 0;
  transferDone_ = false;
  readTimer_ = NONE;
  sectorAddr_ = sectorCount_ = 0;
  readSec_ = readSecStart_ = 0;
  readSecEnd_ = ~0u;
  playMode_ = PM_SILENT;
  cddaStatus_ = CD_STOPPED;
  cddaPos_ = 588;
  cddaNext_ = NONE;
  dataOutPos_ = dataOutWant_ = 0;
  std::memset(modeData_, 0, sizeof modeData_);
  changePhase(BUS_FREE);
}

void ScsiCd::setReq(bool on) {
  if (on && !(sig_ & REQ)) reqRose = true;
  setSignal(REQ, on);
}

void ScsiCd::changePhase(uint8_t p) {
  auto set = [&](bool bsy, bool msg, bool cd, bool io, bool req) {
    setSignal(BSY, bsy); setSignal(MSG, msg); setSignal(CD, cd); setSignal(IO, io); setReq(req);
  };
  switch (p) {
    case BUS_FREE: set(false, false, false, false, false); break;
    case DATA_IN: set(true, false, false, true, false); break;
    case STATUS: set(true, false, true, true, true); break;
    case MESSAGE_IN: set(true, true, true, true, true); break;
    case DATA_OUT: set(true, false, false, false, true); break;
    case COMMAND: set(true, false, true, false, true); break;
    default: set(true, true, true, false, true); break;  // MESSAGE_OUT
  }
  phase_ = p;
}

void ScsiCd::statusMessage(uint8_t status, uint8_t msg) {
  fifo_.clear();
  fifoPos_ = 0;
  msgPending_ = msg;
  statusSent_ = msgSent_ = false;
  db_ = uint8_t(status << 1);
  changePhase(STATUS);
}

void ScsiCd::ccError(uint8_t key, uint8_t asc) {
  senseKey_ = key;
  senseAsc_ = asc;
  statusMessage(ST_CHECK, 0);
}

void ScsiCd::dataIn(const uint8_t* p, size_t n) {
  fifo_.assign(p, p + n);
  fifoPos_ = 0;
  transferDone_ = true;
  changePhase(DATA_IN);
}

void ScsiCd::makeSubQ(uint32_t lba) {
  const int tr = trackOf(lba);
  const uint32_t ts = tr ? trackStart(tr) : 0;
  const uint32_t rel = lba >= ts ? lba - ts : ts - lba;
  uint8_t m, s, f, am, as, af;
  // posição relativa à faixa (sem os 150 setores) e absoluta
  m = uint8_t(rel / (60 * 75)); s = uint8_t(rel / 75 % 60); f = uint8_t(rel % 75);
  cdrom::lbaToMsf(lba, am, as, af);
  subQ_[0] = uint8_t(0x01 | ((tr && isData(tr)) ? 0x40 : 0x00));
  subQ_[1] = bcd(tr ? tr : 1);
  subQ_[2] = lba >= ts ? 1 : 0;
  subQ_[3] = bcd(m); subQ_[4] = bcd(s); subQ_[5] = bcd(f);
  subQ_[6] = 0;
  subQ_[7] = bcd(am); subQ_[8] = bcd(as); subQ_[9] = bcd(af);
}

void ScsiCd::readBase(uint32_t sa, uint32_t sc, int64_t now) {
  if (sa > leadout()) { ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); return; }
  const int tr = trackOf(sa);
  if (!tr && sa != leadout()) { ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); return; }
  if (tr && !isData(tr)) { ccError(SK_MEDIUM, ASC_NOT_DATA); return; }
  if (!sc && sa == leadout()) { ccError(SK_MEDIUM, 0x02); return; }
  sectorAddr_ = sa;
  sectorCount_ = sc;
  if (sc) readTimer_ = now + SECTOR_CLOCKS;
  else { readTimer_ = NONE; statusMessage(ST_GOOD, 0); }
  cddaStatus_ = CD_STOPPED;
}

void ScsiCd::playBase(uint32_t lba, uint32_t len, int8_t status, uint8_t mode) {
  if (lba > leadout() || lba < trackStart(disc_->tracks().front().number)) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); return; }
  if (!len) { statusMessage(ST_GOOD, 0); return; }
  const int tr = trackOf(lba);
  if (tr && isData(tr)) { ccError(SK_MEDIUM, ASC_NOT_AUDIO); return; }
  cddaPos_ = 588;
  readSec_ = readSecStart_ = lba;
  readSecEnd_ = lba + len;
  cddaStatus_ = status;
  playMode_ = mode;
  if (cddaNext_ == NONE) { cddaNext_ = last_ + 1; cddaFrac_ = 0; }
  statusMessage(ST_GOOD, 0);
}

// executa o comando recebido em cmd_
void ScsiCd::execute() {
  const uint8_t* c = cmd_;
  const uint8_t op = c[0];
  const bool needsDisc = !(op == 0x03 || op == 0x0D || op == 0x12 || op == 0x15 || op == 0x1A || op == 0x1E || op == 0x01);
  if (needsDisc && !disc_) { ccError(SK_NOT_READY, 0x3A); return; }
  uint8_t d[2048];
  std::memset(d, 0, sizeof d);
  switch (op) {
    case 0x00: case 0x01: case 0x0D: case 0x1B: case 0x1E: case 0xDB:  // TEST UNIT READY, REZERO, NOP, START/STOP...
      statusMessage(ST_GOOD, 0);
      break;
    case 0x03:  // REQUEST SENSE
      d[0] = 0x70; d[2] = senseKey_; d[7] = 0x0A; d[12] = senseAsc_;
      dataIn(d, 18);
      senseKey_ = senseAsc_ = 0;
      break;
    case 0x08: {  // READ(6)
      uint32_t sc = c[4];
      if (!sc) sc = 256;
      readBase(uint32_t(c[1] & 0x1F) << 16 | uint32_t(c[2]) << 8 | c[3], sc, last_);
      break;
    }
    case 0x28: readBase(be32(c + 2), uint32_t(c[7]) << 8 | c[8], last_); break;  // READ(10)
    case 0xA8: readBase(be32(c + 2), be32(c + 6), last_); break;                 // READ(12)
    case 0x0B: case 0x2B: {  // SEEK
      const uint32_t lba = op == 0x0B ? (uint32_t(c[1] & 0x1F) << 16 | uint32_t(c[2]) << 8 | c[3]) : be32(c + 2);
      if (lba >= leadout()) { ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); break; }
      cddaStatus_ = CD_STOPPED;
      statusMessage(ST_GOOD, 0);
      break;
    }
    case 0x12: {  // INQUIRY
      static const uint8_t inq[36] = {0x05, 0x80, 0x02, 0x00, 0x1F, 0, 0, 0, 'N', 'E', 'C', ' ', ' ', ' ', ' ', ' ',
                                      'C', 'D', '-', 'R', 'O', 'M', ' ', 'D', 'R', 'I', 'V', 'E', ':', 'F', 'X', ' ',
                                      '1', '.', '0', ' '};
      dataIn(inq, std::min<size_t>(c[4], 36));
      break;
    }
    case 0x15:  // MODE SELECT(6): recebe os parâmetros e ignora
      if (c[4]) { dataOutWant_ = c[4]; dataOutPos_ = 0; changePhase(DATA_OUT); }
      else statusMessage(ST_GOOD, 0);
      break;
    case 0x1A: {  // MODE SENSE(6): só o cabeçalho
      d[0] = 3;
      dataIn(d, std::min<size_t>(c[4], 4));
      break;
    }
    case 0x25: {  // READ CD-ROM CAPACITY
      const uint32_t last = leadout() ? leadout() - 1 : 0;
      d[0] = uint8_t(last >> 24); d[1] = uint8_t(last >> 16); d[2] = uint8_t(last >> 8); d[3] = uint8_t(last);
      d[6] = 0x08;  // 2048 bytes por bloco
      dataIn(d, 8);
      break;
    }
    case 0x34:  // PREFETCH
      if (be32(c + 2) >= leadout()) ccError(SK_ILLEGAL, ASC_END_OF_VOLUME);
      else statusMessage(ST_MET, 0);
      break;
    case 0x42: {  // READ SUBCHANNEL (formato 1: posição atual)
      const bool msf = c[1] & 2;
      const uint32_t lba = cddaStatus_ != CD_STOPPED ? readSec_ : sectorAddr_;
      makeSubQ(lba);
      d[1] = cddaStatus_ == CD_PLAYING ? 0x11 : cddaStatus_ == CD_PAUSED ? 0x12 : 0x13;
      size_t n = 4;
      if (c[2] & 0x40) {
        d[3] = 12;
        d[4] = 1; d[5] = subQ_[0]; d[6] = uint8_t(unbcd(subQ_[1])); d[7] = subQ_[2];
        const int tr = trackOf(lba);
        const uint32_t rel = lba - (tr ? trackStart(tr) : 0);
        if (msf) {
          uint8_t m, s, f; cdrom::lbaToMsf(lba, m, s, f);
          d[9] = m; d[10] = s; d[11] = f;
          d[13] = uint8_t(rel / 4500); d[14] = uint8_t(rel / 75 % 60); d[15] = uint8_t(rel % 75);
        } else {
          d[8] = uint8_t(lba >> 24); d[9] = uint8_t(lba >> 16); d[10] = uint8_t(lba >> 8); d[11] = uint8_t(lba);
          d[12] = uint8_t(rel >> 24); d[13] = uint8_t(rel >> 16); d[14] = uint8_t(rel >> 8); d[15] = uint8_t(rel);
        }
        n = 16;
      }
      dataIn(d, std::min<size_t>(n, size_t(c[7]) << 8 | c[8]));
      break;
    }
    case 0x43: {  // READ TOC
      const bool msf = c[1] & 2;
      const auto& tr = disc_->tracks();
      const int first = tr.front().number, last = tr.back().number;
      int start = c[6];
      const size_t alloc = size_t(c[7]) << 8 | c[8];
      if (!alloc) { statusMessage(ST_GOOD, 0); break; }
      if (!start) start = 1;
      else if (start == 0xAA) start = last + 1;
      else if (start > last) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); break; }
      size_t n = 4;
      d[2] = uint8_t(first); d[3] = uint8_t(last);
      for (int t = start; t <= last + 1 && n + 8 <= sizeof d; t++) {
        const bool lo = t == last + 1;
        const uint32_t lba = lo ? leadout() : trackStart(t);
        const bool data = !lo && isData(t);
        d[n + 1] = uint8_t(0x10 | (data ? 4 : 0));
        d[n + 2] = uint8_t(lo ? 0xAA : t);
        if (msf) { uint8_t m, s, f; cdrom::lbaToMsf(lba, m, s, f); d[n + 5] = m; d[n + 6] = s; d[n + 7] = f; }
        else { d[n + 4] = uint8_t(lba >> 24); d[n + 5] = uint8_t(lba >> 16); d[n + 6] = uint8_t(lba >> 8); d[n + 7] = uint8_t(lba); }
        n += 8;
      }
      d[0] = uint8_t((n - 2) >> 8); d[1] = uint8_t(n - 2);
      dataIn(d, std::min(n, alloc));
      break;
    }
    case 0x44: {  // READ HEADER
      const uint32_t lba = be32(c + 2);
      if (lba >= leadout()) { ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); break; }
      d[0] = 1;
      if (c[1] & 2) { uint8_t m, s, f; cdrom::lbaToMsf(lba, m, s, f); d[5] = m; d[6] = s; d[7] = f; }
      else { d[4] = uint8_t(lba >> 24); d[5] = uint8_t(lba >> 16); d[6] = uint8_t(lba >> 8); d[7] = uint8_t(lba); }
      dataIn(d, std::min<size_t>(8, size_t(c[7]) << 8 | c[8]));
      break;
    }
    case 0x45: playBase(be32(c + 2), uint32_t(c[7]) << 8 | c[8], CD_PLAYING, PM_NORMAL); break;  // PLAY AUDIO(10)
    case 0xA5: playBase(be32(c + 2), be32(c + 6), CD_PLAYING, PM_NORMAL); break;                // PLAY AUDIO(12)
    case 0x47: {  // PLAY AUDIO MSF
      const uint32_t a = cdrom::msfToLba(c[3], c[4], c[5]), b = cdrom::msfToLba(c[6], c[7], c[8]);
      if (b < a) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); break; }
      playBase(a, b - a, CD_PLAYING, PM_NORMAL);
      break;
    }
    case 0x48: {  // PLAY AUDIO TRACK INDEX
      const int t0 = c[4], t1 = c[7];
      if (!t0 || t0 > disc_->tracks().back().number) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); break; }
      const uint32_t a = trackStart(t0), b = t1 >= disc_->tracks().back().number ? leadout() : trackStart(t1 + 1);
      playBase(a, b > a ? b - a : 0, CD_PLAYING, PM_NORMAL);
      break;
    }
    case 0x4B:  // PAUSE/RESUME
      if (cddaStatus_ == CD_STOPPED) { ccError(SK_ILLEGAL, ASC_AUDIO_NOT_PLAYING); break; }
      cddaStatus_ = (c[8] & 1) ? CD_PLAYING : CD_PAUSED;
      statusMessage(ST_GOOD, 0);
      break;
    case 0xD8: case 0xD9: {  // NEC: início (SAPSP) e fim (SAPEP) do áudio
      uint32_t lba = 0;
      switch (c[9] & 0xC0) {
        case 0x00: lba = uint32_t(c[3]) << 16 | uint32_t(c[4]) << 8 | c[5]; break;
        case 0x40:
          if (!bcdOk(c[2]) || !bcdOk(c[3]) || !bcdOk(c[4])) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); return; }
          lba = cdrom::msfToLba(unbcd(c[2]), unbcd(c[3]), unbcd(c[4]));
          break;
        case 0x80: {
          if (!c[2] || !bcdOk(c[2])) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); return; }
          const int t = unbcd(c[2]), last = disc_->tracks().back().number;
          if (t > last + 1) { ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); return; }
          lba = t == last + 1 ? leadout() : trackStart(t);
          break;
        }
        default: ccError(SK_ILLEGAL, ASC_INVALID_PARAM); return;
      }
      if (op == 0xD8) {
        if (c[1] & 1) playBase(lba, leadout() - std::min(lba, leadout()), CD_PLAYING, PM_NORMAL);
        else playBase(lba, leadout() - std::min(lba, leadout()), CD_PAUSED, PM_SILENT);
      } else {
        if (cddaStatus_ == CD_STOPPED) { ccError(SK_ILLEGAL, ASC_AUDIO_NOT_PLAYING); return; }
        switch (c[1] & 7) {
          case 0: playMode_ = PM_SILENT; break;
          case 4: playMode_ = PM_LOOP; break;
          default: playMode_ = PM_NORMAL; break;
        }
        cddaStatus_ = CD_PLAYING;
        if (cddaNext_ == NONE) { cddaNext_ = last_ + 1; cddaFrac_ = 0; }
        readSecEnd_ = lba;
        statusMessage(ST_GOOD, 0);
      }
      break;
    }
    case 0xDA:  // NEC: pausa
      if (cddaStatus_ == CD_STOPPED) { ccError(SK_ILLEGAL, ASC_AUDIO_NOT_PLAYING); break; }
      cddaStatus_ = CD_PAUSED;
      statusMessage(ST_GOOD, 0);
      break;
    case 0xDC: ccError(SK_ILLEGAL, 0x24); break;  // ejetar: não suportado (como no aparelho)
    case 0xDD: {  // NEC: subcanal Q
      makeSubQ(cddaStatus_ != CD_STOPPED ? readSec_ : sectorAddr_);
      d[0] = cddaStatus_ == CD_PAUSED ? 2 : cddaStatus_ == CD_PLAYING ? 0 : 3;
      d[1] = subQ_[0]; d[2] = subQ_[1]; d[3] = subQ_[2];
      d[4] = subQ_[3]; d[5] = subQ_[4]; d[6] = subQ_[5];
      d[7] = subQ_[7]; d[8] = subQ_[8]; d[9] = subQ_[9];
      dataIn(d, std::min<size_t>(c[1], 10));
      break;
    }
    case 0xDE: {  // NEC: informações do disco
      const auto& tr = disc_->tracks();
      size_t n = 4;
      switch (c[1] & 3) {
        case 0: d[0] = bcd(tr.front().number); d[1] = bcd(tr.back().number); break;
        case 1: { uint8_t m, s, f; cdrom::lbaToMsf(leadout(), m, s, f); d[0] = bcd(m); d[1] = bcd(s); d[2] = bcd(f); break; }
        case 2: {
          const int t = bcdOk(c[2]) ? unbcd(c[2]) : 0;
          if (t < tr.front().number || t > tr.back().number) { ccError(SK_ILLEGAL, ASC_INVALID_PARAM); return; }
          uint8_t m, s, f; cdrom::lbaToMsf(trackStart(t), m, s, f);
          d[0] = bcd(m); d[1] = bcd(s); d[2] = bcd(f); d[3] = isData(t) ? 4 : 0;
          break;
        }
        default: ccError(SK_ILLEGAL, ASC_INVALID_PARAM); return;
      }
      dataIn(d, n);
      break;
    }
    default: ccError(SK_ILLEGAL, ASC_INVALID_CMD); break;
  }
}

void ScsiCd::runRead(int64_t t) {
  while (readTimer_ != NONE && readTimer_ <= t) {
    if (fifo_.size() - fifoPos_ + 2048 > 65536) { readTimer_ += SECTOR_CLOCKS; continue; }  // fila cheia
    uint8_t buf[2048];
    if (sectorAddr_ >= leadout()) { readTimer_ = NONE; ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); break; }
    if (!disc_->readData(sectorAddr_, buf)) { readTimer_ = NONE; ccError(SK_ILLEGAL, ASC_END_OF_VOLUME); break; }
    if (fifoPos_ && fifoPos_ == fifo_.size()) { fifo_.clear(); fifoPos_ = 0; }
    fifo_.insert(fifo_.end(), buf, buf + 2048);
    makeSubQ(sectorAddr_);
    sectorAddr_++;
    sectorCount_--;
    if (phase_ != DATA_IN) changePhase(DATA_IN);
    if (sectorCount_) { transferDone_ = false; readTimer_ += SECTOR_CLOCKS; }
    else { transferDone_ = true; readTimer_ = NONE; }
  }
}

void ScsiCd::runCdda(int64_t t) {
  while (cddaNext_ != NONE && cddaNext_ <= t) {
    if (cddaStatus_ != CD_PLAYING) {
      if (cddaStatus_ == CD_STOPPED) { cddaNext_ = NONE; break; }
      cdda.push_back(0); cdda.push_back(0);  // em pausa: silêncio
    } else {
      if (cddaPos_ == 588) {
        if (readSec_ >= readSecEnd_) {
          if (playMode_ == PM_LOOP) readSec_ = readSecStart_;
          else { cddaStatus_ = CD_STOPPED; cddaNext_ = NONE; break; }
        }
        if (readSec_ >= leadout()) { cddaStatus_ = CD_STOPPED; cddaNext_ = NONE; break; }
        uint8_t raw[cdrom::RAW_SECTOR];
        disc_->readRaw(readSec_, raw);
        for (int i = 0; i < 588 * 2; i++) cddaBuf_[i] = int16_t(raw[i * 2] | raw[i * 2 + 1] << 8);
        makeSubQ(readSec_);
        readSec_++;
        cddaPos_ = 0;
      }
      if (playMode_ == PM_SILENT) { cdda.push_back(0); cdda.push_back(0); }
      else { cdda.push_back(cddaBuf_[cddaPos_ * 2]); cdda.push_back(cddaBuf_[cddaPos_ * 2 + 1]); }
      cddaPos_++;
    }
    const uint64_t n = MASTER_NUM + cddaFrac_;
    cddaNext_ += int64_t(n / CDDA_DEN);
    cddaFrac_ = uint32_t(n % CDDA_DEN);
  }
}

int64_t ScsiCd::run(int64_t t) {
  if (t >= last_) {
    runRead(t);
    runCdda(t);
    last_ = t;
  }
  const bool rst = sig_ & RST;
  const bool reset = rst && !lastRst_;
  lastRst_ = rst;
  const bool req = sig_ & REQ, ack = sig_ & ACK;
  if (reset) {
    virtualReset();
  } else if (phase_ == BUS_FREE) {
    if (sig_ & SEL) changePhase(COMMAND);
  } else if ((sig_ & ATN) && !req && !ack) {
    changePhase(MESSAGE_OUT);
  } else {
    switch (phase_) {
      case COMMAND:
        if (req && ack) { if (cmdPos_ < sizeof cmd_) cmd_[cmdPos_++] = db_; setReq(false); }
        if (!(sig_ & REQ) && !ack && cmdPos_) {
          if (cmdPos_ >= CDB_LEN[cmd_[0] >> 4]) { execute(); cmdPos_ = 0; }
          else setReq(true);
        }
        break;
      case DATA_OUT:
        if (req && ack) { if (dataOutPos_ < 255) dataOut_[dataOutPos_++] = db_; setReq(false); }
        else if (!req && !ack && dataOutPos_) {
          if (dataOutPos_ >= dataOutWant_) { dataOutPos_ = 0; statusMessage(ST_GOOD, 0); }
          else setReq(true);
        }
        break;
      case MESSAGE_OUT:
        if (req && ack) {  // qualquer mensagem vale como ABORT
          setReq(false);
          fifo_.clear(); fifoPos_ = 0;
          dataOutPos_ = dataOutWant_ = 0;
          readTimer_ = NONE;
          cddaStatus_ = CD_STOPPED;
          changePhase(BUS_FREE);
        }
        break;
      case STATUS:
        if (req && ack) { setReq(false); statusSent_ = true; }
        if (!(sig_ & REQ) && !ack && statusSent_) {
          statusSent_ = false;
          db_ = msgPending_;
          changePhase(MESSAGE_IN);
        }
        break;
      case DATA_IN:
        if (!req && !ack) {
          if (fifoPos_ >= fifo_.size()) {
            if (transferDone_) { transferDone_ = false; statusMessage(ST_GOOD, 0); }
          } else {
            db_ = fifo_[fifoPos_++];
            if (fifoPos_ == fifo_.size() && readTimer_ == NONE) { fifo_.clear(); fifoPos_ = 0; fifo_.shrink_to_fit(); }
            setReq(true);
          }
        }
        if (req && ack) setReq(false);
        break;
      case MESSAGE_IN:
        if (req && ack) { setReq(false); msgSent_ = true; }
        if (!(sig_ & REQ) && !ack && msgSent_) { msgSent_ = false; changePhase(BUS_FREE); }
        break;
      default: break;
    }
  }
  int64_t next = NONE;
  if (readTimer_ < next) next = readTimer_;
  if (cddaNext_ < next) next = cddaNext_;
  return next;
}

void ScsiCd::sync(StateIO& io) {
  io.sync(last_); io.sync(sig_); io.sync(db_); io.sync(phase_); io.sync(lastRst_);
  io.raw(cmd_, sizeof cmd_); io.sync(cmdPos_);
  io.raw(dataOut_, sizeof dataOut_); io.sync(dataOutPos_); io.sync(dataOutWant_);
  io.sync(statusSent_); io.sync(msgSent_); io.sync(msgPending_); io.sync(senseKey_); io.sync(senseAsc_);
  uint32_t n = uint32_t(fifo_.size());
  io.sync(n);
  if (io.reading()) {
    if (n > 1 << 20) throw std::runtime_error("Estado salvo inválido (fila do CD).");
    fifo_.resize(n);
  }
  if (n) io.raw(fifo_.data(), n);
  io.sync(fifoPos_); io.sync(transferDone_);
  io.sync(readTimer_); io.sync(sectorAddr_); io.sync(sectorCount_);
  io.sync(cddaStatus_); io.sync(playMode_); io.sync(readSec_); io.sync(readSecStart_); io.sync(readSecEnd_);
  io.sync(cddaPos_); io.raw(cddaBuf_, sizeof cddaBuf_); io.sync(cddaNext_); io.sync(cddaFrac_);
  io.raw(subQ_, sizeof subQ_);
  if (io.reading()) {
    if (fifoPos_ > fifo_.size()) fifoPos_ = uint32_t(fifo_.size());
    if (cmdPos_ > sizeof cmd_) cmdPos_ = 0;
    if (cddaPos_ > 588) cddaPos_ = 588;
    if (phase_ > MESSAGE_OUT) phase_ = BUS_FREE;
    cddaFrac_ = uint32_t(cddaFrac_ % CDDA_DEN);
    cdda.clear();
  }
}

}  // namespace indigo::pcfx
