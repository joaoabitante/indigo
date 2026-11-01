// Backup da estante no formato da versão web (.indigo, web/backup.js) e identificação de jogos igual à da web
// (web/main.js: romHeader, romId, romSaveKey). Assim um backup feito no site abre no PC/celular e vice-versa.
//
// Formato: "INDIGOB1" + tamanho do cabeçalho (u32) + cabeçalho JSON + blocos binários. O JSON guarda a posição
// de cada bloco ({"$ab":1,"o":..,"n":..} ou {"$ta":"Uint8Array","o":..,"n":..}).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace indigo::backup {

// ------------------------------------------------------------ JSON mínimo
struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;  // ordem preservada

  const Json* get(const std::string& k) const;
  Json& set(const std::string& k, Json v);
  std::string str(const std::string& k, const std::string& def = "") const;
  double num(const std::string& k, double def = 0) const;
  bool flag(const std::string& k) const;

  static Json string(std::string v) { Json j; j.type = Str; j.s = std::move(v); return j; }
  static Json number(double v) { Json j; j.type = Num; j.n = v; return j; }
  static Json boolean(bool v) { Json j; j.type = Bool; j.b = v; return j; }
  static Json array() { Json j; j.type = Arr; return j; }
  static Json object() { Json j; j.type = Obj; return j; }
};
bool parseJson(const std::string& text, Json& out);
std::string writeJson(const Json& j);

// ------------------------------------------------------------ identificação (igual à web)
struct RomIdentity {
  std::string sys;       // gb, gbc, gba, sms, gg, ngp, ngpc ("" se não reconhecer)
  std::string title, gameCode;
  std::string id;        // romId da web
  std::string saveKey;   // "rom:" + id
};
RomIdentity identify(const std::vector<uint8_t>& rom, const std::string& fileName);
// extensão de arquivo para o console ("gba" → ".gba", "ngpc" → ".ngc")
std::string extensionFor(const std::string& sys);

// ------------------------------------------------------------ backup
struct Rom {
  Json meta;                  // tudo o que a web guarda (id, name, fileName, console, saveKey, capa...)
  std::vector<uint8_t> data;  // vazio se o backup foi feito sem as ROMs
  bool hasData = false;
};
struct SaveRec {
  std::string key;
  double at = 0;  // ms desde 1970
  std::vector<uint8_t> data;
  std::string reason;
};
struct Backup {
  std::vector<Rom> roms;
  std::vector<SaveRec> saves, hist;
  size_t states = 0;  // estados da web (não servem no nativo: o formato é outro)
  bool withRoms = false;
};

bool isBackup(const std::vector<uint8_t>& bytes);
bool read(const std::vector<uint8_t>& bytes, Backup& out, std::string& error);
std::vector<uint8_t> write(const Backup& b, double createdAtMs);

}  // namespace indigo::backup
