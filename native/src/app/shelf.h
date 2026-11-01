// Estante do app: importar e exportar backup no formato da versão web (.indigo).
//
// O app guarda os saves pelo nome do arquivo da ROM (saves/<Jogo.gba>.sav, versões em
// saves/historico/<Jogo.gba>.<segundos>.sav). A web guarda pela identidade da ROM ("rom:" + romId). A ponte é
// feita calculando a identidade de cada ROM, igual à web (src/core/backup.cpp).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace indigo::shelf {

struct ImportResult {
  int romsAdded = 0, romsKnown = 0, romsMissing = 0;
  int savesNew = 0, savesNewer = 0, savesOlder = 0, savesSame = 0, versions = 0;
  size_t webStates = 0;
  std::vector<std::string> newPaths;  // ROMs gravadas na pasta do app
  std::string error;
};

// localRoms: ROMs que o app já conhece (biblioteca e recentes), para casar saves de backups sem ROMs
ImportResult importBackup(const std::string& dataDir, const std::vector<uint8_t>& bytes, const std::vector<std::string>& localRoms);

// devolve o arquivo .indigo; `games` recebe quantos jogos entraram
std::vector<uint8_t> exportBackup(const std::string& dataDir, const std::vector<std::string>& localRoms, bool withRoms, int& games);

// versões anteriores de um save, da mais nova para a mais antiga: (caminho, segundos desde 1970)
std::vector<std::pair<std::string, long long>> saveVersions(const std::string& dataDir, const std::string& romKey);
void pruneVersions(const std::string& dataDir, const std::string& romKey, size_t keep);

// horário do save (ms desde 1970) como a web entende: quando o jogo gravou, ou o horário de origem de um save
// que veio da nuvem/backup. Fica num arquivo ao lado (<save>.at); sem ele, vale a data do arquivo.
double saveAt(const std::string& dataDir, const std::string& romKey);
void setSaveAt(const std::string& dataDir, const std::string& romKey, double atMs);
double nowMs();

// lê uma ROM (descompacta .zip): bytes e nome do arquivo da ROM (a chave do save)
bool loadRom(const std::string& path, std::vector<uint8_t>& bytes, std::string& key);

}  // namespace indigo::shelf
