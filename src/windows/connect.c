#include "connect.h"
#include "link.h"
#include "panel_style.h"
#include <stdio.h>
static HWND panel, address, port, invitation, status;
static HFONT font;
int connection_translate(MSG *message) {
    return panel && IsDialogMessageW(panel, message);
}
static HWND control(HWND parent, const WCHAR *kind, const WCHAR *text, DWORD style,
                    int x, int y, int w, int h, int id) {
    HWND result;
    if (!wcscmp(kind, L"BUTTON")) style |= BS_OWNERDRAW;
    result = CreateWindowExW(0,
        kind, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
        parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(result, WM_SETFONT, (WPARAM)font, TRUE);
    return result;
}
void connection_refresh(void) {
    static const WCHAR *states[] = {
        L"Hors ligne", L"En attente du pair ou de son authentification…",
        L"Connecté · TLS 1.2 · ECDHE-PSK / ChaCha20-Poly1305",
        L"Connexion refusée ou interrompue. Vérifiez l’invitation et le réseau.",
        L"Déconnexion en cours…"
    };
    if (panel) SetWindowTextW(status, states[link_state()]);
}
static void start_connection(int server) {
    WCHAR wide[160];
    char host[16], service[6], invite[128];
    uint8_t key[32] = {0};
    int result = -1;
    if (link_busy()) {
        MessageBoxW(panel, L"Déconnectez la session actuelle avant d’en ouvrir une autre.", L"Session en cours", MB_OK);
        return;
    }
    if (server) {
        GetWindowTextW(address, wide, 160);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, host, sizeof(host), NULL, NULL)) goto invalid;
        GetWindowTextW(port, wide, 160);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, service, sizeof(service), NULL, NULL)) goto invalid;
        if (murmur_random_key(key) || murmur_invite_format(invite, sizeof(invite), host, service, key)) goto invalid;
        result = link_start(1, host, service, key);
        if (!result) {
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, invite, -1, wide, 160);
            SetWindowTextW(invitation, wide);
            SetFocus(invitation);
            SendMessageW(invitation, EM_SETSEL, 0, -1);
        }
    } else {
        GetWindowTextW(invitation, wide, 160);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, invite, sizeof(invite), NULL, NULL) ||
            murmur_invite_parse(invite, host, service, key)) goto invalid;
        result = link_start(0, host, service, key);
    }
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(invite, sizeof(invite));
    SecureZeroMemory(wide, sizeof(wide));
    if (result) MessageBoxW(panel, L"Impossible de démarrer la connexion.", L"Connexion", MB_OK | MB_ICONERROR);
    connection_refresh();
    return;
invalid:
    SecureZeroMemory(key, sizeof(key));
    SecureZeroMemory(invite, sizeof(invite));
    SecureZeroMemory(wide, sizeof(wide));
    MessageBoxW(panel, L"Adresse IPv4, port (1–65535) ou invitation incorrecte.\n"
        L"Format attendu : murmur1|adresse|port|secret", L"Invitation invalide", MB_OK | MB_ICONWARNING);
}
static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        control(window, L"STATIC", L"Connecter un proche", 0, 24, 20, 550, 28, 0);
        control(window, L"STATIC", L"1. Héberger : indiquez votre IPv4 accessible par votre proche.", 0, 24, 62, 570, 24, 0);
        address = control(window, L"EDIT", L"127.0.0.1", WS_TABSTOP | ES_AUTOHSCROLL, 24, 96, 220, 30, 301);
        port = control(window, L"EDIT", L"48152", WS_TABSTOP | ES_NUMBER, 260, 96, 100, 30, 302);
        SendMessageW(address, EM_SETLIMITTEXT, 15, 0);
        SendMessageW(port, EM_SETLIMITTEXT, 5, 0);
        control(window, L"BUTTON", L"Héberger", WS_TABSTOP, 380, 95, 200, 32, 303);
        control(window, L"STATIC", L"2. Partagez l’invitation en privé, ou collez celle de votre proche.", 0, 24, 155, 570, 26, 0);
        invitation = control(window, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            24, 192, 556, 76, 304);
        SendMessageW(invitation, EM_SETLIMITTEXT, 127, 0);
        control(window, L"BUTTON", L"Rejoindre", WS_TABSTOP, 380, 282, 200, 34, 305);
        control(window, L"BUTTON", L"Déconnecter", WS_TABSTOP, 24, 282, 160, 34, 306);
        status = control(window, L"STATIC", L"", 0, 24, 344, 556, 48, 307);
        control(window, L"STATIC", L"L’invitation est une clé secrète : toute personne qui la possède peut rejoindre.\n"
            L"127.0.0.1 sert aux tests sur ce PC. Sur Internet, le port TCP doit être\n"
            L"accessible. Le vocal utilise le port UDP suivant (ex. 48153).\n"
            L"La traversée automatique des box n’est pas encore intégrée.",
            0, 24, 408, 556, 90, 0);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == 303) start_connection(1);
            if (LOWORD(wp) == 305) start_connection(0);
            if (LOWORD(wp) == 306) { link_stop(); connection_refresh(); }
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: return direct_panel_color(message, wp, lp);
    case WM_DRAWITEM: return direct_panel_button((DRAWITEMSTRUCT *)lp, font);
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        SetWindowTextW(invitation, L"");
        DeleteObject(font); panel = NULL;
        return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
void connection_show(HWND parent) {
    WNDCLASSW wc = {0};
    if (panel) { SetForegroundWindow(panel); return; }
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MurmurConnection";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    panel = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Direct — Connexion par adresse IP",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 626, 560,
        parent, NULL, wc.hInstance, NULL);
    connection_refresh();
    ShowWindow(panel, SW_SHOW);
}
