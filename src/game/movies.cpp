#include <cstdint>

#include "xdk/hle.h"

using namespace kernel;

// PlayMovie(name, skippable) 0x24A040 (cdecl). The boot plays "logo" (0x41D9E0, the Rockstar logo) as the only one
// that cannot be skipped (0x68BE7); it can be skipped as plogo and the intro can.
constexpr uint32_t kRockstarLogo = 0x41D9E0;

X86_EXTERN_FUNC(__imp__sub_0024A040);
X86_FUNC(sub_0024A040) {
    if (R32(ctx.esp + 4) == kRockstarLogo) W32(ctx.esp + 8, 1);
    __imp__sub_0024A040(ctx, base);
}
