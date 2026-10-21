#ifndef DIRECT_SETTINGS_H
#define DIRECT_SETTINGS_H
#include <windows.h>
void settings_init(int sandbox);
void settings_show(HWND owner);
void settings_refresh(void);
int settings_translate(MSG *message);
void settings_close(void);
#endif
