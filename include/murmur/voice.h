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
/* Empty device IDs follow the system default. IDs are backend-specific. */
typedef struct {
    uint8_t input_id[256], output_id[256];
    unsigned input_selected, output_selected, input_volume, output_volume;
    /* Added later, kept at the end so older saved settings still load. Each person sets their own:
       echo, noise and gain clean-up on or off (available only when built with the DSP), and the
       microphone gate: below this loudness (0-100, the scale of murmur_voice_levels) nothing is sent. */
    unsigned echo, noise, agc, gate;
} murmur_audio_config;
/* Size of the settings saved by versions before the clean-up options existed. */
#define MURMUR_AUDIO_CONFIG_BASE_SIZE offsetof(murmur_audio_config, echo)
typedef struct { char name[256]; uint8_t id[256]; } murmur_audio_device;
void murmur_audio_defaults(murmur_audio_config *config);
int murmur_audio_devices(int capture, murmur_audio_device *devices, unsigned capacity);
murmur_voice *murmur_voice_create_config(int hardware, const murmur_audio_config *config);
void murmur_voice_volume(murmur_voice *voice, unsigned input, unsigned output);
/* Live changes from the owner thread: clean-up switches and the microphone gate (0 = open). */
void murmur_voice_options(murmur_voice *voice, unsigned echo, unsigned noise, unsigned agc, unsigned gate);
/* 1 when this build can clean the microphone (echo, noise, gain), 0 when only the gate is available. */
int murmur_voice_cleanup_available(void);
typedef struct murmur_audio_preview murmur_audio_preview;
/* Explicit local test: capture shows a meter, playback emits a quiet tone. */
murmur_audio_preview *murmur_audio_preview_start(int capture, const murmur_audio_config *config);
void murmur_audio_preview_stop(murmur_audio_preview *preview);
unsigned murmur_audio_preview_level(murmur_audio_preview *preview);
void murmur_audio_preview_volume(murmur_audio_preview *preview, unsigned input, unsigned output);
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
/* Loudness 0..100 of what the microphone sends and of what the other side sends, for "who is
 * speaking". Square-root scale of the peak, which follows hearing better than a linear one:
 * normal speech lands around 30-70, a quiet room below 10. A side reads 0 when muted or when no
 * audio arrived for 150 ms. Owner thread, with the same clock as murmur_voice_pump. */
void murmur_voice_levels(murmur_voice *voice, uint64_t now_ms, unsigned *input, unsigned *output);
/* Callback-side SPSC operations, also available to headless tests.
 * Exactly one producer for capture and one consumer for playback. */
void murmur_voice_capture(murmur_voice *voice, const int16_t *samples, size_t count);
void murmur_voice_playback(murmur_voice *voice, int16_t *samples, size_t count);
#endif
