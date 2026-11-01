// Super FX (GSU): instruções, cache, buffers de ROM/RAM, PLOT e os registradores vistos pelo SNES.
// Tempo: o GSU anda no relógio mestre do SNES (21,477 MHz; com CLSR = 0, a metade). Cada busca de instrução no
// cache custa 1 ciclo (2 a 10,7 MHz), fora do cache 5 (6); leituras da RAM e da ROM e as multiplicações sem o
// modo rápido (CFGR.MS0) somam os ciclos do chip.
#include "gsu.h"

namespace indigo::snes {

namespace {
size_t wrap(size_t addr, size_t size) {
  if (!size) return 0;
  if (!(size & (size - 1))) return addr & (size - 1);
  size_t base = 0, mask = size_t(1) << 23;  // tamanhos que não são potência de 2: espelho como no cartucho
  while (addr >= size) {
    while (!(addr & mask)) mask >>= 1;
    addr -= mask;
    if (size > mask) { size -= mask; base += mask; }
    mask >>= 1;
  }
  return base + addr;
}
}  // namespace

void GSU::reset() {
  for (auto& x : r) x = 0;
  sfr = 0;
  pbr = rombr = rambr = cfgr = scbr = clsr = scmr = colr = por = 0;
  cbr = ramaddr = 0;
  romdr = 0;
  pipeline = 0x01;
  sreg = dreg = 0;
  r15Modified = r14Modified = false;
  for (auto& c : cache) c = 0;
  flushCache();
  pixel[0] = pixel[1] = PixelCache{};
  clocks_ = 0;
}

uint8_t GSU::romAt(uint32_t addr) const {
  if (rom_.empty()) return 0;
  size_t off;
  if ((addr & 0x400000) == 0) off = ((addr & 0x3F0000) >> 1) | (addr & 0x7FFF);
  else off = addr & 0x1FFFFF;
  return rom_[wrap(off, rom_.size())];
}

uint8_t GSU::busRead(uint32_t addr) {
  if ((addr & 0xC00000) == 0x000000) return romAt(addr);
  if ((addr & 0xE00000) == 0x400000) return romAt(addr);
  if ((addr & 0xE00000) == 0x600000 && !ram_.empty()) return ram_[wrap(addr & 0x1FFFF, ram_.size())];
  return 0;
}

void GSU::busWrite(uint32_t addr, uint8_t v) {
  if ((addr & 0xE00000) == 0x600000 && !ram_.empty()) ram_[wrap(addr & 0x1FFFF, ram_.size())] = v;
}

void GSU::setReg(int n, uint16_t v) {
  r[n] = v;
  if (n == 14) r14Modified = true;
  if (n == 15) r15Modified = true;
}

uint8_t GSU::fetch(uint16_t addr) {
  const uint16_t offset = static_cast<uint16_t>(addr - cbr);
  if (offset < 512) {
    if (!cacheValid[offset >> 4]) {
      const uint16_t dp = offset & 0x1F0;
      for (int n = 0; n < 16; n++) {
        step(clsr ? 5 : 6);
        cache[dp + n] = busRead((uint32_t(pbr) << 16) | static_cast<uint16_t>(((cbr + dp) & 0xFFF0) + n));
      }
      cacheValid[offset >> 4] = true;
    } else {
      step(clsr ? 1 : 2);
    }
    return cache[offset];
  }
  step(clsr ? 5 : 6);
  return busRead((uint32_t(pbr) << 16) | addr);
}

uint8_t GSU::pipe() {
  const uint8_t v = pipeline;
  r[15]++;
  pipeline = fetch(r[15]);
  r15Modified = false;
  return v;
}

void GSU::run(int64_t master) {
  if (!(sfr & G)) { clocks_ = 0; return; }
  clocks_ += master;
  int guard = 0;
  while (clocks_ > 0 && (sfr & G)) {
    const uint8_t op = pipeline;
    pipeline = fetch(r[15]);
    r15Modified = false;
    execute(op);
    if (r14Modified) { r14Modified = false; updateRomBuffer(); }
    if (r15Modified) r15Modified = false;
    else r[15]++;
    if (++guard > 1000000) break;
  }
  if (!(sfr & G)) clocks_ = 0;
}

// ---------------------------------------------------------------- PLOT e RPIX
uint8_t GSU::color(uint8_t source) const {
  if (por & 0x04) return static_cast<uint8_t>((colr & 0xF0) | (source >> 4));
  if (por & 0x08) return static_cast<uint8_t>((colr & 0xF0) | (source & 0x0F));
  return source;
}

uint32_t GSU::tileAddr(uint8_t x, uint8_t y) const {
  const int ht = (por & 0x10) ? 3 : (((scmr >> 2) & 1) | ((scmr >> 4) & 2));
  uint32_t cn;
  switch (ht) {
    case 0: cn = ((x & 0xF8) << 1) + ((y & 0xF8) >> 3); break;
    case 1: cn = ((x & 0xF8) << 1) + ((x & 0xF8) >> 1) + ((y & 0xF8) >> 3); break;
    case 2: cn = ((x & 0xF8) << 1) + (x & 0xF8) + ((y & 0xF8) >> 3); break;
    default: cn = ((y & 0x80) << 2) + ((x & 0x80) << 1) + ((y & 0x78) << 1) + ((x & 0x78) >> 3); break;
  }
  return 0x700000 + cn * uint32_t(bpp() << 3) + (uint32_t(scbr) << 10) + uint32_t(y & 7) * 2;
}

void GSU::flushPixels(PixelCache& c) {
  if (!c.bitpend) return;
  const uint8_t x = static_cast<uint8_t>(c.offset << 3);
  const uint8_t y = static_cast<uint8_t>(c.offset >> 5);
  const uint32_t addr = tileAddr(x, y);
  const int n = bpp();
  for (int p = 0; p < n; p++) {
    const uint32_t byte = uint32_t((p >> 1) << 4) + (p & 1);
    uint8_t data = 0;
    for (int k = 0; k < 8; k++) data = static_cast<uint8_t>(data | (((c.data[k] >> p) & 1) << k));
    if (c.bitpend != 0xFF) {
      step(clsr ? 5 : 6);
      data = static_cast<uint8_t>((data & c.bitpend) | (busRead(addr + byte) & ~c.bitpend));
    }
    step(clsr ? 5 : 6);
    busWrite(addr + byte, data);
  }
  c.bitpend = 0;
}

void GSU::plot(uint8_t x, uint8_t y) {
  if (!(por & 0x01)) {  // sem o modo transparente, a cor 0 não desenha
    if ((scmr & 3) == 3) {
      if (por & 0x08) { if (!(colr & 0x0F)) return; }
      else if (!colr) return;
    } else if (!(colr & 0x0F)) {
      return;
    }
  }
  uint8_t c = colr;
  if ((por & 0x02) && (scmr & 3) != 3) {  // pontilhado: cor alta e baixa em xadrez
    if ((x ^ y) & 1) c >>= 4;
    c &= 0x0F;
  }
  const uint16_t offset = static_cast<uint16_t>((y << 5) + (x >> 3));
  if (pixel[0].offset != offset) {
    flushPixels(pixel[1]);
    pixel[1] = pixel[0];
    pixel[0].bitpend = 0;
    pixel[0].offset = offset;
  }
  const int bit = (x & 7) ^ 7;
  pixel[0].data[bit] = c;
  pixel[0].bitpend = static_cast<uint8_t>(pixel[0].bitpend | (1 << bit));
  if (pixel[0].bitpend == 0xFF) {
    flushPixels(pixel[1]);
    pixel[1] = pixel[0];
    pixel[0].bitpend = 0;
  }
}

uint8_t GSU::rpix(uint8_t x, uint8_t y) {
  flushPixels(pixel[1]);
  flushPixels(pixel[0]);
  const uint32_t addr = tileAddr(x, y);
  const int bit = (x & 7) ^ 7;
  uint8_t data = 0;
  for (int p = 0; p < bpp(); p++) {
    const uint32_t byte = uint32_t((p >> 1) << 4) + (p & 1);
    step(clsr ? 5 : 6);
    data = static_cast<uint8_t>(data | (((busRead(addr + byte) >> bit) & 1) << p));
  }
  return data;
}

// ---------------------------------------------------------------- instruções
void GSU::execute(uint8_t op) {
  const int n = op & 0x0F;
  switch (op >> 4) {
    case 0x0:
      switch (op) {
        case 0x00:  // STOP
          if (!(cfgr & 0x80)) sfr |= IRQ;
          sfr &= static_cast<uint16_t>(~G);
          pipeline = 0x01;
          resetPrefix();
          return;
        case 0x01: resetPrefix(); return;  // NOP
        case 0x02:  // CACHE
          if (cbr != (r[15] & 0xFFF0)) { cbr = r[15] & 0xFFF0; flushCache(); }
          resetPrefix();
          return;
        case 0x03: {  // LSR
          const uint16_t s = sr();
          setFlag(CY, s & 1);
          const uint16_t v = static_cast<uint16_t>(s >> 1);
          dr(v); flags(v); resetPrefix();
          return;
        }
        case 0x04: {  // ROL
          const uint16_t s = sr();
          const uint16_t v = static_cast<uint16_t>((s << 1) | ((sfr & CY) ? 1 : 0));
          setFlag(CY, s & 0x8000);
          dr(v); flags(v); resetPrefix();
          return;
        }
        default: {  // desvios relativos (BRA, BGE, BLT, BNE, BEQ, BPL, BMI, BCC, BCS, BVC, BVS)
          const int8_t e = static_cast<int8_t>(pipe());
          const bool s = sfr & S, z = sfr & Z, cy = sfr & CY, ov = sfr & OV;
          bool take = false;
          switch (op) {
            case 0x05: take = true; break;
            case 0x06: take = s == ov; break;
            case 0x07: take = s != ov; break;
            case 0x08: take = !z; break;
            case 0x09: take = z; break;
            case 0x0A: take = !s; break;
            case 0x0B: take = s; break;
            case 0x0C: take = !cy; break;
            case 0x0D: take = cy; break;
            case 0x0E: take = !ov; break;
            default: take = ov; break;
          }
          if (take) setReg(15, static_cast<uint16_t>(r[15] + e));
          return;
        }
      }
    case 0x1:  // TO / MOVE
      if (!(sfr & B)) { dreg = static_cast<uint8_t>(n); return; }
      setReg(n, sr());
      resetPrefix();
      return;
    case 0x2:  // WITH
      sreg = dreg = static_cast<uint8_t>(n);
      sfr |= B;
      return;
    case 0x3:
      if (n <= 11) {  // STW / STB (Rn)
        ramaddr = r[n];
        ramWrite(ramaddr, static_cast<uint8_t>(sr()));
        if (!alt1()) ramWrite(ramaddr ^ 1, static_cast<uint8_t>(sr() >> 8));
        resetPrefix();
        return;
      }
      switch (op) {
        case 0x3C: {  // LOOP
          const uint16_t v = static_cast<uint16_t>(r[12] - 1);
          setReg(12, v);
          flags(v);
          if (v) setReg(15, r[13]);
          resetPrefix();
          return;
        }
        case 0x3D: sfr = static_cast<uint16_t>((sfr & ~B) | ALT1); return;
        case 0x3E: sfr = static_cast<uint16_t>((sfr & ~B) | ALT2); return;
        default: sfr = static_cast<uint16_t>((sfr & ~B) | ALT1 | ALT2); return;
      }
    case 0x4:
      if (n <= 11) {  // LDW / LDB (Rn)
        ramaddr = r[n];
        uint16_t v = ramRead(ramaddr);
        if (!alt1()) v = static_cast<uint16_t>(v | (ramRead(ramaddr ^ 1) << 8));
        dr(v);
        resetPrefix();
        return;
      }
      switch (op) {
        case 0x4C:
          if (!alt1()) {  // PLOT
            plot(static_cast<uint8_t>(r[1]), static_cast<uint8_t>(r[2]));
            setReg(1, static_cast<uint16_t>(r[1] + 1));
          } else {  // RPIX
            const uint16_t v = rpix(static_cast<uint8_t>(r[1]), static_cast<uint8_t>(r[2]));
            dr(v); flags(v);
          }
          resetPrefix();
          return;
        case 0x4D: {  // SWAP
          const uint16_t v = static_cast<uint16_t>((sr() >> 8) | (sr() << 8));
          dr(v); flags(v); resetPrefix();
          return;
        }
        case 0x4E:
          if (!alt1()) colr = color(static_cast<uint8_t>(sr()));  // COLOR
          else por = static_cast<uint8_t>(sr() & 0x1F);          // CMODE
          resetPrefix();
          return;
        default: {  // NOT
          const uint16_t v = static_cast<uint16_t>(~sr());
          dr(v); flags(v); resetPrefix();
          return;
        }
      }
    case 0x5: {  // ADD / ADC / ADD # / ADC #
      const uint16_t s = sr();
      const uint16_t m = alt2() ? static_cast<uint16_t>(n) : r[n];
      const int v = s + m + ((alt1() && (sfr & CY)) ? 1 : 0);
      setFlag(OV, ~(s ^ m) & (m ^ v) & 0x8000);
      setFlag(S, v & 0x8000);
      setFlag(CY, v >= 0x10000);
      setFlag(Z, static_cast<uint16_t>(v) == 0);
      dr(static_cast<uint16_t>(v));
      resetPrefix();
      return;
    }
    case 0x6: {  // SUB / SBC / SUB # / CMP
      const uint16_t s = sr();
      const bool cmp = alt1() && alt2();
      const uint16_t m = (alt2() && !cmp) ? static_cast<uint16_t>(n) : r[n];
      const int v = s - m - ((alt1() && !alt2() && !(sfr & CY)) ? 1 : 0);
      setFlag(OV, (s ^ m) & (s ^ v) & 0x8000);
      setFlag(S, v & 0x8000);
      setFlag(CY, v >= 0);
      setFlag(Z, static_cast<uint16_t>(v) == 0);
      if (!cmp) dr(static_cast<uint16_t>(v));
      resetPrefix();
      return;
    }
    case 0x7: {
      if (op == 0x70) {  // MERGE
        const uint16_t v = static_cast<uint16_t>((r[7] & 0xFF00) | (r[8] >> 8));
        dr(v);
        setFlag(OV, v & 0xC0C0);
        setFlag(S, v & 0x8080);
        setFlag(CY, v & 0xE0E0);
        setFlag(Z, v & 0xF0F0);
        resetPrefix();
        return;
      }
      uint16_t m = alt2() ? static_cast<uint16_t>(n) : r[n];  // AND / BIC / AND # / BIC #
      if (alt1()) m = static_cast<uint16_t>(~m);
      const uint16_t v = static_cast<uint16_t>(sr() & m);
      dr(v); flags(v); resetPrefix();
      return;
    }
    case 0x8: {  // MULT / UMULT / MULT # / UMULT #
      const uint16_t m = alt2() ? static_cast<uint16_t>(n) : r[n];
      uint16_t v;
      if (!alt1()) v = static_cast<uint16_t>(static_cast<int8_t>(sr()) * static_cast<int8_t>(m));
      else v = static_cast<uint16_t>(static_cast<uint8_t>(sr()) * static_cast<uint8_t>(m));
      dr(v); flags(v);
      if (!(cfgr & 0x20)) step(clsr ? 1 : 2);
      resetPrefix();
      return;
    }
    case 0x9:
      switch (op) {
        case 0x90:  // SBK
          ramWrite(ramaddr, static_cast<uint8_t>(sr()));
          ramWrite(ramaddr ^ 1, static_cast<uint8_t>(sr() >> 8));
          resetPrefix();
          return;
        case 0x91: case 0x92: case 0x93: case 0x94:  // LINK #n
          setReg(11, static_cast<uint16_t>(r[15] + n));
          resetPrefix();
          return;
        case 0x95: {  // SEX
          const uint16_t v = static_cast<uint16_t>(static_cast<int8_t>(sr()));
          dr(v); flags(v); resetPrefix();
          return;
        }
        case 0x96: {  // ASR / DIV2
          const uint16_t s = sr();
          setFlag(CY, s & 1);
          uint16_t v = static_cast<uint16_t>(static_cast<int16_t>(s) >> 1);
          if (alt1()) v = static_cast<uint16_t>(v + ((uint32_t(s) + 1) >> 16));
          dr(v); flags(v); resetPrefix();
          return;
        }
        case 0x97: {  // ROR
          const uint16_t s = sr();
          const uint16_t v = static_cast<uint16_t>(((sfr & CY) ? 0x8000 : 0) | (s >> 1));
          setFlag(CY, s & 1);
          dr(v); flags(v); resetPrefix();
          return;
        }
        case 0x9E: {  // LOB
          const uint16_t v = sr() & 0xFF;
          dr(v); setFlag(S, v & 0x80); setFlag(Z, v == 0); resetPrefix();
          return;
        }
        case 0x9F: {  // FMULT / LMULT
          const int32_t res = static_cast<int16_t>(sr()) * static_cast<int16_t>(r[6]);
          if (alt1()) setReg(4, static_cast<uint16_t>(res));
          const uint16_t v = static_cast<uint16_t>(static_cast<uint32_t>(res) >> 16);
          dr(v);
          setFlag(S, v & 0x8000);
          setFlag(CY, res & 0x8000);
          setFlag(Z, v == 0);
          step(((cfgr & 0x20) ? 3 : 7) * (clsr ? 1 : 2));
          resetPrefix();
          return;
        }
        default:  // JMP / LJMP (R8–R13)
          if (!alt1()) {
            setReg(15, r[n]);
          } else {
            pbr = static_cast<uint8_t>(r[n] & 0x7F);
            setReg(15, sr());
            cbr = r[15] & 0xFFF0;
            flushCache();
          }
          resetPrefix();
          return;
      }
    case 0xA:
      if (alt1()) {  // LMS Rn,(yy)
        ramaddr = static_cast<uint16_t>(pipe() << 1);
        const uint16_t lo = ramRead(ramaddr);
        setReg(n, static_cast<uint16_t>(lo | (ramRead(ramaddr ^ 1) << 8)));
      } else if (alt2()) {  // SMS (yy),Rn
        ramaddr = static_cast<uint16_t>(pipe() << 1);
        ramWrite(ramaddr, static_cast<uint8_t>(r[n]));
        ramWrite(ramaddr ^ 1, static_cast<uint8_t>(r[n] >> 8));
      } else {  // IBT Rn,#pp
        setReg(n, static_cast<uint16_t>(static_cast<int8_t>(pipe())));
      }
      resetPrefix();
      return;
    case 0xB:  // FROM / MOVES
      if (!(sfr & B)) { sreg = static_cast<uint8_t>(n); return; }
      {
        const uint16_t v = r[n];
        dr(v);
        setFlag(OV, v & 0x80);
        flags(v);
        resetPrefix();
      }
      return;
    case 0xC: {
      if (op == 0xC0) {  // HIB
        const uint16_t v = sr() >> 8;
        dr(v); setFlag(S, v & 0x80); setFlag(Z, v == 0); resetPrefix();
        return;
      }
      const uint16_t m = alt2() ? static_cast<uint16_t>(n) : r[n];  // OR / XOR / OR # / XOR #
      const uint16_t v = alt1() ? static_cast<uint16_t>(sr() ^ m) : static_cast<uint16_t>(sr() | m);
      dr(v); flags(v); resetPrefix();
      return;
    }
    case 0xD:
      if (op != 0xDF) {  // INC Rn
        const uint16_t v = static_cast<uint16_t>(r[n] + 1);
        setReg(n, v); flags(v); resetPrefix();
        return;
      }
      if (alt2() && alt1()) rombr = static_cast<uint8_t>(sr() & 0x7F);  // ROMB
      else if (alt2()) rambr = static_cast<uint8_t>(sr() & 0x01);       // RAMB
      else colr = color(romdr);                                         // GETC
      resetPrefix();
      return;
    default:  // 0xE e 0xF
      if (op == 0xEF) {  // GETB / GETBH / GETBL / GETBS
        uint16_t v;
        if (alt1() && alt2()) v = static_cast<uint16_t>(static_cast<int8_t>(romdr));
        else if (alt1()) v = static_cast<uint16_t>((romdr << 8) | (sr() & 0xFF));
        else if (alt2()) v = static_cast<uint16_t>((sr() & 0xFF00) | romdr);
        else v = romdr;
        dr(v);
        resetPrefix();
        return;
      }
      if ((op >> 4) == 0xE) {  // DEC Rn
        const uint16_t v = static_cast<uint16_t>(r[n] - 1);
        setReg(n, v); flags(v); resetPrefix();
        return;
      }
      if (alt1()) {  // LM Rn,(xx)
        ramaddr = pipe();
        ramaddr = static_cast<uint16_t>(ramaddr | (pipe() << 8));
        const uint16_t lo = ramRead(ramaddr);
        setReg(n, static_cast<uint16_t>(lo | (ramRead(ramaddr ^ 1) << 8)));
      } else if (alt2()) {  // SM (xx),Rn
        ramaddr = pipe();
        ramaddr = static_cast<uint16_t>(ramaddr | (pipe() << 8));
        ramWrite(ramaddr, static_cast<uint8_t>(r[n]));
        ramWrite(ramaddr ^ 1, static_cast<uint8_t>(r[n] >> 8));
      } else {  // IWT Rn,#xx
        const uint8_t lo = pipe();
        setReg(n, static_cast<uint16_t>(lo | (pipe() << 8)));
      }
      resetPrefix();
      return;
  }
}

// ---------------------------------------------------------------- registradores do lado do SNES
uint8_t GSU::readReg(uint16_t a) {
  a = static_cast<uint16_t>(0x3000 | (a & 0x3FF));
  if (a >= 0x3100 && a < 0x3300) return cache[(cbr + (a - 0x3100)) & 511];
  if (a < 0x3020) { const uint16_t v = r[(a >> 1) & 15]; return static_cast<uint8_t>((a & 1) ? v >> 8 : v); }
  switch (a) {
    case 0x3030: return static_cast<uint8_t>(sfr);
    case 0x3031: { const uint8_t v = static_cast<uint8_t>(sfr >> 8); sfr &= static_cast<uint16_t>(~IRQ); return v; }
    case 0x3034: return pbr;
    case 0x3036: return rombr;
    case 0x303B: return 0x04;  // versão do chip
    case 0x303C: return rambr;
    case 0x303E: return static_cast<uint8_t>(cbr);
    case 0x303F: return static_cast<uint8_t>(cbr >> 8);
    default: return 0;
  }
}

void GSU::writeReg(uint16_t a, uint8_t v) {
  a = static_cast<uint16_t>(0x3000 | (a & 0x3FF));
  if (a >= 0x3100 && a < 0x3300) {
    const int off = (cbr + (a - 0x3100)) & 511;
    cache[off] = v;
    if ((a & 15) == 15) cacheValid[off >> 4] = true;
    return;
  }
  if (a < 0x3020) {
    const int n = (a >> 1) & 15;
    r[n] = (a & 1) ? static_cast<uint16_t>((v << 8) | (r[n] & 0xFF)) : static_cast<uint16_t>((r[n] & 0xFF00) | v);
    if (n == 14) updateRomBuffer();
    if (a == 0x301F) sfr |= G;
    return;
  }
  switch (a) {
    case 0x3030: {
      const bool g = sfr & G;
      sfr = static_cast<uint16_t>((sfr & 0xFF00) | v);
      if (g && !(sfr & G)) { cbr = 0; flushCache(); }
      return;
    }
    case 0x3031: sfr = static_cast<uint16_t>((v << 8) | (sfr & 0xFF)); return;
    case 0x3034: pbr = v & 0x7F; flushCache(); return;
    case 0x3037: cfgr = v; return;
    case 0x3038: scbr = v; return;
    case 0x3039: clsr = v & 1; return;
    case 0x303A: scmr = v; return;
    default: return;
  }
}

void GSU::sync(StateIO& io) {
  io.raw(r, sizeof r);
  io.sync(sfr);
  io.sync(pbr); io.sync(rombr); io.sync(rambr); io.sync(cfgr); io.sync(scbr); io.sync(clsr); io.sync(scmr);
  io.sync(colr); io.sync(por);
  io.sync(cbr); io.sync(ramaddr); io.sync(romdr); io.sync(pipeline); io.sync(sreg); io.sync(dreg);
  io.sync(r15Modified); io.sync(r14Modified);
  io.raw(cache, sizeof cache);
  for (bool& b : cacheValid) io.sync(b);
  for (auto& p : pixel) { io.sync(p.offset); io.sync(p.bitpend); io.raw(p.data, sizeof p.data); }
  io.sync(clocks_);
  if (io.reading()) {
    sreg &= 15; dreg &= 15; rambr &= 1; pbr &= 0x7F; rombr &= 0x7F; clsr &= 1;
    clampTo<int64_t>(clocks_, -100000, 100000);
  }
}

}  // namespace indigo::snes
