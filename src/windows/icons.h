#ifndef DIRECT_ICONS_H
#define DIRECT_ICONS_H
#include <windows.h>
enum direct_icon { ICON_MESSAGE, ICON_USERS, ICON_PHONE, ICON_HANGUP, ICON_MIC,
    ICON_MIC_OFF, ICON_SEND, ICON_MONITOR, ICON_CALENDAR, ICON_GAME, ICON_COUNT };
void direct_icon_draw(HDC dc, UINT dpi, enum direct_icon kind, COLORREF color, int x, int y, int size);
void direct_icons_clear(void);
#endif
