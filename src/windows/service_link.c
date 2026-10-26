#include "service_link.h"
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define QUEUE_SIZE 64
#define SEND_QUEUE 16
typedef struct {
    int used;
    HANDLE thread, stop, wake;
    WCHAR host[256]; unsigned short port; int secure; char guild[33];
    char *outgoing[SEND_QUEUE]; size_t outgoing_size[SEND_QUEUE]; unsigned out_head, out_count;
} slot_state;
static struct {
    HWND owner; CRITICAL_SECTION lock; int ready;
    slot_state slots[SERVICE_SLOTS];
    service_event events[QUEUE_SIZE]; unsigned head, count;
} link_state;

static int valid_host(const char *host, size_t length) {
    size_t i;
    if (!length || length > 200) return 0;
    for (i = 0; i < length; ++i) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return 0;
    }
    return host[0] != '.' && host[0] != '-';
}
int service_link_parse_origin(const char *origin, int *secure, WCHAR host[256], unsigned short *port) {
    const char *start, *colon;
    size_t length, host_length;
    char name[256];
    unsigned long value = 0;
    if (!origin || !secure || !host || !port || strlen(origin) > 240) return -1;
    if (!strncmp(origin, "https://", 8)) { *secure = 1; start = origin + 8; *port = 443; }
    else if (!strncmp(origin, "http://", 7)) { *secure = 0; start = origin + 7; *port = 80; }
    else return -1;
    length = strlen(start);
    if (length && start[length - 1] == '/') --length;       /* a single trailing slash is tolerated */
    colon = memchr(start, ':', length);
    host_length = colon ? (size_t)(colon - start) : length;
    if (!valid_host(start, host_length)) return -1;
    if (colon) {
        const char *digit;
        if (colon + 1 == start + length) return -1;
        for (digit = colon + 1; digit < start + length; ++digit) {
            if (*digit < '0' || *digit > '9') return -1;
            value = value * 10 + (unsigned long)(*digit - '0');
            if (value > 65535) return -1;
        }
        if (!value) return -1;
        *port = (unsigned short)value;
    }
    memcpy(name, start, host_length); name[host_length] = 0;
    /* Plain HTTP and WebSocket are accepted only for a server on this computer. */
    if (!*secure && strcmp(name, "127.0.0.1") && strcmp(name, "localhost")) return -1;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, host, 256) > 0 ? 0 : -1;
}
static int valid_guild(const char *guild) {
    size_t i;
    if (!guild || strlen(guild) != 32) return 0;
    for (i = 0; i < 32; ++i) if (!((guild[i] >= '0' && guild[i] <= '9') || (guild[i] >= 'a' && guild[i] <= 'f'))) return 0;
    return 1;
}
static void post_event(int slot, int kind, int code, char *text) {
    EnterCriticalSection(&link_state.lock);
    if (link_state.count == QUEUE_SIZE) { LeaveCriticalSection(&link_state.lock); free(text); return; }
    link_state.events[(link_state.head + link_state.count) % QUEUE_SIZE] = (service_event){slot, kind, code, text};
    ++link_state.count;
    LeaveCriticalSection(&link_state.lock);
    PostMessageW(link_state.owner, WM_SERVICE_LINK, 0, 0);
}
typedef struct { int slot; HINTERNET socket; } receiver_arg;
static DWORD WINAPI receive_thread(void *argument) {
    receiver_arg *arg = argument;
    char *buffer = malloc(SERVICE_FRAME_MAX + 1);
    size_t used = 0;
    int code = 1006;
    while (buffer) {
        DWORD received = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE kind;
        if (WinHttpWebSocketReceive(arg->socket, buffer + used, (DWORD)(SERVICE_FRAME_MAX - used), &received, &kind)) break;
        if (kind == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            USHORT status = 0; DWORD length = 0; BYTE reason[128];
            code = !WinHttpWebSocketQueryCloseStatus(arg->socket, &status, reason, sizeof(reason), &length) ? status : 1005;
            break;
        }
        if (kind != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && kind != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) { code = 1003; break; }
        used += received;
        if (kind == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            if (used >= SERVICE_FRAME_MAX) { code = 1009; break; }
            continue;
        }
        buffer[used] = 0;
        { char *copy = _strdup(buffer); if (copy) post_event(arg->slot, SERVICE_EVENT_MESSAGE, 0, copy); }
        used = 0;
    }
    free(buffer);
    SetEvent(link_state.slots[arg->slot].wake);
    return (DWORD)code;
}
static HINTERNET connect_socket(HINTERNET session, const slot_state *slot) {
    WCHAR path[96];
    HINTERNET connection, request, socket = NULL;
    DWORD status = 0, size = sizeof(status), redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    swprintf_s(path, 96, L"/v2/guild/%hs/connect?protocol=2", slot->guild);
    connection = WinHttpConnect(session, slot->host, slot->port, 0);
    if (!connection) return NULL;
    request = WinHttpOpenRequest(connection, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        slot->secure ? WINHTTP_FLAG_SECURE : 0);
    if (request) {
        if (WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)) &&
            WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0) &&
            WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(request, NULL) &&
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &size, NULL) &&
            status == 101)
            socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        WinHttpCloseHandle(request);
    }
    WinHttpCloseHandle(connection);
    return socket;
}
static DWORD WINAPI slot_thread(void *argument) {
    int index = (int)(INT_PTR)argument;
    slot_state *slot = &link_state.slots[index];
    HINTERNET session, socket;
    HANDLE receiver = NULL;
    receiver_arg arg;
    int code = 1006;
    session = WinHttpOpen(L"Murmur/0.7", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { post_event(index, SERVICE_EVENT_CLOSE, 1006, NULL); return 0; }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    socket = connect_socket(session, slot);
    if (socket) {
        arg.slot = index; arg.socket = socket;
        receiver = CreateThread(NULL, 0, receive_thread, &arg, 0, NULL);
        if (receiver) post_event(index, SERVICE_EVENT_OPEN, 0, NULL);
    }
    while (receiver && WaitForSingleObject(slot->stop, 0) == WAIT_TIMEOUT && WaitForSingleObject(receiver, 0) == WAIT_TIMEOUT) {
        char *text = NULL; size_t size = 0; DWORD error = 0;
        EnterCriticalSection(&link_state.lock);
        if (slot->out_count) {
            text = slot->outgoing[slot->out_head]; size = slot->outgoing_size[slot->out_head];
            slot->outgoing[slot->out_head] = NULL; slot->out_head = (slot->out_head + 1) % SEND_QUEUE; --slot->out_count;
        }
        LeaveCriticalSection(&link_state.lock);
        if (text) { error = WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text, (DWORD)size); free(text); if (error) break; }
        else { HANDLE events[2] = {slot->stop, slot->wake}; WaitForMultipleObjects(2, events, FALSE, 10000); }
    }
    if (socket) {
        WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(socket);
    }
    if (receiver) { DWORD value = 1006; WaitForSingleObject(receiver, INFINITE); GetExitCodeThread(receiver, &value); code = (int)value; CloseHandle(receiver); }
    WinHttpCloseHandle(session);
    post_event(index, SERVICE_EVENT_CLOSE, code, NULL);
    return 0;
}
void service_link_init(HWND owner) {
    if (link_state.ready) return;
    link_state.owner = owner;
    InitializeCriticalSection(&link_state.lock);
    link_state.ready = 1;
}
static void release_slot(slot_state *slot) {
    unsigned i;
    for (i = 0; i < SEND_QUEUE; ++i) { free(slot->outgoing[i]); slot->outgoing[i] = NULL; }
    slot->out_head = slot->out_count = 0;
    if (slot->thread) { CloseHandle(slot->thread); slot->thread = NULL; }
    if (slot->stop) { CloseHandle(slot->stop); slot->stop = NULL; }
    if (slot->wake) { CloseHandle(slot->wake); slot->wake = NULL; }
    slot->used = 0;
}
int service_link_open(int slot_index, const char *origin, const char *guild) {
    slot_state *slot;
    int secure; WCHAR host[256]; unsigned short port;
    if (!link_state.ready || slot_index < 0 || slot_index >= SERVICE_SLOTS || !valid_guild(guild) ||
        service_link_parse_origin(origin, &secure, host, &port)) return -1;
    slot = &link_state.slots[slot_index];
    if (slot->used) {
        /* A previous connection on this slot must have ended before it is reused. */
        if (!slot->thread || WaitForSingleObject(slot->thread, 0) == WAIT_TIMEOUT) return -1;
        release_slot(slot);
    }
    memset(slot, 0, sizeof(*slot));
    wcscpy_s(slot->host, 256, host); slot->port = port; slot->secure = secure; strcpy_s(slot->guild, sizeof(slot->guild), guild);
    slot->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    slot->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    slot->used = 1;
    slot->thread = slot->stop && slot->wake ? CreateThread(NULL, 0, slot_thread, (void *)(INT_PTR)slot_index, 0, NULL) : NULL;
    if (!slot->thread) { release_slot(slot); return -1; }
    return 0;
}
int service_link_send(int slot_index, const char *utf8, size_t size) {
    slot_state *slot;
    char *copy;
    if (!link_state.ready || slot_index < 0 || slot_index >= SERVICE_SLOTS || !utf8 || !size || size > SERVICE_FRAME_MAX) return -1;
    copy = malloc(size + 1);
    if (!copy) return -1;
    memcpy(copy, utf8, size); copy[size] = 0;
    slot = &link_state.slots[slot_index];
    EnterCriticalSection(&link_state.lock);
    if (!slot->used || slot->out_count == SEND_QUEUE) { LeaveCriticalSection(&link_state.lock); free(copy); return -1; }
    slot->outgoing[(slot->out_head + slot->out_count) % SEND_QUEUE] = copy;
    slot->outgoing_size[(slot->out_head + slot->out_count) % SEND_QUEUE] = size;
    ++slot->out_count;
    LeaveCriticalSection(&link_state.lock);
    SetEvent(slot->wake);
    return 0;
}
void service_link_close(int slot_index) {
    slot_state *slot;
    if (!link_state.ready || slot_index < 0 || slot_index >= SERVICE_SLOTS) return;
    slot = &link_state.slots[slot_index];
    if (slot->used && slot->stop) { SetEvent(slot->stop); SetEvent(slot->wake); }
}
int service_link_next(service_event *event) {
    if (!link_state.ready) return 0;
    EnterCriticalSection(&link_state.lock);
    if (!link_state.count) { LeaveCriticalSection(&link_state.lock); return 0; }
    *event = link_state.events[link_state.head];
    link_state.head = (link_state.head + 1) % QUEUE_SIZE; --link_state.count;
    LeaveCriticalSection(&link_state.lock);
    return 1;
}
void service_link_shutdown(void) {
    int i;
    unsigned j;
    if (!link_state.ready) return;
    for (i = 0; i < SERVICE_SLOTS; ++i) {
        slot_state *slot = &link_state.slots[i];
        if (!slot->used) continue;
        SetEvent(slot->stop); SetEvent(slot->wake);
        if (slot->thread) WaitForSingleObject(slot->thread, 6000);
        release_slot(slot);
    }
    for (j = 0; j < link_state.count; ++j) free(link_state.events[(link_state.head + j) % QUEUE_SIZE].text);
    link_state.count = 0;
}
