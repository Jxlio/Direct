#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <dwmapi.h>
#include <commctrl.h>
#include "link.h"
#include "connect.h"
#include "friends.h"
#include "icons.h"
#include "updates.h"
#include "local_test.h"
#include "settings.h"
#include "signaling.h"
#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/* UI state belongs to the main thread. No idle render loop.
   Network/audio transport runs on a separate owning thread. */
#define ID_INPUT 101
#define ID_SEND 102
#define ID_VOICE 103
#define ID_SCREEN 104
#define ID_MUTE 106
#define MAX_MESSAGES 128
#define MAX_TEXT 2048
typedef struct { WCHAR text[MAX_TEXT + 1]; int room, remote; WCHAR author[64]; SYSTEMTIME time; } Message;
static struct {
    HWND input, send, voice, screen, mute, rooms[3], connection, invite, updates, test;
    HFONT normal, title, caption_font, heading;
    HBRUSH background, field;
    Message messages[MAX_MESSAGES];
    size_t count;
    int room;
    UINT dpi;
    WNDPROC edit_proc;
    int test_mode, test_greeted, test_failed;
    ULONGLONG test_deadline;
    uint8_t last_peer[32]; int have_peer;
} app;
static const WCHAR *peer_name(void) { return app.test_mode ? (app.test_mode==1 ? L"Client B" : L"Client A") : friends_peer_name()?friends_peer_name():L"Votre ami"; }
static const WCHAR *self_name(void) { return app.test_mode ? (app.test_mode==1 ? L"Client A · vous" : L"Client B · vous") : L"Vous"; }
static const WCHAR *rooms[] = { L"Messages", L"Organisation", L"Jeux" };
static const WCHAR *descriptions[] = {
    L"Vos échanges avec l’ami connecté.",
    L"Vos rendez-vous et vos projets, dans une discussion séparée.",
    L"Discutez de vos prochaines parties."
};
static const COLORREF bg = RGB(255, 255, 255), sidebar = RGB(245, 247, 251);
static const COLORREF fg = RGB(24, 34, 52), muted = RGB(104, 116, 137);
static const COLORREF accent = RGB(58, 103, 244);
static const COLORREF field_color = RGB(242, 245, 250);
static int px(int n) { return MulDiv(n, (int)app.dpi, 96); }
static int content_right(int width) { return width; }
static const WCHAR *connection_description(void) {
    static WCHAR simulated[256];
    if (app.test_mode) {
        if (app.test_failed) return L"TEST LOCAL · Échec de connexion. Fermez les deux clients et relancez Test solo.";
        if (link_state()==2) {
            swprintf_s(simulated,256,L"TEST LOCAL · %ls connecté · micro fermé · %u paquets audio reçus · %llu trames décodées",
                peer_name(),link_voice_received_packets(),(unsigned long long)link_voice_decoded_frames());
            return simulated;
        }
        if (!link_state() || link_state()==3) return L"TEST LOCAL · L’autre client est déconnecté. Vos messages de test restent affichés.";
        return L"TEST LOCAL · Connexion automatique des deux clients sur ce PC…";
    }
    if (link_state() == 3) return L"Connexion échouée. Ouvrez Mes amis → Options de connexion pour lire le diagnostic.";
    if (link_state() == 4) return L"Déconnexion en cours…";
    if (link_state() == 2) return L"Votre ami est connecté · conversation chiffrée.";
    if (link_state() != 1) return L"";
    if (friends_auto_connecting()) return L"Connexion automatique : recherche d’un chemin direct et authentification…";
    if (!link_ice_mode()) return L"Connexion manuelle en cours…";
    switch (link_ice_phase()) {
    case -2: return L"Réponse invalide. Utilisez la réponse correspondant à votre invitation en cours.";
    case 1: return L"En attente du code retour de votre ami. Collez-le dans Options de connexion puis cliquez Connecter.";
    case 2: return L"Recherche d’un chemin réseau entre vos deux appareils…";
    case 3: return L"Chemin réseau trouvé. Authentification de la connexion…";
    default: return L"Préparation de la connexion…";
    }
}
static const WCHAR *voice_description(void) {
    static const WCHAR *states[] = {L"Aucun appel en cours", L"En attente de votre ami…",
        L"Connexion à l’appel…", L"Appel en cours", L"Microphone coupé",
        L"Erreur · vérifiez le son"};
    if (!link_voice_requested() && (link_voice_remote()&1)) return L"Appel entrant · répondez ci-dessous";
    return states[link_voice_state()];
}
static void box(HDC dc, int x, int y, int w, int h, COLORREF color) {
    RECT r = { px(x), px(y), px(x + w), px(y + h) };
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &r, brush);
    DeleteObject(brush);
}
static void rounded(HDC dc, int x, int y, int w, int h, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldbrush = SelectObject(dc, brush);
    HGDIOBJ oldpen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, px(x), px(y), px(x + w), px(y + h), px(radius), px(radius));
    SelectObject(dc, oldpen);
    SelectObject(dc, oldbrush);
    DeleteObject(brush);
}
static void glass(HDC dc, int x, int y, int w, int h, int radius, BYTE opacity) {
    HDC layer = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, px(w), px(h));
    HGDIOBJ old = SelectObject(layer, bitmap); RECT fill = {0, 0, px(w), px(h)};
    HRGN clip = CreateRoundRectRgn(px(x), px(y), px(x+w+1), px(y+h+1), px(radius), px(radius));
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, opacity, 0}; int saved = SaveDC(dc);
    FillRect(layer, &fill, (HBRUSH)GetStockObject(WHITE_BRUSH)); SelectClipRgn(dc, clip);
    AlphaBlend(dc, px(x), px(y), px(w), px(h), layer, 0, 0, px(w), px(h), blend);
    RestoreDC(dc, saved); DeleteObject(clip); SelectObject(layer, old); DeleteObject(bitmap); DeleteDC(layer);
    {
        HPEN edge = CreatePen(PS_SOLID, 1, RGB(224, 232, 248));
        HGDIOBJ previous = SelectObject(dc, edge), brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, px(x), px(y), px(x+w), px(y+h), px(radius), px(radius));
        SelectObject(dc, brush); SelectObject(dc, previous); DeleteObject(edge);
    }
}
static void label(HDC dc, HFONT font, COLORREF color, int x, int y,
                  int w, int h, const WCHAR *text, UINT flags) {
    RECT r = { px(x), px(y), px(x + w), px(y + h) };
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
static void icon(HDC dc, int x, int y, int size, enum direct_icon kind, COLORREF color) {
    direct_icon_draw(dc, app.dpi, kind, color, x, y, size);
}
static void fonts(void) {
    int i;
    if (app.normal) DeleteObject(app.normal);
    if (app.title) DeleteObject(app.title);
    if (app.caption_font) DeleteObject(app.caption_font);
    if (app.heading) DeleteObject(app.heading);
    direct_icons_clear();
    app.heading = CreateFontW(-px(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    app.normal = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    app.title = CreateFontW(-px(34), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    app.caption_font = CreateFontW(-px(12), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    if (app.input) {
        SendMessageW(app.input, WM_SETFONT, (WPARAM)app.normal, TRUE);
        SendMessageW(app.send, WM_SETFONT, (WPARAM)app.normal, TRUE);
        SendMessageW(app.voice, WM_SETFONT, (WPARAM)app.normal, TRUE);
        SendMessageW(app.screen, WM_SETFONT, (WPARAM)app.normal, TRUE);
        SendMessageW(app.connection, WM_SETFONT, (WPARAM)app.normal, TRUE);
        SendMessageW(app.mute, WM_SETFONT, (WPARAM)app.normal, TRUE);
        for (i = 0; i < 3; ++i) SendMessageW(app.rooms[i], WM_SETFONT, (WPARAM)app.normal, TRUE);
    }
}
static void layout(HWND window) {
    RECT r;
    int w, h, right, i;
    GetClientRect(window, &r);
    w = MulDiv(r.right, 96, (int)app.dpi);
    h = MulDiv(r.bottom, 96, (int)app.dpi);
    right = content_right(w);
    MoveWindow(app.input, px(300), px(h - 82), px(right - 448), px(26), TRUE);
    MoveWindow(app.send, px(right - 126), px(h - 91), px(90), px(42), TRUE);
    MoveWindow(app.voice, px(28), px(468), px(140), px(38), TRUE);
    MoveWindow(app.mute, px(176), px(468), px(48), px(38), TRUE);
    MoveWindow(app.screen, px(right - 216), px(20), px(188), px(34), TRUE);
    MoveWindow(app.updates, px(right - 344), px(20), px(116), px(34), TRUE);
    MoveWindow(app.connection, px(16), px(100), px(220), px(42), TRUE);
    ShowWindow(app.test, SW_HIDE);
    MoveWindow(app.invite, px(300), px(h / 2 + 100), px(244), px(46), TRUE);
    {
        int any = app.count != 0;
        ShowWindow(app.invite, !any && link_state() != 2 ? SW_SHOWNA : SW_HIDE);
    }
    MoveWindow(app.rooms[0],px(20),px(208),px(212),px(64),TRUE);
    ShowWindow(app.rooms[0],link_state()==2 || app.count ? SW_SHOWNA:SW_HIDE);
    for (i=1;i<3;++i) ShowWindow(app.rooms[i],SW_HIDE);
}
static void send_local(HWND window) {
    WCHAR text[MAX_TEXT + 1];
    uint8_t utf8[MURMUR_CHAT_LIMIT];
    size_t i;
    int length = GetWindowTextW(app.input, text, MAX_TEXT + 1);
    for (i = 0; i < (size_t)length && iswspace(text[i]); ++i) {}
    if (i == (size_t)length) return;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
        (char *)utf8, sizeof(utf8), NULL, NULL);
    if (!length || link_send((unsigned)app.room, utf8, (size_t)length)) {
        SecureZeroMemory(utf8, sizeof(utf8));
        MessageBoxW(window, L"Connectez un proche avant d’envoyer un message.\n"
            L"Si vous êtes déjà connecté, attendez : la file d’envoi peut être pleine.", L"Message non envoyé", MB_OK);
        return;
    }
    SecureZeroMemory(utf8, sizeof(utf8));
    if (app.count == MAX_MESSAGES) {
        memmove(app.messages, app.messages + 1, sizeof(Message) * (MAX_MESSAGES - 1));
        --app.count;
    }
    wcscpy_s(app.messages[app.count].text, MAX_TEXT + 1, text);
    app.messages[app.count++].room = app.room;
    app.messages[app.count - 1].remote = 0;
    GetLocalTime(&app.messages[app.count-1].time);
    wcscpy_s(app.messages[app.count-1].author,64,app.messages[app.count-1].remote?peer_name():self_name());
    SetWindowTextW(app.input, L"");
    InvalidateRect(window, NULL, FALSE);
    SetFocus(app.input);
}
static LRESULT CALLBACK edit_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_GETDLGCODE && lp && ((MSG *)lp)->wParam == VK_RETURN)
        return DLGC_WANTALLKEYS;
    if (message == WM_KEYDOWN && wp == VK_RETURN) {
        send_local(GetParent(window));
        return 0;
    }
    if (message == WM_CHAR && wp == VK_RETURN) return 0;
    return CallWindowProcW(app.edit_proc, window, message, wp, lp);
}
static WNDPROC base_button;
static LRESULT CALLBACK button_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MOUSEMOVE && !GetPropW(window, L"hover")) {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, window, 0};
        SetPropW(window, L"hover", (HANDLE)1); TrackMouseEvent(&track); InvalidateRect(window, NULL, FALSE);
    }
    if (message == WM_MOUSELEAVE || message == WM_NCDESTROY) {
        RemovePropW(window, L"hover"); InvalidateRect(window, NULL, FALSE);
    }
    return CallWindowProcW(base_button, window, message, wp, lp);
}
static void enhance_button(HWND button) {
    if (!base_button) base_button = (WNDPROC)GetWindowLongPtrW(button, GWLP_WNDPROC);
    SetWindowLongPtrW(button, GWLP_WNDPROC, (LONG_PTR)button_proc);
}
static void paint(HWND window, HDC dc) {
    RECT r; int w, h, i, y, matching = 0;
    GetClientRect(window, &r);
    w = MulDiv(r.right, 96, (int)app.dpi); h = MulDiv(r.bottom, 96, (int)app.dpi);
    {
        TRIVERTEX stops[2] = {{0, 0, 0xeb00, 0xf000, 0xff00, 0},
            {r.right, r.bottom, 0xf800, 0xfb00, 0xff00, 0}};
        GRADIENT_RECT gradient = {0, 1}; GradientFill(dc, stops, 2, &gradient, 1, GRADIENT_FILL_RECT_H);
    }
    glass(dc, 12, 12, 228, h-24, 24, 145);
    glass(dc, 264, 12, w-276, 64, 20, 180);
    box(dc, 251, 0, 1, h, RGB(229, 234, 242));
    /* A small brand mark, rather than an unused server rail. */
    rounded(dc, 24, 24, 34, 34, 12, accent);
    icon(dc, 32, 32, 18, ICON_MESSAGE, RGB(255, 255, 255));
    label(dc, app.heading, fg, 70, 22, 148, 24, L"Direct", DT_SINGLELINE);
    label(dc, app.caption_font, muted, 70, 47, 148, 20, L"Messages et appels", DT_SINGLELINE);
    label(dc,app.caption_font,muted,30,80,190,18,L"AMIS",DT_SINGLELINE);
    label(dc,app.caption_font,muted,30,176,190,20,L"CONVERSATIONS",DT_SINGLELINE);
    if(link_state()!=2 && !app.count) label(dc,app.caption_font,muted,30,218,180,52,L"Connectez un ami pour\ncommencer à discuter.",DT_WORDBREAK);
    box(dc,28,308,196,1,RGB(229,234,242));
    glass(dc,24,330,204,190,18,170);
    label(dc,app.caption_font,accent,38,344,178,20,app.test_mode?L"APPEL · AUDIO SIMULÉ":L"APPEL VOCAL",DT_SINGLELINE);
    label(dc,app.caption_font,fg,38,370,178,36,voice_description(),DT_WORDBREAK);
    if(link_voice_requested() || (link_voice_remote()&1)) {
        WCHAR participant[100];
        swprintf_s(participant,100,L"%ls · %ls",peer_name(),(link_voice_remote()&1)?((link_voice_remote()&2)?L"micro coupé":link_voice_requested()?L"a rejoint":L"vous appelle"):L"en attente");
        label(dc,app.caption_font,(link_voice_remote()&1)?RGB(36,114,83):muted,38,406,178,20,participant,DT_SINGLELINE|DT_END_ELLIPSIS);
        if(link_voice_requested()) label(dc,app.caption_font,link_voice_state()==4?muted:RGB(36,114,83),38,430,178,20,link_voice_state()==4?L"Vous · micro coupé":app.test_mode?L"Vous · audio simulé":L"Vous · micro activé",DT_SINGLELINE);
    } else label(dc,app.caption_font,muted,38,406,178,20,L"Lancez un appel à deux",DT_SINGLELINE);
    box(dc, 16, h-90, 220, 1, RGB(229, 234, 242));
    rounded(dc, 24, h-67, 34, 34, 34, RGB(222, 232, 255));
    label(dc, app.caption_font, accent, 24, h-67, 34, 34, L"V", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    label(dc, app.normal, fg, 70, h-72, 148, 24, self_name(), DT_SINGLELINE);
    label(dc, app.caption_font, muted, 70, h-47, 148, 20,
        link_state() == 2 ? L"Connecté à votre ami" : link_state() == 1 ? L"Connexion en cours…" : link_state() == 3 ? L"Échec de connexion" : L"Sur cet appareil", DT_SINGLELINE);
    icon(dc, 281, 27, 20, ICON_MESSAGE, muted);
    label(dc, app.heading, fg, 310, 24, w-660, 26, link_state()==2 || app.count ? peer_name() : L"Conversations", DT_SINGLELINE | DT_END_ELLIPSIS);
    box(dc, 252, 75, w-252, 1, RGB(234, 238, 245));
    label(dc, app.caption_font, muted, 284, 94, w-320, 24, link_state()==2 ? L"● En ligne · messages privés" : descriptions[app.room], DT_SINGLELINE | DT_END_ELLIPSIS);
    label(dc, app.caption_font, link_state() == 3 ? accent : muted, 284, 119, w-320, 20,
        connection_description(), DT_SINGLELINE | DT_END_ELLIPSIS);
    if(link_voice_requested() || (link_voice_remote()&1)) {
        WCHAR call[160];
        glass(dc,284,150,w-320,66,16,210);
        swprintf_s(call,160,L"%ls · %ls",peer_name(),!link_voice_requested()?L"vous appelle":link_voice_state()==1?L"en attente de réponse":L"appel en cours");
        label(dc,app.heading,accent,302,160,w-360,24,call,DT_SINGLELINE|DT_END_ELLIPSIS);
        label(dc,app.caption_font,muted,302,188,w-360,20,app.test_mode?L"Test silencieux : aucun micro ni haut-parleur ouvert.":!link_voice_requested()?L"Cliquez Répondre dans le panneau d’appel.":link_voice_state()==4?L"Votre microphone est coupé.":(link_voice_remote()&2)?L"Votre ami a coupé son microphone.":L"Les commandes d’appel sont dans le panneau de gauche.",DT_SINGLELINE|DT_END_ELLIPSIS);
    }
    for (i = 0; i < (int)app.count; ++i) ++matching;
    if (!matching) {
        int top = h/2-138;
        glass(dc, 280, top-20, w-308, 328, 24, 125);
        rounded(dc, 300, top, 52, 52, 18, RGB(233, 239, 255));
        icon(dc, 312, top+12, 28, ICON_MESSAGE, accent);
        label(dc, app.caption_font, accent, 300, top+78, w-350, 22, L"MESSAGES PRIVÉS · APPELS VOCAUX", DT_SINGLELINE);
        label(dc, app.title, fg, 298, top+105, w-346, 52, L"Parlez avec vos amis.", DT_SINGLELINE | DT_END_ELLIPSIS);
        label(dc, app.normal, muted, 300, top+164, w-360, 46,
            link_state() == 2 ? L"Votre ami est connecté. Envoyez votre premier message\nou démarrez un appel vocal." :
            L"Ajoutez un ami, connectez-vous et commencez à discuter.\nVos échanges sont chiffrés de bout en bout.", DT_WORDBREAK);
    }
    y = h-138;
    for (i = (int)app.count-1; i >= 0; --i) {
        Message *m=&app.messages[i]; int bubble_width=(w-340)*3/4,x,th,row;
        WCHAR author[120]; RECT measure={0,0,px(bubble_width-32),0}; HGDIOBJ old;

        old=SelectObject(dc,app.normal);
        DrawTextW(dc,m->text,-1,&measure,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|DT_EDITCONTROL);
        SelectObject(dc,old); th=MulDiv(measure.bottom,96,(int)app.dpi)+4; row=th+72;
        if(y-row<(link_voice_requested() || (link_voice_remote()&1)?230:148)) break;
        y-=row; x=m->remote?330:w-38-bubble_width;
        rounded(dc,x,y+24,bubble_width,th+24,16,m->remote?RGB(255,255,255):RGB(222,232,255));
        rounded(dc,m->remote?284:x-44,y+26,32,32,32,m->remote?RGB(235,220,255):accent);
        label(dc,app.caption_font,m->remote?RGB(123,69,184):RGB(255,255,255),m->remote?284:x-44,y+26,32,32,m->remote?(app.test_mode==1?L"B":L"A"):L"V",DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(m->room>0 && m->room<3) swprintf_s(author,120,L"%ls · %ls · %02u:%02u",m->author,rooms[m->room],m->time.wHour,m->time.wMinute);
        else swprintf_s(author,120,L"%ls   ·   %02u:%02u",m->author,m->time.wHour,m->time.wMinute);
        label(dc,app.caption_font,m->remote?RGB(123,69,184):accent,x,y,bubble_width,20,author,DT_SINGLELINE);
        { int saved=SaveDC(dc); IntersectClipRect(dc,px(x+16),px(y+36),px(x+bubble_width-16),px(y+36+th));
          label(dc,app.normal,fg,x+16,y+36,bubble_width-32,th,m->text,DT_WORDBREAK|DT_EDITCONTROL); RestoreDC(dc,saved); }
    }
    glass(dc, 280, h-108, w-308, 72, 18, 210);
    rounded(dc, 294, h-91, w-432, 40, 12, field_color);
    label(dc, app.caption_font, muted, 294, h-27, w-330, 20,
        link_state() == 2 ? L"Entrée pour envoyer   ·   Conversation chiffrée" : L"Vos messages restent entre vous. Connectez un ami pour commencer.", DT_SINGLELINE | DT_END_ELLIPSIS);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        link_init(window);
        settings_init(app.test_mode);
        if (app.test_mode) friends_init_test(window,peer_name());
        else { friends_init(window); updates_init(window); }
        app.dpi = GetDpiForWindow(window);
        app.background = CreateSolidBrush(bg);
        app.field = CreateSolidBrush(field_color);
        app.input = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_INPUT, NULL, NULL);
        app.send = CreateWindowW(L"BUTTON", L"Envoyer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_SEND, NULL, NULL);
        app.voice = CreateWindowW(L"BUTTON", L"Appeler", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_VOICE, NULL, NULL);
        app.screen = CreateWindowW(L"BUTTON", L"Partage d’écran · bientôt", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_SCREEN, NULL, NULL);
        app.mute = CreateWindowW(L"BUTTON", L"Couper le microphone", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)ID_MUTE, NULL, NULL);
        EnableWindow(app.mute, FALSE);
        EnableWindow(app.screen, FALSE);
        EnableWindow(app.voice, FALSE);
        EnableWindow(app.send, FALSE);
        app.connection = CreateWindowW(L"BUTTON", L"Amis · ajouter et gérer", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)105, NULL, NULL);
        app.updates = CreateWindowW(L"BUTTON", L"Paramètres", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)108, NULL, NULL);
        app.test = CreateWindowW(L"BUTTON", app.test_mode ? L"Test local · 2 clients" : L"Test solo · ouvrir 2 clients", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)109, NULL, NULL);
        EnableWindow(app.updates,TRUE); EnableWindow(app.test,!app.test_mode);
        SendMessageW(app.connection, WM_SETFONT, (WPARAM)app.normal, TRUE);
        app.invite = CreateWindowW(L"BUTTON", L"Ajouter ou connecter un ami  →", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 0, 0, window, (HMENU)(INT_PTR)107, NULL, NULL);
        {
            int i;
            for (i = 0; i < 3; ++i) {
                WCHAR name[80];
                swprintf_s(name, 80, L"%ls", rooms[i]);
                app.rooms[i] = CreateWindowW(L"BUTTON", name,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    0, 0, 0, 0, window, (HMENU)(INT_PTR)(200 + i), NULL, NULL);
            }
        }
        SendMessageW(app.input, EM_SETCUEBANNER, TRUE, (LPARAM)L"Écrire un message…");
        SendMessageW(app.input, EM_SETLIMITTEXT, MAX_TEXT, 0);
        app.edit_proc = (WNDPROC)SetWindowLongPtrW(app.input, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        fonts();
        enhance_button(app.send); enhance_button(app.voice); enhance_button(app.mute);
        enhance_button(app.screen); enhance_button(app.connection); enhance_button(app.invite);
        enhance_button(app.updates);
        enhance_button(app.test);
        { int i; for (i = 0; i < 3; ++i) enhance_button(app.rooms[i]); }
        layout(window);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = px(820);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = px(760);
        return 0;
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lp;
        app.dpi = HIWORD(wp);
        fonts();
        SetWindowPos(window, NULL, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        layout(window);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    }
    case WM_SIZE:
        layout(window);
        InvalidateRect(window, NULL, FALSE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_VOICE && HIWORD(wp) == BN_CLICKED && link_voice_toggle())
            MessageBoxW(window, L"Connectez d’abord un proche avec Mes amis.\n"
                L"Vous devrez ensuite tous les deux rejoindre le vocal.", L"Vocal", MB_OK);
        if (LOWORD(wp) == ID_MUTE && HIWORD(wp) == BN_CLICKED) link_voice_mute_toggle();
        if (LOWORD(wp) == 105 && HIWORD(wp) == BN_CLICKED) friends_show(window);
        if (LOWORD(wp) == 107 && HIWORD(wp) == BN_CLICKED) friends_show(window);
        if (LOWORD(wp) == 108 && HIWORD(wp) == BN_CLICKED) settings_show(window);
        if (LOWORD(wp) == 109 && HIWORD(wp) == BN_CLICKED && local_test_launch(window))
            MessageBoxW(window,L"Impossible d’ouvrir le test local.",L"Direct · Test solo",MB_OK|MB_ICONWARNING);
        if (LOWORD(wp) >= 200 && LOWORD(wp) <= 202 && HIWORD(wp) == BN_CLICKED) {
            int i;
            app.room = LOWORD(wp) - 200;
            layout(window);
            for (i = 0; i < 3; ++i) InvalidateRect(app.rooms[i], NULL, FALSE);
            InvalidateRect(window, NULL, FALSE);
        }
        if (LOWORD(wp) == ID_SEND && HIWORD(wp) == BN_CLICKED) send_local(window);
        if (LOWORD(wp) == ID_SCREEN && HIWORD(wp) == BN_CLICKED)
            MessageBoxW(window, L"Le partage d’écran reste à intégrer.\nAucun écran n’est capturé.",
                L"Fonction en préparation", MB_OK | MB_ICONINFORMATION);
        return 0;
    case WM_TIMER:
        if (app.test_mode && wp==904) {
            local_test_tick();
            if (app.test_greeted && !link_voice_requested()) KillTimer(window,904);
            if (GetTickCount64()>app.test_deadline && !app.test_greeted && link_state()!=2) {
                app.test_failed=1; local_test_close(); link_stop(); KillTimer(window,904);
            }
            InvalidateRect(window,NULL,FALSE);
        }
        return 0;
    case WM_DIRECT_SIGNAL: if(!app.test_mode) friends_internet_event(); return 0;
    case WM_DIRECT_UPDATE: if (!app.test_mode) updates_refresh(); return 0;
    case WM_MURMUR_LINK: {
        link_message incoming;
        uint8_t current_peer[32];
        if(!app.test_mode && friends_peer_id(current_peer)) {
            if(app.have_peer && memcmp(app.last_peer,current_peer,32)) { SecureZeroMemory(app.messages,sizeof(app.messages)); app.count=0; }
            memcpy(app.last_peer,current_peer,32); app.have_peer=1;
        }
        while (link_receive(&incoming)) {
            WCHAR text[MAX_TEXT + 1];
            int length;
            if (memchr(incoming.text, 0, incoming.size)) continue;
            length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)incoming.text,
                (int)incoming.size, text, MAX_TEXT);
            if (length <= 0) continue;
            text[length] = 0;
            if (app.count == MAX_MESSAGES) {
                memmove(app.messages, app.messages + 1, sizeof(Message) * (MAX_MESSAGES - 1));
                --app.count;
            }
            wcscpy_s(app.messages[app.count].text, MAX_TEXT + 1, text);
            app.messages[app.count].room = (int)incoming.room;
            app.messages[app.count++].remote = 1;
            GetLocalTime(&app.messages[app.count-1].time);
    wcscpy_s(app.messages[app.count-1].author,64,app.messages[app.count-1].remote?peer_name():self_name());
        }
        SecureZeroMemory(&incoming, sizeof(incoming));
        if (app.test_mode && link_state()==2 && !app.test_greeted) {
            app.test_greeted=1;
            SetWindowTextW(app.input,app.test_mode==1 ? L"Bonjour depuis le client A. Ce message traverse la connexion chiffrée locale." : L"Bonjour depuis le client B. Vous pouvez écrire ici et tester l’appel simulé.");
            send_local(window);
        }
        SetWindowTextW(app.voice, link_voice_requested() ? (link_voice_state()==1?L"Annuler":L"Raccrocher") : (link_voice_remote()&1) ? L"Répondre" : app.test_mode ? L"Tester l’appel" : L"Appeler");
        SetWindowTextW(app.mute, link_voice_state() == 4 ? L"Activer le microphone" : L"Couper le microphone");
        EnableWindow(app.mute, link_voice_requested());
        EnableWindow(app.voice, link_state() == 2);
        EnableWindow(app.send, link_state() == 2);
        if (app.test_mode && link_voice_requested()) SetTimer(window,904,500,NULL);
        EnableWindow(app.test,!app.test_mode && !link_busy());
        SetWindowTextW(app.connection,link_state()==2 ? L"Amis · 1 en ligne" : L"Amis · ajouter et gérer");
        SetWindowTextW(app.rooms[0],peer_name());
        settings_refresh();
        layout(window);
        connection_refresh();
        connection_auto_refresh();
        friends_connection_refresh();
        InvalidateRect(window, NULL, FALSE);
        return 0;
    }
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, fg);
        SetBkColor((HDC)wp, field_color);
        return (LRESULT)app.field;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *item = (DRAWITEMSTRUCT *)lp;
        WCHAR text[80];
        int width = MulDiv(item->rcItem.right, 96, (int)app.dpi);
        int height = MulDiv(item->rcItem.bottom, 96, (int)app.dpi);
        BOOL primary = item->CtlID == ID_SEND || item->CtlID == 107 || item->CtlID == ID_VOICE;
        BOOL channel = item->CtlID >= 200 && item->CtlID <= 202;
        BOOL selected = channel && (int)(item->CtlID - 200) == app.room;
        BOOL hover = GetPropW(item->hwndItem, L"hover") && !(item->itemState & ODS_DISABLED);
        COLORREF surface = item->CtlID == ID_SEND ? field_color : item->CtlID == 107 || item->CtlID == 108 || item->CtlID == ID_SCREEN ? bg :
            item->CtlID == ID_VOICE || item->CtlID == ID_MUTE ? RGB(235, 240, 249) : sidebar;
        COLORREF fill = primary ? accent : selected ? RGB(228, 236, 255) : surface;
        if (hover) fill = primary ? RGB(79, 120, 255) : RGB(233, 238, 248);
        if (item->itemState & ODS_SELECTED) fill = primary ? RGB(43, 83, 210) : RGB(221, 230, 247);
        if (item->itemState & ODS_DISABLED) fill = item->CtlID == ID_SEND ? RGB(230, 234, 242) : surface;
        if(item->CtlID==ID_VOICE && !(item->itemState&ODS_DISABLED) && link_voice_requested()) fill=(hover || (item->itemState&ODS_SELECTED))?RGB(190,53,76):RGB(217,66,91);
        box(item->hDC, 0, 0, width, height, surface);
        rounded(item->hDC, 0, 0, width, height, 12,
            fill);
        GetWindowTextW(item->hwndItem, text, 80);
        if(item->CtlID==200) {
            rounded(item->hDC,10,15,32,32,32,RGB(235,220,255));
            label(item->hDC,app.normal,RGB(123,69,184),10,15,32,32,app.test_mode==1?L"B":L"A",DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            label(item->hDC,app.heading,fg,52,10,width-60,24,text,DT_SINGLELINE|DT_END_ELLIPSIS);
            label(item->hDC,app.caption_font,link_state()==2?RGB(36,114,83):muted,52,36,width-60,20,link_state()==2?L"● En ligne":L"○ Hors ligne",DT_SINGLELINE);
            if(item->itemState&ODS_FOCUS) { RECT focus=item->rcItem; InflateRect(&focus,-4,-4); DrawFocusRect(item->hDC,&focus); }
            return TRUE;
        }
        if (selected) rounded(item->hDC, 0, 9, 3, height-18, 3, accent);
        {
            enum direct_icon kind = channel ? (item->CtlID == 201 ? ICON_CALENDAR : item->CtlID == 202 ? ICON_GAME : ICON_MESSAGE) :
                item->CtlID == 105 || item->CtlID == 107 || item->CtlID == 109 ? ICON_USERS : item->CtlID == 108 ? ICON_SETTINGS : item->CtlID == ID_SEND ? ICON_SEND :
                item->CtlID == ID_VOICE ? (link_voice_requested() ? ICON_HANGUP : ICON_PHONE) :
                item->CtlID == ID_MUTE ? (link_voice_state() == 4 ? ICON_MIC_OFF : ICON_MIC) : ICON_MONITOR;
            COLORREF color = (item->itemState & ODS_DISABLED) ? muted : primary ? RGB(255, 255, 255) : selected ? accent : fg;
            icon(item->hDC, item->CtlID == ID_MUTE ? (width-18)/2 : channel ? 16 : 10, (height-18)/2, 18, kind, color);
        }
        if (item->CtlID != ID_MUTE) label(item->hDC, app.normal,
            (item->itemState & ODS_DISABLED) ? muted : primary ? RGB(255, 255, 255) : selected ? accent : hover || item->CtlID == 105 ? fg : muted, channel ? 44 : item->CtlID == ID_MUTE ? 0 : 30, 0,
            channel ? width - 48 : item->CtlID == ID_MUTE ? width : width-34, height, text,
            (channel ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE);
        if (item->itemState & ODS_FOCUS) {
            RECT focus = item->rcItem;
            InflateRect(&focus, -px(4), -px(4));
            DrawFocusRect(item->hDC, &focus);
        }
        return TRUE;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT bounds; HDC buffer; HBITMAP bitmap; HGDIOBJ old;
        GetClientRect(window, &bounds);
        buffer = CreateCompatibleDC(dc); bitmap = CreateCompatibleBitmap(dc, bounds.right, bounds.bottom);
        old = SelectObject(buffer, bitmap); paint(window, buffer);
        BitBlt(dc, 0, 0, bounds.right, bounds.bottom, buffer, 0, 0, SRCCOPY);
        SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer);
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DESTROY:
        settings_close();
        friends_close();
        if (!app.test_mode) updates_shutdown();
        else { KillTimer(window,904); local_test_close(); }
        link_stop();
        DeleteObject(app.normal);
        DeleteObject(app.title);
        DeleteObject(app.caption_font);
        DeleteObject(app.heading);
        direct_icons_clear();
        DeleteObject(app.background);
        DeleteObject(app.field);
        PostQuitMessage(0);
        return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command, int show) {
    WNDCLASSW wc = {0};
    HWND window;
    MSG message;
    BOOL result;
    (void)previous;
    app.test_mode=!wcscmp(command,L"--test-pair") ? 1 : !wcscmp(command,L"--test-peer") ? 2 : 0;
    if (updates_apply_command()) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    app.dpi = GetDpiForSystem();
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MurmurNativeWindow";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassW(&wc)) return 1;
    window = CreateWindowExW(0, wc.lpszClassName, app.test_mode ? (app.test_mode==1 ? L"Direct — Client A · test local" : L"Direct — Client B · test local") : L"Direct — Messages et appels",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        px(1220), px(820), NULL, NULL, instance, NULL);
    if (!window) return 1;
    {
        BOOL dark = FALSE;
        COLORREF caption = sidebar;
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
        { int backdrop = 2, corners = 2;
          DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
          DwmSetWindowAttribute(window, 33, &corners, sizeof(corners)); }
    }
    ShowWindow(window, show);
    if (app.test_mode) {
        MONITORINFO monitor={sizeof(monitor)}; int width,height,x,y;
        GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        width=(monitor.rcWork.right-monitor.rcWork.left)/2-12; height=monitor.rcWork.bottom-monitor.rcWork.top-24;
        x=monitor.rcWork.left+8+(app.test_mode==2 ? width+8 : 0); y=monitor.rcWork.top+12;
        if (width<px(820)) { width=px(900); x=monitor.rcWork.left+(app.test_mode==2 ? 120 : 24); y+=app.test_mode==2 ? 60 : 0; }
        SetWindowPos(window,NULL,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        app.test_deadline=GetTickCount64()+25000;
        if (local_test_start(window,app.test_mode==2)) { app.test_failed=1; MessageBoxW(window,L"Impossible de démarrer le test. Lancez Test solo depuis la fenêtre principale.",L"Direct · Test local",MB_OK|MB_ICONWARNING); }
        else SetTimer(window,904,250,NULL);
    }
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!settings_translate(&message) && !updates_translate(&message) && !friends_translate(&message) && !connection_auto_translate(&message) && !connection_translate(&message) && !IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result == -1 ? 1 : (int)message.wParam;
}
