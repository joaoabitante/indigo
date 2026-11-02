// Cartuchos do Atari 5200: reconhecimento (.a52, .car, .bin), tipos de mapeamento e troca de bancos.
#include <cstring>

#include "a5200.h"

namespace indigo::a5200 {

namespace {
bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// tipo do cabeçalho .car (numeração do Atari800) para os cartuchos de 5200; -1 = não é de 5200
int carKind(uint32_t t) {
  switch (t) {
    case 4: return Cart::K32;
    case 6: return Cart::EE16;
    case 7: return Cart::BB40;
    case 16: return Cart::NS16;
    case 19: return Cart::K8;
    case 20: return Cart::K4;
    case 71: case 72: case 73: case 74: return Cart::SUPER;
    default: return -1;
  }
}

bool isCar(const std::vector<uint8_t>& f) { return f.size() > 16 && std::memcmp(f.data(), "CART", 4) == 0; }
uint32_t carType(const std::vector<uint8_t>& f) {
  return (uint32_t(f[4]) << 24) | (uint32_t(f[5]) << 16) | (uint32_t(f[6]) << 8) | f[7];
}

bool sizeOk(size_t s) {
  return s == 0x1000 || s == 0x2000 || s == 0x4000 || s == 0x8000 || s == 0xA000 || s == 0x10000 ||
         s == 0x20000 || s == 0x40000 || s == 0x80000;
}

// 16 KB: um chip ($8000–$BFFF) ou dois chips ($4000 e $8000, cada um espelhado)? O código dos primeiros 8 KB
// chama e salta (JSR/JMP absolutos) para perto de si: conta os destinos em $4000–$7FFF e em $8000–$9FFF
bool looksTwoChip(const std::vector<uint8_t>& r) {
  int low = 0, high = 0;
  for (size_t i = 0; i + 2 < 0x2000; i++) {
    if (r[i] != 0x20 && r[i] != 0x4C) continue;
    const uint8_t hi = r[i + 2];
    if (hi >= 0x40 && hi < 0x80) low++;
    else if (hi >= 0x80 && hi < 0xA0) high++;
  }
  return low > high;
}
}  // namespace

bool isA5200Rom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (isCar(rom) && (endsWith(n, ".car") || endsWith(n, ".a52") || endsWith(n, ".bin"))) return carKind(carType(rom)) >= 0;
  const size_t s = rom.size();
  if (endsWith(n, ".a52")) return s > 0 && s <= 0x80000;
  if (!endsWith(n, ".bin") || !sizeOk(s) || s > 0xA000) return false;
  // .bin: o vetor de partida ($BFFE) fica nos últimos 2 bytes e aponta para dentro do cartucho; no Atari 2600
  // esses bytes são o vetor de IRQ, quase sempre na mesma página de 4 KB do vetor de reset (bytes -4 e -3)
  const uint8_t hi = rom[s - 1];
  const uint8_t lowest = s >= 0x8000 ? 0x40 : s == 0x4000 ? 0x40 : 0x80;
  if (hi < lowest || hi > 0xBF) return false;
  const uint8_t resetHi = rom[s - 3];
  if ((resetHi & 0x10) && (resetHi & 0xF0) == (hi & 0xF0)) return false;  // cara de 2600
  // o primeiro byte do código de partida tem de ser uma instrução comum de início (SEI, CLD, LDA, LDX, JMP...)
  const size_t a = size_t(hi) << 8 | rom[s - 2];
  size_t off;
  if (s == 0xA000) off = 0x8000 + (a & 0x1FFF);         // Bounty Bob: $8000–$BFFF = os últimos 8 KB
  else if (s == 0x8000) off = a - 0x4000;
  else if (s == 0x4000) off = a >= 0x8000 ? a - 0x8000 : (a & 0x1FFF);  // um chip, ou o primeiro de dois
  else off = a & (s - 1);
  static const uint8_t start[] = {0x78, 0xD8, 0xA9, 0xA2, 0xA0, 0x4C, 0x20, 0xAD, 0x8D, 0xEA, 0x18, 0x38, 0x58};
  return std::memchr(start, rom[off], sizeof start) != nullptr;
}

bool isA5200Bios(const std::vector<uint8_t>& data, const std::string& n) {
  return data.size() == 0x800 && n.find("5200") != std::string::npos && !endsWith(n, ".a52");
}

bool Cart::load(const std::vector<uint8_t>& file, const std::string& n, std::string& erro) {
  std::vector<uint8_t> r;
  int k = -1;
  if (isCar(file)) {
    k = carKind(carType(file));
    if (k < 0) { erro = "Este cartucho .car não é de Atari 5200."; return false; }
    r.assign(file.begin() + 16, file.end());
  } else {
    r = file;
  }
  size_t s = r.size();
  if (s == 0 || s > 0x80000) { erro = "Tamanho de cartucho de Atari 5200 inválido."; return false; }
  if (k < 0) {
    // imagem crua: o tamanho decide (os tamanhos fora do padrão são completados com espelhos)
    if (s <= 0x1000) k = K4;
    else if (s <= 0x2000) k = K8;
    else if (s <= 0x4000) k = NS16;
    else if (s <= 0x8000) k = K32;
    else if (s == 0xA000) k = BB40;
    else k = SUPER;
    (void)n;
  }
  const size_t want = k == K4 ? 0x1000 : k == K8 ? 0x2000 : (k == NS16 || k == EE16) ? 0x4000 : k == K32 ? 0x8000
                    : k == BB40 ? 0xA000 : [&] { size_t p = 0x10000; while (p < s) p <<= 1; return p; }();
  if (s < want) {
    // completa repetindo o conteúdo (um chip menor aparece espelhado no barramento)
    std::vector<uint8_t> big(want);
    for (size_t i = 0; i < want; i++) big[i] = r[i % s];
    r.swap(big);
  } else if (s > want) {
    r.erase(r.begin(), r.begin() + static_cast<long>(s - want));  // o fim (com os vetores) é o que vale
  }
  if (k == NS16 && looksTwoChip(r)) k = EE16;
  kind = static_cast<Kind>(k);
  rom = std::move(r);
  reset();
  // título: 20 caracteres em $BFE8 (códigos de tela do ANTIC; os 2 bits de cima são a cor). Nos super
  // cartuchos vem do último banco, que é onde a BIOS o lê (o acesso a $BFE0–$BFFF troca para ele).
  if (kind == SUPER) { super = static_cast<uint8_t>(rom.size() / 0x8000 - 1); map(); }
  title.clear();
  int letters = 0;
  for (int i = 0; i < 20; i++) {
    const uint8_t c = read(static_cast<uint16_t>(0xBFE8 + i)) & 0x3F;
    const char ch = static_cast<char>(c + 0x20);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) letters++;
    title += (ch >= 0x20 && ch < 0x60) ? ch : ' ';
  }
  while (!title.empty() && title.back() == ' ') title.pop_back();
  while (!title.empty() && title.front() == ' ') title.erase(title.begin());
  if (letters < 3) title.clear();
  reset();
  return true;
}

void Cart::reset() {
  bb[0] = 0; bb[1] = 0;
  super = 0;
  map();
}

void Cart::map() {
  const uint8_t* b = rom.data();
  auto set = [&](int from, int count, const uint8_t* p) {
    for (int i = 0; i < count; i++) page[from - 4 + i] = p + size_t(i) * 0x1000;
  };
  for (auto& p : page) p = nullptr;
  switch (kind) {
    case K4: for (int i = 8; i < 12; i++) page[i - 4] = b; break;
    case K8: set(8, 2, b); set(10, 2, b); break;
    case NS16: set(8, 4, b); break;
    case EE16: set(4, 2, b); set(6, 2, b); set(8, 2, b + 0x2000); set(10, 2, b + 0x2000); break;
    case K32: set(4, 8, b); break;
    case BB40:
      page[0] = b + size_t(bb[0] & 3) * 0x1000;
      page[1] = b + 0x4000 + size_t(bb[1] & 3) * 0x1000;
      set(8, 2, b + 0x8000);
      set(10, 2, b + 0x8000);
      break;
    case SUPER: set(4, 8, b + size_t(super) * 0x8000); break;
  }
}

void Cart::access(uint16_t a) {
  if (kind == BB40) {
    const int n = (a & 0xF) - 6;
    if (a < 0x5000) bb[0] = static_cast<uint8_t>(n); else bb[1] = static_cast<uint8_t>(n);
  } else {
    const uint8_t banks = static_cast<uint8_t>(rom.size() / 0x8000);
    uint8_t s = super;
    switch (a & 0x30) {
      case 0x00: s = static_cast<uint8_t>((s & 3) | (a & 0x0C)); break;          // $BFCx: bits altos
      case 0x10: s = static_cast<uint8_t>((s & 0x0C) | ((a & 0x0C) >> 2)); break;  // $BFDx: bits baixos
      default: s = 0x0F; break;                                                    // $BFEx/$BFFx: último
    }
    super = static_cast<uint8_t>(s & (banks - 1));
  }
  map();
}

void Cart::sync(StateIO& io) {
  io.raw(bb, sizeof bb);
  io.sync(super);
  if (io.reading()) {
    bb[0] &= 3; bb[1] &= 3;
    super = static_cast<uint8_t>(super & (rom.size() / 0x8000 > 0 ? rom.size() / 0x8000 - 1 : 0));
    map();
  }
}

}  // namespace indigo::a5200
