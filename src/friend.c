#include "murmur/friend.h"
#include "murmur/peer.h"
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/platform_util.h>
#include <string.h>
int murmur_friend_code(const uint8_t secret[32], char out[MURMUR_FRIEND_CODE]) {
    static const char hex[] = "0123456789abcdef";
    size_t i;
    if (!secret || !out) return -1;
    memcpy(out, "mfriend1", 8);
    for (i = 0; i < 32; ++i) { out[8+i*2] = hex[secret[i] >> 4]; out[9+i*2] = hex[secret[i]&15]; }
    out[72] = 0; return 0;
}
int murmur_friend_parse(const char *code, uint8_t secret[32]) {
    uint8_t result[32]; size_t i;
    if (!code || !secret || strlen(code) != 72 || memcmp(code, "mfriend1", 8)) return -1;
    for (i = 0; i < 64; ++i) {
        unsigned c = (unsigned char)code[8+i], n;
        if (c >= '0' && c <= '9') n = c-'0';
        else if (c >= 'a' && c <= 'f') n = c-'a'+10;
        else { mbedtls_platform_zeroize(result, sizeof(result)); return -1; }
        if (!(i&1)) result[i/2] = (uint8_t)(n<<4); else result[i/2] |= (uint8_t)n;
    }
    memcpy(secret, result, 32); mbedtls_platform_zeroize(result, sizeof(result)); return 0;
}
static int setup(mbedtls_gcm_context *ctx, const uint8_t secret[32]) {
    uint8_t key[32];
    static const unsigned char domain[] = "murmur/friend/lan/v1";
    int r = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), NULL, 0, secret, 32,
                         domain, sizeof(domain)-1, key, sizeof(key));
    if (!r) r = mbedtls_gcm_setkey(ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
    mbedtls_platform_zeroize(key, sizeof(key)); return r;
}
int murmur_friend_seal(const uint8_t secret[32], unsigned role, const uint8_t session[32],
                       const char *token, uint8_t *packet, size_t *size) {
    mbedtls_gcm_context ctx; uint8_t random[32]; size_t n; int r;
    if (!secret || !session || !token || !packet || !size || role > 1) return -1;
    n = strlen(token); if (!n || n >= 6000 || *size < n+65) return -1;
    if (murmur_random_key(random)) return -1;
    memcpy(packet, "MFL1", 4); packet[4] = (uint8_t)role;
    memcpy(packet+5, session, 32); memcpy(packet+37, random, 12);
    mbedtls_platform_zeroize(random, sizeof(random));
    mbedtls_gcm_init(&ctx); r = setup(&ctx, secret);
    if (!r) r = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, n, packet+37, 12,
        packet, 49, (const uint8_t *)token, packet+65, 16, packet+49);
    mbedtls_gcm_free(&ctx); if (r) return -1;
    *size = n+65; return 0;
}
int murmur_friend_open(const uint8_t secret[32], const uint8_t *packet, size_t size,
                       unsigned *role, uint8_t session[32], char token[6000]) {
    mbedtls_gcm_context ctx; int r;
    if (!secret || !packet || !role || !session || !token || size <= 65 || size >= 6065 ||
        memcmp(packet, "MFL1", 4) || packet[4] > 1) return -1;
    mbedtls_gcm_init(&ctx); r = setup(&ctx, secret);
    if (!r) r = mbedtls_gcm_auth_decrypt(&ctx, size-65, packet+37, 12, packet, 49,
        packet+49, 16, packet+65, (uint8_t *)token);
    mbedtls_gcm_free(&ctx);
    if (r || memchr(token, 0, size-65)) { mbedtls_platform_zeroize(token, 6000); return -1; }
    token[size-65] = 0; *role = packet[4]; memcpy(session, packet+5, 32); return 0;
}
