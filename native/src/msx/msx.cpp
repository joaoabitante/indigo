#include "msx.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "cbios.h"

namespace indigo::msx {

namespace {
constexpr int LINES = 262, CYCLES_PER_LINE = 228;
const char STATE_MAGIC[4] = {'I', 'M', 'X', '1'};

struct EmptyPage {
  uint8_t b[0x2000];
  EmptyPage() { std::fill(b, b + sizeof b, 0xFF); }
};
const EmptyPage EMPTY;

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

bool findSeq(const std::vector<uint8_t>& r, const uint8_t* seq, size_t n) {
  if (r.size() < n) return false;
  for (size_t i = 0; i + n <= r.size(); i++)
    if (r[i] == seq[0] && std::memcmp(&r[i], seq, n) == 0) return true;
  return false;
}
}  // namespace

bool isMsxRom(const std::vector<uint8_t>& rom, const std::string& name) {
  if (endsWith(name, ".mx1") || endsWith(name, ".mx2")) return true;
  if (!endsWith(name, ".rom")) return false;
  for (size_t off : {size_t(0), size_t(0x4000)})
    if (off + 16 <= rom.size() && rom[off] == 'A' && rom[off + 1] == 'B') return true;
  return false;
}

int msxMachineFor(const std::vector<uint8_t>& rom, const std::string& name) {
  if (endsWith(name, ".mx1")) return 1;
  if (endsWith(name, ".mx2")) return 2;
  static const uint8_t OUT9A[] = {0xD3, 0x9A}, OUT9B[] = {0xD3, 0x9B}, LDC9A[] = {0x0E, 0x9A},
                       LDC9B[] = {0x0E, 0x9B}, EXTROM[] = {0xCD, 0x5F, 0x01}, VER[] = {0x3A, 0x2D, 0x00};
  if (findSeq(rom, OUT9A, 2) || findSeq(rom, OUT9B, 2) || findSeq(rom, LDC9A, 2) || findSeq(rom, LDC9B, 2) ||
      findSeq(rom, EXTROM, 3) || findSeq(rom, VER, 3))
    return 2;
  return 1;
}

// ------------------------------------------------------------ máquina
MSX::MSX(const std::vector<uint8_t>& rom, bool isMsx2)
    : msx2(isMsx2), cpu(*this), vdp(isMsx2), cart(rom) {
  const std::vector<uint8_t> fw = firmware(msx2 ? "msx2" : "msx");
  if (!msx2 && fw.size() == 0x8000) {
    mainRom = fw; userBios = true;
  } else if (msx2 && fw.size() == 0xC000) {
    mainRom.assign(fw.begin(), fw.begin() + 0x8000);
    subRom.assign(fw.begin() + 0x8000, fw.end());
    userBios = true;
  } else if (msx2 && fw.size() == 0x8000 && firmware("msx2sub").size() == 0x4000) {
    mainRom = fw; subRom = firmware("msx2sub"); userBios = true;
  }
  if (!userBios) {
    if (msx2) {
      mainRom.assign(CBIOS_MSX2_MAIN, CBIOS_MSX2_MAIN + 0x8000);
      logoRom.assign(CBIOS_MSX2_LOGO, CBIOS_MSX2_LOGO + 0x4000);
      subRom.assign(CBIOS_MSX2_SUB, CBIOS_MSX2_SUB + 0x4000);
    } else {
      mainRom.assign(CBIOS_MSX1_MAIN, CBIOS_MSX1_MAIN + 0x8000);
      logoRom.assign(CBIOS_MSX1_LOGO, CBIOS_MSX1_LOGO + 0x4000);
    }
  }
  segments = msx2 ? 16 : 4;
  ram.assign(static_cast<size_t>(segments) * 0x4000, 0);
  reset();
}

void MSX::reset() {
  cpu.reset();
  vdp.reset();
  psg.reset();
  mixer.reset();
  cart.reset();
  std::fill(ram.begin(), ram.end(), 0);
  psl = 0; ssl3 = 0; ppiC = 0;
  for (int p = 0; p < 4; p++) mapReg[p] = 3 - p;
  std::memset(rtcReg, 0, sizeof rtcReg);
  rtcAddr = 0; rtcMode = 0;
  cycles = 0; audioCycle = 0; lineCycle = 0; over = 0;
  frame = 0;
  setKeys(keys);
  remap();
}

void MSX::remap() {
  for (int r = 0; r < 8; r++) {
    const int page = r >> 1, prim = (psl >> (page * 2)) & 3;
    slotOf[r] = prim;
    wr[r] = nullptr;
    rd[r] = EMPTY.b;
    switch (prim) {
      case 0:
        if (page < 2) rd[r] = mainRom.data() + r * 0x2000;
        else if (page == 2 && !logoRom.empty()) rd[r] = logoRom.data() + (r & 1) * 0x2000;
        break;
      case 1: rd[r] = cart.readPtr(r); break;
      case 2: break;
      default:
        if (!msx2) {
          rd[r] = wr[r] = ram.data() + r * 0x2000;
        } else {
          const int sub = (ssl3 >> (page * 2)) & 3;
          if (sub == 0 && page == 0) rd[r] = subRom.data() + r * 0x2000;
          else if (sub == 2) {
            const size_t off = static_cast<size_t>(mapReg[page] & (segments - 1)) * 0x4000 + (r & 1) * 0x2000;
            rd[r] = wr[r] = ram.data() + off;
          }
        }
        break;
    }
  }
}

int MSX::read(int a) {
  if (a == 0xFFFF && msx2 && slotOf[7] == 3) return (~ssl3) & 0xFF;
  const uint8_t* p = rd[a >> 13];
  if (p) return p[a & 0x1FFF];
  return cart.read(a);  // só o cartucho tem caminho lento (SCC)
}

void MSX::write(int a, int v) {
  const int r = a >> 13;
  if (a == 0xFFFF && msx2 && slotOf[7] == 3) { ssl3 = v & 0xFF; remap(); return; }
  if (wr[r]) { wr[r][a & 0x1FFF] = static_cast<uint8_t>(v); return; }
  if (slotOf[r] == 1) {
    if (cart.hasScc() && a >= 0x9800 && a < 0xA000) audioTo(cycles + lineCycle);
    if (cart.write(a, v)) remap();
  }
}

int MSX::joystick() const {
  if (psg.portB() & 0x40) return 0x7F;  // porta 2: nada ligado
  int v = 0x7F;
  if (keys & KEY_UP) v &= ~0x01;
  if (keys & KEY_DOWN) v &= ~0x02;
  if (keys & KEY_LEFT) v &= ~0x04;
  if (keys & KEY_RIGHT) v &= ~0x08;
  if (keys & KEY_A) v &= ~0x10;
  if (keys & KEY_B) v &= ~0x20;
  return v;
}

int MSX::in(int port) {
  const int p = port & 0xFF;
  switch (p) {
    case 0x98: return vdp.readData();
    case 0x99: { const int v = vdp.readStatus(lineCycle); cpu.irqLine = vdp.irq(); return v; }
    case 0xA2: return psg.readData(joystick());
    case 0xA8: return psl;
    case 0xA9: { const int row = ppiC & 15; return row < 11 ? matrix[row] : 0xFF; }
    case 0xAA: return ppiC;
    case 0xB5:
      if (!msx2) return 0xFF;
      if (rtcAddr < 13) return rtcReg[rtcMode & 3][rtcAddr] | 0xF0;
      if (rtcAddr == 13) return rtcMode | 0xF0;
      return 0xFF;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      if (!msx2) return 0xFF;
      return (mapReg[p - 0xFC] | ~(segments - 1)) & 0xFF;
    default: return 0xFF;
  }
}

void MSX::out(int port, int v) {
  const int p = port & 0xFF;
  v &= 0xFF;
  switch (p) {
    case 0x98: vdp.writeData(v); return;
    case 0x99: vdp.writeControl(v); cpu.irqLine = vdp.irq(); return;
    case 0x9A: vdp.writePalette(v); return;
    case 0x9B: vdp.writeIndirect(v); cpu.irqLine = vdp.irq(); return;
    case 0xA0: psg.writeAddr(v); return;
    case 0xA1: audioTo(cycles + lineCycle); psg.writeData(v); return;
    case 0xA8: psl = v; remap(); return;
    case 0xAA: audioTo(cycles + lineCycle); ppiC = v; return;
    case 0xAB:
      if (!(v & 0x80)) {  // liga/desliga um bit da porta C
        audioTo(cycles + lineCycle);
        const int bit = (v >> 1) & 7;
        ppiC = (v & 1) ? (ppiC | (1 << bit)) : (ppiC & ~(1 << bit));
      }
      return;
    case 0xB4: if (msx2) rtcAddr = v & 15; return;
    case 0xB5:
      if (!msx2) return;
      if (rtcAddr < 13) rtcReg[rtcMode & 3][rtcAddr] = static_cast<uint8_t>(v & 15);
      else if (rtcAddr == 13) rtcMode = v & 15;
      return;
    case 0xFC: case 0xFD: case 0xFE: case 0xFF:
      if (!msx2) return;
      mapReg[p - 0xFC] = v;
      remap();
      return;
    default: return;
  }
}

// teclado: matriz de 11 linhas × 8 bits (0 = apertada), layout internacional
void MSX::setKeys(uint32_t mask) {
  keys = mask;
  std::memset(matrix, 0xFF, sizeof matrix);
  auto press = [&](int row, int bit) { matrix[row] &= static_cast<uint8_t>(~(1 << bit)); };
  if (mask & KEY_UP) press(8, 5);
  if (mask & KEY_DOWN) press(8, 6);
  if (mask & KEY_LEFT) press(8, 4);
  if (mask & KEY_RIGHT) press(8, 7);
  if (mask & (KEY_A | KEY_START)) press(8, 0);  // espaço
  if (mask & KEY_B) press(4, 2);                // M
  if (mask & KEY_SELECT) press(7, 1);           // F5
  if (mask & KEY_X) press(7, 7);                // RETURN
  if (mask & KEY_Y) press(7, 2);                // ESC
  if (mask & KEY_L) press(6, 5);                // F1
  if (mask & KEY_R) press(6, 6);                // F2
}

// som: PSG + SCC + clique do teclado, a cada 16 ciclos de CPU
void MSX::audioTo(int64_t to) {
  const bool scc = cart.hasScc() && cart.scc.used;
  const float click = (ppiC & 0x80) ? 0.12f : 0.0f;
  while (audioCycle + 16 <= to) {
    audioCycle += 16;
    float v = psg.tick() * 0.22f + click;
    if (scc) v += cart.scc.tick() * 0.16f;
    mixer.push(v);
  }
}

void MSX::runUntil(int target) {
  while (lineCycle < target) {
    const int r0 = cpu.r;
    int t = cpu.step();
    t += (cpu.r - r0) & 0x7F;  // espera do M1: o R conta exatamente as buscas de opcode
    lineCycle += t;
  }
}

void MSX::runFrame() {
  for (int line = 0; line < LINES; line++) {
    vdp.startLine(line);
    cpu.irqLine = vdp.irq();
    lineCycle = over;
    runUntil(VDP::HBLANK_CYCLE);
    vdp.hblank(line);
    cpu.irqLine = vdp.irq();
    runUntil(CYCLES_PER_LINE);
    over = lineCycle - CYCLES_PER_LINE;
    cycles += CYCLES_PER_LINE;
    lineCycle = 0;
    audioTo(cycles);
  }
  vdp.endFrame();
  frame++;
}

// ------------------------------------------------------------ save state
void MSX::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  bool kind = msx2;
  io.sync(kind);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || kind != msx2))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  vdp.sync(io);
  psg.sync(io);
  mixer.sync(io);
  cart.sync(io);
  io.bytes(ram);
  for (int* p : {&psl, &ssl3, &ppiC, &mapReg[0], &mapReg[1], &mapReg[2], &mapReg[3], &rtcAddr, &rtcMode, &over})
    io.sync(*p);
  io.raw(rtcReg, sizeof rtcReg);
  io.sync(cycles); io.sync(audioCycle);
  if (io.reading()) {
    for (int* p : {&psl, &ssl3, &ppiC, &mapReg[0], &mapReg[1], &mapReg[2], &mapReg[3]}) *p &= 0xFF;
    rtcAddr &= 15; rtcMode &= 15;
    for (auto& b : rtcReg) for (auto& x : b) x &= 15;
    clampTo(over, 0, 64);
    clampTo<int64_t>(cycles, 0, int64_t(1) << 60);
    if (audioCycle > cycles || audioCycle < cycles - 4096) audioCycle = cycles;
    lineCycle = 0;
    remap();
  }
}

std::vector<uint8_t> MSX::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void MSX::loadState(const std::vector<uint8_t>& s) {
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

// ------------------------------------------------------------ adaptador
MSXCore::MSXCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, int machine) {
  if (rom.empty()) throw std::runtime_error("ROM de MSX vazia.");
  if (machine != 1 && machine != 2) machine = msxMachineFor(rom, "");
  m_ = std::make_unique<MSX>(rom, machine == 2);
  if (save) importSave(*save);
}

void MSXCore::importSave(const std::vector<uint8_t>& b) {
  if (b.empty()) return;
  if (m_->cart.mapper != Mapper::ASCII8 && m_->cart.mapper != Mapper::ASCII16) return;
  auto& s = m_->cart.sram;
  std::copy(b.begin(), b.begin() + std::min(b.size(), s.size()), s.begin());
  m_->cart.sramUsed = true;
}

std::string MSXCore::note() const {
  return "";
}

std::string MSXCore::description() const {
  return std::string(m_->msx2 ? "MSX2" : "MSX1") + ", cartucho " + mapperName(m_->cart.mapper) +
         (m_->userBios ? ", BIOS original" : ", C-BIOS");
}

}  // namespace indigo::msx
