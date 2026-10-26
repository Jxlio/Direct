#ifndef DIRECT_SCREEN_H
#define DIRECT_SCREEN_H
#include <windows.h>
#include "murmur/ice.h"
void screen_init(HWND owner);
void screen_test_mode(int enabled);
int screen_record_game_begin(const wchar_t *directory, int sender);
void screen_record_tick(void);
int screen_record_count(void);
void screen_record_end(void);
void screen_menu(HWND owner);
/* Called only by the ICE owner. Never captures or runs a codec. */
void screen_pump(murmur_ice *peer, int connected);
void screen_disconnect(void);
void screen_shutdown(void);
int screen_sharing(void);
int screen_remote(void);
#ifdef DIRECT_SCREEN_TEST
int screen_codec_test(void);
#endif
#endif
