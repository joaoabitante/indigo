// Máquina Atari 2600: mapa de memória do 6507 (13 bits: TIA, RIOT e cartucho), RIOT 6532 (RAM, temporizador,
// joystick e chaves do console), WSYNC (a CPU para até o fim da linha), detecção NTSC/PAL, paletas, som e
// save states.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "a2600.h"

namespace indigo::a2600 {

namespace {

// paletas "padrão" do Stella como ele as mostra (com os ajustes padrão de contraste/gama), lidas da tela do
// Stella com uma ROM que pinta as 128 cores (0xRRGGBB, índice = valor de cor do TIA >> 1)
constexpr uint32_t kNtsc[128] = {
  0x000000, 0x3f3f3e, 0x646463, 0x848483, 0xa2a2a1, 0xbabab9, 0xd2d2d1, 0xeaeae9,
  0x3d3d00, 0x5e5e0a, 0x7b7b15, 0x999920, 0xb4b42a, 0xcdcd34, 0xe6e63e, 0xfdfd48,
  0x712300, 0x863d0b, 0x995718, 0xad6f26, 0xbd8632, 0xcd9b3e, 0xdcb049, 0xeac254,
  0x861500, 0x9a2f0e, 0xae481e, 0xc0612f, 0xd1773e, 0xe08d4d, 0xefa25b, 0xfdb568,
  0x8a0000, 0x9e1312, 0xb12827, 0xc23d3c, 0xd25150, 0xe26463, 0xef7574, 0xfd8685,
  0x790058, 0x8d126e, 0xa02784, 0xb13b98, 0xc04eaa, 0xd061bc, 0xdd71cc, 0xea82dc,
  0x450078, 0x5d128f, 0x7227a4, 0x883bb9, 0x9b4eca, 0xae61dc, 0xbf71ec, 0xd082fb,
  0x0e0085, 0x291399, 0x4328ad, 0x5d3dbf, 0x7451d0, 0x8b64df, 0xa175ee, 0xb586fb,
  0x00008a, 0x12139d, 0x2428b0, 0x373dc1, 0x4951d1, 0x5a64e0, 0x6a75ee, 0x7986fb,
  0x00157d, 0x123193, 0x244ca7, 0x3767bb, 0x4980cc, 0x5a97dd, 0x6aaeed, 0x79c2fb,
  0x002758, 0x124574, 0x24628d, 0x377ea7, 0x4997be, 0x5ab0d4, 0x6ac7e8, 0x79ddfb,
  0x003526, 0x125742, 0x24765d, 0x379576, 0x49b18e, 0x5acca5, 0x6ae5bb, 0x79fdcf,
  0x003900, 0x135b12, 0x287927, 0x3d973c, 0x51b350, 0x64cd63, 0x75e674, 0x86fd85,
  0x0e3200, 0x2b5411, 0x477323, 0x639336, 0x7db048, 0x95cb59, 0xade569, 0xc2fd78,
  0x272e00, 0x454e0f, 0x626b21, 0x7e8833, 0x97a343, 0xb0bc53, 0xc7d462, 0xddea70,
  0x3d2300, 0x5e420d, 0x7b5f1d, 0x997b2d, 0xb4963b, 0xcdaf4a, 0xe6c757, 0xfddd64,
};
constexpr uint32_t kPal[128] = {
  0x070707, 0x292928, 0x4e4e4d, 0x70706f, 0x8f8f8e, 0xafafae, 0xcacac9, 0xe4e4e3,
  0x070707, 0x292928, 0x4e4e4d, 0x70706f, 0x8f8f8e, 0xafafae, 0xcacac9, 0xe4e4e3,
  0x311c00, 0x5b3c00, 0x816500, 0xa48800, 0xbfa62b, 0xdbc252, 0xf7df73, 0xfff094,
  0x003a00, 0x006400, 0x318800, 0x5aa805, 0x7ac532, 0x9ae058, 0xb9fd78, 0xd1ff9b,
  0x4e0000, 0x751e00, 0x984c00, 0xb56e2c, 0xd28e53, 0xedab75, 0xffc994, 0xffd9b5,
  0x003e00, 0x006700, 0x10880f, 0x3aa739, 0x60c35f, 0x81e080, 0xa1fba0, 0xbfffbe,
  0x59000c, 0x7f0518, 0x9e3341, 0xbc5966, 0xd97986, 0xf49aa5, 0xffb7c3, 0xffd6dc,
  0x00331f, 0x005f3c, 0x028457, 0x32a278, 0x57bf98, 0x78dcb6, 0x98f8d4, 0xb7ffe6,
  0x4a003a, 0x7e0062, 0x9c2782, 0xbb4ea1, 0xd671be, 0xf291db, 0xffb2f1, 0xffd0f5,
  0x002c45, 0x004f71, 0x0c7392, 0x3793b0, 0x5db0cc, 0x7ecde8, 0x9de9ff, 0xbdffff,
  0x410070, 0x6a0093, 0x8925b0, 0xa74ccc, 0xc56fe8, 0xe28fff, 0xeab0ff, 0xf2d0ff,
  0x002477, 0x00349a, 0x245ab7, 0x4b7ad5, 0x6e99f0, 0x8fb9ff, 0xb0d7ff, 0xcff5ff,
  0x1a008b, 0x4700ae, 0x6a2ec9, 0x8a54e5, 0xa976ff, 0xbf97ff, 0xd2b6ff, 0xe6d6ff,
  0x000091, 0x1c16ae, 0x453fca, 0x6964e6, 0x8884ff, 0xa9a5ff, 0xc9c5ff, 0xe7e3ff,
  0x070707, 0x292928, 0x4e4e4d, 0x70706f, 0x8f8f8e, 0xafafae, 0xcacac9, 0xe4e4e3,
  0x070707, 0x292928, 0x4e4e4d, 0x70706f, 0x8f8f8e, 0xafafae, 0xcacac9, 0xe4e4e3,
};

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

}  // namespace

bool isA2600Rom(const std::vector<uint8_t>& rom, const std::string& nome) {
  if (rom.empty() || rom.size() > 512 * 1024) return false;
  if (endsWith(nome, ".a26")) return true;
  if (!endsWith(nome, ".bin")) return false;
  // .bin: último recurso. Tamanho de cartucho comum e vetor de reset dentro do cartucho (A12 = 1)
  const size_t s = rom.size();
  const bool sizeOk = s <= 4096 || s == 8192 || (s >= 10240 && s <= 10496) || s == 12288 || s == 16384 ||
                      s == 32768 || s == 65536;
  if (!sizeOk || s < 256) return false;
  if (s >= 5 && std::memcmp(rom.data(), "g GCE", 5) == 0) return false;  // Vectrex
  const uint8_t hi = rom[s - 3];
  return (hi & 0x10) != 0;
}

// ================================================================== máquina
Machine::Machine(const std::vector<uint8_t>& rom) : cart(rom) {
  cart.cycleCounter = &cycles;
  cpu.bus = this;
  cpu.decimalMode = true;
  cpu.magic = 0xEE;
  powerOn();
}

void Machine::powerOn() {
  cycles = 0;
  std::memset(ram, 0, sizeof ram);
  tia.reset();
  tia.setPal(pal);
  cart.clockRate = pal ? 1182298.0 : 1193191.66666667;
  cart.reset();
  timer_ = 0; intFlag_ = 0; ddra_ = ddrb_ = outa_ = outb_ = 0;
  divider_ = 1024; sub_ = 0;
  wrapped_ = edgePositive_ = false;
  dataBus_ = 0;
  // como o Stella: sem a sequência de reset de 7 ciclos, registradores zerados, pilha em $FD
  cpu.powerOn();
  cpu.s = 0xFD;
  cpu.p = 0x20;
  cpu.pc = static_cast<uint16_t>(cart.peek(0x1FFC) | (cart.peek(0x1FFD) << 8));
}

void Machine::setKeys(uint32_t k) {
  uint8_t j = 0xFF;
  if (k & KEY_RIGHT) j &= ~0x80;
  if (k & KEY_LEFT) j &= ~0x40;
  if (k & KEY_DOWN) j &= ~0x20;
  if (k & KEY_UP) j &= ~0x10;
  if ((j & 0xC0) == 0) j |= 0xC0;  // o joystick de verdade não aperta os dois lados juntos
  if ((j & 0x30) == 0) j |= 0x30;
  joy_ = static_cast<uint8_t>((joy_ & 0x0F) | (j & 0xF0));
  tia.fire[0] = k & KEY_A;
  const uint32_t pressed = k & ~prevKeys_;
  prevKeys_ = k;
  if (pressed & KEY_L) switches_ ^= 0x40;
  if (pressed & KEY_R) switches_ ^= 0x80;
  if (pressed & KEY_X) switches_ ^= 0x08;
  switches_ = static_cast<uint8_t>((switches_ & ~0x03) | ((k & KEY_START) ? 0 : 1) | ((k & KEY_SELECT) ? 0 : 2));
}

void Machine::setKeys2(uint32_t k) {
  uint8_t j = 0x0F;
  if (k & KEY_RIGHT) j &= ~0x08;
  if (k & KEY_LEFT) j &= ~0x04;
  if (k & KEY_DOWN) j &= ~0x02;
  if (k & KEY_UP) j &= ~0x01;
  if ((j & 0x0C) == 0) j |= 0x0C;
  if ((j & 0x03) == 0) j |= 0x03;
  joy_ = static_cast<uint8_t>((joy_ & 0xF0) | j);
  tia.fire[1] = k & KEY_A;
}

void Machine::runFrame() {
  tia.frameDone = false;
  tia.audioOut.clear();
  // um quadro tem ~20 mil ciclos; sem VSYNC o TIA fecha o quadro sozinho em ~680 linhas (~52 mil ciclos)
  const uint64_t limit = cycles + 80000;
  while (!tia.frameDone && cycles < limit) cpu.step();
}

void Machine::cycle() {
  cycles++;
  tia.run(3);
  riotTick();
}

void Machine::waitWsync() {
  // RDY baixo: a CPU espera o começo da próxima linha (o temporizador do RIOT continua contando)
  int n = (228 - tia.hctr()) % 228;
  while (n >= 3) { cycle(); n -= 3; }
  if (n) tia.run(n);
}

uint8_t Machine::read(uint16_t addr) {
  if (tia.wsyncRequested) { tia.wsyncRequested = false; waitWsync(); }
  cycle();
  const uint16_t a = addr & 0x1FFF;
  uint8_t v;
  if (a & 0x1000) {
    v = cart.read(a, dataBus_);
  } else {
    v = (a & 0x80) ? riotRead(a) : tia.read(static_cast<uint8_t>(a), dataBus_);
    if (cart.watchesLow()) cart.lowAccess(a, v, false);
  }
  dataBus_ = v;
  return v;
}

void Machine::write(uint16_t addr, uint8_t v) {
  cycle();
  const uint16_t a = addr & 0x1FFF;
  dataBus_ = v;
  if (a & 0x1000) {
    cart.write(a, v);
    return;
  }
  if (a & 0x80) riotWrite(a, v);
  else tia.write(static_cast<uint8_t>(a), v);
  if (cart.watchesLow()) cart.lowAccess(a, v, true);
}

uint8_t Machine::peek(uint16_t addr) {
  const uint16_t a = addr & 0x1FFF;
  if (a & 0x1000) return cart.peek(a);
  if ((a & 0x280) == 0x80) return ram[a & 0x7F];
  return 0;
}

// ---------------------------------------------------------------- RIOT 6532
void Machine::riotTick() {
  wrapped_ = false;
  const bool tick = sub_ >= divider_ - 1;
  sub_ = tick ? 0 : static_cast<uint16_t>(sub_ + 1);
  if (!(intFlag_ & 0x80)) {
    if (tick) {
      if (timer_ == 0) { timer_ = 0xFF; intFlag_ |= 0x80; wrapped_ = true; }
      else timer_--;
    }
  } else {
    // depois de chegar a zero, conta a cada ciclo
    timer_--;
    wrapped_ = timer_ == 0xFF;
  }
}

uint8_t Machine::riotRead(uint16_t a) {
  if (!(a & 0x200)) return ram[a & 0x7F];
  switch (a & 7) {
    case 0: return static_cast<uint8_t>((outa_ | ~ddra_) & joy_);                    // SWCHA
    case 1: return ddra_;
    case 2: return static_cast<uint8_t>((outb_ | ~ddrb_) & (switches_ | ddrb_));    // SWCHB
    case 3: return ddrb_;
    case 4: case 6:                                                                  // INTIM
      if (!wrapped_) intFlag_ &= ~0x80;
      return timer_;
    default: {                                                                       // TIMINT
      const uint8_t r = intFlag_;
      intFlag_ &= ~0x40;
      return r;
    }
  }
}

void Machine::riotWrite(uint16_t a, uint8_t v) {
  if (!(a & 0x200)) { ram[a & 0x7F] = v; return; }
  if (a & 0x04) {
    if (a & 0x10) {
      static constexpr uint16_t div[4] = {1, 8, 64, 1024};
      divider_ = div[a & 3];
      timer_ = v;
      sub_ = static_cast<uint16_t>(divider_ - 1);
      if (!wrapped_) intFlag_ &= ~0x80;
    } else {
      edgePositive_ = a & 1;
    }
    return;
  }
  switch (a & 3) {
    case 0: outa_ = v; break;
    case 1: ddra_ = v; break;
    case 2: outb_ = v; break;
    default: ddrb_ = v; break;
  }
}

// ---------------------------------------------------------------- estado
void Machine::sync(StateIO& io) {
  cpu.sync(io);
  tia.sync(io);
  cart.sync(io);
  io.raw(ram, sizeof ram);
  io.sync(cycles); io.sync(dataBus_);
  io.sync(timer_); io.sync(intFlag_); io.sync(ddra_); io.sync(ddrb_); io.sync(outa_); io.sync(outb_);
  io.sync(divider_); io.sync(sub_); io.sync(wrapped_); io.sync(edgePositive_);
  io.sync(switches_);
  if (io.reading()) {
    if (divider_ != 1 && divider_ != 8 && divider_ != 64 && divider_ != 1024) divider_ = 1024;
    if (sub_ >= divider_) sub_ = 0;
    cart.clockRate = pal ? 1182298.0 : 1193191.66666667;
  }
}

// ================================================================== núcleo
A2600Core::A2600Core(const std::vector<uint8_t>& rom, const std::vector<uint8_t>*)
    : m_(std::make_unique<Machine>(rom)), fb_(320 * 274, 0xFF000000u) {
  // NTSC ou PAL? Roda 60 quadros (apertando Reset, Select e o botão no 21º) e mede as linhas entre os fins
  // de VSYNC, como o detector do Stella: cada quadro vota de 0 (262 linhas) a 1 (312 linhas).
  for (int f = 0; f < 61; f++) {
    m_->setKeys(f == 20 ? (KEY_START | KEY_SELECT | KEY_A) : 0);
    m_->runFrame();
  }
  m_->setKeys(0);
  m_->pal = m_->tia.palVotes > m_->tia.ntscVotes;
  m_->powerOn();
  setSampleRate(48000);
  convert();
}

void A2600Core::reset() {
  // o "reset" do console é o botão Game Reset; aqui reinicia a máquina como ao ligar
  m_->powerOn();
}

uint32_t A2600Core::paletteRgb(uint8_t idx) const { return (m_->pal ? kPal : kNtsc)[idx & 0x7F]; }

void A2600Core::convert() {
  const uint32_t* pal = m_->pal ? kPal : kNtsc;
  uint32_t lut[128];
  for (int i = 0; i < 128; i++) {
    const uint32_t c = pal[i];
    lut[i] = 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
  }
  const int h = m_->tia.frameHeight;
  const uint8_t* s = m_->tia.screen;
  for (int i = 0; i < 160 * h; i++) {
    const uint32_t c = lut[s[i] & 0x7F];
    fb_[i * 2] = c;
    fb_[i * 2 + 1] = c;
  }
}

void A2600Core::runFrame() {
  m_->runFrame();
  convert();
  mixAudio();
}

void A2600Core::setSampleRate(int rate) {
  rate_ = rate > 0 ? rate : 48000;
  wpos_ = rpos_ = 0;
}

void A2600Core::mixAudio() {
  // amostras nativas: 2 por linha (cor / 114); mistura dos 2 canais como no chip (não linear)
  const double native = (m_->pal ? 3546894.0 : 3579545.0) / 114.0;
  const double r = native / rate_;
  for (uint8_t v : m_->tia.audioOut) {
    const double x = (v * (30.0 + 30.0) / (30.0 + v)) / 30.0 * 0.6;
    while (phase_ < 1.0) {
      const double y = prev_ + (x - prev_) * phase_;
      // corta a componente contínua (o som do 2600 é só positivo)
      hp_ = y - hpIn_ + 0.9985 * hp_;
      hpIn_ = y;
      const size_t next = (wpos_ + 1) % kBuf;
      if (next != rpos_) {
        buf_[wpos_] = static_cast<float>(hp_);
        wpos_ = next;
      }
      phase_ += r;
    }
    phase_ -= 1.0;
    prev_ = x;
  }
}

void A2600Core::pullAudio(float* left, float* right, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = 0;
    if (rpos_ != wpos_) { v = buf_[rpos_]; rpos_ = (rpos_ + 1) % kBuf; }
    left[i] = right[i] = v;
  }
}

std::vector<uint8_t> A2600Core::saveState() {
  StateIO io;
  uint32_t magic = 0x36325641;  // "AV26"
  io.sync(magic);
  uint8_t pal = m_->pal;
  io.sync(pal);
  m_->sync(io);
  io.sync(phase_); io.sync(prev_); io.sync(hp_); io.sync(hpIn_);
  return io.data();
}

void A2600Core::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint32_t magic = 0;
  io.sync(magic);
  if (magic != 0x36325641) throw std::runtime_error("Este estado não é de Atari 2600.");
  uint8_t pal = 0;
  io.sync(pal);
  if ((pal != 0) != m_->pal) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  // carrega numa cópia: se o arquivo estiver danificado, o jogo continua como estava
  const std::vector<uint8_t> backup = saveState();
  try {
    m_->sync(io);
    io.sync(phase_); io.sync(prev_); io.sync(hp_); io.sync(hpIn_);
    finiteOr(phase_, 0); finiteOr(prev_, 0); finiteOr(hp_, 0); finiteOr(hpIn_, 0);
    if (phase_ < 0 || phase_ > 1) phase_ = 0;
    if (std::fabs(prev_) > 2 || std::fabs(hp_) > 4 || std::fabs(hpIn_) > 2) prev_ = hp_ = hpIn_ = 0;
  } catch (...) {
    StateIO restore(backup);
    uint32_t m2 = 0;
    uint8_t p2 = 0;
    restore.sync(m2);
    restore.sync(p2);
    m_->sync(restore);
    restore.sync(phase_); restore.sync(prev_); restore.sync(hp_); restore.sync(hpIn_);
    throw;
  }
  convert();
}

}  // namespace indigo::a2600
