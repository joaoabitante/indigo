/* Programa de teste do Watara Supervision para o cc65 (cl65 -t supervision), escrito para o Índigo.
   Desenha 4 faixas horizontais (tons 0 a 3), marca $1C00 = $5A ao chegar no laço e, a cada NMI do crt0,
   soma 1 em $1C01 e guarda joy_read em $1C02. Usado por indigo-t7_svision. */
#include <supervision.h>
#include <joystick.h>
#include <string.h>

#define MARK ((unsigned char*)0x1C00)

void main(void) {
  unsigned char band, last;
  unsigned char* v = SV_VIDEO;
  SV_LCD.width = 160;
  SV_LCD.height = 160;
  SV_LCD.xpos = 0;
  SV_LCD.ypos = 0;
  for (band = 0; band < 4; ++band) memset(v + band * (40 * 48), band * 0x55, 40 * 48);
  joy_install(joy_static_stddrv);
  MARK[0] = 0x5A;
  last = sv_nmi_counter;
  for (;;) {
    while (sv_nmi_counter == last) {}
    last = sv_nmi_counter;
    ++MARK[1];
    MARK[2] = joy_read(JOY_1);
  }
}
