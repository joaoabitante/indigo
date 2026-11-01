// Decodificadores usados pelas imagens .chd (escritos para o Índigo, sem código de terceiros):
//   - deflate cru (sem cabeçalho zlib), como o codec "zlib" do CHD
//   - LZMA cru (lc=3, lp=0, pb=2, sem cabeçalho e sem marcador de fim), como o codec "lzma" do CHD
//   - FLAC (só os quadros, sem o cabeçalho "fLaC"; 2 canais de 16 bits), como os codecs "flac" e "cdfl"
//   - huffman do MAME (árvore em RLE ou em huffman), usado pelo codec "huff" e pelo mapa da versão 5
//   - CRC16 (CCITT) dos hunks e do mapa, e a correção P/Q (ECC) dos setores Mode 1 tirada pelos codecs de CD
// Todos conferem os limites da entrada e da saída: dado corrompido devolve false, nunca lê ou escreve fora.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace indigo::cdrom::chd {

// descomprime exatamente outLen bytes; false se o dado for inválido ou não der o tamanho certo
bool inflateRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen);
bool lzmaRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen);
// FLAC: preenche outLen bytes (múltiplo de 4) com amostras estéreo de 16 bits, em big-endian se bigEndian.
// consumed recebe quantos bytes da entrada os quadros usaram (o subcódigo do cdfl vem logo depois).
bool flacFrames(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen, bool bigEndian, size_t* consumed);
// codec "huff": árvore de 256 símbolos e depois um byte por símbolo
bool huffman8(const uint8_t* in, size_t inLen, uint8_t* out, size_t outLen);

uint16_t crc16(const uint8_t* p, size_t n);

// lê bits do mais significativo para o menos (como o bitstream_in do MAME): além do fim lê zeros
class BitReader {
public:
  BitReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
  uint32_t peek(int bits) const;
  void remove(int bits) { pos_ += size_t(bits); }
  uint32_t read(int bits) { const uint32_t v = peek(bits); remove(bits); return v; }
  bool overflow() const { return pos_ > n_ * 8; }

private:
  const uint8_t* p_;
  size_t n_;
  size_t pos_ = 0;
};

// decodificador huffman do MAME com até 256 códigos e até 16 bits por código
class Huffman {
public:
  Huffman(int numCodes, int maxBits) : num_(numCodes), maxBits_(maxBits), bits_(size_t(numCodes)),
                                       code_(size_t(numCodes)), lookup_(size_t(1) << maxBits) {}
  bool importRle(BitReader& br);
  bool importHuffman(BitReader& br);
  uint32_t decode(BitReader& br) const {
    const uint16_t l = lookup_[br.peek(maxBits_)];
    br.remove(l & 0x1F);
    return l >> 5;
  }

private:
  bool assignCodes();
  void buildLookup();
  int num_, maxBits_;
  std::vector<uint8_t> bits_;
  std::vector<uint32_t> code_;
  std::vector<uint16_t> lookup_;
};

// setor bruto: gera a correção P e Q (bytes 0x81C..0x92F) do jeito do MAME (no Mode 2, com o cabeçalho zerado)
void eccGenerate(uint8_t* sector2352);

}  // namespace indigo::cdrom::chd
