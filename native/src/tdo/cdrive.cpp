// Unidade de CD do 3DO, ver cdrive.h.
#include "cdrive.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::tdo {

void CdDrive::reset() {
  cmdLen_ = 0;
  status_.clear();
  pendingStatus_.clear();
  data_.clear();
  delay_ = 0;
  spinning_ = false;
  door_ = false;
  doubleSpeed_ = false;
  success_ = playing_ = readDone_ = false;
  sectorSize_ = 2048;
  readLba_ = readLeft_ = 0;
  readTimer_ = 0;
  playLba_ = playEnd_ = playPos_ = 0;
  pcmLba_ = 0xFFFFFFFF;
}

uint8_t CdDrive::stat() const {
  uint8_t s = 0;
  const bool have = disc_ && !door_;
  if (!door_) s |= ST_DOOR;
  if (have) s |= ST_DISC | ST_READY;
  if (spinning_) s |= ST_SPIN;
  if (success_) s |= ST_OK;
  if (playing_) s |= ST_PLAY;
  if (doubleSpeed_) s |= ST_2X;
  return s;
}

void CdDrive::reply(std::initializer_list<uint8_t> bytes, uint8_t extra) {
  pendingStatus_.assign(bytes.begin(), bytes.end());
  pendingStatus_.push_back(uint8_t(stat() | extra));
  delay_ = REPLY_DELAY;
}

void CdDrive::writeCommand(uint8_t b) {
  if (cmdLen_ >= 7) return;
  cmd_[cmdLen_++] = b;
  if (cmdLen_ == 1 && b == 0x08) {  // abortar: um byte, sem resposta
    cmdLen_ = 0;
    readLeft_ = 0;
    readDone_ = false;
    return;
  }
  if (cmdLen_ == 7) execute();
}

uint8_t CdDrive::readStatus() {
  if (status_.empty()) return 0;
  const uint8_t b = status_.front();
  status_.pop_front();
  return b;
}

uint8_t CdDrive::readData() {
  if (data_.empty()) return 0xFF;
  const uint8_t b = data_.front();
  data_.pop_front();
  if (data_.empty() && readDone_ && !readLeft_) {  // fim da leitura: a resposta vem depois do último dado
    readDone_ = false;
    success_ = true;
    reply({0x10});
  }
  return b;
}

void CdDrive::execute() {
  const uint8_t c = cmd_[0];
  cmdLen_ = 0;
  status_.clear();
  const bool have = disc_ && !door_;
  uint8_t m = 0, s = 0, f = 0;
  if (have) cdrom::lbaToMsf(disc_->totalSectors(), m, s, f);
  uint8_t last = 1;
  if (have && !disc_->tracks().empty()) last = uint8_t(disc_->tracks().back().number);
  switch (c) {
    case 0x01: spinning_ = have; reply({c}, have ? 0 : ST_ERR); break;
    case 0x02: spinning_ = have; success_ = have; reply({c}, have ? 0 : ST_ERR); break;
    case 0x03: spinning_ = false; playing_ = false; readLeft_ = 0; readDone_ = false; reply({c}); break;
    case 0x05: success_ = false; reply({c}); break;
    case 0x06:
      door_ = true; spinning_ = playing_ = false; readLeft_ = 0; readDone_ = false; data_.clear();
      reply({c});
      break;
    case 0x07: door_ = false; reply({c}); break;
    case 0x09:
      if (cmd_[1] == 0) {
        const int sz = cmd_[3] << 8 | cmd_[4];
        if (sz == 2048 || sz == 2336 || sz == 2340 || sz == 2352 || sz == 2448) sectorSize_ = sz;
      } else if (cmd_[1] == 3) {
        doubleSpeed_ = cmd_[2] & 0x80;
      }
      success_ = true;
      reply({c});
      break;
    case 0x0E: {  // tocar de MSF a MSF
      const uint32_t a = uint32_t(cmd_[1]) << 16 | uint32_t(cmd_[2]) << 8 | cmd_[3];
      const uint32_t b = uint32_t(cmd_[4]) << 16 | uint32_t(cmd_[5]) << 8 | cmd_[6];
      if (!a && !b) {  // parar
        if (playing_) success_ = true;
        playing_ = spinning_ = false;
        reply({c});
        break;
      }
      auto lbaOf = [](uint32_t msf) { return int64_t((msf >> 16) * 4500 + ((msf >> 8) & 0xFF) * 75 + (msf & 0xFF)) - 150; };
      const int64_t from = lbaOf(a);
      const int64_t to = (b == 0xFFFFFF || !have) ? int64_t(have ? disc_->totalSectors() : 0) : lbaOf(b);
      if (!have || from < 0 || from >= to) { reply({c}, ST_ERR); break; }
      readLeft_ = 0; readDone_ = false;
      playLba_ = uint32_t(from);
      playEnd_ = uint32_t(std::min<int64_t>(to, disc_->totalSectors()));
      playPos_ = 0;
      playing_ = spinning_ = true;
      reply({c});
      break;
    }
    case 0x0F: {  // tocar faixas (da primeira até a última, inclusive)
      const cdrom::Track* first = nullptr;
      uint32_t end = have ? disc_->totalSectors() : 0;
      if (have) for (const auto& t : disc_->tracks()) {
        if (t.number == cmd_[1]) first = &t;
        if (t.number == cmd_[3] + 1) end = t.start;
      }
      if (!first || end <= first->start) { reply({c}, ST_ERR); break; }
      readLeft_ = 0; readDone_ = false;
      playLba_ = first->start;
      playEnd_ = end;
      playPos_ = 0;
      playing_ = spinning_ = true;
      reply({c});
      break;
    }
    case 0x10: {
      uint32_t lba = 0;
      bool bad = !have || (cmd_[4] & 0xFE);
      if (cmd_[4] & 1) lba = uint32_t(cmd_[1]) << 16 | uint32_t(cmd_[2]) << 8 | cmd_[3];
      else if (cmd_[2] > 59 || cmd_[3] > 74 || (cmd_[1] == 0 && cmd_[2] < 2)) bad = true;
      else lba = cdrom::msfToLba(cmd_[1], cmd_[2], cmd_[3]);
      const uint32_t n = uint32_t(cmd_[5]) << 8 | cmd_[6];
      if (bad || lba >= disc_->totalSectors()) { reply({c}, ST_ERR); break; }
      spinning_ = true;
      playing_ = false;
      data_.clear();
      readLba_ = lba;
      readLeft_ = n;
      readDone_ = true;
      readTimer_ = doubleSpeed_ ? CYCLES_PER_SECTOR_2X : CYCLES_PER_SECTOR_2X * 2;
      if (!n) { readDone_ = false; success_ = true; reply({c}); }
      break;
    }
    case 0x80: reply({c, 0xAA, 0x55}); break;
    case 0x82: reply({c, 0, 0, 0, 0, 0, 0, 0, 0}); break;
    case 0x83: reply({c, 0x00, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}); break;  // fabricante/modelo/versão
    case 0x85: case 0x8D:
      if (!have) { reply({c}, ST_ERR); break; }
      reply({c, 0, m, s, f, 0, 0});
      break;
    case 0x8B:
      if (!have) { reply({c}, ST_ERR); break; }
      spinning_ = true;
      success_ = true;
      reply({c, 0x00, 1, last, m, s, f});
      break;
    case 0x8C: {
      const uint8_t t = cmd_[2];
      if (!have || t > last) { reply({c}, ST_ERR); break; }
      spinning_ = true;
      if (t == 0) { reply({c, 0, 0x14, 1, last, m, s, f, 0}); break; }
      const cdrom::Track* tr = nullptr;
      for (const auto& x : disc_->tracks()) if (x.number == t) tr = &x;
      if (!tr) { reply({c}, ST_ERR); break; }
      cdrom::lbaToMsf(tr->start, m, s, f);
      const uint8_t adr = tr->type == cdrom::TrackType::Audio ? 0x10 : 0x14;
      reply({c, 0, adr, t, 0, m, s, f, 0});
      break;
    }
    default:
      reply({c}, ST_ERR);
      break;
  }
}

void CdDrive::tick(int cycles) {
  if (delay_ > 0) {
    delay_ -= cycles;
    if (delay_ <= 0) {
      delay_ = 0;
      for (uint8_t b : pendingStatus_) status_.push_back(b);
      pendingStatus_.clear();
    }
  }
  if (readLeft_) {
    readTimer_ -= cycles;
    while (readTimer_ <= 0 && readLeft_) {
      const size_t sz = size_t(sectorSize_);
      if (data_.size() + sz > FIFO_MAX) { readTimer_ = 0; break; }  // fila cheia: espera a CPU/DMA esvaziar
      uint8_t buf[2448] = {};
      if (sz == 2048) {
        if (!disc_ || !disc_->readData(readLba_, buf)) std::memset(buf, 0, sizeof buf);
      } else if (disc_) {
        uint8_t raw[cdrom::RAW_SECTOR] = {};
        if (disc_->readRaw(readLba_, raw)) {
          // 2352 = setor inteiro; 2340 = sem o sincronismo; 2336 = o que vem depois do cabeçalho
          const size_t from = sz == 2340 ? 12 : sz == 2336 ? 16 : 0;
          std::memcpy(buf, raw + from, std::min(sz, size_t(cdrom::RAW_SECTOR) - from));
        }
      }
      data_.insert(data_.end(), buf, buf + sz);
      readLba_++;
      readLeft_--;
      readTimer_ += doubleSpeed_ ? CYCLES_PER_SECTOR_2X : CYCLES_PER_SECTOR_2X * 2;
    }
  }
}

// 588 amostras estéreo de 16 bits (little-endian) por setor de áudio
void CdDrive::audioSample(int16_t& l, int16_t& r) {
  l = r = 0;
  if (!playing_ || !disc_ || door_) return;
  if (playLba_ >= playEnd_) { playing_ = false; return; }
  if (pcmLba_ != playLba_) {
    if (!disc_->readRaw(playLba_, pcm_)) std::memset(pcm_, 0, sizeof pcm_);
    pcmLba_ = playLba_;
  }
  const uint8_t* p = pcm_ + playPos_ * 4;
  l = int16_t(p[0] | p[1] << 8);
  r = int16_t(p[2] | p[3] << 8);
  if (++playPos_ >= cdrom::RAW_SECTOR / 4) { playPos_ = 0; playLba_++; }
}

namespace {
void syncDeque(StateIO& io, std::deque<uint8_t>& d, size_t max) {
  uint32_t n = uint32_t(d.size());
  io.sync(n);
  if (io.reading()) {
    if (n > max) throw std::runtime_error("Estado salvo danificado.");
    d.assign(n, 0);
  }
  for (auto& b : d) io.sync(b);
}
}  // namespace

void CdDrive::sync(StateIO& io) {
  io.raw(cmd_, sizeof cmd_);
  io.sync(cmdLen_);
  syncDeque(io, status_, 64);
  syncDeque(io, pendingStatus_, 64);
  syncDeque(io, data_, FIFO_MAX);
  io.sync(delay_);
  io.sync(spinning_); io.sync(door_); io.sync(doubleSpeed_); io.sync(success_); io.sync(playing_); io.sync(readDone_);
  io.sync(sectorSize_);
  io.sync(readLba_); io.sync(readLeft_); io.sync(readTimer_);
  io.sync(playLba_); io.sync(playEnd_); io.sync(playPos_);
  if (io.reading()) {
    pcmLba_ = 0xFFFFFFFF;
    if (playPos_ >= uint32_t(cdrom::RAW_SECTOR / 4)) playPos_ = 0;
    if (cmdLen_ < 0 || cmdLen_ > 6) cmdLen_ = 0;
    clampTo(delay_, int32_t(0), int32_t(REPLY_DELAY));
    clampTo(readTimer_, int32_t(-CYCLES_PER_SECTOR_2X * 2), int32_t(CYCLES_PER_SECTOR_2X * 2));
    if (sectorSize_ != 2048 && sectorSize_ != 2336 && sectorSize_ != 2340 && sectorSize_ != 2352 && sectorSize_ != 2448)
      sectorSize_ = 2048;
  }
}

}  // namespace indigo::tdo
