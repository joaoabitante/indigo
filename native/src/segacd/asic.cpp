// ASIC de gráficos do Sega CD (rotação/escala), pela descrição do "Mega-CD Hardware Manual":
//  - fonte: um mapa de selos ("stamp map") na Word RAM 2M. O mapa cobre 256x256 pontos (registrador 58,
//    bit 2 = 0) ou 4096x4096 pontos (bit 2 = 1); cada selo tem 16x16 ou 32x32 pontos (bit 1). Cada palavra do
//    mapa é um selo: bits 0-10 = número (o selo começa em número * 128 bytes; 0 = transparente; em selos de
//    32x32 os 2 bits de baixo não contam), bits 13-14 = giro de 0/90/180/270 graus, bit 15 = espelho horizontal.
//    Um selo é feito de células de 8x8 pontos de 4 bits (32 bytes), guardadas coluna a coluna;
//  - destino: o buffer de imagem, também em células de 8x8 coluna a coluna (o formato de padrão do VDP), com
//    altura de (registrador 5C + 1) células, começando no endereço do registrador 5E e com o deslocamento de
//    pontos do registrador 60 (bits 0-2 horizontal, 3-5 vertical); 62 = largura em pontos, 64 = linhas;
//  - para cada linha um "vetor de traço" de 4 palavras (registrador 66 aponta o primeiro): X e Y iniciais em
//    ponto fixo 13.3 e os passos DX e DY em 5.11 com sinal. Cada ponto da linha lê o mapa na posição (X, Y), o
//    ponto do selo (com giro/espelho) e grava no buffer com o modo de prioridade da Word RAM; fora do mapa o
//    ponto é 0, a menos que a repetição (bit 0 de 58) esteja ligada;
//  - no fim, interrupção de nível 1. O ASIC só funciona com a Word RAM 2M nas mãos do sub-CPU.
// Tempo: cada acesso do ASIC à Word RAM ocupa 3 ciclos do sub-CPU; uma linha faz 4 leituras do vetor de traço,
// 2 leituras por ponto (mapa e selo) e uma escrita por palavra (4 pontos) do buffer. É uma aproximação; o efeito
// visível (a hora da interrupção) bate com a referência dos testes.
#include <algorithm>

#include "segacd.h"

namespace indigo::segacd {

void StampAsic::sync(StateIO& io) {
  io.sync(config); io.sync(busy);
  io.sync(mapReg); io.sync(cellsReg); io.sync(imageReg); io.sync(offsetReg); io.sync(widthReg); io.sync(heightReg);
  io.sync(traceReg); io.sync(clk); io.sync(lineCycles); io.sync(traceAddr); io.sync(line);
  if (io.reading()) {
    clampTo<int64_t>(lineCycles, 1, int64_t(1) << 20);
    traceAddr &= 0x3FFF8;
    line &= 0xFFFF;
  }
}

void SegaCD::asicStart() {
  asic_.busy = true;
  asic_.clk = now();
  asic_.line = 0;
  asic_.traceAddr = (static_cast<uint32_t>(asic_.traceReg) << 2) & 0x3FFF8;
  const int64_t dots = asic_.widthReg & 0x1FF;
  const int64_t writes = ((asic_.offsetReg & 3) + dots + 3) / 4;
  const int64_t accesses = 4 + 2 * dots + writes;
  asic_.lineCycles = accesses * 3 * 4;
}

void SegaCD::asicLine() {
  uint8_t* w = wram_.data();
  auto word = [w](uint32_t a) { a &= 0x3FFFE; return static_cast<uint32_t>((w[a] << 8) | w[a + 1]); };
  const bool repeat = asic_.config & 1, bigStamp = asic_.config & 2, bigMap = asic_.config & 4;
  const uint32_t stampDots = bigStamp ? 32 : 16;
  const uint32_t mapDots = bigMap ? 4096 : 256;
  const uint32_t mapStamps = mapDots / stampDots;
  const uint32_t mapBytes = mapStamps * mapStamps * 2;
  const uint32_t mapBase = (static_cast<uint32_t>(asic_.mapReg) << 2) & 0x3FFFF & ~(mapBytes - 1);
  const uint32_t numberMask = bigStamp ? 0x7FC : 0x7FF;
  const int flipMask = static_cast<int>(stampDots) - 1;
  const uint32_t limit = (mapDots << 11) - 1;  // posições válidas em 13.11

  // vetor de traço: posição 13.3 -> 13.11, passo 5.11 com sinal
  const uint32_t t = asic_.traceAddr;
  uint32_t x = word(t) << 8, y = word(t + 2) << 8;
  const uint32_t dx = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(word(t + 4))));
  const uint32_t dy = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(word(t + 6))));
  asic_.traceAddr = (t + 8) & 0x3FFF8;

  // destino (endereços em pontos = meios bytes): colunas de células com cellsHigh células cada
  const uint32_t cellsHigh = (asic_.cellsReg & 0x1F) + 1u;
  const uint32_t column = cellsHigh * 64;
  const uint32_t rowBase = ((static_cast<uint32_t>(asic_.imageReg) << 3) & 0x7FFC0) + (asic_.offsetReg & 0x38) + asic_.line * 8;
  const uint32_t firstDot = asic_.offsetReg & 7;
  const uint32_t width = asic_.widthReg & 0x1FF;

  for (uint32_t i = 0; i < width; i++, x += dx, y += dy) {
    const uint32_t wrap = repeat ? limit : 0xFFFFFF;
    x &= wrap;
    y &= wrap;
    uint8_t dot = 0;
    if (x <= limit && y <= limit) {
      const uint32_t px = x >> 11, py = y >> 11;
      const uint32_t entry = word(mapBase + ((py / stampDots) * mapStamps + px / stampDots) * 2);
      const uint32_t number = entry & numberMask;
      if (number) {
        int sx = static_cast<int>(px) & flipMask, sy = static_cast<int>(py) & flipMask;
        if (entry & 0x8000) sx ^= flipMask;                              // espelho
        if (entry & 0x4000) { sx ^= flipMask; sy ^= flipMask; }          // meia volta
        if (entry & 0x2000) { const int k = sx; sx = sy ^ flipMask; sy = k; }  // um quarto de volta
        const uint32_t cell = static_cast<uint32_t>(sx >> 3) * (stampDots / 8) + static_cast<uint32_t>(sy >> 3);
        const uint32_t src = (number << 8) + (cell << 6) + static_cast<uint32_t>((sy & 7) * 8 + (sx & 7));
        const uint8_t b = w[(src >> 1) & 0x3FFFF];
        dot = (src & 1) ? (b & 0x0F) : (b >> 4);
      }
    }
    const uint32_t col = firstDot + i;
    const uint32_t dst = rowBase + (col >> 3) * column + (col & 7);
    uint8_t& d = w[(dst >> 1) & 0x3FFFF];
    const uint8_t fresh = (dst & 1) ? static_cast<uint8_t>((d & 0xF0) | dot) : static_cast<uint8_t>((dot << 4) | (d & 0x0F));
    d = blend(d, fresh);
  }
}

void SegaCD::asicRun(int64_t t) {
  if (!asic_.busy) return;
  if (!mode1M_ && ret_) { asic_.clk = t; return; }  // Word RAM 2M do principal: o ASIC espera
  if (t <= asic_.clk) return;
  uint32_t left = asic_.heightReg & 0xFF;
  while (left && asic_.clk < t) {
    // em 1M a Word RAM está dividida entre as CPUs e o ASIC não grava nada útil
    if (!mode1M_) asicLine();
    else asic_.traceAddr = (asic_.traceAddr + 8) & 0x3FFF8;
    asic_.line++;
    left--;
    asic_.clk += asic_.lineCycles;
  }
  asic_.heightReg = static_cast<uint16_t>((asic_.heightReg & 0xFF00) | left);
  if (!left) {
    asic_.busy = false;
    request(1);
  }
}

int64_t SegaCD::asicDeadline() const {
  if (!asic_.busy || (!mode1M_ && ret_)) return INT64_MAX;
  const int left = asic_.heightReg & 0xFF;
  return asic_.clk + static_cast<int64_t>(left > 0 ? left - 1 : 0) * asic_.lineCycles + 1;
}

void SegaCD::testAsic(const uint8_t* w, const uint16_t* r) {
  std::copy(w, w + 0x40000, wram_.begin());
  mode1M_ = false; ret_ = false; dmna_ = false;  // 2M, Word RAM do sub
  pm_ = r[8] & 3;
  asic_.config = static_cast<uint8_t>(r[0]);
  asic_.mapReg = r[1]; asic_.cellsReg = r[2]; asic_.imageReg = r[3]; asic_.offsetReg = r[4];
  asic_.widthReg = r[5]; asic_.heightReg = r[6]; asic_.traceReg = r[7];
  asicStart();
  asicRun(asic_.clk + (int64_t(1) << 40));
}

}  // namespace indigo::segacd
