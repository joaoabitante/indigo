// Tabela de jogos do CPS1/CPS2 (gerada de dados do MAME por gerar_tabela.py) e montagem das memórias a partir
// do pacote .zip no formato do MAME: acha o jogo pelo nome do pacote e pelos CRC-32 dos arquivos de dentro,
// e coloca cada arquivo no lugar certo (intercalações de 16 e 64 bits, ROM_CONTINUE, cópias).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace indigo::cps {

// Configuração do CPS-B de cada placa (endereços relativos ao início dos registradores do CPS-B; -1 = não há)
struct CpsbConfig {
  int cpsbAddr, cpsbValue;           // registrador de identificação e o valor que o jogo espera ler
  int mult1, mult2, multLo, multHi;  // multiplicação de 16 x 16 bits (proteção das placas CPS-B-21)
  int unk1, unk2, unk3;
  int layerCtrl;                     // controle das camadas (ordem e ligadas)
  int prio[4];                       // máscaras de prioridade das tintas
  int palCtrl;                       // páginas da paleta copiadas
  int layerMask[5];                  // bits de "ligada" de scroll1, scroll2, scroll3, estrelas 1, estrelas 2
};

// Faixa de códigos de tile aceita pelo PAL da placa B (tipo: 1 objetos, 2 scroll1, 4 scroll2, 8 scroll3,
// 16 estrelas). Os códigos estão na escala de tiles 8x8 (64 bytes)
struct GfxRange {
  uint8_t type;
  uint32_t start, end;
  uint8_t bank;
};
struct GfxMapper {
  uint32_t bankSize[4];
  uint16_t first, count;
};

struct RomFile {
  const char* name;
  uint32_t crc, size;
};

// modos de carga: 0 byte a byte, 1 bytes alternados (16 bits), 2 palavras trocadas, 3 palavra a cada 8 bytes,
// 4 byte a cada 8 bytes, 5 byte a cada 4 bytes, 6 palavras sem troca, 7 preencher, 8 copiar de outra região
struct RomLoad {
  uint8_t region, mode;
  uint16_t file;  // índice em kFiles; no modo 7 é o valor; no modo 8 é a região de origem
  uint32_t dst, len, src;
};

enum Region { RG_MAIN = 0, RG_GFX, RG_AUDIO, RG_OKI, RG_QSOUND, RG_KEY, RG_STARS, RG_COUNT };

// bandeiras de GameDef::flags
enum : uint8_t {
  GF_12MHZ = 1,      // CPS1: 68000 a 12 MHz
  GF_CPSB_C0 = 2,    // CPS1 (sf2ee/sf2ue): CPS-B em 0x8001c0 em vez de 0x800140
  GF_CPSB_MIRROR = 4,
  GF_6BUTTONS = 8,
  GF_4PLAYERS = 16,  // CPS2: 3 ou 4 jogadores (o botão 4 fica no IN0)
  GF_SINGLE = 32,    // CPS2 de placa única: sem controle de volume digital
};

struct GameDef {
  const char* name;
  const char* parent;
  const char* title;
  uint8_t kind;  // 1 = CPS1, 2 = CPS2
  uint8_t rot;   // 0, 1 = 90°, 2 = 180°, 3 = 270° (sentido horário, como o ROTxx do MAME)
  uint8_t flags;
  uint32_t regionSize[7];
  uint16_t firstLoad, numLoads;
  uint8_t cpsb, mapper;
  uint8_t in2, in3, out2;  // CPS1: portas extras da placa C (relativas ao CPS-B)
  uint8_t dsw[3];          // CPS1: chaves DIP padrão (A, B, C)
};

const CpsbConfig& cpsbConfig(int i);
const GfxMapper& gfxMapper(int i);
const GfxRange& gfxRange(int i);

// procura o jogo no pacote; devolve nullptr se não reconhecer. `faltando` recebe a lista de arquivos ausentes
// quando o nome do pacote é conhecido mas o .zip está incompleto.
const GameDef* identify(const std::vector<uint8_t>& zip, const std::string& nomeMinusculo, std::string* faltando);

// monta as regiões de memória (lança runtime_error se faltar arquivo ou o .zip estiver danificado)
void loadRegions(const GameDef& g, const std::vector<uint8_t>& zip, std::vector<uint8_t> (&regions)[RG_COUNT]);

int gameCount();
const GameDef& gameAt(int i);
const GameDef* gameByName(const std::string& name);
// cargas e arquivos de um jogo (para os testes montarem pacotes sintéticos)
const RomLoad& loadAt(const GameDef& g, int i);
const RomFile& fileAt(int i);

}  // namespace indigo::cps
