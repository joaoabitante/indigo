// Cuidados com dados de fora (backup, nuvem, .zip, nome vindo de outro app): nome de arquivo sem pastas e
// número convertido só quando cabe no tipo de destino.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace indigo {

// nome de arquivo seguro para juntar a uma pasta: sem barras, sem ".." nem caracteres de controle ou proibidos
// no Windows, até 200 bytes (a extensão fica). Vazio vira "jogo".
inline std::string safeFileName(std::string n) {
  for (char& c : n) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
  }
  while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
  constexpr size_t MAX = 200;
  if (n.size() > MAX) {
    const auto dot = n.find_last_of('.');
    std::string ext = dot != std::string::npos && n.size() - dot <= 16 ? n.substr(dot) : "";
    size_t cut = MAX - ext.size();
    while (cut > 0 && (static_cast<unsigned char>(n[cut]) & 0xC0) == 0x80) cut--;  // não corta uma letra UTF-8 ao meio
    n = n.substr(0, cut) + ext;
  }
  if (n.empty() || n == "." || n == "..") return "jogo";
  return n;
}

// número finito e inteiro dentro de [lo, hi] (converter double fora do tipo é comportamento indefinido)
inline bool wholeIn(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi && v == std::floor(v); }

// horário em ms (de backup ou da nuvem): finito e entre 0 e o ano 33658; senão 0
inline double safeMs(double v) { return std::isfinite(v) && v >= 0 && v <= 1e15 ? std::floor(v) : 0; }

// tamanho vindo de fora: inteiro de 0 a 2^53; senão 0
inline size_t safeSize(double v) {
  const double hi = static_cast<double>(SIZE_MAX) < 9007199254740992.0 ? static_cast<double>(SIZE_MAX) : 9007199254740992.0;
  return wholeIn(v, 0, hi) ? static_cast<size_t>(v) : 0;
}

// endereço da nuvem trocado por INDIGO_CLOUD_URL (testes): https://, ou http:// só para este computador
// (localhost ou 127.0.0.1, como no native/tools/cloudmock.mjs). O PIN vai no cabeçalho de cada pedido.
inline bool cloudUrlAllowed(const std::string& u) {
  auto low = u;
  for (char& c : low) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if (low.rfind("https://", 0) == 0) return low.size() > 8;
  if (low.rfind("http://", 0) != 0) return false;
  const std::string rest = low.substr(7);
  for (const char* h : {"localhost", "127.0.0.1"}) {
    const std::string hs = h;
    if (rest.rfind(hs, 0) == 0 && (rest.size() == hs.size() || rest[hs.size()] == ':' || rest[hs.size()] == '/')) return true;
  }
  return false;
}

}  // namespace indigo
