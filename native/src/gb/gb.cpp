#include "gb.h"

#include <algorithm>
#include <stdexcept>

namespace indigo::gb {

namespace {
constexpr int FZ = 0x80, FN = 0x40, FH = 0x20, FC = 0x10;
constexpr int DMA_DELAY = 1;  // 1 ciclo de partida antes do primeiro byte
const int TIMER_BITS[4] = {0x200, 0x8, 0x20, 0x80};
const char STATE_MAGIC[4] = {'I', 'G', 'B', '1'};
inline int sx8(int v) { return static_cast<int8_t>(v); }
inline int ctz(int v) { return __builtin_ctz(static_cast<unsigned>(v)); }
}  // namespace

GB::GB() : ppu(*this), apu(*this), wram(0x8000), hram(0x7F) {}

void GB::loadRom(const std::vector<uint8_t>& data, const std::vector<uint8_t>* saveBytes, Mode mode, int duckKind) {
  cart = std::make_unique<Cart>(data, duckKind);
  duck = duckKind != 0;
  const CartInfo& info = cart->info;
  cgb = duck ? false : mode == Mode::Cgb ? true : mode == Mode::Dmg ? false : info.cgb;
  if (info.cgbOnly) cgb = true;
  // Super Game Boy: só jogos com a marca no cabeçalho rodando como Game Boy clássico
  if (!cgb && mode != Mode::Dmg && sgbEnabled() && isSgbCart(cart->rom)) sgb = std::make_unique<SGB>();
  else sgb.reset();
  title = info.title;
  gameCode = info.code;
  if (saveBytes) cart->importSave(*saveBytes);
  romLoaded = true;
  reset();
}

void GB::reset() {
  std::fill(wram.begin(), wram.end(), 0); std::fill(hram.begin(), hram.end(), 0);
  ppu.reset();
  apu.reset();
  // registradores como o boot ROM deixa
  if (cgb) { a = 0x11; f = 0x80; b = 0x00; c = 0x00; d = 0xFF; e = 0x56; h = 0x00; l = 0x0D; }
  else if (sgb) { a = 0x01; f = 0x00; b = 0x00; c = 0x14; d = 0x00; e = 0x00; h = 0xC0; l = 0x60; }
  else { a = 0x01; f = 0xB0; b = 0x00; c = 0x13; d = 0x00; e = 0xD8; h = 0x01; l = 0x4D; }
  if (sgb) sgb->reset();
  sp = 0xFFFE; pc = 0x0100;
  if (duck) {
    // sem ROM de boot: a CPU começa em $0000 (registros aleatórios no aparelho; aqui zerados, para repetir igual)
    a = f = b = c = d = e = h = l = 0;
    pc = 0x0000;
  }
  ime = false; eiDelay = false; halted = false; haltBug = false; stopped = false; locked = false;
  ie = 0; if_ = 0x01;
  div = cgb ? 0x1EA0 : 0xABCC;
  tima = 0; tma = 0; tac = 0xF8; timaPending = false; timaReloaded = false;
  p1 = 0x30;
  sb = 0; sc = 0x7E; serialTimer = 0;
  dmaActive = false; dmaBlock = false; dmaDelay = 0; dmaSrc = 0; dmaIdx = 0; dmaReg = 0xFF;
  svbk = 1; key1 = 0; doubleSpeed = false;
  hdmaSrc = 0; hdmaDst = 0; hdmaLen = 0; hdmaActive = false; hdmaPending = false;
  std::fill(undoc, undoc + 4, 0);
  frameDots = 0;
  frame = 0;
  serialOut.clear();
}

void GB::setKeys(int mask) {
  const int pressed = mask & ~keys;
  keys = mask & 0xFF;
  if (pressed & 0xFF) { requestIrq(0x10); stopped = false; }
}

// ------------------------------------------------------------ tempo (1 ciclo de máquina)
void GB::tick() {
  const int dots = doubleSpeed ? 2 : 4;
  dotClock += dots;
  frameDots += dots;
  // timer: recarga atrasada do TIMA
  timaReloaded = false;
  if (timaPending) { timaPending = false; tima = tma; if_ |= 4; timaReloaded = true; }
  const int old = div, nw = (old + 4) & 0xFFFF;
  div = nw;
  if ((tac & 4) && (old & TIMER_BITS[tac & 3]) && !(nw & TIMER_BITS[tac & 3])) incTima();
  const int am = doubleSpeed ? 0x2000 : 0x1000;
  if ((old & am) && !(nw & am)) apu.frameStep();
  ppu.tick(dots);
  if (dmaActive) {
    if (dmaIdx >= 160) { dmaActive = false; dmaBlock = false; }
    else if (dmaDelay) dmaDelay--;
    else {
      const int s = dmaSrc + dmaIdx;
      dmaBlock = false;
      ppu.oam[dmaIdx] = static_cast<uint8_t>(read(s >= 0xE000 ? s - 0x2000 : s));
      dmaBlock = true;
      dmaIdx++;
    }
  }
  if (serialTimer && --serialTimer == 0) {
    sb = 0xFF; sc &= 0x7F; if_ |= 8;
  }
}

void GB::incTima() {
  tima = (tima + 1) & 0xFF;
  if (tima == 0) timaPending = true;
}

// ------------------------------------------------------------ memória (sem custo de tempo)
int GB::read(int addr) {
  if (addr < 0x8000) return cart->readRom(addr);
  if (addr < 0xA000) return ppu.vramLocked() ? 0xFF : ppu.vram[(ppu.vbk << 13) | (addr & 0x1FFF)];
  if (addr < 0xC000) return cart->readRam(addr);
  if (addr < 0xD000) return wram[addr & 0xFFF];
  if (addr < 0xE000) return wram[(svbk << 12) | (addr & 0xFFF)];
  if (addr < 0xF000) return wram[addr & 0xFFF];
  if (addr < 0xFE00) return wram[(svbk << 12) | (addr & 0xFFF)];
  if (addr < 0xFEA0) return (dmaBlock || ppu.oamLocked()) ? 0xFF : ppu.oam[addr & 0xFF];
  if (addr < 0xFF00) return 0x00;
  if (addr < 0xFF80) return readIO(addr);
  if (addr < 0xFFFF) return hram[addr & 0x7F];
  return ie;
}

void GB::write(int addr, int v) {
  if (addr < 0x8000) { cart->writeRom(addr, v); return; }
  if (addr < 0xA000) { if (!ppu.vramLocked()) ppu.vram[(ppu.vbk << 13) | (addr & 0x1FFF)] = static_cast<uint8_t>(v); return; }
  if (addr < 0xC000) { cart->writeRam(addr, v); return; }
  if (addr < 0xD000) { wram[addr & 0xFFF] = static_cast<uint8_t>(v); return; }
  if (addr < 0xE000) { wram[(svbk << 12) | (addr & 0xFFF)] = static_cast<uint8_t>(v); return; }
  if (addr < 0xF000) { wram[addr & 0xFFF] = static_cast<uint8_t>(v); return; }
  if (addr < 0xFE00) { wram[(svbk << 12) | (addr & 0xFFF)] = static_cast<uint8_t>(v); return; }
  if (addr < 0xFEA0) { if (!dmaBlock && !ppu.oamLocked()) ppu.oam[addr & 0xFF] = static_cast<uint8_t>(v); return; }
  if (addr < 0xFF00) return;
  if (addr < 0xFF80) { writeIO(addr, v); return; }
  if (addr < 0xFFFF) { hram[addr & 0x7F] = static_cast<uint8_t>(v); return; }
  ie = v;
}

// ------------------------------------------------------------ Mega Duck: registradores trocados de lugar
namespace {
// endereço do Mega Duck -> endereço do Game Boy (-1 = nada ali). Fonte: megaduck-info (bbbbbr, CC0).
int duckToGb(int a) {
  if (a < 0xFF10 || (a >= 0xFF30 && a < 0xFF40)) return a;  // joypad, serial, timer, IF; RAM de onda
  switch (a) {
    case 0xFF10: return 0xFF40; case 0xFF11: return 0xFF41; case 0xFF12: return 0xFF42; case 0xFF13: return 0xFF43;
    case 0xFF14: return 0xFF48; case 0xFF15: return 0xFF49; case 0xFF16: return 0xFF4A; case 0xFF17: return 0xFF4B;
    case 0xFF18: return 0xFF44; case 0xFF19: return 0xFF45; case 0xFF1A: return 0xFF46; case 0xFF1B: return 0xFF47;
    case 0xFF20: return 0xFF10; case 0xFF21: return 0xFF12; case 0xFF22: return 0xFF11; case 0xFF23: return 0xFF13;
    case 0xFF24: return 0xFF14; case 0xFF25: return 0xFF16; case 0xFF27: return 0xFF17; case 0xFF28: return 0xFF18;
    case 0xFF29: return 0xFF19; case 0xFF2A: return 0xFF1A; case 0xFF2B: return 0xFF1B; case 0xFF2C: return 0xFF1C;
    case 0xFF2D: return 0xFF1E; case 0xFF2E: return 0xFF1D;
    case 0xFF40: return 0xFF20; case 0xFF41: return 0xFF22; case 0xFF42: return 0xFF21; case 0xFF43: return 0xFF23;
    case 0xFF44: return 0xFF24; case 0xFF45: return 0xFF26; case 0xFF46: return 0xFF25;
  }
  return -1;
}
// LCDC: bits 0, 1, 2, 3 e 6 em outra ordem (o Duck tem BG em 6, objetos em 0, tamanho em 1, mapas em 2 e 3)
int lcdcFromDuck(int v) {
  return (v & 0xB0) | ((v >> 6) & 1) | ((v & 1) << 1) | ((v & 2) << 1) | ((v & 4) << 1) | ((v & 8) << 3);
}
int lcdcToDuck(int v) {
  return (v & 0xB0) | ((v & 1) << 6) | ((v >> 1) & 1) | ((v >> 1) & 2) | ((v >> 1) & 4) | ((v >> 3) & 8);
}
// dados do som: NR12, NR22, NR42 e NR43 com as metades trocadas; NR32 com o volume em outra ordem
int duckSoundData(int gbAddr, int v) {
  if (gbAddr == 0xFF12 || gbAddr == 0xFF17 || gbAddr == 0xFF21 || gbAddr == 0xFF22) return ((v << 4) | (v >> 4)) & 0xFF;
  if (gbAddr == 0xFF1C) return v ^ ((v << 1) & 0x40);
  return v;
}
}  // namespace

int GB::readIO(int addr) {
  if (!duck) return readGbIO(addr);
  const int g = duckToGb(addr);
  if (g < 0) return 0xFF;
  const int v = readGbIO(g);
  if (g == 0xFF40) return lcdcToDuck(v);
  return duckSoundData(g, v);
}

void GB::writeIO(int addr, int v) {
  if (!duck) { writeGbIO(addr, v); return; }
  const int g = duckToGb(addr);
  if (g < 0) return;
  writeGbIO(g, g == 0xFF40 ? lcdcFromDuck(v) : duckSoundData(g, v));
}

int GB::readGbIO(int addr) {
  PPU& p = ppu;
  switch (addr) {
    case 0xFF00: {
      int low = 0xF;
      if (sgb && sgb->multi()) {
        // vários controles: com P14 e P15 em 1 aparece o número do controle; só o 1º tem botões aqui
        if ((p1 & 0x30) == 0x30) return 0xC0 | 0x30 | (0xF - sgb->player);
        if (sgb->player) return 0xC0 | (p1 & 0x30) | 0xF;
      }
      if (!(p1 & 0x10)) low &= ~((keys >> 4) & 0xF);
      if (!(p1 & 0x20)) low &= ~(keys & 0xF);
      return 0xC0 | (p1 & 0x30) | low;
    }
    case 0xFF01: return sb;
    case 0xFF02: return sc | (cgb ? 0x7C : 0x7E);
    case 0xFF04: return div >> 8;
    case 0xFF05: return tima;
    case 0xFF06: return tma;
    case 0xFF07: return tac | 0xF8;
    case 0xFF0F: return if_ | 0xE0;
    case 0xFF40: return p.lcdc;
    case 0xFF41: return p.readStat();
    case 0xFF42: return p.scy;
    case 0xFF43: return p.scx;
    case 0xFF44: return p.ly;
    case 0xFF45: return p.lyc;
    case 0xFF46: return dmaReg;
    case 0xFF47: return p.bgp;
    case 0xFF48: return p.obp0;
    case 0xFF49: return p.obp1;
    case 0xFF4A: return p.wy;
    case 0xFF4B: return p.wx;
  }
  if (addr >= 0xFF10 && addr < 0xFF40) return apu.read(addr);
  if (!cgb) return 0xFF;
  switch (addr) {
    case 0xFF4D: return (doubleSpeed ? 0x80 : 0) | 0x7E | (key1 & 1);
    case 0xFF4F: return 0xFE | p.vbk;
    case 0xFF55: return hdmaActive ? ((hdmaLen - 1) & 0x7F) : hdmaLen ? 0x80 | ((hdmaLen - 1) & 0x7F) : 0xFF;
    case 0xFF68: return p.bcps | 0x40;
    case 0xFF69: return p.bgPal[p.bcps & 0x3F];
    case 0xFF6A: return p.ocps | 0x40;
    case 0xFF6B: return p.objPal[p.ocps & 0x3F];
    case 0xFF6C: return p.opri | 0xFE;
    case 0xFF70: return svbk | 0xF8;
    case 0xFF72: case 0xFF73: return undoc[addr - 0xFF72];
    case 0xFF74: return undoc[2];
    case 0xFF75: return undoc[3] | 0x8F;
  }
  return 0xFF;
}

void GB::writeGbIO(int addr, int v) {
  PPU& p = ppu;
  switch (addr) {
    case 0xFF00: p1 = v & 0x30; if (sgb) sgb->writeJoyp(p1, ppu.ly, ppu.lcdOn()); return;
    case 0xFF01: sb = v; return;
    case 0xFF02:
      sc = v;
      if ((v & 0x81) == 0x81) {
        // sem cabo: o byte enviado é registrado (testes) e o recebido é 0xFF
        if (serialOut.size() < 4096) serialOut += static_cast<char>(sb);
        serialTimer = (cgb && (v & 2)) ? 8 : 256;
      }
      return;
    case 0xFF04: {
      const int old = div;
      div = 0;
      if ((tac & 4) && (old & TIMER_BITS[tac & 3])) incTima();
      if (old & (doubleSpeed ? 0x2000 : 0x1000)) apu.frameStep();
      return;
    }
    case 0xFF05: if (!timaReloaded) { tima = v; timaPending = false; } return;
    case 0xFF06: tma = v; if (timaReloaded) tima = v; return;
    case 0xFF07: {
      const bool oldOut = (tac & 4) && (div & TIMER_BITS[tac & 3]);
      tac = v & 7;
      const bool newOut = (tac & 4) && (div & TIMER_BITS[tac & 3]);
      if (oldOut && !newOut) incTima();
      return;
    }
    case 0xFF0F: if_ = v & 0x1F; return;
    case 0xFF40: p.writeLcdc(v); return;
    case 0xFF41: p.writeStat(v); return;
    case 0xFF42: p.scy = v; return;
    case 0xFF43: p.scx = v; return;
    case 0xFF44: return;
    case 0xFF45: p.writeLyc(v); return;
    case 0xFF46: dmaReg = v; dmaSrc = v << 8; dmaIdx = 0; dmaDelay = DMA_DELAY; dmaActive = true; return;  // num reinício a OAM segue bloqueada
    case 0xFF47: p.bgp = v; return;
    case 0xFF48: p.obp0 = v; return;
    case 0xFF49: p.obp1 = v; return;
    case 0xFF4A: p.wy = v; return;
    case 0xFF4B: p.wx = v; return;
  }
  if (addr >= 0xFF10 && addr < 0xFF40) { apu.write(addr, v); return; }
  if (!cgb) return;
  switch (addr) {
    case 0xFF4D: key1 = v & 1; return;
    case 0xFF4F: p.vbk = v & 1; return;
    case 0xFF51: hdmaSrc = (hdmaSrc & 0xFF) | (v << 8); return;
    case 0xFF52: hdmaSrc = (hdmaSrc & 0xFF00) | (v & 0xF0); return;
    case 0xFF53: hdmaDst = (hdmaDst & 0xFF) | ((v & 0x1F) << 8); return;
    case 0xFF54: hdmaDst = (hdmaDst & 0x1F00) | (v & 0xF0); return;
    case 0xFF55:
      if (hdmaActive && !(v & 0x80)) { hdmaActive = false; return; }
      hdmaLen = (v & 0x7F) + 1;
      if (v & 0x80) {
        hdmaActive = true;
        if (!p.lcdOn() || p.mode == 0) hdmaPending = true;
      } else {
        // DMA geral: copia tudo de uma vez, com a CPU parada
        while (hdmaLen) hdmaBlock();
      }
      return;
    case 0xFF68: p.bcps = v & 0xBF; return;
    case 0xFF69: p.writeBcpd(v); return;
    case 0xFF6A: p.ocps = v & 0xBF; return;
    case 0xFF6B: p.writeOcpd(v); return;
    case 0xFF6C: p.opri = v & 1; return;
    case 0xFF70: svbk = (v & 7) ? (v & 7) : 1; return;
    case 0xFF72: case 0xFF73: case 0xFF74: undoc[addr - 0xFF72] = static_cast<uint8_t>(v); return;
    case 0xFF75: undoc[3] = static_cast<uint8_t>(v & 0x70); return;
  }
}

void GB::hdmaBlock() {
  const int bank = ppu.vbk << 13;
  for (int k = 0; k < 16; k++) {
    const int s = (hdmaSrc + k) & 0xFFFF;
    const int v = read(s >= 0xE000 ? s - 0x2000 : s);
    ppu.vram[bank | ((hdmaDst + k) & 0x1FFF)] = static_cast<uint8_t>(v);
  }
  hdmaSrc = (hdmaSrc + 16) & 0xFFFF;
  hdmaDst = (hdmaDst + 16) & 0x1FFF;
  hdmaLen--;
  if (!hdmaLen) hdmaActive = false;
  const int cycles = doubleSpeed ? 16 : 8;
  for (int k = 0; k < cycles; k++) tick();
}

// ------------------------------------------------------------ acessos com tempo
void GB::push(int v) {
  tick();
  sp = (sp - 1) & 0xFFFF; wr(sp, v >> 8);
  sp = (sp - 1) & 0xFFFF; wr(sp, v & 0xFF);
}
int GB::pop() {
  const int lo = rd(sp); sp = (sp + 1) & 0xFFFF;
  const int hi = rd(sp); sp = (sp + 1) & 0xFFFF;
  return lo | (hi << 8);
}

int GB::getR(int i) {
  switch (i) {
    case 0: return b; case 1: return c; case 2: return d; case 3: return e;
    case 4: return h; case 5: return l; case 6: return rd(hl()); default: return a;
  }
}
void GB::setR(int i, int v) {
  switch (i) {
    case 0: b = v; break; case 1: c = v; break; case 2: d = v; break; case 3: e = v; break;
    case 4: h = v; break; case 5: l = v; break; case 6: wr(hl(), v); break; default: a = v;
  }
}
int GB::getRR(int i) const { return i == 0 ? bc() : i == 1 ? de() : i == 2 ? hl() : sp; }
void GB::setRR(int i, int v) {
  if (i == 0) setBC(v); else if (i == 1) setDE(v); else if (i == 2) setHL(v); else sp = v & 0xFFFF;
}
bool GB::cond(int i) const {
  switch (i) { case 0: return !(f & FZ); case 1: return (f & FZ) != 0; case 2: return !(f & FC); default: return (f & FC) != 0; }
}

// ------------------------------------------------------------ ULA
void GB::alu(int op, int v) {
  const int x = a;
  switch (op) {
    case 0: case 1: {
      const int cy = op == 1 && (f & FC) ? 1 : 0;
      const int r = x + v + cy;
      f = ((r & 0xFF) ? 0 : FZ) | (((x & 0xF) + (v & 0xF) + cy) > 0xF ? FH : 0) | (r > 0xFF ? FC : 0);
      a = r & 0xFF; break;
    }
    case 2: case 3: case 7: {
      const int cy = op == 3 && (f & FC) ? 1 : 0;
      const int r = x - v - cy;
      f = ((r & 0xFF) ? 0 : FZ) | FN | (((x & 0xF) - (v & 0xF) - cy) < 0 ? FH : 0) | (r < 0 ? FC : 0);
      if (op != 7) a = r & 0xFF;
      break;
    }
    case 4: a = x & v; f = (a ? 0 : FZ) | FH; break;
    case 5: a = x ^ v; f = a ? 0 : FZ; break;
    case 6: a = x | v; f = a ? 0 : FZ; break;
  }
}
int GB::inc8(int v) { const int r = (v + 1) & 0xFF; f = (f & FC) | (r ? 0 : FZ) | ((v & 0xF) == 0xF ? FH : 0); return r; }
int GB::dec8(int v) { const int r = (v - 1) & 0xFF; f = (f & FC) | FN | (r ? 0 : FZ) | ((v & 0xF) == 0 ? FH : 0); return r; }
int GB::spOffset() {
  const int e8 = imm();
  const int s = sx8(e8);
  f = (((sp & 0xF) + (e8 & 0xF)) > 0xF ? FH : 0) | (((sp & 0xFF) + e8) > 0xFF ? FC : 0);
  return (sp + s) & 0xFFFF;
}

void GB::cb() {
  const int op = imm();
  const int r = op & 7, x = op >> 6, y = (op >> 3) & 7;
  int v = getR(r);
  if (x == 1) { f = (f & FC) | FH | ((v & (1 << y)) ? 0 : FZ); return; }
  if (x == 2) { setR(r, v & ~(1 << y)); return; }
  if (x == 3) { setR(r, v | (1 << y)); return; }
  int cy = 0;
  switch (y) {
    case 0: cy = v >> 7; v = ((v << 1) | cy) & 0xFF; break;
    case 1: cy = v & 1; v = (v >> 1) | (cy << 7); break;
    case 2: cy = v >> 7; v = ((v << 1) | ((f & FC) ? 1 : 0)) & 0xFF; break;
    case 3: cy = v & 1; v = (v >> 1) | ((f & FC) ? 0x80 : 0); break;
    case 4: cy = v >> 7; v = (v << 1) & 0xFF; break;
    case 5: cy = v & 1; v = (v >> 1) | (v & 0x80); break;
    case 6: v = ((v >> 4) | (v << 4)) & 0xFF; break;
    case 7: cy = v & 1; v >>= 1; break;
  }
  f = (v ? 0 : FZ) | (cy ? FC : 0);
  setR(r, v);
}

// ------------------------------------------------------------ execução
void GB::step() {
  if (hdmaPending && !halted) { hdmaPending = false; if (hdmaActive) hdmaBlock(); }
  if (halted || stopped || locked) {
    tick();
    if (locked) return;
    if (stopped) return;
    if (!(ie & if_ & 0x1F)) return;
    halted = false;
  }
  if (ime && (ie & if_ & 0x1F)) { interrupt(); return; }
  if (eiDelay) { ime = true; eiDelay = false; }

  const int op = rd(pc);
  if (haltBug) haltBug = false; else pc = (pc + 1) & 0xFFFF;
  exec(op);
}

void GB::interrupt() {
  ime = false;
  tick(); tick();
  sp = (sp - 1) & 0xFFFF; wr(sp, pc >> 8);
  // o vetor é escolhido depois do primeiro byte empilhado (se o IE mudou ali, o resultado muda)
  const int pend = ie & if_ & 0x1F;
  sp = (sp - 1) & 0xFFFF; wr(sp, pc & 0xFF);
  if (!pend) { pc = 0; tick(); return; }
  const int bit = pend & -pend;
  if_ &= ~bit;
  pc = 0x40 + 8 * ctz(bit);
  tick();
}

void GB::exec(int op) {
  // blocos regulares
  if (op >= 0x40 && op < 0x80) {
    if (op == 0x76) {  // HALT
      if (!ime && (ie & if_ & 0x1F)) haltBug = true;
      else halted = true;
      return;
    }
    const int v = getR(op & 7);
    setR((op >> 3) & 7, v);
    return;
  }
  if (op >= 0x80 && op < 0xC0) { const int v = getR(op & 7); alu((op >> 3) & 7, v); return; }

  switch (op) {
    case 0x00: return;
    case 0x01: case 0x11: case 0x21: case 0x31: { const int v = imm16(); setRR(op >> 4, v); return; }
    case 0x02: wr(bc(), a); return;
    case 0x12: wr(de(), a); return;
    case 0x22: { const int x = hl(); wr(x, a); setHL((x + 1) & 0xFFFF); return; }
    case 0x32: { const int x = hl(); wr(x, a); setHL((x - 1) & 0xFFFF); return; }
    case 0x0A: a = rd(bc()); return;
    case 0x1A: a = rd(de()); return;
    case 0x2A: { const int x = hl(); a = rd(x); setHL((x + 1) & 0xFFFF); return; }
    case 0x3A: { const int x = hl(); a = rd(x); setHL((x - 1) & 0xFFFF); return; }
    case 0x03: case 0x13: case 0x23: case 0x33: { const int i = op >> 4; setRR(i, (getRR(i) + 1) & 0xFFFF); tick(); return; }
    case 0x0B: case 0x1B: case 0x2B: case 0x3B: { const int i = op >> 4; setRR(i, (getRR(i) - 1) & 0xFFFF); tick(); return; }
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C: {
      const int i = (op >> 3) & 7; const int v = inc8(getR(i)); setR(i, v); return;
    }
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D: {
      const int i = (op >> 3) & 7; const int v = dec8(getR(i)); setR(i, v); return;
    }
    case 0x34: { const int x = hl(); const int v = inc8(rd(x)); wr(x, v); return; }
    case 0x35: { const int x = hl(); const int v = dec8(rd(x)); wr(x, v); return; }
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x3E: { const int v = imm(); setR((op >> 3) & 7, v); return; }
    case 0x36: { const int v = imm(); wr(hl(), v); return; }
    case 0x07: { const int cy = a >> 7; a = ((a << 1) | cy) & 0xFF; f = cy ? FC : 0; return; }
    case 0x0F: { const int cy = a & 1; a = (a >> 1) | (cy << 7); f = cy ? FC : 0; return; }
    case 0x17: { const int cy = a >> 7; a = ((a << 1) | ((f & FC) ? 1 : 0)) & 0xFF; f = cy ? FC : 0; return; }
    case 0x1F: { const int cy = a & 1; a = (a >> 1) | ((f & FC) ? 0x80 : 0); f = cy ? FC : 0; return; }
    case 0x08: { const int x = imm16(); wr(x, sp & 0xFF); wr((x + 1) & 0xFFFF, sp >> 8); return; }
    case 0x09: case 0x19: case 0x29: case 0x39: {
      const int x = hl(), v = getRR(op >> 4), r = x + v;
      f = (f & FZ) | (((x & 0xFFF) + (v & 0xFFF)) > 0xFFF ? FH : 0) | (r > 0xFFFF ? FC : 0);
      setHL(r & 0xFFFF); tick(); return;
    }
    case 0x10: {  // STOP
      imm();
      if (cgb && (key1 & 1)) { doubleSpeed = !doubleSpeed; key1 = 0; div = 0; }
      else stopped = true;
      return;
    }
    case 0x18: { const int e8 = sx8(imm()); pc = (pc + e8) & 0xFFFF; tick(); return; }
    case 0x20: case 0x28: case 0x30: case 0x38: {
      const int e8 = sx8(imm());
      if (cond((op >> 3) & 3)) { pc = (pc + e8) & 0xFFFF; tick(); }
      return;
    }
    case 0x27: {
      int x = a, fl = f;
      if (!(fl & FN)) {
        if ((fl & FC) || x > 0x99) { x += 0x60; fl |= FC; }
        if ((fl & FH) || (x & 0xF) > 9) x += 6;
      } else {
        if (fl & FC) x -= 0x60;
        if (fl & FH) x -= 6;
      }
      a = x & 0xFF;
      f = (fl & (FN | FC)) | (a ? 0 : FZ);
      return;
    }
    case 0x2F: a ^= 0xFF; f |= FN | FH; return;
    case 0x37: f = (f & FZ) | FC; return;
    case 0x3F: f = (f & (FZ | FC)) ^ FC; return;

    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
      tick();
      if (cond((op >> 3) & 3)) { pc = pop(); tick(); }
      return;
    case 0xC1: setBC(pop()); return;
    case 0xD1: setDE(pop()); return;
    case 0xE1: setHL(pop()); return;
    case 0xF1: { const int v = pop(); a = v >> 8; f = v & 0xF0; return; }
    case 0xC5: push(bc()); return;
    case 0xD5: push(de()); return;
    case 0xE5: push(hl()); return;
    case 0xF5: push((a << 8) | f); return;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA: {
      const int x = imm16();
      if (cond((op >> 3) & 3)) { pc = x; tick(); }
      return;
    }
    case 0xC3: pc = imm16(); tick(); return;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC: {
      const int x = imm16();
      if (cond((op >> 3) & 3)) { push(pc); pc = x; }
      return;
    }
    case 0xCD: { const int x = imm16(); push(pc); pc = x; return; }
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE: { const int v = imm(); alu((op >> 3) & 7, v); return; }
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: push(pc); pc = op & 0x38; return;
    case 0xC9: pc = pop(); tick(); return;
    case 0xD9: pc = pop(); tick(); ime = true; return;
    case 0xCB: cb(); return;
    case 0xE0: { const int x = 0xFF00 | imm(); wr(x, a); return; }
    case 0xF0: { const int x = 0xFF00 | imm(); a = rd(x); return; }
    case 0xE2: wr(0xFF00 | c, a); return;
    case 0xF2: a = rd(0xFF00 | c); return;
    case 0xE8: sp = spOffset(); tick(); tick(); return;
    case 0xF8: setHL(spOffset()); tick(); return;
    case 0xF9: sp = hl(); tick(); return;
    case 0xE9: pc = hl(); return;
    case 0xEA: { const int x = imm16(); wr(x, a); return; }
    case 0xFA: { const int x = imm16(); a = rd(x); return; }
    case 0xF3: ime = false; eiDelay = false; return;
    case 0xFB: eiDelay = true; return;
    default:
      // opcodes inexistentes travam a CPU
      locked = true;
  }
}

void GB::runFrame() {
  ppu.frameDone = false;
  frameDots = 0;
  const int limit = ppu.lcdOn() ? FRAME_DOTS + 456 : FRAME_DOTS;
  while (!ppu.frameDone && frameDots < limit) step();
  apu.catchUp();
  if (sgb) sgb->frameEnd(ppu.shade, ppu.lcdOn());
  frame++;
}

// ------------------------------------------------------------ save state
void GB::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  bool isCgb = cgb;
  io.sync(isCgb);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || isCgb != cgb))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  for (int* p : {&a, &f, &b, &c, &d, &e, &h, &l, &sp, &pc, &ie, &if_, &div, &tima, &tma, &tac, &p1, &sb, &sc,
                 &serialTimer, &dmaDelay, &dmaSrc, &dmaIdx, &dmaReg, &svbk, &key1, &hdmaSrc, &hdmaDst, &hdmaLen})
    io.sync(*p);
  for (bool* p : {&ime, &eiDelay, &halted, &haltBug, &stopped, &locked, &timaPending, &dmaActive, &doubleSpeed,
                  &hdmaActive, &hdmaPending})
    io.sync(*p);
  io.sync(dotClock);
  io.bytes(wram); io.bytes(hram);
  io.raw(undoc, sizeof undoc);
  ppu.sync(io);
  apu.sync(io);
  cart->sync(io);
  // bloco do Super Game Boy no fim (um estado antigo, sem ele, ainda carrega)
  if (sgb && !(io.reading() && io.atEnd())) sgb->sync(io);
  if (io.reading()) {
    for (int* p : {&a, &f, &b, &c, &d, &e, &h, &l, &ie, &tima, &tma, &tac, &sb, &sc, &dmaReg}) *p &= 0xFF;
    sp &= 0xFFFF; pc &= 0xFFFF; div &= 0xFFFF; if_ &= 0x1F; p1 &= 0x30; key1 &= 1;
    clampTo(serialTimer, 0, 256); clampTo(dmaDelay, 0, 8); dmaSrc &= 0xFF00; clampTo(dmaIdx, 0, 160);
    svbk = (svbk & 7) ? (svbk & 7) : 1;
    hdmaSrc &= 0xFFFF; hdmaDst &= 0x1FFF; clampTo(hdmaLen, 0, 128);
    clampTo<int64_t>(dotClock, 0, int64_t(1) << 60);
  }
}

std::vector<uint8_t> GB::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void GB::loadState(const std::vector<uint8_t>& s) {
  // carrega numa cópia primeiro: se o arquivo estiver errado, o jogo em andamento não é afetado
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
GBCore::GBCore(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, Mode mode, int duckKind)
    : gb_(std::make_unique<GB>()) {
  gb_->loadRom(rom, save, mode, duckKind);
}

int duckKind(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.size() < 0x100 || rom.size() > (4u << 20)) return 0;
  auto ends = [&](const char* x) { const size_t l = std::char_traits<char>::length(x); return n.size() >= l && n.compare(n.size() - l, l, x) == 0; };
  if (ends(".md0")) return 2;
  if (ends(".duck") || ends(".md1") || ends(".md2")) return 1;
  return 0;
}

void GBCore::setDmgPalette(const uint32_t* bg, const uint32_t* obj0, const uint32_t* obj1) {
  // mesma regra de setPalette em web/emu-core.js
  const uint32_t* o0 = obj0 ? obj0 : bg;
  const uint32_t* o1 = obj1 ? obj1 : o0;
  std::copy(bg, bg + 4, gb_->ppu.dmgBg);
  std::copy(o0, o0 + 4, gb_->ppu.dmgObj0);
  std::copy(o1, o1 + 4, gb_->ppu.dmgObj1);
}

std::string GBCore::note() const {
  return gb_->sgb ? "Super Game Boy: cores e moldura do jogo (os sons do SNES não tocam)." : "";
}

std::vector<uint8_t> GBCore::exportSave() {
  gb_->cart->dirty = false;
  return gb_->cart->hasSave() ? gb_->cart->exportSave() : std::vector<uint8_t>{};
}

}  // namespace indigo::gb
