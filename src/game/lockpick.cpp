// The lock pick's tumblers turn at the console's speed.
// The HUD widget (vtable 0x4243E8; one a player at 0x4D6298 + 0xBDF0 + player * 0x484) is started by 0xD9000, which
// gives the three tumblers their speeds, 1, 2 and 3, at +0x3B0. Each of its updates (0xE0FD0, vtable +0x14) turns the
// tumbler being picked, index +0x3C0, by its speed times 0.1 radians (0x45A5D4, set by the update itself), forwards
// or back by the byte at +0x3BC + index, from the tumbler's angle (+0x88 of the tumbler at +0x28 + index * 0xDC) and
// kept in 0..2pi. The update runs once a presented frame: the game asks for presentation interval 0x80000002 (two
// vertical blanks), 30 a second on the console, but the port presents 60 times a second or more, which spun the
// tumblers twice as fast or faster, the third past what can be stopped. The tumbler is turned here by the console's
// steps for the real time since the last update, and the game's own turn is held at nothing.
#include <algorithm>
#include <chrono>
#include <cstring>

#include "xdk/hle.h"

using namespace kernel;

namespace {

constexpr uint32_t kWidgets = 0x4D6298 + 0xBDF0, kWidgetStride = 0x484, kPlayers = 4;
constexpr uint32_t kReady = 0x7;        // byte: the opening has played (0xE0E20)
constexpr uint32_t kTumblers = 0x28;    // three, 0xDC apart
constexpr uint32_t kTumblerSize = 0xDC;
constexpr uint32_t kAngle = 0x88;       // of a tumbler
constexpr uint32_t kSpeeds = 0x3B0;     // uint32 x3
constexpr uint32_t kBackwards = 0x3BC;  // byte x3
constexpr uint32_t kIndex = 0x3C0;
constexpr uint32_t kActive = 0x3D0;     // byte
constexpr float kStep = 0.1f;           // radians a speed unit, each console update
constexpr float kRate = 30.0f;          // console updates a second
constexpr float kMaxDt = 2.0f / kRate;  // past a stall the tumbler moves on as after two updates
constexpr float kTwoPi = 6.28318530718f;

using Clock = std::chrono::steady_clock;
Clock::time_point g_last[kPlayers];
bool g_turning[kPlayers];

void WF(uint32_t va, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    W32(va, v);
}

}  // namespace

X86_EXTERN_FUNC(__imp__sub_000E0FD0);
X86_FUNC(sub_000E0FD0) {
    uint32_t widget = ctx.ecx;
    uint32_t player = (widget - kWidgets) / kWidgetStride;
    uint32_t index = R32(widget + kIndex);
    // as the update turns it: started, past its opening, a tumbler left to pick
    bool turning = widget >= kWidgets && (widget - kWidgets) % kWidgetStride == 0 && player < kPlayers &&
                   G<uint8_t>(widget + kActive) && G<uint8_t>(widget + kReady) && index < 3;
    if (!turning) {
        if (player < kPlayers) g_turning[player] = false;
        __imp__sub_000E0FD0(ctx, base);
        return;
    }
    auto now = Clock::now();
    // the first update turns it by one console step
    float dt = g_turning[player] ? std::min(std::chrono::duration<float>(now - g_last[player]).count(), kMaxDt)
                                 : 1.0f / kRate;
    g_last[player] = now;
    g_turning[player] = true;
    uint32_t speed_va = widget + kSpeeds + index * 4, speed = R32(speed_va);
    uint32_t angle_va = widget + kTumblers + index * kTumblerSize + kAngle;
    float turn = float(speed) * kStep * dt * kRate;
    float angle = GF(angle_va) + (G<uint8_t>(widget + kBackwards + index) ? -turn : turn);
    if (angle < 0.0f) angle += kTwoPi;
    else if (angle > kTwoPi) angle -= kTwoPi;
    WF(angle_va, angle);
    W32(speed_va, 0);
    __imp__sub_000E0FD0(ctx, base);
    W32(speed_va, speed);
}
