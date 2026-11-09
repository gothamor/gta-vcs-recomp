// [gta-recomp] ARM builds (macOS on Apple silicon, docs/MACOS.md): the x86 intrinsics header becomes sse2neon,
// which implements the SSE intrinsics with NEON. Only on the include path when PS2X_IS_ARM_TARGET.
#pragma once
#include "sse2neon.h"
