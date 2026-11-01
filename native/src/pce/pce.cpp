#include "pce.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::pce {

namespace {
const char STATE_MAGIC[4] = {'I', 'P', 'C', '1'};

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
  }
  return ~c;
}

// jogos que só existem para o SuperGrafx (CRC32 da ROM sem cabeçalho)
const uint32_t SGX_CRC[] = {0x8C4588E2, 0xCD854B38, 0x4C2126B0, 0xFF30DC62, 0x33A92136, 0x3B13AF61, 0x6D97373E,
                            0x130A2E77, 0xB486A8ED, 0x3334202A, 0xB9F758E4, 0xB5BEE732, 0x0E81080A, 0x7E4D6CAF,
                            0xD6722C04, 0x1F041166, 0x6B0C982E, 0xBEBFE042, 0xEB2466F9};
const uint32_t POPULOUS_CRC[] = {0x083C956A, 0x0A9ADE99};
const uint32_t SF2_CRC = 0xD15CB6BB;
}  // namespace

bool isPceRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (!(endsWith(n, ".pce") || endsWith(n, ".sgx"))) return false;
  return rom.size() >= 0x2000 && rom.size() <= (4u << 20) + 512;
}

std::unique_ptr<PceCore> createPceCore(const std::vector<uint8_t>& rom, const std::string& n,
                                       const std::vector<uint8_t>* save) {
  return std::make_unique<PceCore>(rom, save, endsWith(n, ".sgx"));
}

Pce::Pce(const std::vector<uint8_t>& rom, const std::vector<uint8_t>* save, bool sgxByName) {
  rom_ = rom;
  if (rom_.size() % 0x2000 == 512) rom_.erase(rom_.begin(), rom_.begin() + 512);  // cabeçalho de copiadora
  if (rom_.size() < 0x2000) throw std::runtime_error("ROM de PC Engine pequena demais.");
  if (rom_.size() > (4u << 20)) throw std::runtime_error("ROM de PC Engine grande demais.");
  const uint32_t crc = crc32(rom_.data(), rom_.size());
  romBanks_ = static_cast<int>((rom_.size() + 0x1FFF) / 0x2000);
  rom_.resize(static_cast<size_t>(romBanks_) * 0x2000, 0xFF);
  sgx = sgxByName || std::find(std::begin(SGX_CRC), std::end(SGX_CRC), crc) != std::end(SGX_CRC);
  sf2_ = romBanks_ > 0x80;
  sixButton_ = crc == SF2_CRC;
  if (std::find(std::begin(POPULOUS_CRC), std::end(POPULOUS_CRC), crc) != std::end(POPULOUS_CRC)) cardRam_.assign(0x8000, 0);
  ram_.assign(sgx ? 0x8000 : 0x2000, 0);
  // BRAM já formatada (como o CD-ROM deixa ao apagar tudo): "HUBM", fim da área em $8800, início dos dados em $8010
  bram_.assign(0x800, 0);
  const uint8_t hdr[8] = {0x48, 0x55, 0x42, 0x4D, 0x00, 0x88, 0x10, 0x80};
  std::copy(hdr, hdr + 8, bram_.begin());
  if (save && save->size() == bram_.size()) bram_ = *save;
  for (int i = 0; i < 1024; i++) {
    const int c = i & 0x1FF;
    int g = ((c >> 6) & 7) * 255 / 7, r = ((c >> 3) & 7) * 255 / 7, b = (c & 7) * 255 / 7;
    if (i & 0x200) { const int y = (r * 299 + g * 587 + b * 114) / 1000; r = g = b = y; }
    lut_[i] = 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
  }
  power();
}

std::string Pce::note() const {
  if (sgx) return "SuperGrafx";
  return "";
}

void Pce::importSave(const std::vector<uint8_t>& b) {
  if (b.size() == bram_.size()) { bram_ = b; bramDirty_ = false; }
}

void Pce::reset() { power(); }  // o PC Engine não tem botão de reset: é como ligar de novo

void Pce::power() {
  std::fill(ram_.begin(), ram_.end(), 0);
  std::fill(cardRam_.begin(), cardRam_.end(), 0);
  vce = Vce{};
  for (int i = 0; i < 2; i++) {
    vdc[i].vce = &vce;
    vdc[i].sys = this;
    vdc[i].index = i;
    vdc[i].power();
  }
  psg_.reset();
  psgClk_ = 0;
  timerOn_ = false; timerReload_ = timerCounter_ = 0; timerNext_ = 0;
  padSel_ = padClr_ = padExtra_ = 0;
  vpcPrio_[0] = vpcPrio_[1] = 0x11;
  vpcWin_[0] = vpcWin_[1] = 0;
  vpcSt2_ = 0;
  sf2Bank_ = 0;
  bramLocked_ = true;
  ioBuf_ = 0;
  std::memset(lines_, 0, sizeof lines_);
  std::memset(lineDiv_, 0, sizeof lineDiv_);
  std::memset(row2_, 0, sizeof row2_);
  frameReady_ = false;
  cpu = HuC6280{};
  cpu.bus = this;
  cpu.s = 0xFF;
  mapBanks();
  cpu.reset();
  buildFrame();
  schedule();
}

void Pce::mapBanks() {
  for (int b = 0; b < 0x80; b++) {
    int bank;
    if (sf2_) bank = b < 0x40 ? b : 0x40 + sf2Bank_ * 0x40 + (b - 0x40);
    else if (romBanks_ == 0x30) bank = b < 0x40 ? (b & 0x1F) : (b & 0x0F) + 0x20;
    else if (romBanks_ == 0x40) bank = b < 0x40 ? b : (b & 0x1F) + 0x20;
    else if (romBanks_ == 0x60) bank = b < 0x40 ? b : (b & 0x1F) + 0x40;
    else bank = b;
    bank %= romBanks_;
    cpu.rd[b] = rom_.data() + static_cast<size_t>(bank) * 0x2000;
    cpu.wr[b] = sink_.data();
  }
  if (sf2_) cpu.wr[0] = nullptr;  // escrita no banco 0 troca os bancos altos
  if (!cardRam_.empty())
    for (int b = 0x40; b < 0x60; b++) cpu.rd[b] = cpu.wr[b] = cardRam_.data() + ((b - 0x40) & 3) * 0x2000;
  for (int b = 0x80; b < 0x100; b++) { cpu.rd[b] = ffBank_.data(); cpu.wr[b] = sink_.data(); }
  for (int i = 0; i < 4; i++) {
    uint8_t* p = ram_.data() + (static_cast<size_t>(i) * 0x2000) % ram_.size();
    cpu.rd[0xF8 + i] = p;
    cpu.wr[0xF8 + i] = p;
  }
  if (ext_) ext_->mapBanks(cpu);
  cpu.rd[0xF7] = nullptr; cpu.wr[0xF7] = nullptr;  // BRAM
  cpu.rd[0xFF] = nullptr; cpu.wr[0xFF] = nullptr;  // E/S
}

// ------------------------------------------------------------ tempo
void Pce::schedule() {
  int64_t ne = vdc[0].nextTime();
  if (sgx) ne = std::min(ne, vdc[1].nextTime());
  if (timerOn_) ne = std::min(ne, timerNext_);
  if (ext_) ne = std::min(ne, ext_->nextEvent());
  cpu.nextEvent = ne;
}

void Pce::sync() {
  const int64_t t = cpu.clk;
  if (!sgx) {
    vdc[0].runTo(t);
  } else {
    while (vdc[0].now() < t || vdc[1].now() < t) {
      const int64_t s = std::min(t, std::min(vdc[0].nextTime(), vdc[1].nextTime()));
      vdc[1].runTo(s);
      vdc[0].runTo(s);
    }
  }
  runTimer(t);
  if (ext_) ext_->runTo(t);
  updateIrq();
  schedule();
}

void Pce::stall() {
  cpu.clk += 3;
  sync();
}

void Pce::runTimer(int64_t t) {
  while (timerOn_ && timerNext_ <= t) {
    if (timerCounter_ == 0) {
      timerCounter_ = timerReload_;
      cpu.irqActive |= HuC6280::TIQ;
    } else {
      timerCounter_--;
    }
    timerNext_ += 3072;
  }
}

uint8_t Pce::timerRead() const {
  // logo antes de recarregar, o contador aparece como $7F por alguns ciclos
  if (timerOn_ && timerCounter_ == 0 && timerNext_ - cpu.clk <= 15) return 0x7F;
  return timerCounter_;
}

void Pce::psgRun() {
  const int64_t n = (cpu.clk - psgClk_) / 6;
  if (n > 0) { psg_.run(n); psgClk_ += n * 6; }
}

void Pce::runFrame() {
  frameReady_ = false;
  const int64_t limit = cpu.clk + 1365LL * 263 * 3;
  while (!frameReady_ && cpu.clk < limit) cpu.step();
  psgRun();
}

// ------------------------------------------------------------ vídeo
void Pce::lineBegin(int idx, int scanline) {
  if (idx == 0 && scanline >= 14 && scanline < 256) lineDiv_[scanline - 14] = vce.div;
}

void Pce::lineDone(int idx, int scanline) {
  const int r = scanline - 14;
  if (r < 0 || r >= 242) return;
  if (!sgx) { std::memcpy(lines_[r], vdc[0].row, sizeof lines_[r]); return; }
  if (idx == 1) { std::memcpy(row2_, vdc[1].row, sizeof row2_); return; }
  composeSgx(lines_[r]);
}

void Pce::composeSgx(uint16_t* out) {
  const uint16_t* a = vdc[0].row;
  const uint16_t* b = row2_;
  const int n = std::min(Vdc::ROW, Vdc::LINE / vce.div);
  const int w1 = std::max(0, vpcWin_[0] - 16), w2 = std::max(0, vpcWin_[1] - 16);
  // configuração de cada região: 0 = fora das janelas, 1 = janela 1, 2 = janela 2, 3 = as duas
  const uint8_t cfg[4] = {static_cast<uint8_t>(vpcPrio_[1] >> 4), static_cast<uint8_t>(vpcPrio_[1] & 15),
                          static_cast<uint8_t>(vpcPrio_[0] >> 4), static_cast<uint8_t>(vpcPrio_[0] & 15)};
  for (int i = 0; i < n; i++) {
    const uint8_t c = cfg[(i < w1 ? 1 : 0) | (i < w2 ? 2 : 0)];
    uint16_t v;
    switch (c & 3) {
      case 0: v = vce.pal[0]; break;
      case 1: v = a[i]; break;
      case 2: v = b[i]; break;
      default: {
        const bool s1 = a[i] & Vdc::SPR_FLAG, s2 = b[i] & Vdc::SPR_FLAG, t1 = a[i] & Vdc::TRANSP_FLAG;
        switch ((c >> 2) & 3) {
          case 1: v = (t1 || (s2 && !s1)) ? b[i] : a[i]; break;  // sprites do VDC2 acima do fundo do VDC1
          case 2: {                                              // sprites do VDC1 atrás do fundo do VDC2
            const bool t2 = b[i] & Vdc::TRANSP_FLAG;
            v = (t1 || (s1 && !s2 && !t2)) ? b[i] : a[i];
            break;
          }
          default: v = t1 ? b[i] : a[i]; break;
        }
      }
    }
    out[i] = v;
  }
  for (int i = n; i < Vdc::ROW; i++) out[i] = 0;
}

void Pce::frameDone(int idx) {
  if (idx != 0) return;
  buildFrame();
  frameReady_ = true;
}

void Pce::buildFrame() {
  int count[5] = {};
  for (int r = 1; r <= 240; r++) {
    const int d = lineDiv_[r];
    count[(d >= 2 && d <= 4) ? d : 4]++;
  }
  int m = 4;
  if (count[3] > count[m]) m = 3;
  if (count[2] > count[m]) m = 2;
  const int start = m == 4 ? 48 : m == 3 ? 72 : 120;
  outW_ = m == 4 ? 256 : m == 3 ? 341 : 512;
  const int dbl = m == 2 ? 2 : 1;
  outH_ = 240 * dbl;
  for (int r = 0; r < 240; r++) {
    const uint16_t* src = lines_[r + 1];
    int d = lineDiv_[r + 1];
    if (d < 2 || d > 4) d = 4;
    uint32_t* dst = fb_.data() + static_cast<size_t>(r) * dbl * outW_;
    if (d == m) {
      for (int x = 0; x < outW_; x++) dst[x] = lut_[src[start + x] & 0x3FF];
    } else {
      for (int x = 0; x < outW_; x++) {
        int sx = (start + x) * m / d;
        if (sx >= Vdc::ROW) sx = Vdc::ROW - 1;
        dst[x] = lut_[src[sx] & 0x3FF];
      }
    }
    if (dbl == 2) std::memcpy(dst + outW_, dst, outW_ * sizeof(uint32_t));
  }
}

// ------------------------------------------------------------ registradores
uint8_t Pce::vdcRead(uint16_t off) {
  if (!sgx) return vdc[0].read(off);
  const int a = off & 0x1F;
  if (a < 8) return vdc[0].read(a);
  if (a >= 0x10 && a < 0x18) return vdc[1].read(a);
  switch (a) {
    case 0x08: return vpcPrio_[0];
    case 0x09: return vpcPrio_[1];
    case 0x0A: return static_cast<uint8_t>(vpcWin_[0]);
    case 0x0B: return static_cast<uint8_t>(vpcWin_[0] >> 8);
    case 0x0C: return static_cast<uint8_t>(vpcWin_[1]);
    case 0x0D: return static_cast<uint8_t>(vpcWin_[1] >> 8);
    case 0x0E: case 0x0F: return 0;
    default: return 0xFF;
  }
}

void Pce::vdcWrite(uint16_t off, uint8_t v) {
  if (!sgx) { vdc[0].write(off, v); return; }
  const int a = off & 0x1F;
  if (a < 8) { vdc[0].write(a, v); return; }
  if (a >= 0x10 && a < 0x18) { vdc[1].write(a, v); return; }
  switch (a) {
    case 0x08: vpcPrio_[0] = v; break;
    case 0x09: vpcPrio_[1] = v; break;
    case 0x0A: vpcWin_[0] = static_cast<uint16_t>((vpcWin_[0] & 0x300) | v); break;
    case 0x0B: vpcWin_[0] = static_cast<uint16_t>((vpcWin_[0] & 0xFF) | ((v & 3) << 8)); break;
    case 0x0C: vpcWin_[1] = static_cast<uint16_t>((vpcWin_[1] & 0x300) | v); break;
    case 0x0D: vpcWin_[1] = static_cast<uint16_t>((vpcWin_[1] & 0xFF) | ((v & 3) << 8)); break;
    case 0x0E: vpcSt2_ = v & 1; break;
    default: break;
  }
}

uint8_t Pce::vceRead(uint16_t off) {
  switch (off & 7) {
    case 4: return static_cast<uint8_t>(vce.pal[vce.addr]);
    case 5: {
      const uint8_t v = static_cast<uint8_t>(0xFE | ((vce.pal[vce.addr] >> 8) & 1));
      vce.addr = (vce.addr + 1) & 0x1FF;
      return v;
    }
    default: return 0xFF;
  }
}

void Pce::vceWrite(uint16_t off, uint8_t v) {
  switch (off & 7) {
    case 0:
      vce.ctrl = v;
      vce.lines = (v & 4) ? 263 : 262;
      vce.gray = (v & 0x80) != 0;
      vce.div = (v & 3) == 0 ? 4 : (v & 3) == 1 ? 3 : 2;
      break;
    case 2: vce.addr = static_cast<uint16_t>((vce.addr & 0x100) | v); break;
    case 3: vce.addr = static_cast<uint16_t>((vce.addr & 0xFF) | ((v & 1) << 8)); break;
    case 4: vce.pal[vce.addr] = static_cast<uint16_t>((vce.pal[vce.addr] & 0x100) | v); break;
    case 5:
      vce.pal[vce.addr] = static_cast<uint16_t>((vce.pal[vce.addr] & 0xFF) | ((v & 1) << 8));
      vce.addr = (vce.addr + 1) & 0x1FF;
      break;
    default: break;
  }
}

uint8_t Pce::padRead() const {
  if (padClr_) return sixButton_ ? 0x0F : 0x00;
  uint8_t r = 0x0F;
  const uint32_t k = keys_;
  if (sixButton_ && padExtra_) {
    if (padSel_) return 0x00;  // identificação do controle de 6 botões
    if (k & KEY_L) r &= ~1;
    if (k & KEY_Y) r &= ~2;
    if (k & KEY_X) r &= ~4;
    if (k & KEY_R) r &= ~8;
    return r;
  }
  if (padSel_) {
    if (k & KEY_UP) r &= ~1;
    if (k & KEY_RIGHT) r &= ~2;
    if (k & KEY_DOWN) r &= ~4;
    if (k & KEY_LEFT) r &= ~8;
  } else {
    if (k & KEY_A) r &= ~1;
    if (k & KEY_B) r &= ~2;
    if (k & KEY_SELECT) r &= ~4;
    if (k & KEY_START) r &= ~8;
  }
  return r;
}

void Pce::padWrite(uint8_t v) {
  const uint8_t clr = (v >> 1) & 1;
  if (sixButton_ && clr && !padClr_) padExtra_ ^= 1;
  padClr_ = clr;
  padSel_ = v & 1;
}

uint8_t Pce::slowRead(uint8_t bank, uint16_t off) {
  if (bank == 0xF7) return (!bramLocked_ && off < 0x800) ? bram_[off] : 0xFF;
  if (bank != 0xFF) return ext_ ? ext_->bankRead(bank, off) : 0xFF;
  if (off < 0x400) {
    cpu.penalty();  // a CPU espera um ciclo ao ler o VDC/VCE
    sync();
    const uint8_t v = vdcRead(off);
    updateIrq();
    schedule();
    return v;
  }
  if (off < 0x800) {
    cpu.penalty();
    sync();
    drawAll();
    return vceRead(off);
  }
  if (off < 0xC00) return ioBuf_;
  if (off < 0x1000) {
    sync();
    ioBuf_ = static_cast<uint8_t>((ioBuf_ & 0x80) | (timerRead() & 0x7F));
    return ioBuf_;
  }
  if (off < 0x1400) {
    // bit 7 = 0: interface da BRAM/CD presente; bit 6 = 0: TurboGrafx (os jogos americanos exigem)
    ioBuf_ = static_cast<uint8_t>(0x30 | padRead());
    return ioBuf_;
  }
  if (off < 0x1800) {
    sync();
    if ((off & 3) == 2) ioBuf_ = static_cast<uint8_t>((ioBuf_ & 0xF8) | (cpu.irqDisabled & 7));
    else if ((off & 3) == 3) ioBuf_ = static_cast<uint8_t>((ioBuf_ & 0xF8) | (cpu.irqActive & 7));
    return ioBuf_;
  }
  if (off < 0x1C00) {
    if ((off & 0x3FF) == 3) bramLocked_ = true;  // ler $1803 trava a BRAM
    if (ext_) {
      sync();
      const uint8_t v = ext_->ioRead(off);
      schedule();
      return v;
    }
    return 0x00;
  }
  return 0xFF;
}

void Pce::slowWrite(uint8_t bank, uint16_t off, uint8_t v) {
  if (bank == 0xF7) {
    if (!bramLocked_ && off < 0x800 && bram_[off] != v) { bram_[off] = v; bramDirty_ = true; }
    return;
  }
  if (bank == 0x00 && sf2_) {
    if ((off & 0x1FFC) == 0x1FF0) { sf2Bank_ = off & 3; mapBanks(); }
    return;
  }
  if (bank != 0xFF) {
    if (ext_) ext_->bankWrite(bank, off, v);
    return;
  }
  if (off < 0x400) {
    sync();
    vdcWrite(off, v);
    updateIrq();
    schedule();
    cpu.idle();  // e um ciclo de espera depois da escrita
  } else if (off < 0x800) {
    sync();
    drawAll();
    vceWrite(off, v);
    schedule();
    cpu.idle();
  } else if (off < 0xC00) {
    psgRun();
    psg_.write(off, v);
    ioBuf_ = v;
  } else if (off < 0x1000) {
    sync();
    if (off & 1) {
      const bool on = v & 1;
      if (on != timerOn_) { timerOn_ = on; timerNext_ = cpu.clk + 3072; timerCounter_ = timerReload_; }
    } else {
      timerReload_ = v & 0x7F;
    }
    schedule();
    ioBuf_ = v;
  } else if (off < 0x1400) {
    padWrite(v);
    ioBuf_ = v;
  } else if (off < 0x1800) {
    if ((off & 3) == 2) cpu.irqDisabled = v & 7;
    else if ((off & 3) == 3) cpu.irqActive &= static_cast<uint8_t>(~HuC6280::TIQ);
    ioBuf_ = v;
  } else if (off < 0x1C00) {
    if ((off & 0x3FF) == 7) bramLocked_ = (v & 0x80) == 0;  // $1807 bit 7 libera a BRAM
    if (ext_) {
      sync();
      ext_->ioWrite(off, v);
      schedule();
    }
  }
}

void Pce::stWrite(int port, uint8_t v) {
  sync();
  vdc[sgx && vpcSt2_ ? 1 : 0].write(port, v);
  updateIrq();
  schedule();
  cpu.idle();
}

// ------------------------------------------------------------ save state
void Pce::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  bool s = sgx;
  uint32_t romSize = static_cast<uint32_t>(rom_.size());
  io.sync(s); io.sync(romSize);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || s != sgx || romSize != rom_.size()))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  io.bytes(ram_); io.bytes(bram_); io.bytes(cardRam_);
  io.sync(sf2Bank_); io.sync(bramLocked_); io.sync(ioBuf_);
  io.sync(timerOn_); io.sync(timerReload_); io.sync(timerCounter_); io.sync(timerNext_);
  io.sync(padSel_); io.sync(padClr_); io.sync(padExtra_);
  io.raw(vpcPrio_, sizeof vpcPrio_); io.raw(vpcWin_, sizeof vpcWin_); io.sync(vpcSt2_);
  io.raw(vce.pal, sizeof vce.pal); io.sync(vce.addr); io.sync(vce.ctrl); io.sync(vce.div); io.sync(vce.lines); io.sync(vce.gray);
  vdc[0].sync(io);
  if (sgx) vdc[1].sync(io);
  psg_.sync(io);
  io.sync(psgClk_);
  io.raw(lines_, sizeof lines_); io.raw(lineDiv_, sizeof lineDiv_); io.raw(row2_, sizeof row2_);
  if (io.reading()) {
    clampTo<int64_t>(cpu.clk, 0, int64_t(1) << 60);
    sf2Bank_ &= 3; timerReload_ &= 0x7F; timerCounter_ &= 0x7F; padSel_ &= 1; padClr_ &= 1; padExtra_ &= 1;
    vpcWin_[0] &= 0x3FF; vpcWin_[1] &= 0x3FF; vpcSt2_ &= 1;
    for (auto& c : vce.pal) c &= 0x1FF;
    vce.addr &= 0x1FF;
    if (vce.div < 2 || vce.div > 4) vce.div = 4;
    if (vce.lines != 262 && vce.lines != 263) vce.lines = 262;
    if (timerOn_ && (timerNext_ < cpu.clk - 3072 || timerNext_ > cpu.clk + 3072)) timerNext_ = cpu.clk + 3072;
    if (psgClk_ > cpu.clk || psgClk_ < cpu.clk - (int64_t(1) << 24)) psgClk_ = cpu.clk;
    for (int i = 0; i < (sgx ? 2 : 1); i++) vdc[i].fixTime(cpu.clk);
    mapBanks();
    buildFrame();
    updateIrq();
    schedule();
  }
}

std::vector<uint8_t> Pce::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void Pce::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
}

}  // namespace indigo::pce
