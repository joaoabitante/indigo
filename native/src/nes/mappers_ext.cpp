// Mapeadores com chips próprios de vídeo ou som:
//   5      Nintendo MMC5 (Castlevania III, Just Breed, Uncharted Waters…): PRG/CHR em vários modos, RAM de até
//          64 KB, ExRAM (tabela extra, atributos estendidos, tela dividida), modo de preenchimento, IRQ por
//          linha, multiplicador e o som (2 quadradas + PCM)
//   24/26  Konami VRC6 (Akumajou Densetsu, Madara, Esper Dream 2): 2 quadradas de 16 passos + dente de serra
//   85     Konami VRC7 (Lagrange Point, Tiny Toon Adventures 2): som FM de 6 canais (derivado do YM2413)
//   19     Namco 163 (Megami Tensei II, King of Kings, Final Lap…): até 8 canais de tabela de onda
//   69     Sunsoft FME-7 / 5A / 5B (Batman: Return of the Joker, Gimmick!): o 5B tem som de 3 canais (AY-3-8910)
// Os níveis de som em relação ao console seguem as medições publicadas na wiki do NESdev.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "nes.h"

namespace indigo::nes {
namespace {

constexpr uint8_t LENGTH[32] = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr uint8_t DUTY[4][8] = {{0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};

// mesma curva das quadradas do console (o MMC5 soma as suas no mesmo ponto do circuito)
inline float pulseMix(int n) { return n ? 95.88f / (8128.0f / static_cast<float>(n) + 100.0f) : 0.0f; }

// ================================================================ IRQ dos Konami (VRC4, VRC6, VRC7)
struct VrcIrq {
  uint8_t latch = 0, counter = 0, ctl = 0;
  int prescaler = 341;
  void reset() { latch = counter = ctl = 0; prescaler = 341; }
  void control(uint8_t v, bool& irq) {
    ctl = v & 7;
    if (ctl & 2) { counter = latch; prescaler = 341; }
    irq = false;
  }
  void ack(bool& irq) { ctl = static_cast<uint8_t>((ctl & ~2) | ((ctl & 1) << 1)); irq = false; }
  void clock(bool& irq) {
    if (!(ctl & 2)) return;
    if (ctl & 4) { step(irq); return; }  // modo de ciclos
    prescaler -= 3;                      // modo de linhas: 341/3 ciclos por linha
    if (prescaler <= 0) { prescaler += 341; step(irq); }
  }
  void step(bool& irq) {
    if (counter == 0xFF) { counter = latch; irq = true; }
    else counter++;
  }
  void sync(StateIO& io) {
    io.sync(latch); io.sync(counter); io.sync(ctl); io.sync(prescaler);
    if (io.reading()) { ctl &= 7; clampTo(prescaler, -3, 341); }
  }
};

// ================================================================ 5: MMC5
class MMC5 : public Mapper {
public:
  explicit MMC5(Cart& c) : Mapper(c) {
    customCpu = customNt = true;
    // sem o tamanho no cabeçalho, 64 KB atende todas as placas (EKROM 8 KB, ETROM 16 KB, EWROM 32 KB)
    size_t ram = c.ramSize ? c.ramSize : 0x10000;
    ram = std::clamp<size_t>((ram + 0x1FFF) & ~size_t(0x1FFF), 0x2000, 0x10000);
    c.prgRam.assign(ram, 0);
  }

  void reset() override {
    prgMode_ = 3; chrMode_ = 0; prot1_ = prot2_ = 0; exMode_ = 0; ntMap_ = 0; fillTile_ = fillAttr_ = 0;
    for (auto& r : prgReg_) r = 0;
    prgReg_[4] = 0xFF;
    for (auto& r : chrReg_) r = 0;
    chrUpper_ = 0; lastB_ = false;
    std::memset(exram_, 0, sizeof exram_);
    splitCtl_ = splitScroll_ = splitBank_ = 0;
    irqCompare_ = 0; line_ = 0; irqEnable_ = irqPending_ = inFrame_ = false;
    lastAddr_ = 0; repeat_ = 0; idle_ = 0;
    mulA_ = mulB_ = 0xFF;
    tall_ = render_ = false;
    kind_ = 0; exByte_ = 0; splitRow_ = 0; splitTile_ = 0;
    for (auto& p : pulse_) p = Pulse{};
    pcm_ = 0; pcmCtl_ = 0; pcmIrq_ = false; frameDiv_ = 0; odd_ = false; out_ = 0;
    irq = false;
    update();
  }

  void update() override {
    // PRG: $6000 é sempre RAM; $8000–$DFFF podem ser RAM (bit 7 do registrador = ROM); $E000 é sempre ROM
    setSlot(0, prgReg_[0] & 0x7F, false);
    const uint8_t r5 = prgReg_[2], r6 = prgReg_[3], r7 = prgReg_[4];
    switch (prgMode_ & 3) {
      case 0:
        for (int k = 0; k < 4; k++) setSlot(1 + k, ((r7 & 0x7C) | k), true);
        break;
      case 1:
        setSlot(1, (r5 & 0x7E), r5 & 0x80); setSlot(2, (r5 & 0x7E) | 1, r5 & 0x80);
        setSlot(3, (r7 & 0x7E), true); setSlot(4, (r7 & 0x7E) | 1, true);
        break;
      case 2:
        setSlot(1, (r5 & 0x7E), r5 & 0x80); setSlot(2, (r5 & 0x7E) | 1, r5 & 0x80);
        setSlot(3, r6 & 0x7F, r6 & 0x80); setSlot(4, r7 & 0x7F, true);
        break;
      default:
        setSlot(1, prgReg_[1] & 0x7F, prgReg_[1] & 0x80); setSlot(2, r5 & 0x7F, r5 & 0x80);
        setSlot(3, r6 & 0x7F, r6 & 0x80); setSlot(4, r7 & 0x7F, true);
        break;
    }
    // CHR: conjunto A ($5120–$5127, sprites) e B ($5128–$512B, fundo com sprites 8×16)
    const uint16_t* r = chrReg_;
    switch (chrMode_ & 3) {
      case 0: chr8(r[7]); break;
      case 1: chr4(0, r[3]); chr4(1, r[7]); break;
      case 2: chr2(0, r[1]); chr2(1, r[3]); chr2(2, r[5]); chr2(3, r[7]); break;
      default: for (int k = 0; k < 8; k++) chr1(k, r[k]); break;
    }
    std::memcpy(chrA_, chrOff, sizeof chrA_);
    switch (chrMode_ & 3) {
      case 0: chr8(r[11]); break;
      case 1: chr4(0, r[11]); chr4(1, r[11]); break;
      case 2: chr2(0, r[9]); chr2(1, r[11]); chr2(2, r[9]); chr2(3, r[11]); break;
      default: for (int k = 0; k < 8; k++) chr1(k, r[8 + (k & 3)]); break;
    }
    std::memcpy(chrB_, chrOff, sizeof chrB_);
    std::memcpy(chrOff, lastB_ ? chrB_ : chrA_, sizeof chrOff);
  }

  // ---------------------------------------------------------------- CPU
  uint8_t cpuRead(uint16_t a, uint8_t ob) override {
    if (a == 0xFFFA || a == 0xFFFB) { inFrame_ = false; lastAddr_ = 0; repeat_ = 0; }  // vetor do NMI: fim do quadro
    if (a >= 0x6000) {
      const uint8_t v = slotRead(a);
      // PCM em modo de leitura: cada leitura de $8000–$BFFF vai para o DAC ($00 pede IRQ)
      if ((pcmCtl_ & 1) && a >= 0x8000 && a < 0xC000) {
        if (v) pcm_ = v;
        else pcmIrq_ = true;
        updateIrq();
      }
      return v;
    }
    if (a >= 0x5C00) return exMode_ >= 2 ? exram_[a - 0x5C00] : ob;
    switch (a) {
      case 0x5010: {
        const uint8_t v = static_cast<uint8_t>((pcmIrq_ ? 0x80 : 0) | (pcmCtl_ & 1));
        pcmIrq_ = false;
        updateIrq();
        return v;
      }
      case 0x5015: return static_cast<uint8_t>((ob & 0xFC) | (pulse_[0].length ? 1 : 0) | (pulse_[1].length ? 2 : 0));
      case 0x5204: {
        const uint8_t v = static_cast<uint8_t>((ob & 0x3F) | (irqPending_ ? 0x80 : 0) | (inFrame_ ? 0x40 : 0));
        irqPending_ = false;
        updateIrq();
        return v;
      }
      case 0x5205: return static_cast<uint8_t>((mulA_ * mulB_) & 0xFF);
      case 0x5206: return static_cast<uint8_t>((mulA_ * mulB_) >> 8);
      default: return ob;
    }
  }
  uint8_t cpuPeek(uint16_t a) override { return a >= 0x6000 ? slotRead(a) : 0; }

  void writeReg(uint16_t a, uint8_t v) override {
    if (a >= 0x6000) {
      const int s = (a >> 13) - 3;
      if (!rom_[s] && (prot1_ & 3) == 2 && (prot2_ & 3) == 1) {
        uint8_t& m = cart.prgRam[map_[s] + (a & 0x1FFF)];
        if (m != v) { m = v; if (cart.battery) nes->dirty = true; }
      }
      return;
    }
    if (a >= 0x5C00) {
      // modos 0 e 1: só grava durante o desenho da tela (fora dele grava 0); modo 3: só leitura
      if (exMode_ <= 1) exram_[a - 0x5C00] = inFrame_ ? v : 0;
      else if (exMode_ == 2) exram_[a - 0x5C00] = v;
      return;
    }
    if (a >= 0x5000 && a <= 0x5007) { pulseWrite((a >> 2) & 1, a & 3, v); return; }
    switch (a) {
      case 0x5010: pcmCtl_ = v; updateIrq(); return;
      case 0x5011: if (!(pcmCtl_ & 1) && v) pcm_ = v; return;
      case 0x5015:
        for (int k = 0; k < 2; k++) { pulse_[k].enabled = (v >> k) & 1; if (!pulse_[k].enabled) pulse_[k].length = 0; }
        return;
      case 0x5100: prgMode_ = v & 3; break;
      case 0x5101: chrMode_ = v & 3; break;
      case 0x5102: prot1_ = v & 3; return;
      case 0x5103: prot2_ = v & 3; return;
      case 0x5104: exMode_ = v & 3; return;
      case 0x5105: ntMap_ = v; return;
      case 0x5106: fillTile_ = v; return;
      case 0x5107: fillAttr_ = v & 3; return;
      case 0x5130: chrUpper_ = v & 3; return;
      case 0x5200: splitCtl_ = v; return;
      case 0x5201: splitScroll_ = v; return;
      case 0x5202: splitBank_ = v; return;
      case 0x5203: irqCompare_ = v; return;
      case 0x5204: irqEnable_ = v & 0x80; updateIrq(); return;
      case 0x5205: mulA_ = v; return;
      case 0x5206: mulB_ = v; return;
      default:
        if (a >= 0x5113 && a <= 0x5117) { prgReg_[a - 0x5113] = v; break; }
        if (a >= 0x5120 && a <= 0x512B) {
          chrReg_[a - 0x5120] = static_cast<uint16_t>(v | (chrUpper_ << 8));
          lastB_ = a >= 0x5128;
          break;
        }
        return;
    }
    update();
  }

  void ppuRegWrite(int r, uint8_t v) override {
    if (r == 0) tall_ = v & 0x20;
    else if (r == 1) render_ = v & 0x18;
  }

  // ---------------------------------------------------------------- PPU
  // detecção de linha: o PPU lê o mesmo endereço de tabela de nomes 3 vezes seguidas no fim de cada linha
  void ppuAddress(uint16_t a) override {
    idle_ = 3;
    if (a >= 0x2000 && a <= 0x2FFF && a == lastAddr_) {
      if (++repeat_ == 2) scanlineDetected();
    } else {
      repeat_ = 0;
    }
    lastAddr_ = a;
  }

  uint8_t ntRead(uint16_t a) override {
    const int off = a & 0x3FF;
    const bool attr = off >= 0x3C0;
    if (bgFetch()) {
      if (!attr) {
        kind_ = 0;
        const int dot = nes->ppu.dot;
        const int tile = dot >= 321 ? (dot - 321) / 8 : (dot - 1) / 8 + 2;
        if ((splitCtl_ & 0x80) && exMode_ <= 1) {
          const int thr = splitCtl_ & 0x1F;
          const bool in = (splitCtl_ & 0x40) ? tile >= thr : tile < thr;
          if (in) {
            const int sl = nes->ppu.scanline;
            const int line = sl == 261 ? 0 : (dot >= 321 ? sl + 1 : sl);
            splitRow_ = static_cast<uint8_t>((splitScroll_ + line) % 240);
            splitTile_ = static_cast<uint8_t>(tile & 31);
            kind_ = 2;
            return exram_[(splitRow_ / 8) * 32 + splitTile_];
          }
        }
        if (exMode_ == 1) { exByte_ = exram_[off]; kind_ = 1; }
      } else if (kind_ == 2) {
        const uint8_t at = exram_[0x3C0 + (splitRow_ / 32) * 8 + splitTile_ / 4];
        const int sh = ((splitRow_ >> 4) & 1) * 4 + ((splitTile_ >> 1) & 1) * 2;
        return static_cast<uint8_t>(((at >> sh) & 3) * 0x55);
      } else if (kind_ == 1) {
        return static_cast<uint8_t>((exByte_ >> 6) * 0x55);
      }
    }
    switch ((ntMap_ >> (((a >> 10) & 3) * 2)) & 3) {
      case 0: return ciram[off];
      case 1: return ciram[0x400 | off];
      case 2: return exMode_ <= 1 ? exram_[off] : 0;
      default: return attr ? static_cast<uint8_t>(fillAttr_ * 0x55) : fillTile_;
    }
  }
  void ntWrite(uint16_t a, uint8_t v) override {
    const int off = a & 0x3FF;
    switch ((ntMap_ >> (((a >> 10) & 3) * 2)) & 3) {
      case 0: ciram[off] = v; break;
      case 1: ciram[0x400 | off] = v; break;
      case 2: if (exMode_ <= 1) exram_[off] = v; break;
      default: break;
    }
  }

  uint8_t chrRead(uint16_t a) override {
    const size_t size = cart.chr.size();
    const bool rendering = renderingNow();
    if (rendering && bgFetch()) {
      if (kind_ == 2) return cart.chr[(splitBank_ * size_t(0x1000) + (a & 0xFF8) + (splitRow_ & 7)) % size];
      if (kind_ == 1) {
        const size_t bank = (exByte_ & 0x3F) | (chrUpper_ << 6);
        return cart.chr[(bank * 0x1000 + (a & 0xFFF)) % size];
      }
    }
    const uint32_t* set;
    if (tall_ && rendering) set = spriteFetch() ? chrA_ : chrB_;
    else set = lastB_ ? chrB_ : chrA_;
    return cart.chr[set[a >> 10] + (a & 0x3FF)];
  }

  // ---------------------------------------------------------------- relógio e som
  void cpuCycle() override {
    if (idle_ > 0 && --idle_ == 0) inFrame_ = false;  // o PPU parou de ler: fora do quadro
    odd_ = !odd_;
    if (odd_) {
      for (auto& p : pulse_) {
        if (p.timer == 0) { p.timer = p.period; p.seq = (p.seq - 1) & 7; }
        else p.timer--;
      }
    }
    // envelope e tamanho a 240 Hz fixos
    if (++frameDiv_ >= 7457) {
      frameDiv_ = 0;
      for (auto& p : pulse_) {
        if (p.envStart) { p.envStart = false; p.decay = 15; p.envDiv = p.ctl & 15; }
        else if (p.envDiv == 0) { p.envDiv = p.ctl & 15; if (p.decay) p.decay--; else if (p.ctl & 0x20) p.decay = 15; }
        else p.envDiv--;
        if (p.length && !(p.ctl & 0x20)) p.length--;
      }
    }
    int n = 0;
    for (auto& p : pulse_) {
      if (p.length && DUTY[p.ctl >> 6][p.seq]) n += (p.ctl & 0x10) ? (p.ctl & 15) : p.decay;
    }
    out_ = pulseMix(n) + pcm_ * 0.0016f;
  }
  float audio() override { return out_; }

  void syncRegs(StateIO& io) override {
    io.sync(prgMode_); io.sync(chrMode_); io.sync(prot1_); io.sync(prot2_); io.sync(exMode_); io.sync(ntMap_);
    io.sync(fillTile_); io.sync(fillAttr_); io.raw(prgReg_, sizeof prgReg_); io.raw(chrReg_, sizeof chrReg_);
    io.sync(chrUpper_); io.sync(lastB_); io.raw(exram_, sizeof exram_);
    io.sync(splitCtl_); io.sync(splitScroll_); io.sync(splitBank_);
    io.sync(irqCompare_); io.sync(line_); io.sync(irqEnable_); io.sync(irqPending_); io.sync(inFrame_);
    io.sync(lastAddr_); io.sync(repeat_); io.sync(idle_); io.sync(mulA_); io.sync(mulB_); io.sync(tall_); io.sync(render_);
    io.sync(kind_); io.sync(exByte_); io.sync(splitRow_); io.sync(splitTile_);
    for (auto& p : pulse_) {
      io.sync(p.ctl); io.sync(p.seq); io.sync(p.length); io.sync(p.period); io.sync(p.timer);
      io.sync(p.enabled); io.sync(p.envStart); io.sync(p.envDiv); io.sync(p.decay);
    }
    io.sync(pcm_); io.sync(pcmCtl_); io.sync(pcmIrq_); io.sync(frameDiv_); io.sync(odd_); io.sync(irq);
    if (io.reading()) {
      prgMode_ &= 3; chrMode_ &= 3; exMode_ &= 3; fillAttr_ &= 3; chrUpper_ &= 3;
      clampTo(repeat_, 0, 2); clampTo(idle_, 0, 3); clampTo<uint8_t>(kind_, 0, 2);
      splitRow_ = static_cast<uint8_t>(splitRow_ % 240); splitTile_ &= 31;
      for (auto& p : pulse_) { p.seq &= 7; p.period &= 0x7FF; clampTo(p.timer, 0, 0x7FF); p.envDiv &= 15; p.decay &= 15; }
      clampTo(frameDiv_, 0, 7457);
    }
  }

private:
  struct Pulse {
    uint8_t ctl = 0, seq = 0, length = 0, envDiv = 0, decay = 0;
    uint16_t period = 0;
    int timer = 0;
    bool enabled = false, envStart = false;
  };

  void pulseWrite(int k, int r, uint8_t v) {
    Pulse& p = pulse_[k];
    switch (r) {
      case 0: p.ctl = v; break;
      case 2: p.period = static_cast<uint16_t>((p.period & 0x700) | v); break;
      case 3:
        p.period = static_cast<uint16_t>((p.period & 0xFF) | ((v & 7) << 8));
        if (p.enabled) p.length = LENGTH[v >> 3];
        p.seq = 0;
        p.envStart = true;
        break;
      default: break;
    }
  }
  uint32_t ramOff(int bank) const {
    const size_t n = cart.prgRam.size() / 0x2000;
    if (n == 1) bank = 0;
    else if (n == 2) bank = (bank >> 2) & 1;  // duas memórias de 8 KB: o bit 2 escolhe
    else bank &= static_cast<int>(n >= 8 ? 7 : 3);
    return static_cast<uint32_t>((static_cast<size_t>(bank) % n) * 0x2000);
  }
  void setSlot(int s, int bank, bool rom) {
    rom_[s] = rom;
    map_[s] = rom ? static_cast<uint32_t>(prgBank8(bank)) : ramOff(bank & 7);
  }
  uint8_t slotRead(uint16_t a) const {
    const int s = (a >> 13) - 3;
    const uint32_t off = map_[s] + (a & 0x1FFF);
    return rom_[s] ? cart.prg[off] : cart.prgRam[off];
  }
  bool renderingNow() const {
    const int sl = nes->ppu.scanline;
    return render_ && (sl < 240 || sl == 261);
  }
  bool spriteFetch() const { const int d = nes->ppu.dot; return d >= 257 && d <= 320; }
  bool bgFetch() const {
    if (!renderingNow()) return false;
    const int d = nes->ppu.dot;
    return (d >= 1 && d <= 256) || (d >= 321 && d <= 336);
  }
  void scanlineDetected() {
    if (!inFrame_) {
      inFrame_ = true;
      line_ = 0;
      irqPending_ = false;
    } else {
      line_++;
      if (line_ == irqCompare_) irqPending_ = true;
    }
    updateIrq();
  }
  void updateIrq() { irq = (irqPending_ && irqEnable_) || (pcmIrq_ && (pcmCtl_ & 0x80)); }

  uint8_t prgMode_ = 3, chrMode_ = 0, prot1_ = 0, prot2_ = 0, exMode_ = 0, ntMap_ = 0, fillTile_ = 0, fillAttr_ = 0;
  uint8_t prgReg_[5] = {0, 0, 0, 0, 0xFF};
  uint16_t chrReg_[12] = {};
  uint8_t chrUpper_ = 0;
  bool lastB_ = false;
  uint8_t exram_[0x400] = {};
  uint8_t splitCtl_ = 0, splitScroll_ = 0, splitBank_ = 0;
  uint8_t irqCompare_ = 0, line_ = 0;
  bool irqEnable_ = false, irqPending_ = false, inFrame_ = false;
  uint16_t lastAddr_ = 0;
  int repeat_ = 0, idle_ = 0;
  uint8_t mulA_ = 0xFF, mulB_ = 0xFF;
  bool tall_ = false, render_ = false;
  uint8_t kind_ = 0, exByte_ = 0, splitRow_ = 0, splitTile_ = 0;  // busca do fundo em andamento: 0 normal, 1 atributo estendido, 2 tela dividida
  uint32_t map_[5] = {};
  bool rom_[5] = {};
  uint32_t chrA_[8] = {}, chrB_[8] = {};
  Pulse pulse_[2];
  uint8_t pcm_ = 0, pcmCtl_ = 0;
  bool pcmIrq_ = false, odd_ = false;
  int frameDiv_ = 0;
  float out_ = 0;
};

// ================================================================ 24, 26: Konami VRC6
class VRC6 : public Mapper {
public:
  VRC6(Cart& c, int id) : Mapper(c), id_(id) {}
  void reset() override {
    prg16_ = 0; prg8_ = 0; ctrl_ = 0; freqCtl_ = 0;
    for (int k = 0; k < 8; k++) chr_[k] = static_cast<uint8_t>(k);
    irqc_.reset(); irq = false;
    for (auto& p : pulse_) p = Pulse{};
    saw_ = Saw{};
    out_ = 0;
    update();
  }
  void update() override {
    prg16(0, prg16_); prg8(2, prg8_); prg8(3, -1);
    for (int k = 0; k < 8; k++) chr1(k, chr_[k]);
    static const int M[4] = {VERTICAL, HORIZONTAL, SINGLE0, SINGLE1};
    mirror(M[(ctrl_ >> 2) & 3]);
    ramEnabled = ramWritable = (ctrl_ & 0x80) != 0;
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    if (id_ == 26) a = static_cast<uint16_t>((a & ~3) | ((a & 1) << 1) | ((a >> 1) & 1));  // A0 e A1 trocados
    switch (a & 0xF003) {
      case 0x8000: case 0x8001: case 0x8002: case 0x8003: prg16_ = v & 0x0F; break;
      case 0x9000: case 0xA000: { Pulse& p = pulse_[(a >> 12) - 9]; p.vol = v & 15; p.duty = (v >> 4) & 7; p.mode = v & 0x80; return; }
      case 0x9001: case 0xA001: { Pulse& p = pulse_[(a >> 12) - 9]; p.period = static_cast<uint16_t>((p.period & 0xF00) | v); return; }
      case 0x9002: case 0xA002: {
        Pulse& p = pulse_[(a >> 12) - 9];
        p.period = static_cast<uint16_t>((p.period & 0xFF) | ((v & 15) << 8));
        p.enabled = v & 0x80;
        if (!p.enabled) p.step = 15;
        return;
      }
      case 0x9003: freqCtl_ = v & 7; return;
      case 0xB000: saw_.rate = v & 0x3F; return;
      case 0xB001: saw_.period = static_cast<uint16_t>((saw_.period & 0xF00) | v); return;
      case 0xB002:
        saw_.period = static_cast<uint16_t>((saw_.period & 0xFF) | ((v & 15) << 8));
        saw_.enabled = v & 0x80;
        if (!saw_.enabled) { saw_.acc = 0; saw_.step = 0; }
        return;
      case 0xB003: ctrl_ = v; break;
      case 0xC000: case 0xC001: case 0xC002: case 0xC003: prg8_ = v & 0x1F; break;
      case 0xD000: case 0xD001: case 0xD002: case 0xD003: chr_[a & 3] = v; break;
      case 0xE000: case 0xE001: case 0xE002: case 0xE003: chr_[4 + (a & 3)] = v; break;
      case 0xF000: irqc_.latch = v; return;
      case 0xF001: irqc_.control(v, irq); return;
      case 0xF002: irqc_.ack(irq); return;
      default: return;
    }
    update();
  }
  void cpuCycle() override {
    irqc_.clock(irq);
    if (!(freqCtl_ & 1)) {
      const int shift = (freqCtl_ & 4) ? 8 : (freqCtl_ & 2) ? 4 : 0;
      for (auto& p : pulse_) {
        if (!p.enabled) continue;
        if (p.timer <= 0) { p.timer = p.period >> shift; p.step = (p.step - 1) & 15; }
        else p.timer--;
      }
      if (saw_.enabled) {
        if (saw_.timer <= 0) {
          saw_.timer = saw_.period >> shift;
          if (++saw_.step >= 14) { saw_.step = 0; saw_.acc = 0; }
          else if (!(saw_.step & 1)) saw_.acc = static_cast<uint8_t>(saw_.acc + saw_.rate);
        } else {
          saw_.timer--;
        }
      }
    }
    int n = 0;
    for (auto& p : pulse_) if (p.enabled && (p.mode || p.step <= p.duty)) n += p.vol;
    if (saw_.enabled) n += saw_.acc >> 3;
    out_ = n * 0.0098f;
  }
  float audio() override { return out_; }
  void syncRegs(StateIO& io) override {
    io.sync(prg16_); io.sync(prg8_); io.raw(chr_, sizeof chr_); io.sync(ctrl_); io.sync(freqCtl_);
    irqc_.sync(io); io.sync(irq);
    for (auto& p : pulse_) { io.sync(p.vol); io.sync(p.duty); io.sync(p.mode); io.sync(p.enabled); io.sync(p.period); io.sync(p.timer); io.sync(p.step); }
    io.sync(saw_.rate); io.sync(saw_.acc); io.sync(saw_.step); io.sync(saw_.period); io.sync(saw_.timer); io.sync(saw_.enabled);
    if (io.reading()) {
      for (auto& p : pulse_) { p.vol &= 15; p.duty &= 7; p.period &= 0xFFF; p.step &= 15; clampTo(p.timer, 0, 0xFFF); }
      saw_.rate &= 0x3F; saw_.period &= 0xFFF; clampTo<uint8_t>(saw_.step, 0, 13); clampTo(saw_.timer, 0, 0xFFF);
    }
  }

private:
  struct Pulse { uint8_t vol = 0, duty = 0, step = 15; bool mode = false, enabled = false; uint16_t period = 0; int timer = 0; };
  struct Saw { uint8_t rate = 0, acc = 0, step = 0; uint16_t period = 0; int timer = 0; bool enabled = false; };
  int id_;
  uint8_t prg16_ = 0, prg8_ = 0, chr_[8] = {}, ctrl_ = 0, freqCtl_ = 0;
  VrcIrq irqc_;
  Pulse pulse_[2];
  Saw saw_;
  float out_ = 0;
};

// ================================================================ som FM do VRC7 (6 canais, 2 operadores cada)
// Uma amostra a cada 36 ciclos de CPU (49.716 Hz). Seno e exponencial em tabelas logarítmicas como no chip;
// envelope em passos de 0,1875 dB com as mesmas taxas da família OPL.
class Opll {
public:
  Opll() { initTables(); reset(); }

  void reset() {
    std::memset(custom_, 0, sizeof custom_);
    for (auto& c : ch_) c = Channel{};
    egCounter_ = 0; amPhase_ = 0; pmPhase_ = 0;
  }

  void write(uint8_t r, uint8_t v) {
    r &= 0x3F;
    if (r < 8) { custom_[r] = v; return; }
    const int n = r & 0x0F;
    if (n >= 6) return;
    Channel& c = ch_[n];
    switch (r & 0xF0) {
      case 0x10: c.fnum = static_cast<uint16_t>((c.fnum & 0x100) | v); break;
      case 0x20: {
        c.fnum = static_cast<uint16_t>((c.fnum & 0xFF) | ((v & 1) << 8));
        c.block = (v >> 1) & 7;
        c.sus = v & 0x20;
        const bool key = v & 0x10;
        if (key && !c.key) { keyOn(c.op[0]); keyOn(c.op[1]); }
        else if (!key && c.key) { c.op[0].state = c.op[1].state = RELEASE; }
        c.key = key;
        break;
      }
      case 0x30: c.inst = v >> 4; c.vol = v & 15; break;
      default: break;
    }
  }

  int sample() {
    egCounter_++;
    amPhase_ = (amPhase_ + 1) % 13436;  // 3,7 Hz
    pmPhase_ = (pmPhase_ + 1) & 0x1FFF;  // 6,1 Hz
    const int am = amPhase_ < 6718 ? amPhase_ * 26 / 6718 : (13436 - amPhase_) * 26 / 6718;
    static const int PM[8] = {0, 1, 2, 1, 0, -1, -2, -1};
    const int pm = PM[pmPhase_ >> 10];
    int sum = 0;
    for (auto& c : ch_) {
      const uint8_t* p = c.inst ? ROM[c.inst] : custom_;
      int out[2] = {0, 0};
      for (int k = 0; k < 2; k++) {
        Op& o = c.op[k];
        const uint8_t flags = p[k];
        // fase
        int fnum = c.fnum;
        if (flags & 0x40) fnum += (((fnum >> 6) & 7) * pm) / 2;  // vibrato
        o.phase = (o.phase + ((static_cast<uint32_t>(fnum * MUL2[flags & 15]) << c.block) >> 1)) & 0x7FFFF;
        // envelope
        const int ksr = ((c.block << 1) | (c.fnum >> 8)) >> ((flags & 0x10) ? 0 : 2);
        envelope(o, c, p, k, ksr);
        // atenuação total em passos de 0,1875 dB
        // escala por tecla: 0, 1,5, 3 ou 6 dB por oitava (a tabela guarda a metade do valor de 3 dB/oitava)
        const int kslBits = (k ? p[3] : p[2]) >> 6;
        int att = o.env + (kslBits ? (ksl_[c.block][c.fnum >> 5] << 2) >> (3 - kslBits) : 0);
        att += k ? c.vol * 16 : (p[2] & 0x3F) * 4;
        if (flags & 0x80) att += am;
        const bool half = (p[3] >> (k ? 4 : 3)) & 1;
        int mod;
        if (k == 0) {
          const int fb = p[3] & 7;
          mod = fb ? (c.fb[0] + c.fb[1]) >> (9 - fb) : 0;
        } else {
          mod = out[0];
        }
        out[k] = opOut(o.phase, mod, att, half);
        if (k == 0) { c.fb[1] = c.fb[0]; c.fb[0] = out[0]; }
      }
      sum += out[1];
    }
    return sum;
  }

  void sync(StateIO& io) {
    io.raw(custom_, sizeof custom_);
    for (auto& c : ch_) {
      io.sync(c.fnum); io.sync(c.block); io.sync(c.key); io.sync(c.sus); io.sync(c.inst); io.sync(c.vol);
      io.raw(c.fb, sizeof c.fb);
      for (auto& o : c.op) { io.sync(o.phase); io.sync(o.env); io.sync(o.state); }
    }
    io.sync(egCounter_); io.sync(amPhase_); io.sync(pmPhase_);
    if (io.reading()) {
      for (auto& c : ch_) {
        c.fnum &= 0x1FF; c.block &= 7; c.inst &= 15; c.vol &= 15;
        for (auto& f : c.fb) clampTo(f, -4096, 4096);
        for (auto& o : c.op) { o.phase &= 0x7FFFF; clampTo(o.env, 0, 255); clampTo(o.state, 0, 4); }
      }
      clampTo(amPhase_, 0, 13435); pmPhase_ &= 0x1FFF;
    }
  }

private:
  enum { ATTACK, DECAY, SUSTAIN, RELEASE, OFF };
  struct Op { uint32_t phase = 0; int env = 255; int state = OFF; };
  struct Channel {
    uint16_t fnum = 0;
    uint8_t block = 0, inst = 0, vol = 0;
    bool key = false, sus = false;
    int fb[2] = {0, 0};
    Op op[2];
  };

  // instrumentos de fábrica do VRC7 (lidos do chip): 8 bytes como os registradores $00–$07
  static constexpr uint8_t ROM[16][8] = {
      {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, {0x03, 0x21, 0x05, 0x06, 0xE8, 0x81, 0x42, 0x27},
      {0x13, 0x41, 0x14, 0x0D, 0xD8, 0xF6, 0x23, 0x12}, {0x11, 0x11, 0x08, 0x08, 0xFA, 0xB2, 0x20, 0x12},
      {0x31, 0x61, 0x0C, 0x07, 0xA8, 0x64, 0x61, 0x27}, {0x32, 0x21, 0x1E, 0x06, 0xE1, 0x76, 0x01, 0x28},
      {0x02, 0x01, 0x06, 0x00, 0xA3, 0xE2, 0xF4, 0xF4}, {0x21, 0x61, 0x1D, 0x07, 0x82, 0x81, 0x11, 0x07},
      {0x23, 0x21, 0x22, 0x17, 0xA2, 0x72, 0x01, 0x17}, {0x35, 0x11, 0x25, 0x00, 0x40, 0x73, 0x72, 0x01},
      {0xB5, 0x01, 0x0F, 0x0F, 0xA8, 0xA5, 0x51, 0x02}, {0x17, 0xC1, 0x24, 0x07, 0xF8, 0xF8, 0x22, 0x12},
      {0x71, 0x23, 0x11, 0x06, 0x65, 0x74, 0x18, 0x16}, {0x01, 0x02, 0xD3, 0x05, 0xC9, 0x95, 0x03, 0x02},
      {0x61, 0x63, 0x0C, 0x00, 0x94, 0xC0, 0x33, 0xF6}, {0x21, 0x72, 0x0D, 0x00, 0xC1, 0xD5, 0x56, 0x06}};
  static constexpr uint8_t MUL2[16] = {1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
  static constexpr uint8_t EG_INC[15][8] = {
      {0, 1, 0, 1, 0, 1, 0, 1}, {0, 1, 0, 1, 1, 1, 0, 1}, {0, 1, 1, 1, 0, 1, 1, 1}, {0, 1, 1, 1, 1, 1, 1, 1},
      {1, 1, 1, 1, 1, 1, 1, 1}, {1, 1, 1, 2, 1, 1, 1, 2}, {1, 2, 1, 2, 1, 2, 1, 2}, {1, 2, 2, 2, 1, 2, 2, 2},
      {2, 2, 2, 2, 2, 2, 2, 2}, {2, 2, 2, 4, 2, 2, 2, 4}, {2, 4, 2, 4, 2, 4, 2, 4}, {2, 4, 4, 4, 2, 4, 4, 4},
      {4, 4, 4, 4, 4, 4, 4, 4}, {8, 8, 8, 8, 8, 8, 8, 8}, {0, 0, 0, 0, 0, 0, 0, 0}};

  static void initTables() {
    static bool done = false;
    if (done) return;
    for (int i = 0; i < 256; i++) {
      logsin_[i] = static_cast<uint16_t>(std::lround(-std::log2(std::sin((i + 0.5) * 3.14159265358979323846 / 512)) * 256));
      exp_[i] = static_cast<uint16_t>(std::lround(4095.0 * std::exp2(-i / 256.0)));
    }
    static const double KSL[16] = {0, 9, 12, 13.875, 15, 16.125, 16.875, 17.625, 18, 18.75, 19.125, 19.5, 19.875, 20.25, 20.625, 21};
    for (int b = 0; b < 8; b++)
      for (int i = 0; i < 16; i++) ksl_[b][i] = static_cast<uint8_t>(std::lround(std::max(0.0, KSL[i] - 3.0 * (7 - b)) / 0.1875 / 2));
    done = true;
  }

  // saída de um operador: seno (ou meio seno) pela tabela logarítmica, atenuado
  static int opOut(uint32_t phase, int mod, int att, bool half) {
    const int idx = static_cast<int>(((phase >> 9) + static_cast<uint32_t>(mod)) & 1023);
    if (half && (idx & 512)) return 0;
    int q = idx & 255;
    if (idx & 256) q = 255 - q;
    const int total = logsin_[q] + (att << 3);
    if (total >= 13 * 256) return 0;
    const int v = exp_[total & 255] >> (total >> 8);
    return (idx & 512) ? -v : v;
  }

  void keyOn(Op& o) { o.state = ATTACK; o.phase = 0; }

  void envelope(Op& o, const Channel& c, const uint8_t* p, int k, int ksr) {
    const uint8_t ar = p[4 + k] >> 4, dr = p[4 + k] & 15, sl = p[6 + k] >> 4, rr = p[6 + k] & 15;
    const bool sustained = p[k] & 0x20;
    int r = 0;
    switch (o.state) {
      case ATTACK: r = ar; break;
      case DECAY: r = dr; break;
      case SUSTAIN: r = sustained ? 0 : rr; break;
      case RELEASE: r = c.sus ? 5 : (sustained ? rr : 7); break;
      default: return;
    }
    const int rate = r ? std::min(63, r * 4 + ksr) : 0;
    if (o.state == ATTACK && rate >= 60) { o.env = 0; o.state = DECAY; return; }
    int inc = 0;
    if (rate >= 4) {
      const int g = rate >> 2;
      int shift, row;
      if (g <= 12) { shift = 13 - g; row = rate & 3; }
      else { shift = 0; row = g == 13 ? 4 + (rate & 3) : g == 14 ? 8 + (rate & 3) : 12; }
      if ((egCounter_ & ((1u << shift) - 1)) == 0) inc = EG_INC[row][(egCounter_ >> shift) & 7];
    }
    switch (o.state) {
      case ATTACK:
        if (inc) o.env += (-(o.env + 1) * inc) >> 3;
        if (o.env <= 0) { o.env = 0; o.state = DECAY; }
        break;
      case DECAY:
        o.env += inc;
        if (o.env >= sl * 16) o.state = SUSTAIN;
        break;
      default:
        o.env += inc;
        if (o.env >= 255) { o.env = 255; if (o.state == RELEASE) o.state = OFF; }
        break;
    }
  }

  static inline uint16_t logsin_[256], exp_[256];
  static inline uint8_t ksl_[8][16];
  uint8_t custom_[8] = {};
  Channel ch_[6];
  uint32_t egCounter_ = 0;
  int amPhase_ = 0, pmPhase_ = 0;
};

// ================================================================ 85: Konami VRC7
class VRC7 : public Mapper {
public:
  explicit VRC7(Cart& c) : Mapper(c) {}
  void reset() override {
    for (int k = 0; k < 3; k++) prg_[k] = static_cast<uint8_t>(k);
    for (int k = 0; k < 8; k++) chr_[k] = static_cast<uint8_t>(k);
    ctrl_ = 0; addr_ = 0; div_ = 0; out_ = 0;
    irqc_.reset(); irq = false;
    opll_.reset();
    update();
  }
  void update() override {
    prg8(0, prg_[0]); prg8(1, prg_[1]); prg8(2, prg_[2]); prg8(3, -1);
    for (int k = 0; k < 8; k++) chr1(k, chr_[k]);
    static const int M[4] = {VERTICAL, HORIZONTAL, SINGLE0, SINGLE1};
    mirror(M[ctrl_ & 3]);
    ramEnabled = ramWritable = (ctrl_ & 0x80) != 0;
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    // VRC7a usa o A4 e VRC7b o A3 para o segundo registrador de cada grupo
    const bool second = a & 0x18;
    switch (a & 0xF000) {
      case 0x8000: prg_[second ? 1 : 0] = v & 0x3F; break;
      case 0x9000:
        if (a & 0x20) { opll_.write(addr_, v); return; }
        if (second) { addr_ = v; return; }
        prg_[2] = v & 0x3F;
        break;
      case 0xA000: case 0xB000: case 0xC000: case 0xD000:
        chr_[(((a >> 12) - 0xA) << 1) | (second ? 1 : 0)] = v;
        break;
      case 0xE000:
        if (second) { irqc_.latch = v; return; }
        if ((v & 0x40) && !(ctrl_ & 0x40)) opll_.reset();
        ctrl_ = v;
        break;
      case 0xF000:
        if (second) irqc_.ack(irq); else irqc_.control(v, irq);
        return;
      default: return;
    }
    update();
  }
  void cpuCycle() override {
    irqc_.clock(irq);
    if (++div_ >= 36) {
      div_ = 0;
      out_ = (ctrl_ & 0x40) ? 0.0f : opll_.sample() * 6.0e-5f;
    }
  }
  float audio() override { return out_; }
  void syncRegs(StateIO& io) override {
    io.raw(prg_, sizeof prg_); io.raw(chr_, sizeof chr_); io.sync(ctrl_); io.sync(addr_); io.sync(div_);
    irqc_.sync(io); io.sync(irq);
    opll_.sync(io);
    if (io.reading()) clampTo(div_, 0, 35);
  }

private:
  uint8_t prg_[3] = {}, chr_[8] = {}, ctrl_ = 0, addr_ = 0;
  int div_ = 0;
  float out_ = 0;
  VrcIrq irqc_;
  Opll opll_;
};

// ================================================================ 19: Namco 163
// a RAM interna de som (128 bytes) fica depois dos 8 KB de RAM do cartucho, para ir junto no save
class Namco163 : public Mapper {
public:
  explicit Namco163(Cart& c) : Mapper(c) {
    customCpu = customNt = true;
    c.prgRam.assign(0x2000 + 0x80, 0);
  }
  void reset() override {
    for (int k = 0; k < 8; k++) chr_[k] = static_cast<uint8_t>(k);
    for (int k = 0; k < 4; k++) nt_[k] = static_cast<uint8_t>(0xE0 | (k & 1));
    prg_[0] = 0; prg_[1] = 1; prg_[2] = 2;
    sndOff_ = false; chrRamOff_ = 0; port_ = 0; counter_ = 0; irqOn_ = false; irq = false;
    div_ = 0; cur_ = 7;
    for (auto& o : out_) o = 0;
    update();
  }
  void update() override {
    if (cart.prgRam.size() < 0x2080) cart.prgRam.resize(0x2080, 0);
    prg8(0, prg_[0]); prg8(1, prg_[1]); prg8(2, prg_[2]); prg8(3, -1);
  }
  uint8_t chrRead(uint16_t a) override {
    const uint8_t b = chr_[a >> 10];
    if (b >= 0xE0 && !(chrRamOff_ & (a < 0x1000 ? 0x40 : 0x80))) return ciram[((b & 1) << 10) | (a & 0x3FF)];
    return cart.chr[(b * size_t(0x400) + (a & 0x3FF)) % cart.chr.size()];
  }
  void chrWrite(uint16_t a, uint8_t v) override {
    const uint8_t b = chr_[a >> 10];
    if (b >= 0xE0 && !(chrRamOff_ & (a < 0x1000 ? 0x40 : 0x80))) ciram[((b & 1) << 10) | (a & 0x3FF)] = v;
    else if (cart.chrRam) cart.chr[(b * size_t(0x400) + (a & 0x3FF)) % cart.chr.size()] = v;
  }
  uint8_t ntRead(uint16_t a) override {
    const uint8_t b = nt_[(a >> 10) & 3];
    if (b >= 0xE0) return ciram[((b & 1) << 10) | (a & 0x3FF)];
    return cart.chr[(b * size_t(0x400) + (a & 0x3FF)) % cart.chr.size()];
  }
  void ntWrite(uint16_t a, uint8_t v) override {
    const uint8_t b = nt_[(a >> 10) & 3];
    if (b >= 0xE0) ciram[((b & 1) << 10) | (a & 0x3FF)] = v;
  }
  uint8_t cpuRead(uint16_t a, uint8_t ob) override {
    if (a >= 0x8000) return prgRead(a);
    if (a >= 0x6000) return cart.prgRam[a & 0x1FFF];
    if (a >= 0x5800) return static_cast<uint8_t>((counter_ >> 8) | (irqOn_ ? 0x80 : 0));
    if (a >= 0x5000) return static_cast<uint8_t>(counter_ & 0xFF);
    if (a >= 0x4800) {
      const uint8_t v = sound(port_ & 0x7F);
      if (port_ & 0x80) port_ = static_cast<uint8_t>(0x80 | ((port_ + 1) & 0x7F));
      return v;
    }
    return ob;
  }
  uint8_t cpuPeek(uint16_t a) override {
    if (a >= 0x8000) return prgRead(a);
    if (a >= 0x6000) return cart.prgRam[a & 0x1FFF];
    return 0;
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x4800) return;
    if (a < 0x5000) {
      uint8_t& m = cart.prgRam[0x2000 + (port_ & 0x7F)];
      if (m != v) { m = v; if (cart.battery) nes->dirty = true; }
      if (port_ & 0x80) port_ = static_cast<uint8_t>(0x80 | ((port_ + 1) & 0x7F));
      return;
    }
    if (a < 0x5800) { counter_ = static_cast<uint16_t>((counter_ & 0x7F00) | v); irq = false; return; }
    if (a < 0x6000) { counter_ = static_cast<uint16_t>((counter_ & 0xFF) | ((v & 0x7F) << 8)); irqOn_ = v & 0x80; irq = false; return; }
    if (a < 0x8000) {
      // proteção: $F800 com 0100 nos bits altos libera; os bits baixos protegem cada bloco de 2 KB
      if ((port_ & 0xF0) == 0x40 && !((port_ >> ((a - 0x6000) >> 11)) & 1)) {
        uint8_t& m = cart.prgRam[a & 0x1FFF];
        if (m != v) { m = v; if (cart.battery) nes->dirty = true; }
      }
      return;
    }
    if (a < 0xC000) { chr_[(a - 0x8000) >> 11] = v; return; }
    if (a < 0xE000) { nt_[(a - 0xC000) >> 11] = v; return; }
    if (a < 0xE800) { prg_[0] = v & 0x3F; sndOff_ = v & 0x40; }
    else if (a < 0xF000) { prg_[1] = v & 0x3F; chrRamOff_ = v & 0xC0; }
    else if (a < 0xF800) prg_[2] = v & 0x3F;
    else { port_ = v; return; }
    update();
  }
  void cpuCycle() override {
    if (irqOn_ && counter_ < 0x7FFF && ++counter_ == 0x7FFF) irq = true;
    // um canal por vez a cada 15 ciclos, do 7 para baixo
    if (++div_ < 15) return;
    div_ = 0;
    const int n = ((sound(0x7F) >> 4) & 7) + 1;
    if (cur_ < 8 - n) cur_ = 7;
    const int b = 0x40 + cur_ * 8;
    const uint32_t freq = sound(b) | (sound(b + 2) << 8) | ((sound(b + 4) & 3) << 16);
    uint32_t phase = sound(b + 1) | (sound(b + 3) << 8) | (sound(b + 5) << 16);
    const uint32_t len = static_cast<uint32_t>(256 - (sound(b + 4) & 0xFC)) << 16;
    phase = (phase + freq) % len;
    setSound(b + 1, phase & 0xFF); setSound(b + 3, (phase >> 8) & 0xFF); setSound(b + 5, (phase >> 16) & 0xFF);
    const int s = ((phase >> 16) + sound(b + 6)) & 0xFF;
    const int nib = (sound(s >> 1) >> ((s & 1) * 4)) & 15;
    out_[cur_] = static_cast<int16_t>((nib - 8) * (sound(b + 7) & 15));
    cur_--;
    int sum = 0;
    for (int k = 8 - n; k < 8; k++) sum += out_[k];
    mix_ = sndOff_ ? 0.0f : sum * (0.00125f / static_cast<float>(n));
  }
  float audio() override { return mix_; }
  void syncRegs(StateIO& io) override {
    io.raw(chr_, sizeof chr_); io.raw(nt_, sizeof nt_); io.raw(prg_, sizeof prg_);
    io.sync(sndOff_); io.sync(chrRamOff_); io.sync(port_); io.sync(counter_); io.sync(irqOn_); io.sync(irq);
    io.sync(div_); io.sync(cur_); io.raw(out_, sizeof out_); io.sync(mix_);
    if (io.reading()) { counter_ &= 0x7FFF; clampTo(div_, 0, 14); cur_ &= 7; for (auto& o : out_) clampTo<int16_t>(o, -120, 105); if (!(mix_ > -2 && mix_ < 2)) mix_ = 0; }
  }

private:
  uint8_t sound(int i) const { return cart.prgRam[0x2000 + (i & 0x7F)]; }
  void setSound(int i, uint32_t v) { cart.prgRam[0x2000 + (i & 0x7F)] = static_cast<uint8_t>(v); }
  uint8_t chr_[8] = {}, nt_[4] = {}, prg_[3] = {}, chrRamOff_ = 0, port_ = 0;
  bool sndOff_ = false, irqOn_ = false;
  uint16_t counter_ = 0;
  int div_ = 0, cur_ = 7;
  int16_t out_[8] = {};
  float mix_ = 0;
};

// ================================================================ som do Sunsoft 5B (AY-3-8910 com envelope de 32 passos)
class Sunsoft5B {
public:
  Sunsoft5B() {
    for (int i = 0; i < 32; i++) level_[i] = i ? static_cast<float>(std::pow(10.0, (i - 31) * 1.5 / 20.0)) : 0.0f;
  }
  void reset() {
    std::memset(reg_, 0, sizeof reg_);
    for (int k = 0; k < 3; k++) { cnt_[k] = 0; tone_[k] = false; }
    noiseCnt_ = 0; noiseHalf_ = false; lfsr_ = 1; envCnt_ = 0; envStep_ = 0; envHold_ = false; envAttack_ = false; div_ = 0; out_ = 0;
  }
  void write(uint8_t r, uint8_t v) {
    r &= 15;
    reg_[r] = v;
    if (r == 13) { envStep_ = 0; envHold_ = false; envAttack_ = v & 4; envCnt_ = 0; }
  }
  void clock() {
    if (++div_ < 16) return;
    div_ = 0;
    for (int k = 0; k < 3; k++) {
      const int period = std::max(1, reg_[k * 2] | ((reg_[k * 2 + 1] & 15) << 8));
      if (++cnt_[k] >= period) { cnt_[k] = 0; tone_[k] = !tone_[k]; }
    }
    noiseHalf_ = !noiseHalf_;
    if (noiseHalf_ && ++noiseCnt_ >= std::max(1, reg_[6] & 0x1F)) {
      noiseCnt_ = 0;
      lfsr_ = (lfsr_ >> 1) | (((lfsr_ ^ (lfsr_ >> 3)) & 1) << 16);
    }
    const int envPeriod = std::max(1, reg_[11] | (reg_[12] << 8));
    if (++envCnt_ >= envPeriod) {
      envCnt_ = 0;
      if (!envHold_ && ++envStep_ >= 32) {
        const uint8_t shape = reg_[13];
        if (!(shape & 8)) { envHold_ = true; envAttack_ = false; envStep_ = 31; }  // termina em silêncio
        else if (shape & 1) { envHold_ = true; if (shape & 2) envAttack_ = !envAttack_; envStep_ = 31; }
        else { envStep_ = 0; if (shape & 2) envAttack_ = !envAttack_; }
      }
    }
    const int env = envHold_ && !(reg_[13] & 8) ? 0 : (envAttack_ ? envStep_ : 31 - envStep_);
    float sum = 0;
    for (int k = 0; k < 3; k++) {
      const bool t = ((reg_[7] >> k) & 1) || tone_[k];
      const bool n = ((reg_[7] >> (k + 3)) & 1) || (lfsr_ & 1);
      if (!t || !n) continue;
      const uint8_t v = reg_[8 + k];
      const int idx = (v & 0x10) ? env : ((v & 15) ? (v & 15) * 2 + 1 : 0);
      sum += level_[idx];
    }
    out_ = sum * 0.12f;
  }
  float out() const { return out_; }
  void sync(StateIO& io) {
    io.raw(reg_, sizeof reg_); io.raw(cnt_, sizeof cnt_);
    for (bool& t : tone_) io.sync(t);
    io.sync(noiseCnt_); io.sync(noiseHalf_); io.sync(lfsr_); io.sync(envCnt_); io.sync(envStep_); io.sync(envHold_); io.sync(envAttack_);
    io.sync(div_); io.sync(out_);
    if (io.reading()) {
      for (int& c : cnt_) clampTo(c, 0, 4095);
      clampTo(noiseCnt_, 0, 31); lfsr_ &= 0x1FFFF; if (!lfsr_) lfsr_ = 1; clampTo(envCnt_, 0, 65535); clampTo(envStep_, 0, 31);
      clampTo(div_, 0, 15); if (!(out_ >= 0 && out_ < 2)) out_ = 0;
    }
  }

private:
  uint8_t reg_[16] = {};
  int cnt_[3] = {};
  bool tone_[3] = {};
  int noiseCnt_ = 0;
  bool noiseHalf_ = false;
  uint32_t lfsr_ = 1;
  int envCnt_ = 0, envStep_ = 0;
  bool envHold_ = false, envAttack_ = false;
  int div_ = 0;
  float out_ = 0;
  float level_[32] = {};
};

// ================================================================ 69: Sunsoft FME-7 / 5A / 5B (IRQ que conta ciclos de CPU)
class FME7 : public Mapper {
public:
  explicit FME7(Cart& c) : Mapper(c) {}
  void reset() override {
    cmd_ = 0;
    for (int k = 0; k < 8; k++) chr_[k] = static_cast<uint8_t>(k);
    for (int k = 0; k < 4; k++) prg_[k] = static_cast<uint8_t>(k);
    mirrorReg_ = 0; irqCtl_ = 0; counter_ = 0; irq = false;
    audioAddr_ = 0;
    snd_.reset();
    update();
  }
  void update() override {
    for (int k = 0; k < 8; k++) chr1(k, chr_[k]);
    prg8(0, prg_[1] & 0x3F); prg8(1, prg_[2] & 0x3F); prg8(2, prg_[3] & 0x3F); prg8(3, -1);
    // $6000: bit 6 = RAM (com o bit 7 ligando) ou ROM
    if (prg_[0] & 0x40) { lowRom = -1; ramEnabled = ramWritable = (prg_[0] & 0x80) != 0; }
    else { lowRom = prgBank8(prg_[0] & 0x3F); ramEnabled = ramWritable = false; }
    static const int M[4] = {VERTICAL, HORIZONTAL, SINGLE0, SINGLE1};
    mirror(M[mirrorReg_ & 3]);
  }
  void writeReg(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return;
    if (a >= 0xE000) { snd_.write(audioAddr_, v); return; }  // 5B: dado do som
    if (a >= 0xC000) { audioAddr_ = v & 15; return; }       // 5B: registrador do som
    if (a < 0xA000) { cmd_ = v & 0xF; return; }
    switch (cmd_) {
      case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: chr_[cmd_] = v; break;
      case 8: prg_[0] = v; break;
      case 9: prg_[1] = v; break;
      case 0xA: prg_[2] = v; break;
      case 0xB: prg_[3] = v; break;
      case 0xC: mirrorReg_ = v & 3; break;
      case 0xD: irqCtl_ = v; irq = false; break;
      case 0xE: counter_ = static_cast<uint16_t>((counter_ & 0xFF00) | v); break;
      case 0xF: counter_ = static_cast<uint16_t>((counter_ & 0x00FF) | (v << 8)); break;
    }
    update();
  }
  void cpuCycle() override {
    snd_.clock();
    if (!(irqCtl_ & 0x80)) return;
    if (counter_-- == 0 && (irqCtl_ & 1)) irq = true;
  }
  float audio() override { return snd_.out(); }
  void syncRegs(StateIO& io) override {
    io.sync(cmd_); io.raw(chr_, sizeof chr_); io.raw(prg_, sizeof prg_); io.sync(mirrorReg_); io.sync(irqCtl_); io.sync(counter_); io.sync(irq);
    io.sync(audioAddr_);
    snd_.sync(io);
    if (io.reading()) audioAddr_ &= 15;
  }

private:
  uint8_t cmd_ = 0, chr_[8] = {}, prg_[4] = {}, mirrorReg_ = 0, irqCtl_ = 0, audioAddr_ = 0;
  uint16_t counter_ = 0;
  Sunsoft5B snd_;
};

}  // namespace

std::unique_ptr<Mapper> createExtMapper(Cart& c) {
  switch (c.mapper) {
    case 5: return std::make_unique<MMC5>(c);
    case 19: return std::make_unique<Namco163>(c);
    case 24: case 26: return std::make_unique<VRC6>(c, c.mapper);
    case 69: return std::make_unique<FME7>(c);
    case 85: return std::make_unique<VRC7>(c);
    default: return nullptr;
  }
}

}  // namespace indigo::nes
