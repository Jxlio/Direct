#include <winsock2.h>
#include <ws2tcpip.h>
#include "friends.h"
#include "datadir.h"
#include "link.h"
#include "signaling.h"
#include "murmur/friend.h"
#include <shlobj.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <string.h>
#include <wchar.h>
static friend_book book;
static WCHAR path[MAX_PATH];
static HWND ticker;
static SOCKET discovery = INVALID_SOCKET;
static int selected = -1, enabled, responder_started, answer_received, winsock_ready;
static int sandbox;
static uint8_t session[32];
static struct sockaddr_in destination;
static ULONGLONG deadline;
static const WCHAR *notice = L"Ajoutez un ami une seule fois. La connexion automatique fonctionne sur le même réseau local.";
static direct_service internet_service;
static int internet_enabled,internet_paused,active_friend=-1,token_sent;
static HWND main_window;
static uint8_t challenges[16][32],active_challenge[32];
static unsigned online[16];
static ULONGLONG retry_at[16],attempt_deadline;
static WCHAR connected_name[64];
static void internet_tick(void);
static void refresh(void);
static void internet_start(void) {
    if(sandbox || !enabled || !direct_service_valid(&internet_service) || !book.count) return;
    internet_enabled=1; signal_start(&internet_service,&book);
    SetTimer(ticker,2,1000,NULL);
}
static int internet_send(unsigned index,unsigned role,const uint8_t challenge[32],const char *token) {
    uint8_t route[32],packet[MURMUR_FRIEND_PACKET]; size_t size=sizeof(packet); int result=-1;
    if(!direct_friend_route(book.items[index].secret,route) && !direct_signal_seal(book.items[index].secret,role,challenge,token,packet,&size)) result=signal_send(route,packet,size);
    SecureZeroMemory(packet,sizeof(packet)); return result;
}
static murmur_ice_config internet_ice(void) {
    murmur_ice_config settings={0};
    strcpy_s(settings.stun_host,sizeof(settings.stun_host),"stun.cloudflare.com"); settings.stun_port=3478;
#ifdef MURMUR_INTERNET_TEST
    settings.stun_host[0]=0; strcpy_s(settings.bind_address,sizeof(settings.bind_address),"127.0.0.1");
#endif
    return settings;
}
static void internet_tick(void) {
    signal_frame frame; DWORD i; int changed=0; ULONGLONG now=GetTickCount64();
    if(!internet_enabled || sandbox) return;
    if(signal_state()==0 && book.count && !internet_paused) signal_start(&internet_service,&book);
    if(signal_state()!=2) {
        memset(online,0,sizeof(online));
        if(!link_busy()) notice=signal_state()==4?L"Connexion automatique suspendue. Gardez une seule instance normale de Murmur ouverte, puis relancez-la.":signal_state()==3?L"Mise en relation indisponible. Nouvelle tentative automatique…":L"Connexion au service de mise en relation…";
    }
    while(signal_receive(&frame)) {
        uint8_t route[32],challenge[32]; char token[6000]; unsigned role;
        for(i=0;i<book.count;++i) { if(!direct_friend_route(book.items[i].secret,route) && !memcmp(route,frame.bytes,32)) break; }
        if(i==book.count) continue;
        if(frame.size==33 && frame.bytes[32]==0) { online[i]=1; retry_at[i]=0; changed=1; }
        else if(frame.size==33 && frame.bytes[32]==2) { online[i]=0; changed=1; }
        else if(frame.size>98 && frame.bytes[32]==1 && !direct_signal_open(book.items[i].secret,frame.bytes+33,frame.size-33,&role,challenge,token) && role==(book.items[i].offerer?1u:0u) && !internet_paused) {
            murmur_ice_config settings=internet_ice();
            if(!strcmp(token,"ready") && role==1 && active_friend<0 && !link_busy() && now>=retry_at[i]) {
                if(!link_ice_start(1,&settings,NULL)) { active_friend=(int)i; token_sent=0; memcpy(active_challenge,challenge,32); attempt_deadline=now+35000; notice=L"Ami trouvé. Connexion directe automatique…"; changed=1; }
            } else if(role==0 && active_friend<0 && !link_busy() && !memcmp(challenges[i],challenge,32) && retry_at[i]>now && strcmp(token,"ready")) {
                if(!link_ice_start(0,&settings,token)) { active_friend=(int)i; token_sent=0; memcpy(active_challenge,challenge,32); SecureZeroMemory(challenges[i],32); attempt_deadline=now+35000; notice=L"Ami trouvé. Authentification de la connexion…"; changed=1; }
            } else if(role==1 && active_friend==(int)i && !memcmp(active_challenge,challenge,32) && strcmp(token,"ready")) link_ice_answer(token);
        }
        SecureZeroMemory(token,sizeof(token)); SecureZeroMemory(challenge,sizeof(challenge));
    }
    SecureZeroMemory(&frame,sizeof(frame));
    if(active_friend>=0) {
        if(link_state()==2) {
            if(wcscmp(connected_name,book.items[active_friend].name)) { wcscpy_s(connected_name,64,book.items[active_friend].name); PostMessageW(main_window,WM_MURMUR_LINK,0,0); }
            notice=L"Votre ami est connecté. Messages et appels sont disponibles.";
        } else if(link_state()==3 || (!link_busy() && link_state()!=1) || now>attempt_deadline) {
            if(link_state()!=2) link_stop();
            retry_at[active_friend]=now+60000; active_friend=-1; token_sent=0;
            notice=L"Connexion directe interrompue ou bloquée par le réseau. Nouvelle tentative automatique dans une minute.";
            changed=1;
        }
        if(active_friend>=0 && !token_sent && link_state()!=2) {
            char token[6000];
            if(link_ice_token(token,sizeof(token))==1 && !internet_send((unsigned)active_friend,book.items[active_friend].offerer?0u:1u,active_challenge,token)) token_sent=1;
            SecureZeroMemory(token,sizeof(token));
        }
    }
    if(!internet_paused && active_friend<0 && !link_busy() && signal_state()==2) {
        for(i=0;i<book.count;++i) if(online[i] && !book.items[i].offerer && now>=retry_at[i]) {
            if(!murmur_random_key(challenges[i])) { internet_send(i,1,challenges[i],"ready"); retry_at[i]=now+30000; }
        }
        if(!book.count) notice=L"Ajoutez un ami pour activer la connexion automatique.";
    }
    if(changed) refresh();
}
void friends_internet_event(void) { internet_tick(); refresh(); }
int friends_auto_connecting(void) { return internet_enabled && active_friend>=0 && link_state()==1; }
int friends_peer_id(uint8_t route[32]) { return active_friend>=0 && (DWORD)active_friend<book.count && link_state()==2 && !direct_friend_route(book.items[active_friend].secret,route); }
const WCHAR *friends_peer_name(void) { return link_state()==2 && active_friend>=0 && (DWORD)active_friend<book.count?book.items[active_friend].name:link_state()==2 && connected_name[0]?connected_name:NULL; }
static void refresh(void) {
    if (main_window) PostMessageW(main_window, WM_APP + 34, 0, 0);
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
    if (link_state() == 2) { if(selected>=0) wcscpy_s(connected_name,64,book.items[selected].name); notice = L"Votre ami est connecté. Vous pouvez discuter et rejoindre le vocal."; stop_discovery(); refresh(); return; }
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
static int add_friend_impl(int create, const WCHAR *name, const WCHAR *invitation, WCHAR *generated) {
    saved_friend item = {0}; WCHAR wide[DIRECT_INVITE_MAX]; char code[DIRECT_INVITE_MAX]; DWORD i; direct_service received_service={0}; int result=-1;
    if (!enabled || book.count >= 16 || discovery != INVALID_SOCKET || link_busy() || active_friend>=0) {
        notice = L"Ajout indisponible : arrêtez la connexion en cours, ou vérifiez le stockage (16 amis maximum)."; refresh(); return -1;
    }
    if (!name || wcslen(name)>=64) { notice=L"Nom trop long."; refresh(); return -1; }
    wcscpy_s(item.name,64,name);
    if (!item.name[0]) { notice = L"Choisissez d’abord un nom pour cet ami."; refresh(); return -1; }
    if (create) {
        if (murmur_random_key(item.secret)) { notice=L"Impossible de créer une clé pour cet ami."; goto done; }
        if(direct_invite_create(item.secret,&internet_service,code)) goto done; item.offerer = 1;
    } else {
        if (!invitation || wcslen(invitation)>=DIRECT_INVITE_MAX) { notice=L"Code ami invalide."; goto done; }
        wcscpy_s(wide,DIRECT_INVITE_MAX,invitation);
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, code, sizeof(code), NULL, NULL) ||
            direct_invite_parse(code, item.secret,&received_service)) {
            notice = L"Code ami invalide. Collez le code complet reçu en privé."; goto done;
        }
    }
    if(received_service.origin[0]) {
        if(internet_service.origin[0] && (strcmp(internet_service.origin,received_service.origin) || strcmp(internet_service.access,received_service.access))) { notice=L"Ce code appartient à un autre groupe de mise en relation. Le groupe actuel a été conservé."; goto done; }
        if(signal_profile_save(&received_service)) { notice=L"Impossible de conserver la configuration de connexion."; goto done; }
        internet_service=received_service;
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
        MultiByteToWideChar(CP_UTF8, 0, code, -1, wide, DIRECT_INVITE_MAX);
        if (generated) wcscpy_s(generated,DIRECT_INVITE_MAX,wide);
        notice = L"Ami enregistré. Envoyez ce code une seule fois, en privé, à cette personne uniquement.";
    } else notice = L"Ami enregistré. Vous pouvez maintenant vous retrouver sans échanger de nouveaux codes.";
    result=0;
done:
    internet_start(); SecureZeroMemory(&received_service,sizeof(received_service));
    SecureZeroMemory(&item, sizeof(item)); SecureZeroMemory(code, sizeof(code)); SecureZeroMemory(wide, sizeof(wide)); refresh();
    return result;
}
void friends_init(HWND parent) {
    WNDCLASSW wc = {0}; WSADATA data;
    wc.hInstance = GetModuleHandleW(NULL);
    if (SUCCEEDED(murmur_local_appdata(path)) && wcslen(path)+24 < MAX_PATH) {
        wcscat_s(path, MAX_PATH, L"\\Murmur");
        if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
            wcscat_s(path, MAX_PATH, L"\\friends.dat"); enabled = !friend_book_load(path, &book);
        }
    }
    if (!enabled) notice = L"Stockage des amis inaccessible ou endommagé. Le fichier existant a été conservé.";
    if (!WSAStartup(MAKEWORD(2, 2), &data)) {
        winsock_ready = 1;
                wc.lpszClassName = L"MurmurFriendTicker"; wc.lpfnWndProc = DefWindowProcW; RegisterClassW(&wc);
        ticker = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    }
    main_window=parent; signal_init(parent);
    if(signal_profile_load(&internet_service)>0) { internet_enabled=1; notice=L"Service Internet prêt. Créez un code ami et partagez-le une seule fois en privé."; internet_start(); }
}
void friends_init_test(HWND parent,const WCHAR *peer) {
    main_window=parent;
    sandbox=1; enabled=0; book.version=book.count=1;
    wcscpy_s(book.items[0].name,64,peer);
    notice=L"Test local : contact temporaire, sans modification de votre liste d’amis.";
}
void friends_connection_refresh(void) { if(!sandbox && link_state()!=2) connected_name[0]=0; if (sandbox) refresh(); }
/* Timer messages are dispatched by the main loop, even when the panel is closed. */
int friends_translate(MSG *message) {
    if (message->hwnd == ticker && message->message == WM_TIMER) { if(message->wParam==2) internet_tick(); else tick(); return 1; }
    return 0;
}
void friends_close(void) {
    if(!sandbox && main_window) signal_stop();
    stop_discovery(); if (ticker) { KillTimer(ticker, 1); DestroyWindow(ticker); }
    ticker = NULL;
    if (winsock_ready) { WSACleanup(); winsock_ready = 0; }
    SecureZeroMemory(&book, sizeof(book));
}
unsigned friends_web_count(void) { return book.count; }
const WCHAR *friends_web_name(unsigned index) { return index<book.count?book.items[index].name:L""; }
int friends_web_online(unsigned index) { return index<book.count && (sandbox ? link_state()==2 : online[index]!=0); }
const WCHAR *friends_web_notice(void) { return notice; }
int friends_web_internet_enabled(void) { return internet_enabled; }
int friends_web_create(const WCHAR *name,WCHAR code[512]) { return add_friend_impl(1,name,NULL,code); }
int friends_web_add(const WCHAR *name,const WCHAR *code) { return add_friend_impl(0,name,code,NULL); }
int friends_web_code(unsigned index,WCHAR code[512]) {
    char utf8[DIRECT_INVITE_MAX]; int result=-1;
    if(index<book.count && book.items[index].offerer && !direct_invite_create(book.items[index].secret,&internet_service,utf8) &&
        MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,utf8,-1,code,DIRECT_INVITE_MAX)) result=0;
    SecureZeroMemory(utf8,sizeof(utf8)); return result;
}
void friends_web_connect(unsigned index) {
    if (sandbox || index>=book.count) return;
    if (internet_enabled) { internet_paused=0; memset(retry_at,0,sizeof(retry_at)); internet_tick(); notice=L"Reconnexion automatique activée."; refresh(); }
    else connect_friend((int)index);
}
void friends_web_disconnect(void) {
    internet_paused=1; active_friend=-1; stop_discovery(); link_stop(); notice=L"Connexion arrêtée."; refresh();
}
int friends_web_forget(unsigned index) {
    friend_book updated;
    if (!enabled || index>=book.count || link_busy() || active_friend>=0 || discovery!=INVALID_SOCKET) return -1;
    updated=book;
    memmove(&updated.items[index],&updated.items[index+1],(updated.count-index-1)*sizeof(saved_friend));
    SecureZeroMemory(&updated.items[--updated.count],sizeof(saved_friend));
    if(friend_book_save(path,&updated)) { notice=L"Impossible d’enregistrer la suppression."; SecureZeroMemory(&updated,sizeof(updated)); refresh(); return -1; }
    book=updated;
    if(internet_enabled) { if(book.count) signal_subscribe(&book); else signal_stop(); }
    notice=L"Ami oublié. Son ancien code ne permet plus de vous retrouver sur ce PC.";
    SecureZeroMemory(&updated,sizeof(updated)); refresh(); return 0;
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

#ifdef MURMUR_INTERNET_TEST
int friends_test_online(int offerer) {
    WNDCLASSW wc={0};
    wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(NULL); wc.lpszClassName=L"DirectOnlineTestTicker";
    RegisterClassW(&wc); ticker=CreateWindowW(wc.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,NULL,wc.hInstance,NULL);
    book.version=book.count=1; book.items[0].offerer=(DWORD)offerer;
    memset(book.items[0].secret,0x71,32); wcscpy_s(book.items[0].name,64,L"Synthetic peer");
    enabled=1; signal_init(NULL);
    if(signal_profile_load(&internet_service)!=1) return -1;
    internet_start(); return internet_enabled?0:-1;
}
#endif
