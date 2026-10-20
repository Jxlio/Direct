#include "icons.h"
typedef struct { HICON handle; int pixels; } cached_icon;
static cached_icon cache[ICON_COUNT][4][2];
void direct_icon_draw(HDC dc, UINT dpi, enum direct_icon kind, COLORREF color, int x, int y, int size) {
    unsigned tint = color == RGB(255,255,255) ? 3 : color == RGB(58,103,244) ? 2 : color == RGB(24,34,52) ? 0 : 1;
    int pixels = MulDiv(size, (int)dpi, 96); cached_icon *entry;
    if (kind < 0 || kind >= ICON_COUNT || pixels < 1) return;
    entry = &cache[kind][tint][size > 22];
    if (entry->pixels != pixels || !entry->handle) {
        if (entry->handle) DestroyIcon(entry->handle);
        entry->handle = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(100 + (unsigned)kind*4 + tint), IMAGE_ICON, pixels, pixels, 0);
        entry->pixels = pixels;
    }
    if (entry->handle) DrawIconEx(dc, MulDiv(x, (int)dpi, 96), MulDiv(y, (int)dpi, 96), entry->handle, pixels, pixels, 0, NULL, DI_NORMAL);
}
void direct_icons_clear(void) {
    unsigned kind, tint, size;
    for (kind=0; kind<ICON_COUNT; ++kind) for (tint=0; tint<4; ++tint) for (size=0; size<2; ++size) {
        if (cache[kind][tint][size].handle) DestroyIcon(cache[kind][tint][size].handle);
        cache[kind][tint][size].handle = NULL; cache[kind][tint][size].pixels = 0;
    }
}
