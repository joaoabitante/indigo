#include "cps.h"

#include <cstring>
#include <stdexcept>

namespace indigo::cps {

namespace {
constexpr int LINE_PIX = 512, LINES = 262;
constexpr int64_t CPS1_Z80_HZ = 3579545, PIX_HZ = 8000000;

const GameDef& findGame(const std::vector<uint8_t>& zip, const std::string& nome) {
  std::string falta;
  const GameDef* g = identify(zip, nome, &falta);
  if (g) return *g;
  if (!falta.empty())
    throw std::runtime_error("Pacote de CPS incompleto. Faltam: " + falta.substr(0, 300) +
                             (falta.size() > 300 ? "..." : "") +
                             ". Se for uma versão (clone), use o pacote \"merged\" ou \"non-merged\" do MAME.");
  throw std::runtime_error("Jogo de CPS1/CPS2 não reconhecido neste pacote.");
}

inline uint16_t combine(uint16_t old, uint16_t v, uint16_t mask) { return static_cast<uint16_t>((old & ~mask) | (v & mask)); }
}  // namespace

bool isCpsSet(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo) {
  return identify(zip, nomeMinusculo, nullptr) != nullptr;
}

Cps::Cps(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo, const std::vector<uint8_t>* save)
    : game(findGame(zip, nomeMinusculo)), cps2(game.kind == 2), cpu(*this), z80(*this), cfg_(cpsbConfig(game.cpsb)) {
  loadRegions(game, zip, rg_);
  if (rg_[RG_MAIN].size() < 8) throw std::runtime_error("Programa do jogo vazio.");
  if (cps2) {
    const Cps2Key key = cps2ParseKey(rg_[RG_KEY]);
    if (!key.valid) throw std::runtime_error("Falta a chave do CPS2 (arquivo .key) no pacote.");
    cps2Decrypt(rg_[RG_MAIN], dec_, key);
    // os gráficos do CPS2 vêm embaralhados em blocos de 2 MB (como o "unshuffle" do MAME)
    std::vector<uint8_t>& g = rg_[RG_GFX];
    const size_t bank = 0x200000;
    std::vector<uint64_t> tmp(bank / 8);
    for (size_t off = 0; off + bank <= g.size(); off += bank) {
      std::memcpy(tmp.data(), g.data() + off, bank);
      // permutação recursiva: em cada metade, troca o segundo quarto com o terceiro
      auto unshuffle = [&](auto&& self, uint64_t* buf, size_t len) -> void {
        if (len == 2) return;
        len /= 2;
        self(self, buf, len);
        self(self, buf + len, len);
        for (size_t i = 0; i < len / 2; i++) std::swap(buf[len / 2 + i], buf[len + i]);
      };
      unshuffle(unshuffle, tmp.data(), bank / 8);
      std::memcpy(g.data() + off, tmp.data(), bank);
    }
    cpuNum_ = 1475;  // 16 MHz x 0,7375 (estados de espera, como o MAME)
  } else {
    cpuNum_ = (game.flags & GF_12MHZ) ? 1500 : 1250;
  }
  ram_.assign(0x10000, 0);
  ram2_.assign(0x4002, 0);
  gfxram_.assign(0x20000, 0);
  obj2_[0].assign(0x1000, 0);
  obj2_[1].assign(0x1000, 0);
  zram_.assign(cps2 ? 0x1000 : 0x800, 0);
  zram2_.assign(0x1000, 0);
  objBuf_.assign(cps2 ? 0x1000 : 0x400, 0);
  fbIdx_.assign(384 * 224, 0xBFF);
  spr_.assign(384 * 224, 0);
  out_.assign(384 * 224, 0xFF000000u);
  qs_.setRom(&rg_[RG_QSOUND]);
  if (save) importSave(*save);
  eeprom_.dirty = false;
  reset();
}

Cps::~Cps() = default;

void Cps::reset() {
  std::memset(cpsA_, 0, sizeof cpsA_);
  std::memset(cpsB_, 0, sizeof cpsB_);
  // valores iniciais do CPS-A (os mesmos do MAME)
  cpsA_[0] = 0x9200; cpsA_[1] = 0x9000; cpsA_[2] = 0x9040; cpsA_[3] = 0x9080; cpsA_[4] = 0x9100;
  std::fill(ram_.begin(), ram_.end(), 0);
  std::fill(ram2_.begin(), ram2_.end(), 0);
  std::fill(gfxram_.begin(), gfxram_.end(), 0);
  for (auto& o : obj2_) std::fill(o.begin(), o.end(), 0);
  std::fill(zram_.begin(), zram_.end(), 0);
  std::fill(zram2_.begin(), zram2_.end(), 0);
  std::fill(objBuf_.begin(), objBuf_.end(), 0);
  std::memset(out2_, 0, sizeof out2_);
  std::memset(pal_, 0, sizeof pal_);
  for (auto& c : color_) c = 0xFF000000u;
  priCtrl_ = objBank_ = 0;
  latch_[0] = latch_[1] = 0;
  sndBank_ = 0;
  dial_ = dialBase_ = 0;
  z80Reset_ = false;
  irq2_ = irq4_ = false;
  for (int i = 0; i < 3; i++) rasterCnt_[i] = rasterReload_[i] = 0;
  rasterAt_ = -1;
  curLine_ = 0;
  pix_ = cpuCyc_ = z80Cyc_ = sndDone_ = okiNext_ = 0;
  z80IrqNext_ = 32000;
  z80IrqHold_ = false;
  frame_ = 0;
  lastLine_ = 15;
  ym_.reset();
  oki_.reset();
  oki_.pin7 = true;
  qs_.reset();
  okiOut_ = 0;
  resFrac_ = 0; accL_ = accR_ = 0;
  eeprom_.resetLines();
  cpu.reset();
  updateIrq();
  z80.reset();
}

// ------------------------------------------------------------------ memória do 68000
uint32_t Cps::cpsBase(int reg, uint32_t boundary) const {
  uint32_t base = static_cast<uint32_t>(cpsA_[reg]) * 256;
  base &= ~(boundary - 1);
  return base & 0x3FFFF;
}

uint16_t Cps::inputWord(int which) const {
  const uint32_t k = keys_;
  const bool six = (game.flags & GF_6BUTTONS) != 0;
  const bool forgottn = !cps2 && std::strncmp(game.name, "forgottn", 8) == 0;
  bool b[7] = {};
  if (six) {
    b[1] = k & KEY_Y; b[2] = k & KEY_X; b[3] = k & KEY_R; b[4] = k & KEY_B; b[5] = k & KEY_A; b[6] = k & KEY_L;
  } else {
    b[1] = (k & (KEY_Y | KEY_B)) != 0; b[2] = (k & (KEY_X | KEY_A)) != 0; b[3] = k & KEY_R;
    b[4] = !forgottn && (k & KEY_L);
  }
  uint8_t joy = 0xFF;
  if (k & KEY_RIGHT) joy &= ~0x01;
  if (k & KEY_LEFT) joy &= ~0x02;
  if (k & KEY_DOWN) joy &= ~0x04;
  if (k & KEY_UP) joy &= ~0x08;
  if (b[1]) joy &= ~0x10;
  if (b[2]) joy &= ~0x20;
  if (b[3]) joy &= ~0x40;
  switch (which) {
    case 0:  // CPS1: jogadores (IN1); CPS2: IN0
      if (cps2 && !six && b[4]) joy &= ~0x80;
      return static_cast<uint16_t>(0xFF00 | joy);
    case 1: {  // CPS2 IN1: botões 4-6 do jogador 1 (jogos de 6 botões)
      uint16_t v = 0xFFFF;
      if (six) {
        if (b[4]) v &= ~0x0001;
        if (b[5]) v &= ~0x0002;
        if (b[6]) v &= ~0x0004;
      } else if (!(game.flags & GF_4PLAYERS) && b[4]) {
        v &= ~0x0001;
      }
      return v;
    }
    case 2: {  // CPS1 IN2 (placa C): botões 4-6
      uint16_t v = 0x00FF;
      if (b[4]) v &= ~0x01;
      if (b[5]) v &= ~0x02;
      if (b[6]) v &= ~0x04;
      return v;
    }
    default: {  // sistema: CPS1 IN0 (byte) / CPS2 IN2
      if (cps2) {
        uint16_t v = 0xFFFE | (eeprom_.dataOut() ? 1 : 0);
        if (k & KEY_START) v &= ~0x0100;
        if (k & KEY_SELECT) v &= ~0x1000;
        return v;
      }
      uint8_t v = 0xFF;
      if (k & KEY_SELECT) v &= ~0x01;
      if (k & KEY_START) v &= ~0x10;
      return v;
    }
  }
}

uint16_t Cps::cpsbRead(int off) {
  const CpsbConfig& c = cfg_;
  if (off == c.cpsbAddr / 2) return static_cast<uint16_t>(c.cpsbValue);
  if (off == c.multLo / 2) return static_cast<uint16_t>((static_cast<uint32_t>(cpsB_[(c.mult1 / 2) & 0x1F]) * cpsB_[(c.mult2 / 2) & 0x1F]) & 0xFFFF);
  if (off == c.multHi / 2) return static_cast<uint16_t>((static_cast<uint32_t>(cpsB_[(c.mult1 / 2) & 0x1F]) * cpsB_[(c.mult2 / 2) & 0x1F]) >> 16);
  if (game.in2 && off == game.in2 / 2) return inputWord(2);
  if (game.in3 && off == game.in3 / 2) return 0xFFFF;
  if (cps2) {
    if (off == 0x0E / 2) {
      // posição horizontal (de 2 em 2 pixels) relativa ao contador de raster 3
      const int hpos = static_cast<int>(((cpuCyc_ * 1000) / cpuNum_) - pix_);
      return static_cast<uint16_t>((((rasterCnt_[2] - hpos / 2) << 1) & 0x1FE) | (cpsB_[0x0E / 2] & 1));
    }
    if (off == 0x10 / 2 || off == 0x12 / 2) return rasterCnt_[off & 1] & 0x1FF;
  }
  return 0xFFFF;
}

void Cps::cpsbWrite(int off, uint16_t v, uint16_t mask) {
  off &= 0x1F;
  cpsB_[off] = combine(cpsB_[off], v, mask);
  if (cps2) {
    const uint16_t d = cpsB_[off];
    if (off == 0x0E / 2) { rasterReload_[2] = (d >> 1) & 0xFF; return; }
    if (off == 0x10 / 2 || off == 0x12 / 2) {
      rasterReload_[off & 1] = d & 0x1FF;
      if (d & 0x8000) rasterCnt_[off & 1] = rasterReload_[off & 1];
    }
  }
}

void Cps::cpsaWrite(int off, uint16_t v, uint16_t mask) {
  off &= 0x1F;
  cpsA_[off] = combine(cpsA_[off], v, mask);
  if (off == 5) buildPalette();  // a cópia da paleta acontece na escrita do endereço da paleta
}

uint16_t Cps::ioRead(uint32_t a) {
  if (!cps2) {
    if (a <= 0x800007) return inputWord(0);
    if (a >= 0x800018 && a <= 0x80001F) {
      const int off = (a - 0x800018) >> 1;
      const uint8_t in = off == 0 ? static_cast<uint8_t>(inputWord(3)) : game.dsw[off - 1];
      return static_cast<uint16_t>((in << 8) | 0xFF);
    }
    if (std::strncmp(game.name, "forgottn", 8) == 0 || std::strncmp(game.name, "lostwrld", 8) == 0) {
      const int cnt = (dial_ - dialBase_) & 0xFFF;
      if (a == 0x800052) return static_cast<uint16_t>(0xFF00 | (cnt & 0xFF));
      if (a == 0x800054) return static_cast<uint16_t>(0xFF00 | (cnt >> 8));
      if (a == 0x80005A || a == 0x80005C) return 0xFF00;
    }
    const bool c0 = (game.flags & GF_CPSB_C0) != 0;
    if (!c0 && a >= 0x800140 && a <= 0x80017F) return cpsbRead((a - 0x800140) >> 1);
    if ((c0 || (game.flags & GF_CPSB_MIRROR)) && a >= 0x8001C0 && a <= 0x8001FF) return cpsbRead((a - 0x8001C0) >> 1);
    return 0;
  }
  if (a >= 0x800140 && a <= 0x80017F) return cpsbRead((a - 0x800140) >> 1);
  if (a >= 0x804140 && a <= 0x80417F) return cpsbRead((a - 0x804140) >> 1);
  switch (a) {
    case 0x804000: return inputWord(0);
    case 0x804010: return inputWord(1);
    case 0x804020: return inputWord(3);
    case 0x804030: return (game.flags & GF_SINGLE) ? 0xD000 : 0xE021;  // volume digital no máximo
    case 0x8040B0: case 0x8040B2: return 0xFFFF;
    default: return 0;
  }
}

void Cps::ioWrite(uint32_t a, uint16_t v, uint16_t mask) {
  if (!cps2) {
    if (a >= 0x800100 && a <= 0x80013F) { cpsaWrite((a - 0x800100) >> 1, v, mask); return; }
    const bool c0 = (game.flags & GF_CPSB_C0) != 0;
    if (!c0 && a >= 0x800140 && a <= 0x80017F) { cpsbWrite((a - 0x800140) >> 1, v, mask); return; }
    if ((c0 || (game.flags & GF_CPSB_MIRROR)) && a >= 0x8001C0 && a <= 0x8001FF) { cpsbWrite((a - 0x8001C0) >> 1, v, mask); return; }
    if (a >= 0x800180 && a <= 0x800187) { latch_[0] = (mask & 0x00FF) ? static_cast<uint8_t>(v) : static_cast<uint8_t>(v >> 8); return; }
    if (a >= 0x800188 && a <= 0x80018F) { if (mask & 0x00FF) latch_[1] = static_cast<uint8_t>(v); return; }
    if (a == 0x800040) { dialBase_ = dial_; return; }  // Forgotten Worlds: zera o contador do giratório
    return;
  }
  if (a >= 0x800100 && a <= 0x80013F) { cpsaWrite((a - 0x800100) >> 1, v, mask); return; }
  if (a >= 0x804100 && a <= 0x80413F) { cpsaWrite((a - 0x804100) >> 1, v, mask); return; }
  if (a >= 0x800140 && a <= 0x80017F) { cpsbWrite((a - 0x800140) >> 1, v, mask); return; }
  if (a >= 0x804140 && a <= 0x80417F) { cpsbWrite((a - 0x804140) >> 1, v, mask); return; }
  if (a == 0x804040) {
    if (mask & 0xFF00) eeprom_.write((v & 0x4000) != 0, (v & 0x2000) != 0, (v & 0x1000) != 0);
    if (mask & 0x00FF) {
      const bool rst = !(v & 0x08);  // bit 3 = 0 segura o Z80 em reset
      if (rst && !z80Reset_) { z80.reset(); z80IrqHold_ = false; z80.irqLine = false; }
      z80Reset_ = rst;
    }
    return;
  }
  if (a == 0x8040E0) { if (mask & 0x00FF) objBank_ = v & 1; return; }
}

uint16_t Cps::readWord(uint32_t a, uint16_t) {
  a &= 0xFFFFFE;
  if (a < 0x400000) {
    const std::vector<uint8_t>& r = rg_[RG_MAIN];
    return a + 1 < r.size() ? static_cast<uint16_t>((r[a] << 8) | r[a + 1]) : 0;
  }
  if (a >= 0xFF0000) return static_cast<uint16_t>((ram_[a & 0xFFFF] << 8) | ram_[(a & 0xFFFF) + 1]);
  if (a >= 0x900000 && a < 0x930000) return gfxram_[(a - 0x900000) >> 1];
  if (a >= 0x800000 && a < 0x810000) return ioRead(a);
  if (cps2) {
    if (a >= 0x400000 && a < 0x40000C) return out2_[(a - 0x400000) >> 1];
    if (a >= 0x618000 && a < 0x61A000) return static_cast<uint16_t>(0xFF00 | zram_[((a - 0x618000) >> 1) & 0xFFF]);
    if (a >= 0x660000 && a < 0x664002) return static_cast<uint16_t>((ram2_[a - 0x660000] << 8) | ram2_[a - 0x660000 + 1]);
    if (a >= 0x708000 && a < 0x710000) return obj2_[objBank_ ^ 1][(a & 0x1FFF) >> 1];
  }
  return 0;
}

void Cps::writeWord(uint32_t a, uint16_t v, uint16_t mask) {
  a &= 0xFFFFFE;
  if (a >= 0xFF0000) {
    const uint32_t o = a & 0xFFFF;
    if (mask & 0xFF00) ram_[o] = static_cast<uint8_t>(v >> 8);
    if (mask & 0x00FF) ram_[o + 1] = static_cast<uint8_t>(v);
    return;
  }
  if (a >= 0x900000 && a < 0x930000) {
    uint16_t& w = gfxram_[(a - 0x900000) >> 1];
    w = combine(w, v, mask);
    return;
  }
  if (a >= 0x800000 && a < 0x810000) { ioWrite(a, v, mask); return; }
  if (cps2) {
    if (a >= 0x400000 && a < 0x40000C) { uint16_t& w = out2_[(a - 0x400000) >> 1]; w = combine(w, v, mask); return; }
    if (a >= 0x618000 && a < 0x61A000) { if (mask & 0x00FF) zram_[((a - 0x618000) >> 1) & 0xFFF] = static_cast<uint8_t>(v); return; }
    if (a >= 0x660000 && a < 0x664002) {
      const uint32_t o = a - 0x660000;
      if (mask & 0xFF00) ram2_[o] = static_cast<uint8_t>(v >> 8);
      if (mask & 0x00FF) ram2_[o + 1] = static_cast<uint8_t>(v);
      return;
    }
    if (a >= 0x700000 && a < 0x702000) { uint16_t& w = obj2_[objBank_][(a & 0x1FFF) >> 1]; w = combine(w, v, mask); return; }
    if (a >= 0x708000 && a < 0x710000) { uint16_t& w = obj2_[objBank_ ^ 1][(a & 0x1FFF) >> 1]; w = combine(w, v, mask); return; }
  }
}

uint8_t Cps::read8(uint32_t a) {
  const uint16_t w = readWord(a, (a & 1) ? 0x00FF : 0xFF00);
  return (a & 1) ? static_cast<uint8_t>(w) : static_cast<uint8_t>(w >> 8);
}
uint16_t Cps::read16(uint32_t a) { return readWord(a, 0xFFFF); }
void Cps::write8(uint32_t a, uint8_t v) {
  writeWord(a, static_cast<uint16_t>((v << 8) | v), (a & 1) ? 0x00FF : 0xFF00);
}
void Cps::write16(uint32_t a, uint16_t v) { writeWord(a, v, 0xFFFF); }

uint16_t Cps::fetch16(uint32_t a) {
  if (cps2 && a < 0x400000) {
    a &= 0xFFFFFE;
    return a + 1 < dec_.size() ? static_cast<uint16_t>((dec_[a] << 8) | dec_[a + 1]) : 0;
  }
  return readWord(a, 0xFFFF);
}

int Cps::intAck(int level) {
  // o reconhecimento (autovetor) apaga as duas linhas, IPL1 e IPL2
  irq2_ = irq4_ = false;
  updateIrq();
  return 24 + level;
}

// ------------------------------------------------------------------ Z80
int Cps::read(int a) {
  a &= 0xFFFF;
  const std::vector<uint8_t>& rom = rg_[RG_AUDIO];
  if (a < 0x8000) return static_cast<size_t>(a) < rom.size() ? rom[static_cast<size_t>(a)] : 0xFF;
  if (a < 0xC000) {
    const size_t o = 0x10000 + static_cast<size_t>(sndBank_) * 0x4000 + static_cast<size_t>(a - 0x8000);
    return o < rom.size() ? rom[o] : 0xFF;
  }
  if (cps2) {
    if (a < 0xD000) return zram_[static_cast<size_t>(a - 0xC000)];
    if (a == 0xD007) return qs_.read();
    if (a >= 0xF000) return zram2_[static_cast<size_t>(a - 0xF000)];
    return 0xFF;
  }
  if (a >= 0xD000 && a < 0xD800) return zram_[static_cast<size_t>(a - 0xD000)];
  switch (a) {
    case 0xF000: case 0xF001: runSound(z80Pix()); return ym_.status();
    case 0xF002: runSound(z80Pix()); return oki_.read();
    case 0xF008: return latch_[0];
    case 0xF00A: return latch_[1];
    default: return 0xFF;
  }
}

void Cps::write(int a, int v) {
  a &= 0xFFFF;
  const uint8_t b = static_cast<uint8_t>(v);
  if (cps2) {
    if (a >= 0xC000 && a < 0xD000) { zram_[static_cast<size_t>(a - 0xC000)] = b; return; }
    if (a >= 0xF000) { zram2_[static_cast<size_t>(a - 0xF000)] = b; return; }
    if (a >= 0xD000 && a <= 0xD002) { runSound(z80Pix()); qs_.write(a - 0xD000, b); return; }
    if (a == 0xD003) {
      int bank = b & 0x0F;
      if (0x10000 + static_cast<size_t>(bank) * 0x4000 >= rg_[RG_AUDIO].size()) bank = 0;
      sndBank_ = static_cast<uint8_t>(bank);
    }
    return;
  }
  if (a >= 0xD000 && a < 0xD800) { zram_[static_cast<size_t>(a - 0xD000)] = b; return; }
  switch (a) {
    case 0xF000: case 0xF001:
      runSound(z80Pix());
      ym_.write(a & 1, b);
      z80.irqLine = ym_.irq();
      break;
    case 0xF002: runSound(z80Pix()); oki_.write(b); break;
    case 0xF004: sndBank_ = b & 1; break;
    case 0xF006: runSound(z80Pix()); oki_.pin7 = b & 1; break;
    default: break;
  }
}

int64_t Cps::z80Pix() const { return cps2 ? z80Cyc_ : (z80Cyc_ * PIX_HZ) / CPS1_Z80_HZ; }

// ------------------------------------------------------------------ tempo
void Cps::runCpu(int64_t pixTarget) {
  const int64_t target = (pixTarget * cpuNum_) / 1000;
  while (cpuCyc_ < target) {
    if (rasterAt_ >= 0 && (cpuCyc_ * 1000) / cpuNum_ >= rasterAt_) {
      // interrupção de raster (IRQ4; IRQ6 se coincidir com a do vblank): desenha até a linha atual antes
      rasterAt_ = -1;
      irq4_ = true;
      updateIrq();
      renderUpTo(curLine_);
    }
    int c = cpu.step();
    if (c <= 0) c = 4;
    cpuCyc_ += c;
  }
}

void Cps::runZ80(int64_t pixTarget) {
  const int64_t target = cps2 ? pixTarget : (pixTarget * CPS1_Z80_HZ) / PIX_HZ;
  if (cps2 && z80Reset_) {
    z80Cyc_ = std::max(z80Cyc_, target);
    while (z80IrqNext_ <= z80Cyc_) z80IrqNext_ += 32000;
    return;
  }
  while (z80Cyc_ < target) {
    if (cps2) {
      // interrupção periódica de 250 Hz (8 MHz / 32000), segura até o Z80 aceitar
      if (z80Cyc_ >= z80IrqNext_) { z80IrqHold_ = true; z80IrqNext_ += 32000; }
      z80.irqLine = z80IrqHold_;
    } else {
      runSound(z80Pix());
      z80.irqLine = ym_.irq();
    }
    const bool ack = z80.irqLine && z80.iff1 && !z80.eiDelay && !z80.nmiPending;
    int c = z80.step();
    if (c <= 0) c = 4;
    z80Cyc_ += c;
    if (ack && cps2) { z80IrqHold_ = false; z80.irqLine = false; }
  }
}

void Cps::pushSample(float l, float r, double chipRate) {
  // reamostragem por média na janela de cada amostra de saída (filtro em caixa)
  const double step = chipRate / outRate_;
  double d = 1.0;
  while (d > 0) {
    if (resFrac_ <= 0) resFrac_ = step;
    const double t = d < resFrac_ ? d : resFrac_;
    accL_ += static_cast<float>(l * t);
    accR_ += static_cast<float>(r * t);
    resFrac_ -= t;
    d -= t;
    if (resFrac_ <= 1e-9) {
      bufL_[outW_] = static_cast<float>(accL_ / step);
      bufR_[outW_] = static_cast<float>(accR_ / step);
      outW_ = (outW_ + 1) % OUTBUF;
      if (outW_ == outRd_) outRd_ = (outRd_ + 1) % OUTBUF;
      accL_ = accR_ = 0;
      resFrac_ = step;
    }
  }
}

void Cps::runSound(int64_t pix) {
  if (cps2) {
    // QSound: 60 MHz / 2 / 1248 = 24038 Hz; uma amostra a cada 332,8 pixels
    const int64_t target = (pix * 5) / 1664;
    const double rate = 60000000.0 / 2496.0;
    while (sndDone_ < target) {
      int L, R;
      qs_.sample(L, R);
      sndDone_++;
      pushSample(static_cast<float>(L) / 32768.0f, static_cast<float>(R) / 32768.0f, rate);
    }
    return;
  }
  // YM2151: relógio / 64 = 55930 Hz; OKI: 1 MHz / 132 (ou / 165) na mesma linha do tempo
  const int64_t target = (pix * CPS1_Z80_HZ) / (PIX_HZ * 64);
  const double rate = static_cast<double>(CPS1_Z80_HZ) / 64.0;
  while (sndDone_ < target) {
    const int64_t t = (sndDone_ * PIX_HZ * 64) / CPS1_Z80_HZ;
    while (okiNext_ <= t) {
      okiOut_ = oki_.sample(rg_[RG_OKI]);
      okiNext_ += oki_.pin7 ? 1056 : 1320;
    }
    int L, R;
    ym_.sample(L, R);
    sndDone_++;
    const float m = (static_cast<float>(L + R) * 0.35f + static_cast<float>(okiOut_) * 0.30f) / 32768.0f;
    pushSample(m, m, rate);
  }
}

void Cps::runFrame() {
  // Forgotten Worlds: L/R giram o botão giratório (20 passos por quadro, como o PORT_KEYDELTA do MAME)
  if (keys_ & KEY_L) dial_ -= 20;
  if (keys_ & KEY_R) dial_ += 20;
  dial_ &= 0xFFFF;
  for (int line = 0; line < LINES; line++) {
    curLine_ = line;
    pix_ = static_cast<int64_t>(frame_) * LINES * LINE_PIX + static_cast<int64_t>(line) * LINE_PIX;
    if (line == 0) { lastLine_ = 15; drawSprites(); }
    if (cps2) {
      // contadores de raster do CPS-B-21: recarregam na linha 0 e contam para baixo a cada linha
      int pending = 0;
      for (int i = 0; i < 2; i++) {
        if (line == 0) rasterCnt_[i] = rasterReload_[i];
        else rasterCnt_[i] = (rasterCnt_[i] - 1) & 0x1FF;
        if (rasterCnt_[i] == 0) pending |= 1 << i;
      }
      rasterCnt_[2] = rasterReload_[2];
      if (pending) rasterAt_ = pix_ + rasterCnt_[2] * 2;
    }
    if (line == 240) {
      // começo do vblank: termina o desenho do quadro, guarda os objetos e pede a IRQ2
      renderUpTo(239);
      finishFrame();
      latchObjects();
      irq2_ = true;
      updateIrq();
    }
    runCpu(pix_ + LINE_PIX);
    runZ80(pix_ + LINE_PIX);
    runSound(pix_ + LINE_PIX);
  }
  frame_++;
}

void Cps::setSampleRate(int rate) { outRate_ = rate > 0 ? rate : 48000; }

void Cps::pullAudio(float* l, float* r, size_t n) {
  for (size_t k = 0; k < n; k++) {
    if (outRd_ == outW_) { l[k] = r[k] = 0; continue; }
    l[k] = bufL_[outRd_]; r[k] = bufR_[outRd_];
    outRd_ = (outRd_ + 1) % OUTBUF;
  }
}

// ------------------------------------------------------------------ save (EEPROM do CPS2)
std::vector<uint8_t> Cps::exportSave() {
  eeprom_.dirty = false;
  std::vector<uint8_t> b(128);
  for (int i = 0; i < 64; i++) { b[2 * i] = static_cast<uint8_t>(eeprom_.mem[i] >> 8); b[2 * i + 1] = static_cast<uint8_t>(eeprom_.mem[i]); }
  return b;
}

void Cps::importSave(const std::vector<uint8_t>& b) {
  if (!cps2 || b.size() < 128) return;
  for (int i = 0; i < 64; i++) eeprom_.mem[i] = static_cast<uint16_t>((b[2 * i] << 8) | b[2 * i + 1]);
}

// ------------------------------------------------------------------ save state
void Cps::sync(StateIO& io) {
  uint32_t magic = cps2 ? 0x43505332 : 0x43505331;  // "CPS2"/"CPS1"
  io.sync(magic);
  if (io.reading() && magic != (cps2 ? 0x43505332u : 0x43505331u)) throw std::runtime_error("Estado salvo de outro sistema.");
  uint32_t nameHash = 0;
  for (const char* p = game.name; *p; p++) nameHash = nameHash * 31 + static_cast<uint8_t>(*p);
  uint32_t h = nameHash;
  io.sync(h);
  if (io.reading() && h != nameHash) throw std::runtime_error("Estado salvo de outro jogo.");
  cpu.sync(io);
  z80.sync(io);
  io.bytes(ram_); io.bytes(ram2_); io.bytes(zram_); io.bytes(zram2_);
  {
    uint32_t n = static_cast<uint32_t>(gfxram_.size());
    io.sync(n);
    if (io.reading() && n != gfxram_.size()) throw std::runtime_error("Estado salvo incompatível com este jogo.");
    io.raw(gfxram_.data(), gfxram_.size() * 2);
    for (auto& o : obj2_) io.raw(o.data(), o.size() * 2);
    io.raw(objBuf_.data(), objBuf_.size() * 2);
  }
  io.raw(cpsA_, sizeof cpsA_); io.raw(cpsB_, sizeof cpsB_); io.raw(out2_, sizeof out2_); io.raw(pal_, sizeof pal_);
  io.sync(priCtrl_); io.sync(objBank_);
  io.raw(latch_, sizeof latch_); io.sync(sndBank_); io.sync(dial_); io.sync(dialBase_);
  eeprom_.sync(io);
  io.sync(z80Reset_); io.sync(irq2_); io.sync(irq4_);
  io.raw(rasterCnt_, sizeof rasterCnt_); io.raw(rasterReload_, sizeof rasterReload_);
  io.sync(rasterAt_); io.sync(pix_); io.sync(cpuCyc_); io.sync(z80Cyc_); io.sync(sndDone_); io.sync(okiNext_);
  io.sync(z80IrqNext_); io.sync(z80IrqHold_); io.sync(frame_);
  ym_.sync(io); oki_.sync(io); qs_.sync(io);
  io.sync(okiOut_); io.sync(resFrac_); io.sync(accL_); io.sync(accR_);
  if (io.reading()) {
    objBank_ &= 1;
    priCtrl_ &= 0xFFFF;
    const size_t banks = rg_[RG_AUDIO].size() > 0x10000 ? (rg_[RG_AUDIO].size() - 0x10000) / 0x4000 : 0;
    if (sndBank_ >= banks && sndBank_ > 0) sndBank_ = 0;
    for (auto& r : rasterCnt_) r &= 0x1FF;
    for (auto& r : rasterReload_) r &= 0x1FF;
    // relógios: um estado danificado não pode deixar um laço sem fim no próximo quadro
    const int64_t base = static_cast<int64_t>(frame_) * LINES * LINE_PIX;
    if (frame_ > (1ull << 40)) throw std::runtime_error("Estado salvo danificado.");
    const int64_t cpuBase = (base * cpuNum_) / 1000;
    if (cpuCyc_ < cpuBase - 4 * LINE_PIX * 2 || cpuCyc_ > cpuBase + 4 * LINE_PIX * 2) cpuCyc_ = cpuBase;
    const int64_t zBase = cps2 ? base : (base * CPS1_Z80_HZ) / PIX_HZ;
    if (z80Cyc_ < zBase - 4 * LINE_PIX || z80Cyc_ > zBase + 4 * LINE_PIX) z80Cyc_ = zBase;
    const int64_t sBase = cps2 ? (base * 5) / 1664 : (base * CPS1_Z80_HZ) / (PIX_HZ * 64);
    if (sndDone_ < sBase - 4096 || sndDone_ > sBase + 4096) sndDone_ = sBase;
    if (!cps2 && (okiNext_ < base - 4 * LINE_PIX || okiNext_ > base + 4 * LINE_PIX)) okiNext_ = base;
    if (cps2 && (z80IrqNext_ < zBase - 64000 || z80IrqNext_ > zBase + 64000)) z80IrqNext_ = zBase + 32000;
    if (rasterAt_ < -1 || rasterAt_ > base + LINES * LINE_PIX) rasterAt_ = -1;
    finiteOr(resFrac_, 0);
    clampTo(resFrac_, 0.0, 64.0);
    if (!(accL_ == accL_) || accL_ > 1e6f || accL_ < -1e6f) accL_ = 0;
    if (!(accR_ == accR_) || accR_ > 1e6f || accR_ < -1e6f) accR_ = 0;
    for (int i = 0; i < 0xC00; i++) colorOf(i);
    updateIrq();
  }
}

std::vector<uint8_t> Cps::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void Cps::loadState(const std::vector<uint8_t>& s) {
  StateIO io(s);
  sync(io);
}

std::string CpsCore::note() const {
  const std::string n = m_->game.name;
  if (n.rfind("forgottn", 0) == 0 || n.rfind("lostwrld", 0) == 0) return "L e R giram o personagem (botão giratório do arcade).";
  return "";
}

}  // namespace indigo::cps
