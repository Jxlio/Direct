#ifndef DIRECT_RENDEZVOUS_H
#define DIRECT_RENDEZVOUS_H
#include <stdint.h>
#include <stddef.h>
#define DIRECT_INVITE_MAX 512
typedef struct { char origin[256], access[65]; } direct_service;
int direct_service_valid(const direct_service *service);
int direct_invite_create(const uint8_t secret[32], const direct_service *service, char out[DIRECT_INVITE_MAX]);
int direct_invite_parse(const char *code, uint8_t secret[32], direct_service *service);
int direct_friend_route(const uint8_t secret[32], uint8_t route[32]);
/* Separate encryption domain from the LAN protocol. */
int direct_signal_seal(const uint8_t secret[32], unsigned role, const uint8_t challenge[32], const char *token, uint8_t *packet, size_t *size);
int direct_signal_open(const uint8_t secret[32], const uint8_t *packet, size_t size, unsigned *role, uint8_t challenge[32], char token[6000]);
#endif
