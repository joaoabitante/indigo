#include "vb.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::vb {

namespace {
const char STATE_MAGIC[4] = {'I', 'V', 'B', '1'};
// bits do registro do controle (SDR): 0 bateria fraca, 1 sempre 1, 2 A, 3 B, 4 R, 5 L, 6 direito-cima,
// 7 direito-direita, 8 esquerdo-direita, 9 esquerdo-esquerda, 10 esquerdo-baixo, 11 esquerdo-cima, 12 Start,
// 13 Select, 14 direito-esquerda, 15 direito-baixo
enum : uint16_t {
  P_A = 1 << 2, P_B = 1 << 3, P_R = 1 << 4, P_L = 1 << 5, P_RU = 1 << 6, P_RR = 1 << 7, P_LR = 1 << 8,
  P_LL = 1 << 9, P_LD = 1 << 10, P_LU = 1 << 11, P_START = 1 << 12, P_SELECT = 1 << 13, P_RL = 1 << 14, P_RD = 1 << 15,
};
bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
}  // namespace

// o cartucho não tem assinatura no cabeçalho: reconhece pela extensão
bool isVbRom(const std::vector<uint8_t>& rom, const std::string& nome) {
  return !rom.empty() && (endsWith(nome, ".vb") || endsWith(nome, ".vboy"));
}

VB::VB(const std::vector<uint8_t>& data) : cpu(*this), vip(*this), sram(0x10000, 0) {
  if (data.empty()) throw std::runtime_error("ROM do Virtual Boy vazia.");
  if (data.size() > (16u << 20)) throw std::runtime_error("ROM do Virtual Boy grande demais (máximo 16 MB).");
  size_t size = 1024;
  while (size < data.size()) size <<= 1;
  // tamanho que não é potência de 2: completa repetindo o fim (como um chip maior com linhas soltas)
  rom_.assign(size, 0xFF);
  std::copy(data.begin(), data.end(), rom_.begin());
  if (data.size() < size && (data.size() & (data.size() - 1)) == 0)
    for (size_t i = data.size(); i < size; i++) rom_[i] = data[i % data.size()];
  romMask_ = static_cast<uint32_t>(size - 1);
  // título: 20 bytes em 0xFFFFFDE0 (fim da ROM - 0x220); só mostra se for texto ASCII
  const size_t h = size - 0x220;
  std::string t;
  bool ascii = true;
  for (size_t i = 0; i < 20; i++) {
    const uint8_t c = rom_[h + i];
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) { ascii = false; break; }
    t += static_cast<char>(c);
  }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  if (ascii) title = t;
  reset();
}

void VB::reset() {
  std::memset(wram_, 0, sizeof wram_);
  ts = 0; runUntil = 0;
  irqLines_ = 0; wcr_ = 0;
  cpu.reset();
  cpu.irqLevel = -1;
  vip.reset();
  vsu.reset();
  tcr_ = 0; tReload_ = 0xFFFF; tCounter_ = 0xFFFF; tDivider_ = 2000;
  tStatus_ = tShadow_ = tReloadPending_ = false; tLast_ = 0;
  padLatched_ = 0; sdr_ = 0; scr_ = 0; readBit_ = 0; readCounter_ = 0; keyIrq_ = false; inLast_ = 0;
}

void VB::setIrq(int source, bool on) {
  if (on) irqLines_ |= static_cast<uint8_t>(1 << source);
  else irqLines_ &= static_cast<uint8_t>(~(1 << source));
  int lv = -1;
  for (int i = 4; i >= 0; i--) if (irqLines_ & (1 << i)) { lv = i; break; }
  cpu.irqLevel = static_cast<int8_t>(lv);
}

// ---------------------------------------------------------------------------------------------- E/S
uint8_t VB::ioRead8(uint32_t a) {
  switch ((a >> 24) & 7) {
    case 0: return vip.read8(a);
    case 2: return hwRead(a);
    case 6: return sram[a & 0xFFFF];
    default: return 0;  // VSU (só escrita), expansão e área livre
  }
}

uint16_t VB::ioRead16(uint32_t a) {
  switch ((a >> 24) & 7) {
    case 0: return vip.read16(a);
    case 2: return hwRead(a);
    case 6: return static_cast<uint16_t>(sram[a & 0xFFFE] | (sram[(a & 0xFFFE) + 1] << 8));
    default: return 0;
  }
}

void VB::ioWrite8(uint32_t a, uint8_t v) {
  switch ((a >> 24) & 7) {
    case 0: vip.write8(a, v); break;
    case 1: vsu.write(a, v, ts); break;
    case 2: hwWrite(a, v); break;
    case 6: sram[a & 0xFFFF] = v; sramUsed = true; sramDirty = true; break;
    default: break;
  }
}

void VB::ioWrite16(uint32_t a, uint16_t v) {
  switch ((a >> 24) & 7) {
    case 0: vip.write16(a, v); break;
    case 1: vsu.write(a, static_cast<uint8_t>(v), ts); break;
    case 2: hwWrite(a, static_cast<uint8_t>(v)); break;
    case 6:
      sram[a & 0xFFFE] = static_cast<uint8_t>(v); sram[(a & 0xFFFE) + 1] = static_cast<uint8_t>(v >> 8);
      sramUsed = true; sramDirty = true;
      break;
    default: break;
  }
}

// registros de hardware em 0x02000000 (8 bits, um a cada 4 bytes)
uint8_t VB::hwRead(uint32_t a) {
  if (a & 3) return 0;
  switch (a & 0xFF) {
    case 0x10: inputUpdate(); return static_cast<uint8_t>(sdr_);       // SDLR (resultado da última leitura serial)
    case 0x14: inputUpdate(); return static_cast<uint8_t>(sdr_ >> 8);  // SDHR
    case 0x28: inputUpdate(); return static_cast<uint8_t>(scr_ | 0x4C | (readCounter_ > 0 ? 2 : 0));
    case 0x18: timerUpdate(); return static_cast<uint8_t>(tCounter_);
    case 0x1C: timerUpdate(); return static_cast<uint8_t>(tCounter_ >> 8);
    case 0x20: timerUpdate(); return static_cast<uint8_t>(tcr_ | 0xE4 | (tStatus_ ? 2 : 0));
    case 0x24: return static_cast<uint8_t>(wcr_ | 0xFC);
    default: return 0;  // porta de comunicação: nada ligado
  }
}

void VB::hwWrite(uint32_t a, uint8_t v) {
  if (a & 3) return;
  switch (a & 0xFF) {
    case 0x18: timerUpdate(); tReload_ = static_cast<uint16_t>((tReload_ & 0xFF00) | v); tReloadPending_ = true; break;
    case 0x1C: timerUpdate(); tReload_ = static_cast<uint16_t>((tReload_ & 0x00FF) | (v << 8)); tReloadPending_ = true; break;
    case 0x20: {
      timerUpdate();
      if (v & 4) {  // Z-Stat-Clr (não limpa se o contador está parado em zero e ligado)
        if (!((tcr_ & 1) && tCounter_ == 0)) tStatus_ = false;
        tShadow_ = false;
      }
      if ((v & 1) && !(tcr_ & 1)) tDivider_ = (v & 0x10) ? 400 : 2000;
      tcr_ = v & 0x19;
      if (!(tcr_ & 8)) tStatus_ = tShadow_ = false;
      setIrq(1, tShadow_ && (tcr_ & 8));
      limit(timerNext());
      break;
    }
    case 0x24: wcr_ = v & 3; break;
    case 0x28:  // SCR: pedido de leitura do controle pelo hardware
      inputUpdate();
      if ((v & 4) && !(scr_ & 1) && readCounter_ <= 0) { padLatched_ = pad_; readBit_ = 0; readCounter_ = 640; }
      if (v & 1) { readCounter_ = 0; readBit_ = 0; }
      if (v & 0x80) { keyIrq_ = false; setIrq(0, false); }
      scr_ = v & 0xB1;
      if (readCounter_ > 0) limit(ts + readCounter_);
      break;
    default: break;
  }
}

// temporizador: conta para baixo a cada 20 µs ou 100 µs e avisa ao chegar a zero
void VB::timerUpdate() {
  const int64_t run = ts - tLast_;
  tLast_ = ts;
  if (!(tcr_ & 1) || run <= 0) return;
  int64_t left = run;
  while (left > 0) {
    if (left < tDivider_) { tDivider_ -= static_cast<int32_t>(left); break; }
    left -= tDivider_;
    if (!tCounter_ || tReloadPending_) { tCounter_ = tReload_; tReloadPending_ = false; }
    if (tCounter_) tCounter_--;
    if (!tCounter_ || tStatus_) tStatus_ = tShadow_ = true;
    tDivider_ = (tcr_ & 0x10) ? 400 : 2000;
    // muitas voltas sem ninguém olhando (contador recarregando a zero): não precisa repetir uma a uma
    if (left > (int64_t(1) << 24)) left %= int64_t(tDivider_) * 65536;
  }
  setIrq(1, tShadow_ && (tcr_ & 8));
}

int64_t VB::timerNext() const { return (tcr_ & 1) ? tLast_ + tDivider_ : INT64_MAX; }

void VB::inputUpdate() {
  int64_t clocks = ts - inLast_;
  inLast_ = ts;
  if (readCounter_ <= 0 || clocks <= 0) return;
  while (clocks > 0 && readCounter_ > 0) {
    if (clocks < readCounter_) { readCounter_ -= static_cast<int32_t>(clocks); break; }
    clocks -= readCounter_;
    sdr_ = static_cast<uint16_t>((sdr_ & ~(1u << readBit_)) | (padLatched_ & (1u << readBit_)));
    if (++readBit_ < 16) readCounter_ = 640;
    else {
      readCounter_ = 0;
      if (!(scr_ & 0x80)) { keyIrq_ = true; setIrq(0, true); }
    }
  }
}

// ---------------------------------------------------------------------------------------------- quadro
void VB::runFrame() {
  vip.frameStarted = false;
  const int64_t hardLimit = ts + 2 * VIP::FRAME_CYCLES;
  while (!vip.frameStarted && ts < hardLimit) {
    runUntil = std::min(vip.nextEvent(), hardLimit);
    runUntil = std::min(runUntil, timerNext());
    if (readCounter_ > 0) runUntil = std::min(runUntil, inLast_ + readCounter_);
    if (runUntil <= ts) runUntil = ts + 1;
    cpu.run();
    if (ts >= vip.nextEvent()) vip.update(ts);
    if (tcr_ & 1) timerUpdate();
    if (readCounter_ > 0) inputUpdate();
  }
  vsu.run(ts);
}

// ---------------------------------------------------------------------------------------------- estado
void VB::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  uint32_t mask = romMask_;
  io.sync(mask);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || mask != romMask_))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  io.sync(ts);
  cpu.sync(io);
  vip.sync(io);
  vsu.sync(io);
  io.raw(wram_, sizeof wram_);
  io.bytes(sram);
  io.sync(sramUsed);
  io.sync(irqLines_); io.sync(wcr_);
  io.sync(tcr_); io.sync(tReload_); io.sync(tCounter_); io.sync(tDivider_);
  io.sync(tStatus_); io.sync(tShadow_); io.sync(tReloadPending_); io.sync(tLast_);
  io.sync(pad_); io.sync(padLatched_); io.sync(sdr_); io.sync(scr_); io.sync(readBit_); io.sync(readCounter_);
  io.sync(keyIrq_); io.sync(inLast_);
  if (io.reading()) {
    clampTo<int64_t>(ts, 0, int64_t(1) << 60);
    irqLines_ &= 0x1F; wcr_ &= 3; tcr_ &= 0x19;
    clampTo(tDivider_, 1, 2000);
    // relógios parados há muito tempo são válidos (temporizador desligado); só valores impossíveis mudam
    if (tLast_ > ts || tLast_ < 0) tLast_ = ts;
    if (inLast_ > ts || inLast_ < 0) inLast_ = ts;
    clampTo(readBit_, 0, 16);
    clampTo(readCounter_, 0, 640);
    scr_ &= 0xB1;
    vsu.clampClock(ts);
    // o relógio do VIP anda junto com o da CPU
    vip.clampClock(ts);
    setIrq(0, irqLines_ & 1);
  }
}

std::vector<uint8_t> VB::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void VB::loadState(const std::vector<uint8_t>& s) {
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

// ---------------------------------------------------------------------------------------------- adaptador
VBCore::VBCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : m_(std::make_unique<VB>(rom)), fb_(768 * 224, 0xFF000000u) {
  // brilho linear do LED → valor de tela (curva gama 2,2, como o Mednafen)
  for (int i = 0; i < 256; i++) {
    double g = std::pow(i / 255.0, 1.0 / 2.2);
    g = g * 255 / 255;
    gamma_[i] = static_cast<uint32_t>(g * 255);
  }
  if (save) importSave(*save);
  applyKeys();
}

void VBCore::importSave(const std::vector<uint8_t>& b) {
  if (b.empty()) return;
  std::copy(b.begin(), b.begin() + std::min(b.size(), m_->sram.size()), m_->sram.begin());
  m_->sramUsed = true;
}

void VBCore::setStereoMode(int mode) {
  stereo_ = mode < 0 || mode > 2 ? 0 : mode;
  std::fill(fb_.begin(), fb_.end(), 0xFF000000u);
  compose();
}

void VBCore::applyKeys() {
  const uint32_t k = keys_;
  uint16_t p = 2;  // bit 1 sempre 1; bateria boa
  if (k & KEY_A) p |= P_A;
  if (k & KEY_B) p |= P_B;
  if (k & KEY_L) p |= P_L;
  if (k & KEY_R) p |= P_R;
  if (k & KEY_START) p |= P_START;
  if (k & KEY_UP) p |= P_LU;
  if (k & KEY_DOWN) p |= P_LD;
  if (k & KEY_LEFT) p |= P_LL;
  if (k & KEY_RIGHT) p |= P_LR;
  const bool sel = k & KEY_SELECT, x = k & KEY_X, y = k & KEY_Y;
  if (sel && (x || y)) {  // SELECT como modificador
    if (x) p |= P_RL;
    if (y) p |= P_RR;
  } else {
    if (sel) p |= P_SELECT;
    if (x) p |= P_RU;
    if (y) p |= P_RD;
  }
  if (rightPad_ & KEY_UP) p |= P_RU;
  if (rightPad_ & KEY_DOWN) p |= P_RD;
  if (rightPad_ & KEY_LEFT) p |= P_RL;
  if (rightPad_ & KEY_RIGHT) p |= P_RR;
  // direções opostas ao mesmo tempo confundem alguns jogos
  if ((p & (P_LL | P_LR)) == (P_LL | P_LR)) p &= ~(P_LL | P_LR);
  if ((p & (P_LU | P_LD)) == (P_LU | P_LD)) p &= ~(P_LU | P_LD);
  m_->setPad(p);
}

void VBCore::runFrame() {
  m_->runFrame();
  compose();
}

// monta a imagem de saída a partir da intensidade de cada olho (0xAABBGGRR)
void VBCore::compose() {
  const uint8_t* L = m_->vip.lum[0];
  const uint8_t* R = m_->vip.lum[1];
  const int n = VIP::W * VIP::H;
  if (stereo_ == 0) {
    for (int i = 0; i < n; i++) fb_[i] = 0xFF000000u | gamma_[L[i]];
  } else if (stereo_ == 1) {
    for (int i = 0; i < n; i++) {
      const uint32_t r = gamma_[R[i]];
      fb_[i] = 0xFF000000u | gamma_[L[i]] | (r << 8) | (r << 16);
    }
  } else {
    for (int y = 0; y < VIP::H; y++)
      for (int x = 0; x < VIP::W; x++) {
        fb_[y * 768 + x] = 0xFF000000u | gamma_[L[y * VIP::W + x]];
        fb_[y * 768 + 384 + x] = 0xFF000000u | gamma_[R[y * VIP::W + x]];
      }
  }
}

}  // namespace indigo::vb
