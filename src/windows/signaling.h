#ifndef DIRECT_SIGNALING_H
#define DIRECT_SIGNALING_H
#include <windows.h>
#include "friends.h"
#include "murmur/rendezvous.h"
#define WM_DIRECT_SIGNAL (WM_APP+30)
typedef struct { size_t size; uint8_t bytes[6097]; } signal_frame;
int signal_profile_load(direct_service *profile);
int signal_profile_save(const direct_service *profile);
void signal_init(HWND owner);
int signal_start(const direct_service *profile, const friend_book *book);
void signal_subscribe(const friend_book *book);
int signal_send(const uint8_t route[32],const uint8_t *packet,size_t size);
int signal_receive(signal_frame *frame);
int signal_state(void);
void signal_stop(void);
#endif
