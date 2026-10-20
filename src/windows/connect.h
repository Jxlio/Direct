#ifndef MURMUR_CONNECT_H
#define MURMUR_CONNECT_H
#include <windows.h>
void connection_show(HWND parent);
void connection_refresh(void);
int connection_translate(MSG *message);
void connection_auto_show(HWND parent);
void connection_auto_refresh(void);
int connection_auto_translate(MSG *message);
#endif
