// Máquina Atari 7800: mapa de memória, relógio (linha de 454 ciclos do mestre, CPU a 4 ou 6 ciclos por
// acesso), o DMA do MARIA que para a CPU, a saída de vídeo ponto a ponto (mudanças de cor no meio da linha
// aparecem no meio da linha), RIOT, SaveKey e o som.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "a7800.h"

namespace indigo::a7800 {

namespace {
constexpr int LINE = 454;        // ciclos do mestre por linha
constexpr int DMA_START = 28;    // o MARIA pede o barramento 7 ciclos de CPU depois do começo da linha
constexpr int VISIBLE = 134;     // primeiro ponto visível (33,5 ciclos de CPU)
constexpr int DMA_LIMIT = 430;   // o MARIA não passa disso numa linha

// paleta NTSC: 16 matizes x 16 luminâncias; matiz 1 tem a fase da salva de cor (dourado) e cada matiz gira
// 25,7 graus
struct Palette {
  uint32_t c[256];
  Palette() {
    for (int h = 0; h < 16; h++) {
      for (int l = 0; l < 16; l++) {
        const double y = 0.06 + l / 15.0 * 0.88;
        double u = 0, v = 0;
        if (h) {
          const double ang = (180.0 - (h - 1) * 25.7) * 3.14159265358979 / 180.0;
          u = 0.2 * std::cos(ang);
          v = 0.2 * std::sin(ang);
        }
        double r = y + 1.140 * v, g = y - 0.395 * u - 0.581 * v, b = y + 2.032 * u;
        if (!h && !l) r = g = b = 0;
        auto q = [](double x) { return static_cast<uint32_t>(std::lround(std::clamp(x, 0.0, 1.0) * 255)); };
        c[h * 16 + l] = 0xFF000000u | (q(b) << 16) | (q(g) << 8) | q(r);
      }
    }
  }
};
const Palette PAL_NTSC;

bool slowAddr(uint16_t a) {
  if (a >= 0x600) return false;
  if (a < 0x400) return (a & 0xFF) < 0x20 || (a & 0x280) == 0x280;  // TIA e RIOT (E/S)
  return a & 0x80;                                                    // RAM do RIOT
}
}  // namespace

A7800::A7800(Cart c) : cart(std::move(c)) {
  pal = cart.pal;
  lines = pal ? 313 : 263;
  outHeight = pal ? 288 : 240;
  masterHz = pal ? 1773447.0 * 4 : 1789772.5 * 4;
  fb.assign(size_t(320) * outHeight, 0xFF000000u);
  palette = PAL_NTSC.c;
  hasPokey = cart.pokey4000 || cart.pokey450 || cart.pokey800;
  if (cart.saveKey) eeprom.assign(0x8000, 0xFF);
  const std::vector<uint8_t> fw = firmware("a7800");
  if (fw.size() == 0x1000 || fw.size() == 0x4000) bios = fw;
  cpu.bus = this;
  cpu.decimalMode = true;
  cpu.magic = 0xEE;
  setSampleRate(48000);
  reset(true);
}

void A7800::reset(bool power) {
  if (power) {
    std::memset(ram1, 0, sizeof ram1);
    std::memset(ram2, 0, sizeof ram2);
    std::memset(riotRam, 0, sizeof riotRam);
    std::fill(cart.ram.begin(), cart.ram.end(), 0);
    cpu.powerOn();
    mclk = lineStart = haltUntil = phi2 = riotLast = 0;
    audioM = tiaNext = 0;
    line = 0;
    std::memset(col, 0, sizeof col);
    std::memset(lineRam, 0, sizeof lineRam);
    ddra = ora = ddrb = orb = 0;
    intim = 0; riotFlags = 0; interval = 1024; counter = 1024; expired = false;
    tia.reset();
    pokey.reset();
    i2cState = 0; i2cBits = 0; i2cByte = 0; i2cOut = 1; i2cScl = i2cSda = true; i2cRead = i2cAck = false;
    i2cAddr = 0; i2cCount = 0;
    sampleT = sampleV = capV = 0;
    audioLevel = 0;
    front = 0; rendered = 0;
    openBus = 0;
    oneButton = true;
  }
  cart.reset();
  ctrl = 0x7F; dmaOn = false; vblank = true; wsync = false;
  dll = dl = 0; offset = holey = writeMode = 0; nmiPending = nmiLine = false;
  dmaPending = false;
  biosOn = !bios.empty();
  ctrlLock = !biosOn;
  cpu.reset();
}

// ---------------------------------------------------------------- controles
void A7800::setKeys(uint32_t m) {
  uint8_t j = 0xFF;
  if (m & KEY_RIGHT) j &= ~0x80;
  if (m & KEY_LEFT) j &= ~0x40;
  if (m & KEY_DOWN) j &= ~0x20;
  if (m & KEY_UP) j &= ~0x10;
  if ((j & 0xC0) == 0) j |= 0xC0;  // o joystick não deixa as duas direções opostas
  if ((j & 0x30) == 0) j |= 0x30;
  joy = j;
  buttons = static_cast<uint8_t>(((m & KEY_A) ? 1 : 0) | ((m & KEY_B) ? 2 : 0));
  sw = static_cast<uint8_t>(((m & KEY_START) ? 1 : 0) | ((m & KEY_SELECT) ? 2 : 0) | ((m & KEY_X) ? 8 : 0));
  const uint32_t pressed = m & ~prevKeys;
  if (pressed & KEY_L) diff ^= 1;
  if (pressed & KEY_R) diff ^= 2;
  prevKeys = m;
}

uint8_t A7800::swcha() const {
  const uint8_t pins = static_cast<uint8_t>((joy & 0xF0) | 0x0F);
  uint8_t v = static_cast<uint8_t>((pins & ~ddra) | (ora & ddra));
  if (cart.saveKey && !saveKeySda()) v &= ~0x04;  // a EEPROM puxa a linha SDA para baixo
  return v;
}

uint8_t A7800::tiaRead(int r) const {
  switch (r) {
    case 0x8: return (buttons & 2) ? 0x80 : 0;  // INPT0: botão direito (nível alto = apertado)
    case 0x9: return (buttons & 1) ? 0x80 : 0;  // INPT1: botão esquerdo
    case 0xC: return (oneButton && buttons) ? 0 : 0x80;  // INPT4: botão do modo de um botão (nível baixo)
    case 0xD: return 0x80;
    default: return 0;
  }
}

// ---------------------------------------------------------------- relógio
void A7800::advance(uint64_t n) {
  const uint64_t target = mclk + n;
  for (;;) {
    if (dmaPending && lineStart + DMA_START <= target) {
      if (mclk < lineStart + DMA_START) mclk = lineStart + DMA_START;
      dmaPending = false;
      runDma();
      continue;
    }
    if (lineStart + LINE <= target) {
      mclk = lineStart + LINE;
      endLine();
      continue;
    }
    mclk = target;
    return;
  }
}

void A7800::idleTo(uint64_t m) {
  if (m <= mclk) return;
  phi2 += m - mclk;  // o relógio da CPU continua correndo com ela parada
  advance(m - mclk);
}

void A7800::beginCycle(uint16_t a) {
  if (mclk < haltUntil) idleTo(haltUntil);
  phi2 += 4;  // o RIOT conta ciclos da CPU (o ciclo lento também é um só)
  cpuCycles++;
  advance(slowAddr(a) ? 6 : 4);
}

void A7800::endCycle() {
  bool irq = false;
  if (hasPokey && pokey.irqEnabled()) { audioSync(); irq = pokey.irq(); }
  cpu.endCycle(nmiLine, irq);
  nmiLine = false;
}

uint8_t A7800::read(uint16_t a) {
  beginCycle(a);
  const uint8_t v = memRead(a);
  openBus = v;
  endCycle();
  return v;
}

void A7800::write(uint16_t a, uint8_t v) {
  beginCycle(a);
  openBus = v;
  memWrite(a, v);
  endCycle();
  if (wsync) {  // a CPU espera o começo da próxima linha
    wsync = false;
    idleTo(lineStart + LINE);
  }
}

void A7800::endLine() {
  renderTo(LINE);
  std::memset(lineRam[front], 0, 160);
  front ^= 1;
  lineStart += LINE;
  rendered = 0;
  if (++line >= lines) { line = 0; frameDone = true; frameCount++; }
  if (line == 16) vblank = false;
  if (line == lines - 5) vblank = true;
  dmaPending = line >= 16 && line < lines - 5;
  audioSync();
}

void A7800::runFrame() {
  frameDone = false;
  // um quadro tem ~30 mil ciclos de CPU; o limite só protege contra estado impossível
  for (int guard = 0; !frameDone && guard < 400000; guard++) {
    cpu.step();
    if (biosFailed) { biosFailed = false; bios.clear(); reset(true); }
  }
}

// ---------------------------------------------------------------- memória
uint8_t A7800::memRead(uint16_t a) {
  if (a >= 0x4000) {
    if (biosOn && a >= 0x10000 - bios.size()) return bios[a - (0x10000 - bios.size())];
    if (a < 0x8000) {
      if (cart.mirrorRam) return cart.ram[(a - 0x4000) & 0x3EFF];
      if (cart.pokey4000) { audioSync(); return pokey.read(a, mclk / 4); }
    }
    const uint8_t* p = cart.rpage[a >> 12];
    return p ? p[a & 0xFFF] : openBus;
  }
  if (a >= 0x3000) { const uint8_t* p = cart.rpage[3]; return p ? p[a & 0xFFF] : openBus; }
  if (a >= 0x2000) return ram2[a & 0x7FF];
  if (a >= 0x1800) return ram1[a & 0x7FF];
  if (a < 0x400) {
    const int lo = a & 0xFF;
    if (lo < 0x20) return tiaRead(lo & 0x0F);
    if (lo < 0x40) return lo == 0x28 ? (vblank ? 0x80 : 0) : 0;  // MSTAT; o resto do MARIA só aceita escrita
    if ((a & 0x280) == 0x280) return riotRead(a);
    if (a < 0x100 || (a >= 0x140 && a < 0x200)) return ram2[a];  // espelhos de $2040 e $2140
    return openBus;
  }
  if (a < 0x600 && (a & 0x80)) return riotRam[a & 0x7F];
  if (cart.pokey450 && a >= 0x450 && a < 0x470) { audioSync(); return pokey.read(a, mclk / 4); }
  if (cart.pokey800 && a >= 0x800 && a < 0x1000) { audioSync(); return pokey.read(a, mclk / 4); }
  return openBus;
}

uint8_t A7800::peek(uint16_t a) const {
  if (a >= 0x4000) { const uint8_t* p = cart.rpage[a >> 12]; return p ? p[a & 0xFFF] : 0; }
  return mariaRead(a);
}

void A7800::memWrite(uint16_t a, uint8_t v) {
  if (a >= 0x4000) {
    if (a < 0x8000) {
      if (cart.mirrorRam) { cart.ram[(a - 0x4000) & 0x3EFF] = v; return; }
      if (cart.pokey4000) { audioSync(); pokey.write(a, v, mclk / 4); return; }
    }
    uint8_t* p = cart.wpage[a >> 12];
    if (p) p[a & 0xFFF] = v;
    else cart.write(a, v);
    return;
  }
  if (a >= 0x3000) return;
  if (a >= 0x2000) { ram2[a & 0x7FF] = v; return; }
  if (a >= 0x1800) { ram1[a & 0x7FF] = v; return; }
  if (a < 0x400) {
    const int lo = a & 0xFF;
    if (lo < 0x20) {
      if (!ctrlLock) {  // INPTCTRL: só até travar
        ctrlLock = v & 1;
        if (v & 4) biosOn = false;
        // a BIOS desistiu do modo 7800 (assinatura recusada): liga direto no cartucho, sem ela
        if ((v & 1) && !(v & 2) && !bios.empty()) biosFailed = true;
        return;
      }
      if (lo >= 0x15 && lo <= 0x1A) { audioSync(); tia.write(lo, v); }
      return;
    }
    if (lo < 0x40) { mariaWrite(lo & 0x1F, v); return; }
    if ((a & 0x280) == 0x280) { riotWrite(a, v); return; }
    if (a < 0x100 || (a >= 0x140 && a < 0x200)) ram2[a] = v;
    return;
  }
  if (a < 0x600 && (a & 0x80)) { riotRam[a & 0x7F] = v; return; }
  if (cart.pokey450 && a >= 0x450 && a < 0x470) { audioSync(); pokey.write(a, v, mclk / 4); return; }
  if (cart.pokey800 && a >= 0x800 && a < 0x1000) { audioSync(); pokey.write(a, v, mclk / 4); return; }
}

uint8_t A7800::mariaRead(uint16_t a) const {
  if (a >= 0x4000) {
    if (biosOn && a >= 0x10000 - bios.size()) return bios[a - (0x10000 - bios.size())];
    if (cart.mirrorRam && a < 0x8000) return cart.ram[(a - 0x4000) & 0x3EFF];
    if (a >= 0xA000 && a < 0xC000 && cart.mariaHasSpecial()) return cart.mariaSpecial(a);
    const uint8_t* p = cart.mpage[a >> 12];
    return p ? p[a & 0xFFF] : 0;
  }
  if (a >= 0x3000) { const uint8_t* p = cart.mpage[3]; return p ? p[a & 0xFFF] : 0; }
  if (a >= 0x2000) return ram2[a & 0x7FF];
  if (a >= 0x1800) return ram1[a & 0x7FF];
  if (a < 0x400) {
    if ((a & 0xFF) >= 0x40 && (a < 0x100 || (a >= 0x140 && a < 0x200))) return ram2[a];
    return 0;
  }
  if (a < 0x600 && (a & 0x80)) return riotRam[a & 0x7F];
  return 0;
}

// ---------------------------------------------------------------- MARIA
void A7800::mariaWrite(int r, uint8_t v) {
  if ((r & 3) || r == 0 || r == 0x1C) renderTo(static_cast<int>(mclk - lineStart));
  if (r & 3) { col[r] = v; return; }
  switch (r) {
    case 0x00: col[0] = v; break;
    case 0x04: wsync = true; break;
    case 0x0C: dpph = v; break;
    case 0x10: dppl = v; break;
    case 0x14: charbase = v; break;
    case 0x1C:
      ctrl = v;
      if (((v >> 5) & 3) == 2) dmaOn = true;
      else if (((v >> 5) & 3) == 3) dmaOn = false;
      break;
    default: break;
  }
}

void A7800::loadDll() {
  const uint8_t b = mariaRead(dll);
  offset = b & 15;
  holey = (b >> 5) & 3;
  if (b & 0x80) nmiPending = true;
  dl = static_cast<uint16_t>((mariaRead(static_cast<uint16_t>(dll + 1)) << 8) | mariaRead(static_cast<uint16_t>(dll + 2)));
}

void A7800::runDma() {
  std::memset(lineRam[front ^ 1], 0, 160);
  if (!dmaOn) { lastDmaCycles = 0; return; }
  if (line == 16) { dll = static_cast<uint16_t>((dpph << 8) | dppl); loadDll(); }
  const int cycles = drawLine();
  lastDmaCycles = cycles;
  if (line > 16) {
    if (offset == 0) { dll = static_cast<uint16_t>(dll + 3); loadDll(); }
    else offset--;
  }
  // o MARIA devolve o barramento num ciclo inteiro de CPU; se usou o tempo todo, só na próxima linha
  haltUntil = cycles >= DMA_LIMIT ? lineStart + LINE : mclk + static_cast<uint64_t>((cycles + 3) & ~3);
  if (nmiPending) { nmiPending = false; nmiLine = true; }
}

// grava um byte de gráfico na memória de linha; devolve quantas células andou
int A7800::lineRamWrite(uint16_t addr, int hpos, int palIdx) {
  uint8_t* lr = lineRam[front ^ 1];
  const uint8_t d = mariaRead(addr);
  const int p = palIdx << 2;
  const bool kang = ctrl & 4;
  if (writeMode) {
    int c = (p & 0x10) | (d & 0x0C) | (d >> 6);  // 160B/320B/320C: P2 D3 D2 D7 D6
    if (((c & 3) || kang) && (hpos & 0xFF) < 160) lr[hpos & 0xFF] = static_cast<uint8_t>(c);
    c = (p & 0x10) | ((d & 0x03) << 2) | ((d & 0x30) >> 4);  // P2 D1 D0 D5 D4
    if (((c & 3) || kang) && ((hpos + 1) & 0xFF) < 160) lr[(hpos + 1) & 0xFF] = static_cast<uint8_t>(c);
    return 2;
  }
  for (int i = 0; i < 4; i++) {
    const int c = p | ((d >> (6 - 2 * i)) & 3);
    const int x = (hpos + i) & 0xFF;
    if (((c & 3) || kang) && x < 160) lr[x] = static_cast<uint8_t>(c);
  }
  return 4;
}

// percorre a lista de exibição da zona e devolve os ciclos (7,16 MHz) que o DMA levou
int A7800::drawLine() {
  int cycles = 16;  // partida do DMA
  uint16_t p = dl;
  const bool cw = ctrl & 0x10;
  auto isHole = [&](uint16_t a) {
    return ((holey & 2) && (a & 0x9000) == 0x9000) || ((holey & 1) && (a & 0x8800) == 0x8800);
  };
  for (int guard = 0; guard < 128 && cycles < DMA_LIMIT; guard++) {
    const uint8_t b1 = mariaRead(static_cast<uint16_t>(p + 1));
    if ((b1 & 0x5F) == 0) break;  // fim da lista
    uint16_t gfx = static_cast<uint16_t>((mariaRead(static_cast<uint16_t>(p + 2)) << 8) | mariaRead(p));
    int width, hpos, palIdx;
    bool ind = false;
    if ((b1 & 0x1F) == 0) {  // cabeçalho de 5 bytes
      const uint8_t b3 = mariaRead(static_cast<uint16_t>(p + 3));
      width = ((b3 ^ 0xFF) & 0x1F) + 1;
      hpos = mariaRead(static_cast<uint16_t>(p + 4));
      palIdx = b3 >> 5;
      writeMode = b1 & 0x80;
      ind = b1 & 0x20;
      p = static_cast<uint16_t>(p + 5);
      cycles += 10;
    } else {
      width = ((b1 ^ 0xFF) & 0x1F) + 1;
      hpos = mariaRead(static_cast<uint16_t>(p + 3));
      palIdx = b1 >> 5;
      p = static_cast<uint16_t>(p + 4);
      cycles += 8;
    }
    bool holeSeen = false;
    const int step = writeMode ? 2 : 4;
    for (int x = 0; x < width && cycles < DMA_LIMIT; x++) {
      if (ind) {
        const uint8_t c = mariaRead(static_cast<uint16_t>(gfx + x));
        const uint16_t a = static_cast<uint16_t>(((charbase << 8) | c) + (offset << 8));
        if (isHole(a)) {
          if (!holeSeen) cycles += 3;
          holeSeen = true;
          hpos += step * (cw ? 2 : 1);
          continue;
        }
        cycles += 3;
        hpos += lineRamWrite(a, hpos, palIdx);
        if (cw) { hpos += lineRamWrite(static_cast<uint16_t>(a + 1), hpos, palIdx); cycles += 6; }
        else cycles += 3;
      } else {
        const uint16_t a = static_cast<uint16_t>(gfx + x + (offset << 8));
        if (isHole(a)) {
          if (!holeSeen) cycles += 3;
          holeSeen = true;
          hpos += step;
          continue;
        }
        hpos += lineRamWrite(a, hpos, palIdx);
        cycles += 3;
      }
    }
  }
  if (offset == 0) {  // última linha da zona: busca da DLL (e o NMI, se a próxima zona pede)
    cycles += 8;
    if (mariaRead(static_cast<uint16_t>(dll + 3)) & 0x80) cycles += 17;
  }
  return cycles;
}

void A7800::renderTo(int pos) {
  const int px = std::clamp(pos - VISIBLE, 0, 320);
  if (px <= rendered) return;
  const int row = line - kFirstOut;
  if (row < 0 || row >= outHeight) { rendered = px; return; }
  uint32_t* out = fb.data() + size_t(row) * 320;
  const uint8_t* lr = lineRam[front];
  const int rm = ctrl & 3;
  const uint8_t kill = (ctrl & 0x80) ? 0x0F : 0xFF;
  for (int x = rendered; x < px; x++) {
    const uint8_t c = lr[x >> 1];
    uint8_t idx;
    if (rm < 2) idx = c;
    else if (rm == 2) idx = (x & 1) ? static_cast<uint8_t>((c & 0x10) | ((c << 1) & 2) | ((c >> 2) & 1))
                                    : static_cast<uint8_t>((c & 0x10) | (c & 2) | ((c >> 3) & 1));
    else idx = (x & 1) ? static_cast<uint8_t>((c & 0x1C) | ((c << 1) & 2)) : static_cast<uint8_t>(c & 0x1E);
    const uint8_t v = (idx & 3) ? col[idx & 0x1F] : col[0];
    out[x] = palette[v & kill];
  }
  rendered = px;
}

// ---------------------------------------------------------------- RIOT
void A7800::riotSync() {
  uint64_t n = (phi2 >> 2) - riotLast;
  riotLast = phi2 >> 2;
  while (n) {
    if (expired) { intim = static_cast<uint8_t>(intim - n); return; }
    if (n < counter) { counter -= static_cast<uint32_t>(n); return; }
    n -= counter;
    if (intim == 0) { expired = true; riotFlags |= 0x80; intim = 0xFF; counter = 1; continue; }
    intim--;
    counter = interval;
    const uint64_t k = std::min<uint64_t>(n / interval, intim);
    intim = static_cast<uint8_t>(intim - k);
    n -= k * interval;
  }
}

uint8_t A7800::riotRead(uint16_t a) {
  if (!(a & 4)) {
    switch (a & 3) {
      case 0: return swcha();
      case 1: return ddra;
      case 2: {
        uint8_t in = 0xFF;
        if (sw & 1) in &= ~0x01;
        if (sw & 2) in &= ~0x02;
        if (sw & 8) in &= ~0x08;
        if (!(diff & 1)) in &= ~0x40;
        if (!(diff & 2)) in &= ~0x80;
        return static_cast<uint8_t>((in & ~ddrb) | (orb & ddrb));
      }
      default: return ddrb;
    }
  }
  riotSync();
  if (!(a & 1)) { riotFlags &= ~0x80; return intim; }  // INTIM (apaga o aviso do temporizador)
  const uint8_t f = riotFlags;
  riotFlags &= ~0x40;
  return f;
}

void A7800::riotWrite(uint16_t a, uint8_t v) {
  if (!(a & 4)) {
    switch (a & 3) {
      case 0: ora = v; break;
      case 1: ddra = v; break;
      case 2: orb = v; break;
      default: ddrb = v; break;
    }
    // pino 6 da porta 1 em saída baixa = joystick de dois botões
    if ((a & 3) >= 2 && (ddrb & 4)) oneButton = orb & 4;
    if ((a & 3) < 2 && cart.saveKey) {
      const bool scl = !(ddra & 8) || (ora & 8);
      const bool sda = !(ddra & 4) || (ora & 4);
      saveKeyPins(scl, sda);
    }
    return;
  }
  if (a & 0x10) {
    riotSync();
    static const uint16_t iv[4] = {1, 8, 64, 1024};
    interval = iv[a & 3];
    intim = v;
    counter = interval;
    expired = false;
    riotFlags &= ~0x80;
  }
}

// ---------------------------------------------------------------- SaveKey (EEPROM I2C 24LC256 na porta 2)
bool A7800::saveKeySda() const { return i2cOut != 0; }

void A7800::saveKeyPins(bool scl, bool sda) {
  enum { IDLE, DEV, ADDR_HI, ADDR_LO, WRITE, READ };
  const bool rise = scl && !i2cScl, fall = !scl && i2cScl;
  const bool wasSda = i2cSda;
  i2cSda = sda;
  i2cScl = scl;
  if (scl && !rise && sda != wasSda) {  // SDA mudou com SCL alto: início ou parada
    i2cState = sda ? IDLE : DEV;
    i2cBits = 0; i2cByte = 0; i2cAck = false; i2cOut = 1;
    return;
  }
  if (i2cState == IDLE) return;
  auto loadByte = [&] {
    i2cByte = eeprom[i2cAddr & 0x7FFF];
    i2cAddr = static_cast<uint16_t>((i2cAddr + 1) & 0x7FFF);
    i2cOut = i2cByte >> 7;
    i2cBits = 1;
  };
  if (i2cState == READ) {  // a EEPROM manda os bits; i2cBits = bits já postos na linha
    if (i2cAck) { if (fall) { i2cAck = false; loadByte(); } return; }  // fim da confirmação do byte de leitura
    if (rise && i2cBits == 9) i2cRead = !sda;  // o mestre confirma (0) ou encerra (1)
    if (fall) {
      if (i2cBits < 8) { i2cOut = (i2cByte >> (7 - i2cBits)) & 1; i2cBits++; }
      else if (i2cBits == 8) { i2cOut = 1; i2cBits = 9; }
      else if (i2cRead) loadByte();
      else { i2cState = IDLE; i2cOut = 1; }
    }
    return;
  }
  if (rise && !i2cAck && i2cBits < 8) { i2cByte = static_cast<uint8_t>((i2cByte << 1) | (sda ? 1 : 0)); i2cBits++; }
  if (!fall) return;
  if (i2cAck) {  // fim do pulso de confirmação
    i2cAck = false;
    i2cOut = 1;
    return;
  }
  if (i2cBits < 8) return;
  const uint8_t b = i2cByte;
  i2cBits = 0;
  i2cByte = 0;
  switch (i2cState) {
    case DEV:
      if ((b & 0xF0) != 0xA0) { i2cState = IDLE; return; }
      i2cState = (b & 1) ? READ : ADDR_HI;
      break;
    case ADDR_HI: i2cAddr = static_cast<uint16_t>((b & 0x7F) << 8); i2cState = ADDR_LO; break;
    case ADDR_LO: i2cAddr = static_cast<uint16_t>(i2cAddr | b); i2cState = WRITE; break;
    default:
      if (eeprom[i2cAddr & 0x7FFF] != b) { eeprom[i2cAddr & 0x7FFF] = b; dirty = true; }
      i2cAddr = static_cast<uint16_t>((i2cAddr & 0x7FC0) | ((i2cAddr + 1) & 0x3F));  // página de 64 bytes
      break;
  }
  i2cAck = true;
  i2cOut = 0;
}

// ---------------------------------------------------------------- som
void A7800::setSampleRate(int rate) { samplePeriod = masterHz / std::max(rate, 8000); }

void A7800::emit(double v) {
  const double o = v - capV;
  capV += o * 0.0015;
  buf[wpos] = static_cast<float>(o);
  wpos = (wpos + 1) % kBuf;
  if (wpos == rpos) rpos = (rpos + 1) % kBuf;
}

void A7800::integrate(int lvl, uint64_t dt) {
  const double v = lvl * (0.8 / 60.0);
  double d = static_cast<double>(dt);
  while (sampleT + d >= samplePeriod) {
    const double take = samplePeriod - sampleT;
    emit((sampleV + v * take) / samplePeriod);
    d -= take;
    sampleT = 0;
    sampleV = 0;
  }
  sampleT += d;
  sampleV += v * d;
}

void A7800::audioSync() {
  while (audioM < mclk) {
    uint64_t nxt = std::min(tiaNext, mclk);
    if (hasPokey) {
      const uint64_t pe = pokey.nextEvent();
      if (pe < UINT64_MAX / 8 && pe * 4 < nxt) nxt = std::max(pe * 4, audioM);
    }
    integrate(audioLevel, nxt - audioM);
    audioM = nxt;
    if (audioM >= tiaNext) { tia.tick(); tiaNext += 228; }
    if (hasPokey) pokey.fire(audioM / 4);
    audioLevel = tia.level() + (hasPokey ? pokey.level() : 0);
  }
}

void A7800::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { l[k] = r[k] = 0; continue; }
    l[k] = r[k] = buf[rpos];
    rpos = (rpos + 1) % kBuf;
  }
}

// ---------------------------------------------------------------- estados
void A7800::sync(StateIO& io) {
  cpu.sync(io);
  cart.sync(io);
  io.raw(ram1, sizeof ram1); io.raw(ram2, sizeof ram2); io.raw(riotRam, sizeof riotRam);
  io.sync(biosOn); io.sync(ctrlLock); io.sync(openBus);
  io.sync(mclk); io.sync(lineStart); io.sync(haltUntil); io.sync(phi2); io.sync(line);
  io.sync(dmaPending); io.sync(nmiLine); io.sync(wsync);
  io.raw(col, sizeof col); io.sync(ctrl); io.sync(dpph); io.sync(dppl); io.sync(charbase);
  io.sync(dmaOn); io.sync(vblank); io.sync(dll); io.sync(dl); io.sync(offset); io.sync(holey); io.sync(writeMode);
  io.sync(nmiPending); io.raw(lineRam, sizeof lineRam); io.sync(front); io.sync(rendered);
  io.sync(ddra); io.sync(ora); io.sync(ddrb); io.sync(orb); io.sync(intim); io.sync(riotFlags);
  io.sync(interval); io.sync(counter); io.sync(expired); io.sync(riotLast);
  io.sync(diff); io.sync(prevKeys); io.sync(oneButton);
  io.sync(i2cState); io.sync(i2cBits); io.sync(i2cByte); io.sync(i2cOut); io.sync(i2cScl); io.sync(i2cSda);
  io.sync(i2cRead); io.sync(i2cAck); io.sync(i2cAddr); io.sync(i2cCount);
  if (cart.saveKey) io.bytes(eeprom);
  tia.sync(io);
  pokey.sync(io);
  io.sync(audioM); io.sync(tiaNext); io.sync(audioLevel); io.sync(sampleT); io.sync(sampleV); io.sync(capV);
  io.sync(frameCount);
  if (io.reading()) {
    // só valores que o aparelho não pode ter são trocados: um estado válido volta byte a byte igual
    if (biosOn && bios.empty()) biosOn = false;
    if (line < 0 || line >= lines) line = 0;
    if (mclk < lineStart || mclk - lineStart >= LINE) lineStart = mclk;
    if (haltUntil > mclk + 4 * LINE) haltUntil = mclk;
    front &= 1;
    if (rendered < 0 || rendered > 320) rendered = 0;
    if (interval != 1 && interval != 8 && interval != 64 && interval != 1024) interval = 1024;
    if (counter == 0 || counter > interval) counter = interval;
    if (riotLast > (phi2 >> 2)) riotLast = phi2 >> 2;
    if (audioM > mclk || mclk - audioM > 4 * LINE) audioM = mclk;
    if (tiaNext < audioM || tiaNext > audioM + 228) tiaNext = audioM;
    if (audioLevel < 0 || audioLevel > 90) audioLevel = 0;
    finiteOr(sampleT, 0); finiteOr(sampleV, 0); finiteOr(capV, 0);
    if (sampleT < 0 || sampleT >= samplePeriod) { sampleT = 0; sampleV = 0; }
    if (std::fabs(sampleV) > 1e6) sampleV = 0;
    if (std::fabs(capV) > 1e6) capV = 0;
    if (i2cState > 5) i2cState = 0;
    if (i2cBits > 9) i2cBits = 0;
    if (hasPokey) pokey.sanitize(audioM / 4);
  }
}

std::vector<uint8_t> A7800::saveState() {
  audioSync();
  StateIO io;
  uint32_t magic = 0x30383741;  // "A780"
  io.sync(magic);
  sync(io);
  return io.data();
}

void A7800::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint32_t magic = 0;
  io.sync(magic);
  if (magic != 0x30383741) throw std::runtime_error("Este estado não é de Atari 7800.");
  const std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
  } catch (...) {
    StateIO restore(backup);
    restore.sync(magic);
    sync(restore);
    throw;
  }
}

// ---------------------------------------------------------------- núcleo
namespace {
Cart loadCart(const std::vector<uint8_t>& rom) {
  Cart c;
  std::string err;
  if (!c.load(rom, "", err)) throw std::runtime_error(err);
  return c;
}
}  // namespace

A7800Core::A7800Core(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save)
    : m_(std::make_unique<A7800>(loadCart(rom))) {
  if (save) importSave(*save);
}

double A7800Core::fps() const { return m_->pal ? 1773447.0 * 4 / (454.0 * 313) : 1789772.5 * 4 / (454.0 * 263); }

void A7800Core::importSave(const std::vector<uint8_t>& b) {
  if (b.empty() || m_->eeprom.empty()) return;
  std::memcpy(m_->eeprom.data(), b.data(), std::min(b.size(), m_->eeprom.size()));
  m_->dirty = false;
}

std::string A7800Core::note() const {
  if (m_->cart.kind == Cart::SOUPER) return "Cartucho Souper: a música do chip BupChip não é emulada.";
  return "";
}

}  // namespace indigo::a7800
