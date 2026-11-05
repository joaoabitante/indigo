// Unidade de GD-ROM do Dreamcast: interface ATA (0x005F7018 e 0x005F7080-0x005F709C) com pacotes de 12 bytes
// (comando 0xA0). Pacotes tratados: 0x00 TEST_UNIT, 0x10 REQ_STAT, 0x11 REQ_MODE, 0x12 SET_MODE, 0x13 REQ_ERROR,
// 0x14 GET_TOC (área de baixa e de alta densidade), 0x15 REQ_SES, 0x16 abrir a bandeja, 0x20 CD_PLAY, 0x21 CD_SEEK,
// 0x30/0x31 CD_READ (FAD ou MSF; dados de 2048 bytes, Form 2 de 2324 ou o setor bruto de 2352), 0x40 GET_SCD
// (subcódigo Q) e 0x70/0x71 (autenticação do disco: só respondem que terminaram, sem os dados de segurança).
// Comandos ATA: 0x08 reinício, 0xA1 identificação, 0xEF SET FEATURES. Leitura por PIO (fases de até 32 KB) ou por
// DMA do G1 (SB_GDSTAR/GDLEN/GDST), com a interrupção do GD-ROM no bit 0 do ISTEXT do Holly.
// Endereços do disco em FAD (LBA + 150); a área de alta densidade de um GD começa no LBA 45000.
#include <algorithm>

#include "dc.h"

namespace indigo::dc {

namespace {
enum : uint8_t { BSY = 0x80, DRDY = 0x40, DSC = 0x10, DRQ = 0x08, CHECK = 0x01 };
constexpr uint32_t PHASE = 0x8000;
}  // namespace

Gdrom::Gdrom(System& s) : sys(s) {}

void Gdrom::reset() {
  status = DRDY | DSC; error = 0; features = 0; ireason = 0; sectorCount = 0; drvSel = 0; devCtrl = 0;
  byteCount = 0; packetPos = 0; state = 0; buf.clear(); bufPos = phaseEnd = 0;
  readFad = readLeft = 0; readSecSize = 2048; readSel = 0; dmaMode = false;
  driveState = disc ? 1 : 7; curFad = 150; playStart = playEnd = 0; playRepeat = 0;
  std::memset(mode_, 0, sizeof mode_);
  // REQ_MODE: identificação da unidade
  std::memcpy(mode_ + 0x12, "SE      ", 8);
  sense = asc = 0;
  cddaPos_ = 588;
  pendingPacket_ = false;
  gd = false;
  if (disc)
    for (const auto& t : disc->tracks()) if (t.start >= 45000) gd = true;
}

int Gdrom::discFormat() const {
  if (!disc) return 0;
  if (gd) return 8;
  for (const auto& t : disc->tracks()) if (t.type == cdrom::TrackType::Mode2) return 2;
  return 1;
}

void Gdrom::raise() {
  if (!(devCtrl & 2)) sys.setExt(0, true);
}

void Gdrom::complete(bool check) {
  state = 0;
  status = DRDY | DSC | (check ? CHECK : 0);
  if (check) error = uint8_t(sense << 4);
  ireason = 3;
  buf.clear(); bufPos = phaseEnd = 0;
  raise();
}

void Gdrom::sendData(const uint8_t* d, size_t n, size_t alloc) {
  n = std::min(n, alloc);
  if (!n) { complete(); return; }
  buf.assign(d, d + n);
  bufPos = 0;
  state = 2;
  phaseEnd = uint32_t(std::min<size_t>(n, PHASE));
  byteCount = uint16_t(phaseEnd);
  status = DRDY | DRQ;
  ireason = 2;
  raise();
}

// próxima fase de dados da leitura PIO (ou o fim)
void Gdrom::nextPhase() {
  if (bufPos < buf.size()) {
    phaseEnd = uint32_t(std::min<size_t>(buf.size(), size_t(bufPos) + PHASE));
    byteCount = uint16_t(phaseEnd - bufPos);
    status = DRDY | DRQ;
    ireason = 2;
    raise();
    return;
  }
  complete();
}

bool Gdrom::fillRead() {
  buf.clear();
  uint8_t raw[cdrom::RAW_SECTOR];
  const uint32_t n = std::min<uint32_t>(readLeft, 8192);
  buf.reserve(size_t(n) * readSecSize);
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t lba = readFad + i - 150;
    if (!disc || !disc->readRaw(lba, raw)) return false;
    reads++;
    const uint8_t* src;
    if (readSecSize == 2352) src = raw;
    else if (raw[15] == 2) src = readSecSize == 2324 ? raw + 24 : raw + 24;
    else src = raw + 16;
    buf.insert(buf.end(), src, src + readSecSize);
  }
  curFad = readFad + n;
  readLeft = 0;
  return true;
}

void Gdrom::spi() {
  const uint8_t* p = packet;
  const uint8_t cmd = p[0];
  uint8_t r[512];
  std::memset(r, 0, sizeof r);
  // REQ_ERROR devolve o erro do comando anterior; os outros comandos começam sem erro
  const uint8_t prevSense = sense, prevAsc = asc;
  sense = 0; asc = 0;
  auto checkFail = [&](uint8_t key, uint8_t code) { sense = key; asc = code; complete(true); };
  if (!disc && cmd != 0x11 && cmd != 0x12 && cmd != 0x13 && cmd != 0x10) { checkFail(2, 0x3A); return; }
  switch (cmd) {
    case 0x00: complete(); return;
    case 0x10: {  // REQ_STAT
      const int ti = disc ? disc->trackAt(curFad - 150) : -1;
      r[0] = driveState;
      r[1] = uint8_t(discFormat() << 4);
      r[2] = (ti >= 0 && disc->tracks()[size_t(ti)].type == cdrom::TrackType::Audio) ? 0x01 : 0x41;
      r[3] = uint8_t(ti >= 0 ? disc->tracks()[size_t(ti)].number : 0);
      r[4] = 1;
      r[5] = uint8_t(curFad >> 16); r[6] = uint8_t(curFad >> 8); r[7] = uint8_t(curFad);
      const size_t off = std::min<size_t>(p[2], 10);
      sendData(r + off, 10 - off, p[4]);
      return;
    }
    case 0x11: {  // REQ_MODE
      const size_t off = std::min<size_t>(p[2], 32);
      sendData(mode_ + off, 32 - off, p[4]);
      return;
    }
    case 0x12: {  // SET_MODE: dados vêm da CPU
      const size_t off = std::min<size_t>(p[2], 32);
      const size_t n = std::min<size_t>(p[4], 32 - off);
      if (!n) { complete(); return; }
      buf.assign(n, 0);
      bufPos = 0; phaseEnd = uint32_t(n);
      setModeOff_ = uint32_t(off);
      state = 3;
      byteCount = uint16_t(n);
      status = DRDY | DRQ;
      ireason = 0;
      raise();
      return;
    }
    case 0x13:  // REQ_ERROR
      r[0] = 0xF0; r[2] = prevSense; r[8] = prevAsc;
      sendData(r, 10, p[4]);
      return;
    case 0x14: {  // GET_TOC
      const int area = p[1] & 1;
      if (area == 1 && !gd) { checkFail(5, 0x24); return; }
      std::memset(r, 0xFF, 408);
      int first = 100, last = 0;
      uint32_t leadout = 0;
      uint8_t lastCtrl = 0x41;
      for (const auto& t : disc->tracks()) {
        if (t.number < 1 || t.number > 99) continue;  // a tabela só tem 99 entradas
        const bool hi = t.start >= 45000;
        if (gd && hi != (area == 1)) continue;
        const uint32_t fad = t.start + 150;
        const uint8_t ctrl = t.type == cdrom::TrackType::Audio ? 0x01 : 0x41;
        uint8_t* e = r + 4 * (t.number - 1);
        e[0] = uint8_t(fad >> 16); e[1] = uint8_t(fad >> 8); e[2] = uint8_t(fad); e[3] = ctrl;
        first = std::min(first, t.number); last = std::max(last, t.number);
        leadout = std::max(leadout, fad + t.length);
        lastCtrl = ctrl;
      }
      if (last == 0) { checkFail(5, 0x24); return; }
      auto ctrlOf = [&](int n) {
        for (const auto& t : disc->tracks()) if (t.number == n) return t.type == cdrom::TrackType::Audio ? uint8_t(0x01) : uint8_t(0x41);
        return uint8_t(0x41);
      };
      r[396] = 0; r[397] = 0; r[398] = uint8_t(first); r[399] = ctrlOf(first);
      r[400] = 0; r[401] = 0; r[402] = uint8_t(last); r[403] = ctrlOf(last);
      r[404] = uint8_t(leadout >> 16); r[405] = uint8_t(leadout >> 8); r[406] = uint8_t(leadout); r[407] = lastCtrl;
      sendData(r, 408, size_t(p[3]) << 8 | p[4]);
      return;
    }
    case 0x15: {  // REQ_SES
      const int ses = p[2];
      const auto& tr = disc->tracks();
      r[0] = driveState;
      if (ses == 0) {
        r[2] = gd ? 2 : 1;
        const uint32_t lo = disc->totalSectors() + 150;
        r[3] = uint8_t(lo >> 16); r[4] = uint8_t(lo >> 8); r[5] = uint8_t(lo);
      } else {
        // GD: sessão 1 = área de baixa densidade, 2 = a de alta; CD: uma sessão só
        const cdrom::Track* t0 = nullptr;
        for (const auto& t : tr)
          if (!gd ? ses == 1 : (ses == 1 ? t.start < 45000 : (ses == 2 && t.start >= 45000))) { t0 = &t; break; }
        if (!t0) { checkFail(5, 0x24); return; }
        const uint32_t fad = t0->start + 150;
        r[2] = uint8_t(t0->number);
        r[3] = uint8_t(fad >> 16); r[4] = uint8_t(fad >> 8); r[5] = uint8_t(fad);
      }
      sendData(r, 6, p[4]);
      return;
    }
    case 0x16: complete(); return;
    case 0x20: {  // CD_PLAY
      const int type = p[1] & 7;
      auto addr = [&](int o) -> uint32_t {
        if (type == 2) return cdrom::msfToLba(p[o], p[o + 1], p[o + 2]) + 150;
        return uint32_t(p[o]) << 16 | uint32_t(p[o + 1]) << 8 | p[o + 2];
      };
      if (type != 7) { playStart = addr(2); playEnd = addr(8); curFad = playStart; }
      playRepeat = p[6] & 15;
      driveState = 3;
      cddaPos_ = 588;
      complete();
      return;
    }
    case 0x21: {  // CD_SEEK
      const int type = p[1] & 15;
      if (type == 1) curFad = uint32_t(p[2]) << 16 | uint32_t(p[3]) << 8 | p[4];
      else if (type == 2) curFad = cdrom::msfToLba(p[2], p[3], p[4]) + 150;
      driveState = type == 3 ? 2 : 1;
      complete();
      return;
    }
    case 0x30: case 0x31: {  // CD_READ
      const bool msf = p[1] & 1;
      const int sel = p[1] >> 4;
      readFad = msf ? cdrom::msfToLba(p[2], p[3], p[4]) + 150 : (uint32_t(p[2]) << 16 | uint32_t(p[3]) << 8 | p[4]);
      readLeft = uint32_t(p[8]) << 16 | uint32_t(p[9]) << 8 | p[10];
      const int exp = (p[1] >> 1) & 7;
      readSecSize = sel == 0xF ? 2352 : (exp == 4 ? 2324 : 2048);
      readSel = uint8_t(sel);
      if (!readLeft) { complete(); return; }
      if (readFad < 150 || !fillRead()) { checkFail(5, 0x21); return; }
      driveState = 1;
      if (dmaMode) {
        bufPos = 0; phaseEnd = uint32_t(buf.size());
        state = 4;
        status = DRDY | DRQ;
        ireason = 2;
        return;  // espera o SB_GDST
      }
      bufPos = 0;
      state = 2;
      nextPhase();
      return;
    }
    case 0x40: {  // GET_SCD
      const int fmt = p[1] & 15;
      const int ti = disc->trackAt(curFad - 150);
      const uint8_t as = driveState == 3 ? 0x11 : driveState == 1 ? 0x12 : 0x15;
      if (fmt == 1) {
        r[1] = as; r[3] = 14;
        r[4] = (ti >= 0 && disc->tracks()[size_t(ti)].type == cdrom::TrackType::Audio) ? 0x01 : 0x41;
        r[5] = uint8_t(ti >= 0 ? disc->tracks()[size_t(ti)].number : 0);
        r[6] = 1;
        const uint32_t rel = ti >= 0 && curFad - 150 >= disc->tracks()[size_t(ti)].start ? curFad - 150 - disc->tracks()[size_t(ti)].start : 0;
        r[7] = uint8_t(rel >> 16); r[8] = uint8_t(rel >> 8); r[9] = uint8_t(rel);
        r[11] = uint8_t(curFad >> 16); r[12] = uint8_t(curFad >> 8); r[13] = uint8_t(curFad);
        sendData(r, 14, size_t(p[3]) << 8 | p[4]);
      } else {
        r[1] = as; r[3] = 100;
        sendData(r, 100, size_t(p[3]) << 8 | p[4]);
      }
      return;
    }
    case 0x70: case 0x71: complete(); return;
    default: checkFail(5, 0x20); return;
  }
}

void Gdrom::command(uint8_t c) {
  switch (c) {
    case 0xA0:  // PACKET
      state = 1; packetPos = 0;
      status = DRDY | DRQ;
      ireason = 1;
      return;
    case 0xA1: {  // IDENTIFY: identificação da unidade (80 bytes)
      uint8_t id[80] = {};
      id[0] = 0x00; id[1] = 0xB4; id[2] = 0x19; id[3] = 0x00;
      std::memcpy(id + 8, "SE      ", 8);
      std::memcpy(id + 16, "CD-ROM DRIVE    ", 16);
      std::memcpy(id + 32, "6.43    ", 8);
      sendData(id, 80, 80);
      return;
    }
    case 0xEF: complete(); return;
    case 0x08: status = DRDY | DSC; error = 0; state = 0; return;
    default:
      error = 0x04;
      state = 0; status = DRDY | CHECK; ireason = 3;
      raise();
      return;
  }
}

uint32_t Gdrom::read(uint32_t off, int size) {
  switch (off) {
    case 0x18: return status;
    case 0x80: {
      if (state != 2) return 0;
      uint32_t v = 0;
      const int n = size >= 2 ? 2 : 1;
      for (int i = 0; i < n; i++) if (bufPos < buf.size()) v |= uint32_t(buf[bufPos++]) << (8 * i);
      if (bufPos >= phaseEnd) nextPhase();
      return v;
    }
    case 0x84: return error;
    case 0x88: return ireason;
    case 0x8C: return uint32_t(discFormat() << 4 | (driveState & 15));
    case 0x90: return byteCount & 0xFF;
    case 0x94: return byteCount >> 8;
    case 0x98: return drvSel;
    case 0x9C: sys.setExt(0, false); return status;
    default: return 0;
  }
}

void Gdrom::write(uint32_t off, uint32_t v, int size) {
  switch (off) {
    case 0x18: devCtrl = uint8_t(v); return;
    case 0x80:
      if (state == 1) {
        packet[packetPos++ % 12] = uint8_t(v);
        if (size >= 2 && packetPos < 12) packet[packetPos++] = uint8_t(v >> 8);
        if (packetPos >= 12) {
          state = 0; packetPos = 0;
          status = BSY;
          pendingPacket_ = true;
          sys.schedule(System::EV_GDROM, 4000);
        }
      } else if (state == 3) {
        if (bufPos >= buf.size()) { complete(); return; }
        buf[bufPos++] = uint8_t(v);
        if (size >= 2 && bufPos < buf.size()) buf[bufPos++] = uint8_t(v >> 8);
        if (bufPos >= buf.size()) {
          for (size_t i = 0; i < buf.size() && setModeOff_ + i < 32; i++) mode_[setModeOff_ + i] = buf[i];
          complete();
        }
      }
      return;
    case 0x84: features = uint8_t(v); dmaMode = v & 1; return;
    case 0x88: sectorCount = uint8_t(v); return;
    case 0x90: byteCount = uint16_t((byteCount & 0xFF00) | (v & 0xFF)); return;
    case 0x94: byteCount = uint16_t((byteCount & 0xFF) | (v & 0xFF) << 8); return;
    case 0x98: drvSel = uint8_t(v); return;
    case 0x9C: command(uint8_t(v)); return;
    default: return;
  }
}

void Gdrom::event() {
  if (pendingPacket_) { pendingPacket_ = false; spi(); }
}

void Gdrom::dmaTransfer() {
  uint32_t* sb = sys.sb;
  auto SBr = [&](uint32_t o) -> uint32_t& { return sb[(o - 0x6800) >> 2]; };
  const uint32_t dst = SBr(0x7404) & 0x1FFFFFE0, len = SBr(0x7408);
  uint32_t n = 0;
  if (state == 4) {
    n = std::min<uint32_t>(len, uint32_t(buf.size()) - bufPos);
    for (uint32_t i = 0; i < n; i++) sys.write8(dst + i, buf[bufPos + i]);
    bufPos += n;
  }
  SBr(0x74F4) = dst + n;
  SBr(0x74F8) = n;
  sys.schedule(System::EV_GDDMA, 2000 + n / 8);
}

void Gdrom::dmaDone() {
  if (state == 4 && bufPos >= buf.size()) complete();
}

void Gdrom::cddaSamples(int16_t* lr, int n) {
  for (int i = 0; i < n; i++) {
    if (driveState != 3 || !disc) { lr[2 * i] = lr[2 * i + 1] = 0; continue; }
    if (cddaPos_ >= 588) {
      if (curFad >= playEnd) {
        if (playRepeat > 0) { if (playRepeat < 15) playRepeat--; curFad = playStart; }
        else { driveState = 1; lr[2 * i] = lr[2 * i + 1] = 0; continue; }
      }
      uint8_t raw[cdrom::RAW_SECTOR];
      const int ti = disc->trackAt(curFad - 150);
      if (ti >= 0 && disc->tracks()[size_t(ti)].type == cdrom::TrackType::Audio && disc->readRaw(curFad - 150, raw))
        std::memcpy(cddaBuf_, raw, sizeof cddaBuf_);
      else
        std::memset(cddaBuf_, 0, sizeof cddaBuf_);
      curFad++;
      cddaPos_ = 0;
    }
    lr[2 * i] = cddaBuf_[2 * cddaPos_];
    lr[2 * i + 1] = cddaBuf_[2 * cddaPos_ + 1];
    cddaPos_++;
  }
}

void Gdrom::sync(StateIO& io) {
  io.sync(status); io.sync(error); io.sync(features); io.sync(ireason); io.sync(sectorCount); io.sync(drvSel);
  io.sync(devCtrl); io.sync(byteCount); io.raw(packet, sizeof packet); io.sync(packetPos); io.sync(state);
  uint32_t n = uint32_t(buf.size());
  io.sync(n);
  if (io.reading()) { if (n > 8192u * 2352u) throw std::runtime_error("Estado salvo inválido (GD-ROM)."); buf.resize(n); }
  if (n) io.raw(buf.data(), n);
  io.sync(bufPos); io.sync(phaseEnd); io.sync(readFad); io.sync(readLeft); io.sync(readSecSize); io.sync(readSel);
  io.sync(dmaMode); io.sync(driveState); io.sync(curFad); io.sync(playStart); io.sync(playEnd); io.sync(playRepeat);
  io.raw(mode_, sizeof mode_); io.sync(sense); io.sync(asc); io.raw(cddaBuf_, sizeof cddaBuf_); io.sync(cddaPos_);
  io.sync(pendingPacket_); io.sync(setModeOff_); io.sync(reads);
  if (io.reading()) {
    clampTo(packetPos, 0, 11); clampTo(state, 0, 4); clampTo(cddaPos_, 0, 588);
    if (bufPos > buf.size()) bufPos = uint32_t(buf.size());
    if (phaseEnd > buf.size()) phaseEnd = uint32_t(buf.size());
    if ((state == 3 || state == 4) && bufPos >= buf.size()) state = 0;
    if (state == 2 && bufPos > phaseEnd) bufPos = phaseEnd;
    if (setModeOff_ > 32) setModeOff_ = 32;
    if (readSecSize != 2048 && readSecSize != 2324 && readSecSize != 2352) readSecSize = 2048;
  }
}

}  // namespace indigo::dc
