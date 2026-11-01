#include "shelf.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <map>

#include "core/backup.h"
#include "core/zip.h"
#include "files.h"
#include "platform.h"

namespace indigo::shelf {

namespace {
constexpr size_t HIST_MAX = 12;

double mtimeMs(const std::string& path) {
  SDL_Time t = 0;
  return files::modified(path, t) ? static_cast<double>(t / 1000000) : 0.0;
}

// nome de arquivo seguro (sem barras nem caracteres proibidos no Windows)
std::string safeName(std::string n) {
  for (char& c : n) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
  while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
  return n.empty() ? "jogo" : n;
}

std::string histPath(const std::string& dataDir, const std::string& key, long long sec) {
  return dataDir + "saves/historico/" + key + "." + std::to_string(sec) + ".sav";
}

void dedupe(std::vector<std::string>& v) {
  std::vector<std::string> out;
  for (auto& s : v) if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  v.swap(out);
}
}  // namespace

double nowMs() {
  SDL_Time t = 0;
  SDL_GetCurrentTime(&t);
  return static_cast<double>(t / 1000000);
}

double saveAt(const std::string& dataDir, const std::string& romKey) {
  const std::string sav = dataDir + "saves/" + romKey + ".sav";
  std::vector<uint8_t> b;
  if (files::read(sav + ".at", b) && !b.empty()) {
    const double v = std::atof(std::string(b.begin(), b.end()).c_str());
    if (v > 0) return v;
  }
  return mtimeMs(sav);
}

void setSaveAt(const std::string& dataDir, const std::string& romKey, double atMs) {
  char t[40];
  SDL_snprintf(t, sizeof t, "%.0f\n", atMs);
  const std::string s = t;
  files::write(dataDir + "saves/" + romKey + ".sav.at", std::vector<uint8_t>(s.begin(), s.end()));
}

bool loadRom(const std::string& path, std::vector<uint8_t>& bytes, std::string& key) {
  if (!files::read(path, bytes)) return false;
  key = platform::displayName(path);
  if (zip::isZip(bytes)) {
    std::string inner, err;
    std::vector<uint8_t> data;
    if (!zip::extractRom(bytes, inner, data, err)) return false;
    bytes.swap(data);
    key = inner;
  }
  return true;
}

std::vector<std::pair<std::string, long long>> saveVersions(const std::string& dataDir, const std::string& romKey) {
  std::vector<std::pair<std::string, long long>> out;
  const std::string dir = dataDir + "saves/historico/", pre = romKey + ".";
  for (const std::string& n : files::list(dir)) {
    if (n.rfind(pre, 0) != 0 || n.size() < pre.size() + 5 || n.compare(n.size() - 4, 4, ".sav") != 0) continue;
    // o meio do nome tem de ser só o horário (para "Zelda.gb" não pegar as versões de "Zelda.gbc")
    const std::string mid = n.substr(pre.size(), n.size() - pre.size() - 4);
    if (mid.empty() || mid.size() > 15 || mid.find_first_not_of("0123456789") != std::string::npos) continue;
    out.push_back({dir + n, std::stoll(mid)});
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  return out;
}

void pruneVersions(const std::string& dataDir, const std::string& romKey, size_t keep) {
  const auto list = saveVersions(dataDir, romKey);
  for (size_t k = keep; k < list.size(); k++) SDL_RemovePath(list[k].first.c_str());
}

ImportResult importBackup(const std::string& dataDir, const std::vector<uint8_t>& bytes, const std::vector<std::string>& localRoms) {
  ImportResult res;
  backup::Backup bk;
  if (!backup::read(bytes, bk, res.error)) return res;
  res.webStates = bk.states;

  // identidade (como na web) → nome do arquivo da ROM neste aparelho
  std::map<std::string, std::string> byId;
  for (const auto& p : localRoms) {
    std::vector<uint8_t> b;
    std::string key;
    if (loadRom(p, b, key)) byId[backup::identify(b, key).id] = key;
  }
  std::map<std::string, std::string> bySaveKey;  // chave de save da web → nome do arquivo da ROM
  for (const auto& r : bk.roms) {
    const std::string id = r.meta.str("id");
    std::string key;
    if (auto it = byId.find(id); it != byId.end()) {
      key = it->second;
      res.romsKnown++;
    } else if (r.hasData) {
      std::string fname = r.meta.str("fileName");
      if (fname.find('.') == std::string::npos) fname = r.meta.str("name", "jogo") + backup::extensionFor(r.meta.str("console"));
      fname = safeName(fname);
      // nome já usado por outra ROM: acrescenta " (2)", " (3)"...
      std::string candidate = fname;
      for (int n = 2; n < 100; n++) {
        std::vector<uint8_t> cur;
        if (!files::read(dataDir + "roms/" + candidate, cur) || cur == r.data) break;
        const auto dot = fname.find_last_of('.');
        candidate = fname.substr(0, dot) + " (" + std::to_string(n) + ")" + (dot == std::string::npos ? "" : fname.substr(dot));
      }
      if (!files::write(dataDir + "roms/" + candidate, r.data)) continue;
      key = candidate;
      byId[id] = key;
      res.newPaths.push_back(dataDir + "roms/" + candidate);
      res.romsAdded++;
    } else {
      res.romsMissing++;
      continue;
    }
    bySaveKey[r.meta.str("saveKey", "rom:" + id)] = key;
  }

  // saves: fica o mais novo; o outro vai para as versões anteriores (nada é apagado)
  for (const auto& s : bk.saves) {
    auto it = bySaveKey.find(s.key);
    if (it == bySaveKey.end() || s.data.empty()) continue;
    const std::string& key = it->second;
    const std::string path = dataDir + "saves/" + key + ".sav";
    std::vector<uint8_t> cur;
    if (!files::read(path, cur)) {
      if (files::write(path, s.data)) { setSaveAt(dataDir, key, s.at); res.savesNew++; }
    } else if (cur == s.data) {
      res.savesSame++;
    } else if (s.at > saveAt(dataDir, key)) {
      files::write(histPath(dataDir, key, static_cast<long long>(saveAt(dataDir, key) / 1000)), cur);
      if (files::write(path, s.data)) { setSaveAt(dataDir, key, s.at); res.savesNewer++; }
    } else {
      files::write(histPath(dataDir, key, static_cast<long long>(s.at / 1000)), s.data);
      res.savesOlder++;
    }
  }
  for (const auto& h : bk.hist) {
    auto it = bySaveKey.find(h.key);
    if (it == bySaveKey.end() || h.data.empty()) continue;
    const std::string p = histPath(dataDir, it->second, static_cast<long long>(h.at / 1000));
    if (!files::exists(p) && files::write(p, h.data)) res.versions++;
  }
  for (const auto& [sk, key] : bySaveKey) pruneVersions(dataDir, key, HIST_MAX);
  return res;
}

std::vector<uint8_t> exportBackup(const std::string& dataDir, const std::vector<std::string>& localRoms, bool withRoms, int& games) {
  backup::Backup bk;
  bk.withRoms = withRoms;
  games = 0;
  std::vector<std::string> paths = localRoms;
  dedupe(paths);
  std::vector<std::string> seen;
  for (const auto& p : paths) {
    std::vector<uint8_t> b;
    std::string key;
    if (!loadRom(p, b, key)) continue;
    const auto id = backup::identify(b, key);
    if (id.sys.empty() || std::find(seen.begin(), seen.end(), id.id) != seen.end()) continue;
    seen.push_back(id.id);
    backup::Rom r;
    r.meta = backup::Json::object();
    const auto dot = key.find_last_of('.');
    const double t = mtimeMs(p);
    r.meta.set("id", backup::Json::string(id.id));
    r.meta.set("name", backup::Json::string(dot == std::string::npos ? key : key.substr(0, dot)));
    r.meta.set("fileName", backup::Json::string(key));
    r.meta.set("title", backup::Json::string(id.title));
    r.meta.set("gameCode", backup::Json::string(id.gameCode));
    r.meta.set("console", backup::Json::string(id.sys));
    r.meta.set("size", backup::Json::number(static_cast<double>(b.size())));
    r.meta.set("saveKey", backup::Json::string(id.saveKey));
    r.meta.set("added", backup::Json::number(t));
    r.meta.set("lastPlayed", backup::Json::number(t));
    if (withRoms) { r.data = std::move(b); r.hasData = true; }
    bk.roms.push_back(std::move(r));
    games++;
    const std::string savePath = dataDir + "saves/" + key + ".sav";
    backup::SaveRec s;
    if (files::read(savePath, s.data) && !s.data.empty()) {
      s.key = id.saveKey;
      s.at = saveAt(dataDir, key);
      s.reason = "pc";
      bk.saves.push_back(std::move(s));
    }
    for (const auto& [hp, sec] : saveVersions(dataDir, key)) {
      backup::SaveRec h;
      if (!files::read(hp, h.data) || h.data.empty()) continue;
      h.key = id.saveKey;
      h.at = static_cast<double>(sec) * 1000.0;
      h.reason = "versão anterior";
      bk.hist.push_back(std::move(h));
    }
  }
  SDL_Time now = 0;
  SDL_GetCurrentTime(&now);
  return backup::write(bk, static_cast<double>(now / 1000000));
}

}  // namespace indigo::shelf
