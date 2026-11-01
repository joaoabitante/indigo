// MADAM: controlador de memória do 3DO com o motor de cels (desenho de sprites por quadriláteros), o motor de
// matrizes e a lista de exibição de vídeo (VDL). Escrito a partir da documentação pública do 3DO (formato do CCB,
// das cels, do PIXC e da VDL descritos para os programadores do Portfolio) e com nomes próprios.
//
// Memória vista pelo MADAM: 0x000000-0x1FFFFF DRAM (2 MB) e 0x200000-0x2FFFFF VRAM (1 MB), big-endian.
//
// Registradores (endereço - 0x03300000), interpretação deste núcleo:
//   0x0000 revisão  0x0004 MSYSBITS  0x0008 MCTL  0x000C SLTIME  0x0020/0x0024/0x0028 abort/priv/status
//   0x0100 escrita: desenha a lista de cels que começa no CCB de 0x05A0 (a CPU fica parada enquanto desenha)
//   0x0130 REGCTL0: largura do destino em pixels (bits 0-10; 0 = 320)
//   0x0134 REGCTL1: recorte (x máximo nos bits 0-10, y máximo nos bits 16-26)
//   0x0138 REGCTL2: base do quadro lido pelo PIXC   0x013C REGCTL3: base do quadro escrito
//   0x0400-0x04CF canais de DMA 0..12 para o DSP (endereço, bytes, próximo endereço, próximos bytes)
//   0x0540/0x0544 DMA do XBUS (endereço de destino, bytes)   0x0570/0x0574 DMA do controle (PBUS)
//   0x0580 endereço da VDL (lido no começo de cada quadro)   0x05A0 primeiro CCB
//   0x0600-0x063C matriz 4x4 (16.16)  0x0640-0x064C vetor  0x0660-0x066C resultado  0x0680 N (projeção)
//   0x07FC escrita: 1 = matriz 3x3 x vetor, 2 = 4x4 x vetor, 3 = 3x3 com projeção (x,y * N / z)
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
  uint16_t plut[32] = {};
  uint64_t celPixels = 0;  // pixels desenhados (conta para o tempo da CPU parada)
  uint32_t celsDrawn = 0;

  void reset();
  uint32_t read(uint32_t off);
  // devolve os ciclos em que a CPU fica parada (motor de cels)
  int write(uint32_t off, uint32_t v);

  // desenha a lista de cels a partir do CCB; devolve os ciclos gastos
  int drawCels(uint32_t ccb);
  void matrixOp(uint32_t cmd);

  // VDL: monta a imagem de 320x240 em out (0xAABBGGRR)
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
  void plotQuad(CelCtx& c, const int64_t (&px)[4], const int64_t (&py)[4], uint16_t color, bool pbit, int amv);
  void putPixel(CelCtx& c, int x, int y, uint16_t color, bool pbit, int amv);
  uint16_t blend(uint16_t half, uint16_t cel, uint16_t frame, int amv, bool useAv) const;
};

}  // namespace indigo::tdo
