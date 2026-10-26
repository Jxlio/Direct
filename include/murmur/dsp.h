#ifndef MURMUR_DSP_H
#define MURMUR_DSP_H
#include <stddef.h>
#include <stdint.h>

/* Speech clean-up for the microphone: echo cancellation, noise suppression and automatic gain.
 * Mono, 48 kHz, 16-bit. Works in blocks of 10 ms and keeps its own small queues, so callers may pass
 * any number of samples. No allocation after creation: safe to call from an audio callback.
 *
 * Echo cancellation needs to know what was sent to the speakers: call murmur_dsp_far() with each
 * block of playback, then murmur_dsp_near() with the microphone block recorded at the same time. */
#define MURMUR_DSP_RATE 48000
#define MURMUR_DSP_FRAME 480

typedef struct murmur_dsp murmur_dsp;
typedef struct { int echo, noise, agc; } murmur_dsp_options;

murmur_dsp *murmur_dsp_create(const murmur_dsp_options *options);
void murmur_dsp_free(murmur_dsp *dsp);
void murmur_dsp_set(murmur_dsp *dsp, const murmur_dsp_options *options);
void murmur_dsp_far(murmur_dsp *dsp, const int16_t *playback, size_t count);
/* Cleans `count` microphone samples in place. With every option off the samples are left untouched. */
void murmur_dsp_near(murmur_dsp *dsp, int16_t *microphone, size_t count);
#endif
