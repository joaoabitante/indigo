#include "cart.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace indigo::gb {

namespace {

const int RAM_SIZES[] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};

bool isBattery(int t) {
  switch (t) {
    case 0x03: case 0x06: case 0x09: case 0x0D: case 0x0F: case 0x10: case 0x13: case 0x1B: case 0x1E: case 0xFF:
      return true;
    default:
      return false;
  }
}

double nowMs() {
  using namespace std::chrono;
  return static_cast<double>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void putU32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
uint32_t getU32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

}  // namespace

CartInfo cartInfo(const std::vector<uint8_t>& rom) {
  CartInfo i;
  const int cgbFlag = rom[0x143];
  const int titleEnd = (cgbFlag & 0x80) ? 0x143 : 0x144;
  std::string title;
  for (int k = 0x134; k < titleEnd; k++) {
    const int c = rom[k];
    if (!c) break;
    if (c >= 0x20 && c < 0x7F) title += static_cast<char>(c);
  }
  // trim
  const auto b = title.find_first_not_of(" \t\r\n");
  const auto e = title.find_last_not_of(" \t\r\n");
  i.title = b == std::string::npos ? "" : title.substr(b, e - b + 1);

  const int type = rom[0x147];
  i.type = type;
  if (type >= 0x01 && type <= 0x03) i.mbc = Mbc::Mbc1;
  else if (type == 0x05 || type == 0x06) i.mbc = Mbc::Mbc2;
  else if (type >= 0x0F && type <= 0x13) i.mbc = Mbc::Mbc3;
  else if (type >= 0x19 && type <= 0x1E) i.mbc = Mbc::Mbc5;
  else if (type == 0xFF) i.mbc = Mbc::Mbc1;  // HuC1 se comporta como MBC1 para os jogos
  const int rs = rom[0x149];
  i.ramSize = rs < 6 ? RAM_SIZES[rs] : 0;
  if (i.mbc == Mbc::Mbc2) i.ramSize = 512;
  if (type == 0x08 || type == 0x09) i.ramSize = i.ramSize ? i.ramSize : 0x2000;
  const unsigned code = rom[0x14D] | (rom[0x14E] << 8) | (rom[0x14F] << 16);
  char buf[16];
  std::snprintf(buf, sizeof buf, "%06X", code);
  i.code = buf;
  i.cgb = cgbFlag == 0x80 || cgbFlag == 0xC0;
  i.cgbOnly = cgbFlag == 0xC0;
  i.battery = isBattery(type);
  i.rtc = type == 0x0F || type == 0x10;
  i.rumble = type >= 0x1C && type <= 0x1E;
  return i;
}

Cart::Cart(const std::vector<uint8_t>& src) {
  // ROM com tamanho potência de 2 (mínimo 32 KB) para as máscaras de banco
  size_t size = 0x8000;
  while (size < src.size()) size <<= 1;
  rom.assign(size, 0xFF);
  std::copy(src.begin(), src.end(), rom.begin());
  romBanks = static_cast<int>(size >> 14);
  info = cartInfo(rom);
  ram.assign(std::max(info.ramSize, 0), 0xFF);
  ramBanks = std::max(1, info.ramSize >> 13);
  // MBC1 multicart (coletâneas de 1 MB): o logo da Nintendo se repete no início do 2º jogo (banco 0x10)
  multicart = info.mbc == Mbc::Mbc1 && size == 0x100000 && sameLogo(0x40104);
  rtcStamp = nowMs();
  update();
}

void Cart::update() {
  const int mask = romBanks - 1;
  switch (info.mbc) {
    case Mbc::Mbc1: {
      const int sh = multicart ? 4 : 5, b1 = multicart ? bank1 & 0xF : bank1;
      rom0 = ((mode ? (bank2 << sh) : 0) & mask) << 14;
      romX = (((bank2 << sh) | b1) & mask) << 14;
      ramOff = ((mode ? bank2 : 0) % ramBanks) << 13;
      break;
    }
    case Mbc::Mbc2:
      rom0 = 0; romX = (romBank & mask) << 14; ramOff = 0;
      break;
    case Mbc::Mbc3:
      rom0 = 0; romX = ((romBank ? romBank : 1) & mask) << 14;
      ramOff = ((ramBank & 3) % ramBanks) << 13;
      break;
    case Mbc::Mbc5:
      rom0 = 0; romX = (romBank & mask) << 14;
      ramOff = ((ramBank & (info.rumble ? 7 : 0xF)) % ramBanks) << 13;
      break;
    default:
      rom0 = 0; romX = 0x4000 & ((romBanks << 14) - 1); ramOff = 0;
      break;
  }
}

bool Cart::sameLogo(size_t off) const {
  if (off + 48 > rom.size()) return false;
  for (size_t k = 0; k < 48; k++) if (rom[off + k] != rom[0x104 + k]) return false;
  return true;
}

void Cart::writeRom(int a, int v) {
  switch (info.mbc) {
    case Mbc::Mbc1:
      if (a < 0x2000) ramOn = (v & 0xF) == 0xA;
      else if (a < 0x4000) bank1 = (v & 0x1F) ? (v & 0x1F) : 1;
      else if (a < 0x6000) bank2 = v & 3;
      else mode = v & 1;
      break;
    case Mbc::Mbc2:
      if (a < 0x4000) {
        if (a & 0x100) romBank = (v & 0xF) ? (v & 0xF) : 1;
        else ramOn = (v & 0xF) == 0xA;
      }
      break;
    case Mbc::Mbc3:
      if (a < 0x2000) ramOn = (v & 0xF) == 0xA;
      else if (a < 0x4000) romBank = (v & 0x7F) ? (v & 0x7F) : 1;
      else if (a < 0x6000) ramBank = v & 0xF;
      else {
        if (v == 0) rtcLatchArm = true;
        else if (v == 1 && rtcLatchArm) {
          rtcTick();
          std::copy(rtc, rtc + 5, rtcLatched);
          rtcLatchArm = false;
        } else rtcLatchArm = false;
      }
      break;
    case Mbc::Mbc5:
      if (a < 0x2000) ramOn = (v & 0xF) == 0xA;
      else if (a < 0x3000) romBank = (romBank & 0x100) | v;
      else if (a < 0x4000) romBank = (romBank & 0xFF) | ((v & 1) << 8);
      else if (a < 0x6000) { ramBank = v & 0xF; if (info.rumble) rumble = (v & 8) != 0; }
      break;
    default:
      break;
  }
  update();
}

int Cart::readRam(int a) {
  if (!ramOn) return 0xFF;
  if (info.mbc == Mbc::Mbc2) return ram[a & 0x1FF] | 0xF0;
  if (info.mbc == Mbc::Mbc3 && ramBank >= 8) {
    if (!info.rtc || ramBank > 0xC) return 0xFF;
    static const int MASK[] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
    static const int OR[] = {0xC0, 0xC0, 0xE0, 0, 0x3E};
    const int k = ramBank - 8;
    return (rtcLatched[k] & MASK[k]) | OR[k];
  }
  if (ram.empty()) return 0xFF;
  return ram[(ramOff | (a & 0x1FFF)) % ram.size()];
}

void Cart::writeRam(int a, int v) {
  if (!ramOn) return;
  if (info.mbc == Mbc::Mbc2) { ram[a & 0x1FF] = v & 0xF; dirty = true; return; }
  if (info.mbc == Mbc::Mbc3 && ramBank >= 8) {
    if (!info.rtc || ramBank > 0xC) return;
    rtcTick();
    const int k = ramBank - 8;
    static const int MASK[] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
    rtc[k] = v & MASK[k];
    if (k == 0) rtcSub = 0;
    rtcLatched[k] = rtc[k];
    dirty = true;
    return;
  }
  if (ram.empty()) return;
  ram[(ramOff | (a & 0x1FFF)) % ram.size()] = static_cast<uint8_t>(v);
  dirty = true;
}

// avança o relógio do cartucho pelo tempo real decorrido (como a bateria faria)
void Cart::rtcTick() {
  if (!info.rtc) return;
  const double now = nowMs();
  double ms = now - rtcStamp;
  rtcStamp = now;
  if (ms <= 0 || (rtc[4] & 0x40)) return;
  ms += rtcSub;
  const int64_t secs = static_cast<int64_t>(std::floor(ms / 1000));
  rtcSub = std::fmod(ms, 1000);
  addSeconds(secs);
}

void Cart::addSeconds(int64_t secs) {
  int32_t* r = rtc;
  while (secs > 0) {
    const int64_t step = std::min<int64_t>(secs, 86400 * 30);
    secs -= step;
    int64_t s = r[0] + step;
    int64_t m = r[1] + s / 60; s %= 60;
    int64_t h = r[2] + m / 60; m %= 60;
    int64_t d = (((r[4] & 1) << 8) | r[3]) + h / 24; h %= 24;
    int32_t hi = r[4] & 0xC0;
    if (d > 511) { d &= 511; hi |= 0x80; }
    r[0] = int32_t(s); r[1] = int32_t(m); r[2] = int32_t(h); r[3] = int32_t(d & 0xFF); r[4] = hi | int32_t(d >> 8);
  }
}

bool Cart::hasSave() const { return info.battery && (!ram.empty() || info.rtc); }

std::vector<uint8_t> Cart::exportSave() {
  const size_t rtcBytes = info.rtc ? 48 : 0;
  std::vector<uint8_t> out(ram.size() + rtcBytes, 0);
  std::copy(ram.begin(), ram.end(), out.begin());
  if (rtcBytes) {
    rtcTick();
    uint8_t* p = out.data() + ram.size();
    for (int k = 0; k < 5; k++) { putU32(p + k * 4, rtc[k]); putU32(p + 20 + k * 4, rtcLatched[k]); }
    const double t = std::floor(rtcStamp / 1000);
    putU32(p + 40, static_cast<uint32_t>(static_cast<uint64_t>(t)));
    putU32(p + 44, static_cast<uint32_t>(std::floor(t / 4294967296.0)));
  }
  return out;
}

void Cart::importSave(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return;
  std::copy(bytes.begin(), bytes.begin() + std::min(bytes.size(), ram.size()), ram.begin());
  if (info.rtc && bytes.size() >= ram.size() + 44) {
    const uint8_t* p = bytes.data() + ram.size();
    for (int k = 0; k < 5; k++) { rtc[k] = getU32(p + k * 4) & 0xFF; rtcLatched[k] = getU32(p + 20 + k * 4) & 0xFF; }
    const double t = getU32(p + 40) + (bytes.size() >= ram.size() + 48 ? getU32(p + 44) * 4294967296.0 : 0);
    rtcStamp = t > 0 ? t * 1000 : nowMs();
    rtcSub = 0;
    rtcTick();  // conta o tempo em que o jogo ficou desligado
  }
}

void Cart::sync(StateIO& io) {
  io.bytes(ram);
  io.raw(rtc, sizeof rtc);
  io.raw(rtcLatched, sizeof rtcLatched);
  io.sync(rtcStamp);
  io.sync(ramOn); io.sync(bank1); io.sync(bank2); io.sync(mode); io.sync(romBank); io.sync(ramBank);
  if (io.reading()) {
    bank1 &= 0x1F; bank2 &= 3; mode &= 1; romBank &= 0x1FF; ramBank &= 0xF;
    finiteOr(rtcStamp, nowMs()); if (rtcStamp < 0) rtcStamp = nowMs();
    update();
  }
}

}  // namespace indigo::gb
