#include "murmur/dsp.h"
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <stdlib.h>
#include <string.h>

/* The echo path may be long on a laptop with speakers: 200 ms of filter covers the device latency
   and a typical room. */
#define FILTER_LENGTH (MURMUR_DSP_RATE / 5)
#define QUEUE (MURMUR_DSP_FRAME * 8)
struct murmur_dsp {
    SpeexEchoState *echo;
    SpeexPreprocessState *clean;
    murmur_dsp_options options;
    int16_t far_queue[QUEUE], in_queue[QUEUE], out_queue[QUEUE];
    size_t far_count, in_count, out_count;
};
static void apply_options(murmur_dsp *d) {
    spx_int32_t on = 1, off = 0, suppress = -28, echo_suppress = -45, echo_active = -25, max_gain = 20;
    float level = 12000.0f;                                                       /* target loudness of the voice */
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_DENOISE, d->options.noise ? &on : &off);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &suppress);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_AGC, d->options.agc ? &on : &off);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_AGC_LEVEL, &level);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_AGC_MAX_GAIN, &max_gain);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS, &echo_suppress);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_ECHO_SUPPRESS_ACTIVE, &echo_active);
}
murmur_dsp *murmur_dsp_create(const murmur_dsp_options *options) {
    murmur_dsp *d = calloc(1, sizeof(*d));
    spx_int32_t rate = MURMUR_DSP_RATE;
    if (!d) return NULL;
    if (options) d->options = *options;
    d->echo = speex_echo_state_init(MURMUR_DSP_FRAME, FILTER_LENGTH);
    d->clean = speex_preprocess_state_init(MURMUR_DSP_FRAME, MURMUR_DSP_RATE);
    if (!d->echo || !d->clean) { murmur_dsp_free(d); return NULL; }
    speex_echo_ctl(d->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
    speex_preprocess_ctl(d->clean, SPEEX_PREPROCESS_SET_ECHO_STATE, d->echo);
    apply_options(d);
    return d;
}
void murmur_dsp_free(murmur_dsp *d) {
    if (!d) return;
    if (d->echo) speex_echo_state_destroy(d->echo);
    if (d->clean) speex_preprocess_state_destroy(d->clean);
    free(d);
}
void murmur_dsp_set(murmur_dsp *d, const murmur_dsp_options *options) {
    if (!d || !options) return;
    if (options->echo && !d->options.echo) speex_echo_state_reset(d->echo);   /* start from a clean estimate */
    d->options = *options;
    apply_options(d);
}
static void push(int16_t *queue, size_t *count, const int16_t *samples, size_t n) {
    if (n > QUEUE) { samples += n - QUEUE; n = QUEUE; }
    if (*count + n > QUEUE) {                       /* keep the newest: a late consumer must not fall behind forever */
        size_t drop = *count + n - QUEUE;
        memmove(queue, queue + drop, (*count - drop) * sizeof(int16_t));
        *count -= drop;
    }
    memcpy(queue + *count, samples, n * sizeof(int16_t));
    *count += n;
}
static void pop(int16_t *queue, size_t *count, size_t n) {
    memmove(queue, queue + n, (*count - n) * sizeof(int16_t));
    *count -= n;
}
void murmur_dsp_far(murmur_dsp *d, const int16_t *playback, size_t count) {
    if (d && playback && count && d->options.echo) push(d->far_queue, &d->far_count, playback, count);
}
void murmur_dsp_near(murmur_dsp *d, int16_t *mic, size_t count) {
    if (!d || !mic || !count) return;
    if (!d->options.echo && !d->options.noise && !d->options.agc) { d->in_count = d->out_count = d->far_count = 0; return; }
    push(d->in_queue, &d->in_count, mic, count);
    while (d->in_count >= MURMUR_DSP_FRAME) {
        int16_t reference[MURMUR_DSP_FRAME] = {0}, cleaned[MURMUR_DSP_FRAME];
        if (d->options.echo) {
            if (d->far_count >= MURMUR_DSP_FRAME) { memcpy(reference, d->far_queue, sizeof(reference)); pop(d->far_queue, &d->far_count, MURMUR_DSP_FRAME); }
            speex_echo_cancellation(d->echo, d->in_queue, reference, cleaned);
        } else memcpy(cleaned, d->in_queue, sizeof(cleaned));
        speex_preprocess_run(d->clean, cleaned);
        push(d->out_queue, &d->out_count, cleaned, MURMUR_DSP_FRAME);
        pop(d->in_queue, &d->in_count, MURMUR_DSP_FRAME);
    }
    /* Hand back exactly `count` samples; before the first block is ready that is silence. */
    if (d->out_count >= count) { memcpy(mic, d->out_queue, count * sizeof(int16_t)); pop(d->out_queue, &d->out_count, count); }
    else { size_t lead = count - d->out_count; memset(mic, 0, lead * sizeof(int16_t)); memcpy(mic + lead, d->out_queue, d->out_count * sizeof(int16_t)); d->out_count = 0; }
}
