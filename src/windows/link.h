#ifndef MURMUR_WINDOWS_LINK_H
#define MURMUR_WINDOWS_LINK_H
#include <windows.h>
#include "murmur/peer.h"
#include "murmur/ice.h"
#define WM_MURMUR_LINK (WM_APP + 10)
typedef struct {
    unsigned room;
    size_t size;
    uint8_t text[MURMUR_CHAT_LIMIT];
} link_message;
void link_init(HWND window);
int link_start(int server, const char *host, const char *port, const uint8_t key[32]);
void link_stop(void);
int link_send(unsigned room, const uint8_t *text, size_t size);
int link_receive(link_message *message);
/* 0 offline, 1 waiting, 2 connected, 3 failed, 4 stopping. */
int link_state(void);
int link_error(void);
int link_busy(void);
/* Explicit local opt-in only. 0 off, 1 awaiting peer, 2 securing UDP,
 * 3 live, 4 muted, 5 audio/network error. */
int link_voice_toggle(void);
void link_voice_mute_toggle(void);
int link_voice_state(void);
int link_voice_requested(void);
unsigned link_voice_received_packets(void);
uint64_t link_voice_decoded_frames(void);
int link_ice_start(int offerer, const murmur_ice_config *settings, const char *offer);
int link_ice_answer(const char *answer);
int link_ice_token(char *token, size_t capacity);
int link_ice_mode(void);
int link_ice_phase(void);
int link_ice_relayed(void);
#endif
