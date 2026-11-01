// Drive do Sega CD (CDD), pelo protocolo descrito no "Mega-CD Hardware Manual": o sub-CPU escreve 10 nibbles
// de comando (CS0-CS9, registradores 42-4B) e o drive responde com 10 nibbles de estado (RS0-RS9, 38-41) a cada
// interrupção de 75 Hz.
//   CS0: 0 pedir estado, 1 parar, 2 ler o TOC (CS3 = qual informação), 3 tocar a partir de CS2-CS7 (MSF em BCD),
//        4 buscar e pausar, 6 pausar, 7 continuar, 8 avançar rápido, 9 voltar rápido, A pausar (pular faixa),
//        C fechar a bandeja, D abrir a bandeja;
//   RS0: estado (0 parado, 1 tocando, 2 buscando, 3 varrendo, 4 em pausa, 5 bandeja aberta, 9 leu o TOC,
//        B sem disco, C fim do disco); RS1: o que vem em RS2-RS7 (0 tempo absoluto, 1 tempo da faixa,
//        2 faixa atual, 3 fim do disco, 4 primeira/última faixa, 5 início de uma faixa, F nada);
//   RS8: tipo da faixa (4 = dados); RS9: soma de verificação (complemento da soma de RS0-RS8, 4 bits).
// A cabeça anda um setor a cada 1/75 s; ao ler dados, o setor vai para o CDC. A busca leva um tempo fixo mais
// um tempo proporcional à distância (aproximado).
#include <cstdlib>
#include <cstring>

#include "segacd.h"

namespace indigo::segacd {

namespace {
enum : uint8_t { ST_STOP = 0, ST_PLAY = 1, ST_SEEK = 2, ST_SCAN = 3, ST_PAUSE = 4, ST_OPEN = 5, ST_END = 0x0C };
constexpr int SCAN_SECTORS = 30;        // setores por quadro no avanço/volta rápido
constexpr int SEEK_BASE = 12;           // quadros fixos de uma busca
constexpr int SEEK_FULL = 120;          // quadros a mais para atravessar o disco inteiro
constexpr int DISC_SECTORS = 270000;    // 60 minutos
uint8_t toBcd(int n) { n = std::abs(n) % 100; return static_cast<uint8_t>(((n / 10) << 4) | (n % 10)); }
}  // namespace

void CdDrive::attach(cdrom::Disc* d) {
  disc_ = d;
  tracks_.clear();
  const auto& t = d->tracks();
  const size_t n = std::min<size_t>(t.size(), 99);
  leadOut_ = static_cast<int>(d->totalSectors());
  for (size_t i = 0; i < n; i++) {
    TrackInfo ti;
    ti.first = static_cast<int>(t[i].start);
    // a faixa vai até o começo do pregap da seguinte (ou até o fim do disco)
    ti.end = i + 1 < n ? static_cast<int>(t[i + 1].start - t[i + 1].pregap) : leadOut_;
    ti.data = t[i].type != cdrom::TrackType::Audio;
    ti.mode = t[i].type == cdrom::TrackType::Mode1 ? 1 : 2;
    tracks_.push_back(ti);
  }
}

void CdDrive::reset() {
  wait_ = track_ = head_ = 0;
  state_ = idleState();
  queued_ = 0;
  fadeNow = fadeTarget = 0x400;
  audioSample_ = 0;
  muted = true;
  sectorLba_ = -1;
}

// RS2-RS7: minutos, segundos e quadros em BCD (um nibble por registrador)
void CdDrive::putTime(int sectors) {
  sectors = std::abs(sectors);
  const uint8_t m = toBcd(sectors / (75 * 60)), s = toBcd((sectors / 75) % 60), f = toBcd(sectors % 75);
  rs[2] = m >> 4; rs[3] = m & 15; rs[4] = s >> 4; rs[5] = s & 15; rs[6] = f >> 4; rs[7] = f & 15;
}

// resposta de posição: 0 tempo absoluto, 1 tempo dentro da faixa, 2 número da faixa
void CdDrive::reportPosition(int kind) {
  const uint8_t type = dataTrack(track_) ? 4 : 0;
  if (kind == 0) { putTime(head_ + 150); rs[8] = type; }
  else if (kind == 1) { putTime(head_ - firstSector(track_)); rs[8] = type; }
  else if (kind == 2) {
    if (track_ < tracks()) { const uint8_t b = toBcd(track_ + 1); rs[2] = b >> 4; rs[3] = b & 15; }
    else rs[2] = rs[3] = 0x0A;
  }
}

void CdDrive::finishReply() {
  int sum = 0;
  for (int i = 0; i < 9; i++) sum += rs[i];
  rs[9] = static_cast<uint8_t>(~sum & 0x0F);
}

void CdDrive::command() {
  // resposta curta de "parado/buscando": só RS0, sem informação
  auto blank = [&](uint8_t st) {
    std::memset(rs, 0, sizeof rs);
    rs[0] = st;
    rs[1] = 0x0F;
  };
  switch (cs[0] & 0x0F) {
    case 0x0:  // pedir estado: repete a última informação pedida, com os valores de agora
      if (wait_ <= 3) {
        rs[0] = state_;
        if (state_ != ST_STOP && state_ <= ST_PAUSE) {
          if (rs[1] == 0x0F) rs[1] = 0;
          if (rs[1] <= 2) reportPosition(rs[1]);
        }
      }
      break;
    case 0x1:
    case 0xC:  // parar / fechar a bandeja
      muted = true;
      state_ = idleState();
      blank(ST_STOP);
      track_ = head_ = 0;
      break;
    case 0xD:  // abrir a bandeja
      muted = true;
      state_ = ST_OPEN;
      blank(ST_OPEN);
      track_ = head_ = 0;
      break;
    case 0x2: {  // TOC
      const int what = cs[3];
      if (what > 6) return;  // pedido desconhecido: nada muda
      rs[0] = state_;
      rs[1] = static_cast<uint8_t>(what);
      for (int i = 2; i <= 8; i++) rs[i] = 0;
      if (what <= 2) reportPosition(what);
      else if (what == 3) putTime(leadOut_ + 150);
      else if (what == 4) { const uint8_t last = toBcd(tracks()); rs[3] = 1; rs[4] = last >> 4; rs[5] = last & 15; }
      else if (what == 5) {
        int n = cs[4] * 10 + cs[5];
        if (n < 1) n = 1;
        if (n > tracks() + 1) n = tracks() + 1;
        putTime(firstSector(n - 1) + 150);
        if (dataTrack(n - 1)) rs[6] |= 0x08;
        rs[8] = static_cast<uint8_t>(n % 10);
      }
      break;
    }
    case 0x3: blank(ST_SEEK); queued_ = ST_PLAY; break;
    case 0x4: blank(ST_SEEK); queued_ = ST_PAUSE; break;
    case 0x6: case 0xA: muted = true; state_ = ST_PAUSE; rs[0] = state_; break;
    case 0x7: state_ = ST_PLAY; rs[0] = state_; break;
    case 0x8: scanStep_ = SCAN_SECTORS; state_ = ST_SCAN; rs[0] = state_; break;
    case 0x9: scanStep_ = -SCAN_SECTORS; state_ = ST_SCAN; rs[0] = state_; break;
    default: rs[0] = state_; break;
  }
  finishReply();
}

CdDrive::Frame CdDrive::tick() {
  Frame f;
  if (wait_ > 0) {
    wait_--;
  } else if (state_ == ST_PLAY) {
    if (track_ >= tracks()) { state_ = ST_END; return f; }
    f.toDecoder = true;
    if (dataTrack(track_)) {
      const int t = head_ + 150;
      f.header[0] = toBcd(t / (75 * 60)); f.header[1] = toBcd((t / 75) % 60); f.header[2] = toBcd(t % 75);
      f.header[3] = tracks_[track_].mode;
      if (head_ >= 0) f.dataLba = head_;
    } else if (head_ >= firstSector(track_)) {
      muted = false;  // passou do pregap: o som da faixa começa
    }
    head_++;
    if (head_ >= endSector(track_)) {
      cueAudio(firstSector(track_ + 1));
      track_++;
      muted = true;
    }
  } else {
    // parado, em pausa ou varrendo: o CDC recebe setores sem header (a cabeça fica sobre o mesmo setor)
    f.toDecoder = true;
    if (dataTrack(track_) && head_ >= 0) f.dataLba = head_;
    if (state_ == ST_SCAN) {
      int t = track_;
      head_ += scanStep_;
      if (head_ >= endSector(t)) {
        if (++t >= tracks()) {
          head_ = leadOut_; track_ = tracks(); state_ = ST_END; muted = true;
          return f;
        }
        head_ = firstSector(t);
      } else if (head_ < firstSector(t)) {
        if (t > 0) head_ = endSector(--t);
        else head_ = 0;
      }
      if (dataTrack(t)) muted = true;
      else { cueAudio(head_); muted = false; }
      track_ = t;
    }
  }
  if (queued_) {
    // a busca pedida começa: o destino vem de CS2-CS7 (MSF)
    const int target = ((cs[2] * 10 + cs[3]) * 60 + (cs[4] * 10 + cs[5])) * 75 + (cs[6] * 10 + cs[7]) - 150;
    if (!wait_) wait_ = SEEK_BASE;
    wait_ += std::abs(target - head_) * SEEK_FULL / DISC_SECTORS;
    head_ = target;
    int t = 0;
    while (t < tracks() && endSector(t) <= target) t++;
    if (!dataTrack(t)) cueAudio(target < firstSector(t) ? firstSector(t) : target);
    track_ = t;
    muted = true;
    state_ = queued_;
    queued_ = 0;
  }
  return f;
}

void CdDrive::readAudio(int n, std::vector<int16_t>& out) {
  out.assign(static_cast<size_t>(n) * 2, 0);
  if (muted || track_ >= tracks()) return;
  int level = fadeNow;
  for (int i = 0; i < n; i++) {
    const int64_t s = audioSample_++;
    const int lba = static_cast<int>(s / 588);
    if (lba != sectorLba_) {
      if (!disc_ || !disc_->readRaw(static_cast<uint32_t>(lba), sector_)) std::memset(sector_, 0, sizeof sector_);
      sectorLba_ = lba;
    }
    const uint8_t* p = &sector_[(s % 588) * 4];
    // volume do fader: 0x400 = 0 dB (10 bits de atenuação; os 2 de baixo só valem perto do silêncio)
    const int gain = (level & 0x7FC) ? (level & 0x7FC) : (level & 3);
    out[i * 2] = static_cast<int16_t>(static_cast<int16_t>(p[0] | (p[1] << 8)) * gain / 1024);
    out[i * 2 + 1] = static_cast<int16_t>(static_cast<int16_t>(p[2] | (p[3] << 8)) * gain / 1024);
    level += (level < fadeTarget) - (level > fadeTarget);  // o fader anda um passo por amostra
  }
  fadeNow = static_cast<uint16_t>(level);
}

void CdDrive::sync(StateIO& io) {
  io.raw(rs, sizeof rs); io.raw(cs, sizeof cs);
  io.sync(muted); io.sync(fadeNow); io.sync(fadeTarget);
  io.sync(state_); io.sync(queued_); io.sync(wait_); io.sync(track_); io.sync(head_); io.sync(scanStep_);
  io.sync(audioSample_);
  if (io.reading()) {
    for (auto& b : rs) b &= 0x0F;
    fadeNow &= 0xFFF; fadeTarget &= 0xFFF;
    if (track_ < 0 || track_ > tracks()) track_ = 0;
    clampTo(wait_, 0, 100000);
    clampTo(scanStep_, -1000, 1000);
    clampTo(head_, -1000000, 1000000);
    clampTo<int64_t>(audioSample_, 0, int64_t(leadOut_ + 75) * 588);
    if (queued_ > ST_END) queued_ = 0;
    sectorLba_ = -1;
  }
}

}  // namespace indigo::segacd
