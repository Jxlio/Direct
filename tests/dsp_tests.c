#include "murmur/dsp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define SECOND MURMUR_DSP_RATE
#define PI 3.14159265358979

static unsigned seed = 20261026u;
static double noise(void) { seed = seed * 1664525u + 1013904223u; return ((seed >> 8) / 8388608.0) - 1.0; }
/* Speech-like: low-passed noise that switches on and off in syllables. */
static double speech(long n, double amplitude) {
    static double state;
    long phase = n % (SECOND / 3);
    state += 0.35 * (noise() - state);
    return phase < SECOND / 5 ? amplitude * state * 2.2 : 0.0;
}
static double power(const int16_t *x, long n) { double sum = 0; long i; for (i = 0; i < n; ++i) sum += (double)x[i] * x[i]; return sum / (double)n; }
static double db(double ratio) { return 10.0 * log10(ratio > 1e-12 ? ratio : 1e-12); }
static double tone_amplitude(const int16_t *x, long n, double hz) {      /* Goertzel */
    double w = 2.0 * PI * hz / SECOND, c = 2.0 * cos(w), s1 = 0, s2 = 0, s;
    long i;
    for (i = 0; i < n; ++i) { s = x[i] + c * s1 - s2; s2 = s1; s1 = s; }
    return sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) * 2.0 / (double)n;
}
static int16_t clip(double v) { return (int16_t)(v > 32000 ? 32000 : v < -32000 ? -32000 : v); }

/* A small room: the speaker signal comes back after 50-85 ms, in four reflections. */
static const struct { long delay; double gain; } room[] = {{2400, 0.45}, {2640, 0.25}, {3300, 0.15}, {4100, 0.08}};
static double echo_of(const int16_t *far, long n) {
    double sum = 0; unsigned k;
    for (k = 0; k < sizeof(room) / sizeof(room[0]); ++k) if (n >= room[k].delay) sum += room[k].gain * far[n - room[k].delay];
    return sum;
}
/* Runs far-end speech through the room, with an optional near-end tone in the second half, in blocks of `block`. */
static void run(murmur_dsp *dsp, int16_t *far, int16_t *mic, int16_t *out, long total, double tone_amp, long block) {
    long n;
    for (n = 0; n < total; ++n) {
        far[n] = clip(speech(n, 9000));
        mic[n] = clip(echo_of(far, n) + 12.0 * noise() + (tone_amp > 0 && n > total / 2 ? tone_amp * sin(2.0 * PI * 600.0 * (double)n / SECOND) : 0.0));
    }
    memcpy(out, mic, (size_t)total * sizeof(int16_t));
    for (n = 0; n < total; n += block) {
        long take = n + block <= total ? block : total - n;
        murmur_dsp_far(dsp, far + n, (size_t)take);
        murmur_dsp_near(dsp, out + n, (size_t)take);
    }
}

int main(void) {
    const long total = 10 * SECOND;
    int16_t *far = malloc((size_t)total * 2), *mic = malloc((size_t)total * 2), *out = malloc((size_t)total * 2);
    murmur_dsp_options echo_only = {1, 0, 0}, none = {0, 0, 0}, all = {1, 1, 1};
    murmur_dsp *dsp;
    long from;
    CHECK(far && mic && out);

    /* Off means off: the microphone is returned untouched, bit for bit, in any block size. */
    dsp = murmur_dsp_create(&none); CHECK(dsp);
    run(dsp, far, mic, out, SECOND, 0, 441);
    CHECK(!memcmp(mic, out, (size_t)SECOND * 2));
    murmur_dsp_free(dsp);

    /* Echo: after a few seconds of learning the room, the echo is cut by well over 15 dB. */
    dsp = murmur_dsp_create(&echo_only); CHECK(dsp);
    run(dsp, far, mic, out, total, 0, MURMUR_DSP_FRAME);
    from = 8 * SECOND;
    { double before = power(mic + from, 2 * SECOND), after = power(out + from, 2 * SECOND);
      fprintf(stderr, "echo: %.1f dB -> %.1f dB (reduction %.1f dB)\n", db(before), db(after), db(before / after));
      CHECK(db(before / after) > 15.0); }
    murmur_dsp_free(dsp);

    /* The same, with blocks that are not a multiple of 10 ms (as a sound card may deliver them). */
    dsp = murmur_dsp_create(&echo_only); CHECK(dsp);
    run(dsp, far, mic, out, total, 0, 441);
    { double before = power(mic + from, 2 * SECOND), after = power(out + from, 2 * SECOND);
      fprintf(stderr, "echo, 441-sample blocks: reduction %.1f dB\n", db(before / after));
      CHECK(db(before / after) > 12.0); }
    murmur_dsp_free(dsp);

    /* Double talk: a person speaking over the echo must still be heard. */
    dsp = murmur_dsp_create(&echo_only); CHECK(dsp);
    run(dsp, far, mic, out, total, 3000.0, MURMUR_DSP_FRAME);
    { double kept = tone_amplitude(out + 7 * SECOND, 3 * SECOND, 600.0), wanted = tone_amplitude(mic + 7 * SECOND, 3 * SECOND, 600.0);
      fprintf(stderr, "near-end tone kept: %.0f of %.0f\n", kept, wanted);
      CHECK(kept > 0.4 * wanted); }
    murmur_dsp_free(dsp);

    /* Noise: steady background noise is reduced between words. */
    { murmur_dsp_options denoise = {0, 1, 0};
      int16_t *a = out, *b = mic; long i, quiet = 0; double with = 0, without = 0;
      for (i = 0; i < total; ++i) far[i] = clip(speech(i, 9000) + 500.0 * noise());
      memcpy(a, far, (size_t)total * 2); memcpy(b, far, (size_t)total * 2);
      dsp = murmur_dsp_create(&denoise); CHECK(dsp);
      for (i = 0; i + MURMUR_DSP_FRAME <= total; i += MURMUR_DSP_FRAME) murmur_dsp_near(dsp, a + i, MURMUR_DSP_FRAME);
      for (i = 5 * SECOND; i < total; ++i) if (i % (SECOND / 3) > SECOND / 5 + 3000) { with += (double)a[i] * a[i]; without += (double)b[i] * b[i]; ++quiet; }
      fprintf(stderr, "noise between words: %.1f dB -> %.1f dB\n", db(without / (double)quiet), db(with / (double)quiet));
      CHECK(quiet > 1000 && db(without / with) > 6.0);
      murmur_dsp_free(dsp); }

    /* Automatic gain: a quiet voice is brought up, a loud one is not pushed into clipping. */
    { murmur_dsp_options gain = {0, 0, 1}; long i; double raise;
      for (i = 0; i < total; ++i) mic[i] = clip(speech(i, 600));
      memcpy(out, mic, (size_t)total * 2);
      dsp = murmur_dsp_create(&gain); CHECK(dsp);
      for (i = 0; i + MURMUR_DSP_FRAME <= total; i += MURMUR_DSP_FRAME) murmur_dsp_near(dsp, out + i, MURMUR_DSP_FRAME);
      raise = db(power(out + 6 * SECOND, 3 * SECOND) / power(mic + 6 * SECOND, 3 * SECOND));
      fprintf(stderr, "quiet voice raised by %.1f dB\n", raise);
      CHECK(raise > 6.0);
      murmur_dsp_free(dsp);
      for (i = 0; i < total; ++i) mic[i] = clip(speech(i, 30000));
      memcpy(out, mic, (size_t)total * 2);
      dsp = murmur_dsp_create(&gain); CHECK(dsp);
      for (i = 0; i + MURMUR_DSP_FRAME <= total; i += MURMUR_DSP_FRAME) murmur_dsp_near(dsp, out + i, MURMUR_DSP_FRAME);
      CHECK(power(out + 6 * SECOND, 3 * SECOND) < 1.2 * power(mic + 6 * SECOND, 3 * SECOND));
      murmur_dsp_free(dsp); }

    /* Switching options while running is safe, and bad arguments are ignored. */
    dsp = murmur_dsp_create(&none); CHECK(dsp);
    { long n; int16_t block[441];
      for (n = 0; n < 200; ++n) {
          int k; for (k = 0; k < 441; ++k) block[k] = clip(speech(n * 441 + k, 5000));
          murmur_dsp_far(dsp, block, 441);
          if (n == 50) murmur_dsp_set(dsp, &all);
          if (n == 100) murmur_dsp_set(dsp, &echo_only);
          if (n == 150) murmur_dsp_set(dsp, &none);
          murmur_dsp_near(dsp, block, 441);
      } }
    murmur_dsp_near(NULL, out, 10); murmur_dsp_far(NULL, out, 10); murmur_dsp_set(NULL, &all); murmur_dsp_free(NULL);
    murmur_dsp_free(dsp);
    free(far); free(mic); free(out);
    puts("Echo cancellation, noise suppression and automatic gain passed on synthetic rooms.");
    return 0;
}
