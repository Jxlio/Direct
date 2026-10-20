#ifndef MURMUR_FRIENDS_H
#define MURMUR_FRIENDS_H
#include <windows.h>
#include <stdint.h>
typedef struct { WCHAR name[64]; uint8_t secret[32]; DWORD offerer; } saved_friend;
typedef struct { DWORD version, count; saved_friend items[16]; } friend_book;
int friend_book_load(const WCHAR *path, friend_book *book);
int friend_book_save(const WCHAR *path, const friend_book *book);
void friends_init(HWND parent);
void friends_show(HWND parent);
int friends_translate(MSG *message);
void friends_close(void);
#endif
