#include "murmur/friend.h"
#include "murmur/peer.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    uint8_t secret[32], other[32], session[32], restored[32], packet[MURMUR_FRIEND_PACKET], original[MURMUR_FRIEND_PACKET];
    char code[MURMUR_FRIEND_CODE], token[6000], large[6000];
    unsigned role; size_t size, i;
    CHECK(!murmur_random_key(secret) && !murmur_random_key(other) && !murmur_random_key(session));
    CHECK(!murmur_friend_code(secret, code) && !murmur_friend_parse(code, restored));
    CHECK(!memcmp(secret, restored, 32));
    code[20] = 'x'; CHECK(murmur_friend_parse(code, restored) == -1);
    CHECK(murmur_friend_parse("mfriend1", restored) == -1);
    memset(large, 'a', sizeof(large)-1); large[sizeof(large)-1] = 0;
    size = sizeof(packet); CHECK(!murmur_friend_seal(secret, 0, session, large, packet, &size));
    CHECK(!murmur_friend_open(secret, packet, size, &role, restored, token));
    CHECK(!role && !memcmp(session, restored, 32) && !strcmp(token, large));
    CHECK(murmur_friend_open(other, packet, size, &role, restored, token) == -1);
    memcpy(original, packet, size);
    /* Every envelope field, ciphertext and tag byte is authenticated. */
    for (i = 0; i < size; ++i) {
        packet[i] ^= 1;
        CHECK(murmur_friend_open(secret, packet, size, &role, restored, token) == -1);
        packet[i] = original[i];
    }
    CHECK(murmur_friend_open(secret, packet, size-1, &role, restored, token) == -1);
    size = sizeof(packet); CHECK(!murmur_friend_seal(secret, 1, session, "response", packet, &size));
    CHECK(!murmur_friend_open(secret, packet, size, &role, restored, token) && role == 1 && !strcmp(token, "response"));
    size = 20; CHECK(murmur_friend_seal(secret, 0, session, "offer", packet, &size) == -1);
    puts("Friend codes and authenticated encrypted discovery passed."); return 0;
}
