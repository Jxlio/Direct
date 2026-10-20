#include "murmur/voice.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
typedef struct { uint8_t data[MURMUR_VOICE_PACKET]; int size; } encoded;
static encoded packets[100];
static int prepare(void) {
    murmur_voice *sender = murmur_voice_create(0);
    int16_t input[MURMUR_VOICE_SAMPLES];
    int frame, i;
    CHECK(sender);
    for (frame = 0; frame < 100; ++frame) {
        for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i)
            input[i] = (int16_t)(6000 * sin(6.283185307179586 * 440 * (frame * 960 + i) / 48000.0));
        murmur_voice_capture(sender, input, MURMUR_VOICE_SAMPLES);
        packets[frame].size = murmur_voice_encode(sender, packets[frame].data);
        CHECK(packets[frame].size > 4);
    }
    murmur_voice_free(sender);
    return 0;
}
static int simulation(int losses, int late, int wrap) {
    const int delays[] = {25, 0, 35, 0, 10};
    murmur_voice *receiver = murmur_voice_create(0);
    murmur_voice_stats stats;
    int16_t output[240];
    int sent[100] = {0};
    int now, i;
    CHECK(receiver);
    for (now = 0; now <= 2040; now += 5) {
        for (i = 0; i < 100; ++i) {
            int arrival = i * 20 + (late && i == 20 ? 100 : delays[i % 5]);
            if (sent[i] || now < arrival || (losses && (i == 20 || i == 21))) continue;
            {
                encoded packet = packets[i];
                if (wrap) {
                    uint32_t sequence = UINT32_MAX - 2 + (uint32_t)i;
                    packet.data[0] = (uint8_t)(sequence >> 24); packet.data[1] = (uint8_t)(sequence >> 16);
                    packet.data[2] = (uint8_t)(sequence >> 8); packet.data[3] = (uint8_t)sequence;
                }
                CHECK(murmur_voice_decode(receiver, packet.data, (size_t)packet.size) == (late && i == 20 ? 0 : 1));
                if (i == 10) CHECK(murmur_voice_decode(receiver, packet.data, (size_t)packet.size) == 0);
            }
            sent[i] = 1;
        }
        murmur_voice_pump(receiver, (uint64_t)now);
        murmur_voice_playback(receiver, output, 240);
        murmur_voice_get_stats(receiver, &stats);
        CHECK(stats.buffered <= MURMUR_VOICE_JITTER_PACKETS);
    }
    CHECK(stats.decoded == (uint64_t)(losses ? 98 : late ? 99 : 100));
    CHECK(stats.concealed == (uint64_t)(losses ? 2 : late ? 1 : 0));
    CHECK(stats.reordered > 0 && stats.duplicates == 1);
    CHECK(stats.late == (uint64_t)(late ? 1 : 0));
    murmur_voice_free(receiver);
    return 0;
}
static int silence_and_stall(void) {
    murmur_voice *receiver = murmur_voice_create(0);
    murmur_voice_stats stats;
    int16_t output[MURMUR_VOICE_SAMPLES];
    int now, i;
    CHECK(receiver);
    CHECK(murmur_voice_decode(receiver, packets[0].data, (size_t)packets[0].size));
    for (now = 0; now <= 200; now += 20) {
        murmur_voice_pump(receiver, (uint64_t)now);
        murmur_voice_playback(receiver, output, MURMUR_VOICE_SAMPLES);
    }
    murmur_voice_get_stats(receiver, &stats);
    CHECK(stats.decoded == 1 && stats.concealed == 3 && stats.buffered == 0);
    CHECK(murmur_voice_decode(receiver, packets[1].data, (size_t)packets[1].size));
    murmur_voice_pump(receiver, 220); murmur_voice_pump(receiver, 260);
    murmur_voice_get_stats(receiver, &stats); CHECK(stats.decoded == 2);
    for (i = 2; i < 30; ++i) {
        CHECK(murmur_voice_decode(receiver, packets[i].data, (size_t)packets[i].size));
        murmur_voice_get_stats(receiver, &stats); CHECK(stats.buffered <= 8);
    }
    murmur_voice_pump(receiver, 280);
    murmur_voice_pump(receiver, 1000); /* long scheduler stall */
    murmur_voice_get_stats(receiver, &stats);
    CHECK(stats.buffered <= 1 && stats.resyncs > 0);
    murmur_voice_free(receiver);
    return 0;
}
int main(void) {
    CHECK(!prepare());
    CHECK(!simulation(0, 0, 0));
    CHECK(!simulation(1, 0, 0));
    CHECK(!simulation(0, 1, 0));
    CHECK(!simulation(0, 0, 1));
    CHECK(!silence_and_stall());
    puts("Simulated jitter, reordering, losses, late packets, wrap, silence and stalls passed without devices.");
    return 0;
}
