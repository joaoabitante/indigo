#include "system.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace indigo::ps1 {

namespace {
uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
  return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8 | uint32_t(b[o + 2]) << 16 | uint32_t(b[o + 3]) << 24;
}

// tempo de acesso de uma região pelo registrador de atraso e pelo COM_DELAY (fórmula do nocash/psx-spx);
// devolve os ciclos extras de 8, 16 e 32 bits
void accessTime(uint32_t delay, uint32_t com, uint8_t out[3]) {
  int first = 0, seq = 0, min = 0;
  const int access = int((delay >> 4) & 15);
  // COM0 (recuperação) só pesa nos acessos seguidos: o cpu/access-time do console mede 18 ciclos no SPUCNT de 16
  // bits e 11 na expansão 2 de 8 bits (com o COM0 no primeiro acesso dariam 22 e 16)
  if (delay & 0x100) seq += int(com & 15) - 1;
  if (delay & 0x400) { first += int((com >> 8) & 15); seq += int((com >> 8) & 15); }
  if (delay & 0x800) min = int((com >> 12) & 15);
  if (first < 6) first++;
  first += access + 2;
  seq += access + 2;
  if (first < min + 6) first = min + 6;
  if (seq < min + 2) seq = min + 2;
  const bool bus16 = (delay & 0x1000) != 0;
  const int b = first, h = bus16 ? first : first + seq, w = bus16 ? first + seq : first + 3 * seq;
  out[0] = uint8_t(std::clamp(b - 1, 0, 255));
  out[1] = uint8_t(std::clamp(h - 1, 0, 255));
  out[2] = uint8_t(std::clamp(w - 1, 0, 255));
}

constexpr uint32_t RAM_LOAD_COST = 4;   // leitura da RAM: ~5 ciclos no total
constexpr uint32_t RAM_FETCH_COST = 4;  // busca fora da cache na RAM
constexpr uint32_t IO_COST = 1;         // registradores internos (GPU, DMA, timers, IRQ...)
}  // namespace

System::System(const std::vector<uint8_t>& bios)
    : cpu(*this), gpu(*this), dma(*this), timers(*this), sio(*this) {
  // IRQ do CD e da SPU na subida da linha; o áudio do CD (CD-DA e XA, 44,1 kHz) entra na SPU, que aplica o
  // volume do CD (0x1F801DB0) e o bit de habilitação do SPUCNT
  cdrom.onIrq = [this] { raiseIrq(IRQ_CDROM); };
  spu.onIrq = [this] { raiseIrq(IRQ_SPU); };
  cdrom.onAudio = [this](int16_t l, int16_t r) { spu.pushCdAudio(l, r); };
  dbg_ = std::getenv("PS1DBG") != nullptr;
  ramBuf.assign(RAM_SIZE + 8, 0);
  biosBuf.assign(BIOS_SIZE + 8, 0);
  ram = ramBuf.data();
  biosMem = biosBuf.data();
  if (!bios.empty()) std::memcpy(biosMem, bios.data(), std::min<size_t>(bios.size(), BIOS_SIZE));
  reset();
}

void System::buildPages() {
  std::fill(std::begin(rpage), std::end(rpage), nullptr);
  std::fill(std::begin(wpage), std::end(wpage), nullptr);
  for (uint32_t p = 0; p < 0x80; p++) {  // 8 MB: a RAM de 2 MB espelhada 4 vezes
    rpage[p] = wpage[p] = ram + (p & 0x1F) * 0x10000;
    rcost[p][0] = rcost[p][1] = rcost[p][2] = RAM_LOAD_COST;
    fcost[p] = RAM_FETCH_COST;
  }
  for (uint32_t p = 0; p < 8; p++) rpage[0x1FC0 + p] = biosMem + p * 0x10000;
  updateMemTiming();
}

void System::updateMemTiming() {
  uint8_t c[3];
  accessTime(memCtrl[4], memCtrl[8], c);
  for (uint32_t p = 0; p < 8; p++) {
    std::memcpy(rcost[0x1FC0 + p], c, 3);
    fcost[0x1FC0 + p] = c[2];
  }
  accessTime(memCtrl[6], memCtrl[8], cdCost);
  accessTime(memCtrl[5], memCtrl[8], spuCost);
  accessTime(memCtrl[2], memCtrl[8], exp1Cost);
  accessTime(memCtrl[7], memCtrl[8], exp2Cost);
  accessTime(memCtrl[3], memCtrl[8], exp3Cost);
}

void System::reset() {
  std::memset(ram, 0, RAM_SIZE);
  std::memset(scratch, 0, sizeof scratch);
  // valores da BIOS depois da partida (os de antes são parecidos; a BIOS os grava logo no começo)
  const uint32_t mc[9] = {0x1F000000, 0x1F802000, 0x0013243F, 0x00003022, 0x0013243F,
                          0x200931E1, 0x00020843, 0x00070777, 0x00031125};
  std::memcpy(memCtrl, mc, sizeof mc);
  ramSizeReg = 0x00000B88;
  sio1Mode = 0;
  cacheCtrl = 0;
  buildPages();
  istat = imask = 0;
  now = 0; nextEvent = 0; periphLast_ = 0;
  frameDone = false;
  cpu.reset();
  gpu.reset();
  dma.reset();
  timers.reset();
  sio.reset();
  gte.reset();
  spu.reset();
  cdrom.reset();
  mdec.reset();
}

void System::syncPeriph() {
  uint64_t el = now - periphLast_;
  while (el) {
    const int c = int(std::min<uint64_t>(el, 1u << 20));
    cdrom.run(c);
    spu.run(c);
    mdec.run(c);
    el -= uint64_t(c);
  }
  periphLast_ = now;
}

void System::syncAll() {
  syncPeriph();
  gpu.sync();
  timers.sync();
  sio.sync();
  dma.sync();
}

void System::runFrame() {
  frameDone = false;
  const uint64_t limit = now + CPU_HZ / 8;  // sem vblank (faixa vertical absurda): no máximo 1/8 s por quadro
  while (!frameDone && now < limit) {
    uint64_t ev = std::min(gpu.nextEvent(), timers.nextEvent());
    ev = std::min(ev, sio.nextEvent());
    ev = std::min(ev, dma.nextEvent());
    ev = std::min(ev, now + 1024);
    nextEvent = ev;
    cpu.run();
    syncAll();
  }
}

uint32_t System::ioRead(uint32_t ph, int size) {
  if (ph >= 0x1F801000 && ph < 0x1F802000) {
    const uint32_t off = ph & 0xFFF;
    // o CD-ROM e a SPU ficam atrás do barramento com o próprio tempo de acesso (cdCost / spuCost)
    if (!((off >= 0x800 && off < 0x804) || off >= 0xC00)) now += IO_COST;
    if (off < 0x24) return memCtrl[off >> 2] >> ((ph & 3) * 8);
    if (off >= 0x40 && off < 0x50) return sio.read(off & 0xF, size);
    if (off >= 0x50 && off < 0x60) {  // SIO1 (serial): sem cabo, sempre pronta para enviar
      if ((off & 0xF) == 4) return 0x00000005u;
      if ((off & 0xF) == 8) return sio1Mode;  // SIO_MODE guarda o que se escreve (8 bits)
      return 0;
    }
    if (off >= 0x60 && off < 0x64) return ramSizeReg >> ((ph & 3) * 8);
    if (off >= 0x70 && off < 0x74) return istat >> ((ph & 3) * 8);
    if (off >= 0x74 && off < 0x78) return imask >> ((ph & 3) * 8);
    if (off >= 0x80 && off < 0x100) return dma.read(off & ~3u) >> ((ph & 3) * 8);
    if (off >= 0x100 && off < 0x130) { timers.sync(); return timers.read(off & ~3u) >> ((ph & 3) * 8); }
    if (off >= 0x800 && off < 0x804) {
      now += cdCost[size >> 1];
      syncPeriph();
      const uint32_t v = cdrom.read(off & 3, size);
      if (dbg_) std::fprintf(stderr, "CD r %X = %02X  pc=%08X t=%llu\n", off & 3, v, cpu.pc, (unsigned long long)now);
      return v;
    }
    if (off >= 0x810 && off < 0x818) {
      if (off < 0x814) return gpu.readData();
      gpu.sync();
      return gpu.status() >> ((ph & 3) * 8);
    }
    if (off >= 0x820 && off < 0x828) { syncPeriph(); return mdec.read(off & 7, size); }
    if (off >= 0xC00) {
      now += spuCost[size >> 1];
      syncPeriph();
      const uint32_t v = spu.read(off - 0xC00, size);
      if (dbg_) std::fprintf(stderr, "SPU r %X = %04X  pc=%08X\n", off - 0xC00, v, cpu.pc);
      return v;
    }
    return 0;
  }
  if (ph >= 0x1F000000 && ph < 0x1F800000) { now += exp1Cost[size >> 1]; return 0xFFFFFFFF; }  // expansão 1 vazia
  if (ph >= 0x1F802000 && ph < 0x1F804000) {
    now += exp2Cost[size >> 1];
    if (ph == 0x1F802021) return 0x0C;  // DUART: transmissor pronto
    return 0xFFFFFFFF;
  }
  if (ph >= 0x1FA00000 && ph < 0x1FC00000) { now += exp3Cost[size >> 1]; return 0; }  // expansão 3 vazia
  return 0;
}

void System::ioWrite(uint32_t ph, uint32_t v, int size) {
  if (ph >= 0x1F801000 && ph < 0x1F802000) {
    const uint32_t off = ph & 0xFFF;
    const uint32_t v32 = v << ((ph & 3) * 8);
    if (off < 0x24) {
      memCtrl[off >> 2] = v32;
      updateMemTiming();
      return;
    }
    if (off >= 0x40 && off < 0x50) { sio.write(off & 0xF, v, size); reschedule(); return; }
    if (off >= 0x50 && off < 0x60) {
      if ((off & 0xF) == 8) sio1Mode = uint8_t(v);
      return;
    }
    if (off >= 0x60 && off < 0x64) { ramSizeReg = v32; return; }
    if (off >= 0x70 && off < 0x74) {
      const uint32_t mask = size == 4 ? 0xFFFFFFFFu : ((1u << (size * 8)) - 1) << ((ph & 3) * 8);
      istat &= v32 | ~mask;
      updateIrq();
      return;
    }
    if (off >= 0x74 && off < 0x78) { imask = v32 & 0x7FF; updateIrq(); return; }
    if (off >= 0x80 && off < 0x100) { dma.write(off & ~3u, v32); reschedule(); return; }
    if (off >= 0x100 && off < 0x130) { timers.sync(); timers.write(off & ~3u, v32); reschedule(); return; }
    if (off >= 0x800 && off < 0x804) {
      syncPeriph();
      if (dbg_) std::fprintf(stderr, "CD w %X <- %02X  pc=%08X t=%llu\n", off & 3, v, cpu.pc, (unsigned long long)now);
      // porta de 8 bits: uma escrita de 16 ou 32 bits vira 2 ou 4 escritas de byte no mesmo endereço, do byte
      // baixo ao alto (cpu/io-access-bitwidth no console: escrever 0x5678 em 1F801800 deixa o índice em 2)
      for (int i = 0; i < size; i++) cdrom.write(off & 3, (v >> (8 * i)) & 0xFF, 1);
      reschedule();
      return;
    }
    if (off >= 0x810 && off < 0x818) {
      if (off < 0x814) gpu.gp0(v32); else { gpu.sync(); gpu.gp1(v32); }
      return;
    }
    if (off >= 0x820 && off < 0x828) { syncPeriph(); mdec.write(off & 7, v, size); return; }
    if (off >= 0xC00) {
      syncPeriph();
      spu.write(off - 0xC00, v, size);
      return;
    }
    return;
  }
  // expansão 2, BIOS e o resto: escrita ignorada
}

bool System::loadExe(const std::vector<uint8_t>& exe) {
  if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) return false;
  const uint32_t pc0 = rd32(exe, 0x10), gp0 = rd32(exe, 0x14);
  const uint32_t taddr = rd32(exe, 0x18), tsize = rd32(exe, 0x1C);
  const uint32_t baddr = rd32(exe, 0x28), bsize = rd32(exe, 0x2C);
  const uint32_t saddr = rd32(exe, 0x30), ssize = rd32(exe, 0x34);
  const size_t n = std::min<size_t>(tsize, exe.size() - 0x800);
  for (size_t i = 0; i < n && i < RAM_SIZE; i++) ram[(taddr + i) & (RAM_SIZE - 1)] = exe[0x800 + i];
  for (uint32_t i = 0; i < bsize && i < RAM_SIZE; i++) ram[(baddr + i) & (RAM_SIZE - 1)] = 0;
  cpu.r[28] = gp0;
  const uint32_t sp = saddr ? saddr + ssize : 0x801FFFF0;
  cpu.r[29] = cpu.r[30] = sp;
  cpu.jump(pc0);
  return true;
}

void System::installMiniKernel() {
  static const uint32_t handler[] = {
      0x401A6800,  // mfc0 k0, cause
      0x401B7000,  // mfc0 k1, epc
      0x00000000,
      0x335A007C,  // andi k0, k0, 0x7C
      0x17400005,  // bnez k0, outra exceção
      0x00000000,
      0x3C1A1F80,  // lui k0, 0x1F80
      0xAF401070,  // sw zero, 0x1070(k0): reconhece todas as interrupções
      0x03600008,  // jr k1
      0x42000010,  // rfe
      0x277B0004,  // addiu k1, k1, 4: pula a instrução (syscall/break)
      0x03600008,  // jr k1
      0x42000010,  // rfe
  };
  auto put = [&](uint32_t a, uint32_t w) { std::memcpy(ram + a, &w, 4); };
  // o vetor (0x80) só salta para o corpo em 0x1000: as entradas A0/B0/C0 vêm logo depois dele
  put(0x80, 0x3C1A8000);  // lui k0, 0x8000
  put(0x84, 0x375A1000);  // ori k0, k0, 0x1000
  put(0x88, 0x03400008);  // jr k0
  put(0x8C, 0);
  for (size_t i = 0; i < sizeof handler / 4; i++) put(0x1000 + uint32_t(i) * 4, handler[i]);
  for (uint32_t a : {0xA0u, 0xB0u, 0xC0u}) { put(a, 0x03E00008); put(a + 4, 0); }  // jr ra
  cpu.sr = 0;  // vetores na RAM
}

void System::onHook() {
  if (!pendingExe_.empty()) {
    const auto exe = std::move(pendingExe_);
    pendingExe_.clear();
    cpu.hookPc = 1;
    loadExe(exe);
  }
}

void System::sync(StateIO& io) {
  io.sync(now);
  io.raw(ram, RAM_SIZE);
  io.raw(scratch, sizeof scratch);
  io.sync(cacheCtrl);
  io.raw(memCtrl, sizeof memCtrl);
  io.sync(ramSizeReg);
  io.sync(istat); io.sync(imask);
  io.sync(periphLast_);
  cpu.sync(io);
  gpu.sync(io);
  dma.sync(io);
  timers.sync(io);
  sio.sync(io);
  if (io.reading()) { gte.load(io); spu.load(io); cdrom.load(io); mdec.load(io); }
  else { gte.save(io); spu.save(io); cdrom.save(io); mdec.save(io); }
  if (io.reading()) {
    istat &= 0x7FF; imask &= 0x7FF;
    if (periphLast_ > now) periphLast_ = now;
    updateMemTiming();
    updateIrq();
  }
}

}  // namespace indigo::ps1
