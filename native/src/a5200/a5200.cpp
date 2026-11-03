// Máquina Atari 5200: mapa de memória, relógio (linha de 114 ciclos com os ciclos que o ANTIC rouba da CPU),
// ANTIC (lista de exibição e DMA ciclo a ciclo: cada byte é buscado e vira pontos no seu ciclo), GTIA (jogadores, mísseis, prioridade, colisões e a saída ponto a
// ponto: uma cor trocada no meio da linha aparece no meio da linha) e o som do POKEY.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "a5200.h"

namespace indigo::a5200 {

namespace {
constexpr int NMI_CYC = 8;      // ciclo da linha em que o ANTIC dá o NMI (DLI e VBI)
constexpr int WSYNC_CYC = 105;  // a CPU parada por WSYNC volta neste ciclo
constexpr int FIRST = 8;        // primeira linha da tela (e da lista de exibição)
constexpr int VBI_LINE = 248;   // começo do retraço vertical
constexpr int REFRESH = 25;     // primeiro dos 9 pedidos de refresh da memória (um a cada 4, até o 57)
constexpr int PF_END = 105;     // último ciclo em que a DMA do playfield toma o barramento

// Posição dos ciclos (a numeração do Altirra: mísseis no 0, instrução no 1, jogadores do 2 ao 5, endereço da
// lista no 6 e 7, refresh no 25, 29, ... 57, NMI no 8, fim do WSYNC no 105). A DMA do playfield segue o mapa
// medido no aparelho pela equipe do Atari800 (cycle_map.c, GPL-2+, só lido): nome do caractere no ciclo
// NAME0 + HSCROL/2 + k * intervalo, byte do bitmap 2 ciclos depois, desenho do caractere 3 ciclos depois.
// O refresh que cai num ciclo do playfield espera o próximo ciclo livre; se outro vence enquanto ele espera, um
// se perde (por isso a primeira linha do modo 2 tem 1 refresh, não 9).

// linhas por linha de modo e pontos de cor por byte lido (modos 2 a F)
constexpr uint8_t MODE_LINES[16] = {1, 1, 8, 10, 8, 16, 8, 16, 8, 4, 4, 2, 1, 2, 1, 1};
constexpr uint8_t CC_PER_BYTE[16] = {0, 0, 4, 4, 4, 4, 8, 8, 16, 16, 8, 8, 8, 4, 4, 4};
// larguras (DMACTL bits 0–1): começo em pontos de cor, largura, ciclo do começo da busca
constexpr int WSTART[4] = {0, 64, 48, 32};
constexpr int WSIZE[4] = {0, 128, 160, 192};
constexpr int NAME0[4] = {0, 26, 18, 10};

// paleta NTSC do GTIA: 16 matizes x 16 luminâncias (matiz 1 = fase da salva de cor, cada matiz gira 25,7 graus)
struct Palette {
  uint32_t c[256];
  Palette() {
    for (int h = 0; h < 16; h++) {
      for (int l = 0; l < 16; l++) {
        const double y = 0.04 + l / 15.0 * 0.92;
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
const Palette PAL;

inline int mult(uint8_t size) { return (size & 3) == 1 ? 2 : (size & 3) == 3 ? 4 : 1; }
}  // namespace

uint32_t A5200::color(uint8_t c) { return PAL.c[c]; }

A5200::A5200(Cart c, const std::vector<uint8_t>& b) : cart(std::move(c)), bios(b) {
  if (bios.size() != 0x800) throw std::runtime_error("BIOS do Atari 5200 inválida (precisa ter 2 KB).");
  cpu.bus = this;
  cpu.decimalMode = true;
  cpu.magic = 0xEE;
  pokey.ext = true;
  setSampleRate(48000);
  reset(true);
}

void A5200::runCycles(int n) {
  for (int i = 0; i < n; i++) tick();
}

void A5200::reset(bool power) {
  if (power) {
    std::memset(ram, 0, sizeof ram);
    cpu.powerOn();
    clk = cpuCycles = 0;
    audioM = 0; audioLevel = 0;
    sampleT = sampleV = capV = 0;
    frameCount = 0;
  }
  cart.reset();
  pokey.reset();
  for (int i = 0; i < 8; i++) pokey.pot[i] = 114;
  dmactl = chactl = hscrol = vscrol = pmbase = chbase = nmien = 0;
  nmist = 0x1F;
  dlist = memScan = 0;
  ir = 0; dctr = 0; needIr = true; prevVs = false; jvb = false; firstRow = true;
  dlActive = refPending = false; fetched = 0;
  std::memset(names, 0, sizeof names);
  for (int i = 0; i < 4; i++) { hposp[i] = hposm[i] = sizep[i] = grafp[i] = colpm[i] = colpf[i] = 0; mpf[i] = ppf[i] = mpl[i] = ppl[i] = 0; }
  sizem = grafm = colbk = prior = vdelay = gractl = consol = 0;
  trigLatch = 0;
  wsync = nmiLine = frameDone = false;
  openBus = 0xFF;
  line = 0; cyc = 0;
  startLine();
  cpu.reset();
}

// ---------------------------------------------------------------- controles
void A5200::setInput(uint8_t x, uint8_t y, bool t, bool sh, int key) {
  pokey.pot[0] = x;
  pokey.pot[1] = y;
  trig = static_cast<uint8_t>((trig & ~1) | (t ? 1 : 0));
  shiftKey = sh;
  keyCode = key;
}

void A5200::setKeys(uint32_t m) {
  // teclado numérico do PC (bits KEY_KP*): tem prioridade sobre os botões
  static const int digit[10] = {KP_0, KP_1, KP_2, KP_3, KP_4, KP_5, KP_6, KP_7, KP_8, KP_9};
  int kp = KP_NONE;
  for (int i = 0; i < 10 && kp == KP_NONE; i++)
    if (m & (KEY_KP0 << i)) kp = digit[i];
  if (kp == KP_NONE && (m & KEY_KPSTAR)) kp = KP_STAR;
  if (kp == KP_NONE && (m & KEY_KPHASH)) kp = KP_HASH;
  if (m & KEY_X) {  // "shift" do teclado: o controle fica parado e os botões viram teclas
    const bool u = m & KEY_UP, d = m & KEY_DOWN, l = m & KEY_LEFT, r = m & KEY_RIGHT;
    int k = KP_NONE;
    if (u) k = l ? KP_1 : r ? KP_3 : KP_2;
    else if (d) k = l ? KP_7 : r ? KP_9 : KP_8;
    else if (l) k = KP_4;
    else if (r) k = KP_6;
    else if (m & KEY_A) k = KP_5;
    else if (m & KEY_B) k = KP_0;
    else if (m & KEY_Y) k = KP_1;
    else if (m & KEY_L) k = KP_STAR;
    else if (m & KEY_R) k = KP_HASH;
    else if (m & KEY_START) k = KP_RESET;
    setInput(114, 114, false, false, kp != KP_NONE ? kp : k);
    return;
  }
  const uint8_t x = (m & KEY_LEFT) ? 6 : (m & KEY_RIGHT) ? 220 : 114;
  const uint8_t y = (m & KEY_UP) ? 6 : (m & KEY_DOWN) ? 220 : 114;
  int k = kp;
  if (k == KP_NONE) {
    if (m & KEY_START) k = KP_START;
    else if (m & KEY_SELECT) k = KP_PAUSE;
    else if (m & KEY_Y) k = KP_1;
    else if (m & KEY_L) k = KP_STAR;
    else if (m & KEY_R) k = KP_HASH;
  }
  setInput(x, y, m & KEY_A, m & KEY_B, k);
}

// ---------------------------------------------------------------- relógio
void A5200::tick() {
  clk++;
  if (++cyc == NMI_CYC) {
    if (dliAt >= 0) { nmist = 0x9F; if (nmien & 0x80) nmiLine = true; }
    if (vbiAt >= 0) { nmist = 0x5F; if (nmien & 0x40) nmiLine = true; }
  }
  if (cyc >= CYCLES) endLine();
  else anticCycle();
}

void A5200::beginCycle() {
  while (steal[cyc]) tick();
  cpuCycles++;
}

void A5200::endCycle() {
  tick();
  bool irq = false;
  if (pokey.irqEnabled()) {
    if (clk >= pokey.nextEvent()) audioSync();
    irq = pokey.irq();
  }
  cpu.endCycle(nmiLine, irq);
  nmiLine = false;
}

uint8_t A5200::read(uint16_t a) {
  beginCycle();
  const uint8_t v = memRead(a);
  openBus = v;
  endCycle();
  return v;
}

void A5200::write(uint16_t a, uint8_t v) {
  beginCycle();
  openBus = v;
  memWrite(a, v);
  endCycle();
  if (wsync) {  // a CPU fica parada até o ciclo 105 (desta linha ou da próxima)
    wsync = false;
    if (cyc > WSYNC_CYC) { const int l = line; while (line == l) tick(); }
    while (cyc < WSYNC_CYC) tick();
  }
}

void A5200::endLine() {
  renderTo(228);
  if (dlActive) endModeRow();
  int n = 0;
  for (int i = 0; i < CYCLES; i++) n += steal[i];
  stolenLast = n;
  stolenLine[line] = static_cast<uint8_t>(n);
  std::memcpy(stolenAt[line], steal, CYCLES);
  cyc = 0;
  if (++line >= LINES) { line = 0; frameDone = true; frameCount++; }
  startLine();
}

void A5200::startLine() {
  std::memset(steal, 0, sizeof steal);
  dliAt = vbiAt = -1;
  rendered = 0;
  dlActive = refPending = false;
  for (int n = 0; n < 4; n++) pTrig[n] = mTrig[n] = -1;
  lineVisible = line >= FIRST && line < VBI_LINE;
  if (line == VBI_LINE) vbiAt = NMI_CYC;
  if (line == FIRST) { needIr = true; jvb = false; prevVs = false; }
  if (lineVisible) { std::memset(pf, 0, sizeof pf); std::memset(hr, 0, sizeof hr); }
  anticCycle();
  if (line == 0) audioSync();
}

void A5200::runFrame() {
  frameDone = false;
  pokey.keyboard(keyCode, shiftKey);
  if (trig && (gractl & 4)) trigLatch |= trig;
  for (int guard = 0; !frameDone && guard < 200000; guard++) cpu.step();
  while (!frameDone) tick();
}

// ---------------------------------------------------------------- memória
uint8_t A5200::memRead(uint16_t a) {
  if (a < 0x4000) return ram[a];
  if (a < 0xC000) {
    if (cart.switches(a)) cart.access(a);
    return cart.read(a);
  }
  if (a < 0xD000) return gtiaRead(a & 0x1F);
  if (a >= 0xD400 && a < 0xD500) return anticRead(a & 0x0F);
  if (a >= 0xE800 && a < 0xF000) { audioSync(); return pokey.read(a & 0x0F, clk); }
  if (a >= 0xF800) return bios[a - 0xF800];
  return openBus;
}

void A5200::memWrite(uint16_t a, uint8_t v) {
  if (a < 0x4000) { ram[a] = v; return; }
  if (a < 0xC000) { if (cart.switches(a)) cart.access(a); return; }
  if (a < 0xD000) { gtiaWrite(a & 0x1F, v); return; }
  if (a >= 0xD400 && a < 0xD500) { anticWrite(a & 0x0F, v); return; }
  if (a >= 0xE800 && a < 0xF000) { audioSync(); pokey.write(a & 0x0F, v, clk); return; }
}

uint8_t A5200::dmaRead(uint16_t a) const {
  if (a < 0x4000) return ram[a];
  if (a < 0xC000) return cart.read(a);
  if (a >= 0xF800) return bios[a - 0xF800];
  return 0xFF;
}

uint8_t A5200::peek(uint16_t a) const { return dmaRead(a); }

void A5200::poke(uint16_t a, uint8_t v) { memWrite(a, v); }

// ---------------------------------------------------------------- ANTIC
uint8_t A5200::anticRead(int r) {
  switch (r) {
    case 0x0B: return static_cast<uint8_t>(line >> 1);  // VCOUNT
    case 0x0C: case 0x0D: return 0;                     // caneta ótica
    case 0x0F: return nmist;
    default: return 0xFF;
  }
}

void A5200::anticWrite(int r, uint8_t v) {
  switch (r) {
    case 0x0: dmactl = v & 0x3F; break;
    case 0x1: chactl = v & 7; break;
    case 0x2: dlist = static_cast<uint16_t>((dlist & 0xFF00) | v); break;
    case 0x3: dlist = static_cast<uint16_t>((dlist & 0x00FF) | (v << 8)); break;
    case 0x4: hscrol = v & 15; break;
    case 0x5: vscrol = v & 15; break;
    case 0x7: pmbase = v; break;
    case 0x9: chbase = v; break;
    case 0xA: wsync = true; break;
    case 0xE: nmien = v & 0xC0; break;
    case 0xF: nmist = 0x1F; break;  // NMIRES
    default: break;
  }
}

// o que o ANTIC faz no ciclo que começa agora (cyc): DMA de jogadores e mísseis, lista, playfield e refresh
void A5200::anticCycle() {
  const int c = cyc;
  if (lineVisible) {
    if (c == 0) pmDma(-1);
    else if (c == 1) dlFetch();
    else if (c <= 5) pmDma(c - 2);
    else if (dlActive && c >= NAME0[3]) pfCycle(c);
  }
  if (c >= REFRESH && c <= REFRESH + 32 && !((c - REFRESH) & 3)) refPending = true;
  // o refresh atrasado pega o primeiro ciclo livre, até o fim da linha (no modo 2 largo cai no 106); só um fica
  // pendente. Segue o Atari800; não foi conferido no aparelho se ele passa do ciclo 105
  if (refPending && !steal[c]) { steal[c] = 1; refPending = false; }
}

// jogadores e mísseis: mísseis no ciclo 0 (a DMA de jogadores liga também a dos mísseis), jogador n no 2 + n
void A5200::pmDma(int n) {
  const bool single = dmactl & 0x10;
  const int pl = single ? line : line >> 1;
  const uint16_t base = static_cast<uint16_t>(single ? (pmbase & 0xF8) << 8 : (pmbase & 0xFC) << 8);
  auto delayed = [&](int bit) { return !single && (vdelay & bit) && !(line & 1); };
  if (n < 0) {
    if (!(dmactl & 0x0C)) return;
    steal[0] = 1;
    if (gractl & 1) {
      const uint8_t d = dmaRead(static_cast<uint16_t>(base + (single ? 0x300 : 0x180) + pl));
      for (int k = 0; k < 4; k++)
        if (!delayed(1 << k)) grafm = static_cast<uint8_t>((grafm & ~(3 << (2 * k))) | (d & (3 << (2 * k))));
    }
    return;
  }
  if (!(dmactl & 0x08)) return;
  steal[2 + n] = 1;
  if ((gractl & 2) && !delayed(0x10 << n))
    grafp[n] = dmaRead(static_cast<uint16_t>(base + (single ? 0x400 + n * 0x100 : 0x200 + n * 0x80) + pl));
}

// ciclo 1: instrução nova (quando a linha de modo anterior acabou), endereço do LMS ou do salto (ciclos 6 e 7),
// primeira linha da rolagem vertical (VSCROL de agora) e o DLI previsto para esta linha
void A5200::dlFetch() {
  if (!(dmactl & 0x20)) return;  // sem a DMA da lista: fundo, e a lista fica parada
  if (jvb) {                     // esperando o retraço vertical (o DLI se repete em toda linha)
    if (ir & 0x80) dliAt = NMI_CYC;
    return;
  }
  auto dlByte = [&] {
    const uint8_t b = dmaRead(dlist);
    dlist = static_cast<uint16_t>((dlist & 0xFC00) | ((dlist + 1) & 0x3FF));  // contador de 10 bits
    return b;
  };
  dlActive = true;
  if (needIr) {
    needIr = false;
    ir = dlByte();
    steal[1] = 1;
    dctr = 0;
    fetched = 0;
    firstRow = true;
    const int m = ir & 15;
    if (m == 1 || (m >= 2 && (ir & 0x40))) {
      const uint8_t lo = dlByte(), hi = dlByte();
      steal[6] = steal[7] = 1;
      const uint16_t a = static_cast<uint16_t>(lo | (hi << 8));
      if (m == 1) dlist = a; else memScan = a;
    }
    if (m == 1 && (ir & 0x40)) {  // JVB
      jvb = true;
      dlActive = false;
      prevVs = false;
      if (ir & 0x80) dliAt = NMI_CYC;
      return;
    }
    if (m >= 2 && (ir & 0x20) && !prevVs) dctr = vscrol;
  }
  if ((ir & 0x80) && dctr == lastRow()) dliAt = NMI_CYC;
}

// última linha da linha de modo: a normal do modo (brancos: 1 a 8 linhas; salto: 1), ou VSCROL na linha de modo
// que fecha uma região rolada (qualquer instrução sem o bit de rolagem logo depois de uma com ele, até um JMP, que
// então ocupa VSCROL+1 linhas). Regra do Atari800 (antic.c) e do Altirra Hardware Reference Manual (ANTIC,
// rolagem vertical); não conferida no aparelho
int A5200::lastRow() const {
  const int m = ir & 15;
  const bool vs = m >= 2 && (ir & 0x20);
  if (!vs && prevVs) return vscrol;
  return m == 0 ? (ir >> 4) & 7 : m == 1 ? 0 : MODE_LINES[m] - 1;
}

// fim da linha da TV (ciclo 113): compara com o VSCROL deste momento; no fim da linha de modo o endereço da tela
// anda o número de bytes buscados na primeira linha
void A5200::endModeRow() {
  if (dctr == lastRow()) {
    needIr = true;
    prevVs = (ir & 15) >= 2 && (ir & 0x20);
    memScan = static_cast<uint16_t>((memScan & 0xF000) | ((memScan + fetched) & 0x0FFF));
    fetched = 0;
  } else {
    dctr = (dctr + 1) & 15;
  }
  firstRow = false;
}

// DMA do playfield no ciclo c, com os registradores de agora (DMACTL, HSCROL, CHBASE, CHACTL): nome do caractere
// (só na primeira linha), desenho do caractere 3 ciclos depois (toda linha), ou byte do bitmap 2 ciclos depois
// do nome (na primeira linha; nas outras o mesmo byte sai do buffer interno, sem DMA)
void A5200::pfCycle(int c) {
  const int mode = ir & 15, width = dmactl & 3;
  if (mode < 2 || !width) return;
  const bool hs = ir & 0x10;
  const int fw = hs ? std::min(width + 1, 3) : width;
  const int cpb = CC_PER_BYTE[mode], iv = cpb / 2, nb = WSIZE[fw] / cpb;
  const int start = NAME0[fw] + (hs ? hscrol >> 1 : 0);
  const int x0 = WSTART[fw] + (hs ? hscrol : 0);
  auto slot = [&](int off) {
    const int d = c - start - off;
    return (d < 0 || d % iv || d / iv >= nb) ? -1 : d / iv;
  };
  auto take = [&] { if (c <= PF_END) steal[c] = 1; };
  auto fetch = [&](int i) {
    names[i] = dmaRead(static_cast<uint16_t>((memScan & 0xF000) | ((memScan + fetched) & 0x0FFF)));
    fetched++;
    take();
  };
  if (mode < 8) {
    if (firstRow) { const int i = slot(0); if (i >= 0) fetch(i); }
    const int i = slot(3);
    if (i >= 0) { take(); emitByte(i, x0 + i * cpb); }
  } else {
    const int i = slot(2);
    if (i >= 0) { if (firstRow) fetch(i); emitByte(i, x0 + i * cpb); }
  }
}

// pontos do byte i (nome ou bitmap) na saída do ANTIC (pf/hr), a partir do ponto de cor x, cortados pela largura
void A5200::emitByte(int i, int x) {
  const int mode = ir & 15;
  const int width = dmactl & 3;
  const int lo = WSTART[width], hi = WSTART[width] + WSIZE[width];
  auto put = [&](int code) {
    if (x >= lo && x < hi && x < 232) pf[x] = static_cast<uint8_t>(code);
    x++;
  };
  auto putHr = [&](int bits) {  // 2 meios pontos de alta resolução
    if (x >= lo && x < hi && x < 232) { pf[x] = 5; hr[x] = static_cast<uint8_t>(bits); }
    x++;
  };
  const int r = dctr;
  const uint16_t cb = static_cast<uint16_t>((chbase & (mode < 6 ? 0xFC : 0xFE)) << 8);
  auto glyph = [&](uint8_t ch, int row) {
    if (chactl & 4) row = 7 - row;  // reflexo vertical
    return dmaRead(static_cast<uint16_t>(cb + (ch & (mode < 6 ? 0x7F : 0x3F)) * 8 + (row & 7)));
  };
  const uint8_t b = names[i];
  switch (mode) {
    case 2: case 3: {
      uint8_t d;
      if (mode == 2) d = glyph(b, r & 7);
      else {
        const bool desc = (b & 0x60) == 0x60;
        const int rr = r & 15;
        if (desc) d = rr < 2 ? 0 : rr < 8 ? glyph(b, rr) : rr < 10 ? glyph(b, rr - 8) : 0;
        else d = rr < 8 ? glyph(b, rr) : 0;
      }
      if (b & 0x80) {
        if (chactl & 1) d = 0;
        if (chactl & 2) d ^= 0xFF;
      }
      for (int k = 6; k >= 0; k -= 2) putHr((d >> k) & 3);
      break;
    }
    case 4: case 5: {
      const uint8_t d = glyph(b, mode == 4 ? (r & 7) : ((r >> 1) & 7));
      for (int k = 6; k >= 0; k -= 2) {
        const int v = (d >> k) & 3;
        put(v == 3 ? ((b & 0x80) ? 4 : 3) : v);
      }
      break;
    }
    case 6: case 7: {
      const uint8_t d = glyph(b, mode == 6 ? (r & 7) : ((r >> 1) & 7));
      for (int k = 7; k >= 0; k--) put(((d >> k) & 1) ? 1 + (b >> 6) : 0);
      break;
    }
    case 8: case 0xA: case 0xD: case 0xE: {  // 4 pontos de 2 bits
      const int w = mode == 8 ? 4 : mode == 0xA ? 2 : 1;
      for (int k = 6; k >= 0; k -= 2)
        for (int j = 0; j < w; j++) put((b >> k) & 3);
      break;
    }
    case 9: case 0xB: case 0xC: {  // 8 pontos de 1 bit (PF0)
      const int w = mode == 9 ? 2 : 1;
      for (int k = 7; k >= 0; k--)
        for (int j = 0; j < w; j++) put((b >> k) & 1);
      break;
    }
    default:  // F: 8 meios pontos
      for (int k = 6; k >= 0; k -= 2) putHr((b >> k) & 3);
      break;
  }
}

// ---------------------------------------------------------------- GTIA
uint8_t A5200::gtiaRead(int r) {
  if (r < 0x10) {
    renderTo(2 * cyc);
    switch (r >> 2) {
      case 0: return mpf[r & 3];
      case 1: return ppf[r & 3];
      case 2: return mpl[r & 3];
      default: return ppl[r & 3];
    }
  }
  if (r < 0x14) {  // TRIG0–3: 0 = apertado
    const int b = 1 << (r & 3);
    return ((trig | trigLatch) & b) ? 0 : 1;
  }
  return 0x0F;  // PAL (NTSC) e o resto
}

void A5200::gtiaWrite(int r, uint8_t v) {
  renderTo(2 * cyc);
  if (r < 4) { hposp[r] = v; return; }
  if (r < 8) { hposm[r - 4] = v; return; }
  if (r < 0x0C) { sizep[r - 8] = v & 3; return; }
  switch (r) {
    case 0x0C: sizem = v; break;
    case 0x0D: case 0x0E: case 0x0F: case 0x10: grafp[r - 0x0D] = v; break;
    case 0x11: grafm = v; break;
    case 0x12: case 0x13: case 0x14: case 0x15: colpm[r - 0x12] = v; break;
    case 0x16: case 0x17: case 0x18: case 0x19: colpf[r - 0x16] = v; break;
    case 0x1A: colbk = v; break;
    case 0x1B: prior = v; break;
    case 0x1C: vdelay = v; break;
    case 0x1D: gractl = v & 7; if (!(v & 4)) trigLatch = 0; break;
    case 0x1E: std::memset(mpf, 0, 4); std::memset(ppf, 0, 4); std::memset(mpl, 0, 4); std::memset(ppl, 0, 4); break;
    case 0x1F: consol = v; break;
    default: break;
  }
}

// combina o playfield com jogadores e mísseis até o ponto de cor "to" (exclusive) da linha atual
void A5200::renderTo(int to) {
  to = std::clamp(to, 0, 228);
  if (to <= rendered) return;
  if (!lineVisible) { rendered = to; return; }
  uint32_t* out = fb.data() + size_t(line - FIRST) * W;
  const int gm = prior >> 6;
  const bool fifth = prior & 0x10, multi = prior & 0x20;
  const bool PRI0 = prior & 1, PRI1 = prior & 2, PRI2 = prior & 4, PRI3 = prior & 8;
  const bool PRI01 = PRI0 || PRI1, PRI12 = PRI1 || PRI2, PRI23 = PRI2 || PRI3, PRI03 = PRI0 || PRI3;
  for (int cc = rendered; cc < to; cc++) {
    for (int n = 0; n < 4; n++) {
      if (cc == hposp[n]) pTrig[n] = static_cast<int16_t>(cc);
      if (cc == hposm[n]) mTrig[n] = static_cast<int16_t>(cc);
    }
    if (cc < 32 || cc >= 224) continue;
    const uint8_t code = pf[cc];
    const bool hires = code == 5;
    uint8_t pfm = hires ? 4 : code ? static_cast<uint8_t>(1 << (code - 1)) : 0;
    uint8_t coll = hires ? (hr[cc] ? 4 : 0) : pfm;
    int bk = colbk;
    bool gtiaPix = false;
    if (gm && hires) {  // modos do GTIA: 4 bits por 2 pontos de cor
      gtiaPix = true;
      const int v = (hr[cc & ~1] << 2) | hr[cc | 1];
      pfm = 0;
      if (gm == 1) bk = (colbk & 0xF0) | v;
      else if (gm == 3) bk = (v << 4) | (colbk & 0x0F);
      else if (v < 4) bk = colpm[v];
      else if (v < 8) pfm = static_cast<uint8_t>(1 << (v - 4));
      coll = pfm;
    }
    uint8_t p = 0, m = 0;
    for (int n = 0; n < 4; n++) {
      if (grafp[n] && pTrig[n] >= 0) {
        const int k = mult(sizep[n]), dx = cc - pTrig[n];
        if (dx >= 0 && dx < 8 * k && ((grafp[n] >> (7 - dx / k)) & 1)) p |= static_cast<uint8_t>(1 << n);
      }
      const int bits = (grafm >> (2 * n)) & 3;
      if (bits && mTrig[n] >= 0) {
        const int k = mult(static_cast<uint8_t>(sizem >> (2 * n))), dx = cc - mTrig[n];
        if (dx >= 0 && dx < 2 * k && ((bits >> (1 - dx / k)) & 1)) m |= static_cast<uint8_t>(1 << n);
      }
    }
    if (m) for (int n = 0; n < 4; n++) if (m & (1 << n)) { mpf[n] |= coll; mpl[n] |= p; }
    if (p) for (int n = 0; n < 4; n++) if (p & (1 << n)) { ppf[n] |= coll; ppl[n] |= static_cast<uint8_t>(p & ~(1 << n)); }
    // prioridade pelas equações do GTIA
    const uint8_t P = fifth ? p : static_cast<uint8_t>(p | m);
    if (fifth && m) pfm |= 8;
    const bool P0 = P & 1, P1 = P & 2, P2 = P & 4, P3 = P & 8;
    const bool PF0 = pfm & 1, PF1 = pfm & 2, PF2 = pfm & 4, PF3 = pfm & 8;
    const bool P01 = P0 || P1, P23 = P2 || P3, PF01 = PF0 || PF1, PF23 = PF2 || PF3;
    const bool SP0 = P0 && !(PF01 && PRI23) && !(PRI2 && PF23);
    const bool SP1 = P1 && !(PF01 && PRI23) && !(PRI2 && PF23) && (!P0 || multi);
    const bool SP2 = P2 && !P01 && !(PF23 && PRI12) && !(PF01 && !PRI0);
    const bool SP3 = P3 && !P01 && !(PF23 && PRI12) && !(PF01 && !PRI0) && (!P2 || multi);
    const bool SF3 = PF3 && !(P23 && PRI03) && !(P01 && !PRI2);
    const bool SF0 = PF0 && !(P23 && PRI0) && !(P01 && PRI01) && !SF3;
    const bool SF1 = PF1 && !(P23 && PRI0) && !(P01 && PRI01) && !SF3;
    const bool SF2 = PF2 && !(P23 && PRI03) && !(P01 && !PRI2);
    const bool SB = !P01 && !P23 && !PF01 && !PF23;
    int c = 0;
    if (SP0) c |= colpm[0];
    if (SP1) c |= colpm[1];
    if (SP2) c |= colpm[2];
    if (SP3) c |= colpm[3];
    if (SF0) c |= colpf[0];
    if (SF1) c |= colpf[1];
    if (SF2) c |= colpf[2];
    if (SF3) c |= colpf[3];
    if (SB) c |= bk;
    uint32_t* o = out + (cc - 32) * 2;
    if (gtiaPix && gm == 1 && SB) { o[0] = o[1] = PAL.c[c & 0xFF]; continue; }
    if (hires && !gtiaPix) {
      const int lit = (c & 0xF0) | (colpf[1] & 0x0E);
      o[0] = PAL.c[((hr[cc] & 2) ? lit : c) & 0xFE];
      o[1] = PAL.c[((hr[cc] & 1) ? lit : c) & 0xFE];
    } else {
      o[0] = o[1] = PAL.c[c & 0xFE];
    }
  }
  rendered = to;
}

// ---------------------------------------------------------------- som
void A5200::setSampleRate(int rate) { samplePeriod = 1789772.5 / std::max(rate, 8000); }

void A5200::integrate(int lvl, uint64_t dt) {
  const double v = lvl * (0.8 / 60.0);
  double d = static_cast<double>(dt);
  while (sampleT + d >= samplePeriod) {
    const double take = samplePeriod - sampleT;
    const double o = (sampleV + v * take) / samplePeriod - capV;
    capV += o * 0.0015;  // capacitor de saída (tira o nível contínuo)
    buf[wpos] = static_cast<float>(o);
    wpos = (wpos + 1) % kBuf;
    if (wpos == rpos) rpos = (rpos + 1) % kBuf;
    d -= take;
    sampleT = 0;
    sampleV = 0;
  }
  sampleT += d;
  sampleV += v * d;
}

void A5200::audioSync() {
  while (audioM < clk) {
    uint64_t nxt = clk;
    const uint64_t pe = pokey.nextEvent();
    if (pe < nxt) nxt = std::max(pe, audioM);
    integrate(audioLevel, nxt - audioM);
    audioM = nxt;
    pokey.fire(audioM);
    audioLevel = pokey.level();
  }
}

void A5200::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { l[k] = r[k] = 0; continue; }
    l[k] = r[k] = buf[rpos];
    rpos = (rpos + 1) % kBuf;
  }
}

// ---------------------------------------------------------------- estados
void A5200::sync(StateIO& io) {
  cpu.sync(io);
  cart.sync(io);
  io.raw(ram, sizeof ram);
  io.sync(openBus);
  io.sync(clk); io.sync(cpuCycles); io.sync(line); io.sync(cyc); io.sync(frameCount);
  io.raw(steal, sizeof steal);
  for (uint8_t& c : steal) c = c != 0;
  io.sync(wsync); io.sync(nmiLine); io.sync(dliAt); io.sync(vbiAt);
  io.sync(dmactl); io.sync(chactl); io.sync(hscrol); io.sync(vscrol); io.sync(pmbase); io.sync(chbase);
  io.sync(nmien); io.sync(nmist); io.sync(dlist); io.sync(memScan); io.sync(ir); io.sync(dctr);
  io.sync(needIr); io.sync(prevVs); io.sync(jvb); io.sync(firstRow); io.sync(dlActive); io.sync(refPending);
  io.sync(fetched);
  io.raw(names, sizeof names); io.raw(pf, sizeof pf); io.raw(hr, sizeof hr); io.sync(lineVisible);
  io.raw(hposp, 4); io.raw(hposm, 4); io.raw(sizep, 4); io.sync(sizem); io.raw(grafp, 4); io.sync(grafm);
  io.raw(colpm, 4); io.raw(colpf, 4); io.sync(colbk); io.sync(prior); io.sync(vdelay); io.sync(gractl);
  io.sync(consol); io.raw(mpf, 4); io.raw(ppf, 4); io.raw(mpl, 4); io.raw(ppl, 4); io.sync(trigLatch);
  io.sync(rendered);
  for (int n = 0; n < 4; n++) { io.sync(pTrig[n]); io.sync(mTrig[n]); }
  pokey.sync(io);
  pokey.syncExt(io);
  io.sync(audioM); io.sync(audioLevel); io.sync(sampleT); io.sync(sampleV); io.sync(capV);
  if (io.reading()) {
    // só valores que o aparelho não pode ter são trocados: um estado válido volta byte a byte igual
    if (line < 0 || line >= LINES) line = 0;
    if (cyc < 0 || cyc >= CYCLES) cyc = 0;
    if (dliAt != NMI_CYC) dliAt = -1;
    if (vbiAt != NMI_CYC) vbiAt = -1;
    lineVisible = line >= FIRST && line < VBI_LINE;
    if (rendered < 0 || rendered > 228) rendered = 0;
    // o disparo é sempre num ponto já desenhado (pTrig < rendered); outro valor viria de estado adulterado
    for (int n = 0; n < 4; n++) {
      if (pTrig[n] < -1 || pTrig[n] >= rendered) pTrig[n] = -1;
      if (mTrig[n] < -1 || mTrig[n] >= rendered) mTrig[n] = -1;
    }
    for (uint8_t& v : pf) if (v > 5) v = 0;
    for (uint8_t& v : hr) v &= 3;
    dctr &= 15; hscrol &= 15; vscrol &= 15; chactl &= 7; gractl &= 7; dmactl &= 0x3F;
    if (audioM > clk || clk - audioM > 4 * CYCLES * LINES) audioM = clk;
    if (audioLevel < 0 || audioLevel > 60) audioLevel = 0;
    finiteOr(sampleT, 0); finiteOr(sampleV, 0); finiteOr(capV, 0);
    if (sampleT < 0 || sampleT >= samplePeriod) { sampleT = 0; sampleV = 0; }
    if (std::fabs(sampleV) > 1e6) sampleV = 0;
    if (std::fabs(capV) > 1e6) capV = 0;
    pokey.sanitize(audioM);
  }
}

std::vector<uint8_t> A5200::saveState() {
  audioSync();
  StateIO io;
  uint32_t magic = 0x31303235;  // "5201": formato da 2.5 (ANTIC ciclo a ciclo)
  io.sync(magic);
  sync(io);
  return io.data();
}

void A5200::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  uint32_t magic = 0;
  io.sync(magic);
  if (magic == 0x30303235)
    throw std::runtime_error("Este estado é de uma versão anterior do núcleo do Atari 5200 (o ANTIC mudou).");
  if (magic != 0x31303235) throw std::runtime_error("Este estado não é de Atari 5200.");
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
Cart loadCart(const std::vector<uint8_t>& rom, const std::string& n) {
  Cart c;
  std::string err;
  if (!c.load(rom, n, err)) throw std::runtime_error(err);
  return c;
}
std::vector<uint8_t> needBios() {
  std::vector<uint8_t> b = firmware("a5200");
  if (b.size() != 0x800)
    throw std::runtime_error("o Atari 5200 precisa da BIOS do console. Abra o arquivo da BIOS (5200.rom, 2 KB) "
                             "uma vez no Índigo.");
  return b;
}
}  // namespace

A5200Core::A5200Core(const std::vector<uint8_t>& rom, const std::string& n, const std::vector<uint8_t>*)
    : m_(std::make_unique<A5200>(loadCart(rom, n), needBios())) {}

}  // namespace indigo::a5200
