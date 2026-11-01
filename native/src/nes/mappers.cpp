// Cabeçalho iNES / NES 2.0 e os mapeadores mais comuns: 0 NROM, 1 MMC1 (SxROM, inclusive SUROM 512 KB),
// 2 UxROM, 3 CNROM, 4 MMC3 (TxROM, IRQ por linha pelo A12), 7 AxROM, 9 MMC2 (Punch-Out!!), 11 Color Dreams,
// 66 GxROM, 71 Camerica, 10 MMC4, 34 BNROM/NINA-001, 79 NINA-03/06, 140 Jaleco, 206 Namco 108,
// 21/22/23/25 Konami VRC2/VRC4. Os chips com som ou vídeo próprios ficam em mappers_ext.cpp.
#include <algorithm>

#include "nes.h"

namespace indigo::nes {

bool isNesRom(const std::vector<uint8_t>& d) {
  return d.size() >= 16 && d[0] == 'N' && d[1] == 'E' && d[2] == 'S' && d[3] == 0x1A;
}

bool parseRom(const std::vector<uint8_t>& d, Cart& c, std::string& error) {
  if (!isNesRom(d)) { error = "Não é uma ROM de NES (.nes)."; return false; }
  const bool nes2 = (d[7] & 0x0C) == 0x08;
  size_t prgBanks = d[4], chrBanks = d[5];
  c.mapper = (d[6] >> 4) | (d[7] & 0xF0);
  if (nes2) {
    c.mapper |= (d[8] & 0x0F) << 8;
    c.submapper = d[8] >> 4;
    const int vol = d[10] & 0x0F, nv = d[10] >> 4;
    c.ramSize = std::max(vol ? size_t(64) << vol : 0, nv ? size_t(64) << nv : 0);
    if ((d[9] & 0x0F) != 0x0F) prgBanks |= size_t(d[9] & 0x0F) << 8;
    if ((d[9] & 0xF0) != 0xF0) chrBanks |= size_t(d[9] & 0xF0) << 4;
  } else if (d[12] || d[13] || d[14] || d[15]) {
    c.mapper &= 0x0F;  // cabeçalho antigo com lixo no fim ("DiskDude!"): só o nibble baixo vale
  }
  c.vertical = d[6] & 1;
  c.battery = d[6] & 2;
  c.fourScreen = d[6] & 8;
  size_t off = 16 + ((d[6] & 4) ? 512 : 0);
  const size_t prgSize = prgBanks * 0x4000, chrSize = chrBanks * 0x2000;
  if (prgSize == 0 || off + prgSize > d.size()) { error = "ROM de NES incompleta."; return false; }
  c.prg.assign(d.begin() + off, d.begin() + off + prgSize);
  off += prgSize;
  if (chrSize) {
    const size_t n = std::min(chrSize, d.size() - off);
    c.chr.assign(d.begin() + off, d.begin() + off + n);
    c.chr.resize(chrSize, 0);
    c.chrRam = false;
  } else {
    c.chr.assign(0x2000, 0);
    c.chrRam = true;
  }
  // 32 KB de RAM no SxROM com 512 KB não existe aqui; 8 KB atende quase tudo
  c.prgRam.assign(0x2000, 0);
  return true;
}

// ---------------------------------------------------------------- bancos
int Mapper::prgBank8(int bank) const {
  const int n = static_cast<int>(cart.prg.size() / 0x2000);
  bank %= n;
  if (bank < 0) bank += n;
  return bank * 0x2000;
}
void Mapper::prg8(int slot, int bank) { prgOff[slot] = static_cast<uint32_t>(prgBank8(bank)); }
void Mapper::prg16(int slot, int bank) { prg8(slot * 2, bank * 2); prg8(slot * 2 + 1, bank * 2 + 1); }
void Mapper::prg32(int bank) { for (int k = 0; k < 4; k++) prg8(k, bank * 4 + k); }
void Mapper::chr1(int slot, int bank) {
  const int n = static_cast<int>(cart.chr.size() / 0x400);
  bank %= n;
  if (bank < 0) bank += n;
  chrOff[slot] = static_cast<uint32_t>(bank) * 0x400;
}
void Mapper::chr2(int slot, int bank) { chr1(slot * 2, bank * 2); chr1(slot * 2 + 1, bank * 2 + 1); }
void Mapper::chr4(int slot, int bank) { for (int k = 0; k < 4; k++) chr1(slot * 4 + k, bank * 4 + k); }
void Mapper::chr8(int bank) { for (int k = 0; k < 8; k++) chr1(k, bank * 8 + k); }
void Mapper::mirror(int m) {
  if (cart.fourScreen) m = FOUR;
  static const uint8_t T[5][4] = {{0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 2, 3}};
  for (int k = 0; k < 4; k++) nt[k] = T[m][k];
}

namespace {

// ---------------------------------------------------------------- 0, 2, 3, 7, 11, 66, 71 (registrador único)
class Simple : public Mapper {
public:
  Simple(Cart& c, int id) : Mapper(c), id_(id) {}
  void reset() override { reg_ = 0; update(); }
  void update() override {
    const int fixedMirror = cart.vertical ? VERTICAL : HORIZONTAL;
    switch (id_) {
      case 0: prg16(0, 0); prg16(1, 1); chr8(0); mirror(fixedMirror); break;
      case 2: prg16(0, reg_); prg16(1, -1); chr8(0); mirror(fixedMirror); break;
      case 3: prg16(0, 0); prg16(1, 1); chr8(reg_); mirror(fixedMirror); break;
      case 7: prg32(reg_ & 7); chr8(0); mirror((reg_ & 0x10) ? SINGLE1 : SINGLE0); break;
      case 11: prg32(reg_ & 3); chr8(reg_ >> 4); mirror(fixedMirror); break;
      case 66: prg32((reg_ >> 4) & 3); chr8(reg_ & 3); mirror(fixedMirror); break;
      case 71:
        prg16(0, reg_); prg16(1, -1); chr8(0);
        mirror(mirror71_ < 0 ? fixedMirror : (mirror71_ ? SINGLE1 : SINGLE0));
        break;
      default: break;
    }
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    if (id_ == 71) {
      if (a < 0xA000 && a >= 0x9000) mirror71_ = (v >> 4) & 1;  // Fire Hawk
      else if (a >= 0xC000) reg_ = v;
    } else if (id_ == 0) {
      return;
    } else {
      // conflito de barramento (a ROM também responde): o valor gravado é o E dos dois
      reg_ = (id_ == 2 || id_ == 3 || id_ == 66 || id_ == 11) ? (v & prgRead(a)) : v;
    }
    update();
  }
  void syncRegs(StateIO& io) override { io.sync(reg_); io.sync(mirror71_); }

private:
  int id_;
  uint8_t reg_ = 0;
  int mirror71_ = -1;
};

// ---------------------------------------------------------------- 1: MMC1
class MMC1 : public Mapper {
public:
  explicit MMC1(Cart& c) : Mapper(c) {}
  void reset() override { shift_ = 0x10; ctrl_ |= 0x0C; update(); }
  void update() override {
    static const int M[4] = {SINGLE0, SINGLE1, VERTICAL, HORIZONTAL};
    mirror(M[ctrl_ & 3]);
    // SUROM (512 KB): o bit 4 do banco de CHR escolhe a metade de 256 KB do PRG
    const int outer = cart.prg.size() > 0x40000 ? (chr0_ & 0x10) : 0;
    const int bank = (prg_ & 0x0F) | outer;
    switch ((ctrl_ >> 2) & 3) {
      case 0: case 1: prg16(0, bank & ~1); prg16(1, bank | 1); break;
      case 2: prg16(0, outer); prg16(1, bank); break;
      case 3: prg16(0, bank); prg16(1, 0x0F | outer); break;
    }
    if (ctrl_ & 0x10) { chr4(0, chr0_); chr4(1, chr1_); }
    else chr8(chr0_ >> 1);
    ramEnabled = !(prg_ & 0x10);
    ramWritable = ramEnabled;
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    // duas escritas em ciclos seguidos (instruções de leitura-modificação-escrita): só a primeira vale
    const uint64_t now = nes ? nes->cycles : 0;
    const bool consecutive = now == last_ + 1;
    last_ = now;
    if (consecutive) return;
    if (v & 0x80) { shift_ = 0x10; ctrl_ |= 0x0C; update(); return; }
    const bool full = shift_ & 1;
    shift_ = static_cast<uint8_t>((shift_ >> 1) | ((v & 1) << 4));
    if (!full) return;
    const uint8_t val = shift_;
    shift_ = 0x10;
    switch ((a >> 13) & 3) {
      case 0: ctrl_ = val; break;
      case 1: chr0_ = val; break;
      case 2: chr1_ = val; break;
      case 3: prg_ = val; break;
    }
    update();
  }
  void syncRegs(StateIO& io) override { io.sync(shift_); io.sync(ctrl_); io.sync(chr0_); io.sync(chr1_); io.sync(prg_); io.sync(last_); }

private:
  uint8_t shift_ = 0x10, ctrl_ = 0x0C, chr0_ = 0, chr1_ = 0, prg_ = 0;
  uint64_t last_ = ~0ull;
};

// ---------------------------------------------------------------- 4: MMC3
class MMC3 : public Mapper {
public:
  explicit MMC3(Cart& c) : Mapper(c) {}
  void reset() override {
    select_ = 0;
    r_[0] = 0; r_[1] = 2; r_[2] = 4; r_[3] = 5; r_[4] = 6; r_[5] = 7; r_[6] = 0; r_[7] = 1;
    mirrorReg_ = cart.vertical ? 0 : 1;
    ramReg_ = 0x80;
    latch_ = counter_ = 0;
    reload_ = enabled_ = false;
    irq = false;
    update();
  }
  void update() override {
    const bool prgMode = select_ & 0x40, chrMode = select_ & 0x80;
    prg8(prgMode ? 2 : 0, r_[6]);
    prg8(1, r_[7]);
    prg8(prgMode ? 0 : 2, -2);
    prg8(3, -1);
    const int base = chrMode ? 4 : 0, other = chrMode ? 0 : 4;
    chr1(base + 0, r_[0] & 0xFE); chr1(base + 1, r_[0] | 1);
    chr1(base + 2, r_[1] & 0xFE); chr1(base + 3, r_[1] | 1);
    for (int k = 0; k < 4; k++) chr1(other + k, r_[2 + k]);
    mirror(mirrorReg_ ? HORIZONTAL : VERTICAL);
    ramEnabled = ramReg_ & 0x80;
    ramWritable = ramEnabled && !(ramReg_ & 0x40);
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    const bool odd = a & 1;
    switch ((a >> 13) & 3) {
      case 0: if (odd) r_[select_ & 7] = v; else select_ = v; update(); break;
      case 1: if (odd) ramReg_ = v; else mirrorReg_ = v & 1; update(); break;
      case 2: if (odd) { counter_ = 0; reload_ = true; } else latch_ = v; break;
      case 3: if (odd) enabled_ = true; else { enabled_ = false; irq = false; } break;
    }
  }
  // o contador anda a cada subida do A12 que vem depois de o A12 ficar baixo por um tempo (filtro do chip)
  void ppuAddress(uint16_t a) override {
    const bool a12 = a & 0x1000;
    if (a12 && !a12_) {
      if (nes->ppuDots - lowSince_ >= 10) clock();
    } else if (!a12 && a12_) {
      lowSince_ = nes->ppuDots;
    }
    a12_ = a12;
  }
  void syncRegs(StateIO& io) override {
    io.sync(select_); io.raw(r_, sizeof r_); io.sync(mirrorReg_); io.sync(ramReg_);
    io.sync(latch_); io.sync(counter_); io.sync(reload_); io.sync(enabled_); io.sync(a12_); io.sync(lowSince_); io.sync(irq);
  }

private:
  void clock() {
    if (counter_ == 0 || reload_) counter_ = latch_;
    else counter_--;
    if (counter_ == 0 && enabled_) irq = true;
    reload_ = false;
  }
  uint8_t select_ = 0, r_[8] = {}, mirrorReg_ = 0, ramReg_ = 0x80, latch_ = 0, counter_ = 0;
  bool reload_ = false, enabled_ = false, a12_ = false;
  uint64_t lowSince_ = 0;
};

// ---------------------------------------------------------------- 9: MMC2 (troca de CHR pelos blocos $FD/$FE)
class MMC2 : public Mapper {
public:
  explicit MMC2(Cart& c) : Mapper(c) {}
  void reset() override { prgReg_ = 0; for (auto& b : chr_) b = 0; latch_[0] = latch_[1] = 0xFE; mirrorReg_ = 0; update(); }
  void update() override {
    prg8(0, prgReg_); prg8(1, -3); prg8(2, -2); prg8(3, -1);
    chr4(0, chr_[latch_[0] == 0xFD ? 0 : 1]);
    chr4(1, chr_[latch_[1] == 0xFD ? 2 : 3]);
    mirror(mirrorReg_ ? HORIZONTAL : VERTICAL);
  }
  void writeReg(uint16_t a, uint8_t v) override {
    switch (a >> 12) {
      case 0xA: prgReg_ = v & 0xF; break;
      case 0xB: chr_[0] = v & 0x1F; break;
      case 0xC: chr_[1] = v & 0x1F; break;
      case 0xD: chr_[2] = v & 0x1F; break;
      case 0xE: chr_[3] = v & 0x1F; break;
      case 0xF: mirrorReg_ = v & 1; break;
      default: return;
    }
    update();
  }
  uint8_t chrRead(uint16_t a) override {
    const uint8_t v = Mapper::chrRead(a);
    if (a == 0x0FD8) { latch_[0] = 0xFD; update(); }
    else if (a == 0x0FE8) { latch_[0] = 0xFE; update(); }
    else if (a >= 0x1FD8 && a <= 0x1FDF) { latch_[1] = 0xFD; update(); }
    else if (a >= 0x1FE8 && a <= 0x1FEF) { latch_[1] = 0xFE; update(); }
    return v;
  }
  void syncRegs(StateIO& io) override { io.sync(prgReg_); io.raw(chr_, sizeof chr_); io.raw(latch_, sizeof latch_); io.sync(mirrorReg_); }

private:
  uint8_t prgReg_ = 0, chr_[4] = {}, latch_[2] = {0xFE, 0xFE}, mirrorReg_ = 0;
};


// ---------------------------------------------------------------- 10: MMC4 (Fire Emblem; como o MMC2 com PRG de 16 KB)
class MMC4 : public Mapper {
public:
  explicit MMC4(Cart& c) : Mapper(c) {}
  void reset() override { prgReg_ = 0; for (auto& b : chr_) b = 0; latch_[0] = latch_[1] = 0xFE; mirrorReg_ = 0; update(); }
  void update() override {
    prg16(0, prgReg_); prg16(1, -1);
    chr4(0, chr_[latch_[0] == 0xFD ? 0 : 1]);
    chr4(1, chr_[latch_[1] == 0xFD ? 2 : 3]);
    mirror(mirrorReg_ ? HORIZONTAL : VERTICAL);
  }
  void writeReg(uint16_t a, uint8_t v) override {
    switch (a >> 12) {
      case 0xA: prgReg_ = v & 0xF; break;
      case 0xB: chr_[0] = v & 0x1F; break;
      case 0xC: chr_[1] = v & 0x1F; break;
      case 0xD: chr_[2] = v & 0x1F; break;
      case 0xE: chr_[3] = v & 0x1F; break;
      case 0xF: mirrorReg_ = v & 1; break;
      default: return;
    }
    update();
  }
  uint8_t chrRead(uint16_t a) override {
    const uint8_t v = Mapper::chrRead(a);
    if (a >= 0x0FD8 && a <= 0x0FDF) { latch_[0] = 0xFD; update(); }
    else if (a >= 0x0FE8 && a <= 0x0FEF) { latch_[0] = 0xFE; update(); }
    else if (a >= 0x1FD8 && a <= 0x1FDF) { latch_[1] = 0xFD; update(); }
    else if (a >= 0x1FE8 && a <= 0x1FEF) { latch_[1] = 0xFE; update(); }
    return v;
  }
  void syncRegs(StateIO& io) override { io.sync(prgReg_); io.raw(chr_, sizeof chr_); io.raw(latch_, sizeof latch_); io.sync(mirrorReg_); }

private:
  uint8_t prgReg_ = 0, chr_[4] = {}, latch_[2] = {0xFE, 0xFE}, mirrorReg_ = 0;
};

// ---------------------------------------------------------------- 34, 79, 140: registradores de PRG/CHR de 32/8 KB
class Discrete : public Mapper {
public:
  Discrete(Cart& c, int id) : Mapper(c), id_(id) {}
  void reset() override {
    prg_ = chr0_ = 0; chr1_ = 1;
    // NINA-001 (34 com CHR em ROM maior que 8 KB) usa a RAM em $6000; o 140 ocupa $6000 com o registrador
    if (id_ == 140) { ramEnabled = false; ramWritable = false; }
    update();
  }
  void update() override {
    prg32(prg_);
    if (id_ == 34 && nina()) { chr4(0, chr0_); chr4(1, chr1_); }
    else chr8(chr0_);
    mirror(cart.vertical ? VERTICAL : HORIZONTAL);
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (id_ == 34) {
      if (nina()) {
        if (a == 0x7FFD) prg_ = v & 1;
        else if (a == 0x7FFE) chr0_ = v & 15;
        else if (a == 0x7FFF) chr1_ = v & 15;
        else return;
      } else if (a >= 0x8000) {
        prg_ = v & prgRead(a);
      } else {
        return;
      }
    } else if (id_ == 79) {
      if (a < 0x4100 || a >= 0x6000 || !(a & 0x100)) return;
      prg_ = (v >> 3) & 1;
      chr0_ = v & 7;
    } else {  // 140
      if (a < 0x6000 || a >= 0x8000) return;
      prg_ = (v >> 4) & 3;
      chr0_ = v & 15;
    }
    update();
  }
  void syncRegs(StateIO& io) override { io.sync(prg_); io.sync(chr0_); io.sync(chr1_); }

private:
  bool nina() const { return !cart.chrRam && cart.chr.size() > 0x2000; }
  int id_;
  uint8_t prg_ = 0, chr0_ = 0, chr1_ = 1;
};

// ---------------------------------------------------------------- 206: Namco 108 (DxROM, um MMC3 simples)
class Namco108 : public Mapper {
public:
  explicit Namco108(Cart& c) : Mapper(c) {}
  void reset() override { select_ = 0; r_[0] = 0; r_[1] = 2; r_[2] = 4; r_[3] = 5; r_[4] = 6; r_[5] = 7; r_[6] = 0; r_[7] = 1; update(); }
  void update() override {
    prg8(0, r_[6] & 0x0F); prg8(1, r_[7] & 0x0F); prg8(2, -2); prg8(3, -1);
    chr2(0, (r_[0] & 0x3F) >> 1); chr2(1, (r_[1] & 0x3F) >> 1);
    for (int k = 0; k < 4; k++) chr1(4 + k, r_[2 + k] & 0x3F);
    mirror(cart.fourScreen ? FOUR : (cart.vertical ? VERTICAL : HORIZONTAL));
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000 || a >= 0xA000) return;
    if (a & 1) r_[select_ & 7] = v; else select_ = v;
    update();
  }
  void syncRegs(StateIO& io) override { io.sync(select_); io.raw(r_, sizeof r_); }

private:
  uint8_t select_ = 0, r_[8] = {};
};

// ---------------------------------------------------------------- 21, 22, 23, 25: Konami VRC2 e VRC4
// as placas ligam os pinos de endereço do chip em linhas diferentes; sem submapeador, as variantes de cada
// número são aceitas juntas (como no Mesen)
class VRC24 : public Mapper {
public:
  VRC24(Cart& c, int id) : Mapper(c), id_(id) {}
  void reset() override {
    prg0_ = 0; prg1_ = 1; mirrorReg_ = 0; swap_ = 0;
    for (int k = 0; k < 8; k++) chr_[k] = static_cast<uint16_t>(k);
    latch_ = counter_ = 0; ctl_ = 0; prescaler_ = 341; irq = false;
    update();
  }
  void update() override {
    if (swap_ & 2) { prg8(0, -2); prg8(2, prg0_); }
    else { prg8(0, prg0_); prg8(2, -2); }
    prg8(1, prg1_);
    prg8(3, -1);
    for (int k = 0; k < 8; k++) chr1(k, id_ == 22 ? (chr_[k] >> 1) : chr_[k]);
    if (vrc2()) mirror((mirrorReg_ & 1) ? HORIZONTAL : VERTICAL);
    else { static const int M[4] = {VERTICAL, HORIZONTAL, SINGLE0, SINGLE1}; mirror(M[mirrorReg_ & 3]); }
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    const int r = translate(a);
    switch (r & 0xF000) {
      case 0x8000: prg0_ = v & 0x1F; break;
      case 0x9000:
        if (vrc2() || (r & 3) < 2) mirrorReg_ = v & 3;
        else if ((r & 3) == 2) swap_ = v;
        break;
      case 0xA000: prg1_ = v & 0x1F; break;
      case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
        const int idx = (((r >> 12) - 0xB) << 1) | ((r >> 1) & 1);
        if (r & 1) chr_[idx] = static_cast<uint16_t>((chr_[idx] & 0x0F) | ((v & 0x1F) << 4));
        else chr_[idx] = static_cast<uint16_t>((chr_[idx] & 0x1F0) | (v & 0x0F));
        break;
      }
      case 0xF000:
        if (vrc2()) break;
        switch (r & 3) {
          case 0: latch_ = (latch_ & 0xF0) | (v & 0x0F); break;
          case 1: latch_ = static_cast<uint8_t>((latch_ & 0x0F) | (v << 4)); break;
          case 2:
            ctl_ = v & 7;
            if (ctl_ & 2) { counter_ = latch_; prescaler_ = 341; }
            irq = false;
            break;
          case 3: ctl_ = (ctl_ & ~2) | ((ctl_ & 1) << 1); irq = false; break;
        }
        break;
    }
    update();
  }
  void cpuCycle() override {
    if (vrc2() || !(ctl_ & 2)) return;
    if (ctl_ & 4) { clock(); return; }  // modo de ciclos
    prescaler_ -= 3;                     // modo de linhas: 341/3 ciclos por linha
    if (prescaler_ <= 0) { prescaler_ += 341; clock(); }
  }
  void syncRegs(StateIO& io) override {
    io.sync(prg0_); io.sync(prg1_); io.sync(mirrorReg_); io.sync(swap_); io.raw(chr_, sizeof chr_);
    io.sync(latch_); io.sync(counter_); io.sync(ctl_); io.sync(prescaler_); io.sync(irq);
    if (io.reading()) clampTo(prescaler_, -3, 341);
  }

private:
  bool vrc2() const { return id_ == 22 || (id_ == 23 && cart.submapper == 3); }
  void clock() {
    if (counter_ == 0xFF) { counter_ = latch_; irq = true; }
    else counter_++;
  }
  int translate(uint16_t a) const {
    int a0, a1;
    switch (id_) {
      case 21: a0 = ((a >> 1) | (a >> 6)) & 1; a1 = ((a >> 2) | (a >> 7)) & 1; break;
      case 22: a0 = (a >> 1) & 1; a1 = a & 1; break;
      case 23: a0 = (a | (a >> 2)) & 1; a1 = ((a >> 1) | (a >> 3)) & 1; break;
      default: a0 = ((a >> 1) | (a >> 3)) & 1; a1 = (a | (a >> 2)) & 1; break;  // 25
    }
    return (a & 0xF000) | (a1 << 1) | a0;
  }
  int id_;
  uint8_t prg0_ = 0, prg1_ = 1, mirrorReg_ = 0, swap_ = 0, latch_ = 0, counter_ = 0, ctl_ = 0;
  uint16_t chr_[8] = {};
  int prescaler_ = 341;
};

}  // namespace

std::unique_ptr<Mapper> Mapper::create(Cart& c) {
  switch (c.mapper) {
    case 0: case 2: case 3: case 7: case 11: case 66: case 71: return std::make_unique<Simple>(c, c.mapper);
    case 1: return std::make_unique<MMC1>(c);
    case 4: return std::make_unique<MMC3>(c);
    case 9: return std::make_unique<MMC2>(c);
    case 10: return std::make_unique<MMC4>(c);
    case 34: case 79: case 140: return std::make_unique<Discrete>(c, c.mapper);
    case 206: return std::make_unique<Namco108>(c);
    case 21: case 22: case 23: case 25: return std::make_unique<VRC24>(c, c.mapper);
    default: return createExtMapper(c);  // 5, 19, 24, 26, 69 (FME-7 com o som do 5B), 85
  }
}

}  // namespace indigo::nes
