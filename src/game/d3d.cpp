#include <cstdint>
#include <cstring>
#include <mutex>

#include "kernel/memory.h"
#include "xdk/hle.h"

using namespace kernel;

// Direct3D functions the host has no version of. The device is the host's, so the console code must not run on it.

// D3DDevice_DrawIndexedVerticesUP(prim, index count, WORD indices, vertices, stride) at 0x35F150, between
// DrawVerticesUP and DrawVertices, pops 20. The console code sets up its lazy state from the device's vertex shader
// at +0x794, which the host's device leaves 0. The indexed vertices are laid out in index order and drawn with the
// host's DrawVerticesUP 0x35EFE0, which keeps every primitive type, strips and fans too. That copies them at once,
// so one scratch buffer serves every draw.
namespace {

std::mutex g_up_lock;
uint32_t g_up_scratch = 0, g_up_size = 0;

}  // namespace

X86_EXTERN_FUNC(sub_0035EFE0);
X86_FUNC(sub_0035F150) {
    uint32_t ret = R32(ctx.esp), prim = R32(ctx.esp + 4), count = R32(ctx.esp + 8), indices = R32(ctx.esp + 12),
             vertices = R32(ctx.esp + 16), stride = R32(ctx.esp + 20);
    std::lock_guard<std::mutex> lock(g_up_lock);
    uint32_t bytes = count * stride;
    if (bytes > g_up_size) {
        if (g_up_scratch) mem::FreeContiguous(g_up_scratch);
        g_up_size = (bytes + 0xFFFF) & ~0xFFFFu;
        g_up_scratch = mem::AllocContiguous(g_up_size, 0, 0xFFFFFFFF, 0x1000);
    }
    if (!g_up_scratch || !count || !stride || !indices || !vertices) {
        Return(ctx, 0, 5);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t index;
        std::memcpy(&index, base + indices + i * 2, 2);
        std::memcpy(base + g_up_scratch + i * stride, base + vertices + uint32_t(index) * stride, stride);
    }
    ctx.esp += 24;
    X86_PUSH32(stride);
    X86_PUSH32(g_up_scratch);
    X86_PUSH32(count);
    X86_PUSH32(prim);
    X86_PUSH32(ret);
    sub_0035EFE0(ctx, base);
}
