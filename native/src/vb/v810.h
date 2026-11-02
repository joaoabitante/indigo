// CPU do Virtual Boy: a V810 comum (src/v810), ligada ao barramento do VB.
#pragma once
#include "../v810/v810.h"

namespace indigo::vb {

class VB;
using V810 = indigo::v810::Cpu<VB>;

}  // namespace indigo::vb
