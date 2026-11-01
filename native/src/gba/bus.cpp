// Barramento de memória do GBA (porte de src/bus.js)
// 0x00 BIOS | 0x02 EWRAM 256K | 0x03 IWRAM 32K | 0x04 IO | 0x05 Paleta | 0x06 VRAM | 0x07 OAM
// 0x08-0x0D ROM (3 estados de espera) | 0x0E SRAM/Flash
#include <algorithm>

#include "gba.h"

namespace indigo::gba {

namespace {
const int N_TABLE[4] = {4, 3, 2, 8};
inline int region(int64_t addr) { return static_cast<int>((U32(addr) >> 24) & 0xF); }
inline bool isRom(int reg) { return reg >= 8 && reg <= 0xD; }
}  // namespace

Bus::Bus(GBA& g)
    : bios(0x4000), ewram(0x40000), iwram(0x8000), pal(0x400), vram(0x18000), oam(0x400), gba(g) {
  setWaitcnt(0);
}

void Bus::loadRom(const std::vector<uint8_t>& data) {
  // tamanho arredondado para múltiplo de 4 (como as visões tipadas da versão web)
  const size_t size = (data.size() + 3) & ~size_t(3);
  rom.assign(size, 0);
  std::copy(data.begin(), data.end(), rom.begin());
  romSize = static_cast<int64_t>(data.size());
}

void Bus::setWaitcnt(int v) {
  const int sram = N_TABLE[v & 3];
  const int ws0n = N_TABLE[(v >> 2) & 3], ws0s = (v & 0x10) ? 1 : 2;
  const int ws1n = N_TABLE[(v >> 5) & 3], ws1s = (v & 0x80) ? 1 : 4;
  const int ws2n = N_TABLE[(v >> 8) & 3], ws2s = (v & 0x400) ? 1 : 8;
  prefetch = (v & 0x4000) != 0;
  if (!prefetch) pfNext = -1;
  uint8_t* w = wait16;
  uint8_t* d = wait32;
  w[0] = 1; d[0] = 1; w[1] = 1; d[1] = 1;
  w[2] = 3; d[2] = 6; w[3] = 1; d[3] = 1; w[4] = 1; d[4] = 1;
  w[5] = 1; d[5] = 2; w[6] = 1; d[6] = 2; w[7] = 1; d[7] = 1;
  const int ws[3][2] = {{ws0n, ws0s}, {ws1n, ws1s}, {ws2n, ws2s}};
  for (int k = 0; k < 3; k++) {
    for (int rg : {8 + 2 * k, 9 + 2 * k}) {
      romN[rg] = static_cast<uint8_t>(1 + ws[k][0]); romS[rg] = static_cast<uint8_t>(1 + ws[k][1]);
      w[rg] = 0; d[rg] = 0;  // a ROM é cobrada por romCost
    }
  }
  w[14] = w[15] = d[14] = d[15] = static_cast<uint8_t>(1 + sram);
}

// custo de um acesso à ROM: N ou S conforme a sequência (que quebra a cada 128 KB) e o prefetch
void Bus::romCost(int64_t addr, bool is32, bool isCode) {
  const int reg = region(addr);
  const int n = romN[reg], sq = romS[reg];
  const bool seq = addr == seqNext && (I32(addr) & 0x1FFFF) != 0;
  seqNext = addr + (is32 ? 4 : 2);
  int64_t cost;
  if (isCode && prefetch) {
    const int64_t now = gba.now();
    const int need = is32 ? 2 : 1;
    if (seq && addr == pfNext) {
      const int64_t span = now - pfTime;
      const int64_t made = span > 0 ? span / sq : 0;
      int64_t avail = pfCount + made;
      const bool full = avail >= 8;
      if (full) avail = 8;
      if (avail >= need) {
        cost = 1;
        pfCount = static_cast<int>(avail - need);
        pfTime = full ? now + 1 : pfTime + made * sq;
      } else {
        const int64_t partial = span - made * sq;
        cost = std::max<int64_t>(1, (need - avail) * sq - partial);
        pfCount = 0;
        pfTime = now + cost;
      }
    } else {
      cost = is32 ? n + sq : n;
      pfCount = 0;
      pfTime = now + cost;
    }
    pfNext = addr + (is32 ? 4 : 2);
  } else {
    cost = seq ? (is32 ? 2 * sq : sq) : (is32 ? n + sq : n);
    if (!isCode) pfNext = -1;  // leitura de dados na ROM interrompe o prefetch
  }
  cycles += cost;
}

// custo de uma busca de instrução (ciclo S/N do pipeline)
void Bus::codeCost(int64_t addr, bool is32) {
  const int reg = region(addr);
  if (isRom(reg)) romCost(addr, is32, true);
  else cycles += is32 ? wait32[reg] : wait16[reg];
}

// valor da instrução (o custo da busca é cobrado à parte, no endereço que o pipeline realmente busca)
int32_t Bus::fetch16(int64_t addr) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  int32_t v;
  switch (reg) {
    case 0x0: v = ld16(bios, (a & 0x3FFF) & ~1u); biosLast = ld32(bios, (U32(addr + 4) & 0x3FFC)); break;
    case 0x2: v = ld16(ewram, (a & 0x3FFFF) & ~1u); break;
    case 0x3: v = ld16(iwram, (a & 0x7FFF) & ~1u); break;
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
      const int64_t o = a & 0x1FFFFFF;
      v = o < romSize ? ld16(rom, static_cast<size_t>(o) & ~size_t(1)) : static_cast<int32_t>((a >> 1) & 0xFFFF);
      break;
    }
    default: { const int64_t c = cycles; v = read16(addr); cycles = c; break; }
  }
  lastFetch = static_cast<int32_t>(U32(v) | (U32(v) << 16));
  return v;
}

int32_t Bus::fetch32(int64_t addr) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  int32_t v;
  switch (reg) {
    case 0x0: v = ld32(bios, (a & 0x3FFF) & ~3u); biosLast = ld32(bios, (U32(addr + 8) & 0x3FFC)); break;
    case 0x2: v = ld32(ewram, (a & 0x3FFFF) & ~3u); break;
    case 0x3: v = ld32(iwram, (a & 0x7FFF) & ~3u); break;
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
      const int64_t o = a & 0x1FFFFFF;
      v = o < romSize ? ld32(rom, static_cast<size_t>(o) & ~size_t(3))
                      : static_cast<int32_t>(((a >> 1) & 0xFFFF) | ((((a >> 1) + 1) & 0xFFFF) << 16));
      break;
    }
    default: { const int64_t c = cycles; v = read32(addr); cycles = c; break; }
  }
  lastFetch = v;
  return v;
}

size_t Bus::vramIndex(int64_t addr) {
  uint32_t o = U32(addr) & 0x1FFFF;
  if (o >= 0x18000) o -= 0x8000;
  return o;
}

// ---------------- leitura ----------------
int32_t Bus::read8(int64_t addr) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  cycles += wait16[reg];
  switch (reg) {
    case 0x0:
      if (addr >= 0x4000) return (U32(lastFetch) >> ((a & 3) << 3)) & 0xFF;
      if ((U32(gba.cpu.nextPC) >> 24) != 0) return (U32(biosLast) >> ((a & 3) << 3)) & 0xFF;
      return addr >= 0 ? bios[static_cast<size_t>(addr)] : 0;
    case 0x2: return ewram[a & 0x3FFFF];
    case 0x3: return iwram[a & 0x7FFF];
    case 0x4: return gba.io.read8(static_cast<int>(a & 0xFFFFFF));
    case 0x5: return pal[a & 0x3FF];
    case 0x6: return vram[vramIndex(addr)];
    case 0x7: return oam[a & 0x3FF];
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
      romCost(I32(addr) & ~1, false, false);
      const int64_t o = a & 0x1FFFFFF;
      if (gpio && o >= 0xC4 && o < 0xCA && gpio->readable()) return (o & 1) ? 0 : gpio->read(static_cast<int>(o));
      if (o < romSize) return rom[static_cast<size_t>(o)];
      if (reg == 0xD && save && save->isEeprom) return save->read16() & 0xFF;
      return ((a >> 1) >> ((a & 1) << 3)) & 0xFF;
    }
    case 0xE: case 0xF:
      return save ? save->read8(a & 0xFFFF) : 0xFF;
    default:
      return (U32(lastFetch) >> ((a & 3) << 3)) & 0xFF;
  }
}

int32_t Bus::read16(int64_t addr) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  cycles += wait16[reg];
  switch (reg) {
    case 0x0:
      if (addr >= 0x4000) return (U32(lastFetch) >> ((a & 2) << 3)) & 0xFFFF;
      if ((U32(gba.cpu.nextPC) >> 24) != 0) return (U32(biosLast) >> ((a & 2) << 3)) & 0xFFFF;
      return addr >= 0 ? ld16(bios, static_cast<size_t>(addr) & ~size_t(1)) : 0;
    case 0x2: return ld16(ewram, (a & 0x3FFFF) & ~1u);
    case 0x3: return ld16(iwram, (a & 0x7FFF) & ~1u);
    case 0x4: return gba.io.read16(static_cast<int>(a & 0xFFFFFE));
    case 0x5: return ld16(pal, (a & 0x3FF) & ~1u);
    case 0x6: return ld16(vram, vramIndex(addr) & ~size_t(1));
    case 0x7: return ld16(oam, (a & 0x3FF) & ~1u);
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
      romCost(I32(addr) & ~1, false, false);
      if (reg == 0xD && save && save->isEeprom && (romSize <= 0x1000000 || (a & 0x1FFFFFF) >= 0x1FFFF00)) return save->read16();
      const int64_t o = a & 0x1FFFFFF;
      if (gpio && o >= 0xC4 && o < 0xCA && gpio->readable()) return gpio->read(static_cast<int>(o & ~1));
      if (o < romSize) return ld16(rom, static_cast<size_t>(o) & ~size_t(1));
      return (a >> 1) & 0xFFFF;
    }
    case 0xE: case 0xF: {
      const int b = save ? save->read8(a & 0xFFFF) : 0xFF;
      return b * 0x101;
    }
    default:
      return (U32(lastFetch) >> ((a & 2) << 3)) & 0xFFFF;
  }
}

int32_t Bus::read32(int64_t addr) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  cycles += wait32[reg];
  switch (reg) {
    case 0x0:
      if (addr >= 0x4000) return lastFetch;
      if ((U32(gba.cpu.nextPC) >> 24) != 0) return biosLast;
      return addr >= 0 ? ld32(bios, static_cast<size_t>(addr) & ~size_t(3)) : 0;
    case 0x2: return ld32(ewram, (a & 0x3FFFF) & ~3u);
    case 0x3: return ld32(iwram, (a & 0x7FFF) & ~3u);
    case 0x4: {
      const int x = static_cast<int>(a & 0xFFFFFC);
      const int lo = gba.io.read16(x);
      const int hi = gba.io.read16(x + 2);
      return static_cast<int32_t>(U32(lo) | (U32(hi) << 16));
    }
    case 0x5: return ld32(pal, (a & 0x3FF) & ~3u);
    case 0x6: return ld32(vram, vramIndex(addr) & ~size_t(3));
    case 0x7: return ld32(oam, (a & 0x3FF) & ~3u);
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD: {
      romCost(I32(addr) & ~3, true, false);
      const int64_t o = a & 0x1FFFFFF;
      if (gpio && o >= 0xC4 && o < 0xCA && gpio->readable()) {
        const int x = static_cast<int>(o & ~3);
        return static_cast<int32_t>(U32(gpio->read(x)) | (U32(gpio->read(x + 2)) << 16));
      }
      if (o < romSize) return ld32(rom, static_cast<size_t>(o) & ~size_t(3));
      return static_cast<int32_t>(((a >> 1) & 0xFFFF) | ((((a >> 1) + 1) & 0xFFFF) << 16));
    }
    case 0xE: case 0xF: {
      const uint32_t b = save ? static_cast<uint32_t>(save->read8(a & 0xFFFF)) : 0xFF;
      return static_cast<int32_t>(b * 0x01010101u);
    }
    default:
      return lastFetch;
  }
}

// ---------------- escrita ----------------
void Bus::write8(int64_t addr, int64_t value) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  cycles += wait16[reg];
  const uint32_t val = U32(value) & 0xFF;
  switch (reg) {
    case 0x2: ewram[a & 0x3FFFF] = static_cast<uint8_t>(val); return;
    case 0x3: iwram[a & 0x7FFF] = static_cast<uint8_t>(val); return;
    case 0x4: gba.io.write8(static_cast<int>(a & 0xFFFFFF), static_cast<int>(val)); return;
    case 0x5: st16(pal, (a & 0x3FE), val * 0x101); return;
    case 0x6: {
      const size_t o = vramIndex(addr);
      const bool bitmap = (gba.io.regs[0] & 7) >= 3;
      if (o < (bitmap ? 0x14000u : 0x10000u)) st16(vram, o & ~size_t(1), val * 0x101);
      return;
    }
    case 0x7: return;  // OAM ignora escritas de 8 bits
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
      romCost(I32(addr) & ~1, false, false);
      if (gpio && reg == 8 && (a & 0x1FFFFFF) >= 0xC4 && (a & 0x1FFFFFF) < 0xCA && !(a & 1)) gpio->write(static_cast<int>(a & 0xFE), static_cast<int>(val));
      return;
    case 0xE: case 0xF:
      if (save) save->write8(a & 0xFFFF, static_cast<int>(val));
      return;
    default: return;
  }
}

void Bus::write16(int64_t addr, int64_t value) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  cycles += wait16[reg];
  const uint32_t val = U32(value) & 0xFFFF;
  switch (reg) {
    case 0x2: st16(ewram, (a & 0x3FFFF) & ~1u, val); return;
    case 0x3: st16(iwram, (a & 0x7FFF) & ~1u, val); return;
    case 0x4: gba.io.write16(static_cast<int>(a & 0xFFFFFE), static_cast<int>(val)); return;
    case 0x5: st16(pal, (a & 0x3FF) & ~1u, val); return;
    case 0x6: st16(vram, vramIndex(addr) & ~size_t(1), val); return;
    case 0x7: st16(oam, (a & 0x3FF) & ~1u, val); return;
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
      romCost(I32(addr) & ~1, false, false);
      if (reg == 0xD && save && save->isEeprom) save->write16(static_cast<int>(val));
      else if (gpio && reg == 8) { const uint32_t o = a & 0x1FFFFFE; if (o >= 0xC4 && o < 0xCA) gpio->write(static_cast<int>(o), static_cast<int>(val)); }
      return;
    case 0xE: case 0xF:
      if (save) save->write8(a & 0xFFFF, static_cast<int>((val >> ((a & 1) << 3)) & 0xFF));
      return;
    default: return;
  }
}

void Bus::write32(int64_t addr, int64_t value) {
  const int reg = region(addr);
  const uint32_t a = U32(addr);
  cycles += wait32[reg];
  const uint32_t val = U32(value);
  switch (reg) {
    case 0x2: st32(ewram, (a & 0x3FFFF) & ~3u, val); return;
    case 0x3: st32(iwram, (a & 0x7FFF) & ~3u, val); return;
    case 0x4: gba.io.write32(static_cast<int>(a & 0xFFFFFC), static_cast<int32_t>(val)); return;
    case 0x5: st32(pal, (a & 0x3FF) & ~3u, val); return;
    case 0x6: st32(vram, vramIndex(addr) & ~size_t(3), val); return;
    case 0x7: st32(oam, (a & 0x3FF) & ~3u, val); return;
    case 0x8: case 0x9: case 0xA: case 0xB: case 0xC: case 0xD:
      romCost(I32(addr) & ~3, true, false);
      if (gpio && reg == 8) {
        const uint32_t o = a & 0x1FFFFFC;
        if (o >= 0xC4 && o < 0xCA) gpio->write(static_cast<int>(o), static_cast<int>(val & 0xFFFF));
        if (o + 2 >= 0xC4 && o + 2 < 0xCA) gpio->write(static_cast<int>(o + 2), static_cast<int>((val >> 16) & 0xFFFF));
      }
      return;
    case 0xE: case 0xF:
      if (save) save->write8(a & 0xFFFF, static_cast<int>((val >> ((a & 3) << 3)) & 0xFF));
      return;
    default: return;
  }
}

void Bus::sync(StateIO& io) {
  io.bytes(ewram); io.bytes(iwram); io.bytes(pal); io.bytes(vram); io.bytes(oam);
  io.bytes(bios);  // a BIOS original pode estar carregada; a própria é reinstalada igual
  io.sync(biosLast); io.sync(lastFetch); io.sync(cycles);
  io.raw(wait16, sizeof wait16); io.raw(wait32, sizeof wait32); io.raw(romN, sizeof romN); io.raw(romS, sizeof romS);
  io.sync(prefetch); io.sync(seqNext); io.sync(pfNext); io.sync(pfTime); io.sync(pfCount);
  if (io.reading()) {
    for (int k = 0; k < 16; k++) { clampTo<uint8_t>(wait16[k], 0, 9); clampTo<uint8_t>(wait32[k], 0, 18); }
    for (int k = 8; k <= 0xD; k++) { clampTo<uint8_t>(romN[k], 1, 9); clampTo<uint8_t>(romS[k], 1, 9); }
    clampTo(pfCount, 0, 8);
  }
}

}  // namespace indigo::gba
