// Leitor mínimo de .zip (porte de web/zip.js): acha a ROM dentro do arquivo e descompacta (DEFLATE próprio).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace indigo::zip {

bool isZip(const std::vector<uint8_t>& bytes);

// devolve false com a mensagem em `error` se não achar ROM ou o arquivo estiver danificado
bool extractRom(const std::vector<uint8_t>& bytes, std::string& name, std::vector<uint8_t>& data, std::string& error);

// Pacotes com várias ROMs (jogos de arcade no formato do MAME: mslug.zip, sf2.zip...): lista os arquivos de
// dentro (nome sem pastas, tamanho e CRC-32 gravados no .zip) e extrai um pelo nome. O nome compara sem
// diferenciar maiúsculas. Em .zip danificado, listEntries devolve false e extractEntry também.
struct Entry {
  std::string name;
  uint32_t size = 0, crc = 0;
};
bool listEntries(const std::vector<uint8_t>& bytes, std::vector<Entry>& out);
bool extractEntry(const std::vector<uint8_t>& bytes, const std::string& name, std::vector<uint8_t>& data);

// método de compressão do arquivo `name` no .zip (0 guardado, 8 Deflate, 9 Deflate64...; -1 se não existir) e o
// nome dele, para a mensagem de erro quando for um que o Índigo não lê
int entryMethod(const std::vector<uint8_t>& bytes, const std::string& name);
const char* methodName(int method);

// junta dois pacotes num só (o do jogo e o do jogo principal, nos pacotes "split" do MAME): os arquivos do
// primeiro, e os do segundo que o primeiro não tem. Os dados comprimidos são copiados como estão.
std::vector<uint8_t> merge(const std::vector<uint8_t>& first, const std::vector<uint8_t>& second);

// DEFLATE "cru" (sem cabeçalho zlib); `expected` é o tamanho final conhecido (0 = desconhecido)
bool inflate(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected = 0);

}  // namespace indigo::zip
