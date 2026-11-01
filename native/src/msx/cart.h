// Cartucho do MSX: ROM simples (8/16/32/48/64 KB, posição achada pelo cabeçalho "AB") e os mapeadores de
// megaROM mais comuns, escolhidos por heurística como no openMSX/blueMSX (contagem das instruções
// LD (nnnn),A que gravam nos endereços dos registradores de cada mapeador):
//   Konami (sem SCC)  bancos de 8 KB; 4000h fixo, registradores em 6000h/8000h/A000h
//   Konami SCC        bancos de 8 KB em 5000h/7000h/9000h/B000h; 9000h=3Fh liga o SCC em 9800h–9FFFh
//   ASCII 8 KB        bancos de 8 KB em 6000h/6800h/7000h/7800h (com SRAM: Xanadu, Royal Blood…)
//   ASCII 16 KB       bancos de 16 KB em 6000h/7000h (com SRAM: Hydlide 2, Daisenryaku…)
//   genérico 8 KB     cada gravação em 4000h–BFFFh troca o banco daquela janela
// SRAM dos ASCII: o banco recebe o bit logo acima do maior banco da ROM (ex.: ROM de 128 KB em ASCII16 =
// 8 bancos → bit 10h); aí 8000h–BFFFh mostra a SRAM (8 KB, gravável). Ela vira save de bateria.
// Sem banco de dados de jogos, a SRAM só é ligada quando o jogo grava nessa janela com o bit ligado (ou
// quando já existe um save): até lá o valor vale como espelho da ROM, como num cartucho sem SRAM.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../core/state.h"
#include "audio.h"

namespace indigo::msx {

enum class Mapper : int { PLAIN = 0, KONAMI, KONAMI_SCC, ASCII8, ASCII16, GENERIC8K };
const char* mapperName(Mapper m);
// escolhe o mapeador (e, para ROM simples, o endereço onde ela começa)
Mapper guessMapper(const std::vector<uint8_t>& rom, int& plainStart);

class Cart {
public:
  explicit Cart(const std::vector<uint8_t>& data);
  void reset();

  // ponteiro para a janela de 8 KB (0..7) ou nullptr quando precisa do caminho lento (SCC)
  const uint8_t* readPtr(int region) const { return rd[region]; }
  int read(int addr) const;
  // devolve true se o mapa de memória mudou
  bool write(int addr, int v);

  Mapper mapper = Mapper::PLAIN;
  int plainStart = 0x4000;
  SCC scc;
  std::vector<uint8_t> sram;  // 8 KB (só nos ASCII)
  bool sramUsed = false, sramDirty = false;
  bool hasScc() const { return mapper == Mapper::KONAMI_SCC; }

  void sync(StateIO& io);

private:
  void remap();
  std::vector<uint8_t> rom;  // tamanho potência de 2, múltiplo de 8 KB
  int banks8 = 1;            // número de bancos de 8 KB
  int bank[8] = {};          // banco de 8 KB por janela; -1 = vazio, -2 = SRAM
  bool sramReg[8] = {};      // janela selecionada com o bit da SRAM
  int sramBit = 0;           // bit do registrador que liga a SRAM (0 = sem SRAM)
  bool sccOn = false;
  const uint8_t* rd[8] = {};
};

}  // namespace indigo::msx
