// Mikey: temporizadores, interrupções, som, tela (DMA + paleta), UART e portas de E/S.
#include <algorithm>
#include <cmath>

#include "lynx.h"

namespace indigo::lynx {

namespace {
// encadeamento: T0→T2→T4 e T1→T3→T5→T7→A0→A1→A2→A3→T1 (A = canais de som, índices 8-11)
constexpr int NEXT_LINK[12] = {2, 3, 4, 5, -1, 7, -1, 8, 9, 10, 11, 1};
constexpr int UART_BYTE = 11 * 8;  // 11 bits, 8 estouros do T4 por bit

inline uint32_t color(uint8_t g, uint8_t br) {
  const uint32_t r = (br & 0x0F) * 17u, gg = (g & 0x0F) * 17u, b = (br >> 4) * 17u;
  return 0xFF000000u | (b << 16) | (gg << 8) | r;
}
}  // namespace

// ---------------------------------------------------------------- temporizadores
// Os temporizadores com relógio próprio contam nos múltiplos do período (o pré-divisor é comum a todos).
// A contagem é feita "atrasada": updateTimers() leva todos até 'now', disparando cada estouro no instante certo.
void Lynx::writeCtla(int i, uint8_t v) {
  Timer& t = tim[i];
  if (i < 8) t.irqEn = (v & 0x80) && i != 4;
  if (v & 0x40) t.done = false;
  t.reload = v & 0x10;
  const bool wasCounting = t.count;
  const uint8_t oldLink = t.linking;
  t.count = v & 0x08;
  t.linking = v & 0x07;
  if (t.count && t.linking != 7 && (!wasCounting || oldLink != t.linking || t.next <= now)) {
    const uint64_t period = 16ull << t.linking;
    t.next = (now / period + 1) * period;
  }
}

static bool active(const Timer& t) { return t.count && (t.reload || !t.done); }

void Lynx::countTimer(int i, uint64_t when) {
  Timer& t = tim[i];
  if (!active(t)) return;
  t.borrowIn = true;
  if (t.cnt == 0) borrow(i, when);
  else t.cnt--;
}

void Lynx::borrow(int i, uint64_t when) {
  Timer& t = tim[i];
  t.borrowOut = true;
  t.done = true;
  t.cnt = t.reload ? t.bkup : 0;
  if (i < 8) {
    if (t.irqEn) intStatus |= static_cast<uint8_t>(1u << i);
    if (i == 0) hbl();
    else if (i == 2) vbl();
    else if (i == 4) uartTick();
  } else {
    Audio& a = aud[i - 8];
    if (t.bkup || t.linking) {
      // LFSR de 12 bits: o bit novo é o inverso da paridade das saídas escolhidas
      const uint32_t par = __builtin_popcount(a.lfsr & a.taps) & 1;
      a.lfsr = static_cast<uint16_t>(((a.lfsr << 1) & 0xFFE) | (par ^ 1));
    }
    int out;
    if (a.integrate) out = std::clamp(a.out + ((a.lfsr & 1) ? a.vol : -a.vol), -128, 127);
    else out = (a.lfsr & 1) ? a.vol : -a.vol;
    if (out != a.out) {
      audioAdvance(when);
      a.out = static_cast<int8_t>(out);
      audioMix();
    }
  }
  const int j = NEXT_LINK[i];
  if (j >= 0 && tim[j].linking == 7) countTimer(j, when);
}

void Lynx::updateTimers() {
  for (int i = 0; i < 12; i++) {
    Timer& t = tim[i];
    t.borrowIn = t.borrowOut = false;
    if (t.linking == 7) continue;
    const uint64_t period = 16ull << t.linking;
    // canal de som a 1 MHz com recarga 0: fora da faixa audível, fica parado (como o Handy)
    if (i >= 8 && t.bkup == 0 && t.linking == 0) { if (t.next <= now) t.next = (now / period + 1) * period; continue; }
    int guard = 0;
    while (active(t) && t.next <= now && guard++ < 4096) {
      const uint64_t k = (now - t.next) / period + 1;
      if (k <= t.cnt) {
        t.cnt = static_cast<uint8_t>(t.cnt - k);
        t.next += k * period;
        t.borrowIn = true;
        break;
      }
      const uint64_t when = t.next + t.cnt * period;
      t.next = when + period;
      t.cnt = 0;
      t.borrowIn = true;
      borrow(i, when);
    }
    if (!active(t) && t.next <= now) t.next = (now / period + 1) * period;
  }
  scheduleNext();
}

void Lynx::scheduleNext() {
  uint64_t ev = frameEnd;
  const bool uartBusy = txCountdown >= 0 || rxCountdown >= 0 || sendBreak;
  for (int i = 0; i < 12; i++) {
    const Timer& t = tim[i];
    if (t.linking == 7 || !active(t)) continue;
    if (i >= 8 && t.bkup == 0 && t.linking == 0) continue;
    // estouros que não mudam nada visível podem ser contados depois
    const int j = NEXT_LINK[i];
    const bool feeds = j >= 0 && tim[j].linking == 7 && active(tim[j]);
    if (i < 8 && !t.irqEn && !feeds && i != 0 && i != 2 && !(i == 4 && uartBusy)) continue;
    const uint64_t when = t.next + static_cast<uint64_t>(t.cnt) * (16ull << t.linking);
    if (when < ev) ev = when;
  }
  if (sleeping && sleepUntil < ev) ev = sleepUntil;
  nextEvent = ev;
}

// a interrupção da UART (bit 4) é por nível: fica ativa enquanto a condição durar
bool Lynx::uartIrq() const { return (txIrqEn && txCountdown < 0) || (rxIrqEn && rxReady); }
bool Lynx::irqLine() const { return intStatus || uartIrq(); }

// ---------------------------------------------------------------- tela
void Lynx::hbl() {
  const int lastLine = tim[2].bkup;
  rest = lynxLine == lastLine - 2 || lynxLine == lastLine - 3 || lynxLine == lastLine - 4;
  if (lynxLine == lastLine - 3) {
    lynxAddr = static_cast<uint16_t>((dispadr & 0xFFFC) + ((dispctl & 2) ? 3 : 0));
    dmaLines = SCREEN_H;
    curRow = 0;
  }
  if (lynxLine > 0) lynxLine--;
  if (dmaLines > 0) {
    dmaLines--;
    if (dispctl & 1) {
      uint32_t pal[16];
      for (int c = 0; c < 16; c++) pal[c] = color(green[c], bluered[c]);
      uint32_t* row = lcd + curRow * SCREEN_W;
      const bool flip = dispctl & 2;
      for (int k = 0; k < SCREEN_W / 2; k++) {
        const uint8_t b = ram[lynxAddr];
        if (flip) { lynxAddr--; row[2 * k] = pal[b & 15]; row[2 * k + 1] = pal[b >> 4]; }
        else { lynxAddr++; row[2 * k] = pal[b >> 4]; row[2 * k + 1] = pal[b & 15]; }
      }
      now += (80 + 100) * 4;  // a DMA da tela toma o barramento
    }
    if (curRow < SCREEN_H - 1) curRow++;
  }
}

void Lynx::vbl() {
  dmaLines = 0;
  lynxLine = tim[2].bkup;
  if (sleeping && suzyBusyUntil <= now) sleeping = false;
  presentFrame();
}

void Lynx::presentFrame() {
  if (cart.rotation == 1) {  // girado para a esquerda: a primeira linha vira a coluna da direita
    for (int y = 0; y < SCREEN_H; y++)
      for (int x = 0; x < SCREEN_W; x++) fb[x * SCREEN_H + (SCREEN_H - 1 - y)] = lcd[y * SCREEN_W + x];
  } else if (cart.rotation == 2) {
    for (int y = 0; y < SCREEN_H; y++)
      for (int x = 0; x < SCREEN_W; x++) fb[(SCREEN_W - 1 - x) * SCREEN_H + y] = lcd[y * SCREEN_W + x];
  } else {
    std::copy(lcd, lcd + SCREEN_W * SCREEN_H, fb.begin());
  }
}

// ---------------------------------------------------------------- UART (sem cabo: o que sai volta)
void Lynx::uartTick() {
  if (rxCountdown == 0) {
    rxCountdown = -1;
    if (rxReady) overrun = true;
    rxData = rxPending;
    rxReady = true;
  } else if (rxCountdown > 0) {
    rxCountdown--;
  }
  if (txCountdown == 0) {
    if (sendBreak) {
      txData = 0x8000;
      txCountdown = UART_BYTE;
      rxPending = 0x8000; rxCountdown = UART_BYTE;
    } else {
      txCountdown = -1;
    }
  } else if (txCountdown > 0) {
    txCountdown--;
  }
}

// ---------------------------------------------------------------- som
void Lynx::audioMix() {
  int l = 0, r = 0;
  for (int c = 0; c < 4; c++) {
    const int o = aud[c].out;
    if (!(stereo & (0x10 << c))) l += (pan & (0x10 << c)) ? o * (atten[c] & 0xF0) / 256 : o;
    if (!(stereo & (0x01 << c))) r += (pan & (0x01 << c)) ? o * (atten[c] & 0x0F) / 16 : o;
  }
  curL = l;
  curR = r;
}

void Lynx::audioAdvance(uint64_t t) {
  if (t <= audioTick) return;
  while (audioTick < t) {
    const double toSample = ticksPerSample - sampleTick;
    const uint64_t left = t - audioTick;
    if (static_cast<double>(left) < toSample) {
      accL += curL * static_cast<double>(left);
      accR += curR * static_cast<double>(left);
      sampleTick += static_cast<double>(left);
      audioTick = t;
      break;
    }
    uint64_t step = static_cast<uint64_t>(std::ceil(toSample));
    if (step == 0) step = 1;
    accL += curL * toSample;
    accR += curR * toSample;
    audioTick += step;
    sampleTick = sampleTick + static_cast<double>(step) - ticksPerSample;
    // resto do passo já pertence à próxima amostra
    const double extra = sampleTick;
    float l = static_cast<float>(accL / ticksPerSample / 512.0);
    float r = static_cast<float>(accR / ticksPerSample / 512.0);
    accL = curL * extra;
    accR = curR * extra;
    // passa-alta (~20 Hz) para tirar o nível contínuo
    hpL += (l - hpL) * 0.0026;
    hpR += (r - hpR) * 0.0026;
    l -= static_cast<float>(hpL);
    r -= static_cast<float>(hpR);
    bufL[wpos] = l;
    bufR[wpos] = r;
    wpos = (wpos + 1) % BUF;
    if (wpos == rpos) rpos = (rpos + 1) % BUF;
  }
}

void Lynx::setSampleRate(int rate) {
  if (rate < 8000) rate = 8000;
  if (rate > 192000) rate = 192000;
  ticksPerSample = 16000000.0 / rate;
  sampleTick = 0;
}

void Lynx::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (rpos == wpos) { l[k] = r[k] = 0; continue; }
    l[k] = bufL[rpos];
    r[k] = bufR[rpos];
    rpos = (rpos + 1) % BUF;
  }
}

// ---------------------------------------------------------------- registradores
void Lynx::mikeyWrite(uint8_t r, uint8_t v) {
  if (r < 0x40) {
    updateTimers();
    audioAdvance(now);
    const bool audio = r >= 0x20;
    const int i = audio ? 8 + ((r - 0x20) >> 3) : r >> 2;
    Timer& t = tim[i];
    const int reg = audio ? (r & 7) : (r & 3);
    if (!audio) {
      switch (reg) {
        case 0: t.bkup = v; break;
        case 1: writeCtla(i, v); break;
        case 2: t.cnt = v; break;
        case 3: t.done = v & 8; t.lastClock = v & 4; t.borrowIn = v & 2; t.borrowOut = v & 1; break;
      }
    } else {
      Audio& a = aud[i - 8];
      switch (reg) {
        case 0: a.vol = static_cast<int8_t>(v); break;
        case 1: a.taps = static_cast<uint16_t>((a.taps & 0x080) | (v & 0x3F) | ((v & 0xC0) << 4)); break;
        case 2: a.out = static_cast<int8_t>(v); break;
        case 3: a.lfsr = static_cast<uint16_t>((a.lfsr & 0xF00) | v); break;
        case 4: t.bkup = v; break;
        case 5:
          a.taps = static_cast<uint16_t>((a.taps & ~0x080) | ((v & 0x80) ? 0x080 : 0));
          a.integrate = v & 0x20;
          writeCtla(i, v);
          break;
        case 6: t.cnt = v; break;
        case 7:
          a.lfsr = static_cast<uint16_t>((a.lfsr & 0x0FF) | ((v & 0xF0) << 4));
          t.lastClock = v & 4; t.borrowIn = v & 2; t.borrowOut = v & 1;
          break;
      }
      audioMix();
    }
    scheduleNext();
    return;
  }
  switch (r) {
    case 0x40: case 0x41: case 0x42: case 0x43: audioAdvance(now); atten[r & 3] = v; audioMix(); break;
    case 0x44: audioAdvance(now); pan = v; audioMix(); break;
    case 0x50: audioAdvance(now); stereo = v; audioMix(); break;
    case 0x80: updateTimers(); intStatus &= static_cast<uint8_t>(~v); break;
    case 0x81: intStatus |= v; break;
    case 0x87: {  // SYSCTL1: bit 0 = estrobo de endereço do cartucho
      sysctl1 = v;
      cartStrobe(v & 1);
      break;
    }
    case 0x8A: iodir = v; break;
    case 0x8B:
      iodat = v;
      cart.addrData = v & 0x02;
      break;
    case 0x8C:  // SERCTL
      txIrqEn = v & 0x80; rxIrqEn = v & 0x40; parEn = v & 0x10; parEven = v & 0x01;
      if (v & 0x08) { overrun = false; framing = false; }
      sendBreak = v & 0x02;
      if (sendBreak) { txCountdown = UART_BYTE; rxPending = 0x8000; rxCountdown = UART_BYTE; }
      updateTimers();
      break;
    case 0x8D:  // SERDAT
      txData = v;
      txCountdown = UART_BYTE;
      rxPending = static_cast<uint16_t>(v | ((!parEn && parEven) ? 0x100 : 0));
      rxCountdown = UART_BYTE;
      updateTimers();
      break;
    case 0x90: break;  // SDONEACK
    case 0x91:         // CPUSLEEP: dorme até a Suzy acabar
      if (sprGo && suzyBusEn && suzyBusyUntil <= now) {
        const uint32_t cost = paintSprites();
        suzyBusyUntil = now + cost;
      }
      if (suzyBusyUntil > now) { sleeping = true; sleepUntil = suzyBusyUntil; scheduleNext(); }
      break;
    case 0x92: dispctl = v; break;
    case 0x93: pbkup = v; break;
    case 0x94: dispadr = static_cast<uint16_t>((dispadr & 0xFF00) | v); break;
    case 0x95: dispadr = static_cast<uint16_t>((dispadr & 0x00FF) | (v << 8)); break;
    default:
      if (r >= 0xA0 && r < 0xB0) green[r & 15] = v & 0x0F;
      else if (r >= 0xB0 && r < 0xC0) bluered[r & 15] = v;
      break;
  }
}

uint8_t Lynx::mikeyRead(uint8_t r) {
  if (r < 0x40) {
    const bool audio = r >= 0x20;
    const int i = audio ? 8 + ((r - 0x20) >> 3) : r >> 2;
    const int reg = audio ? (r & 7) : (r & 3);
    Timer& t = tim[i];
    if (!audio) {
      switch (reg) {
        case 0: return t.bkup;
        case 1: return static_cast<uint8_t>((t.irqEn ? 0x80 : 0) | (t.reload ? 0x10 : 0) | (t.count ? 0x08 : 0) | t.linking);
        case 2: updateTimers(); return t.cnt;
        default:
          updateTimers();
          return static_cast<uint8_t>((t.done ? 8 : 0) | (t.lastClock ? 4 : 0) | (t.borrowIn ? 2 : 0) | (t.borrowOut ? 1 : 0));
      }
    }
    const Audio& a = aud[i - 8];
    switch (reg) {
      case 0: return static_cast<uint8_t>(a.vol);
      case 1: return static_cast<uint8_t>((a.taps & 0x3F) | ((a.taps >> 4) & 0xC0));
      case 2: return static_cast<uint8_t>(a.out);
      case 3: return static_cast<uint8_t>(a.lfsr);
      case 4: return t.bkup;
      case 5:
        return static_cast<uint8_t>(((a.taps & 0x080) ? 0x80 : 0) | (a.integrate ? 0x20 : 0) | (t.reload ? 0x10 : 0) |
                                    (t.count ? 0x08 : 0) | t.linking);
      case 6: updateTimers(); return t.cnt;
      default:
        updateTimers();
        return static_cast<uint8_t>(((a.lfsr >> 4) & 0xF0) | (t.lastClock ? 4 : 0) | (t.borrowIn ? 2 : 0) | (t.borrowOut ? 1 : 0));
    }
  }
  switch (r) {
    case 0x40: case 0x41: case 0x42: case 0x43: return atten[r & 3];
    case 0x44: return pan;
    case 0x50: return stereo;
    case 0x80: case 0x81: updateTimers(); return static_cast<uint8_t>(intStatus | (uartIrq() ? 0x10 : 0));
    case 0x84: case 0x85: return 0;
    case 0x86: return 0x80;  // AUDIN
    case 0x88: return 0x01;  // MIKEYHREV
    case 0x8A: return iodir;
    case 0x8B: {
      uint8_t v = 0;
      if (eeprom.present() && !(iodir & 0x10)) v |= eeprom.outputBit() ? 0x10 : 0;
      else v |= iodat & 0x10;
      v |= (iodir & 0x08) ? (((iodat & 0x08) && rest) ? 0 : 0x08) : 0;
      v |= (iodir & 0x04) ? (iodat & 0x04) : 0;
      v |= (iodir & 0x02) ? (iodat & 0x02) : 0;
      v |= (iodir & 0x01) ? (iodat & 0x01) : 0x01;
      return v;
    }
    case 0x8C: {
      uint8_t v = 0;
      if (txCountdown < 0) v |= 0xA0;
      if (rxReady) v |= 0x40;
      if (overrun) v |= 0x08;
      if (framing) v |= 0x04;
      if (rxData & 0x8000) v |= 0x02;
      if (rxData & 0x0100) v |= 0x01;
      return v;
    }
    case 0x8D: rxReady = false; return static_cast<uint8_t>(rxData);
    case 0x92: return dispctl;
    case 0x93: return pbkup;
    case 0x94: return static_cast<uint8_t>(dispadr);
    case 0x95: return static_cast<uint8_t>(dispadr >> 8);
    default:
      if (r >= 0xA0 && r < 0xB0) return green[r & 15];
      if (r >= 0xB0 && r < 0xC0) return bluered[r & 15];
      return 0xFF;
  }
}

}  // namespace indigo::lynx
