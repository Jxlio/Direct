#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#include <wctype.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include "link.h"
#include "datadir.h"
#include "friends.h"
#include "updates.h"
#include "local_test.h"
#include "settings.h"
#include "signaling.h"
#include "screen.h"
#include "media.h"
#include "identity_store.h"
#include "service_link.h"
#include "web_ui.h"
#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

/* UI state belongs to the main thread. No idle render loop.
   Network/audio transport runs on a separate owning thread. */
#define ID_VOICE 103
#define ID_MUTE 106
#define ID_TEST 109
#define ID_ATTACH 110
#define MAX_MESSAGES 128
#define MAX_TEXT 2048
typedef struct { WCHAR text[MAX_TEXT + 1]; uint32_t uid; int room, remote; WCHAR author[64]; SYSTEMTIME time; uint32_t media_id; unsigned media_kind; int media_pending; } Message;
static struct {
    HWND window;
    Message messages[MAX_MESSAGES];
    size_t count;
    int room;
    UINT dpi;
    int minimized, keepalive, unread;
    int test_mode, test_greeted, test_failed, custom_frame, record_mode, record_started;
    ULONGLONG test_deadline;
    ULONGLONG record_deadline;
    WCHAR record_dir[MAX_PATH];
    uint32_t sending_media_id, next_uid;
    uint8_t last_peer[32]; int have_peer;
    WCHAR invite_code[512], network_notice[256], identity_text[24], identity_public[131], identity_enc[131];
} app;
static const WCHAR *peer_name(void) { return app.test_mode ? (app.test_mode==1 ? L"Client B" : L"Client A") : friends_peer_name()?friends_peer_name():L"Votre ami"; }
static const WCHAR *self_name(void) { return app.test_mode ? (app.test_mode==1 ? L"Client A · vous" : L"Client B · vous") : L"Vous"; }
static const WCHAR *connection_description(void);
static const WCHAR *voice_description(void);
static void write_record_voice(void) {
    WCHAR path[MAX_PATH]; char report[160]; DWORD written;
    HANDLE file;
    if(!app.record_dir[0]) return;
    swprintf_s(path,MAX_PATH,L"%ls\\%ls",app.record_dir,app.test_mode==1?L"sender-voice.txt":L"receiver-voice.txt");
    file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return;
    {
        int count=snprintf(report,sizeof(report),"voice_state=%d\nreceived_packets=%u\ndecoded_frames=%llu\n",
            link_voice_state(),link_voice_received_packets(),(unsigned long long)link_voice_decoded_frames());
        if(count>0) WriteFile(file,report,(DWORD)count,&written,NULL);
    }
    CloseHandle(file);
}
/* "Ouvrir le dossier" on the appearance card: the folder holding user.css, which is created with a
   commented example the first time so there is something to start from. */
static void open_theme_folder(HWND window) {
    static const char example[] =
        "/* Murmur: votre feuille de style personnelle. Elle s'applique par-dessus le theme choisi. */\r\n"
        "/* Exemple : decommentez pour changer les couleurs de base.\r\n"
        ":root {\r\n"
        "  --bg-main: #1e2a22;\r\n"
        "  --bg-side: #17211b;\r\n"
        "  --accent: #7bd88f;\r\n"
        "}\r\n"
        "*/\r\n";
    WCHAR folder[MAX_PATH], file[MAX_PATH]; HANDLE handle; DWORD written;
    if (FAILED(murmur_local_appdata(folder)) || wcslen(folder) + 24 >= MAX_PATH) return;
    wcscat_s(folder, MAX_PATH, L"\\Murmur"); CreateDirectoryW(folder, NULL);
    swprintf_s(file, MAX_PATH, L"%ls\\user.css", folder);
    handle = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (handle != INVALID_HANDLE_VALUE) { WriteFile(handle, example, (DWORD)sizeof(example) - 1, &written, NULL); CloseHandle(handle); }
    ShellExecuteW(window, L"open", folder, NULL, NULL, SW_SHOWNORMAL);
}
/* Unread messages: the title counts them and, when the window is not in front, the taskbar button
   flashes. Only a number crosses over, never a word of any message. */
static void show_unread(HWND window, int count) {
    WCHAR title[96];
    if (count < 0 || count > 999) count = count < 0 ? 0 : 999;
    if (count > app.unread && GetForegroundWindow() != window) {
        FLASHWINFO flash = {sizeof(flash), window, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&flash);
    }
    app.unread = count;
    if (app.test_mode) return;
    if (count) swprintf_s(title, 96, L"(%d) Murmur — Messages et appels", count); else wcscpy_s(title, 96, L"Murmur — Messages et appels");
    SetWindowTextW(window, title);
}
/* A minimized window needs no rendering. With no call in progress the page is suspended,
   which frees most of its memory; a call (or an incoming one) wakes it again. */
static void apply_background(void) {
    /* A page that is connected to a server keeps listening, so messages and calls still arrive. */
    int call = link_voice_requested() || (link_voice_remote() & 1) || app.keepalive;
    web_ui_set_background(app.minimized, !call);
}
static void web_ui_sync(void) {
    WebUiMessage messages[MAX_MESSAGES];
    WebUiFriend friends[16];
    WebUiAudio audio;
    const direct_audio_web_state *native_audio;
    WebUiState state;
    char network_utf8[MURMUR_ICE_TOKEN]={0};
    WCHAR network_token[MURMUR_ICE_TOKEN]={0};
    WCHAR update_status[512]={0};
    size_t i;
    if (!web_ui_active()) return;
    for (i = 0; i < app.count; ++i) {
        messages[i].text = app.messages[i].text;
        messages[i].uid = app.messages[i].uid;
        messages[i].author = app.messages[i].author;
        messages[i].room = app.messages[i].room;
        messages[i].remote = app.messages[i].remote;
        messages[i].hour = app.messages[i].time.wHour;
        messages[i].minute = app.messages[i].time.wMinute;
        messages[i].media_id = app.messages[i].media_id;
        messages[i].media_kind = app.messages[i].media_kind;
        messages[i].media_pending = app.messages[i].media_pending;
        if(messages[i].media_id && !media_get(messages[i].media_id,NULL,NULL,NULL)) messages[i].media_id=0;
    }
    ZeroMemory(&state, sizeof(state));
    ZeroMemory(&audio,sizeof(audio));
    native_audio=settings_web_state();
    audio.input_count=native_audio->input_count; audio.output_count=native_audio->output_count;
    for(i=0;i<(size_t)audio.input_count;++i) audio.inputs[i]=native_audio->inputs[i];
    for(i=0;i<(size_t)audio.output_count;++i) audio.outputs[i]=native_audio->outputs[i];
    audio.input_choice=native_audio->input_choice; audio.output_choice=native_audio->output_choice;
    audio.input_volume=native_audio->input_volume; audio.output_volume=native_audio->output_volume;
    audio.preview_active=native_audio->preview_active; audio.preview_level=native_audio->preview_level;
    audio.locked=native_audio->locked; audio.sandbox=native_audio->sandbox; audio.notice=native_audio->notice;
    audio.echo=(int)native_audio->echo; audio.noise=(int)native_audio->noise; audio.agc=(int)native_audio->agc; audio.gate=(int)native_audio->gate; audio.cleanup_available=native_audio->cleanup_available;
    state.audio=&audio;
    state.friend_count=friends_web_count();
    if(state.friend_count>16) state.friend_count=16;
    for(i=0;i<state.friend_count;++i) { friends[i].name=friends_web_name((unsigned)i); friends[i].online=friends_web_online((unsigned)i); }
    state.friends=friends; state.friends_notice=friends_web_notice();
    state.internet_enabled=friends_web_internet_enabled();
    state.invite_code=app.invite_code; state.network_notice=app.network_notice; state.identity=app.identity_text; state.identity_public=app.identity_public; state.identity_enc=app.identity_enc;
    if(link_ice_mode() && link_ice_token(network_utf8,sizeof(network_utf8))>=0)
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,network_utf8,-1,network_token,MURMUR_ICE_TOKEN);
    state.network_token=network_token;
    state.network_phase=link_ice_mode()?link_ice_phase():0;
    state.network_error=link_error();
    if(!app.test_mode) updates_web_snapshot(update_status,&state.update_available,&state.update_busy);
    state.update_status=update_status;
    state.peer = peer_name(); state.self = self_name();
    state.connection = connection_description(); state.voice = voice_description();
    state.messages = messages; state.message_count = app.count;
    state.room = app.room; state.connected = link_state() == 2;
    state.connecting = link_state() == 1; state.call_requested = link_voice_requested();
    state.call_incoming = (link_voice_remote() & 1) != 0;
    state.muted = link_voice_state() == 4;
    state.screen_enabled = link_state() == 2 && link_ice_mode();
    state.sharing = screen_sharing(); state.remote_screen = screen_remote();
    state.test_mode = app.test_mode;
    state.media_busy = app.sending_media_id!=0;
    web_ui_update(&state);
    SecureZeroMemory(network_utf8,sizeof(network_utf8)); SecureZeroMemory(network_token,sizeof(network_token));
}
static int px(int n) { return MulDiv(n, (int)app.dpi, 96); }
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
static void send_text(HWND window, const WCHAR *text) {
    uint8_t utf8[MURMUR_CHAT_LIMIT];
    size_t i;
    int length = (int)wcsnlen(text, MAX_TEXT + 1);
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
    ZeroMemory(&app.messages[app.count],sizeof(Message)); app.messages[app.count].uid=++app.next_uid;
    wcsncpy_s(app.messages[app.count].text, MAX_TEXT + 1, text, (size_t)length);
    app.messages[app.count++].room = app.room;
    app.messages[app.count - 1].remote = 0;
    GetLocalTime(&app.messages[app.count-1].time);
    wcscpy_s(app.messages[app.count-1].author,64,app.messages[app.count-1].remote?peer_name():self_name());
    web_ui_sync();
}
static void append_media_message(HWND window,uint32_t id,unsigned kind,const WCHAR *label,int remote,int pending) {
    Message *item;
    if(app.count==MAX_MESSAGES) { memmove(app.messages,app.messages+1,sizeof(Message)*(MAX_MESSAGES-1)); --app.count; }
    item=&app.messages[app.count++]; ZeroMemory(item,sizeof(*item)); item->uid=++app.next_uid;
    wcsncpy_s(item->text,MAX_TEXT+1,label,_TRUNCATE);
    item->room=0; item->remote=remote; item->media_id=id; item->media_kind=kind; item->media_pending=pending;
    wcscpy_s(item->author,64,remote?peer_name():self_name());
    GetLocalTime(&item->time);
    InvalidateRect(window,NULL,FALSE); web_ui_sync();
}
static int web_utf8(const WCHAR *source,char *dest,int capacity) {
    return source && WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,source,-1,dest,capacity,NULL,NULL)>0?0:-1;
}
static void web_network_action(const WCHAR *action,const WCHAR *text) {
    WCHAR copy[13000],*fields[7],*cursor; int count,i,reply,result=-1;
    murmur_ice_config config={0}; char token[MURMUR_ICE_TOKEN]={0}; unsigned long port;
    if(!wcscmp(action,L"net-apply")) {
        if(!web_utf8(text,token,sizeof(token))) result=link_ice_answer(token);
        wcscpy_s(app.network_notice,256,result?L"Réponse invalide ou session expirée.":L"Réponse appliquée. Connexion en cours…");
        SecureZeroMemory(token,sizeof(token)); web_ui_sync(); return;
    }
    reply=!wcscmp(action,L"net-reply"); count=reply?7:6;
    if(!text || wcslen(text)>=sizeof(copy)/sizeof(copy[0])) return;
    wcscpy_s(copy,13000,text); fields[0]=copy;
    for(i=1;i<count;++i) { cursor=wcschr(fields[i-1],L'\n'); if(!cursor) goto done; *cursor=0; fields[i]=cursor+1; }
    i=reply?1:0;
    if(web_utf8(fields[i],config.stun_host,sizeof(config.stun_host)) ||
       web_utf8(fields[i+2],config.turn_host,sizeof(config.turn_host)) ||
       web_utf8(fields[i+4],config.username,sizeof(config.username)) ||
       web_utf8(fields[i+5],config.password,sizeof(config.password))) goto done;
    port=wcstoul(fields[i+1],NULL,10); if(!port || port>65535) goto done; config.stun_port=(uint16_t)port;
    port=wcstoul(fields[i+3],NULL,10); if(!port || port>65535) goto done; config.turn_port=(uint16_t)port;
    if(reply && web_utf8(fields[0],token,sizeof(token))) goto done;
    if(!link_busy()) result=link_ice_start(!reply,&config,reply?token:NULL);
done:
    wcscpy_s(app.network_notice,256,result?L"Connexion impossible. Vérifiez le code et les paramètres réseau.":L"Invitation créée. Attendez le code à partager ci-dessous.");
    SecureZeroMemory(copy,sizeof(copy)); SecureZeroMemory(token,sizeof(token)); SecureZeroMemory(&config,sizeof(config));
    web_ui_sync();
}
/* The only kinds of text the page may have signed: each starts with its protocol domain, so a signature
   made for one purpose can never be mistaken for another, and arbitrary text is never signed. */
static int signable_domain(const char *message) {
    static const char *const domains[] = { "murmur/v2/auth|", "murmur/v2/envelope|", "murmur/v2/enc|", "murmur/v2/name|",
        "murmur/v2/role|", "murmur/v2/owner|", "murmur/v2/voice|" };
    size_t i;
    for (i = 0; i < sizeof(domains) / sizeof(domains[0]); ++i) if (!strncmp(message, domains[i], strlen(domains[i]))) return 1;
    return 0;
}
/* The page asks for signatures but never sees the key. Only the two protocol domains are signed,
   so a script cannot make the user's identity sign arbitrary data. */
static void web_identity_sign(const WCHAR *text) {
    static const char digits[] = "0123456789abcdef";
    const WCHAR *bar = text ? wcschr(text, L'|') : NULL;
    char id[17] = {0}, message[24100]; uint8_t signature[MURMUR_ID_SIGNATURE]; char hex[MURMUR_ID_SIGNATURE * 2 + 1];
    size_t i, id_length;
    int length;
    if (!bar || (id_length = (size_t)(bar - text)) == 0 || id_length > 16) return;
    for (i = 0; i < id_length; ++i) { if (text[i] > 127 || !isalnum((unsigned char)text[i])) return; id[i] = (char)text[i]; }
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, bar + 1, -1, message, sizeof(message), NULL, NULL);
    if (length <= 1 || !signable_domain(message) ||
        identity_store_sign((const uint8_t *)message, (size_t)length - 1, signature)) { web_ui_signature(id, NULL); return; }
    for (i = 0; i < sizeof(signature); ++i) { hex[i * 2] = digits[signature[i] >> 4]; hex[i * 2 + 1] = digits[signature[i] & 15]; }
    hex[sizeof(hex) - 1] = 0;
    web_ui_signature(id, hex);
}
/* Key agreement for the group key: "id|peer encryption key (hex)". The page turns the secret into
   keys with HKDF; the identity itself never leaves this module. */
static void web_identity_ecdh(const WCHAR *text) {
    static const char digits[] = "0123456789abcdef";
    const WCHAR *bar = text ? wcschr(text, L'|') : NULL;
    char id[17] = {0}; uint8_t peer[MURMUR_ID_PUBLIC], shared[32]; char hex[65];
    size_t i, id_length;
    if (!bar || (id_length = (size_t)(bar - text)) == 0 || id_length > 16 || wcslen(bar + 1) != MURMUR_ID_PUBLIC * 2) return;
    for (i = 0; i < id_length; ++i) { if (text[i] > 127 || !isalnum((unsigned char)text[i])) return; id[i] = (char)text[i]; }
    for (i = 0; i < MURMUR_ID_PUBLIC; ++i) {
        WCHAR high = bar[1 + i * 2], low = bar[2 + i * 2]; int a, b;
        a = high >= L'0' && high <= L'9' ? high - L'0' : high >= L'a' && high <= L'f' ? high - L'a' + 10 : -1;
        b = low >= L'0' && low <= L'9' ? low - L'0' : low >= L'a' && low <= L'f' ? low - L'a' + 10 : -1;
        if (a < 0 || b < 0) { web_ui_ecdh(id, NULL); return; }
        peer[i] = (uint8_t)(a << 4 | b);
    }
    if (identity_store_ecdh(peer, shared)) { web_ui_ecdh(id, NULL); return; }
    for (i = 0; i < 32; ++i) { hex[i * 2] = digits[shared[i] >> 4]; hex[i * 2 + 1] = digits[shared[i] & 15]; }
    hex[64] = 0;
    web_ui_ecdh(id, hex);
    SecureZeroMemory(shared, sizeof(shared)); SecureZeroMemory(hex, sizeof(hex));
}
/* "slot|origin|guild" opens a server connection; "slot|frame" sends; "slot" closes. */
static void web_service_action(const WCHAR *action, const WCHAR *text) {
    static char utf8[74100]; int slot, length;
    if (!text || !text[0] || text[0] < L'0' || text[0] >= L'0' + SERVICE_SLOTS) return;
    slot = text[0] - L'0';
    if (!wcscmp(action, L"service-close")) { service_link_close(slot); return; }
    if (text[1] != L'|') return;
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text + 2, -1, utf8, sizeof(utf8), NULL, NULL);
    if (length <= 1) return;
    if (!wcscmp(action, L"service-send")) { service_link_send(slot, utf8, (size_t)length - 1); return; }
    {
        char *split = strchr(utf8, '|');
        if (!split) { web_ui_service_event(slot, "close", 1008, NULL); return; }
        *split = 0;
        if (service_link_open(slot, utf8, split + 1)) web_ui_service_event(slot, "close", 1008, NULL);
    }
}
static void web_ui_action(const wchar_t *action, const wchar_t *text) {
    HWND window = app.window;
    int id = 0;
    if (!wcscmp(action, L"ready")) {
        if (!app.custom_frame) {
            app.custom_frame = 1;
            SetWindowPos(window, NULL, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        web_ui_resize(window); web_ui_sync(); return;
    }
    if (!wcscmp(action, L"window-minimize")) { ShowWindow(window, SW_MINIMIZE); return; }
    if (!wcscmp(action, L"window-maximize")) { ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE); return; }
    if (!wcscmp(action, L"window-close")) { PostMessageW(window, WM_CLOSE, 0, 0); return; }
    if (!wcscmp(action, L"window-drag")) {
        ReleaseCapture();
        SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return;
    }
    if (!wcscmp(action, L"send")) {
        if (text && wcslen(text) <= MAX_TEXT) send_text(window, text);
        return;
    }
    if (!wcscmp(action,L"attach")) { SendMessageW(window,WM_COMMAND,MAKEWPARAM(ID_ATTACH,BN_CLICKED),0); return; }
    if (!wcscmp(action,L"screen-signal") && text && link_state()==2 && link_voice_requested()) {
        size_t length=wcslen(text); uint8_t packet[MURMUR_CHAT_LIMIT];
        int written;
        if(!length || length>MURMUR_CHAT_LIMIT-4) return;
        packet[0]=0; packet[1]='W'; packet[2]='R'; packet[3]='1';
        written=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,(int)length,
            (char *)packet+4,MURMUR_CHAT_LIMIT-4,NULL,NULL);
        if(written>0) link_send(2,packet,(size_t)written+4);
        SecureZeroMemory(packet,sizeof(packet));
        return;
    }
    if (!wcscmp(action,L"open-url") && text && wcslen(text)<=2048 &&
        (!_wcsnicmp(text,L"https://",8) || !_wcsnicmp(text,L"http://",7))) {
        const WCHAR *p=text;
        while(*p && *p>=32 && *p!=127) ++p;
        if(!*p) ShellExecuteW(window,L"open",text,NULL,NULL,SW_SHOWNORMAL);
        return;
    }
    if (!wcscmp(action,L"webview-unavailable")) {
        MessageBoxW(window,L"Murmur a besoin du composant Microsoft Edge WebView2 pour afficher son interface.\n"
            L"Installez-le depuis microsoft.com/edge/webview2 puis relancez Murmur.",L"Murmur",MB_OK|MB_ICONWARNING);
        PostMessageW(window,WM_CLOSE,0,0); return;
    }
    if (!wcscmp(action,L"unread")) { show_unread(window, text ? (int)wcstol(text, NULL, 10) : 0); return; }
    if (!wcscmp(action,L"keepalive")) { app.keepalive = text && text[0] == L'1'; if (app.minimized) apply_background(); return; }
    if (!wcscmp(action,L"theme-open")) { open_theme_folder(window); return; }
    if (!wcscmp(action,L"identity-sign")) { web_identity_sign(text); return; }
    if (!wcscmp(action,L"identity-ecdh")) { web_identity_ecdh(text); return; }
    if (!wcscmp(action,L"service-open") || !wcscmp(action,L"service-send") || !wcscmp(action,L"service-close")) { web_service_action(action,text); return; }
    if (!wcscmp(action,L"friends")) { web_ui_sync(); return; }
    if (!wcscmp(action,L"settings") || !wcscmp(action,L"audio-enumerate")) { settings_web_enumerate(); web_ui_sync(); return; }
    if (!wcscmp(action,L"friend-create")) {
        app.invite_code[0]=0; friends_web_create(text,app.invite_code); web_ui_sync(); return;
    }
    if (!wcscmp(action,L"friend-add")) {
        const WCHAR *split=wcschr(text,L'\n');
        if(split && split-text<64) { WCHAR name[64]; wcsncpy_s(name,64,text,(size_t)(split-text)); friends_web_add(name,split+1); SecureZeroMemory(name,sizeof(name)); }
        web_ui_sync(); return;
    }
    if (!wcscmp(action,L"friend-code") || !wcscmp(action,L"friend-connect") || !wcscmp(action,L"friend-forget")) {
        unsigned index=(unsigned)wcstoul(text,NULL,10);
        if(!wcscmp(action,L"friend-code")) { app.invite_code[0]=0; friends_web_code(index,app.invite_code); }
        if(!wcscmp(action,L"friend-connect")) friends_web_connect(index);
        if(!wcscmp(action,L"friend-forget")) { friends_web_forget(index); app.invite_code[0]=0; }
        web_ui_sync(); return;
    }
    if (!wcscmp(action,L"friend-disconnect")) { friends_web_disconnect(); web_ui_sync(); return; }
    if (!wcscmp(action,L"audio-input") || !wcscmp(action,L"audio-output")) {
        settings_web_select(!wcscmp(action,L"audio-input"),(int)wcstol(text,NULL,10)); web_ui_sync(); return;
    }
    if (!wcscmp(action,L"audio-in-volume") || !wcscmp(action,L"audio-out-volume")) {
        settings_web_volume(!wcscmp(action,L"audio-in-volume"),(unsigned)wcstoul(text,NULL,10)); web_ui_sync(); return;
    }
    if (!wcscmp(action,L"audio-cleanup")) {
        unsigned v[4]={0,0,0,0}; int i=0; const WCHAR *p=text;
        while(p && *p && i<4) { v[i++]=(unsigned)wcstoul(p,(WCHAR **)&p,10); if(*p==L'|') ++p; else break; }
        if(i==4) settings_web_cleanup(v[0],v[1],v[2],v[3]);
        web_ui_sync(); return;
    }
    if (!wcscmp(action,L"audio-preview-in") || !wcscmp(action,L"audio-preview-out")) {
        settings_web_preview(window,!wcscmp(action,L"audio-preview-in")); web_ui_sync(); return;
    }
    if (!wcscmp(action,L"net-invite") || !wcscmp(action,L"net-reply") || !wcscmp(action,L"net-apply")) {
        web_network_action(action,text); return;
    }
    if (!wcscmp(action,L"net-disconnect")) { link_stop(); wcscpy_s(app.network_notice,256,L"Connexion arrêtée."); web_ui_sync(); return; }
    if (!wcscmp(action,L"update-check") && !app.test_mode) { updates_web_check(); web_ui_sync(); return; }
    if (!wcscmp(action,L"update-download") && !app.test_mode) { updates_web_download(); web_ui_sync(); return; }
    if (!wcscmp(action, L"voice")) id = ID_VOICE;
    else if (!wcscmp(action, L"mute")) id = ID_MUTE;
    else if (!wcscmp(action, L"test")) id = ID_TEST;
    if (id) SendMessageW(window, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), 0);
}
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    switch (message) {
    case WM_NCCALCSIZE:
        if (app.custom_frame && wp) {
            NCCALCSIZE_PARAMS *params = (NCCALCSIZE_PARAMS *)lp;
            if (IsZoomed(window)) {
                MONITORINFO monitor = {sizeof(monitor)};
                if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
                    params->rgrc[0] = monitor.rcWork;
            }
            return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    case WM_NCHITTEST:
        if (app.custom_frame && !IsZoomed(window)) {
            RECT bounds;
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int edge = GetSystemMetricsForDpi(SM_CXSIZEFRAME, app.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, app.dpi);
            int left, right, top, bottom;
            GetWindowRect(window, &bounds);
            left = x < bounds.left + edge; right = x >= bounds.right - edge;
            top = y < bounds.top + edge; bottom = y >= bounds.bottom - edge;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        return DefWindowProcW(window, message, wp, lp);
    case WM_CREATE:
        screen_init(window);
        screen_test_mode(app.test_mode!=0);
        link_init(window);
        settings_init(app.test_mode);
        if (app.test_mode) friends_init_test(window,peer_name());
        else { friends_init(window); updates_init(window); }
        app.window = window;
        if (!app.test_mode && !identity_store_open_default()) {
            /* Show the first 16 hex digits in groups of four: enough to compare by voice. */
            const char *fingerprint = identity_store_fingerprint(); int i, n = 0;
            for (i = 0; i < 16; ++i) { if (i && !(i & 3)) app.identity_text[n++] = L' '; app.identity_text[n++] = (WCHAR)fingerprint[i]; }
            app.identity_text[n] = 0;
            { const uint8_t *key = identity_store_public(); static const WCHAR digits[] = L"0123456789abcdef";
              for (i = 0; i < (int)MURMUR_ID_PUBLIC; ++i) { app.identity_public[i * 2] = digits[key[i] >> 4]; app.identity_public[i * 2 + 1] = digits[key[i] & 15]; }
              app.identity_public[MURMUR_ID_PUBLIC * 2] = 0;
              key = identity_store_enc_public();
              for (i = 0; i < (int)MURMUR_ID_PUBLIC; ++i) { app.identity_enc[i * 2] = digits[key[i] >> 4]; app.identity_enc[i * 2 + 1] = digits[key[i] & 15]; }
              app.identity_enc[MURMUR_ID_PUBLIC * 2] = 0; }
        }
        app.dpi = GetDpiForWindow(window);
        web_ui_set_media_provider(media_get);
        service_link_init(window);
        web_ui_init(window, web_ui_action);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = px(820);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = px(760);
        if (app.custom_frame) {
            MONITORINFO monitor = {sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                ((MINMAXINFO *)lp)->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                ((MINMAXINFO *)lp)->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                ((MINMAXINFO *)lp)->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                ((MINMAXINFO *)lp)->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            }
        }
        return 0;
    case WM_DPICHANGED: {
        RECT *suggested = (RECT *)lp;
        app.dpi = HIWORD(wp);
        SetWindowPos(window, NULL, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        web_ui_resize(window);
        web_ui_sync();
        return 0;
    }
    case WM_SIZE:
        if ((wp == SIZE_MINIMIZED) != (app.minimized != 0)) { app.minimized = wp == SIZE_MINIMIZED; apply_background(); }
        web_ui_resize(window);
        web_ui_sync();
        return 0;
    case WM_COMMAND:
        if(LOWORD(wp)==ID_ATTACH && HIWORD(wp)==BN_CLICKED) {
            WCHAR label[MAX_PATH]={0}; uint32_t id=0; unsigned kind=0;
            int result=media_pick_and_send(window,&id,&kind,label,MAX_PATH);
            if(result==1) {
                app.sending_media_id=id;
                append_media_message(window,id,kind,label,0,1);
                SetTimer(window,908,20,NULL);
            } else if(result<0) MessageBoxW(window,
                result==-3?L"Image trop volumineuse. Choisissez un fichier de 2 Mo maximum.":
                result==-4?L"Format ou dimensions non pris en charge. Utilisez JPEG, PNG ou GIF.":
                result==-2?L"Connectez un ami et attendez la fin de l’envoi en cours.":
                L"Impossible d’ouvrir cette image.",L"Murmur · Image",MB_OK|MB_ICONWARNING);
        }
        if (LOWORD(wp) == ID_VOICE && HIWORD(wp) == BN_CLICKED && link_voice_toggle())
            MessageBoxW(window, L"Connectez d’abord un proche avec Mes amis.\n"
                L"Vous devrez ensuite tous les deux rejoindre le vocal.", L"Vocal", MB_OK);
        if (LOWORD(wp) == ID_MUTE && HIWORD(wp) == BN_CLICKED) link_voice_mute_toggle();
        if (LOWORD(wp) == ID_TEST && HIWORD(wp) == BN_CLICKED && local_test_launch(window))
            MessageBoxW(window,L"Impossible d’ouvrir le test local.",L"Murmur · Test solo",MB_OK|MB_ICONWARNING);
        web_ui_sync();
        return 0;
    case WM_TIMER:
        if(wp==908 && app.sending_media_id) {
            int result=media_pump();
            if(result) {
                size_t i;
                KillTimer(window,908);
                for(i=0;i<app.count;++i) if(app.messages[i].media_id==app.sending_media_id) app.messages[i].media_pending=result<0?-1:0;
                app.sending_media_id=0; web_ui_sync();
            }
            return 0;
        }
        if (wp==906 && app.record_mode && app.test_mode==2) {
            screen_record_tick();
            if (GetTickCount64()>=app.record_deadline) { write_record_voice(); screen_record_end(); KillTimer(window,906); }
            return 0;
        }
        if (wp==907 && app.record_mode && app.test_mode==1) {
            write_record_voice(); screen_record_end(); KillTimer(window,907); return 0;
        }
        if (wp==905) { settings_web_tick(); web_ui_sync(); return 0; }
        if (app.test_mode && wp==904) {
            local_test_tick();
            if (app.test_greeted && !link_voice_requested()) KillTimer(window,904);
            if (GetTickCount64()>app.test_deadline && !app.test_greeted && link_state()!=2) {
                app.test_failed=1; local_test_close(); link_stop(); KillTimer(window,904);
            }
            InvalidateRect(window,NULL,FALSE);
            web_ui_sync();
        }
        return 0;
    case WM_APP+31:
        web_ui_sync(); return 0;
    case WM_MURMUR_LEVELS: { unsigned self = 0, peer = 0; link_voice_levels(&self, &peer); web_ui_levels((int)self, (int)peer); return 0; }
    case WM_SERVICE_LINK: {
        service_event event;
        while (service_link_next(&event)) {
            web_ui_service_event(event.slot, event.kind == SERVICE_EVENT_OPEN ? "open" : event.kind == SERVICE_EVENT_CLOSE ? "close" : "message", event.code, event.text);
            free(event.text);
        }
        return 0;
    }
    case WM_DIRECT_SIGNAL: if(!app.test_mode) friends_internet_event(); web_ui_sync(); return 0;
    case WM_APP+34: web_ui_sync(); return 0;
    case WM_DIRECT_UPDATE: web_ui_sync(); return 0;
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
            if(incoming.size>4 && incoming.text[0]==0 &&
               !memcmp(incoming.text+1,"WR1",3)) {
                if(web_ui_active() && link_voice_requested() && incoming.size-4<=MURMUR_CHAT_LIMIT-4) {
                    WCHAR signal[MURMUR_CHAT_LIMIT];
                    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,
                        (const char *)incoming.text+4,(int)incoming.size-4,signal,MURMUR_CHAT_LIMIT-1);
                    if(count>0) { signal[count]=0; web_ui_screen_signal(signal); }
                }
                continue;
            }
            if(incoming.size && incoming.text[0]==0) {
                uint32_t id=0; unsigned kind=0;
                if(media_receive(&incoming,&id,&kind)==1)
                    append_media_message(window,id,kind,kind==3?L"GIF animé":L"Image",1,0);
                continue;
            }
            if (memchr(incoming.text, 0, incoming.size)) continue;
            length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (char *)incoming.text,
                (int)incoming.size, text, MAX_TEXT);
            if (length <= 0) continue;
            text[length] = 0;
            if (app.count == MAX_MESSAGES) {
                memmove(app.messages, app.messages + 1, sizeof(Message) * (MAX_MESSAGES - 1));
                --app.count;
            }
            ZeroMemory(&app.messages[app.count],sizeof(Message)); app.messages[app.count].uid=++app.next_uid;
            wcscpy_s(app.messages[app.count].text, MAX_TEXT + 1, text);
            app.messages[app.count].room = (int)incoming.room;
            app.messages[app.count++].remote = 1;
            GetLocalTime(&app.messages[app.count-1].time);
    wcscpy_s(app.messages[app.count-1].author,64,app.messages[app.count-1].remote?peer_name():self_name());
        }
        SecureZeroMemory(&incoming, sizeof(incoming));
        if (app.test_mode && link_state()==2 && !app.test_greeted) {
            app.test_greeted=1;
            send_text(window,app.test_mode==1 ? L"Bonjour depuis le client A. Ce message traverse la connexion chiffrée locale." : L"Bonjour depuis le client B. Vous pouvez écrire ici et tester l’appel simulé.");
        }
        if(app.record_mode && app.test_mode && link_state()==2 && !app.record_started) {
            app.record_started=1;
            link_voice_toggle();
            if(screen_record_game_begin(app.record_dir,app.test_mode==1)) app.test_failed=1;
            else {
                app.record_deadline=GetTickCount64()+10000;
                SetTimer(window,app.test_mode==1?907:906,app.test_mode==1?10000:100,NULL);
            }
        }
        if (app.test_mode && link_voice_requested()) SetTimer(window,904,500,NULL);
        friends_connection_refresh();
        if (app.minimized) apply_background();
        web_ui_sync();
        return 0;
    }
    case WM_DESTROY:
        web_ui_close();
        media_close();
        settings_close();
        friends_close();
        if (!app.test_mode) updates_shutdown();
        else { KillTimer(window,904); local_test_close(); }
        link_stop();
        screen_shutdown();
        service_link_shutdown();
        identity_store_close();
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
    app.record_mode=!wcscmp(command,L"--test-pair-record") || !wcscmp(command,L"--test-peer-record");
    app.test_mode=(!wcscmp(command,L"--test-pair") || !wcscmp(command,L"--test-pair-record")) ? 1 :
        (!wcscmp(command,L"--test-peer") || !wcscmp(command,L"--test-peer-record")) ? 2 : 0;
    local_test_record_mode(app.record_mode);
    if(app.record_mode) {
        DWORD length=GetEnvironmentVariableW(L"DIRECT_SCREEN_RECORD_DIR",app.record_dir,MAX_PATH);
        if(!length || length>=MAX_PATH) wcscpy_s(app.record_dir,MAX_PATH,L"out\\screen-diagnostic");
    }
    if (updates_apply_command()) return 0;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    app.dpi = GetDpiForSystem();
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"MurmurWindow";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassW(&wc)) return 1;
    window = CreateWindowExW(0, wc.lpszClassName, app.test_mode ? (app.test_mode==1 ? L"Murmur — Client A · test local" : L"Murmur — Client B · test local") : L"Murmur — Messages et appels",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
        px(1220), px(820), NULL, NULL, instance, NULL);
    if (!window) return 1;
    {
        BOOL dark = TRUE;
        COLORREF caption = RGB(32, 32, 32);
        DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
        DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
        { int backdrop = 2, corners = 2;
          DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
          DwmSetWindowAttribute(window, 33, &corners, sizeof(corners)); }
    }
    ShowWindow(window, app.record_mode ? SW_HIDE : show);
    if (app.test_mode) {
        MONITORINFO monitor={sizeof(monitor)}; int width,height,x,y;
        GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        width=(monitor.rcWork.right-monitor.rcWork.left)/2-12; height=monitor.rcWork.bottom-monitor.rcWork.top-24;
        x=monitor.rcWork.left+8+(app.test_mode==2 ? width+8 : 0); y=monitor.rcWork.top+12;
        if (width<px(820)) { width=px(900); x=monitor.rcWork.left+(app.test_mode==2 ? 120 : 24); y+=app.test_mode==2 ? 60 : 0; }
        SetWindowPos(window,NULL,x,y,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        app.test_deadline=GetTickCount64()+25000;
        if (local_test_start(window,app.test_mode==2)) { app.test_failed=1; MessageBoxW(window,L"Impossible de démarrer le test. Lancez Test solo depuis la fenêtre principale.",L"Murmur · Test local",MB_OK|MB_ICONWARNING); }
        else SetTimer(window,904,250,NULL);
    }
    while ((result = GetMessageW(&message, NULL, 0, 0)) > 0) {
        if (!friends_translate(&message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return result == -1 ? 1 : (int)message.wParam;
}
