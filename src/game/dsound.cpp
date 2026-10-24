#include <cmath>
#include <cstdint>

#include "audio/dsound.h"
#include "xdk/hle.h"

using namespace kernel;
namespace ds = warriors::dsound;

// DirectSound methods the SDK has no name for. The objects are the host's, so the console code must not run on
// them. Each takes (object, argument) and pops 8. The CDirectSoundBuffer methods are hooked themselves, which covers
// their IDirectSoundBuffer thunks (0x3A9DF8.., 0x3AA02C..), and take the buffer less 0x1C; the IDirectSoundStream
// exports jmp into the CDirectSoundStream methods, so the recompiler folds each into its thunk and the thunk is hooked.
// The CDirectSoundVoice method each forwards to identifies it. The host side is src/audio/dsound.cpp.
namespace {

void Done(X86Context& ctx) { Return(ctx, 0, 2); }

// the host buffer of a CDirectSoundBuffer method's object
uint32_t BufferOf(uint32_t object) { return object ? object + 0x1C : 0; }

}  // namespace

// SetLFO (voice 0x3A841E): the host mixer has no LFOs
X86_FUNC(sub_003A9648) { (void)base; Done(ctx); }  // buffer
X86_FUNC(sub_003AA08A) { (void)base; Done(ctx); }  // stream, jmp 0x3A995A

// GetVoiceProperties(DSVOICEPROPS*) (voice 0x3A84A8 asks the hardware voice): XACT 0x3D347C reads it when it moves
// a voice to another output, and gives the voice its mix bins back from it
X86_FUNC(sub_003A97CE) {  // buffer
    (void)base;
    ds::GetVoiceProperties(BufferOf(R32(ctx.esp + 4)), R32(ctx.esp + 8));
    Done(ctx);
}
X86_FUNC(sub_003AA0D8) {  // stream, jmp 0x3A9AF4
    (void)base;
    ds::GetVoiceProperties(R32(ctx.esp + 4), R32(ctx.esp + 8));
    Done(ctx);
}

// XACT computes 3D voice data in software with 0x3AC3BE (XACT 0x3D30FB, through the thunk 0x3AC6DF), which calls the
// HRTF table at 0x3CD868 that DirectSoundUseFullHRTF fills. The host's DirectSoundUseFullHRTF only returns, so the
// table is filled here with the library's own setup 0x3AFC2B: plain stores of function pointers, no hardware.
constexpr uint32_t kHrtfTable = 0x3CD868;

X86_EXTERN_FUNC(__imp__sub_003AFC2B);
X86_EXTERN_FUNC(__imp__sub_003AC6DF);
X86_FUNC(sub_003AC6DF) {
    if (!R32(kHrtfTable)) {
        X86_PUSH32(0);  // its ret pops this
        __imp__sub_003AFC2B(ctx, base);
    }
    __imp__sub_003AC6DF(ctx, base);
}

// IDirectSound_CreateSoundStream(dsound, desc, stream out, outer) at 0x3AD1C1, between CreateSoundBuffer and the
// 3 argument export, pops 16. XACT 0x3D2948 makes its streams with it. The host has DirectSoundCreateStream(desc,
// stream out) 0x3AD589, which takes the same description and makes the same host stream.
X86_EXTERN_FUNC(sub_003AD589);
X86_FUNC(sub_003AD1C1) {
    uint32_t ret = R32(ctx.esp), desc = R32(ctx.esp + 8), out = R32(ctx.esp + 12);
    ctx.esp += 20;
    X86_PUSH32(out);
    X86_PUSH32(desc);
    X86_PUSH32(ret);
    sub_003AD589(ctx, base);
}

// The DS3D exports 0x3ABCF6..0x3ABF0E follow the XDK declaration order, which their stack sizes confirm. XACT places
// its sounds with 3D voice data instead; the cone, mode and factor setters have nothing to change.
template <int Args>
static void Ignore(X86Context& ctx) {
    Return(ctx, 0, Args);
}
X86_FUNC(sub_003ABD16) { (void)base; Ignore<4>(ctx); }  // IDirectSoundBuffer_SetConeAngles(in, out, apply)
X86_FUNC(sub_003ABD3A) { (void)base; Ignore<5>(ctx); }  // IDirectSoundBuffer_SetConeOrientation(x, y, z, apply)
X86_FUNC(sub_003ABD6F) { (void)base; Ignore<3>(ctx); }  // IDirectSoundBuffer_SetConeOutsideVolume(volume, apply)
X86_FUNC(sub_003ABDD7) { (void)base; Ignore<3>(ctx); }  // IDirectSoundBuffer_SetMode(mode, apply)
X86_FUNC(sub_003ABE61) { (void)base; Ignore<3>(ctx); }  // the distance, Doppler and rolloff factors (factor, apply)
X86_FUNC(sub_003ABE85) { (void)base; Ignore<3>(ctx); }
X86_FUNC(sub_003ABEA9) { (void)base; Ignore<3>(ctx); }

// IDirectSoundStream thunks into CDirectSoundStream 0x3AB6EF.., between SetFrequency and SetMixBins as for buffers.
// XACT 0x3D344D points a stream at the submix it plays through.
X86_FUNC(sub_003ABF36) {  // SetOutputBuffer(buffer)
    (void)base;
    ds::SetOutputBuffer(R32(ctx.esp + 4), R32(ctx.esp + 8));
    Ignore<2>(ctx);
}
X86_FUNC(sub_003ABF40) { (void)base; Ignore<3>(ctx); }  // SetAllParameters(params, apply)
X86_FUNC(sub_003ABF45) { (void)base; Ignore<4>(ctx); }  // SetConeAngles(in, out, apply)

// IDirectSoundBuffer_SetNotificationPositions(count, positions) at 0x3AA00C: 0x3A8C13 hands both to the buffer data
// at +0x1C, then updates the voice at +0x20. Nothing in the game calls it.
X86_FUNC(sub_003AA00C) { (void)base; Ignore<3>(ctx); }

// IDirectSoundBuffer_PlayEx(time low, time high, flags) at 0x3A9EC4 and PauseEx(time low, time high, pause) at
// 0x3A9F40, beside Play and Pause, pop 16. Nothing in the game calls them; should anything, the host's Play(0, 0,
// flags) 0x3A9EA0 and Pause(pause) 0x3A9F24 start and pause at once.
X86_EXTERN_FUNC(sub_003A9EA0);
X86_EXTERN_FUNC(sub_003A9F24);
X86_FUNC(sub_003A9EC4) {
    uint32_t ret = R32(ctx.esp), buffer = R32(ctx.esp + 4), flags = R32(ctx.esp + 16);
    ctx.esp += 20;
    X86_PUSH32(flags);
    X86_PUSH32(0);
    X86_PUSH32(0);
    X86_PUSH32(buffer);
    X86_PUSH32(ret);
    sub_003A9EA0(ctx, base);
}
X86_FUNC(sub_003A9F40) {
    uint32_t ret = R32(ctx.esp), buffer = R32(ctx.esp + 4), pause = R32(ctx.esp + 16);
    ctx.esp += 20;
    X86_PUSH32(pause);
    X86_PUSH32(buffer);
    X86_PUSH32(ret);
    sub_003A9F24(ctx, base);
}

// SetPitch(pitch) (voice 0x3A83E9), from XACT 0x3D2C2C and 0x3D2C37 for its pitch and variation: the absolute pitch,
// the wave's own rate (XAudioCalculatePitch 0x3A7B17) included
X86_FUNC(sub_003A95FA) {  // buffer
    (void)base;
    ds::SetPitch(BufferOf(R32(ctx.esp + 4)), int32_t(R32(ctx.esp + 8)));
    Done(ctx);
}
X86_FUNC(sub_003AA085) {  // IDirectSoundStream_SetPitch, jmp 0x3A98B6
    (void)base;
    ds::SetPitch(R32(ctx.esp + 4), int32_t(R32(ctx.esp + 8)));
    Done(ctx);
}

// Set3DVoiceData(DS3DVOICEDATA*) (voice 0x3A84BB keeps it at +0xB8), from XACT's 3D update 0x3D30CE: a 3D submix gets
// its direction and front, rear and centre levels (flags & 0xDC), a sound playing through one its distance, cone and
// Doppler pitch (flags & 0x33)
X86_FUNC(sub_003A981C) {  // buffer
    (void)base;
    ds::Set3DVoiceData(BufferOf(R32(ctx.esp + 4)), R32(ctx.esp + 8));
    Done(ctx);
}
X86_FUNC(sub_003AA0DD) {  // stream, jmp 0x3A9B46
    (void)base;
    ds::Set3DVoiceData(R32(ctx.esp + 4), R32(ctx.esp + 8));
    Done(ctx);
}

// Use3DVoiceData(BOOL) (voice 0x3A85EC sets bit 0 of the settings flags byte +0xB): the voice's levels follow its 3D
// voice data. XACT 0x3D3463 and 0x3D34A5 set it for a voice that is or plays through a 3D submix.
X86_FUNC(sub_003A986A) {  // buffer
    (void)base;
    ds::Use3DVoiceData(BufferOf(R32(ctx.esp + 4)), R32(ctx.esp + 8) != 0);
    Done(ctx);
}
X86_FUNC(sub_003AA0E2) {  // stream, jmp 0x3A9B98
    (void)base;
    ds::Use3DVoiceData(R32(ctx.esp + 4), R32(ctx.esp + 8) != 0);
    Done(ctx);
}
