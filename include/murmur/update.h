#ifndef DIRECT_UPDATE_H
#define DIRECT_UPDATE_H
#include <stddef.h>
#include <stdint.h>
#define DIRECT_UPDATE_LIMIT (32u * 1024u * 1024u)
typedef struct { unsigned major, minor, patch; } direct_version;
typedef struct { direct_version version; size_t size; uint8_t sha256[32]; } direct_update;
int direct_version_parse(const char *text, direct_version *version);
int direct_version_compare(direct_version a, direct_version b);
/* Signature authenticates the exact manifest bytes, including the final LF. */
int direct_update_verify(const uint8_t *manifest, size_t size, const uint8_t *signature,
                         size_t signature_size, direct_update *update);
int direct_update_hash(const direct_update *update, const uint8_t *data, size_t size);
#endif
