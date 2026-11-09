// Sega Saturn: barramento, quadro, SMPC e o núcleo (ver saturn.h).
#include "saturn.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::saturn {

namespace {
constexpr uint32_t BIOS_SIZE = 0x80000;
constexpr uint32_t STATE_MAGIC = 0x53415432;  // "SAT2" (2.5: bloco de CD com a unidade e os tempos)

inline uint32_t rdBE(const uint8_t* p, int size) {
  return size == 1 ? p[0] : size == 2 ? uint32_t(p[0] << 8 | p[1]) : (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]);
}
inline void wrBE(uint8_t* p, int size, uint32_t v) {
  if (size == 1) { p[0] = uint8_t(v); return; }
  if (size == 2) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); return; }
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
bool contains(const std::vector<uint8_t>& b, const char* s) {
  const size_t n = std::strlen(s);
  return std::search(b.begin(), b.end(), s, s + n) != b.end();
}
}  // namespace

bool isSaturnBios(const std::vector<uint8_t>& b) {
  if (b.size() != BIOS_SIZE) return false;
  // vetor de reset do SH-2: PC na própria ROM (com ou sem cache)
  // (a pilha não é conferida: os vetores da BIOS real nunca foram vistos aqui)
  const uint32_t ppc = rdBE(b.data(), 4) & 0x1FFFFFFF;
  if (ppc >= BIOS_SIZE || (ppc & 1)) return false;
  return contains(b, "SEGA SEGASATURN") || contains(b, "SEGASATURN");
}

bool isSaturnDisc(cdrom::Disc& d) {
  for (const auto& t : d.tracks()) {
    if (t.type == cdrom::TrackType::Audio) continue;
    uint8_t buf[cdrom::DATA_SECTOR];
    return d.readData(t.start, buf) && std::memcmp(buf, "SEGA SEGASATURN ", 16) == 0;
  }
  return false;
}

// ------------------------------------------------------------------------------------------------ sistema

System::System(const std::vector<uint8_t>& b, cdrom::Disc* disc)
    : bios(BIOS_SIZE, 0xFF), lwram(0x100000), hwram(0x100000), backup(0x8000) {
  std::copy(b.begin(), b.begin() + std::min<size_t>(b.size(), BIOS_SIZE), bios.begin());
  cd.setDisc(disc);
  slave.slaveMode = true;
  // região: a primeira que o disco aceita, nesta ordem (EUA, Japão, Ásia, Brasil, Coreia, Europa, Ásia PAL, A. Latina)
  if (disc) {
    uint8_t ip[cdrom::DATA_SECTOR];
    for (const auto& t : disc->tracks()) {
      if (t.type == cdrom::TrackType::Audio) continue;
      if (disc->readData(t.start, ip) && std::memcmp(ip, "SEGA SEGASATURN ", 16) == 0) {
        static const struct { char c; uint8_t code; } ORDER[] = {{'U', 4}, {'J', 1}, {'T', 2}, {'B', 5}, {'K', 6}, {'E', 0xC}, {'A', 0xA}, {'L', 0xD}};
        for (const auto& o : ORDER)
          if (std::memchr(ip + 0x40, o.c, 10)) { area = o.code; break; }
      }
      break;
    }
  }
  pal = area == 0xC || area == 0xA;
  vdp2.pal = pal;
  // RAM de backup formatada como o aparelho deixa ("BackUpRam Format" repetido no primeiro bloco)
  static const char fmt[] = "BackUpRam Format";
  for (int i = 0; i < 64; i++) backup[size_t(i)] = uint8_t(fmt[i % 16]);
  reset();
}

void System::mapMemory() {
  for (Sh2* c : {&master, &slave}) {
    for (int b : {6, 7}) {
      c->rpage[b] = hwram.data();
      c->wpage[b] = hwram.data();
      c->pmask[b] = 0xFFFFF;
      c->rwait[b] = 0;
    }
    c->remapped();
  }
}

void System::reset() {
  std::fill(lwram.begin(), lwram.end(), 0);
  std::fill(hwram.begin(), hwram.end(), 0);
  mapMemory();
  scu.reset();
  smpc.reset();
  vdp1.reset();
  vdp2.reset();
  scsp.reset();
  cd.reset();
  master.clock = slave.clock = 0;
  master.powerOn();
  slave.powerOn();
  lineBase_ = 0;
  line = 0;
  audioFrac_ = 0;
  audio.clear();
  inVblank = inHblank = false;
}

void System::slavePower(bool on) {
  if (on) {
    slave.clock = master.clock;
    slave.powerOn();
  }
  smpc.slaveOn = on;
}

void System::soundPower(bool on) {
  if (on && !scsp.cpuOn) scsp.cpu.reset();
  scsp.cpuOn = on;
  smpc.soundOn = on;
}

void System::slaveIrl() {
  slave.setIrl(inVblank ? 6 : inHblank ? 2 : 0);
}

int System::irlVector(Sh2& c, int level) {
  if (&c == &master) return scu.masterVector();
  return inVblank ? 0x43 : 0x41;
}

uint32_t System::busRead(uint32_t a, int size) {
  a &= 0x07FFFFFF;
  if (a < 0x00100000) return rdBE(&bios[a & (BIOS_SIZE - 1) & ~uint32_t(size - 1)], size);
  if (a < 0x00180000) {  // SMPC: registradores nos bytes ímpares
    const uint32_t reg = (a >> 1) & 0x3F;
    if (size == 1) return (a & 1) ? smpc.read(reg) : 0xFF;
    if (size == 2) return 0xFF00u | smpc.read(reg);
    return (0xFF00u | smpc.read(reg)) << 16 | 0xFF00u | smpc.read(reg + 1);
  }
  if (a < 0x00200000) {  // RAM de backup nos bytes ímpares
    const uint32_t i = (a >> 1) & 0x7FFF;
    if (size == 1) return (a & 1) ? backup[i] : 0xFF;
    if (size == 2) return 0xFF00u | backup[i];
    return (0xFF00u | backup[i]) << 16 | 0xFF00u | backup[(i + 1) & 0x7FFF];
  }
  if (a < 0x00400000) return rdBE(&lwram[a & 0xFFFFF & ~uint32_t(size - 1)], size);
  if (a >= 0x06000000) return rdBE(&hwram[a & 0xFFFFF & ~uint32_t(size - 1)], size);
  if (a >= 0x05800000 && a < 0x05900000) {  // bloco de CD
    const uint32_t o = a & 0xFFFFF;
    if (o >= 0x18000 && o < 0x19000) {
      if (size == 4) return cd.readData32();
      const uint16_t v = cd.readData();
      return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
    }
    if (o >= 0x90000) {
      // registro 0x00 (0x25890000) é a porta de dados de 16 bits, a que a BIOS usa (o TOC, os setores)
      if ((o & 0x3C) == 0) {
        if (size == 4) return cd.readData32();
        const uint16_t v = cd.readData();
        return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
      }
      if (size == 4) return uint32_t(cd.read(o & 0x3C)) << 16 | cd.read(o & 0x3C);
      const uint16_t v = cd.read(o & 0x3C);
      return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
    }
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
  }
  if (a >= 0x05A00000 && a < 0x05B00000) return rdBE(&scsp.ram[a & 0x7FFFF & ~uint32_t(size - 1)], size);
  if (a >= 0x05B00000 && a < 0x05C00000) {
    const uint32_t o = a & 0xFFF;
    if (size == 4) return uint32_t(scsp.readReg(o & ~3u)) << 16 | scsp.readReg((o & ~3u) + 2);
    const uint16_t v = scsp.readReg(o & ~1u);
    return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
  }
  if (a >= 0x05C00000 && a < 0x05C80000) return rdBE(&vdp1.vram[a & 0x7FFFF & ~uint32_t(size - 1)], size);
  if (a >= 0x05C80000 && a < 0x05D00000) {
    const uint32_t o = a & 0x3FFFF;
    if (size == 4) return uint32_t(vdp1.fbRead16(o & ~3u)) << 16 | vdp1.fbRead16((o & ~3u) + 2);
    const uint16_t v = vdp1.fbRead16(o & ~1u);
    return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
  }
  if (a >= 0x05D00000 && a < 0x05E00000) {
    const uint32_t o = a & 0x1F;
    if (size == 4) return uint32_t(vdp1.readReg(o & ~3u)) << 16 | vdp1.readReg((o & ~3u) + 2);
    const uint16_t v = vdp1.readReg(o & ~1u);
    return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
  }
  if (a >= 0x05E00000 && a < 0x05F00000) return rdBE(&vdp2.vram[a & 0x7FFFF & ~uint32_t(size - 1)], size);
  if (a >= 0x05F00000 && a < 0x05F80000) return rdBE(&vdp2.cram[a & 0xFFF & ~uint32_t(size - 1)], size);
  if (a >= 0x05F80000 && a < 0x05FC0000) {
    const uint32_t o = a & 0x1FF;
    if (size == 4) return uint32_t(vdp2.readReg(o & ~3u)) << 16 | vdp2.readReg((o & ~3u) + 2);
    const uint16_t v = vdp2.readReg(o & ~1u);
    return size == 2 ? v : (a & 1) ? (v & 0xFF) : (v >> 8);
  }
  if (a >= 0x05FE0000 && a < 0x05FF0000) {
    const uint32_t v = scu.read(a & 0xFC);
    if (size == 4) return v;
    if (size == 2) return (a & 2) ? (v & 0xFFFF) : (v >> 16);
    return (v >> ((3 - (a & 3)) * 8)) & 0xFF;
  }
  // cartucho ausente e áreas vazias
  return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

void System::busWrite(uint32_t a, int size, uint32_t v) {
  a &= 0x07FFFFFF;
  if (a < 0x00100000) return;  // ROM
  if (a < 0x00180000) {
    const uint32_t reg = (a >> 1) & 0x3F;
    if (size == 1) { if (a & 1) smpc.write(reg, uint8_t(v)); }
    else if (size == 2) smpc.write(reg, uint8_t(v));
    else { smpc.write(reg, uint8_t(v >> 16)); smpc.write(reg + 1, uint8_t(v)); }
    return;
  }
  if (a < 0x00200000) {
    const uint32_t i = (a >> 1) & 0x7FFF;
    auto put = [&](uint32_t k, uint8_t b) { if (backup[k] != b) { backup[k] = b; backupDirty = true; } };
    if (size == 1) { if (a & 1) put(i, uint8_t(v)); }
    else if (size == 2) put(i, uint8_t(v));
    else { put(i, uint8_t(v >> 16)); put((i + 1) & 0x7FFF, uint8_t(v)); }
    return;
  }
  if (a < 0x00400000) { wrBE(&lwram[a & 0xFFFFF & ~uint32_t(size - 1)], size, v); return; }
  if (a >= 0x06000000) { wrBE(&hwram[a & 0xFFFFF & ~uint32_t(size - 1)], size, v); return; }
  if (a >= 0x01000000 && a < 0x02000000) {  // pulso FTI: 0x01000000 = escravo, 0x01800000 = mestre
    ((a & 0x00800000) ? master : slave).inputCapture();
    return;
  }
  if (a >= 0x05800000 && a < 0x05900000) {
    const uint32_t o = a & 0xFFFFF;
    if (o >= 0x18000 && o < 0x19000) {  // porta de dados (PUT SECTOR DATA)
      if (size == 4) { cd.writeData(uint16_t(v >> 16)); cd.writeData(uint16_t(v)); return; }
      cd.writeData(uint16_t(size == 1 ? (v & 0xFF) * 0x101 : v));
      return;
    }
    if (o >= 0x90000) {
      if ((o & 0x3C) == 0) {  // porta de dados também aqui
        if (size == 4) { cd.writeData(uint16_t(v >> 16)); cd.writeData(uint16_t(v)); return; }
        cd.writeData(uint16_t(size == 1 ? (v & 0xFF) * 0x101 : v));
        return;
      }
      if (size == 4) { cd.write(o & 0x3C, uint16_t(v)); return; }
      cd.write(o & 0x3C, uint16_t(size == 1 ? (v & 0xFF) * 0x101 : v));
    }
    return;
  }
  if (a >= 0x05A00000 && a < 0x05B00000) { wrBE(&scsp.ram[a & 0x7FFFF & ~uint32_t(size - 1)], size, v); return; }
  if (a >= 0x05B00000 && a < 0x05C00000) {
    const uint32_t o = a & 0xFFF;
    if (size == 4) { scsp.writeReg(o & ~3u, uint16_t(v >> 16), 0xFFFF); scsp.writeReg((o & ~3u) + 2, uint16_t(v), 0xFFFF); }
    else if (size == 2) scsp.writeReg(o & ~1u, uint16_t(v), 0xFFFF);
    else scsp.writeReg(o & ~1u, uint16_t((v & 0xFF) * 0x101), (a & 1) ? 0x00FF : 0xFF00);
    return;
  }
  if (a >= 0x05C00000 && a < 0x05C80000) { wrBE(&vdp1.vram[a & 0x7FFFF & ~uint32_t(size - 1)], size, v); return; }
  if (a >= 0x05C80000 && a < 0x05D00000) {
    const uint32_t o = a & 0x3FFFF;
    if (size == 4) { vdp1.fbWrite16(o & ~3u, uint16_t(v >> 16)); vdp1.fbWrite16((o & ~3u) + 2, uint16_t(v)); }
    else if (size == 2) vdp1.fbWrite16(o & ~1u, uint16_t(v));
    else vdp1.fbWrite8(o, uint8_t(v));
    return;
  }
  if (a >= 0x05D00000 && a < 0x05E00000) {
    const uint32_t o = a & 0x1F;
    if (size == 4) { vdp1.writeReg(o & ~3u, uint16_t(v >> 16)); vdp1.writeReg((o & ~3u) + 2, uint16_t(v)); }
    else vdp1.writeReg(o & ~1u, uint16_t(v));
    return;
  }
  if (a >= 0x05E00000 && a < 0x05F00000) { wrBE(&vdp2.vram[a & 0x7FFFF & ~uint32_t(size - 1)], size, v); return; }
  if (a >= 0x05F00000 && a < 0x05F80000) { wrBE(&vdp2.cram[a & 0xFFF & ~uint32_t(size - 1)], size, v); return; }
  if (a >= 0x05F80000 && a < 0x05FC0000) {
    const uint32_t o = a & 0x1FF;
    if (size == 4) { vdp2.writeReg(o & ~3u, uint16_t(v >> 16)); vdp2.writeReg((o & ~3u) + 2, uint16_t(v)); }
    else vdp2.writeReg(o & ~1u, uint16_t(v));
    return;
  }
  if (a >= 0x05FE0000 && a < 0x05FF0000) {
    const uint32_t off = a & 0xFC;
    if (size == 4) scu.write(off, v, 0xFFFFFFFF);
    else if (size == 2) scu.write(off, (a & 2) ? v : v << 16, (a & 2) ? 0x0000FFFF : 0xFFFF0000);
    else { const int sh = (3 - (a & 3)) * 8; scu.write(off, (v & 0xFF) << sh, 0xFFu << sh); }
  }
}

void System::runCpus(int64_t until) {
  // os dois SH-2 andam em fatias curtas para a comunicação pela memória e pelo FTI ficar perto do aparelho
  while (master.clock < until) {
    const int64_t to = std::min<int64_t>(until, master.clock + 64);
    const int64_t from = master.clock;
    master.run(to);
    if (smpc.slaveOn) slave.run(to);
    else slave.clock = to;
    scu.tick(int(master.clock - from));
  }
}

void System::runFrame() {
  const int vis = vdp2.visibleLines();
  for (line = 0; line < lines(); line++) {
    if (line == 0) {
      inVblank = false;
      vdp1.vblankOut();
      scu.vblankOut();
      scu.raise(INT_VBOUT);
      scu.factor(DF_VBOUT);
    }
    if (line == vis) {
      inVblank = true;
      vdp2.setStatus(true, false, line, frames & 1);
      vdp2.render();
      vdp1.vblankIn();
      scu.raise(INT_VBIN);
      scu.factor(DF_VBIN);
      smpc.vblankIn();
    }
    inHblank = false;
    vdp2.setStatus(inVblank, false, line, frames & 1);
    slaveIrl();
    runCpus(lineBase_ + HBLANK_AT);
    inHblank = true;
    vdp2.setStatus(inVblank, true, line, frames & 1);
    slaveIrl();
    scu.lineStart(line);
    scu.raise(INT_HBIN);
    scu.factor(DF_HBIN);
    runCpus(lineBase_ + CYCLES_PER_LINE);
    lineBase_ += CYCLES_PER_LINE;
    scu.dspRun(CYCLES_PER_LINE / 2);
    cd.run(CYCLES_PER_LINE);
    vdp1.run(CYCLES_PER_LINE);
    // som: o 68000 roda 256 ciclos por amostra (11,2896 MHz / 44,1 kHz)
    audioFrac_ += uint32_t(AUDIO_HZ) * CYCLES_PER_LINE;
    while (audioFrac_ >= uint32_t(CPU_HZ)) {
      audioFrac_ -= uint32_t(CPU_HZ);
      int16_t l = 0, r = 0, el = 0, er = 0;
      if (cd.audioOut(el, er)) scsp.cdda(el, er); else scsp.cdda(0, 0);
      if (scsp.cpuOn) scsp.run68k(256);
      scsp.sample(l, r);
      audio.push_back(l);
      audio.push_back(r);
    }
  }
  line = 0;
  audioCut = 0;
  if (audio.size() > 44100 * 2) {
    audioCut = (audio.size() - 44100) / 2;
    audio.erase(audio.begin(), audio.end() - 44100);
  }
  frames++;
}

void System::sync(StateIO& io) {
  io.bytes(lwram);
  io.bytes(hwram);
  io.bytes(backup);
  master.sync(io);
  slave.sync(io);
  scu.sync(io);
  smpc.sync(io);
  vdp1.sync(io);
  vdp2.sync(io);
  scsp.sync(io);
  cd.sync(io);
  io.sync(lineBase_);
  io.sync(audioFrac_);
  io.sync(frames);
  io.sync(inVblank);
  io.sync(inHblank);
  if (io.reading()) {
    audioFrac_ %= uint32_t(CPU_HZ);
    // relógios coerentes: um estado adulterado não pode mandar uma CPU rodar bilhões de ciclos
    if (lineBase_ < master.clock - CYCLES_PER_LINE * 4 || lineBase_ > master.clock + CYCLES_PER_LINE * 4) lineBase_ = master.clock;
    if (slave.clock < master.clock - CYCLES_PER_LINE * 4 || slave.clock > master.clock + CYCLES_PER_LINE * 4) slave.clock = master.clock;
    mapMemory();
  }
}

// ------------------------------------------------------------------------------------------------ SMPC

void Smpc::reset() {
  std::memset(ireg_, 0, sizeof ireg_);
  std::memset(oreg_, 0, sizeof oreg_);
  sr_ = sf_ = comreg_ = 0;
  pdr_[0] = pdr_[1] = 0x7F;
  ddr_[0] = ddr_[1] = 0;
  iosel_ = exle_ = 0;
  slaveOn = soundOn = false;
  pendingPad_ = false;
}

void Smpc::setRtc(const uint8_t bcd[7]) { std::memcpy(rtc_, bcd, 7); }

uint8_t Smpc::read(uint32_t reg) {
  reg &= 0x3F;
  if (reg >= 0x10 && reg < 0x30) return oreg_[reg - 0x10];
  switch (reg) {
    case 0x30: return sr_;
    case 0x31: return sf_;
    case 0x3A: case 0x3B: {
      // modo direto (IOSEL): controle padrão nas portas 1 e 2 (a 2 só com o jogador 2), sem o protocolo TH/TR completo
      const int p = reg - 0x3A;
      if (p == 1 && !pad2On_) return 0x7F;
      const uint8_t th = pdr_[p] & 0x40, tr = pdr_[p] & 0x20;
      const uint16_t b = uint16_t(~(p ? pad2_ : pad_));
      uint8_t v;
      if (th && tr) v = uint8_t(0x70 | ((b >> 3) & 0x08) | 0x04);              // L em bit 3
      else if (!th && tr) v = uint8_t(0x30 | ((b >> 8) & 0x0F));            // B C A Start? (D3..D0 = Start A C B)
      else if (th && !tr) v = uint8_t(0x50 | ((b >> 12) & 0x0F));           // direita esquerda baixo cima
      else v = uint8_t(0x10 | ((b >> 4) & 0x0F));                           // R X Y Z
      return uint8_t((v & ~ddr_[p]) | (pdr_[p] & ddr_[p]));
    }
    default: return 0xFF;
  }
}

void Smpc::write(uint32_t reg, uint8_t v) {
  reg &= 0x3F;
  if (reg < 7) {
    ireg_[reg] = v;
    if (reg == 0 && pendingPad_) {
      if (v & 0x40) { pendingPad_ = false; sr_ &= 0x0F; sf_ = 0; }   // break
      else if (v & 0x80) peripheral();                               // continue
    }
    return;
  }
  switch (reg) {
    case 0x0F: comreg_ = v; command(v); break;
    case 0x31: sf_ = 1; break;
    case 0x3A: pdr_[0] = v & 0x7F; break;
    case 0x3B: pdr_[1] = v & 0x7F; break;
    case 0x3C: ddr_[0] = v & 0x7F; break;
    case 0x3D: ddr_[1] = v & 0x7F; break;
    case 0x3E: iosel_ = v & 3; break;
    case 0x3F: exle_ = v & 3; break;
    default: break;
  }
}

void Smpc::command(uint8_t c) {
  switch (c) {
    case 0x02: sys_.slavePower(true); break;    // SSHON
    case 0x03: sys_.slavePower(false); break;   // SSHOFF
    case 0x06: sys_.soundPower(true); break;    // SNDON
    case 0x07: sys_.soundPower(false); break;   // SNDOFF
    case 0x0E: case 0x0F: sys_.slavePower(false); break;  // CKCHG352/320: o escravo para
    case 0x10: {                                // INTBACK
      pendingPad_ = false;
      const bool wantStatus = (ireg_[0] & 0x0F) != 0, wantPad = (ireg_[1] & 0x08) != 0;
      if (wantStatus) {
        oreg_[0] = 0x80;                          // relógio válido, reset liberado
        std::memcpy(oreg_ + 1, rtc_, 7);
        oreg_[8] = 0;
        oreg_[9] = sys_.area;                     // região (do disco)
        oreg_[10] = uint8_t(0x24 | (slaveOn << 4) | 0x08 | 0x02 | (soundOn ? 1 : 0));
        oreg_[11] = 0x02 | 0x40;
        std::memcpy(oreg_ + 12, smem_, 4);
        sr_ = uint8_t(0x0F | (wantPad ? 0x20 : 0));
        pendingPad_ = wantPad;
        sys_.scu.raise(INT_SMPC);
      } else if (wantPad) {
        peripheral();
        oreg_[31] = c;
        return;
      }
      break;
    }
    case 0x16: std::memcpy(rtc_, ireg_, 7); break;   // SETTIME
    case 0x17: std::memcpy(smem_, ireg_, 4); break;  // SETSMEM
    default: break;  // MSHON, CDON/CDOFF, SYSRES, NMIREQ, RESENAB/RESDISA: sem efeito aqui
  }
  oreg_[31] = c;
  sf_ = 0;
}

void Smpc::peripheral() {
  // porta 1: controle padrão ligado direto; porta 2: vazia ou o controle do jogador 2
  std::memset(oreg_, 0xFF, 31);
  const uint16_t b = uint16_t(~pad_);
  oreg_[0] = 0xF1;
  oreg_[1] = 0x02;
  oreg_[2] = uint8_t(b >> 8);
  oreg_[3] = uint8_t(b | 0x07);
  oreg_[4] = 0xF0;
  if (pad2On_) {
    const uint16_t b2 = uint16_t(~pad2_);
    oreg_[4] = 0xF1;
    oreg_[5] = 0x02;
    oreg_[6] = uint8_t(b2 >> 8);
    oreg_[7] = uint8_t(b2 | 0x07);
  }
  sr_ = 0xC0 | 0x01;   // dados de periférico, primeiro bloco, sem mais dados
  pendingPad_ = false;
  sf_ = 0;
  sys_.scu.raise(INT_SMPC);
}

void Smpc::vblankIn() {}

void Smpc::sync(StateIO& io) {
  io.raw(ireg_, sizeof ireg_);
  io.raw(oreg_, sizeof oreg_);
  io.sync(sr_); io.sync(sf_); io.sync(comreg_);
  io.raw(pdr_, 2); io.raw(ddr_, 2); io.sync(iosel_); io.sync(exle_);
  io.raw(smem_, 4); io.raw(rtc_, 7);
  io.sync(pad_);
  io.sync(pendingPad_);
  io.sync(slaveOn);
  io.sync(soundOn);
}

// ------------------------------------------------------------------------------------------------ núcleo

namespace {
std::string discTitle(cdrom::Disc& d) {
  uint8_t buf[cdrom::DATA_SECTOR];
  for (const auto& t : d.tracks()) {
    if (t.type == cdrom::TrackType::Audio) continue;
    if (!d.readData(t.start, buf)) break;
    // nome do jogo no cabeçalho (IP.BIN): 0x60, 112 bytes
    std::string s(reinterpret_cast<const char*>(buf + 0x60), 112);
    while (!s.empty() && (s.back() == ' ' || s.back() == 0)) s.pop_back();
    bool ok = !s.empty();
    for (char c : s) if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) ok = false;
    if (ok) return s;
    break;
  }
  return d.name();
}
}  // namespace

SaturnCore::SaturnCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios)
    : disc_(std::move(disc)) {
  const std::vector<uint8_t>& b = bios ? *bios : firmware("saturn");
  if (b.empty()) throw std::runtime_error("o Saturn precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo sega_101.bin) uma vez no Índigo.");
  sys_ = std::make_unique<System>(b, disc_.get());
  if (save) importSave(*save);
  title_ = disc_ ? discTitle(*disc_) : "Saturn";
  note_ = contains(b, "Indigo Saturn") ? " · BIOS de teste do Índigo" : " · Saturn experimental: muitos jogos ainda não rodam";
}

namespace {
uint16_t padBits(uint32_t k) {
  // byte de cima: direita esquerda baixo cima Start A C B; de baixo: R X Y Z L
  uint16_t p = 0;
  if (k & KEY_RIGHT) p |= 0x8000;
  if (k & KEY_LEFT) p |= 0x4000;
  if (k & KEY_DOWN) p |= 0x2000;
  if (k & KEY_UP) p |= 0x1000;
  if (k & KEY_START) p |= 0x0800;
  if (k & KEY_Y) p |= 0x0400;      // A (fileira de baixo: esquerda)
  if (k & KEY_B) p |= 0x0200;      // C (direita)
  if (k & KEY_A) p |= 0x0100;      // B (meio)
  if (k & KEY_R) p |= 0x0080;
  if (k & KEY_X) p |= 0x0040;      // X
  if (k & KEY_SELECT) p |= 0x0020; // Y
  if (k & KEY_L) p |= 0x0008;
  return p;
}
}  // namespace

void SaturnCore::setKeys(uint32_t k) { sys_->setPad(padBits(k)); }
void SaturnCore::setKeys2(uint32_t k) { sys_->setPad2(padBits(k)); }

size_t SaturnCore::audioAvailable() const {
  const double frames = double(sys_->audio.size() / 2);
  const double n = (frames - 1 - audioPos_) * rate_ / System::AUDIO_HZ;
  return n > 0 ? size_t(n) : 0;
}

void SaturnCore::pullAudio(float* l, float* r, size_t n) {
  auto& a = sys_->audio;
  const size_t frames = a.size() / 2;
  const double step = double(System::AUDIO_HZ) / rate_;
  for (size_t i = 0; i < n; i++) {
    const size_t k = size_t(audioPos_);
    if (k + 1 >= frames) { l[i] = r[i] = 0; continue; }
    const double f = audioPos_ - double(k);
    l[i] = float((a[2 * k] * (1 - f) + a[2 * k + 2] * f) / 32768.0);
    r[i] = float((a[2 * k + 1] * (1 - f) + a[2 * k + 3] * f) / 32768.0);
    audioPos_ += step;
  }
  const size_t used = std::min(size_t(audioPos_), frames);
  a.erase(a.begin(), a.begin() + long(used * 2));
  audioPos_ -= double(used);
}

void SaturnCore::importSave(const std::vector<uint8_t>& b) {
  if (b.size() < 64) return;
  std::fill(sys_->backup.begin(), sys_->backup.end(), 0);
  std::copy(b.begin(), b.begin() + std::min(b.size(), sys_->backup.size()), sys_->backup.begin());
  sys_->backupDirty = false;
}

std::vector<uint8_t> SaturnCore::saveState() {
  StateIO io;
  uint32_t magic = STATE_MAGIC;
  io.sync(magic);
  sys_->sync(io);
  return io.data();
}

void SaturnCore::loadState(const std::vector<uint8_t>& s) {
  StateIO probe(s);
  uint32_t magic = 0;
  probe.sync(magic);
  if (magic != STATE_MAGIC) throw std::runtime_error("Estado salvo não é deste console.");
  const std::vector<uint8_t> before = saveState();
  try {
    sys_->sync(probe);
  } catch (...) {
    StateIO back(before);
    uint32_t m = 0;
    back.sync(m);
    sys_->sync(back);
    throw;
  }
  // a fila de som continua (um quadro, no máximo): o estado recomeça na mesma amostra
}

}  // namespace indigo::saturn
