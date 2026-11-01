// DSPP do 3DO, ver dspp.h.
#include "dspp.h"

namespace indigo::tdo {

void Dspp::reset() {
  for (auto& w : data) w = 0;
  running = false;
  pc = rbase = 0;
  acc = 0;
  fz = fn = fc = fv = false;
  for (auto& s : stack) s = 0;
  sp = 0;
  frameCount = 0;
  noise = 0x1234567;
  outL = outR = 0;
}

uint16_t Dspp::read(uint16_t a) {
  a &= DATA_WORDS - 1;
  if (a >= 0x300 && a <= 0x30C) return readFifo ? readFifo(a - 0x300) : 0;
  if (a == 0x3F8) {
    noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
    return uint16_t(noise);
  }
  if (a == 0x3FA) return uint16_t(frameCount);
  return data[a];
}

void Dspp::write(uint16_t a, uint16_t v) {
  a &= DATA_WORDS - 1;
  if (a == 0x3F0) outL = int16_t(v);
  else if (a == 0x3F1) outR = int16_t(v);
  else if (a == 0x3FE) { if (irq) irq(); }
  data[a] = v;
}

void Dspp::runSample() {
  frameCount++;
  if (!running) { outL = outR = 0; return; }
  pc = 0;
  sp = 0;
  for (int n = 0; n < MAX_STEPS; n++) {
    const uint16_t ins = fetch();
    const bool sleep = (ins & 0x8000) ? control(ins) : arith(ins);
    if (sleep) break;
  }
}

bool Dspp::control(uint16_t ins) {
  const uint16_t target = ins & 0x3FF;
  switch ((ins >> 13) & 3) {
    case 0:
      switch ((ins >> 10) & 7) {
        case 1: pc = uint16_t(acc & (CODE_WORDS - 1)); break;  // BAC
        case 2: if (sp) pc = stack[--sp & 3]; break;           // RTS
        case 3: rbase = target; break;                         // RBASE
        case 4: return true;                                   // SLEEP
        default: break;                                        // NOP
      }
      return false;
    case 1: {
      bool take;
      switch ((ins >> 10) & 7) {
        case 1: take = fz; break;
        case 2: take = fn; break;
        case 3: take = fc; break;
        case 4: take = !fz; break;
        case 5: take = !fn; break;
        case 6: take = !fc; break;
        case 7: take = fv; break;
        default: take = true; break;
      }
      if (take) pc = target & (CODE_WORDS - 1);
      return false;
    }
    case 2: {
      const uint16_t v = fetch();
      write(target, v);
      return false;
    }
    default:
      if (ins & 0x1000) { stack[sp & 3] = pc; sp = uint8_t((sp + 1) & 7); if (sp > 4) sp = 4; }
      pc = target & (CODE_WORDS - 1);
      return false;
  }
}

bool Dspp::arith(uint16_t ins) {
  const int nops = (ins >> 13) & 3;
  const bool m2sel = ins & 0x1000;
  const int muxa = (ins >> 10) & 3, muxb = (ins >> 8) & 3;
  const int alu = (ins >> 4) & 0xF, bs = ins & 0xF;
  struct Op { int32_t value; int addr; bool wb; };
  Op ops[3];
  for (int i = 0; i < nops; i++) {
    const uint16_t w = fetch();
    Op& o = ops[i];
    o.wb = false;
    o.addr = -1;
    if (w & 0x8000) {
      int32_t v = int32_t(uint32_t(w & 0x1FFF) << 19) >> 19;
      if (w & 0x2000) v *= 8;
      o.value = v;
    } else {
      int a;
      if (w & 0x4000) a = (rbase + (w & 0xF)) & (DATA_WORDS - 1);
      else {
        a = w & 0x3FF;
        if (w & 0x400) a = data[a] & 0x3FF;
      }
      o.addr = a;
      o.wb = w & 0x2000;
      o.value = int16_t(read(uint16_t(a)));
    }
  }
  int k = 0;
  auto next = [&]() -> int32_t { return k < nops ? ops[k++].value : 0; };
  int32_t prod = 0;
  if (muxa == 3 || muxb == 3) {
    const int32_t f1 = next();
    const int32_t f2 = m2sel ? int32_t(int16_t(acc)) : next();
    prod = (f1 * f2) >> 15;
  }
  auto pick = [&](int m) -> int32_t {
    switch (m) {
      case 0: return int16_t(acc);
      case 1: return next();
      case 2: return 0;
      default: return prod;
    }
  };
  const int32_t a = pick(muxa), b = pick(muxb);
  int32_t r = 0;
  bool c = fc, v = false;
  auto add = [&](int32_t x, int32_t y, int cin) {
    const uint32_t ux = uint16_t(x), uy = uint16_t(y);
    const uint32_t t = ux + uy + uint32_t(cin);
    c = t > 0xFFFF;
    const int32_t sum = x + y + cin;
    v = sum > 32767 || sum < -32768;
    r = sum;
  };
  switch (alu) {
    case 0: r = a; break;
    case 1: add(0, -a, 0); break;
    case 2: add(a, b, 0); break;
    case 3: add(a, b, fc ? 1 : 0); break;
    case 4: add(a, -b, 0); c = uint16_t(a) >= uint16_t(b); break;
    case 5: add(a, -b - (fc ? 0 : 1), 0); break;
    case 6: add(a, 1, 0); break;
    case 7: add(a, -1, 0); break;
    case 8: r = b; break;
    case 9: r = ~a; break;
    case 10: r = a & b; break;
    case 11: r = ~(a & b); break;
    case 12: r = a | b; break;
    case 13: r = ~(a | b); break;
    case 14: r = a ^ b; break;
    default: r = ~(a ^ b); break;
  }
  if (bs >= 1 && bs <= 5) r <<= bs;
  else if (bs == 6) r <<= 8;
  else if (bs == 7) { if (r > 32767) r = 32767; else if (r < -32768) r = -32768; }
  else if (bs == 8) r >>= 8;
  else if (bs >= 9) r >>= (bs - 8);
  const int16_t res = int16_t(r);
  acc = res;
  fz = res == 0;
  fn = res < 0;
  fc = c;
  fv = v;
  // resultado: um operando de endereço que sobrou, e os marcados com o bit de gravação
  if (k < nops && ops[k].addr >= 0) write(uint16_t(ops[k].addr), uint16_t(res));
  for (int i = 0; i < nops; i++)
    if (ops[i].wb && ops[i].addr >= 0 && !(i == k)) write(uint16_t(ops[i].addr), uint16_t(res));
  return false;
}

void Dspp::sync(StateIO& io) {
  io.raw(code, sizeof code);
  io.raw(data, sizeof data);
  io.sync(running);
  io.sync(pc); io.sync(rbase); io.sync(acc);
  io.sync(fz); io.sync(fn); io.sync(fc); io.sync(fv);
  io.raw(stack, sizeof stack);
  io.sync(sp);
  io.sync(frameCount); io.sync(noise);
  io.sync(outL); io.sync(outR);
  if (io.reading()) {
    pc &= CODE_WORDS - 1;
    rbase &= DATA_WORDS - 1;
    if (sp > 4) sp = 4;
    if (!noise) noise = 1;
  }
}

}  // namespace indigo::tdo
