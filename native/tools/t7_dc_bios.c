/* BIOS de teste do Índigo para o Dreamcast (não é a BIOS da Sega: só o mínimo para os testes). Lê o TOC do
 * GD-ROM pela interface ATA, carrega o IP.BIN (16 setores) em 0x8C008000, confere "SEGA SEGAKATANA ", procura o
 * programa de partida (nome no IP.BIN, 0x60) na raiz do ISO 9660 da faixa de dados, carrega em 0x8C010000 e pula
 * para lá. Também confere o cabeçalho da flash. Mensagens pela SCIF. */
#include "t7_dc_hw.h"

static u8 sec[2048];

void main(void) {
  u32 fad, ffad, fsize, n;
  char name[17];
  int i;
  puts_("BIOS de teste do Indigo\n");
  if (!memcmp_((const void*)0xA021A000, "KATANA_FLASH", 12)) puts_("BIOS: flash ok\n");
  fad = gd_data_fad(1);
  if (!fad) fad = gd_data_fad(0);
  if (!fad) { puts_("BIOS: sem disco\n"); return; }
  puts_("BIOS: dados no FAD "); putdec(fad); puts_("\n");
  if (gd_read(fad, 16, (void*)0x8C008000, 0) < 0) { puts_("BIOS: erro ao ler o IP.BIN\n"); return; }
  if (memcmp_((const void*)0x8C008000, "SEGA SEGAKATANA ", 16)) { puts_("BIOS: IP.BIN invalido\n"); return; }
  for (i = 0; i < 16; i++) {
    char c = *(const char*)(0x8C008060 + i);
    name[i] = c == ' ' ? 0 : c;
  }
  name[16] = 0;
  puts_("BIOS: IP.BIN ok, partida "); puts_(name); puts_("\n");
  if (!iso_find(fad, name, &ffad, &fsize, sec)) { puts_("BIOS: programa nao encontrado\n"); return; }
  n = (fsize + 2047) / 2048;
  if (gd_read(ffad, n, (void*)0x8C010000, 0) < 0) { puts_("BIOS: erro ao ler o programa\n"); return; }
  puts_("BIOS: programa com "); putdec(fsize); puts_(" bytes\n");
  ((void (*)(void))0x8C010000)();
}
