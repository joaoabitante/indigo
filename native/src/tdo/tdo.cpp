// Panasonic 3DO: barramento, CLIO e o núcleo (ver tdo.h).
#include "tdo.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::tdo {

namespace {
constexpr uint32_t ROM_SIZE = 0x100000;
constexpr uint32_t NVRAM_SIZE = 0x8000;
constexpr uint32_t DMA_XBUS = 1u << 20, DMA_PBUS = 1u << 31;
constexpr uint32_t IRQ_VINT0 = 1, IRQ_VINT1 = 2, IRQ_XBUS = 4, IRQ_DSP = 1u << 11, IRQ_XBUS_DMA = 1u << 29;

bool contains(const std::vector<uint8_t>& b, const char* s) {
  const size_t n = std::strlen(s);
  return std::search(b.begin(), b.end(), s, s + n) != b.end();
}
}  // namespace

bool isTdoBios(const std::vector<uint8_t>& b) {
  if (b.size() != ROM_SIZE) return false;  // as BIOS conhecidas (FZ-1, FZ-10, GoldStar, Sanyo) têm 1 MB
  if (b[4] == 0x24 && b[5] == 0xFF && b[6] == 0xAE && b[7] == 0x51) return false;  // logotipo de ROM de GBA
  if ((b[0] >> 4) != 0xE) return false;  // primeira instrução do ARM com condição "sempre" (big-endian)
  if (b.size() > 6 && b[0] == 1 && b[1] == 0x5A) return false;
  return contains(b, "3DO") || contains(b, "3do") || contains(b, "Opera") || contains(b, "OPERA");
}

bool isOperaDisc(cdrom::Disc& d) {
  int first = -1;
  for (size_t i = 0; i < d.tracks().size(); i++)
    if (d.tracks()[i].type != cdrom::TrackType::Audio) { first = int(i); break; }
  if (first < 0) return false;
  uint8_t buf[cdrom::DATA_SECTOR];
  if (!d.readData(d.tracks()[size_t(first)].start, buf)) return false;
  return buf[0] == 1 && buf[1] == 0x5A && buf[2] == 0x5A && buf[3] == 0x5A && buf[4] == 0x5A && buf[5] == 0x5A &&
         buf[6] == 1 && std::memcmp(buf + 40, "CD-ROM", 6) == 0;
}

// ------------------------------------------------------------------------------------------------ sistema

System::System(const std::vector<uint8_t>& b, cdrom::Disc* disc) : mem(MEM_SIZE), bios(ROM_SIZE), nvram(NVRAM_SIZE) {
  std::copy(b.begin(), b.begin() + std::min<size_t>(b.size(), ROM_SIZE), bios.begin());
  cd.setDisc(disc);
  dsp.readFifo = [this](int ch) { return dspFifo(ch); };
  dsp.irq = [this]() { raise0(IRQ_DSP); };
  reset();
}

void System::reset() {
  std::fill(mem.begin(), mem.end(), 0);
  madam.reset();
  dsp.reset();
  std::memset(dsp.code, 0, sizeof dsp.code);
  cd.reset();
  cpu.reset(0x03000000);
  pend0_ = pend1_ = mask0_ = mask1_ = 0;
  vint0_ = vint1_ = 0x3FF;
  for (int i = 0; i < 16; i++) timers_[i] = reload_[i] = 0;
  timerCtl_[0] = timerCtl_[1] = 0;
  slack_ = timerFrac_ = 0;
  dmaEnable_ = 0;
  xbusSel_ = xbusPollEn_ = expctl_ = 0;
  semaphore_ = 0;
  seed_ = 0x13579BDF;
  std::memset(clioOther_, 0, sizeof clioOther_);
  line_ = field_ = 0;
  budget_ = stall_ = 0;
  audioAcc_ = 0;
  audio.clear();
  std::memset(fb, 0, sizeof fb);
}

void System::updateFiq() {
  pend0_ = pend1_ ? (pend0_ | 0x80000000u) : (pend0_ & 0x7FFFFFFFu);
  const bool on = (pend0_ & mask0_ & 0x7FFFFFFFu) || (pend1_ & mask1_);
  cpu.setFiq(on);
}

uint32_t System::read32(uint32_t a) {
  if (a < MEM_SIZE) return madam.m32(a);
  if (a >= 0x03000000 && a < 0x03100000) {
    const uint32_t o = a & (ROM_SIZE - 4);
    return uint32_t(bios[o]) << 24 | uint32_t(bios[o + 1]) << 16 | uint32_t(bios[o + 2]) << 8 | bios[o + 3];
  }
  if (a >= 0x03140000 && a < 0x03160000) return nvram[(a >> 2) & (NVRAM_SIZE - 1)];
  if (a >= 0x03300000 && a < 0x03400000) return madam.read(a & 0xFFFFF);
  if (a >= 0x03400000 && a < 0x03500000) return clioRead(a & 0xFFFF);
  return 0;
}

uint8_t System::read8(uint32_t a) {
  if (a < MEM_SIZE) return mem[a];
  if (a >= 0x03000000 && a < 0x03100000) return bios[a & (ROM_SIZE - 1)];
  const uint32_t w = read32(a & ~3u);
  return uint8_t(w >> (24 - (a & 3) * 8));
}

void System::write32(uint32_t a, uint32_t v) {
  if (a < MEM_SIZE) {
    mem[a] = uint8_t(v >> 24); mem[a + 1] = uint8_t(v >> 16); mem[a + 2] = uint8_t(v >> 8); mem[a + 3] = uint8_t(v);
    return;
  }
  if (a >= 0x03140000 && a < 0x03160000) {
    uint8_t& n = nvram[(a >> 2) & (NVRAM_SIZE - 1)];
    if (n != uint8_t(v)) { n = uint8_t(v); nvramDirty = true; }
    return;
  }
  if (a >= 0x03200000 && a < 0x03300000) {
    const uint32_t off = a & 0x7FFFF;
    const uint32_t page = 0x200000 + (off & 0xFF800);
    if (a < 0x03280000) {
      for (uint32_t i = 0; i < 2048; i += 4) write32(page + i, v);
    } else {
      const uint32_t from = 0x200000 + (v & 0xFF800);
      std::memmove(&mem[page], &mem[from], 2048);
    }
    stall_ += 64;
    return;
  }
  if (a >= 0x03300000 && a < 0x03400000) { stall_ += madam.write(a & 0xFFFFF, v); return; }
  if (a >= 0x03400000 && a < 0x03500000) { clioWrite(a & 0xFFFF, v); return; }
}

void System::write8(uint32_t a, uint8_t v) {
  if (a < MEM_SIZE) { mem[a] = v; return; }
  if (a >= 0x03140000 && a < 0x03160000) { write32(a & ~3u, v); return; }
  if (a >= 0x03400000 && a < 0x03500000) { clioWrite(a & 0xFFFC, v); return; }
}

uint32_t System::clioRead(uint32_t off) {
  off &= 0xFFFC;
  if (off >= 0x0100 && off < 0x0180) {
    const int n = int((off - 0x100) >> 3);
    return (off & 4) ? reload_[n] : timers_[n];
  }
  if (off >= 0x1800 && off < 0x2000) return dsp.code[(off - 0x1800) >> 2];
  if (off >= 0x3000 && off < 0x3400) return dsp.data[(off - 0x3000) >> 2];
  if (off >= 0x3800 && off < 0x3840) return dsp.data[0x3E0 + ((off - 0x3800) >> 2)];
  if (off >= 0x0540 && off < 0x0580) {
    uint32_t s = xbusPollEn_;
    if (xbusSel_ == 0) {
      if (cd.statusReady()) s |= 0x10;
      if (cd.dataReady()) s |= 0x20;
      if (cd.canCommand()) s |= 0x40;
    }
    return s;
  }
  if (off >= 0x0580 && off < 0x05C0) return xbusSel_ == 0 ? cd.readStatus() : 0;
  if (off >= 0x05C0 && off < 0x0600) return xbusSel_ == 0 ? cd.readData() : 0;
  if (off >= 0x0500 && off < 0x0540) return xbusSel_;
  switch (off) {
    case 0x0000: return 0x02020000;
    case 0x0008: return vint0_;
    case 0x000C: return vint1_;
    case 0x0024:
      seed_ ^= seed_ << 13; seed_ ^= seed_ >> 17; seed_ ^= seed_ << 5;
      return seed_;
    case 0x0030: return uint32_t(std::clamp(CYCLES_PER_LINE - budget_, 0, CYCLES_PER_LINE - 1));
    case 0x0034: return uint32_t(line_) | uint32_t(field_) << 11;
    case 0x0040: case 0x0044: return pend0_;
    case 0x0048: case 0x004C: return mask0_;
    case 0x0060: case 0x0064: return pend1_;
    case 0x0068: case 0x006C: return mask1_;
    case 0x0200: case 0x0204: return timerCtl_[0];
    case 0x0208: case 0x020C: return timerCtl_[1];
    case 0x0220: return slack_;
    case 0x0304: case 0x0308: return dmaEnable_;
    case 0x0400: case 0x0404: return expctl_;
    case 0x17D0: return semaphore_;
    case 0x17E8: return dsp.running ? 1 : 0;
    default: break;
  }
  if (off < 0x0100) return clioOther_[off >> 2];
  return 0;
}

void System::clioWrite(uint32_t off, uint32_t v) {
  off &= 0xFFFC;
  if (off >= 0x0100 && off < 0x0180) {
    const int n = int((off - 0x100) >> 3);
    if (off & 4) reload_[n] = uint16_t(v);
    else timers_[n] = uint16_t(v);
    return;
  }
  if (off >= 0x1800 && off < 0x2000) { dsp.code[(off - 0x1800) >> 2] = uint16_t(v); return; }
  if (off >= 0x3000 && off < 0x3400) { dsp.data[(off - 0x3000) >> 2] = uint16_t(v); return; }
  if (off >= 0x0500 && off < 0x0540) { xbusSel_ = v & 0xFF; return; }
  if (off >= 0x0540 && off < 0x0580) { xbusPollEn_ = v & 3; return; }
  if (off >= 0x0580 && off < 0x05C0) { if (xbusSel_ == 0) cd.writeCommand(uint8_t(v)); return; }
  switch (off) {
    case 0x0008: vint0_ = v & 0x7FF; return;
    case 0x000C: vint1_ = v & 0x7FF; return;
    case 0x0020: seed_ = v ? v : 1; return;
    case 0x0040: pend0_ |= v & 0x7FFFFFFFu; updateFiq(); return;
    case 0x0044: pend0_ &= ~v; updateFiq(); return;
    case 0x0048: mask0_ |= v; updateFiq(); return;
    case 0x004C: mask0_ &= ~v; updateFiq(); return;
    case 0x0060: pend1_ |= v; updateFiq(); return;
    case 0x0064: pend1_ &= ~v; updateFiq(); return;
    case 0x0068: mask1_ |= v; updateFiq(); return;
    case 0x006C: mask1_ &= ~v; updateFiq(); return;
    case 0x0200: timerCtl_[0] |= v; return;
    case 0x0204: timerCtl_[0] &= ~v; return;
    case 0x0208: timerCtl_[1] |= v; return;
    case 0x020C: timerCtl_[1] &= ~v; return;
    case 0x0220: slack_ = v & 0x3FF; return;
    case 0x0304: dmaEnable_ |= v; return;
    case 0x0308: dmaEnable_ &= ~v; return;
    case 0x0400: expctl_ |= v; return;
    case 0x0404: expctl_ &= ~v; return;
    case 0x17D0: semaphore_ = v; return;
    case 0x17E8: dsp.running = v & 1; return;
    case 0x17F0: {
      uint16_t code[Dspp::CODE_WORDS];
      std::memcpy(code, dsp.code, sizeof code);
      dsp.reset();
      std::memcpy(dsp.code, code, sizeof code);
      return;
    }
    default: break;
  }
  if (off < 0x0100) clioOther_[off >> 2] = v;
}

void System::timersTick(int cycles) {
  const uint32_t period = 64 + slack_;
  timerFrac_ += uint32_t(cycles);
  while (timerFrac_ >= period) {
    timerFrac_ -= period;
    bool prevUnder = false;
    for (int t = 0; t < 16; t++) {
      const uint32_t ctl = (timerCtl_[t >> 3] >> ((t & 7) * 4)) & 0xF;
      bool under = false;
      if (ctl & 1) {
        const bool step = (ctl & 4) ? prevUnder : true;
        if (step) {
          if (timers_[t] == 0) {
            under = true;
            timers_[t] = (ctl & 2) ? reload_[t] : 0xFFFF;
          } else {
            timers_[t]--;
          }
        }
      }
      if (under && (t & 1)) pend0_ |= 1u << (10 - (t >> 1));
      prevUnder = under;
    }
  }
  updateFiq();
}

uint16_t System::dspFifo(int ch) {
  if (ch < 0 || ch > 12 || !(dmaEnable_ & (1u << ch))) return 0;
  uint32_t* r = &madam.regs[(0x400 >> 2) + ch * 4];
  if (r[1] < 2) return 0;
  const uint16_t v = madam.m16(r[0]);
  r[0] += 2;
  r[1] -= 2;
  if (r[1] < 2) {
    if (r[3] >= 2) { r[0] = r[2]; r[1] = r[3]; r[3] = 0; }
    else dmaEnable_ &= ~(1u << ch);
    pend0_ |= 1u << (16 + ch);
    updateFiq();
  }
  return v;
}

void System::xbusDma() {
  if (!(dmaEnable_ & DMA_XBUS)) return;
  uint32_t& addr = madam.regs[0x540 >> 2];
  uint32_t& count = madam.regs[0x544 >> 2];
  int moved = 0;
  while (count && cd.dataReady() && moved < 4096) {
    const uint8_t b = cd.readData();
    if (addr < MEM_SIZE) mem[addr] = b;
    addr++;
    count--;
    moved++;
  }
  if (!count) {
    dmaEnable_ &= ~DMA_XBUS;
    raise0(IRQ_XBUS_DMA);
  }
}

void System::pbusDma() {
  if (!(dmaEnable_ & DMA_PBUS)) return;
  uint32_t addr = madam.regs[0x570 >> 2];
  const uint32_t len = std::min<uint32_t>(madam.regs[0x574 >> 2], 64);
  for (uint32_t i = 0; i < len; i++, addr++) {
    uint8_t b = 0;
    if (i == 0) b = uint8_t(pad_ >> 8);
    else if (i == 1) b = uint8_t(pad_);
    if (addr < MEM_SIZE) mem[addr] = b;
  }
  dmaEnable_ &= ~DMA_PBUS;
  raise1(1);
}

void System::tickHardware(int cycles) {
  timersTick(cycles);
  cd.tick(cycles);
  xbusDma();
  // XBUS: interrupção enquanto a unidade tiver resposta ou dado e a interrupção estiver ligada
  if (xbusSel_ == 0 && (((xbusPollEn_ & 1) && cd.statusReady()) || ((xbusPollEn_ & 2) && cd.dataReady()))) pend0_ |= IRQ_XBUS;
  audioAcc_ += uint64_t(cycles) * AUDIO_HZ;
  while (audioAcc_ >= uint64_t(CPU_HZ)) {
    audioAcc_ -= uint64_t(CPU_HZ);
    dsp.runSample();
    if (audio.size() < 44100 * 4) { audio.push_back(dsp.outL); audio.push_back(dsp.outR); }
  }
  updateFiq();
}

void System::runLine() {
  if (uint32_t(line_) == vint0_) raise0(IRQ_VINT0);
  if (uint32_t(line_) == vint1_) raise0(IRQ_VINT1);
  budget_ += CYCLES_PER_LINE;
  while (budget_ > 0) {
    if (stall_ > 0) {
      const int s = std::min<int32_t>(stall_, budget_);
      stall_ -= s;
      budget_ -= s;
      continue;
    }
    budget_ -= cpu.step();
  }
  tickHardware(CYCLES_PER_LINE);
  if (line_ == VISIBLE) {
    madam.renderFrame(fb);
    pbusDma();
  }
  if (++line_ >= LINES) { line_ = 0; field_ ^= 1; }
}

void System::runFrame() {
  for (int i = 0; i < LINES; i++) runLine();
  frames++;
}

void System::sync(StateIO& io) {
  cpu.sync(io);
  io.raw(mem.data(), mem.size());
  io.raw(nvram.data(), nvram.size());
  madam.sync(io);
  dsp.sync(io);
  cd.sync(io);
  io.sync(pend0_); io.sync(pend1_); io.sync(mask0_); io.sync(mask1_);
  io.sync(vint0_); io.sync(vint1_);
  io.raw(timers_, sizeof timers_);
  io.raw(reload_, sizeof reload_);
  io.raw(timerCtl_, sizeof timerCtl_);
  io.sync(slack_); io.sync(timerFrac_);
  io.sync(dmaEnable_);
  io.sync(xbusSel_); io.sync(xbusPollEn_); io.sync(expctl_);
  io.sync(semaphore_); io.sync(seed_);
  io.raw(clioOther_, sizeof clioOther_);
  io.sync(line_); io.sync(field_); io.sync(budget_); io.sync(stall_);
  io.sync(audioAcc_);
  io.sync(pad_);
  io.sync(frames);
  if (io.reading()) {
    slack_ &= 0x3FF;
    if (timerFrac_ >= 64 + slack_) timerFrac_ = 0;
    xbusPollEn_ &= 3;
    if (!seed_) seed_ = 1;
    if (line_ < 0 || line_ >= LINES) line_ = 0;
    field_ &= 1;
    clampTo(budget_, int32_t(-1000), int32_t(CYCLES_PER_LINE));
    clampTo(stall_, int32_t(0), int32_t(1) << 26);
    if (audioAcc_ >= uint64_t(CPU_HZ)) audioAcc_ = 0;
    audio.clear();
    updateFiq();
  }
}

// ------------------------------------------------------------------------------------------------ núcleo

namespace {
std::string volumeName(cdrom::Disc& d) {
  uint8_t buf[cdrom::DATA_SECTOR];
  int first = -1;
  for (size_t i = 0; i < d.tracks().size(); i++)
    if (d.tracks()[i].type != cdrom::TrackType::Audio) { first = int(i); break; }
  if (first >= 0 && d.readData(d.tracks()[size_t(first)].start, buf)) {
    std::string s(reinterpret_cast<const char*>(buf + 8), 32);
    s = s.substr(0, s.find('\0'));
    bool ok = !s.empty();
    for (char c : s) if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) ok = false;
    if (ok) return s;
  }
  return d.name();
}
}  // namespace

TdoCore::TdoCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios)
    : disc_(std::move(disc)) {
  const std::vector<uint8_t>& b = bios ? *bios : firmware("3do");
  if (b.empty()) throw std::runtime_error("o 3DO precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo panafz10.bin) uma vez no Índigo.");
  sys_ = std::make_unique<System>(b, disc_.get());
  if (save) importSave(*save);
  title_ = disc_ ? volumeName(*disc_) : "3DO";
  note_ = contains(b, "Indigo 3DO") ? " · BIOS de teste do Índigo" : " · 3DO experimental: se algo der errado, avise";
}

void TdoCore::setKeys(uint32_t k) {
  uint16_t p = 0x8000;  // identificação do controle
  if (k & KEY_DOWN) p |= 0x1000;
  if (k & KEY_UP) p |= 0x0800;
  if (k & KEY_RIGHT) p |= 0x0400;
  if (k & KEY_LEFT) p |= 0x0200;
  // A, B e C seguem a fileira do controle do portátil: botão da esquerda (Y), de baixo (A) e da direita (B)
  if (k & KEY_Y) p |= 0x0100;      // A
  if (k & KEY_A) p |= 0x0080;      // B
  if (k & KEY_B) p |= 0x0040;      // C
  if (k & KEY_START) p |= 0x0020;  // P (play)
  if (k & KEY_SELECT) p |= 0x0010; // X (stop)
  if (k & KEY_R) p |= 0x0008;
  if (k & KEY_L) p |= 0x0004;
  sys_->setPad(p);
}

size_t TdoCore::audioAvailable() const {
  const double frames = double(sys_->audio.size() / 2);
  const double n = (frames - 1 - audioPos_) * rate_ / System::AUDIO_HZ;
  return n > 0 ? size_t(n) : 0;
}

void TdoCore::pullAudio(float* l, float* r, size_t n) {
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

void TdoCore::importSave(const std::vector<uint8_t>& b) {
  std::fill(sys_->nvram.begin(), sys_->nvram.end(), 0);
  std::copy(b.begin(), b.begin() + std::min(b.size(), sys_->nvram.size()), sys_->nvram.begin());
  sys_->nvramDirty = false;
}

std::vector<uint8_t> TdoCore::saveState() {
  StateIO io;
  uint32_t magic = 0x33444F31;  // "3DO1"
  io.sync(magic);
  sys_->sync(io);
  return io.data();
}

void TdoCore::loadState(const std::vector<uint8_t>& s) {
  StateIO probe(s);
  uint32_t magic = 0;
  probe.sync(magic);
  if (magic != 0x33444F31) throw std::runtime_error("Estado salvo não é deste console.");
  // primeiro numa cópia: um estado truncado não pode deixar o jogo pela metade
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
  audioPos_ -= double(size_t(audioPos_));  // a fila de som recomeça vazia (só a fração do reamostrador fica)
}

std::unique_ptr<TdoCore> createTdoCore(const std::string& path, const std::vector<uint8_t>* save, std::string* err) {
  std::string e;
  auto disc = cdrom::open(path, &e);
  if (!disc) { if (err) *err = e; return nullptr; }
  try {
    return std::make_unique<TdoCore>(std::move(disc), save);
  } catch (const std::exception& ex) {
    if (err) *err = ex.what();
    return nullptr;
  }
}

}  // namespace indigo::tdo
