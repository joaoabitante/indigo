// Cartucho do Lynx: formatos de arquivo, porta do cartucho (contador + página), EEPROM 93Cxx e a imitação da
// ROM de partida (decifra o carregador do cartucho).
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "lynx.h"

namespace indigo::lynx {

namespace {

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

bool hasLynxHeader(const std::vector<uint8_t>& f) {
  return f.size() >= 64 && f[0] == 'L' && f[1] == 'Y' && f[2] == 'N' && f[3] == 'X';
}
bool hasBs93(const std::vector<uint8_t>& f) {
  return f.size() >= 10 && f[6] == 'B' && f[7] == 'S' && f[8] == '9' && f[9] == '3';
}

// ---------------------------------------------------------------- RSA da ROM (expoente 3, módulo público)
constexpr int CHUNK = 51;
const uint8_t PUBLIC_MOD[CHUNK] = {
    0x35, 0xB5, 0xA3, 0x94, 0x28, 0x06, 0xD8, 0xA2, 0x26, 0x95, 0xD7, 0x71, 0xB2, 0x3C, 0xFD, 0x56, 0x1C,
    0x4A, 0x19, 0xB6, 0xA3, 0xB0, 0x26, 0x00, 0x36, 0x5A, 0x30, 0x6E, 0x3C, 0x4D, 0x63, 0x38, 0x1B, 0xD4,
    0x1C, 0x13, 0x64, 0x89, 0x36, 0x4C, 0xF2, 0xBA, 0x2A, 0x58, 0xF4, 0xFE, 0xE1, 0xFD, 0xAC, 0x7E, 0x79};

// números de 51 bytes, o mais significativo primeiro
bool geq(const uint8_t* a, const uint8_t* b) {
  for (int i = 0; i < CHUNK; i++) if (a[i] != b[i]) return a[i] > b[i];
  return true;
}
void subInPlace(uint8_t* a, const uint8_t* b) {
  int borrow = 0;
  for (int i = CHUNK - 1; i >= 0; i--) {
    int t = a[i] - b[i] - borrow;
    borrow = t < 0;
    a[i] = static_cast<uint8_t>(t);
  }
}
void addInPlace(uint8_t* a, const uint8_t* b) {
  int carry = 0;
  for (int i = CHUNK - 1; i >= 0; i--) {
    int t = a[i] + b[i] + carry;
    carry = t >> 8;
    a[i] = static_cast<uint8_t>(t);
  }
}
// r = m * n (mod PUBLIC_MOD), bit a bit do mais significativo (como a ROM faz)
void mulMod(uint8_t* r, const uint8_t* m, const uint8_t* n) {
  std::memset(r, 0, CHUNK);
  for (int i = 0; i < CHUNK; i++) {
    for (int bit = 7; bit >= 0; bit--) {
      int carry = 0;
      for (int k = CHUNK - 1; k >= 0; k--) {
        const int t = r[k] * 2 + carry;
        r[k] = static_cast<uint8_t>(t);
        carry = t >> 8;
      }
      if ((n[i] >> bit) & 1) {
        addInPlace(r, m);
        if (geq(r, PUBLIC_MOD)) {
          subInPlace(r, PUBLIC_MOD);
          if (geq(r, PUBLIC_MOD)) subInPlace(r, PUBLIC_MOD);
        }
      } else if (geq(r, PUBLIC_MOD)) {
        subInPlace(r, PUBLIC_MOD);
      }
    }
  }
}

}  // namespace

bool isLynxRom(const std::vector<uint8_t>& rom, const std::string& nome) {
  if (hasLynxHeader(rom)) return true;
  if (endsWith(nome, ".lyx")) return rom.size() >= 256;
  if (endsWith(nome, ".o") || endsWith(nome, ".lnx")) return hasBs93(rom) || hasLynxHeader(rom);
  return false;
}

// ---------------------------------------------------------------- cartucho
uint8_t Cart::read(int b, bool useA) {
  const std::vector<uint8_t>& d = useA ? bankA[b] : bank[b];
  uint8_t v = 0xFF;
  if (mask[b]) {
    const uint32_t a = ((static_cast<uint32_t>(shifter) << shiftBits[b]) + (counter & ((1u << shiftBits[b]) - 1))) & mask[b];
    if (a < d.size()) v = d[a];
  }
  if (!strobe) counter = (counter + 1) & 0x7FF;
  return v;
}

void Cart::write(int, bool, uint8_t) {
  if (!strobe) counter = (counter + 1) & 0x7FF;
}

// ---------------------------------------------------------------- EEPROM
void Eeprom::configure(uint8_t type) {
  eightBit = (type & 0x80) != 0;
  switch (type & 7) {
    case 1: addrMask = 0x7F; cmdBits = 10; addrBits = 7; break;    // 93C46
    case 2: addrMask = 0xFF; cmdBits = 12; addrBits = 9; break;    // 93C56
    case 3: addrMask = 0x1FF; cmdBits = 12; addrBits = 9; break;   // 93C66
    case 4: addrMask = 0x3FF; cmdBits = 14; addrBits = 11; break;  // 93C76
    case 5: addrMask = 0x7FF; cmdBits = 14; addrBits = 11; break;  // 93C86
    default: addrMask = 0; cmdBits = 1; addrBits = 1; return;
  }
  if (!eightBit) { addrMask >>= 1; cmdBits--; addrBits--; }
}

void Eeprom::reset() {
  state = IDLE; busyCount = 0; writeEnabled = false; dout = true; lastClk = false;
  bitsLeft = 0; shift = 0; readData = 0; addr = 0;
}

uint16_t Eeprom::word(uint16_t a) const {
  if (eightBit) return data[a & 0x7FF];
  const size_t i = (a & 0x3FF) * 2u;
  return static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
}
void Eeprom::setWord(uint16_t a, uint16_t v) {
  if (eightBit) { if (data[a & 0x7FF] != (v & 0xFF)) dirty = true; data[a & 0x7FF] = static_cast<uint8_t>(v); return; }
  const size_t i = (a & 0x3FF) * 2u;
  if (word(a) != v) dirty = true;
  data[i] = static_cast<uint8_t>(v);
  data[i + 1] = static_cast<uint8_t>(v >> 8);
}

bool Eeprom::outputBit() {
  if (state == BUSY || state == IDLE) {
    // depois de uma gravação, o chip fica ocupado (DO baixo) por um tempo e depois avisa que terminou
    if (busyCount < 2) { busyCount++; dout = false; readData = 0; }
    else { dout = true; readData = 0xFFFF; state = WAIT; }
  }
  return dout;
}

void Eeprom::counterChanged(uint16_t counter, uint8_t iodir, uint8_t iodat) {
  if (!present()) return;
  const bool clk = counter & 0x02;
  const bool rising = clk && !lastClk;
  lastClk = clk;
  if (!rising) return;
  const uint32_t doneMask = eightBit ? 0x100 : 0x10000;
  dout = (readData & (doneMask >> 1)) != 0;
  readData <<= 1;
  const bool cs = counter & 0x80;
  const bool out = iodir & 0x10;
  const bool di = out && (iodat & 0x10);
  if (!cs) state = IDLE;
  switch (state) {
    case IDLE:
      shift = 0;
      if (cs) {
        if (di) { dout = false; state = CMD; bitsLeft = cmdBits - 1; }
        else if (!out) { state = BUSY; readData = 0; dout = false; busyCount = 0; }
      }
      break;
    case CMD: {
      shift = (shift << 1) | (di ? 1 : 0);
      if (--bitsLeft > 0) break;
      state = IDLE;
      addr = static_cast<uint16_t>(shift & addrMask);
      const uint32_t opc = shift >> addrBits;
      if (opc == 3) {  // ERASE
        if (writeEnabled) setWord(addr, eightBit ? 0xFF : 0xFFFF);
      } else if (opc == 2) {  // READ
        readData = word(addr);
        dout = false;
        state = WAIT;
      } else if (opc == 1) {  // WRITE: os dados vêm a seguir
        shift = 1;
        state = DATA;
      } else {
        const uint32_t sub = shift >> (addrBits - 2);
        if (sub == 0) writeEnabled = false;       // EWDS
        else if (sub == 3) writeEnabled = true;   // EWEN
        else if (sub == 2 && writeEnabled) {      // ERAL
          for (uint16_t a = 0; a <= addrMask; a++) setWord(a, eightBit ? 0xFF : 0xFFFF);
        } else if (sub == 1) {                    // WRAL (aceito sem gravar, como o Handy)
        }
      }
      break;
    }
    case DATA:
      shift = (shift << 1) | (di ? 1 : 0);
      if (shift & doneMask) {
        state = WAIT;
        if (writeEnabled) setWord(addr, static_cast<uint16_t>(shift & (eightBit ? 0xFF : 0xFFFF)));
        busyCount = 0;
        readData = 0;
        dout = false;
      }
      break;
    default: break;
  }
}

void Eeprom::sync(StateIO& io) {
  io.bytes(data);
  io.sync(state); io.sync(busyCount); io.sync(writeEnabled); io.sync(dout); io.sync(lastClk);
  io.sync(bitsLeft); io.sync(shift); io.sync(readData); io.sync(addr);
  if (io.reading()) {
    if (state > BUSY) state = IDLE;
    bitsLeft = std::clamp(bitsLeft, 0, 16);
  }
}

// ---------------------------------------------------------------- carga do arquivo
Lynx::Lynx(const std::vector<uint8_t>& f, const std::vector<uint8_t>* save) : cpu(*this) {
  std::memset(ram, 0, sizeof ram);
  std::memset(rom, 0xFF, sizeof rom);
  if (hasBs93(f) && !hasLynxHeader(f)) {
    homebrew = true;
    homebrewLoad = static_cast<uint16_t>(((f[2] << 8) | f[3]) - 10);
    homebrewImage = f;
    title = "";
  } else {
    size_t off = 0;
    uint32_t page0 = 0, page1 = 0;
    if (hasLynxHeader(f)) {
      off = 64;
      page0 = f[4] | (f[5] << 8);
      page1 = f[6] | (f[7] << 8);
      std::string n(reinterpret_cast<const char*>(&f[10]), 32);
      n = n.substr(0, n.find('\0'));
      while (!n.empty() && n.back() == ' ') n.pop_back();
      cart.name = n;
      cart.rotation = f[58] <= 2 ? f[58] : 0;
      cart.audin = f[59] & 1;
      cart.eepromType = f[60];
    } else {
      // imagem crua: um banco só, páginas de tamanho/256
      page0 = static_cast<uint32_t>(f.size() >> 8);
    }
    auto bits = [](uint32_t page) {
      switch (page) {
        case 0x100: return 8;
        case 0x200: return 9;
        case 0x400: return 10;
        case 0x800: return 11;
        default: return 0;
      }
    };
    cart.shiftBits[0] = bits(page0);
    cart.shiftBits[1] = bits(page1);
    if (!cart.shiftBits[0]) {
      // tamanho fora do padrão: escolhe o menor banco onde o arquivo cabe
      const size_t len = f.size() - off;
      cart.shiftBits[0] = len <= 0x10000 ? 8 : len <= 0x20000 ? 9 : len <= 0x40000 ? 10 : 11;
    }
    const uint8_t* p = f.data() + off;
    size_t left = f.size() - off;
    for (int b = 0; b < 2; b++) {
      if (!cart.shiftBits[b]) continue;
      cart.mask[b] = (256u << cart.shiftBits[b]) - 1;
      const size_t sz = cart.mask[b] + 1;
      cart.bank[b].assign(sz, 0xFF);
      const size_t n = std::min(left, sz);
      std::copy(p, p + n, cart.bank[b].begin());
      p += n; left -= n;
    }
    if (cart.audin) {
      for (int b = 0; b < 2; b++) {
        if (!cart.mask[b]) continue;
        cart.bankA[b].assign(cart.mask[b] + 1, 0xFF);
        const size_t n = std::min(left, cart.bankA[b].size());
        std::copy(p, p + n, cart.bankA[b].begin());
        p += n; left -= n;
      }
    }
    if (cart.bank[0].empty() || f.size() - off < 52) throw std::runtime_error("Arquivo do Lynx sem dados de cartucho.");
    title = cart.name == "NO HEADER" ? "" : cart.name;
    eeprom.configure(cart.eepromType);
    if (save && eeprom.present()) {
      std::copy(save->begin(), save->begin() + std::min(save->size(), eeprom.data.size()), eeprom.data.begin());
    }
  }
  // sem a ROM: conteúdo pseudoaleatório fixo (há demos que leem a ROM como semente) e os vetores do Handy
  uint32_t seed = 0x1234567u;
  for (int i = 0; i < 0x1F8; i++) { seed = seed * 1103515245u + 12345u; rom[i] = static_cast<uint8_t>(seed >> 16); }
  rom[0x1F8] = 0x00; rom[0x1F9] = 0x80; rom[0x1FA] = 0x00; rom[0x1FB] = 0x30;
  rom[0x1FC] = 0x80; rom[0x1FD] = 0xFF; rom[0x1FE] = 0x80; rom[0x1FF] = 0xFF;
  const std::vector<uint8_t> fw = firmware("lynx");
  if (fw.size() == 512 && !homebrew) {
    std::memcpy(rom, fw.data(), 512);
    realBoot = true;
  }
  fb.assign(SCREEN_W * SCREEN_H, 0xFF000000u);
  if (cart.rotation) { outW = SCREEN_H; outH = SCREEN_W; }
  reset();
}

// ---------------------------------------------------------------- imitação da ROM de partida
// lê o primeiro quadro cifrado do cartucho (a partir da posição atual), decifra e grava a partir de ($05)
void Lynx::hleLoad() {
  uint16_t dest = static_cast<uint16_t>(ram[5] | (ram[6] << 8));
  const uint8_t first = cart.read(0, false);
  const int blocks = 256 - first;
  uint8_t acc = 0;
  for (int b = 0; b < blocks && b < 5; b++) {
    uint8_t enc[CHUNK], num[CHUNK], sq[CHUNK], cube[CHUNK];
    for (int i = 0; i < CHUNK; i++) enc[i] = cart.read(0, false);
    for (int i = 0; i < CHUNK; i++) num[CHUNK - 1 - i] = enc[i];  // o bloco vem com o byte menos significativo antes
    mulMod(sq, num, num);
    mulMod(cube, num, sq);
    for (int i = CHUNK - 1; i > 0; i--) {
      acc = static_cast<uint8_t>(acc + cube[i]);
      ram[dest++] = acc;
    }
  }
  ram[5] = static_cast<uint8_t>(dest);
  ram[6] = static_cast<uint8_t>(dest >> 8);
  cpu.pc = 0x0200;
}

// estado do hardware como a ROM deixa ao pular para o carregador (o que os demos de 249 bytes esperam);
// valores conferidos no emulador Felix (licença MIT), que documenta a saída da ROM
void Lynx::hleBoot() {
  std::memset(ram, 0, sizeof ram);
  mapctl = 0x08;
  // temporizadores de 60 Hz: linha (T0, 1 µs, 159 contagens) e quadro (T2, ligado a T0, 105 linhas)
  writeCtla(0, 0);
  tim[0].bkup = 0x9E;
  writeCtla(0, 0x18);
  tim[2].bkup = 0x68;
  writeCtla(2, 0x1F);
  dispctl = 0x0D;
  dispadr = 0x2000;
  pbkup = 0x29;
  // paleta: a ROM só mexe nas cores 0 (preto) e 15
  green[15] = 0x0E;
  bluered[15] = 0x3E;
  iodir = 0x03;
  sysctl1 = 0x02;
  cpu.a = 0; cpu.x = 0; cpu.y = 2; cpu.s = 0xFD;
  cpu.p = Cpu65SC02<Lynx>::I | Cpu65SC02<Lynx>::U | Cpu65SC02<Lynx>::Z;
  if (homebrew) {
    const size_t n = std::min<size_t>(homebrewImage.size(), 0x10000 - homebrewLoad);
    std::memcpy(ram + homebrewLoad, homebrewImage.data(), n);
    cpu.pc = homebrewLoad;
    return;
  }
  cart.shifter = 0;
  cart.counter = 0;
  ram[5] = 0x00;
  ram[6] = 0x02;
  hleLoad();
  ram[0x20] = ram[5];  // a ROM deixa o mesmo valor em $20 (o demo timewarp depende disso)
}

// chamada a uma rotina da ROM sem a ROM: imita o efeito e volta
void Lynx::hleCall(uint16_t pc) {
  auto rts = [&] {
    cpu.s++;
    const uint8_t lo = ram[0x100 | cpu.s];
    cpu.s++;
    cpu.pc = static_cast<uint16_t>((lo | (ram[0x100 | cpu.s] << 8)) + 1);
  };
  switch (pc) {
    case 0xFE00:  // seleciona a página do cartucho (A)
      cart.shifter = cpu.a;
      cart.counter = 0;
      rts();
      break;
    case 0xFE19:
    case 0xFF80:
      hleBoot();
      break;
    case 0xFE4A:
      hleLoad();
      break;
    default:
      rts();
      break;
  }
  now += 40;
}

}  // namespace indigo::lynx
