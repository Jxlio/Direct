#ifndef MURMUR_FRIEND_H
#define MURMUR_FRIEND_H
#include <stddef.h>
#include <stdint.h>
#define MURMUR_FRIEND_CODE 73
#define MURMUR_FRIEND_PACKET 6064
/* A separate random secret for each friendship. Never a public identifier. */
int murmur_friend_code(const uint8_t secret[32], char out[MURMUR_FRIEND_CODE]);
int murmur_friend_parse(const char *code, uint8_t secret[32]);
/* Authenticated session ID prevents replies from older attempts being accepted. */
int murmur_friend_seal(const uint8_t secret[32], unsigned role, const uint8_t session[32],
                       const char *token, uint8_t *packet, size_t *size);
int murmur_friend_open(const uint8_t secret[32], const uint8_t *packet, size_t size,
                       unsigned *role, uint8_t session[32], char token[6000]);
#endif
