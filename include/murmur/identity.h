#ifndef MURMUR_IDENTITY_H
#define MURMUR_IDENTITY_H
#include <stddef.h>
#include <stdint.h>

/* User identity: an ECDSA P-256 key pair. The public key (uncompressed point)
 * is the account identifier; its SHA-256 digest is the short fingerprint.
 * Signatures are raw r||s, 64 bytes, over SHA-256 of the message.
 * The secret is a 32-byte scalar. Callers own storage and must wipe it. */
#define MURMUR_ID_SECRET 32u
#define MURMUR_ID_PUBLIC 65u
#define MURMUR_ID_SIGNATURE 64u
#define MURMUR_ID_FINGERPRINT 65u /* 64 lowercase hex characters plus NUL */

int murmur_identity_generate(uint8_t secret[MURMUR_ID_SECRET], uint8_t pub[MURMUR_ID_PUBLIC]);
int murmur_identity_public(const uint8_t secret[MURMUR_ID_SECRET], uint8_t pub[MURMUR_ID_PUBLIC]);
int murmur_identity_sign(const uint8_t secret[MURMUR_ID_SECRET], const uint8_t *message,
                         size_t size, uint8_t signature[MURMUR_ID_SIGNATURE]);
int murmur_identity_verify(const uint8_t pub[MURMUR_ID_PUBLIC], const uint8_t *message,
                           size_t size, const uint8_t signature[MURMUR_ID_SIGNATURE]);
int murmur_identity_fingerprint(const uint8_t pub[MURMUR_ID_PUBLIC], char out[MURMUR_ID_FINGERPRINT]);

/* Encryption key: a second P-256 key pair derived from the identity secret (HMAC-SHA256 with a
 * fixed label), so it is recoverable from the identity alone and never shares the signing key.
 * Its public half is published signed by the identity. ECDH yields a 32-byte shared secret that
 * callers must run through a KDF before use. */
int murmur_identity_enc_public(const uint8_t secret[MURMUR_ID_SECRET], uint8_t pub[MURMUR_ID_PUBLIC]);
int murmur_identity_ecdh(const uint8_t secret[MURMUR_ID_SECRET], const uint8_t peer[MURMUR_ID_PUBLIC],
                         uint8_t shared[32]);
#endif
