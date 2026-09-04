#include "stdafx.h"
#include "d3d8to9.h"
#include "Memory.h"

void ApplyD3D8To9Patch() {
    Memory::PatchNop(0x0079633F, 6);
}
