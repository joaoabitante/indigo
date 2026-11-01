// Atari Lynx: mapa de memória, laço principal, teclas, save state e a interface Core.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "lynx.h"

namespace indigo::lynx {

namespace {
const char STATE_MAGIC[4] = {'I', 'L', 'Y', '1'};
}

// ---------------------------------------------------------------- barramento da CPU
// Tempo: 16 tiques por µs. Um acesso na mesma página de 256 bytes do anterior usa o modo página da DRAM
// (4 tiques); fora dela, 5 tiques (média de ~3,6 MHz, como no aparelho).
uint8_t Lynx::read(uint16_t a) {
  const uint16_t page = a >> 8;
  now += page == lastPage ? 4 : 5;
  lastPage = page;
  if (a < 0xFC00) return ram[a];
  if (a < 0xFD00) return (mapctl & 1) ? ram[a] : suzyRead(static_cast<uint8_t>(a));
  if (a < 0xFE00) return (mapctl & 2) ? ram[a] : mikeyRead(static_cast<uint8_t>(a));
  if (a < 0xFFF8) return (mapctl & 4) ? ram[a] : rom[a - 0xFE00];
  if (a == 0xFFF8) return ram[a];
  if (a == 0xFFF9) return mapctl;
  return (mapctl & 8) ? ram[a] : rom[a - 0xFE00];
}

void Lynx::write(uint16_t a, uint8_t v) {
  const uint16_t page = a >> 8;
  now += page == lastPage ? 4 : 5;
  lastPage = page;
  if (a >= 0xFC00 && a < 0xFD00 && !(mapctl & 1)) { suzyWrite(static_cast<uint8_t>(a), v); return; }
  if (a >= 0xFD00 && a < 0xFE00 && !(mapctl & 2)) { mikeyWrite(static_cast<uint8_t>(a), v); return; }
  if (a == 0xFFF9) { mapctl = v; return; }
  ram[a] = v;  // a ROM só é lida: o que se grava ali vai para a RAM por baixo
}

// ---------------------------------------------------------------- porta do cartucho
void Lynx::cartStrobe(bool s) {
  if (s) cart.counter = 0;
  if (s && !cart.strobe) cart.shifter = static_cast<uint8_t>((cart.shifter << 1) | (cart.addrData ? 1 : 0));
  cart.strobe = s;
  eeprom.counterChanged(cart.counter, iodir, iodat);
}

uint8_t Lynx::cartRead(int b) {
  const bool useA = cart.audin && (iodat & 0x10);
  const uint8_t v = cart.read(b, useA);
  eeprom.counterChanged(cart.counter, iodir, iodat);
  return v;
}

void Lynx::cartWrite(int b, uint8_t v) {
  cart.write(b, cart.audin && (iodat & 0x10), v);
  eeprom.counterChanged(cart.counter, iodir, iodat);
}

// ---------------------------------------------------------------- reset
void Lynx::reset() {
  std::memset(ram, 0, sizeof ram);
  mapctl = 0;
  lastPage = 0xFFFF;
  sleeping = false;
  sleepUntil = suzyBusyUntil = 0;
  for (auto& t : tim) t = Timer{};
  for (auto& a : aud) a = Audio{};
  for (auto& v : atten) v = 0xFF;
  pan = stereo = 0;
  intStatus = 0;
  dispctl = 0; pbkup = 0; dispadr = 0; lynxAddr = 0;
  lynxLine = 0; dmaLines = 0; curRow = 0;
  std::memset(green, 0, sizeof green);
  std::memset(bluered, 0, sizeof bluered);
  iodir = iodat = sysctl1 = 0;
  rest = false;
  txIrqEn = rxIrqEn = parEn = parEven = sendBreak = false;
  rxReady = overrun = framing = false;
  txCountdown = rxCountdown = -1;
  txData = rxData = rxPending = 0;
  rxHasPending = false;
  std::memset(lcd, 0, sizeof lcd);
  curL = curR = 0;
  accL = accR = 0;
  audioTick = now;
  std::memset(sreg, 0, sizeof sreg);
  sw(0x28) = 0x007F;  // HSIZOFF
  sw(0x2A) = 0x007F;  // VSIZOFF
  std::memset(mathABCD, 0xFF, 4); std::memset(mathEFGH, 0xFF, 4); std::memset(mathJKLM, 0xFF, 4); std::memset(mathNP, 0xFF, 2);
  signAB = signCD = 1;
  sprctl0 = sprctl1 = sprcoll = sprinit = 0;
  suzyBusEn = sprGo = everOn = false;
  stopOnCurrent = leftHand = vStretch = noCollide = accumulate = signedMath = lastCarry = mathBit = unsafe = false;
  for (int i = 0; i < 16; i++) penIndex[i] = static_cast<uint8_t>(i);
  cart.counter = 0; cart.shifter = 0; cart.addrData = false; cart.strobe = false;
  eeprom.reset();
  frameEnd = now;
  if (realBoot) {
    cpu.s = 0xFF;
    cpu.p = 0x24;
    cpu.reset();
  } else {
    hleBoot();
  }
  scheduleNext();
}

void Lynx::setKeys(uint32_t m) {
  keys = m;
  bool up = m & KEY_UP, down = m & KEY_DOWN, left = m & KEY_LEFT, right = m & KEY_RIGHT;
  if (cart.rotation == 1) {  // tela girada para a esquerda: o cima da tela é a esquerda do Lynx
    const bool u = up, d = down, l = left, r = right;
    left = u; right = d; down = l; up = r;
  } else if (cart.rotation == 2) {
    const bool u = up, d = down, l = left, r = right;
    right = u; left = d; up = l; down = r;
  }
  joystick = static_cast<uint8_t>((up ? 0x80 : 0) | (down ? 0x40 : 0) | (left ? 0x20 : 0) | (right ? 0x10 : 0) |
                                  ((m & KEY_L) ? 0x08 : 0) | ((m & KEY_R) ? 0x04 : 0) | ((m & KEY_B) ? 0x02 : 0) |
                                  ((m & KEY_A) ? 0x01 : 0));
  switches = (m & KEY_START) ? 0x01 : 0x00;
}

// ---------------------------------------------------------------- laço principal
void Lynx::runFrame() {
  frameEnd += TICKS_PER_FRAME;
  if (frameEnd < now) frameEnd = now + TICKS_PER_FRAME;
  scheduleNext();
  using C = Cpu65SC02<Lynx>;
  while (now < frameEnd) {
    if (now >= nextEvent) updateTimers();
    if (sleeping) {
      if (now >= sleepUntil) {
        sleeping = false;
      } else if (irqLine() && !(cpu.p & C::I)) {
        sleeping = false;  // a interrupção acorda a CPU (o laço do programa confere a Suzy de novo)
      } else {
        uint64_t to = std::min(sleepUntil, std::min(nextEvent, frameEnd));
        if (to <= now) to = now + 1;
        now = to;
        continue;
      }
    }
    if (irqLine() && !(cpu.p & C::I)) {
      cpu.irq();
      continue;
    }
    if (!realBoot && cpu.pc >= 0xFE00 && cpu.pc < 0xFFF8 && !(mapctl & 4)) {
      hleCall(cpu.pc);
      continue;
    }
    cpu.step();
  }
  audioAdvance(now);
}

// ---------------------------------------------------------------- save state
void Lynx::sync(StateIO& io) {
  io.raw(ram, sizeof ram);
  io.sync(mapctl); io.sync(lastPage);
  cpu.sync(io);
  io.sync(now); io.sync(frameEnd); io.sync(sleepUntil); io.sync(suzyBusyUntil); io.sync(sleeping);
  for (auto& t : tim) {
    io.sync(t.bkup); io.sync(t.cnt); io.sync(t.linking); io.sync(t.irqEn); io.sync(t.reload); io.sync(t.count);
    io.sync(t.done); io.sync(t.lastClock); io.sync(t.borrowIn); io.sync(t.borrowOut); io.sync(t.next);
  }
  for (auto& a : aud) { io.sync(a.vol); io.sync(a.out); io.sync(a.lfsr); io.sync(a.taps); io.sync(a.integrate); }
  io.raw(atten, 4); io.sync(pan); io.sync(stereo); io.sync(intStatus);
  io.sync(dispctl); io.sync(pbkup); io.sync(dispadr); io.sync(lynxAddr); io.sync(lynxLine); io.sync(dmaLines); io.sync(curRow);
  io.raw(green, 16); io.raw(bluered, 16);
  io.sync(iodir); io.sync(iodat); io.sync(sysctl1); io.sync(rest);
  io.sync(txIrqEn); io.sync(rxIrqEn); io.sync(parEn); io.sync(parEven); io.sync(sendBreak);
  io.sync(rxReady); io.sync(overrun); io.sync(framing); io.sync(txCountdown); io.sync(rxCountdown);
  io.sync(txData); io.sync(rxData); io.sync(rxPending); io.sync(rxHasPending);
  io.raw(lcd, sizeof lcd);
  io.sync(curL); io.sync(curR); io.sync(audioTick); io.sync(sampleTick); io.sync(accL); io.sync(accR); io.sync(hpL); io.sync(hpR);
  io.raw(sreg, sizeof sreg);
  io.raw(mathABCD, 4); io.raw(mathEFGH, 4); io.raw(mathJKLM, 4); io.raw(mathNP, 2);
  io.sync(signAB); io.sync(signCD);
  io.sync(sprctl0); io.sync(sprctl1); io.sync(sprcoll); io.sync(sprinit);
  io.sync(suzyBusEn); io.sync(sprGo); io.sync(everOn);
  io.sync(stopOnCurrent); io.sync(leftHand); io.sync(vStretch); io.sync(noCollide); io.sync(accumulate);
  io.sync(signedMath); io.sync(lastCarry); io.sync(mathBit); io.sync(unsafe);
  io.raw(penIndex, 16);
  io.sync(cart.counter); io.sync(cart.shifter); io.sync(cart.addrData); io.sync(cart.strobe);
  eeprom.sync(io);
  if (io.reading()) {
    for (auto& t : tim) t.linking &= 7;
    for (auto& a : aud) { a.lfsr &= 0xFFF; a.taps &= 0xCBF; }
    for (auto& p : penIndex) p &= 15;
    for (int i = 0; i < 16; i++) green[i] &= 15;
    lynxLine = std::clamp(lynxLine, 0, 255);
    dmaLines = std::clamp(dmaLines, 0, SCREEN_H);
    curRow = std::clamp(curRow, 0, SCREEN_H - 1);
    txCountdown = std::clamp(txCountdown, -1, 88);
    rxCountdown = std::clamp(rxCountdown, -1, 88);
    cart.counter &= 0x7FF;
    if (signAB != -1) signAB = 1;
    if (signCD != -1) signCD = 1;
    finiteOr(sampleTick, 0); finiteOr(accL, 0); finiteOr(accR, 0); finiteOr(hpL, 0); finiteOr(hpR, 0);
    if (sampleTick < 0 || sampleTick > ticksPerSample) sampleTick = 0;
    // instantes muito distantes de 'now' (estado danificado) voltam para perto dele
    auto near = [&](uint64_t& t) { if (t > now + (1ull << 32)) t = now; };
    for (auto& t : tim) near(t.next);
    near(frameEnd); near(sleepUntil); near(suzyBusyUntil);
    if (audioTick > now || audioTick + (1ull << 32) < now) audioTick = now;
  }
}

std::vector<uint8_t> Lynx::saveState() {
  StateIO io;
  uint8_t magic[4];
  std::memcpy(magic, STATE_MAGIC, 4);
  io.raw(magic, 4);
  sync(io);
  return io.data();
}

void Lynx::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint8_t magic[4];
  io.raw(magic, 4);
  if (std::memcmp(magic, STATE_MAGIC, 4) != 0) throw std::runtime_error("Estado salvo não é do Lynx.");
  // carrega numa cópia: se o arquivo estiver danificado, a máquina continua como estava
  std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
  } catch (...) {
    StateIO back(backup);
    back.raw(magic, 4);
    sync(back);
    throw;
  }
  presentFrame();
  scheduleNext();
}

// ---------------------------------------------------------------- Core
LynxCore::LynxCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) {
  if (!isLynxRom(rom, "")) {
    // .lyx sem cabeçalho: aceito pelo tamanho
    if (rom.size() < 256) throw std::runtime_error("Arquivo do Lynx inválido.");
  }
  m_ = std::make_unique<Lynx>(rom, save);
}

std::vector<uint8_t> LynxCore::exportSave() {
  Lynx& l = *m_;
  l.eeprom.dirty = false;
  return std::vector<uint8_t>(l.eeprom.data.begin(), l.eeprom.data.begin() + static_cast<long>(l.eeprom.size()));
}

void LynxCore::importSave(const std::vector<uint8_t>& b) {
  Lynx& l = *m_;
  if (!l.eeprom.present()) return;
  std::fill(l.eeprom.data.begin(), l.eeprom.data.end(), 0xFF);
  std::copy(b.begin(), b.begin() + static_cast<long>(std::min(b.size(), l.eeprom.data.size())), l.eeprom.data.begin());
  l.eeprom.dirty = false;
}

std::string LynxCore::note() const {
  const Lynx& l = *m_;
  std::string n;
  if (l.cart.rotation == 1) n = "Jogo em pé (tela girada para a esquerda): a imagem já sai girada e o direcional acompanha.";
  else if (l.cart.rotation == 2) n = "Jogo em pé (tela girada para a direita): a imagem já sai girada e o direcional acompanha.";
  if (!l.realBoot && !l.homebrew) {
    if (!n.empty()) n += " ";
    n += "Partida sem a ROM do Lynx (lynxboot.img): o Índigo imita a ROM.";
  }
  return n;
}

}  // namespace indigo::lynx
