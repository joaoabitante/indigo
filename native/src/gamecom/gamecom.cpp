// Núcleo Tiger Game.com (ver gamecom.h).
#include "gamecom.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::gamecom {

namespace {
constexpr char STATE_MAGIC[4] = {'G', 'C', 'M', '1'};
constexpr uint32_t CART_FLAG = 0x80000000u;
// pré-divisores dos timers 0/1 (o MAME conta metade do valor da tabela em ciclos da CPU)
constexpr uint32_t TIMER_LIMIT[8] = {2, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
// tons do LCD (do mais escuro ao mais claro), em 0xAABBGGRR
constexpr uint32_t SHADE[5] = {0xFF000000u, 0xFF2F4F0Fu, 0xFF4F8F6Fu, 0xFF8FCF8Fu, 0xFF8FFFDFu};

bool hasSig(const std::vector<uint8_t>& b, size_t at) {
  static const char SIG[] = "TigerDMGC";
  return b.size() >= at + 5 + 9 && std::memcmp(b.data() + at + 5, SIG, 9) == 0;
}
bool endsWith(const std::string& s, const char* suf) {
  const std::string x(suf);
  return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}
}  // namespace

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

bool isGamecomRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.size() > CART_SPACE) return false;
  if (hasSig(rom, 0) || hasSig(rom, 0x40000)) return true;
  return endsWith(n, ".tgc") && rom.size() >= 0x20;
}

std::string gamecomFirmwareKind(const std::vector<uint8_t>& b, const std::string& n) {
  if (b.size() != INT_ROM_SIZE && b.size() != EXT_ROM_SIZE) return "";
  const uint32_t c = crc32(b.data(), b.size());
  if (b.size() == INT_ROM_SIZE && c == 0xA0CEC361u) return "gamecom_int";  // internal.bin conhecido
  if (b.size() == EXT_ROM_SIZE && c == 0xE235A589u) return "gamecom_ext";  // external.bin conhecido
  // cartucho nunca vira ROM do console: assinatura TigerDMGC ou extensão .tgc
  if (hasSig(b, 0) || endsWith(n, ".tgc")) return "";
  // pelo nome, só o do arquivo (a pasta pode se chamar "gamecom")
  const size_t sl = n.find_last_of("/\\");
  const std::string f = sl == std::string::npos ? n : n.substr(sl + 1);
  const bool named = f.find("gamecom") != std::string::npos || f.find("game.com") != std::string::npos ||
                     f.find("game_com") != std::string::npos;
  if (!named) return "";
  return b.size() == INT_ROM_SIZE ? "gamecom_int" : "gamecom_ext";
}

// ------------------------------------------------------------------ montagem
GameCom::GameCom(const std::vector<uint8_t>& c, const std::vector<uint8_t>& ir, const std::vector<uint8_t>& er)
    : cart(CART_SPACE, 0xFF), extRom(er), bufL(AUDIO_BUF), bufR(AUDIO_BUF) {
  if (c.empty() || c.size() > CART_SPACE) throw std::runtime_error("Cartucho de Game.com com tamanho inválido.");
  // como no MAME: 1,75 MB entra em $40000; o resto se repete até encher os 2 MB
  if (c.size() == 0x1C0000) {
    std::memcpy(cart.data() + 0x40000, c.data(), c.size());
    std::memcpy(cart.data(), c.data(), 0x40000);
  } else {
    for (size_t i = 0; i < CART_SPACE; i++) cart[i] = c[i % c.size()];
  }
  const size_t h = hasSig(cart, 0x40000) ? 0x40000 : 0;
  for (int i = 0; i < 9; i++) {
    const char ch = char(cart[h + 0x11 + size_t(i)]);
    if (ch >= 0x20 && ch < 0x7F) cartTitle += ch;
  }
  while (!cartTitle.empty() && cartTitle.back() == ' ') cartTitle.pop_back();
  std::memcpy(intRom, ir.data(), std::min(ir.size(), sizeof intRom));
  extRom.resize(EXT_ROM_SIZE, 0xFF);
  reset();
}

std::string GameCom::title() const { return cartTitle.empty() ? "Game.com" : cartTitle; }

void GameCom::reset() {
  std::memset(ram, 0, sizeof ram);
  std::memset(vram, 0, sizeof vram);
  for (auto& b : bankBase) b = 0;
  cartSelected = false;
  cycles = 0;
  lineCycle = 0;
  lineNo = 0;
  frameDone = false;
  tm[0] = tm[1] = Timer{};
  clkOn = false;
  clkPeriod = clkCount = 0;
  dmaPending = false;
  dmaWait = 0;
  sg[0] = sg[1] = Tone{};
  noiseAcc = 0;
  lfsr = 0x7FFF;
  dacLevel = 0x80;
  cpu.reset();
  // valores iniciais das portas (como no MAME)
  ram[0x14] = 0xFF; ram[0x15] = 0xFF; ram[0x16] = 0xFF; ram[0x17] = 0x00;
  muxLo = muxHi = 0xFF;
  ram[0x2B] = 0xFF; ram[0x2D] = 0x42; ram[0x5F] = 0x38;
}

// ------------------------------------------------------------------ barramento
uint8_t GameCom::read(uint16_t a) {
  if (a < 0x400) {
    if (a == 0x32) return uint8_t((ram[a] & 0x7F) | (lineNo >= SCREEN_W ? 0x80 : 0));  // LCV: bit 7 = retraço
    return ram[a];
  }
  if (a < 0x1000) return 0;
  if (a < 0x2000) return intRom[a - 0x1000];
  if (a < 0xA000) {
    const uint32_t b = bankBase[((a - 0x2000) >> 13) + 1];
    const uint32_t off = (b & ~CART_FLAG) + (a & 0x1FFFu);
    return (b & CART_FLAG) ? cart[off & (CART_SPACE - 1)] : extRom[off & (EXT_ROM_SIZE - 1)];
  }
  if (a < 0xE000) return 0;  // vídeo: só escrita
  return ext[a - 0xE000];
}

void GameCom::write(uint16_t a, uint8_t v) {
  if (a < 0x400) {
    if (a >= 0x14 && a <= 0x17) {
      ram[a] = v;
      if (a == 0x15) muxLo = v;
      if (a == 0x16) muxHi = v;
      if (a == 0x15 || a == 0x16) inputMux();
      if (a == 0x17) cartSelected = (v & 0xC0) == 0x40;  // só o cartucho 1
      return;
    }
    if (a >= 0x20 && a < 0x80) { ioWrite(uint8_t(a), v); return; }
    ram[a] = v;
    return;
  }
  if (a < 0xA000) return;
  if (a < 0xE000) { vram[a - 0xA000] = v; return; }
  if (ext[a - 0xE000] != v) extDirty = true;
  ext[a - 0xE000] = v;
}

std::vector<uint8_t> GameCom::exportExt() {
  extDirty = false;
  return std::vector<uint8_t>(ext, ext + sizeof ext);
}

void GameCom::importExt(const std::vector<uint8_t>& b) {
  if (b.size() != sizeof ext) return;  // tamanho errado: ignora
  std::memcpy(ext, b.data(), sizeof ext);
  extDirty = false;
}

void GameCom::setMmu(int n, uint8_t v) {
  if (v < 0x20) bankBase[n] = uint32_t(v) << 13;
  else if (cartSelected) bankBase[n] = CART_FLAG | (uint32_t(v) << 13);
  // sem cartucho selecionado a janela fica como estava (como no MAME)
}

void GameCom::ioWrite(uint8_t r, uint8_t v) {
  switch (r) {
    case 0x24: break;  // MMU0: a ROM interna nunca sai do lugar no Game.com
    case 0x25: case 0x26: case 0x27: case 0x28: setMmu(r - 0x24, v); break;
    case 0x32: v &= 0x7F; break;  // LCV: o bit 7 é só leitura
    case 0x34:  // DMC
      ram[r] = v;
      if (v & 0x80) { dmaPending = true; dmaWait = 0; }
      return;
    case 0x3C: v &= 0x7F; break;  // DMBR
    case 0x50:  // TM0C
    case 0x52: {  // TM1C
      Timer& t = tm[(r - 0x50) >> 1];
      t.on = v & 0x80;
      t.preMax = std::max<uint32_t>(1, TIMER_LIMIT[v & 7] >> 1);
      t.pre = 0;
      ram[r + 1] = 0;
      break;
    }
    case 0x51: case 0x53: tm[(r - 0x51) >> 1].max = v; v = 0; break;  // TM0D/TM1D: limite; o contador zera
    case 0x54:  // CLKT
      if (v & 0x80) {
        clkOn = true;
        clkPeriod = uint32_t(CPU_HZ) * ((v & 0x40) ? 60u : 1u);
        clkCount = 0;
      } else {
        clkOn = false;
        v &= 0xC0;
      }
      break;
    case 0x4E:  // SGDA: o DAC só toca com os geradores desligados
      if ((ram[0x40] & 0x8F) == 0x88) dacLevel = v;
      break;
    default: break;
  }
  ram[r] = v;
}

// ------------------------------------------------------------------ teclas e caneta
void GameCom::setKeys(uint32_t mask) { keys = mask; }

void GameCom::setPointer(int x, int y, bool down) {
  ptrX = std::clamp(x, 0, SCREEN_W - 1);
  ptrY = std::clamp(y, 0, SCREEN_H - 1);
  ptrDown = down;
}

void GameCom::stylusColumn(int col) {
  const uint16_t data = (penDown && penX / 16 == col) ? uint16_t(1u << (penY / 16)) : 0;
  if (data) {
    const uint16_t y = data ^ 0x3FF;
    ram[0x14] = uint8_t(y);
    ram[0x15] = uint8_t((ram[0x15] & 0xFC) | (y >> 8));
  } else {
    ram[0x14] = 0xFF;
    ram[0x15] |= 3;
  }
}

void GameCom::inputMux() {
  // a multiplexação vem do que a CPU escreveu em P1/P2 (os bits 0-1 de P1 lidos são entradas)
  const uint16_t mux = uint16_t(muxLo | (muxHi << 8));
  if (mux == 0x7FFF) {  // teclas 1 (ativas em 0)
    uint8_t in0 = 0xFF, in1 = 0x03;
    if (!stylusMode) {
      if (keys & KEY_UP) in0 &= ~0x01;
      if (keys & KEY_DOWN) in0 &= ~0x02;
      if (keys & KEY_LEFT) in0 &= ~0x04;
      if (keys & KEY_RIGHT) in0 &= ~0x08;
      if (keys & KEY_A) in0 &= ~0x80;
    }
    if (keys & KEY_SELECT) in0 &= ~0x10;  // Menu
    if (keys & KEY_START) in0 &= ~0x20;   // Pause
    if (keys & KEY_R) in0 &= ~0x40;       // Sound
    if (keys & KEY_B) in1 &= ~0x01;
    if (keys & KEY_X) in1 &= ~0x02;       // C
    ram[0x14] = in0;
    ram[0x15] = uint8_t((ram[0x15] & 0xFC) | in1);
    return;
  }
  if (mux == 0xFFFF) {  // teclas 2: Power (nunca) e D
    uint8_t in2 = 0x03;
    if (keys & KEY_Y) in2 &= ~0x02;
    ram[0x14] = uint8_t((ram[0x14] & 0xFC) | in2);
    ram[0x15] = 0xFF;
    return;
  }
  // colunas da tela de toque: um bit em 0 entre os bits 2..15
  for (int col = 0; col < 13; col++)
    if (mux == uint16_t(~(1u << (col + 2)))) { stylusColumn(col); return; }
}

// ------------------------------------------------------------------ DMA (como no MAME)
void GameCom::runDma() {
  dmaPending = false;
  const uint8_t dmc = ram[0x34];
  if (!(dmc & 0x80)) return;
  const bool overwrite = dmc & 1;
  const int mode = dmc & 6;
  const int adjX = (dmc & 8) ? -1 : 1;
  const bool decY = dmc & 0x10;
  const int bw = ram[0x37], bh = ram[0x38];
  const uint8_t sx = ram[0x35], sy = ram[0x36], dx = ram[0x39], dy = ram[0x3A], pal = ram[0x3B];
  int srcW = (ram[0x31] & 0x20) ? 50 : 40, dstW = srcW;
  uint16_t srcMask = 0x1FFF, dstMask = 0x1FFF;
  const uint8_t* src = vram + ((ram[0x3D] & 1) ? 0x2000 : 0);
  uint8_t* dst = vram + ((ram[0x3D] & 2) ? 0x2000 : 0);
  switch (mode) {
    case 2:  // ROM -> vídeo (bancos de 16 KB)
      srcW = 64;
      srcMask = 0x3FFF;
      if (ram[0x3C] < 16) src = extRom.data() + (size_t(ram[0x3C]) << 14);
      else if (cartSelected) src = cart.data() + (size_t(ram[0x3C]) << 14);
      break;
    case 4:  // RAM estendida -> vídeo
      srcW = 64;
      src = ext;
      break;
    case 6:  // vídeo -> RAM estendida
      dstW = 64;
      dst = ext;
      break;
    default: break;
  }
  uint16_t srcLine = uint16_t(srcW * sy + (sx >> 2)), dstLine = uint16_t(dstW * dy + (dx >> 2));
  uint16_t srcCur = srcLine, dstCur = dstLine;
  uint8_t sxc = sx & 3, dxc = dx & 3;
  for (int y = 0; y <= bh; y++) {
    for (int x = 0; x <= bw; x++) {
      const uint16_t sa = srcCur & srcMask, da = dstCur & dstMask;
      const int dshift = (dxc ^ 3) << 1, sshift = (sxc ^ 3) << 1;
      const uint8_t px = (src[sa] >> sshift) & 3;
      if (overwrite || px) {
        const uint8_t others = uint8_t(dst[da] & ~(3 << dshift));
        dst[da] = uint8_t(others | (((pal >> (px << 1)) & 3) << dshift));
      }
      sxc = uint8_t(sxc + adjX);
      if (sxc & 4) { srcCur = uint16_t(srcCur + adjX); sxc &= 3; }
      dxc++;
      if (dxc & 4) { dstCur++; dxc &= 3; }
    }
    sxc = sx & 3;
    dxc = dx & 3;
    srcLine = uint16_t(decY ? srcLine - srcW : srcLine + srcW);
    srcCur = srcLine;
    dstLine = uint16_t(dstLine + dstW);
    dstCur = dstLine;
  }
  ram[0x34] &= 0x7F;
  dmaDone++;
  cpu.irq(Sm8500::IRQ_DMA);
}

// ------------------------------------------------------------------ tempo
void GameCom::tick(int c, bool halted) {
  cycles += uint64_t(c);
  for (int i = 0; i < 2; i++) {
    Timer& t = tm[i];
    if (!t.on) continue;
    t.pre += uint32_t(c);
    while (t.pre >= t.preMax) {
      t.pre -= t.preMax;
      uint8_t& cnt = ram[0x51 + 2 * i];
      cnt++;
      if (cnt >= t.max) {
        cnt = 0;
        cpu.irq(i ? Sm8500::IRQ_TIM1 : Sm8500::IRQ_TIM0);
      }
    }
  }
  if (clkOn) {
    clkCount += uint32_t(c);
    if (clkCount >= clkPeriod) {
      clkCount -= clkPeriod;
      ram[0x54] = uint8_t((ram[0x54] & 0xC0) | ((ram[0x54] + 1) & 0x3F));
      cpu.irq(Sm8500::IRQ_CK);
    }
  }
  if (dmaPending) {
    dmaWait += uint32_t(c);
    if (halted || dmaWait >= 4096) runDma();
  }
  soundStep(c);
  lineCycle += c;
  while (lineCycle >= LINE_CYCLES) {
    lineCycle -= LINE_CYCLES;
    if (lineNo < SCREEN_W) drawLine(lineNo);
    lineNo++;
    if (lineNo == SCREEN_W) cpu.irq(Sm8500::IRQ_LCDC);  // começo do retraço
    if (lineNo >= LINES) { lineNo = 0; endFrame(); }
  }
}

// uma "linha" do LCD é uma coluna da tela: 40 bytes = 160 pixels de cima para baixo
void GameCom::drawLine(int l) {
  const uint8_t lcdc = ram[0x30];
  if (!(lcdc & 0x80)) {
    for (int y = 0; y < SCREEN_H; y++) fb[y * SCREEN_W + l] = SHADE[4];
    return;
  }
  static constexpr uint8_t PAL[4][4] = {{4, 3, 2, 0}, {4, 3, 1, 0}, {4, 3, 1, 0}, {4, 2, 1, 0}};
  const uint8_t* p = PAL[(lcdc >> 4) & 3];
  const uint8_t* line = vram + ((lcdc & 0x40) ? 0x2000 : 0) + 40 * l;
  for (int i = 0; i < 40; i++) {
    const uint8_t b = line[i];
    for (int k = 0; k < 4; k++) fb[(i * 4 + k) * SCREEN_W + l] = SHADE[p[(b >> (6 - 2 * k)) & 3]];
  }
}

void GameCom::endFrame() {
  frameDone = true;
  frameCount++;
  if (stylusMode) {  // cursor da caneta: uma cruz que inverte os pixels
    for (int d = -3; d <= 3; d++) {
      const int pts[2][2] = {{curX + d, curY}, {curX, curY + d}};
      for (const auto& q : pts) {
        if (q[0] < 0 || q[0] >= SCREEN_W || q[1] < 0 || q[1] >= SCREEN_H) continue;
        uint32_t& px = fb[q[1] * SCREEN_W + q[0]];
        px = (px & 0xFF000000u) | (~px & 0x00FFFFFFu);
      }
    }
  }
}

void GameCom::runFrame() {
  // caneta: KEY_L liga/desliga; o direcional move o cursor e A toca
  if ((keys & KEY_L) && !(prevKeys & KEY_L)) stylusMode = !stylusMode;
  prevKeys = keys;
  if (stylusMode) {
    const bool moving = keys & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
    holdFrames = moving ? holdFrames + 1 : 0;
    const int sp = holdFrames > 20 ? 4 : 2;
    if (keys & KEY_LEFT) curX -= sp;
    if (keys & KEY_RIGHT) curX += sp;
    if (keys & KEY_UP) curY -= sp;
    if (keys & KEY_DOWN) curY += sp;
    curX = std::clamp(curX, 0, SCREEN_W - 1);
    curY = std::clamp(curY, 0, SCREEN_H - 1);
  }
  penDown = stylusMode && (keys & KEY_A);
  penX = curX;
  penY = curY;
  if (ptrDown) { penDown = true; penX = ptrX; penY = ptrY; }
  frameDone = false;
  while (!frameDone) cpu.step();
}

// ------------------------------------------------------------------ som
void GameCom::soundStep(int c) {
  const uint8_t sgc = ram[0x40];
  for (int ch = 0; ch < 2; ch++) {
    const uint32_t t = uint32_t(ram[0x46 + 2 * ch] << 8 | ram[0x47 + 2 * ch]);
    if (!(sgc & 0x80) || !(sgc & (1 << ch)) || !t) continue;
    Tone& s = sg[ch];
    s.acc += uint32_t(c);
    const uint32_t period = 2 * t;  // 2.764.800 passos por segundo / SGnT
    while (s.acc >= period) { s.acc -= period; s.pos = uint8_t((s.pos + 1) & 31); }
  }
  const uint32_t t2 = uint32_t(ram[0x4C] << 8 | ram[0x4D]);
  if ((sgc & 0x84) == 0x84 && t2) {
    noiseAcc += uint32_t(c);
    while (noiseAcc >= 2 * t2) {
      noiseAcc -= 2 * t2;
      const uint16_t bit = uint16_t((lfsr ^ (lfsr >> 1)) & 1);
      lfsr = uint16_t((lfsr >> 1) | (bit << 14));
    }
  }
  sampleAcc += c;
  while (sampleAcc >= cyclesPerSample) {
    sampleAcc -= cyclesPerSample;
    emitSample();
  }
}

void GameCom::emitSample() {
  const uint8_t sgc = ram[0x40];
  double v = 0;
  if (sgc & 0x80) {
    for (int ch = 0; ch < 2; ch++) {
      if (!(sgc & (1 << ch))) continue;
      const uint8_t* w = ram + 0x60 + 0x10 * ch;
      const uint8_t pos = sg[ch].pos;
      const int nib = (w[pos >> 1] >> ((pos & 1) * 4)) & 0xF;
      v += (nib / 15.0 - 0.5) * (ram[0x42 + 2 * ch] & 0x1F) / 31.0 * 0.5;
    }
    if (sgc & 0x04) v += ((lfsr & 1) ? 0.25 : -0.25) * (ram[0x4A] & 0x1F) / 31.0;
    if (sgc & 0x08) v += (int(dacLevel) - 128) / 128.0 * 0.5;
  }
  const double out = v - hpCap;  // passa-alta (tira o nível contínuo)
  hpCap += out * 0.002;
  const float f = float(std::clamp(out, -1.0, 1.0));
  lastOut = f;
  bufL[wpos] = bufR[wpos] = f;
  wpos = (wpos + 1) % AUDIO_BUF;
  if (wpos == rpos) rpos = (rpos + 1) % AUDIO_BUF;
}

void GameCom::setSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  cyclesPerSample = double(CPU_HZ) / rate;
}

void GameCom::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { l[k] = 0; r[k] = 0; continue; }
    l[k] = bufL[rpos];
    r[k] = bufR[rpos];
    rpos = (rpos + 1) % AUDIO_BUF;
  }
}

// ------------------------------------------------------------------ save state
void GameCom::syncState(StateIO& s) {
  char magic[4];
  std::memcpy(magic, STATE_MAGIC, 4);
  s.raw(magic, 4);
  uint32_t crc = crc32(cart.data(), 0x8000);
  s.sync(crc);
  if (s.reading() && (std::memcmp(magic, STATE_MAGIC, 4) != 0 || crc != crc32(cart.data(), 0x8000)))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(s);
  s.raw(ram, sizeof ram);
  s.raw(vram, sizeof vram);
  s.raw(ext, sizeof ext);
  for (auto& b : bankBase) s.sync(b);
  s.sync(cartSelected); s.sync(muxLo); s.sync(muxHi);
  s.sync(cycles); s.sync(frameCount); s.sync(lineCycle); s.sync(lineNo);
  for (auto& t : tm) { s.sync(t.on); s.sync(t.pre); s.sync(t.preMax); s.sync(t.max); }
  s.sync(clkOn); s.sync(clkPeriod); s.sync(clkCount);
  s.sync(dmaPending); s.sync(dmaWait);
  s.sync(prevKeys); s.sync(stylusMode); s.sync(curX); s.sync(curY); s.sync(holdFrames);
  for (auto& t : sg) { s.sync(t.acc); s.sync(t.pos); }
  s.sync(noiseAcc); s.sync(lfsr); s.sync(dacLevel);
  s.sync(sampleAcc); s.sync(hpCap);
  if (s.reading()) {
    // limites contra arquivos danificados
    clampTo(lineCycle, 0, LINE_CYCLES - 1);
    clampTo(lineNo, 0, LINES - 1);
    for (auto& t : tm) {
      clampTo<uint32_t>(t.preMax, 1, 65536);
      t.pre %= t.preMax;
    }
    // acumuladores do som abaixo de um período (senão o laço de passos demora bilhões de voltas)
    for (int ch = 0; ch < 2; ch++) {
      const uint32_t t = uint32_t(ram[0x46 + 2 * ch] << 8 | ram[0x47 + 2 * ch]);
      sg[ch].acc = t ? sg[ch].acc % (2 * t) : 0;
    }
    const uint32_t t2 = uint32_t(ram[0x4C] << 8 | ram[0x4D]);
    noiseAcc = t2 ? noiseAcc % (2 * t2) : 0;
    if (clkOn) clampTo<uint32_t>(clkPeriod, 1, uint32_t(CPU_HZ) * 60);
    clampTo(curX, 0, SCREEN_W - 1);
    clampTo(curY, 0, SCREEN_H - 1);
    finiteOr(sampleAcc, 0);
    finiteOr(hpCap, 0);
    sampleAcc = std::clamp(sampleAcc, 0.0, cyclesPerSample);
    for (auto& t : sg) t.pos &= 31;
    extDirty = true;  // a RAM estendida veio do estado
  }
}

std::vector<uint8_t> GameCom::saveState() {
  StateIO s;
  syncState(s);
  return s.data();
}

void GameCom::loadState(const std::vector<uint8_t>& data) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO s(data);
    syncState(s);
  } catch (...) {
    StateIO s(backup);
    syncState(s);
    throw;
  }
  wpos = rpos = 0;
}

// ------------------------------------------------------------------ adaptador
GameComCore::GameComCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save) {
  const std::vector<uint8_t> ir = indigo::firmware("gamecom_int");
  const std::vector<uint8_t> er = indigo::firmware("gamecom_ext");
  if (ir.size() != INT_ROM_SIZE || er.size() != EXT_ROM_SIZE)
    throw std::runtime_error(
        "O Game.com precisa das duas ROMs do seu console: a interna (4 KB, internal.bin) e a do sistema "
        "(256 KB, external.bin). Abra cada arquivo no Índigo uma vez para instalá-las e depois abra o "
        "cartucho de novo.");
  m_ = std::make_unique<GameCom>(rom, ir, er);
  if (save) m_->importExt(*save);
}

std::string GameComCore::note() const {
  return "Game.com: L liga a caneta (direcional move, A toca na tela); C = X, D = Y, Menu = Select, "
         "Pause = Start, Sound = R.";
}

}  // namespace indigo::gamecom
