// Cifra do programa do CPS2 (veja cps2crypt.cpp): leitura da chave do arquivo .key (20 bytes, como nos pacotes
// atuais do MAME) e decifração das instruções.
#pragma once
#include <cstdint>
#include <vector>

namespace indigo::cps {

struct Cps2Key {
  uint32_t key[2] = {0, 0};       // chave mestra de 64 bits
  uint32_t lower = 0, upper = 0;  // faixa cifrada (endereços em bytes)
  bool valid = false;
};

Cps2Key cps2ParseKey(const std::vector<uint8_t>& keyFile);
// dec = rom com as palavras da faixa cifrada decifradas (rom em bytes, palavras big-endian)
void cps2Decrypt(const std::vector<uint8_t>& rom, std::vector<uint8_t>& dec, const Cps2Key& key);
uint16_t cps2DecryptWord(uint16_t enc, uint32_t wordAddr, const Cps2Key& key);
uint16_t cps2EncryptWord(uint16_t plain, uint32_t wordAddr, const Cps2Key& key);  // inverso (para testes)

}  // namespace indigo::cps
