#ifndef MURMUR_FRIENDS_H
#define MURMUR_FRIENDS_H
#include <windows.h>
#include <stdint.h>
typedef struct { WCHAR name[64]; uint8_t secret[32]; DWORD offerer; } saved_friend;
typedef struct { DWORD version, count; saved_friend items[16]; } friend_book;
int friend_book_load(const WCHAR *path, friend_book *book);
int friend_book_save(const WCHAR *path, const friend_book *book);
void friends_init(HWND parent);
void friends_init_test(HWND parent,const WCHAR *peer);
void friends_connection_refresh(void);
void friends_internet_event(void);
const WCHAR *friends_peer_name(void);
int friends_auto_connecting(void);
int friends_peer_id(uint8_t route[32]);
int friends_translate(MSG *message);
void friends_close(void);
/* Main-window UI: no secondary window is required for these actions. */
unsigned friends_web_count(void);
const WCHAR *friends_web_name(unsigned index);
int friends_web_online(unsigned index);
const WCHAR *friends_web_notice(void);
int friends_web_internet_enabled(void);
int friends_web_create(const WCHAR *name, WCHAR code[512]);
int friends_web_add(const WCHAR *name, const WCHAR *code);
int friends_web_code(unsigned index, WCHAR code[512]);
void friends_web_connect(unsigned index);
void friends_web_disconnect(void);
int friends_web_forget(unsigned index);
#endif
