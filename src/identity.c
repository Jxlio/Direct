#include "murmur/identity.h"
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <string.h>

#define CURVE_BITS 256u
#define SIGN_ALG PSA_ALG_ECDSA(PSA_ALG_SHA_256)

static void attributes(psa_key_attributes_t *a, int pair, psa_key_usage_t usage) {
    *a = psa_key_attributes_init();
    psa_set_key_type(a, pair ? PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)
                             : PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(a, CURVE_BITS);
    psa_set_key_usage_flags(a, usage);
    psa_set_key_algorithm(a, SIGN_ALG);
}
static int import_secret(const uint8_t secret[MURMUR_ID_SECRET], psa_key_usage_t usage, psa_key_id_t *key) {
    psa_key_attributes_t a;
    attributes(&a, 1, usage);
    return psa_import_key(&a, secret, MURMUR_ID_SECRET, key) == PSA_SUCCESS ? 0 : -1;
}
int murmur_identity_generate(uint8_t secret[MURMUR_ID_SECRET], uint8_t pub[MURMUR_ID_PUBLIC]) {
    psa_key_attributes_t a;
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = -1;
    if (!secret || !pub || psa_crypto_init() != PSA_SUCCESS) return -1;
    attributes(&a, 1, PSA_KEY_USAGE_EXPORT | PSA_KEY_USAGE_SIGN_MESSAGE);
    if (psa_generate_key(&a, &key) != PSA_SUCCESS) return -1;
    if (psa_export_key(key, secret, MURMUR_ID_SECRET, &length) == PSA_SUCCESS && length == MURMUR_ID_SECRET &&
        psa_export_public_key(key, pub, MURMUR_ID_PUBLIC, &length) == PSA_SUCCESS && length == MURMUR_ID_PUBLIC)
        result = 0;
    psa_destroy_key(key);
    if (result) { mbedtls_platform_zeroize(secret, MURMUR_ID_SECRET); memset(pub, 0, MURMUR_ID_PUBLIC); }
    return result;
}
int murmur_identity_public(const uint8_t secret[MURMUR_ID_SECRET], uint8_t pub[MURMUR_ID_PUBLIC]) {
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = -1;
    if (!secret || !pub || psa_crypto_init() != PSA_SUCCESS || import_secret(secret, PSA_KEY_USAGE_SIGN_MESSAGE, &key)) return -1;
    if (psa_export_public_key(key, pub, MURMUR_ID_PUBLIC, &length) == PSA_SUCCESS && length == MURMUR_ID_PUBLIC) result = 0;
    psa_destroy_key(key);
    return result;
}
int murmur_identity_sign(const uint8_t secret[MURMUR_ID_SECRET], const uint8_t *message, size_t size,
                         uint8_t signature[MURMUR_ID_SIGNATURE]) {
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = -1;
    if (!secret || !signature || (!message && size) || psa_crypto_init() != PSA_SUCCESS ||
        import_secret(secret, PSA_KEY_USAGE_SIGN_MESSAGE, &key)) return -1;
    if (psa_sign_message(key, SIGN_ALG, message, size, signature, MURMUR_ID_SIGNATURE, &length) == PSA_SUCCESS &&
        length == MURMUR_ID_SIGNATURE) result = 0;
    psa_destroy_key(key);
    return result;
}
int murmur_identity_verify(const uint8_t pub[MURMUR_ID_PUBLIC], const uint8_t *message, size_t size,
                           const uint8_t signature[MURMUR_ID_SIGNATURE]) {
    psa_key_attributes_t a;
    psa_key_id_t key = 0;
    int result = -1;
    if (!pub || !signature || (!message && size) || pub[0] != 4 || psa_crypto_init() != PSA_SUCCESS) return -1;
    attributes(&a, 0, PSA_KEY_USAGE_VERIFY_MESSAGE);
    if (psa_import_key(&a, pub, MURMUR_ID_PUBLIC, &key) != PSA_SUCCESS) return -1;
    if (psa_verify_message(key, SIGN_ALG, message, size, signature, MURMUR_ID_SIGNATURE) == PSA_SUCCESS) result = 0;
    psa_destroy_key(key);
    return result;
}
/* The encryption scalar is HMAC-SHA256(identity secret, label || counter). A value outside
 * 1..n-1 is rejected by key import, and the next counter is tried (almost never needed). */
static int import_enc_key(const uint8_t secret[MURMUR_ID_SECRET], psa_key_id_t *key) {
    static const char label[] = "murmur/v2/enc|";
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t input[sizeof(label)], scalar[32];
    unsigned counter;
    psa_key_attributes_t a = psa_key_attributes_init();
    memcpy(input, label, sizeof(label) - 1);
    psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&a, CURVE_BITS);
    psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&a, PSA_ALG_ECDH);
    for (counter = 0; counter < 16; ++counter) {
        input[sizeof(label) - 1] = (uint8_t)counter;
        if (mbedtls_md_hmac(md, secret, MURMUR_ID_SECRET, input, sizeof(label), scalar)) break;
        if (psa_import_key(&a, scalar, sizeof(scalar), key) == PSA_SUCCESS) {
            mbedtls_platform_zeroize(scalar, sizeof(scalar));
            return 0;
        }
    }
    mbedtls_platform_zeroize(scalar, sizeof(scalar));
    return -1;
}
int murmur_identity_enc_public(const uint8_t secret[MURMUR_ID_SECRET], uint8_t pub[MURMUR_ID_PUBLIC]) {
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = -1;
    if (!secret || !pub || psa_crypto_init() != PSA_SUCCESS || import_enc_key(secret, &key)) return -1;
    if (psa_export_public_key(key, pub, MURMUR_ID_PUBLIC, &length) == PSA_SUCCESS && length == MURMUR_ID_PUBLIC) result = 0;
    psa_destroy_key(key);
    return result;
}
int murmur_identity_ecdh(const uint8_t secret[MURMUR_ID_SECRET], const uint8_t peer[MURMUR_ID_PUBLIC], uint8_t shared[32]) {
    psa_key_id_t key = 0;
    size_t length = 0;
    int result = -1;
    if (!secret || !peer || !shared || peer[0] != 4 || psa_crypto_init() != PSA_SUCCESS || import_enc_key(secret, &key)) return -1;
    if (psa_raw_key_agreement(PSA_ALG_ECDH, key, peer, MURMUR_ID_PUBLIC, shared, 32, &length) == PSA_SUCCESS && length == 32) result = 0;
    else mbedtls_platform_zeroize(shared, 32);
    psa_destroy_key(key);
    return result;
}
int murmur_identity_fingerprint(const uint8_t pub[MURMUR_ID_PUBLIC], char out[MURMUR_ID_FINGERPRINT]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t digest[32];
    size_t length = 0, i;
    if (!pub || !out || psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, pub, MURMUR_ID_PUBLIC, digest, sizeof(digest), &length) != PSA_SUCCESS || length != 32) return -1;
    for (i = 0; i < 32; ++i) { out[i * 2] = digits[digest[i] >> 4]; out[i * 2 + 1] = digits[digest[i] & 15]; }
    out[64] = 0;
    return 0;
}
