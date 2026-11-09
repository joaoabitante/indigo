#include <algorithm>
#include "vectrex.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::vectrex {

namespace {
bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool hasHeader(const std::vector<uint8_t>& r) { return r.size() >= 11 && std::memcmp(r.data(), "g GCE", 5) == 0; }

// níveis do DAC do AY (logarítmico, ~3 dB por passo; medidas usadas pelo MAME/ayumi)
const float AY_VOL[16] = {0.0f,    0.0106f, 0.0150f, 0.0222f, 0.0320f, 0.0466f, 0.0665f, 0.1039f,
                          0.1237f, 0.1986f, 0.2803f, 0.3548f, 0.4702f, 0.6030f, 0.7530f, 1.0f};

constexpr int UNITS_PER_PIXEL = 100;  // 33000 x 41000 unidades do integrador = 330 x 410 pixels
constexpr int GLOW_ONE = 4096;        // brilho de uma linha comum num quadro
}  // namespace

bool isVectrexBios(const std::vector<uint8_t>& rom) {
  // ROM do sistema: 8 KB, reset em F000 e os vetores de interrupção apontando para a RAM (CBxx)
  return rom.size() == 0x2000 && rom[0x1FFE] == 0xF0 && rom[0x1FFF] == 0x00 && rom[0x1FF2] == 0xCB &&
         rom[0x1FF8] == 0xCB && rom[0x1FFC] == 0xCB;
}

bool isVectrexRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.empty() || rom.size() > 0x10000) return false;
  return hasHeader(rom) || isVectrexBios(rom) || endsWith(n, ".vec") || endsWith(n, ".gam");
}

// ====================================================================== AY-3-8912

void AY::reset() {
  std::memset(r, 0, sizeof r);
  r[7] = 0xFF;  // tudo desligado até o jogo programar
  for (int c = 0; c < 3; c++) { toneCnt[c] = 0; toneOut[c] = 0; }
  noiseCnt = envCnt = envStep = 0;
  lfsr = 1;
  envHold = envAlt = half = false;
  out = 0;
}

void AY::write(int reg, uint8_t v) {
  static const uint8_t MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF,
                                   0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};
  reg &= 15;
  r[reg] = v & MASK[reg];
  if (reg == 13) {  // reinicia o envelope
    envStep = 0; envCnt = 0; envHold = false;
    envAlt = (r[13] & 4) != 0;  // aqui envAlt = direção atual (true = subindo)
  }
}

uint8_t AY::read(int reg, uint8_t portIn) const {
  reg &= 15;
  if (reg == 14) return (r[7] & 0x40) ? static_cast<uint8_t>(r[14] & portIn) : portIn;
  if (reg == 15) return (r[7] & 0x80) ? r[15] : 0xFF;
  return r[reg];
}

void AY::tick8() {
  for (int c = 0; c < 3; c++) {
    int period = r[c * 2] | (r[c * 2 + 1] << 8);
    if (period == 0) period = 1;
    if (++toneCnt[c] >= period) { toneCnt[c] = 0; toneOut[c] ^= 1; }
  }
  half = !half;
  if (half) {  // ruído e envelope andam a cada 16 ciclos
    int np = r[6] & 0x1F;
    if (np == 0) np = 1;
    if (++noiseCnt >= np) {
      noiseCnt = 0;
      const uint32_t bit = (lfsr ^ (lfsr >> 3)) & 1;
      lfsr = (lfsr >> 1) | (bit << 16);
    }
    int ep = r[11] | (r[12] << 8);
    if (ep == 0) ep = 1;
    if (++envCnt >= ep) {
      envCnt = 0;
      if (!envHold && ++envStep >= 16) {
        const int sh = r[13];
        if (!(sh & 8)) { envHold = true; envStep = 0; envAlt = false; }  // fica em 0
        else if (sh & 1) {                                                 // segura no fim (ou no oposto)
          envHold = true;
          const bool endHigh = (sh & 2) ? !envAlt : envAlt;
          envAlt = endHigh; envStep = 15;
        } else {
          if (sh & 2) envAlt = !envAlt;
          envStep = 0;
        }
      }
    }
  }
  int env = envAlt ? envStep : 15 - envStep;
  if (envHold) env = envAlt ? 15 : 0;
  const int noise = lfsr & 1;
  float s = 0;
  for (int c = 0; c < 3; c++) {
    const bool t = toneOut[c] || (r[7] & (1 << c));
    const bool n = noise || (r[7] & (8 << c));
    if (t && n) s += AY_VOL[(r[8 + c] & 0x10) ? env : (r[8 + c] & 15)];
  }
  out = s / 3.0f;
}

void AY::sync(StateIO& io) {
  io.raw(r, sizeof r);
  io.raw(toneCnt, sizeof toneCnt); io.raw(toneOut, sizeof toneOut);
  io.sync(noiseCnt); io.sync(envCnt); io.sync(envStep); io.sync(lfsr);
  io.sync(envHold); io.sync(envAlt); io.sync(half); io.sync(out);
  if (io.reading()) {
    for (int c = 0; c < 3; c++) { clampTo(toneCnt[c], 0, 4096); toneOut[c] &= 1; }
    clampTo(noiseCnt, 0, 32); clampTo(envCnt, 0, 65536); clampTo(envStep, 0, 15);
    lfsr &= 0x1FFFF; if (!lfsr) lfsr = 1;
    if (!(out >= 0.0f && out <= 1.0f)) out = 0;
  }
}

// ====================================================================== VIA 6522

void VIA::reset() {
  orb = ora = ddrb = ddra = acr = pcr = ifr = ier = 0;
  t1ll = t1lh = t2ll = sr = 0;
  t1c = t2c = 0xFFFF;
  t1armed = t2armed = false;
  t1pb7 = true;
  srBit = false;
  srLeft = 0;
}

uint8_t VIA::pbPins() const {
  uint8_t p = static_cast<uint8_t>((orb & ddrb) | ~ddrb);
  if (acr & 0x80) p = static_cast<uint8_t>((p & 0x7F) | (t1pb7 ? 0x80 : 0));
  return p;
}

bool VIA::cb2High() const {
  if ((acr >> 2) & 7) return srBit;
  return (pcr & 0xE0) != 0xC0;
}

// registrador de deslocamento no modo 110 (saída no ritmo do relógio): 1 bit a cada 2 ciclos; no 9º passo o
// último bit continua na saída (18 ciclos por byte, como medido no Vectrex). Ele gira: depois de 8 passos
// o valor volta ao original.
void VIA::srStep() {
  if (srLeft <= 0) return;
  if (srLeft & 1) {
    if (srLeft == 1) {
      srBit = sr & 1;
    } else {
      srBit = (sr & 0x80) != 0;
      sr = static_cast<uint8_t>((sr << 1) | (sr >> 7));
    }
  }
  if (--srLeft == 0) {
    ifr |= 0x04;
    if (((acr >> 2) & 7) == 4) srLeft = 18;  // modo 100: roda sem parar
  }
}

void VIA::tick() {
  if (t1c == 0) {
    if (t1armed) {
      ifr |= 0x40;
      if (acr & 0x40) t1pb7 = !t1pb7;
      else { t1armed = false; t1pb7 = true; }
    }
    t1c = (acr & 0x40) ? static_cast<uint16_t>(t1lh << 8 | t1ll) : 0xFFFF;
  } else {
    t1c--;
  }
  if (!(acr & 0x20)) {  // modo de contar pulsos (PB6) não é usado no Vectrex
    if (t2c == 0) {
      if (t2armed) { ifr |= 0x20; t2armed = false; }
      t2c = 0xFFFF;
    } else {
      t2c--;
    }
  }
  if ((acr >> 2) & 7) srStep();
}

uint8_t VIA::read(int reg, uint8_t pbIn, uint8_t paIn) {
  switch (reg & 15) {
    case 0x0: {
      ifr &= ~0x18;
      uint8_t v = static_cast<uint8_t>((orb & ddrb) | (pbIn & ~ddrb));
      if (acr & 0x80) v = static_cast<uint8_t>((v & 0x7F) | (t1pb7 ? 0x80 : 0));
      return v;
    }
    case 0x1: ifr &= ~0x03; return static_cast<uint8_t>((ora & ddra) | (paIn & ~ddra));
    case 0xF: return static_cast<uint8_t>((ora & ddra) | (paIn & ~ddra));
    case 0x2: return ddrb;
    case 0x3: return ddra;
    case 0x4: ifr &= ~0x40; return static_cast<uint8_t>(t1c);
    case 0x5: return static_cast<uint8_t>(t1c >> 8);
    case 0x6: return t1ll;
    case 0x7: return t1lh;
    case 0x8: ifr &= ~0x20; return static_cast<uint8_t>(t2c);
    case 0x9: return static_cast<uint8_t>(t2c >> 8);
    case 0xA:
      ifr &= ~0x04;
      if ((acr >> 2) & 7) srLeft = 18;
      return sr;
    case 0xB: return acr;
    case 0xC: return pcr;
    case 0xD: return static_cast<uint8_t>(ifr | (irq() ? 0x80 : 0));
    default: return static_cast<uint8_t>(ier | 0x80);
  }
}

void VIA::write(int reg, uint8_t v) {
  switch (reg & 15) {
    case 0x0: orb = v; ifr &= ~0x18; break;
    case 0x1: ora = v; ifr &= ~0x03; break;
    case 0xF: ora = v; break;
    case 0x2: ddrb = v; break;
    case 0x3: ddra = v; break;
    case 0x4: case 0x6: t1ll = v; break;
    case 0x5:
      t1lh = v;
      t1c = static_cast<uint16_t>(t1lh << 8 | t1ll);
      ifr &= ~0x40;
      t1armed = true;
      t1pb7 = false;  // PB7 (RAMP) desce enquanto o timer conta
      break;
    case 0x7: t1lh = v; ifr &= ~0x40; break;
    case 0x8: t2ll = v; break;
    case 0x9:
      t2c = static_cast<uint16_t>(v << 8 | t2ll);
      ifr &= ~0x20;
      t2armed = true;
      break;
    case 0xA:
      sr = v;
      ifr &= ~0x04;
      if ((acr >> 2) & 7) { srLeft = 18; srStep(); srStep(); }
      break;
    case 0xB: acr = v; break;
    case 0xC: pcr = v; break;
    case 0xD: ifr &= static_cast<uint8_t>(~(v & 0x7F)); break;
    default:
      if (v & 0x80) ier |= v & 0x7F;
      else ier &= static_cast<uint8_t>(~(v & 0x7F));
      break;
  }
}

void VIA::sync(StateIO& io) {
  io.sync(orb); io.sync(ora); io.sync(ddrb); io.sync(ddra); io.sync(acr); io.sync(pcr); io.sync(ifr); io.sync(ier);
  io.sync(t1ll); io.sync(t1lh); io.sync(t2ll); io.sync(sr); io.sync(t1c); io.sync(t2c);
  io.sync(t1armed); io.sync(t2armed); io.sync(t1pb7); io.sync(srBit); io.sync(srLeft);
  if (io.reading()) { ifr &= 0x7F; ier &= 0x7F; clampTo(srLeft, 0, 18); }
}

// ====================================================================== máquina

VectrexCore::VectrexCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>*)
    : cpu_(*this), ram_(1024, 0), glow_(W * H, 0), fb_(W * H, 0xFF000000u), buf_(BUF, 0.0f) {
  const std::vector<uint8_t> fw = firmware("vectrex");
  if (fw.size() == 0x2000) {
    bios_ = fw;
    if (!isVectrexBios(rom) || rom != fw) cart_ = rom;
  } else if (isVectrexBios(rom)) {
    bios_ = rom;  // abriram a própria BIOS: liga sem cartucho (Mine Storm)
  } else {
    throw std::runtime_error("Abra o arquivo da BIOS do Vectrex (8 KB) como um jogo");
  }
  if (cart_.size() > 0x10000) throw std::runtime_error("Cartucho de Vectrex grande demais.");
  // título: depois de "g GCE 19xx\x80", ponteiro da música (2) e altura/largura/y/x (4), texto até 0x80
  const std::vector<uint8_t>& h = cart_;
  if (cart_.empty()) title_ = "MINE STORM";  // jogo embutido na BIOS
  else if (hasHeader(h)) {
    size_t p = 5;
    while (p < h.size() && p < 32 && h[p] != 0x80) p++;
    p += 7;
    for (; p < h.size() && h[p] != 0x80 && h[p] != 0 && title_.size() < 40; p++)
      if (h[p] >= 0x20 && h[p] < 0x7F) title_ += static_cast<char>(h[p]);
    while (!title_.empty() && title_.back() == ' ') title_.pop_back();
  }
  // 64 KB: o banco escolhido com PB6 em 1 (estado ao ligar) tem de ter o cabeçalho; senão a polaridade é a outra
  if (cart_.size() > 0x8000) {
    const bool hi = std::memcmp(cart_.data() + 0x8000, "g GCE", 5) == 0;
    bankInvert_ = !hi && hasHeader(cart_);
  }
  setSampleRate(48000);
  reset();
}

void VectrexCore::reset() {
  via_.reset();
  ay_.reset();
  for (size_t i = 0; i < ram_.size(); i++) ram_[i] = static_cast<uint8_t>(i);  // conteúdo ao ligar (como o vecx)
  dac_ = ySH_ = ref_ = zSH_ = soundSH_ = pot_ = 0;
  ayLatch_ = ayMode_ = 0;
  bx_ = by_ = 0;
  segOn_ = false;
  clock_ = devClock_ = instrStart_ = 0;
  frameEnd_ = 0;
  analogInputs();
  cpu_.a = cpu_.b = 0; cpu_.x = cpu_.y = cpu_.u = cpu_.s = 0;
  cpu_.irqLine = cpu_.firqLine = false;
  cpu_.reset();
}

void VectrexCore::setSampleRate(int rate) {
  ticksPerSample_ = (1500000.0 / 8.0) / (rate > 0 ? rate : 48000);
}

void VectrexCore::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos_ == wpos_) { l[k] = r[k] = 0; continue; }
    l[k] = r[k] = buf_[rpos_];
    rpos_ = (rpos_ + 1) % BUF;
  }
}

uint8_t VectrexCore::joyPot(int sel) const {
  int v = 0;
  if (sel == 0) v = (keys_ & KEY_RIGHT) ? 127 : (keys_ & KEY_LEFT) ? -128 : 0;
  else if (sel == 1) v = (keys_ & KEY_UP) ? 127 : (keys_ & KEY_DOWN) ? -128 : 0;
  return static_cast<uint8_t>(static_cast<int8_t>(v));
}

// ---------------------------------------------------------------- barramento

uint8_t VectrexCore::read(uint16_t a) {
  if (a < 0x8000) {
    if (cart_.empty()) return 0xFF;
    const size_t off = (cart_.size() > 0x8000 ? static_cast<size_t>(bank_) * 0x8000 : 0) + a;
    return off < cart_.size() ? cart_[off] : 0xFF;
  }
  if (a >= 0xE000) return bios_[a - 0xE000];
  if (a >= 0xD000) {
    if (a >= 0xD800 || !(a & 0x0800)) {
      // VIA: acerta os dispositivos até o ciclo em que o 6809 lê (o último da instrução)
      catchUp(ioTime());
      const uint8_t pbIn = static_cast<uint8_t>(0xDF | (dac_ < static_cast<int8_t>(pot_) ? 0x20 : 0));
      uint8_t paIn = 0xFF;
      if (ayMode_ == 1) {
        const uint8_t buttons = static_cast<uint8_t>(~(((keys_ & KEY_A) ? 1 : 0) | ((keys_ & KEY_B) ? 2 : 0) |
                                                       ((keys_ & KEY_X) ? 4 : 0) | ((keys_ & KEY_Y) ? 8 : 0)));
        paIn = ay_.read(ayLatch_, buttons);
      }
      return via_.read(a & 15, pbIn, paIn);
    }
  }
  if (a >= 0xC800) return ram_[a & 0x3FF];
  return 0xFF;
}

void VectrexCore::write(uint16_t a, uint8_t v) {
  if (a < 0xC800 || a >= 0xE000) return;
  if (a < 0xD000) { ram_[a & 0x3FF] = v; return; }
  if (a >= 0xD800) ram_[a & 0x3FF] = v;  // D800-DFFF: RAM e VIA juntas
  catchUp(ioTime());
  const int reg = a & 15;
  via_.write(reg, v);
  if (reg <= 3 || reg == 15) analogInputs();
}

// ciclo em que o 6809 faz o acesso: o último da instrução (no acesso de 16 bits, o primeiro byte 1 antes)
uint64_t VectrexCore::ioTime() const {
  if (accessAtStart) return instrStart_;
  const int off = cpu_.cyc - 1 - cpu_.lag;
  return instrStart_ + static_cast<uint64_t>(off > 0 ? off : 0);
}

// porta A -> DAC; porta B -> multiplexador, controle do AY e banco do cartucho
void VectrexCore::analogInputs() {
  const uint8_t pins = via_.pbPins();
  if (via_.ddra == 0xFF) dac_ = static_cast<int8_t>(via_.ora);
  if (!(pins & 1)) {
    const int sel = (pins >> 1) & 3;
    switch (sel) {
      case 0: ySH_ = dac_; break;
      case 1: ref_ = dac_; break;
      case 2: zSH_ = dac_ > 0 ? dac_ : 0; break;
      default: soundSH_ = dac_; break;
    }
    pot_ = static_cast<int8_t>(joyPot(sel));
  }
  ayMode_ = static_cast<uint8_t>(((pins & 0x10) ? 2 : 0) | ((pins & 0x08) ? 1 : 0));
  if (ayMode_ == 3) ayLatch_ = via_.ora & 0x0F;
  else if (ayMode_ == 2) ay_.write(ayLatch_, via_.ora);
  if (cart_.size() > 0x8000) bank_ = ((pins >> 6) & 1) ^ (bankInvert_ ? 1 : 0);
}

// ---------------------------------------------------------------- tempo

void VectrexCore::catchUp(uint64_t t) {
  while (devClock_ < t) { tick(); devClock_++; }
}

void VectrexCore::tick() {
  via_.tick();
  const uint8_t pins = via_.pbPins();
  const int32_t px = bx_, py = by_;
  int32_t dx = 0, dy = 0;
  const bool zero = via_.ca2Low();
  if (zero) {
    bx_ = by_ = 0;
  } else if (!(pins & 0x80)) {  // RAMP ativo: os integradores andam
    dx = dac_ - ref_;
    dy = ySH_ - ref_;
    // os integradores saturam (como no console, que tem limite de tensão): o mesmo limite que o carregar do
    // estado impõe, senão um estado salvo com o feixe muito longe não voltaria igual
    bx_ = std::clamp(bx_ + dx, -(1 << 24), 1 << 24);
    by_ = std::clamp(by_ + dy, -(1 << 24), 1 << 24);
  }
  const bool vis = via_.cb2High() && zSH_ > 0 && !zero;
  if (vis) {
    if (!(segOn_ && dx == segDX_ && dy == segDY_ && zSH_ == segZ_)) {
      flushSegment();
      segOn_ = true; segX_ = px; segY_ = py; segDX_ = dx; segDY_ = dy; segZ_ = zSH_; segCycles_ = 0;
    }
    if (segCycles_ < (1 << 24)) segCycles_++;
  } else if (segOn_) {
    flushSegment();
  }

  if (++sub8_ >= 8) {  // som: o AY anda em passos de 8 ciclos; a amostra é a média desses passos
    sub8_ = 0;
    ay_.tick8();
    acc_ += ay_.level() * 0.55 + soundSH_ / 128.0 * 0.35;
    accN_++;
    frac_ += 1;
    if (frac_ >= ticksPerSample_) {
      frac_ -= ticksPerSample_;
      const double in = acc_ / (accN_ ? accN_ : 1);
      acc_ = 0; accN_ = 0;
      const double o = in - cap_;  // passa-alta: tira o nível contínuo
      cap_ += o * 0.0015;
      buf_[wpos_] = static_cast<float>(o);
      wpos_ = (wpos_ + 1) % BUF;
      if (wpos_ == rpos_) rpos_ = (rpos_ + 1) % BUF;
    }
  }
}

void VectrexCore::runFrame() {
  frameEnd_ += CYCLES_PER_FRAME;
  while (clock_ < frameEnd_) stepInstr();
  present();
}

void VectrexCore::stepInstr() {
  instrStart_ = clock_;
  cpu_.irqLine = via_.irq();
  clock_ += static_cast<uint64_t>(cpu_.step());
  catchUp(clock_);
}

// ---------------------------------------------------------------- desenho

void VectrexCore::plot(int x, int y, float v) {
  if (static_cast<unsigned>(x) >= static_cast<unsigned>(W) || static_cast<unsigned>(y) >= static_cast<unsigned>(H)) return;
  uint16_t& g = glow_[y * W + x];
  const int n = g + static_cast<int>(v * GLOW_ONE);
  g = static_cast<uint16_t>(n > 65535 ? 65535 : n);
}

// amostras a cada ≤0,6 pixel, cada uma espalhada nos 4 pixels vizinhos (bilinear): linha suavizada
void VectrexCore::drawLine(float x0, float y0, float x1, float y1, float inten) {
  const float dx = x1 - x0, dy = y1 - y0;
  const float len = std::sqrt(dx * dx + dy * dy);
  int n = static_cast<int>(len / 0.6f) + 1;
  if (n > 4000) n = 4000;
  const float step = len / n;
  const float w = inten * (len > 0.6f ? step : 1.0f);
  for (int i = 0; i <= n; i++) {
    const float t = static_cast<float>(i) / n;
    const float fx = x0 + dx * t - 0.5f, fy = y0 + dy * t - 0.5f;
    if (fx < -2 || fy < -2 || fx > W + 1 || fy > H + 1) continue;
    const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
    const float ax = fx - ix, ay = fy - iy;
    const float ww = (i == 0 || i == n) ? w * 0.5f : w;
    plot(ix, iy, ww * (1 - ax) * (1 - ay));
    plot(ix + 1, iy, ww * ax * (1 - ay));
    plot(ix, iy + 1, ww * (1 - ax) * ay);
    plot(ix + 1, iy + 1, ww * ax * ay);
  }
}

void VectrexCore::flushSegment() {
  if (!segOn_) return;
  segOn_ = false;
  if (segCycles_ <= 0) return;
  const float x0 = W / 2.0f + static_cast<float>(segX_) / UNITS_PER_PIXEL;
  const float y0 = H / 2.0f - static_cast<float>(segY_) / UNITS_PER_PIXEL;
  const float x1 = W / 2.0f + static_cast<float>(bx_) / UNITS_PER_PIXEL;
  const float y1 = H / 2.0f - static_cast<float>(by_) / UNITS_PER_PIXEL;
  const float len = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
  const float z = segZ_ / 127.0f;
  if (len < 0.7f) {  // ponto: o brilho cresce com o tempo parado
    float e = 0.3f + 0.12f * segCycles_;
    if (e > 3.0f) e = 3.0f;
    drawLine(x0, y0, x1, y1, z * e);
    return;
  }
  float dwell = segCycles_ / len;  // ciclos por pixel: linha lenta = mais brilhante
  float e = 0.35f + 0.65f * dwell;
  if (e > 2.5f) e = 2.5f;
  drawLine(x0, y0, x1, y1, z * e);
}

namespace {
// cor do fósforo para cada pixel (gancho para overlays coloridos: hoje é o mesmo branco-azulado na tela toda)
inline void tintFor(int /*x*/, int /*y*/, float& r, float& g, float& b) { r = 0.86f; g = 0.92f; b = 1.0f; }

struct GlowLut {
  float v[4096];
  GlowLut() { for (int i = 0; i < 4096; i++) v[i] = 1.0f - std::exp(-(i * 16.0f / GLOW_ONE) * 1.1f); }
};
const GlowLut LUT;
}  // namespace

void VectrexCore::present() {
  // o trecho aceso em andamento entra neste quadro e continua no próximo a partir de onde parou
  if (segOn_) {
    const int32_t dx = segDX_, dy = segDY_, z = segZ_;
    flushSegment();
    segOn_ = true; segX_ = bx_; segY_ = by_; segDX_ = dx; segDY_ = dy; segZ_ = z; segCycles_ = 0;
  }
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      uint16_t& g = glow_[y * W + x];
      const float v = LUT.v[g >> 4];
      float tr, tg, tb;
      tintFor(x, y, tr, tg, tb);
      const uint32_t R = static_cast<uint32_t>(v * tr * 255.0f + 0.5f);
      const uint32_t G = static_cast<uint32_t>(v * tg * 255.0f + 0.5f);
      const uint32_t B = static_cast<uint32_t>(v * tb * 255.0f + 0.5f);
      fb_[y * W + x] = 0xFF000000u | (B << 16) | (G << 8) | R;
      g = static_cast<uint16_t>((g * 13u) >> 5);  // persistência: ~40% sobra para o próximo quadro
    }
  }
}

// ---------------------------------------------------------------- save state

void VectrexCore::sync(StateIO& io) {
  uint32_t magic = 0x58434556;  // "VECX"
  uint32_t ver = 1;
  io.sync(magic); io.sync(ver);
  if (io.reading() && (magic != 0x58434556 || ver != 1)) throw std::runtime_error("Estado salvo de outro console.");
  uint32_t cartSize = static_cast<uint32_t>(cart_.size());
  io.sync(cartSize);
  if (io.reading() && cartSize != cart_.size()) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu_.sync(io);
  via_.sync(io);
  ay_.sync(io);
  io.bytes(ram_);
  io.sync(bank_); io.sync(clock_); io.sync(devClock_); io.sync(frameEnd_);
  io.sync(dac_); io.sync(ySH_); io.sync(ref_); io.sync(zSH_); io.sync(soundSH_); io.sync(pot_);
  io.sync(ayLatch_); io.sync(ayMode_); io.sync(bx_); io.sync(by_);
  io.sync(segOn_); io.sync(segX_); io.sync(segY_); io.sync(segCycles_); io.sync(segDX_); io.sync(segDY_); io.sync(segZ_);
  uint32_t gn = static_cast<uint32_t>(glow_.size());
  io.sync(gn);
  if (io.reading() && gn != glow_.size()) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  io.raw(glow_.data(), glow_.size() * sizeof(uint16_t));
  io.sync(frac_); io.sync(acc_); io.sync(cap_); io.sync(accN_); io.sync(sub8_);
  if (io.reading()) {
    bank_ &= 1;
    ayLatch_ &= 15;
    if (ayMode_ > 3) ayMode_ = 0;
    if (zSH_ < 0) zSH_ = 0;
    if (clock_ > (uint64_t(1) << 50)) clock_ = 0;
    if (frameEnd_ + 64 < clock_ || frameEnd_ > clock_ + CYCLES_PER_FRAME + 64) frameEnd_ = clock_;  // a última instrução passa um pouco do fim
    if (devClock_ > clock_ || devClock_ + 64 < clock_) devClock_ = clock_;
    clampTo(bx_, -(1 << 24), 1 << 24); clampTo(by_, -(1 << 24), 1 << 24);
    clampTo(segX_, -(1 << 24), 1 << 24); clampTo(segY_, -(1 << 24), 1 << 24);
    clampTo(segDX_, -256, 256); clampTo(segDY_, -256, 256); clampTo(segZ_, 0, 127);
    clampTo(segCycles_, 0, 1 << 24);
    finiteOr(frac_, 0); clampTo(frac_, 0.0, ticksPerSample_);
    finiteOr(acc_, 0); finiteOr(cap_, 0); clampTo(accN_, 0, 1 << 20); clampTo(sub8_, 0, 7);
  }
}

std::vector<uint8_t> VectrexCore::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void VectrexCore::loadState(const std::vector<uint8_t>& state) {
  // carrega numa cópia: se o arquivo estiver danificado, o jogo em andamento continua igual
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(state);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

}  // namespace indigo::vectrex
