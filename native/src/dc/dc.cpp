// Sega Dreamcast: o console inteiro (agendamento por linha de vídeo, eventos com hora marcada), reconhecimento
// da BIOS, da flash e do disco, save (VMU + flash), save state e a interface Core.
#include <algorithm>
#include <chrono>

#include "dc.h"

namespace indigo::dc {

namespace {
std::string lowerStr(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}
bool contains(const std::vector<uint8_t>& b, const char* s, size_t from, size_t to) {
  const size_t n = std::strlen(s);
  to = std::min(to, b.size());
  for (size_t i = from; i + n <= to; i++)
    if (std::memcmp(&b[i], s, n) == 0) return true;
  return false;
}
constexpr uint32_t STATE_MAGIC = 0x44434332;  // "DCC2"
}  // namespace

bool isDcBoot(const std::vector<uint8_t>& b, const std::string& name) {
  if (b.size() != BOOT_SIZE) return false;
  if (std::memcmp(&b[0x100], "SEGA", 4) == 0) return false;  // cartucho de Mega Drive de 2 MB
  const std::string n = lowerStr(name);
  return n.find("dc_boot") != std::string::npos || n.find("dc_bios") != std::string::npos ||
         n.find("dcboot") != std::string::npos || n.find("dc-boot") != std::string::npos ||
         n.find("dreamcast") != std::string::npos || n.substr(n.find_last_of("/\\") + 1) == "boot.bin";
}

bool isDcFlash(const std::vector<uint8_t>& b, const std::string& name) {
  if (b.size() != FLASH_SIZE) return false;
  if (contains(b, "KATANA_FLASH", 0, b.size())) return true;
  const std::string n = lowerStr(name);
  return n.find("dc_flash") != std::string::npos || n.find("dc_nvmem") != std::string::npos;
}

bool isDcDisc(cdrom::Disc& d) {
  uint8_t buf[cdrom::DATA_SECTOR];
  for (const auto& t : d.tracks())
    if (t.type != cdrom::TrackType::Audio && d.readData(t.start, buf) && std::memcmp(buf, "SEGA SEGAKATANA ", 16) == 0)
      return true;
  return false;
}

// ================================================================================================ System
System::System(const std::vector<uint8_t>& b, const std::vector<uint8_t>& f, cdrom::Disc* disc)
    : ram(RAM_SIZE), vram(VRAM_SIZE), aram(ARAM_SIZE), boot(b), flash(f) {
  boot.resize(BOOT_SIZE, 0xFF);
  flash.resize(FLASH_SIZE, 0xFF);
  gdrom.disc = disc;
  buildMap();
  reset();
}
System::~System() = default;

void System::reset() {
  std::fill(ram.begin(), ram.end(), 0);
  std::fill(vram.begin(), vram.end(), 0);
  std::fill(aram.begin(), aram.end(), 0);
  std::fill(std::begin(sb), std::end(sb), 0u);
  sb[(0x689C - 0x6800) / 4] = 0x0B;
  istnrm = istext = isterr = 0;
  for (auto& e : evAt) e = INT64_MAX;
  cpu.cycles = 0;
  cpu.reset();
  pvr.reset();
  aica.reset();
  gdrom.reset();
  line = 0; frames = 0; frameStart = 0;
  flashState_ = 0; audioFrac_ = 0; g2Pending_ = 0;
  // relógio do AICA: segundos desde 1950-01-01 (a hora do aparelho, só na partida)
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  aica.rtc = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(now).count() + 631152000LL);
  std::fill(std::begin(fb), std::end(fb), 0xFF000000u);
  updateIrq();
}

void System::loadProgram(const std::vector<uint8_t>& bin, uint32_t addr, uint32_t entry) {
  const uint32_t off = addr & 0xFFFFFF;
  const size_t n = std::min(bin.size(), size_t(RAM_SIZE - std::min(off, RAM_SIZE)));
  std::copy(bin.begin(), bin.begin() + long(n), ram.begin() + off);
  cpu.pc = entry;
  cpu.r[15] = 0x8D000000;
  cpu.vbr = 0x8C000000;
  cpu.setSr(0x400000F0);  // privilegiado, interrupções mascaradas, banco 0
  cpu.setFpscr(0x00040001);
}

void System::runLine() {
  const uint32_t load = pvr.regs[0xD8 / 4];
  const bool inter = pvr.regs[0xD0 / 4] & 0x10;
  lines = int(((load >> 16) & 0x3FF) + 1);
  if (inter) lines /= 2;
  lines = std::clamp(lines, 200, 700);
  const int64_t cpl = int64_t(SH4_HZ / 59.94 / lines);
  const int64_t end = frameStart + cpl * (line + 1);
  for (int guard = 0; cpu.cycles < end && guard < 100000; guard++) {
    int64_t next = end;
    for (int64_t t : evAt) next = std::min(next, t);
    next = std::min(next, cpu.tmuNextEvent());
    if (next > cpu.cycles) cpu.run(next);
    if (cpu.sleeping && cpu.cycles < next) cpu.cycles = next;
    for (int e = 0; e < EV_COUNT; e++)
      if (evAt[e] <= cpu.cycles) doEvent(e);
    if (cpu.tmuNextEvent() <= cpu.cycles) cpu.tmuSync();
  }
  if (cpu.cycles < end) cpu.cycles = end;  // laço de segurança: não deixa o quadro travar
  cpu.tmuSync();
  // eventos de vídeo desta linha
  const uint32_t vbi = pvr.regs[0xCC / 4];
  if (uint32_t(line) == (vbi & 0x3FF)) {
    raiseNrm(3);
    pvr.drawOutput(fb, fbW, fbH);
    if ((sb[(0x6C10 - 0x6800) / 4] & 1) && (sb[(0x6C14 - 0x6800) / 4] & 1)) {
      runDma([&] { maple.dma(sb[(0x6C04 - 0x6800) / 4] & 0x1FFFFFE0); schedule(EV_MAPLE, 20000); });
    }
  }
  if (uint32_t(line) == ((vbi >> 16) & 0x3FF)) raiseNrm(4);
  const uint32_t hbi = pvr.regs[0xC8 / 4];
  const uint32_t hmode = (hbi >> 12) & 3, comp = hbi & 0x3FF;
  if ((hmode == 0 && uint32_t(line) == comp) || (hmode == 1 && comp && line % int(comp) == 0) || hmode == 2) raiseNrm(5);
  // som: amostras desta linha (44,1 kHz) e o ARM7 (64 ciclos por amostra)
  audioFrac_ += 4410000;
  const int per = lines * 5994;
  while (audioFrac_ >= per) {
    audioFrac_ -= per;
    aica.runArm(64);
    aica.sample();
  }
}

void System::runFrame() {
  frameStart = cpu.cycles;
  for (line = 0; line < lines; line++) runLine();
  line = 0;
  frames++;
}

void System::sync(StateIO& io) {
  io.bytes(ram); io.bytes(vram); io.bytes(aram); io.bytes(flash);
  cpu.sync(io);
  pvr.sync(io);
  aica.sync(io);
  gdrom.sync(io);
  maple.sync(io);
  io.raw(sb, sizeof sb);
  io.sync(istnrm); io.sync(istext); io.sync(isterr);
  io.sync(line); io.sync(lines); io.sync(frames); io.sync(frameStart); io.sync(flashDirty);
  io.raw(evAt, sizeof evAt);
  io.sync(flashState_); io.sync(audioFrac_); io.sync(g2Pending_);
  if (io.reading()) {
    // relógio longe do fim do int64 (somas de atraso não estouram)
    constexpr int64_t MAXC = int64_t(1) << 60;
    if (cpu.cycles < 0 || cpu.cycles > MAXC || frameStart < 0 || frameStart > MAXC)
      throw std::runtime_error("Estado salvo inválido (relógio).");
    clampTo(lines, 200, 700); clampTo(line, 0, 699); clampTo(flashState_, 0, 6);
    clampTo(audioFrac_, 0, 700 * 5994);
    for (auto& e : evAt) if (e < 0) e = 0;
    updateIrq();
  }
}

// ================================================================================================ DcCore
DcCore::DcCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* boot,
               const std::vector<uint8_t>* flash)
    : disc_(std::move(disc)) {
  boot_ = boot ? *boot : firmware("dc_boot");
  flash_ = flash ? *flash : firmware("dc_flash");
  if (boot_.size() != BOOT_SIZE)
    throw std::runtime_error("o Dreamcast precisa da BIOS do console. Abra o arquivo da BIOS (por exemplo dc_boot.bin) uma vez no Índigo.");
  if (flash_.size() != FLASH_SIZE) flash_.assign(FLASH_SIZE, 0xFF);
  sys_ = std::make_unique<System>(boot_, flash_, disc_.get());
  // nome do jogo: campo de 128 bytes do IP.BIN (0x80)
  title_ = disc_ ? disc_->name() : "Dreamcast";
  if (disc_) {
    uint8_t buf[cdrom::DATA_SECTOR];
    for (const auto& t : disc_->tracks())
      if (t.type != cdrom::TrackType::Audio && disc_->readData(t.start, buf) && std::memcmp(buf, "SEGA SEGAKATANA ", 16) == 0) {
        std::string n(reinterpret_cast<char*>(buf + 0x80), 128);
        while (!n.empty() && (n.back() == ' ' || n.back() == '\0')) n.pop_back();
        bool ok = !n.empty();
        for (char c : n) if (uint8_t(c) < 0x20 || uint8_t(c) > 0x7E) ok = false;
        if (ok) title_ = n;
      }
  }
  if (save) importSave(*save);
}

void DcCore::runFrame() {
  sys_->runFrame();
  // som que ninguém puxou: guarda no máximo 1 s (o mais recente)
  auto& a = sys_->aica.out;
  constexpr size_t MAXS = 44100 * 2;
  if (a.size() > MAXS) {
    const size_t drop = (a.size() - MAXS) & ~size_t(1);
    a.erase(a.begin(), a.begin() + long(drop));
    audioPos_ = std::max(0.0, audioPos_ - double(drop / 2));
  }
}

void DcCore::reset() { sys_->reset(); }  // o VMU e a flash ficam

void DcCore::pullAudio(float* l, float* r, size_t n) {
  auto& a = sys_->aica.out;
  const size_t frames = a.size() / 2;
  const double step = 44100.0 / rate_;
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

size_t DcCore::audioAvailable() const {
  const double frames = double(sys_->aica.out.size() / 2);
  const double n = (frames - 1 - audioPos_) * rate_ / 44100.0;
  return n > 0 ? size_t(n) : 0;
}

std::vector<uint8_t> DcCore::exportSave() {
  sys_->maple.vmuDirty = false;
  sys_->flashDirty = false;
  std::vector<uint8_t> s = sys_->maple.vmu;
  s.insert(s.end(), sys_->flash.begin(), sys_->flash.end());
  return s;
}

void DcCore::importSave(const std::vector<uint8_t>& b) {
  if (b.size() >= VMU_SIZE) std::copy(b.begin(), b.begin() + VMU_SIZE, sys_->maple.vmu.begin());
  if (b.size() >= VMU_SIZE + FLASH_SIZE) std::copy(b.begin() + VMU_SIZE, b.begin() + VMU_SIZE + FLASH_SIZE, sys_->flash.begin());
  sys_->maple.vmuDirty = false;
  sys_->flashDirty = false;
}

std::vector<uint8_t> DcCore::saveState() {
  StateIO io;
  uint32_t magic = STATE_MAGIC;
  io.sync(magic);
  sys_->sync(io);
  return io.data();
}

void DcCore::loadState(const std::vector<uint8_t>& s) {
  StateIO probe(s);
  uint32_t magic = 0;
  probe.sync(magic);
  if (magic != STATE_MAGIC) throw std::runtime_error("Estado salvo não é deste console.");
  const std::vector<uint8_t> before = saveState();
  try {
    sys_->sync(probe);
    if (!probe.atEnd()) throw std::runtime_error("Estado salvo com bytes sobrando.");
  } catch (...) {
    StateIO back(before);
    uint32_t m = 0;
    back.sync(m);
    sys_->sync(back);
    throw;
  }
  audioPos_ -= double(size_t(audioPos_));
}

std::string DcCore::note() const {
  return "Dreamcast: ainda sem som; sem MMU, neblina nem DSP de som; volumes modificadores só como sombra simples; a velocidade depende muito do aparelho.";
}

}  // namespace indigo::dc
