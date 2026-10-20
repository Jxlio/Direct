#include "connect.h"
#include "link.h"
#include "panel_style.h"
#include <commctrl.h>
#include <wchar.h>
#include <string.h>
static HWND panel, incoming, outgoing, status, stun, stun_port, turn, turn_port, username, password;
static HFONT font;
static HWND control(HWND window, const WCHAR *kind, const WCHAR *text, DWORD style,
                    int x, int y, int w, int h, int id) {
    HWND item;
    if (!wcscmp(kind, L"BUTTON")) style |= BS_OWNERDRAW;
    item = CreateWindowExW(0, kind, text,
        WS_CHILD | WS_VISIBLE | style, x, y, w, h, window, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(item, WM_SETFONT, (WPARAM)font, TRUE);
    return item;
}
int connection_auto_translate(MSG *message) { return panel && IsDialogMessageW(panel, message); }
void connection_auto_refresh(void) {
    char token[MURMUR_ICE_TOKEN] = {0};
    WCHAR text[MURMUR_ICE_TOKEN], previous[MURMUR_ICE_TOKEN];
    WCHAR failure[512];
    const WCHAR *description = L"Créez une invitation, ou répondez à celle de votre proche.";
    int state, phase;
    if (!panel) return;
    if (link_ice_mode()) {
        if (link_ice_token(token, sizeof(token)) >= 0) {
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, token, -1, text, MURMUR_ICE_TOKEN);
            GetWindowTextW(outgoing, previous, MURMUR_ICE_TOKEN);
            if (wcscmp(text, previous)) SetWindowTextW(outgoing, text);
        }
        state = link_state(); phase = link_ice_phase();
        if (state == 3) {
            int error = link_error();
            const WCHAR *reason = (error == -10 || error == -11) ?
                L"Aucun chemin réseau établi. Les box ou le pare-feu peuvent bloquer la liaison ; un relais TURN peut être nécessaire." :
                error == -21 ? L"Le chemin réseau a été trouvé, mais l’authentification a expiré. Recréez l’invitation et échangez les deux codes rapidement." :
                L"La connexion a échoué. Réessayez avec une nouvelle invitation et conservez ce code pour le diagnostic.";
            swprintf_s(failure, 512, L"Échec · code %d. %ls", error, reason); description = failure;
        }
        else if (state == 4) description = L"Déconnexion en cours…";
        else if (!state) description = L"Hors ligne.";
        else if (state == 2) description = link_ice_relayed() ? L"Connecté via un relais · chiffré de bout en bout." : L"Connecté directement · chiffré de bout en bout.";
        else if (phase == -2) description = L"Réponse invalide : collez la réponse correspondant à cette invitation.";
        else if (!token[0]) description = L"Recherche des chemins réseau : préparation de votre invitation/réponse…";
        else if (phase == 1) description = L"Partagez votre invitation. Collez ensuite la réponse reçue, puis cliquez Appliquer la réponse.";
        else if (phase == 2) description = L"Vérification des chemins directs et du relais configuré…";
        else description = L"Authentification en cours. Si vous avez répondu, transmettez votre réponse au proche.";
    }
    SetWindowTextW(status, description);
    SecureZeroMemory(token, sizeof(token)); SecureZeroMemory(text, sizeof(text)); SecureZeroMemory(previous, sizeof(previous));
}
static int read_text(HWND item, char *out, int capacity) {
    WCHAR text[MURMUR_ICE_TOKEN];
    int result;
    GetWindowTextW(item, text, MURMUR_ICE_TOKEN);
    result = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, out, capacity, NULL, NULL);
    SecureZeroMemory(text, sizeof(text));
    return result ? 0 : -1;
}
static void action(int id) {
    murmur_ice_config settings = {0};
    char token[MURMUR_ICE_TOKEN] = {0};
    BOOL valid;
    UINT port;
    int result = -1;
    if (id == 405) { link_stop(); connection_auto_refresh(); return; }
    if (id == 406) { connection_show(panel); return; }
    if (id == 403) {
        if (!read_text(incoming, token, sizeof(token))) result = link_ice_answer(token);
        goto done;
    }
    if (link_busy()) {
        MessageBoxW(panel, L"Déconnectez la session actuelle avant d’en créer une nouvelle.", L"Session en cours", MB_OK);
        return;
    }
    if (read_text(stun, settings.stun_host, sizeof(settings.stun_host)) ||
        read_text(turn, settings.turn_host, sizeof(settings.turn_host)) ||
        read_text(username, settings.username, sizeof(settings.username)) ||
        read_text(password, settings.password, sizeof(settings.password))) goto done;
    port = GetDlgItemInt(panel, 411, &valid, FALSE);
    if (!valid || !port || port > 65535) goto done;
    settings.stun_port = (uint16_t)port;
    port = GetDlgItemInt(panel, 413, &valid, FALSE);
    if (!valid || !port || port > 65535) goto done;
    settings.turn_port = (uint16_t)port;
    if (id == 402 && read_text(incoming, token, sizeof(token))) goto done;
    result = link_ice_start(id == 401, &settings, id == 402 ? token : NULL);
done:
    SecureZeroMemory(token, sizeof(token)); SecureZeroMemory(&settings, sizeof(settings));
    if (result) MessageBoxW(panel, L"Impossible de démarrer. Vérifiez les paramètres et le code reçu.\n"
        L"« Appliquer la réponse » sert uniquement à appliquer une réponse à votre invitation en cours.", L"Connexion", MB_OK | MB_ICONWARNING);
    connection_auto_refresh();
}
static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_CREATE:
        font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        control(window, L"STATIC", L"Se connecter à un ami sur Internet", 0, 24, 20, 640, 26, 0);
        control(window, L"STATIC", L"Créez une invitation et envoyez-la en privé. Votre ami répond avec son code.\nCollez sa réponse, puis cliquez Appliquer la réponse.", 0, 24, 56, 640, 48, 0);
        control(window, L"BUTTON", L"Créer une invitation", WS_TABSTOP, 24, 118, 194, 40, 401);
        control(window, L"BUTTON", L"Répondre à l’invitation", WS_TABSTOP, 238, 118, 194, 40, 402);
        control(window, L"BUTTON", L"Appliquer la réponse", WS_TABSTOP, 452, 118, 202, 40, 403);
        control(window, L"STATIC", L"Invitation ou réponse reçue de votre ami", 0, 24, 180, 630, 22, 0);
        incoming = control(window, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, 24, 208, 630, 70, 420);
        control(window, L"STATIC", L"Votre code à envoyer en privé · Ctrl+A puis Ctrl+C", 0, 24, 298, 630, 22, 0);
        outgoing = control(window, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 24, 326, 630, 70, 421);
        SendMessageW(incoming, EM_SETLIMITTEXT, MURMUR_ICE_TOKEN - 1, 0);
        status = control(window, L"STATIC", L"", 0, 24, 414, 630, 66, 422);
        control(window, L"BUTTON", L"Déconnecter", WS_TABSTOP, 24, 488, 180, 34, 405);
        control(window, L"BUTTON", L"Connexion par adresse IP…", WS_TABSTOP, 422, 488, 232, 34, 406);
        control(window, L"STATIC", L"PARAMÈTRES RÉSEAU · gardez les valeurs par défaut pour un premier essai", 0, 24, 548, 630, 24, 0);
        control(window, L"STATIC", L"Découverte d’adresse (STUN)", 0, 24, 580, 310, 22, 0);
        stun = control(window, L"EDIT", L"stun.cloudflare.com", WS_TABSTOP | ES_AUTOHSCROLL, 24, 606, 310, 28, 410);
        stun_port = control(window, L"EDIT", L"3478", WS_TABSTOP | ES_NUMBER, 346, 606, 74, 28, 411);
        control(window, L"STATIC", L"Relais de secours (TURN) · facultatif", 0, 24, 650, 630, 22, 0);
        turn = control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 24, 676, 310, 28, 412);
        turn_port = control(window, L"EDIT", L"3478", WS_TABSTOP | ES_NUMBER, 346, 676, 74, 28, 413);
        username = control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 24, 720, 210, 28, 414);
        password = control(window, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 250, 720, 404, 28, 415);
        SendMessageW(username, EM_SETCUEBANNER, TRUE, (LPARAM)L"Identifiant du relais");
        SendMessageW(password, EM_SETCUEBANNER, TRUE, (LPARAM)L"Mot de passe du relais");
        SendMessageW(stun, EM_SETLIMITTEXT, 255, 0); SendMessageW(turn, EM_SETLIMITTEXT, 255, 0);
        SendMessageW(username, EM_SETLIMITTEXT, 127, 0); SendMessageW(password, EM_SETLIMITTEXT, 127, 0);
        SendMessageW(stun_port, EM_SETLIMITTEXT, 5, 0); SendMessageW(turn_port, EM_SETLIMITTEXT, 5, 0);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED && LOWORD(wp) >= 401 && LOWORD(wp) <= 406) action(LOWORD(wp));
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT: return direct_panel_color(message, wp, lp);
    case WM_DRAWITEM: return direct_panel_button((DRAWITEMSTRUCT *)lp, font);
    case WM_CLOSE: DestroyWindow(window); return 0;
    case WM_DESTROY:
        DeleteObject(font); panel = NULL; return 0;
    default: return DefWindowProcW(window, message, wp, lp);
    }
}
void connection_auto_show(HWND parent) {
    WNDCLASSW wc = {0};
    if (panel) { SetForegroundWindow(panel); return; }
    wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"MurmurAutomaticConnection";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassW(&wc);
    panel = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Direct — Connexion Internet",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 704, 820, parent, NULL, wc.hInstance, NULL);
    connection_auto_refresh(); ShowWindow(panel, SW_SHOW);
}
