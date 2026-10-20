#include "murmur/update.h"
#ifdef DIRECT_UPDATE_TEST_KEY
#include "update_test_key.h"
#else
#include "update_key.h"
#endif
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <psa/crypto.h>
#include <limits.h>
#include <string.h>
static int number(const char **cursor, unsigned *out) {
    const char *p = *cursor; unsigned n = 0;
    if (*p < '0' || *p > '9' || (*p == '0' && p[1] >= '0' && p[1] <= '9')) return -1;
    do { unsigned digit = (unsigned)(*p-'0'); if (n > (UINT_MAX-digit)/10) return -1; n = n*10+digit; ++p; } while (*p >= '0' && *p <= '9');
    *cursor = p; *out = n; return 0;
}
int direct_version_parse(const char *text, direct_version *v) {
    direct_version result; const char *p = text;
    if (!p || !v || number(&p, &result.major) || *p++ != '.' || number(&p, &result.minor) ||
        *p++ != '.' || number(&p, &result.patch) || *p) return -1;
    *v = result; return 0;
}
int direct_version_compare(direct_version a, direct_version b) {
    if (a.major != b.major) return a.major > b.major ? 1 : -1;
    if (a.minor != b.minor) return a.minor > b.minor ? 1 : -1;
    return a.patch == b.patch ? 0 : a.patch > b.patch ? 1 : -1;
}
static int parse(const uint8_t *bytes, size_t size, direct_update *out) {
    char text[256], *p, *version_end; const char *number_end; unsigned n; size_t i; direct_update update;
    if (!bytes || !out || !size || size >= sizeof(text) || memchr(bytes, 0, size)) return -1;
    memcpy(text, bytes, size); text[size] = 0;
    if (strncmp(text, "DIRECT-UPDATE-1\n", 16)) return -1;
    p = text+16; version_end = strchr(p, '\n'); if (!version_end) return -1;
    *version_end = 0; if (direct_version_parse(p, &update.version)) return -1;
    p = version_end+1; if (strncmp(p, "win-x64\n", 8)) return -1; p += 8;
    number_end=p;
    if (number(&number_end, &n) || !n || n > DIRECT_UPDATE_LIMIT || *number_end++ != '\n') return -1;
    p=(char *)number_end;
    if (strlen(p) != 65 || p[64] != '\n') return -1;
    update.size = n;
    for (i=0; i<64; ++i) {
        unsigned c = (unsigned char)p[i], digit;
        if (c >= '0' && c <= '9') digit = c-'0'; else if (c >= 'a' && c <= 'f') digit = c-'a'+10; else return -1;
        if (!(i&1)) update.sha256[i/2] = (uint8_t)(digit<<4); else update.sha256[i/2] |= (uint8_t)digit;
    }
    *out = update; return 0;
}
int direct_update_verify(const uint8_t *manifest, size_t size, const uint8_t *sig, size_t sig_size, direct_update *update) {
    mbedtls_pk_context key; uint8_t digest[32]; int result = -1;
    if (!manifest || !sig || !update || !size || size >= 256 || !sig_size || sig_size > 80 || psa_crypto_init() != PSA_SUCCESS) return -1;
    mbedtls_pk_init(&key);
    if (!mbedtls_sha256(manifest, size, digest, 0) &&
        !mbedtls_pk_parse_public_key(&key, (const uint8_t *)DIRECT_UPDATE_PUBLIC_KEY, sizeof(DIRECT_UPDATE_PUBLIC_KEY)) &&
        !mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, sig_size)) result = parse(manifest, size, update);
    mbedtls_pk_free(&key); return result;
}
int direct_update_hash(const direct_update *update, const uint8_t *data, size_t size) {
    uint8_t digest[32]; unsigned difference = 0; size_t i;
    if (!update || !data || size != update->size || size > DIRECT_UPDATE_LIMIT || mbedtls_sha256(data, size, digest, 0)) return -1;
    for (i=0; i<32; ++i) difference |= digest[i]^update->sha256[i];
    return difference ? -1 : 0;
}
