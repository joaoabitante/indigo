// Implementação da CPU V810 (veja v810.h). Incluída só pelos .cpp que instanciam a CPU de um console.
#pragma once
#include <cmath>
#include <cstring>

#include "v810.h"

namespace indigo::v810 {


namespace {
// tipo da instrução anterior, para o custo das cargas e gravações seguidas (o V810 sobrepõe acessos)
enum { LO_LONG = -1, LO_OTHER = 0, LO_LD = 1, LO_ST = 2, LO_IN = 3, LO_OUT = 4 };

inline int32_t sext(uint32_t v, int bits) { return static_cast<int32_t>(v << (32 - bits)) >> (32 - bits); }
inline float asFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
inline uint32_t asBits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

constexpr uint32_t FPU_HANDLER = 0xFFFFFF60;
}  // namespace

template <class Bus>
void Cpu<Bus>::reset() {
  std::memset(r, 0, sizeof r);
  std::memset(sr, 0, sizeof sr);
  pc = 0xFFFFFFF0;
  sr[ECR] = 0x0000FFF0;
  sr[PSW] = F_NP;
  sr[PIR] = Bus::V810_PIR;  // 0x5346 ("NVC") no Virtual Boy, 0x8100 no PC-FX
  sr[TKCW] = 0x000000E0;
  halt = 0;
  lastOp = LO_OTHER;
  std::memset(cacheTag_, 0, sizeof cacheTag_);
  std::memset(cacheValid_, 0, sizeof cacheValid_);
}

// busca de meia palavra de instrução: com o cache ligado, um acerto não custa nada e uma falta lê 32 bits
// pelo barramento de 16 bits; sem cache, a segunda meia palavra de uma instrução longa custa 2 ciclos
template <class Bus>
uint16_t Cpu<Bus>::fetch(uint32_t a, bool second) {
  if (sr[CHCW] & 2) {
    const int idx = (a >> 3) & 127, sub = 1 << ((a >> 2) & 1);
    const uint32_t tag = a >> 10;
    if (cacheTag_[idx] != tag) {
      cacheTag_[idx] = tag;
      cacheValid_[idx] = static_cast<uint8_t>(sub);
      bus_.ts += 3;
    } else if (!(cacheValid_[idx] & sub)) {
      cacheValid_[idx] |= sub;
      bus_.ts += 3;
    }
  } else if (second) {
    bus_.ts += 2;
  }
  return bus_.fetch16(a & ~1u);
}

template <class Bus>
bool Cpu<Bus>::cond(int c) const {
  const uint32_t p = sr[PSW];
  const bool z = p & F_Z, s = p & F_S, ov = p & F_OV, cy = p & F_CY;
  bool v;
  switch (c & 7) {
    case 0: v = ov; break;               // V
    case 1: v = cy; break;               // C / L
    case 2: v = z; break;                // Z / E
    case 3: v = cy || z; break;          // NH
    case 4: v = s; break;                // N
    case 5: v = true; break;             // T
    case 6: v = s != ov; break;          // LT
    default: v = (s != ov) || z; break;  // LE
  }
  return (c & 8) ? !v : v;
}

template <class Bus>
uint32_t Cpu<Bus>::add(uint32_t a, uint32_t b) {
  const uint32_t res = a + b;
  flag(F_OV, ((a ^ ~b) & (a ^ res)) >> 31);
  flag(F_CY, res < a);
  setSZ(res);
  return res;
}

template <class Bus>
uint32_t Cpu<Bus>::sub(uint32_t a, uint32_t b) {
  const uint32_t res = a - b;
  flag(F_OV, ((a ^ b) & (a ^ res)) >> 31);
  flag(F_CY, b > a);
  setSZ(res);
  return res;
}

// exceção: o PC atual é o endereço de retorno (quem chama ajusta antes)
template <class Bus>
void Cpu<Bus>::exception(uint32_t handler, uint16_t code) {
  uint32_t& psw = sr[PSW];
  if (psw & F_NP) {  // exceção durante o tratamento de uma exceção dupla: o processador para
    halt = 2;
    return;
  }
  if (psw & F_EP) {  // exceção dupla
    sr[FEPC] = pc;
    sr[FEPSW] = psw;
    sr[ECR] = (sr[ECR] & 0xFFFF) | (static_cast<uint32_t>(code) << 16);
    psw = (psw | F_NP | F_ID) & ~F_AE;
    pc = 0xFFFFFFD0;
    return;
  }
  sr[EIPC] = pc;
  sr[EIPSW] = psw;
  sr[ECR] = (sr[ECR] & 0xFFFF0000) | code;
  psw = (psw | F_EP | F_ID) & ~F_AE;
  pc = handler;
}

template <class Bus>
void Cpu<Bus>::interrupt() {
  const int lv = irqLevel;
  uint32_t& psw = sr[PSW];
  sr[EIPC] = pc;
  sr[EIPSW] = psw;
  sr[ECR] = (sr[ECR] & 0xFFFF0000) | (0xFE00u | (lv << 4));
  const uint32_t next = lv + 1 > 15 ? 15 : lv + 1;
  psw = ((psw | F_EP | F_ID) & ~F_AE & ~0xF0000u) | (next << 16);
  pc = 0xFFFFFE00u | (lv << 4);
  halt = 0;
}

template <class Bus>
void Cpu<Bus>::ldsr(int reg, uint32_t v) {
  switch (reg) {
    case EIPC: case FEPC: case ADTRE: sr[reg] = v & ~1u; break;
    case EIPSW: case FEPSW: sr[reg] = v & 0xFF3FF; break;
    case PSW: sr[PSW] = v & 0xFF3FF; break;
    case CHCW: sr[CHCW] = v & 2; cacheControl(v); break;
    default: break;  // ECR, PIR e TKCW só leitura; os demais não existem
  }
}

// CHCW: limpar (ICC), despejar (ICD) ou restaurar (ICR) o cache. Guardamos só as etiquetas, então o despejo
// grava os dados como zero; a restauração lê as etiquetas de volta.
template <class Bus>
void Cpu<Bus>::cacheControl(uint32_t v) {
  switch (v & 0x31) {
    case 0x01: {
      const uint32_t start = (v >> 20) & 0xFFF, count = (v >> 8) & 0xFFF;
      for (uint32_t i = 0; i < count && i + start < 128; i++) { cacheTag_[i + start] = 0; cacheValid_[i + start] = 0; }
      break;
    }
    case 0x10: {
      const uint32_t sa = v & ~0xFFu;
      for (int i = 0; i < 128; i++) {
        bus_.write32(sa + i * 8, 0);
        bus_.write32(sa + i * 8 + 4, 0);
      }
      for (int i = 0; i < 128; i++)
        bus_.write32(sa + 1024 + i * 4, (cacheTag_[i] & 0x3FFFFF) | (static_cast<uint32_t>(cacheValid_[i] & 3) << 22));
      bus_.ts += 384 * 4;
      break;
    }
    case 0x20: {
      const uint32_t sa = v & ~0xFFu;
      for (int i = 0; i < 128; i++) {
        const uint32_t t = bus_.read32(sa + 1024 + i * 4);
        cacheTag_[i] = t & 0x3FFFFF;
        cacheValid_[i] = static_cast<uint8_t>((t >> 22) & 3);
      }
      bus_.ts += 384 * 4;
      break;
    }
    default: break;
  }
}

// instruções de cadeia de bits. Registros: r30 = endereço da origem, r27 = bit inicial da origem,
// r29 = endereço do destino (nas operações) ou bits pulados (nas buscas), r26 = bit do destino, r28 = tamanho.
// A instrução é interrompível: ao alcançar o limite de tempo no fim de uma palavra, os registros guardam o
// progresso e ela é executada de novo (devolve true).
template <class Bus>
bool Cpu<Bus>::bitString(int sub) {
  int64_t& ts = bus_.ts;
  if (sub & 8) {
    uint32_t dstOff = r[26] & 31, srcOff = r[27] & 31, len = r[28];
    uint32_t dst = r[29] & ~3u, src = r[30] & ~3u;
    uint32_t sc = 0, dc = 0;
    bool haveSrc = false, haveDst = false;
    while (len) {
      if (!haveSrc) { sc = bus_.read32(src); ts += 4; haveSrc = true; }
      if (!haveDst) { dc = bus_.read32(dst); ts += 4; haveDst = true; }
      const uint32_t bit = (sc >> srcOff) & 1, m = 1u << dstOff;
      switch (sub & 7) {
        case 0: if (bit) dc |= m; break;                    // ORBSU
        case 1: if (!bit) dc &= ~m; break;                  // ANDBSU
        case 2: if (bit) dc ^= m; break;                    // XORBSU
        case 3: dc = bit ? (dc | m) : (dc & ~m); break;     // MOVBSU
        case 4: if (!bit) dc |= m; break;                   // ORNBSU
        case 5: if (bit) dc &= ~m; break;                   // ANDNBSU
        case 6: if (!bit) dc ^= m; break;                   // XORNBSU
        default: dc = bit ? (dc & ~m) : (dc | m); break;    // NOTBSU
      }
      srcOff = (srcOff + 1) & 31;
      dstOff = (dstOff + 1) & 31;
      len--;
      if (!srcOff) { src += 4; haveSrc = false; }
      if (!dstOff) {
        bus_.write32(dst, dc);
        ts += 4;
        dst += 4;
        haveDst = false;
        if (ts >= bus_.runUntil) break;
      }
    }
    if (haveDst) { bus_.write32(dst, dc); ts += 4; }
    r[26] = dstOff; r[27] = srcOff; r[28] = len; r[29] = dst; r[30] = src;
    return len != 0;
  }
  // buscas: SCH0BSU (0), SCH0BSD (1), SCH1BSU (2), SCH1BSD (3)
  const bool down = sub & 1;
  const uint32_t want = (sub >> 1) & 1;
  uint32_t off = r[27] & 31, len = r[28], skipped = r[29], src = r[30] & ~3u;
  uint32_t sc = 0;
  bool have = false, found = false;
  while (len) {
    if (!have) { ts += 1; sc = bus_.read32(src); ts += 4; have = true; }
    if (((sc >> off) & 1) == want) {
      found = true;
      // os registros apontam para o bit anterior ao encontrado (no sentido da busca)
      if (down) { if (off == 31) { off = 0; src += 4; } else off++; }
      else { if (off == 0) { off = 31; src -= 4; } else off--; }
      break;
    }
    skipped++;
    len--;
    if (down) {
      if (off == 0) { off = 31; src -= 4; have = false; if (ts >= bus_.runUntil) break; }
      else off--;
    } else {
      off = (off + 1) & 31;
      if (!off) { src += 4; have = false; if (ts >= bus_.runUntil) break; }
    }
  }
  r[27] = off; r[28] = len; r[29] = skipped; r[30] = src;
  if (found) flag(F_Z, false);
  else if (!len) flag(F_Z, true);
  return !found && len != 0;
}

// operando reservado (NaN, infinito ou desnormalizado) gera a exceção FRO
template <class Bus>
bool Cpu<Bus>::fpuReserved(uint32_t a, uint32_t b) {
  for (uint32_t v : {a, b}) {
    if (!(v & 0x7FFFFFFF)) continue;
    const uint32_t e = (v >> 23) & 0xFF;
    if (e == 0 || e == 0xFF) {
      sr[PSW] |= F_FRO;
      pc -= 4;
      exception(FPU_HANDLER, 0xFF60);
      return true;
    }
  }
  return false;
}

// grava o resultado de uma operação de ponto flutuante com as bandeiras do PSW. Resultado desnormalizado vira
// zero com o mesmo sinal (FUD + FPR); estouro grava o infinito e gera a exceção FOV.
template <class Bus>
void Cpu<Bus>::fpuResult(int reg2, float f, bool inexact, bool exactNonZero) {
  uint32_t bits = asBits(f);
  bool under = false;
  if ((bits & 0x7F800000) == 0 && (bits & 0x7FFFFF)) { under = true; inexact = true; bits &= 0x80000000; }
  else if (!(bits & 0x7FFFFFFF) && exactNonZero) { under = true; inexact = true; }
  const bool over = (bits & 0x7FFFFFFF) == 0x7F800000;
  if (over) inexact = true;
  uint32_t& psw = sr[PSW];
  psw &= ~(F_Z | F_S | F_CY | F_OV);
  if (!(bits & 0x7FFFFFFF)) psw |= F_Z;
  else if (bits & 0x80000000) psw |= F_S | F_CY;
  r[reg2] = bits;
  if (under) psw |= F_FUD;
  if (inexact) psw |= F_FPR;
  if (over) {
    psw |= F_FOV;
    pc -= 4;
    exception(FPU_HANDLER, 0xFF64);
  }
}

// formato VII: unidade de ponto flutuante e as instruções próprias do Virtual Boy (reg2 é o destino)
template <class Bus>
void Cpu<Bus>::fpu(int sub, int reg2, int reg1) {
  int64_t& ts = bus_.ts;
  const uint32_t a = r[reg2], b = r[reg1];
  if (!Bus::V810_NINTENDO && (sub == 0x08 || sub == 0x09 || sub == 0x0A || sub == 0x0C)) {
    pc -= 4;  // XB, XH, REV e MPYHW são do Virtual Boy: no PC-FX, código inválido
    exception(0xFFFFFF90, 0xFF90);
    return;
  }
  switch (sub) {
    case 0x00: {  // CMPF.S reg1, reg2: compara reg2 com reg1
      ts += 6;
      if (fpuReserved(a, b)) return;
      const float fa = asFloat(a), fb = asFloat(b);
      uint32_t& psw = sr[PSW];
      psw &= ~(F_Z | F_S | F_CY | F_OV);
      if (fa == fb) psw |= F_Z;
      else if (fa < fb) psw |= F_S | F_CY;
      return;
    }
    case 0x02: {  // CVT.WS: inteiro -> real
      ts += 5;
      const int32_t i = static_cast<int32_t>(b);
      const float f = static_cast<float>(i);
      fpuResult(reg2, f, static_cast<double>(f) != static_cast<double>(i), false);
      return;
    }
    case 0x03: case 0x0B: {  // CVT.SW (arredonda para o par mais próximo) e TRNC.SW (trunca)
      ts += sub == 0x03 ? 8 : 7;
      if (fpuReserved(b, 0)) return;
      const double v = asFloat(b);
      const double rr = sub == 0x03 ? std::nearbyint(v) : std::trunc(v);
      if (rr >= 2147483648.0 || rr < -2147483648.0) {
        sr[PSW] |= F_FIV;
        pc -= 4;
        exception(FPU_HANDLER, 0xFF70);
        return;
      }
      const uint32_t res = static_cast<uint32_t>(static_cast<int32_t>(rr));
      r[reg2] = res;
      flag(F_OV, false);
      setSZ(res);
      if (rr != v) sr[PSW] |= F_FPR;
      return;
    }
    case 0x04: case 0x05: {  // ADDF.S / SUBF.S
      ts += sub == 0x04 ? 8 : 11;
      if (fpuReserved(a, b)) return;
      const double x = asFloat(a), y = sub == 0x04 ? static_cast<double>(asFloat(b)) : -static_cast<double>(asFloat(b));
      const double s = x + y;
      // soma exata = s + err (algoritmo TwoSum); o arredondamento duplo double->float é inofensivo na soma
      const double bb = s - x;
      const double err = (x - (s - bb)) + (y - bb);
      const float f = static_cast<float>(s);
      fpuResult(reg2, f, err != 0 || static_cast<double>(f) != s, s != 0 || err != 0);
      return;
    }
    case 0x06: {  // MULF.S: o produto de dois floats é exato em double
      ts += 7;
      if (fpuReserved(a, b)) return;
      const double d = static_cast<double>(asFloat(a)) * static_cast<double>(asFloat(b));
      const float f = static_cast<float>(d);
      fpuResult(reg2, f, static_cast<double>(f) != d, d != 0);
      return;
    }
    case 0x07: {  // DIVF.S reg1, reg2: reg2 / reg1
      ts += 43;
      if (fpuReserved(a, b)) return;
      if (!(b & 0x7FFFFFFF)) {
        if (!(a & 0x7FFFFFFF)) { sr[PSW] |= F_FIV; pc -= 4; exception(FPU_HANDLER, 0xFF70); }
        else { sr[PSW] |= F_FZD; pc -= 4; exception(FPU_HANDLER, 0xFF68); }
        return;
      }
      const double d = static_cast<double>(asFloat(a)) / static_cast<double>(asFloat(b));
      const float f = static_cast<float>(d);
      fpuResult(reg2, f, static_cast<double>(f) != d, (a & 0x7FFFFFFF) != 0);
      return;
    }
    case 0x08:  // XB: troca os dois bytes de baixo
      ts += 1;
      r[reg2] = (a & 0xFFFF0000) | ((a & 0xFF) << 8) | ((a >> 8) & 0xFF);
      return;
    case 0x09:  // XH: troca as meias palavras
      ts += 1;
      r[reg2] = (a << 16) | (a >> 16);
      return;
    case 0x0A: {  // REV: inverte a ordem dos bits de reg1
      ts += 1;
      uint32_t v = b;
      v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
      v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
      v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
      v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
      r[reg2] = (v >> 16) | (v << 16);
      return;
    }
    case 0x0C:  // MPYHW: reg2 *= reg1 (17 bits com sinal)
      ts += 8;
      r[reg2] = a * static_cast<uint32_t>(sext(b & 0x1FFFF, 17));
      return;
    default:
      pc -= 4;
      exception(0xFFFFFF90, 0xFF90);
      return;
  }
}

template <class Bus>
void Cpu<Bus>::step() {
  int64_t& ts = bus_.ts;
  const uint32_t at = pc;
  const uint32_t op = fetch(at, false);
  const int opc = static_cast<int>(op >> 10);
  const int r1 = op & 31, r2 = (op >> 5) & 31;
  int8_t lo = LO_OTHER;

  // alvo de desvio desalinhado em 32 bits custa um ciclo a mais com o cache ligado
  auto branchTo = [&](uint32_t target) {
    pc = target & ~1u;
    ts += 3;
    if ((sr[CHCW] & 2) && (pc & 2)) ts += 1;
  };
  auto loadCost = [&](int extraLd, int extraOther) {
    if (lastOp >= 0) ts += lastOp == LO_LD ? extraLd : extraOther;
    lo = LO_LD;
  };

  switch (opc) {
    // ---------------------------------------------------------------- formato I (registro, registro)
    case 0x00: r[r2] = r[r1]; pc += 2; ts += 1; break;                    // MOV
    case 0x01: r[r2] = add(r[r2], r[r1]); pc += 2; ts += 1; break;        // ADD
    case 0x02: r[r2] = sub(r[r2], r[r1]); pc += 2; ts += 1; break;        // SUB
    case 0x03: sub(r[r2], r[r1]); pc += 2; ts += 1; break;                // CMP
    case 0x04: case 0x05: case 0x07: case 0x14: case 0x15: case 0x17: {  // SHL / SHR / SAR (reg e imediato)
      const uint32_t n = (opc & 0x10) ? static_cast<uint32_t>(r1) : (r[r1] & 31);
      const uint32_t v = r[r2];
      uint32_t res;
      bool cy;
      switch (opc & 0x0F) {
        case 0x04: res = v << n; cy = n && ((v >> (32 - n)) & 1); break;
        case 0x05: res = v >> n; cy = n && ((v >> (n - 1)) & 1); break;
        default: res = static_cast<uint32_t>(static_cast<int32_t>(v) >> n); cy = n && ((v >> (n - 1)) & 1); break;
      }
      flag(F_CY, cy);
      flag(F_OV, false);
      setSZ(res);
      r[r2] = res;
      pc += 2; ts += 1;
      break;
    }
    case 0x06: branchTo(r[r1]); break;  // JMP [reg1]
    case 0x08: {  // MUL
      const int64_t p = static_cast<int64_t>(static_cast<int32_t>(r[r1])) * static_cast<int32_t>(r[r2]);
      const uint32_t lo32 = static_cast<uint32_t>(p);
      r[30] = static_cast<uint32_t>(static_cast<uint64_t>(p) >> 32);
      r[r2] = lo32;
      setSZ(lo32);
      flag(F_OV, p != static_cast<int64_t>(static_cast<int32_t>(lo32)));
      pc += 2; ts += 13; lo = LO_LONG;
      break;
    }
    case 0x0A: {  // MULU
      const uint64_t p = static_cast<uint64_t>(r[r1]) * r[r2];
      const uint32_t lo32 = static_cast<uint32_t>(p);
      r[30] = static_cast<uint32_t>(p >> 32);
      r[r2] = lo32;
      setSZ(lo32);
      flag(F_OV, (p >> 32) != 0);
      pc += 2; ts += 13; lo = LO_LONG;
      break;
    }
    case 0x09: case 0x0B: {  // DIV / DIVU (resto em r30)
      ts += opc == 0x09 ? 38 : 36;
      lo = LO_LONG;
      const uint32_t d = r[r1], n = r[r2];
      if (!d) { exception(0xFFFFFF80, 0xFF80); break; }  // o PC de retorno é a própria instrução
      uint32_t q, rem;
      if (opc == 0x0B) { q = n / d; rem = n % d; }
      else if (n == 0x80000000u && d == 0xFFFFFFFFu) { q = 0x80000000u; rem = 0; }
      else {
        q = static_cast<uint32_t>(static_cast<int32_t>(n) / static_cast<int32_t>(d));
        rem = static_cast<uint32_t>(static_cast<int32_t>(n) % static_cast<int32_t>(d));
      }
      r[30] = rem;
      r[r2] = q;
      flag(F_OV, opc == 0x09 && n == 0x80000000u && d == 0xFFFFFFFFu);
      setSZ(q);
      pc += 2;
      break;
    }
    case 0x0C: r[r2] |= r[r1]; flag(F_OV, false); setSZ(r[r2]); pc += 2; ts += 1; break;   // OR
    case 0x0D: r[r2] &= r[r1]; flag(F_OV, false); setSZ(r[r2]); pc += 2; ts += 1; break;   // AND
    case 0x0E: r[r2] ^= r[r1]; flag(F_OV, false); setSZ(r[r2]); pc += 2; ts += 1; break;   // XOR
    case 0x0F: r[r2] = ~r[r1]; flag(F_OV, false); setSZ(r[r2]); pc += 2; ts += 1; break;   // NOT
    // ---------------------------------------------------------------- formato II (imediato de 5 bits)
    case 0x10: r[r2] = static_cast<uint32_t>(sext(r1, 5)); pc += 2; ts += 1; break;              // MOV imm
    case 0x11: r[r2] = add(r[r2], static_cast<uint32_t>(sext(r1, 5))); pc += 2; ts += 1; break;  // ADD imm
    case 0x12: r[r2] = cond(r1 & 15) ? 1 : 0; pc += 2; ts += 1; break;                           // SETF
    case 0x13: sub(r[r2], static_cast<uint32_t>(sext(r1, 5))); pc += 2; ts += 1; break;          // CMP imm
    case 0x16: sr[PSW] &= ~F_ID; pc += 2; ts += 1; break;                                         // CLI
    case 0x1E: sr[PSW] |= F_ID; pc += 2; ts += 1; break;                                          // SEI
    case 0x18:  // TRAP vetor
      pc += 2; ts += 15;
      exception(0xFFFFFFA0u + (r1 & 0x10), static_cast<uint16_t>(0xFFA0 + r1));
      break;
    case 0x19: {  // RETI
      ts += 10;
      if (sr[PSW] & F_NP) { pc = sr[FEPC] & ~1u; sr[PSW] = sr[FEPSW]; }
      else { pc = sr[EIPC] & ~1u; sr[PSW] = sr[EIPSW]; }
      break;
    }
    case 0x1A: pc += 2; ts += 1; halt = 1; break;                 // HALT
    case 0x1C: ldsr(r1, r[r2]); pc += 2; ts += 1; break;           // LDSR reg2, regID
    case 0x1D: r[r2] = sr[r1]; pc += 2; ts += 1; break;            // STSR regID, reg2
    case 0x1F: {                                                    // cadeias de bits
      if (r1 >= 0x10 || (!(r1 & 8) && r1 >= 4)) { exception(0xFFFFFF90, 0xFF90); ts += 1; break; }
      ts += 1;
      if (!bitString(r1)) pc += 2;  // senão, continua na próxima vez
      break;
    }
    // ---------------------------------------------------------------- formato III (desvio condicional)
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
      if (cond((op >> 9) & 15)) branchTo(at + static_cast<uint32_t>(sext(op & 0x1FF, 9)));
      else { pc += 2; ts += 1; }
      break;
    // ---------------------------------------------------------------- formatos IV, V e VI (32 bits)
    case 0x2A: case 0x2B: {  // JR / JAL
      const uint32_t disp = ((op & 0x3FF) << 16) | fetch(at + 2, true);
      if (opc == 0x2B) r[31] = at + 4;
      branchTo(at + static_cast<uint32_t>(sext(disp, 26)));
      break;
    }
    case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: {
      const uint32_t imm = fetch(at + 2, true);
      pc += 4; ts += 1;
      switch (opc) {
        case 0x28: r[r2] = r[r1] + static_cast<uint32_t>(sext(imm, 16)); break;       // MOVEA
        case 0x29: r[r2] = add(r[r1], static_cast<uint32_t>(sext(imm, 16))); break;   // ADDI
        case 0x2C: r[r2] = r[r1] | imm; flag(F_OV, false); setSZ(r[r2]); break;        // ORI
        case 0x2D: r[r2] = r[r1] & imm; flag(F_OV, false); setSZ(r[r2]); break;        // ANDI
        case 0x2E: r[r2] = r[r1] ^ imm; flag(F_OV, false); setSZ(r[r2]); break;        // XORI
        default: r[r2] = r[r1] + (imm << 16); break;                                    // MOVHI
      }
      break;
    }
    case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3B: {  // LD.x / IN.x
      const uint32_t a = r[r1] + static_cast<uint32_t>(sext(fetch(at + 2, true), 16));
      pc += 4;
      ts += (opc & 8) ? ((opc & 3) == 3 ? 5 : 3) : 1;  // o acesso acontece depois do primeiro ciclo
      uint32_t v;
      switch (opc & 3) {
        case 0: v = (opc & 8) ? bus_.in8(a) : bus_.read8(a); if (opc == 0x30) v = static_cast<uint32_t>(sext(v, 8)); break;
        case 1: v = (opc & 8) ? bus_.in16(a & ~1u) : bus_.read16(a & ~1u); if (opc == 0x31) v = static_cast<uint32_t>(sext(v, 16)); break;
        default: v = (opc & 8) ? bus_.in32(a & ~3u) : bus_.read32(a & ~3u); break;
      }
      r[r2] = v;
      if (opc & 8) {  // IN
        lo = LO_IN;
      } else {
        if ((opc & 3) == 3) loadCost(3, 4);
        else loadCost(1, 2);
      }
      break;
    }
    case 0x34: case 0x35: case 0x37: case 0x3C: case 0x3D: case 0x3F: {  // ST.x / OUT.x
      const uint32_t a = r[r1] + static_cast<uint32_t>(sext(fetch(at + 2, true), 16));
      pc += 4;
      const uint32_t v = r[r2];
      ts += 1;
      switch (opc & 3) {
        case 0: if (opc & 8) bus_.out8(a, static_cast<uint8_t>(v)); else bus_.write8(a, static_cast<uint8_t>(v)); break;
        case 1: if (opc & 8) bus_.out16(a & ~1u, static_cast<uint16_t>(v)); else bus_.write16(a & ~1u, static_cast<uint16_t>(v)); break;
        default: if (opc & 8) bus_.out32(a & ~3u, v); else bus_.write32(a & ~3u, v); break;
      }
      const int8_t kind = (opc & 8) ? LO_OUT : LO_ST;
      if (lastOp == kind) ts += (opc & 3) == 3 ? 3 : 1;
      lo = kind;
      break;
    }
    case 0x3A: {  // CAXI disp16[reg1], reg2 (troca condicional com r30)
      const uint32_t a = (r[r1] + static_cast<uint32_t>(sext(fetch(at + 2, true), 16))) & ~3u;
      pc += 4; ts += 26;
      const uint32_t mem = bus_.read32(a);
      const uint32_t cmp = sub(r[r2], mem);
      bus_.write32(a, cmp == 0 ? r[30] : mem);
      r[r2] = mem;
      break;
    }
    case 0x3E: {  // formato VII
      const uint32_t sub2 = fetch(at + 2, true) >> 10;
      pc += 4; ts += 1;
      fpu(static_cast<int>(sub2), r2, r1);
      lo = LO_LONG;
      break;
    }
    default:  // 0x1B, 0x32, 0x36: código inválido (o PC de retorno é a própria instrução)
      ts += 1;
      exception(0xFFFFFF90, 0xFF90);
      break;
  }
  r[0] = 0;
  lastOp = lo;
}

template <class Bus>
void Cpu<Bus>::run() {
  Bus& v = bus_;
  while (v.ts < v.runUntil) {
    if (irqLevel >= 0 && halt != 2) {
      const uint32_t psw = sr[PSW];
      if (!(psw & (F_NP | F_EP | F_ID)) && irqLevel >= static_cast<int>((psw >> 16) & 15)) interrupt();
    }
    if (halt) { v.ts = v.runUntil; break; }
    step();
  }
}

template <class Bus>
void Cpu<Bus>::sync(StateIO& io) {
  io.raw(r, sizeof r);
  io.sync(pc);
  io.raw(sr, sizeof sr);
  io.sync(halt); io.sync(irqLevel); io.sync(lastOp);
  io.raw(cacheTag_, sizeof cacheTag_);
  io.raw(cacheValid_, sizeof cacheValid_);
  if (io.reading()) {
    r[0] = 0;
    if (halt > 2) halt = 2;
    if (irqLevel < -1 || irqLevel > 15) irqLevel = -1;
    if (lastOp < LO_LONG || lastOp > LO_OUT) lastOp = LO_OTHER;
    for (auto& c : cacheValid_) c &= 3;
  }
}

}  // namespace indigo::v810
