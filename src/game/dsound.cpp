#include <cstdint>

#include "xdk/hle.h"

using namespace kernel;

// DirectSound methods the host has no version of. The objects are the host's, so the console code must not run on
// them. Each takes (object, argument) and pops 8. The CDirectSoundBuffer methods are hooked themselves, which covers
// their IDirectSoundBuffer thunks (0x3A9DF8.., 0x3AA02C..); the IDirectSoundStream exports jmp into the
// CDirectSoundStream methods, so the recompiler folds each into its thunk and the thunk is hooked. The
// CDirectSoundVoice method each forwards to identifies it.
namespace {

void Done(X86Context& ctx) { Return(ctx, 0, 2); }

// DSVOICEPROPS as XDK 5849 declares it: mix bin count, 8 bin/volume pairs, 6 LONG levels
constexpr uint32_t kVoicePropsSize = 92;

}  // namespace

// SetPitch (voice 0x3A83E9): the host mixer only follows SetFrequency
X86_FUNC(sub_003A95FA) { (void)base; Done(ctx); }  // buffer
X86_FUNC(sub_003AA085) { (void)base; Done(ctx); }  // stream, jmp 0x3A98B6

// SetLFO (voice 0x3A841E): the host mixer has no LFOs
X86_FUNC(sub_003A9648) { (void)base; Done(ctx); }  // buffer
X86_FUNC(sub_003AA08A) { (void)base; Done(ctx); }  // stream, jmp 0x3A995A

// GetVoiceProperties(DSVOICEPROPS*) (voice 0x3A84A8 asks the hardware voice): XACT 0x3D347C reads it when it moves
// a voice to another output. No mix bins, pitch or attenuation to report.
static void GetVoiceProperties(X86Context& ctx) {
    uint32_t props = R32(ctx.esp + 8);
    for (uint32_t o = 0; props && o < kVoicePropsSize; o += 4) W32(props + o, 0);
    Done(ctx);
}
X86_FUNC(sub_003A97CE) { (void)base; GetVoiceProperties(ctx); }  // buffer
X86_FUNC(sub_003AA0D8) { (void)base; GetVoiceProperties(ctx); }  // stream, jmp 0x3A9AF4

// Set3DVoiceData(DS3DVOICEDATA*) (voice 0x3A84BB writes the 3D settings at +0xB8): positions go through SetPosition
X86_FUNC(sub_003A981C) { (void)base; Done(ctx); }  // buffer
X86_FUNC(sub_003AA0DD) { (void)base; Done(ctx); }  // stream, jmp 0x3A9B46

// Use3DVoiceData(BOOL) (voice 0x3A85EC sets bit 0 of the settings flags byte +0xB): makes the APU take XACT's 3D
// voice data, which the host mixer has none of. XACT 0x3D34AD calls it after SetOutputBuffer.
X86_FUNC(sub_003A986A) { (void)base; Done(ctx); }  // buffer
X86_FUNC(sub_003AA0E2) { (void)base; Done(ctx); }  // stream, jmp 0x3A9B98

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

// The DS3D exports 0x3ABCF6..0x3ABF0E follow the XDK declaration order, which their stack sizes confirm. The host
// mixer takes position and distances only; the cone, mode and factor setters have nothing to change.
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
// XACT 0x3D344D sets a stream's output buffer; the host voices keep their own mix bins, as its buffer version does.
X86_FUNC(sub_003ABF36) { (void)base; Ignore<2>(ctx); }  // SetOutputBuffer(buffer)
X86_FUNC(sub_003ABF40) { (void)base; Ignore<3>(ctx); }  // SetAllParameters(params, apply)
X86_FUNC(sub_003ABF45) { (void)base; Ignore<4>(ctx); }  // SetConeAngles(in, out, apply)

// IDirectSoundBuffer_SetNotificationPositions(count, positions) at 0x3AA00C: 0x3A8C13 hands both to the buffer data
// at +0x1C, then updates the voice at +0x20. The host mixer signals no notification events.
X86_FUNC(sub_003AA00C) { (void)base; Ignore<3>(ctx); }
