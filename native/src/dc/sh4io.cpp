// Periféricos internos do SH-4 (área P4, 0xFF000000-0xFFFFFFFF, também vista em 0x1F000000): CCN (cache, store
// queues, códigos de exceção), INTC (prioridades), TMU (3 temporizadores), DMAC (registradores; o canal 2 é
// disparado pelo Holly), BSC (porta A com o tipo de cabo de vídeo), CPG e SCIF (só transmissão: vira texto de
// depuração). Endereços e bits do manual de hardware do SH7750.
#include "dc.h"

namespace indigo::dc {

namespace {
// ciclos da CPU por passo de temporizador (Pφ = 50 MHz = CPU / 4)
int64_t tmuDiv(uint16_t tcr) {
  static const int64_t d[8] = {16, 64, 256, 1024, 4096, 4096, 4096, 4096};
  return d[tcr & 7];
}
}  // namespace

void Sh4::tmuSync() {
  const int64_t el = cycles - tmuLast_;
  tmuLast_ = cycles;
  if (el <= 0) return;
  bool irq = false;
  for (int i = 0; i < 3; i++) {
    if (!(tstr & (1 << i))) continue;
    const int64_t div = tmuDiv(tcr[i]);
    const int64_t tot = int64_t(tmuFrac_[i]) + el;
    uint64_t ticks = uint64_t(tot / div);
    tmuFrac_[i] = uint32_t(tot % div);
    if (!ticks) continue;
    if (ticks <= tcnt[i]) { tcnt[i] -= uint32_t(ticks); continue; }
    // estourou: recarrega com TCOR e continua contando
    ticks -= uint64_t(tcnt[i]) + 1;
    const uint64_t per = uint64_t(tcor[i]) + 1;
    tcnt[i] = tcor[i] - uint32_t(ticks % per);
    if (!(tcr[i] & 0x100)) { tcr[i] |= 0x100; if (tcr[i] & 0x20) irq = true; }
  }
  if (irq) updateIrq();
}

int64_t Sh4::tmuNextEvent() const {
  int64_t best = INT64_MAX / 2;
  for (int i = 0; i < 3; i++) {
    if (!(tstr & (1 << i)) || !(tcr[i] & 0x20) || (tcr[i] & 0x100)) continue;
    const int64_t div = tmuDiv(tcr[i]);
    const int64_t t = tmuLast_ + (int64_t(tcnt[i]) + 1) * div - tmuFrac_[i];
    if (t < best) best = t;
  }
  return best;
}

void Sh4::updateIrq() {
  int best = 0;
  uint32_t code = 0;
  if (irl_ < 15) { best = 15 - irl_; code = 0x200 + 0x20 * uint32_t(irl_); }
  for (int i = 0; i < 3; i++) {
    if ((tcr[i] & 0x120) != 0x120) continue;
    const int p = (ipra >> (12 - 4 * i)) & 15;
    if (p > best) { best = p; code = 0x400 + 0x20 * uint32_t(i); }
  }
  for (int i = 0; i < 4; i++) {
    if ((chcr[i] & 6) != 6) continue;
    const int p = (iprc >> 8) & 15;
    if (p > best) { best = p; code = 0x640 + 0x20 * uint32_t(i); }
  }
  pendPrio_ = best;
  pendCode_ = code;
  if (best) exitRun_ = true;
}

uint32_t Sh4::p4Read(uint32_t a, int size) {
  if (a < 0xE4000000u) return sq[(a >> 2) & 15];
  if (a < 0xFF000000u) return 0;  // matrizes de cache e de TLB: não emuladas
  switch (a) {
    case 0xFF000000: return pteh;
    case 0xFF000004: return ptel;
    case 0xFF000008: return ttb;
    case 0xFF00000C: return tea;
    case 0xFF000010: return mmucr;
    case 0xFF00001C: return ccr;
    case 0xFF000020: return tra;
    case 0xFF000024: return expevt;
    case 0xFF000028: return intevt;
    case 0xFF000030: return 0x040205C1;  // PVR: SH7091
    case 0xFF000038: return qacr[0];
    case 0xFF00003C: return qacr[1];
    case 0xFF800000: return bcr1;
    case 0xFF800004: return bcr2;
    case 0xFF800008: return wcr1;
    case 0xFF80000C: return wcr2;
    case 0xFF800010: return wcr3;
    case 0xFF800014: return mcr;
    case 0xFF800018: return pcr;
    case 0xFF80001C: return rtcsr;
    case 0xFF800020: return rtcnt;
    case 0xFF800024: return rtcor;
    case 0xFF800028: rfcr = uint16_t((rfcr + 1) & 0x3FF); return rfcr;
    case 0xFF80002C: return pctra;
    case 0xFF800030: {
      // porta A: bits 8-9 = tipo de cabo de vídeo (3 = composto); os bits 0-3 de saída voltam como escritos.
      // Os pinos 0 e 1 são ligados um ao outro no console (a BIOS confere isso e para se não for assim): se um
      // deles é saída, os dois leem o valor dele; se nenhum é, o pull-up deixa os dois em 1.
      uint32_t out = 0;
      for (int b = 0; b < 4; b++)
        if (pctra >> (2 * b) & 1) out |= pdtra & (1u << b);
      const bool o0 = pctra & 1, o1 = pctra >> 2 & 1;
      uint32_t lig = 1;
      if (o0) lig &= pdtra & 1;
      if (o1) lig &= pdtra >> 1 & 1;
      out = (out & ~3u) | (lig ? 3u : 0u);
      return out | (3u << 8);
    }
    case 0xFF800040: return pctrb;
    case 0xFF800044: return pdtrb;
    case 0xFF800048: return gpioic;
    case 0xFFA00040: return dmaor;
    case 0xFFC00000: return frqcr;
    case 0xFFC00004: return stbcr;
    case 0xFFC00008: return wtcnt;
    case 0xFFC0000C: return wtcsr;
    case 0xFFC00010: return stbcr2;
    case 0xFFD00000: return icr;
    case 0xFFD00004: return ipra;
    case 0xFFD00008: return iprb;
    case 0xFFD0000C: return iprc;
    case 0xFFD80000: return tocr;
    case 0xFFD80004: return tstr;
    case 0xFFD8002C: return tcpr2;
    case 0xFFE80000: return scsmr2;
    case 0xFFE80004: return scbrr2;
    case 0xFFE80008: return scscr2;
    case 0xFFE80010: return scfsr2 | 0x60;  // transmissor sempre vazio
    case 0xFFE80014: return 0;
    case 0xFFE80018: return scfcr2;
    case 0xFFE8001C: return 0;
    case 0xFFE80020: return scsptr2;
    case 0xFFE80024: return sclsr2;
    default: break;
  }
  if (a >= 0xFFA00000u && a < 0xFFA00040u) {
    const int ch = (a >> 4) & 3;
    switch (a & 0xF) {
      case 0: return sar[ch];
      case 4: return dar[ch];
      case 8: return dmatcr[ch];
      default: return chcr[ch];
    }
  }
  if (a >= 0xFFD80008u && a < 0xFFD8002Cu) {
    tmuSync();
    const int i = int((a - 0xFFD80008u) / 12), k = int((a - 0xFFD80008u) % 12);
    return k == 0 ? tcor[i] : k == 4 ? tcnt[i] : tcr[i];
  }
  (void)size;
  return 0;
}

void Sh4::p4Write(uint32_t a, uint32_t v, int size) {
  if (a < 0xE4000000u) { sq[(a >> 2) & 15] = v; return; }
  if (a < 0xFF000000u) return;
  switch (a) {
    case 0xFF000000: pteh = v; return;
    case 0xFF000004: ptel = v; return;
    case 0xFF000008: ttb = v; return;
    case 0xFF00000C: tea = v; return;
    case 0xFF000010: mmucr = v & ~4u; return;  // TI (invalida a TLB) volta a 0
    case 0xFF00001C: ccr = v & ~0x0808u; return;  // ICI/OCI voltam a 0
    case 0xFF000020: tra = v; return;
    case 0xFF000024: expevt = v; return;
    case 0xFF000028: intevt = v; return;
    case 0xFF000038: qacr[0] = v & 0x1C; return;
    case 0xFF00003C: qacr[1] = v & 0x1C; return;
    case 0xFF800000: bcr1 = v; return;
    case 0xFF800004: bcr2 = uint16_t(v); return;
    case 0xFF800008: wcr1 = uint16_t(v); return;
    case 0xFF80000C: wcr2 = uint16_t(v); return;
    case 0xFF800010: wcr3 = uint16_t(v); return;
    case 0xFF800014: mcr = v; return;
    case 0xFF800018: pcr = uint16_t(v); return;
    case 0xFF80001C: rtcsr = uint16_t(v); return;
    case 0xFF800020: rtcnt = uint16_t(v); return;
    case 0xFF800024: rtcor = uint16_t(v); return;
    case 0xFF800028: rfcr = uint16_t(v & 0x3FF); return;
    case 0xFF80002C: pctra = v; return;
    case 0xFF800030: pdtra = uint16_t(v); return;
    case 0xFF800040: pctrb = v; return;
    case 0xFF800044: pdtrb = uint16_t(v); return;
    case 0xFF800048: gpioic = uint16_t(v); return;
    case 0xFFA00040: dmaor = v; return;
    case 0xFFC00000: frqcr = uint16_t(v); return;
    case 0xFFC00004: stbcr = uint8_t(v); return;
    case 0xFFC00008: if ((v >> 8) == 0x5A) wtcnt = uint16_t(v & 0xFF); return;
    case 0xFFC0000C: if ((v >> 8) == 0xA5) wtcsr = uint8_t(v); return;
    case 0xFFC00010: stbcr2 = uint8_t(v); return;
    case 0xFFD00000: icr = uint16_t(v); return;
    case 0xFFD00004: ipra = uint16_t(v); updateIrq(); return;
    case 0xFFD00008: iprb = uint16_t(v); updateIrq(); return;
    case 0xFFD0000C: iprc = uint16_t(v); updateIrq(); return;
    case 0xFFD80000: tocr = uint8_t(v & 1); return;
    case 0xFFD80004: tmuSync(); tstr = uint8_t(v & 7); exitRun_ = true; return;
    case 0xFFD8002C: tcpr2 = v; return;
    case 0xFFE80000: scsmr2 = uint16_t(v); return;
    case 0xFFE80004: scbrr2 = uint8_t(v); return;
    case 0xFFE80008: scscr2 = uint16_t(v); return;
    case 0xFFE8000C: if (serial.size() < (1u << 20)) serial.push_back(char(v)); return;
    case 0xFFE80010: scfsr2 = uint16_t(scfsr2 & v); return;
    case 0xFFE80018: scfcr2 = uint16_t(v); return;
    case 0xFFE80020: scsptr2 = uint16_t(v); return;
    case 0xFFE80024: sclsr2 = uint16_t(v); return;
    default: break;
  }
  if (a >= 0xFFA00000u && a < 0xFFA00040u) {
    const int ch = (a >> 4) & 3;
    switch (a & 0xF) {
      case 0: sar[ch] = v; break;
      case 4: dar[ch] = v; break;
      case 8: dmatcr[ch] = v & 0xFFFFFF; break;
      default: chcr[ch] = v; updateIrq(); break;
    }
    return;
  }
  if (a >= 0xFFD80008u && a < 0xFFD8002Cu) {
    tmuSync();
    const int i = int((a - 0xFFD80008u) / 12), k = int((a - 0xFFD80008u) % 12);
    if (k == 0) tcor[i] = v;
    else if (k == 4) tcnt[i] = v;
    else {
      // UNF só apaga escrevendo 0
      const uint16_t nv = uint16_t(v & 0x3FF);
      tcr[i] = uint16_t((nv & ~0x100) | (nv & tcr[i] & 0x100));
      tmuFrac_[i] = 0;
      updateIrq();
    }
    exitRun_ = true;
    return;
  }
  (void)size;
}

}  // namespace indigo::dc
