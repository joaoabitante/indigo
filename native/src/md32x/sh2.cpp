#include "sh2.h"

#include <algorithm>
#include <cstring>

namespace indigo::md32x {

namespace {
constexpr uint32_t T = 1, S = 2, Q = 0x100, M = 0x200, SRMASK = 0x3F3;
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v); }
inline uint32_t sx8(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v))); }
inline uint32_t sx16(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }
// divisores dos temporizadores (em ciclos do SH-2)
constexpr int FRT_DIV[4] = {8, 32, 128, 0};
constexpr int WDT_DIV[8] = {2, 64, 128, 256, 512, 1024, 4096, 8192};
}  // namespace

Sh2::Sh2(Sh2Bus& b, int i) : id(i), bus(b) {}  // o dono chama powerOn() quando a memória estiver pronta

void Sh2::powerOn() {
  std::memset(r, 0, sizeof r);
  pr = gbr = mach = macl = 0;
  ccr_ = 0;
  std::memset(tag_, 0, sizeof tag_);
  std::memset(lru_, 0, sizeof lru_);
  std::memset(cacheData, 0, sizeof cacheData);
  dvsr_ = dvdnt_ = dvcr_ = vcrdiv_ = dvdnth_ = dvdntl_ = 0;
  for (auto& d : dma_) d = Dma{};
  dmaor_ = 0;
  tier_ = 1; ftcsr_ = 0; tcr_ = 0; tocr_ = 0xE0; temp_ = 0; frc_ = 0; ocra_ = ocrb_ = 0xFFFF; ficr_ = 0;
  wtcsr_ = 0x18; wtcnt_ = 0; rstcsr_ = 0x1F;
  ipra_ = iprb_ = vcra_ = vcrb_ = vcrc_ = vcrd_ = vcrwdt_ = icr_ = 0;
  smr_ = 0; brr_ = 0xFF; scr_ = 0; tdr_ = 0xFF; ssr_ = 0x84; rdr_ = 0;
  std::memset(misc_, 0, sizeof misc_);
  intLevel_ = intVector_ = 0;
  irl_ = 0;
  reset();
}

void Sh2::reset() {
  sr = 0xF0;
  vbr = 0;
  sleeping = false;
  inSlot_ = false;
  fbase_ = fend_ = 0;
  frtLast_ = wdtLast_ = clock;
  timerNext_ = INT64_MAX;
  pc = read32(0);
  r[15] = read32(4);
  irqCheck_ = true;
}

// ------------------------------------------------------------ memória
uint32_t Sh2::extRead(uint32_t a, int size) {
  a &= 0x1FFFFFFF;
  const unsigned blk = a >> 24;
  if (const uint8_t* p = rpage[blk]) {
    cyc_ += rwait[blk];
    const uint8_t* q = p + (a & pmask[blk]);
    return size == 1 ? *q : size == 2 ? be16(q) : be32(q);
  }
  return size == 1 ? bus.rd8(*this, a) : size == 2 ? bus.rd16(*this, a) : bus.rd32(*this, a);
}

void Sh2::extWrite(uint32_t a, int size, uint32_t v) {
  a &= 0x1FFFFFFF;
  const unsigned blk = a >> 24;
  if (uint8_t* p = wpage[blk]) {
    uint8_t* q = p + (a & pmask[blk]);
    if (size == 1) *q = uint8_t(v); else if (size == 2) put16(q, uint16_t(v)); else put32(q, v);
    return;
  }
  if (size == 1) bus.wr8(*this, a, uint8_t(v));
  else if (size == 2) bus.wr16(*this, a, uint16_t(v));
  else bus.wr32(*this, a, v);
}

// LRU de 6 bits do manual do SH7604: substitui a via 0 com 111xxx, 1 com 0xx11x, 2 com x0x0x1, 3 com xx0x00
void Sh2::lruTouch(int set, int way) {
  uint8_t& l = lru_[set];
  switch (way) {
    case 0: l &= 0x07; break;
    case 1: l = uint8_t((l & 0x19) | 0x20); break;
    case 2: l = uint8_t((l & 0x2A) | 0x14); break;
    default: l = uint8_t(l | 0x0B); break;
  }
}

int Sh2::lruVictim(int set) const {
  const uint8_t l = lru_[set];
  if (ccr_ & 8) return (l & 1) ? 2 : 3;  // duas vias: só as vias 2 e 3 são cache
  if ((l & 0x38) == 0x38) return 0;
  if ((l & 0x26) == 0x06) return 1;
  if ((l & 0x15) == 0x01) return 2;
  return 3;
}

int Sh2::cacheFind(uint32_t a) const {
  const int set = (a >> 4) & 63;
  const uint32_t t = (a & 0x1FFFFC00) | 1;
  const int w0 = (ccr_ & 8) ? 2 : 0;
  for (int w = w0; w < 4; w++) if (tag_[set][w] == t) return w;
  return -1;
}

uint32_t Sh2::cacheRead(uint32_t a, int size, bool code) {
  const int set = (a >> 4) & 63;
  if (set == fset_) { fbase_ = fend_ = 0; }
  int w = cacheFind(a);
  if (w < 0) {
    if (code ? (ccr_ & 2) : (ccr_ & 4)) return extRead(a, size);  // substituição desligada: lê direto
    w = lruVictim(set);
    uint8_t* line = &cacheData[w * 1024 + set * 16];
    const uint32_t base = a & ~15u;
    for (int k = 0; k < 16; k += 4) put32(line + k, extRead(base + k, 4));
    cyc_ += 4;
    tag_[set][w] = (a & 0x1FFFFC00) | 1;
    fbase_ = fend_ = 0;
  }
  lruTouch(set, w);
  const uint8_t* q = &cacheData[w * 1024 + set * 16 + (a & 15)];
  return size == 1 ? *q : size == 2 ? be16(q) : be32(q);
}

void Sh2::cacheWrite(uint32_t a, int size, uint32_t v) {
  if (int((a >> 4) & 63) == fset_) { fbase_ = fend_ = 0; }
  const int w = cacheFind(a);
  if (w >= 0) {
    uint8_t* q = &cacheData[w * 1024 + ((a >> 4) & 63) * 16 + (a & 15)];
    if (size == 1) *q = uint8_t(v); else if (size == 2) put16(q, uint16_t(v)); else put32(q, v);
    lruTouch((a >> 4) & 63, w);
  }
  extWrite(a, size, v);
}

uint8_t Sh2::read8(uint32_t a) {
  switch (a >> 29) {
    case 0: if (ccr_ & 1) return uint8_t(cacheRead(a, 1, false)); return uint8_t(extRead(a, 1));
    case 1: return uint8_t(extRead(a, 1));
    case 3: return 0;
    case 6: return cacheData[a & 0xFFF];
    case 7: if (a >= 0xFFFFFE00) return uint8_t(ioRead(a, 1)); return 0;
    case 2: return uint8_t(extRead(a, 1));
    default: return 0;  // 8xxxxxxx-Bxxxxxxx: sem nada
  }
}

uint16_t Sh2::read16(uint32_t a) {
  a &= ~1u;
  switch (a >> 29) {
    case 0: if (ccr_ & 1) return uint16_t(cacheRead(a, 2, false)); return uint16_t(extRead(a, 2));
    case 1: return uint16_t(extRead(a, 2));
    case 3: return 0;
    case 6: return be16(&cacheData[a & 0xFFE]);
    case 7: if (a >= 0xFFFFFE00) return uint16_t(ioRead(a, 2)); return 0;
    case 2: return uint16_t(extRead(a, 2));
    default: return 0;
  }
}

uint32_t Sh2::read32(uint32_t a) {
  a &= ~3u;
  switch (a >> 29) {
    case 0: if (ccr_ & 1) return cacheRead(a, 4, false); return extRead(a, 4);
    case 1: return extRead(a, 4);
    case 3: {  // matriz de endereços: etiqueta, LRU e válido da linha (via escolhida em CCR)
      const int set = (a >> 4) & 63, w = (ccr_ >> 6) & 3;
      return (tag_[set][w] & 0x1FFFFC00) | (uint32_t(lru_[set]) << 4) | ((tag_[set][w] & 1) << 2);
    }
    case 6: return be32(&cacheData[a & 0xFFC]);
    case 7: if (a >= 0xFFFFFE00) return ioRead(a, 4); return 0;
    case 2: return extRead(a, 4);
    default: return 0;
  }
}

void Sh2::write8(uint32_t a, uint8_t v) {
  switch (a >> 29) {
    case 0: if (ccr_ & 1) { cacheWrite(a, 1, v); return; } extWrite(a, 1, v); return;
    case 1: extWrite(a, 1, v); return;
    case 2: case 3: write32(a & ~3u, 0); return;
    case 6: cacheData[a & 0xFFF] = v; fbase_ = fend_ = 0; return;
    case 7: if (a >= 0xFFFFFE00) ioWrite(a, 1, v); return;
    default: return;
  }
}

void Sh2::write16(uint32_t a, uint16_t v) {
  a &= ~1u;
  switch (a >> 29) {
    case 0: if (ccr_ & 1) { cacheWrite(a, 2, v); return; } extWrite(a, 2, v); return;
    case 1: extWrite(a, 2, v); return;
    case 2: case 3: write32(a & ~3u, 0); return;
    case 6: put16(&cacheData[a & 0xFFE], v); fbase_ = fend_ = 0; return;
    case 7: if (a >= 0xFFFFFE00) ioWrite(a, 2, v); return;
    default: return;
  }
}

void Sh2::write32(uint32_t a, uint32_t v) {
  a &= ~3u;
  switch (a >> 29) {
    case 0: if (ccr_ & 1) { cacheWrite(a, 4, v); return; } extWrite(a, 4, v); return;
    case 1: extWrite(a, 4, v); return;
    case 2: {  // descarte associativo: invalida a linha com esse endereço
      const int w = cacheFind(a);
      if (w >= 0) tag_[(a >> 4) & 63][w] &= ~1u;
      fbase_ = fend_ = 0;
      return;
    }
    case 3: {  // matriz de endereços: grava etiqueta e válido na via escolhida em CCR
      const int set = (a >> 4) & 63, w = (ccr_ >> 6) & 3;
      tag_[set][w] = (v & 0x1FFFFC00) | ((v >> 2) & 1);
      lru_[set] = uint8_t((v >> 4) & 0x3F);
      fbase_ = fend_ = 0;
      return;
    }
    case 6: put32(&cacheData[a & 0xFFC], v); fbase_ = fend_ = 0; return;
    case 7: if (a >= 0xFFFFFE00) ioWrite(a, 4, v); return;
    default: return;
  }
}

// busca de instrução: atalho para a página direta (área sem cache) ou para a linha da cache já tocada
uint16_t Sh2::fetch(uint32_t a) {
  if (a - fbase_ < fend_ - fbase_ && fgen_ == mapGen_) return be16(fptr_ + (a - fbase_));
  const unsigned area = a >> 29;
  if (area == 1 || (area == 0 && !(ccr_ & 1))) {
    const uint32_t pa = a & 0x1FFFFFFF;
    const unsigned blk = pa >> 24;
    if (const uint8_t* p = rpage[blk]) {
      const uint32_t off = pa & pmask[blk];
      fptr_ = p;
      fbase_ = a - off;
      fend_ = fbase_ + pmask[blk] + 1;
      fset_ = -1;
      fgen_ = mapGen_;
      return be16(p + off);
    }
  }
  if (area == 0 && (ccr_ & 1)) {
    const uint16_t v = uint16_t(cacheRead(a, 2, true));
    const int w = cacheFind(a);
    if (w >= 0) {  // a linha está na cache: as próximas buscas nela não mudam o LRU
      fptr_ = &cacheData[w * 1024 + ((a >> 4) & 63) * 16];
      fbase_ = a & ~15u;
      fend_ = fbase_ + 16;
      fset_ = (a >> 4) & 63;
      fgen_ = mapGen_;
    }
    return v;
  }
  if (area == 6) return be16(&cacheData[a & 0xFFE]);
  return read16(a);
}

// ------------------------------------------------------------ exceções e interrupções
void Sh2::exception(int vec, uint32_t savePc) {
  r[15] -= 4;
  write32(r[15], sr & SRMASK);
  r[15] -= 4;
  write32(r[15], savePc);
  npc_ = read32(vbr + uint32_t(vec) * 4);
}

void Sh2::illegal() {
  // no slot de um desvio vira "instrução ilegal no slot" (vetor 6), com o endereço do desvio
  exception(inSlot_ ? 6 : 4, pc);
  cyc_ += 5;
}

void Sh2::updateInternalIrq() {
  int lvl = 0, vec = 0;
  auto cand = [&](int l, int v) { if (l > lvl) { lvl = l; vec = v & 0x7F; } };
  if ((dvcr_ & 3) == 3) cand(ipra_ >> 12, int(vcrdiv_));
  for (int c = 0; c < 2; c++)
    if ((dma_[c].chcr & 6) == 6) cand((ipra_ >> 8) & 15, int(dma_[c].vcr));
  if ((wtcsr_ & 0xC0) == 0x80) cand((ipra_ >> 4) & 15, vcrwdt_ >> 8);
  {
    const int l = iprb_ >> 12;
    if ((ssr_ & 0x38) && (scr_ & 0x40)) cand(l, vcra_ >> 8);           // ERI
    if ((ssr_ & 0x40) && (scr_ & 0x40)) cand(l, vcra_);                // RXI
    if ((ssr_ & 0x80) && (scr_ & 0x80)) cand(l, vcrb_ >> 8);           // TXI
    if ((ssr_ & 0x04) && (scr_ & 0x04)) cand(l, vcrb_);                // TEI
  }
  {
    const int l = (iprb_ >> 8) & 15;
    if ((ftcsr_ & 0x80) && (tier_ & 0x80)) cand(l, vcrc_ >> 8);        // ICI
    if ((ftcsr_ & tier_ & 0x0C)) cand(l, vcrc_);                       // OCIA/OCIB
    if ((ftcsr_ & 0x02) && (tier_ & 0x02)) cand(l, vcrd_ >> 8);        // OVI
  }
  if (lvl != intLevel_ || vec != intVector_) { intLevel_ = lvl; intVector_ = vec; }
  irqCheck_ = true;
}

void Sh2::checkIrq() {
  irqCheck_ = false;
  const int mask = (sr >> 4) & 15;
  const int lvl = std::max(irl_, intLevel_);
  if (lvl <= mask) return;
  sleeping = false;
  npc_ = pc;
  if (irl_ >= intLevel_) {
    const int lv = irl_;  // o barramento pode baixar a IRL ao dar o vetor
    const int ev = bus.irlVector(*this, lv);
    exception(ev >= 0 ? ev : 64 + lv / 2, pc);
    bus.irlAck(*this, lv);
    sr = (sr & ~0xF0u) | (uint32_t(lv) << 4);
  } else {
    exception(intVector_, pc);
    sr = (sr & ~0xF0u) | (uint32_t(intLevel_) << 4);
  }
  pc = npc_;
  clock += 13;
  irqCheck_ = true;
}

bool Sh2::isBranch(uint16_t op) const {
  switch (op >> 12) {
    case 0x0: return (op & 0xF0FF) == 0x0003 || (op & 0xF0FF) == 0x0023 || op == 0x000B || op == 0x002B;
    case 0x4: return (op & 0xF0FF) == 0x400B || (op & 0xF0FF) == 0x402B;
    case 0x8: { const int k = (op >> 8) & 0xF; return k == 9 || k == 0xB || k == 0xD || k == 0xF; }
    case 0xA: case 0xB: return true;
    case 0xC: return ((op >> 8) & 0xF) == 3;  // TRAPA
    default: return false;
  }
}

// desvio atrasado: executa a instrução do slot e vai para o destino
void Sh2::delayed(uint32_t target) {
  const uint32_t slotPc = pc + 2;
  const uint16_t op = fetch(slotPc);
  if (isBranch(op)) {
    // instrução ilegal no slot: empilha o endereço do desvio
    exception(6, pc);
    cyc_ += 5;
    return;
  }
  npc_ = target;
  inSlot_ = true;
  exec(op, target + 2);
  inSlot_ = false;
  cyc_ += 1;
}

// ------------------------------------------------------------ execução
int Sh2::step() {
  cyc_ = 1;
  const uint16_t op = fetch(pc);
  npc_ = pc + 2;
  exec(op, pc + 4);
  pc = npc_;
  clock += cyc_;
  return cyc_;
}

void Sh2::run(int64_t until) {
  irqCheck_ = true;  // SR pode ter sido mudado de fora (estado carregado, testes)
  while (clock < until) {
    if (clock >= timerNext_) timersRun();
    if (irqCheck_) checkIrq();
    if (sleeping) { clock = std::min(until, std::max(clock, timerNext_)); continue; }
    cyc_ = 1;
    const uint16_t op = fetch(pc);
    npc_ = pc + 2;
    exec(op, pc + 4);
    pc = npc_;
    clock += cyc_;
    if (stop) { stop = false; break; }
  }
}

// temporizadores: atualizados só quando chega a hora do próximo evento (comparação, estouro) ou num acesso
void Sh2::timersRun() {
  if ((tcr_ & 3) != 3) frtRun();
  if (wtcsr_ & 0x20) wdtRun();
  timersSchedule();
}

void Sh2::timersSchedule() {
  int64_t next = INT64_MAX;
  if ((tcr_ & 3) != 3 && (tier_ & 0x8E)) {
    uint32_t toA = uint16_t(ocra_ - frc_), toB = uint16_t(ocrb_ - frc_);
    if (!toA) toA = 0x10000;
    if (!toB) toB = 0x10000;
    const uint32_t ticks = std::min<uint32_t>({toA, toB, 0x10000u - frc_});
    next = frtLast_ + int64_t(ticks) * FRT_DIV[tcr_ & 3];
  }
  if (wtcsr_ & 0x20) next = std::min(next, wdtLast_ + int64_t(256 - wtcnt_) * WDT_DIV[wtcsr_ & 7]);
  timerNext_ = next;
}

void Sh2::exec(uint16_t op, uint32_t pcv) {
  const int n = (op >> 8) & 15, m = (op >> 4) & 15;
  uint32_t& Rn = r[n];
  const uint32_t Rm = r[m];
  auto setT = [&](bool b) { sr = (sr & ~T) | (b ? T : 0); };
  switch (op >> 12) {
    case 0x0:
      switch (op & 0xF) {
        case 0x2:
          switch (m) {
            case 0: Rn = sr & SRMASK; return;
            case 1: Rn = gbr; return;
            case 2: Rn = vbr; return;
          }
          break;
        case 0x3:
          if (m == 0) { pr = pcv; cyc_ += 1; delayed(pcv + r[n]); return; }  // BSRF
          if (m == 2) { cyc_ += 1; delayed(pcv + r[n]); return; }            // BRAF
          break;
        case 0x4: write8(Rn + r[0], uint8_t(Rm)); return;
        case 0x5: write16(Rn + r[0], uint16_t(Rm)); return;
        case 0x6: write32(Rn + r[0], Rm); return;
        case 0x7: macl = Rn * Rm; cyc_ += 1; return;
        case 0x8:
          if (n) break;
          if (m == 0) { sr &= ~T; return; }
          if (m == 1) { sr |= T; return; }
          if (m == 2) { mach = macl = 0; return; }
          break;
        case 0x9:
          if (m == 0 && n == 0) return;                          // NOP
          if (m == 1 && n == 0) { sr &= ~(M | Q | T); return; }   // DIV0U
          if (m == 2) { Rn = sr & T; return; }                    // MOVT
          break;
        case 0xA:
          if (m == 0) { Rn = mach; return; }
          if (m == 1) { Rn = macl; return; }
          if (m == 2) { Rn = pr; return; }
          break;
        case 0xB:
          if (n) break;
          if (m == 0) { cyc_ += 1; delayed(pr); return; }  // RTS
          if (m == 1) { sleeping = true; cyc_ += 2; return; }  // SLEEP
          if (m == 2) {                                          // RTE
            const uint32_t t = read32(r[15]);
            r[15] += 4;
            sr = read32(r[15]) & SRMASK;
            r[15] += 4;
            cyc_ += 3;
            irqCheck_ = true;
            delayed(t);
            return;
          }
          break;
        case 0xC: Rn = sx8(read8(Rm + r[0])); return;
        case 0xD: Rn = sx16(read16(Rm + r[0])); return;
        case 0xE: Rn = read32(Rm + r[0]); return;
        case 0xF: {  // MAC.L @Rm+,@Rn+
          const int32_t vn = int32_t(read32(r[n])); r[n] += 4;
          const int32_t vm = int32_t(read32(r[m])); r[m] += 4;
          const uint64_t prod = uint64_t(int64_t(vn) * int64_t(vm));
          if (sr & S) {
            // saturação em 48 bits, exatamente como o código C do manual (e a referência): só os 16 bits de
            // baixo de MACH entram na soma
            const uint32_t lo = macl + uint32_t(prod);
            uint32_t hi = uint32_t(prod >> 32) + (lo < macl ? 1 : 0) + (mach & 0xFFFF);
            uint32_t l2 = lo;
            if (int32_t(hi) < 0 && hi < 0xFFFF8000u) { hi = 0x8000; l2 = 0; }
            else if (int32_t(hi) > 0 && hi > 0x7FFF) { hi = 0x7FFF; l2 = 0xFFFFFFFF; }
            mach = hi; macl = l2;
          } else {
            const uint64_t res = ((uint64_t(mach) << 32) | macl) + prod;
            mach = uint32_t(res >> 32); macl = uint32_t(res);
          }
          cyc_ += 2;
          return;
        }
      }
      illegal();
      return;
    case 0x1: write32(Rn + (op & 15) * 4, Rm); return;
    case 0x2:
      switch (op & 0xF) {
        case 0x0: write8(Rn, uint8_t(Rm)); return;
        case 0x1: write16(Rn, uint16_t(Rm)); return;
        case 0x2: write32(Rn, Rm); return;
        case 0x4: write8(Rn - 1, uint8_t(Rm)); Rn -= 1; return;
        case 0x5: write16(Rn - 2, uint16_t(Rm)); Rn -= 2; return;
        case 0x6: write32(Rn - 4, Rm); Rn -= 4; return;
        case 0x7:  // DIV0S
          sr &= ~(M | Q | T);
          if (Rn & 0x80000000u) sr |= Q;
          if (Rm & 0x80000000u) sr |= M;
          if ((Rn ^ Rm) & 0x80000000u) sr |= T;
          return;
        case 0x8: setT((Rn & Rm) == 0); return;
        case 0x9: Rn &= Rm; return;
        case 0xA: Rn ^= Rm; return;
        case 0xB: Rn |= Rm; return;
        case 0xC: {
          const uint32_t x = Rn ^ Rm;
          setT(!(x & 0xFF000000u) || !(x & 0xFF0000u) || !(x & 0xFF00u) || !(x & 0xFFu));
          return;
        }
        case 0xD: Rn = (Rn >> 16) | (Rm << 16); return;
        case 0xE: macl = uint32_t(uint16_t(Rn)) * uint16_t(Rm); return;
        case 0xF: macl = uint32_t(int32_t(int16_t(Rn)) * int32_t(int16_t(Rm))); return;
      }
      illegal();
      return;
    case 0x3:
      switch (op & 0xF) {
        case 0x0: setT(Rn == Rm); return;
        case 0x2: setT(Rn >= Rm); return;
        case 0x3: setT(int32_t(Rn) >= int32_t(Rm)); return;
        case 0x4: {  // DIV1: um passo da divisão sem restauração (tabela do manual)
          const bool oldQ = sr & Q, mm = sr & M;
          const bool q = (Rn & 0x80000000u) != 0;
          const uint32_t sh = (Rn << 1) | (sr & T);
          bool carry;
          if (oldQ == mm) { Rn = sh - Rm; carry = Rn > sh; }
          else { Rn = sh + Rm; carry = Rn < sh; }
          const bool nq = q ^ carry ^ mm;
          sr = (sr & ~(Q | T)) | (nq ? Q : 0) | (nq == mm ? T : 0);
          return;
        }
        case 0x5: {  // DMULU.L
          const uint64_t res = uint64_t(Rn) * Rm;
          mach = uint32_t(res >> 32); macl = uint32_t(res);
          cyc_ += 1;
          return;
        }
        case 0x6: setT(Rn > Rm); return;
        case 0x7: setT(int32_t(Rn) > int32_t(Rm)); return;
        case 0x8: Rn -= Rm; return;
        case 0xA: {  // SUBC
          const uint32_t t = sr & T, a = Rn;
          const uint32_t tmp = a - Rm;
          Rn = tmp - t;
          setT(a < tmp || tmp < Rn);
          return;
        }
        case 0xB: {  // SUBV
          const uint32_t res = Rn - Rm;
          setT((((Rn ^ Rm) & (Rn ^ res)) >> 31) != 0);
          Rn = res;
          return;
        }
        case 0xC: Rn += Rm; return;
        case 0xD: {  // DMULS.L
          const int64_t res = int64_t(int32_t(Rn)) * int32_t(Rm);
          mach = uint32_t(uint64_t(res) >> 32); macl = uint32_t(res);
          cyc_ += 1;
          return;
        }
        case 0xE: {  // ADDC
          const uint32_t t = sr & T, a = Rn;
          const uint32_t tmp = a + Rm;
          Rn = tmp + t;
          setT(a > tmp || tmp > Rn);
          return;
        }
        case 0xF: {  // ADDV
          const uint32_t res = Rn + Rm;
          setT(((~(Rn ^ Rm) & (Rn ^ res)) >> 31) != 0);
          Rn = res;
          return;
        }
      }
      illegal();
      return;
    case 0x4:
      switch (op & 0xFF) {
        case 0x00: case 0x20: setT(Rn >> 31); Rn <<= 1; return;            // SHLL / SHAL
        case 0x10: Rn -= 1; setT(Rn == 0); return;                        // DT
        case 0x01: setT(Rn & 1); Rn >>= 1; return;                        // SHLR
        case 0x11: setT(int32_t(Rn) >= 0); return;                        // CMP/PZ
        case 0x21: setT(Rn & 1); Rn = uint32_t(int32_t(Rn) >> 1); return; // SHAR
        case 0x02: write32(Rn - 4, mach); Rn -= 4; return;
        case 0x12: write32(Rn - 4, macl); Rn -= 4; return;
        case 0x22: write32(Rn - 4, pr); Rn -= 4; return;
        case 0x03: write32(Rn - 4, sr & SRMASK); Rn -= 4; cyc_ += 1; return;
        case 0x13: write32(Rn - 4, gbr); Rn -= 4; cyc_ += 1; return;
        case 0x23: write32(Rn - 4, vbr); Rn -= 4; cyc_ += 1; return;
        case 0x04: setT(Rn >> 31); Rn = (Rn << 1) | (Rn >> 31); return;   // ROTL
        case 0x24: { const uint32_t t = sr & T; setT(Rn >> 31); Rn = (Rn << 1) | t; return; }  // ROTCL
        case 0x05: setT(Rn & 1); Rn = (Rn >> 1) | (Rn << 31); return;     // ROTR
        case 0x15: setT(int32_t(Rn) > 0); return;                         // CMP/PL
        case 0x25: { const uint32_t t = sr & T; setT(Rn & 1); Rn = (Rn >> 1) | (t << 31); return; }  // ROTCR
        case 0x06: mach = read32(Rn); Rn += 4; return;
        case 0x16: macl = read32(Rn); Rn += 4; return;
        case 0x26: pr = read32(Rn); Rn += 4; return;
        case 0x07: sr = read32(Rn) & SRMASK; Rn += 4; cyc_ += 2; irqCheck_ = true; return;
        case 0x17: gbr = read32(Rn); Rn += 4; cyc_ += 2; return;
        case 0x27: vbr = read32(Rn); Rn += 4; cyc_ += 2; return;
        case 0x08: Rn <<= 2; return;
        case 0x18: Rn <<= 8; return;
        case 0x28: Rn <<= 16; return;
        case 0x09: Rn >>= 2; return;
        case 0x19: Rn >>= 8; return;
        case 0x29: Rn >>= 16; return;
        case 0x0A: mach = Rn; return;
        case 0x1A: macl = Rn; return;
        case 0x2A: pr = Rn; return;
        case 0x0B: { const uint32_t t = Rn; pr = pcv; cyc_ += 1; delayed(t); return; }  // JSR
        case 0x1B: {                                                                     // TAS.B
          const uint32_t a = Rn;
          const uint8_t v = read8(a);
          setT(v == 0);
          write8(a, uint8_t(v | 0x80));
          cyc_ += 3;
          return;
        }
        case 0x2B: cyc_ += 1; delayed(Rn); return;  // JMP
        case 0x0E: sr = Rn & SRMASK; irqCheck_ = true; return;
        case 0x1E: gbr = Rn; return;
        case 0x2E: vbr = Rn; return;
        default:
          if ((op & 0xF) == 0xF) {  // MAC.W @Rm+,@Rn+
            const int32_t vn = int32_t(int16_t(read16(r[n]))); r[n] += 2;
            const int32_t vm = int32_t(int16_t(read16(r[m]))); r[m] += 2;
            const int32_t prod = vn * vm;
            if (sr & S) {
              const int64_t res = int64_t(int32_t(macl)) + prod;
              if (res > 0x7FFFFFFF) macl = 0x7FFFFFFF;
              else if (res < -0x7FFFFFFFLL - 1) macl = 0x80000000u;
              else macl = uint32_t(res);
            } else {
              const uint64_t res = ((uint64_t(mach) << 32) | macl) + uint64_t(int64_t(prod));
              mach = uint32_t(res >> 32); macl = uint32_t(res);
            }
            cyc_ += 2;
            return;
          }
      }
      illegal();
      return;
    case 0x5: Rn = read32(Rm + (op & 15) * 4); return;
    case 0x6:
      switch (op & 0xF) {
        case 0x0: Rn = sx8(read8(Rm)); return;
        case 0x1: Rn = sx16(read16(Rm)); return;
        case 0x2: Rn = read32(Rm); return;
        case 0x3: Rn = Rm; return;
        case 0x4: { const uint32_t v = sx8(read8(Rm)); if (n != m) r[m] += 1; Rn = v; return; }
        case 0x5: { const uint32_t v = sx16(read16(Rm)); if (n != m) r[m] += 2; Rn = v; return; }
        case 0x6: { const uint32_t v = read32(Rm); if (n != m) r[m] += 4; Rn = v; return; }
        case 0x7: Rn = ~Rm; return;
        case 0x8: Rn = (Rm & 0xFFFF0000u) | ((Rm & 0xFF) << 8) | ((Rm >> 8) & 0xFF); return;
        case 0x9: Rn = (Rm << 16) | (Rm >> 16); return;
        case 0xA: {  // NEGC
          const uint32_t t = sr & T;
          Rn = 0u - Rm - t;
          setT(Rm != 0 || t);
          return;
        }
        case 0xB: Rn = 0u - Rm; return;
        case 0xC: Rn = Rm & 0xFF; return;
        case 0xD: Rn = Rm & 0xFFFF; return;
        case 0xE: Rn = sx8(Rm); return;
        case 0xF: Rn = sx16(Rm); return;
      }
      return;
    case 0x7: Rn += sx8(op); return;
    case 0x8: {
      const uint32_t d4 = op & 15;
      switch (n) {
        case 0x0: write8(r[m] + d4, uint8_t(r[0])); return;
        case 0x1: write16(r[m] + d4 * 2, uint16_t(r[0])); return;
        case 0x4: r[0] = sx8(read8(r[m] + d4)); return;
        case 0x5: r[0] = sx16(read16(r[m] + d4 * 2)); return;
        case 0x8: setT(r[0] == sx8(op)); return;
        case 0x9: if (sr & T) { npc_ = pcv + sx8(op) * 2; cyc_ += 2; } return;    // BT
        case 0xB: if (!(sr & T)) { npc_ = pcv + sx8(op) * 2; cyc_ += 2; } return; // BF
        case 0xD: if (sr & T) { cyc_ += 1; delayed(pcv + sx8(op) * 2); } return;   // BT/S
        case 0xF: if (!(sr & T)) { cyc_ += 1; delayed(pcv + sx8(op) * 2); } return; // BF/S
      }
      illegal();
      return;
    }
    case 0x9: Rn = sx16(read16(pcv + (op & 0xFF) * 2)); return;
    case 0xA: cyc_ += 1; delayed(pcv + (static_cast<int32_t>(static_cast<uint32_t>(op) << 20) >> 19)); return;
    case 0xB: pr = pcv; cyc_ += 1; delayed(pcv + (static_cast<int32_t>(static_cast<uint32_t>(op) << 20) >> 19)); return;
    case 0xC: {
      const uint32_t d = op & 0xFF;
      switch (n) {
        case 0x0: write8(gbr + d, uint8_t(r[0])); return;
        case 0x1: write16(gbr + d * 2, uint16_t(r[0])); return;
        case 0x2: write32(gbr + d * 4, r[0]); return;
        case 0x3:  // TRAPA
          if (inSlot_) return;
          exception(int(d), pcv - 2);
          cyc_ += 7;
          return;
        case 0x4: r[0] = sx8(read8(gbr + d)); return;
        case 0x5: r[0] = sx16(read16(gbr + d * 2)); return;
        case 0x6: r[0] = read32(gbr + d * 4); return;
        case 0x7: r[0] = (pcv & ~3u) + d * 4; return;  // MOVA
        case 0x8: setT((r[0] & d) == 0); return;
        case 0x9: r[0] &= d; return;
        case 0xA: r[0] ^= d; return;
        case 0xB: r[0] |= d; return;
        case 0xC: setT((read8(gbr + r[0]) & d) == 0); cyc_ += 2; return;
        case 0xD: { const uint32_t a = gbr + r[0]; write8(a, uint8_t(read8(a) & d)); cyc_ += 2; return; }
        case 0xE: { const uint32_t a = gbr + r[0]; write8(a, uint8_t(read8(a) ^ d)); cyc_ += 2; return; }
        case 0xF: { const uint32_t a = gbr + r[0]; write8(a, uint8_t(read8(a) | d)); cyc_ += 2; return; }
      }
      return;
    }
    case 0xD: Rn = read32((pcv & ~3u) + (op & 0xFF) * 4); return;
    case 0xE: Rn = sx8(op); return;
    default: illegal(); return;
  }
}

// ------------------------------------------------------------ periféricos internos
void Sh2::inputCapture() {
  frtRun();
  ficr_ = frc_;
  ftcsr_ |= 0x80;
  updateInternalIrq();
}

void Sh2::frtRun() {
  const int div = FRT_DIV[tcr_ & 3];
  if (!div) { frtLast_ = clock; return; }
  int64_t ticks = (clock - frtLast_) / div;
  if (ticks <= 0) return;
  frtLast_ += ticks * div;
  const uint8_t before = ftcsr_;
  while (ticks > 0) {
    // próximo evento: comparação A/B ou estouro
    uint32_t toA = uint16_t(ocra_ - frc_), toB = uint16_t(ocrb_ - frc_), toOv = 0x10000u - frc_;
    if (!toA) toA = 0x10000;
    if (!toB) toB = 0x10000;
    const uint32_t step = std::min<uint32_t>({toA, toB, toOv});
    if (ticks < int64_t(step)) { frc_ = uint16_t(frc_ + ticks); break; }
    ticks -= step;
    frc_ = uint16_t(frc_ + step);
    if (frc_ == ocra_) { ftcsr_ |= 0x08; if (ftcsr_ & 1) frc_ = 0; }
    if (frc_ == ocrb_) ftcsr_ |= 0x04;
    if (frc_ == 0 && step == toOv) ftcsr_ |= 0x02;
  }
  if (ftcsr_ != before) updateInternalIrq();
}

void Sh2::wdtRun() {
  const int div = WDT_DIV[wtcsr_ & 7];
  const int64_t ticks = (clock - wdtLast_) / div;
  if (ticks <= 0) return;
  wdtLast_ += ticks * div;
  const int64_t total = int64_t(wtcnt_) + ticks;
  wtcnt_ = uint8_t(total);
  if (total >= 256) {
    if (wtcsr_ & 0x40) {  // modo cão de guarda
      rstcsr_ |= 0x80;
      if (rstcsr_ & 0x40) { wtcsr_ &= ~0x20; reset(); }
    } else if (!(wtcsr_ & 0x80)) {
      wtcsr_ |= 0x80;
      updateInternalIrq();
    }
  }
}

void Sh2::divide32() {
  dvdnt_ = dvdntl_;
  if (dvsr_ == 0 || (dvdntl_ == 0x80000000u && dvsr_ == 0xFFFFFFFFu)) {
    // estouro: quociente saturado pelo sinal, resto sem sentido (fica o dividendo)
    dvcr_ |= 1;
    dvdnth_ = dvdntl_;
    dvdntl_ = (int32_t(dvdntl_) < 0) ? 0x80000000u : 0x7FFFFFFFu;
    if (dvsr_ == 0xFFFFFFFFu) dvdntl_ = 0x80000000u, dvdnth_ = 0;
  } else {
    const int32_t a = int32_t(dvdntl_), b = int32_t(dvsr_);
    dvdnth_ = uint32_t(a % b);
    dvdntl_ = uint32_t(a / b);
  }
  dvdnt_ = dvdntl_;
  cyc_ += 39;
  if (dvcr_ & 1) updateInternalIrq();
}

void Sh2::divide64() {
  const int64_t a = int64_t((uint64_t(dvdnth_) << 32) | dvdntl_);
  const int32_t b = int32_t(dvsr_);
  bool ovf = b == 0;
  int64_t q = 0, rem = 0;
  if (!ovf) {
    if (a == INT64_MIN && b == -1) ovf = true;
    else {
      q = a / b; rem = a % b;
      if (q > 0x7FFFFFFF || q < -0x80000000LL) ovf = true;
    }
  }
  if (ovf) {
    dvcr_ |= 1;
    const bool neg = (a < 0) != (b < 0);
    dvdntl_ = neg ? 0x80000000u : 0x7FFFFFFFu;
  } else {
    dvdnth_ = uint32_t(rem);
    dvdntl_ = uint32_t(q);
  }
  dvdnt_ = dvdntl_;
  cyc_ += 39;
  if (dvcr_ & 1) updateInternalIrq();
}

bool Sh2::dmaActive(int ch) const {
  const Dma& d = dma_[ch];
  return (dmaor_ & 7) == 1 && (d.chcr & 3) == 1;
}

void Sh2::dmaUnit(int ch) {
  Dma& d = dma_[ch];
  const int ts = (d.chcr >> 10) & 3;
  const int sz = ts == 0 ? 1 : ts == 1 ? 2 : 4;
  auto rd = [&](uint32_t a) -> uint32_t {
    if ((a >> 29) <= 1) return extRead(a, sz);
    return sz == 1 ? read8(a) : sz == 2 ? read16(a) : read32(a);
  };
  auto wr = [&](uint32_t a, uint32_t v) {
    if ((a >> 29) <= 1) extWrite(a, sz, v);
    else if (sz == 1) write8(a, uint8_t(v)); else if (sz == 2) write16(a, uint16_t(v)); else write32(a, v);
  };
  const int dm = (d.chcr >> 14) & 3, sm = (d.chcr >> 12) & 3;
  if (ts == 3) {  // 16 bytes: quatro palavras longas, conta 4 em TCR
    for (int k = 0; k < 4; k++) {
      wr(d.dar + (dm ? k * 4 : 0), rd(d.sar + (sm ? k * 4 : 0)));
    }
    if (sm == 1) d.sar += 16; else if (sm == 2) d.sar -= 16;
    if (dm == 1) d.dar += 16; else if (dm == 2) d.dar -= 16;
    d.tcr = (d.tcr - 4) & 0xFFFFFF;
    if (d.tcr > 0xFFFFF0) d.tcr = 0;
    clock += 8;
    return;
  }
  wr(d.dar, rd(d.sar));
  if (sm == 1) d.sar += sz; else if (sm == 2) d.sar -= sz;
  if (dm == 1) d.dar += sz; else if (dm == 2) d.dar -= sz;
  d.tcr = (d.tcr - 1) & 0xFFFFFF;
  clock += 2;
}

void Sh2::dmaDone(int ch) {
  dma_[ch].chcr |= 2;  // TE
  if (dma_[ch].chcr & 4) updateInternalIrq();
}

void Sh2::dmaStart(int ch) {
  if (!dmaActive(ch)) return;
  Dma& d = dma_[ch];
  if (d.chcr & 0x200) {  // pedido automático: transfere tudo de uma vez
    uint32_t n = d.tcr ? d.tcr : 0x1000000;
    if (((d.chcr >> 10) & 3) == 3) n = (n + 3) / 4;
    d.tcr = d.tcr ? d.tcr : 0x1000000;
    for (uint32_t k = 0; k < n && d.tcr; k++) dmaUnit(ch);
    d.tcr = 0;
    dmaDone(ch);
    return;
  }
  bus.dmaKick(*this, ch);
}

bool Sh2::dreq(int ch, int maxUnits) {
  if (!dmaActive(ch) || (dma_[ch].chcr & 0x200)) return false;
  Dma& d = dma_[ch];
  if (!d.tcr) d.tcr = 0x1000000;
  for (int k = 0; k < maxUnits && d.tcr; k++) dmaUnit(ch);
  if (!d.tcr) dmaDone(ch);
  return true;
}

void Sh2::sciSend() {
  // a serial dos dois SH-2 do 32X é ligada uma na outra: o byte vai direto para o outro
  if (!(scr_ & 0x20) || (ssr_ & 0x80) || !other) return;
  Sh2& o = *other;
  if (o.scr_ & 0x10) {
    o.rdr_ = tdr_;
    if (o.ssr_ & 0x40) o.ssr_ |= 0x20;  // ORER: o anterior não foi lido
    o.ssr_ |= 0x40;
    o.updateInternalIrq();
  }
  ssr_ |= 0x84;  // TDRE e TEND
  updateInternalIrq();
}

uint32_t Sh2::ioRead(uint32_t a, int size) {
  const uint32_t o = a & 0x1FF;
  const bool divu = o >= 0x100 && o < 0x140, dmac = o >= 0x180 && o < 0x1C0;
  if (size == 4) {
    if (divu) {
      switch (o & 0x1C) {
        case 0x00: return dvsr_;
        case 0x04: return dvdnt_;
        case 0x08: return dvcr_;
        case 0x0C: return vcrdiv_;
        case 0x10: case 0x18: return dvdnth_;
        default: return dvdntl_;
      }
    }
    if (dmac) {
      const int ch = (o >> 4) & 1;
      switch (o) {
        case 0x180: case 0x190: return dma_[ch].sar;
        case 0x184: case 0x194: return dma_[ch].dar;
        case 0x188: case 0x198: return dma_[ch].tcr;
        case 0x18C: case 0x19C: return dma_[ch].chcr;
        case 0x1A0: return dma_[0].vcr;
        case 0x1A8: return dma_[1].vcr;
        case 0x1B0: return dmaor_;
        default: return 0;
      }
    }
    if (o >= 0x100) {
      uint32_t v = 0;
      for (int k = 0; k < 4; k++) v = (v << 8) | misc_[(o & ~3u) + k];
      if ((o & ~3u) == 0x1E0 && slaveMode) v |= 0x8000;  // BCR1.MASTER: 1 = escravo
      return v;
    }
    return (ioRead(a & ~3u, 2) << 16) | ioRead((a & ~3u) + 2, 2);
  }
  if (size == 2) {
    switch (o & ~1u) {
      case 0x60: return iprb_;
      case 0x62: return vcra_;
      case 0x64: return vcrb_;
      case 0x66: return vcrc_;
      case 0x68: return vcrd_;
      case 0xE0: return icr_;
      case 0xE2: return ipra_;
      case 0xE4: return vcrwdt_;
      case 0x80: wdtRun(); return uint32_t(wtcsr_ << 8) | wtcnt_;
      case 0x82: return uint32_t(rstcsr_ << 8) | 0xFF;
      default:
        if (divu || dmac) return ioRead(a & ~3u, 4) >> ((o & 2) ? 0 : 16) & 0xFFFF;
        if (o >= 0x100) return uint32_t(misc_[o & ~1u] << 8) | misc_[o | 1] | ((o & ~1u) == 0x1E2 && slaveMode ? 0x8000 : 0);
        return (ioRead(a & ~1u, 1) << 8) | ioRead(a | 1, 1);
    }
  }
  switch (o) {
    case 0x00: return smr_;
    case 0x01: return brr_;
    case 0x02: return scr_;
    case 0x03: return tdr_;
    case 0x04: return ssr_;
    case 0x05: return rdr_;
    case 0x10: return tier_;
    case 0x11: frtRun(); return ftcsr_;
    case 0x12: frtRun(); temp_ = uint8_t(frc_); return frc_ >> 8;
    case 0x13: return temp_;
    case 0x14: return ((tocr_ & 0x10) ? ocrb_ : ocra_) >> 8;
    case 0x15: return ((tocr_ & 0x10) ? ocrb_ : ocra_) & 0xFF;
    case 0x16: return tcr_;
    case 0x17: return tocr_;
    case 0x18: temp_ = uint8_t(ficr_); return ficr_ >> 8;
    case 0x19: return temp_;
    case 0x71: return dma_[0].drcr;
    case 0x72: return dma_[1].drcr;
    case 0x80: wdtRun(); return wtcsr_;
    case 0x81: wdtRun(); return wtcnt_;
    case 0x82: return rstcsr_;
    case 0x92: return ccr_ & 0xCF;
    default:
      if ((o >= 0x60 && o < 0x6A) || (o >= 0xE0 && o < 0xE6)) return (ioRead(a & ~1u, 2) >> ((o & 1) ? 0 : 8)) & 0xFF;
      if (divu || dmac) return (ioRead(a & ~3u, 4) >> ((3 - (o & 3)) * 8)) & 0xFF;
      return misc_[o];
  }
}

void Sh2::ioWrite8(uint32_t a, uint8_t v) {
  ioWrite8x(a, v);
  timersSchedule();
}

void Sh2::ioWrite8x(uint32_t a, uint8_t v) {
  const uint32_t o = a & 0x1FF;
  switch (o) {
    case 0x00: smr_ = v; return;
    case 0x01: brr_ = v; return;
    case 0x02: { const uint8_t old = scr_; scr_ = v; if (!(old & 0x20) && (v & 0x20)) sciSend(); updateInternalIrq(); return; }
    case 0x03: tdr_ = v; return;
    case 0x04:  // SSR: os bits de estado só podem ser zerados (escrevendo 0)
      ssr_ = uint8_t((ssr_ & (v | 0x06)) | (v & 1));
      sciSend();
      updateInternalIrq();
      return;
    case 0x10: frtRun(); tier_ = uint8_t((v & 0x8E) | 1); updateInternalIrq(); return;
    case 0x11: frtRun(); ftcsr_ = uint8_t((ftcsr_ & v & 0x8E) | (v & 1)); updateInternalIrq(); return;
    case 0x12: frtRun(); temp_ = v; return;
    case 0x13: frtRun(); frc_ = uint16_t((temp_ << 8) | v); frtLast_ = clock; return;
    case 0x14: temp_ = v; return;
    case 0x15: frtRun(); if (tocr_ & 0x10) ocrb_ = uint16_t((temp_ << 8) | v); else ocra_ = uint16_t((temp_ << 8) | v); return;
    case 0x16: frtRun(); tcr_ = v & 0x83; frtLast_ = clock; return;
    case 0x17: tocr_ = uint8_t(v | 0xE0); return;
    case 0x71: dma_[0].drcr = v & 3; return;
    case 0x72: dma_[1].drcr = v & 3; return;
    case 0x92:
      ccr_ = v & 0xCF;
      if (v & 0x10) for (auto& s : tag_) for (auto& t : s) t &= ~1u;  // CP: descarta tudo
      if (v & 0x10) std::memset(lru_, 0, sizeof lru_);
      fbase_ = fend_ = 0;
      return;
    default:
      if (o >= 0x60 && o < 0x6A) {
        uint16_t* rr[] = {&iprb_, &vcra_, &vcrb_, &vcrc_, &vcrd_};
        uint16_t& x = *rr[(o - 0x60) / 2];
        x = (o & 1) ? uint16_t((x & 0xFF00) | v) : uint16_t((x & 0xFF) | (v << 8));
        updateInternalIrq();
        return;
      }
      if (o >= 0xE0 && o < 0xE6) {
        uint16_t* rr[] = {&icr_, &ipra_, &vcrwdt_};
        uint16_t& x = *rr[(o - 0xE0) / 2];
        x = (o & 1) ? uint16_t((x & 0xFF00) | v) : uint16_t((x & 0xFF) | (v << 8));
        updateInternalIrq();
        return;
      }
      misc_[o] = v;
      return;
  }
}

void Sh2::ioWrite(uint32_t a, int size, uint32_t v) {
  const uint32_t o = a & 0x1FF;
  if (o >= 0x100 && o < 0x140) {  // DIVU (acessos de 32 bits; 0x120-0x13F espelha)
    if (size != 4) { v = (size == 2) ? (v & 0xFFFF) : (v & 0xFF); }
    switch (o & 0x1C) {
      case 0x00: dvsr_ = v; return;
      case 0x04: dvdntl_ = v; dvdnth_ = (int32_t(v) < 0) ? 0xFFFFFFFFu : 0; divide32(); return;
      case 0x08: dvcr_ = v & 3; updateInternalIrq(); return;
      case 0x0C: vcrdiv_ = v & 0x7F; updateInternalIrq(); return;
      case 0x10: case 0x18: dvdnth_ = v; return;
      default: dvdntl_ = v; divide64(); return;
    }
  }
  if (o >= 0x180 && o < 0x1C0) {
    if (size != 4) return;
    const int ch = (o >> 4) & 1;
    switch (o) {
      case 0x180: case 0x190: dma_[ch].sar = v; return;
      case 0x184: case 0x194: dma_[ch].dar = v; return;
      case 0x188: case 0x198: dma_[ch].tcr = v & 0xFFFFFF; return;
      case 0x18C: case 0x19C: {
        // TE só pode ser zerado (depois de lido como 1)
        const uint32_t te = dma_[ch].chcr & v & 2;
        dma_[ch].chcr = (v & 0xFFFD) | te;
        updateInternalIrq();
        dmaStart(ch);
        return;
      }
      case 0x1A0: dma_[0].vcr = v & 0x7F; updateInternalIrq(); return;
      case 0x1A8: dma_[1].vcr = v & 0x7F; updateInternalIrq(); return;
      case 0x1B0: dmaor_ = (dmaor_ & v & 6) | (v & 9); dmaStart(0); dmaStart(1); return;
      default: return;
    }
  }
  if (size == 4) {
    if (o >= 0x1E0) {  // BSC: só aceita com a chave A55A nos 16 bits de cima
      if ((v >> 16) == 0xA55A) { misc_[o + 2] = uint8_t(v >> 8); misc_[o + 3] = uint8_t(v); }
      return;
    }
    ioWrite(a, 2, v >> 16);
    ioWrite(a + 2, 2, v & 0xFFFF);
    return;
  }
  if (size == 2) {
    if (o == 0x80) {  // WDT: A5xx = WTCSR, 5Axx = WTCNT
      wdtRun();
      if ((v >> 8) == 0xA5) {
        wtcsr_ = uint8_t((wtcsr_ & v & 0x80) | (v & 0x7F) | 0x18);
        if (!(wtcsr_ & 0x20)) wtcnt_ = 0;
        wdtLast_ = clock;
        updateInternalIrq();
      } else if ((v >> 8) == 0x5A) {
        wtcnt_ = uint8_t(v);
      }
      timersSchedule();
      return;
    }
    if (o == 0x82) {
      if ((v >> 8) == 0xA5) rstcsr_ = uint8_t((rstcsr_ & v & 0x80) | 0x1F);
      else if ((v >> 8) == 0x5A) rstcsr_ = uint8_t((rstcsr_ & 0x80) | (v & 0x60) | 0x1F);
      return;
    }
    if ((o >= 0x60 && o < 0x6A) || (o >= 0xE0 && o < 0xE6)) {
      ioWrite8(o, uint8_t(v >> 8));
      ioWrite8(o + 1, uint8_t(v));
      return;
    }
    if (o >= 0x1E0) return;
    ioWrite8(o, uint8_t(v >> 8));
    ioWrite8(o + 1, uint8_t(v));
    return;
  }
  ioWrite8(o, uint8_t(v));
}

// ------------------------------------------------------------ estado
void Sh2::sync(StateIO& io) {
  io.raw(r, sizeof r);
  io.sync(pc); io.sync(pr); io.sync(sr); io.sync(gbr); io.sync(vbr); io.sync(mach); io.sync(macl);
  io.sync(clock); io.sync(sleeping); io.sync(irl_);
  io.raw(cacheData, sizeof cacheData); io.sync(ccr_); io.raw(tag_, sizeof tag_); io.raw(lru_, sizeof lru_);
  io.sync(dvsr_); io.sync(dvdnt_); io.sync(dvcr_); io.sync(vcrdiv_); io.sync(dvdnth_); io.sync(dvdntl_);
  for (auto& d : dma_) { io.sync(d.sar); io.sync(d.dar); io.sync(d.tcr); io.sync(d.chcr); io.sync(d.vcr); io.sync(d.drcr); }
  io.sync(dmaor_);
  io.sync(tier_); io.sync(ftcsr_); io.sync(tcr_); io.sync(tocr_); io.sync(temp_);
  io.sync(frc_); io.sync(ocra_); io.sync(ocrb_); io.sync(ficr_); io.sync(frtLast_);
  io.sync(wtcsr_); io.sync(wtcnt_); io.sync(rstcsr_); io.sync(wdtLast_);
  io.sync(ipra_); io.sync(iprb_); io.sync(vcra_); io.sync(vcrb_); io.sync(vcrc_); io.sync(vcrd_); io.sync(vcrwdt_); io.sync(icr_);
  io.sync(smr_); io.sync(brr_); io.sync(scr_); io.sync(tdr_); io.sync(ssr_); io.sync(rdr_);
  io.raw(misc_, sizeof misc_);
  if (io.reading()) {
    // nenhum valor válido muda; só ficam dentro do que o hardware pode ter (sem acesso fora dos limites)
    clampTo(irl_, 0, 15);
    for (auto& d : dma_) d.tcr &= 0xFFFFFF;
    fbase_ = fend_ = 0;
    irqCheck_ = true;
    updateInternalIrq();
    timersSchedule();
  }
}

}  // namespace indigo::md32x
