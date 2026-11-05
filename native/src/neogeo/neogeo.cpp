#include "neogeo.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "../core/zip.h"

namespace indigo::neogeo {

namespace {
const char STATE_MAGIC[4] = {'I', 'N', 'G', '1'};
constexpr uint32_t WATCHDOG_CYCLES = 1622015;  // ~8 quadros sem "chutar" o vigia (0x300001) reinicia o aparelho
constexpr int64_t CYC_PER_FRAME = int64_t(LSPC::CYC_PER_LINE) * LSPC::LINES;
constexpr uint32_t RTC_HZ = 12000000;

bool endsWith(const std::string& s, const std::string& suf) {
  return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// cartão de memória formatado (mesmo conteúdo do Geolith, BSD-3): cabeçalho e a cadeia de blocos livres
void formatMemcard(std::vector<uint8_t>& m) {
  static const uint8_t HDR[64] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x0a, 0x0a, 0xff,
      0x52, 0x6f, 0x73, 0x79, 0x20, 0x4c, 0x65, 0x65, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
      0x4e, 0x40, 0x45, 0x44, 0x4f, 0x48, 0x2d, 0x4c, 0x47, 0x50, 0x45, 0x54, 0x4f, 0x58, 0x80, 0x5c,
      0x02, 0x60, 0x00, 0x64, 0x00, 0x68, 0x00, 0x6c, 0x00, 0x70, 0x00, 0x74, 0x00, 0x78, 0x00, 0x7c};
  m.assign(0x800, 0);
  std::memcpy(m.data(), HDR, sizeof HDR);
  for (int k = 0; k < 32; k++) {
    const int v = 0x80 + 8 * k;
    m[0x40 + 4 * k] = 0xFF;
    m[0x41 + 4 * k] = static_cast<uint8_t>(v);
    m[0x42 + 4 * k] = static_cast<uint8_t>((v + 4) >> 8);
    m[0x43 + 4 * k] = static_cast<uint8_t>(v + 4);
  }
  for (int i = 0; i < 5; i++) m[0xC0 + i] = m[0x100 + i] = 2;
  for (int i = 0x140; i < 0x800; i += 2) {
    const int v = 0x280 + 4 * ((i - 0x140) / 2);
    m[i] = static_cast<uint8_t>(v >> 8);
    m[i + 1] = static_cast<uint8_t>(v);
  }
}

void wordSwap(std::vector<uint8_t>& v) {
  for (size_t i = 0; i + 1 < v.size(); i += 2) std::swap(v[i], v[i + 1]);
}
}  // namespace

// tabela da redução vertical (000-lo.lo): o nível n desenha n+1 das 256 linhas, escolhidas numa ordem fixa que
// espalha as linhas puladas; cada entrada diz qual linha do sprite vai para cada linha da tela (0xFF = fim)
std::vector<uint8_t> makeL0() {
  std::vector<uint8_t> l0(0x10000, 0xFF);
  bool used[256] = {};
  int level = 0;
  static const int I[8] = {8, 12, 10, 14, 9, 13, 11, 15};
  static const int J[4] = {0, 16, -8, 8};
  static const int K[8] = {0, -128, 64, -64, 32, -96, 96, -32};
  for (int i : I)
    for (int j : J)
      for (int k : K) {
        used[128 + i + j + k] = true;
        int n = 0;
        for (int v = 0; v < 256; v++)
          if (used[v]) l0[level * 256 + n++] = static_cast<uint8_t>(v);
        level++;
      }
  return l0;
}

bool isNeoGeoSet(const std::vector<uint8_t>& zip, const std::string& nome) {
  return zip::isZip(zip) && looksLikeNeoGeoZip(zip, nome);
}

bool isNeoGeoBiosZip(const std::vector<uint8_t>& data, const std::string& nome) {
  std::vector<zip::Entry> e;
  if (!zip::isZip(data) || !zip::listEntries(data, e)) return false;
  (void)nome;
  bool bios = false, game = false;
  for (auto& x : e) {
    const std::string l = lower(x.name);
    if (x.size == 0x20000 && (l == "sp-s2.sp1" || l == "sp-s3.sp1" || l == "sp-s.sp1" || l == "sp-u2.sp1" ||
                              l == "sp-e.sp1" || l == "sp-45.sp1" || l == "vs-bios.rom" || l == "neo-epo.bin" ||
                              l == "neo-po.bin" || l == "japan-j3.bin" || (l.rfind("uni-bios", 0) == 0 && endsWith(l, ".rom"))))
      bios = true;
    if (endsWith(l, ".p1") || endsWith(l, ".c1")) game = true;
  }
  return bios && !game;
}

bool isNeoGeoRom(const std::vector<uint8_t>& rom, const std::string& nome) {
  if (isNeoFile(rom)) return true;
  return isNeoGeoSet(rom, nome);
}

// ------------------------------------------------------------------ construção
NeoGeo::~NeoGeo() = default;

NeoGeo::NeoGeo(const std::vector<uint8_t>& rom, const std::string& nome, const std::vector<uint8_t>* save)
    : cpu(*this), z80(*this), ram_(0x10000), nvram_(0x10000), zram_(0x800), cartRam_(0x2000), pvcRam_(0x1000) {
  const std::string n = lower(nome);
  if (isNeoFile(rom)) cart = loadNeo(rom);
  else if (zip::isZip(rom)) cart = loadZipSet(rom, n);
  else throw std::runtime_error("Arquivo do Neo Geo não reconhecido (use o .zip do MAME ou o .neo).");
  loadBios();
  hasCartRam_ = cart.board == BOARD_RAM8K;
  uint32_t m = 1;
  while (m < std::min<size_t>(cart.p.size(), 0x100000)) m <<= 1;
  pFixedMask_ = m - 1;
  lspc.setCrom(cart.c.data(), cart.c.size());
  lspc.setL0(l0_.data());
  ym.setRom(cart.v1.data(), cart.v1.size(), cart.v2.empty() ? cart.v1.data() : cart.v2.data(),
            cart.v2.empty() ? cart.v1.size() : cart.v2.size());
  formatMemcard(memcard_);
  if (save) importSave(*save);
  dirty_ = false;
  // relógio do MVS começa na hora local
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  rtcTime_[0] = tm.tm_sec % 60; rtcTime_[1] = tm.tm_min; rtcTime_[2] = tm.tm_hour; rtcTime_[3] = tm.tm_mday;
  rtcTime_[4] = tm.tm_wday; rtcTime_[5] = tm.tm_mon + 1; rtcTime_[6] = tm.tm_year % 100;
  std::string note = "BIOS: " + biosName + (mvs ? " (modo MVS: SELECT = moeda)" : " (modo AES)");
  if (!cart.note.empty()) note += ". " + cart.note;
  cart.note = note;
  setSampleRate(48000);
  reset();
}

void NeoGeo::loadBios() {
  const std::vector<uint8_t> fw = firmware("neogeo");
  if (fw.empty()) throw std::runtime_error("Abra o arquivo neogeo.zip (BIOS do Neo Geo) como um jogo");
  std::vector<zip::Entry> e;
  if (zip::isZip(fw)) {
    if (!zip::listEntries(fw, e)) throw std::runtime_error("O neogeo.zip (BIOS do Neo Geo) está danificado.");
    auto get = [&](const std::string& name, std::vector<uint8_t>& out) {
      for (auto& x : e)
        if (lower(x.name) == name) return zip::extractEntry(fw, x.name, out);
      return false;
    };
    // 1) Universe BIOS, a mais nova
    std::string uni;
    for (auto& x : e) {
      const std::string l = lower(x.name);
      if (l.rfind("uni-bios", 0) == 0 && endsWith(l, ".rom") && x.size == 0x20000 && l > uni) uni = l;
    }
    static const char* MVS[] = {"sp-s2.sp1", "sp-s3.sp1", "sp-s.sp1",   "sp-u2.sp1",    "sp1-u2",      "sp-e.sp1",
                                "sp-45.sp1", "sp1-u4.bin", "sp1-u3.bin", "sp-j2.sp1",   "sp-j3.sp1",   "vs-bios.rom",
                                "japan-j3.bin", "sp1.jipan.1024", "sp-1v1_3db8c.bin"};
    static const char* AES[] = {"neo-epo.bin", "neo-po.bin", "aes-bios.bin"};
    if (!uni.empty() && get(uni, bios_)) {
      biosName = "Universe BIOS " + uni.substr(9, uni.size() - 13);
      std::replace(biosName.begin(), biosName.end(), '_', '.');
      mvs = true;
    }
    for (const char* b : MVS) {
      if (!bios_.empty()) break;
      if (get(b, bios_)) { biosName = std::string("MVS ") + b; mvs = true; }
    }
    for (const char* b : AES) {
      if (!bios_.empty()) break;
      if (get(b, bios_)) { biosName = std::string("AES ") + b; mvs = false; }
    }
    if (bios_.empty()) throw std::runtime_error("O neogeo.zip não tem nenhuma BIOS conhecida (sp-s2.sp1, uni-bios...).");
    get("sfix.sfix", sfix_);
    get("sm1.sm1", sm1_);
    get("000-lo.lo", l0_);
  } else if (fw.size() == 0x20000) {  // arquivo da BIOS solto
    bios_ = fw;
    biosName = "MVS";
    mvs = true;
  } else {
    throw std::runtime_error("Abra o arquivo neogeo.zip (BIOS do Neo Geo) como um jogo");
  }
  if (bios_.size() < 0x20000) bios_.resize(0x20000, 0xFF);
  bios_.resize(0x20000);
  wordSwap(bios_);  // os arquivos da BIOS têm as palavras com os bytes trocados, como as P ROMs
  sfix_.resize(0x20000, 0);
  if (sm1_.empty() || !mvs) sm1_.clear();
  else sm1_.resize(0x20000, 0xFF);
  if (l0_.size() < 0x10000) l0_ = makeL0();
}

void NeoGeo::reset() {
  std::fill(ram_.begin(), ram_.end(), 0);
  std::fill(zram_.begin(), zram_.end(), 0);
  std::fill(std::begin(lspc.vram), std::end(lspc.vram), 0);
  lspc.vramChanged();
  std::fill(std::begin(lspc.pal), std::end(lspc.pal), 0);
  std::fill(pvcRam_.begin(), pvcRam_.end(), 0);
  timerCtrl_ = 0; timerReload_ = timerCounter_ = timerFrag_ = 0;
  crtFix_ = !mvs;  // ao ligar o MVS usa a SFIX e o SM1 da placa (a BIOS troca para o cartucho)
  irqPending_ = 0;  // os relógios (cyc_, z80Cyc_, frameEnd_) continuam: o som não perde a sincronia
  frame = 0;
  softReset();
  irqPending_ |= 4;  // interrupção de reset (nível 3) pendente ao ligar
  rtcCmd_ = rtcClk_ = rtcStb_ = rtcTp_ = rtcTpMode_ = rtcFmode_ = rtc1Hz_ = 0;
  rtcData_ = 0; rtcCycles_ = 0; rtcTpInterval_ = RTC_HZ; rtcTpCounter_ = 0;
}

void NeoGeo::softReset() {
  syncZ80(now());
  soundCode_ = soundReply_ = 0;
  watchdog_ = 0;
  vecBios_ = true;
  sramLock_ = false;
  crdLock_[0] = crdLock_[1] = 0;
  crdRegSel_ = 0;
  pOutput_ = 0;
  smaRng_ = 0x2345;
  bankBase_ = cart.p.size() > 0x100000 ? 0x100000 : 0;
  cartReg_[0] = cartReg_[1] = 0;
  protReg_ = 0;
  kof98State_ = 0;
  lspc.reset();
  selectFix();
  z80.reset();
  ym.run(now());
  ym.reset();
  nmiEnabled_ = nmiPending_ = false;
  zbank_[0] = 0x8000; zbank_[1] = 0xC000; zbank_[2] = 0xE000; zbank_[3] = 0xF000;
  cpu.reset();
  resetStall_ = 40;  // o reset do 68000 leva 40 ciclos (contados no próximo passo)
}

void NeoGeo::selectFix() {
  if (crtFix_ || sfix_.empty()) lspc.setFix(cart.s.data(), cart.s.size(), cart.fixBank);
  else lspc.setFix(sfix_.data(), sfix_.size(), 0);
}

void NeoGeo::setSampleRate(int rate) { ym.setRates(masterClock() / 3.0 / 144.0, rate > 0 ? rate : 48000); }

// ------------------------------------------------------------------ save (RAM de backup + cartão + RAM do cartucho)
std::vector<uint8_t> NeoGeo::exportSave() {
  dirty_ = false;
  std::vector<uint8_t> s(nvram_);
  s.insert(s.end(), memcard_.begin(), memcard_.end());
  if (hasCartRam_) s.insert(s.end(), cartRam_.begin(), cartRam_.end());
  return s;
}

void NeoGeo::importSave(const std::vector<uint8_t>& b) {
  if (b.size() < nvram_.size() + memcard_.size()) return;
  std::copy(b.begin(), b.begin() + 0x10000, nvram_.begin());
  std::copy(b.begin() + 0x10000, b.begin() + 0x10800, memcard_.begin());
  if (hasCartRam_ && b.size() >= 0x12800) std::copy(b.begin() + 0x10800, b.begin() + 0x12800, cartRam_.begin());
}

// ------------------------------------------------------------------ 68000: programa e bancos do cartucho
uint16_t NeoGeo::romWord(uint32_t a) {
  if (cart.board == BOARD_KOF98 && (a & ~3u) == 0x100) {
    if (kof98State_ == 1) return (a & 2) ? 0x00FD : 0x00C2;
    if (kof98State_ == 2) return (a & 2) ? 0x4F2D : 0x4E45;
  }
  const uint32_t i = a & pFixedMask_;
  if (i + 1 >= cart.p.size()) return 0xFFFF;
  return static_cast<uint16_t>((cart.p[i] << 8) | cart.p[i + 1]);
}

uint16_t NeoGeo::bankRead16(uint32_t a) {
  switch (cart.board) {
    case BOARD_SMA: {
      if (a == 0x2FE446) return 0x9A37;
      const SmaInfo& s = SMA_INFO[cart.smaType];
      if (a == s.rng[0] || a == s.rng[1]) {  // gerador aleatório (como no MAME: devolve o valor antes de avançar)
        const uint16_t old = smaRng_;
        const uint16_t bit = ((smaRng_ >> 2) ^ (smaRng_ >> 3) ^ (smaRng_ >> 5) ^ (smaRng_ >> 6) ^ (smaRng_ >> 7) ^
                              (smaRng_ >> 11) ^ (smaRng_ >> 12) ^ (smaRng_ >> 15)) & 1;
        smaRng_ = static_cast<uint16_t>((smaRng_ << 1) | bit);
        return old;
      }
      break;
    }
    case BOARD_PVC:
      if (a >= 0x2FE000) return pvcRam_[(a >> 1) & 0xFFF];
      break;
    case BOARD_MSLUGX:
      if (a >= 0x2FFFE0 && a <= 0x2FFFEF) {
        auto pbyte = [&](uint32_t x) -> uint8_t { return x < cart.p.size() ? cart.p[x] : 0xFF; };
        if (cartReg_[0] == 0x0001) {
          const uint16_t r = (pbyte(0xDEDD2 + ((cartReg_[1] >> 3) & 0xFFF)) >> (~cartReg_[1] & 7)) & 1;
          cartReg_[1]++;
          return r;
        }
        if (cartReg_[0] == 0x0FFF) {
          const int32_t sel = static_cast<int32_t>((ram_[0xF00A] << 8) | ram_[0xF00B]) - 1;
          return (pbyte(0xDEDD2 + ((sel >> 3) & 0xFFF)) >> (~sel & 7)) & 1;
        }
        return 0;
      }
      break;
    case BOARD_CT0: {
      const uint16_t r = (protReg_ >> 24) & 0xFF;
      switch (a) {
        case 0x200000: case 0x236000: case 0x236008: case 0x255550: case 0x2FF000: case 0x2FFFF0: return r;
        case 0x236004: case 0x23600C: return static_cast<uint16_t>(((r & 0x0F) << 4) | ((r & 0xF0) >> 4));
        default: break;
      }
      break;
    }
    case BOARD_RAM8K:
      if (a <= 0x201FFF) return static_cast<uint16_t>((cartRam_[a & 0x1FFE] << 8) | cartRam_[(a & 0x1FFE) + 1]);
      return 0xFFFF;
    default: break;
  }
  uint32_t i = cart.p.size() > 0x100000 ? bankBase_ + (a & 0xFFFFF) : (a & pFixedMask_);
  if (i + 1 >= cart.p.size()) return 0xFFFF;
  return static_cast<uint16_t>((cart.p[i] << 8) | cart.p[i + 1]);
}

uint8_t NeoGeo::bankRead8(uint32_t a) {
  if (cart.board == BOARD_CT0) {
    const uint8_t r = (protReg_ >> 24) & 0xFF;
    switch (a) {
      case 0x200001: case 0x236001: case 0x236009: case 0x255551: case 0x2FF001: case 0x2FFFF1: return r;
      case 0x236005: case 0x23600D: return static_cast<uint8_t>(((r & 0x0F) << 4) | ((r & 0xF0) >> 4));
      default: break;
    }
    const uint32_t i = cart.p.size() > 0x100000 ? bankBase_ + (a & 0xFFFFF) : (a & pFixedMask_);
    return i < cart.p.size() ? cart.p[i] : 0xFF;
  }
  const uint16_t w = bankRead16(a & ~1u);
  return static_cast<uint8_t>((a & 1) ? w : w >> 8);
}

void NeoGeo::bankWrite(uint32_t a, uint16_t v, bool byte) {
  auto defaultBank = [&]() {
    if (a < 0x2FFFF0) return;
    const size_t len = cart.p.size();
    int bank = v & 7;
    if (len <= 0x100000) return;
    if (static_cast<size_t>(bank + 1) * 0x100000 >= len) bank = 0;
    bankBase_ = static_cast<uint32_t>(bank + 1) * 0x100000;
  };
  switch (cart.board) {
    case BOARD_SMA: {
      const SmaInfo& s = SMA_INFO[cart.smaType];
      if ((a & ~1u) == s.bankAddr) {
        int b = 0;
        for (int i = 0; i < 6; i++) b |= ((v >> s.bits[i]) & 1) << i;
        bankBase_ = (0x100000 + s.offset[b]) & 0xFFFFFF;
      }
      return;
    }
    case BOARD_PVC: {
      if (a < 0x2FE000) return;
      const uint32_t off = (a >> 1) & 0xFFF;
      uint16_t& w = pvcRam_[off];
      if (!byte) w = v;
      else if (a & 1) w = static_cast<uint16_t>((w & 0xFF00) | (v & 0xFF));
      else w = static_cast<uint16_t>((w & 0x00FF) | (v << 8));
      if (off == 0xFF0) {  // cor empacotada -> componentes
        const uint16_t pen = pvcRam_[0xFF0];
        const uint8_t b = static_cast<uint8_t>(((pen & 0x000F) << 1) | ((pen & 0x1000) >> 12));
        const uint8_t g = static_cast<uint8_t>(((pen & 0x00F0) >> 3) | ((pen & 0x2000) >> 13));
        const uint8_t r = static_cast<uint8_t>(((pen & 0x0F00) >> 7) | ((pen & 0x4000) >> 14));
        const uint8_t s = static_cast<uint8_t>((pen & 0x8000) >> 15);
        pvcRam_[0xFF1] = static_cast<uint16_t>((g << 8) | b);
        pvcRam_[0xFF2] = static_cast<uint16_t>((s << 8) | r);
      } else if (off == 0xFF4 || off == 0xFF5) {  // componentes -> cor empacotada
        const uint16_t gb = pvcRam_[0xFF4], sr = pvcRam_[0xFF5];
        pvcRam_[0xFF6] = static_cast<uint16_t>(((gb & 0x001E) >> 1) | ((gb & 0x1E00) >> 5) | ((sr & 0x001E) << 7) |
                                               ((gb & 0x0001) << 12) | ((gb & 0x0100) << 5) | ((sr & 0x0001) << 14) |
                                               ((sr & 0x0100) << 7));
      } else if (off >= 0xFF8) {  // troca de banco
        const uint32_t addr = (pvcRam_[0xFF8] >> 8) | (static_cast<uint32_t>(pvcRam_[0xFF9]) << 8);
        pvcRam_[0xFF8] = static_cast<uint16_t>((pvcRam_[0xFF8] & 0xFE00) | 0x00A0);
        pvcRam_[0xFF9] &= 0x7FFF;
        bankBase_ = (addr + 0x100000) & 0xFFFFFF;
      }
      return;
    }
    case BOARD_KOF98:
      if (a == 0x20AAAA && !byte) {
        if (v == 0x0090) kof98State_ = 1;
        else if (v == 0x00F0) kof98State_ = 2;
        return;
      }
      defaultBank();
      return;
    case BOARD_MSLUGX:
      if (a >= 0x2FFFE0 && a <= 0x2FFFEF) {
        switch (a & ~1u) {
          case 0x2FFFE0: cartReg_[0] = 0; break;
          case 0x2FFFE2: case 0x2FFFE4: cartReg_[0] |= v; break;
          case 0x2FFFEA: cartReg_[0] = 0; cartReg_[1] = 0; break;
          default: break;
        }
        return;
      }
      defaultBank();
      return;
    case BOARD_CT0:
      if (byte) {
        switch (a) {
          case 0x236001: case 0x236005: case 0x236009: case 0x23600D: case 0x255551: case 0x2FF001: case 0x2FFFF1:
            protReg_ <<= 8;
            return;
          default: break;
        }
      } else {
        switch (a) {
          case 0x211112: protReg_ = 0xFF000000; return;
          case 0x233332: protReg_ = 0x0000FFFF; return;
          case 0x242812: protReg_ = 0x81422418; return;
          case 0x244442: protReg_ = 0x00FF0000; return;
          case 0x255552: protReg_ = 0xFF00FF00; return;
          case 0x256782: protReg_ = 0xF05A3601; return;
          default: break;
        }
      }
      defaultBank();
      return;
    case BOARD_RAM8K:
      if (a <= 0x201FFF) {
        if (byte) cartRam_[a & 0x1FFF] = static_cast<uint8_t>(v);
        else { cartRam_[a & 0x1FFE] = static_cast<uint8_t>(v >> 8); cartRam_[(a & 0x1FFE) + 1] = static_cast<uint8_t>(v); }
        dirty_ = true;
      }
      return;
    default:
      defaultBank();
      return;
  }
}

// ------------------------------------------------------------------ 68000: registradores
uint8_t NeoGeo::pad(uint32_t k) {
  uint8_t v = 0xFF;
  if (k & KEY_UP) v &= ~0x01;
  if (k & KEY_DOWN) v &= ~0x02;
  if (k & KEY_LEFT) v &= ~0x04;
  if (k & KEY_RIGHT) v &= ~0x08;
  if (k & KEY_B) v &= ~0x10;  // A
  if (k & KEY_A) v &= ~0x20;  // B
  if (k & KEY_Y) v &= ~0x40;  // C
  if (k & KEY_X) v &= ~0x80;  // D
  return v;
}

uint8_t NeoGeo::statusA() {
  // moedas 1-2 e serviço soltos (1 = solto); moedas 3-4 só existem no MVS (a Universe BIOS num AES quer 0);
  // bits 6-7 = relógio
  uint8_t v = mvs ? 0x1F : 0x07;
  if (mvs && (keys_ & KEY_SELECT)) v &= ~0x01;
  if (mvs && (keys2_ & KEY_SELECT)) v &= ~0x02;  // moeda 2
  if (mvs) v = static_cast<uint8_t>(v | (rtcRead() << 6));
  return v;
}

uint8_t NeoGeo::statusB() {
  uint8_t v = 0x0F;  // start/select dos 2 jogadores soltos; cartão de memória presente e sem proteção
  if (keys_ & KEY_START) v &= ~0x01;
  if (!mvs && (keys_ & KEY_SELECT)) v &= ~0x02;
  if (keys2_ & KEY_START) v &= ~0x04;
  if (!mvs && (keys2_ & KEY_SELECT)) v &= ~0x08;
  if (mvs) v |= 0x80;
  return v;
}

uint16_t NeoGeo::ioRead16(uint32_t a) {
  switch (a & 0xFE0000) {
    case 0x300000: {
      if (a & 0x80) return static_cast<uint16_t>((((keys_ & KEY_L) ? 0x00 : 0x80) << 8) | ((keys_ & KEY_L) ? 0x00 : 0x80));
      const uint8_t p = p1();
      return static_cast<uint16_t>((p << 8) | 0xFF);  // P1 / chaves DIP (todas desligadas)
    }
    case 0x320000:
      syncZ80(now());
      return static_cast<uint16_t>((soundReply_ << 8) | statusA());
    case 0x340000: return static_cast<uint16_t>((pad(keys2_) << 8) | 0xFF);  // P2
    case 0x380000: { const uint8_t b = statusB(); return static_cast<uint16_t>((b << 8) | b); }
    case 0x3C0000:
      switch (a & 0xE) {
        case 0x0: case 0x2: case 0x8: case 0xA: return lspc.readData();
        case 0x4: case 0xC: return lspc.readMod();
        default: return lspc.readMode();
      }
    default: return 0xFFFF;
  }
}

uint8_t NeoGeo::ioRead8(uint32_t a) {
  switch (a & 0xFE0000) {
    case 0x300000:
      if (!(a & 1)) return p1();
      if (a & 0x80) return (keys_ & KEY_L) ? 0x00 : 0x80;  // REG_SYSTYPE: bit 7 = botão de teste, bit 6 = 2 encaixes
      return 0xFF;  // REG_DIPSW
    case 0x320000:
      if (!(a & 1)) { syncZ80(now()); return soundReply_; }
      return statusA();
    case 0x340000: return (a & 1) ? 0xFF : pad(keys2_);
    case 0x380000: return statusB();
    default: {
      const uint16_t w = ioRead16(a & ~1u);
      return static_cast<uint8_t>((a & 1) ? w : w >> 8);
    }
  }
}

void NeoGeo::systemLatch(int reg) {
  switch (reg) {
    case 0: lspc.setShadow(false); break;            // REG_NOSHADOW
    case 8: lspc.setShadow(true); break;             // REG_SHADOW
    case 1: vecBios_ = true; break;                  // REG_SWPBIOS
    case 9: vecBios_ = false; break;                 // REG_SWPROM
    case 2: crdLock_[0] = 0; break;                  // REG_CRDUNLOCK1
    case 10: crdLock_[0] = 1; break;                 // REG_CRDLOCK1
    case 3: crdLock_[1] = 1; break;                  // REG_CRDLOCK2
    case 11: crdLock_[1] = 0; break;                 // REG_CRDUNLOCK2
    case 4: crdRegSel_ = 1; break;                   // REG_CRDREGSEL
    case 12: crdRegSel_ = 0; break;                  // REG_CRDNORMAL
    case 5: syncZ80(now()); crtFix_ = false; selectFix(); break;  // REG_BRDFIX (SFIX e SM1 da placa)
    case 13: syncZ80(now()); crtFix_ = true; selectFix(); break;  // REG_CRTFIX (S e M1 do cartucho)
    case 6: sramLock_ = true; break;                 // REG_SRAMLOCK
    case 14: sramLock_ = false; break;               // REG_SRAMUNLOCK
    case 7: lspc.setPalBank(1); break;               // REG_PALBANK1
    case 15: lspc.setPalBank(0); break;              // REG_PALBANK0
    default: break;
  }
}

void NeoGeo::soundCommand(uint8_t v) {
  syncZ80(now());
  soundCode_ = v;
  nmiPending_ = true;
  if (nmiEnabled_) z80.nmiPending = true;  // pulso de NMI (como no Geolith; desligado = perdido)
}

void NeoGeo::ioWrite(uint32_t a, uint16_t v, bool byte) {
  const bool lo = !byte || (a & 1);  // o byte de baixo foi escrito
  const uint8_t lv = static_cast<uint8_t>(v);
  switch (a & 0xFE0000) {
    case 0x300000:
      if (lo) watchdog_ = 0;  // DOGE: qualquer escrita em 0x300001 reinicia a contagem do vigia
      break;
    case 0x320000:
      if (!byte) soundCommand(static_cast<uint8_t>(v >> 8));
      else if (!(a & 1)) soundCommand(lv);
      break;
    case 0x380000:
      if (!lo) break;
      switch (a & 0x7E) {
        case 0x00: pOutput_ = lv; break;
        case 0x50: if (mvs) rtcWrite(lv & 7); break;
        default: break;
      }
      break;
    case 0x3A0000:
      if (lo) systemLatch((a >> 1) & 0xF);
      break;
    case 0x3C0000: {
      if (byte) {
        if (a & 1) break;  // só o byte de cima responde (e vai para os dois lados)
        v = static_cast<uint16_t>((lv << 8) | lv);
      }
      switch (a & 0xE) {
        case 0x0: lspc.writeAddr(v); break;
        case 0x2: lspc.writeData(v); break;
        case 0x4: lspc.writeMod(v); break;
        case 0x6: {
          lspc.writeMode(v);
          const uint8_t prev = timerCtrl_;
          timerCtrl_ = v & 0xF0;
          if (!(prev & 0x40) && (timerCtrl_ & 0x40) && lspc.scanline >= 248) timerCounter_ = timerReload_;
          break;
        }
        case 0x8: timerReload_ = (timerReload_ & 0xFFFF) | (static_cast<uint32_t>(v) << 16); break;
        case 0xA:
          timerReload_ = (timerReload_ & 0xFFFF0000) | v;
          if (timerCtrl_ & 0x20) timerCounter_ = timerReload_;
          break;
        case 0xC:
          if (v & 4) irqPending_ &= ~1;
          if (v & 2) irqPending_ &= ~2;
          if (v & 1) irqPending_ &= ~4;
          break;
        default: break;
      }
      break;
    }
    default: break;
  }
}

// ------------------------------------------------------------------ 68000: mapa de memória
uint16_t NeoGeo::read16(uint32_t a) {
  if (a < 0x100000) {
    if (a < 0x80 && vecBios_) return static_cast<uint16_t>((bios_[a] << 8) | bios_[a + 1]);
    return romWord(a);
  }
  if (a < 0x200000) { const uint32_t o = a & 0xFFFF; return static_cast<uint16_t>((ram_[o] << 8) | ram_[o + 1]); }
  if (a < 0x300000) return bankRead16(a);
  if (a < 0x400000) return ioRead16(a);
  if (a < 0x800000) return lspc.palRead(a);
  if (a < 0xC00000) return static_cast<uint16_t>(0xFF00 | memcard_[(a >> 1) & 0x7FF]);
  if (a < 0xD00000) { const uint32_t o = a & 0x1FFFF; return static_cast<uint16_t>((bios_[o] << 8) | bios_[o + 1]); }
  if (a < 0xE00000 && mvs) { const uint32_t o = a & 0xFFFF; return static_cast<uint16_t>((nvram_[o] << 8) | nvram_[o + 1]); }
  return 0xFFFF;
}

uint8_t NeoGeo::read8(uint32_t a) {
  if (a < 0x100000) {
    if (a < 0x80 && vecBios_) return bios_[a];
    const uint16_t w = romWord(a & ~1u);
    return static_cast<uint8_t>((a & 1) ? w : w >> 8);
  }
  if (a < 0x200000) return ram_[a & 0xFFFF];
  if (a < 0x300000) return bankRead8(a);
  if (a < 0x400000) return ioRead8(a);
  if (a < 0x800000) { const uint16_t w = lspc.palRead(a); return static_cast<uint8_t>((a & 1) ? w : w >> 8); }
  if (a < 0xC00000) return (a & 1) ? memcard_[(a >> 1) & 0x7FF] : 0xFF;
  if (a < 0xD00000) return bios_[a & 0x1FFFF];
  if (a < 0xE00000 && mvs) return nvram_[a & 0xFFFF];
  return 0xFF;
}

void NeoGeo::write16(uint32_t a, uint16_t v) {
  if (a < 0x100000) return;
  if (a < 0x200000) { const uint32_t o = a & 0xFFFF; ram_[o] = static_cast<uint8_t>(v >> 8); ram_[o + 1] = static_cast<uint8_t>(v); return; }
  if (a < 0x300000) { bankWrite(a, v, false); return; }
  if (a < 0x400000) { ioWrite(a, v, false); return; }
  if (a < 0x800000) { lspc.palWrite(a, v); return; }
  if (a < 0xC00000) {
    if (!crdLock_[0] && !crdLock_[1]) {
      uint8_t& m = memcard_[(a >> 1) & 0x7FF];
      if (m != static_cast<uint8_t>(v)) { m = static_cast<uint8_t>(v); dirty_ = true; }
    }
    return;
  }
  if (a >= 0xD00000 && a < 0xE00000 && mvs && !sramLock_) {
    const uint32_t o = a & 0xFFFF;
    if (nvram_[o] != static_cast<uint8_t>(v >> 8) || nvram_[o + 1] != static_cast<uint8_t>(v)) dirty_ = true;
    nvram_[o] = static_cast<uint8_t>(v >> 8);
    nvram_[o + 1] = static_cast<uint8_t>(v);
  }
}

void NeoGeo::write8(uint32_t a, uint8_t v) {
  if (a < 0x100000) return;
  if (a < 0x200000) { ram_[a & 0xFFFF] = v; return; }
  if (a < 0x300000) { bankWrite(a, v, true); return; }
  if (a < 0x400000) { ioWrite(a, v, true); return; }
  if (a < 0x800000) {
    uint16_t w = lspc.palRead(a);
    w = (a & 1) ? static_cast<uint16_t>((w & 0xFF00) | v) : static_cast<uint16_t>((w & 0x00FF) | (v << 8));
    lspc.palWrite(a, w);
    return;
  }
  if (a < 0xC00000) {
    if (!crdLock_[0] && !crdLock_[1]) {
      uint8_t& m = memcard_[(a >> 1) & 0x7FF];
      if (m != v) { m = v; dirty_ = true; }
    }
    return;
  }
  if (a >= 0xD00000 && a < 0xE00000 && mvs && !sramLock_) {
    uint8_t& n = nvram_[a & 0xFFFF];
    if (n != v) { n = v; dirty_ = true; }
  }
}

// ------------------------------------------------------------------ Z80
int NeoGeo::read(int a) {
  const std::vector<uint8_t>& m = (crtFix_ || sm1_.empty()) ? cart.m : sm1_;
  const size_t mask = m.size() - 1;  // tamanho potência de 2 (finish/loadBios)
  if (a < 0x8000) return m[static_cast<size_t>(a) & mask];
  if (a < 0xC000) return m[(zbank_[0] + (a & 0x3FFF)) & mask];
  if (a < 0xE000) return m[(zbank_[1] + (a & 0x1FFF)) & mask];
  if (a < 0xF000) return m[(zbank_[2] + (a & 0x0FFF)) & mask];
  if (a < 0xF800) return m[(zbank_[3] + (a & 0x07FF)) & mask];
  return zram_[a & 0x7FF];
}

void NeoGeo::write(int a, int v) {
  if (a >= 0xF800) zram_[a & 0x7FF] = static_cast<uint8_t>(v);
}

int NeoGeo::in(int port) {
  const int lo = port & 0xFF;
  switch (lo) {
    case 0x00: nmiPending_ = false; return soundCode_;
    case 0x04: case 0x05: case 0x06: case 0x07: ym.run(z80Cyc_); return ym.read(lo & 3);
    default: break;
  }
  switch (lo & 0x0F) {  // troca de banco do NEO-ZMC: o número do banco vai no byte de cima do endereço da porta
    case 0x08: zbank_[3] = static_cast<uint32_t>((port >> 8) & 0x7F) * 0x800; break;
    case 0x09: zbank_[2] = static_cast<uint32_t>((port >> 8) & 0x3F) * 0x1000; break;
    case 0x0A: zbank_[1] = static_cast<uint32_t>((port >> 8) & 0x1F) * 0x2000; break;
    case 0x0B: zbank_[0] = static_cast<uint32_t>((port >> 8) & 0x0F) * 0x4000; break;
    default: break;
  }
  return 0;
}

void NeoGeo::out(int port, int v) {
  switch (port & 0xFF) {
    case 0x00: case 0xC0: soundCode_ = 0; break;
    case 0x04: case 0x05: case 0x06: case 0x07: ym.run(z80Cyc_); ym.write(port & 3, static_cast<uint8_t>(v)); break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: nmiEnabled_ = true; break;
    case 0x0C: soundReply_ = static_cast<uint8_t>(v); break;
    case 0x18: nmiEnabled_ = false; break;
    default: break;
  }
}

void NeoGeo::syncZ80(int64_t target) {
  int guard = 0;
  while (z80Cyc_ < target) {
    ym.run(z80Cyc_);
    z80.irqLine = ym.irq();
    const int t = z80.step();
    z80Cyc_ += static_cast<int64_t>(t > 0 ? t : 4) * 3;  // Z80 a 4 MHz = 1/3 do 68000
    if (++guard > 2000000) { z80Cyc_ = target; break; }
  }
}

// ------------------------------------------------------------------ RTC uPD4990A (MVS)
uint8_t NeoGeo::rtcRead() const {
  const uint8_t out = (rtcFmode_ == 0 || rtcFmode_ == 3) ? rtc1Hz_ : static_cast<uint8_t>(rtcData_ & 1);
  return static_cast<uint8_t>((out << 1) | rtcTp_);
}

void NeoGeo::rtcWrite(uint8_t v) {
  const bool clkUp = !rtcClk_ && (v & 2), stbUp = !rtcStb_ && (v & 4);
  rtcClk_ = v & 2;
  rtcStb_ = v & 4;
  if (stbUp) {
    auto bcd = [](int x) { return static_cast<uint64_t>(((x / 10) << 4) | (x % 10)); };
    auto unbcd = [&](int sh) { return static_cast<int>(((rtcData_ >> (sh + 4)) & 15) * 10 + ((rtcData_ >> sh) & 15)); };
    switch (rtcCmd_ & 15) {
      case 0x0: rtcFmode_ = 0; rtcTpInterval_ = RTC_HZ / 64; break;
      case 0x1: rtcFmode_ = 1; break;
      case 0x2:
        rtcFmode_ = 2;
        rtcTime_[0] = unbcd(0) % 60; rtcTime_[1] = unbcd(8) % 60; rtcTime_[2] = unbcd(16) % 24;
        rtcTime_[3] = std::clamp(unbcd(24), 1, 31); rtcTime_[4] = static_cast<int>((rtcData_ >> 32) & 15) % 7;
        rtcTime_[5] = std::clamp(static_cast<int>((rtcData_ >> 36) & 15), 1, 12); rtcTime_[6] = unbcd(40) % 100;
        break;
      case 0x3:
        rtcFmode_ = 3;
        rtcData_ = bcd(rtcTime_[0]) | (bcd(rtcTime_[1]) << 8) | (bcd(rtcTime_[2]) << 16) | (bcd(rtcTime_[3]) << 24) |
                   (static_cast<uint64_t>(rtcTime_[4]) << 32) | (static_cast<uint64_t>(rtcTime_[5]) << 36) |
                   (bcd(rtcTime_[6]) << 40);
        break;
      case 0x4: rtcTpInterval_ = RTC_HZ / 64; rtcTpMode_ = 0; break;
      case 0x5: rtcTpInterval_ = RTC_HZ / 256; rtcTpMode_ = 0; break;
      case 0x6: rtcTpInterval_ = RTC_HZ / 2048; rtcTpMode_ = 0; break;
      case 0x7: rtcTpInterval_ = RTC_HZ / 4096; rtcTpMode_ = 0; break;
      case 0x8: rtcTpInterval_ = RTC_HZ; rtcTpCounter_ = 0; rtcTpMode_ = 0; break;
      case 0x9: rtcTpInterval_ = RTC_HZ * 10; rtcTpCounter_ = 0; rtcTpMode_ = 0; break;
      case 0xA: rtcTpInterval_ = RTC_HZ * 30; rtcTpCounter_ = 0; rtcTpMode_ = 0; break;
      case 0xB: rtcTpInterval_ = RTC_HZ * 60; rtcTpCounter_ = 0; rtcTpMode_ = 0; break;
      case 0xC: rtcTp_ = 0; rtcTpMode_ = 0; break;
      case 0xD: rtcTpCounter_ = 0; rtcTpMode_ = 0; break;
      case 0xE: rtcTpMode_ = 1; break;
      default: break;
    }
  }
  if (!rtcStb_ && clkUp) {
    const uint64_t outBit = (rtcCmd_ & 1) ? 0x800000000000ull : 0;
    rtcCmd_ = static_cast<uint8_t>(((rtcCmd_ >> 1) & 7) | ((v & 1) << 3));
    if (rtcFmode_ == 1) rtcData_ = ((rtcData_ >> 1) & 0x7FFFFFFFFFFFull) | outBit;
  }
}

void NeoGeo::rtcTick(int c) {
  rtcCycles_ += static_cast<uint32_t>(c);
  if (rtcCycles_ >= RTC_HZ) {
    rtcCycles_ -= RTC_HZ;
    static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int* t = rtcTime_;
    if (++t[0] >= 60) {
      t[0] = 0;
      if (++t[1] >= 60) {
        t[1] = 0;
        if (++t[2] >= 24) {
          t[2] = 0;
          t[4] = (t[4] + 1) % 7;
          const int dm = (t[5] == 2 && t[6] % 4 == 0) ? 29 : DAYS[(t[5] - 1) % 12];
          if (++t[3] > dm) {
            t[3] = 1;
            if (++t[5] > 12) { t[5] = 1; t[6] = (t[6] + 1) % 100; }
          }
        }
      }
    }
  }
  rtc1Hz_ = rtcCycles_ >= RTC_HZ / 2;
  if (rtcTpMode_ == 0) {
    rtcTpCounter_ += static_cast<uint32_t>(c);
    if (rtcTpCounter_ >= rtcTpInterval_) rtcTpCounter_ -= rtcTpInterval_;
    rtcTp_ = rtcTpCounter_ >= rtcTpInterval_ / 2;
  }
}

// ------------------------------------------------------------------ execução
int NeoGeo::irqLevel() const {
  if (irqPending_ & 4) return 3;
  if (irqPending_ & 2) return 2;
  if (irqPending_ & 1) return 1;
  return 0;
}

void NeoGeo::step() {
  int c;
  if (resetStall_) {
    c = resetStall_;
    resetStall_ = 0;
  } else {
    cpu.ipl = irqLevel();
    c = cpu.step();
    if (c <= 0) c = 4;
    cpu.cycles = 0;
  }
  cyc_ += c;
  watchdog_ += static_cast<uint32_t>(c);
  // o jogo travou (ou não chuta o vigia): reinicia como o aparelho (o vídeo volta à linha 0, como no Geolith)
  if (watchdog_ >= WATCHDOG_CYCLES) softReset();
  if (mvs) rtcTick(c);
  // temporizador da interrupção de linha: conta em pontos (6 MHz = metade do 68000)
  uint32_t dec = static_cast<uint32_t>(c) >> 1;
  timerFrag_ += c & 1;
  if (timerFrag_ >= 2) { timerFrag_ -= 2; dec++; }
  if (timerCounter_ > dec) {
    timerCounter_ -= dec;
  } else {
    for (uint32_t i = 0; i < dec; i++) {
      if (--timerCounter_ == 0) {
        if (timerCtrl_ & 0x80) timerCounter_ += timerReload_;
        if (timerCtrl_ & 0x10) irqPending_ |= 2;
      }
    }
  }
  const int ev = lspc.run(c);
  if (ev & LSPC::EV_VBLANK) irqPending_ |= 1;
  if ((ev & LSPC::EV_RELOAD_TIMER) && (timerCtrl_ & 0x40)) timerCounter_ = timerReload_;
}

void NeoGeo::runFrame() {
  frameEnd_ += CYC_PER_FRAME;
  int guard = 0;
  while (cyc_ < frameEnd_ && ++guard < 400000) step();
  if (cyc_ < frameEnd_) cyc_ = frameEnd_;
  syncZ80(cyc_);
  ym.run(cyc_);
  frame++;
}

// ------------------------------------------------------------------ save state
void NeoGeo::sync(StateIO& io) {
  char magic[4];
  std::copy(STATE_MAGIC, STATE_MAGIC + 4, magic);
  io.raw(magic, 4);
  uint32_t psize = static_cast<uint32_t>(cart.p.size()), csize = static_cast<uint32_t>(cart.c.size());
  io.sync(psize);
  io.sync(csize);
  if (io.reading() && (!std::equal(magic, magic + 4, STATE_MAGIC) || psize != cart.p.size() || csize != cart.c.size()))
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  z80.sync(io);
  lspc.sync(io);
  ym.sync(io);
  io.bytes(ram_); io.bytes(nvram_); io.bytes(memcard_); io.bytes(zram_); io.bytes(cartRam_);
  uint32_t n = static_cast<uint32_t>(pvcRam_.size());
  io.sync(n);
  if (io.reading() && n != pvcRam_.size()) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  io.raw(pvcRam_.data(), pvcRam_.size() * 2);
  io.sync(vecBios_); io.sync(sramLock_); io.sync(crtFix_); io.raw(crdLock_, sizeof crdLock_); io.sync(crdRegSel_);
  io.sync(pOutput_); io.sync(soundCode_); io.sync(soundReply_); io.sync(nmiEnabled_); io.sync(nmiPending_);
  io.raw(zbank_, sizeof zbank_); io.sync(irqPending_); io.sync(timerCtrl_); io.sync(timerReload_);
  io.sync(timerCounter_); io.sync(timerFrag_); io.sync(watchdog_);
  io.sync(bankBase_); io.raw(cartReg_, sizeof cartReg_); io.sync(protReg_); io.sync(kof98State_); io.sync(smaRng_);
  io.sync(rtcCmd_); io.sync(rtcClk_); io.sync(rtcStb_); io.sync(rtcTp_); io.sync(rtcTpMode_); io.sync(rtcFmode_);
  io.sync(rtc1Hz_); io.sync(rtcData_); io.sync(rtcCycles_); io.sync(rtcTpInterval_); io.sync(rtcTpCounter_);
  io.raw(rtcTime_, sizeof rtcTime_);
  io.sync(cyc_); io.sync(frameEnd_); io.sync(z80Cyc_); io.sync(frame); io.sync(resetStall_);
  if (io.reading()) {
    // só valores impossíveis são recusados (os índices são limitados no uso)
    for (auto& b : zbank_) if (b > 0x7F * 0x800 + 0x8000) throw std::runtime_error("Estado salvo danificado.");
    if (bankBase_ > 0x1000000) throw std::runtime_error("Estado salvo danificado.");
    if (irqPending_ & ~7) throw std::runtime_error("Estado salvo danificado.");
    if (rtcTpInterval_ == 0 || rtcTpInterval_ > RTC_HZ * 60 || rtcCycles_ > RTC_HZ || rtcTpCounter_ > RTC_HZ * 60)
      throw std::runtime_error("Estado salvo danificado.");
    const int lim[7][2] = {{0, 59}, {0, 59}, {0, 23}, {1, 31}, {0, 6}, {1, 12}, {0, 99}};
    for (int i = 0; i < 7; i++)
      if (rtcTime_[i] < lim[i][0] || rtcTime_[i] > lim[i][1]) throw std::runtime_error("Estado salvo danificado.");
    if (cyc_ < 0 || frameEnd_ < 0 || cyc_ > frameEnd_ + CYC_PER_FRAME || frameEnd_ > cyc_ + 2 * CYC_PER_FRAME ||
        z80Cyc_ < cyc_ - CYC_PER_FRAME || z80Cyc_ > cyc_ + 1000)
      throw std::runtime_error("Estado salvo danificado.");
    if (resetStall_ < 0 || resetStall_ > 40) throw std::runtime_error("Estado salvo danificado.");
    ym.clampClock(cyc_);
    selectFix();
  }
}

std::vector<uint8_t> NeoGeo::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void NeoGeo::loadState(const std::vector<uint8_t>& s) {
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

}  // namespace indigo::neogeo
