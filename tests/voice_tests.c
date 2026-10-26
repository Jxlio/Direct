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
    /* "Who is speaking". The test tone above is coded as comfort noise (DTX), so this uses loud noise,
       which the codec treats as speech. Loud while audio flows; zero when it stops or the microphone is muted. */
    { murmur_voice *talker = murmur_voice_create(0), *listener = murmur_voice_create(0);
      unsigned in_level = 99, out_level = 99, seed = 12345;
      int16_t noise[MURMUR_VOICE_SAMPLES], hush[MURMUR_VOICE_SAMPLES] = {0};
      int k;
      CHECK(talker && listener);
      for (k = 0; k < 60; ++k) {
          for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i) { seed = seed * 1103515245u + 12345u; noise[i] = (int16_t)((int)((seed >> 16) % 16001) - 8000); }
          murmur_voice_capture(talker, noise, MURMUR_VOICE_SAMPLES);
          size = murmur_voice_encode(talker, packet);
          CHECK(size > 4);
          CHECK(murmur_voice_decode(listener, packet, (size_t)size) == 1);
          murmur_voice_pump(listener, (uint64_t)k * 20);
          murmur_voice_playback(listener, output, MURMUR_VOICE_SAMPLES);
      }
      murmur_voice_levels(talker, 1190, &in_level, &out_level);
      CHECK(in_level > 40 && in_level < 100 && out_level == 0);         /* the talker hears itself loud; nothing came back */
      murmur_voice_levels(listener, 1190, &in_level, &out_level);
      CHECK(out_level > 25 && in_level == 0);                           /* the listener plays the peer; its microphone is silent */
      murmur_voice_levels(listener, 1500, &in_level, &out_level);
      CHECK(out_level == 0);                                            /* the peer went quiet */
      murmur_voice_levels(talker, 1500, &in_level, &out_level);
      CHECK(in_level == 0);                                             /* no new microphone audio either */
      murmur_voice_capture(talker, noise, MURMUR_VOICE_SAMPLES);
      murmur_voice_levels(talker, 1510, &in_level, &out_level);
      CHECK(in_level > 40);
      murmur_voice_mute(talker, 1);
      murmur_voice_levels(talker, 1520, &in_level, &out_level);
      CHECK(in_level == 0);                                             /* muted reads as silent at once */
      murmur_voice_mute(talker, 0);
      for (k = 0; k < 40; ++k) murmur_voice_capture(talker, hush, MURMUR_VOICE_SAMPLES);
      murmur_voice_levels(talker, 1530, &in_level, &out_level);
      CHECK(in_level < 5);                                              /* silence decays away */
      murmur_voice_levels(NULL, 0, &in_level, &out_level);
      CHECK(in_level == 0 && out_level == 0);
      murmur_voice_levels(talker, 1540, NULL, NULL);
      /* The microphone gate: below the chosen loudness nothing is sent; above it, the voice passes and the
         gate stays open a moment after, so the ends of words are kept. */
      { int16_t faint[MURMUR_VOICE_SAMPLES]; unsigned level_in = 0, ignored = 0, again = 99;
        for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i) { seed = seed * 1103515245u + 12345u; faint[i] = (int16_t)((int)((seed >> 16) % 1001) - 500); }
        murmur_voice_options(talker, 0, 0, 0, 30);
        for (k = 0; k < 20; ++k) murmur_voice_capture(talker, faint, MURMUR_VOICE_SAMPLES);
        murmur_voice_levels(talker, 2000, &level_in, &ignored);
        CHECK(level_in == 0);                                           /* faint noise stays behind the gate */
        murmur_voice_capture(talker, noise, MURMUR_VOICE_SAMPLES);
        murmur_voice_levels(talker, 2010, &level_in, &ignored);
        CHECK(level_in > 40);                                           /* a voice opens it */
        murmur_voice_capture(talker, faint, MURMUR_VOICE_SAMPLES);
        murmur_voice_levels(talker, 2020, &level_in, &ignored);
        CHECK(level_in > 20);                                           /* still open right after the voice stopped */
        for (k = 0; k < 12; ++k) murmur_voice_capture(talker, faint, MURMUR_VOICE_SAMPLES);
        murmur_voice_levels(talker, 2030, &again, &ignored);
        CHECK(again < 5);                                               /* closed again after the pause */
        murmur_voice_options(talker, 0, 0, 0, 0);
        for (k = 0; k < 3; ++k) murmur_voice_capture(talker, faint, MURMUR_VOICE_SAMPLES);
        murmur_voice_levels(talker, 2040, &level_in, &ignored);
        CHECK(level_in > 3);                                            /* with the gate off the faint sound passes */
        CHECK(murmur_voice_cleanup_available() == 0 || murmur_voice_cleanup_available() == 1); }
      murmur_voice_free(talker); murmur_voice_free(listener); }
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
    /* Playback gain is applied even to PCM already in the jitter buffer. */
    murmur_voice_volume(receiver,100,0);
    murmur_voice_playback(receiver,output,MURMUR_VOICE_SAMPLES);
    for(i=0;i<MURMUR_VOICE_SAMPLES;++i) CHECK(output[i]==0);
    murmur_voice_free(sender); murmur_voice_free(receiver);
    {
        murmur_audio_config config;
        murmur_audio_defaults(&config);
        CHECK(config.input_volume==100 && config.output_volume==100 && !config.input_selected && !config.output_selected);
        config.input_volume=0;
        sender=murmur_voice_create_config(0,&config); receiver=murmur_voice_create(0);
        CHECK(sender && receiver);
        for(frame=0;frame<12;++frame) {
            int control_size;
            for(i=0;i<MURMUR_VOICE_SAMPLES;++i) input[i]=4000;
            memset(output,0,sizeof(output));
            murmur_voice_capture(sender,input,MURMUR_VOICE_SAMPLES);
            murmur_voice_capture(receiver,output,MURMUR_VOICE_SAMPLES);
            size=murmur_voice_encode(sender,packet);
            control_size=murmur_voice_encode(receiver,last);
            CHECK(size>4 && size==control_size && !memcmp(packet,last,(size_t)size));
        }
        murmur_voice_free(sender); murmur_voice_free(receiver);
    }
    puts("Opus roundtrip, PLC, replay ordering, mute and bounded buffers passed; no devices opened.");
    return 0;
}
