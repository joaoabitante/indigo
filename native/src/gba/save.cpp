// Memória de save do cartucho (porte de src/save.js: SRAM, Flash 64K/128K, EEPROM 512B/8K) e relógio do
// cartucho nas portas GPIO (porte de src/gpio.js: S-3511A de Pokémon Ruby/Sapphire/Emerald, Boktai...).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <stdexcept>

#include "gba.h"

namespace indigo::gba {

// ------------------------------------------------------------ tipo de save
SaveType detectSaveType(const std::vector<uint8_t>& rom) {
  // procura as assinaturas deixadas pela biblioteca da Nintendo
  static const char* const TAGS[] = {"EEPROM_V", "SRAM_V", "SRAM_F_V", "FLASH_V", "FLASH512_V", "FLASH1M_V"};
  const int64_t len = static_cast<int64_t>(rom.size());
  for (int64_t i = 0; i < len - 12; i += 4) {
    const int c = rom[static_cast<size_t>(i)];
    if (c != 0x45 && c != 0x53 && c != 0x46) continue;
    for (const char* tag : TAGS) {
      if (c != tag[0]) continue;
      const size_t tl = std::strlen(tag);
      bool ok = true;
      for (size_t k = 1; k < tl; k++) {
        // no JS, ler além do fim dá undefined, que nunca é igual
        if (static_cast<int64_t>(i + k) >= len || rom[static_cast<size_t>(i) + k] != static_cast<uint8_t>(tag[k])) { ok = false; break; }
      }
      if (ok) {
        if (!std::strncmp(tag, "EEPROM", 6)) return SaveType::Eeprom;
        if (!std::strncmp(tag, "SRAM", 4)) return SaveType::Sram;
        if (!std::strcmp(tag, "FLASH1M_V")) return SaveType::Flash128;
        return SaveType::Flash64;
      }
    }
  }
  return SaveType::Sram;  // padrão inofensivo
}

Save::Save(SaveType t) : type(t), isEeprom(t == SaveType::Eeprom) {
  size_t n = 0x2000;
  if (t == SaveType::Sram) n = 0x8000;
  else if (t == SaveType::Flash64) n = 0x10000;
  else if (t == SaveType::Flash128) n = 0x20000;
  data.assign(n, 0xFF);
}

void Save::load(const std::vector<uint8_t>& bytes) {
  std::copy_n(bytes.begin(), std::min(bytes.size(), data.size()), data.begin());
}

// ---------------- SRAM / Flash ----------------
int Save::read8(int64_t addr) {
  if (type == SaveType::Sram) return data[static_cast<size_t>(addr & 0x7FFF)];
  if (isFlash()) {
    if (fId && addr < 2) {
      if (type == SaveType::Flash128) return addr == 0 ? 0x62 : 0x13;
      return addr == 0 ? 0xBF : 0xD4;
    }
    const size_t idx = static_cast<size_t>((fBank << 16) | addr);
    return idx < data.size() ? data[idx] : 0;
  }
  return 0xFF;
}

void Save::write8(int64_t addr, int v) {
  if (type == SaveType::Sram) { data[static_cast<size_t>(addr & 0x7FFF)] = static_cast<uint8_t>(v); dirty = true; return; }
  if (!isFlash()) return;
  if (fCmd == 0xA0) {  // gravação de um byte
    const size_t idx = static_cast<size_t>((fBank << 16) | addr);
    if (idx < data.size()) data[idx] &= static_cast<uint8_t>(v);
    fCmd = 0; dirty = true; return;
  }
  if (fCmd == 0xB0 && addr == 0) { fBank = type == SaveType::Flash128 ? (v & 1) : 0; fCmd = 0; return; }
  if (fState == 0 && addr == 0x5555 && v == 0xAA) { fState = 1; return; }
  if (fState == 1 && addr == 0x2AAA && v == 0x55) { fState = 2; return; }
  if (fState == 2) {
    fState = 0;
    if (fCmd == 0x80) {  // apagar
      if (addr == 0x5555 && v == 0x10) { std::fill(data.begin(), data.end(), 0xFF); dirty = true; }
      else if (v == 0x30) {
        const size_t base = static_cast<size_t>((fBank << 16) | (addr & 0xF000));
        const size_t end = std::min(base + 0x1000, data.size());
        if (base < end) std::fill(data.begin() + static_cast<ptrdiff_t>(base), data.begin() + static_cast<ptrdiff_t>(end), 0xFF);
        dirty = true;
      }
      fCmd = 0; return;
    }
    if (addr == 0x5555) {
      switch (v) {
        case 0x90: fId = true; break;
        case 0xF0: fId = false; break;
        case 0x80: fCmd = 0x80; break;
        case 0xA0: fCmd = 0xA0; break;
        case 0xB0: fCmd = 0xB0; break;
        default: break;
      }
    }
    return;
  }
  fState = 0;
}

// ---------------- EEPROM (serial, 1 bit por acesso) ----------------
void Save::dmaHint(int count, bool isWrite) {
  if (eAddrBits) return;
  if (isWrite) {
    if (count == 9 || count == 73) resizeEeprom(6);
    else if (count == 17 || count == 81) resizeEeprom(14);
  }
}

void Save::resizeEeprom(int bits) {
  eAddrBits = bits;
  if (bits == 6 && data.size() != 0x200) data.resize(0x200);
}

int Save::read16() {
  if (eReading) {
    const int bit = eReadBuf[static_cast<size_t>(eReadPos++)];
    if (eReadPos >= static_cast<int>(eReadBuf.size())) eReading = false;
    return bit;
  }
  return 1;
}

void Save::write16(int v) {
  eBits.push_back(static_cast<uint8_t>(v & 1));
  const std::vector<uint8_t>& bits = eBits;
  if (bits.size() < 2) return;
  const int ab = eAddrBits ? eAddrBits : 14;
  const int cmd = (bits[0] << 1) | bits[1];
  const size_t n = data.size();
  if (cmd == 3) {  // leitura
    if (bits.size() < static_cast<size_t>(2 + ab + 1)) return;
    int a = 0;
    for (int k = 0; k < ab; k++) a = (a << 1) | bits[static_cast<size_t>(2 + k)];
    a = (a & 0x3FF) * 8;
    eReadBuf.assign(4, 0);
    for (int k = 0; k < 8; k++) {
      const int byte = data[static_cast<size_t>(a + k) % n];
      for (int b = 7; b >= 0; b--) eReadBuf.push_back(static_cast<uint8_t>((byte >> b) & 1));
    }
    eReading = true; eReadPos = 0;
    eBits.clear();
  } else if (cmd == 2) {  // escrita
    if (bits.size() < static_cast<size_t>(2 + ab + 64 + 1)) return;
    int a = 0;
    for (int k = 0; k < ab; k++) a = (a << 1) | bits[static_cast<size_t>(2 + k)];
    a = (a & 0x3FF) * 8;
    for (int k = 0; k < 8; k++) {
      int byte = 0;
      for (int b = 0; b < 8; b++) byte = (byte << 1) | bits[static_cast<size_t>(2 + ab + k * 8 + b)];
      data[static_cast<size_t>(a + k) % n] = static_cast<uint8_t>(byte);
    }
    dirty = true;
    eBits.clear();
  } else {
    eBits.clear();
  }
}

void Save::sync(StateIO& io) {
  int t = static_cast<int>(type);
  io.sync(t);
  if (io.reading() && t != static_cast<int>(type)) throw std::runtime_error("Estado salvo incompatível com este jogo.");
  uint32_t n = static_cast<uint32_t>(data.size());
  io.sync(n);
  if (io.reading()) {
    const bool okSize = n == data.size() || (isEeprom && (n == 0x200 || n == 0x2000));
    if (!okSize) throw std::runtime_error("Estado salvo incompatível com este jogo.");
    data.resize(n);
  }
  io.raw(data.data(), data.size());
  io.sync(dirty);
  io.sync(fState); io.sync(fCmd); io.sync(fId); io.sync(fBank);
  uint32_t nb = static_cast<uint32_t>(eBits.size());
  io.sync(nb);
  if (io.reading()) { if (nb > 256) throw std::runtime_error("Estado salvo danificado."); eBits.resize(nb); }
  if (nb) io.raw(eBits.data(), nb);
  io.sync(eAddrBits); io.sync(eReading);
  uint32_t nr = static_cast<uint32_t>(eReadBuf.size());
  io.sync(nr);
  if (io.reading()) { if (nr > 256) throw std::runtime_error("Estado salvo danificado."); eReadBuf.resize(nr); }
  if (nr) io.raw(eReadBuf.data(), nr);
  io.sync(eReadPos);
  if (io.reading()) {
    for (auto& b : eBits) b &= 1;
    for (auto& b : eReadBuf) b &= 1;
    if (eAddrBits != 0 && eAddrBits != 6 && eAddrBits != 14) eAddrBits = 14;
    clampTo(fState, 0, 2);
    fBank = type == SaveType::Flash128 ? (fBank & 1) : 0;
    if (eReadBuf.empty()) eReading = false;
    clampTo(eReadPos, 0, std::max(0, static_cast<int>(eReadBuf.size()) - 1));
  }
}

// ------------------------------------------------------------ relógio do cartucho
namespace {
double g_fixedNow = 0;

double nowMs() {
  if (g_fixedNow != 0) return g_fixedNow;
  using namespace std::chrono;
  return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

// campos de data/hora locais de um instante em ms (como os getters de Date do JS)
bool localParts(double ms, std::tm& out) {
  if (!std::isfinite(ms)) return false;
  const std::time_t secs = static_cast<std::time_t>(std::floor(ms / 1000.0));
#ifdef _WIN32
  return localtime_s(&out, &secs) == 0;
#else
  return localtime_r(&secs, &out) != nullptr;
#endif
}

int bcd(int v) { return (((v / 10) % 10) << 4) | (v % 10); }
int unbcd(int b) { return (b >> 4) * 10 + (b & 0xF); }
enum { IDLE = 0, COMMAND = 1, READ = 2, WRITE = 3 };
}  // namespace

void setFixedNowMs(double ms) { g_fixedNow = ms; }

bool hasRtc(const std::vector<uint8_t>& rom, const std::string& gameCode) {
  static const char* const CODES[] = {"AXV", "AXP", "BPE", "U3I", "U32", "U33", "BR4"};
  const std::string g = gameCode.substr(0, 3);
  for (const char* c : CODES) if (g == c) return true;
  // assinatura da biblioteca de relógio da Nintendo
  static const uint8_t TAG[8] = {0x53, 0x49, 0x49, 0x52, 0x54, 0x43, 0x5F, 0x56};  // SIIRTC_V
  const int64_t len = static_cast<int64_t>(rom.size());
  for (int64_t i = 0; i < len - 8; i++) {
    if (rom[static_cast<size_t>(i)] != 0x53) continue;
    int k = 1;
    while (k < 8 && rom[static_cast<size_t>(i + k)] == TAG[k]) k++;
    if (k == 8) return true;
  }
  return false;
}

int CartRtc::read(int off) const {
  if (off == 0xC4) return ((pins & dir) | (out & ~dir)) & 0xF;
  if (off == 0xC6) return dir & 0xF;
  if (off == 0xC8) return ctrl & 1;
  return 0;
}

void CartRtc::write(int off, int v) {
  if (off == 0xC6) { dir = v & 0xF; return; }
  if (off == 0xC8) { ctrl = v & 1; return; }
  if (off != 0xC4) return;
  const int prev = pins;
  pins = (prev & ~dir) | ((v & dir) & 0xF);
  const int sck = pins & 1, sio = (pins >> 1) & 1, cs = (pins >> 2) & 1;
  if (!cs) { state = IDLE; return; }
  if (!(prev & 4)) { state = COMMAND; bits = 0; cmd = 0; return; }  // CS subiu: começa um comando
  if (!(!(prev & 1) && sck)) return;                                 // só a borda de subida do SCK move bits
  switch (state) {
    case COMMAND:
      cmd = (cmd << 1) | sio;
      if (++bits == 8) command();
      break;
    case READ: {
      const int b = bits;
      out = (out & ~2) | (((data[b >> 3] >> (b & 7)) & 1) << 1);
      if (++bits >= len * 8) state = IDLE;
      break;
    }
    case WRITE: {
      const int b = bits;
      if (sio) data[b >> 3] = static_cast<uint8_t>(data[b >> 3] | (1 << (b & 7)));
      if (++bits >= len * 8) { commit(); state = IDLE; }
      break;
    }
    default: break;
  }
}

void CartRtc::command() {
  int c = cmd & 0xFF;
  // alguns jogos mandam o comando com os bits invertidos (0110 no fim): normaliza
  if ((c >> 4) != 6 && (c & 0xF) == 6) {
    int r = 0;
    for (int k = 0; k < 8; k++) r |= ((c >> k) & 1) << (7 - k);
    c = r;
  }
  bits = 0;
  if ((c >> 4) != 6) { state = IDLE; return; }
  const int rg = (c >> 1) & 7, rd = c & 1;
  reg = rg;
  std::memset(data, 0, sizeof data);
  static const int LEN[8] = {0, 1, 7, 3, 0, 0, 0, 0};
  len = LEN[rg];
  if (rg == 0) { status = 0x40; offset = 0; state = IDLE; return; }  // reset
  if (!len) { state = IDLE; return; }
  if (rd) {
    if (rg == 1) data[0] = static_cast<uint8_t>(status);
    else fillTime(rg == 2);
    state = READ;
  } else {
    state = WRITE;
  }
}

void CartRtc::fillTime(bool withDate) {
  std::tm t{};
  if (!localParts(nowMs() + offset, t)) return;
  const int h = t.tm_hour;
  int hb = bcd(h);
  if (!(status & 0x40)) hb = bcd(h % 12) | (h >= 12 ? 0x80 : 0);
  if (withDate) {
    const int parts[7] = {bcd((t.tm_year + 1900) % 100), bcd(t.tm_mon + 1), bcd(t.tm_mday), t.tm_wday, hb, bcd(t.tm_min), bcd(t.tm_sec)};
    for (int k = 0; k < 7; k++) data[k] = static_cast<uint8_t>(parts[k]);
  } else {
    data[0] = static_cast<uint8_t>(hb); data[1] = static_cast<uint8_t>(bcd(t.tm_min)); data[2] = static_cast<uint8_t>(bcd(t.tm_sec));
  }
}

void CartRtc::commit() {
  if (reg == 1) { status = data[0] & 0x6A; return; }
  const uint8_t* d = data;
  std::tm now{};
  if (!localParts(nowMs() + offset, now)) return;
  int y = now.tm_year + 1900, mo = now.tm_mon, da = now.tm_mday, i = 0;
  if (reg == 2) { y = 2000 + unbcd(d[0]); mo = unbcd(d[1] & 0x1F) - 1; da = unbcd(d[2] & 0x3F); i = 4; }
  int h = unbcd(d[i] & 0x3F);
  if (!(status & 0x40) && (d[i] & 0x80)) h += 12;
  std::tm target{};
  target.tm_year = y - 1900; target.tm_mon = mo; target.tm_mday = da;
  target.tm_hour = h; target.tm_min = unbcd(d[i + 1] & 0x7F); target.tm_sec = unbcd(d[i + 2] & 0x7F);
  target.tm_isdst = -1;
  const std::time_t tt = std::mktime(&target);
  if (tt == static_cast<std::time_t>(-1)) return;
  offset = static_cast<double>(tt) * 1000.0 - nowMs();
}

void CartRtc::sync(StateIO& io) {
  io.sync(offset); io.sync(status);
  for (int* p : {&dir, &ctrl, &pins, &out, &state, &bits, &cmd, &len, &reg}) io.sync(*p);
  io.raw(data, sizeof data);
  if (io.reading()) {
    finiteOr(offset, 0); clampTo(offset, -1.0e13, 1.0e13);
    status &= 0xFF; dir &= 0xF; ctrl &= 1; pins &= 0xF; out &= 0xF;
    clampTo(state, 0, 3); clampTo(bits, 0, 63); clampTo(len, 0, 7); reg &= 7;
  }
}

}  // namespace indigo::gba
