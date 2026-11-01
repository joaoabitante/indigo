// Console GBA (porte de src/gba.js): liga CPU, memória, vídeo, som e o escalonador por scanline.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "gba.h"

namespace indigo::gba {

namespace {
constexpr int HDRAW = 960, HBLANK = 272, LINES = 228;
const char STATE_MAGIC[4] = {'I', 'G', 'B', '1'};
const uint8_t RTC_MAGIC[4] = {0x52, 0x54, 0x43, 0x31};  // "RTC1": ajuste do relógio guardado depois do save

// String.fromCharCode de bytes (Latin-1) para UTF-8
void appendLatin1(std::string& s, uint8_t c) {
  if (c < 0x80) s += static_cast<char>(c);
  else { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
}
bool jsSpace(uint8_t c) { return (c >= 9 && c <= 13) || c == 32 || c == 0xA0; }
}  // namespace

GBA::GBA() : bus(*this), io(*this), cpu(*this), ppu(*this), apu(*this), hle(*this) {
  installHleBios(bus);
}

void GBA::loadBios(const std::vector<uint8_t>& data) {
  std::fill(bus.bios.begin(), bus.bios.end(), 0);
  std::copy_n(data.begin(), std::min<size_t>(data.size(), 0x4000), bus.bios.begin());
  realBios = true;
}

void GBA::loadRom(const std::vector<uint8_t>& data, const std::vector<uint8_t>* saveBytes) {
  bus.loadRom(data);
  bus.save = std::make_unique<Save>(detectSaveType(data));
  if (saveBytes) bus.save->load(*saveBytes);
  // título: bytes não nulos de 0xA0 a 0xAB, sem espaços nas pontas (String.trim)
  size_t a = 0xA0, b = std::min<size_t>(0xAC, data.size());
  std::vector<uint8_t> raw;
  for (size_t k = a; k < b; k++) if (data[k]) raw.push_back(data[k]);
  size_t s = 0, e = raw.size();
  while (s < e && jsSpace(raw[s])) s++;
  while (e > s && jsSpace(raw[e - 1])) e--;
  title.clear();
  for (size_t k = s; k < e; k++) appendLatin1(title, raw[k]);
  gameCode.clear();
  for (size_t k = 0xAC; k < 0xB0; k++) {
    const uint8_t c = k < data.size() && data[k] ? data[k] : 32;
    appendLatin1(gameCode, c);
  }
  if (hasRtc(data, gameCode)) bus.gpio = std::make_unique<CartRtc>();
  else bus.gpio.reset();
  romLoaded = true;
  reset();
}

void GBA::reset() {
  io.reset();
  apu.reset();
  std::fill(bus.ewram.begin(), bus.ewram.end(), 0); std::fill(bus.iwram.begin(), bus.iwram.end(), 0);
  std::fill(bus.pal.begin(), bus.pal.end(), 0); std::fill(bus.vram.begin(), bus.vram.end(), 0);
  std::fill(bus.oam.begin(), bus.oam.end(), 0);
  bus.setWaitcnt(0);
  if (!realBios) installHleBios(bus);
  cpu.reset(!realBios);
  cpu.hle = !realBios;
  if (!realBios) {
    io.regs[0] = 0x0080;
    io.write16(0, 0);
  }
  totalBase = 0; chunkCycles = 0;
  frame = 0;
  ppu.vblankLatch();
}

int64_t GBA::run(int64_t cycles) {
  int64_t done = 0;
  chunkCycles = 0;
  while (done < cycles) {
    if (io.ime && (io.ie & io.ifr & 0x3FFF) && !cpu.i) cpu.irq();
    if (cpu.halted) {
      if (io.ie & io.ifr & 0x3FFF) { cpu.halted = false; continue; }
      int64_t skip = cycles - done;
      if (io.timersActive) {
        const int64_t t = io.cyclesToTimerEvent();
        if (t > 0 && t < skip) skip = t;
        if (skip < 1) skip = 1;
        io.tickTimers(skip);
      }
      done += skip; chunkCycles = done;
      continue;
    }
    bus.cycles = 0; io.stepTicked = 0;
    if (traceHook) traceHook(*this);
    const int extra = cpu.step();
    const int64_t c = bus.cycles + extra;
    if (io.timersActive) io.tickTimers(c - io.stepTicked);
    bus.cycles = 0; io.stepTicked = 0;
    done += c; chunkCycles = done;
  }
  totalBase += done;
  chunkCycles = 0;
  return done - cycles;  // excedente
}

void GBA::runFrame() {
  int64_t over = 0;
  for (int line = 0; line < LINES; line++) {
    io.vcount = line;
    int ds = io.dispstat & ~0x7;
    if (line >= 160 && line < 227) ds |= 1;
    const int lyc = io.dispstat >> 8;
    if (line == lyc) { ds |= 4; if (io.dispstat & 0x20) io.requestIrq(IRQ_VCOUNT); }
    io.dispstat = ds;
    if (line == 160) {
      if (ds & 0x8) io.requestIrq(IRQ_VBLANK);
      bus.cycles = 0; io.stepTicked = 0;
      io.triggerDma(1);
      if (io.timersActive) io.tickTimers(bus.cycles - io.stepTicked);
      over += bus.cycles; bus.cycles = 0; io.stepTicked = 0;
      ppu.vblankLatch();
    }
    over = run(HDRAW - over);
    io.dispstat |= 2;
    if (line < 160) {
      ppu.renderLine(line);
      bus.cycles = 0; io.stepTicked = 0;
      io.triggerDma(2);
      if (io.timersActive) io.tickTimers(bus.cycles - io.stepTicked);
      over += bus.cycles; bus.cycles = 0; io.stepTicked = 0;
    }
    if (io.dispstat & 0x10) io.requestIrq(IRQ_HBLANK);
    over = run(HBLANK - over);
    io.dispstat &= ~2;
  }
  apu.catchUp();
  frame++;
}

// ------------------------------------------------------------ save do cartucho (formato da versão web)
std::vector<uint8_t> GBA::exportSave() {
  Save& s = *bus.save;
  s.dirty = false;
  CartRtc* rtc = bus.gpio.get();
  if (!rtc || rtc->offset == 0 || rtc->offset != rtc->offset) return s.data;
  std::vector<uint8_t> out(s.data.size() + 16, 0);
  std::copy(s.data.begin(), s.data.end(), out.begin());
  const size_t n = s.data.size();
  std::copy(RTC_MAGIC, RTC_MAGIC + 4, out.begin() + static_cast<ptrdiff_t>(n));
  uint64_t bits;
  std::memcpy(&bits, &rtc->offset, 8);
  for (int k = 0; k < 8; k++) out[n + 4 + k] = static_cast<uint8_t>(bits >> (8 * k));
  out[n + 12] = static_cast<uint8_t>(rtc->status);
  return out;
}

void GBA::importSave(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return;
  bus.save->load(bytes);
  const size_t n = bus.save->data.size();
  CartRtc* rtc = bus.gpio.get();
  if (rtc && bytes.size() >= n + 16 && std::equal(RTC_MAGIC, RTC_MAGIC + 4, bytes.begin() + static_cast<ptrdiff_t>(n))) {
    uint64_t bits = 0;
    for (int k = 0; k < 8; k++) bits |= uint64_t(bytes[n + 4 + k]) << (8 * k);
    double off;
    std::memcpy(&off, &bits, 8);
    finiteOr(off, 0);  // "|| 0" do JS (NaN vira 0)
    clampTo(off, -1.0e13, 1.0e13);
    rtc->offset = off;
    rtc->status = bytes[n + 12] ? bytes[n + 12] : 0x40;
  }
}

// ------------------------------------------------------------ save state
void GBA::sync(StateIO& s) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  s.raw(magic, 4);
  bool rb = realBios, hasGpio = bus.gpio != nullptr;
  s.sync(rb); s.sync(hasGpio);
  if (s.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || rb != realBios || hasGpio != (bus.gpio != nullptr)))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  s.sync(totalBase); s.sync(chunkCycles); s.sync(frame);
  if (s.reading()) {
    clampTo<int64_t>(totalBase, 0, int64_t(1) << 52);
    clampTo<int64_t>(chunkCycles, 0, 1 << 20);
  }
  cpu.sync(s);
  bus.sync(s);
  io.sync(s);
  if (s.reading()) {
    // contadores de uma única instrução (entre quadros valem 0): valores enormes travariam os timers
    clampTo<int64_t>(bus.cycles, 0, 1 << 20);
    clampTo<int64_t>(io.stepTicked, 0, bus.cycles);
  }
  ppu.sync(s);
  apu.sync(s);  // depois do relógio (totalBase e bus.cycles)
  s.sync(hle.waiting);
  bus.save->sync(s);
  if (bus.gpio) bus.gpio->sync(s);
}

std::vector<uint8_t> GBA::saveState() {
  StateIO s;
  sync(s);
  return s.data();
}

void GBA::loadState(const std::vector<uint8_t>& st) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO s(st);
    sync(s);
  } catch (...) {
    StateIO s(backup);
    sync(s);
    throw;
  }
}

// ------------------------------------------------------------ adaptador
GBACore::GBACore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios)
    : g_(std::make_unique<GBA>()) {
  if (bios && bios->size() >= 0x4000) g_->loadBios(*bios);
  g_->loadRom(rom, nullptr);
  if (save) g_->importSave(*save);  // como a versão web: o save entra depois, com o ajuste do relógio
}

}  // namespace indigo::gba
