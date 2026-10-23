#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "xdk/hle.h"

using namespace kernel;

// XACT plays a 3D sound through two voices: the sound's own buffer or stream, and a DSBCAPS_MIXIN submix buffer from
// its pool of 64 that carries the speaker routing (crosstalk 6-9, centre 2, I3DL2 10, sub 3). The sound's voice is
// pointed at the submix with SetOutputBuffer, and keeps only the I3DL2 and sub bins of its own (0x3D3408). The host
// mixes no voice into another, so those sounds reached only the reverb send and the sub: punches, footsteps and
// voices played barely audible. The sound's voice gets the submix's bins as well, so it plays where the submix would.
namespace {

using Bins = std::vector<std::pair<uint32_t, int32_t>>;  // DSMIXBINVOLUMEPAIR: bin, mB

std::mutex g_lock;
std::unordered_map<uint32_t, Bins> g_bins;     // the bins XACT gave each buffer or stream
std::unordered_map<uint32_t, uint32_t> g_route;  // sound buffer or stream -> the 3D submix it outputs to

// the submix bins XACT's pool buffers are made with, should one not have been set yet
const Bins kSubmix = {{6, 0}, {8, 0}, {7, 0}, {9, 0}, {10, 0}, {2, 0}, {3, 0}};

Bins Read(uint32_t mixbins) {
    Bins bins;
    if (!mixbins) return bins;
    uint32_t n = R32(mixbins), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n && i < 8 && pairs; i++) bins.push_back({R32(pairs + i * 8), R32(pairs + i * 8 + 4)});
    return bins;
}

}  // namespace

// at XACT's SetMixBins(object, DSMIXBINS*) calls 0x3D2CB7 (buffer) and 0x3D2CC2 (stream), the arguments pushed
void XactRecordBins(X86Context& ctx, uint8_t* base) {
    (void)base;
    std::lock_guard<std::mutex> lock(g_lock);
    g_bins[R32(ctx.esp)] = Read(R32(ctx.esp + 4));
}

// at the sound voice's SetMixBins 0x3D352B (buffer) and 0x3D351D (stream) in 0x3D3408: edi is the XACT voice it
// outputs to, whose buffer at +0x1C is the submix. The DSMIXBINS is XACT's own, with room for 8 pairs (ebp + 0x24).
void XactRouteToSubmix(X86Context& ctx, uint8_t* base) {
    (void)base;
    uint32_t target = ctx.edi, mixbins = R32(ctx.esp + 4);
    if (!target || !mixbins || R32(mixbins) > 2) return;  // the other path here sets a voice's full 8 bins itself
    uint32_t submix = R32(target + 0x1C), sound = R32(ctx.esp);
    Bins bins;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_route[sound] = submix;
        auto it = g_bins.find(submix);
        bins = it != g_bins.end() && !it->second.empty() ? it->second : kSubmix;
    }
    for (const auto& own : Read(mixbins)) {
        bool have = false;
        for (const auto& b : bins) have |= b.first == own.first;
        if (!have) bins.push_back(own);
    }
    if (bins.size() > 8) bins.resize(8);
    uint32_t pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < bins.size(); i++) {
        W32(pairs + i * 8, bins[i].first);
        W32(pairs + i * 8 + 4, uint32_t(bins[i].second));
    }
    W32(mixbins, uint32_t(bins.size()));
}

// Distance. XACT works out a 3D sound's distance and cone volumes in software for the submix (DS3DVOICEDATA +4 and +8,
// mB, 0x3AF4C8) and leaves them out of what it hands DirectSound (0x3D311D masks the flags with 0xDC). The submix
// mixes nothing on the host, so its sounds carry them: each sound plays at XACT's volume plus its submix's distance
// and cone volumes, or ambient sounds across the level play as loud as the characters next to the player.
namespace {

std::unordered_map<uint32_t, int32_t> g_distance;    // submix -> distance + cone, mB
std::unordered_map<uint32_t, int32_t> g_volume;      // sound buffer or stream -> XACT's volume, mB
std::unordered_map<uint32_t, bool> g_stream;         // sound -> a stream

int32_t Effective(uint32_t sound) {
    int64_t mb = g_volume.count(sound) ? g_volume[sound] : 0;
    auto r = g_route.find(sound);
    if (r != g_route.end() && g_distance.count(r->second)) mb += g_distance[r->second];
    return int32_t(std::clamp<int64_t>(mb, -10000, 0));
}

}  // namespace

X86_EXTERN_FUNC(sub_003A9DDC);  // the host's IDirectSoundBuffer_SetVolume
X86_EXTERN_FUNC(sub_003A9908);  // the host's CDirectSoundStream_SetVolume

// sets the host volume from a hook, every register kept
static void Apply(X86Context& ctx, uint8_t* base, uint32_t sound) {
    uint32_t eax = ctx.eax, ecx = ctx.ecx, edx = ctx.edx;
    bool stream = g_stream.count(sound) && g_stream[sound];
    X86_PUSH32(uint32_t(Effective(sound)));
    X86_PUSH32(sound);
    X86_PUSH32(0);  // the host's Return pops this
    (stream ? sub_003A9908 : sub_003A9DDC)(ctx, base);
    ctx.eax = eax;
    ctx.ecx = ecx;
    ctx.edx = edx;
}

// called from Set3DVoiceData on a submix buffer (src/game/dsound.cpp)
void XactSubmix3D(X86Context& ctx, uint8_t* base, uint32_t submix, uint32_t data) {
    int32_t mb = int32_t(R32(data + 4)) + int32_t(R32(data + 8));
    std::lock_guard<std::mutex> lock(g_lock);
    auto d = g_distance.find(submix);
    if (d != g_distance.end() && d->second == mb) return;
    g_distance[submix] = mb;
    for (const auto& [sound, to] : g_route)
        if (to == submix) Apply(ctx, base, sound);
}

// XACT's voice routing 0x3D3408 (thiscall: the voice, its target voice, a flag) sets the sound's submix again, through
// XactRouteToSubmix when there is a 3D one
X86_EXTERN_FUNC(__imp__sub_003D3408);
X86_FUNC(sub_003D3408) {
    uint32_t buffer = R32(ctx.ecx + 0x1C), stream = R32(ctx.ecx + 0x20);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_route.erase(buffer);
        g_route.erase(stream);
    }
    __imp__sub_003D3408(ctx, base);
    std::lock_guard<std::mutex> lock(g_lock);
    for (uint32_t sound : {buffer, stream})
        if (sound) Apply(ctx, base, sound);
}

// XACT's voice SetVolume(volume) 0x3D2BC3 (thiscall): a buffer's volume, its stream's goes through the thunk below
X86_EXTERN_FUNC(__imp__sub_003D2BC3);
X86_FUNC(sub_003D2BC3) {
    uint32_t buffer = R32(ctx.ecx + 0x1C);
    if (buffer) {
        std::lock_guard<std::mutex> lock(g_lock);
        g_volume[buffer] = int32_t(R32(ctx.esp + 4));
        g_stream[buffer] = false;
        W32(ctx.esp + 4, uint32_t(Effective(buffer)));
    }
    __imp__sub_003D2BC3(ctx, base);
}
X86_FUNC(sub_003AA080) {  // IDirectSoundStream_SetVolume(stream, volume), jmp CDirectSoundStream_SetVolume
    uint32_t stream = R32(ctx.esp + 4);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_volume[stream] = int32_t(R32(ctx.esp + 8));
        g_stream[stream] = true;
        W32(ctx.esp + 8, uint32_t(Effective(stream)));
    }
    sub_003A9908(ctx, base);
}
