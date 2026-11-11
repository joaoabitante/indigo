// Nintendo DS: montagem do console, partida direta (sem BIOS), agenda de linhas, timers, DMA, divisor/raiz,
// save state e a classe Core.
#include "nds.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace indigo::nds {

namespace {
// BIOS mínima própria (escrita do zero): só os vetores e o despachante de IRQ que chama o tratador do jogo
// (ARM9: endereço em DTCM+0x3FFC; ARM7: em 0x0380FFFC), como a BIOS real faz. As SWI são tratadas em hle.cpp.
const uint32_t BIOS9_CODE[] = {0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE,
                               0xEA000000, 0xEAFFFFFE,
                               0xE92D500F,   // stmfd sp!, {r0-r3, r12, lr}
                               0xEE190F11,   // mrc p15, 0, r0, c9, c1, 0   (base da DTCM)
                               0xE1A00620,   // mov r0, r0, lsr #12
                               0xE1A00600,   // mov r0, r0, lsl #12
                               0xE2800901,   // add r0, r0, #0x4000
                               0xE28FE000,   // add lr, pc, #0
                               0xE510F004,   // ldr pc, [r0, #-4]
                               0xE8BD500F,   // ldmfd sp!, {r0-r3, r12, lr}
                               0xE25EF004,   // subs pc, lr, #4
                               0xEAFFFFFE};  // retorno das rotinas do jogo chamadas pela HLE (0xFFFF0044)
const uint32_t BIOS7_CODE[] = {0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE, 0xEAFFFFFE,
                               0xEA000000, 0xEAFFFFFE,
                               0xE92D500F,   // stmfd sp!, {r0-r3, r12, lr}
                               0xE3A00301,   // mov r0, #0x04000000
                               0xE28FE000,   // add lr, pc, #0
                               0xE510F004,   // ldr pc, [r0, #-4]   (0x03FFFFFC = 0x0380FFFC)
                               0xE8BD500F,   // ldmfd sp!, {r0-r3, r12, lr}
                               0xE25EF004,   // subs pc, lr, #4
                               0xEAFFFFFE};  // retorno (0x38)

inline uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
  return o + 4 <= b.size() ? b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (uint32_t(b[o + 3]) << 24) : 0;
}
inline uint16_t rd16(const std::vector<uint8_t>& b, size_t o) { return o + 2 <= b.size() ? uint16_t(b[o] | (b[o + 1] << 8)) : 0; }

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}
}  // namespace

uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n);

uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n) {
  static const uint16_t val[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int j = 0; j < 8; j++) {
      const bool carry = crc & 1;
      crc >>= 1;
      if (carry) crc ^= static_cast<uint16_t>(val[j] << (7 - j));
    }
  }
  return crc;
}

bool isNdsRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.size() < 0x200) return false;
  // logotipo: CRC 0xCF56 em 0x15C (todos os cartuchos têm) e os endereços ARM9/ARM7 dentro da RAM
  // sem a extensão, o CRC16 do cabeçalho (0x15E) também precisa bater: dois bytes só não bastam para não roubar
  // arquivos de outros consoles
  const bool logo = rom[0x15C] == 0x56 && rom[0x15D] == 0xCF &&
                    crc16(0xFFFF, rom.data(), 0x15E) == uint16_t(rom[0x15E] | (rom[0x15F] << 8));
  const bool ext = n.size() >= 4 && n.compare(n.size() - 4, 4, ".nds") == 0;
  if (!logo && !ext) return false;
  const uint32_t a9off = rd32(rom, 0x20), a9size = rd32(rom, 0x2C), a7off = rd32(rom, 0x30), a7size = rd32(rom, 0x3C);
  if (!logo && (a9size == 0 || a9size > 0x3BFE00 || a7size > 0x3BFE00)) return false;
  (void)a9off; (void)a7off;
  return true;
}

std::string ndsBiosKind(const std::vector<uint8_t>& d) {
  if (d.size() == 4096 && crc32(d.data(), d.size()) == 0x2AB23573u) return "nds_bios9";
  if (d.size() == 16384 && crc32(d.data(), d.size()) == 0x1280F0D5u) return "nds_bios7";
  return "";
}

NDS::NDS() {
  mainRam.assign(MAIN_RAM, 0);
  wram.assign(0x8000, 0);
  wram7.assign(0x10000, 0);
  vram.assign(0xA4000, 0);
  pal.assign(0x800, 0);
  oam.assign(0x800, 0);
  itcm.assign(0x8000, 0);
  dtcm.assign(0x4000, 0);
  bios9.assign(0x1000, 0);
  bios7.assign(0x4000, 0);
  wifiRam.assign(0x2000, 0);
  remapVram();
  arm9.costShift = 1;
  arm7.costShift = 1;
}

NDS::~NDS() = default;

void NDS::loadRom(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) {
  if (rom.size() < 0x200) throw std::runtime_error("Arquivo pequeno demais para um cartucho de Nintendo DS.");
  const uint32_t a9off = rd32(rom, 0x20), a9ram = rd32(rom, 0x28), a9size = rd32(rom, 0x2C);
  const uint32_t a7off = rd32(rom, 0x30), a7ram = rd32(rom, 0x38), a7size = rd32(rom, 0x3C);
  if (a9size > 0x3BFE00 || a7size > 0x3BFE00 || uint64_t(a9off) + a9size > rom.size() || uint64_t(a7off) + a7size > rom.size())
    throw std::runtime_error("Cabeçalho do cartucho de Nintendo DS inválido (programas fora do arquivo).");
  (void)a9ram; (void)a7ram;
  cart.load(rom);
  if (save && !save->empty()) cart.setSave(*save);
  titleStr.clear();
  for (int i = 0; i < 12 && rom[i]; i++) titleStr += (rom[i] >= 32 && rom[i] < 127) ? char(rom[i]) : '?';
  while (!titleStr.empty() && titleStr.back() == ' ') titleStr.pop_back();
  gameCode.assign(reinterpret_cast<const char*>(&rom[0x0C]), 4);
  for (char& ch : gameCode) if (ch < 32 || ch > 126) ch = '?';
  spi.buildFirmware();
  loaded = true;
  reset();
}

void NDS::reset() {
  std::fill(mainRam.begin(), mainRam.end(), 0);
  std::fill(wram.begin(), wram.end(), 0);
  std::fill(wram7.begin(), wram7.end(), 0);
  std::fill(vram.begin(), vram.end(), 0);
  std::fill(pal.begin(), pal.end(), 0);
  std::fill(oam.begin(), oam.end(), 0);
  std::fill(itcm.begin(), itcm.end(), 0);
  std::fill(dtcm.begin(), dtcm.end(), 0);
  std::fill(wifiRam.begin(), wifiRam.end(), 0);
  const std::vector<uint8_t> ub9 = firmware("nds_bios9");
  const std::vector<uint8_t> ub7 = firmware("nds_bios7");
  const bool userBios = ub9.size() == 0x1000 && ub7.size() == 0x4000;
  if (userBios) {
    bios9 = ub9;
    bios7 = ub7;
  } else {
    std::fill(bios9.begin(), bios9.end(), 0);
    std::fill(bios7.begin(), bios7.end(), 0);
    for (size_t i = 0; i < sizeof BIOS9_CODE / 4; i++) for (int k = 0; k < 4; k++) bios9[i * 4 + k] = uint8_t(BIOS9_CODE[i] >> (8 * k));
    for (size_t i = 0; i < sizeof BIOS7_CODE / 4; i++) for (int k = 0; k < 4; k++) bios7[i * 4 + k] = uint8_t(BIOS7_CODE[i] >> (8 * k));
  }
  io9 = CpuIo{};
  io7 = CpuIo{};
  io9.hle = io7.hle = !userBios;
  for (auto& x : vramcnt) x = 0;
  wramcnt = 3;
  exmemcnt = 0x6000; exmemstat = 0;
  powcnt1 = 0x820F; powcnt2 = 1;
  postflg9 = postflg7 = 1; haltcnt = 0;
  keys = 0x3FF; extkeys = 0x7F;
  touchDown = false;
  vcount = 0; lineCycle = 0;
  divcnt = sqrtcnt = 0; divNumer = divDenom = divResult = divRem = 0; sqrtParam = 0; sqrtResult = 0;
  biosProt = 0;
  cpCtrl = 0x00052078; dtcmReg = 0x0080000A; itcmReg = 0x00000020;
  for (auto& x : cpRegion) x = 0;
  for (auto& x : cpMisc) x = 0;
  gpuA.reset();
  gpuB.reset();
  gpu3d.reset();
  spu.reset();
  spi.cnt = 0; spi.data = 0; spi.fwCmd = 0; spi.tscCmd = 0; spi.pmPos = 0;
  rtc = Rtc{};
  cart.romctrl = 0; cart.auxspicnt = 0; cart.xferLeft = 0; cart.xferKind = 0;
  cart.spiCmd = 0; cart.spiPos = 0; cart.csHold = false; cart.status = 0; cart.irPos = cart.irCmd = 0;
  remapVram();
  updateTcm();
  arm9.reset();
  arm7.reset();
  now7 = 0; cyc9 = cyc7 = 0;
  frameCount = 0;
  std::memset(fb, 0, sizeof fb);
  if (loaded) directBoot();
  updateTcm();
  mapGen++;
}

// carrega os binários do cabeçalho na memória e deixa o estado que o firmware deixaria ao saltar para o jogo
void NDS::directBoot() {
  const std::vector<uint8_t>& rom = cart.rom;
  const uint32_t a9off = rd32(rom, 0x20), a9entry = rd32(rom, 0x24), a9ram = rd32(rom, 0x28), a9size = rd32(rom, 0x2C);
  const uint32_t a7off = rd32(rom, 0x30), a7entry = rd32(rom, 0x34), a7ram = rd32(rom, 0x38), a7size = rd32(rom, 0x3C);
  // o firmware deixa a memória compartilhada inteira com o ARM7 e o cartucho com o ARM9
  wramcnt = 3;
  for (uint32_t i = 0; i < a9size; i++) bus9.write8(a9ram + i, rom[a9off + i]);
  if (a9off == 0x4000 && a9size >= 8 && std::memcmp(&rom[0x4000], "encryObj", 8) == 0) {
    static const uint8_t UND[4] = {0xFF, 0xDE, 0xFF, 0xE7};  // 0xE7FFDEFF
    for (uint32_t i = 0; i < 8; i++) bus9.write8(a9ram + i, UND[i & 3]);
  }
  for (uint32_t i = 0; i < a7size; i++) bus7.write8(a7ram + i, rom[a7off + i]);
  // cabeçalho em 0x027FFE00 e dados do sistema
  for (uint32_t i = 0; i < 0x170 && i < rom.size(); i++) mainRam[0x3FFE00 + i] = rom[i];
  auto w32 = [&](uint32_t a, uint32_t v) { for (int k = 0; k < 4; k++) mainRam[(a & 0x3FFFFF) + k] = uint8_t(v >> (8 * k)); };
  auto w16 = [&](uint32_t a, uint32_t v) { for (int k = 0; k < 2; k++) mainRam[(a & 0x3FFFFF) + k] = uint8_t(v >> (8 * k)); };
  w32(0x027FF800, cart.chipId); w32(0x027FF804, cart.chipId);
  w16(0x027FF808, rd16(rom, 0x15E)); w16(0x027FF80A, rd16(rom, 0x6C));
  w16(0x027FF850, 0x5835);
  w32(0x027FFC00, cart.chipId); w32(0x027FFC04, cart.chipId);
  w16(0x027FFC08, rd16(rom, 0x15E)); w16(0x027FFC0A, rd16(rom, 0x6C));
  w16(0x027FFC10, 0x5835);
  w16(0x027FFC30, 0xFFFF);
  w16(0x027FFC40, 1);  // partiu do cartucho
  // dados de usuário do firmware (cópia em 0x027FFC80)
  if (spi.firmware.size() >= 0x40000) for (int i = 0; i < 0x70; i++) mainRam[0x3FFC80 + i] = spi.firmware[0x3FE00 + i];

  // CPUs
  arm9.reset();
  arm9.switchMode(M_IRQ); arm9.r[13] = 0x03003F80;
  arm9.switchMode(M_SVC); arm9.r[13] = 0x03003FC0;
  arm9.switchMode(M_SYS); arm9.r[13] = 0x03002F7C;
  arm9.r[12] = arm9.r[14] = a9entry;
  arm9.i = 0; arm9.f = 0;
  arm9.nextPC = a9entry & ~3u;
  arm7.reset();
  arm7.switchMode(M_IRQ); arm7.r[13] = 0x0380FF80;
  arm7.switchMode(M_SVC); arm7.r[13] = 0x0380FFC0;
  arm7.switchMode(M_SYS); arm7.r[13] = 0x0380FD80;
  arm7.r[12] = arm7.r[14] = a7entry;
  arm7.i = 0; arm7.f = 0;
  arm7.nextPC = a7entry & ~3u;
  // o firmware deixa o cartucho em modo de dados (KEY2) e a BIOS do ARM7 protegida
  biosProt = 0x1204;
  powcnt1 = 0x820F;
  // brilho e telas ligadas; o som com a polarização no meio
  spu.bias = 0x200;
  powcnt2 = 1;
}

void NDS::setKeys(uint32_t mask) {
  keys = static_cast<uint16_t>(~mask & 0x3FF);
  extkeys = static_cast<uint16_t>((extkeys & ~3u) | ((mask & KEY_X) ? 0 : 1) | ((mask & KEY_Y) ? 0 : 2));
  // IRQ do teclado (KEYCNT) nas duas CPUs
  for (int c = 0; c < 2; c++) {
    CpuIo& io = cio(c);
    if (!(io.keycnt & 0x4000)) continue;
    const uint16_t sel = io.keycnt & 0x3FF, pressed = static_cast<uint16_t>(~keys & 0x3FF);
    const bool hit = (io.keycnt & 0x8000) ? (pressed & sel) == sel : (pressed & sel) != 0;
    if (hit && sel) io.ifr |= 1u << 12;
  }
  updateIrq();
}

void NDS::setTouch(int x, int y, bool down) {
  touchDown = down && x >= 0 && y >= 0 && x < SCREEN_W && y < SCREEN_H;
  if (touchDown) { touchX = x; touchY = y; }
  extkeys = static_cast<uint16_t>(touchDown ? (extkeys & ~0x40u) : (extkeys | 0x40u));
}

// ================================================================== timers
uint32_t NDS::readTimer(CpuIo& io, int i) const { return io.tm[i].counter & 0xFFFF; }

void NDS::writeTimerCtrl(CpuIo& io, int, int i, uint16_t v) {
  Timer& t = io.tm[i];
  const bool wasOn = t.ctrl & 0x80;
  t.ctrl = v & 0xC7;
  if (!wasOn && (v & 0x80)) { t.counter = t.reload; t.sub = 0; }
}

void NDS::timerOverflow(CpuIo& io, int cpuIdx, int i) {
  Timer& t = io.tm[i];
  t.counter = t.reload;
  if (t.ctrl & 0x40) { io.ifr |= 1u << (3 + i); updateIrq(); }
  (void)cpuIdx;
  if (i < 3) {
    Timer& nx = io.tm[i + 1];
    if ((nx.ctrl & 0x84) == 0x84) {
      nx.counter++;
      if (nx.counter > 0xFFFF) timerOverflow(io, cpuIdx, i + 1);
    }
  }
}

void NDS::tickTimers(CpuIo& io, int c, int cycles) {
  static const int SHIFT[4] = {0, 6, 8, 10};
  for (int i = 0; i < 4; i++) {
    Timer& t = io.tm[i];
    if ((t.ctrl & 0x84) != 0x80) continue;
    t.sub += cycles;
    const int sh = SHIFT[t.ctrl & 3];
    uint32_t inc = t.sub >> sh;
    t.sub &= (1u << sh) - 1;
    while (inc) {
      const uint32_t room = 0x10000 - t.counter;
      if (inc < room) { t.counter += inc; break; }
      inc -= room;
      timerOverflow(io, c, i);
    }
  }
}

// ================================================================== DMA
void NDS::writeDmaCnt(int c, int ch, uint32_t v) {
  Dma& d = cio(c).dma[ch];
  const bool was = d.cnt & 0x80000000u;
  if (c == 1) v &= ch == 3 ? 0xF7E0FFFFu : 0xF7E03FFFu;
  d.cnt = v;
  if (!(v & 0x80000000u)) { d.running = false; return; }
  if (was) return;
  d.src = d.sad;
  d.dst = d.dad;
  uint32_t n = c ? (v & (ch == 3 ? 0xFFFF : 0x3FFF)) : (v & 0x1FFFFF);
  if (n == 0) n = c ? (ch == 3 ? 0x10000 : 0x4000) : 0x200000;
  d.count = n;
  d.running = true;
  const int mode = c ? static_cast<int>((v >> 28) & 3) : static_cast<int>((v >> 27) & 7);
  if (mode == 0) runDma(c, ch);
  else if (c == 0 && mode == 7) runDma(c, ch);  // a FIFO de geometria executa na hora
  else if ((c == 0 && mode == 5) || (c == 1 && mode == 2)) cartDataReady();
}

void NDS::dmaStart(int c, int mode) {
  CpuIo& io = cio(c);
  for (int ch = 0; ch < 4; ch++) {
    const Dma& d = io.dma[ch];
    if (!(d.cnt & 0x80000000u)) continue;
    const int m = c ? static_cast<int>((d.cnt >> 28) & 3) : static_cast<int>((d.cnt >> 27) & 7);
    if (m == mode) runDma(c, ch);
  }
}

void NDS::runDma(int c, int ch) {
  // um DMA que escreve no próprio registro de controle (ou no ROMCTRL, no modo cartucho) se rearma de dentro da
  // cópia e chamaria runDma de novo sem fim: além de poucos níveis, a transferência nova fica para depois
  if (dmaDepth_ >= 4) return;
  struct Nivel { int& n; explicit Nivel(int& x) : n(x) { n++; } ~Nivel() { n--; } } nivel(dmaDepth_);
  CpuIo& io = cio(c);
  Dma& d = io.dma[ch];
  ArmBus& bus = c ? static_cast<ArmBus&>(bus7) : static_cast<ArmBus&>(bus9);
  const bool word = d.cnt & 0x04000000u;
  const int step = word ? 4 : 2;
  const int dctl = (d.cnt >> 21) & 3, sctl = (d.cnt >> 23) & 3;
  const int sstep = sctl == 0 ? step : sctl == 1 ? -step : 0;
  const int dstep = (dctl == 0 || dctl == 3) ? step : dctl == 1 ? -step : 0;
  const int mode = c ? static_cast<int>((d.cnt >> 28) & 3) : static_cast<int>((d.cnt >> 27) & 7);
  uint32_t n = d.count;
  // modo cartucho: a contagem é por palavra pronta (o jogo usa 1 com repetição)
  for (uint32_t k = 0; k < n; k++) {
    if (word) bus.write32(d.dst & ~3u, bus.read32(d.src & ~3u));
    else bus.write16(d.dst & ~1u, bus.read16(d.src & ~1u));
    d.src += sstep;
    d.dst += dstep;
  }
  if (c == 0) cyc9 -= n * 2;
  else cyc7 -= n;
  if ((d.cnt & 0x02000000u) && mode != 0) {
    uint32_t cnt = c ? (d.cnt & (ch == 3 ? 0xFFFF : 0x3FFF)) : (d.cnt & 0x1FFFFF);
    if (cnt == 0) cnt = c ? (ch == 3 ? 0x10000 : 0x4000) : 0x200000;
    d.count = cnt;
    if (dctl == 3) d.dst = d.dad;
  } else {
    d.cnt &= ~0x80000000u;
    d.running = false;
  }
  if (d.cnt & 0x40000000u) {
    io.ifr |= 1u << (8 + ch);
    updateIrq();
  }
}

// a palavra do cartucho está pronta: os DMAs no modo cartucho buscam (o bloco inteiro fica pronto de uma vez)
void NDS::cartDataReady() {
  const int c = (exmemcnt & 0x800) ? 1 : 0;
  const int mode = c ? 2 : 5;
  CpuIo& io = cio(c);
  for (int guard = 0; guard < 0x10000 && cart.xferLeft > 0; guard++) {
    int ch = -1;
    for (int k = 0; k < 4; k++) {
      const Dma& d = io.dma[k];
      const int m = c ? static_cast<int>((d.cnt >> 28) & 3) : static_cast<int>((d.cnt >> 27) & 7);
      if ((d.cnt & 0x80000000u) && m == mode) { ch = k; break; }
    }
    if (ch < 0) return;
    runDma(c, ch);
  }
}

void NDS::cartXferDone() {
  const int c = (exmemcnt & 0x800) ? 1 : 0;
  if (cart.auxspicnt & 0x4000) {
    cio(c).ifr |= 1u << 19;
    updateIrq();
  }
}

// ================================================================== divisor e raiz
void NDS::divide() {
  divcnt &= ~0x4000;
  if (divDenom == 0) divcnt |= 0x4000;
  switch (divcnt & 3) {
    case 0: {
      const int32_t num = static_cast<int32_t>(divNumer), den = static_cast<int32_t>(divDenom);
      if (den == 0) {
        divResult = num < 0 ? 1 : -1;
        divRem = num;
        // no modo de 32 bits a metade alta do resultado sai com o sinal trocado
        divResult = static_cast<int64_t>((uint64_t(uint32_t(divResult)) | (num < 0 ? 0xFFFFFFFF00000000ull : 0)));
      } else if (num == INT32_MIN && den == -1) {
        divResult = int64_t(uint32_t(INT32_MIN)); divRem = 0;
      } else {
        divResult = num / den; divRem = num % den;
      }
      break;
    }
    case 1: case 3: {
      const int64_t num = divNumer;
      const int32_t den = static_cast<int32_t>(divDenom);
      if (den == 0) { divResult = num < 0 ? 1 : -1; divRem = num; }
      else if (num == INT64_MIN && den == -1) { divResult = INT64_MIN; divRem = 0; }
      else { divResult = num / den; divRem = num % den; }
      break;
    }
    default: {
      const int64_t num = divNumer, den = divDenom;
      if (den == 0) { divResult = num < 0 ? 1 : -1; divRem = num; }
      else if (num == INT64_MIN && den == -1) { divResult = INT64_MIN; divRem = 0; }
      else { divResult = num / den; divRem = num % den; }
      break;
    }
  }
}

void NDS::squareRoot() {
  uint64_t v = (sqrtcnt & 1) ? sqrtParam : (sqrtParam & 0xFFFFFFFFull);
  uint64_t res = 0, bit = 1ull << 62;
  while (bit > v) bit >>= 2;
  while (bit) {
    if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; }
    else res >>= 1;
    bit >>= 2;
  }
  sqrtResult = static_cast<uint32_t>(res);
}

// ================================================================== agenda
void NDS::runCycles(int cycles) {
  int done = 0;
  while (done < cycles) {
    const int slice = std::min(64, cycles - done);
    cyc9 += slice * 2;
    if (cyc9 > 0) cyc9 -= arm9.run(static_cast<int>(cyc9), mapGen);
    cyc7 += slice;
    if (cyc7 > 0) cyc7 -= arm7.run(static_cast<int>(cyc7), mapGen);
    tickTimers(io9, 0, slice);
    tickTimers(io7, 1, slice);
    spu.run(slice);
    done += slice;
    lineCycle += slice;
    now7 += slice;
  }
}

void NDS::startLine(int line) {
  vcount = static_cast<uint16_t>(line);
  lineCycle = 0;
  for (int c = 0; c < 2; c++) {
    CpuIo& io = cio(c);
    const int lyc = ((io.dispstat >> 8) & 0xFF) | ((io.dispstat & 0x80) << 1);
    if (line == lyc && (io.dispstat & 0x20)) io.ifr |= 1u << 2;
    if (line == 192 && (io.dispstat & 0x08)) io.ifr |= 1u;
  }
  updateIrq();
  if (line == 192) {
    gpuA.vblank();
    gpuB.vblank();
    gpu3d.vblank();
    dmaStart(0, 1);
    dmaStart(1, 1);
  }
  if (line < 192) dmaStart(0, 3);
}

void NDS::hblank(int line) {
  for (int c = 0; c < 2; c++) {
    CpuIo& io = cio(c);
    if (io.dispstat & 0x10) io.ifr |= 1u << 1;
  }
  updateIrq();
  if (line < 192) {
    gpuA.renderLine(line);
    gpuB.renderLine(line);
    // tela de cima: engine A se POWCNT1.15 = 1
    const bool aTop = powcnt1 & 0x8000;
    for (int e = 0; e < 2; e++) {
      const Gpu2D& g = e == 0 ? gpuA : gpuB;
      const bool top = (e == 0) == aTop;
      uint32_t* out = fb + (top ? 0 : SCREEN_H * SCREEN_W) + line * SCREEN_W;
      const uint16_t mb = g.reg[0x6C / 2];
      const int mode = (mb >> 14) & 3;
      int fac = mb & 0x1F; if (fac > 16) fac = 16;
      for (int x = 0; x < SCREEN_W; x++) {
        const uint16_t c15 = g.line[x];
        int r = (c15 & 0x1F) << 1, gg = ((c15 >> 5) & 0x1F) << 1, b = ((c15 >> 10) & 0x1F) << 1;
        if (mode == 1 && fac) { r += ((63 - r) * fac) >> 4; gg += ((63 - gg) * fac) >> 4; b += ((63 - b) * fac) >> 4; }
        else if (mode == 2 && fac) { r -= (r * fac) >> 4; gg -= (gg * fac) >> 4; b -= (b * fac) >> 4; }
        r = (r << 2) | (r >> 4); gg = (gg << 2) | (gg >> 4); b = (b << 2) | (b >> 4);
        out[x] = 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(gg) << 8) | uint32_t(r);
      }
    }
    dmaStart(0, 2);
  }
}

void NDS::runFrame() {
  if (!loaded) return;
  for (int line = 0; line < LINES; line++) {
    startLine(line);
    runCycles(HBLANK_AT);
    hblank(line);
    runCycles(LINE_CYCLES - HBLANK_AT);
  }
  frameCount++;
}

// ================================================================== save state
void NDS::sync(StateIO& io) {
  uint32_t magic = 0x4E445331;  // "NDS1"
  io.sync(magic);
  if (magic != 0x4E445331) throw std::runtime_error("Estado salvo não é de Nintendo DS.");
  arm9.sync(io); arm7.sync(io);
  io.sync(arm9.forceIrq); io.sync(arm7.forceIrq);
  for (CpuIo* c : {&io9, &io7}) {
    io.sync(c->ie); io.sync(c->ifr); io.sync(c->ime); io.sync(c->dispstat);
    for (auto& t : c->tm) { io.sync(t.reload); io.sync(t.ctrl); io.sync(t.counter); io.sync(t.sub); }
    for (auto& d : c->dma) { io.sync(d.sad); io.sync(d.dad); io.sync(d.cnt); io.sync(d.src); io.sync(d.dst); io.sync(d.count); io.sync(d.running); }
    io.raw(c->dmaFill, sizeof c->dmaFill);
    io.sync(c->ipcSync); io.sync(c->ipcFifoCnt);
    io.raw(c->recv.d, sizeof c->recv.d); io.sync(c->recv.r); io.sync(c->recv.n); io.sync(c->recv.last);
    io.sync(c->keycnt); io.sync(c->hle); io.sync(c->intrWaitArmed);
  }
  io.bytes(mainRam); io.bytes(wram); io.bytes(wram7); io.bytes(vram); io.bytes(pal); io.bytes(oam);
  io.bytes(itcm); io.bytes(dtcm); io.bytes(bios9); io.bytes(bios7); io.bytes(wifiRam);
  io.sync(cpCtrl); io.sync(dtcmReg); io.sync(itcmReg);
  io.raw(cpRegion, sizeof cpRegion); io.raw(cpMisc, sizeof cpMisc);
  io.raw(vramcnt, sizeof vramcnt); io.sync(wramcnt);
  io.sync(exmemcnt); io.sync(exmemstat); io.sync(powcnt1); io.sync(powcnt2);
  io.sync(postflg9); io.sync(postflg7); io.sync(haltcnt);
  io.sync(keys); io.sync(extkeys); io.sync(touchX); io.sync(touchY); io.sync(touchDown);
  io.sync(vcount); io.sync(divcnt); io.sync(sqrtcnt); io.sync(divNumer); io.sync(divDenom); io.sync(divResult);
  io.sync(divRem); io.sync(sqrtParam); io.sync(sqrtResult); io.sync(biosProt);
  io.sync(frameCount); io.sync(now7); io.sync(lineCycle); io.sync(cyc9); io.sync(cyc7);
  gpuA.sync(io); gpuB.sync(io); gpu3d.sync(io); spu.sync(io); cart.sync(io); spi.sync(io); rtc.sync(io);
  // acrescentado na 2.8.2, no fim (os estados antigos, sem ele, continuam abrindo): em que ponto está a conversa
  // com o chip de infravermelho dos cartuchos Pokémon e a sondagem do tipo de save
  if (!io.reading() || !io.atEnd()) cart.syncIr(io);
  if (io.reading()) {
    // validação: nada pode apontar para fora das memórias nem travar o app
    for (CpuIo* c : {&io9, &io7}) {
      c->ime &= 1;
      clampTo(c->recv.r, 0, 15);
      clampTo(c->recv.n, 0, 16);
      for (auto& t : c->tm) { t.counter &= 0xFFFF; t.sub &= 0x3FF; t.ctrl &= 0xC7; }
      for (auto& d : c->dma) if (d.count > 0x200000) d.count = 0x200000;
    }
    wramcnt &= 3;
    clampTo(vcount, uint16_t(0), uint16_t(LINES - 1));
    clampTo(lineCycle, 0, LINE_CYCLES);
    clampTo(touchX, 0, SCREEN_W - 1);
    clampTo(touchY, 0, SCREEN_H - 1);
    if (cyc9 > 1000000 || cyc9 < -1000000) cyc9 = 0;
    if (cyc7 > 1000000 || cyc7 < -1000000) cyc7 = 0;
    remapVram();
    updateTcm();
    updateIrq();
    mapGen++;
  }
}

std::vector<uint8_t> NDS::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void NDS::loadState(const std::vector<uint8_t>& s) {
  // carrega numa cópia: um estado ruim não estraga o jogo em andamento
  auto tmp = std::make_unique<NDS>();
  tmp->cart.rom = cart.rom;
  tmp->cart.romMask = cart.romMask;
  tmp->cart.chipId = cart.chipId;
  tmp->spi.firmware = spi.firmware;
  tmp->loaded = loaded;
  StateIO io(s);
  tmp->sync(io);
  if (!io.atEnd()) throw std::runtime_error("Estado salvo incompatível (sobram dados).");
  StateIO again(s);
  sync(again);
}

// ================================================================== Core
NDSCore::NDSCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) : s_(std::make_unique<NDS>()) {
  s_->loadRom(rom, save);
}

std::vector<uint8_t> NDSCore::exportSave() {
  s_->cart.dirty = false;
  return s_->cart.save;
}

void NDSCore::importSave(const std::vector<uint8_t>& b) {
  s_->cart.setSave(b);
  s_->reset();
}

void NDSCore::setPointer(int x, int y, bool down) {
  // a imagem tem as duas telas empilhadas; o toque vale só na de baixo
  const bool aTop = s_->powcnt1 & 0x8000;
  (void)aTop;
  s_->setTouch(x, y - SCREEN_H, down && y >= SCREEN_H);
}

std::string NDSCore::note() const {
  return "Nintendo DS: partida direta sem BIOS; toque na tela de baixo.";
}

}  // namespace indigo::nds
