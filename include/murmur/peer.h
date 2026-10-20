#ifndef MURMUR_PEER_H
#define MURMUR_PEER_H
#include <stddef.h>
#include <stdint.h>
#include "murmur/frame.h"
#define MURMUR_KEY_SIZE 32
#define MURMUR_CHAT_LIMIT 8192
typedef struct murmur_peer murmur_peer;
typedef enum {
    MURMUR_PEER_FAILED = -1, MURMUR_PEER_WAITING = 0,
    MURMUR_PEER_CONNECTED = 1, MURMUR_PEER_CLOSED = 2
} murmur_peer_state;
/* One owning thread; never call concurrently on the same peer.
 * Server binds all IPv4 interfaces. Client host must be numeric IPv4.
 * Key is an invitation secret, not a permanent user identity. */
int murmur_random_key(uint8_t key[MURMUR_KEY_SIZE]);
murmur_peer *murmur_peer_open(int server, const char *host, const char *port,
                              const uint8_t key[MURMUR_KEY_SIZE]);
/* Independent UDP/DTLS channel. Session invitation key is domain-separated by HKDF. */
murmur_peer *murmur_media_open(int server, const char *host, const char *port,
                              const uint8_t key[MURMUR_KEY_SIZE]);
int murmur_media_key(const uint8_t session[32], uint8_t media[32]);
int murmur_peer_send_voice_state(murmur_peer *peer, unsigned flags);
int murmur_peer_send_audio(murmur_peer *peer, const uint8_t *packet, size_t size);
int murmur_peer_receive_packet(murmur_peer *peer, murmur_kind *kind, unsigned *room,
                               uint8_t *data, size_t capacity, size_t *size);
murmur_peer_state murmur_peer_step(murmur_peer *peer, unsigned timeout_ms);
int murmur_peer_send(murmur_peer *peer, unsigned room, const uint8_t *utf8, size_t size);
/* 1 message, 0 no message, -1 insufficient capacity/invalid argument. */
int murmur_peer_receive(murmur_peer *peer, unsigned *room, uint8_t *utf8,
                        size_t capacity, size_t *size);
int murmur_peer_error(const murmur_peer *peer);
void murmur_peer_free(murmur_peer *peer);
/* Strict portable invitation: murmur1|numeric-ipv4|port|64 lowercase hex chars. */
int murmur_invite_format(char *out, size_t capacity, const char *host, const char *port,
                         const uint8_t key[MURMUR_KEY_SIZE]);
int murmur_invite_parse(const char *text, char host[16], char port[6],
                        uint8_t key[MURMUR_KEY_SIZE]);
#endif
