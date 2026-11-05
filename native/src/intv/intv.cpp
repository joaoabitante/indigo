#include "intv.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace indigo::intv {

namespace {
const char STATE_MAGIC[4] = {'I', 'N', 'T', '3'};
constexpr int64_t NEVER = INT64_MAX / 4;

// paleta do STIC (cores do jzIntv, medidas numa TV)
const uint8_t PALETTE[16][3] = {
  {0x00, 0x00, 0x00}, {0x00, 0x2D, 0xFF}, {0xFF, 0x3D, 0x10}, {0xC9, 0xCF, 0xAB},
  {0x38, 0x6B, 0x3F}, {0x00, 0xA7, 0x56}, {0xFA, 0xEA, 0x50}, {0xFF, 0xFC, 0xFF},
  {0xBD, 0xAC, 0xC8}, {0x24, 0xB8, 0xFF}, {0xFF, 0xB4, 0x1F}, {0x54, 0x6E, 0x00},
  {0xFF, 0x4E, 0x57}, {0xA4, 0x96, 0xFF}, {0x75, 0xCC, 0x80}, {0xB5, 0x1A, 0x58},
};

bool endsWith(const std::string& s, const char* suf) {
  const size_t n = std::strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

uint32_t crc32(const std::vector<uint8_t>& d) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : d) {
    c ^= b;
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
  }
  return ~c;
}

// .rom do Intellicart (jzIntv): byte 0 = $A8 (ou $41/$61), byte 1 = número de segmentos, byte 2 = complemento;
// cada segmento: página inicial e final (8 bits altos, inclusiva), palavras big-endian e CRC-16; depois
// 16 bytes de atributos (4 bits por bloco de 2K: leitura, escrita, 8 bits, troca de bancos), 32 bytes de faixa
// fina e CRC-16.
bool parseRom(const std::vector<uint8_t>& d, Cart* cart) {
  if (d.size() < 3 + 50) return false;
  if (!(d[0] == 0xA8 || (d[0] & ~0x20) == 0x41) || (d[1] ^ d[2]) != 0xFF) return false;
  size_t o = 3;
  const int n = d[1];
  std::vector<bool> loaded(cart ? 256 : 0, false);
  for (int s = 0; s < n; s++) {
    if (o + 2 > d.size()) return false;
    const int lo = d[o] << 8, hi = (d[o + 1] << 8) + 0x100;
    if (hi <= lo) return false;
    o += 2;
    const size_t bytes = size_t(hi - lo) * 2;
    if (o + bytes + 2 > d.size()) return false;
    if (cart) {
      for (int a = lo; a < hi; a++, o += 2) {
        cart->mem[a] = static_cast<uint16_t>(d[o] << 8 | d[o + 1]);
        cart->attr[a] |= 1;
      }
      for (int p = lo >> 8; p < (hi >> 8); p++) loaded[p] = true;
    } else {
      o += bytes;
    }
    o += 2;  // CRC
  }
  if (o + 50 > d.size()) return false;
  // depois da tabela pode vir uma área de "tags" (jzIntv novo): aceita, mas não usa
  if (cart) {
    for (int i = 0; i < 32; i++) {
      const int at = (d[o + (i >> 1)] >> ((i & 1) * 4)) & 0xF;
      const int f = d[o + 16 + ((i >> 1) | ((i & 1) << 4))];
      const int flo = (f >> 4) & 7, fhi = (f & 7) + 1;
      if (!at || fhi < flo) continue;
      for (int a = ((i << 3) + flo) << 8; a < ((i << 3) + fhi) << 8; a++) cart->attr[a] |= static_cast<uint8_t>(at);
    }
    // RAM do cartucho começa zerada (as páginas de escrita sem dados vieram como $FFFF)
    for (int p = 0; p < 256; p++) {
      if (loaded[p]) continue;
      for (int a = p << 8; a < (p << 8) + 256; a++)
        if (cart->attr[a] & 2) cart->mem[a] = 0;
    }
  }
  return true;
}

// cabeçalho da EXEC em $5000 num .bin (mapa padrão): seis endereços em dois bytes (BIDECLE); o início e o
// título têm de cair no cartucho
bool binHeader(const std::vector<uint8_t>& d, size_t* titleWord) {
  if (d.size() < 1024 || (d.size() & 1) || d.size() > 128 * 1024) return false;
  auto w = [&](size_t i) { return d[i * 2] << 8 | d[i * 2 + 1]; };
  for (size_t i = 0; i < 12; i++) if (w(i) > 0xFF) return false;
  const size_t words = d.size() / 2;
  auto toWord = [&](int a) -> long {
    long i = -1;
    if (a >= 0x5000 && a < 0x7000) i = a - 0x5000;
    else if (a >= 0xD000 && a < 0xE000) i = a - 0xD000 + 0x2000;
    else if (a >= 0xF000) i = a - 0xF000 + 0x3000;
    return i >= 0 && size_t(i) < words ? i : -1;
  };
  const long t = toWord(w(10) | w(11) << 8);
  if (toWord(w(4) | w(5) << 8) < 0 || t < 0) return false;
  if (titleWord) *titleWord = size_t(t);
  return true;
}

// .bin reconhecido pelo conteúdo: cabeçalho válido e, além disso, ou só palavras de 10 bits (os cartuchos da
// época: bytes altos com só os 2 bits de baixo), ou um título legível (ano e texto terminado em 0)
bool intvBin(const std::vector<uint8_t>& d) {
  size_t t = 0;
  if (!binHeader(d, &t)) return false;
  bool tenBit = true;
  size_t nonzero = 0;
  for (size_t i = 0; i < d.size(); i += 2) {
    if (d[i] & 0xFC) tenBit = false;
    if (d[i] || d[i + 1]) nonzero++;
  }
  if (tenBit && nonzero > d.size() / 16) return true;
  const size_t words = d.size() / 2;
  int letters = 0;
  for (size_t i = t + 1; i < words && i < t + 64; i++) {
    const int c = d[i * 2] << 8 | d[i * 2 + 1];
    if (c == 0) return letters >= 2;
    if (c < 0x20 || c > 0x7E) return false;
    letters++;
  }
  return false;
}
}  // namespace

// .cfg do jzIntv que o app achou ao lado do arquivo (só vale para .bin/.int/.itv)
std::string cfgFor(const std::string& n) {
  if (!endsWith(n, ".bin") && !endsWith(n, ".int") && !endsWith(n, ".itv")) return "";
  const std::string key = "intv_cfg";
  const std::vector<uint8_t>& c = firmware(key);
  return std::string(c.begin(), c.end());
}

bool isPalName(const std::string& n) {
  // o cartucho serve nas duas regiões (quem é PAL é o console): só "(pal)" no nome liga o PAL; "(europe)" não
  return n.find("(pal") != std::string::npos;
}

bool isIntvRom(const std::vector<uint8_t>& rom, const std::string& n) {
  if (rom.empty() || rom.size() > 2 * 1024 * 1024) return false;
  if (!(rom.size() & 1) && endsWith(n, ".bin")) {  // .bin com .cfg ao lado: o .cfg diz que é Intellivision
    const std::string cfg = cfgFor(n);
    Cart c;
    std::string err;
    if (!cfg.empty() && cfg.find("[mapping]") != std::string::npos && parseCfg(cfg, rom, c, err)) return true;
  }
  if (rom.size() > 256 * 1024) return false;
  if (endsWith(n, ".int") || endsWith(n, ".itv")) return (rom.size() & 1) == 0 || parseRom(rom, nullptr);
  if (endsWith(n, ".rom")) return parseRom(rom, nullptr);
  if (endsWith(n, ".bin")) return intvBin(rom);
  return false;
}

// "exec"/"grom" como palavra solta no nome ("exec.bin", "intv_exec.bin", "GROM (1).bin"),
// para "Executioner.bin" do 2600 não virar BIOS
static bool nameWord(const std::string& f, const char* w) {
  const size_t L = std::strlen(w);
  auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
  for (size_t p = f.find(w); p != std::string::npos; p = f.find(w, p + 1))
    if ((p == 0 || !letter(f[p - 1])) && (p + L >= f.size() || !letter(f[p + L]))) return true;
  return false;
}

int intvBiosKind(const std::vector<uint8_t>& d, const std::string& n) {
  const auto slash = n.find_last_of("/\\");
  std::string f = slash == std::string::npos ? n : n.substr(slash + 1);
  for (char& c : f) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  if (d.size() == 8192) {
    const uint32_t c = crc32(d);
    if (c == 0xCBCE86F7u) return 1;  // EXEC do Intellivision (outras versões: pelo nome do arquivo)
    if (nameWord(f, "exec") && endsWith(f, ".bin")) return 1;
  }
  if (d.size() == 2048) {
    if (crc32(d) == 0x683A4158u) return 2;
    if (nameWord(f, "grom") && endsWith(f, ".bin")) return 2;
  }
  return 0;
}

namespace {
// mapa padrão do jzIntv para .bin sem .cfg
void defaultMap(const std::vector<uint8_t>& d, Cart& cart) {
  const size_t words = d.size() / 2;
  struct Span { size_t from, len; int addr; };
  const Span spans[3] = {{0, 0x2000, 0x5000}, {0x2000, 0x1000, 0xD000}, {0x3000, 0x1000, 0xF000}};
  for (const Span& s : spans) {
    if (words <= s.from) break;
    const size_t len = std::min(s.len, words - s.from);
    for (size_t i = 0; i < len; i++) {
      const size_t b = (s.from + i) * 2;
      cart.mem[s.addr + i] = static_cast<uint16_t>(d[b] << 8 | d[b + 1]);
      cart.attr[s.addr + i] |= 1;
    }
  }
}

// número do .cfg: $hex, 0xhex ou decimal; -1 se não for número
long cfgNumber(const std::string& t) {
  size_t i = 0;
  int base = 10;
  if (!t.empty() && t[0] == '$') { i = 1; base = 16; }
  else if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { i = 2; base = 16; }
  if (i >= t.size() || t.size() - i > 8) return -1;
  long v = 0;
  for (; i < t.size(); i++) {
    const char c = t[i];
    int dg;
    if (c >= '0' && c <= '9') dg = c - '0';
    else if (base == 16 && c >= 'a' && c <= 'f') dg = c - 'a' + 10;
    else if (base == 16 && c >= 'A' && c <= 'F') dg = c - 'A' + 10;
    else return -1;
    v = v * base + dg;
  }
  return v;
}

std::string upper(std::string s) {
  for (char& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
  return s;
}

// separa a linha em palavras: números, "-", "=", nomes e "texto entre aspas"
std::vector<std::string> cfgTokens(const std::string& line) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i < line.size()) {
    const char c = line[i];
    if (c == ' ' || c == '\t' || c == '\r') { i++; continue; }
    if (c == ';') break;
    if (c == '-' || c == '=' || c == ',') { v.emplace_back(1, c); i++; continue; }
    if (c == '"') {
      const size_t e = line.find('"', i + 1);
      v.push_back(line.substr(i, (e == std::string::npos ? line.size() : e + 1) - i));
      i = e == std::string::npos ? line.size() : e + 1;
      continue;
    }
    size_t e = i;
    while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r' && line[e] != '-' &&
           line[e] != '=' && line[e] != ';' && line[e] != ',')
      e++;
    v.push_back(line.substr(i, e - i));
    i = e;
  }
  return v;
}
}  // namespace

// .cfg do jzIntv (doc/programming/intellicart.txt do jzIntv, mais o PAGE n das páginas do ECS):
//   [mapping]    $lo - $hi = $end [PAGE n]   palavras lo..hi do .bin no endereço end, legíveis
//   [preload]    $lo - $hi = $end [PAGE n]   só carrega (para a troca de bancos), sem atributo
//   [memattr]    $lo - $hi = RAM 8|16 [PAGE n] (ou WOM, ROM)
//   [bankswitch] $lo - $hi                   faixa que usa os registros $40-$5F do Intellicart
//   [vars]       name = "Título"
// Seções desconhecidas ([keys], [joystick], [macro]...) e linhas que não se entendem são ignoradas.
bool parseCfg(const std::string& cfg, const std::vector<uint8_t>& d, Cart& cart, std::string& err) {
  const size_t words = d.size() / 2;
  std::string sec;
  int mappings = 0, ignored = 0, badMaps = 0;
  size_t work = 0;
  constexpr size_t CFG_MAX_WORK = 64 * 65536;  // 4M palavras: bem mais que qualquer mapa de verdade
  std::vector<uint8_t> loadedPg(16 * 16 * 16, 0);  // páginas do ECS com dados, por bloco de 256
  std::vector<bool> loaded(65536, false);
  struct Attr { long lo, hi; uint8_t f; int page; };
  std::vector<Attr> attrs;
  size_t p = 0;
  while (p <= cfg.size()) {
    size_t e = cfg.find('\n', p);
    if (e == std::string::npos) e = cfg.size();
    const std::vector<std::string> t = cfgTokens(cfg.substr(p, e - p));
    p = e + 1;
    if (t.empty()) continue;
    if (t[0].size() >= 2 && t[0][0] == '[') {
      sec = upper(t[0].substr(1, t[0].find(']') == std::string::npos ? std::string::npos : t[0].find(']') - 1));
      continue;
    }
    if (sec == "VARS") {
      if (t.size() >= 3 && upper(t[0]) == "NAME" && t[1] == "=" && t[2].size() >= 2 && t[2][0] == '"') {
        std::string n = t[2].substr(1, t[2].size() - 2 + (t[2].back() == '"' ? 0 : 1));
        if (n.size() > 60) n.resize(60);
        cart.cfgTitle = n;
      }
      continue;
    }
    // "$lo - $hi" no começo
    if (t.size() < 3 || t[1] != "-") continue;
    const long lo = cfgNumber(t[0]), hi = cfgNumber(t[2]);
    if (lo < 0 || hi < lo || hi > 0xFFFFF) continue;
    // PAGE n: 0-15 (decimal, $hex) ou um dígito hex sozinho (A-F); PAGE inválido descarta a linha inteira,
    // para a faixa não cair no mapa direto por engano
    int page = -1;
    bool badPage = false;
    for (size_t k = 3; k < t.size(); k++)
      if (upper(t[k]) == "PAGE") {
        const std::string pt = k + 1 < t.size() ? upper(t[k + 1]) : "";
        long pg = cfgNumber(pt);
        if (pt.size() == 1 && pt[0] >= 'A' && pt[0] <= 'F') pg = pt[0] - 'A' + 10;
        if (pg >= 0 && pg < 16) page = static_cast<int>(pg);
        else badPage = true;
      }
    if (badPage) { ignored++; if (sec == "MAPPING") badMaps++; continue; }
    // trabalho total limitado: um .cfg com milhares de faixas de 64K não deixa o jogo demorar para abrir
    work += static_cast<size_t>(std::min<long>(hi - lo + 1, 0x10000));
    if (work > CFG_MAX_WORK) { err = "O .cfg do Intellivision tem faixas demais."; return false; }
    if (sec == "MAPPING" || sec == "PRELOAD") {
      if (t.size() < 5 || t[3] != "=") continue;
      const long at = cfgNumber(t[4]);
      if (at < 0 || at > 0xFFFF || hi - lo > 0xFFFF) continue;
      if (sec == "MAPPING") mappings++;
      if (page >= 0) cart.usePages();
      for (long o = lo; o <= hi && size_t(o) < words; o++) {
        const uint16_t w = static_cast<uint16_t>(d[o * 2] << 8 | d[o * 2 + 1]);
        const uint16_t a = static_cast<uint16_t>(at + (o - lo));
        if (page >= 0) {
          const size_t i = Cart::pidx(a >> 12, page, a & 0xFFF);
          cart.pmem[i] = w;
          if (sec == "MAPPING") { cart.pattr[i] |= 1; cart.pagedSegs |= static_cast<uint16_t>(1u << (a >> 12)); }
          loadedPg[((a >> 12) * 16 + page) * 16 + ((a >> 8) & 15)] = 1;
        } else {
          cart.mem[a] = w;
          if (sec == "MAPPING") cart.attr[a] |= 1;
          loaded[a] = true;
        }
      }
    } else if (sec == "MEMATTR") {
      if (t.size() < 5 || t[3] != "=" || hi > 0xFFFF) continue;
      const std::string k = upper(t[4]);
      uint8_t f = k == "RAM" ? 3 : k == "WOM" ? 2 : k == "ROM" ? 1 : 0;
      if (!f) continue;
      if (t.size() >= 6 && cfgNumber(t[5]) == 8) f |= 4;
      attrs.push_back({lo, hi, f, page});
    } else if (sec == "BANKSWITCH") {
      if (hi > 0xFFFF) continue;
      attrs.push_back({lo, hi, 8, -1});
    }
  }
  // .cfg só com [memattr]/[vars]: o .bin fica no mapa padrão (mas não se o [mapping] tinha só linhas inválidas)
  if (!mappings && !badMaps) defaultMap(d, cart);
  for (const Attr& x : attrs) {
    if (x.page >= 0) cart.usePages();
    for (long a = x.lo; a <= x.hi; a++) {
      if (x.page >= 0) {
        const size_t i = Cart::pidx(int(a >> 12), x.page, int(a & 0xFFF));
        cart.pattr[i] |= x.f;
        cart.pagedSegs |= static_cast<uint16_t>(1u << (a >> 12));
        if ((x.f & 2) && !loadedPg[((a >> 12) * 16 + x.page) * 16 + ((a >> 8) & 15)]) cart.pmem[i] = 0;
      } else {
        cart.attr[a] |= x.f;
        if ((x.f & 2) && !loaded[a]) cart.mem[a] = 0;  // RAM sem dados começa zerada
      }
    }
  }
  bool any = false;
  for (uint8_t f : cart.attr) if (f & 3) { any = true; break; }
  if (!any && !cart.pagedSegs) {
    err = ignored ? "O .cfg do Intellivision tem PAGE inválido (use 0-15 ou A-F)." : "O .cfg do Intellivision não mapeia nada do .bin.";
    return false;
  }
  if (ignored) cart.cfgWarn = std::to_string(ignored) + " linha(s) do .cfg com PAGE inválido foram ignoradas.";
  return true;
}

bool loadCart(const std::vector<uint8_t>& d, const std::string& n, Cart& cart, std::string& err, const std::string& cfg) {
  cart = Cart();
  if (d.empty()) { err = "Cartucho de Intellivision vazio."; return false; }
  if (parseRom(d, nullptr)) {
    parseRom(d, &cart);
    cart.format = "rom";
    return true;
  }
  if (endsWith(n, ".rom")) { err = "Arquivo .rom de Intellivision com cabeçalho inválido."; return false; }
  if (d.size() & 1) { err = "Cartucho de Intellivision com tamanho ímpar."; return false; }
  if (!cfg.empty()) {
    if (!parseCfg(cfg, d, cart, err)) return false;
    cart.format = "bin+cfg";
    return true;
  }
  if (d.size() / 2 > 0x4000) { err = "Cartucho .bin grande demais para o mapa padrão (precisaria do .cfg)."; return false; }
  defaultMap(d, cart);
  cart.format = "bin";
  return true;
}

namespace {
// título do cabeçalho ($500A/$500B aponta para: ano, texto, 0)
std::string cartTitle(const Cart& cart) {
  const auto rd = [&](int a) { return (cart.attr[a & 0xFFFF] & 1) ? cart.mem[a & 0xFFFF] : 0xFFFF; };
  const int ta = (rd(0x500A) & 0xFF) | ((rd(0x500B) & 0xFF) << 8);
  if (rd(0x500A) == 0xFFFF || !ta) return "";
  std::string t;
  for (int a = ta + 1; a < ta + 64; a++) {
    const int c = rd(a) & 0xFF;
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return "";
    t += static_cast<char>(c);
  }
  while (!t.empty() && t.back() == ' ') t.pop_back();
  return t;
}
}  // namespace

std::string intvTitle(const std::vector<uint8_t>& rom, const std::string& n) {
  Cart cart;
  std::string err;
  if (!loadCart(rom, n, cart, err, cfgFor(n))) return "";
  return cart.cfgTitle.empty() ? cartTitle(cart) : cart.cfgTitle;
}

// ================================================================== máquina
Intv::Intv(const Cart& cart, const std::vector<uint16_t>& exec, const std::vector<uint8_t>& grom, bool pal)
    : cart_(cart), exec_(exec), pal_(pal) {
  exec_.resize(4096, 0xFFFF);
  std::memcpy(stic.gmem, grom.data(), std::min<size_t>(grom.size(), 2048));
  initMem_ = cart_.mem;
  initPmem_ = cart_.pmem;
  for (uint8_t f : cart_.attr) if (f & 8) { icartBanks_ = true; break; }
  // blocos de 256 palavras que podem mudar (vão no save state): RAM direta; com a troca de bancos do
  // Intellicart, qualquer bloco (a escrita cai onde o registro mandar); RAM das páginas do ECS
  bool bankedRam = false;
  for (uint8_t f : cart_.attr) if ((f & 10) == 10) { bankedRam = true; break; }
  for (int p = 0; p < 256; p++) {
    bool w = bankedRam;
    for (int a = p << 8; a < (p << 8) + 256 && !w; a++) w = cart_.attr[a] & 2;
    if (w) ramBlocks_.push_back(p);
  }
  for (size_t b = 0; b < cart_.pattr.size() / 256; b++) {
    bool w = false;
    for (size_t i = b * 256; i < b * 256 + 256 && !w; i++) w = cart_.pattr[i] & 2;
    if (w) pageRamBlocks_.push_back(static_cast<uint32_t>(b));
  }
  reset();
}

void Intv::reset() {
  // RAM do cartucho volta à imagem do arquivo (zerada onde não veio nada)
  cart_.mem = initMem_;
  cart_.pmem = initPmem_;
  for (int b = 0; b < 32; b++) bank_[b] = static_cast<uint8_t>(b << 3);  // mapa direto até o programa mudar
  std::memset(curPage_, 0, sizeof curPage_);
  std::memset(scratch, 0, sizeof scratch);
  std::memset(sysram, 0, sizeof sysram);
  std::memset(btab_, 0, sizeof btab_);
  stic.reset();
  psg.reset();
  cpu.reset();
  now = 0;
  phase_ = 0;
  nextPhase_ = 57;
  vidEnable_ = frameReady_ = false;
  displayOn = false;
  cpRow_ = 0;
  sticAcc_ = gmemAcc_ = NEVER;
  intak_ = 0;
  intrq_ = busrq_ = false;
  intrqUntil_ = busrqUntil_ = 0;
}

// fases do STIC dentro do quadro (jzIntv, stic.c): 0 = começo do retraço (imagem e interrupção),
// 1 = registros fecham, 2..15 = busca das fileiras do BACKTAB (pedidos de barramento).
// NTSC: linha de 57 ciclos, fileira de cartões = 16 linhas (912). PAL: linha de 64 ciclos (fileira de 1024);
// as buscas e as bordas da parte visível duram o mesmo (são do STIC) e o que sobra do quadro vai para o retraço.
void Intv::phaseEvent() {
  const int64_t t = nextPhase_;
  const int hd = stic.reg[0x30] & 7, vd = stic.reg[0x31] & 7;
  const int line = pal_ ? 64 : 57, row = 16 * line;
  const int vb1 = pal_ ? FRAME_CYCLES_PAL - (120 + 12 * row + 74) - 896 : 2900, vb2 = vb1 + 896;
  int len = 0, next = (phase_ + 1) & 15;
  switch (phase_) {
    case 0:
      if (vidEnable_) stic.render(btab_);
      displayOn = vidEnable_;
      frameReady_ = true;
      frames++;
      intrq_ = true;
      intrqUntil_ = t + vb1 + 7;
      if (vidEnable_) intak_ = NEVER;  // registros só abrem depois que a CPU atender a interrupção
      stic.enableWrite = false;
      cpRow_ = 0;
      sticAcc_ = t + vb1;
      gmemAcc_ = t + vb2;
      len = vb1;
      break;
    case 1:
      len = vb2 - vb1;
      vidEnable_ = stic.enableWrite;
      stic.enableWrite = false;
      if (!vidEnable_) sticAcc_ = gmemAcc_ = NEVER;
      break;
    case 2:
      len = 120 + 2 * line * vd + hd;
      if (vidEnable_) { busrq_ = true; busrqUntil_ = t + 68; }
      break;
    case 15:
      if (vidEnable_ && vd == 0) { busrq_ = true; busrqUntil_ = t + 38; }
      len = 57 + 17;
      next = 0;
      break;
    default:  // 3..14
      len = phase_ == 14 ? row - 2 * line * vd - hd : row;
      if (vidEnable_) { busrq_ = true; busrqUntil_ = t + 108; }
      break;
  }
  nextPhase_ = t + len;
  phase_ = next;
}

void Intv::runFrame() {
  frameReady_ = false;
  const int64_t limit = now + 2 * frameCycles();
  while (now < limit) {
    while (now >= nextPhase_) phaseEvent();
    if (frameReady_) break;
    if (intrq_ && (cpu.intr & 1)) {
      intrq_ = false;
      if (now <= intrqUntil_) {
        now += cpu.interrupt();
        intak_ = now;
        continue;
      }
    }
    if (busrq_ && (cpu.intr & 2)) {
      busrq_ = false;
      if (now <= busrqUntil_) {
        if (cpRow_ < 12) std::memcpy(&btab_[cpRow_ * 20], &sysram[cpRow_ * 20], 20 * sizeof(uint16_t));
        cpRow_++;
        now = busrqUntil_;
        continue;
      }
    }
    now += cpu.step();
  }
  psg.run(now);
}

// ------------------------------------------------------------------ barramento
uint16_t Intv::sticRead(uint16_t a) {
  if (!sticOpen(now + 4)) return static_cast<uint16_t>(a < 0x80 ? (a & 0x0E) : 0xFFFF);
  if (a < 0x40) return stic.readReg(a);
  return stic.gmem[0x800 + (a & 0x3F)];  // $40-$7F: amostra da GRAM
}

void Intv::sticWrite(uint16_t a, uint16_t v) {
  if (!sticOpen(now + 4)) return;
  stic.writeReg(a & 0x3F, v);
}

// cartucho: primeiro a página do ECS visível no segmento, depois o mapa direto (ou pelos bancos do Intellicart)
uint16_t Intv::cartRead(uint16_t a, bool* hit) {
  const int seg = a >> 12;
  if ((cart_.pagedSegs >> seg) & 1) {
    const size_t i = Cart::pidx(seg, curPage_[seg], a & 0xFFF);
    const uint8_t f = cart_.pattr[i];
    if (f & 1) { *hit = true; return (f & 4) ? (cart_.pmem[i] & 0xFF) : cart_.pmem[i]; }
  }
  const uint8_t f = cart_.attr[a];
  if (!(f & 1)) { *hit = false; return 0xFFFF; }
  *hit = true;
  const uint16_t ia = (f & 8) ? static_cast<uint16_t>((a & 0x7FF) + (bank_[a >> 11] << 8)) : a;
  return (f & 4) ? (cart_.mem[ia] & 0xFF) : cart_.mem[ia];
}

// devolve true se o cartucho aceitou a escrita como RAM
bool Intv::cartWrite(uint16_t a, uint16_t v) {
  const int seg = a >> 12;
  // troca de página do ECS: $xA5y em $xFFF
  if ((a & 0xFFF) == 0xFFF && (v & 0x0FF0) == 0x0A50 && (v >> 12) == seg) curPage_[seg] = v & 15;
  if ((cart_.pagedSegs >> seg) & 1) {
    const size_t i = Cart::pidx(seg, curPage_[seg], a & 0xFFF);
    const uint8_t f = cart_.pattr[i];
    if (f & 2) { cart_.pmem[i] = (f & 4) ? (v & 0xFF) : v; return true; }
  }
  const uint8_t f = cart_.attr[a];
  if (!(f & 2)) return false;
  const uint16_t ia = (f & 8) ? static_cast<uint16_t>((a & 0x7FF) + (bank_[a >> 11] << 8)) : a;
  cart_.mem[ia] = (f & 4) ? (v & 0xFF) : v;
  return true;
}

uint16_t Intv::read(uint16_t a) {
  if (a < 0x80) return sticRead(a);
  if (a >= 0x100 && a < 0x1F0) return scratch[a - 0x100];
  if (a >= 0x1F0 && a < 0x200) {
    const int r = a & 15;
    if (r == 14) return (psg.r_[8] & 0x40) ? psg.r_[14] : static_cast<uint8_t>(~padR);
    if (r == 15) return (psg.r_[8] & 0x80) ? psg.r_[15] : static_cast<uint8_t>(~padL);
    return psg.read(r);
  }
  if (a >= 0x200 && a < 0x360) return sysram[a - 0x200];
  if (a >= 0x1000 && a < 0x2000) return exec_[a - 0x1000];
  if (a >= 0x3000 && a < 0x4000) {
    if (!gmemOpen(now + 4)) return a & 0x3FFF;
    return (a & 0x800) ? stic.gmem[0x800 + (a & 0x1FF)] : stic.gmem[a & 0x7FF];
  }
  bool hit;
  const uint16_t v = cartRead(a, &hit);
  if (hit) return v;
  if (a >= 0x4000 && (a & 0x3FFF) < 0x40) {  // espelho dos registros do STIC: lê $FFFF, mas $21 ainda vale
    if ((a & 0x3F) == 0x21 && sticOpen(now + 4)) stic.readReg(0x21);
    return 0xFFFF;
  }
  return 0xFFFF;
}

void Intv::write(uint16_t a, uint16_t v) {
  if (a < 0x40) { sticWrite(a, v); return; }
  if (a < 0x60 && icartBanks_) {  // registros de banco do Intellicart: $40 + bloco de 4K, $50 = metade de cima
    const int r = a - 0x40;
    bank_[((r & 15) << 1) | (r >> 4)] = static_cast<uint8_t>(v);
    return;
  }
  if (a >= 0x100 && a < 0x1F0) { scratch[a - 0x100] = static_cast<uint8_t>(v); return; }
  if (a >= 0x1F0 && a < 0x200) { psg.write(a & 15, static_cast<uint8_t>(v), now); return; }
  if (a >= 0x200 && a < 0x360) { sysram[a - 0x200] = v; return; }
  const bool ram = cartWrite(a, v);
  // GRAM em $3800 (e espelhos em $7800, $B800, $F800); o STIC também atende em $4000, $8000, $C000
  // ROM do cartucho na faixa (mapa direto ou a página do ECS visível) bloqueia o espelho
  const int seg = a >> 12;
  const bool rom = (cart_.attr[a] & 1) ||
                   (((cart_.pagedSegs >> seg) & 1) && (cart_.pattr[Cart::pidx(seg, curPage_[seg], a & 0xFFF)] & 1));
  if ((a & 0x3800) == 0x3800 && ((a >> 14) == 0 || !rom)) {
    if (gmemOpen(now + 4)) stic.gmem[0x800 + (a & 0x1FF)] = static_cast<uint8_t>(v);
    return;
  }
  if (a >= 0x4000 && (a & 0x3FFF) < 0x40 && !ram) sticWrite(a & 0x3F, v);
}

// ------------------------------------------------------------------ estado
void Intv::sync(StateIO& io) {
  char magic[4];
  std::memcpy(magic, STATE_MAGIC, 4);
  io.raw(magic, 4);
  if (io.reading() && std::memcmp(magic, STATE_MAGIC, 4) != 0)
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  // forma do cartucho e da máquina: um estado de outro cartucho ou de outra região é recusado
  uint32_t shape[3] = {static_cast<uint32_t>(ramBlocks_.size()), static_cast<uint32_t>(pageRamBlocks_.size()),
                       static_cast<uint32_t>((pal_ ? 1 : 0) | (icartBanks_ ? 2 : 0) | (uint32_t(cart_.pagedSegs) << 8))};
  uint32_t got[3] = {shape[0], shape[1], shape[2]};
  io.raw(got, sizeof got);
  if (io.reading() && std::memcmp(got, shape, sizeof shape) != 0)
    throw std::runtime_error("Estado salvo incompatível com este jogo.");
  cpu.sync(io);
  stic.sync(io);
  psg.sync(io);
  io.raw(scratch, sizeof scratch);
  io.raw(sysram, sizeof sysram);
  io.raw(btab_, sizeof btab_);
  for (int p : ramBlocks_) io.raw(&cart_.mem[size_t(p) << 8], 256 * sizeof(uint16_t));
  for (uint32_t b : pageRamBlocks_) io.raw(&cart_.pmem[size_t(b) << 8], 256 * sizeof(uint16_t));
  io.raw(bank_, sizeof bank_);
  io.raw(curPage_, sizeof curPage_);
  io.sync(now); io.sync(frames);
  io.sync(phase_); io.sync(nextPhase_); io.sync(vidEnable_); io.sync(displayOn); io.sync(cpRow_);
  io.sync(sticAcc_); io.sync(gmemAcc_); io.sync(intak_);
  io.sync(intrq_); io.sync(busrq_); io.sync(intrqUntil_); io.sync(busrqUntil_);
  io.sync(padL); io.sync(padR);
  if (io.reading()) {
    const int64_t fc = frameCycles();
    for (uint8_t& p : curPage_) p &= 15;
    clampTo<int64_t>(now, 0, int64_t(1) << 52);
    clampTo(phase_, 0, 15);
    clampTo<int64_t>(nextPhase_, now - 1, now + fc);
    clampTo(cpRow_, 0, 16);
    // prazos do estado: um valor absurdo faria o pedido de barramento pular o relógio para longe
    // (valores no passado são inofensivos: só entram em comparações)
    intrqUntil_ = std::min<int64_t>(intrqUntil_, now + fc);
    busrqUntil_ = std::min<int64_t>(busrqUntil_, now + fc);
    for (int64_t* t : {&sticAcc_, &gmemAcc_, &intak_})
      if (*t > now + fc) *t = NEVER;
    psg.clampClock(now);
  }
}

std::vector<uint8_t> Intv::saveState() {
  StateIO io;
  sync(io);
  return io.data();
}

void Intv::loadState(const std::vector<uint8_t>& s) {
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

// ================================================================== adaptador
IntvCore::IntvCore(const std::vector<uint8_t>& rom, const std::string& n) : fb_(Stic::OUT_W * 2 * Stic::OUT_H, 0xFF000000u) {
  Cart cart;
  std::string err;
  if (!loadCart(rom, n, cart, err, cfgFor(n))) throw std::runtime_error(err);
  const std::vector<uint8_t> fe = firmware("intv_exec");
  const std::vector<uint8_t> fg = firmware("intv_grom");
  std::vector<uint16_t> exec;
  std::vector<uint8_t> grom;
  if (fe.size() == 8192) {
    exec.resize(4096);
    for (int i = 0; i < 4096; i++) exec[i] = static_cast<uint16_t>(fe[i * 2] << 8 | fe[i * 2 + 1]);
  } else {
    exec = freeExec();
  }
  grom = fg.size() == 2048 ? fg : freeGrom();
  if (fe.size() != 8192 || fg.size() != 2048) {
    // cartucho só com palavras de 10 bits = jogo da época (Mattel, Activision, Imagic...), que usa as rotinas e a
    // fonte da EXEC/GROM originais: sem a BIOS do usuário não roda, então avisa em vez de abrir uma tela errada
    bool tenBit = true;
    for (int a = 0; a < 65536 && tenBit; a++)
      if ((cart.attr[a] & 3) == 1 && cart.mem[a] != 0xFFFF && cart.mem[a] >= 0x400) tenBit = false;
    for (size_t i = 0; i < cart.pattr.size() && tenBit; i++)
      if ((cart.pattr[i] & 3) == 1 && cart.pmem[i] != 0xFFFF && cart.pmem[i] >= 0x400) tenBit = false;
    if (tenBit)
      throw std::runtime_error("Este cartucho de Intellivision precisa da BIOS do console. Abra uma vez no Índigo o "
                               "exec.bin e o grom.bin do seu Intellivision (ficam guardados para os próximos jogos).");
    note_ = "Intellivision sem a BIOS do console: rodando com a EXEC e a GROM livres do Índigo (só jogos caseiros). "
            "Para os jogos comerciais, abra uma vez o exec.bin e o grom.bin do seu Intellivision.";
  }
  m_ = std::make_unique<Intv>(cart, exec, grom, isPalName(n));
  title_ = cart.cfgTitle.empty() ? cartTitle(cart) : cart.cfgTitle;
  if (m_->pal()) note_ += std::string(note_.empty() ? "" : " ") + "Intellivision PAL (50 Hz), pelo \"(PAL)\" no nome do arquivo.";
  if (!cart.cfgWarn.empty()) note_ += std::string(note_.empty() ? "" : " ") + cart.cfgWarn;
  applyKeys();
}

// bits do controle (1 = apertado): disco, botões laterais e teclado
uint8_t IntvCore::padBits(uint32_t k, int keypad) {
  uint8_t p = 0;
  const bool u = k & KEY_UP, d = k & KEY_DOWN, l = k & KEY_LEFT, r = k & KEY_RIGHT;
  if (u && r) p |= 0x16;
  else if (u && l) p |= 0x1C;
  else if (d && r) p |= 0x13;
  else if (d && l) p |= 0x19;
  else if (u && !d) p |= 0x04;
  else if (d && !u) p |= 0x01;
  else if (r && !l) p |= 0x02;
  else if (l && !r) p |= 0x08;
  if (k & KEY_X) p |= 0xA0;  // botões de cima
  if (k & KEY_B) p |= 0x60;  // baixo à esquerda
  if (k & KEY_A) p |= 0xC0;  // baixo à direita
  static const uint8_t KEYS[12] = {0x48, 0x81, 0x41, 0x21, 0x82, 0x42, 0x22, 0x84, 0x44, 0x24, 0x88, 0x28};
  if (k & KEY_START) p |= KEYS[11];
  if (k & KEY_SELECT) p |= KEYS[10];
  if (k & KEY_L) p |= KEYS[1];
  if (k & KEY_R) p |= KEYS[2];
  if (k & KEY_Y) p |= KEYS[0];
  if (keypad >= 0 && keypad < 12) p |= KEYS[keypad];
  return p;
}

// tecla do teclado numérico pelos bits KEY_KP* (teclado numérico do PC e teclado na tela do app):
// 0-9, * = Clear (10) e # = Enter (11), como no 5200; -1 = nenhuma
int IntvCore::keypadFromKeys(uint32_t k) {
  for (int i = 0; i < 10; i++)
    if (k & (KEY_KP0 << i)) return i;
  if (k & KEY_KPSTAR) return 10;
  if (k & KEY_KPHASH) return 11;
  return -1;
}

void IntvCore::applyKeys() {
  const uint8_t p = padBits(keys_, keypad_ >= 0 ? keypad_ : keypadFromKeys(keys_));
  const uint8_t p2 = padBits(keys2_, keypadFromKeys(keys2_));
  m_->padL = swap_ ? p2 : p;
  m_->padR = swap_ ? p : p2;
}

void IntvCore::runFrame() {
  m_->runFrame();
  present();
}

// cada ponto do STIC vira 2×1 na saída (352×224); tela desligada: a última imagem mais escura
void IntvCore::present() {
  const int W = Stic::OUT_W, H = Stic::OUT_H;
  const bool on = m_->displayOn;
  uint32_t pal[16];
  for (int i = 0; i < 16; i++) {
    uint32_t r = PALETTE[i][0], g = PALETTE[i][1], b = PALETTE[i][2];
    if (!on) { r >>= 1; g >>= 1; b >>= 1; }
    pal[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
  }
  for (int y = 0; y < H; y++) {
    const uint8_t* s = &m_->stic.out[y * W];
    uint32_t* o = &fb_[y * W * 2];
    for (int x = 0; x < W; x++) o[2 * x] = o[2 * x + 1] = pal[s[x] & 15];
  }
}

}  // namespace indigo::intv
