#include "cdunit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::pcecd {

namespace {
constexpr int64_t CDDA_TICK = 487;                 // relógios mestres por amostra de CD-DA (~44,1 kHz)
constexpr int64_t SECTOR_CLOCKS = 286363;          // 2048 bytes a 153600 bytes/s (75 setores por segundo)
constexpr int64_t RETRY_CLOCKS = 6228409;          // ~290 ms: o programa não leu o setor anterior a tempo
constexpr int64_t T_SEL = 75000, T_ABORT = 900000, T_REQ = 3000, T_GOOD = 3000, T_TUR = 450000;
constexpr int64_t T_SUBQ = 3000, T_TOC = 9000, T_SENSE = 451, T_ACK = 21, T_DMA = 60, T_LATCH = 700;
constexpr int64_t TIME_SPAN = int64_t(1) << 36;    // nenhum evento válido fica tão longe no futuro
constexpr double CD_SCALE = 0.75 / 32768.0;        // CD-DA em fundo de escala = 0,75
constexpr double AD_SCALE = CD_SCALE * 10.0;       // ADPCM (12 bits) = 0,47 no máximo

uint8_t bcd(uint32_t v) { return uint8_t(((v / 10) % 10) << 4 | (v % 10)); }
uint32_t unbcd(uint8_t v) { return (v >> 4) * 10u + (v & 15u); }

// passos do MSM5205: 49 níveis de floor(16 * 1,1^n), 8 diferenças por nível
struct StepTable {
  int16_t v[49 * 8];
  StepTable() {
    for (int n = 0; n < 49; n++) {
      const int s = int(std::floor(16.0 * std::pow(11.0 / 10.0, n) + 1e-9));
      for (int k = 0; k < 8; k++) v[n * 8 + k] = int16_t(s / 8 + ((k & 4) ? s : 0) + ((k & 2) ? s / 2 : 0) + ((k & 1) ? s / 4 : 0));
    }
  }
};
const StepTable STEPS;
const int8_t STEP_SHIFT[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
}  // namespace

// Tempo de busca do leitor: adaptação do modelo medido por Dave Shadoff (github.com/pce-devel/PCECD_seek,
// licença MIT). O disco é dividido em 14 zonas com setores por volta e tempo de rotação diferentes.
double CdUnit::seekMs(uint32_t from, uint32_t to) {
  struct Zone { uint32_t spr, first, last; double rot; };
  static const Zone Z[14] = {
    {10, 0, 12572, 133.47}, {11, 12573, 30244, 146.82}, {12, 30245, 49523, 160.17}, {13, 49524, 70408, 173.51},
    {14, 70409, 92900, 186.86}, {15, 92901, 116998, 200.21}, {16, 116999, 142703, 213.56},
    {17, 142704, 170014, 226.90}, {18, 170015, 198932, 240.25}, {19, 198933, 229456, 253.60},
    {20, 229457, 261587, 266.95}, {21, 261588, 295324, 280.29}, {22, 295325, 330668, 293.64},
    {23, 330669, 333012, 306.99}};
  auto zone = [](uint32_t lba) {
    for (int i = 0; i < 14; i++) if (lba >= Z[i].first && lba <= Z[i].last) return i;
    return 0;
  };
  const int s = zone(from), d = zone(to);
  const uint32_t gap = from > to ? from - to : to - from;
  double tracks;
  if (d == s) tracks = double(gap / Z[d].spr);
  else if (d > s) tracks = double((Z[s].last - std::min(from, Z[s].last)) / Z[s].spr) + double((to - std::min(to, Z[d].first)) / Z[d].spr) + 1606.48 * (d - s - 1);
  else tracks = double((from - std::min(from, Z[s].first)) / Z[s].spr) + double((Z[d].last - std::min(to, Z[d].last)) / Z[d].spr) + 1606.48 * (s - d - 1);
  double ms;
  if (gap < 2) ms = 9.0 * 1000 / 60;
  else if (gap < 5) ms = 9.0 * 1000 / 60 + Z[d].rot / 2;
  else if (tracks <= 80) ms = 18.0 * 1000 / 60 + Z[d].rot / 2;
  else if (tracks <= 160) ms = 22.0 * 1000 / 60 + Z[d].rot / 2;
  else if (tracks <= 644) ms = 22.0 * 1000 / 60 + Z[d].rot / 2 + (tracks - 161) * 16.66 / 80;
  else ms = 48.0 * 1000 / 60 + (tracks - 644) * 16.66 / 195;
  return std::floor(ms);  // milissegundos inteiros, como no modelo original
}

int64_t CdUnit::seekClocks(uint32_t from, uint32_t to) const { return int64_t(MASTER_HZ * seekMs(from, to) / 1000.0); }

CdUnit::CdUnit(cdrom::Disc& disc, Model model, pce::HuC6280& cpu) : disc_(disc), model_(model), cpu_(cpu) {
  if (model_ != Model::CdRom2) superRam_.assign(0x30000, 0);
  arcade_ = model_ == Model::ArcadeCdRom2;
  if (arcade_) acRam_.assign(0x200000, 0);
  reset();
}

void CdUnit::reset() {
  now_ = 0;
  irqEnabled_ = irqActive_ = resetReg_ = 0;
  readRight_ = false; latch_ = 0; latchUntil_ = 0;
  sig_ = 0; phase_ = BUS_FREE; msgDone_ = false; dataPort_ = readPort_ = 0;
  sector_ = 0; sectorsLeft_ = 0; std::memset(cmd_, 0, sizeof cmd_); cmdLen_ = 0;
  std::memset(fifo_, 0, sizeof fifo_); fifoPos_ = fifoLen_ = 0;
  tUpdate_ = tSector_ = tAckClr_ = NONE; updType_ = U_CMD; changed_ = false;
  cdStatus_ = CD_INACTIVE; cdEnd_ = END_STOP; cdStart_ = cdStop_ = cdSector_ = 0; cdSample_ = 0; cdL_ = cdR_ = 0;
  tCdTick_ = CDDA_TICK; tPlayStart_ = NONE; subTick_ = subPos_ = 0;
  cdBufSector_ = -1;
  faderReg_ = 0; faderStart_ = 0;
  std::fill(adpcmRam_.begin(), adpcmRam_.end(), 0);
  adAddrPort_ = adReadAddr_ = adWriteAddr_ = 0; adLength_ = 0;
  adCtrl_ = adDma_ = adRate_ = adReadBuf_ = adWriteBuf_ = 0;
  adNibble_ = adPlaying_ = adPlayReq_ = adEnd_ = adHalf_ = false;
  adOut_ = 2048; adMag_ = 0;
  tAdSample_ = tAdRead_ = tAdWrite_ = tDma_ = NONE; adFrac_ = 0;
  std::fill(cdRam_.begin(), cdRam_.end(), 0);
  std::fill(superRam_.begin(), superRam_.end(), 0);
  std::fill(acRam_.begin(), acRam_.end(), 0);
  acUsed_ = false;
  for (auto& p : acPort_) p = AcPort{};
  acValue_ = 0; acShift_ = acRotate_ = 0;
  audioClk_ = 0; accL_ = accR_ = accN_ = 0;
  irqLine();
}

// ------------------------------------------------------------ memória
void CdUnit::mapBanks(pce::HuC6280& cpu) {
  for (int i = 0; i < 8; i++) cpu.rd[0x80 + i] = cpu.wr[0x80 + i] = cdRam_.data() + i * 0x2000;
  if (!superRam_.empty())
    for (int i = 0; i < 24; i++) cpu.rd[0x68 + i] = cpu.wr[0x68 + i] = superRam_.data() + i * 0x2000;
  if (arcade_)
    for (int i = 0; i < 4; i++) { cpu.rd[0x40 + i] = nullptr; cpu.wr[0x40 + i] = nullptr; }
}

uint8_t CdUnit::bankRead(uint8_t bank, uint16_t) {
  if (arcade_ && bank >= 0x40 && bank <= 0x43) return acPortRead(bank & 3);
  return 0xFF;
}

void CdUnit::bankWrite(uint8_t bank, uint16_t, uint8_t v) {
  if (arcade_ && bank >= 0x40 && bank <= 0x43) acPortWrite(bank & 3, v);
}

// ------------------------------------------------------------ interrupções
void CdUnit::irqLine() {
  const bool on = (irqEnabled_ & irqActive_) != 0;
  cpu_.irqActive = uint8_t((cpu_.irqActive & ~pce::HuC6280::IRQ2) | (on ? pce::HuC6280::IRQ2 : 0));
}

void CdUnit::irqSet(uint8_t src) {
  if (resetReg_ & 2) return;  // com o leitor em reset, nada liga
  if (!(irqActive_ & src)) { irqActive_ |= src; irqLine(); }
}

void CdUnit::irqClear(uint8_t src) {
  if (irqActive_ & src) { irqActive_ &= uint8_t(~src); irqLine(); }
}

// ------------------------------------------------------------ SCSI
void CdUnit::setSig(uint16_t s, bool on) {
  const uint16_t n = on ? uint16_t(sig_ | s) : uint16_t(sig_ & ~s);
  if (n != sig_) { sig_ = n; changed_ = true; }
}

void CdUnit::scsiIrq() {
  if ((sig_ & REQ) && (sig_ & IO) && (sig_ & BSY)) irqSet((sig_ & CD) ? IRQ_STATUS : IRQ_DATA);
  else { irqClear(IRQ_STATUS); irqClear(IRQ_DATA); }
}

void CdUnit::setPhase(uint8_t p) {
  if (phase_ == p) return;
  tUpdate_ = NONE;
  changed_ = true;
  phase_ = p;
  sig_ &= uint16_t(~(BSY | CD | IO | MSG | REQ));
  irqClear(IRQ_STATUS); irqClear(IRQ_DATA);
  switch (p) {
    case COMMAND: tSector_ = NONE; sig_ |= BSY | CD | REQ; break;
    case DATA_IN: sig_ |= BSY | IO; break;
    case MESSAGE_IN: sig_ |= BSY | CD | IO | MSG | REQ; break;
    case STATUS: sig_ |= BSY | CD | IO | REQ; break;
    case BUSY: sig_ |= BSY; break;
    default: break;
  }
  scsiIrq();
}

void CdUnit::scsiReset() {
  sig_ &= uint16_t(~(ACK | CD | IO | MSG | REQ));
  phase_ = BUS_FREE;
  dataPort_ = readPort_ = 0;
  sectorsLeft_ = 0;
  tSector_ = NONE;
  cmdLen_ = 0;
  fifoPos_ = fifoLen_ = 0;
  cdStatus_ = CD_STOPPED;
  scsiIrq();
}

void CdUnit::fifoSet(const uint8_t* p, int n) {
  n = std::clamp(n, 0, int(sizeof fifo_));
  std::memcpy(fifo_, p, size_t(n));
  fifoPos_ = 0;
  fifoLen_ = uint16_t(n);
}

void CdUnit::statusMessage(uint8_t status, uint8_t data, int len) {
  uint8_t b[8];
  std::memset(b, data, sizeof b);
  fifoSet(b, std::min(len, 8));
  msgDone_ = false;
  readPort_ = status;
  setPhase(STATUS);
}

// o mesmo pedido já em espera não é reiniciado
void CdUnit::queue(uint8_t type, int64_t delay) {
  if (type != updType_ || tUpdate_ == NONE) { updType_ = type; tUpdate_ = now_ + delay; }
}

void CdUnit::update() {
  if (!changed_) return;
  if (sig_ & RST) { scsiReset(); changed_ = false; return; }
  int guard = 64;
  do {
    changed_ = false;
    if (sig_ & SEL) {
      tUpdate_ = NONE;
      if (phase_ != DATA_IN) {
        queue(U_CMD, T_SEL);
      } else {
        // seleção no meio de uma leitura: status com 8 bytes e, bem depois, volta à fase de comando
        statusMessage(0, 0, 8);
        queue(U_CMD, T_ABORT);
        changed_ = false;
        break;
      }
    } else {
      switch (phase_) {
        case COMMAND: processCommand(); break;
        case DATA_IN: processDataIn(); break;
        case MESSAGE_IN: processMessageIn(); break;
        case STATUS: processStatus(); break;
        default: break;
      }
    }
  } while (changed_ && --guard > 0);
  changed_ = false;
}

void CdUnit::processStatus() {
  if ((sig_ & REQ) && (sig_ & ACK)) {
    setSig(REQ, false); scsiIrq();
  } else if (!(sig_ & REQ) && !(sig_ & ACK) && fifoPos_ < fifoLen_) {
    readPort_ = fifo_[fifoPos_++];
    if (fifoPos_ >= fifoLen_) { fifoPos_ = fifoLen_ = 0; setPhase(MESSAGE_IN); }
    else { setSig(REQ, true); scsiIrq(); }
  }
}

void CdUnit::processMessageIn() {
  if ((sig_ & REQ) && (sig_ & ACK)) {
    setSig(REQ, false); scsiIrq();
    msgDone_ = true;
  } else if (!(sig_ & REQ) && !(sig_ & ACK) && msgDone_) {
    msgDone_ = false;
    setPhase(BUS_FREE);
  }
}

void CdUnit::processDataIn() {
  if ((sig_ & REQ) && (sig_ & ACK)) {
    setSig(REQ, false); scsiIrq();
  } else if (!(sig_ & REQ) && !(sig_ & ACK)) {
    if (fifoPos_ < fifoLen_) {
      readPort_ = fifo_[fifoPos_++];
      if (fifoPos_ >= fifoLen_) fifoPos_ = fifoLen_ = 0;
      setSig(REQ, true); scsiIrq();
    } else if (sectorsLeft_ == 0) {
      queue(U_GOOD, T_GOOD);  // fim da transferência: status depois de um tempo (e a IRQ)
    }
  }
}

namespace {
int commandSize(uint8_t c) {
  switch (c) {
    case 0x00: case 0x03: case 0x08: return 6;
    case 0xD8: case 0xD9: case 0xDA: case 0xDD: case 0xDE: return 10;
    default: return 0;
  }
}
}  // namespace

void CdUnit::processCommand() {
  if ((sig_ & REQ) && (sig_ & ACK)) {
    setSig(REQ, false); scsiIrq();
    if (cmdLen_ < sizeof cmd_) cmd_[cmdLen_++] = dataPort_;
  } else if (!(sig_ & REQ) && !(sig_ & ACK) && cmdLen_ > 0) {
    const int size = commandSize(cmd_[0]);
    if (size == 0) {  // comando desconhecido: status bom logo
      statusMessage(0, 0);
      cmdLen_ = 0;
    } else if (cmdLen_ >= size) {
      execCommand();
      cmdLen_ = 0;
    } else {
      queue(U_REQ, T_REQ);  // pede o próximo byte
    }
  }
}

uint32_t CdUnit::audioLba() const {
  switch (cmd_[9] & 0xC0) {
    case 0x00: return uint32_t(cmd_[3]) << 16 | uint32_t(cmd_[4]) << 8 | cmd_[5];
    case 0x40: return uint32_t((unbcd(cmd_[2]) * 60 + unbcd(cmd_[3])) * 75 + unbcd(cmd_[4])) - 150u;
    case 0x80: {
      const uint32_t n = unbcd(cmd_[2]);
      for (const auto& t : disc_.tracks()) if (uint32_t(t.number) == n) return t.start;
      return 0;
    }
    default: return 0;
  }
}

void CdUnit::execCommand() {
  const auto& tracks = disc_.tracks();
  const uint32_t total = disc_.totalSectors();
  const uint8_t op = cmd_[0];
  if (op != 0x08) sectorsLeft_ = 0;
  switch (op) {
    case 0x00:  // TEST UNIT READY
      queue(U_GOOD, T_TUR);
      break;
    case 0x03: {  // REQUEST SENSE: nunca há erro pendente
      const int n = std::min<int>(cmd_[4], 18);
      if (n == 0) { queue(U_GOOD, T_SENSE); break; }
      uint8_t s[18] = {0x70, 0, 0, 0, 0, 0, 0, 0x0A};
      fifoSet(s, n);
      queue(U_DATA_IN, T_SENSE);
      break;
    }
    case 0x08: {  // READ(6)
      const uint32_t lba = uint32_t(cmd_[1] & 0x1F) << 16 | uint32_t(cmd_[2]) << 8 | cmd_[3];
      const uint16_t count = cmd_[4] ? cmd_[4] : 256;
      if (lba >= total) { statusMessage(0, 0); break; }
      const uint32_t from = currentSector();
      tSector_ = now_ + seekClocks(from, lba) + SECTOR_CLOCKS;
      sector_ = lba;
      sectorsLeft_ = count;
      fifoPos_ = fifoLen_ = 0;
      setPhase(DATA_IN);  // já entra na fase de dados (sem REQ até o primeiro setor chegar)
      cdStatus_ = CD_INACTIVE;
      break;
    }
    case 0xD8: {  // AUDIO START POSITION
      const uint32_t lba = audioLba();
      if (lba >= total || disc_.trackAt(lba) < 0) { statusMessage(0, 0); break; }
      cdPlay(lba, cmd_[1] == 0);
      setPhase(BUSY);  // o status vem quando a busca terminar
      break;
    }
    case 0xD9: {  // AUDIO STOP POSITION
      uint32_t lba = audioLba();
      if (total && lba >= total) lba = total - 1;
      switch (cmd_[1]) {
        case 0: cdStatus_ = CD_STOPPED; break;
        case 1: cdStop_ = lba; cdEnd_ = END_LOOP; cdStatus_ = CD_PLAYING; break;
        case 2: cdStop_ = lba; cdEnd_ = END_IRQ; cdStatus_ = CD_PLAYING; break;
        case 3: cdStop_ = lba; cdEnd_ = END_STOP; cdStatus_ = CD_PLAYING; break;
        default: break;
      }
      if (cmd_[1] == 1 || cmd_[1] == 2) setPhase(BUSY);
      else statusMessage(0, 0);
      break;
    }
    case 0xDA:  // PAUSE
      cdStatus_ = CD_PAUSED;
      statusMessage(0, 0);
      break;
    case 0xDD: {  // READ SUBCODE Q
      const uint32_t s = currentSector();
      const int ti = disc_.trackAt(s);
      uint8_t q[10] = {cdStatus_, 0x01, 0, 1, 0, 0, 0, 0, 0, 0};
      if (ti >= 0) {
        const auto& t = tracks[size_t(ti)];
        if (t.type != cdrom::TrackType::Audio) q[1] = 0x41;
        q[2] = bcd(uint32_t(t.number));
        const uint32_t rel = s >= t.start ? s - t.start : t.start - s;
        if (s < t.start) q[3] = 0;  // pregap: índice 0, contando para trás
        q[4] = bcd(rel / 4500); q[5] = bcd(rel / 75 % 60); q[6] = bcd(rel % 75);  // tempo na faixa
        uint8_t m, sec, f;
        cdrom::lbaToMsf(s, m, sec, f);  // tempo absoluto (com os 2 segundos iniciais)
        q[7] = bcd(m); q[8] = bcd(sec); q[9] = bcd(f);
      }
      fifoSet(q, 10);
      queue(U_DATA_IN, T_SUBQ);
      break;
    }
    case 0xDE: {  // READ TOC
      uint8_t d[4] = {0, 0, 0, 0};
      uint8_t m, s, f;
      const int last = tracks.empty() ? 1 : tracks.back().number;
      switch (cmd_[1]) {
        case 0: d[0] = 0x01; d[1] = bcd(uint32_t(last)); break;
        case 1: cdrom::lbaToMsf(total, m, s, f); d[0] = bcd(m); d[1] = bcd(s); d[2] = bcd(f); break;
        case 2: {
          int n = int(unbcd(cmd_[2]));
          if (n == 0) n = 1;
          const cdrom::Track* t = nullptr;
          for (const auto& x : tracks) if (x.number == n) { t = &x; break; }
          cdrom::lbaToMsf(t ? t->start : total, m, s, f);
          d[0] = bcd(m); d[1] = bcd(s); d[2] = bcd(f);
          d[3] = (t && t->type != cdrom::TrackType::Audio) ? 4 : 0;
          break;
        }
        default: statusMessage(0, 0); return;
      }
      fifoSet(d, 4);
      queue(U_DATA_IN, T_TOC);
      break;
    }
    default: statusMessage(0, 0); break;
  }
}

uint8_t CdUnit::dataRead() const {
  return (phase_ == STATUS || phase_ == DATA_IN || phase_ == MESSAGE_IN) ? readPort_ : dataPort_;
}

void CdUnit::ackAutoClear() {
  setSig(ACK, true);
  tAckClr_ = now_ + T_ACK;
}

// ------------------------------------------------------------ CD-DA
void CdUnit::cdPlay(uint32_t start, bool pause) {
  const int ti = disc_.trackAt(start);
  if (ti < 0) return;
  const auto& t = disc_.tracks()[size_t(ti)];
  tPlayStart_ = now_ + std::max<int64_t>(1, seekClocks(currentSector(), start));
  cdStart_ = start;
  cdStatus_ = pause ? CD_PAUSED : CD_PLAYING;
  cdStop_ = t.length ? t.start + t.length - 1 : t.start;
  cdEnd_ = END_STOP;
  cdSample_ = 0;
  cdSector_ = start;
  tCdTick_ = now_ + CDDA_TICK;
}

void CdUnit::cdSampleOut() {
  if (cdStatus_ != CD_PLAYING) { cdL_ = cdR_ = 0; return; }
  if (cdBufSector_ != int64_t(cdSector_)) {
    if (!disc_.readRaw(cdSector_, cdBuf_.data())) std::fill(cdBuf_.begin(), cdBuf_.end(), 0);
    cdBufSector_ = cdSector_;
  }
  const uint8_t* p = cdBuf_.data() + cdSample_ * 4;
  cdL_ = int16_t(uint16_t(p[0] | p[1] << 8));
  cdR_ = int16_t(uint16_t(p[2] | p[3] << 8));
  if (++cdSample_ >= 588) {
    cdSample_ = 0;
    cdSector_++;
    if (cdSector_ > cdStop_) {
      const uint32_t total = disc_.totalSectors();
      if (total && cdSector_ >= total) cdSector_ = total - 1;
      switch (cdEnd_) {
        case END_LOOP: cdSector_ = cdStart_; break;
        case END_IRQ: cdStatus_ = CD_STOPPED; statusMessage(0, 0); break;
        default: cdStatus_ = CD_STOPPED; break;
      }
    }
  }
}

void CdUnit::cdTick() {
  tCdTick_ += CDDA_TICK;
  if (++subTick_ >= 6) {  // um byte de subcódigo a cada 6 amostras (98 por setor), com IRQ
    subTick_ = 0;
    irqSet(IRQ_SUBCODE);
    if (++subPos_ >= 98) subPos_ = 0;
  }
  if (tPlayStart_ == NONE) cdSampleOut();
}

// ------------------------------------------------------------ fader e som
double CdUnit::faderVol(bool adpcm, int64_t t) const {
  if (!(faderReg_ & 0x08) || ((faderReg_ & 0x02) != 0) != adpcm) return 1.0;
  const double perPercent = ((faderReg_ & 0x04) ? 0.025 : 0.06) * MASTER_HZ;  // relógios para baixar 1%
  const double v = 1.0 - (double(t) - double(faderStart_)) / perPercent / 100.0;
  return std::clamp(v, 0.0, 1.0);
}

void CdUnit::setSampleRate(int rate) {
  hostRate_ = rate > 0 ? rate : 48000;
  hostPeriod_ = MASTER_HZ / hostRate_;
  aw_ = ar_ = 0;
  accL_ = accR_ = accN_ = 0;
}

void CdUnit::emit(double l, double r) {
  abL_[aw_] = float(std::clamp(l, -1.0, 1.0));
  abR_[aw_] = float(std::clamp(r, -1.0, 1.0));
  aw_ = (aw_ + 1) % ABUF;
  if (aw_ == ar_) ar_ = (ar_ + 1) % ABUF;
}

void CdUnit::pull(float* l, float* r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (ar_ == aw_) { l[i] = r[i] = 0; continue; }
    l[i] = abL_[ar_];
    r[i] = abR_[ar_];
    ar_ = (ar_ + 1) % ABUF;
  }
}

void CdUnit::advanceAudio(int64_t t) {
  if (t <= audioClk_) return;
  const double vc = faderVol(false, audioClk_) * CD_SCALE;
  const double ad = adPlaying_ ? double(adOut_ - 2048) * faderVol(true, audioClk_) * AD_SCALE : 0.0;
  const double l = cdL_ * vc + ad, r = cdR_ * vc + ad;
  double rem = double(t - audioClk_);
  audioClk_ = t;
  while (rem > 0) {
    const double need = std::max(0.0, hostPeriod_ - accN_);
    if (rem < need) { accL_ += l * rem; accR_ += r * rem; accN_ += rem; break; }
    accL_ += l * need; accR_ += r * need;
    emit(accL_ / hostPeriod_, accR_ / hostPeriod_);
    accL_ = accR_ = accN_ = 0;
    rem -= need;
  }
}

// ------------------------------------------------------------ ADPCM
void CdUnit::adSetHalf(bool v) {
  if (adHalf_ == v) return;
  adHalf_ = v;
  if (v) irqSet(IRQ_ADPCM_HALF); else irqClear(IRQ_ADPCM_HALF);
}

void CdUnit::adSetEnd(bool v) {
  if (adEnd_ == v) return;
  adEnd_ = v;
  if (v) irqSet(IRQ_ADPCM_END); else irqClear(IRQ_ADPCM_END);
}

void CdUnit::adReset() {
  adPlayReq_ = false;
  tAdWrite_ = tAdRead_ = NONE;
  adReadAddr_ = adWriteAddr_ = adAddrPort_ = 0;
  adSetEnd(false);
  adSetHalf(false);
  adLength_ = 0;
  adNibble_ = false;
  adOut_ = 2048;
  adMag_ = 0;
}

// estado contínuo do registro de controle ($180D): bit 4 copia o endereço para o comprimento, bit 7 segura
// o ADPCM em reset
void CdUnit::adFlags() {
  if (adCtrl_ & 0x10) { adLength_ = adAddrPort_; adSetEnd(false); }
  if (adCtrl_ & 0x80) adReset();
  adTimer();
}

int64_t CdUnit::adPeriodNext() {
  const uint64_t num = uint64_t(16 - (adRate_ & 15)) * 236250000u;
  int64_t q = int64_t(num / 352000u);
  adFrac_ += uint32_t(num % 352000u);
  if (adFrac_ >= 352000u) { adFrac_ -= 352000u; q++; }
  return q;
}

void CdUnit::adTimer() {
  const bool active = adPlaying_ || adPlayReq_ || (adCtrl_ & 0x20);
  if (!active) tAdSample_ = NONE;
  else if (tAdSample_ == NONE) tAdSample_ = now_ + adPeriodNext();
}

void CdUnit::adControl(uint8_t v) {
  if ((v & 0x02) && !(adCtrl_ & 0x02)) adWriteAddr_ = uint16_t(adAddrPort_ - ((v & 0x01) ? 0 : 1));
  if ((v & 0x08) && !(adCtrl_ & 0x08)) adReadAddr_ = uint16_t(adAddrPort_ - ((v & 0x04) ? 0 : 1));
  if (!adPlaying_ && (v & 0x20)) adPlayReq_ = true;
  adCtrl_ = v;
  adFlags();
}

// a RAM do ADPCM (DRAM) é acessada em janelas de 18 relógios: renovação, escrita, escrita, leitura
int64_t CdUnit::adSlot(bool forRead) const {
  static const uint8_t RD[4] = {3, 2, 1, 4}, WR[4] = {1, 1, 3, 2};
  const int64_t idx = now_ / 18;
  const int type = int(idx & 3);
  const int64_t next = (idx + (forRead ? RD[type] : WR[type])) * 18;
  return now_ + std::max<int64_t>(3, (next - now_) / 3 * 3);
}

void CdUnit::adSample() {
  if (adCtrl_ & 0x80) { adPlaying_ = (adCtrl_ & 0x20) != 0; adPlayReq_ = false; return; }
  if (((adCtrl_ & 0x40) && adLength_ == 0) || !(adCtrl_ & 0x20) || (!adPlaying_ && !adPlayReq_)) {
    adPlayReq_ = false;
    adPlaying_ = false;
    return;
  }
  if (adPlayReq_) { adPlayReq_ = false; adPlaying_ = true; adOut_ = 2048; adMag_ = 0; }
  uint8_t d;
  if (adNibble_) {
    adNibble_ = false;
    d = adpcmRam_[adReadAddr_] & 15;
    adReadAddr_++;
    adLength_ = (adLength_ - 1) & 0x1FFFF;
  } else {
    adNibble_ = true;
    d = adpcmRam_[adReadAddr_] >> 4;
  }
  const int diff = STEPS.v[adMag_ * 8 + (d & 7)];
  adOut_ = int16_t(std::clamp(adOut_ + ((d & 8) ? -diff : diff), 0, 4095));
  adMag_ = uint8_t(std::clamp(int(adMag_) + STEP_SHIFT[d & 7], 0, 48));
  adSetHalf(adLength_ <= 0x8000);
  if (adLength_ == 0) adSetEnd(true);
}

void CdUnit::adDmaCheck() {
  if ((adDma_ & 3) && tDma_ == NONE && !(sig_ & ACK) && !(sig_ & CD) && (sig_ & IO) && (sig_ & REQ)) tDma_ = now_ + T_DMA;
}

// ------------------------------------------------------------ Arcade Card
uint32_t CdUnit::acAddr(const AcPort& p) const {
  uint32_t a = p.base;
  if (p.ctrl & 0x02) {
    a += p.offset;
    if (p.ctrl & 0x08) a += 0xFF0000;  // deslocamento "com sinal"
  }
  return a & 0x1FFFFF;
}

void CdUnit::acAutoInc(AcPort& p) {
  if (!(p.ctrl & 0x01)) return;
  if (p.ctrl & 0x10) p.base = (p.base + p.inc) & 0xFFFFFF;
  else p.offset = uint16_t(p.offset + p.inc);
}

void CdUnit::acAddOffset(AcPort& p) {
  uint32_t a = p.base + p.offset;
  if (p.ctrl & 0x08) a += 0xFF0000;
  p.base = a & 0xFFFFFF;
}

uint8_t CdUnit::acPortRead(int n) {
  AcPort& p = acPort_[n];
  const uint32_t a = acAddr(p);
  acAutoInc(p);
  return acRam_[a];
}

void CdUnit::acPortWrite(int n, uint8_t v) {
  AcPort& p = acPort_[n];
  const uint32_t a = acAddr(p);
  acAutoInc(p);
  acRam_[a] = v;
  acUsed_ = true;
}

uint8_t CdUnit::acRead(uint16_t off) {
  if (off < 0x1A80) {
    const int n = (off >> 4) & 3;
    const AcPort& p = acPort_[n];
    switch (off & 15) {
      case 0: case 1: return acPortRead(n);
      case 2: return uint8_t(p.base);
      case 3: return uint8_t(p.base >> 8);
      case 4: return uint8_t(p.base >> 16);
      case 5: return uint8_t(p.offset);
      case 6: return uint8_t(p.offset >> 8);
      case 7: return uint8_t(p.inc);
      case 8: return uint8_t(p.inc >> 8);
      case 9: return p.ctrl;
      case 10: return 0;
      default: return 0xFF;
    }
  }
  switch (off) {
    case 0x1AE0: return uint8_t(acValue_);
    case 0x1AE1: return uint8_t(acValue_ >> 8);
    case 0x1AE2: return uint8_t(acValue_ >> 16);
    case 0x1AE3: return uint8_t(acValue_ >> 24);
    case 0x1AE4: return acShift_;
    case 0x1AE5: return acRotate_;
    case 0x1AFE: return 0x10;  // versão
    case 0x1AFF: return 0x51;  // assinatura do Arcade Card
    default: return 0xFF;
  }
}

void CdUnit::acWrite(uint16_t off, uint8_t v) {
  if (off < 0x1A80) {
    const int n = (off >> 4) & 3;
    AcPort& p = acPort_[n];
    const int trig = (p.ctrl >> 5) & 3;  // quando somar o deslocamento à base
    switch (off & 15) {
      case 0: case 1: acPortWrite(n, v); break;
      case 2: p.base = (p.base & 0xFFFF00) | v; break;
      case 3: p.base = (p.base & 0xFF00FF) | uint32_t(v) << 8; break;
      case 4: p.base = (p.base & 0x00FFFF) | uint32_t(v) << 16; break;
      case 5: p.offset = uint16_t((p.offset & 0xFF00) | v); if (trig == 1) acAddOffset(p); break;
      case 6: p.offset = uint16_t((p.offset & 0x00FF) | v << 8); if (trig == 2) acAddOffset(p); break;
      case 7: p.inc = uint16_t((p.inc & 0xFF00) | v); break;
      case 8: p.inc = uint16_t((p.inc & 0x00FF) | v << 8); break;
      case 9: p.ctrl = v & 0x7F; break;
      case 10: if (trig == 3) acAddOffset(p); break;
      default: break;
    }
    return;
  }
  switch (off) {
    case 0x1AE0: acValue_ = (acValue_ & 0xFFFFFF00u) | v; break;
    case 0x1AE1: acValue_ = (acValue_ & 0xFFFF00FFu) | uint32_t(v) << 8; break;
    case 0x1AE2: acValue_ = (acValue_ & 0xFF00FFFFu) | uint32_t(v) << 16; break;
    case 0x1AE3: acValue_ = (acValue_ & 0x00FFFFFFu) | uint32_t(v) << 24; break;
    case 0x1AE4:  // deslocamento: bit 3 = para a direita ((~v & 7) + 1 bits), senão para a esquerda (v & 7)
      acShift_ = v;
      if (v) acValue_ = (v & 8) ? acValue_ >> ((~v & 7) + 1) : acValue_ << (v & 7);
      break;
    case 0x1AE5:  // rotação, com a mesma codificação
      acRotate_ = v;
      if (v) {
        if (v & 8) { const int s = (~v & 7) + 1; acValue_ = (acValue_ >> s) | (acValue_ << (32 - s)); }
        else { const int s = v & 7; if (s) acValue_ = (acValue_ << s) | (acValue_ >> (32 - s)); }
      }
      break;
    default: break;
  }
}

// ------------------------------------------------------------ registradores
uint8_t CdUnit::ioRead(uint16_t off) {
  if (off >= 0x1A00) return arcade_ ? acRead(off) : 0xFF;
  if ((off & 0x3FC) == 0xC0) {  // $18C0-$18C3: assinatura do Super System Card
    static const uint8_t SIG[4] = {0x00, 0xAA, 0x55, 0x03};
    return model_ != Model::CdRom2 ? SIG[off & 3] : 0xFF;
  }
  switch (off & 0x3FF) {
    case 0x00:
      return uint8_t(((sig_ & IO) ? 0x08 : 0) | ((sig_ & CD) ? 0x10 : 0) | ((sig_ & MSG) ? 0x20 : 0) |
                     ((sig_ & REQ) ? 0x40 : 0) | ((sig_ & BSY) ? 0x80 : 0));
    case 0x01: return dataRead();
    case 0x02: return uint8_t(irqEnabled_ | ((sig_ & ACK) ? 0x80 : 0));
    case 0x03: return uint8_t(irqActive_ | (readRight_ ? 0 : 0x02));
    case 0x04: return resetReg_;
    case 0x05: return uint8_t(latch_);
    case 0x06: return uint8_t(uint16_t(latch_) >> 8);
    case 0x07: irqClear(IRQ_SUBCODE); return 0;  // subcódigo (as imagens não trazem: zero)
    case 0x08: {  // dado com ACK automático (leitura rápida)
      const uint8_t v = dataRead();
      if ((sig_ & REQ) && (sig_ & IO) && !(sig_ & CD)) { ackAutoClear(); update(); adDmaCheck(); }
      return v;
    }
    case 0x0A: tAdRead_ = adSlot(true); return adReadBuf_;
    case 0x0B: return adDma_;
    case 0x0C:
      return uint8_t((adEnd_ ? 0x01 : 0) | (tAdWrite_ != NONE ? 0x04 : 0) | (adPlaying_ ? 0x08 : 0) | (tAdRead_ != NONE ? 0x80 : 0));
    case 0x0D: return adCtrl_;
    case 0x0E: return adRate_;
    case 0x09: return 0;
    case 0x0F: return faderReg_;
    default: return 0xFF;
  }
}

void CdUnit::ioWrite(uint16_t off, uint8_t v) {
  if (off >= 0x1A00) { if (arcade_) acWrite(off, v); return; }
  switch (off & 0x3FF) {
    case 0x00:  // seleção do leitor
      setSig(SEL, true); update();
      setSig(SEL, false); update();
      break;
    case 0x01: dataPort_ = v; break;
    case 0x02:
      setSig(ACK, (v & 0x80) != 0); update();
      irqEnabled_ = v & 0x7F;
      irqLine();
      break;
    case 0x04:
      resetReg_ = v & 0x0F;
      setSig(RST, (v & 0x02) != 0); update();
      if (v & 0x02) { irqEnabled_ &= 0x8F; irqLine(); }
      break;
    case 0x05:  // troca o canal (esquerdo/direito) e guarda a amostra atual do CD-DA
      if (now_ >= latchUntil_) {
        readRight_ = !readRight_;
        latch_ = readRight_ ? cdR_ : cdL_;
        latchUntil_ = now_ + T_LATCH;
      }
      break;
    case 0x08: adAddrPort_ = uint16_t((adAddrPort_ & 0xFF00) | v); adFlags(); break;
    case 0x09: adAddrPort_ = uint16_t((adAddrPort_ & 0x00FF) | v << 8); adFlags(); break;
    case 0x0A: adWriteBuf_ = v; tAdWrite_ = adSlot(false); break;
    case 0x0B:
      if (fifoPos_ >= fifoLen_) v &= uint8_t(~1);  // o bit 0 só liga com um setor em transferência
      adDma_ = v;
      break;
    case 0x0D: adControl(v); break;
    case 0x0E: adRate_ = v; break;
    case 0x0F: faderReg_ = v; faderStart_ = now_; break;
    default: break;
  }
  adDmaCheck();
}

// ------------------------------------------------------------ tempo
int64_t CdUnit::nextEvent() const {
  int64_t e = std::min({tUpdate_, tSector_, tAckClr_, tPlayStart_, tAdSample_, tAdRead_, tAdWrite_, tDma_});
  // as amostras do CD-DA não precisam acordar a CPU; a IRQ do subcódigo sim (a cada 6 amostras)
  return std::min(e, tCdTick_ + int64_t(5 - std::min<int>(subTick_, 5)) * CDDA_TICK);
}

void CdUnit::runTo(int64_t t) {
  for (;;) {
    const int64_t e = std::min({tUpdate_, tSector_, tAckClr_, tPlayStart_, tAdSample_, tAdRead_, tAdWrite_, tDma_, tCdTick_});
    if (e > t) break;
    advanceAudio(e);
    now_ = e;
    if (tAckClr_ <= e) { tAckClr_ = NONE; setSig(ACK, false); update(); }
    if (tUpdate_ <= e) {
      tUpdate_ = NONE;
      switch (updType_) {
        case U_CMD: setPhase(COMMAND); break;
        case U_REQ: setSig(REQ, true); scsiIrq(); break;
        case U_DATA_IN: setPhase(DATA_IN); break;
        default: statusMessage(0, 0); break;
      }
      changed_ = true;
      update();
    }
    if (tSector_ <= e) {
      if (fifoPos_ >= fifoLen_) {
        uint8_t buf[cdrom::DATA_SECTOR];
        disc_.readData(sector_, buf);
        fifoSet(buf, cdrom::DATA_SECTOR);
        sector_ = (sector_ + 1) & 0x1FFFFF;
        if (sectorsLeft_) sectorsLeft_--;
        tSector_ = sectorsLeft_ ? e + SECTOR_CLOCKS : NONE;
        setPhase(DATA_IN);
        changed_ = true;
        update();
      } else {
        tSector_ = e + RETRY_CLOCKS;  // o setor anterior ainda não foi lido: o leitor busca de novo
      }
    }
    if (tPlayStart_ <= e) { tPlayStart_ = NONE; statusMessage(0, 0); update(); }
    if (tAdSample_ <= e) {
      tAdSample_ = NONE;
      adSample();
      adFlags();
      if (tAdSample_ == NONE && (adPlaying_ || adPlayReq_ || (adCtrl_ & 0x20))) tAdSample_ = e + adPeriodNext();
    }
    if (tAdRead_ <= e) {
      tAdRead_ = NONE;
      adReadBuf_ = adpcmRam_[adReadAddr_++];
      if (!(adCtrl_ & 0x10)) {
        if (adLength_ > 0) { adLength_--; adSetHalf(adLength_ < 0x8000); }
        else { adSetEnd(true); adSetHalf(false); }
      }
      adFlags();
    }
    if (tAdWrite_ <= e) {
      tAdWrite_ = NONE;
      adpcmRam_[adWriteAddr_++] = adWriteBuf_;
      if (adLength_ == 0) adSetEnd(true);
      adSetHalf(adLength_ < 0x8000);
      if (!(adCtrl_ & 0x10)) adLength_ = (adLength_ + 1) & 0x1FFFF;
      adFlags();
    }
    if (tDma_ <= e) {
      if (tAdWrite_ != NONE) {
        tDma_ = e + 3;  // a escrita anterior ainda não terminou
      } else {
        tDma_ = NONE;
        tAdWrite_ = adSlot(false);
        adWriteBuf_ = dataRead();
        const bool more = fifoPos_ < fifoLen_;
        ackAutoClear();
        update();
        if (!more) adDma_ &= uint8_t(~1);
      }
    }
    if (tCdTick_ <= e) cdTick();
    adDmaCheck();
  }
  advanceAudio(t);
  if (t > now_) now_ = t;
}

// ------------------------------------------------------------ save state
void CdUnit::sync(StateIO& io) {
  uint8_t model = uint8_t(model_);
  uint32_t total = disc_.totalSectors(), ntracks = uint32_t(disc_.tracks().size());
  io.sync(model); io.sync(total); io.sync(ntracks);
  if (io.reading() && (model != uint8_t(model_) || total != disc_.totalSectors() || ntracks != disc_.tracks().size()))
    throw std::runtime_error("Estado salvo de outro disco ou de outro System Card.");
  io.sync(now_);
  io.sync(irqEnabled_); io.sync(irqActive_); io.sync(resetReg_); io.sync(readRight_); io.sync(latch_); io.sync(latchUntil_);
  io.sync(sig_); io.sync(phase_); io.sync(msgDone_); io.sync(dataPort_); io.sync(readPort_);
  io.sync(sector_); io.sync(sectorsLeft_); io.raw(cmd_, sizeof cmd_); io.sync(cmdLen_);
  io.raw(fifo_, sizeof fifo_); io.sync(fifoPos_); io.sync(fifoLen_);
  io.sync(tUpdate_); io.sync(tSector_); io.sync(tAckClr_); io.sync(updType_);
  io.sync(cdStatus_); io.sync(cdEnd_); io.sync(cdStart_); io.sync(cdStop_); io.sync(cdSector_); io.sync(cdSample_);
  io.sync(cdL_); io.sync(cdR_); io.sync(tCdTick_); io.sync(tPlayStart_); io.sync(subTick_); io.sync(subPos_);
  io.sync(faderReg_); io.sync(faderStart_);
  io.bytes(adpcmRam_);
  io.sync(adAddrPort_); io.sync(adReadAddr_); io.sync(adWriteAddr_); io.sync(adLength_);
  io.sync(adCtrl_); io.sync(adDma_); io.sync(adRate_); io.sync(adReadBuf_); io.sync(adWriteBuf_);
  io.sync(adNibble_); io.sync(adPlaying_); io.sync(adPlayReq_); io.sync(adEnd_); io.sync(adHalf_);
  io.sync(adOut_); io.sync(adMag_); io.sync(tAdSample_); io.sync(tAdRead_); io.sync(tAdWrite_); io.sync(tDma_); io.sync(adFrac_);
  io.bytes(cdRam_);
  io.bytes(superRam_);
  if (arcade_) {
    io.sync(acUsed_);
    if (acUsed_) io.bytes(acRam_);
    else if (io.reading()) std::fill(acRam_.begin(), acRam_.end(), 0);
    for (auto& p : acPort_) { io.sync(p.base); io.sync(p.offset); io.sync(p.inc); io.sync(p.ctrl); }
    io.sync(acValue_); io.sync(acShift_); io.sync(acRotate_);
  }
  io.sync(audioClk_); io.sync(accL_); io.sync(accR_); io.sync(accN_);
  if (!io.reading()) return;
  // valores impossíveis num estado de verdade (levariam a acesso fora da memória ou a laços longos): recusa
  auto bad = [](bool c) { if (c) throw std::runtime_error("Estado salvo danificado (CD-ROM)."); };
  bad(now_ < 0 || now_ > cpu_.clk || cpu_.clk - now_ > (int64_t(1) << 30));
  for (int64_t t : {tUpdate_, tSector_, tAckClr_, tPlayStart_, tAdSample_, tAdRead_, tAdWrite_, tDma_})
    bad(t != NONE && (t < now_ || t - now_ > TIME_SPAN));
  bad(tCdTick_ < now_ || tCdTick_ - now_ > CDDA_TICK);
  bad(audioClk_ != now_);
  bad(!(accL_ == accL_) || !(accR_ == accR_) || !(accN_ >= 0 && accN_ < 1e6) || std::fabs(accL_) > 1e9 || std::fabs(accR_) > 1e9);
  bad(sig_ > 0xFF || phase_ > BUSY || updType_ > U_GOOD || cmdLen_ > sizeof cmd_);
  bad(fifoLen_ > sizeof fifo_ || fifoPos_ > fifoLen_ || sectorsLeft_ > 256 || sector_ > 0x1FFFFF);
  bad(cdStatus_ > CD_STOPPED || cdEnd_ > END_IRQ || cdSample_ >= 588 || subTick_ >= 6 || subPos_ >= 98);
  bad(adLength_ > 0x1FFFF || adOut_ < 0 || adOut_ > 4095 || adMag_ > 48 || adFrac_ >= 352000u);
  bad((irqEnabled_ & 0x80) || (irqActive_ & 0x83) || resetReg_ > 15);
  for (const auto& p : acPort_) bad(p.base > 0xFFFFFF || p.ctrl > 0x7F);
  cdBufSector_ = -1;
  changed_ = false;
  irqLine();
}

}  // namespace indigo::pcecd
