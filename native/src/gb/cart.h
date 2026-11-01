// Cartucho do Game Boy / Game Boy Color (porte de src/gb/cart.js): cabeçalho, MBC1 (com multicart),
// MBC2, MBC3 com relógio, MBC5; save da bateria (RAM + relógio no formato de 48 bytes).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../core/state.h"

namespace indigo::gb {

enum class Mbc { None, Mbc1, Mbc2, Mbc3, Mbc5 };

struct CartInfo {
  std::string title;
  bool cgb = false, cgbOnly = false;
  int type = 0;
  Mbc mbc = Mbc::None;
  int ramSize = 0;
  bool battery = false, rtc = false, rumble = false;
  std::string code;
};

CartInfo cartInfo(const std::vector<uint8_t>& rom);

class Cart {
public:
  explicit Cart(const std::vector<uint8_t>& rom);

  CartInfo info;
  std::vector<uint8_t> rom, ram;
  bool dirty = false;
  bool rumble = false;

  int readRom(int a) const { return a < 0x4000 ? rom[rom0 | a] : rom[romX | (a & 0x3FFF)]; }
  void writeRom(int a, int v);
  int readRam(int a);
  void writeRam(int a, int v);

  bool hasSave() const;
  std::vector<uint8_t> exportSave();
  void importSave(const std::vector<uint8_t>& bytes);

  void sync(StateIO& io);

private:
  void update();
  bool sameLogo(size_t off) const;
  void rtcTick();
  void addSeconds(int64_t secs);

  int romBanks = 2, ramBanks = 1;
  bool ramOn = false;
  int bank1 = 1, bank2 = 0, mode = 0;
  int romBank = 1, ramBank = 0;
  int rom0 = 0, romX = 0x4000, ramOff = 0;
  bool multicart = false;
  int32_t rtc[5] = {}, rtcLatched[5] = {};
  bool rtcLatchArm = false;
  double rtcStamp = 0;  // milissegundos desde a época (como Date.now())
  double rtcSub = 0;
};

}  // namespace indigo::gb
