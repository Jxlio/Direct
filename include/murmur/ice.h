#ifndef MURMUR_ICE_H
#define MURMUR_ICE_H
#include "murmur/peer.h"
#define MURMUR_ICE_TOKEN 6000
typedef struct murmur_ice murmur_ice;
typedef struct {
    char stun_host[256], turn_host[256], username[128], password[128];
    char bind_address[64];
    uint16_t stun_port, turn_port;
    int require_relay;
    /* Optional lightweight notification from ICE's thread: only wake the owner,
       never call this API recursively from the notification. */
    void (*wakeup)(void *user);
    void *wakeup_user;
} murmur_ice_config;
/* One owning thread for every API below; ICE callbacks only enqueue datagrams.
 * offerer=1 creates a fresh secret; offerer=0 imports a private offer token.
 * No default public server: empty settings gather local candidates only. */
murmur_ice *murmur_ice_create(int offerer, const murmur_ice_config *config, const char *offer);
void murmur_ice_free(murmur_ice *peer);
/* Returns 1 token ready, 0 still gathering, -1 error/capacity too small.
 * Offer contains the session secret and addresses. Answer authenticates its SDP
 * with HMAC and does not contain the secret. Never publish or log tokens. */
int murmur_ice_local_token(murmur_ice *peer, char *token, size_t capacity);
int murmur_ice_accept_answer(murmur_ice *peer, const char *answer);
/* Monotonic clock. -1 failed, 0 gathering, 1 waiting answer, 2 ICE checks,
 * 3 DTLS handshake, 4 authenticated and ready. */
int murmur_ice_step(murmur_ice *peer, uint64_t now_ms);
int murmur_ice_error(const murmur_ice *peer);
int murmur_ice_relayed(const murmur_ice *peer);
unsigned murmur_ice_wait_ms(const murmur_ice *peer, uint64_t now_ms);
int murmur_ice_pending_message(const murmur_ice *peer);
int murmur_ice_send(murmur_ice *peer, unsigned room, const uint8_t *data, size_t size);
int murmur_ice_voice_state(murmur_ice *peer, unsigned flags);
int murmur_ice_receive(murmur_ice *peer, murmur_kind *kind, unsigned *room,
                        uint8_t *data, size_t capacity, size_t *size);
int murmur_ice_send_audio(murmur_ice *peer, const uint8_t *data, size_t size);
int murmur_ice_receive_audio(murmur_ice *peer, uint8_t *data, size_t capacity, size_t *size);
#endif
