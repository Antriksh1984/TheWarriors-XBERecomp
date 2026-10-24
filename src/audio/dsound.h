#pragma once
#include <cstdint>

// DirectSound calls this XBE makes through exports the SDK has no name for (src/game/dsound.cpp). The objects are the
// host's buffers and streams, as the title holds them.
namespace warriors::dsound {

// SetPitch: round(4096 * log2(Hz / 48000))
void SetPitch(uint32_t voice, int32_t pitch);
// SetOutputBuffer: the voice plays into a DSBCAPS_MIXIN buffer, 0 to its own mix bins only
void SetOutputBuffer(uint32_t voice, uint32_t mixin);
// Set3DVoiceData(DS3DVOICEDATA*): the levels and direction XACT works out in software, kept by its flags
void Set3DVoiceData(uint32_t voice, uint32_t data);
// Use3DVoiceData(BOOL): the voice's levels follow its 3D voice data
void Use3DVoiceData(uint32_t voice, bool use);
// GetVoiceProperties(DSVOICEPROPS*): the voice's mix bins, pitch and volume
void GetVoiceProperties(uint32_t voice, uint32_t props);

}  // namespace warriors::dsound
