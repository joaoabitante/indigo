#include "timers.h"

#include <algorithm>

#include "system.h"

namespace indigo::ps1 {

void Timers::reset() {
  for (auto& t : t_) t = T{0, 0x400, 0, false, false};
  last_ = sys_.now;
  dotFrac_ = 0;
  inVblank_ = false;
}

bool Timers::sysClock(int i) const {
  return i == 2 ? !((t_[2].mode >> 9) & 1) : !((t_[i].mode >> 8) & 1);
}

bool Timers::countsSys(int i) const {
  const uint32_t src = (t_[i].mode >> 8) & 3;
  if (i == 2) return true;
  return !(src & 1);
}

void Timers::irq(int i) {
  T& t = t_[i];
  if (!(t.mode & 0x40) && t.irqDone) return;  // modo de uma vez: só a primeira
  t.irqDone = true;
  if (t.mode & 0x80) {
    t.mode ^= 0x400;                          // alterna
    if (!(t.mode & 0x400)) sys_.raiseIrq(IRQ_TMR0 + i);
  } else {
    sys_.raiseIrq(IRQ_TMR0 + i);              // pulso: o bit 10 cai e volta logo
    t.mode |= 0x400;
  }
}

void Timers::add(int i, uint32_t n) {
  T& t = t_[i];
  if (!n || t.paused) return;
  const uint32_t old = t.counter;
  uint64_t c = uint64_t(old) + n;
  bool target = false, overflow = false;
  // volta a 0 no alvo: o contador chega ao alvo e um ciclo do sistema depois volta a 0, perdendo o passo desse
  // ciclo (timer-dump do ps1-tests, relógio de pontos manual: 1, 2, 3, 4 e logo 0 com alvo 4; timers, relógio do
  // sistema com alvo 10: 0 por 2 ciclos e 1..10 por 1 ciclo cada, período 12). Nos relógios lentos o período fica
  // igual ao alvo; no do sistema, alvo+2 (HOLD marca o 0 que ainda segura o próximo passo)
  if ((t.mode & 0x08) && old <= t.target && t.target) {
    if (sysClock(i)) {
      const uint64_t period = uint64_t(t.target) + 2, at = uint64_t(t.target) + 1;  // fase em que mostra o alvo
      const uint64_t ph0 = (t.mode & HOLD) ? 0 : old == 0 ? 1 : uint64_t(old) + 1, ph = ph0 + n;
      target = ph0 < at ? ph >= at : ph >= period + at;
      t.mode &= ~HOLD;
      if (ph % period == 0) t.mode |= HOLD;
      c = ph % period <= 1 ? 0 : ph % period - 1;
    } else if (c >= t.target) {
      target = old < t.target || c >= 2 * uint64_t(t.target);
      c %= t.target;
    }
  } else if ((t.mode & 0x08) && !t.target) {
    c = 0;
    target = true;
  } else {
    if (old < t.target && c >= t.target) target = true;
    t.mode &= ~HOLD;
    if (c >= 0xFFFF) overflow = true;
    if (c >= 0x10000) {
      c &= 0xFFFF;  // depois do FFFFh volta a 0 (período 10000h)
      if ((t.mode & 0x08) && c >= t.target) { target = true; c = t.target ? c % t.target : 0; }
      else if (c >= t.target) target = true;
    }
  }
  t.counter = uint32_t(c);
  if (target) t.mode |= 0x800;
  if (overflow) t.mode |= 0x1000;
  if ((target && (t.mode & 0x10)) || (overflow && (t.mode & 0x20))) irq(i);
}

void Timers::sync() {
  const uint64_t now = sys_.now;
  if (now <= last_) return;
  const uint64_t el = now - last_;
  // timer 0: sistema ou relógio de pontos
  if ((t_[0].mode >> 8) & 1) {
    const double r = sys_.gpu.videoPerCpu() / sys_.gpu.dotDivider();
    const uint64_t step = uint64_t(r * 4294967296.0);
    const uint64_t total = dotFrac_ + el * step;
    dotFrac_ = total & 0xFFFFFFFFu;
    add(0, uint32_t(std::min<uint64_t>(total >> 32, 0xFFFFFFFFu)));
  } else {
    add(0, uint32_t(std::min<uint64_t>(el, 0xFFFFFFFFu)));
  }
  // timer 1: sistema (o hblank conta em hblank())
  if (!((t_[1].mode >> 8) & 1)) add(1, uint32_t(std::min<uint64_t>(el, 0xFFFFFFFFu)));
  // timer 2: sistema ou sistema/8
  if ((t_[2].mode >> 9) & 1) add(2, uint32_t(std::min<uint64_t>((now >> 3) - (last_ >> 3), 0xFFFFFFFFu)));
  else add(2, uint32_t(std::min<uint64_t>(el, 0xFFFFFFFFu)));
  last_ = now;
}

uint64_t Timers::nextEvent() const {
  uint64_t best = ~0ull;
  for (int i = 0; i < 3; i++) {
    const T& t = t_[i];
    if (t.paused || !(t.mode & 0x30)) continue;
    if (!(t.mode & 0x40) && t.irqDone) continue;
    if (i == 1 && ((t.mode >> 8) & 1)) continue;  // hblank: a virada de linha já é um evento
    uint32_t dist = 0xFFFF - t.counter;
    if (dist == 0) dist = 0x10000;
    if ((t.mode & 0x10) && t.counter < t.target) dist = std::min(dist, t.target - t.counter + ((t.mode & HOLD) ? 1 : 0));
    if ((t.mode & 0x08) && t.target == 0) dist = 1;
    if (dist == 0) dist = 1;
    uint64_t cyc = dist;
    if (i == 2 && ((t.mode >> 9) & 1)) cyc = uint64_t(dist) * 8;
    if (i == 0 && ((t.mode >> 8) & 1)) cyc = uint64_t(double(dist) * sys_.gpu.dotDivider() / sys_.gpu.videoPerCpu()) + 1;
    best = std::min(best, last_ + cyc);
  }
  return best;
}

void Timers::hblank() {
  sync();
  T& t0 = t_[0];
  if (t0.mode & 1) {
    switch ((t0.mode >> 1) & 3) {
      case 1: case 2: t0.counter = 0; break;
      case 3: t0.paused = false; t0.mode &= ~1u; break;  // espera o primeiro hblank e corre livre
      default: break;
    }
  }
  if ((t_[1].mode >> 8) & 1) add(1, 1);
}

void Timers::vblank(bool on) {
  sync();
  inVblank_ = on;
  T& t = t_[1];
  if (!(t.mode & 1)) return;
  switch ((t.mode >> 1) & 3) {
    case 0: t.paused = on; break;
    case 1: if (on) t.counter = 0; break;
    case 2: if (on) t.counter = 0; t.paused = !on; break;
    case 3: if (on) { t.paused = false; t.mode &= ~1u; } break;
  }
}

uint32_t Timers::read(uint32_t off) {
  const int i = int((off >> 4) & 3);
  if (i > 2) return 0;
  T& t = t_[i];
  switch (off & 0xC) {
    case 0x0: return t.counter;
    case 0x4: { const uint32_t v = t.mode & 0x1FFF; t.mode &= ~0x1800u; return v; }
    case 0x8: return t.target;
    default: return 0;
  }
}

void Timers::write(uint32_t off, uint32_t v) {
  const int i = int((off >> 4) & 3);
  if (i > 2) return;
  T& t = t_[i];
  switch (off & 0xC) {
    case 0x0: t.counter = v & 0xFFFF; t.mode &= ~HOLD; break;
    case 0x4: {
      t.mode = (v & 0x3FF) | 0x400;
      t.counter = 0;
      t.irqDone = false;
      t.paused = false;
      if (t.mode & 1) {
        const uint32_t sm = (t.mode >> 1) & 3;
        if (i == 2) t.paused = (sm == 0 || sm == 3);
        else if (i == 1) t.paused = sm == 3 || (sm == 0 && inVblank_) || (sm == 2 && !inVblank_);
        else t.paused = sm == 3 || sm == 2;
      }
      if (i == 0) dotFrac_ = 0;
      break;
    }
    case 0x8: t.target = v & 0xFFFF; break;
    default: break;
  }
}

void Timers::sync(StateIO& io) {
  for (auto& t : t_) {
    io.sync(t.counter); io.sync(t.mode); io.sync(t.target); io.sync(t.paused); io.sync(t.irqDone);
    if (io.reading()) { t.counter &= 0xFFFF; t.target &= 0xFFFF; t.mode &= 0x1FFF | HOLD; }
  }
  io.sync(last_); io.sync(dotFrac_); io.sync(inVblank_);
  if (io.reading()) { dotFrac_ &= 0xFFFFFFFFu; if (last_ > sys_.now) last_ = sys_.now; }
}

}  // namespace indigo::ps1
