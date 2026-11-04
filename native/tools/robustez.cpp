// Teste de robustez: ROMs de lixo aleatório com cabeçalhos válidos (cada núcleo executa código sem sentido),
// arquivos minúsculos/vazios e save states corrompidos, truncados ou de outro console. Nada pode travar nem
// acessar memória indevida (rodar com AddressSanitizer/UBSan); um estado inválido deve ser recusado com
// exceção e o jogo em andamento tem de continuar igual.
// uso: indigo-robustez [rodadas] [estadosAdulteradosPorRom] [semente]
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <filesystem>
#include <fstream>

#include "../src/fds/fds.h"
#include "../src/cdrom/cdrom.h"
#include "../src/core/core.h"
#include "../src/neogeocd/neogeocd.h"
#include "../src/segacd/segacd.h"
#include "../src/pcecd/pcecd.h"
#include "../src/ps1/ps1.h"
#include "../src/saturn/saturn.h"
#include "../src/tdo/tdo.h"
#include "../src/dc/dc.h"

using namespace indigo;

namespace {
uint32_t x = 12345;
uint32_t rnd() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }

std::vector<uint8_t> randomBytes(size_t n) {
  std::vector<uint8_t> v(n);
  for (auto& b : v) b = static_cast<uint8_t>(rnd() >> 24);
  return v;
}

// ROM aleatória com o cabeçalho de cada console (para escolher o núcleo)
std::vector<uint8_t> makeRom(int kind, size_t size) {
  std::vector<uint8_t> r = randomBytes(size);
  switch (kind) {
    case 0: {  // Game Boy: logo da Nintendo nos 4 primeiros bytes que o detector olha
      const uint8_t logo[4] = {0xCE, 0xED, 0x66, 0x66};
      std::memcpy(&r[0x104], logo, 4);
      break;
    }
    case 29: {  // Nintendo 64: cabeçalho .z64 (partida HLE em 0x80000400, tipo de save aleatório) e código aleatório
      const uint8_t h[16] = {0x80, 0x37, 0x12, 0x40, 0, 0, 0, 0x0F, 0x80, 0, 0x04, 0, 0, 0, 0x14, 0x4C};
      std::memcpy(&r[0], h, 16);
      std::memcpy(&r[0x3B], "NEDE", 4);
      static const uint8_t SAVES[] = {0, 1, 2, 3, 5};
      r[0x3F] = static_cast<uint8_t>(SAVES[rnd() % 5] << 4);
      break;
    }
    case 30: {  // Nintendo DS: programas do ARM9 e do ARM7 em lugares e tamanhos aleatórios (às vezes fora do arquivo)
      auto w32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) r[o + i] = static_cast<uint8_t>(v >> (8 * i)); };
      const uint32_t n = static_cast<uint32_t>(size);
      w32(0x20, 0x200 + (rnd() % 8 ? 0 : rnd() % n)); w32(0x24, 0x02000000 + (rnd() & 0xFFFC)); w32(0x28, 0x02000000);
      w32(0x2C, rnd() % 8 ? n / 2 : 1 + rnd() % 0x3BFE00);
      w32(0x30, n / 2 + (rnd() % 8 ? 0 : rnd() % n)); w32(0x34, 0x037F8000); w32(0x38, 0x037F8000);
      w32(0x3C, rnd() % 8 ? std::min<uint32_t>(n / 4, 0x10000) : rnd() % 0x3BFE00);
      break;
    }
    case 27:  // Atari Jaguar: marca do cartucho em 0x400 e partida em 0x802000
      r[0x400] = r[0x401] = r[0x402] = r[0x403] = 4;
      r[0x404] = 0; r[0x405] = 0x80; r[0x406] = 0x20; r[0x407] = 0;
      break;
    case 1: case 2: {  // Master System / Game Gear: "TMR SEGA" em 0x7FF0
      std::memcpy(&r[0x7FF0], "TMR SEGA", 8);
      r[0x7FFF] = kind == 2 ? 0x6C : 0x4C;
      break;
    }
    case 4: {  // Game Boy Advance: assinatura do logo em 4..7, desvio para 0xC0 e, às vezes, as marcas de save/relógio
      const uint8_t logo[4] = {0x24, 0xFF, 0xAE, 0x51};
      std::memcpy(&r[4], logo, 4);
      r[0] = 0x2E; r[1] = 0x00; r[2] = 0x00; r[3] = 0xEA;  // b 0xC0
      static const char* TAGS[] = {"EEPROM_V", "SRAM_V", "FLASH_V", "FLASH512_V", "FLASH1M_V", "SIIRTC_V"};
      const int nt = static_cast<int>(rnd() % 3);
      for (int k = 0; k < nt; k++) {
        const char* t = TAGS[rnd() % 6];
        const size_t at = 0x1000 + ((rnd() % (size - 0x1100)) & ~size_t(3));
        std::memcpy(&r[at], t, std::strlen(t));
      }
      break;
    }
    case 5: {  // NES: cabeçalho iNES com um dos mapeadores suportados, PRG e CHR aleatórios
      static const int MAPPERS[] = {0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 19, 21, 22, 23, 24, 25, 26, 34, 66, 69, 71, 79, 85, 140, 206};
      const int mapper = MAPPERS[rnd() % (sizeof MAPPERS / sizeof MAPPERS[0])];
      const int prg = 1 << (rnd() % 6), chr = (rnd() & 1) ? 0 : 1 << (rnd() % 5);
      r = randomBytes(16 + prg * 0x4000 + chr * 0x2000);
      r[0] = 'N'; r[1] = 'E'; r[2] = 'S'; r[3] = 0x1A;
      r[4] = static_cast<uint8_t>(prg); r[5] = static_cast<uint8_t>(chr);
      r[6] = static_cast<uint8_t>(((mapper & 0xF) << 4) | (rnd() & 0x0B));  // espelhamento, bateria, 4 telas
      r[7] = static_cast<uint8_t>(mapper & 0xF0);
      for (int k = 8; k < 16; k++) r[k] = 0;
      // vetores de reset/NMI/IRQ dentro da ROM
      const size_t vec = 16 + prg * 0x4000 - 6;
      for (int k = 0; k < 6; k += 2) { r[vec + k] = static_cast<uint8_t>(rnd()); r[vec + k + 1] = static_cast<uint8_t>(0x80 | (rnd() & 0x7F)); }
      break;
    }
    case 6: {  // SNES: cabeçalho LoROM ou HiROM com o vetor de reset na ROM (às vezes com SRAM)
      const bool hi = rnd() & 1;
      const size_t h = hi ? 0xFFC0 : 0x7FC0;
      if (r.size() < h + 0x40) r.resize(h + 0x40);
      std::memcpy(&r[h], "INDIGO ROBUSTEZ      ", 21);
      r[h + 0x15] = static_cast<uint8_t>(hi ? 0x21 : 0x20);
      r[h + 0x16] = static_cast<uint8_t>((rnd() & 1) ? 0x02 : 0x00);
      r[h + 0x18] = static_cast<uint8_t>(rnd() % 6);
      r[h + 0x1C] = 0x00; r[h + 0x1D] = 0x00; r[h + 0x1E] = 0xFF; r[h + 0x1F] = 0xFF;
      r[h + 0x3C] = static_cast<uint8_t>(rnd()); r[h + 0x3D] = static_cast<uint8_t>(0x80 | (rnd() & 0x7F));
      for (int k = 0x24; k < 0x3C; k += 2) { r[h + k] = static_cast<uint8_t>(rnd()); r[h + k + 1] = static_cast<uint8_t>(0x80 | (rnd() & 0x7F)); }
      if (hi && rnd() % 3 == 0) {
        // SPC7110 (às vezes com o relógio): ROM de 3 ou 5 MB e um programa no reset que grava bytes da ROM em todos
        // os registradores do chip ($4800-$4842: tabela, descompressão, porta de dados, contas, mapa e relógio) e
        // lê a porta de descompressão (fluxos de lixo nos três modos), a porta de dados, $50:xxxx, a RAM e a ROM
        // de dados
        std::vector<uint8_t> big = randomBytes((rnd() & 1) ? 0x300000 : 0x500000);
        std::memcpy(big.data(), r.data(), std::min(r.size(), big.size()));
        r.swap(big);
        r[h + 0x15] = 0x3A;
        r[h + 0x16] = static_cast<uint8_t>((rnd() & 1) ? 0xF9 : 0xF5);
        r[h + 0x18] = 0x03;
        const uint8_t prog[41] = {0xE8, 0xBD, 0x00, 0x90, 0x9D, 0x00, 0x48, 0xBD, 0x00, 0x91, 0x9D, 0x30, 0x48,
                                  0xAD, 0x00, 0x48, 0xAD, 0x10, 0x48, 0xAF, 0x34, 0x12, 0x50, 0xAD, 0x41, 0x48,
                                  0x9D, 0x00, 0x60, 0xBD, 0x00, 0x70, 0xAD, 0x1A, 0x48, 0xAF, 0x00, 0x00, 0xD0,
                                  0x80, 0xD7};
        std::memcpy(&r[0x8000], prog, sizeof prog);
        r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
      } else if (!hi && rnd() % 3 == 0) {
        // Super FX: o programa no reset liga o GSU com registradores e endereço aleatórios, espera parar e repete
        // (o GSU roda bytes aleatórios da ROM ou da RAM: instruções, PLOT, cache, desvios, buffers)
        r[h + 0x16] = static_cast<uint8_t>((rnd() & 1) ? 0x15 : 0x13);
        r[h - 3] = static_cast<uint8_t>(5 + rnd() % 3);
        const uint8_t prog[39] = {0xA9, static_cast<uint8_t>(rnd() & 0x27), 0x8D, 0x3A, 0x30,
                                  0xA9, static_cast<uint8_t>(rnd()), 0x8D, 0x37, 0x30,
                                  0xA9, static_cast<uint8_t>(rnd()), 0x8D, 0x39, 0x30,
                                  0xA9, static_cast<uint8_t>(rnd()), 0x8D, 0x34, 0x30,
                                  0xA9, static_cast<uint8_t>(rnd()), 0x8D, 0x1E, 0x30,
                                  0xA9, static_cast<uint8_t>(rnd()), 0x8D, 0x1F, 0x30,
                                  0xAD, 0x30, 0x30, 0x29, 0x20, 0xD0, 0xF9, 0x80, 0xD9};
        std::memcpy(&r[0], prog, sizeof prog);
        r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
      } else if (!hi && rnd() % 3 == 0) {
        // SA-1: o programa no reset liga o SA-1 (que roda bytes aleatórios da ROM: DMA, conversões, contas, MMC,
        // BW-RAM como bitmap) e depois escreve bytes da ROM em todos os registradores do lado do SNES, lendo
        // $2300+ e a BW-RAM (que pode estar em conversão tipo 1) sem parar
        r[h + 0x15] = 0x23;
        r[h + 0x16] = static_cast<uint8_t>((rnd() & 1) ? 0x35 : 0x33);
        const uint8_t prog[19] = {0x9C, 0x00, 0x22, 0xE8, 0xBD, 0x00, 0x90, 0x9D, 0x00, 0x22,
                                  0xBD, 0x00, 0x23, 0xBF, 0x00, 0x00, 0x40, 0x80, 0xF0};
        std::memcpy(&r[0], prog, sizeof prog);
        r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
      } else if (!hi && rnd() % 3 == 0) {
        // S-DD1: o programa no reset troca as janelas do MMC e faz DMA de endereço fixo de $C0-$FF para a WRAM com
        // descompressão ligada nos canais escolhidos por bytes da ROM (fluxos comprimidos de lixo), e lê o
        // endereço do fluxo pela CPU
        r[h + 0x15] = 0x32;
        r[h + 0x16] = static_cast<uint8_t>((rnd() & 1) ? 0x45 : 0x43);
        const uint8_t prog[74] = {0xE8, 0xBD, 0x00, 0x90, 0x8D, 0x04, 0x48, 0xBD, 0x00, 0x91, 0x8D, 0x06, 0x48, 0xA9, 0x08,
                                  0x8D, 0x00, 0x43, 0xA9, 0x80, 0x8D, 0x01, 0x43, 0xBD, 0x00, 0x92, 0x8D, 0x02, 0x43, 0xBD,
                                  0x00, 0x93, 0x8D, 0x03, 0x43, 0xBD, 0x00, 0x94, 0x09, 0xC0, 0x8D, 0x04, 0x43, 0xBD, 0x00,
                                  0x95, 0x8D, 0x05, 0x43, 0x9C, 0x06, 0x43, 0xA9, 0xFF, 0x8D, 0x00, 0x48, 0xBD, 0x00, 0x96,
                                  0x8D, 0x01, 0x48, 0xA9, 0x01, 0x8D, 0x0B, 0x42, 0xAF, 0x50, 0x34, 0xC2, 0x80, 0xB6};
        std::memcpy(&r[0], prog, sizeof prog);
        r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
      } else if (!hi && rnd() % 2 == 0) {
        // DSP-1: o programa no reset manda bytes que mudam (comandos e parâmetros) e lê DR e SR sem parar; metade
        // das vezes com um "arquivo do chip" de lixo, para o µPD7725 rodar instruções aleatórias
        r[h + 0x16] = static_cast<uint8_t>((rnd() & 1) ? 0x05 : 0x03);
        const uint8_t prog[16] = {0xE8, 0x8A, 0x8F, 0x00, 0x80, 0x30, 0xAF, 0x00, 0x80, 0x30, 0xAF, 0x00, 0xC0, 0x30, 0x80, 0xF0};
        std::memcpy(&r[0], prog, sizeof prog);
        r[h + 0x3C] = 0x00; r[h + 0x3D] = 0x80;
        setFirmware("dsp1", (rnd() & 1) ? randomBytes(8192) : std::vector<uint8_t>{});
      }
      break;
    }
    case 10: {  // ColecoVision: cabeçalho AA 55 e início do jogo em 8024h; a BIOS é lixo aleatório ou um salto ao cartucho
      r[0] = 0xAA; r[1] = 0x55; r[0x0A] = 0x24; r[0x0B] = 0x80;
      if (rnd() & 1) setFirmware("coleco", randomBytes(8192));
      else { auto b = std::vector<uint8_t>(8192, 0); b[0] = 0xC3; b[1] = 0x24; b[2] = 0x80; setFirmware("coleco", b); }
      break;
    }
    case 18: {  // Mega Drive: "SEGA" em 0x100 e início em 0x200 (código aleatório), pilha no topo da RAM
      std::memcpy(&r[0x100], "SEGA MEGA DRIVE ", 16);
      r[0] = 0x00; r[1] = 0xFF; r[2] = 0xFF; r[3] = 0xFE;
      r[4] = 0; r[5] = 0; r[6] = 0x02; r[7] = 0x00;
      if (rnd() & 1) { std::memcpy(&r[0x1B0], "RA", 2); r[0x1B2] = 0xF8; r[0x1B3] = 0x20; r[0x1B4] = 0; r[0x1B5] = 0x20; r[0x1B6] = 0; r[0x1B7] = 0x01; r[0x1B8] = 0; r[0x1B9] = 0x20; r[0x1BA] = 0x3F; r[0x1BB] = 0xFF; }
      break;
    }
    case 21: {  // 32X: "SEGA 32X", liga o adaptador e solta os SH-2 (código aleatório nos três processadores)
      std::memcpy(&r[0x100], "SEGA 32X        ", 16);
      r[0] = 0x00; r[1] = 0xFF; r[2] = 0xFF; r[3] = 0xFE;
      r[4] = 0; r[5] = 0; r[6] = 0x02; r[7] = 0x00;
      const uint8_t start[] = {0x13, 0xFC, 0x00, 0x03, 0x00, 0xA1, 0x51, 0x01};  // move.b #3,$A15101
      std::memcpy(&r[0x200], start, sizeof start);
      auto w32 = [&](size_t a, uint32_t v) { r[a] = uint8_t(v >> 24); r[a + 1] = uint8_t(v >> 16); r[a + 2] = uint8_t(v >> 8); r[a + 3] = uint8_t(v); };
      w32(0x3D4, rnd() % size); w32(0x3D8, rnd() & 0x3FFFF); w32(0x3DC, rnd() % 0x48000);
      w32(0x3E0, 0x06000000 + (rnd() & 0x3FFFE)); w32(0x3E4, (rnd() & 1) ? 0x02000000 + (rnd() % size) : 0x06000000 + (rnd() & 0x3FFFE));
      w32(0x3E8, 0x06000000 + (rnd() & 0x3FFFC)); w32(0x3EC, rnd());
      if (rnd() & 1) { std::memcpy(&r[0x1B0], "RA", 2); r[0x1B2] = 0xF8; r[0x1B3] = 0x20; r[0x1B4] = 0; r[0x1B5] = 0x20; r[0x1B6] = 0; r[0x1B7] = 0x01; r[0x1B8] = 0; r[0x1B9] = 0x20; r[0x1BA] = 0x3F; r[0x1BB] = 0xFF; }
      break;
    }
    case 17:  // PC Engine: código aleatório desde o banco 0 (o vetor de reset fica no fim do primeiro banco de 8 KB)
      r[0x1FFE] = 0x00; r[0x1FFF] = 0xE0;
      break;
    case 25:  // Mega Duck (.md2): código aleatório desde $0000
    case 26:  // Watara Supervision: código e vetores aleatórios
      break;
    case 16:  // Virtual Boy: vetor de reset no fim da ROM (código aleatório)
      break;
    case 15:  // Atari Lynx sem cabeçalho (.lyx): o carregador cifrado é lixo aleatório
      break;
    case 14: {  // Vectrex: cabeçalho "g GCE" e código aleatório; BIOS de lixo ou uma que salta para o cartucho
      std::memcpy(&r[0], "g GCE", 5);
      if (rnd() & 1) setFirmware("vectrex", randomBytes(8192));
      else { auto b = std::vector<uint8_t>(8192, 0x12); b[0x1FFE] = 0x00; b[0x1FFF] = 0x20; setFirmware("vectrex", b); }
      break;
    }
    case 13: {  // Atari 7800 sem cabeçalho: vetor de reset para o começo do cartucho (código aleatório)
      r[size - 4] = 0x00; r[size - 3] = static_cast<uint8_t>(0x10000 - size >= 0x4000 ? (0x10000 - size) >> 8 : 0x40);
      break;
    }
    case 12: {  // WonderSwan: rodapé com o salto de início para código aleatório (Color metade das vezes, com save)
      r[size - 16] = 0xEA; r[size - 15] = 0x00; r[size - 14] = 0x00; r[size - 13] = 0x00; r[size - 12] = 0xF0;
      r[size - 9] = rnd() & 1;
      r[size - 6] = static_cast<uint8_t>((rnd() & 1) ? 0x02 : 0x10);
      break;
    }
    case 11:  // SG-1000: código aleatório desde 0000h
      break;
    case 9: {  // MSX: cabeçalho "AB" e endereço de início no cartucho (código aleatório)
      r[0] = 'A'; r[1] = 'B'; r[2] = 0x10; r[3] = 0x40;
      break;
    }
    case 8: {  // Atari 2600: vetor de reset apontando para o começo do cartucho (código aleatório)
      r[size - 4] = 0x00; r[size - 3] = 0xF0;
      break;
    }
    case 7: {  // Pokémon Mini: cabeçalho "MN" em $2100 e "NINTENDO" em $21A4, código aleatório depois dele
      r[0x2100] = 'M'; r[0x2101] = 'N';
      std::memcpy(&r[0x21A4], "NINTENDO", 8);
      break;
    }
    default: {  // Neo Geo Pocket
      std::memcpy(&r[0], "COPYRIGHT BY SNK CORPORATION", 28);
      const uint32_t start = 0x200000 + (rnd() % size);
      r[0x1C] = start & 0xFF; r[0x1D] = (start >> 8) & 0xFF; r[0x1E] = (start >> 16) & 0xFF;
      r[0x23] = (rnd() & 1) ? 0x10 : 0x00;
      break;
    }
  }
  return r;
}

// Neo Geo CD: BIOS aleatória com a estrutura de uma BIOS do CD (pilha, reset e vetores na BIOS) cujo programa
// escreve bytes e palavras da própria BIOS em registradores do CD ($FF0000), no LC8951, na comunicação com a
// unidade e na janela de upload, às vezes com uma receita de DMA conhecida (montado de um trecho de assembly do
// 68000); as interrupções vão para um RTE ou para código aleatório. O CD é um ISO 9660 com IPL.TXT e setores
// aleatórios, às vezes com uma faixa de áudio aleatória.
const uint8_t NGCD_PROG[] = {
    0x46, 0xFC, 0x20, 0x00, 0x43, 0xF9, 0x00, 0xC0, 0x10, 0x00, 0xB3, 0xFC, 0x00, 0xC7, 0xF0, 0x00,
    0x65, 0x06, 0x43, 0xF9, 0x00, 0xC0, 0x10, 0x00, 0x72, 0x00, 0x12, 0x19, 0xE1, 0x49, 0x12, 0x19,
    0x14, 0x19, 0xE1, 0x4A, 0x14, 0x19, 0x16, 0x19, 0x02, 0x43, 0x00, 0x07, 0x67, 0x30, 0x0C, 0x43,
    0x00, 0x01, 0x67, 0x40, 0x0C, 0x43, 0x00, 0x02, 0x67, 0x58, 0x02, 0x41, 0x01, 0xFF, 0x41, 0xF9,
    0x00, 0xFF, 0x00, 0x00, 0x08, 0x01, 0x00, 0x00, 0x67, 0x0A, 0x11, 0x82, 0x10, 0x00, 0x18, 0x30,
    0x10, 0x00, 0x60, 0xB6, 0x31, 0x82, 0x10, 0x00, 0x38, 0x30, 0x10, 0x00, 0x60, 0xAC, 0x02, 0x81,
    0x00, 0x00, 0xFF, 0xFE, 0x06, 0x81, 0x00, 0xE0, 0x00, 0x00, 0x20, 0x41, 0x30, 0x82, 0x18, 0x28,
    0x00, 0x01, 0x60, 0x96, 0x02, 0x42, 0x00, 0x07, 0xD4, 0x42, 0x41, 0xFA, 0x00, 0x3E, 0x33, 0xF0,
    0x20, 0x00, 0x00, 0xFF, 0x00, 0x7E, 0x13, 0xFC, 0x00, 0x40, 0x00, 0xFF, 0x00, 0x61, 0x60, 0x00,
    0xFF, 0x7A, 0x13, 0xC2, 0x00, 0xFF, 0x01, 0x01, 0x13, 0xC1, 0x00, 0xFF, 0x01, 0x03, 0x18, 0x39,
    0x00, 0xFF, 0x01, 0x03, 0x13, 0xC1, 0x00, 0xFF, 0x01, 0x63, 0x13, 0xC2, 0x00, 0xFF, 0x01, 0x65,
    0x18, 0x39, 0x00, 0xFF, 0x01, 0x61, 0x60, 0x00, 0xFF, 0x52, 0xFE, 0x3D, 0xFF, 0xC5, 0xFE, 0xF5,
    0xFF, 0xCD, 0xE2, 0xDD, 0xFC, 0x2D, 0xCF, 0xFD, 0xFF, 0x89,
};

void put32(std::vector<uint8_t>& b, size_t o, uint32_t v) {
  b[o] = static_cast<uint8_t>(v >> 24); b[o + 1] = static_cast<uint8_t>(v >> 16);
  b[o + 2] = static_cast<uint8_t>(v >> 8); b[o + 3] = static_cast<uint8_t>(v);
}

std::unique_ptr<Core> makeNeoGeoCd(const std::vector<uint8_t>* save) {
  std::vector<uint8_t> b = randomBytes(0x80000);
  put32(b, 0, 0x0010F300);
  put32(b, 4, 0xC00400);
  for (size_t v = 8; v < 0x100; v += 4) put32(b, v, (rnd() & 1) ? 0xC00300 : 0xC00000 + (rnd() & 0x7FFFE));
  b[0x300] = 0x4E; b[0x301] = 0x73;  // RTE
  std::memcpy(&b[0x400], NGCD_PROG, sizeof NGCD_PROG);
  setFirmware("neogeocd", b);
  // CD: setores de dados Mode 1 (ISO 9660 com o IPL.TXT no diretório raiz) e talvez uma faixa de áudio
  const uint32_t sectors = 24 + rnd() % 120;
  std::vector<uint8_t> data(static_cast<size_t>(sectors) * 2048);
  for (auto& x : data) x = static_cast<uint8_t>(rnd() >> 24);
  uint8_t* pvd = &data[16 * 2048];
  std::memset(pvd, 0, 2048);
  pvd[0] = 1; std::memcpy(pvd + 1, "CD001", 5);
  uint8_t* root = pvd + 156;
  root[0] = 34; root[2] = 20; root[10] = 0x00; root[11] = 0x08;
  uint8_t* dir = &data[20 * 2048];
  std::memset(dir, 0, 2048);
  dir[0] = 42; dir[2] = 21; dir[10] = static_cast<uint8_t>(rnd()); dir[11] = static_cast<uint8_t>(rnd() & 0x0F);
  dir[32] = 9; std::memcpy(dir + 33, "IPL.TXT;1", 9);
  for (int k = 0, o = 42; k < 6; k++, o += 40) {  // arquivos com nomes, posições e tamanhos aleatórios
    static const char* NAMES[] = {"A.PRG;1", "B.SPR;1", "C.FIX;1", "D.PCM;1", "E.Z80;1", "F.PRG;1"};
    dir[o] = 40; dir[o + 2] = static_cast<uint8_t>(rnd() % (sectors + 8)); dir[o + 10] = static_cast<uint8_t>(rnd());
    dir[o + 11] = static_cast<uint8_t>(rnd()); dir[o + 12] = static_cast<uint8_t>(rnd() & 3);
    dir[o + 32] = 7; std::memcpy(dir + o + 33, NAMES[k], 7);
  }
  const char* ipl = (rnd() & 1) ? "A.PRG,0,0\r\nB.SPR,1,F0000\r\nC.FIX,0,0\r\nD.PCM,1,FFF00\r\nE.Z80,0,0\r\n" : "F.PRG,9,1FFFFE\r\n";
  std::memcpy(&data[21 * 2048], ipl, std::strlen(ipl));
  const auto dirPath = std::filesystem::temp_directory_path();
  const std::string bin = (dirPath / "indigo-robustez-ngcd.bin").string(), cue = (dirPath / "indigo-robustez-ngcd.cue").string();
  {
    std::ofstream f(bin, std::ios::binary);
    std::vector<uint8_t> raw(2352);
    for (uint32_t l = 0; l < sectors; l++) {
      std::fill(raw.begin(), raw.end(), 0);
      std::fill(raw.begin() + 1, raw.begin() + 11, 0xFF);
      raw[15] = 1;
      std::memcpy(&raw[16], &data[l * 2048], 2048);
      f.write(reinterpret_cast<const char*>(raw.data()), 2352);
    }
    const bool audio = rnd() & 1;
    if (audio) {
      const auto a = randomBytes(2352u * (75 + rnd() % 300));
      f.write(reinterpret_cast<const char*>(a.data()), static_cast<std::streamsize>(a.size()));
    }
    std::ofstream c(cue);
    c << "FILE \"indigo-robustez-ngcd.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n";
    if (audio) {
      const uint32_t m = sectors / 4500, sec = sectors / 75 % 60, fr = sectors % 75;
      char t[64];
      std::snprintf(t, sizeof t, "  TRACK 02 AUDIO\r\n    INDEX 01 %02u:%02u:%02u\r\n", m, sec, fr);
      c << t;
    }
  }
  std::string err;
  auto disc = cdrom::open(cue, &err);
  if (!disc) throw std::runtime_error("robustez: CD não abriu: " + err);
  return std::make_unique<neogeocd::NeoGeoCdCore>(std::move(disc), save);
}

constexpr int KINDS = 32;
const char* NAMES[KINDS] = {"Game Boy", "Master System", "Game Gear", "Neo Geo Pocket", "Game Boy Advance", "NES", "SNES", "Pokémon Mini", "Atari 2600", "MSX", "ColecoVision", "SG-1000", "WonderSwan", "Atari 7800", "Vectrex", "Atari Lynx", "Virtual Boy", "PC Engine", "Mega Drive", "Neo Geo CD", "PC Engine CD", "32X", "PlayStation", "3DO", "Famicom Disk System", "Mega Duck", "Watara Supervision", "Atari Jaguar", "Saturn", "Nintendo 64", "Nintendo DS", "Dreamcast"};
const char* EXTS[KINDS] = {"x.gb", "x.sms", "x.gg", "x.ngc", "x.gba", "x.nes", "x.sfc", "x.min", "x.a26", "x.rom", "x.col", "x.sg", "x.ws", "x.a78", "x.vec", "x.lyx", "x.vb", "x.pce", "x.md", "x.cue", "x.cue", "x.32x", "x.exe", "x.cue", "x.fds", "x.md2", "x.sv", "x.j64", "x.cue", "x.z64", "x.nds", "x.gdi"};

std::unique_ptr<Core> makePceCd(const std::vector<uint8_t>* save);
std::unique_ptr<Core> makePs1(const std::vector<uint8_t>* save);
std::unique_ptr<Core> makeTdo(const std::vector<uint8_t>* save);
std::unique_ptr<Core> makeSaturn(const std::vector<uint8_t>* save);
std::unique_ptr<Core> makeDc(const std::vector<uint8_t>* save);

// Famicom Disk System: lados com o bloco 1 certo e o resto em blocos de tamanho aleatório (ou lixo puro); BIOS de
// lixo com os vetores em $E000–$FFFF ou a BIOS de teste do t7_fds.sh
std::unique_ptr<Core> makeFds(const std::vector<uint8_t>* save) {
  const int lados = 1 + static_cast<int>(rnd() % 3);
  std::vector<uint8_t> d = {'F', 'D', 'S', 0x1A, static_cast<uint8_t>(lados)};
  d.resize(16, 0);
  for (int s = 0; s < lados; s++) {
    std::vector<uint8_t> l = randomBytes(65500);
    l[0] = 1;
    std::memcpy(&l[1], "*NINTENDO-HVC*", 14);
    size_t p = 56;
    if (rnd() & 1) {
      l[p] = 2; l[p + 1] = static_cast<uint8_t>(rnd() % 8); p += 2;
      while (p + 17 < l.size() && (rnd() % 8)) {
        l[p] = 3;
        const uint16_t n = static_cast<uint16_t>(rnd() % 0x3000);
        l[p + 13] = static_cast<uint8_t>(n); l[p + 14] = static_cast<uint8_t>(n >> 8);
        p += 16;
        if (p + n + 1 >= l.size()) break;
        l[p] = 4;
        p += 1 + n;
      }
    }
    d.insert(d.end(), l.begin(), l.end());
  }
  std::vector<uint8_t> bios = randomBytes(0x2000);
  for (int k = 0x1FFA; k < 0x2000; k += 2) bios[k + 1] = static_cast<uint8_t>(0xE0 | (rnd() & 0x1F));
  if (rnd() & 1) {
    if (FILE* f = std::fopen("/root/indigo-env/roms/fds/testbios.rom", "rb")) {
      std::vector<uint8_t> b(0x2000);
      if (std::fread(b.data(), 1, b.size(), f) == b.size()) bios.swap(b);
      std::fclose(f);
    }
  }
  auto c = std::make_unique<fds::FdsCore>(d, (rnd() & 1) ? "x.fds" : "x.qd", save, &bios);
  if (rnd() & 1) c->setDiskSide(static_cast<int>(rnd() % 4) - 1);
  return c;
}

std::unique_ptr<Core> createKind(int kind, size_t size, const std::vector<uint8_t>* save) {
  if (kind == 19) return makeNeoGeoCd(save);
  if (kind == 20) return makePceCd(save);
  if (kind == 22) return makePs1(save);
  if (kind == 23) return makeTdo(save);
  if (kind == 24) return makeFds(save);
  if (kind == 28) return makeSaturn(save);
  if (kind == 31) return makeDc(save);
  return createCore(makeRom(kind, size), EXTS[kind], save);
}

// Sega CD: disco sintético na memória (faixa de dados com "SEGADISCSYSTEM" e lixo, uma faixa de áudio com 2 s de
// pregap) e a BIOS de teste do t7_segacd.sh se existir (INDIGO_SCD_BIOS ou /root/indigo-env/roms/segacd), senão
// uma BIOS de lixo com o cabeçalho de BIOS do CD
class MemDisc : public cdrom::Disc {
public:
  MemDisc(uint32_t dataSectors, uint32_t audioSectors, uint32_t seed) : seed_(seed), data_(dataSectors) {
    cdrom::Track t;
    t.number = 1; t.type = cdrom::TrackType::Mode1; t.start = 0; t.length = dataSectors;
    tracks_.push_back(t);
    t.number = 2; t.type = cdrom::TrackType::Audio; t.pregap = 150; t.start = dataSectors + 150; t.length = audioSectors;
    tracks_.push_back(t);
    total_ = dataSectors + 150 + audioSectors;
    name_ = "aleatorio";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, cdrom::RAW_SECTOR);
    if (lba >= total_) return false;
    uint32_t s = seed_ ^ (lba * 2654435761u) ^ 0x5bd1e995u;
    for (int i = 0; i < cdrom::RAW_SECTOR; i++) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; out[i] = static_cast<uint8_t>(s); }
    if (lba < data_) {
      std::memset(out, 0xFF, 12); out[0] = out[11] = 0;
      const uint32_t a = lba + 150;
      auto bcd = [](uint32_t v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); };
      out[12] = bcd(a / 4500); out[13] = bcd(a / 75 % 60); out[14] = bcd(a % 75); out[15] = 1;
      if (lba == 0) std::memcpy(out + 16, "SEGADISCSYSTEM  ", 16);
    } else if (lba < data_ + 150) std::memset(out, 0, cdrom::RAW_SECTOR);
    return true;
  }
private:
  uint32_t seed_, data_;
};

std::vector<uint8_t> segaCdBios(int round) {
  std::vector<uint8_t> b;
  const char* env = std::getenv("INDIGO_SCD_BIOS");
  const std::string path = env ? env : "/root/indigo-env/roms/segacd/bios_teste.bin";
  if (round % 2 == 0) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
      b.resize(0x20000);
      if (std::fread(b.data(), 1, b.size(), f) != b.size()) b.clear();
      std::fclose(f);
    }
  }
  if (b.empty()) {
    b = randomBytes(0x20000);
    std::memcpy(&b[0x100], "SEGA MEGA DRIVE ", 16);
    b[0x180] = 'B'; b[0x181] = 'R';
  }
  return b;
}


// disco de lixo na memória: dados, áudio e dados de novo (setores gerados do LBA)
class PceMemDisc : public cdrom::Disc {
public:
  explicit PceMemDisc(uint32_t seed) : seed_(seed) {
    const uint32_t a = 50 + seed % 300, b = 20 + (seed >> 8) % 300, c = 10 + (seed >> 16) % 100;
    tracks_ = {{1, cdrom::TrackType::Mode1, 0, 0, a}, {2, cdrom::TrackType::Audio, a, 0, b}, {3, cdrom::TrackType::Mode1, a + b, 0, c}};
    total_ = a + b + c;
    name_ = "lixo";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    if (lba >= total_) { std::memset(out, 0, cdrom::RAW_SECTOR); return false; }
    uint32_t h = seed_ ^ (lba * 2654435761u);
    for (int i = 0; i < cdrom::RAW_SECTOR; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; out[i] = uint8_t(h >> 24); }
    if (tracks_[size_t(trackAt(lba))].type != cdrom::TrackType::Audio) out[15] = 1;
    return true;
  }
private:
  uint32_t seed_;
};

// "System Card" de lixo: depois de mapear a E/S e a RAM, instruções aleatórias que leem e escrevem os
// registradores do CD ($1800-$180F, $18C0, $1A00-$1AFF), trocam bancos e às vezes ligam as IRQs; em laço
std::vector<uint8_t> makePceCdBios() {
  std::vector<uint8_t> b = randomBytes(0x40000);
  std::vector<uint8_t> c = {0x78, 0xD4, 0xA9, 0xFF, 0x53, 0x01, 0xA9, 0xF8, 0x53, 0x02, 0xA2, 0xFF, 0x9A};  // sei csh tam0 tam1 txs
  if (rnd() & 1) c.insert(c.end(), {0xA9, 0x00, 0x8D, 0x02, 0x14, 0x58});  // IRQs ligadas e cli
  const int n = 200 + int(rnd() % 400);
  for (int i = 0; i < n; i++) {
    uint16_t a = uint16_t(0x1800 + (rnd() % 16));
    const uint32_t k = rnd() % 16;
    if (k == 0) a = uint16_t(0x18C0 + (rnd() % 4));
    else if (k < 3) a = uint16_t(0x1A00 + (rnd() % 256));
    const uint8_t v = uint8_t(rnd() >> 24);
    switch (rnd() % 6) {
      case 0: case 1: c.insert(c.end(), {0xA9, v, 0x8D, uint8_t(a), uint8_t(a >> 8)}); break;  // lda #v; sta a
      case 2: c.insert(c.end(), {0xAD, uint8_t(a), uint8_t(a >> 8)}); break;                    // lda a
      case 3: c.insert(c.end(), {0xA9, uint8_t(0x40 + rnd() % 0x48), 0x53, 0x04, 0xA9, v, 0x8D, uint8_t(rnd()), uint8_t(0x40 + rnd() % 0x20), 0xAD, uint8_t(rnd()), 0x40}); break;  // tam2, sta/lda na janela
      case 4: c.insert(c.end(), {0xA2, uint8_t(rnd()), 0xCA, 0xD0, 0xFD}); break;               // espera
      default: c.insert(c.end(), {0xAD, 0x00, 0x18, 0xAD, 0x01, 0x18}); break;                 // status e dado
    }
  }
  c.insert(c.end(), {0x4C, 0x0D, 0xE0});  // jmp $E00D (depois do txs)
  if (c.size() > 0x1FF0) c.resize(0x1FF0);
  std::copy(c.begin(), c.end(), b.begin());
  for (int v = 0; v < 5; v++) { b[0x1FF6 + v * 2] = 0x00; b[0x1FF7 + v * 2] = 0xE0; }  // todos os vetores: $E000
  b[0x1FF6] = 0xF0; b[0x1FF7] = 0xFF; b[0x1FF8] = 0xF0; b[0x1FF9] = 0xFF; b[0x1FFA] = 0xF0; b[0x1FFB] = 0xFF;  // IRQs: rti
  b[0x1FF0] = 0x40;
  std::memcpy(&b[0x3FFB6], "PC Engine CD-ROM SYSTEM", 23);
  return b;
}

std::unique_ptr<Core> makePceCd(const std::vector<uint8_t>* save) {
  const auto bios = makePceCdBios();
  const pcecd::Model m = pcecd::Model(rnd() % 3);
  return std::make_unique<pcecd::PceCdCore>(std::make_unique<PceMemDisc>(rnd()), save, &bios, &m);
}
// PlayStation: programa PS-X EXE de lixo (instruções aleatórias misturadas com leituras e escritas nos registradores
// de E/S: GPU, DMA, timers, interrupções, SIO0, CD, SPU, MDEC), carregado direto ou depois da BIOS. A BIOS é a de
// teste do t9_ps1.sh (INDIGO_PS1_BIOS ou /root/indigo-env/roms/ps1/bios/teste.bin) ou 512 KB de lixo.
std::unique_ptr<Core> makePs1(const std::vector<uint8_t>* save) {
  static const uint16_t IO[] = {0x1040, 0x1044, 0x1048, 0x104A, 0x104E, 0x1070, 0x1074, 0x1080, 0x1084, 0x1088, 0x10A0,
                                0x10A4, 0x10A8, 0x10B0, 0x10B4, 0x10B8, 0x10E0, 0x10E4, 0x10E8, 0x10F0, 0x10F4, 0x1100,
                                0x1104, 0x1108, 0x1110, 0x1114, 0x1118, 0x1120, 0x1124, 0x1128, 0x1800, 0x1801, 0x1802,
                                0x1803, 0x1810, 0x1810, 0x1810, 0x1814, 0x1820, 0x1824, 0x1D80, 0x1DAA, 0x1DA6, 0x1DA8,
                                0x1C00, 0x1C08, 0x1060, 0x1000, 0x1010};
  std::vector<uint32_t> code = {0x3C081F80};  // lui t0, 0x1F80
  const int n = 2000 + int(rnd() % 6000);
  for (int i = 0; i < n; i++) {
    const uint32_t k = rnd() % 10;
    const uint32_t off = IO[rnd() % (sizeof IO / sizeof IO[0])];
    if (k < 3) {
      code.push_back(0x3C090000 | (rnd() & 0xFFFF));                 // lui t1, aleatório
      code.push_back(0x35290000 | (rnd() & 0xFFFF));                 // ori t1, t1, aleatório
      static const uint32_t ST[] = {0xAD090000, 0xA5090000, 0xA1090000};  // sw/sh/sb t1, off(t0)
      code.push_back(ST[rnd() % 3] | ((rnd() % 3 == 0) ? (off & ~3u) : off & ~1u));
    } else if (k < 5) {
      code.push_back(0x8D0A0000 | (off & ~3u));                      // lw t2, off(t0)
    } else if (k < 6) {
      code.push_back(0x24050000 | (rnd() & 0x3FF));                  // addiu a1, zero, n
      code.push_back(0x24A5FFFF);                                    // addiu a1, a1, -1
      code.push_back(0x14A0FFFE);                                    // bnez a1, -1
      code.push_back(0);
    } else {
      code.push_back(rnd());                                         // instrução qualquer
    }
  }
  code.push_back(0x08004000);  // j 0x80010000
  code.push_back(0);
  std::vector<uint8_t> exe(0x800 + code.size() * 4, 0);
  std::memcpy(exe.data(), "PS-X EXE", 8);
  auto put = [&](size_t o, uint32_t v) { std::memcpy(&exe[o], &v, 4); };
  put(0x10, 0x80010000); put(0x18, 0x80010000); put(0x1C, uint32_t(code.size() * 4)); put(0x30, 0x801FFF00);
  std::memcpy(&exe[0x800], code.data(), code.size() * 4);
  std::vector<uint8_t> bios;
  const char* env = std::getenv("INDIGO_PS1_BIOS");
  if (rnd() & 1) {
    if (FILE* f = std::fopen(env ? env : "/root/indigo-env/roms/ps1/bios/teste.bin", "rb")) {
      bios.resize(512 * 1024);
      if (std::fread(bios.data(), 1, bios.size(), f) != bios.size()) bios.clear();
      std::fclose(f);
    }
  }
  if (bios.empty()) bios = randomBytes(512 * 1024);
  const auto mode = (rnd() & 1) ? ps1::Ps1Core::ExeBoot::Direct : ps1::Ps1Core::ExeBoot::Bios;
  auto c = std::make_unique<ps1::Ps1Core>(exe, "lixo.exe", save, mode, &bios);
  static int scale = 0;  // resolução interna 1x, 2x e 4x, uma de cada vez (sem mexer na sequência aleatória)
  c->setResolution(1 << (scale++ % 3));
  return c;
}

// 3DO: disco Opera sintético (rótulo certo, resto aleatório, talvez uma faixa de áudio) e uma "BIOS" que é um
// programa ARM aleatório escrevendo valores aleatórios nos registradores do MADAM, do CLIO (CD, DMA, DSP, timers,
// interrupções), no SPORT e na NVRAM, com laços e instruções quaisquer; às vezes a BIOS de teste do t9b_3do.sh
// (INDIGO_3DO_BIOS ou /root/indigo-env/roms/3do/bios/teste.bin)
class TdoMemDisc : public cdrom::Disc {
public:
  explicit TdoMemDisc(uint32_t seed) : seed_(seed) {
    const uint32_t a = 20 + seed % 400, b = (seed >> 9) & 1 ? 75 + (seed >> 12) % 200 : 0;
    tracks_.push_back({1, cdrom::TrackType::Mode1, 0, 0, a});
    if (b) tracks_.push_back({2, cdrom::TrackType::Audio, a + 150, 150, b});
    total_ = a + (b ? 150 + b : 0);
    data_ = a;
    name_ = "lixo3do";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, cdrom::RAW_SECTOR);
    if (lba >= total_) return false;
    uint32_t h = seed_ ^ (lba * 2654435761u);
    for (int i = 0; i < cdrom::RAW_SECTOR; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; out[i] = static_cast<uint8_t>(h); }
    if (lba < data_) {
      std::memset(out, 0, 16);
      std::memset(out + 1, 0xFF, 10);
      out[15] = 1;
      if (lba == 0) { out[16] = 1; std::memset(out + 17, 0x5A, 5); out[22] = 1; std::memcpy(out + 56, "CD-ROM", 6); }
    }
    return true;
  }
private:
  uint32_t seed_, data_ = 0;
};

std::unique_ptr<Core> makeTdo(const std::vector<uint8_t>* save) {
  static const uint32_t REGS[] = {0x03300100, 0x03300130, 0x03300134, 0x03300138, 0x0330013C, 0x03300400, 0x03300404,
                                  0x03300408, 0x0330040C, 0x03300540, 0x03300544, 0x03300570, 0x03300574, 0x03300580,
                                  0x033005A0, 0x03300600, 0x03300640, 0x03300680, 0x033007FC, 0x03400008, 0x0340000C,
                                  0x03400040, 0x03400044, 0x03400048, 0x0340004C, 0x03400060, 0x03400068, 0x03400100,
                                  0x03400104, 0x03400118, 0x03400200, 0x03400208, 0x03400220, 0x03400304, 0x03400308,
                                  0x03400500, 0x03400540, 0x03400580, 0x03400580, 0x03400580, 0x034005C0, 0x034017E8,
                                  0x034017F0, 0x03401800, 0x03401804, 0x03401808, 0x03403000, 0x03403040, 0x03200000,
                                  0x03284000, 0x03140010, 0x00100000, 0x00200000, 0x033005A4, 0x03300008,
                                  0x03300004, 0x03300410, 0x03300414, 0x03300418, 0x03300500, 0x03300504,
                                  0x03300578, 0x03400050, 0x03400300, 0x03400400, 0x034017D0, 0x034017FC,
                                  0x03402000, 0x03402004, 0x03403400, 0x03403C00, 0x03400028, 0x03400084};
  std::vector<uint32_t> code;
  auto movConst = [&](int rd, uint32_t v) {  // mov + orr de 8 bits em 8 bits
    code.push_back(0xE3A00000u | uint32_t(rd) << 12 | 0x400 | (v >> 24));        // mov rd, #(v>>24) ror 8
    code.push_back(0xE3800000u | uint32_t(rd) << 16 | uint32_t(rd) << 12 | 0x800 | ((v >> 16) & 0xFF));  // ror 16
    code.push_back(0xE3800000u | uint32_t(rd) << 16 | uint32_t(rd) << 12 | 0xC00 | ((v >> 8) & 0xFF));   // ror 24
    code.push_back(0xE3800000u | uint32_t(rd) << 16 | uint32_t(rd) << 12 | (v & 0xFF));
  };
  code.push_back(0xE51FF004);  // ldr pc, [pc, #-4]: a CPU parte do 0 (ROM por cima da DRAM) e pula para a ROM
  code.push_back(0x03000008);
  code.push_back(0xE321F013);  // msr cpsr_c, #0x13 (libera IRQ/FIQ)
  movConst(13, 0x001F0000);
  const int n = 300 + int(rnd() % 1500);
  for (int i = 0; i < n; i++) {
    const uint32_t k = rnd() % 10;
    if (k < 4) {
      movConst(0, REGS[rnd() % (sizeof REGS / sizeof REGS[0])] + ((rnd() & 3) == 0 ? (rnd() & 0x3FC) : 0));
      movConst(1, rnd());
      code.push_back((rnd() & 3) ? 0xE5801000u : 0xE5C01000u);  // str/strb r1, [r0]
    } else if (k < 6) {
      movConst(0, REGS[rnd() % (sizeof REGS / sizeof REGS[0])]);
      code.push_back(0xE5902000u);  // ldr r2, [r0]
    } else if (k < 7) {
      code.push_back(0xE3A05000u | (rnd() & 0xFF));  // mov r5, #n
      code.push_back(0xE2555001u);                   // subs r5, r5, #1
      code.push_back(0x1AFFFFFDu);                   // bne -1
    } else {
      uint32_t op = rnd();
      if ((op >> 25 & 7) == 5) op &= ~0x0E000000u;  // sem desvios aleatórios para fora do programa
      code.push_back(op);
    }
  }
  code.push_back(0xEA000000u | ((uint32_t(-int32_t(code.size()) - 2)) & 0xFFFFFF));  // volta ao começo
  std::vector<uint8_t> bios(0x100000, 0);
  for (size_t i = 0; i < code.size() && i * 4 + 4 <= bios.size(); i++) {
    bios[i * 4] = uint8_t(code[i] >> 24); bios[i * 4 + 1] = uint8_t(code[i] >> 16);
    bios[i * 4 + 2] = uint8_t(code[i] >> 8); bios[i * 4 + 3] = uint8_t(code[i]);
  }
  if (rnd() & 1) {
    const char* env = std::getenv("INDIGO_3DO_BIOS");
    if (FILE* f = std::fopen(env ? env : "/root/indigo-env/roms/3do/bios/teste.bin", "rb")) {
      std::vector<uint8_t> b(0x100000);
      if (std::fread(b.data(), 1, b.size(), f) == b.size()) bios.swap(b);
      std::fclose(f);
    }
  }
  return std::make_unique<tdo::TdoCore>(std::make_unique<TdoMemDisc>(rnd()), save, &bios);
}
// Saturn: disco sintético ("SEGA SEGASATURN " e lixo, talvez uma faixa de áudio) e uma "BIOS" de código SH-2
// aleatório (os dois SH-2, os registradores e as memórias recebem valores quaisquer); às vezes a BIOS de teste do
// t7_saturn.sh (INDIGO_SATURN_BIOS ou /root/indigo-env/roms/saturn/bios/teste.bin)
class SaturnMemDisc : public cdrom::Disc {
public:
  explicit SaturnMemDisc(uint32_t seed) : seed_(seed) {
    const uint32_t a = 20 + seed % 400, b = (seed >> 9) & 1 ? 75 + (seed >> 12) % 200 : 0;
    tracks_.push_back({1, cdrom::TrackType::Mode1, 0, 0, a});
    if (b) tracks_.push_back({2, cdrom::TrackType::Audio, a + 150, 150, b});
    total_ = a + (b ? 150 + b : 0);
    data_ = a;
    name_ = "lixosaturn";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, cdrom::RAW_SECTOR);
    if (lba >= total_) return false;
    uint32_t h = seed_ ^ (lba * 2654435761u);
    for (int i = 0; i < cdrom::RAW_SECTOR; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; out[i] = static_cast<uint8_t>(h); }
    if (lba < data_) {
      std::memset(out, 0, 16);
      std::memset(out + 1, 0xFF, 10);
      out[15] = 1;
      if (lba == 0) std::memcpy(out + 16, "SEGA SEGASATURN ", 16);
    }
    return true;
  }
private:
  uint32_t seed_, data_ = 0;
};

std::unique_ptr<Core> makeSaturn(const std::vector<uint8_t>* save) {
  std::vector<uint8_t> bios = randomBytes(0x80000);
  auto put32 = [&](size_t o, uint32_t v) { bios[o] = uint8_t(v >> 24); bios[o + 1] = uint8_t(v >> 16); bios[o + 2] = uint8_t(v >> 8); bios[o + 3] = uint8_t(v); };
  for (size_t v = 0; v < 128; v++) put32(v * 4, 0x400 + (rnd() & 0x7FFE));  // vetores dentro da ROM
  put32(4, 0x06002000); put32(12, 0x06002000);
  // registradores com endereços do Saturn para o código aleatório usar como ponteiros
  static const uint32_t REGS[] = {0x25FE0000, 0x25FE0080, 0x25FE00A0, 0x25D00000, 0x25F80000, 0x25E00000, 0x25C00000,
                                  0x25B00400, 0x25A00000, 0x25890008, 0x20100000, 0x20180000, 0x21000000, 0x21800000};
  for (size_t i = 0x400; i + 6 < 0x8000; i += 6 + (rnd() % 64) * 2) {
    // mov.l @(disp,pc),rN de um endereço da tabela; o resto do código é aleatório
    const int rn = int(rnd() % 14);
    bios[i] = uint8_t(0xD0 | rn); bios[i + 1] = 0;
    const size_t lit = (i + 4) & ~size_t(3);
    if (lit + 4 <= bios.size()) put32(lit, REGS[rnd() % (sizeof REGS / sizeof REGS[0])] + (rnd() & 0xFC));
  }
  if (rnd() & 1) {
    const char* env = std::getenv("INDIGO_SATURN_BIOS");
    if (FILE* f = std::fopen(env ? env : "/root/indigo-env/roms/saturn/bios/teste.bin", "rb")) {
      std::vector<uint8_t> b(0x80000);
      if (std::fread(b.data(), 1, b.size(), f) == b.size()) bios.swap(b);
      std::fclose(f);
    }
  }
  return std::make_unique<saturn::SaturnCore>(std::make_unique<SaturnMemDisc>(rnd()), save, &bios);
}

// Dreamcast: disco sintético ("SEGA SEGAKATANA ", às vezes uma faixa na área GD em LBA 45000 e uma de áudio), uma
// BIOS de código SH-4 aleatório e a flash aleatória; às vezes a BIOS e a flash de teste do t7_dc.sh
// (INDIGO_DC_BIOS ou /root/indigo-env/roms/dc/bios). Os registradores da CPU recebem endereços do hardware.
class DcMemDisc : public cdrom::Disc {
public:
  explicit DcMemDisc(uint32_t seed) : seed_(seed) {
    const uint32_t a = 20 + seed % 200;
    tracks_.push_back({1, cdrom::TrackType::Mode1, 0, 0, a});
    total_ = a;
    if ((seed >> 9) & 1) { tracks_.push_back({2, cdrom::TrackType::Audio, a + 150, 150, 75 + (seed >> 12) % 100}); total_ = tracks_.back().start + tracks_.back().length; }
    if ((seed >> 10) & 1) { tracks_.push_back({uint8_t(tracks_.size() + 1), cdrom::TrackType::Mode1, 45000, 0, 64}); total_ = 45064; gd_ = true; }
    name_ = "lixodc";
  }
  bool readRaw(uint32_t lba, uint8_t* out) override {
    std::memset(out, 0, cdrom::RAW_SECTOR);
    if (lba >= total_) return false;
    uint32_t h = seed_ ^ (lba * 2654435761u);
    for (int i = 0; i < cdrom::RAW_SECTOR; i++) { h ^= h << 13; h ^= h >> 17; h ^= h << 5; out[i] = static_cast<uint8_t>(h); }
    bool data = false;
    for (const auto& t : tracks_) data |= t.type == cdrom::TrackType::Mode1 && lba >= t.start && lba < t.start + t.length;
    if (data) {
      std::memset(out, 0, 16);
      std::memset(out + 1, 0xFF, 10);
      out[15] = 1;
      if (lba == 0 || (gd_ && lba == 45000)) std::memcpy(out + 16, "SEGA SEGAKATANA ", 16);
    }
    return true;
  }
private:
  uint32_t seed_;
  bool gd_ = false;
};

std::unique_ptr<Core> makeDc(const std::vector<uint8_t>* save) {
  std::vector<uint8_t> boot = randomBytes(dc::BOOT_SIZE), flash = randomBytes(dc::FLASH_SIZE);
  if (rnd() & 1) {
    const char* env = std::getenv("INDIGO_DC_BIOS");
    const std::string dir = env ? env : "/root/indigo-env/roms/dc/bios";
    for (auto [name, buf] : {std::pair{"/teste_boot.bin", &boot}, std::pair{"/teste_flash.bin", &flash}})
      if (FILE* f = std::fopen((dir + name).c_str(), "rb")) {
        std::vector<uint8_t> b(buf->size());
        if (std::fread(b.data(), 1, b.size(), f) == b.size()) buf->swap(b);
        std::fclose(f);
      }
  }
  auto c = std::make_unique<dc::DcCore>(std::make_unique<DcMemDisc>(rnd()), save, &boot, &flash);
  static const uint32_t BASES[] = {0xA05F6800, 0xA05F8000, 0xA05F7000, 0xA0700000, 0xA0800000, 0xA4000000,
                                   0xA5000000, 0x10000000, 0xE0000000, 0xFFD80000, 0xFFA00000, 0xA05F9000,
                                   0xA05F7400, 0xA05F6C00, 0xFF000000, 0x8C000000};
  for (int i = 0; i < 16; i++) c->sys().cpu.r[i] = BASES[rnd() % 16] + (rnd() & 0xFC);
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  const int rounds = argc > 1 ? std::atoi(argv[1]) : 20;
  const int flipTrials = argc > 2 ? std::atoi(argv[2]) : 1;  // estados com bytes trocados por ROM
  if (argc > 3) x = static_cast<uint32_t>(std::strtoul(argv[3], nullptr, 10)) | 1;  // semente
  int roms = 0, statesOk = 0, statesRefused = 0, problems = 0;

  // arquivos minúsculos e vazios: não podem derrubar o detector nem os núcleos
  for (size_t n : {0, 1, 0x40, 0x14F, 0x150, 0x200, 0x3FFF}) {
    for (const char* ext : {"a.gb", "a.gbc", "a.sms", "a.gg", "a.ngp", "a.bin", "a.gba", "a.agb", "a.nes", "a.sfc", "a.min", "a.a26", "a.a52", "a.int", "a.itv", "a.sgb", "a.fds", "a.qd", "a.rom", "a.mx1", "a.mx2", "a.col", "a.sg", "a.ws", "a.a78", "a.vec", "a.lnx", "a.lyx", "a.vb", "a.duck", "a.md0", "a.sv", "a.pce", "a.sgx", "a.md", "a.smd", "a.gen", "a.32x", "a.tgc", "a.j64", "a.jag", "a.abs", "a.z64", "a.n64", "a.v64", "a.nds"}) {
      auto data = randomBytes(n);
      if (n >= 16 && std::strcmp(ext, "a.nes") == 0) { data[0] = 'N'; data[1] = 'E'; data[2] = 'S'; data[3] = 0x1A; }
      std::unique_ptr<Core> core;
      try { core = createCore(data, ext, nullptr); } catch (const std::exception&) { continue; }  // NES truncado: recusado
      if (core) for (int f = 0; f < 5; f++) core->runFrame();
    }
  }

  for (int round = 0; round < rounds; round++) {
    for (int kind = 0; kind < KINDS; kind++) {
      const size_t size = kind == 15 ? static_cast<size_t>(0x20000) << (rnd() % 2) : kind == 14 ? static_cast<size_t>(0x2000) << (rnd() % 3) : kind == 13 ? static_cast<size_t>(0x4000) << (rnd() % 2) : kind == 8 ? static_cast<size_t>(0x1000) << (rnd() % 4) : kind == 3 ? 0x8000 + (rnd() % 0x80000) : ((kind == 0 || kind == 4) ? 0x8000 << (rnd() % 6) : 0x8000 << (rnd() % 4));
      const auto save = randomBytes(rnd() % 0x9000);
      std::unique_ptr<Core> core;
      try {
        core = createKind(kind, size, (rnd() & 1) ? &save : nullptr);
      } catch (const std::exception&) {
        // bytes aleatórios podem parecer um esquema de bancos do 2600 sem suporte (recusa com mensagem, certo)
        // DS: programas fora do arquivo são recusados com mensagem (também certo)
        if (kind == 8 || kind == 15 || kind == 30) continue;
        throw;
      }
      if (!core) { std::printf("ROM de %s não reconhecida\n", NAMES[kind]); problems++; continue; }
      roms++;
      for (int f = 0; f < 120; f++) { core->setKeys(rnd() & 0x3FF); core->runFrame(); }
      // PlayStation: troca a resolução interna no meio do jogo (a VRAM ampliada é refeita da de verdade)
      if (auto* p = dynamic_cast<ps1::Ps1Core*>(core.get())) { p->setResolution(p->resolution() == 4 ? 2 : p->resolution() * 2); core->runFrame(); }
      const size_t n = core->audioAvailable();
      std::vector<float> L(n), R(n);
      core->pullAudio(L.data(), R.data(), n);
      core->exportSave();

      // estado válido: salvar, rodar, carregar e conferir que volta ao mesmo ponto
      const auto st = core->saveState();
      std::vector<uint32_t> fb1(core->framebuffer(), core->framebuffer() + core->width() * core->height());
      core->loadState(st);
      // salvar -> carregar -> salvar tem de dar os mesmos bytes (nenhum campo válido "corrigido" na leitura)
      if (const auto st2 = core->saveState(); st2 != st) {
        size_t at = 0;
        while (at < st.size() && at < st2.size() && st[at] == st2[at]) at++;
        std::printf("%s: estado carregado não volta igual ao salvo (byte %zu de %zu)\n", NAMES[kind], at, st.size());
        problems++;
      }
      for (int f = 0; f < 3; f++) core->runFrame();

      // estados corrompidos: truncados, com bytes trocados, vazios, aleatórios e de outro console
      std::vector<std::vector<uint8_t>> bad = {
        {}, randomBytes(64), std::vector<uint8_t>(st.begin(), st.begin() + st.size() / 2),
        std::vector<uint8_t>(st.begin(), st.begin() + 4),
      };
      for (int t = 0; t < flipTrials; t++) {
        auto flipped = st;
        for (int k = 0; k < 8; k++) flipped[rnd() % flipped.size()] ^= static_cast<uint8_t>(1 + (rnd() & 0x7F));
        bad.push_back(flipped);
        // bytes trocados só no começo e no fim (registradores e variáveis pequenas, longe das memórias grandes)
        auto edges = st;
        for (int k = 0; k < 4; k++) {
          const size_t span = std::min<size_t>(edges.size(), 2048);
          const size_t at = (rnd() & 1) ? rnd() % span : edges.size() - 1 - rnd() % span;
          edges[at] ^= static_cast<uint8_t>(1 + (rnd() & 0x7F));
        }
        bad.push_back(edges);
      }
      const int nx = (kind + 1) % KINDS;
      const int ok = (nx == 8 || nx == 15 || nx == 30) ? 0 : nx;  // 2600 e DS aleatórios podem ser recusados
      auto other = createKind(ok, 0x8000, nullptr);
      other->runFrame();
      bad.push_back(other->saveState());

      for (const auto& b : bad) {
        const auto before = core->saveState();
        try {
          core->loadState(b);
          statesOk++;  // byte trocado num lugar inofensivo pode carregar; o núcleo tem de seguir rodando
          for (int f = 0; f < 30; f++) core->runFrame();
          core->loadState(before);
        } catch (const std::exception&) {
          statesRefused++;
          // recusado: o jogo em andamento tem de estar exatamente como antes
          if (core->saveState() != before) { std::printf("%s: estado recusado alterou o jogo!\n", NAMES[kind]); problems++; }
        }
      }
      (void)fb1;
    }
    // Sega CD (núcleo criado a partir do disco, não da ROM)
    for (int k = 0; k < 2; k++) {
      const auto bios = segaCdBios(round + k);
      const auto save = randomBytes(rnd() % 0x3000);
      auto core = std::make_unique<segacd::SegaCdCore>(std::make_unique<MemDisc>(200 + rnd() % 400, 75 + rnd() % 300, rnd()),
                                                       bios, (rnd() & 1) ? &save : nullptr, (rnd() & 1) != 0);
      roms++;
      core->setSampleRate((rnd() & 1) ? 44100 : 48000);
      for (int f = 0; f < 240; f++) { core->setKeys(rnd() & 0x3FF); core->runFrame(); }
      const size_t n = core->audioAvailable();
      std::vector<float> L(n), R(n);
      core->pullAudio(L.data(), R.data(), n);
      core->exportSave();
      const auto st = core->saveState();
      core->loadState(st);
      if (core->saveState() != st) { std::printf("Sega CD: estado carregado não volta igual ao salvo\n"); problems++; }
      for (int f = 0; f < 3; f++) core->runFrame();
      std::vector<std::vector<uint8_t>> bad = {{}, randomBytes(64), std::vector<uint8_t>(st.begin(), st.begin() + static_cast<long>(st.size() / 2))};
      for (int t = 0; t < flipTrials; t++) {
        auto flipped = st;
        for (int j = 0; j < 8; j++) flipped[rnd() % flipped.size()] ^= static_cast<uint8_t>(1 + (rnd() & 0x7F));
        bad.push_back(flipped);
        // registradores e relógios: começo do estado do Sega CD (depois do estado do Mega Drive) e o fim
        auto edges = st;
        for (int j = 0; j < 6; j++) {
          const size_t span = std::min<size_t>(edges.size(), 4096);
          const size_t pos = (j & 1) ? edges.size() - 1 - rnd() % span : rnd() % edges.size();
          edges[pos] ^= static_cast<uint8_t>(1 + (rnd() & 0x7F));
        }
        bad.push_back(edges);
      }
      auto other = createCore(makeRom(18, 0x8000), "x.md", nullptr);
      other->runFrame();
      bad.push_back(other->saveState());
      for (const auto& b : bad) {
        const auto before = core->saveState();
        try {
          core->loadState(b);
          statesOk++;
          for (int f = 0; f < 30; f++) core->runFrame();
          core->loadState(before);
        } catch (const std::exception&) {
          statesRefused++;
          if (core->saveState() != before) { std::printf("Sega CD: estado recusado alterou o jogo!\n"); problems++; }
        }
      }
    }
    std::printf("rodada %d/%d: %d ROMs, %d estados recusados, %d aceitos, %d problemas\n", round + 1, rounds, roms, statesRefused, statesOk, problems);
    std::fflush(stdout);
  }
  std::printf("ROMs aleatórias executadas: %d | estados corrompidos recusados: %d | aceitos (e seguiram rodando): %d | problemas: %d\n",
              roms, statesRefused, statesOk, problems);
  return problems ? 1 : 0;
}
