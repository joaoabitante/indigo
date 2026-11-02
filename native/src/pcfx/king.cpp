// Partes adaptadas do Mednafen (Beetle PC-FX, Mednafen Team, GPL-2.0 ou posterior); veja native/TERCEIROS.md.
#include "king.h"

#include <algorithm>
#include <cstring>

namespace indigo::pcfx {

namespace {
constexpr int DMA_STEP = 10;  // o KING olha o barramento SCSI a cada 10 ciclos durante o DMA
template <typename T>
inline void regSet(T& r, uint16_t v, bool msh) {
  if (msh) r = T((uint32_t(r) & 0x0000FFFFu) | (uint32_t(v) << 16));
  else r = T((uint32_t(r) & 0xFFFF0000u) | v);
}
template <typename T>
inline uint16_t regGet(T r, bool msh) { return uint16_t(msh ? uint32_t(r) >> 16 : uint32_t(r)); }
inline int32_t signX(int bits, uint32_t v) { return int32_t(v << (32 - bits)) >> (32 - bits); }
}  // namespace

// ------------------------------------------------------------------ HuC6261
void FxVce::reset() {
  ar = 0; pictureMode = 0; palOffsetRw = palLatch = 0;
  std::memset(palOffset, 0, sizeof palOffset);
  std::memset(priority, 0, sizeof priority);
  chromaY = chromaU = chromaV = ccr = ble = spbl = 0;
  std::memset(coef, 0, sizeof coef);
  std::memset(pal, 0, sizeof pal);
  rebuild();
}

uint16_t FxVce::read16(uint32_t a) {
  if (!(a & 4)) {
    // registro de estado: número do registro, contador de linha, campo ímpar e "imagem ativa"
    const bool active = !(inHblank || raster < 22 || raster == 262);
    return uint16_t(ar | (oddField ? 0x4000 : 0) | ((raster & 0x1FF) << 5) | (active ? 0x8000 : 0));
  }
  switch (ar) {
    case 0x00: return pictureMode;
    case 0x01: return palOffsetRw;
    case 0x02: case 0x03: {
      const uint16_t r = palLatch;
      palOffsetRw = (palOffsetRw + 1) & 0x1FF;
      palLatch = pal[palOffsetRw];
      return r;
    }
    case 0x04: case 0x05: case 0x06: case 0x07: return palOffset[ar - 4];
    case 0x08: return priority[0];
    case 0x09: return priority[1];
    case 0x0A: return chromaY;
    case 0x0B: return chromaU;
    case 0x0C: return chromaV;
    case 0x0D: return ccr;
    case 0x0E: return ble;
    case 0x0F: return spbl;
    default: return ar >= 0x10 && ar <= 0x15 ? coef[ar - 0x10] : 0;
  }
}

void FxVce::write16(uint32_t a, uint16_t v) {
  if (!(a & 4)) { ar = v & 0x1F; return; }
  switch (ar) {
    case 0x00: pictureMode = v; break;
    case 0x01: palOffsetRw = v & 0x1FF; palLatch = pal[palOffsetRw]; break;
    case 0x02:
      palLatch = v;
      pal[palOffsetRw] = v;
      palCache(palOffsetRw);
      palOffsetRw = (palOffsetRw + 1) & 0x1FF;
      break;
    case 0x04: palOffset[0] = v; break;
    case 0x05: palOffset[1] = v; break;
    case 0x06: palOffset[2] = v; break;
    case 0x07: palOffset[3] = v & 0xFF; break;
    case 0x08: priority[0] = v & 0x0777; break;
    case 0x09: priority[1] = v & 0x7777; break;
    case 0x0A: chromaY = v; break;
    case 0x0B: chromaU = v; break;
    case 0x0C: chromaV = v; break;
    case 0x0D: ccr = v; break;
    case 0x0E: ble = v; break;
    case 0x0F: spbl = v; break;
    default: if (ar >= 0x10 && ar <= 0x15) coef[ar - 0x10] = v & 0xFFF; break;
  }
}

void FxVce::sync(StateIO& io) {
  io.sync(ar); io.sync(pictureMode); io.sync(palOffsetRw); io.sync(palLatch);
  io.raw(palOffset, sizeof palOffset); io.raw(priority, sizeof priority);
  io.sync(chromaY); io.sync(chromaU); io.sync(chromaV); io.sync(ccr); io.sync(ble); io.sync(spbl);
  io.raw(coef, sizeof coef); io.raw(pal, sizeof pal);
  io.sync(raster); io.sync(inHblank); io.sync(oddField);
  if (io.reading()) { ar &= 0x1F; palOffsetRw &= 0x1FF; rebuild(); }
}

// ------------------------------------------------------------------ KING
void King::reset(int64_t now) {
  std::memset(kram, 0xFF, sizeof kram);
  last_ = now;
  ar_ = 0; kramRA_ = kramWA_ = 0; kramMode_ = 0; page_ = 0;
  bgMode_ = priority_ = scrollMode_ = 0;
  std::memset(bgSize_, 0, sizeof bgSize_);
  std::memset(batAddr_, 0, sizeof batAddr_); std::memset(cgAddr_, 0, sizeof cgAddr_);
  subBat_ = subCg_ = 0;
  std::memset(xScroll_, 0, sizeof xScroll_); std::memset(yScroll_, 0, sizeof yScroll_);
  affA_ = affB_ = affC_ = affD_ = affX_ = affY_ = 0;
  mprogCtrl_ = mprogAddr_ = 0; std::memset(mprog_, 0, sizeof mprog_);
  adpcmControl_ = 0;
  for (int c = 0; c < 2; c++) { adpcmBufMode_[c] = adpcmSal_[c] = adpcmMid_[c] = adpcmStatus_[c] = 0; adpcmEnd_[c] = adpcmPlay_[c] = 0; }
  adpcmIrq_ = false;
  rbCtrl_ = rbStart_ = rbBlocks_ = rasterLine_ = 0; rbKram_ = 0; rasterIrq_ = false;
  reg01_ = reg02_ = reg03_ = 0;
  subCtrl_ = subBuf_ = dataCache_ = dmaLatch_ = 0;
  cdIrq_ = subIrq_ = dmaIrq_ = false;
  drq_ = dmaRecv_ = dmaSend_ = dmaFlip_ = false;
  dmaAddr_ = dmaSize_ = 0; dmaStatus_ = 0;
  dmaNext_ = NONE;
  cd_.power(now);
  cdNext_ = now;
  vce_.reset();
  redoIrq();
}

void King::redoIrq() {
  irq_ = adpcmIrq_ || (dmaIrq_ && (dmaStatus_ & 2)) || cdIrq_ || subIrq_ || rasterIrq_;
}

uint16_t King::scsiSignals() const {
  return uint16_t((cd_.sig(ScsiCd::SEL) ? 0x02 : 0) | (cd_.sig(ScsiCd::IO) ? 0x04 : 0) | (cd_.sig(ScsiCd::CD) ? 0x08 : 0) |
                  (cd_.sig(ScsiCd::MSG) ? 0x10 : 0) | (cd_.sig(ScsiCd::REQ) ? 0x20 : 0) | (cd_.sig(ScsiCd::BSY) ? 0x40 : 0) |
                  (cd_.sig(ScsiCd::RST) ? 0x80 : 0));
}

bool King::phaseMatch() const {
  return cd_.sig(ScsiCd::IO) == bool(reg03_ & 1) && cd_.sig(ScsiCd::CD) == bool(reg03_ & 2) && cd_.sig(ScsiCd::MSG) == bool(reg03_ & 4);
}

void King::cdStep(int64_t t) {
  cdNext_ = cd_.run(t);
  if (cd_.reqRose) {
    cd_.reqRose = false;
    // REQ subiu numa fase diferente da esperada (registro 3) com o modo DMA ligado: interrupção do CD
    if ((reg02_ & 2) && !phaseMatch()) { cdIrq_ = true; redoIrq(); }
  }
}

void King::realDma(uint8_t b) {
  if (!dmaFlip_) {
    dmaLatch_ = b;
  } else {
    kram[page_ & 1][dmaAddr_ & 0x3FFFF] = uint16_t(dmaLatch_ | (b << 8));
    dmaAddr_ = ((dmaAddr_ + 1) & 0x1FFFF) | (dmaAddr_ & 0x20000);
    dmaSize_ = (dmaSize_ - 2) & 0x3FFFF;
    if (!dmaSize_) {
      dmaIrq_ = true;
      dmaStatus_ &= ~1;
      redoIrq();
      return;
    }
  }
  dmaFlip_ = !dmaFlip_;
}

void King::dmaStep(int64_t t) {
  cdStep(t);
  if (dmaRecv_) {
    if (!cd_.sig(ScsiCd::CD) && cd_.sig(ScsiCd::IO)) {
      if (cd_.sig(ScsiCd::REQ) && !cd_.sig(ScsiCd::ACK)) {
        if (!drq_) {
          drq_ = true;
          dataCache_ = cd_.db();
          if (dmaStatus_ & 1) {
            drq_ = false;
            realDma(dataCache_);
            cd_.setSignal(ScsiCd::ACK, true);
            cdStep(t);
          }
        }
      } else if (cd_.sig(ScsiCd::ACK) && !cd_.sig(ScsiCd::REQ)) {
        cd_.setSignal(ScsiCd::ACK, false);
        cdStep(t);
      }
    }
  } else if (dmaSend_) {
    if (!cd_.sig(ScsiCd::IO)) {
      if (cd_.sig(ScsiCd::REQ) && !cd_.sig(ScsiCd::ACK)) {
        if (!drq_) {
          cd_.setDB(dataCache_);
          cd_.setSignal(ScsiCd::ACK, true);
          cdStep(t);
          drq_ = true;
        }
      } else if (cd_.sig(ScsiCd::ACK) && !cd_.sig(ScsiCd::REQ)) {
        cd_.setSignal(ScsiCd::ACK, false);
        cdStep(t);
      }
    }
  }
}

int64_t King::update(int64_t t) {
  for (;;) {
    const int64_t e = std::min(cdNext_, dmaNext_);
    if (e > t) break;
    if (dmaNext_ <= cdNext_) {
      dmaStep(dmaNext_);
      dmaNext_ = (dmaRecv_ || dmaSend_) ? dmaNext_ + DMA_STEP : NONE;
    } else {
      cdStep(cdNext_);
    }
  }
  cdStep(t);
  last_ = t;
  return std::min(cdNext_, dmaNext_);
}

uint16_t King::read16(int64_t now, uint32_t a) {
  update(now);
  const bool msh = a & 2;
  uint16_t ret = 0;
  if (!(a & 4)) {
    if (!msh) {
      // número do registro e as fontes de interrupção (lidas zeram a de linha e a de subcanal)
      ret = ar_;
      if (adpcmIrq_) ret |= 0x400;
      if (subIrq_) ret |= 0x1000;
      if (dmaIrq_ && (dmaStatus_ & 2)) ret |= 0x2000;
      else if (cdIrq_) ret |= 0x4000;
      if (rasterIrq_) ret |= 0x800;
      subIrq_ = false;
      rasterIrq_ = false;
      redoIrq();
    } else {
      ret = uint16_t(scsiSignals() | (subBuf_ << 8));
    }
    return ret;
  }
  switch (ar_) {
    case 0x00: ret = cd_.db(); break;
    case 0x01: ret = regGet(reg01_, msh); break;
    case 0x02: ret = regGet(reg02_, msh); break;
    case 0x03: ret = regGet(reg03_, msh); break;
    case 0x04: if (!msh) ret = scsiSignals(); break;
    case 0x05:
      if (reg01_ & 0x80) break;
      if (msh) {
        ret = dataCache_;
        if (dmaRecv_) {
          drq_ = false;
          cd_.setSignal(ScsiCd::ACK, true);
          cdStep(now);
        }
      } else {
        ret |= cd_.sig(ScsiCd::ACK) ? 0x01 : 0;
        ret |= cd_.sig(ScsiCd::ATN) ? 0x02 : 0;
        if ((dmaRecv_ || dmaSend_) && drq_) ret |= 0x40;
        if (!dmaIrq_ && cdIrq_) ret |= 0x10;
        if (phaseMatch()) ret |= 0x08;
      }
      break;
    case 0x06: ret = dataCache_; break;
    case 0x07: cdIrq_ = false; redoIrq(); ret = 0xFF; break;
    case 0x08: if (!msh) { ret = subBuf_; subBuf_ = 0; } break;
    case 0x09: ret = regGet(dmaAddr_, msh); break;
    case 0x0A: ret = regGet(dmaSize_, msh); break;
    case 0x0B: if (!msh) { ret = dmaIrq_ ? 1 : 0; dmaIrq_ = false; redoIrq(); } break;
    case 0x0C: ret = regGet(kramRA_, msh); break;
    case 0x0D: ret = regGet(kramWA_, msh); break;
    case 0x0E: {
      const int pg = (kramRA_ & 0x80000000u) ? 1 : 0;
      const int32_t inc = int32_t((kramRA_ & (0x3FFu << 18)) << 4) >> 22;
      ret = kram[pg][kramRA_ & 0x3FFFF];
      kramRA_ = (kramRA_ & ~0x1FFFFu) | ((kramRA_ + uint32_t(inc)) & 0x1FFFF);
      break;
    }
    case 0x0F: ret = uint16_t(page_); break;
    case 0x10: ret = regGet(bgMode_, msh); break;
    case 0x15: ret = mprogCtrl_; break;
    case 0x53:
      ret = uint16_t(adpcmStatus_[0] | (adpcmStatus_[1] << 2));
      adpcmStatus_[0] = adpcmStatus_[1] = 0;
      adpcmIrq_ = false;
      redoIrq();
      break;
    default: break;
  }
  return ret;
}

void King::write16(int64_t now, uint32_t a, uint16_t v) {
  const bool msh = a & 2;
  if (!(a & 4)) {
    if (!msh) ar_ = v & 0x7F;
    return;
  }
  update(now);
  auto reg2 = [&](uint8_t x) {
    if (!(x & 2) && (reg02_ & 2)) {  // sai do modo DMA
      cdIrq_ = false;
      redoIrq();
      cd_.setSignal(ScsiCd::ACK, false);
      drq_ = false;
      dmaRecv_ = dmaSend_ = false;
      dmaNext_ = NONE;
    }
    reg02_ = x;
  };
  switch (ar_) {
    case 0x00:
      if (!(reg01_ & 0x80) && !msh) { cd_.setDB(uint8_t(v)); cdStep(now); }
      break;
    case 0x01:
      if (!msh) {
        if (v & 0x80) {  // RST também zera os registros do controlador SCSI
          if (!(reg01_ & 0x80)) { cd_.setDB(0); reg2(0); reg03_ = 0; dataCache_ = 0; }
          reg01_ = v & 0x80;
        } else {
          reg01_ = v & (1 | 2 | 4 | 0x10 | 0x80);
          cd_.setSignal(ScsiCd::ATN, v & 2);
          cd_.setSignal(ScsiCd::SEL, v & 4);
          cd_.setSignal(ScsiCd::ACK, v & 0x10);
        }
        cd_.setSignal(ScsiCd::RST, v & 0x80);
        cdStep(now);
      }
      break;
    case 0x02: if (!(reg01_ & 0x80) && !msh) { reg2(uint8_t(v)); cdStep(now); } break;
    case 0x03: if (!(reg01_ & 0x80) && !msh) { reg03_ = v & 7; cdStep(now); } break;
    case 0x05:
      if (reg01_ & 0x80) break;
      if (!msh) {  // começa a enviar (DMA do iniciador para o alvo)
        dmaRecv_ = false;
        dmaSend_ = true;
        drq_ = true;
        dmaNext_ = now + DMA_STEP;
      } else if (dmaSend_ && drq_) {
        dataCache_ = uint8_t(v);
        drq_ = false;
      }
      break;
    case 0x07:
      if (reg01_ & 0x80) break;
      if (reg02_ & 2) {  // começa a receber
        dmaRecv_ = true;
        dmaSend_ = false;
        dmaNext_ = now + DMA_STEP;
      }
      break;
    case 0x08: subCtrl_ = v & 3; subIrq_ = false; redoIrq(); break;
    case 0x09: regSet(dmaAddr_, v, msh); dmaAddr_ &= 0x3FFFF; break;
    case 0x0A: regSet(dmaSize_, v, msh); dmaSize_ &= 0x3FFFE; dmaFlip_ = false; break;
    case 0x0B: regSet(dmaStatus_, v, msh); dmaStatus_ &= 3; dmaIrq_ = false; redoIrq(); dmaFlip_ = false; break;
    case 0x0C: regSet(kramRA_, v, msh); break;
    case 0x0D: regSet(kramWA_, v, msh); break;
    case 0x0E: {
      const int pg = (kramWA_ & 0x80000000u) ? 1 : 0;
      const int32_t inc = int32_t((kramWA_ & (0x3FFu << 18)) << 4) >> 22;
      kram[pg][kramWA_ & 0x3FFFF] = v;
      kramWA_ = (kramWA_ & ~0x1FFFFu) | ((kramWA_ + uint32_t(inc)) & 0x1FFFF);
      break;
    }
    case 0x0F: regSet(page_, v, msh); break;
    case 0x10: regSet(bgMode_, v, msh); break;
    case 0x12: if (!msh) priority_ = v; break;
    case 0x13: if (!msh) mprogAddr_ = v & 0xF; break;
    case 0x14: if (!msh) { mprog_[mprogAddr_] = v; mprogAddr_ = (mprogAddr_ + 1) & 0xF; } break;
    case 0x15: regSet(mprogCtrl_, v, msh); mprogCtrl_ &= 1; break;
    case 0x16: regSet(scrollMode_, v, msh); scrollMode_ &= 0xF; break;
    case 0x20: regSet(batAddr_[0], v, msh); break;
    case 0x21: regSet(cgAddr_[0], v, msh); break;
    case 0x22: regSet(subBat_, v, msh); break;
    case 0x23: regSet(subCg_, v, msh); break;
    case 0x24: regSet(batAddr_[1], v, msh); break;
    case 0x25: regSet(cgAddr_[1], v, msh); break;
    case 0x28: regSet(batAddr_[2], v, msh); break;
    case 0x29: regSet(cgAddr_[2], v, msh); break;
    case 0x2A: regSet(batAddr_[3], v, msh); break;
    case 0x2B: regSet(cgAddr_[3], v, msh); break;
    case 0x2C: regSet(bgSize_[0], v, msh); break;
    case 0x2D: regSet(bgSize_[1], v, msh); bgSize_[1] &= 0xFF; break;
    case 0x2E: regSet(bgSize_[2], v, msh); bgSize_[2] &= 0xFF; break;
    case 0x2F: regSet(bgSize_[3], v, msh); bgSize_[3] &= 0xFF; break;
    case 0x30: regSet(xScroll_[0], v, msh); xScroll_[0] &= 0x7FF; break;
    case 0x31: regSet(yScroll_[0], v, msh); yScroll_[0] &= 0x7FF; break;
    case 0x32: regSet(xScroll_[1], v, msh); xScroll_[1] &= 0x3FF; break;
    case 0x33: regSet(yScroll_[1], v, msh); yScroll_[1] &= 0x3FF; break;
    case 0x34: regSet(xScroll_[2], v, msh); xScroll_[2] &= 0x3FF; break;
    case 0x35: regSet(yScroll_[2], v, msh); yScroll_[2] &= 0x3FF; break;
    case 0x36: regSet(xScroll_[3], v, msh); xScroll_[3] &= 0x3FF; break;
    case 0x37: regSet(yScroll_[3], v, msh); yScroll_[3] &= 0x3FF; break;
    case 0x38: regSet(affA_, v, msh); break;
    case 0x39: regSet(affB_, v, msh); break;
    case 0x3A: regSet(affC_, v, msh); break;
    case 0x3B: regSet(affD_, v, msh); break;
    case 0x3C: regSet(affX_, v, msh); break;
    case 0x3D: regSet(affY_, v, msh); break;
    case 0x40:
      if (!msh) { rbCtrl_ = v & 3; }
      rasterIrq_ = false;
      redoIrq();
      break;
    case 0x41: regSet(rbKram_, v, msh); rbKram_ &= 0x3FFFF; break;
    case 0x42: if (!msh) rbStart_ = v & 0x1FF; break;
    case 0x43: regSet(rbBlocks_, v, msh); rbBlocks_ &= 0x1F; break;
    case 0x44: if (!msh) rasterLine_ = v & 0x1FF; break;
    case 0x50:
      if (!msh) {
        for (int c = 0; c < 2; c++)
          if (!(adpcmControl_ & (1 << c)) && (v & (1 << c))) adpcmPlay_[c] = uint32_t(adpcmSal_[c]) * 256;
        adpcmControl_ = v;
        redoIrq();
      }
      break;
    case 0x51: regSet(adpcmBufMode_[0], v, msh); redoIrq(); break;
    case 0x52: regSet(adpcmBufMode_[1], v, msh); redoIrq(); break;
    case 0x58: regSet(adpcmSal_[0], v, msh); adpcmSal_[0] &= 0x3FF; break;
    case 0x59: regSet(adpcmEnd_[0], v, msh); adpcmEnd_[0] &= 0x3FFFF; break;
    case 0x5A: regSet(adpcmMid_[0], v, msh); adpcmMid_[0] &= 0xFFF; break;
    case 0x5C: regSet(adpcmSal_[1], v, msh); adpcmSal_[1] &= 0x3FF; break;
    case 0x5D: regSet(adpcmEnd_[1], v, msh); adpcmEnd_[1] &= 0x3FFFF; break;
    case 0x5E: regSet(adpcmMid_[1], v, msh); adpcmMid_[1] &= 0xFFF; break;
    case 0x61: kramMode_ = v & 1; break;
    default: break;
  }
}

uint16_t King::adpcmHalfWord(int ch) {
  const int pg = (page_ & 0x100) ? 1 : 0;
  const uint16_t r = kram[pg][adpcmPlay_[ch] & 0x3FFFF];
  adpcmPlay_[ch] = (adpcmPlay_[ch] & 0x20000) | ((adpcmPlay_[ch] + 1) & 0x1FFFF);
  if (adpcmPlay_[ch] == (((adpcmEnd_[ch] + 1) & 0x1FFFF) | (adpcmEnd_[ch] & 0x20000))) {
    if (!(adpcmBufMode_[ch] & 1)) adpcmControl_ &= ~(1 << ch);  // fim: para
    else adpcmPlay_[ch] = uint32_t(adpcmSal_[ch]) << 8;        // repetição
    adpcmStatus_[ch] |= 1;
    if (adpcmBufMode_[ch] & 2) { adpcmIrq_ = true; redoIrq(); }
  } else if (adpcmPlay_[ch] == uint32_t(adpcmMid_[ch]) << 6) {
    adpcmStatus_[ch] |= 2;
    if (adpcmBufMode_[ch] & 4) { adpcmIrq_ = true; redoIrq(); }
  }
  return r;
}

void King::rasterLine(int raster) {
  if (raster == rasterLine_ && (rbCtrl_ & 2)) { rasterIrq_ = true; redoIrq(); }
}

void King::drawBgLine(int raster, uint32_t* line) {
  std::fill(line, line + 272, 0u);
  if (!(mprogCtrl_ & 1)) return;  // sem o microprograma, o KING não busca os planos
  for (int prio = 1; prio <= 7; prio++)
    for (int n = 0; n < 4; n++)
      if (((priority_ >> (n * 3)) & 7) == prio) drawBg(line, n, raster);
}

// Um plano do KING numa linha. O microprograma (16 passos, 8 por metade da KRAM) diz em que ciclos o chip
// busca o BAT (mapa de blocos) e os dados de padrão (CG) de cada plano; só o que é buscado aparece.
void King::drawBg(uint32_t* target, int n, int raster) {
  static const uint8_t ssTable[16] = {3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 3, 3, 3, 3, 3};
  static const bool ssInvalid[16] = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1};
  const uint32_t layer = uint32_t(L_BG0 + n) << 28;
  const uint32_t palOff = ((vce_.palOffset[1 + (n >> 1)] >> ((n & 1) ? 8 : 0)) << 1) & 0x1FF;
  auto pal = [&](uint32_t i) { return vce_.yuv[(palOff + i) & 0x1FF]; };
  const uint16_t* kp = kram[(page_ & 0x10) ? 1 : 0];
  const uint16_t mode = (bgMode_ >> (n * 4)) & 0xF;
  const int m = mode & 7;
  if (!m || m >= 6) return;
  const bool endless = (scrollMode_ >> n) & 1;
  const uint32_t xs = uint32_t(signX(n ? 10 : 11, xScroll_[n]));
  const uint32_t ys = uint32_t(signX(n ? 10 : 11, yScroll_[n]));
  const uint32_t yoff = (ys + uint32_t(raster - 22)) & 0xFFFF;

  const uint32_t batOff = uint32_t(batAddr_[n]) * 1024, batSubOff = n ? batOff : uint32_t(subBat_) * 1024;
  const uint32_t cgOff = uint32_t(cgAddr_[n]) * 1024, cgSubOff = n ? cgOff : uint32_t(subCg_) * 1024;
  // cada base escolhe a metade da KRAM (bit 17); o endereço anda dentro dela
  auto rd = [&](uint32_t base, uint32_t addr) { return kp[(base & 0x20000) | (addr & 0x1FFFF)]; };

  const int batMask = 0x7FF >> 3;
  const uint32_t wShift = ssTable[(bgSize_[n] & 0xF0) >> 4];
  const bool wInvalid = ssInvalid[(bgSize_[n] & 0xF0) >> 4];
  const uint32_t bw = (1u << wShift) >> 3;
  const int32_t hShift = ssTable[bgSize_[n] & 0x0F];
  const int32_t bh = (1 << hShift) >> 3;
  const bool swInvalid = n ? wInvalid : ssInvalid[(bgSize_[n] & 0xF000) >> 12];
  const uint32_t swShift = n ? wShift : ssTable[(bgSize_[n] & 0xF000) >> 12];
  const uint32_t sw = (1u << swShift) >> 3, swMask = sw - 1;
  const uint32_t swTest = endless ? uint32_t(batMask + 1) : std::max(bw, sw);
  const int32_t shShift = n ? hShift : ssTable[(bgSize_[n] & 0x0F00) >> 8];
  const int32_t sh = (1 << shShift) >> 3, shMask = sh - 1;
  const int32_t shTest = endless ? batMask + 1 : std::max(bh, sh);

  uint16_t cgMask[8] = {}, cgRemap[8] = {}, cgSubMask[8] = {}, cgSubRemap[8] = {};
  bool cgOfBat[8] = {}, cgSubOfBat[8] = {};
  bool batFetch = false, batSubFetch = false;
  const bool rotate = n == 0 && (priority_ & 0x1000);
  {
    int k = 0, ks = 0;
    for (int x = 0; x < 8; x++) {
      uint16_t mp = mprog_[((cgOff & 0x20000) ? 8 : 0) + x];
      if (((mp >> 6) & 3) == n && !(mp & 0x100) && !(mp & 0x010)) {
        cgMask[k] = ((mp & 0x20) != 0) == rotate ? 0xFFFF : 0;
        cgRemap[k] = mp & 7;
        cgOfBat[k] = mp & 8;
        k++;
      }
      mp = mprog_[((cgSubOff & 0x20000) ? 8 : 0) + x];
      if (((mp >> 6) & 3) == n && !(mp & 0x100) && !(mp & 0x010)) {
        cgSubMask[ks] = ((mp & 0x20) != 0) == rotate ? 0xFFFF : 0;
        cgSubRemap[ks] = mp & 7;
        cgSubOfBat[ks] = mp & 8;
        ks++;
      }
    }
    for (int x = 0; x < 8; x++) {
      uint16_t mp = mprog_[((batOff & 0x20000) ? 8 : 0) + x];
      if (((mp >> 6) & 3) == n && !(mp & 0x100) && (mp & 0x010) && ((mp & 0x020) != 0) == rotate) batFetch = true;
      mp = mprog_[((batSubOff & 0x20000) ? 8 : 0) + x];
      if (((mp >> 6) & 3) == n && !(mp & 0x100) && (mp & 0x010) && ((mp & 0x020) != 0) == rotate) batSubFetch = true;
    }
  }

  const int32_t wmul = 1 << wShift, wmask = ((1 << hShift) - 1) & (wInvalid ? 0 : -1);
  const int32_t wmulSub = 1 << swShift, wmaskSub = ((1 << shShift) - 1) & (swInvalid ? 0 : -1);

  if (rotate && m <= 4) {
    // rotação e escala (só o plano 0, modos de 4 a 64 K cores): coordenadas em ponto fixo de 8 bits
    const int32_t bwMask = endless ? int32_t(bw) - 1 : 0xFFFF, bhMask = endless ? bh - 1 : 0xFFFF;
    const int32_t a = int16_t(affA_), b = int16_t(affB_), c = int16_t(affC_), d = int16_t(affD_);
    const int32_t rx = int32_t(int16_t(signX(11, xs))) - int16_t(affX_);
    const int32_t ry = raster + int32_t(int16_t(signX(11, ys))) - 22 - int16_t(affY_);
    int32_t xacc = rx * a + ry * b + (int32_t(int16_t(affX_)) << 8);
    int32_t yacc = ry * d + rx * c + (int32_t(int16_t(affY_)) << 8);
    uint32_t* t = target + 8;
    for (int x = 0; x < 256; x++, xacc += a, yacc += c) {
      const uint16_t nx = uint16_t(xacc >> 8), ny = uint16_t(yacc >> 8);
      const uint32_t bx = (nx >> 3) & uint32_t(bwMask);
      const int32_t by = (ny >> 3) & bhMask;
      const int ysm = ny & 7;
      if (!(endless || (bx < bw && by < bh))) continue;
      uint16_t bat = 0;
      if (mode & 8) bat = rd(batOff, batOff + bx + ((uint32_t(by) << wShift) >> 3));
      uint32_t px = 0;
      switch (m) {
        case 1: {
          const uint16_t cg = (mode & 8) ? rd(cgOff, cgOff + (bat & 0xFFF) * 8 + ysm) : rd(cgOff, cgOff + bx + uint32_t((ny & wmask) * wmul / 8));
          const uint32_t v = (cg >> ((7 - (nx & 7)) << 1)) & 3;
          if (v) px = pal(((mode & 8) ? ((bat >> 12) << 2) : 0) + v) | layer;
          break;
        }
        case 2: {
          const uint32_t base = (mode & 8) ? cgOff + (bat & 0xFFF) * 16 + ysm * 2 : cgOff + bx * 2 + uint32_t((ny & wmask) * wmul / 4);
          const uint16_t cg = rd(cgOff, base + ((nx >> 2) & 1));
          const uint32_t v = (cg >> ((3 - (nx & 3)) << 2)) & 15;
          if (v) px = pal(((mode & 8) ? ((bat >> 12) << 4) : 0) + v) | layer;
          break;
        }
        case 3: {
          const uint32_t base = (mode & 8) ? cgOff + uint32_t(bat) * 32 + ysm * 4 : cgOff + bx * 4 + uint32_t((ny & wmask) * wmul / 2);
          const uint16_t cg = rd(cgOff, base + ((nx >> 1) & 3));
          const uint32_t v = (cg >> (((nx & 1) ^ 1) << 3)) & 0xFF;
          if (v) px = pal(v) | layer;
          break;
        }
        default: {
          const uint32_t base = (mode & 8) ? cgOff + uint32_t(bat) * 64 + ysm * 8 : cgOff + bx * 8 + uint32_t((ny & wmask) * wmul);
          const uint16_t cg = rd(cgOff, base + (nx & 7));
          if (cg >> 8) px = uint32_t(cg & 0xF0) << 8 | uint32_t(cg & 0x0F) << 4 | uint32_t(cg & 0xFF00) << 8 | layer;
          break;
        }
      }
      if (px) t[x] = px;
    }
    return;
  }

  // sem rotação: blocos de 8 pontos; cgN palavras de padrão por bloco conforme o modo
  static const int cgNeed[6] = {0, 1, 2, 4, 8, 8};
  static const int yShift[6] = {0, 3, 2, 1, 0, 0};
  const int cgN = cgNeed[m];
  const int32_t yPos = ((int32_t(yoff) & wmask) * wmul) >> yShift[m];
  const int32_t yPosSub = ((int32_t(yoff) & wmaskSub) * wmulSub) >> yShift[m];
  const uint32_t by = (yoff >> 3) & uint32_t(batMask);
  uint32_t bx = (xs >> 3) & uint32_t(batMask);
  const int ysm = yoff & 7;
  uint32_t* t = target + 8 - (xs & 7);
  for (int x = 0; x < 256 + 8; x += 8, bx = (bx + 1) & uint32_t(batMask)) {
    bool sub;
    if (bx < bw && by < uint32_t(bh)) sub = false;
    else if (bx < swTest && int32_t(by) < shTest) sub = true;
    else continue;
    const uint32_t bOff = sub ? batSubOff : batOff, cOff = sub ? cgSubOff : cgOff;
    uint32_t eff = bOff;
    if (mode & 8) {
      if (!sub) eff += bx + (((by & (wInvalid ? 0u : ~0u)) << wShift) >> 3);
      else eff += (bx & swMask) + (((by & (swInvalid ? 0u : ~0u) & uint32_t(shMask)) << swShift) >> 3);
    }
    uint16_t bat = (sub ? batSubFetch : batFetch) ? rd(bOff, eff) : 0;
    const uint32_t pbn = (mode & 8) ? bat >> 12 : 0;
    bat &= 0xFFF;
    const uint32_t ptr0 = cOff + (sub ? (bx & swMask) : bx) * uint32_t(cgN) + uint32_t(sub ? yPosSub : yPos);
    const uint32_t ptr1 = cOff + uint32_t(bat) * 8 * uint32_t(cgN) + uint32_t(ysm * cgN);
    const uint16_t* msk = sub ? cgSubMask : cgMask;
    const uint16_t* rmp = sub ? cgSubRemap : cgRemap;
    const bool* ofb = sub ? cgSubOfBat : cgOfBat;
    uint16_t cg[8];
    for (int k = 0; k < cgN; k++) cg[k] = uint16_t(rd(cOff, (ofb[k] ? ptr1 : ptr0) + rmp[k]) & msk[k]);
    uint32_t* o = t + x;
    switch (m) {
      case 1:
        for (int i = 0; i < 8; i++) { const uint32_t v = (cg[0] >> (14 - i * 2)) & 3; if (v) o[i] = pal((pbn << 2) + v) | layer; }
        break;
      case 2:
        for (int i = 0; i < 8; i++) { const uint32_t v = (cg[i >> 2] >> (12 - (i & 3) * 4)) & 15; if (v) o[i] = pal((pbn << 4) + v) | layer; }
        break;
      case 3:
        for (int i = 0; i < 8; i++) { const uint32_t v = (cg[i >> 1] >> ((i & 1) ? 0 : 8)) & 0xFF; if (v) o[i] = pal(v) | layer; }
        break;
      case 4:
        for (int i = 0; i < 8; i++)
          if (cg[i] & 0xFF00) o[i] = uint32_t(cg[i] & 0xF0) << 8 | uint32_t(cg[i] & 0x0F) << 4 | uint32_t(cg[i] & 0xFF00) << 8 | layer;
        break;
      default:  // 16 M cores: Y de cada ponto e U/V em comum para o par
        for (int i = 0; i < 8; i += 2) {
          const uint16_t yy = cg[i], uv = cg[i + 1];
          if (yy >> 8) o[i] = uint32_t(yy & 0xFF00) << 8 | uv | layer;
          if (yy & 0xFF) o[i + 1] = uint32_t(yy & 0xFF) << 16 | uv | layer;
        }
        break;
    }
  }
}

void King::sync(StateIO& io) {
  io.raw(kram, sizeof kram);
  io.sync(irq_); io.sync(last_); io.sync(cdNext_); io.sync(dmaNext_);
  io.sync(ar_); io.sync(kramRA_); io.sync(kramWA_); io.sync(kramMode_); io.sync(page_);
  io.sync(bgMode_); io.sync(priority_); io.sync(scrollMode_);
  io.raw(bgSize_, sizeof bgSize_); io.raw(batAddr_, sizeof batAddr_); io.raw(cgAddr_, sizeof cgAddr_);
  io.sync(subBat_); io.sync(subCg_);
  io.raw(xScroll_, sizeof xScroll_); io.raw(yScroll_, sizeof yScroll_);
  io.sync(affA_); io.sync(affB_); io.sync(affC_); io.sync(affD_); io.sync(affX_); io.sync(affY_);
  io.sync(mprogCtrl_); io.sync(mprogAddr_); io.raw(mprog_, sizeof mprog_);
  io.sync(adpcmControl_); io.raw(adpcmBufMode_, sizeof adpcmBufMode_); io.raw(adpcmSal_, sizeof adpcmSal_);
  io.raw(adpcmMid_, sizeof adpcmMid_); io.raw(adpcmStatus_, sizeof adpcmStatus_);
  io.raw(adpcmEnd_, sizeof adpcmEnd_); io.raw(adpcmPlay_, sizeof adpcmPlay_); io.sync(adpcmIrq_);
  io.sync(rbCtrl_); io.sync(rbStart_); io.sync(rbBlocks_); io.sync(rasterLine_); io.sync(rbKram_); io.sync(rasterIrq_);
  io.sync(reg01_); io.sync(reg02_); io.sync(reg03_);
  io.sync(subCtrl_); io.sync(subBuf_); io.sync(dataCache_); io.sync(dmaLatch_);
  io.sync(cdIrq_); io.sync(subIrq_); io.sync(dmaIrq_);
  io.sync(drq_); io.sync(dmaRecv_); io.sync(dmaSend_); io.sync(dmaFlip_);
  io.sync(dmaAddr_); io.sync(dmaSize_); io.sync(dmaStatus_);
  cd_.sync(io);
  if (io.reading()) { mprogAddr_ &= 0xF; ar_ &= 0x7F; }
}

}  // namespace indigo::pcfx
