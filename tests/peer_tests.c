#include "murmur/peer.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "murmur/voice.h"
#include <mbedtls/timing.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static int exchange(int bad_key) {
    uint8_t key[32], other[32], received[128];
    const uint8_t message[] = "Bonjour \xF0\x9F\x91\x8B, message chiffre!";
    murmur_peer *host, *client;
    murmur_peer_state hs = MURMUR_PEER_WAITING, cs = MURMUR_PEER_WAITING;
    time_t deadline = time(NULL) + 8;
    unsigned room;
    size_t size;
    memset(key, 0x42, 32); memset(other, bad_key ? 0x43 : 0x42, 32);
    host = murmur_peer_open(1, NULL, "49183", key);
    CHECK(host);
    client = murmur_peer_open(0, "127.0.0.1", "49183", other);
    CHECK(client);
    while (time(NULL) < deadline) {
        hs = murmur_peer_step(host, 1); cs = murmur_peer_step(client, 1);
        if (hs == MURMUR_PEER_FAILED || cs == MURMUR_PEER_FAILED ||
            (hs == MURMUR_PEER_CONNECTED && cs == MURMUR_PEER_CONNECTED)) break;
    }
    if (bad_key) {
        CHECK(hs != MURMUR_PEER_CONNECTED && cs != MURMUR_PEER_CONNECTED);
        CHECK(hs == MURMUR_PEER_FAILED || cs == MURMUR_PEER_FAILED);
    } else {
        CHECK(hs == MURMUR_PEER_CONNECTED && cs == MURMUR_PEER_CONNECTED);
        CHECK(murmur_peer_send(client, 2, message, sizeof(message) - 1) == 0);
        CHECK(murmur_peer_send(client, 2, message, sizeof(message) - 1) == -1);
        do {
            murmur_peer_step(client, 1); murmur_peer_step(host, 1);
            if (murmur_peer_receive(host, &room, received, sizeof(received), &size) == 1) break;
        } while (time(NULL) < deadline);
        CHECK(size == sizeof(message) - 1 && room == 2 && !memcmp(received, message, size));
        CHECK(murmur_peer_send(host, 0, received, size) == 0);
        do {
            murmur_peer_step(host, 1); murmur_peer_step(client, 1);
            if (murmur_peer_receive(client, &room, received, sizeof(received), &size) == 1) break;
        } while (time(NULL) < deadline);
        CHECK(size == sizeof(message) - 1 && room == 0 && !memcmp(received, message, size));
        CHECK(murmur_peer_send(host, 3, message, 1) == -1);
        CHECK(murmur_peer_send(host, 0, message, MURMUR_CHAT_LIMIT + 1) == -1);
    }
    murmur_peer_free(client); murmur_peer_free(host);
    return 0;
}
static int worker_client(void) {
    uint8_t key[32], received[64];
    murmur_peer *peer;
    time_t deadline = time(NULL) + 8;
    unsigned room = 0;
    size_t size = 0;
    int sent = 0, got = 0;
    memset(key, 0x42, sizeof(key));
    peer = murmur_peer_open(0, "127.0.0.1", "49184", key);
    CHECK(peer);
    while (time(NULL) < deadline) {
        murmur_peer_state state = murmur_peer_step(peer, 5);
        CHECK(state != MURMUR_PEER_FAILED && state != MURMUR_PEER_CLOSED);
        if (state != MURMUR_PEER_CONNECTED) continue;
        if (!sent) { CHECK(murmur_peer_send(peer, 1, (const uint8_t *)"worker ping", 11) == 0); sent = 1; }
        got = murmur_peer_receive(peer, &room, received, sizeof(received), &size);
        if (got == 1) break;
    }
    CHECK(got == 1 && room == 1 && size == 11 && !memcmp(received, "worker pong", 11));
    murmur_peer_free(peer);
    return 0;
}
static int media_exchange(int bad_key) {
    uint8_t session[32], key[32], other[32], received[64];
    const uint8_t packet[] = {0, 0, 0, 1, 12, 34, 56};
    murmur_peer *host, *client;
    murmur_peer_state hs = MURMUR_PEER_WAITING, cs = MURMUR_PEER_WAITING;
    time_t deadline = time(NULL) + 8;
    murmur_kind kind = MURMUR_TEXT;
    unsigned room = 0;
    size_t size = 0;
    int got = 0;
    memset(session, 0x42, sizeof(session));
    CHECK(murmur_media_key(session, key) == 0 && memcmp(session, key, 32));
    memcpy(other, key, 32);
    if (bad_key) other[0] ^= 1;
    host = murmur_media_open(1, NULL, "49185", key); CHECK(host);
    client = murmur_media_open(0, "127.0.0.1", "49185", other); CHECK(client);
    while (time(NULL) < deadline) {
        hs = murmur_peer_step(host, 1); cs = murmur_peer_step(client, 1);
        if (hs == MURMUR_PEER_FAILED || cs == MURMUR_PEER_FAILED ||
            (hs == MURMUR_PEER_CONNECTED && cs == MURMUR_PEER_CONNECTED)) break;
    }
    if (bad_key) {
        CHECK(hs != MURMUR_PEER_CONNECTED && cs != MURMUR_PEER_CONNECTED);
        CHECK(hs == MURMUR_PEER_FAILED || cs == MURMUR_PEER_FAILED);
    } else {
        CHECK(hs == MURMUR_PEER_CONNECTED && cs == MURMUR_PEER_CONNECTED);
        CHECK(murmur_peer_send(client, 0, packet, sizeof(packet)) == -1);
        CHECK(murmur_peer_send_audio(client, packet, sizeof(packet)) == 0);
        do {
            murmur_peer_step(client, 1); murmur_peer_step(host, 1);
            got = murmur_peer_receive_packet(host, &kind, &room, received, sizeof(received), &size);
            if (got == 1) break;
        } while (time(NULL) < deadline);
        CHECK(got == 1 && kind == MURMUR_AUDIO && size == sizeof(packet) && !memcmp(received, packet, size));
    }
    murmur_peer_free(host); murmur_peer_free(client);
    return 0;
}
static int voice_worker_client(void) {
    struct mbedtls_timing_hr_time timer = {0};
    uint8_t key[32], media_key[32], buffer[MURMUR_CHAT_LIMIT];
    int16_t pcm[MURMUR_VOICE_SAMPLES];
    murmur_peer *peer, *media = NULL;
    murmur_voice *voice = murmur_voice_create(0);
    time_t deadline = time(NULL) + 10;
    int request = 0, ping = 0, sent_audio = 0, got_audio = 0, got_mute = 0, leaving = 0;
    unsigned remote_flags = 0, room;
    size_t size;
    murmur_kind kind;
    int i;
    memset(key, 0x42, sizeof(key));
    for (i = 0; i < MURMUR_VOICE_SAMPLES; ++i) pcm[i] = (i % 64) < 32 ? 4000 : -4000;
    CHECK(voice && !murmur_media_key(key, media_key));
    mbedtls_timing_get_timer(&timer, 1);
    peer = murmur_peer_open(0, "127.0.0.1", "49186", key); CHECK(peer);
    while (time(NULL) < deadline) {
        murmur_peer_state state = murmur_peer_step(peer, 2);
        CHECK(state != MURMUR_PEER_FAILED && state != MURMUR_PEER_CLOSED);
        if (state != MURMUR_PEER_CONNECTED) continue;
        if (!request && !murmur_peer_send_voice_state(peer, 1)) request = 1;
        if (request && !ping && !murmur_peer_send(peer, 0, (const uint8_t *)"voice request", 13)) ping = 1;
        if (murmur_peer_receive_packet(peer, &kind, &room, buffer, sizeof(buffer), &size) == 1 && kind == MURMUR_CONTROL) {
            remote_flags = buffer[0];
            if (remote_flags == 3) got_mute = 1;
            if (!remote_flags && got_mute) leaving = 1;
        }
        if (remote_flags && !media) { media = murmur_media_open(0, "127.0.0.1", "49187", media_key); CHECK(media); }
        if (media && !leaving) {
            state = murmur_peer_step(media, 0);
            CHECK(state != MURMUR_PEER_FAILED && state != MURMUR_PEER_CLOSED);
            if (state == MURMUR_PEER_CONNECTED) {
                if (sent_audio < 3) {
                    int encoded;
                    murmur_voice_capture(voice, pcm, MURMUR_VOICE_SAMPLES);
                    encoded = murmur_voice_encode(voice, buffer);
                    CHECK(encoded > 4);
                    if (!murmur_peer_send_audio(media, buffer, (size_t)encoded)) ++sent_audio;
                }
                if (murmur_peer_receive_packet(media, &kind, &room, buffer, sizeof(buffer), &size) == 1)
                    murmur_voice_decode(voice, buffer, size);
                {
                    murmur_voice_stats stats;
                    murmur_voice_pump(voice, mbedtls_timing_get_timer(&timer, 0));
                    murmur_voice_get_stats(voice, &stats);
                    got_audio = stats.decoded > 0;
                }
            }
        }
        if (leaving && !murmur_peer_send(peer, 0, (const uint8_t *)"voice left", 10)) {
            int n;
            for (n = 0; n < 5; ++n) murmur_peer_step(peer, 2);
            break;
        }
    }
    CHECK(got_audio && got_mute && leaving && sent_audio > 0);
    murmur_voice_free(voice); murmur_peer_free(media); murmur_peer_free(peer);
    return 0;
}
int main(int argc, char **argv) {
    char invite[128], host[16], port[6];
    uint8_t key[32], decoded[32];
    if (argc == 2 && !strcmp(argv[1], "--worker-client")) return worker_client();
    if (argc == 2 && !strcmp(argv[1], "--voice-worker-client")) return voice_worker_client();
    CHECK(murmur_random_key(key) == 0);
    CHECK(murmur_invite_format(invite, sizeof(invite), "127.0.0.1", "49183", key) == 0);
    CHECK(murmur_invite_parse(invite, host, port, decoded) == 0);
    CHECK(!strcmp(host, "127.0.0.1") && !strcmp(port, "49183") && !memcmp(key, decoded, 32));
    CHECK(murmur_invite_parse("murmur1|localhost|0|bad", host, port, decoded) == -1);
    CHECK(murmur_invite_format(invite, sizeof(invite), "127.0.0.01", "49183", key) == -1);
    CHECK(murmur_invite_format(invite, sizeof(invite), "256.0.0.1", "49183", key) == -1);
    CHECK(murmur_invite_format(invite, sizeof(invite), "127.0.0.1", "65536", key) == -1);
    CHECK(murmur_invite_format(invite, sizeof(invite), "127.0.0.1", "0", key) == -1);
    CHECK(murmur_invite_format(invite, sizeof(invite), "127.0.0.1", "49183", key) == 0);
    invite[strlen(invite) - 1] = 'g';
    CHECK(murmur_invite_parse(invite, host, port, decoded) == -1);
    CHECK(murmur_invite_format(invite, 4, "127.0.0.1", "49183", key) == -1);
    CHECK(exchange(0) == 0);
    CHECK(exchange(1) == 0);
    CHECK(media_exchange(0) == 0);
    CHECK(media_exchange(1) == 0);
    puts("TLS exchange and wrong-invitation rejection passed.");
    return 0;
}
