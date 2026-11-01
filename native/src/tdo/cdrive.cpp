// Unidade de CD do 3DO, ver cdrive.h.
#include "cdrive.h"

namespace indigo::tdo {

void CdDrive::reset() {
  cmdLen_ = 0;
  status_.clear();
  pendingStatus_.clear();
  data_.clear();
  delay_ = 0;
  spinning_ = disc_ != nullptr;
  door_ = false;
  doubleSpeed_ = true;
  error_ = false;
  readLba_ = readLeft_ = 0;
  readTimer_ = 0;
}

uint8_t CdDrive::stat() const {
  uint8_t s = 0;
  if (!door_) s |= ST_DOOR;
  if (disc_ && !door_) s |= ST_DISC;
  if (spinning_) s |= ST_SPIN;
  if (error_) s |= ST_ERR;
  if (doubleSpeed_) s |= ST_2X;
  if (spinning_ && disc_ && !door_) s |= ST_READY;
  return s;
}

void CdDrive::reply(std::initializer_list<uint8_t> bytes) {
  pendingStatus_.assign(bytes.begin(), bytes.end());
  pendingStatus_.push_back(stat());
  delay_ = REPLY_DELAY;
}

void CdDrive::writeCommand(uint8_t b) {
  if (cmdLen_ >= 7) return;
  cmd_[cmdLen_++] = b;
  if (cmdLen_ == 7) execute();
}

uint8_t CdDrive::readStatus() {
  if (status_.empty()) return 0;
  const uint8_t b = status_.front();
  status_.pop_front();
  return b;
}

uint8_t CdDrive::readData() {
  if (data_.empty()) return 0;
  const uint8_t b = data_.front();
  data_.pop_front();
  return b;
}

void CdDrive::execute() {
  const uint8_t c = cmd_[0];
  cmdLen_ = 0;
  error_ = false;
  status_.clear();
  const bool have = disc_ && !door_;
  switch (c) {
    case 0x01:  // busca
      if (!have) error_ = true;
      else spinning_ = true;
      reply({c});
      break;
    case 0x02: spinning_ = have; if (!have) error_ = true; reply({c}); break;
    case 0x03: spinning_ = false; readLeft_ = 0; reply({c}); break;
    case 0x04: reply({c, 0x00}); break;
    case 0x06: door_ = true; spinning_ = false; readLeft_ = 0; data_.clear(); reply({c}); break;
    case 0x07: door_ = false; spinning_ = disc_ != nullptr; reply({c}); break;
    case 0x08: readLeft_ = 0; reply({c}); break;
    case 0x09: doubleSpeed_ = cmd_[1] & 0x80; reply({c}); break;
    case 0x0A: readLeft_ = 0; data_.clear(); doubleSpeed_ = true; spinning_ = have; reply({c}); break;
    case 0x0B: data_.clear(); reply({c}); break;
    case 0x10: {
      const uint32_t lba = cdrom::msfToLba(cmd_[1], cmd_[2], cmd_[3]);
      const uint32_t n = uint32_t(cmd_[5]) << 8 | cmd_[6];
      if (!have || cmd_[1] > 99 || cmd_[2] > 59 || cmd_[3] > 74 || (cmd_[1] == 0 && cmd_[2] < 2) ||
          lba >= disc_->totalSectors()) {
        error_ = true;
        reply({c});
        break;
      }
      spinning_ = true;
      readLba_ = lba;
      readLeft_ = n;
      readTimer_ = doubleSpeed_ ? CYCLES_PER_SECTOR_2X : CYCLES_PER_SECTOR_2X * 2;
      reply({c});
      break;
    }
    case 0x83: reply({c, 0x00, 0x10, 0x00, 0x01, 0x00, 0x00}); break;  // fabricante/modelo/versão
    case 0x8B: {
      uint8_t m = 0, s = 2, f = 0;
      reply({c, 0x00, m, s, f, 0x00, 0x00});
      break;
    }
    case 0x8E: {
      if (!have) { error_ = true; reply({c}); break; }
      uint8_t m, s, f;
      cdrom::lbaToMsf(disc_->totalSectors(), m, s, f);
      const auto& t = disc_->tracks();
      const uint8_t first = t.empty() ? 1 : uint8_t(t.front().number), last = t.empty() ? 1 : uint8_t(t.back().number);
      reply({c, 0x00, first, last, m, s, f});
      break;
    }
    case 0x8F: {
      if (!have) { error_ = true; reply({c}); break; }
      const auto& t = disc_->tracks();
      const cdrom::Track* tr = nullptr;
      for (const auto& x : t) if (x.number == cmd_[1]) tr = &x;
      if (!tr) { error_ = true; reply({c}); break; }
      uint8_t m, s, f;
      cdrom::lbaToMsf(tr->start, m, s, f);
      const uint8_t ctl = tr->type == cdrom::TrackType::Audio ? 0x10 : 0x14;
      reply({c, 0x00, ctl, uint8_t(tr->number), 0x00, m, s, f});
      break;
    }
    default:
      error_ = true;
      reply({c});
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
  if (readLeft_ && delay_ <= 0) {
    readTimer_ -= cycles;
    while (readTimer_ <= 0 && readLeft_) {
      if (data_.size() + 2048 > FIFO_MAX) { readTimer_ = 0; break; }  // fila cheia: espera a CPU/DMA esvaziar
      uint8_t buf[2048];
      if (!disc_ || !disc_->readData(readLba_, buf)) {
        for (auto& b : buf) b = 0;
      }
      data_.insert(data_.end(), buf, buf + 2048);
      readLba_++;
      readLeft_--;
      readTimer_ += doubleSpeed_ ? CYCLES_PER_SECTOR_2X : CYCLES_PER_SECTOR_2X * 2;
    }
  }
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
  io.sync(spinning_); io.sync(door_); io.sync(doubleSpeed_); io.sync(error_);
  io.sync(readLba_); io.sync(readLeft_); io.sync(readTimer_);
  if (io.reading()) {
    if (cmdLen_ < 0 || cmdLen_ > 6) cmdLen_ = 0;
    clampTo(delay_, int32_t(0), int32_t(REPLY_DELAY));
    clampTo(readTimer_, int32_t(-CYCLES_PER_SECTOR_2X * 2), int32_t(CYCLES_PER_SECTOR_2X * 2));
  }
}

}  // namespace indigo::tdo
