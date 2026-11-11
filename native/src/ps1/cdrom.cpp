// Controlador de CD-ROM do PlayStation 1 (veja cdrom.h).
#include "cdrom.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace indigo::ps1 {

namespace {

constexpr int32_t ACK_MOTOR_ON = 0xC4E1;    // primeira resposta com o motor girando (média medida)
constexpr int32_t ACK_MOTOR_OFF = 0x5CF4;   // com o motor parado
constexpr int32_t ACK_INIT = 0x13CCE;       // Init e ReadTOC fazem uma inicialização antes de responder
constexpr int32_t INIT_MIN = 476000;       // segunda resposta do Init (ps1-tests cdrom/timing: média 476300 ciclos)
constexpr int32_t READY_DELAY = 2000;       // depois do reconhecimento, até a próxima interrupção
constexpr int32_t SPINUP_CYCLES = CdRom::CPU_HZ;  // partida do motor: ~1 s
constexpr int32_t REPORT_DELAY = 24000000;  // sem relatórios do Play logo depois da busca (~0,7 s)
constexpr int32_t AUDIO_TICK = 768;
// a área final (lead-out) aceita busca até ~4 minutos depois do início: no disco de 70:25:16 do ps1-tests
// cdrom/getloc o console busca 74:00:00 (3:34 depois) e recusa 74:30:00 (4:04 depois) com erro de busca
constexpr int32_t LEADOUT_SEEK = 4 * 60 * 75;         // 33,8688 MHz / 44,1 kHz

inline uint8_t toBcd(int v) { return uint8_t(((v / 10) << 4) | (v % 10)); }
inline int fromBcd(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
inline bool validBcd(uint8_t b) { return (b & 0x0F) <= 9 && (b >> 4) <= 9; }
inline int16_t clamp16(int32_t v) { return int16_t(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }
inline int16_t rd16(const uint8_t* p) { return int16_t(uint16_t(p[0] | p[1] << 8)); }

// argumentos aceitos por comando (mínimo, máximo); -1 = comando inválido
const int8_t ARGS[0x20][2] = {
    {-1, -1}, {0, 0}, {3, 3}, {0, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0},   // 00..07
    {0, 0},   {0, 0}, {0, 0}, {0, 0}, {0, 0}, {2, 2}, {1, 1}, {0, 0},   // 08..0F
    {0, 0},   {0, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 0}, {0, 0}, {-1, -1}, // 10..17
    {-1, -1}, {1, 16}, {0, 0}, {0, 0}, {0, 16}, {2, 2}, {0, 0}, {-1, -1} // 18..1F
};

// interpolador em zigue-zague do XA-ADPCM (37,8 -> 44,1 kHz): 7 fases de 29 coeficientes, a tabela medida no
// hardware (documentação psx-spx), indexada [coeficiente][fase]
const int16_t ZIGZAG[29][7] = {
    {0, 0, 0, 0, -0x0001, +0x0002, -0x0005},
    {0, 0, 0, -0x0001, +0x0003, -0x0008, +0x0011},
    {0, 0, -0x0001, +0x0003, -0x0008, +0x0010, -0x0023},
    {0, -0x0002, +0x0003, -0x0008, +0x0011, -0x0023, +0x0046},
    {0, 0, -0x0002, +0x0006, -0x0010, +0x002B, -0x0017},
    {-0x0002, +0x0003, -0x0005, +0x0005, +0x000A, +0x001A, -0x0044},
    {+0x000A, -0x0013, +0x001F, -0x001B, +0x006B, -0x00EB, +0x015B},
    {-0x0022, +0x003C, -0x004A, +0x00A6, -0x016D, +0x027B, -0x0347},
    {+0x0041, -0x004B, +0x00B3, -0x01A8, +0x0350, -0x0548, +0x080E},
    {-0x0054, +0x00A2, -0x0192, +0x0372, -0x0623, +0x0AFA, -0x1249},
    {+0x0034, -0x00E3, +0x02B1, -0x05BF, +0x0BCD, -0x16FA, +0x3C07},
    {+0x0009, +0x0132, -0x039E, +0x09B8, -0x1780, +0x53E0, +0x53E0},
    {-0x010A, -0x0043, +0x04F8, -0x11B4, +0x6794, +0x3C07, -0x16FA},
    {+0x0400, -0x0267, -0x05A6, +0x74BB, +0x234C, -0x1249, +0x0AFA},
    {-0x0A78, +0x0C9D, +0x7939, +0x0C9D, -0x0A78, +0x080E, -0x0548},
    {+0x234C, +0x74BB, -0x05A6, -0x0267, +0x0400, -0x0347, +0x027B},
    {+0x6794, -0x11B4, +0x04F8, -0x0043, -0x010A, +0x015B, -0x00EB},
    {-0x1780, +0x09B8, -0x039E, +0x0132, +0x0009, -0x0044, +0x001A},
    {+0x0BCD, -0x05BF, +0x02B1, -0x00E3, +0x0034, -0x0017, +0x002B},
    {-0x0623, +0x0372, -0x0192, +0x00A2, -0x0054, +0x0046, -0x0023},
    {+0x0350, -0x01A8, +0x00B3, -0x004B, +0x0041, -0x0023, +0x0010},
    {-0x016D, +0x00A6, -0x004A, +0x003C, -0x0022, +0x0011, -0x0008},
    {+0x006B, -0x001B, +0x001F, -0x0013, +0x000A, -0x0005, +0x0002},
    {+0x000A, +0x0005, -0x0005, +0x0003, -0x0001, 0, 0},
    {-0x0010, +0x0006, -0x0002, 0, 0, 0, 0},
    {+0x0011, -0x0008, +0x0003, -0x0002, +0x0001, 0, 0},
    {-0x0008, +0x0003, -0x0001, 0, 0, 0, 0},
    {+0x0003, -0x0001, 0, 0, 0, 0, 0},
    {-0x0001, 0, 0, 0, 0, 0, 0},
};

const int XA_POS[4] = {0, 60, 115, 98};
const int XA_NEG[4] = {0, 0, -52, -55};

}  // namespace

CdRom::CdRom(cdrom::Disc* disc) : disc_(disc) {
  detectDisc();
  reset();
}

void CdRom::detectDisc() {
  discRegion_ = 0;
  discMode2_ = discHasData_ = false;
  if (!disc_) return;
  for (const auto& t : disc_->tracks()) {
    if (t.type != cdrom::TrackType::Audio) discHasData_ = true;
    if (t.type == cdrom::TrackType::Mode2) discMode2_ = true;
  }
  if (!discHasData_) return;
  // licença no setor 4: "Sony Computer Entertainment Amer|Euro|Inc"
  uint8_t buf[2048];
  if (!disc_->readData(4, buf)) return;
  static const char KEY[] = "Sony Computer Entertainment ";
  const size_t kl = sizeof(KEY) - 1;
  for (size_t i = 0; i + kl + 4 <= sizeof(buf); i++) {
    if (std::memcmp(buf + i, KEY, kl) != 0) continue;
    const char* r = reinterpret_cast<const char*>(buf + i + kl);
    if (!std::memcmp(r, "Amer", 4)) discRegion_ = 'A';
    else if (!std::memcmp(r, "Euro", 4)) discRegion_ = 'E';
    else if (!std::memcmp(r, "Inc", 3)) discRegion_ = 'I';
    break;
  }
}

void CdRom::softReset() {
  index_ = 0;
  std::memset(params_, 0, sizeof(params_));
  prmCount_ = 0;
  std::memset(result_, 0, sizeof(result_));
  resCount_ = resRd_ = 0;
  if_ = 0;
  ie_ = 0;
  readyDelay_ = 0;
  asyncType_ = asyncCount_ = 0;
  request_ = 0;
  cmd_ = 0;
  phase_ = CMD_IDLE;
  cmdTimer_ = 0;
  nargs_ = 0;
  drive_ = after_ = STOPPED;
  cur_ = target_ = setloc_ = 0;
  setlocDirty_ = true;
  sectorTimer_ = 0;
  seekResult_ = -1;
  retry_ = 0;
  sectorsRead_ = 0;
  reportDelay_ = 0;
  reportLastF_ = 0xFF;
  playTrack_ = -1;
  ff_ = 0;
  mode_ = 0x20;
  filterFile_ = filterChan_ = 0;
  muted_ = adpMute_ = false;
  const uint8_t v0[4] = {0x80, 0, 0x80, 0};
  std::memcpy(volPending_, v0, 4);
  std::memcpy(vol_, v0, 4);
  pipeCount_ = pipePos_ = 0;
  std::memset(q_, 0, sizeof(q_));
  std::memset(header_, 0, sizeof(header_));
  headerValid_ = false;
  sectorSize_ = 0;
  sectorNew_ = false;
  fifoSize_ = fifoPos_ = 0;
  shellOpen_ = true;
  clearAudio();
  updateIrq();
}

void CdRom::reset() {
  irqOut_ = false;
  softReset();
  holdLogical_ = false;
  audioTimer_ = AUDIO_TICK;
  spinTimer_ = 0;
  spinDown_ = 0;
  if (disc_ && !lidOpen_) {
    drive_ = SPINUP;
    spinTimer_ = SPINUP_CYCLES;
  }
}

void CdRom::setDisc(cdrom::Disc* disc) {
  const bool had = disc_ != nullptr;
  disc_ = disc;
  detectDisc();
  if (lidOpen_) return;
  // trocar o disco com a tampa fechada equivale a abrir e fechar
  stopReading();
  if (had) lidEvent();
  drive_ = STOPPED;
  sectorTimer_ = 0;
  shellOpen_ = true;
  headerValid_ = false;
  if (disc_) {
    drive_ = SPINUP;
    spinTimer_ = SPINUP_CYCLES;
  }
}

void CdRom::setLidOpen(bool open) {
  if (open == lidOpen_) return;
  lidOpen_ = open;
  if (open) {
    stopReading();
    lidEvent();
    drive_ = STOPPED;
    sectorTimer_ = spinTimer_ = 0;
    spinDown_ = disc_ ? CPU_HZ / 2 : 0;
    headerValid_ = false;
    shellOpen_ = true;
  } else if (disc_) {
    spinDown_ = 0;
    drive_ = SPINUP;
    spinTimer_ = SPINUP_CYCLES;
    cur_ = 0;
  }
}

// abrir a tampa: INT5 com o status só com a tampa aberta e o erro, código 08h
void CdRom::lidEvent() {
  // no console (ps1-tests cdrom/disc-swap) o INT5 da tampa aberta vem só com o bit de erro
  const uint8_t b[2] = {0x01, 0x08};
  setAsync(5, b, 2);
  if (phase_ != CMD_RECEIVED) phase_ = CMD_IDLE;
}

// ------------------------------------------------------------------ interrupções e respostas

void CdRom::updateIrq() {
  const bool line = (if_ & ie_ & 0x1F) != 0;
  if (line && !irqOut_ && onIrq) onIrq();
  irqOut_ = line;
}

void CdRom::raise(uint8_t type, const uint8_t* bytes, int n) {
  std::memset(result_, 0, sizeof(result_));
  n = std::min(n, 16);
  if (n) std::memcpy(result_, bytes, size_t(n));
  resCount_ = uint8_t(n);
  resRd_ = 0;
  if_ = uint8_t((if_ & 0x10) | (type & 7));
  readyDelay_ = READY_DELAY;
  updateIrq();
}

void CdRom::setAsync(uint8_t type, const uint8_t* bytes, int n) {
  asyncType_ = type;
  asyncCount_ = uint8_t(std::min(n, 8));
  std::memcpy(asyncBytes_, bytes, asyncCount_);
  if (canRaise()) {
    raise(asyncType_, asyncBytes_, asyncCount_);
    asyncType_ = asyncCount_ = 0;
  }
}

uint8_t CdRom::stat(bool error) const {
  uint8_t s = 0;
  switch (drive_) {
    case PLAYING: s |= 0x80; break;
    case READING: s |= headerValid_ ? 0x20 : 0x40; break;
    case SEEKING:
    case SEEKING_LOGICAL:
    case SEEK_VERIFY: s |= 0x40; break;
    default: break;
  }
  if (shellOpen_ || lidOpen_ || !disc_) s |= 0x10;
  if ((drive_ != STOPPED && drive_ != SPINUP) || spinDown_ > 0) s |= 0x02;
  if (error) s |= 0x01;
  return s;
}

void CdRom::errorResponse(uint8_t code, uint8_t statOr) {
  const uint8_t b[2] = {uint8_t(stat(true) | statOr), code};
  raise(5, b, 2);
}

// comandos que precisam do disco: sem disco, tampa aberta ou motor partindo dão INT5(stat+1, 80h)
bool CdRom::needDisc() {
  if (discReady()) return true;
  errorResponse(0x80);
  return false;
}

// ------------------------------------------------------------------ registradores

uint32_t CdRom::read(uint32_t offset, int size) {
  offset &= 3;
  auto one = [&](uint32_t a) -> uint8_t {
    switch (a) {
      case 0: {
        uint8_t v = index_;
        if (xaCount_) v |= 0x04;
        if (!prmCount_) v |= 0x08;
        if (prmCount_ < 16) v |= 0x10;
        if (resCount_) v |= 0x20;
        if (fifoPos_ < fifoSize_) v |= 0x40;
        if (phase_ == CMD_RECEIVED) v |= 0x80;
        return v;
      }
      case 1: {
        const uint8_t v = result_[resRd_];
        resRd_ = (resRd_ + 1) & 15;
        if (resCount_) resCount_--;
        return v;
      }
      case 2: {
        if (fifoPos_ < fifoSize_) return fifo_[fifoPos_++];
        // depois do fim o hardware repete um byte perto do fim do setor
        if (!fifoSize_) return 0;
        if (fifoSize_ < 8) return fifo_[0];  // (só com um estado salvo estragado: os setores têm 2048 ou mais)
        return fifo_[fifoSize_ - (fifoSize_ == 2048 ? 8 : 4)];
      }
      default:
        return (index_ & 1) ? uint8_t(0xE0 | if_) : uint8_t(0xE0 | ie_);
    }
  };
  if (size <= 1) return one(offset);
  if (offset == 2) {  // 16/32 bits do buffer de dados: bytes em sequência
    uint32_t v = 0;
    for (int i = 0; i < size; i++) v |= uint32_t(one(2)) << (8 * i);
    return v;
  }
  const uint32_t b = one(offset);
  return size == 2 ? (b | b << 8) : (b * 0x01010101u);
}

uint32_t CdRom::dmaRead() {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= uint32_t(read(2, 1)) << (8 * i);
  return v;
}

void CdRom::write(uint32_t offset, uint32_t value, int size) {
  (void)size;
  offset &= 3;
  const uint8_t v = uint8_t(value);
  if (offset == 0) {
    index_ = v & 3;
    return;
  }
  switch (index_ * 3 + (offset - 1)) {
    case 0:  // comando: o novo substitui o que estiver pendente (inclusive a segunda resposta)
      cmd_ = v;
      phase_ = CMD_RECEIVED;
      cmdTimer_ = (drive_ == STOPPED || drive_ == SPINUP) ? ACK_MOTOR_OFF : ACK_MOTOR_ON;
      if (v == 0x0A || v == 0x1E) cmdTimer_ = ACK_INIT;
      break;
    case 1:  // parâmetro
      if (prmCount_ < 16) params_[prmCount_++] = v;
      break;
    case 2:  // HCHPCTL
      if (v & 0x80) {
        if (fifoPos_ >= fifoSize_ && sectorSize_) {
          // carrega o setor pendente (ou repete o último, se não chegou outro)
          std::memcpy(fifo_, sector_, sectorSize_);
          fifoSize_ = sectorSize_;
          fifoPos_ = 0;
          sectorNew_ = false;
        }
      } else {
        fifoSize_ = fifoPos_ = 0;
      }
      if (v & 0x20) {
        if_ |= 0x10;
        updateIrq();
      }
      request_ = v;
      break;
    case 3:  // WRDATA (mapa de som): não emulado
      break;
    case 4:  // HINTMSK
      ie_ = v & 0x1F;
      updateIrq();
      break;
    case 5:  // HCLRCTL
      if_ &= uint8_t(~(v & 0x1F));
      updateIrq();
      if (v & 0x40) prmCount_ = 0;
      if (v & 0x80) {
        softReset();
        holdLogical_ = false;
        if (discReady()) drive_ = PAUSED;
      }
      break;
    case 6:  // CI (mapa de som): não emulado
      break;
    case 7: volPending_[0] = v; break;  // L->L
    case 8: volPending_[1] = v; break;  // L->R
    case 9: volPending_[2] = v; break;  // R->R
    case 10: volPending_[3] = v; break; // R->L
    case 11:  // ADPCTL
      adpMute_ = v & 1;
      if (v & 0x20) std::memcpy(vol_, volPending_, 4);
      break;
    default: break;
  }
}

// ------------------------------------------------------------------ tempo

void CdRom::run(int cycles) {
  while (cycles > 0) {
    int32_t step = cycles;
    auto lim = [&](int32_t t) {
      if (t > 0 && t < step) step = t;
    };
    lim(audioTimer_);
    const bool sectorOn = sectorTimer_ > 0, spinOn = spinTimer_ > 0;
    const bool cmdOn = phase_ != CMD_IDLE && cmdTimer_ > 0;
    const bool readyOn = !(if_ & 0x0F) && readyDelay_ > 0;
    if (sectorOn) lim(sectorTimer_);
    if (spinOn) lim(spinTimer_);
    if (cmdOn) lim(cmdTimer_);
    if (readyOn) lim(readyDelay_);

    audioTimer_ -= step;
    if (sectorOn) sectorTimer_ -= step;
    if (spinOn) spinTimer_ -= step;
    if (cmdOn) cmdTimer_ -= step;
    if (readyOn) readyDelay_ -= step;
    if (reportDelay_ > 0) reportDelay_ -= step;
    if (spinDown_ > 0) spinDown_ -= step;
    cycles -= step;

    if (spinOn && spinTimer_ <= 0) {
      spinTimer_ = 0;
      if (drive_ == SPINUP) {
        drive_ = PAUSED;
        target_ = cur_;
        holdLogical_ = false;
        makeQ(cur_, q_);  // com o motor girando o subcanal Q já é lido (GetlocP logo depois da partida)
      }
    }
    if (asyncType_ && canRaise()) {
      raise(asyncType_, asyncBytes_, asyncCount_);
      asyncType_ = asyncCount_ = 0;
    }
    if (sectorOn && sectorTimer_ <= 0) sectorEvent();
    if (phase_ != CMD_IDLE && cmdTimer_ <= 0 && canRaise()) {
      if (phase_ == CMD_RECEIVED) execute();
      else second();
    }
    if (audioTimer_ <= 0) {
      audioTimer_ += AUDIO_TICK;
      audioTick();
    }
  }
}

// busca: ~0,6 ms fixos mais o deslocamento da cabeça (o disco inteiro em ~1 s), 0,3 s a mais nos saltos
// longos (acima de 30 s de disco), a volta da pausa (cerca de 5,5 setores) e a partida do motor parado
int32_t CdRom::seekTime(int32_t from, int32_t to, bool resume) const {
  int64_t t = 0;
  if (drive_ == STOPPED || drive_ == SPINUP) {
    t += SPINUP_CYCLES;
    from = 0;
  }
  const int64_t d = std::abs(int64_t(from) - to);
  t += std::max<int64_t>(20000, d * CPU_HZ / (72 * 60 * 75));
  if (d >= 2250) t += int64_t(CPU_HZ) * 3 / 10;
  else if (resume && drive_ == PAUSED) t += int64_t(period()) * 11 / 2;
  else if (d >= 3 && d < 12) t += int64_t(period()) * 4;
  return int32_t(std::min<int64_t>(t, INT32_MAX / 2));
}

void CdRom::startSeek(int32_t target, bool logical, Drive after, int32_t extra, bool resume) {
  sectorTimer_ = std::max(1, seekTime(cur_, target, resume) + extra);
  target_ = target;
  seekFail_ = disc_ && int64_t(target) >= int64_t(disc_->totalSectors()) + LEADOUT_SEEK;
  // a posição já vai para o alvo (o subcanal Q também), e a busca lógica ainda confere o cabeçalho
  cur_ = target;
  retry_ = 128;
  makeQ(target, q_);
  seekResult_ = 0;
  drive_ = logical ? SEEKING_LOGICAL : SEEKING;
  after_ = after;
}

void CdRom::stopReading() {
  asyncType_ = asyncCount_ = 0;
  clearAudio();
  pipeCount_ = pipePos_ = 0;
  sectorsRead_ = 0;
}

// ------------------------------------------------------------------ comandos

void CdRom::execute() {
  phase_ = CMD_IDLE;
  const uint8_t c = cmd_;
  if (c >= 0x20 || ARGS[c][0] < 0) {
    errorResponse(0x40);  // comando inválido (os parâmetros ficam na FIFO)
    return;
  }
  nargs_ = prmCount_;
  std::memcpy(args_, params_, sizeof(args_));
  prmCount_ = 0;
  if (nargs_ < ARGS[c][0] || nargs_ > ARGS[c][1]) {
    errorResponse(0x20);
    return;
  }
  const uint8_t* a = args_;
  uint8_t r[16];
  auto ack = [&]() {
    r[0] = stat();
    raise(3, r, 1);
  };
  auto secondIn = [&](int32_t t) {
    phase_ = CMD_SECOND;
    cmdTimer_ = std::max(1, t);
  };
  switch (c) {
    case 0x01:  // Getstat (Nop): também apaga o aviso de tampa aberta
      ack();
      if (!lidOpen_ && disc_) shellOpen_ = false;
      break;
    case 0x02: {  // Setloc
      if (!needDisc()) break;
      if (!validBcd(a[0]) || !validBcd(a[1]) || !validBcd(a[2]) || a[1] > 0x59 || a[2] > 0x74) {
        errorResponse(0x10);
        break;
      }
      setloc_ = (fromBcd(a[0]) * 60 + fromBcd(a[1])) * 75 + fromBcd(a[2]) - 150;
      setlocDirty_ = true;
      ack();
      break;
    }
    case 0x03: {  // Play
      if (!needDisc()) break;
      ack();
      ff_ = 0;
      const auto& tr = disc_->tracks();
      int track = (nargs_ && a[0]) ? fromBcd(a[0]) : 0;
      if (track) {
        const int first = tr.front().number, last = tr.back().number;
        if (track < first) track = first;
        if (track > last) {  // além da última: volta ao começo da faixa atual
          const int ti = disc_->trackAt(uint32_t(std::max(0, cur_)));
          track = ti >= 0 ? tr[size_t(ti)].number : first;
        }
        stopReading();
        playTrack_ = toBcd(track);
        startSeek(int32_t(tr[size_t(track - first)].start), false, PLAYING, 0);
        headerValid_ = false;
        reportLastF_ = 0xFF;
      } else if (setlocDirty_ || drive_ != PLAYING) {
        stopReading();
        const int32_t t = setlocDirty_ ? setloc_ : cur_;
        playTrack_ = -1;
        startSeek(t, false, PLAYING, 0);
        headerValid_ = false;
        reportLastF_ = 0xFF;
      }
      setlocDirty_ = false;
      break;
    }
    case 0x04:
    case 0x05:  // Forward / Backward: só durante o Play
      if (!needDisc()) break;
      if (drive_ != PLAYING) {
        errorResponse(0x80);
        break;
      }
      ack();
      ff_ = c == 0x04 ? 1 : -1;
      break;
    case 0x06:
    case 0x1B: {  // ReadN / ReadS
      if (!needDisc()) break;
      ack();
      if ((drive_ == SEEKING_LOGICAL || drive_ == SEEK_VERIFY) && target_ == setloc_ && after_ == READING) {
        setlocDirty_ = false;
        break;
      }
      if (setlocDirty_ || drive_ != READING) {
        stopReading();
        sectorSize_ = 0;
        sectorNew_ = false;
        int32_t t = target_;
        if (setlocDirty_) t = setloc_;
        else if (drive_ != PAUSED && drive_ != STANDBY) t = cur_;
        // o pausado volta ao alvo que estava segurando; a volta da pausa não custa nada a mais na leitura: o
        // ps1-tests (cdrom/timing) mede o primeiro setor no console em 0,67 a 0,98 milhão de ciclos em 1x
        startSeek(t, true, READING, 0, false);
        headerValid_ = false;
      }
      setlocDirty_ = false;
      break;
    }
    case 0x07:  // MotorOn: só com o motor parado (senão INT5 com 20h)
      if (!needDisc()) break;
      if (drive_ != STOPPED) {
        errorResponse(0x20);
        break;
      }
      ack();
      stopReading();
      target_ = cur_;
      holdLogical_ = false;
      drive_ = STANDBY;
      secondIn(CPU_HZ / 10);
      break;
    case 0x08: {  // Stop
      if (!needDisc()) break;
      ack();
      if (drive_ == STOPPED) {
        secondIn(0x1D7B);
        break;
      }
      const bool dbl = mode_ & 0x80;
      stopReading();
      drive_ = STOPPED;
      sectorTimer_ = 0;
      headerValid_ = false;
      secondIn(dbl ? 0x18A6076 : 0xD38ACA);
      break;
    }
    case 0x09:  // Pause
      if (!needDisc()) break;
      ack();
      if (drive_ == PAUSED || drive_ == STOPPED) {
        secondIn(0x1DF2);
        break;
      }
      // volta alguns setores: a leitura retomada repete o último entregue
      cur_ -= int32_t(std::min<uint32_t>(sectorsRead_, 4));
      sectorsRead_ = 0;
      pipeCount_ = pipePos_ = 0;
      asyncType_ = asyncCount_ = 0;
      target_ = cur_;
      drive_ = PAUSED;
      sectorTimer_ = period();
      secondIn((mode_ & 0x80) ? 0x10BD93 : 0x21181C);
      break;
    case 0x0A:  // Init: modo 20h, sem mudo, motor ligado e busca ao começo do disco
      ack();
      muted_ = false;
      mode_ = 0x20;
      setloc_ = 0;
      if (discReady()) {
        stopReading();
        sectorSize_ = 0;
        sectorNew_ = false;
        const bool shortHop = std::abs(cur_) < 12 && drive_ != STOPPED && drive_ != SPINUP;
        // busca pelo subcanal (como o SeekP): a cabeça para no pregap, antes do 00:02:00 (ps1-tests cdrom/getloc)
        startSeek(0, false, PAUSED, 0, false);
        // perto do começo é só um pulo curto: a segunda resposta sai com o tempo mínimo do Init
        if (shortHop) sectorTimer_ = std::min(sectorTimer_, 20000);
        phase_ = CMD_SEEKWAIT;
        cmdTimer_ = INIT_MIN;  // a segunda resposta espera o fim da busca, e nunca sai antes disso
      } else {
        secondIn(70000);
      }
      break;
    case 0x0B:  // Mute
    case 0x0C:  // Demute
      if (!needDisc()) break;
      muted_ = c == 0x0B;
      ack();
      break;
    case 0x0D:  // Setfilter
      if (!needDisc()) break;
      filterFile_ = a[0];
      filterChan_ = a[1];
      ack();
      break;
    case 0x0E:  // Setmode
      mode_ = a[0];
      ack();
      break;
    case 0x0F:  // Getparam
      r[0] = stat();
      r[1] = mode_;
      r[2] = 0;
      r[3] = filterFile_;
      r[4] = filterChan_;
      raise(3, r, 5);
      break;
    case 0x10:  // GetlocL: cabeçalho e subcabeçalho do último setor de dados
      if (!needDisc()) break;
      if (!headerValid_) {
        errorResponse(0x80);
        break;
      }
      raise(3, header_, 8);
      break;
    case 0x11:  // GetlocP: subcanal Q (com o motor parado não há subcanal)
      if (!needDisc()) break;
      if (drive_ == STOPPED) {
        errorResponse(0x80);
        break;
      }
      r[0] = q_[1]; r[1] = q_[2]; r[2] = q_[3]; r[3] = q_[4];
      r[4] = q_[5]; r[5] = q_[7]; r[6] = q_[8]; r[7] = q_[9];
      raise(3, r, 8);
      break;
    case 0x12:  // SetSession: só há uma sessão
      if (!needDisc()) break;
      if (a[0] == 0) {
        errorResponse(0x10);
        break;
      }
      ack();
      args_[0] = a[0];
      secondIn(33868);
      break;
    case 0x13: {  // GetTN
      if (!needDisc()) break;
      const auto& tr = disc_->tracks();
      r[0] = stat();
      r[1] = toBcd(tr.front().number);
      r[2] = toBcd(tr.back().number);
      raise(3, r, 3);
      break;
    }
    case 0x14: {  // GetTD: início da faixa (índice 1) em minutos e segundos; 0 = fim do disco
      if (!needDisc()) break;
      const auto& tr = disc_->tracks();
      uint32_t lba;
      if (a[0] == 0) {
        lba = disc_->totalSectors();
      } else {
        const int t = fromBcd(a[0]);
        if (!validBcd(a[0]) || t < tr.front().number || t > tr.back().number) {
          errorResponse(0x10);
          break;
        }
        lba = tr[size_t(t - tr.front().number)].start;
      }
      uint8_t m, s, f;
      cdrom::lbaToMsf(lba, m, s, f);
      r[0] = stat();
      r[1] = toBcd(m);
      r[2] = toBcd(s);
      raise(3, r, 3);
      break;
    }
    case 0x15:  // SeekL (pelo cabeçalho dos setores de dados)
    case 0x16:  // SeekP (pelo subcanal Q)
      if (!needDisc()) break;
      ack();
      sectorsRead_ = 0;
      pipeCount_ = pipePos_ = 0;
      asyncType_ = asyncCount_ = 0;
      if (c == 0x16) headerValid_ = false;
      startSeek(setloc_, c == 0x15, STANDBY, 0);
      phase_ = CMD_SEEKWAIT;
      cmdTimer_ = 256;
      break;
    case 0x19: {  // Test
      const uint8_t sub = a[0];
      if (sub <= 0x03 || (sub >= 0x10 && sub <= 0x1A)) {
        ack();
      } else if (sub == 0x04) {
        ack();
      } else if (sub == 0x05) {
        const bool lic = discReady() && discHasData_;
        r[0] = lic ? 1 : 0;
        r[1] = lic ? 1 : 0;
        raise(3, r, 2);
      } else if (sub == 0x20) {  // data e versão do BIOS do HC05: 10/01/1997, vC2
        const uint8_t v[4] = {0x97, 0x01, 0x10, 0xC2};
        raise(3, v, 4);
      } else if (sub == 0x21) {  // chaves: bit 0 cabeça no começo, bit 1 tampa aberta
        r[0] = uint8_t((cur_ < 1000 ? 1 : 0) | (lidOpen_ ? 2 : 0));
        raise(3, r, 1);
      } else if (sub == 0x22) {
        char reg = consoleRegion_ ? consoleRegion_ : (discRegion_ ? discRegion_ : 'A');
        const char* s = reg == 'E' ? "for Europe" : (reg == 'I' ? "for Japan" : "for U/C");
        raise(3, reinterpret_cast<const uint8_t*>(s), int(std::strlen(s)));
      } else if (sub == 0x23 || sub == 0x24) {
        raise(3, reinterpret_cast<const uint8_t*>("CXD2545Q"), 8);
      } else if (sub == 0x25) {
        raise(3, reinterpret_cast<const uint8_t*>("CXD1815Q"), 8);
      } else {
        errorResponse(0x10);
      }
      break;
    }
    case 0x1A:  // GetID
      if (lidOpen_ || drive_ == SPINUP) {
        errorResponse(0x80);
        break;
      }
      ack();
      secondIn(0x4A00);
      break;
    case 0x1C:  // Reset do HC05: como abrir e fechar a tampa, sem segunda resposta
      ack();
      mode_ = 0x20;
      muted_ = false;
      stopReading();
      sectorTimer_ = 0;
      headerValid_ = false;
      if (discReady()) {
        drive_ = SPINUP;
        spinTimer_ = 0x400000;
      }
      break;
    case 0x1D:  // GetQ
      if (!needDisc()) break;
      ack();
      secondIn(0x4A00);
      break;
    case 0x1E:  // ReadTOC
      if (!needDisc()) break;
      ack();
      stopReading();
      headerValid_ = false;
      secondIn(30000000 + seekTime(cur_, 0));
      target_ = 0;
      holdLogical_ = false;
      drive_ = PAUSED;
      sectorTimer_ = 0;
      break;
    default:
      errorResponse(0x40);
      break;
  }
}

// segundas respostas (e a espera do fim da busca)
void CdRom::second() {
  uint8_t r[16];
  if (phase_ == CMD_SEEKWAIT) {
    if (drive_ == SEEKING || drive_ == SEEKING_LOGICAL || drive_ == SEEK_VERIFY) {
      cmdTimer_ = std::max(256, sectorTimer_);
      return;
    }
    phase_ = CMD_IDLE;
    if (seekResult_ < 0) {
      r[0] = uint8_t(stat() | 0x04);
      r[1] = 0x04;
      raise(5, r, 2);
    } else {
      r[0] = stat();
      raise(2, r, 1);
    }
    return;
  }
  phase_ = CMD_IDLE;
  switch (cmd_) {
    case 0x1A: {  // GetID
      if (!disc_) {
        const uint8_t b[8] = {0x08, 0x40, 0, 0, 0, 0, 0, 0};
        raise(5, b, 8);
      } else if (!discHasData_) {  // CD de música
        const uint8_t b[8] = {uint8_t(stat() | 0x08), 0x90, 0, 0, 0, 0, 0, 0};
        raise(5, b, 8);
      } else {
        char reg = discRegion_ ? discRegion_ : (consoleRegion_ ? consoleRegion_ : 'A');
        const uint8_t b[8] = {stat(), 0x00, uint8_t(discMode2_ ? 0x20 : 0x00), 0x00, 'S', 'C', 'E', uint8_t(reg)};
        raise(2, b, 8);
      }
      break;
    }
    case 0x12:  // SetSession
      if (args_[0] == 1) {
        r[0] = stat();
        raise(2, r, 1);
      } else {
        r[0] = uint8_t(stat() | 0x04);
        r[1] = 0x40;
        raise(5, r, 2);
      }
      break;
    case 0x1D: {  // GetQ: entrada do TOC (A0 primeira faixa, A1 última, A2 fim, ou a faixa pedida)
      const auto& tr = disc_->tracks();
      const uint8_t point = args_[1];
      uint32_t lba = 0;
      bool ok = true;
      uint8_t ctrl = tr.front().type == cdrom::TrackType::Audio ? 0 : 4;
      if (point == 0xA0 || point == 0xA1) {
        const auto& t = point == 0xA0 ? tr.front() : tr.back();
        std::memset(r, 0, 11);
        r[0] = uint8_t(ctrl << 4 | 1);
        r[2] = point;
        r[7] = toBcd(t.number);
        r[8] = point == 0xA0 ? (discMode2_ ? 0x20 : 0x00) : 0;
        raise(2, r, 11);
        break;
      }
      if (point == 0xA2) lba = disc_->totalSectors();
      else {
        const int t = fromBcd(point);
        ok = validBcd(point) && t >= tr.front().number && t <= tr.back().number;
        if (ok) {
          lba = tr[size_t(t - tr.front().number)].start;
          ctrl = tr[size_t(t - tr.front().number)].type == cdrom::TrackType::Audio ? 0 : 4;
        }
      }
      if (!ok) {
        r[0] = uint8_t(stat() | 0x04);
        r[1] = 0x40;
        raise(5, r, 2);
        break;
      }
      uint8_t m, s, f;
      cdrom::lbaToMsf(lba, m, s, f);
      std::memset(r, 0, 11);
      r[0] = uint8_t(ctrl << 4 | 1);
      r[2] = point;
      r[7] = toBcd(m);
      r[8] = toBcd(s);
      r[9] = toBcd(f);
      raise(2, r, 11);
      break;
    }
    default:
      r[0] = stat();
      raise(2, r, 1);
      break;
  }
}

// ------------------------------------------------------------------ leitura dos setores

bool CdRom::makeQ(int32_t lba, uint8_t* q) const {
  // antes do LBA 0 (os 2 s iniciais) o subcanal é o do pregap da faixa 1 (índice 0), como o ps1-tests
  // cdrom/getloc mostra no console logo depois do Init
  if (!disc_ || lba < -150) return false;
  const uint32_t total = disc_->totalSectors();
  const auto& tr = disc_->tracks();
  uint8_t ctrl, trackBcd, index;
  uint32_t rel;
  if (lba >= 0 && uint32_t(lba) >= total) {  // área final
    ctrl = tr.back().type == cdrom::TrackType::Audio ? 0 : 4;
    trackBcd = 0xAA;
    index = 1;
    rel = uint32_t(lba) - total;
  } else {
    const int ti = lba < 0 ? 0 : disc_->trackAt(uint32_t(lba));
    const auto& t = tr[size_t(ti < 0 ? 0 : ti)];
    ctrl = t.type == cdrom::TrackType::Audio ? 0 : 4;
    trackBcd = toBcd(t.number);
    index = int64_t(lba) < int64_t(t.start) ? 0 : 1;
    rel = uint32_t(std::abs(int64_t(lba) - int64_t(t.start)));
  }
  q[0] = uint8_t(ctrl << 4 | 1);
  q[1] = trackBcd;
  q[2] = toBcd(index);
  q[3] = toBcd(int(rel / 4500 % 100));
  q[4] = toBcd(int(rel / 75 % 60));
  q[5] = toBcd(int(rel % 75));
  q[6] = 0;
  const uint32_t a = uint32_t(lba) + 150;
  q[7] = toBcd(int(a / 4500 % 100));
  q[8] = toBcd(int(a / 75 % 60));
  q[9] = toBcd(int(a % 75));
  q[10] = q[11] = 0;
  return true;
}

void CdRom::readSector(int32_t lba, uint8_t* raw, uint8_t* q) {
  if (disc_ && lba >= 0 && uint32_t(lba) < disc_->totalSectors()) {
    if (!disc_->readRaw(uint32_t(lba), raw)) std::memset(raw, 0, 2352);
  } else {
    std::memset(raw, 0, 2352);
    // os 2 s antes do LBA 0 e a área final de um disco de dados têm setores de dados com cabeçalho (a imagem
    // não os guarda): o SeekL perto do começo e do fim funciona como no console
    const auto* tr = disc_ ? &disc_->tracks() : nullptr;
    if (tr && !tr->empty() && lba >= -150) {
      const auto& t = lba < 0 ? tr->front() : tr->back();
      if (t.type != cdrom::TrackType::Audio) {
        uint8_t m, sec, f;
        cdrom::lbaToMsf(uint32_t(lba), m, sec, f);
        std::memset(raw + 1, 0xFF, 10);
        raw[12] = toBcd(m);
        raw[13] = toBcd(sec);
        raw[14] = toBcd(f);
        raw[15] = t.type == cdrom::TrackType::Mode1 ? 1 : 2;
      }
    }
  }
  if (makeQ(lba, q)) std::memcpy(q_, q, 12);
  else std::memcpy(q, q_, 12);
}

void CdRom::sectorEvent() {
  if ((drive_ == SEEKING || drive_ == SEEKING_LOGICAL) && seekFail_) {
    // além do fim do disco: erro de busca (INT5 stat+4, 04h na segunda resposta) e o motor para
    seekFail_ = false;
    holdLogical_ = false;
    headerValid_ = false;
    drive_ = STOPPED;
    seekResult_ = -1;
    sectorTimer_ = 0;
    pipeCount_ = pipePos_ = 0;
    return;
  }
  if (drive_ == SEEKING) {
    cur_ = target_;
    holdLogical_ = false;
    drive_ = after_;
    seekResult_ = 1;
    reportDelay_ = REPORT_DELAY;
    if (drive_ == PAUSED || drive_ == STANDBY) cur_ = std::max(-150, cur_ - 9);
  } else if (drive_ == SEEKING_LOGICAL) {
    cur_ = target_;
    holdLogical_ = true;
    drive_ = SEEK_VERIFY;
  }
  if (drive_ != PAUSED && drive_ != STANDBY && drive_ != READING && drive_ != PLAYING && drive_ != SEEK_VERIFY) {
    sectorTimer_ = 0;
    return;
  }
  // o setor mais antigo da latência sai para o decodificador
  if (pipeCount_ == 2) processSector(pipe_[pipePos_], pipeQ_[pipePos_]);
  uint8_t* raw = pipe_[pipePos_];
  uint8_t* q = pipeQ_[pipePos_];
  readSector(cur_, raw, q);

  if (q[1] == 0xAA && (drive_ == PLAYING || drive_ == READING)) {
    // fim do disco: INT4 e pausa
    headerValid_ = false;
    target_ = cur_;
    drive_ = PAUSED;
    pipeCount_ = pipePos_ = 0;
    sectorsRead_ = 0;
    uint8_t b[1] = {stat()};
    setAsync(4, b, 1);
    sectorTimer_ += period();
    return;
  }
  if (drive_ == PLAYING) {
    if (mode_ & 0x02) {  // pausa automática no fim da faixa
      if (playTrack_ < 0) playTrack_ = q[1];
      if (q[1] != uint8_t(playTrack_)) {
        uint8_t b[1] = {stat()};
        setAsync(4, b, 1);
        target_ = cur_;
        drive_ = PAUSED;
        pipeCount_ = pipePos_ = 0;
        sectorsRead_ = 0;
        sectorTimer_ = 0;
        return;
      }
    }
    if ((mode_ & 0x04) && reportDelay_ <= 0 && ((q[9] >> 4) != reportLastF_ || ff_)) {
      // relatório: tempo absoluto nos quadros x0/x2/x4/x6, relativo (segundos + 80h) nos x1/x3/x5/x7, e o pico
      const int ch = q[8] & 1;
      int peak = 0;
      for (int i = 0; i < 588; i++) peak = std::max(peak, std::min(32767, std::abs(int(rd16(raw + i * 4 + ch * 2)))));
      peak |= ch << 15;
      uint8_t b[8];
      b[0] = stat();
      b[1] = q[1];
      b[2] = q[2];
      if (q[9] & 0x10) {
        b[3] = q[3];
        b[4] = uint8_t(q[4] | 0x80);
        b[5] = q[5];
      } else {
        b[3] = q[7];
        b[4] = q[8];
        b[5] = q[9];
      }
      b[6] = uint8_t(peak);
      b[7] = uint8_t(peak >> 8);
      setAsync(1, b, 8);
    }
    reportLastF_ = q[9] >> 4;
  }
  pipePos_ ^= 1;
  if (pipeCount_ < 2) pipeCount_++;
  sectorTimer_ += period();
  if (drive_ == PLAYING && ff_) {
    cur_ += ff_ * 12;
    if (cur_ < 0) cur_ = 0;
  } else {
    cur_++;
  }
  if (drive_ == PAUSED || drive_ == STANDBY) {
    // parada: a cabeça fica dançando um pouco antes do alvo
    if (cur_ >= target_ + (holdLogical_ ? 2 : 0)) cur_ = std::max(-150, cur_ - 9);
  } else {
    sectorsRead_++;
  }
}

void CdRom::processSector(const uint8_t* raw, const uint8_t* q) {
  if (q[0] & 0x40) {  // setor de dados
    if (drive_ == SEEK_VERIFY || drive_ == READING || (holdLogical_ && (drive_ == PAUSED || drive_ == STANDBY))) {
      std::memcpy(header_, raw + 12, 12);
      headerValid_ = true;
      if (drive_ == SEEK_VERIFY) {
        const int32_t lba = (fromBcd(raw[12]) * 60 + fromBcd(raw[13])) * 75 + fromBcd(raw[14]) - 150;
        if (lba == target_) {
          drive_ = after_;
          seekResult_ = 1;
          reportDelay_ = REPORT_DELAY;
        } else if (!retry_) {
          holdLogical_ = false;
          drive_ = STANDBY;
          seekResult_ = -1;
        } else {
          retry_--;
        }
      }
    }
    if (drive_ == READING) {
      if ((mode_ & 0x40) && raw[15] == 2 && (raw[18] & 0x64) == 0x64) {
        // XA-ADPCM: vai para o decodificador (se passar no filtro), nunca para a CPU
        if (!(mode_ & 0x08) || (raw[16] == filterFile_ && raw[17] == filterChan_)) decodeXa(raw);
      } else {
        deliverData(raw);
      }
    }
  } else {  // setor de áudio
    if (drive_ == SEEK_VERIFY) {
      if (mode_ & 0x01) {
        drive_ = after_;
        seekResult_ = 1;
        reportDelay_ = REPORT_DELAY;
      } else if (!retry_) {
        holdLogical_ = false;
        drive_ = STANDBY;
        seekResult_ = -1;
      } else {
        retry_--;
      }
    }
    if ((drive_ == READING && (mode_ & 0x01)) || drive_ == PLAYING) pushCdda(raw);
  }
}

void CdRom::deliverData(const uint8_t* raw) {
  int off, size;
  if (mode_ & 0x10) {
    off = 24;
    size = 2328;
  } else if (mode_ & 0x20) {
    off = 12;
    size = 2340;
  } else {
    off = raw[15] == 1 ? 16 : 24;
    size = 2048;
  }
  std::memcpy(sector_, raw + off, size_t(size));
  sectorSize_ = uint16_t(size);
  sectorNew_ = true;
  const uint8_t b[1] = {stat()};
  setAsync(1, b, 1);
}

// ------------------------------------------------------------------ áudio

void CdRom::clearAudio() {
  cddaRd_ = cddaCount_ = 0;
  cddaStep_ = 1;
  xaRd_ = xaCount_ = 0;
  std::memset(xaPrev_, 0, sizeof(xaPrev_));
  xaLast_[0] = xaLast_[1] = 0;
  std::memset(zig_, 0, sizeof(zig_));
  zigPos_ = zigPhase_ = 0;
}

void CdRom::pushCdda(const uint8_t* raw) {
  if (cddaCount_ + 588 > CDDA_CAP) return;
  for (int i = 0; i < 588; i++) {
    const int w = (cddaRd_ + cddaCount_ + i) % CDDA_CAP;
    cdda_[w][0] = rd16(raw + i * 4);
    cdda_[w][1] = rd16(raw + i * 4 + 2);
  }
  cddaCount_ = uint16_t(cddaCount_ + 588);
  cddaStep_ = (mode_ & 0x80) ? 2 : 1;
}

// decodifica os 18 grupos de 128 bytes de um setor XA para a fila de 37,8 kHz (18,9 kHz entra duplicado)
void CdRom::decodeXa(const uint8_t* raw) {
  const uint8_t coding = raw[19];
  const bool stereo = coding & 0x01, half = coding & 0x04, bits8 = coding & 0x10;
  const int units = bits8 ? 4 : 8;
  const int perSector = 18 * units * 28 / (stereo ? 2 : 1) * (half ? 2 : 1);
  if (xaCount_ + perSector > XA_CAP) return;  // fila cheia: o setor se perde
  int16_t buf[8][28];
  for (int g = 0; g < 18; g++) {
    const uint8_t* sg = raw + 24 + g * 128;
    for (int u = 0; u < units; u++) {
      const uint8_t hdr = sg[4 + u];
      int shift = hdr & 0x0F;
      if (shift > 12) shift = 9;
      const int f = (hdr >> 4) & 3;
      const int ch = stereo ? (u & 1) : 0;
      int32_t old = xaPrev_[ch][0], older = xaPrev_[ch][1];
      for (int j = 0; j < 28; j++) {
        int32_t t;
        if (bits8) t = int16_t(uint16_t(sg[16 + u + j * 4] << 8)) >> shift;
        else t = int16_t(uint16_t(((sg[16 + (u >> 1) + j * 4] >> ((u & 1) * 4)) & 0x0F) << 12)) >> shift;
        const int32_t pred = xaBeetleRounding_ ? ((old * XA_POS[f]) >> 6) + ((older * XA_NEG[f]) >> 6)
                                               : (old * XA_POS[f] + older * XA_NEG[f] + 32) >> 6;
        const int32_t s = clamp16(t + pred);
        older = old;
        old = s;
        buf[u][j] = int16_t(s);
      }
      xaPrev_[ch][0] = int16_t(old);
      xaPrev_[ch][1] = int16_t(older);
    }
    // 18,9 kHz vira 37,8 kHz com uma amostra intermediária (média das vizinhas) antes do zigue-zague
    auto push = [&](int16_t l, int16_t r) {
      if (half) {
        const int w = (xaRd_ + xaCount_) % XA_CAP;
        xa_[w][0] = int16_t((xaLast_[0] + l) >> 1);
        xa_[w][1] = int16_t((xaLast_[1] + r) >> 1);
        xaCount_++;
      }
      const int w = (xaRd_ + xaCount_) % XA_CAP;
      xa_[w][0] = l;
      xa_[w][1] = r;
      xaCount_++;
      xaLast_[0] = l;
      xaLast_[1] = r;
    };
    if (stereo) {
      for (int b = 0; b < units / 2; b++)
        for (int j = 0; j < 28; j++) {
          if (onXaSample) onXaSample(buf[2 * b][j], buf[2 * b + 1][j]);
          push(buf[2 * b][j], buf[2 * b + 1][j]);
        }
    } else {
      for (int u = 0; u < units; u++)
        for (int j = 0; j < 28; j++) {
          if (onXaSample) onXaSample(buf[u][j], buf[u][j]);
          push(buf[u][j], buf[u][j]);
        }
    }
  }
}

void CdRom::audioTick() {
  int32_t l = 0, r = 0;
  if (cddaCount_ >= cddaStep_) {
    for (int i = 0; i < cddaStep_; i++) {
      l = cdda_[cddaRd_][0];
      r = cdda_[cddaRd_][1];
      cddaRd_ = uint16_t((cddaRd_ + 1) % CDDA_CAP);
      cddaCount_--;
    }
  }
  // XA: a cada 6 amostras de 37,8 kHz saem 7 de 44,1 kHz, pelas 7 fases da tabela
  bool xaOut = true;
  if (zigPhase_ == 0) {
    if (xaCount_ >= 6) {
      for (int i = 0; i < 6; i++) {
        zig_[0][zigPos_ & 31] = xa_[xaRd_][0];
        zig_[1][zigPos_ & 31] = xa_[xaRd_][1];
        zigPos_ = uint8_t((zigPos_ + 1) & 31);
        xaRd_ = uint16_t((xaRd_ + 1) % XA_CAP);
        xaCount_--;
      }
    } else {
      xaOut = false;
    }
  }
  if (xaOut) {
    for (int c = 0; c < 2; c++) {
      int32_t sum = 0;
      for (int i = 0; i < 29; i++) sum += (zig_[c][(zigPos_ - 1 - i) & 31] * ZIGZAG[i][zigPhase_]) >> 15;
      const int32_t v = clamp16(sum);
      if (!adpMute_) (c ? r : l) += v;
    }
    zigPhase_ = uint8_t((zigPhase_ + 1) % 7);
  }
  int16_t ol = 0, orr = 0;
  if (!muted_) {
    ol = clamp16(((l * vol_[0]) >> 7) + ((r * vol_[3]) >> 7));
    orr = clamp16(((l * vol_[1]) >> 7) + ((r * vol_[2]) >> 7));
  }
  if (onAudio) onAudio(ol, orr);
}

// ------------------------------------------------------------------ estado salvo

void CdRom::sync(StateIO& io) {
  io.sync(index_);
  io.raw(params_, sizeof(params_));
  io.sync(prmCount_);
  io.raw(result_, sizeof(result_));
  io.sync(resCount_);
  io.sync(resRd_);
  io.sync(ie_);
  io.sync(if_);
  io.sync(irqOut_);
  io.sync(readyDelay_);
  io.sync(asyncType_);
  io.raw(asyncBytes_, sizeof(asyncBytes_));
  io.sync(asyncCount_);
  io.sync(request_);
  io.sync(cmd_);
  io.sync(phase_);
  io.sync(cmdTimer_);
  io.raw(args_, sizeof(args_));
  io.sync(nargs_);
  io.sync(drive_);
  io.sync(after_);
  io.sync(cur_);
  io.sync(target_);
  io.sync(setloc_);
  io.sync(setlocDirty_);
  io.sync(sectorTimer_);
  io.sync(spinTimer_);
  io.sync(spinDown_);
  io.sync(seekResult_);
  io.sync(seekFail_);
  io.sync(retry_);
  io.sync(holdLogical_);
  io.sync(sectorsRead_);
  io.sync(reportDelay_);
  io.sync(reportLastF_);
  io.sync(playTrack_);
  io.sync(ff_);
  io.sync(mode_);
  io.sync(filterFile_);
  io.sync(filterChan_);
  io.sync(muted_);
  io.sync(adpMute_);
  io.raw(volPending_, sizeof(volPending_));
  io.raw(vol_, sizeof(vol_));
  io.sync(lidOpen_);
  io.sync(shellOpen_);
  io.raw(pipe_, sizeof(pipe_));
  io.raw(pipeQ_, sizeof(pipeQ_));
  io.sync(pipeCount_);
  io.sync(pipePos_);
  io.raw(q_, sizeof(q_));
  io.raw(header_, sizeof(header_));
  io.sync(headerValid_);
  io.raw(sector_, sizeof(sector_));
  io.sync(sectorSize_);
  io.sync(sectorNew_);
  io.raw(fifo_, sizeof(fifo_));
  io.sync(fifoSize_);
  io.sync(fifoPos_);
  io.sync(audioTimer_);
  io.raw(cdda_, sizeof(cdda_));
  io.sync(cddaRd_);
  io.sync(cddaCount_);
  io.sync(cddaStep_);
  io.raw(xa_, sizeof(xa_));
  io.sync(xaRd_);
  io.sync(xaCount_);
  io.raw(xaPrev_, sizeof(xaPrev_));
  io.raw(xaLast_, sizeof(xaLast_));
  io.raw(zig_, sizeof(zig_));
  io.sync(zigPos_);
  io.sync(zigPhase_);
  if (io.reading()) {
    index_ &= 3;
    clampTo<uint8_t>(prmCount_, 0, 16);
    clampTo<uint8_t>(resCount_, 0, 16);
    resRd_ &= 15;
    clampTo<uint8_t>(asyncCount_, 0, 8);
    clampTo<uint8_t>(phase_, CMD_IDLE, CMD_SEEKWAIT);
    clampTo<uint8_t>(nargs_, 0, 16);
    clampTo<uint8_t>(drive_, STOPPED, STANDBY);
    clampTo<int32_t>(spinDown_, 0, CPU_HZ);
    clampTo<uint8_t>(after_, STOPPED, STANDBY);
    clampTo<uint8_t>(pipeCount_, 0, 2);
    pipePos_ &= 1;
    if (sectorSize_ != 2048 && sectorSize_ != 2340 && sectorSize_ != 2328) sectorSize_ = 0;
    clampTo<uint16_t>(fifoSize_, 0, 2340);
    clampTo<uint16_t>(fifoPos_, 0, fifoSize_);
    if (audioTimer_ < 1 || audioTimer_ > AUDIO_TICK) audioTimer_ = AUDIO_TICK;
    clampTo<uint16_t>(cddaRd_, 0, CDDA_CAP - 1);
    clampTo<uint16_t>(cddaCount_, 0, CDDA_CAP);
    clampTo<uint8_t>(cddaStep_, 1, 2);
    clampTo<uint16_t>(xaRd_, 0, XA_CAP - 1);
    clampTo<uint16_t>(xaCount_, 0, XA_CAP);
    zigPos_ &= 31;
    zigPhase_ %= 7;
    if (!disc_) {
      drive_ = STOPPED;
      sectorTimer_ = 0;
    }
  }
}

}  // namespace indigo::ps1
