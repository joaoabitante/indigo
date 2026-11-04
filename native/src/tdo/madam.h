// MADAM: controlador de memória do 3DO com o motor de cels (desenho de sprites por quadriláteros), o motor de
// matrizes e a lista de exibição de vídeo (VDL). Escrito a partir da documentação pública do 3DO (formato do CCB,
// das cels, do PIXC e da VDL descritos para os programadores do Portfolio) e com nomes próprios.
//
// Memória vista pelo MADAM: 0x000000-0x1FFFFF DRAM (2 MB) e 0x200000-0x2FFFFF VRAM (1 MB), big-endian.
//
// Registradores (endereço - 0x03300000), interpretação deste núcleo (documentação pública do hardware e o driver
// do 3DO no MAME, licença BSD-3-Clause):
//   0x0000 revisão (0x01020200)  0x0004 MSYSBITS (tamanho da DRAM/VRAM, ver tdo.h; 0x51 na partida)
//   0x0008 MCTL: bit 15 começa o DMA do controle (PBUS), bit 14 liga a VDL (vídeo), bit 13 liga o DAC de vídeo
//   0x000C SLTIME  0x0020/0x0024 abort/priv  0x0028 STATBITS (bit 4 motor de cels rodando, bit 6 terminou)
//   0x0100 SPRSTRT: desenha a lista de cels que começa no CCB de 0x05A4 (a CPU fica parada enquanto desenha)
//   0x0104 SPRSTOP  0x0108 SPRCNTU  0x0110 CCOBCTL0  0x0120 PPMPC
//   0x0130 REGCTL0: largura do quadro lido (bits 0-7) e do escrito (bits 8-15), cada uma como soma de um
//     grupo (bits 0-3: 1 = 32, 4 = 256, 8 = 1024) e outro (bits 4-7: 1 = 64, 2 = 128, 4 = 256): 320 = 0x14
//   0x0134 REGCTL1: recorte (x máximo nos bits 0-10, y máximo nos bits 16-26)
//   0x0138 REGCTL2: base do quadro lido pelo PIXC   0x013C REGCTL3: base do quadro escrito
//   0x0400 + 16n canal de DMA n (endereço, contagem em bytes menos 4, próximo endereço, próxima contagem):
//     0..12 RAM -> DSP, 20 XBUS (0x540), 23 controle (0x570: destino, contagem, origem), 24 VDL (0x580),
//     26 motor de cels (0x5A0 controle, 0x5A4 primeiro CCB)
//   0x0600-0x063C matriz 4x4 (16.16)  0x0640-0x064C vetor B  0x0660-0x066C resultado anterior
//   0x0670-0x067C resultado  0x0680/0x0684 N (64 bits, parte alta e baixa) para a divisão por Z
//   0x07F0/0x07F4 liga/limpa o controle  0x07F8 estado  0x07FC escrita: 0 = passa o resultado para 0x0660,
//     1 = 4x4 x vetor, 2 = 3x3 x vetor, 3 = 3x3 com a projeção (x,y * N / z); antes de cada conta o resultado
//     anterior vai para 0x0660 (a biblioteca do 3DO lê os resultados um passo atrasados)
//
// CCB: flags, próximo, origem, PLUT e, só quando a flag correspondente está ligada e nesta ordem, X/Y (YOXY),
// HDX/HDY/VDX/VDY (LDSIZE), HDDX/HDDY (LDPRS), PIXC (LDPPMP) e PRE0/PRE1 (CCBPRE; sem ela os preâmbulos estão
// no começo dos dados da cel). PRE1: largura - 1 (bits 0-10), LRFORM (bit 11), WOFFSET10 (bits 16-25, cels de
// 8 e 16 bits) ou WOFFSET8 (bits 24-31, as outras): palavras por linha - 2.
//
// Quadro (frame buffer): dois pixels de 16 bits por palavra, a linha par na metade de cima e a ímpar na de baixo
// ("left/right"); endereço do pixel (x, y) = base + (y/2)*largura*4 + x*4 + (y%2)*2. Pixel 1-5-5-5 (bit 15 = P).
#pragma once
#include <cstdint>
#include <vector>

#include "../core/state.h"

namespace indigo::tdo {

constexpr uint32_t MEM_SIZE = 0x300000;  // DRAM + VRAM

class Madam {
public:
  explicit Madam(std::vector<uint8_t>& mem) : mem_(mem) { reset(); }

  uint32_t regs[0x800 / 4] = {};
  // registradores do motor de cels (ficam de uma cel para a seguinte quando o CCB não os carrega)
  int32_t xpos = 0, ypos = 0, hdx = 0, hdy = 0, vdx = 0, vdy = 0, hddx = 0, hddy = 0;
  uint32_t pixc = 0x1F001F00, plutPtr = 0;
  uint32_t statbits = 0;
  uint16_t plut[32] = {};
  uint64_t celPixels = 0;  // pixels desenhados (conta para o tempo da CPU parada)
  uint32_t celsDrawn = 0;
  // pontos testados nas caixas dos quadriláteros numa lista de cels: teto contra cels enormes (HDX/VDY grandes)
  // que fariam dezenas de milhões de testes; ao passar dele o resto da lista não desenha
  static constexpr int64_t QUAD_BUDGET = int64_t(1) << 23;
  int64_t quadBudget = QUAD_BUDGET;
  bool budgetHit = false;

  void reset();
  uint32_t read(uint32_t off);
  // devolve os ciclos em que a CPU fica parada (motor de cels)
  int write(uint32_t off, uint32_t v);

  // desenha a lista de cels a partir do CCB; devolve os ciclos gastos
  int drawCels(uint32_t ccb);
  void matrixOp(uint32_t cmd);

  // VDL: monta a imagem de 320x240 em out (0xAABBGGRR). As 16 primeiras linhas da VDL ficam antes da área
  // visível (o sistema real põe ali a sua "pré-exibição"; o MAME começa a VDL 16 linhas antes da 1ª visível).
  static constexpr int HIDDEN_LINES = 16;
  void renderFrame(uint32_t* out);

  void sync(StateIO& io);

  uint32_t m32(uint32_t a) const {
    a = (a % MEM_SIZE) & ~3u;
    return uint32_t(mem_[a]) << 24 | uint32_t(mem_[a + 1]) << 16 | uint32_t(mem_[a + 2]) << 8 | mem_[a + 3];
  }
  uint16_t m16(uint32_t a) const {
    a = a % MEM_SIZE & ~1u;
    return uint16_t(mem_[a] << 8 | mem_[a + 1]);
  }
  void w16(uint32_t a, uint16_t v) {
    a = a % MEM_SIZE & ~1u;
    mem_[a] = uint8_t(v >> 8);
    mem_[a + 1] = uint8_t(v);
  }

private:
  std::vector<uint8_t>& mem_;

  struct CelCtx;
  void drawOne(uint32_t flags, uint32_t src, uint32_t pre0, uint32_t pre1);
  void plotQuad(CelCtx& c, const int64_t (&px)[4], const int64_t (&py)[4], uint16_t color, bool pbit, uint16_t amv);
  void putPixel(CelCtx& c, int x, int y, uint16_t color, bool pbit, uint16_t amv);
  uint16_t blend(uint16_t half, uint16_t cel, uint16_t frame, uint16_t amv, uint32_t flags) const;
};

}  // namespace indigo::tdo
