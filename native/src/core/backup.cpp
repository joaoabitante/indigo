#include "backup.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../gamecom/gamecom.h"
#include "../nes/nes.h"
#include "../fds/fds.h"
#include "../snes/snes.h"
#include "../pce/pce.h"
#include "../pm/pm.h"
#include "../tms/coleco.h"
#include "../vectrex/vectrex.h"
#include "../intv/intv.h"
#include "../tms/sg1000.h"
#include "../ws/ws.h"
#include "../md/md.h"
#include "../msx/msx.h"
#include "../a2600/a2600.h"
#include "../a5200/a5200.h"
#include "../a7800/a7800.h"
#include "../jaguar/jaguar.h"
#include "../cps/cps.h"
#include "../neogeo/neogeo.h"
#include "../gnw/gnw.h"
#include "safe.h"
#include "zip.h"
#include "../lynx/lynx.h"
#include "../vb/vb.h"
#include "../gb/gb.h"
#include "../svision/svision.h"
#include "../ngp/ngp.h"
#include "../sms/sms.h"

namespace indigo::backup {

// ------------------------------------------------------------ JSON
const Json* Json::get(const std::string& k) const {
  for (const auto& [key, v] : o) if (key == k) return &v;
  return nullptr;
}
Json& Json::set(const std::string& k, Json v) {
  for (auto& [key, cur] : o) if (key == k) { cur = std::move(v); return cur; }
  o.emplace_back(k, std::move(v));
  return o.back().second;
}
std::string Json::str(const std::string& k, const std::string& def) const {
  const Json* v = get(k);
  return v && v->type == Str ? v->s : def;
}
double Json::num(const std::string& k, double def) const {
  const Json* v = get(k);
  return v && v->type == Num ? v->n : def;
}
bool Json::flag(const std::string& k) const {
  const Json* v = get(k);
  return v && ((v->type == Bool && v->b) || (v->type == Num && v->n != 0));
}

namespace {
struct Parser {
  const std::string& t;
  size_t i = 0;
  int depth = 0;
  void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; }
  static void utf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += static_cast<char>(c);
    else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }
  bool hex4(uint32_t& v) {
    if (i + 4 > t.size()) return false;
    v = 0;
    for (int k = 0; k < 4; k++) {
      const char c = t[i++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }
  bool str(std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    i++;
    while (i < t.size()) {
      const char c = t[i++];
      if (c == '"') return true;
      if (c != '\\') { out += c; continue; }
      if (i >= t.size()) return false;
      const char e = t[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t c1;
          if (!hex4(c1)) return false;
          if (c1 >= 0xD800 && c1 < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
            i += 2;
            uint32_t c2;
            if (!hex4(c2)) return false;
            if (c2 >= 0xDC00 && c2 < 0xE000) c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
            else { utf8(out, 0xFFFD); c1 = c2; }
          }
          utf8(out, c1);
          break;
        }
        default: return false;
      }
    }
    return false;
  }
  bool value(Json& v) {
    if (++depth > 200) return false;
    ws();
    if (i >= t.size()) return false;
    const char c = t[i];
    bool ok = true;
    if (c == '{') {
      v = Json::object();
      i++; ws();
      if (i < t.size() && t[i] == '}') { i++; }
      else for (;;) {
        ws();
        std::string k;
        if (!str(k)) { ok = false; break; }
        ws();
        if (i >= t.size() || t[i] != ':') { ok = false; break; }
        i++;
        Json x;
        if (!value(x)) { ok = false; break; }
        v.o.emplace_back(std::move(k), std::move(x));
        ws();
        if (i < t.size() && t[i] == ',') { i++; continue; }
        if (i < t.size() && t[i] == '}') { i++; break; }
        ok = false; break;
      }
    } else if (c == '[') {
      v = Json::array();
      i++; ws();
      if (i < t.size() && t[i] == ']') { i++; }
      else for (;;) {
        Json x;
        if (!value(x)) { ok = false; break; }
        v.a.push_back(std::move(x));
        ws();
        if (i < t.size() && t[i] == ',') { i++; continue; }
        if (i < t.size() && t[i] == ']') { i++; break; }
        ok = false; break;
      }
    } else if (c == '"') {
      v = Json::string("");
      ok = str(v.s);
    } else if (t.compare(i, 4, "true") == 0) { v = Json::boolean(true); i += 4; }
    else if (t.compare(i, 5, "false") == 0) { v = Json::boolean(false); i += 5; }
    else if (t.compare(i, 4, "null") == 0) { v = Json(); i += 4; }
    else {
      const char* start = t.c_str() + i;
      char* end = nullptr;
      const double d = std::strtod(start, &end);
      if (end == start) ok = false;
      else { v = Json::number(d); i += static_cast<size_t>(end - start); }
    }
    depth--;
    return ok;
  }
};

void writeStr(std::string& out, const std::string& s) {
  out += '"';
  for (unsigned char c : s) {
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
    else out += static_cast<char>(c);
  }
  out += '"';
}

void writeVal(std::string& out, const Json& j) {
  switch (j.type) {
    case Json::Null: out += "null"; break;
    case Json::Bool: out += j.b ? "true" : "false"; break;
    case Json::Num: {
      if (!std::isfinite(j.n)) { out += "null"; break; }
      char b[40];
      if (j.n == std::floor(j.n) && std::fabs(j.n) < 9007199254740992.0) std::snprintf(b, sizeof b, "%.0f", j.n);
      else std::snprintf(b, sizeof b, "%.17g", j.n);
      out += b;
      break;
    }
    case Json::Str: writeStr(out, j.s); break;
    case Json::Arr:
      out += '[';
      for (size_t k = 0; k < j.a.size(); k++) { if (k) out += ','; writeVal(out, j.a[k]); }
      out += ']';
      break;
    case Json::Obj:
      out += '{';
      for (size_t k = 0; k < j.o.size(); k++) {
        if (k) out += ',';
        writeStr(out, j.o[k].first);
        out += ':';
        writeVal(out, j.o[k].second);
      }
      out += '}';
      break;
  }
}
}  // namespace

bool parseJson(const std::string& text, Json& out) {
  Parser p{text};
  if (!p.value(out)) return false;
  p.ws();
  return p.i == text.size();
}

std::string writeJson(const Json& j) {
  std::string out;
  writeVal(out, j);
  return out;
}

// ------------------------------------------------------------ identificação
namespace {
std::string lower(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  return s;
}
bool ends(const std::string& s, const char* e) {
  const size_t l = std::strlen(e);
  return s.size() >= l && s.compare(s.size() - l, l, e) == 0;
}
int at(const std::vector<uint8_t>& b, size_t k) { return k < b.size() ? b[k] : -1; }  // -1 = undefined do JS
void latin1(std::string& s, int c) {
  if (c < 0x80) s += static_cast<char>(c);
  else { s += static_cast<char>(0xC0 | (c >> 6)); s += static_cast<char>(0x80 | (c & 0x3F)); }
}
// String.prototype.trim do JS para texto vindo de bytes Latin-1 (guardado em UTF-8: 1 ou 2 bytes por letra)
std::string jsTrim(const std::string& s) {
  std::vector<std::pair<size_t, int>> cps;  // (posição, código)
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0xC0 && i + 1 < s.size()) { cps.push_back({i, ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F)}); i += 2; }
    else { cps.push_back({i, c}); i++; }
  }
  auto ws = [](int c) { return (c >= 9 && c <= 13) || c == 32 || c == 0xA0; };
  size_t b = 0, e = cps.size();
  while (b < e && ws(cps[b].second)) b++;
  while (e > b && ws(cps[e - 1].second)) e--;
  if (b == e) return "";
  const size_t from = cps[b].first, to = e < cps.size() ? cps[e].first : s.size();
  return s.substr(from, to - from);
}

std::string detectSystem(const std::vector<uint8_t>& b, const std::string& name) {
  const std::string n = lower(name);
  const bool gbLogo = b.size() >= 0x150 && b[0x104] == 0xCE && b[0x105] == 0xED && b[0x106] == 0x66 && b[0x107] == 0x66;
  const bool gbaLogo = b.size() >= 0xC0 && b[4] == 0x24 && b[5] == 0xFF && b[6] == 0xAE && b[7] == 0x51;
  if (zip::isZip(b) && cps::isCpsSet(b, n)) return "cps";  // arcade: o .zip inteiro é o jogo
  if (!(zip::isZip(b) && neogeo::isNeoGeoBiosZip(b, n)) && neogeo::isNeoGeoRom(b, n)) return "neogeo";
  if (zip::isZip(b) && gnw::isGnwSet(b)) return "gnw";  // Game & Watch: .zip com o desenho .svg da tela
  if (gamecom::isGamecomRom(b, n)) return "gamecom";
  if (jaguar::isJaguarRom(b, n)) return "jaguar";
  if (nes::isNesRom(b)) return "nes";
  if (!fds::isFdsBios(b, n) && fds::isFdsDisk(b, n)) return "fds";
  if (snes::isSnesRom(b, n)) return "snes";
  if (ngp::isNgpRom(b)) return ngp::ngpInfo(b).color ? "ngpc" : "ngp";
  if (const int dk = gb::duckKind(b, n)) return dk == 2 ? "duck0" : "duck";  // Mega Duck (o .md0 tem outro mapeador)
  if (md::isMdRom(b, n)) return "md";
  if (pm::isPMRom(b, n)) return "pm";
  if (pce::isPceRom(b, n)) return ends(n, ".sgx") ? "sgx" : "pce";
  if (sv::isSvRom(b, n)) return "svision";
  if (vb::isVbRom(b, n)) return "vb";
  if (lynx::isLynxRom(b, n)) return "lynx";
  if (vectrex::isVectrexRom(b, n)) return "vectrex";
  if (a7800::isA7800Rom(b, n)) return "a7800";
  if (ws::isWsRom(b, n)) return (b.size() >= 10 && (b[b.size() - 9] & 1)) ? "wsc" : "ws";
  if (tms::isColecoRom(b, n)) return "coleco";
  if (tms::isSg1000Rom(b, n)) return "sg1000";
  if (intv::isIntvRom(b, n)) return "intv";  // antes do MSX (.rom) e do Atari 2600 (.bin)
  if (msx::isMsxRom(b, n)) return "msx";
  if (b.size() >= 0x200 && b[0x15C] == 0x56 && b[0x15D] == 0xCF) return "nds";
  if (ends(n, ".nds")) return "nds";
  if (a5200::isA5200Rom(b, n) && !gbLogo && sms::smsKind(b, "").empty()) return "a5200";
  if (gbaLogo || (at(b, 0xB2) == 0x96 && (ends(n, ".gba") || ends(n, ".agb") || ends(n, ".bin")))) return "gba";
  if (gbLogo) return (b[0x143] & 0x80) ? "gbc" : "gb";
  const std::string sk = sms::smsKind(b, n);
  if (!sk.empty()) return sk;
  if (ends(n, ".gba") || ends(n, ".agb")) return "gba";
  if (ends(n, ".gbc") || ends(n, ".cgb")) return "gbc";
  if (ends(n, ".gb") || ends(n, ".sgb")) return "gb";
  if (a2600::isA2600Rom(b, n)) return "a2600";
  return "";
}
}  // namespace

std::string extensionFor(const std::string& sys) {
  if (sys == "gba") return ".gba";
  if (sys == "gbc") return ".gbc";
  if (sys == "gb") return ".gb";
  if (sys == "gg") return ".gg";
  if (sys == "sms") return ".sms";
  if (sys == "ngpc") return ".ngc";
  if (sys == "ngp") return ".ngp";
  if (sys == "nes") return ".nes";
  if (sys == "fds") return ".fds";
  if (sys == "snes") return ".sfc";
  if (sys == "pm") return ".min";
  if (sys == "cps") return ".zip";
  if (sys == "neogeo") return ".zip";
  if (sys == "gnw") return ".zip";
  if (sys == "md") return ".md";
  if (sys == "coleco") return ".col";
  if (sys == "a5200") return ".a52";
  if (sys == "a7800") return ".a78";
  if (sys == "vectrex") return ".vec";
  if (sys == "intv") return ".int";  // o núcleo reconhece o formato .rom pelo conteúdo também em .int
  if (sys == "lynx") return ".lnx";
  if (sys == "vb") return ".vb";
  if (sys == "duck") return ".duck";
  if (sys == "duck0") return ".md0";
  if (sys == "svision") return ".sv";
  if (sys == "pce") return ".pce";
  if (sys == "sgx") return ".sgx";
  if (sys == "ws") return ".ws";
  if (sys == "wsc") return ".wsc";
  if (sys == "sg1000") return ".sg";
  if (sys == "msx") return ".rom";
  if (sys == "a2600") return ".a26";
  if (sys == "gamecom") return ".tgc";
  if (sys == "jaguar") return ".j64";
  return ".bin";
}

RomIdentity identify(const std::vector<uint8_t>& b, const std::string& fileName) {
  RomIdentity r;
  r.sys = detectSystem(b, fileName);
  if (r.sys == "nds") r.sys.clear();
  if (r.sys == "gg" || r.sys == "sms") {
    r.gameCode = sms::smsCode(sms::smsNormalize(b));
  } else if (r.sys == "ngp" || r.sys == "ngpc") {
    const auto i = ngp::ngpInfo(b);
    r.title = i.title; r.gameCode = i.gameCode;
  } else if (r.sys == "snes") {
    // título do cabeçalho do cartucho; o código fica "SNES" + tamanho + hash, como no NES (o site não tem SNES)
    snes::Cart c;
    std::string err;
    if (snes::parseRom(b, c, err)) r.title = c.title;
    r.gameCode = "SNES";
  } else if (r.sys == "pce" || r.sys == "sgx") {
    r.gameCode = "PCE";  // o HuCard não tem título nem código
  } else if (r.sys == "vb") {
    // título do cabeçalho (20 bytes, 0x220 antes do fim da ROM), quando tem letras legíveis
    if (b.size() >= 0x220)
      for (size_t k = b.size() - 0x220; k < b.size() - 0x220 + 20; k++) { const int c = at(b, k); if (c >= 0x20 && c < 0x7F) r.title += static_cast<char>(c); }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    r.gameCode = "VB";
  } else if (r.sys == "lynx") {
    // título do cabeçalho .lnx (bytes 10 a 41), quando tem
    if (b.size() >= 64 && std::memcmp(&b[0], "LYNX", 4) == 0)
      for (size_t k = 10; k < 42; k++) { const int c = at(b, k); if (c >= 0x20 && c < 0x7F) r.title += static_cast<char>(c); else if (c == 0) break; }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    r.gameCode = "LYNX";
  } else if (r.sys == "duck" || r.sys == "duck0") {
    r.gameCode = "DUCK";  // os cartuchos do Mega Duck não têm cabeçalho
  } else if (r.sys == "svision") {
    r.gameCode = "SVISION";
  } else if (r.sys == "vectrex") {
    r.gameCode = "VECTREX";
  } else if (r.sys == "a5200") {
    // título do cartucho: 20 caracteres em $BFE8 (24 bytes antes do fim), em códigos de tela do ANTIC
    if (b.size() >= 24)
      for (size_t k = b.size() - 24; k < b.size() - 4; k++) { const int c = (at(b, k) & 0x3F) + 0x20; r.title += static_cast<char>(c < 0x60 ? c : ' '); }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    while (!r.title.empty() && r.title.front() == ' ') r.title.erase(r.title.begin());
    r.gameCode = "A5200";
  } else if (r.sys == "intv") {
    r.title = intv::intvTitle(b, fileName);  // texto do título apontado pelo cabeçalho em $5000
    r.gameCode = "INTV";
  } else if (r.sys == "a7800") {
    // título do cabeçalho .a78 (bytes 17 a 48), quando tem
    if (b.size() >= 128 && std::memcmp(&b[1], "ATARI7800", 9) == 0)
      for (size_t k = 17; k < 49; k++) { const int c = at(b, k); if (c >= 0x20 && c < 0x7F) r.title += static_cast<char>(c); }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    r.gameCode = "A7800";
  } else if (r.sys == "ws" || r.sys == "wsc") {
    r.gameCode = "WS";  // o rodapé tem só números (editora e jogo), sem título
  } else if (r.sys == "coleco") {
    r.gameCode = "COLECO";
  } else if (r.sys == "sg1000") {
    r.gameCode = "SG1000";
  } else if (r.sys == "msx") {
    r.gameCode = "MSX";  // o cartucho não tem título
  } else if (r.sys == "a2600") {
    r.gameCode = "A2600";  // o cartucho não tem título nem código
  } else if (r.sys == "neogeo") {
    r.gameCode = "NEOGEO";
  } else if (r.sys == "cps") {
    r.gameCode = "CPS";
  } else if (r.sys == "gnw") {
    r.gameCode = "GNW";
  } else if (r.sys == "md") {
    // título do cabeçalho (nome internacional em 0x150, 48 letras; .smd desintercalado antes)
    const auto m = md::mdNormalize(b);
    for (size_t k = 0x150; k < 0x180 && k < m.size(); k++) { const int c = m[k]; if (c >= 0x20 && c < 0x7F) r.title += static_cast<char>(c); }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    { std::string t; bool sp = false; for (char c : r.title) { if (c == ' ') { if (!sp) t += c; sp = true; } else { t += c; sp = false; } } r.title = t; }
    r.gameCode = "MD";
  } else if (r.sys == "pm") {
    // título do cabeçalho ($21B0, 12 letras); o código fica "PM" + tamanho + hash, como no NES
    for (size_t k = 0x21B0; k < 0x21BC; k++) { const int c = at(b, k); if (c >= 0x20 && c < 0x7F) r.title += static_cast<char>(c); }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    r.gameCode = "PM";
  } else if (r.sys == "fds") {
    // nome do jogo no bloco 1 do disco (3 letras); código "FDS" + tamanho + hash, como no NES
    const size_t o = (b.size() >= 16 && b[0] == 'F' && b[1] == 'D' && b[2] == 'S') ? 16 : 0;
    for (size_t k = o + 16; k < o + 19; k++) { const int c = at(b, k); if (c >= 0x20 && c < 0x7F) r.title += static_cast<char>(c); }
    while (!r.title.empty() && r.title.back() == ' ') r.title.pop_back();
    r.gameCode = "FDS";
  } else if (r.sys == "nes") {
    // o cartucho de NES não tem título nem código: fica "NES" + tamanho + hash (o site ainda não tem NES)
    r.gameCode = "NES";
  } else if (r.sys == "gba") {
    std::string t;
    for (size_t k = 0xA0; k < 0xAC; k++) { const int c = at(b, k); if (c > 0) latin1(t, c); }
    r.title = jsTrim(t);
    for (size_t k = 0xAC; k < 0xB0; k++) { const int c = at(b, k); latin1(r.gameCode, c > 0 ? c : 32); }
  } else if (r.sys == "gb" || r.sys == "gbc") {
    std::string t;
    const size_t end = (at(b, 0x143) > 0 && (at(b, 0x143) & 0x80)) ? 0x143 : 0x144;
    for (size_t k = 0x134; k < end; k++) {
      const int c = at(b, k);
      if (c <= 0) break;  // 0 ou fim do arquivo (undefined) param
      if (c >= 0x20 && c < 0x7F) t += static_cast<char>(c);
    }
    r.title = jsTrim(t);
    auto byte = [&](size_t k) { const int c = at(b, k); return c < 0 ? 0 : c; };
    char code[16];
    std::snprintf(code, sizeof code, "%06X", byte(0x14D) | (byte(0x14E) << 8) | (byte(0x14F) << 16));
    r.gameCode = code;
  }
  // romId (web/library.js): código + título só com letras/números + tamanho + hash amostrado
  uint32_t h = 0x811C9DC5u;
  const size_t step = std::max<size_t>(1, b.size() / 65536);
  for (size_t i = 0; i < b.size(); i += step) { h ^= b[i]; h *= 0x01000193u; }
  std::string code = jsTrim(r.gameCode);
  if (code.empty()) code = "XXXX";
  std::string t;
  for (char c : r.title)
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_') t += c;
  if (t.empty()) t = "rom";
  char tail[48];
  std::snprintf(tail, sizeof tail, "-%zu-%x", b.size(), h);
  r.id = code + "-" + t + tail;
  r.saveKey = "rom:" + r.id;
  return r;
}

// ------------------------------------------------------------ backup
namespace {
const char MAGIC[] = "INDIGOB1";

// referência a um bloco binário depois do JSON
bool blobRef(const Json& v, size_t base, const std::vector<uint8_t>& bytes, std::vector<uint8_t>& out) {
  if (v.type != Json::Obj || (!v.get("$ab") && !v.get("$ta"))) return false;
  // posição e tamanho têm de ser inteiros dentro do arquivo (NaN, infinito ou fração = backup danificado)
  const double o = v.num("o", -1), n = v.num("n", -1);
  if (base > bytes.size()) return false;
  const double room = static_cast<double>(bytes.size() - base);
  if (!wholeIn(o, 0, room) || !wholeIn(n, 0, room)) return false;
  const size_t oo = static_cast<size_t>(o), nn = static_cast<size_t>(n);
  if (oo > bytes.size() - base || nn > bytes.size() - base - oo) return false;
  const size_t from = base + oo;
  out.assign(bytes.begin() + static_cast<long>(from), bytes.begin() + static_cast<long>(from + nn));
  return true;
}
bool isRef(const Json& v) { return v.type == Json::Obj && (v.get("$ab") || v.get("$ta") || v.get("$num")); }

SaveRec saveFrom(const Json& r, size_t base, const std::vector<uint8_t>& bytes) {
  SaveRec s;
  s.key = r.str("key");
  s.at = safeMs(r.num("at"));
  s.reason = r.str("reason");
  if (const Json* d = r.get("data")) blobRef(*d, base, bytes, s.data);
  return s;
}

Json ref(std::vector<uint8_t>& parts, const std::vector<uint8_t>& data, bool typed) {
  Json j = Json::object();
  if (typed) j.set("$ta", Json::string("Uint8Array")); else j.set("$ab", Json::number(1));
  j.set("o", Json::number(static_cast<double>(parts.size())));
  j.set("n", Json::number(static_cast<double>(data.size())));
  parts.insert(parts.end(), data.begin(), data.end());
  return j;
}
}  // namespace

bool isBackup(const std::vector<uint8_t>& b) { return b.size() >= 12 && std::memcmp(b.data(), MAGIC, 8) == 0; }

bool read(const std::vector<uint8_t>& bytes, Backup& out, std::string& error) {
  out = Backup{};
  if (!isBackup(bytes)) { error = "Esse arquivo não é um backup do Índigo."; return false; }
  const uint32_t jlen = bytes[8] | (bytes[9] << 8) | (bytes[10] << 16) | (uint32_t(bytes[11]) << 24);
  if (12 + static_cast<size_t>(jlen) > bytes.size()) { error = "Backup incompleto ou danificado."; return false; }
  Json head;
  if (!parseJson(std::string(bytes.begin() + 12, bytes.begin() + 12 + jlen), head) || head.type != Json::Obj) {
    error = "Backup danificado (cabeçalho ilegível).";
    return false;
  }
  const size_t base = 12 + jlen;
  out.withRoms = head.flag("withRoms");
  if (const Json* roms = head.get("roms"); roms && roms->type == Json::Arr) {
    for (const Json& r : roms->a) {
      if (r.type != Json::Obj) continue;
      Rom rom;
      rom.meta = Json::object();
      for (const auto& [k, v] : r.o) {
        if (k == "data") { rom.hasData = blobRef(v, base, bytes, rom.data); continue; }
        if (!isRef(v)) rom.meta.o.emplace_back(k, v);
      }
      out.roms.push_back(std::move(rom));
    }
  }
  if (const Json* s = head.get("saves"); s && s->type == Json::Arr)
    for (const Json& r : s->a) if (r.type == Json::Obj) out.saves.push_back(saveFrom(r, base, bytes));
  if (const Json* s = head.get("hist"); s && s->type == Json::Arr)
    for (const Json& r : s->a) if (r.type == Json::Obj) out.hist.push_back(saveFrom(r, base, bytes));
  if (const Json* s = head.get("states"); s && s->type == Json::Arr) out.states = s->a.size();
  return true;
}

std::vector<uint8_t> write(const Backup& b, double createdAtMs) {
  std::vector<uint8_t> parts;
  Json head = Json::object();
  head.set("format", Json::number(1));
  head.set("app", Json::string("Índigo"));
  head.set("createdAt", Json::number(createdAtMs));
  head.set("withRoms", Json::boolean(b.withRoms));
  Json roms = Json::array();
  for (const Rom& r : b.roms) {
    Json m = r.meta.type == Json::Obj ? r.meta : Json::object();
    if (r.hasData) m.set("data", ref(parts, r.data, true)); else m.set("data", Json());
    roms.a.push_back(std::move(m));
  }
  head.set("roms", std::move(roms));
  Json saves = Json::array();
  for (const SaveRec& s : b.saves) {
    Json j = Json::object();
    j.set("key", Json::string(s.key));
    j.set("at", Json::number(s.at));
    j.set("size", Json::number(static_cast<double>(s.data.size())));
    j.set("data", ref(parts, s.data, true));
    j.set("source", Json::string(s.reason.empty() ? "pc" : s.reason));
    saves.a.push_back(std::move(j));
  }
  head.set("saves", std::move(saves));
  Json hist = Json::array();
  for (const SaveRec& s : b.hist) {
    Json j = Json::object();
    char at[32];
    std::snprintf(at, sizeof at, "%.0f", s.at);
    j.set("hid", Json::string(s.key + "@" + at));
    j.set("key", Json::string(s.key));
    j.set("at", Json::number(s.at));
    j.set("size", Json::number(static_cast<double>(s.data.size())));
    j.set("data", ref(parts, s.data, false));
    j.set("reason", Json::string(s.reason.empty() ? "versão anterior" : s.reason));
    hist.a.push_back(std::move(j));
  }
  head.set("hist", std::move(hist));
  head.set("states", Json::array());
  const std::string json = writeJson(head);
  std::vector<uint8_t> out(MAGIC, MAGIC + 8);
  const uint32_t n = static_cast<uint32_t>(json.size());
  for (int k = 0; k < 4; k++) out.push_back(static_cast<uint8_t>(n >> (8 * k)));
  out.insert(out.end(), json.begin(), json.end());
  out.insert(out.end(), parts.begin(), parts.end());
  return out;
}

}  // namespace indigo::backup
