// The mouse turns the play camera by its own angle, as on PC, not as a stick.
// sub_0003EEC0 (thiscall on the camera, from its update 0x4E2E0) reads the right stick for the camera: past the stick
// thresholds at 0x458338.. it sets a yaw speed at +0x344 and a pitch speed at +0x3A4, radians a second. The update
// turns the camera about its target by yaw times the frame time (+0x190) in 0x3F840, then clears it, and adds pitch
// times the frame time to the pitch at +0x3A0 between its limits (+0x39C, +0x398). The mouse's angle over the frame
// time is added to those speeds, so the camera turns by exactly the mouse's angle, without the stick's speed cap or
// dead zone, and only where the game would let the stick turn it.
#include <cfloat>
#include <cstring>

#include "core/log.h"
#include "game/kbm.h"
#include "xdk/hle.h"

using namespace kernel;

namespace {

// camera
constexpr uint32_t kYawSpeed = 0x344, kPitchSpeed = 0x3A4, kFrameTime = 0x190;
constexpr uint32_t kTurnRight = 0x420, kTurnLeft = 0x424;  // 0 while the camera may not turn that way
constexpr uint32_t kManual = 0x354;       // 0.334 once the player turns the camera, as the stick sets it
constexpr uint32_t kPitchManual = 0x384;  // 1 once the player tilts it
constexpr uint32_t kPitchTarget = 0x3C0;  // a pitch the camera is heading to; -FLT_MAX for none
constexpr uint32_t kAutoTurn = 0x460;
constexpr float kManualValue = 0.334f;
// the stick's yaw speed is positive to the left, its pitch speed positive when pushed forward
constexpr float kPitchSign = -1.0f;

bool g_yaw = false, g_pitch = false;  // the call reached the stick's yaw and pitch, the game lets them turn
uint32_t g_calls = 0;

void WF(uint32_t va, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    W32(va, v);
}

}  // namespace

// 0x3F15E: the stick's yaw, past the checks for the camera's mode and a lock on
void CameraYawStick(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    g_yaw = true;
}

// 0x3F393: the stick's pitch, where the caller asks for it
void CameraPitchStick(X86Context& ctx, uint8_t* base) {
    (void)ctx, (void)base;
    g_pitch = true;
}

// the world's update (from the scheduler 0x2B2430): play, a fixed camera scene or a cutscene, not a front end menu
X86_EXTERN_FUNC(__imp__sub_0016D8D0);
X86_FUNC(sub_0016D8D0) {
    warriors::kbm::WorldSeen();
    __imp__sub_0016D8D0(ctx, base);
}

// (port, ?, bool* turned, ...), pops 0x28
X86_EXTERN_FUNC(__imp__sub_0003EEC0);
X86_FUNC(sub_0003EEC0) {
    uint32_t camera = ctx.ecx;
    uint32_t port = R32(ctx.esp + 4), turned = R32(ctx.esp + 0xC);
    g_yaw = g_pitch = false;
    __imp__sub_0003EEC0(ctx, base);
    if (port != 0) return;  // the keyboard and mouse are player 1's
    warriors::kbm::CameraSeen(g_yaw || g_pitch);
    const auto& s = warriors::kbm::Get();
    if (g_yaw) WF(camera + kYawSpeed, GF(camera + kYawSpeed) * s.stick_camera_speed);
    if (g_pitch) WF(camera + kPitchSpeed, GF(camera + kPitchSpeed) * s.stick_camera_speed);
    float yaw, pitch;
    bool moved = warriors::kbm::TakeLook(yaw, pitch);
    float dt = GF(camera + kFrameTime);
    if (warriors::kbm::TestMode() && (moved || (++g_calls % 120) == 0))
        LOGR("camera %08X: yaw stick %d pitch stick %d, captured %d, mouse %g %g, dt %g, turn %g %g, pitch %g (%g..%g)",
             camera, g_yaw, g_pitch, warriors::kbm::Captured(), yaw, pitch, dt, GF(camera + kTurnLeft),
             GF(camera + kTurnRight), GF(camera + 0x3A0), GF(camera + 0x39C), GF(camera + 0x398));
    if (!moved) return;
    if (!(dt > 0.0f)) return;
    if (g_yaw && yaw != 0.0f) {
        float speed = -yaw / dt;
        if (GF(camera + (speed > 0 ? kTurnLeft : kTurnRight)) <= 0.0f) speed = 0.0f;
        if (speed != 0.0f) {
            WF(camera + kYawSpeed, GF(camera + kYawSpeed) + speed);
            WF(camera + (speed > 0 ? kTurnRight : kTurnLeft), 1.0f);
            WF(camera + kManual, kManualValue);
            G<uint8_t>(camera + kAutoTurn) = 0;
            if (turned) G<uint8_t>(turned) = 1;
        }
    }
    if (g_pitch && pitch != 0.0f) {
        WF(camera + kPitchSpeed, GF(camera + kPitchSpeed) + kPitchSign * pitch / dt);
        WF(camera + kPitchTarget, -FLT_MAX);
        WF(camera + kPitchManual, 1.0f);
        WF(camera + kManual, kManualValue);
        if (turned) G<uint8_t>(turned) = 1;
    }
}
