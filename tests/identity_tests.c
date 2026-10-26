#include "murmur/identity.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(void) {
    uint8_t secret[MURMUR_ID_SECRET], other_secret[MURMUR_ID_SECRET], pub[MURMUR_ID_PUBLIC], other_pub[MURMUR_ID_PUBLIC], derived[MURMUR_ID_PUBLIC];
    uint8_t signature[MURMUR_ID_SIGNATURE], second[MURMUR_ID_SIGNATURE];
    const uint8_t message[] = "murmur/v2/auth|nonce";
    char fingerprint[MURMUR_ID_FINGERPRINT], again[MURMUR_ID_FINGERPRINT];
    CHECK(!murmur_identity_generate(secret, pub) && pub[0] == 4);
    CHECK(!murmur_identity_generate(other_secret, other_pub) && memcmp(pub, other_pub, sizeof(pub)));
    CHECK(!murmur_identity_public(secret, derived) && !memcmp(pub, derived, sizeof(pub)));
    CHECK(!murmur_identity_sign(secret, message, sizeof(message) - 1, signature));
    CHECK(!murmur_identity_verify(pub, message, sizeof(message) - 1, signature));
    /* Wrong key, altered message, altered signature and an all-zero signature are all rejected. */
    CHECK(murmur_identity_verify(other_pub, message, sizeof(message) - 1, signature));
    CHECK(murmur_identity_verify(pub, message, sizeof(message) - 2, signature));
    memcpy(second, signature, sizeof(second)); second[10] ^= 1;
    CHECK(murmur_identity_verify(pub, message, sizeof(message) - 1, second));
    memset(second, 0, sizeof(second));
    CHECK(murmur_identity_verify(pub, message, sizeof(message) - 1, second));
    /* An empty message is signable; a point that is not uncompressed is refused. */
    CHECK(!murmur_identity_sign(secret, NULL, 0, signature) && !murmur_identity_verify(pub, NULL, 0, signature));
    memcpy(derived, pub, sizeof(derived)); derived[0] = 2;
    CHECK(murmur_identity_verify(derived, message, sizeof(message) - 1, signature));
    CHECK(!murmur_identity_fingerprint(pub, fingerprint) && !murmur_identity_fingerprint(pub, again) && !strcmp(fingerprint, again) && strlen(fingerprint) == 64);
    CHECK(!murmur_identity_fingerprint(other_pub, again) && strcmp(fingerprint, again));
    /* Encryption key: derived, stable, distinct from the signing key, and ECDH is symmetric. */
    { uint8_t enc_a[MURMUR_ID_PUBLIC], enc_a2[MURMUR_ID_PUBLIC], enc_b[MURMUR_ID_PUBLIC], ab[32], ba[32], ac[32], bad[MURMUR_ID_PUBLIC];
      CHECK(!murmur_identity_enc_public(secret, enc_a) && !murmur_identity_enc_public(secret, enc_a2) && !memcmp(enc_a, enc_a2, sizeof(enc_a)));
      CHECK(enc_a[0] == 4 && memcmp(enc_a, pub, sizeof(enc_a)));
      CHECK(!murmur_identity_enc_public(other_secret, enc_b) && memcmp(enc_a, enc_b, sizeof(enc_a)));
      CHECK(!murmur_identity_ecdh(secret, enc_b, ab) && !murmur_identity_ecdh(other_secret, enc_a, ba) && !memcmp(ab, ba, 32));
      CHECK(!murmur_identity_ecdh(secret, enc_a, ac) && memcmp(ab, ac, 32));        /* self agreement differs from a pair */
      memcpy(bad, enc_b, sizeof(bad)); bad[10] ^= 1;                                  /* point off the curve */
      CHECK(murmur_identity_ecdh(secret, bad, ab));
      bad[0] = 2; CHECK(murmur_identity_ecdh(secret, bad, ab));
      CHECK(murmur_identity_enc_public(NULL, enc_a) && murmur_identity_ecdh(secret, NULL, ab)); }
    CHECK(murmur_identity_generate(NULL, pub) && murmur_identity_sign(NULL, message, 1, signature));
    puts("Identity generation, signing, verification, rejection and fingerprints passed.");
    return 0;
}
