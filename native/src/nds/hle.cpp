// Funções SWI da BIOS do DS em alto nível (o DS não pede BIOS). Comportamento conforme o GBATEK; o código é
// próprio. As versões "por callback" da descompressão chamam as rotinas do jogo (abrir, ler byte, fechar).
#include <algorithm>
#include <cmath>
#include <vector>

#include "nds.h"

namespace indigo::nds {

uint16_t crc16(uint16_t crc, const uint8_t* p, size_t n);

namespace {
// leitura da origem: memória direta ou rotinas do jogo
struct Src {
  NDS& s;
  int cpu;
  ArmBus& bus;
  uint32_t addr;
  uint32_t cb = 0;  // 0 = memória; senão endereço da tabela de callbacks
  Src(NDS& n, int c, ArmBus& b, uint32_t a, uint32_t callbacks) : s(n), cpu(c), bus(b), addr(a), cb(callbacks) {}
  uint32_t header(uint32_t dst, uint32_t param) {
    if (!cb) { const uint32_t h = bus.read32(addr & ~3u); addr += 4; return h; }
    const uint32_t open = bus.read32(cb);
    const uint32_t h = s.callGuest(cpu, open, addr, dst, param);
    addr += 4;
    return h;
  }
  uint8_t get8() {
    if (!cb) return static_cast<uint8_t>(bus.read8(addr++));
    const uint32_t fn = bus.read32(cb + 8);
    return static_cast<uint8_t>(s.callGuest(cpu, fn, addr++));
  }
  uint32_t get32() {
    if (!cb) { const uint32_t v = bus.read32(addr & ~3u); addr += 4; return v; }
    const uint32_t fn = bus.read32(cb + 16);
    const uint32_t v = s.callGuest(cpu, fn, addr);
    addr += 4;
    return v;
  }
  void close() {
    if (!cb) return;
    const uint32_t fn = bus.read32(cb + 4);
    if (fn) s.callGuest(cpu, fn, addr);
  }
};

// escrita do resultado: em bytes ou em pares de 16 bits (VRAM)
struct Dst {
  ArmBus& bus;
  uint32_t addr;
  bool w16;
  int pend = -1;
  std::vector<uint8_t> hist;
  Dst(ArmBus& b, uint32_t a, bool wide) : bus(b), addr(a), w16(wide) {}
  void put(uint8_t v) {
    hist.push_back(v);
    if (!w16) { bus.write8(addr++, v); return; }
    if (pend < 0) pend = v;
    else { bus.write16(addr, uint32_t(pend) | (uint32_t(v) << 8)); addr += 2; pend = -1; }
  }
  uint8_t back(uint32_t d) const { return d <= hist.size() && d ? hist[hist.size() - d] : 0; }
};

constexpr uint32_t LIMIT = 0x1000000;  // nenhum descompressor escreve mais de 16 MB (dados corrompidos não travam)
}  // namespace

// chama uma rotina do jogo e devolve r0 (as SWI por callback); roda só aquela CPU até a rotina voltar
uint32_t NDS::callGuest(int c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  Arm& cpu = c ? arm7 : arm9;
  if (guestDepth_ > 2 || fn == 0 || guestStuck_) return 0;
  guestDepth_++;
  uint32_t saved[16];
  for (int k = 0; k < 16; k++) saved[k] = cpu.r[k];
  const uint32_t savedNext = cpu.nextPC, savedCpsr = cpu.cpsr();
  const uint32_t ret = c ? 0x38u : (io9.hle ? 0xFFFF0044u : 0xFFFF0044u);
  cpu.r[0] = a0; cpu.r[1] = a1; cpu.r[2] = a2;
  cpu.r[14] = ret;
  cpu.t = fn & 1;
  cpu.nextPC = fn & ~1u;
  int64_t steps = 0;
  while (cpu.nextPC != ret) {
    if (steps >= guestBudget_) { guestStuck_ = true; break; }  // laço infinito: desiste da SWI inteira
    cpu.step();
    if (cpu.halted) cpu.halted = false;
    steps++;
  }
  guestBudget_ -= steps;
  if (c == 0) cyc9 -= steps; else cyc7 -= steps;  // o tempo gasto na rotina conta para a CPU
  const uint32_t res = cpu.r[0];
  cpu.setCpsr(savedCpsr);
  for (int k = 0; k < 16; k++) cpu.r[k] = saved[k];
  cpu.nextPC = savedNext;
  cpu.branched = false;
  guestDepth_--;
  return res;
}

bool NDS::hleSwi(int c, int n, uint32_t addr) {
  CpuIo& io = cio(c);
  if (!io.hle) return false;
  Arm& cpu = c ? arm7 : arm9;
  ArmBus& bus = c ? static_cast<ArmBus&>(bus7) : static_cast<ArmBus&>(bus9);
  uint32_t* r = cpu.r;
  if (c == 0) cyc9 -= 20; else cyc7 -= 10;
  if (guestDepth_ == 0) { guestBudget_ = 8000000; guestStuck_ = false; }  // orçamento total da SWI (8M passos)
  auto intrWait = [&](bool discard, uint32_t mask) {
    const uint32_t flagsAddr = c ? 0x0380FFF8u : dtcmBase + 0x3FF8;
    if (!io.intrWaitArmed) {
      if (discard) bus.write32(flagsAddr, bus.read32(flagsAddr) & ~mask);
      io.intrWaitArmed = true;
    }
    io.ime = 1;
    updateIrq();
    const uint32_t flags = bus.read32(flagsAddr);
    if (flags & mask) {
      bus.write32(flagsAddr, flags & ~mask);
      io.intrWaitArmed = false;
      return;
    }
    // volta para a própria SWI e espera uma IRQ (o tratador do jogo marca as flags e a SWI confere de novo)
    cpu.setPC(addr);
    cpu.halted = true;
    cpu.forceIrq = true;
    if (c) haltcnt = 0x80;
    if (cpu.irqLine) cpu.halted = false;
  };
  switch (n) {
    case 0x00: {  // SoftReset
      const uint32_t entry = c ? bus.read32(0x027FFE34) : bus.read32(0x027FFE24);
      cpu.switchMode(M_SVC); r[13] = c ? 0x0380FFDC : 0x03003FC0; r[14] = 0; cpu.setSpsr(0);
      cpu.switchMode(M_IRQ); r[13] = c ? 0x0380FFB0 : 0x03003F80; r[14] = 0; cpu.setSpsr(0);
      cpu.switchMode(M_SYS); r[13] = c ? 0x0380FF00 : 0x03002F7C;
      for (int k = 0; k < 13; k++) r[k] = 0;
      r[14] = 0;
      cpu.t = 0;
      cpu.setPC(entry);
      break;
    }
    case 0x03: {  // WaitByLoop: cerca de 4 ciclos por volta
      // a espera para a CPU na hora (o aperto de mão do IPCSYNC dos jogos depende disso)
      const int64_t loops = static_cast<int32_t>(r[0]) > 0 ? r[0] : 0;
      cpu.stall += static_cast<int>(std::min<int64_t>(loops * 4, 1 << 28));
      r[0] = 0;
      break;
    }
    case 0x04: intrWait(r[0] != 0, r[1]); break;
    case 0x05: r[0] = 1; r[1] = 1; intrWait(true, 1); break;
    case 0x06:  // Halt
      cpu.halted = true;
      if (c) haltcnt = 0x80;
      if (io.ie & io.ifr) cpu.halted = false;
      break;
    case 0x07:  // Sleep (ARM7): trata como Halt
      if (c) { cpu.halted = true; haltcnt = 0x80; }
      break;
    case 0x08:  // SoundBias
      if (c) spu.bias = r[0] ? 0x200 : 0;
      break;
    case 0x09: {  // Div
      const int32_t num = static_cast<int32_t>(r[0]), den = static_cast<int32_t>(r[1]);
      if (den == 0) { r[0] = num < 0 ? uint32_t(-1) : 1; r[1] = uint32_t(num); r[3] = 1; break; }
      if (num == INT32_MIN && den == -1) { r[0] = uint32_t(INT32_MIN); r[1] = 0; r[3] = uint32_t(INT32_MIN); break; }
      const int32_t q = num / den;
      r[0] = uint32_t(q); r[1] = uint32_t(num % den); r[3] = uint32_t(q < 0 ? -q : q);
      break;
    }
    case 0x0B: {  // CpuSet
      uint32_t src = r[0], dst = r[1];
      const uint32_t cnt = r[2] & 0x1FFFFF;
      const bool fill = r[2] & (1u << 24), word = r[2] & (1u << 26);
      if (word) {
        src &= ~3u; dst &= ~3u;
        const uint32_t v = bus.read32(src);
        for (uint32_t k = 0; k < cnt; k++) { bus.write32(dst, fill ? v : bus.read32(src)); dst += 4; if (!fill) src += 4; }
      } else {
        src &= ~1u; dst &= ~1u;
        const uint32_t v = bus.read16(src);
        for (uint32_t k = 0; k < cnt; k++) { bus.write16(dst, fill ? v : bus.read16(src)); dst += 2; if (!fill) src += 2; }
      }
      if (c == 0) cyc9 -= cnt * 2; else cyc7 -= cnt * 2;
      break;
    }
    case 0x0C: {  // CpuFastSet
      uint32_t src = r[0] & ~3u, dst = r[1] & ~3u;
      const uint32_t cnt = ((r[2] & 0x1FFFFF) + 7) & ~7u;
      const bool fill = r[2] & (1u << 24);
      const uint32_t v = bus.read32(src);
      for (uint32_t k = 0; k < cnt; k++) { bus.write32(dst, fill ? v : bus.read32(src)); dst += 4; if (!fill) src += 4; }
      if (c == 0) cyc9 -= cnt; else cyc7 -= cnt;
      break;
    }
    case 0x0D: {  // Sqrt
      uint32_t v = r[0], res = 0, bit = 1u << 30;
      while (bit > v) bit >>= 2;
      while (bit) { if (v >= res + bit) { v -= res + bit; res = (res >> 1) + bit; } else res >>= 1; bit >>= 2; }
      r[0] = res;
      break;
    }
    case 0x0E: {  // GetCRC16
      uint16_t crc = static_cast<uint16_t>(r[0]);
      const uint32_t a = r[1], len = r[2] & 0xFFFFF;
      for (uint32_t k = 0; k < len; k++) { const uint8_t b = static_cast<uint8_t>(bus.read8(a + k)); crc = crc16(crc, &b, 1); }
      r[0] = crc;
      break;
    }
    case 0x0F: r[0] = 0; break;  // IsDebugger
    case 0x10: {  // BitUnPack
      uint32_t src = r[0], dst = r[1];
      const uint32_t info = r[2];
      const uint32_t len = bus.read16(info), srcW = bus.read8(info + 2), dstW = bus.read8(info + 3);
      const uint32_t off = bus.read32(info + 4) & 0x7FFFFFFF;
      const bool zeroFlag = bus.read32(info + 4) >> 31;
      if (!srcW || !dstW || srcW > 8 || dstW > 32 || (8 % srcW) || (32 % dstW)) break;
      uint32_t out = 0;
      int outBits = 0;
      for (uint32_t k = 0; k < len && k < LIMIT; k++) {
        const uint32_t b = bus.read8(src++);
        for (uint32_t bit = 0; bit < 8; bit += srcW) {
          uint32_t v = (b >> bit) & ((1u << srcW) - 1);
          if (v || zeroFlag) v += off;
          out |= (dstW == 32 ? v : (v & ((1u << dstW) - 1))) << outBits;
          outBits += dstW;
          if (outBits >= 32) { bus.write32(dst & ~3u, out); dst += 4; out = 0; outBits = 0; }
        }
      }
      break;
    }
    case 0x11: case 0x12: {  // LZ77 (normal em bytes / por callback em 16 bits)
      const bool cbk = n == 0x12;
      Src in(*this, c, bus, r[0], cbk ? r[3] : 0);
      const uint32_t hdr = in.header(r[1], r[2]);
      uint32_t size = hdr >> 8;
      if (size > LIMIT) size = LIMIT;
      Dst out(bus, r[1], cbk);
      while (out.hist.size() < size && !guestStuck_) {
        const uint8_t flags = in.get8();
        for (int k = 0; k < 8 && out.hist.size() < size; k++) {
          if (flags & (0x80 >> k)) {
            const uint8_t b1 = in.get8(), b2 = in.get8();
            const uint32_t len = (b1 >> 4) + 3, disp = (((b1 & 0xF) << 8) | b2) + 1;
            for (uint32_t j = 0; j < len && out.hist.size() < size; j++) out.put(out.back(disp));
          } else out.put(in.get8());
        }
      }
      in.close();
      break;
    }
    case 0x14: case 0x15: {  // RLE
      const bool cbk = n == 0x15;
      Src in(*this, c, bus, r[0], cbk ? r[3] : 0);
      const uint32_t hdr = in.header(r[1], r[2]);
      uint32_t size = hdr >> 8;
      if (size > LIMIT) size = LIMIT;
      Dst out(bus, r[1], cbk);
      while (out.hist.size() < size && !guestStuck_) {
        const uint8_t f = in.get8();
        if (f & 0x80) {
          const uint8_t v = in.get8();
          for (int k = 0; k < (f & 0x7F) + 3 && out.hist.size() < size; k++) out.put(v);
        } else {
          for (int k = 0; k < (f & 0x7F) + 1 && out.hist.size() < size; k++) out.put(in.get8());
        }
      }
      in.close();
      break;
    }
    case 0x13: {  // Huffman por callback (escreve palavras de 32 bits)
      Src in(*this, c, bus, r[0], r[3]);
      const uint32_t hdr = in.header(r[1], r[2]);
      const uint32_t bits = hdr & 0xF;
      uint32_t size = hdr >> 8;
      if (size > LIMIT) size = LIMIT;
      if (bits != 4 && bits != 8) { in.close(); break; }
      const uint32_t treeSize = (uint32_t(in.get8()) + 1) * 2;
      std::vector<uint8_t> tree(treeSize);
      tree[0] = static_cast<uint8_t>(treeSize / 2 - 1);
      for (uint32_t k = 1; k < treeSize && !guestStuck_; k++) tree[k] = in.get8();
      uint32_t dst = r[1], outWord = 0, written = 0;
      int outBits = 0;
      uint32_t node = 1;  // índice do nó atual na árvore (a raiz em 1)
      while (written < size && !guestStuck_) {
        const uint32_t w = in.get32();
        for (int b = 31; b >= 0 && written < size; b--) {
          if (node >= treeSize) { written = size; break; }
          const uint8_t nd = tree[node];
          const uint32_t next = (node & ~1u) + (uint32_t(nd & 0x3F) << 1) + 2;
          const bool right = (w >> b) & 1;
          const bool leaf = right ? (nd & 0x40) : (nd & 0x80);
          const uint32_t child = next + (right ? 1 : 0);
          if (child >= treeSize) { written = size; break; }
          if (leaf) {
            outWord |= uint32_t(tree[child] & ((1u << bits) - 1)) << outBits;
            outBits += bits;
            if (outBits == 32) { bus.write32(dst & ~3u, outWord); dst += 4; written += 4; outWord = 0; outBits = 0; }
            node = 1;
          } else node = child;
        }
      }
      in.close();
      break;
    }
    case 0x16: case 0x18: {  // Diff8bitUnFilterWrite8bit / Diff16bitUnFilter
      uint32_t src = r[0], dst = r[1];
      const uint32_t hdr = bus.read32(src & ~3u);
      src += 4;
      uint32_t size = hdr >> 8;
      if (size > LIMIT) size = LIMIT;
      if (n == 0x16) {
        uint8_t acc = 0;
        for (uint32_t k = 0; k < size; k++) { acc = uint8_t(acc + bus.read8(src++)); bus.write8(dst++, acc); }
      } else {
        uint16_t acc = 0;
        for (uint32_t k = 0; k + 1 < size + 1 && k < size; k += 2) {
          acc = uint16_t(acc + bus.read16(src & ~1u)); src += 2;
          bus.write16(dst & ~1u, acc); dst += 2;
        }
      }
      break;
    }
    case 0x1A: {  // GetSineTable (ARM7): seno de um quarto de volta em 64 passos
      const uint32_t i = r[0] & 0x3F;
      r[0] = static_cast<uint32_t>(std::lround(std::sin(i * 3.141592653589793 / 128.0) * 32768.0)) & 0xFFFF;
      if (r[0] == 0x8000) r[0] = 0x7FFF;
      break;
    }
    case 0x1B: {  // GetPitchTable (ARM7): 2^(i/768) - 1 em 16 bits
      const uint32_t i = r[0] % 768;
      r[0] = static_cast<uint32_t>(std::lround((std::pow(2.0, i / 768.0) - 1.0) * 65536.0)) & 0xFFFF;
      break;
    }
    case 0x1C: {  // GetVolumeTable (ARM7): decibéis (0,1 dB) para volume linear, com as faixas de divisor
      const int i = static_cast<int>(r[0] % 724);
      const int db = i - 723;
      const double mult = db < -240 ? 16.0 : db < -120 ? 4.0 : db < -60 ? 2.0 : 1.0;
      long v = std::lround(127.0 * std::pow(10.0, db / 200.0) * mult);
      if (v > 127) v = 127;
      r[0] = static_cast<uint32_t>(v);
      break;
    }
    case 0x1D: r[0] = 0; break;  // GetBootProcs
    case 0x1F: break;             // CustomPost
    default: break;
  }
  return true;
}

}  // namespace indigo::nds
