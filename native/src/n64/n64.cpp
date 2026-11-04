// Nintendo 64: mapa de memória, interfaces do RCP (MI, VI, AI, PI, SI, RI, SP, DP), PIF/joybus, saves, partida
// HLE e o laço de um quadro. Registros conforme a documentação pública do hardware (n64brew.dev e manuais do SDK).
#include "n64.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace indigo::n64 {

namespace {
const char STATE_MAGIC[4] = {'I', 'N', '6', '4'};
constexpr int64_t SLICE = 512;  // tiques entre as trocas CPU/RSP

inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

// tipo de save pelos 2 caracteres do código do jogo (cabeçalho 0x3C-0x3D). Só os tipos que não são o padrão
// (EEPROM 4K): os jogos sem entrada usam EEPROM 4K e SRAM/FlashRAM conforme acessarem.
struct SaveId { char id[3]; SaveType t; };
constexpr SaveId SAVE_IDS[] = {
  {"PD", SaveType::Eeprom16k}, {"B7", SaveType::Eeprom16k}, {"YS", SaveType::Eeprom16k}, {"DO", SaveType::Eeprom16k},
  {"ZL", SaveType::Sram32k}, {"AL", SaveType::Sram32k}, {"FZ", SaveType::Sram32k}, {"TE", SaveType::Sram32k},
  {"OB", SaveType::Sram32k},
  {"ZS", SaveType::Flash}, {"MQ", SaveType::Flash}, {"CC", SaveType::Flash}, {"SQ", SaveType::Flash},
};

// CRC-8 (polinômio 0x85) dos 32 bytes de dados do Controller Pak, como o PIF calcula
uint8_t pakCrc(const uint8_t* d) {
  uint8_t crc = 0;
  for (int i = 0; i <= 32; i++) {
    for (int b = 7; b >= 0; b--) {
      const uint8_t x = (crc & 0x80) ? 0x85 : 0;
      crc = uint8_t(crc << 1);
      if (i < 32 && (d[i] >> b) & 1) crc |= 1;
      crc ^= x;
    }
  }
  return crc;
}
}  // namespace

// --------------------------------------------------------------------------------------- detecção e ordem
std::vector<uint8_t> toZ64(const std::vector<uint8_t>& r) {
  if (r.size() < 0x1000) return {};
  std::vector<uint8_t> o(r.size() & ~size_t(3));
  if (r[0] == 0x80 && r[1] == 0x37) {
    std::copy(r.begin(), r.begin() + long(o.size()), o.begin());
  } else if (r[0] == 0x37 && r[1] == 0x80) {
    for (size_t i = 0; i + 1 < o.size(); i += 2) { o[i] = r[i + 1]; o[i + 1] = r[i]; }
  } else if (r[0] == 0x40 && r[1] == 0x12) {
    for (size_t i = 0; i + 3 < o.size(); i += 4) { o[i] = r[i + 3]; o[i + 1] = r[i + 2]; o[i + 2] = r[i + 1]; o[i + 3] = r[i]; }
  } else {
    return {};
  }
  return o;
}

bool isN64Rom(const std::vector<uint8_t>& r, const std::string& n) {
  if (r.size() < 0x1000) return false;
  // primeiros 4 bytes do cabeçalho (configuração do PI do domínio 1): 80 37 12 40 em qualquer das três ordens
  const bool z = r[0] == 0x80 && r[1] == 0x37 && r[2] == 0x12 && r[3] == 0x40;
  const bool v = r[0] == 0x37 && r[1] == 0x80 && r[2] == 0x40 && r[3] == 0x12;
  const bool w = r[0] == 0x40 && r[1] == 0x12 && r[2] == 0x37 && r[3] == 0x80;
  (void)n;
  return z || v || w;
}

// --------------------------------------------------------------------------------------- construção
N64::N64(const std::vector<uint8_t>& z64) : cpu(*this), rsp(*this), rdp(*this) {
  rdram.assign(RDRAM_SIZE, 0);
  romSize = uint32_t(std::min<size_t>(z64.size(), 64u << 20) & ~size_t(3));
  rom.assign(romSize, 0);
  for (uint32_t i = 0; i < romSize; i += 4) {
    const uint32_t w = be32(&z64[i]);
    std::memcpy(&rom[i], &w, 4);
  }
  // nome (0x20, 20 bytes) e código (0x3B, 4 bytes)
  for (int i = 0; i < 20; i++) {
    const char c = char(z64[0x20 + i]);
    title += (c >= 32 && c < 127) ? c : ' ';
  }
  while (!title.empty() && title.back() == ' ') title.pop_back();
  while (!title.empty() && title.front() == ' ') title.erase(title.begin());
  for (int i = 0x3B; i < 0x3F; i++) { const char c = char(z64[i]); gameId += (c >= 32 && c < 127) ? c : '?'; }
  pal = gameId.size() == 4 && std::strchr("DFIPSUXYL", gameId[3]) != nullptr;
  // CIC pelo CRC32 do código de partida (0x40-0xFFF)
  switch (crc32(&z64[0x40], 0xFC0)) {
    case 0x6170A4A1: cic = 6101; cicSeed = 0x3F; break;
    case 0x009E9EA3: cic = 7102; cicSeed = 0x3F; break;
    case 0x0B050EE0: cic = 6103; cicSeed = 0x78; break;
    case 0x98BC2C86: cic = 6105; cicSeed = 0x91; break;
    case 0xACC8580A: cic = 6106; cicSeed = 0x85; break;
    default: cic = 6102; cicSeed = 0x3F; break;
  }
  detectSave();
  eeprom.assign(saveType == SaveType::Eeprom16k ? 2048 : 512, 0xFF);
  sram.assign(saveType == SaveType::Sram96k ? 98304 : 32768, 0xFF);
  flash.assign(131072, 0xFF);
  mempak.assign(32768, 0);
  fb.assign(640 * 576, 0xFF000000u);
  aud_.assign(1 << 15, 0.0f);
  reset();
}

void N64::detectSave() {
  saveType = SaveType::Auto;
  const uint8_t* id = reinterpret_cast<const uint8_t*>(gameId.c_str());
  if (gameId.size() != 4) return;
  if (id[1] == 'E' && id[2] == 'D') {
    // cabeçalho "homebrew": byte 0x3F (aqui o 4º caractere) traz o save nos 4 bits altos
    uint32_t w;
    std::memcpy(&w, &rom[0x3C], 4);
    switch ((w & 0xFF) >> 4) {
      case 0: saveType = SaveType::Nenhum; break;
      case 1: saveType = SaveType::Eeprom4k; break;
      case 2: saveType = SaveType::Eeprom16k; break;
      case 3: saveType = SaveType::Sram32k; break;
      case 4: saveType = SaveType::Sram96k; break;
      case 5: saveType = SaveType::Flash; break;
      case 6: saveType = SaveType::Sram32k; break;
      default: break;
    }
    return;
  }
  for (const auto& s : SAVE_IDS)
    if (s.id[0] == char(id[1]) && s.id[1] == char(id[2])) { saveType = s.t; return; }
}

void N64::reset() {
  std::fill(rdram.begin(), rdram.end(), 0);
  std::memset(spmem, 0, sizeof spmem);
  std::memset(sp, 0, sizeof sp);
  std::memset(dpc, 0, sizeof dpc);
  std::memset(vi, 0, sizeof vi);
  std::memset(ai, 0, sizeof ai);
  std::memset(pi, 0, sizeof pi);
  std::memset(ri, 0, sizeof ri);
  std::memset(si, 0, sizeof si);
  std::memset(pifRam, 0, sizeof pifRam);
  miMode = miIntr = miMask = 0;
  cpu.reset();
  rsp.reset();
  rdp.reset();
  sp[4] = 1;  // RSP parado
  dpc[3] = 0;
  line_ = field_ = 0;
  piDoneAt_ = siDoneAt_ = 0;
  pifPending_ = false;
  aiN_ = 0; aiPos_ = 0; aiFrac_ = 0; aiL_ = aiR_ = 0; outFrac_ = 0;
  flashMode_ = 0; flashOffset_ = flashEraseOffset_ = 0; flashStatus_ = 0;
  hleBoot();
}

// partida HLE: o que a ROM do PIF e o IPL3 do cartucho deixam pronto
void N64::hleBoot() {
  // 4 KB iniciais do cartucho na DMEM
  std::memcpy(spmem, rom.data(), std::min<uint32_t>(0x1000, romSize));
  uint32_t entry;
  std::memcpy(&entry, &rom[8], 4);
  if (cic == 6103) entry -= 0x100000;
  if (cic == 6106) entry -= 0x200000;
  const uint32_t dest = entry & 0x1FFFFFFF;
  const uint32_t n = std::min<uint32_t>(0x100000, romSize > 0x1000 ? romSize - 0x1000 : 0);
  if (dest < RDRAM_SIZE) std::memcpy(&rdram[dest & ~3u], &rom[0x1000], std::min<uint32_t>(n, RDRAM_SIZE - (dest & ~3u)));
  // variáveis que o IPL3 deixa para o sistema (osTvType, osRomType, osRomBase, osResetType, osVersion, osMemSize)
  auto w32 = [&](uint32_t a, uint32_t v) { std::memcpy(&rdram[a], &v, 4); };
  w32(0x300, pal ? 0 : 1);
  w32(0x304, 0);
  w32(0x308, 0xB0000000u);
  w32(0x30C, 0);
  w32(0x314, 0);
  w32(0x318, RDRAM_SIZE);
  if (cic == 6105) w32(0x3F0, RDRAM_SIZE);
  // registros da CPU
  cpu.gpr[11] = 0xFFFFFFFFA4000040ull;
  cpu.gpr[19] = 0;
  cpu.gpr[20] = pal ? 0 : 1;
  cpu.gpr[21] = 0;
  cpu.gpr[22] = cicSeed;
  cpu.gpr[23] = 0;
  cpu.gpr[29] = 0xFFFFFFFFA4001FF0ull;
  cpu.gpr[31] = 0xFFFFFFFFA4001550ull;
  // interfaces como o IPL deixa
  uint32_t h;
  std::memcpy(&h, &rom[0], 4);
  pi[5] = h & 0xFF; pi[6] = (h >> 8) & 0xFF; pi[7] = (h >> 16) & 0x0F; pi[8] = (h >> 20) & 0x03;
  ri[0] = 0x0E; ri[1] = 0x40; ri[3] = 0x14; ri[4] = 0x63634;
  miMode = 0x80;
  vi[6] = pal ? 625 : 525;
  cpu.jump(uint64_t(int64_t(int32_t(entry))));
}

// --------------------------------------------------------------------------------------- interrupções
void N64::raise(int bit) {
  miIntr |= 1u << bit;
  cpu.setIp(2, (miIntr & miMask) != 0);
}
void N64::clear(int bit) {
  miIntr &= ~(1u << bit);
  cpu.setIp(2, (miIntr & miMask) != 0);
}

// --------------------------------------------------------------------------------------- SP / DP
uint32_t N64::spRead(int reg) {
  switch (reg & 7) {
    case 4: return sp[4];
    case 5: case 6: return 0;
    case 7: { const uint32_t v = sp[7]; sp[7] = 1; return v; }
    default: return sp[reg & 7];
  }
}

void N64::spWrite(int reg, uint32_t v) {
  switch (reg & 7) {
    case 0: sp[0] = v & 0x1FF8; break;
    case 1: sp[1] = v & 0xFFFFF8; break;
    case 2: sp[2] = v; spDma(false); break;
    case 3: sp[3] = v; spDma(true); break;
    case 4: {
      uint32_t& s = sp[4];
      if ((v & 1) && !(v & 2)) s &= ~1u;
      if ((v & 2) && !(v & 1)) s |= 1u;
      if (v & 4) s &= ~2u;
      if ((v & 8) && !(v & 16)) clear(0);
      if ((v & 16) && !(v & 8)) raise(0);
      if ((v & 32) && !(v & 64)) s &= ~0x20u;
      if ((v & 64) && !(v & 32)) s |= 0x20u;
      if ((v & 128) && !(v & 256)) s &= ~0x40u;
      if ((v & 256) && !(v & 128)) s |= 0x40u;
      for (int i = 0; i < 8; i++) {
        const bool c = v & (1u << (9 + 2 * i)), st = v & (1u << (10 + 2 * i));
        if (c && !st) s &= ~(1u << (7 + i));
        if (st && !c) s |= 1u << (7 + i);
      }
      break;
    }
    case 7: sp[7] = 0; break;
    default: break;
  }
}

void N64::spDma(bool toRdram) {
  const uint32_t lenReg = toRdram ? sp[3] : sp[2];
  const uint32_t len = ((lenReg & 0xFFF) | 7) + 1;
  const uint32_t count = ((lenReg >> 12) & 0xFF) + 1, skip = (lenReg >> 20) & 0xFF8;
  uint32_t mem = sp[0] & 0x1FF8, dram = sp[1] & 0xFFFFF8;
  const uint32_t bank = mem & 0x1000;
  for (uint32_t c = 0; c < count; c++) {
    for (uint32_t i = 0; i < len; i += 4) {
      uint8_t* s = &spmem[bank | ((mem + i) & 0xFFF)];
      const uint32_t d = (dram + i) & (RDRAM_SIZE - 1);
      if (toRdram) std::memcpy(&rdram[d], s, 4);
      else std::memcpy(s, &rdram[d], 4);
    }
    mem = bank | ((mem + len) & 0xFFF);
    dram = (dram + len + skip) & 0xFFFFF8;
  }
  sp[0] = mem; sp[1] = dram;
  if (toRdram) sp[3] = 0xFF8; else sp[2] = 0xFF8;
}

uint32_t N64::dpRead(int reg) {
  switch (reg & 7) {
    case 3: return dpc[3] | 0x80;
    case 4: return uint32_t(cpu.cycles) & 0xFFFFFF;
    default: return dpc[reg & 7];
  }
}

void N64::dpWrite(int reg, uint32_t v) {
  switch (reg & 7) {
    case 0: dpc[0] = v & 0xFFFFF8; dpc[3] |= 0x400; break;
    case 1:
      dpc[1] = v & 0xFFFFF8;
      if (dpc[3] & 0x400) { dpc[2] = dpc[0]; dpc[3] &= ~0x400u; }
      if (!(dpc[3] & 2)) rdp.run();  // freeze segura a lista
      break;
    case 3: {
      uint32_t& s = dpc[3];
      if (v & 1) s &= ~1u;
      if (v & 2) s |= 1u;
      if (v & 4) { s &= ~2u; rdp.run(); }
      if (v & 8) s |= 2u;
      if (v & 16) s &= ~4u;
      if (v & 32) s |= 4u;
      break;
    }
    default: break;
  }
}

// --------------------------------------------------------------------------------------- mapa de memória
uint32_t N64::read32(uint32_t pa) {
  pa &= 0x1FFFFFFC;
  if (pa < 0x03F00000) {
    if (pa < RDRAM_SIZE) { uint32_t v; std::memcpy(&v, &rdram[pa], 4); return v; }
    return 0;
  }
  if (pa < 0x04000000) return 0;  // registros da RDRAM
  if (pa < 0x04040000) { uint32_t v; std::memcpy(&v, &spmem[pa & 0x1FFC], 4); return v; }
  if (pa < 0x04080000) return spRead(int(pa >> 2) & 7);
  if (pa < 0x04100000) return (pa & 4) ? 0 : rsp.pc;
  if (pa < 0x04200000) return dpRead(int(pa >> 2) & 7);
  if (pa < 0x04300000) return 0;  // DPS
  if (pa < 0x04400000) {
    switch ((pa >> 2) & 3) {
      case 0: return miMode;
      case 1: return 0x02020102;
      case 2: return miIntr;
      default: return miMask;
    }
  }
  if (pa < 0x04500000) {
    const int r = int(pa >> 2) & 15;
    if (r == 4) return uint32_t((line_ * 2) | (field_ & ((vi[0] >> 6) & 1)));
    return r < 14 ? vi[r] : 0;
  }
  if (pa < 0x04600000) {
    switch ((pa >> 2) & 7) {
      case 1: return aiN_ ? aiQ_[0].len - std::min(aiPos_, aiQ_[0].len) : 0;
      case 3: return (aiN_ >= 2 ? 0x80000001u : 0) | (aiN_ ? 0x40000000u : 0) | 0x01100000u;
      default: return 0;
    }
  }
  if (pa < 0x04700000) {
    const int r = int(pa >> 2) & 15;
    if (r == 4) return (piDoneAt_ ? 1u : 0u) | ((miIntr >> 4) & 1 ? 8u : 0u);
    if (r == 2 || r == 3) return 0x7F;
    return r < 13 ? pi[r] : 0;
  }
  if (pa < 0x04800000) return ri[(pa >> 2) & 7];
  if (pa < 0x04900000) {
    const int r = int(pa >> 2) & 7;
    if (r == 6) return (siDoneAt_ ? 1u : 0u) | ((miIntr >> 1) & 1 ? 0x1000u : 0u);
    return r < 7 ? si[r] : 0;
  }
  if (pa < 0x05000000) return 0;
  if (pa < 0x1FC00000) return cartRead(pa);
  if (pa < 0x1FC007C0) return 0;  // ROM do PIF: não existe aqui
  if (pa < 0x1FC00800) return be32(&pifRam[pa & 0x3C]);
  return 0;
}

void N64::write32(uint32_t pa, uint32_t v, uint32_t mask) {
  pa &= 0x1FFFFFFC;
  if (pa < 0x03F00000) {
    if (pa < RDRAM_SIZE) {
      uint32_t o;
      std::memcpy(&o, &rdram[pa], 4);
      o = (o & ~mask) | (v & mask);
      std::memcpy(&rdram[pa], &o, 4);
    }
    return;
  }
  if (pa < 0x04000000) return;
  if (pa < 0x04040000) {
    uint32_t o;
    std::memcpy(&o, &spmem[pa & 0x1FFC], 4);
    o = (o & ~mask) | (v & mask);
    std::memcpy(&spmem[pa & 0x1FFC], &o, 4);
    return;
  }
  if (pa < 0x04080000) { spWrite(int(pa >> 2) & 7, v); return; }
  if (pa < 0x04100000) { if (!(pa & 4)) rsp.setPc(v); return; }
  if (pa < 0x04200000) { dpWrite(int(pa >> 2) & 7, v); return; }
  if (pa < 0x04300000) return;
  if (pa < 0x04400000) {
    switch ((pa >> 2) & 3) {
      case 0:
        miMode = (miMode & ~0x7Fu) | (v & 0x7F);
        if (v & 0x80) miMode &= ~0x80u;
        if (v & 0x100) miMode |= 0x80u;
        if (v & 0x200) miMode &= ~0x100u;
        if (v & 0x400) miMode |= 0x100u;
        if (v & 0x800) clear(5);
        if (v & 0x1000) miMode &= ~0x200u;
        if (v & 0x2000) miMode |= 0x200u;
        break;
      case 3:
        for (int i = 0; i < 6; i++) {
          if (v & (1u << (2 * i))) miMask &= ~(1u << i);
          if (v & (1u << (2 * i + 1))) miMask |= 1u << i;
        }
        cpu.setIp(2, (miIntr & miMask) != 0);
        break;
      default: break;
    }
    return;
  }
  if (pa < 0x04500000) {
    const int r = int(pa >> 2) & 15;
    if (r == 4) { clear(3); return; }
    if (r < 14) vi[r] = v;
    return;
  }
  if (pa < 0x04600000) {
    switch ((pa >> 2) & 7) {
      case 0: ai[0] = v & 0xFFFFF8; break;
      case 1: {
        const uint32_t len = v & 0x3FFF8;
        if (aiN_ < 2 && len) {
          aiQ_[aiN_++] = {ai[0], len};
          if (aiN_ == 1) { aiPos_ = 0; raise(2); }
        }
        break;
      }
      case 2: ai[2] = v & 1; break;
      case 3: clear(2); break;
      case 4: ai[4] = v & 0x3FFF; break;
      case 5: ai[5] = v & 0xF; break;
      default: break;
    }
    return;
  }
  if (pa < 0x04700000) {
    const int r = int(pa >> 2) & 15;
    switch (r) {
      case 0: pi[0] = v & 0xFFFFFE; break;
      case 1: pi[1] = v & 0xFFFFFFFE; break;
      case 2: pi[2] = v; piDma(false); break;
      case 3: pi[3] = v; piDma(true); break;
      case 4: if (v & 2) clear(4); if (v & 1) piDoneAt_ = 0; break;
      default: if (r < 13) pi[r] = v & 0xFF; break;
    }
    return;
  }
  if (pa < 0x04800000) { ri[(pa >> 2) & 7] = v; return; }
  if (pa < 0x04900000) {
    const int r = int(pa >> 2) & 7;
    switch (r) {
      case 0: si[0] = v & 0xFFFFF8; break;
      case 1: siDma(true); break;   // PIF -> RDRAM
      case 4: siDma(false); break;  // RDRAM -> PIF
      case 6: clear(1); break;
      default: break;
    }
    return;
  }
  if (pa < 0x05000000) return;
  if (pa < 0x1FC00000) { cartWrite(pa, v); return; }
  if (pa >= 0x1FC007C0 && pa < 0x1FC00800) {
    uint8_t* p = &pifRam[pa & 0x3C];
    const uint32_t o = be32(p);
    const uint32_t n = (o & ~mask) | (v & mask);
    p[0] = uint8_t(n >> 24); p[1] = uint8_t(n >> 16); p[2] = uint8_t(n >> 8); p[3] = uint8_t(n);
    if ((pa & 0x3C) == 0x3C && (pifRam[63] & 1)) pifRun();
  }
}

// --------------------------------------------------------------------------------------- cartucho (PI)
uint32_t N64::cartRead(uint32_t pa) {
  if (pa >= 0x10000000) {
    const uint32_t o = pa - 0x10000000;
    if (o < romSize) { uint32_t v; std::memcpy(&v, &rom[o], 4); return v; }
    return ((pa & 0xFFFF) << 16) | (pa & 0xFFFF);
  }
  if (pa >= 0x08000000 && pa < 0x10000000) {
    if (flashUsed || saveType == SaveType::Flash) {
      if (flashMode_ == 1) return (pa & 4) ? uint32_t(flashStatus_) : uint32_t(flashStatus_ >> 32);
      return 0;
    }
    const uint32_t o = (pa & 0x7FFF) | (saveType == SaveType::Sram96k ? ((pa >> 18) & 3) * 0x8000 : 0);
    if (o + 4 <= sram.size()) return be32(&sram[o]);
    return 0;
  }
  return ((pa & 0xFFFF) << 16) | (pa & 0xFFFF);
}

void N64::cartWrite(uint32_t pa, uint32_t v) {
  if (pa >= 0x08000000 && pa < 0x10000000) {
    if ((pa & 0x1FFFF) >= 0x10000 && saveType != SaveType::Sram32k && saveType != SaveType::Sram96k) {
      flashUsed = true;
      flashCommand(v);
      return;
    }
    if (flashUsed || saveType == SaveType::Flash) return;
    const uint32_t o = (pa & 0x7FFF) | (saveType == SaveType::Sram96k ? ((pa >> 18) & 3) * 0x8000 : 0);
    if (o + 4 <= sram.size()) {
      sram[o] = uint8_t(v >> 24); sram[o + 1] = uint8_t(v >> 16); sram[o + 2] = uint8_t(v >> 8); sram[o + 3] = uint8_t(v);
      sramUsed = true; saveDirty = true;
    }
  }
}

void N64::flashCommand(uint32_t v) {
  switch (v >> 24) {
    case 0xD2:  // executa
      if (flashMode_ == 2) {
        if (flashEraseOffset_ == 0xFFFFFFFFu) std::fill(flash.begin(), flash.end(), 0xFF);
        else std::fill_n(flash.begin() + (flashEraseOffset_ & 0x1C000), 0x4000, 0xFF);
        saveDirty = true;
      } else if (flashMode_ == 3) {
        std::memcpy(&flash[flashOffset_ & 0x1FF80], flashPage_, 128);
        saveDirty = true;
      }
      break;
    case 0xE1: flashMode_ = 1; flashStatus_ = 0x1111800100C2001Eull; break;
    case 0xF0: flashMode_ = 0; flashStatus_ = 0x11118004F0000000ull; break;
    case 0x4B: flashMode_ = 2; flashEraseOffset_ = (v & 0xFFFF) * 128; flashStatus_ = 0x1111800800C2001Eull; break;
    case 0x3C: flashMode_ = 2; flashEraseOffset_ = 0xFFFFFFFFu; flashStatus_ = 0x1111800800C2001Eull; break;
    case 0xB4: flashMode_ = 3; break;
    case 0xA5: flashOffset_ = (v & 0xFFFF) * 128; flashStatus_ = 0x1111800400C2001Eull; break;
    default: break;
  }
}

void N64::piDma(bool toRdram) {
  const uint32_t len = std::min<uint32_t>(((toRdram ? pi[3] : pi[2]) & 0xFFFFFF) + 1, RDRAM_SIZE);
  uint32_t dram = pi[0] & 0xFFFFFE, cart = pi[1];
  auto dramByte = [&](uint32_t a) -> uint8_t& { return rdram[(a & (RDRAM_SIZE - 1)) ^ 3]; };
  if (toRdram) {
    if (cart >= 0x10000000 && cart < 0x1FC00000) {
      const uint32_t o = cart - 0x10000000;
      for (uint32_t i = 0; i < len; i++)
        dramByte(dram + i) = (o + i < romSize) ? rom[(o + i) ^ 3] : 0;
    } else if (cart >= 0x08000000 && cart < 0x10000000) {
      if (flashUsed || saveType == SaveType::Flash) {
        if (flashMode_ == 1) {
          for (uint32_t i = 0; i < len; i++) dramByte(dram + i) = uint8_t(flashStatus_ >> (56 - 8 * (i & 7)));
        } else {
          const uint32_t o = (cart & 0xFFFF) * 2;
          for (uint32_t i = 0; i < len; i++) dramByte(dram + i) = flash[(o + i) & 0x1FFFF];
        }
      } else {
        for (uint32_t i = 0; i < len; i++) {
          const uint32_t c = cart + i;
          const uint32_t o = (c & 0x7FFF) | (saveType == SaveType::Sram96k ? ((c >> 18) & 3) * 0x8000 : 0);
          dramByte(dram + i) = o < sram.size() ? sram[o] : 0xFF;
        }
      }
    } else {
      for (uint32_t i = 0; i < len; i++) dramByte(dram + i) = 0;
    }
  } else {
    if (cart >= 0x08000000 && cart < 0x10000000) {
      if (flashUsed || saveType == SaveType::Flash) {
        if (flashMode_ == 3) for (uint32_t i = 0; i < len && i < 128; i++) flashPage_[i] = dramByte(dram + i);
      } else {
        for (uint32_t i = 0; i < len; i++) {
          const uint32_t c = cart + i;
          const uint32_t o = (c & 0x7FFF) | (saveType == SaveType::Sram96k ? ((c >> 18) & 3) * 0x8000 : 0);
          if (o < sram.size()) sram[o] = dramByte(dram + i);
        }
        sramUsed = true; saveDirty = true;
      }
    }
  }
  pi[0] = (dram + len + 1) & 0xFFFFFE;
  pi[1] = (cart + len + 1) & ~1u;
  piDoneAt_ = cpu.cycles + 100 + len / 4;
}

// --------------------------------------------------------------------------------------- SI / PIF / joybus
void N64::siDma(bool toRdram) {
  const uint32_t d = si[0] & (RDRAM_SIZE - 1) & ~3u;
  if (toRdram) {
    if (pifPending_) { pifRun(); pifPending_ = false; }
    for (int i = 0; i < 64; i += 4) {
      const uint32_t w = be32(&pifRam[i]);
      std::memcpy(&rdram[(d + uint32_t(i)) & (RDRAM_SIZE - 1)], &w, 4);
    }
  } else {
    for (int i = 0; i < 64; i += 4) {
      uint32_t w;
      std::memcpy(&w, &rdram[(d + uint32_t(i)) & (RDRAM_SIZE - 1)], 4);
      pifRam[i] = uint8_t(w >> 24); pifRam[i + 1] = uint8_t(w >> 16); pifRam[i + 2] = uint8_t(w >> 8); pifRam[i + 3] = uint8_t(w);
    }
    if (pifRam[63] & 1) pifPending_ = true;
  }
  siDoneAt_ = cpu.cycles + 4096;
}

void N64::pifRun() {
  uint8_t* ram = pifRam;
  int ch = 0, i = 0;
  while (i < 63) {
    const uint8_t b = ram[i];
    if (b == 0xFE) break;
    if (b == 0xFF || b == 0xFD) { i++; continue; }
    if (b == 0x00) { ch++; i++; continue; }
    const int tx = b & 0x3F;
    const int rx = ram[i + 1] & 0x3F;
    if (i + 2 + tx + rx > 63) break;
    joybus(ch, &ram[i + 2], tx, &ram[i + 2 + tx], rx, ram[i + 1]);
    i += 2 + tx + rx;
    ch++;
  }
  ram[63] &= ~1u;
}

void N64::joybus(int ch, uint8_t* tx, int txn, uint8_t* rx, int rxn, uint8_t& rxflag) {
  if (txn < 1) return;
  const uint8_t cmd = tx[0];
  auto semResposta = [&] { rxflag |= 0x80; };
  if (ch < 4) {
    if (!padOn[ch]) { semResposta(); return; }
    switch (cmd) {
      case 0x00: case 0xFF:
        if (rxn >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = 0x01; }
        break;
      case 0x01:
        if (rxn >= 4) {
          rx[0] = uint8_t(pad[ch] >> 8); rx[1] = uint8_t(pad[ch]);
          rx[2] = uint8_t(stickX[ch]); rx[3] = uint8_t(stickY[ch]);
        }
        break;
      case 0x02:  // lê o Controller Pak
        if (txn >= 3 && rxn >= 33) {
          const uint32_t a = (uint32_t(tx[1]) << 8 | tx[2]) & ~0x1Fu;
          for (int k = 0; k < 32; k++) rx[k] = (a < 0x8000 && ch == 0) ? mempak[a + uint32_t(k)] : 0;
          rx[32] = pakCrc(rx);
        }
        break;
      case 0x03:  // grava no Controller Pak
        if (txn >= 35 && rxn >= 1) {
          const uint32_t a = (uint32_t(tx[1]) << 8 | tx[2]) & ~0x1Fu;
          if (a < 0x8000 && ch == 0) {
            std::memcpy(&mempak[a], &tx[3], 32);
            mempakUsed = true; saveDirty = true;
          }
          rx[0] = pakCrc(&tx[3]);
        }
        break;
      default: semResposta(); break;
    }
    return;
  }
  if (ch == 4 && saveType != SaveType::Nenhum && saveType != SaveType::Sram32k && saveType != SaveType::Sram96k &&
      saveType != SaveType::Flash) {
    const bool k16 = saveType == SaveType::Eeprom16k;
    switch (cmd) {
      case 0x00: case 0xFF:
        if (rxn >= 3) { rx[0] = 0x00; rx[1] = k16 ? 0xC0 : 0x80; rx[2] = 0x00; }
        return;
      case 0x04:
        if (txn >= 2 && rxn >= 8) {
          const uint32_t a = uint32_t(tx[1]) * 8;
          for (int k = 0; k < 8; k++) rx[k] = a + 8 <= eeprom.size() ? eeprom[a + uint32_t(k)] : 0xFF;
          eepromUsed = true;
        }
        return;
      case 0x05:
        if (txn >= 10) {
          const uint32_t a = uint32_t(tx[1]) * 8;
          if (a + 8 <= eeprom.size()) { std::memcpy(&eeprom[a], &tx[2], 8); saveDirty = true; }
          eepromUsed = true;
          if (rxn >= 1) rx[0] = 0;
        }
        return;
      default: break;
    }
  }
  semResposta();
}

// --------------------------------------------------------------------------------------- som (AI)
void N64::aiPop() {
  if (aiN_ == 0) return;
  aiQ_[0] = aiQ_[1];
  aiN_--;
  aiPos_ = 0;
  if (aiN_) raise(2);
}

void N64::audioPush(float l, float r) {
  if (audW_ - audR_ + 2 > aud_.size()) audR_ += 2;  // cheio: descarta o mais antigo
  aud_[audW_ % aud_.size()] = l;
  aud_[(audW_ + 1) % aud_.size()] = r;
  audW_ += 2;
}

void N64::aiAdvance(int64_t ciclos) {
  const double viClock = pal ? 49656530.0 : 48681812.0;
  const int64_t freq = int64_t(viClock / double((ai[4] & 0x3FFF) + 1));
  outFrac_ += ciclos * sampleRate_;
  while (outFrac_ >= CPU_CLOCK) {
    outFrac_ -= CPU_CLOCK;
    if (aiN_ && (ai[2] & 1) && freq > 0) {
      aiFrac_ += freq;
      while (aiFrac_ >= sampleRate_ && aiN_) {
        aiFrac_ -= sampleRate_;
        const uint32_t a = (aiQ_[0].addr + aiPos_) & (RDRAM_SIZE - 1) & ~3u;
        uint32_t w;
        std::memcpy(&w, &rdram[a], 4);
        aiL_ = int16_t(w >> 16); aiR_ = int16_t(w);
        aiPos_ += 4;
        if (aiPos_ >= aiQ_[0].len) aiPop();
      }
      if (!aiN_) aiFrac_ = 0;
    } else {
      aiL_ = int16_t(aiL_ * 15 / 16); aiR_ = int16_t(aiR_ * 15 / 16);
    }
    audioPush(aiL_ / 32768.0f, aiR_ / 32768.0f);
  }
}

void N64::pullAudio(float* l, float* r, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (audR_ < audW_) {
      l[i] = aud_[audR_ % aud_.size()];
      r[i] = aud_[(audR_ + 1) % aud_.size()];
      audR_ += 2;
    } else {
      l[i] = r[i] = 0;
    }
  }
}

// --------------------------------------------------------------------------------------- vídeo (VI)
void N64::renderVi() {
  const uint32_t tipo = vi[0] & 3;
  const uint32_t origem = vi[1] & 0xFFFFFF, larg = vi[2] & 0xFFF;
  int hs = int(vi[9] >> 16) & 0x3FF, he = int(vi[9]) & 0x3FF;
  int vs = int(vi[10] >> 16) & 0x3FF, ve = int(vi[10]) & 0x3FF;
  const int xs = int(vi[12] & 0xFFF), ys = int(vi[13] & 0xFFF);
  int cols = (he > hs && xs) ? (he - hs) * xs / 1024 : int(larg);
  int rows = (ve > vs && ys) ? ((ve - vs) / 2) * ys / 1024 : (larg > 320 ? 480 : 240);
  cols = std::clamp(cols, 1, 640);
  rows = std::clamp(rows, 1, 576);
  outW = cols <= 320 ? 320 : 640;
  outH = rows <= 240 ? 240 : rows <= 288 ? 288 : rows <= 480 ? 480 : 576;
  if (tipo < 2 || larg == 0) {
    std::fill(fb.begin(), fb.begin() + outW * outH, 0xFF000000u);
    return;
  }
  for (int oy = 0; oy < outH; oy++) {
    const uint32_t fy = uint32_t(oy * rows / outH);
    uint32_t* dst = &fb[size_t(oy) * outW];
    for (int ox = 0; ox < outW; ox++) {
      const uint32_t fx = uint32_t(ox * cols / outW);
      const uint32_t idx = fy * larg + fx;
      if (tipo == 2) {
        const uint32_t a = (origem + idx * 2) & (RDRAM_SIZE - 2);
        uint16_t v;
        std::memcpy(&v, &rdram[a ^ 2], 2);
        const uint32_t r = (v >> 11) & 31, g = (v >> 6) & 31, b = (v >> 1) & 31;
        dst[ox] = 0xFF000000u | ((r << 3) | (r >> 2)) | (((g << 3) | (g >> 2)) << 8) | (((b << 3) | (b >> 2)) << 16);
      } else {
        const uint32_t a = (origem + idx * 4) & (RDRAM_SIZE - 4);
        uint32_t v;
        std::memcpy(&v, &rdram[a], 4);
        dst[ox] = 0xFF000000u | (v >> 24) | (((v >> 16) & 0xFF) << 8) | (((v >> 8) & 0xFF) << 16);
      }
    }
  }
}

// --------------------------------------------------------------------------------------- tempo
void N64::events() {
  if (piDoneAt_ && cpu.cycles >= piDoneAt_) { piDoneAt_ = 0; raise(4); }
  if (siDoneAt_ && cpu.cycles >= siDoneAt_) { siDoneAt_ = 0; raise(1); }
}

void N64::runSlice(int64_t ate) {
  while (cpu.cycles < ate) {
    int64_t t = std::min(ate, cpu.cycles + SLICE);
    if (piDoneAt_ && piDoneAt_ < t) t = std::max(cpu.cycles + 1, piDoneAt_);
    if (siDoneAt_ && siDoneAt_ < t) t = std::max(cpu.cycles + 1, siDoneAt_);
    const int64_t t0 = cpu.cycles;
    cpu.run(t);
    rsp.run(cpu.cycles);  // RSP a 62,5 MHz: um tique
    aiAdvance(cpu.cycles - t0);
    events();
  }
}

void N64::runFrame() {
  const int halfLines = int(vi[6] & 0x3FF) >= 400 ? int(vi[6] & 0x3FF) : (pal ? 625 : 525);
  const int lines = (halfLines + 1) / 2;
  const int64_t porQuadro = int64_t(double(CPU_CLOCK) / frameRate());
  const int64_t inicio = cpu.cycles;
  for (int l = 0; l < lines; l++) {
    line_ = l;
    if (int((vi[3] & 0x3FF) >> 1) == l) raise(3);
    runSlice(inicio + porQuadro * (l + 1) / lines);
  }
  renderVi();
  if (vi[0] & 0x40) field_ ^= 1; else field_ = 0;
  frames++;
}

// --------------------------------------------------------------------------------------- save state
void N64::sync(StateIO& io) {
  cpu.sync(io);
  rsp.sync(io);
  rdp.sync(io);
  io.bytes(rdram);
  io.raw(spmem, sizeof spmem);
  io.raw(sp, sizeof sp); io.raw(dpc, sizeof dpc);
  io.sync(miMode); io.sync(miIntr); io.sync(miMask);
  io.raw(vi, sizeof vi); io.raw(ai, sizeof ai); io.raw(pi, sizeof pi); io.raw(ri, sizeof ri); io.raw(si, sizeof si);
  io.raw(pifRam, sizeof pifRam);
  io.bytes(eeprom); io.bytes(sram); io.bytes(flash); io.bytes(mempak);
  io.sync(eepromUsed); io.sync(sramUsed); io.sync(flashUsed); io.sync(mempakUsed);
  io.sync(line_); io.sync(field_);
  int64_t pd = piDoneAt_ ? piDoneAt_ - cpu.cycles : -1, sd = siDoneAt_ ? siDoneAt_ - cpu.cycles : -1;
  io.sync(pd); io.sync(sd);
  io.sync(pifPending_);
  for (auto& q : aiQ_) { io.sync(q.addr); io.sync(q.len); }
  io.sync(aiN_); io.sync(aiPos_); io.sync(aiFrac_); io.sync(aiL_); io.sync(aiR_);
  io.sync(flashMode_); io.sync(flashOffset_); io.sync(flashEraseOffset_); io.sync(flashStatus_);
  io.raw(flashPage_, sizeof flashPage_);
  io.sync(frames);
  if (io.reading()) {
    clampTo(line_, 0, 400); field_ &= 1;
    piDoneAt_ = pd < 0 ? 0 : cpu.cycles + std::clamp<int64_t>(pd, 1, 1 << 26);
    siDoneAt_ = sd < 0 ? 0 : cpu.cycles + std::clamp<int64_t>(sd, 1, 1 << 26);
    clampTo(aiN_, 0, 2);
    for (auto& q : aiQ_) { q.addr &= 0xFFFFF8; q.len &= 0x3FFF8; }
    if (aiN_ && aiPos_ > aiQ_[0].len) aiPos_ = 0;
    clampTo(aiFrac_, int64_t(0), int64_t(1) << 30);
    clampTo(flashMode_, 0, 3);
    flashOffset_ &= 0x1FF80;
    if (flashEraseOffset_ != 0xFFFFFFFFu) flashEraseOffset_ &= 0x1FFFF;
    sp[0] &= 0x1FF8; sp[1] &= 0xFFFFF8;
    for (int i = 0; i < 3; i++) dpc[i] &= 0xFFFFF8;
    // RSP: relógio perto do da CPU
    const int64_t alvo = cpu.cycles;
    if (rsp.cycles > alvo + 100000 || rsp.cycles < alvo - 100000) rsp.cycles = alvo;
    outFrac_ = 0;
    cpu.setIp(2, (miIntr & miMask) != 0);
  }
}

std::vector<uint8_t> N64::saveState() {
  StateIO io;
  char m[4];
  std::memcpy(m, STATE_MAGIC, 4);
  io.raw(m, 4);
  sync(io);
  return io.data();
}

void N64::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  char m[4];
  io.raw(m, 4);
  if (std::memcmp(m, STATE_MAGIC, 4) != 0) throw std::runtime_error("Estado salvo não é do Nintendo 64.");
  const std::vector<uint8_t> backup = saveState();
  try {
    sync(io);
    if (!io.atEnd()) throw std::runtime_error("Estado salvo incompatível.");
  } catch (...) {
    StateIO b(backup); char k[4]; b.raw(k, 4); sync(b);
    throw;
  }
}

// --------------------------------------------------------------------------------------- núcleo
N64Core::N64Core(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save) {
  const std::vector<uint8_t> z = toZ64(rom);
  if (z.empty()) throw std::runtime_error("Cartucho de Nintendo 64 inválido.");
  (void)nome;
  m_ = std::make_unique<N64>(z);
  if (save && !save->empty()) importSave(*save);
}

void N64Core::setKeys(uint32_t k) {
  uint16_t p = 0;
  if (k & KEY_A) p |= P_A;
  if (k & KEY_B) p |= P_B;
  if (k & KEY_L) p |= P_Z;
  if (k & KEY_R) p |= P_R;
  if (k & KEY_START) p |= P_START;
  if (k & KEY_SELECT) p |= P_L;
  if (k & KEY_X) p |= P_CU;
  if (k & KEY_Y) p |= P_CL;
  // teclado numérico: 8/2/4/6 = botões C, 5 = C para baixo também; 7/9/1/3 = direcional digital
  if (k & (KEY_KP0 << 8)) p |= P_CU;
  if (k & (KEY_KP0 << 2)) p |= P_CD;
  if (k & (KEY_KP0 << 5)) p |= P_CD;
  if (k & (KEY_KP0 << 4)) p |= P_CL;
  if (k & (KEY_KP0 << 6)) p |= P_CR;
  if (k & (KEY_KP0 << 7)) p |= P_DU;
  if (k & (KEY_KP0 << 1)) p |= P_DD;
  if (k & (KEY_KP0 << 9)) p |= P_DR;
  if (k & (KEY_KP0 << 3)) p |= P_DL;
  m_->pad[0] = p;
  // direcional = analógico no máximo (±80, como um controle de verdade)
  int x = 0, y = 0;
  if (k & KEY_LEFT) x -= 80;
  if (k & KEY_RIGHT) x += 80;
  if (k & KEY_UP) y += 80;
  if (k & KEY_DOWN) y -= 80;
  m_->stickX[0] = int8_t(x);
  m_->stickY[0] = int8_t(y);
}

std::vector<uint8_t> N64Core::exportSave() {
  N64& s = *m_;
  s.saveDirty = false;
  std::vector<uint8_t> out;
  const SaveType t = s.saveType;
  if (t == SaveType::Flash || (t == SaveType::Auto && s.flashUsed)) out = s.flash;
  else if (t == SaveType::Sram32k || t == SaveType::Sram96k || (t == SaveType::Auto && s.sramUsed)) out = s.sram;
  else if (t == SaveType::Eeprom4k || t == SaveType::Eeprom16k || (t == SaveType::Auto && s.eepromUsed)) out = s.eeprom;
  if (s.mempakUsed || out.empty()) out.insert(out.end(), s.mempak.begin(), s.mempak.end());
  return out;
}

void N64Core::importSave(const std::vector<uint8_t>& b) {
  N64& s = *m_;
  auto principal = [&](const uint8_t* p, size_t n) {
    if (n == 131072) { s.flash.assign(p, p + n); s.flashUsed = true; }
    else if (n == 98304 || n == 32768) {
      if (n == 98304 && s.saveType == SaveType::Auto) s.saveType = SaveType::Sram96k;
      s.sram.assign(p, p + n); s.sramUsed = true;
    } else if (n == 512 || n == 2048) {
      if (s.saveType == SaveType::Auto && n == 2048) s.saveType = SaveType::Eeprom16k;
      s.eeprom.assign(p, p + n); s.eepromUsed = true;
    }
  };
  const size_t n = b.size();
  const bool sramJogo = s.saveType == SaveType::Sram32k || s.saveType == SaveType::Sram96k;
  if (n == 32768 && !sramJogo) {
    // só o Controller Pak (ou SRAM num jogo sem tipo conhecido: vai para os dois)
    s.mempak.assign(b.begin(), b.end());
    s.mempakUsed = true;
    if (s.saveType == SaveType::Auto) s.sram.assign(b.begin(), b.end());
    return;
  }
  for (size_t p : {size_t(512), size_t(2048), size_t(32768), size_t(98304), size_t(131072)}) {
    if (n == p) { principal(b.data(), n); return; }
    if (n == p + 32768) {
      principal(b.data(), p);
      s.mempak.assign(b.begin() + long(p), b.end());
      s.mempakUsed = true;
      return;
    }
  }
}

std::string N64Core::note() const {
  return std::string("Nintendo 64 (experimental") + (m_->pal ? ", PAL 50 Hz" : "") +
         "): partida sem BIOS; direcional = analógico, A/B, L = Z, R = R, Select = L, X/Y = botões C (e o teclado "
         "numérico 8/2/4/6).";
}

}  // namespace indigo::n64
