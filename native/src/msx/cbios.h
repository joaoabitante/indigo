// C-BIOS 0.29a (http://cbios.sourceforge.net/), BIOS livre do MSX com licença tipo BSD (aviso completo de
// direitos autorais em cbios.cpp). Só roda cartuchos (não tem BASIC nem disco), que é o que o Índigo precisa.
// Versão internacional/US: teclado internacional e 60 Hz.
#pragma once
#include <cstdint>

namespace indigo::msx {

extern const uint8_t CBIOS_MSX1_MAIN[32768];  // slot 0, 0000h–7FFFh (MSX1, VDP TMS9918A)
extern const uint8_t CBIOS_MSX1_LOGO[16384];  // slot 0, 8000h–BFFFh (logotipo da abertura)
extern const uint8_t CBIOS_MSX2_MAIN[32768];  // slot 0, 0000h–7FFFh (MSX2, VDP V9938)
extern const uint8_t CBIOS_MSX2_LOGO[16384];  // slot 0, 8000h–BFFFh
extern const uint8_t CBIOS_MSX2_SUB[16384];   // sub-ROM do MSX2 (slot 3-0, 0000h–3FFFh)

}  // namespace indigo::msx
