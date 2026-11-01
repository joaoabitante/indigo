// Arquivos pelo SDL (UTF-8 em todos os sistemas; no Android também aceita "content://").
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace indigo::files {

inline bool read(const std::string& path, std::vector<uint8_t>& out) {
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (!data) return false;
  const auto* p = static_cast<const uint8_t*>(data);
  out.assign(p, p + size);
  SDL_free(data);
  return true;
}

inline bool write(const std::string& path, const std::vector<uint8_t>& data) {
  return SDL_SaveFile(path.c_str(), data.data(), data.size());
}

inline bool modified(const std::string& path, SDL_Time& t) {
  SDL_PathInfo info{};
  if (!SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE) return false;
  t = info.modify_time;
  return true;
}

inline bool exists(const std::string& path) {
  SDL_Time t;
  return modified(path, t);
}

// nomes dos arquivos de uma pasta (sem o caminho)
inline std::vector<std::string> list(const std::string& dir) {
  std::vector<std::string> out;
  int count = 0;
  char** names = SDL_GlobDirectory(dir.c_str(), nullptr, 0, &count);
  for (int k = 0; names && k < count; k++) out.push_back(names[k]);
  SDL_free(names);
  return out;
}

inline std::string fileName(const std::string& path) {
  const auto slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace indigo::files
