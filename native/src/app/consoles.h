// Consoles na estante/biblioteca do app: extensões de arquivo, aba (código curto como "NES", "5200") de cada
// uma, ordem das abas e aba de cada núcleo. Uma lista só para a estante, o diálogo de abrir do PC e a associação
// de arquivos do Windows (antes eram três listas soltas, e o Jaguar tinha ficado fora do diálogo).
// Sem SDL: o teste tools/t_app_uso.cpp confere tudo aqui.
#pragma once
#include <cstddef>
#include <string>

namespace indigo::app {

struct ExtTab { const char* ext; const char* tab; };

// extensões que entram sozinhas na estante (uma pasta de jogos). Ficam de fora as genéricas demais (.bin, .exe):
// essas só aparecem quando o jogo já foi aberto (o app lembra o console). .zip e CDs: o app decide olhando dentro.
inline constexpr ExtTab EXT_TABS[] = {
  {".sfc", "SNES"}, {".smc", "SNES"}, {".swc", "SNES"}, {".fig", "SNES"},
  {".nes", "NES"}, {".fds", "FDS"}, {".qd", "FDS"},
  {".gb", "GB"}, {".dmg", "GB"}, {".sgb", "GB"}, {".gbc", "GBC"}, {".gba", "GBA"}, {".agb", "GBA"},
  {".sms", "SMS"}, {".gg", "GG"}, {".sg", "SG"}, {".sc", "SG"},
  {".ngp", "NGP"}, {".ngc", "NGP"}, {".npc", "NGP"}, {".min", "PM"},
  {".a26", "2600"}, {".a52", "5200"}, {".a78", "7800"}, {".lnx", "LYNX"}, {".lyx", "LYNX"},
  {".rom", "MSX"}, {".mx1", "MSX"}, {".mx2", "MSX"}, {".col", "COLECO"}, {".cv", "COLECO"},
  {".ws", "WS"}, {".wsc", "WS"}, {".pc2", "WS"}, {".vec", "VECTREX"}, {".gam", "VECTREX"},
  {".int", "INTV"}, {".itv", "INTV"}, {".vb", "VB"},
  {".duck", "DUCK"}, {".md0", "DUCK"}, {".md1", "DUCK"}, {".md2", "DUCK"}, {".sv", "SV"},
  {".pce", "PCE"}, {".sgx", "PCE"}, {".md", "MD"}, {".gen", "MD"}, {".smd", "MD"}, {".32x", "32X"},
  {".tgc", "GAMECOM"}, {".j64", "JAGUAR"}, {".jag", "JAGUAR"}, {".abs", "JAGUAR"},
  {".neo", "ARCADE"}, {".zip", "ARCADE"}, {".cue", "CD"}, {".chd", "CD"}, {".iso", "CD"},
};

// ordem das abas da biblioteca (e da estante); "outros" no fim
inline constexpr const char* TAB_ORDER[] = {
  "SNES", "MD", "32X", "NES", "FDS", "GB", "GBC", "GBA", "SMS", "GG", "NGP", "PCE", "PM", "2600", "5200", "7800",
  "LYNX", "MSX", "COLECO", "SG", "WS", "VECTREX", "INTV", "VB", "DUCK", "SV", "G&W", "GAMECOM", "JAGUAR", "ARCADE",
  "PS1", "3DO", "PCFX", "SATURN", "CD", "outros"};

inline std::string lowerAscii(std::string s) {
  for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
  return s;
}

// extensão (com o ponto, minúscula) do nome; vazia se não tem
inline std::string extOf(const std::string& name) {
  const std::string n = lowerAscii(name);
  const auto slash = n.find_last_of("/\\"), dot = n.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  return n.substr(dot);
}

// aba pela extensão; "" = extensão que a estante não conhece (.exe e .psx só abrem: não entram sozinhos)
inline std::string tabOfExt(const std::string& ext) {
  for (const auto& e : EXT_TABS) if (ext == e.ext) return e.tab;
  if (ext == ".exe" || ext == ".psx") return "PS1";
  return "";
}

// o arquivo entra sozinho na estante?
inline bool shelfExtension(const std::string& name) {
  const std::string e = extOf(name);
  for (const auto& t : EXT_TABS) if (e == t.ext) return true;
  return false;
}

// aba de um núcleo aberto (Core::system())
inline std::string tabOfSystem(const std::string& s) {
  static constexpr ExtTab SYS[] = {
    {"snes", "SNES"}, {"nes", "NES"}, {"fds", "FDS"}, {"gb", "GB"}, {"gbc", "GBC"}, {"gba", "GBA"},
    {"sms", "SMS"}, {"gg", "GG"}, {"sg1000", "SG"}, {"ngp", "NGP"}, {"ngpc", "NGP"}, {"pm", "PM"},
    {"a2600", "2600"}, {"a5200", "5200"}, {"a7800", "7800"}, {"lynx", "LYNX"}, {"msx", "MSX"},
    {"coleco", "COLECO"}, {"ws", "WS"}, {"wsc", "WS"}, {"vectrex", "VECTREX"}, {"intv", "INTV"}, {"vb", "VB"},
    {"duck", "DUCK"}, {"svision", "SV"}, {"pce", "PCE"}, {"sgx", "PCE"}, {"md", "MD"}, {"32x", "32X"},
    {"gnw", "G&W"}, {"gamecom", "GAMECOM"}, {"jaguar", "JAGUAR"}, {"neogeo", "ARCADE"}, {"cps1", "ARCADE"},
    {"cps2", "ARCADE"}, {"ps1", "PS1"}, {"3do", "3DO"}, {"pcfx", "PCFX"}, {"saturn", "SATURN"},
    {"segacd", "CD"}, {"pcecd", "CD"}, {"neogeocd", "CD"},
  };
  for (const auto& e : SYS) if (s == e.ext) return e.tab;
  return "outros";
}

// filtro do diálogo de abrir (SDL): "sfc;smc;..." com todas as extensões da estante e as que só abrem
inline const std::string& dialogPattern() {
  static const std::string p = [] {
    std::string s;
    for (const auto& e : EXT_TABS) s += std::string(e.ext + 1) + ";";
    return s + "bin;exe;psx;indigo";
  }();
  return p;
}

}  // namespace indigo::app
