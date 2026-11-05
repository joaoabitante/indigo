#include "neogeocd.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::neogeocd {

namespace {
const char STATE_MAGIC[4] = {'I', 'N', 'C', '1'};
constexpr int64_t CYC_PER_FRAME = int64_t(neogeo::LSPC::CYC_PER_LINE) * neogeo::LSPC::LINES;
constexpr uint32_t WATCHDOG_CYCLES = 1633375;  // 0,135 s (como medido no NeoCD)
constexpr uint32_t RAM_SIZE = 0x200000, BIOS_SIZE = 0x80000, SPR_SIZE = 0x400000, FIX_SIZE = 0x20000,
                   PCM_SIZE = 0x100000, ZRAM_SIZE = 0x10000, BACKUP_SIZE = 0x2000;
constexpr uint32_t DMA_MAX_WORDS = 0x400000;  // limite prático de uma operação de DMA (o registrador tem 32 bits)

uint32_t be32(const std::vector<uint8_t>& b, size_t o, bool swap) {
  auto byte = [&](size_t i) { return static_cast<uint32_t>(b[swap ? (i ^ 1) : i]); };
  return (byte(o) << 24) | (byte(o + 1) << 16) | (byte(o + 2) << 8) | byte(o + 3);
}
bool inBios(uint32_t a) { return a >= 0xC00000 && a < 0xC80000; }

// remendos do NeoCD (bios.cpp, LGPL-3: só os endereços e os bytes): o "reconhecimento do CD" das BIOS
// originais é uma checagem de proteção do disco que as imagens não têm; nas BIOS modificadas que conferem a
// própria soma, a soma também é desligada
struct Patch { uint32_t addr; uint8_t from[6], to[6]; int n; };
const Patch CDZ_RECOG[] = {{0xC0EB82, {0x66, 0x10}, {0x4E, 0x71}, 2}, {0xC0D280, {0x66, 0x74}, {0x4E, 0x71}, 2}};
const Patch FRONT_RECOG[] = {{0xC10B64, {0x66, 0x04}, {0x4E, 0x71}, 2}};
const Patch TOP_RECOG[] = {{0xC10436, {0x66, 0x04}, {0x4E, 0x71}, 2}};
const Patch UNI33_SUM[] = {{0xC1D3EC, {0x67, 0x32}, {0x60, 0x32}, 2}};
const Patch CDZ_SMK_SUM[] = {{0xC62C08, {0x22, 0x39, 0x00, 0xC6, 0xFF, 0xF4}, {0x22, 0x00, 0x4E, 0x71, 0x4E, 0x71}, 6}};
const Patch CDZ_SMKB_SUM[] = {{0xC62BF4, {0x22, 0x39, 0x00, 0xC6, 0xFF, 0xF4}, {0x22, 0x00, 0x4E, 0x71, 0x4E, 0x71}, 6}};
const Patch FRONT_SMKB_SUM[] = {{0xC23EBE, {0x22, 0x39, 0x00, 0xC2, 0xFF, 0xF4}, {0x22, 0x00, 0x4E, 0x71, 0x4E, 0x71}, 6}};
const Patch TOP_SMK_SUM[] = {{0xC2400A, {0x22, 0x39, 0x00, 0xC2, 0xFF, 0xF4}, {0x22, 0x00, 0x4E, 0x71, 0x4E, 0x71}, 6}};
const Patch TOP_SMKB_SUM[] = {{0xC23FBE, {0x22, 0x39, 0x00, 0xC2, 0xFF, 0xF4}, {0x22, 0x00, 0x4E, 0x71, 0x4E, 0x71}, 6}};

template <size_t N>
bool applyPatches(std::vector<uint8_t>& b, const Patch (&p)[N]) {
  for (const Patch& x : p)
    if (std::memcmp(&b[x.addr - 0xC00000], x.from, static_cast<size_t>(x.n)) != 0) return false;
  for (const Patch& x : p) std::memcpy(&b[x.addr - 0xC00000], x.to, static_cast<size_t>(x.n));
  return true;
}
bool hasBytes(const std::vector<uint8_t>& b, uint32_t addr, std::initializer_list<uint8_t> v) {
  size_t i = addr - 0xC00000;
  for (uint8_t x : v)
    if (b[i++] != x) return false;
  return true;
}

uint16_t bitReverse16(uint16_t v) {
  uint16_t r = 0;
  for (int i = 0; i < 16; i++) if (v & (1u << i)) r = static_cast<uint16_t>(r | (0x8000u >> i));
  return r;
}

cdrom::Disc& discRef(const std::unique_ptr<cdrom::Disc>& d) {
  if (!d) throw std::runtime_error("Imagem de CD do Neo Geo CD não aberta.");
  return *d;
}
}  // namespace

bool isNeoGeoCdBios(const std::vector<uint8_t>& b, int* family) {
  if (b.size() != BIOS_SIZE) return false;
  const bool swap = b[0] == 0x10 && b[1] == 0x00;  // arquivo com as palavras em ordem invertida
  if (be32(b, 0, swap) != 0x0010F300) return false;
  // reset, interrupções de nível 1-3 e as duas do CD apontam para dentro da BIOS
  for (size_t v : {4, 0x54, 0x58, 0x64, 0x68, 0x6C})
    if (!inBios(be32(b, v, swap))) return false;
  if (family) {
    const uint32_t l3 = be32(b, 0x6C, swap);
    *family = l3 == 0xC0C85E ? 1 : l3 == 0xC0C222 ? 2 : l3 == 0xC0A3E8 ? 3 : 0;
  }
  return true;
}

// ------------------------------------------------------------------ construção
NeoGeoCd::~NeoGeoCd() = default;

NeoGeoCd::NeoGeoCd(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save)
    : cpu(*this), z80(*this), disc_(std::move(disc)), cd_(discRef(disc_)), ram_(RAM_SIZE), spr_(SPR_SIZE),
      fix_(FIX_SIZE), pcm_(PCM_SIZE), zram_(ZRAM_SIZE), backup_(BACKUP_SIZE) {
  if (cdrom::detectSystem(*disc_) != "neogeocd")
    throw std::runtime_error("Este CD não é do Neo Geo CD (falta o IPL.TXT).");
  loadBios();
  title = disc_->name();
  l0_ = neogeo::makeL0();
  lspc.setCrom(spr_.data(), spr_.size());
  lspc.setFix(fix_.data(), fix_.size(), neogeo::LSPC::FIX_NONE);
  lspc.setL0(l0_.data());
  lspc.setCdSpriteLayout(true);
  ym.setRom(pcm_.data(), pcm_.size(), pcm_.data(), pcm_.size());
  cdL_.assign(neogeo::YM2610::BUF, 0.0f);
  cdR_.assign(neogeo::YM2610::BUF, 0.0f);
  if (save) importSave(*save);
  dirty_ = false;
  setSampleRate(48000);
  reset();
}

void NeoGeoCd::loadBios() {
  const std::vector<uint8_t> fw = firmware("neogeocd");
  if (fw.empty()) throw std::runtime_error("Abra antes a BIOS do Neo Geo CD (arquivo de 512 KB) como um jogo.");
  int fam = 0;
  if (!isNeoGeoCdBios(fw, &fam)) throw std::runtime_error("A BIOS do Neo Geo CD guardada não foi reconhecida.");
  bios_ = fw;
  if (bios_[0] == 0x10 && bios_[1] == 0x00)
    for (size_t i = 0; i + 1 < bios_.size(); i += 2) std::swap(bios_[i], bios_[i + 1]);
  biosFamily_ = fam;
  static const char* NAMES[4] = {"Neo Geo CD (desconhecida)", "Neo Geo CD front-loader", "Neo Geo CD top-loader",
                                 "Neo Geo CDZ"};
  biosName = NAMES[fam];
  // modificações conhecidas (pelo endereço do reset e por uma assinatura, como no NeoCD)
  const bool smkB = (fam == 1 && hasBytes(bios_, 0xC00004, {0x00, 0xC2, 0x33, 0x00})) ||
                    (fam == 2 && hasBytes(bios_, 0xC00004, {0x00, 0xC2, 0x34, 0x00})) ||
                    (fam == 3 && hasBytes(bios_, 0xC00004, {0x00, 0xC6, 0x20, 0x00}));
  const bool smk = (fam == 2 && hasBytes(bios_, 0xC00004, {0x00, 0xC2, 0x34, 0x14})) ||
                   (fam == 3 && hasBytes(bios_, 0xC00004, {0x00, 0xC6, 0x20, 0x14}));
  const bool uni32 = fam == 3 && hasBytes(bios_, 0xC00150, {0x1C, 0xCA, 0x85, 0x8A});
  const bool uni33 = fam == 3 && hasBytes(bios_, 0xC00150, {0xA4, 0x4B, 0x15, 0x2F});
  if (smk) biosName += " (SMKDan 0.07)";
  if (smkB) biosName += " (SMKDan 0.07b)";
  if (uni32) biosName += " (Universe BIOS CD 3.2)";
  if (uni33) biosName += " (Universe BIOS CD 3.3)";
  if (fam == 3) {
    applyPatches(bios_, CDZ_RECOG);
    if (smk) applyPatches(bios_, CDZ_SMK_SUM);
    if (smkB) applyPatches(bios_, CDZ_SMKB_SUM);
    if (uni33) applyPatches(bios_, UNI33_SUM);
  } else if (fam == 1) {
    applyPatches(bios_, FRONT_RECOG);
    if (smkB) applyPatches(bios_, FRONT_SMKB_SUM);
  } else if (fam == 2) {
    applyPatches(bios_, TOP_RECOG);
    if (smk) applyPatches(bios_, TOP_SMK_SUM);
    if (smkB) applyPatches(bios_, TOP_SMKB_SUM);
  }
}

void NeoGeoCd::reset() {
  // ligar o aparelho: as memórias voláteis zeram (a RAM de backup fica); os relógios continuam
  std::fill(ram_.begin(), ram_.end(), 0);
  std::fill(spr_.begin(), spr_.end(), 0);
  std::fill(fix_.begin(), fix_.end(), 0);
  std::fill(pcm_.begin(), pcm_.end(), 0);
  std::fill(zram_.begin(), zram_.end(), 0);
  std::fill(std::begin(lspc.vram), std::end(lspc.vram), 0);
  lspc.vramChanged();
  for (int bank = 0; bank < 2; bank++) {  // paleta zerada (e convertida) nos dois bancos
    lspc.setPalBank(bank);
    for (uint32_t i = 0; i < 0x1000; i++) lspc.palWrite(i << 1, 0);
  }
  lspc.reset();
  lspc.sprOn = false; lspc.fixOn = false; lspc.videoOn = false;
  vecRom_ = true;
  area_ = busReq_ = sprBank_ = pcmBank_ = selector_ = 0;
  irqMask1_ = irqMask2_ = 0;
  irqPending_ = 0;
  commOn_ = false;
  syncZ80(now());
  z80On_ = false;
  wdOn_ = false; wdCounter_ = 0;
  dmaReset();
  soundCode_ = soundReply_ = 0;
  nmiEnabled_ = false;
  timerCtrl_ = 0; timerReload_ = timerCounter_ = timerFrag_ = 0;
  cd_.reset();
  cdCounter_ = cd_.delay(biosFamily_ == 3);
  z80.reset();
  ym.run(now());
  ym.reset();
  cpu.reset();
  frame = 0;
}

void NeoGeoCd::setSampleRate(int rate) {
  outRate_ = rate > 0 ? rate : 48000;
  ym.setRates(masterClock() / 3.0 / 144.0, outRate_);
}

void NeoGeoCd::importSave(const std::vector<uint8_t>& b) {
  if (b.size() < BACKUP_SIZE) return;
  std::copy(b.begin(), b.begin() + BACKUP_SIZE, backup_.begin());
}

void NeoGeoCd::pullAudio(float* l, float* r, size_t n) {
  ym.pull(l, r, n);
  const size_t B = cdL_.size();
  for (size_t i = 0; i < n && cdRd_ != cdW_; i++) {
    l[i] = std::clamp(l[i] + cdL_[cdRd_], -1.0f, 1.0f);
    r[i] = std::clamp(r[i] + cdR_[cdRd_], -1.0f, 1.0f);
    cdRd_ = (cdRd_ + 1) % B;
  }
  // a fila do CD nunca fica maior que a do YM2610 (as duas andam juntas)
  while ((cdW_ + B - cdRd_) % B > ym.available()) cdRd_ = (cdRd_ + 1) % B;
}

// ------------------------------------------------------------------ 68000: entradas, vídeo, janela de upload
uint8_t NeoGeoCd::inputRead(uint32_t a) {
  const bool normal = selector_ == 0x00 || selector_ == 0x12 || selector_ == 0x1B;
  const uint32_t k = keys_;
  switch (a & 0xFE0000) {
    case 0x300000: case 0x340000: {  // jogador 1 / jogador 2
      if (!normal) return 0xFF;
      const uint32_t p = (a & 0xFE0000) == 0x340000 ? keys2_ : keys_;
      uint8_t v = 0xFF;
      if (p & KEY_UP) v &= ~0x01;
      if (p & KEY_DOWN) v &= ~0x02;
      if (p & KEY_LEFT) v &= ~0x04;
      if (p & KEY_RIGHT) v &= ~0x08;
      if (p & KEY_B) v &= ~0x10;  // A
      if (p & KEY_A) v &= ~0x20;  // B
      if (p & KEY_Y) v &= ~0x40;  // C
      if (p & KEY_X) v &= ~0x80;  // D
      return v;
    }
    default: {  // 0x380000: start/select dos dois jogadores (bits 0-3, 1 = solto)
      if (!normal) return 0x0F;
      uint8_t v = 0x0F;
      if (k & KEY_START) v &= ~0x01;
      if (k & KEY_SELECT) v &= ~0x02;
      if (keys2_ & KEY_START) v &= ~0x04;
      if (keys2_ & KEY_SELECT) v &= ~0x08;
      return v;
    }
  }
}

uint16_t NeoGeoCd::lspcRead(uint32_t a) {
  switch (a & 0xE) {
    case 0x0: case 0x2: case 0x8: case 0xA: return lspc.readData();
    case 0x4: case 0xC: return lspc.readMod();
    default: return lspc.readMode();
  }
}

void NeoGeoCd::lspcWrite(uint32_t a, uint16_t v) {
  switch (a & 0xE) {
    case 0x0: lspc.writeAddr(v); break;
    case 0x2: lspc.writeData(v); break;
    case 0x4: lspc.writeMod(v); break;
    case 0x6: {
      lspc.writeMode(v);
      const uint8_t prev = timerCtrl_;
      timerCtrl_ = v & 0xF0;
      if (!(prev & 0x40) && (timerCtrl_ & 0x40) && lspc.scanline >= 248) timerCounter_ = timerReload_;
      break;
    }
    case 0x8: timerReload_ = (timerReload_ & 0xFFFF) | (static_cast<uint32_t>(v) << 16); break;
    case 0xA:
      timerReload_ = (timerReload_ & 0xFFFF0000) | v;
      if (timerCtrl_ & 0x20) timerCounter_ = timerReload_;
      break;
    case 0xC:  // no CD: 4 = quadro, 2 = linha
      if (v & 4) irqPending_ &= ~IRQ_VBL;
      if (v & 2) irqPending_ &= ~IRQ_RASTER;
      break;
    default: break;
  }
}

uint8_t NeoGeoCd::windowRead8(uint32_t a) {
  if (!(area_ & busReq_)) return 0xFF;
  switch (area_) {
    case AREA_SPR: return spr_[((a & 0xFFFFF) + (sprBank_ & 3u) * 0x100000) & (SPR_SIZE - 1)];
    case AREA_FIX: return (a & 1) ? fix_[(a >> 1) & (FIX_SIZE - 1)] : 0xFF;
    case AREA_Z80: return (a & 1) ? zram_[(a >> 1) & (ZRAM_SIZE - 1)] : 0xFF;
    case AREA_PCM: return (a & 1) ? pcm_[(((a & 0xFFFFF) >> 1) + (pcmBank_ & 1u) * 0x80000) & (PCM_SIZE - 1)] : 0xFF;
    default: return 0xFF;
  }
}

uint16_t NeoGeoCd::windowRead16(uint32_t a) {
  if (!(area_ & busReq_)) return 0xFFFF;
  switch (area_) {
    case AREA_SPR: {
      const uint32_t o = ((a & 0xFFFFE) + (sprBank_ & 3u) * 0x100000) & (SPR_SIZE - 2);
      return static_cast<uint16_t>((spr_[o] << 8) | spr_[o + 1]);
    }
    case AREA_FIX: return static_cast<uint16_t>(0xFF00 | fix_[(a >> 1) & (FIX_SIZE - 1)]);
    case AREA_Z80: return static_cast<uint16_t>(0xFF00 | zram_[(a >> 1) & (ZRAM_SIZE - 1)]);
    case AREA_PCM: return static_cast<uint16_t>(0xFF00 | pcm_[(((a & 0xFFFFF) >> 1) + (pcmBank_ & 1u) * 0x80000) & (PCM_SIZE - 1)]);
    default: return 0xFFFF;
  }
}

void NeoGeoCd::windowWrite8(uint32_t a, uint8_t v) {
  if (!(area_ & busReq_)) return;
  switch (area_) {
    case AREA_SPR: spr_[((a & 0xFFFFF) + (sprBank_ & 3u) * 0x100000) & (SPR_SIZE - 1)] = v; break;
    case AREA_FIX: if (a & 1) fix_[(a >> 1) & (FIX_SIZE - 1)] = v; break;
    case AREA_Z80: if (a & 1) { syncZ80(now()); zram_[(a >> 1) & (ZRAM_SIZE - 1)] = v; } break;
    case AREA_PCM:
      if (a & 1) { ym.run(now()); pcm_[(((a & 0xFFFFF) >> 1) + (pcmBank_ & 1u) * 0x80000) & (PCM_SIZE - 1)] = v; }
      break;
    default: break;
  }
}

void NeoGeoCd::windowWrite16(uint32_t a, uint16_t v) {
  if (!(area_ & busReq_)) return;
  switch (area_) {
    case AREA_SPR: {
      const uint32_t o = ((a & 0xFFFFE) + (sprBank_ & 3u) * 0x100000) & (SPR_SIZE - 2);
      spr_[o] = static_cast<uint8_t>(v >> 8);
      spr_[o + 1] = static_cast<uint8_t>(v);
      break;
    }
    case AREA_FIX: fix_[(a >> 1) & (FIX_SIZE - 1)] = static_cast<uint8_t>(v); break;
    case AREA_Z80: syncZ80(now()); zram_[(a >> 1) & (ZRAM_SIZE - 1)] = static_cast<uint8_t>(v); break;
    case AREA_PCM:
      ym.run(now());
      pcm_[(((a & 0xFFFFF) >> 1) + (pcmBank_ & 1u) * 0x80000) & (PCM_SIZE - 1)] = static_cast<uint8_t>(v);
      break;
    default: break;
  }
}

// ------------------------------------------------------------------ 68000: registradores do CD ($FF0000)
uint8_t NeoGeoCd::cdRead8(uint32_t a) {
  switch (a & 0x1FF) {
    case 0x103: return cd_.readRegister();
    case 0x161: return cd_.readResponse();
    default: return 0x00;
  }
}

uint16_t NeoGeoCd::cdRead16(uint32_t a) {
  switch (a & 0x1FE) {
    case 0x004: return irqMask2_;
    case 0x11C: {  // configuração: país (USA) e, fora do CDZ, a tampa fechada
      const unsigned nationality = 1;
      return static_cast<uint16_t>(((~nationality & 7u) << 8) | (biosFamily_ == 3 ? 0 : 0x1000));
    }
    case 0x188: return cd_.playing() ? bitReverse16(static_cast<uint16_t>(cd_.audioSample(0))) : 0;
    case 0x18A: return cd_.playing() ? bitReverse16(static_cast<uint16_t>(cd_.audioSample(1))) : 0;
    default: return 0x0000;
  }
}

void NeoGeoCd::cdWrite8(uint32_t a, uint8_t v) {
  switch (a & 0x1FF) {
    case 0x00F:  // reconhecimento das interrupções do CD
      if (v & 0x20) irqPending_ &= ~IRQ_DECODER;
      if (v & 0x10) irqPending_ &= ~IRQ_COMM;
      break;
    case 0x061:  // DMA: $40 começa, $00 zera os registradores
      if (v == 0x40) dmaStart();
      else if (v == 0) dmaReset();
      break;
    case 0x101: cd_.setRegister(v); break;
    case 0x103: cd_.writeRegister(v); break;
    case 0x105:  // área da janela de upload
      area_ = v == 0 ? AREA_SPR : v == 1 ? AREA_PCM : v == 4 ? AREA_Z80 : v == 5 ? AREA_FIX : 0;
      break;
    case 0x111: lspc.sprOn = v == 0; break;
    case 0x115: lspc.fixOn = v == 0; break;
    case 0x119: lspc.videoOn = v != 0; break;
    case 0x121: busReq_ |= AREA_SPR; break;
    case 0x123: busReq_ |= AREA_PCM; break;
    case 0x127: busReq_ |= AREA_Z80; break;
    case 0x129: busReq_ |= AREA_FIX; break;
    case 0x141: busReq_ &= ~AREA_SPR; break;
    case 0x143: busReq_ &= ~AREA_PCM; break;
    case 0x147: busReq_ &= ~AREA_Z80; break;
    case 0x149: busReq_ &= ~AREA_FIX; break;
    case 0x163: cd_.writeCommand(v); break;
    case 0x165: cd_.pointerStep(v); break;
    case 0x16F: wdOn_ = v == 0; break;  // vigia: 0 liga, 1 desliga
    case 0x181:  // 0 desliga a comunicação com a unidade; os ponteiros do pacote voltam ao começo
      commOn_ = v != 0;
      cd_.resetPointers();
      break;
    case 0x183:  // Z80: 0 parado; outro valor solta (com reset do Z80 e do YM2610)
      syncZ80(now());
      if (!v) {
        z80On_ = false;
      } else {
        z80On_ = true;
        z80.reset();
        ym.run(now());
        ym.reset();
      }
      break;
    case 0x1A1: sprBank_ = v; break;
    case 0x1A3: pcmBank_ = v; break;
    default: break;
  }
}

void NeoGeoCd::cdWrite16(uint32_t a, uint16_t v) {
  switch (a & 0x1FE) {
    case 0x000: cd_.driveReset(); break;
    case 0x002: irqMask1_ = v; break;
    case 0x004: irqMask2_ = v; break;
    case 0x064: dmaDst_ = (dmaDst_ & 0xFFFF) | (static_cast<uint32_t>(v) << 16); break;
    case 0x066: dmaDst_ = (dmaDst_ & 0xFFFF0000) | v; break;
    case 0x068: dmaSrc_ = (dmaSrc_ & 0xFFFF) | (static_cast<uint32_t>(v) << 16); break;
    case 0x06A: dmaSrc_ = (dmaSrc_ & 0xFFFF0000) | v; break;
    case 0x06C: dmaPattern_ = v; break;
    case 0x070: dmaLen_ = (dmaLen_ & 0xFFFF) | (static_cast<uint32_t>(v) << 16); break;
    case 0x072: dmaLen_ = (dmaLen_ & 0xFFFF0000) | v; break;
    default:
      if ((a & 0x1FE) >= 0x7E && (a & 0x1FE) <= 0x8E) dmaCfg_[((a & 0x1FE) - 0x7E) >> 1] = v;
      break;
  }
}

void NeoGeoCd::soundCommand(uint8_t v) {
  syncZ80(now());
  soundCode_ = v;
  if (nmiEnabled_ && z80On_) z80.nmiPending = true;
}

void NeoGeoCd::switchWrite(uint32_t a) {
  switch (a & 0x1E) {
    case 0x00: lspc.setShadow(false); break;
    case 0x10: lspc.setShadow(true); break;
    case 0x02: vecRom_ = true; break;   // vetores da BIOS
    case 0x12: vecRom_ = false; break;  // vetores da RAM
    case 0x0E: lspc.setPalBank(0); break;  // como no NeoCD e no MAME
    case 0x1E: lspc.setPalBank(1); break;
    default: break;
  }
}

void NeoGeoCd::backupWrite(uint32_t a, uint8_t v) {
  uint8_t& b = backup_[(a & 0x3FFF) >> 1];
  if (b != v) { b = v; dirty_ = true; }
}

// ------------------------------------------------------------------ 68000: mapa de memória
uint16_t NeoGeoCd::read16(uint32_t a) {
  if (a < 0x200000) {
    if (a < 0x80 && vecRom_) return static_cast<uint16_t>((bios_[a] << 8) | bios_[a + 1]);
    return static_cast<uint16_t>((ram_[a] << 8) | ram_[a + 1]);
  }
  switch (a & 0xFE0000) {
    case 0x300000: case 0x340000: case 0x380000: return static_cast<uint16_t>((inputRead(a) << 8) | 0xFF);
    case 0x320000: syncZ80(now()); return static_cast<uint16_t>((soundReply_ << 8) | 0xFF);
    case 0x3C0000: return lspcRead(a);
    default: break;
  }
  if (a >= 0x400000 && a < 0x500000) return lspc.palRead(a);
  if (a >= 0x800000 && a < 0x900000) return static_cast<uint16_t>(0xFF00 | backup_[(a & 0x3FFF) >> 1]);
  if (a >= 0xC00000 && a < 0xD00000) { const uint32_t o = a & (BIOS_SIZE - 1); return static_cast<uint16_t>((bios_[o] << 8) | bios_[o + 1]); }
  if (a >= 0xE00000 && a < 0xF00000) return windowRead16(a);
  if (a >= 0xFF0000 && a < 0xFF0200) return cdRead16(a);
  return 0xFFFF;
}

uint8_t NeoGeoCd::read8(uint32_t a) {
  if (a < 0x200000) {
    if (a < 0x80 && vecRom_) return bios_[a];
    return ram_[a];
  }
  switch (a & 0xFE0000) {
    case 0x300000: case 0x340000: case 0x380000: return (a & 1) ? 0xFF : inputRead(a);
    case 0x320000:
      if (a & 1) return 0xFF;
      syncZ80(now());
      return soundReply_;
    case 0x3C0000: { const uint16_t w = lspcRead(a & ~1u); return (a & 1) ? 0xFF : static_cast<uint8_t>(w >> 8); }
    default: break;
  }
  if (a >= 0x400000 && a < 0x500000) { const uint16_t w = lspc.palRead(a); return static_cast<uint8_t>((a & 1) ? w : w >> 8); }
  if (a >= 0x800000 && a < 0x900000) return (a & 1) ? backup_[(a & 0x3FFF) >> 1] : 0xFF;
  if (a >= 0xC00000 && a < 0xD00000) return bios_[a & (BIOS_SIZE - 1)];
  if (a >= 0xE00000 && a < 0xF00000) return windowRead8(a);
  if (a >= 0xFF0000 && a < 0xFF0200) return cdRead8(a);
  return 0xFF;
}

void NeoGeoCd::write16(uint32_t a, uint16_t v) {
  if (a < 0x200000) {
    if (a < 0x80 && vecRom_) return;  // vetores da BIOS: a escrita se perde (como no NeoCD)
    ram_[a] = static_cast<uint8_t>(v >> 8);
    ram_[a + 1] = static_cast<uint8_t>(v);
    return;
  }
  switch (a & 0xFE0000) {
    case 0x300000: wdCounter_ = 0; return;  // vigia
    case 0x320000: soundCommand(static_cast<uint8_t>(v >> 8)); return;
    case 0x380000: selector_ = static_cast<uint8_t>(v); return;
    case 0x3A0000: switchWrite(a); return;
    case 0x3C0000: lspcWrite(a, v); return;
    default: break;
  }
  if (a >= 0x400000 && a < 0x500000) { lspc.palWrite(a, v); return; }
  if (a >= 0x800000 && a < 0x900000) { backupWrite(a, static_cast<uint8_t>(v)); return; }
  if (a >= 0xE00000 && a < 0xF00000) { windowWrite16(a, v); return; }
  if (a >= 0xFF0000 && a < 0xFF0200) cdWrite16(a, v);
}

void NeoGeoCd::write8(uint32_t a, uint8_t v) {
  if (a < 0x200000) {
    if (a < 0x80 && vecRom_) return;
    ram_[a] = v;
    return;
  }
  switch (a & 0xFE0000) {
    case 0x300000: if (a & 1) wdCounter_ = 0; return;
    case 0x320000: if (!(a & 1)) soundCommand(v); return;
    case 0x380000: if (a & 1) selector_ = v; return;
    case 0x3A0000: if (a & 1) switchWrite(a); return;
    case 0x3C0000: if (!(a & 1)) lspcWrite(a, static_cast<uint16_t>((v << 8) | v)); return;
    default: break;
  }
  if (a >= 0x400000 && a < 0x500000) {
    uint16_t w = lspc.palRead(a);
    w = (a & 1) ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | (v << 8));
    lspc.palWrite(a, w);
    return;
  }
  if (a >= 0x800000 && a < 0x900000) { if (a & 1) backupWrite(a, v); return; }
  if (a >= 0xE00000 && a < 0xF00000) { windowWrite8(a, v); return; }
  if (a >= 0xFF0000 && a < 0xFF0200) cdWrite8(a, v);
}

int NeoGeoCd::intAck(int level) {
  switch (level) {
    case 1: return 0x68 / 4;                                       // quadro
    case 2: return (irqPending_ & IRQ_COMM) ? 0x58 / 4 : 0x54 / 4;  // CD: comunicação ou decodificador
    case 3: return 0x64 / 4;                                       // linha
    default: return 24 + level;
  }
}

// ------------------------------------------------------------------ DMA
// O DMA do CD é programado por uma "receita" de 9 palavras ($FF007E..$FF008E); como no NeoCD, a operação é
// reconhecida pela primeira palavra e feita de uma vez.
void NeoGeoCd::dmaReset() {
  std::fill(std::begin(dmaCfg_), std::end(dmaCfg_), 0);
  dmaSrc_ = dmaDst_ = dmaLen_ = 0;
  dmaPattern_ = 0;
}

bool NeoGeoCd::dmaRegion(uint32_t a, uint32_t& base, uint32_t& mask) const {
  struct R { uint32_t start, end, mask; };
  static const R REG[] = {
      {0x000000, 0x1FFFFF, 0x1FFFFF}, {0x300000, 0x31FFFF, 1},      {0x320000, 0x33FFFF, 1},
      {0x340000, 0x35FFFF, 1},        {0x380000, 0x39FFFF, 1},      {0x3A0000, 0x3BFFFF, 0x1F},
      {0x3C0000, 0x3DFFFF, 0xF},      {0x400000, 0x4FFFFF, 0x1FFF}, {0x800000, 0x8FFFFF, 0x3FFF},
      {0xC00000, 0xCFFFFF, 0x7FFFF},  {0xE00000, 0xEFFFFF, 0xFFFFF}, {0xFF0000, 0xFF01FF, 0x1FF},
      {0x200000, 0x2FFFFF, 0},        {0x360000, 0x37FFFF, 0},      {0x3E0000, 0x3FFFFF, 0}};
  a &= 0xFFFFFF;
  for (const R& r : REG)
    if (a >= r.start && a <= r.end) { base = r.start; mask = r.mask; return true; }
  return false;
}

uint16_t NeoGeoCd::dmaRead(uint32_t base, uint32_t mask, uint32_t& off) {
  const uint32_t a = base + (off & mask & ~1u);
  off += 2;
  if (base == 0) return static_cast<uint16_t>((ram_[a] << 8) | ram_[a + 1]);  // RAM direto (sem os vetores)
  if (mask == 0) return 0xFFFF;
  return read16(a);
}

void NeoGeoCd::dmaWrite(uint32_t base, uint32_t mask, uint32_t& off, uint16_t v) {
  const uint32_t a = base + (off & mask & ~1u);
  off += 2;
  if (base == 0) { ram_[a] = static_cast<uint8_t>(v >> 8); ram_[a + 1] = static_cast<uint8_t>(v); return; }
  if (mask == 0 || base == 0xC00000) return;
  write16(a, v);
}

void NeoGeoCd::dmaFromCd(bool oddBytes) {
  uint32_t base, mask;
  if (!dmaRegion(dmaDst_, base, mask)) return;
  if (dmaLen_ > 0x400) {  // como no NeoCD (Art of Fighting com a BIOS do CDZ): corta e corrige a variável da BIOS
    write16(0x10FEFC, 0x0000);
    write16(0x10FEFE, 0x0800);
    dmaLen_ = 0x400;
  }
  const uint8_t* src = cd_.buffer();
  uint32_t off = dmaDst_ & mask;
  for (uint32_t i = 0; i < dmaLen_; i++) {
    const uint16_t w = static_cast<uint16_t>((src[2 * i] << 8) | src[2 * i + 1]);
    if (oddBytes) {
      dmaWrite(base, mask, off, static_cast<uint16_t>(w >> 8));
      dmaWrite(base, mask, off, w);
    } else {
      dmaWrite(base, mask, off, w);
    }
  }
  cd_.endTransfer();
}

void NeoGeoCd::dmaStart() {
  if (inDma_) return;
  inDma_ = true;
  const uint32_t len = std::min(dmaLen_, DMA_MAX_WORDS);
  uint32_t sb, sm, db, dm;
  switch (dmaCfg_[0]) {
    case 0xFE3D: case 0xFE6D:  // cópia (origem em $FF0064, destino em $FF0068)
    case 0xE2DD: case 0xF2DD: {  // cópia para bytes ímpares
      if (!dmaRegion(dmaDst_, sb, sm) || !dmaRegion(dmaSrc_, db, dm)) break;
      const bool odd = dmaCfg_[0] == 0xE2DD || dmaCfg_[0] == 0xF2DD;
      uint32_t so = dmaDst_ & sm, dof = dmaSrc_ & dm;
      for (uint32_t i = 0; i < len; i++) {
        const uint16_t w = dmaRead(sb, sm, so);
        if (odd) {
          dmaWrite(db, dm, dof, static_cast<uint16_t>((w >> 8) | (w << 8)));
          dmaWrite(db, dm, dof, w);
        } else {
          dmaWrite(db, dm, dof, w);
        }
      }
      break;
    }
    case 0xFFC5: case 0xFF89: dmaFromCd(false); break;  // buffer do CD
    case 0xFC2D: dmaFromCd(true); break;                // buffer do CD para bytes ímpares
    case 0xFEF5: {  // preenche com o próprio endereço
      if (!dmaRegion(dmaDst_, db, dm)) break;
      uint32_t addr = dmaDst_, off = dmaDst_ & dm;
      for (uint32_t i = 0; i < len; i++) {
        dmaWrite(db, dm, off, static_cast<uint16_t>(addr >> 16));
        dmaWrite(db, dm, off, static_cast<uint16_t>(addr));
        addr += 4;
      }
      break;
    }
    case 0xCFFD: {  // preenche bytes ímpares com o próprio endereço
      if (!dmaRegion(dmaDst_, db, dm)) break;
      uint32_t addr = dmaDst_, off = dmaDst_ & dm;
      for (uint32_t i = 0; i < len; i++) {
        dmaWrite(db, dm, off, static_cast<uint16_t>(addr >> 24));
        dmaWrite(db, dm, off, static_cast<uint16_t>(addr >> 16));
        dmaWrite(db, dm, off, static_cast<uint16_t>(addr >> 8));
        dmaWrite(db, dm, off, static_cast<uint16_t>(addr));
        addr += 8;
      }
      break;
    }
    case 0xFFCD: case 0xFFDD: {  // preenche com o padrão
      if (!dmaRegion(dmaDst_, db, dm)) break;
      uint32_t off = dmaDst_ & dm;
      for (uint32_t i = 0; i < len; i++) dmaWrite(db, dm, off, dmaPattern_);
      break;
    }
    default: break;
  }
  inDma_ = false;
}

// ------------------------------------------------------------------ Z80 (64 KB de RAM)
int NeoGeoCd::read(int a) { return zram_[static_cast<size_t>(a) & 0xFFFF]; }
void NeoGeoCd::write(int a, int v) { zram_[static_cast<size_t>(a) & 0xFFFF] = static_cast<uint8_t>(v); }

int NeoGeoCd::in(int port) {
  switch (port & 0xFF) {
    case 0x00: return soundCode_;
    case 0x04: case 0x05: case 0x06: case 0x07: ym.run(z80Cyc_); return ym.read(port & 3);
    default: return 0;
  }
}

void NeoGeoCd::out(int port, int v) {
  switch (port & 0xFF) {
    case 0x00: soundCode_ = 0; break;
    case 0x04: case 0x05: case 0x06: case 0x07: ym.run(z80Cyc_); ym.write(port & 3, static_cast<uint8_t>(v)); break;
    case 0x08: nmiEnabled_ = true; break;
    case 0x0C: soundReply_ = static_cast<uint8_t>(v); break;
    case 0x18: nmiEnabled_ = false; break;
    default: break;
  }
}

void NeoGeoCd::syncZ80(int64_t target) {
  if (!z80On_) {
    if (z80Cyc_ < target) z80Cyc_ = target;
    return;
  }
  int guard = 0;
  while (z80Cyc_ < target) {
    ym.run(z80Cyc_);
    z80.irqLine = ym.irq();
    const int t = z80.step();
    z80Cyc_ += static_cast<int64_t>(t > 0 ? t : 4) * 3;  // Z80 a 4 MHz = 1/3 do 68000
    if (++guard > 2000000) { z80Cyc_ = target; break; }
  }
}

// ------------------------------------------------------------------ execução
int NeoGeoCd::irqLevel() const {
  if (irqPending_ & IRQ_RASTER) return 3;
  if (irqPending_ & (IRQ_COMM | IRQ_DECODER)) return 2;
  if (irqPending_ & IRQ_VBL) return 1;
  return 0;
}

void NeoGeoCd::cdTick() {
  const bool cdz = biosFamily_ == 3;
  const int next = cd_.delay(cdz);  // o intervalo segue o estado de antes do setor (como no NeoCD)
  if (cd_.tick() && (irqMask1_ & 0x500) == 0x500) irqPending_ |= IRQ_DECODER;
  if ((irqMask1_ & 0x50) == 0x50 && commOn_) irqPending_ |= IRQ_COMM;
  cdCounter_ += next;
}

void NeoGeoCd::step() {
  cpu.ipl = irqLevel();
  int c = cpu.step();
  if (c <= 0) c = 4;
  cpu.cycles = 0;
  cyc_ += c;
  if (wdOn_) {
    wdCounter_ += static_cast<uint32_t>(c);
    if (wdCounter_ >= WATCHDOG_CYCLES) { wdCounter_ = 0; cpu.reset(); }  // o vigia só reinicia o 68000
  }
  // temporizador da interrupção de linha (pontos a 6 MHz)
  uint32_t dec = static_cast<uint32_t>(c) >> 1;
  timerFrag_ += c & 1;
  if (timerFrag_ >= 2) { timerFrag_ -= 2; dec++; }
  if (timerCounter_ > dec) {
    timerCounter_ -= dec;
  } else {
    for (uint32_t i = 0; i < dec; i++) {
      if (--timerCounter_ == 0) {
        if (timerCtrl_ & 0x80) timerCounter_ += timerReload_;
        if ((timerCtrl_ & 0x10) && (irqMask2_ & 0x300) == 0x300) irqPending_ |= IRQ_RASTER;
      }
    }
  }
  const int ev = lspc.run(c);
  if ((ev & neogeo::LSPC::EV_VBLANK) && (irqMask2_ & 0x30) == 0x30) irqPending_ |= IRQ_VBL;
  if ((ev & neogeo::LSPC::EV_RELOAD_TIMER) && (timerCtrl_ & 0x40)) timerCounter_ = timerReload_;
  cdCounter_ -= c;
  while (cdCounter_ <= 0) cdTick();
}

void NeoGeoCd::runFrame() {
  const size_t before = ym.available();
  frameEnd_ += CYC_PER_FRAME;
  int guard = 0;
  while (cyc_ < frameEnd_ && ++guard < 400000) step();
  if (cyc_ < frameEnd_) cyc_ = frameEnd_;
  syncZ80(cyc_);
  ym.run(cyc_);
  // CD-DA: tantas amostras quanto o YM2610 produziu neste quadro, na mesma taxa
  const size_t after = ym.available();
  const size_t B = cdL_.size();
  if (after > before) {
    size_t n = after - before;
    std::vector<float> l(n), r(n);
    cd_.produce(l.data(), r.data(), n, outRate_);
    for (size_t i = 0; i < n; i++) {
      cdL_[cdW_] = l[i];
      cdR_[cdW_] = r[i];
      cdW_ = (cdW_ + 1) % B;
      if (cdW_ == cdRd_) cdRd_ = (cdRd_ + 1) % B;
    }
  }
  frame++;
}

// ------------------------------------------------------------------ save state
void NeoGeoCd::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  uint32_t sectors = disc_->totalSectors(), tracks = static_cast<uint32_t>(disc_->tracks().size());
  io.sync(sectors);
  io.sync(tracks);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || sectors != disc_->totalSectors() ||
                       tracks != disc_->tracks().size()))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  z80.sync(io);
  lspc.sync(io);
  ym.sync(io);
  cd_.sync(io);
  io.bytes(ram_); io.bytes(spr_); io.bytes(fix_); io.bytes(pcm_); io.bytes(zram_); io.bytes(backup_);
  io.sync(lspc.sprOn); io.sync(lspc.fixOn); io.sync(lspc.videoOn);
  io.sync(vecRom_); io.sync(area_); io.sync(busReq_); io.sync(sprBank_); io.sync(pcmBank_); io.sync(selector_);
  io.sync(irqMask1_); io.sync(irqMask2_); io.sync(irqPending_); io.sync(commOn_); io.sync(z80On_); io.sync(wdOn_);
  io.sync(wdCounter_); io.raw(dmaCfg_, sizeof dmaCfg_); io.sync(dmaSrc_); io.sync(dmaDst_); io.sync(dmaLen_);
  io.sync(dmaPattern_); io.sync(soundCode_); io.sync(soundReply_); io.sync(nmiEnabled_);
  io.sync(timerCtrl_); io.sync(timerReload_); io.sync(timerCounter_); io.sync(timerFrag_); io.sync(cdCounter_);
  io.sync(cyc_); io.sync(frameEnd_); io.sync(z80Cyc_); io.sync(frame);
  if (io.reading()) {
    // só valores impossíveis são recusados (as áreas e os bancos são limitados no uso)
    if (irqPending_ & ~15) throw std::runtime_error("Estado salvo danificado.");
    if (area_ != 0 && area_ != AREA_SPR && area_ != AREA_PCM && area_ != AREA_Z80 && area_ != AREA_FIX)
      throw std::runtime_error("Estado salvo danificado.");
    if (wdCounter_ > WATCHDOG_CYCLES || timerFrag_ > 1) throw std::runtime_error("Estado salvo danificado.");
    if (cdCounter_ < -1000 || cdCounter_ > 200000) throw std::runtime_error("Estado salvo danificado.");
    if (cyc_ < 0 || frameEnd_ < 0 || cyc_ > frameEnd_ + CYC_PER_FRAME || frameEnd_ > cyc_ + 2 * CYC_PER_FRAME ||
        z80Cyc_ < cyc_ - CYC_PER_FRAME || z80Cyc_ > cyc_ + 1000)
      throw std::runtime_error("Estado salvo danificado.");
    ym.clampClock(cyc_);
  }
}

std::vector<uint8_t> NeoGeoCd::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void NeoGeoCd::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

}  // namespace indigo::neogeocd
