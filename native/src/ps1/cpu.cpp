#include "cpu.h"

#include <cstring>

#include "system.h"

namespace indigo::ps1 {

namespace {
enum Exc : uint32_t { EXC_INT = 0, EXC_ADEL = 4, EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9,
                      EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12 };

inline uint32_t ld32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint16_t ld16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void st32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void st16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }

// regiões com o espaço físico de 512 MB: KUSEG (só os primeiros 512 MB existem), KSEG0 e KSEG1
constexpr uint64_t WB_COST = 4;  // escrita na RAM pelo buffer de escrita
constexpr bool MAPPED[8] = {true, false, false, false, true, true, false, false};
// registradores de onde o console busca instruções (cpu/code-in-io: os da SPU e os do DMA respondem como memória;
// o scratchpad, o MDEC e os de interrupção dão erro de barramento na busca)
inline bool ioFetchable(uint32_t region, uint32_t ph) {
  return MAPPED[region] && ((ph >= 0x1F801080 && ph < 0x1F801100) || (ph >= 0x1F801C00 && ph < 0x1F802000));
}
}  // namespace

void Cpu::reset() {
  std::memset(r, 0, sizeof r);
  hi = lo = 0;
  sr = 0x00400000;  // BEV: vetores na ROM
  cause = 0; epc = 0; badv = 0;
  bpc = bda = tar = dcic = bdam = bpcm = 0;
  ldReg = ldVal = nextLdReg = nextLdVal = 0;
  inDelay = branched = false;
  hiloReady = 0;
  wbReady = 0;
  gteReady = 0;
  std::memset(itag, 0xFF, sizeof itag);
  std::memset(ivalid, 0, sizeof ivalid);
  jump(0xBFC00000);
  updateIrq();
}

void Cpu::exception(uint32_t code, uint32_t copn) {
  if (ldReg) { r[ldReg] = ldVal; ldReg = 0; }  // a carga anterior termina
  nextLdReg = 0;                               // a instrução que falhou não carrega nada
  cause = (cause & ~0xB000007Cu) | (code << 2) | (copn << 28);
  if (inDelay) { epc = curPc - 4; cause |= 0x80000000u; } else epc = curPc;
  sr = (sr & ~0x3Fu) | ((sr << 2) & 0x3Fu);
  const uint32_t vec = (sr & 0x400000) ? 0xBFC00180 : 0x80000080;
  pc = vec; npc = vec + 4;
  branched = false;
  updateIrq();
}

uint32_t Cpu::fetch(uint32_t a) {
  System& s = sys_;
  const uint32_t region = a >> 29;
  const uint32_t ph = a & 0x1FFFFFFF;
  const uint8_t* p = MAPPED[region] ? s.rpage[ph >> 16] : nullptr;
  if (!p) {
    if (ioFetchable(region, ph)) return s.ioRead(ph, 4);
    return 0xFFFFFFFF;  // fora da RAM e da BIOS: erro de barramento na busca (quem chama confere)
  }
  const uint32_t op = ld32(p + (ph & 0xFFFF));
  if (region == 5 || !(s.cacheCtrl & 0x800)) {
    s.now += s.fcost[ph >> 16];
  } else {
    const uint32_t line = (a >> 4) & 0xFF, word = (a >> 2) & 3, tag = ph >> 12;
    if (itag[line] != tag || !((ivalid[line] >> word) & 1)) {
      // linha preenchida da palavra pedida até o fim (rajada)
      itag[line] = tag;
      ivalid[line] = uint8_t((0xF << word) & 0xF);
      s.now += s.fcost[ph >> 16] + (3 - word);
    }
  }
  return op;
}

bool Cpu::memRead(uint32_t a, int size, uint32_t& out) {
  if (a & uint32_t(size - 1)) { badv = a; exception(EXC_ADEL); return false; }
  System& s = sys_;
  const uint32_t region = a >> 29;
  if (MAPPED[region]) {
    const uint32_t ph = a & 0x1FFFFFFF;
    if (const uint8_t* p = s.rpage[ph >> 16]) {
      p += ph & 0xFFFF;
      out = size == 4 ? ld32(p) : size == 2 ? ld16(p) : *p;
      if (s.now < wbReady) s.now = wbReady;  // a leitura espera o buffer de escrita esvaziar
      s.now += s.rcost[ph >> 16][size >> 1];
      return true;
    }
    if ((ph & 0xFFFFFC00) == 0x1F800000) {
      if (region == 5) { exception(EXC_DBE); return false; }  // sem scratchpad no KSEG1
      const uint8_t* q = s.scratch + (ph & 0x3FF);
      out = size == 4 ? ld32(q) : size == 2 ? ld16(q) : *q;
      return true;
    }
    if (s.now < wbReady) s.now = wbReady;  // o barramento só fica livre com o buffer de escrita vazio
    out = s.ioRead(ph, size);
    return true;
  }
  if (a == 0xFFFE0130) { out = s.cacheCtrl; return true; }
  if (region >= 6) { out = 0; return true; }
  exception(EXC_DBE);
  return false;
}

bool Cpu::memWrite(uint32_t a, uint32_t v, int size) {
  if (a & uint32_t(size - 1)) { badv = a; exception(EXC_ADES); return false; }
  System& s = sys_;
  const uint32_t region = a >> 29;
  if (sr & 0x10000) {
    // cache isolada: a escrita vai para a cache de instruções (a BIOS limpa as etiquetas assim), não à memória
    if (region < 5) ivalid[(a >> 4) & 0xFF] = 0;
    return true;
  }
  if (MAPPED[region]) {
    const uint32_t ph = a & 0x1FFFFFFF;
    if (uint8_t* p = s.wpage[ph >> 16]) {
      p += ph & 0xFFFF;
      if (size == 4) st32(p, v); else if (size == 2) st16(p, uint16_t(v)); else *p = uint8_t(v);
      wbReady = (wbReady > s.now ? wbReady : s.now) + WB_COST;
      return true;
    }
    if ((ph & 0xFFFFFC00) == 0x1F800000) {
      if (region == 5) { exception(EXC_DBE); return false; }
      uint8_t* q = s.scratch + (ph & 0x3FF);
      if (size == 4) st32(q, v); else if (size == 2) st16(q, uint16_t(v)); else *q = uint8_t(v);
      return true;
    }
    s.ioWrite(ph, v, size);
    return true;
  }
  if (a == 0xFFFE0130) { s.cacheCtrl = v; return true; }
  if (region >= 6) return true;
  exception(EXC_DBE);
  return false;
}

void Cpu::hook() {
  const uint32_t p = pc & 0x1FFFFFFF;
  if ((p == 0xA0 && r[9] == 0x3C) || (p == 0xB0 && r[9] == 0x3D)) {
    if (tty.size() < (1u << 20)) tty += char(r[4]);
  }
  if (pc == hookPc) sys_.onHook();
}

void Cpu::run() {
  System& s = sys_;
  while (s.now < s.nextEvent) {
    if (irqPending) {
      curPc = pc;
      inDelay = branched;
      // instrução do GTE na fila: o hardware a executa antes de atender (a BIOS pula ao voltar)
      uint32_t peek = 0;
      if (!(pc & 3)) {
        const uint32_t ph = pc & 0x1FFFFFFF;
        if (MAPPED[pc >> 29]) if (const uint8_t* p = s.rpage[ph >> 16]) peek = ld32(p + (ph & 0xFFFF));
      }
      if ((peek >> 25) == 0x25 && (sr & 0x40000000)) gteStart(peek & 0x1FFFFFF);
      exception(EXC_INT);
      continue;
    }
    if ((pc & 0x1FFFFF00) == 0 || pc == hookPc) hook();
    curPc = pc;
    inDelay = branched;
    branched = false;
    s.now += 1;
    if (pc & 3) { badv = pc; exception(EXC_ADEL); continue; }
    const uint32_t op = fetch(pc);
    if (op == 0xFFFFFFFF && !s.rpage[(pc & 0x1FFFFFFF) >> 16] && !ioFetchable(pc >> 29, pc & 0x1FFFFFFF)) {
      exception(EXC_IBE);
      continue;
    }
    pc = npc;
    npc += 4;
    execute(op);
    if (ldReg) r[ldReg] = ldVal;
    ldReg = nextLdReg; ldVal = nextLdVal; nextLdReg = 0;
    r[0] = 0;
  }
}

void Cpu::execute(uint32_t op) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31;
  const uint32_t imm = op & 0xFFFF;
  const uint32_t simm = uint32_t(int32_t(int16_t(imm)));
  switch (op >> 26) {
    case 0x00: execSpecial(op); break;
    case 0x01: {  // BcondZ
      const bool ge = (rt & 1) != 0;
      const bool taken = ge ? int32_t(r[rs]) >= 0 : int32_t(r[rs]) < 0;
      const uint32_t target = pc + (simm << 2);  // pc já aponta para o delay slot
      if ((rt & 0x1E) == 0x10) setReg(31, pc + 4);
      if (taken) branch(target);
      break;
    }
    case 0x02: branch((pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2)); break;  // J
    case 0x03: setReg(31, pc + 4); branch((pc & 0xF0000000) | ((op & 0x3FFFFFF) << 2)); break;  // JAL
    case 0x04: if (r[rs] == r[rt]) branch(pc + (simm << 2)); break;  // BEQ
    case 0x05: if (r[rs] != r[rt]) branch(pc + (simm << 2)); break;  // BNE
    case 0x06: if (int32_t(r[rs]) <= 0) branch(pc + (simm << 2)); break;  // BLEZ
    case 0x07: if (int32_t(r[rs]) > 0) branch(pc + (simm << 2)); break;   // BGTZ
    case 0x08: {  // ADDI
      const uint32_t a = r[rs], res = a + simm;
      if (~(a ^ simm) & (a ^ res) & 0x80000000u) { exception(EXC_OV); break; }
      setReg(rt, res);
      break;
    }
    case 0x09: setReg(rt, r[rs] + simm); break;                                // ADDIU
    case 0x0A: setReg(rt, int32_t(r[rs]) < int32_t(simm) ? 1 : 0); break;      // SLTI
    case 0x0B: setReg(rt, r[rs] < simm ? 1 : 0); break;                        // SLTIU
    case 0x0C: setReg(rt, r[rs] & imm); break;                                 // ANDI
    case 0x0D: setReg(rt, r[rs] | imm); break;                                 // ORI
    case 0x0E: setReg(rt, r[rs] ^ imm); break;                                 // XORI
    case 0x0F: setReg(rt, imm << 16); break;                                   // LUI
    case 0x10: execCop0(op); break;
    case 0x11: case 0x13:  // COP1/COP3: não existem; só a exceção quando desligados
      if (!(sr & (1u << (28 + ((op >> 26) & 3))))) exception(EXC_CPU, (op >> 26) & 3);
      break;
    case 0x12: execCop2(op); break;
    case 0x20: { uint32_t v; if (memRead(r[rs] + simm, 1, v)) load(rt, uint32_t(int32_t(int8_t(v)))); break; }   // LB
    case 0x21: { uint32_t v; if (memRead(r[rs] + simm, 2, v)) load(rt, uint32_t(int32_t(int16_t(v)))); break; } // LH
    case 0x22: case 0x26: {  // LWL / LWR
      const uint32_t a = r[rs] + simm;
      uint32_t w;
      if (!memRead(a & ~3u, 4, w)) break;
      const uint32_t cur = (rt == ldReg && ldReg) ? ldVal : r[rt];  // a carga pendente chega antes
      const uint32_t sh = a & 3;
      uint32_t v;
      if ((op >> 26) == 0x22) v = (cur & (0x00FFFFFFu >> (sh * 8))) | (w << ((3 - sh) * 8));
      else v = (cur & ~(0xFFFFFFFFu >> (sh * 8))) | (w >> (sh * 8));
      load(rt, v);
      break;
    }
    case 0x23: { uint32_t v; if (memRead(r[rs] + simm, 4, v)) load(rt, v); break; }  // LW
    case 0x24: { uint32_t v; if (memRead(r[rs] + simm, 1, v)) load(rt, v & 0xFF); break; }    // LBU
    case 0x25: { uint32_t v; if (memRead(r[rs] + simm, 2, v)) load(rt, v & 0xFFFF); break; }  // LHU
    case 0x28: memWrite(r[rs] + simm, r[rt], 1); break;  // SB
    case 0x29: memWrite(r[rs] + simm, r[rt], 2); break;  // SH
    case 0x2B: memWrite(r[rs] + simm, r[rt], 4); break;  // SW
    case 0x2A: case 0x2E: {  // SWL / SWR
      const uint32_t a = r[rs] + simm;
      uint32_t w;
      const bool iso = (sr & 0x10000) != 0;
      if (iso) { memWrite(a & ~3u, 0, 4); break; }
      if (!memRead(a & ~3u, 4, w)) break;
      const uint32_t sh = a & 3, v = r[rt];
      if ((op >> 26) == 0x2A) w = (w & ~(0xFFFFFFFFu >> ((3 - sh) * 8))) | (v >> ((3 - sh) * 8));
      else w = (w & ~(0xFFFFFFFFu << (sh * 8))) | (v << (sh * 8));
      memWrite(a & ~3u, w, 4);
      break;
    }
    case 0x30: case 0x31: case 0x33: case 0x38: case 0x39: case 0x3B: {  // LWC0/1/3, SWC0/1/3
      // sem o coprocessador ligado: exceção; ligado: nada acontece (não existem no PS1)
      const uint32_t n = (op >> 26) & 3;
      if (!(sr & (1u << (28 + n)))) exception(EXC_CPU, n);
      break;
    }
    case 0x32: {  // LWC2
      if (!(sr & 0x40000000)) { exception(EXC_CPU, 2); break; }
      gteWait();
      uint32_t v;
      if (memRead(r[rs] + simm, 4, v)) sys_.gte.writeData(int(rt), v);
      break;
    }
    case 0x3A: {  // SWC2
      if (!(sr & 0x40000000)) { exception(EXC_CPU, 2); break; }
      gteWait();
      memWrite(r[rs] + simm, sys_.gte.readData(int(rt)), 4);
      break;
    }
    default: exception(EXC_RI); break;
  }
}

void Cpu::execSpecial(uint32_t op) {
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  System& s = sys_;
  switch (op & 0x3F) {
    case 0x00: setReg(rd, r[rt] << sa); break;                                   // SLL
    case 0x02: setReg(rd, r[rt] >> sa); break;                                   // SRL
    case 0x03: setReg(rd, uint32_t(int32_t(r[rt]) >> sa)); break;                // SRA
    case 0x04: setReg(rd, r[rt] << (r[rs] & 31)); break;                         // SLLV
    case 0x06: setReg(rd, r[rt] >> (r[rs] & 31)); break;                         // SRLV
    case 0x07: setReg(rd, uint32_t(int32_t(r[rt]) >> (r[rs] & 31))); break;      // SRAV
    case 0x08: branch(r[rs]); break;                                             // JR
    case 0x09: { const uint32_t t = r[rs]; setReg(rd, pc + 4); branch(t); break; }  // JALR
    case 0x0C: exception(EXC_SYS); break;
    case 0x0D: exception(EXC_BP); break;
    case 0x10: if (s.now < hiloReady) s.now = hiloReady; setReg(rd, hi); break;  // MFHI
    case 0x11: hi = r[rs]; break;                                                // MTHI
    case 0x12: if (s.now < hiloReady) s.now = hiloReady; setReg(rd, lo); break;  // MFLO
    case 0x13: lo = r[rs]; break;                                                // MTLO
    case 0x18: {  // MULT
      const int64_t m = int64_t(int32_t(r[rs])) * int64_t(int32_t(r[rt]));
      lo = uint32_t(m); hi = uint32_t(uint64_t(m) >> 32);
      const uint32_t a = r[rs] & 0x80000000u ? ~r[rs] : r[rs];
      hiloReady = s.now + (a < 0x800 ? 6 : a < 0x100000 ? 9 : 13);
      break;
    }
    case 0x19: {  // MULTU
      const uint64_t m = uint64_t(r[rs]) * uint64_t(r[rt]);
      lo = uint32_t(m); hi = uint32_t(m >> 32);
      hiloReady = s.now + (r[rs] < 0x800 ? 6 : r[rs] < 0x100000 ? 9 : 13);
      break;
    }
    case 0x1A: {  // DIV
      const int32_t n = int32_t(r[rs]), d = int32_t(r[rt]);
      if (d == 0) { hi = uint32_t(n); lo = n >= 0 ? 0xFFFFFFFFu : 1; }
      else if (uint32_t(n) == 0x80000000u && d == -1) { hi = 0; lo = 0x80000000u; }
      else { lo = uint32_t(n / d); hi = uint32_t(n % d); }
      hiloReady = s.now + 36;
      break;
    }
    case 0x1B: {  // DIVU
      const uint32_t n = r[rs], d = r[rt];
      if (d == 0) { hi = n; lo = 0xFFFFFFFFu; } else { lo = n / d; hi = n % d; }
      hiloReady = s.now + 36;
      break;
    }
    case 0x20: {  // ADD
      const uint32_t a = r[rs], b = r[rt], res = a + b;
      if (~(a ^ b) & (a ^ res) & 0x80000000u) { exception(EXC_OV); break; }
      setReg(rd, res);
      break;
    }
    case 0x21: setReg(rd, r[rs] + r[rt]); break;  // ADDU
    case 0x22: {  // SUB
      const uint32_t a = r[rs], b = r[rt], res = a - b;
      if ((a ^ b) & (a ^ res) & 0x80000000u) { exception(EXC_OV); break; }
      setReg(rd, res);
      break;
    }
    case 0x23: setReg(rd, r[rs] - r[rt]); break;     // SUBU
    case 0x24: setReg(rd, r[rs] & r[rt]); break;     // AND
    case 0x25: setReg(rd, r[rs] | r[rt]); break;     // OR
    case 0x26: setReg(rd, r[rs] ^ r[rt]); break;     // XOR
    case 0x27: setReg(rd, ~(r[rs] | r[rt])); break;  // NOR
    case 0x2A: setReg(rd, int32_t(r[rs]) < int32_t(r[rt]) ? 1 : 0); break;  // SLT
    case 0x2B: setReg(rd, r[rs] < r[rt] ? 1 : 0); break;                    // SLTU
    default: exception(EXC_RI); break;
  }
}

void Cpu::execCop0(uint32_t op) {
  if ((sr & 2) && !(sr & 0x10000000)) { exception(EXC_CPU, 0); return; }  // modo usuário sem CU0
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  if (rs == 0x00 || rs == 0x02) {  // MFC0 (CFC0 lê o mesmo)
    uint32_t v = 0;
    switch (rd) {
      case 3: v = bpc; break;
      case 5: v = bda; break;
      case 6: v = tar; break;
      case 7: v = dcic; break;
      case 8: v = badv; break;
      case 9: v = bdam; break;
      case 11: v = bpcm; break;
      case 12: v = sr; break;
      case 13: v = cause; break;
      case 14: v = epc; break;
      case 15: v = 0x00000002; break;  // PRId
      default: exception(EXC_RI); return;
    }
    load(rt, v);
  } else if (rs == 0x04 || rs == 0x06) {  // MTC0
    const uint32_t v = r[rt];
    switch (rd) {
      case 3: bpc = v; break;
      case 5: bda = v; break;
      case 7: dcic = v & 0xFF80F03Fu; break;
      case 9: bdam = v; break;
      case 11: bpcm = v; break;
      case 12: sr = v & 0xF247FF3Fu; updateIrq(); break;
      case 13: cause = (cause & ~0x300u) | (v & 0x300u); updateIrq(); break;
      default: break;
    }
  } else if (rs & 0x10) {
    if ((op & 0x3F) == 0x10) {  // RFE
      sr = (sr & ~0xFu) | ((sr >> 2) & 0xFu);
      updateIrq();
    }
    // outros comandos do COP0 (TLB) não existem no PS1 e não geram exceção (confere no console)
  } else {
    exception(EXC_RI);
  }
}

// o GTE trabalha junto com a CPU: o comando leva os ciclos que command() devolve e a CPU só espera quando volta
// a mexer no COP2 (outro comando, MFC2/CFC2/MTC2/CTC2, LWC2/SWC2) antes de ele terminar
void Cpu::gteWait() {
  if (sys_.now < gteReady) sys_.now = gteReady;
}

void Cpu::gteStart(uint32_t cmd) {
  gteWait();
  gteReady = sys_.now + uint64_t(sys_.gte.command(cmd));
}

void Cpu::execCop2(uint32_t op) {
  if (!(sr & 0x40000000)) { exception(EXC_CPU, 2); return; }
  System& s = sys_;
  gteWait();
  if (op & 0x02000000) { gteStart(op & 0x1FFFFFF); return; }
  const uint32_t rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  switch (rs) {
    case 0x00: load(rt, s.gte.readData(int(rd))); break;  // MFC2
    case 0x02: load(rt, s.gte.readCtrl(int(rd))); break;  // CFC2
    case 0x04: s.gte.writeData(int(rd), r[rt]); break;    // MTC2
    case 0x06: s.gte.writeCtrl(int(rd), r[rt]); break;    // CTC2
    default: exception(EXC_RI); break;
  }
}

void Cpu::sync(StateIO& io) {
  io.raw(r, sizeof r);
  io.sync(hi); io.sync(lo); io.sync(pc); io.sync(npc);
  io.sync(sr); io.sync(cause); io.sync(epc); io.sync(badv);
  io.sync(bpc); io.sync(bda); io.sync(tar); io.sync(dcic); io.sync(bdam); io.sync(bpcm);
  io.sync(ldReg); io.sync(ldVal); io.sync(curPc);
  io.sync(inDelay); io.sync(branched);
  io.sync(hiloReady);
  io.sync(wbReady);
  io.sync(gteReady);
  io.raw(itag, sizeof itag); io.raw(ivalid, sizeof ivalid);
  if (io.reading()) {
    ldReg &= 31;
    r[0] = 0;
    for (auto& v : ivalid) v &= 0xF;
    nextLdReg = 0;
    if (gteReady > sys_.now + 4096) gteReady = sys_.now;  // estado danificado: nenhum comando dura tanto
    updateIrq();
  }
}

}  // namespace indigo::ps1
