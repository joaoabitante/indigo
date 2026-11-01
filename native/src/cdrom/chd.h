// Imagens .chd do MAME (versão 5, CD: codecs cdlz, cdzl, cdfl e os de dados lzma, zlib, huff, flac).
// As faixas vêm dos metadados CHT2/CHTR; os hunks são lidos do arquivo sob demanda, com um cache pequeno.
#pragma once
#include <cstdint>
#include <string>

#include "cdrom/cdrom.h"

namespace indigo::cdrom {

std::unique_ptr<Disc> openChd(const std::string& path, std::string* err);

// informações de um disco aberto de .chd (para os testes): codecs, hunks por tipo de compressão do mapa e
// quantos setores tiveram a correção (ECC) refeita pelos codecs de CD
struct ChdInfo {
  std::string codecs[4];      // "cdlz", "zlib", ... ("" = vazio)
  uint32_t hunkBytes = 0;
  uint32_t hunkCount = 0;
  uint32_t mapTypes[8] = {};  // 0..3 = codec 0..3, 4 = sem compressão, 5 = cópia de outro hunk, 6 = pai
  uint64_t hunksDecoded = 0;
  uint64_t eccRestored = 0;
};
// false se o disco não veio de um .chd
bool chdInfo(const Disc& d, ChdInfo& out);

}  // namespace indigo::cdrom
