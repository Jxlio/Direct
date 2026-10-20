#ifndef MURMUR_VOICE_H
#define MURMUR_VOICE_H
#include <stddef.h>
#include <stdint.h>
#define MURMUR_VOICE_RATE 48000
#define MURMUR_VOICE_SAMPLES 960
#define MURMUR_VOICE_PACKET 404
#define MURMUR_VOICE_JITTER_PACKETS 8
typedef struct {
    uint64_t received, decoded, concealed, late, duplicates, reordered, resyncs;
    unsigned buffered;
} murmur_voice_stats;
typedef struct murmur_voice murmur_voice;
/* Owner thread creates, encodes, decodes, changes mute and destroys.
 * hardware=0 never enumerates/opens devices, for deterministic offline tests.
 * hardware=1 opens the default duplex device and must follow explicit opt-in. */
murmur_voice *murmur_voice_create(int hardware);
void murmur_voice_free(murmur_voice *voice);
void murmur_voice_mute(murmur_voice *voice, int muted);
/* Returns encoded size, 0 if no complete frame, -1 on codec failure. */
int murmur_voice_encode(murmur_voice *voice, uint8_t packet[MURMUR_VOICE_PACKET]);
/* Enqueue authenticated packets for ordered playout. Returns 1 accepted,
 * 0 stale/duplicate/invalid/unsupported-duration packet. Owner thread only. */
int murmur_voice_decode(murmur_voice *voice, const uint8_t *packet, size_t size);
/* Owner thread supplies a monotonic millisecond clock, normally every 5-10 ms.
 * 40 ms reorder window; at most two frames produced per call. Never call in
 * the hardware callback. No allocation and no device access in this function. */
void murmur_voice_pump(murmur_voice *voice, uint64_t now_ms);
void murmur_voice_get_stats(const murmur_voice *voice, murmur_voice_stats *stats);
/* Callback-side SPSC operations, also available to headless tests.
 * Exactly one producer for capture and one consumer for playback. */
void murmur_voice_capture(murmur_voice *voice, const int16_t *samples, size_t count);
void murmur_voice_playback(murmur_voice *voice, int16_t *samples, size_t count);
#endif
