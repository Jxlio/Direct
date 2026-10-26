#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "murmur/ice.h"
#include "murmur/voice.h"
#include "murmur/friend.h"
#include <juice/juice.h>
#include <mbedtls/timing.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define pause_ms() Sleep(2)
#else
#include <time.h>
static void pause_ms(void) { struct timespec delay = {0, 2000000}; nanosleep(&delay, NULL); }
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static struct mbedtls_timing_hr_time timer;
static uint64_t now_ms(void) { return mbedtls_timing_get_timer(&timer, 0); }
static int token(murmur_ice *p, char out[MURMUR_ICE_TOKEN]) {
    uint64_t deadline = now_ms() + 8000;
    int result;
    do { result = murmur_ice_local_token(p, out, MURMUR_ICE_TOKEN); if (result) return result; pause_ms(); } while (now_ms() < deadline);
    return -1;
}
static int friend_signal(char text[MURMUR_ICE_TOKEN], unsigned expected_role) {
    uint8_t key[32], session[32], received_session[32], packet[MURMUR_FRIEND_PACKET];
    char decoded[MURMUR_ICE_TOKEN]; unsigned role; size_t size = sizeof(packet);
    CHECK(!murmur_random_key(key) && !murmur_random_key(session));
    CHECK(!murmur_friend_seal(key, expected_role, session, text, packet, &size));
    CHECK(!murmur_friend_open(key, packet, size, &role, received_session, decoded));
    CHECK(role == expected_role && !memcmp(session, received_session, 32) && !strcmp(text, decoded));
    strcpy(text, decoded); return 0;
}
static int exchange(int relay) {
    murmur_ice_config settings = {0};
    murmur_ice *host, *client;
    juice_server_t *server = NULL;
    char offer[MURMUR_ICE_TOKEN], answer[MURMUR_ICE_TOKEN], bad[MURMUR_ICE_TOKEN];
    uint8_t payload[MURMUR_CHAT_LIMIT], incoming[MURMUR_CHAT_LIMIT];
    uint64_t deadline;
    unsigned room;
    size_t size;
    murmur_kind kind;
    int hs = 0, cs = 0, got = 0, test;
    strcpy(settings.bind_address, "127.0.0.1");
    if (relay) {
        juice_server_credentials_t credentials = {"murmur-test", "test-only-relay-password", 4};
        juice_server_config_t config = {0};
        config.credentials = &credentials; config.credentials_count = 1;
        config.max_allocations = 4; config.max_peers = 8;
        config.bind_address = "127.0.0.1"; config.external_address = "127.0.0.1"; config.port = 49190;
        config.relay_port_range_begin = 49191; config.relay_port_range_end = 49200;
        server = juice_server_create(&config); CHECK(server);
        strcpy(settings.stun_host, "127.0.0.1"); settings.stun_port = 49190;
        strcpy(settings.turn_host, "127.0.0.1"); settings.turn_port = 49190;
        strcpy(settings.username, credentials.username); strcpy(settings.password, credentials.password);
        settings.require_relay = 1;
    }
    host = murmur_ice_create(1, &settings, NULL); CHECK(host);
    CHECK(token(host, offer) == 1);
    CHECK(!friend_signal(offer, 0));
    strcpy(bad, offer); bad[20] = bad[20] == 'A' ? 'B' : 'A';
    CHECK(murmur_ice_create(0, &settings, bad) == NULL);
    client = murmur_ice_create(0, &settings, offer); CHECK(client);
    CHECK(token(client, answer) == 1);
    CHECK(!friend_signal(answer, 1));
    strcpy(bad, answer); bad[60] = bad[60] == 'A' ? 'B' : 'A';
    CHECK(murmur_ice_accept_answer(host, bad) == -1);
    if (!relay) {
        /* Simulate time spent copying the answer while ICE/DTLS traffic arrives. */
        deadline = now_ms() + 1100;
        while (now_ms() < deadline) { CHECK(murmur_ice_step(client, now_ms()) >= 0); pause_ms(); }
    }
    CHECK(!murmur_ice_accept_answer(host, answer));
    deadline = now_ms() + 12000;
    do {
        hs = murmur_ice_step(host, now_ms()); cs = murmur_ice_step(client, now_ms());
        if (hs < 0 || cs < 0) { fprintf(stderr, "ICE/DTLS errors: %d/%d\n", murmur_ice_error(host), murmur_ice_error(client)); break; }
        if (hs == 4 && cs == 4) break;
        pause_ms();
    } while (now_ms() < deadline);
    CHECK(hs == 4 && cs == 4);
    CHECK(!!murmur_ice_relayed(host) == relay && !!murmur_ice_relayed(client) == relay);
    memset(payload, 'a', sizeof(payload));
    for (test = 0; test < 3; ++test) {
        size_t length = test == 1 ? sizeof(payload) : 17;
        CHECK(!murmur_ice_send(client, (unsigned)test, payload, length));
        deadline = now_ms() + 4000; got = 0;
        do {
            CHECK(murmur_ice_step(client, now_ms()) == 4 && murmur_ice_step(host, now_ms()) == 4);
            got = murmur_ice_receive(host, &kind, &room, incoming, sizeof(incoming), &size);
            if (got) break;
            pause_ms();
        } while (now_ms() < deadline);
        CHECK(got == 1 && kind == MURMUR_TEXT && room == (unsigned)test && size == length && !memcmp(incoming, payload, size));
    }
    CHECK(!murmur_ice_voice_state(host, 3));
    deadline = now_ms() + 4000; got = 0;
    do {
        CHECK(murmur_ice_step(client, now_ms()) == 4 && murmur_ice_step(host, now_ms()) == 4);
        got = murmur_ice_receive(client, &kind, &room, incoming, sizeof(incoming), &size);
        if (got) break;
        pause_ms();
    } while (now_ms() < deadline);
    CHECK(got == 1 && kind == MURMUR_CONTROL && size == 1 && incoming[0] == 3);
    {
        murmur_voice *encoder = murmur_voice_create(0), *decoder = murmur_voice_create(0);
        murmur_voice_stats stats;
        int16_t pcm[MURMUR_VOICE_SAMPLES] = {0};
        int encoded;
        CHECK(encoder && decoder);
        murmur_voice_capture(encoder, pcm, MURMUR_VOICE_SAMPLES);
        encoded = murmur_voice_encode(encoder, payload); CHECK(encoded > 4);
        CHECK(!murmur_ice_send_audio(host, payload, (size_t)encoded));
        deadline = now_ms() + 4000; got = 0;
        do {
            CHECK(murmur_ice_step(client, now_ms()) == 4 && murmur_ice_step(host, now_ms()) == 4);
            got = murmur_ice_receive_audio(client, incoming, sizeof(incoming), &size);
            if (got) break;
            pause_ms();
        } while (now_ms() < deadline);
        CHECK(got == 1 && size == (size_t)encoded && !memcmp(payload, incoming, size));
        CHECK(murmur_voice_decode(decoder, incoming, size) == 1);
        murmur_voice_pump(decoder, 0); murmur_voice_pump(decoder, 40);
        murmur_voice_get_stats(decoder, &stats); CHECK(stats.decoded == 1);
        murmur_voice_free(encoder); murmur_voice_free(decoder);
    }
    {
        unsigned i;
        memset(payload,0x5a,MURMUR_SCREEN_PACKET);
        CHECK(murmur_ice_send_screen(host,payload,MURMUR_SCREEN_PACKET+1)==-1);
        CHECK(murmur_ice_send_screen(host,payload,0)==-1);
        for(i=0;i<16;++i) CHECK(!murmur_ice_send_screen(host,payload,MURMUR_SCREEN_PACKET));
        CHECK(murmur_ice_send_screen(host,payload,1)==1);
        deadline=now_ms()+4000; got=0;
        do {
            CHECK(murmur_ice_step(client,now_ms())==4 && murmur_ice_step(host,now_ms())==4);
            if(murmur_ice_receive_screen(client,incoming,sizeof(incoming),&size)==1) {
                CHECK(size==MURMUR_SCREEN_PACKET && !memcmp(payload,incoming,size)); ++got;
            }
            if(got==16) break;
            pause_ms();
        } while(now_ms()<deadline);
        CHECK(got==16);
    }
    murmur_ice_free(client); murmur_ice_free(host);
    if (server) juice_server_destroy(server);
    puts(relay ? "Authenticated offer/answer, encrypted reliable text and Opus through local TURN passed." :
        "Authenticated offer/answer, encrypted reliable text and Opus through direct ICE passed.");
    return 0;
}
static int worker_client(void) {
    char offer[MURMUR_ICE_TOKEN], answer[MURMUR_ICE_TOKEN];
    murmur_ice_config config = {0};
    murmur_ice *peer;
    uint64_t deadline;
    int sent = 0, requested = 0, got = 0;
    uint8_t bytes[128]; size_t size; unsigned room; murmur_kind kind;
    CHECK(fgets(offer, sizeof(offer), stdin));
    strcpy(config.bind_address, "127.0.0.1");
    peer = murmur_ice_create(0, &config, offer); CHECK(peer);
    CHECK(token(peer, answer) == 1);
    /* Private anonymous pipe to the parent test, never a log or a public service. */
    fputs(answer, stdout); fputc('\n', stdout); fflush(stdout);
    memset(offer, 0, sizeof(offer)); memset(answer, 0, sizeof(answer));
    deadline = now_ms() + 8000;
    do {
        int state = murmur_ice_step(peer, now_ms()); CHECK(state >= 0);
        if (state == 4) {
            if (!requested && !murmur_ice_voice_state(peer, 1)) requested = 1;
            if (requested && !sent && !murmur_ice_send(peer, 2, (const uint8_t *)"hello over ICE", 14)) sent = 1;
            got = murmur_ice_receive(peer, &kind, &room, bytes, sizeof(bytes), &size);
            if (got == 1) break;
        }
        pause_ms();
    } while (now_ms() < deadline);
    CHECK(got == 1 && kind == MURMUR_TEXT && room == 2 && size == 7 && !memcmp(bytes, "ICE ack", 7));
    murmur_ice_free(peer);
    return 0;
}
int main(int argc, char **argv) {
    juice_set_log_level(JUICE_LOG_LEVEL_NONE);
    mbedtls_timing_get_timer(&timer, 1);
    if (argc == 2 && !strcmp(argv[1], "--worker-client")) return worker_client();
    CHECK(!exchange(0));
    CHECK(!exchange(1));
    return 0;
}
