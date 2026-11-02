// Instancia a CPU V810 comum (src/v810) para o barramento do PC-FX.
#include "../v810/v810_impl.h"
#include "pcfx.h"

template class indigo::v810::Cpu<indigo::pcfx::Pcfx>;
