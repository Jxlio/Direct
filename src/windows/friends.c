#include <winsock2.h>
#include <ws2tcpip.h>
#include "friends.h"
#include "connect.h"
#include "link.h"
#include "murmur/friend.h"
#include <shlobj.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <string.h>
#include <wchar.h>
static friend_book book;
static WCHAR path[MAX_PATH];
static HWND panel, list, name_edit, code_edit, status, ticker;
static HFONT font, title_font, small_font;
static HBRUSH background_brush, input_brush;
static UINT panel_dpi = 96;
static int scaled(int n) { return MulDiv(n, (int)panel_dpi, 96); }
static SOCKET discovery = INVALID_SOCKET;
static int selected = -1, enabled, responder_started, answer_received, winsock_ready;
static int sandbox;
static uint8_t session[32];
static struct sockaddr_in destination;
static ULONGLONG deadline;
static const WCHAR *notice = L"Ajoutez un ami une seule fois. La connexion automatique fonctionne sur le même réseau local.";
static void refresh(void) {
    DWORD i; LRESULT selection;
    if (!panel) return;
    selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < book.count; ++i) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)book.items[i].name);
    if (selection != LB_ERR) SendMessageW(list, LB_SETCURSEL, (WPARAM)selection, 0);
    EnableWindow(GetDlgItem(panel, 502), !sandbox && selection != LB_ERR && !link_busy() && discovery == INVALID_SOCKET);
    EnableWindow(GetDlgItem(panel, 509), selection != LB_ERR && !link_busy() && discovery == INVALID_SOCKET);
    SetWindowTextW(status, notice);
    if (sandbox) {
        SetWindowTextW(status,link_state()==2 ? L"Ami de test connecté. Messages réels chiffrés ; appel simulé, sans accès au microphone." : L"Connexion du second client local en cours. Aucun contact personnel n’est modifié.");
        EnableWindow(GetDlgItem(panel,506),FALSE); EnableWindow(GetDlgItem(panel,507),FALSE);
        EnableWindow(GetDlgItem(panel,508),FALSE); EnableWindow(GetDlgItem(panel,509),FALSE);
    }
    ShowWindow(list, book.count ? SW_SHOWNA : SW_HIDE);
    InvalidateRect(panel, NULL, FALSE);
}
static void stop_discovery(void) {
    if (ticker) KillTimer(ticker, 1);
    if (discovery != INVALID_SOCKET) closesocket(discovery);
    discovery = INVALID_SOCKET; selected = -1; responder_started = answer_received = 0;
    SecureZeroMemory(session, sizeof(session));
}
static void tick(void) {
    uint8_t packet[MURMUR_FRIEND_PACKET], received_session[32];
    char token[6000]; struct sockaddr_in source; unsigned role;
    int length, source_size, i; size_t packet_size;
    if (discovery == INVALID_SOCKET || selected < 0) return;
    if (link_state() == 2) { notice = L"Votre ami est connecté. Vous pouvez discuter et rejoindre le vocal."; stop_discovery(); refresh(); return; }
    if (GetTickCount64() > deadline || ((book.items[selected].offerer || responder_started) && (link_state() == 3 || link_state() == 4))) {
        notice = L"Connexion interrompue ou ami introuvable. Vérifiez que vous avez tous les deux cliqué Retrouver, sur le même réseau.";
        link_stop(); stop_discovery(); refresh(); return;
    }
    for (i = 0; i < 8; ++i) {
        source_size = sizeof(source);
        length = recvfrom(discovery, (char *)packet, sizeof(packet), 0, (struct sockaddr *)&source, &source_size);
        if (length <= 0) break;
        if (murmur_friend_open(book.items[selected].secret, packet, (size_t)length, &role, received_session, token)) continue;
        if (book.items[selected].offerer && role == 1 && !answer_received && !memcmp(session, received_session, 32)) {
            if (!link_ice_answer(token)) answer_received = 1;
        } else if (!book.items[selected].offerer && role == 0 && !responder_started && !link_busy()) {
            murmur_ice_config settings = {0};
            if (!link_ice_start(0, &settings, token)) {
                memcpy(session, received_session, 32); destination = source; responder_started = 1;
                notice = L"Ami trouvé. Authentification de la connexion…"; refresh();
            }
        }
    }
    if ((book.items[selected].offerer || responder_started) && link_ice_token(token, sizeof(token)) == 1) {
        packet_size = sizeof(packet);
        if (!murmur_friend_seal(book.items[selected].secret, book.items[selected].offerer ? 0 : 1,
                                session, token, packet, &packet_size))
            sendto(discovery, (const char *)packet, (int)packet_size, 0, (struct sockaddr *)&destination, sizeof(destination));
    }
    SecureZeroMemory(token, sizeof(token)); SecureZeroMemory(packet, sizeof(packet));
    SecureZeroMemory(received_session, sizeof(received_session));
}
static void connect_friend(int index) {
    struct sockaddr_in local = {0}; u_long nonblocking = 1; BOOL broadcast = TRUE;
    murmur_ice_config settings = {0};
    if (!ticker || index < 0 || (DWORD)index >= book.count || link_busy() || discovery != INVALID_SOCKET) {
        notice = L"Sélectionnez un ami et déconnectez la session actuelle avant de réessayer."; refresh(); return;
    }
    discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery == INVALID_SOCKET) goto fail;
    local.sin_family = AF_INET; local.sin_port = htons(49381); local.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef MURMUR_FRIEND_DISCOVERY_TEST
    local.sin_port = htons(book.items[index].offerer ? 49382 : 49383);
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    strcpy_s(settings.bind_address, sizeof(settings.bind_address), "127.0.0.1");
#endif
    if (setsockopt(discovery, SOL_SOCKET, SO_BROADCAST, (const char *)&broadcast, sizeof(broadcast)) ||
        ioctlsocket(discovery, FIONBIO, &nonblocking) || bind(discovery, (struct sockaddr *)&local, sizeof(local))) goto fail;
    selected = index; deadline = GetTickCount64()+120000;
    destination = local; destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
#ifdef MURMUR_FRIEND_DISCOVERY_TEST
    destination.sin_port = htons(book.items[index].offerer ? 49383 : 49382);
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#endif
    if (book.items[index].offerer && (murmur_random_key(session) || link_ice_start(1, &settings, NULL))) goto fail;
    notice = L"Recherche de votre ami sur le réseau local… Il doit aussi cliquer Retrouver. Aucun code de session à échanger.";
    SetTimer(ticker, 1, 1000, NULL);
    refresh(); return;
fail:
    stop_discovery(); notice = L"Découverte indisponible. Vérifiez qu’une autre instance de Murmur n’utilise pas déjà ce PC."; refresh();
}
static void add_friend(int create) {
    saved_friend item = {0}; WCHAR wide[128]; char code[128]; DWORD i;
    if (!enabled || book.count >= 16 || discovery != INVALID_SOCKET || link_busy()) {
        notice = L"Ajout indisponible : arrêtez la connexion en cours, ou vérifiez le stockage (16 amis maximum)."; refresh(); return;
    }
    GetWindowTextW(name_edit, item.name, 64);
    if (!item.name[0]) { notice = L"Choisissez d’abord un nom pour cet ami."; refresh(); return; }
    if (create) {
        if (murmur_random_key(item.secret)) return;
        murmur_friend_code(item.secret, code); item.offerer = 1;
    } else {
        GetWindowTextW(code_edit, wide, 128);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, code, sizeof(code), NULL, NULL) ||
            murmur_friend_parse(code, item.secret)) {
            notice = L"Code ami invalide. Collez le code complet reçu en privé."; goto done;
        }
    }
    for (i = 0; i < book.count; ++i) if (!memcmp(item.secret, book.items[i].secret, 32)) {
        notice = L"Cet ami est déjà enregistré."; goto done;
    }
    book.items[book.count++] = item;
    if (friend_book_save(path, &book)) {
        SecureZeroMemory(&book.items[--book.count], sizeof(item));
        notice = L"Impossible d’enregistrer cet ami. Aucun ajout effectué."; goto done;
    }
    if (create) {
        MultiByteToWideChar(CP_UTF8, 0, code, -1, wide, 128); SetWindowTextW(code_edit, wide);
        notice = L"Ami enregistré. Envoyez ce code une seule fois, en privé, à cette personne uniquement.";
    } else { SetWindowTextW(code_edit, L""); notice = L"Ami enregistré. Vous pouvez maintenant vous retrouver sans échanger de nouveaux codes."; }
done:
    SecureZeroMemory(&item, sizeof(item)); SecureZeroMemory(code, sizeof(code)); SecureZeroMemory(wide, sizeof(wide)); refresh();
}
static HWND control(HWND window, const WCHAR *kind, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND child;
    if (!wcscmp(kind, L"BUTTON")) style |= BS_OWNERDRAW;
    child = CreateWindowExW(0, kind, text,
        WS_CHILD | WS_VISIBLE | style, scaled(x), scaled(y), scaled(w), scaled(h), window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE); return child;
}
static void panel_label(HDC dc, const WCHAR *text, int x, int y, int w, int h, HFONT face, COLORREF color) {
    RECT r = {scaled(x), scaled(y), scaled(x+w), scaled(y+h)};
    HGDIOBJ old = SelectObject(dc, face); SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &r, DT_WORDBREAK | DT_NOPREFIX); SelectObject(dc, old);
}
static void panel_round(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); HGDIOBJ old = SelectObject(dc, brush);
    HGDIOBJ pen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, scaled(12), scaled(12));
    SelectObject(dc, pen); SelectObject(dc, old); DeleteObject(brush);
}
static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_TIMER: tick(); return 0;
    case WM_CREATE:
        panel_dpi = GetDpiForWindow(window);
        background_brush = CreateSolidBrush(RGB(255, 255, 255)); input_brush = CreateSolidBrush(RGB(242, 245, 250));
        font = CreateFontW(-scaled(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        title_font = CreateFontW(-scaled(28), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        small_font = CreateFontW(-scaled(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        list = control(window, L"LISTBOX", L"", WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 28, 132, 244, 238, 501);
        SendMessageW(list, LB_SETITEMHEIGHT, 0, scaled(58));
        control(window, L"BUTTON", L"Connecter sur ce réseau", WS_TABSTOP, 28, 390, 244, 42, 502);
        control(window, L"BUTTON", L"Annuler la connexion", WS_TABSTOP, 28, 442, 244, 34, 503);
        control(window, L"BUTTON", L"Oublier cet ami", WS_TABSTOP, 28, 484, 244, 30, 509);
        name_edit = control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 332, 164, 340, 27, 504);
        SendMessageW(name_edit, EM_SETLIMITTEXT, 63, 0);
        SendMessageW(name_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Nom de votre ami");
        control(window, L"BUTTON", L"Créer un code ami", WS_TABSTOP, 320, 212, 364, 42, 506);
        code_edit = control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 332, 316, 340, 27, 505);
        SendMessageW(code_edit, EM_SETLIMITTEXT, 72, 0);
        SendMessageW(code_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Collez le code reçu ici");
        control(window, L"BUTTON", L"Ajouter avec ce code", WS_TABSTOP, 320, 366, 364, 40, 507);
        status = control(window, L"STATIC", L"", 0, 32, 548, 648, 46, 520);
        SendMessageW(status, WM_SETFONT, (WPARAM)small_font, TRUE);
        control(window, L"BUTTON", L"Connexion Internet…", WS_TABSTOP, 464, 608, 220, 32, 508);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(window, &ps); RECT r;
        GetClientRect(window, &r); FillRect(dc, &r, background_brush);
        panel_label(dc, L"Amis et connexions", 28, 24, 656, 44, title_font, RGB(24, 34, 52));
        panel_label(dc, L"Enregistrez un ami ou connectez-vous pour discuter et appeler.", 30, 74, 650, 24, font, RGB(104, 116, 137));
        panel_label(dc, L"VOS AMIS", 28, 110, 244, 20, small_font, RGB(58, 103, 244));
        panel_label(dc, L"AJOUTER UN AMI", 320, 110, 364, 20, small_font, RGB(58, 103, 244));
        panel_label(dc, L"Nom de l’ami", 320, 136, 364, 22, small_font, RGB(104, 116, 137));
        { RECT field = {scaled(320), scaled(156), scaled(684), scaled(199)}; panel_round(dc, field, RGB(242, 245, 250)); }
        { RECT field = {scaled(320), scaled(308), scaled(684), scaled(351)}; panel_round(dc, field, RGB(242, 245, 250)); }
        panel_label(dc, L"CODE AMI REÇU OU CRÉÉ", 320, 280, 364, 22, small_font, RGB(104, 116, 137));
        panel_label(dc, L"Le code est privé. Partagez-le uniquement avec cet ami.\nAprès création, il apparaît dans le champ ci-dessus.", 320, 430, 364, 60, small_font, RGB(104, 116, 137));
        if (!book.count) {
            panel_label(dc, L"Aucun ami\nenregistré", 40, 176, 210, 72, title_font, RGB(24, 34, 52));
            panel_label(dc, L"Ajoutez votre premier ami\navec une invitation privée.", 40, 272, 210, 60, font, RGB(104, 116, 137));
        }
        panel_label(dc, L"Deux lieux différents ? Utilisez Connexion Internet.\nLes codes amis fonctionnent sur le réseau local pour l’instant.", 30, 608, 420, 44, small_font, RGB(104, 116, 137));
        EndPaint(window, &ps); return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, RGB(104, 116, 137)); SetBkColor((HDC)wp, RGB(255, 255, 255)); return (LRESULT)background_brush;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor((HDC)wp, RGB(24, 34, 52)); SetBkColor((HDC)wp, RGB(242, 245, 250)); return (LRESULT)input_brush;
    case WM_MEASUREITEM: ((MEASUREITEMSTRUCT *)lp)->itemHeight = (UINT)scaled(58); return TRUE;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp; RECT r = item->rcItem; WCHAR text[96]; HGDIOBJ old;
        int primary = item->CtlID == 502 || item->CtlID == 506;
        int chosen = (item->itemState & ODS_SELECTED) != 0;
        int disabled = (item->itemState & ODS_DISABLED) != 0;
        FillRect(item->hDC, &r, background_brush);
        if (item->CtlID == 501) {
            if (item->itemID == (UINT)-1 || item->itemID >= book.count) return TRUE;
            panel_round(item->hDC, r, chosen ? RGB(228, 236, 255) : RGB(246, 248, 252));
            r.left += scaled(14); r.top += scaled(8);
            old = SelectObject(item->hDC, font); SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, RGB(24, 34, 52));
            DrawTextW(item->hDC, book.items[item->itemID].name, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            r.top += scaled(24); SelectObject(item->hDC, small_font); SetTextColor(item->hDC, RGB(104, 116, 137));
            DrawTextW(item->hDC, sandbox ? (link_state()==2 ? L"En ligne · test local" : L"Connexion de test…") : L"Ami enregistré", -1, &r, DT_SINGLELINE); SelectObject(item->hDC, old);
        } else {
            panel_round(item->hDC, r, disabled ? RGB(242, 245, 250) : primary ? (chosen ? RGB(43, 83, 210) : RGB(58, 103, 244)) : chosen ? RGB(221, 230, 247) : RGB(242, 245, 250));
            GetWindowTextW(item->hwndItem, text, 96); old = SelectObject(item->hDC, font);
            SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, disabled ? RGB(133, 145, 164) : primary ? RGB(255, 255, 255) : RGB(58, 73, 98));
            DrawTextW(item->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); SelectObject(item->hDC, old);
        }
        if (item->itemState & ODS_FOCUS) { r = item->rcItem; InflateRect(&r, -scaled(3), -scaled(3)); DrawFocusRect(item->hDC, &r); }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 501 && HIWORD(wp) == LBN_SELCHANGE) {
            LRESULT index = SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (index >= 0 && (DWORD)index < book.count && book.items[index].offerer) {
                char code[MURMUR_FRIEND_CODE]; WCHAR wide[MURMUR_FRIEND_CODE];
                murmur_friend_code(book.items[index].secret, code);
                MultiByteToWideChar(CP_UTF8, 0, code, -1, wide, MURMUR_FRIEND_CODE);
                SetWindowTextW(code_edit, wide);
                SecureZeroMemory(code, sizeof(code)); SecureZeroMemory(wide, sizeof(wide));
            } else SetWindowTextW(code_edit, L"");
            EnableWindow(GetDlgItem(window, 502), !sandbox && index != LB_ERR && !link_busy() && discovery == INVALID_SOCKET);
            EnableWindow(GetDlgItem(window, 509), !sandbox && index != LB_ERR && !link_busy() && discovery == INVALID_SOCKET);
        }
        if (HIWORD(wp) == BN_CLICKED) switch (LOWORD(wp)) {
        case 502: connect_friend((int)SendMessageW(list, LB_GETCURSEL, 0, 0)); break;
        case 503: stop_discovery(); link_stop(); notice = L"Connexion arrêtée."; refresh(); break;
        case 506: add_friend(1); break;
        case 507: add_friend(0); break;
        case 508: connection_auto_show(window); break;
        case 509: {
            LRESULT index = SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (enabled && index >= 0 && (DWORD)index < book.count && !link_busy() && discovery == INVALID_SOCKET) {
                friend_book updated = book;
                memmove(&updated.items[index], &updated.items[index+1], (updated.count-(DWORD)index-1)*sizeof(saved_friend));
                SecureZeroMemory(&updated.items[--updated.count], sizeof(saved_friend));
                if (!friend_book_save(path, &updated)) { book = updated; notice = L"Ami oublié. Son ancien code ne permet plus de vous retrouver sur ce PC."; }
                else notice = L"Impossible d’enregistrer la suppression.";
                SecureZeroMemory(&updated, sizeof(updated)); refresh();
            }
            break;
        }
        } return 0;
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY: if (window == panel) { panel = NULL; DeleteObject(font); DeleteObject(title_font); DeleteObject(small_font); DeleteObject(background_brush); DeleteObject(input_brush); } return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
void friends_init(HWND parent) {
    WNDCLASSW wc = {0}; WSADATA data;
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MurmurFriends";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1); RegisterClassW(&wc);
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, path)) && wcslen(path)+24 < MAX_PATH) {
        wcscat_s(path, MAX_PATH, L"\\Murmur");
        if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            wcscat_s(path, MAX_PATH, L"\\friends.dat"); enabled = !friend_book_load(path, &book);
        }
    }
    if (!enabled) notice = L"Stockage des amis inaccessible ou endommagé. Le fichier existant a été conservé.";
    if (!WSAStartup(MAKEWORD(2, 2), &data)) {
        winsock_ready = 1;
        /* A separate class avoids creating controls for the message-only timer window. */
        wc.lpszClassName = L"MurmurFriendTicker"; wc.lpfnWndProc = DefWindowProcW; RegisterClassW(&wc);
        ticker = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    }
    (void)parent;
}
void friends_init_test(HWND parent,const WCHAR *peer) {
    WNDCLASSW wc={0}; (void)parent;
    wc.lpfnWndProc=proc; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"MurmurFriends";
    wc.hCursor=LoadCursorW(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH); RegisterClassW(&wc);
    sandbox=1; enabled=0; book.version=book.count=1;
    wcscpy_s(book.items[0].name,64,peer);
    notice=L"Test local : contact temporaire, sans modification de votre liste d’amis.";
}
void friends_connection_refresh(void) { if (sandbox) refresh(); }
/* Timer messages are dispatched by the main loop, even when the panel is closed. */
int friends_translate(MSG *message) {
    if (message->hwnd == ticker && message->message == WM_TIMER) { tick(); return 1; }
    return panel && IsDialogMessageW(panel, message);
}
void friends_show(HWND parent) {
    RECT owner; int x, y; BOOL dark = FALSE; COLORREF caption = RGB(255, 255, 255);
    if (panel) { SetForegroundWindow(panel); return; }
    panel_dpi = GetDpiForWindow(parent); GetWindowRect(parent, &owner);
    x = owner.left + ((owner.right-owner.left)-scaled(732))/2;
    y = owner.top + ((owner.bottom-owner.top)-scaled(704))/2;
    panel = CreateWindowExW(WS_EX_CONTROLPARENT, L"MurmurFriends", L"Direct — Amis et connexions",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, scaled(732), scaled(704),
        parent, NULL, GetModuleHandleW(NULL), NULL);
    DwmSetWindowAttribute(panel, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(panel, 35, &caption, sizeof(caption));
    refresh(); ShowWindow(panel, SW_SHOW);
}
void friends_close(void) {
    stop_discovery(); if (ticker) { KillTimer(ticker, 1); DestroyWindow(ticker); }
    ticker = NULL;
    if (winsock_ready) { WSACleanup(); winsock_ready = 0; }
    SecureZeroMemory(&book, sizeof(book));
}
#ifdef MURMUR_FRIEND_DISCOVERY_TEST
/* Uses the production discovery controller, with two loopback ports instead of
 * a LAN broadcast. No profile files, public network or audio devices. */
int friends_test_endpoint(int offerer) {
    WSADATA data; ULONGLONG until; link_message message; int got = 0, sent = 0, result = -1;
    if (WSAStartup(MAKEWORD(2, 2), &data)) return -1;
    ticker = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
    if (!ticker) return -1;
    book.version = book.count = 1; book.items[0].offerer = (DWORD)offerer;
    memset(book.items[0].secret, 0x42, 32); wcscpy_s(book.items[0].name, 64, L"test-only");
    link_init(NULL); connect_friend(0); until = GetTickCount64()+12000;
    while (GetTickCount64() < until) {
        tick();
        if (link_state() == 2 && !sent && !offerer) {
            if (link_send(0, (const uint8_t *)"friend hello", 12)) break;
            sent = 1;
        }
        if (link_receive(&message)) {
            if (offerer && message.size == 12 && !memcmp(message.text, "friend hello", 12)) {
                if (link_send(0, (const uint8_t *)"friend ack", 10)) break;
                got = 1;
            } else if (!offerer && message.size == 10 && !memcmp(message.text, "friend ack", 10)) {
                got = 1; break;
            } else break;
        }
        if (offerer && got && !link_busy()) break;
        if (link_state() == 3 && !got) break;
        Sleep(20);
    }
    if (got && !link_voice_requested() && !link_voice_state()) result = 0;
    stop_discovery(); link_stop(); until = GetTickCount64()+2000;
    while (link_busy() && GetTickCount64() < until) Sleep(2);
    if (link_busy()) result = -1;
    friends_close(); WSACleanup(); return result;
}
#endif
