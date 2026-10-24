// DirectSound (XDK 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
//
// The SDK's runtime/xdk/dsound.cpp, built in its place (CMakeLists.txt), with what XACT relies on in this game:
// - mix-in buffers: a voice SetOutputBuffer points at a DSBCAPS_MIXIN buffer plays into it, and the mix-in plays the
//   sum as its own source. XACT plays every 3D sound through one of these submixes.
// - XACT's 3D voice data (Set3DVoiceData, Use3DVoiceData): the distance, cone, front, rear, centre and I3DL2 levels it
//   works out in software, applied to the mix bins as the library's 0x3B2FF5 does, the Doppler pitch, and the
//   direction, which places a mono voice between the ears.
// - the amplitude envelope (SetEG), and StopEx and FlushEx that let it release.
// - the stream pause and flush rules of the library's voice (0x3B1EBD, 0x3B1353): DSSTREAMPAUSE_PAUSENOACTIVATE holds
//   an idle stream's packets until Pause(0), and an asynchronous flush stops the voice at once and hands its packets
//   back in DirectSoundDoWork.
#include "audio/dsound.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <optional>
#include <vector>

#include "audio/audio.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"
#include "xdk/dsp_image.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the SDK knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_Pause
#define SYM_IDirectSoundBuffer_Pause FFFFFF0D
#define HAS_IDirectSoundBuffer_Pause 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetEG
#define SYM_IDirectSoundBuffer_SetEG FFFFFF0E
#define HAS_IDirectSoundBuffer_SetEG 0
#endif

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

// DSMIXBIN numbers the host mixes by name
enum MixBin : uint32_t {
    kFrontLeft,
    kFrontRight,
    kCentre,
    kSub,
    kBackLeft,
    kBackRight,
    kCrosstalkFrontLeft,
    kCrosstalkFrontRight,
    kCrosstalkBackLeft,
    kCrosstalkBackRight,
    kI3dl2,
    kFirstSend = kI3dl2,  // effect sends from here up
};
// the voice processor's multipass bin: a voice with an output buffer plays into the mix-in through it
constexpr uint32_t kMultipass = 31;
constexpr uint32_t kBins = dsp::kBins;
constexpr uint32_t kInvalidCall = 0x88780032u;  // DSERR_INVALIDCALL
constexpr uint32_t kMaxChannels = 8;
// rows of a voice's mix: its channels, then a mono 3D voice's two ears
constexpr uint32_t kLeftEar = kMaxChannels;
constexpr uint32_t kRightEar = kMaxChannels + 1;
constexpr uint32_t kRows = kMaxChannels + 2;
constexpr uint32_t kEarLength = 64;  // past the longest interaural delay, 31 samples
constexpr uint32_t kMixIn = 0x2000;  // DSBCAPS_MIXIN

// gain of each row on each mix bin
struct Gains {
    float bin[kRows][kBins];
};

// The amplitude envelope (DSENVELOPEDESC with dwEG 1; 0 is the pitch and filter one). Its times are 12 bit counts of
// 512 samples and its sustain a level of 255; dwMode is the phase SetEG puts it in, the voice processor's EG states.
enum Phase : uint32_t { kEgOff, kEgDelay, kEgAttack, kEgHold, kEgDecay, kEgSustain, kEgRelease, kEgForceRelease };
constexpr uint32_t kAmplitudeEg = 1;
constexpr uint32_t kEgUnit = 512;

struct Envelope {
    uint32_t mode = kEgOff;
    uint32_t delay = 0, attack = 0, hold = 0, decay = 0, release = 0;  // samples
    float sustain = 1;
    uint32_t phase = kEgOff;
    uint32_t t = 0;      // samples into the phase
    float level = 1;     // the last sample's
    float from = 1;      // the level the release started at
};

// Brown and Duda's head shadow (one pole, one zero) for an ear a sound is theta degrees from: 6 dB more treble facing
// it, 20 dB less at 150 degrees, the bass the same
struct Shadow {
    float b0 = 1, b1 = 0, a1 = 0;
    float x1 = 0, y1 = 0;
    void Design(float theta) {
        constexpr float kPi = 3.14159265f;
        constexpr float kBeta = 2 * 343.0f / 0.0875f;    // 2c/a, a the head's radius
        constexpr float kK = 2.0f * float(audio::kRate);  // bilinear transform
        float alpha = 1.05f + 0.95f * std::cos(theta / 150.0f * kPi);
        b0 = (alpha * kK + kBeta) / (kK + kBeta);
        b1 = (kBeta - alpha * kK) / (kK + kBeta);
        a1 = (kBeta - kK) / (kK + kBeta);
    }
    float Run(float x) {
        float y = b0 * x + b1 * x1 - a1 * y1 + 1e-20f;  // the offset keeps silence out of denormals
        x1 = x;
        y1 = y;
        return y;
    }
};

// DS3DVOICEDATA dwords by offset / 4, kept by its flag bits as the voice's settings keep them (0x3A84BB)
enum VoiceData : uint32_t {
    kVdDistance = 1,  // mB, flag 1
    kVdCone = 2,      // mB, flag 2
    kVdFront = 3,     // mB, front pair and centre; flag 4 with kVdRear
    kVdRear = 4,
    kVdFrontOnly = 5,  // mB, flag 8 with kVdCentre
    kVdCentre = 6,
    kVdAll = 7,        // mB, front, rear and centre; flag 0x10 with kVdI3dl2
    kVdI3dl2 = 8,
    kVdDoppler = 9,    // pitch, flag 0x20
    kVdAzimuth = 10,   // degrees, plus to the right; flag 0x40 with kVdElevation
    kVdElevation = 11,
    kVdFilter = 12,    // the I3DL2 filter, flag 0x80 with the one after it
    kVdCount = 14,
};
constexpr uint32_t kDirectionFlag = 0x40;

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    uint32_t caps = 0;  // DSBCAPS
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0, play_start = 0, play_len = 0;
    uint32_t owned = 0;  // memory the library made for the buffer
    bool playing = false, looping = false, paused = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    bool hold = false;      // stream: DSSTREAMPAUSE_PAUSENOACTIVATE while idle, its packets wait for Pause(0)
    bool flushing = false;  // stream: stopped by an asynchronous flush, its packets go back in DirectSoundDoWork
    bool flush_at_end = false;  // stream: flushed so once its envelope's release ends
    double pos = 0;  // frames
    uint32_t freq = 0;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // mix-in: what its sources played into it this block, and whether any did
    uint32_t output = 0;  // the mix-in this voice plays into
    std::vector<float> input;
    bool fed = false;
    // levels
    float volume = 1, headroom = 1;
    int32_t volume_mb = 0;
    uint8_t nbins = 0, bin[8] = {};
    bool speakers = true;  // no DSMIXBINS given: the speakers in order; a list, or a mix-in, takes its place
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d = false;
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    bool use_data = false;  // Use3DVoiceData
    bool has_direction = false;
    int32_t voice_data[kVdCount] = {};
    Envelope eg;
    Gains gain = {};  // the last block's gains, the next block ramps from them
    float send_gain = 0;
    float ear_gain[2] = {1, 1};
    bool has_gain = false;
    // a mono 3D voice's interaural delay in samples, its recent samples and each ear's head shadow
    float itd = 0;
    float ear[kEarLength] = {};
    uint32_t ear_pos = 0;
    Shadow shadow[2];
    // ADPCM block cache: the block read last and the one read before it, a frame between two blocks reads both
    int64_t block[2] = {-1, -1};
    uint8_t last = 0;  // decoded[last] holds block[last]
    int16_t decoded[2][65 * 8];

    // a mix-in buffer plays what other voices play into it
    bool mixin() const { return caps & kMixIn; }
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;
// IDirectSound_SetMixBinHeadroom: the voice processor halves a bin per step, 1 for every bin but the last by default
uint8_t g_bin_headroom[kBins] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                                 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0};
struct Segment {
    uint32_t at;
    uint32_t bytes;
};
std::vector<Segment> g_effect_states;  // each effect's state segment of the downloaded image

void ForgetBlocks(Voice& v) {
    v.block[0] = -1;
    v.block[1] = -1;
}

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }
int32_t ClampMb(int64_t mb) { return int32_t(std::clamp<int64_t>(mb, -10000, 0)); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// the decoded ADPCM block at guest address at, decoded once while it stays in the cache
const int16_t* DecodedBlock(Voice& v, int64_t at) {
    for (uint8_t k = 0; k < 2; k++) {
        if (v.block[k] != at) continue;
        v.last = k;
        return v.decoded[k];
    }
    uint8_t k = v.last ^ 1;  // the slot not read last
    DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded[k]);
    v.block[k] = at;
    v.last = k;
    return v.decoded[k];
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        return DecodedBlock(v, at)[(f % 64) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// the envelope

// starts the envelope over in the phase its mode names
void Trigger(Envelope& e) {
    e.phase = e.mode;
    e.t = 0;
    switch (e.phase) {
        case kEgDelay:
        case kEgAttack: e.level = 0; break;
        case kEgSustain: e.level = e.sustain; break;
        default: e.level = 1; break;
    }
    e.from = e.level;
}

void Enter(Envelope& e, uint32_t phase) {
    e.phase = phase;
    e.t = 0;
}

// the release from where the envelope is; false where there is none to play, an envelope turned off or one of no
// length, and the voice stops at once
bool Release(Envelope& e) {
    if (e.mode == kEgOff || !e.release) return false;
    e.from = e.level;
    Enter(e, kEgRelease);
    return true;
}

// the envelope's level for the next sample; false once its release has run out
bool Step(Envelope& e, float& level) {
    for (;;) {
        switch (e.phase) {
            case kEgDelay:
                if (e.t < e.delay) {
                    e.t++;
                    level = 0;
                    return true;
                }
                Enter(e, kEgAttack);
                continue;
            case kEgAttack:
                if (e.t < e.attack) {
                    level = float(e.t++) / float(e.attack);
                    return true;
                }
                Enter(e, kEgHold);
                continue;
            case kEgHold:
                if (e.t < e.hold) {
                    e.t++;
                    level = 1;
                    return true;
                }
                Enter(e, kEgDecay);
                continue;
            case kEgDecay:
                if (e.t < e.decay) {
                    level = 1 + (e.sustain - 1) * float(e.t++) / float(e.decay);
                    return true;
                }
                Enter(e, kEgSustain);
                continue;
            case kEgSustain: level = e.sustain; return true;
            case kEgRelease:
            case kEgForceRelease: {
                uint32_t length = e.phase == kEgRelease ? e.release : kEgUnit;
                if (e.t < length) {
                    level = e.from * (1 - float(e.t++) / float(length));
                    return true;
                }
                level = 0;
                return false;
            }
            default: level = 1; return true;
        }
    }
}

// the console's stereo downmix of the speaker bins: centre and back pair at -3 dB, the sub dropped
constexpr float kFold = 0.707f;

// A voice with no speaker bin and no effects image plays its loudest bin on the front pair; with speaker bins the
// crosstalk bins join their speakers and effect sends are dropped. Without the title's image nothing reads them.
void RouteWithoutImage(const Voice& v, Gains& g) {
    bool speakers = false;
    for (uint32_t s = 0; s < v.nbins; s++) speakers |= v.bin[s] < kFirstSend && v.bin[s] != kSub;
    if (!v.nbins) speakers = true;
    for (auto& row : g.bin) {
        if (!speakers) {
            float loudest = *std::max_element(row, row + kBins);
            std::fill(row, row + kBins, 0.0f);
            row[kFrontLeft] = loudest;
            row[kFrontRight] = loudest;
            continue;
        }
        row[kFrontLeft] += row[kCrosstalkFrontLeft];
        row[kFrontRight] += row[kCrosstalkFrontRight];
        row[kBackLeft] += row[kCrosstalkBackLeft];
        row[kBackRight] += row[kCrosstalkBackRight];
        std::fill(row + kCrosstalkFrontLeft, row + kBins, 0.0f);
    }
}

// The library gives every hardware voice (two channels) a run of n / voices slots. A stereo voice plays its left
// channel on even slots and its right on odd ones.
uint32_t ChannelOfSlot(const Voice& v, uint32_t voice, uint32_t slot) {
    uint32_t voice_channels = std::min<uint32_t>(v.channels - voice * 2, 2);
    return voice * 2 + slot % voice_channels;
}

// each channel's level on each bin; the voice processor divides a bin by 2^headroom, which the image may undo
Gains SlotGains(const Voice& v) {
    static const uint8_t kSpeakerBins[6] = {0, 1, 2, 3, 4, 5};  // no mix bins given: the speakers in order
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t voices = (channels - 1) / 2 + 1;
    const uint8_t* bin = v.nbins ? v.bin : kSpeakerBins;
    uint32_t n = v.nbins ? v.nbins : std::min<uint32_t>(voices * 2, 6);
    uint32_t per = std::max<uint32_t>(n / voices, 1);
    bool image = dsp::Loaded();
    Gains g = {};
    if (!v.nbins && !v.speakers) return g;
    for (uint32_t k = 0; k < voices; k++) {
        for (uint32_t s = k * per; s < (k + 1) * per && s < n; s++) {
            uint32_t b = bin[s];
            float level = Db(v.bin_mb[b]) * v.volume * v.headroom;
            if (image) level = std::ldexp(level, -g_bin_headroom[b]);
            g.bin[ChannelOfSlot(v, k, s)][b] += level;
        }
    }
    return g;
}

// What a bin loses to XACT's 3D voice data, as the library's 0x3B2FF5 works it out: the distance and cone volumes on
// every bin, the front, rear and centre volumes on theirs, the I3DL2 volume on the reverb send.
float VoiceDataScale(const Voice& v, uint32_t b) {
    if (!v.use_data) return 1;
    const int32_t* d = v.voice_data;
    int32_t base = ClampMb(int64_t(d[kVdDistance]) + d[kVdCone]);
    int64_t mb = base;
    if (b == kCentre) {
        mb = int64_t(d[kVdAll]) + d[kVdCentre] + d[kVdFront] + base;
    } else if (b == kCrosstalkFrontLeft || b == kCrosstalkFrontRight) {
        mb = int64_t(d[kVdAll]) + d[kVdFrontOnly] + d[kVdFront] + base;
    } else if (b == kCrosstalkBackLeft || b == kCrosstalkBackRight) {
        mb = int64_t(d[kVdAll]) + d[kVdRear] + base;
    } else if (b == kI3dl2) {
        mb = int64_t(d[kVdI3dl2]) + base;
    }
    return Db(ClampMb(mb));
}

// A 3D voice as the library's 3D pass sees it. Its levels have no left/right term: a mono voice's two ears differ
// by the interaural delay, and on a stereo console the back crosstalk pair is silent. XACT's voice data carries its
// own levels and only the direction is taken from here.
struct Place {
    bool on = false;
    bool data = false;  // placed by XACT's voice data
    float att = 1;      // distance, every bin
    float front = 1;    // crosstalk front pair
    float centre = 1;   // centre bin
    float azimuth = 0;  // degrees, plus to the right
    float elevation = 0;
};

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// the library's octant estimate of the angle of along from across: 45 degrees where they are equal
float OctantAngle(float along, float across) {
    float a = std::fabs(along);
    float s = std::fabs(across);
    if (a > s) return 45 * s / a;
    if (s > 0) return 90 - 45 * a / s;
    return 0;
}

float Azimuth(const float d[3], const float front[3], const float right[3]) {
    float ahead = Dot(d, front);
    float side = Dot(d, right);
    float deg = OctantAngle(ahead, side);
    if (ahead < 0) deg = 180 - deg;
    return side < 0 ? -deg : deg;
}

float Elevation(const float d[3], const float front[3], const float right[3], const float top[3]) {
    float flat = std::hypot(Dot(d, front), Dot(d, right));
    float up = Dot(d, top);
    float deg = OctantAngle(flat, up);
    return up < 0 ? -deg : deg;
}

// interaural delay in samples by azimuth in steps of 3 degrees at elevation 0, the far ear's HRTF delay
float Itd(float azimuth) {
    static const uint8_t kDelay[61] = {0,  2,  3,  4,  4,  5,  6,  9,  10, 11, 12, 12, 13, 14, 17, 18,
                                       19, 20, 21, 21, 22, 23, 24, 25, 26, 28, 29, 29, 31, 30, 30, 31,
                                       30, 28, 27, 28, 27, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15,
                                       14, 13, 13, 12, 11, 8,  7,  6,  6,  3,  2,  2,  0};
    constexpr int kStep = 3;
    if (!(std::fabs(azimuth) <= 180)) return 0;
    int deg = int(std::fabs(azimuth) + kStep / 2.0f) / kStep * kStep;
    float delay = kDelay[deg / kStep];
    return azimuth < 0 ? -delay : delay;
}

// A voice whose fifth bin is the centre shares the front with it below 45 degrees: 0.7071 cos^2(2a) and
// 0.8409 cos(2a) by whole degrees of azimuth and elevation
void ShareCentre(Place& at) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr int kCentreLimit = 45;
    int az = int(std::fabs(at.azimuth));
    int el = int(std::fabs(at.elevation));
    if (az >= kCentreLimit || el >= kCentreLimit) {
        at.centre = 0;
        return;
    }
    float both = std::cos(2 * az * kRadiansPerDegree) * std::cos(2 * el * kRadiansPerDegree);
    at.centre *= 0.7071f * both;
    at.front *= 1 - 0.5f * both * both;
}

// distance attenuation, 1.0 inside the min distance: min / distance, or the voice's rolloff curve
float Attenuation(const Voice& v, float dist) {
    if (dist <= v.min_dist) return 1;
    if (v.curve.empty()) return v.min_dist / std::min(dist, v.max_dist);
    if (v.max_dist <= v.min_dist) return 1;
    // points spread evenly from min to max distance, 1.0 at min
    float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
    size_t i = std::min(size_t(at), v.curve.size() - 1);
    float from = i ? v.curve[i - 1] : 1.0f;
    return from + (v.curve[i] - from) * (at - i);
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

Place Locate(const Voice& v) {
    static const float kAhead[3] = {0, 0, 1};
    static const float kRight[3] = {1, 0, 0};
    static const float kUp[3] = {0, 1, 0};
    Place at;
    if (v.has_direction) {
        at.on = at.data = true;
        at.azimuth = F32(uint32_t(v.voice_data[kVdAzimuth]));
        at.elevation = F32(uint32_t(v.voice_data[kVdElevation]));
        if (!std::isfinite(at.azimuth) || !std::isfinite(at.elevation)) at.azimuth = at.elevation = 0;
        return at;
    }
    if (!v.is3d || v.mode == 2) return at;
    at.on = true;
    bool relative = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !relative; i++) d[i] -= g_listener[i];
    at.att = Attenuation(v, std::sqrt(Dot(d, d)));
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    const float* front = relative ? kAhead : g_front;
    const float* side = relative ? kRight : right;
    const float* top = relative ? kUp : g_top;
    at.azimuth = Azimuth(d, front, side);
    at.elevation = Elevation(d, front, side, top);
    at.centre = at.front;
    if (v.nbins >= 5 && v.bin[4] == kCentre) ShareCentre(at);
    return at;
}

float BinScale(const Place& at, uint32_t b) {
    if (!at.on || at.data) return 1;
    if (b == kCentre) return at.att * at.centre;
    if (b == kCrosstalkFrontLeft || b == kCrosstalkFrontRight) return at.att * at.front;
    if (b == kCrosstalkBackLeft || b == kCrosstalkBackRight) return 0;
    return at.att;
}

// a mono 3D voice's crosstalk bins play its ears: the left bins the left ear, the right bins the right one
void SplitEars(Gains& g) {
    static const uint32_t kLeft[2] = {kCrosstalkFrontLeft, kCrosstalkBackLeft};
    static const uint32_t kRight[2] = {kCrosstalkFrontRight, kCrosstalkBackRight};
    for (int k = 0; k < 2; k++) {
        std::swap(g.bin[0][kLeft[k]], g.bin[kLeftEar][kLeft[k]]);
        std::swap(g.bin[0][kRight[k]], g.bin[kRightEar][kRight[k]]);
    }
}

Gains Levels(const Voice& v, const Place& at) {
    Gains g = SlotGains(v);
    float scale[kBins];
    for (uint32_t b = 0; b < kBins; b++) scale[b] = BinScale(at, b) * VoiceDataScale(v, b);
    for (auto& row : g.bin) {
        for (uint32_t b = 0; b < kBins; b++) row[b] *= scale[b];
    }
    if (at.on && v.channels == 1) SplitEars(g);
    if (!dsp::Loaded()) RouteWithoutImage(v, g);
    // a sound placed at NaN goes silent alone; its NaN gain would silence the whole mix
    for (auto& row : g.bin) {
        for (float& gain : row) {
            if (!std::isfinite(gain)) gain = 0;
        }
    }
    return g;
}

// what a voice plays into its mix-in through the multipass bin: its volume, headroom and 3D distance
float SendLevel(const Voice& v) {
    float level = v.volume * v.headroom * VoiceDataScale(v, kMultipass);
    return std::isfinite(level) ? level : 0.0f;
}

// Each ear's level and head shadow toward a direction: the far ear 6 dB under the near one at the side (equal power),
// and its treble shadowed by the head
void EarLevels(const Place& at, float gain[2], float theta[2]) {
    constexpr float kRadiansPerDegree = 0.017453293f;
    constexpr float kSpread = 0.6f;
    float side = std::sin(at.azimuth * kRadiansPerDegree) * std::cos(at.elevation * kRadiansPerDegree);
    side = std::clamp(side, -1.0f, 1.0f);
    gain[0] = std::sqrt(1 - kSpread * side);
    gain[1] = std::sqrt(1 + kSpread * side);
    theta[0] = std::acos(-side) / kRadiansPerDegree;  // from the left ear's axis
    theta[1] = std::acos(side) / kRadiansPerDegree;
}

// a row's level on one bin, ramped across the block: levels follow the game once a frame, stepped a fast turn clicks
struct Lane {
    uint32_t row;
    uint32_t bin;
    float gain;
    float step;
};

// the levels the block can hear; one zero at both ends of its ramp adds nothing to the mix
std::vector<Lane> AudibleLanes(const Gains& from, const Gains& to, uint32_t frames) {
    std::vector<Lane> lanes;
    for (uint32_t r = 0; r < kRows; r++) {
        for (uint32_t b = 0; b < kBins; b++) {
            float a = from.bin[r][b];
            float z = to.bin[r][b];
            if (a != 0 || z != 0) lanes.push_back({r, b, a, (z - a) / float(frames)});
        }
    }
    return lanes;
}

// the voice's recent samples, the ear further from the sound reads them later
void PushEar(Voice& v, float x) {
    v.ear[v.ear_pos % kEarLength] = x;
    v.ear_pos++;
}
float EarAt(const Voice& v, float delay) {
    float back = std::max(delay, 0.0f);
    uint32_t whole = uint32_t(back);
    float part = back - float(whole);
    float now = v.ear[(v.ear_pos - 1 - whole) % kEarLength];
    float before = v.ear[(v.ear_pos - 2 - whole) % kEarLength];
    return now + (before - now) * part;
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
void Deactivate(Voice& v) {
    v.sync = false;
    if (v.active) v.active = v.paused = v.ending = false;
}

// a stream playing its packets: one held by PAUSENOACTIVATE or stopped by a flush is not, whatever it holds
bool Live(const Voice& v) { return v.active && !v.hold && !v.flushing; }

// where a frame and the one after it are read from, for interpolation
struct Cursor {
    uint32_t data;
    uint32_t f;
    uint32_t next_data;
    uint32_t next_f;
};

// the stream's frame at its position, none once its packets ran out
std::optional<Cursor> StreamFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
        Packet pk = v.packets.front();
        v.packets.pop_front();
        pk.used = pk.size;
        Complete(v, pk, 0);
        v.pos -= FramesOf(v, pk.size);
        f = uint32_t(v.pos);
    }
    if (v.packets.empty()) {
        v.pos = 0;
        if (v.ending) Deactivate(v);
        return std::nullopt;
    }
    Packet& pk = v.packets.front();
    Cursor at = {pk.data, f, pk.data, f};
    if (f + 1 < FramesOf(v, pk.size)) {
        at.next_f = f + 1;
    } else if (v.packets.size() > 1) {
        at.next_data = v.packets[1].data;
        at.next_f = 0;
    }
    return at;
}

// the buffer's frame at its position, looping or stopping at the end of its region; none once stopped
std::optional<Cursor> BufferFrame(Voice& v) {
    uint32_t f = uint32_t(v.pos);
    uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
    uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
    if (f >= end) {
        if (!v.looping || end <= start) {
            v.playing = false;
            v.pos = 0;
            return std::nullopt;
        }
        v.pos = start + std::fmod(v.pos - start, double(end - start));
        f = uint32_t(v.pos);
    }
    if (!v.data) return std::nullopt;
    uint32_t base = v.data + v.play_start;
    Cursor at = {base, f, base, f};
    if (f + 1 < end) {
        at.next_f = f + 1;
    } else if (v.looping) {
        at.next_f = start;
    }
    return at;
}

// a voice's envelope released to silence: a buffer stops, a stream is flushed in DirectSoundDoWork
void Released(Voice& v) {
    Enter(v.eg, kEgOff);
    v.eg.level = 1;
    if (v.stream) {
        v.flushing = v.hold = true;
        v.flush_at_end = false;
    } else {
        v.playing = false;
    }
}

// The rate a voice plays at: its frequency, the Doppler pitch in its 3D voice data, and that of its mix-in (the
// library adds an output buffer's pitch to its sources', 0x3B3249)
double PlayRate(const Voice& v, const Voice* to) {
    double rate = double(v.freq ? v.freq : v.rate) / audio::kRate;
    int32_t pitch = v.use_data ? v.voice_data[kVdDoppler] : 0;
    if (to && to->use_data) pitch += to->voice_data[kVdDoppler];
    if (pitch) rate *= std::exp2(std::clamp(pitch, -32767, 8191) / 4096.0);
    return rate;
}

// adds the voice to the bins, frames of kBins interleaved, its levels ramped across the block; a voice with a mix-in
// also plays into the mix-in's input, send
void MixVoice(Voice& v, float* bins, uint32_t frames, Voice* to) {
    Place at = Locate(v);
    Gains target = Levels(v, at);
    float itd = Itd(at.azimuth);
    bool ears = at.on && v.channels == 1;
    float ear_gain[2] = {1, 1}, theta[2] = {90, 90};
    if (ears) EarLevels(at, ear_gain, theta);
    float send_to = to ? SendLevel(v) : 0.0f;
    if (!v.has_gain) {
        v.gain = target;
        v.itd = itd;
        v.send_gain = send_to;
        v.ear_gain[0] = ear_gain[0];
        v.ear_gain[1] = ear_gain[1];
    }
    v.has_gain = true;
    if (ears) {
        v.shadow[0].Design(theta[0]);
        v.shadow[1].Design(theta[1]);
    }
    std::vector<Lane> lanes = AudibleLanes(v.gain, target, frames);
    float itd_step = (itd - v.itd) / float(frames);
    float ear_step[2] = {(ear_gain[0] - v.ear_gain[0]) / float(frames), (ear_gain[1] - v.ear_gain[1]) / float(frames)};
    float send_step = (send_to - v.send_gain) / float(frames);
    float* send = to ? to->input.data() : nullptr;
    uint32_t channels = std::min<uint32_t>(v.channels, kMaxChannels);
    uint32_t read = ears ? 1u : 0u;  // the channels a lane plays, a bit each
    for (const Lane& lane : lanes) {
        if (lane.row < kMaxChannels) read |= 1u << lane.row;
    }
    if (send) read |= (1u << channels) - 1;
    double rate = PlayRate(v, to);
    v.block[v.last ^ 1] = -1;  // the title may have rewritten that block's memory since the last mix
    for (uint32_t i = 0; i < frames; i++) {
        float row[kRows] = {};
        if (v.mixin()) {
            row[0] = v.input[i];
        } else {
            std::optional<Cursor> cursor = v.stream ? StreamFrame(v) : BufferFrame(v);
            if (!cursor) break;
            float t = float(v.pos - cursor->f);
            for (uint32_t c = 0; c < channels; c++) {
                if (!(read >> c & 1)) continue;
                float x = Sample(v, cursor->data, cursor->f, int(c));
                row[c] = x + (Sample(v, cursor->next_data, cursor->next_f, int(c)) - x) * t;
            }
        }
        float env;
        if (!Step(v.eg, env)) {
            Released(v);
            break;
        }
        v.eg.level = env;
        if (ears) {
            v.itd += itd_step;
            v.ear_gain[0] += ear_step[0];
            v.ear_gain[1] += ear_step[1];
            PushEar(v, row[0]);
            row[kLeftEar] = v.shadow[0].Run(EarAt(v, v.itd)) * v.ear_gain[0];
            row[kRightEar] = v.shadow[1].Run(EarAt(v, -v.itd)) * v.ear_gain[1];
        }
        float* frame = bins + size_t(i) * kBins;
        for (Lane& lane : lanes) {
            lane.gain += lane.step;
            frame[lane.bin] += row[lane.row] * lane.gain * env;
        }
        if (send) {
            v.send_gain += send_step;
            float sum = 0;
            for (uint32_t c = 0; c < channels; c++) sum += row[c];
            send[i] += sum * v.send_gain * env;
        }
        v.pos += rate;
    }
    v.gain = target;
    v.itd = itd;
    v.send_gain = send_to;
    v.ear_gain[0] = ear_gain[0];
    v.ear_gain[1] = ear_gain[1];
}

bool Audible(const Voice& v) {
    if (v.paused || v.sync) return false;
    if (v.stream) return !v.hold && !v.flushing;
    return v.playing;
}

void Render(float* out, uint32_t frames) {
    static std::vector<float> bins;  // the output thread alone renders
    bins.assign(size_t(frames) * kBins, 0.0f);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) {
        if (!v.mixin()) continue;
        v.input.assign(frames, 0.0f);
        v.fed = false;
    }
    // the sounds, some into their mix-ins, then the mix-ins
    for (auto& [obj, v] : g_voices) {
        if (v.mixin() || !Audible(v)) continue;
        Voice* to = nullptr;
        if (v.output) {
            auto it = g_voices.find(v.output);
            if (it != g_voices.end() && it->second.mixin()) to = &it->second;
        }
        MixVoice(v, bins.data(), frames, to);
        if (to) to->fed = true;
    }
    for (auto& [obj, v] : g_voices) {
        if (!v.mixin() || !v.fed || v.paused) continue;
        MixVoice(v, bins.data(), frames, nullptr);
    }
    if (dsp::Loaded()) dsp::Run(bins.data(), frames);
    for (uint32_t i = 0; i < frames; i++) {
        const float* b = &bins[size_t(i) * kBins];
        out[i * 2] = std::clamp(b[kFrontLeft] + kFold * (b[kCentre] + b[kBackLeft]), -1.0f, 1.0f);
        out[i * 2 + 1] = std::clamp(b[kFrontRight] + kFold * (b[kCentre] + b[kBackRight]), -1.0f, 1.0f);
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void FlushStream(Voice& v);

// The work DirectSoundDoWork does on the title's thread: streams an asynchronous flush or a released envelope stopped
// give their packets back, then the callbacks of every packet done run. Under g_lock.
std::vector<Done> TakeDone() {
    for (auto& [obj, v] : g_voices) {
        if (v.stream && v.flushing) FlushStream(v);
    }
    std::vector<Done> done;
    done.swap(t_flushed);
    done.insert(done.end(), g_done.begin(), g_done.end());
    g_done.clear();
    return done;
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done = TakeDone();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes. The multipass bin is the voice's output buffer, which it keeps.
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;
    v.speakers = !mixbins;
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    uint8_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = R32(pairs + i * 8) & 31;
        if (b == kMultipass) continue;
        v.bin[kept++] = uint8_t(b);
        v.bin_mb[b] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = kept;
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

// DSENVELOPEDESC: dwEG, dwMode, then delay, attack, hold, decay and release times and the sustain level. A playing
// voice's envelope starts over in the new mode, as the voice processor's state is set to it.
void SetEnvelope(Voice& v, uint32_t desc) {
    if (!desc) return;
    if (R32(desc) != kAmplitudeEg) {
        LOG_FIRST(4, "dsound: pitch and filter envelope not reproduced");
        return;
    }
    Envelope& e = v.eg;
    auto time = [&](uint32_t at) { return (R32(desc + at) & 0xFFF) * kEgUnit; };
    e.mode = R32(desc + 4) & 7;
    e.delay = time(8);
    e.attack = time(0xC);
    e.hold = time(0x10);
    e.decay = time(0x14);
    e.release = time(0x18);
    e.sustain = float(R32(desc + 0x1C) & 0xFF) / 255.0f;
    if (v.stream ? Live(v) : v.playing) Trigger(e);
}

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

// SetOutputBuffer (the library's 0x3AA8AE): a voice given a mix-in loses its own mix bins for the mix-in's multipass
// bin, which SetMixBins keeps; one taken off its mix-in keeps the bins it has
void ConnectOutput(Voice& v, uint32_t mixin) {
    if (mixin && mixin != v.output) {
        v.nbins = 0;
        v.speakers = false;
    }
    v.output = mixin;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);  // DSBUFFERDESC: size, flags, bytes, format, mix bins
    v.caps = flags;
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    if (bytes && !v.mixin()) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

// the effect's state segment when offset and size lie inside it, else 0
uint32_t EffectState(uint32_t effect, uint32_t offset, uint32_t size) {
    if (effect >= g_effect_states.size()) return 0;
    const Segment& state = g_effect_states[effect];
    if (offset > state.bytes || size > state.bytes - offset) return 0;
    return state.at;
}

}  // namespace

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx), SYM(IDirectSoundBuffer_Pause), SYM(IDirectSoundBuffer_SetEG),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for (auto& s : g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for (auto& s : g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G<uint8_t>(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 0);  // DSSPEAKER_STEREO
    return 0;
}
// (this, effect, offset, data, size, flags): writes the effect's state
HLE_STDCALL(IDirectSound_SetEffectData, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(state + a[2]), HostPtr(a[3]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
// (this, mix bin, headroom)
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] < kBins) g_bin_headroom[a[1]] = a[2] & 7;
    return 0;
}
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
// (this, DSI3DL2LISTENER, apply): room, room HF, reflections and reverb are integers
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) {
    float p[12];
    for (int i = 0; i < 12; i++) {
        uint32_t v = R32(a[1] + i * 4);
        bool integer = i < 2 || i == 5 || i == 7;
        p[i] = integer ? float(int32_t(v)) : F32(v);
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    dsp::SetListener(p);
    return 0;
}
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done = TakeDone();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// The descriptor keeps the image's effect list so the title finds its indices. State segments hold the image's plain
// state, which the host effects read (xdk/dsp_image.cpp); code, Y memory and scratch point at zeroed memory.
// loc: DSEFFECTIMAGELOC, the I3DL2 reverb's index first
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name, uint32_t loc) {
    constexpr int kStateSegment = 1;
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    std::vector<Segment> states;
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            uint32_t at = u32(m + k * 8);
            bool inside = at <= img.size() && size <= img.size() - at;
            if (k == kStateSegment && inside) std::memcpy(HostPtr(seg), img.data() + at, size);
            if (k == kStateSegment) states.push_back({seg, size});
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    int32_t reverb = loc ? int32_t(R32(loc)) : -1;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_effect_states = states;
    std::vector<uint32_t> addresses;
    for (const Segment& s : states) addresses.push_back(s.at);
    dsp::Load(img, t, addresses, reverb);
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str(), a[1]);
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory", a[3]);
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): reads the effect's state
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t state = EffectState(a[1], a[2], a[4]);
    if (!state || !a[3]) return kInvalidCall;
    std::memcpy(HostPtr(a[3]), HostPtr(state + a[2]), a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
// (this, reserved, reserved, flags): a voice started, or one releasing, starts its envelope over
BUFFER(Play, 4) {
    if (!v.playing || v.eg.phase >= kEgRelease) {
        v.has_gain = false;  // a new sound does not ramp from the last one's levels
        Trigger(v.eg);
    }
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
// (this, time low, time high, flags): DSBSTOPEX_ENVELOPE (1) lets the envelope release, and with
// DSBSTOPEX_RELEASEWAVEFORM (2) the voice leaves its loop to play the rest of the buffer meanwhile (0x3B5383). With
// no release to play it stops now, as the voice processor's does.
BUFFER(StopEx, 4) {
    if (!v.playing) return 0;
    if ((a[3] & 1) && Release(v.eg)) {
        if (a[3] & 2) v.looping = false;
        return 0;
    }
    v.playing = false;
    return 0;
}
// (this, flags): 0 resumes, 1 pauses, 2 holds until IDirectSound_SynchPlayback
BUFFER(Pause, 2) {
    v.paused = a[1] == 1;
    v.sync = a[1] == 2;
    return 0;
}
BUFFER(SetEG, 2) {
    SetEnvelope(v, a[1]);
    return 0;
}
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    ForgetBlocks(v);
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.play_len ? (p + BytesOf(v, 64 * 4)) % v.play_len : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume_mb = int32_t(a[1]);
    v.volume = Db(v.volume_mb);
    return 0;
}
BUFFER(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) { return 0; }
// (this, DS3DBUFFER, apply): position at +4, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(R32(a[1] + 4 + i * 4));
    v.min_dist = F32(R32(a[1] + 0x34));
    v.max_dist = F32(R32(a[1] + 0x38));
    v.mode = R32(a[1] + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
// (this, mix-in buffer or 0)
BUFFER(SetOutputBuffer, 2) {
    ConnectOutput(v, a[1]);
    return 0;
}
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    ForgetBlocks(v);
    return 0;
}

// streams, the object carries the XDK's own vtable

HLE_STDCALL(DirectSoundCreateStream, 2) {
    XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),
                XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t desc = a[0];
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.caps = flags & ~kMixIn;
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    W32(a[1], obj);
    return 0;
}

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            std::lock_guard<std::recursive_mutex> lock(g_lock); \
            Voice* v = Find(a[0]);                              \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap(t_flushed);                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

namespace {

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    v.flushing = v.flush_at_end = false;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

}  // namespace

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    ForgetBlocks(v);
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
STREAM(GetStatus, 2) {
    W32(a[1], v.packets.size() < v.max_packets ? 1u : 0u);  // XMEDIASTATUS_ACCEPT_INPUT
    return 0;
}
// a stream that went quiet starts its levels and envelope fresh with its first packet
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    if (!v.active) {
        v.has_gain = false;
        Trigger(v.eg);
    }
    v.active = true;
    v.ending = false;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
// (this, time low, time high, flags). 0 flushes now. Otherwise (0x3B1353) a playing stream with an envelope flag
// releases it, and with DSSTREAMFLUSHEX_RELEASEWAVEFORM (4) is flushed once the release ends; without one it stops
// now. Short of 4 the flush itself waits for DirectSoundDoWork, so packets the title sends meanwhile go with it.
// XACT stops a stream so (its voice Stop 0x3D2A6C) and starts the next cue with Pause(0) (0x3D2C7E). The reads it
// still had on the way land later here than on the console, after the flush, and would start the stopped stream
// again: a skipped cutscene's dialogue went on over the game. The stream is held as PAUSENOACTIVATE holds it until
// then; the callbacks of its flush run in DirectSoundDoWork, so a cue they start is not held after it.
STREAM(FlushEx, 4) {
    uint32_t flags = a[3];
    if (!flags) {
        FlushStream(v);
        return 0;
    }
    bool live = Live(v);
    if (live && (flags & 4) && Release(v.eg)) {
        v.flush_at_end = true;  // held once the release ends (Released)
        return 0;
    }
    v.hold = true;
    if ((flags & 4) && !live) return 0;
    v.flushing = true;
    return 0;
}
// (this, mode), as the library's voice 0x3B1EBD: DSSTREAMPAUSE_RESUME (0) also lets a held stream start,
// PAUSENOACTIVATE (3) holds an idle stream's packets until then and resumes a playing one
STREAM(Pause, 2) {
    switch (a[1]) {
        case 0: v.hold = v.paused = v.sync = false; break;
        case 1:
            v.paused = true;
            v.sync = false;
            break;
        case 2:
            v.sync = true;
            v.paused = false;
            break;
        case 3:
            if (Live(v)) {
                v.paused = v.sync = false;
            } else {
                v.hold = true;
            }
            break;
        default: return 0x80004005u;
    }
    return 0;
}
STREAM(SetVolume, 2) {
    v.volume_mb = int32_t(a[1]);
    v.volume = Db(v.volume_mb);
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
// (this, DSENVELOPEDESC)
HLE_STDCALL(IDirectSoundStream_SetEG, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (Voice* v = Find(a[0])) SetEnvelope(*v, a[1]);
    return 0;
}
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) { return 0; }

// the exports the SDK has no name for (audio/dsound.h)

namespace warriors::dsound {

void SetPitch(uint32_t voice, int32_t pitch) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (Voice* v = Find(voice)) v->freq = uint32_t(std::lround(48000.0 * std::exp2(pitch / 4096.0)));
}

void SetOutputBuffer(uint32_t voice, uint32_t mixin) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (Voice* v = Find(voice)) ConnectOutput(*v, mixin);
}

void Set3DVoiceData(uint32_t voice, uint32_t data) {
    // each flag bit: the first dword it carries and how many
    static const struct {
        uint32_t flag, first, count;
    } kFields[] = {{0x01, kVdDistance, 1},  {0x02, kVdCone, 1},       {0x04, kVdFront, 2}, {0x08, kVdFrontOnly, 2},
                   {0x10, kVdAll, 2},        {0x20, kVdDoppler, 1},    {kDirectionFlag, kVdAzimuth, 2},
                   {0x80, kVdFilter, 2}};
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(voice);
    if (!v || !data) return;
    uint32_t flags = R32(data) & 0xFF;
    for (const auto& f : kFields) {
        if (!(flags & f.flag)) continue;
        for (uint32_t k = 0; k < f.count; k++) v->voice_data[f.first + k] = int32_t(R32(data + (f.first + k) * 4));
    }
    if (flags & kDirectionFlag) v->has_direction = true;
}

void Use3DVoiceData(uint32_t voice, bool use) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (Voice* v = Find(voice)) v->use_data = use;
}

// DSVOICEPROPS: the mix bin count, 8 bin and volume pairs, then pitch, volume and four I3DL2 and filter levels. Slots
// past the voice's bins report the multipass bin, silent, which SetMixBins leaves out again.
void GetVoiceProperties(uint32_t voice, uint32_t props) {
    constexpr uint32_t kSize = 92;
    constexpr uint32_t kPairs = 8;
    if (!props) return;
    std::memset(HostPtr(props), 0, kSize);
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(voice);
    if (!v) return;
    W32(props, v->nbins);
    for (uint32_t i = 0; i < kPairs; i++) {
        bool used = i < v->nbins;
        W32(props + 4 + i * 8, used ? v->bin[i] : kMultipass);
        W32(props + 8 + i * 8, uint32_t(used ? v->bin_mb[v->bin[i]] : -10000));
    }
    double hz = v->freq ? v->freq : v->rate;
    W32(props + 0x44, uint32_t(int32_t(std::lround(4096.0 * std::log2(std::max(hz, 1.0) / 48000.0)))));
    W32(props + 0x48, uint32_t(v->volume_mb));
}

}  // namespace warriors::dsound
