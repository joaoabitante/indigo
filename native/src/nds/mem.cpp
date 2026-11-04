// Mapas de memória do ARM9 e do ARM7, VRAM com bancos mapeáveis e os registradores de E/S.
#include <cstring>

#include "nds.h"

namespace indigo::nds {

namespace {
inline uint32_t ld32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
inline uint32_t ld16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline void st32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }
inline void st16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

constexpr uint32_t BANK_OFF[9] = {0x00000, 0x20000, 0x40000, 0x60000, 0x80000, 0x90000, 0x94000, 0x98000, 0xA0000};
constexpr uint32_t BANK_SIZE[9] = {0x20000, 0x20000, 0x20000, 0x20000, 0x10000, 0x4000, 0x4000, 0x8000, 0x4000};
}  // namespace

// ================================================================== VRAM
void NDS::remapVram() {
  for (auto& p : pgBgA) p = -1;
  for (auto& p : pgObjA) p = -1;
  for (auto& p : pgBgB) p = -1;
  for (auto& p : pgObjB) p = -1;
  for (auto& p : pgArm7) p = -1;
  for (auto& p : pgTex) p = -1;
  for (auto& p : pgTexPal) p = -1;
  for (auto& p : pgBgExtA) p = -1;
  for (auto& p : pgBgExtB) p = -1;
  pgObjExtA = pgObjExtB = -1;
  auto mapPages = [](int32_t* tbl, int n, uint32_t pageStart, uint32_t off, uint32_t size, uint32_t pg = 0x4000) {
    for (uint32_t k = 0; k < size / pg; k++) {
      const uint32_t idx = pageStart + k;
      if (idx < uint32_t(n)) tbl[idx] = static_cast<int32_t>(off + k * pg);
    }
  };
  for (int b = 0; b < 9; b++) {
    const uint8_t c = vramcnt[b];
    if (!(c & 0x80)) continue;
    const int mst = c & 7, ofs = (c >> 3) & 3;
    const uint32_t off = BANK_OFF[b], size = BANK_SIZE[b];
    switch (b) {
      case 0: case 1: case 2: case 3:  // A, B, C, D
        switch (mst & 7) {
          case 1: mapPages(pgBgA, 32, ofs * 8, off, size); break;
          case 2:
            if (b < 2) mapPages(pgObjA, 16, (ofs & 1) * 8, off, size);
            else mapPages(pgArm7, 16, (ofs & 1) * 8, off, size);
            break;
          case 3: mapPages(pgTex, 32, ofs * 8, off, size); break;
          case 4:
            if (b == 2) mapPages(pgBgB, 8, 0, off, size);
            else if (b == 3) mapPages(pgObjB, 8, 0, off, size);
            break;
          default: break;
        }
        break;
      case 4:  // E
        switch (mst) {
          case 1: mapPages(pgBgA, 32, 0, off, size); break;
          case 2: mapPages(pgObjA, 16, 0, off, size); break;
          case 3: mapPages(pgTexPal, 8, 0, off, size); break;
          case 4: mapPages(pgBgExtA, 4, 0, off, 0x8000, 0x2000); break;
          default: break;
        }
        break;
      case 5: case 6: {  // F, G
        const uint32_t pg = (ofs & 1) + (ofs >> 1) * 4;
        switch (mst) {
          case 1: mapPages(pgBgA, 32, pg, off, size); break;
          case 2: mapPages(pgObjA, 16, pg, off, size); break;
          case 3: mapPages(pgTexPal, 8, (ofs & 1) + (ofs >> 1) * 4, off, size); break;
          case 4: mapPages(pgBgExtA, 4, (ofs & 1) * 2, off, 0x4000, 0x2000); break;
          case 5: pgObjExtA = static_cast<int32_t>(off); break;
          default: break;
        }
        break;
      }
      case 7:  // H
        switch (mst & 3) {
          case 1: mapPages(pgBgB, 8, 0, off, size); break;
          case 2: mapPages(pgBgExtB, 4, 0, off, 0x8000, 0x2000); break;
          default: break;
        }
        break;
      default:  // I
        switch (mst & 3) {
          case 1: mapPages(pgBgB, 8, 2, off, size); break;
          case 2: mapPages(pgObjB, 8, 0, off, size); break;
          case 3: pgObjExtB = static_cast<int32_t>(off); break;
          default: break;
        }
        break;
    }
  }
  // as páginas de 16 KB do BG-B: o banco H tem 32 KB (páginas 0 e 1) e o I fica em 0x8000 (página 2)
  mapGen++;
}

uint32_t NDS::vramOffset(uint32_t a) const {
  int32_t p;
  switch ((a >> 21) & 7) {
    case 0: p = pgBgA[(a >> 14) & 31]; break;
    case 1: p = pgBgB[(a >> 14) & 7]; break;
    case 2: p = pgObjA[(a >> 14) & 15]; break;
    case 3: p = pgObjB[(a >> 14) & 7]; break;
    case 4: {
      const uint32_t off = a & 0xFFFFF;
      if (off >= 0xA4000) return ~0u;
      int b = 8;
      while (b > 0 && off < BANK_OFF[b]) b--;
      if ((vramcnt[b] & 0x87) != 0x80) return ~0u;
      return off;
    }
    default: return ~0u;
  }
  return p < 0 ? ~0u : uint32_t(p) + (a & 0x3FFF);
}

void NDS::writeVramcnt(int bank, uint8_t v) {
  static const uint8_t MSTMASK[9] = {3, 3, 7, 7, 7, 7, 7, 3, 3};
  vramcnt[bank] = v & (0x98 | MSTMASK[bank]);
  remapVram();
}

void NDS::writeWramcnt(uint8_t v) {
  wramcnt = v & 3;
  mapGen++;
}

uint8_t* NDS::wramPtr9(uint32_t a) {
  switch (wramcnt) {
    case 0: return &wram[a & 0x7FFF];
    case 1: return &wram[0x4000 + (a & 0x3FFF)];
    case 2: return &wram[a & 0x3FFF];
    default: return nullptr;
  }
}

uint8_t* NDS::wramPtr7(uint32_t a) {
  if (a >= 0x03800000) return &wram7[a & 0xFFFF];
  switch (wramcnt) {
    case 0: return &wram7[a & 0xFFFF];
    case 1: return &wram[a & 0x3FFF];
    case 2: return &wram[0x4000 + (a & 0x3FFF)];
    default: return &wram[a & 0x7FFF];
  }
}

void NDS::updateTcm() {
  dtcmBase = dtcmReg & 0xFFFFF000u;
  uint32_t sh = (dtcmReg >> 1) & 0x1F;
  dtcmSize = sh > 22 ? 0x80000000u : 512u << sh;
  if (dtcmSize < 0x1000) dtcmSize = 0x1000;
  sh = (itcmReg >> 1) & 0x1F;
  itcmSize = sh > 22 ? 0x80000000u : 512u << sh;
  if (itcmSize < 0x1000) itcmSize = 0x1000;
  if (!(cpCtrl & (1u << 18))) itcmSize = 0;
  if (!(cpCtrl & (1u << 16))) dtcmSize = 0;
  arm9.excBase = (cpCtrl & (1u << 13)) ? 0xFFFF0000u : 0;
  mapGen++;
}

// ================================================================== ARM9
#define TCM_READ(N, LD)                                                                          \
  if (a < s.itcmSize) { if (!(s.cpCtrl & (1u << 19))) return LD(&s.itcm[a & (0x7FFF & ~(N - 1))]); return 0; } \
  if (a - s.dtcmBase < s.dtcmSize) { if (!(s.cpCtrl & (1u << 17))) return LD(&s.dtcm[(a - s.dtcmBase) & (0x3FFF & ~(N - 1))]); return 0; }

uint32_t Bus9::read32(uint32_t a) {
  a &= ~3u;
  TCM_READ(4, ld32)
  switch (a >> 24) {
    case 0x02: return ld32(&s.mainRam[a & 0x3FFFFF]);
    case 0x03: { const uint8_t* p = s.wramPtr9(a); return p ? ld32(p) : 0; }
    case 0x04: return s.io9Read(a, 4);
    case 0x05: return ld32(&s.pal[a & 0x7FF]);
    case 0x06: { const uint32_t o = s.vramOffset(a); return o == ~0u ? 0 : ld32(&s.vram[o]); }
    case 0x07: return ld32(&s.oam[a & 0x7FF]);
    case 0x08: case 0x09: case 0x0A: return 0xFFFFFFFFu;
    case 0xFF: return a >= 0xFFFF0000u ? ld32(&s.bios9[a & 0xFFF]) : 0;
    default: return 0;
  }
}

uint32_t Bus9::read16(uint32_t a) {
  a &= ~1u;
  TCM_READ(2, ld16)
  switch (a >> 24) {
    case 0x02: return ld16(&s.mainRam[a & 0x3FFFFF]);
    case 0x03: { const uint8_t* p = s.wramPtr9(a); return p ? ld16(p) : 0; }
    case 0x04: return s.io9Read(a, 2);
    case 0x05: return ld16(&s.pal[a & 0x7FF]);
    case 0x06: { const uint32_t o = s.vramOffset(a); return o == ~0u ? 0 : ld16(&s.vram[o]); }
    case 0x07: return ld16(&s.oam[a & 0x7FF]);
    case 0x08: case 0x09: case 0x0A: return 0xFFFF;
    case 0xFF: return a >= 0xFFFF0000u ? ld16(&s.bios9[a & 0xFFF]) : 0;
    default: return 0;
  }
}

uint32_t Bus9::read8(uint32_t a) {
  if (a < s.itcmSize) return (s.cpCtrl & (1u << 19)) ? 0 : s.itcm[a & 0x7FFF];
  if (a - s.dtcmBase < s.dtcmSize) return (s.cpCtrl & (1u << 17)) ? 0 : s.dtcm[(a - s.dtcmBase) & 0x3FFF];
  switch (a >> 24) {
    case 0x02: return s.mainRam[a & 0x3FFFFF];
    case 0x03: { const uint8_t* p = s.wramPtr9(a); return p ? *p : 0; }
    case 0x04: return s.io9Read(a, 1);
    case 0x05: return s.pal[a & 0x7FF];
    case 0x06: { const uint32_t o = s.vramOffset(a); return o == ~0u ? 0 : s.vram[o]; }
    case 0x07: return s.oam[a & 0x7FF];
    case 0x08: case 0x09: case 0x0A: return 0xFF;
    case 0xFF: return a >= 0xFFFF0000u ? s.bios9[a & 0xFFF] : 0;
    default: return 0;
  }
}

void Bus9::write32(uint32_t a, uint32_t v) {
  a &= ~3u;
  if (a < s.itcmSize) { st32(&s.itcm[a & 0x7FFC], v); return; }
  if (a - s.dtcmBase < s.dtcmSize) { st32(&s.dtcm[(a - s.dtcmBase) & 0x3FFC], v); return; }
  switch (a >> 24) {
    case 0x02: st32(&s.mainRam[a & 0x3FFFFF], v); break;
    case 0x03: if (uint8_t* p = s.wramPtr9(a)) st32(p, v); break;
    case 0x04: s.io9Write(a, v, 4); break;
    case 0x05: st32(&s.pal[a & 0x7FF], v); break;
    case 0x06: { const uint32_t o = s.vramOffset(a); if (o != ~0u) st32(&s.vram[o], v); break; }
    case 0x07: st32(&s.oam[a & 0x7FF], v); break;
    default: break;
  }
}

void Bus9::write16(uint32_t a, uint32_t v) {
  a &= ~1u;
  if (a < s.itcmSize) { st16(&s.itcm[a & 0x7FFE], v); return; }
  if (a - s.dtcmBase < s.dtcmSize) { st16(&s.dtcm[(a - s.dtcmBase) & 0x3FFE], v); return; }
  switch (a >> 24) {
    case 0x02: st16(&s.mainRam[a & 0x3FFFFF], v); break;
    case 0x03: if (uint8_t* p = s.wramPtr9(a)) st16(p, v); break;
    case 0x04: s.io9Write(a, v, 2); break;
    case 0x05: st16(&s.pal[a & 0x7FF], v); break;
    case 0x06: { const uint32_t o = s.vramOffset(a); if (o != ~0u) st16(&s.vram[o], v); break; }
    case 0x07: st16(&s.oam[a & 0x7FF], v); break;
    default: break;
  }
}

void Bus9::write8(uint32_t a, uint32_t v) {
  if (a < s.itcmSize) { s.itcm[a & 0x7FFF] = uint8_t(v); return; }
  if (a - s.dtcmBase < s.dtcmSize) { s.dtcm[(a - s.dtcmBase) & 0x3FFF] = uint8_t(v); return; }
  switch (a >> 24) {
    case 0x02: s.mainRam[a & 0x3FFFFF] = uint8_t(v); break;
    case 0x03: if (uint8_t* p = s.wramPtr9(a)) *p = uint8_t(v); break;
    case 0x04: s.io9Write(a, v, 1); break;
    default: break;  // paleta, VRAM e OAM ignoram escritas de 8 bits no ARM9
  }
}

const uint8_t* Bus9::codePage(uint32_t a, uint32_t& lo, uint32_t& hi, uint32_t& mask) {
  if (a < s.itcmSize) {
    lo = 0; hi = s.itcmSize - 1; mask = 0x7FFF;
    return s.itcm.data();
  }
  switch (a >> 24) {
    case 0x02:
      if (s.itcmSize > 0x02000000u) return nullptr;
      lo = 0x02000000; hi = 0x02FFFFFF; mask = 0x3FFFFF;
      return s.mainRam.data();
    case 0x03:
      if (s.itcmSize > 0x03000000u) return nullptr;
      switch (s.wramcnt) {
        case 0: lo = 0x03000000; hi = 0x03FFFFFF; mask = 0x7FFF; return s.wram.data();
        case 1: lo = 0x03000000; hi = 0x03FFFFFF; mask = 0x3FFF; return s.wram.data() + 0x4000;
        case 2: lo = 0x03000000; hi = 0x03FFFFFF; mask = 0x3FFF; return s.wram.data();
        default: return nullptr;
      }
    case 0xFF:
      if (a < 0xFFFF0000u) return nullptr;
      lo = 0xFFFF0000u; hi = 0xFFFFFFFFu; mask = 0xFFF;
      return s.bios9.data();
    default: return nullptr;
  }
}

bool Bus9::swi(int n, uint32_t addr) { return s.hleSwi(0, n, addr); }

uint32_t Bus9::cp15Read(uint32_t crn, uint32_t crm, uint32_t op2) {
  switch (crn) {
    case 0:
      if (op2 == 1) return 0x0F0D2112;  // tipo de cache
      if (op2 == 2) return 0x00140180;  // tamanho das TCM
      return 0x41059461;               // ARM946E-S
    case 1: return s.cpCtrl;
    case 2: return s.cpMisc[op2 & 1];
    case 3: return s.cpMisc[2];
    case 5: return s.cpMisc[3 + (op2 & 1)];
    case 6: return s.cpRegion[crm & 7];
    case 9:
      if (crm == 1) return op2 == 0 ? s.dtcmReg : s.itcmReg;
      return 0;
    default: return 0;
  }
}

void Bus9::cp15Write(uint32_t crn, uint32_t crm, uint32_t op2, uint32_t v) {
  switch (crn) {
    case 1:
      s.cpCtrl = (v & 0x000FF085u) | 0x78u;
      s.updateTcm();
      break;
    case 2: s.cpMisc[op2 & 1] = v; break;
    case 3: s.cpMisc[2] = v; break;
    case 5: s.cpMisc[3 + (op2 & 1)] = v; break;
    case 6: s.cpRegion[crm & 7] = v; break;
    case 7:
      // esperar interrupção (não para se já houver uma pendente em IE & IF)
      if ((crm == 0 && op2 == 4) || (crm == 8 && op2 == 2)) s.arm9.halted = !(s.io9.ie & s.io9.ifr);
      break;
    case 9:
      if (crm == 1) {
        if (op2 == 0) s.dtcmReg = v; else s.itcmReg = v;
        s.updateTcm();
      }
      break;
    default: break;
  }
}

// ================================================================== ARM7
// RAM do Wi-Fi (0x04804000, 8 KB, espelhada a cada 64 KB); o rádio não existe (registradores lidos como 0)
uint8_t* Bus7::wifiRam(uint32_t a) {
  const uint32_t o = a & 0xFFFF;
  if (a >= 0x04810000 || o < 0x4000 || o >= 0x6000) return nullptr;
  return &s.wifiRam[o - 0x4000];
}

uint32_t Bus7::read32(uint32_t a) {
  a &= ~3u;
  switch (a >> 24) {
    case 0x00: return a < 0x4000 ? ld32(&s.bios7[a]) : 0;
    case 0x02: return ld32(&s.mainRam[a & 0x3FFFFF]);
    case 0x03: return ld32(s.wramPtr7(a));
    case 0x04:
      if (a >= 0x04800000) return wifiRam(a) ? ld32(wifiRam(a)) : 0;
      return s.io7Read(a, 4);
    case 0x06: { const int32_t p = s.pgArm7[(a >> 14) & 15]; return p < 0 ? 0 : ld32(&s.vram[p + (a & 0x3FFF)]); }
    case 0x08: case 0x09: case 0x0A: return 0xFFFFFFFFu;
    default: return 0;
  }
}

uint32_t Bus7::read16(uint32_t a) {
  a &= ~1u;
  switch (a >> 24) {
    case 0x00: return a < 0x4000 ? ld16(&s.bios7[a]) : 0;
    case 0x02: return ld16(&s.mainRam[a & 0x3FFFFF]);
    case 0x03: return ld16(s.wramPtr7(a));
    case 0x04:
      if (a >= 0x04800000) return wifiRam(a) ? ld16(wifiRam(a)) : 0;
      return s.io7Read(a, 2);
    case 0x06: { const int32_t p = s.pgArm7[(a >> 14) & 15]; return p < 0 ? 0 : ld16(&s.vram[p + (a & 0x3FFF)]); }
    case 0x08: case 0x09: case 0x0A: return 0xFFFF;
    default: return 0;
  }
}

uint32_t Bus7::read8(uint32_t a) {
  switch (a >> 24) {
    case 0x00: return a < 0x4000 ? s.bios7[a] : 0;
    case 0x02: return s.mainRam[a & 0x3FFFFF];
    case 0x03: return *s.wramPtr7(a);
    case 0x04:
      if (a >= 0x04800000) return wifiRam(a) ? *wifiRam(a) : 0;
      return s.io7Read(a, 1);
    case 0x06: { const int32_t p = s.pgArm7[(a >> 14) & 15]; return p < 0 ? 0 : s.vram[p + (a & 0x3FFF)]; }
    case 0x08: case 0x09: case 0x0A: return 0xFF;
    default: return 0;
  }
}

void Bus7::write32(uint32_t a, uint32_t v) {
  a &= ~3u;
  switch (a >> 24) {
    case 0x02: st32(&s.mainRam[a & 0x3FFFFF], v); break;
    case 0x03: st32(s.wramPtr7(a), v); break;
    case 0x04: if (a < 0x04800000) s.io7Write(a, v, 4); else if (uint8_t* p = wifiRam(a)) st32(p, v); break;
    case 0x06: { const int32_t p = s.pgArm7[(a >> 14) & 15]; if (p >= 0) st32(&s.vram[p + (a & 0x3FFF)], v); break; }
    default: break;
  }
}

void Bus7::write16(uint32_t a, uint32_t v) {
  a &= ~1u;
  switch (a >> 24) {
    case 0x02: st16(&s.mainRam[a & 0x3FFFFF], v); break;
    case 0x03: st16(s.wramPtr7(a), v); break;
    case 0x04: if (a < 0x04800000) s.io7Write(a, v, 2); else if (uint8_t* p = wifiRam(a)) st16(p, v); break;
    case 0x06: { const int32_t p = s.pgArm7[(a >> 14) & 15]; if (p >= 0) st16(&s.vram[p + (a & 0x3FFF)], v); break; }
    default: break;
  }
}

void Bus7::write8(uint32_t a, uint32_t v) {
  switch (a >> 24) {
    case 0x02: s.mainRam[a & 0x3FFFFF] = uint8_t(v); break;
    case 0x03: *s.wramPtr7(a) = uint8_t(v); break;
    case 0x04: if (a < 0x04800000) s.io7Write(a, v, 1); else if (uint8_t* p = wifiRam(a)) *p = uint8_t(v); break;
    case 0x06: { const int32_t p = s.pgArm7[(a >> 14) & 15]; if (p >= 0) s.vram[p + (a & 0x3FFF)] = uint8_t(v); break; }
    default: break;
  }
}

const uint8_t* Bus7::codePage(uint32_t a, uint32_t& lo, uint32_t& hi, uint32_t& mask) {
  switch (a >> 24) {
    case 0x00:
      if (a >= 0x4000) return nullptr;
      lo = 0; hi = 0x3FFF; mask = 0x3FFF;
      return s.bios7.data();
    case 0x02: lo = 0x02000000; hi = 0x02FFFFFF; mask = 0x3FFFFF; return s.mainRam.data();
    case 0x03:
      if (a >= 0x03800000) { lo = 0x03800000; hi = 0x03FFFFFF; mask = 0xFFFF; return s.wram7.data(); }
      switch (s.wramcnt) {
        case 0: lo = 0x03000000; hi = 0x037FFFFF; mask = 0xFFFF; return s.wram7.data();
        case 1: lo = 0x03000000; hi = 0x037FFFFF; mask = 0x3FFF; return s.wram.data();
        case 2: lo = 0x03000000; hi = 0x037FFFFF; mask = 0x3FFF; return s.wram.data() + 0x4000;
        default: lo = 0x03000000; hi = 0x037FFFFF; mask = 0x7FFF; return s.wram.data();
      }
    default: return nullptr;
  }
}

bool Bus7::swi(int n, uint32_t addr) { return s.hleSwi(1, n, addr); }

// ================================================================== interrupções, IPC
void NDS::updateIrq() {
  arm9.irqLine = io9.ime && (io9.ie & io9.ifr);
  arm7.irqLine = io7.ime && (io7.ie & io7.ifr);
  if (arm9.irqLine) arm9.halted = false;
  if (arm7.irqLine) { arm7.halted = false; haltcnt = 0; }
  // o ARM7 parado pelo HALTCNT acorda com qualquer IRQ habilitada (mesmo com IME = 0)
  if (io7.ie & io7.ifr) { if (arm7.halted && haltcnt == 0x80) { arm7.halted = false; haltcnt = 0; } }
  if (io9.ie & io9.ifr) arm9.halted = false;
}

void NDS::ipcFifoUpdate() {
  // condições de IRQ: envio vazio (bit 2) e recebimento não vazio (bit 10), por nível
  for (int c = 0; c < 2; c++) {
    CpuIo& me = cio(c);
    CpuIo& other = cio(1 - c);
    const uint32_t bit = c ? 0 : 0;
    (void)bit;
    if ((me.ipcFifoCnt & 0x8004) == 0x8004 && other.recv.empty()) {
      if (!(me.ipcFifoCnt & 0x0010)) { me.ipcFifoCnt |= 0x0010; me.ifr |= 1u << 17; }  // bit 4 interno: já avisou
    } else me.ipcFifoCnt &= ~0x0010;
    if ((me.ipcFifoCnt & 0x8400) == 0x8400 && !me.recv.empty()) {
      if (!(me.ipcFifoCnt & 0x0020)) { me.ipcFifoCnt |= 0x0020; me.ifr |= 1u << 18; }
    } else me.ipcFifoCnt &= ~0x0020;
  }
  updateIrq();
}

uint32_t NDS::ipcRead(int c, uint32_t a, int size) {
  CpuIo& me = cio(c);
  CpuIo& other = cio(1 - c);
  const uint32_t off = a & 0xFFFFFF;
  if (off == 0x100000) {  // IPCFIFORECV
    uint32_t v;
    if (!(me.ipcFifoCnt & 0x8000)) v = me.recv.last;
    else if (me.recv.empty()) { me.ipcFifoCnt |= 0x4000; v = me.recv.last; }
    else { v = me.recv.pop(); ipcFifoUpdate(); }
    if (size == 1) return (v >> ((a & 3) * 8)) & 0xFF;
    if (size == 2) return (v >> ((a & 2) * 8)) & 0xFFFF;
    return v;
  }
  uint32_t v = 0;
  if ((off & ~3u) == 0x180) {
    v = ((other.ipcSync >> 8) & 0xF) | (me.ipcSync & 0x4F00);
  } else if ((off & ~3u) == 0x184) {
    v = me.ipcFifoCnt & 0xC40C;
    if (other.recv.empty()) v |= 1;
    if (other.recv.full()) v |= 2;
    if (me.recv.empty()) v |= 0x100;
    if (me.recv.full()) v |= 0x200;
  } else if ((off & ~3u) == 0x188) {
    return 0;
  }
  if (size == 1) return (v >> ((a & 1) * 8)) & 0xFF;
  if (size == 4) return v;  // 0x180 de 32 bits: 0x182 é 0
  return v & 0xFFFF;
}

void NDS::ipcWrite(int c, uint32_t a, uint32_t v, int size) {
  CpuIo& me = cio(c);
  CpuIo& other = cio(1 - c);
  const uint32_t off = a & 0xFFF;
  if ((off & ~3u) == 0x180) {
    if (size == 1) { if (off & 1) v <<= 8; else return; }  // escrever só o byte baixo não muda nada
    me.ipcSync = (me.ipcSync & 0x000F) | (v & 0x4F00);
    if ((v & 0x2000) && (other.ipcSync & 0x4000)) other.ifr |= 1u << 16;
    updateIrq();
  } else if ((off & ~3u) == 0x184) {
    if (size == 1) {
      if (off & 1) v = (me.ipcFifoCnt & 0xFF) | (v << 8);
      else v = (me.ipcFifoCnt & 0xFF00) | v;
    }
    if (v & 0x0008) other.recv.clear();
    if (v & 0x4000) me.ipcFifoCnt &= ~0x4000;
    me.ipcFifoCnt = (me.ipcFifoCnt & 0x4030) | (v & 0x8404);
    ipcFifoUpdate();
  } else if ((off & ~3u) == 0x188) {
    if (!(me.ipcFifoCnt & 0x8000)) return;
    if (other.recv.full()) { me.ipcFifoCnt |= 0x4000; return; }
    other.recv.push(v);
    ipcFifoUpdate();
  }
}

// ================================================================== E/S do ARM9
uint32_t NDS::io9Read(uint32_t a, int size) {
  const uint32_t off = a & 0xFFFFFF;
  if (off >= 0x100000) {
    if (off < 0x100004) return ipcRead(0, a, size);
    if (off >= 0x100010 && off < 0x100014) {
      if (exmemcnt & 0x800) return 0;
      const uint32_t v = cart.readData();
      return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
    }
    return 0;
  }
  if (size == 4) {
    // registradores de 32 bits com leitura especial
    if (off >= 0x0B0 && off < 0x0E0) {
      const Dma& d = io9.dma[(off - 0xB0) / 12];
      const uint32_t r = (off - 0xB0) % 12;
      return r == 8 ? d.cnt : 0;
    }
    if (off >= 0x620 && off < 0x6A4) return gpu3d.readReg(a);
    if (off == 0x600 || off == 0x604) return gpu3d.readReg(a);
    if (off == 0x1A4) return (exmemcnt & 0x800) ? 0 : cart.romctrl;
    return io9Read(a, 2) | (io9Read(a + 2, 2) << 16);
  }
  if (size == 1) {
    const uint32_t h = io9Read(a & ~1u, 2);
    return (a & 1) ? h >> 8 : h & 0xFF;
  }
  // 16 bits
  if (off < 0x70 && off != 0x04 && off != 0x06) return gpuA.readReg(off, 2);
  if (off >= 0x1000 && off < 0x1070) return gpuB.readReg(off - 0x1000, 2);
  switch (off) {
    case 0x004: {
      uint16_t v = io9.dispstat & 0xFFB8;
      if (vcount >= 192 && vcount < 262) v |= 1;
      if (lineCycle >= HBLANK_AT) v |= 2;
      if (vcount == (((io9.dispstat >> 8) & 0xFF) | ((io9.dispstat & 0x80) << 1))) v |= 4;
      return v;
    }
    case 0x006: return vcount;
    case 0x0E0: case 0x0E2: case 0x0E4: case 0x0E6: case 0x0E8: case 0x0EA: case 0x0EC: case 0x0EE:
      return (io9.dmaFill[(off - 0xE0) >> 2] >> ((off & 2) * 8)) & 0xFFFF;
    case 0x100: case 0x104: case 0x108: case 0x10C: return readTimer(io9, (off - 0x100) >> 2);
    case 0x102: case 0x106: case 0x10A: case 0x10E: return io9.tm[(off - 0x100) >> 2].ctrl;
    case 0x130: return keys;
    case 0x132: return io9.keycnt;
    case 0x180: case 0x184: return ipcRead(0, a, 2);
    case 0x1A0: return (exmemcnt & 0x800) ? 0 : cart.auxspicnt;
    case 0x1A2: return (exmemcnt & 0x800) ? 0 : cart.spiOut;
    case 0x1A4: return (exmemcnt & 0x800) ? 0 : cart.romctrl & 0xFFFF;
    case 0x1A6: return (exmemcnt & 0x800) ? 0 : cart.romctrl >> 16;
    case 0x204: return exmemcnt;
    case 0x208: return io9.ime;
    case 0x210: return io9.ie & 0xFFFF;
    case 0x212: return io9.ie >> 16;
    case 0x214: return io9.ifr & 0xFFFF;
    case 0x216: return io9.ifr >> 16;
    case 0x240: return vramcnt[0] | (vramcnt[1] << 8);
    case 0x242: return vramcnt[2] | (vramcnt[3] << 8);
    case 0x244: return vramcnt[4] | (vramcnt[5] << 8);
    case 0x246: return vramcnt[6] | (wramcnt << 8);
    case 0x248: return vramcnt[7] | (vramcnt[8] << 8);
    case 0x280: return divcnt;
    case 0x290: case 0x292: case 0x294: case 0x296: return (uint64_t(divNumer) >> ((off - 0x290) * 8)) & 0xFFFF;
    case 0x298: case 0x29A: case 0x29C: case 0x29E: return (uint64_t(divDenom) >> ((off - 0x298) * 8)) & 0xFFFF;
    case 0x2A0: case 0x2A2: case 0x2A4: case 0x2A6: return (uint64_t(divResult) >> ((off - 0x2A0) * 8)) & 0xFFFF;
    case 0x2A8: case 0x2AA: case 0x2AC: case 0x2AE: return (uint64_t(divRem) >> ((off - 0x2A8) * 8)) & 0xFFFF;
    case 0x2B0: return sqrtcnt;
    case 0x2B4: return sqrtResult & 0xFFFF;
    case 0x2B6: return sqrtResult >> 16;
    case 0x2B8: case 0x2BA: case 0x2BC: case 0x2BE: return (sqrtParam >> ((off - 0x2B8) * 8)) & 0xFFFF;
    case 0x300: return postflg9;
    case 0x304: return powcnt1;
    default: break;
  }
  if (off >= 0x0B0 && off < 0x0E0) {
    const Dma& d = io9.dma[(off - 0xB0) / 12];
    const uint32_t r = (off - 0xB0) % 12;
    if (r == 8) return d.cnt & 0xFFFF;
    if (r == 10) return d.cnt >> 16;
    return 0;
  }
  if (off >= 0x320 && off < 0x700) return gpu3d.readReg(a);
  return 0;
}

void NDS::io9Write(uint32_t a, uint32_t v, int size) {
  const uint32_t off = a & 0xFFFFFF;
  if (size == 4) {
    if (off == 0x188) { ipcWrite(0, a, v, 4); return; }
    if (off >= 0x400 && off < 0x440) { gpu3d.writeFifo(v); return; }
    if (off >= 0x440 && off < 0x600) { gpu3d.writeCmd(static_cast<int>((off - 0x400) >> 2), v); return; }
    if (off == 0x1A4) { if (!(exmemcnt & 0x800)) cart.writeRomctrl(v); return; }
    if (off >= 0x0B0 && off < 0x0E0) {
      const int ch = static_cast<int>((off - 0xB0) / 12), r = static_cast<int>((off - 0xB0) % 12);
      Dma& d = io9.dma[ch];
      if (r == 0) d.sad = v & 0x0FFFFFFE;
      else if (r == 4) d.dad = v & 0x0FFFFFFE;
      else writeDmaCnt(0, ch, v);
      return;
    }
    if (off >= 0x0E0 && off < 0x0F0) { io9.dmaFill[(off - 0xE0) >> 2] = v; return; }
    if (off >= 0x100 && off < 0x110) {
      const int i = static_cast<int>((off - 0x100) >> 2);
      io9.tm[i].reload = v & 0xFFFF;
      writeTimerCtrl(io9, 0, i, v >> 16);
      return;
    }
    if (off >= 0x290 && off < 0x2C0) {
      const int64_t vv = v;
      switch (off) {
        case 0x290: divNumer = (divNumer & ~int64_t(0xFFFFFFFF)) | uint32_t(vv); divide(); return;
        case 0x294: divNumer = (divNumer & 0xFFFFFFFF) | (int64_t(uint32_t(vv)) << 32); divide(); return;
        case 0x298: divDenom = (divDenom & ~int64_t(0xFFFFFFFF)) | uint32_t(vv); divide(); return;
        case 0x29C: divDenom = (divDenom & 0xFFFFFFFF) | (int64_t(uint32_t(vv)) << 32); divide(); return;
        case 0x2B8: sqrtParam = (sqrtParam & ~0xFFFFFFFFull) | v; squareRoot(); return;
        case 0x2BC: sqrtParam = (sqrtParam & 0xFFFFFFFFull) | (uint64_t(v) << 32); squareRoot(); return;
        default: return;
      }
    }
    if (off >= 0x320 && off < 0x3C0) {
      for (int k = 0; k < 4; k++) gpu3d.regs[off - 0x320 + k] = uint8_t(v >> (8 * k));
      return;
    }
    if (off < 0x70 && off != 0x04) { gpuA.writeReg(off, v, 4); return; }
    if (off >= 0x1000 && off < 0x1070) { gpuB.writeReg(off - 0x1000, v, 4); return; }
    io9Write(a, v & 0xFFFF, 2);
    io9Write(a + 2, v >> 16, 2);
    return;
  }
  if (off >= 0x400 && off < 0x600) return;  // a FIFO de geometria só aceita 32 bits
  if (off < 0x70 && off != 0x04 && off != 0x05 && off != 0x06 && off != 0x07) { gpuA.writeReg(off, v, size); return; }
  if (off >= 0x1000 && off < 0x1070) { gpuB.writeReg(off - 0x1000, v, size); return; }
  if (off >= 0x320 && off < 0x3C0) {
    for (int k = 0; k < size; k++) gpu3d.regs[off - 0x320 + k] = uint8_t(v >> (8 * k));
    return;
  }
  if (size == 1) {
    // bytes isolados com efeito próprio
    if (off >= 0x240 && off < 0x24A) {
      if (off == 0x247) writeWramcnt(uint8_t(v));
      else writeVramcnt(off < 0x247 ? int(off - 0x240) : int(off - 0x241), uint8_t(v));
      return;
    }
    if (off >= 0x1A8 && off < 0x1B0) { if (!(exmemcnt & 0x800)) cart.cmd[off - 0x1A8] = uint8_t(v); return; }
    if (off == 0x1A2) { if (!(exmemcnt & 0x800)) cart.writeAuxSpiData(uint8_t(v)); return; }
    if (off == 0x208) { io9.ime = v & 1; updateIrq(); return; }
    if (off == 0x300) { postflg9 = (postflg9 & 1) | (v & 3); return; }
    if (off == 0x180 || off == 0x181 || off == 0x184 || off == 0x185) { ipcWrite(0, a, v, 1); return; }
    if (off >= 0x214 && off < 0x218) { io9.ifr &= ~(v << ((off - 0x214) * 8)); updateIrq(); return; }
    // demais: combina com o valor atual de 16 bits
    const uint32_t cur = io9Read(a & ~1u, 2);
    const uint32_t nv = (a & 1) ? (cur & 0xFF) | (v << 8) : (cur & 0xFF00) | v;
    io9Write(a & ~1u, nv, 2);
    return;
  }
  v &= 0xFFFF;
  switch (off) {
    case 0x004: io9.dispstat = (io9.dispstat & 0x7) | (v & 0xFFB8); return;
    case 0x006: return;
    case 0x0E0: case 0x0E2: case 0x0E4: case 0x0E6: case 0x0E8: case 0x0EA: case 0x0EC: case 0x0EE: {
      uint32_t& f = io9.dmaFill[(off - 0xE0) >> 2];
      f = (off & 2) ? (f & 0xFFFF) | (v << 16) : (f & 0xFFFF0000u) | v;
      return;
    }
    case 0x100: case 0x104: case 0x108: case 0x10C: io9.tm[(off - 0x100) >> 2].reload = v; return;
    case 0x102: case 0x106: case 0x10A: case 0x10E: writeTimerCtrl(io9, 0, (off - 0x100) >> 2, v); return;
    case 0x132: io9.keycnt = v; return;
    case 0x180: case 0x184: ipcWrite(0, a, v, 2); return;
    case 0x188: case 0x18A: return;
    case 0x1A0: if (!(exmemcnt & 0x800)) cart.writeAuxSpiCnt(v); return;
    case 0x1A2: if (!(exmemcnt & 0x800)) cart.writeAuxSpiData(uint8_t(v)); return;
    case 0x1A4: if (!(exmemcnt & 0x800)) cart.writeRomctrl((cart.romctrl & 0xFFFF0000u) | v); return;
    case 0x1A6: if (!(exmemcnt & 0x800)) cart.writeRomctrl((cart.romctrl & 0xFFFF) | (v << 16)); return;
    case 0x1A8: case 0x1AA: case 0x1AC: case 0x1AE:
      if (!(exmemcnt & 0x800)) { cart.cmd[off - 0x1A8] = uint8_t(v); cart.cmd[off - 0x1A7] = uint8_t(v >> 8); }
      return;
    case 0x204: exmemcnt = static_cast<uint16_t>((v & 0xE8FF) | 0x2000); return;
    case 0x208: io9.ime = v & 1; updateIrq(); return;
    case 0x210: io9.ie = (io9.ie & 0xFFFF0000u) | v; updateIrq(); return;
    case 0x212: io9.ie = (io9.ie & 0xFFFF) | (v << 16); updateIrq(); return;
    case 0x214: io9.ifr &= ~v; updateIrq(); return;
    case 0x216: io9.ifr &= ~(v << 16); updateIrq(); return;
    case 0x240: writeVramcnt(0, uint8_t(v)); writeVramcnt(1, uint8_t(v >> 8)); return;
    case 0x242: writeVramcnt(2, uint8_t(v)); writeVramcnt(3, uint8_t(v >> 8)); return;
    case 0x244: writeVramcnt(4, uint8_t(v)); writeVramcnt(5, uint8_t(v >> 8)); return;
    case 0x246: writeVramcnt(6, uint8_t(v)); writeWramcnt(uint8_t(v >> 8)); return;
    case 0x248: writeVramcnt(7, uint8_t(v)); writeVramcnt(8, uint8_t(v >> 8)); return;
    case 0x280: divcnt = static_cast<uint16_t>(v & 3); divide(); return;
    case 0x2B0: sqrtcnt = static_cast<uint16_t>(v & 1); squareRoot(); return;
    case 0x300: postflg9 = (postflg9 & 1) | (v & 3); return;
    case 0x304: {
      const uint16_t old = powcnt1;
      powcnt1 = static_cast<uint16_t>(v & 0x820F);
      if ((old ^ powcnt1) & 0x8000) {}
      return;
    }
    case 0x600: gpu3d.writeGxstat(v); return;
    case 0x602: gpu3d.writeGxstat(v << 16); return;
    default: break;
  }
  if (off >= 0x0B0 && off < 0x0E0) {
    const int ch = static_cast<int>((off - 0xB0) / 12), r = static_cast<int>((off - 0xB0) % 12);
    Dma& d = io9.dma[ch];
    switch (r) {
      case 0: d.sad = (d.sad & 0xFFFF0000u) | (v & 0xFFFE); break;
      case 2: d.sad = (d.sad & 0xFFFF) | ((v & 0x0FFF) << 16); break;
      case 4: d.dad = (d.dad & 0xFFFF0000u) | (v & 0xFFFE); break;
      case 6: d.dad = (d.dad & 0xFFFF) | ((v & 0x0FFF) << 16); break;
      case 8: d.cnt = (d.cnt & 0xFFFF0000u) | v; break;
      default: writeDmaCnt(0, ch, (d.cnt & 0xFFFF) | (v << 16)); break;
    }
    return;
  }
  if (off >= 0x290 && off < 0x2C0) {
    // metades de 16 bits do divisor/raiz
    const uint32_t base = off & ~3u;
    uint32_t cur = io9Read(a & ~3u, 4);
    cur = (off & 2) ? (cur & 0xFFFF) | (v << 16) : (cur & 0xFFFF0000u) | v;
    io9Write(base | 0x04000000, cur, 4);
  }
}

// ================================================================== E/S do ARM7
uint32_t NDS::io7Read(uint32_t a, int size) {
  const uint32_t off = a & 0xFFFFFF;
  if (off >= 0x100000) {
    if (off < 0x100004) return ipcRead(1, a, size);
    if (off >= 0x100010 && off < 0x100014) {
      if (!(exmemcnt & 0x800)) return 0;
      const uint32_t v = cart.readData();
      return size == 4 ? v : size == 2 ? (v & 0xFFFF) : (v & 0xFF);
    }
    return 0;
  }
  if (size == 4) {
    if (off >= 0x400 && off < 0x520) return spu.read(off, 4);
    if (off == 0x1A4) return (exmemcnt & 0x800) ? cart.romctrl : 0;
    if (off >= 0x0B0 && off < 0x0E0) {
      const Dma& d = io7.dma[(off - 0xB0) / 12];
      return (off - 0xB0) % 12 == 8 ? d.cnt : 0;
    }
    return io7Read(a, 2) | (io7Read(a + 2, 2) << 16);
  }
  if (size == 1) {
    if (off >= 0x400 && off < 0x520) return spu.read(off, 1);
    if (off == 0x138) return rtc.read() & 0xFF;
    const uint32_t h = io7Read(a & ~1u, 2);
    return (a & 1) ? h >> 8 : h & 0xFF;
  }
  if (off >= 0x400 && off < 0x520) return spu.read(off, 2);
  switch (off) {
    case 0x004: {
      uint16_t v = io7.dispstat & 0xFFB8;
      if (vcount >= 192 && vcount < 262) v |= 1;
      if (lineCycle >= HBLANK_AT) v |= 2;
      if (vcount == (((io7.dispstat >> 8) & 0xFF) | ((io7.dispstat & 0x80) << 1))) v |= 4;
      return v;
    }
    case 0x006: return vcount;
    case 0x100: case 0x104: case 0x108: case 0x10C: return readTimer(io7, (off - 0x100) >> 2);
    case 0x102: case 0x106: case 0x10A: case 0x10E: return io7.tm[(off - 0x100) >> 2].ctrl;
    case 0x130: return keys;
    case 0x132: return io7.keycnt;
    case 0x134: return 0x8000;
    case 0x136: return extkeys;
    case 0x138: return rtc.read();
    case 0x180: case 0x184: return ipcRead(1, a, 2);
    case 0x1A0: return (exmemcnt & 0x800) ? cart.auxspicnt : 0;
    case 0x1A2: return (exmemcnt & 0x800) ? cart.spiOut : 0;
    case 0x1A4: return (exmemcnt & 0x800) ? cart.romctrl & 0xFFFF : 0;
    case 0x1A6: return (exmemcnt & 0x800) ? cart.romctrl >> 16 : 0;
    case 0x1C0: return spi.cnt;
    case 0x1C2: return spi.data;
    case 0x204: return exmemstat | (exmemcnt & 0xE880);
    case 0x208: return io7.ime;
    case 0x210: return io7.ie & 0xFFFF;
    case 0x212: return io7.ie >> 16;
    case 0x214: return io7.ifr & 0xFFFF;
    case 0x216: return io7.ifr >> 16;
    case 0x240: {
      uint16_t v = 0;
      if ((vramcnt[2] & 0x87) == 0x82) v |= 1;
      if ((vramcnt[3] & 0x87) == 0x82) v |= 2;
      return v | (wramcnt << 8);
    }
    case 0x300: return postflg7 | (haltcnt << 8);
    case 0x304: return powcnt2;
    case 0x308: return biosProt & 0xFFFF;
    default: break;
  }
  if (off >= 0x0B0 && off < 0x0E0) {
    const Dma& d = io7.dma[(off - 0xB0) / 12];
    const uint32_t r = (off - 0xB0) % 12;
    if (r == 8) return d.cnt & 0xFFFF;
    if (r == 10) return d.cnt >> 16;
  }
  return 0;
}

void NDS::io7Write(uint32_t a, uint32_t v, int size) {
  const uint32_t off = a & 0xFFFFFF;
  if (off >= 0x400 && off < 0x520) { spu.write(off, v, size); return; }
  if (size == 4) {
    if (off == 0x188) { ipcWrite(1, a, v, 4); return; }
    if (off == 0x1A4) { if (exmemcnt & 0x800) cart.writeRomctrl(v); return; }
    if (off >= 0x0B0 && off < 0x0E0) {
      const int ch = static_cast<int>((off - 0xB0) / 12), r = static_cast<int>((off - 0xB0) % 12);
      Dma& d = io7.dma[ch];
      if (r == 0) d.sad = v & 0x0FFFFFFE;
      else if (r == 4) d.dad = v & 0x0FFFFFFE;
      else writeDmaCnt(1, ch, v);
      return;
    }
    if (off >= 0x100 && off < 0x110) {
      const int i = static_cast<int>((off - 0x100) >> 2);
      io7.tm[i].reload = v & 0xFFFF;
      writeTimerCtrl(io7, 1, i, v >> 16);
      return;
    }
    io7Write(a, v & 0xFFFF, 2);
    io7Write(a + 2, v >> 16, 2);
    return;
  }
  if (size == 1) {
    switch (off) {
      case 0x138: rtc.write((rtc.pins & 0xFF00) | v); return;
      case 0x139: return;
      case 0x1C2: spi.writeData(uint8_t(v)); return;
      case 0x1A2: if (exmemcnt & 0x800) cart.writeAuxSpiData(uint8_t(v)); return;
      case 0x208: io7.ime = v & 1; updateIrq(); return;
      case 0x300: postflg7 = (postflg7 & 1) | (v & 1); return;
      case 0x301:
        haltcnt = uint8_t(v & 0xC0);
        // Halt (e Sleep, tratado como Halt): acorda com qualquer IRQ em IE & IF
        if ((v & 0x80) && !(io7.ie & io7.ifr)) arm7.halted = true;
        else haltcnt = 0;
        return;
      case 0x180: case 0x181: case 0x184: case 0x185: ipcWrite(1, a, v, 1); return;
      default: break;
    }
    if (off >= 0x1A8 && off < 0x1B0) { if (exmemcnt & 0x800) cart.cmd[off - 0x1A8] = uint8_t(v); return; }
    if (off >= 0x214 && off < 0x218) { io7.ifr &= ~(v << ((off - 0x214) * 8)); updateIrq(); return; }
    const uint32_t cur = io7Read(a & ~1u, 2);
    const uint32_t nv = (a & 1) ? (cur & 0xFF) | (v << 8) : (cur & 0xFF00) | v;
    io7Write(a & ~1u, nv, 2);
    return;
  }
  v &= 0xFFFF;
  switch (off) {
    case 0x004: io7.dispstat = (io7.dispstat & 0x7) | (v & 0xFFB8); return;
    case 0x100: case 0x104: case 0x108: case 0x10C: io7.tm[(off - 0x100) >> 2].reload = v; return;
    case 0x102: case 0x106: case 0x10A: case 0x10E: writeTimerCtrl(io7, 1, (off - 0x100) >> 2, v); return;
    case 0x132: io7.keycnt = v; return;
    case 0x138: rtc.write(v); return;
    case 0x180: case 0x184: ipcWrite(1, a, v, 2); return;
    case 0x1A0: if (exmemcnt & 0x800) cart.writeAuxSpiCnt(v); return;
    case 0x1A2: if (exmemcnt & 0x800) cart.writeAuxSpiData(uint8_t(v)); return;
    case 0x1A4: if (exmemcnt & 0x800) cart.writeRomctrl((cart.romctrl & 0xFFFF0000u) | v); return;
    case 0x1A6: if (exmemcnt & 0x800) cart.writeRomctrl((cart.romctrl & 0xFFFF) | (v << 16)); return;
    case 0x1A8: case 0x1AA: case 0x1AC: case 0x1AE:
      if (exmemcnt & 0x800) { cart.cmd[off - 0x1A8] = uint8_t(v); cart.cmd[off - 0x1A7] = uint8_t(v >> 8); }
      return;
    case 0x1C0: spi.writeCnt(v); return;
    case 0x1C2: spi.writeData(uint8_t(v)); return;
    case 0x204: exmemstat = static_cast<uint16_t>(v & 0x7F); return;
    case 0x208: io7.ime = v & 1; updateIrq(); return;
    case 0x210: io7.ie = (io7.ie & 0xFFFF0000u) | v; updateIrq(); return;
    case 0x212: io7.ie = (io7.ie & 0xFFFF) | (v << 16); updateIrq(); return;
    case 0x214: io7.ifr &= ~v; updateIrq(); return;
    case 0x216: io7.ifr &= ~(v << 16); updateIrq(); return;
    case 0x300: io7Write(a, v & 0xFF, 1); io7Write(a + 1, v >> 8, 1); return;
    case 0x304: powcnt2 = static_cast<uint16_t>(v & 3); return;
    case 0x308: if (!biosProt) biosProt = v; return;
    default: break;
  }
  if (off >= 0x0B0 && off < 0x0E0) {
    const int ch = static_cast<int>((off - 0xB0) / 12), r = static_cast<int>((off - 0xB0) % 12);
    Dma& d = io7.dma[ch];
    switch (r) {
      case 0: d.sad = (d.sad & 0xFFFF0000u) | (v & 0xFFFE); break;
      case 2: d.sad = (d.sad & 0xFFFF) | ((v & 0x0FFF) << 16); break;
      case 4: d.dad = (d.dad & 0xFFFF0000u) | (v & 0xFFFE); break;
      case 6: d.dad = (d.dad & 0xFFFF) | ((v & 0x0FFF) << 16); break;
      case 8: d.cnt = (d.cnt & 0xFFFF0000u) | v; break;
      default: writeDmaCnt(1, ch, (d.cnt & 0xFFFF) | (v << 16)); break;
    }
  }
}

}  // namespace indigo::nds
