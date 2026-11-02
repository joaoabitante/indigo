// Instancia a CPU V810 comum (src/v810) para o barramento do Virtual Boy.
#include "../v810/v810_impl.h"
#include "vb.h"

template class indigo::v810::Cpu<indigo::vb::VB>;
