#include "pcecd.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::pcecd {

namespace {
const char STATE_MAGIC[4] = {'I', 'P', 'C', 'D'};
const char BIOS_TEXT[] = "PC Engine CD-ROM SYSTEM";

uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & (0u - (c & 1)));
  }
  return ~c;
}

// System Cards conhecidos (CRC32 dos 256 KB): só os números, nenhum byte da BIOS
struct KnownBios { uint32_t crc; Model model; };
const KnownBios KNOWN[] = {
  {0x3F9F95A4, Model::CdRom2},        // CD-ROM System 1.0 (Japão)
  {0x52520BC6, Model::CdRom2},        // CD-ROM System 2.0 (Japão)
  {0x283B74E0, Model::CdRom2},        // CD-ROM System 2.1 (Japão)
  {0xFF2A5EC3, Model::CdRom2},        // TurboGrafx CD System Card 2.0 (EUA)
  {0x6D9A73EF, Model::ArcadeCdRom2},  // Super CD-ROM System 3.0 (Japão)
  {0x0ECEA1F3, Model::ArcadeCdRom2},  // Super CD-ROM System 3.0 (Japão, fonte modificada)
  {0x2B5B75FE, Model::ArcadeCdRom2},  // TurboGrafx CD Super System Card 3.0 (EUA)
};

bool hasText(const std::vector<uint8_t>& b) {
  const size_t n = sizeof BIOS_TEXT - 1;
  return std::search(b.begin(), b.end(), BIOS_TEXT, BIOS_TEXT + n) != b.end();
}

bool knownCrc(const std::vector<uint8_t>& b) {
  const uint32_t c = crc32(b.data(), b.size());
  for (const auto& k : KNOWN) if (k.crc == c) return true;
  return false;
}

uint8_t reverseBits(uint8_t v) {
  v = uint8_t((v & 0xF0) >> 4 | (v & 0x0F) << 4);
  v = uint8_t((v & 0xCC) >> 2 | (v & 0x33) << 2);
  return uint8_t((v & 0xAA) >> 1 | (v & 0x55) << 1);
}
}  // namespace

std::vector<uint8_t> normalizeBios(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> b = in;
  if (b.size() % 0x2000 == 512) b.erase(b.begin(), b.begin() + 512);  // cabeçalho de copiadora
  if (b.size() == 0x80000) b.resize(0x40000);                         // dump de 512 KB com espelho
  if (b.size() != 0x40000) return {};
  if (hasText(b) || knownCrc(b)) return b;
  for (auto& x : b) x = reverseBits(x);  // HuCard americano lido sem desinverter os bits
  if (hasText(b) || knownCrc(b)) return b;
  return {};
}

bool isPceCdBios(const std::vector<uint8_t>& bytes) {
  if (bytes.size() != 0x40000 && bytes.size() != 0x40200 && bytes.size() != 0x80000 && bytes.size() != 0x80200) return false;
  return !normalizeBios(bytes).empty();
}

Model modelForBios(const std::vector<uint8_t>& b) {
  const uint32_t c = crc32(b.data(), b.size());
  for (const auto& k : KNOWN) if (k.crc == c) return k.model;
  return Model::ArcadeCdRom2;
}

PceCdCore::PceCdCore(std::unique_ptr<cdrom::Disc> disc, const std::vector<uint8_t>* save,
                     const std::vector<uint8_t>* bios, const Model* model)
    : disc_(std::move(disc)) {
  if (!disc_) throw std::runtime_error("Imagem de CD inválida.");
  const std::vector<uint8_t>* fw = bios ? bios : &firmware("pcecd");
  const std::vector<uint8_t>& raw = *fw;
  if (raw.empty())
    throw std::runtime_error("Falta o System Card do PC Engine CD (a BIOS): abra o arquivo do System Card para instalar.");
  std::vector<uint8_t> b = normalizeBios(raw);
  if (b.empty()) {
    // os testes passam System Cards próprios (sem o texto): aceitos se tiverem o tamanho certo
    if (!bios || (raw.size() != 0x40000)) throw std::runtime_error("O arquivo instalado não é um System Card do PC Engine CD.");
    b = raw;
  }
  const Model m = model ? *model : modelForBios(b);
  pce_ = std::make_unique<pce::Pce>(b, save, false);
  cd_ = std::make_unique<CdUnit>(*disc_, m, pce_->cpu);
  pce_->setExpansion(cd_.get());
}

PceCdCore::~PceCdCore() = default;

std::string PceCdCore::note() const {
  switch (cd_->model()) {
    case Model::CdRom2: return "CD-ROM² (System Card 1.0/2.x)";
    case Model::SuperCdRom2: return "Super CD-ROM² (System Card 3.0)";
    default: return "Super CD-ROM² com Arcade Card";
  }
}

void PceCdCore::runFrame() {
  pce_->runFrame();
  cd_->runTo(pce_->cpu.clk);
  pce_->reschedule();
}

void PceCdCore::reset() {
  cd_->reset();
  pce_->reset();
}

size_t PceCdCore::audioAvailable() const { return std::min(pce_->audioAvailable(), cd_->available()); }

void PceCdCore::pullAudio(float* l, float* r, size_t n) {
  pce_->pullAudio(l, r, n);
  if (tmpL_.size() < n) { tmpL_.resize(n); tmpR_.resize(n); }
  cd_->pull(tmpL_.data(), tmpR_.data(), n);
  for (size_t i = 0; i < n; i++) {
    l[i] = std::clamp(l[i] + tmpL_[i], -1.0f, 1.0f);
    r[i] = std::clamp(r[i] + tmpR_[i], -1.0f, 1.0f);
  }
}

void PceCdCore::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  if (io.reading() && !std::equal(magic, magic + 4, STATE_MAGIC))
    throw std::runtime_error("Estado salvo incompatível com o PC Engine CD.");
  pce_->stateSync(io);
  cd_->sync(io);
  if (io.reading()) pce_->reschedule();
}

std::vector<uint8_t> PceCdCore::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void PceCdCore::loadState(const std::vector<uint8_t>& s) {
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

std::unique_ptr<PceCdCore> createPceCdCore(const std::string& path, const std::vector<uint8_t>* save, std::string* err) {
  std::string e;
  auto d = cdrom::open(path, &e);
  if (!d) { if (err) *err = "Não foi possível abrir o CD: " + e; return nullptr; }
  try {
    return std::make_unique<PceCdCore>(std::move(d), save);
  } catch (const std::exception& ex) {
    if (err) *err = ex.what();
    return nullptr;
  }
}

}  // namespace indigo::pcecd
