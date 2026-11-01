#include "cdunit.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::neogeocd {

namespace {
uint8_t bcd(unsigned v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }
unsigned unbcd(uint8_t v) { return (v >> 4) * 10u + (v & 15u); }
constexpr size_t FIFO_MAX = 44100 * 2;  // meio segundo de som (amostras intercaladas)
// tempo entre setores em ciclos do 68000 (12,084 MHz): 75 Hz lendo, 150 Hz lendo dados no CDZ, ~64,64 Hz parado
constexpr int DELAY_75 = 161120, DELAY_150 = 80560, DELAY_IDLE = 186943;
}  // namespace

void CdUnit::reset() {
  status_ = IDLE;
  pos_ = 0;
  playing_ = false;
  std::memset(cmd_, 0, sizeof cmd_);
  std::memset(resp_, 0, sizeof resp_);
  setChecksum(resp_);
  regPtr_ = 0;
  resetPointers();
  updateHead(0);
  sbout_ = ifctrl_ = dbcl_ = dbch_ = dacl_ = dach_ = dtrg_ = dtack_ = 0;
  wal_ = 0x30; wah_ = 0x09;  // 2352
  ctrl0_ = ctrl1_ = ptl_ = pth_ = 0;
  comin_ = 0;
  ifstat_ = 0xFF;
  std::memset(stat_, 0, sizeof stat_);
  std::memset(buffer_, 0, sizeof buffer_);
  fifo_.clear();
  frac_ = 0;
}

// ------------------------------------------------------------------ disco
bool CdUnit::isData() const {
  const int t = trackIdx();
  return t >= 0 && disc_.tracks()[static_cast<size_t>(t)].type != cdrom::TrackType::Audio;
}
bool CdUnit::isAudio() const {
  const int t = trackIdx();
  return t >= 0 && disc_.tracks()[static_cast<size_t>(t)].type == cdrom::TrackType::Audio;
}
int CdUnit::trackNumberAt() const {
  const int t = trackIdx();
  return t >= 0 ? disc_.tracks()[static_cast<size_t>(t)].number : 0;
}
bool CdUnit::pregap() const {
  const int t = trackIdx();
  return t >= 0 && pos_ < disc_.tracks()[static_cast<size_t>(t)].start;
}
uint32_t CdUnit::trackStart(int number) const {
  for (const auto& t : disc_.tracks())
    if (t.number == number) return t.start;
  return 0;
}
bool CdUnit::trackIsData(int number) const {
  for (const auto& t : disc_.tracks())
    if (t.number == number) return t.type != cdrom::TrackType::Audio;
  return false;
}

void CdUnit::updateHead(uint32_t lba) {
  uint8_t m, s, f;
  cdrom::lbaToMsf(lba, m, s, f);
  head_[0] = bcd(m); head_[1] = bcd(s); head_[2] = bcd(f); head_[3] = 0x01;
}

// ------------------------------------------------------------------ comunicação
uint8_t CdUnit::checksum(const uint8_t* p) {
  unsigned sum = 0;
  for (int i = 0; i < 4; i++) sum += (p[i] >> 4) + (p[i] & 15u);
  sum += (p[4] >> 4) + 5u;
  return static_cast<uint8_t>(~sum & 15u);
}

uint8_t CdUnit::readResponse() const {
  const uint8_t b = resp_[respPtr_ >> 1];
  return static_cast<uint8_t>(((respPtr_ & 1) ? (b & 15) : (b >> 4)) | (strobe_ << 4));
}

void CdUnit::writeCommand(uint8_t v) {
  uint8_t& b = cmd_[cmdPtr_ >> 1];
  b = (cmdPtr_ & 1) ? static_cast<uint8_t>((b & 0xF0) | (v & 15)) : static_cast<uint8_t>((b & 0x0F) | (v << 4));
}

void CdUnit::pointerStep(uint8_t v) {
  switch (v) {
    case 1:
      cmdPtr_ = (cmdPtr_ + 1) % 10;
      if (cmdPtr_ == 0) process();
      break;
    case 2:
      strobe_ = 0;
      respPtr_ = (respPtr_ + 1) % 10;
      break;
    case 3: strobe_ = 1; break;
    default: break;
  }
}

void CdUnit::driveReset() {
  playing_ = false;
  status_ = IDLE;
}

void CdUnit::process() {
  uint8_t* r = resp_;
  auto clear = [&](uint8_t first) { r[0] = first; r[1] = r[2] = r[3] = r[4] = 0; };
  if ((cmd_[4] & 15) != checksum(cmd_)) {  // soma errada: responde só com o estado
    clear(status_);
    setChecksum(r);
    return;
  }
  switch (cmd_[0]) {
    case 0x00: r[0] = static_cast<uint8_t>((r[0] & 0x0F) | status_); break;  // estado
    case 0x10:  // parar
      playing_ = false;
      status_ = IDLE;
      clear(status_);
      break;
    case 0x20: {  // consultas
      if (status_ == IDLE && !disc_.tracks().empty()) status_ = STOPPED;
      const uint8_t dataFlag = isData() ? 0x40 : 0x00;
      uint8_t m, s, f;
      switch (cmd_[1] & 15) {
        case 0x00:  // posição absoluta
          cdrom::lbaToMsf(pos_, m, s, f);
          r[0] = status_; r[1] = bcd(m); r[2] = bcd(s); r[3] = bcd(f); r[4] = dataFlag;
          break;
        case 0x01: {  // posição dentro da faixa (o pregap conta para trás)
          const int t = trackIdx();
          uint32_t rel = 0;
          if (t >= 0) {
            const auto& tr = disc_.tracks()[static_cast<size_t>(t)];
            rel = pregap() ? tr.start - (pos_ + 1) : pos_ - tr.start;
          }
          const uint32_t a = rel;  // MSF sem os 2 s iniciais
          r[0] = static_cast<uint8_t>(status_ | 0x01);
          r[1] = bcd(a / (60 * 75) % 100); r[2] = bcd(a / 75 % 60); r[3] = bcd(a % 75); r[4] = dataFlag;
          break;
        }
        case 0x02:  // faixa e índice atuais
          r[0] = static_cast<uint8_t>(status_ | 0x02);
          r[1] = bcd(static_cast<unsigned>(trackNumberAt()));
          r[2] = bcd(trackIdx() < 0 ? 0u : (pregap() ? 0u : 1u));
          r[3] = 0; r[4] = dataFlag;
          break;
        case 0x03:  // início da área final (lead-out)
          cdrom::lbaToMsf(disc_.totalSectors(), m, s, f);
          r[0] = static_cast<uint8_t>(status_ | 0x03); r[1] = bcd(m); r[2] = bcd(s); r[3] = bcd(f); r[4] = 0;
          break;
        case 0x04:  // primeira e última faixa
          r[0] = static_cast<uint8_t>(status_ | 0x04);
          r[1] = bcd(disc_.tracks().empty() ? 0u : static_cast<unsigned>(disc_.tracks().front().number));
          r[2] = bcd(disc_.tracks().empty() ? 0u : static_cast<unsigned>(disc_.tracks().back().number));
          r[3] = r[4] = 0;
          break;
        case 0x05: {  // início de uma faixa
          const int n = static_cast<int>(unbcd(cmd_[2]));
          cdrom::lbaToMsf(trackStart(n), m, s, f);
          r[0] = static_cast<uint8_t>(status_ | 0x05); r[1] = bcd(m); r[2] = bcd(s);
          r[3] = static_cast<uint8_t>(bcd(f) | (trackIsData(n) ? 0x80 : 0));
          r[4] = static_cast<uint8_t>(cmd_[2] << 4);
          break;
        }
        case 0x06:  // fim do disco?
          if (pos_ >= disc_.totalSectors()) status_ = END_OF_DISC;
          r[0] = static_cast<uint8_t>(status_ | 0x06); r[1] = r[2] = r[3] = 0; r[4] = dataFlag;
          break;
        case 0x07:  // reconhecimento do disco (CDZ): 2 é o que quase todos os jogos esperam
          r[0] = static_cast<uint8_t>(status_ | 0x07); r[1] = 0x02; r[2] = r[3] = r[4] = 0;
          break;
        default: clear(status_); break;
      }
      break;
    }
    case 0x30: {  // tocar a partir do MSF (BCD)
      const uint32_t lba = cdrom::msfToLba(static_cast<int>(unbcd(cmd_[1])), static_cast<int>(unbcd(cmd_[2])),
                                           static_cast<int>(unbcd(cmd_[3])));
      playing_ = true;
      seek(lba);
      status_ = PLAYING;
      r[0] = static_cast<uint8_t>(status_ | 0x02); r[1] = bcd(static_cast<unsigned>(trackNumberAt()));
      r[2] = r[3] = r[4] = 0;
      break;
    }
    case 0x40:  // posicionar: o NeoCD só para
      playing_ = false;
      status_ = PAUSED;
      clear(SEEKING);
      break;
    case 0x50: r[0] = status_; break;
    case 0x60: playing_ = false; status_ = PAUSED; r[0] = status_; break;
    case 0x70: playing_ = true; status_ = PLAYING; r[0] = status_; break;
    case 0x80: {  // avançar
      const uint32_t end = disc_.totalSectors() ? disc_.totalSectors() - 1 : 0;
      seek(std::min(pos_ + 30, end));
      status_ = PLAYING;
      r[0] = SCANNING;
      break;
    }
    case 0x90:  // voltar
      seek(pos_ < 30 ? 0 : pos_ - 30);
      status_ = PLAYING;
      r[0] = SCANNING;
      break;
    case 0xB0: {  // ir para a faixa
      playing_ = true;
      seek(trackStart(static_cast<int>(unbcd(cmd_[1]))));
      status_ = PLAYING;
      r[0] = static_cast<uint8_t>(status_ | 0x02); r[1] = bcd(static_cast<unsigned>(trackNumberAt()));
      r[2] = r[3] = r[4] = 0;
      break;
    }
    default: clear(status_); break;  // 02, 13, 23... (proteção do CDZ) e desconhecidos
  }
  setChecksum(r);
}

// ------------------------------------------------------------------ LC8951
uint8_t CdUnit::readRegister() {
  uint8_t v = 0;
  switch (regPtr_ & 15) {
    case 0x0: v = comin_; break;
    case 0x1: v = ifstat_; break;
    case 0x2: v = dbcl_; break;
    case 0x3: v = dbch_; break;
    case 0x4: case 0x5: case 0x6: case 0x7: v = (ctrl1_ & SHDREN) ? 0 : head_[regPtr_ & 3]; break;
    case 0x8: v = ptl_; break;
    case 0x9: v = pth_; break;
    case 0xA: v = wal_; break;
    case 0xB: v = wah_; break;
    case 0xC: case 0xD: case 0xE: v = stat_[regPtr_ & 3]; break;
    case 0xF:
      v = stat_[3];
      ifstat_ &= static_cast<uint8_t>(~DECI);  // como no NeoCD: ler STAT3 mexe na DECI
      break;
  }
  if (regPtr_) regPtr_ = static_cast<uint8_t>((regPtr_ & 0xF0) | ((regPtr_ + 1) & 15));
  return v;
}

void CdUnit::writeRegister(uint8_t v) {
  switch (regPtr_ & 15) {
    case 0x0: sbout_ = v; break;
    case 0x1: ifctrl_ = v; break;
    case 0x2: dbcl_ = v; break;
    case 0x3: dbch_ = v; break;
    case 0x4: dacl_ = v; break;
    case 0x5: dach_ = v; break;
    case 0x6:
      dtrg_ = v;
      if (ifctrl_ & DOUTEN) ifstat_ &= static_cast<uint8_t>(~DTBSY);  // começa a transferência
      break;
    case 0x7: dtack_ = v; ifstat_ |= DTEI; break;
    case 0x8: wal_ = v; break;
    case 0x9: wah_ = v; break;
    case 0xA: ctrl0_ = v; break;
    case 0xB: ctrl1_ = v; break;
    case 0xC: ptl_ = v; break;
    case 0xD: pth_ = v; break;
    default: break;  // CTRL2, RESET
  }
  if (regPtr_) regPtr_ = static_cast<uint8_t>((regPtr_ & 0xF0) | ((regPtr_ + 1) & 15));
}

void CdUnit::endTransfer() {
  ifstat_ |= DTBSY;
  const unsigned dbc = (dbch_ << 8) | dbcl_, dac = ((dach_ << 8) | dacl_) + dbc + 1;
  dacl_ = static_cast<uint8_t>(dac); dach_ = static_cast<uint8_t>(dac >> 8);
  dbcl_ = dbch_ = 0;
}

// ------------------------------------------------------------------ leitura
int CdUnit::delay(bool cdz) const {
  if (!playing_) return DELAY_IDLE;
  return (isData() && cdz) ? DELAY_150 : DELAY_75;
}

bool CdUnit::tick() {
  if (!playing_) return false;
  // como no NeoCD: a condição da interrupção é vista antes de decodificar o setor
  const bool irq = decoderEnabled() && decoderIrqEnabled() && decoderIrqPending();
  if (ctrl0_ & DECEN) {
    updateHead(pos_);
    if (isData()) {
      if (!disc_.readData(pos_, buffer_)) std::memset(buffer_, 0, sizeof buffer_);
      unsigned wa = ((wah_ << 8) | wal_) + 2352u, pt = ((pth_ << 8) | ptl_) + 2352u;
      wal_ = static_cast<uint8_t>(wa); wah_ = static_cast<uint8_t>(wa >> 8);
      ptl_ = static_cast<uint8_t>(pt); pth_ = static_cast<uint8_t>(pt >> 8);
      stat_[0] = 0x80;  // CRC certo
      stat_[1] = 0;
      stat_[2] = static_cast<uint8_t>((ctrl0_ & AUTORQ) ? (ctrl1_ & MODRQ) : (ctrl1_ & (MODRQ | FORMRQ)));
      stat_[3] = 0;
      ifstat_ &= static_cast<uint8_t>(~DECI);
    }
  }
  if (isAudio() && pos_ < disc_.totalSectors()) {
    uint8_t raw[cdrom::RAW_SECTOR];
    if (!disc_.readRaw(pos_, raw)) std::memset(raw, 0, sizeof raw);
    for (int i = 0; i < cdrom::RAW_SECTOR; i += 2) fifo_.push_back(static_cast<int16_t>(raw[i] | (raw[i + 1] << 8)));
    while (fifo_.size() > FIFO_MAX) fifo_.pop_front();
  }
  pos_++;
  return irq;
}

// ------------------------------------------------------------------ CD-DA
void CdUnit::produce(float* l, float* r, size_t n, double outRate) {
  const double step = 44100.0 / (outRate > 0 ? outRate : 48000.0);
  for (size_t i = 0; i < n; i++) {
    float a = 0, b = 0;
    if (fifo_.size() >= 4) {
      const float t = static_cast<float>(frac_);
      a = (fifo_[0] + (fifo_[2] - fifo_[0]) * t) / 32768.0f;
      b = (fifo_[1] + (fifo_[3] - fifo_[1]) * t) / 32768.0f;
    } else if (fifo_.size() >= 2) {
      a = fifo_[0] / 32768.0f;
      b = fifo_[1] / 32768.0f;
    }
    l[i] = a;
    r[i] = b;
    frac_ += step;
    while (frac_ >= 1.0) {
      if (fifo_.size() < 4) { frac_ = 0; if (!fifo_.empty()) fifo_.clear(); break; }
      fifo_.pop_front(); fifo_.pop_front();
      frac_ -= 1.0;
    }
  }
}

int16_t CdUnit::audioSample(int ch) const {
  return fifo_.size() >= 2 ? fifo_[static_cast<size_t>(ch & 1)] : 0;
}

// ------------------------------------------------------------------ estado
void CdUnit::sync(StateIO& io) {
  io.sync(status_); io.sync(pos_); io.sync(playing_);
  io.raw(cmd_, sizeof cmd_); io.raw(resp_, sizeof resp_);
  io.sync(cmdPtr_); io.sync(respPtr_); io.sync(strobe_);
  io.sync(regPtr_);
  io.sync(sbout_); io.sync(ifctrl_); io.sync(dbcl_); io.sync(dbch_); io.sync(dacl_); io.sync(dach_); io.sync(dtrg_);
  io.sync(dtack_); io.sync(wal_); io.sync(wah_); io.sync(ctrl0_); io.sync(ctrl1_); io.sync(ptl_); io.sync(pth_);
  io.sync(comin_); io.sync(ifstat_); io.raw(head_, sizeof head_); io.raw(stat_, sizeof stat_);
  io.raw(buffer_, sizeof buffer_);
  uint32_t n = static_cast<uint32_t>(fifo_.size());
  io.sync(n);
  if (io.reading()) {
    // ponteiros do pacote só vão de 0 a 9 e o strobe é um bit; a fila tem no máximo FIFO_MAX amostras
    if (cmdPtr_ >= 10 || respPtr_ >= 10 || strobe_ > 1 || n > FIFO_MAX || (n & 1))
      throw std::runtime_error("Estado salvo danificado.");
    std::vector<int16_t> v(n);
    if (n) io.raw(v.data(), n * sizeof(int16_t));
    fifo_.assign(v.begin(), v.end());
  } else {
    std::vector<int16_t> v(fifo_.begin(), fifo_.end());
    if (n) io.raw(v.data(), n * sizeof(int16_t));
  }
  io.sync(frac_);
  if (io.reading() && !(frac_ >= 0.0 && frac_ < 1.0)) throw std::runtime_error("Estado salvo danificado.");
}

}  // namespace indigo::neogeocd
