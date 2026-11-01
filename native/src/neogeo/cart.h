// Cartuchos do Neo Geo: leitura dos pacotes .zip no formato do MAME (tabela de 307 jogos gerada da lista de
// software do MAME, com os arquivos achados pelo CRC-32 ou pelo nome; jogos fora da tabela, como os homebrews,
// pelos sufixos .p1 .p2 .s1 .m1 .v1 .v2... .c1 .c2...) e do formato .neo (arquivo único do NeoSD, do MiSTer e do
// Geolith), e as cifras/proteções dos cartuchos:
//  - NEO-CMC42 e NEO-CMC50: gráficos C (xor de dados + embaralhamento de endereços) e camada fixa (que fica no
//    fim da C ROM); CMC50 também cifra o M1 (Z80);
//  - NEO-SMA (KOF99, Garou, Metal Slug 3, KOF2000): P ROM com dados e endereços embaralhados, troca de banco
//    embaralhada, gerador de números aleatórios e o registrador de presença;
//  - NEO-PCM2 (V ROM com endereços trocados; versão nova com xor), P ROM em blocos (KOF2002, Matrimelee,
//    Samurai Shodown V/Special), NEO-PVC (Metal Slug 5, SVC Chaos, KOF2003: P ROM cifrada, RAM de proteção
//    com conversão de cores e troca de banco própria), KOF98 (P ROM cifrada e o "overlay" no cabeçalho),
//    Metal Slug X (desafio-resposta) e PRO-CT0 (Fatal Fury 2, Super Sidekicks).
// Os algoritmos e tabelas das cifras vêm do MAME (src/devices/bus/neogeo, licença BSD-3-Clause).
//
// Sem suporte (mensagem de erro ao abrir): as placas piratas da lista do MAME (tipos "boot_*": kof2002 Magic
// Plus, kof10th, svcboot/svcplus, ms5plus, kf2k3bl/pl/upl, cthd2003, kog, lans2004, garoubl, samsho5b, sbp...),
// clones "split" sem o pai no mesmo .zip. Carregam mas sem o periférico: V-Liner (botões próprios), os
// mahjong (controle de mahjong), The Irritating Maze (trackball e BIOS própria), multijogador por cabo e 4
// jogadores. O formato .neo cobre as mesmas placas pelo NGH, como o Geolith.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace indigo::neogeo {

enum Board {
  BOARD_DEFAULT = 0,
  BOARD_CT0,     // Fatal Fury 2, Super Sidekicks
  BOARD_KOF98,   // overlay em 0x100
  BOARD_MSLUGX,  // desafio-resposta em 0x2FFFE0
  BOARD_SMA,     // NEO-SMA (variante em smaType)
  BOARD_PVC,     // NEO-PVC
  BOARD_RAM8K,   // 8 KB de RAM com bateria em 0x200000 (Jockey Grand Prix, V-Liner)
};

enum SmaType { SMA_KOF99 = 0, SMA_GAROU, SMA_GAROUH, SMA_MSLUG3, SMA_MSLUG3A, SMA_KOF2000 };

struct SmaInfo {
  uint32_t rng[2];       // leituras que devolvem o número aleatório (0 = não tem)
  uint32_t bankAddr;     // escrita que troca o banco
  uint8_t bits[6];       // bits do valor escrito que formam o número do banco (do bit 0 ao 5)
  uint32_t offset[64];   // deslocamento de cada banco a partir de 0x100000
};
extern const SmaInfo SMA_INFO[6];

struct Cart {
  std::vector<uint8_t> p;   // programa do 68000 (big-endian; 0x000000-0x0FFFFF fixo, depois os bancos)
  std::vector<uint8_t> s;   // camada fixa (tamanho potência de 2)
  std::vector<uint8_t> m;   // programa do Z80 (já decifrado)
  std::vector<uint8_t> v1;  // ADPCM-A
  std::vector<uint8_t> v2;  // ADPCM-B (vazio = mesma da ADPCM-A)
  std::vector<uint8_t> c;   // sprites (C1/C2 intercaladas byte a byte)
  std::string title, setName, note;
  uint32_t ngh = 0;
  int board = BOARD_DEFAULT;
  int smaType = 0;
  int fixBank = 0;  // 0 nenhuma, 1 por linha, 2 por tile (S ROM de 512 KB)
};

// .neo: cabeçalho "NEO" 1
bool isNeoFile(const std::vector<uint8_t>& data);
Cart loadNeo(const std::vector<uint8_t>& data);
// .zip no formato do MAME (lança runtime_error com o motivo quando falta arquivo ou o jogo não tem suporte)
Cart loadZipSet(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo);
// o .zip parece um jogo de Neo Geo (pela tabela ou pelos sufixos)? o nome do arquivo ajuda a decidir
bool looksLikeNeoGeoZip(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo);

}  // namespace indigo::neogeo
