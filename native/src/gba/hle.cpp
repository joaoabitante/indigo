// BIOS em alto nível (porte de src/bios.js): substitui a BIOS proprietária da Nintendo. Instala um pequeno
// tratador de IRQ em código ARM e emula as chamadas SWI em C++.
//
// Diferenças deliberadas em relação ao JS (só em entradas que travariam a versão web num laço infinito):
// BitUnPack com largura de origem 0 e Huffman com 0 bits por símbolo/árvore sem folhas retornam em vez de travar.
#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "gba.h"

namespace indigo::gba {

namespace {
// gerado de src/bios.js (Math.round(Math.sin/cos(θ)·16384), θ = n/256·2π) para ficar idêntico à versão web
const int16_t SIN_T[256] = {0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,16384,16379,16364,16340,16305,16261,16207,16143,16069,15986,15893,15791,15679,15557,15426,15286,15137,14978,14811,14635,14449,14256,14053,13842,13623,13395,13160,12916,12665,12406,12140,11866,11585,11297,11003,10702,10394,10080,9760,9434,9102,8765,8423,8076,7723,7366,7005,6639,6270,5897,5520,5139,4756,4370,3981,3590,3196,2801,2404,2006,1606,1205,804,402,0,-402,-804,-1205,-1606,-2006,-2404,-2801,-3196,-3590,-3981,-4370,-4756,-5139,-5520,-5897,-6270,-6639,-7005,-7366,-7723,-8076,-8423,-8765,-9102,-9434,-9760,-10080,-10394,-10702,-11003,-11297,-11585,-11866,-12140,-12406,-12665,-12916,-13160,-13395,-13623,-13842,-14053,-14256,-14449,-14635,-14811,-14978,-15137,-15286,-15426,-15557,-15679,-15791,-15893,-15986,-16069,-16143,-16207,-16261,-16305,-16340,-16364,-16379,-16384,-16379,-16364,-16340,-16305,-16261,-16207,-16143,-16069,-15986,-15893,-15791,-15679,-15557,-15426,-15286,-15137,-14978,-14811,-14635,-14449,-14256,-14053,-13842,-13623,-13395,-13160,-12916,-12665,-12406,-12140,-11866,-11585,-11297,-11003,-10702,-10394,-10080,-9760,-9434,-9102,-8765,-8423,-8076,-7723,-7366,-7005,-6639,-6270,-5897,-5520,-5139,-4756,-4370,-3981,-3590,-3196,-2801,-2404,-2006,-1606,-1205,-804,-402};
const int16_t COS_T[256] = {16384,16379,16364,16340,16305,16261,16207,16143,16069,15986,15893,15791,15679,15557,15426,15286,15137,14978,14811,14635,14449,14256,14053,13842,13623,13395,13160,12916,12665,12406,12140,11866,11585,11297,11003,10702,10394,10080,9760,9434,9102,8765,8423,8076,7723,7366,7005,6639,6270,5897,5520,5139,4756,4370,3981,3590,3196,2801,2404,2006,1606,1205,804,402,0,-402,-804,-1205,-1606,-2006,-2404,-2801,-3196,-3590,-3981,-4370,-4756,-5139,-5520,-5897,-6270,-6639,-7005,-7366,-7723,-8076,-8423,-8765,-9102,-9434,-9760,-10080,-10394,-10702,-11003,-11297,-11585,-11866,-12140,-12406,-12665,-12916,-13160,-13395,-13623,-13842,-14053,-14256,-14449,-14635,-14811,-14978,-15137,-15286,-15426,-15557,-15679,-15791,-15893,-15986,-16069,-16143,-16207,-16261,-16305,-16340,-16364,-16379,-16384,-16379,-16364,-16340,-16305,-16261,-16207,-16143,-16069,-15986,-15893,-15791,-15679,-15557,-15426,-15286,-15137,-14978,-14811,-14635,-14449,-14256,-14053,-13842,-13623,-13395,-13160,-12916,-12665,-12406,-12140,-11866,-11585,-11297,-11003,-10702,-10394,-10080,-9760,-9434,-9102,-8765,-8423,-8076,-7723,-7366,-7005,-6639,-6270,-5897,-5520,-5139,-4756,-4370,-3981,-3590,-3196,-2801,-2404,-2006,-1606,-1205,-804,-402,0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,5897,6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,10702,11003,11297,11585,11866,12140,12406,12665,12916,13160,13395,13623,13842,14053,14256,14449,14635,14811,14978,15137,15286,15426,15557,15679,15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379};

const uint32_t IRQ_STUB[][2] = {
  // 0x00 reset, 0x04 und, 0x08 swi, 0x0C pabt, 0x10 dabt, 0x14 -, 0x18 irq, 0x1C fiq
  {0x00, 0xEA00001E},   // b 0x80 (laço de segurança)
  {0x18, 0xEA000042},   // b 0x128
  {0x80, 0xEAFFFFFE},   // b .
  {0x128, 0xE92D500F},  // stmfd sp!, {r0-r3, r12, lr}
  {0x12C, 0xE3A00301},  // mov r0, #0x04000000
  {0x130, 0xE28FE000},  // add lr, pc, #0
  {0x134, 0xE510F004},  // ldr pc, [r0, #-4]
  {0x138, 0xE8BD500F},  // ldmfd sp!, {r0-r3, r12, lr}
  {0x13C, 0xE25EF004},  // subs pc, lr, #4
  {0x144, 0xE55EC002},  // (nunca executada; é o valor de prefetch após o retorno da IRQ)
};

inline int32_t imul(int64_t a, int64_t b) { return I32(static_cast<int64_t>(static_cast<uint64_t>(I32(a)) * static_cast<uint64_t>(I32(b)))); }
constexpr double kPi = 3.141592653589793;  // Math.PI
inline int32_t sx16(int32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

// Math.round do JS (metades sempre para cima)
double jsRound(double x) {
  const double f = std::floor(x);
  return (x - f >= 0.5) ? f + 1 : f;
}
// x >>> 0 de um double
uint32_t toUint32(double x) {
  if (!std::isfinite(x)) return 0;
  double t = std::trunc(x);
  t = std::fmod(t, 4294967296.0);
  if (t < 0) t += 4294967296.0;
  return static_cast<uint32_t>(t);
}

// endereço Number do JS (double possivelmente enorme) para o int64 que o barramento recebe: mesmo valor
// módulo 2^32 (é o que >>> e & usam) e mesmo sinal/ordem em relação a 0 e 2^32 (o que as comparações usam)
int64_t jsAddr(double v) {
  if (v >= 0 && v < 4294967296.0) return static_cast<int64_t>(v);
  if (v < 0 && v >= -4294967296.0) return static_cast<int64_t>(v);
  if (!std::isfinite(v)) return 0;
  double m = std::fmod(v, 4294967296.0);
  if (m < 0) m += 4294967296.0;
  const int64_t low = static_cast<int64_t>(m);
  return v > 0 ? (int64_t{1} << 32) + low : low - (int64_t{1} << 32);
}

// fluxo de bytes descomprimidos (em VRAM somente em unidades de 16 bits)
struct Writer {
  Bus& bus;
  int64_t d;
  bool vram;
  int pending = -1;
  std::vector<uint8_t> buf;
  Writer(Bus& b, int64_t dst, bool v) : bus(b), d(dst), vram(v) {}
  void put(int b) {
    buf.push_back(static_cast<uint8_t>(b));
    if (!vram) { bus.write8(d++, b); return; }
    if (pending < 0) pending = b;
    else { bus.write16(d, pending | (b << 8)); d += 2; pending = -1; }
  }
  // no JS um deslocamento além do início devolve undefined, que se comporta como 0
  int get(int64_t back) const { return back <= static_cast<int64_t>(buf.size()) ? buf[buf.size() - static_cast<size_t>(back)] : 0; }
  int64_t length() const { return static_cast<int64_t>(buf.size()); }
};
}  // namespace

void installHleBios(Bus& bus) {
  std::fill(bus.bios.begin(), bus.bios.end(), 0);
  for (const auto& e : IRQ_STUB) Bus::st32(bus.bios, e[0], e[1]);
}

void HleBios::handle(int n, int64_t addr) {
  CPU& cpu = gba.cpu;
  int32_t* r = cpu.r;
  Bus& bus = gba.bus;
  bus.cycles += 8;
  bus.biosLast = static_cast<int32_t>(0xE3A02004u);  // valor que a BIOS real deixa no barramento após um SWI
  switch (n) {
    case 0x00: softReset(); break;
    case 0x01: registerRamReset(r[0]); break;
    case 0x02: cpu.halted = true; break;
    case 0x03: cpu.halted = true; break;
    case 0x04: intrWait(r[0], r[1], addr); break;
    case 0x05: r[0] = 1; r[1] = 1; intrWait(1, 1, addr); break;
    case 0x06: div(r[0], r[1]); break;
    case 0x07: div(r[1], r[0]); break;
    case 0x08: r[0] = I32(static_cast<int64_t>(std::floor(std::sqrt(static_cast<double>(U32(r[0])))))); break;
    case 0x09: arcTan(); break;
    case 0x0A: arcTan2(); break;
    case 0x0B: cpuSet(); break;
    case 0x0C: cpuFastSet(); break;
    case 0x0D: r[0] = static_cast<int32_t>(0xBAAE187Fu); break;
    case 0x0E: bgAffineSet(); break;
    case 0x0F: objAffineSet(); break;
    case 0x10: bitUnPack(); break;
    case 0x11: lz77(false); break;
    case 0x12: lz77(true); break;
    case 0x13: huffman(); break;
    case 0x14: rle(false); break;
    case 0x15: rle(true); break;
    case 0x16: diff(8, false); break;
    case 0x17: diff(8, true); break;
    case 0x18: diff(16, true); break;
    case 0x19: break;  // SoundBias
    case 0x1F: midiKey2Freq(); break;
    default: break;
  }
}

void HleBios::softReset() {
  Bus& bus = gba.bus;
  CPU& cpu = gba.cpu;
  const int flag = bus.iwram[0x7FFA];
  for (int a = 0x7E00; a < 0x8000; a++) bus.iwram[a] = 0;
  cpu.switchMode(0x13); cpu.r[13] = 0x03007FE0; cpu.r[14] = 0; cpu.setSpsr(0);
  cpu.switchMode(0x12); cpu.r[13] = 0x03007FA0; cpu.r[14] = 0; cpu.setSpsr(0);
  cpu.switchMode(0x1F); cpu.r[13] = 0x03007F00;
  for (int k = 0; k < 13; k++) cpu.r[k] = 0;
  cpu.t = 0;
  cpu.setPC(flag ? 0x02000000 : 0x08000000);
}

void HleBios::registerRamReset(int32_t flags) {
  Bus& bus = gba.bus;
  IO& io = gba.io;
  if (flags & 0x01) std::fill(bus.ewram.begin(), bus.ewram.end(), 0);
  if (flags & 0x02) std::fill(bus.iwram.begin(), bus.iwram.begin() + 0x7E00, 0);
  if (flags & 0x04) std::fill(bus.pal.begin(), bus.pal.end(), 0);
  if (flags & 0x08) std::fill(bus.vram.begin(), bus.vram.end(), 0);
  if (flags & 0x10) std::fill(bus.oam.begin(), bus.oam.end(), 0);
  if (flags & 0x80) {
    for (int a = 0x8; a < 0x60; a += 2) io.write16(a, 0);
    io.write16(0x20, 0x100); io.write16(0x26, 0x100); io.write16(0x30, 0x100); io.write16(0x36, 0x100);
  }
  io.write16(0, 0x80);
}

void HleBios::intrWait(int32_t discard, int32_t mask, int64_t addr) {
  Bus& bus = gba.bus;
  IO& io = gba.io;
  CPU& cpu = gba.cpu;
  if (!waiting) {
    waiting = true;
    if (discard) Bus::st16(bus.iwram, 0x7FF8, Bus::ld16(bus.iwram, 0x7FF8) & U32(~mask));
  }
  io.ime = 1;
  const int32_t flags = Bus::ld16(bus.iwram, 0x7FF8);
  if (flags & mask) {
    Bus::st16(bus.iwram, 0x7FF8, U32(flags & ~mask));
    waiting = false;
    return;
  }
  cpu.halted = true;
  cpu.setPC(addr);  // reexecuta o SWI ao acordar
}

void HleBios::div(int32_t num, int32_t den) {
  int32_t* r = gba.cpu.r;
  if (den == 0) { r[0] = num < 0 ? -1 : 1; r[1] = num; r[3] = 1; return; }
  // (num / den) | 0 do JS: a divisão em double é exata o bastante para truncar igual à divisão inteira
  const int64_t q64 = static_cast<int64_t>(num) / static_cast<int64_t>(den);
  const int32_t q = I32(q64);
  r[0] = q;
  r[1] = I32(static_cast<int64_t>(num) - imul(q, den));
  r[3] = I32(std::llabs(static_cast<long long>(q)));
}

void HleBios::arcTan() {
  int32_t* r = gba.cpu.r;
  const int32_t x = r[0];
  const int32_t a = I32(-static_cast<int64_t>(imul(x, x) >> 14));
  int32_t b = (imul(0xA9, a) >> 14) + 0x390;
  b = (imul(b, a) >> 14) + 0x91C;
  b = (imul(b, a) >> 14) + 0xFB6;
  b = (imul(b, a) >> 14) + 0x16AA;
  b = (imul(b, a) >> 14) + 0x2081;
  b = (imul(b, a) >> 14) + 0x3651;
  b = (imul(b, a) >> 14) + 0xA2F9;
  r[0] = imul(x, b) >> 16;
}

void HleBios::arcTan2() {
  int32_t* r = gba.cpu.r;
  const int32_t x = sx16(r[0]), y = sx16(r[1]);
  double ang = std::atan2(static_cast<double>(y), static_cast<double>(x));
  if (ang < 0) ang += 2 * kPi;
  r[0] = static_cast<int32_t>(static_cast<int64_t>(jsRound(ang / (2 * kPi) * 0x10000)) & 0xFFFF);
}

void HleBios::cpuSet() {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0], dst = r[1];
  const int32_t ctl = r[2];
  const int32_t count = ctl & 0x1FFFFF;
  const bool fixed = (ctl & 0x01000000) != 0;
  if (ctl & 0x04000000) {
    src = I32(src) & ~3; dst = I32(dst) & ~3;
    const int32_t val = fixed ? bus.read32(src) : 0;
    for (int32_t k = 0; k < count; k++) {
      bus.write32(dst, fixed ? val : bus.read32(src));
      if (!fixed) src += 4;
      dst += 4;
    }
  } else {
    src = I32(src) & ~1; dst = I32(dst) & ~1;
    const int32_t val = fixed ? bus.read16(src) : 0;
    for (int32_t k = 0; k < count; k++) {
      bus.write16(dst, fixed ? val : bus.read16(src));
      if (!fixed) src += 2;
      dst += 2;
    }
  }
}

void HleBios::cpuFastSet() {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0] & ~3, dst = r[1] & ~3;
  const int32_t ctl = r[2];
  const int32_t count = ((ctl & 0x1FFFFF) + 7) & ~7;
  const bool fixed = (ctl & 0x01000000) != 0;
  const int32_t val = fixed ? bus.read32(src) : 0;
  for (int32_t k = 0; k < count; k++) {
    bus.write32(dst, fixed ? val : bus.read32(src));
    if (!fixed) src += 4;
    dst += 4;
  }
}

void HleBios::bgAffineSet() {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0], dst = r[1];
  for (int32_t k = 0; k < r[2]; k++) {
    const int32_t ox = bus.read32(src), oy = bus.read32(src + 4);
    const int32_t dx = sx16(bus.read16(src + 8)), dy = sx16(bus.read16(src + 10));
    const int32_t sx = sx16(bus.read16(src + 12)), sy = sx16(bus.read16(src + 14));
    const int ang = bus.read16(src + 16);
    const int idx = (ang >> 8) & 0xFF;
    const int32_t sn = SIN_T[idx], cs = COS_T[idx];
    const int64_t pa = imul(sx, cs) >> 14, pb = -static_cast<int64_t>(imul(sx, sn) >> 14);
    const int64_t pc = imul(sy, sn) >> 14, pd = imul(sy, cs) >> 14;
    bus.write16(dst, pa); bus.write16(dst + 2, pb); bus.write16(dst + 4, pc); bus.write16(dst + 6, pd);
    bus.write32(dst + 8, static_cast<int64_t>(ox) - (static_cast<int64_t>(imul(pa, dx)) + imul(pb, dy)));
    bus.write32(dst + 12, static_cast<int64_t>(oy) - (static_cast<int64_t>(imul(pc, dx)) + imul(pd, dy)));
    src += 20; dst += 16;
  }
}

void HleBios::objAffineSet() {
  // dst e off são Number no JS e dst cresce sem máscara: com contagens absurdas passa de 2^53 e perde precisão.
  // Para ficar idêntico, a conta é feita em double, como lá.
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0];
  double dst = r[1];
  const double off = r[3];
  for (int32_t k = 0; k < r[2]; k++) {
    const int32_t sx = sx16(bus.read16(src)), sy = sx16(bus.read16(src + 2));
    const int ang = bus.read16(src + 4);
    const int idx = (ang >> 8) & 0xFF;
    const int32_t sn = SIN_T[idx], cs = COS_T[idx];
    bus.write16(jsAddr(dst), imul(sx, cs) >> 14);
    bus.write16(jsAddr(dst + off), -static_cast<int64_t>(imul(sx, sn) >> 14));
    bus.write16(jsAddr(dst + off * 2), imul(sy, sn) >> 14);
    bus.write16(jsAddr(dst + off * 3), imul(sy, cs) >> 14);
    src += 8; dst += off * 4;
  }
}

void HleBios::bitUnPack() {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  const int64_t src = r[0];
  int64_t dst = r[1];
  const int64_t info = r[2];
  const int32_t len = bus.read16(info);
  const int sw = bus.read8(info + 2), dw = bus.read8(info + 3);
  const int32_t offRaw = bus.read32(info + 4);
  const int32_t offset = offRaw & 0x7FFFFFFF;
  const bool zeroFlag = (U32(offRaw) >> 31) != 0;
  if (sw == 0) return;  // no JS: laço infinito
  uint32_t out = 0;
  int outBits = 0;
  const int32_t smask = I32((int64_t{1} << (sw & 31)) - 1);
  for (int32_t k = 0; k < len; k++) {
    const int byte = bus.read8(src + k);
    for (int b = 0; b < 8; b += sw) {
      int32_t v = (byte >> b) & smask;
      if (v != 0 || zeroFlag) v = I32(static_cast<int64_t>(v) + offset);
      const uint32_t piece = dw == 32 ? U32(v) : U32(v & I32((int64_t{1} << (dw & 31)) - 1));
      out |= piece << (outBits & 31);
      outBits += dw;
      if (outBits >= 32) {
        bus.write32(dst, static_cast<int32_t>(out)); dst += 4; out = 0; outBits = 0;
      }
    }
  }
}

void HleBios::lz77(bool vram) {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0];
  const int32_t header = bus.read32(src); src += 4;
  const int64_t size = U32(header) >> 8;
  Writer w(bus, r[1], vram);
  while (w.length() < size) {
    const int flags = bus.read8(src++);
    for (int b = 7; b >= 0 && w.length() < size; b--) {
      if (flags & (1 << b)) {
        const int b1 = bus.read8(src++), b2 = bus.read8(src++);
        const int len = (b1 >> 4) + 3;
        const int disp = (((b1 & 0xF) << 8) | b2) + 1;
        for (int k = 0; k < len && w.length() < size; k++) w.put(w.get(disp));
      } else {
        w.put(bus.read8(src++));
      }
    }
  }
}

void HleBios::rle(bool vram) {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0];
  const int32_t header = bus.read32(src); src += 4;
  const int64_t size = U32(header) >> 8;
  Writer w(bus, r[1], vram);
  while (w.length() < size) {
    const int f = bus.read8(src++);
    if (f & 0x80) {
      const int len = (f & 0x7F) + 3, v = bus.read8(src++);
      for (int k = 0; k < len && w.length() < size; k++) w.put(v);
    } else {
      const int len = (f & 0x7F) + 1;
      for (int k = 0; k < len && w.length() < size; k++) w.put(bus.read8(src++));
    }
  }
}

void HleBios::huffman() {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  const int64_t src = r[0];
  int64_t dst = r[1];
  const int32_t header = bus.read32(src);
  const int bits = header & 0xF;
  const int64_t size = U32(header) >> 8;
  const int treeSize = bus.read8(src + 4);
  const int64_t treeBase = src + 5;
  int64_t stream = src + 4 + (treeSize + 1) * 2;
  uint32_t outWord = 0;
  int outBits = 0;
  int64_t written = 0;
  int64_t node = treeBase;
  int nodeVal = bus.read8(node);
  if (bits == 0 && size > 0) return;  // no JS: laço infinito (nunca completa uma palavra)
  int64_t words = 0;
  while (written < size) {
    if (++words > (int64_t{1} << 23)) return;  // árvore sem folhas: o JS travaria
    const uint32_t word = U32(bus.read32(stream)); stream += 4;
    for (int b = 31; b >= 0 && written < size; b--) {
      const int dir = (word >> b) & 1;
      const int offset = nodeVal & 0x3F;
      const int64_t next = static_cast<int64_t>(I32(node) & ~1) + offset * 2 + 2;
      const int isLeaf = dir ? (nodeVal & 0x40) : (nodeVal & 0x80);
      node = next + dir;
      nodeVal = bus.read8(node);
      if (isLeaf) {
        outWord |= U32(nodeVal & ((1 << bits) - 1)) << outBits;
        outBits += bits;
        if (outBits >= 32) { bus.write32(dst, static_cast<int32_t>(outWord)); dst += 4; written += 4; outWord = 0; outBits = 0; }
        node = treeBase; nodeVal = bus.read8(node);
      }
    }
  }
}

void HleBios::diff(int width, bool vram) {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  int64_t src = r[0], dst = r[1];
  const int32_t header = bus.read32(src); src += 4;
  const int64_t size = U32(header) >> 8;
  if (width == 8) {
    Writer w(bus, dst, vram);
    int acc = 0;
    for (int64_t k = 0; k < size; k++) { acc = (acc + bus.read8(src++)) & 0xFF; w.put(acc); }
  } else {
    int acc = 0;
    for (int64_t k = 0; k < size; k += 2) { acc = (acc + bus.read16(src)) & 0xFFFF; src += 2; bus.write16(dst, acc); dst += 2; }
  }
}

void HleBios::midiKey2Freq() {
  int32_t* r = gba.cpu.r;
  Bus& bus = gba.bus;
  const double freq = U32(bus.read32(static_cast<int64_t>(r[0]) + 4));
  const double v = freq / std::pow(2.0, (180.0 - r[1] - r[2] / 256.0) / 12.0);
  r[0] = static_cast<int32_t>(toUint32(v));
}

}  // namespace indigo::gba
