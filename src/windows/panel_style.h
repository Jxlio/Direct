#ifndef DIRECT_PANEL_STYLE_H
#define DIRECT_PANEL_STYLE_H
#include <windows.h>
/* Shared native controls for the Internet and manual connection panels. */
static inline LRESULT direct_panel_color(UINT message, WPARAM wp, LPARAM lp) {
    HDC dc = (HDC)wp;
    WCHAR kind[16]; int input;
    GetClassNameW((HWND)lp, kind, 16);
    input = message == WM_CTLCOLOREDIT || !lstrcmpW(kind, L"Edit");
    SetTextColor(dc, RGB(24, 34, 52));
    SetBkColor(dc, input ? RGB(242, 245, 250) : RGB(255, 255, 255));
    SetDCBrushColor(dc, input ? RGB(242, 245, 250) : RGB(255, 255, 255));
    return (LRESULT)GetStockObject(DC_BRUSH);
}
static inline LRESULT direct_panel_button(DRAWITEMSTRUCT *item, HFONT font) {
    WCHAR text[128]; RECT r = item->rcItem;
    int primary = item->CtlID == 401 || item->CtlID == 403 || item->CtlID == 602;
    int disabled = (item->itemState & ODS_DISABLED) != 0;
    COLORREF color = primary ? RGB(58, 103, 244) : RGB(235, 240, 250);
    HBRUSH fill; HGDIOBJ old, pen, face;
    if (item->itemState & ODS_SELECTED) color = primary ? RGB(43, 83, 210) : RGB(221, 230, 247);
    if (disabled) color = RGB(242, 245, 250);
    FillRect(item->hDC, &r, (HBRUSH)GetStockObject(WHITE_BRUSH)); fill = CreateSolidBrush(color);
    old = SelectObject(item->hDC, fill); pen = SelectObject(item->hDC, GetStockObject(NULL_PEN));
    RoundRect(item->hDC, r.left, r.top, r.right, r.bottom, 12, 12);
    SelectObject(item->hDC, pen); SelectObject(item->hDC, old); DeleteObject(fill);
    GetWindowTextW(item->hwndItem, text, 128); face = SelectObject(item->hDC, font);
    SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, disabled ? RGB(133, 145, 164) : primary ? RGB(255, 255, 255) : RGB(58, 73, 98));
    DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(item->hDC, face);
    if (item->itemState & ODS_FOCUS) { InflateRect(&r, -4, -4); DrawFocusRect(item->hDC, &r); }
    return TRUE;
}
#endif
