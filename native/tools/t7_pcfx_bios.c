/* BIOS de teste do Índigo para o PC-FX (escrita do zero; não contém nada da BIOS da NEC).
   Faz o que a BIOS de verdade faz para dar a partida: acha a primeira faixa de dados do CD (READ TOC), confere
   "PC-FX:Hu_CD-ROM" no setor 0, lê o cabeçalho do setor 1 (setor e quantidade do programa, endereço de carga
   e de partida), carrega o programa por DMA para a KRAM, copia para a RAM e pula para ele. As interrupções e
   exceções da CPU são desviadas para a RAM (0x400 + deslocamento do vetor), onde o programa põe as rotinas.
   Marca em 0x1FFF00: "BOOT", LBA da faixa, setores, endereço de carga, partida e o passo em que parou. */
#include "t7_pcfx_scsi.h"

__attribute__((used)) const char bios_sig[] = "Indigo PC-FX BIOS de teste";
#define MARK ((volatile uint32_t*)0x001FFF00)

static void color(uint16_t yuv) { vw(1, 0); vw(2, yuv); vw(0, 0x0100); }  /* cor de fundo mostra o passo */

int main(void) {
  /* vetores na RAM começam como RETI */
  for (uint32_t a = 0x400; a < 0x600; a += 16) *(volatile uint16_t*)a = 0x6400;
  MARK[0] = 0; MARK[6] = (uint32_t)bios_sig[0];
  out16(0xE40, 0x7F); /* todas as interrupções mascaradas */
  color(0x4088);      /* cinza: começou */
  kw(1, 0x80); kw(1, 0x00); /* reinicia o barramento SCSI */
  MARK[5] = 1;
  uint8_t tur[6] = {0, 0, 0, 0, 0, 0};
  scsi_pio(tur, 6, 0, 0, 0);
  const int32_t lba = first_data_track();
  MARK[1] = (uint32_t)lba;
  if (lba < 0) { color(0x50F0); MARK[5] = 2; for (;;) {} }
  static uint8_t hdr[4096];
  uint8_t c[10];
  cdb10(c, 0x28, (uint32_t)lba, 2);
  uint32_t got = 0;
  if (scsi_pio(c, 10, hdr, sizeof hdr, &got) || got != 4096) { color(0x50F0); MARK[5] = 3; for (;;) {} }
  static const char magic[] = "PC-FX:Hu_CD-ROM";
  for (int i = 0; i < 15; i++)
    if (hdr[i] != (uint8_t)magic[i]) { color(0x500F); MARK[5] = 4; for (;;) {} }
  const uint8_t* h = hdr + 2048;
#define LE32(p) ((uint32_t)(p)[0] | (uint32_t)(p)[1] << 8 | (uint32_t)(p)[2] << 16 | (uint32_t)(p)[3] << 24)
  const uint32_t off = LE32(h + 0x20), count = LE32(h + 0x24), load = LE32(h + 0x28), entry = LE32(h + 0x2C);
  MARK[2] = count; MARK[3] = load; MARK[4] = entry;
  MARK[5] = 5;
  /* carrega em pedaços de até 64 setores (128 KB = 64 K palavras de KRAM) */
  uint32_t done = 0;
  while (done < count) {
    uint32_t n = count - done;
    if (n > 64) n = 64;
    cdb10(c, 0x28, (uint32_t)lba + off + done, (uint16_t)n);
    if (scsi_dma(c, 10, 0, n * 2048)) { color(0x50F0); MARK[5] = 6; for (;;) {} }
    kram_to_ram(0, (uint16_t*)(load + done * 2048), n * 1024);
    done += n;
  }
  MARK[5] = 7;
  MARK[0] = 0x544F4F42; /* "BOOT" */
  color(0x8088);
  ((void (*)(void))entry)();
  for (;;) {}
  return 0;
}
