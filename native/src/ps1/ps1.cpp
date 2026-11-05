#include "ps1.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "system.h"

namespace indigo::ps1 {

namespace {
const char STATE_MAGIC[4] = {'I', 'P', 'S', '1'};
constexpr uint32_t STATE_VERSION = 3;  // 3: DMA com chopping, FIFOs do MDEC, busca além do fim do disco
constexpr size_t CARD_SIZE = 128 * 1024;
constexpr size_t GME_HEADER = 3904;  // DexDrive (.gme)
const char BIOS_TEXT[] = "Sony Computer Entertainment";

// cartão novo já formatado: quadro 0 "MC", diretório vazio, lista de setores ruins vazia
std::vector<uint8_t> blankCard() {
  std::vector<uint8_t> c(CARD_SIZE, 0);
  auto frame = [&](int i) { return c.data() + i * 128; };
  auto checksum = [&](int i) {
    uint8_t x = 0;
    for (int k = 0; k < 127; k++) x ^= frame(i)[k];
    frame(i)[127] = x;
  };
  frame(0)[0] = 'M'; frame(0)[1] = 'C'; checksum(0);
  for (int i = 1; i < 16; i++) {
    frame(i)[0] = 0xA0;
    frame(i)[8] = 0xFF; frame(i)[9] = 0xFF;
    checksum(i);
  }
  for (int i = 16; i < 36; i++) {
    std::memset(frame(i), 0xFF, 4);
    frame(i)[8] = 0xFF; frame(i)[9] = 0xFF;
    checksum(i);
  }
  std::memcpy(frame(63), frame(0), 128);
  return c;
}
}  // namespace

bool isPs1Bios(const std::vector<uint8_t>& b) {
  if (b.size() != BIOS_SIZE) return false;
  const size_t n = sizeof BIOS_TEXT - 1;
  return std::search(b.begin(), b.end(), BIOS_TEXT, BIOS_TEXT + n) != b.end();
}

bool isPsxExe(const std::vector<uint8_t>& b) {
  return b.size() >= 0x800 && std::memcmp(b.data(), "PS-X EXE", 8) == 0;
}

namespace {
std::vector<uint8_t> pickBios(const std::vector<uint8_t>* bios) {
  const std::vector<uint8_t>* fwp = bios ? bios : &firmware("ps1");
  const std::vector<uint8_t>& fw = *fwp;
  if (fw.empty()) throw std::runtime_error("Falta a BIOS do PlayStation: abra o arquivo da BIOS (por exemplo SCPH1001.BIN) para instalar.");
  if (isPs1Bios(fw)) return fw;
  // os testes passam BIOS próprias (sem o texto da Sony): aceitas se tiverem o tamanho certo
  if (bios && fw.size() == BIOS_SIZE) return fw;
  throw std::runtime_error("O arquivo instalado não é uma BIOS do PlayStation.");
}
}  // namespace

Ps1Core::Ps1Core(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save, const std::vector<uint8_t>* bios)
    : disc_(std::move(disc)) {
  if (!disc_) throw std::runtime_error("Imagem de CD inválida.");
  sys_ = std::make_unique<System>(pickBios(bios));
  title_ = disc_->name();
  sys_->cdrom.setDisc(disc_.get());
  sys_->sio.card = blankCard();
  if (save) importSave(*save);
  start();
}

Ps1Core::Ps1Core(const std::vector<uint8_t>& exe, const std::string& name, const std::vector<uint8_t>* save,
                 ExeBoot boot, const std::vector<uint8_t>* bios)
    : title_(name), exe_(exe), boot_(boot) {
  if (!isPsxExe(exe)) throw std::runtime_error("O arquivo não é um programa PS-X EXE.");
  if (boot == ExeBoot::Bios) sys_ = std::make_unique<System>(pickBios(bios));
  else sys_ = std::make_unique<System>(bios ? *bios : std::vector<uint8_t>());
  const size_t dot = title_.find_last_of('.');
  if (dot != std::string::npos && dot > 0) title_.resize(dot);
  const size_t slash = title_.find_last_of("/\\");
  if (slash != std::string::npos) title_.erase(0, slash + 1);
  sys_->sio.card = blankCard();
  if (save) importSave(*save);
  start();
}

Ps1Core::~Ps1Core() = default;

void Ps1Core::start() {
  if (!exe_.empty()) {
    if (boot_ == ExeBoot::Direct) {
      sys_->installMiniKernel();
      sys_->loadExe(exe_);
    } else {
      sys_->sideload(exe_);
    }
  }
  updatePad();
}

void Ps1Core::reset() {
  std::vector<uint8_t> card = sys_->sio.card;
  sys_->reset();
  sys_->sio.card = std::move(card);
  audioL_.clear(); audioR_.clear(); audioPos_ = 0;
  start();
}

namespace {
// botões no formato do PS1 (1 = apertado)
uint16_t padBits(uint32_t k, bool l2, bool r2, bool l3, bool r3) {
  uint16_t b = 0;
  if (k & KEY_SELECT) b |= 1 << 0;
  if (l3) b |= 1 << 1;
  if (r3) b |= 1 << 2;
  if (k & KEY_START) b |= 1 << 3;
  if (k & KEY_UP) b |= 1 << 4;
  if (k & KEY_RIGHT) b |= 1 << 5;
  if (k & KEY_DOWN) b |= 1 << 6;
  if (k & KEY_LEFT) b |= 1 << 7;
  if (l2) b |= 1 << 8;
  if (r2) b |= 1 << 9;
  if (k & KEY_L) b |= 1 << 10;
  if (k & KEY_R) b |= 1 << 11;
  if (k & KEY_X) b |= 1 << 12;
  if (k & KEY_A) b |= 1 << 13;
  if (k & KEY_B) b |= 1 << 14;
  if (k & KEY_Y) b |= 1 << 15;
  return b;
}
}  // namespace

void Ps1Core::updatePad() { sys_->sio.pad[0].buttons = uint16_t(~padBits(keys_, l2_, r2_, l3_, r3_)); }

void Ps1Core::setKeys(uint32_t mask) { keys_ = mask; updatePad(); }
void Ps1Core::setKeys2(uint32_t mask) {
  sys_->sio.pad2On = true;
  sys_->sio.pad[1].buttons = uint16_t(~padBits(mask, false, false, false, false));
}
void Ps1Core::setExtraButtons(bool l2, bool r2, bool l3, bool r3) { l2_ = l2; r2_ = r2; l3_ = l3; r3_ = r3; updatePad(); }
void Ps1Core::setAnalog(uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry) {
  PadState& p = sys_->sio.pad[0];
  p.lx = lx; p.ly = ly; p.rx = rx; p.ry = ry;
}
void Ps1Core::pressAnalogButton() { sys_->sio.toggleAnalog(); }
bool Ps1Core::analogMode() const { return sys_->sio.analogMode; }
uint8_t Ps1Core::rumbleSmall() const { return sys_->sio.motorSmall; }
uint8_t Ps1Core::rumbleLarge() const { return sys_->sio.motorLarge; }
const std::string& Ps1Core::tty() const { return sys_->cpu.tty; }
void Ps1Core::insertDisc(std::unique_ptr<cdrom::Disc> disc) {
  sys_->cdrom.setDisc(disc.get());
  disc_ = std::move(disc);
}
void Ps1Core::setLidOpen(bool open) { sys_->cdrom.setLidOpen(open); }
bool Ps1Core::lidOpen() const { return sys_->cdrom.lidOpen(); }

void Ps1Core::runFrame() {
  sys_->runFrame();
  // som: a SPU entrega pares a 44,1 kHz; guarda no máximo 1 s
  int16_t buf[2048 * 2];
  while (size_t n = sys_->spu.take(buf, 2048)) {
    for (size_t i = 0; i < n; i++) {
      audioL_.push_back(buf[i * 2] / 32768.0f);
      audioR_.push_back(buf[i * 2 + 1] / 32768.0f);
    }
    while (audioL_.size() > 44100) { audioL_.pop_front(); audioR_.pop_front(); }
  }
}

const uint32_t* Ps1Core::framebuffer() const { return sys_->gpu.frame(); }
int Ps1Core::width() const { return sys_->gpu.frameWidth(); }
int Ps1Core::height() const { return sys_->gpu.frameHeight(); }
void Ps1Core::setResolution(int scale) { sys_->gpu.setScale(scale); }
int Ps1Core::resolution() const { return sys_->gpu.scale(); }
double Ps1Core::fps() const {
  return (sys_->gpu.status() >> 20) & 1 ? 33868800.0 / (314.0 * 3406.0 * 33868800.0 / 53203425.0)
                                        : 33868800.0 / (263.0 * 3413.0 * 33868800.0 / 53693175.0);
}

size_t Ps1Core::audioAvailable() const {
  if (audioL_.size() < 2) return 0;
  const double left = double(audioL_.size() - 1) - audioPos_;
  return left <= 0 ? 0 : size_t(left * rate_ / 44100.0);
}

void Ps1Core::pullAudio(float* l, float* r, size_t n) {
  const double step = 44100.0 / rate_;
  for (size_t i = 0; i < n; i++) {
    if (audioL_.size() < 2) { l[i] = r[i] = 0; continue; }
    const size_t k = size_t(audioPos_);
    const float f = float(audioPos_ - double(k));
    if (k + 1 >= audioL_.size()) { l[i] = audioL_.back(); r[i] = audioR_.back(); continue; }
    l[i] = audioL_[k] + (audioL_[k + 1] - audioL_[k]) * f;
    r[i] = audioR_[k] + (audioR_[k + 1] - audioR_[k]) * f;
    audioPos_ += step;
  }
  const size_t drop = std::min(size_t(audioPos_), audioL_.size() > 0 ? audioL_.size() - 1 : 0);
  audioL_.erase(audioL_.begin(), audioL_.begin() + long(drop));
  audioR_.erase(audioR_.begin(), audioR_.begin() + long(drop));
  audioPos_ -= double(drop);
}

bool Ps1Core::saveDirty() const { return sys_->sio.cardDirty; }

std::vector<uint8_t> Ps1Core::exportSave() {
  sys_->sio.cardDirty = false;
  return sys_->sio.card;
}

void Ps1Core::importSave(const std::vector<uint8_t>& b) {
  if (b.size() == CARD_SIZE) sys_->sio.card = b;
  else if (b.size() == CARD_SIZE + GME_HEADER) sys_->sio.card.assign(b.begin() + GME_HEADER, b.end());
  // outro tamanho: não é um cartão; fica o atual
}

std::string Ps1Core::note() const {
  if (!exe_.empty()) return boot_ == ExeBoot::Direct ? "Programa PS-X EXE (sem BIOS)" : "Programa PS-X EXE (carregado depois da BIOS)";
  return "";
}

void Ps1Core::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC)) throw std::runtime_error("Estado salvo incompatível com o PlayStation.");
  uint32_t ver = STATE_VERSION;
  io.sync(ver);
  if (io.reading() && ver != STATE_VERSION) throw std::runtime_error("Estado salvo de outra versão do núcleo do PlayStation.");
  sys_->sync(io);
}

std::vector<uint8_t> Ps1Core::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void Ps1Core::loadState(const std::vector<uint8_t>& s) {
  const std::vector<uint8_t> backup = saveState();
  try {
    StateIO io(s);
    sync(io);
  } catch (...) {
    StateIO io(backup);
    sync(io);
    throw;
  }
  updatePad();
}

std::unique_ptr<Ps1Core> createPs1Core(const std::string& path, const std::vector<uint8_t>* save, std::string* err) {
  std::string e;
  auto d = cdrom::open(path, &e);
  if (!d) { if (err) *err = "Não foi possível abrir o CD: " + e; return nullptr; }
  try {
    return std::make_unique<Ps1Core>(std::move(d), save);
  } catch (const std::exception& ex) {
    if (err) *err = ex.what();
    return nullptr;
  }
}

}  // namespace indigo::ps1
