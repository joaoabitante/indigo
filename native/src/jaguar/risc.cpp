#include "risc.h"

#include <algorithm>
#include <cstring>

#include "jaguar.h"

namespace indigo::jaguar {

namespace {
constexpr uint32_t IMASK = 1u << 3, REGPAGE = 1u << 14;
inline uint32_t zeroIs32(uint32_t n) { return n ? n : 32; }
inline int64_t sext40(int64_t v) { return (v << 24) >> 24; }
}  // namespace

Risc::Risc(Jaguar& j, bool isDsp)
    : dsp(isDsp), ram(isDsp ? 0x2000 : 0x1000, 0), ramBase(isDsp ? 0xF1B000 : 0xF03000),
      ramMask(isDsp ? 0x1FFF : 0xFFF), j_(j) {}

void Risc::reset() {
  std::memset(r, 0, sizeof r);
  std::fill(ram.begin(), ram.end(), 0);
  pc = ramBase; flags = 0; fz = fc = fn = 0; ctrl = 0; lat = 0;
  hidata = remain = divctrl = mtxc = endian = modulo = 0;
  mtxa = ramBase;
  acc = 0; bank_ = 0; clk = 0; stalls = 0;
  std::memset(ready_, 0, sizeof ready_);
  divFree_ = hiReady_ = 0; extLat_ = 0;
}

void Risc::updateBank() { bank_ = ((flags & REGPAGE) && !(flags & IMASK)) ? 32 : 0; }

void Risc::setIrq(int n) {
  if (!running()) return;  // parado, o processador não registra interrupções
  lat |= 1u << n;
}

uint32_t Risc::enables() const {
  uint32_t e = (flags >> 4) & 0x1F;
  if (dsp && (flags & (1u << 16))) e |= 0x20;
  return e;
}

// entrada de interrupção (o que o hardware faz sozinho): r31 -= 4; guarda o endereço; salta para o vetor
void Risc::checkIrq() {
  if (flags & IMASK) return;
  const uint32_t p = lat & enables();
  if (!p) return;
  int which = 0;
  for (int i = 5; i >= 0; i--) if (p & (1u << i)) { which = i; break; }
  flags |= IMASK;
  updateBank();
  R(31) -= 4;
  store32(R(31) & ~3u, pc - 2);
  pc = ramBase + static_cast<uint32_t>(which) * 16;
  clk += 6;
}

// ------------------------------------------------------------------------------------- registros
uint32_t Risc::regRead(uint32_t off) {
  switch (off & 0x3C) {
    case 0x00: return flags | static_cast<uint32_t>(fz) | (static_cast<uint32_t>(fc) << 1) | (static_cast<uint32_t>(fn) << 2);
    case 0x04: return mtxc;
    case 0x08: return mtxa;
    case 0x0C: return endian;
    case 0x10: return pc;
    case 0x14: return (ctrl & 0x83F) | ((lat & 0x1F) << 6) | ((lat & 0x20) << 11) | 0x2000;
    case 0x18: return dsp ? modulo : hidata;
    case 0x1C: return remain;
    case 0x20: return dsp ? static_cast<uint32_t>((acc >> 32) & 0xFF) : 0;
    default: return 0;
  }
}

void Risc::regWrite(uint32_t off, uint32_t v) {
  switch (off & 0x3C) {
    case 0x00: {
      const uint32_t keep = (v & IMASK) ? (flags & IMASK) : 0;
      fz = v & 1; fc = (v >> 1) & 1; fn = (v >> 2) & 1;
      lat &= ~((v >> 9) & 0x1F);
      if (dsp && (v & (1u << 17))) lat &= ~0x20u;
      flags = (v & (0x1F0u | REGPAGE | 0x8000u | (dsp ? 0x10000u : 0))) | keep;
      updateBank();
      break;
    }
    case 0x04: mtxc = v & 0x1F; break;
    case 0x08: mtxa = ramBase | (v & 0xFFC); break;
    case 0x0C: endian = v; break;
    case 0x10: pc = v & 0xFFFFFE; break;
    case 0x14:
      if (v & 2) j_.riscCpuInt(dsp);   // interrupção para o 68000
      if (v & 4) { const uint32_t c = ctrl; ctrl |= 1; setIrq(0); ctrl = c; }
      ctrl = v & 0x839;
      break;
    case 0x18: if (dsp) modulo = v; else hidata = v; break;
    case 0x1C: divctrl = v; break;
    default: break;
  }
}

void Risc::regWrite16(uint32_t off, uint16_t v, bool lo) {
  const uint32_t o = off & 0x3C;
  if (o == 0x00) {  // bandeiras: a metade de cima só tem a interrupção 5 do DSP
    if (!lo) {
      if (dsp) { flags = (v & 1) ? flags | 0x10000u : flags & ~0x10000u; if (v & 2) lat &= ~0x20u; }
      return;
    }
    regWrite(0, (flags & 0x10000u) | v);
    return;
  }
  if (o == 0x14) { if (lo) regWrite(0x14, v); return; }  // controle: todos os bits na metade de baixo
  const uint32_t cur = o == 0x1C ? divctrl : regRead(o);
  regWrite(o, lo ? ((cur & 0xFFFF0000u) | v) : ((cur & 0xFFFFu) | (uint32_t(v) << 16)));
}

// ------------------------------------------------------------------------------------- memória
// Custo de acesso fora da RAM local (valores aproximados: a documentação pública descreve o comportamento, não
// os números exatos). A DRAM tem ciclos de página de ~2 tiques e troca de página de ~5; a ROM do cartucho é mais
// lenta; os registros do próprio chip respondem logo. O DSP fala com o barramento principal por 16 bits: uma
// palavra de 32 bits são dois acessos.
int Risc::extLatency(uint32_t a, bool w32) {
  a &= 0xFFFFFF;
  int n;
  if (a < 0x800000) n = 6;                                      // DRAM
  else if (a < 0xF00000) n = 10;                                // cartucho / ROM de partida
  else n = (dsp ? (a >= 0xF10000) : (a < 0xF10000)) ? 1 : 4;    // registros do próprio chip ou do outro
  if (dsp && w32 && a < 0xF00000) n *= 2;
  return n;
}
// o GPU e o DSP esperam o barramento principal (memória e cartucho) enquanto o Blitter ou o OP o usam;
// os registros dos chips ficam fora dessa disputa
void Risc::waitBus(uint32_t a) {
  if ((a & 0xFFFFFF) >= 0xF00000) return;
  const int64_t f = j_.busFree();
  if (clk + extra_ < f) extra_ = f - clk;
}
uint16_t Risc::fetch(uint32_t a) {
  if (local(a)) { const uint8_t* p = &ram[(a - ramBase) & ramMask & ~1u]; return uint16_t((p[0] << 8) | p[1]); }
  waitBus(a);
  extra_ += extLatency(a, false);  // sem fila: a busca de fora para o processador
  j_.accessTime = clk + extra_;
  return j_.read16(a & 0xFFFFFE);
}
uint32_t Risc::load32(uint32_t a) {
  if (local(a)) return ramRead32(a);
  waitBus(a);
  extLat_ = extLatency(a, true);
  j_.accessTime = clk + extra_;
  return j_.read32(a & 0xFFFFFC);
}
uint32_t Risc::load16(uint32_t a) {
  if (local(a)) return ramRead32(a);
  waitBus(a);
  extLat_ = extLatency(a, false);
  j_.accessTime = clk + extra_;
  return j_.read16(a & 0xFFFFFE);
}
uint32_t Risc::load8(uint32_t a) {
  if (local(a)) return ramRead32(a);
  waitBus(a);
  extLat_ = extLatency(a, false);
  j_.accessTime = clk + extra_;
  return j_.read8(a & 0xFFFFFF);
}
// escrita de fora: passa pelo buffer de escrita (1 tique; 2 no DSP para 32 bits)
void Risc::store32(uint32_t a, uint32_t v) {
  if (local(a)) { ramWrite32(a, v); return; }
  waitBus(a);
  extra_ += (dsp && (a & 0xFFFFFF) < 0xF00000) ? 2 : 1;
  j_.accessTime = clk + extra_;
  j_.write32(a & 0xFFFFFC, v);
}
void Risc::store16(uint32_t a, uint32_t v) {
  if (local(a)) { ramWrite32(a, v & 0xFFFF); return; }
  waitBus(a);
  extra_ += 1;
  j_.accessTime = clk + extra_;
  j_.write16(a & 0xFFFFFE, static_cast<uint16_t>(v));
}
void Risc::store8(uint32_t a, uint32_t v) {
  if (local(a)) { ramWrite32(a, v & 0xFF); return; }
  waitBus(a);
  extra_ += 1;
  j_.accessTime = clk + extra_;
  j_.write8(a & 0xFFFFFF, static_cast<uint8_t>(v));
}

bool Risc::cond(int cc) const {
  if ((cc & 1) && fz) return false;
  if ((cc & 2) && !fz) return false;
  const int f = (cc & 0x10) ? fn : fc;
  if ((cc & 4) && f) return false;
  if ((cc & 8) && !f) return false;
  return true;
}

// ------------------------------------------------------------------------------------- execução
// placar: antes de executar, espera os registros que a instrução lê (ou escreve) e que ainda não chegaram
void Risc::waitRegs(uint16_t op) {
  const int o = op >> 10, s = (op >> 5) & 31, d = op & 31;
  int64_t need = clk;
  auto use = [&](int idx) { if (ready_[idx] > need) need = ready_[idx]; };
  // campo s com valor imediato (não é registro)
  static constexpr uint64_t IMM = (1ull << 2) | (1ull << 3) | (1ull << 6) | (1ull << 7) | (1ull << 13) | (1ull << 14) |
                                  (1ull << 15) | (1ull << 24) | (1ull << 25) | (1ull << 27) | (1ull << 29) | (1ull << 31) |
                                  (1ull << 35) | (1ull << 38) | (1ull << 43) | (1ull << 44) | (1ull << 49) | (1ull << 50) |
                                  (1ull << 51) | (1ull << 53) | (1ull << 57) | (1ull << 63) | (1ull << 8) | (1ull << 12) |
                                  (1ull << 19) | (1ull << 54);
  bool imm = (IMM >> o) & 1;
  if (dsp && o == 32) imm = true;  // SUBQMOD
  if (!dsp && (o == 32 || o == 33 || o == 62)) imm = true;  // SAT8/SAT16/SAT24 só usam o destino
  if (o == 37) use((bank_ ^ 32) + s);                       // MOVEFA lê o outro banco
  else if (!imm) use(bank_ + s);
  if (o != 52 && o != 53 && o != 57) use(bank_ + d);        // nos desvios, d é a condição
  if (o == 43 || o == 49 || o == 58 || o == 60) use(bank_ + 14);
  if (o == 44 || o == 50 || o == 59 || o == 61) use(bank_ + 15);
  if (o == 21 && divFree_ > need) need = divFree_;          // DIV espera a divisão anterior
  if (!dsp && o == 48 && hiReady_ > need) need = hiReady_;  // STOREP espera o HIDATA do LOADP
  if (need > clk) { stalls += uint64_t(need - clk); clk = need; }
}

void Risc::run(int64_t until) {
  while (clk < until && running()) {
    checkIrq();
    extra_ = 0;
    const uint16_t op = fetch(pc);
    pc += 2;
    waitRegs(op);
    extLat_ = 0;
    j_.accessTime = clk;
    scoreOp_ = op;
    scoreBank_ = bank_;
    exec(op);
    clk += 1 + extra_;
    // a marca no placar vale para a instrução de atraso quando o desvio é tomado
    const uint16_t sop = scoreOp_;
    const int o = sop >> 10, bank = scoreBank_;
    // LOAD de fora: o registro chega depois; o processador segue (placar)
    if (extLat_ && ((o >= 39 && o <= 41) || o == 43 || o == 44 || o == 58 || o == 59 || (o == 42 && !dsp))) {
      ready_[bank + (sop & 31)] = clk + extLat_;
      if (o == 42) hiReady_ = clk + extLat_;  // LOADP: o HIDATA chega junto
    } else if (o == 21) { ready_[bank + (sop & 31)] = clk + 16; divFree_ = clk + 16; }  // DIV: 16 tiques a mais
  }
  if (clk < until) clk = until;
}

void Risc::exec(uint16_t op) {
  const int s = (op >> 5) & 31, d = op & 31;
  uint32_t& rn = R(d);
  const uint32_t rm = R(s);
  switch (op >> 10) {
    case 0: { const uint32_t a = rn, res = a + rm; fc = rm > ~a; rn = res; zn(res); break; }           // ADD
    case 1: { const uint64_t res = uint64_t(rn) + rm + fc; fc = (res >> 32) & 1; rn = uint32_t(res); zn(rn); break; }  // ADDC
    case 2: { const uint32_t a = rn, b = zeroIs32(s), res = a + b; fc = b > ~a; rn = res; zn(res); break; }  // ADDQ
    case 3: rn += zeroIs32(s); break;                                                                  // ADDQT
    case 4: { const uint32_t a = rn, res = a - rm; fc = rm > a; rn = res; zn(res); break; }             // SUB
    case 5: {                                                                                          // SUBC
      const uint64_t res = uint64_t(rn) + uint64_t(rm ^ 0xFFFFFFFFu) + (fc ^ 1);
      fc = ((res >> 32) & 1) ^ 1; rn = uint32_t(res); zn(rn); break;
    }
    case 6: { const uint32_t a = rn, b = zeroIs32(s), res = a - b; fc = b > a; rn = res; zn(res); break; }  // SUBQ
    case 7: rn -= zeroIs32(s); break;                                                                  // SUBQT
    case 8: { const uint32_t a = rn, res = 0u - a; fc = a != 0; rn = res; zn(res); break; }             // NEG
    case 9: rn &= rm; zn(rn); break;                                                                   // AND
    case 10: rn |= rm; zn(rn); break;                                                                  // OR
    case 11: rn ^= rm; zn(rn); break;                                                                  // XOR
    case 12: rn = ~rn; zn(rn); break;                                                                  // NOT
    case 13: fz = !((rn >> s) & 1); break;                                                             // BTST
    case 14: rn |= 1u << s; zn(rn); break;                                                             // BSET
    case 15: rn &= ~(1u << s); zn(rn); break;                                                          // BCLR
    case 16: rn = uint32_t(uint16_t(rm)) * uint16_t(rn); zn(rn); break;                               // MULT
    case 17: rn = uint32_t(int32_t(int16_t(rn)) * int16_t(rm)); zn(rn); break;                        // IMULT
    case 18: { const int32_t v = int32_t(int16_t(rn)) * int16_t(rm); acc = v; zn(uint32_t(v)); break; }  // IMULTN
    case 19: rn = uint32_t(acc); break;                                                                // RESMAC
    case 20: {                                                                                         // IMACN
      const int32_t v = int32_t(int16_t(rm)) * int16_t(rn);
      acc = dsp ? sext40(acc + v) : int64_t(int32_t(uint32_t(acc) + uint32_t(v)));
      break;
    }
    case 21: {                                                                                         // DIV
      const uint32_t b = rm;
      const uint64_t a = (divctrl & 1) ? uint64_t(rn) << 16 : uint64_t(rn);
      if (b == 0) { rn = 0xFFFFFFFFu; remain = uint32_t(a); }
      else { const uint64_t q = a / b; remain = uint32_t(a - q * b); rn = uint32_t(q); }
      break;
    }
    case 22: {                                                                                         // ABS
      fc = rn >> 31;
      if (rn == 0x80000000u) { fn = 1; fz = 0; }
      else { if (fc) rn = 0u - rn; fn = 0; fz = rn == 0; }
      break;
    }
    case 23: {                                                                                         // SH
      const int32_t n = int32_t(rm);
      if (n < 0) { fc = rn >> 31; rn = n <= -32 ? 0 : rn << -n; }
      else { fc = rn & 1; rn = n >= 32 ? 0 : rn >> n; }
      zn(rn); break;
    }
    case 24: { const int n = 32 - s; fc = rn >> 31; rn = n >= 32 ? 0 : rn << n; zn(rn); break; }      // SHLQ
    case 25: { const uint32_t n = zeroIs32(s); fc = rn & 1; rn = n >= 32 ? 0 : rn >> n; zn(rn); break; }  // SHRQ
    case 26: {                                                                                         // SHA
      const int32_t n = int32_t(rm);
      if (n < 0) { fc = rn >> 31; rn = n <= -32 ? 0 : rn << -n; }
      else { fc = rn & 1; rn = uint32_t(n >= 32 ? int32_t(rn) >> 31 : int32_t(rn) >> n); }
      zn(rn); break;
    }
    case 27: { const uint32_t n = zeroIs32(s); fc = rn & 1; rn = uint32_t(int32_t(rn) >> (n >= 32 ? 31 : n)); zn(rn); break; }  // SHARQ
    case 28: { const uint32_t n = rm & 31, v = rn; rn = n ? (v >> n) | (v << (32 - n)) : v; fc = v >> 31; zn(rn); break; }  // ROR
    case 29: { const uint32_t n = s & 31, v = rn; rn = n ? (v >> n) | (v << (32 - n)) : v; fc = v >> 31; zn(rn); break; }   // RORQ
    case 30: { const uint32_t a = rn, res = a - rm; fc = rm > a; zn(res); break; }                     // CMP
    case 31: { const uint32_t b = uint32_t(int32_t(s << 27) >> 27), a = rn, res = a - b; fc = b > a; zn(res); break; }  // CMPQ
    case 32:
      if (dsp) { const uint32_t a = rn, b = zeroIs32(s), res = ((a - b) & ~modulo) | (a & modulo); rn = res; fc = b > a; zn(res); }  // SUBQMOD
      else { rn = int32_t(rn) < 0 ? 0 : (rn > 0xFF ? 0xFF : rn); zn(rn); }                             // SAT8
      break;
    case 33:
      if (dsp) { const int32_t v = int32_t(rn); rn = uint32_t(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }  // SAT16S
      else rn = int32_t(rn) < 0 ? 0 : (rn > 0xFFFF ? 0xFFFF : rn);                                    // SAT16
      zn(rn); break;
    case 34: rn = rm; break;                                                                           // MOVE
    case 35: rn = uint32_t(s); break;                                                                  // MOVEQ
    case 36: r[(bank_ ^ 32) + d] = rm; break;                                                          // MOVETA
    case 37: rn = r[(bank_ ^ 32) + s]; break;                                                          // MOVEFA
    case 38: { const uint32_t lo = fetch(pc), hi = fetch(pc + 2); pc += 4; rn = lo | (hi << 16); break; }  // MOVEI
    case 39: rn = load8(rm); break;                                                                    // LOADB
    case 40: rn = load16(rm); break;                                                                   // LOADW
    case 41: rn = load32(rm); break;                                                                   // LOAD
    case 42:
      if (dsp) {                                                                                       // SAT32S
        const int64_t t = sext40(acc) >> 32;
        rn = t < -1 ? 0x80000000u : (t > 0 ? 0x7FFFFFFFu : rn); zn(rn);
      } else { hidata = load32(rm & ~7u); rn = load32((rm & ~7u) + 4); }                              // LOADP
      break;
    case 43: rn = load32(R(14) + zeroIs32(s) * 4); break;                                              // LOAD (R14+n)
    case 44: rn = load32(R(15) + zeroIs32(s) * 4); break;                                              // LOAD (R15+n)
    case 45: store8(rm, rn); break;                                                                    // STOREB
    case 46: store16(rm, rn); break;                                                                   // STOREW
    case 47: store32(rm, rn); break;                                                                   // STORE
    case 48:
      if (dsp) {                                                                                       // MIRROR
        uint32_t v = rn, m = 0;
        for (int i = 0; i < 32; i++) { m = (m << 1) | (v & 1); v >>= 1; }
        rn = m; zn(rn);
      } else { store32(rm & ~7u, hidata); store32((rm & ~7u) + 4, rn); }                              // STOREP
      break;
    case 49: store32(R(14) + zeroIs32(s) * 4, rn); break;                                              // STORE (R14+n)
    case 50: store32(R(15) + zeroIs32(s) * 4, rn); break;                                              // STORE (R15+n)
    case 51: rn = pc - 2; break;                                                                       // MOVE PC
    case 52:                                                                                           // JUMP
    case 53: {                                                                                         // JR
      if (!cond(d)) break;
      const uint32_t to = (op >> 10) == 52 ? rm : pc + uint32_t(int32_t(s << 27) >> 26);
      // desvio dentro da instrução de atraso: sem nova recursão (vale o desvio de fora)
      if (inDelay_) break;
      // instrução de atraso: roda antes do desvio
      const uint16_t ds = fetch(pc);
      pc += 2;
      // a instrução de atraso também passa pelo placar e marca o que carregar
      waitRegs(ds);
      extLat_ = 0;
      scoreOp_ = ds;
      scoreBank_ = bank_;
      j_.accessTime = clk + extra_;
      inDelay_ = true;
      exec(ds);
      inDelay_ = false;
      extra_ += 2;  // instrução de atraso + fila esvaziada pelo desvio tomado
      pc = to & 0xFFFFFE;
      break;
    }
    case 54: {                                                                                         // MMULT
      const int count = mtxc & 0xF;
      uint32_t a = mtxa;
      int64_t sum = 0;
      for (int i = 0; i < count; i++) {
        const uint32_t w = r[(bank_ ^ 32) + ((s + (i >> 1)) & 31)];  // banco alternativo
        const int16_t x = int16_t((i & 1) ? w >> 16 : w);
        const int16_t y = int16_t(local(a) ? ramRead32(a) : 0);
        sum += int32_t(x) * y;
        a = ramBase | ((a + ((mtxc & 0x10) ? 4u * count : 4u)) & 0xFFC);
      }
      mtxa = a;
      rn = uint32_t(int32_t(sum)); zn(rn);
      extra_ += count;
      break;
    }
    case 55: rn = (uint32_t(int32_t(rm) >> 8) & 0xFF800000u) | (rm & 0x007FFFFFu); zn(rn); break;    // MTOI
    case 56: {                                                                                         // NORMI
      uint32_t v = rm, res = uint32_t(-32);
      if (v) { res = uint32_t(-23); while ((v >>= 1) != 0) res++; }
      rn = res; zn(res); break;
    }
    case 57: break;                                                                                    // NOP
    case 58: rn = load32(R(14) + rm); break;                                                           // LOAD (R14+Rm)
    case 59: rn = load32(R(15) + rm); break;                                                           // LOAD (R15+Rm)
    case 60: store32(R(14) + rm, rn); break;                                                           // STORE (R14+Rm)
    case 61: store32(R(15) + rm, rn); break;                                                           // STORE (R15+Rm)
    case 62: if (!dsp) { rn = int32_t(rn) < 0 ? 0 : (rn > 0xFFFFFF ? 0xFFFFFF : rn); zn(rn); } break; // SAT24
    case 63:
      if (dsp) { const uint32_t a = rn, b = zeroIs32(s), res = ((a + b) & ~modulo) | (a & modulo); rn = res; fc = b > ~a; zn(res); }  // ADDQMOD
      else if (s == 0) rn = ((rn >> 10) & 0xF000) | ((rn >> 5) & 0x0F00) | (rn & 0xFF);              // PACK
      else rn = ((rn & 0xF000) << 10) | ((rn & 0x0F00) << 5) | (rn & 0xFF);                            // UNPACK
      break;
  }
}

void Risc::rebase(int64_t lo, int64_t hi) {
  const int64_t c = std::clamp(clk, lo, hi), d = c - clk;
  clk = c;
  for (int64_t& t : ready_) t += d;
  divFree_ += d; hiReady_ += d;
}

void Risc::sync(StateIO& io) {
  io.sync(clk);
  io.raw(r, sizeof r);
  io.sync(pc); io.sync(flags); io.sync(fz); io.sync(fc); io.sync(fn); io.sync(ctrl); io.sync(lat);
  io.sync(hidata); io.sync(remain); io.sync(divctrl); io.sync(mtxc); io.sync(mtxa); io.sync(endian); io.sync(modulo);
  io.sync(acc);
  io.bytes(ram);
  // placar guardado como distância ao relógio
  for (int i = 0; i < 64; i++) { int32_t d = int32_t(ready_[i] > clk ? ready_[i] - clk : 0); io.sync(d); ready_[i] = clk + std::clamp(d, 0, 64); }
  { int32_t d = int32_t(divFree_ > clk ? divFree_ - clk : 0); io.sync(d); divFree_ = clk + std::clamp(d, 0, 64); }
  { int32_t d = int32_t(hiReady_ > clk ? hiReady_ - clk : 0); io.sync(d); hiReady_ = clk + std::clamp(d, 0, 64); }
  if (io.reading()) {
    fz &= 1; fc &= 1; fn &= 1;
    mtxa = ramBase | (mtxa & 0xFFC);
    updateBank();
  }
}

}  // namespace indigo::jaguar
