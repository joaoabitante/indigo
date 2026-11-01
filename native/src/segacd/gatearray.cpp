// Sega CD: gate array, pelos mapas de registradores do "Mega-CD Hardware Manual".
//
// Lado principal (A12000-A1202F):
//   00 hi: bit 7 IEN2 (cópia da máscara do sub), bit 0 IFL2 (pede a interrupção 2 do sub); lo: bit 1 SBRQ, bit 0 SRES
//   02 hi: WP (proteção da PRG-RAM em blocos de 512 bytes); lo: bits 6-7 BK (banco da PRG-RAM), 2 MODE, 1 DMNA, 0 RET
//   04 hi: EDT, DSR e DD (destino dos dados do CDC); 06: vetor da interrupção horizontal; 08: dados do CDC (host)
//   0A: endereço do DMA do CDC; 0C: cronômetro; 0E: sinalizadores (hi do principal, lo do sub)
//   10-1F: palavras de comando (escritas pelo principal); 20-2F: palavras de estado (escritas pelo sub)
// Lado do sub (FF8000-FF81FF): os mesmos e mais
//   00 hi: LEDs; lo: bit 0 RES0 (0 reinicia os periféricos); 02 lo: bits 3-4 PM (prioridade de escrita)
//   04 lo: endereço do registrador do CDC; 06 lo: dado do registrador do CDC
//   30: temporizador (passos de 30,72 us, interrupção 3); 32: máscara das interrupções 1-6
//   34: fader do CD-DA (bits 4-14); 36: bit 8 DM (som do CD-DA mudo), bit 2 HOCK (interrupção 4 do drive)
//   38-41: estado do drive RS0-RS9; 42-4B: comando do drive CS0-CS9 (escrever 4A/4B envia)
//   4C: cores da fonte (bits 4-7 para bits 1, 0-3 para bits 0); 4E: padrão de 16 bits; 50-57: pontos gerados
//   58-67: ASIC (asic.cpp); 100-1FF: buffer de subcódigo
// Também o DMA do CDC: o LC8951 entrega os bytes e o gate array os leva ao destino escolhido no DD (2 = host do
// principal, 3 = host do sub, 4 = RAM do PCM, 5 = PRG-RAM, 7 = Word RAM), em rajadas de 8 bytes.
#include <algorithm>
#include <cstring>

#include "segacd.h"

namespace indigo::segacd {

namespace {
constexpr int64_t TICK_30US = 384 * 4;          // 30,72 us = 384 ciclos do sub-CPU (temporizador e cronômetro)
constexpr int64_t DRIVE_FRAME3 = 2000000;       // 1/75 s em terços de ciclo SCD (50 MHz / 75 * 3)
constexpr int64_t DMA_BYTE = 16;                // ciclos SCD por byte do DMA do CDC
}  // namespace

// ------------------------------------------------------------ tempo
int64_t SegaCD::now() const { return subClk_ + static_cast<int64_t>(sub.cycles) * 4; }
int64_t SegaCD::mainNow() const { return md_.cpuTime() * scdPerLine_ / 3420; }

void SegaCD::runTo(int64_t mclk) {
  const int64_t t = mclk * scdPerLine_ / 3420;
  advance(t);
  runTransfer(t);
  asicRun(t);
}

int64_t SegaCD::nextEvent(int64_t target) const {
  int64_t e = target;
  if (timerAt_) e = std::min(e, timerAt_);
  e = std::min(e, (driveAt3_ + 2) / 3);
  e = std::min(e, asicDeadline());
  if (xfer_) e = std::min(e, transferDeadline());
  return e;
}

void SegaCD::advance(int64_t target) {
  while (subClk_ < target) {
    int64_t stop = nextEvent(target);
    if (stop <= subClk_) stop = subClk_ + 1;
    execute(stop);
    if (timerAt_ && subClk_ >= timerAt_) {
      const int64_t period = timerReload_ * TICK_30US;
      if (!period) timerAt_ = 0;
      else if ((timerAt_ += period) <= subClk_) timerAt_ = subClk_ + period;
      request(3);
    }
    while (subClk_ * 3 >= driveAt3_) {
      driveAt3_ += DRIVE_FRAME3;
      driveTick();
    }
    if (subClk_ >= asicDeadline()) asicRun(subClk_);
    if (xfer_ && subClk_ >= transferDeadline()) runTransfer(subClk_);
  }
}

void SegaCD::execute(int64_t stop) {
  if (subHalt_ || subWaiting_) { subClk_ = stop; return; }
  if (subReset_) {
    subReset_ = false;
    sub.cycles = 0;
    sub.reset();
    subClk_ += static_cast<int64_t>(sub.cycles) * 4;
    sub.cycles = 0;
  }
  while (subClk_ < stop) {
    sub.ipl = ipl_;
    if (sub.stopped && ipl_ <= sub.mask) { subClk_ = stop; return; }
    // com a Word RAM 2M nas mãos do principal, uma instrução do sub pode ter de esperar: guarda o estado para
    // refazer a instrução quando a memória voltar
    struct Regs {
      uint32_t d[8], a[8], otherSp, pc; uint16_t ir, irc; int x, n, z, v, c, s, t, mask; bool stopped;
    } keep;
    const bool mayWait = !mode1M_ && ret_;
    if (mayWait) {
      std::memcpy(keep.d, sub.d, sizeof keep.d); std::memcpy(keep.a, sub.a, sizeof keep.a);
      keep.otherSp = sub.otherSp; keep.pc = sub.pc; keep.ir = sub.ir; keep.irc = sub.irc;
      keep.x = sub.fx; keep.n = sub.fn; keep.z = sub.fz; keep.v = sub.fv; keep.c = sub.fc;
      keep.s = sub.sMode; keep.t = sub.tMode; keep.mask = sub.mask; keep.stopped = sub.stopped;
    }
    int c;
    try {
      c = sub.step();
    } catch (const SubWait&) {
      std::memcpy(sub.d, keep.d, sizeof keep.d); std::memcpy(sub.a, keep.a, sizeof keep.a);
      sub.otherSp = keep.otherSp; sub.pc = keep.pc; sub.ir = keep.ir; sub.irc = keep.irc;
      sub.fx = keep.x; sub.fn = keep.n; sub.fz = keep.z; sub.fv = keep.v; sub.fc = keep.c;
      sub.sMode = keep.s; sub.tMode = keep.t; sub.mask = keep.mask; sub.stopped = keep.stopped;
      sub.cycles = 0;
      subWaiting_ = true;
      subClk_ = stop;
      return;
    }
    subClk_ += static_cast<int64_t>(c > 0 ? c : 4) * 4;
    sub.cycles = 0;
  }
}

// ------------------------------------------------------------ interrupções
void SegaCD::refreshIpl() {
  const int m = pending_ & mask_ & 0x7E;
  int lv = 0;
  for (int i = 6; i >= 1 && !lv; i--) if (m & (1 << i)) lv = i;
  ipl_ = lv;
  sub.ipl = lv;
}

void SegaCD::request(int level) {
  if (!(mask_ & (1 << level))) return;
  pending_ |= static_cast<uint8_t>(1 << level);
  refreshIpl();
}

void SegaCD::acknowledge(int level) {
  pending_ &= static_cast<uint8_t>(~(1 << level));
  if (level == 2) ifl2_ = false;
  refreshIpl();
}

void SegaCD::cdcInterruptCheck(bool wasActive, int64_t t) {
  if (!wasActive && cdc_.interruptActive(t)) request(5);  // o gate array reage à borda do /INT do LC8951
}

void SegaCD::setTimer(uint8_t v) {
  timerReload_ = v;
  timerAt_ = v ? now() + v * TICK_30US : 0;
}

void SegaCD::setMask(uint8_t v) {
  mask_ = v;
  if (!(v & 2)) pending_ &= static_cast<uint8_t>(~2);  // desligar a interrupção do ASIC descarta a pendente
  refreshIpl();
}

uint16_t SegaCD::stopwatch(int64_t t) const { return static_cast<uint16_t>(((t - stopwatchZero_) / TICK_30US) & 0xFFF); }

// ------------------------------------------------------------ drive -> CDC
void SegaCD::driveTick() {
  const CdDrive::Frame f = drive_.tick();
  const int64_t t = now();
  if (f.toDecoder) {
    uint8_t raw[2352];
    const uint8_t* data = nullptr;
    if (f.dataLba >= 0 && cdc_.wantsSectorData()) {
      if (!disc_->readRaw(static_cast<uint32_t>(f.dataLba), raw)) std::memset(raw, 0, sizeof raw);
      data = raw;
    }
    const bool was = cdc_.interruptActive(t);
    cdc_.sectorArrived(f.header, data, t);
    cdcInterruptCheck(was, t);
  }
  // a interrupção 4 fica pendente mesmo mascarada (aparece quando a máscara liga)
  if (hock_) {
    pending_ |= 1 << 4;
    refreshIpl();
  }
}

// ------------------------------------------------------------ CDC: registradores e transferência
void SegaCD::cdcWrite(uint8_t v) {
  const int64_t t = now();
  const bool was = cdc_.interruptActive(t);
  switch (cdc_.writeRegister(v, t)) {
    case Lc8951::Effect::StopTransfer: xfer_ = xferHeld_ = TO_NONE; break;
    case Lc8951::Effect::StartTransfer:
      edt_ = dsr_ = 0;
      beginTransfer();
      xferClk_ = t;
      break;
    case Lc8951::Effect::ChipReset:
      xfer_ = xferHeld_ = TO_NONE;
      pending_ &= static_cast<uint8_t>(~(1 << 5));
      refreshIpl();
      return;
    default: break;
  }
  cdcInterruptCheck(was, t);
}

uint8_t SegaCD::cdcRead() { return cdc_.readRegister(now()); }

void SegaCD::setCdcMode(uint8_t dd) {
  edt_ = dsr_ = 0;
  dd_ = dd & 7;
  runTransfer(now());
  beginTransfer();
  dmaAddr_ = 0;
}

void SegaCD::beginTransfer() {
  if (!cdc_.outputEnabled()) return;
  xfer_ = xferHeld_ = TO_NONE;
  switch (dd_) {
    case 2: case 3:  // host: a primeira palavra já fica pronta no registrador 08
      hostData_ = cdc_.outWord();
      dsr_ = 1;
      if (cdc_.countDone()) finishTransfer(now(), false);
      break;
    case 4: xfer_ = TO_PCM; break;
    case 5: (resetBusreq_ & 2 ? xferHeld_ : xfer_) = TO_PRG; break;  // PRG-RAM emprestada ao principal: espera
    case 7:
      if (mode1M_) xfer_ = subHalf() ? TO_WRAM_B : TO_WRAM_A;
      else (ret_ ? xferHeld_ : xfer_) = TO_WRAM_2M;
      break;
    default: break;
  }
}

void SegaCD::finishTransfer(int64_t when, bool dma) {
  const bool was = cdc_.interruptActive(when);
  cdc_.endTransfer(when);
  edt_ = 1;
  if (dma) dsr_ = 0;
  cdcInterruptCheck(was, when);
}

uint16_t SegaCD::hostRead(uint8_t who) {
  const uint16_t v = hostData_;
  if (dd_ == who && dsr_) {
    if (edt_) dsr_ = 0;
    else {
      hostData_ = cdc_.outWord();
      if (cdc_.countDone()) finishTransfer(now(), false);
    }
  }
  return v;
}

void SegaCD::moveBytes(int n) {
  if (n <= 0) return;
  if (xfer_ == TO_PCM) {  // endereço em unidades de 4 bytes na janela de 4 KB do banco de ondas
    pcmRun(now());
    uint32_t dst = (static_cast<uint32_t>(dmaAddr_) << 2) & 0xFFF;
    dmaAddr_ = static_cast<uint16_t>(dmaAddr_ + (n >> 2));
    uint8_t* bank = &pcm_.ram[static_cast<size_t>(pcm_.waveBank()) << 12];
    for (int i = 0; i < n; i++) { bank[dst] = cdc_.outByte(); dst = (dst + 1) & 0xFFF; }
    return;
  }
  // PRG-RAM e Word RAM: palavras, endereço em unidades de 8 bytes
  const int words = n >> 1;
  uint32_t dst = static_cast<uint32_t>(dmaAddr_) << 3;
  dmaAddr_ = static_cast<uint16_t>(dmaAddr_ + (words >> 2));
  for (int i = 0; i < words; i++, dst += 2) {
    const uint16_t v = cdc_.outWord();
    uint32_t o;
    if (xfer_ == TO_PRG) { o = dst & 0x7FFFE; prg_[o] = static_cast<uint8_t>(v >> 8); prg_[o + 1] = static_cast<uint8_t>(v); continue; }
    if (xfer_ == TO_WRAM_2M) o = dst & 0x3FFFE;
    else o = interleave(xfer_ == TO_WRAM_A ? 0 : 1, dst & 0x1FFFE);
    wram_[o] = static_cast<uint8_t>(v >> 8);
    wram_[o + 1] = static_cast<uint8_t>(v);
  }
}

int64_t SegaCD::transferDeadline() const {
  const int64_t bytes = (cdc_.bytesLeft() + 7) / 8 * 8;
  return xferClk_ + (bytes - 1) * DMA_BYTE + 1;
}

void SegaCD::runTransfer(int64_t t) {
  if (!xfer_ || t <= xferClk_) return;
  const int64_t due = (t - xferClk_ + DMA_BYTE - 1) / DMA_BYTE / 8 * 8;  // rajadas de 8 bytes já completas
  const int left = cdc_.bytesLeft();
  if (left <= due) {
    moveBytes(left);
    xferClk_ += left * DMA_BYTE;
    xfer_ = xferHeld_ = TO_NONE;
    finishTransfer(xferClk_, true);
  } else if (due > 0) {
    moveBytes(static_cast<int>(due));
    xferClk_ += due * DMA_BYTE;
  }
}

// ------------------------------------------------------------ registradores: principal
uint8_t SegaCD::mainModeByte() const {
  return static_cast<uint8_t>((bank_ << 6) | (mode1M_ << 2) | (dmna_ << 1) | ret_);
}
uint8_t SegaCD::subModeByte() const {
  return static_cast<uint8_t>((pm_ << 3) | (mode1M_ << 2) | (dmna_ << 1) | ret_);
}

uint8_t SegaCD::mainRegRead8(uint32_t a) {
  sync68k();
  if ((a & 0xFFFF00) != 0xA12000) return 0xFF;
  const int i = a & 0x3F;
  if (i >= 0x10 && i < 0x30) {
    const uint16_t w = i < 0x20 ? commCmd_[(i - 0x10) >> 1] : commStatus_[(i - 0x20) >> 1];
    return static_cast<uint8_t>((i & 1) ? w : w >> 8);
  }
  switch (i) {
    case 0x00: return static_cast<uint8_t>(((mask_ & 4) << 5) | ifl2_);
    case 0x01: return resetBusreq_;
    case 0x02: return wp_;
    case 0x03: return mainModeByte();
    case 0x04: return cdcModeByte();
    case 0x05: return 0;
    case 0x06: return static_cast<uint8_t>(hint_ >> 8);
    case 0x07: return static_cast<uint8_t>(hint_);
    case 0x08: return static_cast<uint8_t>(hostData_ >> 8);
    case 0x09: return static_cast<uint8_t>(hostData_);
    case 0x0A: return static_cast<uint8_t>(dmaAddr_ >> 8);
    case 0x0B: return static_cast<uint8_t>(dmaAddr_);
    case 0x0C: return static_cast<uint8_t>(stopwatch(mainNow()) >> 8);
    case 0x0D: return static_cast<uint8_t>(stopwatch(mainNow()));
    case 0x0E: return commMain_;
    case 0x0F: return commSub_;
    default: return static_cast<uint8_t>((a & 1) ? md_.cpu.irc : md_.cpu.irc >> 8);
  }
}

uint16_t SegaCD::mainRegRead16(uint32_t a) {
  sync68k();
  if ((a & 0xFFFF00) != 0xA12000) return 0xFFFF;
  const int i = a & 0x3E;
  if (i >= 0x10 && i < 0x30) return i < 0x20 ? commCmd_[(i - 0x10) >> 1] : commStatus_[(i - 0x20) >> 1];
  switch (i) {
    case 0x00: return static_cast<uint16_t>(((mask_ & 4) << 13) | (ifl2_ << 8) | resetBusreq_);
    case 0x02: return static_cast<uint16_t>((wp_ << 8) | mainModeByte());
    case 0x04: return static_cast<uint16_t>(cdcModeByte() << 8);
    case 0x06: return hint_;
    case 0x08: return hostRead(2);
    case 0x0A: return dmaAddr_;
    case 0x0C: return stopwatch(mainNow());
    case 0x0E: return static_cast<uint16_t>((commMain_ << 8) | commSub_);
    default: return md_.cpu.irc;
  }
}

void SegaCD::mainInterrupt2() {
  if (!(mask_ & 4)) return;
  ifl2_ = true;
  pending_ |= 1 << 2;
  refreshIpl();
}

void SegaCD::mainResetBusreq(uint8_t v) {
  const bool wasHalted = subHalt_;
  if (v & 1) {  // SRES = 1: sub-CPU rodando (ou parado pelo SBRQ)
    if (!(resetBusreq_ & 1)) subReset_ = true;  // saindo do reset: relê os vetores
    subHalt_ = (v & 2) != 0;
    resetBusreq_ = v & 3;
  } else {      // reset: o sub fica parado e o SBRQ lê 1
    subHalt_ = true;
    resetBusreq_ = 2;
  }
  if (sub.stopped && !subReset_) resetBusreq_ &= static_cast<uint8_t>(~2);  // sub em STOP: o SBRQ lê 0
  if (subHalt_ == wasHalted) return;
  const int64_t t = mainNow();
  if (resetBusreq_ & 2) {  // o principal pegou a PRG-RAM: o DMA para ela espera
    if (xfer_ == TO_PRG) {
      runTransfer(t);
      if (xfer_ == TO_PRG) { xferHeld_ = TO_PRG; xfer_ = TO_NONE; }
    }
  } else if (xferHeld_ == TO_PRG) {
    xfer_ = TO_PRG; xferHeld_ = TO_NONE;
    xferClk_ = std::max(xferClk_, t);
  }
}

// A12003. 2M: DMNA = 1 entrega a Word RAM ao sub-CPU (RET vai a 0). 1M: DMNA = 1 pede a troca dos bancos, que
// o sub faz escrevendo RET; escrever 0 deixa o DMNA lido em 1.
void SegaCD::mainMemoryMode(uint8_t v) {
  bank_ = (v >> 6) & 3;
  const bool askDmna = (v & 2) != 0;
  if (mode1M_) {
    if (askDmna) swapAsked_ = true;
    else dmna_ = true;
    return;
  }
  if (!askDmna) return;
  swapAsked_ = true;
  dmna_ = true;
  ret_ = false;
  const int64_t t = mainNow();
  if (subWaiting_) { subWaiting_ = false; subClk_ = std::max(subClk_, t); }
  if (asic_.busy) asic_.clk = std::max(asic_.clk, t);
  if (xferHeld_ == TO_WRAM_2M) {
    xfer_ = TO_WRAM_2M; xferHeld_ = TO_NONE;
    xferClk_ = std::max(xferClk_, t);
  }
}

void SegaCD::mainRegWrite8(uint32_t a, uint8_t v) {
  if ((a & 0xFFFF00) != 0xA12000) return;
  sync68k();
  const int i = a & 0x3F;
  if (i >= 0x10 && i < 0x20) {
    uint16_t& w = commCmd_[(i - 0x10) >> 1];
    w = (i & 1) ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | (v << 8));
    return;
  }
  switch (i) {
    case 0x00: if (v & 1) mainInterrupt2(); break;
    case 0x01: mainResetBusreq(v); break;
    case 0x02: wp_ = v; break;
    case 0x03: mainMemoryMode(v); break;
    case 0x0E: case 0x0F: commMain_ = v; break;
    default: break;
  }
}

void SegaCD::mainRegWrite16(uint32_t a, uint16_t v) {
  if ((a & 0xFFFF00) != 0xA12000) return;
  sync68k();
  const int i = a & 0x3E;
  if (i >= 0x10 && i < 0x20) { commCmd_[(i - 0x10) >> 1] = v; return; }
  switch (i) {
    case 0x00:
      mainResetBusreq(static_cast<uint8_t>(v));
      if (v & 0x100) mainInterrupt2();
      break;
    case 0x02:
      wp_ = static_cast<uint8_t>(v >> 8);
      mainMemoryMode(static_cast<uint8_t>(v));
      break;
    case 0x06: hint_ = v; break;
    case 0x08: hostRead(2); break;
    case 0x0E: commMain_ = static_cast<uint8_t>(v >> 8); break;
    default: break;
  }
}

// ------------------------------------------------------------ registradores: sub-CPU
uint16_t SegaCD::fontWord(int k) const {
  const int bits = (fontBits_ >> (12 - 4 * k)) & 0xF;  // 4 pontos, o da esquerda no bit 3
  const int ink = fontColor_ >> 4, paper = fontColor_ & 0xF;
  uint16_t w = 0;
  for (int i = 0; i < 4; i++) w = static_cast<uint16_t>(w | (((bits >> i) & 1 ? ink : paper) << (4 * i)));
  return w;
}

uint16_t SegaCD::subRegRead16(uint32_t a) {
  a &= 0x1FE;
  if (a >= 0x100) return subcode_[(a >> 1) & 0x3F];
  if (a >= 0x10 && a < 0x30) return a < 0x20 ? commCmd_[(a - 0x10) >> 1] : commStatus_[(a - 0x20) >> 1];
  if (a >= 0x38 && a < 0x42) return static_cast<uint16_t>((drive_.rs[a - 0x38] << 8) | drive_.rs[a - 0x37]);
  if (a >= 0x42 && a < 0x4C) return static_cast<uint16_t>((drive_.cs[a - 0x42] << 8) | drive_.cs[a - 0x41]);
  if (a >= 0x50 && a < 0x58) return fontWord(static_cast<int>(a - 0x50) >> 1);
  if (a >= 0x58 && a < 0x68) asicRun(now());
  switch (a) {
    case 0x00: return static_cast<uint16_t>((led_ << 8) | 0x01);  // RES0 lê 1 (periféricos prontos)
    case 0x02: return static_cast<uint16_t>((wp_ << 8) | subModeByte());
    case 0x04: return static_cast<uint16_t>((cdcModeByte() << 8) | cdc_.address());
    case 0x06: return cdcRead();
    case 0x08: return hostRead(3);
    case 0x0A: return dmaAddr_;
    case 0x0C: return stopwatch(now());
    case 0x0E: return static_cast<uint16_t>((commMain_ << 8) | commSub_);
    case 0x30: return timerReload_;
    case 0x32: return mask_;
    case 0x36: return static_cast<uint16_t>((drive_.muted << 8) | (hock_ << 2));
    case 0x4C: return fontColor_;
    case 0x4E: return fontBits_;
    case 0x58: return static_cast<uint16_t>((asic_.busy << 15) | asic_.config);
    case 0x5A: return asic_.mapReg;
    case 0x5C: return asic_.cellsReg;
    case 0x5E: return asic_.imageReg;
    case 0x60: return asic_.offsetReg;
    case 0x62: return asic_.widthReg;
    case 0x64: return asic_.heightReg;
    case 0x66: return asic_.traceReg;
    default: return 0;
  }
}

uint8_t SegaCD::subRegRead8(uint32_t a) {
  a &= 0x1FF;
  switch (a) {  // os que têm efeito na leitura ou não são metade de uma palavra
    case 0x00: return led_;
    case 0x01: return 0x01;
    case 0x06: return 0;
    case 0x07: return cdcRead();
    case 0x08: return static_cast<uint8_t>(hostData_ >> 8);
    case 0x09: return static_cast<uint8_t>(hostData_);
    default: break;
  }
  const uint16_t w = subRegRead16(a & 0x1FE);
  return static_cast<uint8_t>((a & 1) ? w : w >> 8);
}

// FF8003. 2M: RET = 1 devolve a Word RAM ao principal. 1M: RET escolhe qual banco fica com cada CPU e atende o
// pedido de troca do principal. Mudar o MODE troca de 2M para 1M e volta.
void SegaCD::subMemoryMode(uint8_t v) {
  bool wantRet = v & 1;
  const bool want1M = (v & 4) != 0;
  pm_ = (v >> 3) & 3;
  if (wantRet != ret_ || want1M != mode1M_) {
    if (want1M) {
      if (wantRet) swapAsked_ = false;
      ret_ = wantRet; mode1M_ = true; dmna_ = false;
      return;
    }
    if (mode1M_ && !swapAsked_) wantRet = true;  // volta para 2M sem pedido do principal: a RAM fica com ele
    if (mode1M_ && !wantRet) {                   // volta para 2M com a RAM do sub
      ret_ = false; mode1M_ = false; dmna_ = true;
      return;
    }
    if (wantRet) {  // a Word RAM 2M volta para o principal
      const int64_t t = now();
      asicRun(t);
      if (xfer_ == TO_WRAM_2M) {
        runTransfer(t);
        if (xfer_ == TO_WRAM_2M) { xferHeld_ = TO_WRAM_2M; xfer_ = TO_NONE; }
      }
      swapAsked_ = false;
      ret_ = true; mode1M_ = false; dmna_ = false;
      return;
    }
  }
  mode1M_ = want1M;
}

void SegaCD::subRegWrite16(uint32_t a, uint16_t v) {
  a &= 0x1FE;
  const uint8_t lo = static_cast<uint8_t>(v), hi = static_cast<uint8_t>(v >> 8);
  if (a >= 0x100) { subcode_[(a >> 1) & 0x3F] = v; return; }
  if (a >= 0x20 && a < 0x30) { commStatus_[(a - 0x20) >> 1] = v; return; }
  if (a >= 0x42 && a < 0x4A) { drive_.cs[a - 0x42] = hi; drive_.cs[a - 0x41] = lo; return; }
  switch (a) {
    case 0x00:
      led_ = hi;
      if (!(v & 1)) peripheralReset();
      break;
    case 0x02: subMemoryMode(lo); break;
    case 0x04: cdc_.setAddress(lo); setCdcMode(hi); break;
    case 0x06: cdcWrite(lo); break;
    case 0x08: hostRead(3); break;
    case 0x0A: dmaAddr_ = v; break;
    case 0x0C: {  // zera a contagem (o divisor de 30,72 us continua na mesma fase)
      const int64_t t = now();
      stopwatchZero_ += (t - stopwatchZero_) / TICK_30US * TICK_30US;
      break;
    }
    case 0x0E: commSub_ = lo; break;
    case 0x30: setTimer(lo); break;
    case 0x32: setMask(lo); break;
    case 0x34: drive_.fadeTarget = static_cast<uint16_t>(v >> 4); break;
    case 0x36: hock_ = (v & 4) != 0; break;
    case 0x4A:  // CS8/CS9: envia o comando (os registradores voltam a 0)
      drive_.cs[8] = drive_.cs[9] = 0;
      drive_.command();
      break;
    case 0x4C: fontColor_ = lo; break;
    case 0x4E: fontBits_ = v; break;
    case 0x58: asic_.config = lo; break;
    case 0x5A: asic_.mapReg = v; break;
    case 0x5C: asic_.cellsReg = v; break;
    case 0x5E: asic_.imageReg = v; break;
    case 0x60: asic_.offsetReg = v; break;
    case 0x62: asic_.widthReg = v; break;
    case 0x64: asic_.heightReg = v; break;
    case 0x66: asic_.traceReg = v; asicStart(); break;
    default: break;
  }
}

void SegaCD::subRegWrite8(uint32_t a, uint8_t v) {
  a &= 0x1FF;
  auto setByte = [&](uint16_t& w) { w = (a & 1) ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | (v << 8)); };
  if (a >= 0x100) { setByte(subcode_[(a >> 1) & 0x3F]); return; }
  if (a >= 0x20 && a < 0x30) { setByte(commStatus_[(a - 0x20) >> 1]); return; }
  if (a >= 0x42 && a < 0x4C) { drive_.cs[a - 0x42] = v; return; }
  // registradores só de palavra: o byte aparece nas duas metades do barramento
  if ((a >= 0x08 && a < 0x0E) || a == 0x34 || a == 0x35 || (a >= 0x5A && a < 0x68)) {
    subRegWrite16(a & 0x1FE, static_cast<uint16_t>((v << 8) | v));
    return;
  }
  switch (a) {
    case 0x00: led_ = v; break;
    case 0x01: if (!(v & 1)) peripheralReset(); break;
    case 0x02: case 0x03: subMemoryMode(v); break;
    case 0x04: setCdcMode(v); break;
    case 0x05: cdc_.setAddress(v); break;
    case 0x07: cdcWrite(v); break;
    case 0x0E: case 0x0F: commSub_ = v; break;
    case 0x30: case 0x31: setTimer(v); break;
    case 0x33: setMask(v); break;
    case 0x37: hock_ = (v & 4) != 0; break;
    case 0x4C: case 0x4D: fontColor_ = v; break;
    case 0x4E: case 0x4F: setByte(fontBits_); break;
    case 0x59: asic_.config = v; break;
    default: break;
  }
}

// RES0 = 0: periféricos do CD (CDC, drive, PCM, temporizador, ASIC, interrupções) voltam ao estado de ligar
void SegaCD::peripheralReset() {
  const int64_t t = now();
  dd_ = edt_ = dsr_ = 0;
  hostData_ = 0xFFFF;
  dmaAddr_ = 0xFFFF;
  timerReload_ = 0; timerAt_ = 0;
  mask_ = 0;
  pending_ = 0;
  hock_ = false;
  fontColor_ = 0; fontBits_ = 0;
  std::memset(subcode_, 0, sizeof subcode_);
  asic_ = StampAsic{};
  asic_.clk = 0;
  stopwatchZero_ = t;
  driveAt3_ = t * 3 + DRIVE_FRAME3;
  std::memset(drive_.rs, 0, sizeof drive_.rs);
  drive_.rs[9] = 0x0F;
  std::memset(drive_.cs, 0xFF, sizeof drive_.cs);
  drive_.reset();
  cdc_.reset();
  xfer_ = xferHeld_ = TO_NONE;
  xferClk_ = 0;
  pcmRun(t);
  pcm_.reset();
  refreshIpl();
}

}  // namespace indigo::segacd
