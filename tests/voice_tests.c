#include "murmur/voice.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    murmur_voice *sender = murmur_voice_create(0), *receiver = murmur_voice_create(0);
    int16_t input[MURMUR_VOICE_SAMPLES], output[MURMUR_VOICE_SAMPLES];
    uint8_t packet[MURMUR_VOICE_PACKET], last[MURMUR_VOICE_PACKET];
    int frame, i, size, nonzero = 0, last_size = 0;
    double error_sum = 0;
    CHECK(sender && receiver);
    /* Real codec and SPSC buffers, no device enumeration or microphone access. */
    for (frame = 0; frame < 100; ++frame) {
        for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i)
            input[i] = (int16_t)(8000 * sin(2.0 * 3.141592653589793 * 440 * (frame * 960 + i) / 48000.0));
        murmur_voice_capture(sender, input, MURMUR_VOICE_SAMPLES);
        size = murmur_voice_encode(sender, packet);
        CHECK(size > 4 && size <= MURMUR_VOICE_PACKET);
        /* Lose one frame, exercise PLC and acceptance of the next sequence. */
        if (frame != 30) CHECK(murmur_voice_decode(receiver, packet, (size_t)size) == 1);
        murmur_voice_pump(receiver, (uint64_t)frame * 20);
        murmur_voice_playback(receiver, output, MURMUR_VOICE_SAMPLES);
        for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i) {
            CHECK(abs(output[i]) < 24000);
            if (output[i]) ++nonzero;
            error_sum += (double)output[i] * output[i];
        }
        if (frame == 90) { memcpy(last, packet, (size_t)size); last_size = size; }
    }
    CHECK(nonzero > 70000 && error_sum > 1000000);
    CHECK(murmur_voice_decode(receiver, packet, (size_t)size) == 0); /* duplicate */
    CHECK(murmur_voice_decode(receiver, last, (size_t)last_size) == 0); /* out of order */
    CHECK(murmur_voice_decode(receiver, packet, 4) == 0);
    CHECK(murmur_voice_decode(receiver, packet, MURMUR_VOICE_PACKET + 1) == 0);
    CHECK(murmur_voice_encode(sender, packet) == 0); /* no complete capture frame */
    murmur_voice_mute(sender, 1);
    murmur_voice_capture(sender, input, MURMUR_VOICE_SAMPLES);
    CHECK(murmur_voice_encode(sender, packet) == 0);
    murmur_voice_mute(sender, 0);
    murmur_voice_capture(sender, input, MURMUR_VOICE_SAMPLES / 2);
    CHECK(murmur_voice_encode(sender, packet) == 0);
    murmur_voice_capture(sender, input, MURMUR_VOICE_SAMPLES / 2);
    CHECK(murmur_voice_encode(sender, packet) > 4);
    /* Flood capture: memory stays bounded and stale samples are skipped. */
    for (i = 0; i < 20; ++i) murmur_voice_capture(sender, input, MURMUR_VOICE_SAMPLES);
    CHECK(murmur_voice_encode(sender, packet) > 4);
    CHECK(murmur_voice_encode(sender, packet) == 0);
    murmur_voice_free(sender); murmur_voice_free(receiver);
    puts("Opus roundtrip, PLC, replay ordering, mute and bounded buffers passed; no devices opened.");
    return 0;
}
