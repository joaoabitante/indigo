// NEC VR4300: interpretador do MIPS III com COP0 (TLB, exceções) e COP1 (FPU). Escrito a partir do manual do
// usuário do VR4300 (NEC) e do manual da arquitetura MIPS III, sem código de terceiros.
#include "vr4300.h"

#include <cfenv>
#include <cmath>
#include <cstring>

#include "n64.h"

namespace indigo::n64 {

namespace {
inline uint64_t sx32(uint32_t v) { return uint64_t(int64_t(int32_t(v))); }
inline uint64_t sx16(uint32_t v) { return uint64_t(int64_t(int16_t(v))); }
inline uint64_t sx8(uint32_t v) { return uint64_t(int64_t(int8_t(v))); }
constexpr int EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5, EXC_SYS = 8,
              EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15;
// bits do FCR31
constexpr uint32_t F_I = 1, F_U = 2, F_O = 4, F_Z = 8, F_V = 16, F_E = 32;
const int ROUND[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
}  // namespace

void Vr4300::reset() {
  std::memset(gpr, 0, sizeof gpr);
  std::memset(fpr, 0, sizeof fpr);
  std::memset(cop0, 0, sizeof cop0);
  for (auto& t : tlb) t = Tlb{};
  hi = lo = 0;
  fcr31 = 0;
  cycles = 0;
  countBase_ = 0;
  cop0[1] = 31;            // Random
  fpage_ = 1;
  cop0[12] = 0x34000000;   // Status: CU0, CU1, FR (como o IPL deixa)
  cop0[15] = 0x00000B22;   // PRId
  cop0[16] = 0x7006E463;   // Config
  cop0[14] = cop0[30] = 0xFFFFFFFFFFFFFFFFull;
  cop0[11] = 0xFFFFFFFF;
  pc_ = 0xFFFFFFFFBFC00000ull; npc_ = pc_ + 4;
  delay_ = nextDelay_ = false;
  llbit_ = false;
  scheduleCompare();
  updateIrq();
}

// --------------------------------------------------------------------------------------- interrupções
void Vr4300::setIp(int bit, bool on) {
  if (on) cop0[13] |= 1u << (8 + bit);
  else cop0[13] &= ~uint64_t(1u << (8 + bit));
  updateIrq();
}

void Vr4300::updateIrq() {
  const uint64_t st = cop0[12];
  irq_ = (st & 1) && !(st & 6) && (cop0[13] & st & 0xFF00);
}

void Vr4300::scheduleCompare() {
  const uint32_t c = count();
  uint64_t delta = uint32_t(uint32_t(cop0[11]) - c);
  if (delta == 0) delta = 1ull << 32;
  const uint64_t cur4 = uint64_t(cycles) * 3 + countBase_;
  const uint64_t falta4 = (cur4 & ~3ull) + delta * 4 - cur4;
  compareAt_ = cycles + int64_t((falta4 + 2) / 3);
}

void Vr4300::exception(int code, int ce) {
  uint64_t& st = cop0[12];
  uint64_t& cause = cop0[13];
  bool refill = false;
  (void)refill;
  if (!(st & 2)) {
    cop0[14] = delay_ ? curPc_ - 4 : curPc_;
    if (delay_) cause |= 0x80000000u; else cause &= ~uint64_t(0x80000000u);
  }
  cause = (cause & ~uint64_t(0x3000007C)) | (uint32_t(code) << 2) | (uint32_t(ce) << 28);
  const bool exlAntes = st & 2;
  st |= 2;
  uint64_t base = (st & (1u << 22)) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
  (void)exlAntes;
  pc_ = base + 0x180;
  npc_ = pc_ + 4;
  nextDelay_ = false;
  exc_ = true;
  updateIrq();
}

void Vr4300::tlbException(int code, uint64_t va, bool refill) {
  const bool exl = cop0[12] & 2;
  cop0[8] = va;
  cop0[4] = (cop0[4] & ~uint64_t(0x7FFFF0)) | ((va >> 9) & 0x7FFFF0);
  cop0[20] = (cop0[20] & ~uint64_t(0x1FFFFFFF0ull)) | ((va >> 9) & 0x7FFFFFF0) | (((va >> 62) & 3) << 31);
  cop0[10] = (va & 0xC00000FFFFFFE000ull) | (cop0[10] & 0xFF);
  exception(code);
  if (refill && !exl) {
    const uint64_t base = (cop0[12] & (1u << 22)) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    pc_ = base + ((cop0[12] & 0x80) ? 0x80 : 0);  // KX: refill de 64 bits
    npc_ = pc_ + 4;
  }
}

void Vr4300::addrError(int code, uint64_t va) {
  cop0[8] = va;
  exception(code);
}

// --------------------------------------------------------------------------------------- memória
bool Vr4300::tlbLookup(uint64_t va, uint32_t& pa, bool write) {
  const uint8_t asid = uint8_t(cop0[10]);
  for (const Tlb& e : tlb) {
    const uint64_t m = uint64_t(e.mask) | 0x1FFF;
    if ((va ^ e.hi) & 0xC00000FFFFFFE000ull & ~m) continue;
    if (!e.g && uint8_t(e.hi) != asid) continue;
    const uint64_t half = (m + 1) >> 1;
    const uint32_t loe = (va & half) ? e.lo1 : e.lo0;
    if (!(loe & 2)) { tlbException(write ? EXC_TLBS : EXC_TLBL, va, false); return false; }
    if (write && !(loe & 4)) { tlbException(EXC_MOD, va, false); return false; }
    const uint64_t pfn = uint64_t((loe >> 6) & 0xFFFFF) << 12;
    pa = uint32_t((pfn & ~(half - 1)) | (va & (half - 1)));
    return true;
  }
  tlbException(write ? EXC_TLBS : EXC_TLBL, va, true);
  return false;
}

inline bool Vr4300::translate(uint64_t va, uint32_t& pa, bool write) {
  const uint32_t v = uint32_t(va);
  if ((v & 0xC0000000u) == 0x80000000u) { pa = v & 0x1FFFFFFF; return true; }
  return tlbLookup(va, pa, write);
}

inline bool Vr4300::fetch(uint64_t va, uint32_t& op) {
  if ((va & ~0xFFCull) == fpage_) { std::memcpy(&op, fptr_ + (va & 0xFFC), 4); return true; }
  return fetchSlow(va, op);
}

bool Vr4300::fetchSlow(uint64_t va, uint32_t& op) {
  if (va & 3) { addrError(EXC_ADEL, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, false)) return false;
  if (pa < N64::RDRAM_SIZE) {
    std::memcpy(&op, &sys.rdram[pa], 4);
    fpage_ = va & ~0xFFFull;
    fptr_ = &sys.rdram[pa & ~0xFFFu];
  } else {
    op = sys.read32(pa);
  }
  return true;
}

bool Vr4300::load8(uint64_t va, uint64_t& v) {
  uint32_t pa;
  if (!translate(va, pa, false)) return false;
  v = pa < N64::RDRAM_SIZE ? sys.rdram[pa ^ 3] : sys.read8(pa);
  return true;
}
bool Vr4300::load16(uint64_t va, uint64_t& v) {
  if (va & 1) { addrError(EXC_ADEL, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, false)) return false;
  if (pa < N64::RDRAM_SIZE) { uint16_t h; std::memcpy(&h, &sys.rdram[pa ^ 2], 2); v = h; }
  else v = sys.read16(pa);
  return true;
}
bool Vr4300::load32(uint64_t va, uint64_t& v) {
  if (va & 3) { addrError(EXC_ADEL, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, false)) return false;
  if (pa < N64::RDRAM_SIZE) { uint32_t w; std::memcpy(&w, &sys.rdram[pa], 4); v = w; }
  else v = sys.read32(pa);
  return true;
}
bool Vr4300::load64(uint64_t va, uint64_t& v) {
  if (va & 7) { addrError(EXC_ADEL, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, false)) return false;
  uint32_t a, b;
  if (pa < N64::RDRAM_SIZE) { std::memcpy(&a, &sys.rdram[pa], 4); std::memcpy(&b, &sys.rdram[pa + 4], 4); }
  else { a = sys.read32(pa); b = sys.read32(pa + 4); }
  v = (uint64_t(a) << 32) | b;
  return true;
}
bool Vr4300::store8(uint64_t va, uint32_t v) {
  uint32_t pa;
  if (!translate(va, pa, true)) return false;
  if (pa < N64::RDRAM_SIZE) sys.rdram[pa ^ 3] = uint8_t(v);
  else sys.write8(pa, uint8_t(v));
  return true;
}
bool Vr4300::store16(uint64_t va, uint32_t v) {
  if (va & 1) { addrError(EXC_ADES, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, true)) return false;
  if (pa < N64::RDRAM_SIZE) { const uint16_t h = uint16_t(v); std::memcpy(&sys.rdram[pa ^ 2], &h, 2); }
  else sys.write16(pa, uint16_t(v));
  return true;
}
bool Vr4300::store32(uint64_t va, uint32_t v) {
  if (va & 3) { addrError(EXC_ADES, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, true)) return false;
  if (pa < N64::RDRAM_SIZE) std::memcpy(&sys.rdram[pa], &v, 4);
  else sys.write32(pa, v);
  return true;
}
bool Vr4300::store64(uint64_t va, uint64_t v) {
  if (va & 7) { addrError(EXC_ADES, va); return false; }
  uint32_t pa;
  if (!translate(va, pa, true)) return false;
  const uint32_t a = uint32_t(v >> 32), b = uint32_t(v);
  if (pa < N64::RDRAM_SIZE) { std::memcpy(&sys.rdram[pa], &a, 4); std::memcpy(&sys.rdram[pa + 4], &b, 4); }
  else { sys.write32(pa, a); sys.write32(pa + 4, b); }
  return true;
}

// --------------------------------------------------------------------------------------- laço principal
void Vr4300::run(int64_t ate) {
  const int rm = fcr31 & 3;
  const int oldRound = std::fegetround();
  if (rm) std::fesetround(ROUND[rm]);
  while (cycles < ate) {
    if (cycles >= compareAt_) {
      cop0[13] |= 0x8000;
      compareAt_ += (int64_t(1) << 34) / 3;
      updateIrq();
    }
    const int64_t lim = ate < compareAt_ ? ate : compareAt_;
    while (cycles < lim) {
      if (irq_) {
        curPc_ = pc_;
        delay_ = nextDelay_;
        exception(EXC_INT);
        exc_ = false;
      }
      uint32_t op;
      curPc_ = pc_;
      delay_ = nextDelay_;
      nextDelay_ = false;
      cycles++;
      if (!fetch(pc_, op)) { exc_ = false; continue; }
      pc_ = npc_;
      npc_ += 4;
      step(op);
      gpr[0] = 0;
      exc_ = false;
      instrucoes++;
      // laço de espera (desvio para si mesmo com nop no atraso): pula até o próximo evento
      if (nextDelay_ && npc_ == curPc_ && !irq_ && op != 0) {
        uint32_t ds;
        uint32_t pa;
        if (uint32_t(pc_) >= 0x80000000u && uint32_t(pc_) < 0xC0000000u && (pa = uint32_t(pc_) & 0x1FFFFFFF) < N64::RDRAM_SIZE) {
          std::memcpy(&ds, &sys.rdram[pa], 4);
          if (ds == 0 && ((op >> 26) == 2 || (op >> 26) == 4 || (op >> 26) == 5 || op >> 26 == 0x14) ) {
            // só desvios incondicionais ou que não dependem de nada que mude sozinho
            const uint32_t opc = op >> 26;
            bool sempre = opc == 2 || ((opc == 4 || opc == 0x14) && ((op >> 21) & 31) == ((op >> 16) & 31));
            if (sempre && lim > cycles + 2) cycles = lim - 2;
          }
        }
      }
    }
  }
  if (rm || std::fegetround() != oldRound) std::fesetround(oldRound);
}

void Vr4300::branch(bool cond, uint32_t op, bool likely) {
  if (cond) {
    npc_ = curPc_ + 4 + (sx16(op) << 2);
    nextDelay_ = true;
  } else if (likely) {
    pc_ = npc_;
    npc_ += 4;
  } else {
    nextDelay_ = true;
  }
}

void Vr4300::step(uint32_t op) {
  const int rs = (op >> 21) & 31, rt = (op >> 16) & 31;
  const uint64_t imm = sx16(op);
  uint64_t v;
  switch (op >> 26) {
    case 0x00: special(op); break;
    case 0x01: regimm(op); break;
    case 0x02:  // J
      npc_ = (pc_ & ~0x0FFFFFFFull) | ((op & 0x3FFFFFF) << 2);
      nextDelay_ = true;
      break;
    case 0x03:  // JAL
      gpr[31] = curPc_ + 8;
      npc_ = (pc_ & ~0x0FFFFFFFull) | ((op & 0x3FFFFFF) << 2);
      nextDelay_ = true;
      break;
    case 0x04: branch(gpr[rs] == gpr[rt], op, false); break;
    case 0x05: branch(gpr[rs] != gpr[rt], op, false); break;
    case 0x06: branch(int64_t(gpr[rs]) <= 0, op, false); break;
    case 0x07: branch(int64_t(gpr[rs]) > 0, op, false); break;
    case 0x08: {  // ADDI
      const int32_t a = int32_t(gpr[rs]), b = int32_t(imm);
      int32_t r;
      if (__builtin_add_overflow(a, b, &r)) { exception(EXC_OV); break; }
      gpr[rt] = sx32(uint32_t(r));
      break;
    }
    case 0x09: gpr[rt] = sx32(uint32_t(gpr[rs] + imm)); break;
    case 0x0A: gpr[rt] = int64_t(gpr[rs]) < int64_t(imm); break;
    case 0x0B: gpr[rt] = gpr[rs] < imm; break;
    case 0x0C: gpr[rt] = gpr[rs] & (op & 0xFFFF); break;
    case 0x0D: gpr[rt] = gpr[rs] | (op & 0xFFFF); break;
    case 0x0E: gpr[rt] = gpr[rs] ^ (op & 0xFFFF); break;
    case 0x0F: gpr[rt] = sx32(op << 16); break;
    case 0x10: cop0op(op); break;
    case 0x11: cop1op(op); break;
    case 0x12: exception(EXC_CPU, 2); break;  // COP2 não existe na CPU
    case 0x14: branch(gpr[rs] == gpr[rt], op, true); break;
    case 0x15: branch(gpr[rs] != gpr[rt], op, true); break;
    case 0x16: branch(int64_t(gpr[rs]) <= 0, op, true); break;
    case 0x17: branch(int64_t(gpr[rs]) > 0, op, true); break;
    case 0x18: {  // DADDI
      int64_t r;
      if (__builtin_add_overflow(int64_t(gpr[rs]), int64_t(imm), &r)) { exception(EXC_OV); break; }
      gpr[rt] = uint64_t(r);
      break;
    }
    case 0x19: gpr[rt] = gpr[rs] + imm; break;
    case 0x1A: case 0x1B: {  // LDL / LDR
      const uint64_t va = gpr[rs] + imm;
      if (!load64(va & ~7ull, v)) break;
      const int k = int(va & 7);
      if ((op >> 26) == 0x1A) {
        const int sh = 8 * k;
        const uint64_t keep = sh ? (gpr[rt] & ((1ull << sh) - 1)) : 0;
        gpr[rt] = (v << sh) | keep;
      } else {
        const int sh = 8 * (7 - k);
        const uint64_t keep = sh ? (gpr[rt] & ~(~0ull >> sh)) : 0;
        gpr[rt] = (v >> sh) | keep;
      }
      break;
    }
    case 0x20: if (load8(gpr[rs] + imm, v)) gpr[rt] = sx8(uint32_t(v)); break;
    case 0x21: if (load16(gpr[rs] + imm, v)) gpr[rt] = sx16(uint32_t(v)); break;
    case 0x22: case 0x26: {  // LWL / LWR
      const uint64_t va = gpr[rs] + imm;
      if (!load32(va & ~3ull, v)) break;
      const int k = int(va & 3);
      const uint32_t w = uint32_t(v), old = uint32_t(gpr[rt]);
      if ((op >> 26) == 0x22) {
        const int sh = 8 * k;
        gpr[rt] = sx32((w << sh) | (sh ? (old & ((1u << sh) - 1)) : 0));
      } else {
        const int sh = 8 * (3 - k);
        const uint32_t r = (w >> sh) | (sh ? (old & ~(0xFFFFFFFFu >> sh)) : 0);
        gpr[rt] = k == 3 ? sx32(r) : ((gpr[rt] & 0xFFFFFFFF00000000ull) | r);
      }
      break;
    }
    case 0x23: if (load32(gpr[rs] + imm, v)) gpr[rt] = sx32(uint32_t(v)); break;
    case 0x24: if (load8(gpr[rs] + imm, v)) gpr[rt] = v; break;
    case 0x25: if (load16(gpr[rs] + imm, v)) gpr[rt] = v; break;
    case 0x27: if (load32(gpr[rs] + imm, v)) gpr[rt] = uint32_t(v); break;
    case 0x28: store8(gpr[rs] + imm, uint32_t(gpr[rt])); break;
    case 0x29: store16(gpr[rs] + imm, uint32_t(gpr[rt])); break;
    case 0x2A: case 0x2E: {  // SWL / SWR
      const uint64_t va = gpr[rs] + imm;
      if (!load32(va & ~3ull, v)) { break; }
      const int k = int(va & 3);
      uint32_t w = uint32_t(v);
      const uint32_t r = uint32_t(gpr[rt]);
      if ((op >> 26) == 0x2A) {
        const int sh = 8 * k;
        w = (w & (sh ? ~(0xFFFFFFFFu >> sh) : 0)) | (r >> sh);
      } else {
        const int sh = 8 * (3 - k);
        w = (w & (sh ? ~(0xFFFFFFFFu << sh) : 0)) | (r << sh);
      }
      store32(va & ~3ull, w);
      break;
    }
    case 0x2B: store32(gpr[rs] + imm, uint32_t(gpr[rt])); break;
    case 0x2C: case 0x2D: {  // SDL / SDR
      const uint64_t va = gpr[rs] + imm;
      if (!load64(va & ~7ull, v)) break;
      const int k = int(va & 7);
      const uint64_t r = gpr[rt];
      if ((op >> 26) == 0x2C) {
        const int sh = 8 * k;
        v = (v & (sh ? ~(~0ull >> sh) : 0)) | (r >> sh);
      } else {
        const int sh = 8 * (7 - k);
        v = (v & (sh ? ~(~0ull << sh) : 0)) | (r << sh);
      }
      store64(va & ~7ull, v);
      break;
    }
    case 0x2F: break;  // CACHE: sem caches
    case 0x30:  // LL
      if (load32(gpr[rs] + imm, v)) {
        gpr[rt] = sx32(uint32_t(v));
        llbit_ = true;
        uint32_t pa;
        if (translate(gpr[rs] + imm, pa, false)) cop0[17] = pa >> 4;
      }
      break;
    case 0x34:  // LLD
      if (load64(gpr[rs] + imm, v)) {
        gpr[rt] = v;
        llbit_ = true;
        uint32_t pa;
        if (translate(gpr[rs] + imm, pa, false)) cop0[17] = pa >> 4;
      }
      break;
    case 0x37: if (load64(gpr[rs] + imm, v)) gpr[rt] = v; break;
    case 0x38:  // SC
      if (llbit_) { if (store32(gpr[rs] + imm, uint32_t(gpr[rt]))) gpr[rt] = 1; }
      else gpr[rt] = 0;
      break;
    case 0x3C:  // SCD
      if (llbit_) { if (store64(gpr[rs] + imm, gpr[rt])) gpr[rt] = 1; }
      else gpr[rt] = 0;
      break;
    case 0x3F: store64(gpr[rs] + imm, gpr[rt]); break;
    // COP1: loads/stores
    case 0x31: case 0x35: case 0x39: case 0x3D: {
      if (!(cop0[12] & (1u << 29))) { exception(EXC_CPU, 1); break; }
      const uint64_t va = gpr[rs] + imm;
      switch (op >> 26) {
        case 0x31: if (load32(va, v)) fset32(rt, uint32_t(v)); break;
        case 0x35: if (load64(va, v)) fset64(rt, v); break;
        case 0x39: store32(va, fget32(rt)); break;
        case 0x3D: store64(va, fget64(rt)); break;
      }
      break;
    }
    case 0x32: case 0x36: case 0x3A: case 0x3E:  // LWC2/LDC2/SWC2/SDC2
      exception(EXC_CPU, 2);
      break;
    default: exception(EXC_RI); break;
  }
}

void Vr4300::special(uint32_t op) {
  const int rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  const uint64_t a = gpr[rs], b = gpr[rt];
  switch (op & 63) {
    case 0x00: gpr[rd] = sx32(uint32_t(b) << sa); break;
    case 0x02: gpr[rd] = sx32(uint32_t(b) >> sa); break;
    case 0x03: gpr[rd] = sx32(uint32_t(int64_t(b) >> sa)); break;
    case 0x04: gpr[rd] = sx32(uint32_t(b) << (a & 31)); break;
    case 0x06: gpr[rd] = sx32(uint32_t(b) >> (a & 31)); break;
    case 0x07: gpr[rd] = sx32(uint32_t(int64_t(b) >> (a & 31))); break;
    case 0x08: npc_ = a; nextDelay_ = true; break;  // JR
    case 0x09: npc_ = a; gpr[rd] = curPc_ + 8; nextDelay_ = true; break;  // JALR
    case 0x0C: exception(EXC_SYS); break;
    case 0x0D: exception(EXC_BP); break;
    case 0x0F: break;  // SYNC
    case 0x10: gpr[rd] = hi; break;
    case 0x11: hi = a; break;
    case 0x12: gpr[rd] = lo; break;
    case 0x13: lo = a; break;
    case 0x14: gpr[rd] = b << (a & 63); break;
    case 0x16: gpr[rd] = b >> (a & 63); break;
    case 0x17: gpr[rd] = uint64_t(int64_t(b) >> (a & 63)); break;
    case 0x18: {
      const int64_t r = int64_t(int32_t(a)) * int64_t(int32_t(b));
      lo = sx32(uint32_t(r)); hi = sx32(uint32_t(uint64_t(r) >> 32));
      break;
    }
    case 0x19: {
      const uint64_t r = uint64_t(uint32_t(a)) * uint64_t(uint32_t(b));
      lo = sx32(uint32_t(r)); hi = sx32(uint32_t(r >> 32));
      break;
    }
    case 0x1A: {
      const int32_t n = int32_t(a), d = int32_t(b);
      if (d == 0) { lo = n >= 0 ? ~0ull : 1; hi = sx32(uint32_t(n)); }
      else if (n == INT32_MIN && d == -1) { lo = sx32(uint32_t(n)); hi = 0; }
      else { lo = sx32(uint32_t(n / d)); hi = sx32(uint32_t(n % d)); }
      break;
    }
    case 0x1B: {
      const uint32_t n = uint32_t(a), d = uint32_t(b);
      if (d == 0) { lo = ~0ull; hi = sx32(n); }
      else { lo = sx32(n / d); hi = sx32(n % d); }
      break;
    }
    case 0x1C: {
      const __int128 r = __int128(int64_t(a)) * __int128(int64_t(b));
      lo = uint64_t(r); hi = uint64_t(r >> 64);
      break;
    }
    case 0x1D: {
      const unsigned __int128 r = (unsigned __int128)a * b;
      lo = uint64_t(r); hi = uint64_t(r >> 64);
      break;
    }
    case 0x1E: {
      const int64_t n = int64_t(a), d = int64_t(b);
      if (d == 0) { lo = n >= 0 ? ~0ull : 1; hi = uint64_t(n); }
      else if (n == INT64_MIN && d == -1) { lo = uint64_t(n); hi = 0; }
      else { lo = uint64_t(n / d); hi = uint64_t(n % d); }
      break;
    }
    case 0x1F:
      if (b == 0) { lo = ~0ull; hi = a; }
      else { lo = a / b; hi = a % b; }
      break;
    case 0x20: {
      int32_t r;
      if (__builtin_add_overflow(int32_t(a), int32_t(b), &r)) { exception(EXC_OV); break; }
      gpr[rd] = sx32(uint32_t(r));
      break;
    }
    case 0x21: gpr[rd] = sx32(uint32_t(a + b)); break;
    case 0x22: {
      int32_t r;
      if (__builtin_sub_overflow(int32_t(a), int32_t(b), &r)) { exception(EXC_OV); break; }
      gpr[rd] = sx32(uint32_t(r));
      break;
    }
    case 0x23: gpr[rd] = sx32(uint32_t(a - b)); break;
    case 0x24: gpr[rd] = a & b; break;
    case 0x25: gpr[rd] = a | b; break;
    case 0x26: gpr[rd] = a ^ b; break;
    case 0x27: gpr[rd] = ~(a | b); break;
    case 0x2A: gpr[rd] = int64_t(a) < int64_t(b); break;
    case 0x2B: gpr[rd] = a < b; break;
    case 0x2C: {
      int64_t r;
      if (__builtin_add_overflow(int64_t(a), int64_t(b), &r)) { exception(EXC_OV); break; }
      gpr[rd] = uint64_t(r);
      break;
    }
    case 0x2D: gpr[rd] = a + b; break;
    case 0x2E: {
      int64_t r;
      if (__builtin_sub_overflow(int64_t(a), int64_t(b), &r)) { exception(EXC_OV); break; }
      gpr[rd] = uint64_t(r);
      break;
    }
    case 0x2F: gpr[rd] = a - b; break;
    case 0x30: if (int64_t(a) >= int64_t(b)) exception(EXC_TR); break;
    case 0x31: if (a >= b) exception(EXC_TR); break;
    case 0x32: if (int64_t(a) < int64_t(b)) exception(EXC_TR); break;
    case 0x33: if (a < b) exception(EXC_TR); break;
    case 0x34: if (a == b) exception(EXC_TR); break;
    case 0x36: if (a != b) exception(EXC_TR); break;
    case 0x38: gpr[rd] = b << sa; break;
    case 0x3A: gpr[rd] = b >> sa; break;
    case 0x3B: gpr[rd] = uint64_t(int64_t(b) >> sa); break;
    case 0x3C: gpr[rd] = b << (sa + 32); break;
    case 0x3E: gpr[rd] = b >> (sa + 32); break;
    case 0x3F: gpr[rd] = uint64_t(int64_t(b) >> (sa + 32)); break;
    default: exception(EXC_RI); break;
  }
}

void Vr4300::regimm(uint32_t op) {
  const int rs = (op >> 21) & 31;
  const int64_t a = int64_t(gpr[rs]);
  const int64_t imm = int64_t(sx16(op));
  switch ((op >> 16) & 31) {
    case 0x00: branch(a < 0, op, false); break;
    case 0x01: branch(a >= 0, op, false); break;
    case 0x02: branch(a < 0, op, true); break;
    case 0x03: branch(a >= 0, op, true); break;
    case 0x08: if (a >= imm) exception(EXC_TR); break;
    case 0x09: if (uint64_t(a) >= uint64_t(imm)) exception(EXC_TR); break;
    case 0x0A: if (a < imm) exception(EXC_TR); break;
    case 0x0B: if (uint64_t(a) < uint64_t(imm)) exception(EXC_TR); break;
    case 0x0C: if (a == imm) exception(EXC_TR); break;
    case 0x0E: if (a != imm) exception(EXC_TR); break;
    case 0x10: gpr[31] = curPc_ + 8; branch(a < 0, op, false); break;
    case 0x11: gpr[31] = curPc_ + 8; branch(a >= 0, op, false); break;
    case 0x12: gpr[31] = curPc_ + 8; branch(a < 0, op, true); break;
    case 0x13: gpr[31] = curPc_ + 8; branch(a >= 0, op, true); break;
    default: exception(EXC_RI); break;
  }
}

// --------------------------------------------------------------------------------------- COP0
uint64_t Vr4300::readCop0(int r) {
  switch (r) {
    case 1: {  // Random: desce a cada instrução entre Wired e 31
      const uint32_t w = uint32_t(cop0[6]) & 31;
      if (w >= 31) return 31;
      const uint32_t n = 32 - w;
      return 31 - uint64_t(cycles - int64_t(cop0[1] >> 8)) % n;
    }
    case 9: return count();
    case 7: case 21: case 22: case 23: case 24: case 25: case 31: return 0;
    default: return cop0[r];
  }
}

void Vr4300::writeCop0(int r, uint64_t v) {
  switch (r) {
    case 0: cop0[0] = (cop0[0] & 0x80000000u) | (v & 0x3F); break;
    case 1: break;
    case 2: case 3: cop0[r] = v & 0x3FFFFFFF; break;
    case 4: cop0[4] = (cop0[4] & 0x7FFFF0) | (v & ~0x7FFFFFull); break;
    case 5: cop0[5] = v & 0x01FFE000; break;
    case 6: cop0[6] = v & 0x3F; cop0[1] = uint64_t(cycles) << 8 | 31; break;
    case 9: countBase_ = (uint64_t(uint32_t(v)) << 2) - uint64_t(cycles) * 3; scheduleCompare(); break;
    case 10: cop0[10] = v & 0xC00000FFFFFFE0FFull; fpage_ = 1; break;
    case 11:
      cop0[11] = uint32_t(v);
      cop0[13] &= ~uint64_t(0x8000);
      scheduleCompare();
      updateIrq();
      break;
    case 12: {
      const uint64_t antes = cop0[12];
      cop0[12] = (cop0[12] & ~0xFF57FFFFull) | (v & 0xFF57FFFF);
      (void)antes;
      updateIrq();
      break;
    }
    case 13: cop0[13] = (cop0[13] & ~uint64_t(0x300)) | (v & 0x300); updateIrq(); break;
    case 14: case 30: cop0[r] = v; break;
    case 16: cop0[16] = (cop0[16] & ~0x0F00800Full) | (v & 0x0F00800F); break;
    case 17: case 18: case 19: cop0[r] = uint32_t(v); break;
    case 20: cop0[20] = (cop0[20] & 0x1FFFFFFF0ull) | (v & ~0x1FFFFFFFFull); break;
    case 28: cop0[28] = v & 0x0FFFFFC0; break;
    case 29: cop0[29] = 0; break;
    default: break;
  }
}

void Vr4300::cop0op(uint32_t op) {
  const int rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  switch ((op >> 21) & 31) {
    case 0x00: gpr[rt] = sx32(uint32_t(readCop0(rd))); break;
    case 0x01: gpr[rt] = readCop0(rd); break;
    case 0x04: writeCop0(rd, sx32(uint32_t(gpr[rt]))); break;
    case 0x05: writeCop0(rd, gpr[rt]); break;
    default:
      if (op & (1u << 25)) {
        switch (op & 63) {
          case 0x01: {  // TLBR
            const Tlb& e = tlb[cop0[0] & 31];
            cop0[5] = e.mask;
            cop0[10] = e.hi & ~uint64_t(e.mask);
            cop0[2] = e.lo0 | (e.g ? 1 : 0);
            cop0[3] = e.lo1 | (e.g ? 1 : 0);
            break;
          }
          case 0x02: case 0x06: {  // TLBWI / TLBWR
            const int i = (op & 63) == 2 ? int(cop0[0] & 31) : int(readCop0(1));
            Tlb& e = tlb[i];
            e.mask = uint32_t(cop0[5]) & 0x01FFE000;
            e.hi = cop0[10] & ~uint64_t(e.mask) & 0xC00000FFFFFFE0FFull;
            e.g = cop0[2] & cop0[3] & 1;
            e.lo0 = uint32_t(cop0[2]) & 0x3FFFFFFE;
            e.lo1 = uint32_t(cop0[3]) & 0x3FFFFFFE;
            fpage_ = 1;
            break;
          }
          case 0x08: {  // TLBP
            cop0[0] = 0x80000000u;
            for (int i = 0; i < 32; i++) {
              const Tlb& e = tlb[i];
              const uint64_t m = uint64_t(e.mask) | 0x1FFF;
              if ((cop0[10] ^ e.hi) & 0xC00000FFFFFFE000ull & ~m) continue;
              if (!e.g && uint8_t(e.hi) != uint8_t(cop0[10])) continue;
              cop0[0] = i;
              break;
            }
            break;
          }
          case 0x18:  // ERET
            if (cop0[12] & 4) { pc_ = cop0[30]; cop0[12] &= ~uint64_t(4); }
            else { pc_ = cop0[14]; cop0[12] &= ~uint64_t(2); }
            npc_ = pc_ + 4;
            nextDelay_ = false;
            llbit_ = false;
            updateIrq();
            break;
          default: break;
        }
      } else {
        exception(EXC_RI);
      }
      break;
  }
}

// --------------------------------------------------------------------------------------- COP1 (FPU)
uint32_t Vr4300::fget32(int r) const {
  if (fr()) return uint32_t(fpr[r]);
  return r & 1 ? uint32_t(fpr[r & ~1] >> 32) : uint32_t(fpr[r]);
}
void Vr4300::fset32(int r, uint32_t v) {
  if (fr()) { fpr[r] = (fpr[r] & 0xFFFFFFFF00000000ull) | v; return; }
  uint64_t& d = fpr[r & ~1];
  if (r & 1) d = (d & 0xFFFFFFFFull) | (uint64_t(v) << 32);
  else d = (d & 0xFFFFFFFF00000000ull) | v;
}
uint64_t Vr4300::fget64(int r) const { return fr() ? fpr[r] : fpr[r & ~1]; }
void Vr4300::fset64(int r, uint64_t v) { if (fr()) fpr[r] = v; else fpr[r & ~1] = v; }

namespace {
uint32_t hostFlags() {
  const int e = std::fetestexcept(FE_ALL_EXCEPT);
  uint32_t f = 0;
  if (e & FE_INEXACT) f |= F_I;
  if (e & FE_UNDERFLOW) f |= F_U;
  if (e & FE_OVERFLOW) f |= F_O;
  if (e & FE_DIVBYZERO) f |= F_Z;
  if (e & FE_INVALID) f |= F_V;
  return f;
}
inline float f32(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline double f64(uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }
inline uint32_t b32(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }
inline uint64_t b64(double d) { uint64_t v; std::memcpy(&v, &d, 8); return v; }
inline bool sub32(uint32_t v) { return (v & 0x7F800000) == 0 && (v & 0x7FFFFF); }
inline bool sub64(uint64_t v) { return (v & 0x7FF0000000000000ull) == 0 && (v & 0xFFFFFFFFFFFFFull); }
inline bool nan32(uint32_t v) { return (v & 0x7F800000) == 0x7F800000 && (v & 0x7FFFFF); }
inline bool nan64(uint64_t v) { return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & 0xFFFFFFFFFFFFFull); }
// NaN sinalizador (bit mais alto da fração = 1 no MIPS)
inline bool snan32(uint32_t v) { return nan32(v) && (v & 0x400000); }
inline bool snan64(uint64_t v) { return nan64(v) && (v & 0x8000000000000ull); }
constexpr uint32_t QNAN32 = 0x7FBFFFFF;
constexpr uint64_t QNAN64 = 0x7FF7FFFFFFFFFFFFull;
}  // namespace

// ajusta a causa e as flags; true se a exceção do FPU foi tomada (resultado não é gravado)
bool Vr4300::fpuCheck(uint32_t f) {
  fcr31 = (fcr31 & ~0x3F000u) | (f << 12);
  const uint32_t en = ((fcr31 >> 7) & 0x1F) | F_E;
  if (f & en) { exception(EXC_FPE); return true; }
  fcr31 |= (f & 0x1F) << 2;
  return false;
}
bool Vr4300::fpuUnimpl() { return fpuCheck(F_E); }

void Vr4300::cop1op(uint32_t op) {
  if (!(cop0[12] & (1u << 29))) { exception(EXC_CPU, 1); return; }
  const int fmt = (op >> 21) & 31, ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  switch (fmt) {
    case 0x00: gpr[ft] = sx32(fget32(fs)); return;
    case 0x01: gpr[ft] = fget64(fs); return;
    case 0x02: gpr[ft] = sx32(fs == 31 ? fcr31 : fs == 0 ? 0x00000A00u : 0u); return;
    case 0x04: fset32(fs, uint32_t(gpr[ft])); return;
    case 0x05: fset64(fs, gpr[ft]); return;
    case 0x06:
      if (fs == 31) {
        fcr31 = uint32_t(gpr[ft]) & 0x0183FFFF;
        std::fesetround(ROUND[fcr31 & 3]);
        const uint32_t cause = (fcr31 >> 12) & 0x3F, en = ((fcr31 >> 7) & 0x1F) | F_E;
        if (cause & en) exception(EXC_FPE);
      }
      return;
    case 0x08: {
      const bool c = (fcr31 >> 23) & 1;
      switch (ft & 3) {
        case 0: branch(!c, op, false); break;
        case 1: branch(c, op, false); break;
        case 2: branch(!c, op, true); break;
        case 3: branch(c, op, true); break;
      }
      return;
    }
    case 0x10: case 0x11: case 0x14: case 0x15: break;
    default: exception(EXC_RI); return;
  }
  const int fn = op & 63;
  const bool S = fmt == 0x10, D = fmt == 0x11, W = fmt == 0x14, L = fmt == 0x15;
  // comparações
  if (fn >= 0x30) {
    if (!(S || D)) { fpuUnimpl(); return; }
    bool un, eq, lt, sig;
    if (S) {
      const uint32_t a = fget32(fs), b = fget32(ft);
      un = nan32(a) || nan32(b);
      sig = snan32(a) || snan32(b);
      const float x = f32(a), y = f32(b);
      eq = !un && x == y; lt = !un && x < y;
    } else {
      const uint64_t a = fget64(fs), b = fget64(ft);
      un = nan64(a) || nan64(b);
      sig = snan64(a) || snan64(b);
      const double x = f64(a), y = f64(b);
      eq = !un && x == y; lt = !un && x < y;
    }
    if (un && (sig || (fn & 8))) { if (fpuCheck(F_V)) return; }
    else fcr31 &= ~0x3F000u;
    const bool c = ((fn & 1) && un) || ((fn & 2) && eq) || ((fn & 4) && lt);
    if (c) fcr31 |= 1u << 23; else fcr31 &= ~(1u << 23);
    return;
  }
  switch (fn) {
    case 0x05: case 0x06: case 0x07: {  // ABS, MOV, NEG
      if (S) {
        uint32_t a = fget32(fs);
        if (fn != 6 && nan32(a)) { fpuCheck(F_V); return; }
        if (fn == 5) a &= 0x7FFFFFFF; else if (fn == 7) a ^= 0x80000000u;
        if (fn != 6) fcr31 &= ~0x3F000u;
        fset32(fd, a);
      } else if (D) {
        uint64_t a = fget64(fs);
        if (fn != 6 && nan64(a)) { fpuCheck(F_V); return; }
        if (fn == 5) a &= 0x7FFFFFFFFFFFFFFFull; else if (fn == 7) a ^= 0x8000000000000000ull;
        if (fn != 6) fcr31 &= ~0x3F000u;
        fset64(fd, a);
      } else fpuUnimpl();
      return;
    }
    default: break;
  }
  // aritmética
  if (fn <= 0x04) {
    if (!(S || D)) { fpuUnimpl(); return; }
    std::feclearexcept(FE_ALL_EXCEPT);
    if (S) {
      const uint32_t a = fget32(fs), b = fn == 4 ? 0 : fget32(ft);
      if (sub32(a) || sub32(b)) { fpuUnimpl(); return; }
      if (nan32(a) || nan32(b)) {
        if (snan32(a) || snan32(b)) { if (fpuCheck(F_V)) return; }
        else fcr31 &= ~0x3F000u;
        fset32(fd, QNAN32);
        return;
      }
      volatile float x = f32(a), y = f32(b);
      volatile float r;
      switch (fn) {
        case 0: r = x + y; break;
        case 1: r = x - y; break;
        case 2: r = x * y; break;
        case 3: r = x / y; break;
        default: r = std::sqrt(float(x)); break;
      }
      uint32_t f = hostFlags();
      uint32_t rb = b32(r);
      if (sub32(rb)) {
        if (!(fcr31 & (1u << 24))) { fpuUnimpl(); return; }
        rb &= 0x80000000u; f |= F_U | F_I;
      }
      if (nan32(rb)) rb = QNAN32;
      if (fpuCheck(f)) return;
      fset32(fd, rb);
    } else {
      const uint64_t a = fget64(fs), b = fn == 4 ? 0 : fget64(ft);
      if (sub64(a) || sub64(b)) { fpuUnimpl(); return; }
      if (nan64(a) || nan64(b)) {
        if (snan64(a) || snan64(b)) { if (fpuCheck(F_V)) return; }
        else fcr31 &= ~0x3F000u;
        fset64(fd, QNAN64);
        return;
      }
      volatile double x = f64(a), y = f64(b);
      volatile double r;
      switch (fn) {
        case 0: r = x + y; break;
        case 1: r = x - y; break;
        case 2: r = x * y; break;
        case 3: r = x / y; break;
        default: r = std::sqrt(double(x)); break;
      }
      uint32_t f = hostFlags();
      uint64_t rb = b64(r);
      if (sub64(rb)) {
        if (!(fcr31 & (1u << 24))) { fpuUnimpl(); return; }
        rb &= 0x8000000000000000ull; f |= F_U | F_I;
      }
      if (nan64(rb)) rb = QNAN64;
      if (fpuCheck(f)) return;
      fset64(fd, rb);
    }
    return;
  }
  // conversões para inteiro: ROUND/TRUNC/CEIL/FLOOR (.L 0x08-0x0B, .W 0x0C-0x0F), CVT.W 0x24, CVT.L 0x25
  if ((fn >= 0x08 && fn <= 0x0F) || fn == 0x24 || fn == 0x25) {
    if (!(S || D)) { fpuUnimpl(); return; }
    double x;
    if (S) { const uint32_t a = fget32(fs); if (nan32(a) || sub32(a) || (a & 0x7F800000) == 0x7F800000) { fpuUnimpl(); return; } x = f32(a); }
    else { const uint64_t a = fget64(fs); if (nan64(a) || sub64(a) || (a & 0x7FF0000000000000ull) == 0x7FF0000000000000ull) { fpuUnimpl(); return; } x = f64(a); }
    double r;
    const int modo = fn >= 0x24 ? -1 : (fn & 3);
    switch (modo) {
      case 0: { const int old = std::fegetround(); std::fesetround(FE_TONEAREST); r = std::nearbyint(x); std::fesetround(old); break; }
      case 1: r = std::trunc(x); break;
      case 2: r = std::ceil(x); break;
      case 3: r = std::floor(x); break;
      default: r = std::nearbyint(x); break;
    }
    const bool longo = fn == 0x25 || (fn >= 0x08 && fn <= 0x0B);
    uint32_t f = r != x ? F_I : 0;
    if (longo) {
      if (r >= 9223372036854775808.0 || r < -9223372036854775808.0) { fpuUnimpl(); return; }
      if (fpuCheck(f)) return;
      fset64(fd, uint64_t(int64_t(r)));
    } else {
      if (r >= 2147483648.0 || r < -2147483648.0) { fpuUnimpl(); return; }
      if (fpuCheck(f)) return;
      fset32(fd, uint32_t(int32_t(r)));
    }
    return;
  }
  // CVT.S / CVT.D
  if (fn == 0x20 || fn == 0x21) {
    std::feclearexcept(FE_ALL_EXCEPT);
    const bool paraS = fn == 0x20;
    if ((paraS && S) || (!paraS && D)) { fpuUnimpl(); return; }
    uint32_t f;
    if (paraS) {
      volatile float r;
      if (D) {
        const uint64_t a = fget64(fs);
        if (sub64(a)) { fpuUnimpl(); return; }
        if (nan64(a)) { if (snan64(a) && fpuCheck(F_V)) return; fset32(fd, QNAN32); return; }
        volatile double x = f64(a); r = float(x);
      } else if (W) { volatile int32_t x = int32_t(fget32(fs)); r = float(x); }
      else { volatile int64_t x = int64_t(fget64(fs)); r = float(x); }
      f = hostFlags();
      uint32_t rb = b32(r);
      if (sub32(rb)) { if (!(fcr31 & (1u << 24))) { fpuUnimpl(); return; } rb &= 0x80000000u; f |= F_U | F_I; }
      if (fpuCheck(f)) return;
      fset32(fd, rb);
    } else {
      volatile double r;
      if (S) {
        const uint32_t a = fget32(fs);
        if (sub32(a)) { fpuUnimpl(); return; }
        if (nan32(a)) { if (snan32(a) && fpuCheck(F_V)) return; fset64(fd, QNAN64); return; }
        volatile float x = f32(a); r = double(x);
      } else if (W) { volatile int32_t x = int32_t(fget32(fs)); r = double(x); }
      else { volatile int64_t x = int64_t(fget64(fs)); r = double(x); }
      f = hostFlags();
      if (fpuCheck(f)) return;
      fset64(fd, b64(r));
    }
    return;
  }
  (void)L;
  fpuUnimpl();
}

// --------------------------------------------------------------------------------------- save state
void Vr4300::sync(StateIO& io) {
  io.raw(gpr, sizeof gpr);
  io.sync(hi); io.sync(lo);
  io.raw(fpr, sizeof fpr);
  io.sync(fcr31);
  io.raw(cop0, sizeof cop0);
  for (auto& t : tlb) { io.sync(t.mask); io.sync(t.hi); io.sync(t.lo0); io.sync(t.lo1); io.sync(t.g); }
  io.sync(cycles); io.sync(countBase_);
  io.sync(pc_); io.sync(npc_); io.sync(nextDelay_); io.sync(llbit_); io.sync(compareAt_);
  if (io.reading()) {
    gpr[0] = 0;
    fpage_ = 1;
    fcr31 &= 0x0183FFFF;
    for (auto& t : tlb) { t.mask &= 0x01FFE000; t.lo0 &= 0x3FFFFFFE; t.lo1 &= 0x3FFFFFFE; }
    if (cycles < 0) cycles = 0;
    if (compareAt_ < cycles || compareAt_ > cycles + (int64_t(1) << 34)) scheduleCompare();
    delay_ = false;
    updateIrq();
  }
}

}  // namespace indigo::n64
